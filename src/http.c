/*
 * http.c - the HTTP/1.1 engine: parse a request with picohttpparser, read its body, run the
 * middleware chain and the endpoint against a context, then send what they wrote. All of it runs
 * on the connection's coroutine, so await_recv and await_send simply suspend it and the loop
 * resumes it - a body that outgrows the buffer streams to the wire from inside the handler.
 */
#define _GNU_SOURCE
#include "internal.h"
#include "picohttpparser.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef IOMA_REQ_CAP
#define IOMA_REQ_CAP      16384     /* request line + headers + body must fit here; else 413/431 */
#endif
#ifndef IOMA_PARAM_CAP
#define IOMA_PARAM_CAP    2048      /* per-request arena for percent-decoded query parameters    */
#endif
#ifndef IOMA_HEAD_CAP
#define IOMA_HEAD_CAP     4096      /* a serialized head must fit here                           */
#endif
#ifndef IOMA_OUT_CAP
#define IOMA_OUT_CAP      8192      /* body bytes buffered before the reply streams              */
#endif
#define IOMA_HEAD_RESERVE 512       /* room in front of the body buffer for the head or a chunk size */

/* serve() hands req.headers to picohttpparser as its header array: a kv (two slices) must lay
 * out exactly like a phr_header (name, name_len, value, value_len). */
_Static_assert(sizeof(ioma_kv) == sizeof(struct phr_header), "ioma_kv must mirror phr_header");
_Static_assert(offsetof(ioma_kv, key)    == offsetof(struct phr_header, name) &&
               offsetof(ioma_slice, len) == offsetof(struct phr_header, name_len) &&
               offsetof(ioma_kv, value)  == offsetof(struct phr_header, value),
               "ioma_kv must mirror phr_header");

/* ── request headers ───────────────────────────────────────────────────────────────────── */

/* Parse a decimal size; stops at the first non-digit. */
static size_t parse_size(const char *s, size_t n)
{
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (size_t)(s[i] - '0');
    }
    return v;
}

/* Is `tok` one of the comma-separated tokens in the header value [s, s+n)? Case-insensitive. */
static bool token_present_ci(const char *s, size_t n, const char *tok)
{
    size_t tl = strlen(tok);
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) i++;
        size_t j = i;
        while (j < n && s[j] != ',') j++;
        size_t e = j;
        while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
        if (eq_ci(s + i, e - i, tok, tl)) return true;
        i = j + 1;
    }
    return false;
}

/* The three headers the engine itself needs; p == NULL when absent. */
struct hdrs {
    ioma_slice content_length, transfer_enc, connection;
};

/* Lower-case ASCII in place, eight bytes per step. The bytes must all be below 0x80 - true for
 * header names, which picohttpparser only accepts as HTTP tokens - so the adds cannot carry
 * between bytes: +0x3f sets a byte's high bit from 'A' up, +0x25 from 'Z'+1 up, and the
 * difference marks exactly 'A'..'Z'. */
static inline void lower_inplace(char *s, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        uint64_t upper = ((w + 0x3f3f3f3f3f3f3f3fULL) & ~(w + 0x2525252525252525ULL)) & 0x8080808080808080ULL;
        w |= upper >> 2;                                    /* 0x80 >> 2 == 0x20 */
        memcpy(s + i, &w, 8);
    }
    for (; i < n; i++)
        if ((unsigned)(s[i] - 'A') < 26u) s[i] |= 0x20;
}

/* One pass over the request headers: lower-case each name in place (the buffer is ours), so
 * handlers and this switch compare with plain memcmp. The switch on the name length rejects
 * nearly every header before a byte is compared. */
static struct hdrs pick_headers(ioma_request *req)
{
    struct hdrs h = { { NULL, 0 }, { NULL, 0 }, { NULL, 0 } };
    for (size_t i = 0; i < req->n_headers; i++) {
        ioma_kv *x = &req->headers[i];
        char *k = (char *)x->key.p;
        lower_inplace(k, x->key.len);
        switch (x->key.len) {
        case 14:
            if (memcmp(k, "content-length", 14) == 0)    h.content_length = x->value;
            break;
        case 17:
            if (memcmp(k, "transfer-encoding", 17) == 0) h.transfer_enc = x->value;
            break;
        case 10:
            if (memcmp(k, "connection", 10) == 0)        h.connection = x->value;
            break;
        default:
            break;
        }
    }
    return h;
}

/* HTTP/1.1 keeps alive unless "close"; HTTP/1.0 only with "keep-alive". */
static bool keep_alive_from(int minor_version, ioma_slice cv)
{
    bool ka = minor_version >= 1;
    if (cv.p) {
        if      (token_present_ci(cv.p, cv.len, "close"))      ka = false;
        else if (token_present_ci(cv.p, cv.len, "keep-alive")) ka = true;
    }
    return ka;
}

/* ── the reply: head serialization and the body sink ───────────────────────────────────── */

/* Write v in decimal at dst; return the digit count. A digit loop, no printf. */
static inline int put_uint(char *dst, size_t v)
{
    char tmp[20];
    int  i = 0;
    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int j = 0; j < i; j++)
        dst[j] = tmp[i - 1 - j];
    return i;
}

/* The same in hex, for chunk sizes. */
static inline int put_hex(char *dst, size_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    int  i = 0;
    do {
        tmp[i++] = digits[v & 15];
        v >>= 4;
    } while (v);
    for (int j = 0; j < i; j++)
        dst[j] = tmp[i - 1 - j];
    return i;
}

/* A constant slice: pointer + length, so a precomposed line is one memcpy. */
struct cslice { const char *p; int len; };
#define CSLICE(lit) (struct cslice){ (lit), (int)(sizeof(lit) - 1) }

/* The precomposed status line for the common codes; NULL for the rest (built on the spot). */
static struct cslice status_line(int code)
{
    switch (code) {
    case 200: return CSLICE("HTTP/1.1 200 OK\r\n");
    case 204: return CSLICE("HTTP/1.1 204 No Content\r\n");
    case 400: return CSLICE("HTTP/1.1 400 Bad Request\r\n");
    case 404: return CSLICE("HTTP/1.1 404 Not Found\r\n");
    case 405: return CSLICE("HTTP/1.1 405 Method Not Allowed\r\n");
    case 500: return CSLICE("HTTP/1.1 500 Internal Server Error\r\n");
    default:  return (struct cslice){ NULL, 0 };
    }
}

/* A bodyless framework reply (parse errors, limits). Best effort; the caller then closes. */
static void send_status(conn_t *c, int code)
{
    char  head[128];
    char *p = head;

    struct cslice sl = status_line(code);
    if (sl.p) {
        memcpy(p, sl.p, (size_t)sl.len);
        p += sl.len;
    } else {
        memcpy(p, "HTTP/1.1 ", 9);
        p += 9;
        p += put_uint(p, (size_t)code);
        *p++ = ' ';
        const char *r = ioma_reason(code);
        size_t rl = strlen(r);
        memcpy(p, r, rl);
        p += rl;
        memcpy(p, "\r\n", 2);
        p += 2;
    }
    memcpy(p, "Content-Length: 0\r\nConnection: close\r\n\r\n", 40);
    p += 40;

    await_send(c, head, (size_t)(p - head));
}

/* How the body is delimited on the wire. */
enum framing { FRAME_LENGTH, FRAME_CHUNKED, FRAME_UNTIL_CLOSE };

/* Serialize the head into dst by memcpy of precomposed pieces plus the integer writer - no
 * snprintf. Returns the length, or -1 if it does not fit. */
static int build_head(const ioma_ctx *c, char *dst, size_t cap, enum framing f, size_t length)
{
    char *p   = dst;
    char *end = dst + cap;

#define NEED(n)     do { if ((size_t)(end - p) < (size_t)(n)) return -1; } while (0)
#define PUT(src, n) do { NEED(n); memcpy(p, (src), (size_t)(n)); p += (n); } while (0)
#define PUTC(lit)   PUT((lit), sizeof(lit) - 1)

    struct cslice sl = status_line(c->status);
    if (sl.p) {
        PUT(sl.p, sl.len);
    } else {
        PUTC("HTTP/1.1 ");
        NEED(3);
        p += put_uint(p, (size_t)c->status);
        PUTC(" ");
        const char *reason = ioma_reason(c->status);
        PUT(reason, strlen(reason));
        PUTC("\r\n");
    }

    PUTC("Content-Type: ");
    PUT(c->content_type.p, c->content_type.len);
    PUTC("\r\n");

    if (f == FRAME_LENGTH) {
        PUTC("Content-Length: ");
        NEED(20);
        p += put_uint(p, length);
        PUTC("\r\n");
    } else if (f == FRAME_CHUNKED) {
        PUTC("Transfer-Encoding: chunked\r\n");
    }

    /* Connection: only when it says something. HTTP/1.1 is persistent by default, so a kept-alive
     * 1.1 reply carries none; a 1.0 client that asked for keep-alive is told it got it; a closing
     * reply always says close. */
    bool ka = c->req.keep_alive && !c->close;
    if (!ka)
        PUTC("Connection: close\r\n");
    else if (c->req.minor_version == 0)
        PUTC("Connection: keep-alive\r\n");

    for (size_t i = 0; i < c->n_headers; i++) {
        PUT(c->headers[i].key.p, c->headers[i].key.len);
        PUTC(": ");
        PUT(c->headers[i].value.p, c->headers[i].value.len);
        PUTC("\r\n");
    }

    PUTC("\r\n");
#undef PUTC
#undef PUT
#undef NEED
    return (int)(p - dst);
}

/* Wrap the buffered body as one chunk: the size line goes just before it (into the reserve), the
 * CRLF just after it (into the slack). start/total describe the framed span. */
static void frame_chunk(char *body, size_t n, char **start, size_t *total)
{
    char sz[16];
    int  l = put_hex(sz, n);
    char *s = body - (l + 2);
    memcpy(s, sz, (size_t)l);
    s[l]     = '\r';
    s[l + 1] = '\n';
    body[n]     = '\r';
    body[n + 1] = '\n';
    *start = s;
    *total = n + (size_t)l + 4;
}

/* Send the buffered body, with the head in front of it the first time. That first time decides
 * the framing: a final flush with the head unsent means the whole body is here (Content-Length,
 * one send); an early flush means the body outgrew the buffer, so it streams - with the declared
 * length if the handler gave one, else chunked on HTTP/1.1, else until close on HTTP/1.0. */
static int flush(ioma_ctx *c, bool final)
{
    if (c->failed)
        return -1;
    char  *body  = c->out;
    size_t n     = c->out_len;
    char  *start = body;
    size_t total = n;

    if (!c->head_sent) {
        enum framing f      = FRAME_LENGTH;
        size_t       length = n;
        if (c->has_length) {
            length = c->content_length;
        } else if (!final) {
            if (c->req.minor_version >= 1) {
                f = FRAME_CHUNKED;
                c->chunked = true;
            } else {
                f = FRAME_UNTIL_CLOSE;
                c->close = true;
            }
        }
        c->head_sent = true;

        char head[IOMA_HEAD_CAP];
        int  hl = build_head(c, head, sizeof head, f, length);
        if (hl < 0) {
            c->failed = true;
            return -1;
        }
        if (c->chunked && n)
            frame_chunk(body, n, &start, &total);
        if ((size_t)hl <= (size_t)(start - c->buf)) {          /* the head fits in the reserve */
            start -= hl;
            memcpy(start, head, (size_t)hl);
            total += (size_t)hl;
        } else if (await_send(c->conn, head, (size_t)hl) < 0) {
            c->failed = true;
            return -1;
        }
    } else if (c->chunked && n) {
        frame_chunk(body, n, &start, &total);
    }

    c->out_len = 0;
    if (total && await_send(c->conn, start, total) < 0) {
        c->failed = true;
        return -1;
    }
    return 0;
}

/* After the chain: send what is left - the whole reply if nothing went out yet - and close a
 * chunked stream. */
static int finish(ioma_ctx *c)
{
    if (c->failed)
        return -1;
    if (!c->head_sent || c->out_len)
        if (flush(c, true) < 0)
            return -1;
    if (c->chunked && await_send(c->conn, "0\r\n\r\n", 5) < 0)
        return -1;
    return 0;
}

/* Buffer body bytes; send the buffer, head first, whenever it fills. */
int ioma_write(ioma_ctx *c, const void *data, size_t len)
{
    if (c->failed)
        return -1;
    const char *p = data;
    while (len) {
        size_t room = c->out_cap - c->out_len;
        if (room == 0) {
            if (flush(c, false) < 0)
                return -1;
            continue;
        }
        size_t n = len < room ? len : room;
        memcpy(c->out + c->out_len, p, n);
        c->out_len += n;
        p   += n;
        len -= n;
    }
    return 0;
}

/* Format straight into the buffer. If it does not fit the room left, flush and format again;
 * something bigger than the whole buffer is formatted on the heap and written in pieces. */
int ioma_printf(ioma_ctx *c, const char *fmt, ...)
{
    if (c->failed)
        return -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t  room = c->out_cap - c->out_len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(c->out + c->out_len, room, fmt, ap);
        va_end(ap);
        if (n < 0)
            return -1;
        if ((size_t)n < room) {
            c->out_len += (size_t)n;
            return 0;
        }
        if ((size_t)n >= c->out_cap) {                        /* larger than the buffer itself */
            char *tmp = malloc((size_t)n + 1);
            if (!tmp)
                return -1;
            va_start(ap, fmt);
            vsnprintf(tmp, (size_t)n + 1, fmt, ap);
            va_end(ap);
            int rc = ioma_write(c, tmp, (size_t)n);
            free(tmp);
            return rc;
        }
        if (flush(c, false) < 0)
            return -1;
    }
    return -1;
}

/* Send what is buffered now. Starts streaming: the head goes out with it. */
int ioma_flush(ioma_ctx *c)
{
    return flush(c, false);
}

/* ── request bodies (suspend on the recv) ──────────────────────────────────────────────── */

/* Decode a chunked body in place right after the headers, reading more as needed. phr keeps
 * state across calls, so a split anywhere - mid chunk-size hex included - works. Returns the
 * decoded length, or -1 once the connection is finished (413/400 sent, or the peer went away). */
static long read_chunked_body(conn_t *c, char *buf, size_t header_len, size_t have)
{
    struct phr_chunked_decoder dec;
    memset(&dec, 0, sizeof dec);
    dec.consume_trailer = 1;

    size_t  decoded = have - header_len;                 /* raw bytes already here to decode */
    ssize_t pret    = phr_decode_chunked(&dec, buf + header_len, &decoded);
    while (pret == -2) {                                 /* needs more: append after the decoded prefix */
        size_t off = header_len + decoded;
        if (off == IOMA_REQ_CAP) {
            send_status(c, 413);
            return -1;
        }
        int n = await_recv(c, buf + off, IOMA_REQ_CAP - off);
        if (n <= 0) return -1;
        size_t rsize = (size_t)n;
        pret = phr_decode_chunked(&dec, buf + off, &rsize);
        decoded += rsize;
    }
    if (pret < 0) {                                      /* malformed */
        send_status(c, 400);
        return -1;
    }
    return (long)decoded;
}

/* Read until header_len + Content-Length bytes are buffered; *have is updated. Returns 0, or -1
 * once the connection is finished (413 sent, or the peer went away). */
static int read_fixed_body(conn_t *c, char *buf, size_t total, size_t *have)
{
    if (total > IOMA_REQ_CAP) {
        send_status(c, 413);
        return -1;
    }
    while (*have < total) {
        int n = await_recv(c, buf + *have, IOMA_REQ_CAP - *have);
        if (n <= 0) return -1;
        *have += (size_t)n;
    }
    return 0;
}

/* ── the connection loop ───────────────────────────────────────────────────────────────── */

/* The proactor handler for every connection: parse, read the body, run the chain against a
 * context, send what it wrote; repeat while kept alive. Returning closes the connection. */
void ioma__serve(conn_t *c)
{
    char   buf[IOMA_REQ_CAP];
    char   params[IOMA_PARAM_CAP];                    /* decoded query parameters land here   */
    char   out[IOMA_HEAD_RESERVE + IOMA_OUT_CAP + 2]; /* head reserve, body buffer, CRLF slack */
    size_t have = 0, last_len = 0;

    for (;;) {
        ioma_ctx      ctx;
        ioma_request *rq = &ctx.req;
        size_t nphr = IOMA_MAX_HEADERS;
        const char *method = NULL, *target = NULL;
        size_t ml = 0, tl = 0;
        int minor = 0;

        /* Parse straight into ctx.req.headers. With nothing buffered - the usual state right
         * after a reply - skip the parse and just read. */
        int pret = have ? phr_parse_request(buf, have, &method, &ml, &target, &tl, &minor,
                                            (struct phr_header *)rq->headers, &nphr, last_len)
                        : -2;

        if (pret == -2) {                                /* headers not complete yet */
            if (have == IOMA_REQ_CAP) {
                send_status(c, 431);
                return;
            }
            last_len = have;
            int n = await_recv(c, buf + have, IOMA_REQ_CAP - have);
            if (n <= 0) return;                          /* peer closed or error */
            have += (size_t)n;
            continue;
        }
        if (pret < 0) {                                  /* malformed */
            send_status(c, 400);
            return;
        }
        size_t header_len = (size_t)pret;

        rq->method        = (ioma_slice){ method, ml };
        rq->target        = (ioma_slice){ target, tl };
        rq->minor_version = minor;
        rq->n_headers     = nphr;

        const char *q = memchr(target, '?', tl);
        if (q) {
            rq->path  = (ioma_slice){ target, (size_t)(q - target) };
            rq->query = (ioma_slice){ q + 1, tl - rq->path.len - 1 };
        } else {
            rq->path  = (ioma_slice){ target, tl };
            rq->query = (ioma_slice){ target + tl, 0 };
        }
        rq->n_params = rq->query.len
            ? ioma_kv_parse(rq->query.p, rq->query.len, rq->params, IOMA_MAX_PARAMS, params, sizeof params)
            : 0;
        rq->n_route = 0;                                 /* the router fills these */

        struct hdrs h = pick_headers(rq);

        /* Body. leftover_* is what belongs to the next request on a kept-alive connection. */
        size_t leftover_off = 0, leftover_len = 0;
        if (h.transfer_enc.p && token_present_ci(h.transfer_enc.p, h.transfer_enc.len, "chunked")) {
            long n = read_chunked_body(c, buf, header_len, have);
            if (n < 0) return;
            rq->body = (ioma_slice){ buf + header_len, (size_t)n };
            /* bytes pipelined after a chunked body are not carried; the next request reads fresh */
        } else {
            size_t content_length = h.content_length.p ? parse_size(h.content_length.p, h.content_length.len) : 0;
            size_t total = header_len + content_length;
            if (read_fixed_body(c, buf, total, &have) < 0) return;
            rq->body = (ioma_slice){ buf + header_len, content_length };
            leftover_off = total;
            leftover_len = have - total;
        }
        rq->keep_alive = keep_alive_from(minor, h.connection);

        /* the reply side of the context: defaults, and an empty sink over the out buffer */
        ctx.status         = 200;
        ctx.content_type   = (ioma_slice){ "text/plain", 10 };
        ctx.n_headers      = 0;
        ctx.close          = false;
        ctx.head_sent      = false;
        ctx.user           = NULL;
        ctx.conn           = c;
        ctx.buf            = out;
        ctx.out            = out + IOMA_HEAD_RESERVE;
        ctx.out_cap        = IOMA_OUT_CAP;
        ctx.out_len        = 0;
        ctx.content_length = 0;
        ctx.has_length     = false;
        ctx.chunked        = false;
        ctx.failed         = false;

        ioma__dispatch(&ctx);                            /* middleware chain + endpoint */
        if (finish(&ctx) < 0) return;                   /* sends; suspends meanwhile   */

        if (!rq->keep_alive || ctx.close) return;

        if (leftover_len) memmove(buf, buf + leftover_off, leftover_len);   /* pipelining */
        have = leftover_len;
        last_len = 0;
    }
}

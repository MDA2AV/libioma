/*
 * engine.c - the HTTP/1.1 engine: parse a request head with picohttpparser, run the middleware
 * chain and the endpoint against a context, read the body on demand (whole or streamed) and
 * drain what was left, then send what was written. All of it runs on the connection's coroutine,
 * so await_recv and await_send simply suspend it and the loop resumes it.
 */
#define _GNU_SOURCE
#include "http/internal.h"
#include "picohttpparser.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef IOMA_REQ_CAP
#define IOMA_REQ_CAP      16384     /* the head, and a body read whole, must fit here         */
#endif
#ifndef IOMA_PARAM_CAP
#define IOMA_PARAM_CAP    2048      /* per-request arena for percent-decoded query parameters */
#endif
#ifndef IOMA_HEAD_CAP
#define IOMA_HEAD_CAP     4096      /* a serialized reply head must fit here                  */
#endif
#ifndef IOMA_OUT_CAP
#define IOMA_OUT_CAP      8192      /* the write slab: body bytes buffered before a reply streams */
#endif
#ifndef IOMA_DRAIN_MAX
#define IOMA_DRAIN_MAX    (1024 * 1024)   /* unread body discarded after a handler before we close instead */
#endif
#define IOMA_LEAD         512       /* room in front of the slab for the reply head or a chunk size */

/* serve() hands req.headers to picohttpparser as its header array: a kv (two slices) must lay
 * out exactly like a phr_header (name, name_len, value, value_len). */
_Static_assert(sizeof(ioma_kv) == sizeof(struct phr_header), "ioma_kv must mirror phr_header");
_Static_assert(offsetof(ioma_kv, key)    == offsetof(struct phr_header, name) &&
               offsetof(ioma_slice, len) == offsetof(struct phr_header, name_len) &&
               offsetof(ioma_kv, value)  == offsetof(struct phr_header, value),
               "ioma_kv must mirror phr_header");

/* The engine's per-request state, behind ctx->priv. */
struct serve_state {
    conn_t *conn;
    char   *rbuf;                       /* the read buffer: the head, then body bytes         */
    size_t  rcap;
    size_t  have;                       /* bytes of this request received into rbuf so far   */
    size_t  header_len;                 /* where the body starts in rbuf                      */
    size_t  body_got;                   /* body bytes handed out so far                       */
    bool    body_done;                  /* the whole body has been taken off the wire         */
    bool    body_whole;                 /* ioma_body read it into rbuf                        */
    int     body_err;                   /* 0, a status to answer (400, 413), or -1: peer gone */
    size_t  leftover_off, leftover_len; /* the next pipelined request's bytes, in rbuf        */
    /* streaming a chunked body: raw bytes are staged after the head and decoded in place */
    struct phr_chunked_decoder dec;
    size_t  raw_len;                    /* undecoded bytes at the stage                       */
    size_t  dec_pos, dec_len;           /* decoded bytes at the stage, not yet handed out     */
    size_t  tail_len;                   /* bytes past the terminator, parked at the end of rbuf */
};
#define ST(c) ((struct serve_state *)(c)->priv)

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

/* ── the reply: head serialization and the write slab ──────────────────────────────────── */

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
    const ioma_response *r = &c->res;
    char *p   = dst;
    char *end = dst + cap;

#define NEED(n)     do { if ((size_t)(end - p) < (size_t)(n)) return -1; } while (0)
#define PUT(src, n) do { NEED(n); memcpy(p, (src), (size_t)(n)); p += (n); } while (0)
#define PUTC(lit)   PUT((lit), sizeof(lit) - 1)

    struct cslice sl = status_line(r->status);
    if (sl.p) {
        PUT(sl.p, sl.len);
    } else {
        PUTC("HTTP/1.1 ");
        NEED(3);
        p += put_uint(p, (size_t)r->status);
        PUTC(" ");
        const char *reason = ioma_reason(r->status);
        PUT(reason, strlen(reason));
        PUTC("\r\n");
    }

    PUTC("Content-Type: ");
    PUT(r->content_type.p, r->content_type.len);
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
    bool ka = c->req.keep_alive && !r->close;
    if (!ka)
        PUTC("Connection: close\r\n");
    else if (c->req.minor_version == 0)
        PUTC("Connection: keep-alive\r\n");

    for (size_t i = 0; i < r->n_headers; i++) {
        PUT(r->headers[i].key.p, r->headers[i].key.len);
        PUTC(": ");
        PUT(r->headers[i].value.p, r->headers[i].value.len);
        PUTC("\r\n");
    }

    PUTC("\r\n");
#undef PUTC
#undef PUT
#undef NEED
    return (int)(p - dst);
}

/* Wrap the slab's bytes as one chunk: the size line goes just before them (into the lead), the
 * CRLF just after (into the slack). start/total describe the framed span. */
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

/* Send the slab, with the head in front of it the first time. That first time decides the
 * framing: a final flush with the head unsent means the whole body is here (Content-Length, one
 * send); an early flush means the body outgrew the slab, so it streams - with the declared length
 * if the handler gave one, else chunked on HTTP/1.1, else until close on HTTP/1.0. */
static int flush(ioma_ctx *c, bool final)
{
    ioma_response *r = &c->res;
    if (r->failed)
        return -1;
    char  *body  = r->buf;
    size_t n     = r->len;
    char  *start = body;
    size_t total = n;

    if (!r->head_sent) {
        enum framing f      = FRAME_LENGTH;
        size_t       length = n;
        if (r->has_length) {
            length = r->content_length;
        } else if (!final) {
            if (c->req.minor_version >= 1) {
                f = FRAME_CHUNKED;
                r->chunked = true;
            } else {
                f = FRAME_UNTIL_CLOSE;
                r->close = true;
            }
        }
        r->head_sent = true;

        char head[IOMA_HEAD_CAP];
        int  hl = build_head(c, head, sizeof head, f, length);
        if (hl < 0) {
            r->failed = true;
            return -1;
        }
        if (r->chunked && n)
            frame_chunk(body, n, &start, &total);
        if ((size_t)hl <= (size_t)(start - (r->buf - IOMA_LEAD))) {   /* the head fits in the lead */
            start -= hl;
            memcpy(start, head, (size_t)hl);
            total += (size_t)hl;
        } else if (await_send(ST(c)->conn, head, (size_t)hl) < 0) {
            r->failed = true;
            return -1;
        }
    } else if (r->chunked && n) {
        frame_chunk(body, n, &start, &total);
    }

    r->len = 0;
    if (total && await_send(ST(c)->conn, start, total) < 0) {
        r->failed = true;
        return -1;
    }
    return 0;
}

/* After the chain: send what is left - the whole reply if nothing went out yet - and close a
 * chunked stream. */
static int finish(ioma_ctx *c)
{
    ioma_response *r = &c->res;
    if (r->failed)
        return -1;
    if (!r->head_sent || r->len)
        if (flush(c, true) < 0)
            return -1;
    if (r->chunked && await_send(ST(c)->conn, "0\r\n\r\n", 5) < 0)
        return -1;
    return 0;
}

/* Append body bytes to the slab; send it, head first, whenever it fills. */
int ioma_write(ioma_ctx *c, const void *data, size_t len)
{
    ioma_response *r = &c->res;
    if (r->failed)
        return -1;
    const char *p = data;
    while (len) {
        size_t room = r->cap - r->len;
        if (room == 0) {
            if (flush(c, false) < 0)
                return -1;
            continue;
        }
        size_t n = len < room ? len : room;
        memcpy(r->buf + r->len, p, n);
        r->len += n;
        p   += n;
        len -= n;
    }
    return 0;
}

/* Format straight into the slab. If it does not fit the room left, flush and format again;
 * something bigger than the whole slab is formatted on the heap and written in pieces. */
int ioma_printf(ioma_ctx *c, const char *fmt, ...)
{
    ioma_response *r = &c->res;
    if (r->failed)
        return -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t  room = r->cap - r->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(r->buf + r->len, room, fmt, ap);
        va_end(ap);
        if (n < 0)
            return -1;
        if ((size_t)n < room) {
            r->len += (size_t)n;
            return 0;
        }
        if ((size_t)n >= r->cap) {                            /* larger than the slab itself */
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

/* Send what is in the slab now. Starts streaming: the head goes out with it. */
int ioma_flush(ioma_ctx *c)
{
    return flush(c, false);
}

/* ── the body: read on demand ──────────────────────────────────────────────────────────── */

/* The whole body, into the request buffer right after the head. Content-Length: read until it is
 * all there. Chunked: decode in place, reading more as needed - phr keeps state across calls, so a
 * split anywhere works - and remember what follows the terminator (a pipelined next request). */
ioma_slice ioma_body(ioma_ctx *c)
{
    struct serve_state *st = ST(c);
    ioma_request *rq = &c->req;
    const ioma_slice none = { rq->body.p, 0 };

    if (st->body_whole)
        return rq->body;
    if (st->body_got || st->body_err)                    /* already streaming, or failed */
        return none;

    char *body = st->rbuf + st->header_len;
    if (rq->chunked) {
        struct phr_chunked_decoder dec;
        memset(&dec, 0, sizeof dec);
        dec.consume_trailer = 1;
        size_t  decoded = st->have - st->header_len;       /* raw bytes already here */
        ssize_t pret    = phr_decode_chunked(&dec, body, &decoded);
        while (pret == -2) {                               /* needs more: append after the decoded prefix */
            size_t off = st->header_len + decoded;
            if (off == st->rcap) {
                st->body_err = 413;
                return none;
            }
            int n = await_recv(st->conn, st->rbuf + off, st->rcap - off);
            if (n <= 0) {
                st->body_err = -1;
                return none;
            }
            size_t rsize = (size_t)n;
            pret = phr_decode_chunked(&dec, st->rbuf + off, &rsize);
            decoded += rsize;
        }
        if (pret < 0) {
            st->body_err = 400;
            return none;
        }
        rq->body         = (ioma_slice){ body, decoded };
        st->leftover_off = st->header_len + decoded;       /* the bytes past the terminator */
        st->leftover_len = (size_t)pret;
    } else {
        size_t total = st->header_len + rq->content_length;
        if (total > st->rcap) {
            st->body_err = 413;
            return none;
        }
        while (st->have < total) {
            int n = await_recv(st->conn, st->rbuf + st->have, st->rcap - st->have);
            if (n <= 0) {
                st->body_err = -1;
                return none;
            }
            st->have += (size_t)n;
        }
        rq->body         = (ioma_slice){ body, rq->content_length };
        st->leftover_off = total;
        st->leftover_len = st->have - total;
    }
    st->body_got   = rq->body.len;
    st->body_done  = true;
    st->body_whole = true;
    return rq->body;
}

/* Stream a Content-Length body: first the bytes that arrived with the head, then straight from
 * the wire into dst, never past the declared length (so a pipelined request is never consumed). */
static int body_read_fixed(ioma_ctx *c, struct serve_state *st, char *dst, size_t cap)
{
    size_t remaining = c->req.content_length - st->body_got;
    if (remaining == 0) {
        st->body_done = true;
        return 0;
    }
    size_t buffered = st->have - st->header_len;
    if (st->body_got < buffered) {
        size_t n = buffered - st->body_got;
        if (n > remaining) n = remaining;
        if (n > cap)       n = cap;
        memcpy(dst, st->rbuf + st->header_len + st->body_got, n);
        st->body_got += n;
        if (st->body_got == c->req.content_length)
            st->body_done = true;
        return (int)n;
    }
    size_t n = remaining < cap ? remaining : cap;
    int r = await_recv(st->conn, dst, n);
    if (r <= 0) {
        st->body_err = -1;
        return -1;
    }
    st->body_got += (size_t)r;
    if (st->body_got == c->req.content_length)
        st->body_done = true;
    return r;
}

/* Stream a chunked body. Raw bytes are staged after the head and decoded in place; decoded bytes
 * are handed out, then the stage is reused. Bytes past the terminator are parked at the end of the
 * buffer for the next request. */
static int body_read_chunked(struct serve_state *st, char *dst, size_t cap)
{
    char  *stage = st->rbuf + st->header_len;
    size_t scap  = st->rcap - st->header_len;
    for (;;) {
        if (st->dec_len) {
            size_t n = st->dec_len < cap ? st->dec_len : cap;
            memcpy(dst, stage + st->dec_pos, n);
            st->dec_pos  += n;
            st->dec_len  -= n;
            st->body_got += n;
            return (int)n;
        }
        if (st->body_done)
            return 0;
        if (st->raw_len == 0) {
            int r = await_recv(st->conn, stage, scap - 2);   /* keep the parked tail's room */
            if (r <= 0) {
                st->body_err = -1;
                return -1;
            }
            st->raw_len = (size_t)r;
        }
        size_t  sz  = st->raw_len;
        ssize_t ret = phr_decode_chunked(&st->dec, stage, &sz);
        st->raw_len = 0;
        st->dec_pos = 0;
        st->dec_len = sz;
        if (ret == -1) {
            st->body_err = 400;
            return -1;
        }
        if (ret >= 0) {
            st->body_done = true;
            if (ret > 0) {                                   /* the next request's bytes */
                st->tail_len = (size_t)ret;
                memmove(st->rbuf + st->rcap - st->tail_len, stage + sz, st->tail_len);
            }
        }
    }
}

/* The next bytes of the body into dst. */
int ioma_body_read(ioma_ctx *c, void *dst, size_t cap)
{
    struct serve_state *st = ST(c);
    if (st->body_err || cap == 0)
        return -1;
    if (st->body_whole) {                                    /* hand out what ioma_body read */
        size_t rem = c->req.body.len - st->body_got;
        if (rem == 0)
            return 0;
        size_t n = rem < cap ? rem : cap;
        memcpy(dst, c->req.body.p + st->body_got, n);
        st->body_got += n;
        return (int)n;
    }
    if (st->body_done)
        return 0;
    return c->req.chunked ? body_read_chunked(st, dst, cap) : body_read_fixed(c, st, dst, cap);
}

/* After the chain: take an unread body off the wire so the connection stays in sync, up to a
 * limit - past it, the reply says close and the rest is never read. */
static void drain_body(ioma_ctx *c)
{
    struct serve_state *st = ST(c);
    char   tmp[4096];
    size_t drained = 0;
    while (!st->body_done && !st->body_err) {
        if (drained >= IOMA_DRAIN_MAX) {
            c->res.close = true;
            return;
        }
        int n = ioma_body_read(c, tmp, sizeof tmp);
        if (n <= 0)
            return;
        drained += (size_t)n;
    }
}

/* ── the connection loop ───────────────────────────────────────────────────────────────── */

/* The proactor handler for every connection: parse the head, run the chain against a context,
 * drain what it left of the body, send what it wrote; repeat while kept alive. Returning closes
 * the connection. */
void ioma__serve(conn_t *conn)
{
    char   rbuf[IOMA_REQ_CAP];
    char   params[IOMA_PARAM_CAP];                  /* decoded query parameters land here   */
    char   slab[IOMA_LEAD + IOMA_OUT_CAP + 2];      /* lead, the write slab, CRLF slack     */
    size_t have = 0, last_len = 0;

    for (;;) {
        ioma_ctx           ctx;
        ioma_request      *rq = &ctx.req;
        ioma_response     *rs = &ctx.res;
        struct serve_state st;
        size_t nphr = IOMA_MAX_HEADERS;
        const char *method = NULL, *target = NULL;
        size_t ml = 0, tl = 0;
        int minor = 0;

        /* Parse straight into ctx.req.headers. With nothing buffered - the usual state right
         * after a reply - skip the parse and just read. */
        int pret = have ? phr_parse_request(rbuf, have, &method, &ml, &target, &tl, &minor,
                                            (struct phr_header *)rq->headers, &nphr, last_len)
                        : -2;

        if (pret == -2) {                                /* head not complete yet */
            if (have == IOMA_REQ_CAP) {
                send_status(conn, 431);
                return;
            }
            last_len = have;
            int n = await_recv(conn, rbuf + have, IOMA_REQ_CAP - have);
            if (n <= 0) return;                          /* peer closed or error */
            have += (size_t)n;
            continue;
        }
        if (pret < 0) {                                  /* malformed */
            send_status(conn, 400);
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
        rq->n_route_params = 0;                                 /* the router fills these */

        struct hdrs h = pick_headers(rq);
        rq->chunked        = h.transfer_enc.p && token_present_ci(h.transfer_enc.p, h.transfer_enc.len, "chunked");
        rq->content_length = h.content_length.p ? parse_size(h.content_length.p, h.content_length.len) : 0;
        rq->body           = (ioma_slice){ rbuf + header_len, 0 };
        rq->keep_alive     = keep_alive_from(minor, h.connection);

        /* The body stays on the wire until asked for. What already arrived with the head is in
         * rbuf; a Content-Length body that is entirely there also tells where the next request
         * starts. */
        memset(&st, 0, sizeof st);
        st.conn       = conn;
        st.rbuf       = rbuf;
        st.rcap       = IOMA_REQ_CAP;
        st.have       = have;
        st.header_len = header_len;
        if (rq->chunked) {
            st.dec.consume_trailer = 1;
            st.raw_len = have - header_len;
        } else {
            size_t total = header_len + rq->content_length;
            st.body_done = rq->content_length == 0;
            if (have >= total) {
                st.leftover_off = total;
                st.leftover_len = have - total;
            }
        }

        /* The response: defaults, and an empty slab. */
        memset(rs, 0, offsetof(ioma_response, buf));
        rs->status       = 200;
        rs->content_type = (ioma_slice){ "text/plain", 10 };
        rs->buf          = slab + IOMA_LEAD;
        rs->cap          = IOMA_OUT_CAP;
        rs->len          = 0;
        rs->chunked      = false;
        rs->failed       = false;
        ctx.user = NULL;
        ctx.priv = &st;

        ioma__dispatch(&ctx);                            /* middleware chain + endpoint */

        if (st.body_err) {                               /* too large, malformed, or gone */
            if (st.body_err > 0 && !rs->head_sent)
                send_status(conn, st.body_err);
            return;
        }
        drain_body(&ctx);                                /* what the handler left unread */
        if (st.body_err) return;
        if (finish(&ctx) < 0) return;                    /* sends; suspends meanwhile   */

        if (!rq->keep_alive || rs->close) return;

        /* pipelining: carry the bytes that belong to the next request */
        if (st.tail_len) {                               /* parked past a chunked terminator */
            st.leftover_off = IOMA_REQ_CAP - st.tail_len;
            st.leftover_len = st.tail_len;
        }
        if (st.leftover_len) memmove(rbuf, rbuf + st.leftover_off, st.leftover_len);
        have = st.leftover_len;
        last_len = 0;
    }
}

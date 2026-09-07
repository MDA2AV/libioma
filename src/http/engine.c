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
    char   *read_buf;                       /* the read buffer: the head, then body bytes         */
    size_t  read_cap;
    size_t  filled;                       /* bytes of this request received into read_buf so far   */
    size_t  head_len;                 /* where the body starts in read_buf                      */
    size_t  body_read;                   /* body bytes handed out so far                       */
    bool    body_done;                  /* the whole body has been taken off the wire         */
    bool    body_whole;                 /* ioma_body read it into read_buf                        */
    int     body_err;                   /* 0, a status to answer (400, 413), or -1: peer gone */
    size_t  next_req_off, next_req_len; /* the next pipelined request's bytes, in read_buf        */
    /* streaming a chunked body: raw bytes are staged after the head and decoded in place */
    struct phr_chunked_decoder decoder;
    size_t  staged_raw;                    /* undecoded bytes at the stage                       */
    size_t  decoded_pos, decoded_len;           /* decoded bytes at the stage, not yet handed out     */
    size_t  parked_len;                   /* bytes past the terminator, parked at the end of read_buf */
};
#define STATE(c) ((struct serve_state *)(c)->priv)

/* ── request headers ───────────────────────────────────────────────────────────────────── */

/* Fold A-Z to a-z; every other byte unchanged. */
static inline unsigned char lower_ascii(unsigned char a)
{
    return (unsigned)(a - 'A') < 26u ? (unsigned char)(a | 0x20) : a;
}

/* Case-insensitive equality of two slices: a length test, then a byte loop. No libc, no locale. */
static bool eq_ci(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn)
        return false;
    for (size_t i = 0; i < an; i++)
        if (lower_ascii((unsigned char)a[i]) != lower_ascii((unsigned char)b[i]))
            return false;
    return true;
}

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
    size_t tok_len = strlen(tok);
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) i++;
        size_t j = i;
        while (j < n && s[j] != ',') j++;
        size_t e = j;
        while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
        if (eq_ci(s + i, e - i, tok, tok_len)) return true;
        i = j + 1;
    }
    return false;
}

/* The three headers the engine itself needs; p == NULL when absent. */
struct picked_headers {
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
static struct picked_headers pick_headers(ioma_request *req)
{
    struct picked_headers picked = { { NULL, 0 }, { NULL, 0 }, { NULL, 0 } };
    for (size_t i = 0; i < req->n_headers; i++) {
        ioma_kv *hdr  = &req->headers[i];
        char    *name = (char *)hdr->key.p;
        lower_inplace(name, hdr->key.len);
        switch (hdr->key.len) {
        case 14:
            if (memcmp(name, "content-length", 14) == 0) picked.content_length = hdr->value;
            break;
        case 17:
            if (memcmp(name, "transfer-encoding", 17) == 0) picked.transfer_enc = hdr->value;
            break;
        case 10:
            if (memcmp(name, "connection", 10) == 0) picked.connection = hdr->value;
            break;
        default:
            break;
        }
    }
    return picked;
}

/* HTTP/1.1 keeps alive unless "close"; HTTP/1.0 only with "keep-alive". */
static bool keep_alive_from(int minor_version, ioma_slice connection)
{
    bool keep = minor_version >= 1;
    if (connection.p) {
        if      (token_present_ci(connection.p, connection.len, "close"))      keep = false;
        else if (token_present_ci(connection.p, connection.len, "keep-alive")) keep = true;
    }
    return keep;
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
static void send_status(conn_t *conn, int code)
{
    char  head[128];
    char *p = head;

    struct cslice line = status_line(code);
    if (line.p) {
        memcpy(p, line.p, (size_t)line.len);
        p += line.len;
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

    await_send(conn, head, (size_t)(p - head));
}

/* How the body is delimited on the wire. */
enum framing { FRAME_LENGTH, FRAME_CHUNKED, FRAME_UNTIL_CLOSE };

/* Serialize the head into dst by memcpy of precomposed pieces plus the integer writer - no
 * snprintf. Returns the length, or -1 if it does not fit. */
static int build_head(const ioma_ctx *c, char *dst, size_t cap, enum framing framing, size_t length)
{
    const ioma_response *res = &c->res;
    char *p   = dst;
    char *end = dst + cap;

#define NEED(n)     do { if ((size_t)(end - p) < (size_t)(n)) return -1; } while (0)
#define PUT(src, n) do { NEED(n); memcpy(p, (src), (size_t)(n)); p += (n); } while (0)
#define PUTC(lit)   PUT((lit), sizeof(lit) - 1)

    struct cslice line = status_line(res->status);
    if (line.p) {
        PUT(line.p, line.len);
    } else {
        PUTC("HTTP/1.1 ");
        NEED(3);
        p += put_uint(p, (size_t)res->status);
        PUTC(" ");
        const char *reason = ioma_reason(res->status);
        PUT(reason, strlen(reason));
        PUTC("\r\n");
    }

    PUTC("Content-Type: ");
    PUT(res->content_type.p, res->content_type.len);
    PUTC("\r\n");

    if (framing == FRAME_LENGTH) {
        PUTC("Content-Length: ");
        NEED(20);
        p += put_uint(p, body_len);
        PUTC("\r\n");
    } else if (framing == FRAME_CHUNKED) {
        PUTC("Transfer-Encoding: chunked\r\n");
    }

    /* Connection: only when it says something. HTTP/1.1 is persistent by default, so a kept-alive
     * 1.1 reply carries none; a 1.0 client that asked for keep-alive is told it got it; a closing
     * reply always says close. */
    bool ka = c->req.keep_alive && !res->close;
    if (!ka)
        PUTC("Connection: close\r\n");
    else if (c->req.minor_version == 0)
        PUTC("Connection: keep-alive\r\n");

    for (size_t i = 0; i < res->n_headers; i++) {
        PUT(res->headers[i].key.p, res->headers[i].key.len);
        PUTC(": ");
        PUT(res->headers[i].value.p, res->headers[i].value.len);
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
static void frame_chunk(char *body, size_t len, char **start, size_t *total)
{
    char size_line[16];
    int  digits = put_hex(size_line, len);
    char *at = body - (digits + 2);
    memcpy(at, size_line, (size_t)digits);
    at[digits]     = '\r';
    at[digits + 1] = '\n';
    body[len]     = '\r';
    body[len + 1] = '\n';
    *start = at;
    *total = len + (size_t)digits + 4;
}

/* Send the slab, with the head in front of it the first time. That first time decides the
 * framing: a final flush with the head unsent means the whole body is here (Content-Length, one
 * send); an early flush means the body outgrew the slab, so it streams - with the declared length
 * if the handler gave one, else chunked on HTTP/1.1, else until close on HTTP/1.0. */
static int flush(ioma_ctx *c, bool final)
{
    ioma_response *res = &c->res;
    if (res->failed)
        return -1;
    char  *body  = res->buf;
    size_t len   = res->len;
    char  *start = body;
    size_t total = len;

    if (!res->head_sent) {
        enum framing framing      = FRAME_LENGTH;
        size_t       length = n;
        if (res->has_length) {
            length = res->content_length;
        } else if (!final) {
            if (c->req.minor_version >= 1) {
                framing = FRAME_CHUNKED;
                res->chunked = true;
            } else {
                framing = FRAME_UNTIL_CLOSE;
                res->close = true;
            }
        }
        res->head_sent = true;

        char head[IOMA_HEAD_CAP];
        int  head_len = build_head(c, head, sizeof head, framing, body_len);
        if (head_len < 0) {
            res->failed = true;
            return -1;
        }
        if (res->chunked && len)
            frame_chunk(body, len, &start, &total);
        if ((size_t)head_len <= (size_t)(start - (res->buf - IOMA_LEAD))) {   /* the head fits in the lead */
            start -= head_len;
            memcpy(start, head, (size_t)head_len);
            total += (size_t)head_len;
        } else if (await_send(STATE(c)->conn, head, (size_t)head_len) < 0) {
            res->failed = true;
            return -1;
        }
    } else if (res->chunked && len) {
        frame_chunk(body, len, &start, &total);
    }

    res->len = 0;
    if (total && await_send(STATE(c)->conn, start, total) < 0) {
        res->failed = true;
        return -1;
    }
    return 0;
}

/* After the chain: send what is left - the whole reply if nothing went out yet - and close a
 * chunked stream. */
static int finish(ioma_ctx *c)
{
    ioma_response *res = &c->res;
    if (res->failed)
        return -1;
    if (!res->head_sent || res->len)
        if (flush(c, true) < 0)
            return -1;
    if (res->chunked && await_send(STATE(c)->conn, "0\r\n\r\n", 5) < 0)
        return -1;
    return 0;
}

/* Append body bytes to the slab; send it, head first, whenever it fills. */
int ioma_write(ioma_ctx *c, const void *data, size_t len)
{
    ioma_response *res = &c->res;
    if (res->failed)
        return -1;
    const char *src = data;
    while (len) {
        size_t room = res->cap - res->len;
        if (room == 0) {
            if (flush(c, false) < 0)
                return -1;
            continue;
        }
        size_t n = len < room ? len : room;
        memcpy(res->buf + res->len, src, n);
        res->len += n;
        src += n;
        len -= n;
    }
    return 0;
}

/* Format straight into the slab. If it does not fit the room left, flush and format again;
 * something bigger than the whole slab is formatted on the heap and written in pieces. */
int ioma_printf(ioma_ctx *c, const char *fmt, ...)
{
    ioma_response *res = &c->res;
    if (res->failed)
        return -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t  room = res->cap - res->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(res->buf + res->len, room, fmt, ap);
        va_end(ap);
        if (n < 0)
            return -1;
        if ((size_t)n < room) {
            res->len += (size_t)n;
            return 0;
        }
        if ((size_t)n >= res->cap) {                            /* larger than the slab itself */
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
    struct serve_state *state = STATE(c);
    ioma_request *req = &c->req;
    const ioma_slice none = { req->body.p, 0 };

    if (state->body_whole)
        return req->body;
    if (state->body_read || state->body_err)                    /* already streaming, or failed */
        return none;

    char *body = state->read_buf + state->head_len;
    if (req->chunked) {
        struct phr_chunked_decoder decoder;
        memset(&decoder, 0, sizeof decoder);
        decoder.consume_trailer = 1;
        size_t  decoded = state->filled - state->head_len;       /* raw bytes already here */
        ssize_t rc      = phr_decode_chunked(&decoder, body, &decoded);
        while (rc == -2) {                               /* needs more: append after the decoded prefix */
            size_t off = state->head_len + decoded;
            if (off == state->read_cap) {
                state->body_err = 413;
                return none;
            }
            int n = await_recv(state->conn, state->read_buf + off, state->read_cap - off);
            if (n <= 0) {
                state->body_err = -1;
                return none;
            }
            size_t chunk = (size_t)n;
            rc = phr_decode_chunked(&decoder, state->read_buf + off, &chunk);
            decoded += chunk;
        }
        if (rc < 0) {
            state->body_err = 400;
            return none;
        }
        req->body         = (ioma_slice){ body, decoded };
        state->next_req_off = state->head_len + decoded;       /* the bytes past the terminator */
        state->next_req_len = (size_t)rc;
    } else {
        size_t total = state->head_len + req->content_length;
        if (total > state->read_cap) {
            state->body_err = 413;
            return none;
        }
        while (state->filled < total) {
            int n = await_recv(state->conn, state->read_buf + state->filled, state->read_cap - state->filled);
            if (n <= 0) {
                state->body_err = -1;
                return none;
            }
            state->filled += (size_t)n;
        }
        req->body         = (ioma_slice){ body, req->content_length };
        state->next_req_off = total;
        state->next_req_len = state->filled - total;
    }
    state->body_read   = req->body.len;
    state->body_done  = true;
    state->body_whole = true;
    return req->body;
}

/* Stream a Content-Length body: first the bytes that arrived with the head, then straight from
 * the wire into dst, never past the declared length (so a pipelined request is never consumed). */
static int body_read_fixed(ioma_ctx *c, struct serve_state *state, char *dst, size_t cap)
{
    size_t remaining = c->req.content_length - state->body_read;
    if (remaining == 0) {
        state->body_done = true;
        return 0;
    }
    size_t buffered = state->filled - state->head_len;
    if (state->body_read < buffered) {
        size_t n = buffered - state->body_read;
        if (n > remaining) n = remaining;
        if (n > cap)       n = cap;
        memcpy(dst, state->read_buf + state->head_len + state->body_read, n);
        state->body_read += n;
        if (state->body_read == c->req.content_length)
            state->body_done = true;
        return (int)n;
    }
    size_t n = remaining < cap ? remaining : cap;
    int r = await_recv(state->conn, dst, n);
    if (r <= 0) {
        state->body_err = -1;
        return -1;
    }
    state->body_read += (size_t)r;
    if (state->body_read == c->req.content_length)
        state->body_done = true;
    return r;
}

/* Stream a chunked body. Raw bytes are staged after the head and decoded in place; decoded bytes
 * are handed out, then the stage is reused. Bytes past the terminator are parked at the end of the
 * buffer for the next request. */
static int body_read_chunked(struct serve_state *state, char *dst, size_t cap)
{
    char  *stage = state->read_buf + state->head_len;
    size_t stage_cap = state->read_cap - state->head_len;
    for (;;) {
        if (state->decoded_len) {
            size_t n = state->decoded_len < cap ? state->decoded_len : cap;
            memcpy(dst, stage + state->decoded_pos, n);
            state->decoded_pos  += n;
            state->decoded_len  -= n;
            state->body_read += n;
            return (int)n;
        }
        if (state->body_done)
            return 0;
        if (state->staged_raw == 0) {
            int r = await_recv(state->conn, stage, stage_cap - 2);   /* keep the parked tail's room */
            if (r <= 0) {
                state->body_err = -1;
                return -1;
            }
            state->staged_raw = (size_t)r;
        }
        size_t  decoded = state->staged_raw;
        ssize_t rc      = phr_decode_chunked(&state->decoder, stage, &decoded);
        state->staged_raw  = 0;
        state->decoded_pos = 0;
        state->decoded_len = decoded;
        if (rc == -1) {
            state->body_err = 400;
            return -1;
        }
        if (rc >= 0) {
            state->body_done = true;
            if (rc > 0) {                                   /* the next request's bytes */
                state->parked_len = (size_t)rc;
                memmove(state->read_buf + state->read_cap - state->parked_len, stage + decoded, state->parked_len);
            }
        }
    }
}

/* The next bytes of the body into dst. */
int ioma_body_read(ioma_ctx *c, void *dst, size_t cap)
{
    struct serve_state *state = STATE(c);
    if (state->body_err || cap == 0)
        return -1;
    if (state->body_whole) {                                    /* hand out what ioma_body read */
        size_t rem = c->req.body.len - state->body_read;
        if (rem == 0)
            return 0;
        size_t n = rem < cap ? rem : cap;
        memcpy(dst, c->req.body.p + state->body_read, n);
        state->body_read += n;
        return (int)n;
    }
    if (state->body_done)
        return 0;
    return c->req.chunked ? body_read_chunked(state, dst, cap) : body_read_fixed(c, state, dst, cap);
}

/* After the chain: take an unread body off the wire so the connection stays in sync, up to a
 * limit - past it, the reply says close and the rest is never read. */
static void drain_body(ioma_ctx *c)
{
    struct serve_state *state = STATE(c);
    char   tmp[4096];
    size_t drained = 0;
    while (!state->body_done && !state->body_err) {
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
    char   read_buf[IOMA_REQ_CAP];                      /* receive buffer                       */
    char   params[IOMA_PARAM_CAP];                  /* decoded query parameters land here   */
    char   slab[IOMA_LEAD + IOMA_OUT_CAP + 2];      /* lead, the write slab, CRLF slack     */
    size_t filled = 0, already_parsed = 0;             /* bytes in read_buf; bytes the last parse saw */

    for (;;) {
        ioma_ctx           ctx;
        ioma_request      *req = &ctx.req;
        ioma_response     *res = &ctx.res;
        struct serve_state state;
        size_t header_count = IOMA_MAX_HEADERS;
        const char *method = NULL, *target = NULL;
        size_t method_len = 0, target_len = 0;
        int minor_version = 0;

        /* Parse straight into ctx.req.headers. With nothing buffered - the usual state right
         * after a reply - skip the parse and just read. */
        int parsed = filled ? phr_parse_request(read_buf, filled, &method, &method_len, &target, &target_len, &minor_version,
                                            (struct phr_header *)req->headers, &header_count, already_parsed)
                        : -2;

        if (parsed == -2) {                                /* head not complete yet */
            if (filled == IOMA_REQ_CAP) {
                send_status(conn, 431);
                return;
            }
            already_parsed = filled;
            int n = await_recv(conn, read_buf + filled, IOMA_REQ_CAP - filled);
            if (n <= 0) return;                          /* peer closed or error */
            filled += (size_t)n;
            continue;
        }
        if (parsed < 0) {                                  /* malformed */
            send_status(conn, 400);
            return;
        }
        size_t head_len = (size_t)parsed;

        req->method        = (ioma_slice){ method, method_len };
        req->target        = (ioma_slice){ target, target_len };
        req->minor_version = minor_version;
        req->n_headers     = header_count;

        const char *qmark = memchr(target, '?', target_len);
        if (qmark) {
            req->path  = (ioma_slice){ target, (size_t)(qmark - target) };
            req->query = (ioma_slice){ qmark + 1, target_len - req->path.len - 1 };
        } else {
            req->path  = (ioma_slice){ target, target_len };
            req->query = (ioma_slice){ target + target_len, 0 };
        }
        req->n_params = req->query.len
            ? ioma_kv_parse(req->query.p, req->query.len, req->params, IOMA_MAX_PARAMS, params, sizeof params)
            : 0;
        req->n_route_params = 0;                                 /* the router fills these */

        struct picked_headers picked = pick_headers(req);
        req->chunked        = picked.transfer_enc.p && token_present_ci(picked.transfer_enc.p, picked.transfer_enc.len, "chunked");
        req->content_length = picked.content_length.p ? parse_size(picked.content_length.p, picked.content_length.len) : 0;
        req->body           = (ioma_slice){ read_buf + head_len, 0 };
        req->keep_alive     = keep_alive_from(minor_version, picked.connection);

        /* The body stays on the wire until asked for. What already arrived with the head is in
         * read_buf; a Content-Length body that is entirely there also tells where the next request
         * starts. */
        memset(&state, 0, sizeof state);
        state.conn       = conn;
        state.read_buf       = read_buf;
        state.read_cap       = IOMA_REQ_CAP;
        state.filled       = filled;
        state.head_len = head_len;
        if (req->chunked) {
            state.decoder.consume_trailer = 1;
            state.staged_raw = filled - head_len;
        } else {
            size_t total = head_len + req->content_length;
            state.body_done = req->content_length == 0;
            if (filled >= total) {
                state.next_req_off = total;
                state.next_req_len = filled - total;
            }
        }

        /* The response: defaults, and an empty slab. */
        memset(res, 0, offsetof(ioma_response, buf));
        res->status       = 200;
        res->content_type = (ioma_slice){ "text/plain", 10 };
        res->buf          = slab + IOMA_LEAD;
        res->cap          = IOMA_OUT_CAP;
        res->len          = 0;
        res->chunked      = false;
        res->failed       = false;
        ctx.user = NULL;
        ctx.priv = &state;

        ioma__dispatch(&ctx);                            /* middleware chain + endpoint */

        if (state.body_err) {                               /* too large, malformed, or gone */
            if (state.body_err > 0 && !res->head_sent)
                send_status(conn, state.body_err);
            return;
        }
        drain_body(&ctx);                                /* what the handler left unread */
        if (state.body_err) return;
        if (finish(&ctx) < 0) return;                    /* sends; suspends meanwhile   */

        if (!req->keep_alive || res->close) return;

        /* pipelining: carry the bytes that belong to the next request */
        if (state.parked_len) {                               /* parked past a chunked terminator */
            state.next_req_off = IOMA_REQ_CAP - state.parked_len;
            state.next_req_len = state.parked_len;
        }
        if (state.next_req_len) memmove(read_buf, read_buf + state.next_req_off, state.next_req_len);
        filled = state.next_req_len;
        already_parsed = 0;
    }
}

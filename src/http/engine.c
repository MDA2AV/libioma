/*
 * engine.c - the HTTP/1.1 engine: parse a request head with picohttpparser, run the middleware
 * chain and the endpoint against a context, read the body on demand (whole or streamed) and
 * drain what was left, then send what was written. All of it runs on the connection's coroutine,
 * so await_recv and await_send simply suspend it and the loop resumes it.
 */
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
#define IOMA_DRAIN_MAX    (1024UL * 1024)   /* unread body discarded after a handler before we close instead */
#endif
#define IOMA_LEAD         512       /* room in front of the slab for the reply head or a chunk size */

/* serve() hands req.headers to picohttpparser as its header array: a kv (two slices) must lay
 * out exactly like a phr_header (name, name_len, value, value_len). */
static_assert(sizeof(ioma_kv) == sizeof(struct phr_header), "ioma_kv must mirror phr_header");
static_assert(offsetof(ioma_kv, key)    == offsetof(struct phr_header, name) &&
               offsetof(ioma_slice, len) == offsetof(struct phr_header, name_len) &&
               offsetof(ioma_kv, value)  == offsetof(struct phr_header, value),
               "ioma_kv must mirror phr_header");

/* The engine's per-request state, behind ctx->priv. */
struct serve_state {
    conn_t *conn;
    char   *read_buf;                       /* the read buffer (IOMA_REQ_CAP): head, then body     */
    size_t  filled;                         /* bytes received into it: the head and, for a         */
                                            /* Content-Length body, its bytes as they arrive       */
    size_t  head_len;                       /* where the body starts                              */
    size_t  body_read;                      /* body bytes handed out so far                       */
    bool    body_done;                      /* the whole body has been taken off the wire         */
    bool    body_whole;                     /* ioma_body_all read it into read_buf                */
    int     body_err;                       /* 0, a status to answer (400, 413), or -1: peer gone */
    size_t  next_req_off, next_req_len;     /* the next pipelined request's bytes, in read_buf    */
    /* a chunked body: its raw bytes are staged in read_buf and decoded from there */
    size_t  raw_pos, raw_end;               /* raw bytes not yet consumed: read_buf[raw_pos, raw_end) */
    size_t  stage_floor;                    /* the stage stays above this: the head, plus a whole */
                                            /* read's decoded body so far                         */
    size_t  chunk_left;                     /* data bytes of the current chunk still to deliver   */
};
#define STATE(ctx) ((struct serve_state *)(ctx)->priv)

/* ── request headers ───────────────────────────────────────────────────────────────────── */

/* Fold A-Z to a-z; every other byte unchanged. */
static inline unsigned char lower_ascii(unsigned char a)
{
    return (unsigned)(a - 'A') < 26U ? (unsigned char)(a | 0x20U) : a;
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
static bool token_present_ci(const char *value, size_t len, const char *tok)
{
    size_t tok_len = strlen(tok);
    size_t at = 0;
    while (at < len) {
        while (at < len && (value[at] == ' ' || value[at] == ',' || value[at] == '\t')) at++;
        size_t start = at;
        while (at < len && value[at] != ',') at++;
        size_t end = at;                                   /* trim trailing blanks */
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
        if (eq_ci(value + start, end - start, tok, tok_len)) return true;
        at++;
    }
    return false;
}

/* The three headers the engine itself needs; p == nullptr when absent. */
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
        if ((unsigned)(s[i] - 'A') < 26U) s[i] += 'a' - 'A';
}

/* One pass over the request headers: lower-case each name in place (the buffer is ours), so
 * handlers and this switch compare with plain memcmp. The switch on the name length rejects
 * nearly every header before a byte is compared. */
static struct picked_headers pick_headers(ioma_request *req)
{
    struct picked_headers picked = { { nullptr, 0 }, { nullptr, 0 }, { nullptr, 0 } };
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
        tmp[i++] = (char)('0' + (v % 10));
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

/* The precomposed status line for the common codes; nullptr for the rest (built on the spot). */
static struct cslice status_line(int code)
{
    switch (code) {
    case 200: return CSLICE("HTTP/1.1 200 OK\r\n");
    case 204: return CSLICE("HTTP/1.1 204 No Content\r\n");
    case 400: return CSLICE("HTTP/1.1 400 Bad Request\r\n");
    case 404: return CSLICE("HTTP/1.1 404 Not Found\r\n");
    case 405: return CSLICE("HTTP/1.1 405 Method Not Allowed\r\n");
    case 500: return CSLICE("HTTP/1.1 500 Internal Server Error\r\n");
    default:  return (struct cslice){ nullptr, 0 };
    }
}

/* A bodyless framework reply (parse errors, limits): an error path, so plain snprintf. Best
 * effort; the caller then closes. */
static void send_status(conn_t *conn, int code)
{
    char head[128];
    int  len = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\ncontent-length: 0\r\nconnection: close\r\n\r\n",
                        code, ioma_reason(code));
    await_send(conn, head, (size_t)len);
}

/* How the body is delimited on the wire. */
enum framing {
    FRAME_LENGTH,
    FRAME_CHUNKED,
    FRAME_UNTIL_CLOSE,
};

/* Serialize the head into dst by memcpy of precomposed pieces plus the integer writer - no
 * snprintf. Every field name goes out lower-cased: the engine's own are lowercase literals, a
 * handler's are folded as they are copied. Returns the length, or -1 if it does not fit. */
static int build_head(const ioma_ctx *ctx, char *dst, size_t cap, enum framing framing, size_t body_len)
{
    const ioma_response *res = &ctx->res;
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

    PUTC("content-type: ");
    PUT(res->content_type.p, res->content_type.len);
    PUTC("\r\n");

    if (framing == FRAME_LENGTH) {
        PUTC("content-length: ");
        NEED(20);
        p += put_uint(p, body_len);
        PUTC("\r\n");
    } else if (framing == FRAME_CHUNKED) {
        PUTC("transfer-encoding: chunked\r\n");
    }

    /* Connection: only when it says something. HTTP/1.1 is persistent by default, so a kept-alive
     * 1.1 reply carries none; a 1.0 client that asked for keep-alive is told it got it; a closing
     * reply always says close. */
    bool keep = ctx->req.keep_alive && !res->close;
    if (!keep)
        PUTC("connection: close\r\n");
    else if (ctx->req.minor_version == 0)
        PUTC("connection: keep-alive\r\n");

    for (size_t i = 0; i < res->n_headers; i++) {                 /* names go out lower-cased */
        NEED(res->headers[i].key.len);
        for (size_t j = 0; j < res->headers[i].key.len; j++)
            *p++ = (char)lower_ascii((unsigned char)res->headers[i].key.p[j]);
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

/* Mark the reply dead (the peer is gone, or a head that cannot be built) and fail the call. */
static int fail(ioma_response *res)
{
    res->failed = true;
    return -1;
}

/* Send the slab, with the head in front of it the first time. That first time decides the
 * framing: a final flush with the head unsent means the whole body is here (Content-Length, one
 * send); an early flush means the body outgrew the slab, so it streams - with the declared length
 * if the handler gave one, else chunked on HTTP/1.1, else until close on HTTP/1.0. */
static int flush(ioma_ctx *ctx, bool final)
{
    ioma_response *res  = &ctx->res;
    conn_t        *conn = STATE(ctx)->conn;
    if (res->failed)
        return -1;

    char head[IOMA_HEAD_CAP];
    int  head_len = 0;
    if (!res->head_sent) {                                /* the first send: decide the framing */
        enum framing framing  = FRAME_LENGTH;
        size_t       body_len = res->has_length ? res->content_length : res->len;
        if (!res->has_length && !final) {
            if (ctx->req.minor_version >= 1) {
                framing = FRAME_CHUNKED;
                res->chunked = true;
            } else {
                framing = FRAME_UNTIL_CLOSE;
                res->close = true;
            }
        }
        head_len = build_head(ctx, head, sizeof head, framing, body_len);
        if (head_len < 0)
            return fail(res);
        res->head_sent = true;
    }

    char  *start = res->buf;                              /* the span to send */
    size_t total = res->len;
    if (res->chunked && res->len)
        frame_chunk(res->buf, res->len, &start, &total);

    if (head_len) {
        size_t lead_room = (size_t)(start - (res->buf - IOMA_LEAD));   /* free bytes in front of the span */
        if ((size_t)head_len <= lead_room) {              /* prepend: one contiguous send */
            start -= head_len;
            memcpy(start, head, (size_t)head_len);
            total += (size_t)head_len;
        } else if (await_send(conn, head, (size_t)head_len) < 0) {
            return fail(res);
        }
    }

    res->len = 0;
    if (total && await_send(conn, start, total) < 0)
        return fail(res);
    return 0;
}

/* After the chain: send what is left - the whole reply if nothing went out yet - and close a
 * chunked stream. */
static int finish(ioma_ctx *ctx)
{
    ioma_response *res = &ctx->res;
    if (res->failed)
        return -1;
    bool pending = !res->head_sent || res->len;       /* nothing sent yet, or bytes still in the slab */
    if (pending && flush(ctx, true) < 0)
        return -1;
    if (res->chunked && await_send(STATE(ctx)->conn, "0\r\n\r\n", 5) < 0)
        return -1;
    return 0;
}

/* Append body bytes to the slab; send it, head first, whenever it fills. */
int ioma_write(ioma_ctx *ctx, const void *data, size_t len)
{
    ioma_response *res = &ctx->res;
    if (res->failed)
        return -1;
    const char *src = data;
    while (len) {
        size_t room = res->cap - res->len;
        if (room == 0) {
            if (flush(ctx, false) < 0)
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

/* Something bigger than the whole slab: format it on the heap and write it in pieces. */
static int write_formatted_heap(ioma_ctx *ctx, const char *fmt, va_list ap, size_t len)
{
    char *tmp = malloc(len + 1);
    if (!tmp)
        return -1;
    vsnprintf(tmp, len + 1, fmt, ap);
    int rc = ioma_write(ctx, tmp, len);
    free(tmp);
    return rc;
}

/* Format straight into the slab. If it does not fit the room left, flush and format again into
 * the empty slab; if it would not fit even that, it goes through the heap. */
int ioma_printf(ioma_ctx *ctx, const char *fmt, ...)
{
    ioma_response *res = &ctx->res;
    if (res->failed)
        return -1;
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    size_t room = res->cap - res->len;
    int    n    = vsnprintf(res->buf + res->len, room, fmt, ap);
    va_end(ap);

    int rc = -1;
    if (n < 0) {
        /* a formatting error: nothing written */
    } else if ((size_t)n < room) {                        /* it fit */
        res->len += (size_t)n;
        rc = 0;
    } else if ((size_t)n >= res->cap) {                   /* bigger than the slab itself */
        rc = write_formatted_heap(ctx, fmt, again, (size_t)n);
    } else if (flush(ctx, false) == 0) {                    /* make room, then it fits */
        res->len += (size_t)vsnprintf(res->buf, res->cap, fmt, again);
        rc = 0;
    }
    va_end(again);
    return rc;
}

/* Send what is in the slab now. Starts streaming: the head goes out with it. */
int ioma_flush(ioma_ctx *ctx)
{
    return flush(ctx, false);
}

/* ── the body: read on demand ──────────────────────────────────────────────────────────── */

/* A body failure with a status: the engine answers with it after the handler unless a reply is
 * already streaming, and res.status shows it so a handler can stop before it writes anything. */
static void body_fail(ioma_ctx *ctx, int status)
{
    STATE(ctx)->body_err = status;
    ctx->res.status      = status;
}

/* --- a Content-Length body --- */

/* Up to n body bytes into dst: first the ones that arrived with the head, then straight from the
 * wire, never past the declared length (a pipelined request is never consumed). One receive's
 * worth; the count, or -1 when the peer is gone. */
static int fixed_data(ioma_ctx *ctx, struct serve_state *state, char *dst, size_t n)
{
    size_t remaining = ctx->req.content_length - state->body_read;
    if (n > remaining)
        n = remaining;
    size_t buffered = state->filled - state->head_len;      /* body bytes that came with the head */
    if (state->body_read < buffered) {
        if (n > buffered - state->body_read)
            n = buffered - state->body_read;
        memcpy(dst, state->read_buf + state->head_len + state->body_read, n);
    } else {
        int got = await_recv(state->conn, dst, n);
        if (got <= 0) {
            state->body_err = -1;
            return -1;
        }
        n = (size_t)got;
    }
    state->body_read += n;
    if (state->body_read == ctx->req.content_length)
        state->body_done = true;
    return (int)n;
}

/* --- a chunked body: the raw bytes are staged in read_buf, after the head --- */

/* More raw bytes into the stage, read_buf[raw_pos, raw_end). Below stage_floor the buffer is
 * spoken for, so an empty stage restarts there and a full one is compacted down to it. false with
 * the failure recorded: no room at all (413), or the peer gone. */
static bool raw_fill(ioma_ctx *ctx, struct serve_state *state)
{
    char *buf = state->read_buf;
    if (state->raw_pos == state->raw_end) {
        state->raw_pos = state->raw_end = state->stage_floor;
    } else if (state->raw_end == IOMA_REQ_CAP && state->raw_pos > state->stage_floor) {
        size_t held = state->raw_end - state->raw_pos;
        memmove(buf + state->stage_floor, buf + state->raw_pos, held);
        state->raw_pos = state->stage_floor;
        state->raw_end = state->stage_floor + held;
    }
    if (state->raw_end == IOMA_REQ_CAP) {                     /* a size line, or a whole body, too big */
        body_fail(ctx, 413);
        return false;
    }
    int n = await_recv(state->conn, buf + state->raw_end, IOMA_REQ_CAP - state->raw_end);
    if (n <= 0) {
        state->body_err = -1;
        return false;
    }
    state->raw_end += (size_t)n;
    return true;
}

/* The line at the front of the stage, filling until its CRLF is there. Its length without the
 * CRLF, or -1 with the failure recorded. */
static long raw_line(ioma_ctx *ctx, struct serve_state *state)
{
    for (;;) {
        char *line = state->read_buf + state->raw_pos;
        char *eol  = memmem(line, state->raw_end - state->raw_pos, "\r\n", 2);
        if (eol)
            return eol - line;
        if (!raw_fill(ctx, state))
            return -1;
    }
}

/* After the last chunk: trailer lines up to an empty one, then the body is done and whatever
 * follows is the next request. */
static bool chunk_trailers(ioma_ctx *ctx, struct serve_state *state)
{
    for (;;) {
        long len = raw_line(ctx, state);
        if (len < 0)
            return false;
        state->raw_pos += (size_t)len + 2;
        if (len == 0)
            break;
    }
    state->body_done    = true;
    state->next_req_off = state->raw_pos;
    state->next_req_len = state->raw_end - state->raw_pos;
    return true;
}

/* The next chunk's size line - hex digits, an optional extension, CRLF - into chunk_left. The
 * last chunk (size 0) also takes its trailers and ends the body. */
static bool chunk_header(ioma_ctx *ctx, struct serve_state *state)
{
    long len = raw_line(ctx, state);
    if (len < 0)
        return false;
    const char *line = state->read_buf + state->raw_pos;
    size_t      size = 0, i = 0;
    for (; i < (size_t)len; i++) {
        int digit = ioma__hexval((unsigned char)line[i]);
        if (digit < 0)
            break;
        if (size > (SIZE_MAX >> 4)) {
            body_fail(ctx, 400);
            return false;
        }
        size = (size << 4) | (size_t)digit;
    }
    bool ended = i == (size_t)len || line[i] == ';' || line[i] == ' ' || line[i] == '\t';
    if (i == 0 || !ended) {
        body_fail(ctx, 400);
        return false;
    }
    state->raw_pos += (size_t)len + 2;
    if (size == 0)
        return chunk_trailers(ctx, state);
    state->chunk_left = size;
    return true;
}

/* Up to n data bytes of the current chunk: into dst, or when dst is null in place at the stage
 * floor (a whole read), which then moves past them. What the stage holds, after one fill when it
 * is empty; the CRLF that ends the chunk is consumed with its last byte. The count, or -1. */
static int chunk_data(ioma_ctx *ctx, struct serve_state *state, char *dst, size_t n)
{
    char *buf = state->read_buf;
    if (state->raw_pos == state->raw_end && !raw_fill(ctx, state))
        return -1;
    size_t avail = state->raw_end - state->raw_pos;
    if (n > avail)
        n = avail;
    if (n > state->chunk_left)
        n = state->chunk_left;
    if (dst) {
        memcpy(dst, buf + state->raw_pos, n);
    } else {
        memmove(buf + state->stage_floor, buf + state->raw_pos, n);   /* leftwards: the floor never passes raw_pos */
        state->stage_floor += n;
    }
    state->raw_pos    += n;
    state->chunk_left -= n;
    state->body_read  += n;
    if (state->chunk_left == 0) {
        while (state->raw_end - state->raw_pos < 2)
            if (!raw_fill(ctx, state))
                return -1;
        if (buf[state->raw_pos] != '\r' || buf[state->raw_pos + 1] != '\n') {
            body_fail(ctx, 400);
            return -1;
        }
        state->raw_pos += 2;
    }
    return (int)n;
}

/* --- the three reads --- */

/* The whole body, in place right after the head: a Content-Length body is received until it is
 * complete, a chunked one is decoded down over its own raw bytes. Once; then the slice, which is
 * also req.body. */
ioma_slice ioma_body_all(ioma_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    ioma_request       *req   = &ctx->req;
    const ioma_slice    none  = { req->body.p, 0 };

    if (state->body_whole)
        return req->body;
    if (state->body_read || state->body_err)                    /* already streaming, or failed */
        return none;

    char *body = state->read_buf + state->head_len;
    if (req->chunked) {
        while (!state->body_done) {
            if (state->chunk_left == 0) {
                if (!chunk_header(ctx, state))
                    return none;
            } else if (chunk_data(ctx, state, nullptr, state->chunk_left) < 0) {
                return none;
            }
        }
        req->body = (ioma_slice){ body, state->stage_floor - state->head_len };
    } else {
        size_t total = state->head_len + req->content_length;
        if (total > IOMA_REQ_CAP) {
            body_fail(ctx, 413);
            return none;
        }
        while (state->filled < total) {
            int n = await_recv(state->conn, state->read_buf + state->filled, IOMA_REQ_CAP - state->filled);
            if (n <= 0) {
                state->body_err = -1;
                return none;
            }
            state->filled += (size_t)n;
        }
        req->body           = (ioma_slice){ body, req->content_length };
        state->next_req_off = total;
        state->next_req_len = state->filled - total;
        state->body_read    = req->content_length;
        state->body_done    = true;
    }
    state->body_whole = true;
    return req->body;
}

/* The next bytes of the body into dst, reading until n are there or the body ends. */
int ioma_body_read_until(ioma_ctx *ctx, void *dst, size_t n)
{
    struct serve_state *state = STATE(ctx);
    if (state->body_err || n == 0)
        return -1;
    char  *out = dst;
    size_t got = 0;
    while (got < n && !state->body_done) {
        int k;
        if (!ctx->req.chunked)
            k = fixed_data(ctx, state, out + got, n - got);
        else if (state->chunk_left == 0)
            k = chunk_header(ctx, state) ? 0 : -1;
        else
            k = chunk_data(ctx, state, out + got, n - got);
        if (k < 0)
            return -1;
        got += (size_t)k;
    }
    return (int)got;
}

/* The next chunk of a chunked body, whole, into dst: the rest of the current one when a read
 * stopped inside it, else the next. */
int ioma_body_read_chunk(ioma_ctx *ctx, void *dst, size_t cap)
{
    struct serve_state *state = STATE(ctx);
    if (state->body_err || !ctx->req.chunked)
        return -1;
    if (state->body_done)
        return 0;
    if (state->chunk_left == 0) {
        if (!chunk_header(ctx, state))
            return -1;
        if (state->body_done)
            return 0;
    }
    if (state->chunk_left > cap) {                              /* the chunk does not fit dst */
        body_fail(ctx, 413);
        return -1;
    }
    char  *out  = dst;
    size_t want = state->chunk_left, got = 0;
    while (got < want) {
        int k = chunk_data(ctx, state, out + got, want - got);
        if (k < 0)
            return -1;
        got += (size_t)k;
    }
    return (int)got;
}

/* After the chain: take an unread body off the wire so the connection stays in sync, up to a
 * limit - past it, the reply says close and the rest is never read. */
static void drain_body(ioma_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    char   tmp[4096];
    size_t drained = 0;
    while (!state->body_done && !state->body_err) {
        if (drained >= IOMA_DRAIN_MAX) {
            ctx->res.close = true;
            return;
        }
        int n = ioma_body_read_until(ctx, tmp, sizeof tmp);
        if (n <= 0)
            return;
        drained += (size_t)n;
    }
}

/* ── the connection loop ───────────────────────────────────────────────────────────────── */

/* Get a complete request head into read_buf, parsed straight into req (method, target,
 * version, headers). What is buffered is parsed first: after a reply, a pipelined next request
 * may already be there. More is read only when the head is incomplete. Returns the head's
 * length, or -1 once the connection is finished (431 or 400 answered, or the peer went away). */
static long read_head(conn_t *conn, char *read_buf, size_t *filled, ioma_request *req)
{
    size_t already_parsed = 0;                        /* what the previous attempt scanned */
    for (;;) {
        if (*filled) {
            req->n_headers = IOMA_MAX_HEADERS;        /* in: room; out: count */
            int parsed = phr_parse_request(read_buf, *filled,
                                           &req->method.p, &req->method.len,
                                           &req->target.p, &req->target.len,
                                           &req->minor_version,
                                           (struct phr_header *)req->headers, &req->n_headers,
                                           already_parsed);
            if (parsed >= 0)
                return parsed;
            if (parsed == -1) {                       /* malformed */
                send_status(conn, 400);
                return -1;
            }
            already_parsed = *filled;                 /* incomplete: read more */
        }
        if (*filled == IOMA_REQ_CAP) {
            send_status(conn, 431);
            return -1;
        }
        int n = await_recv(conn, read_buf + *filled, IOMA_REQ_CAP - *filled);
        if (n <= 0)
            return -1;                                /* peer closed or error */
        *filled += (size_t)n;
    }
}

/* The rest of the request from its head: path and query, the query split into params, the
 * headers lower-cased and the three the engine needs picked out. The body stays on the wire;
 * body_start is where it begins. */
static void fill_request(ioma_request *req, const char *body_start, char *params_arena, size_t arena_cap)
{
    const char *qmark = memchr(req->target.p, '?', req->target.len);
    if (qmark) {
        req->path  = (ioma_slice){ req->target.p, (size_t)(qmark - req->target.p) };
        req->query = (ioma_slice){ qmark + 1, req->target.len - req->path.len - 1 };
    } else {
        req->path  = req->target;
        req->query = (ioma_slice){ req->target.p + req->target.len, 0 };
    }
    req->n_params = req->query.len
        ? ioma_kv_parse(req->query.p, req->query.len, req->params, IOMA_MAX_PARAMS, params_arena, arena_cap)
        : 0;
    req->n_route_params = 0;                          /* the router fills these */

    struct picked_headers picked = pick_headers(req);
    req->chunked        = picked.transfer_enc.p && token_present_ci(picked.transfer_enc.p, picked.transfer_enc.len, "chunked");
    req->content_length = picked.content_length.p ? parse_size(picked.content_length.p, picked.content_length.len) : 0;
    req->keep_alive     = keep_alive_from(req->minor_version, picked.connection);
    req->body           = (ioma_slice){ body_start, 0 };
}

/* The engine's bookkeeping for reading the body on demand: where it starts, what already
 * arrived with the head, and - for a Content-Length body that is entirely here - where the next
 * request starts. */
static void init_body_state(struct serve_state *state, conn_t *conn, char *read_buf, size_t filled,
                            size_t head_len, const ioma_request *req)
{
    memset(state, 0, sizeof *state);
    state->conn        = conn;
    state->read_buf    = read_buf;
    state->filled      = filled;
    state->head_len    = head_len;
    state->raw_pos     = head_len;                    /* chunked: what came with the head is raw body */
    state->raw_end     = filled;
    state->stage_floor = head_len;
    if (!req->chunked) {
        size_t total = head_len + req->content_length;
        state->body_done = req->content_length == 0;
        if (filled >= total) {
            state->next_req_off = total;
            state->next_req_len = filled - total;
        }
    }
}

/* A response with its defaults and an empty slab. */
static void init_response(ioma_response *res, char *slab)
{
    res->status         = 200;
    res->content_type   = (ioma_slice){ "text/plain", 10 };
    res->n_headers      = 0;                          /* headers[] is only read up to here */
    res->close          = false;
    res->head_sent      = false;
    res->content_length = 0;
    res->has_length     = false;
    res->buf            = slab + IOMA_LEAD;
    res->cap            = IOMA_OUT_CAP;
    res->len            = 0;
    res->chunked        = false;
    res->failed         = false;
}

/* After a kept-alive reply: move the bytes that belong to the next request to the front of the
 * buffer and return how many there are. */
static size_t carry_next_request(struct serve_state *state, char *read_buf)
{
    if (state->next_req_len)
        memmove(read_buf, read_buf + state->next_req_off, state->next_req_len);
    return state->next_req_len;
}

/* The proactor handler for every connection: one request per iteration - get the head, run
 * the chain against a context, drain what it left of the body, send what it wrote - while kept
 * alive. Returning closes the connection. */
void ioma__serve(conn_t *conn)
{
    char   read_buf[IOMA_REQ_CAP];                    /* the request: head, then body bytes */
    char   params[IOMA_PARAM_CAP];                    /* decoded query parameters           */
    char   slab[IOMA_LEAD + IOMA_OUT_CAP + 2];        /* lead, the write slab, CRLF slack   */
    size_t filled = 0;                                /* bytes in read_buf                  */

    for (;;) {
        ioma_ctx           ctx;                       /* this request's context             */
        struct serve_state state;                     /* and the engine's side of it        */

        long head_len = read_head(conn, read_buf, &filled, &ctx.req);
        if (head_len < 0)
            return;
        fill_request(&ctx.req, read_buf + (size_t)head_len, params, sizeof params);
        init_body_state(&state, conn, read_buf, filled, (size_t)head_len, &ctx.req);
        init_response(&ctx.res, slab);
        ctx.user = nullptr;
        ctx.priv = &state;

        ioma__dispatch(&ctx);                         /* middleware chain + endpoint */

        if (state.body_err) {                         /* too large, malformed, or gone */
            if (state.body_err > 0 && !ctx.res.head_sent)
                send_status(conn, state.body_err);
            return;
        }
        drain_body(&ctx);                             /* what the handler left unread */
        if (state.body_err)
            return;
        if (finish(&ctx) < 0)                         /* sends; suspends meanwhile */
            return;
        if (!ctx.req.keep_alive || ctx.res.close)
            return;
        filled = carry_next_request(&state, read_buf);
    }
}

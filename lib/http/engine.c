/*
 * engine.c - the HTTP/1.1 engine: parse a request head with picohttpparser, run the middleware
 * chain and the endpoint against a context, read the body on demand (whole or streamed) through
 * the connection's pipe (io/pipe.h) and drain
 * what was left, then send what was written through the same pipe. All of it runs on the
 * connection's coroutine, so a read or a flush simply suspends it and the loop resumes it.
 */
#include "http/engine.h"
#include "http/internal.h"
#include "http/router.h"
#include "io/pipe.h"
#include "picohttpparser.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IOXD_PARAM_CAP
#define IOXD_PARAM_CAP    2048      /* per-request arena for percent-decoded query parameters */
#endif
#ifndef IOXD_HEAD_CAP
#define IOXD_HEAD_CAP     4096      /* a serialized reply head must fit here                  */
#endif
#ifndef IOXD_DRAIN_MAX
#define IOXD_DRAIN_MAX    (1024UL * 1024)   /* unread body discarded after a handler before we close instead */
#endif
#ifndef IOXD_TRAILER_MAX
#define IOXD_TRAILER_MAX  4096      /* bytes of chunked trailers taken before the body is a 400 */
#endif

/* serve() hands req.headers to picohttpparser as its header array: a kv (two slices) must lay
 * out exactly like a phr_header (name, name_len, value, value_len). */
static_assert(sizeof(ioxd_kv) == sizeof(struct phr_header), "ioxd_kv must mirror phr_header");
static_assert(offsetof(ioxd_kv, key)    == offsetof(struct phr_header, name) &&
               offsetof(ioxd_slice, len) == offsetof(struct phr_header, name_len) &&
               offsetof(ioxd_kv, value)  == offsetof(struct phr_header, value),
               "ioxd_kv must mirror phr_header");

/* The engine's per-request state, behind ctx->priv. */
struct serve_state {
    struct ioxd_pipe *pipe;                 /* the connection: the head is kept in its reader     */
    size_t       body_read;                 /* body bytes handed out so far                       */
    bool         body_done;                 /* the whole body has been taken off the wire         */
    bool         body_whole;                /* ioxd_body_all kept it                              */
    int          body_err;                  /* 0, a status to answer (400, 413), or -1: peer gone */
    size_t       chunk_left;                /* data bytes of the current chunk still to deliver   */
    bool         head_only;                 /* a HEAD request: the reply's body stays unsent      */
    bool         no_body;                   /* decided with the head: HEAD, 1xx, 204, 304         */
    bool         continue_sent;             /* the 100 Continue an Expect asked for went out      */
};
#define STATE(ctx)  ((struct serve_state *)(ctx)->priv)
#define READER(ctx) (&STATE(ctx)->pipe->in)
#define WRITER(ctx) (&STATE(ctx)->pipe->out)

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

/* A Content-Length value: decimal digits and nothing else, into *out; false for anything else,
 * including a number too large for size_t. Anything looser lets one request smuggle another
 * behind a proxy that reads the value differently. */
static bool parse_length(const char *s, size_t n, size_t *out)
{
    if (n == 0)
        return false;
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = (unsigned char)s[i] - (unsigned)'0';    /* wraps huge for a non-digit */
        if (d > 9 || v > (SIZE_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* The comma-separated tokens of a list header value, one per call from *at, blanks trimmed;
 * false once the list is done. */
static bool next_token(const char *value, size_t len, size_t *at, ioxd_slice *tok)
{
    while (*at < len && (value[*at] == ' ' || value[*at] == ',' || value[*at] == '\t')) (*at)++;
    if (*at >= len)
        return false;
    size_t start = *at;
    while (*at < len && value[*at] != ',') (*at)++;
    size_t end = *at;                                      /* trim trailing blanks */
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
    *tok = (ioxd_slice){ value + start, end - start };
    return true;
}

/* Is `tok` one of the comma-separated tokens in the header value [s, s+n)? Case-insensitive. */
static bool token_present_ci(const char *value, size_t len, const char *tok)
{
    size_t     tok_len = strlen(tok), at = 0;
    ioxd_slice t;
    while (next_token(value, len, &at, &t))
        if (eq_ci(t.p, t.len, tok, tok_len))
            return true;
    return false;
}

/* What the engine picks from the request headers as it lower-cases their names: the framing
 * fields, Host, Connection and Expect - and whether they make a request it must refuse. */
struct picked_headers {
    ioxd_slice content_length;
    unsigned   n_content_length, n_transfer_enc, n_host;
    bool       te_last_chunked;             /* the last transfer coding named is chunked        */
    bool       te_other;                    /* a coding other than that final chunked was named  */
    bool       close, keep_alive;           /* over every Connection line                        */
    bool       expect_continue;
    int        refuse;                      /* 0, or the status to answer: 400, 417              */
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

/* One Transfer-Encoding line into the picked state: its codings join the list the earlier
 * lines made, so a chunked that was last is last no more. */
static void pick_transfer_encoding(struct picked_headers *picked, ioxd_slice value)
{
    picked->n_transfer_enc++;
    if (picked->te_last_chunked)
        picked->te_other = true;
    picked->te_last_chunked = false;
    size_t     at = 0;
    ioxd_slice tok;
    bool       any = false;
    while (next_token(value.p, value.len, &at, &tok)) {
        any = true;
        size_t     peek = at;
        ioxd_slice more;
        bool last = !next_token(value.p, value.len, &peek, &more);
        if (last && eq_ci(tok.p, tok.len, "chunked", 7))
            picked->te_last_chunked = true;
        else
            picked->te_other = true;
    }
    if (!any)
        picked->refuse = 400;                              /* an empty list */
}

/* One pass over the request headers: lower-case each name in place (the buffer is ours), so
 * handlers and this switch compare with plain memcmp. The switch on the name length rejects
 * nearly every header before a byte is compared. A folded continuation line (obs-fold) comes
 * from the parser with no name: it must not be interpreted, so the request is refused. */
static struct picked_headers pick_headers(ioxd_request *req)
{
    struct picked_headers picked = {};
    for (size_t i = 0; i < req->n_headers; i++) {
        ioxd_kv *hdr  = &req->headers[i];
        char    *name = (char *)hdr->key.p;
        if (!name) {
            picked.refuse = 400;
            continue;
        }
        lower_inplace(name, hdr->key.len);
        switch (hdr->key.len) {
        case 4:
            if (memcmp(name, "host", 4) == 0) picked.n_host++;
            break;
        case 6:
            if (memcmp(name, "expect", 6) == 0) {
                if (eq_ci(hdr->value.p, hdr->value.len, "100-continue", 12)) picked.expect_continue = true;
                else picked.refuse = 417;
            }
            break;
        case 10:
            if (memcmp(name, "connection", 10) == 0) {
                if (token_present_ci(hdr->value.p, hdr->value.len, "close"))      picked.close = true;
                if (token_present_ci(hdr->value.p, hdr->value.len, "keep-alive")) picked.keep_alive = true;
            }
            break;
        case 14:
            if (memcmp(name, "content-length", 14) == 0) {
                if (picked.n_content_length++ && !(hdr->value.len == picked.content_length.len
                        && memcmp(hdr->value.p, picked.content_length.p, hdr->value.len) == 0))
                    picked.refuse = 400;                   /* two that disagree */
                picked.content_length = hdr->value;
            }
            break;
        case 17:
            if (memcmp(name, "transfer-encoding", 17) == 0) pick_transfer_encoding(&picked, hdr->value);
            break;
        default:
            break;
        }
    }
    return picked;
}

/* HTTP/1.1 keeps alive unless a Connection line says "close"; HTTP/1.0 only with "keep-alive". */
static bool keep_alive_from(int minor_version, const struct picked_headers *picked)
{
    if (picked->close)
        return false;
    return minor_version >= 1 || picked->keep_alive;
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
static void send_status(ioxd_pipewriter *pw, int code)
{
    char head[128];
    int  len = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\ncontent-length: 0\r\nconnection: close\r\n\r\n",
                        code, ioxd_reason(code));
    if (len < 0)
        return;
    if ((size_t)len >= sizeof head)
        len = (int)sizeof head - 1;
    ioxd_pipewriter_reset(pw);                        /* whatever the handler had buffered is moot */
    ioxd_pipewriter_send(pw, head, (size_t)len);
}

/* How the body is delimited on the wire. */
enum framing {
    FRAME_LENGTH,
    FRAME_CHUNKED,
    FRAME_UNTIL_CLOSE,
    FRAME_NONE,                             /* a reply that cannot have one: 1xx, 204, a 304 with no length */
};

/* The status as it goes on the wire: three digits, or a 500 for a handler's mistake. */
static int wire_status(int status)
{
    return status >= 100 && status <= 999 ? status : 500;
}

/* Serialize the head into dst by memcpy of precomposed pieces plus the integer writer - no
 * snprintf. Every field name goes out lower-cased: the engine's own are lowercase literals, a
 * handler's were folded as ioxd_header copied them, so they are one memcpy of the arena.
 * Returns the length, or -1 if it does not fit. */
static int build_head(const ioxd_ctx *ctx, char *dst, size_t cap, enum framing framing, size_t body_len)
{
    const ioxd_response *res = &ctx->res;
    char *p   = dst;
    char *end = dst + cap;
    int   status = wire_status(res->status);

#define NEED(n)     do { if ((size_t)(end - p) < (size_t)(n)) return -1; } while (0)
#define PUT(src, n) do { NEED(n); memcpy(p, (src), (size_t)(n)); p += (n); } while (0)
#define PUTC(lit)   PUT((lit), sizeof(lit) - 1)

    struct cslice line = status_line(status);
    if (line.p) {
        PUT(line.p, line.len);
    } else {
        PUTC("HTTP/1.1 ");
        NEED(3);
        p += put_uint(p, (size_t)status);
        PUTC(" ");
        const char *reason = ioxd_reason(status);
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

    PUT(res->head, res->head_len);                               /* the handler's lines, serialized as added */

    PUTC("\r\n");
#undef PUTC
#undef PUT
#undef NEED
    return (int)(p - dst);
}

/* The chunked terminator: the empty last chunk and the end of the trailers. */
static void put_terminator(char *at)
{
    at[0] = '0';
    at[1] = '\r';
    at[2] = '\n';
    at[3] = '\r';
    at[4] = '\n';
}

/* Mark the reply dead (the peer is gone, or a head that cannot be built) and fail the call. */
static int fail(ioxd_response *res)
{
    res->failed = true;
    return -1;
}

/* Send the slab, with the head in front of it the first time. That first time decides the
 * framing: a final flush with the head unsent means the whole body is here (Content-Length, one
 * send); an early flush means the body outgrew the slab, so it streams - with the declared length
 * if the handler gave one, else chunked on HTTP/1.1, else until close on HTTP/1.0. It also
 * settles what the request and the status dictate: no body at all after HEAD, a 1xx, 204 or
 * 304 (RFC 9112 6.3), no framing header on a 1xx or 204, and "connection: close" when the
 * body still on the wire is more than will be drained. A declared length is held to: a whole
 * buffered reply gets the real one, a stream never exceeds it, and one that falls short closes. */
static int flush(ioxd_ctx *ctx, bool final)
{
    ioxd_response      *res   = &ctx->res;
    ioxd_pipewriter    *pw    = WRITER(ctx);
    struct serve_state *state = STATE(ctx);
    if (res->failed)
        return -1;
    char head[IOXD_HEAD_CAP];
    int  head_len = 0;
    if (!res->head_sent) {                                /* the first send: decide the framing */
        int          status   = wire_status(res->status);
        bool         bodyless = status < 200 || status == 204 || status == 304;
        enum framing framing  = FRAME_LENGTH;
        size_t       body_len = res->has_length && !final ? res->content_length : pw->len;
        state->no_body = state->head_only || bodyless;
        if (res->has_length && final)
            res->content_length = pw->len;                /* buffered whole: the length is what was written */
        if (bodyless) {
            framing = status == 304 && res->has_length ? FRAME_LENGTH : FRAME_NONE;
        } else if (!res->has_length && !final) {
            if (ctx->req.minor_version >= 1) {
                framing = FRAME_CHUNKED;
                res->chunked = true;
            } else {
                framing = FRAME_UNTIL_CLOSE;
                res->close = true;
            }
        }
        if (!state->body_done && !state->body_err) {      /* a body left unread: more than the drain takes means close */
            size_t left = ctx->req.chunked ? SIZE_MAX : ctx->req.content_length - state->body_read;
            if (left > IOXD_DRAIN_MAX || (ctx->req.expect_continue && !state->continue_sent))
                res->close = true;                        /* an expecting client may never send it at all */
        }
        head_len = build_head(ctx, head, sizeof head, framing, body_len);
        if (head_len < 0) {
            send_status(pw, 500);
            return fail(res);
        }
        res->head_sent = true;
    }
    if (state->no_body) {                                 /* the head alone; what was written as a body stays here */
        ioxd_pipewriter_reset(pw);
        if (head_len && ioxd_pipewriter_through(pw, head, (size_t)head_len) < 0)
            return fail(res);
        return 0;
    }
    bool over = false;
    if (res->has_length) {                                /* never a byte past the declared length */
        size_t left = res->content_length - res->body_sent;
        if (pw->len > left) {
            pw->len = left;
            over    = true;
        }
    }
    res->body_sent += pw->len;
    if (res->chunked && pw->len) {                        /* the slab's bytes as one chunk: size in front, CRLF behind */
        char  size_line[16];
        int   digits = put_hex(size_line, pw->len);
        char *front  = ioxd_pipewriter_front(pw, (size_t)digits + 2);
        char *back   = ioxd_pipewriter_back(pw, 2);
        if (!front || !back)
            return fail(res);
        memcpy(front, size_line, (size_t)digits);
        front[digits]     = '\r';
        front[digits + 1] = '\n';
        back[0] = '\r';
        back[1] = '\n';
    }
    if (final && res->chunked) {                          /* the terminator rides the same send */
        char *back = ioxd_pipewriter_back(pw, 5);
        if (!back)
            return fail(res);
        put_terminator(back);
    }
    if (head_len) {
        char *front = ioxd_pipewriter_front(pw, (size_t)head_len);
        if (front)
            memcpy(front, head, (size_t)head_len);        /* one contiguous send */
        else if (ioxd_pipewriter_through(pw, head, (size_t)head_len) < 0)   /* bigger than the lead: on its own, first */
            return fail(res);
    }
    if (ioxd_pipewriter_flush(pw) < 0)
        return fail(res);
    if (over)
        return fail(res);                                 /* the handler wrote past its own length: nothing more goes */
    return 0;
}

/* After the chain: send what is left - the whole reply if nothing went out yet - and close a
 * chunked stream. A streamed reply that fell short of its declared length closes the
 * connection, so the client sees it was cut. */
static int finish(ioxd_ctx *ctx)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed)
        return -1;
    if (!res->head_sent || pw->len) {                     /* nothing sent yet, or bytes still in the slab */
        if (flush(ctx, true) < 0)
            return -1;
    } else if (res->chunked) {                            /* streamed and drained: just the terminator */
        char *back = ioxd_pipewriter_back(pw, 5);
        if (!back)
            return fail(res);
        put_terminator(back);
        if (ioxd_pipewriter_flush(pw) < 0)
            return fail(res);
    }
    if (res->has_length && !STATE(ctx)->no_body && res->body_sent != res->content_length)
        res->close = true;
    return 0;
}

/* Append body bytes to the slab; send it, head first, whenever it fills. */
int ioxd_write(ioxd_ctx *ctx, const void *data, size_t len)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed)
        return -1;
    const char *src = data;
    while (len) {
        size_t room = ioxd_pipewriter_room(pw);
        if (room == 0) {
            if (flush(ctx, false) < 0)
                return -1;
            continue;
        }
        size_t n = len < room ? len : room;
        memcpy(ioxd_pipewriter_at(pw), src, n);
        ioxd_pipewriter_advance(pw, n);
        src += n;
        len -= n;
    }
    return 0;
}

/* Something bigger than the whole slab: format it on the heap and write it in pieces. */
static int write_formatted_heap(ioxd_ctx *ctx, const char *fmt, va_list ap, size_t len)
{
    char *tmp = malloc(len + 1);
    if (!tmp)
        return fail(&ctx->res);                           /* the reply cannot be completed as promised */
    vsnprintf(tmp, len + 1, fmt, ap);
    int rc = ioxd_write(ctx, tmp, len);
    free(tmp);
    return rc;
}

/* Format straight into the slab. If it does not fit the room left, flush and format again into
 * the empty slab; if it would not fit even that, it goes through the heap. */
int ioxd_printf(ioxd_ctx *ctx, const char *fmt, ...)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed)
        return -1;
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    size_t room = ioxd_pipewriter_room(pw);
    int    n    = vsnprintf(ioxd_pipewriter_at(pw), room, fmt, ap);
    va_end(ap);

    int rc = -1;
    if (n < 0) {
        /* a formatting error: nothing written */
    } else if ((size_t)n < room) {                        /* it fit */
        ioxd_pipewriter_advance(pw, (size_t)n);
        rc = 0;
    } else if ((size_t)n >= pw->cap) {                    /* bigger than the slab itself */
        rc = write_formatted_heap(ctx, fmt, again, (size_t)n);
    } else if (flush(ctx, false) == 0) {                  /* make room, then it fits */
        ioxd_pipewriter_advance(pw, (size_t)vsnprintf(ioxd_pipewriter_at(pw), pw->cap, fmt, again));
        rc = 0;
    }
    va_end(again);
    return rc;
}

/* Send what is in the slab now. Starts streaming: the head goes out with it. */
int ioxd_flush(ioxd_ctx *ctx)
{
    return flush(ctx, false);
}

/* n bytes of the reply to write into directly, flushing first when they do not fit. */
void *ioxd_reserve(ioxd_ctx *ctx, size_t n)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed || n > pw->cap)
        return nullptr;
    if (n > ioxd_pipewriter_room(pw) && flush(ctx, false) < 0)
        return nullptr;
    return ioxd_pipewriter_at(pw);
}

/* The caller wrote n of the reserved bytes. */
void ioxd_advance(ioxd_ctx *ctx, size_t n)
{
    ioxd_pipewriter_advance(WRITER(ctx), n);
}

/* ── the body: read on demand ──────────────────────────────────────────────────────────── */

/* A body failure with a status: the engine answers with it after the handler unless a reply is
 * already streaming, and res.status shows it so a handler can stop before it writes anything. */
static void body_fail(ioxd_ctx *ctx, int status)
{
    STATE(ctx)->body_err = status;
    ctx->res.status      = status;
}

/* Before the first read of the body: a client that sent "Expect: 100-continue" is waiting for
 * the interim reply before it sends a byte (RFC 9110 10.1.1), so it goes out now, ahead of
 * anything the handler has buffered. False when the peer is gone. */
static bool body_begin(ioxd_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    if (!ctx->req.expect_continue || state->continue_sent || ctx->res.head_sent)
        return true;
    state->continue_sent = true;
    if (ioxd_pipewriter_through(WRITER(ctx), "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) {
        state->body_err = -1;
        return false;
    }
    return true;
}

/* The live bytes with something unexamined, or more of them; a failure recorded: no room is a
 * 413, the peer gone or the input ending inside the body is -1. */
static bool body_bytes(ioxd_ctx *ctx, ioxd_slice *live)
{
    int rc = ioxd_pipereader_read(READER(ctx), live);
    if (rc > 0)
        return true;
    if (rc == IOXD_PIPE_FULL)
        body_fail(ctx, 413);
    else
        STATE(ctx)->body_err = -1;
    return false;
}

/* --- a chunked body --- */

/* The line at the front of the live bytes, whole: its length without the CRLF, or -1 recorded. */
static long body_line(ioxd_ctx *ctx, ioxd_slice *live)
{
    for (;;) {
        if (!body_bytes(ctx, live))
            return -1;
        const char *eol = memmem(live->p, live->len, "\r\n", 2);
        if (eol)
            return eol - live->p;
        ioxd_pipereader_examine(READER(ctx), live->len);
    }
}

/* After the last chunk: trailer lines up to an empty one, then the body is done. They are
 * dropped unread, and bounded, so a peer cannot hold the connection with an endless trailer. */
static bool chunk_trailers(ioxd_ctx *ctx)
{
    ioxd_slice live  = { nullptr, 0 };
    size_t     taken = 0;
    for (;;) {
        long len = body_line(ctx, &live);
        if (len < 0)
            return false;
        taken += (size_t)len + 2;
        if (taken > IOXD_TRAILER_MAX) {
            body_fail(ctx, 400);
            return false;
        }
        ioxd_pipereader_drop(READER(ctx), (size_t)len + 2);
        if (len == 0)
            break;
    }
    STATE(ctx)->body_done = true;
    return true;
}

/* The next chunk's size line - hex digits, an optional extension, CRLF - into chunk_left. The
 * last chunk (size 0) also takes its trailers and ends the body. The line is held to the
 * grammar (RFC 9112 7.1): digits, then either the CRLF or blanks and a ';' with something after
 * it; a size line the reader cannot hold is malformed, not too large. */
static bool chunk_header(ioxd_ctx *ctx)
{
    ioxd_slice live = { nullptr, 0 };
    long len = body_line(ctx, &live);
    if (len < 0) {
        if (STATE(ctx)->body_err == 413)
            body_fail(ctx, 400);
        return false;
    }
    size_t size = 0, i = 0;
    for (; i < (size_t)len; i++) {
        int digit = ioxd__hexval((unsigned char)live.p[i]);
        if (digit < 0)
            break;
        if (size > (SIZE_MAX >> 4)) {
            body_fail(ctx, 400);
            return false;
        }
        size = (size << 4) | (size_t)digit;
    }
    size_t j = i;
    while (j < (size_t)len && (live.p[j] == ' ' || live.p[j] == '\t')) j++;
    bool ended = (j == (size_t)len && j == i) || (j + 1 < (size_t)len && live.p[j] == ';');
    if (i == 0 || !ended) {
        body_fail(ctx, 400);
        return false;
    }
    ioxd_pipereader_drop(READER(ctx), (size_t)len + 2);
    if (size == 0)
        return chunk_trailers(ctx);
    STATE(ctx)->chunk_left = size;
    return true;
}

/* The CRLF that ends a chunk's data. */
static bool chunk_end(ioxd_ctx *ctx)
{
    ioxd_slice live = { nullptr, 0 };
    for (;;) {
        if (!body_bytes(ctx, &live))
            return false;
        if (live.len >= 2)
            break;
        ioxd_pipereader_examine(READER(ctx), live.len);
    }
    if (live.p[0] != '\r' || live.p[1] != '\n') {
        body_fail(ctx, 400);
        return false;
    }
    ioxd_pipereader_drop(READER(ctx), 2);
    return true;
}

/* --- the three reads --- */

/* The whole body, kept in the reader: a Content-Length body once it is all here, a chunked one
 * chunk by chunk with the data slid together. In place after the head when it all arrived in
 * one kernel buffer, gathered otherwise. Once; then the slice, which is also req.body. */
ioxd_slice ioxd_body_all(ioxd_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    ioxd_pipereader        *pr    = &state->pipe->in;
    ioxd_request       *req   = &ctx->req;
    const ioxd_slice    none  = { nullptr, 0 };

    if (state->body_whole)
        return req->body;
    if (state->body_read || state->body_err)                    /* already streaming, or failed */
        return none;
    if (!state->body_done && !body_begin(ctx))
        return none;

    if (req->chunked) {
        while (!state->body_done) {
            if (state->chunk_left == 0) {
                if (!chunk_header(ctx))
                    return none;
                continue;
            }
            ioxd_slice live = { nullptr, 0 };
            if (!body_bytes(ctx, &live))
                return none;
            size_t n = live.len < state->chunk_left ? live.len : state->chunk_left;
            if (!ioxd_pipereader_keep(pr, n)) {
                body_fail(ctx, 413);
                return none;
            }
            state->chunk_left -= n;
            state->body_read  += n;
            if (state->chunk_left == 0 && !chunk_end(ctx))
                return none;
        }
        req->body = ioxd_pipereader_run(pr);
    } else {
        if (req->content_length > pr->cap) {
            body_fail(ctx, 413);
            return none;
        }
        ioxd_slice live = none;
        while (req->content_length && live.len < req->content_length) {
            if (live.len)
                ioxd_pipereader_examine(pr, live.len);
            if (!body_bytes(ctx, &live))
                return none;
        }
        const char *kept = req->content_length ? ioxd_pipereader_keep(pr, req->content_length) : live.p;
        if (req->content_length && !kept) {
            body_fail(ctx, 413);
            return none;
        }
        req->body        = (ioxd_slice){ kept, req->content_length };
        state->body_read = req->content_length;
        state->body_done = true;
    }
    state->body_whole = true;
    return req->body;
}

/* Up to n bytes of a Content-Length body into dst, straight from the reader. */
static int fixed_data(ioxd_ctx *ctx, struct serve_state *state, char *dst, size_t n)
{
    size_t remaining = ctx->req.content_length - state->body_read;
    if (n > remaining)
        n = remaining;
    int got = ioxd_pipereader_copy(&state->pipe->in, dst, n);
    if (got <= 0) {
        state->body_err = -1;                            /* gone, or the input ended inside the body */
        return -1;
    }
    state->body_read += (size_t)got;
    if (state->body_read == ctx->req.content_length)
        state->body_done = true;
    return got;
}

/* Up to n data bytes of the current chunk into dst; the CRLF after its last byte is taken too. */
static int chunk_data(ioxd_ctx *ctx, struct serve_state *state, char *dst, size_t n)
{
    if (n > state->chunk_left)
        n = state->chunk_left;
    int got = ioxd_pipereader_copy(&state->pipe->in, dst, n);
    if (got <= 0) {
        state->body_err = -1;
        return -1;
    }
    state->chunk_left -= (size_t)got;
    state->body_read  += (size_t)got;
    if (state->chunk_left == 0 && !chunk_end(ctx))
        return got;                                      /* what was copied still counts; the next call fails */
    return got;
}

/* The next bytes of the body into dst, reading until n are there or the body ends. */
int ioxd_body_read_until(ioxd_ctx *ctx, void *dst, size_t n)
{
    struct serve_state *state = STATE(ctx);
    if (state->body_err)
        return -1;
    if (n == 0 || state->body_done)
        return 0;
    if (!body_begin(ctx))
        return -1;
    if (n > INT_MAX)
        n = INT_MAX;                                     /* the count comes back as an int */
    char  *out = dst;
    size_t got = 0;
    while (got < n && !state->body_done) {
        int k;
        if (!ctx->req.chunked)
            k = fixed_data(ctx, state, out + got, n - got);
        else if (state->chunk_left == 0)
            k = chunk_header(ctx) ? 0 : -1;
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
int ioxd_body_read_next_chunk(ioxd_ctx *ctx, void *dst, size_t cap)
{
    struct serve_state *state = STATE(ctx);
    if (state->body_err || !ctx->req.chunked)
        return -1;
    if (state->body_done)
        return 0;
    if (!body_begin(ctx))
        return -1;
    if (state->chunk_left == 0) {
        if (!chunk_header(ctx))
            return -1;
        if (state->body_done)
            return 0;
    }
    if (state->chunk_left > cap || cap > INT_MAX) {              /* the chunk does not fit dst */
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
static void drain_body(ioxd_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    char   tmp[4096];
    size_t drained = 0;
    while (!state->body_done && !state->body_err) {
        if (drained >= IOXD_DRAIN_MAX) {
            ctx->res.close = true;
            return;
        }
        int n = ioxd_body_read_until(ctx, tmp, sizeof tmp);
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
static long read_head(ioxd_ctx *ctx)
{
    ioxd_pipereader  *pr      = READER(ctx);
    ioxd_request *req     = &ctx->req;
    size_t        already = 0;                         /* what the previous attempt scanned */
    ioxd_slice    live = { nullptr, 0 };
    for (;;) {
        int rc = ioxd_pipereader_read(pr, &live);
        if (rc == 0)
            return -1;                                /* the peer is done: a clean end between requests */
        if (rc == IOXD_PIPE_FULL) {
            send_status(WRITER(ctx), 431);
            return -1;
        }
        if (rc < 0)
            return -1;
        req->n_headers = IOXD_MAX_HEADERS;            /* in: room; out: count */
        int parsed = phr_parse_request(live.p, live.len,
                                       &req->method.p, &req->method.len,
                                       &req->target.p, &req->target.len,
                                       &req->minor_version,
                                       (struct phr_header *)req->headers, &req->n_headers,
                                       already);
        if (parsed >= 0) {
            if (!ioxd_pipereader_keep(pr, (size_t)parsed)) {   /* the head stays put, where it was parsed */
                send_status(WRITER(ctx), 431);
                return -1;
            }
            ioxd_pipereader_run_begin(pr);                 /* the body's kept bytes are a run of their own */
            return parsed;
        }
        if (parsed == -1) {                           /* malformed */
            send_status(WRITER(ctx), 400);
            return -1;
        }
        ioxd_pipereader_examine(pr, live.len);             /* incomplete: the next read waits for more */
        already = live.len;
    }
}

/* Does the slice begin with this literal, ignoring ASCII case? */
static bool starts_ci(ioxd_slice s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len >= n && eq_ci(s.p, n, lit, n);
}

/* The rest of the request from its head: path and query (an absolute-form target loses its
 * scheme and authority first), the query split into params, the headers lower-cased and the
 * ones the engine needs picked out, and the framing settled. The body stays on the wire.
 * Returns 0, or the status the request must be refused with. */
static int fill_request(ioxd_request *req, char *params_arena, size_t arena_cap)
{
    ioxd_slice target = req->target;
    if (target.len && target.p[0] != '/' && (starts_ci(target, "http://") || starts_ci(target, "https://"))) {   /* absolute-form: RFC 9112 3.2.2 */
        size_t      skip  = target.p[4] == ':' ? 7 : 8;
        const char *slash = memchr(target.p + skip, '/', target.len - skip);
        target = slash ? (ioxd_slice){ slash, (size_t)(target.p + target.len - slash) } : (ioxd_slice){ "/", 1 };
    }
    const char *qmark = memchr(target.p, '?', target.len);
    if (qmark) {
        req->path  = (ioxd_slice){ target.p, (size_t)(qmark - target.p) };
        req->query = (ioxd_slice){ qmark + 1, target.len - req->path.len - 1 };
    } else {
        req->path  = target;
        req->query = (ioxd_slice){ target.p + target.len, 0 };
    }
    bool truncated = false;
    req->n_params = req->query.len
        ? ioxd_kv_parse(req->query.p, req->query.len, req->params, IOXD_MAX_PARAMS, params_arena, arena_cap, &truncated)
        : 0;
    req->n_route_params = 0;                          /* the router fills these */
    req->body           = (ioxd_slice){ nullptr, 0 };  /* on demand: ioxd_body_all fills it */

    struct picked_headers picked = pick_headers(req);
    req->chunked         = false;
    req->content_length  = 0;
    req->keep_alive      = keep_alive_from(req->minor_version, &picked);
    req->expect_continue = picked.expect_continue;
    if (picked.refuse)
        return picked.refuse;
    if (truncated)                                    /* part of the query would be missing: never act on part */
        return req->n_params == IOXD_MAX_PARAMS ? 400 : 414;
    if (req->minor_version >= 1 ? picked.n_host != 1 : picked.n_host > 1)
        return 400;                                   /* RFC 9112 3.2: exactly one Host on HTTP/1.1 */
    if (picked.n_transfer_enc) {
        if (picked.n_content_length)
            return 400;                               /* both framings: RFC 9112 6.1 */
        if (picked.te_other)
            return 501;                               /* a transfer coding we do not implement */
        if (!picked.te_last_chunked)
            return 400;                               /* chunked, but not as the last coding */
        req->chunked = true;
    } else if (picked.n_content_length) {
        if (!parse_length(picked.content_length.p, picked.content_length.len, &req->content_length))
            return 400;
    }
    return 0;
}

/* The engine's bookkeeping for reading the body on demand: where it starts, what already
 * arrived with the head, and - for a Content-Length body that is entirely here - where the next
 * request starts. */
static void init_body_state(struct serve_state *state, struct ioxd_pipe *pipe, const ioxd_request *req)
{
    *state = (struct serve_state){ .pipe = pipe };
    state->body_done = !req->chunked && req->content_length == 0;
}

/* A response with its defaults and an empty slab. */
static void init_response(ioxd_response *res, ioxd_pipewriter *pw)
{
    res->status         = 200;
    res->content_type   = (ioxd_slice){ "text/plain", 10 };
    res->n_headers      = 0;                          /* headers[] is only read up to here */
    res->close          = false;
    res->head_sent      = false;
    res->content_length = 0;
    res->has_length     = false;
    res->chunked        = false;
    res->failed         = false;
    res->body_sent      = 0;
    res->head_len       = 0;
    ioxd_pipewriter_reset(pw);
}


/* The proactor handler for every connection: one request per iteration - get the head, run
 * the chain against a context, drain what it left of the body, send what it wrote - while kept
 * alive. Returning closes the connection. */
void ioxd__serve(struct ioxd_pipe *pipe)
{
    char params[IOXD_PARAM_CAP];                      /* decoded query parameters */

    for (;;) {
        ioxd_ctx           ctx;                       /* this request's context             */
        struct serve_state state = { .pipe = pipe };
        ctx.priv = &state;

        long head_len = read_head(&ctx);
        if (head_len < 0)
            return;
        int refused = fill_request(&ctx.req, params, sizeof params);
        init_body_state(&state, pipe, &ctx.req);
        init_response(&ctx.res, &pipe->out);
        ctx.user = nullptr;
        if (refused) {                                /* the framing cannot be trusted: answer, close */
            send_status(&pipe->out, refused);
            return;
        }
        state.head_only = ctx.req.method.len == 4 && memcmp(ctx.req.method.p, "HEAD", 4) == 0;

        ioxd__dispatch(&ctx);                         /* middleware chain + endpoint */

        if (state.body_err) {                         /* too large, malformed, or gone */
            if (state.body_err > 0 && !ctx.res.head_sent)
                send_status(&pipe->out, state.body_err);
            return;
        }
        if (ctx.req.expect_continue && !state.continue_sent && !state.body_done)
            ctx.res.close = true;                     /* never asked for: the client may not send it, so no drain */
        else
            drain_body(&ctx);                         /* what the handler left unread */
        if (state.body_err)
            return;
        if (finish(&ctx) < 0)                         /* sends; suspends meanwhile */
            return;
        if (!ctx.req.keep_alive || ctx.res.close)
            return;
        ioxd_pipereader_release(&pipe->in);           /* this request's bytes go; a pipelined next one stays */
    }
}

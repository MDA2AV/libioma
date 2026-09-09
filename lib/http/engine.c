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
#define IOXD_PARAM_CAP    2048
#endif
#ifndef IOXD_HEAD_CAP
#define IOXD_HEAD_CAP     4096
#endif
#ifndef IOXD_DRAIN_MAX
#define IOXD_DRAIN_MAX    (1024UL * 1024)
#endif
#ifndef IOXD_TRAILER_MAX
#define IOXD_TRAILER_MAX  4096
#endif

static_assert(sizeof(ioxd_kv) == sizeof(struct phr_header), "ioxd_kv must mirror phr_header");
static_assert(offsetof(ioxd_kv, key)    == offsetof(struct phr_header, name) &&
               offsetof(ioxd_slice, len) == offsetof(struct phr_header, name_len) &&
               offsetof(ioxd_kv, value)  == offsetof(struct phr_header, value),
               "ioxd_kv must mirror phr_header");

struct serve_state {
    struct ioxd_pipe *pipe;
    size_t       body_read;
    bool         body_done;
    bool         body_whole;
    int          body_err;
    size_t       chunk_left;
    bool         head_only;
    bool         no_body;
    bool         continue_sent;
};
#define STATE(ctx)  ((struct serve_state *)(ctx)->priv)
#define READER(ctx) (&STATE(ctx)->pipe->in)
#define WRITER(ctx) (&STATE(ctx)->pipe->out)

static inline unsigned char lower_ascii(unsigned char a)
{
    return (unsigned)(a - 'A') < 26U ? (unsigned char)(a | 0x20U) : a;
}

static bool eq_ci(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn)
        return false;
    for (size_t i = 0; i < an; i++)
        if (lower_ascii((unsigned char)a[i]) != lower_ascii((unsigned char)b[i]))
            return false;
    return true;
}

static bool parse_length(const char *s, size_t n, size_t *out)
{
    if (n == 0)
        return false;
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = (unsigned char)s[i] - (unsigned)'0';
        if (d > 9 || v > (SIZE_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

static bool next_token(const char *value, size_t len, size_t *at, ioxd_slice *tok)
{
    while (*at < len && (value[*at] == ' ' || value[*at] == ',' || value[*at] == '\t')) (*at)++;
    if (*at >= len)
        return false;
    size_t start = *at;
    while (*at < len && value[*at] != ',') (*at)++;
    size_t end = *at;
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
    *tok = (ioxd_slice){ value + start, end - start };
    return true;
}

static bool token_present_ci(const char *value, size_t len, const char *tok)
{
    size_t     tok_len = strlen(tok), at = 0;
    ioxd_slice t;
    while (next_token(value, len, &at, &t))
        if (eq_ci(t.p, t.len, tok, tok_len))
            return true;
    return false;
}

struct picked_headers {
    ioxd_slice content_length;
    unsigned   n_content_length, n_transfer_enc, n_host;
    bool       te_last_chunked;
    bool       te_other;
    bool       close, keep_alive;
    bool       expect_continue;
    int        refuse;
};

static inline void lower_inplace(char *s, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        uint64_t upper = ((w + 0x3f3f3f3f3f3f3f3fULL) & ~(w + 0x2525252525252525ULL)) & 0x8080808080808080ULL;
        w |= upper >> 2;
        memcpy(s + i, &w, 8);
    }
    for (; i < n; i++)
        if ((unsigned)(s[i] - 'A') < 26U) s[i] += 'a' - 'A';
}

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
        picked->refuse = 400;
}

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
                    picked.refuse = 400;
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

static bool keep_alive_from(int minor_version, const struct picked_headers *picked)
{
    if (picked->close)
        return false;
    return minor_version >= 1 || picked->keep_alive;
}

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

struct cslice { const char *p; int len; };
#define CSLICE(lit) (struct cslice){ (lit), (int)(sizeof(lit) - 1) }

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

static void send_status(ioxd_pipewriter *pw, int code)
{
    char head[128];
    int  len = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\ncontent-length: 0\r\nconnection: close\r\n\r\n",
                        code, ioxd_reason(code));
    if (len < 0)
        return;
    if ((size_t)len >= sizeof head)
        len = (int)sizeof head - 1;
    ioxd__pipewriter_reset(pw);
    ioxd__pipewriter_send(pw, head, (size_t)len);
}

enum framing {
    FRAME_LENGTH,
    FRAME_CHUNKED,
    FRAME_UNTIL_CLOSE,
    FRAME_NONE,
};

static int wire_status(int status)
{
    return status >= 100 && status <= 999 ? status : 500;
}

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

    bool keep = ctx->req.keep_alive && !res->close;
    if (!keep)
        PUTC("connection: close\r\n");
    else if (ctx->req.minor_version == 0)
        PUTC("connection: keep-alive\r\n");

    PUT(res->head, res->head_len);

    PUTC("\r\n");
#undef PUTC
#undef PUT
#undef NEED
    return (int)(p - dst);
}

static void put_terminator(char *at)
{
    at[0] = '0';
    at[1] = '\r';
    at[2] = '\n';
    at[3] = '\r';
    at[4] = '\n';
}

static int fail(ioxd_response *res)
{
    res->failed = true;
    return -1;
}

static int flush(ioxd_ctx *ctx, bool final)
{
    ioxd_response      *res   = &ctx->res;
    ioxd_pipewriter    *pw    = WRITER(ctx);
    struct serve_state *state = STATE(ctx);
    if (res->failed)
        return -1;
    char head[IOXD_HEAD_CAP];
    int  head_len = 0;
    if (!res->head_sent) {
        int          status   = wire_status(res->status);
        bool         bodyless = status < 200 || status == 204 || status == 304;
        enum framing framing  = FRAME_LENGTH;
        state->no_body = state->head_only || bodyless;

        bool   declared = res->has_length && (!final || state->no_body);
        size_t body_len = declared ? res->content_length : pw->len;
        if (res->has_length && !declared)
            res->content_length = pw->len;
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
        if (!state->body_done && !state->body_err) {
            size_t left = ctx->req.chunked ? SIZE_MAX : ctx->req.content_length - state->body_read;
            if (left > IOXD_DRAIN_MAX || (ctx->req.expect_continue && !state->continue_sent))
                res->close = true;
        }
        head_len = build_head(ctx, head, sizeof head, framing, body_len);
        if (head_len < 0) {
            send_status(pw, 500);
            return fail(res);
        }
        res->head_sent = true;
    }
    if (state->no_body) {
        ioxd__pipewriter_reset(pw);
        if (head_len && ioxd__pipewriter_through(pw, head, (size_t)head_len) < 0)
            return fail(res);
        return 0;
    }
    bool over = false;
    if (res->has_length) {
        size_t left = res->content_length - res->body_sent;
        if (pw->len > left) {
            pw->len = left;
            over    = true;
        }
    }
    res->body_sent += pw->len;
    if (res->chunked && pw->len) {
        char  size_line[16];
        int   digits = put_hex(size_line, pw->len);
        char *front  = ioxd__pipewriter_front(pw, (size_t)digits + 2);
        char *back   = ioxd__pipewriter_back(pw, 2);
        if (!front || !back)
            return fail(res);
        memcpy(front, size_line, (size_t)digits);
        front[digits]     = '\r';
        front[digits + 1] = '\n';
        back[0] = '\r';
        back[1] = '\n';
    }
    if (final && res->chunked) {
        char *back = ioxd__pipewriter_back(pw, 5);
        if (!back)
            return fail(res);
        put_terminator(back);
    }
    if (head_len) {
        char *front = ioxd__pipewriter_front(pw, (size_t)head_len);
        if (front)
            memcpy(front, head, (size_t)head_len);
        else if (ioxd__pipewriter_through(pw, head, (size_t)head_len) < 0)
            return fail(res);
    }
    if (ioxd__pipewriter_flush(pw) < 0)
        return fail(res);
    if (over)
        return fail(res);
    return 0;
}

static int finish(ioxd_ctx *ctx)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed)
        return -1;
    if (!res->head_sent || pw->len) {
        if (flush(ctx, true) < 0)
            return -1;
    } else if (res->chunked) {
        char *back = ioxd__pipewriter_back(pw, 5);
        if (!back)
            return fail(res);
        put_terminator(back);
        if (ioxd__pipewriter_flush(pw) < 0)
            return fail(res);
    }
    if (res->has_length && !STATE(ctx)->no_body && res->body_sent != res->content_length)
        res->close = true;
    return 0;
}

int ioxd_write(ioxd_ctx *ctx, const void *data, size_t len)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed)
        return -1;
    const char *src = data;
    while (len) {
        size_t room = ioxd__pipewriter_room(pw);
        if (room == 0) {
            if (flush(ctx, false) < 0)
                return -1;
            continue;
        }
        size_t n = len < room ? len : room;
        memcpy(ioxd__pipewriter_at(pw), src, n);
        ioxd__pipewriter_advance(pw, n);
        src += n;
        len -= n;
    }
    return 0;
}

static int write_formatted_heap(ioxd_ctx *ctx, const char *fmt, va_list ap, size_t len)
{
    char *tmp = malloc(len + 1);
    if (!tmp)
        return fail(&ctx->res);
    vsnprintf(tmp, len + 1, fmt, ap);
    int rc = ioxd_write(ctx, tmp, len);
    free(tmp);
    return rc;
}

int ioxd_printf(ioxd_ctx *ctx, const char *fmt, ...)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed)
        return -1;
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    size_t room = ioxd__pipewriter_room(pw);
    int    n    = vsnprintf(ioxd__pipewriter_at(pw), room, fmt, ap);
    va_end(ap);

    int rc = -1;
    if (n < 0) {
    } else if ((size_t)n < room) {
        ioxd__pipewriter_advance(pw, (size_t)n);
        rc = 0;
    } else if ((size_t)n >= pw->cap) {
        rc = write_formatted_heap(ctx, fmt, again, (size_t)n);
    } else if (flush(ctx, false) == 0) {
        ioxd__pipewriter_advance(pw, (size_t)vsnprintf(ioxd__pipewriter_at(pw), pw->cap, fmt, again));
        rc = 0;
    }
    va_end(again);
    return rc;
}

int ioxd_flush(ioxd_ctx *ctx)
{
    return flush(ctx, false);
}

void *ioxd_reserve(ioxd_ctx *ctx, size_t n)
{
    ioxd_response   *res = &ctx->res;
    ioxd_pipewriter *pw  = WRITER(ctx);
    if (res->failed || n > pw->cap)
        return nullptr;
    if (n > ioxd__pipewriter_room(pw) && flush(ctx, false) < 0)
        return nullptr;
    return ioxd__pipewriter_at(pw);
}

void ioxd_advance(ioxd_ctx *ctx, size_t n)
{
    ioxd__pipewriter_advance(WRITER(ctx), n);
}

static void body_fail(ioxd_ctx *ctx, int status)
{
    STATE(ctx)->body_err = status;
    ctx->res.status      = status;
}

static bool body_begin(ioxd_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    if (!ctx->req.expect_continue || state->continue_sent || ctx->res.head_sent)
        return true;
    state->continue_sent = true;
    if (ioxd__pipewriter_through(WRITER(ctx), "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) {
        state->body_err = -1;
        return false;
    }
    return true;
}

static bool body_bytes(ioxd_ctx *ctx, ioxd_slice *live)
{
    int rc = ioxd__pipereader_read(READER(ctx), live);
    if (rc > 0)
        return true;
    if (rc == IOXD_PIPE_FULL)
        body_fail(ctx, 413);
    else
        STATE(ctx)->body_err = -1;
    return false;
}

static long body_line(ioxd_ctx *ctx, ioxd_slice *live)
{
    for (;;) {
        if (!body_bytes(ctx, live))
            return -1;
        const char *eol = memmem(live->p, live->len, "\r\n", 2);
        if (eol)
            return eol - live->p;
        ioxd__pipereader_examine(READER(ctx), live->len);
    }
}

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
        ioxd__pipereader_drop(READER(ctx), (size_t)len + 2);
        if (len == 0)
            break;
    }
    STATE(ctx)->body_done = true;
    return true;
}

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
        int digit = ioxd__http_hexval((unsigned char)live.p[i]);
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
    ioxd__pipereader_drop(READER(ctx), (size_t)len + 2);
    if (size == 0)
        return chunk_trailers(ctx);
    STATE(ctx)->chunk_left = size;
    return true;
}

static bool chunk_end(ioxd_ctx *ctx)
{
    ioxd_slice live = { nullptr, 0 };
    for (;;) {
        if (!body_bytes(ctx, &live))
            return false;
        if (live.len >= 2)
            break;
        ioxd__pipereader_examine(READER(ctx), live.len);
    }
    if (live.p[0] != '\r' || live.p[1] != '\n') {
        body_fail(ctx, 400);
        return false;
    }
    ioxd__pipereader_drop(READER(ctx), 2);
    return true;
}

ioxd_slice ioxd_body_all(ioxd_ctx *ctx)
{
    struct serve_state *state = STATE(ctx);
    ioxd_pipereader        *pr    = &state->pipe->in;
    ioxd_request       *req   = &ctx->req;
    const ioxd_slice    none  = { nullptr, 0 };

    if (state->body_whole)
        return req->body;
    if (state->body_read || state->body_err)
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
            if (!ioxd__pipereader_keep(pr, n)) {
                body_fail(ctx, 413);
                return none;
            }
            state->chunk_left -= n;
            state->body_read  += n;
            if (state->chunk_left == 0 && !chunk_end(ctx))
                return none;
        }
        req->body = ioxd__pipereader_run(pr);
    } else {
        if (req->content_length > pr->cap) {
            body_fail(ctx, 413);
            return none;
        }
        ioxd_slice live = none;
        while (req->content_length && live.len < req->content_length) {
            if (live.len)
                ioxd__pipereader_examine(pr, live.len);
            if (!body_bytes(ctx, &live))
                return none;
        }
        const char *kept = req->content_length ? ioxd__pipereader_keep(pr, req->content_length) : live.p;
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

static int fixed_data(ioxd_ctx *ctx, struct serve_state *state, char *dst, size_t n)
{
    size_t remaining = ctx->req.content_length - state->body_read;
    if (n > remaining)
        n = remaining;
    int got = ioxd__pipereader_copy(&state->pipe->in, dst, n);
    if (got <= 0) {
        state->body_err = -1;
        return -1;
    }
    state->body_read += (size_t)got;
    if (state->body_read == ctx->req.content_length)
        state->body_done = true;
    return got;
}

static int chunk_data(ioxd_ctx *ctx, struct serve_state *state, char *dst, size_t n)
{
    if (n > state->chunk_left)
        n = state->chunk_left;
    int got = ioxd__pipereader_copy(&state->pipe->in, dst, n);
    if (got <= 0) {
        state->body_err = -1;
        return -1;
    }
    state->chunk_left -= (size_t)got;
    state->body_read  += (size_t)got;
    if (state->chunk_left == 0 && !chunk_end(ctx))
        return got;
    return got;
}

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
        n = INT_MAX;
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
    if (state->chunk_left > cap || cap > INT_MAX) {
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

static long read_head(ioxd_ctx *ctx)
{
    ioxd_pipereader  *pr      = READER(ctx);
    ioxd_request *req     = &ctx->req;
    size_t        already = 0;
    ioxd_slice    live = { nullptr, 0 };
    for (;;) {
        int rc = ioxd__pipereader_read(pr, &live);
        if (rc == 0)
            return -1;
        if (rc == IOXD_PIPE_FULL) {
            send_status(WRITER(ctx), 431);
            return -1;
        }
        if (rc < 0)
            return -1;
        req->n_headers = IOXD_MAX_HEADERS;
        int parsed = phr_parse_request(live.p, live.len,
                                       &req->method.p, &req->method.len,
                                       &req->target.p, &req->target.len,
                                       &req->minor_version,
                                       (struct phr_header *)req->headers, &req->n_headers,
                                       already);
        if (parsed >= 0) {
            if (!ioxd__pipereader_keep(pr, (size_t)parsed)) {
                send_status(WRITER(ctx), 431);
                return -1;
            }
            ioxd__pipereader_run_begin(pr);
            return parsed;
        }
        if (parsed == -1) {
            send_status(WRITER(ctx), 400);
            return -1;
        }
        ioxd__pipereader_examine(pr, live.len);
        already = live.len;
    }
}

static bool starts_ci(ioxd_slice s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len >= n && eq_ci(s.p, n, lit, n);
}

static int fill_request(ioxd_request *req, char *params_arena, size_t arena_cap)
{
    ioxd_slice target = req->target;
    if (target.len && target.p[0] != '/' && (starts_ci(target, "http://") || starts_ci(target, "https://"))) {
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
    req->n_route_params = 0;
    req->body           = (ioxd_slice){ nullptr, 0 };

    struct picked_headers picked = pick_headers(req);
    req->chunked         = false;
    req->content_length  = 0;
    req->keep_alive      = keep_alive_from(req->minor_version, &picked);
    req->expect_continue = picked.expect_continue;
    if (picked.refuse)
        return picked.refuse;
    if (truncated)
        return req->n_params == IOXD_MAX_PARAMS ? 400 : 414;
    if (req->minor_version >= 1 ? picked.n_host != 1 : picked.n_host > 1)
        return 400;
    if (picked.n_transfer_enc) {
        if (picked.n_content_length)
            return 400;
        if (picked.te_other)
            return 501;
        if (!picked.te_last_chunked)
            return 400;
        req->chunked = true;
    } else if (picked.n_content_length) {
        if (!parse_length(picked.content_length.p, picked.content_length.len, &req->content_length))
            return 400;
    }
    return 0;
}

static void init_body_state(struct serve_state *state, struct ioxd_pipe *pipe, const ioxd_request *req)
{
    *state = (struct serve_state){ .pipe = pipe };
    state->body_done = !req->chunked && req->content_length == 0;
}

static void init_response(ioxd_response *res, ioxd_pipewriter *pw)
{
    res->status         = 200;
    res->content_type   = (ioxd_slice){ "text/plain", 10 };
    res->n_headers      = 0;
    res->close          = false;
    res->head_sent      = false;
    res->content_length = 0;
    res->has_length     = false;
    res->chunked        = false;
    res->failed         = false;
    res->body_sent      = 0;
    res->head_len       = 0;
    ioxd__pipewriter_reset(pw);
}

void ioxd__engine_serve(struct ioxd_pipe *pipe)
{
    char params[IOXD_PARAM_CAP];

    for (;;) {
        ioxd_ctx           ctx;
        struct serve_state state = { .pipe = pipe };
        ctx.priv = &state;

        long head_len = read_head(&ctx);
        if (head_len < 0)
            return;
        int refused = fill_request(&ctx.req, params, sizeof params);
        init_body_state(&state, pipe, &ctx.req);
        init_response(&ctx.res, &pipe->out);
        ctx.user = nullptr;
        if (refused) {
            send_status(&pipe->out, refused);
            return;
        }
        state.head_only = ctx.req.method.len == 4 && memcmp(ctx.req.method.p, "HEAD", 4) == 0;

        ioxd__router_dispatch(&ctx);

        if (state.body_err) {
            if (state.body_err > 0 && !ctx.res.head_sent)
                send_status(&pipe->out, state.body_err);
            return;
        }
        if (ctx.req.expect_continue && !state.continue_sent && !state.body_done)
            ctx.res.close = true;
        else
            drain_body(&ctx);
        if (state.body_err)
            return;
        if (finish(&ctx) < 0)
            return;
        if (!ctx.req.keep_alive || ctx.res.close)
            return;
        ioxd__pipereader_release(&pipe->in);
    }
}

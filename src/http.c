/*
 * http.c - the HTTP/1.1 engine: parse a request with picohttpparser, read its body, dispatch it
 * to the routed endpoint, serialize the response, flush it. All of it runs on the connection's
 * coroutine, so await_recv and await_send simply suspend it and the loop resumes it.
 */
#define _GNU_SOURCE
#include "internal.h"
#include "picohttpparser.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef IOMA_REQ_CAP
#define IOMA_REQ_CAP     16384      /* request line + headers + body must fit here; else 413/431 */
#endif
#ifndef IOMA_SCRATCH_CAP
#define IOMA_SCRATCH_CAP 4096       /* per-request arena for ioma_textf                          */
#endif
#ifndef IOMA_HEAD_CAP
#define IOMA_HEAD_CAP    4096       /* serialized status line + headers (+ a small inlined body) */
#endif
#ifndef IOMA_PARAM_CAP
#define IOMA_PARAM_CAP   2048       /* per-request arena for percent-decoded query parameters    */
#endif

/* serve() hands req.headers to picohttpparser as its header array: a kv (two slices) must lay
 * out exactly like a phr_header (name, name_len, value, value_len). */
_Static_assert(sizeof(ioma_kv) == sizeof(struct phr_header), "ioma_kv must mirror phr_header");
_Static_assert(offsetof(ioma_kv, key)   == offsetof(struct phr_header, name) &&
               offsetof(ioma_slice, len) == offsetof(struct phr_header, name_len) &&
               offsetof(ioma_kv, value) == offsetof(struct phr_header, value),
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

/* The three headers the engine itself needs; NULL when absent. */
struct hdrs {
    ioma_slice content_length, transfer_enc, connection;   /* p == NULL when absent */
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

/* ── response writing (suspends on the send) ───────────────────────────────────────────── */

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

/* Serialize the head (and inline a small body) by memcpy of precomposed pieces plus put_uint -
 * no snprintf - then one send when the body fits, else head then body. 0, or -1 after a 500. */
static int write_response(conn_t *c, ioma_request *req, ioma_response *res)
{
    char  head[IOMA_HEAD_CAP];
    char *p   = head;
    char *end = head + sizeof head;

#define NEED(n)     do { if ((size_t)(end - p) < (size_t)(n)) { send_status(c, 500); return -1; } } while (0)
#define PUT(src, n) do { NEED(n); memcpy(p, (src), (size_t)(n)); p += (n); } while (0)
#define PUTC(lit)   PUT((lit), sizeof(lit) - 1)

    struct cslice sl = status_line(res->status);
    if (sl.p) {
        PUT(sl.p, sl.len);
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
    const char *ct = res->content_type ? res->content_type : "text/plain";
    PUT(ct, strlen(ct));
    PUTC("\r\n");

    PUTC("Content-Length: ");
    NEED(20);
    p += put_uint(p, res->body_len);
    PUTC("\r\n");

    /* Connection: only when it says something. HTTP/1.1 is persistent by default, so a kept-alive
     * 1.1 reply carries none; a 1.0 client that asked for keep-alive is told it got it; a closing
     * reply always says close. */
    bool ka = req->keep_alive && !res->close;
    if (!ka)
        PUTC("Connection: close\r\n");
    else if (req->minor_version == 0)
        PUTC("Connection: keep-alive\r\n");

    for (int i = 0; i < res->n_extra; i++) {
        PUT(res->extra[i].key.p, res->extra[i].key.len);
        PUTC(": ");
        PUT(res->extra[i].value.p, res->extra[i].value.len);
        PUTC("\r\n");
    }

    PUTC("\r\n");
#undef PUTC
#undef PUT
#undef NEED

    size_t hl = (size_t)(p - head);

    if (res->body_len && hl + res->body_len <= sizeof head) {   /* the common small reply */
        memcpy(head + hl, res->body, res->body_len);
        return await_send(c, head, hl + res->body_len) < 0 ? -1 : 0;
    }
    if (await_send(c, head, hl) < 0) return -1;
    if (res->body_len && await_send(c, res->body, res->body_len) < 0) return -1;
    return 0;
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

/* The proactor handler for every connection: parse, read the body, dispatch, reply; repeat while
 * kept alive. Returning closes the connection. */
void ioma__serve(conn_t *c)
{
    char   buf[IOMA_REQ_CAP];
    char   scratch[IOMA_SCRATCH_CAP];
    char   params[IOMA_PARAM_CAP];                    /* decoded query parameters land here */
    size_t have = 0, last_len = 0;

    for (;;) {
        ioma_request req;
        size_t nphr = IOMA_MAX_HEADERS;
        const char *method = NULL, *target = NULL;
        size_t ml = 0, tl = 0;
        int minor = 0;

        /* Parse straight into req.headers. With nothing buffered - the usual state right after a
         * reply - skip the parse and just read. */
        int pret = have ? phr_parse_request(buf, have, &method, &ml, &target, &tl, &minor,
                                            (struct phr_header *)req.headers, &nphr, last_len)
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

        req.method        = (ioma_slice){ method, ml };
        req.target        = (ioma_slice){ target, tl };
        req.minor_version = minor;
        req.n_headers     = nphr;

        const char *q = memchr(target, '?', tl);
        if (q) {
            req.path  = (ioma_slice){ target, (size_t)(q - target) };
            req.query = (ioma_slice){ q + 1, tl - req.path.len - 1 };
        } else {
            req.path  = (ioma_slice){ target, tl };
            req.query = (ioma_slice){ target + tl, 0 };
        }
        req.n_params = req.query.len
            ? ioma_kv_parse(req.query.p, req.query.len, req.params, IOMA_MAX_PARAMS, params, sizeof params)
            : 0;
        req.n_route = 0;                                 /* the router fills these */

        struct hdrs h = pick_headers(&req);

        /* Body. leftover_* is what belongs to the next request on a kept-alive connection. */
        size_t leftover_off = 0, leftover_len = 0;
        if (h.transfer_enc.p && token_present_ci(h.transfer_enc.p, h.transfer_enc.len, "chunked")) {
            long n = read_chunked_body(c, buf, header_len, have);
            if (n < 0) return;
            req.body = (ioma_slice){ buf + header_len, (size_t)n };
            /* bytes pipelined after a chunked body are not carried; the next request reads fresh */
        } else {
            size_t content_length = h.content_length.p ? parse_size(h.content_length.p, h.content_length.len) : 0;
            size_t total = header_len + content_length;
            if (read_fixed_body(c, buf, total, &have) < 0) return;
            req.body = (ioma_slice){ buf + header_len, content_length };
            leftover_off = total;
            leftover_len = have - total;
        }

        req.keep_alive  = keep_alive_from(minor, h.connection);
        req.conn        = c;
        req.scratch     = scratch;
        req.scratch_cap = IOMA_SCRATCH_CAP;

        ioma_response res = ioma__dispatch(&req);        /* middleware chain + endpoint */
        if (write_response(c, &req, &res) < 0) return;  /* suspends on the send        */

        if (!req.keep_alive || res.close) return;

        if (leftover_len) memmove(buf, buf + leftover_off, leftover_len);   /* pipelining */
        have = leftover_len;
        last_len = 0;
    }
}

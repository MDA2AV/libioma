/*
 * http.c - the HTTP/1.1 engine: parse a request with picohttpparser, dispatch it to a routed
 * endpoint, serialize the returned response, and flush it. All of this runs on the connection's
 * coroutine (proactor handler), so await_recv and await_send suspend it and the loop resumes it.
 *
 * v1 scope: request line + headers + a Content-Length body that fits the fixed read buffer.
 * Chunked request bodies answer 501 for now (swap the parse layer for llhttp when you need them).
 */
#define _GNU_SOURCE
#include "http.h"
#include "proactor.h"
#include "picohttpparser.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifndef IOMA_REQ_CAP
#define IOMA_REQ_CAP   16384        /* request line + headers + body must fit here; else 413/431 */
#endif
#ifndef IOMA_SCRATCH_CAP
#define IOMA_SCRATCH_CAP 4096       /* per-request arena for ioma_textf                          */
#endif
#ifndef IOMA_HEAD_CAP
#define IOMA_HEAD_CAP  4096         /* serialized status line + headers (+ small inlined body)   */
#endif

ioma_response ioma__dispatch(ioma_request *req);   /* router.c */

/* serve() hands req.headers to picohttpparser as its header array: the two structs must match. */
_Static_assert(sizeof(ioma_header) == sizeof(struct phr_header), "ioma_header must mirror phr_header");
_Static_assert(offsetof(ioma_header, name)      == offsetof(struct phr_header, name) &&
               offsetof(ioma_header, name_len)  == offsetof(struct phr_header, name_len) &&
               offsetof(ioma_header, value)     == offsetof(struct phr_header, value) &&
               offsetof(ioma_header, value_len) == offsetof(struct phr_header, value_len),
               "ioma_header must mirror phr_header");

/* ── small parsers ─────────────────────────────────────────────────────────────────────── */

static size_t parse_size(const char *s, size_t n)
{
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (size_t)(s[i] - '0');
    }
    return v;
}

/* Case-insensitive equality of two ASCII slices, folding only A-Z. No libc call and no locale:
 * this runs several times per request (header lookups, Connection tokens), so it stays a length
 * test plus a tight byte loop. */
static inline unsigned char lower_ascii(unsigned char a)
{
    return (unsigned)(a - 'A') < 26u ? (unsigned char)(a | 0x20) : a;
}

static inline bool eq_ci(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++)
        if (lower_ascii((unsigned char)a[i]) != lower_ascii((unsigned char)b[i])) return false;
    return true;
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

/* ── public helpers ────────────────────────────────────────────────────────────────────── */

bool ioma_slice_eq(const char *s, size_t n, const char *cstr)
{
    return strlen(cstr) == n && memcmp(s, cstr, n) == 0;
}

const char *ioma_header_get(const ioma_request *req, const char *name, size_t *value_len)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i < req->n_headers; i++) {
        if (eq_ci(req->headers[i].name, req->headers[i].name_len, name, nl)) {
            if (value_len) *value_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    if (value_len) *value_len = 0;
    return NULL;
}

ioma_response ioma_bytes(int status, const char *content_type, const void *body, size_t body_len)
{
    /* Set the fields; don't zero the struct. extra[] is the bulk of it and is only ever read up
     * to n_extra, so a ~500-byte memset per response would be pure waste on the hot path. */
    ioma_response r;
    r.status       = status;
    r.content_type = content_type;
    r.body         = body;
    r.body_len     = body_len;
    r.n_extra      = 0;
    r.close        = false;
    return r;
}

ioma_response ioma_text(int status, const char *s)
{
    return ioma_bytes(status, "text/plain", s, strlen(s));
}

ioma_response ioma_json(int status, const char *s)
{
    return ioma_bytes(status, "application/json", s, strlen(s));
}

ioma_response ioma_textf(ioma_request *req, int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(req->scratch, req->scratch_cap, fmt, ap);
    va_end(ap);
    size_t len = 0;
    if (n > 0) len = (size_t)n < req->scratch_cap ? (size_t)n : (req->scratch_cap ? req->scratch_cap - 1 : 0);
    return ioma_bytes(status, "text/plain", req->scratch, len);
}

void ioma_header_set(ioma_response *res, const char *name, const char *value)
{
    if (res->n_extra >= IOMA_MAX_RESP_HEADERS) return;
    ioma_header *h = &res->extra[res->n_extra++];
    h->name = name;   h->name_len = strlen(name);
    h->value = value; h->value_len = strlen(value);
}

const char *ioma_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

/* ── response writing (suspends on the send) ───────────────────────────────────────────── */

/* From the version and the Connection header value the serve loop already picked out. */
static bool keep_alive_from(int minor_version, const char *cv, size_t vl)
{
    bool ka = minor_version >= 1;                      /* HTTP/1.1 defaults keep-alive */
    if (cv) {
        if      (token_present_ci(cv, vl, "close"))      ka = false;
        else if (token_present_ci(cv, vl, "keep-alive")) ka = true;
    }
    return ka;
}

/* Write `v` in decimal at `dst`, return the digit count. No format-string parsing, no division
 * library call the way printf makes - just a digit loop, which the compiler turns into a few
 * multiplies. This is the whole reason the response path can drop snprintf. */
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

/* A constant slice: pointer + length, so a precomposed line is a single memcpy. */
struct cslice { const char *p; int len; };
#define CSLICE(lit) (struct cslice){ (lit), (int)(sizeof(lit) - 1) }

/* Precomposed status line for the common codes: one memcpy, zero formatting. NULL means "build it"
 * (rare codes fall through to the general path in the callers). */
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

/* Serialize the response head (and inline a small body) with memcpy of precomposed pieces plus the
 * integer writer above - no snprintf on the hot path - then one send when the body fits. */
static int write_response(conn_t *c, ioma_request *req, ioma_response *res)
{
    char  head[IOMA_HEAD_CAP];
    char *p   = head;
    char *end = head + sizeof head;

    /* Refuse (500) rather than overrun if a response's headers are pathologically large. */
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
     * 1.1 reply carries no header (24 bytes fewer on every response); a 1.0 client that asked for
     * keep-alive is told it got it; a closing reply always says close. */
    bool ka = req->keep_alive && !res->close;
    if (!ka)
        PUTC("Connection: close\r\n");
    else if (req->minor_version == 0)
        PUTC("Connection: keep-alive\r\n");

    for (int i = 0; i < res->n_extra; i++) {
        PUT(res->extra[i].name, res->extra[i].name_len);
        PUTC(": ");
        PUT(res->extra[i].value, res->extra[i].value_len);
        PUTC("\r\n");
    }

    PUTC("\r\n");
#undef PUTC
#undef PUT
#undef NEED

    size_t hl = (size_t)(p - head);

    /* One send when the body fits right after the head (the common small-response case). */
    if (res->body_len && hl + res->body_len <= sizeof head) {
        memcpy(head + hl, res->body, res->body_len);
        return await_send(c, head, hl + res->body_len) < 0 ? -1 : 0;
    }
    if (await_send(c, head, hl) < 0) return -1;
    if (res->body_len && await_send(c, res->body, res->body_len) < 0) return -1;
    return 0;
}

/* ── the connection serve loop (the proactor handler) ──────────────────────────────────── */

static void serve(conn_t *c)
{
    char buf[IOMA_REQ_CAP];
    char scratch[IOMA_SCRATCH_CAP];
    size_t have = 0, last_len = 0;

    for (;;) {
        ioma_request req;
        size_t nphr = IOMA_MAX_HEADERS;
        const char *method = NULL, *target = NULL;
        size_t ml = 0, tl = 0;
        int minor = 0;

        /* picohttpparser writes the headers straight into req.headers - same four fields in the
         * same order, checked at compile time - so there is no copy loop and no second array.
         * With nothing buffered (the usual state right after a reply) skip the parse and read. */
        int pret = have ? phr_parse_request(buf, have, &method, &ml, &target, &tl, &minor,
                                            (struct phr_header *)req.headers, &nphr, last_len)
                        : -2;

        if (pret == -2) {                                  /* headers not complete yet */
            if (have == IOMA_REQ_CAP) {
                send_status(c, 431);
                return;
            }
            last_len = have;
            int n = await_recv(c, buf + have, IOMA_REQ_CAP - have);
            if (n <= 0) return;                            /* peer closed or error */
            have += (size_t)n;
            continue;
        }
        if (pret < 0) {                                    /* malformed */
            send_status(c, 400);
            return;
        }

        size_t header_len = (size_t)pret;

        req.method = method;  req.method_len = ml;
        req.target = target;  req.target_len = tl;
        req.minor_version = minor;
        req.n_headers = nphr;

        const char *q = memchr(target, '?', tl);
        if (q) {
            req.path = target;     req.path_len = (size_t)(q - target);
            req.query = q + 1;     req.query_len = tl - req.path_len - 1;
        } else {
            req.path = target;     req.path_len = tl;
            req.query = NULL;      req.query_len = 0;
        }

        /* One pass picks out the three headers the framework itself needs. The switch on the
         * name length rejects nearly every header before a single byte is compared. */
        const char *te = NULL, *cl = NULL, *cv = NULL;
        size_t tel = 0, cll = 0, cvl = 0;
        for (size_t i = 0; i < nphr; i++) {
            const ioma_header *h = &req.headers[i];
            switch (h->name_len) {
            case 14:
                if (eq_ci(h->name, 14, "content-length", 14))    { cl = h->value; cll = h->value_len; }
                break;
            case 17:
                if (eq_ci(h->name, 17, "transfer-encoding", 17)) { te = h->value; tel = h->value_len; }
                break;
            case 10:
                if (eq_ci(h->name, 10, "connection", 10))        { cv = h->value; cvl = h->value_len; }
                break;
            default:
                break;
            }
        }

        /* Body: chunked (decoded in place) or Content-Length. leftover_* is what to carry to the
         * next request on a kept-alive connection (pipelining). */
        size_t leftover_off = 0, leftover_len = 0;

        if (te && token_present_ci(te, tel, "chunked")) {
            /* Decode the chunked body in place at buf+header_len, reading more as needed. phr keeps
             * state across calls and asks for more with -2, so this survives arbitrary TCP
             * fragmentation - a split mid chunk-size hex included. */
            struct phr_chunked_decoder dec;
            memset(&dec, 0, sizeof dec);
            dec.consume_trailer = 1;

            size_t decoded = have - header_len;          /* raw bytes already here to decode */
            ssize_t pret = phr_decode_chunked(&dec, buf + header_len, &decoded);
            while (pret == -2) {
                size_t off = header_len + decoded;       /* append after the decoded prefix */
                if (off == IOMA_REQ_CAP) {
                    send_status(c, 413);
                    return;
                }
                int n = await_recv(c, buf + off, IOMA_REQ_CAP - off);
                if (n <= 0) return;
                size_t rsize = (size_t)n;
                pret = phr_decode_chunked(&dec, buf + off, &rsize);
                decoded += rsize;
            }
            if (pret < 0) {                              /* -1 malformed */
                send_status(c, 400);
                return;
            }

            req.body = buf + header_len;
            req.body_len = decoded;
            /* Bytes pipelined after a chunked body are not carried (the common clients don't do
             * it); a kept-alive connection just reads the next request fresh. */
        } else {
            size_t content_length = cl ? parse_size(cl, cll) : 0;

            size_t total = header_len + content_length;
            if (total > IOMA_REQ_CAP) {
                send_status(c, 413);
                return;
            }
            while (have < total) {
                int n = await_recv(c, buf + have, IOMA_REQ_CAP - have);
                if (n <= 0) return;
                have += (size_t)n;
            }

            req.body = buf + header_len;
            req.body_len = content_length;
            leftover_off = total;
            leftover_len = have - total;
        }

        req.keep_alive = keep_alive_from(minor, cv, cvl);
        req.conn = c;
        req.scratch = scratch;
        req.scratch_cap = IOMA_SCRATCH_CAP;

        ioma_response res = ioma__dispatch(&req);            /* middleware chain + endpoint */
        if (write_response(c, &req, &res) < 0) return;      /* suspends on the send */

        if (!req.keep_alive || res.close) return;

        /* pipelining: carry bytes that belong to the next request */
        if (leftover_len) memmove(buf, buf + leftover_off, leftover_len);
        have = leftover_len;
        last_len = 0;
    }
}

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void *worker_thread(void *arg)
{
    proactor_run(arg);
    return NULL;
}

/* CPUs this process may run on (its cpuset), so the default worker count is one per available core
 * rather than a fixed number that would oversubscribe a small cpuset (e.g. 64 threads on 8 cores). */
static int ioma_cpu_count(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0)
            return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* workers <= 0 means "one per available core". */
int ioma_run(int workers, int port)
{
    if (workers <= 0)
        workers = ioma_cpu_count();
    if (port < 1 || port > 65535) {
        fprintf(stderr, "ioma_run: 1<=port<=65535 required\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    proactor_t *ws = calloc((size_t)workers, sizeof *ws);
    pthread_t  *th = calloc((size_t)workers, sizeof *th);
    if (!ws || !th) {
        perror("calloc");
        return 1;
    }

    for (int i = 0; i < workers; i++) {
        ws[i].id      = i;
        ws[i].cpu     = i;
        ws[i].port    = (uint16_t)port;
        ws[i].handler = serve;
        ws[i].stop    = &g_stop;
        if (pthread_create(&th[i], NULL, worker_thread, &ws[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    fprintf(stderr, "ioma: %d workers on :%d\n", workers, port);

    for (int i = 0; i < workers; i++)
        pthread_join(th[i], NULL);
    free(th);
    free(ws);
    return 0;
}

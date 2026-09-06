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
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef IOMA_REQ_CAP
#define IOMA_REQ_CAP   16384        /* request line + headers + body must fit here; else 413/431 */
#endif
#ifndef IOMA_SCRATCH_CAP
#define IOMA_SCRATCH_CAP 4096       /* per-request arena for ioma_textf                          */
#endif
#ifndef IOMA_HEAD_CAP
#define IOMA_HEAD_CAP  4096         /* serialized status line + headers (+ small inlined body)   */
#endif

ioma_handler ioma__match(const ioma_request *req);   /* router.c */

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
        if (e - i == tl && strncasecmp(s + i, tok, tl) == 0) return true;
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
        if (req->headers[i].name_len == nl &&
            strncasecmp(req->headers[i].name, name, nl) == 0) {
            if (value_len) *value_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    if (value_len) *value_len = 0;
    return NULL;
}

ioma_response ioma_bytes(int status, const char *content_type, const void *body, size_t body_len)
{
    ioma_response r;
    memset(&r, 0, sizeof r);
    r.status = status;
    r.content_type = content_type;
    r.body = body;
    r.body_len = body_len;
    return r;
}

ioma_response ioma_text(int status, const char *s) { return ioma_bytes(status, "text/plain", s, strlen(s)); }
ioma_response ioma_json(int status, const char *s) { return ioma_bytes(status, "application/json", s, strlen(s)); }

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

static bool compute_keep_alive(const ioma_request *req)
{
    bool ka = req->minor_version >= 1;                 /* HTTP/1.1 defaults keep-alive */
    size_t vl;
    const char *cv = ioma_header_get(req, "connection", &vl);
    if (cv) {
        if      (token_present_ci(cv, vl, "close"))      ka = false;
        else if (token_present_ci(cv, vl, "keep-alive")) ka = true;
    }
    return ka;
}

/* A bodyless framework reply (parse errors, limits). Best effort; the caller then closes. */
static void send_status(conn_t *c, int code)
{
    char head[256];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                      code, ioma_reason(code));
    if (hl > 0) await_send(c, head, (size_t)hl);
}

static int write_response(conn_t *c, ioma_request *req, ioma_response *res)
{
    char head[IOMA_HEAD_CAP];
    int hl = 0;

#define APP(...) do {                                                                       \
        int _n = snprintf(head + hl, sizeof head - (size_t)hl, __VA_ARGS__);                \
        if (_n < 0 || (size_t)_n >= sizeof head - (size_t)hl) { send_status(c, 500); return -1; } \
        hl += _n;                                                                           \
    } while (0)

    APP("HTTP/1.1 %d %s\r\n", res->status, ioma_reason(res->status));
    APP("Content-Type: %s\r\n", res->content_type ? res->content_type : "text/plain");
    APP("Content-Length: %zu\r\n", res->body_len);
    APP("Connection: %s\r\n", (req->keep_alive && !res->close) ? "keep-alive" : "close");
    for (int i = 0; i < res->n_extra; i++)
        APP("%.*s: %.*s\r\n", (int)res->extra[i].name_len, res->extra[i].name,
                              (int)res->extra[i].value_len, res->extra[i].value);
    APP("\r\n");
#undef APP

    /* One send when the body fits right after the head (the common small-response case). */
    if (res->body_len && (size_t)hl + res->body_len <= sizeof head) {
        memcpy(head + hl, res->body, res->body_len);
        return await_send(c, head, (size_t)hl + res->body_len) < 0 ? -1 : 0;
    }
    if (await_send(c, head, (size_t)hl) < 0) return -1;
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
        struct phr_header phr[IOMA_MAX_HEADERS];
        size_t nphr = IOMA_MAX_HEADERS;
        const char *method, *target;
        size_t ml, tl;
        int minor;

        int pret = phr_parse_request(buf, have, &method, &ml, &target, &tl,
                                     &minor, phr, &nphr, last_len);

        if (pret == -2) {                                  /* headers not complete yet */
            if (have == IOMA_REQ_CAP) { send_status(c, 431); return; }
            last_len = have;
            int n = await_recv(c, buf + have, IOMA_REQ_CAP - have);
            if (n <= 0) return;                            /* peer closed or error */
            have += (size_t)n;
            continue;
        }
        if (pret < 0) { send_status(c, 400); return; }     /* malformed */

        size_t header_len = (size_t)pret;

        ioma_request req;
        memset(&req, 0, sizeof req);
        req.method = method;  req.method_len = ml;
        req.target = target;  req.target_len = tl;
        req.minor_version = minor;

        const char *q = memchr(target, '?', tl);
        if (q) {
            req.path = target;     req.path_len = (size_t)(q - target);
            req.query = q + 1;     req.query_len = tl - req.path_len - 1;
        } else {
            req.path = target;     req.path_len = tl;
        }

        for (size_t i = 0; i < nphr; i++) {
            req.headers[i].name = phr[i].name;   req.headers[i].name_len = phr[i].name_len;
            req.headers[i].value = phr[i].value; req.headers[i].value_len = phr[i].value_len;
        }
        req.n_headers = nphr;

        /* body: Content-Length only for now */
        size_t tel;
        const char *te = ioma_header_get(&req, "transfer-encoding", &tel);
        if (te && token_present_ci(te, tel, "chunked")) { send_status(c, 501); return; }

        size_t cll;
        const char *cl = ioma_header_get(&req, "content-length", &cll);
        size_t content_length = cl ? parse_size(cl, cll) : 0;

        size_t total = header_len + content_length;
        if (total > IOMA_REQ_CAP) { send_status(c, 413); return; }
        while (have < total) {
            int n = await_recv(c, buf + have, IOMA_REQ_CAP - have);
            if (n <= 0) return;
            have += (size_t)n;
        }

        req.body = buf + header_len;
        req.body_len = content_length;
        req.keep_alive = compute_keep_alive(&req);
        req.conn = c;
        req.scratch = scratch;
        req.scratch_cap = IOMA_SCRATCH_CAP;

        ioma_response res = ioma__match(&req)(&req);       /* route + run the endpoint */
        if (write_response(c, &req, &res) < 0) return;      /* suspends on the send */

        if (!req.keep_alive || res.close) return;

        /* pipelining: carry bytes that belong to the next request */
        size_t leftover = have - total;
        if (leftover) memmove(buf, buf + total, leftover);
        have = leftover;
        last_len = 0;
    }
}

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }
static void *worker_thread(void *arg) { proactor_run(arg); return NULL; }

int ioma_run(int workers, int port)
{
    if (workers < 1 || port < 1 || port > 65535) {
        fprintf(stderr, "ioma_run: workers>=1 and 1<=port<=65535 required\n");
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
    if (!ws || !th) { perror("calloc"); return 1; }

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

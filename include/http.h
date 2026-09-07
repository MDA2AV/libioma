/*
 * http.h - the ioma HTTP/1.1 layer: what you write endpoints against.
 *
 * Every request gets a context: the parsed request, the response being shaped, and a body sink.
 * The context is passed to each middleware and to the handler, and any of them may read the
 * request and shape the response. A handler writes its body into the sink; the framework puts
 * the head (status, headers) in front of it - in one send when the body fits the buffer, or
 * streamed when it does not. The handler runs on the connection's coroutine, so a write that has
 * to reach the wire simply suspends it until the send completes.
 *
 *     static void user(ioma_ctx *c) {
 *         ioma_slice id = c->req.route[0].value;              // the :id of "/users/:id"
 *         ioma_printf(c, "user %.*s\n", (int)id.len, id.p);
 *     }
 *     int main(void) {
 *         ioma_route("GET", "/users/:id", user);
 *         return ioma_run(0, 8080);                            // one worker per core
 *     }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct conn conn_t;                 /* opaque here; only advanced handlers touch it */

#ifndef IOMA_MAX_HEADERS
#define IOMA_MAX_HEADERS      64            /* request headers kept                          */
#endif
#ifndef IOMA_MAX_PARAMS
#define IOMA_MAX_PARAMS       32            /* query parameters kept                         */
#endif
#ifndef IOMA_MAX_ROUTE_PARAMS
#define IOMA_MAX_ROUTE_PARAMS 8             /* :name captures a route pattern may have       */
#endif
#ifndef IOMA_MAX_RESP_HEADERS
#define IOMA_MAX_RESP_HEADERS 16            /* headers a reply may add                       */
#endif

/* A slice: pointer + length, the C span. Not NUL-terminated. */
typedef struct { const char *p; size_t len; } ioma_slice;

/* One key/value pair of slices: a header, a query parameter, a route parameter. */
typedef struct { ioma_slice key, value; } ioma_kv;

/* ── the request ───────────────────────────────────────────────────────────────────────── */

/* A parsed request: all the data, as slices into the connection's read buffer (decoded
 * parameters into a per-request arena), valid only until the handler returns. Read the arrays
 * directly; header names are lower-cased, so compare them with lowercase literals. */
typedef struct ioma_request {
    ioma_slice  method;                         /* "GET", "POST", ...                        */
    ioma_slice  target;                         /* raw request target: path plus any query   */
    ioma_slice  path;                           /* the path, query stripped                  */
    ioma_slice  query;                          /* raw text after '?', undecoded             */
    int         minor_version;                  /* 0 or 1 for HTTP/1.0 or 1.1                */

    ioma_kv     headers[IOMA_MAX_HEADERS];      /* names lower-cased, values as received     */
    size_t      n_headers;
    ioma_kv     params[IOMA_MAX_PARAMS];        /* query parameters, percent-decoded         */
    size_t      n_params;
    ioma_kv     route[IOMA_MAX_ROUTE_PARAMS];   /* the :name captures, in pattern order      */
    size_t      n_route;

    ioma_slice  body;                           /* Content-Length body, or a chunked one decoded */
    bool        keep_alive;                     /* computed from version + Connection         */
} ioma_request;

/* ── the context ───────────────────────────────────────────────────────────────────────── */

/* One request's context: the request, the response being shaped, and the body sink. The head
 * (status, content type, headers, framing) is built when the first bytes go to the wire - after
 * the chain returns when everything fit the buffer, earlier when the body streams - and is frozen
 * from then on (head_sent). Valid only while the middleware chain and the handler run. */
typedef struct ioma_ctx {
    ioma_request req;

    int         status;                         /* 200 by default                            */
    ioma_slice  content_type;                   /* "text/plain" by default; any slice        */
    ioma_kv     headers[IOMA_MAX_RESP_HEADERS]; /* added with ioma_header                    */
    size_t      n_headers;
    bool        close;                          /* close the connection after this reply     */
    bool        head_sent;                      /* the head is on the wire; the above are frozen */

    void       *user;                           /* free slot: middleware hands data to the handler */

    /* the sink - private */
    conn_t     *conn;
    char       *buf;                            /* head reserve, then the body buffer        */
    char       *out;                            /* the body buffer                           */
    size_t      out_cap, out_len;
    size_t      content_length;                 /* declared with ioma_content_length         */
    bool        has_length, chunked, failed;
} ioma_ctx;

typedef void (*ioma_handler)(ioma_ctx *c);

/* Middleware runs around the handler (the onion model): shape the context, call ioma_next_run to
 * run the rest of the chain and then the endpoint, then act on the result - or write a reply and
 * return WITHOUT calling ioma_next_run to short-circuit (auth failure, cache hit). */
typedef struct ioma_next ioma_next;
typedef void (*ioma_mw)(ioma_ctx *c, ioma_next *next);
void ioma_next_run(ioma_ctx *c, ioma_next *next);

/* ── the response ──────────────────────────────────────────────────────────────────────── */

/* Body writes. They buffer; a buffer that fills up is sent, head first, and the body streams from
 * then on - chunked on HTTP/1.1, until close on HTTP/1.0, or with the length declared below.
 * Return 0, or -1 once the peer is gone (further writes are ignored). */
int  ioma_write (ioma_ctx *c, const void *data, size_t len);
int  ioma_text  (ioma_ctx *c, const char *s);                          /* a C string          */
int  ioma_printf(ioma_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  ioma_json  (ioma_ctx *c, const char *s);                          /* sets the type, writes */

/* Shape the head. Only before it is sent: ioma_header returns false afterwards. */
bool ioma_header        (ioma_ctx *c, const char *name, const char *value);   /* stays valid until sent */
void ioma_content_type  (ioma_ctx *c, const char *type);
void ioma_content_length(ioma_ctx *c, size_t n);       /* stream a large body with a known length */
int  ioma_flush         (ioma_ctx *c);                 /* send what is buffered now (starts streaming) */

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

bool       ioma_slice_eq (ioma_slice s, const char *cstr);       /* exact compare with a C string */
long       ioma_slice_int(ioma_slice s);                         /* leading integer, else 0        */

/* Parse "k=v&k2=v2" - a query string, a form body - into out, up to cap pairs. Keys and values
 * that need it ('+', %XX) are decoded into arena and point there; the rest are views of s.
 * Returns the pair count. A pair that does not fit the arena is skipped. */
size_t     ioma_kv_parse(const char *s, size_t n, ioma_kv *out, size_t cap, char *arena, size_t arena_cap);

/* ── routing ───────────────────────────────────────────────────────────────────────────── */

/* Register an endpoint. method is matched exactly; path is matched by segment and may contain
 * :name captures ("/users/:id") that land in req.route. An exact path always beats a pattern.
 * Call before ioma_run, from the main thread; the table is then read-only and shared. */
void ioma_route(const char *method, const char *path, ioma_handler fn);
/* Fallback handler when nothing matches (default is a built-in 404). */
void ioma_default(ioma_handler fn);

/* Register global middleware; it runs on every request in the order added, wrapping the handler. */
void ioma_use(ioma_mw mw);

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

/* Start `workers` proactor threads (<= 0: one per core) serving HTTP on `port`, and block until
 * SIGINT/SIGTERM. Returns 0 on clean shutdown. */
int ioma_run(int workers, int port);

/* The reason phrase for a status code ("OK", "Not Found", ...); "Unknown" if unlisted. */
const char *ioma_reason(int status);

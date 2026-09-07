/*
 * http.h - the ioma HTTP/1.1 layer: what you write endpoints against.
 *
 * An endpoint takes a parsed request and returns a response by value; the framework serializes it
 * and flushes it to the wire on the connection's coroutine (the flush suspends until the send
 * completes). Everything in a request is a slice - pointer plus length, the C span - into the
 * connection's read buffer, valid only for the duration of the handler call.
 *
 *     static ioma_response user(ioma_request *req) {
 *         ioma_slice id = ioma_route_get(req, "id");          // from "/users/:id"
 *         return ioma_textf(req, 200, "user %.*s\n", (int)id.len, id.p);
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
#define IOMA_MAX_RESP_HEADERS 16            /* extra headers a handler may add               */
#endif

/* A slice: pointer + length, the C span. Not NUL-terminated. */
typedef struct { const char *p; size_t len; } ioma_slice;

/* One key/value pair of slices: a header, a query parameter, a route parameter. */
typedef struct { ioma_slice key, value; } ioma_kv;

/* A parsed request. Every slice points into the connection's read buffer (decoded parameters
 * into a per-request arena) and is valid only until the handler returns. The request's own
 * slices are never NULL, only possibly empty; a lookup returns p == NULL when the key is absent. */
typedef struct ioma_request {
    ioma_slice  method;                         /* "GET", "POST", ...                        */
    ioma_slice  target;                         /* raw request target: path plus any query   */
    ioma_slice  path;                           /* the path, query stripped                  */
    ioma_slice  query;                          /* raw text after '?', undecoded             */
    int         minor_version;                  /* 0 or 1 for HTTP/1.0 or 1.1                */

    ioma_kv     headers[IOMA_MAX_HEADERS];      /* as received                               */
    size_t      n_headers;
    ioma_kv     params[IOMA_MAX_PARAMS];        /* query parameters, percent-decoded         */
    size_t      n_params;
    ioma_kv     route[IOMA_MAX_ROUTE_PARAMS];   /* the :name captures of the matched route   */
    size_t      n_route;

    ioma_slice  body;                           /* Content-Length body, or a chunked one decoded */
    bool        keep_alive;                     /* computed from version + Connection         */

    conn_t     *conn;                           /* advanced: await_recv/await_send in a handler */
    char       *scratch; size_t scratch_cap;    /* per-request arena for building a body      */
} ioma_request;

/* A response the handler builds and returns. body points at memory that stays valid until the
 * send completes: a string literal, static data, or the request scratch (see ioma_textf). */
typedef struct ioma_response {
    int          status;                        /* 200, 404, ...                             */
    const char  *content_type;                  /* NULL -> "text/plain"                      */
    const void  *body; size_t body_len;
    ioma_kv      extra[IOMA_MAX_RESP_HEADERS];  /* headers added with ioma_header_set        */
    int          n_extra;
    bool         close;                         /* force Connection: close after this reply  */
} ioma_response;

typedef ioma_response (*ioma_handler)(ioma_request *req);

/* Middleware runs around the handler (the onion model): do work before, call ioma_next_run to
 * invoke the rest of the chain and then the endpoint, then do work after and return its response -
 * or return a response WITHOUT calling ioma_next_run to short-circuit (auth failure, cache hit). */
typedef struct ioma_next ioma_next;
typedef ioma_response (*ioma_mw)(ioma_request *req, ioma_next *next);
ioma_response ioma_next_run(ioma_request *req, ioma_next *next);

/* ── slices and lookups ────────────────────────────────────────────────────────────────── */

bool       ioma_slice_eq (ioma_slice s, const char *cstr);       /* exact compare with a C string */
long       ioma_slice_int(ioma_slice s);                         /* leading integer, else 0        */

ioma_slice ioma_header_get(const ioma_request *req, const char *name);   /* case-insensitive     */
ioma_slice ioma_query_get (const ioma_request *req, const char *key);    /* first match, decoded */
ioma_slice ioma_route_get (const ioma_request *req, const char *name);   /* a :name capture      */

/* Parse "k=v&k2=v2" - a query string, a form body - into out, up to cap pairs. Keys and values
 * that need it ('+', %XX) are decoded into arena and point there; the rest are views of s.
 * Returns the pair count. A pair that does not fit the arena is skipped. */
size_t     ioma_kv_parse(const char *s, size_t n, ioma_kv *out, size_t cap, char *arena, size_t arena_cap);

/* ── response builders ─────────────────────────────────────────────────────────────────── */

ioma_response ioma_text (int status, const char *s);                      /* text/plain, strlen(s)   */
ioma_response ioma_json (int status, const char *s);                      /* application/json         */
ioma_response ioma_bytes(int status, const char *content_type,
                         const void *body, size_t body_len);
/* printf a body into req->scratch (truncated to scratch_cap) and return it as text/plain */
ioma_response ioma_textf(ioma_request *req, int status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/* Add a response header. name/value must stay valid until the reply is sent. */
void ioma_header_set(ioma_response *res, const char *name, const char *value);

/* ── routing ───────────────────────────────────────────────────────────────────────────── */

/* Register an endpoint. method is matched exactly; path is matched by segment and may contain
 * :name captures ("/users/:id"), read with ioma_route_get. An exact path always beats a pattern.
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

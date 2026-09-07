/*
 * router.c - the route table and the middleware chain. Both are filled before the workers start
 * and read-only after, so every worker shares them without a lock.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <string.h>

#ifndef IOMA_MAX_ROUTES
#define IOMA_MAX_ROUTES 256
#endif
#ifndef IOMA_MAX_MW
#define IOMA_MAX_MW 16
#endif

typedef struct {
    const char  *method; size_t method_len;     /* lengths fixed at registration: a lookup is  */
    const char  *path;   size_t path_len;       /* length tests first, memcmp only on a hit    */
    ioma_handler fn;                            /* the endpoint (ffi_endpoint for a foreign one) */
    ioma_ffi_handler ffi;                       /* set on a foreign route                      */
    void        *ud;
} route_t;

static ioma_response not_found(ioma_request *req);

static route_t g_routes[IOMA_MAX_ROUTES];
static int     g_nroutes;
static route_t g_fallback_route = { .fn = not_found };   /* what an unmatched request gets */

/* The chain cursor handed to each middleware; ioma_next_run advances it. */
struct ioma_next {
    const ioma_mw *mws;
    int            n;
    int            i;
    ioma_handler   handler;
};

static ioma_mw g_mws[IOMA_MAX_MW];
static int     g_nmw;

/* ── routes ────────────────────────────────────────────────────────────────────────────── */

/* Register an endpoint for an exact (method, path). */
void ioma_route(const char *method, const char *path, const ioma_handler fn)
{
    if (g_nroutes == IOMA_MAX_ROUTES) {
        fprintf(stderr, "ioma: route table full (%d), dropping %s %s\n", IOMA_MAX_ROUTES, method, path);
        return;
    }
    g_routes[g_nroutes++] = (route_t){ .method = method, .method_len = strlen(method),
                                       .path = path,     .path_len = strlen(path), .fn = fn };
}

/* Replace the built-in 404 fallback. */
void ioma_default(const ioma_handler fn)
{
    g_fallback_route.fn = fn;
}

/* The built-in fallback: a plain 404. */
static ioma_response not_found(ioma_request *req)
{
    (void)req;
    return ioma_text(404, "404 Not Found\n");
}

/* Find the route for a request. Never NULL: unmatched requests get the fallback route. */
static const route_t *match(const ioma_request *req)
{
    for (int i = 0; i < g_nroutes; i++) {
        const route_t *r = &g_routes[i];
        if (req->path_len == r->path_len && req->method_len == r->method_len &&
            memcmp(req->path, r->path, r->path_len) == 0 &&
            memcmp(req->method, r->method, r->method_len) == 0) {
            return r;
        }
    }
    return &g_fallback_route;
}

/* ── foreign handlers ──────────────────────────────────────────────────────────────────── */

static __thread const route_t *t_ffi_route;    /* the foreign route being dispatched on this thread */

struct ffi_call {
    ioma_ffi_handler        fn;
    const ioma_ffi_request *req;
    ioma_ffi_response      *res;
    void                   *ud;
};

/* What the loop runs on the thread stack: the foreign handler itself. */
static void ffi_call_run(void *arg)
{
    struct ffi_call *k = arg;
    k->fn(k->req, k->res, k->ud);
}

/* The endpoint every foreign route uses: flatten the request, hop to the thread stack for the
 * call, and turn the filled-in reply into an ioma_response. */
static ioma_response ffi_endpoint(ioma_request *req)
{
    const route_t *r = t_ffi_route;
    ioma_ffi_request fr = {
        req->method, req->method_len, req->path, req->path_len, req->query, req->query_len,
        req->body, req->body_len, req->scratch, req->scratch_cap, req,
    };
    ioma_ffi_response out = { .status = 200 };
    struct ffi_call k = { r->ffi, &fr, &out, r->ud };
    await_call(req->conn->p, ffi_call_run, &k);
    ioma_response res = ioma_bytes(out.status, out.content_type, out.body, out.body_len);
    res.close = out.close != 0;
    return res;
}

/* Register a foreign handler for an exact (method, path). */
void ioma_route_ffi(const char *method, const char *path, ioma_ffi_handler fn, void *userdata)
{
    if (g_nroutes == IOMA_MAX_ROUTES) {
        fprintf(stderr, "ioma: route table full (%d), dropping %s %s\n", IOMA_MAX_ROUTES, method, path);
        return;
    }
    g_routes[g_nroutes++] = (route_t){ .method = method, .method_len = strlen(method),
                                       .path = path,     .path_len = strlen(path),
                                       .fn = ffi_endpoint, .ffi = fn, .ud = userdata };
}

/* ── middleware ────────────────────────────────────────────────────────────────────────── */

/* Register global middleware; it wraps every request in the order added. */
void ioma_use(ioma_mw mw)
{
    if (g_nmw == IOMA_MAX_MW) {
        fprintf(stderr, "ioma: middleware chain full (%d), dropping one\n", IOMA_MAX_MW);
        return;
    }
    g_mws[g_nmw++] = mw;
}

/* Run the next middleware, or the endpoint once the chain is exhausted. A middleware that does
 * not call this short-circuits the request. */
ioma_response ioma_next_run(ioma_request *req, ioma_next *next)
{
    if (next->i < next->n) {
        ioma_mw   mw    = next->mws[next->i];
        ioma_next inner = { next->mws, next->n, next->i + 1, next->handler };
        return mw(req, &inner);
    }
    return next->handler(req);
}

/* Match the route, then run the middleware chain around it. With no middleware registered this
 * is a direct call. */
ioma_response ioma__dispatch(ioma_request *req)
{
    const route_t *r = match(req);
    if (r->ffi)
        t_ffi_route = r;                        /* ffi_endpoint reads it, on this same thread */
    if (g_nmw == 0)
        return r->fn(req);
    ioma_next next = { g_mws, g_nmw, 0, r->fn };
    return ioma_next_run(req, &next);
}

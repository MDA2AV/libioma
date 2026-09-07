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
    ioma_handler fn;
} route_t;

static route_t      g_routes[IOMA_MAX_ROUTES];
static int          g_nroutes;
static ioma_handler g_fallback;

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
    g_fallback = fn;
}

/* The built-in fallback: a plain 404. */
static ioma_response not_found(ioma_request *req)
{
    (void)req;
    return ioma_text(404, "404 Not Found\n");
}

/* Find the handler for a request. Never NULL: unmatched requests get the fallback. */
ioma_handler ioma__match(const ioma_request *req)
{
    for (int i = 0; i < g_nroutes; i++) {
        const route_t *r = &g_routes[i];
        if (req->path_len == r->path_len && req->method_len == r->method_len &&
            memcmp(req->path, r->path, r->path_len) == 0 &&
            memcmp(req->method, r->method, r->method_len) == 0) {
            return r->fn;
        }
    }
    return g_fallback ? g_fallback : not_found;
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
    ioma_handler h = ioma__match(req);
    if (g_nmw == 0)
        return h(req);
    ioma_next next = { g_mws, g_nmw, 0, h };
    return ioma_next_run(req, &next);
}

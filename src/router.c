/*
 * router.c - the route table. Exact (method, path) match, linear scan. Registered once before
 * the workers start, then read-only, so it is shared across worker threads without a lock (the
 * shared-nothing rule bends only for immutable data).
 */
#include "http.h"

#include <stdio.h>
#include <string.h>

#ifndef IOMA_MAX_ROUTES
#define IOMA_MAX_ROUTES 256
#endif

typedef struct {
    const char  *method;
    const char  *path;
    ioma_handler fn;
} route_t;

static route_t      g_routes[IOMA_MAX_ROUTES];
static int          g_nroutes;
static ioma_handler g_fallback;

void ioma_route(const char *method, const char *path, ioma_handler fn)
{
    if (g_nroutes == IOMA_MAX_ROUTES) {
        fprintf(stderr, "ioma: route table full (%d), dropping %s %s\n", IOMA_MAX_ROUTES, method, path);
        return;
    }
    g_routes[g_nroutes++] = (route_t){ method, path, fn };
}

void ioma_default(ioma_handler fn)
{
    g_fallback = fn;
}

/* Built-in fallback: a plain 404. */
static ioma_response not_found(ioma_request *req)
{
    (void)req;
    return ioma_text(404, "404 Not Found\n");
}

/* Called by the serve loop for each request. Never returns NULL - there is always a handler. */
ioma_handler ioma__match(const ioma_request *req)
{
    for (int i = 0; i < g_nroutes; i++) {
        if (ioma_slice_eq(req->method, req->method_len, g_routes[i].method) &&
            ioma_slice_eq(req->path,   req->path_len,   g_routes[i].path)) {
            return g_routes[i].fn;
        }
    }
    return g_fallback ? g_fallback : not_found;
}

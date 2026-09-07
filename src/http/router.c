/*
 * router.c - the route table and the middleware chain. Both are filled before the workers start
 * and read-only after, so every worker shares them without a lock. A path is either exact or a
 * pattern with :name segments; exact routes always win.
 */
#include "http/internal.h"

#include <string.h>

#ifndef IOMA_MAX_ROUTES
#define IOMA_MAX_ROUTES   256
#endif
#ifndef IOMA_MAX_SEGMENTS
#define IOMA_MAX_SEGMENTS 16                        /* path segments a pattern may have */
#endif
#ifndef IOMA_MAX_MW
#define IOMA_MAX_MW       16
#endif

struct seg {
    ioma_slice s;                                   /* the literal, or the name after ':'  */
    bool       capture;
};

typedef struct {
    const char  *method; size_t method_len;         /* lengths fixed at registration: a lookup is  */
    const char  *path;   size_t path_len;           /* length tests first, memcmp only on a hit    */
    struct seg   seg[IOMA_MAX_SEGMENTS];
    int          nseg;                              /* > 0: a pattern route                        */
    ioma_handler fn;
} route_t;

static void not_found(ioma_ctx *ctx);

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

/* Split a path with ':' segments into literals and captures. A path without ':' stays exact. */
static void compile_pattern(route_t *route)
{
    route->nseg = 0;
    if (!memchr(route->path, ':', route->path_len))
        return;
    const char *at = route->path, *end = route->path + route->path_len;
    while (at < end) {
        while (at < end && *at == '/') at++;
        if (at == end) break;
        const char *seg_end = memchr(at, '/', (size_t)(end - at));
        if (!seg_end) seg_end = end;
        if (route->nseg == IOMA_MAX_SEGMENTS) {
            fprintf(stderr, "ioma: pattern %s has more than %d segments\n", route->path, IOMA_MAX_SEGMENTS);
            route->nseg = 0;
            return;
        }
        struct seg *segment = &route->seg[route->nseg++];
        segment->capture = *at == ':';
        segment->s = segment->capture ? (ioma_slice){ at + 1, (size_t)(seg_end - at - 1) }
                                      : (ioma_slice){ at, (size_t)(seg_end - at) };
        at = seg_end;
    }
}

/* Register an endpoint for a method and an exact path or a :name pattern. */
void ioma_route(const char *method, const char *path, const ioma_handler fn)
{
    if (g_nroutes == IOMA_MAX_ROUTES) {
        fprintf(stderr, "ioma: route table full (%d), dropping %s %s\n", IOMA_MAX_ROUTES, method, path);
        return;
    }
    route_t *route = &g_routes[g_nroutes++];
    *route = (route_t){
        .method = method, .method_len = strlen(method),
        .path   = path,   .path_len   = strlen(path),
        .fn     = fn,
    };
    compile_pattern(route);
}

/* Replace the built-in 404 fallback. */
void ioma_default(const ioma_handler fn)
{
    g_fallback_route.fn = fn;
}

/* The built-in fallback: a plain 404. */
static void not_found(ioma_ctx *ctx)
{
    ctx->res.status = 404;
    ioma_text(ctx, "404 Not Found\n");
}

/* Walk the request path against a pattern segment by segment, recording the captures in
 * req->route_params. A trailing slash is tolerated; extra or missing segments are not. */
static bool match_pattern(const route_t *route, ioma_request *req)
{
    const char *at = req->path.p, *end = at + req->path.len;
    size_t captured = 0;
    for (int i = 0; i < route->nseg; i++) {
        while (at < end && *at == '/') at++;
        if (at == end)
            return false;                                    /* fewer segments than the pattern */
        const char *seg_end = memchr(at, '/', (size_t)(end - at));
        if (!seg_end) seg_end = end;
        ioma_slice seg = { at, (size_t)(seg_end - at) };
        if (route->seg[i].capture) {
            if (captured < IOMA_MAX_ROUTE_PARAMS)
                req->route_params[captured++] = (ioma_kv){ route->seg[i].s, seg };
        } else if (seg.len != route->seg[i].s.len || memcmp(seg.p, route->seg[i].s.p, seg.len) != 0) {
            return false;
        }
        at = seg_end;
    }
    while (at < end && *at == '/') at++;
    if (at != end)
        return false;                                        /* more segments than the pattern */
    req->n_route_params = captured;
    return true;
}

/* Does the request's method match the route's? Lengths first, memcmp only on a hit. */
static bool same_method(const route_t *route, const ioma_request *req)
{
    return req->method.len == route->method_len && memcmp(req->method.p, route->method, route->method_len) == 0;
}

/* Does the request's path equal the route's exact path? */
static bool same_path(const route_t *route, const ioma_request *req)
{
    return req->path.len == route->path_len && memcmp(req->path.p, route->path, route->path_len) == 0;
}

/* Find the route for a request and fill its route parameters. Exact routes first, so a static
 * path beats a pattern that would also match it. Never nullptr: unmatched requests get the fallback. */
static const route_t *match(ioma_request *req)
{
    req->n_route_params = 0;
    for (int i = 0; i < g_nroutes; i++) {
        const route_t *route = &g_routes[i];
        if (route->nseg == 0 && same_path(route, req) && same_method(route, req))
            return route;
    }
    for (int i = 0; i < g_nroutes; i++) {
        const route_t *route = &g_routes[i];
        if (route->nseg && same_method(route, req) && match_pattern(route, req))
            return route;
    }
    return &g_fallback_route;
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
void ioma_next_run(ioma_ctx *ctx, ioma_next *next)
{
    if (next->i < next->n) {
        ioma_mw   mw    = next->mws[next->i];
        ioma_next inner = { next->mws, next->n, next->i + 1, next->handler };
        mw(ctx, &inner);
        return;
    }
    next->handler(ctx);
}

/* Match the route, then run the middleware chain around it. With no middleware registered this
 * is a direct call. */
void ioma__dispatch(ioma_ctx *ctx)
{
    const route_t *route = match(&ctx->req);
    if (g_nmw == 0) {
        route->fn(ctx);
        return;
    }
    ioma_next next = { g_mws, g_nmw, 0, route->fn };
    ioma_next_run(ctx, &next);
}

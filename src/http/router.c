/*
 * router.c - the route table and the middleware chain. Both are filled before the workers start
 * and read-only after, so every worker shares them without a lock. A path is either exact or a
 * pattern with :name segments; exact routes always win.
 */
#define _GNU_SOURCE
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

static void not_found(ioma_ctx *c);

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
static void compile_pattern(route_t *r)
{
    r->nseg = 0;
    if (!memchr(r->path, ':', r->path_len))
        return;
    const char *p = r->path, *end = r->path + r->path_len;
    while (p < end) {
        while (p < end && *p == '/') p++;
        if (p == end) break;
        const char *e = memchr(p, '/', (size_t)(end - p));
        if (!e) e = end;
        if (r->nseg == IOMA_MAX_SEGMENTS) {
            fprintf(stderr, "ioma: pattern %s has more than %d segments\n", r->path, IOMA_MAX_SEGMENTS);
            r->nseg = 0;
            return;
        }
        struct seg *s = &r->seg[r->nseg++];
        s->capture = *p == ':';
        s->s = s->capture ? (ioma_slice){ p + 1, (size_t)(e - p - 1) } : (ioma_slice){ p, (size_t)(e - p) };
        p = e;
    }
}

/* Register an endpoint for a method and an exact path or a :name pattern. */
void ioma_route(const char *method, const char *path, const ioma_handler fn)
{
    if (g_nroutes == IOMA_MAX_ROUTES) {
        fprintf(stderr, "ioma: route table full (%d), dropping %s %s\n", IOMA_MAX_ROUTES, method, path);
        return;
    }
    route_t *r = &g_routes[g_nroutes++];
    *r = (route_t){ .method = method, .method_len = strlen(method),
                    .path = path,     .path_len = strlen(path), .fn = fn };
    compile_pattern(r);
}

/* Replace the built-in 404 fallback. */
void ioma_default(const ioma_handler fn)
{
    g_fallback_route.fn = fn;
}

/* The built-in fallback: a plain 404. */
static void not_found(ioma_ctx *c)
{
    c->res.status = 404;
    ioma_text(c, "404 Not Found\n");
}

/* Walk the request path against a pattern segment by segment, recording the captures in
 * req->route_params. A trailing slash is tolerated; extra or missing segments are not. */
static bool match_pattern(const route_t *r, ioma_request *req)
{
    const char *p = req->path.p, *end = p + req->path.len;
    size_t n = 0;
    for (int i = 0; i < r->nseg; i++) {
        while (p < end && *p == '/') p++;
        if (p == end)
            return false;                                    /* fewer segments than the pattern */
        const char *e = memchr(p, '/', (size_t)(end - p));
        if (!e) e = end;
        ioma_slice seg = { p, (size_t)(e - p) };
        if (r->seg[i].capture) {
            if (n < IOMA_MAX_ROUTE_PARAMS)
                req->route_params[n++] = (ioma_kv){ r->seg[i].s, seg };
        } else if (seg.len != r->seg[i].s.len || memcmp(seg.p, r->seg[i].s.p, seg.len) != 0) {
            return false;
        }
        p = e;
    }
    while (p < end && *p == '/') p++;
    if (p != end)
        return false;                                        /* more segments than the pattern */
    req->n_route_params = n;
    return true;
}

/* Find the route for a request and fill its route parameters. Exact routes first, so a static
 * path beats a pattern that would also match it. Never NULL: unmatched requests get the fallback. */
static const route_t *match(ioma_request *req)
{
    req->n_route_params = 0;
    for (int i = 0; i < g_nroutes; i++) {
        const route_t *r = &g_routes[i];
        if (r->nseg == 0 && req->path.len == r->path_len && req->method.len == r->method_len &&
            memcmp(req->path.p, r->path, r->path_len) == 0 &&
            memcmp(req->method.p, r->method, r->method_len) == 0)
            return r;
    }
    for (int i = 0; i < g_nroutes; i++) {
        const route_t *r = &g_routes[i];
        if (r->nseg && req->method.len == r->method_len &&
            memcmp(req->method.p, r->method, r->method_len) == 0 && match_pattern(r, req))
            return r;
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
void ioma_next_run(ioma_ctx *c, ioma_next *next)
{
    if (next->i < next->n) {
        ioma_mw   mw    = next->mws[next->i];
        ioma_next inner = { next->mws, next->n, next->i + 1, next->handler };
        mw(c, &inner);
        return;
    }
    next->handler(c);
}

/* Match the route, then run the middleware chain around it. With no middleware registered this
 * is a direct call. */
void ioma__dispatch(ioma_ctx *c)
{
    const route_t *r = match(&c->req);
    if (g_nmw == 0) {
        r->fn(c);
        return;
    }
    ioma_next next = { g_mws, g_nmw, 0, r->fn };
    ioma_next_run(c, &next);
}

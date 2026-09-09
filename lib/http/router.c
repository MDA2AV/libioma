#include "http/router.h"

#include "http/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEEN_MAX  8
#define ALLOW_MAX 16

struct ioxd_group {
    ioxd_group *parent;
    const char *prefix;
    ioxd_mw     mws[IOXD_MAX_MW];
    int         n_mws;
};

struct ioxd_endpoint {
    ioxd_endpoint *next;
    ioxd_group    *group;
    const char    *method;
    size_t         method_len;
    const char    *path;
    int            seq;
    ioxd_handler   fn;
    ioxd_mw        own[IOXD_MAX_MW];
    int            n_own;

    char          *full;
    ioxd_slice     names[IOXD_MAX_ROUTE_PARAMS];
    size_t         n_names;
    ioxd_mw       *chain;
    int            n_chain;
};

struct node {
    ioxd_slice      seg;
    struct node   **kids;
    int             n_kids;
    struct node    *param;
    ioxd_endpoint **eps;
    int             n_eps;
};

struct seen {
    const struct node *nodes[SEEN_MAX];
    int                n;
};

struct ioxd_next {
    const ioxd_mw *mws;
    int            n;
    int            i;
    ioxd_handler   handler;
};

static void not_found(ioxd_ctx *ctx);

static ioxd_group     g_root = { .prefix = "" };
static ioxd_group    *g_current = &g_root;
static ioxd_endpoint *g_first, *g_last;
static int            g_n_eps;
static struct node    g_tree;
static ioxd_handler   g_fallback = not_found;
static bool           g_built;

static bool same(ioxd_slice a, ioxd_slice b)
{
    return a.len == b.len && memcmp(a.p, b.p, a.len) == 0;
}

#define method_is(ep, name) ((ep)->method_len == sizeof(name) - 1 && \
                             memcmp((ep)->method, (name), sizeof(name) - 1) == 0)

static void *must(void *p)
{
    if (!p) {
        perror("ioxd: malloc");
        abort();
    }
    return p;
}

static bool too_late(const char *what, const char *which)
{
    if (!g_built)
        return false;
    fprintf(stderr, "ioxd: %s %s registered after ioxd_run started; ignored\n", what, which);
    return true;
}

ioxd_group *ioxd_group_new(ioxd_group *parent, const char *prefix)
{
    ioxd_group *group = must(calloc(1, sizeof *group));
    group->parent = parent ? parent : &g_root;
    group->prefix = must(strdup(prefix));
    return group;
}

void ioxd_group_use(ioxd_group *group, ioxd_mw mw)
{
    if (!group)
        group = &g_root;
    if (too_late("middleware for", group == &g_root ? "the root" : group->prefix))
        return;
    if (group->n_mws == IOXD_MAX_MW) {
        fprintf(stderr, "ioxd: group %s already has %d middleware, dropping one\n", group->prefix, IOXD_MAX_MW);
        return;
    }
    group->mws[group->n_mws++] = mw;
}

void ioxd_use(ioxd_mw mw)
{
    if (too_late("middleware for", "the root"))
        return;
    ioxd_group_use(&g_root, mw);
}

ioxd_endpoint *ioxd_route(ioxd_group *group, const char *method, const char *path, ioxd_handler fn)
{
    if (too_late(method, path))
        return nullptr;
    ioxd_endpoint *ep = must(calloc(1, sizeof *ep));
    ep->group      = group ? group : &g_root;
    ep->method     = must(strdup(method));
    ep->method_len = strlen(method);
    ep->path       = must(strdup(path));
    ep->seq        = g_n_eps++;
    ep->fn         = fn;
    if (g_last)
        g_last->next = ep;
    else
        g_first = ep;
    g_last = ep;
    return ep;
}

void ioxd_endpoint_use(ioxd_endpoint *endpoint, ioxd_mw mw)
{
    if (!endpoint)
        return;
    if (too_late("middleware for", endpoint->path))
        return;
    if (endpoint->n_own == IOXD_MAX_MW) {
        fprintf(stderr, "ioxd: %s %s already has %d middleware, dropping one\n", endpoint->method, endpoint->path, IOXD_MAX_MW);
        return;
    }
    endpoint->own[endpoint->n_own++] = mw;
}

ioxd_group *ioxd__group_begin(struct ioxd_group_args args)
{
    ioxd_group *group = ioxd_group_new(g_current, args.prefix);
    for (int i = 0; i < IOXD_MAX_MW && args.mws[i]; i++)
        ioxd_group_use(group, args.mws[i]);
    g_current = group;
    return group;
}

ioxd_group *ioxd__group_end(void)
{
    if (g_current->parent)
        g_current = g_current->parent;
    return nullptr;
}

void ioxd__group_pop(ioxd_group **open)
{
    if (*open)
        ioxd__group_end();
}

ioxd_group *ioxd__group_current(void)
{
    return g_current;
}

ioxd_endpoint *ioxd__endpoint(const char *method, struct ioxd_endpoint_args args)
{
    ioxd_endpoint *ep = ioxd_route(g_current, method, args.path, args.fn);
    for (int i = 0; i < IOXD_MAX_MW && args.mws[i]; i++)
        ioxd_endpoint_use(ep, args.mws[i]);
    return ep;
}

void ioxd_default(ioxd_handler fn)
{
    if (too_late("a fallback", "handler"))
        return;
    g_fallback = fn;
}

static bool next_segment(const char **at, const char *end, ioxd_slice *seg)
{
    const char *p = *at;
    while (p < end && *p == '/')
        p++;
    if (p == end) {
        *at = end;
        return false;
    }
    const char *seg_end = memchr(p, '/', (size_t)(end - p));
    if (!seg_end)
        seg_end = end;
    *seg = (ioxd_slice){ p, (size_t)(seg_end - p) };
    *at  = seg_end;
    return true;
}

static void prepend(char *buf, size_t *at, const char *part, size_t n)
{
    if (n && buf[*at] && part[n - 1] != '/' && buf[*at] != '/')
        buf[--*at] = '/';
    *at -= n;
    memcpy(buf + *at, part, n);
}

static char *full_path(const ioxd_endpoint *ep)
{
    size_t len = strlen(ep->path), parts = 1;
    for (const ioxd_group *g = ep->group; g; g = g->parent) {
        len += strlen(g->prefix);
        parts++;
    }
    char  *full = must(malloc(len + parts + 1));
    size_t at   = len + parts;
    full[at] = '\0';
    prepend(full, &at, ep->path, strlen(ep->path));
    for (const ioxd_group *g = ep->group; g; g = g->parent)
        prepend(full, &at, g->prefix, strlen(g->prefix));
    memmove(full, full + at, len + parts + 1 - at);
    return full;
}

static void flatten_chain(ioxd_endpoint *ep)
{
    int n = ep->n_own;
    for (const ioxd_group *g = ep->group; g; g = g->parent)
        n += g->n_mws;
    ep->n_chain = n;
    if (n == 0)
        return;
    ep->chain = must(malloc((size_t)n * sizeof *ep->chain));
    int at = n - ep->n_own;
    memcpy(ep->chain + at, ep->own, (size_t)ep->n_own * sizeof *ep->chain);
    for (const ioxd_group *g = ep->group; g; g = g->parent) {
        at -= g->n_mws;
        memcpy(ep->chain + at, g->mws, (size_t)g->n_mws * sizeof *ep->chain);
    }
}

static struct node *child(struct node *node, ioxd_slice seg)
{
    for (int i = 0; i < node->n_kids; i++)
        if (same(node->kids[i]->seg, seg))
            return node->kids[i];
    struct node  *kid  = must(calloc(1, sizeof *kid));
    struct node **kids = must(realloc(node->kids, ((size_t)node->n_kids + 1) * sizeof *kids));
    kid->seg   = seg;
    node->kids = kids;
    node->kids[node->n_kids++] = kid;
    return kid;
}

static void insert(ioxd_endpoint *ep)
{
    struct node *node = &g_tree;
    const char  *at = ep->full, *end = ep->full + strlen(ep->full);
    ioxd_slice   seg;
    while (next_segment(&at, end, &seg)) {
        if (seg.p[0] == ':') {
            if (ep->n_names == IOXD_MAX_ROUTE_PARAMS) {
                fprintf(stderr, "ioxd: %s %s has more than %d captures; ignored\n", ep->method, ep->full, IOXD_MAX_ROUTE_PARAMS);
                return;
            }
            ep->names[ep->n_names++] = (ioxd_slice){ seg.p + 1, seg.len - 1 };
            if (!node->param)
                node->param = must(calloc(1, sizeof *node->param));
            node = node->param;
        } else {
            node = child(node, seg);
        }
    }
    for (int i = 0; i < node->n_eps; i++) {
        if (node->eps[i]->method_len == ep->method_len && memcmp(node->eps[i]->method, ep->method, ep->method_len) == 0) {
            fprintf(stderr, "ioxd: duplicate route %s %s; keeping the first\n", ep->method, ep->full);
            return;
        }
    }
    ioxd_endpoint **eps = must(realloc(node->eps, ((size_t)node->n_eps + 1) * sizeof *eps));
    node->eps = eps;
    node->eps[node->n_eps++] = ep;
}

static int group_depth(void)
{
    int depth = 0;
    for (const ioxd_group *g = g_current; g->parent; g = g->parent)
        depth++;
    return depth;
}

void ioxd__router_build(void)
{
    if (g_built)
        return;
    int depth = group_depth();
    if (depth)
        fprintf(stderr, "ioxd: %d group(s) still open at ioxd_run, the innermost \"%s\": everything "
                        "registered after it nested inside\n", depth, g_current->prefix);
    g_built = true;
    for (ioxd_endpoint *ep = g_first; ep; ep = ep->next) {
        ep->full = full_path(ep);
        insert(ep);
        flatten_chain(ep);
    }
}

static const ioxd_endpoint *endpoint_for(const struct node *node, ioxd_slice method)
{
    const ioxd_endpoint *get = nullptr;
    for (int i = 0; i < node->n_eps; i++) {
        if (node->eps[i]->method_len == method.len && memcmp(node->eps[i]->method, method.p, method.len) == 0)
            return node->eps[i];
        if (method_is(node->eps[i], "GET"))
            get = node->eps[i];
    }
    return method.len == 4 && memcmp(method.p, "HEAD", 4) == 0 ? get : nullptr;
}

static const ioxd_endpoint *walk(const struct node *node, const char *at, const char *end,
                                 ioxd_request *req, struct seen *seen)
{
    ioxd_slice seg;
    if (!next_segment(&at, end, &seg)) {
        if (node->n_eps && seen->n < SEEN_MAX)
            seen->nodes[seen->n++] = node;
        return endpoint_for(node, req->method);
    }
    for (int i = 0; i < node->n_kids; i++) {
        if (same(node->kids[i]->seg, seg)) {
            const ioxd_endpoint *ep = walk(node->kids[i], at, end, req, seen);
            if (ep)
                return ep;
            break;
        }
    }
    if (node->param && req->n_route_params < IOXD_MAX_ROUTE_PARAMS) {
        size_t mark = req->n_route_params;
        req->route_params[req->n_route_params++].value = seg;
        const ioxd_endpoint *ep = walk(node->param, at, end, req, seen);
        if (ep)
            return ep;
        req->n_route_params = mark;
    }
    return nullptr;
}

static ioxd_slice decoded(ioxd_slice raw, char *arena, size_t cap, size_t *used)
{
    if (!memchr(raw.p, '%', raw.len) || *used + raw.len > cap)
        return raw;
    char  *dst = arena + *used;
    size_t out = 0;
    for (size_t i = 0; i < raw.len; i++) {
        if (raw.p[i] == '%' && i + 2 < raw.len) {
            int hi = ioxd__hexval((unsigned char)raw.p[i + 1]);
            int lo = ioxd__hexval((unsigned char)raw.p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        dst[out++] = raw.p[i];
    }
    *used += out;
    return (ioxd_slice){ dst, out };
}

static bool listed(const ioxd_endpoint *out[], int n, const ioxd_endpoint *ep)
{
    for (int i = 0; i < n; i++)
        if (out[i]->method_len == ep->method_len &&
            memcmp(out[i]->method, ep->method, ep->method_len) == 0)
            return true;
    return false;
}

static int allowed_endpoints(const struct seen *seen, const ioxd_endpoint *out[ALLOW_MAX])
{
    int n = 0;
    for (int i = 0; i < seen->n; i++) {
        for (int j = 0; j < seen->nodes[i]->n_eps && n < ALLOW_MAX; j++) {
            const ioxd_endpoint *ep = seen->nodes[i]->eps[j];
            if (listed(out, n, ep))
                continue;
            int at = 0;
            while (at < n && out[at]->seq < ep->seq)
                at++;
            memmove(out + at + 1, out + at, (size_t)(n - at) * sizeof *out);
            out[at] = ep;
            n++;
        }
    }
    return n;
}

static const char *allow_value(ioxd_request *req, const struct seen *seen)
{
    const ioxd_endpoint *eps[ALLOW_MAX];
    int    n = allowed_endpoints(seen, eps);
    bool   get = false, head = false;
    size_t len = n ? (size_t)(n - 1) * 2 + 1 : 0;
    for (int i = 0; i < n; i++) {
        len += eps[i]->method_len;
        get  = get  || method_is(eps[i], "GET");
        head = head || method_is(eps[i], "HEAD");
    }
    bool add_head = get && !head;
    if (n == 0 || len + (add_head ? 6 : 0) > sizeof req->route_arena)
        return nullptr;
    char *at = req->route_arena;
    for (int i = 0; i < n; i++) {
        if (i) {
            memcpy(at, ", ", 2);
            at += 2;
        }
        memcpy(at, eps[i]->method, eps[i]->method_len);
        at += eps[i]->method_len;
        if (add_head && method_is(eps[i], "GET")) {
            memcpy(at, ", HEAD", 6);
            at += 6;
        }
    }
    *at = '\0';
    return req->route_arena;
}

void ioxd_next_run(ioxd_ctx *ctx, ioxd_next *next)
{
    if (next->i > next->n)
        return;
    int at = next->i++;
    if (at < next->n)
        next->mws[at](ctx, next);
    else
        next->handler(ctx);
}

static void run(ioxd_ctx *ctx, const ioxd_mw *mws, int n, ioxd_handler fn)
{
    if (n == 0) {
        fn(ctx);
        return;
    }
    ioxd_next next = { mws, n, 0, fn };
    ioxd_next_run(ctx, &next);
}

static void not_found(ioxd_ctx *ctx)
{
    ctx->res.status = 404;
    ioxd_text(ctx, "404 Not Found\n");
}
static void not_allowed(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "405 Method Not Allowed\n");
}

void ioxd__dispatch(ioxd_ctx *ctx)
{
    ioxd_request *req  = &ctx->req;
    struct seen   seen = { .n = 0 };
    req->n_route_params = 0;
    const ioxd_endpoint *ep = walk(&g_tree, req->path.p, req->path.p + req->path.len, req, &seen);
    if (ep) {
        size_t used = 0;
        for (size_t i = 0; i < req->n_route_params; i++) {
            req->route_params[i].key   = ep->names[i];
            req->route_params[i].value = decoded(req->route_params[i].value, req->route_arena,
                                                 sizeof req->route_arena, &used);
        }
        run(ctx, ep->chain, ep->n_chain, ep->fn);
        return;
    }
    req->n_route_params = 0;
    if (seen.n) {
        const char *allow = allow_value(req, &seen);
        ctx->res.status = 405;
        if (allow)
            ioxd_header(ctx, "allow", allow);
        run(ctx, g_root.mws, g_root.n_mws, not_allowed);
        return;
    }
    run(ctx, g_root.mws, g_root.n_mws, g_fallback);
}

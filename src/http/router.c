/*
 * router.c - groups, endpoints, middleware, and the segment tree they resolve into. Everything is
 * registered before the workers start and resolved once by ioma_run; after that it is read-only
 * and every worker shares it without a lock. A request costs one walk down the tree and one call
 * through its endpoint's flat middleware chain.
 */
#include "http/internal.h"

#include <string.h>


struct ioma_group {
    ioma_group *parent;                             /* nullptr only for the root */
    const char *prefix;
    ioma_mw     mws[IOMA_MAX_MW];
    int         n_mws;
};

struct ioma_endpoint {
    ioma_endpoint *next;                            /* the registration list */
    ioma_group    *group;
    const char    *method;
    size_t         method_len;
    const char    *path;                            /* below the group's prefix */
    ioma_handler   fn;
    ioma_mw        own[IOMA_MAX_MW];
    int            n_own;
    /* resolved by ioma__router_build */
    char          *full;                            /* the whole path, prefixes included */
    ioma_slice     names[IOMA_MAX_ROUTE_PARAMS];    /* the :name captures, in path order */
    size_t         n_names;
    ioma_mw       *chain;                           /* root, each group outer to inner, then own */
    int            n_chain;
};

/* One segment of the tree; the root is the empty one. */
struct node {
    ioma_slice      seg;                            /* the static segment this node is */
    struct node   **kids;                           /* static children */
    int             n_kids;
    struct node    *param;                          /* the child that takes any segment */
    ioma_endpoint **eps;                            /* the endpoints here, one per method */
    int             n_eps;
    char           *allow;                          /* "GET, POST": the methods here, for a 405 */
};

/* The chain cursor handed to each middleware; ioma_next_run advances it. */
struct ioma_next {
    const ioma_mw *mws;
    int            n;
    int            i;
    ioma_handler   handler;
};

static void not_found(ioma_ctx *ctx);

static ioma_group     g_root = { .prefix = "" };    /* no prefix; ioma_use's middleware */
static ioma_group    *g_current = &g_root;          /* the script form's open group */
static ioma_endpoint *g_first, *g_last;             /* endpoints in registration order */
static struct node    g_tree;                       /* the root node */
static ioma_handler   g_fallback = not_found;
static bool           g_built;

/* Exact slice compare. */
static bool same(ioma_slice a, ioma_slice b)
{
    return a.len == b.len && memcmp(a.p, b.p, a.len) == 0;
}

/* Out of memory at startup: nothing sensible to continue with. */
static void *must(void *p)
{
    if (!p) {
        perror("ioma: malloc");
        abort();
    }
    return p;
}

/* ── registration ──────────────────────────────────────────────────────────────────────── */

/* A group below parent (nullptr: the root) at prefix. */
ioma_group *ioma_group_new(ioma_group *parent, const char *prefix)
{
    ioma_group *group = must(calloc(1, sizeof *group));
    group->parent = parent ? parent : &g_root;
    group->prefix = prefix;
    return group;
}

/* Middleware around everything below the group. */
void ioma_group_use(ioma_group *group, ioma_mw mw)
{
    if (!group)
        group = &g_root;
    if (group->n_mws == IOMA_MAX_MW) {
        fprintf(stderr, "ioma: group %s already has %d middleware, dropping one\n", group->prefix, IOMA_MAX_MW);
        return;
    }
    group->mws[group->n_mws++] = mw;
}

/* Root middleware: every request. */
void ioma_use(ioma_mw mw)
{
    ioma_group_use(&g_root, mw);
}

/* An endpoint in a group (nullptr: the root). */
ioma_endpoint *ioma_route(ioma_group *group, const char *method, const char *path, ioma_handler fn)
{
    if (g_built) {
        fprintf(stderr, "ioma: %s %s registered after ioma_run started; ignored\n", method, path);
        return nullptr;
    }
    ioma_endpoint *ep = must(calloc(1, sizeof *ep));
    ep->group      = group ? group : &g_root;
    ep->method     = method;
    ep->method_len = strlen(method);
    ep->path       = path;
    ep->fn         = fn;
    if (g_last)
        g_last->next = ep;
    else
        g_first = ep;
    g_last = ep;
    return ep;
}

/* Middleware around one endpoint. */
void ioma_endpoint_use(ioma_endpoint *endpoint, ioma_mw mw)
{
    if (!endpoint)
        return;
    if (endpoint->n_own == IOMA_MAX_MW) {
        fprintf(stderr, "ioma: %s %s already has %d middleware, dropping one\n", endpoint->method, endpoint->path, IOMA_MAX_MW);
        return;
    }
    endpoint->own[endpoint->n_own++] = mw;
}

/* --- the script form (the IOMA_ macros) --- */

/* Open a group below the current one and make it current; its middleware list ends at a null. */
ioma_group *ioma__group_begin(struct ioma_group_args args)
{
    ioma_group *group = ioma_group_new(g_current, args.prefix);
    for (int i = 0; i < IOMA_MAX_MW && args.mws[i]; i++)
        ioma_group_use(group, args.mws[i]);
    g_current = group;
    return group;
}

/* Close the current group; null, so the block's loop ends. */
ioma_group *ioma__group_end(void)
{
    if (g_current->parent)
        g_current = g_current->parent;
    return nullptr;
}

/* The group a script-form registration goes into. */
ioma_group *ioma__group_current(void)
{
    return g_current;
}

/* An endpoint in the current group, with its middleware list (ended by a null). */
ioma_endpoint *ioma__endpoint(const char *method, struct ioma_endpoint_args args)
{
    ioma_endpoint *ep = ioma_route(g_current, method, args.path, args.fn);
    for (int i = 0; i < IOMA_MAX_MW && args.mws[i]; i++)
        ioma_endpoint_use(ep, args.mws[i]);
    return ep;
}

/* Replace the built-in 404 fallback. */
void ioma_default(ioma_handler fn)
{
    g_fallback = fn;
}

/* ── resolution, once, from ioma_run ───────────────────────────────────────────────────── */

/* The next segment of a path from *at, slashes skipped; false at the end. */
static bool next_segment(const char **at, const char *end, ioma_slice *seg)
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
    *seg = (ioma_slice){ p, (size_t)(seg_end - p) };
    *at  = seg_end;
    return true;
}

/* The endpoint's whole path: its groups' prefixes, outermost first, then its own path. Written
 * right to left, from the innermost group up, so no list of the groups is needed. */
static char *full_path(const ioma_endpoint *ep)
{
    size_t path_len = strlen(ep->path), len = path_len;
    for (const ioma_group *g = ep->group; g; g = g->parent)
        len += strlen(g->prefix);
    char  *full = must(malloc(len + 1));
    size_t at   = len - path_len;
    memcpy(full + at, ep->path, path_len + 1);              /* the path last, with its NUL */
    for (const ioma_group *g = ep->group; g; g = g->parent) {
        size_t n = strlen(g->prefix);
        at -= n;
        memcpy(full + at, g->prefix, n);
    }
    return full;
}

/* The endpoint's middleware, flat: the root's, each group's outer to inner, then its own. Filled
 * right to left, like the path. */
static void flatten_chain(ioma_endpoint *ep)
{
    int n = ep->n_own;
    for (const ioma_group *g = ep->group; g; g = g->parent)
        n += g->n_mws;
    ep->n_chain = n;
    if (n == 0)
        return;
    ep->chain = must(malloc((size_t)n * sizeof *ep->chain));
    int at = n - ep->n_own;
    memcpy(ep->chain + at, ep->own, (size_t)ep->n_own * sizeof *ep->chain);
    for (const ioma_group *g = ep->group; g; g = g->parent) {
        at -= g->n_mws;
        memcpy(ep->chain + at, g->mws, (size_t)g->n_mws * sizeof *ep->chain);
    }
}

/* The static child for a segment, made if missing. */
static struct node *child(struct node *node, ioma_slice seg)
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

/* Put an endpoint into the tree along its full path; a ':name' segment goes through the capture
 * child and its name is kept with the endpoint. A duplicate keeps the first. */
static void insert(ioma_endpoint *ep)
{
    struct node *node = &g_tree;
    const char  *at = ep->full, *end = ep->full + strlen(ep->full);
    ioma_slice   seg;
    while (next_segment(&at, end, &seg)) {
        if (seg.p[0] == ':') {
            if (ep->n_names == IOMA_MAX_ROUTE_PARAMS) {
                fprintf(stderr, "ioma: %s %s has more than %d captures; ignored\n", ep->method, ep->full, IOMA_MAX_ROUTE_PARAMS);
                return;
            }
            ep->names[ep->n_names++] = (ioma_slice){ seg.p + 1, seg.len - 1 };
            if (!node->param)
                node->param = must(calloc(1, sizeof *node->param));
            node = node->param;
        } else {
            node = child(node, seg);
        }
    }
    for (int i = 0; i < node->n_eps; i++) {
        if (node->eps[i]->method_len == ep->method_len && memcmp(node->eps[i]->method, ep->method, ep->method_len) == 0) {
            fprintf(stderr, "ioma: duplicate route %s %s; keeping the first\n", ep->method, ep->full);
            return;
        }
    }
    ioma_endpoint **eps = must(realloc(node->eps, ((size_t)node->n_eps + 1) * sizeof *eps));
    node->eps = eps;
    node->eps[node->n_eps++] = ep;
}

/* The allow header of every node with endpoints: its methods, comma-separated. */
static void set_allow(struct node *node)
{
    if (node->n_eps) {
        size_t len = 1;
        for (int i = 0; i < node->n_eps; i++)
            len += node->eps[i]->method_len + 2;
        char *at = node->allow = must(malloc(len));
        for (int i = 0; i < node->n_eps; i++) {
            if (i) {
                memcpy(at, ", ", 2);
                at += 2;
            }
            memcpy(at, node->eps[i]->method, node->eps[i]->method_len);
            at += node->eps[i]->method_len;
        }
        *at = '\0';
    }
    for (int i = 0; i < node->n_kids; i++)
        set_allow(node->kids[i]);
    if (node->param)
        set_allow(node->param);
}

/* Resolve everything registered: full paths into the tree, middleware into flat chains. */
void ioma__router_build(void)
{
    if (g_built)
        return;
    g_built = true;
    for (ioma_endpoint *ep = g_first; ep; ep = ep->next) {
        ep->full = full_path(ep);
        insert(ep);
        flatten_chain(ep);
    }
    set_allow(&g_tree);
}

/* ── a request ─────────────────────────────────────────────────────────────────────────── */

/* The endpoint at a node for the method, or nullptr. */
static const ioma_endpoint *endpoint_for(const struct node *node, ioma_slice method)
{
    for (int i = 0; i < node->n_eps; i++)
        if (node->eps[i]->method_len == method.len && memcmp(node->eps[i]->method, method.p, method.len) == 0)
            return node->eps[i];
    return nullptr;
}

/* Walk the tree along the path from at, the segments that capture nodes take going into
 * req->route_params (values; the names come with the endpoint). The static child is tried before
 * the capture, so a static segment wins, and the capture is tried when the static branch comes
 * to nothing - including when it reaches the end without this method. Returns the endpoint for
 * the method, or nullptr; *seen is the first node the path itself reached, for a 405. */
static const ioma_endpoint *walk(const struct node *node, const char *at, const char *end,
                                 ioma_request *req, const struct node **seen)
{
    ioma_slice seg;
    if (!next_segment(&at, end, &seg)) {
        if (node->n_eps && !*seen)
            *seen = node;
        return endpoint_for(node, req->method);
    }
    for (int i = 0; i < node->n_kids; i++) {
        if (same(node->kids[i]->seg, seg)) {
            const ioma_endpoint *ep = walk(node->kids[i], at, end, req, seen);
            if (ep)
                return ep;
            break;                                  /* static children are unique: no other candidate */
        }
    }
    if (node->param && req->n_route_params < IOMA_MAX_ROUTE_PARAMS) {
        size_t mark = req->n_route_params;
        req->route_params[req->n_route_params++].value = seg;
        const ioma_endpoint *ep = walk(node->param, at, end, req, seen);
        if (ep)
            return ep;
        req->n_route_params = mark;
    }
    return nullptr;
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

/* A handler behind a chain; a direct call when the chain is empty. */
static void run(ioma_ctx *ctx, const ioma_mw *mws, int n, ioma_handler fn)
{
    if (n == 0) {
        fn(ctx);
        return;
    }
    ioma_next next = { mws, n, 0, fn };
    ioma_next_run(ctx, &next);
}

/* The built-in fallbacks. */
static void not_found(ioma_ctx *ctx)
{
    ctx->res.status = 404;
    ioma_text(ctx, "404 Not Found\n");
}
static void not_allowed(ioma_ctx *ctx)               /* status and allow are set before its chain */
{
    ioma_text(ctx, "405 Method Not Allowed\n");
}

/* Find the request's endpoint and run it behind its chain; the fallbacks run behind the root's. */
void ioma__dispatch(ioma_ctx *ctx)
{
    ioma_request      *req  = &ctx->req;
    const struct node *seen = nullptr;
    req->n_route_params = 0;
    const ioma_endpoint *ep = walk(&g_tree, req->path.p, req->path.p + req->path.len, req, &seen);
    if (ep) {
        for (size_t i = 0; i < req->n_route_params; i++)
            req->route_params[i].key = ep->names[i];
        run(ctx, ep->chain, ep->n_chain, ep->fn);
        return;
    }
    req->n_route_params = 0;
    if (seen) {                                     /* the path is known, the method is not */
        ctx->res.status = 405;
        ioma_header(ctx, "allow", seen->allow);
        run(ctx, g_root.mws, g_root.n_mws, not_allowed);
        return;
    }
    run(ctx, g_root.mws, g_root.n_mws, g_fallback);
}

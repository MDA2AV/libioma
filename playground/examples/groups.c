/*
 * groups.c - routes in groups: a prefix plus middleware, nesting, an endpoint's own middleware,
 * a fallback of your own - as plain calls for one part of the tree and as the script for the
 * rest, since they register the same thing.
 *
 *     make examples && ./ioxd-example-groups
 *     curl -i http://127.0.0.1:8080/legacy/ping
 *     curl -i http://127.0.0.1:8080/api/v1/users/42
 *     curl -i http://127.0.0.1:8080/api/v1/admin/stats            # 403 from the group's gate
 *     curl -i -H 'X-Admin: 1' http://127.0.0.1:8080/api/v1/admin/stats
 *     curl -i http://127.0.0.1:8080/nowhere                       # the fallback, as JSON
 *     curl -i -X DELETE http://127.0.0.1:8080/api/v1/users/42     # 405 with an allow header
 *
 * ioxd_run resolves it all once: every full path into a segment tree, every endpoint's
 * middleware - the root's, then each group's from the outside in, then its own - into one flat
 * chain. A request costs one walk and no scan.
 */
#include <ioxd.h>

/* --- middleware --- */

static void api_version(ioxd_ctx *ctx, ioxd_next *next)
{
    ioxd_header(ctx, "x-api-version", "1");
    ioxd_next_run(ctx, next);
}

static void require_admin(ioxd_ctx *ctx, ioxd_next *next)
{
    for (size_t i = 0; i < ctx->req.n_headers; i++)
        if (ioxd_slice_eq(ctx->req.headers[i].key, "x-admin")) {
            ioxd_next_run(ctx, next);
            return;
        }
    ctx->res.status = 403;
    ioxd_text(ctx, "admins only\n");
}

static void no_cache(ioxd_ctx *ctx, ioxd_next *next)
{
    ioxd_header(ctx, "cache-control", "no-store");
    ioxd_next_run(ctx, next);
}

/* --- handlers --- */

static void ping(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "pong\n");
}

static void user(ioxd_ctx *ctx)
{
    ioxd_slice id = ctx->req.route_params[0].value;
    ioxd_printf(ctx, "user %.*s\n", (int)id.len, id.p);
}

static void update_user(ioxd_ctx *ctx)
{
    ioxd_slice id = ctx->req.route_params[0].value;
    ioxd_printf(ctx, "user %.*s updated\n", (int)id.len, id.p);
}

static void stats(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "all good\n");
}

/* The fallback for what no route matches; a path that matches without the method is a built-in
 * 405 with an allow header, whatever this does. */
static void not_found(ioxd_ctx *ctx)
{
    ctx->res.status = 404;
    ioxd_content_type(ctx, "application/json");
    ioxd_printf(ctx, "{\"error\":\"no %.*s here\"}", (int)ctx->req.path.len, ctx->req.path.p);
}

int main(void)
{
    /* as calls: a group is a handle, an endpoint too */
    ioxd_group *legacy = ioxd_group_new(NULL, "/legacy");
    ioxd_group_use(legacy, no_cache);
    ioxd_get(legacy, "/ping", ping);                 /* GET /legacy/ping, behind no_cache */

    /* as a script: the block after IOXD_GROUP is the group, and it nests */
    IOXD_GROUP("/api", api_version) {
        IOXD_GROUP("/v1") {
            IOXD_GET ("/users/:id", user);
            IOXD_POST("/users/:id", update_user, no_cache);     /* this endpoint's own middleware */
            IOXD_GROUP("/admin", require_admin) {
                IOXD_GET("/stats", stats);           /* /api/v1/admin/stats: api_version, require_admin, then stats */
            }
        }
    }
    IOXD_DEFAULT(not_found);

    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}

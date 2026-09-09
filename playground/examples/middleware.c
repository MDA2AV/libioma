/*
 * middleware.c - middleware written by hand: one that stamps a header on every reply, one that
 * times the request around the rest of the chain, one that gates a route and short-circuits,
 * and how a middleware hands what it found to the handler.
 *
 *     make examples && ./ioxd-example-middleware
 *     curl -i http://127.0.0.1:8080/public
 *     curl -i http://127.0.0.1:8080/whoami                              # 401
 *     curl -i -H 'Authorization: Bearer secret' http://127.0.0.1:8080/whoami
 *
 * A middleware is a function of the context and the rest of the chain: whatever it does before
 * ioxd_next_run runs before the handler, whatever it does after runs after, and not calling
 * ioxd_next_run at all is the short-circuit. Headers meant for a streamed reply must be added
 * before ioxd_next_run - the head may already be on the wire afterwards.
 */
#include <ioxd.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

/* Every reply gets an x-request-id, counted across workers. Before ioxd_next_run, so a reply that
 * streams has it too; ioxd_header copies the value, so the local buffer is fine. */
static void request_id(ioxd_ctx *ctx, ioxd_next *next)
{
    static _Atomic unsigned long counter;
    char id[32];
    snprintf(id, sizeof id, "%lu", atomic_fetch_add(&counter, 1) + 1);
    ioxd_header(ctx, "x-request-id", id);
    ioxd_next_run(ctx, next);
}

/* The request, its status and how long the chain took, on stderr, once the chain has run. */
static void timing(ioxd_ctx *ctx, ioxd_next *next)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ioxd_next_run(ctx, next);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    fprintf(stderr, "%.*s %.*s -> %d in %ld us\n", (int)ctx->req.method.len, ctx->req.method.p,
            (int)ctx->req.path.len, ctx->req.path.p, ctx->res.status, us);
}

/* What the gate found, for the handler: on the middleware's own frame, which outlives the chain. */
struct principal {
    const char *name;
    bool        admin;
};

/* The gate: a bearer token, or a 401 and no handler at all. */
static void auth(ioxd_ctx *ctx, ioxd_next *next)
{
    ioxd_slice token = { NULL, 0 };
    for (size_t i = 0; i < ctx->req.n_headers; i++)      /* names arrive lower-cased */
        if (ioxd_slice_eq(ctx->req.headers[i].key, "authorization"))
            token = ctx->req.headers[i].value;
    if (!ioxd_slice_eq(token, "Bearer secret")) {
        ctx->res.status = 401;
        ioxd_header(ctx, "www-authenticate", "Bearer");
        ioxd_text(ctx, "a bearer token, please\n");
        return;                                      /* no ioxd_next_run: the handler never runs */
    }
    struct principal who = { .name = "diogo", .admin = true };
    ctx->user = &who;                                /* handed down; valid until the chain returns */
    ioxd_next_run(ctx, next);
}

static void public_route(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "for everyone\n");
}

static void whoami(ioxd_ctx *ctx)
{
    const struct principal *who = ctx->user;
    ioxd_printf(ctx, "%s%s\n", who->name, who->admin ? " (admin)" : "");
}

int main(void)
{
    IOXD_USE(request_id);                            /* root middleware: every request, the 404s too */
    IOXD_USE(timing);
    IOXD_GET("/public", public_route);
    IOXD_GET("/whoami", whoami, auth);               /* this endpoint's own middleware, after the root's */
    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}

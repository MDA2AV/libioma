/*
 * tests/server.c - the server smoke.py and stress.py talk to: one route per feature of the
 * request/response model. `make check` builds it, runs both suites against it and stops it.
 */
#include <ioma.h>

#include <stdlib.h>

/* GET / */
static void home(ioma_ctx *c)
{
    ioma_text(c, "hello from ioma\n");                 /* 200, text/plain: the defaults */
}

/* GET /health */
static void health(ioma_ctx *c)
{
    ioma_text(c, "ok");
}

/* GET /whoami?x=1&y=2 - everything the request carries, read straight from its fields and
 * arrays: the raw query, then each parameter split and decoded, then every header (names are
 * lower-cased). ioma_printf formats straight into the reply buffer. */
static void whoami(ioma_ctx *c)
{
    ioma_request *r = &c->req;
    ioma_printf(c, "method = %.*s\npath   = %.*s\nquery  = %.*s\nkeep-alive = %s\n",
                (int)r->method.len, r->method.p,
                (int)r->path.len,   r->path.p,
                (int)r->query.len,  r->query.p,
                r->keep_alive ? "yes" : "no");

    for (size_t i = 0; i < r->n_params; i++)
        ioma_printf(c, "param  %.*s = %.*s\n",
                    (int)r->params[i].key.len,   r->params[i].key.p,
                    (int)r->params[i].value.len, r->params[i].value.p);

    for (size_t i = 0; i < r->n_headers; i++)
        ioma_printf(c, "header %.*s: %.*s\n",
                    (int)r->headers[i].key.len,   r->headers[i].key.p,
                    (int)r->headers[i].value.len, r->headers[i].value.p);

    ioma_header(c, "X-Powered-By", "ioma");             /* fine: nothing has gone out yet */
}

/* GET /users/:id?fields=... - the :id capture is req.route_params[0]. A query parameter is found by
 * walking req.params: there are few, so the loop is the lookup. */
static void user(ioma_ctx *c)
{
    ioma_slice id     = c->req.route_params[0].value;
    ioma_slice fields = { "", 0 };
    for (size_t i = 0; i < c->req.n_params; i++)
        if (ioma_slice_eq(c->req.params[i].key, "fields"))
            fields = c->req.params[i].value;
    ioma_printf(c, "user %.*s fields=%.*s\n", (int)id.len, id.p, (int)fields.len, fields.p);
}

/* GET /users/:id/posts/:post - captures come in pattern order; ioma_slice_int reads a number. */
static void post(ioma_ctx *c)
{
    long user_id = ioma_slice_int(c->req.route_params[0].value);
    long post_id = ioma_slice_int(c->req.route_params[1].value);
    ioma_printf(c, "post %ld of user %ld\n", post_id, user_id);
}

/* POST /echo - the body read whole (Content-Length or chunked, decoded) and sent back with the
 * same content type. The reply's content type is a slice, so the request's header value is
 * assigned as is; no copy. */
static void echo(ioma_ctx *c)
{
    ioma_slice body = ioma_body(c);
    ioma_content_type(c, "application/octet-stream");
    for (size_t i = 0; i < c->req.n_headers; i++)
        if (ioma_slice_eq(c->req.headers[i].key, "content-type"))
            c->res.content_type = c->req.headers[i].value;
    ioma_write(c, body.p, body.len);
}

/* POST /greet with a form body (name=...) - ioma_kv_parse splits and decodes it like a query
 * string. The decoded values only need to live while the handler runs, so a local arena will do:
 * the reply is copied into the sink as it is written. */
static void greet(ioma_ctx *c)
{
    ioma_slice body = ioma_body(c);
    ioma_kv    form[8];
    char       arena[512];
    size_t     n = ioma_kv_parse(body.p, body.len, form, 8, arena, sizeof arena);

    ioma_slice name = { "stranger", 8 };
    for (size_t i = 0; i < n; i++)
        if (ioma_slice_eq(form[i].key, "name"))
            name = form[i].value;
    ioma_printf(c, "hello %.*s\n", (int)name.len, name.p);
}

/* GET /stream?n=lines - a body far larger than the buffer. Nothing special to do: once the
 * buffer fills, the framework sends the head and streams the rest (chunked on HTTP/1.1), and
 * each write that reaches the wire just suspends this handler until the send completes. */
static void stream(ioma_ctx *c)
{
    long n = 1000;
    for (size_t i = 0; i < c->req.n_params; i++)
        if (ioma_slice_eq(c->req.params[i].key, "n"))
            n = ioma_slice_int(c->req.params[i].value);
    for (long i = 1; i <= n; i++)
        ioma_printf(c, "line %ld of %ld\n", i, n);
}

/* POST /upload - a body of any size, streamed: each ioma_body_read hands over the next bytes
 * straight from the wire (suspending the handler while they arrive), nothing is buffered. A
 * handler that never asks for the body does not pay for it either: the framework drains it. */
static void upload(ioma_ctx *c)
{
    char   chunk[4096];
    size_t total = 0;
    int    n;
    while ((n = ioma_body_read(c, chunk, sizeof chunk)) > 0)
        total += (size_t)n;
    ioma_printf(c, "%zu bytes\n", total);
}

/* Middleware: stamps a Server header, then runs the rest of the chain. Setting headers before
 * calling ioma_next_run means they land even on a reply that streams (afterwards the head may
 * already be on the wire); c->status and the rest are there to inspect on the way back out.
 * One that wanted to block a request (auth, rate limit) would write its reply and return
 * without calling ioma_next_run. */
static void add_server(ioma_ctx *c, ioma_next *next)
{
    ioma_header(c, "Server", "ioma");
    ioma_next_run(c, next);
}

/* The fallback for anything unrouted, replacing the built-in text 404. */
static void not_found(ioma_ctx *c)
{
    c->res.status = 404;
    ioma_content_type(c, "application/json");
    ioma_text(c, "{\"error\":\"not found\"}");
}

int main(void)
{
    ioma_use(add_server);                        /* global middleware, runs on every request */

    ioma_route("GET",  "/",                      home);
    ioma_route("GET",  "/health",                health);
    ioma_route("GET",  "/whoami",                whoami);
    ioma_route("GET",  "/users/:id",             user);
    ioma_route("GET",  "/users/:id/posts/:post", post);
    ioma_route("POST", "/echo",                  echo);
    ioma_route("POST", "/greet",                 greet);
    ioma_route("GET",  "/stream",                stream);
    ioma_route("POST", "/upload",                upload);
    ioma_default(not_found);

    int workers = 0;                             /* 0: one per core */
    const char *env = getenv("IOMA_WORKERS");
    if (env && atoi(env) > 0)
        workers = atoi(env);

    int port = 8080;
    env = getenv("IOMA_PORT");
    if (env && atoi(env) > 0 && atoi(env) < 65536)
        port = atoi(env);

    return ioma_run(workers, port);
}

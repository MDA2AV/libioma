/*
 * tests/server.c - the server smoke.py and stress.py talk to: one route per feature of the
 * request/response model. `make check` builds it, runs both suites against it and stops it.
 */
#include <ioxd.h>

#include <stdlib.h>

/* GET / */
static void home(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "hello from ioxd\n");                 /* 200, text/plain: the defaults */
}

/* GET /health */
static void health(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "ok");
}

/* GET /whoami?x=1&y=2 - everything the request carries, read straight from its fields and
 * arrays: the raw query, then each parameter split and decoded, then every header (names are
 * lower-cased). ioxd_printf formats straight into the reply buffer. */
static void whoami(ioxd_ctx *ctx)
{
    ioxd_request *r = &ctx->req;
    ioxd_printf(ctx, "method = %.*s\npath   = %.*s\nquery  = %.*s\nkeep-alive = %s\n",
                (int)r->method.len, r->method.p,
                (int)r->path.len,   r->path.p,
                (int)r->query.len,  r->query.p,
                r->keep_alive ? "yes" : "no");

    for (size_t i = 0; i < r->n_params; i++)
        ioxd_printf(ctx, "param  %.*s = %.*s\n",
                    (int)r->params[i].key.len,   r->params[i].key.p,
                    (int)r->params[i].value.len, r->params[i].value.p);

    for (size_t i = 0; i < r->n_headers; i++)
        ioxd_printf(ctx, "header %.*s: %.*s\n",
                    (int)r->headers[i].key.len,   r->headers[i].key.p,
                    (int)r->headers[i].value.len, r->headers[i].value.p);

    ioxd_header(ctx, "X-Powered-By", "ioxd");             /* fine: nothing has gone out yet */
}

/* GET /users/:id?fields=... - the :id capture is req.route_params[0]. A query parameter is found by
 * walking req.params: there are few, so the loop is the lookup. */
static void user(ioxd_ctx *ctx)
{
    ioxd_slice id     = ctx->req.route_params[0].value;
    ioxd_slice fields = { "", 0 };
    for (size_t i = 0; i < ctx->req.n_params; i++)
        if (ioxd_slice_eq(ctx->req.params[i].key, "fields"))
            fields = ctx->req.params[i].value;
    ioxd_printf(ctx, "user %.*s fields=%.*s\n", (int)id.len, id.p, (int)fields.len, fields.p);
}

/* GET /users/:id/posts/:post - captures come in pattern order; ioxd_to_i64 reads a whole number
 * or fails, so a non-numeric id is a 400 instead of a silent zero. */
static void post(ioxd_ctx *ctx)
{
    int64_t user_id, post_id;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &user_id) ||
        !ioxd_to_i64(ctx->req.route_params[1].value, &post_id)) {
        ctx->res.status = 400;
        ioxd_text(ctx, "ids must be integers\n");
        return;
    }
    ioxd_printf(ctx, "post %lld of user %lld\n", (long long)post_id, (long long)user_id);
}

/* GET /convert?i=..&d=..&b=.. - the typed conversions; a value that does not parse is a 400. */
static void convert(ioxd_ctx *ctx)
{
    for (size_t k = 0; k < ctx->req.n_params; k++) {
        ioxd_kv p = ctx->req.params[k];
        int64_t i;
        double  d;
        bool    b;
        if (ioxd_slice_eq(p.key, "i") && ioxd_to_i64(p.value, &i)) {
            ioxd_printf(ctx, "i=%lld\n", (long long)i);
        } else if (ioxd_slice_eq(p.key, "d") && ioxd_to_double(p.value, &d)) {
            ioxd_printf(ctx, "d=%g\n", d);
        } else if (ioxd_slice_eq(p.key, "b") && ioxd_to_bool(p.value, &b)) {
            ioxd_printf(ctx, "b=%s\n", b ? "true" : "false");
        } else {
            ctx->res.status = 400;
            ioxd_printf(ctx, "bad %.*s\n", (int)p.key.len, p.key.p);
            return;
        }
    }
}

/* POST /echo - the body read whole (Content-Length or chunked, decoded) and sent back with the
 * same content type. The reply's content type is a slice, so the request's header value is
 * assigned as is; no copy. */
static void echo(ioxd_ctx *ctx)
{
    ioxd_slice body = ioxd_body_all(ctx);
    ioxd_content_type(ctx, "application/octet-stream");
    for (size_t i = 0; i < ctx->req.n_headers; i++)
        if (ioxd_slice_eq(ctx->req.headers[i].key, "content-type"))
            ctx->res.content_type = ctx->req.headers[i].value;
    ioxd_write(ctx, body.p, body.len);
}

/* POST /greet with a form body (name=...) - ioxd_kv_parse splits and decodes it like a query
 * string. The decoded values only need to live while the handler runs, so a local arena will do:
 * the reply is copied into the sink as it is written. */
static void greet(ioxd_ctx *ctx)
{
    ioxd_slice body = ioxd_body_all(ctx);
    ioxd_kv    form[8];
    char       arena[512];
    size_t     n = ioxd_kv_parse(body.p, body.len, form, 8, arena, sizeof arena);

    ioxd_slice name = { "stranger", 8 };
    for (size_t i = 0; i < n; i++)
        if (ioxd_slice_eq(form[i].key, "name"))
            name = form[i].value;
    ioxd_printf(ctx, "hello %.*s\n", (int)name.len, name.p);
}

/* GET /stream?n=lines - a body far larger than the buffer. Nothing special to do: once the
 * buffer fills, the framework sends the head and streams the rest (chunked on HTTP/1.1), and
 * each write that reaches the wire just suspends this handler until the send completes. */
static void stream(ioxd_ctx *ctx)
{
    int64_t n = 1000;
    for (size_t i = 0; i < ctx->req.n_params; i++)
        if (ioxd_slice_eq(ctx->req.params[i].key, "n"))
            ioxd_to_i64(ctx->req.params[i].value, &n);
    for (int64_t i = 1; i <= n; i++)
        ioxd_printf(ctx, "line %lld of %lld\n", (long long)i, (long long)n);
}

/* POST /upload - a body of any size, streamed: each ioxd_body_read_until hands over the next bytes
 * straight from the wire (suspending the handler while they arrive), nothing is buffered. A
 * handler that never asks for the body does not pay for it either: the framework drains it. */
static void upload(ioxd_ctx *ctx)
{
    char   chunk[4096];
    size_t total = 0;
    int    n, reads = 0;
    while ((n = ioxd_body_read_until(ctx, chunk, sizeof chunk)) > 0) {
        total += (size_t)n;
        reads++;
    }
    ioxd_printf(ctx, "%zu bytes in %d reads\n", total, reads);
}

/* POST /chunks[?first=N] - the body chunk by chunk, exactly as the client framed it: each chunk's
 * length and bytes on a line, "end" after the last. With first=N, N bytes are streamed off first
 * and the chunk read then gives the rest of the chunk they came from. A chunk that does not fit
 * the buffer is a 413 (the engine answers it); a body that is not chunked is a 400. */
static void chunks(ioxd_ctx *ctx)
{
    char    buf[1024];
    int64_t first = 0;
    for (size_t i = 0; i < ctx->req.n_params; i++)
        if (ioxd_slice_eq(ctx->req.params[i].key, "first"))
            ioxd_to_i64(ctx->req.params[i].value, &first);
    if (first > 0 && first <= (int64_t)sizeof buf)
        ioxd_printf(ctx, "first=%d\n", ioxd_body_read_until(ctx, buf, (size_t)first));
    int n;
    while ((n = ioxd_body_read_next_chunk(ctx, buf, sizeof buf)) > 0)
        ioxd_printf(ctx, "%d:%.*s\n", n, n, buf);
    if (n == 0) {
        ioxd_text(ctx, "end\n");
    } else if (ctx->res.status == 200) {                     /* -1 without a status: not chunked */
        ctx->res.status = 400;
        ioxd_text(ctx, "not chunked\n");
    }
}

/* GET /users/new - a static segment beside the :id capture; the static one wins for GET. */
static void new_user_form(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "new user form\n");
}

/* POST /users/:id - and POST /users/new lands here, since the static segment has no POST. */
static void update_user(ioxd_ctx *ctx)
{
    ioxd_slice id = ctx->req.route_params[0].value;
    ioxd_printf(ctx, "updated %.*s\n", (int)id.len, id.p);
}

/* GET /api/ping and GET /api/admin/stats - endpoints in groups; the prefixes come from the groups. */
static void ping(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "pong\n");
}
static void stats(ioxd_ctx *ctx)
{
    ioxd_text(ctx, "stats\n");
}

/* Middleware on the /api group: every reply below it carries the header. */
static void api_header(ioxd_ctx *ctx, ioxd_next *next)
{
    ioxd_header(ctx, "x-api", "v1");
    ioxd_next_run(ctx, next);
}

/* Middleware on /api/admin: the token, or a 401 without running what is below. */
static void require_token(ioxd_ctx *ctx, ioxd_next *next)
{
    for (size_t i = 0; i < ctx->req.n_headers; i++) {
        if (ioxd_slice_eq(ctx->req.headers[i].key, "x-token") && ioxd_slice_eq(ctx->req.headers[i].value, "secret")) {
            ioxd_next_run(ctx, next);
            return;
        }
    }
    ctx->res.status = 401;
    ioxd_text(ctx, "token required\n");
}

/* Middleware on one endpoint only. */
static void endpoint_header(ioxd_ctx *ctx, ioxd_next *next)
{
    ioxd_header(ctx, "x-endpoint", "stats");
    ioxd_next_run(ctx, next);
}

/* GET /json/:id - a small document, written as you go into the reply. */
static void json_item(ioxd_ctx *ctx)
{
    int64_t id;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &id)) {
        ctx->res.status = 400;
        ioxd_text(ctx, "id must be an integer\n");
        return;
    }
    ioxd_json j = ioxd_json_reply(ctx);
    ioxd_json_object(&j);
    ioxd_json_key(&j, "id");    ioxd_json_int(&j, id);
    ioxd_json_key(&j, "name");  ioxd_json_cstr(&j, "Zo\xc3\xab \"Z\" O'Neil\n");
    ioxd_json_key(&j, "ratio"); ioxd_json_double(&j, 0.1);
    ioxd_json_key(&j, "ok");    ioxd_json_bool(&j, true);
    ioxd_json_key(&j, "none");  ioxd_json_null(&j);
    ioxd_json_key(&j, "tags");  ioxd_json_array(&j);
        ioxd_json_cstr(&j, "a");
        ioxd_json_cstr(&j, "b");
    ioxd_json_end(&j);
    ioxd_json_end(&j);
}

/* GET /json/big?n=N - N objects in an array, far more than the slab holds: it streams, chunked. */
static void json_big(ioxd_ctx *ctx)
{
    int64_t n = 2000;
    for (size_t i = 0; i < ctx->req.n_params; i++)
        if (ioxd_slice_eq(ctx->req.params[i].key, "n"))
            ioxd_to_i64(ctx->req.params[i].value, &n);
    ioxd_json j = ioxd_json_reply(ctx);
    ioxd_json_array(&j);
    for (int64_t i = 0; i < n; i++) {
        ioxd_json_object(&j);
        ioxd_json_key(&j, "i");  ioxd_json_int(&j, i);
        ioxd_json_key(&j, "sq"); ioxd_json_int(&j, i * i);
        ioxd_json_end(&j);
    }
    ioxd_json_end(&j);
}

/* Middleware: stamps a Server header, then runs the rest of the chain. Setting headers before
 * calling ioxd_next_run means they land even on a reply that streams (afterwards the head may
 * already be on the wire); c->status and the rest are there to inspect on the way back out.
 * One that wanted to block a request (auth, rate limit) would write its reply and return
 * without calling ioxd_next_run. */
static void add_server(ioxd_ctx *ctx, ioxd_next *next)
{
    ioxd_header(ctx, "Server", "ioxd");
    ioxd_next_run(ctx, next);
}

/* The fallback for anything unrouted, replacing the built-in text 404. */
static void not_found(ioxd_ctx *ctx)
{
    ctx->res.status = 404;
    ioxd_content_type(ctx, "application/json");
    ioxd_text(ctx, "{\"error\":\"not found\"}");
}

/* An environment variable as a number, or the fallback when unset or not a whole number. */
static long env_number(const char *name, long fallback)
{
    const char *text = getenv(name);
    if (!text)
        return fallback;
    char *end;
    long  n = strtol(text, &end, 10);
    return *end == '\0' && end != text ? n : fallback;
}

int main(void)
{

    /* the routes as a script: outside any IOXD_GROUP block this is the root */
    IOXD_USE(add_server);                        /* root middleware: every request */
    IOXD_GET ("/",                      home);
    IOXD_GET ("/health",                health);
    IOXD_GET ("/whoami",                whoami);
    IOXD_GET ("/users/:id",             user);
    IOXD_GET ("/users/new",             new_user_form);     /* static beside the capture: wins for GET */
    IOXD_POST("/users/:id",             update_user);       /* so POST /users/new falls through to :id */
    IOXD_GET ("/users/:id/posts/:post", post);
    IOXD_GET ("/convert",               convert);
    IOXD_POST("/echo",                  echo);
    IOXD_POST("/greet",                 greet);
    IOXD_GET ("/stream",                stream);
    IOXD_POST("/upload",                upload);
    IOXD_POST("/chunks",                chunks);
    IOXD_GET ("/json/:id",              json_item);
    IOXD_GET ("/json/big",              json_big);          /* static beside the capture */
    IOXD_DEFAULT(not_found);

    /* groups: /api with middleware of its own (IOXD_USE inside a block adds to that group; listing
     * it after the prefix, as /admin does, is the same), /api/admin below it gated by a token, and
     * one endpoint with middleware for itself only, listed after its handler */
    IOXD_GROUP("/api") {
        IOXD_USE(api_header);
        IOXD_GET("/ping", ping);
        IOXD_GROUP("/admin", require_token) {
            IOXD_GET("/stats", stats, endpoint_header);
        }
    }

    int workers = (int)env_number("IOXD_WORKERS", 0);          /* 0: one per core */
    int port    = (int)env_number("IOXD_PORT", 8080);
    return ioxd_run(workers, port > 0 && port < 65536 ? port : 8080);
}

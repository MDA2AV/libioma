/*
 * playground/hello - a complete libioma server. Endpoints are plain functions: take a request,
 * return a response. Everything in the request is a slice (pointer + length) into the read
 * buffer, and headers, query parameters and route parameters are arrays of key/value slices
 * that you read directly. The framework parses, routes, serializes and flushes.
 *
 *     make && ./ioma-hello                              # one worker per core, port 8080
 *     curl http://127.0.0.1:8080/
 *     curl 'http://127.0.0.1:8080/whoami?x=1&y=2'
 *     curl 'http://127.0.0.1:8080/users/42?fields=a%20b'
 *     curl http://127.0.0.1:8080/users/42/posts/7
 *     curl -d 'hello' http://127.0.0.1:8080/echo
 *     curl -d 'name=diogo' http://127.0.0.1:8080/greet
 *
 * Against an installed libioma:  cc hello.c $(pkg-config --cflags --libs ioma) -o hello
 */
#include <ioma.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Append printf output to the request's scratch buffer at offset len; returns the new length.
 * A handler that builds its body piece by piece uses this, then hands scratch to ioma_bytes. */
static size_t append(ioma_request *req, size_t len, const char *fmt, ...)
{
    if (len >= req->scratch_cap)
        return len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(req->scratch + len, req->scratch_cap - len, fmt, ap);
    va_end(ap);
    if (n < 0)
        return len;
    size_t end = len + (size_t)n;
    return end < req->scratch_cap ? end : req->scratch_cap - 1;
}

/* GET / - a string literal is valid for the whole program, so returning it is fine. */
static ioma_response home(ioma_request *req)
{
    (void)req;
    return ioma_text(200, "hello from ioma\n");
}

/* GET /health */
static ioma_response health(ioma_request *req)
{
    (void)req;
    return ioma_text(200, "ok");
}

/* GET /whoami?x=1&y=2 - everything the request carries, read straight from its fields and
 * arrays: the raw query, then each parameter split and decoded, then every header (names are
 * lower-cased). */
static ioma_response whoami(ioma_request *req)
{
    size_t n = append(req, 0, "method = %.*s\npath   = %.*s\nquery  = %.*s\nkeep-alive = %s\n",
                      (int)req->method.len, req->method.p,
                      (int)req->path.len,   req->path.p,
                      (int)req->query.len,  req->query.p,
                      req->keep_alive ? "yes" : "no");

    for (size_t i = 0; i < req->n_params; i++)
        n = append(req, n, "param  %.*s = %.*s\n",
                   (int)req->params[i].key.len,   req->params[i].key.p,
                   (int)req->params[i].value.len, req->params[i].value.p);

    for (size_t i = 0; i < req->n_headers; i++)
        n = append(req, n, "header %.*s: %.*s\n",
                   (int)req->headers[i].key.len,   req->headers[i].key.p,
                   (int)req->headers[i].value.len, req->headers[i].value.p);

    ioma_response res = ioma_bytes(200, "text/plain", req->scratch, n);
    ioma_header_set(&res, "X-Powered-By", "ioma");
    return res;
}

/* GET /users/:id?fields=... - the :id capture is req->route[0]. A query parameter is found by
 * walking req->params: there are few, so the loop is the lookup. */
static ioma_response user(ioma_request *req)
{
    ioma_slice id     = req->route[0].value;
    ioma_slice fields = { "", 0 };
    for (size_t i = 0; i < req->n_params; i++)
        if (ioma_slice_eq(req->params[i].key, "fields"))
            fields = req->params[i].value;
    return ioma_textf(req, 200, "user %.*s fields=%.*s\n",
                      (int)id.len, id.p, (int)fields.len, fields.p);
}

/* GET /users/:id/posts/:post - captures come in pattern order; ioma_slice_int reads a number. */
static ioma_response post(ioma_request *req)
{
    long user_id = ioma_slice_int(req->route[0].value);
    long post_id = ioma_slice_int(req->route[1].value);
    return ioma_textf(req, 200, "post %ld of user %ld\n", post_id, user_id);
}

/* POST /echo - the body, Content-Length or chunked, already decoded, sent back with the same
 * content type. A header value is a slice; ioma_bytes wants a C string, so it is copied into
 * scratch with a terminator. The body itself points into the read buffer, still alive when the
 * reply is serialized. */
static ioma_response echo(ioma_request *req)
{
    const char *type = "application/octet-stream";
    for (size_t i = 0; i < req->n_headers; i++) {
        if (ioma_slice_eq(req->headers[i].key, "content-type")) {
            snprintf(req->scratch, req->scratch_cap, "%.*s",
                     (int)req->headers[i].value.len, req->headers[i].value.p);
            type = req->scratch;
        }
    }
    return ioma_bytes(200, type, req->body.p, req->body.len);
}

/* POST /greet with a form body (name=...) - ioma_kv_parse splits and decodes it like a query
 * string. The decoded values only need to live while the handler runs, so a local arena will do;
 * the reply is formatted into scratch, which outlives the handler. */
static ioma_response greet(ioma_request *req)
{
    ioma_kv form[8];
    char    arena[512];
    size_t  n = ioma_kv_parse(req->body.p, req->body.len, form, 8, arena, sizeof arena);

    ioma_slice name = { "stranger", 8 };
    for (size_t i = 0; i < n; i++)
        if (ioma_slice_eq(form[i].key, "name"))
            name = form[i].value;
    return ioma_textf(req, 200, "hello %.*s\n", (int)name.len, name.p);
}

/* Middleware: runs the handler, then stamps a Server header on whatever it returned. One that
 * wanted to block a request (auth, rate limit) would return its own response instead of calling
 * ioma_next_run. */
static ioma_response add_server(ioma_request *req, ioma_next *next)
{
    ioma_response res = ioma_next_run(req, next);
    ioma_header_set(&res, "Server", "ioma");
    return res;
}

/* The fallback for anything unrouted, replacing the built-in text 404. */
static ioma_response not_found(ioma_request *req)
{
    (void)req;
    return ioma_json(404, "{\"error\":\"not found\"}");
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

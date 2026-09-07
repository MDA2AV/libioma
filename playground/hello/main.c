/*
 * playground/hello - a complete ioma HTTP server, built against libioma. Endpoints are plain
 * functions: take a request, return a response. The framework parses, routes, serializes and
 * flushes; the flush suspends the connection's coroutine until io_uring says the send completed.
 *
 *     make && ./ioma-hello              # 4 workers on :8080
 *     curl http://127.0.0.1:8080/
 *     curl http://127.0.0.1:8080/whoami?x=1
 *     curl 'http://127.0.0.1:8080/users/42?fields=a%20b'
 *     curl -d 'hello' http://127.0.0.1:8080/echo
 *
 * Building against an installed libioma instead:
 *     cc hello.c $(pkg-config --cflags --libs ioma) -o hello
 */
#include <ioma.h>

#include <stdlib.h>

/* GET / - a string literal body: valid for the whole program, so returning it is fine. */
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

/* GET /whoami - a dynamic body. ioma_textf formats into req->scratch, which lives long enough
 * (it is the serve loop's buffer) to survive until the framework sends the reply. */
static ioma_response whoami(ioma_request *req)
{
    ioma_response res = ioma_textf(req, 200,
        "method = %.*s\npath   = %.*s\nquery  = %.*s\nkeep-alive = %s\n",
        (int)req->method.len, req->method.p,
        (int)req->path.len,   req->path.p,
        (int)req->query.len,  req->query.p,
        req->keep_alive ? "yes" : "no");
    ioma_header_set(&res, "X-Powered-By", "ioma");
    return res;
}

/* POST /echo - reflect the request body back. req->body points into the read buffer, which is
 * still alive when the framework serializes the reply, so pointing at it is safe. */
static ioma_response echo(ioma_request *req)
{
    return ioma_bytes(200, "application/octet-stream", req->body.p, req->body.len);
}

/* GET /users/:id?fields=... - the route parameter is req->route[0] (the pattern's only capture);
 * the query parameters are in req->params, already split and percent-decoded. */
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

/* Middleware: runs the handler, then stamps a Server header on whatever it returned. A middleware
 * that wanted to block the request (auth, rate limit) would return its own response here instead
 * of calling ioma_next_run. */
static ioma_response add_server(ioma_request *req, ioma_next *next)
{
    ioma_response res = ioma_next_run(req, next);
    ioma_header_set(&res, "Server", "ioma");
    return res;
}

int main(void)
{
    ioma_use(add_server);   /* global middleware, runs on every request */

    ioma_route("GET",  "/",       home);
    ioma_route("GET",  "/health", health);
    ioma_route("GET",  "/whoami", whoami);
    ioma_route("POST", "/echo",   echo);
    ioma_route("GET",  "/users/:id", user);
    /* anything else falls through to the built-in 404 (override with ioma_default) */

    int workers = 4;
    const char *env = getenv("IOMA_WORKERS");
    if (env) {
        int v = atoi(env);
        if (v > 0)
            workers = v;
    }

    int port = 8080;
    env = getenv("IOMA_PORT");
    if (env) {
        int v = atoi(env);
        if (v > 0 && v < 65536)
            port = v;
    }

    return ioma_run(workers, port);
}

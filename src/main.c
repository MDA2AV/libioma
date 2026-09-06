/*
 * main.c - an ioma HTTP server. Endpoints are plain functions: take a request, return a response.
 * The framework parses, routes, serializes and flushes; the flush suspends the connection's
 * coroutine until io_uring says the send completed.
 *
 *     make && ./ioma            # 4 workers on :8080
 *     curl http://127.0.0.1:8080/
 *     curl http://127.0.0.1:8080/whoami?x=1
 *     curl -d 'hello' http://127.0.0.1:8080/echo
 *
 * The raw byte-level proactor API (no HTTP) is still there in proactor.h if you want it.
 */
#include "http.h"

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
        (int)req->method_len, req->method,
        (int)req->path_len,   req->path,
        (int)req->query_len,  req->query ? req->query : "",
        req->keep_alive ? "yes" : "no");
    ioma_header_set(&res, "X-Powered-By", "ioma");
    return res;
}

/* POST /echo - reflect the request body back. req->body points into the read buffer, which is
 * still alive when the framework serializes the reply, so pointing at it is safe. */
static ioma_response echo(ioma_request *req)
{
    return ioma_bytes(200, "application/octet-stream", req->body, req->body_len);
}

int main(void)
{
    ioma_route("GET",  "/",       home);
    ioma_route("GET",  "/health", health);
    ioma_route("GET",  "/whoami", whoami);
    ioma_route("POST", "/echo",   echo);
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

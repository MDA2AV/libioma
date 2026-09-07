/*
 * playground/hello - the smallest libioma server: one route, one worker per core.
 *
 *     make && ./ioma-hello
 *     curl http://127.0.0.1:8080/hello/diogo
 *
 * Against an installed libioma:  cc main.c $(pkg-config --cflags --libs ioma) -o hello
 */
#include <ioma.h>

/* GET /hello/:name - the capture is the first route parameter; the reply goes into the slab. */
static void hello(ioma_ctx *c)
{
    ioma_slice name = c->req.route[0].value;
    ioma_printf(c, "hello %.*s\n", (int)name.len, name.p);
}

int main(void)
{
    ioma_route("GET", "/hello/:name", hello);
    return ioma_run(0, 8080);
}

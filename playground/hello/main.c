/*
 * playground/hello - a small libioma server: two routes, one worker per core.
 *
 *     make && ./ioma-hello
 *     curl http://127.0.0.1:8080/hello/diogo
 *     curl -d 'knock' http://127.0.0.1:8080/repeat/25
 *
 * Against an installed libioma:  cc main.c $(pkg-config --cflags --libs ioma) -o hello
 */
#include <ioma.h>

/* GET /hello/:name - the capture is the first route parameter; the reply goes into the slab. */
static void hello(ioma_ctx *ctx)
{
    ioma_slice name = ctx->req.route_params[0].value;
    ioma_printf(ctx, "hello %.*s\n", (int)name.len, name.p);
}

/* POST /repeat/:times - reads the body, then streams it back that many times as numbered lines.
 * Each write lands in the reply slab; ioma_flush every ten of them sends what is there, so the
 * client sees lines as they are produced instead of one reply at the end. The first flush sends
 * the head and the body streams chunked from then on. The slab also goes out by itself whenever
 * it fills, and whatever is left goes out when the handler returns. A write or a flush returns -1
 * once the peer is gone. */
static void repeat(ioma_ctx *ctx)
{
    int64_t times;
    if (!ioma_to_i64(ctx->req.route_params[0].value, &times) || times < 1) {
        ctx->res.status = 400;
        ioma_text(ctx, "usage: POST a body to /repeat/<times>\n");
        return;
    }
    ioma_slice body = ioma_body_all(ctx);                  /* the whole body; over 16 KB it is refused */
    if (ctx->res.status != 200)
        return;                                      /* 413: the engine sends it, nothing to add */
    for (int64_t i = 1; i <= times; i++) {
        if (ioma_printf(ctx, "%lld: %.*s\n", (long long)i, (int)body.len, body.p) < 0)
            return;
        if (i % 10 == 0 && ioma_flush(ctx) < 0)
            return;
    }
}

int main(void)
{
    IOMA_GET ("/hello/:name",   hello);
    IOMA_POST("/repeat/:times", repeat);
    return ioma_run(0, 8080);
}

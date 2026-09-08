/*
 * playground/hello - a small libioxd server: three routes, one worker per core.
 *
 *     make && ./ioxd-hello
 *     curl http://127.0.0.1:8080/hello/diogo
 *     curl http://127.0.0.1:8080/users/42
 *     curl -d 'knock' http://127.0.0.1:8080/repeat/25
 *
 * Against an installed libioxd:  cc main.c $(pkg-config --cflags --libs ioxd) -o hello
 */
#include <ioxd.h>

/* GET /hello/:name - the capture is the first route parameter; the reply goes into the slab. */
static void hello(ioxd_ctx *ctx)
{
    ioxd_slice name = ctx->req.route_params[0].value;
    ioxd_printf(ctx, "hello %.*s\n", (int)name.len, name.p);
}

/* POST /repeat/:times - reads the body, then streams it back that many times as numbered lines.
 * Each write lands in the reply slab; ioxd_flush every ten of them sends what is there, so the
 * client sees lines as they are produced instead of one reply at the end. The first flush sends
 * the head and the body streams chunked from then on. The slab also goes out by itself whenever
 * it fills, and whatever is left goes out when the handler returns. A write or a flush returns -1
 * once the peer is gone. */
static void repeat(ioxd_ctx *ctx)
{
    int64_t times;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &times) || times < 1) {
        ctx->res.status = 400;
        ioxd_text(ctx, "usage: POST a body to /repeat/<times>\n");
        return;
    }
    ioxd_slice body = ioxd_body_all(ctx);                  /* the whole body; over 16 KB it is refused */
    if (ctx->res.status != 200)
        return;                                      /* 413: the engine sends it, nothing to add */
    for (int64_t i = 1; i <= times; i++) {
        if (ioxd_printf(ctx, "%lld: %.*s\n", (long long)i, (int)body.len, body.p) < 0)
            return;
        if (i % 10 == 0 && ioxd_flush(ctx) < 0)
            return;
    }
}

/* GET /users/:id - a JSON document, written as you go: each call puts its bytes straight into the
 * reply, escaped, with the commas and nesting tracked for you; nothing is built up in memory
 * first. ioxd_json_reply sets the content type. A document larger than the slab streams out
 * chunked while the writer keeps going. */
static void user(ioxd_ctx *ctx)
{
    int64_t id;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &id)) {
        ctx->res.status = 400;
        ioxd_text(ctx, "the id must be an integer\n");
        return;
    }
    ioxd_json j = ioxd_json_reply(ctx);
    ioxd_json_object(&j);
        ioxd_json_key(&j, "id");      ioxd_json_int(&j, id);
        ioxd_json_key(&j, "name");    ioxd_json_cstr(&j, "Zo\xc3\xab \"Z\" O'Neil");   /* escaped on the way out */
        ioxd_json_key(&j, "active");  ioxd_json_bool(&j, id % 2 == 0);
        ioxd_json_key(&j, "score");   ioxd_json_double(&j, 0.1 * (double)id);
        ioxd_json_key(&j, "address"); ioxd_json_object(&j);
            ioxd_json_key(&j, "city"); ioxd_json_cstr(&j, "Porto");
            ioxd_json_key(&j, "zip");  ioxd_json_null(&j);
        ioxd_json_end(&j);
        ioxd_json_key(&j, "tags");    ioxd_json_array(&j);
            ioxd_json_cstr(&j, "new");
            ioxd_json_cstr(&j, "c23");
        ioxd_json_end(&j);
    ioxd_json_end(&j);
}

int main(void)
{
    IOXD_GET ("/hello/:name",   hello);
    IOXD_GET ("/users/:id",     user);
    IOXD_POST("/repeat/:times", repeat);
    return ioxd_run(0, 8080);
}

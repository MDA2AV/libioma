/*
 * playground/hello - a small libioxd server: three routes, one worker per core, and the same
 * routes over TLS when it is given a directory of certificates.
 *
 *     make && ./ioxd-hello
 *     curl http://127.0.0.1:8080/hello/diogo
 *     curl http://127.0.0.1:8080/users/42
 *     curl -d 'knock' http://127.0.0.1:8080/repeat/25
 *
 *     sh tests/mkcerts.sh certs && ./ioxd-hello certs          # self-signed, so curl needs -k
 *     curl -k https://127.0.0.1:8443/hello/diogo
 *     curl -k --resolve sni.test:8443:127.0.0.1 https://sni.test:8443/users/42
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

/* The shapes GET /users/:id replies with, described once: each list defines the struct and its
 * *_to_json function. A line is the field's kind, its type (or the nested struct's name) and its
 * name; ARRAY and OBJECTS lines add the field that holds the count. */
#define ADDRESS_FIELDS(X)                       \
    X(VALUE,    const char *, city)             \
    X(VALUE,    const char *, zip)              /* NULL comes out as null */
IOXD_JSON_STRUCT(address, ADDRESS_FIELDS)

#define ORDER_FIELDS(X)                         \
    X(VALUE,    int,          number)           \
    X(VALUE,    double,       total)
IOXD_JSON_STRUCT(order, ORDER_FIELDS)

#define USER_FIELDS(X)                          \
    X(VALUE,    int64_t,      id)               \
    X(VALUE,    const char *, name)             \
    X(VALUE,    bool,         active)           \
    X(OBJECT,   address,      address)          /* nested, by value                 */ \
    X(OPTIONAL, address,      billing)          /* a pointer: null when there is none */ \
    X(ARRAY,    const char *, tags,   n_tags)   /* scalars, and the count field     */ \
    X(OBJECTS,  order,        orders, n_orders) /* nested objects, and the count     */
IOXD_JSON_STRUCT(user, USER_FIELDS)

/* GET /users/:id - fill the struct, serialize it with one call. The document goes straight into
 * the reply, escaped, streamed chunked if it outgrows the slab; ioxd_json_reply sets the content
 * type. */
static void user_endpoint(ioxd_ctx *ctx)
{
    int64_t id;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &id)) {
        ctx->res.status = 400;
        ioxd_text(ctx, "the id must be an integer\n");
        return;
    }
    const char  *tags[]   = { "new", "c23" };
    struct order orders[] = { { 1, 9.5 }, { 2, 0.25 } };
    struct user  u = {
        .id = id, .name = "Zo\xc3\xab \"Z\" O'Neil", .active = id % 2 == 0,
        .address = { .city = "Porto", .zip = NULL },
        .billing = NULL,
        .tags = tags, .n_tags = 2,
        .orders = orders, .n_orders = id % 2 == 0 ? 2 : 0,
    };
    ioxd_json j = ioxd_json_reply(ctx);
    user_to_json(&j, &u);
}

/* The routes, then the ports, then the run. Plain HTTP on 8080; with a certificate directory on
 * the command line the same routes on 8443 over TLS 1.3 as well. The store holds one host per
 * subdirectory - <dir>/<host>/cert.pem and key.pem - and the client's SNI picks the host,
 * `default` answering for no name or an unknown one (TLS.md). The handshake is OpenSSL's; from
 * then on the kernel encrypts and decrypts, and a handler cannot tell the two ports apart. */
int main(int argc, char **argv)
{
    IOXD_GET ("/hello/:name",   hello);
    IOXD_GET ("/users/:id",     user_endpoint);
    IOXD_POST("/repeat/:times", repeat);

    ioxd_bind(8080, NULL);
    if (argc > 1) {
        ioxd_certs *tls = ioxd_certs_load(argv[1]);
        if (!tls)
            return 1;                                /* the reason is on stderr: no `default`, a bad key, a TLS=0 build */
        ioxd_bind(8443, tls);
    }
    return ioxd_run(0);                              /* one worker per core, over both ports */
}

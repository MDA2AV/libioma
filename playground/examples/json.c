/*
 * json.c - JSON replies: a struct described once and serialized with one call, a list of
 * thousands streamed as one array, an object written call by call, and an error object with
 * its status.
 *
 *     make examples && ./ioxd-example-json
 *     curl http://127.0.0.1:8080/users/42
 *     curl http://127.0.0.1:8080/users?n=5000 | head -c 200
 *     curl http://127.0.0.1:8080/users/abc                   # {"error":"the id must be an integer"}
 *     curl -i http://127.0.0.1:8080/health
 *
 * The writer is forward-only and allocates nothing: the bytes go into the reply slab, escaped
 * as they are written, and stream out chunked when a document outgrows it. Nesting and commas
 * are its business; a handler says what it means. Every call returns false once the reply
 * failed or the call had no place in the document, and the rest is then dropped, so checking
 * the last call - or ioxd_json_done at the end - is enough.
 */
#include <ioxd.h>

/* The shape of a user, described once: the struct and user_to_json come out of it. */
#define ADDRESS_FIELDS(X)                       \
    X(VALUE,    const char *, city)             \
    X(VALUE,    const char *, zip)              /* NULL comes out as null */
IOXD_JSON_STRUCT(address, ADDRESS_FIELDS)

#define USER_FIELDS(X)                          \
    X(VALUE,    int64_t,      id)               \
    X(VALUE,    const char *, name)             \
    X(VALUE,    bool,         active)           \
    X(OBJECT,   address,      address)          /* nested, by value                   */ \
    X(OPTIONAL, address,      billing)          /* a pointer: null when there is none */ \
    X(ARRAY,    const char *, tags,   n_tags)   /* scalars, and the field with the count */
IOXD_JSON_STRUCT(user, USER_FIELDS)

static struct user make_user(int64_t id)
{
    static const char *tags[] = { "new", "c23" };
    return (struct user){
        .id = id, .name = "Zo\xc3\xab \"Z\" O'Neil", .active = id % 2 == 0,     /* escaped on the way out */
        .address = { .city = "Porto", .zip = NULL },
        .billing = NULL,
        .tags = tags, .n_tags = 2,
    };
}

/* An error as a document, with its status: {"error":"..."}. */
static void error_reply(ioxd_ctx *ctx, int status, const char *message)
{
    ctx->res.status = status;
    ioxd_json j = ioxd_json_reply(ctx);              /* content-type: application/json */
    ioxd_json_object(&j);
    IOXD_JSON_FIELD(&j, "error", message);
    ioxd_json_end(&j);
}

/* GET /users/:id - one user, one call. */
static void one_user(ioxd_ctx *ctx)
{
    int64_t id;
    if (!ioxd_to_i64(ctx->req.route_params[0].value, &id)) {
        error_reply(ctx, 400, "the id must be an integer");
        return;
    }
    struct user u = make_user(id);
    ioxd_json j = ioxd_json_reply(ctx);
    user_to_json(&j, &u);
}

/* GET /users?n= - an array of n users: the slab fills and streams, chunked, as it goes. */
static void many_users(ioxd_ctx *ctx)
{
    int64_t n = 100;
    for (size_t i = 0; i < ctx->req.n_params; i++)
        if (ioxd_slice_eq(ctx->req.params[i].key, "n"))
            ioxd_to_i64(ctx->req.params[i].value, &n);
    ioxd_json j = ioxd_json_reply(ctx);
    ioxd_json_array(&j);
    for (int64_t id = 1; id <= n; id++) {
        struct user u = make_user(id);
        if (!user_to_json(&j, &u))
            return;                                  /* the peer is gone: nothing more to write */
    }
    ioxd_json_end(&j);
}

/* GET /health - the bare writer, no macros: a key, then the value's call by type; object,
 * array and end for the nesting. IOXD_JSON_FIELD(&j, "workers", 4) would be the key and the
 * value in one line, the call picked from the C type. */
static void health(ioxd_ctx *ctx)
{
    ioxd_json j = ioxd_json_reply(ctx);
    ioxd_json_object(&j);
    ioxd_json_key(&j, "status");  ioxd_json_cstr(&j, "ok");
    ioxd_json_key(&j, "workers"); ioxd_json_int(&j, 4);
    ioxd_json_key(&j, "load");    ioxd_json_double(&j, 0.25);
    ioxd_json_key(&j, "ports");   ioxd_json_array(&j);
                                  ioxd_json_int(&j, 8080);
                                  ioxd_json_int(&j, 8443);
                                  ioxd_json_end(&j);
    ioxd_json_end(&j);
    if (!ioxd_json_done(&j))                         /* nothing failed, nothing left open */
        ctx->res.status = 500;
}

int main(void)
{
    IOXD_GET("/users/:id", one_user);
    IOXD_GET("/users",     many_users);
    IOXD_GET("/health",    health);
    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}

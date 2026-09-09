/*
 * router_test.c - the router without a server: paths joined into the segment tree, the allow
 * header of a 405, HEAD answered by GET, decoded captures, the middleware chain, and the
 * diagnostics that guard registration. Requests are driven straight through the dispatcher with
 * a context built by hand - one whose reply is marked failed, so the body writes of the built-in
 * fallbacks go nowhere and nothing here touches the wire. `make check` runs it beside the unit
 * test.
 */
#include <ioxd.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* What the engine calls, from the library's private lib/http/router.h. */
void ioxd__router_build(void);
void ioxd__router_dispatch(ioxd_ctx *ctx);

static int checks, failures;

/* Count a check; report a failed one by line. */
static void check(const char *what, bool ok, int line)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(cond) check(#cond, (cond), __LINE__)

/* A slice over a C string. */
static ioxd_slice S(const char *cstr)
{
    return (ioxd_slice){ cstr, strlen(cstr) };
}

/* ── the routes under test ─────────────────────────────────────────────────────────────── */

static const char *ran;                             /* the handler the request reached */
static char        trace[64];                       /* the chain, in the order it ran   */
static size_t      n_trace;

/* One letter of the trace: a middleware's on the way in, its capital on the way out. */
static void mark(char c)
{
    if (n_trace < sizeof trace - 1)
        trace[n_trace++] = c;
    trace[n_trace] = '\0';
}

/* Every handler is the same: it says which one ran and puts an 'h' in the trace. */
#define HANDLER(name) static void name(ioxd_ctx *ctx) { (void)ctx; ran = #name; mark('h'); }
HANDLER(h_health)
HANDLER(h_new)
HANDLER(h_user)
HANDLER(h_patch)
HANDLER(h_api_users)
HANDLER(h_items)
HANDLER(h_leak)
HANDLER(h_after)
HANDLER(h_twice)
HANDLER(h_late)                                     /* registered too late: never reached */
HANDLER(h_doc)
HANDLER(h_doc_head)
HANDLER(h_doc_options)

static void mw_root(ioxd_ctx *ctx, ioxd_next *next)  { mark('r'); ioxd_next_run(ctx, next); mark('R'); }
static void mw_api (ioxd_ctx *ctx, ioxd_next *next)  { mark('a'); ioxd_next_run(ctx, next); mark('A'); }
static void mw_own (ioxd_ctx *ctx, ioxd_next *next)  { mark('o'); ioxd_next_run(ctx, next); mark('O'); }
static void mw_late(ioxd_ctx *ctx, ioxd_next *next)  { mark('!'); ioxd_next_run(ctx, next); }

/* A middleware that runs the rest of the chain twice; the second call must run nothing. */
static void mw_twice(ioxd_ctx *ctx, ioxd_next *next)
{
    mark('t');
    ioxd_next_run(ctx, next);
    ioxd_next_run(ctx, next);
    mark('T');
}

static ioxd_endpoint *g_health;                     /* for a middleware added after the build */

/* The whole table, registered as a script. The order is the order the allow header lists. */
static void register_routes(void)
{
    IOXD_USE(mw_root);
    g_health = IOXD_GET("/health", h_health);       /* no HEAD: its GET answers one */
    IOXD_POST("/users/new", h_new);
    IOXD_GET("/users/:id", h_user);
    IOXD_PATCH("/users/new", h_patch);
    IOXD_POST("/users/:id", h_user);                /* a method on both nodes: listed once */
    IOXD_GET("/twice", h_twice, mw_twice);
    IOXD_GET("/doc", h_doc);
    IOXD_HEAD("/doc", h_doc_head);                  /* a HEAD of its own: not the GET's */
    ioxd_options(NULL, "/doc", h_doc_options);

    IOXD_GROUP("/api", mw_api) {
        IOXD_GET("users", h_api_users, mw_own);     /* neither side brings a '/': "/api/users" */
        IOXD_GROUP("/v2/") {
            IOXD_GET("/items", h_items);            /* both do: "/api/v2/items" */
        }
    }

    ioxd_group *outer = ioxd__router_group_current();
    IOXD_GROUP("/leak") {
        IOXD_GET("/x", h_leak);
        break;                                      /* leaving early must still close the group */
    }
    CHECK(ioxd__router_group_current() == outer);
    IOXD_GET("/after", h_after);                    /* so this is "/after", not "/leak/after" */
}

/* ── driving one request ───────────────────────────────────────────────────────────────── */

static ioxd_ctx g_ctx;

/* The connection, stood in for. ioxd_write takes the address of the writer inside the pipe that
 * the engine's private state points at, and only then sees res.failed and gives up, so ctx.priv
 * needs something shaped like that state: one pointer, to something big enough that the writer's
 * address lands inside it. With failed set nothing is ever read or written through it. */
static alignas(max_align_t) char g_pipe[8192];
static void                     *g_priv[1] = { g_pipe };

/* Dispatch one request against a context shaped like the engine's, minus the connection: the
 * reply is failed from the start, so the body a handler or a built-in fallback writes is
 * dropped and nothing here touches the wire. */
static void request(const char *method, const char *path)
{
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.req.method = S(method);
    g_ctx.req.path   = S(path);
    g_ctx.res.status = 200;
    g_ctx.res.failed = true;
    g_ctx.priv       = g_priv;
    ran      = "";
    n_trace  = 0;
    trace[0] = '\0';
    ioxd__router_dispatch(&g_ctx);
}

/* Did the request reach this handler? */
static bool reached(const char *name)
{
    return strcmp(ran, name) == 0;
}

/* Is the reply's header this text? */
static bool replied_header(const char *name, const char *value)
{
    for (size_t i = 0; i < g_ctx.res.n_headers; i++)
        if (ioxd_slice_eq(g_ctx.res.headers[i].key, name))
            return ioxd_slice_eq(g_ctx.res.headers[i].value, value);
    return false;
}

/* Is the capture of this name this text? */
static bool captured(const char *name, const char *value)
{
    for (size_t i = 0; i < g_ctx.req.n_route_params; i++)
        if (ioxd_slice_eq(g_ctx.req.route_params[i].key, name))
            return ioxd_slice_eq(g_ctx.req.route_params[i].value, value);
    return false;
}

/* Run fn with stderr in a temporary file and leave what it printed in buf: the diagnostics are
 * part of what is being tested. */
static void capture_stderr(void (*fn)(void), char *buf, size_t cap)
{
    buf[0] = '\0';
    fflush(stderr);
    FILE *tmp   = tmpfile();
    int   saved = tmp ? dup(STDERR_FILENO) : -1;
    if (saved < 0) {                                /* no capture; run it anyway */
        if (tmp)
            fclose(tmp);
        fn();
        return;
    }
    dup2(fileno(tmp), STDERR_FILENO);
    fn();
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    if (fseek(tmp, 0, SEEK_SET) == 0)
        buf[fread(buf, 1, cap - 1, tmp)] = '\0';
    fclose(tmp);
}

/* ── the checks ────────────────────────────────────────────────────────────────────────── */

/* A group opened and never closed: everything after it nested inside, so ioxd_run says so. */
static void build_with_a_group_left_open(void)
{
    ioxd__router_group_begin((struct ioxd_group_args){ "/stray", { NULL } });
    ioxd__router_build();
}

static void test_build(void)
{
    char said[512];
    capture_stderr(build_with_a_group_left_open, said, sizeof said);
    CHECK(strstr(said, "1 group(s) still open") != NULL);
    CHECK(strstr(said, "/stray") != NULL);
}

static void test_paths(void)
{
    request("GET", "/api/users");                   /* "/api" and "users", joined */
    CHECK(reached("h_api_users"));
    request("GET", "/apiusers");                    /* what the missing '/' used to make */
    CHECK(g_ctx.res.status == 404);
    request("GET", "/api/v2/items");                /* "/v2/" and "/items": one slash */
    CHECK(reached("h_items"));
    request("GET", "/api/v2//items");               /* a repeated slash counts once */
    CHECK(reached("h_items"));
    request("GET", "/health/");                     /* a trailing slash is tolerated */
    CHECK(reached("h_health"));

    request("GET", "/leak/x");                      /* the block's own endpoint is still there */
    CHECK(reached("h_leak"));
    request("GET", "/after");                       /* and the one after it did not nest */
    CHECK(reached("h_after"));
    request("GET", "/leak/after");
    CHECK(g_ctx.res.status == 404);
}

static void test_captures(void)
{
    request("GET", "/users/7");
    CHECK(reached("h_user") && captured("id", "7"));
    request("GET", "/users/a%2Fb");                 /* an encoded '/' does not split the segment */
    CHECK(reached("h_user") && captured("id", "a/b"));
    request("GET", "/users/%65");
    CHECK(reached("h_user") && captured("id", "e"));
    request("GET", "/users/a%2b");                  /* no '+' rules in a path */
    CHECK(captured("id", "a+"));
    request("GET", "/users/100%25");
    CHECK(captured("id", "100%"));
    request("GET", "/users/%zz");                   /* a malformed escape stays as it came */
    CHECK(captured("id", "%zz"));

    request("GET", "/us%65rs/7");                   /* static segments match the raw bytes */
    CHECK(g_ctx.res.status == 404);
    request("GET", "/users/new");                   /* a static path without GET falls through */
    CHECK(reached("h_user") && captured("id", "new"));
    request("POST", "/users/new");                  /* while the static one wins where it has it */
    CHECK(reached("h_new"));
}

static void test_allow(void)
{
    /* "/users/new" ends the static route and the capture route both, so the 405 allows the
     * methods of each: POST and PATCH here, GET (and the HEAD its GET answers) there. */
    request("DELETE", "/users/new");
    CHECK(g_ctx.res.status == 405);
    CHECK(replied_header("allow", "POST, GET, HEAD, PATCH"));

    request("DELETE", "/health");                   /* HEAD is listed behind the GET that serves it */
    CHECK(g_ctx.res.status == 405 && replied_header("allow", "GET, HEAD"));

    request("DELETE", "/users/7");                  /* the capture node alone */
    CHECK(g_ctx.res.status == 405 && replied_header("allow", "GET, HEAD, POST"));

    request("DELETE", "/nowhere");                  /* no path at all: the fallback, no allow */
    CHECK(g_ctx.res.status == 404 && g_ctx.res.n_headers == 0);
}

static void test_head(void)
{
    request("HEAD", "/health");                     /* served by the GET, method left alone */
    CHECK(reached("h_health") && g_ctx.res.status == 200);
    CHECK(ioxd_slice_eq(g_ctx.req.method, "HEAD"));
    request("HEAD", "/api/users");
    CHECK(reached("h_api_users"));
    request("HEAD", "/users/7");                    /* through a capture too */
    CHECK(reached("h_user") && captured("id", "7"));
    request("HEAD", "/nowhere");
    CHECK(g_ctx.res.status == 404);
    request("HEAD", "/doc");                        /* an explicit HEAD route beats the fallback */
    CHECK(reached("h_doc_head"));
    request("GET", "/doc");
    CHECK(reached("h_doc"));
    request("OPTIONS", "/doc");
    CHECK(reached("h_doc_options"));
    request("PUT", "/doc");                         /* every method the path has, HEAD once */
    CHECK(g_ctx.res.status == 405 && allow_is("GET, HEAD, OPTIONS"));
}

static void test_chain(void)
{
    request("GET", "/api/users");                   /* root, group, own, handler, and back out */
    CHECK(strcmp(trace, "raohOAR") == 0);
    request("GET", "/twice");                       /* the second ioxd_next_run runs nothing */
    CHECK(strcmp(trace, "rthTR") == 0);
    request("GET", "/nowhere");                     /* the fallbacks run behind the root's only */
    CHECK(strcmp(trace, "rR") == 0);
}

/* Everything the guards must refuse once ioxd_run has resolved the table. */
static void register_after_the_build(void)
{
    CHECK(ioxd_route(NULL, "GET", "/late", h_late) == NULL);
    ioxd_use(mw_late);
    ioxd_group_use(NULL, mw_late);
    ioxd_endpoint_use(g_health, mw_late);
    ioxd_default(h_late);
}

static void test_registration_is_closed(void)
{
    char said[1024];
    capture_stderr(register_after_the_build, said, sizeof said);
    int ignored = 0;
    for (const char *at = said; (at = strstr(at, "; ignored")); at++)
        ignored++;
    CHECK(ignored == 5);                            /* one line for each of the five */

    request("GET", "/late");                        /* and none of them took */
    CHECK(g_ctx.res.status == 404 && !reached("h_late"));
    CHECK(strcmp(trace, "rR") == 0);                /* the built-in fallback, behind the root's */
    request("GET", "/health");
    CHECK(reached("h_health") && strcmp(trace, "rhR") == 0);   /* the endpoint's chain is as it was */
}

int main(void)
{
    register_routes();
    test_build();
    test_paths();
    test_captures();
    test_allow();
    test_head();
    test_chain();
    test_registration_is_closed();
    printf("router: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

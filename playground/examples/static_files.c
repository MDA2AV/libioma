/*
 * static_files.c - files from a directory, served by the library: the path under a mount names
 * the file, its extension the content type; a validator and a date answer conditional requests
 * with a 304, a byte range gets its 206, a .br or .gz twin beside the file goes out when the
 * client takes the coding. What a worker served it keeps in memory and checks against the disk
 * before serving again, so a replaced file is served new at once.
 *
 *     mkdir -p www && echo '<h1>hello</h1>' > www/index.html && echo 'h1 { color: teal }' > www/site.css
 *     make examples && ./ioxd-example-static_files www
 *     curl -i http://127.0.0.1:8080/                                  # www/index.html
 *     curl -i http://127.0.0.1:8080/static/site.css
 *     curl -i -H 'If-None-Match: "<the etag>"' http://127.0.0.1:8080/static/site.css   # 304
 *     curl -i -H 'Range: bytes=0-3' http://127.0.0.1:8080/static/site.css              # 206
 *
 * One store serves both routes: /static/... through the fallback handler, so a nested path
 * ("/static/css/site.css") reaches it too, and "/" as the named index. The reads go through the
 * ring, the first time a file is asked for; after that it is served from memory.
 */
#include <ioxd.h>

static ioxd_static *g_files;

/* Anything unrouted: the store answers for its mount (404 for a file that is not there), and
 * what is not under the mount at all gets a plain 404 of our own. */
static void files(ioxd_ctx *ctx)
{
    if (ioxd_static_serve(ctx, g_files))
        return;
    ctx->res.status = 404;
    ioxd_text(ctx, "not here\n");
}

/* GET /: the index, by name. */
static void home(ioxd_ctx *ctx)
{
    ioxd_static_file(ctx, g_files, "index.html");
}

int main(int argc, char **argv)
{
    g_files = ioxd_static_open(&(ioxd_static_config){
        .dir           = argc > 1 ? argv[1] : "www",
        .mount         = "/static",
        .cache_control = "max-age=60",
        .precompressed = true,                       /* site.css.br beside site.css, when the client takes br */
    });
    if (!g_files)
        return 1;                                    /* the reason is on stderr */
    IOXD_GET("/", home);
    IOXD_DEFAULT(files);
    ioxd_bind(8080, NULL);
    int rc = ioxd_run(0);
    ioxd_static_close(g_files);
    return rc;
}

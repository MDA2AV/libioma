/*
 * static_files.c - files from a directory, served by hand: the path's last segment names the
 * file, its extension the content type, fstat the length, an ETag the version, and the bytes go
 * through the reply slab piece by piece with a declared length. HEAD and 304 cost no read.
 *
 *     mkdir -p www && echo '<h1>hello</h1>' > www/index.html && echo 'h1 { color: teal }' > www/site.css
 *     make examples && ./ioxd-example-static_files www
 *     curl -i http://127.0.0.1:8080/
 *     curl -i http://127.0.0.1:8080/static/site.css
 *     curl -i -H 'If-None-Match: "<the etag>"' http://127.0.0.1:8080/static/site.css   # 304
 *
 * The reads are read(2), synchronous on the worker: what the page cache holds comes back at
 * once, a cold file stalls that worker's other connections for the read. Serving files through
 * io_uring, with a snapshot in memory and change detection, is FILES.md - designed, not built.
 */
#include <ioxd.h>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *g_dir = "www";

/* The content type from the extension; what is not known is bytes. */
static const char *type_of(const char *name)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html; charset=utf-8" }, { ".css", "text/css" }, { ".js", "text/javascript" },
        { ".json", "application/json" },          { ".svg", "image/svg+xml" }, { ".png", "image/png" },
        { ".jpg", "image/jpeg" },                 { ".txt", "text/plain; charset=utf-8" },
    };
    const char *dot = strrchr(name, '.');
    if (dot)
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (strcmp(dot, types[i].ext) == 0)
                return types[i].type;
    return "application/octet-stream";
}

/* The file, or a 404. One path segment names it, so it cannot leave the directory; ".." and a
 * hidden file are refused all the same. */
static void send_file(ioxd_ctx *ctx, const char *name)
{
    if (name[0] == '\0' || name[0] == '.' || strstr(name, "..")) {
        ctx->res.status = 404;
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        ctx->res.status = 404;
        return;
    }

    /* the version: size and modification time; a client that has it gets a 304 and no body */
    char etag[64];
    snprintf(etag, sizeof etag, "\"%llx-%llx\"", (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
    for (size_t i = 0; i < ctx->req.n_headers; i++)
        if (ioxd_slice_eq(ctx->req.headers[i].key, "if-none-match") && ioxd_slice_eq(ctx->req.headers[i].value, etag)) {
            ctx->res.status = 304;
            ioxd_header(ctx, "etag", etag);
            close(fd);
            return;
        }

    ioxd_header(ctx, "etag", etag);
    ioxd_header(ctx, "cache-control", "max-age=60");
    ioxd_content_type(ctx, type_of(name));
    ioxd_content_length(ctx, (size_t)st.st_size);   /* Content-Length framing, whatever the size */
    if (ioxd_slice_eq(ctx->req.method, "HEAD")) {   /* the head is all that goes out: no read */
        close(fd);
        return;
    }
    for (off_t left = st.st_size; left > 0;) {
        size_t piece = left < 8192 ? (size_t)left : 8192;
        char  *at = ioxd_reserve(ctx, piece);        /* room in the slab, flushed first when full */
        if (!at)
            break;                                   /* the peer is gone */
        ssize_t n = read(fd, at, piece);
        if (n <= 0)
            break;                                   /* short of the declared length: the engine closes */
        ioxd_advance(ctx, (size_t)n);
        left -= n;
    }
    close(fd);
}

/* GET /static/:name */
static void file_route(ioxd_ctx *ctx)
{
    char name[NAME_MAX + 1];
    if (!ioxd_cstr(ctx->req.route_params[0].value, name, sizeof name)) {   /* too long, or a NUL inside */
        ctx->res.status = 404;
        return;
    }
    send_file(ctx, name);
}

/* GET / */
static void index_route(ioxd_ctx *ctx)
{
    send_file(ctx, "index.html");
}

int main(int argc, char **argv)
{
    if (argc > 1)
        g_dir = argv[1];
    IOXD_GET("/",             index_route);
    IOXD_GET("/static/:name", file_route);
    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}

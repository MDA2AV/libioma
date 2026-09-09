/*
 * stream_request.c - a request body streamed, so a body of any size never sits in memory whole:
 * read through a fixed buffer until it ends, whatever its framing, or taken chunk by chunk
 * exactly as the sender framed it.
 *
 *     make examples && ./ioxd-example-stream_request
 *     head -c 50000000 /dev/urandom > big; curl -T big http://127.0.0.1:8080/upload
 *     curl -H 'Transfer-Encoding: chunked' -T big http://127.0.0.1:8080/chunks
 *
 * ioxd_body_read_until fills the buffer or reaches the end; ioxd_body_read_next_chunk hands over
 * one chunk of a chunked body, whole. Both return 0 once the body is consumed and -1 on a
 * malformed body or a peer that is gone - the engine then answers the 400 or closes, so the
 * handler just returns. Nothing is drained twice: what a handler leaves unread, the engine reads
 * past after it, up to a limit, so the connection stays in sync for the next request.
 */
#include <ioxd.h>

#include <stdint.h>
#include <stdlib.h>

/* PUT or POST /upload: any framing, 4 KB at a time, hashed as it goes (FNV-1a). */
static void upload(ioxd_ctx *ctx)
{
    char     buf[4096];
    uint64_t hash = 1469598103934665603ULL, total = 0;
    for (;;) {
        int n = ioxd_body_read_until(ctx, buf, sizeof buf);
        if (n < 0)
            return;                                  /* malformed or gone: the engine answers */
        if (n == 0)
            break;                                   /* the whole body has been read */
        for (int i = 0; i < n; i++)
            hash = (hash ^ (unsigned char)buf[i]) * 1099511628211ULL;
        total += (uint64_t)n;
    }
    ioxd_printf(ctx, "%llu bytes, fnv1a %016llx\n", (unsigned long long)total, (unsigned long long)hash);
}

/* POST /chunks: a chunked body, each chunk as the sender framed it. A chunk larger than the
 * buffer is a 413, and curl frames 64 KB at a time, so the buffer is that big - and off the
 * coroutine's stack, which is 128 KB with the engine's own frames on it. */
static void chunks(ioxd_ctx *ctx)
{
    if (!ctx->req.chunked) {
        ctx->res.status = 400;
        ioxd_text(ctx, "send it chunked\n");
        return;
    }
    enum { CHUNK_MAX = 65536 };
    char *buf = malloc(CHUNK_MAX);
    if (!buf) {
        ctx->res.status = 500;
        return;
    }
    int count = 0;
    for (;;) {
        int n = ioxd_body_read_next_chunk(ctx, buf, CHUNK_MAX);
        if (n < 0)
            break;                                   /* malformed, gone, or a chunk too large: 413 */
        if (n == 0) {                                /* the last chunk: the body is done */
            ioxd_printf(ctx, "%d chunks in all\n", count);
            break;
        }
        count++;
        if (count <= 5)                              /* the reply streams too, chunked, as the slab fills */
            ioxd_printf(ctx, "chunk %d: %d bytes\n", count, n);
    }
    free(buf);
}

int main(void)
{
    IOXD_PUT ("/upload", upload);
    IOXD_POST("/upload", upload);
    IOXD_PUT ("/chunks", chunks);
    IOXD_POST("/chunks", chunks);
    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}

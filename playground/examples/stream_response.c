/*
 * stream_response.c - replies that stream: a body bigger than the slab goes out chunked as the
 * slab fills, a flush sends what is there on purpose so a client sees lines as they are made, a
 * declared length lets a large body of known size go out with Content-Length instead, and
 * reserve/advance write straight into the slab.
 *
 *     make examples && ./ioxd-example-stream_response
 *     curl -N http://127.0.0.1:8080/ticks                     # a line a quarter second, 20 of them
 *     curl -N http://127.0.0.1:8080/rows?n=100000            # chunked; -N shows it arriving
 *     curl -o /dev/null -w '%{size_download}\n' http://127.0.0.1:8080/blob?n=50000000
 *     curl -I http://127.0.0.1:8080/blob?n=50000000          # HEAD: the head, no body
 *
 * The head goes out with the first send and is frozen from then on: a status or a header set
 * after a flush is too late (ioxd_header returns false). A write or a flush returns -1 once the
 * peer is gone, and a handler should stop then; there is nobody to send to.
 */
#include <ioxd.h>

#include <string.h>

/* The n of ?n=..., or a fallback. */
static long count_param(const ioxd_ctx *ctx, long fallback)
{
    for (size_t i = 0; i < ctx->req.n_params; i++) {
        int64_t v;
        if (ioxd_slice_eq(ctx->req.params[i].key, "n") && ioxd_to_i64(ctx->req.params[i].value, &v) && v >= 0)
            return (long)v;
    }
    return fallback;
}

/* GET /ticks: a live feed. ioxd_delay parks this connection's coroutine on the ring for the
 * quarter second; the worker serves its other connections meanwhile, nothing blocks. */
static void ticks(ioxd_ctx *ctx)
{
    for (int i = 1; i <= 20; i++) {
        if (ioxd_printf(ctx, "tick %d\n", i) < 0 || ioxd_flush(ctx) < 0)
            return;                                  /* the peer is gone */
        if (ioxd_delay(250) != 0)
            return;                                  /* the server is stopping */
    }
}

/* GET /rows?n=: n lines. The slab streams by itself when it fills; the flush every 100 rows
 * sends earlier, so a client sees the feed move instead of 8 KB at a time. */
static void rows(ioxd_ctx *ctx)
{
    long n = count_param(ctx, 1000);
    for (long i = 1; i <= n; i++) {
        if (ioxd_printf(ctx, "row %ld\n", i) < 0)
            return;                                  /* the peer is gone */
        if (i % 100 == 0 && ioxd_flush(ctx) < 0)
            return;
    }
}

/* GET /blob?n=: n bytes with a declared length, written in place. Content-Length rather than
 * chunked framing, so a client knows the size up front; the engine holds the reply to it - a
 * handler that writes more is cut at n and the connection closed, one that writes less closes. */
static void blob(ioxd_ctx *ctx)
{
    long n = count_param(ctx, 1000000);
    ioxd_content_length(ctx, (size_t)n);
    ioxd_content_type(ctx, "application/octet-stream");
    for (long left = n; left > 0;) {
        size_t piece = left < 4096 ? (size_t)left : 4096;
        char  *at = ioxd_reserve(ctx, piece);        /* room in the slab; a flush first if it is full */
        if (!at)
            return;
        memset(at, 'x', piece);
        ioxd_advance(ctx, piece);
        left -= (long)piece;
    }
}

int main(void)
{
    IOXD_GET("/ticks", ticks);
    IOXD_GET("/rows", rows);
    IOXD_GET("/blob", blob);
    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}

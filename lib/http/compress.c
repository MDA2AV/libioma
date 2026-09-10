#include "http/compress.h"
#include "ioxd/compress.h"
#include "ioxd/http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if IOXD_BROTLI
#include <brotli/encode.h>
#endif
#if IOXD_ZLIB
#include <zlib.h>
#endif

#define ARENA_FIRST (1024UL * 1024)
#define ALIGN       16

enum kind { KIND_BR, KIND_GZIP, KINDS };

struct coder {
    struct coder *next;
    enum kind     kind;
#if IOXD_BROTLI
    BrotliEncoderState *br;
    char               *arena;
    size_t              arena_cap, arena_used, arena_peak;
#endif
#if IOXD_ZLIB
    z_stream zs;
#endif
};

static ioxd_compress_config g_cfg = { .brotli_quality = 1, .brotli_window = 22, .gzip_level = 1, .min_bytes = 256 };

/* ── coders ────────────────────────────────────────────────────────────────────────────── */

#if IOXD_BROTLI || IOXD_ZLIB

static const char *const           names[KINDS] = { "br", "gzip" };
static _Thread_local struct coder *tl_free[KINDS];

#if IOXD_BROTLI
static void *arena_alloc(void *opaque, size_t size)
{
    struct coder *c    = opaque;
    size_t        need = (size + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    c->arena_peak += need;
    if (c->arena && c->arena_used + need <= c->arena_cap) {
        void *at = c->arena + c->arena_used;
        c->arena_used += need;
        return at;
    }
    return malloc(size);
}

static void arena_free(void *opaque, void *address)
{
    struct coder *c = opaque;
    if (!address || (c->arena && (char *)address >= c->arena && (char *)address < c->arena + c->arena_cap))
        return;
    free(address);
}

static bool br_make(struct coder *c, size_t hint)
{
    if (!c->arena) {
        c->arena_cap = ARENA_FIRST;
        c->arena     = malloc(c->arena_cap);
    }
    c->arena_used = c->arena_peak = 0;
    c->br         = BrotliEncoderCreateInstance(arena_alloc, arena_free, c);
    if (!c->br)
        return false;
    BrotliEncoderSetParameter(c->br, BROTLI_PARAM_QUALITY, (uint32_t)g_cfg.brotli_quality);
    BrotliEncoderSetParameter(c->br, BROTLI_PARAM_LGWIN, (uint32_t)g_cfg.brotli_window);
    if (hint)
        BrotliEncoderSetParameter(c->br, BROTLI_PARAM_SIZE_HINT, (uint32_t)(hint > UINT32_MAX ? UINT32_MAX : hint));
    return true;
}

static void br_done(struct coder *c)
{
    if (c->br)
        BrotliEncoderDestroyInstance(c->br);
    c->br = nullptr;
    if (c->arena_peak > c->arena_cap) {
        size_t cap  = c->arena_peak + c->arena_peak / 4;
        char  *more = realloc(c->arena, cap);
        if (more) {
            c->arena     = more;
            c->arena_cap = cap;
        }
    }
    c->arena_used = 0;
}
#endif

static struct coder *coder_take(enum kind kind, size_t hint)
{
    struct coder *c = tl_free[kind];
    if (c) {
        tl_free[kind] = c->next;
    } else {
        c = calloc(1, sizeof *c);
        if (!c)
            return nullptr;
        c->kind = kind;
#if IOXD_ZLIB
        if (kind == KIND_GZIP && deflateInit2(&c->zs, g_cfg.gzip_level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            free(c);
            return nullptr;
        }
#endif
    }
#if IOXD_BROTLI
    if (kind == KIND_BR && !br_make(c, hint)) {
        c->next       = tl_free[kind];
        tl_free[kind] = c;
        return nullptr;
    }
#else
    (void)hint;
#endif
#if IOXD_ZLIB
    if (kind == KIND_GZIP)
        deflateReset(&c->zs);
#endif
    return c;
}

static void coder_give(void *arg)
{
    struct coder *c = arg;
#if IOXD_BROTLI
    if (c->kind == KIND_BR)
        br_done(c);
#endif
    c->next          = tl_free[c->kind];
    tl_free[c->kind] = c;
}

#if IOXD_BROTLI
static int br_run(void *arg, const void *in, size_t *in_len, void *out, size_t *out_len, enum ioxd_filter_op op)
{
    struct coder          *c        = arg;
    const uint8_t         *next_in  = in;
    uint8_t               *next_out = out;
    size_t                 avail_in = *in_len, avail_out = *out_len;
    BrotliEncoderOperation bop      = op == IOXD_FILTER_FINISH ? BROTLI_OPERATION_FINISH
                                    : op == IOXD_FILTER_FLUSH  ? BROTLI_OPERATION_FLUSH
                                                               : BROTLI_OPERATION_PROCESS;
    if (!BrotliEncoderCompressStream(c->br, bop, &avail_in, &next_in, &avail_out, &next_out, nullptr))
        return -1;
    *in_len  -= avail_in;
    *out_len -= avail_out;
    if (BrotliEncoderHasMoreOutput(c->br))
        return 1;
    if (op == IOXD_FILTER_FINISH && !BrotliEncoderIsFinished(c->br))
        return 1;
    return 0;
}
#endif

#if IOXD_ZLIB
static int gzip_run(void *arg, const void *in, size_t *in_len, void *out, size_t *out_len, enum ioxd_filter_op op)
{
    struct coder *c = arg;
    c->zs.next_in   = (Bytef *)(uintptr_t)in;
    c->zs.avail_in  = (uInt)*in_len;
    c->zs.next_out  = out;
    c->zs.avail_out = (uInt)*out_len;
    int flush = op == IOXD_FILTER_FINISH ? Z_FINISH : op == IOXD_FILTER_FLUSH ? Z_SYNC_FLUSH : Z_NO_FLUSH;
    int rc    = deflate(&c->zs, flush);
    if (rc == Z_STREAM_ERROR)
        return -1;
    *in_len  -= c->zs.avail_in;
    *out_len -= c->zs.avail_out;
    if (rc == Z_STREAM_END)
        return 0;
    if (op == IOXD_FILTER_FINISH || c->zs.avail_out == 0)
        return 1;
    return 0;
}
#endif

#endif

/* ── the decision ──────────────────────────────────────────────────────────────────────── */

static bool compressible(ioxd_slice type)
{
    ioxd_slice_cut(type, ';', &type, nullptr);
    type = ioxd_slice_trim(type);
    if (ioxd_slice_starts_with(type, "text/") || ioxd_slice_ends_with(type, "+json") || ioxd_slice_ends_with(type, "+xml"))
        return true;
    static const char *const types[] = {
        "application/json", "application/javascript", "application/x-javascript", "application/xml",
        "application/wasm", "image/svg+xml",
    };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (ioxd_slice_eq_ci(type, types[i]))
            return true;
    return false;
}

static bool has_header(const ioxd_response *res, const char *name)
{
    for (size_t i = 0; i < res->n_headers; i++)
        if (ioxd_slice_eq(res->headers[i].key, name))
            return true;
    return false;
}

static void decide(ioxd_ctx *ctx, void *arg, size_t buffered, bool whole)
{
    (void)arg;
    ioxd_response *res = &ctx->res;
    if (res->status < 200 || res->status >= 300 || res->status == 204 || has_header(res, "content-encoding")
        || !compressible(res->content_type))
        return;
    ioxd_header(ctx, "vary", "accept-encoding");
    if (whole && buffered < g_cfg.min_bytes)
        return;
#if IOXD_BROTLI || IOXD_ZLIB
    double        gz = ioxd_accepts_encoding(ctx, "gzip");
    struct coder *c  = nullptr;
    ioxd_filter   filter = { .end = coder_give };
#if IOXD_BROTLI
    double br = ioxd_accepts_encoding(ctx, "br");
    if (br > 0 && br >= gz) {
        c          = coder_take(KIND_BR, whole ? buffered : 0);
        filter.run = br_run;
    }
#endif
#if IOXD_ZLIB
    if (!c && gz > 0) {
        c          = coder_take(KIND_GZIP, 0);
        filter.run = gzip_run;
    }
#endif
    if (!c)
        return;
    filter.arg = c;
    if (!ioxd_reply_filter(ctx, &filter)) {
        coder_give(c);
        return;
    }
    ioxd_header(ctx, "content-encoding", names[c->kind]);
#else
    (void)buffered;
#endif
}

/* ── the middleware ────────────────────────────────────────────────────────────────────── */

void ioxd_compress(ioxd_ctx *ctx, ioxd_next *next)
{
    if (ioxd_req_header(ctx, "accept-encoding").p)
        ioxd_on_head(ctx, decide, nullptr);
    ioxd_next_run(ctx, next);
}

int ioxd_compress_configure(const ioxd_compress_config *config)
{
    ioxd_compress_config cfg = g_cfg;
    if (config->brotli_quality)
        cfg.brotli_quality = config->brotli_quality;
    if (config->brotli_window)
        cfg.brotli_window = config->brotli_window;
    if (config->gzip_level)
        cfg.gzip_level = config->gzip_level;
    if (config->min_bytes)
        cfg.min_bytes = config->min_bytes;
    if (cfg.brotli_quality < 0 || cfg.brotli_quality > 11) {
        fprintf(stderr, "ioxd_compress_configure: brotli_quality must be 0..11\n");
        return -1;
    }
    if (cfg.brotli_window < 10 || cfg.brotli_window > 24) {
        fprintf(stderr, "ioxd_compress_configure: brotli_window must be 10..24\n");
        return -1;
    }
    if (cfg.gzip_level < 1 || cfg.gzip_level > 9) {
        fprintf(stderr, "ioxd_compress_configure: gzip_level must be 1..9\n");
        return -1;
    }
    g_cfg = cfg;
    return 0;
}

const char *ioxd_compress_codings(void)
{
#if IOXD_BROTLI && IOXD_ZLIB
    return "br, gzip";
#elif IOXD_BROTLI
    return "br";
#elif IOXD_ZLIB
    return "gzip";
#else
    return "";
#endif
}

/*
 * ioxd/compress.h - response compression, as middleware: a reply of a compressible type to a
 * client whose Accept-Encoding takes br or gzip goes out coded, brotli or zlib doing the work
 * at the engine's flush. A body that fit the slab is one call and one message with the exact
 * coded length; one that streams is coded flush by flush, chunked.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ioxd/http.h"

/* The middleware: on the root (IOXD_USE), a group, or listed after an endpoint's handler. It
 * does nothing to a request without Accept-Encoding, to a reply that is not 2xx, that already
 * carries a Content-Encoding, whose type is not text, JSON, XML, JavaScript, SVG or WebAssembly,
 * or whose whole body is smaller than min_bytes. Otherwise it picks brotli when the client takes
 * br with a q at least gzip's, gzip when it takes that, and sets Content-Encoding; Vary tells
 * caches either way. Built without brotli or zlib (make BROTLI=0, ZLIB=0, or pkg-config finding
 * neither), it is the middleware that compresses with what it has, or nothing. */
void ioxd_compress(ioxd_ctx *ctx, ioxd_next *next);

/* How hard to compress, process-wide. Zero is the default of every field. Before the run. */
typedef struct ioxd_compress_config {
    int    brotli_quality;   /* 0..11; 1 unless set: fast, and still four to eight times smaller on JSON */
    int    brotli_window;    /* lgwin 10..24; 22 unless set                                             */
    int    gzip_level;       /* 1..9; 1 unless set                                                      */
    size_t min_bytes;        /* a whole body smaller than this goes out as it is; 256 unless set        */
} ioxd_compress_config;
int ioxd_compress_configure(const ioxd_compress_config *config);   /* -1, with the reason on stderr, when a value is out of range */

/* Which codings this build compresses with: "br, gzip", "br", "gzip" or "". */
const char *ioxd_compress_codings(void);

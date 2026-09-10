/*
 * ioxd/static.h - files from a directory: the request path mapped under a mount, the type from
 * the extension, a validator and a date for conditional requests, byte ranges, a pre-compressed
 * twin when the client takes the coding - and what was served kept in memory, per worker,
 * checked against the disk so a replaced file is served new.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "ioxd/http.h"

typedef struct ioxd_static ioxd_static;

/* How a directory is served. Zero is the default of every field but dir. What a worker serves
 * it keeps in memory, up to cache_bytes of file contents, the least recently served dropped
 * first; a file larger than cache_file_max is not kept, and is read from the disk in pieces
 * each time. A kept file is checked against the disk before it is served again - its inode,
 * size and modification time - every time by default, or at most once per revalidate_ms; a file
 * that changed or went is dropped, so a deploy that replaces files shows at once. */
typedef struct ioxd_static_config {
    const char *dir;             /* the directory; required                                             */
    const char *mount;           /* the request path prefix that maps to it: "/static"; NULL or "" is the root */
    const char *index;           /* the file a directory serves: "index.html" unless set; "" for none     */
    const char *cache_control;   /* sent with every file ("max-age=3600"); NULL for none                */
    size_t      cache_bytes;     /* file bytes each worker keeps in memory; 0: 64 MB                     */
    size_t      cache_file_max;  /* a larger file is not kept, and streams from the disk; 0: 8 MB        */
    unsigned    revalidate_ms;   /* how long a kept file goes unchecked against the disk; 0: every request */
    bool        precompressed;   /* serve name.br or name.gz beside a file when the client takes the coding */
    bool        hidden;          /* serve names that begin with a dot (refused otherwise)                */
} ioxd_static_config;

/* Open a directory to serve: NULL, with the reason on stderr, when it cannot be. From the main
 * thread, before the run; at most 16 open at once. */
ioxd_static *ioxd_static_open(const ioxd_static_config *config);

/* The file the request's path names under the mount, served: 200 with the body - the .br or
 * .gz twin as Content-Encoding when there is one and Accept-Encoding takes it, with Vary - 304
 * when the client's copy is current (If-None-Match, If-Modified-Since), 206 for a byte range,
 * 404 when there is no such file or the name is refused (a dot segment, a hidden name), 405 for
 * a method other than GET and HEAD. The 404 carries no body, so a handler may add one of its
 * own after. False, with nothing written, when the path is not under the mount at all: a
 * fallback handler then goes on to its own reply. */
bool ioxd_static_serve(ioxd_ctx *ctx, ioxd_static *files);

/* A named file under the directory, whatever the path: an index, a 404 page, a single-page
 * app's shell. The same replies; 404 when it is not there. */
void ioxd_static_file(ioxd_ctx *ctx, ioxd_static *files, const char *name);

/* Give it back, with everything the workers kept: after ioxd_run returned. NULL is a no-op. */
void ioxd_static_close(ioxd_static *files);

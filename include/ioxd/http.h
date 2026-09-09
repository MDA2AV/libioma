/*
 * ioxd/http.h - the request, the response, the context a handler receives, the body read on
 * demand, the reply written as you go, and the run.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ioxd/slice.h"


#ifndef IOXD_MAX_HEADERS
#define IOXD_MAX_HEADERS      64            /* request headers kept                          */
#endif
#ifndef IOXD_MAX_PARAMS
#define IOXD_MAX_PARAMS       32            /* query parameters kept                         */
#endif
#ifndef IOXD_MAX_ROUTE_PARAMS
#define IOXD_MAX_ROUTE_PARAMS 8             /* :name captures a route pattern may have       */
#endif
#ifndef IOXD_MAX_RESP_HEADERS
#define IOXD_MAX_RESP_HEADERS 16            /* headers a reply may add                       */
#endif
#ifndef IOXD_ROUTE_ARENA
#define IOXD_ROUTE_ARENA      256           /* per-request bytes the router decodes into     */
#endif

/* ── the request ───────────────────────────────────────────────────────────────────────── */

/* All the data of a request, as slices into the connection's read buffer (decoded parameters
 * into a per-request arena), valid only until the handler returns. Read the arrays directly;
 * header names are lower-cased, so compare them with lowercase literals. The body is not here
 * until you ask for it: ioxd_body reads it whole, ioxd_body_read streams it. */
typedef struct ioxd_request {
    ioxd_slice  method;                         /* "GET", "POST", ...                        */
    ioxd_slice  target;                         /* raw request target: path plus any query   */
    ioxd_slice  path;                           /* the path, query stripped                  */
    ioxd_slice  query;                          /* raw text after '?', undecoded             */
    int         minor_version;                  /* 0 or 1 for HTTP/1.0 or 1.1                */

    ioxd_kv     headers[IOXD_MAX_HEADERS];      /* names lower-cased, values as received     */
    size_t      n_headers;
    ioxd_kv     params[IOXD_MAX_PARAMS];        /* query parameters, percent-decoded         */
    size_t      n_params;
    ioxd_kv     route_params[IOXD_MAX_ROUTE_PARAMS];   /* the :name captures, percent-decoded       */
    size_t      n_route_params;

    size_t      content_length;                 /* what the head declared; 0 if nothing      */
    bool        chunked;                        /* the body is chunked: length unknown       */
    ioxd_slice  body;                           /* the whole body, once ioxd_body read it    */
    bool        keep_alive;                     /* computed from version + Connection         */

    char        route_arena[IOXD_ROUTE_ARENA];  /* private: the router's per-request scratch */
} ioxd_request;

/* ── the response ──────────────────────────────────────────────────────────────────────── */

/* The reply being shaped, and the write slab. Body bytes wait in buf[0, len); a full slab is
 * sent and emptied, and the head (status, content type, headers) goes out in front of the first
 * send - after the chain when everything fit, earlier when the body streams - and is frozen from
 * then on (head_sent). You may write into buf + len yourself and advance len, up to cap. */
typedef struct ioxd_response {
    int         status;                         /* 200 by default                            */
    ioxd_slice  content_type;                   /* "text/plain" by default; any slice        */
    ioxd_kv     headers[IOXD_MAX_RESP_HEADERS]; /* added with ioxd_header                    */
    size_t      n_headers;
    bool        close;                          /* close the connection after this reply     */
    bool        head_sent;
    size_t      content_length;                 /* declared with ioxd_content_length         */
    bool        has_length;

    bool        chunked, failed;                /* private: how a stream is framed; peer gone */
} ioxd_response;

/* ── the context ───────────────────────────────────────────────────────────────────────── */

typedef struct ioxd_ctx {
    ioxd_request  req;
    ioxd_response res;
    void         *user;                         /* free slot: middleware hands data to the handler */
    void         *priv;                         /* the engine's own state                    */
} ioxd_ctx;

typedef void (*ioxd_handler)(ioxd_ctx *ctx);

/* Middleware runs around the handler (the onion model): shape the context, call ioxd_next_run to
 * run the rest of the chain and then the endpoint, then act on the result - or write a reply and
 * return WITHOUT calling ioxd_next_run to short-circuit (auth failure, cache hit). */
typedef struct ioxd_next ioxd_next;
typedef void (*ioxd_mw)(ioxd_ctx *ctx, ioxd_next *next);
void ioxd_next_run(ioxd_ctx *ctx, ioxd_next *next);

/* ── the body ──────────────────────────────────────────────────────────────────────────── */

/* The whole body, read into the request buffer once and returned as a slice (also req.body). It
 * must fit the buffer (16 KB by default): otherwise the slice is empty and res.status is 413,
 * which becomes the reply - a handler that streams its reply should check and stop. Not after
 * one of the reads below. */
ioxd_slice ioxd_body_all(ioxd_ctx *ctx);

/* The next bytes of the body into dst, reading until n are there or the body ends. Returns the
 * count (less than n only at the end), 0 once it is all consumed, -1 on error (the connection
 * then closes after the reply). Any size of body, nothing kept in the engine. */
int ioxd_body_read_until(ioxd_ctx *ctx, void *dst, size_t n);

/* The next chunk of a chunked body, exactly as the sender framed it, into dst: the rest of the
 * current chunk when a read stopped inside one, else the next whole one. Returns its length, 0 at
 * the last chunk, -1 on error - a chunk larger than cap is a 413 - or when the body is not
 * chunked. */
int ioxd_body_read_next_chunk(ioxd_ctx *ctx, void *dst, size_t cap);

/* ── the reply ─────────────────────────────────────────────────────────────────────────── */

/* Body writes into the slab. When it fills, it is sent - head first - and the body streams from
 * then on: chunked on HTTP/1.1, until close on HTTP/1.0, or with the length declared below.
 * Return 0, or -1 once the peer is gone (further writes are ignored). */
int  ioxd_write (ioxd_ctx *ctx, const void *data, size_t len);
/* Or write into the slab directly: reserve n bytes (flushing first when they do not fit; nullptr
 * once the peer is gone or n exceeds the slab) and advance by what was written. */
void *ioxd_reserve(ioxd_ctx *ctx, size_t n);
void  ioxd_advance(ioxd_ctx *ctx, size_t n);
int  ioxd_text  (ioxd_ctx *ctx, const char *s);                          /* a C string          */
int  ioxd_printf(ioxd_ctx *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* formatted, into the slab */

/* Shape the head. Only before it is sent: ioxd_header returns false afterwards. */
bool ioxd_header        (ioxd_ctx *ctx, const char *name, const char *value);   /* both stay valid until sent; the name is sent lower-cased */
void ioxd_content_type  (ioxd_ctx *ctx, const char *type);
void ioxd_content_length(ioxd_ctx *ctx, size_t n);       /* stream a large body with a known length */
int  ioxd_flush         (ioxd_ctx *ctx);                 /* send what is in the slab now (starts streaming) */

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

/* Start `workers` proactor threads (<= 0: one per core) serving HTTP on `port` and on every
 * listener added below, and block until SIGINT/SIGTERM. Returns 0 on clean shutdown. */
int ioxd_run(int workers, int port);

/* More ports: each ioxd_listen before the run adds one, plain when tls is NULL, TLS 1.3 terminated
 * in the kernel otherwise (a certificate store from ioxd_tls_new; TLS.md). ioxd_run's own port
 * joins them as a plain one; 0 there means only the listeners added. At most 8. -1 if refused. */
typedef struct ioxd_tls ioxd_tls;
int ioxd_listen(int port, ioxd_tls *tls);

/* The reason phrase for a status code ("OK", "Not Found", ...); "Unknown" if unlisted. */
const char *ioxd_reason(int status);

/*
 * ioxd/http.h - the request, the response, the context a handler receives, the body read on
 * demand, the reply written as you go, and the run.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ioxd/slice.h"


/* The limits that size a request and a reply. They are build-time constants of the LIBRARY:
 * an application may not redefine them, since they lay out the context the library allocates -
 * ioxd_run checks that the two sides agree and refuses to start otherwise. A request past a
 * limit is answered 400 (more headers than fit, more query parameters than fit) or 414 (more
 * decoded query than fits its arena); a reply past one is refused by the call that adds to it. */
#ifndef IOXD_MAX_HEADERS
#define IOXD_MAX_HEADERS      64            /* request headers; more is a 400                */
#endif
#ifndef IOXD_MAX_PARAMS
#define IOXD_MAX_PARAMS       32            /* query parameters; more is a 400               */
#endif
#ifndef IOXD_MAX_ROUTE_PARAMS
#define IOXD_MAX_ROUTE_PARAMS 8             /* :name captures a route pattern may have       */
#endif
#ifndef IOXD_MAX_RESP_HEADERS
#define IOXD_MAX_RESP_HEADERS 16            /* headers a reply may add                       */
#endif
#ifndef IOXD_RESP_HEAD_CAP
#define IOXD_RESP_HEAD_CAP    3072          /* bytes the added headers may serialize to      */
#endif

/* ── the request ───────────────────────────────────────────────────────────────────────── */

/* All the data of a request, as slices into the connection's read buffer (decoded parameters
 * into a per-request arena), valid only until the handler returns. Read the arrays directly;
 * header names are lower-cased, so compare them with lowercase literals. The body is not here
 * until you ask for it: ioxd_body_all reads it whole, ioxd_body_read_until streams it.
 * The engine has already checked the framing (RFC 9112): a Content-Length that is not a plain
 * number, conflicting duplicates, a Transfer-Encoding with anything but a final "chunked",
 * both fields together, a folded header line, or an HTTP/1.1 request without exactly one Host
 * never reach a handler - they are answered 400 (501 for a transfer coding we do not
 * implement) and the connection closes. */
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
    ioxd_kv     route_params[IOXD_MAX_ROUTE_PARAMS];   /* the :name captures, in pattern order      */
    size_t      n_route_params;

    size_t      content_length;                 /* what the head declared; 0 if nothing      */
    bool        chunked;                        /* the body is chunked: length unknown       */
    ioxd_slice  body;                           /* the whole body, once ioxd_body_all read it */
    bool        keep_alive;                     /* computed from version + Connection         */
    bool        expect_continue;                /* "Expect: 100-continue": the body waits for the interim reply the first body read sends */
} ioxd_request;

/* ── the response ──────────────────────────────────────────────────────────────────────── */

/* The reply being shaped. Body bytes wait in the connection's write slab (ioxd_write,
 * ioxd_printf, ioxd_reserve); a full slab is sent and emptied, and the head (status, content
 * type, headers) goes out in front of the first send - after the chain when everything fit,
 * earlier when the body streams - and is frozen from then on (head_sent).
 * What goes on the wire follows the request and the status, not only the handler: a reply to
 * HEAD, a 1xx, 204 or 304 carries no body whatever was written (HEAD keeps the Content-Length
 * the GET would have had; 1xx and 204 carry no framing header at all), a status outside
 * 100-999 goes out as 500, and a declared Content-Length that the writes do not match is
 * corrected when the reply was buffered whole and closes the connection when it streamed. */
typedef struct ioxd_response {
    int         status;                         /* 200 by default                            */
    ioxd_slice  content_type;                   /* "text/plain" by default; assign a slice that outlives the handler (a literal), or ioxd_content_type copies one */
    ioxd_kv     headers[IOXD_MAX_RESP_HEADERS]; /* added with ioxd_header: copies, names lower-cased */
    size_t      n_headers;
    bool        close;                          /* close the connection after this reply     */
    bool        head_sent;
    size_t      content_length;                 /* declared with ioxd_content_length         */
    bool        has_length;

    bool        chunked, failed;                /* private: how a stream is framed; peer gone */
    size_t      body_sent;                      /* private: body bytes sent so far           */
    size_t      head_len;                       /* private: the added headers, serialized    */
    char        head[IOXD_RESP_HEAD_CAP];
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
 * one of the reads below. A malformed chunked body is a 400 the same way.
 * With "Expect: 100-continue" the first of these reads answers "100 Continue" before waiting;
 * a handler that never reads such a body gets its reply sent and the connection closed. */
ioxd_slice ioxd_body_all(ioxd_ctx *ctx);

/* The next bytes of the body into dst, reading until n are there or the body ends. Returns the
 * count (less than n only at the end), 0 once it is all consumed (or for n == 0), -1 on error
 * (the connection then closes after the reply). Any size of body, nothing kept in the engine. */
int ioxd_body_read_until(ioxd_ctx *ctx, void *dst, size_t n);

/* The next chunk of a chunked body, exactly as the sender framed it, into dst: the rest of the
 * current chunk when a read stopped inside one, else the next whole one. Returns its length, 0 at
 * the last chunk, -1 on error - a chunk larger than cap is a 413 - or when the body is not
 * chunked. */
int ioxd_body_read_next_chunk(ioxd_ctx *ctx, void *dst, size_t cap);

/* ── the reply ─────────────────────────────────────────────────────────────────────────── */

/* Body writes into the slab. When it fills, it is sent - head first - and the body streams from
 * then on: chunked on HTTP/1.1, until close on HTTP/1.0, or with the length declared below.
 * Return 0, or -1 once the reply failed: the peer is gone, or a format could not be written
 * (further writes are ignored either way). */
int  ioxd_write (ioxd_ctx *ctx, const void *data, size_t len);
/* Or write into the slab directly: reserve n bytes (flushing first when they do not fit; nullptr
 * once the peer is gone or n exceeds the slab) and advance by what was written - never more
 * than reserved; advance clamps to the room that was there. */
void *ioxd_reserve(ioxd_ctx *ctx, size_t n);
void  ioxd_advance(ioxd_ctx *ctx, size_t n);
int  ioxd_text  (ioxd_ctx *ctx, const char *s);                          /* a C string          */
#if defined(__GNUC__) || defined(__clang__)
int  ioxd_printf(ioxd_ctx *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* formatted, into the slab */
#else
int  ioxd_printf(ioxd_ctx *ctx, const char *fmt, ...);
#endif

/* Shape the head, only before it is sent: each returns false afterwards. ioxd_header copies the
 * name (sent lower-cased) and the value, so temporaries are fine; it also returns false for a
 * name that is not an HTTP token, a value with a control byte (CR, LF, NUL: no response
 * splitting), a header the engine owns (content-length, transfer-encoding, connection), when
 * the table or its IOXD_RESP_HEAD_CAP bytes are full. "content-type" through it sets the
 * content type. */
bool ioxd_header        (ioxd_ctx *ctx, const char *name, const char *value);
bool ioxd_content_type  (ioxd_ctx *ctx, const char *type);                /* copied */
bool ioxd_content_length(ioxd_ctx *ctx, size_t n);       /* stream a large body with a known length */
int  ioxd_flush         (ioxd_ctx *ctx);                 /* send what is in the slab now (starts streaming) */

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

/* Start `workers` proactor threads (<= 0: one per core) serving HTTP on `port` and on every
 * listener added below, and block until SIGINT/SIGTERM. Returns 0 on clean shutdown, non-zero
 * when a worker failed, when the port could not be bound, or when the limits above differ
 * between this header and the library (the context would not match). May be called again
 * after it returns. */
int ioxd__run(int workers, int port, size_t ctx_size);
static inline int ioxd_run(int workers, int port)
{
    return ioxd__run(workers, port, sizeof(ioxd_ctx));
}

/* More ports: each ioxd_listen before the run adds one, plain when tls is NULL, TLS 1.3 terminated
 * in the kernel otherwise (a certificate store from ioxd_tls_new; TLS.md). ioxd_run's own port
 * joins them as a plain one; 0 there means only the listeners added. At most 8. -1 if refused. */
typedef struct ioxd_tls ioxd_tls;
int ioxd_listen(int port, ioxd_tls *tls);

/* The reason phrase for a status code ("OK", "Not Found", ...); "Unknown" if unlisted. */
const char *ioxd_reason(int status);

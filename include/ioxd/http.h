/*
 * ioxd/http.h - the request, the response, the context a handler receives, the body read on
 * demand, and the reply written as you go.
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
#ifndef IOXD_ROUTE_ARENA
#define IOXD_ROUTE_ARENA      256           /* per-request bytes the router decodes into     */
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
    ioxd_kv     route_params[IOXD_MAX_ROUTE_PARAMS];   /* the :name captures, percent-decoded       */
    size_t      n_route_params;

    size_t      content_length;                 /* what the head declared; 0 if nothing      */
    bool        chunked;                        /* the body is chunked: length unknown       */
    ioxd_slice  body;                           /* the whole body, once ioxd_body_all read it */
    bool        keep_alive;                     /* computed from version + Connection         */
    bool        expect_continue;                /* "Expect: 100-continue": the body waits for the interim reply the first body read sends */
    char        route_arena[IOXD_ROUTE_ARENA];  /* private: the router's per-request scratch */
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

/* ── the request, looked up ────────────────────────────────────────────────────────────── */

/* A header of the request by name, or a query parameter by key: the value of the first one, or
 * an absent slice (p NULL, len 0) when there is none - ioxd_slice_eq(v, "") is true of both an
 * absent value and an empty one; v.p tells them apart. Header names arrive lower-cased, so
 * ask with lowercase. */
ioxd_slice ioxd_req_header(const ioxd_ctx *ctx, const char *name);
ioxd_slice ioxd_req_param (const ioxd_ctx *ctx, const char *key);

/* The weight Accept-Encoding gives a coding: its own q (1 when named without one), else the q
 * of "*", else 0 - so 0 means the client does not take it, whether refused or never offered. */
double ioxd_accepts_encoding(const ioxd_ctx *ctx, const char *coding);

/* ── the moments of a reply, for middleware ────────────────────────────────────────────── */

/* The head is final at the reply's first flush: the status, the content type and whether the
 * body fit the slab are known, and the head can still be shaped. Middleware that must decide
 * then - response compression - sets a delegate before running the chain, and the engine calls
 * it once, before the head is serialized, with the body bytes in the slab so far and whether
 * they are all of it (ioxd_header still works inside it). Not called for HEAD. False once the
 * head is out. */
typedef void (*ioxd_head_fn)(ioxd_ctx *ctx, void *arg, size_t buffered, bool whole);
bool ioxd_on_head(ioxd_ctx *ctx, ioxd_head_fn fn, void *arg);

/* A filter on the body - a coder: what the handler wrote passes through run, and what run
 * writes is what is framed and sent. run takes input (all of it may not fit: *in_len comes
 * back as what was consumed, *out_len as what was produced into out) and an op - MORE while
 * the body goes on, FLUSH when the handler flushed on purpose, so a live feed moves, FINISH
 * once the body is complete, called again until nothing is pending. It returns 1 while it has
 * more output than fit, 0 when done with the call, -1 on an error, which fails the reply. end
 * runs once when the reply is over, however it ended. Installed at the on_head moment, before
 * the head is out (false after): the engine then frames the reply chunked when the body
 * streams - a declared length is the plaintext's - and with the exact coded length when it was
 * buffered whole, still one message. */
enum ioxd_filter_op { IOXD_FILTER_MORE, IOXD_FILTER_FLUSH, IOXD_FILTER_FINISH };
typedef struct ioxd_filter {
    int  (*run)(void *arg, const void *in, size_t *in_len, void *out, size_t *out_len, enum ioxd_filter_op op);
    void (*end)(void *arg);
    void  *arg;
} ioxd_filter;
bool ioxd_reply_filter(ioxd_ctx *ctx, const ioxd_filter *filter);

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

/* The reason phrase for a status code ("OK", "Not Found", ...); "Unknown" if unlisted. */
const char *ioxd_reason(int status);

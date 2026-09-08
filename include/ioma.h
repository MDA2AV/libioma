/*
 * ioma.h - the libioma API: what you write endpoints against. The one header that is installed.
 *
 * Every request gets a context: the request, the response, and a slot for your own data. The
 * context is passed to each middleware and to the handler, and any of them may read the request
 * and shape the response. The request is all the data, as slices into the read buffer; its body
 * is read from the wire only when asked for (whole, or streamed), and whatever is left unread is
 * drained after the handler. The response holds the write slab: a handler writes the body into
 * it, and the framework sends the head in front - in one send when it fits, streamed when not.
 * The handler runs on the connection's coroutine, so a read or a write that has to touch the
 * wire simply suspends it until the I/O completes.
 *
 *     static void user(ioma_ctx *ctx) {
 *         ioma_slice id = ctx->req.route_params[0].value;              // the :id of "/users/:id"
 *         ioma_printf(c, "user %.*s\n", (int)id.len, id.p);
 *     }
 *     int main(void) {
 *         ioma_route("GET", "/users/:id", user);
 *         return ioma_run(0, 8080);                            // one worker per core
 *     }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef IOMA_MAX_HEADERS
#define IOMA_MAX_HEADERS      64            /* request headers kept                          */
#endif
#ifndef IOMA_MAX_PARAMS
#define IOMA_MAX_PARAMS       32            /* query parameters kept                         */
#endif
#ifndef IOMA_MAX_ROUTE_PARAMS
#define IOMA_MAX_ROUTE_PARAMS 8             /* :name captures a route pattern may have       */
#endif
#ifndef IOMA_MAX_RESP_HEADERS
#define IOMA_MAX_RESP_HEADERS 16            /* headers a reply may add                       */
#endif

/* A slice: pointer + length, the C span. Not NUL-terminated. */
typedef struct { const char *p; size_t len; } ioma_slice;

/* One key/value pair of slices: a header, a query parameter, a route parameter. */
typedef struct { ioma_slice key, value; } ioma_kv;

/* ── the request ───────────────────────────────────────────────────────────────────────── */

/* All the data of a request, as slices into the connection's read buffer (decoded parameters
 * into a per-request arena), valid only until the handler returns. Read the arrays directly;
 * header names are lower-cased, so compare them with lowercase literals. The body is not here
 * until you ask for it: ioma_body reads it whole, ioma_body_read streams it. */
typedef struct ioma_request {
    ioma_slice  method;                         /* "GET", "POST", ...                        */
    ioma_slice  target;                         /* raw request target: path plus any query   */
    ioma_slice  path;                           /* the path, query stripped                  */
    ioma_slice  query;                          /* raw text after '?', undecoded             */
    int         minor_version;                  /* 0 or 1 for HTTP/1.0 or 1.1                */

    ioma_kv     headers[IOMA_MAX_HEADERS];      /* names lower-cased, values as received     */
    size_t      n_headers;
    ioma_kv     params[IOMA_MAX_PARAMS];        /* query parameters, percent-decoded         */
    size_t      n_params;
    ioma_kv     route_params[IOMA_MAX_ROUTE_PARAMS];   /* the :name captures, in pattern order      */
    size_t      n_route_params;

    size_t      content_length;                 /* what the head declared; 0 if nothing      */
    bool        chunked;                        /* the body is chunked: length unknown       */
    ioma_slice  body;                           /* the whole body, once ioma_body read it    */
    bool        keep_alive;                     /* computed from version + Connection         */
} ioma_request;

/* ── the response ──────────────────────────────────────────────────────────────────────── */

/* The reply being shaped, and the write slab. Body bytes wait in buf[0, len); a full slab is
 * sent and emptied, and the head (status, content type, headers) goes out in front of the first
 * send - after the chain when everything fit, earlier when the body streams - and is frozen from
 * then on (head_sent). You may write into buf + len yourself and advance len, up to cap. */
typedef struct ioma_response {
    int         status;                         /* 200 by default                            */
    ioma_slice  content_type;                   /* "text/plain" by default; any slice        */
    ioma_kv     headers[IOMA_MAX_RESP_HEADERS]; /* added with ioma_header                    */
    size_t      n_headers;
    bool        close;                          /* close the connection after this reply     */
    bool        head_sent;
    size_t      content_length;                 /* declared with ioma_content_length         */
    bool        has_length;

    bool        chunked, failed;                /* private: how a stream is framed; peer gone */
} ioma_response;

/* ── the context ───────────────────────────────────────────────────────────────────────── */

typedef struct ioma_ctx {
    ioma_request  req;
    ioma_response res;
    void         *user;                         /* free slot: middleware hands data to the handler */
    void         *priv;                         /* the engine's own state                    */
} ioma_ctx;

typedef void (*ioma_handler)(ioma_ctx *ctx);

/* Middleware runs around the handler (the onion model): shape the context, call ioma_next_run to
 * run the rest of the chain and then the endpoint, then act on the result - or write a reply and
 * return WITHOUT calling ioma_next_run to short-circuit (auth failure, cache hit). */
typedef struct ioma_next ioma_next;
typedef void (*ioma_mw)(ioma_ctx *ctx, ioma_next *next);
void ioma_next_run(ioma_ctx *ctx, ioma_next *next);

/* ── the body ──────────────────────────────────────────────────────────────────────────── */

/* The whole body, read into the request buffer once and returned as a slice (also req.body). It
 * must fit the buffer (16 KB by default): otherwise the slice is empty and res.status is 413,
 * which becomes the reply - a handler that streams its reply should check and stop. Not after
 * one of the reads below. */
ioma_slice ioma_body_all(ioma_ctx *ctx);

/* The next bytes of the body into dst, reading until n are there or the body ends. Returns the
 * count (less than n only at the end), 0 once it is all consumed, -1 on error (the connection
 * then closes after the reply). Any size of body, nothing kept in the engine. */
int ioma_body_read_until(ioma_ctx *ctx, void *dst, size_t n);

/* The next chunk of a chunked body, exactly as the sender framed it, into dst: the rest of the
 * current chunk when a read stopped inside one, else the next whole one. Returns its length, 0 at
 * the last chunk, -1 on error - a chunk larger than cap is a 413 - or when the body is not
 * chunked. */
int ioma_body_read_next_chunk(ioma_ctx *ctx, void *dst, size_t cap);

/* ── the reply ─────────────────────────────────────────────────────────────────────────── */

/* Body writes into the slab. When it fills, it is sent - head first - and the body streams from
 * then on: chunked on HTTP/1.1, until close on HTTP/1.0, or with the length declared below.
 * Return 0, or -1 once the peer is gone (further writes are ignored). */
int  ioma_write (ioma_ctx *ctx, const void *data, size_t len);
/* Or write into the slab directly: reserve n bytes (flushing first when they do not fit; nullptr
 * once the peer is gone or n exceeds the slab) and advance by what was written. */
void *ioma_reserve(ioma_ctx *ctx, size_t n);
void  ioma_advance(ioma_ctx *ctx, size_t n);
int  ioma_text  (ioma_ctx *ctx, const char *s);                          /* a C string          */
int  ioma_printf(ioma_ctx *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* formatted, into the slab */

/* Shape the head. Only before it is sent: ioma_header returns false afterwards. */
bool ioma_header        (ioma_ctx *ctx, const char *name, const char *value);   /* both stay valid until sent; the name is sent lower-cased */
void ioma_content_type  (ioma_ctx *ctx, const char *type);
void ioma_content_length(ioma_ctx *ctx, size_t n);       /* stream a large body with a known length */
int  ioma_flush         (ioma_ctx *ctx);                 /* send what is in the slab now (starts streaming) */

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

/* Everything a request carries is a slice: bytes with a length, not NUL-terminated, valid until
 * the handler returns. These compare and convert one without copying it. */
bool       ioma_slice_eq         (ioma_slice s, const char *cstr);     /* exact                     */
bool       ioma_slice_eq_ci      (ioma_slice s, const char *cstr);     /* ASCII case-insensitive    */
bool       ioma_slice_starts_with(ioma_slice s, const char *prefix);
bool       ioma_slice_ends_with  (ioma_slice s, const char *suffix);
ioma_slice ioma_slice_trim       (ioma_slice s);                       /* no leading/trailing space, tab, CR, LF */

/* A NUL-terminated copy in buf, for whatever wants a C string. False when it did not fit: buf
 * then holds what fit, still terminated (cap 0 writes nothing). */
bool ioma_cstr(ioma_slice s, char *buf, size_t cap);

/* Conversions. The whole slice must be the value - nothing around it, nothing after it - and a
 * number that does not fit the type fails. On failure *out is left alone and false comes back,
 * so "0" and "not a number" cannot be confused. Integers: an optional '-' and decimal digits.
 * Doubles: also a fraction and an exponent ("2.5", ".5", "1e-3"); never inf, nan or hex; too
 * large fails, too small rounds towards zero.
 * Booleans: true/false, 1/0, yes/no, on/off, any case. */
bool ioma_to_int   (ioma_slice s, int      *out);
bool ioma_to_i64   (ioma_slice s, int64_t  *out);
bool ioma_to_u64   (ioma_slice s, uint64_t *out);
bool ioma_to_double(ioma_slice s, double   *out);
bool ioma_to_bool  (ioma_slice s, bool     *out);

/* Parse "k=v&k2=v2" - a query string, a form body - into out, up to cap pairs. Keys and values
 * that need it ('+', %XX) are decoded into arena and point there; the rest are views of s.
 * Returns the pair count. A pair that does not fit the arena is skipped. */
size_t ioma_kv_parse(const char *text, size_t len, ioma_kv *out, size_t cap, char *arena, size_t arena_cap);

/* ── routing ───────────────────────────────────────────────────────────────────────────── */

/* Endpoints live in groups, and groups nest. A group is a path prefix plus middleware: an
 * endpoint "/users" in a group "/api" under a group "/v1" answers at "/v1/api/users", wrapped by
 * the middleware of every group above it, outermost first, then its own. NULL as the group is
 * the root: no prefix, and the middleware given to ioma_use.
 *
 * Register everything before ioma_run, from the main thread. ioma_run resolves it once: every
 * endpoint's full path into a segment tree and its middleware into one flat chain, which the
 * workers then share read-only. A request costs one walk down the tree - no scan, no regex - and
 * one call through its chain. */
typedef struct ioma_group    ioma_group;
typedef struct ioma_endpoint ioma_endpoint;

ioma_group *ioma_group_new(ioma_group *parent, const char *prefix);   /* "/api"; "" for middleware only */
void        ioma_group_use(ioma_group *group, ioma_mw mw);            /* wraps everything below it     */

/* An endpoint: method matched exactly; path matched by segment below the group's prefix, with
 * :name captures ("/users/:id") landing in req.route_params. A static segment beats a capture at
 * any depth, and a static path that lacks the method falls through to a capture route that has
 * it. A trailing slash is tolerated. */
ioma_endpoint *ioma_route(ioma_group *group, const char *method, const char *path, ioma_handler fn);
void           ioma_endpoint_use(ioma_endpoint *endpoint, ioma_mw mw);   /* wraps this one only */

/* The verbs, for short: ioma_get(api, "/users/:id", user). */
static inline ioma_endpoint *ioma_get   (ioma_group *g, const char *path, ioma_handler fn) { return ioma_route(g, "GET",    path, fn); }
static inline ioma_endpoint *ioma_post  (ioma_group *g, const char *path, ioma_handler fn) { return ioma_route(g, "POST",   path, fn); }
static inline ioma_endpoint *ioma_put   (ioma_group *g, const char *path, ioma_handler fn) { return ioma_route(g, "PUT",    path, fn); }
static inline ioma_endpoint *ioma_patch (ioma_group *g, const char *path, ioma_handler fn) { return ioma_route(g, "PATCH",  path, fn); }
static inline ioma_endpoint *ioma_delete(ioma_group *g, const char *path, ioma_handler fn) { return ioma_route(g, "DELETE", path, fn); }

/* Root middleware: every request, the fallbacks included. */
void ioma_use(ioma_mw mw);
/* The fallback when no path matches (a built-in 404 by default). A path that matches without the
 * method gets a built-in 405 with an allow header. */
void ioma_default(ioma_handler fn);

/* ── the same, as a script ─────────────────────────────────────────────────────────────── */

/* Registration as a block-structured script: a current group, which the block after IOMA_GROUP
 * sets (the root outside any block), endpoints registered into it, with their own middleware
 * listed after the handler, and IOMA_USE adding middleware to it - so a group's middleware is
 * either listed after its prefix or added with IOMA_USE inside its block. Plain functions
 * underneath, so everything is type-checked; a group's block runs exactly once (do not break out
 * of it).
 *
 *     IOMA_USE(log);
 *     IOMA_GET("/", home);
 *     IOMA_GROUP("/api", api_header) {
 *         IOMA_GET("/ping", ping);
 *         IOMA_GROUP("/admin", require_token) {
 *             IOMA_GET("/stats", stats, timing);
 *         }
 *     }
 */
#define IOMA_MAX_MW 16                              /* middleware per group and per endpoint */
struct ioma_group_args    { const char *prefix; ioma_mw mws[IOMA_MAX_MW + 1]; };           /* +1: the ending null */
struct ioma_endpoint_args { const char *path; ioma_handler fn; ioma_mw mws[IOMA_MAX_MW + 1]; };

/* What the macros call: the current group's stack and an endpoint with a middleware list. */
ioma_group    *ioma__group_begin(struct ioma_group_args args);
ioma_group    *ioma__group_end(void);
ioma_group    *ioma__group_current(void);
ioma_endpoint *ioma__endpoint(const char *method, struct ioma_endpoint_args args);

/* The argument lists become the structs above. An argument count picks the expansion, so the
 * middleware list always has its own braces and no macro is ever invoked with an empty variadic
 * part: clean under -Wall -Wextra -pedantic, in C11 and later. */
#define IOMA__CAT2(a, b) a##b
#define IOMA__CAT(a, b)  IOMA__CAT2(a, b)
#define IOMA__PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, name, ...) name
#define IOMA__RW(path, fn, ...) (struct ioma_endpoint_args){ (path), (fn), { __VA_ARGS__, NULL } }
#define IOMA__RB(path, fn)      (struct ioma_endpoint_args){ (path), (fn), { NULL } }
#define IOMA__ROUTE_ARGS(...)   IOMA__PICK(__VA_ARGS__, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, \
                                           IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, IOMA__RW, \
                                           IOMA__RW, IOMA__RW, IOMA__RB, IOMA__RB, IOMA__RB)(__VA_ARGS__)
#define IOMA__GW(prefix, ...)   (struct ioma_group_args){ (prefix), { __VA_ARGS__, NULL } }
#define IOMA__GB(prefix)        (struct ioma_group_args){ (prefix), { NULL } }
#define IOMA__GROUP_ARGS(...)   IOMA__PICK(__VA_ARGS__, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, \
                                           IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GW, \
                                           IOMA__GW, IOMA__GW, IOMA__GW, IOMA__GB, IOMA__GB)(__VA_ARGS__)

#define IOMA_GROUP(...)                                                                             \
    for (ioma_group *IOMA__CAT(ioma__block_, __LINE__) = ioma__group_begin(IOMA__GROUP_ARGS(__VA_ARGS__)); \
         IOMA__CAT(ioma__block_, __LINE__); IOMA__CAT(ioma__block_, __LINE__) = ioma__group_end())
#define IOMA_USE(mw)            ioma_group_use(ioma__group_current(), (mw))
#define IOMA_ROUTE(method, ...) ioma__endpoint((method), IOMA__ROUTE_ARGS(__VA_ARGS__))
#define IOMA_GET(...)           IOMA_ROUTE("GET",    __VA_ARGS__)
#define IOMA_POST(...)          IOMA_ROUTE("POST",   __VA_ARGS__)
#define IOMA_PUT(...)           IOMA_ROUTE("PUT",    __VA_ARGS__)
#define IOMA_PATCH(...)         IOMA_ROUTE("PATCH",  __VA_ARGS__)
#define IOMA_DELETE(...)        IOMA_ROUTE("DELETE", __VA_ARGS__)
#define IOMA_DEFAULT(fn)        ioma_default(fn)

/* ── JSON, written as you go ───────────────────────────────────────────────────────────── */

/* A forward-only JSON writer, the shape of .NET's Utf8JsonWriter: no tree, no allocation. The
 * bytes go straight into the reply - or a raw pipe, or a buffer - escaped as they are written,
 * and stream out as the slab fills. Nesting and commas are tracked, so a handler just says what
 * it means:
 *
 *     ioma_json j = ioma_json_reply(ctx);                  // content-type: application/json
 *     ioma_json_object(&j);
 *         ioma_json_key(&j, "id");    ioma_json_int(&j, id);
 *         ioma_json_key(&j, "name");  ioma_json_string(&j, name);
 *         ioma_json_key(&j, "tags");  ioma_json_array(&j);
 *             ioma_json_cstr(&j, "new");
 *         ioma_json_end(&j);
 *     ioma_json_end(&j);
 *
 * Strings are taken as UTF-8 and passed through; '"', '\' and control characters are escaped.
 * Every call returns false once the sink is gone (the peer left; the buffer is full) or the
 * nesting passed IOMA_JSON_DEPTH, and the rest is dropped, so checking the last call is enough. */
#define IOMA_JSON_DEPTH 64
typedef struct ioma_json {
    enum { IOMA_JSON_TO_REPLY, IOMA_JSON_TO_PIPE, IOMA_JSON_TO_MEM } kind;
    union {
        ioma_ctx         *ctx;
        struct ioma_pipe *pipe;
        struct { char *p; size_t cap, *len; } mem;
    } to;
    uint64_t has_value;                         /* per level: a value is there, so a comma is due */
    uint64_t is_object;                         /* per level: it closes with '}' rather than ']'  */
    unsigned depth;
    bool     after_key;                         /* the next value follows a key: no comma        */
    bool     failed;
} ioma_json;

ioma_json ioma_json_reply(ioma_ctx *ctx);                        /* into the reply; sets its content type */
ioma_json ioma_json_pipe (struct ioma_pipe *pipe);               /* into a raw pipe's slab                */
ioma_json ioma_json_mem  (char *buf, size_t cap, size_t *len);   /* into memory; *len is what was written */

bool ioma_json_object(ioma_json *j);                             /* {                          */
bool ioma_json_array (ioma_json *j);                             /* [                          */
bool ioma_json_end   (ioma_json *j);                             /* } or ], whichever is open  */
bool ioma_json_key   (ioma_json *j, const char *name);           /* "name":                    */
bool ioma_json_string(ioma_json *j, ioma_slice s);               /* "...", escaped             */
bool ioma_json_cstr  (ioma_json *j, const char *s);
bool ioma_json_int   (ioma_json *j, int64_t v);
bool ioma_json_uint  (ioma_json *j, uint64_t v);
bool ioma_json_double(ioma_json *j, double v);                   /* the shortest that reads back the same; nan and inf become null */
bool ioma_json_bool  (ioma_json *j, bool v);
bool ioma_json_null  (ioma_json *j);
bool ioma_json_raw   (ioma_json *j, ioma_slice json);            /* already JSON: copied as is */

/* ── pipes ─────────────────────────────────────────────────────────────────────────────── */

/* A connection as a pipe: a reader over the bytes the kernel received and a writer over a slab.
 * Every call that must wait suspends the connection's coroutine, and the worker's loop resumes
 * it on the completion, so a handler reads and writes in straight-line code. The HTTP engine is
 * one such handler; ioma_run_pipes runs one of yours on raw TCP connections instead. */
typedef struct ioma_pipe ioma_pipe;
typedef void (*ioma_pipe_handler)(ioma_pipe *pipe);
int ioma_run_pipes(int workers, int port, ioma_pipe_handler fn);      /* like ioma_run, without HTTP */

/* Reading. The live bytes are the ones received and not yet consumed, always handed out as one
 * contiguous span - in place in the kernel's buffer when they lie within one. read returns them
 * once some are unexamined, otherwise it waits for more; examine says how many were looked at
 * without being consumed, so the next read waits for more rather than returning the same bytes;
 * drop consumes; keep consumes but leaves the bytes where they are, contiguous with earlier kept
 * bytes and valid until release; copy is the plain read into your own buffer. read and copy
 * return 0 at the end of input, IOMA_PIPE_GONE on a dead peer, IOMA_PIPE_FULL when kept plus
 * live bytes would exceed the pipe's buffer. */
#define IOMA_PIPE_GONE (-1)
#define IOMA_PIPE_FULL (-2)
int         ioma_pipe_read   (ioma_pipe *pipe, ioma_slice *live);
void        ioma_pipe_examine(ioma_pipe *pipe, size_t n);
void        ioma_pipe_drop   (ioma_pipe *pipe, size_t n);
const char *ioma_pipe_keep   (ioma_pipe *pipe, size_t n);
void        ioma_pipe_release(ioma_pipe *pipe);
int         ioma_pipe_copy   (ioma_pipe *pipe, void *dst, size_t n);

/* Writing: a slab, sent on flush. reserve n bytes to write into directly and advance by what was
 * written, or write to copy in; send is write then flush. -1 once the peer is gone. */
void  *ioma_pipe_reserve(ioma_pipe *pipe, size_t n);
void   ioma_pipe_advance(ioma_pipe *pipe, size_t n);
int    ioma_pipe_write  (ioma_pipe *pipe, const void *data, size_t n);
int    ioma_pipe_flush  (ioma_pipe *pipe);
int    ioma_pipe_send   (ioma_pipe *pipe, const void *data, size_t n);

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

/* Start `workers` proactor threads (<= 0: one per core) serving HTTP on `port`, and block until
 * SIGINT/SIGTERM. Returns 0 on clean shutdown. */
int ioma_run(int workers, int port);

/* The reason phrase for a status code ("OK", "Not Found", ...); "Unknown" if unlisted. */
const char *ioma_reason(int status);

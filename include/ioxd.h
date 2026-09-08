/*
 * ioxd.h - the libioxd API: what you write endpoints against. The one header that is installed.
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
 *     static void user(ioxd_ctx *ctx) {
 *         ioxd_slice id = ctx->req.route_params[0].value;              // the :id of "/users/:id"
 *         ioxd_printf(c, "user %.*s\n", (int)id.len, id.p);
 *     }
 *     int main(void) {
 *         ioxd_route("GET", "/users/:id", user);
 *         return ioxd_run(0, 8080);                            // one worker per core
 *     }
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* A slice: pointer + length, the C span. Not NUL-terminated. */
typedef struct { const char *p; size_t len; } ioxd_slice;

/* One key/value pair of slices: a header, a query parameter, a route parameter. */
typedef struct { ioxd_slice key, value; } ioxd_kv;

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
    ioxd_kv     route_params[IOXD_MAX_ROUTE_PARAMS];   /* the :name captures, in pattern order      */
    size_t      n_route_params;

    size_t      content_length;                 /* what the head declared; 0 if nothing      */
    bool        chunked;                        /* the body is chunked: length unknown       */
    ioxd_slice  body;                           /* the whole body, once ioxd_body read it    */
    bool        keep_alive;                     /* computed from version + Connection         */
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

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

/* Everything a request carries is a slice: bytes with a length, not NUL-terminated, valid until
 * the handler returns. These compare and convert one without copying it. */
bool       ioxd_slice_eq         (ioxd_slice s, const char *cstr);     /* exact                     */
bool       ioxd_slice_eq_ci      (ioxd_slice s, const char *cstr);     /* ASCII case-insensitive    */
bool       ioxd_slice_starts_with(ioxd_slice s, const char *prefix);
bool       ioxd_slice_ends_with  (ioxd_slice s, const char *suffix);
ioxd_slice ioxd_slice_trim       (ioxd_slice s);                       /* no leading/trailing space, tab, CR, LF */

/* A NUL-terminated copy in buf, for whatever wants a C string. False when it did not fit: buf
 * then holds what fit, still terminated (cap 0 writes nothing). */
bool ioxd_cstr(ioxd_slice s, char *buf, size_t cap);

/* Conversions. The whole slice must be the value - nothing around it, nothing after it - and a
 * number that does not fit the type fails. On failure *out is left alone and false comes back,
 * so "0" and "not a number" cannot be confused. Integers: an optional '-' and decimal digits.
 * Doubles: also a fraction and an exponent ("2.5", ".5", "1e-3"); never inf, nan or hex; too
 * large fails, too small rounds towards zero.
 * Booleans: true/false, 1/0, yes/no, on/off, any case. */
bool ioxd_to_int   (ioxd_slice s, int      *out);
bool ioxd_to_i64   (ioxd_slice s, int64_t  *out);
bool ioxd_to_u64   (ioxd_slice s, uint64_t *out);
bool ioxd_to_double(ioxd_slice s, double   *out);
bool ioxd_to_bool  (ioxd_slice s, bool     *out);

/* Parse "k=v&k2=v2" - a query string, a form body - into out, up to cap pairs. Keys and values
 * that need it ('+', %XX) are decoded into arena and point there; the rest are views of s.
 * Returns the pair count. A pair that does not fit the arena is skipped. */
size_t ioxd_kv_parse(const char *text, size_t len, ioxd_kv *out, size_t cap, char *arena, size_t arena_cap);

/* ── routing ───────────────────────────────────────────────────────────────────────────── */

/* Endpoints live in groups, and groups nest. A group is a path prefix plus middleware: an
 * endpoint "/users" in a group "/api" under a group "/v1" answers at "/v1/api/users", wrapped by
 * the middleware of every group above it, outermost first, then its own. NULL as the group is
 * the root: no prefix, and the middleware given to ioxd_use.
 *
 * Register everything before ioxd_run, from the main thread. ioxd_run resolves it once: every
 * endpoint's full path into a segment tree and its middleware into one flat chain, which the
 * workers then share read-only. A request costs one walk down the tree - no scan, no regex - and
 * one call through its chain. */
typedef struct ioxd_group    ioxd_group;
typedef struct ioxd_endpoint ioxd_endpoint;

ioxd_group *ioxd_group_new(ioxd_group *parent, const char *prefix);   /* "/api"; "" for middleware only */
void        ioxd_group_use(ioxd_group *group, ioxd_mw mw);            /* wraps everything below it     */

/* An endpoint: method matched exactly; path matched by segment below the group's prefix, with
 * :name captures ("/users/:id") landing in req.route_params. A static segment beats a capture at
 * any depth, and a static path that lacks the method falls through to a capture route that has
 * it. A trailing slash is tolerated. */
ioxd_endpoint *ioxd_route(ioxd_group *group, const char *method, const char *path, ioxd_handler fn);
void           ioxd_endpoint_use(ioxd_endpoint *endpoint, ioxd_mw mw);   /* wraps this one only */

/* The verbs, for short: ioxd_get(api, "/users/:id", user). */
static inline ioxd_endpoint *ioxd_get   (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "GET",    path, fn); }
static inline ioxd_endpoint *ioxd_post  (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "POST",   path, fn); }
static inline ioxd_endpoint *ioxd_put   (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "PUT",    path, fn); }
static inline ioxd_endpoint *ioxd_patch (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "PATCH",  path, fn); }
static inline ioxd_endpoint *ioxd_delete(ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "DELETE", path, fn); }

/* Root middleware: every request, the fallbacks included. */
void ioxd_use(ioxd_mw mw);
/* The fallback when no path matches (a built-in 404 by default). A path that matches without the
 * method gets a built-in 405 with an allow header. */
void ioxd_default(ioxd_handler fn);

/* ── the same, as a script ─────────────────────────────────────────────────────────────── */

/* Registration as a block-structured script: a current group, which the block after IOXD_GROUP
 * sets (the root outside any block), endpoints registered into it, with their own middleware
 * listed after the handler, and IOXD_USE adding middleware to it - so a group's middleware is
 * either listed after its prefix or added with IOXD_USE inside its block. Plain functions
 * underneath, so everything is type-checked; a group's block runs exactly once (do not break out
 * of it).
 *
 *     IOXD_USE(log);
 *     IOXD_GET("/", home);
 *     IOXD_GROUP("/api", api_header) {
 *         IOXD_GET("/ping", ping);
 *         IOXD_GROUP("/admin", require_token) {
 *             IOXD_GET("/stats", stats, timing);
 *         }
 *     }
 */
#define IOXD_MAX_MW 16                              /* middleware per group and per endpoint */
struct ioxd_group_args    { const char *prefix; ioxd_mw mws[IOXD_MAX_MW + 1]; };           /* +1: the ending null */
struct ioxd_endpoint_args { const char *path; ioxd_handler fn; ioxd_mw mws[IOXD_MAX_MW + 1]; };

/* What the macros call: the current group's stack and an endpoint with a middleware list. */
ioxd_group    *ioxd__group_begin(struct ioxd_group_args args);
ioxd_group    *ioxd__group_end(void);
ioxd_group    *ioxd__group_current(void);
ioxd_endpoint *ioxd__endpoint(const char *method, struct ioxd_endpoint_args args);

/* The argument lists become the structs above. An argument count picks the expansion, so the
 * middleware list always has its own braces and no macro is ever invoked with an empty variadic
 * part: clean under -Wall -Wextra -pedantic, in C11 and later. */
#define IOXD__CAT2(a, b) a##b
#define IOXD__CAT(a, b)  IOXD__CAT2(a, b)
#define IOXD__PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, name, ...) name
#define IOXD__RW(path, fn, ...) (struct ioxd_endpoint_args){ (path), (fn), { __VA_ARGS__, NULL } }
#define IOXD__RB(path, fn)      (struct ioxd_endpoint_args){ (path), (fn), { NULL } }
#define IOXD__ROUTE_ARGS(...)   IOXD__PICK(__VA_ARGS__, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, \
                                           IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, \
                                           IOXD__RW, IOXD__RW, IOXD__RB, IOXD__RB, IOXD__RB)(__VA_ARGS__)
#define IOXD__GW(prefix, ...)   (struct ioxd_group_args){ (prefix), { __VA_ARGS__, NULL } }
#define IOXD__GB(prefix)        (struct ioxd_group_args){ (prefix), { NULL } }
#define IOXD__GROUP_ARGS(...)   IOXD__PICK(__VA_ARGS__, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, \
                                           IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, \
                                           IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GB, IOXD__GB)(__VA_ARGS__)

#define IOXD_GROUP(...)                                                                             \
    for (ioxd_group *IOXD__CAT(ioxd__block_, __LINE__) = ioxd__group_begin(IOXD__GROUP_ARGS(__VA_ARGS__)); \
         IOXD__CAT(ioxd__block_, __LINE__); IOXD__CAT(ioxd__block_, __LINE__) = ioxd__group_end())
#define IOXD_USE(mw)            ioxd_group_use(ioxd__group_current(), (mw))
#define IOXD_ROUTE(method, ...) ioxd__endpoint((method), IOXD__ROUTE_ARGS(__VA_ARGS__))
#define IOXD_GET(...)           IOXD_ROUTE("GET",    __VA_ARGS__)
#define IOXD_POST(...)          IOXD_ROUTE("POST",   __VA_ARGS__)
#define IOXD_PUT(...)           IOXD_ROUTE("PUT",    __VA_ARGS__)
#define IOXD_PATCH(...)         IOXD_ROUTE("PATCH",  __VA_ARGS__)
#define IOXD_DELETE(...)        IOXD_ROUTE("DELETE", __VA_ARGS__)
#define IOXD_DEFAULT(fn)        ioxd_default(fn)

/* ── JSON, written as you go ───────────────────────────────────────────────────────────── */

/* A forward-only JSON writer, the shape of .NET's Utf8JsonWriter: no tree, no allocation. The
 * bytes go straight into the reply - or a raw pipe, or a buffer - escaped as they are written,
 * and stream out as the slab fills. Nesting and commas are tracked, so a handler just says what
 * it means:
 *
 *     ioxd_json j = ioxd_json_reply(ctx);                  // content-type: application/json
 *     ioxd_json_object(&j);
 *         ioxd_json_key(&j, "id");    ioxd_json_int(&j, id);
 *         ioxd_json_key(&j, "name");  ioxd_json_string(&j, name);
 *         ioxd_json_key(&j, "tags");  ioxd_json_array(&j);
 *             ioxd_json_cstr(&j, "new");
 *         ioxd_json_end(&j);
 *     ioxd_json_end(&j);
 *
 * Strings are taken as UTF-8 and passed through; '"', '\' and control characters are escaped.
 * Every call returns false once the sink is gone (the peer left; the buffer is full) or the
 * nesting passed IOXD_JSON_DEPTH, and the rest is dropped, so checking the last call is enough. */
#define IOXD_JSON_DEPTH 64
typedef struct ioxd_json {
    enum { IOXD_JSON_TO_REPLY, IOXD_JSON_TO_PIPE, IOXD_JSON_TO_MEM } kind;
    union {
        ioxd_ctx         *ctx;
        struct ioxd_pipe *pipe;
        struct { char *p; size_t cap, *len; } mem;
    } to;
    uint64_t has_value;                         /* per level: a value is there, so a comma is due */
    uint64_t is_object;                         /* per level: it closes with '}' rather than ']'  */
    unsigned depth;
    bool     after_key;                         /* the next value follows a key: no comma        */
    bool     failed;
} ioxd_json;

ioxd_json ioxd_json_reply(ioxd_ctx *ctx);                        /* into the reply; sets its content type */
ioxd_json ioxd_json_pipe (struct ioxd_pipe *pipe);               /* into a raw pipe's slab                */
ioxd_json ioxd_json_mem  (char *buf, size_t cap, size_t *len);   /* into memory; *len is what was written */

bool ioxd_json_object(ioxd_json *j);                             /* {                          */
bool ioxd_json_array (ioxd_json *j);                             /* [                          */
bool ioxd_json_end   (ioxd_json *j);                             /* } or ], whichever is open  */
bool ioxd_json_key   (ioxd_json *j, const char *name);           /* "name":                    */
bool ioxd_json_string(ioxd_json *j, ioxd_slice s);               /* "...", escaped             */
bool ioxd_json_cstr  (ioxd_json *j, const char *s);              /* NULL is null            */
bool ioxd_json_int   (ioxd_json *j, int64_t v);
bool ioxd_json_uint  (ioxd_json *j, uint64_t v);
bool ioxd_json_double(ioxd_json *j, double v);                   /* the shortest that reads back the same; nan and inf become null */
bool ioxd_json_bool  (ioxd_json *j, bool v);
bool ioxd_json_null  (ioxd_json *j);
bool ioxd_json_raw   (ioxd_json *j, ioxd_slice json);            /* already JSON: copied as is */

/* A value by its C type, and a key with one: the _Generic picks ioxd_json_int for the integer
 * types, _uint for the unsigned ones, _double for float and double, _bool, _cstr for a char
 * pointer, _string for a slice. */
#define IOXD_JSON_VALUE(j, x) _Generic((x),                                                        \
        bool: ioxd_json_bool,                                                                      \
        char: ioxd_json_int, signed char: ioxd_json_int, short: ioxd_json_int,                     \
        int: ioxd_json_int, long: ioxd_json_int, long long: ioxd_json_int,                         \
        unsigned char: ioxd_json_uint, unsigned short: ioxd_json_uint, unsigned: ioxd_json_uint,   \
        unsigned long: ioxd_json_uint, unsigned long long: ioxd_json_uint,                         \
        float: ioxd_json_double, double: ioxd_json_double,                                         \
        char *: ioxd_json_cstr, const char *: ioxd_json_cstr,                                      \
        ioxd_slice: ioxd_json_string)((j), (x))
#define IOXD_JSON_FIELD(j, name, x) (ioxd_json_key((j), (name)) && IOXD_JSON_VALUE((j), (x)))

/* A struct described once, serialized with one call. The description is a list of fields, each
 * line its kind, its C type (or, for a nested struct, that struct's name) and its name:
 *
 *     #define USER_FIELDS(X)                                                      \
 *         X(VALUE,    int64_t,      id)        // a scalar, written by its type    \
 *         X(VALUE,    const char *, name)                                          \
 *         X(OBJECT,   address,      address)   // a nested struct, by value        \
 *         X(OPTIONAL, address,      billing)   // a pointer to one; null when NULL \
 *         X(ARRAY,    const char *, tags,   n_tags)     // scalars, and the count field \
 *         X(OBJECTS,  order,        orders, n_orders)   // nested structs, and the count
 *     IOXD_JSON_STRUCT(user, USER_FIELDS)     // struct user, and user_to_json(ioxd_json *, const struct user *)
 *
 * IOXD_JSON_STRUCT defines the struct and the function; IOXD_JSON_WRITER only the function, for
 * a struct declared elsewhere with the same fields. A nested struct's own IOXD_JSON_STRUCT comes
 * first. Counts are size_t; arrays are pointers to their first element. */
#define IOXD_JSON_STRUCT(name, FIELDS)                                                             \
    struct name { FIELDS(IOXD__JSON_MEMBER) };                                                     \
    IOXD_JSON_WRITER(name, FIELDS)
#define IOXD_JSON_WRITER(name, FIELDS)                                                             \
    static inline bool name##_to_json(ioxd_json *j, const struct name *v)                         \
    {                                                                                              \
        ioxd_json_object(j);                                                                       \
        FIELDS(IOXD__JSON_WRITE)                                                                   \
        return ioxd_json_end(j);                                                                   \
    }

/* What each kind of line becomes: a member, and a piece of the writer. */
#define IOXD__JSON_MEMBER(kind, ...)                  IOXD__JSON_MEMBER_##kind(__VA_ARGS__)
#define IOXD__JSON_MEMBER_VALUE(type, field)          type field;
#define IOXD__JSON_MEMBER_OBJECT(sname, field)        struct sname field;
#define IOXD__JSON_MEMBER_OPTIONAL(sname, field)      const struct sname *field;
#define IOXD__JSON_MEMBER_ARRAY(type, field, count)   type *field; size_t count;
#define IOXD__JSON_MEMBER_OBJECTS(sname, field, count) const struct sname *field; size_t count;
#define IOXD__JSON_WRITE(kind, ...)                   IOXD__JSON_WRITE_##kind(__VA_ARGS__)
#define IOXD__JSON_WRITE_VALUE(type, field)           ioxd_json_key(j, #field); IOXD_JSON_VALUE(j, v->field);
#define IOXD__JSON_WRITE_OBJECT(sname, field)         ioxd_json_key(j, #field); sname##_to_json(j, &v->field);
#define IOXD__JSON_WRITE_OPTIONAL(sname, field)       ioxd_json_key(j, #field); if (v->field) sname##_to_json(j, v->field); else ioxd_json_null(j);
#define IOXD__JSON_WRITE_ARRAY(type, field, count)    ioxd_json_key(j, #field); ioxd_json_array(j); \
    for (size_t i_ = 0; i_ < v->count; i_++) { IOXD_JSON_VALUE(j, v->field[i_]); }                    \
    ioxd_json_end(j);
#define IOXD__JSON_WRITE_OBJECTS(sname, field, count) ioxd_json_key(j, #field); ioxd_json_array(j); \
    for (size_t i_ = 0; i_ < v->count; i_++) { sname##_to_json(j, &v->field[i_]); }                   \
    ioxd_json_end(j);

/* ── pipes ─────────────────────────────────────────────────────────────────────────────── */

/* A connection as a pipe: a reader over the bytes the kernel received and a writer over a slab.
 * Every call that must wait suspends the connection's coroutine, and the worker's loop resumes
 * it on the completion, so a handler reads and writes in straight-line code. The HTTP engine is
 * one such handler; ioxd_run_pipes runs one of yours on raw TCP connections instead. */
typedef struct ioxd_pipe ioxd_pipe;
typedef void (*ioxd_pipe_handler)(ioxd_pipe *pipe);
int ioxd_run_pipes(int workers, int port, ioxd_pipe_handler fn);      /* like ioxd_run, without HTTP */

/* Reading. The live bytes are the ones received and not yet consumed, always handed out as one
 * contiguous span - in place in the kernel's buffer when they lie within one. read returns them
 * once some are unexamined, otherwise it waits for more; examine says how many were looked at
 * without being consumed, so the next read waits for more rather than returning the same bytes;
 * drop consumes; keep consumes but leaves the bytes where they are, contiguous with earlier kept
 * bytes and valid until release; copy is the plain read into your own buffer. read and copy
 * return 0 at the end of input, IOXD_PIPE_GONE on a dead peer, IOXD_PIPE_FULL when kept plus
 * live bytes would exceed the pipe's buffer. */
#define IOXD_PIPE_GONE (-1)
#define IOXD_PIPE_FULL (-2)
int         ioxd_pipe_read   (ioxd_pipe *pipe, ioxd_slice *live);
void        ioxd_pipe_examine(ioxd_pipe *pipe, size_t n);
void        ioxd_pipe_drop   (ioxd_pipe *pipe, size_t n);
const char *ioxd_pipe_keep   (ioxd_pipe *pipe, size_t n);
void        ioxd_pipe_release(ioxd_pipe *pipe);
int         ioxd_pipe_copy   (ioxd_pipe *pipe, void *dst, size_t n);

/* Writing: a slab, sent on flush. reserve n bytes to write into directly and advance by what was
 * written, or write to copy in; send is write then flush. -1 once the peer is gone. */
void  *ioxd_pipe_reserve(ioxd_pipe *pipe, size_t n);
void   ioxd_pipe_advance(ioxd_pipe *pipe, size_t n);
int    ioxd_pipe_write  (ioxd_pipe *pipe, const void *data, size_t n);
int    ioxd_pipe_flush  (ioxd_pipe *pipe);
int    ioxd_pipe_send   (ioxd_pipe *pipe, const void *data, size_t n);

/* ── run ───────────────────────────────────────────────────────────────────────────────── */

/* Start `workers` proactor threads (<= 0: one per core) serving HTTP on `port`, and block until
 * SIGINT/SIGTERM. Returns 0 on clean shutdown. */
int ioxd_run(int workers, int port);

/* The reason phrase for a status code ("OK", "Not Found", ...); "Unknown" if unlisted. */
const char *ioxd_reason(int status);

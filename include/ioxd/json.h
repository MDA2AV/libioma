/*
 * ioxd/json.h - JSON written as you go, and a struct described once, serialized with one call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ioxd/http.h"

struct ioxd_pipe;                               /* ioxd/pipe.h */

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
 *     if (!ioxd_json_done(&j)) { ... }                     // whole: nothing failed, nothing open
 *
 * Strings are emitted byte for byte, with only '"', '\' and the control characters escaped:
 * invalid UTF-8 goes out exactly as it came in, so untrusted input has to be validated first.
 * Every call returns false once the sink is gone (the peer left; the buffer is full), the nesting
 * passed IOXD_JSON_DEPTH, or the call had no place in the document - a key outside an object, a
 * value where a key was due, an end with nothing open. The rest is then dropped, so checking the
 * last call is enough; check done() after it to catch an end that was never written. */
#define IOXD_JSON_DEPTH 63                          /* levels: one bit of each mask below apiece */
typedef struct ioxd_json {
    enum {
        IOXD_JSON_TO_REPLY,
        IOXD_JSON_TO_PIPE,
        IOXD_JSON_TO_MEM,
    } kind;

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
bool ioxd_json_done  (ioxd_json *j);                             /* nothing failed, all closed */
bool ioxd_json_key   (ioxd_json *j, const char *name);           /* "name":                    */
bool ioxd_json_string(ioxd_json *j, ioxd_slice s);               /* "...", escaped             */
bool ioxd_json_cstr  (ioxd_json *j, const char *s);              /* NULL is null            */
bool ioxd_json_int   (ioxd_json *j, int64_t v);
bool ioxd_json_uint  (ioxd_json *j, uint64_t v);
bool ioxd_json_double(ioxd_json *j, double v);                   /* the shortest that reads back the same; nan and inf become null */
bool ioxd_json_float (ioxd_json *j, float v);                    /* the same, read back as a float: 0.1f is 0.1 */
bool ioxd_json_bool  (ioxd_json *j, bool v);
bool ioxd_json_null  (ioxd_json *j);
bool ioxd_json_raw   (ioxd_json *j, ioxd_slice json);            /* already JSON: copied as is */

/* A value by its C type, and a key with one: the _Generic picks ioxd_json_int for the integer
 * types, _uint for the unsigned ones, _float and _double for those two, _bool, _cstr for a char
 * pointer, _string for a slice. */
#define IOXD_JSON_VALUE(j, x) _Generic((x),                                                        \
        bool: ioxd_json_bool,                                                                      \
        char: ioxd_json_int, signed char: ioxd_json_int, short: ioxd_json_int,                     \
        int: ioxd_json_int, long: ioxd_json_int, long long: ioxd_json_int,                         \
        unsigned char: ioxd_json_uint, unsigned short: ioxd_json_uint, unsigned: ioxd_json_uint,   \
        unsigned long: ioxd_json_uint, unsigned long long: ioxd_json_uint,                         \
        float: ioxd_json_float, double: ioxd_json_double,                                          \
        char *: ioxd_json_cstr, const char *: ioxd_json_cstr,                                      \
        ioxd_slice: ioxd_json_string)((j), (x))

/* A key and its value in one line. The answer goes through a function so that a field written for
 * its effect - `IOXD_JSON_FIELD(j, "n", n);` - is a plain statement and not a value the compiler
 * sees discarded, while `if (IOXD_JSON_FIELD(j, "n", n))` still reads it. */
static inline bool ioxd__json_wrote(bool ok) { return ok; }
#define IOXD_JSON_FIELD(j, name, x)                                                                \
    ioxd__json_wrote(ioxd_json_key((j), (name)) && IOXD_JSON_VALUE((j), (x)))

/* A struct described once, serialized with one call. The description is a list of fields, each
 * line its kind, its C type (or, for a nested struct, that struct's name) and its name:
 *
 *     #define USER_FIELDS(X)                       \
 *         X(VALUE,    int64_t,      id)            \
 *         X(VALUE,    const char *, name)          \
 *         X(OBJECT,   address,      address)       \
 *         X(OPTIONAL, address,      billing)       \
 *         X(ARRAY,    const char *, tags,   n_tags)    \
 *         X(OBJECTS,  order,        orders, n_orders)
 *     IOXD_JSON_STRUCT(user, USER_FIELDS)
 *
 * VALUE is a scalar, written by its C type; OBJECT a nested struct held by value; OPTIONAL a
 * pointer to one, null where the pointer is NULL; ARRAY scalars and the field holding their
 * count; OBJECTS the same for nested structs. IOXD_JSON_STRUCT defines the struct and the
 * function - struct user, and user_to_json(ioxd_json *, const struct user *); IOXD_JSON_WRITER
 * only the function, for a struct declared elsewhere with the same fields. A nested struct's own
 * IOXD_JSON_STRUCT comes first. Counts are size_t; arrays are pointers to their first element.
 * A note beside a field is written as a block comment, the way playground/hello/main.c writes
 * them: a // one would run on through the backslash and swallow the lines after it. */
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

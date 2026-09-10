/*
 * ioxd/slice.h - a slice, bytes with a length, and a key/value pair of them: what every request
 * carries. Compare and convert them without copying; parse a query string or a form body.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A slice: pointer + length, the C span. Not NUL-terminated. */
typedef struct { const char *p; size_t len; } ioxd_slice;

/* One key/value pair of slices: a header, a query parameter, a route parameter. */
typedef struct { ioxd_slice key, value; } ioxd_kv;

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

/* Everything a request carries is a slice: bytes with a length, not NUL-terminated, valid until
 * the handler returns. These compare and convert one without copying it. */
bool       ioxd_slice_eq         (ioxd_slice s, const char *cstr);     /* exact                     */
bool       ioxd_slice_eq_ci      (ioxd_slice s, const char *cstr);     /* ASCII case-insensitive    */
bool       ioxd_slice_starts_with(ioxd_slice s, const char *prefix);
bool       ioxd_slice_ends_with  (ioxd_slice s, const char *suffix);
ioxd_slice ioxd_slice_trim       (ioxd_slice s);                       /* no leading/trailing space, tab, CR, LF */

/* ── searching ─────────────────────────────────────────────────────────────────────────── */

/* Where something is in a slice - IndexOf: the index of the first needle, or -1 when it is not
 * there. One byte is memchr; a longer needle is matched a block of positions at a time (32 with
 * AVX2 where the CPU has it, 16 with SSE2): its first and last byte compared across the block
 * at once, the middle only where both hit. An empty needle is at 0; one longer than the slice
 * is nowhere. */
ptrdiff_t ioxd_slice_find      (ioxd_slice s, const char *needle);
ptrdiff_t ioxd_slice_find_bytes(ioxd_slice s, const void *needle, size_t n);   /* the needle as bytes  */
ptrdiff_t ioxd_slice_find_char (ioxd_slice s, char c);                         /* the first c          */
ptrdiff_t ioxd_slice_rfind_char(ioxd_slice s, char c);                         /* the last c           */

/* A part of a slice: from an index to the end, or from the start up to one. An index past the
 * end is the end. */
static inline ioxd_slice ioxd_slice_from(ioxd_slice s, size_t at)
{
    if (at > s.len)
        at = s.len;
    return (ioxd_slice){ at ? s.p + at : s.p, s.len - at };
}
static inline ioxd_slice ioxd_slice_upto(ioxd_slice s, size_t at)
{
    return (ioxd_slice){ s.p, at < s.len ? at : s.len };
}

/* Cut at the first sep: what is before it into *head, what is after it into *tail - either may
 * be NULL. False when sep is not there; *head is then all of s and *tail is empty, so
 * "name;params" cut at ';' gives the name either way. */
bool ioxd_slice_cut(ioxd_slice s, char sep, ioxd_slice *head, ioxd_slice *tail);

/* The next item of a separated list, trimmed, with *list moved past it: "br;q=1, gzip" cut at
 * ',' yields "br;q=1", then "gzip". Empty items are skipped. False once the list is spent. */
bool ioxd_slice_next(ioxd_slice *list, char sep, ioxd_slice *item);

/* A NUL-terminated copy in buf, for whatever wants a C string. False when it did not fit: buf
 * then holds what fit, still terminated (cap 0 writes nothing) - and false when the slice holds
 * a NUL of its own, which would end the C string early ("secret.txt%00.png" is not a PNG). */
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
 * that need it ('+', %XX) are decoded into arena and point there; the rest are views of s. A
 * malformed %XX and %00 stay as written. Returns the pair count; *truncated (may be NULL) is set
 * when a pair was left out - past cap, or not fitting the arena - so the caller can refuse the
 * request rather than act on part of it. Every pair is returned, duplicates included, in order:
 * pick a policy (first or last) and keep to it. */
size_t ioxd_kv_parse(const char *text, size_t len, ioxd_kv *out, size_t cap, char *arena, size_t arena_cap,
                     bool *truncated);

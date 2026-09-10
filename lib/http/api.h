/*
 * http/api.h - the notes of api.c, whose declarations are public
 */
#pragma once

/* ── api.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * api.c - the helpers a handler calls: slices and conversions (the substring search is find.c),
 * key/value parsing, the request looked up, shaping the reply, reasons. Nothing here touches the
 * runtime.
 */

/* at file scope:
 *   - ── slices ──────────────────────────────────────────────────────────────────────────────
 *     [bool ioxd_slice_eq(ioxd_slice s, const char *cstr)]
 *   - ── conversions ─────────────────────────────────────────────────────────────────────────
 *     [/ * Decimal digits as a number no larger than limit; false on any other byte, on ]
 *   - strtod follows the process locale (a decimal comma in some), so conversions use the "C"
 *     one, made on first use.  [static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;]
 *   - ── key/value parsing ───────────────────────────────────────────────────────────────────
 *     [/ * Percent-decode [s, s+n) into dst ('+' becomes a space; a malformed %XX, and %]
 *   - ── shaping the reply ───────────────────────────────────────────────────────────────────
 *     [static bool is_tchar(unsigned char c)]
 */

/* ioxd_slice_eq:
 * Is the slice exactly this C string?
 */

/* lower:
 * One byte, ASCII lower-cased.
 */

/* ioxd_slice_eq_ci:
 * Is the slice this C string, ignoring ASCII case?
 */

/* ioxd_slice_starts_with:
 * Does the slice begin with this C string?
 */

/* ioxd_slice_ends_with:
 * Does the slice end with this C string?
 */

/* is_space:
 * The whitespace HTTP allows around a value.
 */

/* ioxd_slice_trim:
 * The slice without leading and trailing whitespace.
 */

/* ioxd_slice_cut:
 * The slice in two at the first sep: absent, the head is the whole slice and the tail empty.
 */

/* ioxd_slice_next:
 * The next non-empty item of a separated list, trimmed; the list moves past it.
 */

/* ioxd_cstr:
 * A NUL-terminated copy of the slice in buf; false when it did not all fit, or when the slice
 * holds a NUL itself - the copy would read as a shorter string to whatever takes it.
 */

/* digits_to_u64:
 * Decimal digits as a number no larger than limit; false on any other byte, on overflow, and
 * on no digits at all.
 *   - wraps huge for a non-digit  [unsigned d = (unsigned char)p[i] - (unsigned)'0';]
 */

/* ioxd_to_u64:
 * An unsigned 64-bit integer.
 */

/* ioxd_to_i64:
 * A signed 64-bit integer: an optional '-' and digits.
 *   - via magnitude - 1: INT64_MIN has no positive twin  [*out = magnitude ?
 *     -(int64_t)(magnitude - 1) - 1 : 0;]
 */

/* ioxd_to_int:
 * An int: a signed 64-bit integer that fits one.
 */

/* ioxd_to_double:
 * A double: digits with an optional fraction and exponent; strtod does the rounding. Too large
 * fails; too small rounds towards zero, like every JSON parser.
 *   - strtod would also take spaces, inf, nan, hex  [for (size_t i = 0; i < s.len; i++) {]
 *   - no "C" locale: never the process one, which may read "2.5" as 2  [return false;]
 */

/* ioxd_to_bool:
 * A boolean: true/false, 1/0, yes/no, on/off in any case.
 */

/* decode:
 * Percent-decode [s, s+n) into dst ('+' becomes a space; a malformed %XX, and %00 - a NUL
 * would end the value early for every C string function - are kept as they are). Never longer
 * than the input; returns the decoded length.
 */

/* decode_into:
 * Decode a slice into the arena and point it there; false when it would not fit.
 */

/* ioxd_kv_parse:
 * "k=v&k2=v2" into pairs; see slice.h. One pass per pair finds '=' and '&' and notes whether
 * either side needs decoding, so the common undecoded pair is a view and costs a short scan.
 * *truncated (may be NULL) says whether a pair was left out: past cap, or not fitting the
 * arena.
 *   - skip empty pairs ("&&")  [if (end > start) {]
 *   - skip the pair, give its arena back  [used = mark;]
 */

/* lookup:
 * The value of the first pair whose key is exactly name; an absent slice otherwise.
 */

/* ioxd_req_header:
 * A request header's value by (lower-cased) name.
 */

/* ioxd_req_param:
 * A query parameter's value by key.
 */

/* is_tchar:
 * An HTTP token character (RFC 9110): what a field name is made of.
 */

/* valid_field_value:
 * A field value may hold anything but a control byte: no CR or LF (they would end the line and
 * start another: response splitting), no NUL, no other C0 byte except a tab, no DEL.
 */

/* content_type_reserved:
 * The bytes of the head arena a copied content type takes: it sits at the far end, so the
 * serialized lines can grow from the front without it in their way.
 */

/* head_room:
 * n bytes appended to the reply's head arena, or nullptr when they do not fit.
 */

/* put_bytes:
 * Bytes into the arena at *at, which moves past them: a line is assembled from its parts.
 */

/* ioxd_header:
 * Add a header to the reply: copied into the head arena as its serialized line, the name
 * lower-cased, and remembered in headers[] as slices into that line. False once the head is on
 * the wire, when the table or the arena is full, when the name is not a token or the value has
 * a control byte, and for the headers the engine writes itself - "content-type" is taken as
 * ioxd_content_type would.
 */

/* ioxd_content_type:
 * Set the content type from a C string: a copy in the head arena (a slice that outlives the
 * handler can be assigned to res.content_type directly). False once the head is sent, or for a
 * value with a control byte, or when the arena is full.
 *   - at the far end, past the lines  [char *copy = res->head + sizeof res->head - n, *at =
 *     copy;]
 */

/* ioxd_content_length:
 * Declare the body length, so a body larger than the slab streams with Content-Length. False
 * once the head is sent.
 */

/* ioxd_text:
 * Write a C string (NULL writes nothing).
 */

/* ioxd_reason:
 * The reason phrase for a status code; "Unknown" if unlisted.
 */

/*
 * json/json.h - the notes of json.c, whose declarations are public
 */
#pragma once

/* ── json.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * json/json.c - the forward-only JSON writer of ioxd.h. Every call writes in place at the
 * sink's tail - the reply slab, a raw pipe's slab, or a buffer - a comma and a value, or a
 * comma and a quoted key, as one run of stores, and moves the tail past them. The engine is
 * only asked for room when the slab is full, which is when it sends. Numbers are formatted
 * into a small stack buffer first; the writer never allocates.
 */

/* at file scope:
 *   - bytes asked of the sink at a time, when a slab has to be flushed for them
 *     [#define RUN_MAX 1024]
 *   - One bit per level in has_value and is_object, the root's included: the deepest must
 *     still fit.  [static_assert(IOXD_JSON_DEPTH < 64, "a level per bit of has_value and
 *     is_object,]
 *   - snprintf and strtod spell the decimal point the way LC_NUMERIC says, and JSON knows only
 *     '.'; a locale like fa_IR spells it with several bytes, so mending one byte afterwards is
 *     not enough. The numbers are formatted in a private "C" locale instead - made once for
 *     the process, worn by the thread for the two calls and handed straight back, so no worker
 *     disturbs another and nothing reads the shared static localeconv returns.
 */

/* tail:
 * Where the next byte goes and how many fit before the sink is full: the slab's tail for a
 * reply or a pipe (the writer the sink was opened with), the buffer's end for memory. Read
 * afresh on every call, since the handler may have written to the same slab in between.
 */

/* commit:
 * Move the tail past n bytes written there.
 */

/* sink_failed:
 * The peer is gone, or the reply failed: nothing more is written, and the writer says so.
 */

/* make_room:
 * The slow path: n bytes of room from the sink, which sends its slab first when they do not
 * fit - a reply or a pipe; a buffer has no more. nullptr marks the writer failed.
 */

/* want:
 * n bytes at the tail: what is there when it fits, else make_room.
 */

/* put:
 * Raw bytes of any length: what fits at the tail, then a flush and the rest, RUN_MAX at a time.
 */

/* needs_escape:
 * The bytes that must be escaped: the quote, the backslash, and control characters.
 */

/* clean_run:
 * How many bytes from p can go out as they are.
 */

/* put_escaped:
 * A string's bytes, escaped: safe runs copied whole, escapes one at a time.
 *   - \u00XX  [default:]
 */

/* put_string:
 * A quoted string, with the comma before it and the byte after it (a key's colon) when there
 * are any: one run of stores when it needs no escape and fits at the tail, which is nearly
 * every key and most values; otherwise piece by piece through put.
 */

/* value_lead:
 * A value may start here? -1 and the writer failed when not: inside an object a value has to
 * follow a key. Else the commas owed: none after a key or for a level's first value, one
 * otherwise - the level's has_value bit records that one is there now.
 */

/* put_value:
 * A value already formatted: the comma it owes and its bytes, in one run.
 */

/* ioxd_json_reply:
 * The reply as the sink: its content type, and the slab's writer for the fast path.
 */

/* open_level:
 * Open a container: its own level starts empty. The depth is checked before anything is
 * written, so a document that goes one level too deep leaves no comma behind.
 */

/* ioxd_json_end:
 * Close the innermost container. Which bracket is remembered by what was written: an object's
 * level is one that took keys - tracked as a bit too. Nothing open, or a key still waiting for
 * its value, is a document that cannot be finished: the writer fails, and says so.
 */

/* ioxd_json_done:
 * Nothing failed, and nothing is left open: the document is whole.
 */

/* ioxd__json_key_n:
 * A key belongs in an object, and one key per value: anything else is a broken document. The
 * comma, the quoted key and its colon go out as one run; the macros pass a literal's length,
 * folded at compile time.
 */

/* ioxd_json_cstr:
 *   - a null pointer is JSON null  [return ioxd_json_null(j);]
 */

/* digits:
 * Decimal digits of a magnitude, right-aligned in tmp; the start of them.
 */

/* ioxd_json_int:
 *   - INT64_MIN has no positive twin  [uint64_t magnitude = v < 0 ? (uint64_t)(-(v + 1)) + 1 :
 *     (uint64_t)v;]
 */

/* shortest:
 * The shortest decimal that reads back as the same value: the precisions from first to last,
 * the first that round-trips. The length written, or -1 if it did not fit or there is no "C"
 * locale.
 */

/* ioxd_json_double:
 * A double: 15, 16 or 17 significant digits.
 */

/* ioxd_json_float:
 * A float: 6 to 9, round-tripped against the float, so 0.1f is 0.1 and not the wider double it
 * would otherwise be promoted to.
 */

/* ioxd_json_raw:
 * Already JSON, copied as is. Nothing is not a value: an empty slice would leave "[,]" behind.
 */

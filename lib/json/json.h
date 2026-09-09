/*
 * json/json.h - the notes of json.c, whose declarations are public
 */
#pragma once

/* ── json.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * json/json.c - the forward-only JSON writer of ioxd.h. Every value goes straight to the sink:
 * a run of safe bytes at a time, an escape at a time, a number formatted into a small stack
 * buffer. The sink is the reply, a raw pipe, or a buffer; the writer never allocates.
 */

/* at file scope:
 *   - bytes asked of the sink at a time  [#define RUN_MAX 1024]
 *   - and the least worth asking for  [#define RUN_MIN 64]
 *   - One bit per level in has_value and is_object, the root's included: the deepest must
 *     still fit.  [static_assert(IOXD_JSON_DEPTH < 64, "a level per bit of has_value and
 *     is_object,]
 *   - ── the sinks ───────────────────────────────────────────────────────────────────────────
 *     [ioxd_json ioxd_json_reply(ioxd_ctx *ctx)]
 *   - ── the values ──────────────────────────────────────────────────────────────────────────
 *     [/ * Open a container: its own level starts empty. The depth is checked before any]
 *   - snprintf and strtod spell the decimal point the way LC_NUMERIC says, and JSON knows only
 *     '.'; a locale like fa_IR spells it with several bytes, so mending one byte afterwards is
 *     not enough. The numbers are formatted in a private "C" locale instead - made once for
 *     the process, worn by the thread for the two calls and handed straight back, so no worker
 *     disturbs another and nothing reads the shared static localeconv returns.
 */

/* reserve:
 * n bytes of the sink to write into, or nullptr; the caller then advances by what it wrote.
 */

/* put:
 * Raw bytes to the sink, in runs the slab can take; false marks the writer failed. A sink
 * refuses a reserve larger than its slab outright, so a refused run is halved and asked for
 * again, down to RUN_MIN: a slab smaller than RUN_MAX still takes the document.
 */

/* needs_escape:
 * The bytes that must be escaped: the quote, the backslash, and control characters.
 */

/* put_string:
 * A string, quoted and escaped: safe runs are copied whole, escapes one at a time.
 *   - \u00XX  [default:]
 */

/* separator:
 * Before a value or a key: the comma its level owes, unless it follows a key.
 */

/* value_ok:
 * A value may start here: inside an object it has to follow a key, or the document is broken
 * and the writer fails. Outside one - in an array, or at the top level - anything goes.
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

/* ioxd_json_key:
 * A key belongs in an object, and one key per value: anything else is a broken document.
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

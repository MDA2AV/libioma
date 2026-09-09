/*
 * json/json.c - the forward-only JSON writer of ioxd.h. Every value goes straight to the sink:
 * a run of safe bytes at a time, an escape at a time, a number formatted into a small stack
 * buffer. The sink is the reply, a raw pipe, or a buffer; the writer never allocates.
 */
#include "ioxd/json.h"
#include "ioxd/pipe.h"

#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_MAX 1024                                  /* bytes asked of the sink at a time */
#define RUN_MIN 64                                    /* and the least worth asking for    */

/* One bit per level in has_value and is_object, the root's included: the deepest must still fit. */
static_assert(IOXD_JSON_DEPTH < 64, "a level per bit of has_value and is_object, plus the root");

/* n bytes of the sink to write into, or nullptr; the caller then advances by what it wrote. */
static char *reserve(ioxd_json *j, size_t n)
{
    switch (j->kind) {
    case IOXD_JSON_TO_REPLY: return ioxd_reserve(j->to.ctx, n);
    case IOXD_JSON_TO_PIPE:  return ioxd_pipe_reserve(j->to.pipe, n);
    case IOXD_JSON_TO_MEM:   return j->to.mem.p && *j->to.mem.len + n <= j->to.mem.cap
                                        ? j->to.mem.p + *j->to.mem.len : nullptr;
    }
    return nullptr;
}

static void advance(ioxd_json *j, size_t n)
{
    switch (j->kind) {
    case IOXD_JSON_TO_REPLY: ioxd_advance(j->to.ctx, n); break;
    case IOXD_JSON_TO_PIPE:  ioxd_pipe_advance(j->to.pipe, n); break;
    case IOXD_JSON_TO_MEM:   *j->to.mem.len += n; break;
    }
}

/* Raw bytes to the sink, in runs the slab can take; false marks the writer failed. A sink refuses
 * a reserve larger than its slab outright, so a refused run is halved and asked for again, down
 * to RUN_MIN: a slab smaller than RUN_MAX still takes the document. */
static bool put(ioxd_json *j, const char *p, size_t n)
{
    while (n) {
        size_t run = n < RUN_MAX ? n : RUN_MAX;
        char  *at  = reserve(j, run);
        while (!at && run > RUN_MIN) {
            run = run / 2 > RUN_MIN ? run / 2 : RUN_MIN;
            at  = reserve(j, run);
        }
        if (!at) {
            j->failed = true;
            return false;
        }
        memcpy(at, p, run);
        advance(j, run);
        p += run;
        n -= run;
    }
    return true;
}

static bool put_cstr(ioxd_json *j, const char *s)
{
    return put(j, s, strlen(s));
}

/* The bytes that must be escaped: the quote, the backslash, and control characters. */
static bool needs_escape(unsigned char c)
{
    return c < 0x20 || c == '"' || c == '\\';
}

/* A string, quoted and escaped: safe runs are copied whole, escapes one at a time. */
static bool put_string(ioxd_json *j, const char *p, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    if (!put(j, "\"", 1))
        return false;
    while (n) {
        size_t run = 0;
        while (run < n && !needs_escape((unsigned char)p[run]))
            run++;
        if (run && !put(j, p, run))
            return false;
        p += run;
        n -= run;
        if (n == 0)
            break;
        unsigned char c = (unsigned char)*p++;
        n--;
        char esc[6] = { '\\', 0, 0, 0, 0, 0 };
        size_t len = 2;
        switch (c) {
        case '"':  esc[1] = '"';  break;
        case '\\': esc[1] = '\\'; break;
        case '\n': esc[1] = 'n';  break;
        case '\r': esc[1] = 'r';  break;
        case '\t': esc[1] = 't';  break;
        case '\b': esc[1] = 'b';  break;
        case '\f': esc[1] = 'f';  break;
        default:                                      /* \u00XX */
            esc[1] = 'u';
            esc[2] = '0';
            esc[3] = '0';
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 15];
            len    = 6;
            break;
        }
        if (!put(j, esc, len))
            return false;
    }
    return put(j, "\"", 1);
}

/* Before a value or a key: the comma its level owes, unless it follows a key. */
static bool separator(ioxd_json *j)
{
    if (j->failed)
        return false;
    if (j->after_key) {
        j->after_key = false;
        return true;
    }
    uint64_t bit = (uint64_t)1 << j->depth;
    if (j->has_value & bit)
        return put(j, ",", 1);
    j->has_value |= bit;
    return true;
}

/* A value may start here: inside an object it has to follow a key, or the document is broken and
 * the writer fails. Outside one - in an array, or at the top level - anything goes. */
static bool value_ok(ioxd_json *j)
{
    if (j->failed)
        return false;
    if ((j->is_object & ((uint64_t)1 << j->depth)) && !j->after_key) {
        j->failed = true;
        return false;
    }
    return separator(j);
}

/* ── the sinks ─────────────────────────────────────────────────────────────────────────── */

ioxd_json ioxd_json_reply(ioxd_ctx *ctx)
{
    ioxd_content_type(ctx, "application/json");
    ioxd_json j = { .kind = IOXD_JSON_TO_REPLY };
    j.to.ctx = ctx;
    return j;
}

ioxd_json ioxd_json_pipe(struct ioxd_pipe *pipe)
{
    ioxd_json j = { .kind = IOXD_JSON_TO_PIPE };
    j.to.pipe = pipe;
    return j;
}

ioxd_json ioxd_json_mem(char *buf, size_t cap, size_t *len)
{
    ioxd_json j = { .kind = IOXD_JSON_TO_MEM };
    j.to.mem.p   = buf;
    j.to.mem.cap = cap;
    j.to.mem.len = len;
    *len = 0;
    return j;
}

/* ── the values ────────────────────────────────────────────────────────────────────────── */

/* Open a container: its own level starts empty. The depth is checked before anything is written,
 * so a document that goes one level too deep leaves no comma behind. */
static bool open_level(ioxd_json *j, char bracket)
{
    if (j->failed)
        return false;
    if (j->depth == IOXD_JSON_DEPTH) {
        j->failed = true;
        return false;
    }
    if (!value_ok(j))
        return false;
    j->depth++;
    uint64_t bit = (uint64_t)1 << j->depth;
    j->has_value &= ~bit;
    if (bracket == '{')
        j->is_object |= bit;
    else
        j->is_object &= ~bit;
    return put(j, &bracket, 1);
}

bool ioxd_json_object(ioxd_json *j)
{
    return open_level(j, '{');
}

bool ioxd_json_array(ioxd_json *j)
{
    return open_level(j, '[');
}

/* Close the innermost container. Which bracket is remembered by what was written: an object's
 * level is one that took keys - tracked as a bit too. Nothing open, or a key still waiting for
 * its value, is a document that cannot be finished: the writer fails, and says so. */
bool ioxd_json_end(ioxd_json *j)
{
    if (j->failed)
        return false;
    if (j->depth == 0 || j->after_key) {
        j->failed = true;
        return false;
    }
    bool object = j->is_object & ((uint64_t)1 << j->depth);
    j->depth--;
    return put(j, object ? "}" : "]", 1);
}

/* Nothing failed, and nothing is left open: the document is whole. */
bool ioxd_json_done(ioxd_json *j)
{
    return !j->failed && j->depth == 0;
}

/* A key belongs in an object, and one key per value: anything else is a broken document. */
bool ioxd_json_key(ioxd_json *j, const char *name)
{
    if (j->failed)
        return false;
    if (!(j->is_object & ((uint64_t)1 << j->depth)) || j->after_key) {
        j->failed = true;
        return false;
    }
    if (!separator(j))
        return false;
    if (!put_string(j, name, strlen(name)) || !put(j, ":", 1))
        return false;
    j->after_key = true;
    return true;
}

bool ioxd_json_string(ioxd_json *j, ioxd_slice s)
{
    return value_ok(j) && put_string(j, s.p, s.len);
}

bool ioxd_json_cstr(ioxd_json *j, const char *s)
{
    if (!s)
        return ioxd_json_null(j);                     /* a null pointer is JSON null */
    return value_ok(j) && put_string(j, s, strlen(s));
}

/* Decimal digits of a magnitude, right-aligned in tmp; the start of them. */
static char *digits(char *tmp_end, uint64_t v)
{
    char *p = tmp_end;
    do {
        *--p = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    return p;
}

bool ioxd_json_uint(ioxd_json *j, uint64_t v)
{
    char  tmp[24];
    char *p = digits(tmp + sizeof tmp, v);
    return value_ok(j) && put(j, p, (size_t)(tmp + sizeof tmp - p));
}

bool ioxd_json_int(ioxd_json *j, int64_t v)
{
    char     tmp[24];
    uint64_t magnitude = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;   /* INT64_MIN has no positive twin */
    char    *p = digits(tmp + sizeof tmp, magnitude);
    if (v < 0)
        *--p = '-';
    return value_ok(j) && put(j, p, (size_t)(tmp + sizeof tmp - p));
}

/* snprintf and strtod spell the decimal point the way LC_NUMERIC says, and JSON knows only '.';
 * a locale like fa_IR spells it with several bytes, so mending one byte afterwards is not enough.
 * The numbers are formatted in a private "C" locale instead - made once for the process, worn by
 * the thread for the two calls and handed straight back, so no worker disturbs another and
 * nothing reads the shared static localeconv returns. */
static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;
static locale_t       c_locale;

static void make_c_locale(void)
{
    c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

/* The shortest decimal that reads back as the same value: the precisions from first to last, the
 * first that round-trips. The length written, or -1 if it did not fit or there is no "C" locale. */
static int shortest(char *tmp, size_t cap, double v, int first, int last, bool as_float)
{
    pthread_once(&c_locale_once, make_c_locale);
    if (!c_locale)
        return -1;
    locale_t previous = uselocale(c_locale);
    if (!previous)
        return -1;
    int n = -1;
    for (int precision = first; precision <= last; precision++) {
        n = snprintf(tmp, cap, "%.*g", precision, v);
        if (n < 0 || (size_t)n >= cap)
            break;
        if (as_float ? strtof(tmp, nullptr) == (float)v : strtod(tmp, nullptr) == v)
            break;
    }
    uselocale(previous);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* A double: 15, 16 or 17 significant digits. */
bool ioxd_json_double(ioxd_json *j, double v)
{
    if (!isfinite(v))
        return ioxd_json_null(j);
    char tmp[32];
    int  n = shortest(tmp, sizeof tmp, v, 15, 17, false);
    if (n < 0) {
        j->failed = true;
        return false;
    }
    return value_ok(j) && put(j, tmp, (size_t)n);
}

/* A float: 6 to 9, round-tripped against the float, so 0.1f is 0.1 and not the wider double it
 * would otherwise be promoted to. */
bool ioxd_json_float(ioxd_json *j, float v)
{
    if (!isfinite(v))
        return ioxd_json_null(j);
    char tmp[32];
    int  n = shortest(tmp, sizeof tmp, (double)v, 6, 9, true);
    if (n < 0) {
        j->failed = true;
        return false;
    }
    return value_ok(j) && put(j, tmp, (size_t)n);
}

bool ioxd_json_bool(ioxd_json *j, bool v)
{
    return value_ok(j) && put_cstr(j, v ? "true" : "false");
}

bool ioxd_json_null(ioxd_json *j)
{
    return value_ok(j) && put_cstr(j, "null");
}

/* Already JSON, copied as is. Nothing is not a value: an empty slice would leave "[,]" behind. */
bool ioxd_json_raw(ioxd_json *j, ioxd_slice json)
{
    if (json.len == 0) {
        j->failed = true;
        return false;
    }
    return value_ok(j) && put(j, json.p, json.len);
}

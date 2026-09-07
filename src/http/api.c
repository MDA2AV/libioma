/*
 * api.c - the helpers a handler calls: slices and conversions, key/value parsing, shaping the
 * reply, reasons.
 * Nothing here touches the runtime.
 */
#include "http/internal.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

/* Is the slice exactly this C string? */
bool ioma_slice_eq(ioma_slice s, const char *cstr)
{
    size_t n = strlen(cstr);
    return n == s.len && (n == 0 || memcmp(s.p, cstr, n) == 0);
}

/* One byte, ASCII lower-cased. */
static unsigned char lower(unsigned char c)
{
    return (unsigned char)(c - 'A') < 26U ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Is the slice this C string, ignoring ASCII case? */
bool ioma_slice_eq_ci(ioma_slice s, const char *cstr)
{
    size_t n = strlen(cstr);
    if (n != s.len)
        return false;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)s.p[i]) != lower((unsigned char)cstr[i]))
            return false;
    return true;
}

/* Does the slice begin with this C string? */
bool ioma_slice_starts_with(ioma_slice s, const char *prefix)
{
    size_t n = strlen(prefix);
    return n <= s.len && (n == 0 || memcmp(s.p, prefix, n) == 0);
}

/* Does the slice end with this C string? */
bool ioma_slice_ends_with(ioma_slice s, const char *suffix)
{
    size_t n = strlen(suffix);
    return n <= s.len && (n == 0 || memcmp(s.p + s.len - n, suffix, n) == 0);
}

/* The whitespace HTTP allows around a value. */
static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The slice without leading and trailing whitespace. */
ioma_slice ioma_slice_trim(ioma_slice s)
{
    while (s.len && is_space(s.p[0])) {
        s.p++;
        s.len--;
    }
    while (s.len && is_space(s.p[s.len - 1]))
        s.len--;
    return s;
}

/* A NUL-terminated copy of the slice in buf; false when it did not all fit. */
bool ioma_cstr(ioma_slice s, char *buf, size_t cap)
{
    if (cap == 0)
        return false;
    size_t n = s.len < cap ? s.len : cap - 1;
    if (n)
        memcpy(buf, s.p, n);
    buf[n] = '\0';
    return n == s.len;
}

/* ── conversions ───────────────────────────────────────────────────────────────────────── */

/* Decimal digits as a number no larger than limit; false on any other byte, on overflow, and
 * on no digits at all. */
static bool digits_to_u64(const char *p, size_t n, uint64_t limit, uint64_t *out)
{
    if (n == 0)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = (unsigned char)p[i] - (unsigned)'0';    /* wraps huge for a non-digit */
        if (d > 9 || v > (limit - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* An unsigned 64-bit integer. */
bool ioma_to_u64(ioma_slice s, uint64_t *out)
{
    return digits_to_u64(s.p, s.len, UINT64_MAX, out);
}

/* A signed 64-bit integer: an optional '-' and digits. */
bool ioma_to_i64(ioma_slice s, int64_t *out)
{
    bool     negative = s.len > 0 && s.p[0] == '-';
    uint64_t limit    = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t magnitude;
    if (!digits_to_u64(s.p + negative, s.len - negative, limit, &magnitude))
        return false;
    if (negative)
        *out = magnitude ? -(int64_t)(magnitude - 1) - 1 : 0;    /* via magnitude - 1: INT64_MIN has no positive twin */
    else
        *out = (int64_t)magnitude;
    return true;
}

/* An int: a signed 64-bit integer that fits one. */
bool ioma_to_int(ioma_slice s, int *out)
{
    int64_t v;
    if (!ioma_to_i64(s, &v) || v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

/* strtod follows the process locale (a decimal comma in some), so conversions use the "C" one,
 * made on first use. */
static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;
static locale_t       c_locale;
static void make_c_locale(void)
{
    c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
}

/* A double: digits with an optional fraction and exponent; strtod does the rounding. Too large
 * fails; too small rounds towards zero, like every JSON parser. */
bool ioma_to_double(ioma_slice s, double *out)
{
    char text[128];
    if (s.len == 0 || s.len >= sizeof text)
        return false;
    for (size_t i = 0; i < s.len; i++) {                 /* strtod would also take spaces, inf, nan, hex */
        char ch      = s.p[i];
        bool after_e = i > 0 && (s.p[i - 1] == 'e' || s.p[i - 1] == 'E');
        bool sign    = (ch == '-' && i == 0) || ((ch == '-' || ch == '+') && after_e);
        if (!(ch >= '0' && ch <= '9') && ch != '.' && ch != 'e' && ch != 'E' && !sign)
            return false;
    }
    memcpy(text, s.p, s.len);
    text[s.len] = '\0';
    pthread_once(&c_locale_once, make_c_locale);
    char  *end;
    errno = 0;
    double v = c_locale ? strtod_l(text, &end, c_locale) : strtod(text, &end);
    if (end != text + s.len || (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)))
        return false;
    *out = v;
    return true;
}

/* A boolean: true/false, 1/0, yes/no, on/off in any case. */
bool ioma_to_bool(ioma_slice s, bool *out)
{
    static const char *const yes[] = { "true", "1", "yes", "on" };
    static const char *const no[]  = { "false", "0", "no", "off" };
    for (size_t i = 0; i < 4; i++) {
        if (ioma_slice_eq_ci(s, yes[i])) {
            *out = true;
            return true;
        }
        if (ioma_slice_eq_ci(s, no[i])) {
            *out = false;
            return true;
        }
    }
    return false;
}

/* ── key/value parsing ─────────────────────────────────────────────────────────────────── */

/* Value of a hex digit, or -1. */
static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20U;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Percent-decode [s, s+n) into dst ('+' becomes a space, a malformed %XX is kept as is).
 * Never longer than the input; returns the decoded length. */
static size_t decode(const char *src, size_t len, char *dst)
{
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = hexval((unsigned char)src[i + 1]), lo = hexval((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)(hi * 16 + lo);
                i += 2;
            } else {
                dst[out++] = '%';
            }
        } else {
            dst[out++] = src[i];
        }
    }
    return out;
}

/* Decode a slice into the arena and point it there; false when it would not fit. */
static bool decode_into(ioma_slice *slice, char *arena, size_t arena_cap, size_t *used)
{
    if (*used + slice->len > arena_cap)
        return false;
    size_t decoded = decode(slice->p, slice->len, arena + *used);
    slice->p    = arena + *used;
    slice->len  = decoded;
    *used      += decoded;
    return true;
}

/* "k=v&k2=v2" into pairs; see http.h. One pass per pair finds '=' and '&' and notes whether
 * either side needs decoding, so the common undecoded pair is a view and costs a short scan. */
size_t ioma_kv_parse(const char *text, size_t len, ioma_kv *out, size_t cap, char *arena, size_t arena_cap)
{
    size_t used = 0, count = 0, start = 0;
    while (start < len && count < cap) {
        size_t end = start, eq_at = len;
        bool   key_needs_decode = false, value_needs_decode = false;
        for (; end < len && text[end] != '&'; end++) {
            char ch = text[end];
            if (ch == '=') {
                if (eq_at == len) eq_at = end;
            } else if (ch == '%' || ch == '+') {
                if (eq_at == len) key_needs_decode = true; else value_needs_decode = true;
            }
        }
        if (end > start) {                                 /* skip empty pairs ("&&") */
            bool has_eq = eq_at < end;
            ioma_slice key   = { text + start, (has_eq ? eq_at : end) - start };
            ioma_slice value = { has_eq ? text + eq_at + 1 : text + end, has_eq ? end - eq_at - 1 : 0 };
            size_t mark = used;
            bool   ok   = (!key_needs_decode   || decode_into(&key,   arena, arena_cap, &used)) &&
                          (!value_needs_decode || decode_into(&value, arena, arena_cap, &used));
            if (ok)
                out[count++] = (ioma_kv){ key, value };
            else
                used = mark;                               /* skip the pair, give its arena back */
        }
        start = end + 1;
    }
    return count;
}

/* ── shaping the reply ─────────────────────────────────────────────────────────────────── */

/* Add a header to the reply. false once the head is on the wire, or when the table is full. */
bool ioma_header(ioma_ctx *c, const char *name, const char *value)
{
    ioma_response *res = &c->res;
    if (res->head_sent || res->n_headers == IOMA_MAX_RESP_HEADERS)
        return false;
    res->headers[res->n_headers++] = (ioma_kv){ { name, strlen(name) }, { value, strlen(value) } };
    return true;
}

/* Set the content type from a C string (a slice can be assigned to res.content_type directly). */
void ioma_content_type(ioma_ctx *c, const char *type)
{
    c->res.content_type = (ioma_slice){ type, strlen(type) };
}

/* Declare the body length, so a body larger than the slab streams with Content-Length. */
void ioma_content_length(ioma_ctx *c, size_t n)
{
    c->res.content_length = n;
    c->res.has_length     = true;
}

/* Write a C string. */
int ioma_text(ioma_ctx *c, const char *s)
{
    return ioma_write(c, s, strlen(s));
}

/* The reason phrase for a status code; "Unknown" if unlisted. */
const char *ioma_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

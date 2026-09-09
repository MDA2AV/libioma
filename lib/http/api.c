/*
 * api.c - the helpers a handler calls: slices and conversions, key/value parsing, shaping the
 * reply, reasons.
 * Nothing here touches the runtime.
 */
#include "http/internal.h"
#include "ioxd/http.h"
#include "ioxd/slice.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

/* Is the slice exactly this C string? */
bool ioxd_slice_eq(ioxd_slice s, const char *cstr)
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
bool ioxd_slice_eq_ci(ioxd_slice s, const char *cstr)
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
bool ioxd_slice_starts_with(ioxd_slice s, const char *prefix)
{
    size_t n = strlen(prefix);
    return n <= s.len && (n == 0 || memcmp(s.p, prefix, n) == 0);
}

/* Does the slice end with this C string? */
bool ioxd_slice_ends_with(ioxd_slice s, const char *suffix)
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
ioxd_slice ioxd_slice_trim(ioxd_slice s)
{
    while (s.len && is_space(s.p[0])) {
        s.p++;
        s.len--;
    }
    while (s.len && is_space(s.p[s.len - 1]))
        s.len--;
    return s;
}

/* A NUL-terminated copy of the slice in buf; false when it did not all fit, or when the slice
 * holds a NUL itself - the copy would read as a shorter string to whatever takes it. */
bool ioxd_cstr(ioxd_slice s, char *buf, size_t cap)
{
    if (cap == 0)
        return false;
    size_t n = s.len < cap ? s.len : cap - 1;
    if (n)
        memcpy(buf, s.p, n);
    buf[n] = '\0';
    return n == s.len && memchr(buf, '\0', n) == nullptr;
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
bool ioxd_to_u64(ioxd_slice s, uint64_t *out)
{
    return digits_to_u64(s.p, s.len, UINT64_MAX, out);
}

/* A signed 64-bit integer: an optional '-' and digits. */
bool ioxd_to_i64(ioxd_slice s, int64_t *out)
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
bool ioxd_to_int(ioxd_slice s, int *out)
{
    int64_t v;
    if (!ioxd_to_i64(s, &v) || v < INT_MIN || v > INT_MAX)
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
bool ioxd_to_double(ioxd_slice s, double *out)
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
    if (!c_locale)
        return false;                                    /* no "C" locale: never the process one, which may read "2.5" as 2 */
    char  *end;
    errno = 0;
    double v = strtod_l(text, &end, c_locale);
    if (end != text + s.len || (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)))
        return false;
    *out = v;
    return true;
}

/* A boolean: true/false, 1/0, yes/no, on/off in any case. */
bool ioxd_to_bool(ioxd_slice s, bool *out)
{
    static const char *const yes[] = { "true", "1", "yes", "on" };
    static const char *const no[]  = { "false", "0", "no", "off" };
    for (size_t i = 0; i < 4; i++) {
        if (ioxd_slice_eq_ci(s, yes[i])) {
            *out = true;
            return true;
        }
        if (ioxd_slice_eq_ci(s, no[i])) {
            *out = false;
            return true;
        }
    }
    return false;
}

/* ── key/value parsing ─────────────────────────────────────────────────────────────────── */

/* Percent-decode [s, s+n) into dst ('+' becomes a space; a malformed %XX, and %00 - a NUL
 * would end the value early for every C string function - are kept as they are).
 * Never longer than the input; returns the decoded length. */
static size_t decode(const char *src, size_t len, char *dst)
{
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = ioxd__hexval((unsigned char)src[i + 1]), lo = ioxd__hexval((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0 && !(hi == 0 && lo == 0)) {
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
static bool decode_into(ioxd_slice *slice, char *arena, size_t arena_cap, size_t *used)
{
    if (*used + slice->len > arena_cap)
        return false;
    size_t decoded = decode(slice->p, slice->len, arena + *used);
    slice->p    = arena + *used;
    slice->len  = decoded;
    *used      += decoded;
    return true;
}

/* "k=v&k2=v2" into pairs; see slice.h. One pass per pair finds '=' and '&' and notes whether
 * either side needs decoding, so the common undecoded pair is a view and costs a short scan.
 * *truncated (may be NULL) says whether a pair was left out: past cap, or not fitting the arena. */
size_t ioxd_kv_parse(const char *text, size_t len, ioxd_kv *out, size_t cap, char *arena, size_t arena_cap,
                     bool *truncated)
{
    size_t used = 0, count = 0, start = 0;
    bool   lost = false;
    while (start < len) {
        if (count == cap) {
            lost = true;
            break;
        }
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
            ioxd_slice key   = { text + start, (has_eq ? eq_at : end) - start };
            ioxd_slice value = { has_eq ? text + eq_at + 1 : text + end, has_eq ? end - eq_at - 1 : 0 };
            size_t mark = used;
            bool   ok   = (!key_needs_decode   || decode_into(&key,   arena, arena_cap, &used)) &&
                          (!value_needs_decode || decode_into(&value, arena, arena_cap, &used));
            if (ok) {
                out[count++] = (ioxd_kv){ key, value };
            } else {
                used = mark;                               /* skip the pair, give its arena back */
                lost = true;
            }
        }
        start = end + 1;
    }
    if (truncated)
        *truncated = lost;
    return count;
}

/* ── shaping the reply ─────────────────────────────────────────────────────────────────── */

/* An HTTP token character (RFC 9110): what a field name is made of. */
static bool is_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != nullptr);
}

/* A field value may hold anything but a control byte: no CR or LF (they would end the line
 * and start another: response splitting), no NUL, no other C0 byte except a tab, no DEL. */
static bool valid_field_value(const char *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }
    return true;
}

/* The bytes of the head arena a copied content type takes: it sits at the far end, so the
 * serialized lines can grow from the front without it in their way. */
static size_t content_type_reserved(const ioxd_response *res)
{
    uintptr_t p = (uintptr_t)res->content_type.p, lo = (uintptr_t)res->head, hi = lo + sizeof res->head;
    return p >= lo && p < hi ? res->content_type.len : 0;
}

/* n bytes appended to the reply's head arena, or nullptr when they do not fit. */
static char *head_room(ioxd_response *res, size_t n)
{
    if (n > sizeof res->head - content_type_reserved(res) - res->head_len)
        return nullptr;
    char *at = res->head + res->head_len;
    res->head_len += n;
    return at;
}

/* Bytes into the arena at *at, which moves past them: a line is assembled from its parts. */
static void put_bytes(char **at, const void *src, size_t n)
{
    memcpy(*at, src, n);
    *at += n;
}

/* Add a header to the reply: copied into the head arena as its serialized line, the name
 * lower-cased, and remembered in headers[] as slices into that line. False once the head is on
 * the wire, when the table or the arena is full, when the name is not a token or the value has
 * a control byte, and for the headers the engine writes itself - "content-type" is taken as
 * ioxd_content_type would. */
bool ioxd_header(ioxd_ctx *ctx, const char *name, const char *value)
{
    ioxd_response *res = &ctx->res;
    if (!name || !value)
        return false;
    size_t name_len = strlen(name), value_len = strlen(value);
    if (res->head_sent || res->n_headers == IOXD_MAX_RESP_HEADERS || name_len == 0)
        return false;
    for (size_t i = 0; i < name_len; i++)
        if (!is_tchar((unsigned char)name[i]))
            return false;
    if (!valid_field_value(value, value_len))
        return false;
    if (name_len == 12 && strncasecmp(name, "content-type", 12) == 0)
        return ioxd_content_type(ctx, value);
    if ((name_len == 14 && strncasecmp(name, "content-length", 14) == 0)
     || (name_len == 17 && strncasecmp(name, "transfer-encoding", 17) == 0)
     || (name_len == 10 && strncasecmp(name, "connection", 10) == 0))
        return false;
    char *line = head_room(res, name_len + 2 + value_len + 2);
    if (!line)
        return false;
    char *at = line;
    for (size_t i = 0; i < name_len; i++)
        *at++ = (char)lower((unsigned char)name[i]);
    put_bytes(&at, ": ", 2);
    put_bytes(&at, value, value_len);
    put_bytes(&at, "\r\n", 2);
    res->headers[res->n_headers++] = (ioxd_kv){ { line, name_len }, { line + name_len + 2, value_len } };
    return true;
}

/* Set the content type from a C string: a copy in the head arena (a slice that outlives the
 * handler can be assigned to res.content_type directly). False once the head is sent, or for a
 * value with a control byte, or when the arena is full. */
bool ioxd_content_type(ioxd_ctx *ctx, const char *type)
{
    ioxd_response *res = &ctx->res;
    if (!type || res->head_sent)
        return false;
    size_t n = strlen(type);
    if (!valid_field_value(type, n))
        return false;
    if (n > sizeof res->head - res->head_len)
        return false;
    char *copy = res->head + sizeof res->head - n, *at = copy;   /* at the far end, past the lines */
    put_bytes(&at, type, n);
    res->content_type = (ioxd_slice){ copy, n };
    return true;
}

/* Declare the body length, so a body larger than the slab streams with Content-Length. False
 * once the head is sent. */
bool ioxd_content_length(ioxd_ctx *ctx, size_t n)
{
    if (ctx->res.head_sent)
        return false;
    ctx->res.content_length = n;
    ctx->res.has_length     = true;
    return true;
}

/* Write a C string (NULL writes nothing). */
int ioxd_text(ioxd_ctx *ctx, const char *s)
{
    return s ? ioxd_write(ctx, s, strlen(s)) : 0;
}

/* The reason phrase for a status code; "Unknown" if unlisted. */
const char *ioxd_reason(int status)
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

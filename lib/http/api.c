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

bool ioxd_slice_eq(ioxd_slice s, const char *cstr)
{
    size_t n = strlen(cstr);
    return n == s.len && (n == 0 || memcmp(s.p, cstr, n) == 0);
}

static unsigned char lower(unsigned char c)
{
    return (unsigned char)(c - 'A') < 26U ? (unsigned char)(c + ('a' - 'A')) : c;
}

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

bool ioxd_slice_starts_with(ioxd_slice s, const char *prefix)
{
    size_t n = strlen(prefix);
    return n <= s.len && (n == 0 || memcmp(s.p, prefix, n) == 0);
}

bool ioxd_slice_ends_with(ioxd_slice s, const char *suffix)
{
    size_t n = strlen(suffix);
    return n <= s.len && (n == 0 || memcmp(s.p + s.len - n, suffix, n) == 0);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

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

bool ioxd_slice_cut(ioxd_slice s, char sep, ioxd_slice *head, ioxd_slice *tail)
{
    ptrdiff_t at = ioxd_slice_find_char(s, sep);
    if (head)
        *head = at < 0 ? s : ioxd_slice_upto(s, (size_t)at);
    if (tail)
        *tail = at < 0 ? ioxd_slice_from(s, s.len) : ioxd_slice_from(s, (size_t)at + 1);
    return at >= 0;
}

bool ioxd_slice_next(ioxd_slice *list, char sep, ioxd_slice *item)
{
    while (list->len) {
        ioxd_slice head;
        ioxd_slice_cut(*list, sep, &head, list);
        head = ioxd_slice_trim(head);
        if (head.len) {
            *item = head;
            return true;
        }
    }
    return false;
}

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

static bool digits_to_u64(const char *p, size_t n, uint64_t limit, uint64_t *out)
{
    if (n == 0)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = (unsigned char)p[i] - (unsigned)'0';
        if (d > 9 || v > (limit - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool ioxd_to_u64(ioxd_slice s, uint64_t *out)
{
    return digits_to_u64(s.p, s.len, UINT64_MAX, out);
}

bool ioxd_to_i64(ioxd_slice s, int64_t *out)
{
    bool     negative = s.len > 0 && s.p[0] == '-';
    uint64_t limit    = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t magnitude;
    if (!digits_to_u64(s.p + negative, s.len - negative, limit, &magnitude))
        return false;
    if (negative)
        *out = magnitude ? -(int64_t)(magnitude - 1) - 1 : 0;
    else
        *out = (int64_t)magnitude;
    return true;
}

bool ioxd_to_int(ioxd_slice s, int *out)
{
    int64_t v;
    if (!ioxd_to_i64(s, &v) || v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;
static locale_t       c_locale;
static void make_c_locale(void)
{
    c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
}

bool ioxd_to_double(ioxd_slice s, double *out)
{
    char text[128];
    if (s.len == 0 || s.len >= sizeof text)
        return false;
    for (size_t i = 0; i < s.len; i++) {
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
        return false;
    char  *end;
    errno = 0;
    double v = strtod_l(text, &end, c_locale);
    if (end != text + s.len || (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)))
        return false;
    *out = v;
    return true;
}

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

static size_t decode(const char *src, size_t len, char *dst)
{
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = ioxd__http_hexval((unsigned char)src[i + 1]), lo = ioxd__http_hexval((unsigned char)src[i + 2]);
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
        if (end > start) {
            bool has_eq = eq_at < end;
            ioxd_slice key   = { text + start, (has_eq ? eq_at : end) - start };
            ioxd_slice value = { has_eq ? text + eq_at + 1 : text + end, has_eq ? end - eq_at - 1 : 0 };
            size_t mark = used;
            bool   ok   = (!key_needs_decode   || decode_into(&key,   arena, arena_cap, &used)) &&
                          (!value_needs_decode || decode_into(&value, arena, arena_cap, &used));
            if (ok) {
                out[count++] = (ioxd_kv){ key, value };
            } else {
                used = mark;
                lost = true;
            }
        }
        start = end + 1;
    }
    if (truncated)
        *truncated = lost;
    return count;
}

static ioxd_slice lookup(const ioxd_kv *pairs, size_t n, const char *name)
{
    size_t len = strlen(name);
    for (size_t i = 0; i < n; i++)
        if (pairs[i].key.len == len && memcmp(pairs[i].key.p, name, len) == 0)
            return pairs[i].value;
    return (ioxd_slice){ nullptr, 0 };
}

ioxd_slice ioxd_req_header(const ioxd_ctx *ctx, const char *name)
{
    return lookup(ctx->req.headers, ctx->req.n_headers, name);
}

ioxd_slice ioxd_req_param(const ioxd_ctx *ctx, const char *key)
{
    return lookup(ctx->req.params, ctx->req.n_params, key);
}

static double qvalue(ioxd_slice params)
{
    ioxd_slice param, key, value;
    while (ioxd_slice_next(&params, ';', &param))
        if (ioxd_slice_cut(param, '=', &key, &value) && ioxd_slice_eq_ci(ioxd_slice_trim(key), "q")) {
            double q;
            return ioxd_to_double(ioxd_slice_trim(value), &q) ? q : 0;
        }
    return 1;
}

double ioxd_accepts_encoding(const ioxd_ctx *ctx, const char *coding)
{
    ioxd_slice accept = ioxd_req_header(ctx, "accept-encoding"), item, name, params;
    double     any = 0;
    while (ioxd_slice_next(&accept, ',', &item)) {
        ioxd_slice_cut(item, ';', &name, &params);
        name = ioxd_slice_trim(name);
        if (ioxd_slice_eq_ci(name, coding))
            return qvalue(params);
        if (ioxd_slice_eq(name, "*"))
            any = qvalue(params);
    }
    return any;
}

static bool is_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != nullptr);
}

static bool valid_field_value(const char *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }
    return true;
}

static size_t content_type_reserved(const ioxd_response *res)
{
    uintptr_t p = (uintptr_t)res->content_type.p, lo = (uintptr_t)res->head, hi = lo + sizeof res->head;
    return p >= lo && p < hi ? res->content_type.len : 0;
}

static char *head_room(ioxd_response *res, size_t n)
{
    if (n > sizeof res->head - content_type_reserved(res) - res->head_len)
        return nullptr;
    char *at = res->head + res->head_len;
    res->head_len += n;
    return at;
}

static void put_bytes(char **at, const void *src, size_t n)
{
    memcpy(*at, src, n);
    *at += n;
}

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
    char *copy = res->head + sizeof res->head - n, *at = copy;
    put_bytes(&at, type, n);
    res->content_type = (ioxd_slice){ copy, n };
    return true;
}

bool ioxd_content_length(ioxd_ctx *ctx, size_t n)
{
    if (ctx->res.head_sent)
        return false;
    ctx->res.content_length = n;
    ctx->res.has_length     = true;
    return true;
}

int ioxd_text(ioxd_ctx *ctx, const char *s)
{
    return s ? ioxd_write(ctx, s, strlen(s)) : 0;
}

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

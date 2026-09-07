/*
 * api.c - the helpers a handler calls: slices, key/value parsing, shaping the reply, reasons.
 * Nothing here touches the runtime.
 */
#define _GNU_SOURCE
#include "http/internal.h"

#include <string.h>

/* ── slices ────────────────────────────────────────────────────────────────────────────── */

/* Is the slice exactly this C string? */
bool ioma_slice_eq(ioma_slice s, const char *cstr)
{
    size_t n = strlen(cstr);
    return n == s.len && memcmp(s.p, cstr, n) == 0;
}

/* The integer a slice starts with (optional sign), 0 if it starts with none. */
long ioma_slice_int(ioma_slice s)
{
    size_t i = 0;
    bool neg = false;
    if (i < s.len && (s.p[i] == '-' || s.p[i] == '+')) {
        neg = s.p[i] == '-';
        i++;
    }
    long v = 0;
    for (; i < s.len && s.p[i] >= '0' && s.p[i] <= '9'; i++)
        v = v * 10 + (s.p[i] - '0');
    return neg ? -v : v;
}

/* ── key/value parsing ─────────────────────────────────────────────────────────────────── */

/* Value of a hex digit, or -1. */
static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
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
    ioma_response *r = &c->res;
    if (r->head_sent || r->n_headers == IOMA_MAX_RESP_HEADERS)
        return false;
    r->headers[r->n_headers++] = (ioma_kv){ { name, strlen(name) }, { value, strlen(value) } };
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

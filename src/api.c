/*
 * api.c - the helpers a handler calls: slices, key/value parsing, shaping the reply, reasons.
 * Nothing here touches the runtime.
 */
#define _GNU_SOURCE
#include "internal.h"

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
static size_t decode(const char *s, size_t n, char *dst)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') {
            dst[o++] = ' ';
        } else if (s[i] == '%' && i + 2 < n) {
            int hi = hexval((unsigned char)s[i + 1]), lo = hexval((unsigned char)s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[o++] = (char)(hi * 16 + lo);
                i += 2;
            } else {
                dst[o++] = '%';
            }
        } else {
            dst[o++] = s[i];
        }
    }
    return o;
}

/* Decode a slice into the arena and point it there; false when it would not fit. */
static bool decode_into(ioma_slice *v, char *arena, size_t arena_cap, size_t *used)
{
    if (*used + v->len > arena_cap)
        return false;
    size_t n = decode(v->p, v->len, arena + *used);
    v->p    = arena + *used;
    v->len  = n;
    *used  += n;
    return true;
}

/* "k=v&k2=v2" into pairs; see http.h. One pass per pair finds '=' and '&' and notes whether
 * either side needs decoding, so the common undecoded pair is a view and costs a short scan. */
size_t ioma_kv_parse(const char *s, size_t n, ioma_kv *out, size_t cap, char *arena, size_t arena_cap)
{
    size_t used = 0, count = 0, i = 0;
    while (i < n && count < cap) {
        size_t j = i, eq = n;
        bool   kdec = false, vdec = false;
        for (; j < n && s[j] != '&'; j++) {
            char c = s[j];
            if (c == '=') {
                if (eq == n) eq = j;
            } else if (c == '%' || c == '+') {
                if (eq == n) kdec = true; else vdec = true;
            }
        }
        if (j > i) {                                       /* skip empty pairs ("&&") */
            bool has_eq = eq < j;
            ioma_slice k = { s + i, (has_eq ? eq : j) - i };
            ioma_slice v = { has_eq ? s + eq + 1 : s + j, has_eq ? j - eq - 1 : 0 };
            size_t mark = used;
            bool   ok   = (!kdec || decode_into(&k, arena, arena_cap, &used)) &&
                          (!vdec || decode_into(&v, arena, arena_cap, &used));
            if (ok)
                out[count++] = (ioma_kv){ k, v };
            else
                used = mark;                               /* skip the pair, give its arena back */
        }
        i = j + 1;
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

/*
 * api.c - the helpers a handler calls: build a response, read a request header, add a response
 * header, reason phrases. Nothing here touches the runtime.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <stdarg.h>
#include <string.h>

/* ── request ───────────────────────────────────────────────────────────────────────────── */

/* Is the slice [s, s+n) exactly the C string? */
bool ioma_slice_eq(const char *s, size_t n, const char *cstr)
{
    return strlen(cstr) == n && memcmp(s, cstr, n) == 0;
}

/* Case-insensitive header lookup. Returns the value slice (not NUL-terminated), or NULL. */
const char *ioma_header_get(const ioma_request *req, const char *name, size_t *value_len)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i < req->n_headers; i++) {
        if (eq_ci(req->headers[i].name, req->headers[i].name_len, name, nl)) {
            if (value_len) *value_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    if (value_len) *value_len = 0;
    return NULL;
}

/* ── response builders ─────────────────────────────────────────────────────────────────── */

/* A response with the given status, content type and body. Sets the fields; does not zero the
 * struct, because extra[] (most of it) is only ever read up to n_extra. */
ioma_response ioma_bytes(int status, const char *content_type, const void *body, size_t body_len)
{
    ioma_response r;
    r.status       = status;
    r.content_type = content_type;
    r.body         = body;
    r.body_len     = body_len;
    r.n_extra      = 0;
    r.close        = false;
    return r;
}

/* text/plain, body length by strlen. */
ioma_response ioma_text(int status, const char *s)
{
    return ioma_bytes(status, "text/plain", s, strlen(s));
}

/* application/json, body length by strlen. */
ioma_response ioma_json(int status, const char *s)
{
    return ioma_bytes(status, "application/json", s, strlen(s));
}

/* printf a text/plain body into req->scratch (truncated to scratch_cap). */
ioma_response ioma_textf(ioma_request *req, int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(req->scratch, req->scratch_cap, fmt, ap);
    va_end(ap);
    size_t len = 0;
    if (n > 0) len = (size_t)n < req->scratch_cap ? (size_t)n : (req->scratch_cap ? req->scratch_cap - 1 : 0);
    return ioma_bytes(status, "text/plain", req->scratch, len);
}

/* Add a header to a response. name/value must stay valid until the reply is sent. */
void ioma_header_set(ioma_response *res, const char *name, const char *value)
{
    if (res->n_extra >= IOMA_MAX_RESP_HEADERS) return;
    ioma_header *h = &res->extra[res->n_extra++];
    h->name = name;   h->name_len = strlen(name);
    h->value = value; h->value_len = strlen(value);
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

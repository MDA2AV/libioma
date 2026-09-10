#include "ioxd/json.h"
#include "http/engine.h"
#include "io/pipe.h"

#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUN_MAX 1024

static_assert(IOXD_JSON_DEPTH < 64, "a level per bit of has_value and is_object, plus the root");

static inline char *tail(ioxd_json *j, size_t *room)
{
    if (j->kind == IOXD_JSON_TO_MEM) {
        if (!j->to.mem.p) {
            *room = 0;
            return nullptr;
        }
        *room = j->to.mem.cap - *j->to.mem.len;
        return j->to.mem.p + *j->to.mem.len;
    }
    ioxd_pipewriter *pw = j->tail;
    *room = pw->cap - pw->len;
    return pw->buf + pw->lead + pw->len;
}

static inline void commit(ioxd_json *j, size_t n)
{
    if (j->kind == IOXD_JSON_TO_MEM)
        *j->to.mem.len += n;
    else
        ((ioxd_pipewriter *)j->tail)->len += n;
}

static inline bool sink_failed(const ioxd_json *j)
{
    switch (j->kind) {
    case IOXD_JSON_TO_REPLY: return ((ioxd_pipewriter *)j->tail)->failed || j->to.ctx->res.failed;
    case IOXD_JSON_TO_PIPE:  return ((ioxd_pipewriter *)j->tail)->failed;
    case IOXD_JSON_TO_MEM:   return false;
    }
    return true;
}

static char *make_room(ioxd_json *j, size_t n)
{
    char *at = nullptr;
    switch (j->kind) {
    case IOXD_JSON_TO_REPLY: at = ioxd_reserve(j->to.ctx, n); break;
    case IOXD_JSON_TO_PIPE:  at = ioxd_pipe_reserve(j->to.pipe, n); break;
    case IOXD_JSON_TO_MEM:   break;
    }
    if (!at)
        j->failed = true;
    return at;
}

static inline char *want(ioxd_json *j, size_t n)
{
    size_t room;
    char  *at = tail(j, &room);
    if (room >= n && !sink_failed(j))
        return at;
    return make_room(j, n);
}

static bool put(ioxd_json *j, const char *p, size_t n)
{
    while (n) {
        size_t room;
        char  *at = tail(j, &room);
        if (room == 0 || sink_failed(j)) {
            if (!make_room(j, n < RUN_MAX ? n : RUN_MAX))
                return false;
            at = tail(j, &room);
        }
        size_t k = n < room ? n : room;
        memcpy(at, p, k);
        commit(j, k);
        p += k;
        n -= k;
    }
    return true;
}

static inline bool needs_escape(unsigned char c)
{
    return c < 0x20 || c == '"' || c == '\\';
}

static size_t clean_run(const char *p, size_t n)
{
    size_t run = 0;
    while (run < n && !needs_escape((unsigned char)p[run]))
        run++;
    return run;
}

static bool put_escaped(ioxd_json *j, const char *p, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    while (n) {
        size_t run = clean_run(p, n);
        if (run && !put(j, p, run))
            return false;
        p += run;
        n -= run;
        if (n == 0)
            break;
        unsigned char c = (unsigned char)*p++;
        n--;
        char   esc[6] = { '\\', 0, 0, 0, 0, 0 };
        size_t len    = 2;
        switch (c) {
        case '"':  esc[1] = '"';  break;
        case '\\': esc[1] = '\\'; break;
        case '\n': esc[1] = 'n';  break;
        case '\r': esc[1] = 'r';  break;
        case '\t': esc[1] = 't';  break;
        case '\b': esc[1] = 'b';  break;
        case '\f': esc[1] = 'f';  break;
        default:
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
    return true;
}

static bool put_string(ioxd_json *j, size_t comma, const char *p, size_t n, char after)
{
    size_t need = comma + n + 2 + (after != 0);
    if (need <= RUN_MAX && clean_run(p, n) == n) {
        size_t room;
        char  *at = tail(j, &room);
        if (at && need <= room && !sink_failed(j)) {
            if (comma)
                *at++ = ',';
            *at++ = '"';
            memcpy(at, p, n);
            at[n] = '"';
            if (after)
                at[n + 1] = after;
            commit(j, need);
            return true;
        }
    }
    if ((comma && !put(j, ",", 1)) || !put(j, "\"", 1) || !put_escaped(j, p, n) || !put(j, "\"", 1))
        return false;
    return after == 0 || put(j, &after, 1);
}

static inline int value_lead(ioxd_json *j)
{
    if (j->failed)
        return -1;
    if (j->after_key) {
        j->after_key = false;
        return 0;
    }
    uint64_t bit = (uint64_t)1 << j->depth;
    if (j->is_object & bit) {
        j->failed = true;
        return -1;
    }
    if (j->has_value & bit)
        return 1;
    j->has_value |= bit;
    return 0;
}

static bool put_value(ioxd_json *j, const char *p, size_t n)
{
    int comma = value_lead(j);
    if (comma < 0)
        return false;
    char *at = want(j, (size_t)comma + n);
    if (!at)
        return false;
    if (comma)
        *at++ = ',';
    memcpy(at, p, n);
    commit(j, (size_t)comma + n);
    return true;
}

ioxd_json ioxd_json_reply(ioxd_ctx *ctx)
{
    ioxd_content_type(ctx, "application/json");
    ioxd_json j = { .kind = IOXD_JSON_TO_REPLY };
    j.to.ctx = ctx;
    j.tail   = ioxd__engine_writer(ctx);
    return j;
}

ioxd_json ioxd_json_pipe(ioxd_pipe *pipe)
{
    ioxd_json j = { .kind = IOXD_JSON_TO_PIPE };
    j.to.pipe = pipe;
    j.tail    = &pipe->out;
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

static bool open_level(ioxd_json *j, char bracket)
{
    if (j->failed)
        return false;
    if (j->depth == IOXD_JSON_DEPTH) {
        j->failed = true;
        return false;
    }
    int comma = value_lead(j);
    if (comma < 0)
        return false;
    j->depth++;
    uint64_t bit = (uint64_t)1 << j->depth;
    j->has_value &= ~bit;
    if (bracket == '{')
        j->is_object |= bit;
    else
        j->is_object &= ~bit;
    char *at = want(j, (size_t)comma + 1);
    if (!at)
        return false;
    if (comma)
        *at++ = ',';
    *at = bracket;
    commit(j, (size_t)comma + 1);
    return true;
}

bool ioxd_json_object(ioxd_json *j)
{
    return open_level(j, '{');
}

bool ioxd_json_array(ioxd_json *j)
{
    return open_level(j, '[');
}

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
    char *at = want(j, 1);
    if (!at)
        return false;
    *at = object ? '}' : ']';
    commit(j, 1);
    return true;
}

bool ioxd_json_done(ioxd_json *j)
{
    return !j->failed && j->depth == 0;
}

bool ioxd__json_key_n(ioxd_json *j, const char *name, size_t len)
{
    if (j->failed)
        return false;
    uint64_t bit = (uint64_t)1 << j->depth;
    if (!(j->is_object & bit) || j->after_key) {
        j->failed = true;
        return false;
    }
    size_t comma = (j->has_value & bit) ? 1 : 0;
    j->has_value |= bit;
    if (!put_string(j, comma, name, len, ':'))
        return false;
    j->after_key = true;
    return true;
}

bool ioxd_json_key(ioxd_json *j, const char *name)
{
    return ioxd__json_key_n(j, name, strlen(name));
}

bool ioxd_json_string(ioxd_json *j, ioxd_slice s)
{
    int comma = value_lead(j);
    return comma >= 0 && put_string(j, (size_t)comma, s.p, s.len, 0);
}

bool ioxd_json_cstr(ioxd_json *j, const char *s)
{
    if (!s)
        return ioxd_json_null(j);
    int comma = value_lead(j);
    return comma >= 0 && put_string(j, (size_t)comma, s, strlen(s), 0);
}

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
    return put_value(j, p, (size_t)(tmp + sizeof tmp - p));
}

bool ioxd_json_int(ioxd_json *j, int64_t v)
{
    char     tmp[24];
    uint64_t magnitude = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    char    *p = digits(tmp + sizeof tmp, magnitude);
    if (v < 0)
        *--p = '-';
    return put_value(j, p, (size_t)(tmp + sizeof tmp - p));
}

static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;
static locale_t       c_locale;

static void make_c_locale(void)
{
    c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

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
    return put_value(j, tmp, (size_t)n);
}

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
    return put_value(j, tmp, (size_t)n);
}

bool ioxd_json_bool(ioxd_json *j, bool v)
{
    return v ? put_value(j, "true", 4) : put_value(j, "false", 5);
}

bool ioxd_json_null(ioxd_json *j)
{
    return put_value(j, "null", 4);
}

bool ioxd_json_raw(ioxd_json *j, ioxd_slice json)
{
    if (json.len == 0) {
        j->failed = true;
        return false;
    }
    int comma = value_lead(j);
    if (comma < 0)
        return false;
    if (comma && !put(j, ",", 1))
        return false;
    return put(j, json.p, json.len);
}

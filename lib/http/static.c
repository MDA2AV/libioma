#include "http/static.h"
#include "http/internal.h"
#include "io/coro.h"
#include "io/internal.h"
#include "ioxd/http.h"
#include "ioxd/static.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ROOTS            16
#define NAME_CAP         1024
#define PIECE            16384
#define SUFFIX_MAX       4
#define CACHE_DEFAULT    (64UL * 1024 * 1024)
#define FILE_MAX_DEFAULT (8UL * 1024 * 1024)

enum variant { PLAIN, BR, GZIP, VARIANTS };
static const char *const suffix[VARIANTS] = { "", ".br", ".gz" };
static const char *const coding[VARIANTS] = { "", "br", "gzip" };

enum range { RANGE_NONE, RANGE_ONE, RANGE_BAD };

struct bytes {
    char  *data;
    size_t len;
};

struct entry {
    struct entry   *chain;
    struct entry   *newer, *older;
    char           *name;
    size_t          name_len;
    uint32_t        hash;
    dev_t           dev;
    ino_t           ino;
    off_t           size;
    struct timespec mtime;
    const char     *type;
    struct bytes    v[VARIANTS];
    size_t          bytes;
    char            etag[48];
    char            modified[32];
    uint64_t        checked_at;
    unsigned        refs;
    bool            dropped;
};

struct cache {
    struct cache  *next;
    struct entry **buckets;
    size_t         n_buckets, count;
    struct entry  *newest, *oldest;
    size_t         bytes, budget;
};

struct ioxd_static {
    int              dirfd;
    char            *dir, *mount, *index, *cache_control;
    size_t           mount_len, index_len;
    size_t           budget, file_max;
    uint64_t         revalidate_ms;
    bool             precompressed, hidden;
    int              slot;
    unsigned         gen;
    pthread_mutex_t  lock;
    struct cache    *caches;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool            g_taken[ROOTS];
static unsigned        g_gen[ROOTS];
static _Thread_local struct {
    struct cache *cache;
    unsigned      gen;
} tl[ROOTS];

/* ── names, types, dates ───────────────────────────────────────────────────────────────── */

static const struct {
    const char *ext, *type;
} types[] = {
    { "html", "text/html; charset=utf-8" },     { "htm", "text/html; charset=utf-8" },
    { "css", "text/css" },                      { "js", "text/javascript" },
    { "mjs", "text/javascript" },               { "json", "application/json" },
    { "map", "application/json" },              { "webmanifest", "application/manifest+json" },
    { "xml", "application/xml" },               { "txt", "text/plain; charset=utf-8" },
    { "md", "text/markdown; charset=utf-8" },   { "csv", "text/csv; charset=utf-8" },
    { "svg", "image/svg+xml" },                 { "png", "image/png" },
    { "jpg", "image/jpeg" },                    { "jpeg", "image/jpeg" },
    { "gif", "image/gif" },                     { "webp", "image/webp" },
    { "avif", "image/avif" },                   { "ico", "image/x-icon" },
    { "woff", "font/woff" },                    { "woff2", "font/woff2" },
    { "ttf", "font/ttf" },                      { "otf", "font/otf" },
    { "pdf", "application/pdf" },               { "wasm", "application/wasm" },
    { "mp4", "video/mp4" },                     { "webm", "video/webm" },
    { "mp3", "audio/mpeg" },                    { "ogg", "audio/ogg" },
    { "zip", "application/zip" },               { "gz", "application/gzip" },
};

static const char *type_of(const char *name, size_t len)
{
    ioxd_slice s     = { name, len };
    ptrdiff_t  dot   = ioxd_slice_rfind_char(s, '.');
    ptrdiff_t  slash = ioxd_slice_rfind_char(s, '/');
    if (dot > slash) {
        ioxd_slice ext = ioxd_slice_from(s, (size_t)dot + 1);
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (ioxd_slice_eq_ci(ext, types[i].ext))
                return types[i].type;
    }
    return "application/octet-stream";
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
    return (uint64_t)t.tv_sec * 1000U + (uint64_t)t.tv_nsec / 1000000U;
}

static const char *const days[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void http_date(char *out, size_t cap, time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static bool number(const char *p, size_t n, int *out)
{
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    return true;
}

static bool parse_date(ioxd_slice s, time_t *out)
{
    s = ioxd_slice_trim(s);
    if (s.len != 29 || s.p[3] != ',' || s.p[4] != ' ' || s.p[7] != ' ' || s.p[11] != ' ' || s.p[16] != ' '
        || s.p[19] != ':' || s.p[22] != ':' || memcmp(s.p + 25, " GMT", 4) != 0)
        return false;
    struct tm tm = {};
    int       month = -1;
    for (int i = 0; i < 12; i++)
        if (memcmp(s.p + 8, months[i], 3) == 0)
            month = i;
    if (month < 0 || !number(s.p + 5, 2, &tm.tm_mday) || !number(s.p + 12, 4, &tm.tm_year)
        || !number(s.p + 17, 2, &tm.tm_hour) || !number(s.p + 20, 2, &tm.tm_min) || !number(s.p + 23, 2, &tm.tm_sec))
        return false;
    tm.tm_mon = month;
    tm.tm_year -= 1900;
    *out = timegm(&tm);
    return *out != (time_t)-1;
}

static void stamp(const struct stat *sb, char *etag, size_t etag_cap, char *modified, size_t modified_cap)
{
    uint64_t mtime = (uint64_t)sb->st_mtim.tv_sec * 1000000000U + (uint64_t)sb->st_mtim.tv_nsec;
    snprintf(etag, etag_cap, "\"%" PRIx64 "-%" PRIx64 "\"", (uint64_t)sb->st_size, mtime);
    http_date(modified, modified_cap, sb->st_mtim.tv_sec);
}

static uint32_t hash_of(const char *p, size_t n)
{
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 16777619U;
    }
    return h;
}

/* ── the worker's table ────────────────────────────────────────────────────────────────── */

static struct cache *cache_of(ioxd_static *st)
{
    if (tl[st->slot].cache && tl[st->slot].gen == st->gen)
        return tl[st->slot].cache;
    struct cache *c = calloc(1, sizeof *c);
    if (!c)
        return nullptr;
    c->n_buckets = 64;
    c->buckets   = calloc(c->n_buckets, sizeof *c->buckets);
    c->budget    = st->budget;
    if (!c->buckets) {
        free(c);
        return nullptr;
    }
    pthread_mutex_lock(&st->lock);
    c->next    = st->caches;
    st->caches = c;
    pthread_mutex_unlock(&st->lock);
    tl[st->slot].cache = c;
    tl[st->slot].gen   = st->gen;
    return c;
}

static struct entry *lookup(const struct cache *c, const char *name, size_t len, uint32_t hash)
{
    for (struct entry *e = c->buckets[hash & (c->n_buckets - 1)]; e; e = e->chain)
        if (e->hash == hash && e->name_len == len && memcmp(e->name, name, len) == 0)
            return e;
    return nullptr;
}

static void recency_out(struct cache *c, struct entry *e)
{
    if (e->newer)
        e->newer->older = e->older;
    else
        c->newest = e->older;
    if (e->older)
        e->older->newer = e->newer;
    else
        c->oldest = e->newer;
    e->newer = e->older = nullptr;
}

static void recency_in(struct cache *c, struct entry *e)
{
    e->older = c->newest;
    e->newer = nullptr;
    if (c->newest)
        c->newest->newer = e;
    c->newest = e;
    if (!c->oldest)
        c->oldest = e;
}

static void touch(struct cache *c, struct entry *e)
{
    if (c->newest != e) {
        recency_out(c, e);
        recency_in(c, e);
    }
}

static void entry_free(struct entry *e)
{
    for (int v = 0; v < VARIANTS; v++)
        free(e->v[v].data);
    free(e->name);
    free(e);
}

static void unlink_entry(struct cache *c, struct entry *e)
{
    struct entry **at = &c->buckets[e->hash & (c->n_buckets - 1)];
    while (*at != e)
        at = &(*at)->chain;
    *at = e->chain;
    recency_out(c, e);
    c->bytes -= e->bytes;
    c->count--;
}

static void drop(struct cache *c, struct entry *e)
{
    unlink_entry(c, e);
    e->dropped = true;
    if (!e->refs)
        entry_free(e);
}

static void release(struct entry *e)
{
    if (--e->refs == 0 && e->dropped)
        entry_free(e);
}

static void make_room(struct cache *c, size_t need)
{
    while (c->bytes + need > c->budget && c->oldest)
        drop(c, c->oldest);
}

static void grow(struct cache *c)
{
    size_t         n       = c->n_buckets * 2;
    struct entry **buckets = calloc(n, sizeof *buckets);
    if (!buckets)
        return;
    for (size_t i = 0; i < c->n_buckets; i++)
        for (struct entry *e = c->buckets[i], *next; e; e = next) {
            next     = e->chain;
            e->chain = buckets[e->hash & (n - 1)];
            buckets[e->hash & (n - 1)] = e;
        }
    free(c->buckets);
    c->buckets   = buckets;
    c->n_buckets = n;
}

static void insert(struct cache *c, struct entry *e)
{
    if (c->count >= c->n_buckets)
        grow(c);
    struct entry **bucket = &c->buckets[e->hash & (c->n_buckets - 1)];
    e->chain = *bucket;
    *bucket  = e;
    recency_in(c, e);
    c->bytes += e->bytes;
    c->count++;
}

/* ── the disk ──────────────────────────────────────────────────────────────────────────── */

static ssize_t read_at(int fd, void *buf, size_t n, off_t off)
{
    proactor_t *p = ioxd__proactor_current();
    if (!p || !ioxd__coro_current())
        return pread(fd, buf, n, off);
    op_t                 op;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode = IORING_OP_READ;
    sqe->fd     = fd;
    sqe->addr   = (uintptr_t)buf;
    sqe->len    = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
    sqe->off    = (uint64_t)off;
    return ioxd__io_await(sqe, &op);
}

static bool read_whole(int fd, size_t size, struct bytes *out)
{
    char *data = malloc(size ? size : 1);
    if (!data)
        return false;
    for (size_t got = 0; got < size;) {
        ssize_t n = read_at(fd, data + got, size - got, (off_t)got);
        if (n <= 0) {
            free(data);
            return false;
        }
        got += (size_t)n;
    }
    out->data = data;
    out->len  = size;
    return true;
}

static void twin(const ioxd_static *st, const char *name, size_t len, const char *sfx, struct bytes *out)
{
    char path[NAME_CAP + SUFFIX_MAX];
    memcpy(path, name, len);
    memcpy(path + len, sfx, strlen(sfx) + 1);
    int fd = openat(st->dirfd, path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return;
    struct stat sb;
    if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && (uint64_t)sb.st_size <= st->file_max)
        read_whole(fd, (size_t)sb.st_size, out);
    close(fd);
}

static struct entry *load(const ioxd_static *st, struct cache *c, const char *name, size_t len, int fd, const struct stat *sb)
{
    struct entry *e = calloc(1, sizeof *e);
    if (!e)
        return nullptr;
    e->name = malloc(len + 1);
    if (!e->name) {
        free(e);
        return nullptr;
    }
    memcpy(e->name, name, len + 1);
    e->name_len   = len;
    e->hash       = hash_of(name, len);
    e->dev        = sb->st_dev;
    e->ino        = sb->st_ino;
    e->size       = sb->st_size;
    e->mtime      = sb->st_mtim;
    e->type       = type_of(name, len);
    e->checked_at = st->revalidate_ms ? now_ms() : 0;
    stamp(sb, e->etag, sizeof e->etag, e->modified, sizeof e->modified);
    if (!read_whole(fd, (size_t)sb->st_size, &e->v[PLAIN])) {
        entry_free(e);
        return nullptr;
    }
    if (st->precompressed)
        for (int v = BR; v < VARIANTS; v++)
            twin(st, name, len, suffix[v], &e->v[v]);
    for (int v = 0; v < VARIANTS; v++)
        e->bytes += e->v[v].len;
    if (e->bytes <= c->budget) {
        make_room(c, e->bytes);
        insert(c, e);
    } else {
        e->dropped = true;
    }
    return e;
}

static bool current(const ioxd_static *st, struct entry *e)
{
    if (st->revalidate_ms) {
        uint64_t now = now_ms();
        if (now - e->checked_at < st->revalidate_ms)
            return true;
        e->checked_at = now;
    }
    struct stat sb;
    return fstatat(st->dirfd, e->name, &sb, 0) == 0 && S_ISREG(sb.st_mode) && sb.st_ino == e->ino && sb.st_dev == e->dev
        && sb.st_size == e->size && sb.st_mtim.tv_sec == e->mtime.tv_sec && sb.st_mtim.tv_nsec == e->mtime.tv_nsec;
}

static bool resolve(const ioxd_static *st, ioxd_slice path, bool decode, char *out, size_t *out_len)
{
    char   raw[NAME_CAP];
    size_t len = 0;
    if (path.len >= NAME_CAP)
        return false;
    for (size_t i = 0; i < path.len; i++) {
        unsigned char ch = (unsigned char)path.p[i];
        if (decode && ch == '%' && i + 2 < path.len) {
            int hi = ioxd__http_hexval((unsigned char)path.p[i + 1]), lo = ioxd__http_hexval((unsigned char)path.p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                ch = (unsigned char)(hi * 16 + lo);
                i += 2;
            }
        }
        if (ch == 0)
            return false;
        raw[len++] = (char)ch;
    }
    bool       dir = len == 0 || raw[len - 1] == '/';
    size_t     n   = 0;
    ioxd_slice rest = { raw, len }, seg;
    while (rest.len) {
        ioxd_slice_cut(rest, '/', &seg, &rest);
        if (seg.len == 0)
            continue;
        if (seg.p[0] == '.' && (!st->hidden || ioxd_slice_eq(seg, ".") || ioxd_slice_eq(seg, "..")))
            return false;
        if (n)
            out[n++] = '/';
        memcpy(out + n, seg.p, seg.len);
        n += seg.len;
    }
    if (dir) {
        if (!st->index_len || n + 1 + st->index_len >= NAME_CAP)
            return false;
        if (n)
            out[n++] = '/';
        memcpy(out + n, st->index, st->index_len);
        n += st->index_len;
    }
    out[n]   = '\0';
    *out_len = n;
    return n > 0;
}

static int open_file(const ioxd_static *st, char *name, size_t *len, struct stat *sb)
{
    int fd = openat(st->dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return -1;
    if (fstat(fd, sb) != 0) {
        close(fd);
        return -1;
    }
    if (S_ISDIR(sb->st_mode)) {
        close(fd);
        if (!st->index_len || *len + 1 + st->index_len >= NAME_CAP)
            return -1;
        name[*len] = '/';
        memcpy(name + *len + 1, st->index, st->index_len + 1);
        *len += 1 + st->index_len;
        fd = openat(st->dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd < 0)
            return -1;
        if (fstat(fd, sb) != 0) {
            close(fd);
            return -1;
        }
    }
    if (!S_ISREG(sb->st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── the reply ─────────────────────────────────────────────────────────────────────────── */

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

static double coding_q(ioxd_slice accept, const char *name)
{
    double     any = 0;
    ioxd_slice item, coding, params;
    while (ioxd_slice_next(&accept, ',', &item)) {
        ioxd_slice_cut(item, ';', &coding, &params);
        coding = ioxd_slice_trim(coding);
        if (ioxd_slice_eq_ci(coding, name))
            return qvalue(params);
        if (ioxd_slice_eq(coding, "*"))
            any = qvalue(params);
    }
    return any;
}

static enum variant pick(const ioxd_ctx *ctx, const struct entry *e)
{
    ioxd_slice accept = ioxd_req_header(ctx, "accept-encoding");
    if (!accept.p)
        return PLAIN;
    double br = e->v[BR].data ? coding_q(accept, coding[BR]) : 0;
    double gz = e->v[GZIP].data ? coding_q(accept, coding[GZIP]) : 0;
    if (br > 0 && br >= gz)
        return BR;
    return gz > 0 ? GZIP : PLAIN;
}

static bool not_modified(const ioxd_ctx *ctx, const char *etag, time_t mtime)
{
    ioxd_slice tags = ioxd_req_header(ctx, "if-none-match"), tag;
    if (tags.p) {
        while (ioxd_slice_next(&tags, ',', &tag)) {
            if (ioxd_slice_starts_with(tag, "W/"))
                tag = ioxd_slice_from(tag, 2);
            if (ioxd_slice_eq(tag, "*") || ioxd_slice_eq(tag, etag))
                return true;
        }
        return false;
    }
    ioxd_slice since = ioxd_req_header(ctx, "if-modified-since");
    time_t     t;
    return since.p && parse_date(since, &t) && mtime <= t;
}

static enum range range_of(const ioxd_ctx *ctx, const char *etag, size_t total, size_t *first, size_t *last)
{
    ioxd_slice range = ioxd_req_header(ctx, "range");
    if (!range.p || !ioxd_slice_starts_with(range, "bytes="))
        return RANGE_NONE;
    ioxd_slice if_range = ioxd_req_header(ctx, "if-range");
    if (if_range.p && !ioxd_slice_eq(ioxd_slice_trim(if_range), etag))
        return RANGE_NONE;
    ioxd_slice spec = ioxd_slice_trim(ioxd_slice_from(range, 6)), lo, hi;
    if (ioxd_slice_find_char(spec, ',') >= 0 || !ioxd_slice_cut(spec, '-', &lo, &hi))
        return RANGE_NONE;
    lo = ioxd_slice_trim(lo);
    hi = ioxd_slice_trim(hi);
    uint64_t a, b;
    if (lo.len == 0) {
        if (!ioxd_to_u64(hi, &b))
            return RANGE_NONE;
        if (b == 0 || total == 0)
            return RANGE_BAD;
        if (b > total)
            b = total;
        *first = total - b;
        *last  = total - 1;
        return RANGE_ONE;
    }
    if (!ioxd_to_u64(lo, &a))
        return RANGE_NONE;
    if (hi.len == 0)
        b = UINT64_MAX;
    else if (!ioxd_to_u64(hi, &b))
        return RANGE_NONE;
    if (a >= total || a > b)
        return RANGE_BAD;
    *first = (size_t)a;
    *last  = b >= total ? total - 1 : (size_t)b;
    return RANGE_ONE;
}

static void content_range(ioxd_ctx *ctx, const char *span, size_t total)
{
    char line[80];
    snprintf(line, sizeof line, "bytes %s/%zu", span, total);
    ioxd_header(ctx, "content-range", line);
}

static bool head_of(ioxd_ctx *ctx, const ioxd_static *st, const char *type, const char *etag, const char *modified,
                    time_t mtime, bool twins)
{
    ctx->res.content_type = (ioxd_slice){ type, strlen(type) };
    ioxd_header(ctx, "etag", etag);
    ioxd_header(ctx, "last-modified", modified);
    if (st->cache_control)
        ioxd_header(ctx, "cache-control", st->cache_control);
    if (twins)
        ioxd_header(ctx, "vary", "accept-encoding");
    if (not_modified(ctx, etag, mtime)) {
        ctx->res.status = 304;
        return false;
    }
    ioxd_header(ctx, "accept-ranges", "bytes");
    return true;
}

static bool span_of(ioxd_ctx *ctx, const char *etag, size_t total, size_t *first, size_t *n)
{
    size_t last = 0;
    switch (range_of(ctx, etag, total, first, &last)) {
    case RANGE_BAD:
        ctx->res.status = 416;
        content_range(ctx, "*", total);
        return false;
    case RANGE_ONE:
        ctx->res.status = 206;
        char span[48];
        snprintf(span, sizeof span, "%zu-%zu", *first, last);
        content_range(ctx, span, total);
        *n = last - *first + 1;
        return true;
    default:
        *first = 0;
        *n     = total;
        return true;
    }
}

static void reply(ioxd_ctx *ctx, const ioxd_static *st, struct entry *e, bool head)
{
    bool         twins = e->v[BR].data || e->v[GZIP].data;
    enum variant v     = twins ? pick(ctx, e) : PLAIN;
    if (!head_of(ctx, st, e->type, e->etag, e->modified, e->mtime.tv_sec, twins))
        return;
    if (v != PLAIN)
        ioxd_header(ctx, "content-encoding", coding[v]);
    size_t first, n;
    if (!span_of(ctx, e->etag, e->v[v].len, &first, &n))
        return;
    ioxd_content_length(ctx, n);
    if (head)
        return;
    e->refs++;
    ioxd_write(ctx, e->v[v].data + first, n);
    release(e);
}

static void stream(ioxd_ctx *ctx, const ioxd_static *st, int fd, const struct stat *sb, const char *name, size_t len, bool head)
{
    char etag[48], modified[32];
    stamp(sb, etag, sizeof etag, modified, sizeof modified);
    if (!head_of(ctx, st, type_of(name, len), etag, modified, sb->st_mtim.tv_sec, false))
        return;
    size_t first, n;
    if (!span_of(ctx, etag, (size_t)sb->st_size, &first, &n))
        return;
    ioxd_content_length(ctx, n);
    if (head)
        return;
    for (off_t off = (off_t)first; n;) {
        size_t piece = n < PIECE ? n : PIECE;
        char  *at    = ioxd_reserve(ctx, piece);
        if (!at)
            return;
        ssize_t got = read_at(fd, at, piece, off);
        if (got <= 0)
            return;
        ioxd_advance(ctx, (size_t)got);
        off += got;
        n -= (size_t)got;
    }
}

static void serve(ioxd_ctx *ctx, ioxd_static *st, char *name, size_t len)
{
    bool head = ioxd_slice_eq(ctx->req.method, "HEAD");
    if (!head && !ioxd_slice_eq(ctx->req.method, "GET")) {
        ctx->res.status = 405;
        ioxd_header(ctx, "allow", "GET, HEAD");
        return;
    }
    struct cache *c = cache_of(st);
    if (!c) {
        ctx->res.status = 500;
        return;
    }
    struct entry *e = lookup(c, name, len, hash_of(name, len));
    if (e && !current(st, e)) {
        drop(c, e);
        e = nullptr;
    }
    if (!e) {
        struct stat sb;
        size_t      asked = len;
        int         fd    = open_file(st, name, &len, &sb);
        if (fd < 0) {
            ctx->res.status = 404;
            return;
        }
        e = len != asked ? lookup(c, name, len, hash_of(name, len)) : nullptr;
        if (e) {
            if (current(st, e)) {
                close(fd);
                touch(c, e);
                reply(ctx, st, e, head);
                return;
            }
            drop(c, e);
        }
        if ((uint64_t)sb.st_size > st->file_max) {
            stream(ctx, st, fd, &sb, name, len, head);
            close(fd);
            return;
        }
        e = load(st, c, name, len, fd, &sb);
        close(fd);
        if (!e) {
            ctx->res.status = 500;
            return;
        }
    } else {
        touch(c, e);
    }
    reply(ctx, st, e, head);
}

/* ── the store ─────────────────────────────────────────────────────────────────────────── */

static char *mount_of(const char *mount)
{
    ioxd_slice s = { mount ? mount : "", mount ? strlen(mount) : 0 };
    while (s.len && s.p[0] == '/')
        s = ioxd_slice_from(s, 1);
    while (s.len && s.p[s.len - 1] == '/')
        s.len--;
    char *m = malloc(s.len + 2);
    if (!m)
        return nullptr;
    m[0] = '/';
    memcpy(m + 1, s.p, s.len);
    m[s.len + 1] = '\0';
    if (s.len == 0)
        m[0] = '\0';
    return m;
}

ioxd_static *ioxd_static_open(const ioxd_static_config *config)
{
    if (!config || !config->dir || !*config->dir) {
        fprintf(stderr, "ioxd_static_open: a directory is required\n");
        return nullptr;
    }
    int dirfd = open(config->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        fprintf(stderr, "ioxd_static_open: %s: %s\n", config->dir, ioxd__io_errstr(errno));
        return nullptr;
    }
    int slot = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < ROOTS && slot < 0; i++)
        if (!g_taken[i]) {
            g_taken[i] = true;
            g_gen[i]++;
            slot = i;
        }
    unsigned gen = slot >= 0 ? g_gen[slot] : 0;
    pthread_mutex_unlock(&g_lock);
    if (slot < 0) {
        fprintf(stderr, "ioxd_static_open: %s: at most %d directories open at once\n", config->dir, ROOTS);
        close(dirfd);
        return nullptr;
    }
    ioxd_static *st = calloc(1, sizeof *st);
    const char  *index = config->index ? config->index : "index.html";
    if (st) {
        st->dirfd         = dirfd;
        st->dir           = strdup(config->dir);
        st->mount         = mount_of(config->mount);
        st->index         = strdup(index);
        st->cache_control = config->cache_control ? strdup(config->cache_control) : nullptr;
        st->budget        = config->cache_bytes ? config->cache_bytes : CACHE_DEFAULT;
        st->file_max      = config->cache_file_max ? config->cache_file_max : FILE_MAX_DEFAULT;
        st->revalidate_ms = config->revalidate_ms;
        st->precompressed = config->precompressed;
        st->hidden        = config->hidden;
        st->slot          = slot;
        st->gen           = gen;
        pthread_mutex_init(&st->lock, nullptr);
    }
    if (!st || !st->dir || !st->mount || !st->index || (config->cache_control && !st->cache_control)) {
        fprintf(stderr, "ioxd_static_open: %s: out of memory\n", config->dir);
        ioxd_static_close(st);
        return nullptr;
    }
    st->mount_len = strlen(st->mount);
    st->index_len = strlen(st->index);
    return st;
}

bool ioxd_static_serve(ioxd_ctx *ctx, ioxd_static *files)
{
    ioxd_slice path = ctx->req.path;
    if (path.len < files->mount_len || memcmp(path.p, files->mount, files->mount_len) != 0)
        return false;
    if (path.len > files->mount_len && path.p[files->mount_len] != '/')
        return false;
    char   name[NAME_CAP];
    size_t len;
    if (!resolve(files, ioxd_slice_from(path, files->mount_len), true, name, &len))
        ctx->res.status = 404;
    else
        serve(ctx, files, name, len);
    return true;
}

void ioxd_static_file(ioxd_ctx *ctx, ioxd_static *files, const char *name)
{
    char   rel[NAME_CAP];
    size_t len;
    if (!name || !resolve(files, (ioxd_slice){ name, strlen(name) }, false, rel, &len))
        ctx->res.status = 404;
    else
        serve(ctx, files, rel, len);
}

void ioxd_static_close(ioxd_static *files)
{
    if (!files)
        return;
    pthread_mutex_lock(&g_lock);
    g_taken[files->slot] = false;
    g_gen[files->slot]++;
    pthread_mutex_unlock(&g_lock);
    for (struct cache *c = files->caches, *next; c; c = next) {
        next = c->next;
        for (size_t i = 0; i < c->n_buckets; i++)
            for (struct entry *e = c->buckets[i], *more; e; e = more) {
                more = e->chain;
                entry_free(e);
            }
        free(c->buckets);
        free(c);
    }
    if (files->dirfd >= 0)
        close(files->dirfd);
    pthread_mutex_destroy(&files->lock);
    free(files->dir);
    free(files->mount);
    free(files->index);
    free(files->cache_control);
    free(files);
}

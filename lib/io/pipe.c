#include "io/pipe.h"
#include "io/proactor.h"

#include <string.h>
#include <sys/uio.h>

void ioxd__pipereader_init(ioxd_pipereader *pr, conn_t *conn, char *buf, size_t cap)
{
    *pr      = (ioxd_pipereader){};
    pr->conn = conn;
    pr->buf  = buf;
    pr->cap  = cap;
}

static ioxd_slice live_span(const ioxd_pipereader *pr)
{
    if (pr->live_in_buf)
        return (ioxd_slice){ pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos };
    if (pr->has_cur)
        return (ioxd_slice){ (const char *)pr->cur.ptr + pr->cur_pos, pr->cur.len - pr->cur_pos };
    return (ioxd_slice){ nullptr, 0 };
}

static void cur_done(ioxd_pipereader *pr)
{
    if (!pr->has_cur || pr->cur_pos < pr->cur.len || (pr->run_in_cur && pr->run_len))
        return;
    if (!pr->cur_is_pinned)
        ioxd__bufring_return(&pr->conn->p->bufs, pr->cur.buf_id);
    pr->has_cur       = false;
    pr->cur_is_pinned = false;
    pr->cur_pos       = 0;
}

static void compact(ioxd_pipereader *pr)
{
    if (pr->buf_pos > pr->floor) {
        memmove(pr->buf + pr->floor, pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos);
        pr->buf_end -= pr->buf_pos - pr->floor;
        pr->buf_pos  = pr->floor;
    }
}

static bool gather(ioxd_pipereader *pr)
{
    size_t live = pr->has_cur ? pr->cur.len - pr->cur_pos : 0;
    size_t run  = pr->run_in_cur ? pr->run_len : 0;
    if (pr->floor + run + live > pr->cap)
        return false;
    if (run) {
        memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->run_start, run);
        pr->run_start = pr->floor;
        pr->floor    += run;
        if (!pr->has_pinned) {
            pr->pinned        = pr->cur;
            pr->has_pinned    = true;
            pr->cur_is_pinned = true;
        }
    }
    pr->run_in_cur = false;
    if (live)
        memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->cur_pos, live);
    pr->buf_pos     = pr->floor;
    pr->buf_end     = pr->floor + live;
    pr->live_in_buf = true;
    if (pr->has_cur) {
        pr->cur_pos = pr->cur.len;
        cur_done(pr);
    }
    return true;
}

static int refuse(ioxd_pipereader *pr, const struct rx_item *item)
{
    ioxd__bufring_return(&pr->conn->p->bufs, item->buf_id);
    pr->error = IOXD_PIPE_FULL;
    return IOXD_PIPE_FULL;
}

static int more(ioxd_pipereader *pr)
{
    if (pr->eof)
        return 0;
    struct rx_item item;
    int rc = ioxd__conn_recv_item(pr->conn, &item);
    if (rc <= 0) {
        pr->eof = true;
        if (rc < 0)
            pr->error = IOXD_PIPE_GONE;
        return rc < 0 ? IOXD_PIPE_GONE : 0;
    }
    if (!pr->live_in_buf && !pr->has_cur) {
        pr->cur           = item;
        pr->has_cur       = true;
        pr->cur_pos       = 0;
        pr->cur_is_pinned = false;
        return 1;
    }
    if (!pr->live_in_buf && !gather(pr))
        return refuse(pr, &item);
    if (pr->buf_end + item.len > pr->cap) {
        compact(pr);
        if (pr->buf_end + item.len > pr->cap)
            return refuse(pr, &item);
    }
    memcpy(pr->buf + pr->buf_end, item.ptr, item.len);
    pr->buf_end += item.len;
    ioxd__bufring_return(&pr->conn->p->bufs, item.buf_id);
    return 1;
}

int ioxd__pipereader_read(ioxd_pipereader *pr, ioxd_slice *live)
{
    if (pr->error)
        return pr->error;
    for (;;) {
        ioxd_slice l = live_span(pr);
        if (l.len > pr->examined) {
            *live = l;
            return 1;
        }
        int rc = more(pr);
        if (rc <= 0)
            return rc;
    }
}

void ioxd__pipereader_examine(ioxd_pipereader *pr, size_t n)
{
    pr->examined = n;
}

static void consume(ioxd_pipereader *pr, size_t n)
{
    size_t have = live_span(pr).len;
    if (n > have)
        n = have;
    if (pr->live_in_buf) {
        pr->buf_pos += n;
        if (pr->buf_pos >= pr->buf_end) {
            pr->buf_pos = pr->buf_end = pr->floor;
            pr->live_in_buf = false;
        }
    } else if (pr->has_cur) {
        pr->cur_pos += n;
        cur_done(pr);
    }
    pr->examined = pr->examined > n ? pr->examined - n : 0;
}

void ioxd__pipereader_drop(ioxd_pipereader *pr, size_t n)
{
    consume(pr, n);
}

const char *ioxd__pipereader_keep(ioxd_pipereader *pr, size_t n)
{
    const char *kept;
    ioxd_slice  live = live_span(pr);
    if (n > live.len)
        return nullptr;
    if (n == 0)
        return live.p;
    if (pr->live_in_buf) {
        if (pr->run_len == 0) {
            pr->run_in_cur = false;
            pr->run_start  = pr->buf_pos;
            pr->floor      = pr->buf_pos;
        } else if (pr->buf_pos != pr->floor) {
            memmove(pr->buf + pr->floor, pr->buf + pr->buf_pos, n);
        }
        kept = pr->buf + pr->floor;
        pr->floor += n;
    } else if (pr->has_cur && (pr->run_len == 0 || pr->run_in_cur)) {
        if (pr->run_len == 0) {
            pr->run_in_cur = true;
            pr->run_start  = pr->cur_pos;
        }
        size_t run_end = pr->run_start + pr->run_len;
        if (run_end != pr->cur_pos)
            memmove((char *)pr->cur.ptr + run_end, (const char *)pr->cur.ptr + pr->cur_pos, n);
        kept = (const char *)pr->cur.ptr + run_end;
    } else if (pr->has_cur) {
        if (pr->floor + n > pr->cap) {
            pr->error = IOXD_PIPE_FULL;
            return nullptr;
        }
        memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->cur_pos, n);
        kept = pr->buf + pr->floor;
        pr->floor += n;
        pr->buf_pos = pr->buf_end = pr->floor;
    } else {
        return nullptr;
    }
    pr->run_len += n;
    consume(pr, n);
    return kept;
}

void ioxd__pipereader_run_begin(ioxd_pipereader *pr)
{
    if (pr->run_in_cur && pr->run_len) {
        if (pr->has_pinned && !pr->cur_is_pinned) {
            if (pr->floor + pr->run_len > pr->cap) {
                pr->error = IOXD_PIPE_FULL;
            } else {
                memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->run_start, pr->run_len);
                pr->floor += pr->run_len;
            }
        } else {
            pr->pinned        = pr->cur;
            pr->has_pinned    = true;
            pr->cur_is_pinned = true;
        }
    }
    pr->run_len    = 0;
    pr->run_in_cur = false;
    cur_done(pr);
}

ioxd_slice ioxd__pipereader_run(const ioxd_pipereader *pr)
{
    const char *base = pr->run_in_cur ? (const char *)pr->cur.ptr : pr->buf;
    return (ioxd_slice){ base + pr->run_start, pr->run_len };
}

void ioxd__pipereader_release(ioxd_pipereader *pr)
{
    pr->run_len    = 0;
    pr->run_in_cur = false;
    if (pr->live_in_buf) {
        memmove(pr->buf, pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos);
        pr->buf_end -= pr->buf_pos;
        pr->buf_pos  = 0;
    } else {
        pr->buf_pos = pr->buf_end = 0;
    }
    pr->floor = 0;
    if (pr->has_pinned) {
        if (pr->cur_is_pinned)
            pr->cur_is_pinned = false;
        else
            ioxd__bufring_return(&pr->conn->p->bufs, pr->pinned.buf_id);
        pr->has_pinned = false;
    }
    cur_done(pr);
}

void ioxd__pipereader_close(ioxd_pipereader *pr)
{
    if (pr->has_cur && !pr->cur_is_pinned)
        ioxd__bufring_return(&pr->conn->p->bufs, pr->cur.buf_id);
    if (pr->has_pinned)
        ioxd__bufring_return(&pr->conn->p->bufs, pr->pinned.buf_id);
    pr->has_cur = pr->has_pinned = pr->cur_is_pinned = false;
}

int ioxd__pipereader_copy(ioxd_pipereader *pr, void *dst, size_t n)
{
    if (pr->error)
        return pr->error;
    for (;;) {
        ioxd_slice l = live_span(pr);
        if (l.len) {
            size_t k = l.len < n ? l.len : n;
            memcpy(dst, l.p, k);
            consume(pr, k);
            return (int)k;
        }
        int rc = more(pr);
        if (rc <= 0)
            return rc;
    }
}

int ioxd__pipereader_avail(ioxd_pipereader *pr, ioxd_slice *live)
{
    if (pr->error)
        return pr->error;
    ioxd_slice l = live_span(pr);
    while (l.len <= pr->examined) {
        if (ioxd__spsc_empty(&pr->conn->rx))
            return 0;
        int rc = more(pr);
        if (rc <= 0)
            return rc;
        l = live_span(pr);
    }
    *live = l;
    return 1;
}

bool ioxd__pipereader_inject(ioxd_pipereader *pr, const void *data, size_t n)
{
    if (pr->error)
        return false;
    if (!pr->live_in_buf) {
        if (pr->has_cur) {
            if (!gather(pr))
                return false;
        } else {
            pr->buf_pos = pr->buf_end = pr->floor;
            pr->live_in_buf = true;
        }
    }
    if (pr->buf_end + n > pr->cap) {
        compact(pr);
        if (pr->buf_end + n > pr->cap)
            return false;
    }
    memcpy(pr->buf + pr->buf_end, data, n);
    pr->buf_end += n;
    return true;
}

void ioxd__pipewriter_init(ioxd_pipewriter *pw, conn_t *conn, char *buf, size_t lead, size_t cap, size_t slack)
{
    *pw       = (ioxd_pipewriter){};
    pw->conn  = conn;
    pw->buf   = buf;
    pw->lead  = lead;
    pw->cap   = cap;
    pw->slack = slack;
}

void ioxd__pipewriter_reset(ioxd_pipewriter *pw)
{
    pw->head = pw->len = pw->tail = 0;
}

int ioxd__pipewriter_flush_with(ioxd_pipewriter *pw, const void *data, size_t n)
{
    if (pw->failed)
        return -1;
    size_t total = pw->head + pw->len + pw->tail;
    int    rc    = 0;
    if (total && n) {
        struct iovec iov[2] = { { pw->buf + pw->lead - pw->head, total }, { (void *)(uintptr_t)data, n } };
        rc = ioxd__conn_sendv(pw->conn, iov, 2);
    } else if (total) {
        rc = ioxd__conn_send(pw->conn, pw->buf + pw->lead - pw->head, total);
    } else if (n) {
        rc = ioxd__conn_send(pw->conn, data, n);
    }
    pw->head = pw->len = pw->tail = 0;
    if (rc < 0) {
        pw->failed = true;
        return -1;
    }
    return 0;
}

int ioxd__pipewriter_flush(ioxd_pipewriter *pw)
{
    return ioxd__pipewriter_flush_with(pw, nullptr, 0);
}

void *ioxd__pipewriter_reserve(ioxd_pipewriter *pw, size_t n)
{
    if (pw->failed || n > pw->cap)
        return nullptr;
    if (pw->len + n > pw->cap && ioxd__pipewriter_flush(pw) < 0)
        return nullptr;
    return ioxd__pipewriter_at(pw);
}

void ioxd__pipewriter_advance(ioxd_pipewriter *pw, size_t n)
{
    size_t room = ioxd__pipewriter_room(pw);
    pw->len += n < room ? n : room;
}

char *ioxd__pipewriter_front(ioxd_pipewriter *pw, size_t n)
{
    if (n > pw->lead - pw->head)
        return nullptr;
    pw->head += n;
    return pw->buf + pw->lead - pw->head;
}

char *ioxd__pipewriter_back(ioxd_pipewriter *pw, size_t n)
{
    if (n > pw->slack - pw->tail)
        return nullptr;
    char *at = pw->buf + pw->lead + pw->len + pw->tail;
    pw->tail += n;
    return at;
}

int ioxd__pipewriter_through(ioxd_pipewriter *pw, const void *data, size_t n)
{
    if (pw->failed)
        return -1;
    if (ioxd__conn_send(pw->conn, data, n) < 0) {
        pw->failed = true;
        return -1;
    }
    return 0;
}

int ioxd__pipewriter_write(ioxd_pipewriter *pw, const void *data, size_t n)
{
    if (pw->failed)
        return -1;
    if (n > pw->cap)
        return ioxd__pipewriter_flush(pw) < 0 ? -1 : ioxd__pipewriter_through(pw, data, n);
    void *at = ioxd__pipewriter_reserve(pw, n);
    if (!at)
        return -1;
    memcpy(at, data, n);
    pw->len += n;
    return 0;
}

int ioxd__pipewriter_send(ioxd_pipewriter *pw, const void *data, size_t n)
{
    return ioxd__pipewriter_write(pw, data, n) < 0 ? -1 : ioxd__pipewriter_flush(pw);
}

void ioxd__pipe_init(ioxd_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t lead, size_t cap, size_t slack)
{
    ioxd__pipereader_init(&p->in, conn, gather, gather_cap);
    ioxd__pipewriter_init(&p->out, conn, slab, lead, cap, slack);
}

void ioxd__pipe_close(ioxd_pipe *p)
{
    ioxd__pipewriter_flush(&p->out);
    ioxd__pipereader_close(&p->in);
}

int         ioxd_pipe_read   (ioxd_pipe *p, ioxd_slice *live)          { return ioxd__pipereader_read(&p->in, live); }
void        ioxd_pipe_examine(ioxd_pipe *p, size_t n)                  { ioxd__pipereader_examine(&p->in, n); }
void        ioxd_pipe_drop   (ioxd_pipe *p, size_t n)                  { ioxd__pipereader_drop(&p->in, n); }
const char *ioxd_pipe_keep   (ioxd_pipe *p, size_t n)                  { return ioxd__pipereader_keep(&p->in, n); }
ioxd_slice  ioxd_pipe_kept   (ioxd_pipe *p)                            { return ioxd__pipereader_run(&p->in); }
void        ioxd_pipe_release(ioxd_pipe *p)                            { ioxd__pipereader_release(&p->in); }
int         ioxd_pipe_copy   (ioxd_pipe *p, void *dst, size_t n)       { return ioxd__pipereader_copy(&p->in, dst, n); }
void       *ioxd_pipe_reserve(ioxd_pipe *p, size_t n)                  { return ioxd__pipewriter_reserve(&p->out, n); }
void        ioxd_pipe_advance(ioxd_pipe *p, size_t n)                  { ioxd__pipewriter_advance(&p->out, n); }
int         ioxd_pipe_write  (ioxd_pipe *p, const void *data, size_t n) { return ioxd__pipewriter_write(&p->out, data, n); }
int         ioxd_pipe_flush  (ioxd_pipe *p)                            { return ioxd__pipewriter_flush(&p->out); }
int         ioxd_pipe_send   (ioxd_pipe *p, const void *data, size_t n) { return ioxd__pipewriter_send(&p->out, data, n); }

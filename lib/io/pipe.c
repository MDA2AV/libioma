/*
 * io/pipe.c - the reader and the writer of io/pipe.h, and the public pipe on top of them.
 */
#include "io/pipe.h"
#include "io/proactor.h"

#include <string.h>

/* ── the reader ────────────────────────────────────────────────────────────────────────── */

void ioxd_pipereader_init(ioxd_pipereader *pr, conn_t *conn, char *buf, size_t cap)
{
    *pr      = (ioxd_pipereader){};
    pr->conn = conn;
    pr->buf  = buf;
    pr->cap  = cap;
}

/* The live bytes: in buf, or in the current kernel buffer. */
static ioxd_slice live_span(const ioxd_pipereader *pr)
{
    if (pr->live_in_buf)
        return (ioxd_slice){ pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos };
    if (pr->has_cur)
        return (ioxd_slice){ (const char *)pr->cur.ptr + pr->cur_pos, pr->cur.len - pr->cur_pos };
    return (ioxd_slice){ nullptr, 0 };
}

/* Let the current kernel buffer go once nothing is left in it, neither live bytes nor the run.
 * One that also holds frozen kept bytes lives on as `pinned`. */
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

/* Reclaim the bytes dropped from the front of buf's live region. */
static void compact(ioxd_pipereader *pr)
{
    if (pr->buf_pos > pr->floor) {
        memmove(pr->buf + pr->floor, pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos);
        pr->buf_end -= pr->buf_pos - pr->floor;
        pr->buf_pos  = pr->floor;
    }
}

/* Move the current buffer's run and live bytes into buf, so what follows can join them. A run
 * that was kept in place has pointers out to it, so its buffer stays pinned rather than going
 * back to the ring - unless another buffer is pinned already (the HTTP engine's head), in which
 * case the run just moves and ioxd_pipereader_run is where to find it. */
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

/* A buffer that cannot be used: back to the ring, and the reader is done. */
static int refuse(ioxd_pipereader *pr, const struct rx_item *item)
{
    ioxd__bufring_return(&pr->conn->p->bufs, item->buf_id);
    pr->error = IOXD_PIPE_FULL;
    return IOXD_PIPE_FULL;
}

/* More bytes: the next kernel buffer, in place when nothing is live, else appended in buf. */
static int more(ioxd_pipereader *pr)
{
    if (pr->eof)
        return 0;
    struct rx_item item;
    int rc = ioxd__await_item(pr->conn, &item);
    if (rc <= 0) {
        pr->eof = true;
        if (rc < 0)
            pr->error = IOXD_PIPE_GONE;
        return rc < 0 ? IOXD_PIPE_GONE : 0;
    }
    if (!pr->live_in_buf && !pr->has_cur) {              /* nothing live: this buffer is the live span */
        pr->cur           = item;
        pr->has_cur       = true;
        pr->cur_pos       = 0;
        pr->cur_is_pinned = false;
        return 1;
    }
    if (!pr->live_in_buf && !gather(pr))                 /* the live bytes leave the current buffer first */
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

int ioxd_pipereader_read(ioxd_pipereader *pr, ioxd_slice *live)
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

void ioxd_pipereader_examine(ioxd_pipereader *pr, size_t n)
{
    pr->examined = n;
}

/* Forget n live bytes of the current place; never more than there are. */
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

void ioxd_pipereader_drop(ioxd_pipereader *pr, size_t n)
{
    consume(pr, n);
}

const char *ioxd_pipereader_keep(ioxd_pipereader *pr, size_t n)
{
    const char *kept;
    ioxd_slice  live = live_span(pr);
    if (n > live.len)
        return nullptr;                                 /* more than is live: the caller's mistake */
    if (n == 0)
        return live.p;
    if (pr->live_in_buf) {
        if (pr->run_len == 0) {                          /* a run starts where the live bytes are */
            pr->run_in_cur = false;
            pr->run_start  = pr->buf_pos;
            pr->floor      = pr->buf_pos;
        } else if (pr->buf_pos != pr->floor) {            /* dropped bytes in between: slide these down */
            memmove(pr->buf + pr->floor, pr->buf + pr->buf_pos, n);
        }
        kept = pr->buf + pr->floor;
        pr->floor += n;
    } else if (pr->has_cur && (pr->run_len == 0 || pr->run_in_cur)) {   /* in place */
        if (pr->run_len == 0) {
            pr->run_in_cur = true;
            pr->run_start  = pr->cur_pos;
        }
        size_t run_end = pr->run_start + pr->run_len;
        if (run_end != pr->cur_pos)
            memmove((char *)pr->cur.ptr + run_end, (const char *)pr->cur.ptr + pr->cur_pos, n);
        kept = (const char *)pr->cur.ptr + run_end;
    } else if (pr->has_cur) {                            /* the run is in buf, the live bytes are not: copy across */
        if (pr->floor + n > pr->cap) {
            pr->error = IOXD_PIPE_FULL;
            return nullptr;
        }
        memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->cur_pos, n);
        kept = pr->buf + pr->floor;
        pr->floor += n;
        pr->buf_pos = pr->buf_end = pr->floor;          /* buf's (empty) live region moves up with it */
    } else {
        return nullptr;                                 /* nothing live: the caller's mistake */
    }
    pr->run_len += n;
    consume(pr, n);
    return kept;
}

void ioxd_pipereader_run_begin(ioxd_pipereader *pr)
{
    if (pr->run_in_cur && pr->run_len) {
        if (pr->has_pinned && !pr->cur_is_pinned) {      /* another buffer is pinned already: this run moves to buf */
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

ioxd_slice ioxd_pipereader_run(const ioxd_pipereader *pr)
{
    const char *base = pr->run_in_cur ? (const char *)pr->cur.ptr : pr->buf;
    return (ioxd_slice){ base + pr->run_start, pr->run_len };
}

void ioxd_pipereader_release(ioxd_pipereader *pr)
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
            pr->cur_is_pinned = false;                   /* it lives on as the current buffer */
        else
            ioxd__bufring_return(&pr->conn->p->bufs, pr->pinned.buf_id);
        pr->has_pinned = false;
    }
    cur_done(pr);
}

void ioxd_pipereader_close(ioxd_pipereader *pr)
{
    if (pr->has_cur && !pr->cur_is_pinned)
        ioxd__bufring_return(&pr->conn->p->bufs, pr->cur.buf_id);
    if (pr->has_pinned)
        ioxd__bufring_return(&pr->conn->p->bufs, pr->pinned.buf_id);
    pr->has_cur = pr->has_pinned = pr->cur_is_pinned = false;
}

int ioxd_pipereader_copy(ioxd_pipereader *pr, void *dst, size_t n)
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

int ioxd_pipereader_avail(ioxd_pipereader *pr, ioxd_slice *live)
{
    if (pr->error)
        return pr->error;
    ioxd_slice l = live_span(pr);
    while (l.len <= pr->examined) {                       /* all seen: take a delivered buffer, if one is queued */
        if (pr->conn->rx_head == pr->conn->rx_tail)
            return 0;
        int rc = more(pr);
        if (rc <= 0)
            return rc;
        l = live_span(pr);
    }
    *live = l;
    return 1;
}

bool ioxd_pipereader_inject(ioxd_pipereader *pr, const void *data, size_t n)
{
    if (pr->error)
        return false;
    if (!pr->live_in_buf) {
        if (pr->has_cur) {                                /* live bytes, or a run, in place: they move first */
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

/* ── the writer ────────────────────────────────────────────────────────────────────────── */

void ioxd_pipewriter_init(ioxd_pipewriter *pw, conn_t *conn, char *buf, size_t lead, size_t cap, size_t slack)
{
    *pw       = (ioxd_pipewriter){};
    pw->conn  = conn;
    pw->buf   = buf;
    pw->lead  = lead;
    pw->cap   = cap;
    pw->slack = slack;
}

void ioxd_pipewriter_reset(ioxd_pipewriter *pw)
{
    pw->head = pw->len = pw->tail = 0;
}

int ioxd_pipewriter_flush(ioxd_pipewriter *pw)
{
    if (pw->failed)
        return -1;
    size_t total = pw->head + pw->len + pw->tail;
    if (total == 0)
        return 0;
    int rc = await_send(pw->conn, pw->buf + pw->lead - pw->head, total);
    pw->head = pw->len = pw->tail = 0;
    if (rc < 0) {
        pw->failed = true;
        return -1;
    }
    return 0;
}

void *ioxd_pipewriter_reserve(ioxd_pipewriter *pw, size_t n)
{
    if (pw->failed || n > pw->cap)
        return nullptr;
    if (pw->len + n > pw->cap && ioxd_pipewriter_flush(pw) < 0)
        return nullptr;
    return ioxd_pipewriter_at(pw);
}

void ioxd_pipewriter_advance(ioxd_pipewriter *pw, size_t n)
{
    size_t room = ioxd_pipewriter_room(pw);
    pw->len += n < room ? n : room;                     /* never past the slab, whatever was claimed */
}

char *ioxd_pipewriter_front(ioxd_pipewriter *pw, size_t n)
{
    if (n > pw->lead - pw->head)
        return nullptr;
    pw->head += n;
    return pw->buf + pw->lead - pw->head;
}

char *ioxd_pipewriter_back(ioxd_pipewriter *pw, size_t n)
{
    if (n > pw->slack - pw->tail)
        return nullptr;
    char *at = pw->buf + pw->lead + pw->len + pw->tail;
    pw->tail += n;
    return at;
}

int ioxd_pipewriter_through(ioxd_pipewriter *pw, const void *data, size_t n)
{
    if (pw->failed)
        return -1;
    if (await_send(pw->conn, data, n) < 0) {
        pw->failed = true;
        return -1;
    }
    return 0;
}

int ioxd_pipewriter_write(ioxd_pipewriter *pw, const void *data, size_t n)
{
    if (pw->failed)
        return -1;
    if (n > pw->cap)                                    /* larger than the slab: straight from the caller's memory */
        return ioxd_pipewriter_flush(pw) < 0 ? -1 : ioxd_pipewriter_through(pw, data, n);
    void *at = ioxd_pipewriter_reserve(pw, n);
    if (!at)
        return -1;
    memcpy(at, data, n);
    pw->len += n;
    return 0;
}

int ioxd_pipewriter_send(ioxd_pipewriter *pw, const void *data, size_t n)
{
    return ioxd_pipewriter_write(pw, data, n) < 0 ? -1 : ioxd_pipewriter_flush(pw);
}

/* ── the pipe ──────────────────────────────────────────────────────────────────────────── */

void ioxd__pipe_init(struct ioxd_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t lead, size_t cap, size_t slack)
{
    ioxd_pipereader_init(&p->in, conn, gather, gather_cap);
    ioxd_pipewriter_init(&p->out, conn, slab, lead, cap, slack);
}

void ioxd__pipe_close(struct ioxd_pipe *p)
{
    ioxd_pipewriter_flush(&p->out);                     /* what a handler left in the slab still goes */
    ioxd_pipereader_close(&p->in);
}

int         ioxd_pipe_read   (ioxd_pipe *p, ioxd_slice *live)          { return ioxd_pipereader_read(&p->in, live); }
void        ioxd_pipe_examine(ioxd_pipe *p, size_t n)                  { ioxd_pipereader_examine(&p->in, n); }
void        ioxd_pipe_drop   (ioxd_pipe *p, size_t n)                  { ioxd_pipereader_drop(&p->in, n); }
const char *ioxd_pipe_keep   (ioxd_pipe *p, size_t n)                  { return ioxd_pipereader_keep(&p->in, n); }
ioxd_slice  ioxd_pipe_kept   (ioxd_pipe *p)                            { return ioxd_pipereader_run(&p->in); }
void        ioxd_pipe_release(ioxd_pipe *p)                            { ioxd_pipereader_release(&p->in); }
int         ioxd_pipe_copy   (ioxd_pipe *p, void *dst, size_t n)       { return ioxd_pipereader_copy(&p->in, dst, n); }
void       *ioxd_pipe_reserve(ioxd_pipe *p, size_t n)                  { return ioxd_pipewriter_reserve(&p->out, n); }
void        ioxd_pipe_advance(ioxd_pipe *p, size_t n)                  { ioxd_pipewriter_advance(&p->out, n); }
int         ioxd_pipe_write  (ioxd_pipe *p, const void *data, size_t n) { return ioxd_pipewriter_write(&p->out, data, n); }
int         ioxd_pipe_flush  (ioxd_pipe *p)                            { return ioxd_pipewriter_flush(&p->out); }
int         ioxd_pipe_send   (ioxd_pipe *p, const void *data, size_t n) { return ioxd_pipewriter_send(&p->out, data, n); }

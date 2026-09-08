/*
 * io/pipe.c - the reader and the writer of io/pipe.h, and the public pipe on top of them.
 */
#include "io/pipe.h"
#include "io/internal.h"

#include <string.h>

/* ── the reader ────────────────────────────────────────────────────────────────────────── */

void ioma_pipereader_init(ioma_pipereader *pr, conn_t *conn, char *buf, size_t cap)
{
    *pr      = (ioma_pipereader){};
    pr->conn = conn;
    pr->buf  = buf;
    pr->cap  = cap;
}

/* The live bytes: in buf, or in the current kernel buffer. */
static ioma_slice live_span(const ioma_pipereader *pr)
{
    if (pr->live_in_buf)
        return (ioma_slice){ pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos };
    if (pr->has_cur)
        return (ioma_slice){ (const char *)pr->cur.ptr + pr->cur_pos, pr->cur.len - pr->cur_pos };
    return (ioma_slice){ nullptr, 0 };
}

/* Let the current kernel buffer go once nothing is left in it, neither live bytes nor the run.
 * One that also holds frozen kept bytes lives on as `pinned`. */
static void cur_done(ioma_pipereader *pr)
{
    if (!pr->has_cur || pr->cur_pos < pr->cur.len || (pr->run_in_cur && pr->run_len))
        return;
    if (!pr->cur_is_pinned)
        ioma__bufring_return(&pr->conn->p->bufs, pr->cur.buf_id);
    pr->has_cur       = false;
    pr->cur_is_pinned = false;
    pr->cur_pos       = 0;
}

/* Reclaim the bytes dropped from the front of buf's live region. */
static void compact(ioma_pipereader *pr)
{
    if (pr->buf_pos > pr->floor) {
        memmove(pr->buf + pr->floor, pr->buf + pr->buf_pos, pr->buf_end - pr->buf_pos);
        pr->buf_end -= pr->buf_pos - pr->floor;
        pr->buf_pos  = pr->floor;
    }
}

/* Move the current buffer's run and live bytes into buf, so what follows can join them. */
static bool gather(ioma_pipereader *pr)
{
    size_t live = pr->has_cur ? pr->cur.len - pr->cur_pos : 0;
    size_t run  = pr->run_in_cur ? pr->run_len : 0;
    if (pr->floor + run + live > pr->cap)
        return false;
    if (run) {
        memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->run_start, run);
        pr->run_start = pr->floor;
        pr->floor    += run;
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
static int refuse(ioma_pipereader *pr, const struct rx_item *item)
{
    ioma__bufring_return(&pr->conn->p->bufs, item->buf_id);
    pr->error = IOMA_PIPE_FULL;
    return IOMA_PIPE_FULL;
}

/* More bytes: the next kernel buffer, in place when nothing is live, else appended in buf. */
static int more(ioma_pipereader *pr)
{
    if (pr->eof)
        return 0;
    struct rx_item item;
    int rc = ioma__await_item(pr->conn, &item);
    if (rc <= 0) {
        pr->eof = true;
        if (rc < 0)
            pr->error = IOMA_PIPE_GONE;
        return rc < 0 ? IOMA_PIPE_GONE : 0;
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
    ioma__bufring_return(&pr->conn->p->bufs, item.buf_id);
    return 1;
}

int ioma_pipereader_read(ioma_pipereader *pr, ioma_slice *live)
{
    if (pr->error)
        return pr->error;
    for (;;) {
        ioma_slice l = live_span(pr);
        if (l.len > pr->examined) {
            *live = l;
            return 1;
        }
        int rc = more(pr);
        if (rc <= 0)
            return rc;
    }
}

void ioma_pipereader_examine(ioma_pipereader *pr, size_t n)
{
    pr->examined = n;
}

/* Forget n live bytes of the current place. */
static void consume(ioma_pipereader *pr, size_t n)
{
    if (pr->live_in_buf) {
        pr->buf_pos += n;
        if (pr->buf_pos == pr->buf_end) {
            pr->buf_pos = pr->buf_end = pr->floor;
            pr->live_in_buf = false;
        }
    } else if (pr->has_cur) {
        pr->cur_pos += n;
        cur_done(pr);
    }
    pr->examined = pr->examined > n ? pr->examined - n : 0;
}

void ioma_pipereader_drop(ioma_pipereader *pr, size_t n)
{
    consume(pr, n);
}

const char *ioma_pipereader_keep(ioma_pipereader *pr, size_t n)
{
    const char *kept;
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
            pr->error = IOMA_PIPE_FULL;
            return nullptr;
        }
        memcpy(pr->buf + pr->floor, (const char *)pr->cur.ptr + pr->cur_pos, n);
        kept = pr->buf + pr->floor;
        pr->floor += n;
    } else {
        return nullptr;                                 /* nothing live: the caller's mistake */
    }
    pr->run_len += n;
    consume(pr, n);
    return kept;
}

void ioma_pipereader_run_begin(ioma_pipereader *pr)
{
    if (pr->run_in_cur && pr->run_len) {
        if (pr->has_pinned && !pr->cur_is_pinned) {      /* another buffer is pinned already: this run moves to buf */
            if (pr->floor + pr->run_len > pr->cap) {
                pr->error = IOMA_PIPE_FULL;
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

ioma_slice ioma_pipereader_run(const ioma_pipereader *pr)
{
    const char *base = pr->run_in_cur ? (const char *)pr->cur.ptr : pr->buf;
    return (ioma_slice){ base + pr->run_start, pr->run_len };
}

void ioma_pipereader_release(ioma_pipereader *pr)
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
            ioma__bufring_return(&pr->conn->p->bufs, pr->pinned.buf_id);
        pr->has_pinned = false;
    }
    cur_done(pr);
}

void ioma_pipereader_close(ioma_pipereader *pr)
{
    if (pr->has_cur && !pr->cur_is_pinned)
        ioma__bufring_return(&pr->conn->p->bufs, pr->cur.buf_id);
    if (pr->has_pinned)
        ioma__bufring_return(&pr->conn->p->bufs, pr->pinned.buf_id);
    pr->has_cur = pr->has_pinned = pr->cur_is_pinned = false;
}

int ioma_pipereader_copy(ioma_pipereader *pr, void *dst, size_t n)
{
    if (pr->error)
        return pr->error;
    for (;;) {
        ioma_slice l = live_span(pr);
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

/* ── the writer ────────────────────────────────────────────────────────────────────────── */

void ioma_pipewriter_init(ioma_pipewriter *pw, conn_t *conn, char *buf, size_t cap)
{
    *pw      = (ioma_pipewriter){};
    pw->conn = conn;
    pw->buf  = buf;
    pw->cap  = cap;
}

int ioma_pipewriter_flush(ioma_pipewriter *pw)
{
    if (pw->failed)
        return -1;
    if (pw->len == 0)
        return 0;
    int rc = await_send(pw->conn, pw->buf, pw->len);
    pw->len = 0;
    if (rc < 0) {
        pw->failed = true;
        return -1;
    }
    return 0;
}

void *ioma_pipewriter_reserve(ioma_pipewriter *pw, size_t n)
{
    if (pw->failed || n > pw->cap)
        return nullptr;
    if (pw->len + n > pw->cap && ioma_pipewriter_flush(pw) < 0)
        return nullptr;
    return pw->buf + pw->len;
}

void ioma_pipewriter_advance(ioma_pipewriter *pw, size_t n)
{
    pw->len += n;
}

int ioma_pipewriter_write(ioma_pipewriter *pw, const void *data, size_t n)
{
    if (pw->failed)
        return -1;
    if (n > pw->cap) {                                   /* larger than the slab: straight from the caller's memory */
        if (ioma_pipewriter_flush(pw) < 0)
            return -1;
        if (await_send(pw->conn, data, n) < 0) {
            pw->failed = true;
            return -1;
        }
        return 0;
    }
    void *at = ioma_pipewriter_reserve(pw, n);
    if (!at)
        return -1;
    memcpy(at, data, n);
    pw->len += n;
    return 0;
}

int ioma_pipewriter_send(ioma_pipewriter *pw, const void *data, size_t n)
{
    return ioma_pipewriter_write(pw, data, n) < 0 ? -1 : ioma_pipewriter_flush(pw);
}

/* ── the pipe ──────────────────────────────────────────────────────────────────────────── */

void ioma__pipe_init(struct ioma_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t slab_cap)
{
    ioma_pipereader_init(&p->in, conn, gather, gather_cap);
    ioma_pipewriter_init(&p->out, conn, slab, slab_cap);
}

void ioma__pipe_close(struct ioma_pipe *p)
{
    ioma_pipereader_close(&p->in);
}

int         ioma_pipe_read   (ioma_pipe *p, ioma_slice *live)          { return ioma_pipereader_read(&p->in, live); }
void        ioma_pipe_examine(ioma_pipe *p, size_t n)                  { ioma_pipereader_examine(&p->in, n); }
void        ioma_pipe_drop   (ioma_pipe *p, size_t n)                  { ioma_pipereader_drop(&p->in, n); }
const char *ioma_pipe_keep   (ioma_pipe *p, size_t n)                  { return ioma_pipereader_keep(&p->in, n); }
void        ioma_pipe_release(ioma_pipe *p)                            { ioma_pipereader_release(&p->in); }
int         ioma_pipe_copy   (ioma_pipe *p, void *dst, size_t n)       { return ioma_pipereader_copy(&p->in, dst, n); }
void       *ioma_pipe_reserve(ioma_pipe *p, size_t n)                  { return ioma_pipewriter_reserve(&p->out, n); }
void        ioma_pipe_advance(ioma_pipe *p, size_t n)                  { ioma_pipewriter_advance(&p->out, n); }
int         ioma_pipe_write  (ioma_pipe *p, const void *data, size_t n) { return ioma_pipewriter_write(&p->out, data, n); }
int         ioma_pipe_flush  (ioma_pipe *p)                            { return ioma_pipewriter_flush(&p->out); }
int         ioma_pipe_send   (ioma_pipe *p, const void *data, size_t n) { return ioma_pipewriter_send(&p->out, data, n); }

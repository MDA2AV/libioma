/*
 * io/pipe.c - the reader and the writer of io/pipe.h, and the public pipe on top of them.
 */
#include "io/pipe.h"
#include "io/internal.h"

#include <string.h>

/* ── the reader ────────────────────────────────────────────────────────────────────────── */

void ioma_reader_init(ioma_reader *r, conn_t *conn, char *buf, size_t cap)
{
    *r      = (ioma_reader){};
    r->conn = conn;
    r->buf  = buf;
    r->cap  = cap;
}

/* The live bytes: in buf, or in the current kernel buffer. */
static ioma_slice live_span(const ioma_reader *r)
{
    if (r->live_in_buf)
        return (ioma_slice){ r->buf + r->buf_pos, r->buf_end - r->buf_pos };
    if (r->has_cur)
        return (ioma_slice){ (const char *)r->cur.ptr + r->cur_pos, r->cur.len - r->cur_pos };
    return (ioma_slice){ nullptr, 0 };
}

/* Let the current kernel buffer go once nothing is left in it, neither live bytes nor the run.
 * One that also holds frozen kept bytes lives on as `pinned`. */
static void cur_done(ioma_reader *r)
{
    if (!r->has_cur || r->cur_pos < r->cur.len || (r->run_in_cur && r->run_len))
        return;
    if (!r->cur_is_pinned)
        ioma__return_buf(r->conn->p, r->cur.buf_id);
    r->has_cur       = false;
    r->cur_is_pinned = false;
    r->cur_pos       = 0;
}

/* Reclaim the bytes dropped from the front of buf's live region. */
static void compact(ioma_reader *r)
{
    if (r->buf_pos > r->floor) {
        memmove(r->buf + r->floor, r->buf + r->buf_pos, r->buf_end - r->buf_pos);
        r->buf_end -= r->buf_pos - r->floor;
        r->buf_pos  = r->floor;
    }
}

/* Move the current buffer's run and live bytes into buf, so what follows can join them. */
static bool gather(ioma_reader *r)
{
    size_t live = r->has_cur ? r->cur.len - r->cur_pos : 0;
    size_t run  = r->run_in_cur ? r->run_len : 0;
    if (r->floor + run + live > r->cap)
        return false;
    if (run) {
        memcpy(r->buf + r->floor, (const char *)r->cur.ptr + r->run_start, run);
        r->run_start = r->floor;
        r->floor    += run;
    }
    r->run_in_cur = false;
    if (live)
        memcpy(r->buf + r->floor, (const char *)r->cur.ptr + r->cur_pos, live);
    r->buf_pos     = r->floor;
    r->buf_end     = r->floor + live;
    r->live_in_buf = true;
    if (r->has_cur) {
        r->cur_pos = r->cur.len;
        cur_done(r);
    }
    return true;
}

/* A buffer that cannot be used: back to the ring, and the reader is done. */
static int refuse(ioma_reader *r, const struct rx_item *item)
{
    ioma__return_buf(r->conn->p, item->buf_id);
    r->error = IOMA_PIPE_FULL;
    return IOMA_PIPE_FULL;
}

/* More bytes: the next kernel buffer, in place when nothing is live, else appended in buf. */
static int more(ioma_reader *r)
{
    if (r->eof)
        return 0;
    struct rx_item item;
    int rc = ioma__await_item(r->conn, &item);
    if (rc <= 0) {
        r->eof = true;
        if (rc < 0)
            r->error = IOMA_PIPE_GONE;
        return rc < 0 ? IOMA_PIPE_GONE : 0;
    }
    if (!r->live_in_buf && !r->has_cur) {              /* nothing live: this buffer is the live span */
        r->cur           = item;
        r->has_cur       = true;
        r->cur_pos       = 0;
        r->cur_is_pinned = false;
        return 1;
    }
    if (!r->live_in_buf && !gather(r))                 /* the live bytes leave the current buffer first */
        return refuse(r, &item);
    if (r->buf_end + item.len > r->cap) {
        compact(r);
        if (r->buf_end + item.len > r->cap)
            return refuse(r, &item);
    }
    memcpy(r->buf + r->buf_end, item.ptr, item.len);
    r->buf_end += item.len;
    ioma__return_buf(r->conn->p, item.buf_id);
    return 1;
}

int ioma_reader_read(ioma_reader *r, ioma_slice *live)
{
    if (r->error)
        return r->error;
    for (;;) {
        ioma_slice l = live_span(r);
        if (l.len > r->examined) {
            *live = l;
            return 1;
        }
        int rc = more(r);
        if (rc <= 0)
            return rc;
    }
}

void ioma_reader_examine(ioma_reader *r, size_t n)
{
    r->examined = n;
}

/* Forget n live bytes of the current place. */
static void consume(ioma_reader *r, size_t n)
{
    if (r->live_in_buf) {
        r->buf_pos += n;
        if (r->buf_pos == r->buf_end) {
            r->buf_pos = r->buf_end = r->floor;
            r->live_in_buf = false;
        }
    } else if (r->has_cur) {
        r->cur_pos += n;
        cur_done(r);
    }
    r->examined = r->examined > n ? r->examined - n : 0;
}

void ioma_reader_drop(ioma_reader *r, size_t n)
{
    consume(r, n);
}

const char *ioma_reader_keep(ioma_reader *r, size_t n)
{
    const char *kept;
    if (r->live_in_buf) {
        if (r->run_len == 0) {                          /* a run starts where the live bytes are */
            r->run_in_cur = false;
            r->run_start  = r->buf_pos;
            r->floor      = r->buf_pos;
        } else if (r->buf_pos != r->floor) {            /* dropped bytes in between: slide these down */
            memmove(r->buf + r->floor, r->buf + r->buf_pos, n);
        }
        kept = r->buf + r->floor;
        r->floor += n;
    } else if (r->has_cur && (r->run_len == 0 || r->run_in_cur)) {   /* in place */
        if (r->run_len == 0) {
            r->run_in_cur = true;
            r->run_start  = r->cur_pos;
        }
        size_t run_end = r->run_start + r->run_len;
        if (run_end != r->cur_pos)
            memmove((char *)r->cur.ptr + run_end, (const char *)r->cur.ptr + r->cur_pos, n);
        kept = (const char *)r->cur.ptr + run_end;
    } else if (r->has_cur) {                            /* the run is in buf, the live bytes are not: copy across */
        if (r->floor + n > r->cap) {
            r->error = IOMA_PIPE_FULL;
            return nullptr;
        }
        memcpy(r->buf + r->floor, (const char *)r->cur.ptr + r->cur_pos, n);
        kept = r->buf + r->floor;
        r->floor += n;
    } else {
        return nullptr;                                 /* nothing live: the caller's mistake */
    }
    r->run_len += n;
    consume(r, n);
    return kept;
}

void ioma_reader_run_begin(ioma_reader *r)
{
    if (r->run_in_cur && r->run_len) {
        if (r->has_pinned && !r->cur_is_pinned) {      /* another buffer is pinned already: this run moves to buf */
            if (r->floor + r->run_len > r->cap) {
                r->error = IOMA_PIPE_FULL;
            } else {
                memcpy(r->buf + r->floor, (const char *)r->cur.ptr + r->run_start, r->run_len);
                r->floor += r->run_len;
            }
        } else {
            r->pinned        = r->cur;
            r->has_pinned    = true;
            r->cur_is_pinned = true;
        }
    }
    r->run_len    = 0;
    r->run_in_cur = false;
    cur_done(r);
}

ioma_slice ioma_reader_run(const ioma_reader *r)
{
    const char *base = r->run_in_cur ? (const char *)r->cur.ptr : r->buf;
    return (ioma_slice){ base + r->run_start, r->run_len };
}

void ioma_reader_release(ioma_reader *r)
{
    r->run_len    = 0;
    r->run_in_cur = false;
    if (r->live_in_buf) {
        memmove(r->buf, r->buf + r->buf_pos, r->buf_end - r->buf_pos);
        r->buf_end -= r->buf_pos;
        r->buf_pos  = 0;
    } else {
        r->buf_pos = r->buf_end = 0;
    }
    r->floor = 0;
    if (r->has_pinned) {
        if (r->cur_is_pinned)
            r->cur_is_pinned = false;                   /* it lives on as the current buffer */
        else
            ioma__return_buf(r->conn->p, r->pinned.buf_id);
        r->has_pinned = false;
    }
    cur_done(r);
}

void ioma_reader_close(ioma_reader *r)
{
    if (r->has_cur && !r->cur_is_pinned)
        ioma__return_buf(r->conn->p, r->cur.buf_id);
    if (r->has_pinned)
        ioma__return_buf(r->conn->p, r->pinned.buf_id);
    r->has_cur = r->has_pinned = r->cur_is_pinned = false;
}

int ioma_reader_copy(ioma_reader *r, void *dst, size_t n)
{
    if (r->error)
        return r->error;
    for (;;) {
        ioma_slice l = live_span(r);
        if (l.len) {
            size_t k = l.len < n ? l.len : n;
            memcpy(dst, l.p, k);
            consume(r, k);
            return (int)k;
        }
        int rc = more(r);
        if (rc <= 0)
            return rc;
    }
}

/* ── the writer ────────────────────────────────────────────────────────────────────────── */

void ioma_writer_init(ioma_writer *w, conn_t *conn, char *buf, size_t cap)
{
    *w      = (ioma_writer){};
    w->conn = conn;
    w->buf  = buf;
    w->cap  = cap;
}

int ioma_writer_flush(ioma_writer *w)
{
    if (w->failed)
        return -1;
    if (w->len == 0)
        return 0;
    int rc = await_send(w->conn, w->buf, w->len);
    w->len = 0;
    if (rc < 0) {
        w->failed = true;
        return -1;
    }
    return 0;
}

void *ioma_writer_reserve(ioma_writer *w, size_t n)
{
    if (w->failed || n > w->cap)
        return nullptr;
    if (w->len + n > w->cap && ioma_writer_flush(w) < 0)
        return nullptr;
    return w->buf + w->len;
}

void ioma_writer_advance(ioma_writer *w, size_t n)
{
    w->len += n;
}

int ioma_writer_write(ioma_writer *w, const void *data, size_t n)
{
    if (w->failed)
        return -1;
    if (n > w->cap) {                                   /* larger than the slab: straight from the caller's memory */
        if (ioma_writer_flush(w) < 0)
            return -1;
        if (await_send(w->conn, data, n) < 0) {
            w->failed = true;
            return -1;
        }
        return 0;
    }
    void *at = ioma_writer_reserve(w, n);
    if (!at)
        return -1;
    memcpy(at, data, n);
    w->len += n;
    return 0;
}

int ioma_writer_send(ioma_writer *w, const void *data, size_t n)
{
    return ioma_writer_write(w, data, n) < 0 ? -1 : ioma_writer_flush(w);
}

/* ── the pipe ──────────────────────────────────────────────────────────────────────────── */

void ioma__pipe_init(struct ioma_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t slab_cap)
{
    ioma_reader_init(&p->in, conn, gather, gather_cap);
    ioma_writer_init(&p->out, conn, slab, slab_cap);
}

void ioma__pipe_close(struct ioma_pipe *p)
{
    ioma_reader_close(&p->in);
}

int         ioma_pipe_read   (ioma_pipe *p, ioma_slice *live)          { return ioma_reader_read(&p->in, live); }
void        ioma_pipe_examine(ioma_pipe *p, size_t n)                  { ioma_reader_examine(&p->in, n); }
void        ioma_pipe_drop   (ioma_pipe *p, size_t n)                  { ioma_reader_drop(&p->in, n); }
const char *ioma_pipe_keep   (ioma_pipe *p, size_t n)                  { return ioma_reader_keep(&p->in, n); }
void        ioma_pipe_release(ioma_pipe *p)                            { ioma_reader_release(&p->in); }
int         ioma_pipe_copy   (ioma_pipe *p, void *dst, size_t n)       { return ioma_reader_copy(&p->in, dst, n); }
void       *ioma_pipe_reserve(ioma_pipe *p, size_t n)                  { return ioma_writer_reserve(&p->out, n); }
void        ioma_pipe_advance(ioma_pipe *p, size_t n)                  { ioma_writer_advance(&p->out, n); }
int         ioma_pipe_write  (ioma_pipe *p, const void *data, size_t n) { return ioma_writer_write(&p->out, data, n); }
int         ioma_pipe_flush  (ioma_pipe *p)                            { return ioma_writer_flush(&p->out); }
int         ioma_pipe_send   (ioma_pipe *p, const void *data, size_t n) { return ioma_writer_send(&p->out, data, n); }

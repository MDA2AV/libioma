/*
 * conn.c - one connection's life on its worker: the pooled conn_t, its two owners' refcount, the
 * multishot recv and the queue of slices it delivers, parking on -ENOBUFS, closing, and the two
 * awaits a handler coroutine calls.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── the object ────────────────────────────────────────────────────────────────────────── */

/* Take a conn_t from the pool (or calloc one) and reset it for a fresh fd. Two owners hold it:
 * the handler coroutine and the multishot recv, so refs starts at 2. */
conn_t *ioma__conn_new(proactor_t *p, int fd)
{
    conn_t *c = p->conn_free;
    if (c) {
        p->conn_free = c->pool_next;
        p->conn_free_count--;
    } else {
        c = calloc(1, sizeof *c);
        if (!c) {
            perror("calloc");
            abort();
        }
    }
    c->fd        = fd;
    c->p         = p;
    c->waiter    = NULL;
    c->rx_head   = 0;
    c->rx_tail   = 0;
    c->recv      = RECV_ARMED;
    c->refs      = 2;
    c->closed    = false;
    c->eof       = false;
    c->err       = 0;
    c->pool_next = NULL;
    p->live++;
    return c;
}

/* Drop one owner's ref. At zero the conn holds nothing (fd closed, buffers returned) and goes
 * back to the pool, or is freed past the cap. */
static void conn_unref(conn_t *c)
{
    if (--c->refs != 0)
        return;
    proactor_t *p = c->p;
    p->live--;
    if (p->conn_free_count < CONN_POOL_MAX) {
        c->pool_next = p->conn_free;
        p->conn_free = c;
        p->conn_free_count++;
    } else {
        free(c);
    }
}

/* Free the pool at worker teardown. */
void ioma__conn_pool_drain(proactor_t *p)
{
    while (p->conn_free) {
        conn_t *c = p->conn_free;
        p->conn_free = c->pool_next;
        free(c);
    }
    p->conn_free_count = 0;
}

/* ── the recv side ─────────────────────────────────────────────────────────────────────── */

/* Arm the multishot recv: one SQE, then a CQE per arrival, each in a buffer the kernel picks. */
void ioma__arm_recv(proactor_t *p, conn_t *c)
{
    struct io_uring_sqe *sqe = get_sqe(p);
    sqe->opcode    = IORING_OP_RECV;
    sqe->fd        = c->fd;
    sqe->flags     = IOSQE_BUFFER_SELECT | (p->ring.fixed_files ? IOSQE_FIXED_FILE : 0);
    sqe->ioprio    = IORING_RECV_MULTISHOT;
    sqe->buf_group = BGID;
    sqe->user_data = UD(c, TAG_RECV);
    c->recv = RECV_ARMED;
}

/* Resume the coroutine parked in await_recv, if there is one. It pops the queue itself. */
static void wake_reader(conn_t *c)
{
    coro_t *w = c->waiter;
    if (w) {
        c->waiter = NULL;
        coro_resume(w);
    }
}

/* Park a connection whose recv ended on -ENOBUFS until a buffer comes back. */
static void starved_push(proactor_t *p, conn_t *c)
{
    if (p->nstarved == p->cap_starved) {
        p->cap_starved = p->cap_starved ? p->cap_starved * 2 : 64;
        p->starved = realloc(p->starved, p->cap_starved * sizeof *p->starved);
        if (!p->starved) {
            perror("realloc");
            abort();
        }
    }
    p->starved[p->nstarved++] = c;
}

/* Forget a parked connection (it closed before any buffer came back). */
static void starved_remove(proactor_t *p, conn_t *c)
{
    for (unsigned i = 0; i < p->nstarved; i++) {
        if (p->starved[i] == c) {
            p->starved[i] = p->starved[--p->nstarved];
            return;
        }
    }
}

/* A recv CQE: queue the data and wake the reader, or record the end of input and drop the recv's
 * ref. -ENOBUFS is not an error: the buffer group ran dry, so park and re-arm later. */
void ioma__on_recv(proactor_t *p, conn_t *c, int res, unsigned flags)
{
    bool     more    = flags & IORING_CQE_F_MORE;
    bool     has_buf = flags & IORING_CQE_F_BUFFER;
    uint16_t bid     = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);

    trace("[w%d] recv fd=%d res=%d more=%d buf=%d bid=%u queued=%u state=%d closed=%d eof=%d\n",
          p->id, c->fd, res, more, has_buf, bid, c->rx_tail - c->rx_head, c->recv, c->closed, c->eof);

    if (res == -ENOBUFS) {
        if (c->closed) {
            c->recv = RECV_DONE;
            conn_unref(c);
            return;
        }
        c->recv = RECV_STARVED;
        starved_push(p, c);
        return;
    }

    if (res <= 0) {                                  /* peer FIN (0), an error, or our own cancel */
        if (has_buf)
            return_buf(p, bid);
        if (!c->eof) {
            c->eof = true;
            c->err = res;
        }
        c->recv = RECV_DONE;
        wake_reader(c);                              /* a parked await_recv returns 0 / -errno   */
        conn_unref(c);                               /* the recv's ref; may recycle c            */
        return;
    }

    if (c->closed) {
        return_buf(p, bid);                          /* the handler is gone; nobody will read it */
    } else if (c->rx_tail - c->rx_head == RX_QUEUE) {
        /* The handler is not draining. Rather than let one peer hoard the buffer group, end its
         * input: the next read sees -ENOBUFS. */
        return_buf(p, bid);
        if (!c->eof) {
            c->eof = true;
            c->err = -ENOBUFS;
        }
        if (more)
            submit_cancel(p, UD(c, TAG_RECV));
        wake_reader(c);
    } else {
        struct rx_item *it = &c->rx[c->rx_tail++ & RX_MASK];
        it->ptr = p->slab + (size_t)bid * BUF_SIZE;
        it->len = (uint32_t)res;
        it->bid = bid;
        wake_reader(c);
    }

    if (!more) {                                     /* the kernel ended the multishot: re-arm    */
        if (!c->closed && !c->eof)
            ioma__arm_recv(p, c);
        else {
            c->recv = RECV_DONE;
            conn_unref(c);
        }
    }
}

/* ── close ─────────────────────────────────────────────────────────────────────────────── */

/* Close the socket: a plain close, or under fixed files a CLOSE SQE on its slot, which rides the
 * next enter with the rest of the batch instead of costing a syscall here. */
static void close_socket(proactor_t *p, int fd)
{
    if (!p->ring.fixed_files) {
        close(fd);
        return;
    }
    struct io_uring_sqe *sqe = get_sqe(p);
    sqe->opcode     = IORING_OP_CLOSE;
    sqe->file_index = (uint32_t)fd + 1;              /* slot + 1; 0 would mean "a real fd" */
    sqe->user_data  = TAG_IGNORE;
}

/* Runs once when the handler returns: cancel the recv, hand unread buffers back, close the fd,
 * drop the handler's ref. The recv's own ref drops on its terminal CQE. */
static void conn_close(conn_t *c)
{
    proactor_t *p = c->p;
    c->closed = true;
    trace("[w%d] close fd=%d state=%d eof=%d err=%d queued=%u\n",
          p->id, c->fd, c->recv, c->eof, c->err, c->rx_tail - c->rx_head);

    if (c->recv == RECV_ARMED) {
        submit_cancel(p, UD(c, TAG_RECV));
    } else if (c->recv == RECV_STARVED) {
        starved_remove(p, c);
        c->recv = RECV_DONE;
        conn_unref(c);
    }
    while (c->rx_head != c->rx_tail)
        return_buf(p, c->rx[c->rx_head++ & RX_MASK].bid);

    close_socket(p, c->fd);
    conn_unref(c);
}

/* The connection's coroutine: run the worker's handler to completion, then close. */
void ioma__conn_main(void *arg)
{
    conn_t *c = arg;
    c->p->handler(c);
    conn_close(c);
}

/* ── awaits ────────────────────────────────────────────────────────────────────────────── */

/* Copy the next delivered slice into buf, parking while the queue is empty. Returns the byte
 * count, 0 when the peer closed, or -errno. A fully consumed buffer goes back to the ring. */
int await_recv(conn_t *c, void *buf, size_t len)
{
    if (len == 0)
        return -EINVAL;
    for (;;) {
        if (c->rx_head != c->rx_tail) {
            struct rx_item *it = &c->rx[c->rx_head & RX_MASK];
            size_t n = it->len < len ? it->len : len;
            memcpy(buf, it->ptr, n);
            it->ptr += n;
            it->len -= (uint32_t)n;
            if (it->len == 0) {
                return_buf(c->p, it->bid);
                c->rx_head++;
            }
            return (int)n;
        }
        if (c->eof)
            return c->err;
        c->waiter = coro_current();
        coro_yield();                                /* ioma__on_recv wakes us */
    }
}

/* Send all of buf: a SEND SQE per round, parked until its CQE. Returns len, or -errno. */
int await_send(conn_t *c, const void *buf, size_t len)
{
    const uint8_t *cur  = buf;
    size_t         left = len;
    while (left > 0) {
        op_t op;
        struct io_uring_sqe *sqe = get_sqe(c->p);
        sqe->opcode    = IORING_OP_SEND;
        sqe->fd        = c->fd;
        sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
        sqe->addr      = (uint64_t)(uintptr_t)cur;
        sqe->len       = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        sqe->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;  /* no SIGPIPE; the kernel finishes short sends */
        int n = await_op(sqe, &op);
        if (n < 0)
            return n;
        if (n == 0)
            return -EPIPE;
        cur  += n;
        left -= (size_t)n;
    }
    return (int)len;
}

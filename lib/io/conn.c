/*
 * conn.c - one connection's life on its worker: the pooled conn_t, its two owners' refcount, the
 * multishot recv and the queue of buffers it delivers, parking on -ENOBUFS, closing, and the
 * awaits the pipe is built on.
 */
#include "io/internal.h"
#include "io/pipe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Stage a one-shot op and park until its CQE. The loop fills op->res and resumes us. */
static int await_op(struct io_uring_sqe *sqe, op_t *op)
{
    op->waiter     = coro_current();
    sqe->user_data = UD(op, TAG_OP);
    coro_yield();
    return op->res;   /* NOLINT(clang-analyzer-core.uninitialized.UndefReturn): set by the loop before it resumed us */
}

/* Ask the kernel to cancel the op carrying that user_data. The acknowledgement CQE is ignored. */
static void submit_cancel(proactor_t *p, uint64_t target_user_data)
{
    struct io_uring_sqe *sqe = ioxd__sqe(p);
    sqe->opcode    = IORING_OP_ASYNC_CANCEL;
    sqe->fd        = -1;
    sqe->addr      = target_user_data;
    sqe->user_data = TAG_IGNORE;
}

/* Cancel the multishot recv, at most one cancel in flight: the queue-full policy would otherwise
 * stage another on every arrival while the queue stays full. Cleared on the terminal CQE. */
static void cancel_recv(proactor_t *p, conn_t *c)
{
    if (c->cancelling)
        return;
    c->cancelling = true;
    submit_cancel(p, UD(c, TAG_RECV));
}

/* End the input once: the first reason wins, so a cancel we asked for afterwards does not
 * overwrite the peer's FIN or the error that really ended it. */
static void end_input(conn_t *c, int err)
{
    if (!c->eof) {
        c->eof = true;
        c->err = err;
    }
}

/* ── the object ────────────────────────────────────────────────────────────────────────── */

/* Take a conn_t from the pool (or calloc one) and reset it for a fresh fd. Two owners hold it:
 * the handler coroutine and the multishot recv, so refs starts at 2. */
conn_t *ioxd__conn_new(proactor_t *p, struct listener *l, int fd)
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
    c->fd         = fd;
    c->p          = p;
    c->listener   = l;
    c->waiter     = nullptr;
    c->rx_head    = 0;
    c->rx_tail    = 0;
    c->recv       = RECV_ARMED;
    c->pausing    = false;
    c->cancelling = false;
    c->refs       = 2;
    c->closed     = false;
    c->eof        = false;
    c->err        = 0;
    c->pool_next  = nullptr;
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
    if (p->conn_free_count < p->cfg.idle_connections) {
        c->pool_next = p->conn_free;
        p->conn_free = c;
        p->conn_free_count++;
    } else {
        free(c);
    }
}

/* Free the pool at worker teardown. */
void ioxd__conn_pool_drain(proactor_t *p)
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
void ioxd__arm_recv(proactor_t *p, conn_t *c)
{
    struct io_uring_sqe *sqe = ioxd__sqe(p);
    sqe->opcode    = IORING_OP_RECV;
    sqe->fd        = c->fd;
    sqe->flags     = IOSQE_BUFFER_SELECT | (p->ring.fixed_files ? IOSQE_FIXED_FILE : 0);
    sqe->ioprio    = IORING_RECV_MULTISHOT;
    sqe->buf_group = BGID;
    sqe->user_data = UD(c, TAG_RECV);
    c->recv       = RECV_ARMED;
    c->cancelling = false;                           /* a fresh arm: no cancel of ours is in flight */
}

/* Resume the coroutine parked waiting for bytes, if there is one. It pops the queue itself. */
static void wake_reader(conn_t *c)
{
    coro_t *waiter = c->waiter;
    if (waiter) {
        c->waiter = nullptr;
        coro_resume(waiter);
    }
}

/* Count a recv that found the buffer ring empty, and say so on stderr at most once a second per
 * worker: starvation otherwise shows only as latency, and the cure is a larger -DBUF_COUNT. */
static void note_starved(proactor_t *p)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    p->starved_total++;
    p->starved_since_log++;
    if (now.tv_sec < p->starved_log_at)
        return;
    fprintf(stderr, "ioxd: [w%d] recv found no provided buffer %llu times (%llu in total, %u connections parked): "
                    "raise recv_buffers (ioxd_configure; now %u)\n",
            p->id, (unsigned long long)p->starved_since_log, (unsigned long long)p->starved_total, p->nstarved + 1, p->bufs.count);
    p->starved_since_log = 0;
    p->starved_log_at    = now.tv_sec + 1;
}

/* Park a connection whose recv ended on -ENOBUFS until a buffer comes back. */
static void starved_push(proactor_t *p, conn_t *c)
{
    if (p->nstarved == p->cap_starved) {
        unsigned cap   = p->cap_starved ? p->cap_starved * 2 : 64;
        conn_t **grown = realloc(p->starved, cap * sizeof *grown);
        if (!grown) {
            perror("realloc");
            abort();
        }
        p->starved     = grown;
        p->cap_starved = cap;
    }
    p->starved[p->nstarved++] = c;
}

/* Forget a parked connection (it closed before any buffer came back). The tail slides down rather
 * than the last entry taking its place: the loop re-arms the list oldest first. */
static void starved_remove(proactor_t *p, conn_t *c)
{
    for (unsigned i = 0; i < p->nstarved; i++) {
        if (p->starved[i] == c) {
            p->nstarved--;
            memmove(&p->starved[i], &p->starved[i + 1], (p->nstarved - i) * sizeof *p->starved);
            return;
        }
    }
}

/* A recv CQE: queue the data and wake the reader, or record the end of input and drop the recv's
 * ref. -ENOBUFS is not an error: the buffer group ran dry, so park and re-arm later. */
void ioxd__on_recv(proactor_t *p, conn_t *c, int result, unsigned flags)
{
    bool     more    = flags & IORING_CQE_F_MORE;
    bool     has_buf = flags & IORING_CQE_F_BUFFER;
    uint16_t buf_id  = (uint16_t)(flags >> (unsigned)IORING_CQE_BUFFER_SHIFT);

    trace("[w%d] recv fd=%d result=%d more=%d buf=%d buf_id=%u queued=%u state=%d closed=%d eof=%d\n",
          p->id, c->fd, result, more, has_buf, buf_id, c->rx_tail - c->rx_head, c->recv, c->closed, c->eof);

    if (!more)
        c->cancelling = false;                       /* the multishot ends here: no cancel is left in flight */

    if (result == -ENOBUFS) {
        if (c->closed) {
            c->recv = RECV_DONE;
            conn_unref(c);
            return;
        }
        if (c->pausing) {                            /* the pause we asked for; starvation stopped it first */
            c->pausing = false;
            c->recv    = RECV_PAUSED;
            wake_reader(c);
            return;
        }
        if (c->eof) {                                /* the input already ended: there is nothing to re-arm */
            c->recv = RECV_DONE;
            wake_reader(c);
            conn_unref(c);
            return;
        }
        c->recv = RECV_STARVED;
        note_starved(p);
        starved_push(p, c);
        return;
    }

    if (result <= 0) {                                  /* peer FIN (0), an error, or our own cancel */
        if (has_buf)
            ioxd__bufring_return(&p->bufs, buf_id);
        if (c->pausing && !c->closed && result == -ECANCELED) {   /* our pause, not the end of input */
            c->pausing = false;
            c->recv    = RECV_PAUSED;
            wake_reader(c);
            return;
        }
        /* Kernel TLS reports a control record - the peer's alert, a KeyUpdate it cannot honour -
         * as -EIO. The TLS design treats that as the end of input, so a close_notify reads like a FIN. */
        end_input(c, result == -EIO && c->listener->certs ? 0 : result);
        c->recv = RECV_DONE;
        wake_reader(c);                              /* a parked reader sees 0 / -errno          */
        conn_unref(c);                               /* the recv's ref; may recycle c            */
        return;
    }

    if (!has_buf) {
        /* A positive result must name the buffer it landed in. Nothing can be done with bytes we
         * cannot find, so end the input the way the queue-full policy does. */
        end_input(c, -EPROTO);
        if (more)
            cancel_recv(p, c);
        wake_reader(c);
    } else if (c->closed) {
        ioxd__bufring_return(&p->bufs, buf_id);                          /* the handler is gone; nobody will read it */
    } else if (c->rx_tail - c->rx_head == RX_QUEUE) {
        /* The handler is not draining. Rather than let one peer hoard the buffer group, end its
         * input: the next read sees -ENOBUFS. */
        ioxd__bufring_return(&p->bufs, buf_id);
        end_input(c, -ENOBUFS);
        if (more)
            cancel_recv(p, c);
        wake_reader(c);
    } else {
        struct rx_item *item = &c->rx[c->rx_tail++ & RX_MASK];
        item->ptr = ioxd__bufring_at(&p->bufs, buf_id);
        item->len = (uint32_t)result;
        item->buf_id = buf_id;
        wake_reader(c);
    }

    if (!more) {                                     /* the kernel ended the multishot: re-arm    */
        if (c->pausing && !c->closed) {              /* ended on its own while a pause was asked */
            c->pausing = false;
            c->recv    = RECV_PAUSED;
            wake_reader(c);
        } else if (!c->closed && !c->eof) {
            ioxd__arm_recv(p, c);
        } else {
            c->recv = RECV_DONE;
            conn_unref(c);
        }
    } else if (p->cancel_each && !c->closed) {       /* shutting down without a blanket cancel */
        cancel_recv(p, c);
    }
}

/* Shutdown: a recv parked on -ENOBUFS holds no operation the kernel can cancel, so end its input
 * by hand. Its handler wakes with an error, unwinds and closes the connection like any other. */
void ioxd__recv_drain(conn_t *c)
{
    if (c->recv != RECV_STARVED)
        return;
    end_input(c, -ECANCELED);
    c->recv = RECV_DONE;
    wake_reader(c);
    conn_unref(c);
}

/* ── close ─────────────────────────────────────────────────────────────────────────────── */

/* Close the socket with a CLOSE SQE, which rides the next enter with the rest of the batch: no
 * syscall here, and the close stays in order behind the SQEs already staged for that same socket -
 * a plain close(2) would race them. A file slot is named by index, a real fd by itself. */
void ioxd__close_socket(proactor_t *p, int fd)
{
    struct io_uring_sqe *sqe = ioxd__sqe(p);
    sqe->opcode    = IORING_OP_CLOSE;
    sqe->user_data = TAG_CLOSE;                      /* only a negative result is news */
    if (p->ring.fixed_files)
        sqe->file_index = (uint32_t)fd + 1;          /* slot + 1; 0 would mean "a real fd" */
    else
        sqe->fd = fd;
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
        cancel_recv(p, c);
    } else if (c->recv == RECV_STARVED || c->recv == RECV_PAUSED) {
        if (c->recv == RECV_STARVED)
            starved_remove(p, c);
        c->recv = RECV_DONE;
        c->refs--;                                   /* the recv side's reference; ours, dropped last, keeps c alive */
    }
    while (c->rx_head != c->rx_tail)
        ioxd__bufring_return(&p->bufs, c->rx[c->rx_head++ & RX_MASK].buf_id);

    ioxd__close_socket(p, c->fd);
    conn_unref(c);
}

/* The connection's coroutine: run the worker's handler to completion, then close. */
void ioxd__conn_main(void *arg)
{
    conn_t          *c = arg;
    char             gather[IOXD_PIPE_GATHER];
    char             slab[IOXD_PIPE_LEAD + IOXD_PIPE_CAP + IOXD_PIPE_SLACK];
    struct ioxd_pipe pipe;
    ioxd__pipe_init(&pipe, c, gather, sizeof gather, slab, IOXD_PIPE_LEAD, IOXD_PIPE_CAP, IOXD_PIPE_SLACK);
    c->p->handler(&pipe);
    ioxd__pipe_close(&pipe);
    conn_close(c);
}

/* ── awaits ────────────────────────────────────────────────────────────────────────────── */

/* The next received buffer, whole: the caller owns it until ioxd__bufring_return. Suspends until one
 * arrives; 1 with the item, 0 at the end of input, <0 an error. */
int ioxd__await_item(conn_t *c, struct rx_item *out)
{
    for (;;) {
        if (c->rx_head != c->rx_tail) {
            *out = c->rx[c->rx_head & RX_MASK];
            c->rx_head++;
            return 1;
        }
        if (c->eof)
            return c->err;
        c->waiter = coro_current();
        coro_yield();                                /* ioxd__on_recv wakes us */
    }
}


/* Stop the multishot recv so nothing more leaves the socket, and park until it has stopped.
 * 0 once it is paused, -1 when the input ended instead - the caller has nothing left to program. */
int ioxd__recv_pause(conn_t *c)
{
    proactor_t *p = c->p;
    if (c->recv == RECV_ARMED) {
        c->pausing = true;
        cancel_recv(p, c);
        while (c->recv == RECV_ARMED) {              /* data may still land meanwhile: fine, it is queued */
            c->waiter = coro_current();
            coro_yield();
        }
        c->pausing = false;
    }
    if (c->recv == RECV_STARVED) {
        starved_remove(p, c);
        c->recv = RECV_PAUSED;
    }
    if (c->eof)                                      /* it ended while we were stopping it */
        return -1;
    return c->recv == RECV_PAUSED ? 0 : -1;
}

/* Arm the recv again after a pause. False when there is nothing to arm - the input ended, or the
 * handler is already gone - so a prologue learns that its connection went away under it. */
bool ioxd__recv_resume(conn_t *c)
{
    if (c->recv != RECV_PAUSED || c->eof || c->closed)
        return false;
    if (c->p->draining) {
        /* The worker is stopping and its blanket cancel has already been and gone: a recv armed
         * now would never be cancelled and the handler would park behind it until the grace period
         * ran out. End the input instead, and let the handler unwind like everyone else's. */
        end_input(c, -ECANCELED);
        c->recv = RECV_DONE;
        conn_unref(c);                               /* the recv's ref; the handler still holds its own */
        return false;
    }
    ioxd__arm_recv(c->p, c);
    return true;
}

int ioxd__recv_exact(conn_t *c, void *dst, size_t n)
{
    uint8_t *at = dst;
    size_t   left = n;
    while (left) {
        op_t op;
        struct io_uring_sqe *sqe = ioxd__sqe(c->p);
        sqe->opcode    = IORING_OP_RECV;
        sqe->fd        = c->fd;
        sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
        sqe->addr      = (uint64_t)(uintptr_t)at;
        sqe->len       = (uint32_t)left;
        sqe->msg_flags = MSG_WAITALL;
        int got = await_op(sqe, &op);
        if (got < 0)
            return got;
        if (got == 0)
            return -ECONNRESET;
        at   += got;
        left -= (size_t)got;
    }
    return (int)n;
}

int ioxd__setsockopt(conn_t *c, int level, int name, const void *val, size_t len)
{
    op_t op;
    struct io_uring_sqe *sqe = ioxd__sqe(c->p);
    sqe->opcode  = IORING_OP_URING_CMD;
    sqe->fd      = c->fd;
    sqe->flags   = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
    sqe->cmd_op  = SOCKET_URING_OP_SETSOCKOPT;
    sqe->level   = (uint32_t)level;
    sqe->optname = (uint32_t)name;
    sqe->optval  = (uint64_t)(uintptr_t)val;
    sqe->optlen  = (uint32_t)len;
    int rc = await_op(sqe, &op);
    /* The plain fallback needs a real descriptor, and under registered files (the default) c->fd is
     * a slot index, so it is dead code there: kernel TLS then wants a kernel with
     * SOCKET_URING_OP_SETSOCKOPT (6.7+), or a build with -DFIXED_FILES=0. */
    if ((rc == -EOPNOTSUPP || rc == -EINVAL) && !c->p->ring.fixed_files)   /* a kernel without the command */
        rc = setsockopt(c->fd, level, name, val, (socklen_t)len) < 0 ? -errno : 0;
    return rc;
}

int ioxd__sendmsg(conn_t *c, const struct msghdr *msg)
{
    op_t op;
    struct io_uring_sqe *sqe = ioxd__sqe(c->p);
    sqe->opcode    = IORING_OP_SENDMSG;
    sqe->fd        = c->fd;
    sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
    sqe->addr      = (uint64_t)(uintptr_t)msg;
    sqe->len       = 1;
    sqe->msg_flags = MSG_NOSIGNAL;
    return await_op(sqe, &op);
}

/* Send all of buf: a SEND SQE per round, parked until its CQE. Returns len, or -errno. */
int await_send(conn_t *c, const void *buf, size_t len)
{
    const uint8_t *src  = buf;
    size_t         left = len;
    while (left > 0) {
        op_t op;
        struct io_uring_sqe *sqe = ioxd__sqe(c->p);
        sqe->opcode    = IORING_OP_SEND;
        sqe->fd        = c->fd;
        sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
        sqe->addr      = (uint64_t)(uintptr_t)src;
        sqe->len       = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        sqe->msg_flags = MSG_NOSIGNAL;                /* no SIGPIPE; the loop finishes short sends (kernel TLS refuses MSG_WAITALL) */
        int n = await_op(sqe, &op);
        if (n < 0)
            return n;
        if (n == 0)
            return -EPIPE;
        src  += n;
        left -= (size_t)n;
    }
    return (int)len;
}

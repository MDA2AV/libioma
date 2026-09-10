#include "io/internal.h"
#include "io/pipe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void submit_cancel(proactor_t *p, uint64_t target_user_data)
{
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode    = IORING_OP_ASYNC_CANCEL;
    sqe->fd        = -1;
    sqe->addr      = target_user_data;
    sqe->user_data = TAG_IGNORE;
}

static void cancel_recv(proactor_t *p, conn_t *c)
{
    if (c->cancelling)
        return;
    c->cancelling = true;
    submit_cancel(p, UD(c, TAG_RECV));
}

static void end_input(conn_t *c, int err)
{
    if (!c->eof) {
        c->eof = true;
        c->err = err;
    }
}

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
    ioxd__spsc_reset(&c->rx);
    c->recv       = RECV_ARMED;
    c->pausing    = false;
    c->cancelling = false;
    c->throttled  = false;
    c->refs       = 2;
    c->closed     = false;
    c->eof        = false;
    c->err        = 0;
    c->pool_next  = nullptr;
    p->live++;
    return c;
}

static void conn_unref(conn_t *c)
{
    if (--c->refs != 0)
        return;
    proactor_t *p = c->p;
    p->live--;
    ioxd__spsc_reset(&c->rx);
    if (p->conn_free_count < p->cfg.idle_connections) {
        c->pool_next = p->conn_free;
        p->conn_free = c;
        p->conn_free_count++;
    } else {
        free(c);
    }
}

void ioxd__conn_pool_drain(proactor_t *p)
{
    while (p->conn_free) {
        conn_t *c = p->conn_free;
        p->conn_free = c->pool_next;
        free(c);
    }
    p->conn_free_count = 0;
}

void ioxd__conn_arm_recv(proactor_t *p, conn_t *c)
{
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode    = IORING_OP_RECV;
    sqe->fd        = c->fd;
    sqe->flags     = IOSQE_BUFFER_SELECT | (p->ring.fixed_files ? IOSQE_FIXED_FILE : 0);
    sqe->ioprio    = IORING_RECV_MULTISHOT;
    sqe->buf_group = BGID;
    sqe->user_data = UD(c, TAG_RECV);
    c->recv       = RECV_ARMED;
    c->cancelling = false;
    c->throttled  = false;
}

static void wake_reader(conn_t *c)
{
    coro_t *waiter = c->waiter;
    if (waiter) {
        c->waiter = nullptr;
        ioxd__coro_resume(waiter);
    }
}

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

void ioxd__conn_on_recv(proactor_t *p, conn_t *c, int result, unsigned flags)
{
    bool     more    = flags & IORING_CQE_F_MORE;
    bool     has_buf = flags & IORING_CQE_F_BUFFER;
    uint16_t buf_id  = (uint16_t)(flags >> (unsigned)IORING_CQE_BUFFER_SHIFT);

    trace("[w%d] recv fd=%d result=%d more=%d buf=%d buf_id=%u queued=%u state=%d closed=%d eof=%d\n",
          p->id, c->fd, result, more, has_buf, buf_id, ioxd__spsc_count(&c->rx), c->recv, c->closed, c->eof);

    if (!more)
        c->cancelling = false;

    if (result == -ENOBUFS) {
        if (c->closed) {
            c->recv = RECV_DONE;
            conn_unref(c);
            return;
        }
        if (c->pausing) {
            c->pausing = false;
            c->recv    = RECV_PAUSED;
            wake_reader(c);
            return;
        }
        if (c->eof) {
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

    if (result <= 0) {
        if (has_buf)
            ioxd__bufring_return(&p->bufs, buf_id);
        if (c->pausing && !c->closed && result == -ECANCELED) {
            c->pausing = false;
            c->recv    = RECV_PAUSED;
            wake_reader(c);
            return;
        }

        end_input(c, result == -EIO && c->listener && c->listener->certs ? 0 : result);
        c->recv = RECV_DONE;
        wake_reader(c);
        conn_unref(c);
        return;
    }

    if (!has_buf) {
        end_input(c, -EPROTO);
        if (more)
            cancel_recv(p, c);
        wake_reader(c);
    } else if (c->closed) {
        ioxd__bufring_return(&p->bufs, buf_id);
    } else if (ioxd__spsc_full(&c->rx) && !ioxd__spsc_grow(&c->rx)) {
        ioxd__bufring_return(&p->bufs, buf_id);
        end_input(c, -ENOMEM);
        if (more)
            cancel_recv(p, c);
        wake_reader(c);
    } else {
        struct rx_item *item = ioxd__spsc_push(&c->rx);
        item->ptr    = ioxd__bufring_at(&p->bufs, buf_id);
        item->len    = (uint32_t)result;
        item->buf_id = buf_id;
        wake_reader(c);
        if (more && !c->pausing && !c->closed && ioxd__spsc_count(&c->rx) >= RX_QUEUE) {
            c->pausing   = true;
            c->throttled = true;
            cancel_recv(p, c);
        }
    }

    if (!more) {
        if (c->pausing && !c->closed) {
            c->pausing = false;
            c->recv    = RECV_PAUSED;
            wake_reader(c);
        } else if (!c->closed && !c->eof) {
            ioxd__conn_arm_recv(p, c);
        } else {
            c->recv = RECV_DONE;
            conn_unref(c);
        }
    } else if (p->cancel_each && !c->closed) {
        cancel_recv(p, c);
    }
}

void ioxd__conn_recv_drain(conn_t *c)
{
    if (c->recv != RECV_STARVED)
        return;
    end_input(c, -ECANCELED);
    c->recv = RECV_DONE;
    wake_reader(c);
    conn_unref(c);
}

void ioxd__conn_close_socket(proactor_t *p, int fd)
{
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode    = IORING_OP_CLOSE;
    sqe->user_data = TAG_CLOSE;
    if (p->ring.fixed_files)
        sqe->file_index = (uint32_t)fd + 1;
    else
        sqe->fd = fd;
}

void ioxd__conn_close(conn_t *c)
{
    proactor_t *p = c->p;
    c->closed = true;
    trace("[w%d] close fd=%d state=%d eof=%d err=%d queued=%u\n",
          p->id, c->fd, c->recv, c->eof, c->err, ioxd__spsc_count(&c->rx));

    if (c->recv == RECV_ARMED) {
        cancel_recv(p, c);
    } else if (c->recv == RECV_STARVED || c->recv == RECV_PAUSED) {
        if (c->recv == RECV_STARVED)
            starved_remove(p, c);
        c->recv = RECV_DONE;
        c->refs--;
    }
    while (!ioxd__spsc_empty(&c->rx))
        ioxd__bufring_return(&p->bufs, ioxd__spsc_pop(&c->rx).buf_id);

    ioxd__conn_close_socket(p, c->fd);
    conn_unref(c);
}

void ioxd__conn_main(void *arg)
{
    conn_t          *c = arg;
    char             gather[IOXD_PIPE_GATHER];
    char             slab[IOXD_PIPE_LEAD + IOXD_PIPE_CAP + IOXD_PIPE_SLACK];
    struct ioxd_pipe pipe;
    ioxd__pipe_init(&pipe, c, gather, sizeof gather, slab, IOXD_PIPE_LEAD, IOXD_PIPE_CAP, IOXD_PIPE_SLACK);
    c->p->handler(&pipe);
    ioxd__pipe_close(&pipe);
    ioxd__conn_close(c);
}

static void throttle_release(conn_t *c)
{
    if (c->recv == RECV_PAUSED)
        ioxd__conn_recv_resume(c);
}

int ioxd__conn_recv_item(conn_t *c, struct rx_item *out)
{
    for (;;) {
        if (!ioxd__spsc_empty(&c->rx)) {
            *out = ioxd__spsc_pop(&c->rx);
            if (c->throttled && ioxd__spsc_count(&c->rx) <= RX_QUEUE / 2)
                throttle_release(c);
            return 1;
        }
        if (c->eof)
            return c->err;
        if (c->throttled)
            throttle_release(c);
        c->waiter = ioxd__coro_current();   /* NOLINT(clang-analyzer-unix.Malloc): our own ref keeps c through the recv's drop */
        ioxd__coro_yield();
    }
}

int ioxd__conn_recv_pause(conn_t *c)
{
    proactor_t *p = c->p;
    if (c->recv == RECV_ARMED) {
        c->pausing = true;
        cancel_recv(p, c);
        while (c->recv == RECV_ARMED) {
            c->waiter = ioxd__coro_current();
            ioxd__coro_yield();
        }
        c->pausing = false;
    }
    if (c->recv == RECV_STARVED) {
        starved_remove(p, c);
        c->recv = RECV_PAUSED;
    }
    if (c->eof)
        return -1;
    return c->recv == RECV_PAUSED ? 0 : -1;
}

bool ioxd__conn_recv_resume(conn_t *c)
{
    if (c->recv != RECV_PAUSED || c->eof || c->closed)
        return false;
    if (c->p->draining) {
        end_input(c, -ECANCELED);
        c->recv = RECV_DONE;
        conn_unref(c);
        return false;
    }
    ioxd__conn_arm_recv(c->p, c);
    return true;
}

int ioxd__conn_recv_exact(conn_t *c, void *dst, size_t n)
{
    uint8_t *at = dst;
    size_t   left = n;
    while (left) {
        op_t op;
        struct io_uring_sqe *sqe = ioxd__proactor_sqe(c->p);
        sqe->opcode    = IORING_OP_RECV;
        sqe->fd        = c->fd;
        sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
        sqe->addr      = (uintptr_t)at;
        sqe->len       = (uint32_t)left;
        sqe->msg_flags = MSG_WAITALL;
        int got = ioxd__io_await(sqe, &op);
        if (got < 0)
            return got;
        if (got == 0)
            return -ECONNRESET;
        at   += got;
        left -= (size_t)got;
    }
    return (int)n;
}

int ioxd__conn_setsockopt(conn_t *c, int level, int name, const void *val, size_t len)
{
    op_t op;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(c->p);
    sqe->opcode  = IORING_OP_URING_CMD;
    sqe->fd      = c->fd;
    sqe->flags   = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
    sqe->cmd_op  = SOCKET_URING_OP_SETSOCKOPT;
    sqe->level   = (uint32_t)level;
    sqe->optname = (uint32_t)name;
    sqe->optval  = (uintptr_t)val;
    sqe->optlen  = (uint32_t)len;
    int rc = ioxd__io_await(sqe, &op);

    if ((rc == -EOPNOTSUPP || rc == -EINVAL) && !c->p->ring.fixed_files)
        rc = setsockopt(c->fd, level, name, val, (socklen_t)len) < 0 ? -errno : 0;
    return rc;
}

int ioxd__conn_sendmsg(conn_t *c, const struct msghdr *msg)
{
    op_t op;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(c->p);
    sqe->opcode    = IORING_OP_SENDMSG;
    sqe->fd        = c->fd;
    sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
    sqe->addr      = (uintptr_t)msg;
    sqe->len       = 1;
    sqe->msg_flags = MSG_NOSIGNAL;
    return ioxd__io_await(sqe, &op);
}

int ioxd__conn_send(conn_t *c, const void *buf, size_t len)
{
    const uint8_t *src  = buf;
    size_t         left = len;
    while (left > 0) {
        op_t op;
        struct io_uring_sqe *sqe = ioxd__proactor_sqe(c->p);
        sqe->opcode    = IORING_OP_SEND;
        sqe->fd        = c->fd;
        sqe->flags     = c->p->ring.fixed_files ? IOSQE_FIXED_FILE : 0;
        sqe->addr      = (uintptr_t)src;
        sqe->len       = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        sqe->msg_flags = MSG_NOSIGNAL;
        int n = ioxd__io_await(sqe, &op);
        if (n < 0)
            return n;
        if (n == 0)
            return -EPIPE;
        src  += n;
        left -= (size_t)n;
    }
    return (int)len;
}

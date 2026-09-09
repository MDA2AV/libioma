/*
 * conn.h - one connection on its worker: the socket, the queue of buffers the kernel filled for
 * it, the state of its multishot recv, and the awaits a coroutine calls on it. Thread-per-core:
 * a connection is only ever touched by the worker that accepted it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "io/coro.h"

/* tunables (override with -D) */
#ifndef RX_QUEUE
#define RX_QUEUE      64                  /* undelivered slices one connection may hold, pow 2  */
#endif
static_assert(((unsigned)RX_QUEUE & ((unsigned)RX_QUEUE - 1U)) == 0 && RX_QUEUE >= 2,
              "RX_QUEUE: a power of two (RX_MASK is a bit mask over it)");
#define RX_MASK       (RX_QUEUE - 1U)
#ifndef CONN_POOL_MAX
#define CONN_POOL_MAX 1024                /* idle conn_t kept warm per worker by default (ioxd_config.idle_connections) */
#endif

typedef struct proactor proactor_t;
typedef struct conn     conn_t;
struct listener;                          /* io/proactor.h: the port it was accepted on */
struct msghdr;                            /* <sys/socket.h>, for ioxd__conn_sendmsg          */

/* A slice the kernel delivered into a provided buffer, waiting for the handler to read it. */
struct rx_item {
    uint8_t *ptr;
    uint32_t len;
    uint16_t buf_id;
};

enum recv_state {
    RECV_ARMED,                           /* multishot recv in flight; the kernel may post CQEs */
    RECV_STARVED,                         /* it ended on -ENOBUFS; re-armed once buffers return */
    RECV_PAUSED,                          /* stopped on purpose, its ref kept; resume re-arms   */
    RECV_DONE,                            /* it posted its terminal CQE                         */
};

struct conn {
    int             fd;                   /* the socket, or its file slot under fixed files    */
    proactor_t     *p;
    struct listener *listener;            /* the port it came in on: plain or TLS               */
    coro_t         *waiter;               /* coroutine parked waiting for bytes, or nullptr      */
    struct rx_item  rx[RX_QUEUE];         /* delivered while nobody was reading                */
    unsigned        rx_head, rx_tail;
    enum recv_state recv;
    bool            pausing;              /* a cancel is in flight to pause the recv           */
    bool            cancelling;           /* a cancel is in flight: do not stage a second one  */
    int             refs;                 /* the handler coroutine + the armed/starved recv    */
    bool            closed;               /* the handler returned; fd closed                   */
    bool            eof;                  /* recv ended: peer FIN, error, or queue overflow    */
    int             err;                  /* 0 on FIN, else the negative errno                 */
    struct conn    *pool_next;            /* free-list link while recycled (not in use): a LIFO - a  */
                                          /* returned conn becomes the head and points at the old one */
};

/* What a coroutine calls, on the owning worker: every one of these suspends it, and the loop
 * resumes it when the completion arrives - so none is named for the waiting, each for what it
 * does, after the syscall where there is one. */
int ioxd__conn_send     (conn_t *c, const void *buf, size_t len);   /* all of buf: len, else -errno                */
int ioxd__conn_recv_item(conn_t *c, struct rx_item *out);           /* the next delivered buffer, whole: 1; 0 at the end; <0 -errno (the reader's primitive) */

/* For a protocol prologue (TLS): stop the multishot recv so nothing more leaves the socket, take
 * what it already delivered, read exact byte counts straight from the socket, program the
 * socket, then resume. All suspend like any await. */
int  ioxd__conn_recv_pause (conn_t *c);                        /* 0 once stopped; -1 if the input already ended */
bool ioxd__conn_recv_resume(conn_t *c);                        /* true once re-armed; false if it ended meanwhile */
int  ioxd__conn_recv_exact (conn_t *c, void *dst, size_t n);   /* n bytes into dst, or <0                     */
int  ioxd__conn_setsockopt (conn_t *c, int level, int name, const void *val, size_t len);   /* 0 or -errno; over the ring */
int  ioxd__conn_sendmsg    (conn_t *c, const struct msghdr *msg);   /* one sendmsg, for a message with control data */

/* For the loop (proactor.c): a connection's life from accept to the pool. */
conn_t *ioxd__conn_new(proactor_t *p, struct listener *l, int fd);   /* from the pool, or fresh  */
void    ioxd__conn_main(void *arg);                      /* the connection's coroutine body      */
void    ioxd__conn_arm_recv(proactor_t *p, conn_t *c);        /* one multishot recv                   */
void    ioxd__conn_on_recv(proactor_t *p, conn_t *c, int res, unsigned flags);   /* a recv CQE       */
void    ioxd__conn_recv_drain(conn_t *c);                     /* shutdown: end a recv parked on -ENOBUFS */
void    ioxd__conn_close_socket(proactor_t *p, int fd);       /* close a socket through the ring      */
void    ioxd__conn_close(conn_t *c);                     /* the coroutine is done with it: cancel, close, drop its ref */
void    ioxd__conn_pool_drain(proactor_t *p);            /* free the pool at teardown            */

/* ── conn.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * conn.c - one connection's life on its worker: the pooled conn_t, its two owners' refcount,
 * the multishot recv and the queue of buffers it delivers, parking on -ENOBUFS, closing, and
 * the awaits the pipe is built on.
 */

/* at file scope:
 *   - ── the object ──────────────────────────────────────────────────────────────────────────
 *     [/ * Take a conn_t from the pool (or calloc one) and reset it for a fresh fd. Two ]
 *   - ── the recv side ───────────────────────────────────────────────────────────────────────
 *     [void ioxd__conn_arm_recv(proactor_t *p, conn_t *c)]
 *   - ── close ───────────────────────────────────────────────────────────────────────────────
 *     [/ * Close the socket with a CLOSE SQE, which rides the next enter with the rest o]
 *   - ── awaits ──────────────────────────────────────────────────────────────────────────────
 *     [/ * The next received buffer, whole: the caller owns it until ioxd__bufring_retur]
 */

/* (await_op, now ioxd__io_await in io/internal.h):
 * Stage a one-shot op and park until its CQE. The loop fills op->res and resumes us.
 */

/* submit_cancel:
 * Ask the kernel to cancel the op carrying that user_data. The acknowledgement CQE is ignored.
 */

/* cancel_recv:
 * Cancel the multishot recv, at most one cancel in flight: the queue-full policy would
 * otherwise stage another on every arrival while the queue stays full. Cleared on the terminal
 * CQE.
 */

/* end_input:
 * End the input once: the first reason wins, so a cancel we asked for afterwards does not
 * overwrite the peer's FIN or the error that really ended it.
 */

/* ioxd__conn_new:
 * Take a conn_t from the pool (or calloc one) and reset it for a fresh fd. Two owners hold it:
 * the handler coroutine and the multishot recv, so refs starts at 2.
 */

/* conn_unref:
 * Drop one owner's ref. At zero the conn holds nothing (fd closed, buffers returned) and goes
 * back to the pool, or is freed past the cap.
 */

/* ioxd__conn_pool_drain:
 * Free the pool at worker teardown.
 */

/* ioxd__conn_arm_recv:
 * Arm the multishot recv: one SQE, then a CQE per arrival, each in a buffer the kernel picks.
 *   - a fresh arm: no cancel of ours is in flight  [c->cancelling = false;]
 */

/* wake_reader:
 * Resume the coroutine parked waiting for bytes, if there is one. It pops the queue itself.
 */

/* note_starved:
 * Count a recv that found the buffer ring empty, and say so on stderr at most once a second
 * per worker: starvation otherwise shows only as latency, and the cure is a larger
 * -DBUF_COUNT.
 */

/* starved_push:
 * Park a connection whose recv ended on -ENOBUFS until a buffer comes back.
 */

/* starved_remove:
 * Forget a parked connection (it closed before any buffer came back). The tail slides down
 * rather than the last entry taking its place: the loop re-arms the list oldest first.
 */

/* ioxd__conn_on_recv:
 * A recv CQE: queue the data and wake the reader, or record the end of input and drop the
 * recv's ref. -ENOBUFS is not an error: the buffer group ran dry, so park and re-arm later.
 *   - the multishot ends here: no cancel is left in flight  [c->cancelling = false;]
 *   - the pause we asked for; starvation stopped it first  [if (c->pausing) {]
 *   - the input already ended: there is nothing to re-arm  [if (c->eof) {]
 *   - peer FIN (0), an error, or our own cancel  [if (result <= 0) {]
 *   - our pause, not the end of input  [if (c->pausing && !c->closed && result == -ECANCELED)
 *     {]
 *   - Kernel TLS reports a control record - the peer's alert, a KeyUpdate it cannot honour -
 *     as -EIO. The TLS design treats that as the end of input, so a close_notify reads like a
 *     FIN.  [end_input(c, result == -EIO && c->listener->certs ? 0 : result);]
 *   - a parked reader sees 0 / -errno  [wake_reader(c);]
 *   - the recv's ref; may recycle c  [conn_unref(c);]
 *   - A positive result must name the buffer it landed in. Nothing can be done with bytes we
 *     cannot find, so end the input the way the queue-full policy does.  [end_input(c,
 *     -EPROTO);]
 *   - the handler is gone; nobody will read it  [ioxd__bufring_return(&p->bufs, buf_id);]
 *   - The handler is not draining. Rather than let one peer hoard the buffer group, end its
 *     input: the next read sees -ENOBUFS.  [ioxd__bufring_return(&p->bufs, buf_id);]
 *   - the kernel ended the multishot: re-arm  [if (!more) {]
 *   - ended on its own while a pause was asked  [if (c->pausing && !c->closed) {]
 *   - shutting down without a blanket cancel  [} else if (p->cancel_each && !c->closed) {]
 */

/* ioxd__conn_recv_drain:
 * Shutdown: a recv parked on -ENOBUFS holds no operation the kernel can cancel, so end its
 * input by hand. Its handler wakes with an error, unwinds and closes the connection like any
 * other.
 */

/* ioxd__conn_close_socket:
 * Close the socket with a CLOSE SQE, which rides the next enter with the rest of the batch: no
 * syscall here, and the close stays in order behind the SQEs already staged for that same
 * socket - a plain close(2) would race them. A file slot is named by index, a real fd by
 * itself.
 *   - only a negative result is news  [sqe->user_data = TAG_CLOSE;]
 *   - slot + 1; 0 would mean "a real fd"  [sqe->file_index = (uint32_t)fd + 1;]
 */

/* ioxd__conn_close:
 * Runs once when the handler returns: cancel the recv, hand unread buffers back, close the fd,
 * drop the handler's ref. The recv's own ref drops on its terminal CQE.
 *   - the recv side's reference; ours, dropped last, keeps c alive  [c->refs--;]
 */

/* ioxd__conn_main:
 * The connection's coroutine: run the worker's handler to completion, then close.
 */

/* ioxd__conn_recv_item:
 * The next received buffer, whole: the caller owns it until ioxd__bufring_return. Suspends
 * until one arrives; 1 with the item, 0 at the end of input, <0 an error.
 *   - ioxd__conn_on_recv wakes us  [ioxd__coro_yield();]
 */

/* ioxd__conn_recv_pause:
 * Stop the multishot recv so nothing more leaves the socket, and park until it has stopped. 0
 * once it is paused, -1 when the input ended instead - the caller has nothing left to program.
 *   - data may still land meanwhile: fine, it is queued  [while (c->recv == RECV_ARMED) {]
 *   - it ended while we were stopping it  [if (c->eof)]
 */

/* ioxd__conn_recv_resume:
 * Arm the recv again after a pause. False when there is nothing to arm - the input ended, or
 * the handler is already gone - so a prologue learns that its connection went away under it.
 *   - The worker is stopping and its blanket cancel has already been and gone: a recv armed
 *     now would never be cancelled and the handler would park behind it until the grace period
 *     ran out. End the input instead, and let the handler unwind like everyone else's.
 *     [end_input(c, -ECANCELED);]
 *   - the recv's ref; the handler still holds its own  [conn_unref(c);]
 */

/* ioxd__conn_setsockopt:
 *   - The plain fallback needs a real descriptor, and under registered files (the default)
 *     c->fd is a slot index, so it is dead code there: kernel TLS then wants a kernel with
 *     SOCKET_URING_OP_SETSOCKOPT (6.7+), or a build with -DFIXED_FILES=0.  [if ((rc ==
 *     -EOPNOTSUPP || rc == -EINVAL) && !c->p->ring.fixed_files)]
 *   - a kernel without the command  [if ((rc == -EOPNOTSUPP || rc == -EINVAL) &&
 *     !c->p->ring.fixed_files)]
 */

/* ioxd__conn_send:
 * Send all of buf: a SEND SQE per round, parked until its CQE. Returns len, or -errno.
 *   - no SIGPIPE; the loop finishes short sends (kernel TLS refuses MSG_WAITALL)
 *     [sqe->msg_flags = MSG_NOSIGNAL;]
 */

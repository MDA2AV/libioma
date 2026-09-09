/*
 * proactor.h - one worker: one thread, one io_uring, one SO_REUSEPORT socket per listening port,
 * one provided buffer ring, and the coroutines that run on it. The connections it serves are
 * io/conn.h. Thread-per-core, shared-nothing: nothing in here is touched by any other thread
 * except the stop flag.
 *
 * The loop: run freshly spawned coroutines, re-arm recvs parked on -ENOBUFS, publish and enter
 * once (submit everything staged, wait for >= 1 completion), then dispatch the CQ batch, each CQE
 * copied out of the ring before its handler runs. The head is published once at the end of the
 * batch - and in ioxd__sqe ahead of an enter made in the middle of one, so the kernel has room for
 * what that enter completes. Dispatching resumes handler coroutines inline, so the sends they
 * stage ride the next enter together with the batch.
 *
 * On stop the loop does not simply leave: it cancels the accepts and everything in flight, then
 * keeps running until the last connection has closed itself or a two-second grace period is up,
 * so parked coroutines wake with errors and their handlers unwind before anything is torn down.
 */
#pragma once

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "ioxd/config.h"
#include "io/bufring.h"
#include "io/conn.h"
#include "io/coro.h"
#include "io/uring.h"

/* defaults (override with -D, or at run time with ioxd_configure) */
#ifndef RING_ENTRIES
#define RING_ENTRIES 4096                 /* SQ depth; the CQ is twice that                     */
#endif
#ifndef STACK_SIZE
#define STACK_SIZE   (128UL * 1024)       /* per coroutine, plus a 64 KB guard; only touched pages cost RSS */
#endif
#ifndef FIXED_FILES
#define FIXED_FILES  16384                /* registered file slots per worker; 0 disables       */
#endif

typedef struct proactor proactor_t;
struct ioxd_pipe;
typedef void (*handler_fn)(struct ioxd_pipe *pipe);   /* a connection, as a pipe */

#ifndef IOXD_MAX_LISTENERS
#define IOXD_MAX_LISTENERS 8                  /* ports one server may serve                        */
#endif

/* A listening port. Every worker opens its own socket on it (SO_REUSEPORT), so the kernel spreads
 * the port's connections across workers; an accept CQE carries the listener it came from. */
struct listener {
    proactor_t *p;
    int         fd;                       /* the socket, or its file slot under fixed files    */
    uint16_t    port;
    void *certs;                      /* the port's certificate store, or nullptr: plain   */

    /* accept back-pressure: out of descriptors, file slots or memory, re-arming at once would
     * spin, so the accept is left unarmed until there is room again (see rearm_stalled). */
    bool        stalled;
    unsigned    stalled_live;             /* p->live when it stalled: re-arm once that drops   */
    time_t      retry_at;                 /* ... or at this second, whichever comes first      */
    time_t      err_log_at;               /* the next second an accept error may be logged     */
    uint64_t    err_since_log;            /* accept errors swallowed since the last line       */
};

struct proactor {
    /* set by the creator */
    int                    id;
    int                    cpu;           /* pin the thread here; -1 = don't                   */
    struct listener        listeners[IOXD_MAX_LISTENERS];   /* port and certs set by the creator  */
    int                    n_listeners;
    handler_fn             handler;
    volatile sig_atomic_t *stop;
    ioxd_config            cfg;           /* every field filled in: the run resolved the defaults */

    /* owned by the worker thread */
    struct uring              ring;
    unsigned      cq_taken;              /* CQEs taken from the ring this batch, head not yet published */
    struct bufring            bufs;       /* the provided buffers recvs deliver into           */
    conn_t                  **starved;    /* connections parked on -ENOBUFS                    */
    unsigned                  nstarved, cap_starved;
    uint64_t                  starved_total;      /* recvs that found the ring empty, ever       */
    uint64_t                  starved_since_log;  /* ... since the last log line                 */
    time_t                    starved_log_at;     /* the next second a log line may go out       */
    coro_t                   *ready_head, *ready_tail;   /* spawned, not yet started           */
    unsigned                  live;       /* open connections                                  */
    unsigned                  file_slots; /* registered file table size, 0 when there is none  */
    uint64_t                  accepted;
    conn_t                   *conn_free;         /* recycled conn_t objects, reused on accept   */
    unsigned                  conn_free_count;
    bool                      draining;   /* stop was seen: take nothing new, let the rest end */
    bool                      cancel_each;        /* ... and CANCEL_ANY was refused: one at a time */
    int                       failed;     /* 0, or the -errno that retired this worker         */
};

/* The worker thread's whole life: ring, buffers, listeners, loop until *stop, drain, teardown. */
void proactor_run(proactor_t *p);

/* Start a coroutine on this worker. Safe from the loop or from any coroutine on it. */
void proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg);

/* Claim an SQE to stage an operation; flushes without waiting when the SQ is full. For the
 * plane's own files: every op goes through here so it rides the loop's next enter. */
struct io_uring_sqe *ioxd__sqe(proactor_t *p);

/* ── proactor.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * proactor.c - the worker: pin to a CPU, own a ring, a buffer ring and a listener, then loop:
 * start spawned coroutines, enter once per batch, dispatch every completion. Connections live
 * in conn.c and buffers in bufring.c; this file is the loop and what feeds it.
 */

/* at file scope:
 *   - how long a stopping worker waits for its connections  [#define DRAIN_GRACE_MS 2000]
 *   - ── submission ──────────────────────────────────────────────────────────────────────────
 *     [/ * Claim an SQE. If the SQ is full mid-batch, submit what is staged and retry. -]
 *   - Claim an SQE. If the SQ is full mid-batch, submit what is staged and retry. -EBUSY means
 *     the kernel is holding completions it could not fit in the CQ and will not take more
 *     submissions, so the retry enters with GETEVENTS to flush them: safe mid-batch, since the
 *     loop dispatches whatever lands next time round.
 *   - ── accept ──────────────────────────────────────────────────────────────────────────────
 *     [/ * Whether this worker may take another connection. Under registered files a soc]
 *   - ── completions ─────────────────────────────────────────────────────────────────────────
 *     [static void dispatch(proactor_t *p, struct io_uring_cqe *cqe)]
 *   - ── scheduling ──────────────────────────────────────────────────────────────────────────
 *     [void proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg)]
 *   - ── listener ────────────────────────────────────────────────────────────────────────────
 *     [/ * A SO_REUSEPORT socket on port; every worker opens its own, so the kernel spre]
 *   - ── shutdown ────────────────────────────────────────────────────────────────────────────
 *     [/ * Stop is set: take nothing new and ask the kernel to end everything in flight,]
 *   - ── the loop ────────────────────────────────────────────────────────────────────────────
 *     [/ * Pin this thread to the idx-th CPU the process may run on. Reading the inherit]
 */

/* now_ms:
 * Coarse monotonic milliseconds: for the drain deadline and the once-a-second retries, never
 * for anything that needs better than a tick.
 */

/* publish_cq:
 * Publish the CQ head for the entries taken so far: once per batch in the loop, and before any
 * enter in the middle of one, so the kernel has room for what the enter completes.
 */

/* ioxd__sqe:
 *   - the kernel must see staged returns before this enter  [ioxd__bufring_publish(&p->bufs);]
 *   - and have room in the CQ for what it completes  [publish_cq(p);]
 *   - reap without waiting; the CQ has room again  [rc = uring_submit_wait(&p->ring, 0,
 *     nullptr);]
 */

/* accept_room:
 * Whether this worker may take another connection. Under registered files a socket lives in a
 * slot, so the table is a hard ceiling: better to stop arming than to fail one accept per
 * arrival.
 */

/* stall_accept:
 * Leave the accept unarmed until there is room again.
 */

/* arm_accept:
 * Arm the multishot accept: one SQE, then a CQE per new connection.
 *   - land each socket in a free slot, not an fd  [sqe->file_index = IORING_FILE_INDEX_ALLOC;]
 */

/* rearm_stalled:
 * Re-arm a listener that stalled: once a connection has closed (p->live below what it was),
 * and once a second regardless, since the shortage may be another thread's and no close of
 * ours would ever announce it. Only ever a handful of listeners, and only after an accept ran
 * out of room.
 */

/* accept_exhausted:
 * Out of descriptors, slots or memory: the next accept would fail the same way.
 */

/* note_accept_error:
 * Log an accept error at most once a second per listener, with how many it stands for: a full
 * file table would otherwise write one line per arrival, forever.
 */

/* on_accept:
 * An accept CQE: wrap the new fd in a conn, arm its recv, spawn its handler coroutine.
 *   - accepted just before the cancel: no new work  [ioxd__close_socket(p, result);]
 *   - TCP_NODELAY came with the listener  [conn_t *c = ioxd__conn_new(p, l, result);]
 *   - our own shutdown cancel is not an error  [} else if (!(p->draining && result ==
 *     -ECANCELED)) {]
 *   - still armed: nothing to do  [if (flags & IORING_CQE_F_MORE)]
 *   - rearm_stalled picks it up when there is room  [stall_accept(l);]
 */

/* dispatch:
 * Route one CQE by the tag in its user_data. Handler coroutines resume inline from here.
 *   - to its next await; op may be gone after  [coro_resume(op->waiter);]
 *   - a failed close leaks a slot: say so  [case TAG_CLOSE:]
 *   - the shutdown's one blanket cancel  [case TAG_DRAIN:]
 *   - TAG_IGNORE: cancel acknowledgements  [default:]
 */

/* proactor_spawn:
 * Queue a new coroutine; the loop starts it on its next iteration.
 */

/* run_ready:
 * Start every coroutine spawned since the last iteration.
 */

/* rearm_starved:
 * Re-arm recvs parked on -ENOBUFS, at most one per buffer that actually came back since the
 * last sweep: re-arming the whole list on a single return would send them all back to the
 * empty ring. Oldest first, so a connection parked early is not starved by later ones.
 *   - only this round's returns count as room  [p->bufs.returned = 0;]
 *   - keeps the ref it already holds  [ioxd__arm_recv(p, p->starved[i]);]
 */

/* listener_open:
 * A SO_REUSEPORT socket on port; every worker opens its own, so the kernel spreads
 * connections. TCP_NODELAY is set here because Linux accepted sockets inherit it: no
 * setsockopt per accept.
 */

/* begin_drain:
 * Stop is set: take nothing new and ask the kernel to end everything in flight, so the loop
 * can keep running until the connections have closed themselves. The accepts are cancelled by
 * name (they must stop even on a kernel without CANCEL_ANY), then one ASYNC_CANCEL with
 * IORING_ASYNC_CANCEL_ANY (5.19+) takes every recv and every send a coroutine is parked on.
 * Their awaits fail with -ECANCELED, the handlers unwind, and conn_close returns the stacks
 * and the fds. A recv parked on -ENOBUFS holds no operation to cancel, so it is ended here by
 * hand.
 *   - nothing re-arms it from here on  [l->stalled = true;]
 *   - its result says whether the kernel knew it  [sqe->user_data    = UD(p, TAG_DRAIN);]
 */

/* pin_to:
 * Pin this thread to the idx-th CPU the process may run on. Reading the inherited affinity
 * mask keeps the mapping right under a non-contiguous cpuset, e.g. a container on 0-31,64-95.
 */

/* fixed_slots:
 * How many registered file slots to ask for: FIXED_FILES, capped by the fd limit the kernel
 * checks the table against. 0 disables the feature.
 */

/* proactor_run:
 * The worker's whole life: setup, the loop until *stop, teardown in dependency order.
 *   - on this thread: DEFER_TASKRUN ties it here  [int rc = uring_init(&p->ring,
 *     p->cfg.ring_entries);]
 *   - optional: enter skips an fd lookup  [uring_register_ring_fd(&p->ring);]
 *   - optional: sockets live in a file table  [unsigned slots = fixed_slots();]
 *   - the ceiling accept_room holds us to  [p->file_slots = slots;]
 *   - truncated: stop growing  [at += (size_t)n < sizeof ports - at ? (size_t)n : sizeof ports
 *     - at - 1;]
 *   - 100 ms: so an idle worker notices *stop  [struct __kernel_timespec wait_at_most = {
 *     .tv_sec = 0, .tv_nsec = 100000000L };]
 *   - from here the stop flag is not read again  [begin_drain(p);]
 *   - one syscall per batch  [rc = uring_submit_wait(&p->ring, 1, &wait_at_most);]
 *   - ioxd_run returns non-zero for it  [p->failed = rc;]
 *   - one ring is gone: retire the others too  [*p->stop  = 1;]
 *   - The batch, one CQE at a time, copied out before the handler runs: the head is published
 *     once at the end - or in ioxd__sqe, ahead of an enter in the middle of the batch, so the
 *     kernel has somewhere to put what that enter completes.  [unsigned ready =
 *     uring_cq_ready(&p->ring);]
 *   - read the tail once  [unsigned ready = uring_cq_ready(&p->ring);]
 *   - handlers run in here  [dispatch(p, &cqe);]
 *   - a close may have made room to accept again  [rearm_stalled(p);]
 *   - The closes the last handlers staged have to reach the kernel before the ring goes, or
 *     their sockets stay open until the process exits.  [ioxd__bufring_publish(&p->bufs);]
 *   - Sockets, then the ring (which cancels every in-flight op and drops its buffer
 *     references), then the memory the kernel could still have referenced.  [for (int i = 0; i
 *     < p->n_listeners; i++)]
 *   - Connections that outlived the grace period may still have a recv the kernel is holding a
 *     buffer for. Unmapping the slab under it would be worse than leaking it at exit.
 *     [fprintf(stderr, "[w%d] %u connections did not close in %d ms: the recv buffers s]
 */

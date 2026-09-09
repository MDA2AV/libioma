/*
 * proactor.h - one worker: one thread, one io_uring, one SO_REUSEPORT listener, one provided
 * buffer ring, and the coroutines that run on it. The connections it serves are io/conn.h. Thread-per-core, shared-nothing: nothing in
 * here is touched by any other thread except the stop flag.
 *
 * The loop: run freshly spawned coroutines, re-arm recvs parked on -ENOBUFS, publish and enter
 * once (submit everything staged, wait for >= 1 completion), then dispatch the CQ batch one entry
 * at a time, publishing the head per entry so the ring keeps room mid-batch. Dispatching resumes
 * handler coroutines inline, so the sends they stage ride the next enter together with the batch.
 *
 * On stop the loop does not simply leave: it cancels the accepts and everything in flight, then
 * keeps running until the last connection has closed itself or a two-second grace period is up,
 * so parked coroutines wake with errors and their handlers unwind before anything is torn down.
 */
#pragma once

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "io/bufring.h"
#include "io/conn.h"
#include "io/coro.h"
#include "io/uring.h"

/* tunables (override with -D) */
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
    void       *tls;                      /* the port's certificate store, or nullptr: plain   */

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
    struct listener        listeners[IOXD_MAX_LISTENERS];   /* port and tls set by the creator  */
    int                    n_listeners;
    handler_fn             handler;
    volatile sig_atomic_t *stop;

    /* owned by the worker thread */
    struct uring              ring;
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

/* The worker thread's whole life: ring, buffers, listener, loop until *stop, teardown. */
void proactor_run(proactor_t *p);

/* Start a coroutine on this worker. Safe from the loop or from any coroutine on it. */
void proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg);

/* Claim an SQE to stage an operation; flushes without waiting when the SQ is full. For the
 * plane's own files: every op goes through here so it rides the loop's next enter. */
struct io_uring_sqe *ioxd__sqe(proactor_t *p);

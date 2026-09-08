/*
 * proactor.h - one worker: one thread, one io_uring, one SO_REUSEPORT listener, one provided
 * buffer ring, and the coroutines that run on it. The connections it serves are io/conn.h. Thread-per-core, shared-nothing: nothing in
 * here is touched by any other thread except the stop flag.
 *
 * The loop: run freshly spawned coroutines, re-arm recvs parked on -ENOBUFS, publish and enter
 * once (submit everything staged, wait for >= 1 completion), dispatch the whole CQ batch, advance
 * the head once. Dispatching resumes handler coroutines inline, so the sends they stage ride the
 * next enter together with the batch.
 */
#pragma once

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "io/conn.h"
#include "io/coro.h"
#include "io/uring.h"

/* tunables (override with -D) */
#ifndef RING_ENTRIES
#define RING_ENTRIES 4096                 /* SQ depth; the CQ is twice that                     */
#endif
#ifndef BUF_COUNT
#define BUF_COUNT    4096                 /* provided recv buffers per worker, power of two     */
#endif
static_assert(((unsigned)BUF_COUNT & ((unsigned)BUF_COUNT - 1U)) == 0 && BUF_COUNT <= 65536, "BUF_COUNT: a power of two, at most 65536 (16-bit buffer ids)");
#ifndef BUF_SIZE
#define BUF_SIZE     2048                 /* bytes per recv buffer (a request rarely needs more) */
#endif
#ifndef STACK_SIZE
#define STACK_SIZE   (64UL * 1024)        /* per coroutine, plus a guard page                   */
#endif
#ifndef FIXED_FILES
#define FIXED_FILES  16384                /* registered file slots per worker; 0 disables       */
#endif

typedef struct proactor proactor_t;
typedef void (*handler_fn)(conn_t *c);

struct proactor {
    /* set by the creator */
    int                    id;
    int                    cpu;           /* pin the thread here; -1 = don't                   */
    uint16_t               port;
    handler_fn             handler;
    volatile sig_atomic_t *stop;

    /* owned by the worker thread */
    struct uring              ring;
    int                       listen_fd;
    struct io_uring_buf_ring *buf_ring;   /* kernel-shared ring of buffer descriptors          */
    uint8_t                  *slab;       /* BUF_COUNT x BUF_SIZE                              */
    unsigned                  buf_tail;   /* local tail, published to buf_ring->tail           */
    bool                      buffers_returned;   /* since the last starved sweep              */
    bool                      buf_dirty;          /* staged buffer returns awaiting one publish */
    conn_t                  **starved;    /* connections parked on -ENOBUFS                    */
    unsigned                  nstarved, cap_starved;
    uint64_t                  starved_total;      /* recvs that found the ring empty, ever       */
    uint64_t                  starved_since_log;  /* ... since the last log line                 */
    time_t                    starved_log_at;     /* the next second a log line may go out       */
    coro_t                   *ready_head, *ready_tail;   /* spawned, not yet started           */
    unsigned                  live;       /* open connections                                  */
    uint64_t                  accepted;
    conn_t                   *conn_free;         /* recycled conn_t objects, reused on accept   */
    unsigned                  conn_free_count;
};

/* The worker thread's whole life: ring, buffers, listener, loop until *stop, teardown. */
void proactor_run(proactor_t *p);

/* Start a coroutine on this worker. Safe from the loop or from any coroutine on it. */
void proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg);

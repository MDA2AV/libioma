/*
 * uring.h - raw io_uring, no liburing. The C twin of ioxide's Ring.cs.
 *
 * Setup with SINGLE_ISSUER | DEFER_TASKRUN | NO_SQARRAY, mmap the rings, claim SQEs against a
 * local tail, publish the tail and enter in one call, drain the CQ as a batch with one barrier
 * per batch. One ring per thread; nothing here is thread-safe and nothing needs to be.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <linux/io_uring.h>

/* Names for the newer bits, in case the build box's headers predate them (the kernel decides at
 * runtime; unsupported features fall back). */
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER    (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN    (1U << 13)
#endif
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY       (1U << 16)
#endif
#ifndef IORING_SQ_CQ_OVERFLOW
#define IORING_SQ_CQ_OVERFLOW         (1U << 1)
#endif
#ifndef IORING_ASYNC_CANCEL_ANY
#define IORING_ASYNC_CANCEL_ANY       (1U << 2)
#endif
/* EXT_ARG and the struct it points at arrived together, so one guard covers both. */
#ifndef IORING_ENTER_EXT_ARG
#define IORING_ENTER_EXT_ARG          (1U << 3)
struct io_uring_getevents_arg {
    uint64_t sigmask;
    uint32_t sigmask_sz;
    uint32_t pad;
    uint64_t ts;
};
#endif
#ifndef IORING_REGISTER_RING_FDS
#define IORING_REGISTER_RING_FDS      20
#define IORING_UNREGISTER_RING_FDS    21
#endif
#ifndef IORING_ENTER_REGISTERED_RING
#define IORING_ENTER_REGISTERED_RING  (1U << 4)
#endif
#ifndef IORING_FILE_INDEX_ALLOC
#define IORING_FILE_INDEX_ALLOC       (~0U)
#endif
#ifndef IORING_RSRC_REGISTER_SPARSE
#define IORING_RSRC_REGISTER_SPARSE   (1U << 0)
#endif

struct uring {
    int      fd;                       /* -1 until both mappings are up (see ioxd__uring_init)      */
    int      enter_fd;                 /* fd, or the registered-ring index (see enter_flags) */
    unsigned enter_flags;              /* 0, or IORING_ENTER_REGISTERED_RING                 */
    bool     fixed_files;              /* a sparse registered file table exists              */

    /* submission side */
    unsigned *sq_head;                 /* kernel-written consumer index            */
    unsigned *sq_tail;                 /* our published producer index             */
    unsigned *sq_array;                /* SQ index array; nullptr under NO_SQARRAY */
    unsigned *sq_flags;                /* kernel-written: IORING_SQ_CQ_OVERFLOW    */
    unsigned  sq_mask, sq_entries;
    unsigned  sqe_tail;                /* local tail: claimed, not yet published   */
    struct io_uring_sqe *sqes;
    bool      has_sq_array;

    /* completion side */
    unsigned *cq_head;                 /* our consumer index                       */
    unsigned *cq_tail;                 /* kernel-written producer index            */
    unsigned  cq_mask;
    struct io_uring_cqe *cqes;
    uint64_t  cq_overflows;            /* enters that found the kernel holding overflowed CQEs */

    /* mappings */
    void  *ring_mem; size_t ring_bytes;
    void  *sqe_mem;  size_t sqe_bytes;
};

/* Create the ring on the calling thread (DEFER_TASKRUN ties the ring to it). 0 or -errno. */
int  ioxd__uring_init(struct uring *ring, unsigned entries);
void ioxd__uring_exit(struct uring *ring);

/* Claim the next SQE, zeroed. nullptr when the SQ is full: submit, then try again. */
struct io_uring_sqe *ioxd__uring_get_sqe(struct uring *ring);

/* Publish claimed SQEs and enter. ioxd__uring_submit never waits; ioxd__uring_submit_wait blocks until
 * wait_nr completions are available or ts (may be nullptr) expires. Return: the number of SQEs the
 * kernel consumed, or -errno. A timeout reads as -ETIME only when there was nothing to submit;
 * with SQEs in hand the kernel returns the count it took and says nothing about the wait, so a
 * non-negative return is not "completions are ready" - always drain the CQ.
 * Under DEFER_TASKRUN only the waiting form reaps completions. */
int  ioxd__uring_submit(struct uring *ring);
int  ioxd__uring_submit_wait(struct uring *ring, unsigned wait_nr, struct __kernel_timespec *ts);

int  ioxd__uring_register(struct uring *ring, unsigned opcode, void *arg, unsigned nr_args);

/* Register the ring's own fd so every enter skips an fd lookup. 0 or -errno (kernel 5.18+). */
int  ioxd__uring_register_ring_fd(struct uring *ring);

/* Create an empty registered file table of n slots, so sockets can live in slots instead of fds:
 * accept lands them there, recv/send/close address them by index. 0 or -errno (kernel 5.19+). */
int  ioxd__uring_register_files_sparse(struct uring *ring, unsigned n);

/* Batched CQ drain: read the tail once, index the batch, publish the head once. */
unsigned ioxd__uring_cq_ready(struct uring *ring);
static inline struct io_uring_cqe *ioxd__uring_cqe_at(struct uring *ring, unsigned i)
{
    return &ring->cqes[(*ring->cq_head + i) & ring->cq_mask];
}
void ioxd__uring_cq_advance(struct uring *ring, unsigned n);

/* ── uring.c: the notes ──────────────────────────────────────────────────────────────────── */

/* sys_setup:
 * io_uring_setup; -errno on failure.
 */

/* sys_enter:
 * io_uring_enter; -errno on failure.
 */

/* ioxd__uring_register:
 * io_uring_register (buffer rings, files, ...); the result, or -errno.
 */

/* ring_clear:
 * The struct owning nothing: no descriptor, no mapping. ioxd__uring_exit over this unmaps nothing
 * and closes nothing, which is what a failed init has to leave behind.
 */

/* ring_failed:
 * Give up on a half-built ring: empty the struct and hand back the error.
 */

/* ioxd__uring_init:
 * Create the ring and mmap both rings plus the SQE array. 0, or -errno. On any failure the
 * struct is left empty, so a caller that calls ioxd__uring_exit anyway unmaps nothing and closes
 * nothing.
 *   - SINGLE_ISSUER: only this thread submits, the kernel skips SQ locking. DEFER_TASKRUN:
 *     completion work runs batched inside enter(GETEVENTS), never as an interrupt. NO_SQARRAY
 *     (6.6+): slot i of the SQ ring is SQE i, one store fewer per submission.  [struct
 *     io_uring_params params;]
 *   - maybe NO_SQARRAY, maybe not: try without it  [if (fd == -EINVAL) {]
 *   - the first error was the real one  [return ring_failed(ring, -EINVAL);]
 *   - every kernel since 5.4  [if (!(params.features & IORING_FEAT_SINGLE_MMAP)) {]
 *   - One mapping holds both rings; size it for whichever ends later. Without the SQ index
 *     array sq_off.array is 0, so the SQ side ends after the last of its header words.
 *     [size_t sq_bytes = sq_array ? params.sq_off.array + (size_t)params.sq_entries * s]
 *   - Everything is up: only now does the struct own an fd and two mappings.  [ring->fd
 *     = fd;]
 */

/* ioxd__uring_register_ring_fd:
 * Register the ring fd in the task's ring table; enter then uses the index and skips the
 * lookup.
 *   - any free index  [up.offset = (uint32_t)-1;]
 *   - the count registered: up.offset is only ours then  [if (rc != 1)]
 */

/* ioxd__uring_register_files_sparse:
 * A sparse file table: n empty slots the kernel fills on direct accept.
 */

/* ioxd__uring_exit:
 * Unmap and close the ring; the kernel cancels anything still in flight and drops the tables.
 *   - fd 0 is a legal descriptor  [if (ring->fd >= 0)  close(ring->fd);]
 */

/* ioxd__uring_get_sqe:
 * Claim the next SQE against the local tail, zeroed. nullptr when the SQ is full.
 *   - full: the caller flushes and retries  [return nullptr;]
 */

/* flush_and_enter:
 * Publish the local tail and enter.
 *   - Count against the kernel-consumed head, so SQEs an -EBUSY enter left unconsumed are
 *     re-counted by the next call instead of stranding (liburing's accounting, ioxide's too).
 *     [unsigned khead = load_acquire(ring->sq_head);]
 *   - The CQ filled and the kernel is holding the rest in its overflow list, where nothing we
 *     do to the ring will find them. Only an enter flushes that backlog, so make one even when
 *     there is nothing to submit and nothing to wait for.  [if (load_acquire(ring->sq_flags) &
 *     IORING_SQ_CQ_OVERFLOW) {]
 */

/* ioxd__uring_submit:
 * Submit everything claimed; never waits.
 */

/* ioxd__uring_submit_wait:
 * Submit, then wait for wait_nr completions or until ts expires (nullptr: no timeout).
 */

/* ioxd__uring_cq_ready:
 * How many CQEs are waiting; reads the kernel's tail once.
 */

/* ioxd__uring_cq_advance:
 * Release n consumed CQEs; publishes the head once.
 *   - nothing consumed: no store, no barrier  [if (n == 0)]
 */

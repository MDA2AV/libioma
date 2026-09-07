/*
 * uring.h - raw io_uring, no liburing. The C twin of ioxide's Ring.cs.
 *
 * Setup with SINGLE_ISSUER | DEFER_TASKRUN | NO_SQARRAY, mmap the rings, claim SQEs against a
 * local tail, publish the tail and enter in one call, drain the CQ as a batch with one barrier
 * per batch. One ring per thread; nothing here is thread-safe and nothing needs to be.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/io_uring.h>

/* Names for the newer bits, in case the build box's headers predate them (the kernel decides at
 * runtime; unsupported features fall back). */
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
    int      fd;
    int      enter_fd;                 /* fd, or the registered-ring index (see enter_flags) */
    unsigned enter_flags;              /* 0, or IORING_ENTER_REGISTERED_RING                 */
    bool     fixed_files;              /* a sparse registered file table exists              */

    /* submission side */
    unsigned *sq_head;                 /* kernel-written consumer index            */
    unsigned *sq_tail;                 /* our published producer index             */
    unsigned *sq_array;                /* SQ index array; unused under NO_SQARRAY  */
    unsigned  sq_mask, sq_entries;
    unsigned  sqe_tail;                /* local tail: claimed, not yet published   */
    struct io_uring_sqe *sqes;
    bool      has_sq_array;

    /* completion side */
    unsigned *cq_head;                 /* our consumer index                       */
    unsigned *cq_tail;                 /* kernel-written producer index            */
    unsigned  cq_mask;
    struct io_uring_cqe *cqes;

    /* mappings */
    void  *ring_mem; size_t ring_bytes;
    void  *sqe_mem;  size_t sqe_bytes;
};

/* Create the ring on the calling thread (DEFER_TASKRUN ties the ring to it). 0 or -errno. */
int  uring_init(struct uring *r, unsigned entries);
void uring_exit(struct uring *r);

/* Claim the next SQE, zeroed. NULL when the SQ is full: submit, then try again. */
struct io_uring_sqe *uring_get_sqe(struct uring *r);

/* Publish claimed SQEs and enter. uring_submit never waits; uring_submit_wait blocks until
 * wait_nr completions are available or ts (may be NULL) expires. Return: submitted count or
 * -errno (-ETIME on timeout). Under DEFER_TASKRUN only the waiting form reaps completions. */
int  uring_submit(struct uring *r);
int  uring_submit_wait(struct uring *r, unsigned wait_nr, struct __kernel_timespec *ts);

int  uring_register(struct uring *r, unsigned opcode, void *arg, unsigned nr_args);

/* Register the ring's own fd so every enter skips an fd lookup. 0 or -errno (kernel 5.18+). */
int  uring_register_ring_fd(struct uring *r);

/* Create an empty registered file table of n slots, so sockets can live in slots instead of fds:
 * accept lands them there, recv/send/close address them by index. 0 or -errno (kernel 5.19+). */
int  uring_register_files_sparse(struct uring *r, unsigned n);

/* Batched CQ drain: read the tail once, index the batch, publish the head once. */
unsigned uring_cq_ready(struct uring *r);
static inline struct io_uring_cqe *uring_cqe_at(struct uring *r, unsigned i)
{
    return &r->cqes[(*r->cq_head + i) & r->cq_mask];
}
void uring_cq_advance(struct uring *r, unsigned n);

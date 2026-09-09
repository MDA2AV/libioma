#include "io/uring.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define load_acquire(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* io_uring_setup; -errno on failure. */
static int sys_setup(unsigned entries, struct io_uring_params *p)
{
    long rc = syscall(SYS_io_uring_setup, entries, p);
    return rc < 0 ? -errno : (int)rc;
}

/* io_uring_enter; -errno on failure. */
static int sys_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags,
                     const void *arg, size_t argsz)
{
    long rc = syscall(SYS_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
    return rc < 0 ? -errno : (int)rc;
}

/* io_uring_register (buffer rings, files, ...); the result, or -errno. */
int uring_register(struct uring *ring, unsigned opcode, void *arg, unsigned nr_args)
{
    long rc = syscall(SYS_io_uring_register, ring->fd, opcode, arg, nr_args);
    return rc < 0 ? -errno : (int)rc;
}

/* The struct owning nothing: no descriptor, no mapping. uring_exit over this unmaps nothing and
 * closes nothing, which is what a failed init has to leave behind. */
static void ring_clear(struct uring *ring)
{
    memset(ring, 0, sizeof *ring);
    ring->fd = ring->enter_fd = -1;
}

/* Give up on a half-built ring: empty the struct and hand back the error. */
static int ring_failed(struct uring *ring, int err)
{
    ring_clear(ring);
    return err;
}

/* Create the ring and mmap both rings plus the SQE array. 0, or -errno. On any failure the struct
 * is left empty, so a caller that calls uring_exit anyway unmaps nothing and closes nothing. */
int uring_init(struct uring *ring, unsigned entries)
{
    ring_clear(ring);

    /* SINGLE_ISSUER: only this thread submits, the kernel skips SQ locking.
     * DEFER_TASKRUN: completion work runs batched inside enter(GETEVENTS), never as an interrupt.
     * NO_SQARRAY (6.6+): slot i of the SQ ring is SQE i, one store fewer per submission. */
    struct io_uring_params params;
    memset(&params, 0, sizeof params);
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_NO_SQARRAY;
    int  fd       = sys_setup(entries, &params);
    bool sq_array = false;
    if (fd == -EINVAL) {                             /* maybe NO_SQARRAY, maybe not: try without it */
        memset(&params, 0, sizeof params);
        params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
        fd = sys_setup(entries, &params);
        if (fd < 0)
            return ring_failed(ring, -EINVAL);       /* the first error was the real one */
        sq_array = true;
    }
    if (fd < 0)
        return ring_failed(ring, fd);
    if (!(params.features & IORING_FEAT_SINGLE_MMAP)) {   /* every kernel since 5.4 */
        close(fd);
        return ring_failed(ring, -ENOSYS);
    }

    /* One mapping holds both rings; size it for whichever ends later. Without the SQ index array
     * sq_off.array is 0, so the SQ side ends after the last of its header words. */
    size_t sq_bytes = sq_array ? params.sq_off.array + (size_t)params.sq_entries * sizeof(unsigned)
                               : params.sq_off.dropped + sizeof(unsigned);
    size_t cq_bytes = params.cq_off.cqes  + (size_t)params.cq_entries * sizeof(struct io_uring_cqe);
    size_t ring_bytes = sq_bytes > cq_bytes ? sq_bytes : cq_bytes;
    void  *ring_mem = mmap(nullptr, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                           fd, IORING_OFF_SQ_RING);
    if (ring_mem == MAP_FAILED) {
        int e = -errno;
        close(fd);
        return ring_failed(ring, e);
    }

    size_t sqe_bytes = (size_t)params.sq_entries * sizeof(struct io_uring_sqe);
    void  *sqe_mem = mmap(nullptr, sqe_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          fd, IORING_OFF_SQES);
    if (sqe_mem == MAP_FAILED) {
        int e = -errno;
        munmap(ring_mem, ring_bytes);
        close(fd);
        return ring_failed(ring, e);
    }

    /* Everything is up: only now does the struct own an fd and two mappings. */
    ring->fd         = fd;
    ring->enter_fd   = fd;
    ring->sq_entries = params.sq_entries;
    ring->has_sq_array = sq_array;
    ring->ring_mem   = ring_mem;
    ring->ring_bytes = ring_bytes;
    ring->sqe_mem    = sqe_mem;
    ring->sqe_bytes  = sqe_bytes;

    char *base = ring_mem;
    ring->sq_head  = (unsigned *)(base + params.sq_off.head);
    ring->sq_tail  = (unsigned *)(base + params.sq_off.tail);
    ring->sq_array = sq_array ? (unsigned *)(base + params.sq_off.array) : nullptr;
    ring->sq_flags = (unsigned *)(base + params.sq_off.flags);
    ring->sq_mask  = *(unsigned *)(base + params.sq_off.ring_mask);
    ring->sqes     = sqe_mem;

    ring->cq_head  = (unsigned *)(base + params.cq_off.head);
    ring->cq_tail  = (unsigned *)(base + params.cq_off.tail);
    ring->cq_mask  = *(unsigned *)(base + params.cq_off.ring_mask);
    ring->cqes     = (struct io_uring_cqe *)(base + params.cq_off.cqes);
    return 0;
}

/* Register the ring fd in the task's ring table; enter then uses the index and skips the lookup. */
int uring_register_ring_fd(struct uring *ring)
{
    struct io_uring_rsrc_update up;
    memset(&up, 0, sizeof up);
    up.offset = (uint32_t)-1;                          /* any free index */
    up.data   = (uint64_t)ring->fd;
    int rc = uring_register(ring, IORING_REGISTER_RING_FDS, &up, 1);
    if (rc < 0)
        return rc;
    if (rc != 1)                                       /* the count registered: up.offset is only ours then */
        return -ENOSPC;
    ring->enter_fd    = (int)up.offset;
    ring->enter_flags = IORING_ENTER_REGISTERED_RING;
    return 0;
}

/* A sparse file table: n empty slots the kernel fills on direct accept. */
int uring_register_files_sparse(struct uring *ring, unsigned n)
{
    struct io_uring_rsrc_register reg;
    memset(&reg, 0, sizeof reg);
    reg.nr    = n;
    reg.flags = IORING_RSRC_REGISTER_SPARSE;
    int rc = uring_register(ring, IORING_REGISTER_FILES2, &reg, sizeof reg);
    if (rc < 0)
        return rc;
    ring->fixed_files = true;
    return 0;
}

/* Unmap and close the ring; the kernel cancels anything still in flight and drops the tables. */
void uring_exit(struct uring *ring)
{
    if (ring->enter_flags & IORING_ENTER_REGISTERED_RING) {
        struct io_uring_rsrc_update up;
        memset(&up, 0, sizeof up);
        up.offset = (uint32_t)ring->enter_fd;
        uring_register(ring, IORING_UNREGISTER_RING_FDS, &up, 1);
    }
    if (ring->sqe_mem)  munmap(ring->sqe_mem, ring->sqe_bytes);
    if (ring->ring_mem) munmap(ring->ring_mem, ring->ring_bytes);
    if (ring->fd >= 0)  close(ring->fd);              /* fd 0 is a legal descriptor */
    ring_clear(ring);
}

/* Claim the next SQE against the local tail, zeroed. nullptr when the SQ is full. */
struct io_uring_sqe *uring_get_sqe(struct uring *ring)
{
    unsigned head = load_acquire(ring->sq_head);
    if (ring->sqe_tail - head >= ring->sq_entries)
        return nullptr;                                   /* full: the caller flushes and retries */

    unsigned slot = ring->sqe_tail & ring->sq_mask;
    if (ring->has_sq_array)
        ring->sq_array[slot] = slot;
    ring->sqe_tail++;

    struct io_uring_sqe *sqe = &ring->sqes[slot];
    memset(sqe, 0, sizeof *sqe);
    return sqe;
}

/* Publish the local tail and enter. */
static int flush_and_enter(struct uring *ring, unsigned wait_nr, unsigned flags,
                           const void *arg, size_t argsz)
{
    /* Count against the kernel-consumed head, so SQEs an -EBUSY enter left unconsumed are
     * re-counted by the next call instead of stranding (liburing's accounting, ioxide's too). */
    unsigned khead = load_acquire(ring->sq_head);
    unsigned to_submit = ring->sqe_tail - khead;

    if (*ring->sq_tail != ring->sqe_tail)
        store_release(ring->sq_tail, ring->sqe_tail);

    /* The CQ filled and the kernel is holding the rest in its overflow list, where nothing we do
     * to the ring will find them. Only an enter flushes that backlog, so make one even when there
     * is nothing to submit and nothing to wait for. */
    if (load_acquire(ring->sq_flags) & IORING_SQ_CQ_OVERFLOW) {
        ring->cq_overflows++;
        flags |= IORING_ENTER_GETEVENTS;
    }

    if (to_submit == 0 && wait_nr == 0 && !(flags & IORING_ENTER_GETEVENTS))
        return 0;
    return sys_enter(ring->enter_fd, to_submit, wait_nr, flags | ring->enter_flags, arg, argsz);
}

/* Submit everything claimed; never waits. */
int uring_submit(struct uring *ring)
{
    return flush_and_enter(ring, 0, 0, nullptr, 0);
}

/* Submit, then wait for wait_nr completions or until ts expires (nullptr: no timeout). */
int uring_submit_wait(struct uring *ring, unsigned wait_nr, struct __kernel_timespec *ts)
{
    if (!ts)
        return flush_and_enter(ring, wait_nr, IORING_ENTER_GETEVENTS, nullptr, 0);

    struct io_uring_getevents_arg arg;
    memset(&arg, 0, sizeof arg);
    arg.ts = (uint64_t)(uintptr_t)ts;
    return flush_and_enter(ring, wait_nr, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                           &arg, sizeof arg);
}

/* How many CQEs are waiting; reads the kernel's tail once. */
unsigned uring_cq_ready(struct uring *ring)
{
    return load_acquire(ring->cq_tail) - *ring->cq_head;
}

/* Release n consumed CQEs; publishes the head once. */
void uring_cq_advance(struct uring *ring, unsigned n)
{
    if (n == 0)                                       /* nothing consumed: no store, no barrier */
        return;
    store_release(ring->cq_head, *ring->cq_head + n);
}

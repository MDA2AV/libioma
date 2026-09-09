/*
 * proactor.c - the worker: pin to a CPU, own a ring, a buffer ring and a listener, then loop:
 * start spawned coroutines, enter once per batch, dispatch every completion. Connections live in
 * conn.c and buffers in bufring.c; this file is the loop and what feeds it.
 */
#include "io/internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DRAIN_GRACE_MS 2000                          /* how long a stopping worker waits for its connections */

/* Coarse monotonic milliseconds: for the drain deadline and the once-a-second retries, never for
 * anything that needs better than a tick. */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* ── submission ────────────────────────────────────────────────────────────────────────── */

/* Claim an SQE. If the SQ is full mid-batch, submit what is staged and retry. -EBUSY means the
 * kernel is holding completions it could not fit in the CQ and will not take more submissions, so
 * the retry enters with GETEVENTS to flush them: safe mid-batch, since the loop dispatches whatever
 * lands next time round. */
/* Publish the CQ head for the entries taken so far: once per batch in the loop, and before any
 * enter in the middle of one, so the kernel has room for what the enter completes. */
static void publish_cq(proactor_t *p)
{
    if (p->cq_taken) {
        uring_cq_advance(&p->ring, p->cq_taken);
        p->cq_taken = 0;
    }
}

struct io_uring_sqe *ioxd__sqe(proactor_t *p)
{
    struct io_uring_sqe *sqe = uring_get_sqe(&p->ring);
    int rc = 0;
    for (int i = 0; !sqe && i < 16; i++) {
        ioxd__bufring_publish(&p->bufs);             /* the kernel must see staged returns before this enter */
        publish_cq(p);                               /* and have room in the CQ for what it completes */
        rc = uring_submit(&p->ring);
        if (rc == -EBUSY || rc == -EAGAIN || rc == -EINTR)
            rc = uring_submit_wait(&p->ring, 0, nullptr);   /* reap without waiting; the CQ has room again */
        sqe = uring_get_sqe(&p->ring);
    }
    if (!sqe) {
        fprintf(stderr, "[w%d] SQ still full after flushing: %s\n",
                p->id, rc < 0 ? ioxd__errstr(-rc) : "the kernel is not consuming it");
        abort();
    }
    return sqe;
}

/* ── accept ────────────────────────────────────────────────────────────────────────────── */

/* Whether this worker may take another connection. Under registered files a socket lives in a
 * slot, so the table is a hard ceiling: better to stop arming than to fail one accept per arrival. */
static bool accept_room(const proactor_t *p)
{
    return !p->draining && (p->file_slots == 0 || p->live < p->file_slots);
}

/* Leave the accept unarmed until there is room again. */
static void stall_accept(struct listener *l)
{
    l->stalled      = true;
    l->stalled_live = l->p->live;
    l->retry_at     = (time_t)(now_ms() / 1000U) + 1;
}

/* Arm the multishot accept: one SQE, then a CQE per new connection. */
static void arm_accept(struct listener *l)
{
    if (!accept_room(l->p)) {
        stall_accept(l);
        return;
    }
    l->stalled = false;
    struct io_uring_sqe *sqe = ioxd__sqe(l->p);
    sqe->opcode    = IORING_OP_ACCEPT;
    sqe->fd        = l->fd;
    sqe->ioprio    = IORING_ACCEPT_MULTISHOT;
    sqe->user_data = UD(l, TAG_ACCEPT);
    if (l->p->ring.fixed_files)
        sqe->file_index = IORING_FILE_INDEX_ALLOC;   /* land each socket in a free slot, not an fd */
}

/* Re-arm a listener that stalled: once a connection has closed (p->live below what it was), and
 * once a second regardless, since the shortage may be another thread's and no close of ours would
 * ever announce it. Only ever a handful of listeners, and only after an accept ran out of room. */
static void rearm_stalled(proactor_t *p)
{
    bool any = false;
    for (int i = 0; i < p->n_listeners; i++)
        any |= p->listeners[i].stalled;
    if (!any)
        return;

    time_t now = (time_t)(now_ms() / 1000U);
    for (int i = 0; i < p->n_listeners; i++) {
        struct listener *l = &p->listeners[i];
        if (l->stalled && (p->live < l->stalled_live || now >= l->retry_at))
            arm_accept(l);
    }
}

/* Out of descriptors, slots or memory: the next accept would fail the same way. */
static bool accept_exhausted(int err)
{
    return err == -ENFILE || err == -EMFILE || err == -ENOMEM || err == -ENOBUFS;
}

/* Log an accept error at most once a second per listener, with how many it stands for: a full file
 * table would otherwise write one line per arrival, forever. */
static void note_accept_error(struct listener *l, int result)
{
    time_t now = (time_t)(now_ms() / 1000U);
    l->err_since_log++;
    if (now < l->err_log_at)
        return;
    fprintf(stderr, "[w%d] accept :%u: %s (%llu since the last line)\n",
            l->p->id, l->port, ioxd__errstr(-result), (unsigned long long)l->err_since_log);
    l->err_since_log = 0;
    l->err_log_at    = now + 1;
}

/* An accept CQE: wrap the new fd in a conn, arm its recv, spawn its handler coroutine. */
static void on_accept(struct listener *l, int result, unsigned flags)
{
    proactor_t *p = l->p;
    trace("[w%d] accept :%u result=%d more=%d\n", p->id, l->port, result, !!(flags & IORING_CQE_F_MORE));
    if (result >= 0 && p->draining) {
        ioxd__close_socket(p, result);               /* accepted just before the cancel: no new work */
    } else if (result >= 0) {
        conn_t *c = ioxd__conn_new(p, l, result);      /* TCP_NODELAY came with the listener */
        ioxd__arm_recv(p, c);
        proactor_spawn(p, ioxd__conn_main, c);
        p->accepted++;
    } else if (!(p->draining && result == -ECANCELED)) {   /* our own shutdown cancel is not an error */
        note_accept_error(l, result);
    }
    if (flags & IORING_CQE_F_MORE)                   /* still armed: nothing to do */
        return;
    if (result < 0 && accept_exhausted(result)) {
        stall_accept(l);                             /* rearm_stalled picks it up when there is room */
        return;
    }
    arm_accept(l);
}

/* ── completions ───────────────────────────────────────────────────────────────────────── */

/* Route one CQE by the tag in its user_data. Handler coroutines resume inline from here. */
static void dispatch(proactor_t *p, struct io_uring_cqe *cqe)
{
    void *ptr = UD_PTR(cqe->user_data);
    switch (UD_TAG(cqe->user_data)) {
    case TAG_OP: {
        op_t *op  = ptr;
        op->res   = cqe->res;
        op->flags = cqe->flags;
        trace("[w%d] op res=%d flags=%#x\n", p->id, cqe->res, cqe->flags);
        coro_resume(op->waiter);                     /* to its next await; op may be gone after */
        break;
    }
    case TAG_RECV:
        ioxd__on_recv(p, ptr, cqe->res, cqe->flags);
        break;
    case TAG_ACCEPT:
        on_accept(ptr, cqe->res, cqe->flags);
        break;
    case TAG_CLOSE:                                  /* a failed close leaks a slot: say so */
        if (cqe->res < 0)
            fprintf(stderr, "[w%d] close: %s\n", p->id, ioxd__errstr(-cqe->res));
        break;
    case TAG_DRAIN:                                  /* the shutdown's one blanket cancel */
        if (cqe->res < 0 && cqe->res != -ENOENT) {
            fprintf(stderr, "[w%d] cancel all: %s; cancelling connections one at a time\n",
                    p->id, ioxd__errstr(-cqe->res));
            p->cancel_each = true;
        }
        break;
    default:                                         /* TAG_IGNORE: cancel acknowledgements */
        break;
    }
}

/* ── scheduling ────────────────────────────────────────────────────────────────────────── */

/* Queue a new coroutine; the loop starts it on its next iteration. */
void proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg)
{
    coro_t *c = coro_create(fn, arg, p->cfg.stack_size);
    if (p->ready_tail)
        p->ready_tail->next = c;
    else
        p->ready_head = c;
    p->ready_tail = c;
}

/* Start every coroutine spawned since the last iteration. */
static void run_ready(proactor_t *p)
{
    while (p->ready_head) {
        coro_t *c = p->ready_head;
        p->ready_head = c->next;
        if (!p->ready_head)
            p->ready_tail = nullptr;
        c->next = nullptr;
        coro_resume(c);
    }
}

/* Re-arm recvs parked on -ENOBUFS, at most one per buffer that actually came back since the last
 * sweep: re-arming the whole list on a single return would send them all back to the empty ring.
 * Oldest first, so a connection parked early is not starved by later ones. */
static void rearm_starved(proactor_t *p)
{
    unsigned room = p->bufs.returned;
    p->bufs.returned = 0;                            /* only this round's returns count as room */
    if (p->nstarved == 0 || room == 0)
        return;

    unsigned n = room < p->nstarved ? room : p->nstarved;
    for (unsigned i = 0; i < n; i++)
        ioxd__arm_recv(p, p->starved[i]);            /* keeps the ref it already holds */
    p->nstarved -= n;
    memmove(p->starved, p->starved + n, p->nstarved * sizeof *p->starved);
}

/* ── listener ──────────────────────────────────────────────────────────────────────────── */

/* A SO_REUSEPORT socket on port; every worker opens its own, so the kernel spreads connections. TCP_NODELAY
 * is set here because Linux accepted sockets inherit it: no setsockopt per accept. */
static int listener_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        abort();
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {   /* NOLINT(readability-trailing-comma): glibc's transparent-union sockaddr argument trips the check */
        perror("bind");
        abort();
    }
    if (listen(fd, 1024) < 0) {
        perror("listen");
        abort();
    }
    return fd;
}

/* ── shutdown ──────────────────────────────────────────────────────────────────────────── */

/* Stop is set: take nothing new and ask the kernel to end everything in flight, so the loop can
 * keep running until the connections have closed themselves. The accepts are cancelled by name
 * (they must stop even on a kernel without CANCEL_ANY), then one ASYNC_CANCEL with
 * IORING_ASYNC_CANCEL_ANY (5.19+) takes every recv and every send a coroutine is parked on. Their
 * awaits fail with -ECANCELED, the handlers unwind, and conn_close returns the stacks and the fds.
 * A recv parked on -ENOBUFS holds no operation to cancel, so it is ended here by hand. */
static void begin_drain(proactor_t *p)
{
    p->draining = true;
    for (int i = 0; i < p->n_listeners; i++) {
        struct listener *l = &p->listeners[i];
        if (!l->stalled) {
            struct io_uring_sqe *sqe = ioxd__sqe(p);
            sqe->opcode    = IORING_OP_ASYNC_CANCEL;
            sqe->fd        = -1;
            sqe->addr      = UD(l, TAG_ACCEPT);
            sqe->user_data = TAG_IGNORE;
        }
        l->stalled = true;                           /* nothing re-arms it from here on */
    }

    struct io_uring_sqe *sqe = ioxd__sqe(p);
    sqe->opcode       = IORING_OP_ASYNC_CANCEL;
    sqe->fd           = -1;
    sqe->cancel_flags = IORING_ASYNC_CANCEL_ANY;
    sqe->user_data    = UD(p, TAG_DRAIN);            /* its result says whether the kernel knew it */

    while (p->nstarved)
        ioxd__recv_drain(p->starved[--p->nstarved]);
}

/* ── the loop ──────────────────────────────────────────────────────────────────────────── */

/* Pin this thread to the idx-th CPU the process may run on. Reading the inherited affinity mask
 * keeps the mapping right under a non-contiguous cpuset, e.g. a container on 0-31,64-95. */
static void pin_to(int idx)
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed) != 0)
        return;

    int count = CPU_COUNT(&allowed);
    if (count <= 0)
        return;

    int target = idx % count;
    int seen = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed))
            continue;
        if (seen == target) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            pthread_setaffinity_np(pthread_self(), sizeof one, &one);
            return;
        }
        seen++;
    }
}

/* How many registered file slots to ask for: FIXED_FILES, capped by the fd limit the kernel
 * checks the table against. 0 disables the feature. */
static unsigned fixed_slots(void)
{
    struct rlimit rl;
    unsigned n = FIXED_FILES;
    if (n && getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < n)
        n = (unsigned)rl.rlim_cur;
    return n;
}

/* The worker's whole life: setup, the loop until *stop, teardown in dependency order. */
void proactor_run(proactor_t *p)
{
    if (p->cpu >= 0)
        pin_to(p->cpu);

    coro_pool_limit(p->cfg.idle_stacks);
    int rc = uring_init(&p->ring, p->cfg.ring_entries);   /* on this thread: DEFER_TASKRUN ties it here */
    if (rc < 0) {
        fprintf(stderr, "[w%d] io_uring_setup: %s%s\n", p->id, ioxd__errstr(-rc),
                rc == -EPERM ? " (io_uring is disabled: see /proc/sys/kernel/io_uring_disabled)" : "");
        abort();
    }
#ifndef NO_REG_RING
    uring_register_ring_fd(&p->ring);                /* optional: enter skips an fd lookup      */
#endif

    unsigned slots = fixed_slots();                  /* optional: sockets live in a file table  */
    if (slots && uring_register_files_sparse(&p->ring, slots) == 0)
        p->file_slots = slots;                       /* the ceiling accept_room holds us to     */
    ioxd__bufring_init(&p->bufs, &p->ring, p->id, p->cfg.recv_buffers, p->cfg.recv_buffer_size);
    char   ports[IOXD_MAX_LISTENERS * 12] = "";
    size_t at = 0;
    for (int i = 0; i < p->n_listeners; i++) {
        struct listener *l = &p->listeners[i];
        l->p  = p;
        l->fd = listener_open(l->port);
        arm_accept(l);
        int n = snprintf(ports + at, sizeof ports - at, "%s:%u%s", i ? " " : "", l->port, l->certs ? "/tls" : "");
        if (n < 0)
            break;
        at += (size_t)n < sizeof ports - at ? (size_t)n : sizeof ports - at - 1;   /* truncated: stop growing */
    }
    fprintf(stderr, "[w%d] listening on %s (cpu %d, %u x %u B recv buffers, ring %u%s%s%s)\n",
            p->id, ports, p->cpu, p->bufs.count, p->bufs.size, p->ring.sq_entries,
            p->ring.has_sq_array ? "" : ", no sqarray",
            p->ring.enter_flags ? ", registered ring" : "",
            p->ring.fixed_files ? ", fixed files" : "");

    struct __kernel_timespec wait_at_most = { .tv_sec = 0, .tv_nsec = 100000000L };   /* 100 ms: so an idle worker notices *stop */
    uint64_t deadline = 0;
    for (;;) {
        if (*p->stop && !p->draining) {
            begin_drain(p);                          /* from here the stop flag is not read again */
            deadline = now_ms() + DRAIN_GRACE_MS;
        }
        if (p->draining && (p->live == 0 || now_ms() >= deadline))
            break;

        run_ready(p);
        rearm_starved(p);
        ioxd__bufring_publish(&p->bufs);

        rc = uring_submit_wait(&p->ring, 1, &wait_at_most);    /* one syscall per batch */
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY) {
            fprintf(stderr, "[w%d] io_uring_enter: %s\n", p->id, ioxd__errstr(-rc));
            p->failed = rc;                          /* ioxd_run returns non-zero for it        */
            *p->stop  = 1;                           /* one ring is gone: retire the others too */
            break;
        }

        /* The batch, one CQE at a time, copied out before the handler runs: the head is published
         * once at the end - or in ioxd__sqe, ahead of an enter in the middle of the batch, so the
         * kernel has somewhere to put what that enter completes. */
        unsigned ready = uring_cq_ready(&p->ring);   /* read the tail once */
        for (unsigned i = 0; i < ready; i++) {
            struct io_uring_cqe cqe = *uring_cqe_at(&p->ring, p->cq_taken);
            p->cq_taken++;
            dispatch(p, &cqe);                       /* handlers run in here */
        }
        publish_cq(p);
        rearm_stalled(p);                            /* a close may have made room to accept again */
    }

    /* The closes the last handlers staged have to reach the kernel before the ring goes, or their
     * sockets stay open until the process exits. */
    ioxd__bufring_publish(&p->bufs);
    uring_submit(&p->ring);

    fprintf(stderr, "[w%d] stopping: %llu accepted, %u still open, %llu cq overflows\n",
            p->id, (unsigned long long)p->accepted, p->live,
            (unsigned long long)p->ring.cq_overflows);

    /* Sockets, then the ring (which cancels every in-flight op and drops its buffer references),
     * then the memory the kernel could still have referenced. */
    for (int i = 0; i < p->n_listeners; i++)
        close(p->listeners[i].fd);
    ioxd__bufring_unregister(&p->bufs, &p->ring);
    uring_exit(&p->ring);
    if (p->live == 0) {
        ioxd__bufring_unmap(&p->bufs);
    } else {
        /* Connections that outlived the grace period may still have a recv the kernel is holding
         * a buffer for. Unmapping the slab under it would be worse than leaking it at exit. */
        fprintf(stderr, "[w%d] %u connections did not close in %d ms: the recv buffers stay mapped\n",
                p->id, p->live, DRAIN_GRACE_MS);
    }
    free(p->starved);
    p->starved  = nullptr;
    p->nstarved = p->cap_starved = 0;
    ioxd__conn_pool_drain(p);
    coro_pool_drain();
}

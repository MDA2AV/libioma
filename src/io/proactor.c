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
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── submission ────────────────────────────────────────────────────────────────────────── */

/* Claim an SQE. If the SQ is full mid-batch, flush without waiting and retry. */
struct io_uring_sqe *ioma__sqe(proactor_t *p)
{
    struct io_uring_sqe *sqe = uring_get_sqe(&p->ring);
    for (int i = 0; !sqe && i < 16; i++) {
        uring_submit(&p->ring);
        sqe = uring_get_sqe(&p->ring);
    }
    if (!sqe) {
        fprintf(stderr, "[w%d] SQ still full after flushing\n", p->id);
        abort();
    }
    return sqe;
}

/* ── accept ────────────────────────────────────────────────────────────────────────────── */

/* Arm the multishot accept: one SQE, then a CQE per new connection. */
static void arm_accept(proactor_t *p)
{
    struct io_uring_sqe *sqe = ioma__sqe(p);
    sqe->opcode    = IORING_OP_ACCEPT;
    sqe->fd        = p->listen_fd;
    sqe->ioprio    = IORING_ACCEPT_MULTISHOT;
    sqe->user_data = UD(p, TAG_ACCEPT);
    if (p->ring.fixed_files)
        sqe->file_index = IORING_FILE_INDEX_ALLOC;   /* land each socket in a free slot, not an fd */
}

/* An accept CQE: wrap the new fd in a conn, arm its recv, spawn its handler coroutine. */
static void on_accept(proactor_t *p, int result, unsigned flags)
{
    trace("[w%d] accept result=%d more=%d\n", p->id, result, !!(flags & IORING_CQE_F_MORE));
    if (result >= 0) {
        conn_t *c = ioma__conn_new(p, result);         /* TCP_NODELAY came with the listener */
        ioma__arm_recv(p, c);
        proactor_spawn(p, ioma__conn_main, c);
        p->accepted++;
    } else {
        fprintf(stderr, "[w%d] accept: %s\n", p->id, strerror(-result));
    }
    if (!(flags & IORING_CQE_F_MORE))
        arm_accept(p);
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
        ioma__on_recv(p, ptr, cqe->res, cqe->flags);
        break;
    case TAG_ACCEPT:
        on_accept(p, cqe->res, cqe->flags);
        break;
    default:                                         /* TAG_IGNORE: cancel acknowledgements */
        break;
    }
}

/* ── scheduling ────────────────────────────────────────────────────────────────────────── */

/* Queue a new coroutine; the loop starts it on its next iteration. */
void proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg)
{
    coro_t *c = coro_create(fn, arg, STACK_SIZE);
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

/* Re-arm every recv parked on -ENOBUFS, but only once a buffer actually came back, so a parked
 * connection never spins the loop. */
static void rearm_starved(proactor_t *p)
{
    if (p->nstarved == 0 || !p->bufs.returned)
        return;
    p->bufs.returned = false;
    for (unsigned i = 0; i < p->nstarved; i++)
        ioma__arm_recv(p, p->starved[i]);            /* keeps the ref it already holds */
    p->nstarved = 0;
}

/* ── listener ──────────────────────────────────────────────────────────────────────────── */

/* A SO_REUSEPORT listener on port, one per worker so the kernel spreads connections. TCP_NODELAY
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

    int rc = uring_init(&p->ring, RING_ENTRIES);     /* on this thread: DEFER_TASKRUN ties it here */
    if (rc < 0) {
        fprintf(stderr, "[w%d] io_uring_setup: %s\n", p->id, strerror(-rc));
        abort();
    }
#ifndef NO_REG_RING
    uring_register_ring_fd(&p->ring);                /* optional: enter skips an fd lookup      */
#endif

    unsigned slots = fixed_slots();                  /* optional: sockets live in a file table  */
    if (slots)
        uring_register_files_sparse(&p->ring, slots);
    ioma__bufring_init(&p->bufs, &p->ring, p->id);
    p->listen_fd = listener_open(p->port);
    arm_accept(p);
    fprintf(stderr, "[w%d] listening on 0.0.0.0:%u (cpu %d, %u x %u B recv buffers, ring %u%s%s%s)\n",
            p->id, p->port, p->cpu, BUF_COUNT, BUF_SIZE, p->ring.sq_entries,
            p->ring.has_sq_array ? "" : ", no sqarray",
            p->ring.enter_flags ? ", registered ring" : "",
            p->ring.fixed_files ? ", fixed files" : "");

    struct __kernel_timespec wait_at_most = { .tv_sec = 0, .tv_nsec = 100000000L };   /* 100 ms: so an idle worker notices *stop */
    while (!*p->stop) {
        run_ready(p);
        rearm_starved(p);
        ioma__bufring_publish(&p->bufs);

        rc = uring_submit_wait(&p->ring, 1, &wait_at_most);    /* one syscall per batch */
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY) {
            fprintf(stderr, "[w%d] io_uring_enter: %s\n", p->id, strerror(-rc));
            break;
        }

        unsigned ready = uring_cq_ready(&p->ring);   /* read the tail once    */
        for (unsigned i = 0; i < ready; i++)
            dispatch(p, uring_cqe_at(&p->ring, i));  /* handlers run in here  */
        uring_cq_advance(&p->ring, ready);           /* publish the head once */
    }

    fprintf(stderr, "[w%d] stopping: %llu accepted, %u still open\n",
            p->id, (unsigned long long)p->accepted, p->live);

    /* Sockets, then the ring (which cancels every in-flight op and drops its buffer references),
     * then the memory the kernel could still have referenced. */
    close(p->listen_fd);
    ioma__bufring_unregister(&p->bufs, &p->ring);
    uring_exit(&p->ring);
    ioma__bufring_unmap(&p->bufs);
    free(p->starved);
    ioma__conn_pool_drain(p);
    coro_pool_drain();
}

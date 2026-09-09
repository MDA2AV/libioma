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

#define DRAIN_GRACE_MS 2000

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void publish_cq(proactor_t *p)
{
    if (p->cq_taken) {
        ioxd__uring_cq_advance(&p->ring, p->cq_taken);
        p->cq_taken = 0;
    }
}

struct io_uring_sqe *ioxd__proactor_sqe(proactor_t *p)
{
    struct io_uring_sqe *sqe = ioxd__uring_get_sqe(&p->ring);
    int rc = 0;
    for (int i = 0; !sqe && i < 16; i++) {
        ioxd__bufring_publish(&p->bufs);
        publish_cq(p);
        rc = ioxd__uring_submit(&p->ring);
        if (rc == -EBUSY || rc == -EAGAIN || rc == -EINTR)
            rc = ioxd__uring_submit_wait(&p->ring, 0, nullptr);
        sqe = ioxd__uring_get_sqe(&p->ring);
    }
    if (!sqe) {
        fprintf(stderr, "[w%d] SQ still full after flushing: %s\n",
                p->id, rc < 0 ? ioxd__io_errstr(-rc) : "the kernel is not consuming it");
        abort();
    }
    return sqe;
}

static bool accept_room(const proactor_t *p)
{
    return !p->draining && (p->file_slots == 0 || p->live < p->file_slots);
}

static void stall_accept(struct listener *l)
{
    l->stalled      = true;
    l->stalled_live = l->p->live;
    l->retry_at     = (time_t)(now_ms() / 1000U) + 1;
}

static void arm_accept(struct listener *l)
{
    if (!accept_room(l->p)) {
        stall_accept(l);
        return;
    }
    l->stalled = false;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(l->p);
    sqe->opcode    = IORING_OP_ACCEPT;
    sqe->fd        = l->fd;
    sqe->ioprio    = IORING_ACCEPT_MULTISHOT;
    sqe->user_data = UD(l, TAG_ACCEPT);
    if (l->p->ring.fixed_files)
        sqe->file_index = IORING_FILE_INDEX_ALLOC;
}

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

static bool accept_exhausted(int err)
{
    return err == -ENFILE || err == -EMFILE || err == -ENOMEM || err == -ENOBUFS;
}

static void note_accept_error(struct listener *l, int result)
{
    time_t now = (time_t)(now_ms() / 1000U);
    l->err_since_log++;
    if (now < l->err_log_at)
        return;
    fprintf(stderr, "[w%d] accept :%u: %s (%llu since the last line)\n",
            l->p->id, l->port, ioxd__io_errstr(-result), (unsigned long long)l->err_since_log);
    l->err_since_log = 0;
    l->err_log_at    = now + 1;
}

static void on_accept(struct listener *l, int result, unsigned flags)
{
    proactor_t *p = l->p;
    trace("[w%d] accept :%u result=%d more=%d\n", p->id, l->port, result, !!(flags & IORING_CQE_F_MORE));
    if (result >= 0 && p->draining) {
        ioxd__conn_close_socket(p, result);
    } else if (result >= 0) {
        conn_t *c = ioxd__conn_new(p, l, result);
        ioxd__conn_arm_recv(p, c);
        ioxd__proactor_spawn(p, ioxd__conn_main, c);
        p->accepted++;
    } else if (!(p->draining && result == -ECANCELED)) {
        note_accept_error(l, result);
    }
    if (flags & IORING_CQE_F_MORE)
        return;
    if (result < 0 && accept_exhausted(result)) {
        stall_accept(l);
        return;
    }
    arm_accept(l);
}

static void dispatch(proactor_t *p, struct io_uring_cqe *cqe)
{
    void *ptr = UD_PTR(cqe->user_data);
    switch (UD_TAG(cqe->user_data)) {
    case TAG_OP: {
        op_t *op  = ptr;
        op->res   = cqe->res;
        op->flags = cqe->flags;
        trace("[w%d] op res=%d flags=%#x\n", p->id, cqe->res, cqe->flags);
        ioxd__coro_resume(op->waiter);
        break;
    }
    case TAG_RECV:
        ioxd__conn_on_recv(p, ptr, cqe->res, cqe->flags);
        break;
    case TAG_ACCEPT:
        on_accept(ptr, cqe->res, cqe->flags);
        break;
    case TAG_CLOSE:
        if (cqe->res < 0)
            fprintf(stderr, "[w%d] close: %s\n", p->id, ioxd__io_errstr(-cqe->res));
        break;
    case TAG_DRAIN:
        if (cqe->res < 0 && cqe->res != -ENOENT) {
            fprintf(stderr, "[w%d] cancel all: %s; cancelling connections one at a time\n",
                    p->id, ioxd__io_errstr(-cqe->res));
            p->cancel_each = true;
        }
        break;
    default:
        break;
    }
}

void ioxd__proactor_spawn(proactor_t *p, void (*fn)(void *), void *arg)
{
    coro_t *c = ioxd__coro_create(fn, arg, p->cfg.stack_size);
    if (p->ready_tail)
        p->ready_tail->next = c;
    else
        p->ready_head = c;
    p->ready_tail = c;
}

static void run_ready(proactor_t *p)
{
    while (p->ready_head) {
        coro_t *c = p->ready_head;
        p->ready_head = c->next;
        if (!p->ready_head)
            p->ready_tail = nullptr;
        c->next = nullptr;
        ioxd__coro_resume(c);
    }
}

static void rearm_starved(proactor_t *p)
{
    unsigned room = p->bufs.returned;
    p->bufs.returned = 0;
    if (p->nstarved == 0 || room == 0)
        return;

    unsigned n = room < p->nstarved ? room : p->nstarved;
    for (unsigned i = 0; i < n; i++)
        ioxd__conn_arm_recv(p, p->starved[i]);
    p->nstarved -= n;
    memmove(p->starved, p->starved + n, p->nstarved * sizeof *p->starved);
}

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

static void begin_drain(proactor_t *p)
{
    p->draining = true;
    for (int i = 0; i < p->n_listeners; i++) {
        struct listener *l = &p->listeners[i];
        if (!l->stalled) {
            struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
            sqe->opcode    = IORING_OP_ASYNC_CANCEL;
            sqe->fd        = -1;
            sqe->addr      = UD(l, TAG_ACCEPT);
            sqe->user_data = TAG_IGNORE;
        }
        l->stalled = true;
    }

    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode       = IORING_OP_ASYNC_CANCEL;
    sqe->fd           = -1;
    sqe->cancel_flags = IORING_ASYNC_CANCEL_ANY;
    sqe->user_data    = UD(p, TAG_DRAIN);

    while (p->nstarved)
        ioxd__conn_recv_drain(p->starved[--p->nstarved]);
}

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

static unsigned fixed_slots(void)
{
    struct rlimit rl;
    unsigned n = FIXED_FILES;
    if (n && getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < n)
        n = (unsigned)rl.rlim_cur;
    return n;
}

static thread_local proactor_t *current;

proactor_t *ioxd__proactor_current(void)
{
    return current;
}

void ioxd__proactor_run(proactor_t *p)
{
    current = p;
    if (p->cpu >= 0)
        pin_to(p->cpu);

    ioxd__coro_pool_limit(p->cfg.idle_stacks);
    int rc = ioxd__uring_init(&p->ring, p->cfg.ring_entries);
    if (rc < 0) {
        fprintf(stderr, "[w%d] io_uring_setup: %s%s\n", p->id, ioxd__io_errstr(-rc),
                rc == -EPERM ? " (io_uring is disabled: see /proc/sys/kernel/io_uring_disabled)" : "");
        abort();
    }
#ifndef NO_REG_RING
    ioxd__uring_register_ring_fd(&p->ring);
#endif

    unsigned slots = fixed_slots();
    if (slots && ioxd__uring_register_files_sparse(&p->ring, slots) == 0)
        p->file_slots = slots;
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
        at += (size_t)n < sizeof ports - at ? (size_t)n : sizeof ports - at - 1;
    }
    fprintf(stderr, "[w%d] listening on %s (cpu %d, %u x %u B recv buffers, ring %u%s%s%s)\n",
            p->id, ports, p->cpu, p->bufs.count, p->bufs.size, p->ring.sq_entries,
            p->ring.has_sq_array ? "" : ", no sqarray",
            p->ring.enter_flags ? ", registered ring" : "",
            p->ring.fixed_files ? ", fixed files" : "");

    struct __kernel_timespec wait_at_most = { .tv_sec = 0, .tv_nsec = 100000000L };
    uint64_t deadline = 0;
    for (;;) {
        if (*p->stop && !p->draining) {
            begin_drain(p);
            deadline = now_ms() + DRAIN_GRACE_MS;
        }
        if (p->draining && (p->live == 0 || now_ms() >= deadline))
            break;

        run_ready(p);
        rearm_starved(p);
        ioxd__bufring_publish(&p->bufs);

        rc = ioxd__uring_submit_wait(&p->ring, 1, &wait_at_most);
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY) {
            fprintf(stderr, "[w%d] io_uring_enter: %s\n", p->id, ioxd__io_errstr(-rc));
            p->failed = rc;
            *p->stop  = 1;
            break;
        }

        unsigned ready = ioxd__uring_cq_ready(&p->ring);
        for (unsigned i = 0; i < ready; i++) {
            struct io_uring_cqe cqe = *ioxd__uring_cqe_at(&p->ring, p->cq_taken);
            p->cq_taken++;
            dispatch(p, &cqe);
        }
        publish_cq(p);
        rearm_stalled(p);
    }

    ioxd__bufring_publish(&p->bufs);
    ioxd__uring_submit(&p->ring);

    fprintf(stderr, "[w%d] stopping: %llu accepted, %u still open, %llu cq overflows\n",
            p->id, (unsigned long long)p->accepted, p->live,
            (unsigned long long)p->ring.cq_overflows);

    for (int i = 0; i < p->n_listeners; i++)
        close(p->listeners[i].fd);
    ioxd__bufring_unregister(&p->bufs, &p->ring);
    ioxd__uring_exit(&p->ring);
    if (p->live == 0) {
        ioxd__bufring_unmap(&p->bufs);
    } else {
        fprintf(stderr, "[w%d] %u connections did not close in %d ms: the recv buffers stay mapped\n",
                p->id, p->live, DRAIN_GRACE_MS);
    }
    free(p->starved);
    p->starved  = nullptr;
    p->nstarved = p->cap_starved = 0;
    ioxd__conn_pool_drain(p);
    ioxd__coro_pool_drain();
}

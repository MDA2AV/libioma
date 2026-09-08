/*
 * run.c - ioma_run: one proactor thread per core serving HTTP, until SIGINT/SIGTERM.
 */
#include "http/internal.h"
#include "io/pipe.h"
#include "io/proactor.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

/* SIGINT/SIGTERM: raise the flag every worker loop polls. */
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* pthread entry: the worker's whole life. */
static void *worker_thread(void *arg)
{
    proactor_run(arg);
    return nullptr;
}

/* CPUs this process may run on (its cpuset), so the default is one worker per available core -
 * never a fixed count that would oversubscribe a small cpuset. */
static int cpu_count(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0)
            return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* Lift the soft fd limit to the hard one: open connections and the registered file table are
 * both checked against it, and the default soft limit is often 1024. */
static void raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* Start the workers (workers <= 0: one per available core) and block until a stop signal. */
/* Worker threads, one proactor each, serving `port` with `handler` on every connection until
 * SIGINT/SIGTERM. What ioma_run and ioma_run_pipes share. */
static int run_workers(int workers, int port, handler_fn handler)
{
    if (workers <= 0)
        workers = cpu_count();
    if (port < 1 || port > 65535) {
        fprintf(stderr, "ioma_run: 1<=port<=65535 required\n");
        return 2;
    }

    raise_nofile();
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    proactor_t *ws = calloc((size_t)workers, sizeof *ws);
    pthread_t  *th = calloc((size_t)workers, sizeof *th);
    if (!ws || !th) {
        perror("calloc");
        free(ws);
        free(th);
        return 1;
    }

    for (int i = 0; i < workers; i++) {
        ws[i].id      = i;
        ws[i].cpu     = i;
        ws[i].port    = (uint16_t)port;
        ws[i].handler = handler;
        ws[i].stop    = &g_stop;
        if (pthread_create(&th[i], nullptr, worker_thread, &ws[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    fprintf(stderr, "ioma: %d workers on :%d\n", workers, port);

    for (int i = 0; i < workers; i++)
        pthread_join(th[i], nullptr);
    free(th);
    free(ws);
    return 0;
}

int ioma_run(int workers, int port)
{
    ioma__router_build();                          /* the routes, resolved once, shared read-only */
    return run_workers(workers, port, ioma__serve);
}

/* ── pipes ─────────────────────────────────────────────────────────────────────────────── */

#ifndef IOMA_PIPE_BUF
#define IOMA_PIPE_BUF  16384                      /* a pipe's gathering buffer (kept + live bytes) */
#endif
#ifndef IOMA_PIPE_SLAB
#define IOMA_PIPE_SLAB 8192                       /* a pipe's write slab                          */
#endif

static ioma_pipe_handler g_pipe_handler;

/* The connection handler behind ioma_run_pipes: a pipe over the connection, the handler on it. */
static void serve_pipe(conn_t *conn)
{
    char             gather[IOMA_PIPE_BUF];
    char             slab[IOMA_PIPE_SLAB];
    struct ioma_pipe pipe;
    ioma__pipe_init(&pipe, conn, gather, sizeof gather, slab, sizeof slab);
    g_pipe_handler(&pipe);
    ioma__pipe_close(&pipe);
}

int ioma_run_pipes(int workers, int port, ioma_pipe_handler fn)
{
    g_pipe_handler = fn;
    return run_workers(workers, port, serve_pipe);
}

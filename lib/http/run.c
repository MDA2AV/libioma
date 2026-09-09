/*
 * run.c - ioxd_run: one proactor thread per core serving HTTP, until SIGINT/SIGTERM.
 */
#include "http/engine.h"
#include "http/router.h"
#include "io/pipe.h"
#include "io/proactor.h"
#include "tls/handshake.h"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
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

/* The ports bound before the run, each with its store or none; every worker opens all of them. */
static struct listener g_listeners[IOXD_MAX_LISTENERS];
static int             g_n_listeners;

int ioxd_bind(int port, ioxd_certs *certs)
{
    if (port < 1 || port > 65535 || g_n_listeners == IOXD_MAX_LISTENERS) {
        fprintf(stderr, "ioxd_bind: port %d refused (1..65535, at most %d ports)\n", port, IOXD_MAX_LISTENERS);
        return -1;
    }
    g_listeners[g_n_listeners++] = (struct listener){ .port = (uint16_t)port, .certs = certs };
    return 0;
}

/* Worker threads (workers <= 0: one per available core), one proactor each, running `handler`
 * on every connection of every bound port until SIGINT/SIGTERM. What ioxd_run and ioxd_run_pipes
 * share. */
static int run_workers(int workers, handler_fn handler)
{
    if (workers <= 0)
        workers = cpu_count();
    if (g_n_listeners == 0) {
        fprintf(stderr, "ioxd_run: nothing bound: ioxd_bind a port first\n");
        return 2;
    }

    g_stop = 0;                           /* a previous run's signal must not stop this one at once */
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

    int rc = 0, started = 0;
    for (int i = 0; i < workers; i++) {
        ws[i].id      = i;
        ws[i].cpu     = i;
        ws[i].handler = handler;
        ws[i].n_listeners = g_n_listeners;
        memcpy(ws[i].listeners, g_listeners, sizeof g_listeners);
        ws[i].stop    = &g_stop;
        if (pthread_create(&th[i], nullptr, worker_thread, &ws[i]) != 0) {
            perror("pthread_create");
            g_stop = 1;                   /* the ones already running must retire, not serve on alone */
            rc = 1;
            break;
        }
        started++;
    }
    if (started) {
        fprintf(stderr, "ioxd: %d workers on", started);
        for (int i = 0; i < g_n_listeners; i++)
            fprintf(stderr, " :%u%s", g_listeners[i].port, g_listeners[i].certs ? "/tls" : "");
        fputc('\n', stderr);
    }

    for (int i = 0; i < started; i++)
        pthread_join(th[i], nullptr);
    for (int i = 0; i < started; i++)
        if (ws[i].failed)                 /* a worker whose ring died: the run did not succeed */
            rc = 1;
    free(th);
    free(ws);
    return rc;
}

/* A TLS listener's connection runs the handshake before its handler; a plain one goes straight in. */
static int prologue(struct ioxd_pipe *pipe)
{
    struct listener *l = pipe->in.conn->listener;
    return l->certs ? ioxd__tls_prologue(pipe, l->certs) : 0;
}

static void serve_http(struct ioxd_pipe *pipe)
{
    if (prologue(pipe) != 0)
        return;
    ioxd__serve(pipe);
    if (pipe->in.conn->listener->certs)
        ioxd__tls_close_notify(pipe);
}

/* ioxd_run, through the header's inline: the caller's sizeof(ioxd_ctx) must be ours, or the
 * limits that size it (IOXD_MAX_HEADERS and friends) were redefined on one side and every
 * handler would read the context at the wrong offsets. */
int ioxd__run(int workers, size_t ctx_size)
{
    if (ctx_size != sizeof(ioxd_ctx)) {
        fprintf(stderr, "ioxd_run: the application's ioxd_ctx is %zu bytes, the library's %zu: "
                        "IOXD_MAX_* limits redefined on one side\n", ctx_size, sizeof(ioxd_ctx));
        return 1;
    }
    ioxd__router_build();                          /* the routes, resolved once, shared read-only */
    return run_workers(workers, serve_http);
}

static ioxd_pipe_handler g_pipe_handler;

static void serve_pipe(struct ioxd_pipe *pipe)
{
    if (prologue(pipe) != 0)
        return;
    g_pipe_handler(pipe);
    if (pipe->in.conn->listener->certs)
        ioxd__tls_close_notify(pipe);
}

int ioxd_run_pipes(int workers, ioxd_pipe_handler fn)
{
    g_pipe_handler = fn;
    return run_workers(workers, serve_pipe);
}

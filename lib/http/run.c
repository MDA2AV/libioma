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

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void *worker_thread(void *arg)
{
    proactor_run(arg);
    return nullptr;
}

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

static void raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

static ioxd_config g_config;

static bool power_of_two(unsigned v)
{
    return v && (v & (v - 1)) == 0;
}

int ioxd_configure(const ioxd_config *config)
{
    const char *why = nullptr;
    if (config->ring_entries && (!power_of_two(config->ring_entries) || config->ring_entries > 32768))
        why = "ring_entries must be a power of two, at most 32768";
    else if (config->recv_buffers && (!power_of_two(config->recv_buffers) || config->recv_buffers > 32768))
        why = "recv_buffers must be a power of two, at most 32768 (the kernel refuses 65536)";
    else if (config->recv_buffer_size && (config->recv_buffer_size < 64 || config->recv_buffer_size > (1U << 20)))
        why = "recv_buffer_size must be between 64 bytes and 1 MB";
    else if (config->stack_size && (config->stack_size < 64UL * 1024 || config->stack_size > 64UL * 1024 * 1024))
        why = "stack_size must be between 64 KB and 64 MB";
    if (why) {
        fprintf(stderr, "ioxd_configure: %s\n", why);
        return -1;
    }
    g_config = *config;
    return 0;
}

static ioxd_config effective_config(void)
{
    ioxd_config c = g_config;
    if (!c.ring_entries)      c.ring_entries     = RING_ENTRIES;
    if (!c.recv_buffers)      c.recv_buffers     = BUF_COUNT;
    if (!c.recv_buffer_size)  c.recv_buffer_size = BUF_SIZE;
    if (!c.stack_size)        c.stack_size       = STACK_SIZE;
    if (!c.idle_stacks)       c.idle_stacks      = CORO_POOL_MAX;
    if (!c.idle_connections)  c.idle_connections = CONN_POOL_MAX;
    return c;
}

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

static int run_workers(int workers, handler_fn handler)
{
    if (workers <= 0)
        workers = cpu_count();
    if (g_n_listeners == 0) {
        fprintf(stderr, "ioxd_run: nothing bound: ioxd_bind a port first\n");
        return 2;
    }

    g_stop = 0;
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

    int         rc = 0, started = 0;
    ioxd_config cfg = effective_config();
    for (int i = 0; i < workers; i++) {
        ws[i].id      = i;
        ws[i].cpu     = i;
        ws[i].cfg     = cfg;
        ws[i].handler = handler;
        ws[i].n_listeners = g_n_listeners;
        memcpy(ws[i].listeners, g_listeners, sizeof g_listeners);
        ws[i].stop    = &g_stop;
        if (pthread_create(&th[i], nullptr, worker_thread, &ws[i]) != 0) {
            perror("pthread_create");
            g_stop = 1;
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
        if (ws[i].failed)
            rc = 1;
    free(th);
    free(ws);
    return rc;
}

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

int ioxd__run(int workers, size_t ctx_size)
{
    if (ctx_size != sizeof(ioxd_ctx)) {
        fprintf(stderr, "ioxd_run: the application's ioxd_ctx is %zu bytes, the library's %zu: "
                        "IOXD_MAX_* limits redefined on one side\n", ctx_size, sizeof(ioxd_ctx));
        return 1;
    }
    ioxd__router_build();
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

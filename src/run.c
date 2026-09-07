/*
 * run.c - ioma_run: one proactor thread per core serving HTTP, until SIGINT/SIGTERM.
 */
#define _GNU_SOURCE
#include "internal.h"

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
    return NULL;
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
int ioma_run(int workers, int port)
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
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    proactor_t *ws = calloc((size_t)workers, sizeof *ws);
    pthread_t  *th = calloc((size_t)workers, sizeof *th);
    if (!ws || !th) {
        perror("calloc");
        return 1;
    }

    for (int i = 0; i < workers; i++) {
        ws[i].id      = i;
        ws[i].cpu     = i;
        ws[i].port    = (uint16_t)port;
        ws[i].handler = ioma__serve;
        ws[i].stop    = &g_stop;
        if (pthread_create(&th[i], NULL, worker_thread, &ws[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    fprintf(stderr, "ioma: %d workers on :%d\n", workers, port);

    for (int i = 0; i < workers; i++)
        pthread_join(th[i], NULL);
    free(th);
    free(ws);
    return 0;
}

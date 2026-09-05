/*
 * main.c - a complete server: N workers, one per core, and a plaintext HTTP/1.1 handler written
 * as linear code. Every await parks the handler's coroutine; the worker's loop resumes it when
 * the io_uring completion arrives, on the same thread, inside the dispatch of that completion.
 *
 *     make && ./ioma [workers] [port]
 *     curl http://127.0.0.1:8080/
 */
#define _GNU_SOURCE
#include "proactor.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static const char RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "ok";
#define RESPONSE_LEN (sizeof RESPONSE - 1)

/* The handler. Its locals live on the coroutine's stack across every await. */
static void handler(conn_t *c)
{
    char   buf[8192];
    size_t have = 0;

    for (;;) {
        int n = await_recv(c, buf + have, sizeof buf - have);
        if (n <= 0) {                                        /* 0: peer closed, <0: -errno */
#ifdef TRACE
            fprintf(stderr, "handler fd=%d: recv -> %d, have=%zu\n", c->fd, n, have);
#endif
            return;
        }
        have += (size_t)n;

        /* answer every complete request in the buffer, keep a partial tail for the next recv */
        size_t off = 0;
        for (;;) {
            char *end = memmem(buf + off, have - off, "\r\n\r\n", 4);
            if (!end)
                break;
            off = (size_t)(end + 4 - buf);
            int rc = await_send(c, RESPONSE, RESPONSE_LEN);
            if (rc < 0) {
#ifdef TRACE
                fprintf(stderr, "handler fd=%d: send -> %d\n", c->fd, rc);
#endif
                return;
            }
        }
        if (off) {
            memmove(buf, buf + off, have - off);
            have -= off;
        }
        if (have == sizeof buf) {
#ifdef TRACE
            fprintf(stderr, "handler fd=%d: request larger than the buffer\n", c->fd);
#endif
            return;                                          /* request larger than the buffer */
        }
    }
}

static void *worker_thread(void *arg)
{
    proactor_run(arg);
    return NULL;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    int port    = argc > 2 ? atoi(argv[2]) : 8080;
    if (workers < 1 || port < 1 || port > 65535) {
        fprintf(stderr, "usage: %s [workers] [port]\n", argv[0]);
        return 2;
    }

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
        ws[i].cpu     = i;                                   /* thread per core */
        ws[i].port    = (uint16_t)port;
        ws[i].handler = handler;
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

/*
 * conn.h - one connection on its worker: the socket, the queue of buffers the kernel filled for
 * it, the state of its multishot recv, and the awaits a coroutine calls on it. Thread-per-core:
 * a connection is only ever touched by the worker that accepted it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/coro.h"

#ifndef RX_QUEUE
#define RX_QUEUE     64                   /* undelivered slices one connection may hold, pow 2  */
#endif

typedef struct proactor proactor_t;
typedef struct conn     conn_t;

/* A slice the kernel delivered into a provided buffer, waiting for the handler to read it. */
struct rx_item {
    uint8_t *ptr;
    uint32_t len;
    uint16_t buf_id;
};

enum recv_state {
    RECV_ARMED,                           /* multishot recv in flight; the kernel may post CQEs */
    RECV_STARVED,                         /* it ended on -ENOBUFS; re-armed once buffers return */
    RECV_DONE,                            /* it posted its terminal CQE                         */
};

struct conn {
    int             fd;                   /* the socket, or its file slot under fixed files    */
    proactor_t     *p;
    coro_t         *waiter;               /* coroutine parked in await_recv, or nullptr           */
    struct rx_item  rx[RX_QUEUE];         /* delivered while nobody was reading                */
    unsigned        rx_head, rx_tail;
    enum recv_state recv;
    int             refs;                 /* the handler coroutine + the armed/starved recv    */
    bool            closed;               /* the handler returned; fd closed                   */
    bool            eof;                  /* recv ended: peer FIN, error, or queue overflow    */
    int             err;                  /* 0 on FIN, else the negative errno                 */
    struct conn    *pool_next;            /* free-list link while recycled (not in use): a LIFO - a  */
                                          /* returned conn becomes the head and points at the old one */
};

/* Awaits: call from a coroutine on the owning worker. The coroutine parks; the loop resumes it
 * when the completion arrives. */
int await_recv(conn_t *c, void *buf, size_t len);        /* >0 bytes, 0 peer closed, <0 -errno */
int await_send(conn_t *c, const void *buf, size_t len);  /* len when all sent, else -errno     */
int ioma__await_item(conn_t *c, struct rx_item *out);    /* the next received buffer, whole: 1, 0 at the end, <0 -errno */

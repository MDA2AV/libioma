/*
 * io/internal.h - what the I/O plane's files share with each other. Private; not installed.
 *
 * Exported internals carry an ioma__ prefix so they cannot collide with a user's symbols. The
 * small hot-path helpers are static inline here so every file still gets them inlined.
 */
#pragma once

#include "io/proactor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── constants ─────────────────────────────────────────────────────────────────────────── */

#define BGID     1                                /* the one provided-buffer group per worker */
#define RX_MASK  (RX_QUEUE  - 1)
#define BUF_MASK (BUF_COUNT - 1)

#ifndef CONN_POOL_MAX
#define CONN_POOL_MAX 1024                        /* idle conn_t kept warm per worker         */
#endif

/* -DTRACE: one line per completion and lifetime event, for chasing a misbehaving path. */
#ifdef TRACE
#define trace(...) fprintf(stderr, __VA_ARGS__)
#else
#define trace(...) ((void)0)
#endif

/* ── completion routing ────────────────────────────────────────────────────────────────── */

/* user_data is a pointer with a tag in its low three bits; everything pointed at is 8-aligned. */
enum { TAG_OP = 0, TAG_RECV = 1, TAG_ACCEPT = 2, TAG_IGNORE = 3 };

#define UD(ptr, tag) ((uint64_t)(uintptr_t)(ptr) | (uint64_t)(tag))
#define UD_PTR(ud)   ((void *)(uintptr_t)((ud) & ~(uint64_t)7))
#define UD_TAG(ud)   ((unsigned)((ud) & 7))

/* A one-shot operation. It lives in the awaiting coroutine's stack frame, which is frozen while
 * the coroutine is parked, so its address is valid for exactly as long as the op is in flight. */
typedef struct op {
    coro_t  *waiter;
    int      res;
    unsigned flags;
} op_t;

/* ── SQE helpers ───────────────────────────────────────────────────────────────────────── */

/* Claim an SQE. If the SQ is full mid-batch, flush without waiting and retry. */
static inline struct io_uring_sqe *get_sqe(proactor_t *p)
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

/* Stage a one-shot op and park until its CQE. The loop fills op->res and resumes us. */
static inline int await_op(struct io_uring_sqe *sqe, op_t *op)
{
    op->waiter     = coro_current();
    sqe->user_data = UD(op, TAG_OP);
    coro_yield();
    return op->res;
}

/* Ask the kernel to cancel the op carrying that user_data. The acknowledgement CQE is ignored. */
static inline void submit_cancel(proactor_t *p, uint64_t target_user_data)
{
    struct io_uring_sqe *sqe = get_sqe(p);
    sqe->opcode    = IORING_OP_ASYNC_CANCEL;
    sqe->fd        = -1;
    sqe->addr      = target_user_data;
    sqe->user_data = TAG_IGNORE;
}

/* ── provided buffers ──────────────────────────────────────────────────────────────────── */

/* Stage a buffer's return to the ring. The loop publishes the tail once per batch. */
static inline void return_buf(proactor_t *p, uint16_t bid)
{
    struct io_uring_buf *b = &p->buf_ring->bufs[p->buf_tail & BUF_MASK];
    b->addr = (uint64_t)(uintptr_t)(p->slab + (size_t)bid * BUF_SIZE);
    b->len  = BUF_SIZE;
    b->bid  = bid;
    p->buf_tail++;
    p->buffers_returned = true;                   /* lets the loop re-arm starved recvs     */
    p->buf_dirty        = true;                   /* tail needs publishing before the enter */
}

/* Publish staged returns to the kernel: one atomic release for the whole batch. */
static inline void bufring_publish(proactor_t *p)
{
    if (!p->buf_dirty)
        return;
    __atomic_store_n(&p->buf_ring->tail, (uint16_t)p->buf_tail, __ATOMIC_RELEASE);
    p->buf_dirty = false;
}

void ioma__bufring_init(proactor_t *p);           /* bufring.c: map, register, fill          */
void ioma__bufring_unregister(proactor_t *p);     /* bufring.c: before uring_exit            */
void ioma__bufring_unmap(proactor_t *p);          /* bufring.c: after uring_exit             */

/* ── connections (conn.c) ──────────────────────────────────────────────────────────────── */

conn_t *ioma__conn_new(proactor_t *p, int fd);
void    ioma__conn_main(void *arg);               /* the connection's coroutine body         */
void    ioma__arm_recv(proactor_t *p, conn_t *c);
void    ioma__on_recv(proactor_t *p, conn_t *c, int res, unsigned flags);
void    ioma__conn_pool_drain(proactor_t *p);

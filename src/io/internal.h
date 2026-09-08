/*
 * io/internal.h - what the I/O plane's files share with each other. Private; not installed.
 *
 * Exported internals carry an ioma__ prefix so they cannot collide with a user's symbols. Only
 * declarations, types and macros live here; LTO inlines the small hot ones across files.
 */
#pragma once

#include "io/proactor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── constants ─────────────────────────────────────────────────────────────────────────── */

#define RX_MASK  (RX_QUEUE  - 1U)

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
enum {
    TAG_OP = 0,
    TAG_RECV = 1,
    TAG_ACCEPT = 2,
    TAG_IGNORE = 3, };

#define UD(ptr, tag) ((uint64_t)(uintptr_t)(ptr) | (uint64_t)(tag))
#define UD_PTR(ud)   ((void *)(uintptr_t)((ud) & ~(uint64_t)7))
#define UD_TAG(ud)   ((unsigned)((ud) & 7U))

/* A one-shot operation. It lives in the awaiting coroutine's stack frame, which is frozen while
 * the coroutine is parked, so its address is valid for exactly as long as the op is in flight. */
typedef struct op {
    coro_t  *waiter;
    int      res;
    unsigned flags;
} op_t;

/* ── shared between the plane's files ──────────────────────────────────────────────────── */

/* proactor.c */
struct io_uring_sqe *ioma__sqe(proactor_t *p);   /* claim an SQE; flushes without waiting if the SQ is full */

/* ── connections (conn.c) ──────────────────────────────────────────────────────────────── */

conn_t *ioma__conn_new(proactor_t *p, int fd);
void    ioma__conn_main(void *arg);               /* the connection's coroutine body         */
void    ioma__arm_recv(proactor_t *p, conn_t *c);
void    ioma__on_recv(proactor_t *p, conn_t *c, int res, unsigned flags);
void    ioma__conn_pool_drain(proactor_t *p);

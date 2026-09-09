/*
 * io/internal.h - the contract between the loop (proactor.c) and the operations (conn.c): how
 * a completion finds what it belongs to, and the trace switch. Private; not installed. Each
 * module's own declarations are in its header (uring.h, coro.h, bufring.h, conn.h, proactor.h).
 */
#pragma once

#include "io/proactor.h"

#include <stdint.h>
#include <stdio.h>

/* -DTRACE: one line per completion and lifetime event, for chasing a misbehaving path. */
#ifdef TRACE
#define trace(...) fprintf(stderr, __VA_ARGS__)
#else
#define trace(...) ((void)0)
#endif

/* ── completion routing ────────────────────────────────────────────────────────────────── */

/* user_data is a pointer with a tag in its low three bits; everything pointed at is 8-aligned.
 * TAG_IGNORE is the zero tag on purpose: an SQE whose user_data was never set then dispatches as
 * "nobody waits for this" instead of as an op with a null pointer. */
enum {
    TAG_IGNORE = 0,                               /* a completion nobody waits for            */
    TAG_OP = 1,                                   /* an op_t: a one-shot await                */
    TAG_RECV = 2,                                 /* a conn_t: its multishot recv             */
    TAG_ACCEPT = 3,                               /* the listener's multishot accept          */
    TAG_CLOSE = 4,                                /* a socket's close: only a failure is news */
    TAG_DRAIN = 5,                                /* the shutdown's blanket cancel            */
};

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

#include <string.h>

/* The text of an errno, for a log line. glibc's strerror has had a per-thread buffer since 2.32,
 * which is what every supported box runs; clang-tidy's concurrency check does not know that. */
static inline const char *ioxd__errstr(int err)
{
    return strerror(err);   /* NOLINT(concurrency-mt-unsafe) */
}

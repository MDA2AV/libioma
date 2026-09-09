/*
 * io/timer.h - a wait that runs on the ring: the coroutine parks on an IORING_OP_TIMEOUT and
 * the worker's loop resumes it when the kernel says the time has passed, the way a send parks
 * on its completion. Nothing is armed on the side and no syscall is made for it: the SQE goes
 * out with whatever batch the loop was already submitting. The public face is ioxd/timer.h.
 */
#pragma once

#include <stdint.h>

#include "io/proactor.h"

/* Park the calling coroutine for ns nanoseconds (at least one: a zero timespec would disarm a
 * timer rather than fire one). 0 once the time has passed, -ECANCELED when the worker is
 * stopping, else -errno. */
int ioxd__timer_delay(proactor_t *p, uint64_t ns);

/* ── timer.c: the notes ─────────────────────────────────────────────────────────────────── */

/* ioxd__timer_delay:
 * The timespec and the op live on the coroutine's stack, which is frozen while it is parked,
 * so the kernel reads the deadline from memory that cannot move. io_uring reports a timeout
 * that ran its course as -ETIME: that is the success here, and comes back as 0. A cancel - the
 * shutdown's blanket ASYNC_CANCEL_ANY takes timeouts too - comes back as -ECANCELED, so a
 * handler waiting in a loop stops when the server does.
 */

/* ioxd_delay, ioxd_delay_ns:
 * The public entry has no context argument: the worker and the coroutine are the thread's
 * current ones. Off a worker - the main thread before the run, or a thread of the application's
 * own - there is no ring to wait on, and the call says so with -1 rather than sleeping.
 */

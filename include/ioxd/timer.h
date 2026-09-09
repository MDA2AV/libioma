/*
 * ioxd/timer.h - waiting without holding the worker: a delay a handler calls like a sleep, that
 * parks its connection's coroutine on the ring and lets every other connection run meanwhile.
 */
#pragma once

#include <stdint.h>

/* Wait ms milliseconds (or ns nanoseconds; less than one becomes one), from a handler, a
 * middleware or a pipe handler: the call returns once the time has passed, and the worker
 * serves its other connections while it waits - nothing blocks. Returns 0 when the time
 * passed, -ECANCELED when the server is stopping (a handler in a loop should return), and -1
 * when called off a worker, where there is nothing to wait on: the main thread before ioxd_run,
 * or a thread of your own. The wait is the kernel's timer, submitted with the batch the worker
 * was already sending; it costs no syscall of its own and no allocation. */
int ioxd_delay   (unsigned ms);
int ioxd_delay_ns(uint64_t ns);

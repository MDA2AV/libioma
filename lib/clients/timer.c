#include "clients/timer.h"
#include "io/coro.h"
#include "io/internal.h"
#include "ioxd/timer.h"

#include <errno.h>
#include <linux/time_types.h>

int ioxd__timer_delay(proactor_t *p, uint64_t ns)
{
    if (ns < 1)
        ns = 1;
    struct __kernel_timespec ts = { .tv_sec = (int64_t)(ns / 1000000000ULL), .tv_nsec = (int64_t)(ns % 1000000000ULL) };
    op_t op;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(p);
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->fd     = -1;
    sqe->addr   = (uint64_t)(uintptr_t)&ts;
    sqe->len    = 1;
    int rc = ioxd__io_await(sqe, &op);
    if (rc == -ETIME)
        return 0;
    return rc;
}

int ioxd_delay_ns(uint64_t ns)
{
    proactor_t *p = ioxd__proactor_current();
    if (!p || !ioxd__coro_current())
        return -1;
    return ioxd__timer_delay(p, ns);
}

int ioxd_delay(unsigned ms)
{
    return ioxd_delay_ns((uint64_t)ms * 1000000ULL);
}

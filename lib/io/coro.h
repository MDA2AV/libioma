/*
 * coro.h - stackful coroutines for one proactor thread.
 *
 * A coroutine runs on its own mmap'd stack with a guard page. Suspending saves the callee-saved
 * registers and switches to the loop's stack; resuming is the reverse. Everything on the
 * coroutine's stack stays exactly where it was while it is parked, which is what lets an
 * io_uring completion be routed to a struct that lives in an await's frame.
 *
 * Discipline: only the loop calls coro_resume, only a coroutine calls coro_yield. A coroutine
 * that wants to start another one hands it to the scheduler (proactor_spawn); resuming from
 * inside a coroutine would overwrite the loop's saved stack pointer.
 *
 * Two things the switch does not carry, both deliberate. It saves the six callee-saved registers
 * and nothing else, so MXCSR and the x87 control word - the rounding mode, the denormal and
 * exception masks - are whatever the last coroutine left behind: a handler that changes an FP mode
 * must put it back before it yields. And switch_x86_64.S carries no .note.gnu.property, which
 * leaves the whole linked program without the IBT/SHSTK markings, so no shadow stack is ever armed
 * around a stack this file forged by hand.
 */
#pragma once

#include <stddef.h>

/* Private to libioxd: hidden symbols cannot be interposed, so nothing outside the library can
 * substitute a scheduler primitive (the .S hides swap_ctx the same way). */
#define CORO_API __attribute__((visibility("hidden")))

typedef struct coro {
    void   *sp;               /* saved stack pointer while suspended                  */
    void   *stack;            /* mmap base; the low CORO_GUARD bytes are the guard    */
    size_t  size;             /* mapping size, guard included                         */
    void  (*fn)(void *);
    void   *arg;
    bool    done;             /* fn returned; the next resume-return frees the stack  */
    struct coro *next;        /* scheduler's ready-list link                          */
} coro_t;

/* Allocate a stack and forge its first frame. The descriptor lives at the top of that stack. */
CORO_API coro_t *coro_create(void (*fn)(void *), void *arg, size_t stack_bytes);

/* Loop only. Run c until it yields; if it finished, its stack is unmapped before returning. */
CORO_API void coro_resume(coro_t *c);

/* Coroutine only. Back to the loop; returns when the loop resumes this coroutine again. */
CORO_API void coro_yield(void);

/* The running coroutine, nullptr on the loop stack. */
CORO_API coro_t *coro_current(void);

/* Unmap the per-thread free list of pooled stacks. Call at worker teardown, on the worker thread. */
CORO_API void coro_pool_drain(void);

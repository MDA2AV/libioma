/*
 * coro.h - stackful coroutines for one proactor thread.
 *
 * A coroutine runs on its own mmap'd stack with an unmapped guard region (CORO_GUARD) below it.
 * Suspending saves the callee-saved registers and switches to the loop's stack; resuming is the
 * reverse. Everything on the coroutine's stack stays exactly where it was while it is parked,
 * which is what lets an io_uring completion be routed to a struct that lives in an await's frame.
 *
 * A finished coroutine's stack is not unmapped but pooled per thread, guard still armed, and
 * handed to the next ioxd__coro_create; past CORO_POOL_MAX idle stacks it is unmapped instead.
 *
 * Discipline: only the loop calls ioxd__coro_resume, only a coroutine calls ioxd__coro_yield. A coroutine
 * that wants to start another one hands it to the scheduler (ioxd__proactor_spawn); resuming from
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

#ifndef CORO_POOL_MAX
#define CORO_POOL_MAX 512           /* warm stacks kept per worker by default (ioxd_config.idle_stacks) */
#endif

/* Private to libioxd: hidden symbols cannot be interposed, so nothing outside the library can
 * substitute a scheduler primitive (the .S hides ioxd__coro_swap the same way). */
#define CORO_API __attribute__((visibility("hidden")))

typedef struct coro {
    void   *sp;               /* saved stack pointer while suspended                  */
    void   *stack;            /* mmap base; the low CORO_GUARD bytes are the guard    */
    size_t  size;             /* mapping size, guard included                         */
    void  (*fn)(void *);
    void   *arg;
    bool    done;             /* fn returned; the next resume-return recycles the stack */
    struct coro *next;        /* scheduler's ready-list link                          */
} coro_t;

/* Allocate a stack and forge its first frame. The descriptor lives at the top of that stack. */
CORO_API coro_t *ioxd__coro_create(void (*fn)(void *), void *arg, size_t stack_bytes);

/* Loop only. Run c until it yields; if it finished, its stack goes to the pool (or is unmapped
 * past CORO_POOL_MAX) before returning. */
CORO_API void ioxd__coro_resume(coro_t *c);

/* Coroutine only. Back to the loop; returns when the loop resumes this coroutine again. */
CORO_API void ioxd__coro_yield(void);

/* The running coroutine, nullptr on the loop stack. */
CORO_API coro_t *ioxd__coro_current(void);

/* Unmap the per-thread free list of pooled stacks. Call at worker teardown, on the worker thread. */
CORO_API void ioxd__coro_pool_drain(void);

/* How many idle stacks this thread keeps warm (CORO_POOL_MAX until told). Before the first create. */
CORO_API void ioxd__coro_pool_limit(unsigned max_idle);

/* ── coro.c: the notes ──────────────────────────────────────────────────────────────────── */

/* at file scope:
 *   - switch_x86_64.S  [extern void ioxd__coro_swap(void **save_sp, void *load_sp);]
 *   - the running coroutine; nullptr on the loop stack  [static thread_local coro_t *cur;]
 *   - the loop's stack pointer while a coroutine runs  [static thread_local void   *loop_sp;]
 *   - PROT_NONE below every stack. Big enough that a frame cannot step over it into the
 *     neighbour below: ioxd__conn_main's is 25 KB and ioxd__engine_serve's 10 KB, so one page would
 *     not do. Address space only - PROT_NONE pages have no RSS.
 *   - Pooled stacks keep their pages: no madvise(MADV_DONTNEED) on the way in. The trade is
 *     deliberate, a warm stack for the next connection against the RSS of an idle one, and the
 *     pool is capped.
 *   - free list of whole stack blocks, linked via ->next  [static thread_local coro_t
 *     *pool_head;]
 */

/* ioxd__coro_current:
 * The running coroutine, or nullptr on the loop stack.
 */

/* coro_entry:
 * First code a new coroutine runs, entered by the forged frame's ret. Never returns.
 *   - a finished coroutine must never be resumed  [abort();]
 */

/* ioxd__coro_create:
 * Get a stack - pooled, or freshly mapped with its guard - and forge its first frame so the
 * first switch into it 'returns' into coro_entry.
 *   - Every caller passes the worker's configured size, so the pool holds one size. A mismatch
 *     would mean a second size is in play and this reuse would hand back the wrong stack.  [if
 *     (pool_head->size != total) {]
 *   - a warm stack: guard still armed, no mmap, no mprotect  [c = pool_head;]
 *   - overflow faults here, not a neighbour  [if (mprotect(mem, guard, PROT_NONE) < 0) {]
 *   - the descriptor sits at the top of its own stack block, 16-aligned  [uintptr_t top =
 *     (uintptr_t)mem + total;]
 *   - Forge the frame ioxd__coro_swap expects: six callee-saved slots below a 16-aligned return slot
 *     that holds coro_entry. The first ioxd__coro_swap pops the six zeros and rets into coro_entry
 *     with rsp = slot + 8, which is 8 mod 16 - exactly the alignment a call would have left.
 *     The zero above the return slot is coro_entry's own (never used) return address, so
 *     backtraces end.  [uint64_t *sp = (uint64_t *)c;]
 *   - rbp rbx r12 r13 r14 r15  [*--sp = 0;]
 */

/* coro_destroy:
 * Pool a finished coroutine's stack for the next one (guard page still armed), or unmap it
 * past the cap. Not unmapping avoids a cross-core TLB shootdown per closed connection. The cap
 * bounds idle stacks kept, not how many coroutines may run.
 *   - the frame it named is gone; a stale sp must not be switched to  [c->sp = nullptr;]
 */

/* ioxd__coro_pool_drain:
 * Unmap every pooled stack. Call on the worker thread at teardown.
 */

/* ioxd__coro_resume:
 * Loop only: switch into c until it yields; if it finished, recycle its stack. Both rules are
 * checked in every build: an assert would go away under -DNDEBUG, and breaking either corrupts
 * the loop's saved stack pointer or switches to a stack that has been recycled - neither of
 * which shows up as anything but a crash somewhere else.
 */

/* ioxd__coro_yield:
 * Coroutine only: switch back to the loop; returns when the loop resumes this coroutine.
 */

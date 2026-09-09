#include "io/coro.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

extern void swap_ctx(void **save_sp, void *load_sp);     /* switch_x86_64.S */

static thread_local coro_t *cur;        /* the running coroutine; nullptr on the loop stack   */
static thread_local void   *loop_sp;    /* the loop's stack pointer while a coroutine runs */

#ifndef CORO_GUARD
#define CORO_GUARD (64UL * 1024)    /* PROT_NONE below every stack. Big enough that a frame cannot
                                     * step over it into the neighbour below: ioxd__conn_main's is
                                     * 25 KB and ioxd__serve's 10 KB, so one page would not do.
                                     * Address space only - PROT_NONE pages have no RSS. */
#endif
/* Pooled stacks keep their pages: no madvise(MADV_DONTNEED) on the way in. The trade is deliberate,
 * a warm stack for the next connection against the RSS of an idle one, and the pool is capped. */
static thread_local coro_t  *pool_head;  /* free list of whole stack blocks, linked via ->next */
static thread_local unsigned pool_count;
static thread_local unsigned pool_max = CORO_POOL_MAX;

void coro_pool_limit(unsigned max_idle)
{
    pool_max = max_idle;
}

/* The running coroutine, or nullptr on the loop stack. */
coro_t *coro_current(void)
{
    return cur;
}

/* First code a new coroutine runs, entered by the forged frame's ret. Never returns. */
static void coro_entry(void)
{
    coro_t *c = cur;
    c->fn(c->arg);
    c->done = true;
    coro_yield();
    abort();                        /* a finished coroutine must never be resumed */
}

/* Get a stack - pooled, or freshly mapped with its guard - and forge its first frame so the
 * first switch into it 'returns' into coro_entry. */
coro_t *coro_create(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    stack_bytes = (stack_bytes + page - 1) & ~(page - 1);
    size_t guard = (CORO_GUARD + page - 1) & ~(page - 1);
    size_t total = stack_bytes + guard;

    coro_t *c;
    if (pool_head) {
        /* Every caller passes the worker's configured size, so the pool holds one size. A mismatch
         * would mean a second size is in play and this reuse would hand back the wrong stack. */
        if (pool_head->size != total) {
            fprintf(stderr, "ioxd: coroutine stack size %zu does not match the pooled %zu\n",
                    total, pool_head->size);
            abort();
        }
        /* a warm stack: guard still armed, no mmap, no mprotect */
        c = pool_head;
        pool_head = c->next;
        pool_count--;
    } else {
        void *mem = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (mem == MAP_FAILED) {
            perror("mmap(stack)");
            abort();
        }
        if (mprotect(mem, guard, PROT_NONE) < 0) {           /* overflow faults here, not a neighbour */
            perror("mprotect(guard)");
            abort();
        }
        /* the descriptor sits at the top of its own stack block, 16-aligned */
        uintptr_t top = (uintptr_t)mem + total;
        c = (coro_t *)((top - sizeof *c) & ~(uintptr_t)15);
        c->stack = mem;
        c->size  = total;
    }

    c->fn   = fn;
    c->arg  = arg;
    c->done = false;
    c->next = nullptr;

    /* Forge the frame swap_ctx expects: six callee-saved slots below a 16-aligned return slot that
     * holds coro_entry. The first swap_ctx pops the six zeros and rets into coro_entry with
     * rsp = slot + 8, which is 8 mod 16 - exactly the alignment a call would have left. The zero
     * above the return slot is coro_entry's own (never used) return address, so backtraces end. */
    uint64_t *sp = (uint64_t *)c;
    *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)coro_entry;
    for (int i = 0; i < 6; i++)
        *--sp = 0;                                            /* rbp rbx r12 r13 r14 r15 */
    c->sp = sp;
    return c;
}

/* Pool a finished coroutine's stack for the next one (guard page still armed), or unmap it past
 * the cap. Not unmapping avoids a cross-core TLB shootdown per closed connection. The cap bounds
 * idle stacks kept, not how many coroutines may run. */
static void coro_destroy(coro_t *c)
{
    c->sp = nullptr;                /* the frame it named is gone; a stale sp must not be switched to */
    if (pool_count < pool_max) {
        c->next = pool_head;
        pool_head = c;
        pool_count++;
        return;
    }
    munmap(c->stack, c->size);
}

/* Unmap every pooled stack. Call on the worker thread at teardown. */
void coro_pool_drain(void)
{
    while (pool_head) {
        coro_t *c = pool_head;
        pool_head = c->next;
        munmap(c->stack, c->size);
    }
    pool_count = 0;
}

/* Loop only: switch into c until it yields; if it finished, recycle its stack. Both rules are
 * checked in every build: an assert would go away under -DNDEBUG, and breaking either corrupts the
 * loop's saved stack pointer or switches to a stack that has been recycled - neither of which
 * shows up as anything but a crash somewhere else. */
void coro_resume(coro_t *c)
{
    if (cur) {
        fprintf(stderr, "ioxd: coro_resume from inside a coroutine; only the loop resumes\n");
        abort();
    }
    if (c->done) {
        fprintf(stderr, "ioxd: coro_resume of a coroutine that already finished\n");
        abort();
    }
    cur = c;
    swap_ctx(&loop_sp, c->sp);
    cur = nullptr;
    if (c->done)
        coro_destroy(c);
}

/* Coroutine only: switch back to the loop; returns when the loop resumes this coroutine. */
void coro_yield(void)
{
    assert(cur != nullptr && "coro_yield needs a running coroutine");
    swap_ctx(&cur->sp, loop_sp);
}

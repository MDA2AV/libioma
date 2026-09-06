#define _GNU_SOURCE
#include "coro.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

extern void swap_ctx(void **save_sp, void *load_sp);     /* switch_x86_64.S */

static __thread coro_t *cur;        /* the running coroutine; NULL on the loop stack   */
static __thread void   *loop_sp;    /* the loop's stack pointer while a coroutine runs */

#ifndef CORO_POOL_MAX
#define CORO_POOL_MAX 512           /* warm stacks kept per worker, reused instead of munmap/mmap */
#endif
static __thread coro_t *pool_head;  /* free list of whole stack blocks, linked via ->next */
static __thread int     pool_count;

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

coro_t *coro_create(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    stack_bytes = (stack_bytes + page - 1) & ~(page - 1);
    size_t total = stack_bytes + page;                       /* plus the guard page */

    coro_t *c;
    if (pool_head && pool_head->size == total) {
        /* Reuse a warm stack: its guard page is still armed, so no mmap and no mprotect. This is
         * what makes connection churn cheap - the whole block, descriptor included, is recycled. */
        c = pool_head;
        pool_head = c->next;
        pool_count--;
    } else {
        void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (mem == MAP_FAILED) {
            perror("mmap(stack)");
            abort();
        }
        if (mprotect(mem, page, PROT_NONE) < 0) {            /* overflow faults here, not a neighbour */
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
    c->next = NULL;

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

/* A finished coroutine's stack goes on the free list for the next one, up to a cap. Not unmapping
 * it avoids the mmap/mprotect on the next create AND the cross-core TLB-shootdown a munmap costs in
 * a many-worker process - the dominant price of high connection churn. The cap bounds *idle*
 * stacks retained, not how many connections may be open: past it, extra stacks are unmapped. */
static void coro_destroy(coro_t *c)
{
    if (pool_count < CORO_POOL_MAX) {
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

void coro_resume(coro_t *c)
{
    assert(cur == NULL && "coro_resume is loop-only; a coroutine spawns, it never resumes");
    cur = c;
    swap_ctx(&loop_sp, c->sp);
    cur = NULL;
    if (c->done)
        coro_destroy(c);
}

void coro_yield(void)
{
    assert(cur != NULL && "coro_yield needs a running coroutine");
    swap_ctx(&cur->sp, loop_sp);
}

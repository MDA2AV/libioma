#include "io/coro.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

extern void ioxd__coro_swap(void **save_sp, void *load_sp);

static thread_local coro_t *cur;
static thread_local void   *loop_sp;

#ifndef CORO_GUARD
#define CORO_GUARD (64UL * 1024)
#endif

static thread_local coro_t  *pool_head;
static thread_local unsigned pool_count;
static thread_local unsigned pool_max = CORO_POOL_MAX;

void ioxd__coro_pool_limit(unsigned max_idle)
{
    pool_max = max_idle;
}

coro_t *ioxd__coro_current(void)
{
    return cur;
}

static void coro_entry(void)
{
    coro_t *c = cur;
    c->fn(c->arg);
    c->done = true;
    ioxd__coro_yield();
    abort();
}

coro_t *ioxd__coro_create(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    stack_bytes = (stack_bytes + page - 1) & ~(page - 1);
    size_t guard = (CORO_GUARD + page - 1) & ~(page - 1);
    size_t total = stack_bytes + guard;

    coro_t *c;
    if (pool_head) {
        if (pool_head->size != total) {
            fprintf(stderr, "ioxd: coroutine stack size %zu does not match the pooled %zu\n",
                    total, pool_head->size);
            abort();
        }

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
        if (mprotect(mem, guard, PROT_NONE) < 0) {
            perror("mprotect(guard)");
            abort();
        }

        uintptr_t top = (uintptr_t)mem + total;
        c = (coro_t *)((top - sizeof *c) & ~(uintptr_t)15);
        c->stack = mem;
        c->size  = total;
    }

    c->fn   = fn;
    c->arg  = arg;
    c->done = false;
    c->next = nullptr;

    uint64_t *sp = (uint64_t *)c;
    *--sp = 0;
    *--sp = (uintptr_t)coro_entry;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    c->sp = sp;
    return c;
}

static void coro_destroy(coro_t *c)
{
    c->sp = nullptr;
    if (pool_count < pool_max) {
        c->next = pool_head;
        pool_head = c;
        pool_count++;
        return;
    }
    munmap(c->stack, c->size);
}

void ioxd__coro_pool_drain(void)
{
    while (pool_head) {
        coro_t *c = pool_head;
        pool_head = c->next;
        munmap(c->stack, c->size);
    }
    pool_count = 0;
}

void ioxd__coro_resume(coro_t *c)
{
    if (cur) {
        fprintf(stderr, "ioxd: ioxd__coro_resume from inside a coroutine; only the loop resumes\n");
        abort();
    }
    if (c->done) {
        fprintf(stderr, "ioxd: ioxd__coro_resume of a coroutine that already finished\n");
        abort();
    }
    cur = c;
    ioxd__coro_swap(&loop_sp, c->sp);
    cur = nullptr;
    if (c->done)
        coro_destroy(c);
}

void ioxd__coro_yield(void)
{
    assert(cur != nullptr && "ioxd__coro_yield needs a running coroutine");
    ioxd__coro_swap(&cur->sp, loop_sp);
}

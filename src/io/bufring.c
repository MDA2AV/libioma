/*
 * bufring.c - the provided buffer ring: a slab of BUF_COUNT x BUF_SIZE bytes that the kernel
 * picks from when a multishot recv delivers data. Returning a buffer is inline in internal.h.
 */
#define _GNU_SOURCE
#include "io/internal.h"

#include <string.h>
#include <sys/mman.h>

/* Map anonymous read/write pages, or abort. */
static void *map_pages(size_t bytes)
{
    void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        abort();
    }
    return m;
}

/* Map the slab and the ring, register the ring as buffer group BGID, and offer every buffer. */
void ioma__bufring_init(proactor_t *p)
{
    p->buf_ring = map_pages((size_t)BUF_COUNT * sizeof(struct io_uring_buf));
    p->slab     = map_pages((size_t)BUF_COUNT * BUF_SIZE);

    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr    = (uint64_t)(uintptr_t)p->buf_ring;
    reg.ring_entries = BUF_COUNT;
    reg.bgid         = BGID;
    int rc = uring_register(&p->ring, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (rc < 0) {
        fprintf(stderr, "[w%d] register pbuf ring: %s\n", p->id, strerror(-rc));
        abort();
    }

    /* Fill every slot, then publish the tail once. bufs[0] overlaps the ring header and the tail
     * sits in bufs[0].resv, so writing only addr/len/bid leaves it untouched. */
    for (unsigned i = 0; i < BUF_COUNT; i++) {
        struct io_uring_buf *b = &p->buf_ring->bufs[i];
        b->addr = (uint64_t)(uintptr_t)(p->slab + (size_t)i * BUF_SIZE);
        b->len  = BUF_SIZE;
        b->bid  = (uint16_t)i;
    }
    p->buf_tail = BUF_COUNT;
    __atomic_store_n(&p->buf_ring->tail, (uint16_t)p->buf_tail, __ATOMIC_RELEASE);
}

/* Stage a buffer's return to the ring. The loop publishes the tail once per batch: one atomic
 * release for many returns, and the kernel is not re-reading a hot tail per request. */
void ioma__return_buf(proactor_t *p, uint16_t bid)
{
    struct io_uring_buf *b = &p->buf_ring->bufs[p->buf_tail & BUF_MASK];
    b->addr = (uint64_t)(uintptr_t)(p->slab + (size_t)bid * BUF_SIZE);
    b->len  = BUF_SIZE;
    b->bid  = bid;
    p->buf_tail++;
    p->buffers_returned = true;                   /* lets the loop re-arm starved recvs     */
    p->buf_dirty        = true;                   /* tail needs publishing before the enter */
}

/* Publish staged returns to the kernel. */
void ioma__bufring_publish(proactor_t *p)
{
    if (!p->buf_dirty)
        return;
    __atomic_store_n(&p->buf_ring->tail, (uint16_t)p->buf_tail, __ATOMIC_RELEASE);
    p->buf_dirty = false;
}

/* Unregister the group. Call before uring_exit. */
void ioma__bufring_unregister(proactor_t *p)
{
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.bgid = BGID;
    uring_register(&p->ring, IORING_UNREGISTER_PBUF_RING, &reg, 1);
}

/* Unmap the ring and the slab. Call after uring_exit, once no in-flight op can reference them. */
void ioma__bufring_unmap(proactor_t *p)
{
    munmap(p->buf_ring, (size_t)BUF_COUNT * sizeof(struct io_uring_buf));
    munmap(p->slab, (size_t)BUF_COUNT * BUF_SIZE);
}

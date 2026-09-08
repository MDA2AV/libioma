/*
 * bufring.c - the provided buffer ring of io/bufring.h.
 */
#include "io/bufring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Map anonymous read/write pages, or abort. */
static void *map_pages(size_t bytes)
{
    void *m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        abort();
    }
    return m;
}

/* Map the slab and the ring, register the ring as buffer group BGID, and offer every buffer. */
void ioxd__bufring_init(struct bufring *b, struct uring *ring, int worker)
{
    b->ring = map_pages((size_t)BUF_COUNT * sizeof(struct io_uring_buf));
    b->slab = map_pages((size_t)BUF_COUNT * BUF_SIZE);

    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr    = (uint64_t)(uintptr_t)b->ring;
    reg.ring_entries = BUF_COUNT;
    reg.bgid         = BGID;
    int rc = uring_register(ring, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (rc < 0) {
        fprintf(stderr, "[w%d] register pbuf ring: %s\n", worker, strerror(-rc));
        abort();
    }

    /* Fill every slot, then publish the tail once. bufs[0] overlaps the ring header and the tail
     * sits in bufs[0].resv, so writing only addr/len/bid leaves it untouched. */
    for (unsigned i = 0; i < BUF_COUNT; i++) {
        struct io_uring_buf *slot = &b->ring->bufs[i];
        slot->addr = (uint64_t)(uintptr_t)ioxd__bufring_at(b, (uint16_t)i);
        slot->len  = BUF_SIZE;
        slot->bid  = (uint16_t)i;
    }
    b->tail = BUF_COUNT;
    __atomic_store_n(&b->ring->tail, (uint16_t)b->tail, __ATOMIC_RELEASE);
}

/* Stage a buffer's return to the ring. The loop publishes the tail once per batch: one atomic
 * release for many returns, and the kernel is not re-reading a hot tail per request. */
void ioxd__bufring_return(struct bufring *b, uint16_t buf_id)
{
    struct io_uring_buf *slot = &b->ring->bufs[b->tail & BUF_MASK];
    slot->addr = (uint64_t)(uintptr_t)ioxd__bufring_at(b, buf_id);
    slot->len  = BUF_SIZE;
    slot->bid  = buf_id;
    b->tail++;
    b->returned = true;                           /* lets the loop re-arm starved recvs     */
    b->dirty    = true;                           /* tail needs publishing before the enter */
}

/* Publish staged returns to the kernel. */
void ioxd__bufring_publish(struct bufring *b)
{
    if (!b->dirty)
        return;
    __atomic_store_n(&b->ring->tail, (uint16_t)b->tail, __ATOMIC_RELEASE);
    b->dirty = false;
}

/* Unregister the group. Call before uring_exit. */
void ioxd__bufring_unregister(struct bufring *b, struct uring *ring)
{
    (void)b;
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.bgid = BGID;
    uring_register(ring, IORING_UNREGISTER_PBUF_RING, &reg, 1);
}

/* Unmap the ring and the slab. Call after uring_exit, once no in-flight op can reference them. */
void ioxd__bufring_unmap(struct bufring *b)
{
    munmap(b->ring, (size_t)BUF_COUNT * sizeof(struct io_uring_buf));
    munmap(b->slab, (size_t)BUF_COUNT * BUF_SIZE);
}

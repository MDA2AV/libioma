#include "io/bufring.h"
#include "io/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void *map_pages(size_t bytes)
{
    void *m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        abort();
    }
    return m;
}

[[noreturn]] void ioxd__bufring_bad_id(const struct bufring *b, uint16_t buf_id)
{
    fprintf(stderr, "ioxd: provided buffer id %u is outside the ring (%u buffers)\n", buf_id, b->count);
    abort();
}

void ioxd__bufring_init(struct bufring *b, struct uring *ring, int worker, unsigned count, unsigned size)
{
    b->count    = count;
    b->size     = size;
    b->mask     = count - 1;
    b->ring     = map_pages((size_t)count * sizeof(struct io_uring_buf));
    b->slab     = map_pages((size_t)count * size);
    b->dirty    = false;
    b->returned = 0;

    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr    = (uint64_t)(uintptr_t)b->ring;
    reg.ring_entries = count;
    reg.bgid         = BGID;
    int rc = uring_register(ring, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (rc < 0) {
        fprintf(stderr, "[w%d] register pbuf ring: %s\n", worker, ioxd__errstr(-rc));
        abort();
    }

    for (unsigned i = 0; i < count; i++) {
        struct io_uring_buf *slot = &b->ring->bufs[i];
        slot->addr = (uint64_t)(uintptr_t)ioxd__bufring_at(b, (uint16_t)i);
        slot->len  = size;
        slot->bid  = (uint16_t)i;
    }
    b->tail = count;
    __atomic_store_n(&b->ring->tail, (uint16_t)b->tail, __ATOMIC_RELEASE);
}

void ioxd__bufring_return(struct bufring *b, uint16_t buf_id)
{
    struct io_uring_buf *slot = &b->ring->bufs[b->tail & b->mask];
    slot->addr = (uint64_t)(uintptr_t)ioxd__bufring_at(b, buf_id);
    slot->len  = b->size;
    slot->bid  = buf_id;
    b->tail++;
    b->returned++;
    b->dirty = true;
}

void ioxd__bufring_publish(struct bufring *b)
{
    if (!b->dirty)
        return;
    __atomic_store_n(&b->ring->tail, (uint16_t)b->tail, __ATOMIC_RELEASE);
    b->dirty = false;
}

void ioxd__bufring_unregister(struct bufring *b, struct uring *ring)
{
    (void)b;
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.bgid = BGID;
    uring_register(ring, IORING_UNREGISTER_PBUF_RING, &reg, 1);
}

void ioxd__bufring_unmap(struct bufring *b)
{
    munmap(b->ring, (size_t)b->count * sizeof(struct io_uring_buf));
    munmap(b->slab, (size_t)b->count * b->size);
}

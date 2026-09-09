/*
 * bufring.h - the provided buffer ring: a slab of BUF_COUNT x BUF_SIZE bytes that the kernel
 * picks from when a multishot recv delivers data, registered as one buffer group, and how
 * buffers go back to it. One per worker, touched only by that worker's thread; returns are
 * staged and published to the kernel once per loop iteration.
 */
#pragma once

#include <stdint.h>

#include "io/uring.h"

/* tunables (override with -D) */
#ifndef BUF_COUNT
#define BUF_COUNT 4096                    /* provided recv buffers per worker, power of two     */
#endif
static_assert(((unsigned)BUF_COUNT & ((unsigned)BUF_COUNT - 1U)) == 0 && BUF_COUNT >= 2 && BUF_COUNT <= 32768,
              "BUF_COUNT: a power of two, at most 32768 (the kernel refuses a ring of 65536 entries)");
#ifndef BUF_SIZE
#define BUF_SIZE  2048                    /* bytes per recv buffer (a request rarely needs more) */
#endif
#define BUF_MASK  (BUF_COUNT - 1U)
#define BGID      1                       /* the one buffer group a worker registers            */

struct bufring {
    struct io_uring_buf_ring *ring;       /* kernel-shared ring of buffer descriptors           */
    uint8_t                  *slab;       /* BUF_COUNT x BUF_SIZE                               */
    unsigned                  tail;       /* local tail, published to ring->tail                */
    bool                      dirty;      /* staged returns awaiting one publish                */
    unsigned                  returned;   /* returns since the loop's last starved sweep        */
};

void ioxd__bufring_init      (struct bufring *b, struct uring *ring, int worker);   /* map, register, offer every buffer */
void ioxd__bufring_return    (struct bufring *b, uint16_t buf_id);                  /* stage a buffer's return           */
void ioxd__bufring_publish   (struct bufring *b);                                   /* the staged returns, one release   */
void ioxd__bufring_unregister(struct bufring *b, struct uring *ring);               /* before uring_exit                 */
void ioxd__bufring_unmap     (struct bufring *b);                                   /* after uring_exit                  */

[[noreturn]] void ioxd__bufring_bad_id(uint16_t buf_id);   /* a buffer id outside the slab: abort */

/* Where a buffer's bytes are. The id comes from the kernel, so it is checked: a wrong one would
 * hand the reader a pointer past the slab. One predicted branch, twice per request. */
static inline uint8_t *ioxd__bufring_at(const struct bufring *b, uint16_t buf_id)
{
    if (buf_id >= BUF_COUNT)
        ioxd__bufring_bad_id(buf_id);
    return b->slab + (size_t)buf_id * BUF_SIZE;
}

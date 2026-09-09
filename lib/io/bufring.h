/*
 * bufring.h - the provided buffer ring: a slab of count x size bytes that the kernel picks from
 * when a multishot recv delivers data, registered as one buffer group, and how buffers go back to
 * it. One per worker, touched only by that worker's thread; returns are staged and published to
 * the kernel once per loop iteration. The count and the size are the worker's configuration
 * (ioxd_config); BUF_COUNT and BUF_SIZE are their defaults.
 */
#pragma once

#include <stdint.h>

#include "io/uring.h"

/* defaults (override with -D, or at run time with ioxd_configure) */
#ifndef BUF_COUNT
#define BUF_COUNT 4096                    /* provided recv buffers per worker, power of two     */
#endif
static_assert(((unsigned)BUF_COUNT & ((unsigned)BUF_COUNT - 1U)) == 0 && BUF_COUNT >= 2 && BUF_COUNT <= 32768,
              "BUF_COUNT: a power of two, at most 32768 (the kernel refuses a ring of 65536 entries)");
#ifndef BUF_SIZE
#define BUF_SIZE  2048                    /* bytes per recv buffer (a request rarely needs more) */
#endif
#define BGID      1                       /* the one buffer group a worker registers            */

struct bufring {
    struct io_uring_buf_ring *ring;       /* kernel-shared ring of buffer descriptors           */
    uint8_t                  *slab;       /* count x size bytes                                 */
    unsigned                  count;      /* buffers: a power of two                            */
    unsigned                  size;       /* bytes in each                                      */
    unsigned                  mask;       /* count - 1: the ring index of a tail                */
    unsigned                  tail;       /* local tail, published to ring->tail                */
    bool                      dirty;      /* staged returns awaiting one publish                */
    unsigned                  returned;   /* returns since the loop's last starved sweep        */
};

void ioxd__bufring_init      (struct bufring *b, struct uring *ring, int worker, unsigned count, unsigned size);   /* map, register, offer every buffer */
void ioxd__bufring_return    (struct bufring *b, uint16_t buf_id);                  /* stage a buffer's return           */
void ioxd__bufring_publish   (struct bufring *b);                                   /* the staged returns, one release   */
void ioxd__bufring_unregister(struct bufring *b, struct uring *ring);               /* before uring_exit                 */
void ioxd__bufring_unmap     (struct bufring *b);                                   /* after uring_exit                  */

[[noreturn]] void ioxd__bufring_bad_id(const struct bufring *b, uint16_t buf_id);   /* a buffer id outside the slab: abort */

/* Where a buffer's bytes are. The id comes from the kernel, so it is checked: a wrong one would
 * hand the reader a pointer past the slab. One predicted branch, twice per request. */
static inline uint8_t *ioxd__bufring_at(const struct bufring *b, uint16_t buf_id)
{
    if (buf_id >= b->count)
        ioxd__bufring_bad_id(b, buf_id);
    return b->slab + (size_t)buf_id * b->size;
}

/* ── bufring.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * bufring.c - the provided buffer ring of io/bufring.h.
 */

/* map_pages:
 * Map anonymous read/write pages, or abort.
 */

/* ioxd__bufring_bad_id:
 * The kernel named a buffer we never offered: the slab pointer it implies is outside the
 * mapping, so there is nothing safe to do with it.
 */

/* ioxd__bufring_init:
 * Map the slab and the ring, register the ring as buffer group BGID, and offer every buffer.
 * count is a power of two no larger than 32768: ioxd_configure checked it.
 *   - Fill every slot, then publish the tail once. bufs[0] overlaps the ring header and the
 *     tail sits in bufs[0].resv, so writing only addr/len/bid leaves it untouched.  [for
 *     (unsigned i = 0; i < count; i++) {]
 */

/* ioxd__bufring_return:
 * Stage a buffer's return to the ring. The loop publishes the tail once per batch: one atomic
 * release for many returns, and the kernel is not re-reading a hot tail per request.
 *   - how many recvs the loop may re-arm  [b->returned++;]
 *   - tail needs publishing before the enter  [b->dirty = true;]
 */

/* ioxd__bufring_publish:
 * Publish staged returns to the kernel.
 */

/* ioxd__bufring_unregister:
 * Unregister the group. Call before uring_exit.
 */

/* ioxd__bufring_unmap:
 * Unmap the ring and the slab. Call after uring_exit, once no in-flight op can reference them.
 */

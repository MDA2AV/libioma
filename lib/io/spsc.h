/*
 * io/spsc.h - a single-producer, single-consumer ring of received buffers: an array whose size
 * is a power of two, a head and a tail that only ever grow, and a mask that turns them into
 * slots. The producer is the recv completion, the consumer the connection's coroutine, and both
 * run on the worker's thread in turn, so the two indices are plain integers: an SPSC ring
 * shared between threads would publish the tail with a release store and read it with an
 * acquire load, and the head the other way - the layout would not change, only the counters
 * would become atomics.
 *
 * RX_QUEUE slots live inline, and are all a connection normally needs: at that many queued
 * buffers its recv is paused (conn.c), so the kernel stops filling more for it. What it had
 * already posted before the pause landed still has to be kept - nothing delivered may be lost -
 * so a full ring doubles onto the heap for that burst, and goes back to the inline slots when
 * the connection is recycled.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef RX_QUEUE
#define RX_QUEUE 64                       /* delivered buffers a connection holds before its recv is paused; a power of two */
#endif
static_assert(((unsigned)RX_QUEUE & ((unsigned)RX_QUEUE - 1U)) == 0 && RX_QUEUE >= 2,
              "RX_QUEUE: a power of two (the ring indexes with a mask over it)");

/* A delivered buffer: where its bytes are, how many, and the ring id to give it back with. */
struct rx_item {
    const void *ptr;
    uint32_t    len;
    uint16_t    buf_id;
};

struct spsc {
    struct rx_item *items;                /* the inline slots, or a larger ring on the heap   */
    unsigned        mask;                 /* the ring's size less one: index & mask is a slot */
    unsigned        head;                 /* the consumer's: the next item to take           */
    unsigned        tail;                 /* the producer's: the next slot to fill           */
    struct rx_item  slots[RX_QUEUE];
};

/* Empty, on the inline slots; a ring grown onto the heap is given back. A zeroed struct too. */
static inline void ioxd__spsc_reset(struct spsc *q)
{
    if (q->items && q->items != q->slots)
        free(q->items);
    q->items = q->slots;
    q->mask  = RX_QUEUE - 1U;
    q->head = q->tail = 0;
}
static inline unsigned ioxd__spsc_count(const struct spsc *q)
{
    return q->tail - q->head;
}
static inline bool ioxd__spsc_empty(const struct spsc *q)
{
    return q->tail == q->head;
}
static inline bool ioxd__spsc_full(const struct spsc *q)
{
    return q->tail - q->head == q->mask + 1U;
}
/* Twice the ring, with the queued items in order at its front. False when the heap refuses. */
static inline bool ioxd__spsc_grow(struct spsc *q)
{
    unsigned        count = q->tail - q->head, size = (q->mask + 1U) * 2U;
    struct rx_item *ring  = malloc(size * sizeof *ring);
    if (!ring)
        return false;
    for (unsigned i = 0; i < count; i++)
        ring[i] = q->items[(q->head + i) & q->mask];
    if (q->items != q->slots)
        free(q->items);
    q->items = ring;
    q->mask  = size - 1U;
    q->head  = 0;
    q->tail  = count;
    return true;
}
/* The producer's slot to fill; the caller checked it is not full, or grew it. */
static inline struct rx_item *ioxd__spsc_push(struct spsc *q)
{
    return &q->items[q->tail++ & q->mask];
}
/* The consumer's next item; the caller checked it is not empty. */
static inline struct rx_item ioxd__spsc_pop(struct spsc *q)
{
    return q->items[q->head++ & q->mask];
}

/*
 * io/spsc.h - a single-producer, single-consumer ring of received buffers: a fixed array whose
 * size is a power of two, a head and a tail that only ever grow, and a mask that turns them
 * into slots. Nothing is moved, allocated or reset. The producer is the recv completion, the
 * consumer the connection's coroutine, and both run on the worker's thread in turn, so the two
 * indices are plain integers: an SPSC ring shared between threads would publish the tail with
 * a release store and read it with an acquire load, and the head the other way - the layout
 * would not change, only the counters would become atomics.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef RX_QUEUE
#define RX_QUEUE 64                       /* delivered buffers one connection may hold, a power of two */
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
    struct rx_item items[RX_QUEUE];
    unsigned       head;                  /* the consumer's: the next item to take           */
    unsigned       tail;                  /* the producer's: the next slot to fill           */
};

static inline void ioxd__spsc_reset(struct spsc *q)
{
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
    return q->tail - q->head == RX_QUEUE;
}
/* The producer's slot to fill; the caller checked it is not full. */
static inline struct rx_item *ioxd__spsc_push(struct spsc *q)
{
    return &q->items[q->tail++ & (RX_QUEUE - 1U)];
}
/* The consumer's next item; the caller checked it is not empty. */
static inline struct rx_item ioxd__spsc_pop(struct spsc *q)
{
    return q->items[q->head++ & (RX_QUEUE - 1U)];
}
static inline const struct rx_item *ioxd__spsc_peek(const struct spsc *q)
{
    return &q->items[q->head & (RX_QUEUE - 1U)];
}

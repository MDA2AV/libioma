# Streams and pipes over the I/O plane (design, branch `streams`)

## Why

Today the I/O plane offers two primitives, `await_recv` and `await_send`, and the HTTP engine
builds everything else on them: a 16 KB request buffer it fills and parses, a reply slab it
appends to and flushes. Both are streams in all but name. Naming them, and moving them below the
HTTP engine, gives three things:

1. a transport-independent surface, so a raw TCP server, a WebSocket upgrade or a TLS layer
   later do not need to know the engine;
2. the reply side formalised as a writer: reserve space in the slab, write into it, advance,
   flush - what the arena handler already does by hand;
3. the request side as a reader over the kernel-filled provided buffers, which is where the one
   copy left on the hot path lives (provided buffer -> request buffer) and where a zero-copy
   parse of the common single-segment head becomes possible.

The shape is System.IO.Pipelines: Kestrel is transport -> PipeReader/PipeWriter -> parser, and
this runtime has the same layering, with coroutines instead of tasks. "Async" here means "may
suspend the coroutine"; the caller does not see it.

## The pieces

**`ioma_writer`** - the slab with a head reserve, a tail, and a connection to flush to.

    void  *ioma_writer_reserve(w, size_t n);   /* n bytes at the tail, flushing first if they do not fit */
    void   ioma_writer_advance(w, size_t n);   /* the caller wrote n of them */
    int    ioma_writer_write  (w, data, n);    /* copy in (reserve + memcpy + advance)                */
    int    ioma_writer_flush  (w);             /* send what is in the slab; suspends                   */
    int    ioma_writer_send   (w, data, n);    /* write + flush                                        */

The HTTP response keeps its head-building on top (first flush builds the head into the lead).
`ioma_write`, `ioma_printf`, `ioma_flush` become thin calls; `ioma_reserve`/`ioma_advance` are new
on the context, for handlers that format straight into the reply.

**`ioma_reader`** - the connection's received bytes as segments, consumed in place.

    struct ioma_segment { const char *p; size_t len; };
    int    ioma_reader_read   (r, const struct ioma_segment **segs, size_t *n);  /* what is buffered; waits for more when nothing is; 0 at EOF, -1 on error */
    void   ioma_reader_advance(r, size_t consumed, size_t examined);              /* consumed: returned to the ring; examined: do not wake me until more than this arrives */
    int    ioma_reader_copy   (r, void *dst, size_t n);                          /* the Stream-style read: up to n bytes into dst */

The segments are the provided buffers as the kernel filled them (2 KB each, in order). A parser
that needs contiguity coalesces: parse in place when the data sits in one segment, copy into a
scratch buffer when it spans. `consumed`/`examined` are Pipelines' contract and what makes
"read until a full head is here" cheap: the reader does not wake the coroutine for a partial
head it already examined.

**`ioma_stream`** - one connection's reader and writer together, what a handler of a non-HTTP
protocol would receive.

## The HTTP engine on top

- `read_head`: `reader_read`; if the head ends inside the first segment, `phr_parse_request`
  runs on the segment itself - no copy - and the request's slices point into the provided
  buffer, which stays owned by the connection until the reply is finished (then `advance`
  returns it). If the head spans segments, coalesce into the request buffer as today.
- Body reads (`ioma_body_all`, `read_until`, `read_next_chunk`) walk segments through the same
  chunk parser; the whole read still needs contiguous output, so it copies when the body spans.
- Pipelining falls out: the bytes after a request are simply not consumed.
- The reply: unchanged behaviour, on the writer.

## Costs and gates

- Holding a provided buffer for the life of a request (instead of copying and returning it at
  once) means a slow handler pins one 2 KB buffer per in-flight request. 4096 per worker; the
  -ENOBUFS parking already handles exhaustion. Measure under the 500k profile.
- An extra indirection per read/write is nothing next to a syscall, but the reader's segment
  bookkeeping must not add branches to the single-segment fast path. Gate: keep-alive and churn
  on 4 reactors within noise of main, then the arena profiles.
- No vtable in the first cut: one transport. TLS or a test transport can come as a compile-time
  layer or a later vtable once there is a second implementation to justify it.

## Order of work

1. Writer: reserve/advance/write/flush/send in the io plane; the response on it;
   `ioma_reserve`/`ioma_advance` public. Small, no behaviour change, measurable.
2. Reader with `copy` only (Stream semantics) and the engine's `read_head` and body stage on it,
   still copying. Same behaviour, same numbers expected. Removes `await_recv` from the engine.
3. Segments + `advance(consumed, examined)`, then the zero-copy head parse. This is the step with
   a payoff and the risk; it gets the fragmentation validator and the raw-socket smoke tests.
4. `ioma_stream` public, with a raw TCP entry point, once the HTTP engine is a clean client of it.

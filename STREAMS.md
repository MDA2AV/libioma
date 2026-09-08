# Streams and pipes over the I/O plane (design, branch `streams`)

**Status.** Steps 1 to 4 below are on the branch: `io/pipe.h` (reader, writer), the HTTP engine
reading through the reader with the in-place fast path, and the public `ioma_pipe` with
`ioma_run_pipes`, a line-echo fixture and its tests. The reader's verbs ended up as examine /
drop / keep / copy rather than one `advance(consumed, examined)`, because kept bytes - the
request model's slices - need to stay put, which Pipelines has no notion of. Open, to talk
about: the public surface (run_begin/run stay private for now); outbound connections
(`ioma_connect` on IORING_OP_CONNECT, the same pipe over a socket to a database or a peer);
a second transport (QUIC, TLS) and whether that wants a vtable; letting the live bytes sit in a
second kernel buffer while a run continues in the first, so streamed bodies never copy twice.

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

**`ioma_pipewriter`** - the slab with a head reserve, a tail, and a connection to flush to.

    void  *ioma_pipewriter_reserve(w, size_t n);   /* n bytes at the tail, flushing first if they do not fit */
    void   ioma_pipewriter_advance(w, size_t n);   /* the caller wrote n of them */
    int    ioma_pipewriter_write  (w, data, n);    /* copy in (reserve + memcpy + advance)                */
    int    ioma_pipewriter_flush  (w);             /* send what is in the slab; suspends                   */
    int    ioma_pipewriter_send   (w, data, n);    /* write + flush                                        */

The HTTP response keeps its head-building on top (first flush builds the head into the lead).
`ioma_write`, `ioma_printf`, `ioma_flush` become thin calls; `ioma_reserve`/`ioma_advance` are new
on the context, for handlers that format straight into the reply.

**`ioma_pipereader`** - the connection's received bytes, one contiguous span at a time.

    int    ioma_pipereader_read   (r, ioma_slice *span);        /* what is buffered, contiguous; waits for more when the caller examined it all; 0 at EOF, -1 on error */
    void   ioma_pipereader_advance(r, size_t consumed, size_t examined);   /* consumed: gone (provided buffers returned to the ring); examined: do not wake me until more than this arrives */
    int    ioma_pipereader_copy   (r, void *dst, size_t n);     /* the Stream-style read: up to n bytes into dst */

Why one span and not a list of segments. io_uring hands data over as provided buffers, each a
pointer and a length, and the reader keeps them as such internally (the rx queue does already).
But every consumer needs contiguous bytes: picohttpparser takes one buffer, the chunk parser
takes one buffer, and the request model hands handlers `ioma_slice`s, which are contiguous by
definition - a header value split across two provided buffers cannot be a slice without a
copy. A segment-aware parser would not remove that copy, only move it, and the parser would be
ours to write and to keep fast. So the reader coalesces, and it does so lazily:

- when everything buffered lies in one provided buffer - a whole request head in one receive,
  the common case - `read` returns that buffer's bytes in place: zero copy, the buffer stays
  owned by the connection until `advance` consumes past it;
- when the data spans buffers, `read` copies the pieces into the connection's request buffer
  (16 KB, on the coroutine's stack: no allocation) and returns that. Spanning requests pay the
  copy they pay today; nothing else does.

That is Kestrel's `IsSingleSegment` fast path, kept inside the reader rather than in every
parser, and `advance(consumed, examined)` is Pipelines' contract: `consumed` releases buffers,
`examined` keeps `read` from returning the same incomplete head twice - it suspends until more
arrives instead.

**`ioma_stream`** - one connection's reader and writer together, what a handler of a non-HTTP
protocol would receive.

## The HTTP engine on top

- `read_head`: `reader_read` gives a span; `phr_parse_request` runs on it. In the common case
  that is the provided buffer itself - no copy - and the request's slices point into it; the
  buffer stays owned by the connection until the reply is finished, when `advance` returns it.
  A head that spans receives arrives coalesced in the request buffer, as today.
- Body reads (`ioma_body_all`, `read_until`, `read_next_chunk`) run the chunk parser on spans
  the same way; the whole read wants contiguous output and gets it from the same coalescing.
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
3. Spans + `advance(consumed, examined)`, then the zero-copy single-buffer path. This is the
   step with a payoff and the risk; it gets the fragmentation validator and the raw-socket
   smoke tests.
4. `ioma_stream` public, with a raw TCP entry point, once the HTTP engine is a clean client of it.

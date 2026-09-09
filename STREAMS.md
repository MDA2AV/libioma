# Streams and pipes over the I/O plane (design, branch `streams`)

**Status.** Steps 1 to 4 below are on the branch: `io/pipe.h` (reader, writer), the HTTP engine
reading through the reader with the in-place fast path, and the public `ioxd_pipe` with
`ioxd_run_pipes`, a line-echo fixture and its tests. The reader's verbs ended up as read /
examine / drop / keep / release / copy, with run_begin and run below them, rather than one
`advance(consumed, examined)`, because kept bytes - the request model's slices - need to stay
put, which Pipelines has no notion of. TLS arrived as a prologue over the same pipe rather than
as a second transport (TLS.md), so no vtable was needed. Open, to talk about: the public surface
(run_begin/run stay private for now); outbound connections (`ioxd_connect` on
IORING_OP_CONNECT, the same pipe over a socket to a database or a peer); QUIC, and whether that
wants a vtable; letting the live bytes sit in a second kernel buffer while a run continues in
the first, so streamed bodies never copy twice.

## Why

Before this work the I/O plane offered two primitives, a recv await and a send await, and the
HTTP engine built everything else on them: a 16 KB request buffer it filled and parsed, a reply
slab it appended to and flushed. Both are streams in all but name. Naming them, and moving them below the
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

**`ioxd_pipewriter`** - the slab with a head reserve, a tail, and a connection to flush to.

    void  *ioxd_pipewriter_reserve(w, size_t n);   /* n bytes at the tail, flushing first if they do not fit */
    void   ioxd_pipewriter_advance(w, size_t n);   /* the caller wrote n of them */
    int    ioxd_pipewriter_write  (w, data, n);    /* copy in (reserve + memcpy + advance)                */
    int    ioxd_pipewriter_flush  (w);             /* send what is in the slab; suspends                   */
    int    ioxd_pipewriter_send   (w, data, n);    /* write + flush                                        */

The HTTP response keeps its head-building on top (first flush builds the head into the lead).
`ioxd_write`, `ioxd_printf`, `ioxd_flush` become thin calls; `ioxd_reserve`/`ioxd_advance` are new
on the context, for handlers that format straight into the reply.

**`ioxd_pipereader`** - the connection's received bytes, one contiguous span at a time.

    int         ioxd_pipereader_read     (r, ioxd_slice *live);  /* the live bytes, contiguous, once some are unexamined; waits for more otherwise; 0 at EOF, <0 on error */
    void        ioxd_pipereader_examine  (r, size_t n);          /* looked at n of them: do not hand back the same bytes, wait for more */
    void        ioxd_pipereader_drop     (r, size_t n);          /* consume n (clamped to what is live); the kernel buffer goes back once nothing is left in it */
    const char *ioxd_pipereader_keep     (r, size_t n);          /* consume n but leave them where they are, contiguous with the run; where they are, or NULL: no room, or n past the live bytes */
    void        ioxd_pipereader_run_begin(r);                    /* freeze the run in progress; the next keep starts another */
    ioxd_slice  ioxd_pipereader_run      (r);                    /* the run in progress, wherever it ended up */
    void        ioxd_pipereader_release  (r);                    /* forget every kept byte; the live ones stay */
    int         ioxd_pipereader_copy     (r, void *dst, size_t n);   /* the Stream-style read: up to n bytes into dst */

Why one span and not a list of segments. io_uring hands data over as provided buffers, each a
pointer and a length, and the reader keeps them as such internally (the rx queue does already).
But every consumer needs contiguous bytes: picohttpparser takes one buffer, the chunk parser
takes one buffer, and the request model hands handlers `ioxd_slice`s, which are contiguous by
definition - a header value split across two provided buffers cannot be a slice without a
copy. A segment-aware parser would not remove that copy, only move it, and the parser would be
ours to write and to keep fast. So the reader coalesces, and it does so lazily:

- when everything buffered lies in one provided buffer - a whole request head in one receive,
  the common case - `read` returns that buffer's bytes in place: zero copy, the buffer stays
  owned by the connection until `drop` consumes past it;
- when the data spans buffers, `read` copies the pieces into the connection's gathering buffer
  (16 KB, on the coroutine's stack: no allocation) and returns that. Spanning requests pay the
  copy they pay today; nothing else does.

That is Kestrel's `IsSingleSegment` fast path, kept inside the reader rather than in every
parser. Pipelines' one `advance(consumed, examined)` became two verbs, `drop` and `examine`, plus
`keep` for the third case Pipelines has no name for: `drop` releases buffers, `examine` keeps
`read` from returning the same incomplete head twice - it suspends until more arrives instead - and
`keep` consumes without moving, because the request model's slices have to stay valid. Kept bytes
form a run; `run_begin` closes one off and pins the kernel buffer it sits in, so pointers already
handed out stay good until `release`.

**`ioxd_pipe`** - one connection's reader and writer together, what a handler of a non-HTTP
protocol receives (`include/ioxd/pipe.h` for the public verbs, `lib/io/pipe.h` for the rest).

## The HTTP engine on top

- `read_head`: `reader_read` gives a span; `phr_parse_request` runs on it. In the common case
  that is the provided buffer itself - no copy - and the request's slices point into it; the
  head is `keep`t, so the buffer stays owned by the connection until the reply is finished, when
  `release` returns it. A head that spans receives arrives coalesced in the gathering buffer.
- Body reads (`ioxd_body_all`, `read_until`, `read_next_chunk`) run the chunk parser on spans
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
   `ioxd_reserve`/`ioxd_advance` public. Small, no behaviour change, measurable.
2. Reader with `copy` only (Stream semantics) and the engine's `read_head` and body stage on it,
   still copying. Same behaviour, same numbers expected. Removes the recv await from the engine.
3. Spans + examine/drop/keep, then the zero-copy single-buffer path. This is the
   step with a payoff and the risk; it gets the fragmentation validator and the raw-socket
   smoke tests.
4. `ioxd_pipe` public, with a raw TCP entry point, once the HTTP engine is a clean client of it.

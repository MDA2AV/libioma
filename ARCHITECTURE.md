# How libioxd works

libioxd is an HTTP/1.1 server library in C. It runs one worker per CPU core, each worker owns
its own io_uring, and every connection is served by a small coroutine that suspends while the
kernel does the I/O. There are no locks and nothing is shared between workers.

```
process
 ├─ worker 0 (thread pinned to CPU 0)          ├─ worker 1 (CPU 1) ...
 │   ├─ io_uring            one ring, one syscall per batch
 │   ├─ SO_REUSEPORT listeners  one per port; the kernel spreads connections across workers
 │   ├─ provided buffer ring    where the kernel puts received bytes
 │   ├─ coroutine per connection   runs ioxd__serve(): parse → handler → reply
 │   └─ pools: conn objects, coroutine stacks
```

`include/ioxd.h` is the whole public API, an umbrella over `include/ioxd/`: `slice.h` (slices and
conversions), `http.h` (request, response, context, body, reply, `ioxd_run`), `router.h` (groups,
endpoints, middleware, the script macros), `json.h` (the writer and `IOXD_JSON_STRUCT`), `pipe.h`
(a connection as a pipe) and `tls.h` (a certificate store), one per concern, each usable on its
own. Under `lib/` there are three planes, one concern per file: `io/` is the I/O plane -
`uring.c` (the ring), `coro.c` + `switch_x86_64.S` (coroutines), `bufring.c` (the buffer ring),
`conn.c` (a connection and its awaits), `pipe.c` (the reader and the writer), `proactor.c` (the
worker loop), with `proactor.h` as the interface the other planes use; `http/` is the HTTP plane -
`engine.c` (parse, body, reply, the serve loop), `router.c` (routes and middleware), `api.c`
(handler helpers), `run.c` (`ioxd_bind`, `ioxd_run`, `ioxd_run_pipes`); and `tls/` is the TLS
prologue - `store.c` (certificates, SNI, reload) and `handshake.c` (the OpenSSL handshake and the
handoff to the kernel). `json/json.c` is the JSON writer, which depends on neither plane. Each
`io/` and `http/` each have an `internal.h` for what their files share and no module owns; `tls/` has none - `store.h` and `handshake.h` are what its two files need from each other, and the runner includes the latter.

---

## 1. io_uring, without liburing

io_uring is two ring buffers shared with the kernel. You write **submission entries** (SQEs,
"do this") into one ring, the kernel writes **completion entries** (CQEs, "this finished, result
N") into the other. One syscall, `io_uring_enter`, both submits and waits.

libioxd talks to it directly (`uring.c`), the way ioxide does, with three setup flags:

- **SINGLE_ISSUER** — only this thread submits, so the kernel skips its SQ locking.
- **DEFER_TASKRUN** — completion work runs batched inside our `enter` call, never as an interrupt
  on some other CPU. Completions arrive exactly when we ask for them.
- **NO_SQARRAY** — slot *i* of the SQ is SQE *i*; one indirection less per submission.

Both rings live in one `mmap`. Claiming an SQE bumps a local tail; the tail is published to the
kernel once, right before `enter`. Draining works the same way in reverse: read the CQ tail once,
handle every CQE in the batch, publish the head once. One barrier per batch, not per entry.

The worker's `enter` waits for at least one completion, with a 100 ms timeout so it can notice the
stop flag when idle. When completions are already pending the kernel returns without arming any
timer, so the timeout costs nothing under load.

### Multishot: arm once, complete many times

Two operations are armed once and keep completing:

- **Multishot accept** on the listener: one SQE, then one CQE per new connection.
- **Multishot recv** on every connection: one SQE, then one CQE per chunk of data that arrives.

Without multishot you would resubmit a recv after every read. With it a connection costs one
submission for its whole life, and the kernel tells you with a flag (`F_MORE`) whether the
operation is still live.

### Provided buffers

A multishot recv cannot point at *your* buffer, because the kernel does not know when data will
arrive. Instead each worker registers a **buffer ring**: `BUF_COUNT` slots of `BUF_SIZE` bytes
(2 KB each). The kernel picks a free buffer when data lands and reports its id in the CQE. When
the handler has consumed it, the worker hands it back by writing its id into the ring again.

Returns are staged and the ring tail is published once per loop iteration, so a batch of returned
buffers costs one atomic store. If the ring runs dry a recv ends with `-ENOBUFS`; the worker logs it
(once a second at most, with counts, so a starved ring is visible and `recv_buffers` can be raised) and the connection is
parked and re-armed as soon as any buffer comes back.

---

## 2. Coroutines

A **stackful coroutine** is a function running on its own stack that can pause in the middle
(`coro_yield`) and be continued later (`coro_resume`), exactly where it left off, with all its local
variables intact. That is what lets a handler be written as plain sequential code:

```c
ioxd_pipe_read(pipe, &live);          /* pauses here until data arrives */
ioxd_pipe_send(pipe, reply, len);     /* pauses here until the send completes */
```

### The switch

`swap_ctx` in `switch_x86_64.S` is 15 instructions. It pushes the six callee-saved registers
(`rbp rbx r12-r15`), stores the stack pointer into the old coroutine, loads the other stack
pointer, pops six registers, and `ret`s into whatever that stack was doing. The C compiler already
saved every other register at the call site, so nothing else needs saving. A switch costs a few
nanoseconds.

### Creating one

`coro_create` maps a 128 KB stack with a 64 KB unmapped **guard** below it (a stack overflow faults
instead of corrupting a neighbour; one page would not do, since `ioxd__conn_main`'s frame is 25 KB
and `ioxd__serve`'s 10 KB, and a frame that big could step clean over it). `PROT_NONE` pages cost
address space and no RSS. The `coro_t` descriptor sits at the top of that same block.
Then it forges a first frame: six zeros and a return address pointing at `coro_entry`. The first
`swap_ctx` into it pops the zeros and "returns" into `coro_entry`, which calls the coroutine's
function. When the function returns, `coro_entry` marks it done and yields for the last time.

### The rules

- Only the **loop** resumes coroutines. A coroutine never resumes another; it *spawns* one, which
  goes on a ready list the loop drains. Two thread-locals track the current coroutine and the
  loop's saved stack pointer, and that is the whole scheduler.
- A finished coroutine's stack goes into a per-worker **pool** (guard still armed) and is
  reused by the next connection, so churn pays no `mmap`/`munmap` and no cross-core TLB shootdown.
  Past `CORO_POOL_MAX` idle stacks the extra ones are unmapped; the cap bounds what is kept warm,
  not how many coroutines may run.

---

## 3. The proactor (the worker loop)

Each worker thread runs `proactor_run`: pin to a CPU, create the ring and buffer ring, open one
socket per listener, arm each multishot accept, then loop:

```
loop:
  stop?            the flag is read once: begin_drain, then run until the last connection ends
  run_ready        start coroutines spawned since last time
  rearm_starved    re-arm recvs that hit -ENOBUFS, if buffers came back
  publish          buffer returns staged during the last batch
  enter            submit everything staged, wait for >= 1 completion   (the one syscall)
  dispatch         read the CQ tail once, then copy out and handle one CQE at a time;
                   handlers resume inline
  advance          publish the CQ head once, for everything taken this batch
  rearm_stalled    a close may have made room for an accept that ran out of it
```

Handlers run *inside* dispatch, on their own stacks. A resumed handler that reaches `await_send`
stages a SEND SQE and parks; the SQE rides the next `enter` together with the rest of the batch.
Each CQE is copied out of the ring before its handler runs, since the handler may need the slot:
`ioxd__sqe` enters again when the SQ fills mid-batch, and it publishes the CQ head first, so the
kernel has somewhere to put what that enter completes. The head is published once per batch
otherwise, which is the one barrier the batch costs.

An accept that runs out of descriptors, file slots or memory is not re-armed at once - that would
spin - so its listener stalls; `rearm_stalled` arms it again once a connection has closed, or once
a second, whichever comes first.

### Routing a completion

Every SQE carries a 64-bit `user_data`. libioxd stores a pointer in it with a 3-bit **tag** in the
low bits (everything it points at is 8-byte aligned):

| tag | points at | meaning |
|---|---|---|
| `IGNORE` (0) | – | a cancel acknowledgement |
| `OP` | an `op_t` on the awaiting coroutine's stack | a one-shot op (send, recv-exact, setsockopt, sendmsg) finished: resume that coroutine |
| `RECV` | the connection | multishot recv delivered data, or ended |
| `ACCEPT` | the `struct listener` | a new connection on that port |
| `CLOSE` | – | a socket closed; only a failure is news |
| `DRAIN` | the worker | the shutdown's one blanket cancel: its result says whether the kernel knew it |

`IGNORE` is the zero tag on purpose: an SQE whose `user_data` was never set then dispatches as
"nobody waits for this" instead of as an op with a null pointer.

The `op_t` can live on the coroutine's stack because a parked coroutine's frame is frozen: its
address stays valid for exactly as long as the operation is in flight.

### A connection's life

Accept → take a `conn_t` from the pool → arm its multishot recv → spawn its handler coroutine.

The connection has **two owners**, the handler coroutine and the armed recv, so `refs` starts at 2.
Data CQEs are queued on the connection (`rx`, a small ring of filled buffers) and wake the handler
if it is parked in the pipe's reader. When the handler returns, `conn_close` cancels the recv, returns any unread
buffers, closes the fd and drops its ref. The recv's ref drops when its terminal CQE arrives
(`-ECANCELED`, or the peer's FIN). At zero refs the `conn_t` goes back to the pool. A connection is
therefore never recycled while a completion for it is still coming.

### The two awaits

`ioxd__await_item` hands the next queued buffer over whole - the pipe's reader owns it from then
until it returns it to the ring - and parks if the queue is empty. `await_send` stages a SEND SQE
(the loop around it finishes short sends; kernel TLS refuses `MSG_WAITALL`) and parks until its CQE; the pipe's
writer is the only caller. Nothing above the pipe touches either. The TLS prologue adds three more
that nothing else uses: `ioxd__recv_pause` and `ioxd__recv_resume` stop and restart the multishot
recv, `ioxd__recv_exact` reads an exact count straight from the socket, and `ioxd__setsockopt`
programs it - all over the ring, all suspending like any await.

### Stopping

The stop flag is read once. `begin_drain` cancels each armed accept by name (they must stop even on
a kernel without `CANCEL_ANY`), marks every listener stalled so nothing re-arms one, stages a single
`ASYNC_CANCEL` with `IORING_ASYNC_CANCEL_ANY` that takes every recv and every send a coroutine is
parked on, and ends by hand the recvs parked on `-ENOBUFS`, which hold no operation to cancel. If
the kernel refuses the blanket cancel, the worker falls back to cancelling connections one at a
time. Then the loop keeps running: the parked awaits fail with `-ECANCELED`, the handlers unwind,
`conn_close` returns the stacks and the fds, and the loop leaves once `live == 0` or a two-second
grace period is up. A connection accepted in the window before the cancel landed is closed unserved.
If any are still open at the deadline, the ring goes but the buffer slab stays mapped: the kernel
may still hold a buffer for a recv, and leaking the slab at exit beats unmapping it underneath.

### Shared nothing

Nothing crosses workers: each has its own ring, sockets, buffers, pools and coroutines. The
kernel balances connections over the listeners (`SO_REUSEPORT`), and worker *i* is pinned to the
*i*-th CPU the process is allowed on, so it is correct under a container cpuset.

---

## 4. Pipes: the reader and the writer

The connection's bytes reach the HTTP engine, or a handler of your own under `ioxd_run_pipes`,
through a pipe (`io/pipe.h`; `ioxd_pipe` in the public header): a reader over the buffers the
kernel filled and a writer over a slab. Every call that has to wait suspends the coroutine and
the loop resumes it on the completion, so one piece of code drives any connection the I/O plane
runs - the shape of .NET's PipeReader and PipeWriter, with coroutines in place of tasks.

**The reader** hands out received bytes as one contiguous span at a time. The kernel delivers
them as provided buffers, 2 KB each, and the reader keeps them as such: when everything live
lies in one buffer - a whole request head in one receive, the common case - `read` returns that
buffer's bytes in place, no copy. Only when the bytes span buffers, or when the consumer keeps
bytes it wants contiguous, are they gathered into the consumer's buffer (`IOXD_PIPE_GATHER`,
16 KB). Six verbs drive it besides `read`: `examine` (looked at n bytes without consuming them, so
the next `read` waits for more instead of returning the same incomplete head), `drop`
(consume), `keep` (consume, but the bytes stay where they are, contiguous with the run and valid
until `release`), `release` (forget every kept byte; the live ones stay) and `copy` (the plain read
into a buffer of your own), with `run_begin` and `run` below them. A run is a stretch of
kept bytes: the head is one and a whole-read body another; `run_begin` freezes the head's so
the body's can move without it.

Kept bytes are what the request model is made of, so `keep` never invalidates a pointer it
handed out. Bytes kept in place stay in the kernel buffer they arrived in, and `run_begin` **pins**
that buffer: it is not returned to the ring when the live bytes move on, but held until `release`.
Only one buffer can be pinned, so a run that would need a second is copied into the gathering
buffer instead, and `ioxd_pipereader_run` says where it ended up. At most two kernel buffers are
held at a time, the pinned one and the live one, so a slow handler pins one 2 KB buffer per request
in flight; the starvation log says when `recv_buffers` should grow. The counts are clamped rather than
trusted: `drop` consumes at most what is live, `keep` refuses an n past it (and refuses, as
`IOXD_PIPE_FULL`, kept plus live bytes that would outgrow the gathering buffer).

**The writer** is the slab: `reserve` n bytes to format into directly and `advance` by what was
written - never past the room that was there, whatever was claimed - `write` to copy in (data
larger than the slab goes straight from the caller's memory), `flush` to send (a suspension),
`send` for both. In front of the slab is a lead and behind it a slack, so a frame's front and back
go out in the same send as the data between them: the HTTP reply puts its head and a chunk's size
line in the lead and the chunk's CRLF in the slack, and sends through the same await; a raw pipe
writes straight and never touches either. Whatever a handler leaves in the slab is flushed when
the pipe closes, so a raw handler that just wrote and returned still has its bytes sent.

## 5. HTTP

`ioxd__serve` in `lib/http/engine.c` is the coroutine every connection runs - behind the TLS
prologue when the listener has a certificate store. Per request:

1. **Parse** with picohttpparser, straight into `req.headers` (the layouts match, so there is no
   copy). `-2` means "incomplete": read more and parse again, so a request split at any byte works.
   `-1` is a 400. A head that outgrows the reader's 16 KB is a 431. The parsed head is `keep`t, so
   every slice in the request points at bytes that stay put, and `run_begin` closes it off so the
   body's kept bytes can be a run of their own.
2. **Check the framing** before a handler ever sees the request, because a proxy in front that
   reads it differently is how one request smuggles another (RFC 9112 6.1). A `Content-Length` is
   decimal digits and nothing else; repeated, its values must agree; both a `Content-Length` and a
   `Transfer-Encoding` is a 400. A `Transfer-Encoding` must end in `chunked` and name no other
   coding - another coding is a 501, a `chunked` that is not last (across every line, since the
   lines join into one list) or an empty list is a 400. A folded continuation line (obs-fold)
   reaches the engine with no name and is refused rather than interpreted. HTTP/1.1 needs exactly
   one `Host`, HTTP/1.0 at most one. Every `Connection` line is read and its tokens counted, with
   `close` winning over `keep-alive`. An `Expect` other than `100-continue` is a 417. An
   absolute-form target loses its scheme and authority (RFC 9112 3.2.2) before the path and query
   are split off. A query with more parameters than fit is a 400, one whose decoded bytes outgrow
   the arena a 414 - never a request acted on in part. Anything refused is answered and the
   connection closes.
3. **Body, on demand**: the pass that lower-cases the header names is the one that picks out
   `Content-Length`, `Transfer-Encoding`, `Host`, `Connection` and `Expect` (a switch on the name
   length rejects nearly every header before a byte is compared), but the body stays on the wire.
   `ioxd_body_all` keeps it whole in the reader - in place after the head when it all arrived in
   one receive, gathered otherwise, a chunked one slid together chunk by chunk; a
   `Content-Length` past the reader's buffer is a 413; `ioxd_body_read_until` streams it, any
   size, filling the caller's buffer; `ioxd_body_read_next_chunk` hands over one chunk exactly as the
   sender framed it. All three chunked paths share one small parser (a size-line reader, a data
   mover, the CRLF after each chunk) that survives a split at any byte. A size line is held to the
   grammar - hex digits, then the CRLF, or blanks and a `;` with an extension after it - and
   trailers are dropped unread and bounded at `IOXD_TRAILER_MAX`, so a peer cannot hold the
   connection open with an endless one; anything else is a 400. With `Expect: 100-continue` the
   first of these reads sends `100 Continue` ahead of anything the handler buffered. Whatever a
   handler leaves unread is drained after it returns, up to `IOXD_DRAIN_MAX`, past which the reply
   says close; a body an expecting client was never asked for is not waited for at all - the reply
   goes out and the connection closes.
4. **Keep-alive**: HTTP/1.1 unless `Connection: close`; HTTP/1.0 only with `Connection: keep-alive`.
5. **Dispatch** a context to the middleware chain and the route (section 6). The context holds
   the request and the response; the response holds the reply being shaped (status, content
   type, headers); the bytes go into the pipe's writer, an 8 KB slab with a lead in front of it
   for the head and a chunk's size line, and slack behind it for a chunk's CRLF.
6. **Write**: the handler calls `ioxd_write` / `ioxd_printf`, or reserves slab bytes with
   `ioxd_reserve` and says how many it used with `ioxd_advance`. If the slab fills, the framework
   sends what it has — head first, framed chunked on HTTP/1.1 or until close on HTTP/1.0 (or with
   a length the handler declared) — and the handler suspends on that send.
7. **Finish**: after the chain returns, whatever is buffered goes out. The usual case is that
   everything fit: the head (with `Content-Length`, serialized by `memcpy` of precomposed pieces
   plus a small integer writer, no `snprintf`) is copied into the lead right before the body
   and the reply is one send. Every header name goes out lower-cased, the engine's and the
   handler's alike; HTTP/1.1 treats names case-insensitively and HTTP/2 requires lowercase, so
   one spelling is the only one there is. A streamed reply gets its terminating chunk. Then loop;
   `release` gives this request's bytes back and leftover bytes of a pipelined next one stay.

What goes on the wire follows the request and the status, not only what the handler wrote. A reply
to HEAD, and a 1xx, 204 or 304, carries no body however much was written (RFC 9112 6.3) - HEAD
keeps the `Content-Length` its GET would have had, while a 1xx and a 204 carry no framing header at
all. A status outside 100-999 goes out as 500. A declared `Content-Length` is held to: a reply that
was buffered whole is corrected to what was actually written, a stream is cut at the declared byte
and the reply fails there, and one that falls short closes the connection so the client can see it
was cut. Whether the reply says `connection: close` because of an undrainable body is settled with
the head, before any of it is sent.

Because the head is built at the first send, middleware can shape headers and status until then,
and `head_sent` tells a handler when that moment has passed. `ioxd_header` copies the name and the
value into the response's own arena as the line they will become, so temporaries are fine; it
lower-cases the name and refuses one that is not an HTTP token, a value with a control byte (no
response splitting), and the three headers the engine owns - `content-length`,
`transfer-encoding`, `connection` - while `content-type` through it is taken as `ioxd_content_type`.

Everything in a request is a slice (pointer + length) into the bytes the reader kept, valid only
during the handler. Three key/value arrays hang off it, read directly: `headers` (names lower-cased
once at parse time, so a plain compare works), `params` (the query, split and percent-decoded into
a per-request arena only when a value needs it, otherwise a zero-copy view), and `route_params`
(the `:name` captures the router filled in, in pattern order). There are three arenas and no more:
that query arena on the serve coroutine's stack, `req.route_arena` (`IOXD_ROUTE_ARENA`) where the
router decodes captures and writes a 405's `allow` value, and `res.head` (`IOXD_RESP_HEAD_CAP`)
where the reply's added header lines and a copied content type live.

---

## 6. Router and middleware

Endpoints are registered in groups before the workers start: a group is a path prefix plus
middleware, groups nest, and the root (`NULL`) is the group with no prefix whose middleware
`ioxd_use` adds. `ioxd_run` resolves the whole table once, and after that it is read-only, so
every worker reads it without a lock.

The resolution turns each endpoint's full path (the prefixes of its groups, outermost first, then
its own path) into a segment tree. The parts are joined with a `/` between them only when neither
side brought one, so a group `/api` and a path `users` make `/api/users` and a repeated slash
counts once. The tree is a node per static segment, plus at most one capture child per
node for a `:name` segment, with the endpoints at a node kept one per method. A request is one
walk down the tree along its path segments. The static child is tried before the capture, so a
static segment wins at any depth, and the walk backs up to the capture when the static branch
comes to nothing - including when it reaches the end without the request's method, so a static
path with only a GET lets a POST fall through to a capture route that has one. A node with a GET
and no HEAD of its own answers HEAD with that GET (RFC 9110 9.3.2): the handler still sees `HEAD`
as the method, and the engine drops the body it writes. Captured segments land in
`req->route_params`, named from the endpoint that matched and percent-decoded into the request's
arena when they need it (a `/users/a%2Fb` captures `a/b`), left raw when they do not fit.

A path the tree knows without the method is a 405 whose `allow` header is the union of the methods
reachable at that path, not one route's: a path can end more than one route - `/users/new` also
ends the `/users/:id` of a capture route - so the walk remembers every end-of-path node it reached
and the header lists each of their methods once, in registration order, with `HEAD` written after
a GET that has no HEAD of its own since that is what would answer it. A path the tree does not know
goes to the fallback (`ioxd_default`, a plain 404 unless replaced). Nothing is scanned and nothing
is compiled per request; the tree is the map.

Middleware is an onion: each layer receives the context and a `next`; it does work, calls
`ioxd_next_run` to continue, and can do more on the way back out, or it replies and returns to
short-circuit the request. Each endpoint's chain is flattened at resolution - the root's
middleware, then each group's from outermost to innermost, then the endpoint's own - into one
array, so dispatch is a call through it with no walking of groups. Both fallbacks run behind the
root's middleware only, so a 405's `allow` header names methods a group's own middleware would
otherwise have gated.

The `IOXD_` macros are the same registrations as a script: `IOXD_GROUP(prefix, middleware...)`
opens a group for the block that follows (a run-once `for`, the group popped when it ends),
`IOXD_GET(path, handler, middleware...)` and its siblings register into the open group, and
`IOXD_USE` adds middleware to it. The middleware lists travel in small structs ended by a null,
so every argument is type-checked and a wrong signature is a compile error.

## 7. One keep-alive request, end to end

1. Bytes arrive. The kernel copies them into a provided buffer and posts a `RECV` CQE.
2. `enter` returns; dispatch queues the slice on the connection and resumes its coroutine.
3. The reader hands `ioxd__serve` the slice in place - no copy; the head is `keep`t there, so the
   buffer stays pinned to the request until it is done.
4. picohttpparser parses; the framing is checked; the route runs; the head is serialized into the
   slab's lead, right in front of the body the handler wrote.
5. `await_send` stages a SEND SQE and yields back to the loop.
6. The loop finishes the batch and calls `enter` once: the SEND is submitted and the loop waits.
7. The SEND CQE (`OP` tag) resumes the coroutine; `release` gives the request's bytes back, the
   buffer goes to the ring, and the next `read` parks.

Two switches in, two out — tens of nanoseconds. The cost of a request is the kernel's, not ours.

---

## 8. JSON, written as you go

`lib/json/json.c` is a forward-only writer, the shape of .NET's `Utf8JsonWriter`: `ioxd_json_object`,
`ioxd_json_key`, `ioxd_json_int`, `ioxd_json_string`, `ioxd_json_end` and so on, each putting its
bytes straight into a sink - the reply through `ioxd_reserve`/`ioxd_advance`, a raw pipe, or a
memory buffer - with no tree and no allocation. Strings are escaped as they are copied, a safe run
at a time; integers go through a digit loop; a double takes the shortest of 15, 16 or 17
significant digits that `strtod` reads back as the same value, and a float the shortest of 6 to 9
that `strtof` reads back as the same float, so `0.1f` is written `0.1` and not the wider double it
would be promoted to. Both are formatted under a private "C" locale - made once for the process,
worn by the thread for the `snprintf` and the read-back and handed straight back - since a locale
like `fa_IR` spells the decimal point in several bytes and mending one afterwards is not enough.

Nesting and the commas it owes are two bits per level - one saying the level has a value already,
one saying it closes with `}` rather than `]` - so a document deeper than `IOXD_JSON_DEPTH` fails
cleanly, and so does one whose calls do not make a document: a key needs an object under it and no
key already waiting for its value, a value inside an object needs a key in front of it, and an
`end` needs something open and no pending key. The first such call fails, the rest are dropped, so
checking the last one is enough; `ioxd_json_done` is the check after it - nothing failed and
nothing is left open. A reply larger than the slab streams out chunked
while the writer keeps going, which is the whole point of writing as you go.

A struct can be described once and serialized with one call: `IOXD_JSON_STRUCT(user, USER_FIELDS)`
expands a field list twice, into the struct's members and into a `user_to_json` function, so the
two cannot drift. A line is the field's kind - `VALUE`, `OBJECT`, `OPTIONAL`, `ARRAY`, `OBJECTS` - its
type and its name; scalars pick their writer by C type through `_Generic`, nested structs call
their own function, arrays loop over a count field. It is text substitution all the way down: the
generated function is the code one would write by hand, with no table and nothing at runtime.

## Tunables

The first six are set at run time, per worker, with `ioxd_configure(&(ioxd_config){ ... })`
before `ioxd_run` (`ioxd/config.h`; a zero field keeps its default, a bad value is refused with a
line on stderr); the build-time name is the default. The rest are build-time only.

| `ioxd_config` field | default (`-D` name) | what |
|---|---|---|
| `ring_entries` | 4096 (`RING_ENTRIES`) | SQ depth (CQ is twice that); a power of two, at most 32768 |
| `recv_buffers` × `recv_buffer_size` | 4096 × 2 KB (`BUF_COUNT`, `BUF_SIZE`) | provided recv buffers per worker: a power of two at most 32768, and 64 B to 1 MB each |
| `stack_size` | 128 KB (`STACK_SIZE`) | per coroutine, above a 64 KB guard (`CORO_GUARD`); at least 64 KB |
| `idle_stacks`, `idle_connections` | 512, 1024 (`CORO_POOL_MAX`, `CONN_POOL_MAX`) | idle stacks / conns kept warm per worker (not connection limits) |

| build-time only | default | what |
|---|---|---|
| `RX_QUEUE` | 64 | slices a connection may hold undelivered |
| `FIXED_FILES` | 16384 | registered file slots per worker, and so its connection ceiling; 0 disables the table |
| `IOXD_PIPE_GATHER` | 16 KB | the reader's gathering buffer: a head must fit here (else 431) and so must a body read whole (else 413) |
| `IOXD_PIPE_LEAD` / `IOXD_PIPE_CAP` / `IOXD_PIPE_SLACK` | 512 / 8 KB / 8 | the writer's slab and the room in front of and behind it |

The library's own limits - `IOXD_MAX_HEADERS` and the rest of the `IOXD_MAX_*` in `ioxd/http.h` -
are neither: they lay out the context, so `ioxd_run` checks that the application and the library
agree and refuses to start otherwise.

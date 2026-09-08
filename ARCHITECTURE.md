# How libioxd works

libioxd is an HTTP/1.1 server library in C. It runs one worker per CPU core, each worker owns
its own io_uring, and every connection is served by a small coroutine that suspends while the
kernel does the I/O. There are no locks and nothing is shared between workers.

```
process
 ├─ worker 0 (thread pinned to CPU 0)          ├─ worker 1 (CPU 1) ...
 │   ├─ io_uring            one ring, one syscall per batch
 │   ├─ SO_REUSEPORT listener   the kernel spreads connections across workers
 │   ├─ provided buffer ring    where the kernel puts received bytes
 │   ├─ coroutine per connection   runs serve(): parse → handler → reply
 │   └─ pools: conn objects, coroutine stacks
```

`include/ioxd.h` is the whole public API, an umbrella over `include/ioxd/`: `slice.h`, `http.h`,
`router.h`, `json.h` and `pipe.h`, one per concern, each usable on its own. Under `lib/` there are two planes, one concern per file:
`io/` is the I/O plane - `uring.c` (the ring), `coro.c` + `switch_x86_64.S` (coroutines),
`bufring.c` (the buffer ring), `conn.c` (a connection and its awaits), `proactor.c` (the worker
loop), with `proactor.h` as the interface the other plane uses - and `http/` is the HTTP plane -
`engine.c` (parse, body, reply, the serve loop), `router.c` (routes and middleware), `api.c`
(handler helpers), `run.c` (`ioxd_run`). Each plane has an `internal.h` for what its files share.

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
(once a second at most, with counts, so a starved ring is visible and `BUF_COUNT` can be raised) and the connection is
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

`coro_create` maps a 64 KB stack with an unmapped **guard page** below it (a stack overflow faults
instead of corrupting a neighbour). The `coro_t` descriptor sits at the top of that same block.
Then it forges a first frame: six zeros and a return address pointing at `coro_entry`. The first
`swap_ctx` into it pops the zeros and "returns" into `coro_entry`, which calls the coroutine's
function. When the function returns, `coro_entry` marks it done and yields for the last time.

### The rules

- Only the **loop** resumes coroutines. A coroutine never resumes another; it *spawns* one, which
  goes on a ready list the loop drains. Two thread-locals track the current coroutine and the
  loop's saved stack pointer, and that is the whole scheduler.
- A finished coroutine's stack goes into a per-worker **pool** (guard page still armed) and is
  reused by the next connection, so churn pays no `mmap`/`munmap`.

---

## 3. The proactor (the worker loop)

Each worker thread runs `proactor_run`: pin to a CPU, create the ring and buffer ring, open the
listener, arm the multishot accept, then loop:

```
loop:
  run_ready        start coroutines spawned since last time
  rearm_starved    re-arm recvs that hit -ENOBUFS, if buffers came back
  publish          buffer returns staged during the last batch
  enter            submit everything staged, wait for >= 1 completion   (the one syscall)
  dispatch         handle every CQE in the batch; handlers resume inline
  advance          publish the CQ head once
```

Handlers run *inside* dispatch, on their own stacks. A resumed handler that reaches `await_send`
stages a SEND SQE and parks; the SQE rides the next `enter` together with the rest of the batch.

### Routing a completion

Every SQE carries a 64-bit `user_data`. libioxd stores a pointer in it with a 3-bit **tag** in the
low bits (everything it points at is 8-byte aligned):

| tag | points at | meaning |
|---|---|---|
| `OP` | an `op_t` on the awaiting coroutine's stack | a one-shot op (send) finished: resume that coroutine |
| `RECV` | the connection | multishot recv delivered data, or ended |
| `ACCEPT` | the worker | a new connection |
| `IGNORE` | – | a cancel acknowledgement |

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
(`MSG_WAITALL`, so the kernel finishes short sends itself) and parks until its CQE; the pipe's
writer is the only caller. Nothing above the pipe touches either.

### Shared nothing

Nothing crosses workers: each has its own ring, listener, buffers, pools and coroutines. The
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
bytes it wants contiguous, are they gathered into the consumer's buffer (the 16 KB request
buffer, for HTTP). Four verbs drive it: `examine` (looked at n bytes without consuming them, so
the next `read` waits for more instead of returning the same incomplete head), `drop`
(consume), `keep` (consume, but the bytes stay where they are, contiguous with the run and valid
until `release`) and `copy` (the plain read into a buffer of your own). A run is a stretch of
kept bytes: the head is one and a whole-read body another; `run_begin` freezes the head's so
the body's can move without it. At most two kernel buffers are held at a time, one with frozen
kept bytes and one with the live bytes, so a slow handler pins one 2 KB buffer per request in
flight; the starvation log says when `BUF_COUNT` should grow.

**The writer** is the slab: `reserve` n bytes to format into directly and `advance` by what was
written, `write` to copy in, `flush` to send (a suspension), `send` for both. The HTTP reply
keeps its own head-building and chunk framing on its slab and sends through the same await; a
raw pipe writes straight.

## 5. HTTP

`serve()` in `http.c` is the coroutine every connection runs. Per request:

1. **Parse** with picohttpparser, straight into `req.headers` (the layouts match, so there is no
   copy). `-2` means "incomplete": read more and parse again, so a request split at any byte works.
2. **Body, on demand**: one pass over the headers picks out `Content-Length`,
   `Transfer-Encoding` and `Connection` (a length test rejects almost every header before a byte
   is compared), but the body stays on the wire. `ioxd_body_all` keeps it whole in the reader - in place
   after the head when it all arrived in one receive, gathered otherwise, a chunked one slid
   together chunk by chunk; `ioxd_body_read_until` streams it, any
   size, filling the caller's buffer; `ioxd_body_read_next_chunk` hands over one chunk exactly as the
   sender framed it. All three chunked paths share one small parser (a raw stage after the head, a
   size-line reader, a data mover) that survives a split at any byte. Whatever a handler leaves
   unread is drained after it returns, up to a limit, past which the reply says close.
3. **Keep-alive**: HTTP/1.1 unless `Connection: close`; HTTP/1.0 only with `Connection: keep-alive`.
4. **Dispatch** a context to the middleware chain and the route (section 5). The context holds
   the request and the response; the response holds the reply being shaped (status, content
   type, headers); the bytes go into the pipe's writer, an 8 KB slab with a lead in front of it
   for the head and a chunk's size line, and slack behind it for a chunk's CRLF.
5. **Write**: the handler calls `ioxd_write` / `ioxd_printf`; bytes land in the buffer. If it
   fills, the framework sends what it has — head first, framed chunked on HTTP/1.1 or until close
   on HTTP/1.0 (or with a length the handler declared) — and the handler suspends on that send.
6. **Finish**: after the chain returns, whatever is buffered goes out. The usual case is that
   everything fit: the head (with `Content-Length`, serialized by `memcpy` of precomposed pieces
   plus a small integer writer, no `snprintf`) is copied into the reserve right before the body
   and the reply is one send. Every header name goes out lower-cased, the engine's and the
   handler's alike; HTTP/1.1 treats names case-insensitively and HTTP/2 requires lowercase, so
   one spelling is the only one there is. A streamed reply gets its terminating chunk. Then loop; leftover
   bytes of a pipelined next request are carried over.

Because the head is built at the first send, middleware can shape headers and status until then,
and `head_sent` tells a handler when that moment has passed.

Everything in a request is a slice (pointer + length) into the read buffer, valid only during
the handler. Three key/value arrays hang off it, read directly: `headers` (names lower-cased once
at parse time, so a plain compare works), `params` (the query, split and percent-decoded into a
small per-request arena only when a value needs it, otherwise a zero-copy view), and `route_params` (the
`:name` captures the router filled in, in pattern order). A `scratch` arena is there for building
a body (`ioxd_textf`).

---

## 6. Router and middleware

Endpoints are registered in groups before the workers start: a group is a path prefix plus
middleware, groups nest, and the root (`NULL`) is the group with no prefix whose middleware
`ioxd_use` adds. `ioxd_run` resolves the whole table once, and after that it is read-only, so
every worker reads it without a lock.

The resolution turns each endpoint's full path (the prefixes of its groups, outermost first, then
its own path) into a segment tree: a node per static segment, plus at most one capture child per
node for a `:name` segment, with the endpoints at a node kept one per method. A request is one
walk down the tree along its path segments. The static child is tried before the capture, so a
static segment wins at any depth, and the walk backs up to the capture when the static branch
comes to nothing - including when it reaches the end without the request's method, so a static
path with only a GET lets a POST fall through to a capture route that has one. Captured segments
land in `req->route_params`, named from the endpoint that matched. A path the tree knows without
the method is a 405 with an `allow` header; a path it does not know goes to the fallback
(`ioxd_default`, a plain 404 unless replaced). Nothing is scanned and nothing is compiled per
request; the tree is the map.

Middleware is an onion: each layer receives the context and a `next`; it does work, calls
`ioxd_next_run` to continue, and can do more on the way back out, or it replies and returns to
short-circuit the request. Each endpoint's chain is flattened at resolution - the root's
middleware, then each group's from outermost to innermost, then the endpoint's own - into one
array, so dispatch is a call through it with no walking of groups. The fallbacks run behind the
root's middleware only.

The `IOXD_` macros are the same registrations as a script: `IOXD_GROUP(prefix, middleware...)`
opens a group for the block that follows (a run-once `for`, the group popped when it ends),
`IOXD_GET(path, handler, middleware...)` and its siblings register into the open group, and
`IOXD_USE` adds middleware to it. The middleware lists travel in small structs ended by a null,
so every argument is type-checked and a wrong signature is a compile error.

## 7. One keep-alive request, end to end

1. Bytes arrive. The kernel copies them into a provided buffer and posts a `RECV` CQE.
2. `enter` returns; dispatch queues the slice on the connection and resumes its coroutine.
3. The reader hands `serve` the slice in place - no copy; the buffer stays the request's until it is done.
4. picohttpparser parses; the route runs; the response is serialized into the head buffer.
5. `await_send` stages a SEND SQE and yields back to the loop.
6. The loop finishes the batch and calls `enter` once: the SEND is submitted and the loop waits.
7. The SEND CQE (`OP` tag) resumes the coroutine; `serve` releases the request's bytes, the buffer
   goes back to the ring, and the next `read` parks.

Two switches in, two out — tens of nanoseconds. The cost of a request is the kernel's, not ours.

---

## 8. JSON, written as you go

`lib/json/json.c` is a forward-only writer, the shape of .NET's `Utf8JsonWriter`: `ioxd_json_object`,
`ioxd_json_key`, `ioxd_json_int`, `ioxd_json_string`, `ioxd_json_end` and so on, each putting its
bytes straight into a sink - the reply through `ioxd_reserve`/`ioxd_advance`, a raw pipe, or a
memory buffer - with no tree and no allocation. Strings are escaped as they are copied, a safe run
at a time; integers go through a digit loop; a double takes the shortest of 15, 16 or 17
significant digits that reads back as the same value, with the decimal point forced to '.'
whatever the locale says. Nesting and the commas it owes are two bits per level, so a document
deeper than `IOXD_JSON_DEPTH` fails cleanly. A reply larger than the slab streams out chunked
while the writer keeps going, which is the whole point of writing as you go.

A struct can be described once and serialized with one call: `IOXD_JSON_STRUCT(user, USER_FIELDS)`
expands a field list twice, into the struct's members and into a `user_to_json` function, so the
two cannot drift. A line is the field's kind - `VALUE`, `OBJECT`, `OPTIONAL`, `ARRAY`, `OBJECTS` - its
type and its name; scalars pick their writer by C type through `_Generic`, nested structs call
their own function, arrays loop over a count field. It is text substitution all the way down: the
generated function is the code one would write by hand, with no table and nothing at runtime.

## Tunables

| name | default | what |
|---|---|---|
| `RING_ENTRIES` | 4096 | SQ depth (CQ is twice that) |
| `BUF_COUNT` × `BUF_SIZE` | 4096 × 2 KB | provided recv buffers per worker |
| `RX_QUEUE` | 64 | slices a connection may hold undelivered |
| `STACK_SIZE` | 64 KB | per coroutine, plus a guard page |
| `CORO_POOL_MAX`, `CONN_POOL_MAX` | 512, 1024 | idle stacks / conns kept warm per worker (not connection limits) |
| `IOXD_REQ_CAP` | 16 KB | a request must fit here, else 413/431 |

Override any of them with `-D` at build time.

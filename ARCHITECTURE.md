# How libioma works

libioma is an HTTP/1.1 server library in C. It runs one worker per CPU core, each worker owns
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

Everything below is in `src/`, one concern per file: `uring.c` (the ring), `coro.c` +
`switch_x86_64.S` (coroutines), `bufring.c` (the buffer ring), `conn.c` (a connection and its
awaits), `proactor.c` (the worker loop), `http.c` (the HTTP engine), `api.c` (handler helpers),
`router.c` (routes and middleware), `run.c` (`ioma_run`). `internal.h` is what they share.

---

## 1. io_uring, without liburing

io_uring is two ring buffers shared with the kernel. You write **submission entries** (SQEs,
"do this") into one ring, the kernel writes **completion entries** (CQEs, "this finished, result
N") into the other. One syscall, `io_uring_enter`, both submits and waits.

libioma talks to it directly (`uring.c`), the way ioxide does, with three setup flags:

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
buffers costs one atomic store. If the ring runs dry a recv ends with `-ENOBUFS`; the connection is
parked and re-armed as soon as any buffer comes back.

---

## 2. Coroutines

A **stackful coroutine** is a function running on its own stack that can pause in the middle
(`coro_yield`) and be continued later (`coro_resume`), exactly where it left off, with all its local
variables intact. That is what lets a handler be written as plain sequential code:

```c
n = await_recv(c, buf, sizeof buf);   /* pauses here until data arrives */
await_send(c, reply, len);            /* pauses here until the send completes */
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

Every SQE carries a 64-bit `user_data`. libioma stores a pointer in it with a 3-bit **tag** in the
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
Data CQEs are queued on the connection (`rx`, a small ring of slices) and wake the handler if it is
parked in `await_recv`. When the handler returns, `conn_close` cancels the recv, returns any unread
buffers, closes the fd and drops its ref. The recv's ref drops when its terminal CQE arrives
(`-ECANCELED`, or the peer's FIN). At zero refs the `conn_t` goes back to the pool. A connection is
therefore never recycled while a completion for it is still coming.

### The two awaits

`await_recv` copies the next queued slice into the caller's buffer, returns the provided buffer to
the ring when the slice is fully consumed, and parks if the queue is empty. `await_send` stages a
SEND SQE (`MSG_WAITALL`, so the kernel finishes short sends itself) and parks until its CQE.

### Shared nothing

Nothing crosses workers: each has its own ring, listener, buffers, pools and coroutines. The
kernel balances connections over the listeners (`SO_REUSEPORT`), and worker *i* is pinned to the
*i*-th CPU the process is allowed on, so it is correct under a container cpuset.

---

## 4. HTTP

`serve()` in `http.c` is the coroutine every connection runs. Per request:

1. **Parse** with picohttpparser, straight into `req.headers` (the layouts match, so there is no
   copy). `-2` means "incomplete": read more and parse again, so a request split at any byte works.
2. **Body**: one pass over the headers picks out `Content-Length`, `Transfer-Encoding` and
   `Connection` (a length test rejects almost every header before a byte is compared). A chunked
   body is decoded in place by `phr_decode_chunked`, which also survives fragmentation.
3. **Keep-alive**: HTTP/1.1 unless `Connection: close`; HTTP/1.0 only with `Connection: keep-alive`.
4. **Dispatch** a context to the middleware chain and the route (section 5). The context holds
   the request, the reply being shaped (status, content type, headers) and a body sink: an 8 KB
   buffer with room reserved in front of it for the head.
5. **Write**: the handler calls `ioma_write` / `ioma_printf`; bytes land in the buffer. If it
   fills, the framework sends what it has — head first, framed chunked on HTTP/1.1 or until close
   on HTTP/1.0 (or with a length the handler declared) — and the handler suspends on that send.
6. **Finish**: after the chain returns, whatever is buffered goes out. The usual case is that
   everything fit: the head (with `Content-Length`, serialized by `memcpy` of precomposed pieces
   plus a small integer writer, no `snprintf`) is copied into the reserve right before the body
   and the reply is one send. A streamed reply gets its terminating chunk. Then loop; leftover
   bytes of a pipelined next request are carried over.

Because the head is built at the first send, middleware can shape headers and status until then,
and `head_sent` tells a handler when that moment has passed.

Everything in a request is a slice (pointer + length) into the read buffer, valid only during
the handler. Three key/value arrays hang off it, read directly: `headers` (names lower-cased once
at parse time, so a plain compare works), `params` (the query, split and percent-decoded into a
small per-request arena only when a value needs it, otherwise a zero-copy view), and `route` (the
`:name` captures the router filled in, in pattern order). A `scratch` arena is there for building
a body (`ioma_textf`).

---

## 5. Router and middleware

`ioma_route(method, path, fn)` fills a table that is read-only once the workers start, so all
workers share it with no lock. A path is exact (lengths first, `memcmp` only on a hit) or a
pattern with `:name` segments (`/users/:id`), matched segment by segment with the captures written
to `req->route`. Exact routes are tried first, so a static path beats a pattern. `ioma_default`
replaces the built-in 404.

Middleware (`ioma_use`) is an onion: each layer receives the request and a `next`; it does work,
calls `ioma_next_run` to run the rest of the chain and the endpoint, then may inspect or replace the
response on the way out. Not calling `next` short-circuits (auth, cache). With no middleware
registered the dispatch is a direct function call.

---

## 6. One keep-alive request, end to end

1. Bytes arrive. The kernel copies them into a provided buffer and posts a `RECV` CQE.
2. `enter` returns; dispatch queues the slice on the connection and resumes its coroutine.
3. `await_recv` copies the slice into `serve`'s buffer and returns the buffer to the ring.
4. picohttpparser parses; the route runs; the response is serialized into the head buffer.
5. `await_send` stages a SEND SQE and yields back to the loop.
6. The loop finishes the batch and calls `enter` once: the SEND is submitted and the loop waits.
7. The SEND CQE (`OP` tag) resumes the coroutine; `serve` loops to `await_recv` and parks.

Two switches in, two out — tens of nanoseconds. The cost of a request is the kernel's, not ours.

---

## Tunables

| name | default | what |
|---|---|---|
| `RING_ENTRIES` | 4096 | SQ depth (CQ is twice that) |
| `BUF_COUNT` × `BUF_SIZE` | 4096 × 2 KB | provided recv buffers per worker |
| `RX_QUEUE` | 64 | slices a connection may hold undelivered |
| `STACK_SIZE` | 64 KB | per coroutine, plus a guard page |
| `CORO_POOL_MAX`, `CONN_POOL_MAX` | 512, 1024 | idle stacks / conns kept warm per worker (not connection limits) |
| `IOMA_REQ_CAP` | 16 KB | a request must fit here, else 413/431 |

Override any of them with `-D` at build time.

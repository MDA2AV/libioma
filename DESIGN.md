# ioxide's TCP core, read for a minimal stackful C runtime

> Note: this is the design record for **ioma** (github.com/MDA2AV/ioma). Paths like `ioxide/...` and `ringzero/...` refer to the author's sibling repos (github.com/MDA2AV/ioxide, github.com/MDA2AV/ringzero); ioma itself is the `stackful/`→`ioma/` runtime described in section 3 and shipped in this repository.


Scope: `ioxide/src/ioxide` — `io_uring/Ring.cs`, `Native/*`, `Reactor/*`, `Reactor/Transport/Tcp/*`,
`Connection/Tcp/*`, `Client/*`, `Utils/*` (≈4.8k lines). UDP, QUIC, TLS, the protocol and client
packages are out of scope. The sibling `ringzero/` (C, liburing, callback handler, 784 lines) is read
as the existing C baseline. Paths below are relative to `Socket/`.

Target: the brief's runtime — one io_uring per worker thread, handlers as stackful coroutines,
`await_*` calls instead of state machines — built **as small as possible first**.

---

## 0. TL;DR

- ioxide's TCP path is: one thread = one io_uring (`SINGLE_ISSUER | DEFER_TASKRUN | NO_SQARRAY`) +
  one `SO_REUSEPORT` listener + one fd-indexed connection table + one shared provided-buffer ring.
  The loop is: drain hand-off queues → `io_uring_enter(submit, wait ≥ 1)` → dispatch the CQ batch by a
  kind byte in `user_data` → advance the CQ head once.
- About half of that code exists for two reasons that vanish under stackful coroutines: (a) a C#
  handler cannot keep anything on its stack across an `await` (hence `IValueTaskSource` cores,
  generation tokens, op-slot tables, per-slot timespecs, the write slab), and (b) a handler may resume
  off-thread (hence `Mpsc` queues, the eventfd wake, `SynchronizationContext`, and a thread-id check on
  every public entry point).
- The minimal C v1 is ≈500 lines: a 15-line context switch, an `op_t` on the awaiting coroutine's
  stack as the CQE routing key, one-shot accept/recv/send, a ready list for spawn, the brief's
  handler. No provided buffers, no multishot, no connection table, no generations, no refcounts, no
  pools. It should match `ringzero` on the same box because the syscall pattern is identical.
- Two things the brief gets wrong or leaves implicit: `coro_resume` from *inside* a coroutine clobbers
  the single `loop_sp` (spawn must go through a ready list, or the switch needs caller links), and
  sends in C need `MSG_NOSIGNAL` (.NET ignores SIGPIPE process-wide, so ioxide never sets it).

---

## 1. How ioxide's TCP path works

### 1.1 Ring — `ioxide/src/ioxide/io_uring/Ring.cs`, `Native/Native.IoUring.cs`

- No liburing: three raw syscalls via `syscall()`, all struct layouts and constants hand-declared.
  SQ and CQ rings come from one mmap (assumes `IORING_FEAT_SINGLE_MMAP`, never checks it), the SQE
  array from a second.
- Setup flags at `Ring.cs:42`: `SINGLE_ISSUER | DEFER_TASKRUN | NO_SQARRAY`, with an `EINVAL`
  fallback that drops `NO_SQARRAY` for pre-6.6 kernels.
- `GetSqe` (`Ring.cs:108`): local tail against the kernel head, returns null when full.
- `SubmitAndWait` (`Ring.cs:127`): to-submit = local tail − kernel head (liburing's accounting, so an
  `-EBUSY` enter that consumed nothing is re-counted next time), publish the tail with a release
  store, `enter(GETEVENTS)` only when asked to wait.
- CQ drained as a batch (`Ring.cs:173`): one acquire read of the tail, index each CQE, one release
  write of the head.
- Not used anywhere: SQPOLL, registered files or buffers, a registered ring fd, `CQSIZE`, `SUBMIT_ALL`.

### 1.2 The loop — `Reactor/Loop/Reactor.Loop.SharedRing.cs:47`

```
drain: buffer-return Q, flush Q, recycle Q, remote client ops, posted continuations
re-arm recvs parked on -ENOBUFS; fire QUIC timers
rc = SubmitAndWait(1)                 // -EINTR / -EAGAIN / -EBUSY are tolerated
for each ready CQE: dispatch by kind  // handler continuations run INSIDE this loop
CqAdvance(ready)
```

Because the handler resumes inside dispatch, the SQEs it produces (the response send) ride the next
`enter` together with the rest of the batch. If the SQ fills mid-batch, `GetSqeOrFlush`
(`Reactor/Reactor.cs:181`) submits without waiting and retries, up to 16 times, then throws.

### 1.3 Routing — `Reactor/Reactor.cs:40-53`

`user_data = [63:56] kind | [47:32] connection generation | [31:0] fd` (or, for client ops, an op-slot
index in the low bits). Kinds: accept, recv, send, wake, client, cancel, timer, udp-recv, udp-send.
Dispatch is `connections[fd]` plus a generation compare (`ConnAt`, `Reactor.cs:135`). The generation
bumps on every recycle, so a straggler CQE from a closed-and-reused fd is recognised and dropped, its
buffer returned. Client ops (`Reactor/Reactor.RingHost.cs:166-211`) skip the table: the slot indexes
an `IRingCompletion[]`, and the slot is freed *before* `Complete` runs so the inline continuation can
submit into it.

### 1.4 Accept — `Reactor/Transport/Tcp/Reactor.Tcp.cs:220`

Each reactor opens its own `socket/bind/listen` per port with `SO_REUSEADDR | SO_REUSEPORT`
(`Reactor.Tcp.cs:382`; IPv4, or one `AF_INET6` dual-stack socket). Multishot accept is armed once per
listener and re-armed when `F_MORE` is clear. Per accept CQE: `TCP_NODELAY` by a `setsockopt` syscall,
pop a pooled `TcpConnection` or build one, `Track(fd)`, refcount = 2, arm a multishot recv stamped with
the connection's generation, start the handler fire-and-forget (`Reactor.Tcp.Handler.cs` observes
faults and releases the handler's ref).

### 1.5 Recv

- **Shared mode** (default): one provided-buffer ring per reactor, bgid 1, `RecvSlots` = 4096 ×
  `RecvBufferSize` = 32 KiB — 128 MiB reserved per reactor, every recv consumes a whole buffer
  (`Reactor.Loop.SharedRing.cs:9`). Multishot `IORING_OP_RECV` with `IOSQE_BUFFER_SELECT`
  (`Reactor.Tcp.cs:13`).
- Per CQE (`Reactor.Tcp.cs:101`): `conn.Complete(res, bid, ptr)` (`Connection/Tcp/TcpConnection.Read.cs:126`)
  pushes `{ptr, bid, len, gen}` into the connection's SPSC ring (64 entries; overflow = cancel and
  teardown) and completes the parked `ReadAsync`.
- `-ENOBUFS` (`Reactor.Tcp.cs:108`) ends the multishot; the connection is parked in `_recvStarved` and
  `RearmStarvedRecvs` (`Reactor.Tcp.cs:292`) re-arms it once *any* buffer returns (`_buffersReturned`
  flag, issue #93).
- `res <= 0` (EOF or error): the reactor detaches the connection, `MarkClosed`, drops the recv-side
  ref (`CloseFromRecv`, `Reactor.Tcp.cs:272`). **A peer FIN tears the connection down at once**, so a
  client that half-closes right after its request races the response. Deliberate and documented
  (`ioxide/tests/Ioxide.Tests.Chaos/TcpChaosTests.cs:167`).
- Handler side: `ReadAsync()` (`TcpConnection.Read.cs:37`) returns a `RecvSnapshot` (SPSC tail marker
  + closed flag), `TryGetItem` drains items up to that tail, `ReturnBuffer(item)` gives the bid back
  (direct when on the reactor thread, through `Mpsc<ushort>` otherwise), `ResetRead()` re-arms the
  value-task core before the next read.
- **Incremental mode** (kernel 6.12+, `Reactor/Loop/Reactor.Loop.Incremental.cs`): one small
  `IOU_PBUF_RING_INC` ring per connection, registered at accept (`:34`) and unregistered at close; the
  kernel appends successive recvs into the same buffer (`F_BUF_MORE`); per-bid offset, refcount and
  kernel-done arrays (`Reactor.Tcp.cs:160`, `Incremental.cs:152`); buffer-group ids capped by
  `MaxConnections`, accepts past the cap are shed.

### 1.6 Send — `Reactor/Loop/Reactor.Loop.DispatchCompletions.cs:13`, `Connection/Tcp/TcpConnection.Write*.cs`

- The handler writes into a 16 KiB per-connection slab (`IBufferWriter<byte>`). Overflow either
  reallocs the slab (Grow) or chains pooled slabs flushed by one `SENDMSG` iovec (Segmented).
- `FlushAsync` (`TcpConnection.Write.Flush.cs:21`) hands off to `SubmitFlush`
  (`Reactor/Loop/Reactor.Drainers.cs:164`): `IORING_OP_SEND` with `MSG_WAITALL`, so the kernel retries
  short sends and one flush is one CQE. A genuinely partial CQE resubmits from `WriteHead`. Optional
  `SEND_ZC`: the data CQE carries `F_MORE`, a later `F_NOTIF` CQE releases the slab, and the flush
  completes on the notif.
- `_flushInProgress` rejects writes while the slab is in flight. A send error cancels the multishot
  recv and tears down.
- Files: `ReadFileAsync` (`TcpConnection.FileRead.cs:27`) does `IORING_OP_READ` straight into the slab
  tail, then `AdvanceWrite(n)`, so header and body leave in one send.

### 1.7 Connection lifetime — `Connection/Tcp/TcpConnection.cs:67-117`, `Reactor.Drainers.cs:95`

Two owners, reactor (recv side) and handler; refcount 2 at accept. `DecRef` to zero hands the object
to `Recycle`, always on the reactor thread: `MarkClosed` completes any parked read or flush,
`ASYNC_CANCEL` targets the multishot recv by exact `user_data` using the *pre-bump* generation,
leftover items go back to the buffer ring (or the per-connection ring is unregistered), `close(fd)`,
`Clear()` bumps the generation and resets state, push to the pool (≤ `PoolMax`, else free).

### 1.8 Cross-thread machinery — all of it disappears in C

`Utils/Mpsc.cs` (Vyukov bounded MPMC specialised to one consumer) for buffer returns and flushes,
`ConcurrentQueue` for recycles, remote client ops and posted continuations, an eventfd behind a
multishot `POLL_ADD` as the wake (`Reactor.Drainers.cs:25`), `ReactorSynchronizationContext.cs` so an
`await Task.Run` comes home, `Reactor.Post.cs`, the `HandOff` path in `Reactor.RingHost.cs:149`, and
the `Interlocked`/`Volatile` choreography in `TcpConnection.Read.cs` and `Write.Flush.cs`
(`_armed`, `_pending`, `_closed`, `_flushArmed`). `REACTOR-MODELS.md` prices the thread hop at
30–41% per request and a shared locked ring at ~3×; the C runtime simply never leaves the worker.

### 1.9 Timers — `Reactor/Loop/Reactor.Timer.cs:12-31`, `Reactor.RingHost.cs:98`

One single-shot `IORING_OP_TIMEOUT` every 250 ms drives registered tickers. Per-operation timeouts
(`SubmitTimeout`) need a `__kernel_timespec` that outlives the await, so the reactor keeps a timespec
array parallel to the op-slot table — a direct consequence of "nothing survives on a C# stack".

---

## 2. What a stackful coroutine replaces

The rule: anything the kernel reads while an op is in flight may live on the coroutine's stack,
because that stack is frozen, not unwound, while the coroutine is parked. That one fact removes most
of this table.

| ioxide mechanism | why it exists | C v1 |
|---|---|---|
| `IValueTaskSource` cores, `_armed`/`_pending`/`_closed`, `ResetRead` | park and resume without a stack | `op_t { coro_t *waiter; int res; }` on the stack; `await_io` yields |
| `user_data = kind\|gen\|fd`, `connections[fd]`, `ConnAt` | route a CQE to an object that may be gone or reused | `user_data = &op`; a one-shot op cannot go stale because its coroutine is parked until the CQE |
| generation counter, `ASYNC_CANCEL` at teardown, stale-CQE drops | multishot CQEs outlive a connection | not needed until multishot (v2) |
| refcount with two owners, `Recycle`, `ReleaseHandlerRefOnFault` | reactor and handler both own the connection | the coroutine owns it; free on return |
| write slab, Grow/Segmented, `_flushInProgress`, `BuildIovec` | handler bytes must sit in reactor-owned memory across the await | `await_send(buf, len)` from the caller's buffer, even a stack buffer; `await_sendmsg(iov)` for gather |
| op-slot table plus parallel timespec array | the timespec must outlive the await | `struct __kernel_timespec ts` on the stack (v3) |
| `Mpsc`, eventfd wake, `Post`, sync context, thread-id checks | handlers may resume off-thread | nothing; one thread by construction |
| `RunHandlerAsync` try/catch | a faulting handler must not leak the connection | the coroutine trampoline runs a finish hook |
| `RecvSnapshot`, SPSC ring, `ReturnBuffer` | multishot delivers data when nobody is reading | v2 rx queue; in v1 nothing is received unless a coroutine is parked on it |
| `SubmitAndWait`, `CqReady/CqeAt/CqAdvance`, `GetSqeOrFlush` | ring plumbing | liburing: `submit_and_wait`, `for_each_cqe`, `cq_advance`, `get_sqe` then `submit` on NULL |

Unchanged: the thread-per-core, shared-nothing shape; per-worker `SO_REUSEPORT` listener;
`SINGLE_ISSUER | DEFER_TASKRUN`; wait for ≥ 1 then batch-dispatch; the handler runs inside dispatch;
submit-without-wait when the SQ is full; `TCP_NODELAY` per accepted socket; `MSG_WAITALL` or a
userspace loop for short sends.

---

## 3. v1 — as basic as it gets

Scope: an HTTP/1.1 "ok" server equivalent to `ioxide/Playground/Tcp/Raw` and
`ringzero/src/app/main.c`, so it benchmarks against both. One-shot everything. No buffer rings, no
multishot, no pools, no timers, no graceful shutdown beyond a stop flag.

Files, ≈500 lines in total:

```
switch_x86_64.S   swap_ctx                                             (~20)
coro.h  coro.c    coro_create / resume / yield / current, stack mmap + guard  (~120)
worker.h worker.c ring, loop, ready list, await_accept / recv / send, spawn   (~200)
listener.c        copy of ringzero/src/lib/listener.c minus O_NONBLOCK         (~40)
main.c            N threads × worker_run, acceptor + handler, signal → stop    (~80)
Makefile          gcc -O2 -g -Wall -march=native -pthread ... -luring
```

### 3.1 Coroutine

```c
typedef struct coro {
    void   *sp;              /* resume point                          */
    void   *stack;           /* mmap base; lowest page is PROT_NONE   */
    size_t  size;
    void  (*fn)(void *);
    void   *arg;
    bool    done;
    struct coro *next;       /* ready-list link                       */
} coro_t;

extern void swap_ctx(void **save_sp, void *load_sp);   /* switch_x86_64.S */

static __thread coro_t *current;
static __thread void   *loop_sp;

static void coro_entry(void)
{
    current->fn(current->arg);
    current->done = true;
    coro_yield();                        /* never returns */
    __builtin_unreachable();
}

coro_t *coro_create(void (*fn)(void *), void *arg, size_t stack_bytes);
/* mmap(stack_bytes + page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK), mprotect the
   lowest page PROT_NONE. Forge the first frame: six zeroed callee-saved slots directly below a
   16-byte-aligned slot holding &coro_entry, and a zero above it as a fake return address. The first
   swap_ctx pops the six slots and rets into coro_entry with rsp ≡ 8 mod 16, as a call would. */

void coro_resume(coro_t *c)
{
    current = c;
    swap_ctx(&loop_sp, c->sp);
    current = NULL;
    if (c->done) coro_destroy(c);        /* we are on the loop stack: safe to munmap */
}

void coro_yield(void) { swap_ctx(&current->sp, loop_sp); }
```

`swap_ctx`: push `rbp rbx r12 r13 r14 r15`; `mov %rsp,(%rdi)`; `mov %rsi,%rsp`; pop them in reverse;
`ret`. Add `.section .note.GNU-stack,"",@progbits` and nothing else (see §4.4).

### 3.2 Worker

```c
typedef struct op { coro_t *waiter; int res; unsigned flags; } op_t;

struct worker {
    struct io_uring ring;
    int      listen_fd, id;
    coro_t  *ready_head, *ready_tail;
};
static __thread struct worker *W;

static struct io_uring_sqe *sqe(void)
{
    struct io_uring_sqe *s = io_uring_get_sqe(&W->ring);
    if (__builtin_expect(!s, 0)) { io_uring_submit(&W->ring); s = io_uring_get_sqe(&W->ring); }
    return s;
}

static int await_io(struct io_uring_sqe *s, op_t *op)
{
    op->waiter = current;
    io_uring_sqe_set_data(s, op);
    coro_yield();                        /* the loop resumes us when the CQE lands */
    return op->res;
}

int await_accept(int lfd)
{ op_t op; struct io_uring_sqe *s = sqe(); io_uring_prep_accept(s, lfd, NULL, NULL, 0);    return await_io(s, &op); }

int await_recv(int fd, void *buf, size_t n)
{ op_t op; struct io_uring_sqe *s = sqe(); io_uring_prep_recv(s, fd, buf, n, 0);           return await_io(s, &op); }

int await_send(int fd, const void *buf, size_t n)
{ op_t op; struct io_uring_sqe *s = sqe(); io_uring_prep_send(s, fd, buf, n, MSG_NOSIGNAL); return await_io(s, &op); }

void coro_spawn(void (*fn)(void *), void *arg)
{ ready_push(W, coro_create(fn, arg, STACK_BYTES)); }   /* the LOOP resumes it, never the caller */

void worker_run(struct worker *w)
{
    W = w;
    struct io_uring_params p = { .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN };
    io_uring_queue_init_params(4096, &w->ring, &p);      /* on THIS thread, DEFER_TASKRUN needs it */
    coro_spawn(acceptor, w);

    while (!g_stop) {
        for (coro_t *c; (c = ready_pop(w)); ) coro_resume(c);

        struct __kernel_timespec ts = { .tv_nsec = 100 * 1000 * 1000 };   /* stop-flag check cadence */
        struct io_uring_cqe *cqe;
        io_uring_submit_and_wait_timeout(&w->ring, &cqe, 1, &ts, NULL);   /* -ETIME is fine */

        unsigned head, n = 0;
        io_uring_for_each_cqe(&w->ring, head, cqe) {
            op_t *op = io_uring_cqe_get_data(cqe);
            op->res = cqe->res; op->flags = cqe->flags;
            coro_resume(op->waiter);                     /* runs until its next await */
            n++;
        }
        io_uring_cq_advance(&w->ring, n);
    }
}
```

### 3.3 Acceptor and handler

```c
static void acceptor(void *arg)
{
    struct worker *w = arg;
    for (;;) {
        int fd = await_accept(w->listen_fd);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        coro_spawn(handler, (void *)(intptr_t)fd);      /* a conn_t arrives with v2 */
    }
}

static int send_all(int fd, const void *p, size_t n)
{
    for (size_t off = 0; off < n; ) {
        int r = await_send(fd, (const char *)p + off, n - off);
        if (r <= 0) return -1;
        off += r;
    }
    return 0;
}

static void handler(void *arg)
{
    int  fd = (int)(intptr_t)arg;
    char req[8192];
    for (;;) {
        int n = await_recv(fd, req, sizeof req);        /* Tcp/Raw parity: one response per recv */
        if (n <= 0) break;                              /* 0 = peer FIN, <0 = -errno */
        if (send_all(fd, RESPONSE, sizeof RESPONSE - 1) < 0) break;
    }
    close(fd);
}
```

### 3.4 main

One `listener_open(port)` per worker (`SO_REUSEPORT`), one pthread per worker calling `worker_run`,
`SIGINT`/`SIGTERM` set `g_stop`, join. When measuring, pin each thread with `pthread_setaffinity_np`
to a P-core; on this i9-14900K the P-core threads are normally cpus 0–15 (confirm with
`cat /sys/devices/cpu_core/cpus`).

---

## 4. Pitfalls, verified on this machine

1. **SIGPIPE.** `tcp_sendmsg` raises it on a send to a peer that closed unless `MSG_NOSIGNAL` is set.
   ioxide never sets it because .NET ignores SIGPIPE; ringzero sets it (`ringzero/src/lib/reactor.c:107`).
   Set it on every send, or `signal(SIGPIPE, SIG_IGN)` at startup.
2. **Nested resume.** The brief's acceptor does `coro_create` then `coro_resume` from inside a
   coroutine. With a single `loop_sp`, that `coro_resume` saves the *acceptor's* sp into `loop_sp`,
   and the next `coro_yield` from anywhere jumps into a dead acceptor frame. Either only the loop
   resumes (spawn = ready list, §3.2) or each coroutine keeps a caller link (minicoro/libaco model).
   The ready list is the basic option.
3. **The CQ is not quite the only queue.** Spawn needs the ready list. To keep "CQ only", spawn could
   instead submit an `IORING_OP_NOP` whose `user_data` is the new coroutine's op; that also yields an
   `await_yield()` for free. Cute, not basic.
4. **CET.** gcc 13.3 here emits the IBT + SHSTK property by default (checked with `readelf -n`). A
   hand-written `.S` with no `.note.gnu.property` leaves the linked binary unmarked, which disables
   both for the process — exactly what a custom stack switch needs. Verify the final binary with
   `readelf -n`; never "fix" a missing-property warning by adding the note unless you also emit
   `endbr64` at entry points and handle the shadow stack.
5. **First-frame alignment.** After the six pops and the `ret` into `coro_entry`, `rsp` must be
   ≡ 8 mod 16, as if `coro_entry` had been called. Put the return-address slot at a 16-aligned address
   with the six zeroed register slots directly below it. The classic symptom of getting this wrong is
   a crash in the first SSE instruction of `printf` or `memcpy`.
6. **DEFER_TASKRUN.** Create the ring on the worker thread, and reap only through
   `io_uring_submit_and_wait*` (which passes `GETEVENTS`). A bare `io_uring_submit` submits but does
   not run completion task-work; liburing's peek and for-each helpers know the flag. ioxide's raw
   loop always waits for ≥ 1 for the same reason.
7. **Stale CQEs.** Not a v1 problem: every op is one-shot and its coroutine stays parked until the
   CQE, so `&op` is always live. It becomes *the* problem the moment a multishot recv or accept exists
   (v2): its `user_data` must point at heap state kept alive until the terminal CQE (`!F_MORE`,
   `-ECANCELED`, `-ENOBUFS`).
8. **Stack size.** 64 KiB plus a guard page covers the brief's handler; stacks cannot grow. In debug
   builds fill the stack with a pattern and print the high-water mark at coroutine exit.
9. **munmap under thread-per-core.** `munmap` in a multithreaded process broadcasts TLB-shootdown
   IPIs to every thread. Harmless in v1 with keep-alive connections; pool stacks per worker before
   measuring connection churn.
10. **Headers.** liburing 2.5 and linux-libc-dev 6.8 (Ubuntu 24.04): `liburing.h` lacks
    `IORING_SETUP_NO_SQARRAY`, and both lack `IOU_PBUF_RING_INC`, `IORING_CQE_F_BUF_MORE`,
    `IORING_RECVSEND_BUNDLE`; `io_uring_prep_futex_wait` is absent. The kernel is 6.14, so the
    features work if you define the constants yourself (ioxide declares every constant it uses). Do
    **not** pass `NO_SQARRAY` to liburing 2.5 unless you have confirmed the library skips the SQ-array
    write, otherwise it scribbles over the ring; vendor liburing ≥ 2.7 as a submodule if you want it.
11. **ASan.** Wrap each switch in `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`
    (gcc 13's `sanitizer/common_interface_defs.h` has them) or ASan misreports every switch. Skip
    ASan for v1, add the two calls with v2.
12. **EOF.** Do not copy ioxide's reactor-side teardown on FIN. With a coroutine, `await_recv`
    returns 0, the handler finishes its write and closes — proper half-close semantics, and the chaos
    suite only requires that half-close never wedges the worker.

---

## 5. ringzero — what to reuse, what to drop

Reuse: `listener.c` (drop `O_NONBLOCK`, unnecessary under io_uring), the `sqe_get` submit-and-retry
helper (`reactor.c:11`), the Makefile skeleton without the `.so`, the signal-to-flag stop, `-luring`.

Drop: the dedicated acceptor thread plus the SPSC fd hand-off (ioxide's per-worker `SO_REUSEPORT`
listener is simpler and shared-nothing; the kernel balances by 4-tuple hash), the 1 ms wait timeout
(wait for ≥ 1 CQE with a 100 ms stop-check timeout instead), the `handler_fn(conn, buf, len)` callback
shape (that *is* the state machine the coroutine removes), and returning the buffer immediately after
the callback (v2 returns it when the handler says so).

ringzero's multishot recv plus provided-buffer ring (`reactor.c:73`, `io_uring_setup_buf_ring`,
`io_uring_buf_ring_add/advance`, `IOSQE_BUFFER_SELECT`, `buf_group`) is ioxide's shared mode in
liburing form, so it is the code to lift for v2.

---

## 6. After v1

- **v2 — multishot recv + provided buffers.** One buffer ring per worker; start at 4–16 KiB buffers,
  not ioxide's 32 KiB × 4096 = 128 MiB per reactor. `conn_t` lives on the heap:
  `{ fd, rx queue of (ptr, len, bid), coro_t *waiter, int refs }`. The multishot's `user_data` points at
  `conn_t` with a tag bit so dispatch tells it from an `op_t`. `await_recv` returns a queued slice at
  once or parks; the handler returns buffers (`buf_ring_add`, one `advance` per loop iteration). Copy
  from ioxide: `-ENOBUFS` parks the connection and the loop re-arms after any return; cancel the
  multishot by `user_data` on close; free `conn_t` only when the handler coroutine has exited *and*
  the recv has posted its terminal CQE — ioxide's two-owner refcount minus the cross-thread paths.
  Multishot accept the same way. `IORING_RECVSEND_BUNDLE` (6.10+) later, so one CQE carries several
  buffers.
- **v3 — timeouts, cancel, files, zero-copy.** `await_timeout(ns)` with the timespec on the stack;
  `IOSQE_IO_LINK` + `LINK_TIMEOUT` for `await_recv_timeout` (the link-timeout CQE needs an ignore tag,
  like ioxide's `KindCancel`); `ASYNC_CANCEL` by `&op` from another coroutine makes the parked one
  return `-ECANCELED`; `openat` / `read` / `splice`; `SEND_ZC` waits for both the `F_MORE` data CQE and
  the `F_NOTIF`; stop via eventfd + multishot `POLL_ADD`, or `MSG_RING` from a main-thread ring.
- **v4 — incremental buffer rings** (`IOU_PBUF_RING_INC`, 6.12+) per connection with the
  offset/refcount/kernel-done accounting from `Reactor.Loop.Incremental.cs`; registered ring fd;
  registered files; `io_uring_prep_cmd_sock` for `TCP_NODELAY` without a syscall.

---

## 7. Baselines and checks

Reference (`ioxide/bench/results/20260809T181559Z.json`; kernel 6.17, 2 reactors, 64 connections,
wrk with 8 threads, 2-byte body):

| sample | rps | CPU µs/req |
|---|---|---|
| Tcp/Raw | 877 K | 2.30 |
| Tcp/Incremental | 911 K | 2.22 |
| Tcp/Hop | 894 K | 2.26 |
| Tcp/TaskRun | 790 K | 4.73 |
| Tcp/Big (64 KiB body) | 235 K | 8.60 |

This box now runs 6.14, so re-run `Playground/Tcp/Raw` and `ringzero/rgzero` with the same wrk settings
before comparing. v1 should land on ringzero's number: same syscalls, and a switch pair costs tens of
nanoseconds against 2.3 µs of CPU per request.

Chaos cases to port from `ioxide/tests/Ioxide.Tests.Chaos/TcpChaosTests.cs` and
`ReactorChaosTests.cs` once v2 exists: a header split across many recvs (> 32 KiB); slow-loris with
no terminator (bound the buffer, refuse, keep serving); LF-only framing that never matches; dribbled
bytes while fresh clients are still answered; half-close; a response larger than the write buffer;
pool reuse leaking a predecessor's bytes; several workers on one port all answering.

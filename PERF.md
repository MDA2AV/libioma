# Performance notes

What has been tried on libioma, what it measured, and what is left. Numbers are saturated
keep-alive throughput of the HttpArena baseline handler with the server pinned to 4 cores of an
i9-14900K and the load on other cores, unless stated otherwise. Run-to-run noise is about ±1%, so
treat anything under that as "no change". Connection churn is the same load with one request per
connection.

`perf` needs the kernel-matched package and root; without it, the reliable way to compare two
builds is exactly this: pin the server to a fixed set of cores, saturate it, compare requests per
second. Higher throughput on the same cores is lower CPU per request, which is what the latency
profiles score.

## Applied

| change | effect |
|---|---|
| Pool coroutine stacks and `conn_t` per worker (no mmap/munmap per connection) | churn 27.7K → 98.6K conn/s |
| One worker per CPU in the cpuset instead of a fixed 64 | 8-core profile: 158 ms → 50 ms mean latency (was 8:1 oversubscribed) |
| Stop zeroing the 2 KB request struct and the 560-byte response struct per request | part of the ~1% below |
| `-O3` (was `-O2`) | +2.5% |
| One-pass header pick with a locale-free compare (was 3 scans × `strncasecmp`), parse straight into `req.headers`, precomputed route lengths | keep-alive +1% |
| `TCP_NODELAY` on the listener only (accepted sockets inherit it on Linux) | churn 98.8K → 114.7K conn/s |
| No `Connection: keep-alive` header on HTTP/1.1 replies (24 bytes fewer per response) | parity with libreactor's reply size |
| Provided buffers 4 KB → 2 KB | throughput-neutral, half the per-worker slab |
| Buffer-ring tail published once per loop iteration instead of per return | neutral here; less contention on many-core hosts |
| Fat LTO objects; consumers link with `-flto` | +2% (handlers inline into the engine) |
| Registered ring fd (`IORING_REGISTER_RING_FDS`) | not measurable |
| Registered file table: direct accept into slots, recv/send/close by index | keep-alive not measurable; churn +3%; connections no longer consume process fds |
| PGO (`-fprofile-generate`, train, `-fprofile-use`) on top of LTO | +1.5–2% |
| Request model: the query split into `params` eagerly, header names lower-cased at parse (8 bytes a step) so handlers compare with plain `ioma_slice_eq` | about −1% each; the price of direct data access |
| gcc 14 and C23 (was gcc 13 and gnu11) | neutral: 1.23M vs 1.22M req/s keep-alive and equal churn, wrk and oha, three interleaved rounds on 4 reactors. The standard changes what the compiler accepts, not the code it emits |

The `-D` switches: `FIXED_FILES=0` disables the file table, `NO_REG_RING` the registered ring fd,
`BUF_SIZE`/`BUF_COUNT`/`RING_ENTRIES`/`RX_QUEUE`/`STACK_SIZE`/`CORO_POOL_MAX`/`CONN_POOL_MAX` are
A recv that finds the ring empty is logged, at most once a second per worker, with counts:
`ioma: [w3] recv found no provided buffer N times ...: raise BUF_COUNT`. That line is the signal to raise
`-DBUF_COUNT` (a power of two, up to 65536; each buffer is `BUF_SIZE` bytes of the per-worker slab).
in `src/io/proactor.h`. Both ring features fall back at runtime on kernels that lack them.

### How to do PGO

Build the library and your program with `-fprofile-generate -fprofile-update=atomic`, run a
representative load (keep-alive GETs, some POSTs with Content-Length and chunked bodies, some
connection churn), stop the server with SIGINT so it exits cleanly and writes the `.gcda` files
next to the objects, then delete the objects and rebuild with `-fprofile-use -fprofile-correction`.
The objects must be compiled to the same paths in both phases.

It cannot be done inside `docker build`: a RUN step runs under Docker's default seccomp profile,
which blocks the io_uring syscalls, so the instrumented server never starts. To ship a PGO build
in a container, train once outside the build (a `docker run --security-opt seccomp=unconfined` of
the instrumented image), copy the `.gcda` files out, commit them next to the Dockerfile and COPY
them into place before the profile-use build. The profile is tied to the library commit and the
image's gcc version; with `-Wno-error=coverage-mismatch` a stale profile degrades to plain -O3 for
the changed functions instead of failing the build.

## Where the time goes

At saturation the per-request cost is about 3 µs on the i9 and about 15 µs on the benchmark box,
and nearly all of it is the kernel: the multishot recv delivery, the send, the TCP stack. The
userspace work (parse, route, serialize, two coroutine switches in and two out) is well under a
microsecond. That is why the remaining wins are in the low single digits: the peers pay the same
kernel cost, and libioma, libreactor and libxev land within a couple of percent of each other.

The 8-CPU saturation profile is the one place where every percent still matters: at the offered
500K req/s libioma sits at ~100% CPU on those cores, and queueing latency explodes near full
utilisation, so a 2% cut in CPU per request buys far more than 2% in latency.

## Not worth it here

- **SQPOLL** — a kernel thread spins on the submission ring; costs a core per ring. Thread-per-core loses.
- **Zero-copy send** — pins pages and posts an extra notification CQE; slower than a copy for replies under a few KB.
- **Send/recv bundles, incremental buffers** — help streams, not one-request-per-recv traffic.
- **NAPI busy-poll** — lower latency for more CPU per request, and the latency profiles score CPU at 0.50.
- **COOP_TASKRUN** — superseded by DEFER_TASKRUN, which is on.
- **liburing** — a wrapper over the same syscalls and mmaps; nothing faster underneath.
- **Bigger or smaller rings and queues** — 4096 entries and 4096 buffers are already far above what the load needs.

## Still open

- **Parse straight from the provided buffer** when a whole request sits in one, skipping the copy in `await_recv`. Small.
- **Connection steering** (`SO_INCOMING_CPU`, reuseport BPF) so a connection is served by the worker on its NIC queue's CPU. Real on a NIC, irrelevant on the loopback the benchmark uses.
- **Profiling on the benchmark host itself** with `perf`, to see the split of that 15 µs. This is the one thing that could reveal something not visible from the i9.

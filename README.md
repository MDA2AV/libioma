# ioma

A minimal thread-per-core io_uring runtime in C where connection handlers are stackful
coroutines: linear code with `await_recv` / `await_send`, no state machines, no callbacks.
Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core; the design notes are in
[`DESIGN.md`](DESIGN.md).

- **No liburing.** Three raw syscalls, the kernel's own structs, the rings mmap'd by hand
  (`src/uring.c`, the twin of ioxide's `Ring.cs`).
- **One ring per thread**, `SINGLE_ISSUER | DEFER_TASKRUN | NO_SQARRAY`, one `SO_REUSEPORT`
  listener per worker, one provided-buffer ring per worker. Nothing is shared, nothing is locked.
- **Multishot accept, multishot recv** with buffer select, `MSG_WAITALL` sends, `ASYNC_CANCEL` on
  close — the same io_uring shape as ioxide.
- **The proactor loop**: run spawned coroutines, re-arm recvs parked on `-ENOBUFS`, one
  `io_uring_enter` (submit everything, wait for at least one completion), dispatch the batch,
  advance the CQ head once. Handlers resume inline inside dispatch, so their sends ride the
  next enter.
- **Routing**: `user_data` is a pointer plus a tag in its low bits. A one-shot op's key is an
  `op_t` in the awaiting coroutine's stack frame, frozen while it is parked. A multishot recv's
  key is the heap `conn_t`, kept alive by a refcount until its terminal CQE.

```
make
./ioma [workers] [port]            # default 4 workers, pinned to cpus 0..3, port 8080
curl http://127.0.0.1:8080/
python3 tests/smoke.py 8080        # keep-alive, pipelining, split requests, half-close, ...
wrk -t8 -c64 -d10s http://127.0.0.1:8080/
```

Layout:

```
src/switch_x86_64.S   swap_ctx: push callee-saved, swap rsp, pop, ret
src/coro.c            stacks with a guard page, the forged first frame, resume/yield
src/uring.c           raw ring: setup, mmap, get_sqe, submit(+wait), batched CQ drain
src/proactor.c        worker: buffer ring, listener, accept/recv/send, conn lifetime, the loop
src/main.c            threads, signals, the HTTP handler
```

Rules a handler lives by: it runs on the worker that accepted it and never leaves; an await parks
it and the loop resumes it; anything it needs across an await can sit on its stack (64 KiB, no
growth); it returns to close the connection.

Requires Linux 6.6+ (for `NO_SQARRAY`; it falls back on older kernels) and a recent
`linux/io_uring.h`. Build: `gcc -O2`, no external dependencies. MIT licensed.

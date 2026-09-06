# ioma

A minimal HTTP/1.1 server framework in C on a thread-per-core io_uring runtime. You write
endpoints as plain functions, take a request, return a response, and the framework parses, routes,
serializes, and flushes. Under it, each connection is a stackful coroutine: the flush suspends it
and the io_uring completion resumes it, so the endpoint reads as linear code with no state machine.
Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core; runtime design notes in
[`DESIGN.md`](DESIGN.md).

```c
#include "http.h"

static ioma_response home(ioma_request *req)   { (void)req; return ioma_text(200, "hello\n"); }
static ioma_response echo(ioma_request *req)   { return ioma_bytes(200, "text/plain", req->body, req->body_len); }

int main(void) {
    ioma_route("GET",  "/",     home);
    ioma_route("POST", "/echo", echo);
    return ioma_run(4, 8080);          // 4 workers, one per core; blocks until Ctrl+C
}
```

```
make
./ioma                                     # 4 workers on :8080 (IOMA_WORKERS / IOMA_PORT override)
curl http://127.0.0.1:8080/
curl -d hello http://127.0.0.1:8080/echo
python3 tests/smoke.py 8080                # routing, keep-alive, pipelining, 404, half-close, ...
wrk -t8 -c64 -d10s http://127.0.0.1:8080/health
```

## Two layers

- **The HTTP framework** (`include/http.h`): `ioma_request`, `ioma_response`, the `ioma_text` /
  `ioma_json` / `ioma_bytes` / `ioma_textf` builders, `ioma_route` / `ioma_default`, and
  `ioma_run`. This is what you write endpoints against.
- **The runtime** (`include/proactor.h`): raw byte-level `await_recv` / `await_send` on the
  proactor, if you want a protocol other than HTTP. The HTTP layer is just one handler on top of it.

## How a request flows

Each accepted connection runs the serve loop on its own coroutine (`src/http.c`):

1. `await_recv` accumulates bytes into a fixed request buffer (this suspends until data arrives).
2. [picohttpparser](https://github.com/h2o/picohttpparser) parses the request line and headers,
   zero-copy, and returns "incomplete" until the full head is in, so a split request just reads more.
3. The body is read to its `Content-Length`.
4. The router matches `(method, path)` and calls your endpoint, which returns an `ioma_response`.
5. The framework serializes the status line, headers, and body, and `await_send` flushes it (this
   suspends until io_uring reports the send done).
6. Keep-alive loops for the next request on the same connection; otherwise the connection closes.

Requests and their slices point straight into the read buffer and are valid only for the endpoint
call. A response body must point at memory alive until the send completes: a string literal, static
data, or `req->scratch` (which `ioma_textf` uses).

## Why picohttpparser

One small file, zero-copy, battle-tested, and re-parse-friendly for streamed reads. It parses the
request line and headers; the body is yours, which is trivial for `Content-Length`. If you later
need chunked request bodies or strict validation without hand-rolling them, swap the parse step in
`src/http.c` for [llhttp](https://github.com/nodejs/llhttp) behind the same `ioma_request`.

## Layout

```
include/http.h          the framework API: request, response, routing, run
include/proactor.h      the runtime API: workers, await_recv / await_send
include/coro.h uring.h  coroutine and raw io_uring interfaces
src/http.c              parse, route, serialize, the connection serve loop, ioma_run
src/router.c            the route table (exact method + path match)
src/proactor.c          worker: buffer ring, listener, accept/recv/send, the proactor loop
src/coro.c switch_*.S   stackful coroutines: the guard-page stack and the register swap
src/uring.c             raw io_uring: setup, mmap, submit, batched CQ drain (no liburing)
third_party/picohttpparser   vendored HTTP request parser (MIT)
```

## v1 limits

Deliberately small, to grow: `Content-Length` bodies only (chunked answers 501); the request head
plus body must fit a 16 KiB buffer (larger answers 413/431); routing is exact `(method, path)` with
no path parameters yet; one static route table shared read-only across workers.

Requires Linux 6.6+ (for `NO_SQARRAY`; it falls back on older kernels). Build with `gcc -O2`, no
external dependencies. MIT licensed.

# libioma

A minimal HTTP/1.1 server **library** in C on a thread-per-core io_uring runtime. You write
endpoints as plain functions, take a request, return a response, and the framework parses, routes,
serializes, and flushes. Under it, each connection is a stackful coroutine: the flush suspends it
and the io_uring completion resumes it, so the endpoint reads as linear code with no state machine.
Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core; runtime design notes in
[`DESIGN.md`](DESIGN.md).

```c
#include <ioma.h>

static ioma_response home(ioma_request *req) { (void)req; return ioma_text(200, "hello\n"); }
static ioma_response echo(ioma_request *req) { return ioma_bytes(200, "text/plain", req->body, req->body_len); }

int main(void) {
    ioma_route("GET",  "/",     home);
    ioma_route("POST", "/echo", echo);
    return ioma_run(4, 8080);          // 4 workers, one per core; blocks until Ctrl+C
}
```

## Build and run

```
make                                       # builds libioma.a, libioma.so, and the examples
./ioma-hello                               # the playground demo: 4 workers on :8080
curl http://127.0.0.1:8080/
curl -d hello http://127.0.0.1:8080/echo
python3 tests/smoke.py 8080                # routing, keep-alive, pipelining, 404, half-close, ...
wrk -t8 -c64 -d10s http://127.0.0.1:8080/health
```

`IOMA_WORKERS` and `IOMA_PORT` override the demo's defaults.

## Use it in your project

**pkg-config** (after `sudo make install`, default prefix `/usr/local`):

```
sudo make install
cc app.c $(pkg-config --cflags --libs ioma) -o app
```

**CMake**, either vendored as a subdirectory:

```cmake
add_subdirectory(ioma)
target_link_libraries(myapp PRIVATE ioma::ioma)
```

or installed and found:

```cmake
find_package(ioma REQUIRED)      # after: cmake --install <build> --prefix <prefix>
target_link_libraries(myapp PRIVATE ioma::ioma)
```

Either way you `#include <ioma.h>`. Headers install under `<prefix>/include/ioma/`.

## The API

The framework surface is in `include/http.h` (pulled in by `<ioma.h>`):

- **Endpoints** return an `ioma_response`: `ioma_text` / `ioma_json` / `ioma_bytes`, or `ioma_textf`
  to format into `req->scratch`. `ioma_header_set(&res, name, value)` adds a response header.
- **Requests** expose method, path, query, headers, and body as zero-copy slices into the read
  buffer, plus case-insensitive `ioma_header_get`. Valid only for the endpoint call.
- **Routing** is exact `(method, path)` via `ioma_route`, with `ioma_default` for the fallback
  (a built-in 404 otherwise).
- **Middleware** wraps every handler: `ioma_use(mw)` registers one; inside it, call `ioma_next_run`
  to invoke the rest of the chain and the endpoint, or return without calling it to short-circuit.
- **`ioma_run(workers, port)`** starts the worker fleet and serves until SIGINT/SIGTERM.

For a non-HTTP protocol, `include/proactor.h` exposes the raw byte-level runtime
(`await_recv` / `await_send`); the HTTP layer is one handler on top of it.

## How a request flows

Each accepted connection runs the serve loop on its own coroutine (`src/http.c`):

1. `await_recv` accumulates bytes into a fixed request buffer (this suspends until data arrives).
2. [picohttpparser](https://github.com/h2o/picohttpparser) parses the request line and headers,
   zero-copy, returning "incomplete" until the full head is in, so a split request just reads more.
3. The body is read to its `Content-Length`, or a chunked body is decoded in place (both fragmentation-safe).
4. The middleware chain runs, then the router matches `(method, path)` and calls your endpoint.
5. The response head is built with memcpy of precomposed pieces plus a hand-rolled integer writer
   (no snprintf), and `await_send` flushes it (this suspends until io_uring reports the send done).
6. Keep-alive loops for the next request on the same connection; otherwise the connection closes.

## Layout

```
include/           public headers (ioma.h umbrella, http.h, proactor.h, coro.h, uring.h)
src/http.c         parse, route, serialize, the connection serve loop, ioma_run
src/router.c       the route table and middleware chain
src/proactor.c     worker: buffer ring, listener, accept/recv/send, the proactor loop
src/coro.c switch_*.S  stackful coroutines: guard-page stacks and the register swap
src/uring.c        raw io_uring: setup, mmap, submit, batched CQ drain (no liburing)
playground/hello/  the demo server, built as ./ioma-hello
third_party/picohttpparser   vendored HTTP request parser (MIT)
```

## v1 limits

Deliberately small, to grow: chunked and Content-Length request bodies (both fragmentation-safe); the request head
plus body must fit a 16 KiB buffer (larger answers 413/431); routing is exact `(method, path)` with
no path parameters yet; the route and middleware tables are set once and then read-only.

Requires Linux 6.6+ (for `NO_SQARRAY`; it falls back on older kernels). Build with `gcc -O2`, no
external dependencies. MIT licensed.

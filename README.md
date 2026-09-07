# libioma

An HTTP/1.1 server library in C. One worker per core, io_uring underneath with no liburing, and a
stackful coroutine per connection, so an endpoint is a plain function that takes a request and
returns a response. Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core. How the
runtime works is in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Build

Run `make` in the repo root. It produces `libioma.a`, `libioma.so` and the demo server
`ioma-hello`. CMake works too. Requirements: Linux 6.x, x86-64, gcc or clang.

## Run

`./ioma-hello` serves on port 8080 with one worker per core; `IOMA_WORKERS` and `IOMA_PORT`
override that. Ctrl-C stops it. It answers on `/`, `/health`, `/whoami` and `POST /echo`.

## Use it in your project

Install with `make install` (set `PREFIX` to choose where), then build against it with pkg-config
(`ioma`) or CMake (`find_package(ioma)`, link `ioma::ioma`). Adding the repo as a CMake
subdirectory works as well. Include `ioma.h`. Compile and link your program with `-flto` and the
compiler inlines your handlers into the engine (the library ships fat LTO objects); it is worth
about two percent.

An endpoint is a function that receives an `ioma_request` and returns an `ioma_response`, built
with `ioma_text`, `ioma_json`, `ioma_bytes` or `ioma_textf`. Everything in the request is a slice
(pointer and length); headers, query parameters and route parameters are key/value collections on
it, read with `ioma_header_get`, `ioma_query_get` and `ioma_route_get`. Add a response header with
`ioma_header_set`. Register endpoints with `ioma_route` (an exact path, or a pattern such as
`/users/:id`), optional middleware with `ioma_use`, a fallback with `ioma_default`, then call
`ioma_run` with a worker count (zero means one per core) and a port.
`playground/hello/main.c` is a complete example.

## Tests and limits

`tests/smoke.py` and `tests/stress.py` run against a live server, given its port. HTTP/1.1 only,
no TLS, requests up to 16 KB, Content-Length or chunked bodies. MIT licensed.

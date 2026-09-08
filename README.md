# libioxd

An HTTP/1.1 server library in C, the sibling of [ioxide](https://github.com/MDA2AV/ioxide), the .NET
library this design started from. One worker per core, io_uring underneath with no liburing, and a
stackful coroutine per connection, so an endpoint is a plain function that takes a request and
returns a response. Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core. How the
runtime works is in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Build

Run `make` in the repo root. It produces `libioxd.a`, `libioxd.so` and the demo server
`ioxd-hello`. CMake works too. Requirements: Linux 6.x, x86-64, gcc 14 or newer: the code is C23. On Ubuntu 24.04
`sudo apt install gcc-14`; make and CMake pick the newest gcc they find unless told otherwise.

## Run

`./ioxd-hello` is the smallest server: two routes, `GET /hello/:name` and a `POST /repeat/:times`
that reads the body and streams it back, one worker per core on port 8080. Ctrl-C stops it. A server exercising every feature of the request and response model
is `tests/server.c`, the fixture the test suites run against.

## Use it in your project

Install with `make install` (set `PREFIX` to choose where), then build against it with pkg-config
(`ioxd`) or CMake (`find_package(ioxd)`, link `ioxd::ioxd`). Adding the repo as a CMake
subdirectory works as well. Include `ioxd.h`. Compile and link your program with `-flto` and the
compiler inlines your handlers into the engine (the library ships fat LTO objects); it is worth
about two percent.

An endpoint is a function that receives a context holding the request and the response.
Everything in the request is a slice (pointer and length); headers, query parameters and route
parameters are key/value arrays on it that you read directly, and the body is read only when you
ask: `ioxd_body_all` reads it whole, `ioxd_body_read_until` streams it, `ioxd_body_read_next_chunk` hands over
one chunk at a time, and what you leave unread is drained.
Everything arrives as slices, bytes with a length; `ioxd_to_int`, `ioxd_to_double`, `ioxd_to_bool` and
the `ioxd_slice_*` helpers compare and convert them without copying, and fail instead of guessing.
Set the status and content type on the response, add headers with `ioxd_header`, and write the
body into its slab with `ioxd_write`, `ioxd_text` or `ioxd_printf`; the framework sends the head
in front of it, in one send when it fits and streamed when it does not. Endpoints live in groups: a group is a path prefix plus middleware, groups nest, and `ioxd_get(api, "/users/:id", user)`
under a group at `/api` answers at `/api/users/:id`, wrapped by the middleware of every group above it; the root
is `NULL`, with `ioxd_use` for middleware on everything. `ioxd_run` resolves it all once into a segment tree and flat
chains, so a request costs one walk and no scan, then serves with a worker count (zero means one per core) and a port.
Underneath, a connection is a pipe: `ioxd_run_pipes` hands a handler of your own the reader and writer the
HTTP engine uses, for raw TCP, with the same suspend-and-resume. A JSON reply is written as you go with
the `ioxd_json` writer, the shape of .NET's Utf8JsonWriter: no tree, no allocation, streamed as the slab fills. The same registrations read as a script with the `IOXD_GET`, `IOXD_GROUP` and `IOXD_USE` macros, a group's block
nesting the routes below it; the hello example and `tests/server.c` are written that way.
`playground/hello/main.c` is a complete example.

## Tests and limits

`make check` builds the fixture server, runs `tests/smoke.py` and `tests/stress.py` against it
and stops it. HTTP/1.1 only, no TLS, a request head and a body read whole up to 16 KB (streamed
bodies have no limit). MIT licensed.

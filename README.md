# libioma

An HTTP/1.1 server library in C. One worker per core, io_uring underneath with no liburing, and a
stackful coroutine per connection, so an endpoint is a plain function that takes a request and
returns a response. Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core. How the
runtime works is in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Build

Run `make` in the repo root. It produces `libioma.a`, `libioma.so` and the demo server
`ioma-hello`. CMake works too. Requirements: Linux 6.x, x86-64, gcc 14 or newer: the code is C23. On Ubuntu 24.04
`sudo apt install gcc-14`; make picks the newest gcc it finds, CMake needs `-DCMAKE_C_COMPILER=gcc-14`.

## Run

`./ioma-hello` is the smallest server: two routes, `GET /hello/:name` and a `POST /repeat/:times`
that reads the body and streams it back, one worker per core on port 8080. Ctrl-C stops it. A server exercising every feature of the request and response model
is `tests/server.c`, the fixture the test suites run against.

## Use it in your project

Install with `make install` (set `PREFIX` to choose where), then build against it with pkg-config
(`ioma`) or CMake (`find_package(ioma)`, link `ioma::ioma`). Adding the repo as a CMake
subdirectory works as well. Include `ioma.h`. Compile and link your program with `-flto` and the
compiler inlines your handlers into the engine (the library ships fat LTO objects); it is worth
about two percent.

An endpoint is a function that receives a context holding the request and the response.
Everything in the request is a slice (pointer and length); headers, query parameters and route
parameters are key/value arrays on it that you read directly, and the body is read only when you
ask: `ioma_body` reads it whole, `ioma_body_read` streams it, and what you leave unread is drained.
Set the status and content type on the response, add headers with `ioma_header`, and write the
body into its slab with `ioma_write`, `ioma_text` or `ioma_printf`; the framework sends the head
in front of it, in one send when it fits and streamed when it does not. Register endpoints with `ioma_route`
(an exact path, or a pattern such as `/users/:id`), optional middleware with `ioma_use`, a fallback
with `ioma_default`, then call `ioma_run` with a worker count (zero means one per core) and a port.
`playground/hello/main.c` is a complete example.

## Tests and limits

`make check` builds the fixture server, runs `tests/smoke.py` and `tests/stress.py` against it
and stops it. HTTP/1.1 only, no TLS, a request head and a body read whole up to 16 KB (streamed
bodies have no limit). MIT licensed.

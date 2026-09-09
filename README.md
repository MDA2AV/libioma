# libioxd

An HTTP/1.1 server library in C, the sibling of [ioxide](https://github.com/MDA2AV/ioxide), the .NET
library this design started from. One worker per core, io_uring underneath with no liburing, and a
stackful coroutine per connection, so an endpoint is a plain function that takes a request and
returns a response. Modelled on [ioxide](https://github.com/MDA2AV/ioxide)'s TCP core. How the
runtime works is in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Build

Run `make` in the repo root. It produces `libioxd.a`, `libioxd.so` and the demo server
`ioxd-hello`. CMake works too. Requirements: Linux 6.x, x86-64, gcc 14 or newer (the code is C23),
and OpenSSL 3 with its headers, which the TLS handshake links against. On Ubuntu 24.04
`sudo apt install gcc-14 libssl-dev`; make and CMake pick the newest gcc they find unless told
otherwise. TLS is built by default: `make TLS=0`, or CMake's `-DIOXD_TLS=OFF`, leaves it out and
with it the OpenSSL dependency, and `ioxd_tls_new` then returns NULL with a line saying so.

## Run

`./ioxd-hello` is the smallest server: three routes, `GET /hello/:name`, a `GET /users/:id` that writes JSON, and a `POST /repeat/:times`
that reads the body and streams it back, one worker per core on port 8080; given a directory of certificates (`sh tests/mkcerts.sh certs && ./ioxd-hello certs`) it serves the same routes over TLS on 8443 too. Ctrl-C stops it. A server exercising every feature of the request and response model
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
chains, so a request costs one walk and no scan, then serves with a worker count (zero means one per core) over every port
bound before it with `ioxd_bind(port, NULL)` - or `ioxd_bind(port, store)` for TLS, the store from
`ioxd_tls_new("<dir>")`, a directory of `<host>/cert.pem` and `key.pem` ([`TLS.md`](TLS.md)).
Underneath, a connection is a pipe: `ioxd_run_pipes` hands a handler of your own the reader and writer the
HTTP engine uses, for raw TCP, with the same suspend-and-resume. A JSON reply is written as you go with
the `ioxd_json` writer, the shape of .NET's Utf8JsonWriter: no tree, no allocation, streamed as the slab fills; a struct
described once with `IOXD_JSON_STRUCT` serializes with one call, nested objects and arrays included. The same registrations read as a script with the `IOXD_GET`, `IOXD_GROUP` and `IOXD_USE` macros, a group's block
nesting the routes below it; the hello example and `tests/server.c` are written that way.
`playground/hello/main.c` is a complete example.

## Tests and limits

`make check` builds the fixtures and runs the router test, then `tests/run-suites.sh`: the unit
test, then the smoke, conformance, stress and early-TLS suites against one HTTP fixture - one
fixture for all four, since a port the suite before it left full of `TIME_WAIT` connections resets
some of the next one's - then the pipe suite against the pipe fixture. `make check-tiny` runs the
stress suite alone against a second build starved on purpose (`-DBUF_COUNT=8 -DBUF_SIZE=64
-DRX_QUEUE=4`), so every request empties the buffer group; `make check-all` runs both. `make tidy`
runs clang-tidy over the library, and `make check-tlsfuzzer TLSFUZZER=<checkout>` runs six of
tlsfuzzer's TLS 1.3 scripts against the fixture. The suites need python3 and the `openssl` command
(`tests/mkcerts.sh` makes the certificates); `tests/tls_early.py` needs tlslite-ng and skips itself
without it, so pass a python that has it as `make check TLS_PYTHON=...`; tlsfuzzer needs its own
requirements in that python; `make tidy` needs clang-tidy. CMake runs the same script: a `check`
target over ctest, with `unit`, `router`, `suites` and `pipes` entries.

HTTP/1.1 only. TLS 1.3, terminated in the kernel after an OpenSSL handshake, one cipher suite, no
tickets and no client certificates ([`TLS.md`](TLS.md)). A request head, and a body read whole, must
fit 16 KB; streamed bodies have no limit. MIT licensed.

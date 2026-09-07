# Kotlin on libioma

A spike: libioma does the I/O in C, the endpoints are Kotlin.

`App.kt` is the program: a few routes with lambdas, then hand the main thread to libioma.
`Ioma.kt` is the binding that keeps Panama out of the app: it loads `libioma.so` through
`java.lang.foreign` (final since JDK 22), registers one C-callable dispatcher for every route with
`ioma_route_ffi`, and gives each handler a `Request` (zero-copy views of the query, body and so on)
and a `Reply` (writes the body into libioma's scratch buffer). One `Request` and one `Reply` are
reused per worker thread, so a request allocates nothing. libioma calls the dispatcher on each
worker's own thread stack, never on a coroutine stack, which is what makes calling a JVM from it
safe.

Run `./run.sh` with `kotlinc` and a JDK 22+ on PATH (tested on JDK 26.0.2.1). It serves `/` and `GET`/`POST /baseline11`
on port 8080. The handlers read the request and write the reply body straight in C memory, so the
hot path allocates nothing on the JVM side.

Rules for a handler: never block, keep it short (every connection on that worker waits while it
runs), and keep any body it returns in memory that outlives the call (the request's scratch
buffer, or a global arena). Garbage-collector pauses show up as latency; use a low-pause collector
such as ZGC for anything latency-sensitive.

Measured on 4 pinned cores with the HttpArena baseline load: the Kotlin handler serves about 1.18M
requests per second, against about 1.26M for the same handler written in C on the coroutine stack.
The difference is the hop to the thread stack; the JVM upcall itself is not measurable.

# Kotlin on libioma

A spike: libioma does the I/O in C, the endpoints are Kotlin. `Server.kt` loads `libioma.so`
through Panama (`java.lang.foreign`, final since JDK 22), turns two Kotlin functions into C function
pointers with upcall stubs, registers them with `ioma_route_ffi`, and hands the main thread to
`ioma_run`. libioma calls the handlers on each worker's own thread stack, never on a coroutine
stack, which is what makes calling a JVM from it safe.

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

# libioxd

An HTTP/1.1 server library in C for Linux: io_uring underneath, a thread per core, a stackful
coroutine per connection, TLS 1.3 terminated in the kernel. A handler reads the request and writes
the reply in straight-line code; the runtime does the waiting.

```c
static void hello(ioxd_ctx *ctx)
{
    ioxd_slice name = ctx->req.route_params[0].value;
    ioxd_printf(ctx, "hello %.*s\n", (int)name.len, name.p);
}

int main(void)
{
    IOXD_GET("/hello/:name", hello);
    ioxd_bind(8080, NULL);
    return ioxd_run(0);
}
```

The manual, with every header, whole example programs and how to build, is at
**[mda2av.github.io/libioxd](https://mda2av.github.io/libioxd/)**. `make` builds the library and the
examples; `make check` runs the tests.

MIT licensed.

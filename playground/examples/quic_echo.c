/*
 * quic_echo.c - a line echo on QUIC: every stream a client opens is served by the same pipe
 * handler that serves a TCP connection, so one function answers both. QUIC needs certificates -
 * it is TLS 1.3, always - and an application protocol to agree on; "echo" is this program's.
 *
 *     make examples && ./ioxd-example-quic_echo certs      # the store of ioxd_certs_load: certs/default/{cert,key}.pem
 *     printf 'hello\n' | nc 127.0.0.1 8080                 # the same echo over TCP
 *     # over QUIC: any client that can open a stream with ALPN "echo", e.g. aioquic in python
 *
 * A stream is a pipe: read what the peer sent, write what goes back, return when done - the
 * return sends the stream's end. The handler runs on a coroutine of its own per stream, so a
 * slow stream never holds another. A one-way stream reads but cannot be written: ioxd_pipe_write
 * returns -1 on it.
 */
#include <ioxd.h>

#include <string.h>

/* Lines in, echoes out, until the peer ends the stream (or the connection). */
static void echo(ioxd_pipe *pipe)
{
    for (;;) {
        ioxd_slice live = { NULL, 0 };
        if (ioxd_pipe_read(pipe, &live) <= 0)            /* the end, or the peer is gone */
            return;
        const char *nl = memchr(live.p, '\n', live.len);
        if (!nl) {                                        /* half a line: wait for the rest */
            ioxd_pipe_examine(pipe, live.len);
            continue;
        }
        size_t len = (size_t)(nl - live.p) + 1;
        char  *out = ioxd_pipe_reserve(pipe, 6 + len);   /* "echo: " + the line, straight into the slab */
        if (!out)
            return;
        memcpy(out, "echo: ", 6);
        memcpy(out + 6, live.p, len);
        ioxd_pipe_advance(pipe, 6 + len);
        ioxd_pipe_drop(pipe, len);
        if (ioxd_pipe_flush(pipe) < 0)
            return;
    }
}

int main(int argc, char **argv)
{
    ioxd_certs *certs = ioxd_certs_load(argc > 1 ? argv[1] : "certs");
    if (!certs)
        return 1;
    ioxd_bind(8080, NULL);                                /* the echo over TCP, plain */
    ioxd_bind_quic(8443, certs, (const char *const[]){ "echo", NULL });   /* and over QUIC streams */
    return ioxd_run_pipes(0, echo);
}

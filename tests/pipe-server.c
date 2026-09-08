/*
 * pipe-server.c - a line echo server on ioxd_run_pipes: the pipe API without HTTP. Each line
 * comes back as "echo: <line>"; "quit" ends the connection; a line longer than the pipe's
 * buffer ends it too (the reader reports no room). `make check` runs tests/pipes.py against it.
 */
#include <ioxd.h>

#include <stdlib.h>
#include <string.h>

/* One connection: lines in, echoes out, until the peer leaves. */
static void echo(ioxd_pipe *pipe)
{
    for (;;) {
        ioxd_slice live = { NULL, 0 };
        if (ioxd_pipe_read(pipe, &live) <= 0)            /* the end, or no room for a longer line */
            return;
        const char *nl = memchr(live.p, '\n', live.len);
        if (!nl) {                                        /* half a line: wait for the rest */
            ioxd_pipe_examine(pipe, live.len);
            continue;
        }
        size_t n = (size_t)(nl - live.p) + 1;
        if (n == 5 && memcmp(live.p, "quit\n", 5) == 0)
            return;
        char *out = ioxd_pipe_reserve(pipe, 6 + n);       /* format straight into the slab */
        if (!out)
            return;
        memcpy(out, "echo: ", 6);                         /* NOLINT(bugprone-not-null-terminated-result): bytes, not a C string */
        memcpy(out + 6, live.p, n);
        ioxd_pipe_advance(pipe, 6 + n);
        ioxd_pipe_drop(pipe, n);
        if (ioxd_pipe_flush(pipe) < 0)
            return;
    }
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? (int)strtol(argv[1], NULL, 10) : 8100;
    return ioxd_run_pipes(2, port, echo);
}

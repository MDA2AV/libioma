/*
 * pipe-server.c - a line echo server on ioxd_run_pipes: the pipe API without HTTP. Each line
 * comes back as "echo: <line>"; "quit" ends the connection; a line longer than the pipe's
 * buffer ends it too (the reader reports no room). Two commands beside the echo take the rest of
 * the API: "copy N" reads the next N bytes into a buffer of the handler's own and sends them
 * back, "hold N" waits for them where the kernel left them - however many receives that takes -
 * keeps them (which consumes them, so nothing is read twice), writes them back out and gives the
 * reader its room again. `make check` runs tests/pipes.py against it. With IOXD_CERTS naming a
 * certificate store, the same echo answers QUIC streams on the next port up, protocol "echo":
 * tests/quic.py talks to that one.
 */
#include <ioxd.h>

#include <stdlib.h>
#include <string.h>

/* The count on a "<name> N" line, or -1 when the line is not one of them. */
static long command(ioxd_slice line, const char *name)
{
    size_t len = strlen(name);
    if (line.len < len + 3 || memcmp(line.p, name, len) != 0 || line.p[len] != ' ')
        return -1;
    char   digits[16];
    size_t n = line.len - len - 2;                        /* without the name, the space, the newline */
    if (n >= sizeof digits)
        return -1;
    memcpy(digits, line.p + len + 1, n);
    digits[n] = '\0';
    char *end;
    long  value = strtol(digits, &end, 10);
    return *end == '\0' && value >= 0 ? value : -1;
}

/* "copy N": the next n bytes into a buffer of ours. ioxd_pipe_copy hands over what it has, so the
 * loop asks again until they are all in; ioxd_pipe_send is the write and the flush in one. */
static int copy_back(ioxd_pipe *pipe, size_t n)
{
    char out[6 + 256 + 1];                                /* "copy: " + the bytes + '\n' */
    if (n > 256)
        return -1;
    memcpy(out, "copy: ", 6);                             /* NOLINT(bugprone-not-null-terminated-result): bytes, not a C string */
    size_t got = 0;
    while (got < n) {
        int rc = ioxd_pipe_copy(pipe, out + 6 + got, n - got);
        if (rc <= 0)
            return -1;
        got += (size_t)rc;
    }
    out[6 + n] = '\n';
    return ioxd_pipe_send(pipe, out, 6 + n + 1);
}

/* "hold N": wait until n bytes are live - they may arrive over several receives - then keep them
 * where they lie, write them back out and release the run. Keeping consumes them, so the read
 * after this waits for new bytes instead of seeing these again. */
static int hold_back(ioxd_pipe *pipe, size_t n)
{
    const char *kept = NULL;
    while (!kept) {
        ioxd_slice live = { NULL, 0 };
        if (ioxd_pipe_read(pipe, &live) <= 0)
            return -1;
        if (live.len < n) {
            ioxd_pipe_examine(pipe, live.len);            /* seen, not consumed: wait for the rest */
            continue;
        }
        kept = ioxd_pipe_keep(pipe, n);
        if (!kept)
            return -1;
    }
    int rc = ioxd_pipe_write(pipe, "hold: ", 6);
    if (rc == 0)
        rc = ioxd_pipe_write(pipe, kept, n);
    if (rc == 0)
        rc = ioxd_pipe_write(pipe, "\n", 1);
    if (rc == 0)
        rc = ioxd_pipe_flush(pipe);
    ioxd_pipe_release(pipe);                              /* the kept run's room, back to the reader */
    return rc;
}

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
        ioxd_slice line = { live.p, (size_t)(nl - live.p) + 1 };
        if (line.len == 5 && memcmp(line.p, "quit\n", 5) == 0)
            return;
        long copy = command(line, "copy");
        long hold = command(line, "hold");
        if (copy >= 0 || hold >= 0) {
            ioxd_pipe_drop(pipe, line.len);               /* the command goes; live.p is stale after this */
            if ((copy >= 0 ? copy_back(pipe, (size_t)copy) : hold_back(pipe, (size_t)hold)) < 0)
                return;
            continue;
        }
        char *out = ioxd_pipe_reserve(pipe, 6 + line.len); /* format straight into the slab */
        if (!out)
            return;
        memcpy(out, "echo: ", 6);                         /* NOLINT(bugprone-not-null-terminated-result): bytes, not a C string */
        memcpy(out + 6, line.p, line.len);
        ioxd_pipe_advance(pipe, 6 + line.len);
        ioxd_pipe_drop(pipe, line.len);
        if (ioxd_pipe_flush(pipe) < 0)
            return;
    }
}

int main(int argc, char **argv)
{
    int         port  = argc > 1 ? (int)strtol(argv[1], NULL, 10) : 8100;
    const char *certs = getenv("IOXD_CERTS");                  /* NOLINT(concurrency-mt-unsafe): no threads yet */
    ioxd_bind(port, NULL);
    if (certs && *certs) {
        ioxd_certs *store = ioxd_certs_load(certs);
        if (!store || ioxd_bind_quic(port + 1, store, (const char *const[]){ "echo", NULL }) != 0)
            return 1;
    }
    return ioxd_run_pipes(2, echo);
}

/*
 * pipes.c - a protocol of your own on the pipe the HTTP engine itself reads and writes through:
 * raw TCP, one coroutine per connection, the same suspend-and-resume. A frame here is a 4-byte
 * big-endian length and a payload; the reply is the payload reversed, framed the same way.
 *
 *     make examples && ./ioxd-example-pipes
 *     printf '\0\0\0\5hello' | nc -q1 127.0.0.1 8100 | xxd     # 00000005 6f6c6c6568
 *
 * Reading never copies unless it has to: read hands over the bytes the kernel delivered as one
 * span, in place; copy takes a fixed-size piece into your own buffer; keep consumes bytes but
 * holds them, contiguous with what was kept before, until release - so a message that arrives
 * in several deliveries is assembled by the reader (in the kernel's buffer while it fits one,
 * in the pipe's own once it spans two) and kept() is the whole of it. Writing goes through a
 * slab: reserve room, write into it, advance, flush.
 */
#include <ioxd.h>

#include <stdint.h>

#define FRAME_MAX 16000                              /* what fits the pipe's 16 KB with room to spare */

static void frames(ioxd_pipe *pipe)
{
    for (;;) {
        /* the length: four bytes, whatever deliveries they came in */
        unsigned char head[4];
        for (int got = 0; got < 4;) {
            int n = ioxd_pipe_copy(pipe, head + got, (size_t)(4 - got));
            if (n <= 0)
                return;                              /* the peer is done, or gone */
            got += n;
        }
        size_t len = (size_t)head[0] << 24 | (size_t)head[1] << 16 | (size_t)head[2] << 8 | head[3];
        if (len == 0 || len > FRAME_MAX)
            return;                                  /* not a frame we take: close */

        /* the payload: kept in place as it arrives, one span at the end */
        for (size_t have = 0; have < len;) {
            ioxd_slice live = { NULL, 0 };
            if (ioxd_pipe_read(pipe, &live) <= 0)
                return;
            size_t take = live.len < len - have ? live.len : len - have;
            if (!ioxd_pipe_keep(pipe, take))
                return;                              /* FULL: kept plus live outgrew the pipe */
            have += take;
        }
        ioxd_slice payload = ioxd_pipe_kept(pipe);   /* len bytes, contiguous */

        /* the reply, framed the same way, written straight into the slab: the length, then the
         * payload reversed in pieces no larger than the slab (8 KB) - reserve flushes what is
         * there when a piece would not fit, so a reply of any size streams through it */
        unsigned char *out = ioxd_pipe_reserve(pipe, 4);
        if (!out)
            return;
        out[0] = (unsigned char)(len >> 24); out[1] = (unsigned char)(len >> 16);
        out[2] = (unsigned char)(len >> 8);  out[3] = (unsigned char)len;
        ioxd_pipe_advance(pipe, 4);
        for (size_t done = 0; done < len;) {
            size_t piece = len - done < 4096 ? len - done : 4096;
            out = ioxd_pipe_reserve(pipe, piece);
            if (!out)
                return;
            for (size_t i = 0; i < piece; i++)
                out[i] = (unsigned char)payload.p[len - 1 - done - i];
            ioxd_pipe_advance(pipe, piece);
            done += piece;
        }
        if (ioxd_pipe_flush(pipe) < 0)
            return;
        ioxd_pipe_release(pipe);                     /* the kept bytes go; the next frame's stay */
    }
}

int main(void)
{
    ioxd_bind(8100, NULL);
    return ioxd_run_pipes(0, frames);
}

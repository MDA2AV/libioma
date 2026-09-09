/*
 * io/pipe.h - a connection as a pipe: a reader over the buffers the kernel filled and a writer
 * over a slab. Every call that must wait suspends the calling coroutine and the worker's loop
 * resumes it on the completion, so the same code drives any connection the I/O plane runs. The
 * HTTP engine reads through the reader; ioxd_run_pipes hands a pipe to a handler of your own.
 */
#pragma once

#include "ioxd.h"
#include "io/conn.h"

/* The reader hands out received bytes as one contiguous span at a time: in place in the kernel's
 * buffer when they lie within one, gathered into the consumer's buffer when they span, or when
 * the consumer keeps bytes it wants contiguous. Live bytes are the ones not yet consumed; kept
 * bytes stay where they are - the consumer's, to point into and even overwrite - until release.
 * At most two kernel buffers are held: one with kept bytes of earlier runs, one with the live
 * bytes and the run in progress. */

// BUF_SIZE can be increased to keep entire requests in a single rx_item, avoiding buffering
typedef struct ioxd_pipereader {
    conn_t        *conn;
    char          *buf;                 /* the gathering buffer, the consumer's */
    size_t         cap;
    size_t         floor;               /* buf[0, floor): kept bytes */
    bool           live_in_buf;         /* the live bytes are buf[buf_pos, buf_end), else in cur */
    size_t         buf_pos, buf_end;
    struct rx_item cur;                 /* the kernel buffer the live bytes sit in */
    bool           has_cur;
    size_t         cur_pos;             /* its bytes consumed so far */
    struct rx_item pinned;              /* a kernel buffer that holds frozen kept bytes */
    bool           has_pinned;
    bool           cur_is_pinned;       /* cur and pinned are the same buffer */
    bool           run_in_cur;          /* the run in progress sits in cur (in place), else in buf */
    size_t         run_start, run_len;
    size_t         examined;            /* live bytes the consumer looked at without consuming */
    bool           eof;
    int            error;               /* 0, or IOXD_PIPE_GONE / IOXD_PIPE_FULL, sticky */
} ioxd_pipereader;

void        ioxd_pipereader_init     (ioxd_pipereader *pr, conn_t *conn, char *buf, size_t cap);
void        ioxd_pipereader_close    (ioxd_pipereader *pr);                    /* returns the buffers it holds */
int         ioxd_pipereader_read     (ioxd_pipereader *pr, ioxd_slice *live);  /* 1: live bytes with something unexamined; waits for more otherwise; 0 at the end of input; <0 error */
void        ioxd_pipereader_examine  (ioxd_pipereader *pr, size_t n);          /* looked at n live bytes: the next read waits for more */
void        ioxd_pipereader_drop     (ioxd_pipereader *pr, size_t n);          /* consume n live bytes */
const char *ioxd_pipereader_keep     (ioxd_pipereader *pr, size_t n);          /* consume n live bytes but keep them, contiguous with the run; where they are, or nullptr: no room (FULL), or n past the live bytes */
void        ioxd_pipereader_run_begin(ioxd_pipereader *pr);                    /* freeze the run; the next keep starts another */
ioxd_slice  ioxd_pipereader_run      (const ioxd_pipereader *pr);              /* the run in progress */
void        ioxd_pipereader_release  (ioxd_pipereader *pr);                    /* forget every kept byte; live bytes stay */
int         ioxd_pipereader_copy     (ioxd_pipereader *pr, void *dst, size_t n);   /* up to n live bytes into dst, consumed; >0, 0 at the end, <0 error */
int         ioxd_pipereader_avail    (ioxd_pipereader *pr, ioxd_slice *live);   /* like read, but never waits: 0 when nothing unexamined was delivered; <0 error */
bool        ioxd_pipereader_inject   (ioxd_pipereader *pr, const void *data, size_t n);   /* bytes that arrived by another route, appended to the live bytes; false: no room */

/* The writer: a slab with a head and a tail. Data goes in at the tail (reserve/advance, or write);
 * a frame's front goes into the lead just before the pending data and its back into the slack just
 * after it, and one flush sends the whole span. The HTTP reply puts its head and a chunk's size
 * line in front and a chunk's CRLF behind; a raw pipe may never touch them. */
typedef struct ioxd_pipewriter {
    conn_t *conn;
    char   *buf;                        /* [lead][cap][slack] */
    size_t  lead, cap, slack;
    size_t  head;                       /* bytes of the lead in use: a frame's front             */
    size_t  len;                        /* data bytes                                            */
    size_t  tail;                       /* bytes of the slack in use: a frame's back             */
    bool    failed;                     /* the peer is gone: every call fails from here on       */
} ioxd_pipewriter;

void   ioxd_pipewriter_init   (ioxd_pipewriter *pw, conn_t *conn, char *buf, size_t lead, size_t cap, size_t slack);
void   ioxd_pipewriter_reset  (ioxd_pipewriter *pw);                          /* drop everything pending           */
void  *ioxd_pipewriter_reserve(ioxd_pipewriter *pw, size_t n);                /* n bytes at the tail, flushing first when they do not fit; nullptr on failure or n > cap */
void   ioxd_pipewriter_advance(ioxd_pipewriter *pw, size_t n);
char  *ioxd_pipewriter_front  (ioxd_pipewriter *pw, size_t n);                /* n bytes just before the pending span; nullptr when the lead has no room  */
char  *ioxd_pipewriter_back   (ioxd_pipewriter *pw, size_t n);                /* n bytes just after it; nullptr when the slack has no room                */
int    ioxd_pipewriter_write  (ioxd_pipewriter *pw, const void *data, size_t n);   /* copy in; larger than the slab goes straight out          */
int    ioxd_pipewriter_through(ioxd_pipewriter *pw, const void *data, size_t n);   /* send now, ahead of the pending span, bypassing the slab   */
int    ioxd_pipewriter_flush  (ioxd_pipewriter *pw);                          /* send front, data and back; suspends */
int    ioxd_pipewriter_send   (ioxd_pipewriter *pw, const void *data, size_t n);   /* write, then flush                                         */

/* Where the next byte goes, and how many fit before the slab is full. */
static inline char *ioxd_pipewriter_at(const ioxd_pipewriter *pw)
{
    return pw->buf + pw->lead + pw->len;
}
static inline size_t ioxd_pipewriter_room(const ioxd_pipewriter *pw)
{
    return pw->cap - pw->len;
}

/* The pair every handler receives, HTTP or raw (the public ioxd_pipe). Its buffers live on the
 * connection's coroutine stack. */
#ifndef IOXD_PIPE_GATHER
#define IOXD_PIPE_GATHER 16384              /* the reader's gathering buffer: kept plus live bytes */
#endif
#ifndef IOXD_PIPE_LEAD
#define IOXD_PIPE_LEAD   512                /* in front of the slab: a frame's front               */
#endif
#ifndef IOXD_PIPE_CAP
#define IOXD_PIPE_CAP    8192               /* the slab: bytes buffered before a flush             */
#endif
#ifndef IOXD_PIPE_SLACK
#define IOXD_PIPE_SLACK  8                  /* behind the slab: a frame's back                     */
#endif
struct ioxd_pipe {
    ioxd_pipereader in;
    ioxd_pipewriter out;
};
void ioxd__pipe_init (struct ioxd_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t lead, size_t cap, size_t slack);
void ioxd__pipe_close(struct ioxd_pipe *p);

/* ── pipe.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * io/pipe.c - the reader and the writer of io/pipe.h, and the public pipe on top of them.
 */

/* at file scope:
 *   - ── the reader ──────────────────────────────────────────────────────────────────────────
 *     [void ioxd_pipereader_init(ioxd_pipereader *pr, conn_t *conn, char *buf, size_t c]
 *   - ── the writer ──────────────────────────────────────────────────────────────────────────
 *     [void ioxd_pipewriter_init(ioxd_pipewriter *pw, conn_t *conn, char *buf, size_t l]
 *   - ── the pipe ────────────────────────────────────────────────────────────────────────────
 *     [void ioxd__pipe_init(struct ioxd_pipe *p, conn_t *conn, char *gather, size_t gat]
 */

/* live_span:
 * The live bytes: in buf, or in the current kernel buffer.
 */

/* cur_done:
 * Let the current kernel buffer go once nothing is left in it, neither live bytes nor the run.
 * One that also holds frozen kept bytes lives on as `pinned`.
 */

/* compact:
 * Reclaim the bytes dropped from the front of buf's live region.
 */

/* gather:
 * Move the current buffer's run and live bytes into buf, so what follows can join them. A run
 * that was kept in place has pointers out to it, so its buffer stays pinned rather than going
 * back to the ring - unless another buffer is pinned already (the HTTP engine's head), in
 * which case the run just moves and ioxd_pipereader_run is where to find it.
 */

/* refuse:
 * A buffer that cannot be used: back to the ring, and the reader is done.
 */

/* more:
 * More bytes: the next kernel buffer, in place when nothing is live, else appended in buf.
 *   - nothing live: this buffer is the live span  [if (!pr->live_in_buf && !pr->has_cur) {]
 *   - the live bytes leave the current buffer first  [if (!pr->live_in_buf && !gather(pr))]
 */

/* consume:
 * Forget n live bytes of the current place; never more than there are.
 */

/* ioxd_pipereader_keep:
 *   - more than is live: the caller's mistake  [return nullptr;]
 *   - a run starts where the live bytes are  [if (pr->run_len == 0) {]
 *   - dropped bytes in between: slide these down  [} else if (pr->buf_pos != pr->floor) {]
 *   - in place  [} else if (pr->has_cur && (pr->run_len == 0 || pr->run_in_cur)) {]
 *   - the run is in buf, the live bytes are not: copy across  [} else if (pr->has_cur) {]
 *   - buf's (empty) live region moves up with it  [pr->buf_pos = pr->buf_end = pr->floor;]
 *   - nothing live: the caller's mistake  [return nullptr;]
 */

/* ioxd_pipereader_run_begin:
 *   - another buffer is pinned already: this run moves to buf  [if (pr->has_pinned &&
 *     !pr->cur_is_pinned) {]
 */

/* ioxd_pipereader_release:
 *   - it lives on as the current buffer  [pr->cur_is_pinned = false;]
 */

/* ioxd_pipereader_avail:
 *   - all seen: take a delivered buffer, if one is queued  [while (l.len <= pr->examined) {]
 */

/* ioxd_pipereader_inject:
 *   - live bytes, or a run, in place: they move first  [if (pr->has_cur) {]
 */

/* ioxd_pipewriter_advance:
 *   - never past the slab, whatever was claimed  [pw->len += n < room ? n : room;]
 */

/* ioxd_pipewriter_write:
 *   - larger than the slab: straight from the caller's memory  [if (n > pw->cap)]
 */

/* ioxd__pipe_close:
 *   - what a handler left in the slab still goes  [ioxd_pipewriter_flush(&p->out);]
 */

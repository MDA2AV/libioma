/*
 * io/pipe.h - a connection as a pipe: a reader over the buffers the kernel filled and a writer
 * over a slab. Every call that must wait suspends the calling coroutine and the worker's loop
 * resumes it on the completion, so the same code drives any connection the I/O plane runs. The
 * HTTP engine reads through the reader; ioma_run_pipes hands a pipe to a handler of your own.
 */
#pragma once

#include "ioma.h"
#include "io/conn.h"

/* The reader hands out received bytes as one contiguous span at a time: in place in the kernel's
 * buffer when they lie within one, gathered into the consumer's buffer when they span, or when
 * the consumer keeps bytes it wants contiguous. Live bytes are the ones not yet consumed; kept
 * bytes stay where they are - the consumer's, to point into and even overwrite - until release.
 * At most two kernel buffers are held: one with kept bytes of earlier runs, one with the live
 * bytes and the run in progress. */

// BUF_SIZE can be increased to keep entire requests in a single rx_item, avoiding buffering
typedef struct ioma_pipereader {
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
    int            error;               /* 0, or IOMA_PIPE_GONE / IOMA_PIPE_FULL, sticky */
} ioma_pipereader;

void        ioma_pipereader_init     (ioma_pipereader *pr, conn_t *conn, char *buf, size_t cap);
void        ioma_pipereader_close    (ioma_pipereader *pr);                    /* returns the buffers it holds */
int         ioma_pipereader_read     (ioma_pipereader *pr, ioma_slice *live);  /* 1: live bytes with something unexamined; waits for more otherwise; 0 at the end of input; <0 error */
void        ioma_pipereader_examine  (ioma_pipereader *pr, size_t n);          /* looked at n live bytes: the next read waits for more */
void        ioma_pipereader_drop     (ioma_pipereader *pr, size_t n);          /* consume n live bytes */
const char *ioma_pipereader_keep     (ioma_pipereader *pr, size_t n);          /* consume n live bytes, kept: contiguous with the run; nullptr when there is no room */
void        ioma_pipereader_run_begin(ioma_pipereader *pr);                    /* freeze the run; the next keep starts another */
ioma_slice  ioma_pipereader_run      (const ioma_pipereader *pr);              /* the run in progress */
void        ioma_pipereader_release  (ioma_pipereader *pr);                    /* forget every kept byte; live bytes stay */
int         ioma_pipereader_copy     (ioma_pipereader *pr, void *dst, size_t n);   /* up to n live bytes into dst, consumed; >0, 0 at the end, <0 error */

/* The writer: a slab with a head and a tail. Data goes in at the tail (reserve/advance, or write);
 * a frame's front goes into the lead just before the pending data and its back into the slack just
 * after it, and one flush sends the whole span. The HTTP reply puts its head and a chunk's size
 * line in front and a chunk's CRLF behind; a raw pipe may never touch them. */
typedef struct ioma_pipewriter {
    conn_t *conn;
    char   *buf;                        /* [lead][cap][slack] */
    size_t  lead, cap, slack;
    size_t  head;                       /* bytes of the lead in use: a frame's front             */
    size_t  len;                        /* data bytes                                            */
    size_t  tail;                       /* bytes of the slack in use: a frame's back             */
    bool    failed;                     /* the peer is gone: every call fails from here on       */
} ioma_pipewriter;

void   ioma_pipewriter_init   (ioma_pipewriter *pw, conn_t *conn, char *buf, size_t lead, size_t cap, size_t slack);
void   ioma_pipewriter_reset  (ioma_pipewriter *pw);                          /* drop everything pending           */
void  *ioma_pipewriter_reserve(ioma_pipewriter *pw, size_t n);                /* n bytes at the tail, flushing first when they do not fit; nullptr on failure or n > cap */
void   ioma_pipewriter_advance(ioma_pipewriter *pw, size_t n);
char  *ioma_pipewriter_front  (ioma_pipewriter *pw, size_t n);                /* n bytes just before the pending span; nullptr when the lead has no room  */
char  *ioma_pipewriter_back   (ioma_pipewriter *pw, size_t n);                /* n bytes just after it; nullptr when the slack has no room                */
int    ioma_pipewriter_write  (ioma_pipewriter *pw, const void *data, size_t n);   /* copy in; larger than the slab goes straight out          */
int    ioma_pipewriter_through(ioma_pipewriter *pw, const void *data, size_t n);   /* send now, ahead of the pending span, bypassing the slab   */
int    ioma_pipewriter_flush  (ioma_pipewriter *pw);                          /* send front, data and back; suspends */
int    ioma_pipewriter_send   (ioma_pipewriter *pw, const void *data, size_t n);   /* write, then flush                                         */

/* Where the next byte goes, and how many fit before the slab is full. */
static inline char *ioma_pipewriter_at(const ioma_pipewriter *pw)
{
    return pw->buf + pw->lead + pw->len;
}
static inline size_t ioma_pipewriter_room(const ioma_pipewriter *pw)
{
    return pw->cap - pw->len;
}

/* The pair every handler receives, HTTP or raw (the public ioma_pipe). Its buffers live on the
 * connection's coroutine stack. */
#ifndef IOMA_PIPE_GATHER
#define IOMA_PIPE_GATHER 16384              /* the reader's gathering buffer: kept plus live bytes */
#endif
#ifndef IOMA_PIPE_LEAD
#define IOMA_PIPE_LEAD   512                /* in front of the slab: a frame's front               */
#endif
#ifndef IOMA_PIPE_CAP
#define IOMA_PIPE_CAP    8192               /* the slab: bytes buffered before a flush             */
#endif
#ifndef IOMA_PIPE_SLACK
#define IOMA_PIPE_SLACK  8                  /* behind the slab: a frame's back                     */
#endif
struct ioma_pipe {
    ioma_pipereader in;
    ioma_pipewriter out;
};
void ioma__pipe_init (struct ioma_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t lead, size_t cap, size_t slack);
void ioma__pipe_close(struct ioma_pipe *p);

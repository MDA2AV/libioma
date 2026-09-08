/*
 * io/pipe.h - a connection as a pipe: a reader over the buffers the kernel filled and a writer
 * over a slab. Every call that must wait suspends the calling coroutine and the worker's loop
 * resumes it on the completion, so the same code drives any connection the I/O plane runs. The
 * HTTP engine reads through the reader; ioma_run_pipes hands a pipe to a handler of your own.
 */
#pragma once

#include "ioma.h"
#include "io/proactor.h"

/* The reader hands out received bytes as one contiguous span at a time: in place in the kernel's
 * buffer when they lie within one, gathered into the consumer's buffer when they span, or when
 * the consumer keeps bytes it wants contiguous. Live bytes are the ones not yet consumed; kept
 * bytes stay where they are - the consumer's, to point into and even overwrite - until release.
 * At most two kernel buffers are held: one with kept bytes of earlier runs, one with the live
 * bytes and the run in progress. */
typedef struct ioma_reader {
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
} ioma_reader;

void        ioma_reader_init     (ioma_reader *r, conn_t *conn, char *buf, size_t cap);
void        ioma_reader_close    (ioma_reader *r);                    /* returns the buffers it holds */
int         ioma_reader_read     (ioma_reader *r, ioma_slice *live);  /* 1: live bytes with something unexamined; waits for more otherwise; 0 at the end of input; <0 error */
void        ioma_reader_examine  (ioma_reader *r, size_t n);          /* looked at n live bytes: the next read waits for more */
void        ioma_reader_drop     (ioma_reader *r, size_t n);          /* consume n live bytes */
const char *ioma_reader_keep     (ioma_reader *r, size_t n);          /* consume n live bytes, kept: contiguous with the run; nullptr when there is no room */
void        ioma_reader_run_begin(ioma_reader *r);                    /* freeze the run; the next keep starts another */
ioma_slice  ioma_reader_run      (const ioma_reader *r);              /* the run in progress */
void        ioma_reader_release  (ioma_reader *r);                    /* forget every kept byte; live bytes stay */
int         ioma_reader_copy     (ioma_reader *r, void *dst, size_t n);   /* up to n live bytes into dst, consumed; >0, 0 at the end, <0 error */

/* The writer: a slab, sent on flush. */
typedef struct ioma_writer {
    conn_t *conn;
    char   *buf;
    size_t  cap, len;
    bool    failed;                     /* the peer is gone: every call fails from here on */
} ioma_writer;

void  ioma_writer_init   (ioma_writer *w, conn_t *conn, char *buf, size_t cap);
void *ioma_writer_reserve(ioma_writer *w, size_t n);                  /* n bytes at the tail, flushing first when they do not fit; nullptr on failure or n > cap */
void  ioma_writer_advance(ioma_writer *w, size_t n);
int   ioma_writer_write  (ioma_writer *w, const void *data, size_t n);  /* copy in; larger than the slab goes straight out */
int   ioma_writer_flush  (ioma_writer *w);                            /* send the slab; suspends */
int   ioma_writer_send   (ioma_writer *w, const void *data, size_t n);  /* write, then flush */

/* The pair, as the public API sees it. */
struct ioma_pipe {
    ioma_reader in;
    ioma_writer out;
};
void ioma__pipe_init (struct ioma_pipe *p, conn_t *conn, char *gather, size_t gather_cap, char *slab, size_t slab_cap);
void ioma__pipe_close(struct ioma_pipe *p);

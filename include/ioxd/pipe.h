/*
 * ioxd/pipe.h - a connection as a pipe: a reader over the bytes the kernel received and a
 * writer over a slab, for handlers of protocols other than HTTP.
 */
#pragma once

#include <stddef.h>

#include "ioxd/slice.h"

/* ── pipes ─────────────────────────────────────────────────────────────────────────────── */

/* A connection as a pipe: a reader over the bytes the kernel received and a writer over a slab.
 * Every call that must wait suspends the connection's coroutine, and the worker's loop resumes
 * it on the completion, so a handler reads and writes in straight-line code. The HTTP engine is
 * one such handler; ioxd_run_pipes (ioxd/run.h) runs one of yours on raw TCP connections instead. */
typedef struct ioxd_pipe ioxd_pipe;
typedef void (*ioxd_pipe_handler)(ioxd_pipe *pipe);         /* run one with ioxd_run_pipes (ioxd/run.h) */

/* Reading. The live bytes are the ones received and not yet consumed, always handed out as one
 * contiguous span - in place in the kernel's buffer when they lie within one. read returns 1
 * with them once some are unexamined, otherwise it waits for more; examine says how many were
 * looked at without being consumed, so the next read waits for more rather than returning the
 * same bytes; drop consumes (never more than is live); keep consumes but leaves the bytes where
 * they are and valid until release - it returns where those n bytes are now, or NULL when they
 * would not fit the pipe's buffer (the pipe is then FULL) or n is more than is live; kept is
 * everything kept since the last release as one span, wherever the reader has it now (a run
 * that outgrew its kernel buffer moved to the pipe's own, so the span, not a pointer from an
 * earlier keep, is what to read a whole message through); copy is the plain read into your own
 * buffer. read and copy return 0 at the end of input, IOXD_PIPE_GONE on a dead peer,
 * IOXD_PIPE_FULL when kept plus live bytes would exceed the pipe's buffer (16 KB). A handler
 * that returns with bytes still in the writer's slab has them sent before the connection closes. */
#define IOXD_PIPE_GONE (-1)
#define IOXD_PIPE_FULL (-2)
int         ioxd_pipe_read   (ioxd_pipe *pipe, ioxd_slice *live);
void        ioxd_pipe_examine(ioxd_pipe *pipe, size_t n);
void        ioxd_pipe_drop   (ioxd_pipe *pipe, size_t n);
const char *ioxd_pipe_keep   (ioxd_pipe *pipe, size_t n);
ioxd_slice  ioxd_pipe_kept   (ioxd_pipe *pipe);
void        ioxd_pipe_release(ioxd_pipe *pipe);
int         ioxd_pipe_copy   (ioxd_pipe *pipe, void *dst, size_t n);

/* Writing: a slab, sent on flush. reserve n bytes to write into directly and advance by what was
 * written, or write to copy in; send is write then flush. -1 once the peer is gone. */
void  *ioxd_pipe_reserve(ioxd_pipe *pipe, size_t n);
void   ioxd_pipe_advance(ioxd_pipe *pipe, size_t n);
int    ioxd_pipe_write  (ioxd_pipe *pipe, const void *data, size_t n);
int    ioxd_pipe_flush  (ioxd_pipe *pipe);
int    ioxd_pipe_send   (ioxd_pipe *pipe, const void *data, size_t n);

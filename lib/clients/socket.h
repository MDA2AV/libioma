/*
 * clients/socket.h - an outbound connection: a socket made and connected on the ring, then the same
 * conn_t and pipe an accepted connection gets, owned by the coroutine that asked for it rather
 * than by one of its own. The public face is ioxd/socket.h.
 */
#pragma once

#include <sys/socket.h>

#include "io/pipe.h"
#include "io/proactor.h"

/* Connect to sa from the calling coroutine, which parks on the connect: the pipe, its buffers
 * on the heap, or nullptr with errno-style reason in *err. */
struct ioxd_pipe *ioxd__socket_connect(proactor_t *p, const struct sockaddr *sa, socklen_t len, int *err);

/* Flush what the writer holds, give the reader's buffers back, close the socket, free the pipe. */
void ioxd__socket_close(struct ioxd_pipe *pipe);

/* ── socket.c: the notes ────────────────────────────────────────────────────────────────── */

/* ioxd__socket_connect:
 * Two one-shot ops, both awaited like a send: IORING_OP_SOCKET, into the registered file table
 * when the worker has one (IORING_FILE_INDEX_ALLOC, so the connection lives in a slot like an
 * accepted one and every later SQE says IOSQE_FIXED_FILE the same way - and without SOCK_CLOEXEC,
 * which the kernel refuses on a slot, since a slot is not a descriptor), then IORING_OP_CONNECT
 * on it. A failed connect closes the socket through the ring and reports why. The conn_t has no
 * listener - the one place that reads it through the connection guards for that - and its
 * multishot recv is armed before the pipe is handed over, so bytes the peer sends first are
 * not lost. The pipe and its two buffers are one heap block, freed at close: a handler's stack
 * is not the place for another 25 KB, and the pipe may outlive the frame that made it.
 */

/* ioxd_connect:
 * Host literals only - IPv4, then IPv6 - since a name lookup blocks and nothing on the ring
 * resolves names; a caller resolves ahead, or on a thread of its own. Off a worker there is no
 * ring to connect on: nullptr.
 */

/*
 * ioxd/socket.h - outbound connections: a TCP connection of your own, made on the ring from a
 * handler, read and written as a pipe.
 */
#pragma once

#include "ioxd/pipe.h"

/* Connect to host:port - an IPv4 or IPv6 literal; names are not resolved here, since a lookup
 * blocks and the ring does not do them, so resolve ahead - and get the connection as a pipe: the
 * same reads and writes a pipe handler has, and TCP_NODELAY set. The call parks the calling
 * coroutine while the kernel connects; the worker serves its other connections meanwhile. From
 * a handler, a middleware or a pipe handler; off a worker, or when the peer refuses, NULL with
 * errno set (EINVAL for a host that is not a literal). The pipe belongs to the caller until
 * ioxd_disconnect, and must not be used by another connection's coroutine. */
ioxd_pipe *ioxd_connect(const char *host, int port);

/* Send what the writer still holds, close the connection and free the pipe. NULL is a no-op. */
void ioxd_disconnect(ioxd_pipe *pipe);

/*
 * tls/tls.h - what the runner needs from the TLS plane: the prologue a TLS connection runs before
 * its handler. The store itself is the public ioxd/tls.h.
 */
#pragma once

#include "ioxd/tls.h"
#include "io/pipe.h"

/* The handshake over the pipe, then the keys into the socket. 0, or -1: the connection is not
 * usable (the handshake failed, the peer left, kernel TLS is unavailable). */
int ioxd__tls_prologue(struct ioxd_pipe *pipe, ioxd_tls *tls);

/* Tell the peer the connection is ending: a close_notify alert, sent as a TLS control record
 * through the kernel. Best effort, for a connection whose prologue succeeded. */
void ioxd__tls_close_notify(struct ioxd_pipe *pipe);

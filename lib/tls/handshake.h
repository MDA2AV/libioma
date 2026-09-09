/*
 * tls/handshake.h - the prologue a TLS connection runs before its handler, and the alert it sends
 * when it ends; for the runner. The keylog callback is here too: the store installs it on every
 * context it makes, and this is where the secrets it catches are wanted.
 */
#pragma once

#include "ioxd/tls.h"
#include "io/pipe.h"

/* The handshake over the pipe, then the keys into the socket. 0, or -1: the connection is not
 * usable (the handshake failed, the peer left, kernel TLS is unavailable). */
int ioxd__tls_prologue(struct ioxd_pipe *pipe, ioxd_certs *certs);

/* Tell the peer the connection is ending: a close_notify alert, sent as a TLS control record
 * through the kernel. Best effort, for a connection whose prologue succeeded. */
void ioxd__tls_close_notify(struct ioxd_pipe *pipe);

#if IOXD_TLS
#include <openssl/ssl.h>

void ioxd__tls_keylog(const SSL *ssl, const char *line);         /* catches the traffic secrets */
#endif

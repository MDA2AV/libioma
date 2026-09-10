/*
 * ioxd/quic.h - QUIC: a UDP port whose connections carry streams, each stream handed to the pipe
 * handler as a pipe of its own. TLS 1.3 comes from the same certificate store a TLS port uses,
 * run by ngtcp2 with OpenSSL; the transport lives in the library, since the kernel offers no
 * QUIC of its own.
 */
#pragma once

#include "ioxd/tls.h"

/* Bind a UDP port for QUIC, with a certificate store (ioxd/tls.h) and the application protocols
 * the port answers, most preferred first, ended by NULL - QUIC requires one, so a client offering
 * none of them is refused at the handshake. Every stream a peer opens is served by the handler of
 * ioxd_run_pipes (ioxd/run.h) as a pipe: what the peer sent on the stream is what the pipe reads,
 * what the handler writes goes back on it, and the handler returning ends the stream. A stream
 * the peer opened one-way reads but cannot be written. ioxd_run, the HTTP server, does not serve
 * a QUIC port yet - HTTP/3 is a layer that is not there - and refuses to start with one bound.
 * -1 if refused: a bad port, no store, no protocol, the table full, or a build without QUIC
 * (make QUIC=1 needs libngtcp2 with its OpenSSL backend, and OpenSSL 3.5 or newer). */
int ioxd_bind_quic(int port, ioxd_certs *certs, const char *const *alpn);

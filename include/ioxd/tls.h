/*
 * ioxd/tls.h - TLS 1.3, terminated in the kernel after an OpenSSL handshake (TLS.md): a store of
 * certificates to listen with.
 */
#pragma once

#include "ioxd/http.h"

/* A store from a directory: <dir>/<host>/cert.pem (the chain) and key.pem for each hostname,
 * `default` for no SNI or no match, `_.example.com` for *.example.com. `default` is required -
 * without it nothing can answer a name we do not have. NULL, with the reason on stderr, when
 * nothing loads or the build has no TLS. Then: ioxd_listen(port, store). */
ioxd_tls *ioxd_tls_new(const char *dir);

/* Read the directory again and switch to what it holds. A host that fails to load - unreadable,
 * mismatched, not valid yet, expired - keeps its old certificate, and the host answering for
 * unmatched SNI keeps answering. Safe while serving: handshakes in flight finish on the table
 * they started with, and reloads serialise against each other. 0, or -1 when nothing could be
 * loaded at all, in which case what was serving still is. */
int ioxd_tls_reload(ioxd_tls *tls);

/* Give the store back: its certificates and the store itself, once the last handshake holding a
 * table of it has finished. Not while a listener still uses it - every TLS connection takes a
 * reference through the store - so this is for a store that was never listened on, or for after
 * ioxd_run has returned. NULL is a no-op. */
void ioxd_tls_free(ioxd_tls *tls);

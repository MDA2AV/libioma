/*
 * ioxd/tls.h - TLS 1.3, terminated in the kernel after an OpenSSL handshake (TLS.md): a store of
 * certificates to listen with.
 */
#pragma once

#include "ioxd/http.h"

/* A store from a directory: <dir>/<host>/cert.pem (the chain) and key.pem for each hostname,
 * `default` for no SNI or no match, `_.example.com` for *.example.com. NULL, with the reason on
 * stderr, when nothing loads or the build has no TLS. Then: ioxd_listen(port, store). */
ioxd_tls *ioxd_tls_new(const char *dir);

/* Read the directory again and switch to what it holds. A host that fails to load keeps its old
 * certificate. Safe while serving: handshakes in flight finish on the table they started with.
 * 0, or -1 when nothing could be loaded at all. */
int ioxd_tls_reload(ioxd_tls *tls);

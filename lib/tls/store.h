/*
 * tls/store.h - the certificate store's entries for the handshake: the table serving now, held
 * by reference for as long as a handshake runs, the context a handshake starts on, and the
 * binding that lets its ClientHello pick a host from that same table. The public side of the
 * store - ioxd_tls_new, ioxd_tls_reload, ioxd_tls_free - is ioxd/tls.h.
 */
#pragma once

#include "ioxd/tls.h"

#if IOXD_TLS
#include <openssl/ssl.h>

struct table;
struct table *ioxd__tls_acquire (ioxd_tls *tls);                 /* the table serving now, referenced */
void          ioxd__tls_release (ioxd_tls *tls, struct table *t);
SSL_CTX      *ioxd__tls_fallback(const struct table *t);         /* the context a handshake starts on */
void          ioxd__tls_bind    (SSL *ssl, struct table *t);     /* the table its ClientHello picks a host from */
#endif

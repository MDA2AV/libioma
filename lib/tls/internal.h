/*
 * tls/internal.h - what the store and the handshake share: the table a handshake holds a
 * reference to, and the keylog callback the store installs on every context.
 */
#pragma once

#include "ioxd/tls.h"

/* The value of a hex digit, or -1. */
static inline int ioxd__hexdigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#if IOXD_TLS
#include <openssl/ssl.h>

struct table;
struct table *ioxd__tls_acquire (ioxd_tls *tls);                 /* store.c: the table serving now, referenced */
void          ioxd__tls_release (ioxd_tls *tls, struct table *t);
SSL_CTX      *ioxd__tls_fallback(const struct table *t);         /* the context a handshake starts on */
void          ioxd__tls_keylog  (const SSL *ssl, const char *line);   /* handshake.c: catches the traffic secrets */
#endif

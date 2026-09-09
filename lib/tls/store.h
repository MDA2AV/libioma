/*
 * tls/store.h - the certificate store's entries for the handshake: the table serving now, held
 * by reference for as long as a handshake runs, the context a handshake starts on, and the
 * binding that lets its ClientHello pick a host from that same table. The public side of the
 * store - ioxd_certs_load, ioxd_certs_reload, ioxd_certs_free - is ioxd/tls.h.
 */
#pragma once

#include "ioxd/tls.h"

#if IOXD_TLS
#include <openssl/ssl.h>

struct table;
struct table *ioxd__tls_acquire (ioxd_certs *certs);                 /* the table serving now, referenced */
void          ioxd__tls_release (ioxd_certs *certs, struct table *t);
SSL_CTX      *ioxd__tls_fallback(const struct table *t);         /* the context a handshake starts on */
void          ioxd__tls_bind    (SSL *ssl, struct table *t);     /* the table its ClientHello picks a host from */
#endif

/* ── store.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * tls/store.c - the certificate store: one SSL_CTX per host directory, pinned to what kernel
 * TLS can carry (TLS 1.3, TLS_AES_128_GCM_SHA256, no tickets), chosen by SNI at the
 * ClientHello. The table of hosts is reference-counted so a reload swaps it under handshakes
 * in flight.
 *
 * A published context is never written to again. A reload carries a host that failed to load
 * forward by sharing its old context, so a context can belong to more than one table and
 * cannot name one: the ClientHello callback is installed once, when the context is built, and
 * finds the table through the SSL its handshake holds a reference for.
 */

/* at file scope:
 *   - the store's own, plus one per handshake in flight  [int          refs;]
 *   - `default`: what answers when SNI matches nothing  [SSL_CTX     *fallback;]
 *   - around the table pointer and its refs  [pthread_mutex_t  lock;]
 *   - one reload at a time, over the whole of it  [pthread_mutex_t  reload;]
 *   - The table a handshake started on, hung on its SSL: where the ClientHello callback finds
 *     it.  [static int            table_ex;]
 *   - built without TLS  [#else]
 */

/* ioxd__tls_bind:
 * The prologue's SSL, told which table it started on. It holds a reference to that table for
 * as long as the SSL lives, so the ClientHello callback can read it back whenever the peer
 * gets round to sending one.
 */

/* ssl_error:
 * The outermost OpenSSL error, as text; the rest of the queue goes with it, since one left
 * behind is reported against the next call that fails.
 */

/* fold:
 * ASCII lowercase, and nothing else: folding every byte would match CR to `-` and DEL to `_`.
 */

/* host_eq:
 * Exact hostname compare, ASCII case-insensitive.
 */

/* lookup:
 * The context for a server name: exact, then the wildcard of its parent domain.
 *   - the root dot: `sni.test.` is `sni.test`  [if (len > 1 && name[len - 1] == '.')]
 *   - a.example.com -> _.example.com  [if (dot) {]
 */

/* on_client_hello:
 * The ClientHello: pick the certificate by the server name, when there is one we know. The
 * table comes from the SSL rather than from `arg`, which is always NULL - see the note at the
 * top.
 *   - no SNI: the default  [return SSL_CLIENT_HELLO_SUCCESS;]
 *   - ServerNameList: one host_name entry  [size_t list_len = (size_t)ext[0] << 8 | ext[1];]
 *   - not the single entry we read: the default  [return SSL_CLIENT_HELLO_SUCCESS;]
 */

/* context_for:
 * One host's context: pinned to what the kernel can carry, with the files it was given. The
 * callbacks go on here, once: nothing writes to a context after it is published.
 */

/* not_current:
 * Why the context's certificate cannot serve now, or NULL when it can: a certificate that is
 * not valid yet or has run out is a load failure like any other, so the old one stays.
 */

/* fallback_name:
 * The host whose context answers when SNI matches nothing.
 */

/* load:
 * Every host directory under dir into a new table; a host that fails keeps its context from
 * `old` when it had one there. NULL when no host loads, or when nothing can answer for SNI
 * that matches nothing: `default` is required, and only a reload may carry the previous one
 * over.
 *   - not a host directory  [continue;]
 *   - once per host, on every load  [if (ks.st_mode & ((unsigned)S_IRGRP |
 *     (unsigned)S_IROTH))]
 *   - it parsed, but it cannot serve now  [if (ctx && why) {]
 *   - keep what was serving  [if (old) {]
 *   - shared with the old table, written to by neither  [SSL_CTX_up_ref(ctx);]
 *   - no `default`: on a reload the host that was answering keeps doing so, if it is still
 *     here  [if (!t->fallback) {]
 */

/* ioxd_certs_free:
 *   - the store's own reference; the last one frees  [ioxd__tls_release(certs, certs->table);]
 */

/* ioxd__tls_acquire:
 * A reference to the table serving now; released after the handshake.
 */

/* ioxd_certs_reload:
 *   - one at a time, so the table it reads stays put  [pthread_mutex_lock(&certs->reload);]
 *   - and cannot be freed while load() reads it  [struct table *old   =
 *     ioxd__tls_acquire(certs);]
 *   - the store's own reference to the old table  [ioxd__tls_release(certs, old);]
 *   - the one this reload took  [ioxd__tls_release(certs, old);]
 */

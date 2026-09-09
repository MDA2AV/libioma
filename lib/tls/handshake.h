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
int ioxd__handshake_prologue(struct ioxd_pipe *pipe, ioxd_certs *certs);

/* Tell the peer the connection is ending: a close_notify alert, sent as a TLS control record
 * through the kernel. Best effort, for a connection whose prologue succeeded. */
void ioxd__handshake_close_notify(struct ioxd_pipe *pipe);

#if IOXD_TLS
#include <openssl/ssl.h>

void ioxd__handshake_keylog(const SSL *ssl, const char *line);         /* catches the traffic secrets */
#endif

/* ── handshake.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * tls/handshake.c - the prologue of a TLS connection: an OpenSSL handshake over the pipe, the
 * traffic secrets caught by the keylog callback, HKDF-Expand-Label into key and IV, and the
 * keys into the socket, so the kernel does every record from then on. The design is in the TLS
 * notes (notes/TLS.md, kept beside the repository rather than in it).
 */

/* at file scope:
 *   - SHA-256 suite: the traffic secrets  [#define SECRET_LEN     32U]
 *   - header, plaintext, tag and padding  [#define RECORD_MAX     (5 + 16384 + 256)]
 *   - early plaintext: what the reader can take  [#define PLAIN_MAX      IOXD_PIPE_GATHER]
 *   - install's own failure, apart from errnos  [#define KDF_FAILED     (-4096)]
 *   - The secrets of one handshake, found through the SSL's ex_data.  [struct secrets {]
 *   - server and client application traffic secrets  [unsigned char tx[SECRET_LEN],
 *     rx[SECRET_LEN];]
 *   - built without TLS  [#else]
 */

/* ssl_error_text:
 * The outermost OpenSSL error, as text; the queue is cleared, so nothing stale is read later.
 */

/* hexdigit:
 * The value of a hex digit, or -1.
 */

/* unhex:
 * 64 hex characters into 32 bytes.
 */

/* ioxd__handshake_keylog:
 * "SERVER_TRAFFIC_SECRET_0 <client random> <secret>" and its CLIENT twin: the two we need.
 *   - past the client random  [const char *secret = strchr(tag, ' ');]
 */

/* expand_label:
 * RFC 8446 HKDF-Expand-Label(secret, label, "", n).
 *   - empty context  [info[at++] = 0;]
 */

/* install:
 * One direction's keys into the socket, from its traffic secret and the record sequence.
 *   - the 12-byte nonce, as the kernel splits it  [memcpy(ci.salt, iv, 4);]
 */

/* flush_outbound:
 * Everything the write BIO holds, out through the pipe.
 */

/* record_len:
 * The length of the TLS record whose 5-byte header is at rec, header included.
 */

/* take_record:
 * One whole TLS record out of the reader into rec: 1 when taken, -1 on error. During the
 * handshake it waits for delivery. While draining it never waits: 0 when nothing delivered is
 * left, which means the socket is at a record boundary; the tail of a record cut by the pause
 * is fetched straight from the socket instead. OpenSSL gets exactly one record per feed, so
 * what a client sent after its Finished stays in the reader for the drain rather than
 * vanishing into the read BIO.
 *   - nothing more was delivered  [if (rc == 0) {]
 *   - the rest of a split record, from the socket  [return -1;]
 */

/* feed_inbound:
 * The next record the client sent, into the read BIO.
 */

/* drain_records:
 * After the handshake, before the kernel takes over receiving: the client may already have
 * sent application data, part of it delivered to us, part still in the socket. Kernel RX
 * starts at a record boundary in the socket, so with the multishot recv stopped this takes
 * whole records from what was delivered - fetching the tail of a split one straight from the
 * socket - through OpenSSL, keeping the plaintext aside. The count consumed is the RX record
 * sequence.
 *   - scratch for one record, past the plaintext  [unsigned char *rec     = plain +
 *     PLAIN_MAX;]
 *   - the socket is at a boundary  [return records;]
 *   - an alert, or a handshake message: nothing we can hand over  [if (rec[0] != 23) {]
 *   - close_notify: what came before it is still served  [if (err == SSL_ERROR_ZERO_RETURN)]
 *   - OpenSSL answered something: a KeyUpdate we cannot follow  [if (BIO_pending(wbio) > 0) {]
 */

/* ioxd__handshake_prologue:
 *   - set on every failure: logged once  [const char    *why = nullptr;]
 *   - early plaintext, then one record: off the coroutine's stack  [plain = malloc(PLAIN_MAX +
 *     RECORD_MAX);]
 *   - not the SSL's yet  [BIO_free(rbio);]
 *   - the SSL owns both from here  [SSL_set_bio(ssl, rbio, wbio);]
 *   - the table its ClientHello picks a host from  [ioxd__certs_bind(ssl, t);]
 *   - the handshake, as an ordinary await loop  [for (;;) {]
 *   - nothing more leaves the socket meanwhile  [if (ioxd__conn_recv_pause(c) < 0) {]
 *   - or the worker is draining: nothing to serve  [why = "input ended after the handshake";]
 *   - and its BIOs  [SSL_free(ssl);]
 */

/* ioxd__handshake_close_notify:
 *   - warning, close_notify  [unsigned char alert[2] = { 1, 0 };]
 *   - the alert record type  [*CMSG_DATA(cm) = 21;]
 */

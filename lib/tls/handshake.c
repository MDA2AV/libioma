/*
 * tls/handshake.c - the prologue of a TLS connection: an OpenSSL handshake over the pipe, the
 * traffic secrets caught by the keylog callback, HKDF-Expand-Label into key and IV, and the keys
 * into the socket, so the kernel does every record from then on. See TLS.md.
 */
#include "tls/tls.h"
#include "tls/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if IOXD_TLS

#include <linux/tls.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/ssl.h>
#include <pthread.h>

#define SECRET_LEN     32U                            /* SHA-256 suite: the traffic secrets       */
#define RECORD_MAX     (5 + 16384 + 256)              /* header, plaintext, tag and padding       */
#define PLAIN_MAX      65536                          /* what a client may send before we listen  */

/* The secrets of one handshake, found through the SSL's ex_data. */
struct secrets {
    unsigned char tx[SECRET_LEN], rx[SECRET_LEN];     /* server and client application traffic secrets */
    bool          have_tx, have_rx;
};

static int             ex_index;
static pthread_once_t  ex_once = PTHREAD_ONCE_INIT;
static void make_ex_index(void)
{
    ex_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
}

/* The last OpenSSL error, as text. */
static const char *ssl_error_text(void)
{
    static thread_local char text[256];
    ERR_error_string_n(ERR_get_error(), text, sizeof text);
    return text;
}

/* 64 hex characters into 32 bytes. */
static bool unhex(const char *hex, unsigned char *out)
{
    for (size_t i = 0; i < SECRET_LEN; i++) {
        int hi = ioxd__hexdigit(hex[2 * i]), lo = ioxd__hexdigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (unsigned char)((unsigned)hi << 4 | (unsigned)lo);
    }
    return true;
}

/* "SERVER_TRAFFIC_SECRET_0 <client random> <secret>" and its CLIENT twin: the two we need. */
void ioxd__tls_keylog(const SSL *ssl, const char *line)
{
    struct secrets *s = SSL_get_ex_data(ssl, ex_index);
    if (!s)
        return;
    const char *tag = nullptr;
    unsigned char *into = nullptr;
    bool *have = nullptr;
    if (strncmp(line, "SERVER_TRAFFIC_SECRET_0 ", 24) == 0) { tag = line + 24; into = s->tx; have = &s->have_tx; }
    if (strncmp(line, "CLIENT_TRAFFIC_SECRET_0 ", 24) == 0) { tag = line + 24; into = s->rx; have = &s->have_rx; }
    if (!tag)
        return;
    const char *secret = strchr(tag, ' ');            /* past the client random */
    if (secret && strlen(secret + 1) >= (size_t)2 * SECRET_LEN && unhex(secret + 1, into))
        *have = true;
}

/* RFC 8446 HKDF-Expand-Label(secret, label, "", n). */
static bool expand_label(const unsigned char *secret, const char *label, unsigned char *out, size_t n)
{
    unsigned char info[64];
    size_t label_len = 6 + strlen(label), at = 0;
    info[at++] = (unsigned char)(n >> 8);
    info[at++] = (unsigned char)n;
    info[at++] = (unsigned char)label_len;
    memcpy(info + at, "tls13 ", 6);
    at += 6;
    memcpy(info + at, label, strlen(label));
    at += strlen(label);
    info[at++] = 0;                                   /* empty context */
    EVP_PKEY_CTX *k = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!k)
        return false;
    size_t len = n;
    bool ok = EVP_PKEY_derive_init(k) == 1
           && EVP_PKEY_CTX_hkdf_mode(k, EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) == 1
           && EVP_PKEY_CTX_set_hkdf_md(k, EVP_sha256()) == 1
           && EVP_PKEY_CTX_set1_hkdf_key(k, secret, SECRET_LEN) == 1
           && EVP_PKEY_CTX_add1_hkdf_info(k, info, (int)at) == 1
           && EVP_PKEY_derive(k, out, &len) == 1 && len == n;
    EVP_PKEY_CTX_free(k);
    return ok;
}

/* One direction's keys into the socket, from its traffic secret and the record sequence. */
static int install(conn_t *c, int direction, const unsigned char *secret, uint64_t seq)
{
    unsigned char key[16], iv[12];
    if (!expand_label(secret, "key", key, sizeof key) || !expand_label(secret, "iv", iv, sizeof iv))
        return -1;
    struct tls12_crypto_info_aes_gcm_128 ci = {};
    ci.info.version     = TLS_1_3_VERSION;
    ci.info.cipher_type = TLS_CIPHER_AES_GCM_128;
    memcpy(ci.key, key, sizeof key);
    memcpy(ci.salt, iv, 4);                           /* the 12-byte nonce, as the kernel splits it */
    memcpy(ci.iv, iv + 4, 8);
    for (unsigned i = 0; i < 8; i++)
        ci.rec_seq[i] = (unsigned char)(seq >> (56U - 8U * i));
    int rc = ioxd__setsockopt(c, SOL_TLS, direction, &ci, sizeof ci);
    explicit_bzero(&ci, sizeof ci);
    explicit_bzero(key, sizeof key);
    explicit_bzero(iv, sizeof iv);
    return rc;
}

/* Everything the write BIO holds, out through the pipe. */
static int flush_outbound(struct ioxd_pipe *pipe, BIO *wbio)
{
    char buf[4096];
    int  n;
    while ((n = BIO_read(wbio, buf, sizeof buf)) > 0)
        if (ioxd_pipewriter_write(&pipe->out, buf, (size_t)n) < 0)
            return -1;
    return ioxd_pipewriter_flush(&pipe->out);
}

/* Whatever the pipe has, or the next thing it gets, into the read BIO. */
static int feed_inbound(struct ioxd_pipe *pipe, BIO *rbio)
{
    ioxd_slice live = { nullptr, 0 };
    int rc = ioxd_pipereader_read(&pipe->in, &live);
    if (rc <= 0)
        return -1;
    BIO_write(rbio, live.p, (int)live.len);
    ioxd_pipereader_drop(&pipe->in, live.len);
    return 0;
}

/* The length of the TLS record at the front of live, header included. */
static size_t record_len(ioxd_slice live)
{
    return 5 + ((size_t)(unsigned char)live.p[3] << 8 | (unsigned char)live.p[4]);
}

/* After the handshake, before the kernel takes over receiving: the client may already have sent
 * application data, part of it delivered to us, part still in the socket. Kernel RX starts at a
 * record boundary in the socket, so with the multishot recv stopped this takes whole records
 * from what was delivered - fetching the tail of a split one straight from the socket - through
 * OpenSSL, keeping the plaintext aside. The count consumed is the RX record sequence. */
static long drain_records(struct ioxd_pipe *pipe, SSL *ssl, BIO *rbio, unsigned char *plain, size_t *plain_len)
{
    ioxd_pipereader *pr = &pipe->in;
    conn_t          *c  = pr->conn;
    unsigned char    tail[RECORD_MAX];
    long             records = 0;
    for (;;) {
        ioxd_slice live = { nullptr, 0 };
        if (!ioxd_pipereader_avail(pr, &live))
            break;                                    /* nothing delivered is left: the socket is at a boundary */
        while (live.len < 5 || live.len < record_len(live)) {
            size_t need = live.len < 5 ? 5 - live.len : record_len(live) - live.len;
            if (live.len >= 5 && record_len(live) > RECORD_MAX)
                return -1;
            ioxd_pipereader_examine(pr, live.len);
            if (ioxd_pipereader_avail(pr, &live))
                continue;                             /* the next delivered buffer joined it */
            if (ioxd__recv_exact(c, tail, need) < 0 || !ioxd_pipereader_inject(pr, tail, need))
                return -1;                            /* the rest was still in the socket */
            if (!ioxd_pipereader_avail(pr, &live))
                return -1;
        }
        size_t len = record_len(live);
        if ((unsigned char)live.p[0] != 23)           /* not application data: an alert; give up */
            return -1;
        BIO_write(rbio, live.p, (int)len);
        ioxd_pipereader_drop(pr, len);
        records++;
        int n;
        while ((n = SSL_read(ssl, plain + *plain_len, (int)(PLAIN_MAX - *plain_len))) > 0)
            *plain_len += (size_t)n;
        if (SSL_get_error(ssl, n) != SSL_ERROR_WANT_READ)
            return -1;                                /* close_notify, or a record we cannot take */
    }
    return records;
}

int ioxd__tls_prologue(struct ioxd_pipe *pipe, ioxd_tls *tls)
{
    pthread_once(&ex_once, make_ex_index);
    conn_t        *c = pipe->in.conn;
    struct table  *t = ioxd__tls_acquire(tls);
    struct secrets s = {};
    unsigned char *plain = nullptr;
    const char    *why = nullptr;                     /* set on every failure: logged once */
    int            r;

    SSL *ssl  = SSL_new(ioxd__tls_fallback(t));
    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    if (!ssl || !rbio || !wbio) {
        why = "out of memory";
        goto out;
    }
    SSL_set_bio(ssl, rbio, wbio);                     /* the SSL owns both from here */
    SSL_set_accept_state(ssl);
    SSL_set_ex_data(ssl, ex_index, &s);

    for (;;) {                                        /* the handshake, as an ordinary await loop */
        int ret = SSL_do_handshake(ssl);
        int err = ret == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl, ret);
        if (flush_outbound(pipe, wbio) < 0) {
            why = "peer gone while sending the handshake";
            goto out;
        }
        if (ret == 1)
            break;
        if (err != SSL_ERROR_WANT_READ) {
            why = ssl_error_text();
            goto out;
        }
        if (feed_inbound(pipe, rbio) < 0) {
            why = "peer gone during the handshake";
            goto out;
        }
    }
    if (!s.have_tx || !s.have_rx) {
        why = "no traffic secrets from the keylog callback";
        goto out;
    }

    if (ioxd__recv_pause(c) < 0) {                    /* nothing more leaves the socket meanwhile */
        why = "input ended after the handshake";
        goto out;
    }
    plain = malloc(PLAIN_MAX);
    size_t plain_len = 0;
    long records = plain ? drain_records(pipe, ssl, rbio, plain, &plain_len) : -1;
    if (records < 0) {
        why = "early application data could not be taken";
        goto out;
    }

    r = ioxd__setsockopt(c, SOL_TCP, TCP_ULP, "tls", sizeof "tls");
    if (r < 0) {
        why = r == -ENOENT ? "kernel TLS unavailable: is the tls module loaded?" : strerror(-r);
        goto out;
    }
    r = install(c, TLS_TX, s.tx, 0);
    if (r == 0)
        r = install(c, TLS_RX, s.rx, (uint64_t)records);
    if (r < 0) {
        why = r == -1 ? "key derivation failed" : strerror(-r);
        goto out;
    }
    if (plain_len && !ioxd_pipereader_inject(&pipe->in, plain, plain_len)) {
        why = "early application data does not fit";
        goto out;
    }
    ioxd__recv_resume(c);
out:
    if (why)
        fprintf(stderr, "ioxd_tls: connection dropped: %s\n", why);
    explicit_bzero(&s, sizeof s);
    if (plain) {
        explicit_bzero(plain, PLAIN_MAX);
        free(plain);
    }
    if (ssl) {
        SSL_free(ssl);
    } else {
        BIO_free(rbio);
        BIO_free(wbio);
    }
    ioxd__tls_release(tls, t);
    return why ? -1 : 0;
}

#else /* built without TLS */

int ioxd__tls_prologue(struct ioxd_pipe *pipe, ioxd_tls *tls)
{
    (void)pipe;
    (void)tls;
    return -1;
}

#endif

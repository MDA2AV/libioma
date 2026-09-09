#include "tls/handshake.h"
#include "tls/store.h"
#include "io/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if IOXD_TLS

#include <linux/tls.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/ssl.h>
#include <pthread.h>

#define SECRET_LEN     32U
#define RECORD_MAX     (5 + 16384 + 256)
#define PLAIN_MAX      IOXD_PIPE_GATHER
#define KDF_FAILED     (-4096)

struct secrets {
    unsigned char tx[SECRET_LEN], rx[SECRET_LEN];
    bool          have_tx, have_rx;
};

static int             ex_index;
static pthread_once_t  ex_once = PTHREAD_ONCE_INIT;
static void make_ex_index(void)
{
    ex_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
}

static const char *ssl_error_text(void)
{
    static thread_local char text[256];
    ERR_error_string_n(ERR_peek_last_error(), text, sizeof text);
    ERR_clear_error();
    return text;
}

static int hexdigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex(const char *hex, unsigned char *out)
{
    for (size_t i = 0; i < SECRET_LEN; i++) {
        int hi = hexdigit(hex[2 * i]), lo = hexdigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (unsigned char)((unsigned)hi << 4 | (unsigned)lo);
    }
    return true;
}

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
    const char *secret = strchr(tag, ' ');
    if (secret && strlen(secret + 1) >= (size_t)2 * SECRET_LEN && unhex(secret + 1, into))
        *have = true;
}

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
    info[at++] = 0;
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

static int install(conn_t *c, int direction, const unsigned char *secret, uint64_t seq)
{
    unsigned char key[16], iv[12];
    if (!expand_label(secret, "key", key, sizeof key) || !expand_label(secret, "iv", iv, sizeof iv))
        return KDF_FAILED;
    struct tls12_crypto_info_aes_gcm_128 ci = {};
    ci.info.version     = TLS_1_3_VERSION;
    ci.info.cipher_type = TLS_CIPHER_AES_GCM_128;
    memcpy(ci.key, key, sizeof key);
    memcpy(ci.salt, iv, 4);
    memcpy(ci.iv, iv + 4, 8);
    for (unsigned i = 0; i < 8; i++)
        ci.rec_seq[i] = (unsigned char)(seq >> (56U - 8U * i));
    int rc = ioxd__setsockopt(c, SOL_TLS, direction, &ci, sizeof ci);
    explicit_bzero(&ci, sizeof ci);
    explicit_bzero(key, sizeof key);
    explicit_bzero(iv, sizeof iv);
    return rc;
}

static int flush_outbound(struct ioxd_pipe *pipe, BIO *wbio)
{
    char buf[4096];
    int  n;
    while ((n = BIO_read(wbio, buf, sizeof buf)) > 0)
        if (ioxd_pipewriter_write(&pipe->out, buf, (size_t)n) < 0)
            return -1;
    return ioxd_pipewriter_flush(&pipe->out);
}

static size_t record_len(const unsigned char *rec)
{
    return 5 + ((size_t)rec[3] << 8 | rec[4]);
}

static int take_record(struct ioxd_pipe *pipe, bool draining, unsigned char *rec, size_t *len)
{
    ioxd_pipereader *pr = &pipe->in;
    size_t have = 0, need = 5;
    while (have < need) {
        ioxd_slice live = { nullptr, 0 };
        int rc = draining ? ioxd_pipereader_avail(pr, &live) : ioxd_pipereader_read(pr, &live);
        if (rc < 0 || (rc == 0 && !draining))
            return -1;
        if (rc == 0) {
            if (have == 0)
                return 0;
            if (ioxd__recv_exact(pr->conn, rec + have, need - have) < 0)
                return -1;
            have = need;
        } else {
            size_t k = live.len < need - have ? live.len : need - have;
            memcpy(rec + have, live.p, k);
            ioxd_pipereader_drop(pr, k);
            have += k;
        }
        if (have == 5 && need == 5) {
            need = record_len(rec);
            if (need > RECORD_MAX)
                return -1;
        }
    }
    *len = have;
    return 1;
}

static int feed_inbound(struct ioxd_pipe *pipe, BIO *rbio, unsigned char *rec)
{
    size_t len;
    if (take_record(pipe, false, rec, &len) <= 0)
        return -1;
    return BIO_write(rbio, rec, (int)len) == (int)len ? 0 : -1;
}

static long drain_records(struct ioxd_pipe *pipe, SSL *ssl, BIO *rbio, BIO *wbio, unsigned char *plain,
                          size_t *plain_len, const char **why)
{
    unsigned char *rec     = plain + PLAIN_MAX;
    long           records = 0;
    for (;;) {
        size_t len;
        int    rc = take_record(pipe, true, rec, &len);
        if (rc < 0) {
            *why = "early application data could not be taken";
            return -1;
        }
        if (rc == 0)
            return records;
        if (rec[0] != 23) {
            *why = "a record other than application data after the handshake";
            return -1;
        }
        if (*plain_len == PLAIN_MAX) {
            *why = "more early application data than the reader can hold";
            return -1;
        }
        if (BIO_write(rbio, rec, (int)len) != (int)len) {
            *why = "out of memory";
            return -1;
        }
        records++;
        int n = -1;
        while (*plain_len < PLAIN_MAX && (n = SSL_read(ssl, plain + *plain_len, (int)(PLAIN_MAX - *plain_len))) > 0)
            *plain_len += (size_t)n;
        if (*plain_len == PLAIN_MAX && SSL_pending(ssl) > 0) {
            *why = "more early application data than the reader can hold";
            return -1;
        }
        if (*plain_len == PLAIN_MAX)
            continue;
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN)
            return records;
        if (err != SSL_ERROR_WANT_READ) {
            *why = "a record OpenSSL could not take after the handshake";
            return -1;
        }
        if (BIO_pending(wbio) > 0) {
            *why = "a post-handshake message from the client (key update?)";
            return -1;
        }
    }
}

int ioxd__tls_prologue(struct ioxd_pipe *pipe, ioxd_certs *certs)
{
    pthread_once(&ex_once, make_ex_index);
    conn_t        *c = pipe->in.conn;
    struct table  *t = ioxd__tls_acquire(certs);
    struct secrets s = {};
    unsigned char *plain = nullptr;
    const char    *why = nullptr;
    int            r;

    SSL *ssl  = SSL_new(ioxd__tls_fallback(t));
    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    plain = malloc(PLAIN_MAX + RECORD_MAX);
    if (!ssl || !rbio || !wbio || !plain) {
        BIO_free(rbio);
        BIO_free(wbio);
        why = "out of memory";
        goto out;
    }
    SSL_set_bio(ssl, rbio, wbio);
    SSL_set_accept_state(ssl);
    SSL_set_ex_data(ssl, ex_index, &s);
    ioxd__tls_bind(ssl, t);

    for (;;) {
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
        if (feed_inbound(pipe, rbio, plain + PLAIN_MAX) < 0) {
            why = "peer gone during the handshake";
            goto out;
        }
    }
    if (!s.have_tx || !s.have_rx) {
        why = "no traffic secrets from the keylog callback";
        goto out;
    }

    if (ioxd__recv_pause(c) < 0) {
        why = "input ended after the handshake";
        goto out;
    }
    size_t plain_len = 0;
    long records = drain_records(pipe, ssl, rbio, wbio, plain, &plain_len, &why);
    if (records < 0)
        goto out;

    r = ioxd__setsockopt(c, SOL_TCP, TCP_ULP, "tls", sizeof "tls");
    if (r < 0) {
        why = r == -ENOENT ? "kernel TLS unavailable: is the tls module loaded?" : ioxd__errstr(-r);
        goto out;
    }
    r = install(c, TLS_TX, s.tx, 0);
    if (r == 0)
        r = install(c, TLS_RX, s.rx, (uint64_t)records);
    if (r < 0) {
        why = r == KDF_FAILED ? "key derivation failed" : ioxd__errstr(-r);
        goto out;
    }
    if (plain_len && !ioxd_pipereader_inject(&pipe->in, plain, plain_len)) {
        why = "early application data does not fit";
        goto out;
    }
    if (!ioxd__recv_resume(c))
        why = "input ended after the handshake";
out:
    if (why)
        fprintf(stderr, "ioxd_certs: connection dropped: %s\n", why);
    explicit_bzero(&s, sizeof s);
    if (plain) {
        explicit_bzero(plain, PLAIN_MAX + RECORD_MAX);
        free(plain);
    }
    SSL_free(ssl);
    ioxd__tls_release(certs, t);
    return why ? -1 : 0;
}

void ioxd__tls_close_notify(struct ioxd_pipe *pipe)
{
    unsigned char alert[2] = { 1, 0 };
    struct iovec  iov      = { alert, sizeof alert };
    union {
        char           buf[CMSG_SPACE(sizeof(unsigned char))];
        struct cmsghdr align;
    } ctl = {};
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf, .msg_controllen = sizeof ctl.buf };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_TLS;
    cm->cmsg_type  = TLS_SET_RECORD_TYPE;
    cm->cmsg_len   = CMSG_LEN(sizeof(unsigned char));
    *CMSG_DATA(cm) = 21;
    ioxd__sendmsg(pipe->in.conn, &msg);
}

#else

int ioxd__tls_prologue(struct ioxd_pipe *pipe, ioxd_certs *certs)
{
    (void)pipe;
    (void)certs;
    return -1;
}

void ioxd__tls_close_notify(struct ioxd_pipe *pipe)
{
    (void)pipe;
}

#endif

#include "tls/store.h"
#include "tls/handshake.h"
#include "io/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if IOXD_TLS

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <sys/stat.h>

struct host {
    char    *name;
    SSL_CTX *ctx;
};

struct table {
    int          refs;
    struct host *hosts;
    int          n;
    SSL_CTX     *fallback;
};

struct ioxd_certs {
    char            *dir;
    struct table    *table;
    pthread_mutex_t  lock;
    pthread_mutex_t  reload;
};

static int            table_ex;
static pthread_once_t table_ex_once = PTHREAD_ONCE_INIT;
static void make_table_ex(void)
{
    table_ex = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
}

void ioxd__tls_bind(SSL *ssl, struct table *t)
{
    pthread_once(&table_ex_once, make_table_ex);
    SSL_set_ex_data(ssl, table_ex, t);
}

static const char *ssl_error(void)
{
    static thread_local char text[256];
    ERR_error_string_n(ERR_peek_last_error(), text, sizeof text);
    ERR_clear_error();
    return text;
}

static unsigned char fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c | 0x20U) : c;
}

static bool host_eq(const char *a, size_t alen, const char *b)
{
    if (strlen(b) != alen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i]))
            return false;
    return true;
}

static SSL_CTX *lookup(const struct table *t, const char *name, size_t len)
{
    if (len > 1 && name[len - 1] == '.')
        len--;
    for (int i = 0; i < t->n; i++)
        if (host_eq(name, len, t->hosts[i].name))
            return t->hosts[i].ctx;
    const char *dot = memchr(name, '.', len);
    if (dot) {
        char wild[256];
        size_t rest = len - (size_t)(dot - name);
        if (rest + 1 < sizeof wild) {
            wild[0] = '_';
            memcpy(wild + 1, dot, rest);
            for (int i = 0; i < t->n; i++)
                if (host_eq(wild, rest + 1, t->hosts[i].name))
                    return t->hosts[i].ctx;
        }
    }
    return nullptr;
}

static int on_client_hello(SSL *ssl, int *alert, void *arg)   /* NOLINT(readability-non-const-parameter): OpenSSL's signature */
{
    (void)alert;
    (void)arg;
    const struct table  *t = SSL_get_ex_data(ssl, table_ex);
    const unsigned char *ext;
    size_t               ext_len;
    if (!t)
        return SSL_CLIENT_HELLO_SUCCESS;
    if (!SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &ext, &ext_len) || ext_len < 5)
        return SSL_CLIENT_HELLO_SUCCESS;
    size_t list_len = (size_t)ext[0] << 8 | ext[1];
    size_t name_len = (size_t)ext[3] << 8 | ext[4];
    if (ext[2] != 0 || name_len + 3 != list_len || list_len + 2 != ext_len)
        return SSL_CLIENT_HELLO_SUCCESS;
    SSL_CTX *ctx = lookup(t, (const char *)ext + 5, name_len);
    if (ctx)
        SSL_set_SSL_CTX(ssl, ctx);
    return SSL_CLIENT_HELLO_SUCCESS;
}

static SSL_CTX *context_for(const char *cert, const char *key)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx)
        return nullptr;
    bool ok = SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION)
           && SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256")
           && SSL_CTX_set_num_tickets(ctx, 0) == 1
           && SSL_CTX_use_certificate_chain_file(ctx, cert) == 1
           && SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) == 1
           && SSL_CTX_check_private_key(ctx) == 1;
    if (!ok) {
        SSL_CTX_free(ctx);
        return nullptr;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_keylog_callback(ctx, ioxd__tls_keylog);
    SSL_CTX_set_client_hello_cb(ctx, on_client_hello, nullptr);
    return ctx;
}

static const char *not_current(const SSL_CTX *ctx)
{
    const X509 *x = SSL_CTX_get0_certificate(ctx);
    if (!x)
        return "no certificate in the chain";
    int before = X509_cmp_time(X509_get0_notBefore(x), nullptr);
    int after  = X509_cmp_time(X509_get0_notAfter(x), nullptr);
    if (before == 0 || after == 0)
        return "a validity period that does not parse";
    if (before > 0)
        return "not valid yet: notBefore is in the future";
    if (after < 0)
        return "expired: notAfter has passed";
    return nullptr;
}

static void table_free(struct table *t)
{
    for (int i = 0; i < t->n; i++) {
        SSL_CTX_free(t->hosts[i].ctx);
        free(t->hosts[i].name);
    }
    free(t->hosts);
    free(t);
}

static const char *fallback_name(const struct table *t)
{
    for (int i = 0; i < t->n; i++)
        if (t->hosts[i].ctx == t->fallback)
            return t->hosts[i].name;
    return nullptr;
}

static struct table *load(const char *dir, const struct table *old)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "ioxd_certs: %s: %s\n", dir, ioxd__errstr(errno));
        return nullptr;
    }
    struct table *t = calloc(1, sizeof *t);
    if (!t) {
        perror("ioxd_certs");
        closedir(d);
        return nullptr;
    }
    t->refs = 1;
    struct dirent *e;
    while ((e = readdir(d))) {   /* NOLINT(concurrency-mt-unsafe): one DIR per reload, reloads serialised */
        if (e->d_name[0] == '.')
            continue;
        char cert[PATH_MAX], key[PATH_MAX];
        int  cn = snprintf(cert, sizeof cert, "%s/%s/cert.pem", dir, e->d_name);
        int  kn = snprintf(key,  sizeof key,  "%s/%s/key.pem",  dir, e->d_name);
        if (cn < 0 || (size_t)cn >= sizeof cert || kn < 0 || (size_t)kn >= sizeof key) {
            fprintf(stderr, "ioxd_certs: %s: the path to its certificate does not fit\n", e->d_name);
            continue;
        }
        struct stat cs, ks;
        if (stat(cert, &cs) != 0 || !S_ISREG(cs.st_mode) || stat(key, &ks) != 0 || !S_ISREG(ks.st_mode))
            continue;
        if (ks.st_mode & ((unsigned)S_IRGRP | (unsigned)S_IROTH))
            fprintf(stderr, "ioxd_certs: %s: mode %03o, readable past its owner\n", key,
                    (unsigned)(ks.st_mode & 0777));
        SSL_CTX    *ctx = context_for(cert, key);
        const char *why = ctx ? not_current(ctx) : ssl_error();
        if (ctx && why) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (!ctx) {
            fprintf(stderr, "ioxd_certs: %s: %s (%s)\n", e->d_name, why, cert);
            if (old) {
                for (int i = 0; i < old->n; i++)
                    if (strcmp(old->hosts[i].name, e->d_name) == 0) {
                        ctx = old->hosts[i].ctx;
                        SSL_CTX_up_ref(ctx);
                    }
            }
            if (!ctx)
                continue;
        }
        struct host *grown = realloc(t->hosts, ((size_t)t->n + 1) * sizeof *grown);
        char        *name  = strdup(e->d_name);
        if (!grown || !name) {
            perror("ioxd_certs");
            abort();
        }
        t->hosts = grown;
        t->hosts[t->n++] = (struct host){ name, ctx };
        if (strcmp(e->d_name, "default") == 0)
            t->fallback = ctx;
    }
    closedir(d);
    if (t->n == 0) {
        fprintf(stderr, "ioxd_certs: %s: no <host>/cert.pem + key.pem loaded\n", dir);
        table_free(t);
        return nullptr;
    }
    if (!t->fallback) {
        const char *prev = old ? fallback_name(old) : nullptr;
        for (int i = 0; prev && !t->fallback && i < t->n; i++)
            if (strcmp(t->hosts[i].name, prev) == 0)
                t->fallback = t->hosts[i].ctx;
        if (!t->fallback) {
            fprintf(stderr, "ioxd_certs: %s: no `default` host (default/cert.pem + key.pem) to answer"
                            " when SNI matches nothing\n", dir);
            table_free(t);
            return nullptr;
        }
        fprintf(stderr, "ioxd_certs: no `default` host; %s still answers when SNI matches nothing\n", prev);
    }
    return t;
}

ioxd_certs *ioxd_certs_load(const char *dir)
{
    struct table *t = load(dir, nullptr);
    if (!t)
        return nullptr;
    ioxd_certs *certs = calloc(1, sizeof *certs);
    char     *own = strdup(dir);
    if (!certs || !own) {
        perror("ioxd_certs");
        table_free(t);
        free(own);
        free(certs);
        return nullptr;
    }
    certs->dir   = own;
    certs->table = t;
    pthread_mutex_init(&certs->lock, nullptr);
    pthread_mutex_init(&certs->reload, nullptr);
    fprintf(stderr, "ioxd_certs: %d host%s from %s\n", t->n, t->n == 1 ? "" : "s", dir);
    return certs;
}

void ioxd_certs_free(ioxd_certs *certs)
{
    if (!certs)
        return;
    ioxd__tls_release(certs, certs->table);
    pthread_mutex_destroy(&certs->reload);
    pthread_mutex_destroy(&certs->lock);
    free(certs->dir);
    free(certs);
}

struct table *ioxd__tls_acquire(ioxd_certs *certs)
{
    pthread_mutex_lock(&certs->lock);
    struct table *t = certs->table;
    t->refs++;
    pthread_mutex_unlock(&certs->lock);
    return t;
}

void ioxd__tls_release(ioxd_certs *certs, struct table *t)
{
    pthread_mutex_lock(&certs->lock);
    int left = --t->refs;
    pthread_mutex_unlock(&certs->lock);
    if (left == 0)
        table_free(t);
}

SSL_CTX *ioxd__tls_fallback(const struct table *t)
{
    return t->fallback;
}

int ioxd_certs_reload(ioxd_certs *certs)
{
    pthread_mutex_lock(&certs->reload);
    struct table *old   = ioxd__tls_acquire(certs);
    struct table *fresh = load(certs->dir, old);
    if (fresh) {
        pthread_mutex_lock(&certs->lock);
        certs->table = fresh;
        pthread_mutex_unlock(&certs->lock);
        fprintf(stderr, "ioxd_certs: reloaded %d host%s from %s\n", fresh->n, fresh->n == 1 ? "" : "s", certs->dir);
        ioxd__tls_release(certs, old);
    }
    ioxd__tls_release(certs, old);
    pthread_mutex_unlock(&certs->reload);
    return fresh ? 0 : -1;
}

#else

ioxd_certs *ioxd_certs_load(const char *dir)
{
    fprintf(stderr, "ioxd_certs: %s: this build has no TLS (make TLS=1 with libssl-dev)\n", dir);
    return nullptr;
}

void ioxd_certs_free(ioxd_certs *certs)
{
    (void)certs;
}

int ioxd_certs_reload(ioxd_certs *certs)
{
    (void)certs;
    return -1;
}

#endif

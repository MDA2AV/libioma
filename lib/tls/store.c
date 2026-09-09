/*
 * tls/store.c - the certificate store: one SSL_CTX per host directory, pinned to what kernel TLS
 * can carry (TLS 1.3, TLS_AES_128_GCM_SHA256, no tickets), chosen by SNI at the ClientHello.
 * The table of hosts is reference-counted so a reload swaps it under handshakes in flight.
 */
#include "tls/tls.h"
#include "tls/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if IOXD_TLS

#include <dirent.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <sys/stat.h>

struct host {
    char    *name;
    SSL_CTX *ctx;
};

struct table {
    int          refs;                                /* the store's own, plus one per handshake in flight */
    struct host *hosts;
    int          n;
    SSL_CTX     *fallback;                            /* `default`, or the first host */
};

struct ioxd_tls {
    char            *dir;
    struct table    *table;
    pthread_mutex_t  lock;                            /* around the table pointer and its refs */
};

/* The last OpenSSL error, as text. */
static const char *ssl_error(void)
{
    static thread_local char text[256];
    ERR_error_string_n(ERR_get_error(), text, sizeof text);
    return text;
}

/* Exact hostname compare, ASCII case-insensitive. */
static bool host_eq(const char *a, size_t alen, const char *b)
{
    if (strlen(b) != alen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if (((unsigned char)a[i] | 0x20U) != ((unsigned char)b[i] | 0x20U))
            return false;
    return true;
}

/* The context for a server name: exact, then the wildcard of its parent domain. */
static SSL_CTX *lookup(const struct table *t, const char *name, size_t len)
{
    for (int i = 0; i < t->n; i++)
        if (host_eq(name, len, t->hosts[i].name))
            return t->hosts[i].ctx;
    const char *dot = memchr(name, '.', len);
    if (dot) {                                        /* a.example.com -> _.example.com */
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

/* The ClientHello: pick the certificate by the server name, when there is one we know. */
static int on_client_hello(SSL *ssl, int *alert, void *arg)   /* NOLINT(readability-non-const-parameter): OpenSSL's signature */
{
    (void)alert;
    const struct table  *t = arg;
    const unsigned char *ext;
    size_t               ext_len;
    if (!SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &ext, &ext_len) || ext_len < 5)
        return SSL_CLIENT_HELLO_SUCCESS;              /* no SNI: the default */
    size_t list_len = (size_t)ext[0] << 8 | ext[1];   /* ServerNameList: one host_name entry */
    size_t name_len = (size_t)ext[3] << 8 | ext[4];
    if (list_len + 2 > ext_len || ext[2] != 0 || name_len + 5 > ext_len)
        return SSL_CLIENT_HELLO_SUCCESS;
    SSL_CTX *ctx = lookup(t, (const char *)ext + 5, name_len);
    if (ctx)
        SSL_set_SSL_CTX(ssl, ctx);
    return SSL_CLIENT_HELLO_SUCCESS;
}

/* One host's context: pinned to what the kernel can carry, with the files it was given. */
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
    return ctx;
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

/* Every host directory under dir into a new table; a host that fails keeps its context from
 * `old` when it had one there. NULL when no host loads. */
static struct table *load(const char *dir, const struct table *old)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "ioxd_tls: %s: %s\n", dir, strerror(errno));
        return nullptr;
    }
    struct table *t = calloc(1, sizeof *t);
    t->refs = 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char cert[PATH_MAX], key[PATH_MAX];
        snprintf(cert, sizeof cert, "%s/%s/cert.pem", dir, e->d_name);
        snprintf(key,  sizeof key,  "%s/%s/key.pem",  dir, e->d_name);
        struct stat st;
        if (stat(cert, &st) != 0)
            continue;                                 /* not a host directory */
        SSL_CTX *ctx = context_for(cert, key);
        if (!ctx) {
            fprintf(stderr, "ioxd_tls: %s: %s\n", cert, ssl_error());
            if (old) {                                /* keep what was serving */
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
        if (!grown) {
            perror("realloc");
            abort();
        }
        t->hosts = grown;
        t->hosts[t->n++] = (struct host){ strdup(e->d_name), ctx };
        if (strcmp(e->d_name, "default") == 0)
            t->fallback = ctx;
    }
    closedir(d);
    if (t->n == 0) {
        fprintf(stderr, "ioxd_tls: %s: no <host>/cert.pem + key.pem loaded\n", dir);
        table_free(t);
        return nullptr;
    }
    if (!t->fallback) {
        t->fallback = t->hosts[0].ctx;
        fprintf(stderr, "ioxd_tls: no `default` host; %s answers when SNI matches nothing\n", t->hosts[0].name);
    }
    for (int i = 0; i < t->n; i++)                    /* the ClientHello callback sees this table */
        SSL_CTX_set_client_hello_cb(t->hosts[i].ctx, on_client_hello, t);
    return t;
}

ioxd_tls *ioxd_tls_new(const char *dir)
{
    struct table *t = load(dir, nullptr);
    if (!t)
        return nullptr;
    ioxd_tls *tls = calloc(1, sizeof *tls);
    tls->dir   = strdup(dir);
    tls->table = t;
    pthread_mutex_init(&tls->lock, nullptr);
    fprintf(stderr, "ioxd_tls: %d host%s from %s\n", t->n, t->n == 1 ? "" : "s", dir);
    return tls;
}

/* A reference to the table serving now; released after the handshake. */
struct table *ioxd__tls_acquire(ioxd_tls *tls)
{
    pthread_mutex_lock(&tls->lock);
    struct table *t = tls->table;
    t->refs++;
    pthread_mutex_unlock(&tls->lock);
    return t;
}

void ioxd__tls_release(ioxd_tls *tls, struct table *t)
{
    pthread_mutex_lock(&tls->lock);
    int left = --t->refs;
    pthread_mutex_unlock(&tls->lock);
    if (left == 0)
        table_free(t);
}

SSL_CTX *ioxd__tls_fallback(const struct table *t)
{
    return t->fallback;
}

int ioxd_tls_reload(ioxd_tls *tls)
{
    struct table *fresh = load(tls->dir, tls->table);
    if (!fresh)
        return -1;
    pthread_mutex_lock(&tls->lock);
    struct table *old = tls->table;
    tls->table = fresh;
    pthread_mutex_unlock(&tls->lock);
    ioxd__tls_release(tls, old);                      /* the store's own reference to the old table */
    fprintf(stderr, "ioxd_tls: reloaded %d host%s from %s\n", fresh->n, fresh->n == 1 ? "" : "s", tls->dir);
    return 0;
}

#else /* built without TLS */

ioxd_tls *ioxd_tls_new(const char *dir)
{
    fprintf(stderr, "ioxd_tls: %s: this build has no TLS (make TLS=1 with libssl-dev)\n", dir);
    return nullptr;
}

int ioxd_tls_reload(ioxd_tls *tls)
{
    (void)tls;
    return -1;
}

#endif

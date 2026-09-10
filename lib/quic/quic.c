#include "quic/quic.h"

#if IOXD_QUIC

#include "io/bufring.h"
#include "io/coro.h"
#include "tls/certs.h"

#include <errno.h>
#include <netinet/udp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IDLE_TIMEOUT      (30 * NGTCP2_SECONDS)
#define HANDSHAKE_TIMEOUT (10 * NGTCP2_SECONDS)
#define CLOSE_REPEAT      8
#define RESETS_PER_SECOND 64
#define IDLE_SENDS        64
#define STREAM_WINDOW     (256UL * 1024)
#define CONN_WINDOW       (1024UL * 1024)
#define MAX_BIDI_STREAMS  100
#define MAX_UNI_STREAMS   3

ngtcp2_tstamp ioxd__quic_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * NGTCP2_SECONDS + (ngtcp2_tstamp)ts.tv_nsec;
}

static pthread_once_t crypto_once = PTHREAD_ONCE_INIT;
static void crypto_init(void)
{
    ngtcp2_crypto_ossl_init();
}

static void note(struct quic_listener *ql, const char *what, int err)
{
    fprintf(stderr, "[w%d] quic :%u: %s: %s\n", ql->p->id, ql->l->port, what,
            err > 0 ? ioxd__io_errstr(err) : ngtcp2_strerror(err));
}

/* ── the connection id table ───────────────────────────────────────────────────────────── */

static uint64_t cid_hash(const uint8_t *cid, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++)
        h = (h ^ cid[i]) * 1099511628211ULL;
    return h;
}

static struct cid_slot *table_slot(struct quic_listener *ql, const uint8_t *cid, size_t len, bool insert)
{
    struct cid_slot *tomb = nullptr;
    for (uint64_t i = cid_hash(cid, len);; i++) {
        struct cid_slot *s = &ql->table[i & ql->table_mask];
        if (s->len == 0) {
            if (!s->tomb)
                return insert ? (tomb ? tomb : s) : nullptr;
            if (!tomb)
                tomb = s;
            continue;
        }
        if (s->len == len && memcmp(s->cid, cid, len) == 0)
            return s;
    }
}

static bool table_grow(struct quic_listener *ql)
{
    unsigned         cap = (ql->table_mask + 1) * 2;
    struct cid_slot *old = ql->table, *fresh = calloc(cap, sizeof *fresh);
    unsigned         old_cap = ql->table_mask + 1;
    if (!fresh)
        return false;
    ql->table       = fresh;
    ql->table_mask  = cap - 1;
    ql->table_tombs = 0;
    for (unsigned i = 0; i < old_cap; i++)
        if (old[i].len)
            *table_slot(ql, old[i].cid, old[i].len, true) = old[i];
    free(old);
    return true;
}

static bool table_add(struct quic_listener *ql, const ngtcp2_cid *cid, struct quic_conn *qc)
{
    if ((ql->table_used + ql->table_tombs + 1) * 2 > ql->table_mask + 1 && !table_grow(ql))
        return false;
    struct cid_slot *s = table_slot(ql, cid->data, cid->datalen, true);
    if (s->len)
        return s->qc == qc;
    if (s->tomb)
        ql->table_tombs--;
    memcpy(s->cid, cid->data, cid->datalen);
    s->len  = (uint8_t)cid->datalen;
    s->tomb = false;
    s->qc   = qc;
    ql->table_used++;
    return true;
}

static void table_remove(struct quic_listener *ql, const ngtcp2_cid *cid)
{
    struct cid_slot *s = table_slot(ql, cid->data, cid->datalen, false);
    if (!s)
        return;
    s->len  = 0;
    s->tomb = true;
    s->qc   = nullptr;
    ql->table_used--;
    ql->table_tombs++;
}

static struct quic_conn *table_find(struct quic_listener *ql, const uint8_t *cid, size_t len)
{
    if (len == 0 || len > NGTCP2_MAX_CIDLEN)
        return nullptr;
    struct cid_slot *s = table_slot(ql, cid, len, false);
    return s ? s->qc : nullptr;
}

static bool cid_register(struct quic_conn *qc, const ngtcp2_cid *cid)
{
    if (qc->n_cids == sizeof qc->cids / sizeof qc->cids[0] || !table_add(qc->ql, cid, qc))
        return false;
    qc->cids[qc->n_cids++] = *cid;
    return true;
}

static void cid_forget(struct quic_conn *qc, const ngtcp2_cid *cid)
{
    table_remove(qc->ql, cid);
    for (unsigned i = 0; i < qc->n_cids; i++)
        if (ngtcp2_cid_eq(&qc->cids[i], cid)) {
            qc->cids[i] = qc->cids[--qc->n_cids];
            return;
        }
}

/* ── the sends ─────────────────────────────────────────────────────────────────────────── */

static struct quic_send *send_get(struct quic_listener *ql)
{
    struct quic_send *s = ql->free_sends;
    if (s) {
        ql->free_sends = s->next;
        ql->n_free--;
        return s;
    }
    s = malloc(sizeof *s);
    if (!s) {
        note(ql, "send", ENOMEM);
        return nullptr;
    }
    s->target.on_cqe = nullptr;
    s->ql            = ql;
    ql->n_sends++;
    return s;
}

static void send_put(struct quic_listener *ql, struct quic_send *s)
{
    if (ql->n_free >= IDLE_SENDS) {
        free(s);
        ql->n_sends--;
        return;
    }
    s->next        = ql->free_sends;
    ql->free_sends = s;
    ql->n_free++;
}

static void send_on_cqe(ioxd_cqe_target *t, int res, unsigned flags)
{
    (void)flags;
    struct quic_send     *s  = (struct quic_send *)t;
    struct quic_listener *ql = s->ql;
    if (res < 0 && res != -ECANCELED) {
        if ((res == -EMSGSIZE || res == -EINVAL || res == -EIO) && s->msg.msg_controllen && !ql->no_gso) {
            ql->no_gso = true;
            note(ql, "GSO refused, sending one datagram at a time", -res);
        } else if (res != -EMSGSIZE) {
            note(ql, "sendmsg", -res);
        }
    }
    send_put(ql, s);
}

static void send_submit(struct quic_listener *ql, struct quic_send *s, const struct sockaddr *to, socklen_t tolen,
                        size_t n, size_t gso)
{
    if (tolen > sizeof s->to)
        tolen = sizeof s->to;
    memcpy(&s->to, to, tolen);
    s->iov = (struct iovec){ s->buf, n };
    s->msg = (struct msghdr){ .msg_name = &s->to, .msg_namelen = tolen, .msg_iov = &s->iov, .msg_iovlen = 1 };
    if (gso && n > gso && !ql->no_gso) {
        s->msg.msg_control    = s->control;
        s->msg.msg_controllen = sizeof s->control;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&s->msg);
        cm->cmsg_level = SOL_UDP;
        cm->cmsg_type  = UDP_SEGMENT;
        cm->cmsg_len   = CMSG_LEN(sizeof(uint16_t));
        uint16_t seg = (uint16_t)gso;
        memcpy(CMSG_DATA(cm), &seg, sizeof seg);
    }
    s->target.on_cqe = send_on_cqe;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(ql->p);
    sqe->opcode    = IORING_OP_SENDMSG;
    sqe->fd        = ql->fd;
    sqe->addr      = (uintptr_t)&s->msg;
    sqe->len       = 1;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = UD(&s->target, TAG_CALL);
}

/* ── the timer: one kernel timeout at the earliest expiry ─────────────────────────────── */

static void heap_swap(struct quic_listener *ql, unsigned a, unsigned b)
{
    struct quic_conn *t = ql->heap[a];
    ql->heap[a] = ql->heap[b];
    ql->heap[b] = t;
    ql->heap[a]->heap_idx = a;
    ql->heap[b]->heap_idx = b;
}

static void heap_up(struct quic_listener *ql, unsigned i)
{
    while (i > 0) {
        unsigned parent = (i - 1) / 2;
        if (ql->heap[parent]->expiry <= ql->heap[i]->expiry)
            break;
        heap_swap(ql, i, parent);
        i = parent;
    }
}

static void heap_down(struct quic_listener *ql, unsigned i)
{
    for (;;) {
        unsigned l = 2 * i + 1, r = l + 1, m = i;
        if (l < ql->n_heap && ql->heap[l]->expiry < ql->heap[m]->expiry)
            m = l;
        if (r < ql->n_heap && ql->heap[r]->expiry < ql->heap[m]->expiry)
            m = r;
        if (m == i)
            return;
        heap_swap(ql, i, m);
        i = m;
    }
}

static void heap_update(struct quic_listener *ql, struct quic_conn *qc)
{
    if (qc->heap_idx == UINT_MAX) {
        if (ql->n_heap == ql->cap_heap) {
            unsigned           cap   = ql->cap_heap ? ql->cap_heap * 2 : 64;
            struct quic_conn **grown = realloc(ql->heap, cap * sizeof *grown);
            if (!grown) {
                note(ql, "timer heap", ENOMEM);
                abort();
            }
            ql->heap     = grown;
            ql->cap_heap = cap;
        }
        qc->heap_idx = ql->n_heap;
        ql->heap[ql->n_heap++] = qc;
        heap_up(ql, qc->heap_idx);
        return;
    }
    heap_up(ql, qc->heap_idx);
    heap_down(ql, qc->heap_idx);
}

static void heap_remove(struct quic_listener *ql, struct quic_conn *qc)
{
    unsigned i = qc->heap_idx;
    if (i == UINT_MAX)
        return;
    qc->heap_idx = UINT_MAX;
    ql->n_heap--;
    if (i == ql->n_heap)
        return;
    ql->heap[i] = ql->heap[ql->n_heap];
    ql->heap[i]->heap_idx = i;
    heap_up(ql, i);
    heap_down(ql, i);
}

static void timer_arm(struct quic_listener *ql, ngtcp2_tstamp deadline)
{
    struct quic_timer *t = &ql->timer;
    ngtcp2_tstamp now   = ioxd__quic_now();
    ngtcp2_tstamp delta = deadline > now ? deadline - now : 1;
    t->ts = (struct __kernel_timespec){ .tv_sec = (int64_t)(delta / NGTCP2_SECONDS), .tv_nsec = (int64_t)(delta % NGTCP2_SECONDS) };
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(ql->p);
    sqe->opcode    = IORING_OP_TIMEOUT;
    sqe->fd        = -1;
    sqe->addr      = (uintptr_t)&t->ts;
    sqe->len       = 1;
    sqe->user_data = UD(&t->target, TAG_CALL);
    t->armed    = true;
    t->deadline = deadline;
}

static void timer_cancel(struct quic_listener *ql)
{
    struct quic_timer *t = &ql->timer;
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(ql->p);
    sqe->opcode    = IORING_OP_TIMEOUT_REMOVE;
    sqe->fd        = -1;
    sqe->addr      = UD(&t->target, TAG_CALL);
    sqe->user_data = TAG_IGNORE;
    t->cancelling = true;
}

static void timer_update(struct quic_listener *ql)
{
    struct quic_timer *t    = &ql->timer;
    ngtcp2_tstamp      want = ql->n_heap ? ql->heap[0]->expiry : UINT64_MAX;
    if (!t->armed) {
        if (want != UINT64_MAX && !ql->p->draining)
            timer_arm(ql, want);
    } else if (want < t->deadline && !t->cancelling) {
        timer_cancel(ql);
    }
}

/* ── connections ───────────────────────────────────────────────────────────────────────── */

static void wake_streams(struct quic_listener *ql);
static void conn_free(struct quic_conn *qc);
static bool conn_close(struct quic_conn *qc);

static void conn_reschedule(struct quic_conn *qc)
{
    if (qc->state != QUIC_OPEN)
        return;
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(qc->conn);
    if (expiry == qc->expiry && qc->heap_idx != UINT_MAX)
        return;
    qc->expiry = expiry;
    heap_update(qc->ql, qc);
    timer_update(qc->ql);
}

static void send_close(struct quic_conn *qc)
{
    struct quic_send *s = send_get(qc->ql);
    if (!s)
        return;
    memcpy(s->buf, qc->close_pkt, qc->close_len);
    send_submit(qc->ql, s, (const struct sockaddr *)&qc->remote, qc->remote_len, qc->close_len, 0);
}

static void conn_leave(struct quic_conn *qc, enum quic_state state)
{
    qc->state  = state;
    qc->expiry = ioxd__quic_now() + 3 * ngtcp2_conn_get_pto(qc->conn);
    heap_update(qc->ql, qc);
    timer_update(qc->ql);
    ioxd__stream_abandon(qc);
}

static bool conn_close(struct quic_conn *qc)
{
    if (qc->state != QUIC_OPEN)
        return true;
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info     pi = {};
    ngtcp2_path_storage_zero(&ps);
    uint8_t *buf = malloc(NGTCP2_MAX_UDP_PAYLOAD_SIZE);
    ngtcp2_ssize n = buf ? ngtcp2_conn_write_connection_close(qc->conn, &ps.path, &pi, buf, NGTCP2_MAX_UDP_PAYLOAD_SIZE,
                                                              &qc->err, ioxd__quic_now())
                         : 0;
    if (n <= 0) {
        free(buf);
        conn_free(qc);
        return false;
    }
    qc->close_pkt = buf;
    qc->close_len = (size_t)n;
    send_close(qc);
    conn_leave(qc, QUIC_CLOSING);
    return true;
}

static void conn_error(struct quic_conn *qc, int rv)
{
    trace("[w%d] quic conn error %d (%s)\n", qc->ql->p->id, rv, ngtcp2_strerror(rv));
    switch (rv) {
    case NGTCP2_ERR_DRAINING:
        conn_leave(qc, QUIC_DRAINING);
        return;
    case NGTCP2_ERR_DROP_CONN:
    case NGTCP2_ERR_RETRY:
    case NGTCP2_ERR_IDLE_CLOSE:
    case NGTCP2_ERR_HANDSHAKE_TIMEOUT:
        conn_free(qc);
        return;
    case NGTCP2_ERR_CRYPTO:
        if (!qc->err.error_code)
            ngtcp2_ccerr_set_tls_alert(&qc->err, ngtcp2_conn_get_tls_alert(qc->conn), nullptr, 0);
        break;
    default:
        if (!qc->err.error_code)
            ngtcp2_ccerr_set_liberr(&qc->err, rv, nullptr, 0);
        break;
    }
    conn_close(qc);
}

void ioxd__quic_flush(struct quic_conn *qc)
{
    struct quic_listener *ql = qc->ql;
    if (qc->state != QUIC_OPEN)
        return;
    for (;;) {
        struct quic_send *s = send_get(ql);
        if (!s)
            return;
        ngtcp2_tstamp       now     = ioxd__quic_now();
        size_t              quantum = ngtcp2_conn_get_send_quantum(qc->conn);
        size_t              one     = ngtcp2_conn_get_max_tx_udp_payload_size(qc->conn);
        size_t              buflen  = quantum > one ? quantum : one;
        if (buflen > sizeof s->buf)
            buflen = sizeof s->buf;
        ngtcp2_path_storage ps;
        ngtcp2_pkt_info     pi  = {};
        size_t              gso = 0;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_ssize n = ngtcp2_conn_write_aggregate_pkt2(qc->conn, &ps.path, &pi, s->buf, buflen, &gso,
                                                          ioxd__stream_write_pkt, ql->no_gso ? 1 : 0, now);
        if (n < 0) {
            send_put(ql, s);
            conn_error(qc, (int)n);
            return;
        }
        ngtcp2_conn_update_pkt_tx_time(qc->conn, now);
        if (n == 0) {
            send_put(ql, s);
            break;
        }
        send_submit(ql, s, (const struct sockaddr *)ps.path.remote.addr, (socklen_t)ps.path.remote.addrlen, (size_t)n, gso);
    }
    conn_reschedule(qc);
}

static void conn_read(struct quic_conn *qc, const struct sockaddr *from, socklen_t fromlen, const uint8_t *pkt, size_t len)
{
    struct quic_listener *ql = qc->ql;
    ngtcp2_path path = {
        .local  = { (ngtcp2_sockaddr *)&ql->local, sizeof ql->local },
        .remote = { (ngtcp2_sockaddr *)from, fromlen },
    };
    ngtcp2_pkt_info pi = {};
    int rv = ngtcp2_conn_read_pkt(qc->conn, &path, &pi, pkt, len, ioxd__quic_now());
    if (rv != 0) {
        conn_error(qc, rv);
        return;
    }
    const ngtcp2_path *now = ngtcp2_conn_get_path(qc->conn);
    if (now->remote.addrlen && now->remote.addrlen <= sizeof qc->remote) {
        memcpy(&qc->remote, now->remote.addr, now->remote.addrlen);
        qc->remote_len = now->remote.addrlen;
    }
    ioxd__quic_flush(qc);
}

static ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *ref)
{
    return ((struct quic_conn *)ref->user_data)->conn;
}

static void cb_rand(uint8_t *dest, size_t n, const ngtcp2_rand_ctx *ctx)
{
    (void)ctx;
    if (RAND_bytes(dest, (int)n) != 1)
        abort();
}

static void stamp_worker(uint8_t *cid, const proactor_t *p)
{
    unsigned n = (unsigned)p->n_workers;
    if (n <= 1 || n > 256)
        return;
    unsigned v = (unsigned)cid[0] - (unsigned)cid[0] % n + (unsigned)p->id;
    if (v > 255)
        v -= n;
    cid[0] = (uint8_t)v;
}

static void mint_cid(struct quic_conn *qc, ngtcp2_cid *cid)
{
    cid->datalen = QUIC_CIDLEN;
    cb_rand(cid->data, QUIC_CIDLEN, nullptr);
    stamp_worker(cid->data, qc->ql->p);
}

static int cb_new_cid(ngtcp2_conn *conn, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token, size_t cidlen, void *user_data)
{
    (void)conn;
    (void)cidlen;
    struct quic_conn *qc = user_data;
    mint_cid(qc, cid);
    if (ngtcp2_crypto_generate_stateless_reset_token(token->data, qc->ql->secret, sizeof qc->ql->secret, cid) != 0
        || !cid_register(qc, cid))
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

static int cb_remove_cid(ngtcp2_conn *conn, const ngtcp2_cid *cid, void *user_data)
{
    (void)conn;
    cid_forget(user_data, cid);
    return 0;
}

static int cb_stream_open(ngtcp2_conn *conn, int64_t id, void *user_data)
{
    (void)conn;
    ioxd__stream_open(user_data, id);
    return 0;
}

static int cb_recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t id, uint64_t offset, const uint8_t *data,
                               size_t len, void *user_data, void *stream_user_data)
{
    (void)conn;
    (void)id;
    (void)offset;
    (void)user_data;
    if (stream_user_data)
        ioxd__stream_recv(stream_user_data, data, len, flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    return 0;
}

static int cb_acked(ngtcp2_conn *conn, int64_t id, uint64_t offset, uint64_t len, void *user_data, void *stream_user_data)
{
    (void)conn;
    (void)id;
    (void)user_data;
    if (stream_user_data)
        ioxd__stream_acked(stream_user_data, offset, len);
    return 0;
}

static int cb_stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t id, uint64_t rx_err, uint64_t tx_err,
                           void *user_data, void *stream_user_data)
{
    (void)flags;
    (void)rx_err;
    (void)tx_err;
    (void)user_data;
    if (stream_user_data)
        ioxd__stream_closed(stream_user_data);
    if (!ngtcp2_conn_is_local_stream(conn, id)) {
        if (ngtcp2_is_bidi_stream(id))
            ngtcp2_conn_extend_max_streams_bidi(conn, 1);
        else
            ngtcp2_conn_extend_max_streams_uni(conn, 1);
    }
    return 0;
}

static int cb_stream_reset(ngtcp2_conn *conn, int64_t id, uint64_t final_size, uint64_t err, void *user_data,
                           void *stream_user_data)
{
    (void)conn;
    (void)id;
    (void)final_size;
    (void)err;
    (void)user_data;
    if (stream_user_data)
        ioxd__stream_reset(stream_user_data);
    return 0;
}

static int cb_stop_sending(ngtcp2_conn *conn, int64_t id, uint64_t err, void *user_data, void *stream_user_data)
{
    (void)conn;
    (void)id;
    (void)user_data;
    if (stream_user_data)
        ioxd__stream_stop(stream_user_data, err);
    return 0;
}

static int cb_extend_max_stream_data(ngtcp2_conn *conn, int64_t id, uint64_t max_data, void *user_data,
                                     void *stream_user_data)
{
    (void)conn;
    (void)id;
    (void)max_data;
    (void)user_data;
    if (stream_user_data)
        ioxd__stream_unblock(stream_user_data);
    return 0;
}

static const ngtcp2_callbacks callbacks = {
    .recv_client_initial      = ngtcp2_crypto_recv_client_initial_cb,
    .recv_crypto_data         = ngtcp2_crypto_recv_crypto_data_cb,
    .encrypt                  = ngtcp2_crypto_encrypt_cb,
    .decrypt                  = ngtcp2_crypto_decrypt_cb,
    .hp_mask                  = ngtcp2_crypto_hp_mask_cb,
    .recv_stream_data         = cb_recv_stream_data,
    .acked_stream_data_offset = cb_acked,
    .stream_open              = cb_stream_open,
    .rand                     = cb_rand,
    .remove_connection_id     = cb_remove_cid,
    .update_key               = ngtcp2_crypto_update_key_cb,
    .stream_reset             = cb_stream_reset,
    .extend_max_stream_data   = cb_extend_max_stream_data,
    .delete_crypto_aead_ctx   = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .stream_stop_sending      = cb_stop_sending,
    .version_negotiation      = ngtcp2_crypto_version_negotiation_cb,
    .get_new_connection_id2   = cb_new_cid,
    .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
    .stream_close2            = cb_stream_close,
};

static void conn_free(struct quic_conn *qc)
{
    struct quic_listener *ql = qc->ql;
    heap_remove(ql, qc);
    for (unsigned i = 0; i < qc->n_cids; i++)
        table_remove(ql, &qc->cids[i]);
    qc->n_cids = 0;
    if (qc->prev)
        qc->prev->next = qc->next;
    else
        ql->conns = qc->next;
    if (qc->next)
        qc->next->prev = qc->prev;
    qc->next = qc->prev = nullptr;
    ql->n_conns--;
    ql->p->live--;
    if (qc->conn) {
        ngtcp2_conn_del(qc->conn);
        qc->conn = nullptr;
    }
    if (qc->ssl) {
        SSL_set_app_data(qc->ssl, nullptr);
        SSL_free(qc->ssl);
        qc->ssl = nullptr;
    }
    if (qc->ossl) {
        ngtcp2_crypto_ossl_ctx_del(qc->ossl);
        qc->ossl = nullptr;
    }
    if (qc->certs) {
        ioxd__certs_release(ql->l->certs, qc->certs);
        qc->certs = nullptr;
    }
    free(qc->close_pkt);
    qc->close_pkt = nullptr;
    qc->state = QUIC_DEAD;
    ioxd__stream_abandon(qc);
    timer_update(ql);
    if (qc->live_coros == 0)
        free(qc);
}

static void accept_conn(struct quic_listener *ql, const struct sockaddr *from, socklen_t fromlen, const uint8_t *pkt, size_t len)
{
    ngtcp2_pkt_hd hd;
    if (ngtcp2_accept(&hd, pkt, len) != 0 || ql->p->draining)
        return;
    struct quic_conn *qc = calloc(1, sizeof *qc);
    if (!qc) {
        note(ql, "accept", ENOMEM);
        return;
    }
    qc->ql       = ql;
    qc->heap_idx = UINT_MAX;
    qc->ref      = (ngtcp2_crypto_conn_ref){ .get_conn = get_conn, .user_data = qc };
    if (fromlen > sizeof qc->remote)
        fromlen = sizeof qc->remote;
    memcpy(&qc->remote, from, fromlen);
    qc->remote_len = fromlen;
    ngtcp2_ccerr_default(&qc->err);

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts        = ioxd__quic_now();
    settings.handshake_timeout = HANDSHAKE_TIMEOUT;

    ngtcp2_cid scid;
    mint_cid(qc, &scid);

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local  = STREAM_WINDOW;
    params.initial_max_stream_data_bidi_remote = STREAM_WINDOW;
    params.initial_max_stream_data_uni         = STREAM_WINDOW;
    params.initial_max_data                    = CONN_WINDOW;
    params.initial_max_streams_bidi            = MAX_BIDI_STREAMS;
    params.initial_max_streams_uni             = MAX_UNI_STREAMS;
    params.max_idle_timeout                    = IDLE_TIMEOUT;
    params.active_connection_id_limit          = 7;
    params.original_dcid                       = hd.dcid;
    params.original_dcid_present               = 1;
    params.stateless_reset_token_present       = 1;
    if (ngtcp2_crypto_generate_stateless_reset_token(params.stateless_reset_token, ql->secret, sizeof ql->secret, &scid) != 0) {
        free(qc);
        return;
    }

    ngtcp2_path path = {
        .local  = { (ngtcp2_sockaddr *)&ql->local, sizeof ql->local },
        .remote = { (ngtcp2_sockaddr *)&qc->remote, qc->remote_len },
    };
    int rv = ngtcp2_conn_server_new(&qc->conn, &hd.scid, &scid, &path, hd.version, &callbacks, &settings, &params, nullptr, qc);
    trace("[w%d] quic accept: server_new %d\n", ql->p->id, rv);
    if (rv != 0) {
        note(ql, "ngtcp2_conn_server_new", rv);
        free(qc);
        return;
    }

    qc->certs = ioxd__certs_acquire(ql->l->certs);
    qc->ssl   = SSL_new(ioxd__certs_fallback_quic(qc->certs));
    if (!qc->ssl || ngtcp2_crypto_ossl_ctx_new(&qc->ossl, qc->ssl) != 0
        || ngtcp2_crypto_ossl_configure_server_session(qc->ssl) != 0) {
        note(ql, "TLS session", ENOMEM);
        ERR_clear_error();
        if (qc->ossl)
            ngtcp2_crypto_ossl_ctx_del(qc->ossl);
        SSL_free(qc->ssl);
        ioxd__certs_release(ql->l->certs, qc->certs);
        ngtcp2_conn_del(qc->conn);
        free(qc);
        return;
    }
    SSL_set_app_data(qc->ssl, &qc->ref);
    SSL_set_accept_state(qc->ssl);
    ioxd__certs_bind(qc->ssl, qc->certs);
    ioxd__certs_bind_alpn(qc->ssl, ql->l->alpn, ql->l->alpn_len);
    ngtcp2_conn_set_tls_native_handle(qc->conn, qc->ossl);

    qc->next = ql->conns;
    if (ql->conns)
        ql->conns->prev = qc;
    ql->conns = qc;
    ql->n_conns++;
    ql->p->live++;
    ql->accepted++;
    if (!cid_register(qc, &scid) || !cid_register(qc, &hd.dcid)) {
        note(ql, "connection id table", ENOMEM);
        conn_free(qc);
        return;
    }
    conn_read(qc, from, fromlen, pkt, len);
}

/* ── datagrams nobody owns ─────────────────────────────────────────────────────────────── */

static void send_version_negotiation(struct quic_listener *ql, const struct sockaddr *from, socklen_t fromlen,
                                     const ngtcp2_version_cid *vc)
{
    struct quic_send *s = send_get(ql);
    if (!s)
        return;
    uint8_t  rnd;
    uint32_t versions[2] = { 0x0a0a0a0aU, NGTCP2_PROTO_VER_V1 };
    cb_rand(&rnd, 1, nullptr);
    ngtcp2_ssize n = ngtcp2_pkt_write_version_negotiation(s->buf, sizeof s->buf, rnd, vc->scid, vc->scidlen,
                                                          vc->dcid, vc->dcidlen, versions, 2);
    if (n <= 0) {
        send_put(ql, s);
        return;
    }
    send_submit(ql, s, from, fromlen, (size_t)n, 0);
}

static void send_stateless_reset(struct quic_listener *ql, const struct sockaddr *from, socklen_t fromlen,
                                 const ngtcp2_version_cid *vc, size_t pktlen)
{
    if (pktlen < QUIC_CIDLEN + 22)
        return;
    time_t now = time(nullptr);
    if (now != ql->resets_at) {
        ql->resets_at   = now;
        ql->resets_left = RESETS_PER_SECOND;
    }
    if (ql->resets_left == 0)
        return;
    ql->resets_left--;
    ngtcp2_cid cid;
    ngtcp2_cid_init(&cid, vc->dcid, vc->dcidlen);
    ngtcp2_stateless_reset_token token;
    if (ngtcp2_crypto_generate_stateless_reset_token(token.data, ql->secret, sizeof ql->secret, &cid) != 0)
        return;
    uint8_t rnd[NGTCP2_MAX_CIDLEN + 22 - NGTCP2_STATELESS_RESET_TOKENLEN];
    size_t  rndlen = pktlen <= 43 ? pktlen - NGTCP2_STATELESS_RESET_TOKENLEN - 1 : sizeof rnd;
    cb_rand(rnd, rndlen, nullptr);
    struct quic_send *s = send_get(ql);
    if (!s)
        return;
    ngtcp2_ssize n = ngtcp2_pkt_write_stateless_reset2(s->buf, sizeof s->buf, &token, rnd, rndlen);
    if (n <= 0) {
        send_put(ql, s);
        return;
    }
    send_submit(ql, s, from, fromlen, (size_t)n, 0);
}

static void dispatch(struct quic_listener *ql, const struct sockaddr *from, socklen_t fromlen, const uint8_t *pkt, size_t len)
{
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, pkt, len, QUIC_CIDLEN);
    trace("[w%d] quic datagram %zu bytes from len %u: decode %d version %#x dcidlen %zu\n", ql->p->id, len, fromlen, rv,
          rv == 0 ? vc.version : 0, rv == 0 ? vc.dcidlen : 0);
    if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
        send_version_negotiation(ql, from, fromlen, &vc);
        return;
    }
    if (rv != 0 || ((pkt[0] & 0x80U) && vc.version == 0))
        return;
    struct quic_conn *qc = table_find(ql, vc.dcid, vc.dcidlen);
    if (!qc) {
        if (pkt[0] & 0x80U)
            accept_conn(ql, from, fromlen, pkt, len);
        else
            send_stateless_reset(ql, from, fromlen, &vc, len);
        return;
    }
    switch (qc->state) {
    case QUIC_OPEN:
        conn_read(qc, from, fromlen, pkt, len);
        break;
    case QUIC_CLOSING:
        if (++qc->close_sent_since >= CLOSE_REPEAT) {
            qc->close_sent_since = 0;
            send_close(qc);
        }
        break;
    default:
        break;
    }
}

/* ── the recv ──────────────────────────────────────────────────────────────────────────── */

static void recv_arm(struct quic_listener *ql)
{
    struct quic_recv *r = &ql->recv;
    r->msg = (struct msghdr){ .msg_name = &r->name, .msg_namelen = sizeof r->name };
    struct io_uring_sqe *sqe = ioxd__proactor_sqe(ql->p);
    sqe->opcode    = IORING_OP_RECVMSG;
    sqe->fd        = ql->fd;
    sqe->addr      = (uintptr_t)&r->msg;
    sqe->len       = 1;
    sqe->flags     = IOSQE_BUFFER_SELECT;
    sqe->ioprio    = IORING_RECV_MULTISHOT;
    sqe->buf_group = BGID;
    sqe->user_data = UD(&r->target, TAG_CALL);
    ql->recv_armed   = true;
    ql->recv_starved = false;
}

static void recv_on_cqe(ioxd_cqe_target *t, int res, unsigned flags)
{
    struct quic_recv     *r  = (struct quic_recv *)t;
    struct quic_listener *ql = r->ql;
    proactor_t           *p  = ql->p;
    bool     more    = flags & IORING_CQE_F_MORE;
    bool     has_buf = flags & IORING_CQE_F_BUFFER;
    uint16_t buf_id  = (uint16_t)(flags >> (unsigned)IORING_CQE_BUFFER_SHIFT);

    trace("[w%d] quic recvmsg res=%d more=%d buf=%d\n", p->id, res, more, has_buf);
    if (res < 0) {
        if (has_buf)
            ioxd__bufring_return(&p->bufs, buf_id);
        if (res == -ENOBUFS) {
            ql->recv_armed   = false;
            ql->recv_starved = true;
        } else if (res == -ECANCELED) {
            ql->recv_armed = false;
        } else {
            note(ql, "recvmsg", -res);
            if (!more) {
                ql->recv_armed = false;
                if (!p->draining)
                    recv_arm(ql);
            }
        }
        wake_streams(ql);
        return;
    }
    if (has_buf) {
        uint8_t *buf = ioxd__bufring_at(&p->bufs, buf_id);
        if ((size_t)res >= sizeof(struct io_uring_recvmsg_out)) {
            struct io_uring_recvmsg_out *o = (struct io_uring_recvmsg_out *)buf;
            uint8_t *name    = buf + sizeof *o;
            uint8_t *payload = name + r->msg.msg_namelen + r->msg.msg_controllen;
            uint8_t *end     = buf + res;
            size_t   len     = o->payloadlen;
            if (payload <= end && !(o->flags & MSG_TRUNC) && o->namelen <= r->msg.msg_namelen) {
                if (len > (size_t)(end - payload))
                    len = (size_t)(end - payload);
                if (len)
                    dispatch(ql, (const struct sockaddr *)name, (socklen_t)o->namelen, payload, len);
            }
        }
        ioxd__bufring_return(&p->bufs, buf_id);
    }
    if (!more) {
        ql->recv_armed = false;
        if (!p->draining)
            recv_arm(ql);
    }
    wake_streams(ql);
}

/* ── the timer's completion, and the streams woken by a cycle ─────────────────────────── */

static void timer_on_cqe(ioxd_cqe_target *t, int res, unsigned flags)
{
    (void)res;
    (void)flags;
    struct quic_timer    *tm = (struct quic_timer *)t;
    struct quic_listener *ql = tm->ql;
    tm->armed = tm->cancelling = false;
    ngtcp2_tstamp now = ioxd__quic_now();
    while (ql->n_heap && ql->heap[0]->expiry <= now) {
        struct quic_conn *qc = ql->heap[0];
        if (qc->state != QUIC_OPEN) {
            conn_free(qc);
            continue;
        }
        int rv = ngtcp2_conn_handle_expiry(qc->conn, now);
        if (rv != 0) {
            conn_error(qc, rv);
            continue;
        }
        ioxd__quic_flush(qc);
        if (qc->state == QUIC_OPEN) {
            ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(qc->conn);
            qc->expiry = expiry > now ? expiry : now + NGTCP2_MILLISECONDS;
            heap_update(ql, qc);
        }
    }
    timer_update(ql);
    wake_streams(ql);
}

void ioxd__quic_wake(struct quic_stream *s)
{
    struct quic_listener *ql = s->qc->ql;
    if (s->waking)
        return;
    s->waking    = true;
    s->wake_next = nullptr;
    if (ql->wake_tail)
        ql->wake_tail->wake_next = s;
    else
        ql->wake_head = s;
    ql->wake_tail = s;
}

void ioxd__quic_unwake(struct quic_stream *s)
{
    struct quic_listener *ql = s->qc->ql;
    if (!s->waking)
        return;
    struct quic_stream *prev = nullptr;
    for (struct quic_stream *w = ql->wake_head; w; prev = w, w = w->wake_next) {
        if (w != s)
            continue;
        if (prev)
            prev->wake_next = w->wake_next;
        else
            ql->wake_head = w->wake_next;
        if (ql->wake_tail == w)
            ql->wake_tail = prev;
        break;
    }
    s->waking    = false;
    s->wake_next = nullptr;
}

static void wake_streams(struct quic_listener *ql)
{
    while (ql->wake_head) {
        struct quic_stream *s = ql->wake_head;
        ql->wake_head = s->wake_next;
        if (!ql->wake_head)
            ql->wake_tail = nullptr;
        s->waking    = false;
        s->wake_next = nullptr;
        ioxd__stream_resume(s);
    }
}

void ioxd__quic_service(proactor_t *p)
{
    for (int i = 0; i < p->n_listeners; i++)
        if (p->listeners[i].ql)
            wake_streams(p->listeners[i].ql);
}

void ioxd__quic_rearm(struct listener *l)
{
    struct quic_listener *ql = l->ql;
    if (ql && ql->recv_starved && !ql->p->draining)
        recv_arm(ql);
}

/* ── the port's life ───────────────────────────────────────────────────────────────────── */

static int socket_open(struct quic_listener *ql, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -errno;
    int one = 1, pmtu = IP_PMTUDISC_DO, bufsz = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
    ql->local = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&ql->local, sizeof ql->local) < 0) {   /* NOLINT(readability-trailing-comma): glibc's transparent-union sockaddr argument trips the check */
        int err = errno;
        close(fd);
        return -err;
    }
    return fd;
}

int ioxd__quic_open(proactor_t *p, struct listener *l)
{
    pthread_once(&crypto_once, crypto_init);
    struct quic_listener *ql = calloc(1, sizeof *ql);
    if (!ql) {
        perror("quic");
        return -1;
    }
    ql->p  = p;
    ql->l  = l;
    ql->fd = socket_open(ql, l->port);
    if (ql->fd < 0) {
        fprintf(stderr, "[w%d] quic :%u: %s\n", p->id, l->port, ioxd__io_errstr(-ql->fd));
        free(ql);
        return -1;
    }
    ql->table_mask = 1023;
    ql->table      = calloc(ql->table_mask + 1, sizeof *ql->table);
    if (!ql->table || RAND_bytes(ql->secret, sizeof ql->secret) != 1) {
        perror("quic");
        close(ql->fd);
        free(ql->table);
        free(ql);
        return -1;
    }
    ql->recv.target.on_cqe  = recv_on_cqe;
    ql->recv.ql             = ql;
    ql->timer.target.on_cqe = timer_on_cqe;
    ql->timer.ql            = ql;
    l->ql = ql;
    l->fd = ql->fd;
    recv_arm(ql);
    return 0;
}

void ioxd__quic_drain(struct listener *l)
{
    struct quic_listener *ql = l->ql;
    if (!ql)
        return;
    if (ql->recv_armed) {
        struct io_uring_sqe *sqe = ioxd__proactor_sqe(ql->p);
        sqe->opcode    = IORING_OP_ASYNC_CANCEL;
        sqe->fd        = -1;
        sqe->addr      = UD(&ql->recv.target, TAG_CALL);
        sqe->user_data = TAG_IGNORE;
    }
    struct quic_conn *next;
    for (struct quic_conn *qc = ql->conns; qc; qc = next) {
        next = qc->next;
        bool there = true;
        if (qc->state == QUIC_OPEN) {
            ngtcp2_ccerr_set_application_error(&qc->err, 0, nullptr, 0);
            there = conn_close(qc);
        }
        if (there)
            conn_free(qc);
    }
    if (ql->timer.armed && !ql->timer.cancelling)
        timer_cancel(ql);
    wake_streams(ql);
}

void ioxd__quic_close(struct listener *l)
{
    struct quic_listener *ql = l->ql;
    if (!ql)
        return;
    struct quic_conn *next;
    for (struct quic_conn *qc = ql->conns; qc; qc = next) {
        next = qc->next;
        conn_free(qc);
    }
    while (ql->free_sends) {
        struct quic_send *s = ql->free_sends;
        ql->free_sends = s->next;
        free(s);
    }
    close(ql->fd);
    free(ql->table);
    free(ql->heap);
    free(ql);
    l->ql = nullptr;
}

#else

int ioxd_bind_quic(int port, ioxd_certs *certs, const char *const *alpn)
{
    (void)certs;
    (void)alpn;
    fprintf(stderr, "ioxd_bind_quic: port %d: this build has no QUIC (make QUIC=1 with libngtcp2 and OpenSSL 3.5)\n", port);
    return -1;
}

#endif

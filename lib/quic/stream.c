#include "quic/quic.h"

#if IOXD_QUIC

#include "io/coro.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static struct chunk *chunk_new(const uint8_t *data, size_t len)
{
    struct chunk *c = malloc(sizeof *c + len);
    if (!c)
        return nullptr;
    c->next   = nullptr;
    c->len    = len;
    c->offset = 0;
    c->fin    = false;
    memcpy(c->data, data, len);
    return c;
}

static void chunks_free(struct chunk *c)
{
    while (c) {
        struct chunk *next = c->next;
        free(c);
        c = next;
    }
}

static bool open_end(const struct quic_stream *s)
{
    return s->qc->state == QUIC_OPEN && !s->closed;
}

static void park(struct quic_stream *s)
{
    s->parked = ioxd__coro_current();
    ioxd__coro_yield();
    s->parked = nullptr;
}

/* ── the pump: streams with something to write, served in turn ────────────────────────── */

static void pump_add(struct quic_stream *s)
{
    struct quic_conn *qc = s->qc;
    if (s->pending || s->tx_blocked || s->tx_stopped || s->closed)
        return;
    s->pending   = true;
    s->pump_next = nullptr;
    if (qc->pump_tail)
        qc->pump_tail->pump_next = s;
    else
        qc->pump = s;
    qc->pump_tail = s;
}

static void pump_remove(struct quic_stream *s)
{
    struct quic_conn *qc = s->qc;
    if (!s->pending)
        return;
    struct quic_stream *prev = nullptr;
    for (struct quic_stream *w = qc->pump; w; prev = w, w = w->pump_next) {
        if (w != s)
            continue;
        if (prev)
            prev->pump_next = w->pump_next;
        else
            qc->pump = w->pump_next;
        if (qc->pump_tail == w)
            qc->pump_tail = prev;
        break;
    }
    s->pending   = false;
    s->pump_next = nullptr;
}

static void pump_rotate(struct quic_stream *s)
{
    struct quic_conn *qc = s->qc;
    if (qc->pump != s || qc->pump_tail == s)
        return;
    qc->pump = s->pump_next;
    s->pump_next = nullptr;
    qc->pump_tail->pump_next = s;
    qc->pump_tail = s;
}

static bool wants_write(const struct quic_stream *s)
{
    return s->tx_sent < s->tx_queued || (s->tx_fin && !s->tx_fin_sent);
}

static size_t tx_vecs(struct quic_stream *s, ngtcp2_vec *vecs, size_t max)
{
    size_t n = 0;
    for (struct chunk *c = s->tx_cur; c && n < max; c = c->next) {
        uint64_t skip = s->tx_sent > c->offset ? s->tx_sent - c->offset : 0;
        if (skip >= c->len)
            continue;
        vecs[n++] = (ngtcp2_vec){ c->data + skip, c->len - (size_t)skip };
    }
    return n;
}

static void tx_advance(struct quic_stream *s, size_t n)
{
    s->tx_sent += n;
    while (s->tx_cur && s->tx_cur->offset + s->tx_cur->len <= s->tx_sent)
        s->tx_cur = s->tx_cur->next;
}

ngtcp2_ssize ioxd__stream_write_pkt(ngtcp2_conn *conn, ngtcp2_path *path, ngtcp2_pkt_info *pi, uint8_t *dest,
                                    size_t destlen, ngtcp2_tstamp ts, void *user_data)
{
    struct quic_conn *qc = user_data;
    for (;;) {
        struct quic_stream *s = qc->pump;
        while (s && !wants_write(s)) {
            pump_remove(s);
            s = qc->pump;
        }
        ngtcp2_ssize ndatalen = 0;
        if (!s)
            return ngtcp2_conn_writev_stream(conn, path, pi, dest, destlen, &ndatalen, NGTCP2_WRITE_STREAM_FLAG_NONE, -1,
                                             nullptr, 0, ts);
        ngtcp2_vec vecs[8];
        size_t     nvec  = tx_vecs(s, vecs, sizeof vecs / sizeof vecs[0]);
        size_t     total = 0;
        for (size_t i = 0; i < nvec; i++)
            total += vecs[i].len;
        bool     all   = s->tx_sent + total == s->tx_queued;
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (s->tx_fin && all)
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(conn, path, pi, dest, destlen, &ndatalen, flags, s->id, vecs, nvec, ts);
        trace("[w%d] quic stream %lld write: %zu vecs %zu bytes fin=%d -> n=%zd ndatalen=%zd\n", qc->ql->p->id,
              (long long)s->id, nvec, total, !!(flags & NGTCP2_WRITE_STREAM_FLAG_FIN), n, ndatalen);
        if (ndatalen > 0)
            tx_advance(s, (size_t)ndatalen);
        if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
            s->tx_blocked = true;
            pump_remove(s);
            continue;
        }
        if (n == NGTCP2_ERR_STREAM_SHUT_WR || n == NGTCP2_ERR_STREAM_NOT_FOUND) {
            s->tx_stopped = true;
            pump_remove(s);
            ioxd__quic_wake(s);
            continue;
        }
        if (n == 0)
            return 0;
        if (n < 0 && n != NGTCP2_ERR_WRITE_MORE)
            return n;
        if ((flags & NGTCP2_WRITE_STREAM_FLAG_FIN) && (size_t)(ndatalen > 0 ? ndatalen : 0) == total)
            s->tx_fin_sent = true;
        if (!wants_write(s))
            pump_remove(s);
        else
            pump_rotate(s);
        if (n == NGTCP2_ERR_WRITE_MORE)
            continue;
        return n;
    }
}

/* ── the link a stream's pipe is on ────────────────────────────────────────────────────── */

static int link_recv_item(void *link, struct rx_item *out)
{
    struct quic_stream *s = link;
    for (;;) {
        struct chunk *c = s->rx_head;
        if (c) {
            s->rx_head = c->next;
            if (!s->rx_head)
                s->rx_tail = nullptr;
            out->ptr    = c->data;
            out->len    = (uint32_t)c->len;
            out->buf_id = 0;
            return 1;
        }
        if (s->rx_fin)
            return 0;
        if (s->rx_reset || !open_end(s))
            return -ECONNRESET;
        park(s);
    }
}

static bool link_has_item(void *link)
{
    return ((struct quic_stream *)link)->rx_head != nullptr;
}

static void link_release(void *link, const struct rx_item *item)
{
    struct quic_stream *s = link;
    struct chunk *c = (struct chunk *)((uint8_t *)(uintptr_t)item->ptr - offsetof(struct chunk, data));
    size_t        len = c->len;
    free(c);
    if (open_end(s)) {
        ngtcp2_conn_extend_max_stream_offset(s->qc->conn, s->id, len);
        ngtcp2_conn_extend_max_offset(s->qc->conn, len);
        ioxd__quic_flush(s->qc);
    }
}

static int link_send(void *link, const void *data, size_t n)
{
    struct quic_stream *s = link;
    if (s->tx_stopped || s->tx_fin || !open_end(s))
        return -EPIPE;
    const uint8_t *p = data;
    for (size_t left = n; left;) {
        size_t        k = left < QUIC_CHUNK_MAX ? left : QUIC_CHUNK_MAX;
        struct chunk *c = chunk_new(p, k);
        if (!c)
            return -ENOMEM;
        c->offset = s->tx_queued;
        s->tx_queued += k;
        if (s->tx_tail)
            s->tx_tail->next = c;
        else
            s->tx_head = c;
        s->tx_tail = c;
        if (!s->tx_cur)
            s->tx_cur = c;
        p += k;
        left -= k;
    }
    pump_add(s);
    ioxd__quic_flush(s->qc);
    while (open_end(s) && !s->tx_stopped && s->tx_queued - s->tx_acked > QUIC_HIGH_WATER)
        park(s);
    if (s->tx_stopped || !open_end(s))
        return -EPIPE;
    return (int)n;
}

static const ioxd_pipe_link stream_link = {
    .recv_item = link_recv_item,
    .has_item  = link_has_item,
    .release   = link_release,
    .send      = link_send,
};

/* ── the stream's life ─────────────────────────────────────────────────────────────────── */

static void stream_free(struct quic_stream *s)
{
    struct quic_conn *qc = s->qc;
    pump_remove(s);
    ioxd__quic_unwake(s);
    struct quic_stream *prev = nullptr;
    for (struct quic_stream *w = qc->streams; w; prev = w, w = w->next) {
        if (w != s)
            continue;
        if (prev)
            prev->next = w->next;
        else
            qc->streams = w->next;
        break;
    }
    chunks_free(s->rx_head);
    chunks_free(s->tx_head);
    free(s);
}

static void stream_finish(struct quic_stream *s)
{
    struct quic_conn *qc = s->qc;
    trace("[w%d] quic stream %lld handler done: closed=%d state=%d\n", qc->ql->p->id, (long long)s->id, s->closed, qc->state);
    if (open_end(s)) {
        if (!s->tx_stopped && !s->tx_fin) {
            s->tx_fin = true;
            pump_add(s);
        }
        if (!s->rx_fin && !s->rx_reset)
            ngtcp2_conn_shutdown_stream_read(qc->conn, 0, s->id, 0);
        ioxd__quic_flush(qc);
    }
    chunks_free(s->rx_head);
    s->rx_head = s->rx_tail = nullptr;
    s->coro_done = true;
    qc->live_coros--;
    if (s->closed || qc->state == QUIC_DEAD)
        stream_free(s);
    if (qc->state == QUIC_DEAD && qc->live_coros == 0)
        free(qc);
}

static void stream_main(void *arg)
{
    struct quic_stream *s = arg;
    char                gather[IOXD_PIPE_GATHER];
    char                slab[IOXD_PIPE_LEAD + IOXD_PIPE_CAP + IOXD_PIPE_SLACK];
    ioxd_pipe           pipe;
    ioxd__pipe_init(&pipe, s, &stream_link, nullptr, gather, sizeof gather, slab, IOXD_PIPE_LEAD, IOXD_PIPE_CAP,
                    IOXD_PIPE_SLACK);
    s->qc->ql->p->stream_handler(&pipe);
    ioxd__pipe_close(&pipe);
    stream_finish(s);
}

void ioxd__stream_open(struct quic_conn *qc, int64_t id)
{
    trace("[w%d] quic stream %lld open\n", qc->ql->p->id, (long long)id);
    struct quic_stream *s = calloc(1, sizeof *s);
    if (!s) {
        ngtcp2_conn_shutdown_stream(qc->conn, 0, id, 0);
        return;
    }
    s->qc = qc;
    s->id = id;
    if (!ngtcp2_is_bidi_stream(id))
        s->tx_stopped = true;
    s->next     = qc->streams;
    qc->streams = s;
    ngtcp2_conn_set_stream_user_data(qc->conn, id, s);
    qc->live_coros++;
    ioxd__proactor_spawn(qc->ql->p, stream_main, s);
}

void ioxd__stream_recv(struct quic_stream *s, const uint8_t *data, size_t len, bool fin)
{
    trace("[w%d] quic stream %lld recv %zu bytes fin=%d parked=%d\n", s->qc->ql->p->id, (long long)s->id, len, fin, s->parked != nullptr);
    if (len) {
        struct chunk *c = chunk_new(data, len);
        if (!c) {
            s->rx_reset = true;
            ioxd__quic_wake(s);
            return;
        }
        if (s->rx_tail)
            s->rx_tail->next = c;
        else
            s->rx_head = c;
        s->rx_tail = c;
    }
    if (fin)
        s->rx_fin = true;
    ioxd__quic_wake(s);
}

void ioxd__stream_acked(struct quic_stream *s, uint64_t offset, uint64_t len)
{
    uint64_t end = offset + len;
    if (end > s->tx_acked)
        s->tx_acked = end;
    while (s->tx_head && s->tx_head->offset + s->tx_head->len <= s->tx_acked) {
        struct chunk *c = s->tx_head;
        s->tx_head = c->next;
        if (!s->tx_head)
            s->tx_tail = nullptr;
        if (s->tx_cur == c)
            s->tx_cur = c->next;
        free(c);
    }
    if (s->parked && s->tx_queued - s->tx_acked <= QUIC_HIGH_WATER)
        ioxd__quic_wake(s);
}

void ioxd__stream_closed(struct quic_stream *s)
{
    s->closed = true;
    pump_remove(s);
    chunks_free(s->tx_head);
    s->tx_head = s->tx_tail = s->tx_cur = nullptr;
    if (s->coro_done) {
        stream_free(s);
        return;
    }
    ioxd__quic_wake(s);
}

void ioxd__stream_reset(struct quic_stream *s)
{
    s->rx_reset = true;
    ioxd__quic_wake(s);
}

void ioxd__stream_stop(struct quic_stream *s, uint64_t app_error)
{
    s->tx_stopped = true;
    pump_remove(s);
    if (open_end(s))
        ngtcp2_conn_shutdown_stream_write(s->qc->conn, 0, s->id, app_error);
    ioxd__quic_wake(s);
}

void ioxd__stream_unblock(struct quic_stream *s)
{
    s->tx_blocked = false;
    if (wants_write(s))
        pump_add(s);
}

void ioxd__stream_abandon(struct quic_conn *qc)
{
    struct quic_stream *next;
    for (struct quic_stream *s = qc->streams; s; s = next) {
        next = s->next;
        pump_remove(s);
        if (s->coro_done) {
            stream_free(s);
            continue;
        }
        if (qc->state == QUIC_DEAD) {
            chunks_free(s->tx_head);
            s->tx_head = s->tx_tail = s->tx_cur = nullptr;
            s->closed  = true;
        }
        ioxd__quic_wake(s);
    }
}

void ioxd__stream_resume(struct quic_stream *s)
{
    coro_t *c = s->parked;
    if (!c)
        return;
    s->parked = nullptr;
    ioxd__coro_resume(c);
}

#endif

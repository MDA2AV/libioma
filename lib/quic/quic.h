/*
 * quic/quic.h - a QUIC port on its worker: the UDP socket, its multishot recvmsg, the
 * connections keyed by connection id, their timers as one kernel timeout, and the sends that
 * leave as GSO trains. ngtcp2 runs the protocol, OpenSSL its TLS 1.3; the streams are
 * quic/stream.c. Private; not installed.
 */
#pragma once

#include "io/internal.h"
#include "io/pipe.h"
#include "ioxd/quic.h"

#if IOXD_QUIC

#include <netinet/in.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#define QUIC_CIDLEN       8                     /* the ids this server mints; short headers are parsed by it */
#define QUIC_SEND_BUF     (63 * 1024)           /* one GSO train, under the UDP payload ceiling            */
#define QUIC_HIGH_WATER   (256UL * 1024)        /* bytes a stream's writer may have unacknowledged before it parks */
#define QUIC_CHUNK_MAX    16384U                /* bytes of stream data one retained chunk holds           */

struct quic_listener;
struct quic_conn;
struct quic_stream;

/* A run of stream bytes: received and not yet read, or written and not yet acknowledged. */
struct chunk {
    struct chunk *next;
    size_t        len;
    uint64_t      offset;                       /* tx: where in the stream it starts                       */
    bool          fin;                          /* rx: the stream ended with these bytes                   */
    uint8_t       data[];
};

/* One stream of a connection, served by a coroutine of its own with a pipe over it. */
struct quic_stream {
    struct quic_conn   *qc;
    int64_t             id;
    struct chunk       *rx_head, *rx_tail;      /* delivered, not yet read                                 */
    bool                rx_fin, rx_reset;       /* the peer finished, or reset, its sending side           */
    struct chunk       *tx_head, *tx_tail;      /* retained until acknowledged; ngtcp2 reads them in place */
    struct chunk       *tx_cur;                 /* the first not yet wholly handed to ngtcp2               */
    uint64_t            tx_queued, tx_sent, tx_acked;   /* stream offsets                                  */
    bool                tx_fin, tx_fin_sent, tx_stopped, tx_blocked;
    bool                pending;                /* in the connection's pump list                           */
    struct quic_stream *pump_next;
    coro_t             *parked;                 /* its coroutine, parked in a read or at the high-water mark */
    bool                coro_done, closed;      /* the handler returned; ngtcp2 is done with the stream    */
    bool                waking;                 /* in the listener's wake list                             */
    struct quic_stream *wake_next;
    struct quic_stream *next;                   /* the connection's list                                   */
};

enum quic_state { QUIC_OPEN, QUIC_CLOSING, QUIC_DRAINING, QUIC_DEAD };

/* A connection: ngtcp2's, its TLS session, its streams, and where it stands in the listener's
 * tables. It outlives its ngtcp2 conn while a stream's coroutine still runs. */
struct quic_conn {
    struct quic_listener   *ql;
    ngtcp2_conn            *conn;
    SSL                    *ssl;
    ngtcp2_crypto_ossl_ctx *ossl;
    ngtcp2_crypto_conn_ref  ref;
    struct table           *certs;              /* the store's table this handshake started on, referenced */
    struct sockaddr_storage remote;
    socklen_t               remote_len;
    ngtcp2_ccerr            err;
    enum quic_state         state;
    uint8_t                *close_pkt;          /* the CONNECTION_CLOSE, repeated while closing            */
    size_t                  close_len;
    unsigned                close_sent_since;   /* packets taken since it was last repeated                */
    ngtcp2_tstamp           expiry;             /* what the heap orders by                                 */
    unsigned                heap_idx;
    ngtcp2_cid              cids[16];           /* the ids in the table for this connection                */
    unsigned                n_cids;
    struct quic_stream     *streams;
    struct quic_stream     *pump, *pump_tail;   /* streams with bytes to write, served in turn             */
    unsigned                live_coros;         /* stream coroutines still running                         */
    struct quic_conn       *next, *prev;        /* the listener's list                                     */
};

/* A staged operation the loop calls back: the first member of each. */
struct quic_recv {
    ioxd_cqe_target         target;
    struct quic_listener   *ql;
    struct msghdr           msg;
    struct sockaddr_storage name;
};
struct quic_send {
    ioxd_cqe_target         target;
    struct quic_listener   *ql;
    struct msghdr           msg;
    struct iovec            iov;
    struct sockaddr_storage to;
    char                    control[CMSG_SPACE(sizeof(uint16_t))];
    struct quic_send       *next;
    uint8_t                 buf[QUIC_SEND_BUF];
};
struct quic_timer {
    ioxd_cqe_target         target;
    struct quic_listener   *ql;
    struct __kernel_timespec ts;
    bool                    armed, cancelling;
    ngtcp2_tstamp           deadline;
};

struct cid_slot {
    uint8_t           cid[NGTCP2_MAX_CIDLEN];
    uint8_t           len;                      /* 0: empty                                                */
    bool              tomb;
    struct quic_conn *qc;
};

struct quic_listener {
    proactor_t          *p;
    struct listener     *l;
    int                  fd;
    struct sockaddr_in   local;
    struct quic_recv     recv;
    bool                 recv_armed, recv_starved;
    struct quic_send    *free_sends;
    unsigned             n_sends, n_free;
    bool                 no_gso;
    struct quic_timer    timer;
    struct quic_conn   **heap;
    unsigned             n_heap, cap_heap;
    struct cid_slot     *table;
    unsigned             table_mask, table_used, table_tombs;
    struct quic_conn    *conns;
    unsigned             n_conns;
    uint8_t              secret[32];            /* stateless reset tokens                                  */
    unsigned             resets_left;           /* this second                                             */
    time_t               resets_at;
    struct quic_stream  *wake_head, *wake_tail;
    uint64_t             accepted;
};

/* quic.c: the port's life on the worker, and what a stream needs of its connection. */
int  ioxd__quic_open  (proactor_t *p, struct listener *l);   /* socket, recv and tables; -1 with the reason logged */
void ioxd__quic_drain (struct listener *l);                   /* shutdown: close every connection, stop receiving */
void ioxd__quic_close (struct listener *l);                   /* after the ring: free it all */
void ioxd__quic_rearm (struct listener *l);                   /* buffers came back: a recv parked on -ENOBUFS */
void ioxd__quic_service(proactor_t *p);                       /* once per loop turn: resume the streams woken */
void ioxd__quic_flush (struct quic_conn *qc);                 /* send what ngtcp2 has to send; a stream's send calls it */
void ioxd__quic_wake  (struct quic_stream *s);                /* resume its parked coroutine, from the loop */
void ioxd__quic_unwake(struct quic_stream *s);                /* a stream freed while queued to wake */
ngtcp2_tstamp ioxd__quic_now(void);

/* stream.c: what the connection's callbacks hand over, and the pump the writes come from. */
void ioxd__stream_open  (struct quic_conn *qc, int64_t id);
void ioxd__stream_recv  (struct quic_stream *s, const uint8_t *data, size_t len, bool fin);
void ioxd__stream_acked (struct quic_stream *s, uint64_t offset, uint64_t len);
void ioxd__stream_closed(struct quic_stream *s);
void ioxd__stream_reset (struct quic_stream *s);
void ioxd__stream_stop  (struct quic_stream *s, uint64_t app_error);
void ioxd__stream_unblock(struct quic_stream *s);
void ioxd__stream_abandon(struct quic_conn *qc);              /* the connection is gone: every stream ends */
ngtcp2_ssize ioxd__stream_write_pkt(ngtcp2_conn *conn, ngtcp2_path *path, ngtcp2_pkt_info *pi,
                                    uint8_t *dest, size_t destlen, ngtcp2_tstamp ts, void *user_data);
void ioxd__stream_resume(struct quic_stream *s);              /* the loop: run what was woken */

#else

static inline int  ioxd__quic_open   (proactor_t *p, struct listener *l) { (void)p; (void)l; return -1; }
static inline void ioxd__quic_drain  (struct listener *l) { (void)l; }
static inline void ioxd__quic_close  (struct listener *l) { (void)l; }
static inline void ioxd__quic_rearm  (struct listener *l) { (void)l; }
static inline void ioxd__quic_service(proactor_t *p) { (void)p; }

#endif

/* ── quic.c: the notes ─────────────────────────────────────────────────────────────────── */

/*
 * quic/quic.c - a QUIC port on one worker. The socket is the worker's own (SO_REUSEPORT), so
 * the kernel spreads peers across workers as it does for TCP; one multishot recvmsg delivers
 * every datagram into a provided buffer, with the peer's address in front. A datagram is
 * routed by the connection id in its header: known ids go to their connection, an Initial with
 * an unknown one starts a connection, the rest is answered with a stateless reset or dropped.
 * A connection is ngtcp2's; a cycle - a datagram read, a timer fired - ends with everything
 * ngtcp2 wants sent leaving as one GSO train per burst. Timers are one kernel timeout per
 * worker, armed at the earliest expiry in a heap of connections. Stream coroutines are never
 * resumed from inside ngtcp2: a callback queues the stream, and the loop resumes it after the
 * cycle, when nothing is on the stack.
 *
 * The ids this server mints carry the worker in their first byte (cid[0] mod workers), so a
 * kernel filter could steer a peer that changed address back here; without one a moved peer
 * lands on whichever worker the 4-tuple hashes to and is dropped there, as a stale id.
 */

/* at file scope:
 *   - what the transport parameters offer a peer: a window per stream, one for the connection,
 *     how many streams it may open, how long the connection may idle, and how long a handshake
 *     may take before the connection is dropped  [#define IDLE_TIMEOUT      (30 *
 *     NGTCP2_SECONDS)]
 *   - a CONNECTION_CLOSE is repeated once per this many packets that still arrive for the
 *     closing connection  [#define CLOSE_REPEAT      8]
 *   - stateless resets a second, so an unknown id cannot make this server flood
 *     [#define RESETS_PER_SECOND 64]
 *   - send buffers kept warm per port  [#define IDLE_SENDS        64]
 */

/* ioxd__quic_now:
 * ngtcp2's clock: nanoseconds, monotonic.
 */

/* table_slot:
 * Open addressing with linear probing over the ids: the slot holding cid, or with insert the
 * first free one on its probe path (a tombstone first, so deleted ids do not pile up).
 */

/* table_grow:
 * Twice the slots, every live id rehashed, the tombstones dropped.
 */

/* cid_register:
 * An id this connection answers to, in the table and on the connection's own list so a free
 * takes them all out; ngtcp2 issues at most the peer's active_connection_id_limit of them.
 */

/* send_get:
 * A send buffer from the port's pool, or a fresh one.
 */

/* send_on_cqe:
 * The send is done, the buffer free. A refused GSO train - a kernel without UDP_SEGMENT, a
 * device that will not - turns GSO off for the port: from then on one packet per send; the
 * train that failed is left to loss recovery.
 */

/* send_submit:
 * One sendmsg: the datagram(s) in the buffer to the peer, with a UDP_SEGMENT control message
 * naming the segment size when the buffer holds a train.
 */

/* heap_update:
 * The connection's place in the heap after its expiry changed: pushed if new, else sifted.
 */

/* timer_arm:
 * The port's one kernel timeout, at the earliest expiry; relative, since the ring's timeouts
 * are. Already due is one nanosecond away.
 */

/* timer_update:
 * The heap's earliest expiry against what is armed: nothing armed and something to wait for
 * arms it; something earlier than the armed deadline cancels the armed one, and its cancel
 * completion re-arms at the new earliest. A later deadline is left: the timer fires early
 * and finds nothing due, which is cheaper than a cancel.
 */

/* conn_reschedule:
 * After a cycle: the connection's next expiry into the heap, the timer after it.
 */

/* conn_leave:
 * Into the closing or draining period: three PTOs, then the connection goes; its streams end
 * now.
 */

/* conn_close:
 * A CONNECTION_CLOSE with the error recorded, sent and kept to repeat, and the closing period
 * begins. Nothing to send - the handshake never got far enough - frees the connection.
 */

/* conn_error:
 * What ngtcp2's error means for the connection: the peer closed it (draining), it is to be
 * dropped without a word, it idled out, or it failed and the peer is told.
 */

/* ioxd__quic_flush:
 * The write cycle: as many packets as ngtcp2 will write into one buffer - a GSO train of
 * equal packets, the last may be shorter - each buffer one sendmsg, until ngtcp2 has nothing
 * more or its pacing says later. The streams' bytes come through ioxd__stream_write_pkt.
 *   - as much as the pacer allows, never less than one packet, never more than the buffer
 *     [size_t buflen = quantum > one ? quantum : one;]
 */

/* conn_read:
 * A datagram for the connection: fed to ngtcp2 on the path it arrived on - a peer that moved
 * is ngtcp2's to validate - then the write cycle, and the timer after it.
 */

/* cb_new_cid:
 * ngtcp2 wants another id to give the peer: random, the worker in its first byte, its
 * stateless reset token from the port's secret, and into the table.
 */

/* cb_stream_close:
 * ngtcp2 is done with a stream; a stream the peer opened gives it one more to open.
 */

/* conn_free:
 * Out of the heap, the table and the list; ngtcp2's conn and the TLS session freed, the
 * store's table released. The streams whose coroutines still run keep the struct itself
 * alive: the last of them frees it.
 */

/* accept_conn:
 * An Initial for an id nobody holds: ngtcp2 checks it is one, and a connection is made for
 * it - ngtcp2's, then the TLS session from the store's QUIC context, bound to the store's
 * table for SNI and to the port's protocols for ALPN. The peer's chosen id and the one this
 * server minted both route to it: the peer keeps using its own until it has read a reply.
 * Then the Initial is fed like any datagram.
 */

/* send_version_negotiation:
 * A version this server does not speak: the versions it does, in a Version Negotiation
 * packet, with a reserved one first so a client is kept honest about ignoring unknown ones.
 */

/* send_stateless_reset:
 * A short-header packet for an id nobody holds: a stateless reset with the token the id would
 * have carried, so a peer that still holds the connection learns it is gone. Shorter than the
 * packet it answers, at most so many a second.
 */

/* dispatch:
 * A datagram: its version and id decoded, then to the connection holding the id, or to accept
 * for an Initial, or answered with a reset. A closing connection repeats its CONNECTION_CLOSE
 * every CLOSE_REPEAT packets; a draining one says nothing.
 */

/* recv_arm:
 * The port's multishot recvmsg into the worker's provided buffers: the kernel writes the
 * peer's address in front of each datagram.
 */

/* recv_on_cqe:
 * One datagram: the io_uring_recvmsg_out header, the name, then the payload, in the buffer
 * the kernel picked; dispatched and the buffer returned. -ENOBUFS parks the recv until
 * buffers come back (ioxd__quic_rearm); the multishot ending re-arms it.
 */

/* timer_on_cqe:
 * The timeout fired, or was cancelled to move: every connection whose expiry passed gets its
 * expiry handled - loss recovery, idle, handshake timeouts, the end of a closing period - and
 * a write cycle; then the timer is armed at the new earliest.
 */

/* ioxd__quic_wake:
 * A stream whose coroutine should run: queued for the loop. Never resumed here - a callback
 * is ngtcp2's stack, and a handler must not run on it.
 */

/* wake_streams:
 * The loop, after a cycle: every queued stream's coroutine resumed.
 */

/* socket_open:
 * The port's UDP socket: SO_REUSEPORT so every worker has one, no fragmentation - QUIC
 * forbids it - and room in the socket buffers for a burst.
 */

/* ioxd__quic_open:
 * The worker's share of the port: socket, tables, secret, and the recv armed.
 */

/* ioxd__quic_drain:
 * Shutdown: the recv cancelled, every connection told (application error 0) and freed at
 * once - the closing period is not waited out - so the worker's count of live connections
 * reaches zero.
 */

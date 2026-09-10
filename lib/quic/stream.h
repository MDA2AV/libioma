/*
 * quic/stream.h - the notes of stream.c, whose declarations are quic/quic.h
 */
#pragma once

/* ── stream.c: the notes ───────────────────────────────────────────────────────────────── */

/*
 * quic/stream.c - a QUIC stream as a pipe: its own coroutine runs the pipe handler over it.
 * What ngtcp2 delivers for the stream is copied into chunks the reader pops and gives back -
 * the datagram's buffer goes back to the kernel at once, and giving a chunk back is what
 * opens the peer's window by that much. What the handler writes is copied into chunks that
 * stay until the peer acknowledges them, since ngtcp2 keeps pointers into them for a
 * retransmission; a writer with more than QUIC_HIGH_WATER unacknowledged parks until acks
 * bring it down. A stream with bytes to send is in its connection's pump, which the write
 * cycle (quic.c) serves in turn, one stream's bytes after another's into the same packet.
 *
 * Nothing here resumes a coroutine: a callback marks the stream and asks the listener to
 * wake it (ioxd__quic_wake), and the loop resumes it once ngtcp2 has unwound.
 */

/* chunk_new:
 * A copy of the bytes, with the header in front.
 */

/* open_end:
 * The connection is serving and ngtcp2 still has the stream: a read may wait, a write may go.
 */

/* park:
 * Suspend the stream's coroutine until the listener resumes it.
 */

/* pump_add:
 * Queue the stream for the write cycle, unless it has nothing it may write now.
 */

/* pump_remove:
 * Take it out, wherever it is in the list.
 */

/* pump_rotate:
 * The head goes to the tail: a stream that wrote gives the next one its turn.
 */

/* wants_write:
 * Bytes not yet handed to ngtcp2, or a FIN not yet sent.
 */

/* tx_vecs:
 * The retained bytes from the send offset on, as vectors for ngtcp2, at most max of them.
 */

/* tx_advance:
 * ngtcp2 took n bytes: move the send offset and the chunk cursor past them.
 */

/* ioxd__stream_write_pkt:
 * One packet for ngtcp2_conn_write_aggregate_pkt2 (quic.c): the pump's streams offer their
 * bytes in turn with WRITE_STREAM_FLAG_MORE, so several share a packet, until ngtcp2 says the
 * packet is complete; with nothing to offer, ngtcp2 writes what it has of its own (ACKs, its
 * frames). A FIN goes with the last bytes, or alone once they are all handed over.
 *   - the window is shut on this stream: it re-enters the pump when ngtcp2 opens it
 *     (ioxd__stream_unblock)  [if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED) {]
 *   - the stream cannot be written any more: the writer learns so  [if (n ==
 *     NGTCP2_ERR_STREAM_SHUT_WR || n == NGTCP2_ERR_STREAM_NOT_FOUND) {]
 *   - nothing could be written now - congestion, pacing - so nothing was  [if (n == 0)]
 *   - the FIN went out only when every byte offered went with it  [if ((flags &
 *     NGTCP2_WRITE_STREAM_FLAG_FIN) && (size_t)(ndatalen > 0 ? ndatalen : 0) == total)]
 */

/* link_recv_item:
 * The reader's next chunk; parks until one arrives. 0 once the peer's FIN was delivered,
 * -ECONNRESET when the peer reset the stream, the stream went away or the connection did.
 */

/* link_release:
 * A chunk read to the end: freed, and the peer's window opened by as much, with a write cycle
 * so the MAX_STREAM_DATA can go out.
 */

/* link_send:
 * The bytes copied into retained chunks, the pump told, a write cycle run; then the writer
 * parks while more than the high-water mark is unacknowledged. -EPIPE once the stream cannot
 * take more: the peer stopped it, it ended, or the connection did.
 */

/* stream_free:
 * Out of every list, its chunks gone. Only once ngtcp2 is done with the stream or the
 * connection is gone: until then ngtcp2 may still read the retained chunks.
 */

/* stream_finish:
 * The handler returned: the FIN goes after the last byte, a peer still sending is told to
 * stop, whatever was not read is dropped. The stream stays for ngtcp2 while it still holds
 * it; a connection already gone is freed by its last coroutine.
 */

/* stream_main:
 * The stream's coroutine: a pipe over the stream, the handler, the finish.
 */

/* ioxd__stream_open:
 * The peer opened a stream: a stream object, hung on ngtcp2's stream as its user data, and a
 * coroutine spawned for it. A one-way stream is read-only, so its send side is stopped from
 * the start.
 */

/* ioxd__stream_recv:
 * Bytes from ngtcp2, inside its callback: copied into a chunk, the reader woken after the
 * cycle.
 */

/* ioxd__stream_acked:
 * The peer acknowledged up to an offset: the chunks wholly below it are freed, and a writer
 * parked at the high-water mark is woken if it is now under it.
 */

/* ioxd__stream_closed:
 * ngtcp2 is done with the stream: its retained bytes can go, and so can the stream once the
 * handler has returned.
 */

/* ioxd__stream_stop:
 * STOP_SENDING from the peer: nothing more is written, and ngtcp2 resets the sending side.
 */

/* ioxd__stream_abandon:
 * The connection left: every stream is woken to end; those whose handler already returned
 * are freed now. With the connection gone, ngtcp2 holds nothing, so the retained bytes go too.
 */

/* ioxd__stream_resume:
 * The loop resumes the parked coroutine, if any, once nothing of ngtcp2's is on the stack.
 */

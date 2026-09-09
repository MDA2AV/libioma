/*
 * http/engine.h - the HTTP/1.1 engine's one entry for the runner: the per-connection loop.
 */
#pragma once

#include "io/pipe.h"

void ioxd__engine_serve(struct ioxd_pipe *pipe);         /* requests on the connection until it ends */

/* ── engine.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * engine.c - the HTTP/1.1 engine: parse a request head with picohttpparser, run the middleware
 * chain and the endpoint against a context, read the body on demand (whole or streamed)
 * through the connection's pipe (io/pipe.h) and drain what was left, then send what was
 * written through the same pipe. All of it runs on the connection's coroutine, so a read or a
 * flush simply suspends it and the loop resumes it.
 */

/* at file scope:
 *   - per-request arena for percent-decoded query parameters  [#define IOXD_PARAM_CAP    2048]
 *   - a serialized reply head must fit here  [#define IOXD_HEAD_CAP     4096]
 *   - unread body discarded after a handler before we close instead  [#define IOXD_DRAIN_MAX
 *     (1024UL * 1024)]
 *   - bytes of chunked trailers taken before the body is a 400  [#define IOXD_TRAILER_MAX
 *     4096]
 *   - serve() hands req.headers to picohttpparser as its header array: a kv (two slices) must
 *     lay out exactly like a phr_header (name, name_len, value, value_len).
 *     [static_assert(sizeof(ioxd_kv) == sizeof(struct phr_header), "ioxd_kv must mirror]
 *   - The engine's per-request state, behind ctx->priv.  [struct serve_state {]
 *   - the connection: the head is kept in its reader  [struct ioxd_pipe *pipe;]
 *   - body bytes handed out so far  [size_t       body_read;]
 *   - the whole body has been taken off the wire  [bool         body_done;]
 *   - ioxd_body_all kept it  [bool         body_whole;]
 *   - 0, a status to answer (400, 413), or -1: peer gone  [int          body_err;]
 *   - data bytes of the current chunk still to deliver  [size_t       chunk_left;]
 *   - a HEAD request: the reply's body stays unsent  [bool         head_only;]
 *   - decided with the head: HEAD, 1xx, 204, 304  [bool         no_body;]
 *   - the 100 Continue an Expect asked for went out  [bool         continue_sent;]
 *   - ── request headers ─────────────────────────────────────────────────────────────────────
 *     [static inline unsigned char lower_ascii(unsigned char a)]
 *   - What the engine picks from the request headers as it lower-cases their names: the
 *     framing fields, Host, Connection and Expect - and whether they make a request it must
 *     refuse.
 *   - the last transfer coding named is chunked  [bool       te_last_chunked;]
 *   - a coding other than that final chunked was named  [bool       te_other;]
 *   - over every Connection line  [bool       close, keep_alive;]
 *   - 0, or the status to answer: 400, 417  [int        refuse;]
 *   - ── the reply: head serialization and the write slab ────────────────────────────────────
 *     [static inline int put_uint(char *dst, size_t v)]
 *   - A constant slice: pointer + length, so a precomposed line is one memcpy.  [struct cslice
 *     { const char *p; int len; };]
 *   - How the body is delimited on the wire.  [enum framing {]
 *   - a reply that cannot have one: 1xx, 204, a 304 with no length  [FRAME_NONE,]
 *   - ── the body: read on demand ────────────────────────────────────────────────────────────
 *     [/ * A body failure with a status: the engine answers with it after the handler un]
 *   - --- a chunked body ---  [static long body_line(ioxd_ctx *ctx, ioxd_slice *live)]
 *   - --- the three reads ---  [/ * The whole body, kept in the reader: a Content-Length body
 *     once it is all here]
 *   - ── the connection loop ─────────────────────────────────────────────────────────────────
 *     [/ * Get a complete request head into read_buf, parsed straight into req (method, ]
 */

/* lower_ascii:
 * Fold A-Z to a-z; every other byte unchanged.
 */

/* eq_ci:
 * Case-insensitive equality of two slices: a length test, then a byte loop. No libc, no
 * locale.
 */

/* parse_length:
 * A Content-Length value: decimal digits and nothing else, into *out; false for anything else,
 * including a number too large for size_t. Anything looser lets one request smuggle another
 * behind a proxy that reads the value differently.
 *   - wraps huge for a non-digit  [unsigned d = (unsigned char)s[i] - (unsigned)'0';]
 */

/* next_token:
 * The comma-separated tokens of a list header value, one per call from *at, blanks trimmed;
 * false once the list is done.
 *   - trim trailing blanks  [size_t end = *at;]
 */

/* token_present_ci:
 * Is `tok` one of the comma-separated tokens in the header value [s, s+n)? Case-insensitive.
 */

/* lower_inplace:
 * Lower-case ASCII in place, eight bytes per step. The bytes must all be below 0x80 - true for
 * header names, which picohttpparser only accepts as HTTP tokens - so the adds cannot carry
 * between bytes: +0x3f sets a byte's high bit from 'A' up, +0x25 from 'Z'+1 up, and the
 * difference marks exactly 'A'..'Z'.
 *   - 0x80 >> 2 == 0x20  [w |= upper >> 2;]
 */

/* pick_transfer_encoding:
 * One Transfer-Encoding line into the picked state: its codings join the list the earlier
 * lines made, so a chunked that was last is last no more.
 *   - an empty list  [picked->refuse = 400;]
 */

/* pick_headers:
 * One pass over the request headers: lower-case each name in place (the buffer is ours), so
 * handlers and this switch compare with plain memcmp. The switch on the name length rejects
 * nearly every header before a byte is compared. A folded continuation line (obs-fold) comes
 * from the parser with no name: it must not be interpreted, so the request is refused.
 *   - two that disagree  [picked.refuse = 400;]
 */

/* keep_alive_from:
 * HTTP/1.1 keeps alive unless a Connection line says "close"; HTTP/1.0 only with "keep-alive".
 */

/* put_uint:
 * Write v in decimal at dst; return the digit count. A digit loop, no printf.
 */

/* put_hex:
 * The same in hex, for chunk sizes.
 */

/* status_line:
 * The precomposed status line for the common codes; nullptr for the rest (built on the spot).
 */

/* send_status:
 * A bodyless framework reply (parse errors, limits): an error path, so plain snprintf. Best
 * effort; the caller then closes.
 *   - whatever the handler had buffered is moot  [ioxd__pipewriter_reset(pw);]
 */

/* wire_status:
 * The status as it goes on the wire: three digits, or a 500 for a handler's mistake.
 */

/* build_head:
 * Serialize the head into dst by memcpy of precomposed pieces plus the integer writer - no
 * snprintf. Every field name goes out lower-cased: the engine's own are lowercase literals, a
 * handler's were folded as ioxd_header copied them, so they are one memcpy of the arena.
 * Returns the length, or -1 if it does not fit.
 *   - Connection: only when it says something. HTTP/1.1 is persistent by default, so a
 *     kept-alive 1.1 reply carries none; a 1.0 client that asked for keep-alive is told it got
 *     it; a closing reply always says close.  [bool keep = ctx->req.keep_alive &&
 *     !res->close;]
 *   - the handler's lines, serialized as added  [PUT(res->head, res->head_len);]
 */

/* put_terminator:
 * The chunked terminator: the empty last chunk and the end of the trailers.
 */

/* fail:
 * Mark the reply dead (the peer is gone, or a head that cannot be built) and fail the call.
 */

/* flush:
 * Send the slab, with the head in front of it the first time. That first time decides the
 * framing: a final flush with the head unsent means the whole body is here (Content-Length,
 * one send); an early flush means the body outgrew the slab, so it streams - with the declared
 * length if the handler gave one, else chunked on HTTP/1.1, else until close on HTTP/1.0. It
 * also settles what the request and the status dictate: no body at all after HEAD, a 1xx, 204
 * or 304 (RFC 9112 6.3), no framing header on a 1xx or 204, and "connection: close" when the
 * body still on the wire is more than will be drained. A declared length is held to: a whole
 * buffered reply gets the real one, a stream never exceeds it, and one that falls short
 * closes.
 *   - the first send: decide the framing  [if (!res->head_sent) {]
 *   - A declared length stands while the body streams, and on a reply that carries no body at
 *     all - HEAD says what the GET would have sent, whether or not the handler wrote it. A
 *     reply buffered whole is measured instead: the length is what was written.  [bool
 *     declared = res->has_length && (!final || state->no_body);]
 *   - a body left unread: more than the drain takes means close  [if (!state->body_done &&
 *     !state->body_err) {]
 *   - an expecting client may never send it at all  [res->close = true;]
 *   - the head alone; what was written as a body stays here  [if (state->no_body) {]
 *   - never a byte past the declared length  [if (res->has_length) {]
 *   - the slab's bytes as one chunk: size in front, CRLF behind  [if (res->chunked && pw->len)
 *     {]
 *   - the terminator rides the same send  [if (final && res->chunked) {]
 *   - one contiguous send  [memcpy(front, head, (size_t)head_len);]
 *   - bigger than the lead: on its own, first  [else if (ioxd__pipewriter_through(pw, head,
 *     (size_t)head_len) < 0)]
 *   - the handler wrote past its own length: nothing more goes  [return fail(res);]
 */

/* finish:
 * After the chain: send what is left - the whole reply if nothing went out yet - and close a
 * chunked stream. A streamed reply that fell short of its declared length closes the
 * connection, so the client sees it was cut.
 *   - nothing sent yet, or bytes still in the slab  [if (!res->head_sent || pw->len) {]
 *   - streamed and drained: just the terminator  [} else if (res->chunked) {]
 */

/* ioxd_write:
 * Append body bytes to the slab; send it, head first, whenever it fills.
 */

/* write_formatted_heap:
 * Something bigger than the whole slab: format it on the heap and write it in pieces.
 *   - the reply cannot be completed as promised  [return fail(&ctx->res);]
 */

/* ioxd_printf:
 * Format straight into the slab. If it does not fit the room left, flush and format again into
 * the empty slab; if it would not fit even that, it goes through the heap.
 *   - a formatting error: nothing written  [} else if ((size_t)n < room) {]
 *   - it fit  [} else if ((size_t)n < room) {]
 *   - bigger than the slab itself  [} else if ((size_t)n >= pw->cap) {]
 *   - make room, then it fits  [} else if (flush(ctx, false) == 0) {]
 */

/* ioxd_flush:
 * Send what is in the slab now. Starts streaming: the head goes out with it.
 */

/* ioxd_reserve:
 * n bytes of the reply to write into directly, flushing first when they do not fit.
 */

/* ioxd_advance:
 * The caller wrote n of the reserved bytes.
 */

/* body_fail:
 * A body failure with a status: the engine answers with it after the handler unless a reply is
 * already streaming, and res.status shows it so a handler can stop before it writes anything.
 */

/* body_begin:
 * Before the first read of the body: a client that sent "Expect: 100-continue" is waiting for
 * the interim reply before it sends a byte (RFC 9110 10.1.1), so it goes out now, ahead of
 * anything the handler has buffered. False when the peer is gone.
 */

/* body_bytes:
 * The live bytes with something unexamined, or more of them; a failure recorded: no room is a
 * 413, the peer gone or the input ending inside the body is -1.
 */

/* body_line:
 * The line at the front of the live bytes, whole: its length without the CRLF, or -1 recorded.
 */

/* chunk_trailers:
 * After the last chunk: trailer lines up to an empty one, then the body is done. They are
 * dropped unread, and bounded, so a peer cannot hold the connection with an endless trailer.
 */

/* chunk_header:
 * The next chunk's size line - hex digits, an optional extension, CRLF - into chunk_left. The
 * last chunk (size 0) also takes its trailers and ends the body. The line is held to the
 * grammar (RFC 9112 7.1): digits, then either the CRLF or blanks and a ';' with something
 * after it; a size line the reader cannot hold is malformed, not too large.
 */

/* chunk_end:
 * The CRLF that ends a chunk's data.
 */

/* ioxd_body_all:
 * The whole body, kept in the reader: a Content-Length body once it is all here, a chunked one
 * chunk by chunk with the data slid together. In place after the head when it all arrived in
 * one kernel buffer, gathered otherwise. Once; then the slice, which is also req.body.
 *   - already streaming, or failed  [if (state->body_read || state->body_err)]
 */

/* fixed_data:
 * Up to n bytes of a Content-Length body into dst, straight from the reader.
 *   - gone, or the input ended inside the body  [state->body_err = -1;]
 */

/* chunk_data:
 * Up to n data bytes of the current chunk into dst; the CRLF after its last byte is taken too.
 *   - what was copied still counts; the next call fails  [return got;]
 */

/* ioxd_body_read_until:
 * The next bytes of the body into dst, reading until n are there or the body ends.
 *   - the count comes back as an int  [n = INT_MAX;]
 */

/* ioxd_body_read_next_chunk:
 * The next chunk of a chunked body, whole, into dst: the rest of the current one when a read
 * stopped inside it, else the next.
 *   - the chunk does not fit dst  [if (state->chunk_left > cap || cap > INT_MAX) {]
 */

/* drain_body:
 * After the chain: take an unread body off the wire so the connection stays in sync, up to a
 * limit - past it, the reply says close and the rest is never read.
 */

/* read_head:
 * Get a complete request head into read_buf, parsed straight into req (method, target,
 * version, headers). What is buffered is parsed first: after a reply, a pipelined next request
 * may already be there. More is read only when the head is incomplete. Returns the head's
 * length, or -1 once the connection is finished (431 or 400 answered, or the peer went away).
 *   - what the previous attempt scanned  [size_t        already = 0;]
 *   - the peer is done: a clean end between requests  [return -1;]
 *   - in: room; out: count  [req->n_headers = IOXD_MAX_HEADERS;]
 *   - the head stays put, where it was parsed  [if (!ioxd__pipereader_keep(pr, (size_t)parsed))
 *     {]
 *   - the body's kept bytes are a run of their own  [ioxd__pipereader_run_begin(pr);]
 *   - malformed  [if (parsed == -1) {]
 *   - incomplete: the next read waits for more  [ioxd__pipereader_examine(pr, live.len);]
 */

/* starts_ci:
 * Does the slice begin with this literal, ignoring ASCII case?
 */

/* fill_request:
 * The rest of the request from its head: path and query (an absolute-form target loses its
 * scheme and authority first), the query split into params, the headers lower-cased and the
 * ones the engine needs picked out, and the framing settled. The body stays on the wire.
 * Returns 0, or the status the request must be refused with.
 *   - absolute-form: RFC 9112 3.2.2  [if (target.len && target.p[0] != '/' &&
 *     (starts_ci(target, "http:]
 *   - the router fills these  [req->n_route_params = 0;]
 *   - on demand: ioxd_body_all fills it  [req->body           = (ioxd_slice){ nullptr, 0 };]
 *   - part of the query would be missing: never act on part  [if (truncated)]
 *   - RFC 9112 3.2: exactly one Host on HTTP/1.1  [return 400;]
 *   - both framings: RFC 9112 6.1  [return 400;]
 *   - a transfer coding we do not implement  [return 501;]
 *   - chunked, but not as the last coding  [return 400;]
 */

/* init_body_state:
 * The engine's bookkeeping for reading the body on demand: where it starts, what already
 * arrived with the head, and - for a Content-Length body that is entirely here - where the
 * next request starts.
 */

/* init_response:
 * A response with its defaults and an empty slab.
 *   - headers[] is only read up to here  [res->n_headers      = 0;]
 */

/* ioxd__engine_serve:
 * The proactor handler for every connection: one request per iteration - get the head, run the
 * chain against a context, drain what it left of the body, send what it wrote - while kept
 * alive. Returning closes the connection.
 *   - decoded query parameters  [char params[IOXD_PARAM_CAP];]
 *   - this request's context  [ioxd_ctx           ctx;]
 *   - the framing cannot be trusted: answer, close  [if (refused) {]
 *   - middleware chain + endpoint  [ioxd__router_dispatch(&ctx);]
 *   - too large, malformed, or gone  [if (state.body_err) {]
 *   - never asked for: the client may not send it, so no drain  [ctx.res.close = true;]
 *   - what the handler left unread  [drain_body(&ctx);]
 *   - sends; suspends meanwhile  [if (finish(&ctx) < 0)]
 *   - this request's bytes go; a pipelined next one stays
 *     [ioxd__pipereader_release(&pipe->in);]
 */

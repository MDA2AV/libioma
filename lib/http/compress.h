/*
 * http/compress.h - the notes of compress.c, whose declarations are public (ioxd/compress.h)
 */
#pragma once

/* ── compress.c: the notes ─────────────────────────────────────────────────────────────── */

/*
 * compress.c - the compression middleware on the engine's two hooks. Its pre-phase does one
 * thing: when the request carries Accept-Encoding, it registers the delegate for the head.
 * That delegate runs at the first flush, with the status, the type, the headers and the size
 * known, decides, takes a coder from this worker's pool, and installs it as the reply's filter;
 * the engine then runs every flushed span through it. A coder is a brotli encoder over an arena
 * - brotli has no reset, so an instance is made per reply, and the arena makes its allocations
 * pointer bumps and its frees no-ops - or a zlib stream, reset per reply. Coders go back to the
 * pool when the reply ends, so a worker keeps as many as it had streaming at once.
 */

/* at file scope:
 *   - the codings, in the order they are preferred  [enum kind { KIND_BR, KIND_GZIP, KINDS };]
 *   - a coder: one reply's encoder state, pooled per worker  [struct coder {]
 *   - the arena a brotli instance allocates from: what fits is a bump, the rest malloc, and the
 *     peak is remembered so the arena is grown for next time  [char *arena;]
 *   - the pools, per worker: a free list per coding  [static _Thread_local struct coder
 *     *tl_free[KINDS];]
 */

/* arena_alloc / arena_free:
 * brotli's allocator: bump what fits, malloc what does not; free only what was malloc'd.
 */

/* coder_take / coder_give:
 * A coder from the pool or fresh, ready for a reply - brotli made with the size hint, so a
 * whole body gets tables sized for it; zlib reset - and back to the pool when the reply ends.
 */

/* br_run / gzip_run:
 * The filter: one call of the encoder with the engine's op, the counts moved to what was
 * consumed and produced, 1 while it holds more output than fit.
 */

/* compressible:
 * A media type worth coding: text, JSON, XML, JavaScript, SVG, WebAssembly, and any +json or
 * +xml.
 */

/* decide:
 * The delegate for the head: the gates, then the coding by the client's q-values, then the
 * filter and the headers.
 */

/* ioxd_compress:
 * The middleware: the delegate when there is an Accept-Encoding, then the chain.
 */

/*
 * http/static.h - the notes of static.c, whose declarations are public (ioxd/static.h)
 */
#pragma once

/* ── static.c: the notes ───────────────────────────────────────────────────────────────── */

/*
 * static.c - files from a directory, kept in memory per worker. A request resolves to a
 * relative name (percent-decoded, empty segments dropped, dot segments and hidden names refused,
 * a directory's index appended), which is looked up in the worker's table: a hit is checked
 * against the disk with one fstatat (inode, size, modification time), a miss is opened, read
 * whole through the ring - the worker serves its other connections meanwhile - with its .br and
 * .gz twins when those are wanted, and inserted, the least recently served entries dropped to
 * make room. The reply is the entry's bytes, written from where they are: a body larger than
 * the slab goes out behind the head in one message (ioxd_write), so a file costs no copy. Each
 * worker has its own table and no lock: the store only lists the tables, for close. A file
 * larger than the keep limit streams from the disk in slab-sized pieces instead.
 */

/* at file scope:
 *   - directories open at once: the thread-local slots  [#define ROOTS 16]
 *   - a relative name, NUL included  [#define NAME_CAP 1024]
 *   - a streamed file goes out in these  [#define PIECE 16384]
 *   - the variants of a file: as it is, its .br twin, its .gz twin  [enum variant { PLAIN, BR,
 *     GZIP, VARIANTS };]
 *   - one file a worker keeps  [struct entry {]
 *   - a worker's table, and the recency list through it  [struct cache {]
 *   - the slots: a store takes one for its life, and a closed store's generation moves on so a
 *     thread-local pointer left from it is seen as stale  [static pthread_mutex_t g_lock]
 */

/* type_of:
 * The content type from the extension of the last segment; bytes when unknown.
 */

/* now_ms:
 * The coarse monotonic clock: cheap, and all a revalidation interval needs.
 */

/* http_date / parse_date:
 * An IMF-fixdate written and read without the locale: the day and month names are tables.
 */

/* hash_of:
 * FNV-1a over the name.
 */

/* cache_of:
 * This worker's table for the store, made on first use and listed in the store for close. A
 * pointer left in the slot by a store closed since is recognised by its generation and dropped.
 */

/* lookup / touch / insert / unlink_entry / drop / release / make_room:
 * The table and its recency list. An entry is charged to the budget while it is in the table;
 * dropping it takes it out at once and frees it when no send is reading from it any more.
 */

/* read_at:
 * A read through the ring when on a worker coroutine - the worker serves others while the disk
 * answers - and pread otherwise.
 */

/* load:
 * A file read whole, with its twins when wanted, into an entry: kept when it fits the budget,
 * served once and freed otherwise.
 */

/* current:
 * Is what the entry holds still what the disk has? One fstatat, at most once per
 * revalidate_ms; the inode, size and modification time must all agree.
 */

/* resolve:
 * The request path under the mount as a relative name, or false for one that is refused.
 */

/* open_file:
 * The file, or a directory's index; -1 when neither is a regular file.
 */

/* coding_q / qvalue:
 * The weight Accept-Encoding gives a coding: named, its own q (1 when none), else that of "*",
 * else 0.
 */

/* pick:
 * The variant to serve: the twin with the highest q the client takes, br before gzip on a tie;
 * the plain file when neither.
 */

/* not_modified:
 * If-None-Match against the entity tag (any listed, "*", weak or strong), else
 * If-Modified-Since against the modification time.
 */

/* range_of:
 * One byte range of the representation: none for no Range, one the server chooses to ignore
 * (several ranges, an If-Range that names another version, a form it does not read), the
 * range, or a 416 for one that starts past the end.
 */

/* reply:
 * The headers, then 304, 416, 206 or 200 with the bytes written from the entry, which is held
 * while the send reads from it.
 */

/* stream:
 * A file too large to keep: the same headers and ranges, the bytes read in pieces through the
 * ring straight into the slab.
 */

/* serve:
 * A resolved name: the table first, then the disk.
 */

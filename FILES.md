# Static files over the ring (design, branch `streams`)

The model is ioxide.file's: small, hot files served from a baked, immutable snapshot; large files
as positional ring reads or, better, spliced from the descriptor straight into the socket - no
thread pool either way, and with kernel TLS the splice is encrypted on its way through.

## The snapshot

`ioxd_files_new("/srv/www")` walks the root once and builds a table keyed by URL path. Files up to
a threshold (256 KB) get their whole response baked: status line, `content-type` from the
extension, `content-length`, `etag` (inode, size, mtime), `last-modified`, then the body, in one
block, so serving one is a lookup and one send with nothing formatted. Larger files keep an open
descriptor and their `statx`; serving one is the head from the snapshot and the body by
`IORING_OP_SPLICE` from the descriptor through a pipe pair into the socket, in slices the writer
paces, or by positional `IORING_OP_READ` into the reply slab where splice is not wanted. Conditional
requests (`if-none-match`, `if-modified-since`) answer 304 from the snapshot alone. Ranges and
precompressed `.gz`/`.br` siblings are second-round work.

The router grows a tail capture, `/static/*path`, so a handler is one line:

    IOXD_GET("/static/*path", assets);   /* ioxd_files_serve(ctx, files, ctx->req.route_params[0].value) */

## Knowing a file changed

Watching the file itself misses the case that matters: an atomic replace is a `rename` over the
old name, a new inode, and the old watch dies with the old one. So the watch is on directories,
one per directory in the tree (inotify watches are cheap; an asset tree of a few hundred
directories is nothing), and the events that mean "the content under this name is different" are
`IN_CLOSE_WRITE` (a write in place finished), `IN_MOVED_TO` (the atomic replace landed),
`IN_CREATE` and `IN_DELETE`. The descriptor is read as an ordinary ring read by the same control
coroutine that watches certificates - it is one facility, `lib/io/watch.c`, with two clients.

Events are debounced, then the changed entries are confirmed with `statx` against the snapshot
(inode, size, mtime), re-baked, and a new snapshot is published: an atomic pointer with a
reference count, so a request that is mid-send from a baked block keeps the old snapshot alive
until its send completes, and the old snapshot is freed when the last lease drops. That is
ioxide.file's lease-safe reload, driven by the watch instead of only by `Reload()`, which stays
for deployments that swap the tree whole. `fanotify` could watch a whole mount with one
descriptor but needs privileges; inotify per directory does not. The fallback for filesystems
without events is the same `statx` sweep on a timer.

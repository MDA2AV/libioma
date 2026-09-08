# TLS: one way, in the kernel (design, branch `streams`)

## The shape

Kernel TLS handles the record layer of an ordinary socket - encrypt on send, decrypt on receive -
but not the handshake. So a TLS connection is a plain connection with a prologue:

1. **Handshake in userspace, over the pipe.** OpenSSL runs a TLS 1.3 server handshake through
   memory BIOs on the connection's coroutine: ciphertext from the pipe's reader goes into the read
   BIO, `SSL_do_handshake`, the write BIO is drained into the pipe's writer and flushed, repeat on
   `WANT_READ`. Handshake bytes ride the same ring as everything else and the handshake suspends
   like any await. Nothing above the pipe knows it happened.
2. **Keys into the socket.** The keylog callback hands us the TLS 1.3 traffic secrets (server for
   TX, client for RX); RFC 8446 HKDF-Expand-Label makes the key and the IV; `setsockopt(TCP_ULP,
   "tls")` attaches the ULP and `setsockopt(SOL_TLS, TLS_TX / TLS_RX)` installs them. Secrets are
   zeroed once programmed. This is ioxide.tls's handoff, in C.
3. **Both directions in the kernel from then on.** The pipe's multishot recv delivers plaintext into
   the provided buffers; the writer's sends are encrypted by the kernel; `splice` from a file
   descriptor is encrypted too, which is what makes zero-copy file serving over TLS possible.
   The HTTP engine, the reader and the writer are untouched.

"One way" means exactly this: TLS 1.3, one AEAD suite the kernel knows (TLS_AES_128_GCM_SHA256;
AES-256-GCM and ChaCha20-Poly1305 are one table entry each), no session tickets (a ticket would
advance the record sequence after the handshake and break the handoff), no 0-RTT, no client
certificates in the first cut. No userspace record path exists, so there is nothing to fall back
to: a client that cannot do this suite fails the handshake.

**The receive handoff, precisely.** A TLS 1.3 client may send its first request in the same
segment as its Finished, so when the handshake completes the reader may hold ciphertext past it.
Kernel RX can only start at a record boundary in the socket's own queue, so before installing RX
the prologue feeds whatever the reader holds through OpenSSL - `SSL_read` - until it ends on a
record boundary (waiting for the tail of a split record if it has to), hands the plaintext it got
to the reader as if it had just arrived, and installs `TLS_RX` with the record sequence those
records consumed. A control record after the handoff - close_notify, or a KeyUpdate the kernel
cannot honour - surfaces as `-EIO` on the recv, which the connection treats as end of input.
Browsers do not send KeyUpdate; that is the accepted limit.

## Certificates: files on disk, SNI, a default, rotation

- **Layout.** A directory per listener: `<dir>/<hostname>/cert.pem` (the chain) and `key.pem`, plus
  `<dir>/default/` for no SNI or no match. Matching is exact hostname, then one wildcard level
  (`*.example.com` as the directory name `_.example.com`).
- **SNI.** `SSL_CTX_set_client_hello_cb` reads the server-name extension, looks the host up, and
  switches the connection's context with `SSL_set_SSL_CTX`. Unknown or absent: the default.
- **Rotation without a restart.** One control coroutine on worker 0 watches the directory tree
  with inotify - `IN_CLOSE_WRITE` for a file rewritten in place, `IN_MOVED_TO` for the atomic
  replace (`rename` over the old file; a watch on the file itself would be lost with the inode),
  `IN_CREATE`/`IN_DELETE` for hosts added and removed - read off the inotify descriptor as an
  ordinary ring read. Events are debounced (500 ms: editors and certbot touch a directory several
  times), then each changed host is confirmed with `statx` (inode, size, mtime against what is
  loaded), parsed, checked (the key matches the certificate, the chain parses, not yet expired),
  and built into a new `SSL_CTX`. The new table is published with an atomic pointer and a
  generation; every handshake takes a reference to the table it started with, so an in-flight
  handshake finishes on the old certificate and the old table is freed when the last reference
  drops. A `statx` sweep every 60 s is the safety net for filesystems where inotify is silent
  (NFS, some bind mounts), and `ioxd_tls_reload()` / SIGHUP force it for deployments that prefer
  to say when. A rejected file (bad key, mismatch) is logged and the old certificate stays.

## Where it plugs in

Listeners become explicit, one per port and per worker, so plain and TLS can coexist and more than
one port can be served (ioxide's multi-port):

    ioxd_tls *tls = ioxd_tls_new("/etc/ioxd/certs");     // reads the tree, starts watching
    ioxd_listen(8080, NULL);                             // plain
    ioxd_listen(8443, tls);                              // TLS
    ioxd_run(0);                                         // workers over every listener

Per connection, `conn_main` runs the prologue when its listener has a TLS context, then the same
handler as always. `ctx->req` gains the negotiated server name and the fact that the connection is
TLS, for handlers and redirects. The dependency is libssl at handshake time; the data path is the
kernel's. Test surface: the fixture on a self-signed default certificate plus one SNI host; the
smoke suite through Python's `ssl` module; a rotation test that rewrites `cert.pem` atomically and
checks the next handshake presents the new one.

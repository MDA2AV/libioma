# TLS: one way, in the kernel (design, branch `streams`)

**Status.** Built: `lib/tls/store.c` (the store, SNI, `ioxd_certs_reload`, `ioxd_certs_free`) and
`lib/tls/handshake.c` (the prologue and the handoff); `ioxd_bind(port, ioxd_certs_load(dir))`.
Built by default; `make TLS=0` or `-DIOXD_TLS=OFF` leaves it out, and `ioxd_certs_load` then says so
and returns NULL. Verified by the smoke suite through
Python's `ssl` (default certificate, SNI, the `_.example.com` wildcard, an unknown name, a POST
body and a 1 MB upload through kernel RX, keep-alive, a 3000-object reply through kernel TX, a
refused TLS 1.2 client, and a reload while it serves), by
`tests/tls_early.py` with tlslite-ng at the wire level (the request in the same TCP write as the
client's Finished, whole and with its record cut in two with a pause; two early requests; a
close_notify; a corrupted record), by six of tlsfuzzer's TLS 1.3 scripts (`make check-tlsfuzzer`), and
by a testssl.sh scan (TLS 1.3 only, forward-secret AEAD only). The certificates the suites use come
from `tests/mkcerts.sh`, which makes three short-lived self-signed hosts - `default` (RSA),
`sni.test` and the wildcard `_.example.com` (ECDSA) - and remakes one that is within three days of
expiring.

Kernel TLS programs the socket with `setsockopt`, and the plane sends that over the ring
(`IORING_OP_URING_CMD` with `SOCKET_URING_OP_SETSOCKOPT`): under the registered file table, which
is the default, a connection is a slot index and not a descriptor `setsockopt(2)` could be called
on. So a TLS listener wants a kernel that knows that command, 6.7 or newer - or a build with
`-DFIXED_FILES=0`, where the plain call is the fallback.

Known limits, all deliberate for now:
one suite, TLS_AES_128_GCM_SHA256 - AES-256-GCM and ChaCha20-Poly1305 are one table entry each in
the store and one key-size case in the handoff; no tickets, no 0-RTT, no client certificates; the
server sends close_notify when it closes, but a control record from the peer after the handoff
(an alert, a KeyUpdate, an over-long record) ends the connection without an alert of ours, since
the plain recv only reports it as an error - seeing record types would mean recvmsg with control
data on the multishot recv. Still open: the rotation watcher below (FILES.md shares it), and
letting `ctx->req` carry the negotiated server name and the fact that the connection is TLS, for
handlers and redirects - neither is built.

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
   zeroed once programmed. This is ioxide.certs's handoff, in C.
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
OpenSSL is therefore fed exactly one record per call all the way through the handshake: a whole
record is assembled out of the reader and written into the read BIO, and nothing more, so what the
client sent behind its Finished is still in the reader when the handshake ends rather than
swallowed by the BIO.

Kernel RX can only start at a record boundary in the socket's own queue, so before installing RX
the prologue stops the multishot recv and drains what is left. The drain assembles records into a
scratch buffer of its own, one at a time, never waiting: while records are there they go through
OpenSSL and the plaintext is kept aside; when nothing delivered is left, the socket is at a record
boundary and the drain is done. The tail of a record the pause cut in two is fetched straight from
the socket (`ioxd__recv_exact`). The plaintext is then handed to the reader as if it had just
arrived, and `TLS_RX` is installed with the record sequence those records consumed.

Three things end the connection there, each with a line saying which: a record that is not
application data (an alert, or a handshake message), a post-handshake message OpenSSL wants to
answer - a KeyUpdate, seen as bytes appearing in the write BIO, which the handoff cannot follow -
and early plaintext larger than the reader can hold, since the plaintext has to fit the reader's
gathering buffer (`IOXD_PIPE_GATHER`, 16 KB). A client's close_notify during the drain is not a
failure: the records before it are still served, and the connection then ends normally.

After the handoff the kernel owns the records, and a control record from the peer - close_notify,
or a KeyUpdate the kernel cannot honour - surfaces as `-EIO` on the recv, which the connection
treats as end of input. Browsers do not send KeyUpdate; that is the accepted limit.

## Certificates: files on disk, SNI, a default, a reload

- **Layout.** A directory per listener: `<dir>/<hostname>/cert.pem` (the chain) and `key.pem`, plus
  `<dir>/default/` for no SNI or no match. Matching is exact hostname, then one wildcard level
  (`*.example.com` as the directory name `_.example.com`). `default` is required: without it there
  is nothing to answer a name we do not have, so `ioxd_certs_load` says so and returns NULL. Only a
  reload may go without one, and only by carrying the host that was already answering forward.
- **What a host must be to load.** The key has to match the certificate and the chain has to parse,
  and the certificate's validity dates are checked as well: one that has expired, or that does not
  start until later, is a load failure like any other. A key file readable past its owner is
  logged, once per host per load.
- **SNI.** `SSL_CTX_set_client_hello_cb` reads the server-name extension, looks the host up, and
  switches the connection's context with `SSL_set_SSL_CTX`. Unknown or absent: the default. The
  callback goes on a context once, when it is built, and finds the table through the SSL that its
  handshake holds a reference to - not through the context - because a reload shares a carried-over
  context between two tables, and a published `SSL_CTX` is never written to again.
- **Reload.** `ioxd_certs_reload(tls)` reads the directory again and switches to what it holds. It is
  manual: nothing watches the tree, and nothing sends it a signal. Reloads serialise against each
  other, and the table is reference-counted - every handshake takes a reference to the table it
  started with - so a handshake in flight finishes on the certificate it began with and the old
  table is freed when the last reference drops. A host that fails to load keeps the context it was
  serving, so an expired replacement changes nothing but a line on stderr; the host answering for
  unmatched SNI keeps answering. It returns -1 when nothing at all could be loaded, and what was
  serving still is. `ioxd_certs_free(tls)` gives the store back once no handshake holds a table of
  it: for a store never listened on, or for after `ioxd_run` has returned.

## Where it plugs in

Ports are bound one by one, each plain or with a store, and every worker opens all of them, so
plain and TLS coexist and more than one port can be served (ioxide's multi-port):

    ioxd_certs *tls = ioxd_certs_load("/etc/ioxd/certs");     // reads the tree
    ioxd_bind(8080, NULL);                               // plain
    ioxd_bind(8443, tls);                                // TLS
    ioxd_run(0);                                         // workers over every bound port

Per connection, `ioxd__conn_main` runs the pipe's handler, and `run.c` puts the prologue in front
of it when the listener has a TLS store; a close_notify goes out when the handler is done. The
dependency is libssl at handshake time; the data path is the kernel's. The test fixture
(`tests/server.c`) serves `IOXD_CERTS` on the port two above its own and exposes
`POST /tls/reload`, which is how the smoke suite rewrites `sni.test`'s files, checks that the old
certificate is still served until the reload, and checks that a connection open across it keeps
answering.

## Planned: rotation without a restart

Not built. One control coroutine on worker 0 would watch the directory tree
with inotify - `IN_CLOSE_WRITE` for a file rewritten in place, `IN_MOVED_TO` for the atomic
replace (`rename` over the old file; a watch on the file itself would be lost with the inode),
`IN_CREATE`/`IN_DELETE` for hosts added and removed - reading the inotify descriptor as an
ordinary ring read. Events would be debounced (500 ms: editors and certbot touch a directory
several times), then each changed host confirmed with `statx` (inode, size, mtime against what is
loaded) before the reload the store already does. A `statx` sweep every 60 s is the safety net for
filesystems where inotify is silent (NFS, some bind mounts), and a SIGHUP would force one for
deployments that prefer to say when. `ioxd_certs_reload` is what all of that would call; today it is
the whole of it, and the application decides when. FILES.md shares the same watch facility.

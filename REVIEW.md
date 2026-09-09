# The review of 2026-09-09

Sixteen independent reviewers, one per area, read the whole tree at commit 1f00207 (branch
`streams`, the state after TLS shipped) with the brief "only what you verified by reading the
code; quote the line". Their findings were consolidated, verified again where the fix depended on
it, and fixed in the commits that follow 1f00207. This is the record: what was found, what was
done about it, and what was deliberately left.

Severity is the reviewer's, kept even where the fix turned out to be small. "Fixed" means the
code changed and a test exercises the new behaviour; the test suites are named in README.md.

## The ones that mattered most

- **A request sent in the same segment as the client's TLS Finished was lost** (critical). The
  handshake loop wrote every delivered byte into OpenSSL's read BIO; an application record that
  arrived with the Finished vanished into it, the drain found nothing, and kernel RX was installed
  one sequence number behind - the connection hung. The test meant to catch this held the
  Finished through `send()`, but tlslite sends it through `sendall()`, so its three coalescing
  cases never coalesced. Fixed: one record per feed, the drain assembles records in its own
  scratch; the test holds both calls and fails on the old code. `lib/tls/handshake.c`,
  `tests/tls_early.py`.
- **The public headers were never in the repository.** `.gitignore` had a bare `ioxd` line for
  an old binary; it also matched `include/ioxd/`, so `slice.h`, `http.h`, `router.h`, `json.h`,
  `pipe.h` and `tls.h` were never committed and a fresh clone did not build. Found by the fix
  agents, not the reviewers. Fixed: `/ioxd`, headers added.
- **Request smuggling through the framing headers** (critical, four ways). `Content-Length`
  stopped at the first non-digit and did not check overflow (`+5`, `0x5`, `5abc`, 2^64+5 all
  read as something else), duplicates were last-wins, `Transfer-Encoding` was last-wins and
  matched `chunked` anywhere in the list, both framings together were accepted, a folded
  continuation line was skipped, Host was never checked. Every one verified live as a desync
  behind a proxy. Fixed with 400 (501 for a transfer coding we do not implement) and close;
  `tests/conformance.py` sends each. `lib/http/engine.c`.
- **Response splitting through `ioxd_header`** (critical). Values went on the wire byte for
  byte, and a percent-decoded `%0d%0a` in a query value became a second header line. Fixed:
  names must be tokens, values may hold no control byte, the engine's own headers cannot be set,
  and both are copied into a per-reply arena - the pointers used to be read after the handler's
  frame was gone, which sent stack garbage. `lib/http/api.c`, `include/ioxd/http.h`.
- **HEAD, 204 and 304 carried bodies** (critical), and `HEAD` of a GET route was a 405. Fixed:
  the router serves HEAD from GET, the engine sends the head alone (Content-Length as the GET
  would have had; no framing header on a 1xx or 204). `lib/http/router.c`, `lib/http/engine.c`.
- **A kept pointer from `ioxd_pipe_keep` could dangle** (critical, public pipe API only). A run
  started in a fresh kernel buffer was copied out by `gather` and the buffer went back to the
  ring, where another connection's recv refilled it. Fixed: the buffer is pinned until release.
  `lib/io/pipe.c`.
- **A paused recv was stranded under buffer starvation** (high, three reviewers). The
  `-ENOBUFS` completion never looked at `pausing`, so a TLS handshake that hit starvation parked
  its coroutine forever, leaking the connection. Fixed, along with the rest of the pause/resume
  corners (eof, closed, a stale cancel). `lib/io/conn.c`.
- **Shutdown abandoned every live connection** (high). The loop exited on the stop flag, parked
  coroutines never unwound, their stacks and sockets leaked, and the recv slab was unmapped while
  recvs could still land in it; `ioxd_run` returned 0 and could not be called again. Fixed: a
  drain (accepts cancelled, one `ASYNC_CANCEL_ANY`, run until nothing is live or 2 s pass), the
  stop flag and listener table reset per run, a worker failure returned. `lib/io/proactor.c`,
  `lib/http/run.c`.
- **A reload could free a table a stalled handshake still used** (critical, not yet reachable:
  nothing called reload). The ClientHello callback's argument was re-pointed on an `SSL_CTX`
  shared between two tables. Fixed: the table rides on the SSL. Also: certificates are checked
  for validity dates, a missing `default` is an error, reloads serialise. `lib/tls/store.c`.

## By area

### I/O plane (uring, bufring, conn, proactor, coro)

| Sev | Finding | Status |
|---|---|---|
| high | mmap failure in `uring_init` left the struct populated; `uring_exit` double-freed | fixed |
| high | persistent accept error (-ENFILE with the file table full) spun re-arming at 100 % CPU | fixed: stalled listeners re-arm when connections drop or after a second; the table is a ceiling |
| high | `ioxd__sqe` ignored `uring_submit`'s -EBUSY and aborted the process | fixed: the CQ head is published before a mid-batch enter, which retries with GETEVENTS |
| high | slab unmapped with recvs in flight; live connections abandoned at stop | fixed: the drain |
| high | one 4 KB guard page below 25 KB frames, no stack probes in our own flags | fixed: 64 KB guard, `-fstack-clash-protection`, 128 KB stacks |
| high | `swap_ctx` and `coro_*` exported and interposable from the .so | fixed: hidden, plus a version script exporting `ioxd_*` only |
| medium | CQ overflow invisible | fixed: the flag forces an enter; overflows counted in the stop line |
| medium | starved sweep re-armed every parked connection per returned buffer | fixed: at most as many as were returned, FIFO |
| medium | fatal enter error retired one worker silently | fixed: stop set, `ioxd_run` non-zero |
| medium | no CFI in the context switch; loop-only invariant was an `assert` | fixed |
| medium | `BUF_COUNT` bound one power of two too permissive; compat `#ifndef`s missing for the newer flags | fixed |
| medium | pooled stacks never trimmed | left: warm stacks are the point of the pool (`CORO_POOL_MAX` bounds it); noted in coro.c |
| medium | a stack per accepted connection before any handler runs | left: pooled, and admission is now bounded by the file table |
| low | TAG_OP was 0 (a forgotten user_data dispatched as a null op); direct close untagged; `fd > 0`; banner snprintf; dangling `starved` | fixed |
| low | MXCSR/x87 not switched; CET off for the whole program by the switch's missing note | documented in coro.h and switch_x86_64.S |
| low | setsockopt fallback dead under registered files | documented: kernel TLS needs `SOCKET_URING_OP_SETSOCKOPT` (6.7+) or `FIXED_FILES=0` |

### Pipes

| Sev | Finding | Status |
|---|---|---|
| critical | kept pointers dangled after `gather` | fixed: the buffer is pinned |
| high | `inject` with a run held in `cur` broke the live-in-buf invariant | fixed |
| high | the gathering buffer (16 KB) is smaller than a maximal TLS record | fixed on the TLS side: records are assembled in the prologue's scratch, never in the reader |
| medium | `drop` past the live bytes, `keep` of more than is live, `advance` past the slab | fixed: clamped |
| low | `avail` collapsed error and empty; `inject` ignored a sticky error; a raw handler's unsent slab was dropped at close | fixed |

### HTTP engine, request side

| Sev | Finding | Status |
|---|---|---|
| critical | `Content-Length` parse, duplicates, `Transfer-Encoding` last-wins, both framings | fixed: 400/501 and close |
| high | obs-fold skipped; Host never checked | fixed |
| medium | trailers unbounded; absolute-form targets 404; `Connection` last-wins | fixed |
| low | bare whitespace after a chunk size; `int` overflow on huge reads; bytes copied lost on a chunk-end error | fixed |
| low | `ioxd_kv_parse` dropped pairs silently past its limits | fixed: `truncated` out-parameter; the engine answers 400/414 |

### HTTP engine, reply side

| Sev | Finding | Status |
|---|---|---|
| critical | header values unvalidated; HEAD/204/304 bodies | fixed |
| high | handler-set `content-length`/`transfer-encoding` reached the wire; a declared length never reconciled | fixed: refused; buffered replies get the real length, streams are cut at it and close when short |
| medium | framing headers on 1xx/204; status not validated; no `100 Continue`; drain cap decided after the head froze | fixed |
| low | a head past its cap closed silently; `ioxd_advance` unbounded; OOM in `ioxd_printf` not sticky | fixed |

### Router

| Sev | Finding | Status |
|---|---|---|
| high | the 405 `allow` list omitted methods reachable through a capture route | fixed: the union of every path node reached, HEAD after GET |
| high | four registration functions lacked the after-`ioxd_run` guard | fixed |
| medium | `break` inside `IOXD_GROUP` left the group open; prefix and path glued without a slash; 17 middleware silently dropped one; captures raw while `params` are decoded | fixed |
| low | caller strings kept by pointer; `ioxd_next_run` twice replayed the chain; allow behind group middleware | fixed / fixed / documented |

### Slices, conversions, helpers

| Sev | Finding | Status |
|---|---|---|
| critical | `ioxd_header` accepted CRLF (see above) | fixed |
| medium | `%00` decoded to a NUL and `ioxd_cstr` reported success on it | fixed: `%00` stays literal, `ioxd_cstr` refuses an embedded NUL |
| low | NULL C strings crashed; a locale failure fell back to the process locale; 128-byte numbers refused | fixed / fixed / left (documented) |

### JSON

| Sev | Finding | Status |
|---|---|---|
| high | a multi-byte decimal point corrupted every double; an unbalanced `end` went unreported | fixed: a private C locale; `failed` set; `ioxd_json_done` |
| medium | no level rules (a key in an array, a value without a key); `float` printed as a double; the header example's `//` swallowed macro lines | fixed |
| low | depth 64 aliased the root level; a trailing comma on a too-deep open; empty `raw`; refused reserves not retried | fixed |

### TLS

| Sev | Finding | Status |
|---|---|---|
| critical | early data lost with the Finished (see above); the reload callback argument | fixed |
| high | early plaintext of 64 KB could not be delivered after the keys were installed; a split record could be spliced wrongly | fixed: bounded by the reader, assembled in scratch |
| high | no validity-period check on load | fixed |
| medium | close_notify in the drain window lost the request before it; a KeyUpdate desynchronised silently; reload raced itself; the fallback host followed `readdir` order | fixed |
| low | SNI case-fold matched control bytes; a trailing dot; error queue leftovers; no `ioxd_tls_free`; key file permissions | fixed |

### Public API and documentation

| Sev | Finding | Status |
|---|---|---|
| critical | the limits that size `ioxd_ctx` could be redefined by an application | fixed: `ioxd_run` passes `sizeof(ioxd_ctx)` and the library refuses a mismatch |
| high | `ioxd_header` pointer lifetime; strings kept by the router | fixed: copied |
| medium | `ioxd_run` one-shot; `pthread_create` failure path; comments naming members and functions that do not exist; the umbrella header's example did not compile | fixed |
| medium | README said "no TLS", lacked the prerequisites; `IOXD_REQ_CAP`; the rotation watcher described as built; DESIGN.md claiming to describe what shipped | fixed in the docs pass |

### Tests and build

| Sev | Finding | Status |
|---|---|---|
| critical | no negative framing tests at all | fixed: `tests/conformance.py` |
| high | an assertion that could not fail; the starvation test never starved; the fixture's exit status discarded; `TLS=0` not a build dependency; the CMake package unusable downstream | fixed: `check-tiny`, `tests/run-suites.sh`, an `obj/flags` stamp, `find_dependency(OpenSSL)` |
| medium | assertions that passed on a hang; untested public functions; wildcard SNI and reload untested; `ctest` ran one test | fixed |
| low | no `make tidy`; `concurrency-*` off | fixed |

## What the reviewers confirmed correct

The io_uring memory ordering, the two-owner reference count on a connection, the buffer-ring
contract and every buffer's path back to the ring, the context switch and its forged frame, the
chunked framing of replies, the front/back arithmetic of the writer, the key schedule and nonce
split of the TLS handoff, the SNI parser under ASan, the JSON escaping and number formatting, and
the conversions under four million fuzzed inputs. The defects were at the edges: failure paths,
shutdown, what a handler may hand the engine, and the framing a peer declares.

## Cost

The hardening costs about 1 % on the saturated keep-alive benchmark (PERF.md). It is the price
of checking what a request declares before acting on it.

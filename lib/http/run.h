/*
 * http/run.h - the notes of run.c, whose declarations are public
 */
#pragma once

/* ── run.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * run.c - ioxd_run: one proactor thread per core serving HTTP, until SIGINT/SIGTERM.
 */

/* at file scope:
 *   - The configuration for the runs that follow: what ioxd_configure was given, zeros where
 *     the default is wanted. Validated on the way in, so the run never sees a bad value.
 *   - The ports bound before the run, each with its store or none; every worker opens all of
 *     them.  [static struct listener g_listeners[IOXD_MAX_LISTENERS];]
 */

/* on_signal:
 * SIGINT/SIGTERM: raise the flag every worker loop polls.
 */

/* worker_thread:
 * pthread entry: the worker's whole life.
 */

/* cpu_count:
 * CPUs this process may run on (its cpuset), so the default is one worker per available core -
 * never a fixed count that would oversubscribe a small cpuset.
 */

/* raise_nofile:
 * Lift the soft fd limit to the hard one: open connections and the registered file table are
 * both checked against it, and the default soft limit is often 1024.
 */

/* effective_config:
 * The configuration a worker gets: what was set, the build's defaults for the rest.
 */

/* run_workers:
 * Worker threads (workers <= 0: one per available core), one proactor each, running `handler`
 * on every connection of every bound port until SIGINT/SIGTERM. What ioxd_run and
 * ioxd_run_pipes share.
 *   - a previous run's signal must not stop this one at once  [g_stop = 0;]
 *   - the ones already running must retire, not serve on alone  [g_stop = 1;]
 *   - a worker whose ring died: the run did not succeed  [if (ws[i].failed)]
 */

/* prologue:
 * A TLS listener's connection runs the handshake before its handler; a plain one goes straight
 * in.
 */

/* ioxd__run:
 * ioxd_run, through the header's inline: the caller's sizeof(ioxd_ctx) must be ours, or the
 * limits that size it (IOXD_MAX_HEADERS and friends) were redefined on one side and every
 * handler would read the context at the wrong offsets.
 *   - the routes, resolved once, shared read-only  [ioxd__router_build();]
 */

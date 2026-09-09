// ReSharper disable CppRedundantInlineSpecifier

/*
 * ioxd/run.h - the run: bind the ports, plain or TLS, then start the workers - serving HTTP, or
 * a pipe handler of your own.
 */
#pragma once

#include <stddef.h>

#include "ioxd/http.h"
#include "ioxd/pipe.h"

/* Bind a port: plain when certs is NULL, TLS 1.3 terminated in the kernel otherwise, with the
 * certificate store from ioxd_certs_load (ioxd/tls.h). Every bound port serves the same routes,
 * or the same pipe handler; bind as many as you need (at most 8), then run. -1 if refused: a bad
 * port, or the table is full. */
typedef struct ioxd_certs ioxd_certs;
int ioxd_bind(int port, ioxd_certs *certs);

/* Start `workers` proactor threads (<= 0: one per core) serving HTTP on every bound port, and
 * block until SIGINT/SIGTERM. Returns 0 on clean shutdown, non-zero when nothing was bound, when a
 * port could not be opened, when a worker failed, or when the limits in ioxd/http.h differ
 * between this header and the library (the context would not match). May be called again after
 * it returns; the ports stay bound. */
int ioxd__run_http(int workers, size_t ctx_size);
static inline int ioxd_run(int workers)
{
    return ioxd__run_http(workers, sizeof(ioxd_ctx));
}

/* The same, without HTTP: every connection on every bound port is handed to fn as a pipe. */
int ioxd_run_pipes(int workers, ioxd_pipe_handler fn);

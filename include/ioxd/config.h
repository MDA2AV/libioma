/*
 * ioxd/config.h - the runtime's knobs: the ring, the receive buffers, the coroutine stacks and
 * the pools, per worker. Set once before ioxd_run; the build's values are the defaults.
 */
#pragma once

#include <stddef.h>

/* Every field is per worker, and a zero field keeps its default. The receive buffers are what
 * the kernel delivers into, so their count bounds how much may be in flight before recvs park
 * on -ENOBUFS (the log says "raise recv_buffers" when that happens) and their size bounds what
 * one delivery holds; a worker's slab is count x size bytes. */
typedef struct ioxd_config {
    unsigned ring_entries;      /* submission queue entries, the completion queue twice that; a power of two, at most 32768; 8192 */
    unsigned recv_buffers;      /* provided receive buffers; a power of two, at most 32768; 1024 */
    unsigned recv_buffer_size;  /* bytes in each, 64 to 1 MB; 16384 - a whole TLS record, a body of 10 KB in one delivery */
    size_t   stack_size;        /* a connection's coroutine stack, above a 64 KB guard; at least 64 KB; 128 KB */
    unsigned idle_stacks;       /* stacks kept warm for the next connections; 512 */
    unsigned idle_connections;  /* connection records kept warm; 1024 */
} ioxd_config;

/* Apply a configuration to the runs that follow. -1, with the reason on stderr, when a value is
 * refused - nothing changes then. ioxd_configure(&(ioxd_config){ 0 }) is the defaults. */
int ioxd_configure(const ioxd_config *config);

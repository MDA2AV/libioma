/*
 * http/find.h - the substring search behind ioxd_slice_find, one function per instruction set,
 * so the unit test can run every one on the machine it has. Private; not installed.
 */
#pragma once

#include <stddef.h>

/* The index of the first needle in hay, or -1. Every one wants 2 <= n <= len: the public entry
 * has already answered the empty, the single-byte and the too-long needle. */
ptrdiff_t ioxd__find_scalar(const char *hay, size_t len, const char *needle, size_t n);   /* memchr on the first byte, then the rest */
#ifdef __x86_64__
ptrdiff_t ioxd__find_sse2  (const char *hay, size_t len, const char *needle, size_t n);   /* 16 positions a step                      */
ptrdiff_t ioxd__find_avx2  (const char *hay, size_t len, const char *needle, size_t n);   /* 32; only where __builtin_cpu_supports("avx2") */
#endif

/* ── find.c: the notes ─────────────────────────────────────────────────────────────────── */

/*
 * find.c - IndexOf for slices. The vector search is the first-and-last-byte one: the needle's
 * first byte is compared against a block of positions and its last byte against the block
 * shifted by the needle's length, and only a position where both hit is compared in the
 * middle - so a block costs two loads, two compares, an and and a movemask, and the memcmp runs
 * only at candidates. The last block of a haystack overlaps the one before it rather than
 * falling back to a byte loop; a haystack too short for a block, but holding at least one, is
 * searched by its first byte alone under a mask; shorter than that is the scalar loop.
 */

/* ioxd__find_scalar:
 * memchr hops to each first byte; the last byte and the middle decide.
 */

/* load16 / mask16 / block16:
 * The vector code is GCC's vector extension plus the movemask builtin, not the intrinsics
 * header - the same pcmpeqb, pand and pmovmskb, and a header fewer to parse. A block of 16
 * positions from i: the candidates where first and last byte both match, each tried in order,
 * so the first match is the lowest index.
 */

/* ioxd__find_sse2:
 * Blocks of 16 positions; the last one overlaps its predecessor rather than tailing off into a
 * byte loop. A haystack of at least 16 bytes whose positions do not fill a block is searched by
 * the first byte alone, the positions past the last valid start masked off.
 */

/* ioxd__find_avx2:
 * The same with 32 positions a step, for the CPUs that have it; fewer positions than a block
 * go to the SSE2 search, which handles the short cases.
 */

/* find:
 * The one the CPU has. __builtin_cpu_supports reads a table libgcc filled before main.
 */

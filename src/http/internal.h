/*
 * http/internal.h - what the HTTP plane's files share with each other. Private; not installed.
 * The plane sits on the I/O plane's interface (io/proactor.h): connections and the awaits.
 */
#pragma once

#include "ioma.h"
#include "io/proactor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void ioma__serve(conn_t *c);                      /* engine.c: the per-connection HTTP loop  */
void ioma__dispatch(ioma_ctx *c);                 /* router.c: middleware chain + endpoint   */

/* ── ASCII helpers ─────────────────────────────────────────────────────────────────────── */

/* Fold A-Z to a-z; every other byte unchanged. */
static inline unsigned char lower_ascii(unsigned char a)
{
    return (unsigned)(a - 'A') < 26u ? (unsigned char)(a | 0x20) : a;
}

/* Case-insensitive equality of two slices: a length test, then a tight byte loop. No libc, no
 * locale - it runs several times per request. */
static inline bool eq_ci(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn)
        return false;
    for (size_t i = 0; i < an; i++)
        if (lower_ascii((unsigned char)a[i]) != lower_ascii((unsigned char)b[i]))
            return false;
    return true;
}

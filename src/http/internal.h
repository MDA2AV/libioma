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

void ioma__serve(conn_t *conn);                   /* engine.c: the per-connection HTTP loop  */
void ioma__dispatch(ioma_ctx *ctx);               /* router.c: middleware chain + endpoint   */

/* The value of a hex digit, or -1. */
static inline int ioma__hexval(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20U;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

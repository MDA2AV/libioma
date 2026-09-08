/*
 * http/internal.h - the helpers the HTTP plane's files share and no module owns. Private; not
 * installed. Each module's own entries are in its header (engine.h, router.h).
 */
#pragma once

/* The value of a hex digit, or -1. Percent-decoding (api.c) and chunk sizes (engine.c). */
static inline int ioma__hexval(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20U;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

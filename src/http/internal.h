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

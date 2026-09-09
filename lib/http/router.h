/*
 * http/router.h - the router's entries for the runner and the engine: resolve everything
 * registered once, before the workers start; then dispatch each request through the result.
 */
#pragma once

#include "ioxd.h"

void ioxd__router_build(void);                    /* the segment tree and the flat chains, once */
void ioxd__dispatch(ioxd_ctx *ctx);               /* the request's endpoint, behind its chain    */

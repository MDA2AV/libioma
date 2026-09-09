/*
 * http/engine.h - the HTTP/1.1 engine's one entry for the runner: the per-connection loop.
 */
#pragma once

#include "io/pipe.h"

void ioxd__serve(struct ioxd_pipe *pipe);         /* requests on the connection until it ends */

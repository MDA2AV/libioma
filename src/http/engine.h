/*
 * http/engine.h - the HTTP/1.1 engine's one entry for the runner: the per-connection loop.
 */
#pragma once

#include "io/conn.h"

void ioma__serve(conn_t *conn);                   /* requests on the connection until it ends */

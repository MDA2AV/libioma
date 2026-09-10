/*
 * ioxd.h - the libioxd API: what you write endpoints against. Include this one; it brings in the
 * parts under ioxd/, one per concern, which are installed beside it.
 *
 * Every request gets a context: the request, the response, and a slot for your own data. The
 * context is passed to each middleware and to the handler, and any of them may read the request
 * and shape the response. The request is all the data, as slices into the read buffer; its body
 * is read from the wire only when asked for (whole, or streamed), and whatever is left unread is
 * drained after the handler. The response holds the write slab: a handler writes the body into
 * it, and the framework sends the head in front - in one send when it fits, streamed when not.
 * The handler runs on the connection's coroutine, so a read or a write that has to touch the
 * wire simply suspends it until the I/O completes.
 *
 *     static void user(ioxd_ctx *ctx) {
 *         ioxd_slice id = ctx->req.route_params[0].value;              // the :id of "/users/:id"
 *         ioxd_printf(ctx, "user %.*s\n", (int)id.len, id.p);
 *     }
 *     int main(void) {
 *         IOXD_GET("/users/:id", user);                       // or ioxd_route(NULL, "GET", "/users/:id", user)
 *         ioxd_bind(8080, NULL);                              // plain; ioxd_bind(8443, ioxd_certs_load("certs")) for TLS
 *         return ioxd_run(0);                                  // one worker per core
 *     }
 */
#pragma once

#include "ioxd/slice.h"    /* slices, conversions, key/value parsing              */
#include "ioxd/config.h"   /* the runtime's knobs, set before the run              */
#include "ioxd/http.h"     /* request, response, context, body, reply             */
#include "ioxd/router.h"   /* groups, endpoints, middleware; the script macros    */
#include "ioxd/json.h"     /* JSON written as you go; structs described once      */
#include "ioxd/pipe.h"     /* a connection as a pipe, for other protocols         */
#include "ioxd/run.h"      /* bind the ports, run the workers                     */
#include "ioxd/timer.h"    /* a delay that parks the connection, not the worker    */
#include "ioxd/socket.h"   /* outbound connections, as pipes                       */
#include "ioxd/tls.h"      /* certificates, for a TLS listener                    */
#include "ioxd/quic.h"     /* a QUIC port: its streams as pipes                     */

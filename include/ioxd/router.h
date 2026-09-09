/*
 * ioxd/router.h - groups, endpoints and middleware, and the same as a script.
 */
#pragma once

#include <stddef.h>

#include "ioxd/http.h"

/* ── routing ───────────────────────────────────────────────────────────────────────────── */

/* Endpoints live in groups, and groups nest. A group is a path prefix plus middleware: an
 * endpoint "/users" in a group "/api" under a group "/v1" answers at "/v1/api/users", wrapped by
 * the middleware of every group above it, outermost first, then its own. NULL as the group is
 * the root: no prefix, and the middleware given to ioxd_use. A prefix and what follows it are
 * joined by a '/' when neither side brings one ("/api" and "users" is "/api/users"), and a
 * repeated slash counts once.
 *
 * Register everything before ioxd_run, from the main thread; methods, paths and prefixes are
 * copied, so temporaries are fine, and anything registered once ioxd_run has started is ignored
 * with a line on stderr. ioxd_run resolves it once: every endpoint's full path into a segment
 * tree and its middleware into one flat chain, which the workers then share read-only. A request
 * costs one walk down the tree - no scan, no regex - and one call through its chain. */
typedef struct ioxd_group    ioxd_group;
typedef struct ioxd_endpoint ioxd_endpoint;

ioxd_group *ioxd_group_new(ioxd_group *parent, const char *prefix);   /* "/api"; "" for middleware only */
void        ioxd_group_use(ioxd_group *group, ioxd_mw mw);            /* wraps everything below it     */

/* An endpoint: method matched exactly, except that HEAD is answered by the GET of a path that
 * has no HEAD of its own; path matched by segment below the group's prefix, with :name captures
 * ("/users/:id") landing in req.route_params. A static segment beats a capture at any depth, and
 * a static path that lacks the method falls through to a capture route that has it. A trailing
 * slash is tolerated. Segments are matched as they arrive on the wire, so an escape in a static
 * segment does not match it, but a captured value is handed over percent-decoded ("/users/a%2Fb"
 * captures "a/b") - raw in the rare case that it does not fit IOXD_ROUTE_ARENA. */
ioxd_endpoint *ioxd_route(ioxd_group *group, const char *method, const char *path, ioxd_handler fn);
void           ioxd_endpoint_use(ioxd_endpoint *endpoint, ioxd_mw mw);   /* wraps this one only */

/* The verbs, for short: ioxd_get(api, "/users/:id", user). */
static inline ioxd_endpoint *ioxd_get   (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "GET",    path, fn); }
static inline ioxd_endpoint *ioxd_post  (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "POST",   path, fn); }
static inline ioxd_endpoint *ioxd_put   (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "PUT",    path, fn); }
static inline ioxd_endpoint *ioxd_patch (ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "PATCH",  path, fn); }
static inline ioxd_endpoint *ioxd_delete(ioxd_group *g, const char *path, ioxd_handler fn) { return ioxd_route(g, "DELETE", path, fn); }

/* Root middleware: every request, the fallbacks included. */
void ioxd_use(ioxd_mw mw);
/* The fallback when no path matches (a built-in 404 by default). A path that matches without the
 * method gets a built-in 405 whose allow header lists every method that path has, on a capture
 * route too. Both run behind the root's middleware only, so that allow header names methods a
 * group's own middleware would otherwise have gated. */
void ioxd_default(ioxd_handler fn);

/* ── the same, as a script ─────────────────────────────────────────────────────────────── */

/* Registration as a block-structured script: a current group, which the block after IOXD_GROUP
 * sets (the root outside any block), endpoints registered into it, with their own middleware
 * listed after the handler, and IOXD_USE adding middleware to it - so a group's middleware is
 * either listed after its prefix or added with IOXD_USE inside its block. Plain functions
 * underneath, so everything is type-checked; a group's block runs exactly once, and leaving it
 * early - break, return, goto - still closes the group.
 *
 *     IOXD_USE(log);
 *     IOXD_GET("/", home);
 *     IOXD_GROUP("/api", api_header) {
 *         IOXD_GET("/ping", ping);
 *         IOXD_GROUP("/admin", require_token) {
 *             IOXD_GET("/stats", stats, timing);
 *         }
 *     }
 */
#define IOXD_MAX_MW 16                              /* middleware per group and per endpoint */
struct ioxd_group_args    { const char *prefix; ioxd_mw mws[IOXD_MAX_MW + 1]; };           /* +1: the ending null */
struct ioxd_endpoint_args { const char *path; ioxd_handler fn; ioxd_mw mws[IOXD_MAX_MW + 1]; };

/* What the macros call: the current group's stack and an endpoint with a middleware list. */
ioxd_group    *ioxd__group_begin(struct ioxd_group_args args);
ioxd_group    *ioxd__group_end(void);
void           ioxd__group_pop(ioxd_group **open);
ioxd_group    *ioxd__group_current(void);
ioxd_endpoint *ioxd__endpoint(const char *method, struct ioxd_endpoint_args args);

/* The argument lists become the structs above. An argument count picks the expansion, so the
 * middleware list always has its own braces and no macro is ever invoked with an empty variadic
 * part: clean under -Wall -Wextra -pedantic, in C11 and later. */
#define IOXD__CAT2(a, b) a##b
#define IOXD__CAT(a, b)  IOXD__CAT2(a, b)
#define IOXD__PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, name, ...) name
#define IOXD__RW(path, fn, ...) (struct ioxd_endpoint_args){ (path), (fn), { __VA_ARGS__, NULL } }
#define IOXD__RB(path, fn)      (struct ioxd_endpoint_args){ (path), (fn), { NULL } }
#define IOXD__ROUTE_ARGS(...)   IOXD__PICK(__VA_ARGS__, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, \
                                           IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, IOXD__RW, \
                                           IOXD__RW, IOXD__RW, IOXD__RB, IOXD__RB, IOXD__RB)(__VA_ARGS__)
#define IOXD__GW(prefix, ...)   (struct ioxd_group_args){ (prefix), { __VA_ARGS__, NULL } }
#define IOXD__GB(prefix)        (struct ioxd_group_args){ (prefix), { NULL } }
/* IOXD__TOO_MANY_GROUP_MW is left undefined on purpose: a 17th middleware lands on it and does
 * not compile, rather than overflowing the list and losing the last one quietly. The endpoint
 * form fails on its own - a 17th middleware there is taken for the macro name. */
#define IOXD__GROUP_ARGS(...)   IOXD__PICK(__VA_ARGS__, IOXD__TOO_MANY_GROUP_MW, \
                                           IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, \
                                           IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, IOXD__GW, \
                                           IOXD__GW, IOXD__GW, IOXD__GB, IOXD__GB)(__VA_ARGS__)

/* A break, a return or a goto out of the block skips the loop's increment, so where the compiler
 * has __attribute__((cleanup)) the pop is hung on the block's variable and runs on every way out;
 * a block that ended on its own has already popped and nulled it. Elsewhere the plain form
 * stands, and a group still open at ioxd_run is reported. */
#ifdef __GNUC__
#define IOXD_GROUP(...)                                                                             \
    for (ioxd_group *IOXD__CAT(ioxd__block_, __LINE__) __attribute__((cleanup(ioxd__group_pop)))    \
             = ioxd__group_begin(IOXD__GROUP_ARGS(__VA_ARGS__));                                    \
         IOXD__CAT(ioxd__block_, __LINE__); IOXD__CAT(ioxd__block_, __LINE__) = ioxd__group_end())
#else
#define IOXD_GROUP(...)                                                                             \
    for (ioxd_group *IOXD__CAT(ioxd__block_, __LINE__)                                              \
             = ioxd__group_begin(IOXD__GROUP_ARGS(__VA_ARGS__));                                    \
         IOXD__CAT(ioxd__block_, __LINE__); IOXD__CAT(ioxd__block_, __LINE__) = ioxd__group_end())
#endif
#define IOXD_USE(mw)            ioxd_group_use(ioxd__group_current(), (mw))
#define IOXD_ROUTE(method, ...) ioxd__endpoint((method), IOXD__ROUTE_ARGS(__VA_ARGS__))
#define IOXD_GET(...)           IOXD_ROUTE("GET",    __VA_ARGS__)
#define IOXD_POST(...)          IOXD_ROUTE("POST",   __VA_ARGS__)
#define IOXD_PUT(...)           IOXD_ROUTE("PUT",    __VA_ARGS__)
#define IOXD_PATCH(...)         IOXD_ROUTE("PATCH",  __VA_ARGS__)
#define IOXD_DELETE(...)        IOXD_ROUTE("DELETE", __VA_ARGS__)
#define IOXD_DEFAULT(fn)        ioxd_default(fn)

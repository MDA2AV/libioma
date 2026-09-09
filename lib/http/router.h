/*
 * http/router.h - the router's entries for the runner and the engine: resolve everything
 * registered once, before the workers start; then dispatch each request through the result.
 */
#pragma once

#include "ioxd.h"

void ioxd__router_build(void);                    /* the segment tree and the flat chains, once */
void ioxd__dispatch(ioxd_ctx *ctx);               /* the request's endpoint, behind its chain    */

/* ── router.c: the notes ──────────────────────────────────────────────────────────────────── */

/*
 * router.c - groups, endpoints, middleware, and the segment tree they resolve into. Everything
 * is registered before the workers start and resolved once by ioxd_run; after that it is
 * read-only and every worker shares it without a lock. A request costs one walk down the tree
 * and one call through its endpoint's flat middleware chain.
 */

/* at file scope:
 *   - end-of-path nodes one walk remembers  [#define SEEN_MAX  8]
 *   - methods a 405's allow header lists  [#define ALLOW_MAX 16]
 *   - nullptr only for the root  [ioxd_group *parent;]
 *   - our copy of the caller's  [const char *prefix;]
 *   - the registration list  [ioxd_endpoint *next;]
 *   - our copy of the caller's  [const char    *method;]
 *   - below the group's prefix; our copy too  [const char    *path;]
 *   - registration order, for the allow header  [int            seq;]
 *   - resolved by ioxd__router_build  [char          *full;]
 *   - the whole path, prefixes included  [char          *full;]
 *   - the :name captures, in path order  [ioxd_slice     names[IOXD_MAX_ROUTE_PARAMS];]
 *   - root, each group outer to inner, then own  [ioxd_mw       *chain;]
 *   - One segment of the tree; the root is the empty one.  [struct node {]
 *   - the static segment this node is  [ioxd_slice      seg;]
 *   - static children  [struct node   **kids;]
 *   - the child that takes any segment  [struct node    *param;]
 *   - the endpoints here, one per method  [ioxd_endpoint **eps;]
 *   - Every end-of-path node a walk reached, for a 405's allow header: a path can end more
 *     than one route ("/users/new" is also the "/users/:id" of a capture route), and all of
 *     their methods are allowed. A path that ends more than SEEN_MAX of them loses the rest of
 *     the list.
 *   - The chain cursor handed to each middleware; ioxd_next_run advances it.  [struct
 *     ioxd_next {]
 *   - no prefix; ioxd_use's middleware  [static ioxd_group     g_root = { .prefix = "" };]
 *   - the script form's open group  [static ioxd_group    *g_current = &g_root;]
 *   - endpoints in registration order  [static ioxd_endpoint *g_first, *g_last;]
 *   - how many, so each gets its seq  [static int            g_n_eps;]
 *   - the root node  [static struct node    g_tree;]
 *   - Is this the endpoint's method? name must be a literal, for the sizeof.  [#define
 *     method_is(ep, name) ((ep)->method_len == sizeof(name) - 1 && \]
 *   - ── registration ────────────────────────────────────────────────────────────────────────
 *     [/ * Registration is over once ioxd_run has resolved the table: it is read-only fr]
 *   - --- the script form (the IOXD_ macros) ---  [ioxd_group *ioxd__group_begin(struct
 *     ioxd_group_args args)]
 *   - ── resolution, once, from ioxd_run ─────────────────────────────────────────────────────
 *     [static bool next_segment(const char **at, const char *end, ioxd_slice *seg)]
 *   - ── a request ───────────────────────────────────────────────────────────────────────────
 *     [/ * The endpoint at a node for the method, or nullptr. A node with a GET and no H]
 */

/* same:
 * Exact slice compare.
 */

/* must:
 * Out of memory at startup: nothing sensible to continue with.
 */

/* too_late:
 * Registration is over once ioxd_run has resolved the table: it is read-only from then on and
 * the workers are already reading it, so anything later is dropped with a word about it.
 */

/* ioxd_group_new:
 * A group below parent (nullptr: the root) at prefix.
 */

/* ioxd_group_use:
 * Middleware around everything below the group.
 */

/* ioxd_use:
 * Root middleware: every request.
 */

/* ioxd_route:
 * An endpoint in a group (nullptr: the root).
 */

/* ioxd_endpoint_use:
 * Middleware around one endpoint.
 */

/* ioxd__group_begin:
 * Open a group below the current one and make it current; its middleware list ends at a null.
 */

/* ioxd__group_end:
 * Close the current group; null, so the block's loop ends.
 */

/* ioxd__group_pop:
 * The block's cleanup handler, where the compiler has one: a break, return or goto out of an
 * IOXD_GROUP skips the loop's increment, so the group is popped here instead. A block that
 * ended on its own already popped and nulled the variable, and this does nothing.
 */

/* ioxd__group_current:
 * The group a script-form registration goes into.
 */

/* ioxd__endpoint:
 * An endpoint in the current group, with its middleware list (ended by a null).
 */

/* ioxd_default:
 * Replace the built-in 404 fallback.
 */

/* next_segment:
 * The next segment of a path from *at, slashes skipped; false at the end.
 */

/* prepend:
 * Put one part of n bytes in front of what is already at buf + *at, with a '/' between them
 * when neither side brought one - so a group "/api" and a path "users" join as "/api/users".
 */

/* full_path:
 * The endpoint's whole path: its groups' prefixes, outermost first, then its own path. Written
 * right to left, from the innermost group up, so no list of the groups is needed, then moved
 * to the front of the buffer over whatever the joining slashes did not need.
 *   - at most one joining '/' per part  [char  *full = must(malloc(len + parts + 1));]
 */

/* flatten_chain:
 * The endpoint's middleware, flat: the root's, each group's outer to inner, then its own.
 * Filled right to left, like the path.
 */

/* child:
 * The static child for a segment, made if missing.
 */

/* insert:
 * Put an endpoint into the tree along its full path; a ':name' segment goes through the
 * capture child and its name is kept with the endpoint. A duplicate keeps the first.
 */

/* group_depth:
 * How deep the open group is; anything but zero at ioxd_run means an IOXD_GROUP block was left
 * without its end, and every registration after it silently nested inside.
 */

/* ioxd__router_build:
 * Resolve everything registered: full paths into the tree, middleware into flat chains.
 */

/* endpoint_for:
 * The endpoint at a node for the method, or nullptr. A node with a GET and no HEAD answers
 * HEAD with its GET endpoint (RFC 9110 9.3.2): the handler still sees "HEAD" as the method,
 * and the engine drops the body it writes.
 */

/* walk:
 * Walk the tree along the path from at, the segments that capture nodes take going into
 * req->route_params (raw values; the names come with the endpoint, and dispatch decodes). The
 * static child is tried before the capture, so a static segment wins, and the capture is tried
 * when the static branch comes to nothing - including when it reaches the end without this
 * method. Returns the endpoint for the method, or nullptr; every node the path itself reached
 * with endpoints on it lands in *seen, for a 405.
 *   - static children are unique: no other candidate  [break;]
 */

/* decoded:
 * Percent-decode a captured segment into the request's arena and point it there ('+' is a
 * plain '+' in a path, and a malformed %XX is kept as is). Untouched when it has nothing to
 * decode, or when the arena has no room for it.
 */

/* listed:
 * Is this method already in the list? Two nodes of one walk can allow the same one.
 */

/* allowed_endpoints:
 * The endpoints of every node the walk reached, each method once, in registration order: an
 * insertion sort by seq, which is all the ordering a handful of methods needs.
 */

/* allow_value:
 * Those methods as "GET, HEAD, POST" in the request's arena, HEAD written after a GET that has
 * no HEAD of its own since that is what answers it. nullptr when there is nothing to say, or
 * when the list would not fit.
 */

/* ioxd_next_run:
 * Run the next middleware, or the endpoint once the chain is exhausted. A middleware that does
 * not call this short-circuits the request. The cursor moves in place, so a middleware that
 * calls this a second time does not replay what is behind it: once the chain has run out the
 * call does nothing at all.
 *   - the chain and the handler are both spent  [if (next->i > next->n)]
 */

/* run:
 * A handler behind a chain; a direct call when the chain is empty.
 */

/* not_found:
 * The built-in fallbacks.
 */

/* not_allowed:
 *   - status and allow are set before its chain  [static void not_allowed(ioxd_ctx *ctx)]
 */

/* ioxd__dispatch:
 * Find the request's endpoint and run it behind its chain; the fallbacks run behind the
 * root's.
 *   - the path is known, the method is not  [if (seen.n) {]
 */

/* Atlas - A9: the remote gateway.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What it is
 *
 * A process that terminates HTTP from off this machine, authenticates the
 * bearer credential, checks scopes, and forwards **only** explicitly supported
 * operations to `atlasd` over the ordinary Unix socket.
 *
 * It is structurally the same shape as the MCP adapter: it holds no database
 * handle, opens no repository, creates no process and has no filesystem write
 * path. Every answer it gives came over the socket.
 *
 * ## What it cannot do, and why that is not a promise
 *
 * The gateway runs as its own account, named by `gateway_uid` in the root-owned
 * policy. That uid is neither the operator uid nor a dispatcher uid, so
 * `decision.approve`, `backup.create`, `code.index`, every `job.` and every
 * `dispatch.` method answer it `unknown method` — the same answer a name that
 * does not exist gets. Under A7.1 it also cannot read the index at all, because
 * the index is 0700 `atlasd`.
 *
 * So: **a compromised gateway cannot approve a decision, register a repository,
 * read a backup, run a job, build an index or read the database.** That is true
 * because of who it runs as. Nothing in this file is what makes it true, and a
 * bug in this file cannot make it false.
 *
 * ## There is no arbitrary forwarding
 *
 * A client never names an Atlas method. The gateway maps a fixed set of routes
 * and MCP tool names onto a fixed set of daemon calls; a request that matches no
 * route is a 404 and never becomes a socket message. There is no path by which a
 * request body chooses what Atlas is asked.
 *
 * ## TLS
 *
 * Atlas terminates none. See `atlas/gwpolicy.h`: an in-process TLS stack would
 * be a new third-party dependency, which the project's hard rules forbid, so
 * `tls_mode = REVERSE_PROXY` records that something in front terminates it.
 * Never describe A9 as providing TLS.
 */
#ifndef ATLAS_GATEWAY_H
#define ATLAS_GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "atlas/apikey.h"
#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/http.h"

typedef struct atlas_gateway atlas_gateway;

typedef struct atlas_gateway_opts {
    /* The daemon socket. NULL resolves it the ordinary way; tests set it. */
    const char *socket_path;
    /* Milliseconds one forwarded daemon call may take. Zero means the
     * compiled-in default. */
    int timeout_ms;
    /* Where diagnostics go. Never stdout — a gateway has no stdout protocol,
     * but keeping the habit means a log line can never corrupt a response. */
    FILE *errout;
} atlas_gateway_opts;

/* Creates a gateway over a policy the caller already loaded and checked.
 *
 * The policy is copied. A gateway whose policy could change under it would be a
 * gateway whose bounds are not the ones an operator read. */
atlas_status atlas_gateway_open(const atlas_gwpolicy *policy, const atlas_gateway_opts *opts,
                                atlas_gateway **out, atlas_err *err);
void atlas_gateway_close(atlas_gateway *g);

/* Handles one complete HTTP request and produces one complete HTTP response.
 *
 * Bytes in, bytes out, with no socket involved. That is deliberate: it makes
 * every route, every refusal and the whole authentication path testable against
 * a real fixture daemon without opening a listening port, and it means the
 * socket loop below contains no behaviour of its own.
 *
 * `request` must be a complete request — head and, if declared, body. The
 * response is always a complete, well-formed HTTP message: there is no path
 * that produces a partial one, and a failure anywhere becomes a 500 rather than
 * a closed connection with nothing said. */
atlas_status atlas_gateway_serve_bytes(atlas_gateway *g, const char *request, size_t len,
                                       atlas_buf *response, atlas_err *err);

/* Binds, listens and serves until `*stop` becomes true.
 *
 * Concurrency is bounded by the policy: a connection beyond the ceiling is
 * answered 503 and closed rather than queued, which is A8-CI's rule about
 * deterministic refusal — a queued connection that eventually times out is a
 * slow failure nobody can distinguish from a hang.
 *
 * Reads carry deadlines, so a peer that stops mid-request costs one slot for a
 * bounded time rather than forever. */
atlas_status atlas_gateway_serve(atlas_gateway *g, volatile bool *stop, atlas_err *err);

/* The `atlas gateway run` command. Loads the compiled-in policy, refuses to
 * start when it is disabled, and reports why. */
atlas_status atlas_service_gateway_run(atlas_err *err);
/* `atlas gateway status`: what the policy says, without binding anything. */
atlas_status atlas_service_gateway_status(FILE *out, bool json, atlas_err *err);

#endif /* ATLAS_GATEWAY_H */

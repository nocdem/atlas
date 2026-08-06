/* Atlas - the stdio Model Context Protocol adapter.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas mcp` speaks JSON-RPC over stdin and stdout and is, structurally, a
 * client of the Atlas daemon. It holds no database handle of its own — not even
 * a read-only one — so every answer it gives came over the authenticated Unix
 * socket, and every rule the daemon enforces about bounds, provenance and the
 * single writer applies to it for free.
 *
 * What it must never do, and what the code is arranged to make hard:
 *
 *   - open the writable index (it opens no index at all);
 *   - start a daemon (it connects or reports degraded);
 *   - perform a repository scan (it can ask the daemon to schedule one, and
 *     that is the whole of its influence over the indexer);
 *   - write to a target repository (it has no write path to a filesystem);
 *   - execute anything (there is no process creation in the adapter);
 *   - put a byte on stdout that is not an MCP message.
 *
 * The last one is a protocol requirement rather than a safety one, and it is
 * why diagnostics go to stderr unconditionally. A log line on stdout does not
 * degrade an MCP session, it ends it.
 *
 * Trust boundary: an MCP result may carry repository prose — a commit subject,
 * a path — because a caller asked for that specific thing. Every such field is
 * bounded, safe-encoded, labelled with its provenance, and accompanied by
 * `untrusted_data: true`. None of it ever reaches automatic context; that is a
 * different code path with a stricter rule. See docs/ai-trust-boundary.md.
 */
#ifndef ATLAS_MCP_H
#define ATLAS_MCP_H

#include <stdbool.h>
#include <stdio.h>

#include "atlas/error.h"

typedef struct atlas_mcp_opts {
    /* Overrides the socket path. Tests set this; nothing else does. */
    const char *socket_path;
    /* Milliseconds a daemon call may take. Zero means the default. */
    int timeout_ms;
} atlas_mcp_opts;

void atlas_mcp_opts_init(atlas_mcp_opts *o);

/* Serves until stdin reaches end of file, which is how an MCP client shuts a
 * stdio server down. Returns ATLAS_OK for a clean exit. */
atlas_status atlas_mcp_run(FILE *in, FILE *out, FILE *errout, const atlas_mcp_opts *opts,
                           atlas_err *err);

/* The protocol version Atlas prefers when a client asks for one it does not
 * know. Exposed so tests assert against the same constant the server uses. */
#define ATLAS_MCP_PREFERRED_PROTOCOL "2025-06-18"

/* True when Atlas can speak this revision. Exposed for the same reason. */
bool atlas_mcp_protocol_supported(const char *version);

/* The tool names Atlas exposes, NULL-terminated. Used by `atlas integrate
 * claude doctor` and by the plugin fixture tests, so that the documented tool
 * surface and the implemented one cannot drift. */
const char *const *atlas_mcp_tool_names(void);

#endif /* ATLAS_MCP_H */

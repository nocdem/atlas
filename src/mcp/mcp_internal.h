/* Atlas - MCP adapter internals shared between its translation units.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header.
 */
#ifndef ATLAS_MCP_INTERNAL_H
#define ATLAS_MCP_INTERNAL_H

#include <stdbool.h>
#include <stdio.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/ipc.h"
#include "atlas/jsonread.h"
#include "atlas/json.h"
#include "atlas/limits.h"
#include "atlas/mcp.h"
#include "atlas/safetext.h"

/* A JSON-RPC id, preserved exactly.
 *
 * The specification allows a string or a number, and a response must echo what
 * it was sent. Coercing one to the other — the obvious shortcut — silently
 * breaks a client that correlates by identity rather than by value. */
typedef enum atlas_mcp_id_kind {
    ATLAS_MCP_ID_NONE = 0, /* a notification: no response is sent at all */
    ATLAS_MCP_ID_INT,
    ATLAS_MCP_ID_STRING
} atlas_mcp_id_kind;

typedef struct atlas_mcp_id {
    atlas_mcp_id_kind kind;
    int64_t number;
    atlas_buf text;
} atlas_mcp_id;

void atlas_mcp_id_init(atlas_mcp_id *id);
void atlas_mcp_id_free(atlas_mcp_id *id);
/* Writes the id as the `id` member of the document being built. */
atlas_status atlas_mcp_id_write(atlas_json *j, const atlas_mcp_id *id, atlas_err *err);

/* One allowed filesystem root, and the repository it resolved to.
 *
 * Roots are the authorization boundary. A tool call names a repository only by
 * a name that one of these resolved to, so there is no argument a caller can
 * send that reaches a repository outside the set the client granted. */
typedef struct atlas_mcp_root {
    atlas_buf path;      /* absolute, decoded from the client's file: URI */
    atlas_buf repo;      /* the registered name, empty when unregistered */
    bool resolved;       /* the daemon has been asked about this root */
    bool registered;     /* it is a repository Atlas has in its index */
    bool register_failed; /* registration was attempted and refused */
    atlas_buf refusal;   /* why, when it was */
} atlas_mcp_root;

/* Decodes a `file:` URI into an absolute local path.
 *
 * Exposed so the URI rules can be tested directly rather than only through a
 * live MCP session, because most of them are refusals and a refusal is easiest
 * to get wrong quietly.
 *
 * Accepts: an empty authority (`file:///p`) and `localhost` (`file://localhost/p`),
 * percent-encoded octets including multi-byte UTF-8, and paths containing
 * spaces.
 *
 * Refuses: any other authority (a remote host is not a local path), a malformed
 * or truncated `%` escape, a decoded NUL, a decoded byte that would introduce a
 * path separator or a `.`/`..` component after normalisation, and anything that
 * does not begin with `/`. A refusal is reported, never guessed at: decoding a
 * root wrongly would authorize a directory that merely looks like the right
 * one. */
atlas_status atlas_mcp_decode_file_uri(const char *uri, atlas_buf *out, atlas_err *err);

/* Whether a string is usable as an external session id.
 *
 * Accepts 1..ATLAS_AI_SESSION_KEY_MAX bytes of [A-Za-z0-9._:-] — enough for the
 * UUID Claude Code uses, and nothing else. Two exclusions are load-bearing
 * rather than tidiness:
 *
 *   - `/` is how a subagent key is spelled (`<session>/<agent>`), so an id
 *     containing one could name a subagent session that the MCP connection has
 *     no relationship to.
 *   - anything the daemon's safe encoding would rewrite would arrive as a
 *     different string from the one the hooks send for the same conversation,
 *     and the two would stop matching without anything reporting that they had.
 *
 * Over-long is rejected, never truncated: a truncated id is a different id, and
 * the one it collides with belongs to somebody else. Exposed so the rule can be
 * tested directly. */
bool atlas_mcp_session_id_valid(const char *value, size_t len);

typedef struct atlas_mcp_server {
    FILE *in;
    FILE *out;
    FILE *errout;
    atlas_buf socket;
    int timeout_ms;
    atlas_safe_pool safe;

    /* The external session id this server process was spawned with, from
     * CLAUDE_CODE_SESSION_ID — the same string the hooks receive as `session_id`
     * for the same conversation. Empty when the variable was absent, empty, or
     * rejected by atlas_mcp_session_id_valid(), and empty is not a failure: it
     * means every write this connection makes is recorded sessionless.
     *
     * Raw bytes, forwarded to the daemon unencoded. The daemon safe-encodes
     * every session key it receives, so pre-encoding here would produce a key
     * that no longer equals the one the hooks send. */
    atlas_buf session_key;
    /* The variable was set to something that is not a usable id. Kept apart from
     * "absent" so the reason a record is unbound can be reported accurately. */
    bool session_id_rejected;

    bool initialized;      /* the client sent notifications/initialized */
    bool got_initialize;   /* the client sent initialize */
    bool client_has_roots; /* the client advertised the roots capability */
    atlas_buf protocol;    /* the negotiated version */

    atlas_mcp_root roots[ATLAS_MCP_MAX_ROOTS];
    size_t root_count;
    bool roots_requested;
    int64_t roots_request_id;

    /* Ids for requests the server sends to the client. Negative so they cannot
     * collide with a client's own ids under any plausible numbering scheme. */
    int64_t next_outgoing_id;
} atlas_mcp_server;

/* --- transport ------------------------------------------------------------ */

/* Writes one message: the document, then exactly one newline.
 *
 * The document is checked for an interior newline before it is sent. The writer
 * escapes control bytes inside strings and emits none of its own, so this can
 * only fail if the writer is broken — which is exactly when a framing bug would
 * otherwise be silent and produce a stream a client cannot resynchronise. */
atlas_status atlas_mcp_send(atlas_mcp_server *s, const atlas_buf *document, atlas_err *err);

/* Builds a document with the streaming writer and sends it. `build` receives a
 * writer positioned before the outermost value. */
typedef atlas_status (*atlas_mcp_build_fn)(atlas_json *j, void *ud, atlas_err *err);
atlas_status atlas_mcp_emit(atlas_mcp_server *s, atlas_mcp_build_fn build, void *ud,
                            atlas_err *err);

/* Standard JSON-RPC error codes, plus the one MCP adds. */
#define ATLAS_MCP_PARSE_ERROR (-32700)
#define ATLAS_MCP_INVALID_REQUEST (-32600)
#define ATLAS_MCP_METHOD_NOT_FOUND (-32601)
#define ATLAS_MCP_INVALID_PARAMS (-32602)
#define ATLAS_MCP_INTERNAL_ERROR (-32603)

atlas_status atlas_mcp_send_error(atlas_mcp_server *s, const atlas_mcp_id *id, int code,
                                  const char *message, atlas_err *err);

/* Diagnostics. Always stderr, never stdout: a log line on stdout does not
 * degrade an MCP session, it ends it. */
void atlas_mcp_log(atlas_mcp_server *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* --- daemon access -------------------------------------------------------- */

/* One request to the daemon. Returns NULL when it could not be reached or
 * answered, having already logged why. The caller reports degraded state; there
 * is no path that invents an answer. */
atlas_ipc_response *atlas_mcp_call(atlas_mcp_server *s, const char *method, const char *params);

/* --- tools ---------------------------------------------------------------- */

/* Writes the `tools` array of a tools/list result. */
atlas_status atlas_mcp_write_tool_list(atlas_json *j, atlas_err *err);

/* Handles one tools/call. Always sends exactly one response. */
atlas_status atlas_mcp_call_tool(atlas_mcp_server *s, const atlas_mcp_id *id, const char *name,
                                 const atlas_jsonv *arguments, atlas_err *err);

/* Resolves the repository a tool call is about, honouring the root
 * authorization rule. `*repo_out` receives a registered repository name.
 *
 * `requested` may be NULL (use the first root) or a repository name, which must
 * be one a granted root resolved to. Anything else is refused: that is the
 * whole of the access control, and it is deliberately a whitelist rather than a
 * path comparison. */
atlas_status atlas_mcp_resolve_repo(atlas_mcp_server *s, const char *requested, atlas_buf *repo_out,
                                    atlas_err *err);

#endif /* ATLAS_MCP_INTERNAL_H */

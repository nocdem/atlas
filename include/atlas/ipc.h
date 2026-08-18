/* Atlas - local IPC: framing, transport and request/response contract.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Atlas' IPC is deliberately small and deliberately local.
 *
 * Transport
 *   A Unix-domain SOCK_STREAM socket at $XDG_RUNTIME_DIR/atlas/atlas.sock. The
 *   runtime directory is 0700 and the socket 0600, both verified rather than
 *   assumed. There is no /tmp fallback: a world-writable directory is not a
 *   place to put an endpoint that can mutate an index, so a missing
 *   XDG_RUNTIME_DIR is an actionable error instead of a downgrade.
 *
 *   Every accepted connection is checked with SO_PEERCRED and refused unless the
 *   peer's effective UID equals ours. The filesystem permissions and the
 *   credential check are independent: either alone would be enough, and neither
 *   is trusted to be the only one.
 *
 * Framing
 *   Length-prefixed, so a partial read is never mistaken for a complete request:
 *
 *     offset 0  4 bytes  magic "ATL1"
 *     offset 4  2 bytes  protocol version, big-endian
 *     offset 6  2 bytes  flags, big-endian, reserved and must be zero
 *     offset 8  4 bytes  payload length, big-endian
 *     offset 12          payload: UTF-8 JSON, exactly `length` bytes
 *
 *   The length is validated against a hard ceiling before a single payload byte
 *   is read, so an attacker cannot make Atlas allocate by claiming a large
 *   frame. Reads and writes carry deadlines, so a peer that stops mid-frame
 *   costs one slot for a bounded time rather than forever.
 *
 * Requests and responses
 *   Request:  {"id": "...", "method": "...", "params": { ... }}
 *   Success:  {"id": "...", "ok": true,  "result": { ... }}
 *   Failure:  {"id": "...", "ok": false, "error": {"status": N, "code": "...",
 *                                                  "message": "..."}}
 *
 *   `status` is the same stable exit-code contract the CLI uses, so a caller
 *   never has to map two vocabularies. Large results are paginated with an
 *   explicit cursor and a `more` flag; nothing is ever silently truncated.
 *
 * There is deliberately no shutdown method. A remotely reachable "stop" turns
 * any local process that can open the socket into something that can disable
 * indexing; systemd already owns the lifecycle, and SIGTERM already works.
 */
#ifndef ATLAS_IPC_H
#define ATLAS_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/json.h"
#include "atlas/syspolicy.h"
#include "atlas/limits.h"

/* --- framing ------------------------------------------------------------- */

typedef struct atlas_ipc_header {
    uint16_t version;
    uint16_t flags;
    uint32_t length;
} atlas_ipc_header;

/* Writes ATLAS_IPC_HEADER_BYTES into `out`, which must be at least that large. */
void atlas_ipc_header_encode(unsigned char *out, uint32_t payload_len);

/* Parses a header from exactly ATLAS_IPC_HEADER_BYTES. Rejects a wrong magic, an
 * unsupported version, any non-zero reserved flag, and a length above `max`. */
atlas_status atlas_ipc_header_decode(const unsigned char *in, uint32_t max, atlas_ipc_header *out,
                                     atlas_err *err);

/* Blocking-with-deadline frame I/O over a connected socket.
 *
 * `atlas_ipc_read_frame` handles partial reads: it returns only when a complete
 * payload has arrived, the deadline has passed, or the peer closed. A clean
 * close before any byte of a frame reports `*eof_out = true` with ATLAS_OK,
 * because an idle peer hanging up is not an error. */
atlas_status atlas_ipc_read_frame(int fd, uint32_t max_payload, int timeout_ms, atlas_buf *out,
                                  bool *eof_out, atlas_err *err);
atlas_status atlas_ipc_write_frame(int fd, const void *payload, size_t len, int timeout_ms,
                                   atlas_err *err);

/* --- socket -------------------------------------------------------------- */

/* Resolves the runtime directory and the socket path inside it. Neither is
 * created.
 *
 * **A7.1.** When a root-anchored system policy is active these return the
 * socket that policy names, and `$XDG_RUNTIME_DIR` is not consulted at all —
 * which is what lets a client on an SSH session with no runtime directory reach
 * the shared daemon, and what stops a client choosing a different endpoint by
 * setting an environment variable. Otherwise the per-user behaviour is
 * unchanged: `$XDG_RUNTIME_DIR/atlas`, or `/run/user/<uid>/atlas` on proof, and
 * ATLAS_ERR_CONFIG with an actionable message when neither is available. */
atlas_status atlas_ipc_runtime_dir(atlas_buf *out, atlas_err *err);
atlas_status atlas_ipc_socket_path(atlas_buf *out, atlas_err *err);

/* Names the index this process is addressing, so socket resolution can follow
 * it.
 *
 * **A socket belongs to an index.** A system policy says "the shared daemon
 * serves /var/lib/atlas on /run/atlas/atlas.sock"; it does not say every Atlas
 * process must use that socket. A command explicitly pointed at a *different*
 * data directory — `atlas --data-dir /tmp/x`, which is how an operator runs an
 * offline lifecycle command and how the suite isolates itself — is talking
 * about another index, and must reach that index's own per-user endpoint rather
 * than the system one. This is the same rule `route_to_daemon()` already
 * applies when it insists the daemon must own *this* directory.
 *
 * Called once, early, by the CLI, with whatever `--data-dir` resolved to. When
 * it is unset, or set to the policy's own data directory, the system socket is
 * used — which is the normal path for the daemon, for MCP and for a hook, none
 * of which names a directory. */
void atlas_ipc_socket_scope_set(const char *data_dir);

/* Creates the runtime directory and verifies it.
 *
 * `policy` may be NULL, which is per-user mode: mode 0700, owned by this uid,
 * nothing for group or other. In system mode the directory is 0750 owned by
 * this uid with the policy's client group, so members of that group can
 * traverse to the socket and nobody else can. Either way the result is
 * re-checked after being set, and a directory that cannot be brought to the
 * required shape is refused rather than used. */
atlas_status atlas_ipc_ensure_runtime_dir(const char *dir, const atlas_syspolicy *policy,
                                          atlas_err *err);

/* Binds and listens.
 *
 * A pre-existing path is only removed when it is a socket owned by this user
 * that nothing is listening on. A symlink, a regular file, a directory or a
 * live socket are all refused rather than unlinked: silently deleting whatever
 * happens to be at that path is how a lock file or somebody's data disappears.
 *
 * `policy` may be NULL for per-user mode, where the socket is 0600. In system
 * mode it is 0660 with the policy's client group, established explicitly and
 * then verified by `lstat`; if the owner, group or mode cannot be brought to
 * exactly that, the socket is unlinked and the daemon does not start. A socket
 * that is more open than intended is worse than no daemon. */
atlas_status atlas_ipc_listen(const char *socket_path, const atlas_syspolicy *policy, int *fd_out,
                              atlas_err *err);

/* Accepts one connection and verifies the peer's credentials. On refusal the
 * connection is already closed and ATLAS_ERR_INTEGRITY is returned. `*fd_out` is
 * -1 with ATLAS_OK when no connection was pending.
 *
 * The uid is taken from `SO_PEERCRED`, which the kernel fills in at connect
 * time. It is never read from the request, the environment or `/proc`. With a
 * NULL `policy` the only acceptable peer is this process's own uid; with an
 * active one, that uid or a uid the root-owned policy lists. Nothing else is
 * accepted, and the check happens before a single byte is read. */
/* `peer_uid_out` receives the kernel's answer about the peer, recorded at
 * connect time from SO_PEERCRED. A8 carries it into dispatch, because that is
 * the only identity a method group may be selected on: a client describing
 * itself in the request body is describing itself to a field Atlas does not
 * have. */
atlas_status atlas_ipc_accept(int listen_fd, const atlas_syspolicy *policy, int *fd_out,
                              int64_t *peer_pid_out, int64_t *peer_uid_out, atlas_err *err);

/* Connects to a listening daemon. Returns ATLAS_ERR_CONFIG when nothing is
 * listening, which callers treat as "the daemon is not running" rather than as a
 * failure. */
atlas_status atlas_ipc_connect(const char *socket_path, int timeout_ms, int *fd_out,
                               atlas_err *err);

/* --- request parsing ----------------------------------------------------- */

/* One parsed request. Field storage is owned by the parsed document, so the
 * whole thing is freed with atlas_ipc_request_free(). */
typedef struct atlas_ipc_request atlas_ipc_request;

atlas_status atlas_ipc_request_parse(const void *payload, size_t len, atlas_ipc_request **out,
                                     atlas_err *err);
void atlas_ipc_request_free(atlas_ipc_request *req);

/* The request id, always a printable safe string (never NULL). A request that
 * carried no id, or an unusable one, gets "0" so a response can still be
 * correlated. */
const char *atlas_ipc_request_id(const atlas_ipc_request *req);
const char *atlas_ipc_request_method(const atlas_ipc_request *req);

/* Typed accessors over `params`. Each returns false when the member is absent or
 * of the wrong type; there is no coercion, because guessing what a caller meant
 * is how a protocol grows undocumented behaviour. */
bool atlas_ipc_param_str(const atlas_ipc_request *req, const char *key, const char **out);
bool atlas_ipc_param_int(const atlas_ipc_request *req, const char *key, int64_t *out);
bool atlas_ipc_param_bool(const atlas_ipc_request *req, const char *key, bool *out);

/* An array of strings. A2 needs one — a list of paths a reason concerns — and
 * it is the only aggregate the protocol accepts, deliberately: every other
 * parameter is a scalar, so the request document stays shallow and the depth
 * limit stays comfortable.
 *
 * The handle borrows from the parsed request and is valid only as long as it
 * is. Non-string entries are reported as absent rather than coerced, like every
 * other accessor here. */
typedef struct atlas_ipc_array atlas_ipc_array;

bool atlas_ipc_param_array(const atlas_ipc_request *req, const char *key,
                           const atlas_ipc_array **out);
size_t atlas_ipc_array_len(const atlas_ipc_array *arr);
bool atlas_ipc_array_str(const atlas_ipc_array *arr, size_t index, const char **out);

/* --- client -------------------------------------------------------------- */

/* One request/response round trip against a running daemon. `response_out`
 * receives the raw JSON response payload, which the caller parses or forwards.
 *
 * This is the whole client: there is no persistent session, no reconnection
 * logic and no retry. A CLI invocation that cannot reach the daemon falls back
 * to its offline path, which is a decision the caller makes explicitly. */
atlas_status atlas_ipc_call(const char *socket_path, const char *method, const char *params_json,
                            atlas_buf *response_out, atlas_err *err);

/* Like atlas_ipc_call, with an explicit deadline. A hook has milliseconds to
 * spare and must fail open rather than make a person wait; the default
 * ten-second timeout is right for a CLI command and wrong for a hook. */
atlas_status atlas_ipc_call_timeout(const char *socket_path, const char *method,
                                    const char *params_json, int timeout_ms,
                                    atlas_buf *response_out, atlas_err *err);

/* True when a daemon is reachable at the resolved socket path right now. Any
 * configuration error (no XDG_RUNTIME_DIR, for example) reports "not running"
 * rather than failing, because every caller of this is deciding whether to take
 * the offline path. */
bool atlas_ipc_daemon_reachable(void);

/* Whether a reachable daemon is the one that owns `data_dir`.
 *
 * `atlas_ipc_daemon_reachable` answers "is a daemon there", which is the wrong
 * question for a mutation: there is one socket per user runtime directory, but
 * the data directory is chosen per invocation by `--data-dir` or
 * `ATLAS_DATA_DIR`. A caller that routed on reachability alone could have its
 * write applied to whichever directory the daemon was started with, and nothing
 * in either process would say so.
 *
 * False when nothing is listening, when the daemon owns a different directory,
 * and when it does not say which it owns. Failing closed costs a local lock
 * acquisition; failing open costs somebody else's index. */
bool atlas_ipc_daemon_owns(const char *data_dir);

/* --- A2: building requests with the typed writer -------------------------
 *
 * A0 and A1 built the two request documents they needed with `atlas_buf_appendf`
 * and refused any repository path containing a quote, a backslash or a control
 * byte rather than escaping it (docs/backlog.md item 11). A2 sends filesystem
 * paths, session identifiers and model-authored prose over this socket, so a
 * hand-built document is no longer defensible.
 *
 * This builds `params` through the same streaming writer the daemon answers
 * with, so exactly one implementation of the escaping contract exists on both
 * sides of the socket. The writer is handed out already positioned inside the
 * params object: a caller emits members and never sees the braces. */
typedef struct atlas_ipc_params atlas_ipc_params;

atlas_status atlas_ipc_params_begin(atlas_ipc_params **out, atlas_json **writer_out,
                                    atlas_err *err);
/* Closes the object and hands over the finished document. Frees the builder. */
atlas_status atlas_ipc_params_finish(atlas_ipc_params *p, atlas_buf *out, atlas_err *err);
/* The failure path. Exactly one of finish and abort runs, as with atlas_json. */
void atlas_ipc_params_abort(atlas_ipc_params *p);

/* --- A2: reading responses -----------------------------------------------
 *
 * A1's client returned the raw payload and its one caller looked for a
 * substring. That was honest about how little it needed; A2 needs typed access,
 * so the response is parsed with the same bounded, hostile-input discipline the
 * daemon applies to requests. */
typedef struct atlas_ipc_response atlas_ipc_response;

atlas_status atlas_ipc_response_parse(const void *payload, size_t len, atlas_ipc_response **out,
                                      atlas_err *err);
void atlas_ipc_response_free(atlas_ipc_response *r);

bool atlas_ipc_response_ok(const atlas_ipc_response *r);
/* The daemon's status code, which is the CLI's exit-code vocabulary. */
atlas_status atlas_ipc_response_status(const atlas_ipc_response *r);
/* The error message, already safe-encoded by the daemon. "" when there is none. */
const char *atlas_ipc_response_message(const atlas_ipc_response *r);

bool atlas_ipc_result_str(const atlas_ipc_response *r, const char *key, const char **out);
bool atlas_ipc_result_int(const atlas_ipc_response *r, const char *key, int64_t *out);
bool atlas_ipc_result_bool(const atlas_ipc_response *r, const char *key, bool *out);

/* A8. One member of one object inside a result array — `job.list` is the first
 * response shape that needs it. Same discipline as the scalar accessors: typed,
 * bounded, no coercion, and an embedded NUL reads as absent. */
bool atlas_ipc_result_arr_obj_str(const atlas_ipc_response *r, const char *arr_key, size_t index,
                                  const char *key, const char **out);
bool atlas_ipc_result_arr_obj_bool(const atlas_ipc_response *r, const char *arr_key, size_t index,
                                   const char *key, bool *out);
/* How many elements a top-level array has, and how many an array *inside* one
 * of its objects has. A reader that walked until a key was absent could not
 * tell an empty array from a missing one, and for a reason list those mean
 * different things. */
/* One level into a nested result object, and no further.
 *
 * `decision.get` answers with `result.document` and `result.revision`, and
 * every accessor above reads exactly one level — so the client parser read flat
 * names, matched nothing, and produced an empty document while reporting
 * success. These exist so a nested reply can be read rather than guessed at. A
 * general path syntax is deliberately not offered: it would be a query language
 * inside the response parser, and the replies that would need one should be
 * flattened instead. */
bool atlas_ipc_result_obj_str(const atlas_ipc_response *r, const char *obj_key, const char *key,
                              const char **out);
bool atlas_ipc_result_obj_int(const atlas_ipc_response *r, const char *obj_key, const char *key,
                              int64_t *out);
bool atlas_ipc_result_obj_bool(const atlas_ipc_response *r, const char *obj_key, const char *key,
                               bool *out);
bool atlas_ipc_result_obj_arr_len(const atlas_ipc_response *r, const char *obj_key,
                                  const char *arr_key, size_t *out);
bool atlas_ipc_result_obj_arr_str(const atlas_ipc_response *r, const char *obj_key,
                                  const char *arr_key, size_t index, const char **out);
bool atlas_ipc_result_obj_arr_obj_str(const atlas_ipc_response *r, const char *obj_key,
                                      const char *arr_key, size_t index, const char *key,
                                      const char **out);
bool atlas_ipc_result_obj_arr_obj_int(const atlas_ipc_response *r, const char *obj_key,
                                      const char *arr_key, size_t index, const char *key,
                                      int64_t *out);

bool atlas_ipc_result_arr_len(const atlas_ipc_response *r, const char *arr_key, size_t *out);
bool atlas_ipc_result_arr_str(const atlas_ipc_response *r, const char *arr_key, size_t index,
                              const char **out);
bool atlas_ipc_result_arr_obj_arr_len(const atlas_ipc_response *r, const char *arr_key,
                                      size_t index, const char *key, size_t *out);
bool atlas_ipc_result_arr_obj_arr_str(const atlas_ipc_response *r, const char *arr_key,
                                      size_t index, const char *key, size_t sub_index,
                                      const char **out);
bool atlas_ipc_result_arr_obj_int(const atlas_ipc_response *r, const char *arr_key, size_t index,
                                  const char *key, int64_t *out);

/* Re-emits the whole `result` object through `j`, member by member.
 *
 * Not a byte copy. Every string goes through atlas_json_str and every number
 * through atlas_json_int, so an adapter forwarding a daemon result into its own
 * document produces text this writer escaped rather than text it trusted. That
 * matters because it means there is still no "write these bytes as JSON"
 * primitive anywhere in Atlas — the one hole through which an unescaped value
 * eventually reaches a consumer.
 *
 * The walk is iterative and depth-bounded for the same reason the request
 * parser's is: recursing through a hostile document is the stack exhaustion the
 * limit exists to prevent. */
atlas_status atlas_ipc_result_write(const atlas_ipc_response *r, atlas_json *j, atlas_err *err);

/* --- A9.2.6: the one refusal a client may safely retry ----------------------
 *
 * The daemon refuses a write it cannot take right now — because the single
 * writer thread is inside a semantic pass with no statable end — with a message
 * that *begins with this token*. The token is the contract, not the prose after
 * it, and it is a token rather than a status code deliberately: the other way a
 * synchronous write fails is the deadline expiring, which leaves the job queued
 * and running, so the write does happen and the caller simply never hears the
 * outcome. Those two are the same status and opposite facts, and a caller that
 * confuses them either retries a write that already ran or abandons one that
 * did not.
 *
 * A11.1's run driver is the first caller to act on it: a completion refused
 * this way carries a worker's whole result, so retrying is the difference
 * between recording it and running a second worker for the same task. */
#define ATLAS_IPC_BUSY_TOKEN "BUSY:"

/* True when `message` is that refusal. Matched anywhere in the text rather than
 * only at the start, because a client that wrapped the daemon's message in its
 * own context is still reading the same refusal. */
bool atlas_ipc_message_is_busy(const char *message);

#endif /* ATLAS_IPC_H */

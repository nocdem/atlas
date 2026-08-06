/* Atlas - shared field limits.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These bounds are deliberately explicit: every fixed-size field in Atlas has a
 * documented ceiling, and input that exceeds it is rejected with a clear error
 * rather than silently truncated.
 */
#ifndef ATLAS_LIMITS_H
#define ATLAS_LIMITS_H

/* User-facing repository name. */
#define ATLAS_NAME_MAX 128u
/* Hex object id: 40 for SHA-1, 64 for SHA-256. */
#define ATLAS_OID_HEX_MAX 64u
#define ATLAS_OID_HEX_MAX_INCL (ATLAS_OID_HEX_MAX + 1u)
/* ISO-8601 UTC timestamp, e.g. "2026-08-06T13:56:00Z". */
#define ATLAS_TS_MAX 32u
/* Branch / ref shorthand. */
#define ATLAS_BRANCH_MAX 256u

/* --- A1: daemon, IPC, watcher and indexing bounds ------------------------
 *
 * Every one of these is a hard ceiling, not a hint. Nothing is silently
 * truncated when one is reached: the caller receives a structured error, or the
 * repository enters an explicit degraded state that `atlas daemon status` and
 * `atlas events` report. "Looks current" is never allowed to mean "gave up".
 */

/* IPC ------------------------------------------------------------------- */
/* Wire protocol version carried in every frame header. */
#define ATLAS_IPC_PROTOCOL_VERSION 1u
/* Frame header: magic(4) + version(2) + flags(2) + payload length(4), big-endian. */
#define ATLAS_IPC_HEADER_BYTES 12u
#define ATLAS_IPC_MAGIC "ATL1"
/* A request larger than this is refused before any of it is parsed. */
#define ATLAS_IPC_MAX_REQUEST_BYTES (1u * 1024u * 1024u)
/* A response larger than this is an internal error, never a truncated document. */
#define ATLAS_IPC_MAX_RESPONSE_BYTES (8u * 1024u * 1024u)
/* Maximum nesting depth accepted in a request document. */
#define ATLAS_IPC_MAX_JSON_DEPTH 24u
/* Concurrent accepted connections. Further connects are accepted and closed
 * with a structured error rather than left hanging. */
#define ATLAS_IPC_MAX_CLIENTS 64u
/* A client that cannot complete a frame within this many milliseconds is closed. */
#define ATLAS_IPC_READ_TIMEOUT_MS 10000
#define ATLAS_IPC_WRITE_TIMEOUT_MS 10000
/* Buffered but unsent response bytes per client before the client is dropped. */
#define ATLAS_IPC_MAX_CLIENT_BACKLOG_BYTES (ATLAS_IPC_MAX_RESPONSE_BYTES + 65536u)
/* Rows returned by one paginated response. */
#define ATLAS_IPC_MAX_ROWS 1000
/* Request id string length. */
#define ATLAS_IPC_MAX_REQUEST_ID 64u
/* Method name length. */
#define ATLAS_IPC_MAX_METHOD 64u

/* Daemon internals ------------------------------------------------------- */
/* Pending jobs in the single-writer queue before producers get backpressure. */
#define ATLAS_WRITER_QUEUE_MAX 4096u
/* Hashing/stat worker threads. Clamped against the online CPU count. */
#define ATLAS_WORKER_COUNT_MAX 8u
#define ATLAS_WORKER_COUNT_DEFAULT 4u
/* Jobs queued for the worker pool. */
#define ATLAS_WORKER_QUEUE_MAX 8192u
/* Rows applied inside one write transaction. Bounded so a transaction can never
 * be held open across an unbounded amount of work. */
#define ATLAS_DB_BATCH_MAX 256

/* Watcher ---------------------------------------------------------------- */
/* Inotify watches Atlas will install for one repository. Exceeding it is a
 * degraded state, reported, not a silent partial watch. */
#define ATLAS_WATCH_MAX_DIRS 8192u
/* Directories descended during one recursive discovery pass. */
#define ATLAS_WATCH_MAX_DISCOVER_DIRS 8192u
/* Files recorded by one recursive discovery pass. */
#define ATLAS_WATCH_MAX_DISCOVER_FILES 20000
/* Raw inotify events buffered between drains. */
#define ATLAS_WATCH_EVENT_QUEUE_MAX 16384u
/* Coalescing window: a burst of writes inside this window becomes one pass. */
#define ATLAS_WATCH_DEBOUNCE_MS 400
/* Upper bound on how long a debounce can be extended by continued activity. */
#define ATLAS_WATCH_MAX_DEBOUNCE_MS 5000
/* Periodic reconciliation, whether or not any event arrived. */
#define ATLAS_WATCH_RECONCILE_INTERVAL_MS 300000
/* Unpaired IN_MOVED_FROM cookies held while waiting for their IN_MOVED_TO. */
#define ATLAS_WATCH_MAX_PENDING_MOVES 1024u
/* Paths the watcher will name individually when it asks for a reconciliation.
 * A named path is hashed unconditionally, whatever its metadata says. Beyond
 * this many, the watcher stops naming them and asks for a full content
 * verification instead: it cannot enumerate what changed, so it must not
 * pretend it can. */
#define ATLAS_WATCH_MAX_DIRTY_PATHS 4096u
/* Bytes of dirty-path names held for one repository, so a repository full of
 * very long paths cannot make the set unbounded in memory rather than in count. */
#define ATLAS_WATCH_MAX_DIRTY_BYTES (1024u * 1024u)
/* How long an unpaired move cookie is held before it is treated as a delete. */
#define ATLAS_WATCH_MOVE_PAIR_MS 2000

/* Durable event journal --------------------------------------------------- */
/* Rows kept per repository in the bounded raw event journal. Older rows are
 * pruned; SOURCE and GIT evidence is never pruned with them. */
#define ATLAS_EVENTS_RETAIN_PER_REPO 20000
/* Rows one `events.since` response may carry. */
#define ATLAS_EVENTS_PAGE_MAX 1000

/* Filesystem work -------------------------------------------------------- */
/* Bytes hashed for one file before it is recorded as truncated with a reason. */
#define ATLAS_HASH_MAX_FILE_BYTES (256u * 1024u * 1024u)
/* Bytes of git stdout accepted by a daemon-side invocation. */
#define ATLAS_DAEMON_GIT_MAX_OUTPUT (64u * 1024u * 1024u)
/* Per-git-invocation timeout inside the daemon. */
#define ATLAS_DAEMON_GIT_TIMEOUT_MS 120000

/* --- A2: AI integration, MCP and hooks -----------------------------------
 *
 * The A2 adapters are the first Atlas code whose output reaches something that
 * interprets language rather than merely displaying it, so every bound here is
 * about two things at once: keeping a response finite, and keeping the amount
 * of repository-derived text that can reach a model small and deliberate.
 *
 * Nothing is silently truncated. A list that hits a ceiling reports `more` and
 * a cursor; a string that hits one is refused, not clipped, except where the
 * field explicitly documents a bounded excerpt. */

/* Context envelope --------------------------------------------------------- */
/* Claude's own ceiling for hook output strings is 10000 characters. Atlas uses
 * a much stricter one: automatic context is Atlas-owned metadata, and metadata
 * that needs four kilobytes to describe one repository is not metadata. */
#define ATLAS_AI_MAX_CONTEXT_BYTES 4096
/* The context envelope's own version, independent of the wire protocol and of
 * the schema. A consumer that caches an envelope can tell when its shape
 * changed without inspecting it. */
#define ATLAS_AI_CONTEXT_VERSION 1
/* Bytes of a repository root path reproduced in the automatic envelope. The
 * root is the one filesystem path the envelope carries, and it is the directory
 * the client is already running in. */
#define ATLAS_AI_CONTEXT_ROOT_MAX 160u

/* Sessions ----------------------------------------------------------------- */
/* An external client's own session identifier, after safe encoding. */
#define ATLAS_AI_SESSION_KEY_MAX 128u
/* Provider and client names, e.g. "anthropic" / "claude-code". */
#define ATLAS_AI_CLIENT_NAME_MAX 64u
/* A subagent identifier or type, after safe encoding. */
#define ATLAS_AI_AGENT_ID_MAX 128u
/* Sessions one repository may have open before the oldest idle ones expire. */
#define ATLAS_AI_MAX_OPEN_SESSIONS_PER_REPO 64
/* An open session untouched for this long is expired by the next pass that
 * notices, so a client that vanished does not hold a change set open forever. */
#define ATLAS_AI_SESSION_IDLE_EXPIRY_MS 86400000

/* Recorded reasons and decisions -------------------------------------------- */
/* A reason or decision summary. Bounded because it is durable and because it is
 * model-authored text that later enters another model's context. */
#define ATLAS_AI_SUMMARY_MAX 512u
#define ATLAS_AI_DETAIL_MAX 2048u
/* Paths one reason or decision may name. */
#define ATLAS_AI_MAX_PATHS_PER_RECORD 64
/* Durable reason and decision rows one session may create. Beyond this the
 * request is refused with a structured error rather than accepted and dropped. */
#define ATLAS_AI_MAX_RECORDS_PER_SESSION 500

/* Change sets and observed paths -------------------------------------------- */
/* Observed changed paths attributed to one session/repository change set. */
#define ATLAS_AI_MAX_CHANGED_PATHS 4096
/* Ephemeral hook events retained per session. These exist for idempotency and
 * for a bounded audit trail; they are pruned, and durable reasons and decisions
 * are never pruned with them. */
#define ATLAS_AI_EVENTS_RETAIN_PER_SESSION 2000
/* Direct-edit paths one PostToolBatch may name. */
#define ATLAS_AI_MAX_BATCH_PATHS 256

/* Hooks -------------------------------------------------------------------- */
/* A hook payload larger than this is refused before it is parsed. Claude's own
 * payloads are far smaller; this bounds a malformed or hostile one. */
#define ATLAS_HOOK_MAX_INPUT_BYTES (1u * 1024u * 1024u)
/* How long a hook waits on the daemon before giving up and failing open. A hook
 * that blocks is a hook that makes the user wait, so this is deliberately far
 * below Claude's timeout rather than close to it. */
#define ATLAS_HOOK_IPC_TIMEOUT_MS 2000
/* SessionEnd hooks share a 1.5 second budget across every installed hook, so
 * Atlas gives itself a fraction of it and fails open past that. */
#define ATLAS_HOOK_TEARDOWN_TIMEOUT_MS 700

/* MCP ---------------------------------------------------------------------- */
/* One newline-delimited JSON-RPC message. Checked against the accumulated line
 * length before any allocation grows to hold it. */
#define ATLAS_MCP_MAX_MESSAGE_BYTES (1u * 1024u * 1024u)
/* A tool result document. Well below Claude's 25000-token MCP output limit, so
 * a result is never the thing that fills a context window. */
#define ATLAS_MCP_MAX_RESULT_BYTES (128u * 1024u)
/* Rows one tool call may return, and the default when none is requested. */
#define ATLAS_MCP_MAX_ROWS 200
#define ATLAS_MCP_DEFAULT_ROWS 50
/* Filesystem roots the client may advertise. */
#define ATLAS_MCP_MAX_ROOTS 32
/* How long an MCP tool call waits on the daemon before reporting degraded. */
#define ATLAS_MCP_IPC_TIMEOUT_MS 5000
/* How long the server waits for the client's roots/list answer. Exceeding it is
 * not an error: the server falls back to CLAUDE_PROJECT_DIR. */
#define ATLAS_MCP_ROOTS_TIMEOUT_MS 3000
/* Bytes of repository-derived prose one explicitly-requested field may carry.
 * A commit subject is evidence; a commit subject that fills a screen is a
 * payload. */
#define ATLAS_MCP_MAX_PROSE_BYTES 512u

#endif /* ATLAS_LIMITS_H */

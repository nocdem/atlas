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

/* --- A3: structural code intelligence ------------------------------------
 *
 * A3 reads source bytes and builds a graph from them, so every bound here is
 * about the same two things A2's were: keeping the work finite on a hostile or
 * merely enormous input, and keeping the amount of repository-derived text that
 * can reach a model small and deliberate.
 *
 * Same rule as everywhere else: nothing is silently truncated. Reaching a bound
 * writes a `code_index_errors` row, sets `truncated` on the file, and is
 * reported in every result derived from it. */

/* Parsing ----------------------------------------------------------------- */
/* Bytes of one source file the lexer will read. Beyond it the file is recorded
 * with `truncated` and a reason; it is never parsed as a prefix pretending to be
 * whole. */
#define ATLAS_CODE_MAX_FILE_BYTES (4u * 1024u * 1024u)
/* Bytes in one token. A generated file can contain a megabyte-long string
 * literal; an identifier that long is not an identifier. */
#define ATLAS_CODE_MAX_TOKEN_BYTES 4096u
/* Brace, parenthesis and conditional nesting the extractor will track. */
#define ATLAS_CODE_MAX_NESTING_DEPTH 256u
/* Symbols recorded for one file. */
#define ATLAS_CODE_MAX_SYMBOLS_PER_FILE 4096
/* Relations recorded for one file, across every kind. */
#define ATLAS_CODE_MAX_RELATIONS_PER_FILE 16384
/* Call-candidate occurrences recorded for one file. */
#define ATLAS_CODE_MAX_OCCURRENCES_PER_FILE 16384
/* `#include` directives recorded for one file. */
#define ATLAS_CODE_MAX_INCLUDES_PER_FILE 1024
/* Bytes of a symbol name or an include spelling, after safe encoding. Refused
 * rather than truncated: half an identifier is a different identifier. */
#define ATLAS_CODE_MAX_NAME_BYTES 256u
/* How far include resolution will follow a chain before reporting depth. */
#define ATLAS_CODE_MAX_INCLUDE_DEPTH 32
/* Files one structural pass will parse. Beyond it the pass reports truncation
 * and the remainder is picked up by the next pass, which sees them as still
 * differing by content hash. */
#define ATLAS_CODE_MAX_PARSE_FILES_PER_PASS 20000
/* Files held in memory as parse results at once.
 *
 * The pass selects, parses and applies in chunks of this size rather than
 * gathering every result first. A parse result is a few kilobytes — an arena of
 * names plus three arrays — so holding twenty thousand of them at once is
 * hundreds of megabytes for no reason: the writer applies them one at a time
 * anyway. Chunking makes the structural stage's memory a property of this
 * constant rather than of the repository, which is the bound Atlas claims
 * everywhere else. */
#define ATLAS_CODE_PARSE_CHUNK 512

/* Resolution --------------------------------------------------------------- */
/* Candidate rows kept for one ambiguous relation. `candidate_count` still
 * reports the true number, so the ambiguity is never understated. */
#define ATLAS_CODE_MAX_CANDIDATES 16
/* Symbol names one incremental resolution pass will re-resolve by name before
 * falling back to a whole-repository resolution. The fallback is reported; it
 * is a re-resolution, never a reparse. */
#define ATLAS_CODE_MAX_RESOLVE_NAMES 4096
/* Bounded error/truncation rows retained per repository. */
#define ATLAS_CODE_ERRORS_RETAIN_PER_REPO 1000

/* Compile databases -------------------------------------------------------- */
/* Bytes of compile_commands.json read. Checked before the parser is entered. */
#define ATLAS_CODE_MAX_COMPILE_DB_BYTES (32u * 1024u * 1024u)
/* Translation units recorded from one compile database. */
#define ATLAS_CODE_MAX_COMPILE_UNITS 20000
/* Entries in one unit's `arguments` array that will be walked. */
#define ATLAS_CODE_MAX_COMPILE_ARGS 4096
/* Include directories and defines recorded per unit. */
#define ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT 256
#define ATLAS_CODE_MAX_DEFINES_PER_UNIT 512
/* Bytes of one recognised argument value (a directory, a define). */
#define ATLAS_CODE_MAX_ARG_BYTES 1024u

/* Queries ------------------------------------------------------------------ */
/* Hard ceiling on caller-selected traversal depth. */
#define ATLAS_CODE_MAX_TRAVERSAL_DEPTH 8
#define ATLAS_CODE_DEFAULT_TRAVERSAL_DEPTH 2
/* Nodes one traversal will visit before reporting truncation. */
#define ATLAS_CODE_MAX_TRAVERSAL_NODES 2000
/* Rows one structural query may return, and the default when none is asked for. */
#define ATLAS_CODE_MAX_ROWS 200
#define ATLAS_CODE_DEFAULT_ROWS 50

/* --- A4: decision documents, revisions and operator approval --------------
 *
 * Every one of these is a ceiling on something a model or an operator writes,
 * and every one of them is reported when it is reached rather than silently
 * applied. A decision document is durable and canonical, so an unbounded field
 * here is an unbounded row in the one part of the index that is never rebuilt
 * from anything.
 */

/* The public identifier Atlas assigns a decision document: a fixed prefix and
 * lowercase hex. Bounded, opaque, and composed only of bytes the automatic
 * context envelope's allowlist already permits, which is what makes it the one
 * decision-shaped value that may appear there.
 *
 * **32 hex characters — 128 bits.** These identifiers are durable, they are
 * exported into Markdown and JSON that leaves the machine, and databases get
 * merged, restored from backups and compared across machines. 64 bits is
 * comfortable for one database and is not comfortable for a population of them:
 * the birthday bound puts a fifty-percent collision at about four billion
 * identifiers at 64 bits, and at about two hundred sixty quintillion at 128.
 * The extra sixteen characters cost nothing anybody will notice.
 *
 * It is an **identifier, not a secret**. It is unguessable in practice, and
 * nothing anywhere treats knowing one as authorisation. */
#define ATLAS_DECISION_UID_HEX 32u
#define ATLAS_DECISION_UID_PREFIX "atlas-dec-"
/* strlen(prefix) + hex + NUL. */
#define ATLAS_DECISION_UID_MAX (10u + ATLAS_DECISION_UID_HEX + 1u)
/* How many times a create will re-derive a uid after a UNIQUE collision before
 * giving up. A collision at 128 bits means the entropy source is broken rather
 * than that Atlas got unlucky, so the retry exists to survive a transient fault
 * and the ceiling exists so a broken one is reported instead of spun on. */
#define ATLAS_DECISION_UID_MAX_ATTEMPTS 8

/* Revision prose. Bounded separately because they are read differently: a title
 * is shown in a list, the rest only when a caller asks for one document. */
#define ATLAS_DECISION_TITLE_MAX 200u
#define ATLAS_DECISION_TEXT_MAX 4096u
/* Alternatives considered: a list, in the order the proposer gave. */
#define ATLAS_DECISION_MAX_ALTERNATIVES 16
#define ATLAS_DECISION_ALTERNATIVE_MAX 512u
/* Links from one revision to paths, commits, change sets, symbols and other
 * decision documents, of every kind together. */
#define ATLAS_DECISION_MAX_LINKS 128
/* Revisions one document may accumulate. A document that needs more than this
 * is two documents. */
#define ATLAS_DECISION_MAX_REVISIONS 1000
/* Migration 10: the reason one decision was related to another, or the reason a
 * relation was withdrawn. Sized like an alternative rather than like a text
 * field: it explains one edge, and a paragraph that needs more than this is
 * making an argument that belongs in the decision itself. Refused when it does
 * not fit, never truncated — a bound that silently shortens an explanation
 * produces a record that reads as complete and is not. */
#define ATLAS_DECISION_EDGE_NOTE_MAX 1024u
/* How many edge events one read returns. The history of one document's
 * relations, bounded like every other listing. */
#define ATLAS_DECISION_EDGE_EVENTS_MAX 512

/* The searchable projection of one revision. Bounded so that the degraded
 * (non-FTS5) search is a bounded scan of a narrow table rather than of the
 * revision prose itself. */
#define ATLAS_DECISION_HAYSTACK_MAX 2048u
/* Bytes of a search query Atlas will accept. */
#define ATLAS_DECISION_QUERY_MAX 128u

/* Rows one decision query may return, and the default when none is asked for. */
#define ATLAS_DECISION_MAX_ROWS 200
#define ATLAS_DECISION_DEFAULT_ROWS 50
/* Timeline events returned for one document. */
#define ATLAS_DECISION_MAX_EVENTS 500

/* --- the operator channel ---
 *
 * A challenge is a short-lived, single-use capability bound to exactly one
 * (repository, document, revision, content hash) tuple. */

/* Random bytes in a challenge token, hex encoded. */
#define ATLAS_DECISION_CHALLENGE_BYTES 16u
#define ATLAS_DECISION_CHALLENGE_HEX (ATLAS_DECISION_CHALLENGE_BYTES * 2u)
/* How long a challenge is valid. Long enough to read a confirmation prompt,
 * short enough that one left on a terminal is not a standing capability. */
#define ATLAS_DECISION_CHALLENGE_TTL_MS 120000
/* Unconsumed challenges retained before the oldest expired ones are pruned.
 *
 * A whole-database bound rather than a per-repository one, and deliberately:
 * an unspent capability is not a fact about a repository, it is a fixed-size
 * cost, and a per-repository budget would let the total grow with the number of
 * registered worktrees. Pruning removes only *expired, unconsumed* rows — a
 * consumed challenge is part of an approval record and is never removed. */
#define ATLAS_DECISION_CHALLENGES_RETAIN 200
/* Hex characters of the content hash the operator must type back. Short enough
 * to type, long enough that typing it is a deliberate act about one revision. */
#define ATLAS_DECISION_CONFIRM_HEX 8u
/* Bytes of the confirmation line read from the terminal. */
#define ATLAS_DECISION_CONFIRM_MAX 64u

/* Depth of the document-supersession chain Atlas will walk when checking that a
 * supersession would not create a cycle. A chain longer than this is refused
 * rather than followed, because an unbounded walk over attacker-influenced
 * data is not a check. */
#define ATLAS_DECISION_MAX_SUPERSEDE_DEPTH 64

/* Decision identifiers reported in the automatic file context envelope. Opaque
 * ids only, and never any decision prose. */
#define ATLAS_DECISION_CONTEXT_MAX_IDS 8

/* --- A6: impact gates and stale-decision detection ------------------------
 *
 * Every bound here decides an answer rather than trimming one. An assessment
 * that reaches a ceiling does not report a smaller result; it reports UNKNOWN,
 * and a gate that sees an UNKNOWN is BLOCKED. So each of these is the point at
 * which Atlas stops being able to prove something, and the cost of setting one
 * too low is a refusal, which is the direction a safety gate is allowed to
 * fail in.
 */

/* Commits walked back from the indexed head while looking for a decision's
 * validation point.
 *
 * Reaching this is not "the commit is not an ancestor" — it is "Atlas stopped
 * looking", and those are different answers. The walk that hits it yields
 * UNKNOWN. */
#define ATLAS_GATE_MAX_ANCESTRY_COMMITS 20000

/* Distinct repository paths collected from the commits in one change range.
 *
 * A range that touched more paths than this is one Atlas cannot enumerate, and
 * a change set it cannot enumerate is one it must not test membership against:
 * every miss would be indistinguishable from a path that was never there. */
#define ATLAS_GATE_MAX_CHANGED_PATHS 50000

/* Structural traversal from a decision's own anchors while looking for a
 * changed dependency. Deliberately shallower than A3's ceiling: this walk runs
 * once per assessed decision rather than once per user question. */
#define ATLAS_GATE_DEFAULT_IMPACT_DEPTH 3
#define ATLAS_GATE_MAX_IMPACT_DEPTH ATLAS_CODE_MAX_TRAVERSAL_DEPTH
#define ATLAS_GATE_MAX_IMPACT_NODES 2000

/* Reason codes carried by one assessment. The vocabulary is closed and small;
 * this bounds how many of it one verdict may cite at once. */
#define ATLAS_GATE_MAX_REASONS 12
/* Bytes of the packed reason list stored on a challenge or a validation row.
 * Reason codes are Atlas literals from a closed vocabulary, so this is a
 * storage bound rather than a trust one. */
#define ATLAS_GATE_MAX_REASON_TEXT 512u

/* Decisions one `gate check` will assess. A repository with more approved
 * decisions than this is reported as a limit reached, which is UNKNOWN, which
 * is BLOCKED — never as a pass over the ones that fitted. */
#define ATLAS_GATE_MAX_DECISIONS 2000

/* Path prefixes one gate query may be scoped to. */
#define ATLAS_GATE_MAX_SCOPE_PATHS 64

/* --- A8-CI: compiler-aware semantic intelligence ---------------------------
 *
 * Every bound here refuses rather than clamps, and every one that is reached is
 * *reported* — A5's rule about `--older-than` and A6's about a truncated walk,
 * for the same reason. A discarded number nobody is told about turns "Atlas
 * stopped looking" into "Atlas looked and found nothing", and those are
 * different answers a caller acts on differently. */

/* Wall clock for one translation unit's child parser. Reaching it is
 * ATLAS_SEM_WHY_TIMEOUT and the unit is FAILED, never quietly skipped.
 * The observed worst case on the acceptance corpus is 1.5 s. */
#define ATLAS_SEM_PARSE_TIMEOUT_MS 120000
/* Silence from the child before it is presumed wedged. Separate from the wall
 * clock because the two catch different failures — A8's argument in proc.h. */
#define ATLAS_SEM_PARSE_IDLE_MS 30000
/* Address space the child parser may map, in bytes. libclang holds an entire
 * translation unit's AST in memory, and a pathological generated header is the
 * input that finds the ceiling. */
#define ATLAS_SEM_PARSE_MAX_ADDRESS_SPACE (4ull * 1024ull * 1024ull * 1024ull)
/* NDJSON one child may emit. Bounds the parent's read as well as the child's
 * write, so neither side depends on the other's good behaviour. */
#define ATLAS_SEM_PARSE_MAX_STDOUT (256u * 1024u * 1024u)
/* Facts one translation unit may contribute. */
#define ATLAS_SEM_MAX_FACTS_PER_UNIT 2000000
/* Translation units one generation may hold. */
#define ATLAS_SEM_MAX_UNITS 50000
/* Bytes of one USR, one symbol name and one type spelling. A USR encodes nested
 * scopes, so it is allowed to be considerably longer than a name. */
#define ATLAS_SEM_MAX_USR_BYTES 2048u
#define ATLAS_SEM_MAX_NAME_BYTES 512u
#define ATLAS_SEM_MAX_TYPE_BYTES 1024u
/* Compilation databases one repository may present. */
#define ATLAS_SEM_MAX_COMPDBS 32
/* How deep the include closure is followed when deciding whether a translation
 * unit's inputs changed.
 *
 * This is a *correctness* bound, not a performance one. The closure is what
 * makes "a header changed" invalidate every unit that reaches it, however
 * indirectly — C projects nest headers five and ten deep, so a shallow walk
 * would carry a stale unit forward and report it COMPLETE. Reaching this depth
 * is reported, and a unit whose closure was truncated is reparsed rather than
 * reused: an input set Atlas could not finish enumerating is one it must not
 * claim is unchanged. */
#define ATLAS_SEM_MAX_INCLUDE_DEPTH 64
/* Candidate targets recorded for one indirect call site. The true number is
 * reported even when it exceeds this — A3's rule about `candidate_count`: a
 * bound that makes an ambiguity look smaller than it is is a bound that lies. */
#define ATLAS_SEM_MAX_INDIRECT_CANDIDATES 32
/* Rows written per transaction while a generation is applied. A1's rule about
 * never holding a write transaction across unbounded work. */
#define ATLAS_SEM_APPLY_BATCH 4000

/* --- bounded semantic queries --- */

/* Default and hard ceiling for call-graph traversal depth. */
#define ATLAS_SEM_DEFAULT_DEPTH 3
#define ATLAS_SEM_MAX_DEPTH 16
/* Nodes and edges one traversal may visit. */
#define ATLAS_SEM_MAX_NODES 5000
#define ATLAS_SEM_MAX_EDGES 20000
/* Rows one query returns, and paths one trace returns. */
#define ATLAS_SEM_MAX_ROWS 500
#define ATLAS_SEM_MAX_PATHS 16
/* Wall clock for one bounded query. Reaching it is an explicit truncation. */
#define ATLAS_SEM_QUERY_TIMEOUT_MS 10000
/* Bytes one query result may occupy. */
#define ATLAS_SEM_MAX_RESULT_BYTES (8u * 1024u * 1024u)

/* --- the task context builder --- */

/* Default and maximum byte budget for one context package. The token budget a
 * caller gives is converted at ATLAS_SEM_BYTES_PER_TOKEN and then treated as
 * bytes, because bytes are what Atlas can actually count. */
#define ATLAS_SEM_CONTEXT_DEFAULT_BYTES (32u * 1024u)
#define ATLAS_SEM_CONTEXT_MAX_BYTES (512u * 1024u)
#define ATLAS_SEM_BYTES_PER_TOKEN 4
/* Items one context package may hold, and terms one task description
 * contributes to ranking. */
#define ATLAS_SEM_CONTEXT_MAX_ITEMS 400
#define ATLAS_SEM_CONTEXT_MAX_TERMS 64
/* A9.1. How far the knowledge pass reaches: how many distinct anchor paths one
 * package asks about, and how many records it takes per anchor.
 *
 * Two separate bounds because they bound two different costs. The anchor count
 * bounds the number of queries, and it is small because the seeds are already
 * ranked — the files a task is about are at the top, and asking about four
 * hundred of them would spend the package's whole budget on records anchored to
 * something the reader will never open. The per-anchor count bounds one file's
 * records, and a file with more knowledge recorded against it than this is a file
 * whose records a reader should list explicitly with `decision for-file` rather
 * than meet inside a context package.
 *
 * Both are reported when reached: the package's `missing` list already carries
 * ATLAS_SEM_MISSING_BUDGET, and neither of these silently trims a result that
 * looks complete — an anchor beyond the ceiling contributes no item, and the item
 * count and budget accounting say what was actually included. */
#define ATLAS_SEM_CONTEXT_MAX_DECISION_ANCHORS 16u
#define ATLAS_SEM_CONTEXT_MAX_DECISIONS_PER_ANCHOR 8
/* Bytes of one task description Atlas will read. Longer is a usage error, not a
 * silent truncation: a ranked answer to half a question is worse than a
 * refusal. */
#define ATLAS_SEM_CONTEXT_MAX_TASK_BYTES 8192u
/* Starting paths and symbols one context request may name. */
#define ATLAS_SEM_CONTEXT_MAX_SEEDS 64

/* --- long-running daemon operations ----------------------------------------
 *
 * Operation records the daemon keeps in memory, oldest evicted first.
 *
 * Sized for what an operator can actually be waiting on: at most one backup and
 * one semantic index run at a time, so this is a history rather than a working
 * set, and 64 covers a long session of both without the table becoming a place
 * where memory grows without an owner.
 *
 * When the ring wraps, the evicted id becomes unknown — which is the same
 * answer a restart gives for every id, so a client already has to handle it.
 * The bound is never silently applied to a *result*: no operation is ever
 * dropped or truncated, only the memory of one that already finished. */
#define ATLAS_OPS_MAX_RECORDS 64u

/* How long a client waits between polls of a long operation, and how long it
 * keeps polling before giving up and saying so.
 *
 * The wait is a ceiling on politeness rather than on the operation: the client
 * prints what it knows and exits non-zero if it is reached, and the operation
 * keeps running — which is the whole point of the split. A full semantic index
 * of a large repository was measured at 141 s, so the ceiling is set well above
 * it and is still not a claim about how long indexing takes. */
#define ATLAS_OPS_POLL_INTERVAL_MS 500
#define ATLAS_OPS_CLIENT_WAIT_MS 1800000

/* --- A9: the remote gateway -------------------------------------------------
 *
 * These are the **absolute** ceilings. `/etc/atlas/gateway.conf` may lower any
 * of them and may never raise one — A8's rule, so the policy decides how much a
 * deployment permits and this header decides how much the policy may permit. A
 * gateway that could widen its own bounds is not bounded by anything.
 *
 * Every bound here is refused rather than clamped where a caller supplies the
 * value, and reported where the gateway reaches it. */

/* One HTTP request line, one header line, and the whole header block.
 *
 * Small on purpose. A request line is a method, a path and a version; a header
 * is a name and a value. Anything larger is not a request Atlas serves, and
 * accepting it would mean buffering bytes chosen by an unauthenticated peer. */
#define ATLAS_GW_MAX_REQUEST_LINE 8192u
#define ATLAS_GW_MAX_HEADER_LINE 8192u
#define ATLAS_GW_MAX_HEADERS 64u
#define ATLAS_GW_MAX_HEADER_BYTES 32768u

/* One request body. An MCP message is a JSON-RPC document; a web API call
 * carries almost nothing. The ceiling is checked against `Content-Length`
 * *before* a byte of body is read, so an attacker cannot make Atlas allocate by
 * claiming a large request — the same rule the Unix-socket framing follows. */
#define ATLAS_GW_MAX_BODY_BYTES (1024u * 1024u)

/* One response body the gateway will assemble. A result that does not fit is a
 * structured statement that it does not fit, never a truncation. */
#define ATLAS_GW_MAX_RESPONSE_BYTES (8u * 1024u * 1024u)

/* Connections served at once, and how long one may occupy a slot.
 *
 * Bounded concurrency is what stops a slow-loris from costing more than a fixed
 * amount of memory and a fixed number of descriptors. A connection that has not
 * finished sending its head, or has gone quiet mid-body, loses its slot. */
#define ATLAS_GW_MAX_CONNECTIONS 64
#define ATLAS_GW_HEADER_TIMEOUT_MS 10000
#define ATLAS_GW_BODY_TIMEOUT_MS 30000
#define ATLAS_GW_IDLE_TIMEOUT_MS 15000

/* How long the gateway will wait for the daemon to answer one forwarded call.
 * Longer than a read needs and shorter than a client's patience. */
#define ATLAS_GW_UPSTREAM_TIMEOUT_MS 60000

/* Requests one peer may make per minute, and the ceiling on that ceiling.
 *
 * Behind a reverse proxy every request appears to come from the proxy unless
 * `trust_forwarded_for` is set, so this degrades to a global limit. That is
 * stated in `docs/remote-access.md` rather than hidden: a limit that looks
 * per-peer and is not is worse than one nobody believed in. */
#define ATLAS_GW_DEFAULT_RATE_PER_MINUTE 600
#define ATLAS_GW_MAX_RATE_PER_MINUTE 60000

/* Browser sessions held in gateway memory, and how long one lives.
 *
 * In memory and forgotten on restart, deliberately — the reason A8-CI's
 * operations table is. A durable session would need a durable secret and a
 * table to hold it, and re-authenticating after a gateway restart is the
 * correct experience for an operator tool. */
#define ATLAS_GW_MAX_SESSIONS 32u
#define ATLAS_GW_DEFAULT_SESSION_TTL_SECONDS 43200 /* 12 hours */
#define ATLAS_GW_MAX_SESSION_TTL_SECONDS 604800    /* a week; past this, a typo */
/* The opaque browser session token: 32 bytes of kernel randomness, hex. */
#define ATLAS_GW_SESSION_TOKEN_BYTES 32u
#define ATLAS_GW_SESSION_TOKEN_HEX 64u

/* --- A9.2: claims, attestations, evidence and verification ----------------
 *
 * A6's rule governs every bound here: **a limit that is reached is reported**.
 * A truncated reason list, a claim with more attestations than fit, or an
 * evidence graph deeper than the walk allows must never look like a smaller
 * problem than it is — that is the one property a verification engine cannot
 * afford to lack, because its whole output is a statement about how much
 * evidence there is. */

/* Reasons kept on one aggregate. The *fold* happens before a reason is stored,
 * so exceeding this weakens the verdict exactly as much as fitting would; what
 * is lost is only the enumeration, and `reason_total` reports the true count. */
#define ATLAS_VERIFY_MAX_REASONS 12u

/* Text ceilings. A claim is one discrete proposition: if it does not fit in
 * this, it is a topic rather than a claim and splitting it is the fix. Refused,
 * never truncated — a silently shortened proposition is a different one. */
#define ATLAS_VERIFY_CLAIM_TEXT_MAX 4096u
#define ATLAS_VERIFY_SCOPE_MAX 1024u
#define ATLAS_VERIFY_DOMAIN_MAX 64u
#define ATLAS_VERIFY_METHOD_MAX 1024u
#define ATLAS_VERIFY_NAME_MAX 256u
#define ATLAS_VERIFY_VERIFIER_INPUT_MAX 2048u

/* Attestations and evidence folded into one aggregation, and evidence
 * dependency edges walked while grouping.
 *
 * Bounded because aggregation runs inside a read that a remote client can ask
 * for, and an unbounded fold over a table anybody may append to is a request
 * that costs whatever the table costs. Reaching either bound is
 * `ATLAS_VREASON_...` noted on the result and reported, so a partial fold never
 * reads as a complete one. */
#define ATLAS_VERIFY_MAX_ATTESTATIONS 512u
#define ATLAS_VERIFY_MAX_EVIDENCE 2048u
#define ATLAS_VERIFY_MAX_DEP_EDGES 4096u

/* Claims examined when assessing one knowledge record. */
#define ATLAS_VERIFY_MAX_CLAIMS 256u

/* Rules a root-owned verification policy may carry. Small on purpose: the
 * phase's guidance is that a deterministic enforcement allowlist should be
 * narrow enough for a reviewer to read in one sitting, and a ceiling that makes
 * a sprawling one impossible is worth more than advice saying not to. */
#define ATLAS_VERIFY_MAX_POLICY_RULES 32u

/* The default ceiling on how old evidence may be and still bear on a claim
 * about the present, in seconds. An operational fact observed a fortnight ago
 * says little about a system redeployed since. Policy may lower it per kind;
 * this is what applies when policy says nothing. */
#define ATLAS_VERIFY_DEFAULT_MAX_EVIDENCE_AGE 86400

#endif /* ATLAS_LIMITS_H */

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
/* A9.2.6. How often a caller waiting for the writer re-examines what the writer
 * is actually doing.
 *
 * The serve loop dispatches one request at a time, so every millisecond it
 * spends waiting for the writer is a millisecond no other client is served —
 * including `daemon.ping`. A waiter therefore does not sleep out its whole
 * timeout in one call: it wakes on this interval and asks whether the writer has
 * entered a job whose duration Atlas cannot state, which is the one condition
 * under which continuing to wait is pointless rather than merely slow.
 *
 * Short enough that the head-of-line stall it bounds is below any figure a
 * client would call unresponsive, long enough that an idle daemon is not woken
 * for nothing. It is a responsiveness bound, never a correctness one: a waiter
 * that never woke early would still return the right answer, just later. */
#define ATLAS_WRITER_WAIT_SLICE_MS 100
/* How long a waiter gives an unbounded job to reach a yield point before it
 * takes its own job back out of the queue and reports a refusal.
 *
 * A9.2.6 had no such grace: the slice above *was* the answer, so a write that
 * arrived a millisecond into a semantic pass was refused a tenth of a second
 * later and every one of them was, for minutes at a time. That was honest and it
 * was also the whole cost — a recovery sweep that failed every twenty seconds
 * for a pilot window, a submission that needed sixteen attempts across
 * forty-seven seconds. The pass now yields between translation units and between
 * chunks of a discovery walk, so the interesting question stopped being "is the
 * writer busy?" and became "will it come back soon enough?".
 *
 * Measured from the waiter's **first observation** of an unbounded stretch, not
 * from the moment its job was queued: a job queued a second before a pass starts
 * has not yet been made to wait for anything, and charging it for that second
 * would refuse writes that were about to be served.
 *
 * Two seconds because it must sit between two figures Atlas already holds. The
 * gap between two yield points is one translation unit's parse — milliseconds
 * for an ordinary unit — so a pass that is yielding at all comes back well
 * inside it. And the smallest synchronous deadline on the writer path is a
 * hook's `AI_WRITE_TIMEOUT_MS` of 4000 ms, so a waiter that spends its whole
 * grace still backs out with time left to report the refusal rather than
 * timing out — which matters because those two answers mean opposite things
 * about whether the write is still on its way.
 *
 * The cost is stated rather than hidden: inside a non-yielding stretch — one
 * unit that parses for up to `ATLAS_SEM_PARSE_TIMEOUT_MS` — a refusal now takes
 * about this long instead of about a slice. */
#define ATLAS_WRITER_YIELD_GRACE_MS 2000
/* Hashing/stat worker threads. Clamped against the online CPU count. */
#define ATLAS_WORKER_COUNT_MAX 8u
#define ATLAS_WORKER_COUNT_DEFAULT 4u
/* Jobs queued for the worker pool. */
#define ATLAS_WORKER_QUEUE_MAX 8192u
/* Rows applied inside one write transaction. Bounded so a transaction can never
 * be held open across an unbounded amount of work. */
#define ATLAS_DB_BATCH_MAX 256

/* Watcher ---------------------------------------------------------------- */
/* P0. The watch budget, in four separate constants, because `ATLAS_WATCH_MAX_DIRS`
 * was one number answering four different questions and getting three of them
 * wrong.
 *
 * It was documented as a per-repository ceiling and enforced against the
 * *daemon-global* map count, with a `+1 >=` comparison that made the real
 * ceiling 8191. On a machine with `fs.inotify.max_user_watches` at 122910 and
 * two registered repositories, the second repository — chosen by `ORDER BY
 * name`, not by need — was permanently degraded and its index permanently not
 * current, while Atlas used 6.7% of the watches the kernel would have given it.
 *
 * The replacement separates the resource question (how many watches may this
 * machine spend?) from the correctness question (which watches must exist for a
 * branch switch to be observed?), and derives the default from the kernel
 * rather than from a number somebody picked.
 *
 * Every figure below rests on a measurement taken on the machine this was
 * written for: 60000 fresh directories, then 60000 inotify watches with Atlas'
 * exact mask, `/proc/meminfo` Slab deltas in two phases — 1452 B per directory
 * for the inode and dentry a watch then *pins* against reclaim, plus 67 B for
 * the mark itself. **1519 B, call it 1.5 KiB, per watched directory.** The
 * widely repeated "about a kilobyte per watch" is an under-estimate, and a
 * budget built on it would be half again as expensive as advertised. */

/* The compiled refusal ceiling. A root-owned policy may not exceed it.
 *
 * This is NOT a support claim and must never be quoted as one: it is the point
 * past which Atlas refuses a configured value, nothing more. What Atlas has
 * actually been measured to do is `ATLAS_WATCH_PROVEN_ENVELOPE_DIRS` below, and
 * the two are reported as separate fields for exactly that reason. */
#define ATLAS_WATCH_DIRS_HARD_CEILING 262144u

/* The acceptance-proven product envelope: what `scripts/perf-watch.sh` asserts,
 * in both a deep and a wide tree shape, and the only figure documentation may
 * claim.
 *
 * It is a claim about a *runtime* condition, not about the binary: it holds only
 * where the resolved `effective_total` actually reaches it. On a machine whose
 * kernel or policy budget resolves lower, Atlas reports the effective envelope
 * it really has and claims nothing beyond it. */
#define ATLAS_WATCH_PROVEN_ENVELOPE_DIRS 65536u

/* Floor and ceiling for the kernel-derived default, before policy.
 *
 * MIN so that a tiny or unreadable `max_user_watches` still yields a working
 * watcher — the kernel then refuses with ENOSPC, which is reported as itself.
 * SOFT_MAX so that a machine with a very large limit does not have Atlas
 * silently claim a quarter of a million watches (384 MiB pinned) because nobody
 * said otherwise. A default is what Atlas chooses unasked, and choosing large
 * unasked is how a resource decision becomes a surprise. */
#define ATLAS_WATCH_TOTAL_MIN 1024u
#define ATLAS_WATCH_TOTAL_SOFT_MAX 262144u

/* The share of `fs.inotify.max_user_watches` the derived default claims,
 * expressed as a percentage so the arithmetic stays in integers.
 *
 * Two values, because the uid means two different things. Under A7.1 a system
 * deployment runs the daemon as its own `atlasd`, which has no other consumer of
 * its inotify budget, so claiming half of it costs nobody anything. A per-user
 * install shares the uid with the operator's editor, IDE, language servers and
 * file manager — all of which watch trees too — so it claims a fifth. The
 * distinction is read from the root-owned policy Atlas already loads, not from
 * a new setting. */
#define ATLAS_WATCH_KERNEL_SHARE_PCT_SYSTEM 50u
#define ATLAS_WATCH_KERNEL_SHARE_PCT_USER 20u

/* Metadata watches: the reserve is a floor, not a cap, and they are different
 * numbers on purpose.
 *
 * RESERVE is held back from the source pool so that HEAD, the index, `refs/` and
 * `info/exclude` can always be watched: a repository whose source tree exhausts
 * the budget must still observe its own branch switches. Before P0 the metadata
 * watches were installed *after* the unbounded source walk, so a large
 * repository silently stopped watching HEAD — branch correctness was contingent
 * on the source tree fitting.
 *
 * MAX is the separate explicit ceiling. A repository needing more than this many
 * metadata directories has a pathological ref layout and is told so by name;
 * at the measured cost that is 24 MiB of metadata watches for one repository. */
#define ATLAS_WATCH_META_RESERVE_PER_REPO 256u
#define ATLAS_WATCH_META_MAX_PER_REPO 16384u

/* Repositories this watcher will observe.
 *
 * 256 and not more because every product of the bounds has to stay inside the
 * hard ceiling with no arithmetic edge: 256 x ATLAS_WATCH_META_RESERVE_PER_REPO
 * is 65536, comfortably below ATLAS_WATCH_DIRS_HARD_CEILING, whereas 1024 would
 * make the reserve exactly the ceiling and the source pool exactly zero. It is
 * also the value `wd_slot.sub_count` must be able to represent, which is checked
 * against its type in `tests/test_watch_budget.c` rather than assumed.
 *
 * Registration is not given a new refusal surface: a repository past this bound
 * is registered as before and reported degraded by the watcher, because losing
 * observation is a watcher fact and refusing `repo add` would be a change to a
 * different contract. */
#define ATLAS_WATCH_MAX_REPOS 256u

/* Directories the resumable priming walk advances per watcher-loop tick.
 *
 * The watcher does not poll inotify while it primes, and the kernel queue holds
 * ATLAS_WATCH_EVENT_QUEUE_MAX events before it overflows — an overflow being
 * global to the instance, so it gaps every repository at once. Chunking is what
 * bounds the drain interval, and it is a correctness measure rather than a
 * performance one: a faster walk would not remove the queue. */
#define ATLAS_WATCH_PRIME_CHUNK_DIRS 512u

/* The depth-first priming frontier, in bytes of pending path.
 *
 * Depth-first rather than breadth-first so that popping truncates the buffer and
 * reclaims as it goes; the old walk kept every consumed path for the length of
 * the traversal. The bound is still stated because a very wide directory puts
 * every sibling on the frontier at once, and a bound that is reached must report
 * itself rather than truncate a tree silently. */
#define ATLAS_WATCH_FRONTIER_MAX_BYTES (32u * 1024u * 1024u)

/* P0. How long a repository waits before Atlas asks git about its ignore rules
 * again, after the last attempt failed.
 *
 * Degrading the repository does not on its own stop the asking: a repository is
 * reached through its pending-ignore queue as well as through the staleness
 * flag, and a failure empties neither. Without a wait, a repository whose git
 * invocation keeps failing -- a deleted working tree, a promisor repository that
 * `atlas_git_open` refuses by design -- costs one process every watcher tick for
 * as long as the condition lasts.
 *
 * Exponential between these two, and reset on any success. A genuine ignore-rule
 * event clears the wait immediately, because that is new information rather than
 * the same question asked again. */
#define ATLAS_WATCH_IGNORE_RETRY_MIN_MS 1000
#define ATLAS_WATCH_IGNORE_RETRY_MAX_MS 60000

/* Directories waiting for an ignore decision, and the bytes of their names.
 *
 * A directory that appears while the daemon runs cannot be judged against the
 * ignore inventory, because that inventory lists paths that *existed* when it
 * was built and this one did not. It waits here until one bounded `git ls-files`
 * per debounce tick answers for the whole queue at once. While anything is in
 * it the repository is priming and its index is not current: a watch that has
 * not been installed yet is a subtree whose events are being missed. */
#define ATLAS_WATCH_MAX_PENDING_IGNORE 4096u
#define ATLAS_WATCH_MAX_PENDING_IGNORE_BYTES (1024u * 1024u)

/* Ignored directory entries held per repository, and their bytes.
 *
 * `git ls-files --directory` collapses an ignored tree to a single entry, so
 * this is roughly 1770x what the repository that motivated P0 actually reports.
 * Past it the repository is degraded and says so: the previous behaviour was to
 * watch the surplus, which spent the watch budget on exactly the trees the
 * ignore set exists to skip. */
#define ATLAS_WATCH_MAX_IGNORED_DIRS 65536u
#define ATLAS_WATCH_MAX_IGNORED_BYTES (8u * 1024u * 1024u)

/* Directories visited by one priming walk, as a multiple of the repository's
 * own share. Separate from the watch budget because they answer different
 * questions: one bounds work, the other bounds a resource, and before P0 both
 * set the same flag and produced the same sentence. */
#define ATLAS_WATCH_DISCOVER_FACTOR 2u
/* Files recorded by one recursive discovery pass. This is the reconciliation
 * pass's default `max_untracked`, and it is not a limit on how many files a
 * repository may hold — a distinction the P0 review had to make explicitly
 * because the two are easy to confuse and only one of them is a watcher bound. */
#define ATLAS_WATCH_MAX_DISCOVER_FILES 20000
/* Raw inotify events buffered between drains. */
#define ATLAS_WATCH_EVENT_QUEUE_MAX 16384u
/* Coalescing window: a burst of writes inside this window becomes one pass. */
#define ATLAS_WATCH_DEBOUNCE_MS 400
/* Upper bound on how long a debounce can be extended by continued activity. */
#define ATLAS_WATCH_MAX_DEBOUNCE_MS 5000
/* Periodic reconciliation, whether or not any event arrived. */
#define ATLAS_WATCH_RECONCILE_INTERVAL_MS 300000

/* A13. How long a mirror may go unrefreshed before Atlas stops calling an
 * index built from it current.
 *
 * **Derived, not chosen.** `ATLAS_WATCH_RECONCILE_INTERVAL_MS` is already
 * Atlas' answer to "how long may a repository go without being re-examined" --
 * the watcher re-reconciles every repository at that cadence whether or not any
 * event arrived. A mirror older than that is older than Atlas' own
 * re-examination period, so the bound is the same number rather than a second
 * opinion about the same question. */
#define ATLAS_SCANNER_MIRROR_MAX_AGE_MS ATLAS_WATCH_RECONCILE_INTERVAL_MS

/* How often Atlas asks a scanner to come back.
 *
 * Half the bound, so a scanner keeping the cadence has one whole missed poll of
 * margin before the mirror is called stale. That is the only content of the
 * number: it is not a guess about how often a tree changes, and nothing here
 * decides how much work a scanner does -- a pass that takes longer simply
 * delays the next one. */
#define ATLAS_SCANNER_POLL_INTERVAL_MS (ATLAS_SCANNER_MIRROR_MAX_AGE_MS / 2)
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

/* A9.2.5. Extra attempts one translation unit gets when its failure was
 * transient — a parse child that died, or one that exceeded its wall clock.
 *
 * **One**, and the bound is compile-time and per unit and per pass, which is
 * what makes a storm impossible rather than unlikely. The retry happens inside
 * the pass that is already running: no durable retry state exists, so a daemon
 * restart cannot find a half-finished retry to be confused about, and no timer
 * anywhere can wake up and try again. A unit that fails twice in one pass is
 * recorded failed exactly as before and the generation is INCOMPLETE, visibly.
 *
 * Higher would buy very little: the failures this covers are memory pressure and
 * load, and a second immediate attempt either finds the pressure gone or does
 * not. What it would cost is bounded compiler runs on a machine that is already
 * short of memory, which is the situation that produced the failure. */
#define ATLAS_SEM_UNIT_TRANSIENT_RETRIES 1

/* A9.2.5. Transient retries one *pass* may spend in total.
 *
 * The per-unit bound alone is not a bound on the pass. A machine under enough
 * memory pressure to kill one parse child will kill many, and
 * `ATLAS_SEM_WHY_TIMEOUT` is worse: a unit that exhausted
 * `ATLAS_SEM_PARSE_TIMEOUT_MS` and is retried spends it again, so the worst case
 * without this is **twice the whole pass's duration** rather than twice one
 * unit's — each doubling inside the write transaction the unit holds while its
 * child runs.
 *
 * 64 is chosen to be generous for the case the retry exists for — a handful of
 * children lost to a transient spike — and far below the point where retrying is
 * the dominant cost. Reaching it means the machine, not the repository, is the
 * problem; the pass finishes with the units it has and says so, and the
 * remaining failures are recorded exactly as they were before A9.2.5. */
#define ATLAS_SEM_PASS_TRANSIENT_RETRIES 64
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

/* --- A9.2.3: the daemon's semantic freshness sweep --------------------------
 *
 * How often the daemon asks whether any configured repository's semantic index
 * has fallen behind. It is a bounded read per repository — a digest over the
 * file index's content hashes and a read of the build description — so the
 * interval is chosen for how quickly an edit should turn into a rebuild rather
 * than for the cost of asking.
 *
 * Deliberately slower than the watcher's debounce and faster than its
 * reconciliation interval: the semantic sweep has nothing to do until the file
 * index has caught up, and holding until it has is part of the plan rather than
 * a delay. Nothing about correctness depends on this number — a sweep that
 * happens late converges late — which is why it is a constant and not a policy
 * key. */
#define ATLAS_SEM_SWEEP_INTERVAL_MS 15000

/* How many configured repositories one sweep will consider.
 *
 * Reached rather than silently applied: a repository dropped from a sweep is one
 * that never rebuilds, and A8-CI's rule is that every bound that is reached is
 * reported. */
#define ATLAS_SEM_SWEEP_MAX_REPOS 256

/* --- A9.2.4: build-input discovery ------------------------------------------
 *
 * A9.2.3 stated that compilation databases are *named, never discovered*, and
 * A9.2.4 reverses that. The refusal is replaced by a bounded search universe,
 * and these constants are what bounds it. Every one of them is **reported when
 * it is reached**, because a walk that stopped early and said nothing would be
 * indistinguishable from a repository that has no further build description —
 * which is the exact confusion this season exists to end.
 *
 * How often the daemon re-walks a repository looking for compilation databases.
 *
 * Far slower than `ATLAS_SEM_SWEEP_INTERVAL_MS` on purpose, and the asymmetry is
 * the design rather than a compromise. The sweep asks a question answered from
 * the index and from a handful of file digests; discovery walks a directory
 * tree, which is the one expensive thing in this layer. Splitting them is what
 * lets an *edited* compilation database move the source identity immediately —
 * its content is digested on every identity computation — while a *newly
 * created* one is noticed at the next discovery pass. That is convergence, not
 * correctness: nothing is ever wrong in between, only later. */
#define ATLAS_SEM_DISCOVERY_INTERVAL_MS 300000

/* Directory depth below the repository root the walk will descend.
 *
 * A build tree is shallow — `build/`, `out/x86_64/`, `cmake-build-debug/` — and a
 * ceiling this generous covers every layout anybody writes while refusing a
 * pathological tree the cost of an unbounded descent. Reached ⇒ PARTIAL. */
#define ATLAS_SEM_DISCOVERY_MAX_DEPTH 12

/* Directory entries the walk will read in total across the whole repository.
 *
 * The bound that actually protects the daemon: depth alone does not, because a
 * wide tree is expensive at depth one. Reached ⇒ PARTIAL. */
#define ATLAS_SEM_DISCOVERY_MAX_ENTRIES 400000

/* Directory entries the walk reads between two offers to yield the writer.
 *
 * The walk holds the writer thread for as long as it runs, and it runs entirely
 * before any transaction opens — which is what makes it safe to hand the thread
 * back mid-walk at all. It is asked per *entry* rather than per file opened,
 * because a tree of empty directories reads entries and opens nothing, and a
 * poll frequency that depended on candidates would never fire there.
 *
 * Compiled in rather than tuned: it is a responsiveness figure, not a
 * correctness one — a walk that never offered would still produce the same
 * candidate list, just with everything else waiting. Small enough that a
 * `readdir` cadence keeps latency-critical writes moving, large enough that the
 * offer is not made once per filename. */
#define ATLAS_SEM_DISCOVER_YIELD_EVERY 256

/* Candidate compilation databases one repository may hold.
 *
 * Distinct from `ATLAS_SEM_MAX_COMPDBS`, which bounds how many are *accepted*
 * into a generation: a candidate that is rejected still costs a row and still
 * has to be shown, so the candidate ceiling is the larger of the two. Reached ⇒
 * PARTIAL. */
#define ATLAS_SEM_DISCOVERY_MAX_CANDIDATES 128

/* A9.2.5. Obstacles one discovery walk records with their exact path.
 *
 * Until this season the walk kept the **first** reason it fell short and no path
 * at all, so a single declared `--exclude` consumed the one slot and masked
 * every unreadable directory for the rest of the walk — which on `/opt/atlas`
 * itself is precisely what happened. A bounded list replaces the scalar, and
 * reaching this bound is itself reported rather than silently trimming: an
 * obstacle list that was truncated without saying so would recreate the hole it
 * exists to close.
 *
 * 32 rather than the candidate ceiling: an operator acts on the first few, and
 * the list is a diagnostic rather than an inventory. */
#define ATLAS_SEM_DISCOVERY_MAX_OBSTACLES 32

/* Operator-declared exclusion prefixes, and vendor prefixes, per repository.
 * Bounded for the reason every stored list in Atlas is: a durable value an
 * operator reads back should not be able to grow without a stated ceiling. */
#define ATLAS_SEM_DISCOVERY_MAX_EXCLUDES 64

/* Bytes of one candidate's repository-relative path, `%XX`-encoded.
 *
 * Encoding can triple a path made entirely of bytes that need escaping, so this
 * is the encoded length rather than the raw one — a bound stated about the wrong
 * representation is a bound that does not hold. A candidate whose path does not
 * fit is *not* silently dropped: it makes the walk PARTIAL, because a path Atlas
 * could not name is a part of the universe it cannot account for. */
#define ATLAS_SEM_MAX_PATH_BYTES 1024u

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

/* A9.2.1. Pieces of evidence one *intake call* may attach to one attestation.
 *
 * Far below `ATLAS_VERIFY_MAX_EVIDENCE`, which bounds a fold over a whole
 * claim; this bounds a single request, and a request is a thing somebody
 * retries. It is reached by refusing rather than by trimming — A5's rule about
 * bounds — because an attestation silently recorded as resting on fewer pieces
 * of evidence than it declared would be grouped as a weaker interpretation than
 * it is, which changes what it is worth without saying so.
 *
 * The public id of a verification object: `atlas-ev-` and a hex digest, plus
 * room for the longer prefixes. Bounded so a uid list can be parsed into fixed
 * storage without allocating per element. */
#define ATLAS_VERIFY_MAX_EVIDENCE_PER_ATTESTATION 64u
#define ATLAS_VERIFY_UID_MAX 96u

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

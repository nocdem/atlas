# The daemon and its IPC protocol

Atlas A1 adds a long-running process. This document is the contract for it: what
the daemon is, what it may and may not do, and exactly what goes over the socket.

## What the daemon is

One process, the same `atlas` binary, running `atlas daemon run` in the
foreground. There is no separate daemon executable, no double fork, no `setsid`
and no pid file.

That is deliberate. systemd already supervises processes, restarts them, collects
their output and reports their state. A service that backgrounds itself
reimplements a worse version of all four and, more importantly, makes its own
failures invisible: a daemon that forks and then dies looks to its parent exactly
like a daemon that forked and then worked.

## Threads, and what each one may touch

| thread | owns | may touch | must not touch |
| --- | --- | --- | --- |
| main | the listening socket, every client connection, a signalfd | read-only database handles, one per request | the writable handle |
| writer | **the only writable database handle** | git, the filesystem, the worker pool | the socket |
| watcher | the inotify descriptor | a read-only database handle, git (to read ignore rules) | any writable handle |
| workers | nothing durable | the filesystem, hashing | any database handle, any process creation |

A SQLite connection is **never** shared between threads. This is structural, not
a convention: the writable handle is created by the writer thread inside
`writer_main` and never escapes it, and readers open their own with
`SQLITE_OPEN_READONLY` so a stray write fails immediately rather than contending
for — or worse, taking — the write lock.

The worker pool is a parallel-for with a barrier, not a queue of futures. A batch
of hash jobs is dispatched and the dispatching thread waits for all of them, so
there is no window in which a result from an abandoned pass can arrive later and
be applied.

## Exactly one writer

Enforced by an advisory `flock(LOCK_EX|LOCK_NB)` on `<data-dir>/atlas.lock`.

- The daemon holds it for its whole lifetime.
- An offline one-shot writer (`atlas scan`, `atlas repo add`, `atlas repo
  remove` with no daemon running) holds it for the duration of its mutation.
- A second writer of either kind is refused with an actionable message. It is
  never queued, and never allowed to proceed anyway.

`flock` rather than a pid file because the kernel releases it when the holder
dies, however it dies. A killed daemon leaves no stale lock to clean up, and
there is no window where a recycled pid makes a live process look like a dead
one. The lock file's *contents* are diagnostics only — who is holding it is
decided by the kernel, never by what the file says.

Read commands do not need the lock. `atlas_ctx_open` in `ATLAS_CTX_AUTO` mode
takes it when it is free (so a first run can create and migrate the database) and
falls back to a read-only handle when the daemon holds it. That is what lets
`atlas status`, `atlas search` and the rest keep working while the daemon runs.

## A9.2.6: what happens to a write while the writer is busy

One writer thread and one FIFO means a write can be queued behind work that takes
minutes — an automatic semantic index pass, or the walk that looks for build
descriptions. The serve loop dispatches one request at a time, so a caller
waiting on the writer is not the only one waiting: **every** client is, including
`daemon.ping`, which touches no database and takes no lock.

That is what used to happen. Measured on Atlas' own repository: ping 26 ms idle,
**3.9 s for every write that arrived during a pass**, with the write itself
failing after 4027 ms — and Claude Code fires a hook, and therefore a session
write, on every event.

A waiter now asks what the writer is doing. If the writer is inside a job with no
statable duration *and* the waiter's job is still in the queue, the waiter gives
the pass `ATLAS_WRITER_YIELD_GRACE_MS` — two seconds — to reach a yield point and
serve it. If none comes in that time, the job is taken back out and the caller is
answered at once:

```
BUSY: the Atlas daemon is performing semantic maintenance and cannot take this
write yet. Nothing was queued and nothing will run, so the request may be sent
again.
```

**That second sentence is the contract.** A `BUSY:` refusal means nothing ran, so
the request may be retried. It is a different claim from the timeout a
synchronous write can still hit — that one means the write *was* accepted and
will be applied, and only the result was abandoned. Retrying it duplicates the
write. The two are never merged into one message, and neither adds an exit code:
both are `internal` (1), typed by the token, as Atlas types every other refusal.

What this does **not** cover, deliberately:

- **Reconciliation, snapshot and maintenance jobs are not treated as unbounded.**
  An incremental reconciliation finishes well inside these timeouts and fires on
  every file change, and refusing writes during one would *drop* hook records
  rather than delay them, because hooks fail open. A first full pass over a large
  tree can therefore still hold the loop up to the waiting caller's timeout.
- **A write during a semantic pass is served at the pass's next yield point, and
  refused only when none arrives within the waiter's grace.** An unbounded job
  now hands the writer thread back at the points where nothing is open —
  between translation units, once before the generation is opened and once after
  the unit loop ends, and every
  `ATLAS_SEM_DISCOVER_YIELD_EVERY` directory entries during a discovery walk —
  and the thread is lent to whatever latency-critical writes are waiting before
  the pass resumes. A hook session record that arrives during a *yielding* pass
  now lands. During a non-yielding stretch it is still refused and, because
  hooks fail open, still lost. **The stretch is bounded by one translation
  unit's parse**, up to `ATLAS_SEM_PARSE_TIMEOUT_MS`; a single very large unit
  is the residual this does not solve, and there is no yield inside a unit
  because the per-unit transaction deliberately spans the parse child.

Ordering is deliberately narrowed, in one direction, and this is the statement of
it. First-in-first-out is preserved **among the kinds a yield may drain** (the
drain scans the queue front to back), and **within every kind**. What a drained
job may now do is pass a *queued unbounded* one: a lease renewal that arrives
behind a queued semantic pass is served before it. Refusing that would reimpose
the starvation the yield exists to end. The two orderings that are load-bearing
outside this file survive untouched, because both are per domain and every
drained domain keeps its own order: the orchestration ledger's writes stay in the
order they were accepted, and so do the decision lifecycle's. A job that backs
out is still excised with the same shifting discipline, and everything a yield
does not drain keeps its position untouched.

`job_kind_is_drainable` in `src/daemon/writer.c` is the whole answer to "which
kinds", it has no `default:`, and every `false` carries its reason at the case.

`docs/engineering-rules.md` carries the rules and the argument for each;
`docs/extending.md` carries the checklist for adding a job kind or a new
synchronous writer call.

## Socket

```
$XDG_RUNTIME_DIR/atlas/atlas.sock
```

When `$XDG_RUNTIME_DIR` is not set, Atlas looks at `/run/user/<uid>` — the
directory a login session would have named — and uses it **only on proof**: it
must exist, not be a symbolic link, be a directory, be owned by this uid, and
grant nothing to group or other. `lstat` rather than `stat`, because following a
link there would let whoever could create one choose the directory.

That is not a relaxation of the rule below; it is the same rule applied to the
one path that rule already names. The variable's absence is an environment
accident rather than evidence the directory is unsafe: a non-login SSH
invocation, a scheduled launch and a hook spawned by an editor all reach a
machine where `/run/user/<uid>` exists and the variable does not, and without
the fallback every hook on such a machine reports the daemon unreachable while
the daemon is running perfectly.

- runtime directory: created 0700, verified to be a directory we own with no
  group or other access, and refused if it is a symlink
- socket: bound under `umask(0077)`, then `chmod` 0600, then **verified** — a
  socket that is still group-readable after that is refused rather than served
- every accepted connection is checked with `SO_PEERCRED` and refused unless the
  peer's UID equals ours

The filesystem permissions and the credential check are independent on purpose.
Either alone would be sufficient today; neither is trusted to be the only one,
because they fail differently — a permission mistake, a bind mount, or a
descriptor handed over by a more privileged process would each defeat exactly one
of them.

**There is still no `/tmp` fallback.** When neither `$XDG_RUNTIME_DIR` nor a
private `/run/user/<uid>` is available, that is an actionable error naming both
and what to do — never a downgrade to a world-writable directory. An endpoint
that can mutate an index does not belong somewhere every local user can write.

### Removing a socket that is in the way

Only a socket, only one we own, and only one that nothing answers on. A symlink,
a regular file, a directory, or a live daemon's socket are all **refused**.
Liveness is decided by trying to connect, not by a pid file, so a daemon that is
answering never has its socket unlinked out from under it.

## Framing

```
offset  0   4 bytes   magic "ATL1"
offset  4   2 bytes   protocol version, big-endian (currently 1)
offset  6   2 bytes   flags, big-endian, reserved, must be zero
offset  8   4 bytes   payload length, big-endian
offset 12   ...       payload: UTF-8 JSON, exactly `length` bytes
```

- The length is validated against `ATLAS_IPC_MAX_REQUEST_BYTES` **before a single
  payload byte is read**, so a claimed length can never become an allocation.
- Reserved flag bits are refused rather than ignored. An old daemon that silently
  dropped a future flag would answer a different question from the one it was
  asked.
- Reads and writes carry deadlines. A partial frame times out; a clean close
  *between* frames is normal and is not an error, but a close *part way through*
  one is.

The serve loop is non-blocking with per-connection state: each connection carries
its own partially-read frame and its own partially-written response. The obvious
alternative — poll, then read a whole frame from whichever connection is ready —
lets one client that sends three bytes and stops hold the entire loop for the
read timeout, which any local process could trigger by accident. There is a
regression test for exactly that.

Backpressure: a connection with an unsent response is not read from until it has
drained, so a client cannot pipeline requests faster than it reads answers and
grow the daemon's memory.

## Requests and responses

```json
{"id": "cli", "method": "repo.state", "params": {"repo": "dna"}}
```

```json
{"id": "cli", "ok": true, "result": { ... }}
{"id": "cli", "ok": false, "error": {"status": 4, "code": "repo", "message": "..."}}
```

`status` is the same stable exit-code contract the CLI uses (see
`docs/architecture.md`), so a caller never has to map two vocabularies.

Requests are parsed with vendored **yyjson** (see
`third_party/yyjson/PROVENANCE.md`). Responses are built with Atlas' own
streaming writer, so the escaping contract established in A0 is the one the
daemon speaks, and there is exactly one implementation of it.

There is no "write these bytes as JSON" primitive anywhere in the response path.
Every value goes through the typed writer.

### Methods

| method | params | result |
| --- | --- | --- |
| `daemon.ping` | — | `pong`, `atlas`, `protocol`, `pid` |
| `daemon.status` | — | version, uptime, socket, repository counts, watch count, write-queue depth and limit, worker count |
| `repo.list` | — | `repositories[]`, each with its full index state |
| `repo.state` | `repo` | one repository's index state |
| `repo.sync` | `repo`, `full?` | `queued`, `sync_seq`, `full` |
| `events.since` | `repo`, `since?`, `limit?` | `events[]`, `count`, `cursor`, `more` |

### A2 methods

Added for the AI adapters. Both of them — `atlas mcp` and `atlas hook` — are
separate processes that hold no database handle at all, so everything they can
do is in this table. They are grouped in `src/ipc/server_ai.c` but looked up
through the *same* dispatch as the methods above: two dispatchers is how a method
comes to behave differently depending on which one found it.

| method | params | result |
| --- | --- | --- |
| `repo.resolve` | `path` | `registered`, and the full index state when it is |
| `repo.search` | `repo`\|`root`, `query`, `limit?` | `results[]`, `count`, `search_mode`, `degraded` |
| `ai.session.open` | identity, `source?` | session, repository, change set |
| `ai.session.touch` | identity, `dedup_key?` | as above |
| `ai.session.close` | identity, `source?` | as above |
| `ai.session.attach` | identity, `source` | as above |
| `ai.session.checkpoint` | identity, `phase` | as above, plus the counters checkpointed |
| `ai.session.get` | identity | the caller's session and its change-set counters, plus `open_sessions` |
| `ai.tool.record` | identity, `tool`, `phase`, `tool_use_id?`, `paths?` | as above |
| `ai.batch.correlate` | identity, `paths?`, `sync?` | changed, direct, ambiguous and unresolved counts |
| `ai.turn.close` | identity, `dedup_key` | as above, having recorded UNKNOWN for what is unexplained |
| `ai.reason.record` | identity, `summary`\|`unknown`, `paths`, `confidence?` | the record id and its provenance |
| `ai.decision.record` | identity, `title`, `statement`, `rationale?`, `paths?` | the record id, and `approved: false` |
| `ai.context` | identity | the bounded context envelope and its size |
| `ai.changed` | `repo`\|`root`, `scope?`, `limit?`, `cursor?` | indexed working-tree changes by git scope |
| `ai.file.context` | `repo`\|`root`, `path`, `limit?` | indexed facts, recorded history, recorded reasons |
| `ai.memory.search` | `repo`\|`root`, `query`, `limit?` | recorded reasons and decisions |

"identity" is `provider`, `client`, `session_key`, and `root` or `repo`; a
session key is safe-encoded and bounded on the way in.

### A3 methods

Seven, and every one of them is a read except `code.sync`. They live in
`src/ipc/server_code.c`, are registered in the same table as everything else,
and are served the same way: reads on the serve loop with their own read-only
handle, the one write submitted to the writer thread.

| Method | Params | Result |
| --- | --- | --- |
| `code.status` | `repo`\|`root` | structural currency, generation, counts, degraded state, compile-database presence, and the analyzer identity that built the graph next to the one this binary would produce |
| `code.sync` | `repo`, `rebuild?` | `queued`, `sync_seq` — the same job the file index uses, with a structural rebuild flag |
| `code.file` | `repo`\|`root`, `path`, `limit?` | roles with their basis, symbols, includes, call candidates, dependents |
| `code.symbol.search` | `repo`\|`root`, `query`, `limit?` | matching symbol sites, each with file, kind, linkage and resolution |
| `code.symbol` | `repo`\|`root`, `symbol`, `limit?` | every recorded site of the name, with callers and callees |
| `code.deps` | `repo`\|`root`, `path`, `depth?`, `reverse?` | bounded traversal outward or inward |
| `code.impact` | `repo`\|`root`, `path`, `depth?` | inbound candidates, split by resolution class |

Every one of them opens with the same preamble — `code_index_current`,
`code_generation`, and a typed reason when it is not current — so no caller can
read a structural answer without also being told how much to trust it. `deps`
and `impact` are the same bounded walk in opposite directions, served by one
function, because two implementations would eventually disagree.

### A session is found by its key, and by nothing else

Every method above resolves the session by exact `(provider, client,
session_key)`. There is no other way to reach one — in particular, **a
repository never identifies a session.**

An earlier version of `ai.reason.record` and `ai.decision.record` accepted no
session key and attached the record to the newest open session for the
repository. With two Claude Code sessions on one worktree that recorded session
A's reason against session B, and the stored row was indistinguishable from a
correct one. The query that did it no longer exists.

Reason and decision records are the two methods that may arrive without a key —
a generic MCP client has no external session id to send, and refusing the write
would lose the content as well as the attribution. They are stored **sessionless**
and say so. Every other method requires a key and is refused without one.

Three fields carry this, on every `ai.*` mutation result:

| field | meaning |
| --- | --- |
| `session` | the Atlas session row id, or `0` |
| `session_unbound` | true when no session was bound. Reported, not inferred from `session == 0` |
| `unbound_reason` | `no_session_id`, `unknown_session`, `session_closed`, or `""` |

`no_session_id` — none was sent. `unknown_session` — one was sent and Atlas has
never opened a session with it, which is what "the hooks are not installed" looks
like. `session_closed` — the session with that id exists and has ended; a reason
or a decision binds only to an **open** session, because those two carry content
about a conversation and binding them to a finished one is a claim Atlas cannot
support. Bookkeeping methods sent by the hooks bind by key whatever the state,
including the `ai.session.close` that does the closing.

`ai.session.get` and `ai.context` resolve the same way, so a read cannot promise
an attachment the following write would refuse. Without a key, `ai.session.get`
reports `present: false` and `session: 0` — and `open_sessions`, the number of
open sessions with that repository attached, which is the most that can be said
truthfully about a repository without knowing who is asking.

A session named by an exact key binds even when it never attached the repository
the record is about. The id proves which conversation this is; it does not prove
the conversation was working there, so `change_set` stays `0` and the repository
is **not** silently attached to the session — an implicit attachment would change
the concurrency accounting for every other session on that worktree.

**Reads run on the serve loop; writes go to the writer.** Every `ai.*` mutation
is validated completely — bounds, provenance, path relativity — *before* anything
is queued, then handed to the writer thread as one typed `atlas_ai_op` with a
four-second deadline. The caller is inside a hook or a tool call that a person is
waiting on, so a truthful timeout beats a stall.

### What `registered` means — and that nothing here sets it

**A7 removed `repo.ensure`, `repo.add` and `repo.remove`.** No method in this
protocol registers or deregisters a repository; `repo.resolve` reports and
nothing more. With them went `exact_root`, which existed only to bound what
`repo.ensure` would register.

The reason is who chooses the input. A session's working directory, a
`DirectoryAdded` payload and the granted-roots list an MCP client answers are all
chosen by, or influenced by, the model — so while those methods existed, "a model
looked here" was sufficient for Atlas to begin opening, reading, hashing and
running git against a tree nobody had vouched for. `docs/security/A7_SECURITY_REVIEW.md`
(ATLAS-A7-002, ATLAS-A7-003) has the reproduction.

Registration is now a local CLI operation under the data-directory write lock,
which the daemon holds while it runs, so `atlas repo add` requires the daemon to
be stopped — the contract A5 already established for `backup restore` and
`maintenance prune`.

`registered` therefore reports one fact and can no longer be conflated with a
second:

| field | means |
| --- | --- |
| `registered` | the repository is in the index, put there by an operator |

`ai.context` returns `repo_id` and `root_hash` rather than the repository name,
for the same reason the envelope does: this response exists so an adapter can
inject `context`, and an adapter that found a name here might inject that too.

**No method here runs git**, and since A7 there is no exception: the one method
that did was `repo.ensure`, which is gone. `repo.resolve` is a pure index lookup — a git process in
the serve loop would let one question stall every other client for the git
timeout.

**Path arguments may be absolute.** An adapter observes absolute paths — a hook is
handed one by Claude, and a model naming a file has no reason to know where Atlas
thinks the root is — so the server relativises them against the repository's own
root bytes from the index. A path outside the repository is dropped rather than
refused: an agent editing a file elsewhere is ordinary, and refusing the whole
request would lose the paths that *are* inside it.

### Building requests with the typed writer

A1's client built its two request documents with `atlas_buf_appendf` and refused
any repository path containing a quote, a backslash or a control byte rather than
escaping it — recorded as item 11 in `docs/backlog.md`. That was defensible when
the only things crossing the socket were a validated name and a path the CLI
could refuse.

A2 sends filesystem paths it did not choose, session identifiers a client chose,
and prose a model wrote. So `atlas_ipc_params_begin`/`_finish` build `params`
through the same first-party streaming writer the daemon answers with, and every
A2 caller uses it. One escaping implementation, both directions.

Responses are read with `atlas_ipc_response_parse`, which applies the same
bounded, hostile-input discipline the daemon applies to requests, and
`atlas_ipc_result_write` re-emits a result into another document *through the
typed writer* rather than copying bytes. There is still no "write these bytes as
JSON" primitive anywhere in Atlas.

**There is deliberately no `daemon.shutdown`.** A remotely reachable stop turns
any local process that can open the socket into something that can disable
indexing. systemd owns the lifecycle and `SIGTERM` already works. A test asserts
that the method does not exist.

`repo.sync` **queues** rather than performs. A pass can take minutes; performing
it inside the serve loop would stall every other client. The returned `sync_seq`
is what `atlas sync --wait` polls `repo.state` for.

`repo.add` and `repo.remove` are not in this protocol at all — see above. They
are local operations that take the data-directory write lock, so a running daemon
makes them refuse rather than route.

## Pagination

`events.since` requests one row more than the limit and never delivers it, so
`more` is a fact rather than an inference from the page being full. Nothing is
ever silently truncated: a response that would exceed the size ceiling is
replaced by a structured error about not fitting.

## Resource limits

Every one of these is a hard ceiling. Nothing is silently truncated when one is
reached: the caller receives a structured error, or the repository enters an
explicit degraded state that `atlas daemon status` reports. They are defined in
one place, `include/atlas/limits.h`.

| bound | value | on reach |
| --- | --- | --- |
| concurrent IPC clients | 64 | connection accepted and closed with a log line |
| request frame | 1 MiB | structured error before any payload read |
| response frame | 8 MiB | structured error, never a truncated document |
| JSON nesting depth | 24 | structured error |
| per-client unsent bytes | response limit + 64 KiB | client dropped |
| read / write deadline | 10 s | client dropped |
| rows per `events.since` | 1000 | `more: true` and a cursor |
| writer queue | 4096 jobs | submission refused, with backpressure reported |
| worker threads | min(4, cores), max 8 | — |
| rows per write transaction | 256 | the transaction commits and a new one begins |
| inotify watches, daemon-wide | derived; see below | degraded state with a reason code, full reconciliation scheduled |
| git metadata watches per repository | 256 reserved, 16384 maximum | `meta_budget` |
| directories awaiting an ignore decision | 4096 / 1 MiB | `pending_ignore_overflow` |
| ignored directory entries per repository | 65536 / 8 MiB | `ignore_overflow` |
| repositories one watcher observes | 256 | `repo_limit` |
| priming frontier | 32 MiB of pending path | `frontier_overflow` |
| discovered files per pass | 20000 | `truncated` with a reason |
| candidate paths per pass | 250000 | `truncated` with a reason |
| debounce window | 400 ms, capped at 5 s | — |
| periodic reconciliation | 5 minutes | — |
| retained raw events per repository | 20000 | oldest pruned; **evidence is never pruned** |
| bytes hashed per file | 256 MiB | recorded with `truncated` and a reason |
| git output / timeout | 64 MiB / 120 s | the pass fails and is retried |

## The watch budget

Until P0 this table said **"inotify watches per repository — 8192"**. It was
wrong in three ways at once, and the wrongness was visible in production for
weeks without reading as a bug.

The check was `w->map.count + 1 >= ATLAS_WATCH_MAX_DIRS`, and `map.count` is the
**daemon-global** watch count. So the bound was not per repository; and `+ 1 >=`
made the real ceiling 8191, which is exactly the number `daemon status` reported.
Repositories were served in `ORDER BY name`, so which repository was left
degraded was decided by its name. On a machine whose kernel offered 122,910
watches, Atlas stopped at 8,191 and told the losing repository it had *more
directories than Atlas will watch* — a sentence in which nothing was true.

### How the budget is resolved

```
K              = fs.inotify.max_user_watches       (read at startup and on each rebuild)
share          = 50%  when the root-owned policy puts this daemon in system mode
                 20%  otherwise
kernel_derived = clamp(K * share, ATLAS_WATCH_TOTAL_MIN, ATLAS_WATCH_TOTAL_SOFT_MAX)
effective      = min(policy watch_max_dirs_total OR kernel_derived,
                     ATLAS_WATCH_DIRS_HARD_CEILING)
```

Derived rather than compiled, because a watch budget written into a header is a
guess about a machine its author never saw. The two shares are the deployment
distinction Atlas already knows: under A7.1 a system daemon runs as its own
`atlasd` with no other consumer of that uid's inotify budget, while a per-user
daemon shares the uid with the operator's editor, IDE and language servers.

A root-owned `/etc/atlas/system.conf` may state `watch_max_dirs_total`. A value
outside `[1024, 262144]` is **MALFORMED, not clamped** — and a malformed policy
falls back to legacy mode entirely, which on a system deployment removes every
`client_uid` from the socket. Install a new binary before adding the key.

A policy value above what the kernel will grant is allowed: root may also raise
the sysctl, and the kernel then refuses with `ENOSPC`, which is reported as
`kernel_limit` rather than as an Atlas budget.

### Allocation between repositories

Two phases, across every repository:

1. **Metadata first.** Every repository's git directory, common directory,
   `info/` and `refs/` tree, drawing on a reserve of 256 held back for it. Before
   P0 these went in *after* the source walk, so a repository whose tree exhausted
   the budget stopped watching its own `HEAD` — branch correctness was contingent
   on the source tree fitting.
2. **Source trees, by rounds.** Each round divides the remaining pool among the
   repositories that still want watches, **after setting aside what each of them
   still owes its metadata reserve**. A repository that finishes under its share
   returns the remainder automatically, so a single large repository alone
   receives the whole budget and no repository's completeness depends on where
   its name sorts.

The reserve is held back on *every* round, not only the first. Meta-first
ordering alone covers the initial build and nothing after it: a `.git/info`
created live, or a new `refs/` subdirectory after a branch is pushed, would find
the budget already spent. Without the reserve, "branch correctness does not
depend on the source tree fitting" would be true for the first minute and false
afterwards.

When the whole budget is smaller than the reserve — a badly under-provisioned
machine — Atlas says so once, with both numbers, and then installs what fits:
metadata takes what there is and source is told `total_budget`. The reserve is a
target for what source may not take, not a precondition for starting. An earlier
draft degraded every repository in that situation without installing anything,
which threw away the metadata watches that were still affordable and were the
ones that mattered most.

### The supported scale, and the ceiling, are different numbers

| Field | Value | What it is |
| --- | --- | --- |
| `ATLAS_WATCH_DIRS_HARD_CEILING` | 262144 | the compiled point past which a configured value is **refused**. Not a support claim. |
| `ATLAS_WATCH_PROVEN_ENVELOPE_DIRS` | 65536 | the acceptance-measured envelope, in a deep and a wide tree shape |

**Documentation may claim the envelope and nothing more**, and the claim holds
only where the resolved `effective_total` actually reaches it. On a machine whose
kernel or policy budget resolves lower, `daemon status` reports the effective
figure and Atlas claims nothing beyond it. Raising the envelope needs an isolated
acceptance environment and a re-run of `scripts/perf-watch.sh`, not an edit here.

### The cost, measured

60,000 fresh directories, then 60,000 watches with Atlas' exact mask, taken from
`/proc/meminfo` Slab deltas in two phases: **1452 B** per directory for the inode
and dentry a watch then *pins* against reclaim, plus **67 B** for the mark
itself — **about 1.5 KiB per watched directory**. The commonly repeated figure of
one kilobyte is an under-estimate, and a budget built on it costs half again what
it advertises. Userspace adds roughly 218 B per watch.

### Watches and subscriptions are different numbers

`watches` counts **physical** inotify descriptors — what the kernel holds and
what it charges against `max_user_watches`. `watch_subscriptions` counts
`(repository, descriptor)` pairs.

They are equal only when nothing is shared, and two registered worktrees of one
repository share every descriptor on their common git directory. So:

```
sum(per-repository watched_directories) == watch_subscriptions >= watches
```

The `>=` is not a hedge. Any surface that treats the two as one number is wrong
for exactly the case linked worktrees create, and `watched_shared` is reported
per repository so the difference is readable rather than looking like an error.

### A status read never waits for the writer, and never speaks for it

Everything the previous section reports comes from the watcher's own counters, so
none of it depends on the writer thread. The *watch state* did: it is published
as a `SET_WATCH` job on the writer's queue, and the section above on what happens
to a write while the writer is busy says how long that queue can be.

`repo.state` and `status` therefore overlay the watcher's live view — priming,
degraded, owes a gap — onto the stored row before deriving `index_current`, so a
repository is never reported current on the strength of a publication that has
not landed. The overlay costs one mutex in the watcher, involves no writer and
does no I/O, so the read stays answerable while the writer is held. It can only
ever claim *less* than the row it was given. `docs/watcher-consistency.md` has
the full contract; the fields it can change are `watch_state` and
`pending_full_reconcile`, and through them `index_current` and
`not_current_reason`.

`watch_owed_gaps` in `status` is the daemon-wide count of obligations the watcher
holds and has not yet seen recorded. `repositories_with_event_gap` beside it
still counts stored rows, which is what its name says; when the writer is held it
is `watch_owed_gaps` that moves.

## Client fallback

`atlas_ipc_daemon_reachable()` answers one question — should this invocation take
the offline path? Every failure mode (no `XDG_RUNTIME_DIR`, no socket, nothing
listening) collapses to "not reachable", because every caller does the same thing
with all three. There is no reconnection logic and no retry: a command that
cannot reach the daemon decides for itself, visibly, in the calling code.

## A2 resource limits

Added in `include/atlas/limits.h` alongside the A1 bounds. Same rule: nothing is
silently truncated when one is reached.

| bound | value | on reach |
| --- | --- | --- |
| automatic context envelope | 4 KiB | the envelope is discarded rather than emitted |
| hook payload | 1 MiB | refused whole; the hook still answers `{}` |
| hook daemon deadline | 2 s (700 ms at session end) | the hook fails open |
| MCP message | 1 MiB | structured error, then the stream resynchronises at the next newline |
| MCP tool result | 128 KiB | structured error naming the ceiling, never a truncated document |
| MCP rows per call | 200 (50 by default) | `more: true` and a cursor |
| MCP daemon deadline | 5 s | the tool reports degraded |
| client roots | 32 | further roots ignored |
| repository prose per field | 512 bytes | bounded excerpt |
| session key / agent id | 128 bytes | refused, never truncated |
| reason summary / detail | 512 / 2048 bytes | refused |
| paths per record | 64 | refused |
| paths per batch | 256 | refused |
| changed paths per change set | 4096 | the change set is marked truncated |
| records per session | 500 | refused with a structured error |
| session events per session | 2000 | oldest pruned; **durable records are never pruned** |
| idle session expiry | 24 h | the session becomes `expired`, not `closed` |

The MCP result ceiling is well below Claude's own 25 000-token MCP output limit,
so an Atlas result is never the thing that fills a context window.

## A3 resource limits

Same rule again, and the traversal bounds matter most: an impact query is the
one place where an honest answer could be the size of the repository.

| bound | value | on reach |
| --- | --- | --- |
| file size parsed | 4 MiB | the file is recorded with `parse_status = failed` and a reason |
| token, name, nesting, symbols and relations per file | see `include/atlas/limits.h` | the file is `partial`, an error row is written, the repository is degraded |
| files parsed per pass | 20 000, in chunks of 512 | the pass reports truncation; the remainder still differs by hash, so the next pass takes it |
| compile database | 64 MiB, 100 000 entries | read up to the ceiling and reported as truncated |
| candidates per ambiguous edge | bounded | `candidate_count` still reports the *true* number, so a bound never makes an ambiguity look smaller than it is |
| traversal depth | 8 requested, hard maximum | clamped, and the clamp is reported |
| traversal nodes and edges | bounded per query | `truncated` with the ceiling named |
| rows per structural response | the A2 row bound | `more: true` and a cursor |
| retained structural errors per repository | `ATLAS_CODE_ERRORS_RETAIN_PER_REPO` | oldest pruned; the `degraded` flag is not pruned with them, because the flag is the durable statement |

The one that is easy to get wrong is the candidate count, and it is worth saying
plainly: the *set* of candidates is bounded and the *count* is not. Reporting
"3 candidates" when there were forty would be a bounded answer that lies.

## Why the MCP adapter holds no database handle

`atlas mcp` could open a read-only handle — CLI read commands do, and it would be
faster. It deliberately does not.

The reason is that a read-only handle is a schema dependency. An adapter holding
one has to migrate in lockstep with the daemon, has to handle a schema it does not
recognise, and has to decide what to do when the daemon is mid-migration. Going
through the socket means the daemon answers those questions once, for everyone,
and the adapter's failure mode is the single one it already has to handle
anyway: the daemon is not there.

It also makes the security argument short. The MCP server cannot open the index,
cannot start a daemon, cannot scan a repository, cannot write to a filesystem and
cannot create a process. That is a list a reviewer can check.


## A4: the `decision.*` method group

Ten methods, in `src/ipc/server_decision.c`, looked up through the same single
dispatch as the other groups.

| method | writes | notes |
| --- | --- | --- |
| `decision.list` | no | also serves search and decisions-for-a-file |
| `decision.get` | no | one whole revision, with every link's resolved currency |
| `decision.history` | no | revisions and the timeline, plus `ledger_agrees` |
| `decision.propose` | yes | a new document at revision 1 |
| `decision.revise` | yes | a new **proposed** revision of an existing document |
| `decision.promote` | yes | an A4 document from an A2 `ai_decisions` row |

Every write is one typed `atlas_decision_op` handed to the writer thread as an
`ATLAS_JOB_DECISION`, exactly as A2's writes are handed over as
`ATLAS_JOB_AI`. Issuing a capability is a write, so it goes through the writer
too: **there is no path to `atlas_decision_apply` that does not run on the
writer thread**, and the whole operation is one transaction, so a capability
cannot be spent without the transition it authorised.

### The parameter surface is the boundary

`token` and `confirmation` are read by the three spending methods and by nothing
else. No proposal method reads them, so "a proposal cannot carry an approval" is
a property of the parameter surface rather than of a check somebody has to
remember.

The `decision.*` methods are reachable over the socket — the CLI is a socket
client like everything else — but the MCP tool surface exposes no tool that
calls the three spending ones, and no MCP tool schema declares a `token` or a
`confirmation` at all.

### The CLI's two routes

The data-directory lock decides. When this process holds it, the operation runs
on this thread through the same `atlas_decision_apply` the writer calls; when
something else holds it, that something is the daemon and the operation goes
over the socket. One of the two is always true, and neither branch has a copy of
the lifecycle rules.

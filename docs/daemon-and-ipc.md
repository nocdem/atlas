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

## Socket

```
$XDG_RUNTIME_DIR/atlas/atlas.sock
```

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

**There is no `/tmp` fallback.** A missing `XDG_RUNTIME_DIR` is an actionable
error that names the likely path and what to do, not a downgrade to a
world-writable directory. An endpoint that can mutate an index does not belong
somewhere every local user can write.

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
| `repo.add` | `path`, `name?` | `repository` |
| `repo.remove` | `repo` | `repository` |
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
| `repo.ensure` | `path`, `name?`, `exact_root?` | as above, registering the worktree when nothing matches |
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

### `exact_root`, and what `registered` means

`repo.ensure` normally resolves **upward**: given a subdirectory it registers the
worktree that contains it, which is what a person running `atlas repo add src/`
means. `exact_root: true` refuses that, before anything is inserted, unless the
given path is itself the worktree root.

The MCP adapter always asks for the exact form. A client that granted one
directory did not grant its parent, and registering the parent would index files
outside what was granted. The session-start hook does not ask for it: a person
who launched a session in a subdirectory means the worktree, and the client's own
file access already spans it.

Every `ai.*` and `repo.*` response distinguishes two facts that used to be
conflated in one always-false field:

| field | means |
| --- | --- |
| `registered` | the repository is in the index, however it got there |
| `registered_now` | *this call* performed the registration |

`ai.context` returns `repo_id` and `root_hash` rather than the repository name,
for the same reason the envelope does: this response exists so an adapter can
inject `context`, and an adapter that found a name here might inject that too.

**No method here runs git**, with one exception: `repo.ensure` routes to the
writer, which runs a handful of `git rev-parse` calls to register a worktree.
`repo.resolve` is a pure index lookup for exactly this reason — a git process in
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

`repo.add` and `repo.remove` are handed to the writer with a 30-second deadline,
because they are fast (a few `git rev-parse` calls and one insert) and a caller
genuinely wants the answer.

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
| inotify watches per repository | 8192 | degraded state, full reconciliation scheduled |
| discovered files per pass | 20000 | `truncated` with a reason |
| candidate paths per pass | 250000 | `truncated` with a reason |
| debounce window | 400 ms, capped at 5 s | — |
| periodic reconciliation | 5 minutes | — |
| retained raw events per repository | 20000 | oldest pruned; **evidence is never pruned** |
| bytes hashed per file | 256 MiB | recorded with `truncated` and a reason |
| git output / timeout | 64 MiB / 120 s | the pass fails and is retried |

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

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

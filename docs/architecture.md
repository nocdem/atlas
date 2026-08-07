# Architecture

## The shape of it

```
                      ┌──────────────┐
   atlas CLI ────────►│   service    │◄──────── future: skill, MCP adapter
   (src/cli)          │ (src/core)   │
        │             └──────┬───────┘
        │                    │
        ▼                    ├──────────────► git adapter (src/git)
   renderers                 │                read-only, no shell
   human │ json              │                       │
   (one interface)           ▼                       ▼
                       db layer (src/db)        git executable
                       typed operations         (source of truth)
                             │
                             ▼
                        SQLite index
                        (rebuildable)
```

Data flows one way: Git and the working tree are read, facts are written to the
index, and queries are answered from the index with provenance attached. Nothing
flows back toward a repository.

## Layers

| Layer | Location | Responsibility | Never does |
| --- | --- | --- | --- |
| CLI | `src/cli` | parse arguments, choose a renderer, call the service | SQL, Git, formatting decisions about content |
| renderers | `src/cli/render_*.c` | turn service results into text or JSON | query anything |
| service | `src/core/service.c` | all command behaviour, one result per command | format output |
| scanner | `src/core/scan.c` | walk the index and history, write facts and evidence | modify a repository |
| git adapter | `src/git` | run read-only Git, parse its output | interpret meaning |
| db | `src/db` | typed operations over the index | leak `sqlite3` types upward |
| core | `src/core`, `src/output` | buffers, errors, paths, hashing, processes, JSON | know about repositories |
| daemon | `src/daemon`, `src/ipc` | the writer thread, the worker pool, the watcher, the socket | answer a question the service layer has not been asked |
| adapters | `src/ai`, `src/mcp`, `src/hook` | the A2 session service and the two Claude Code surfaces | open a database handle of their own (`src/mcp` and `src/hook` do not) |
| code | `src/code` | the A3 lexical C indexer, compile-database reader, resolver and traversal | decide what a fact *means* — it records a resolution class and stops |

The service layer exists so that the CLI's human renderer, its JSON renderer, and
any future adapter observe **identical** results. A renderer receives a service
result and formats it; it cannot reach past the service to ask its own question.
That is what makes "human and JSON cannot disagree" a structural property rather
than a discipline.

## Invariants

These are the rules the code is built to keep. Where one is enforced in code
rather than by convention, that is noted.

1. **SQLite is a rebuildable index, never the canonical record of history.**
   Deleting `atlas.db` loses nothing that cannot be recovered by rescanning.
2. **Git and repository contents are authoritative** for source and history
   facts. When the index and the repository disagree, the repository is right and
   `atlas status` reports the drift.
3. **Every result preserves provenance.** A fact that cannot say where it came
   from is not reported. See [provenance.md](provenance.md).
4. **Atlas distinguishes six evidence types**: `SOURCE`, `GIT`, `DECISION`,
   `USER_STATEMENT`, `INFERENCE`, `UNKNOWN`.
5. **A0 may create only `SOURCE` and `GIT` evidence.** Enforced in code:
   `atlas_db_evidence_insert` refuses any other kind with exit code 7, and a test
   asserts that no other kind can reach the table.
6. **A0 never invents or infers a historical reason.** When a reason is
   requested, the answer is `UNKNOWN`.
7. **No Atlas command modifies a registered repository.** Enforced by an argv
   allowlist checked before each `fork`, and proven by tree-digest tests.
8. **Repository contents, filenames and Git metadata are untrusted input.**
9. **All parsers use bounded memory and fail clearly on malformed input.**
10. **Schema changes use numbered transactional migrations.**
11. **Human and JSON modes consume the same service results.**
12. **The CLI and future integrations share one service layer.**
13. **A structural fact carries a resolution class, and the class is part of the
    fact.** Enforced in code: `resolution` is `NOT NULL` with a `CHECK`
    constraint, and `atlas_code_resolution_writable_in_a3` refuses
    `MODEL_PROPOSAL` on the write path — the same shape as rule 5, one phase
    later. See [code-intelligence.md](code-intelligence.md).

## Error handling and ownership

**Errors.** Every fallible function returns `atlas_status` and takes an
`atlas_err *`. `atlas_err` is a by-value struct with a fixed-size message buffer,
so reporting an error can never itself fail on allocation. Status codes are the
process exit codes and are part of the stable CLI contract.

Cleanup is centralised per function: acquire, then a single exit path (or a
`goto` to one) that releases in reverse order. There is no early `return` that
skips a release.

**Ownership.** The rules are explicit rather than implied:

- `atlas_buf` owns its allocation. Initialise with `ATLAS_BUF_INIT`, release with
  `atlas_buf_free`. Ownership is never transferred by assignment; use
  `atlas_buf_detach` to hand the allocation to a caller.
- Opaque handles (`atlas_ctx`, `atlas_db`, `atlas_git`, `atlas_json`) are created
  by an `_open`/`_new` function and destroyed by the matching `_close`/`_free`.
  The creator owns the handle. A partially constructed handle is destroyed by its
  own destructor, so an error path calls `_close` on it rather than unwinding by
  hand.
- Row callbacks receive **borrowed** pointers valid only for the duration of the
  call, because they point into a live SQLite statement. A callback that needs a
  value beyond its own scope copies it. This is why the file report is built
  inside the row callback rather than returned from it.
- `atlas_json_finish` frees the writer; on the failure path the caller calls
  `atlas_json_free` instead. Exactly one of the two runs.
- Structs with owned members have `_init`/`_free` pairs
  (`atlas_repo_info`, `atlas_doctor_report`, `atlas_status_report`).

Opaque structs are used where the internals must not leak: `atlas_db` hides
`sqlite3`, `atlas_git` hides the process environment and executable path, and
`atlas_json` hides its nesting stack.

## Streaming

Nothing in Atlas builds a complete response in memory:

- Git output is consumed in chunks and split into NUL-delimited records
  incrementally, with a per-record ceiling
- file content is hashed as it is read
- query results are streamed row by row into the renderer, so `--json` on a large
  result set costs output bandwidth rather than RAM
- the JSON writer emits directly to the output stream

The one deliberate exception is a scan, which runs inside a single database
transaction so that a failure leaves no partial scan behind.

## Concurrency

A0 is single-threaded. SQLite is opened with WAL where the filesystem allows it
and a 5 s busy timeout, so a second Atlas process reading concurrently is safe. A
scan takes a write transaction; two simultaneous scans of the same repository
serialise, and the loser waits or reports a database error rather than
interleaving.

## Build layout

```
CMakeLists.txt          canonical build
Makefile                thin wrapper: make, make test, make asan, make ubsan
cmake/AtlasWarnings.cmake   warning policy, warnings are errors
include/atlas/          public headers
src/main.c              entry point, nothing else
src/cli/                argument parsing, renderers
src/core/               service, scanner, buffers, errors, paths, hashing, procs
src/db/                 schema, migrations, typed operations
src/git/                read-only adapter and parsers
src/output/             streaming JSON writer
tests/                  first-party harness, fixtures, CTest suites
```

`atlas_core` is a static library; the `atlas` executable is `src/main.c` linked
against it, and every test links the same library. Tests can reach internal
headers under `src/`, which is how migration rollback and the degraded search path
are exercised.

## The diff command

`atlas diff` reports the complete working-tree change state rather than one
comparison. `git status --porcelain=v2` is the single source of truth for *what*
changed: one invocation yields the staged state against HEAD, the unstaged state
against the index, renames with their origin path, unmerged paths and untracked
paths, and it works with an unborn HEAD. Line counts come from
`git diff --numstat` and `git diff --cached --numstat` and are enrichment only: a
count that is unavailable is reported as unknown rather than as zero.

Four scopes are reported separately, because they are different facts:

| Scope | Comparison | Evidence |
| --- | --- | --- |
| `staged` | index against HEAD | `GIT` |
| `unstaged` | working tree against the index | `GIT` |
| `unmerged` | conflicted paths | `GIT` |
| `untracked` | on disk, not in the index | `SOURCE` |

A path staged and then modified again appears twice, once per scope. Collapsing
those into one entry would lose a fact a change-session recorder needs.

For untracked paths Atlas records **identity, not payload**: the safe path
representation, the size, and a SHA-256 content hash, plus a binary flag derived
from a NUL-byte sniff of the first 8000 bytes. File contents are never emitted. A
wholly untracked directory arrives from Git already collapsed to a single entry, so
dropping a large build tree into a repository does not produce a huge report.

The entry list is collected before rendering, because the header must be written
before the first entry and the summary after the last. That collection is bounded
by `--limit` (default 2000 entries): past the ceiling the report sets `truncated`
with a reason, while the per-scope counts still reflect reality, so the summary is
never quietly wrong.

## A1: the daemon

A1 adds threads, a socket and a background process to a codebase that had none.
The layering rules do not change; the new code sits under them.

```
src/cli      argument parsing + renderers
src/core     service.c, service_daemon.c, reconcile.c, lock.c, unit.c,
             scan.c, buffers, errors, paths, sha256, proc
src/db       schema, migrations, typed operations, db_state.c
src/git      read-only git adapter + parsers
src/ipc      frame codec, socket policy, request parsing, serve loop
src/daemon   writer thread, worker pool, inotify watcher, run loop
src/output   streaming JSON writer
```

A renderer still queries nothing. The service layer still formats nothing. The
daemon's IPC dispatch builds its responses with the same streaming JSON writer
the CLI uses, so there is one escaping implementation rather than two.

### Threads

Four kinds, with a strict rule about what each may touch. The full table is in
[daemon-and-ipc.md](daemon-and-ipc.md); the short version:

- **main** — the serve loop. Read-only database handles, one per request.
- **writer** — the only writable handle. Created on that thread, never shared.
- **watcher** — inotify. A read-only handle to enumerate repositories, and git to
  read ignore rules. Never writes; it hands state changes to the writer as jobs.
- **workers** — hashing only. No database handle, no process creation.

A SQLite connection is never shared between threads. That is enforced by where
the handles are created, not by a comment.

### Why the reconciliation pass has the shape it does

Four stages — observe, select, hash, apply — with the transaction opened only in
the last one, in bounded batches.

The alternative, a single transaction around the whole pass, is simpler to write
and wrong in two ways. It holds the write lock for the length of the pass, which
on a large repository is long enough that every reader sees stale data. And it
puts git invocations and file reads inside a transaction, so a slow git or a slow
disk extends the lock hold indefinitely.

Between hashing and applying, HEAD is read again. If it moved, the results
describe a repository that no longer exists and the pass is **abandoned** rather
than committed. This is not an optimisation: committing them would leave the
index describing a mixture of two branches, which is worse than being briefly out
of date.

### Why exactly one writer

SQLite's own locking would serialise the writes, but it would not stop two
processes from both believing they own the index. A daemon reconciling in the
background and an `atlas scan` started by hand would interleave generations and
produce a consistent database describing an inconsistent story. The advisory lock
makes "who owns this index" a single answer.

### The one thing the daemon will not do

There is no remotely callable shutdown. Anything local that can open the socket
could otherwise disable indexing. systemd owns the lifecycle; `SIGTERM` works.

### A1 resource bounds, in one place

Every ceiling A1 introduces is a macro in `include/atlas/limits.h`, not a literal
at a call site. The full table with what happens when each is reached is in
[daemon-and-ipc.md](daemon-and-ipc.md).

The rule that matters more than any individual number: **nothing is silently
truncated.** Reaching a bound produces a structured error, or a `truncated` flag
with a stated reason, or an explicit degraded state. A response that would exceed
the size ceiling becomes an error about not fitting, not half a document. A
discovery pass that hits its file ceiling says so rather than indexing a prefix.
A full write queue refuses the submission and reports backpressure rather than
dropping the work.

The reason is the same one behind `UNKNOWN`: an answer that quietly omits part of
itself is worse than an answer that admits it is incomplete, because only one of
the two can be acted on.

## A3: the structural index

A3 is a **stage of the reconciliation pass**, not a second pipeline, and that is
the whole architectural decision. There is one writer, one lock, one generation
and one place that decides what changed; adding a second indexer would have
needed a second answer to every one of those.

```
select  — one SQL query comparing files.content_hash against the hash the
          stored graph facts were extracted from. No I/O at all.
parse   — the existing worker pool. A job reads bytes through
          atlas_path_open_nofollow, lexes them, and fills its own slot. It
          touches no database handle and creates no process.
apply   — the writer, per file, in bounded transactions: delete every row the
          file owns, insert the new ones. No file read inside a transaction.
resolve — deterministic, after everything is applied, over a scope bounded by
          the change rather than by the repository.
```

The three A1 rules the stage inherits unchanged: workers touch no SQLite,
nothing forks a process off the writer thread, and no transaction is held across
unbounded work. The chunked select/parse/apply loop exists for the third one —
`ATLAS_CODE_PARSE_CHUNK` bounds how many parse results are held at once, so the
stage's memory is a property of the constant rather than of the repository.

**The resolver is the part with a design in it.** "Re-attempt every unresolved
edge" is a correct answer that costs the same on an untouched repository as on a
rebuild, so the scope is described explicitly by `atlas_code_resolve_scope`: the
files parsed, the externally linked definition names that changed, whether the
set of paths changed, and whether the scope is trustworthy at all. Each field
narrows the sweep to something that could genuinely answer differently. The
reasoning behind each is in [code-intelligence.md](code-intelligence.md); the
durable `code_index_state.resolve_settled` flag is what makes skipping safe
across a crash, because it is cleared before the work and set after it.

### The graph knows which analyzer built it

A3's fourth staleness signal, and the only one that is not about the inputs.
`code_index_state.analyzer_id` references an interned `code_analyzers` row
holding two Atlas-owned constants, so an upgrade that corrects the lexer or the
resolver makes every existing graph stale even though not one repository byte
changed and every generation still lines up. The next ordinary pass notices and
rebuilds; the rebuild deletes `code_files` and `code_units` and nothing else, so
sessions, recorded reasons, decisions, evidence, commits and the file index come
through untouched.

Normalized on purpose — one integer per repository rather than two columns on
six hundred thousand relation rows — and a *reference* rather than the values,
so a future producer that mixes sources can add the same reference to
`code_relations` without a redesign. See
[code-intelligence.md](code-intelligence.md).

### What A3 deliberately did not add

- **No second writer, no second service, no second lock.** `atlas code sync` is
  `atlas sync` with a rebuild flag.
- **No new dependency.** No Clang, no libclang, no tree-sitter, no ctags, no
  runtime of any kind. The lexer is first-party and bounded.
- **No execution of anything a repository controls.** `compile_commands.json` is
  read as data through an argument allowlist; the `command` string is hashed and
  discarded. `tests/test_code_compdb.c` plants an executable marker in four
  places inside a compile database and asserts it never ran.
- **No compiler semantics.** Atlas records what the bytes say and how sure it is,
  and never chooses between two definitions of a name.
- **No external analyzer.** No SCIP, no Clang, no LSP, no plugin loader. The
  analyzer identity exists so a *future* one could be told apart from this one,
  not because one is coming in A3.

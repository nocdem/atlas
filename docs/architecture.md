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

# Data model

The database is a **rebuildable index**. Every row in it is derived from Git or
from the working tree, and every row can be reconstructed by rescanning. Nothing
in Atlas treats it as the canonical record of history.

Current schema version: **5**. `atlas doctor` reports the version in force and the
version the binary expects.

## Migrations

Migrations are numbered, applied in ascending order, and each runs inside its own
transaction. If any statement in a migration fails, the whole migration is rolled
back and the recorded version does not advance; the error names the migration and
says it was rolled back. Applying migrations to an up-to-date database is a no-op,
and a gap in the numbering is refused rather than applied out of order.

A migration is a list of statement groups rather than one string, because ISO C
only guarantees 4095-byte string literals and one group per table is easier to
read.

Applied migrations are recorded in `schema_migrations(version, name, applied_at)`.

Applied so far:

1. **initial schema** — every table listed below.
2. **worktree identity** — adds `git_dir`, `git_dir_text` and `is_linked_worktree`
   to `repositories`, plus an index on `git_common_dir`. See "Repository and
   worktree identity".
3. **continuous indexing state** — filesystem identity on `files`, plus
   `repo_index_state`, `repo_events`, `repo_commit_tips` and `daemon_state`.
4. **AI sessions, change reasons and decisions** — the A2 tables, plus the
   per-path working-tree change snapshot. See "Migration 4" below.
5. **structural code graph** — the A3 tables: structurally indexed files with
   typed roles, translation units and their configurations, symbols, call
   candidates, relations with a resolution class, ambiguity candidates, the
   interned analyzer identities that produced them, and bounded indexing errors.
   See "Migration 5" below.

FTS5 objects are deliberately **not** part of a numbered migration: whether FTS5
exists is a property of the linked SQLite build, not of the schema. They are
created idempotently after migration when available, and their absence is reported
by `atlas doctor` and degrades search rather than failing it.

## Pragmas

Set at open time and verified by `atlas doctor`:

| Pragma | Value | Why |
| --- | --- | --- |
| `foreign_keys` | `ON` | cascade deletes are load-bearing for `repo remove` |
| `journal_mode` | `WAL` where supported | concurrent readers; the actual mode in force is reported |
| `busy_timeout` | 5000 ms | a concurrent scan waits rather than failing instantly |
| `synchronous` | `NORMAL` | the index is rebuildable, so full fsync is not worth the cost |

`integrity_check` and `foreign_key_check` are both run by `atlas doctor`, and both
are bounded to the first 20 reported rows so a corrupt file cannot flood the
output.

## Paths

Every path is stored twice, and both forms matter:

- `path_raw` (`BLOB`) — the exact bytes Git reported. **This is the key.** Lookups
  use these bytes, so a path that is not valid UTF-8 is still addressable.
- `path_text` (`TEXT`) — a lossless printable encoding used for display, search
  and JSON. Bytes that are not valid UTF-8, control bytes, `%` and DEL become
  `%XX` (uppercase hex). ASCII paths are byte-identical in both forms.
- `path_is_utf8` (`INTEGER`) — whether the raw bytes were valid UTF-8, so a
  consumer knows whether `path_text` is an encoding or the original.

The encoding is reversible and is accepted as input, so a path printed by Atlas
can be pasted back into a command.

## Tables

### `repositories`

One row per registered **worktree**. The unique keys are the user-facing `name` and
the canonical `root_path`, so the same worktree cannot be registered twice under
two names. See "Repository and worktree identity" below.

| Column | Notes |
| --- | --- |
| `id` | internal id, referenced by everything else |
| `name` | unique, user-facing; `[A-Za-z0-9._-]`, 1–128 bytes, not starting with `-` or `.` |
| `root_path`, `root_path_text` | canonical working-tree root as reported by `git rev-parse --show-toplevel` |
| `git_common_dir`, `git_common_dir_text` | absolute common Git directory, shared by every worktree of one repository |
| `git_dir`, `git_dir_text` | this worktree's own Git directory; what distinguishes two worktrees (migration 2) |
| `is_linked_worktree` | 1 for a linked worktree, 0 for the main one (migration 2) |
| `object_format` | `sha1`, `sha256`, or `unknown` when this Git cannot report it |
| `registered_at` | ISO-8601 UTC |
| `last_scan_at`, `last_scan_id` | `NULL`/0 until the first scan |
| `scanned_head` | the head the index describes; compared against live head to detect drift |
| `current_branch` | `NULL` when detached |
| `head_state` | `born`, `unborn`, `detached`, or `unknown` |
| `dirty`, `dirty_staged`, `dirty_unstaged`, `dirty_untracked`, `dirty_unmerged` | dirty-state summary observed by the last scan |
| `scanner_uid` | the uid whose scanner may report about this repository; `0` means none assigned (migration 27, A13) |
| `mirror_complete` | `1` only once a scanner-built mirror has finished publishing; cleared at the start of every run (migration 28, A13) |
| `mirror_at` | when the mirror named by `mirror_complete` was last published (migration 28, A13) |
| `trailer_scan_high` | the highest `commits.id` a memory-reconciliation pass has examined for a commit trailer, advanced on every scan regardless of what it found (migration 30, A12.1) |

`last_scan_id` is a soft reference without a foreign key, because `scans` is
created after `repositories` and the two would otherwise be circular. It is
cleared by the cascade when scans are deleted.

### `scans`

One row per scan attempt, `status` in `running`, `ok`, `failed`. A scan row
records the head, branch, object format and dirty summary observed at the time,
plus the counters the scan reported. Because the whole scan runs in one
transaction, a failed scan leaves no row at all; a `failed` row can only appear if
the failure was recorded deliberately.

### `files`

One row per tracked path per repository, unique on `(repo_id, path_raw)`. A path
that leaves the index is **marked** deleted, never removed, because its history
still matters.

| Column | Notes |
| --- | --- |
| `file_type` | `regular`, `symlink`, `other`, `missing` |
| `language` | detected from extension or basename; `NULL` when undetected |
| `git_mode` | e.g. `100644`, `100755`, `120000`, `160000` |
| `git_index_oid` | object id from the index |
| `content_hash`, `content_hash_algo` | Atlas' own SHA-256 of working-tree content; for a symlink, of the link text. `NULL` when not hashed. |
| `size_bytes` | bytes hashed, or the link text length |
| `is_executable` | from the Git mode, or the working-tree mode for a regular file |
| `is_symlink` | the working tree entry is a symlink |
| `unsafe_path` | a path component was a symlink, so the file was refused |
| `read_error` | why there is no hash: missing, refused, too large, or a submodule |
| `first_seen_scan_id`, `last_seen_scan_id`, `first_seen_at`, `last_seen_at` | lifetime |
| `deleted`, `deleted_at`, `deleted_scan_id` | tombstone |

A file counts as **changed** only when something Atlas records about it changed:
content hash, Git object id, mode, type, language, executable or symlink state,
size, unsafe-path flag, or read error. Scan ids and timestamps are excluded on
purpose, which is what makes repeated scans of an unchanged tree report no churn.
A path that had been marked deleted and is tracked again counts as an addition and
the tombstone is cleared.

### `commits`

One row per commit per repository, unique on `(repo_id, oid)`. Stores parents as a
space-separated list plus a parent count, author name and email, author and
committer Unix timestamps, the subject (first line of the raw message) and the
full raw message body.

Commits are inserted with `ON CONFLICT DO NOTHING`, so rescanning is cheap and
idempotent: a commit Atlas already has is not rewritten, and its file changes are
not re-inserted.

### `file_changes`

One row per changed path per commit. `change_type` is constrained to `add`,
`modify`, `delete`, `rename`, `copy`, `typechange`, `unmerged`, `unknown`. Rename
and copy rows carry `old_path_raw`/`old_path_text` and a similarity `score`, and
`raw_status` keeps Git's own code (e.g. `R100`) so nothing is lost in translation.

Because both the new and old paths are indexed, history for a renamed path is
visible from either side.

Merge commits are recorded, but A0 does not walk per-parent diffs, so a merge
contributes no `file_changes` rows.

### `compile_databases`

Recorded in A0, parsed in A3 — the A0 note said A2 and the phase slipped. One row
per `compile_commands.json` found, unique on
`(repo_id, path_raw)`, holding whether it is a regular file, whether it is a
symlink, its content hash (of the link text when it is a symlink), its size, and a
`parsed` flag that is always 0 in A0. Atlas looks for it both among tracked files
and directly at the repository root, since it is usually generated and ignored.

### `evidence`

Links an indexed fact back to the exact Git object, path or scan it came from. The
`kind` CHECK constraint lists all six evidence types so the schema is stable
across phases, but A0 code can only write `SOURCE` and `GIT` — enforced in
`atlas_db_evidence_insert`, not merely by convention.

Evidence is written when a fact is **new or changed**, never for an unchanged
file. That is deliberate: it keeps the table proportional to what was learned
rather than to how often you scanned, and it makes "a repeated scan creates no
evidence" a checkable property.

## Indexes

`idx_scans_repo`, `idx_files_repo_live`, `idx_files_repo_text`,
`idx_commits_repo_time`, `idx_changes_commit`, `idx_changes_repo_path`,
`idx_changes_repo_oldpath`, `idx_evidence_repo_kind`, `idx_evidence_repo_path`,
plus the uniqueness indexes implied by the `UNIQUE` constraints above.

## Full-text search

When FTS5 is available, two external-content virtual tables shadow the base
tables:

```sql
CREATE VIRTUAL TABLE files_fts   USING fts5(path_text, content='files',   content_rowid='id', ...);
CREATE VIRTUAL TABLE commits_fts USING fts5(subject, body, content='commits', content_rowid='id', ...);
```

External content means the text is not duplicated. They are rebuilt after each
scan, which is simple and correct; incremental maintenance can come later if it
ever matters.

Without FTS5, search falls back to a bounded `LIKE` scan and reports mode
`degraded-like` so the caller knows results are unranked. See
[provenance.md](provenance.md) for how that surfaces in output.

## Repository and worktree identity

A row in `repositories` identifies **one worktree**, not one repository. Several
Git worktrees can share a single object store while having entirely independent
working state, and the index has to keep both facts straight.

Three columns carry the identity:

| Column | What it is | Same across worktrees? |
| --- | --- | --- |
| `root_path` | canonical working-tree root (`git rev-parse --show-toplevel`) | **no**, unique per worktree |
| `git_common_dir` | the shared object store (`--git-common-dir`) | **yes** |
| `git_dir` | this worktree's own Git directory (`--git-dir`) | **no**, unique per worktree |

For the main worktree, `git_dir` equals `git_common_dir`. For a linked worktree
created with `git worktree add`, `git_dir` is `<common>/worktrees/<name>` and
`is_linked_worktree` is 1. So:

- two rows with **different** `git_common_dir` are unrelated repositories
- two rows with the **same** `git_common_dir` and **different** `git_dir` are
  worktrees of one repository
- the same `git_dir` twice cannot happen, because `root_path` is unique and a
  worktree has exactly one root

Everything else is per-row and therefore per-worktree: `scanned_head`,
`current_branch`, `head_state`, the dirty summary, and every `files`, `commits`,
`file_changes` and `evidence` row, all of which are keyed by `repo_id`. Two
worktrees of one repository are scanned, queried and removed independently, and
`ON DELETE CASCADE` removes only the rows belonging to the registration being
removed.

`atlas status` reports `sibling_worktrees`: the number of other registrations
sharing this one's `git_common_dir`. That makes it visible that other worktrees
exist without implying their state is the same.

Commits are stored per registration, so two worktrees of one repository each hold
their own `commits` rows even though Git stores the objects once. That is a
deliberate trade: it keeps every row attributable to the registration that
observed it, keeps `repo remove` a pure cascade, and keeps a stale registration
from silently affecting a live one. It costs disk when many worktrees of a large
repository are registered, and a later phase may share commit rows behind a join
table once there is a reason to.

### Why this needed a migration

Schema version 1 recorded only `git_common_dir`. Two worktrees of one repository
were therefore distinguishable by root path but nothing recorded *which* worktree
a row described, nor that one of them was linked rather than main. Migration 2
adds `git_dir`, `git_dir_text` and `is_linked_worktree`, plus an index on
`git_common_dir` so siblings can be found. `atlas scan` verifies both the root and
the git dir against the registration and refuses with exit 7 if either has moved,
so a pruned or relocated worktree is reported rather than silently indexed as
something else.

## Migration 3 — continuous indexing state (A1)

A0 answered one question: *what did the last scan see?* A1 has to answer a harder
one: *is what Atlas holds right now current, and if not, in what way is it not
current?* That needs state A0 never recorded.

Nothing in migration 3 is destructive. Every statement is an `ALTER TABLE ... ADD
COLUMN` with a default or a `CREATE` for a new object, so a schema-v2 database
migrates forward with its rows intact and no table is recreated. There is a test
that seeds a v2 database through the shipped v1 and v2 statements, populates it,
migrates it, and asserts every row survives.

### New columns on `files`

| column | type | why |
| --- | --- | --- |
| `fs_dev`, `fs_ino`, `fs_size`, `fs_mtime_sec`, `fs_mtime_nsec`, `fs_ctime_sec`, `fs_ctime_nsec`, `fs_mode` | INTEGER, nullable | the filesystem identity last observed. A pass that finds all eight unchanged does **not** read the file. This is what makes reconciliation incremental. **ctime is not optional**: mtime is writable, so without ctime a same-length in-place edit with the mtime restored by `utimensat` compares as unchanged forever. Nothing in userspace can set ctime. |
| `tracked` | INTEGER NOT NULL DEFAULT 1 | 0 for a file discovered inside an untracked directory. The default backfills correctly: A0 only ever recorded tracked files. |
| `ignored` | INTEGER NOT NULL DEFAULT 0 | git's own ignore rules cover it |
| `truncated`, `truncated_reason` | INTEGER / TEXT | the content was not fully hashed, and why. Never left implicit. |
| `last_generation` | INTEGER NOT NULL DEFAULT 0 | the pass that last saw it |

The identity columns are nullable and are read as a **unit**: any NULL among them
means the whole identity is unknown, and an unknown identity is always rehashed —
exactly once, after which the row has one. A partially recorded identity would
compare unequal forever and rehash the file on every pass.

### `repo_index_state` — one row per registered worktree

| column | why |
| --- | --- |
| `generation` | the pass currently in flight |
| `last_complete_generation` | the newest pass that finished consistently — **the only generation a reader is ever shown** |
| `last_reconcile_at`, `last_complete_at` | when |
| `watch_state` | `unwatched` / `watching` / `degraded` / `incomplete` / `error`, CHECKed |
| `watch_detail`, `watched_dirs` | what the watcher is doing and how much of it |
| `event_gap` | Atlas cannot prove it observed every change. While set, nothing may describe the index as current. |
| `pending_full_reconcile` | a full pass is owed; persisted, so the obligation survives a restart |
| `last_error` | the last failure, for `daemon status` |
| `last_sync_seq` | what `atlas sync --wait` polls for |

`last_complete_generation` is advanced with `max()`, never assignment, so a slow
pass finishing after a newer one cannot move the published state backwards.

### `repo_events` — the durable, monotonic cursor

`id INTEGER PRIMARY KEY AUTOINCREMENT`, so the cursor is database-wide and
strictly increasing and "everything after N" is one indexed range scan that does
not depend on wall-clock time. AUTOINCREMENT specifically, so a deleted row's id
is never reused — a pruned journal must not renumber into a consumer's cursor.

`dedup_key` with a **partial unique index** on `(repo_id, generation, dedup_key)
WHERE dedup_key IS NOT NULL` makes ingestion idempotent: the same observation
replayed after a restart collides instead of appending a duplicate. Events with
no key (a reconciliation summary) are always appended.

Rows here have **bounded retention** (`ATLAS_EVENTS_RETAIN_PER_REPO`, 20000).
Durable `SOURCE` and `GIT` evidence in `evidence` is **never** pruned with them:
raw events are a convenience for consumers, evidence is the provenance record.

### `repo_commit_tips`

`(repo_id, ref_name) → tip_oid`. What each ref was at when its history was last
ingested, so the next pass runs `git log HEAD --not <tip>` rather than replaying
everything. A detached HEAD gets its own key (`HEAD@detached`) so checking out a
commit does not corrupt the branch's recorded position.

### `daemon_state`

A single row, held to one by `CHECK(id = 1)` so a second daemon cannot append a
second identity and make `daemon status` ambiguous. It is **diagnostic only**:
liveness is proven by the advisory lock, not by this row. A killed daemon leaves
the row behind, and the released lock is what disproves it.

### What migration 3 creates

Tables: `repo_index_state`, `repo_events`, `repo_commit_tips`, `daemon_state`.
Indexes: `idx_files_repo_generation`, `idx_repo_events_repo`,
`idx_repo_events_dedup` (partial, unique).
Columns: thirteen on `files`, listed above.
Constraints: `CHECK` on `repo_index_state.watch_state`, `CHECK` on
`repo_events.kind`, `CHECK(id = 1)` on `daemon_state`, and `REFERENCES
repositories(id) ON DELETE CASCADE` on all four new tables so `repo remove`
remains a pure cascade.

## Migration 4 — AI sessions, change reasons and decisions (A2)

A1 answered "is the index current?". A2 has to answer two more: *which AI session
was in a position to change this, and did anybody say why?*

Nothing in migration 4 is destructive. Every statement is a `CREATE` for a new
object, so a schema-v3 database migrates forward with its rows intact and no
existing table is recreated. `tests/test_migrate3.c` seeds a v2 database and
migrates it all the way forward; `tests/test_ai_schema.c` asserts the v4 objects
exist and that migrating twice changes nothing.

**The A0 rule is untouched.** `evidence` still `CHECK`s its six kinds, and
`atlas_db_evidence_insert` still refuses everything but `SOURCE` and `GIT`. AI
records are a different kind of thing and live in different tables. Widening
`evidence` to fit them would have made "how does Atlas know this?" and "what did
a model claim?" the same question, which is the confusion A2 exists to avoid.

### The client is not Claude

`ai_clients(provider, name)` — a second adapter is another row, not another
table. Nothing in the schema names Claude, and nothing in `src/ai` does either.
The adapter that knows the name is `src/hook/hook.c`, in two `#define`s.

### `ai_sessions`

One row per client session, keyed `(client_id, session_key)` where `session_key`
is the client's own identifier, safe-encoded and bounded on the way in.

`parent_id` carries resume and fork lineage. A **subagent is a session with a
parent and an `agent_type`**, not a flag on its parent: that makes its change
set, its reasons and its tool records separable without a second set of tables.

Opening a session key Atlas already has is a **resume**, not a replacement. The
row keeps its change set and its records, and `resumes` is incremented. Replacing
it would orphan all three and make a resumed session look like a new one that had
done nothing.

`state` is `open`, `closed` or `expired`. The distinction between the last two is
kept deliberately: a session that stopped answering did not end on purpose, and
"the client crashed" is a different fact from "the user quit". Idle sessions are
expired opportunistically at the next session open, bounded by
`ATLAS_AI_SESSION_IDLE_EXPIRY_MS`.

### `ai_session_repos` and `ai_change_sets`

A session that changes directory or gains a working directory **gains** a
repository rather than replacing one: work it did before the change still belongs
to it, and the earlier change set stays open.

One change set per session per repository records the window over which that
session was in a position to change that repository, with the HEAD and generation
it started from.

### `ai_changed_paths` — the honest table

| `attribution` | means |
| --- | --- |
| `direct_edit` | this session invoked an edit tool naming this path **and** the index then observed the path change |
| `observed` | only the second half |
| `ambiguous` | another session had the same repository open over the same window |

`concurrent_sessions` records how many, so the ambiguity is a number rather than
an adjective.

**Attribution never improves.** A row already marked ambiguous stays ambiguous
whatever a later observation claims, because the overlap is a fact about a window
that has already passed and a later clean observation does not retract it. A row
may be promoted from `observed` to `direct_edit`, which is the one direction that
adds information rather than discarding it. Enforced in the `ON CONFLICT` clause
in `src/db/db_ai.c`, not in a caller.

### `ai_reasons` and `ai_decisions`

`provenance` is one of `MODEL_PROPOSAL`, `MODEL_INFERENCE`,
`USER_APPROVED_DECISION` or `UNKNOWN`. The third is in the `CHECK` so the schema
is stable across phases; A2 cannot write it.

`approved INTEGER NOT NULL DEFAULT 0 CHECK(approved = 0)`. The column exists and
is pinned to zero, because A2 has no way to prove a human approved anything — an
argument asserting approval is a string a model produced. Lifting the restriction
is a migration, which is a change somebody has to make on purpose. See
[ai-trust-boundary.md](ai-trust-boundary.md) for the other two layers.

`ai_reasons.state` distinguishes a recorded reason from a recorded *absence* of
one. `unknown` is a first-class row: "nobody said why" and "Atlas was never
asked" are different facts and a query has to tell them apart.

`dedup_key` with a partial unique index makes a replayed write collide instead of
duplicating, and the existing row's id is returned — so a caller that retried gets
the identifier it would have got the first time.

### `ai_session_events` — bounded, and deliberately thin

What is absent is the point of the table. A row records that a named tool ran,
whether it reported success, and at most one normalized path. There is no prompt,
no tool input, no tool result, no error text, no command line.

Rows are pruned to `ATLAS_AI_EVENTS_RETAIN_PER_SESSION`. The prune statement
addresses `ai_session_events` alone; durable reasons and decisions are in other
tables and nothing in it can reach them.

### `ai_evidence_links`

Links a model record to the newest `SOURCE` or `GIT` evidence Atlas holds for the
same path, so a claim and the facts about the same path stay connected without
either becoming the other. A path with no evidence links nothing, silently: that
is the normal state of a file Atlas has not indexed yet.

### `ai_checkpoints`

Bounded counters only. The Atlas-owned session state is already in the tables
above, so a checkpoint records that compaction happened and what the state was —
never a summary of the conversation.

### `repo_worktree_changes` — per-path change scope

A1 recorded the dirty *counts* per repository, which answers "does this repository
have staged changes" and not "which paths are staged". An MCP adapter has to
answer the second from the index, because running git inside the daemon's serve
loop would let one such question stall every other client for the git timeout.

The reconciliation pass already runs one `git status --porcelain=v2` for the
counts — `atlas_git_read_worktree_state` is literally `atlas_git_read_status` with
a NULL callback — so it now records the entries it was already parsing. No extra
git invocation.

The table is a **snapshot replaced wholesale by each pass**, not a journal: a path
that is no longer dirty is not a historical fact worth keeping, and history
already lives in `file_changes`. `generation` is what makes a stale snapshot
visible.

### What migration 4 creates

Tables: `ai_clients`, `ai_sessions`, `ai_session_repos`, `ai_session_events`,
`ai_change_sets`, `ai_changed_paths`, `ai_reasons`, `ai_reason_paths`,
`ai_decisions`, `ai_decision_paths`, `ai_evidence_links`, `ai_checkpoints`,
`repo_worktree_changes`.

Every one of them references `repositories(id)` or `ai_sessions(id)` with
`ON DELETE CASCADE`, so `repo remove` remains a pure cascade.

## Migration 5 — the structural code graph (A3)

A2 answered "who changed this and did anybody say why". A3 answers "what is this,
what is it connected to, and what might a change to it reach" — and it has to
answer without ever claiming to be a compiler.

Nothing in migration 5 is destructive. Every statement is a `CREATE` for a new
object, so a schema-v4 database migrates forward with its rows intact and no
existing table is recreated. `tests/test_migrate3.c` seeds a v2 database and
drives it all the way here; `tests/test_ai_schema.c` asserts migrating twice
changes nothing.

**The A0 rule is untouched for the third time.** `evidence` still `CHECK`s its
six kinds, `atlas_db_evidence_insert` still refuses everything but `SOURCE` and
`GIT`, and A3 writes no evidence at all. `tests/test_code_trust.c` runs a
structural pass and asserts the evidence table gained nothing but `SOURCE` and
`GIT`. Structural facts are a different kind of thing: they carry their own
resolution class and their own provenance column, because "how does Atlas know
this?" and "what did a lexical scan guess?" must stay different questions.

### Resolution is a column, not a convention

Every symbol, occurrence, relation and role carries a `resolution` constrained to
`SOURCE_EXACT`, `BUILD_METADATA`, `UNIQUE_LEXICAL`, `AMBIGUOUS`, `UNRESOLVED`,
`CONDITIONAL`, `MODEL_PROPOSAL` or `UNKNOWN`. The vocabulary is complete across
phases so the schema is stable; `MODEL_PROPOSAL` is in it and A3 may not write
it, refused by `atlas_code_resolution_writable_in_a3` and by the one place on the
write path that checks — the same shape as A2's approval restriction.

Full detail, including the explicit non-claims, is in
[code-intelligence.md](code-intelligence.md).

### `code_analyzers` — who produced the graph

Two columns and both are Atlas-owned: `name` is a string literal in the binary
(`atlas-c-lexical`) and `version` is an integer the binary decides. Nothing here
is derived from a repository, a compile database or a model, which is what makes
the pair safe to report to a model.

It exists because provenance and resolution answer two questions and there is a
third. Provenance says which source a fact was read from; resolution says how
firmly it was established; neither says which *algorithm* produced it. Upgrade
Atlas with a corrected lexer, change not one byte of the repository, and every
generation still lines up while the graph is now wrong in exactly the way the
upgrade fixed — and reports itself current.

`code_index_state.analyzer_id` references one row here, so the fact is stored
once per repository rather than once per relation. It is a reference rather than
the values themselves so the per-fact case stays reachable: a future producer
that mixes sources adds `analyzer_id INTEGER REFERENCES code_analyzers(id)` to
`code_relations` — one integer per row against a vocabulary already interned.

Rows are never updated. An analyzer identity is a historical fact about what
built something, not a setting, so an upgrade adds a row and leaves the record
of what produced the previous graph intact.

### `code_index_state` — one row per repository

The generation pair mirrors `repo_index_state` exactly, and holds the
*reconciliation* pass's generation, so "does the graph describe the file index?"
is an integer comparison rather than an inference from timestamps.
`last_complete_generation` advances with `max()` for the same reason it does
there. `degraded` is the honesty bit: while it is set, `code_index_current` is
false.

The counters (`symbols`, `relations`, `ambiguous`, `unresolved`) are recomputed
from the tables at the end of each pass rather than incremented as rows are
written. An incremented counter drifts the first time a path fails part way
through and nothing notices; five `COUNT(*)` queries cannot.

They are recomputed on every pass that could have written a row and skipped on
one that provably could not. Two of the five are full scans of the relation
table by design — there is no index on `resolution` — so at half a million edges
they were most of what an otherwise empty pass cost, and running them to confirm
a number that cannot have moved is the same mistake as reparsing an unchanged
file.

`analyzer_id` is what makes an upgrade visible. A mismatch against the binary's
own constants makes `code_index_current` false with the fixed reason *"the
structural index was produced by a different analyzer version"*, and the next
ordinary pass rebuilds — deleting only `code_files` and `code_units` and the
rows that cascade from them, so sessions, reasons, decisions, evidence, commits
and the file index come through untouched.

`resolve_settled` is what makes skipping the *rest* of such a pass safe. It
records that every edge has been through resolution since the last thing that
could change an answer, and a pass with no file parsed, none removed and no
compile-database change skips resolution entirely when it is set. It is cleared
by `atlas_db_code_state_begin`, before any work, and set by
`atlas_db_code_state_complete` — so a pass that died half way through resolution
leaves it 0 and the next pass sweeps the repository. Inferring it from "did the
last pass complete?" would not survive that, which is the one case it exists for.

### `code_files` — and why `content_hash` is the load-bearing column

One row per structurally indexed file, keyed on `(repo_id, path_raw)` like every
other path in Atlas. `content_hash` is the hash of the bytes the facts were
extracted from, and selection for the next pass is a comparison against
`files.content_hash`.

That comparison is the whole incremental story. It is deliberately *not* "was
this file hashed by this pass": a full content-verifying pass rehashes every byte
and finds the same hash, so an unchanged repository still parses nothing — and
the five-minute periodic full pass does not reparse the world every five minutes.

`basename_raw` is denormalised from `path_raw` and indexed. Resolving `#include
"atlas/buf.h"` against `include/atlas/buf.h` is a suffix match, and a suffix
match has no index; without a basename to seek on, every unresolvable include
cost a full scan of the table.

Keyed on the path rather than on `files(id)` because `files` rows are
**tombstoned rather than deleted**. A foreign key from `files` would fire only on
`repo remove`, which is the one case it is not needed for, so removing a file's
graph rows is explicit writer-path work — see below.

### `code_file_roles` — the basis travels with the role

A file may hold several roles, and each row records the `basis` it was arrived
at on: `extension`, `path_naming`, `content_marker`, `build_metadata`,
`include_graph` or `none`. A file under `tests/` is *named* like a test; that is
a fact about the path and not proof about the file, and the basis is what keeps
the difference legible rather than flattened into an assertion.

### `code_symbols` — a symbol is a site

Two files each defining `static void helper(void)` produce two rows and nothing
merges them. Merging them would be a decision a lexical indexer has no basis for,
so cross-file identity is expressed by edges with a resolution class instead.

`linkage` (`external`, `internal`, `none`, `unknown`) is load-bearing rather than
descriptive: an internal-linkage definition is a candidate only for occurrences
in its own file, and that rule lives in the SQL so no caller can forget it. It is
also in the repository-wide index, because a lookup of a name every file defines
must not fetch and discard one row per file.

### `code_relations` — one table, two indexes, a class on every edge

Every edge, with a typed endpoint on each side. One table rather than one per
kind, and the reason is the queries rather than tidiness: inbound and outbound
traversal become the same shape over `idx_code_rel_src` and `idx_code_rel_dst`,
which is what keeps reverse-dependency and impact bounded and fast at hundreds of
thousands of edges.

`dst_name` carries the *spelling* — the include text, the callee identifier — and
is kept whether or not the edge resolved, because "this file includes something
called `config.h` that I cannot place" is a fact and dropping it would be a
silence. `spelling_form` distinguishes `"x.h"` from `<x.h>`, which changes what
the spelling means: a compiler searches the including file's own directory for
the first and not for the second.

`owner_file_id` is what makes replacement per file rather than per repository:
reindexing one file is one delete by that column followed by the inserts.

There is deliberately **no index on `resolution` alone**. It looks useful and is
not: the sweeps reach their rows through `(repo_id, kind, id)` and filter
resolution from the row, and the two per-pass counters are happy to scan. What
such an index would cost is a B-tree insertion on every one of a few hundred
thousand relation inserts, for a column whose value changes for most rows during
the same pass.

`idx_code_rel_kind_id(repo_id, kind, id)` is the paging key, and it exists
because of a query plan rather than a query. A resolution sweep reads
`WHERE repo_id=? AND kind=? AND id > ? ORDER BY id LIMIT n`; without this index
SQLite reached those rows through `idx_code_rel_name`, which orders by
`dst_name` and cannot satisfy the ORDER BY, so it built a temp B-tree — sorting
*every* edge of that kind and discarding all but `n`. Paging repeated that once
per page, which is quadratic in the repository. `idx_code_rel_name` needs no
such companion: `id` is the rowid, SQLite appends the rowid to every index, so
the by-name form is already ordered by `(repo_id, kind, dst_name, id)`.

`idx_code_files_basename` is named explicitly by the include-resolution
statement with `INDEXED BY`, and that is worth reading as documentation rather
than as tuning. `code_files` has two indexes beginning with `repo_id` — this one
and the implicit index behind `UNIQUE(repo_id, path_raw)` — and left to choose
between them for a basename lookup SQLite took the unique one, seeking on
`repo_id` alone and scanning every file in the repository. Measured, that was
5 444 rows visited per unresolvable include across 6 836 of them: thirty-seven
million row visits, and the largest single cost of a structural pass.

`INDEXED BY` is the opposite of a planner hint. It is a hard constraint: drop or
rename the index and the statement fails to prepare with a clear error, rather
than quietly becoming a scan again. `tests/test_code_graph.c` asserts both that
the index exists and that a query of that shape reaches its rows through it.

Two more indexes exist for foreign keys rather than for queries:
`idx_code_symbols_enclosing` and `idx_code_occ_enclosing`. `enclosing_id`
cascades from `code_symbols`, and SQLite enforces a cascade by looking for
children of the row being deleted; without an index on the child column that
lookup is a full scan, once per deleted symbol. Reparsing one file deletes a
dozen symbols, so it scanned every occurrence in the repository a dozen times.
An unindexed foreign key is invisible until the table is large, and then it is
most of what a one-file update costs.

### One relation kind is recognised and deliberately not written

`symbol_contains_occurrence` is in the vocabulary and in the `CHECK`, and the
built-in analyzer never writes one. The containment fact is stored — as
`code_occurrences.enclosing_id`, the column the extractor writes it to, with a
foreign key and an index. Storing it a second time as an edge cost 235 520 rows
on the acceptance fixture, 38 % of the whole relation table, five index
insertions each, and no query in Atlas ever read one.

The kind stays because a producer with no occurrence table of its own would need
it, and keeping it makes that an insert rather than a migration.

### `code_candidates` — the alternatives, kept

Atlas does not choose between same-named symbols, so an `AMBIGUOUS` edge stores
its candidates. `candidate_count` on the relation reports the true number even
when more existed than the ceiling keeps, so a bound never makes an ambiguity
look smaller than it is.

### `code_units`, `code_unit_includes`, `code_unit_defines`

One row per translation unit from a validated compile-database record, keyed
`(source, output)` so one file compiled twice with different flags stays two
configurations.

`command_hash` and `command_present` are the whole of what is kept from the
`command` string. The string itself is deliberately not stored: it is a shell
command line, Atlas has no use for it beyond noticing that it changed, and a
value nothing holds is a value nothing can accidentally run.

An include directory outside the repository is stored with `external = 1`. It
explains why an include resolved to nothing, and it is **never opened**:
recording where a build looks is not the same as being allowed to look there.

### `code_index_errors`

Bounded, and the reason every ceiling lands here: a limit reached silently is a
limit that makes the index look complete when it is not. Rows are pruned to
`ATLAS_CODE_ERRORS_RETAIN_PER_REPO`; the `degraded` flag is not pruned with them,
because the flag is the durable statement and these are the detail behind it.

### Invalidation is targeted, and that is also the point

An edge that resolved to a symbol of a file about to be reparsed points at an id
that is about to stop existing. `atlas_db_code_relations_unsettle_for_file` puts
those edges back to UNRESOLVED *before* the delete, reaching them through
`idx_code_rel_dst` from the ids it is about to remove. Afterwards the ids are
gone and only a left join over every relation could find the damage — which is
what `atlas_db_code_relations_dangling` does, and why it is now reserved for the
rebuild path, where a scan is proportionate.

### Deletion is explicit, and that is the point

`files` rows are tombstoned rather than removed, so `ON DELETE CASCADE` from
`files` never fires on an ordinary deletion or rename. Every structural pass asks
which `code_files` rows have no live `files` row and deletes them, cascading the
symbols, occurrences, relations, candidates and roles they own — then re-resolves
the edges that pointed at them, found by a left join rather than guessed at.

A rename is a tombstone plus an addition in `files`, and therefore a removal plus
a parse here. Nothing has to recognise a rename as such, which is what makes the
stale-row case impossible rather than handled.

### What migration 5 creates

Tables: `code_analyzers`, `code_index_state`, `code_files`, `code_file_roles`,
`code_symbols`, `code_occurrences`, `code_relations`, `code_candidates`,
`code_units`, `code_unit_includes`, `code_unit_defines`, `code_index_errors`.

`code_analyzers` is created first, because `code_index_state` references it.

Every one references `repositories(id)` or `code_files(id)` with
`ON DELETE CASCADE`, so `repo remove` remains a pure cascade.

## The prepared-statement cache

Not a schema change, but a property of every query above.

`atlas_db_prepare` returns a cached statement when it has one, keyed on the SQL
*pointer* — every call site passes a string literal with static storage duration,
so the pointer is a stable identity. `atlas_db_finish` returns it rather than
finalising it, and every site in `src/db` calls that instead of
`sqlite3_finalize`.

A0 and A1 issued one or two statements per file and never noticed the cost of
preparing them. A3 issues a few hundred — a symbol, an occurrence and several
relations per file, then a resolution per edge — and preparing them all afresh
was, measured, half the cost of indexing a large repository.

One cache per handle, and a handle belongs to one thread, so there is nothing to
synchronise: that is the rule the daemon already keeps. A cached statement that
is already stepping is marked in use and a re-entrant caller gets a fresh one,
because handing out the same statement twice would reset an iteration mid-flight.

## Two pragmas the graph made necessary

`PRAGMA cache_size=-65536`. SQLite's default page cache is about two megabytes:
ample for an index of paths and commits, nothing at all for a graph whose
indexes are hundreds of megabytes. Negative means kibibytes rather than pages,
so the ceiling does not move with the page size, and it is a cache — an upper
bound, not a commitment.

`PRAGMA temp_store=MEMORY`. Every `INSERT ... RETURNING id` makes SQLite
materialise the returned row in an ephemeral table, and with the default
`temp_store` an ephemeral table is a *file*: created, written and unlinked once
per inserted row. Sampling the structural pass on the acceptance fixture found
the writer inside `pwrite` on that temporary file in almost every sample — the
single largest cost of building the graph, and none of it was the graph. What
goes to memory instead is bounded by what Atlas asks for: one row per RETURNING,
and sorters for queries that are all limited.

The relation insert goes further and drops RETURNING altogether. It is the only
one of Atlas' inserts that is a plain insert with no `ON CONFLICT`, so
`last_insert_rowid()` is the id and nothing has to be read back — and it runs a
hundred times per file, more than every other insert in A3 put together. Its
neighbours keep RETURNING because an upsert may take the update branch, where
the last inserted rowid is not the row's.


## A4: decision documents (migration 6)

Seven tables. Nothing here cascades from `repositories`, and that is the one
structural break with everything above.

| table | holds |
| --- | --- |
| `decision_documents` | stable identity: the public `uid`, the soft `repo_id`, the durable `repo_root_hash`, the immutable A9.1 `kind`, and a **cache** of the ledger (`current_status`, `current_revision_id`) |
| `decision_revisions` | immutable revisions: content, `content_hash`, `proposed_by`, `state`, `imported_from_ai_decision_id` |
| `decision_alternatives` | the alternatives considered, in order |
| `decision_links` | paths, commits, change sets, symbol snapshots, and links to other documents |
| `decision_events` | the append-only lifecycle ledger — **canonical** |
| `decision_challenges` | short-lived single-use operator capabilities |
| `decision_search` | the bounded searchable projection, and the content table for the optional `decisions_fts` |

### `repo_id` is a soft reference, and a path hash is not an identity

Every other table in the schema is a rebuildable index keyed to a registered
worktree, so `repo remove` is a pure cascade. A decision document is not
rebuildable from anything, and rule 10 of the phase is that no decision record
is ever physically deleted — so a foreign key with `ON DELETE CASCADE` here
would make `atlas repo remove --yes` silently destroy approval history.

Two columns carry the repository, and only one of them is an identity.

`repo_root_hash` is the SHA-256 of the canonical root's raw bytes — the same
value the automatic context envelope reports. It answers "same directory", which
is a **location**. It is kept for reporting and is never sufficient to decide
that two registrations are the same repository.

`repo_identity_hash` is what an automatic relink matches on. It is a
**path-qualified lineage fingerprint**, committing to the canonical root path,
the object format, **and the sorted set of root commits Atlas has ingested**.
The root-commit set is the discriminating component: it is stable across clones,
fetches, rewrites of later history and re-registration at the same path, and it
differs between unrelated repositories. The root path is hashed alongside it, so
the fingerprint as a whole is *not* stable across a move — see the note below.
It is computed from the `commits` table, so it costs one indexed query and needs
no git invocation. It is empty when no lineage is known, and an empty identity
matches nothing.

What the fingerprint does and does not do:

| situation | reattached automatically? | why |
| --- | --- | --- |
| same path, same lineage — a `repo remove` / `repo add` cycle | yes | the whole fingerprint matches |
| same path, unrelated history — `rm -rf` then `git init` | **no** | the root commits differ |
| same lineage, different path — a clone or a move | **no** | the root path differs, so the fingerprint does |
| either side has no ingested history | **no** | an empty identity matches nothing |

The third row is a real limitation rather than an oversight. Moving a repository
leaves its decisions orphaned and visible in `atlas decision orphaned`; manual
relinking is deferred to a later phase. The alternative — matching on lineage
alone — would reattach across every clone on the machine, including ones the
operator never meant to associate, so the conservative direction was chosen.

The attach and the detach are deliberately split:

- `atlas_db_decision_detach_repo` runs unconditionally inside
  `atlas_db_repo_add`. Every document carrying that `repo_id` is detached to
  `repo_id = 0`, which no `repositories` row can have. It needs no git and no
  history, so it cannot be skipped — and it closes a real hole, because
  `repositories.id` is a rowid and **rowids are reused**.
- `atlas_db_decision_relink_after_ingest` runs from `atlas_db_scan_finish` on a
  successful pass, when the lineage is finally knowable, and attaches only on an
  exact non-empty identity match. It also backfills the identity onto documents
  already attached to this repository whose identity is empty — which is not a
  guess, since they demonstrably belong to it — and never overwrites an existing
  one.

Splitting them makes the failure mode fail-closed by construction: forgetting
the attach can only orphan a decision, which is visible through
`atlas decision orphaned` and recoverable by re-registering; it can never attach
one to the wrong repository, which is neither.

### `decision_documents.uid`

`atlas-dec-` and 32 lowercase hex characters — 128 bits, `TEXT NOT NULL UNIQUE`.
Derived from the repository identity, the row id, the timestamp, a retry counter
and 16 bytes of kernel entropy. Assignment retries on a UNIQUE collision and
then fails loudly rather than falling back to something sequential. It is an
identifier, not a secret.

### Indexes that are load-bearing

- `idx_decision_docs_status ON decision_documents(repo_id, current_status, id DESC)`
  — the hot query is "approved decisions for this repository"; without status in
  the index that is a seek followed by a fetch-and-discard per proposed
  document.
- `CREATE UNIQUE INDEX idx_decision_rev_current ON decision_revisions(document_id) WHERE state = 'APPROVED'`
  — rule 9 as a schema constraint. A second effective revision is impossible to
  bring into existence, so a wrong approve/supersede ordering fails loudly
  instead of leaving two that every later read quietly picks between.
- `idx_decision_links_path ON decision_links(path_raw) WHERE path_raw IS NOT NULL`
  — "which decisions concern this file?" seeks from the raw path bytes.

### The query shape that matters

Every listing joins the document projection to a bounded inner `SELECT DISTINCT`
of matching document ids rather than filtering with `d.id IN (…)`. The `IN`
spelling reads correctly and is linear in the *repository*: SQLite satisfies
`ORDER BY d.id DESC LIMIT n` by walking every document in id order, evaluating a
correlated head-revision subquery per row. Measured at ten thousand documents
that was 1 474 ms against a 100 ms budget. See the comment on
`DECISION_DOC_SELECT` in `src/db/db_decision.c`.

### FTS5

`decisions_fts` is created in `atlas_db_ensure_fts`, not in the numbered
migration, for the reason the top of `migrate.c` gives: FTS5 availability is a
property of the linked SQLite build rather than of the schema. Search degrades
to a bounded, repository-filtered scan of `decision_search` when it is absent.


## A6: revalidation (migration 7)

A6 assesses whether an approved decision is still about the code that is there
now. That assessment is **computed on every read and never stored**, for the
reason A4 gives about link currency: a cached answer to "is this still current?"
is wrong for exactly as long as nobody has recomputed it.

So the phase adds one table, and it is not a table of assessments. It records the
*human act* that a stale assessment calls for.

### `decision_challenges`, rebuilt

SQLite cannot widen a CHECK in place, and the `intent` vocabulary gains
`revalidate`. The migration therefore rebuilds the table — the first Atlas
migration to rebuild an existing one rather than only add new ones.

**Row ids are preserved exactly.** `decision_events.challenge_id` points into
this table without a foreign key, so a rebuild that renumbered would silently
re-point every approval record at somebody else's capability, and nothing about
the result would look wrong. `tests/test_migrate7.c` compares the rows id for id
across the migration and then joins every event to the challenge it names.

Four columns are added, all NULL for every intent but `revalidate`:

| Column | What it is |
| --- | --- |
| `indexed_commit` | The repository state the capability was issued against. Compared again when it is spent: a difference is **commit drift** and refuses the operation. |
| `evidence_digest` | A digest of what the revision's anchors resolved to at issue time. A difference is **evidence drift**. |
| `prior_freshness` | The verdict the operator was shown. A closed A6 vocabulary. |
| `prior_reasons` | The reason codes they were shown, space-separated. Closed vocabulary; a token outside it is refused rather than stored. |

Both drift checks are pure database reads. Consumption runs on the writer thread
inside the transaction that spends the capability, where A1 forbids creating a
process or reading a file, so drift is detected without Git and without the
filesystem.

### `decision_validations`

The append-only revalidation ledger. Nothing updates a row and nothing deletes
one; there is no `_clear`, no `_prune` and no `_forget`.

**It is not part of the A4 ledger and changes no lifecycle state.** A revalidated
revision was `APPROVED` before and is `APPROVED` after, its approval event is
untouched, and `decision_events` keeps exactly the four transitions it had — so
the replay in `atlas_db_decision_verify` is the same function over the same
vocabulary as before. Revalidation records a different kind of act: not "this
became policy" but "somebody checked that it still describes this code".

| Column | Notes |
| --- | --- |
| `document_id`, `revision_id`, `revision_no` | What was revalidated. |
| `content_hash` | The digest it covered, recorded on the row for the reason `decision_events` records one: a later reader can see which bytes were revalidated without trusting that the revision row is the one that was there. |
| `repo_id` | A **soft** reference, for A4's reason — a foreign key would make `repo remove --yes` destroy validation history. |
| `repo_identity_hash` | The durable identity: the same path-qualified lineage fingerprint the documents carry. A revalidation is looked up by revision *and* identity, so one worktree's revalidation does not establish a validation point for another — the two are at different commits by construction. |
| `validated_at_commit` | The exact state it was checked against. This becomes the decision's new validation point, and every later assessment measures its change range from here. |
| `evidence_digest` | What the anchors resolved to at that moment. It becomes the baseline the direct-evidence question is asked against, which is what stops a revalidated decision from reporting `STALE` for ever — a revision is immutable, so its link snapshots can never be updated. |
| `actor` | `LOCAL_OPERATOR_CONFIRMED`, the only value the CHECK accepts. It says the operator channel was used. It does not name a person, does not prove one was present, and is not a signature. |
| `challenge_id` | The capability that was spent. `UNIQUE`, so one capability can produce at most one record even if the consumption check were somehow bypassed. |
| `prior_freshness`, `prior_reasons` | The assessment that prompted the revalidation, preserved rather than replaced. Without them the ledger would say a decision was revalidated and could not say what was wrong with it. |

`atlas doctor` checks this ledger's **structure**: every row must name a revision
and a document that exist, must carry the digest its revision carries, must
record a repository state, and must point at a challenge that was issued for a
revalidation and actually consumed. It deliberately does **not** re-derive the
evidence digests against the live index — those are meant to drift, and that is
`atlas gate check`'s question. Reported, never repaired.

Both tables are `CANONICAL` and not prunable; see `docs/operations.md`.

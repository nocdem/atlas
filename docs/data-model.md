# Data model

The database is a **rebuildable index**. Every row in it is derived from Git or
from the working tree, and every row can be reconstructed by rescanning. Nothing
in Atlas treats it as the canonical record of history.

Current schema version: **4**. `atlas doctor` reports the version in force and the
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

Recorded in A0, parsed in A2. One row per `compile_commands.json` found, unique on
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

# Data model

The database is a **rebuildable index**. Every row in it is derived from Git or
from the working tree, and every row can be reconstructed by rescanning. Nothing
in Atlas treats it as the canonical record of history.

Current schema version: **2**. `atlas doctor` reports the version in force and the
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

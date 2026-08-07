/* Atlas - numbered, transactional, idempotent schema migrations.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Rules:
 *   - migrations are numbered and applied in ascending order
 *   - each migration runs inside its own transaction and is rolled back whole
 *     if any statement in it fails
 *   - applying migrations to an up-to-date database is a no-op
 *   - FTS5 objects are NOT part of a numbered migration: their availability is a
 *     property of the linked SQLite build, not of the schema, so they are
 *     created idempotently after migration and reported by `atlas doctor`.
 */
#include "db/db_internal.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

static const char M1_BOOKKEEPING[] =
    /* Bookkeeping ------------------------------------------------------- */
    "CREATE TABLE schema_migrations ("
    "  version INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  applied_at TEXT NOT NULL"
    ");";

/* Repositories ----------------------------------------------------------- */
static const char M1_REPOSITORIES[] =
    "CREATE TABLE repositories ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL UNIQUE,"
    "  root_path BLOB NOT NULL UNIQUE,"     /* canonical root, exact bytes */
    "  root_path_text TEXT NOT NULL,"       /* safe text form */
    "  git_common_dir BLOB,"
    "  git_common_dir_text TEXT,"
    "  object_format TEXT NOT NULL DEFAULT 'unknown',"
    "  registered_at TEXT NOT NULL,"
    "  last_scan_at TEXT,"
    /* Soft reference: scans is created after this table, so this deliberately
     * carries no FK constraint. It is cleared by ON DELETE CASCADE of scans. */
    "  last_scan_id INTEGER,"
    "  scanned_head TEXT,"
    "  current_branch TEXT,"
    "  head_state TEXT NOT NULL DEFAULT 'unknown',"
    "  dirty INTEGER NOT NULL DEFAULT 0,"
    "  dirty_staged INTEGER NOT NULL DEFAULT 0,"
    "  dirty_unstaged INTEGER NOT NULL DEFAULT 0,"
    "  dirty_untracked INTEGER NOT NULL DEFAULT 0,"
    "  dirty_unmerged INTEGER NOT NULL DEFAULT 0"
    ");";

/* Scans ------------------------------------------------------------------ */
static const char M1_SCANS[] =
    "CREATE TABLE scans ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  started_at TEXT NOT NULL,"
    "  finished_at TEXT,"
    "  status TEXT NOT NULL CHECK(status IN ('running','ok','failed')),"
    "  head_oid TEXT,"
    "  head_state TEXT,"
    "  branch TEXT,"
    "  object_format TEXT,"
    "  dirty INTEGER NOT NULL DEFAULT 0,"
    "  files_total INTEGER NOT NULL DEFAULT 0,"
    "  files_added INTEGER NOT NULL DEFAULT 0,"
    "  files_modified INTEGER NOT NULL DEFAULT 0,"
    "  files_deleted INTEGER NOT NULL DEFAULT 0,"
    "  files_unchanged INTEGER NOT NULL DEFAULT 0,"
    "  files_unreadable INTEGER NOT NULL DEFAULT 0,"
    "  commits_ingested INTEGER NOT NULL DEFAULT 0,"
    "  error TEXT"
    ");"
    "CREATE INDEX idx_scans_repo ON scans(repo_id, id DESC);";

/* Files ------------------------------------------------------------------ */
static const char M1_FILES[] =
    "CREATE TABLE files ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  path_is_utf8 INTEGER NOT NULL DEFAULT 1,"
    "  file_type TEXT NOT NULL CHECK(file_type IN ('regular','symlink','other','missing')),"
    "  language TEXT,"
    "  git_mode TEXT,"
    "  git_index_oid TEXT,"
    "  content_hash TEXT,"
    "  content_hash_algo TEXT,"
    "  size_bytes INTEGER,"
    "  is_executable INTEGER NOT NULL DEFAULT 0,"
    "  is_symlink INTEGER NOT NULL DEFAULT 0,"
    "  unsafe_path INTEGER NOT NULL DEFAULT 0,"
    "  read_error TEXT,"
    "  first_seen_scan_id INTEGER NOT NULL,"
    "  last_seen_scan_id INTEGER NOT NULL,"
    "  first_seen_at TEXT NOT NULL,"
    "  last_seen_at TEXT NOT NULL,"
    "  deleted INTEGER NOT NULL DEFAULT 0,"
    "  deleted_at TEXT,"
    "  deleted_scan_id INTEGER,"
    "  UNIQUE(repo_id, path_raw)"
    ");"
    "CREATE INDEX idx_files_repo_live ON files(repo_id, deleted);"
    "CREATE INDEX idx_files_repo_text ON files(repo_id, path_text);";

/* Commits ---------------------------------------------------------------- */
static const char M1_COMMITS[] =
    "CREATE TABLE commits ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  oid TEXT NOT NULL,"
    "  parents TEXT NOT NULL DEFAULT '',"
    "  parent_count INTEGER NOT NULL DEFAULT 0,"
    "  author_name TEXT NOT NULL DEFAULT '',"
    "  author_email TEXT NOT NULL DEFAULT '',"
    "  author_time INTEGER,"
    "  commit_time INTEGER,"
    "  subject TEXT NOT NULL DEFAULT '',"
    "  body TEXT NOT NULL DEFAULT '',"
    "  ingested_scan_id INTEGER,"
    "  UNIQUE(repo_id, oid)"
    ");"
    "CREATE INDEX idx_commits_repo_time ON commits(repo_id, commit_time DESC);";

/* File changes ----------------------------------------------------------- */
static const char M1_FILE_CHANGES[] =
    "CREATE TABLE file_changes ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  commit_id INTEGER NOT NULL REFERENCES commits(id) ON DELETE CASCADE,"
    "  change_type TEXT NOT NULL CHECK(change_type IN"
    "    ('add','modify','delete','rename','copy','typechange','unmerged','unknown')),"
    "  score INTEGER,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  old_path_raw BLOB,"
    "  old_path_text TEXT,"
    "  raw_status TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_changes_commit ON file_changes(commit_id);"
    "CREATE INDEX idx_changes_repo_path ON file_changes(repo_id, path_raw);"
    "CREATE INDEX idx_changes_repo_oldpath ON file_changes(repo_id, old_path_raw);";

/* Compile databases (recorded in A0, parsed in A2) ------------------------ */
static const char M1_COMPILE_DATABASES[] =
    "CREATE TABLE compile_databases ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  is_regular_file INTEGER NOT NULL DEFAULT 0,"
    "  is_symlink INTEGER NOT NULL DEFAULT 0,"
    "  content_hash TEXT,"
    "  size_bytes INTEGER,"
    "  scan_id INTEGER NOT NULL,"
    "  seen_at TEXT NOT NULL,"
    "  parsed INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(repo_id, path_raw)"
    ");";

/* Evidence --------------------------------------------------------------- */
/* The CHECK lists every evidence type Atlas will ever produce so the schema is
 * stable across phases; A0 code only ever writes SOURCE and GIT. */
static const char M1_EVIDENCE[] =
    "CREATE TABLE evidence ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('SOURCE','GIT','DECISION','USER_STATEMENT','INFERENCE','UNKNOWN')),"
    "  scan_id INTEGER,"
    "  git_oid TEXT,"
    "  path_raw BLOB,"
    "  path_text TEXT,"
    "  commit_oid TEXT,"
    "  detail TEXT,"
    "  created_at TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_evidence_repo_kind ON evidence(repo_id, kind);"
    "CREATE INDEX idx_evidence_repo_path ON evidence(repo_id, path_raw);";

/* Migration 2: worktree identity.
 *
 * Several Git worktrees share one common Git directory but have distinct
 * canonical roots, HEADs, branches and dirty states. Migration 1 recorded only
 * the common dir, which made two worktrees of one repository indistinguishable
 * from each other in the index: you could see that they were related but not
 * which was which, and nothing recorded that one was linked rather than main.
 * That is an identity defect in the foundation, so it is fixed here rather than
 * deferred. */
static const char M2_WORKTREE_IDENTITY[] =
    "ALTER TABLE repositories ADD COLUMN git_dir BLOB;"
    "ALTER TABLE repositories ADD COLUMN git_dir_text TEXT;"
    "ALTER TABLE repositories ADD COLUMN is_linked_worktree INTEGER NOT NULL DEFAULT 0;"
    /* Worktrees of one repository are found by their shared common dir. */
    "CREATE INDEX idx_repositories_common_dir ON repositories(git_common_dir);";

static const char *const M2_STATEMENTS[] = {
    M2_WORKTREE_IDENTITY,
    NULL,
};

/* Migration 3: continuous indexing state.
 *
 * A0 answered one question: "what did the last scan see?". A1 has to answer a
 * harder one: "is what Atlas holds right now current, and if not, in what way is
 * it not current?". That needs state A0 never recorded — which generation of the
 * index is complete, when reconciliation last succeeded, whether the watcher has
 * an unresolved event gap, and a cursor an A2 consumer can resume from.
 *
 * Nothing here is destructive. Every statement is an ALTER TABLE ... ADD COLUMN
 * with a default, or a CREATE for a new object, so a schema-v2 database migrates
 * forward with its rows intact and no table is recreated. */

/* Filesystem identity, so a reconciliation can decide *not* to hash a file.
 * Without this every pass would rehash the whole tree, which is the single thing
 * A1 must not do. All columns are nullable: a row written by A0 simply has no
 * recorded identity yet and is treated as a rehash candidate exactly once.
 *
 * ctime is recorded alongside mtime and is not optional. mtime is writable —
 * `utimensat` lets anyone who can write a file restore the mtime it had before
 * the write — so an identity without ctime reports a cache hit for a same-length
 * in-place edit whose mtime was put back, and preserves the stale content hash
 * forever. Nothing in userspace can set ctime; every write to the inode,
 * including the utimensat itself, bumps it. */
static const char M3_FILE_IDENTITY[] =
    "ALTER TABLE files ADD COLUMN fs_dev INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_ino INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_size INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_mtime_sec INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_mtime_nsec INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_ctime_sec INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_ctime_nsec INTEGER;"
    "ALTER TABLE files ADD COLUMN fs_mode INTEGER;"
    /* A0 only ever recorded tracked files, so 1 is the correct backfill. */
    "ALTER TABLE files ADD COLUMN tracked INTEGER NOT NULL DEFAULT 1;"
    "ALTER TABLE files ADD COLUMN ignored INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE files ADD COLUMN truncated INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE files ADD COLUMN truncated_reason TEXT;"
    "ALTER TABLE files ADD COLUMN last_generation INTEGER NOT NULL DEFAULT 0;"
    "CREATE INDEX idx_files_repo_generation ON files(repo_id, last_generation);";

/* One row per registered worktree, describing how current its index is. Created
 * lazily by the daemon and by `atlas scan`, so a v2 database needs no backfill.
 *
 * `generation` is the pass currently in flight; `last_complete_generation` is
 * the newest pass that finished consistently. A reader is only ever shown the
 * latter, which is what makes a crash mid-pass invisible to readers.
 *
 * `event_gap` is the honesty bit: it is set when an IN_Q_OVERFLOW, a watch-limit
 * failure or a worker error means Atlas cannot prove it saw every change. While
 * it is set, Atlas must not describe the index as current. */
static const char M3_REPO_INDEX_STATE[] =
    "CREATE TABLE repo_index_state ("
    "  repo_id INTEGER PRIMARY KEY REFERENCES repositories(id) ON DELETE CASCADE,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  last_complete_generation INTEGER NOT NULL DEFAULT 0,"
    "  last_reconcile_at TEXT,"
    "  last_complete_at TEXT,"
    "  watch_state TEXT NOT NULL DEFAULT 'unwatched' CHECK(watch_state IN"
    "    ('unwatched','watching','degraded','incomplete','error')),"
    "  watch_detail TEXT,"
    "  watched_dirs INTEGER NOT NULL DEFAULT 0,"
    "  event_gap INTEGER NOT NULL DEFAULT 0,"
    "  pending_full_reconcile INTEGER NOT NULL DEFAULT 0,"
    "  last_error TEXT,"
    "  last_sync_seq INTEGER NOT NULL DEFAULT 0"
    ");";

/* The monotonic cursor A2 consumers resume from. `id` is database-wide and
 * strictly increasing, so "everything after cursor N" is one indexed range scan
 * and never depends on wall-clock time.
 *
 * `dedup_key` makes ingestion idempotent: the same observation replayed after a
 * restart collides on the partial unique index instead of appending a duplicate.
 * Rows here are pruned to ATLAS_EVENTS_RETAIN_PER_REPO. The durable SOURCE and
 * GIT evidence in `evidence` is never pruned with them. */
static const char M3_REPO_EVENTS[] =
    "CREATE TABLE repo_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('file_added','file_modified','file_deleted','head_changed','index_changed',"
    "     'reconcile_started','reconciled','degraded','recovered','overflow',"
    "     'watch_limit','branch_rewrite','error')),"
    "  path_raw BLOB,"
    "  path_text TEXT,"
    "  detail TEXT,"
    "  created_at TEXT NOT NULL,"
    "  dedup_key TEXT"
    ");"
    "CREATE INDEX idx_repo_events_repo ON repo_events(repo_id, id);"
    "CREATE UNIQUE INDEX idx_repo_events_dedup ON repo_events(repo_id, generation, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* Which commit each ref was at when its history was last ingested, so the next
 * pass walks `HEAD --not <tip>` rather than replaying the whole history. */
static const char M3_COMMIT_TIPS[] =
    "CREATE TABLE repo_commit_tips ("
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  ref_name TEXT NOT NULL,"
    "  tip_oid TEXT NOT NULL,"
    "  ingested_at TEXT NOT NULL,"
    "  PRIMARY KEY(repo_id, ref_name)"
    ");";

/* Single-row daemon liveness record. Bounded by its own CHECK to one row, so a
 * second daemon cannot append a second identity and make `daemon status`
 * ambiguous. Liveness is still proven by the advisory lock, not by this row: a
 * killed daemon leaves the row behind and the lock is what disproves it. */
static const char M3_DAEMON_STATE[] =
    "CREATE TABLE daemon_state ("
    "  id INTEGER PRIMARY KEY CHECK(id = 1),"
    "  pid INTEGER,"
    "  boot_id TEXT,"
    "  started_at TEXT,"
    "  last_heartbeat_at TEXT,"
    "  stopped_at TEXT,"
    "  protocol_version INTEGER NOT NULL DEFAULT 1,"
    "  atlas_version TEXT NOT NULL DEFAULT '',"
    "  socket_path TEXT"
    ");";

static const char *const M3_STATEMENTS[] = {
    M3_FILE_IDENTITY, M3_REPO_INDEX_STATE, M3_REPO_EVENTS, M3_COMMIT_TIPS, M3_DAEMON_STATE, NULL,
};

/* Migration 4: AI sessions, change sets, change reasons and decisions (A2).
 *
 * Nothing here is destructive. Every statement is a CREATE for a new object or
 * an ALTER TABLE ... ADD COLUMN with a default, so a schema-v3 database migrates
 * forward with every row intact and no existing table is recreated. There is a
 * test that seeds a v3 database through the shipped v1..v3 statements, populates
 * it, migrates it, and asserts every row survives.
 *
 * The A0 rule is untouched: `evidence` still CHECKs its six kinds and
 * `atlas_db_evidence_insert` still refuses everything but SOURCE and GIT. AI
 * records are a *separate* kind of thing and live in separate tables. Widening
 * `evidence` to fit them would have made "how does Atlas know this?" and "what
 * did a model claim?" the same question, which is exactly the confusion A2 has
 * to avoid. */

/* Provider-neutral client identity. Nothing in the schema names Claude: a
 * second adapter is another row, not another table. */
static const char M4_AI_CLIENTS[] =
    "CREATE TABLE ai_clients ("
    "  id INTEGER PRIMARY KEY,"
    "  provider TEXT NOT NULL,"           /* e.g. 'anthropic' */
    "  name TEXT NOT NULL,"               /* e.g. 'claude-code' */
    "  first_seen_at TEXT NOT NULL,"
    "  last_seen_at TEXT NOT NULL,"
    "  UNIQUE(provider, name)"
    ");";

/* One row per client session, including resumes and forks.
 *
 * `session_key` is the client's own identifier, safe-encoded on the way in. It
 * is unique per client rather than globally, because two providers may
 * legitimately choose the same string.
 *
 * `parent_id` carries resume and fork lineage, and a subagent is a session with
 * a parent and an `agent_type`. Modelling a subagent as a session rather than as
 * a flag means its change set, its reasons and its tool records are separable
 * from its parent's without a second set of tables. */
static const char M4_AI_SESSIONS[] =
    "CREATE TABLE ai_sessions ("
    "  id INTEGER PRIMARY KEY,"
    "  client_id INTEGER NOT NULL REFERENCES ai_clients(id) ON DELETE CASCADE,"
    "  session_key TEXT NOT NULL,"
    "  parent_id INTEGER REFERENCES ai_sessions(id) ON DELETE SET NULL,"
    "  agent_id TEXT,"
    "  agent_type TEXT,"
    "  client_version TEXT,"
    "  state TEXT NOT NULL DEFAULT 'open' CHECK(state IN ('open','closed','expired')),"
    "  started_at TEXT NOT NULL,"
    "  last_seen_at TEXT NOT NULL,"
    "  closed_at TEXT,"
    "  close_reason TEXT,"
    "  resumes INTEGER NOT NULL DEFAULT 0,"
    "  compactions INTEGER NOT NULL DEFAULT 0,"
    "  turns INTEGER NOT NULL DEFAULT 0,"
    "  tool_calls INTEGER NOT NULL DEFAULT 0,"
    "  records INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(client_id, session_key)"
    ");"
    "CREATE INDEX idx_ai_sessions_state ON ai_sessions(state, last_seen_at);";

/* Which repositories a session has been in. A session that changes directory or
 * gains a working directory gains a row here rather than replacing its
 * repository, because work done before the change still belongs to it. */
static const char M4_AI_SESSION_REPOS[] =
    "CREATE TABLE ai_session_repos ("
    "  session_id INTEGER NOT NULL REFERENCES ai_sessions(id) ON DELETE CASCADE,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  attached_at TEXT NOT NULL,"
    "  source TEXT NOT NULL DEFAULT 'session_start',"
    "  base_head TEXT,"
    "  PRIMARY KEY(session_id, repo_id)"
    ");"
    "CREATE INDEX idx_ai_session_repos_repo ON ai_session_repos(repo_id);";

/* Ephemeral hook observations. These exist for two reasons: idempotency, and a
 * bounded audit trail of what a session did.
 *
 * What is deliberately absent is the point of the table: no prompt, no tool
 * input, no tool result, no error text, no command line. A row records that a
 * named tool ran, whether it reported success, and at most one normalized path.
 * `dedup_key` makes a redelivered hook collide instead of appending.
 *
 * Rows here are pruned to ATLAS_AI_EVENTS_RETAIN_PER_SESSION. Durable reasons
 * and decisions are never pruned with them. */
static const char M4_AI_SESSION_EVENTS[] =
    "CREATE TABLE ai_session_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id INTEGER NOT NULL REFERENCES ai_sessions(id) ON DELETE CASCADE,"
    "  repo_id INTEGER REFERENCES repositories(id) ON DELETE CASCADE,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('session_open','session_resume','session_close','turn','tool_intent','tool_ok',"
    "     'tool_failed','batch','checkpoint','root_attached','turn_close')),"
    "  tool_name TEXT,"
    "  tool_use_id TEXT,"
    "  path_raw BLOB,"
    "  path_text TEXT,"
    "  created_at TEXT NOT NULL,"
    "  dedup_key TEXT"
    ");"
    "CREATE INDEX idx_ai_session_events_session ON ai_session_events(session_id, id);"
    "CREATE UNIQUE INDEX idx_ai_session_events_dedup ON ai_session_events(session_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* One change set per session per repository: the window over which that session
 * was in a position to change that repository. */
static const char M4_AI_CHANGE_SETS[] =
    "CREATE TABLE ai_change_sets ("
    "  id INTEGER PRIMARY KEY,"
    "  session_id INTEGER NOT NULL REFERENCES ai_sessions(id) ON DELETE CASCADE,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  opened_at TEXT NOT NULL,"
    "  closed_at TEXT,"
    "  base_head TEXT,"
    "  last_head TEXT,"
    "  base_generation INTEGER NOT NULL DEFAULT 0,"
    "  last_generation INTEGER NOT NULL DEFAULT 0,"
    "  truncated INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(session_id, repo_id)"
    ");"
    "CREATE INDEX idx_ai_change_sets_repo ON ai_change_sets(repo_id);";

/* Observed changed paths, with how the attribution was arrived at.
 *
 * `attribution` is the honest field. 'direct_edit' means this session invoked an
 * edit tool naming this path AND the index then observed the path change.
 * 'observed' means only the second half. 'ambiguous' means another session had
 * the same repository open over the same window, so neither claim is supportable
 * — and `concurrent_sessions` records how many, so the ambiguity is a number
 * rather than an adjective.
 *
 * Once a row is ambiguous it stays ambiguous: a later direct edit does not
 * retroactively make an earlier overlapping observation unambiguous. */
static const char M4_AI_CHANGED_PATHS[] =
    "CREATE TABLE ai_changed_paths ("
    "  id INTEGER PRIMARY KEY,"
    "  change_set_id INTEGER NOT NULL REFERENCES ai_change_sets(id) ON DELETE CASCADE,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  attribution TEXT NOT NULL DEFAULT 'observed' CHECK(attribution IN"
    "    ('direct_edit','observed','ambiguous')),"
    "  direct_tool TEXT,"
    "  first_at TEXT NOT NULL,"
    "  last_at TEXT NOT NULL,"
    "  occurrences INTEGER NOT NULL DEFAULT 1,"
    "  concurrent_sessions INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(change_set_id, path_raw)"
    ");";

/* Change-reason proposals.
 *
 * `approved` exists and is pinned to 0 by a CHECK. That is deliberate: A2 has no
 * way to prove a human approved anything — an argument claiming approval is a
 * string a model produced — so rather than leaving the column out and having a
 * later phase add it, or leaving it writable and having something set it, the
 * schema states that this phase may not. Lifting the restriction is a migration,
 * which is a change somebody has to make on purpose.
 *
 * `state` distinguishes a recorded reason from a recorded *absence* of one.
 * 'unknown' is a first-class row, not a missing row: "nobody said why" and
 * "Atlas was never asked" are different facts and a query has to tell them
 * apart. */
static const char M4_AI_REASONS[] =
    "CREATE TABLE ai_reasons ("
    "  id INTEGER PRIMARY KEY,"
    "  session_id INTEGER REFERENCES ai_sessions(id) ON DELETE SET NULL,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  change_set_id INTEGER REFERENCES ai_change_sets(id) ON DELETE SET NULL,"
    "  created_at TEXT NOT NULL,"
    "  provenance TEXT NOT NULL CHECK(provenance IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','USER_APPROVED_DECISION','UNKNOWN')),"
    "  state TEXT NOT NULL CHECK(state IN ('proposed','unknown','superseded')),"
    "  confidence TEXT NOT NULL DEFAULT 'none' CHECK(confidence IN"
    "    ('none','low','medium','high')),"
    "  summary TEXT,"
    "  detail TEXT,"
    "  unknown_reason TEXT,"
    "  approved INTEGER NOT NULL DEFAULT 0 CHECK(approved = 0),"
    "  dedup_key TEXT"
    ");"
    "CREATE INDEX idx_ai_reasons_repo ON ai_reasons(repo_id, id DESC);"
    "CREATE UNIQUE INDEX idx_ai_reasons_dedup ON ai_reasons(repo_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* The paths a reason concerns, keyed by raw bytes like every other path. */
static const char M4_AI_REASON_PATHS[] =
    "CREATE TABLE ai_reason_paths ("
    "  reason_id INTEGER NOT NULL REFERENCES ai_reasons(id) ON DELETE CASCADE,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  PRIMARY KEY(reason_id, path_raw)"
    ");"
    "CREATE INDEX idx_ai_reason_paths_path ON ai_reason_paths(path_raw);";

/* Architectural and implementation decision proposals. Same approval rule. */
static const char M4_AI_DECISIONS[] =
    "CREATE TABLE ai_decisions ("
    "  id INTEGER PRIMARY KEY,"
    "  session_id INTEGER REFERENCES ai_sessions(id) ON DELETE SET NULL,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  created_at TEXT NOT NULL,"
    "  provenance TEXT NOT NULL CHECK(provenance IN ('MODEL_PROPOSAL','MODEL_INFERENCE')),"
    "  state TEXT NOT NULL DEFAULT 'proposed' CHECK(state IN ('proposed','superseded')),"
    "  title TEXT NOT NULL,"
    "  statement TEXT NOT NULL,"
    "  rationale TEXT,"
    "  approved INTEGER NOT NULL DEFAULT 0 CHECK(approved = 0),"
    "  dedup_key TEXT"
    ");"
    "CREATE INDEX idx_ai_decisions_repo ON ai_decisions(repo_id, id DESC);"
    "CREATE UNIQUE INDEX idx_ai_decisions_dedup ON ai_decisions(repo_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

static const char M4_AI_DECISION_PATHS[] =
    "CREATE TABLE ai_decision_paths ("
    "  decision_id INTEGER NOT NULL REFERENCES ai_decisions(id) ON DELETE CASCADE,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  PRIMARY KEY(decision_id, path_raw)"
    ");"
    "CREATE INDEX idx_ai_decision_paths_path ON ai_decision_paths(path_raw);";

/* Links a model record to the SOURCE or GIT evidence that existed for the same
 * path, so "what did a model claim" and "what does Atlas actually know" stay
 * connected without either becoming the other. */
static const char M4_AI_EVIDENCE_LINKS[] =
    "CREATE TABLE ai_evidence_links ("
    "  id INTEGER PRIMARY KEY,"
    "  subject_kind TEXT NOT NULL CHECK(subject_kind IN ('reason','decision')),"
    "  subject_id INTEGER NOT NULL,"
    "  evidence_id INTEGER NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,"
    "  UNIQUE(subject_kind, subject_id, evidence_id)"
    ");";

/* Compaction checkpoints. Bounded counters only: the Atlas-owned session state
 * is already in the tables above, so a checkpoint records that compaction
 * happened and what the state was, never a summary of the conversation. */
static const char M4_AI_CHECKPOINTS[] =
    "CREATE TABLE ai_checkpoints ("
    "  id INTEGER PRIMARY KEY,"
    "  session_id INTEGER NOT NULL REFERENCES ai_sessions(id) ON DELETE CASCADE,"
    "  created_at TEXT NOT NULL,"
    "  phase TEXT NOT NULL CHECK(phase IN ('pre_compact','post_compact')),"
    "  repos INTEGER NOT NULL DEFAULT 0,"
    "  changed_paths INTEGER NOT NULL DEFAULT 0,"
    "  unresolved_paths INTEGER NOT NULL DEFAULT 0,"
    "  reasons INTEGER NOT NULL DEFAULT 0,"
    "  decisions INTEGER NOT NULL DEFAULT 0,"
    "  dedup_key TEXT"
    ");"
    "CREATE UNIQUE INDEX idx_ai_checkpoints_dedup ON ai_checkpoints(session_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* Per-path working-tree change scope, as the last reconciliation observed it.
 *
 * A1 recorded only the dirty *counts* per repository, which is enough to say
 * "this repository has staged changes" and not enough to say which paths they
 * are. A2's changed-files tool has to answer the second question from the index
 * rather than by running git inside the serve loop, so the reconciliation pass
 * — which already runs `git status --porcelain=v2` for the counts — now records
 * the entries it was already parsing.
 *
 * The table is a snapshot, not a journal: each pass replaces the repository's
 * rows wholesale, because a path that is no longer dirty is not a historical
 * fact worth keeping. `generation` is what makes a stale snapshot visible. */
static const char M4_WORKTREE_CHANGES[] =
    "CREATE TABLE repo_worktree_changes ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  scope TEXT NOT NULL CHECK(scope IN ('staged','unstaged','untracked','unmerged')),"
    "  status TEXT NOT NULL,"
    "  change_type TEXT NOT NULL,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    "  old_path_raw BLOB,"
    "  old_path_text TEXT,"
    "  is_directory INTEGER NOT NULL DEFAULT 0,"
    "  observed_at TEXT NOT NULL,"
    "  UNIQUE(repo_id, scope, path_raw)"
    ");"
    "CREATE INDEX idx_worktree_changes_repo ON repo_worktree_changes(repo_id, scope);";

static const char *const M4_STATEMENTS[] = {
    M4_AI_CLIENTS,        M4_AI_SESSIONS,      M4_AI_SESSION_REPOS, M4_AI_SESSION_EVENTS,
    M4_AI_CHANGE_SETS,    M4_AI_CHANGED_PATHS, M4_AI_REASONS,       M4_AI_REASON_PATHS,
    M4_AI_DECISIONS,      M4_AI_DECISION_PATHS, M4_AI_EVIDENCE_LINKS, M4_AI_CHECKPOINTS,
    M4_WORKTREE_CHANGES,  NULL,
};

static const char *const M1_STATEMENTS[] = {
    M1_BOOKKEEPING, M1_REPOSITORIES,       M1_SCANS,    M1_FILES,
    M1_COMMITS,     M1_FILE_CHANGES,       M1_COMPILE_DATABASES, M1_EVIDENCE,
    NULL,
};

/* Migration 5: the structural code graph (A3).
 *
 * Forward-only and purely additive. Every statement is a CREATE for a new
 * object; no existing table is recreated, no existing column is altered, and no
 * existing row is touched. A schema-v4 database migrates forward with everything
 * intact, which `tests/test_migrate3.c` asserts by seeding a v2 database and
 * driving it all the way here.
 *
 * The A0 rule is untouched again: `evidence` still CHECKs its six kinds and
 * `atlas_db_evidence_insert` still refuses everything but SOURCE and GIT.
 * Structural facts are a different kind of thing and carry their own
 * `resolution` and `provenance` columns — the same separation A2 made for model
 * records, and for the same reason. "How does Atlas know this?" and "what did a
 * lexical scan guess?" must not become one question.
 *
 * Every table here references `repositories(id) ON DELETE CASCADE`, directly or
 * through `code_files`, so `repo remove` remains a pure cascade.
 *
 * What a cascade does *not* cover is the ordinary case: `files` rows are
 * tombstoned rather than deleted, so a foreign key from `files` would never
 * fire on a deletion or a rename. Removing a file's graph rows is therefore
 * explicit writer-path work, and `code_files` is keyed on `(repo_id, path_raw)`
 * rather than on `files(id)` so that work is a single delete per path. */

/* One row per repository, describing how current the structural index is.
 *
 * The generation pair mirrors `repo_index_state` exactly, and for the same
 * reason: `last_complete_generation` is the only generation a reader is shown,
 * so a crash half way through a structural pass is invisible rather than
 * half-visible. It carries the *reconciliation* pass's generation number, so
 * "the structural index describes the file index" is an integer comparison
 * rather than a guess.
 *
 * The counters are recomputed at the end of each pass rather than incremented
 * as rows are written. Incremented counters drift the first time a path fails
 * part way through; recomputed ones cannot, and the cost is four indexed
 * COUNT(*) queries per pass. */
/* The producers Atlas has seen, interned.
 *
 * Both columns are Atlas-owned: `name` is a string literal in the binary and
 * `version` is an integer the binary decides. Nothing here is derived from a
 * repository, from a compile database or from a model, which is what makes the
 * pair safe to report. A row is created the first time a producer indexes
 * anything and is never updated, so an upgrade adds a row rather than rewriting
 * the history of what built what. */
static const char M5_CODE_ANALYZERS[] =
    "CREATE TABLE code_analyzers ("
    "  id INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  version INTEGER NOT NULL,"
    "  first_seen_at TEXT,"
    "  UNIQUE(name, version)"
    ");";

static const char M5_CODE_INDEX_STATE[] =
    "CREATE TABLE code_index_state ("
    "  repo_id INTEGER PRIMARY KEY REFERENCES repositories(id) ON DELETE CASCADE,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  last_complete_generation INTEGER NOT NULL DEFAULT 0,"
    "  last_indexed_at TEXT,"
    "  last_complete_at TEXT,"
    /* Set when a parse failed, a ceiling was reached, or resolution had to fall
     * back to the whole repository. While it is set, the structural index is
     * never described as current. */
    "  degraded INTEGER NOT NULL DEFAULT 0,"
    "  degraded_reason TEXT,"
    "  detail TEXT,"
    "  last_error TEXT,"
    "  files_indexed INTEGER NOT NULL DEFAULT 0,"
    "  files_parsed_last INTEGER NOT NULL DEFAULT 0,"
    "  symbols INTEGER NOT NULL DEFAULT 0,"
    "  relations INTEGER NOT NULL DEFAULT 0,"
    "  ambiguous INTEGER NOT NULL DEFAULT 0,"
    "  unresolved INTEGER NOT NULL DEFAULT 0,"
    "  compile_db_present INTEGER NOT NULL DEFAULT 0,"
    "  compile_db_hash TEXT,"
    "  compile_units INTEGER NOT NULL DEFAULT 0,"
    "  compile_entries_dropped INTEGER NOT NULL DEFAULT 0,"
    /* Which analyzer built this graph, as a reference into `code_analyzers`.
     *
     * Normalized on purpose. The alternative — a name and a version on every
     * relation — would put two more columns on six hundred thousand rows to say
     * the same thing six hundred thousand times, and a structural pass has
     * exactly one producer. One integer per repository is the whole fact.
     *
     * It is a *reference* rather than the values themselves so the per-fact case
     * stays reachable without a redesign: a future importer that mixes producers
     * — an optional SCIP index for the files it covers and this lexical analyzer
     * for the rest — adds `analyzer_id INTEGER REFERENCES code_analyzers(id)` to
     * `code_relations`, and that is one integer per row rather than two strings,
     * with the vocabulary already interned and already joined the same way. */
    "  analyzer_id INTEGER REFERENCES code_analyzers(id),"
    /* Whether every edge in the repository has been through resolution since
     * the last thing that could change the answer.
     *
     * This is what lets a pass that parsed nothing skip resolution instead of
     * re-attempting every unresolved edge. An UNRESOLVED edge becomes resolvable
     * only when the candidate universe changes — a file appears or leaves, a
     * definition is added or removed, the compile database changes — and all of
     * those make a pass parse or remove something. Re-attempting them when
     * nothing changed asks the same question of the same rows and gets the same
     * answer, which at five thousand files took a minute per pass.
     *
     * It is durable, and cleared at the *start* of a pass rather than inferred
     * at the end, so a crash half way through resolution leaves it 0 and the
     * next pass resolves. A flag derived from "did the last pass complete?"
     * would not survive that, which is the one case it exists for. */
    "  resolve_settled INTEGER NOT NULL DEFAULT 0"
    ");";

/* One row per structurally indexed file.
 *
 * `content_hash` is the load-bearing column: it is the hash of the bytes these
 * facts were extracted from, and selection for the next pass is a comparison
 * against `files.content_hash`. That is what makes "an unchanged pass parses
 * zero files" true even for a full content-verifying pass, which rehashes every
 * byte and finds the same hash.
 *
 * Keyed on the raw path bytes like every other path in Atlas. */
static const char M5_CODE_FILES[] =
    "CREATE TABLE code_files ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    /* A soft reference to `files(id)`, for diagnostics only. Not a foreign key:
     * a tombstoned file keeps its row, so the FK would never fire where it is
     * needed and would imply a cascade that does not exist. */
    "  file_id INTEGER NOT NULL DEFAULT 0,"
    "  path_raw BLOB NOT NULL,"
    "  path_text TEXT NOT NULL,"
    /* The last path component, indexed.
     *
     * Resolving `#include \"atlas/buf.h\"` against `include/atlas/buf.h` is a
     * *suffix* match, and a suffix match has no index — so without this every
     * unresolvable include (every `<stdio.h>`, every path Atlas cannot place)
     * costs a full scan of this table. On a five-thousand-file repository that
     * is tens of millions of row comparisons per pass, and it was the whole
     * cost of the initial index before this column existed.
     *
     * A basename is derivable from `path_raw`, so this is denormalised on
     * purpose: it turns the scan into a seek, and the suffix is still checked
     * exactly against the handful of rows the seek returns. */
    "  basename_raw BLOB NOT NULL DEFAULT x'',"
    "  language TEXT NOT NULL CHECK(language IN ('c','c-header','c-fragment')),"
    "  content_hash TEXT,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  parsed_at TEXT NOT NULL,"
    "  parse_status TEXT NOT NULL CHECK(parse_status IN ('ok','partial','failed','skipped')),"
    "  parse_detail TEXT,"
    "  truncated INTEGER NOT NULL DEFAULT 0,"
    "  truncated_reason TEXT,"
    "  include_guard INTEGER NOT NULL DEFAULT 0,"
    "  symbol_count INTEGER NOT NULL DEFAULT 0,"
    "  include_count INTEGER NOT NULL DEFAULT 0,"
    "  occurrence_count INTEGER NOT NULL DEFAULT 0,"
    "  bytes INTEGER NOT NULL DEFAULT 0,"
    "  lines INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(repo_id, path_raw)"
    ");"
    "CREATE INDEX idx_code_files_repo_text ON code_files(repo_id, path_text);"
    "CREATE INDEX idx_code_files_repo_gen ON code_files(repo_id, generation);"
    "CREATE INDEX idx_code_files_basename ON code_files(repo_id, basename_raw);";

/* Typed, evidence-backed roles. A file may hold several, and each says how it
 * was arrived at — path naming is evidence about a path, not proof about a
 * file, and a consumer shown `role=test basis=path_naming` knows exactly how
 * much Atlas knows. */
static const char M5_CODE_FILE_ROLES[] =
    "CREATE TABLE code_file_roles ("
    "  id INTEGER PRIMARY KEY,"
    "  code_file_id INTEGER NOT NULL REFERENCES code_files(id) ON DELETE CASCADE,"
    "  role TEXT NOT NULL CHECK(role IN"
    "    ('implementation','public_header','private_header','test','build_metadata',"
    "     'documentation','vendored','generated','unknown')),"
    "  basis TEXT NOT NULL CHECK(basis IN"
    "    ('extension','path_naming','content_marker','build_metadata','include_graph','none')),"
    "  resolution TEXT NOT NULL CHECK(resolution IN"
    "    ('SOURCE_EXACT','BUILD_METADATA','UNIQUE_LEXICAL','AMBIGUOUS','UNRESOLVED',"
    "     'CONDITIONAL','MODEL_PROPOSAL','UNKNOWN')),"
    "  UNIQUE(code_file_id, role, basis)"
    ");";

/* A symbol is a *site*, not a global entity.
 *
 * Two files each defining `static void helper(void)` produce two rows and
 * nothing merges them, because merging them would be a decision a lexical
 * indexer has no basis for. Cross-file identity is expressed by edges in
 * `code_relations` with a resolution class attached.
 *
 * `linkage` is load-bearing rather than decorative: an `internal` definition is
 * a candidate only for occurrences in the same file. `none` is the
 * preprocessor's answer, and `unknown` is a real outcome the extractor reports
 * rather than guesses past. */
static const char M5_CODE_SYMBOLS[] =
    "CREATE TABLE code_symbols ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  code_file_id INTEGER NOT NULL REFERENCES code_files(id) ON DELETE CASCADE,"
    /* Raw name bytes are the lookup key; `name_text` is the safe display form,
     * exactly as with paths. A C identifier is ASCII in practice and not
     * guaranteed to be, and the one place that assumption is wrong is the one
     * place it matters. */
    "  name BLOB NOT NULL,"
    "  name_text TEXT NOT NULL,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('function','macro','macro_function','typedef','struct','union','enum',"
    "     'enum_constant','variable','unknown')),"
    "  linkage TEXT NOT NULL CHECK(linkage IN ('external','internal','none','unknown')),"
    "  resolution TEXT NOT NULL CHECK(resolution IN"
    "    ('SOURCE_EXACT','BUILD_METADATA','UNIQUE_LEXICAL','AMBIGUOUS','UNRESOLVED',"
    "     'CONDITIONAL','MODEL_PROPOSAL','UNKNOWN')),"
    "  is_definition INTEGER NOT NULL DEFAULT 0,"
    "  is_declaration INTEGER NOT NULL DEFAULT 0,"
    "  line INTEGER NOT NULL DEFAULT 0,"
    "  col INTEGER NOT NULL DEFAULT 0,"
    "  byte_offset INTEGER NOT NULL DEFAULT 0,"
    "  end_line INTEGER NOT NULL DEFAULT 0,"
    "  enclosing_id INTEGER REFERENCES code_symbols(id) ON DELETE SET NULL,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(code_file_id, kind, name, byte_offset)"
    ");"
    /* Resolution looks symbols up by name and linkage across a repository, and
     * the graph queries look them up by file. Both directions are indexed
     * because both are on a hot path.
     *
     * `linkage` is in the repository-wide index deliberately, and it is the
     * difference between linear and quadratic. Every file in a large C project
     * has a `static` helper with the same handful of names, so a lookup of
     * `helper` across the repository visits one index entry per file — and
     * without linkage in the index, each of those entries costs a row fetch and
     * a join before being discarded. With it, the filter happens inside the
     * index and the discarded entries cost almost nothing.
     *
     * The by-file index carries `name` for the same reason: an internal symbol
     * is a candidate only inside its own file, so that lookup is
     * `(code_file_id, name)` and must be a seek rather than a scan of the file's
     * symbols. */
    "CREATE INDEX idx_code_symbols_name ON code_symbols(repo_id, name, linkage, is_definition);"
    "CREATE INDEX idx_code_symbols_text ON code_symbols(repo_id, name_text);"
    "CREATE INDEX idx_code_symbols_file ON code_symbols(code_file_id, name);"
    /* Same reason as `idx_code_occ_enclosing`: `enclosing_id` is a self
     * reference with ON DELETE SET NULL, and enforcing it without an index on
     * the child column is a scan of this table per deleted symbol. */
    "CREATE INDEX idx_code_symbols_enclosing ON code_symbols(enclosing_id);";

/* A lexical call candidate inside a function body.
 *
 * The occurrence's *existence* is exact: those bytes really are an identifier
 * followed by `(`. What it refers to is a separate fact, carried by the
 * `symbol_calls_symbol` relation this row owns. */
static const char M5_CODE_OCCURRENCES[] =
    "CREATE TABLE code_occurrences ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  code_file_id INTEGER NOT NULL REFERENCES code_files(id) ON DELETE CASCADE,"
    "  enclosing_id INTEGER REFERENCES code_symbols(id) ON DELETE CASCADE,"
    "  name BLOB NOT NULL,"
    "  name_text TEXT NOT NULL,"
    "  kind TEXT NOT NULL CHECK(kind IN ('call_candidate')),"
    "  resolution TEXT NOT NULL CHECK(resolution IN"
    "    ('SOURCE_EXACT','BUILD_METADATA','UNIQUE_LEXICAL','AMBIGUOUS','UNRESOLVED',"
    "     'CONDITIONAL','MODEL_PROPOSAL','UNKNOWN')),"
    "  line INTEGER NOT NULL DEFAULT 0,"
    "  col INTEGER NOT NULL DEFAULT 0,"
    "  byte_offset INTEGER NOT NULL DEFAULT 0,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(code_file_id, byte_offset)"
    ");"
    "CREATE INDEX idx_code_occ_file ON code_occurrences(code_file_id);"
    "CREATE INDEX idx_code_occ_name ON code_occurrences(repo_id, name);"
    /* Not for a query — for the foreign key.
     *
     * `enclosing_id` cascades from `code_symbols`, and SQLite enforces a cascade
     * by looking for children of the row being deleted. Without an index on the
     * child column that lookup is a full scan of this table, once per deleted
     * symbol; reparsing one file deletes a dozen symbols and so scanned every
     * occurrence in the repository a dozen times. An unindexed foreign key is
     * invisible until the table is large, and then it is most of what a
     * one-file update costs. */
    "CREATE INDEX idx_code_occ_enclosing ON code_occurrences(enclosing_id);";

/* Every edge, in one table, with a typed endpoint on each side.
 *
 * One table rather than one per relation kind, and the reason is the queries
 * rather than tidiness: inbound and outbound traversal become the same query
 * shape over two indexes, which is what keeps reverse-dependency and impact
 * bounded and fast at two hundred thousand edges. Ten tables would mean ten
 * unions per traversal step.
 *
 * `dst_name` carries the *spelling* — the include text, the callee identifier —
 * and is kept whether or not the edge resolved. An include Atlas cannot place is
 * still a recorded fact; dropping it would be a silence, and "this file includes
 * something called config.h that I cannot find" is exactly the kind of thing a
 * reader needs to know.
 *
 * `owner_file_id` is what makes incremental replacement per file rather than
 * per repository: reindexing one file is one delete by this column followed by
 * the inserts. */
static const char M5_CODE_RELATIONS[] =
    "CREATE TABLE code_relations ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  owner_file_id INTEGER NOT NULL REFERENCES code_files(id) ON DELETE CASCADE,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('file_includes_file','file_defines_symbol','file_declares_symbol',"
    "     'unit_compiles_file','unit_uses_header','symbol_contains_occurrence',"
    "     'symbol_calls_symbol','symbol_declared_by','symbol_defined_by',"
    "     'file_depends_on_file')),"
    "  src_kind TEXT NOT NULL CHECK(src_kind IN ('file','symbol','unit','occurrence')),"
    "  src_id INTEGER NOT NULL,"
    "  dst_kind TEXT NOT NULL CHECK(dst_kind IN"
    "    ('file','symbol','unit','occurrence','unresolved')),"
    "  dst_id INTEGER NOT NULL DEFAULT 0,"
    "  dst_name BLOB,"
    "  dst_name_text TEXT,"
    /* How the destination was spelled, for the one relation kind where the
     * spelling changes what it means: a compiler searches the including file's
     * own directory for `\"x.h\"` and does not for `<x.h>`. Without this, an
     * angle include of a system header would resolve against a same-named file
     * next to the includer and be recorded as exact. NULL for every other
     * kind. */
    "  spelling_form TEXT CHECK(spelling_form IS NULL OR spelling_form IN ('quote','angle')),"
    "  resolution TEXT NOT NULL CHECK(resolution IN"
    "    ('SOURCE_EXACT','BUILD_METADATA','UNIQUE_LEXICAL','AMBIGUOUS','UNRESOLVED',"
    "     'CONDITIONAL','MODEL_PROPOSAL','UNKNOWN')),"
    "  provenance TEXT NOT NULL CHECK(provenance IN"
    "    ('SOURCE','BUILD_METADATA','INFERENCE','UNKNOWN')),"
    "  candidate_count INTEGER NOT NULL DEFAULT 0,"
    /* A fixed Atlas vocabulary (the ATLAS_CODE_WHY_* strings), never assembled
     * from repository bytes: this value reaches a model's context. */
    "  detail TEXT,"
    "  line INTEGER NOT NULL DEFAULT 0,"
    "  col INTEGER NOT NULL DEFAULT 0,"
    "  generation INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX idx_code_rel_src ON code_relations(repo_id, src_kind, src_id, kind);"
    "CREATE INDEX idx_code_rel_dst ON code_relations(repo_id, dst_kind, dst_id, kind);"
    "CREATE INDEX idx_code_rel_owner ON code_relations(owner_file_id);"
    /* Re-resolution finds edges by the name they mention, which is how a header
     * that gains a definition updates the call sites elsewhere that name it
     * without anything being reparsed.
     *
     * `id` is not named because it does not have to be: `id` is the rowid, and
     * SQLite appends the rowid to every index. So this index is really ordered
     * by `(repo_id, kind, dst_name, id)`, and the by-name resolution sweep's
     * `AND r.id > ?` cursor is a range constraint on it rather than a filter. */
    "CREATE INDEX idx_code_rel_name ON code_relations(repo_id, kind, dst_name);"
    /* The paging key for a resolution sweep that is *not* restricted to a name.
     *
     * Without it, `WHERE repo_id=? AND kind=? AND id > ? ORDER BY id LIMIT n`
     * reached its rows through `idx_code_rel_name`, which orders by `dst_name`
     * and therefore cannot satisfy the ORDER BY. SQLite compensated with a temp
     * B-tree — so every page sorted *every* edge of that kind in the repository
     * and then discarded all but `n` of them. Paging through the whole kind
     * repeated that once per page, which is quadratic in the repository: at five
     * thousand files it was, measured, the dominant cost of both the initial
     * pass and every pass afterwards.
     *
     * With this index the cursor is a seek and the LIMIT stops the scan, so a
     * sweep costs one pass over the kind however many pages it takes. */
    "CREATE INDEX idx_code_rel_kind_id ON code_relations(repo_id, kind, id);";
/* There is deliberately no index on `resolution` alone.
 *
 * It looks useful — the resolution sweeps filter on it and the state counters
 * count by it — and it is not. The sweeps reach their rows through
 * `idx_code_rel_kind_id` and filter resolution from the row; the counters run
 * once per pass that wrote anything and are perfectly happy to scan. What an
 * index on it would cost is a B-tree insertion on every one of a few hundred
 * thousand relation inserts, and an update on every resolution — for a column
 * whose value changes for most rows during the pass that inserts them. */

/* The candidate set behind an AMBIGUOUS edge.
 *
 * Atlas does not choose between same-named symbols, so the alternatives are
 * stored rather than discarded. `candidate_count` on the relation reports the
 * true number even when more candidates existed than are kept here, so the
 * ambiguity is never understated by the ceiling. */
static const char M5_CODE_CANDIDATES[] =
    "CREATE TABLE code_candidates ("
    "  id INTEGER PRIMARY KEY,"
    "  relation_id INTEGER NOT NULL REFERENCES code_relations(id) ON DELETE CASCADE,"
    "  node_kind TEXT NOT NULL CHECK(node_kind IN ('file','symbol','unit')),"
    "  node_id INTEGER NOT NULL,"
    "  rank INTEGER NOT NULL DEFAULT 0,"
    "  detail TEXT,"
    "  UNIQUE(relation_id, node_kind, node_id)"
    ");"
    "CREATE INDEX idx_code_candidates_rel ON code_candidates(relation_id, rank);";

/* One translation unit, from a validated compile-database record.
 *
 * `command_hash` and `command_present` are the whole of what is kept from the
 * `command` string. The string itself is deliberately not stored: it is a shell
 * command line, Atlas has no use for it beyond noticing that it changed, and a
 * value nothing holds is a value nothing can accidentally run.
 *
 * `(source_path_raw, output)` rather than source alone: one file compiled twice
 * with different flags is two configurations, and collapsing them would lose
 * exactly the distinction a compile database exists to record. */
static const char M5_CODE_UNITS[] =
    "CREATE TABLE code_units ("
    "  id INTEGER PRIMARY KEY,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  source_path_raw BLOB NOT NULL,"
    "  source_path_text TEXT NOT NULL,"
    "  output_text TEXT NOT NULL DEFAULT '',"
    "  directory_text TEXT NOT NULL DEFAULT '',"
    "  language_standard TEXT,"
    "  explicit_language TEXT,"
    "  arg_count INTEGER NOT NULL DEFAULT 0,"
    "  dropped_args INTEGER NOT NULL DEFAULT 0,"
    "  command_present INTEGER NOT NULL DEFAULT 0,"
    "  command_hash TEXT,"
    "  entry_index INTEGER NOT NULL DEFAULT 0,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(repo_id, source_path_raw, output_text)"
    ");"
    "CREATE INDEX idx_code_units_repo ON code_units(repo_id, source_path_text);";

/* An include directory a unit was configured with.
 *
 * `external` marks a directory outside the registered repository. It is stored
 * because it explains why an include resolved to nothing, and it is **never**
 * opened: nothing in Atlas reads a file from an external include directory.
 * Recording where a build looks is not the same as being allowed to look
 * there. */
static const char M5_CODE_UNIT_INCLUDES[] =
    "CREATE TABLE code_unit_includes ("
    "  id INTEGER PRIMARY KEY,"
    "  unit_id INTEGER NOT NULL REFERENCES code_units(id) ON DELETE CASCADE,"
    "  kind TEXT NOT NULL CHECK(kind IN ('search','quote','system','after')),"
    "  dir_raw BLOB NOT NULL,"
    "  dir_text TEXT NOT NULL,"
    "  external INTEGER NOT NULL DEFAULT 0,"
    "  rank INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(unit_id, kind, dir_raw)"
    ");"
    "CREATE INDEX idx_code_unit_inc_unit ON code_unit_includes(unit_id, rank);";

static const char M5_CODE_UNIT_DEFINES[] =
    "CREATE TABLE code_unit_defines ("
    "  id INTEGER PRIMARY KEY,"
    "  unit_id INTEGER NOT NULL REFERENCES code_units(id) ON DELETE CASCADE,"
    "  name TEXT NOT NULL,"
    "  value TEXT,"
    "  undef INTEGER NOT NULL DEFAULT 0,"
    "  rank INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(unit_id, name, undef)"
    ");";

/* Bounded indexing errors and truncation state.
 *
 * A ceiling that is reached silently is a ceiling that makes the index look
 * complete when it is not, so every one of them lands here and the repository is
 * marked degraded. Rows are pruned to ATLAS_CODE_ERRORS_RETAIN_PER_REPO; the
 * degraded flag is not, because the flag is the durable statement and these are
 * the detail behind it. */
static const char M5_CODE_INDEX_ERRORS[] =
    "CREATE TABLE code_index_errors ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  repo_id INTEGER NOT NULL REFERENCES repositories(id) ON DELETE CASCADE,"
    "  path_text TEXT,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('parse_failed','parse_partial','truncated','binary','too_large',"
    "     'compile_db_error','resolve_fallback','pass_truncated')),"
    "  detail TEXT,"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  created_at TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_code_errors_repo ON code_index_errors(repo_id, id);";

static const char *const M5_STATEMENTS[] = {
    /* `code_analyzers` first: `code_index_state` references it. */
    M5_CODE_ANALYZERS,
    M5_CODE_INDEX_STATE, M5_CODE_FILES,        M5_CODE_FILE_ROLES,   M5_CODE_SYMBOLS,
    M5_CODE_OCCURRENCES, M5_CODE_RELATIONS,    M5_CODE_CANDIDATES,   M5_CODE_UNITS,
    M5_CODE_UNIT_INCLUDES, M5_CODE_UNIT_DEFINES, M5_CODE_INDEX_ERRORS, NULL,
};

static const atlas_migration MIGRATIONS[] = {
    {1, "initial schema", M1_STATEMENTS},
    {2, "worktree identity", M2_STATEMENTS},
    {3, "continuous indexing state", M3_STATEMENTS},
    {4, "AI sessions, change reasons and decisions", M4_STATEMENTS},
    {5, "structural code graph", M5_STATEMENTS},
};

const atlas_migration *atlas_migrations(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]);
    }
    return MIGRATIONS;
}

/* schema_migrations does not exist before migration 1, so the applied set is
 * probed rather than queried. */
static atlas_status applied_version(atlas_db *db, int *out, atlas_err *err) {
    int v = atlas_db_schema_version(db, err);
    if (v < 0) {
        return err != NULL && err->status != ATLAS_OK ? err->status : ATLAS_ERR_DB;
    }
    *out = v;
    return ATLAS_OK;
}

static atlas_status record_migration(atlas_db *db, const atlas_migration *m, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "INSERT INTO schema_migrations(version, name, applied_at) VALUES(?1, ?2, ?3);", &stmt,
        err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int(stmt, 1, m->version) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind migration version");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, m->name, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_migrate_list(atlas_db *db, const atlas_migration *list, size_t count,
                                   atlas_err *err) {
    int current = 0;
    atlas_status st = applied_version(db, &current, err);
    if (st != ATLAS_OK) {
        return st;
    }

    for (size_t i = 0; i < count; i++) {
        const atlas_migration *m = &list[i];
        if (m->version <= current) {
            continue; /* already applied: migrations are idempotent as a set */
        }
        if (m->version != current + 1) {
            return atlas_err_set(err, ATLAS_ERR_DB,
                                 "migration %d is out of sequence (database is at %d)", m->version,
                                 current);
        }

        st = atlas_db_begin(db, err);
        if (st != ATLAS_OK) {
            return st;
        }
        for (size_t k = 0; st == ATLAS_OK && m->statements != NULL && m->statements[k] != NULL;
             k++) {
            st = atlas_db_exec_sql(db, m->statements[k], err);
        }
        if (st == ATLAS_OK) {
            st = record_migration(db, m, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_rollback(db);
            /* Preserve the sqlite message but make the failing migration clear. */
            char detail[ATLAS_ERR_MSG_MAX];
            (void)snprintf(detail, sizeof(detail), "%s", atlas_err_msg(err));
            return atlas_err_set(err, ATLAS_ERR_DB,
                                 "migration %d (%s) failed and was rolled back: %s", m->version,
                                 m->name != NULL ? m->name : "unnamed", detail);
        }
        st = atlas_db_commit(db, err);
        if (st != ATLAS_OK) {
            atlas_db_rollback(db);
            return st;
        }
        current = m->version;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_migrate(atlas_db *db, atlas_err *err) {
    if (db->read_only) {
        /* A reader must never be the thing that upgrades the schema: two readers
         * racing to migrate is exactly the corruption the single-writer rule
         * exists to prevent. Report the mismatch and let the caller decide. */
        int have = atlas_db_schema_version(db, err);
        if (have < 0) {
            return err != NULL && err->status != ATLAS_OK ? err->status : ATLAS_ERR_DB;
        }
        if (have == ATLAS_SCHEMA_VERSION) {
            return ATLAS_OK;
        }
        return atlas_err_set(err, ATLAS_ERR_DB,
                             "database schema is at version %d but this Atlas expects %d, and "
                             "this handle is read-only. Stop the daemon (systemctl --user stop "
                             "atlas) and run any Atlas command to migrate.",
                             have, ATLAS_SCHEMA_VERSION);
    }
    size_t count = 0;
    const atlas_migration *list = atlas_migrations(&count);
    atlas_status st = atlas_db_migrate_list(db, list, count, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_db_ensure_fts(db, err);
}

atlas_status atlas_db_ensure_fts(atlas_db *db, atlas_err *err) {
    if (!db->caps.fts5) {
        db->fts_ready = false;
        return ATLAS_OK; /* search degrades; doctor reports it */
    }
    static const char sql[] =
        "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
        "  path_text, content='files', content_rowid='id',"
        "  tokenize='unicode61 remove_diacritics 0');"
        "CREATE VIRTUAL TABLE IF NOT EXISTS commits_fts USING fts5("
        "  subject, body, content='commits', content_rowid='id',"
        "  tokenize='unicode61 remove_diacritics 0');";
    atlas_status st = atlas_db_exec_sql(db, sql, err);
    if (st != ATLAS_OK) {
        /* A build that advertises FTS5 but cannot create the tables must not
         * take the whole command down: fall back to degraded search. */
        db->caps.fts5 = false;
        db->fts_ready = false;
        atlas_err_init(err);
        return ATLAS_OK;
    }
    db->fts_ready = true;
    return ATLAS_OK;
}

atlas_status atlas_db_fts_rebuild(atlas_db *db, atlas_err *err) {
    if (!db->fts_ready) {
        return ATLAS_OK;
    }
    return atlas_db_exec_sql(db,
                             "INSERT INTO files_fts(files_fts) VALUES('rebuild');"
                             "INSERT INTO commits_fts(commits_fts) VALUES('rebuild');",
                             err);
}

bool atlas_db_fts_ready(const atlas_db *db) {
    return db->fts_ready;
}

/* --- incremental FTS5 maintenance ---------------------------------------
 *
 * These exist so that a one-file change costs one FTS row, not a whole
 * repository's reindex. The tables are external-content FTS5 over `files` and
 * `commits`, which means FTS5 does not see the underlying writes and cannot
 * derive the previous value of a column itself: removing a stale term requires
 * handing back the exact text that was indexed. Callers that only ever insert
 * (a new path, a new commit) never need the delete half.
 *
 * A failure here is never fatal to the caller's transaction on its own: the FTS
 * tables are derived data, and `atlas doctor` plus `atlas_db_fts_rebuild` can
 * reconstruct them from the facts. The status is still returned so the caller
 * decides. */

atlas_status atlas_db_fts_file_delete(atlas_db *db, int64_t file_id, const char *path_text,
                                      atlas_err *err) {
    if (!db->fts_ready) {
        return ATLAS_OK;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "INSERT INTO files_fts(files_fts, rowid, path_text) VALUES('delete', ?1, ?2);", &stmt,
        err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, file_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, path_text != NULL ? path_text : "", err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_fts_file_upsert(atlas_db *db, int64_t file_id, const char *old_path_text,
                                      const char *new_path_text, atlas_err *err) {
    if (!db->fts_ready) {
        return ATLAS_OK;
    }
    if (old_path_text != NULL) {
        atlas_status del = atlas_db_fts_file_delete(db, file_id, old_path_text, err);
        if (del != ATLAS_OK) {
            return del;
        }
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "INSERT INTO files_fts(rowid, path_text) VALUES(?1, ?2);", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, file_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, new_path_text != NULL ? new_path_text : "", err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_fts_commit_insert(atlas_db *db, int64_t commit_id, const char *subject,
                                        const char *body, size_t body_len, atlas_err *err) {
    if (!db->fts_ready) {
        return ATLAS_OK;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "INSERT INTO commits_fts(rowid, subject, body) VALUES(?1, ?2, ?3);", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, commit_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind commit id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, subject != NULL ? subject : "", err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 3, body != NULL ? body : "", body != NULL ? body_len : 0,
                                  err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

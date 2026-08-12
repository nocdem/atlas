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

/* Migration 6: decision documents, immutable revisions and operator approval
 * (A4).
 *
 * Forward-only, transactional and purely additive, like every migration before
 * it. Every statement is a CREATE for a new object. No existing table is
 * recreated, no existing column is altered, no existing CHECK is relaxed and no
 * existing row is touched — which `tests/test_migrate6.c` asserts by seeding a
 * populated schema-5 database and comparing every A0..A3 table row by row
 * across the migration, and again across a second, no-op migration.
 *
 * **The A2 restriction is deliberately left in place.** `ai_decisions.approved`
 * still CHECKs `approved = 0`, `atlas_provenance_writable_in_a2` still refuses
 * `USER_APPROVED_DECISION`, and neither A2 insert statement binds the column.
 * Lifting that CHECK was the obvious way to build this phase and it is the
 * wrong one: it would make an approval something that happens *to* a model's
 * own row, in the same table the model writes, distinguished from a proposal by
 * one integer. A4 approval is a different record about a different object —
 * an immutable revision — with its own actor vocabulary and its own append-only
 * ledger, so the A2 statement "a model proposal never becomes approved by
 * itself" stays literally true rather than becoming a historical note.
 *
 * **The evidence table is untouched again.** A4 writes no evidence at all.
 * `atlas_db_evidence_insert` still refuses everything except SOURCE and GIT,
 * and `INFERENCE` remains reserved and unused: A4 introduces no deterministic
 * inference with a defined provenance, so using the kind would only mean "we
 * had one spare". `tests/test_decision_lifecycle.c` asserts the table gains nothing
 * across a full decision lifecycle, and that the reserved kinds stay unused.
 *
 * **Nothing here cascades from `repositories`, and that is the one structural
 * break with every table above.** Everything else in the schema is a
 * rebuildable index keyed to a registered worktree, so `repo remove` is a pure
 * cascade. A decision document is not rebuildable from anything: it is the
 * canonical record of an approval, and rule 10 of the phase is that no
 * decision, revision, approval or rejection record is ever physically deleted.
 * A foreign key with ON DELETE CASCADE here would make `atlas repo remove
 * --yes` silently destroy approval history, so `repo_id` is a *soft* reference
 * and the durable identity is `repo_root_hash`. Removing a repository orphans
 * its decisions; registering the same root again relinks them by hash. See
 * docs/decision-lifecycle.md, and `atlas_db_decision_relink_repo`. */

/* The stable identity of one decision, across every revision of it.
 *
 * `uid` is the public handle: `atlas-dec-` and sixteen lowercase hex
 * characters, derived from values Atlas chose. It is opaque on purpose — it
 * carries no repository byte, so it is the one decision-derived value the
 * automatic context envelope is allowed to contain.
 *
 * `current_status` and `current_revision_id` are a **cache** of
 * `decision_events`, which is canonical. They exist because "what is the state
 * of this document" is asked by every list, every file query and every hook,
 * and replaying a ledger to answer it would put a correlated subquery on the
 * hot path. They are written in the same transaction as the event that changes
 * them, and `atlas_db_decision_verify` recomputes them from the ledger so the
 * cache can be checked rather than trusted. */
static const char M6_DECISION_DOCUMENTS[] =
    "CREATE TABLE decision_documents ("
    "  id INTEGER PRIMARY KEY,"
    "  uid TEXT NOT NULL UNIQUE,"
    /* Soft reference. No FK, no cascade: see the header comment above. */
    "  repo_id INTEGER NOT NULL,"
    /* SHA-256 of the canonical root path's raw bytes — the same value the
     * automatic context envelope reports as `root_hash`. Kept for reporting,
     * and **not** sufficient on its own to decide that two registrations are
     * the same repository: a path is a location, not an identity. */
    "  repo_root_hash TEXT NOT NULL,"
    /* The durable repository identity this document was written against, and
     * the only thing an automatic relink is allowed to match on.
     *
     * A **path-qualified lineage fingerprint**: the canonical root path, the
     * object format, **and the sorted set of root commits Atlas has ingested
     * for that repository**. `rm -rf` a repository, `git init` an unrelated one
     * at the same path, and the root commits differ, so the identity differs
     * and the old decisions stay orphaned. A path hash alone would have
     * attached them. Equally, the same lineage at a different path does not
     * match either, because the path is part of the fingerprint — automatic
     * reattachment requires all of it, and manual relinking is deferred.
     *
     * Empty when the lineage was unknown at the time — an unborn HEAD, or a
     * repository whose history had not been ingested. An empty identity never
     * matches anything, so such a document is never auto-relinked. That is the
     * fail-closed direction: an orphan is visible and recoverable, and a
     * decision attached to the wrong project is neither. */
    "  repo_identity_hash TEXT NOT NULL DEFAULT '',"
    "  created_at TEXT NOT NULL,"
    "  updated_at TEXT NOT NULL,"
    "  latest_revision_no INTEGER NOT NULL DEFAULT 0,"
    /* The approved, not-yet-superseded revision. NULL when there is none, which
     * is the normal state of a document nobody has approved. */
    "  current_revision_id INTEGER,"
    "  current_status TEXT NOT NULL DEFAULT 'PROPOSED' CHECK(current_status IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED')),"
    /* Document-level supersession: this document was replaced by another. A
     * soft self reference rather than an FK, because documents are never
     * deleted and an FK would only add an enforcement cost. */
    "  superseded_by_document_id INTEGER,"
    "  superseded_at TEXT"
    ");"
    "CREATE INDEX idx_decision_docs_repo ON decision_documents(repo_id, id DESC);"
    /* The list and count queries filter by repository *and* status, and the hot
     * one is "approved decisions for this repository". Without status in the
     * index that is a seek followed by a fetch and a discard per proposed
     * document, which at ten thousand documents is most of the query. */
    "CREATE INDEX idx_decision_docs_status ON decision_documents(repo_id, current_status, id DESC);"
    /* Relinking after a `repo remove` / `repo add` cycle seeks by identity. The
     * root hash is indexed too, for the orphan listing and for reporting. */
    "CREATE INDEX idx_decision_docs_root ON decision_documents(repo_root_hash);"
    "CREATE INDEX idx_decision_docs_identity ON decision_documents(repo_identity_hash)"
    "  WHERE repo_identity_hash <> '';";

/* One immutable revision.
 *
 * **No content column of this table is ever updated.** There is no UPDATE
 * statement in `db_decision.c` that names `title`, `context_text`,
 * `decision_text`, `rationale_text`, `consequences_text`, `scope`,
 * `content_hash` or `basis_head`; a change is a new row with the next
 * `revision_no`. `state` is updated, because a state is not content — it is
 * where the ledger has left this row — and `tests/test_decision_lifecycle.c`
 * asserts the distinction by hashing every content column before and after
 * every transition.
 *
 * `content_hash` is the domain-separated canonical digest from
 * `atlas_decision_content_hash`. Approval binds to it, so a revision whose
 * stored hash does not match a rehash of its stored content is a corrupt row
 * rather than a mild inconsistency: `atlas doctor` reports it and approval
 * refuses it. */
static const char M6_DECISION_REVISIONS[] =
    "CREATE TABLE decision_revisions ("
    "  id INTEGER PRIMARY KEY,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  title TEXT NOT NULL,"
    "  context_text TEXT NOT NULL DEFAULT '',"
    "  decision_text TEXT NOT NULL DEFAULT '',"
    "  rationale_text TEXT NOT NULL DEFAULT '',"
    "  consequences_text TEXT NOT NULL DEFAULT '',"
    "  scope TEXT NOT NULL DEFAULT 'UNKNOWN' CHECK(scope IN"
    "    ('UNKNOWN','REPOSITORY','SUBSYSTEM','PATHS')),"
    /* Which kind of actor proposed it. The operator channel may also propose —
     * a person typing `atlas decision propose` — so this is not a synonym for
     * \"a model wrote it\". */
    "  proposed_by TEXT NOT NULL CHECK(proposed_by IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC')),"
    /* Soft reference: A2 sessions may be expired, and a decision outlives the
     * conversation that proposed it. A2's attribution rule is unchanged — a
     * record that cannot be attached by exact key is stored sessionless with a
     * typed reason, never attached to a neighbouring session. */
    "  session_id INTEGER,"
    "  session_unbound INTEGER NOT NULL DEFAULT 0,"
    "  unbound_reason TEXT,"
    "  basis_head TEXT,"
    /* The repository identity **as captured when this revision was written**.
     *
     * It is on the revision rather than only on the document because it is part
     * of the canonical content hash, and a hashed input has to be immutable.
     * `decision_documents.repo_identity_hash` is attachment metadata: it starts
     * empty on a repository whose history has not been ingested and is
     * backfilled when the lineage becomes knowable. Hashing *that* meant an
     * ordinary propose-then-scan changed the input used to verify an
     * already-written revision, and `atlas doctor` reported a healthy record as
     * corrupt.
     *
     * Empty is a real recorded value meaning "nothing was knowable then", and
     * it stays empty. A later revision captures whatever is knowable at its own
     * write time, so revisions of one document may legitimately differ. */
    "  basis_repo_identity_hash TEXT NOT NULL DEFAULT '',"
    "  created_at TEXT NOT NULL,"
    "  state TEXT NOT NULL DEFAULT 'PROPOSED' CHECK(state IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED')),"
    /* Set when this revision was created by promoting an A2 `ai_decisions` row.
     * A soft reference to a row that is never modified and never deleted. */
    "  imported_from_ai_decision_id INTEGER,"
    /* Idempotency for a retried propose or revise, exactly as A2 does it. */
    "  dedup_key TEXT,"
    "  UNIQUE(document_id, revision_no)"
    ");"
    "CREATE INDEX idx_decision_rev_doc ON decision_revisions(document_id, revision_no DESC);"
    /* **Rule 9 of the phase, as a schema constraint rather than as care.**
     *
     * At most one approved revision may exist for one document. A partial
     * unique index makes a second one impossible to insert or update into
     * existence, so the atomicity of approve-and-supersede is enforced by
     * SQLite rather than by the ordering of two statements. A bug that got the
     * ordering wrong would fail loudly here instead of leaving two effective
     * revisions that every later read quietly picks between. */
    "CREATE UNIQUE INDEX idx_decision_rev_current ON decision_revisions(document_id)"
    "  WHERE state = 'APPROVED';"
    /* The promote path checks whether an A2 proposal has already been imported,
     * which must be a seek: it runs once per legacy row during a bulk promote. */
    "CREATE UNIQUE INDEX idx_decision_rev_import ON decision_revisions(imported_from_ai_decision_id)"
    "  WHERE imported_from_ai_decision_id IS NOT NULL;"
    "CREATE UNIQUE INDEX idx_decision_rev_dedup ON decision_revisions(document_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* Alternatives considered, in the order the proposer gave them.
 *
 * A child table rather than one blob because "three alternatives were
 * considered" is a countable fact that a reader, an export and a test can each
 * check, and a blob turns all three into string parsing. */
static const char M6_DECISION_ALTERNATIVES[] =
    "CREATE TABLE decision_alternatives ("
    "  id INTEGER PRIMARY KEY,"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  ordinal INTEGER NOT NULL,"
    "  text TEXT NOT NULL,"
    "  UNIQUE(revision_id, ordinal)"
    ");";

/* What a revision concerns.
 *
 * **Nothing here references a migration-5 table.** A `code_symbols` row is
 * derived data: a structural rebuild deletes it and an analyzer upgrade
 * replaces it, and both are routine. A durable decision whose subject
 * disappears when a cache is rebuilt would be a decision about nothing, so a
 * symbol link is a *snapshot* — the name bytes, the kind, the file it was in,
 * the line, the basis commit, that file's content hash at the time, and the
 * analyzer name and version that produced the fact.
 *
 * Resolution against that snapshot happens when the link is read, and its
 * outcome is reported rather than stored: CURRENT, CHANGED, MISSING, AMBIGUOUS
 * or UNKNOWN. Atlas never re-points a link. A rename produces MISSING, not a
 * quiet new target, because "the decision now refers to a different symbol" is
 * not something a rename can establish. */
static const char M6_DECISION_LINKS[] =
    "CREATE TABLE decision_links ("
    "  id INTEGER PRIMARY KEY,"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('path','commit','change_set','symbol','supersedes','replaced_by')),"
    /* Paths are bytes. `path_raw` is the key and `path_text` the lossless safe
     * encoding, exactly as in `files` and `code_files`. */
    "  path_raw BLOB,"
    "  path_text TEXT,"
    "  commit_oid TEXT,"
    "  change_set_id INTEGER,"
    /* Soft reference to another decision document, by row id. */
    "  target_document_id INTEGER,"
    /* The symbol snapshot, recorded whole or not at all. */
    "  symbol_name BLOB,"
    "  symbol_name_text TEXT,"
    "  symbol_kind TEXT,"
    "  symbol_line INTEGER NOT NULL DEFAULT 0,"
    "  basis_commit TEXT,"
    "  file_content_hash TEXT,"
    "  analyzer_name TEXT,"
    "  analyzer_version INTEGER NOT NULL DEFAULT 0,"
    "  created_at TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_decision_links_rev ON decision_links(revision_id, id);"
    /* \"Which decisions concern this file?\" is one of the four queries the
     * phase has to answer in bounded time, and it seeks from the path bytes. */
    "CREATE INDEX idx_decision_links_path ON decision_links(path_raw) WHERE path_raw IS NOT NULL;"
    "CREATE INDEX idx_decision_links_symbol ON decision_links(symbol_name)"
    "  WHERE symbol_name IS NOT NULL;"
    "CREATE INDEX idx_decision_links_commit ON decision_links(commit_oid)"
    "  WHERE commit_oid IS NOT NULL;"
    "CREATE INDEX idx_decision_links_target ON decision_links(target_document_id)"
    "  WHERE target_document_id IS NOT NULL;";

/* The append-only lifecycle ledger. **This is canonical.**
 *
 * Every transition is a row here, and no row is ever updated or deleted. The
 * status columns on the document and the revision are derived from it and are
 * written in the same transaction; `atlas_db_decision_verify` recomputes them
 * by replay and reports a disagreement rather than repairing one, so `atlas
 * doctor` can check the cache without becoming a thing that writes.
 *
 * `content_hash` is recorded on the event as well as on the revision. That is
 * not redundancy: it is what makes an approval bind to *content* rather than to
 * a row, so a later reader can see which bytes were approved without trusting
 * that the revision row is the one that was there.
 *
 * `actor` carries the honest name. `LOCAL_OPERATOR_CONFIRMED` means the
 * operator channel was used — a real terminal, a single-use challenge bound to
 * this exact revision and hash, and a confirmation typed against that hash. It
 * does not name a person and is not a signature. */
static const char M6_DECISION_EVENTS[] =
    "CREATE TABLE decision_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    /* NULL only for a document-level event that names no single revision. */
    "  revision_id INTEGER REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL DEFAULT 0,"
    "  event TEXT NOT NULL CHECK(event IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED')),"
    "  actor TEXT NOT NULL CHECK(actor IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC')),"
    "  content_hash TEXT,"
    /* Which challenge was spent. Present exactly on the events an operator
     * caused, absent on everything else, so \"was this approval channelled?\"
     * is answerable from the ledger alone. */
    "  challenge_id INTEGER,"
    /* What replaced this revision, when the event is SUPERSEDED. */
    "  superseded_by_revision_id INTEGER,"
    "  superseded_by_document_id INTEGER,"
    /* A fixed Atlas vocabulary, never assembled from repository or model bytes:
     * this value is reported and may reach a model. */
    "  detail TEXT,"
    "  created_at TEXT NOT NULL,"
    "  dedup_key TEXT"
    ");"
    "CREATE INDEX idx_decision_events_doc ON decision_events(document_id, id);"
    "CREATE INDEX idx_decision_events_rev ON decision_events(revision_id, id);"
    "CREATE UNIQUE INDEX idx_decision_events_dedup ON decision_events(document_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* Short-lived, single-use operator capabilities.
 *
 * A challenge is bound to a repository, a document, a revision *and* the
 * content hash of that revision. Every rejection the phase requires falls out
 * of that tuple plus two columns: an expired challenge fails on `expires_at`, a
 * replayed one on `consumed`, one issued for a different revision on
 * `revision_id`, and one whose content changed underneath it on `content_hash`
 * — which cannot happen through Atlas, since revisions are immutable, and is
 * checked anyway because \"cannot happen\" is not a check.
 *
 * Consumption is `UPDATE ... WHERE id = ? AND consumed = 0` inside the same
 * writer transaction as the transition, and the writer requires that it changed
 * exactly one row. So two concurrent approvals of one challenge cannot both
 * succeed, and a transition cannot happen without spending a capability.
 *
 * A consumed row is never deleted: it is part of the approval record, and
 * `challenge_id` on the event points at it. Pruning removes expired, unconsumed
 * rows only. */
static const char M6_DECISION_CHALLENGES[] =
    "CREATE TABLE decision_challenges ("
    "  id INTEGER PRIMARY KEY,"
    "  token TEXT NOT NULL UNIQUE,"
    "  repo_id INTEGER NOT NULL,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede')),"
    "  supersede_document_id INTEGER,"
    "  created_at TEXT NOT NULL,"
    "  expires_at TEXT NOT NULL,"
    "  consumed INTEGER NOT NULL DEFAULT 0,"
    "  consumed_at TEXT"
    ");"
    "CREATE INDEX idx_decision_challenges_repo ON decision_challenges(repo_id, consumed, expires_at);";

/* The searchable projection of one revision.
 *
 * A separate narrow table rather than a query over the prose columns, for one
 * reason that is measurable: the degraded search path is a scan, and scanning a
 * table whose rows are a bounded two kilobytes costs a fraction of scanning
 * five prose columns across every revision in the database. It is also the
 * content table for the optional FTS5 index.
 *
 * `haystack` is lowercased and bounded to ATLAS_DECISION_HAYSTACK_MAX. It is
 * derived data — regenerated whenever a revision is written — and is the one
 * decision table that a rebuild may legitimately recreate. */
static const char M6_DECISION_SEARCH[] =
    "CREATE TABLE decision_search ("
    "  revision_id INTEGER PRIMARY KEY REFERENCES decision_revisions(id),"
    "  document_id INTEGER NOT NULL,"
    "  repo_id INTEGER NOT NULL,"
    "  haystack TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_decision_search_repo ON decision_search(repo_id, revision_id);";

static const char *const M6_STATEMENTS[] = {
    M6_DECISION_DOCUMENTS, M6_DECISION_REVISIONS,  M6_DECISION_ALTERNATIVES,
    M6_DECISION_LINKS,     M6_DECISION_EVENTS,     M6_DECISION_CHALLENGES,
    M6_DECISION_SEARCH,    NULL,
};

/* --- 7: revalidation, and the capability that authorises one ---------------
 *
 * A6 assesses whether an approved decision is still about the code that is
 * there now. That assessment is computed on every read and never stored, for
 * the reason A4 gives about link currency: a cached answer to "is this still
 * current?" is wrong for exactly as long as nobody has recomputed it.
 *
 * So the phase adds one table, and it is not a table of assessments. It is the
 * record of the *human* act that a stale assessment calls for: an operator
 * looked at the decision against an exact repository state and said it still
 * stands. That is history rather than state, which is why it is stored, and it
 * is the reason the phase needs a migration at all.
 *
 * The challenge table is rebuilt rather than extended because SQLite cannot
 * widen a CHECK in place, and the intent vocabulary gains a member. It also
 * gains the two values a revalidation capability must be bound to and an
 * approval capability has no use for. Row ids are preserved exactly:
 * `decision_events.challenge_id` points into this table without a foreign key,
 * so a rebuild that renumbered would silently re-point every approval record at
 * somebody else's capability. */
static const char M7_CHALLENGES[] =
    /* Nothing REFERENCES decision_challenges, so this rebuild cannot orphan a
     * declared constraint; the soft reference from decision_events is what the
     * explicit id copy below protects. */
    "CREATE TABLE decision_challenges_new ("
    "  id INTEGER PRIMARY KEY,"
    "  token TEXT NOT NULL UNIQUE,"
    "  repo_id INTEGER NOT NULL,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede','revalidate')),"
    "  supersede_document_id INTEGER,"
    /* REVALIDATE only. The indexed head and the evidence digest the capability
     * was issued against.
     *
     * Both are bound at issue and compared at consume, and both comparisons are
     * pure database reads. That is deliberate: consumption happens on the
     * writer thread inside the transaction that spends the capability, and A1
     * forbids a git process or a file read in there. Commit drift and evidence
     * drift are therefore detected without either. */
    "  indexed_commit TEXT,"
    "  evidence_digest TEXT,"
    /* The assessment as it stood when the operator was shown it. Recorded here
     * rather than recomputed at consume so that what is preserved in the
     * validation record is what the human actually saw. `prior_reasons` is a
     * space-separated list of A6 reason codes — Atlas string literals from a
     * closed vocabulary, refused rather than reproduced if one is not in it. */
    "  prior_freshness TEXT CHECK(prior_freshness IS NULL OR prior_freshness IN"
    "    ('FRESH','STALE','IMPACTED','UNKNOWN')),"
    "  prior_reasons TEXT,"
    "  created_at TEXT NOT NULL,"
    "  expires_at TEXT NOT NULL,"
    "  consumed INTEGER NOT NULL DEFAULT 0,"
    "  consumed_at TEXT"
    ");"
    "INSERT INTO decision_challenges_new"
    "  (id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "   supersede_document_id, created_at, expires_at, consumed, consumed_at)"
    "  SELECT id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "         supersede_document_id, created_at, expires_at, consumed, consumed_at"
    "  FROM decision_challenges;"
    "DROP TABLE decision_challenges;"
    "ALTER TABLE decision_challenges_new RENAME TO decision_challenges;"
    "CREATE INDEX idx_decision_challenges_repo ON decision_challenges(repo_id, consumed, expires_at);";

/* The append-only revalidation ledger.
 *
 * **It does not change a lifecycle state and it is not part of the A4 ledger.**
 * A revalidated revision is still APPROVED, was always APPROVED, and its
 * approval event is untouched; `decision_events` keeps exactly the four
 * transitions it had, so the replay in `atlas_db_decision_verify` is the same
 * function over the same vocabulary as before. Revalidation is a second,
 * parallel record of a different kind of act: not "this became policy" but
 * "somebody checked that it still describes this code".
 *
 * Nothing updates a row here and nothing deletes one. `prior_freshness` and
 * `prior_reasons` preserve the assessment that prompted the revalidation, which
 * is the half a naive design loses: without it the ledger says a decision was
 * revalidated and cannot say what was wrong with it, and the record of a
 * concern that has been addressed is worth as much as the record of addressing
 * it.
 *
 * `repo_id` is a soft reference for A4's reason — an FK would make `repo remove
 * --yes` destroy validation history — and `repo_identity_hash` is the durable
 * identity, the same path-qualified lineage fingerprint the documents carry. */
static const char M7_VALIDATIONS[] =
    "CREATE TABLE decision_validations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL,"
    /* The digest the revalidation covered, recorded on the row for the reason
     * decision_events records it: so a later reader can see which bytes were
     * revalidated without trusting that the revision row is the one that was
     * there. */
    "  content_hash TEXT NOT NULL,"
    "  repo_id INTEGER,"
    "  repo_identity_hash TEXT,"
    /* The exact state it was revalidated against. This becomes the decision's
     * new validation point, and every later assessment measures its change
     * range from here. */
    "  validated_at_commit TEXT NOT NULL,"
    "  evidence_digest TEXT NOT NULL,"
    "  intent TEXT NOT NULL CHECK(intent IN ('revalidate')),"
    /* The honest name, and the only actor this table accepts. It says the
     * operator channel was used. It does not name a person, does not prove one
     * was present, and is not a signature. */
    "  actor TEXT NOT NULL CHECK(actor IN ('LOCAL_OPERATOR_CONFIRMED')),"
    "  challenge_id INTEGER NOT NULL,"
    "  prior_freshness TEXT NOT NULL CHECK(prior_freshness IN"
    "    ('FRESH','STALE','IMPACTED','UNKNOWN')),"
    "  prior_reasons TEXT NOT NULL,"
    "  created_at TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_decision_validations_rev ON decision_validations(revision_id, id);"
    "CREATE INDEX idx_decision_validations_doc ON decision_validations(document_id, id);"
    /* One capability, one validation. The challenge table already refuses a
     * second consumption; this makes the same fact true of the record, so a
     * replayed write cannot produce two rows even if the consumption check were
     * somehow bypassed. */
    "CREATE UNIQUE INDEX idx_decision_validations_challenge"
    "  ON decision_validations(challenge_id);";

static const char *const M7_STATEMENTS[] = {
    M7_CHALLENGES,
    M7_VALIDATIONS,
    NULL,
};

/* --- migration 8: the A8 durable orchestration control plane ---------------
 *
 * Eight tables, and the shape of each is an argument.
 *
 * These tables are **canonical**, not derived. A job record is the only account
 * of what was asked for, what was granted, what ran and what came back; nothing
 * rebuilds it from a repository, because the repository never held it. That is
 * why none of them appears as prunable in `RETENTION[]` and why there is no
 * `_clear` for any of them.
 *
 * `orch_jobs.repo_id` is a **soft reference with no foreign key**, exactly like
 * `decision_documents.repo_id` and for the same reason: an FK would make
 * `repo remove --yes` destroy execution history. `repo_identity_hash` is the
 * durable identity. And because `repositories.id` is a reused rowid, the pointer
 * is cleared when a repository is removed — in the same transaction as the
 * delete, by `atlas_db_orch_forget_repo`. A column holding a rowid that outlives
 * its row is the A4 defect, and it is not repeated here.
 *
 * Every CHECK on a state column deliberately omits 'UNKNOWN'. UNKNOWN is the
 * zero of `atlas_orch_state` and means "nobody filled this in"; a persisted job
 * may never be in it, so the schema refuses to store it rather than trusting
 * every writer to remember.
 */
static const char M8_JOBS[] =
    "CREATE TABLE orch_jobs ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    /* The external identifier. Random, unguessable and unique: a predictable
     * job id is one another local process can name before it exists. */
    "  job_uid TEXT NOT NULL UNIQUE,"
    "  spec_version INTEGER NOT NULL,"
    /* The canonical digest of everything immutable that was asked for. Two
     * submissions that digest identically are the same request, which is what
     * makes the idempotency key meaningful rather than decorative. */
    "  spec_digest TEXT NOT NULL,"
    /* From SO_PEERCRED at submission. Never from the request body — there is no
     * code path by which a client's own claim about its uid reaches this
     * column. */
    "  submitter_uid INTEGER NOT NULL,"
    "  repo_id INTEGER,"
    "  repo_name TEXT NOT NULL,"
    "  repo_identity_hash TEXT NOT NULL,"
    /* Exact and resolved before the job was persisted. A branch name never
     * reaches here: a moving reference in a stored specification is a job whose
     * source depends on when it happens to run. */
    "  source_commit TEXT NOT NULL,"
    "  mode TEXT NOT NULL,"
    "  driver TEXT NOT NULL,"
    /* UNTRUSTED_DATA. Stored as submitted, labelled at every boundary it
     * crosses, and never placed in automatic model context. */
    "  task_text TEXT NOT NULL,"
    /* Canonical netstring-encoded lists: length-prefixed, so no element can be
     * confused with a delimiter whatever it contains. */
    "  allowed_paths TEXT NOT NULL,"
    "  validations TEXT NOT NULL,"
    "  wall_timeout_ms INTEGER NOT NULL,"
    "  idle_timeout_ms INTEGER NOT NULL,"
    "  max_attempts INTEGER NOT NULL,"
    "  max_output_bytes INTEGER NOT NULL,"
    "  max_artifact_bytes INTEGER NOT NULL,"
    "  max_artifact_count INTEGER NOT NULL,"
    "  correlation TEXT NOT NULL DEFAULT '',"
    "  parent_job_uid TEXT NOT NULL DEFAULT '',"
    "  idempotency_key TEXT NOT NULL DEFAULT '',"
    "  state TEXT NOT NULL CHECK(state IN"
    "    ('QUEUED','LEASED','PREPARING','RUNNING','VALIDATING','SUCCEEDED','FAILED',"
    "     'CANCEL_REQUESTED','CANCELLED','TIMED_OUT','RECOVERY_REQUIRED')),"
    "  attempts_started INTEGER NOT NULL DEFAULT 0,"
    "  cancel_requested INTEGER NOT NULL DEFAULT 0 CHECK(cancel_requested IN (0,1)),"
    /* The id of the transition that produced the current state. Ordering
     * authority is this sequence, never a timestamp: two events in the same
     * millisecond are ordered by their ledger ids, and a clock that steps
     * backwards cannot reorder history. Timestamps are evidence. */
    "  state_seq INTEGER NOT NULL DEFAULT 0,"
    "  created_at TEXT NOT NULL,"
    "  created_ms INTEGER NOT NULL,"
    /* Absolute wall deadline, computed once at submission. A job that never
     * gets leased still has to end somewhere. */
    "  deadline_ms INTEGER NOT NULL,"
    "  terminal_at TEXT"
    ");"
    "CREATE INDEX idx_orch_jobs_state ON orch_jobs(state, id);"
    "CREATE INDEX idx_orch_jobs_repo ON orch_jobs(repo_identity_hash, id);"
    "CREATE INDEX idx_orch_jobs_submitter ON orch_jobs(submitter_uid, id);";

/* One row per execution attempt. `attempt_no` is monotonic per job and unique,
 * so a replayed or duplicated grant cannot produce two rows claiming to be the
 * same attempt. */
static const char M8_ATTEMPTS[] =
    "CREATE TABLE orch_attempts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  attempt_no INTEGER NOT NULL,"
    /* The kernel's answer about the dispatcher that took this attempt. */
    "  dispatcher_uid INTEGER NOT NULL,"
    "  dispatcher_id TEXT NOT NULL,"
    /* What the worker *says* its pid is. Recorded as the worker's claim and
     * used for nothing that matters: a worker describing itself is not evidence
     * about itself, and every authorisation decision uses the lease instead. */
    "  claimed_pid INTEGER NOT NULL DEFAULT 0,"
    "  state TEXT NOT NULL CHECK(state IN"
    "    ('LEASED','PREPARING','RUNNING','VALIDATING','SUCCEEDED','FAILED',"
    "     'CANCEL_REQUESTED','CANCELLED','TIMED_OUT','RECOVERY_REQUIRED')),"
    "  driver TEXT NOT NULL,"
    "  driver_version TEXT NOT NULL DEFAULT '',"
    "  exit_kind TEXT NOT NULL DEFAULT 'UNKNOWN',"
    "  exit_code INTEGER NOT NULL DEFAULT -1,"
    "  failure_reason TEXT NOT NULL DEFAULT 'UNKNOWN',"
    "  event_count INTEGER NOT NULL DEFAULT 0,"
    "  event_bytes INTEGER NOT NULL DEFAULT 0,"
    "  artifact_count INTEGER NOT NULL DEFAULT 0,"
    "  artifact_bytes INTEGER NOT NULL DEFAULT 0,"
    "  started_at TEXT NOT NULL,"
    "  ended_at TEXT,"
    "  UNIQUE(job_id, attempt_no)"
    ");"
    "CREATE INDEX idx_orch_attempts_job ON orch_attempts(job_id, attempt_no);";

/* The lease. A bearer capability, so the token itself is never stored — only a
 * domain-separated digest of it, and the token is handed to the dispatcher once
 * at grant and never again.
 *
 * The partial unique index is the whole concurrency guarantee: **at most one
 * unreleased lease per job**, enforced by the schema rather than by care. It is
 * what makes "no job is executed twice concurrently" a hard failure instead of
 * two dispatchers each believing they own the work. This is the shape A4 uses
 * for `at most one approved revision per document`, for the same reason. */
static const char M8_LEASES[] =
    "CREATE TABLE orch_leases ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  attempt_id INTEGER NOT NULL REFERENCES orch_attempts(id) ON DELETE CASCADE,"
    "  token_digest TEXT NOT NULL UNIQUE,"
    "  granted_at TEXT NOT NULL,"
    "  expires_ms INTEGER NOT NULL,"
    "  renewals INTEGER NOT NULL DEFAULT 0,"
    "  max_renewals INTEGER NOT NULL,"
    "  last_heartbeat_ms INTEGER NOT NULL,"
    "  released_at TEXT,"
    "  release_reason TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE UNIQUE INDEX idx_orch_leases_attempt ON orch_leases(attempt_id);"
    "CREATE UNIQUE INDEX idx_orch_leases_active ON orch_leases(job_id)"
    "  WHERE released_at IS NULL;"
    "CREATE INDEX idx_orch_leases_expiry ON orch_leases(expires_ms)"
    "  WHERE released_at IS NULL;";

/* The append-only state ledger. Nothing updates a row here and nothing deletes
 * one; `id` is AUTOINCREMENT so a deleted row's number can never be handed to a
 * later one, and it is the ordering authority the job row points at. */
static const char M8_TRANSITIONS[] =
    "CREATE TABLE orch_transitions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  attempt_id INTEGER,"
    "  from_state TEXT NOT NULL,"
    "  to_state TEXT NOT NULL,"
    /* A closed vocabulary. Free-form text here would be a place for
     * worker-chosen wording to end up reading like an Atlas statement; what the
     * worker says goes in orch_events, labelled as the worker's. */
    "  reason TEXT NOT NULL,"
    "  actor TEXT NOT NULL CHECK(actor IN ('CLIENT','DISPATCHER','ATLAS')),"
    "  actor_uid INTEGER NOT NULL DEFAULT 0,"
    "  detail TEXT NOT NULL DEFAULT '',"
    "  at TEXT NOT NULL"
    ");"
    "CREATE INDEX idx_orch_transitions_job ON orch_transitions(job_id, id);";

/* Structured worker events. `seq` is the worker's own counter and the unique
 * index over (attempt_id, seq) is what makes a duplicated delivery a refusal
 * rather than a second row — a retrying dispatcher must not be able to inflate
 * its own history. Counts and bytes are accumulated on the attempt so the bound
 * can be enforced without a scan. */
static const char M8_EVENTS[] =
    "CREATE TABLE orch_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  attempt_id INTEGER NOT NULL REFERENCES orch_attempts(id) ON DELETE CASCADE,"
    "  seq INTEGER NOT NULL,"
    "  kind TEXT NOT NULL,"
    /* UNTRUSTED_DATA, and known to be. Safe-encoded before it reaches a
     * terminal or a JSON document, and never treated as an instruction. */
    "  payload TEXT NOT NULL,"
    "  at TEXT NOT NULL,"
    "  UNIQUE(attempt_id, seq)"
    ");"
    "CREATE INDEX idx_orch_events_attempt ON orch_events(attempt_id, seq);";

/* The artifact manifest, and — for artifacts small enough to be worth it — the
 * bytes.
 *
 * The daemon cannot read the worker's workspace: it is 0700 `atlas-worker` and
 * `atlasd` is not that account. So an artifact is either carried inline in the
 * completion envelope, up to a bound, and stored here, or it is described by
 * name, size and digest and its bytes stay in the workspace. `content_stored`
 * says which, so a reader is never left to infer that an absent blob means an
 * empty file.
 *
 * There is deliberately **no path column**. An artifact is addressed by its
 * server-assigned id, never by a filesystem path a client could supply. */
static const char M8_ARTIFACTS[] =
    "CREATE TABLE orch_artifacts ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  attempt_id INTEGER NOT NULL REFERENCES orch_attempts(id) ON DELETE CASCADE,"
    "  name TEXT NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  size_bytes INTEGER NOT NULL,"
    "  sha256 TEXT NOT NULL,"
    "  content_stored INTEGER NOT NULL DEFAULT 0 CHECK(content_stored IN (0,1)),"
    "  content BLOB,"
    "  at TEXT NOT NULL,"
    "  UNIQUE(attempt_id, name)"
    ");"
    "CREATE INDEX idx_orch_artifacts_job ON orch_artifacts(job_id, id);";

/* Idempotent submission. The key is scoped to the submitter, because two
 * different principals choosing the same key are not making the same request.
 *
 * A replay with the *same* digest returns the existing job. A replay with a
 * *different* digest is a conflict and is refused: silently returning the older
 * job would run something other than what was asked for, and the caller would
 * have no way to notice. */
static const char M8_IDEMPOTENCY[] =
    "CREATE TABLE orch_idempotency ("
    "  submitter_uid INTEGER NOT NULL,"
    "  key TEXT NOT NULL,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  spec_digest TEXT NOT NULL,"
    "  created_at TEXT NOT NULL,"
    "  PRIMARY KEY(submitter_uid, key)"
    ");";
/* Deliberately *not* WITHOUT ROWID. It would be a marginally tighter table and
 * it would also be the one table in the schema with no rowid, which breaks every
 * generic query that orders by it — `tests/test_maintenance.c` digests each
 * retained table with `ORDER BY rowid` to prove a prune left it untouched.
 * A storage micro-optimisation is not worth being the exception. */

/* What the worker reported about itself over the life of an attempt. Written on
 * a phase change rather than on every heartbeat, so the row count is a property
 * of the state machine rather than of how long a job ran. Everything in it is
 * the worker's claim and is stored as such. */
static const char M8_OBSERVATIONS[] =
    "CREATE TABLE orch_observations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  attempt_id INTEGER NOT NULL REFERENCES orch_attempts(id) ON DELETE CASCADE,"
    "  at TEXT NOT NULL,"
    "  at_ms INTEGER NOT NULL,"
    "  claimed_pid INTEGER NOT NULL DEFAULT 0,"
    "  phase TEXT NOT NULL,"
    "  note TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE INDEX idx_orch_observations_attempt ON orch_observations(attempt_id, id);";

/* The source snapshot a leased attempt is entitled to receive.
 *
 * The manifest is persisted rather than held in memory so that a dispatcher
 * which restarts mid-transfer resumes against the *same* snapshot identity. A
 * re-enumeration could legitimately differ — the repository is a live directory —
 * and a worker that received the first half of one tree and the second half of
 * another would hold something that never existed.
 *
 * **No content is stored here.** Only the manifest: path, mode, object id, size
 * and digest. The bytes are re-read from the repository per chunk, because
 * SQLite is Atlas' rebuildable *index* and putting repository content in it
 * would make it something else. */
static const char M8_SNAPSHOTS[] =
    "CREATE TABLE orch_snapshots ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  attempt_id INTEGER NOT NULL REFERENCES orch_attempts(id) ON DELETE CASCADE,"
    "  job_id INTEGER NOT NULL REFERENCES orch_jobs(id) ON DELETE CASCADE,"
    "  protocol INTEGER NOT NULL,"
    "  source_commit TEXT NOT NULL,"
    "  tree_oid TEXT NOT NULL,"
    "  entry_count INTEGER NOT NULL,"
    "  total_bytes INTEGER NOT NULL,"
    "  digest TEXT NOT NULL,"
    "  refused_symlinks INTEGER NOT NULL DEFAULT 0,"
    "  refused_gitlinks INTEGER NOT NULL DEFAULT 0,"
    "  refused_other INTEGER NOT NULL DEFAULT 0,"
    "  created_at TEXT NOT NULL,"
    "  completed_at TEXT"
    ");"
    /* One snapshot per attempt. A second `open` returns the first rather than
     * enumerating again, and this index is what makes that a hard fact. */
    "CREATE UNIQUE INDEX idx_orch_snapshots_attempt ON orch_snapshots(attempt_id);";

/* The canonical ordered manifest. `idx` is the position in the order the digest
 * covers, so a transfer that reordered anything cannot match. */
static const char M8_SNAPSHOT_ENTRIES[] =
    "CREATE TABLE orch_snapshot_entries ("
    "  snapshot_id INTEGER NOT NULL REFERENCES orch_snapshots(id) ON DELETE CASCADE,"
    "  idx INTEGER NOT NULL,"
    /* Raw repository bytes: a path is bytes, not text. */
    "  path BLOB NOT NULL,"
    "  mode TEXT NOT NULL,"
    "  oid TEXT NOT NULL,"
    "  size_bytes INTEGER NOT NULL,"
    "  sha256 TEXT NOT NULL,"
    "  PRIMARY KEY(snapshot_id, idx)"
    ");";

static const char *const M8_STATEMENTS[] = {
    M8_JOBS,       M8_ATTEMPTS,  M8_LEASES,       M8_TRANSITIONS,
    M8_EVENTS,     M8_ARTIFACTS, M8_IDEMPOTENCY,  M8_OBSERVATIONS,
    M8_SNAPSHOTS,  M8_SNAPSHOT_ENTRIES,
    NULL,
};


/* --- migration 9: a general decision-to-decision relation ------------------
 *
 * `decision_links.kind` is a closed vocabulary enforced by a CHECK constraint,
 * and it held no way to say that one decision simply *relates to* another. The
 * two kinds that reference a document — `supersedes` and `replaced_by` — are
 * lifecycle facts: the supersede transition writes them and `recompute_status`
 * reads them, so using either for a general relation would make an ordinary
 * cross-reference change a document's status. That is why this is a migration
 * and not a reinterpretation of an existing value.
 *
 * SQLite cannot alter a CHECK, so the table is rebuilt. The rebuild copies
 * every column by name — ids, both endpoints, kind, timestamps and every
 * snapshot field — recreates all four indexes, and runs inside the single
 * transaction `atlas_db_migrate` already wraps each migration in, so a failure
 * leaves both the schema version and the rows exactly as they were.
 *
 * Nothing else changes. No other table is touched, no lifecycle rule moves, and
 * `relates_to` is deliberately inert: it resolves, it is reported, it is
 * preserved across transitions, and no status computation reads it. */
static const char M9_LINKS_NEW[] =
    "CREATE TABLE decision_links_new ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id) ON DELETE CASCADE,"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('path','commit','change_set','symbol','supersedes','replaced_by','relates_to')),"
    "  path_raw BLOB,"
    "  path_text TEXT,"
    "  commit_oid TEXT,"
    "  change_set_id INTEGER,"
    "  target_document_id INTEGER,"
    "  symbol_name BLOB,"
    "  symbol_name_text TEXT,"
    "  symbol_kind TEXT,"
    "  symbol_line INTEGER NOT NULL DEFAULT 0,"
    "  basis_commit TEXT,"
    "  file_content_hash TEXT,"
    "  analyzer_name TEXT,"
    "  analyzer_version INTEGER NOT NULL DEFAULT 0,"
    "  created_at TEXT NOT NULL"
    ");";

/* Column by column rather than `SELECT *`: a rebuild that relied on column
 * order would silently move data if the two definitions ever drifted. */
static const char M9_COPY[] =
    "INSERT INTO decision_links_new"
    "  (id, revision_id, kind, path_raw, path_text, commit_oid, change_set_id,"
    "   target_document_id, symbol_name, symbol_name_text, symbol_kind, symbol_line,"
    "   basis_commit, file_content_hash, analyzer_name, analyzer_version, created_at)"
    " SELECT id, revision_id, kind, path_raw, path_text, commit_oid, change_set_id,"
    "   target_document_id, symbol_name, symbol_name_text, symbol_kind, symbol_line,"
    "   basis_commit, file_content_hash, analyzer_name, analyzer_version, created_at"
    " FROM decision_links;";

static const char *const M9_STATEMENTS[] = {
    M9_LINKS_NEW,
    M9_COPY,
    "DROP TABLE decision_links;",
    "ALTER TABLE decision_links_new RENAME TO decision_links;",
    /* All four, exactly as migration 6 created them. */
    "CREATE INDEX idx_decision_links_rev ON decision_links(revision_id, id);",
    "CREATE INDEX idx_decision_links_path ON decision_links(path_raw)"
    "  WHERE path_raw IS NOT NULL;",
    "CREATE INDEX idx_decision_links_symbol ON decision_links(symbol_name)"
    "  WHERE symbol_name IS NOT NULL;",
    "CREATE INDEX idx_decision_links_commit ON decision_links(commit_oid)"
    "  WHERE commit_oid IS NOT NULL;",
    "CREATE INDEX idx_decision_links_target ON decision_links(target_document_id)"
    "  WHERE target_document_id IS NOT NULL;",
    NULL,
};

/* --- migration 10: durable evidence about a decision-to-decision edge -----
 *
 * Migration 9 made `relates_to` expressible. It did not make it explicable: an
 * edge could be drawn but the reason it was drawn had nowhere to live, so the
 * only copy of every justification stayed in the shell manifest that wrote the
 * edges. A relationship nobody can account for is a relationship nobody can
 * review.
 *
 * The reason is stored **outside the revision**, and that placement is the
 * whole design rather than a convenience:
 *
 *   - A revision is immutable and its links are covered by the canonical
 *     content hash. A rationale inside one would either change
 *     `ATLAS_DECISION_HASH_DOMAIN` — making every already-approved digest a
 *     claim about bytes that no longer encode the same way, which is exactly
 *     the corruption `atlas doctor` exists to report — or force a new revision
 *     and a fresh approval for every document that ever gained an edge.
 *   - An explanation attached after an approval is not part of what was
 *     approved. It is evidence *about* the edge. A6 draws the same line between
 *     the content hash, which never changes, and the evidence digest, which is
 *     expected to.
 *
 * The row is keyed by the **semantic edge** — source document, target document,
 * kind — and never by `decision_links.id`. A link row is written afresh with a
 * new id on every revision, so an id-keyed explanation would be silently lost
 * by the next revise, which is the failure this table exists to end.
 *
 * The table is append-only: one row per thing that happened to the edge, in
 * `id` order, never a timestamp (an A8 rule — wall-clock times are evidence,
 * not ordering). `ADDED` and `ANNOTATED` carry the rationale, `REMOVED` carries
 * the reason it was withdrawn, and the current rationale of an edge is the note
 * on its highest-id `ADDED`/`ANNOTATED` row. Nothing here decides whether an
 * edge is live: the current revision's links are canonical for that, and this
 * table is the account of how they came to be. Correcting a rationale appends;
 * there is no UPDATE and no DELETE, so a mistyped explanation is superseded in
 * the open rather than overwritten.
 *
 * Purely additive. No existing table is touched, no index is rebuilt, no
 * lifecycle rule moves and no stored hash changes — which is what makes this
 * migration safe to run against a database holding approved decisions. */
static const char *const M10_STATEMENTS[] = {
    "CREATE TABLE decision_edge_events ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    /* Both endpoints are decision_documents rowids. A decision record is never
     * deleted, so unlike A4's cross-model pointers these cannot outlive their
     * row, and the foreign key is real rather than soft. */
    "  source_document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  target_document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  kind TEXT NOT NULL CHECK(kind IN ('relates_to')),"
    "  event TEXT NOT NULL CHECK(event IN ('ADDED','ANNOTATED','REMOVED')),"
    /* Untrusted prose, stored raw and safe-encoded on the way out, exactly as
     * a revision's own text is. Encoding it here as well is what produced the
     * A8.2 double-encoding defect. */
    "  note TEXT NOT NULL,"
    "  provenance TEXT NOT NULL CHECK(provenance IN"
    "    ('OPERATOR','D1_MANIFEST','D3_REPAIR','UNKNOWN')),"
    /* The revision that carried the edge when the event was recorded, when one
     * is known. A soft reference: 0 means none was recorded. */
    "  revision_id INTEGER NOT NULL DEFAULT 0,"
    "  created_at TEXT NOT NULL"
    ");",
    /* The one read this table serves: every event for one edge, in order. */
    "CREATE INDEX idx_decision_edge_events_edge ON decision_edge_events"
    "  (source_document_id, target_document_id, kind, id);",
    /* And the account of one document's outgoing edges. */
    "CREATE INDEX idx_decision_edge_events_source ON decision_edge_events"
    "  (source_document_id, id);",
    NULL,
};

/* --- migration 11: the compiler-derived semantic index ----------------------
 *
 * Purely additive. No existing table is touched, no index is rebuilt, no
 * lifecycle rule moves and no stored hash changes — which is what makes this
 * safe to run against a database holding approved decisions, and why one
 * migration is enough for the whole season.
 *
 * The design decision that shapes every table here: **a semantic index is
 * derived, rebuildable data and nothing authoritative may depend on it.** So
 * nothing in A0..A8 gains a foreign key into these tables, `repo_id` is a soft
 * reference exactly as `orch_jobs.repo_id` is (a `repositories` rowid is reused,
 * and `atlas_db_repo_remove` clears it inside its own transaction), and every
 * row is scoped to a *generation* rather than to a repository. Dropping every
 * generation would lose nothing a pass cannot rebuild from the repository and
 * the compilation database.
 *
 * Generation scoping is also what makes replacement atomic without a rewrite.
 * A pass writes a new generation's rows while the previous generation is still
 * being read, and publication is one UPDATE of `sem_current`. A crash leaves a
 * RUNNING generation nobody points at, which the next pass reports and reaps —
 * A8's argument for `resolve_settled`, in the shape a generation table wants:
 * the durable record says what happened rather than what was intended. */
static const char *const M11_STATEMENTS[] = {
    /* One row per attempt to build an index. A failed attempt keeps its row on
     * purpose: "indexing this repository has failed four times today" is an
     * operational fact, and a table that only recorded successes could not
     * state it. */
    "CREATE TABLE sem_generations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    /* Soft reference. No FK: an FK would make `repo remove --yes` cascade into
     * the index, and more importantly a reused rowid would silently re-point a
     * generation at a different repository. Cleared on removal. */
    "  repo_id INTEGER NOT NULL DEFAULT 0,"
    /* The durable identity, kept so a generation can still say which repository
     * it described after the rowid is gone. */
    "  repo_identity_hash TEXT NOT NULL DEFAULT '',"
    "  commit_id TEXT NOT NULL DEFAULT '',"
    "  compdb_digest TEXT NOT NULL DEFAULT '',"
    "  compdb_count INTEGER NOT NULL DEFAULT 0,"
    "  compiler_id TEXT NOT NULL DEFAULT '',"
    "  compiler_version TEXT NOT NULL DEFAULT '',"
    "  analyzer_id TEXT NOT NULL DEFAULT '',"
    "  analyzer_version INTEGER NOT NULL DEFAULT 0,"
    /* UNKNOWN is deliberately absent, so a zeroed or malformed write cannot
     * produce a generation that looks runnable — the shape every A6/A8 state
     * CHECK uses. */
    "  status TEXT NOT NULL CHECK(status IN ('RUNNING','COMPLETE','FAILED')),"
    "  started_at TEXT NOT NULL,"
    "  completed_at TEXT NOT NULL DEFAULT '',"
    "  tu_total INTEGER NOT NULL DEFAULT 0,"
    "  tu_complete INTEGER NOT NULL DEFAULT 0,"
    "  tu_partial INTEGER NOT NULL DEFAULT 0,"
    "  tu_failed INTEGER NOT NULL DEFAULT 0,"
    "  tu_unsupported INTEGER NOT NULL DEFAULT 0,"
    "  symbol_count INTEGER NOT NULL DEFAULT 0,"
    "  edge_count INTEGER NOT NULL DEFAULT 0,"
    "  include_count INTEGER NOT NULL DEFAULT 0,"
    "  duration_ms INTEGER NOT NULL DEFAULT 0,"
    /* A fixed Atlas string or empty. Never compiler output: a diagnostic quotes
     * untrusted source. */
    "  failure_reason TEXT NOT NULL DEFAULT ''"
    ");",
    "CREATE INDEX idx_sem_generations_repo ON sem_generations(repo_id, id);",

    /* The atomic pointer. One row per repository, updated in the same
     * transaction that marks a generation COMPLETE, which is what makes
     * publication a single decision rather than a state readers can catch
     * half-made. A repository with no row has no index — ABSENT, which is a
     * different answer from STALE and must stay one. */
    "CREATE TABLE sem_current ("
    "  repo_id INTEGER PRIMARY KEY,"
    "  generation_id INTEGER NOT NULL REFERENCES sem_generations(id)"
    ");",

    /* The compilation databases a generation read, with the digest of each. Two
     * rows for a repository that presents two, which DNA does; the generation's
     * own `compdb_digest` is over all of them in path order, so a change to
     * either makes the generation stale. */
    "CREATE TABLE sem_compdbs ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  generation_id INTEGER NOT NULL REFERENCES sem_generations(id),"
    "  path_text TEXT NOT NULL,"
    "  digest TEXT NOT NULL,"
    "  entries INTEGER NOT NULL DEFAULT 0,"
    "  entries_dropped INTEGER NOT NULL DEFAULT 0"
    ");",
    "CREATE INDEX idx_sem_compdbs_gen ON sem_compdbs(generation_id);",

    /* One translation unit: a source compiled under one configuration.
     *
     * `config_digest` is what makes two compilations of one file two units
     * rather than one — the domain-separated, length-prefixed digest over the
     * include directories, defines, standard and language. `input_digest`
     * covers the source and every file it included, and is the whole basis of
     * incremental indexing: a unit whose input digest is unchanged is copied
     * forward rather than reparsed. */
    "CREATE TABLE sem_units ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  generation_id INTEGER NOT NULL REFERENCES sem_generations(id),"
    "  source_text TEXT NOT NULL,"
    "  compdb_id INTEGER NOT NULL DEFAULT 0,"
    "  config_digest TEXT NOT NULL DEFAULT '',"
    "  input_digest TEXT NOT NULL DEFAULT '',"
    "  status TEXT NOT NULL CHECK(status IN ('COMPLETE','PARTIAL','FAILED','UNSUPPORTED')),"
    "  why TEXT NOT NULL DEFAULT '',"
    "  diagnostics_errors INTEGER NOT NULL DEFAULT 0,"
    "  symbols INTEGER NOT NULL DEFAULT 0,"
    "  edges INTEGER NOT NULL DEFAULT 0,"
    "  duration_ms INTEGER NOT NULL DEFAULT 0,"
    "  reused INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(generation_id, source_text, config_digest)"
    ");",
    "CREATE INDEX idx_sem_units_gen_status ON sem_units(generation_id, status);",

    /* A symbol occurrence with an identity.
     *
     * The identity is Clang's USR, and a declaration and its definition share
     * one — correctly, they are the same entity — so `is_definition` is a
     * property of the row and the definition/declaration relationship is
     * answered by the rows that share a USR. That is why the unique key is the
     * whole site rather than the USR: two rows for one USR is the normal,
     * meaningful case.
     *
     * `external` marks an entity Atlas does not describe — a libc function, a
     * type from a system header — recorded so an edge has a destination.
     * External rows carry no location, because Atlas did not index the file. */
    "CREATE TABLE sem_symbols ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  generation_id INTEGER NOT NULL REFERENCES sem_generations(id),"
    "  usr TEXT NOT NULL,"
    "  name TEXT NOT NULL DEFAULT '',"
    "  kind TEXT NOT NULL DEFAULT 'UNKNOWN',"
    "  linkage TEXT NOT NULL DEFAULT 'UNKNOWN',"
    "  type_text TEXT NOT NULL DEFAULT '',"
    "  file_text TEXT NOT NULL DEFAULT '',"
    "  line INTEGER NOT NULL DEFAULT 0,"
    "  col INTEGER NOT NULL DEFAULT 0,"
    "  end_line INTEGER NOT NULL DEFAULT 0,"
    "  is_definition INTEGER NOT NULL DEFAULT 0,"
    "  external INTEGER NOT NULL DEFAULT 0,"
    "  evidence TEXT NOT NULL DEFAULT 'PROVEN'"
    "    CHECK(evidence IN ('PROVEN','CANDIDATE','LEXICAL','UNKNOWN')),"
    "  UNIQUE(generation_id, usr, file_text, line, is_definition)"
    ");",
    "CREATE INDEX idx_sem_symbols_name ON sem_symbols(generation_id, name);",
    "CREATE INDEX idx_sem_symbols_usr ON sem_symbols(generation_id, usr);",
    "CREATE INDEX idx_sem_symbols_file ON sem_symbols(generation_id, file_text);",

    /* An edge between two USRs.
     *
     * Endpoints are USRs rather than `sem_symbols` rowids on purpose: a call is
     * a fact about two *entities*, and an entity has several rows. Joining by
     * USR at read time keeps "which of the four declarations did you mean" a
     * question the reader answers rather than one the writer guessed at.
     *
     * `dst_usr` may be empty: an indirect call whose target Atlas cannot name is
     * a recorded edge with no destination, which is the single most important
     * thing a bounded call graph carries. `proto` holds the canonical prototype
     * that lets a later step attach candidates. */
    "CREATE TABLE sem_edges ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  generation_id INTEGER NOT NULL REFERENCES sem_generations(id),"
    "  kind TEXT NOT NULL CHECK(kind IN"
    "    ('CALLS','MAY_CALL','ADDRESS_TAKEN','REFERENCES','DECLARATION_OF',"
    "     'HAS_FIELD','HAS_TYPE','PARAM_TYPE','RETURN_TYPE','EXPANDED_FROM')),"
    "  src_usr TEXT NOT NULL DEFAULT '',"
    "  dst_usr TEXT NOT NULL DEFAULT '',"
    "  evidence TEXT NOT NULL"
    "    CHECK(evidence IN ('PROVEN','CANDIDATE','LEXICAL','UNKNOWN')),"
    "  unit_id INTEGER NOT NULL DEFAULT 0,"
    "  file_text TEXT NOT NULL DEFAULT '',"
    "  line INTEGER NOT NULL DEFAULT 0,"
    "  col INTEGER NOT NULL DEFAULT 0,"
    "  proto TEXT NOT NULL DEFAULT '',"
    /* The number of candidate targets that *existed*, which may exceed the
     * number recorded. A3's rule about `candidate_count`: a bound that makes an
     * ambiguity look smaller than it is is a bound that lies. */
    "  candidate_total INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(generation_id, kind, src_usr, dst_usr, file_text, line, col)"
    ");",
    "CREATE INDEX idx_sem_edges_src ON sem_edges(generation_id, src_usr, kind);",
    "CREATE INDEX idx_sem_edges_dst ON sem_edges(generation_id, dst_usr, kind);",
    "CREATE INDEX idx_sem_edges_proto ON sem_edges(generation_id, kind, proto);",
    "CREATE INDEX idx_sem_edges_file ON sem_edges(generation_id, file_text);",

    /* The include graph, as the preprocessor actually resolved it. `to_text`
     * empty means the directive led outside the repository: the spelling is
     * kept, the destination is not claimed. */
    "CREATE TABLE sem_includes ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  generation_id INTEGER NOT NULL REFERENCES sem_generations(id),"
    "  from_text TEXT NOT NULL,"
    "  to_text TEXT NOT NULL DEFAULT '',"
    "  spelling TEXT NOT NULL DEFAULT '',"
    "  line INTEGER NOT NULL DEFAULT 0,"
    "  evidence TEXT NOT NULL"
    "    CHECK(evidence IN ('PROVEN','CANDIDATE','LEXICAL','UNKNOWN')),"
    "  UNIQUE(generation_id, from_text, to_text, spelling, line)"
    ");",
    "CREATE INDEX idx_sem_includes_from ON sem_includes(generation_id, from_text);",
    "CREATE INDEX idx_sem_includes_to ON sem_includes(generation_id, to_text);",
    NULL,
};

/* --- migration 12: A9 remote credentials and the gateway audit trail --------
 *
 * Purely additive, like migration 11. Nothing in A0..A8 gains a column, a
 * foreign key or a CHECK, and no stored hash changes — which is what makes this
 * safe to run against a database holding approved decisions.
 *
 * Two tables, and they answer two different questions that must not be merged:
 * `api_keys` is *who may ask*, and `gw_audit` is *what was asked*. An audit row
 * therefore records the key id as plain text rather than as a foreign key: the
 * account of what a credential did must survive the credential, and a revoked
 * key whose audit rows vanished with it would be the one case an operator most
 * needs to read.
 *
 * There is deliberately no column anywhere here that could hold a plaintext
 * secret. `verifier` is an HMAC output and `salt` is its key; neither is
 * reversible, and no statement in `db_gw.c` ever writes the token. That is not
 * a convention — there is no column to write it to.
 */
static const char *const M12_STATEMENTS[] = {
    "CREATE TABLE api_keys ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    /* The selector, lowercase hex. Not a secret: it is the indexed lookup that
     * keeps verification O(1) and stops the cost of authenticating scaling with
     * how many credentials exist. */
    "  key_id TEXT NOT NULL UNIQUE,"
    /* Operator text, validated by atlas_apikey_label_valid before it arrives:
     * printable ASCII, no quote, backslash or percent, so what an operator
     * reads back is what they typed. */
    "  label TEXT NOT NULL,"
    /* The canonical space-separated scope list, in enum order. Stored as text
     * rather than as a mask so a future Atlas can add a scope without a
     * migration, and so an *older* Atlas reading a newer row fails closed on a
     * name it does not know rather than silently dropping the bit. */
    "  scopes TEXT NOT NULL,"
    /* The verifier and its salt. 16 and 32 bytes; a row of any other shape
     * matches nothing rather than everything, checked in atlas_apikey_verify. */
    "  salt BLOB NOT NULL,"
    "  verifier BLOB NOT NULL,"
    /* Names the construction, so a future phase that changes it can tell an old
     * row from a new one instead of verifying it under the wrong rule. */
    "  kdf TEXT NOT NULL CHECK(kdf IN ('HMAC-SHA256')),"
    /* UNKNOWN is deliberately absent from the CHECK: the schema will not hold a
     * key whose status Atlas cannot read, and the enum keeps UNKNOWN at zero so
     * a zeroed struct authorises nothing either. */
    "  status TEXT NOT NULL CHECK(status IN ('ACTIVE','REVOKED')),"
    "  created_at TEXT NOT NULL,"
    "  revoked_at TEXT NOT NULL DEFAULT '',"
    /* Written at most once per throttle interval rather than per request: a
     * timestamp that costs a write on every authenticated call would make the
     * daemon's single writer the gateway's bottleneck. It is evidence of use,
     * not an access log — that is what gw_audit is. */
    "  last_used_at TEXT NOT NULL DEFAULT '',"
    /* Rotation is create-then-revoke, recorded from both ends so the chain can
     * be read in either direction. Soft references by key_id text, never by
     * rowid: these must survive anything that renumbers rows. */
    "  rotated_from TEXT NOT NULL DEFAULT '',"
    "  rotated_to TEXT NOT NULL DEFAULT ''"
    ");",
    "CREATE INDEX idx_api_keys_status ON api_keys(status, id);",

    "CREATE TABLE gw_audit ("
    /* AUTOINCREMENT because this table is prunable and a reused id would
     * silently re-point any cursor a reader holds — A5's rule about repo_events,
     * and the reason `scans` is not prunable. */
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  at TEXT NOT NULL,"
    /* Which surface the request arrived on. A closed vocabulary: an audit trail
     * that cannot say whether something came from a model or a browser cannot
     * answer the question it exists for. */
    "  interface TEXT NOT NULL CHECK(interface IN ('REMOTE_MCP','WEB_API','WEB_GUI')),"
    /* The principal, by key id and label. Plain text and not a foreign key, so
     * the account of what a credential did outlives the credential. Empty when
     * the request never authenticated — which is itself the fact being
     * recorded. */
    "  key_id TEXT NOT NULL DEFAULT '',"
    "  label TEXT NOT NULL DEFAULT '',"
    /* What was asked for: an Atlas method or tool name, or the fixed name of the
     * gateway route. Never a URL, never a header, never a body. */
    "  operation TEXT NOT NULL,"
    /* Was it permitted, and did it then work? Two questions, two columns: a
     * request that was allowed and failed is a different event from one that
     * was denied, and folding them loses exactly the distinction an operator
     * reads this table for. */
    "  decision TEXT NOT NULL CHECK(decision IN ('ALLOWED','DENIED')),"
    "  outcome TEXT NOT NULL CHECK(outcome IN ('OK','FAILED','UNKNOWN')),"
    /* The Atlas status code, so the exit-code vocabulary is the same one here. */
    "  status INTEGER NOT NULL DEFAULT 0,"
    "  duration_ms INTEGER NOT NULL DEFAULT 0,"
    /* Fixed Atlas-owned text saying why a request was denied, or the safe-encoded
     * message of a failure. Never a secret, never an Authorization header, never
     * a request body, and safe-encoded before it is written so a crafted input
     * cannot forge a line in this table. */
    "  detail TEXT NOT NULL DEFAULT ''"
    ");",
    /* The two reads this table serves: most recent first, and one principal's
     * history. */
    "CREATE INDEX idx_gw_audit_at ON gw_audit(id DESC);",
    "CREATE INDEX idx_gw_audit_key ON gw_audit(key_id, id DESC);",
    NULL,
};

/* --- migration 13: knowledge kinds, and a lifecycle state for closure ------
 *
 * A9.1. Two changes to meaning, and no new table.
 *
 * 1. `decision_documents` gains `kind`: which sort of durable engineering
 *    knowledge this record is. A4 had one category and called it a decision, so
 *    a consensus constant, a release rule, a currently deployed chain id and an
 *    approach that was tried and abandoned all had to be written down as
 *    choices between alternatives. The prose survived that; the reason a later
 *    reader should treat them differently did not.
 *
 *    The column defaults to `DECISION` and every existing row takes that
 *    default, which is not a migration convenience but the definition: every
 *    record written before this vocabulary existed *was* a decision, so
 *    defaulting is what preserves its meaning rather than what approximates it.
 *
 *    It is on the document rather than on the revision, and therefore **not
 *    part of the canonical content hash**. Hashing it would move all 79 stored
 *    digests on the machine this was written for — 56 of them approved — and
 *    `atlas doctor` reports a moved digest as tampering, correctly. The kind is
 *    also identity-like rather than content: it is fixed before revision 1
 *    exists, no statement in `db_decision.c` names it in an UPDATE, and
 *    reclassifying is superseding with a document of the right kind, which keeps
 *    the record of how the knowledge used to be classified. See the
 *    field-by-field table in docs/decision-lifecycle.md.
 *
 * 2. The lifecycle vocabulary gains `RESOLVED`, so `decision_revisions.state`,
 *    `decision_documents.current_status` and `decision_events.event` widen, and
 *    `decision_challenges.intent` gains `resolve`. An approved OBLIGATION whose
 *    demand has been met is not superseded — nothing replaced it — and is not
 *    rejected, because it was accepted and was real. Without a fourth terminal
 *    state, closing one out meant either lying about a replacement or leaving a
 *    discharged obligation reported as outstanding for ever.
 *
 * **Four tables are rebuilt, because SQLite cannot widen a CHECK in place.**
 * That is the precedent migrations 7 and 9 set. What is new here is that three
 * of them are foreign-key parents, and one child — `decision_links` — declares
 * `ON DELETE CASCADE`, so a rebuild of `decision_revisions` with foreign keys
 * enforced would have deleted every link of every decision *silently*. This is
 * the migration that carries `foreign_keys_off`, and that cascade is the reason
 * the flag exists rather than a hypothetical one.
 *
 * Row ids are copied explicitly in every one of the four, column by column
 * rather than by `SELECT *`. `decision_events.challenge_id` points into
 * `decision_challenges` with no foreign key, `decision_validations.challenge_id`
 * likewise, and both would silently name somebody else's capability if a rebuild
 * renumbered. Every index is recreated by name; missing
 * `idx_decision_rev_current` — the partial unique index that is the *only*
 * enforcement of "at most one approved revision per document" — would delete
 * that guarantee without any statement failing.
 *
 * And the migration verifies itself before it commits: it records every
 * affected and every child table's row count first, then requires afterwards
 * that all nine counts are unchanged, that every document's id and uid and
 * every revision's id and content hash survived as pairs, that the events
 * sequence still covers the highest id, and that `foreign_key_check` is silent.
 * Any of those failing aborts the transaction, so the failure mode of the
 * riskiest statement in Atlas is a rollback rather than a quiet loss. */
static const char M13_SNAPSHOT[] =
    /* Taken before anything is touched. Both helpers are dropped at the end of
     * the migration, so a completed schema 13 contains neither. */
    "CREATE TABLE m13_counts(t TEXT PRIMARY KEY, n INTEGER NOT NULL);"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_documents', count(*) FROM decision_documents;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_revisions', count(*) FROM decision_revisions;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_events', count(*) FROM decision_events;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_challenges', count(*) FROM decision_challenges;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_alternatives', count(*)"
    "  FROM decision_alternatives;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_links', count(*) FROM decision_links;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_search', count(*) FROM decision_search;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_validations', count(*)"
    "  FROM decision_validations;"
    "INSERT INTO m13_counts(t, n) SELECT 'decision_edge_events', count(*)"
    "  FROM decision_edge_events;"
    "INSERT INTO m13_counts(t, n) SELECT 'events_max_id',"
    "  COALESCE((SELECT max(id) FROM decision_events), 0);"
    /* Identity pairs, so the check is not merely about how many rows there are.
     * A rebuild that preserved the count and renumbered the ids would pass a
     * count check and destroy every soft reference in the database. */
    "CREATE TABLE m13_docs(id INTEGER PRIMARY KEY, uid TEXT NOT NULL, status TEXT NOT NULL);"
    "INSERT INTO m13_docs(id, uid, status)"
    "  SELECT id, uid, current_status FROM decision_documents;"
    "CREATE TABLE m13_revs(id INTEGER PRIMARY KEY, content_hash TEXT NOT NULL, state TEXT NOT NULL);"
    "INSERT INTO m13_revs(id, content_hash, state)"
    "  SELECT id, content_hash, state FROM decision_revisions;";

static const char M13_DOCUMENTS[] =
    "CREATE TABLE decision_documents_new ("
    "  id INTEGER PRIMARY KEY,"
    "  uid TEXT NOT NULL UNIQUE,"
    "  repo_id INTEGER NOT NULL,"
    "  repo_root_hash TEXT NOT NULL,"
    "  repo_identity_hash TEXT NOT NULL DEFAULT '',"
    /* A9.1. Which sort of knowledge this record is, orthogonal to
     * `current_status`. The default is the definition of every pre-A9.1 row and
     * not a fallback: those records were decisions.
     *
     * Immutable. No UPDATE in `db_decision.c` names this column, which is the
     * same guarantee a revision's prose columns have. */
    "  kind TEXT NOT NULL DEFAULT 'DECISION' CHECK(kind IN"
    "    ('DECISION','POLICY','INVARIANT','OPERATIONAL_FACT','ACCEPTED_RISK','OBLIGATION',"
    "     'PARKED','REJECTED_ALTERNATIVE')),"
    "  created_at TEXT NOT NULL,"
    "  updated_at TEXT NOT NULL,"
    "  latest_revision_no INTEGER NOT NULL DEFAULT 0,"
    "  current_revision_id INTEGER,"
    "  current_status TEXT NOT NULL DEFAULT 'PROPOSED' CHECK(current_status IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED','RESOLVED')),"
    "  superseded_by_document_id INTEGER,"
    "  superseded_at TEXT"
    ");"
    "INSERT INTO decision_documents_new"
    "  (id, uid, repo_id, repo_root_hash, repo_identity_hash, kind, created_at, updated_at,"
    "   latest_revision_no, current_revision_id, current_status, superseded_by_document_id,"
    "   superseded_at)"
    "  SELECT id, uid, repo_id, repo_root_hash, repo_identity_hash, 'DECISION', created_at,"
    "         updated_at, latest_revision_no, current_revision_id, current_status,"
    "         superseded_by_document_id, superseded_at"
    "  FROM decision_documents;"
    "DROP TABLE decision_documents;"
    "ALTER TABLE decision_documents_new RENAME TO decision_documents;"
    /* All four exactly as migration 6 created them, plus one. */
    "CREATE INDEX idx_decision_docs_repo ON decision_documents(repo_id, id DESC);"
    "CREATE INDEX idx_decision_docs_status ON decision_documents(repo_id, current_status, id DESC);"
    "CREATE INDEX idx_decision_docs_root ON decision_documents(repo_root_hash);"
    "CREATE INDEX idx_decision_docs_identity ON decision_documents(repo_identity_hash)"
    "  WHERE repo_identity_hash <> '';"
    /* The new read A9.1 adds: one repository's records of one kind. Status and
     * kind get an index each rather than one composite, because either filter
     * is used alone as often as both are used together and SQLite will pick the
     * more selective one. */
    "CREATE INDEX idx_decision_docs_kind ON decision_documents(repo_id, kind, id DESC);";

static const char M13_REVISIONS[] =
    "CREATE TABLE decision_revisions_new ("
    "  id INTEGER PRIMARY KEY,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  title TEXT NOT NULL,"
    "  context_text TEXT NOT NULL DEFAULT '',"
    "  decision_text TEXT NOT NULL DEFAULT '',"
    "  rationale_text TEXT NOT NULL DEFAULT '',"
    "  consequences_text TEXT NOT NULL DEFAULT '',"
    "  scope TEXT NOT NULL DEFAULT 'UNKNOWN' CHECK(scope IN"
    "    ('UNKNOWN','REPOSITORY','SUBSYSTEM','PATHS')),"
    "  proposed_by TEXT NOT NULL CHECK(proposed_by IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC')),"
    "  session_id INTEGER,"
    "  session_unbound INTEGER NOT NULL DEFAULT 0,"
    "  unbound_reason TEXT,"
    "  basis_head TEXT,"
    "  basis_repo_identity_hash TEXT NOT NULL DEFAULT '',"
    "  created_at TEXT NOT NULL,"
    /* RESOLVED joins the vocabulary. A revision that is resolved was approved,
     * is no longer effective, and was not replaced — see atlas/decision.h. */
    "  state TEXT NOT NULL DEFAULT 'PROPOSED' CHECK(state IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED','RESOLVED')),"
    "  imported_from_ai_decision_id INTEGER,"
    "  dedup_key TEXT,"
    "  UNIQUE(document_id, revision_no)"
    ");"
    "INSERT INTO decision_revisions_new"
    "  (id, document_id, revision_no, content_hash, title, context_text, decision_text,"
    "   rationale_text, consequences_text, scope, proposed_by, session_id, session_unbound,"
    "   unbound_reason, basis_head, basis_repo_identity_hash, created_at, state,"
    "   imported_from_ai_decision_id, dedup_key)"
    "  SELECT id, document_id, revision_no, content_hash, title, context_text, decision_text,"
    "         rationale_text, consequences_text, scope, proposed_by, session_id, session_unbound,"
    "         unbound_reason, basis_head, basis_repo_identity_hash, created_at, state,"
    "         imported_from_ai_decision_id, dedup_key"
    "  FROM decision_revisions;"
    "DROP TABLE decision_revisions;"
    "ALTER TABLE decision_revisions_new RENAME TO decision_revisions;"
    "CREATE INDEX idx_decision_rev_doc ON decision_revisions(document_id, revision_no DESC);"
    /* Rule 9 of A4, and the one index whose absence would be invisible: it is
     * the sole enforcement of at most one approved revision per document. */
    "CREATE UNIQUE INDEX idx_decision_rev_current ON decision_revisions(document_id)"
    "  WHERE state = 'APPROVED';"
    "CREATE UNIQUE INDEX idx_decision_rev_import ON decision_revisions"
    "  (imported_from_ai_decision_id)"
    "  WHERE imported_from_ai_decision_id IS NOT NULL;"
    "CREATE UNIQUE INDEX idx_decision_rev_dedup ON decision_revisions(document_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

static const char M13_EVENTS[] =
    /* AUTOINCREMENT is recreated as AUTOINCREMENT: A8's ordering rule is that
     * the ledger's id orders it, and a reused id would let a later event sort
     * before an earlier one. Copying the ids explicitly sets `sqlite_sequence`
     * to the highest of them, which the verification below requires. */
    "CREATE TABLE decision_events_new ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL DEFAULT 0,"
    "  event TEXT NOT NULL CHECK(event IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED','RESOLVED')),"
    "  actor TEXT NOT NULL CHECK(actor IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC')),"
    "  content_hash TEXT,"
    "  challenge_id INTEGER,"
    "  superseded_by_revision_id INTEGER,"
    "  superseded_by_document_id INTEGER,"
    "  detail TEXT,"
    "  created_at TEXT NOT NULL,"
    "  dedup_key TEXT"
    ");"
    "INSERT INTO decision_events_new"
    "  (id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
    "   superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key)"
    "  SELECT id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
    "         superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key"
    "  FROM decision_events;"
    "DROP TABLE decision_events;"
    "ALTER TABLE decision_events_new RENAME TO decision_events;"
    "CREATE INDEX idx_decision_events_doc ON decision_events(document_id, id);"
    "CREATE INDEX idx_decision_events_rev ON decision_events(revision_id, id);"
    "CREATE UNIQUE INDEX idx_decision_events_dedup ON decision_events(document_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

static const char M13_CHALLENGES[] =
    "CREATE TABLE decision_challenges_new ("
    "  id INTEGER PRIMARY KEY,"
    "  token TEXT NOT NULL UNIQUE,"
    "  repo_id INTEGER NOT NULL,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    /* `resolve` joins the intents. A fourth intent on one capability rather than
     * a second mechanism, for the reason `revalidate` was: it needs exactly the
     * properties approval needed and no others. */
    "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede','revalidate',"
    "    'resolve')),"
    "  supersede_document_id INTEGER,"
    "  indexed_commit TEXT,"
    "  evidence_digest TEXT,"
    "  prior_freshness TEXT CHECK(prior_freshness IS NULL OR prior_freshness IN"
    "    ('FRESH','STALE','IMPACTED','UNKNOWN')),"
    "  prior_reasons TEXT,"
    "  created_at TEXT NOT NULL,"
    "  expires_at TEXT NOT NULL,"
    "  consumed INTEGER NOT NULL DEFAULT 0,"
    "  consumed_at TEXT"
    ");"
    "INSERT INTO decision_challenges_new"
    "  (id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "   supersede_document_id, indexed_commit, evidence_digest, prior_freshness, prior_reasons,"
    "   created_at, expires_at, consumed, consumed_at)"
    "  SELECT id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "         supersede_document_id, indexed_commit, evidence_digest, prior_freshness,"
    "         prior_reasons, created_at, expires_at, consumed, consumed_at"
    "  FROM decision_challenges;"
    "DROP TABLE decision_challenges;"
    "ALTER TABLE decision_challenges_new RENAME TO decision_challenges;"
    "CREATE INDEX idx_decision_challenges_repo ON decision_challenges"
    "  (repo_id, consumed, expires_at);";

/* The migration's own acceptance test, run inside its own transaction.
 *
 * The named CHECK is the error message: a failure reports
 * `no_decision_row_may_be_lost_in_migration_13`, the runner wraps it as
 * "migration 13 ... failed and was rolled back", and nothing is written. */
static const char M13_VERIFY[] =
    "CREATE TABLE m13_verify(ok INTEGER NOT NULL,"
    "  CONSTRAINT no_decision_row_may_be_lost_in_migration_13 CHECK(ok = 1));"
    "INSERT INTO m13_verify(ok) SELECT CASE WHEN"
    "     (SELECT n FROM m13_counts WHERE t='decision_documents')"
    "       = (SELECT count(*) FROM decision_documents)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_revisions')"
    "       = (SELECT count(*) FROM decision_revisions)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_events')"
    "       = (SELECT count(*) FROM decision_events)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_challenges')"
    "       = (SELECT count(*) FROM decision_challenges)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_alternatives')"
    "       = (SELECT count(*) FROM decision_alternatives)"
    /* The cascade this whole flag exists for. */
    " AND (SELECT n FROM m13_counts WHERE t='decision_links')"
    "       = (SELECT count(*) FROM decision_links)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_search')"
    "       = (SELECT count(*) FROM decision_search)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_validations')"
    "       = (SELECT count(*) FROM decision_validations)"
    " AND (SELECT n FROM m13_counts WHERE t='decision_edge_events')"
    "       = (SELECT count(*) FROM decision_edge_events)"
    /* Identity, not just arithmetic: every id/uid and id/content_hash pair
     * survived as a pair, and every status and state came across unchanged. */
    " AND (SELECT count(*) FROM m13_docs h JOIN decision_documents d"
    "        ON d.id = h.id AND d.uid = h.uid AND d.current_status = h.status)"
    "       = (SELECT n FROM m13_counts WHERE t='decision_documents')"
    " AND (SELECT count(*) FROM m13_revs h JOIN decision_revisions r"
    "        ON r.id = h.id AND r.content_hash = h.content_hash AND r.state = h.state)"
    "       = (SELECT n FROM m13_counts WHERE t='decision_revisions')"
    /* Every migrated document is a DECISION, which is what backward
     * compatibility means here rather than what it approximates. */
    " AND (SELECT count(*) FROM decision_documents WHERE kind <> 'DECISION') = 0"
    /* The ledger's AUTOINCREMENT sequence still covers every id it issued, so no
     * future event can take an id an existing one already has. */
    " AND COALESCE((SELECT seq FROM sqlite_sequence WHERE name='decision_events'), 0)"
    "       >= (SELECT n FROM m13_counts WHERE t='events_max_id')"
    /* And nothing dangles. Foreign keys were off for the rebuild; this is the
     * check that says the schema is consistent again before the commit that
     * makes it visible. */
    " AND (SELECT count(*) FROM pragma_foreign_key_check) = 0"
    "  THEN 1 ELSE 0 END;"
    "DROP TABLE m13_verify;"
    "DROP TABLE m13_counts;"
    "DROP TABLE m13_docs;"
    "DROP TABLE m13_revs;";

static const char *const M13_STATEMENTS[] = {
    M13_SNAPSHOT, M13_DOCUMENTS, M13_REVISIONS, M13_EVENTS, M13_CHALLENGES, M13_VERIFY, NULL,
};

/* `foreign_keys_off` is written out for every row rather than left to default,
 * so that "which migrations run with foreign keys enforced?" is answered by
 * reading this table instead of by counting initialisers. */
static const atlas_migration MIGRATIONS[] = {
    {1, "initial schema", M1_STATEMENTS, false},
    {2, "worktree identity", M2_STATEMENTS, false},
    {3, "continuous indexing state", M3_STATEMENTS, false},
    {4, "AI sessions, change reasons and decisions", M4_STATEMENTS, false},
    {5, "structural code graph", M5_STATEMENTS, false},
    {6, "decision documents, revisions and operator approval", M6_STATEMENTS, false},
    {7, "decision revalidation records", M7_STATEMENTS, false},
    {8, "durable orchestration control plane", M8_STATEMENTS, false},
    {9, "a general decision-to-decision relation", M9_STATEMENTS, false},
    {10, "durable evidence about a decision-to-decision edge", M10_STATEMENTS, false},
    {11, "compiler-derived semantic index", M11_STATEMENTS, false},
    {12, "remote API credentials and the gateway audit trail", M12_STATEMENTS, false},
    /* The one migration that rebuilds foreign-key parents. See
     * `atlas_migration.foreign_keys_off` and the migration 13 comment: with
     * foreign keys enforced, `decision_links`' declared cascade would have made
     * the rebuild of `decision_revisions` delete every link silently. */
    {13, "knowledge kinds and a lifecycle state for closure", M13_STATEMENTS, true},
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

    /* A database from the future is refused, never used. The loop below only
     * ever *adds* migrations, so without this check a database at a version
     * this build has never heard of would fall straight through it and be
     * reported as migrated — and then written to under constraints, tables and
     * meanings this binary does not know. An older Atlas silently writing into
     * a newer schema is how a rebuildable index becomes an unrebuildable one.
     * The read-only path in `atlas_db_migrate` already refuses; this is the
     * same refusal on the path that can actually do the damage. */
    if (count > 0 && current > list[count - 1].version) {
        return atlas_err_set(err, ATLAS_ERR_DB,
                             "database schema is at version %d but this Atlas understands at "
                             "most %d. Refusing to open a database written by a newer Atlas.",
                             current, list[count - 1].version);
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

        /* Before `BEGIN`, because `PRAGMA foreign_keys` is a no-op inside a
         * transaction and would silently do nothing here. See
         * `atlas_migration.foreign_keys_off` for why any migration needs it:
         * rebuilding a table that a child references with `ON DELETE CASCADE`
         * would otherwise empty the child without failing. The migration itself
         * is still one transaction, and it checks its own row preservation and
         * `foreign_key_check` before that transaction commits. */
        if (m->foreign_keys_off) {
            st = atlas_db_exec_sql(db, "PRAGMA foreign_keys=OFF;", err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            for (size_t k = 0; st == ATLAS_OK && m->statements != NULL && m->statements[k] != NULL;
                 k++) {
                st = atlas_db_exec_sql(db, m->statements[k], err);
            }
            if (st == ATLAS_OK) {
                st = record_migration(db, m, err);
            }
            if (st != ATLAS_OK) {
                atlas_db_rollback(db);
            } else {
                st = atlas_db_commit(db, err);
                if (st != ATLAS_OK) {
                    atlas_db_rollback(db);
                }
            }
        }
        /* Restored on every exit path, including the failing ones: a connection
         * left with foreign keys off is a connection whose next write is
         * unchecked, and this one goes on to serve the process. */
        if (m->foreign_keys_off) {
            atlas_err restore;
            atlas_err_init(&restore);
            atlas_status rst = atlas_db_exec_sql(db, "PRAGMA foreign_keys=ON;", &restore);
            if (rst != ATLAS_OK && st == ATLAS_OK) {
                return atlas_err_set(err, ATLAS_ERR_DB,
                                     "migration %d (%s) applied but foreign key enforcement could "
                                     "not be restored: %s",
                                     m->version, m->name != NULL ? m->name : "unnamed",
                                     atlas_err_msg(&restore));
            }
        }
        if (st != ATLAS_OK) {
            /* Preserve the sqlite message but make the failing migration clear. */
            char detail[ATLAS_ERR_MSG_MAX];
            (void)snprintf(detail, sizeof(detail), "%s", atlas_err_msg(err));
            return atlas_err_set(err, ATLAS_ERR_DB,
                                 "migration %d (%s) failed and was rolled back: %s", m->version,
                                 m->name != NULL ? m->name : "unnamed", detail);
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
        "  tokenize='unicode61 remove_diacritics 0');"
        /* A4. Here rather than in migration 6 for the reason stated at the top
         * of this file: FTS5 availability is a property of the linked SQLite
         * build, not of the schema, so a numbered migration that assumed it
         * would refuse to apply on a build without it. Decision search degrades
         * to a bounded, repository-filtered scan of `decision_search` when this
         * is absent, and `atlas doctor` reports which one is in use. */
        "CREATE VIRTUAL TABLE IF NOT EXISTS decisions_fts USING fts5("
        "  haystack, content='decision_search', content_rowid='revision_id',"
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
                             "INSERT INTO commits_fts(commits_fts) VALUES('rebuild');"
                             "INSERT INTO decisions_fts(decisions_fts) VALUES('rebuild');",
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

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

static const char *const M1_STATEMENTS[] = {
    M1_BOOKKEEPING, M1_REPOSITORIES,       M1_SCANS,    M1_FILES,
    M1_COMMITS,     M1_FILE_CHANGES,       M1_COMPILE_DATABASES, M1_EVIDENCE,
    NULL,
};

static const atlas_migration MIGRATIONS[] = {
    {1, "initial schema", M1_STATEMENTS},
    {2, "worktree identity", M2_STATEMENTS},
    {3, "continuous indexing state", M3_STATEMENTS},
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
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind migration version");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, m->name, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
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
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, path_text != NULL ? path_text : "", err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
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
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, new_path_text != NULL ? new_path_text : "", err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
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
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind commit id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, subject != NULL ? subject : "", err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 3, body != NULL ? body : "", body != NULL ? body_len : 0,
                                  err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

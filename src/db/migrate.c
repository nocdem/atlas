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

static const char *const M1_STATEMENTS[] = {
    M1_BOOKKEEPING, M1_REPOSITORIES,       M1_SCANS,    M1_FILES,
    M1_COMMITS,     M1_FILE_CHANGES,       M1_COMPILE_DATABASES, M1_EVIDENCE,
    NULL,
};

static const atlas_migration MIGRATIONS[] = {
    {1, "initial schema", M1_STATEMENTS},
    {2, "worktree identity", M2_STATEMENTS},
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

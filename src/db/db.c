/* Atlas - database lifecycle, capabilities and helpers.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "db/db_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"

/* --- error plumbing ------------------------------------------------------ */

atlas_status atlas_db_fail(atlas_db *db, atlas_err *err, atlas_status st, const char *what) {
    const char *msg = (db != NULL && db->h != NULL) ? sqlite3_errmsg(db->h) : "no database handle";
    return atlas_err_set(err, st, "%s: %s", what, msg);
}

atlas_status atlas_db_exec_sql(atlas_db *db, const char *sql, atlas_err *err) {
    char *emsg = NULL;
    if (sqlite3_exec(db->h, sql, NULL, NULL, &emsg) != SQLITE_OK) {
        atlas_status st = atlas_err_set(err, ATLAS_ERR_DB, "sql failed: %s",
                                        emsg != NULL ? emsg : sqlite3_errmsg(db->h));
        sqlite3_free(emsg);
        return st;
    }
    sqlite3_free(emsg);
    return ATLAS_OK;
}

/* --- the prepared-statement cache -----------------------------------------
 *
 * Every query in Atlas is a string literal, and preparing one is not free: the
 * planner runs each time, which for the more involved statements costs a
 * hundred microseconds or so. A0 and A1 did one or two statements per file and
 * never noticed. A3's structural pass does a few hundred — a symbol, an
 * occurrence and several relations per file, then a resolution per edge — and
 * preparing them all afresh was, measured, the whole cost of indexing a large
 * repository.
 *
 * The cache is keyed on the **SQL pointer**, not on its contents. Every call
 * site passes a string literal with static storage duration, so the pointer is
 * a stable identity and the lookup is a pointer compare rather than a hash of a
 * kilobyte of SQL. A caller that ever passes a constructed string simply misses
 * the cache and prepares as before; nothing breaks, it is only slower.
 *
 * Three properties make it safe:
 *
 *   - **One cache per handle, and a handle belongs to one thread.** The writer
 *     owns the only writable handle and each reader opens its own, so there is
 *     no sharing to synchronise. That is the same rule the rest of the daemon
 *     already keeps, not a new one.
 *   - **Re-entrancy falls back.** A cached statement that is already stepping —
 *     a query issued from inside another query's row loop — is marked in use,
 *     and the second caller gets a freshly prepared one. Handing out the same
 *     statement twice would reset an iteration mid-flight.
 *   - **Release is explicit.** `atlas_db_finish` returns a statement to the
 *     cache or finalises it, and every call site in src/db calls it instead of
 *     `sqlite3_finalize`. */

atlas_status atlas_db_prepare(atlas_db *db, const char *sql, sqlite3_stmt **out, atlas_err *err) {
    *out = NULL;
    for (size_t i = 0; i < db->stmt_cache_count; i++) {
        if (db->stmt_cache[i].sql == sql && !db->stmt_cache[i].in_use) {
            db->stmt_cache[i].in_use = true;
            /* Reset rather than re-prepare. The reset's return code is the
             * previous step's error, which the previous caller already saw, so
             * it is deliberately ignored here. */
            (void)sqlite3_reset(db->stmt_cache[i].stmt);
            (void)sqlite3_clear_bindings(db->stmt_cache[i].stmt);
            *out = db->stmt_cache[i].stmt;
            return ATLAS_OK;
        }
    }
    if (sqlite3_prepare_v2(db->h, sql, -1, out, NULL) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot prepare statement");
    }
    if (db->stmt_cache_count < ATLAS_DB_STMT_CACHE) {
        db->stmt_cache[db->stmt_cache_count].sql = sql;
        db->stmt_cache[db->stmt_cache_count].stmt = *out;
        db->stmt_cache[db->stmt_cache_count].in_use = true;
        db->stmt_cache_count++;
    }
    return ATLAS_OK;
}

void atlas_db_finish(atlas_db *db, sqlite3_stmt *stmt) {
    if (stmt == NULL) {
        return;
    }
    for (size_t i = 0; i < db->stmt_cache_count; i++) {
        if (db->stmt_cache[i].stmt == stmt) {
            db->stmt_cache[i].in_use = false;
            /* Reset now rather than at the next use, so a statement sitting in
             * the cache holds no read lock and pins nothing. */
            (void)sqlite3_reset(stmt);
            (void)sqlite3_clear_bindings(stmt);
            return;
        }
    }
    /* Not cached — prepared directly, or prepared while a cached copy was in
     * use. It belongs to the caller and is destroyed here. */
    sqlite3_finalize(stmt);
}

void atlas_db_cache_clear(atlas_db *db) {
    for (size_t i = 0; i < db->stmt_cache_count; i++) {
        sqlite3_finalize(db->stmt_cache[i].stmt);
    }
    db->stmt_cache_count = 0;
}

atlas_status atlas_db_step_done(atlas_db *db, sqlite3_stmt *stmt, atlas_err *err) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        atlas_status st = atlas_db_fail(db, err, ATLAS_ERR_DB, "statement failed");
        atlas_db_finish(db, stmt);
        return st;
    }
    atlas_db_finish(db, stmt);
    return ATLAS_OK;
}

atlas_status atlas_db_query_int64(atlas_db *db, const char *sql, int64_t *out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    *out = 0;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "query failed");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* sqlite takes lengths as int; refuse anything that would not survive the cast. */
static atlas_status check_len(size_t n, atlas_err *err) {
    if (n > (size_t)0x7fffffff) {
        return atlas_err_set(err, ATLAS_ERR_DB, "value of %zu bytes is too large to bind", n);
    }
    return ATLAS_OK;
}

atlas_status atlas_db_bind_blob(atlas_db *db, sqlite3_stmt *stmt, int idx, const void *data,
                                size_t n, atlas_err *err) {
    atlas_status st = check_len(n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* SQLITE_TRANSIENT: sqlite copies, so callers keep ownership of the bytes. */
    if (sqlite3_bind_blob(stmt, idx, data, (int)n, SQLITE_TRANSIENT) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind blob");
    }
    return ATLAS_OK;
}

atlas_status atlas_db_bind_text_n(atlas_db *db, sqlite3_stmt *stmt, int idx, const char *s,
                                  size_t n, atlas_err *err) {
    atlas_status st = check_len(n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_text(stmt, idx, s, (int)n, SQLITE_TRANSIENT) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind text");
    }
    return ATLAS_OK;
}

atlas_status atlas_db_bind_text_opt(atlas_db *db, sqlite3_stmt *stmt, int idx, const char *s,
                                   atlas_err *err) {
    if (s == NULL) {
        if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind null");
        }
        return ATLAS_OK;
    }
    return atlas_db_bind_text_n(db, stmt, idx, s, strlen(s), err);
}

const char *atlas_db_col_text(sqlite3_stmt *stmt, int col) {
    const unsigned char *p = sqlite3_column_text(stmt, col);
    return (p != NULL) ? (const char *)p : "";
}

const char *atlas_db_col_text_opt(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return NULL;
    }
    const unsigned char *p = sqlite3_column_text(stmt, col);
    return (p != NULL) ? (const char *)p : NULL;
}

atlas_status atlas_db_col_copy(sqlite3_stmt *stmt, int col, char *dst, size_t dst_size,
                               const char *field, atlas_err *err) {
    const char *s = atlas_db_col_text(stmt, col);
    size_t n = strlen(s);
    if (n + 1u > dst_size) {
        return atlas_err_set(err, ATLAS_ERR_DB, "stored %s is %zu bytes, exceeding the %zu limit",
                             field, n, dst_size - 1u);
    }
    memcpy(dst, s, n + 1u);
    return ATLAS_OK;
}

/* --- lifecycle ---------------------------------------------------------- */

static void detect_caps(atlas_db *db) {
    (void)snprintf(db->caps.sqlite_version, sizeof(db->caps.sqlite_version), "%s",
                   sqlite3_libversion());

    /* FTS5 presence is probed, never assumed from compile options. */
    sqlite3_stmt *stmt = NULL;
    db->caps.fts5 = false;
    char *emsg = NULL;
    if (sqlite3_exec(db->h, "CREATE VIRTUAL TABLE temp.atlas_fts5_probe USING fts5(x);", NULL, NULL,
                     &emsg) == SQLITE_OK) {
        db->caps.fts5 = true;
        (void)sqlite3_exec(db->h, "DROP TABLE temp.atlas_fts5_probe;", NULL, NULL, NULL);
    }
    sqlite3_free(emsg);

    db->caps.journal_mode[0] = '\0';
    if (sqlite3_prepare_v2(db->h, "PRAGMA journal_mode;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *m = sqlite3_column_text(stmt, 0);
            if (m != NULL) {
                (void)snprintf(db->caps.journal_mode, sizeof(db->caps.journal_mode), "%s",
                               (const char *)m);
            }
        }
        atlas_db_finish(db, stmt);
    }
    db->caps.wal = (strcmp(db->caps.journal_mode, "wal") == 0);

    db->caps.foreign_keys = false;
    stmt = NULL;
    if (sqlite3_prepare_v2(db->h, "PRAGMA foreign_keys;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            db->caps.foreign_keys = sqlite3_column_int(stmt, 0) != 0;
        }
        atlas_db_finish(db, stmt);
    }
}

/* Keep the database and its sidecars private to the owning user. */
static void restrict_perms(const char *path) {
    char side[4096];
    (void)chmod(path, S_IRUSR | S_IWUSR);
    if (snprintf(side, sizeof(side), "%s-wal", path) < (int)sizeof(side)) {
        (void)chmod(side, S_IRUSR | S_IWUSR);
    }
    if (snprintf(side, sizeof(side), "%s-shm", path) < (int)sizeof(side)) {
        (void)chmod(side, S_IRUSR | S_IWUSR);
    }
}

atlas_status atlas_db_open(const char *path, atlas_db **out, atlas_err *err) {
    *out = NULL;
    if (path == NULL || path[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "empty database path");
    }

    atlas_db *db = calloc(1u, sizeof(*db));
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory opening database");
    }

    /* Create the file ourselves so it is never briefly world-readable. */
    if (strcmp(path, ":memory:") != 0) {
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            atlas_status st =
                atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot open database %s", path);
            free(db);
            return st;
        }
        (void)close(fd);
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(path, &db->h, flags, NULL) != SQLITE_OK) {
        atlas_status st = atlas_err_set(err, ATLAS_ERR_DB, "cannot open database %s: %s", path,
                                        db->h != NULL ? sqlite3_errmsg(db->h) : "unknown error");
        sqlite3_close(db->h);
        free(db);
        return st;
    }

    (void)sqlite3_busy_timeout(db->h, 5000);
    atlas_status st = atlas_db_exec_sql(db, "PRAGMA foreign_keys=ON;", err);
    if (st == ATLAS_OK) {
        /* WAL is a preference, not a requirement: some filesystems refuse it and
         * `atlas doctor` reports whatever mode is actually in force. */
        char *emsg = NULL;
        (void)sqlite3_exec(db->h, "PRAGMA journal_mode=WAL;", NULL, NULL, &emsg);
        sqlite3_free(emsg);
        st = atlas_db_exec_sql(db, "PRAGMA synchronous=NORMAL;", err);
    }
    if (st == ATLAS_OK) {
        /* SQLite's default page cache is two thousand pages — about two
         * megabytes. That is ample for an index of file paths and commits, and
         * it is nothing at all for a structural graph: A3 writes hundreds of
         * thousands of rows across half a dozen indexes, and at two megabytes
         * every B-tree descent goes back to the filesystem. Measured on the
         * five-thousand-file acceptance fixture, that alone was minutes.
         *
         * Negative means kibibytes rather than pages, so the ceiling does not
         * change with the page size. Sixty-four mebibytes is a small fraction of
         * the documented memory budget and is bounded: it is a cache, so it is
         * an upper bound rather than a commitment. */
        st = atlas_db_exec_sql(db, "PRAGMA cache_size=-65536;", err);
    }
    if (st == ATLAS_OK) {
        /* Every `INSERT ... RETURNING id` in Atlas makes SQLite open an
         * ephemeral table to hold the returned row, and with the default
         * `temp_store` an ephemeral table is a file: created, written and
         * unlinked once per inserted row. Sampling the structural pass on the
         * acceptance fixture found the writer inside `pwrite` on that temporary
         * file in almost every sample — it was the single largest cost of
         * building the graph, and none of it was the graph.
         *
         * In memory instead. What goes there is bounded by what Atlas asks for:
         * one row per RETURNING, and sorters for queries that are all limited.
         * Nothing here streams an unbounded result set through a sorter, which
         * is the one case where this pragma would trade disk for unbounded
         * memory. */
        st = atlas_db_exec_sql(db, "PRAGMA temp_store=MEMORY;", err);
    }
    if (st != ATLAS_OK) {
        sqlite3_close(db->h);
        free(db);
        return st;
    }

    detect_caps(db);
    if (strcmp(path, ":memory:") != 0) {
        restrict_perms(path);
    }
    *out = db;
    return ATLAS_OK;
}

/* A read-only handle for concurrent readers.
 *
 * WAL lets any number of readers run alongside the single writer, but only if a
 * reader genuinely never writes. Opening SQLITE_OPEN_READONLY makes that a
 * property of the handle rather than a promise about the code: a stray INSERT
 * fails immediately instead of contending for the write lock, or worse,
 * succeeding from a process that is supposed to have no write authority.
 *
 * The database must already exist and already be migrated: a reader must never
 * be the thing that creates or upgrades it. */
atlas_status atlas_db_open_readonly(const char *path, atlas_db **out, atlas_err *err) {
    *out = NULL;
    if (path == NULL || path[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "empty database path");
    }
    atlas_db *db = calloc(1u, sizeof(*db));
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory opening database");
    }
    db->read_only = true;
    if (sqlite3_open_v2(path, &db->h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        atlas_status st = atlas_err_set(err, ATLAS_ERR_DB, "cannot open database %s read-only: %s",
                                        path, db->h != NULL ? sqlite3_errmsg(db->h) : "unknown");
        sqlite3_close(db->h);
        free(db);
        return st;
    }
    (void)sqlite3_busy_timeout(db->h, 5000);
    /* foreign_keys is a no-op on a read-only handle but keeps the two open paths
     * reporting the same capabilities to `atlas doctor`. */
    (void)sqlite3_exec(db->h, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    detect_caps(db);
    /* FTS5 availability was probed on a temp table; whether the shadow tables
     * exist in *this* database still has to be established, and a reader may not
     * create them. */
    int64_t have = 0;
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (atlas_db_query_int64(db,
                             "SELECT count(*) FROM sqlite_schema WHERE type='table'"
                             " AND name IN ('files_fts','commits_fts');",
                             &have, &ignore) == ATLAS_OK) {
        db->fts_ready = db->caps.fts5 && have == 2;
    }
    *out = db;
    return ATLAS_OK;
}

bool atlas_db_is_readonly(const atlas_db *db) {
    return db != NULL && db->read_only;
}

void atlas_db_close(atlas_db *db) {
    if (db == NULL) {
        return;
    }
    if (db->tx_depth > 0) {
        (void)sqlite3_exec(db->h, "ROLLBACK;", NULL, NULL, NULL);
        db->tx_depth = 0;
    }
    /* Before the handle goes: sqlite3_close refuses to close a connection with
     * an unfinalised statement, and every cached one is exactly that. */
    atlas_db_cache_clear(db);
    (void)sqlite3_close(db->h);
    free(db);
}

const atlas_db_caps *atlas_db_caps_of(const atlas_db *db) {
    return &db->caps;
}

void atlas_db_disable_fts_for_tests(atlas_db *db) {
    db->caps.fts5 = false;
    db->fts_ready = false;
}

int atlas_db_schema_version(atlas_db *db, atlas_err *err) {
    int64_t exists = 0;
    atlas_status st = atlas_db_query_int64(
        db, "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='schema_migrations';",
        &exists, err);
    if (st != ATLAS_OK) {
        return -1;
    }
    if (exists == 0) {
        return 0;
    }
    int64_t v = 0;
    st = atlas_db_query_int64(db, "SELECT coalesce(max(version),0) FROM schema_migrations;", &v,
                              err);
    if (st != ATLAS_OK) {
        return -1;
    }
    return (int)v;
}

atlas_status atlas_db_begin(atlas_db *db, atlas_err *err) {
    if (db->tx_depth > 0) {
        db->tx_depth++;
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_exec_sql(db, "BEGIN IMMEDIATE;", err);
    if (st == ATLAS_OK) {
        db->tx_depth = 1;
    }
    return st;
}

atlas_status atlas_db_commit(atlas_db *db, atlas_err *err) {
    if (db->tx_depth == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "commit without an open transaction");
    }
    if (db->tx_depth > 1) {
        db->tx_depth--;
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_exec_sql(db, "COMMIT;", err);
    if (st == ATLAS_OK) {
        db->tx_depth = 0;
    }
    return st;
}

void atlas_db_rollback(atlas_db *db) {
    if (db->tx_depth == 0) {
        return;
    }
    (void)sqlite3_exec(db->h, "ROLLBACK;", NULL, NULL, NULL);
    db->tx_depth = 0;
}

/* --- checks ------------------------------------------------------------- */

static atlas_status pragma_collect(atlas_db *db, const char *sql, const char *ok_text,
                                   atlas_buf *out, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_buf_reset(out);
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int rows = 0;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "pragma failed");
            break;
        }
        if (rows >= 20) { /* bounded: a corrupt file can report thousands */
            st = atlas_buf_append_str(out, "\n(further rows omitted)", err);
            break;
        }
        if (rows > 0) {
            st = atlas_buf_append_ch(out, '\n', err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        int ncol = sqlite3_column_count(stmt);
        for (int c = 0; c < ncol; c++) {
            if (c > 0) {
                st = atlas_buf_append_ch(out, ' ', err);
                if (st != ATLAS_OK) {
                    break;
                }
            }
            st = atlas_buf_append_str(out, atlas_db_col_text(stmt, c), err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        if (st != ATLAS_OK) {
            break;
        }
        rows++;
    }
    atlas_db_finish(db, stmt);
    if (st != ATLAS_OK) {
        return st;
    }
    if (rows == 0 && ok_text != NULL) {
        return atlas_buf_append_str(out, ok_text, err);
    }
    return ATLAS_OK;
}

atlas_status atlas_db_integrity_check(atlas_db *db, atlas_buf *out, atlas_err *err) {
    /* integrity_check reports the single row "ok" for a healthy database. */
    return pragma_collect(db, "PRAGMA integrity_check;", "ok", out, err);
}

atlas_status atlas_db_foreign_key_check(atlas_db *db, atlas_buf *out, atlas_err *err) {
    return pragma_collect(db, "PRAGMA foreign_key_check;", "ok", out, err);
}

const char *atlas_evidence_kind_name(atlas_evidence_kind k) {
    switch (k) {
    case ATLAS_EV_SOURCE: return "SOURCE";
    case ATLAS_EV_GIT: return "GIT";
    case ATLAS_EV_DECISION: return "DECISION";
    case ATLAS_EV_USER_STATEMENT: return "USER_STATEMENT";
    case ATLAS_EV_INFERENCE: return "INFERENCE";
    case ATLAS_EV_UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char *atlas_search_mode_name(atlas_search_mode m) {
    switch (m) {
    case ATLAS_SEARCH_FTS5: return "fts5";
    case ATLAS_SEARCH_DEGRADED_LIKE: return "degraded-like";
    }
    return "unknown";
}

/* Atlas - the SQLite half of `atlas maintenance`.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Table names reaching this file come from the compiled-in RETENTION[] array in
 * src/core/service_maintenance.c and from nowhere else. They are formatted into
 * SQL because a table name cannot be a bound parameter; the statements
 * therefore miss the prepared-statement cache and are prepared afresh, which is
 * what the cache's header says happens to any SQL that is not a string literal.
 * Nothing repository-controlled or model-controlled can reach these strings.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/maintenance.h"
#include "db_internal.h"

atlas_status atlas_db_maintenance_table_exists(atlas_db *db, const char *table, bool *out,
                                               atlas_err *err) {
    *out = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT 1 FROM sqlite_schema WHERE type IN ('table','view') AND name=?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a table name");
    }
    *out = sqlite3_step(stmt) == SQLITE_ROW;
    atlas_db_finish(db, stmt);
    return ATLAS_OK;
}

atlas_status atlas_db_maintenance_count(atlas_db *db, const char *table, int64_t *out,
                                        atlas_err *err) {
    *out = 0;
    /* Guarded rather than trusted. The allowlist is a compiled-in array, so
     * this can only fire if somebody adds a name to it with a character that
     * has no business in an identifier — which is the moment to find out. */
    for (const char *p = table; *p != '\0'; p++) {
        if ((*p < 'a' || *p > 'z') && *p != '_' && (*p < '0' || *p > '9')) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "refusing to count a table with an unexpected name");
        }
    }
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM \"%s\";", table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->h, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count a table");
    }
    atlas_status st = ATLAS_OK;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a table count");
    }
    sqlite3_finalize(stmt);
    return st;
}

/* The eligibility predicate appears twice below — once wrapped in a count, once
 * in a delete — and the two must select the same rows, or a plan describes a
 * set that an apply does not remove. They are kept adjacent for that reason,
 * and `tests/test_backup.c` asserts a plan's count equals what an apply
 * removes on the same fixture rather than trusting the two texts to match.
 *
 * `?1` is the cutoff, `?2` the per-repository retain floor. The OFFSET subquery
 * yields NULL for a repository with `retain` rows or fewer, and `id <= NULL` is
 * NULL rather than true — so such a repository contributes nothing, which is
 * the intended reading of a floor. */

atlas_status atlas_db_maintenance_events_eligible(atlas_db *db, const char *cutoff, int64_t retain,
                                                  int64_t *out, atlas_err *err) {
    *out = 0;
    static const char SQL[] = "SELECT count(*) FROM ("
                              "SELECT e.id FROM repo_events e"
                              " WHERE e.created_at < ?1"
                              "   AND e.id <= (SELECT x.id FROM repo_events x"
                              "                WHERE x.repo_id = e.repo_id"
                              "                ORDER BY x.id DESC LIMIT 1 OFFSET ?2));";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_text(stmt, 1, cutoff, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, retain) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind maintenance bounds");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count eligible events");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_maintenance_events_prune(atlas_db *db, const char *cutoff, int64_t retain,
                                               int64_t batch, int64_t *removed_out, bool *more_out,
                                               atlas_err *err) {
    *removed_out = 0;
    *more_out = false;
    static const char SQL[] = "DELETE FROM repo_events WHERE id IN ("
                              "SELECT e.id FROM repo_events e"
                              " WHERE e.created_at < ?1"
                              "   AND e.id <= (SELECT x.id FROM repo_events x"
                              "                WHERE x.repo_id = e.repo_id"
                              "                ORDER BY x.id DESC LIMIT 1 OFFSET ?2)"
                              " LIMIT ?3);";
    /* One transaction per batch, never one across the loop. A delete that runs
     * long must not hold the write transaction for its whole duration; that is
     * the same rule every A1 pass follows. A failure rolls this batch back
     * whole, and the operation is idempotent, so re-running finishes it. */
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_text(stmt, 1, cutoff, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 2, retain) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 3, batch) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind maintenance bounds");
            atlas_db_finish(db, stmt);
        } else {
            st = atlas_db_step_done(db, stmt, err);
            if (st == ATLAS_OK) {
                *removed_out = sqlite3_changes(db->h);
            }
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        *removed_out = 0;
        return st;
    }
    *more_out = *removed_out >= batch;
    return ATLAS_OK;
}

/* --- A9: the gateway audit trail -------------------------------------------
 *
 * Same shape as the `repo_events` pair above, and deliberately not a
 * generalisation of it: the two differ in what the retain floor is counted
 * over, and a single parameterised query that took a table name would be a
 * query built from a string, which is the one thing this file does not do.
 *
 * The floor is global rather than per-repository because an audit row belongs
 * to a credential and an interface, not to a repository. Both conditions still
 * apply together: a row goes only when it is older than the cutoff *and* is
 * outside the newest `retain`, so a quiet installation keeps its whole trail
 * however old it is. */
atlas_status atlas_db_maintenance_audit_eligible(atlas_db *db, const char *cutoff, int64_t retain,
                                                 int64_t *out, atlas_err *err) {
    *out = 0;
    static const char SQL[] = "SELECT count(*) FROM gw_audit"
                              " WHERE at < ?1"
                              "   AND id <= (SELECT x.id FROM gw_audit x"
                              "              ORDER BY x.id DESC LIMIT 1 OFFSET ?2);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_text(stmt, 1, cutoff, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, retain) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind maintenance bounds");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count eligible audit rows");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_maintenance_audit_prune(atlas_db *db, const char *cutoff, int64_t retain,
                                              int64_t batch, int64_t *removed_out, bool *more_out,
                                              atlas_err *err) {
    *removed_out = 0;
    *more_out = false;
    static const char SQL[] = "DELETE FROM gw_audit WHERE id IN ("
                              "SELECT id FROM gw_audit"
                              " WHERE at < ?1"
                              "   AND id <= (SELECT x.id FROM gw_audit x"
                              "              ORDER BY x.id DESC LIMIT 1 OFFSET ?2)"
                              " LIMIT ?3);";
    /* One transaction per batch, never one across the loop — A1's rule about
     * never holding a write transaction across unbounded work. A failure rolls
     * this batch back whole and the operation is idempotent, so re-running
     * finishes it. */
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_text(stmt, 1, cutoff, -1, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 2, retain) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 3, batch) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind maintenance bounds");
            atlas_db_finish(db, stmt);
        } else {
            st = atlas_db_step_done(db, stmt, err);
            if (st == ATLAS_OK) {
                *removed_out = sqlite3_changes(db->h);
            }
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        *removed_out = 0;
        return st;
    }
    *more_out = *removed_out >= batch;
    return ATLAS_OK;
}

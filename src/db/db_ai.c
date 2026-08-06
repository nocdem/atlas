/* Atlas - A2 storage: AI sessions, change sets, reasons, decisions.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Typed operations over the migration-4 tables, plus the per-path working-tree
 * change snapshot the reconciliation pass records.
 *
 * Two properties are enforced here rather than by convention:
 *
 *   - A record's `approved` column is never written. The schema CHECKs it to 0
 *     and nothing in this file binds it, so a caller cannot make Atlas claim a
 *     human approved something A2 has no way to verify.
 *   - Attribution never improves. A changed path already marked ambiguous stays
 *     ambiguous, because a later unambiguous observation says nothing about the
 *     earlier overlapping one.
 *
 * Nothing here touches `evidence`. `atlas_db_evidence_insert` still refuses
 * everything but SOURCE and GIT, and the link table joins the two worlds
 * without merging them.
 */
#include "db/db_internal.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

/* --- small helpers ------------------------------------------------------- */

/* Binds an id, or SQL NULL when the id is 0. Several columns here are nullable
 * references and 0 is the "absent" sentinel throughout Atlas. */
static atlas_status bind_id_opt(atlas_db *db, sqlite3_stmt *stmt, int idx, int64_t id,
                                atlas_err *err) {
    int rc = (id > 0) ? sqlite3_bind_int64(stmt, idx, id) : sqlite3_bind_null(stmt, idx);
    if (rc != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind an identifier");
    }
    return ATLAS_OK;
}

/* Binds a blob, or SQL NULL when there is none. */
static atlas_status bind_blob_opt(atlas_db *db, sqlite3_stmt *stmt, int idx, const void *data,
                                  size_t n, atlas_err *err) {
    if (data == NULL) {
        if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a null path");
        }
        return ATLAS_OK;
    }
    return atlas_db_bind_blob(db, stmt, idx, data, n, err);
}

/* Runs a statement that yields at most one integer. */
static atlas_status query_int_1(atlas_db *db, const char *sql, int64_t arg, int64_t *out,
                                atlas_err *err) {
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, arg) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a query argument");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a count");
    }
    sqlite3_finalize(stmt);
    return st;
}

/* Clamps a caller-supplied row limit. A limit of zero or below means the
 * default; anything above the ceiling is the ceiling. Never unbounded. */
static int64_t clamp_limit(int64_t limit, int64_t def, int64_t max) {
    if (limit <= 0) {
        return def;
    }
    return limit > max ? max : limit;
}

/* --- working-tree change snapshot ---------------------------------------- */

atlas_status atlas_db_worktree_changes_clear(atlas_db *db, int64_t repo_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "DELETE FROM repo_worktree_changes WHERE repo_id=?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_worktree_change_insert(atlas_db *db, int64_t repo_id, int64_t generation,
                                             const atlas_worktree_change_record *rec,
                                             atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* One porcelain-v2 record can produce a staged and an unstaged entry for the
     * same path, which is two distinct facts, so the uniqueness key includes the
     * scope. A replay within one pass updates rather than duplicating. */
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO repo_worktree_changes(repo_id, generation, scope, status, change_type,"
        " path_raw, path_text, old_path_raw, old_path_text, is_directory, observed_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
        " ON CONFLICT(repo_id, scope, path_raw) DO UPDATE SET"
        " generation=excluded.generation, status=excluded.status,"
        " change_type=excluded.change_type, old_path_raw=excluded.old_path_raw,"
        " old_path_text=excluded.old_path_text, is_directory=excluded.is_directory,"
        " observed_at=excluded.observed_at;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    char status_text[2] = {rec->status != '\0' ? rec->status : '?', '\0'};

    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, generation) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 10, rec->is_directory ? 1 : 0) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a worktree change");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, rec->scope, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, status_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, rec->change_type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 6, rec->path_raw, rec->path_raw_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, rec->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = bind_blob_opt(db, stmt, 8, rec->old_path_raw, rec->old_path_raw_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, rec->old_path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 11, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_worktree_changes_list(atlas_db *db, int64_t repo_id, const char *scope,
                                            int64_t after_id, int64_t limit,
                                            atlas_worktree_change_cb cb, void *ud,
                                            int64_t *count_out, int64_t *cursor_out,
                                            bool *more_out, atlas_err *err) {
    *count_out = 0;
    *cursor_out = after_id;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_MCP_DEFAULT_ROWS, ATLAS_MCP_MAX_ROWS);

    sqlite3_stmt *stmt = NULL;
    /* One row more than the limit is requested and never delivered, so `more` is
     * a fact rather than an inference from the page being full. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT id, generation, scope, status, change_type, path_raw, path_text, old_path_text,"
        " is_directory, observed_at FROM repo_worktree_changes"
        " WHERE repo_id=?1 AND id>?2 AND (?3 IS NULL OR scope=?3)"
        " ORDER BY id ASC LIMIT ?4;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, after_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, want + 1) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a worktree change query");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, scope, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*count_out >= want) {
            *more_out = true;
            break;
        }
        atlas_worktree_change_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(stmt, 0);
        row.generation = sqlite3_column_int64(stmt, 1);
        row.scope = atlas_db_col_text(stmt, 2);
        row.status = atlas_db_col_text(stmt, 3);
        row.change_type = atlas_db_col_text(stmt, 4);
        row.path_raw = sqlite3_column_blob(stmt, 5);
        row.path_raw_len = (size_t)sqlite3_column_bytes(stmt, 5);
        row.path_text = atlas_db_col_text(stmt, 6);
        row.old_path_text = atlas_db_col_text_opt(stmt, 7);
        row.is_directory = sqlite3_column_int(stmt, 8) != 0;
        row.observed_at = atlas_db_col_text(stmt, 9);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                sqlite3_finalize(stmt);
                return st;
            }
        }
        (*count_out)++;
        *cursor_out = row.id;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the worktree change snapshot");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_worktree_changes_count(atlas_db *db, int64_t repo_id, int64_t *staged,
                                             int64_t *unstaged, int64_t *untracked,
                                             int64_t *unmerged, atlas_err *err) {
    *staged = 0;
    *unstaged = 0;
    *untracked = 0;
    *unmerged = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT scope, COUNT(*) FROM repo_worktree_changes"
                                       " WHERE repo_id=?1 GROUP BY scope;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *scope = atlas_db_col_text(stmt, 0);
        int64_t n = sqlite3_column_int64(stmt, 1);
        if (strcmp(scope, "staged") == 0) {
            *staged = n;
        } else if (strcmp(scope, "unstaged") == 0) {
            *unstaged = n;
        } else if (strcmp(scope, "untracked") == 0) {
            *untracked = n;
        } else if (strcmp(scope, "unmerged") == 0) {
            *unmerged = n;
        }
    }
    if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count worktree changes");
    }
    sqlite3_finalize(stmt);
    return st;
}

/* --- clients ------------------------------------------------------------- */

atlas_status atlas_db_ai_client_upsert(atlas_db *db, const char *provider, const char *name,
                                       int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_clients(provider, name, first_seen_at, last_seen_at)"
        " VALUES(?1,?2,?3,?3)"
        " ON CONFLICT(provider, name) DO UPDATE SET last_seen_at=excluded.last_seen_at;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, provider, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The row id is read back rather than taken from last_insert_rowid, because
     * the ON CONFLICT path does not insert and would leave that stale. */
    stmt = NULL;
    st = atlas_db_prepare(db, "SELECT id FROM ai_clients WHERE provider=?1 AND name=?2;", &stmt,
                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, provider, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, name, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot resolve the AI client row");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_client_find(atlas_db *db, const char *provider, const char *name,
                                     int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "SELECT id FROM ai_clients WHERE provider=?1 AND name=?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, provider, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, name, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look an AI client up");
    }
    sqlite3_finalize(stmt);
    return st;
}

/* --- sessions ------------------------------------------------------------ */

void atlas_ai_session_report_init(atlas_ai_session_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->provider);
    atlas_buf_init(&r->client);
    atlas_buf_init(&r->session_key);
    atlas_buf_init(&r->agent_type);
}

void atlas_ai_session_report_free(atlas_ai_session_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->provider);
    atlas_buf_free(&r->client);
    atlas_buf_free(&r->session_key);
    atlas_buf_free(&r->agent_type);
}

atlas_status atlas_db_ai_session_find_state(atlas_db *db, int64_t client_id,
                                            const char *session_key, int64_t *id_out,
                                            bool *open_out, atlas_err *err) {
    *id_out = 0;
    *open_out = false;
    if (session_key == NULL || session_key[0] == '\0') {
        return ATLAS_OK;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT id, state FROM ai_sessions WHERE client_id=?1 AND session_key=?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, client_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind client id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, session_key, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
        const char *state = atlas_db_col_text(stmt, 1);
        *open_out = (state != NULL && strcmp(state, "open") == 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look a session up");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_session_find(atlas_db *db, int64_t client_id, const char *session_key,
                                      int64_t *id_out, atlas_err *err) {
    bool open = false;
    return atlas_db_ai_session_find_state(db, client_id, session_key, id_out, &open, err);
}

atlas_status atlas_db_ai_session_open(atlas_db *db, int64_t client_id, const char *session_key,
                                      int64_t parent_id, const char *agent_id,
                                      const char *agent_type, const char *client_version,
                                      int64_t *id_out, bool *created_out, atlas_err *err) {
    *id_out = 0;
    *created_out = false;

    atlas_status st = atlas_db_ai_session_find(db, client_id, session_key, id_out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));

    if (*id_out > 0) {
        /* A resume, not a replacement. The existing row keeps its change set,
         * its recorded reasons and its history; re-creating it would orphan all
         * three and make a resumed session look like a new one that had done
         * nothing. */
        sqlite3_stmt *stmt = NULL;
        st = atlas_db_prepare(db,
                              "UPDATE ai_sessions SET state='open', last_seen_at=?2,"
                              " resumes=resumes+1, closed_at=NULL, close_reason=NULL"
                              " WHERE id=?1;",
                              &stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(stmt, 1, *id_out) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind session id");
        }
        st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
        if (st != ATLAS_OK) {
            sqlite3_finalize(stmt);
            return st;
        }
        return atlas_db_step_done(db, stmt, err);
    }

    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "INSERT INTO ai_sessions(client_id, session_key, parent_id, agent_id,"
                          " agent_type, client_version, state, started_at, last_seen_at)"
                          " VALUES(?1,?2,?3,?4,?5,?6,'open',?7,?7);",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, client_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind client id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, session_key, err);
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, stmt, 3, parent_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, (agent_id != NULL && agent_id[0] != '\0') ? agent_id : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 5, (agent_type != NULL && agent_type[0] != '\0') ? agent_type : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 6, (client_version != NULL && client_version[0] != '\0') ? client_version : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    *id_out = sqlite3_last_insert_rowid(db->h);
    *created_out = true;
    return ATLAS_OK;
}

atlas_status atlas_db_ai_session_touch(atlas_db *db, int64_t session_id, const char *counter,
                                       atlas_err *err) {
    /* The counter name is chosen from a fixed set here rather than interpolated,
     * so no caller can turn it into SQL. */
    const char *sql = "UPDATE ai_sessions SET last_seen_at=?2 WHERE id=?1;";
    if (counter != NULL) {
        if (strcmp(counter, "turns") == 0) {
            sql = "UPDATE ai_sessions SET last_seen_at=?2, turns=turns+1 WHERE id=?1;";
        } else if (strcmp(counter, "tool_calls") == 0) {
            sql = "UPDATE ai_sessions SET last_seen_at=?2, tool_calls=tool_calls+1 WHERE id=?1;";
        } else if (strcmp(counter, "records") == 0) {
            sql = "UPDATE ai_sessions SET last_seen_at=?2, records=records+1 WHERE id=?1;";
        } else if (strcmp(counter, "compactions") == 0) {
            sql = "UPDATE ai_sessions SET last_seen_at=?2, compactions=compactions+1 WHERE id=?1;";
        }
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind session id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_ai_session_close(atlas_db *db, int64_t session_id, const char *reason,
                                       atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* Closing is idempotent: a second close leaves the first close's timestamp
     * and reason in place, because a redelivered SessionEnd must not rewrite
     * when the session actually ended. */
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE ai_sessions SET state='closed', closed_at=?2,"
                                       " close_reason=?3, last_seen_at=?2"
                                       " WHERE id=?1 AND state <> 'closed';",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind session id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, reason, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* A closed session's change sets close with it, so a later pass does not
     * keep attributing changes to a client that has gone. */
    stmt = NULL;
    st = atlas_db_prepare(
        db, "UPDATE ai_change_sets SET closed_at=?2 WHERE session_id=?1 AND closed_at IS NULL;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind session id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_ai_sessions_expire(atlas_db *db, const char *cutoff_iso, int64_t *count_out,
                                         atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *stmt = NULL;
    /* Expired, not closed. A session that stopped answering did not end
     * deliberately, and recording the difference keeps "the client crashed"
     * distinguishable from "the user quit". */
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE ai_sessions SET state='expired', closed_at=?1,"
                                       " close_reason='idle'"
                                       " WHERE state='open' AND last_seen_at < ?1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, cutoff_iso, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK) {
        *count_out = sqlite3_changes(db->h);
    }
    return st;
}

atlas_status atlas_db_ai_session_get(atlas_db *db, int64_t client_id, const char *session_key,
                                     atlas_ai_session_report *out, atlas_err *err) {
    out->present = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "SELECT s.id, c.provider, c.name, s.session_key, s.agent_type, s.state,"
                         " s.started_at, s.last_seen_at, s.turns, s.tool_calls, s.records,"
                         " s.compactions, s.resumes"
                         " FROM ai_sessions s JOIN ai_clients c ON c.id = s.client_id"
                         " WHERE s.client_id=?1 AND s.session_key=?2;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, client_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind client id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, session_key, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->present = true;
        out->id = sqlite3_column_int64(stmt, 0);
        st = atlas_buf_set_str(&out->provider, atlas_db_col_text(stmt, 1), err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->client, atlas_db_col_text(stmt, 2), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->session_key, atlas_db_col_text(stmt, 3), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->agent_type, atlas_db_col_text(stmt, 4), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(stmt, 5, out->state, sizeof(out->state), "state", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(stmt, 6, out->started_at, sizeof(out->started_at), "started_at",
                                   err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(stmt, 7, out->last_seen_at, sizeof(out->last_seen_at),
                                   "last_seen_at", err);
        }
        if (st == ATLAS_OK) {
            out->turns = sqlite3_column_int64(stmt, 8);
            out->tool_calls = sqlite3_column_int64(stmt, 9);
            out->records = sqlite3_column_int64(stmt, 10);
            out->compactions = sqlite3_column_int64(stmt, 11);
            out->resumes = sqlite3_column_int64(stmt, 12);
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a session");
    }
    sqlite3_finalize(stmt);
    if (st == ATLAS_OK && out->present) {
        st = query_int_1(db, "SELECT COUNT(*) FROM ai_session_repos WHERE session_id=?1;", out->id,
                         &out->repos, err);
    }
    return st;
}

atlas_status atlas_db_ai_session_attach_repo(atlas_db *db, int64_t session_id, int64_t repo_id,
                                             const char *source, const char *base_head,
                                             atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* DO NOTHING rather than DO UPDATE: the first attachment records when the
     * session first saw this repository and what HEAD was then, and a later
     * re-attachment must not move that baseline. */
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_session_repos(session_id, repo_id, attached_at, source, base_head)"
        " VALUES(?1,?2,?3,?4,?5) ON CONFLICT(session_id, repo_id) DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a session/repository pair");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, source != NULL ? source : "session_start", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5,
                                    (base_head != NULL && base_head[0] != '\0') ? base_head : NULL,
                                    err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- session events ------------------------------------------------------ */

atlas_status atlas_db_ai_event_append(atlas_db *db, int64_t session_id, int64_t repo_id,
                                      const char *kind, const char *tool_name,
                                      const char *tool_use_id, const void *path_raw,
                                      size_t path_len, const char *path_text,
                                      const char *dedup_key, bool *inserted_out, atlas_err *err) {
    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    /* The dedup key collides on a partial unique index, so a hook Claude
     * delivered twice — a retry, a fork, a resume replaying its tail — records
     * one row rather than two. */
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_session_events(session_id, repo_id, kind, tool_name, tool_use_id,"
        " path_raw, path_text, created_at, dedup_key) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind session id");
    }
    st = bind_id_opt(db, stmt, 2, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 4, (tool_name != NULL && tool_name[0] != '\0') ? tool_name : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 5, (tool_use_id != NULL && tool_use_id[0] != '\0') ? tool_use_id : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = bind_blob_opt(db, stmt, 6, path_raw, path_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 7, (path_text != NULL && path_text[0] != '\0') ? path_text : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 9, (dedup_key != NULL && dedup_key[0] != '\0') ? dedup_key : NULL, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && inserted_out != NULL) {
        *inserted_out = sqlite3_changes(db->h) > 0;
    }
    return st;
}

atlas_status atlas_db_ai_events_prune(atlas_db *db, int64_t session_id, int64_t retain,
                                      int64_t *removed_out, atlas_err *err) {
    if (removed_out != NULL) {
        *removed_out = 0;
    }
    if (retain <= 0) {
        retain = ATLAS_AI_EVENTS_RETAIN_PER_SESSION;
    }
    sqlite3_stmt *stmt = NULL;
    /* This statement reaches only ai_session_events. Durable reasons and
     * decisions are in other tables and nothing here can address them. */
    atlas_status st = atlas_db_prepare(db,
                                       "DELETE FROM ai_session_events WHERE session_id=?1 AND id"
                                       " NOT IN (SELECT id FROM ai_session_events"
                                       " WHERE session_id=?1 ORDER BY id DESC LIMIT ?2);",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, retain) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a prune bound");
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && removed_out != NULL) {
        *removed_out = sqlite3_changes(db->h);
    }
    return st;
}

atlas_status atlas_db_ai_event_intent_for_path(atlas_db *db, int64_t session_id,
                                               const void *path_raw, size_t path_len,
                                               atlas_buf *tool_out, bool *found_out,
                                               atlas_err *err) {
    *found_out = false;
    sqlite3_stmt *stmt = NULL;
    /* Only 'tool_intent' and 'tool_ok' count. A tool that failed said so, and a
     * failed edit is not evidence that this session changed the file. */
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT tool_name FROM ai_session_events"
                                       " WHERE session_id=?1 AND path_raw=?2"
                                       " AND kind IN ('tool_intent','tool_ok')"
                                       " ORDER BY id DESC LIMIT 1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind session id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *found_out = true;
        st = atlas_buf_set_str(tool_out, atlas_db_col_text(stmt, 0), err);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a tool intent");
    }
    sqlite3_finalize(stmt);
    return st;
}

/* --- change sets --------------------------------------------------------- */

atlas_status atlas_db_ai_change_set_find(atlas_db *db, int64_t session_id, int64_t repo_id,
                                         int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT id FROM ai_change_sets WHERE session_id=?1 AND repo_id=?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a change-set key");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look a change set up");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_change_set_ensure(atlas_db *db, int64_t session_id, int64_t repo_id,
                                           const char *base_head, int64_t base_generation,
                                           int64_t *id_out, atlas_err *err) {
    atlas_status st = atlas_db_ai_change_set_find(db, session_id, repo_id, id_out, err);
    if (st != ATLAS_OK || *id_out > 0) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "INSERT INTO ai_change_sets(session_id, repo_id, opened_at, base_head,"
                          " last_head, base_generation, last_generation)"
                          " VALUES(?1,?2,?3,?4,?4,?5,?5);",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, base_generation) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a change set");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4,
                                    (base_head != NULL && base_head[0] != '\0') ? base_head : NULL,
                                    err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK) {
        *id_out = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

atlas_status atlas_db_ai_changed_path_record(atlas_db *db, int64_t change_set_id,
                                             const void *path_raw, size_t path_len,
                                             const char *path_text, const char *attribution,
                                             const char *direct_tool, int64_t concurrent_sessions,
                                             atlas_err *err) {
    /* The set is bounded. A session that touches more paths than this stops
     * recording individual ones; the change set's `truncated` flag is what a
     * reader sees, so the ceiling is visible rather than silent. */
    int64_t have = 0;
    atlas_status st =
        query_int_1(db, "SELECT COUNT(*) FROM ai_changed_paths WHERE change_set_id=?1;",
                    change_set_id, &have, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (have >= ATLAS_AI_MAX_CHANGED_PATHS) {
        sqlite3_stmt *tstmt = NULL;
        st = atlas_db_prepare(db, "UPDATE ai_change_sets SET truncated=1 WHERE id=?1;", &tstmt,
                              err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(tstmt, 1, change_set_id) != SQLITE_OK) {
            sqlite3_finalize(tstmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind change-set id");
        }
        return atlas_db_step_done(db, tstmt, err);
    }

    sqlite3_stmt *stmt = NULL;
    /* Attribution never improves.
     *
     * CASE, not assignment: a row already 'ambiguous' stays ambiguous whatever
     * this observation claims, because the earlier overlap is a fact about a
     * window that has already passed and a later clean observation does not
     * retract it. A row may be promoted from 'observed' to 'direct_edit', which
     * is the one direction that adds information rather than discarding it. */
    st = atlas_db_prepare(
        db,
        "INSERT INTO ai_changed_paths(change_set_id, path_raw, path_text, attribution,"
        " direct_tool, first_at, last_at, occurrences, concurrent_sessions)"
        " VALUES(?1,?2,?3,?4,?5,?6,?6,1,?7)"
        " ON CONFLICT(change_set_id, path_raw) DO UPDATE SET"
        " attribution = CASE"
        "   WHEN ai_changed_paths.attribution='ambiguous' THEN 'ambiguous'"
        "   WHEN excluded.attribution='ambiguous' THEN 'ambiguous'"
        "   WHEN excluded.attribution='direct_edit' THEN 'direct_edit'"
        "   ELSE ai_changed_paths.attribution END,"
        " direct_tool = COALESCE(excluded.direct_tool, ai_changed_paths.direct_tool),"
        " last_at = excluded.last_at,"
        " occurrences = ai_changed_paths.occurrences + 1,"
        " concurrent_sessions = MAX(ai_changed_paths.concurrent_sessions,"
        "                           excluded.concurrent_sessions);",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, change_set_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 7, concurrent_sessions) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a changed path");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, attribution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 5, (direct_tool != NULL && direct_tool[0] != '\0') ? direct_tool : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_ai_changed_counts(atlas_db *db, int64_t change_set_id, int64_t *total,
                                        int64_t *direct, int64_t *ambiguous, int64_t *unresolved,
                                        atlas_err *err) {
    *total = 0;
    *direct = 0;
    *ambiguous = 0;
    *unresolved = 0;
    sqlite3_stmt *stmt = NULL;
    /* `unresolved` is the honesty counter: a changed path for which no reason of
     * any kind — not even an explicit UNKNOWN — has been recorded. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT COUNT(*),"
        " SUM(CASE WHEN p.attribution='direct_edit' THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN p.attribution='ambiguous' THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN NOT EXISTS (SELECT 1 FROM ai_reason_paths rp"
        "   JOIN ai_reasons r ON r.id = rp.reason_id"
        "   WHERE rp.path_raw = p.path_raw AND r.change_set_id = p.change_set_id)"
        "  THEN 1 ELSE 0 END)"
        " FROM ai_changed_paths p WHERE p.change_set_id=?1;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, change_set_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind change-set id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *total = sqlite3_column_int64(stmt, 0);
        *direct = sqlite3_column_int64(stmt, 1);
        *ambiguous = sqlite3_column_int64(stmt, 2);
        *unresolved = sqlite3_column_int64(stmt, 3);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count changed paths");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_changed_list(atlas_db *db, int64_t change_set_id, int64_t limit,
                                      atlas_ai_changed_cb cb, void *ud, int64_t *count_out,
                                      bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_MCP_DEFAULT_ROWS, ATLAS_MCP_MAX_ROWS);

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT p.path_text, p.attribution, p.direct_tool, p.first_at, p.last_at, p.occurrences,"
        " p.concurrent_sessions, p.path_raw,"
        " EXISTS (SELECT 1 FROM ai_reason_paths rp JOIN ai_reasons r ON r.id = rp.reason_id"
        "   WHERE rp.path_raw = p.path_raw AND r.change_set_id = p.change_set_id)"
        " FROM ai_changed_paths p WHERE p.change_set_id=?1"
        " ORDER BY p.last_at DESC, p.id DESC LIMIT ?2;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, change_set_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, want + 1) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a changed-path query");
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*count_out >= want) {
            *more_out = true;
            break;
        }
        atlas_ai_changed_row row;
        memset(&row, 0, sizeof(row));
        row.path_text = atlas_db_col_text(stmt, 0);
        row.attribution = atlas_db_col_text(stmt, 1);
        row.direct_tool = atlas_db_col_text_opt(stmt, 2);
        row.first_at = atlas_db_col_text(stmt, 3);
        row.last_at = atlas_db_col_text(stmt, 4);
        row.occurrences = sqlite3_column_int64(stmt, 5);
        row.concurrent_sessions = sqlite3_column_int64(stmt, 6);
        row.path_raw = sqlite3_column_blob(stmt, 7);
        row.path_raw_len = (size_t)sqlite3_column_bytes(stmt, 7);
        row.has_reason = sqlite3_column_int(stmt, 8) != 0;
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                sqlite3_finalize(stmt);
                return st;
            }
        }
        (*count_out)++;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read changed paths");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_concurrent_sessions(atlas_db *db, int64_t repo_id, int64_t except_session,
                                             int64_t *count_out, atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT COUNT(*) FROM ai_sessions s"
                                       " JOIN ai_session_repos r ON r.session_id = s.id"
                                       " WHERE r.repo_id=?1 AND s.state='open' AND s.id <> ?2;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, except_session) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a concurrency query");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count_out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count concurrent sessions");
    }
    sqlite3_finalize(stmt);
    return st;
}

/* --- durable records ----------------------------------------------------- */

/* `approved` is deliberately absent from both insert statements below. The
 * column exists, defaults to 0 and is CHECKed to 0; not binding it is the third
 * layer, so that adding an approval path is a deliberate act in three places
 * rather than an accident in one. */

atlas_status atlas_db_ai_reason_insert(atlas_db *db, const atlas_ai_record_input *in,
                                       int64_t *id_out, bool *duplicate_out, atlas_err *err) {
    *id_out = 0;
    *duplicate_out = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_reasons(session_id, repo_id, change_set_id, created_at, provenance,"
        " state, confidence, summary, detail, unknown_reason, dedup_key)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_id_opt(db, stmt, 1, in->session_id, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, in->repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, stmt, 3, in->change_set_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, in->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, in->state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, in->confidence != NULL ? in->confidence : "none",
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, in->summary, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, in->detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, in->unknown_reason, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 11, in->dedup_key, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_changes(db->h) > 0) {
        *id_out = sqlite3_last_insert_rowid(db->h);
        return ATLAS_OK;
    }
    /* The dedup key suppressed it. The existing row's id is returned so a caller
     * that retried gets the same identifier it would have got the first time. */
    *duplicate_out = true;
    if (in->dedup_key == NULL) {
        return ATLAS_OK;
    }
    stmt = NULL;
    st = atlas_db_prepare(db, "SELECT id FROM ai_reasons WHERE repo_id=?1 AND dedup_key=?2;", &stmt,
                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, in->repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, in->dedup_key, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_reason_path_add(atlas_db *db, int64_t reason_id, const void *path_raw,
                                         size_t path_len, const char *path_text, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "INSERT INTO ai_reason_paths(reason_id, path_raw, path_text)"
                                       " VALUES(?1,?2,?3) ON CONFLICT DO NOTHING;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, reason_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind reason id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, path_text, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_ai_decision_insert(atlas_db *db, const atlas_ai_record_input *in,
                                         int64_t *id_out, bool *duplicate_out, atlas_err *err) {
    *id_out = 0;
    *duplicate_out = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_decisions(session_id, repo_id, created_at, provenance, state, title,"
        " statement, rationale, dedup_key) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_id_opt(db, stmt, 1, in->session_id, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, in->repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, in->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, in->state != NULL ? in->state : "proposed", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, in->title, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, in->statement, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, in->rationale, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, in->dedup_key, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_changes(db->h) > 0) {
        *id_out = sqlite3_last_insert_rowid(db->h);
        return ATLAS_OK;
    }
    *duplicate_out = true;
    if (in->dedup_key == NULL) {
        return ATLAS_OK;
    }
    stmt = NULL;
    st = atlas_db_prepare(db, "SELECT id FROM ai_decisions WHERE repo_id=?1 AND dedup_key=?2;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, in->repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, in->dedup_key, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_ai_decision_path_add(atlas_db *db, int64_t decision_id, const void *path_raw,
                                           size_t path_len, const char *path_text,
                                           atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO ai_decision_paths(decision_id, path_raw, path_text)"
                         " VALUES(?1,?2,?3) ON CONFLICT DO NOTHING;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, decision_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind decision id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, path_text, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_ai_evidence_link(atlas_db *db, const char *subject_kind, int64_t subject_id,
                                       int64_t repo_id, const void *path_raw, size_t path_len,
                                       atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    /* The newest SOURCE or GIT evidence Atlas holds for the path. A path with no
     * evidence links nothing and that is not an error: it is the normal state of
     * a file Atlas has not indexed yet. */
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_evidence_links(subject_kind, subject_id, evidence_id)"
        " SELECT ?1, ?2, e.id FROM evidence e"
        " WHERE e.repo_id=?3 AND e.path_raw=?4 AND e.kind IN ('SOURCE','GIT')"
        " ORDER BY e.id DESC LIMIT 1"
        " ON CONFLICT DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, subject_kind, err);
    if (st == ATLAS_OK && (sqlite3_bind_int64(stmt, 2, subject_id) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 3, repo_id) != SQLITE_OK)) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind an evidence link");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 4, path_raw, path_len, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_ai_checkpoint_insert(atlas_db *db, int64_t session_id, const char *phase,
                                           int64_t repos, int64_t changed_paths,
                                           int64_t unresolved_paths, int64_t reasons,
                                           int64_t decisions, const char *dedup_key,
                                           bool *inserted_out, atlas_err *err) {
    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO ai_checkpoints(session_id, created_at, phase, repos, changed_paths,"
        " unresolved_paths, reasons, decisions, dedup_key) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, session_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, repos) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, changed_paths) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, unresolved_paths) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 7, reasons) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, decisions) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a checkpoint");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, phase, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 9, (dedup_key != NULL && dedup_key[0] != '\0') ? dedup_key : NULL, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && inserted_out != NULL) {
        *inserted_out = sqlite3_changes(db->h) > 0;
    }
    return st;
}

/* --- reading records ----------------------------------------------------- */

/* Streams reason rows from a prepared statement whose columns are the fixed
 * list below. Shared by the list and search variants so the two cannot report
 * the same row differently. */
static atlas_status stream_reasons(atlas_db *db, sqlite3_stmt *stmt, int64_t want,
                                   atlas_ai_reason_cb cb, void *ud, int64_t *count_out,
                                   bool *more_out, atlas_err *err) {
    atlas_status st = ATLAS_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*count_out >= want) {
            *more_out = true;
            break;
        }
        atlas_ai_reason_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(stmt, 0);
        row.session_id = sqlite3_column_int64(stmt, 1);
        row.created_at = atlas_db_col_text(stmt, 2);
        row.provenance = atlas_db_col_text(stmt, 3);
        row.state = atlas_db_col_text(stmt, 4);
        row.confidence = atlas_db_col_text(stmt, 5);
        row.summary = atlas_db_col_text_opt(stmt, 6);
        row.detail = atlas_db_col_text_opt(stmt, 7);
        row.unknown_reason = atlas_db_col_text_opt(stmt, 8);
        row.approved = sqlite3_column_int(stmt, 9) != 0;
        row.path_count = sqlite3_column_int64(stmt, 10);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                sqlite3_finalize(stmt);
                return st;
            }
        }
        (*count_out)++;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read recorded reasons");
    }
    sqlite3_finalize(stmt);
    return st;
}

#define AI_REASON_COLUMNS                                                                          \
    "r.id, r.session_id, r.created_at, r.provenance, r.state, r.confidence, r.summary, r.detail,"  \
    " r.unknown_reason, r.approved,"                                                               \
    " (SELECT COUNT(*) FROM ai_reason_paths p WHERE p.reason_id = r.id)"

atlas_status atlas_db_ai_reasons_list(atlas_db *db, int64_t repo_id, const void *path_raw,
                                      size_t path_len, int64_t limit, atlas_ai_reason_cb cb,
                                      void *ud, int64_t *count_out, bool *more_out,
                                      atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_MCP_DEFAULT_ROWS, ATLAS_MCP_MAX_ROWS);

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT " AI_REASON_COLUMNS " FROM ai_reasons r WHERE r.repo_id=?1"
        " AND (?2 IS NULL OR EXISTS (SELECT 1 FROM ai_reason_paths p"
        "      WHERE p.reason_id = r.id AND p.path_raw = ?2))"
        " ORDER BY r.id DESC LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, want + 1) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a reason query");
    }
    st = bind_blob_opt(db, stmt, 2, path_raw, path_len, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return stream_reasons(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_ai_reasons_search(atlas_db *db, int64_t repo_id, const char *query,
                                        int64_t limit, atlas_ai_reason_cb cb, void *ud,
                                        int64_t *count_out, bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_MCP_DEFAULT_ROWS, ATLAS_MCP_MAX_ROWS);

    sqlite3_stmt *stmt = NULL;
    /* A bounded LIKE with an explicit ESCAPE, so a query containing % or _ means
     * those characters rather than becoming a wildcard the caller did not ask
     * for. Deliberately not FTS5: these tables are small, and a query language a
     * model can be surprised by is a query language that returns the wrong rows
     * without saying so. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT " AI_REASON_COLUMNS " FROM ai_reasons r WHERE r.repo_id=?1"
        " AND (r.summary LIKE ?2 ESCAPE '\\' OR r.detail LIKE ?2 ESCAPE '\\'"
        "      OR r.unknown_reason LIKE ?2 ESCAPE '\\')"
        " ORDER BY r.id DESC LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, want + 1) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a reason search");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, query, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return stream_reasons(db, stmt, want, cb, ud, count_out, more_out, err);
}

static atlas_status stream_decisions(atlas_db *db, sqlite3_stmt *stmt, int64_t want,
                                     atlas_ai_decision_cb cb, void *ud, int64_t *count_out,
                                     bool *more_out, atlas_err *err) {
    atlas_status st = ATLAS_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*count_out >= want) {
            *more_out = true;
            break;
        }
        atlas_ai_decision_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(stmt, 0);
        row.session_id = sqlite3_column_int64(stmt, 1);
        row.created_at = atlas_db_col_text(stmt, 2);
        row.provenance = atlas_db_col_text(stmt, 3);
        row.state = atlas_db_col_text(stmt, 4);
        row.title = atlas_db_col_text(stmt, 5);
        row.statement = atlas_db_col_text(stmt, 6);
        row.rationale = atlas_db_col_text_opt(stmt, 7);
        row.approved = sqlite3_column_int(stmt, 8) != 0;
        row.path_count = sqlite3_column_int64(stmt, 9);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                sqlite3_finalize(stmt);
                return st;
            }
        }
        (*count_out)++;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read recorded decisions");
    }
    sqlite3_finalize(stmt);
    return st;
}

#define AI_DECISION_COLUMNS                                                                        \
    "d.id, d.session_id, d.created_at, d.provenance, d.state, d.title, d.statement, d.rationale,"  \
    " d.approved, (SELECT COUNT(*) FROM ai_decision_paths p WHERE p.decision_id = d.id)"

atlas_status atlas_db_ai_decisions_list(atlas_db *db, int64_t repo_id, const void *path_raw,
                                        size_t path_len, int64_t limit, atlas_ai_decision_cb cb,
                                        void *ud, int64_t *count_out, bool *more_out,
                                        atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_MCP_DEFAULT_ROWS, ATLAS_MCP_MAX_ROWS);

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT " AI_DECISION_COLUMNS " FROM ai_decisions d WHERE d.repo_id=?1"
        " AND (?2 IS NULL OR EXISTS (SELECT 1 FROM ai_decision_paths p"
        "      WHERE p.decision_id = d.id AND p.path_raw = ?2))"
        " ORDER BY d.id DESC LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, want + 1) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a decision query");
    }
    st = bind_blob_opt(db, stmt, 2, path_raw, path_len, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return stream_decisions(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_ai_decisions_search(atlas_db *db, int64_t repo_id, const char *query,
                                          int64_t limit, atlas_ai_decision_cb cb, void *ud,
                                          int64_t *count_out, bool *more_out, atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    int64_t want = clamp_limit(limit, ATLAS_MCP_DEFAULT_ROWS, ATLAS_MCP_MAX_ROWS);

    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT " AI_DECISION_COLUMNS " FROM ai_decisions d WHERE d.repo_id=?1"
        " AND (d.title LIKE ?2 ESCAPE '\\' OR d.statement LIKE ?2 ESCAPE '\\'"
        "      OR d.rationale LIKE ?2 ESCAPE '\\')"
        " ORDER BY d.id DESC LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, want + 1) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a decision search");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, query, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return stream_decisions(db, stmt, want, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_ai_repo_record_counts(atlas_db *db, int64_t repo_id, int64_t *proposed,
                                            int64_t *approved, int64_t *reasons, atlas_err *err) {
    atlas_status st = query_int_1(
        db, "SELECT COUNT(*) FROM ai_decisions WHERE repo_id=?1 AND state='proposed';", repo_id,
        proposed, err);
    if (st == ATLAS_OK) {
        /* Always zero in A2. Reported anyway, so the envelope's shape does not
         * change when an approval workflow arrives. */
        st = query_int_1(db, "SELECT COUNT(*) FROM ai_decisions WHERE repo_id=?1 AND approved=1;",
                         repo_id, approved, err);
    }
    if (st == ATLAS_OK) {
        st = query_int_1(db, "SELECT COUNT(*) FROM ai_reasons WHERE repo_id=?1;", repo_id, reasons,
                         err);
    }
    return st;
}

/* Atlas - search over indexed file paths and commit messages.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * FTS5 is used when the linked SQLite provides it. When it does not, search
 * falls back to a bounded substring match and reports ATLAS_SEARCH_DEGRADED_LIKE
 * so the caller can tell the user that results are not ranked. Silent failure is
 * never an option.
 */
#include "db/db_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

/* Wrap each whitespace-separated term as an FTS5 phrase with a prefix match, so
 * an arbitrary user string can never be interpreted as FTS5 query syntax. */
static atlas_status build_fts_query(const char *q, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    const unsigned char *p = (const unsigned char *)q;
    size_t i = 0;
    int terms = 0;
    while (p[i] != '\0') {
        while (p[i] != '\0' && isspace((int)p[i])) {
            i++;
        }
        if (p[i] == '\0') {
            break;
        }
        size_t start = i;
        while (p[i] != '\0' && !isspace((int)p[i])) {
            i++;
        }
        if (terms > 0) {
            atlas_status st = atlas_buf_append_ch(out, ' ', err);
            if (st != ATLAS_OK) {
                return st;
            }
        }
        atlas_status st = atlas_buf_append_ch(out, '"', err);
        if (st != ATLAS_OK) {
            return st;
        }
        for (size_t k = start; k < i; k++) {
            if (p[k] == '"') {
                st = atlas_buf_append_str(out, "\"\"", err); /* doubled inside a phrase */
            } else {
                st = atlas_buf_append(out, p + k, 1u, err);
            }
            if (st != ATLAS_OK) {
                return st;
            }
        }
        st = atlas_buf_append_str(out, "\"*", err);
        if (st != ATLAS_OK) {
            return st;
        }
        terms++;
    }
    if (terms == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "search query is empty");
    }
    return ATLAS_OK;
}

/* Escape the LIKE metacharacters so a query is always a literal substring. */
static atlas_status build_like_pattern(const char *q, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append_ch(out, '%', err);
    for (const char *p = q; st == ATLAS_OK && *p != '\0'; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            st = atlas_buf_append_ch(out, '\\', err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        st = atlas_buf_append(out, p, 1u, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_buf_append_ch(out, '%', err);
}

static atlas_status run_file_query(atlas_db *db, const char *sql, int64_t repo_id,
                                   const char *match, int64_t limit, atlas_search_cb cb, void *ud,
                                   int64_t *count, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, match, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 3, limit) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind search parameters");
        }
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "file search failed");
            break;
        }
        atlas_search_hit hit;
        memset(&hit, 0, sizeof(hit));
        hit.kind = "file";
        hit.path_raw = sqlite3_column_blob(stmt, 0);
        int n = sqlite3_column_bytes(stmt, 0);
        hit.path_raw_len = n > 0 ? (size_t)n : 0u;
        hit.path_text = atlas_db_col_text(stmt, 1);
        hit.path_is_utf8 = sqlite3_column_int(stmt, 2) != 0;
        hit.git_index_oid = atlas_db_col_text_opt(stmt, 3);
        hit.deleted = sqlite3_column_int(stmt, 4) != 0;
        hit.evidence = "SOURCE";
        (*count)++;
        if (cb != NULL) {
            st = cb(&hit, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

static atlas_status run_commit_query(atlas_db *db, const char *sql, int64_t repo_id,
                                     const char *match, int64_t limit, atlas_search_cb cb, void *ud,
                                     int64_t *count, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, match, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 3, limit) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind search parameters");
        }
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "commit search failed");
            break;
        }
        atlas_search_hit hit;
        memset(&hit, 0, sizeof(hit));
        hit.kind = "commit";
        hit.commit_oid = atlas_db_col_text(stmt, 0);
        hit.subject = atlas_db_col_text(stmt, 1);
        hit.author_name = atlas_db_col_text(stmt, 2);
        hit.author_time = sqlite3_column_int64(stmt, 3);
        hit.evidence = "GIT";
        (*count)++;
        if (cb != NULL) {
            st = cb(&hit, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_search(atlas_db *db, int64_t repo_id, const char *query, int64_t limit,
                             atlas_search_mode *mode_out, atlas_search_cb cb, void *ud,
                             int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (query == NULL || query[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "search query is empty");
    }
    int64_t effective_limit = limit > 0 ? limit : 50;
    int64_t count = 0;
    atlas_search_mode mode = db->fts_ready ? ATLAS_SEARCH_FTS5 : ATLAS_SEARCH_DEGRADED_LIKE;
    if (mode_out != NULL) {
        *mode_out = mode;
    }

    atlas_buf pattern = ATLAS_BUF_INIT;
    atlas_status st;
    if (mode == ATLAS_SEARCH_FTS5) {
        st = build_fts_query(query, &pattern, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&pattern);
            return st;
        }
        st = run_file_query(db,
                            "SELECT f.path_raw, f.path_text, f.path_is_utf8, f.git_index_oid,"
                            " f.deleted FROM files_fts JOIN files f ON f.id = files_fts.rowid"
                            " WHERE files_fts MATCH ?1 AND f.repo_id = ?2"
                            " ORDER BY bm25(files_fts), f.path_text LIMIT ?3;",
                            repo_id, atlas_buf_cstr(&pattern), effective_limit, cb, ud, &count, err);
        if (st == ATLAS_OK) {
            st = run_commit_query(
                db,
                "SELECT c.oid, c.subject, c.author_name, c.author_time"
                " FROM commits_fts JOIN commits c ON c.id = commits_fts.rowid"
                " WHERE commits_fts MATCH ?1 AND c.repo_id = ?2"
                " ORDER BY bm25(commits_fts), c.commit_time DESC LIMIT ?3;",
                repo_id, atlas_buf_cstr(&pattern), effective_limit, cb, ud, &count, err);
        }
    } else {
        st = build_like_pattern(query, &pattern, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&pattern);
            return st;
        }
        st = run_file_query(db,
                            "SELECT path_raw, path_text, path_is_utf8, git_index_oid, deleted"
                            " FROM files WHERE path_text LIKE ?1 ESCAPE '\\' AND repo_id = ?2"
                            " ORDER BY path_text LIMIT ?3;",
                            repo_id, atlas_buf_cstr(&pattern), effective_limit, cb, ud, &count, err);
        if (st == ATLAS_OK) {
            st = run_commit_query(db,
                                  "SELECT oid, subject, author_name, author_time FROM commits"
                                  " WHERE (subject LIKE ?1 ESCAPE '\\' OR body LIKE ?1 ESCAPE '\\')"
                                  " AND repo_id = ?2 ORDER BY commit_time DESC LIMIT ?3;",
                                  repo_id, atlas_buf_cstr(&pattern), effective_limit, cb, ud,
                                  &count, err);
        }
    }

    atlas_buf_free(&pattern);
    if (st == ATLAS_OK && count_out != NULL) {
        *count_out = count;
    }
    return st;
}

/* Atlas - indexed facts: files, commits, changes, evidence, compile databases.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "db/db_internal.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

#define FILE_COLS                                                                          \
    "id, path_raw, path_text, path_is_utf8, file_type, language, git_mode, git_index_oid,"  \
    " content_hash, content_hash_algo, size_bytes, is_executable, is_symlink, unsafe_path," \
    " read_error, first_seen_scan_id, last_seen_scan_id, first_seen_at, last_seen_at,"      \
    " deleted, deleted_at"

static void load_file_row(sqlite3_stmt *stmt, atlas_file_row *row) {
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->path_raw = sqlite3_column_blob(stmt, 1);
    int n = sqlite3_column_bytes(stmt, 1);
    row->path_raw_len = n > 0 ? (size_t)n : 0u;
    row->path_text = atlas_db_col_text(stmt, 2);
    row->path_is_utf8 = sqlite3_column_int(stmt, 3) != 0;
    row->file_type = atlas_db_col_text(stmt, 4);
    row->language = atlas_db_col_text_opt(stmt, 5);
    row->git_mode = atlas_db_col_text_opt(stmt, 6);
    row->git_index_oid = atlas_db_col_text_opt(stmt, 7);
    row->content_hash = atlas_db_col_text_opt(stmt, 8);
    row->content_hash_algo = atlas_db_col_text_opt(stmt, 9);
    row->size_known = sqlite3_column_type(stmt, 10) != SQLITE_NULL;
    row->size_bytes = row->size_known ? sqlite3_column_int64(stmt, 10) : 0;
    row->is_executable = sqlite3_column_int(stmt, 11) != 0;
    row->is_symlink = sqlite3_column_int(stmt, 12) != 0;
    row->unsafe_path = sqlite3_column_int(stmt, 13) != 0;
    row->read_error = atlas_db_col_text_opt(stmt, 14);
    row->first_seen_scan_id = sqlite3_column_int64(stmt, 15);
    row->last_seen_scan_id = sqlite3_column_int64(stmt, 16);
    row->first_seen_at = atlas_db_col_text(stmt, 17);
    row->last_seen_at = atlas_db_col_text(stmt, 18);
    row->deleted = sqlite3_column_int(stmt, 19) != 0;
    row->deleted_at = atlas_db_col_text_opt(stmt, 20);
}

static bool str_eq_opt(const char *a, const char *b) {
    if (a == NULL && b == NULL) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    return strcmp(a, b) == 0;
}

/* True when nothing Atlas records about the file has changed. Timestamps and
 * scan ids are excluded on purpose: they change every scan, and including them
 * would make repeated scans of an unchanged tree report churn. */
static bool file_row_matches(const atlas_file_row *old, const atlas_file_record *rec) {
    if (old->deleted) {
        return false;
    }
    if (!str_eq_opt(old->content_hash, rec->content_hash)) {
        return false;
    }
    if (!str_eq_opt(old->git_index_oid, rec->git_index_oid)) {
        return false;
    }
    if (!str_eq_opt(old->git_mode, rec->git_mode)) {
        return false;
    }
    if (!str_eq_opt(old->file_type, rec->file_type)) {
        return false;
    }
    if (!str_eq_opt(old->language, rec->language)) {
        return false;
    }
    if (!str_eq_opt(old->read_error, rec->read_error)) {
        return false;
    }
    if (old->is_executable != rec->is_executable || old->is_symlink != rec->is_symlink ||
        old->unsafe_path != rec->unsafe_path) {
        return false;
    }
    if (old->size_known != rec->size_known) {
        return false;
    }
    if (old->size_known && old->size_bytes != rec->size_bytes) {
        return false;
    }
    return true;
}

static atlas_status bind_file_fields(atlas_db *db, sqlite3_stmt *stmt, int base,
                                    const atlas_file_record *rec, atlas_err *err) {
    atlas_status st = atlas_db_bind_text_opt(db, stmt, base + 0, rec->file_type, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, base + 1, rec->language, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, base + 2, rec->git_mode, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, base + 3, rec->git_index_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, base + 4, rec->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, base + 5,
                                    rec->content_hash != NULL ? rec->content_hash_algo : NULL, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    int rc = rec->size_known ? sqlite3_bind_int64(stmt, base + 6, rec->size_bytes)
                             : sqlite3_bind_null(stmt, base + 6);
    if (rc != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind size");
    }
    if (sqlite3_bind_int(stmt, base + 7, rec->is_executable ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, base + 8, rec->is_symlink ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, base + 9, rec->unsafe_path ? 1 : 0) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file flags");
    }
    return atlas_db_bind_text_opt(db, stmt, base + 10, rec->read_error, err);
}

atlas_status atlas_db_file_upsert(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                  const atlas_file_record *rec, atlas_upsert_kind *kind_out,
                                  atlas_err *err) {
    if (kind_out != NULL) {
        *kind_out = ATLAS_UPSERT_UNCHANGED;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));

    sqlite3_stmt *sel = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT " FILE_COLS " FROM files WHERE repo_id=?1 AND path_raw=?2;", &sel, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(sel, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, sel, 2, rec->path_raw, rec->path_raw_len, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(sel);
        return st;
    }

    int rc = sqlite3_step(sel);
    bool exists = (rc == SQLITE_ROW);
    atlas_file_row old;
    bool unchanged = false;
    int64_t existing_id = 0;
    if (exists) {
        load_file_row(sel, &old);
        existing_id = old.id;
        unchanged = file_row_matches(&old, rec);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read existing file row");
        sqlite3_finalize(sel);
        return st;
    }
    bool was_deleted = exists && old.deleted;
    sqlite3_finalize(sel); /* borrowed pointers in `old` die here */

    if (!exists) {
        sqlite3_stmt *ins = NULL;
        st = atlas_db_prepare(db,
                              "INSERT INTO files(repo_id, path_raw, path_text, path_is_utf8,"
                              " file_type, language, git_mode, git_index_oid, content_hash,"
                              " content_hash_algo, size_bytes, is_executable, is_symlink,"
                              " unsafe_path, read_error, first_seen_scan_id, last_seen_scan_id,"
                              " first_seen_at, last_seen_at, deleted)"
                              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,"
                              "?16,?17,?18,?19,0);",
                              &ins, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(ins, 1, repo_id) != SQLITE_OK) {
            sqlite3_finalize(ins);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
        }
        st = atlas_db_bind_blob(db, ins, 2, rec->path_raw, rec->path_raw_len, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, ins, 3, rec->path_text, err);
        }
        if (st == ATLAS_OK && sqlite3_bind_int(ins, 4, rec->path_is_utf8 ? 1 : 0) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind path encoding flag");
        }
        if (st == ATLAS_OK) {
            st = bind_file_fields(db, ins, 5, rec, err);
        }
        if (st == ATLAS_OK) {
            if (sqlite3_bind_int64(ins, 16, scan_id) != SQLITE_OK ||
                sqlite3_bind_int64(ins, 17, scan_id) != SQLITE_OK) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, ins, 18, now, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, ins, 19, now, err);
        }
        if (st != ATLAS_OK) {
            sqlite3_finalize(ins);
            return st;
        }
        st = atlas_db_step_done(db, ins, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (kind_out != NULL) {
            *kind_out = ATLAS_UPSERT_ADDED;
        }
        return ATLAS_OK;
    }

    if (unchanged) {
        sqlite3_stmt *upd = NULL;
        st = atlas_db_prepare(
            db, "UPDATE files SET last_seen_scan_id=?1, last_seen_at=?2 WHERE id=?3;", &upd, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(upd, 1, scan_id) != SQLITE_OK) {
            sqlite3_finalize(upd);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
        }
        st = atlas_db_bind_text_opt(db, upd, 2, now, err);
        if (st == ATLAS_OK && sqlite3_bind_int64(upd, 3, existing_id) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
        }
        if (st != ATLAS_OK) {
            sqlite3_finalize(upd);
            return st;
        }
        return atlas_db_step_done(db, upd, err);
    }

    sqlite3_stmt *upd = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE files SET file_type=?1, language=?2, git_mode=?3,"
                          " git_index_oid=?4, content_hash=?5, content_hash_algo=?6,"
                          " size_bytes=?7, is_executable=?8, is_symlink=?9, unsafe_path=?10,"
                          " read_error=?11, last_seen_scan_id=?12, last_seen_at=?13,"
                          " deleted=0, deleted_at=NULL, deleted_scan_id=NULL,"
                          " path_text=?14, path_is_utf8=?15 WHERE id=?16;",
                          &upd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_file_fields(db, upd, 1, rec, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(upd, 12, scan_id) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, upd, 13, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, upd, 14, rec->path_text, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int(upd, 15, rec->path_is_utf8 ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind path encoding flag");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(upd, 16, existing_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(upd);
        return st;
    }
    st = atlas_db_step_done(db, upd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (kind_out != NULL) {
        /* A path that had been recorded as deleted and is tracked again counts as
         * an addition, which is what a user sees. */
        *kind_out = was_deleted ? ATLAS_UPSERT_ADDED : ATLAS_UPSERT_MODIFIED;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_files_mark_deleted(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                         int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE files SET deleted=1, deleted_at=?1,"
                                       " deleted_scan_id=?2 WHERE repo_id=?3 AND deleted=0"
                                       " AND last_seen_scan_id<>?2;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, now, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(stmt, 2, scan_id) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 3, repo_id) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind ids");
        }
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (count_out != NULL) {
        *count_out = sqlite3_changes(db->h);
    }
    return ATLAS_OK;
}

atlas_status atlas_db_file_get(atlas_db *db, int64_t repo_id, const void *path_raw, size_t path_len,
                               atlas_file_row_cb cb, void *ud, bool *found, atlas_err *err) {
    *found = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT " FILE_COLS " FROM files WHERE repo_id=?1 AND path_raw=?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        atlas_file_row row;
        load_file_row(stmt, &row);
        *found = true;
        if (cb != NULL) {
            st = cb(&row, ud, err);
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read file row");
    }
    sqlite3_finalize(stmt);
    return st;
}

atlas_status atlas_db_file_history(atlas_db *db, int64_t repo_id, const void *path_raw,
                                   size_t path_len, int64_t limit, atlas_history_cb cb, void *ud,
                                   int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    /* A rename is visible from both sides, so the path matches either column. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT c.oid, c.author_name, c.author_email, c.author_time, c.commit_time, c.subject,"
        " fc.change_type, fc.path_text, fc.old_path_text, fc.score"
        " FROM file_changes fc JOIN commits c ON c.id = fc.commit_id"
        " WHERE fc.repo_id=?1 AND (fc.path_raw=?2 OR fc.old_path_raw=?2)"
        /* git log walks newest first, so within one scan a lower commit id is the
         * newer commit; that is the tiebreak when timestamps are identical. */
        " ORDER BY c.commit_time DESC, c.id ASC LIMIT ?3;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(stmt, 3, limit > 0 ? limit : -1) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind limit");
        }
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }

    int64_t n = 0;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read history row");
            break;
        }
        atlas_history_row row;
        memset(&row, 0, sizeof(row));
        row.commit_oid = atlas_db_col_text(stmt, 0);
        row.author_name = atlas_db_col_text(stmt, 1);
        row.author_email = atlas_db_col_text(stmt, 2);
        row.author_time = sqlite3_column_int64(stmt, 3);
        row.commit_time = sqlite3_column_int64(stmt, 4);
        row.subject = atlas_db_col_text(stmt, 5);
        row.change_type = atlas_db_col_text(stmt, 6);
        row.path_text = atlas_db_col_text(stmt, 7);
        row.old_path_text = atlas_db_col_text_opt(stmt, 8);
        row.score_known = sqlite3_column_type(stmt, 9) != SQLITE_NULL;
        row.score = row.score_known ? sqlite3_column_int(stmt, 9) : 0;
        n++;
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    if (st == ATLAS_OK && count_out != NULL) {
        *count_out = n;
    }
    return st;
}

/* --- commits and changes ------------------------------------------------- */

atlas_status atlas_db_commit_upsert(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                    const atlas_commit_record *rec, int64_t *commit_id_out,
                                    bool *inserted_out, atlas_err *err) {
    if (commit_id_out != NULL) {
        *commit_id_out = 0;
    }
    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO commits(repo_id, oid, parents, parent_count, author_name,"
                         " author_email, author_time, commit_time, subject, body, ingested_scan_id)"
                         " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
                         " ON CONFLICT(repo_id, oid) DO NOTHING;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, rec->oid, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, rec->parents != NULL ? rec->parents : "", err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int(stmt, 4, rec->parent_count) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind parent count");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, rec->author_name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->author_email, err);
    }
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(stmt, 7, rec->author_time) != SQLITE_OK ||
            sqlite3_bind_int64(stmt, 8, rec->commit_time) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind commit timestamps");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, rec->subject, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 10, rec->body != NULL ? rec->body : "", rec->body_len,
                                  err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 11, scan_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool inserted = sqlite3_changes(db->h) > 0;
    if (inserted_out != NULL) {
        *inserted_out = inserted;
    }

    if (commit_id_out != NULL) {
        if (inserted) {
            *commit_id_out = sqlite3_last_insert_rowid(db->h);
        } else {
            sqlite3_stmt *sel = NULL;
            st = atlas_db_prepare(db, "SELECT id FROM commits WHERE repo_id=?1 AND oid=?2;", &sel,
                                  err);
            if (st != ATLAS_OK) {
                return st;
            }
            if (sqlite3_bind_int64(sel, 1, repo_id) != SQLITE_OK) {
                sqlite3_finalize(sel);
                return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
            }
            st = atlas_db_bind_text_opt(db, sel, 2, rec->oid, err);
            if (st != ATLAS_OK) {
                sqlite3_finalize(sel);
                return st;
            }
            int rc = sqlite3_step(sel);
            if (rc == SQLITE_ROW) {
                *commit_id_out = sqlite3_column_int64(sel, 0);
            } else if (rc != SQLITE_DONE) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look up commit");
            }
            sqlite3_finalize(sel);
        }
    }
    return st;
}

atlas_status atlas_db_change_insert(atlas_db *db, int64_t repo_id, int64_t commit_id,
                                    const atlas_change_record *rec, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "INSERT INTO file_changes(repo_id, commit_id, change_type,"
                                       " score, path_raw, path_text, old_path_raw, old_path_text,"
                                       " raw_status) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, commit_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind ids");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, rec->change_type, err);
    if (st == ATLAS_OK) {
        int rc = rec->score_known ? sqlite3_bind_int(stmt, 4, rec->score)
                                  : sqlite3_bind_null(stmt, 4);
        if (rc != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind similarity score");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 5, rec->path_raw, rec->path_raw_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = (rec->old_path_raw != NULL)
                 ? atlas_db_bind_blob(db, stmt, 7, rec->old_path_raw, rec->old_path_raw_len, err)
                 : atlas_db_bind_text_opt(db, stmt, 7, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, rec->old_path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, rec->raw_status, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_changes_clear_for_commit(atlas_db *db, int64_t commit_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "DELETE FROM file_changes WHERE commit_id=?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, commit_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind commit id");
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- compile databases and evidence ------------------------------------- */

atlas_status atlas_db_compile_db_upsert(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                        const atlas_compile_db_record *rec, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO compile_databases(repo_id, path_raw, path_text,"
                         " is_regular_file, is_symlink, content_hash, size_bytes, scan_id, seen_at,"
                         " parsed) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,0)"
                         " ON CONFLICT(repo_id, path_raw) DO UPDATE SET"
                         " is_regular_file=excluded.is_regular_file,"
                         " is_symlink=excluded.is_symlink, content_hash=excluded.content_hash,"
                         " size_bytes=excluded.size_bytes, scan_id=excluded.scan_id,"
                         " seen_at=excluded.seen_at;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, rec->path_raw, rec->path_raw_len, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, rec->path_text, err);
    }
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int(stmt, 4, rec->is_regular_file ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int(stmt, 5, rec->is_symlink ? 1 : 0) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind compile database flags");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->content_hash, err);
    }
    if (st == ATLAS_OK) {
        int rc = rec->size_known ? sqlite3_bind_int64(stmt, 7, rec->size_bytes)
                                 : sqlite3_bind_null(stmt, 7);
        if (rc != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind size");
        }
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 8, scan_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_evidence_insert(atlas_db *db, int64_t repo_id, atlas_evidence_kind kind,
                                      int64_t scan_id, const char *git_oid, const void *path_raw,
                                      size_t path_len, const char *path_text,
                                      const char *commit_oid, const char *detail, atlas_err *err) {
    if (kind != ATLAS_EV_SOURCE && kind != ATLAS_EV_GIT) {
        /* A0 must not be able to write inferred or claimed evidence, even by
         * accident: the restriction is enforced here, not only by convention. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                            "A0 may only record SOURCE and GIT evidence, refused %s",
                            atlas_evidence_kind_name(kind));
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "INSERT INTO evidence(repo_id, kind, scan_id, git_oid,"
                                       " path_raw, path_text, commit_oid, detail, created_at)"
                                       " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, atlas_evidence_kind_name(kind), err);
    if (st == ATLAS_OK) {
        int rc = (scan_id > 0) ? sqlite3_bind_int64(stmt, 3, scan_id) : sqlite3_bind_null(stmt, 3);
        if (rc != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, git_oid, err);
    }
    if (st == ATLAS_OK) {
        st = (path_raw != NULL) ? atlas_db_bind_blob(db, stmt, 5, path_raw, path_len, err)
                                : atlas_db_bind_text_opt(db, stmt, 5, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, commit_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, now, err);
    }
    if (st != ATLAS_OK) {
        sqlite3_finalize(stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

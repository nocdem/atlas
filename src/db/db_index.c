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
    " deleted, deleted_at, tracked, ignored, truncated, truncated_reason, last_generation"

void atlas_file_record_init(atlas_file_record *rec) {
    memset(rec, 0, sizeof(*rec));
    /* Tracked is the overwhelmingly common case and the A0-compatible default.
     * Zeroing the struct would quietly mark every file untracked, which would
     * corrupt the meaning of the column rather than fail loudly. */
    rec->tracked = true;
}

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
    row->tracked = sqlite3_column_int(stmt, 21) != 0;
    row->ignored = sqlite3_column_int(stmt, 22) != 0;
    row->truncated = sqlite3_column_int(stmt, 23) != 0;
    row->truncated_reason = atlas_db_col_text_opt(stmt, 24);
    row->last_generation = sqlite3_column_int64(stmt, 25);
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
    /* A1 facts are part of what "unchanged" means. A file that stops being
     * ignored, or whose content stopped being truncated, has changed even when
     * its hash is identical, and a caller that filtered on those fields would
     * otherwise never be told. `last_generation` is deliberately excluded: like
     * the scan ids, it moves on every pass. */
    if (old->tracked != rec->tracked || old->ignored != rec->ignored ||
        old->truncated != rec->truncated) {
        return false;
    }
    if (!str_eq_opt(old->truncated_reason, rec->truncated_reason)) {
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
    st = atlas_db_bind_text_opt(db, stmt, base + 10, rec->read_error, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A1: discovery classification. */
    if (sqlite3_bind_int(stmt, base + 11, rec->tracked ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, base + 12, rec->ignored ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, base + 13, rec->truncated ? 1 : 0) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind discovery flags");
    }
    st = atlas_db_bind_text_opt(db, stmt, base + 14, rec->truncated_reason, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, base + 15, rec->generation) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation");
    }

    /* A1: filesystem identity. Bound as a unit: a partially recorded identity
     * would compare unequal forever and rehash the file on every pass, so an
     * unknown identity is stored as all-NULL instead. */
    const int64_t fs[ATLAS_FS_IDENTITY_COLUMNS] = {
        rec->fs.dev,        rec->fs.ino,       rec->fs.size,       rec->fs.mtime_sec,
        rec->fs.mtime_nsec, rec->fs.ctime_sec, rec->fs.ctime_nsec, rec->fs.mode,
    };
    for (int i = 0; i < ATLAS_FS_IDENTITY_COLUMNS; i++) {
        int idx = base + 16 + i;
        int rc2 = rec->fs.known ? sqlite3_bind_int64(stmt, idx, fs[i])
                                : sqlite3_bind_null(stmt, idx);
        if (rc2 != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind filesystem identity");
        }
    }
    return ATLAS_OK;
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
        atlas_db_finish(db, sel);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, sel, 2, rec->path_raw, rec->path_raw_len, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, sel);
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
        atlas_db_finish(db, sel);
        return st;
    }
    /* Short-circuit: `old` is filled by `load_file_row` only when `exists`, and
     * `&&` does not evaluate the right operand otherwise. cppcheck reports this
     * as an uninitialised read (`uninitStructMember`) because it does not model
     * the short circuit here; the suppression is inline and narrow so that a
     * future edit which *does* read `old` unconditionally is still reported.
     * The alternative — zeroing a struct that is about to be overwritten from a
     * statement row — would hide exactly that mistake. */
    /* cppcheck-suppress uninitStructMember */
    bool was_deleted = exists && old.deleted;
    atlas_db_finish(db, sel); /* borrowed pointers in `old` die here */

    if (!exists) {
        sqlite3_stmt *ins = NULL;
        st = atlas_db_prepare(db,
                              "INSERT INTO files(repo_id, path_raw, path_text, path_is_utf8,"
                              " file_type, language, git_mode, git_index_oid, content_hash,"
                              " content_hash_algo, size_bytes, is_executable, is_symlink,"
                              " unsafe_path, read_error, tracked, ignored, truncated,"
                              " truncated_reason, last_generation, fs_dev, fs_ino, fs_size,"
                              " fs_mtime_sec, fs_mtime_nsec, fs_ctime_sec, fs_ctime_nsec,"
                              " fs_mode, first_seen_scan_id,"
                              " last_seen_scan_id, first_seen_at, last_seen_at, deleted)"
                              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,"
                              "?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,"
                              "?31,?32,0);",
                              &ins, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(ins, 1, repo_id) != SQLITE_OK) {
            atlas_db_finish(db, ins);
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
            if (sqlite3_bind_int64(ins, 29, scan_id) != SQLITE_OK ||
                sqlite3_bind_int64(ins, 30, scan_id) != SQLITE_OK) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, ins, 31, now, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, ins, 32, now, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, ins);
            return st;
        }
        st = atlas_db_step_done(db, ins, err);
        if (st != ATLAS_OK) {
            return st;
        }
        /* Maintain the search index for exactly this row. A full rebuild here
         * would make every single-file change cost a whole repository reindex,
         * which is the thing A1 exists to avoid. `path_text` is a deterministic
         * function of `path_raw`, and `path_raw` is the key, so an update can
         * never change the indexed text: only the insert needs to touch FTS. */
        st = atlas_db_fts_file_upsert(db, sqlite3_last_insert_rowid(db->h), NULL, rec->path_text,
                                      err);
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
        /* Liveness bookkeeping only. The filesystem identity is refreshed too, so
         * that a file whose row predates A1 stops being a rehash candidate after
         * exactly one pass instead of being read again forever. */
        st = atlas_db_prepare(db,
                              "UPDATE files SET last_seen_scan_id=?1, last_seen_at=?2,"
                              " last_generation=?4, fs_dev=?5, fs_ino=?6, fs_size=?7,"
                              " fs_mtime_sec=?8, fs_mtime_nsec=?9, fs_ctime_sec=?10,"
                              " fs_ctime_nsec=?11, fs_mode=?12 WHERE id=?3;",
                              &upd, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(upd, 1, scan_id) != SQLITE_OK) {
            atlas_db_finish(db, upd);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
        }
        st = atlas_db_bind_text_opt(db, upd, 2, now, err);
        if (st == ATLAS_OK && (sqlite3_bind_int64(upd, 3, existing_id) != SQLITE_OK ||
                               sqlite3_bind_int64(upd, 4, rec->generation) != SQLITE_OK)) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
        }
        if (st == ATLAS_OK) {
            const int64_t fsv[ATLAS_FS_IDENTITY_COLUMNS] = {
                rec->fs.dev,        rec->fs.ino,       rec->fs.size,       rec->fs.mtime_sec,
                rec->fs.mtime_nsec, rec->fs.ctime_sec, rec->fs.ctime_nsec, rec->fs.mode,
            };
            for (int i = 0; i < ATLAS_FS_IDENTITY_COLUMNS && st == ATLAS_OK; i++) {
                int rc2 = rec->fs.known ? sqlite3_bind_int64(upd, 5 + i, fsv[i])
                                        : sqlite3_bind_null(upd, 5 + i);
                if (rc2 != SQLITE_OK) {
                    st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind filesystem identity");
                }
            }
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, upd);
            return st;
        }
        return atlas_db_step_done(db, upd, err);
    }

    sqlite3_stmt *upd = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE files SET file_type=?1, language=?2, git_mode=?3,"
                          " git_index_oid=?4, content_hash=?5, content_hash_algo=?6,"
                          " size_bytes=?7, is_executable=?8, is_symlink=?9, unsafe_path=?10,"
                          " read_error=?11, tracked=?12, ignored=?13, truncated=?14,"
                          " truncated_reason=?15, last_generation=?16, fs_dev=?17, fs_ino=?18,"
                          " fs_size=?19, fs_mtime_sec=?20, fs_mtime_nsec=?21, fs_ctime_sec=?22,"
                          " fs_ctime_nsec=?23, fs_mode=?24,"
                          " last_seen_scan_id=?25, last_seen_at=?26,"
                          " deleted=0, deleted_at=NULL, deleted_scan_id=NULL,"
                          " path_text=?27, path_is_utf8=?28 WHERE id=?29;",
                          &upd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_file_fields(db, upd, 1, rec, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(upd, 25, scan_id) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, upd, 26, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, upd, 27, rec->path_text, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int(upd, 28, rec->path_is_utf8 ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind path encoding flag");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(upd, 29, existing_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file id");
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, upd);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
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
    atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st == ATLAS_OK) {
        if (sqlite3_bind_int64(stmt, 3, limit > 0 ? limit : -1) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind limit");
        }
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
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
    atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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

    /* Captured before any other insert runs: sqlite3_last_insert_rowid() is
     * per-connection, and the FTS write below would otherwise overwrite it. */
    int64_t new_id = inserted ? sqlite3_last_insert_rowid(db->h) : 0;
    if (inserted) {
        /* Same reasoning as for files: index this commit, do not rebuild. */
        st = atlas_db_fts_commit_insert(db, new_id, rec->subject, rec->body, rec->body_len, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    if (commit_id_out != NULL) {
        if (inserted) {
            *commit_id_out = new_id;
        } else {
            sqlite3_stmt *sel = NULL;
            st = atlas_db_prepare(db, "SELECT id FROM commits WHERE repo_id=?1 AND oid=?2;", &sel,
                                  err);
            if (st != ATLAS_OK) {
                return st;
            }
            if (sqlite3_bind_int64(sel, 1, repo_id) != SQLITE_OK) {
                atlas_db_finish(db, sel);
                return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
            }
            st = atlas_db_bind_text_opt(db, sel, 2, rec->oid, err);
            if (st != ATLAS_OK) {
                atlas_db_finish(db, sel);
                return st;
            }
            int rc = sqlite3_step(sel);
            if (rc == SQLITE_ROW) {
                *commit_id_out = sqlite3_column_int64(sel, 0);
            } else if (rc != SQLITE_DONE) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look up commit");
            }
            atlas_db_finish(db, sel);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
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
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

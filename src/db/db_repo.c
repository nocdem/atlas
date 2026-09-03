/* Atlas - repository and scan bookkeeping.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "db/db_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/orch_ops.h"
#include "atlas/sem_ops.h"
#include "atlas/pathrep.h"

/* Column order shared by every repository query. */
#define REPO_COLS                                                                       \
    "id, name, root_path, root_path_text, git_common_dir, object_format, registered_at," \
    " last_scan_at, last_scan_id, scanned_head, current_branch, head_state, dirty,"      \
    " dirty_staged, dirty_unstaged, dirty_untracked, dirty_unmerged" \
    ", git_dir, is_linked_worktree, scanner_uid"                                         \
    ", mirror_complete, mirror_at"

void atlas_repo_info_init(atlas_repo_info *ri) {
    memset(ri, 0, sizeof(*ri));
    atlas_buf_init(&ri->root_path);
    atlas_buf_init(&ri->root_path_text);
    atlas_buf_init(&ri->git_common_dir);
    atlas_buf_init(&ri->git_dir);
    (void)snprintf(ri->object_format, sizeof(ri->object_format), "unknown");
    (void)snprintf(ri->head_state, sizeof(ri->head_state), "unknown");
}

void atlas_repo_info_free(atlas_repo_info *ri) {
    if (ri == NULL) {
        return;
    }
    atlas_buf_free(&ri->root_path);
    atlas_buf_free(&ri->root_path_text);
    atlas_buf_free(&ri->git_common_dir);
    atlas_buf_free(&ri->git_dir);
}

atlas_status atlas_db_check_repo_name(const char *name, atlas_err *err) {
    if (name == NULL || name[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "repository name is empty");
    }
    size_t n = strlen(name);
    if (n > ATLAS_NAME_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "repository name exceeds %u bytes",
                             ATLAS_NAME_MAX);
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "repository name may only contain letters, digits, '.', '_' and "
                                 "'-' (offending byte at offset %zu)",
                                 i);
        }
    }
    if (name[0] == '-' || name[0] == '.') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "repository name must not start with '%c'",
                             name[0]);
    }
    return ATLAS_OK;
}

static atlas_status load_repo(sqlite3_stmt *stmt, atlas_repo_info *ri, atlas_err *err) {
    ri->id = sqlite3_column_int64(stmt, 0);
    atlas_status st = atlas_db_col_copy(stmt, 1, ri->name, sizeof(ri->name), "repository name", err);
    if (st != ATLAS_OK) {
        return st;
    }

    const void *root = sqlite3_column_blob(stmt, 2);
    int root_len = sqlite3_column_bytes(stmt, 2);
    st = atlas_buf_set(&ri->root_path, root, root_len > 0 ? (size_t)root_len : 0u, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_buf_set_str(&ri->root_path_text, atlas_db_col_text(stmt, 3), err);
    if (st != ATLAS_OK) {
        return st;
    }
    const void *cdir = sqlite3_column_blob(stmt, 4);
    int cdir_len = sqlite3_column_bytes(stmt, 4);
    st = atlas_buf_set(&ri->git_common_dir, cdir, cdir_len > 0 ? (size_t)cdir_len : 0u, err);
    if (st != ATLAS_OK) {
        return st;
    }

    st = atlas_db_col_copy(stmt, 5, ri->object_format, sizeof(ri->object_format), "object format",
                           err);
    if (st == ATLAS_OK) {
        st = atlas_db_col_copy(stmt, 6, ri->registered_at, sizeof(ri->registered_at),
                               "registration timestamp", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_col_copy(stmt, 7, ri->last_scan_at, sizeof(ri->last_scan_at),
                               "last scan timestamp", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_col_copy(stmt, 9, ri->scanned_head, sizeof(ri->scanned_head), "scanned head",
                               err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_col_copy(stmt, 10, ri->current_branch, sizeof(ri->current_branch), "branch",
                               err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_col_copy(stmt, 11, ri->head_state, sizeof(ri->head_state), "head state", err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    const void *gdir = sqlite3_column_blob(stmt, 17);
    int gdir_len = sqlite3_column_bytes(stmt, 17);
    st = atlas_buf_set(&ri->git_dir, gdir, gdir_len > 0 ? (size_t)gdir_len : 0u, err);
    if (st != ATLAS_OK) {
        return st;
    }

    ri->last_scan_id = sqlite3_column_int64(stmt, 8);
    ri->dirty = sqlite3_column_int(stmt, 12) != 0;
    ri->dirty_staged = sqlite3_column_int(stmt, 13);
    ri->dirty_unstaged = sqlite3_column_int(stmt, 14);
    ri->dirty_untracked = sqlite3_column_int(stmt, 15);
    ri->dirty_unmerged = sqlite3_column_int(stmt, 16);
    ri->is_linked_worktree = sqlite3_column_int(stmt, 18) != 0;
    ri->scanner_uid = sqlite3_column_int64(stmt, 19);
    ri->mirror_complete = sqlite3_column_int(stmt, 20) != 0;
    {
        const unsigned char *t = sqlite3_column_text(stmt, 21);
        (void)snprintf(ri->mirror_at, sizeof(ri->mirror_at), "%s", t != NULL ? (const char *)t : "");
    }
    return ATLAS_OK;
}

atlas_status atlas_db_repo_add(atlas_db *db, const char *name, const atlas_repo_identity *ident,
                               int64_t *id_out, atlas_err *err) {
    if (id_out != NULL) {
        *id_out = 0;
    }
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_buf root_text = ATLAS_BUF_INIT;
    atlas_buf cdir_text = ATLAS_BUF_INIT;
    atlas_buf gdir_text = ATLAS_BUF_INIT;
    st = atlas_path_text_encode(ident->root, ident->root_len, &root_text, err);
    if (st == ATLAS_OK) {
        st = atlas_path_text_encode(ident->common_dir, ident->common_dir_len, &cdir_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_path_text_encode(ident->git_dir, ident->git_dir_len, &gdir_text, err);
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "INSERT INTO repositories(name, root_path, root_path_text,"
                          " git_common_dir, git_common_dir_text, object_format, registered_at,"
                          " git_dir, git_dir_text, is_linked_worktree)"
                          " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);",
                          &stmt, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, name, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 2, ident->root, ident->root_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 3, atlas_buf_cstr(&root_text), root_text.len, err);
    }
    if (st == ATLAS_OK) {
        st = (ident->common_dir_len > 0)
                 ? atlas_db_bind_blob(db, stmt, 4, ident->common_dir, ident->common_dir_len, err)
                 : atlas_db_bind_text_opt(db, stmt, 4, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = (cdir_text.len > 0)
                 ? atlas_db_bind_text_n(db, stmt, 5, atlas_buf_cstr(&cdir_text), cdir_text.len, err)
                 : atlas_db_bind_text_opt(db, stmt, 5, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(
            db, stmt, 6, ident->object_format != NULL ? ident->object_format : "unknown", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, now, err);
    }
    if (st == ATLAS_OK) {
        st = (ident->git_dir_len > 0)
                 ? atlas_db_bind_blob(db, stmt, 8, ident->git_dir, ident->git_dir_len, err)
                 : atlas_db_bind_text_opt(db, stmt, 8, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = (gdir_text.len > 0)
                 ? atlas_db_bind_text_n(db, stmt, 9, atlas_buf_cstr(&gdir_text), gdir_text.len, err)
                 : atlas_db_bind_text_opt(db, stmt, 9, NULL, err);
    }
    if (st == ATLAS_OK &&
        sqlite3_bind_int(stmt, 10, ident->is_linked_worktree ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind worktree flag");
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        goto done;
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        int ext = sqlite3_extended_errcode(db->h);
        if (ext == SQLITE_CONSTRAINT_UNIQUE || ext == SQLITE_CONSTRAINT_PRIMARYKEY) {
            /* Report which uniqueness was violated: the name or the root. */
            const char *msg = sqlite3_errmsg(db->h);
            if (msg != NULL && strstr(msg, "root_path") != NULL) {
                st = atlas_err_set(err, ATLAS_ERR_REPO,
                                   "that repository root is already registered under another name");
            } else {
                st = atlas_err_set(err, ATLAS_ERR_REPO, "repository name \"%s\" is already in use",
                                   name);
            }
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot register repository");
        }
        atlas_db_finish(db, stmt);
        goto done;
    }
    atlas_db_finish(db, stmt);
    int64_t new_id = sqlite3_last_insert_rowid(db->h);
    if (id_out != NULL) {
        *id_out = new_id;
    }

    /* A4. Decision documents do not cascade from `repositories` — they are the
     * one canonical, non-rebuildable record in the index — so a new repository
     * row has to start with none of them attached.
     *
     * **Detach only, and unconditionally.** `repositories.id` is a rowid and
     * rowids are reused: remove the only repository and the next `repo add`
     * gets the same id, so without this an unrelated project would inherit the
     * previous one's approved decisions silently, in every list and count.
     *
     * Attaching is a separate step and deliberately *not* here. It needs the
     * repository's durable identity, which commits to the root commits Atlas
     * has ingested — and at registration Atlas has ingested none. So the attach
     * happens after a scan, where the lineage is knowable. Splitting it this
     * way makes the failure mode fail-closed by construction: forgetting the
     * attach can only leave decisions orphaned, which is visible and
     * recoverable, and never attach them to the wrong repository, which is
     * neither. */
    {
        int64_t detached = 0;
        st = atlas_db_decision_detach_repo(db, new_id, &detached, err);
        if (st != ATLAS_OK) {
            goto done;
        }
    }

done:
    atlas_buf_free(&root_text);
    atlas_buf_free(&cdir_text);
    atlas_buf_free(&gdir_text);
    return st;
}

atlas_status atlas_db_repo_siblings(atlas_db *db, int64_t repo_id, const void *common_dir,
                                    size_t common_dir_len, atlas_repo_cb cb, void *ud,
                                    int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (common_dir == NULL || common_dir_len == 0) {
        return ATLAS_OK; /* nothing to relate it to */
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT " REPO_COLS " FROM repositories"
                                       " WHERE git_common_dir = ?1 AND id <> ?2 ORDER BY name;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_blob(db, stmt, 1, common_dir, common_dir_len, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    int64_t n = 0;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot list sibling worktrees");
            break;
        }
        st = load_repo(stmt, &ri, err);
        if (st != ATLAS_OK) {
            break;
        }
        n++;
        if (cb != NULL) {
            st = cb(&ri, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_repo_info_free(&ri);
    atlas_db_finish(db, stmt);
    if (st == ATLAS_OK && count_out != NULL) {
        *count_out = n;
    }
    return st;
}

static atlas_status repo_get_one(atlas_db *db, const char *sql, const void *key, size_t key_len,
                                 bool key_is_blob, atlas_repo_info *out, bool *found,
                                 atlas_err *err) {
    *found = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = key_is_blob ? atlas_db_bind_blob(db, stmt, 1, key, key_len, err)
                     : atlas_db_bind_text_n(db, stmt, 1, (const char *)key, key_len, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        st = load_repo(stmt, out, err);
        if (st == ATLAS_OK) {
            *found = true;
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read repository");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_repo_get(atlas_db *db, const char *name, atlas_repo_info *out, bool *found,
                               atlas_err *err) {
    if (name == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no repository name given");
    }
    return repo_get_one(db, "SELECT " REPO_COLS " FROM repositories WHERE name = ?1;", name,
                        strlen(name), false, out, found, err);
}

atlas_status atlas_db_repo_get_by_root(atlas_db *db, const void *root_raw, size_t root_len,
                                       atlas_repo_info *out, bool *found, atlas_err *err) {
    return repo_get_one(db, "SELECT " REPO_COLS " FROM repositories WHERE root_path = ?1;",
                        root_raw, root_len, true, out, found, err);
}

atlas_status atlas_db_repo_get_containing(atlas_db *db, const void *path, size_t path_len,
                                          atlas_repo_info *out, bool *found_out, atlas_err *err) {
    *found_out = false;
    if (path == NULL || path_len == 0) {
        return ATLAS_OK;
    }

    /* Walked here rather than expressed as a prefix query on purpose. A LIKE or
     * a substring comparison on `root_path` would match "/srv/proj" against
     * "/srv/project", which is a different repository; walking component by
     * component compares whole paths and cannot. It is also longest-match, so a
     * linked worktree registered inside another repository's tree resolves to
     * itself. */
    atlas_buf probe = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_set(&probe, path, path_len, err);
    while (st == ATLAS_OK && probe.len > 0) {
        /* A trailing separator is not part of a canonical root. */
        while (probe.len > 1u && probe.data[probe.len - 1u] == '/') {
            probe.len--;
            probe.data[probe.len] = '\0';
        }
        st = atlas_db_repo_get_by_root(db, probe.data, probe.len, out, found_out, err);
        if (st != ATLAS_OK || *found_out) {
            break;
        }
        size_t cut = probe.len;
        while (cut > 0 && probe.data[cut - 1u] != '/') {
            cut--;
        }
        if (cut <= 1u) {
            /* No separator left, or only the leading one. "/" is never a
             * repository root Atlas will resolve to: matching it would make a
             * stray registration capture every path on the machine. */
            break;
        }
        probe.len = cut - 1u;
        probe.data[probe.len] = '\0';
    }
    atlas_buf_free(&probe);
    return st;
}

atlas_status atlas_db_repo_get_by_id(atlas_db *db, int64_t repo_id, atlas_repo_info *out,
                                     bool *found, atlas_err *err) {
    /* The daemon queues work by id, not by name: a repository that is renamed or
     * removed between queueing and running must resolve to nothing rather than
     * to whatever now holds that name. */
    *found = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT " REPO_COLS " FROM repositories WHERE id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        st = load_repo(stmt, out, err);
        if (st == ATLAS_OK) {
            *found = true;
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read repository");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_repo_set_scanner_uid(atlas_db *db, int64_t repo_id, int64_t uid,
                                           atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE repositories SET scanner_uid = ?1 WHERE id = ?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, uid) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_err_set(err, ATLAS_ERR_DB, "cannot bind the scanner uid");
    }
    return atlas_db_step_done(db, stmt, err);
}

/* A13. Records what a mirroring run claims about what it left behind.
 *
 * Called twice per repository per run: once at the start with `complete` false,
 * which is what makes a crash leave the mirror marked incomplete rather than
 * stale-complete; and once at the end with the run's own verdict. The
 * asymmetry is the point — false costs a refusal, true would cost a delete
 * sweep against a half-written tree, so the value that survives a failure is
 * the one that refuses.
 *
 * `at` is written only alongside a `complete` of true. A time on an incomplete
 * mirror would say when it was last *touched*, and every reader of it wants to
 * know when it was last *whole*. */
atlas_status atlas_db_repo_set_mirror_state(atlas_db *db, int64_t repo_id, bool complete,
                                            const char *at, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE repositories SET mirror_complete = ?1, mirror_at = ?2 WHERE id = ?3;", &stmt,
        err);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *when = complete && at != NULL ? at : "";
    if (sqlite3_bind_int(stmt, 1, complete ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, when, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_err_set(err, ATLAS_ERR_DB, "cannot bind the mirror state");
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_repo_scanner_uid(atlas_db *db, int64_t repo_id, int64_t *out,
                                       atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "SELECT scanner_uid FROM repositories WHERE id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_err_set(err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    /* A repository that does not exist reports 0, the same as one with no
     * assignment: both mean "no scanner may report about this", which is the
     * only question this function answers. */
    int64_t uid = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        uid = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    if (out != NULL) {
        *out = uid;
    }
    return ATLAS_OK;
}

/* A12.1 T14 fix round (migration 30). See the declaration in `include/atlas/
 * db.h` and the migration 30 comment for why this cursor lives on
 * `repositories` rather than inside a `memory_generations` row: it is
 * current state, `scanner_uid`/`mirror_complete`'s own shape, not a ledger
 * entry, and it must have a writer that runs whether or not a reconciliation
 * pass found anything to record. */
atlas_status atlas_db_repo_trailer_scan_high(atlas_db *db, int64_t repo_id, int64_t *out,
                                             atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT trailer_scan_high FROM repositories WHERE id = ?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    /* A repository that does not exist, or one that has never had a trailer
     * scan pass, both report 0 -- "start from the top of history" is the
     * correct answer either way. */
    int64_t high = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        high = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    if (out != NULL) {
        *out = high;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_repo_set_trailer_scan_high(atlas_db *db, int64_t repo_id, int64_t high,
                                                 atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE repositories SET trailer_scan_high = ?1 WHERE id = ?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, high) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_err_set(err, ATLAS_ERR_DB, "cannot bind the trailer scan cursor");
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_repo_list(atlas_db *db, atlas_repo_cb cb, void *ud, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT " REPO_COLS " FROM repositories ORDER BY name;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot list repositories");
            break;
        }
        st = load_repo(stmt, &ri, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (cb != NULL) {
            st = cb(&ri, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_repo_info_free(&ri);
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_repo_remove(atlas_db *db, const char *name, bool *removed, atlas_err *err) {
    *removed = false;
    /* Two statements that must both happen or neither.
     *
     * The A2 rows about to be cascaded away are named by
     * `decision_revisions.imported_from_ai_decision_id`, and A4 documents
     * deliberately do not cascade — so those pointers would survive their
     * targets and then be handed to somebody else when SQLite reuses the
     * rowids. They are cleared first, while the rows still exist to be selected,
     * and in the same transaction so a failure cannot leave valid pointers
     * cleared beside a repository that is still registered. */
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_decision_forget_legacy_origins(db, name, NULL, err);

    /* A8 job records hold `repo_id` as a soft reference and do not cascade, for
     * the same reason A4 documents do not: an FK would make `repo remove --yes`
     * destroy execution history. But `repositories.id` is a reused rowid, so the
     * pointer is cleared here — while the repository still exists to be named —
     * and in the same transaction as the delete. `repo_identity_hash` stays: it
     * is what the history is about. */
    if (st == ATLAS_OK) {
        atlas_repo_info ri;
        atlas_repo_info_init(&ri);
        bool found = false;
        st = atlas_db_repo_get(db, name, &ri, &found, err);
        if (st == ATLAS_OK && found) {
            st = atlas_db_orch_forget_repo(db, ri.id, err);
        }
        /* The semantic index holds `repo_id` the same way and for the same
         * reason, so it is cleared the same way. A8-CI's generations are derived
         * data — dropping them would lose nothing that cannot be rebuilt — but a
         * *stale pointer* is not a loss, it is a wrong answer: the next
         * repository to take this rowid would inherit an index describing
         * somebody else's code, and every symbol it returned would look
         * legitimate. That is the A4 defect, and it is not repeated. */
        if (st == ATLAS_OK && found) {
            st = atlas_db_sem_forget_repo(db, ri.id, err);
        }
        /* A9.2.3's build description is keyed on the same reused rowid, and it
         * is the row that decides whether the daemon runs a compiler. Left
         * behind, the next repository to take this rowid would inherit somebody
         * else's opt-in — so this one is *deleted* rather than zeroed: an
         * operator's statement about a repository that is gone describes
         * nothing, and there is no later question it could answer. */
        if (st == ATLAS_OK && found) {
            st = atlas_db_sem_config_forget_repo(db, ri.id, err);
        }
        /* A9.2.4's discovered candidates are keyed on the same reused rowid.
         * They are derived — another walk reproduces them — but a row left
         * behind would tell the next repository to take this rowid that Atlas
         * had discovered build inputs it never looked for, complete with a
         * discovery verdict nobody earned. */
        if (st == ATLAS_OK && found) {
            st = atlas_db_sem_inputs_forget(db, ri.id, err);
        }
        atlas_repo_info_free(&ri);
    }

    sqlite3_stmt *stmt = NULL;
    /* ON DELETE CASCADE removes scans, files, commits, changes, compile
     * databases, evidence and the A2 records. Nothing outside the Atlas database
     * is touched, and no A4 decision record is deleted. */
    if (st == ATLAS_OK) {
        st = atlas_db_prepare(db, "DELETE FROM repositories WHERE name = ?1;", &stmt, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 1, name, err);
        if (st != ATLAS_OK) {
            atlas_db_finish(db, stmt);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        return st;
    }
    *removed = sqlite3_changes(db->h) > 0;
    return atlas_db_commit(db, err);
}

atlas_status atlas_db_repo_counts(atlas_db *db, int64_t repo_id, atlas_repo_counts *out,
                                  atlas_err *err) {
    memset(out, 0, sizeof(*out));
    static const struct {
        const char *sql;
        size_t offset;
    } queries[] = {
        {"SELECT count(*) FROM files WHERE repo_id=?1 AND deleted=0;",
         offsetof(atlas_repo_counts, files_live)},
        {"SELECT count(*) FROM files WHERE repo_id=?1 AND deleted=1;",
         offsetof(atlas_repo_counts, files_deleted)},
        {"SELECT count(*) FROM commits WHERE repo_id=?1;", offsetof(atlas_repo_counts, commits)},
        {"SELECT count(*) FROM file_changes WHERE repo_id=?1;",
         offsetof(atlas_repo_counts, changes)},
        {"SELECT count(*) FROM scans WHERE repo_id=?1;", offsetof(atlas_repo_counts, scans)},
        {"SELECT count(*) FROM evidence WHERE repo_id=?1;", offsetof(atlas_repo_counts, evidence)},
        {"SELECT count(*) FROM compile_databases WHERE repo_id=?1;",
         offsetof(atlas_repo_counts, compile_databases)},
    };

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        atlas_status st = atlas_db_prepare(db, queries[i].sql, &stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
        }
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int64_t v = sqlite3_column_int64(stmt, 0);
            memcpy((char *)out + queries[i].offset, &v, sizeof(v));
        } else if (rc != SQLITE_DONE) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count rows");
        }
        atlas_db_finish(db, stmt);
    }
    return ATLAS_OK;
}

/* --- scans -------------------------------------------------------------- */

atlas_status atlas_db_scan_begin(atlas_db *db, int64_t repo_id, const atlas_scan_state *stt,
                                 int64_t *scan_id_out, atlas_err *err) {
    *scan_id_out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "INSERT INTO scans(repo_id, started_at, status, head_oid,"
                                       " head_state, branch, object_format, dirty)"
                                       " VALUES(?1, ?2, 'running', ?3, ?4, ?5, ?6, ?7);",
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
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, stt->head_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, stt->head_state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, stt->branch, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, stt->object_format, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int(stmt, 7, stt->dirty ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind dirty flag");
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    *scan_id_out = sqlite3_last_insert_rowid(db->h);
    return ATLAS_OK;
}

atlas_status atlas_db_scan_finish(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                  const char *status, const char *error_text, int64_t files_total,
                                  int64_t files_added, int64_t files_modified,
                                  int64_t files_deleted, int64_t files_unchanged,
                                  int64_t files_unreadable, int64_t commits_ingested,
                                  atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE scans SET finished_at=?1, status=?2, error=?3,"
                                       " files_total=?4, files_added=?5, files_modified=?6,"
                                       " files_deleted=?7, files_unchanged=?8,"
                                       " files_unreadable=?9, commits_ingested=?10"
                                       " WHERE id=?11 AND repo_id=?12;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, error_text, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    const int64_t vals[] = {files_total,     files_added,      files_modified, files_deleted,
                            files_unchanged, files_unreadable, commits_ingested};
    for (int i = 0; i < 7; i++) {
        if (sqlite3_bind_int64(stmt, 4 + i, vals[i]) != SQLITE_OK) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan counter");
        }
    }
    if (sqlite3_bind_int64(stmt, 11, scan_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 12, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
    }
    atlas_status done = atlas_db_step_done(db, stmt, err);
    if (done != ATLAS_OK || status == NULL || strcmp(status, "ok") != 0) {
        return done;
    }
    /* A4. A completed pass is the first moment a repository's durable identity
     * is knowable: it commits to the root commits, and those only exist in the
     * index once history has been ingested.
     *
     * So this is where an orphaned decision document can be reattached — never
     * at registration, where Atlas has read nothing and could only match on the
     * path. Here rather than in the two callers because it is one choke point
     * and two would eventually disagree. Only on a successful pass: a failed
     * one has proved nothing about which repository this is.
     *
     * A match is exact or there is none, so the normal outcome is zero and the
     * cost is one indexed query per completed pass. */
    int64_t relinked = 0;
    return atlas_db_decision_relink_after_ingest(db, repo_id, &relinked, err);
}

atlas_status atlas_db_repo_apply_scan(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                      const atlas_scan_state *stt, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "UPDATE repositories SET last_scan_at=?1, last_scan_id=?2,"
                         " scanned_head=?3, current_branch=?4, head_state=?5, object_format=?6,"
                         " dirty=?7, dirty_staged=?8, dirty_unstaged=?9, dirty_untracked=?10,"
                         " dirty_unmerged=?11 WHERE id=?12;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, now, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, scan_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind scan id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, stt->head_oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, stt->branch, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, stt->head_state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, stt->object_format, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    const int flags[] = {stt->dirty ? 1 : 0, stt->dirty_staged, stt->dirty_unstaged,
                         stt->dirty_untracked, stt->dirty_unmerged};
    for (int i = 0; i < 5; i++) {
        if (sqlite3_bind_int(stmt, 7 + i, flags[i]) != SQLITE_OK) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind dirty counter");
        }
    }
    if (sqlite3_bind_int64(stmt, 12, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    return atlas_db_step_done(db, stmt, err);
}

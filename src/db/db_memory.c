/* Atlas - A12.1: typed operations over migration 29's memory tables.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The reconciled-memory layer's database surface, and deliberately the only
 * one: this file's functions are the only production writers of the
 * `memory_*` tables — T17 greps `src/memory/` for INSERTs to prove it — for
 * the reason `db_verify.c` is the single write point over the verification
 * tables: a second path would bypass whatever rule the first one carries.
 * Migration 29 creates the tables and the retention scanner reads their
 * rows, which is every table's lot and no exception to the rule.
 *
 * T4 creates this file with exactly one function, the version lookup the
 * verification write point resolves a snapshot reference through. An external
 * memory source's absolute path can never pass `atlas_db_verify_file_hash`'s
 * index lookup, and §8's rule — Atlas computes the content identity itself
 * rather than trusting a supplied one — must survive that: so the evidence op
 * names a stored row by uid, and everything the evidence asserts about the
 * bytes is read back out of the row Atlas wrote when it read them. T8 extends
 * this file with the reconciliation pass's own typed operations.
 */
#include "atlas/memory.h"

#include <string.h>

#include "db_internal.h"

void atlas_memory_version_row_init(atlas_memory_version_row *r) {
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof *r);
    atlas_buf_init(&r->version_uid);
    atlas_buf_init(&r->commit_oid);
    atlas_buf_init(&r->content_sha256);
    atlas_buf_init(&r->path_text);
    atlas_buf_init(&r->observed_at);
}

void atlas_memory_version_row_free(atlas_memory_version_row *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->version_uid);
    atlas_buf_free(&r->commit_oid);
    atlas_buf_free(&r->content_sha256);
    atlas_buf_free(&r->path_text);
    atlas_buf_free(&r->observed_at);
    memset(r, 0, sizeof *r);
}

/* Column text into an owned buffer; a NULL column reads as empty, which every
 * buffer already is after `_init`. */
static atlas_status take_col(atlas_buf *out, sqlite3_stmt *stmt, int col, atlas_err *err) {
    const char *s = (const char *)sqlite3_column_text(stmt, col);
    return atlas_buf_set(out, s == NULL ? "" : s, s == NULL ? 0 : strlen(s), err);
}

atlas_status atlas_db_memory_version_by_uid(atlas_db *db, const char *uid,
                                            atlas_memory_version_row *out, bool *found_out,
                                            atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory version lookup to run");
    }
    /* The join is the point: the caller gets the source's stored path beside
     * the version's stored hash, so evidence about a snapshot never has to ask
     * a second question — or worse, take the missing half from the request. */
    static const char SQL[] =
        "SELECT v.id, v.source_id, s.repo_id, v.version_uid, v.commit_oid,"
        "  v.content_sha256, v.content_bytes, s.path_text, v.observed_at"
        " FROM memory_source_versions v"
        " JOIN memory_sources s ON s.id = v.source_id"
        " WHERE v.version_uid = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, uid, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        out->id = sqlite3_column_int64(stmt, 0);
        out->source_id = sqlite3_column_int64(stmt, 1);
        out->repo_id = sqlite3_column_int64(stmt, 2);
        st = take_col(&out->version_uid, stmt, 3, err);
        if (st == ATLAS_OK) {
            st = take_col(&out->commit_oid, stmt, 4, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->content_sha256, stmt, 5, err);
        }
        out->content_bytes = sqlite3_column_int64(stmt, 6);
        if (st == ATLAS_OK) {
            st = take_col(&out->path_text, stmt, 7, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->observed_at, stmt, 8, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

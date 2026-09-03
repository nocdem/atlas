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

#include "atlas/hmac.h"
#include "db_internal.h"

/* --- uids -------------------------------------------------------------------
 *
 * `db_verify.c`'s own shape and its own reason: 128 bits of kernel randomness,
 * hex, behind a fixed one-character prefix -- 'm' for a source, 'v' for a
 * version, matching migration 29's own comments. An identifier, not a secret;
 * nothing treats knowing one as authorisation. Fails rather than falling back
 * when the CSPRNG is unavailable, because a uid built from a predictable input
 * is one that collides. */
#define MEMORY_UID_BYTES 16u

static atlas_status memory_uid(const char *prefix, atlas_buf *out, atlas_err *err) {
    unsigned char raw[MEMORY_UID_BYTES];
    atlas_status st = atlas_random_bytes(raw, sizeof raw, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char hex[MEMORY_UID_BYTES * 2u + 1u];
    atlas_hex_encode_lower(raw, sizeof raw, hex);
    atlas_buf_reset(out);
    st = atlas_buf_append_str(out, prefix, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, hex, err);
    }
    return st;
}

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

atlas_status atlas_db_memory_source_by_uid(atlas_db *db, const char *uid, int64_t *id_out,
                                           int64_t *repo_id_out, atlas_memory_source_class *cls_out,
                                           atlas_buf *path_raw_out, atlas_buf *path_text_out,
                                           bool *found_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (id_out != NULL) {
        *id_out = 0;
    }
    if (repo_id_out != NULL) {
        *repo_id_out = 0;
    }
    if (cls_out != NULL) {
        *cls_out = ATLAS_MEMORY_SOURCE_UNKNOWN;
    }
    if (db == NULL || uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory source lookup to run");
    }
    static const char SQL[] =
        "SELECT id, repo_id, cls, path_raw, path_text FROM memory_sources"
        " WHERE source_uid = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, uid, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        if (id_out != NULL) {
            *id_out = sqlite3_column_int64(stmt, 0);
        }
        if (repo_id_out != NULL) {
            *repo_id_out = sqlite3_column_int64(stmt, 1);
        }
        if (cls_out != NULL) {
            const char *cn = (const char *)sqlite3_column_text(stmt, 2);
            if (!atlas_memory_source_class_parse(cn != NULL ? cn : "", cls_out)) {
                *cls_out = ATLAS_MEMORY_SOURCE_UNKNOWN;
            }
        }
        if (st == ATLAS_OK && path_raw_out != NULL) {
            const void *blob = sqlite3_column_blob(stmt, 3);
            int blen = sqlite3_column_bytes(stmt, 3);
            st = atlas_buf_set(path_raw_out, blob, blen > 0 ? (size_t)blen : 0u, err);
        }
        if (st == ATLAS_OK && path_text_out != NULL) {
            st = take_col(path_text_out, stmt, 4, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_memory_source_list(atlas_db *db, int64_t repo_id, atlas_memory_source_cb cb,
                                         void *ctx, atlas_err *err) {
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory source list to run");
    }
    static const char SQL[] =
        "SELECT id, source_uid, cls, path_text, registered_at FROM memory_sources"
        " WHERE repo_id = ?1 ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot list memory sources");
            break;
        }
        int64_t id = sqlite3_column_int64(stmt, 0);
        const char *uid = (const char *)sqlite3_column_text(stmt, 1);
        const char *cn = (const char *)sqlite3_column_text(stmt, 2);
        const char *path_text = (const char *)sqlite3_column_text(stmt, 3);
        const char *registered_at = (const char *)sqlite3_column_text(stmt, 4);
        atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_UNKNOWN;
        (void)atlas_memory_source_class_parse(cn != NULL ? cn : "", &cls);
        if (cb != NULL) {
            st = cb(id, uid != NULL ? uid : "", cls, path_text != NULL ? path_text : "",
                   registered_at != NULL ? registered_at : "", ctx, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- T8: the reconciliation pass's own typed operations --------------------
 *
 * Called only from inside the apply phase's transaction (`atlas_memory_apply_
 * in_tx`, `src/memory/reconcile.c`) or, for the two read-only lookups, from
 * the observe phase where a plain single-statement read is not the write
 * transaction A1 forbids around a file read or a git process. Nothing here
 * opens or closes a transaction of its own -- the caller already holds one,
 * or holds none and is only reading. */

atlas_status atlas_db_memory_source_find(atlas_db *db, int64_t repo_id, atlas_memory_source_class cls,
                                         const void *path_raw, size_t path_raw_len, int64_t *id_out,
                                         atlas_buf *uid_out, bool *found_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (id_out != NULL) {
        *id_out = 0;
    }
    if (db == NULL || path_raw == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory source lookup to run");
    }
    static const char SQL[] =
        "SELECT id, source_uid FROM memory_sources WHERE repo_id = ?1 AND cls = ?2"
        "  AND path_raw = ?3;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, atlas_memory_source_class_name(cls), err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 3, path_raw, path_raw_len, err);
    }
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        if (id_out != NULL) {
            *id_out = sqlite3_column_int64(stmt, 0);
        }
        if (uid_out != NULL) {
            st = take_col(uid_out, stmt, 1, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_memory_source_upsert(atlas_db *db, int64_t repo_id, atlas_memory_source_class cls,
                                           const void *path_raw, size_t path_raw_len,
                                           const char *path_text, const char *now, int64_t *id_out,
                                           atlas_buf *uid_out, atlas_err *err) {
    if (id_out != NULL) {
        *id_out = 0;
    }
    if (db == NULL || path_raw == NULL || path_text == NULL || now == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory source to materialise");
    }
    bool found = false;
    atlas_status st =
        atlas_db_memory_source_find(db, repo_id, cls, path_raw, path_raw_len, id_out, uid_out,
                                    &found, err);
    if (st != ATLAS_OK || found) {
        return st;
    }

    atlas_buf uid = ATLAS_BUF_INIT;
    st = memory_uid("m", &uid, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&uid);
        return st;
    }

    static const char SQL[] =
        "INSERT INTO memory_sources(repo_id, source_uid, cls, path_raw, path_text, registered_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6);";
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, atlas_buf_cstr(&uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_memory_source_class_name(cls), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, stmt, 4, path_raw, path_raw_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    if (st == ATLAS_OK) {
        if (id_out != NULL) {
            *id_out = sqlite3_last_insert_rowid(db->h);
        }
        if (uid_out != NULL) {
            st = atlas_buf_set(uid_out, uid.data, uid.len, err);
        }
    }
    atlas_buf_free(&uid);
    return st;
}

atlas_status atlas_db_memory_version_exists(atlas_db *db, int64_t source_id, const char *content_sha256,
                                            bool *found_out, int64_t *id_out, atlas_buf *uid_out,
                                            atlas_buf *observed_at_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (id_out != NULL) {
        *id_out = 0;
    }
    if (db == NULL || content_sha256 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory version to look for");
    }
    static const char SQL[] =
        "SELECT id, version_uid, observed_at FROM memory_source_versions"
        " WHERE source_id = ?1 AND content_sha256 = ?2 ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, source_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the source id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, content_sha256, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        if (id_out != NULL) {
            *id_out = sqlite3_column_int64(stmt, 0);
        }
        if (uid_out != NULL) {
            st = take_col(uid_out, stmt, 1, err);
        }
        if (st == ATLAS_OK && observed_at_out != NULL) {
            st = take_col(observed_at_out, stmt, 2, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_memory_version_insert(atlas_db *db, int64_t source_id, const char *commit_oid,
                                            const char *blob_oid, const char *content_sha256,
                                            int64_t content_bytes, const void *content,
                                            size_t content_len, const char *observed_at,
                                            const char *recorded_at, int64_t read_by_uid,
                                            int64_t *id_out, atlas_buf *uid_out, atlas_err *err) {
    if (id_out != NULL) {
        *id_out = 0;
    }
    if (db == NULL || content_sha256 == NULL || observed_at == NULL || recorded_at == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory version to record");
    }
    atlas_buf uid = ATLAS_BUF_INIT;
    atlas_status st = memory_uid("v", &uid, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&uid);
        return st;
    }

    static const char SQL[] =
        "INSERT INTO memory_source_versions(source_id, version_uid, commit_oid, blob_oid,"
        "  content_sha256, content_bytes, content, observed_at, recorded_at, read_by_uid)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, source_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the source id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, atlas_buf_cstr(&uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, commit_oid != NULL ? commit_oid : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, blob_oid != NULL ? blob_oid : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, content_sha256, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 6, content_bytes) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the content length");
    }
    if (st == ATLAS_OK) {
        /* Exactly the versions with no blob carry their own bytes -- the
         * migration's own CHECK. A git-tracked version's `content` is NULL:
         * git is canonical for it and duplicating the bytes here would be a
         * second copy nothing reads. */
        if (blob_oid != NULL && blob_oid[0] != '\0') {
            if (sqlite3_bind_null(stmt, 7) != SQLITE_OK) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind a null content column");
            }
        } else {
            st = atlas_db_bind_blob(db, stmt, 7, content, content_len, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, observed_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, recorded_at, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 10, read_by_uid) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the reading uid");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    if (st == ATLAS_OK) {
        if (id_out != NULL) {
            *id_out = sqlite3_last_insert_rowid(db->h);
        }
        if (uid_out != NULL) {
            st = atlas_buf_set(uid_out, uid.data, uid.len, err);
        }
    }
    atlas_buf_free(&uid);
    return st;
}

atlas_status atlas_db_memory_version_latest(atlas_db *db, int64_t source_id,
                                            atlas_memory_version_row *out, bool *found_out,
                                            atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory version lookup to run");
    }
    static const char SQL[] =
        "SELECT v.id, v.source_id, s.repo_id, v.version_uid, v.commit_oid,"
        "  v.content_sha256, v.content_bytes, s.path_text, v.observed_at, v.content"
        " FROM memory_source_versions v"
        " JOIN memory_sources s ON s.id = v.source_id"
        " WHERE v.source_id = ?1 ORDER BY v.id DESC LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, source_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the source id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
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
        if (st == ATLAS_OK) {
            const void *blob = sqlite3_column_blob(stmt, 9);
            int blob_len = sqlite3_column_bytes(stmt, 9);
            st = atlas_buf_set(&out->content, blob, blob_len > 0 ? (size_t)blob_len : 0u, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* T11 fix round (Important 1). Exactly `atlas_db_memory_version_latest`'s own
 * SELECT with `v.content` dropped from the column list -- a caller that never
 * reads the bytes must never pay to fetch and copy them. `out->content` is
 * left as `_init` left it (empty), matching `atlas_db_memory_version_by_uid`'s
 * own contract.
 *
 * Also closes T11 fix round Important 2's other half at the source: unlike
 * `atlas_db_memory_version_latest` above, a `sqlite3_step` result that is
 * neither `SQLITE_ROW` nor `SQLITE_DONE` is reported as the database failure
 * it is, rather than silently read as "no such version" -- the distinction
 * `memory.status`'s caller (`emit_source`, `src/ipc/server_memory.c`) depends
 * on to stop conflating a failed read with a genuine absence. */
atlas_status atlas_db_memory_version_latest_meta(atlas_db *db, int64_t source_id,
                                                 atlas_memory_version_row *out, bool *found_out,
                                                 atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory version lookup to run");
    }
    static const char SQL[] =
        "SELECT v.id, v.source_id, s.repo_id, v.version_uid, v.commit_oid,"
        "  v.content_sha256, v.content_bytes, s.path_text, v.observed_at"
        " FROM memory_source_versions v"
        " JOIN memory_sources s ON s.id = v.source_id"
        " WHERE v.source_id = ?1 ORDER BY v.id DESC LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, source_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the source id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
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
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the memory source's latest version");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_memory_anchor_add(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                        atlas_memory_anchor_kind kind, const char *value,
                                        atlas_err *err) {
    if (db == NULL || claim_uid == NULL || value == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory anchor to record");
    }
    static const char SQL[] =
        "INSERT OR IGNORE INTO memory_claim_anchors(repo_id, claim_uid, kind, value)"
        " VALUES(?1, ?2, ?3, ?4);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, claim_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_memory_anchor_kind_name(kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, value, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    return st;
}

atlas_status atlas_db_memory_anchor_prune_one(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                              atlas_memory_anchor_kind kind, const char *value,
                                              atlas_err *err) {
    if (db == NULL || claim_uid == NULL || value == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no claim anchor to prune");
    }
    static const char SQL[] =
        "DELETE FROM memory_claim_anchors"
        " WHERE repo_id = ?1 AND claim_uid = ?2 AND kind = ?3 AND value = ?4;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, claim_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_memory_anchor_kind_name(kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, value, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    return st;
}

atlas_status atlas_db_memory_unanchored_add(atlas_db *db, int64_t source_version_id, int64_t ordinal,
                                            const char *text_sha256, const void *text,
                                            size_t text_len, bool *landed_out, atlas_err *err) {
    if (landed_out != NULL) {
        *landed_out = false;
    }
    if (db == NULL || text_sha256 == NULL || (text == NULL && text_len > 0)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no unanchored candidate to record");
    }
    static const char SQL[] =
        "INSERT OR IGNORE INTO memory_unanchored(source_version_id, ordinal, text_sha256, text)"
        " VALUES(?1, ?2, ?3, ?4);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, source_version_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the source version id");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, ordinal) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the ordinal");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, text_sha256, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 4, (const char *)text, text_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
        /* `step_done` is OK on zero changed rows -- `INSERT OR IGNORE`
         * hitting the UNIQUE constraint is not an error, but it is not a new
         * row either, and the two must not read the same to a caller
         * counting how many landed. */
        if (st == ATLAS_OK && landed_out != NULL) {
            *landed_out = sqlite3_changes(db->h) > 0;
        }
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    return st;
}

atlas_status atlas_db_memory_generation_next(atlas_db *db, int64_t repo_id, int64_t *next_out,
                                             atlas_err *err) {
    if (next_out != NULL) {
        *next_out = 0;
    }
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no repository to derive a generation for");
    }
    static const char SQL[] =
        "SELECT COALESCE(MAX(generation), 0) + 1 FROM memory_generations WHERE repo_id = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW && next_out != NULL) {
        *next_out = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_memory_generation_insert(atlas_db *db, int64_t repo_id, int64_t generation,
                                               atlas_memory_gen_cause cause,
                                               const char *repo_identity_hash,
                                               const char *head_commit,
                                               const char *decision_set_digest,
                                               const char *source_set_digest,
                                               int64_t trailer_scan_high, const char *created_at,
                                               int64_t *id_out, atlas_err *err) {
    if (id_out != NULL) {
        *id_out = 0;
    }
    if (db == NULL || repo_identity_hash == NULL || created_at == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory generation to record");
    }
    static const char SQL[] =
        "INSERT INTO memory_generations(repo_id, generation, cause, repo_identity_hash,"
        "  head_commit, decision_set_digest, source_set_digest, trailer_scan_high, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, generation) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the generation number");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_memory_gen_cause_name(cause), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, repo_identity_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, head_commit != NULL ? head_commit : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6,
                                    decision_set_digest != NULL ? decision_set_digest : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7,
                                    source_set_digest != NULL ? source_set_digest : "", err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 8, trailer_scan_high) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the trailer scan high-water mark");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    if (st == ATLAS_OK && id_out != NULL) {
        *id_out = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

atlas_status atlas_db_memory_claim_diff_add(atlas_db *db, int64_t generation_id, const char *claim_uid,
                                            atlas_memory_diff_kind kind, const char *reason,
                                            atlas_err *err) {
    if (db == NULL || claim_uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory claim diff to record");
    }
    static const char SQL[] =
        "INSERT OR IGNORE INTO memory_claim_diffs(generation_id, claim_uid, kind, reason)"
        " VALUES(?1, ?2, ?3, ?4);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the generation id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, claim_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_memory_diff_kind_name(kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, reason != NULL ? reason : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    return st;
}

/* --- T9: cross-generation reads --------------------------------------------
 *
 * None of these touch a `verify_*` table -- they read the five `memory_*`
 * tables this file already owns. `src/memory/reconcile.c` is the one caller
 * that also reads `verify_claims`, and it does so through the existing public
 * `atlas_db_verify_claim_find`/`atlas_db_decision_*` reads, never through a
 * new function added here -- the same division T7's `atlas_memory_anchor_
 * resolve` already keeps. */

atlas_status atlas_db_memory_generation_latest(atlas_db *db, int64_t repo_id, int64_t *generation_out,
                                               atlas_buf *head_commit_out,
                                               atlas_buf *decision_set_digest_out,
                                               atlas_buf *source_set_digest_out, bool *found_out,
                                               atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (generation_out != NULL) {
        *generation_out = 0;
    }
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no repository to look a generation up for");
    }
    static const char SQL[] =
        "SELECT generation, head_commit, decision_set_digest, source_set_digest"
        " FROM memory_generations WHERE repo_id = ?1 ORDER BY generation DESC LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (generation_out != NULL) {
            *generation_out = sqlite3_column_int64(stmt, 0);
        }
        if (head_commit_out != NULL) {
            st = take_col(head_commit_out, stmt, 1, err);
        }
        if (st == ATLAS_OK && decision_set_digest_out != NULL) {
            st = take_col(decision_set_digest_out, stmt, 2, err);
        }
        if (st == ATLAS_OK && source_set_digest_out != NULL) {
            st = take_col(source_set_digest_out, stmt, 3, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* A12.1 T16. See the declaration in `memory.h` for the full contract. Two
 * statements rather than one join: `memory_generations` may exist with zero
 * `memory_claim_diffs` rows (a pass that recorded a source revision but had
 * nothing to say about any individual claim), and a join would make that
 * indistinguishable from "no such generation" -- both would return zero rows. */
atlas_status atlas_db_memory_generation_diffs_list(atlas_db *db, int64_t repo_id, int64_t generation,
                                                   atlas_memory_diff_row_cb cb, void *ctx,
                                                   bool *found_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || cb == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no generation diff listing to run");
    }
    static const char FIND[] =
        "SELECT id FROM memory_generations WHERE repo_id = ?1 AND generation = ?2;";
    sqlite3_stmt *fstmt = NULL;
    atlas_status st = atlas_db_prepare(db, FIND, &fstmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(fstmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(fstmt, 2, generation) != SQLITE_OK) {
        atlas_db_finish(db, fstmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the generation lookup");
    }
    int64_t generation_id = 0;
    int rc = sqlite3_step(fstmt);
    if (rc == SQLITE_ROW) {
        generation_id = sqlite3_column_int64(fstmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look up the generation");
    }
    atlas_db_finish(db, fstmt);
    if (st != ATLAS_OK) {
        return st;
    }
    if (generation_id == 0) {
        return ATLAS_OK; /* *found_out stays false */
    }
    if (found_out != NULL) {
        *found_out = true;
    }

    static const char SQL[] =
        "SELECT claim_uid, kind, reason FROM memory_claim_diffs"
        " WHERE generation_id = ?1 ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the generation id");
    }
    while (st == ATLAS_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *claim_uid = (const char *)sqlite3_column_text(stmt, 0);
        const char *kind_name = (const char *)sqlite3_column_text(stmt, 1);
        const char *reason = (const char *)sqlite3_column_text(stmt, 2);
        atlas_memory_diff_kind kind = ATLAS_MEMORY_DIFF_UNKNOWN;
        if (kind_name == NULL || !atlas_memory_diff_kind_parse(kind_name, &kind)) {
            /* A row this table's own CHECK constraint should make impossible.
             * Reported rather than silently skipped, so a corrupt row is a
             * finding and not a quietly shorter list. */
            st = atlas_err_set(err, ATLAS_ERR_DB,
                               "a stored claim diff carries an unrecognised kind");
            break;
        }
        st = cb(claim_uid != NULL ? claim_uid : "", kind, reason != NULL ? reason : "", ctx, err);
    }
    if (st == ATLAS_OK && rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the generation's claim diffs");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* A12.1 T16. See the declaration in `memory.h` for the full contract and the
 * four states `*checked_out`/`*complete_out`/`*claim_uids_out` together let a
 * caller tell apart. */
atlas_status atlas_db_memory_pack_reliance_get(atlas_db *db, const char *run_uid, bool *checked_out,
                                               bool *complete_out, atlas_buf *claim_uids_out,
                                               bool *found_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (checked_out != NULL) {
        *checked_out = false;
    }
    if (complete_out != NULL) {
        *complete_out = false;
    }
    if (claim_uids_out != NULL) {
        atlas_buf_reset(claim_uids_out);
    }
    if (db == NULL || run_uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no reliance lookup to run");
    }
    static const char SQL[] =
        "SELECT reliance_checked, reliance_complete, reliance_claim_uids"
        " FROM memory_context_packs WHERE run_uid = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, run_uid, err);
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (checked_out != NULL) {
                *checked_out = sqlite3_column_int64(stmt, 0) != 0;
            }
            if (complete_out != NULL) {
                *complete_out = sqlite3_column_int64(stmt, 1) != 0;
            }
            if (claim_uids_out != NULL) {
                st = take_col(claim_uids_out, stmt, 2, err);
            }
            if (st == ATLAS_OK && found_out != NULL) {
                *found_out = true;
            }
        } else if (rc != SQLITE_DONE) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the pack's reliance state");
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* T9 fix-round-1 added `ORDER BY kind, value` here so `compute_decision_set_
 * digest` (`src/memory/reconcile.c`) folds DECISION tuples in a stable order
 * -- without it, two runs over the exact same tuple set could concatenate
 * them in a different order and hash to a different digest with nothing
 * about a decision having actually moved.
 *
 * fix-round-2 (Minor, disclosed rather than silently accepted): a
 * repository whose `decision_set_digest` was computed and stored *before*
 * this ordering existed reads a digest built from whatever order SQLite
 * happened to return then; the first pass after upgrading to this ordering
 * recomputes the same tuple set in the now-guaranteed order and can get a
 * *different* digest string for an unchanged decision set, which
 * `determine_cause` reads as `DECISION_REVISION` once, spuriously. This is
 * not fixed here: reverting the ordering reopens the flip-flopping risk it
 * was added to close (a real, unbounded, per-pass risk) in exchange for
 * avoiding a one-time, self-correcting transition (the stored digest is
 * stable again from the very next pass, and the spurious cause produces a
 * real, harmless generation -- not a wrong diff, only an imprecise label
 * for one pass). Closing it correctly needs the digest format itself to be
 * versioned so an old digest can be recognised as old, which is a schema
 * question outside a fix round's four files. */
atlas_status atlas_db_memory_anchor_distinct(atlas_db *db, int64_t repo_id,
                                             atlas_memory_anchor_tuple_cb cb, void *ctx,
                                             atlas_err *err) {
    if (db == NULL || cb == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no anchor listing to run");
    }
    static const char SQL[] =
        "SELECT DISTINCT kind, value FROM memory_claim_anchors WHERE repo_id = ?1"
        " ORDER BY kind, value;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the anchor kinds");
            break;
        }
        const char *kind_name = (const char *)sqlite3_column_text(stmt, 0);
        const char *value = (const char *)sqlite3_column_text(stmt, 1);
        atlas_memory_anchor_kind kind = ATLAS_MEMORY_ANCHOR_UNKNOWN;
        if (kind_name == NULL || !atlas_memory_anchor_kind_parse(kind_name, &kind)) {
            continue; /* a row this build's vocabulary cannot name is skipped, not fabricated */
        }
        st = cb(kind, value != NULL ? value : "", ctx, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_memory_anchor_claim_uids(atlas_db *db, int64_t repo_id,
                                               atlas_memory_anchor_kind kind, const char *value,
                                               atlas_memory_claim_uid_cb cb, void *ctx,
                                               atlas_err *err) {
    if (db == NULL || value == NULL || cb == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no anchor claim listing to run");
    }
    static const char SQL[] =
        "SELECT claim_uid FROM memory_claim_anchors"
        " WHERE repo_id = ?1 AND kind = ?2 AND value = ?3 ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, atlas_memory_anchor_kind_name(kind), err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, value, err);
    }
    for (; st == ATLAS_OK;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the anchor's claim uids");
            break;
        }
        const char *uid = (const char *)sqlite3_column_text(stmt, 0);
        st = cb(uid != NULL ? uid : "", ctx, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* T12. `atlas_db_memory_anchor_claim_uids`' reverse: every anchor recorded
 * for one claim uid, ordered so two reads over an unchanged set agree byte
 * for byte. */
atlas_status atlas_db_memory_anchors_for_claim(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                               atlas_memory_anchor_tuple_cb cb, void *ctx,
                                               atlas_err *err) {
    if (db == NULL || claim_uid == NULL || cb == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no claim anchor listing to run");
    }
    static const char SQL[] =
        "SELECT kind, value FROM memory_claim_anchors"
        " WHERE repo_id = ?1 AND claim_uid = ?2 ORDER BY kind, value;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, claim_uid, err);
    for (; st == ATLAS_OK;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the claim's anchors");
            break;
        }
        const char *kind_name = (const char *)sqlite3_column_text(stmt, 0);
        const char *value = (const char *)sqlite3_column_text(stmt, 1);
        atlas_memory_anchor_kind kind = ATLAS_MEMORY_ANCHOR_UNKNOWN;
        if (kind_name == NULL || !atlas_memory_anchor_kind_parse(kind_name, &kind)) {
            continue; /* a row this build's vocabulary cannot name is skipped, not fabricated */
        }
        st = cb(kind, value != NULL ? value : "", ctx, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* T12. This repository's current total of `memory_unanchored` rows, joined
 * through the source version each belongs to -- every prior reader of this
 * table had a `source_version_id` in hand already; this is the first
 * repository-wide total. */
atlas_status atlas_db_memory_unanchored_count(atlas_db *db, int64_t repo_id, int64_t *count_out,
                                              atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (db == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no repository to count unanchored candidates for");
    }
    static const char SQL[] =
        "SELECT COUNT(*) FROM memory_unanchored u"
        " JOIN memory_source_versions v ON v.id = u.source_version_id"
        " JOIN memory_sources s ON s.id = v.source_id"
        " WHERE s.repo_id = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (count_out != NULL) {
            *count_out = sqlite3_column_int64(stmt, 0);
        }
    } else {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count unanchored candidates");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* T12. The one INSERT into `memory_context_packs` -- this file's own rule.
 * `UNIQUE(run_uid)` does the actual freezing; a second call for the same
 * run_uid fails on the constraint. `reliance_checked`, `reliance_complete`
 * and `reliance_claim_uids` take their column defaults, since T13's
 * completion-time reliance check has nothing to say yet about a run that has
 * not started. */
atlas_status atlas_db_memory_pack_insert(atlas_db *db, const char *run_uid,
                                         const atlas_memory_pack *p, const char *now,
                                         atlas_err *err) {
    if (db == NULL || run_uid == NULL || run_uid[0] == '\0' || p == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no run or pack to freeze");
    }
    static const char SQL[] =
        "INSERT INTO memory_context_packs(run_uid, repo_id, repo_identity_hash, pinned_commit,"
        "  source_identity, memory_generation, decision_set_digest, source_set_digest,"
        "  pack_digest, rendered, claim_count, excluded_count, unanchored_count,"
        "  claims_manifest, flagged_anchors, created_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, run_uid, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 2, p->repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_buf_cstr(&p->repo_identity_hash), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, atlas_buf_cstr(&p->pinned_commit), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, atlas_buf_cstr(&p->source_identity), err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 6, p->memory_generation) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the memory generation");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, atlas_buf_cstr(&p->decision_set_digest), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, atlas_buf_cstr(&p->source_set_digest), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, atlas_buf_cstr(&p->pack_digest), err);
    }
    if (st == ATLAS_OK &&
        sqlite3_bind_blob(stmt, 10, p->rendered.data != NULL ? p->rendered.data : "",
                          (int)p->rendered.len, SQLITE_TRANSIENT) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the rendered pack body");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 11, p->claim_count) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the claim count");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 12, p->excluded_count) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the excluded count");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 13, p->unanchored_count) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the unanchored count");
    }
    /* Bound by explicit length (`atlas_db_bind_text_n`), not `atlas_db_bind_
     * text_opt(..., atlas_buf_cstr(...), ...)`: a netstring is a
     * length-prefixed encoding precisely so an embedded byte is never
     * confused with a delimiter, and binding through a NUL-terminated view
     * would silently truncate at the first embedded NUL instead -- discarding
     * that binary safety at the one point these bytes are written. No stored
     * component (a claim uid, a vocabulary name, a `path_text`-encoded anchor
     * value) can contain a NUL today, so this has no live effect; it is
     * corrected because the netstring format's whole reason to exist is to
     * make that true by construction, not by which bytes happen to occur. */
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 14,
                                  p->claims_manifest.data != NULL ? p->claims_manifest.data : "",
                                  p->claims_manifest.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, stmt, 15,
                                  p->flagged_anchors.data != NULL ? p->flagged_anchors.data : "",
                                  p->flagged_anchors.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 16, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    } else {
        atlas_db_finish(db, stmt);
    }
    return st;
}

/* T12. Reads one frozen pack back by its run uid. */
atlas_status atlas_db_memory_pack_get(atlas_db *db, const char *run_uid, atlas_memory_pack *out,
                                      bool *found_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || run_uid == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no pack lookup to run");
    }
    atlas_memory_pack_free(out);
    atlas_memory_pack_init(out);
    static const char SQL[] =
        "SELECT repo_id, repo_identity_hash, pinned_commit, source_identity, memory_generation,"
        "  decision_set_digest, source_set_digest, pack_digest, rendered, claim_count,"
        "  excluded_count, unanchored_count, claims_manifest, flagged_anchors"
        " FROM memory_context_packs WHERE run_uid = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, run_uid, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->repo_id = sqlite3_column_int64(stmt, 0);
        st = take_col(&out->repo_identity_hash, stmt, 1, err);
        if (st == ATLAS_OK) {
            st = take_col(&out->pinned_commit, stmt, 2, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->source_identity, stmt, 3, err);
        }
        if (st == ATLAS_OK) {
            out->memory_generation = sqlite3_column_int64(stmt, 4);
            st = take_col(&out->decision_set_digest, stmt, 5, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->source_set_digest, stmt, 6, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->pack_digest, stmt, 7, err);
        }
        if (st == ATLAS_OK) {
            const void *blob = sqlite3_column_blob(stmt, 8);
            int blob_len = sqlite3_column_bytes(stmt, 8);
            st = atlas_buf_set(&out->rendered, blob != NULL ? blob : "",
                               blob_len > 0 ? (size_t)blob_len : 0u, err);
        }
        if (st == ATLAS_OK) {
            out->claim_count = sqlite3_column_int64(stmt, 9);
            out->excluded_count = sqlite3_column_int64(stmt, 10);
            out->unanchored_count = sqlite3_column_int64(stmt, 11);
            st = take_col(&out->claims_manifest, stmt, 12, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->flagged_anchors, stmt, 13, err);
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the frozen context pack");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- T13: the reliance check's one write -------------------------------------
 *
 * A tiny, file-local netstring reader for the one-element-per-record shape
 * `atlas_memory_pack_reliance_match` (`src/memory/pack.c`) produces and this
 * column stores: `<count>:` then that many single elements. A second, private
 * copy rather than a shared one, for the reason `src/memory/pack.c`'s own
 * tokenizer comment gives: a small, closed-form codec used by one layer, not a
 * cross-layer dependency for something this file would otherwise have no
 * reason to import. */
static bool reliance_ns_take(const char *text, size_t total, size_t *pos, const char **out,
                             size_t *len) {
    size_t i = *pos;
    size_t n = 0;
    size_t digits = 0;
    while (i < total && text[i] >= '0' && text[i] <= '9') {
        if (digits > 9) {
            return false;
        }
        n = n * 10u + (size_t)(text[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i >= total || text[i] != ':') {
        return false;
    }
    i++;
    if (n > total - i) {
        return false;
    }
    *out = text + i;
    *len = n;
    i += n;
    if (i >= total || text[i] != ',') {
        return false;
    }
    *pos = i + 1u;
    return true;
}

static atlas_status reliance_decode_into(const char *text, atlas_buf *out, size_t cap,
                                         size_t *n_out, atlas_err *err) {
    *n_out = 0;
    size_t total = text != NULL ? strlen(text) : 0u;
    if (total == 0) {
        return ATLAS_OK;
    }
    size_t pos = 0;
    size_t n = 0;
    size_t digits = 0;
    while (pos < total && text[pos] >= '0' && text[pos] <= '9') {
        n = n * 10u + (size_t)(text[pos] - '0');
        pos++;
        digits++;
        if (digits > 6) {
            return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored reliance claim list");
        }
    }
    if (digits == 0 || pos >= total || text[pos] != ':') {
        return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored reliance claim list");
    }
    pos++;
    if (n > cap) {
        return atlas_err_set(err, ATLAS_ERR_DB,
                             "a stored reliance claim list holds %zu entries, cap is %zu", n, cap);
    }
    for (size_t i = 0; i < n; i++) {
        const char *p = NULL;
        size_t len = 0;
        if (!reliance_ns_take(text, total, &pos, &p, &len)) {
            return atlas_err_set(err, ATLAS_ERR_DB, "malformed stored reliance claim list");
        }
        atlas_status st = atlas_buf_set(&out[i], p, len, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    *n_out = n;
    return ATLAS_OK;
}

/* T13, Decision 8. Called once per completion whose run's frozen pack has at
 * least one flagged PATH anchor (`src/db/db_orch.c`'s `reliance_check`, inside
 * `op_complete`'s own transaction) -- never for a run with no pack, and never
 * for a pack with nothing flagged, both of which have nothing for this check
 * to say.
 *
 * A11.6 makes a run's repo-tree chain more than one completion in general (a
 * retry, a narrower follow-up after a failed gate), so this merges rather than
 * replaces: `reliance_checked` is sticky (once 1, stays 1), `reliance_complete`
 * is the AND of every completion's own observation (one incomplete touched-
 * paths view makes the run's overall finding incomplete, and nothing makes it
 * complete again), and `reliance_claim_uids` is the union, deduplicated, of
 * every completion's matched set, in the order a uid was first seen. This is
 * the one write to `memory_context_packs.reliance_*`, and the row must already
 * exist -- freezing it is `atlas_memory_pack_freeze_in_tx`'s job, not this
 * one's, so a missing row is reported rather than created. */
atlas_status atlas_db_memory_pack_reliance_set(atlas_db *db, const char *run_uid, bool complete,
                                               const char *matched_claim_uids, atlas_err *err) {
    if (db == NULL || run_uid == NULL || run_uid[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no run to record a reliance check for");
    }
    static const char SEL[] =
        "SELECT reliance_complete, reliance_claim_uids FROM memory_context_packs"
        " WHERE run_uid = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SEL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, run_uid, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    bool found = rc == SQLITE_ROW;
    bool old_complete = true;
    atlas_buf old_uids = ATLAS_BUF_INIT;
    if (found) {
        old_complete = sqlite3_column_int64(stmt, 0) != 0;
        st = atlas_buf_set_str(&old_uids, atlas_db_col_text(stmt, 1), err);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the pack row to record reliance");
    }
    atlas_db_finish(db, stmt);
    if (st != ATLAS_OK) {
        atlas_buf_free(&old_uids);
        return st;
    }
    if (!found) {
        atlas_buf_free(&old_uids);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "run %s has no frozen context pack to record a reliance check on",
                             run_uid);
    }

    atlas_buf merged[ATLAS_MEMORY_PACK_MAX_CLAIMS];
    for (size_t i = 0; i < ATLAS_MEMORY_PACK_MAX_CLAIMS; i++) {
        atlas_buf_init(&merged[i]);
    }
    size_t merged_n = 0;
    struct {
        const char *text;
    } sources[] = {{atlas_buf_cstr(&old_uids)}, {matched_claim_uids}};
    for (size_t s = 0; st == ATLAS_OK && s < 2u; s++) {
        atlas_buf batch[ATLAS_MEMORY_PACK_MAX_CLAIMS];
        for (size_t i = 0; i < ATLAS_MEMORY_PACK_MAX_CLAIMS; i++) {
            atlas_buf_init(&batch[i]);
        }
        size_t batch_n = 0;
        st = reliance_decode_into(sources[s].text, batch, ATLAS_MEMORY_PACK_MAX_CLAIMS, &batch_n,
                                  err);
        for (size_t i = 0; st == ATLAS_OK && i < batch_n; i++) {
            bool already = false;
            for (size_t m = 0; !already && m < merged_n; m++) {
                already = merged[m].len == batch[i].len &&
                         (batch[i].len == 0 ||
                          memcmp(merged[m].data, batch[i].data, batch[i].len) == 0);
            }
            if (!already) {
                if (merged_n >= ATLAS_MEMORY_PACK_MAX_CLAIMS) {
                    /* M2, T13 fix round. Unreachable in practice: `old_uids`
                     * and `matched_claim_uids` are each subsets of one pack's
                     * own flagged-claim set, itself capped at
                     * `ATLAS_MEMORY_PACK_MAX_CLAIMS` by
                     * `atlas_memory_pack_build`, so their union cannot exceed
                     * the cap either. Refused rather than silently dropped,
                     * the same discipline `atlas_memory_pack_reliance_match`
                     * (`src/memory/pack.c`) already applies to the identical
                     * situation -- a silent drop here would lower no flag and
                     * leave the row claiming a complete uid set it does not
                     * hold. */
                    st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                       "more distinct claims were merged into a reliance check "
                                       "than the pack could ever have flagged");
                } else {
                    st = atlas_buf_set(&merged[merged_n], batch[i].data, batch[i].len, err);
                    if (st == ATLAS_OK) {
                        merged_n++;
                    }
                }
            }
        }
        for (size_t i = 0; i < ATLAS_MEMORY_PACK_MAX_CLAIMS; i++) {
            atlas_buf_free(&batch[i]);
        }
    }
    atlas_buf_free(&old_uids);

    atlas_buf encoded = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&encoded, err, "%zu:", merged_n);
    }
    for (size_t i = 0; st == ATLAS_OK && i < merged_n; i++) {
        size_t n = merged[i].len;
        st = atlas_buf_appendf(&encoded, err, "%zu:", n);
        if (st == ATLAS_OK && n > 0) {
            st = atlas_buf_append(&encoded, merged[i].data, n, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(&encoded, ',', err);
        }
    }
    for (size_t i = 0; i < ATLAS_MEMORY_PACK_MAX_CLAIMS; i++) {
        atlas_buf_free(&merged[i]);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&encoded);
        return st;
    }

    static const char UPD[] =
        "UPDATE memory_context_packs SET reliance_checked = 1, reliance_complete = ?1,"
        "  reliance_claim_uids = ?2 WHERE run_uid = ?3;";
    sqlite3_stmt *q = NULL;
    st = atlas_db_prepare(db, UPD, &q, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&encoded);
        return st;
    }
    (void)sqlite3_bind_int64(q, 1, (old_complete && complete) ? 1 : 0);
    st = atlas_db_bind_text_n(db, q, 2, atlas_buf_cstr(&encoded), encoded.len, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, q, 3, run_uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, q, err);
    } else {
        atlas_db_finish(db, q);
    }
    atlas_buf_free(&encoded);
    return st;
}

atlas_status atlas_db_memory_claim_diff_last_kind(atlas_db *db, int64_t repo_id, const char *claim_uid,
                                                  atlas_memory_diff_kind *kind_out, bool *found_out,
                                                  atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || claim_uid == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no claim diff history to look up");
    }
    static const char SQL[] =
        "SELECT d.kind FROM memory_claim_diffs d"
        " JOIN memory_generations g ON g.id = d.generation_id"
        " WHERE g.repo_id = ?1 AND d.claim_uid = ?2"
        " ORDER BY g.generation DESC LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, claim_uid, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *kind_name = (const char *)sqlite3_column_text(stmt, 0);
        if (kind_name != NULL && kind_out != NULL &&
            atlas_memory_diff_kind_parse(kind_name, kind_out)) {
            if (found_out != NULL) {
                *found_out = true;
            }
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* T9 fix-round-2 (C3, second half), comment corrected in fix-round-3: `?2` is
 * spliced into a `LIKE` pattern (`?2 || '/%'`) so that a literal `%` or `_`
 * *inside the source's own path_text* would otherwise be read as a wildcard
 * rather than a literal character, matching more (or, with `_`, subtly
 * different) rows than the source's own path names.
 *
 * Round 2's comment overstated how often that literal character occurs: it
 * claimed "any path containing a space, a non-ASCII byte or another `%`
 * guarantees" one, but `atlas_codepoint_is_unsafe`
 * (`src/core/safetext.c:50-75`) does not list a space (0x20 is not `< 0x20`),
 * and a valid non-ASCII UTF-8 codepoint passes `atlas_text_encode_safe`
 * verbatim unless it is on that same unsafe list -- neither is escaped, so
 * neither produces a `%` or `_` on its own. Only three things do: a literal
 * `%` byte (escaped so the `%XX` encoding stays reversible), a codepoint
 * `atlas_codepoint_is_unsafe` actually flags (C0/C1 controls, DEL, line and
 * paragraph separators, the bidi override ranges), or invalid UTF-8 (escaped
 * byte for byte). The code guards against all three regardless of how often
 * any one of them occurs; this paragraph is only correcting how wide a net
 * the justification claimed, which is the sort of sentence a later reader
 * takes as established without checking.
 *
 * The `ESCAPE '\\'` clause already in the SQL only ever meant "a backslash
 * before a wildcard in the *pattern* is literal"; nothing inserted a
 * backslash into `?2`'s own bound value, so it was inert for exactly the
 * content it needed to guard. Backslash-escapes `\`, `%` and `_` (in that
 * order, so an existing backslash is not double-escaped) before binding. */
static atlas_status like_escape(const char *raw, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    if (raw == NULL) {
        return ATLAS_OK;
    }
    atlas_status st = ATLAS_OK;
    for (const unsigned char *p = (const unsigned char *)raw; *p != '\0' && st == ATLAS_OK; p++) {
        if (*p == '\\' || *p == '%' || *p == '_') {
            st = atlas_buf_append_ch(out, '\\', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(out, (char)*p, err);
        }
    }
    return st;
}

atlas_status atlas_db_memory_dir_hash_mismatch(atlas_db *db, int64_t repo_id, int64_t source_id,
                                               const char *path_text, bool *changed_out,
                                               atlas_err *err) {
    if (changed_out != NULL) {
        *changed_out = false;
    }
    if (db == NULL || path_text == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no directory source to check");
    }
    /* T9 fix-round-1 (C3), corrected in fix-round-2, fix-round-3 and again in
     * fix-round-4. The whole predicate list below answers exactly one
     * question -- "does this `files` row still describe current content that
     * `src/memory/read.c` would ingest as a memory file?" -- and every
     * clause exists because an earlier round asked that question in a way
     * this query did not yet agree with. Two places deciding "is this a
     * memory file?" independently is the recurring defect; this comment is
     * the running ledger of where they diverged.
     *
     * Depth (round 1): one level below `?2`, no further slash after that --
     * `path_text LIKE ?2 || '/%'` minus `path_text LIKE ?2 || '/%/%'`. `?2`
     * itself is escaped by `like_escape` before binding (see its own
     * comment) so a literal `%` or `_` inside the source's own path can no
     * longer act as a wildcard. Still bounded at one more than
     * `ATLAS_MEMORY_MAX_DIR_ENTRIES` files examined.
     *
     * Suffix case (round 2): `read.c:410-413`'s own `memcmp` against `.md`
     * is case-sensitive, no folding, so `LIKE '%.md'` -- which SQLite folds
     * ASCII case on by default -- matched `NOTES.MD` forever
     * (`changed_out = true` for ever, the permanent-`SOURCE_REVISION` loop
     * round 1 set out to close) though `read.c` never ingests it. Fixed with
     * `substr(path_text, -3) = '.md'`: plain `=` on a `TEXT` column with no
     * declared collation is `BINARY` -- SQLite's own default -- so this is
     * exactly `read.c`'s comparison, byte for byte, and takes no `ESCAPE`
     * clause because it is not a pattern.
     *
     * Entry type (round 3): `read.c:414-421`'s listing filter excludes only
     * `S_ISDIR`, so a symlink named `x.md` is listed and then refused by
     * `open_fs_file` with outcome `ATLAS_MEMORY_READ_SYMLINK`
     * (`read.c:129-135`) though a real scan gives it `file_type = 'symlink'`
     * with `content_hash` = the hash of its link text, A13's own rule
     * (`src/core/scan.c:329-341`) -- that row carries a real hash, so
     * nothing before this predicate excluded it. `file_type = 'regular'`,
     * `read.c`'s own dividing line, is what closes this door, and it is the
     * only predicate that does: a fifo/socket/device (`file_type = 'other'`)
     * and a row recorded `file_type = 'missing'` also fail it, but neither
     * was ever a door a real scan could open. Every producer of both leaves
     * `content_hash` unset -- `outcome_file_type` maps every outcome that is
     * not a regular file or a symlink to `'other'` or, for `ENTRY_MISSING`,
     * to `'missing'` (`src/core/reconcile.c:838-852`), and `rec.content_hash`
     * is assigned only under `e->have_hash` (`reconcile.c:912-915`), which
     * none of those outcomes set -- so the `hash == NULL` skip already in
     * this loop (below) excluded both before this predicate existed.
     * `file_type = 'regular'` still costs nothing against them and stays as
     * defence in depth; a corrected record of that is at
     * `tests/test_memory_reconcile.c` beside the doors test this predicate
     * belongs to.
     *
     * Size bound (round 3): a `file_type = 'regular'` `.md` file over
     * `ATLAS_MEMORY_MAX_SOURCE_BYTES` still has a real content hash --
     * `file_type = 'regular'` alone does not exclude it -- but
     * `read.c:158-163` refuses it with outcome `TOO_LARGE` and no bytes,
     * this season's own "a bound that is reached is refused, never
     * trimmed", so it too is never versioned. `size_bytes <= ?4` restates
     * that bound; `size_bytes IS NOT NULL AND size_bytes >= 0` excludes a
     * row this pass cannot even ask the question of, on the same footing --
     * a row `read.c` cannot be shown to ingest is not evidence of a
     * mismatch either, exactly the posture `size_bytes IS NULL` already
     * gets from every other reader of this column. This and the symlink
     * predicate above are the two doors of round 3 a real scan could
     * actually produce; naming all four as equally live is exactly the
     * reasoning -- entry *type* as the discriminator for "would `read.c`
     * ingest this" -- that stopped one field short of the fifth door below,
     * because type answers that question only for a symlink.
     *
     * Deletion (round 4, the fifth door): `atlas_db_files_mark_deleted`
     * (`src/db/db_index.c:404-407`) is `UPDATE files SET deleted=1,
     * deleted_at=?1, deleted_scan_id=?2 ... ` -- it touches nothing else, so
     * a `.md` file removed from the tree keeps its last `file_type =
     * 'regular'`, in-bound `size_bytes` and real `content_hash`. `read.c`'s
     * `readdir` (`read.c:397-433`) cannot list a path that is gone, so no
     * `memory_source_versions` row is ever written for that hash under this
     * `source_id`, and this query reported a mismatch for it for ever -- no
     * race required: any `.md` deleted before the memory source was ever
     * registered, or before the first pass ran, reaches this from the very
     * first call. `deleted_scan_id IS NULL` closes it, spelled exactly as
     * the sibling branch of the very same loop already spells it for
     * `REPO_FILE` sources: `atlas_memory_plan_for`'s `REPO_FILE` branch
     * (`src/memory/reconcile.c:2237`) calls `atlas_db_verify_file_hash`,
     * whose SQL filters `deleted_scan_id IS NULL AND content_hash IS NOT
     * NULL` under the comment "A deleted file has no current content, and
     * reporting its last known hash would let a claim about bytes that are
     * gone verify for ever" (`src/db/db_verify.c:2062-2067`); this
     * function is what the `REPO_DIR` branch further down that same loop
     * calls instead (`reconcile.c:2252`). One question, asked
     * twice by two branches of one loop, must get one answer; a reader
     * changing either predicate list should read the other. `deleted = 0`
     * would answer the same question here -- the two writers of this table
     * that touch either column always set both together
     * (`db_index.c:358` clears both on re-tracking, `db_index.c:404-407`
     * sets both on deletion) -- but the sibling's own spelling is kept so
     * two places answering one question cannot drift on which column they
     * consult if that ever stops being true.
     *
     * The `LIMIT ?3` below has no `ORDER BY`. One thing about that is proved
     * and one is not, and they are kept apart here because an earlier round
     * of this comment ran them together and got a safety argument out of it
     * that does not hold.
     *
     * **Proved: a directory within bound is never truncated.** `read.c`
     * refuses a directory whose live matching children exceed
     * `ATLAS_MEMORY_MAX_DIR_ENTRIES` outright rather than versioning the
     * first 64 of them (`read.c:425-431,441-448`, "refused rather than
     * trimmed"), so a source this function can say anything useful about has
     * at most `ATLAS_MEMORY_MAX_DIR_ENTRIES` live matching rows -- and now
     * that `deleted_scan_id IS NULL` keeps a dead row from occupying one of
     * the slots, "live rows" and "rows this query can see" are the same
     * number. `LIMIT ?3` binds `ATLAS_MEMORY_MAX_DIR_ENTRIES + 1`, so every
     * such directory is returned whole and the order it comes back in cannot
     * matter. That is also what closes the fifth door's more dangerous
     * second consequence: before `deleted_scan_id IS NULL`, an unbounded run
     * of dead rows could fill the window and push the one live, genuinely
     * changed file out of it, with no `ORDER BY` to say which 65 of a larger
     * set came back -- a permanent silent miss rather than the permanent
     * false alarm the other doors produce.
     *
     * **Not proved, and stated rather than argued away: this function does
     * not detect a directory that has grown past the ceiling.** An earlier
     * round claimed it did, by counting: at most 64 rows are ever versioned,
     * so a 65th must be unversioned and the pass must run. The count is
     * wrong twice over. Versions are keyed by content, not by path --
     * `atlas_db_memory_version_exists` matches on `(source_id,
     * content_sha256)` alone -- so N live rows collapse to however many
     * distinct hashes they carry, and two empty files carry one. And the
     * version set is cumulative: nothing prunes `memory_source_versions`, so
     * "at most 64" is true of a single pass and false of the table, which
     * after twenty passes holds hundreds of hashes to match against. There
     * is no bounded set to count against, and no finite `LIMIT` recovers the
     * argument. `cp a.md a-copy.md` in a 64-file source is the whole repro:
     * 65 live rows, 64 hashes, every one already versioned, `changed_out`
     * false.
     *
     * That answer is the honest one for the question *this* function owns --
     * no content appeared that Atlas has not already seen, so there is no
     * source revision -- and it is recorded here because the cost lands
     * elsewhere. `atlas_memory_plan_for`'s only caller submits a pass just
     * when it derives a cause, so nothing schedules an unconditional
     * periodic pass, and a source directory that crosses the ceiling by
     * copying a file it already holds is not examined again until some other
     * signal moves. When one does, the pass does not report the ceiling as a
     * per-item obstacle either: `read.c` returns `ATLAS_ERR_CONFIG` for the
     * whole listing, so the pass fails rather than recording an item nobody
     * could read.
     *
     * Both sides of every one of these comparisons are snapshots taken at
     * different times: `files.size_bytes`, `files.file_type` and
     * `files.deleted_scan_id` are from the last scan, `read.c` stats and
     * classifies again at read time. A file that changed kind, crossed the
     * size bound, or was deleted between the two is judged on stale data for
     * one pass -- convergence, not correctness, the same posture the rest of
     * this check already has (the case and depth fixes above carry no
     * re-stat either). Do not add one here. */
    static const char SQL[] =
        "SELECT content_hash FROM files"
        " WHERE repo_id = ?1"
        "   AND path_text LIKE ?2 || '/%' ESCAPE '\\'"
        "   AND path_text NOT LIKE ?2 || '/%/%' ESCAPE '\\'"
        "   AND length(path_text) > 3"
        "   AND substr(path_text, -3) = '.md'"
        "   AND file_type = 'regular'"
        "   AND size_bytes IS NOT NULL"
        "   AND size_bytes >= 0"
        "   AND size_bytes <= ?4"
        "   AND deleted_scan_id IS NULL"
        " LIMIT ?3;";
    atlas_buf escaped = ATLAS_BUF_INIT;
    atlas_status st = like_escape(path_text, &escaped, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&escaped);
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&escaped);
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        atlas_buf_free(&escaped);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, atlas_buf_cstr(&escaped), err);
    atlas_buf_free(&escaped);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 3, (int64_t)ATLAS_MEMORY_MAX_DIR_ENTRIES + 1) !=
                              SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the row limit");
    }
    if (st == ATLAS_OK &&
        sqlite3_bind_int64(stmt, 4, (int64_t)ATLAS_MEMORY_MAX_SOURCE_BYTES) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the size bound");
    }
    for (; st == ATLAS_OK;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the indexed files");
            break;
        }
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        if (hash == NULL || hash[0] == '\0') {
            continue;
        }
        bool found = false;
        st = atlas_db_memory_version_exists(db, source_id, hash, &found, NULL, NULL, NULL, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (!found) {
            if (changed_out != NULL) {
                *changed_out = true;
            }
            break;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- T14: commit trailer bindings -------------------------------------------
 *
 * `memory_trailer_bindings` (migration 29, `src/db/migrate.c:4450-4467`).
 * `src/memory/trailer.c` owns the parsing and the field-by-field resolution;
 * these two functions are the whole of this table's database surface, T17's
 * grep target exactly as every other `memory_*` table's is. */

atlas_status atlas_db_memory_trailer_binding_insert(atlas_db *db, int64_t repo_id,
                                                    const char *commit_oid,
                                                    const atlas_memory_trailer_binding *b,
                                                    const char *recorded_at, bool *landed_out,
                                                    atlas_err *err) {
    if (landed_out != NULL) {
        *landed_out = false;
    }
    if (db == NULL || commit_oid == NULL || commit_oid[0] == '\0' || b == NULL ||
       recorded_at == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no trailer binding to record");
    }
    static const char SQL[] =
        "INSERT OR IGNORE INTO memory_trailer_bindings(repo_id, commit_oid, has_block, run_uid,"
        "  memory_generation, context_digest_ok, decision_set_ok, change_reason_uid,"
        "  unknown_fields, recorded_at, bound_hit) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,"
        "  ?11);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, commit_oid, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 3, b->has_block ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind has_block");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, atlas_buf_cstr(&b->run_uid), err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 5, b->memory_generation) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the memory generation");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 6, b->context_digest_ok ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind context_digest_ok");
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 7, b->decision_set_ok ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind decision_set_ok");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, atlas_buf_cstr(&b->change_reason_uid), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, atlas_buf_cstr(&b->unknown_fields), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, recorded_at, err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 11, b->bound_hit ? 1 : 0) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind bound_hit");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
        /* Fix round M2: `INSERT OR IGNORE` hitting `UNIQUE(repo_id,
         * commit_oid)` on a re-walked commit returns ATLAS_OK with zero rows
         * changed -- `atlas_db_memory_unanchored_add`'s own precedent
         * (`sqlite3_changes(db->h) > 0`) is what lets a caller count landed
         * writes rather than re-presentations. */
        if (st == ATLAS_OK && landed_out != NULL) {
            *landed_out = sqlite3_changes(db->h) > 0;
        }
    } else {
        atlas_db_finish(db, stmt);
        stmt = NULL;
    }
    if (stmt != NULL) {
        atlas_db_finish(db, stmt);
    }
    return st;
}

atlas_status atlas_db_memory_trailer_binding_get(atlas_db *db, int64_t repo_id,
                                                 const char *commit_oid,
                                                 atlas_memory_trailer_binding *out, bool *found_out,
                                                 atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    if (db == NULL || commit_oid == NULL || commit_oid[0] == '\0' || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no trailer binding to look up");
    }
    atlas_memory_trailer_binding_free(out);
    atlas_memory_trailer_binding_init(out);
    static const char SQL[] =
        "SELECT has_block, run_uid, memory_generation, context_digest_ok, decision_set_ok,"
        "  change_reason_uid, unknown_fields, bound_hit FROM memory_trailer_bindings"
        " WHERE repo_id = ?1 AND commit_oid = ?2;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, commit_oid, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->has_block = sqlite3_column_int64(stmt, 0) != 0;
        st = take_col(&out->run_uid, stmt, 1, err);
        if (st == ATLAS_OK) {
            out->memory_generation = sqlite3_column_int64(stmt, 2);
        }
        if (st == ATLAS_OK) {
            out->context_digest_ok = sqlite3_column_int64(stmt, 3) != 0;
        }
        if (st == ATLAS_OK) {
            out->decision_set_ok = sqlite3_column_int64(stmt, 4) != 0;
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->change_reason_uid, stmt, 5, err);
        }
        if (st == ATLAS_OK) {
            st = take_col(&out->unknown_fields, stmt, 6, err);
        }
        if (st == ATLAS_OK) {
            out->bound_hit = sqlite3_column_int64(stmt, 7) != 0;
        }
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read the trailer binding");
    }
    atlas_db_finish(db, stmt);
    return st;
}

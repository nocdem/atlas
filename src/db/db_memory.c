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
    /* T9 fix-round-1 (C3), corrected in fix-round-2 and again in fix-round-3.
     * The whole predicate list below answers exactly one question -- "would
     * `src/memory/read.c` ingest this `files` row as a memory file?" -- and
     * every clause exists because round 1 or round 2 or round 3 asked that
     * question in a way this query did not yet agree with. Two places
     * deciding "is this a memory file?" independently is the recurring
     * defect; this comment is the running ledger of where they diverged.
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
     * Entry type (round 3, doors 1-3): `read.c:414-421`'s listing filter
     * excludes only `S_ISDIR`, so a symlink, a fifo/socket/device, or a row
     * a scan could not classify at all reaches `open_fs_file` and is refused
     * there -- `ATLAS_MEMORY_READ_SYMLINK` for a symlink
     * (`read.c:129-135`), `ABSENT` for anything else not a regular file
     * (`read.c:140-146`) -- so none of them is ever given a
     * `memory_source_versions` row. A real scan still gives each of them a
     * `files` row: `file_type = 'symlink'` with `content_hash` = the hash of
     * the link text (A13's own rule, `src/core/scan.c:329-341`),
     * `file_type = 'other'` for a fifo/socket/device, or `file_type =
     * 'missing'` for the fourth member of the CHECK'd vocabulary
     * (`src/db/migrate.c:85`). Without a type filter this query matched all
     * three and reported a mismatch it could never close. `file_type =
     * 'regular'` is `read.c`'s own dividing line, restated as a predicate.
     *
     * Size bound (round 3, door 4): a `file_type = 'regular'` `.md` file
     * over `ATLAS_MEMORY_MAX_SOURCE_BYTES` still has a real content hash --
     * `file_type = 'regular'` alone does not exclude it -- but
     * `read.c:158-163` refuses it with outcome `TOO_LARGE` and no bytes,
     * this season's own "a bound that is reached is refused, never
     * trimmed", so it too is never versioned. `size_bytes <= ?4` restates
     * that bound; `size_bytes IS NOT NULL AND size_bytes >= 0` excludes a
     * row this pass cannot even ask the question of, on the same footing --
     * a row `read.c` cannot be shown to ingest is not evidence of a
     * mismatch either, exactly the posture `size_bytes IS NULL` already
     * gets from every other reader of this column.
     *
     * Both sides of every one of these comparisons are snapshots taken at
     * different times: `files.size_bytes` and `files.file_type` are from
     * the last scan, `read.c` stats and classifies again at read time. A
     * file that changed kind or crossed the size bound between the two is
     * judged on stale data for one pass -- convergence, not correctness,
     * the same posture the rest of this check already has (the case and
     * depth fixes above carry no re-stat either). Do not add one here. */
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

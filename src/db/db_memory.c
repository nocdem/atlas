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
    static const char *D = "0123456789abcdef";
    for (size_t i = 0; i < sizeof raw; i++) {
        hex[i * 2u] = D[raw[i] >> 4];
        hex[i * 2u + 1u] = D[raw[i] & 0x0fu];
    }
    hex[sizeof hex - 1u] = '\0';
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

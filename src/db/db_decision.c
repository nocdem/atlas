/* Atlas - typed operations over the migration-6 decision tables.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * sqlite3 types stay in this directory, as everywhere else. The lifecycle rules
 * live in src/decision/lifecycle.c; what is here is the storage they act on,
 * plus the three queries that are load-bearing enough to be worth arguing
 * about: link currency, the supersession-cycle walk, and the ledger replay that
 * lets `atlas doctor` check the cached status without writing anything.
 *
 * Two invariants are enforced *here* rather than by the callers.
 *
 * 1. **No content column is ever updated.** Search this file for `UPDATE
 *    decision_revisions` and there is exactly one statement; it sets `state`
 *    and nothing else. There is no UPDATE for `decision_alternatives`,
 *    `decision_links` or `decision_events` at all.
 *
 * 2. **Nothing here deletes a decision record.** The only DELETE in the file is
 *    `atlas_db_decision_challenges_prune`, and it carries `consumed = 0 AND
 *    expires_at < ?` so a spent capability — which an approval event points at
 *    — is never among the rows it can see.
 */
#include "db/db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision.h"

/* --- small helpers -------------------------------------------------------- */

static atlas_status bind_blob_opt(atlas_db *db, sqlite3_stmt *s, int idx, const atlas_buf *b,
                                  atlas_err *err) {
    if (b == NULL || b->len == 0) {
        if (sqlite3_bind_null(s, idx) != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind NULL");
        }
        return ATLAS_OK;
    }
    return atlas_db_bind_blob(db, s, idx, b->data, b->len, err);
}

static atlas_status bind_text_buf_opt(atlas_db *db, sqlite3_stmt *s, int idx, const atlas_buf *b,
                                      atlas_err *err) {
    if (b == NULL || b->len == 0) {
        if (sqlite3_bind_null(s, idx) != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind NULL");
        }
        return ATLAS_OK;
    }
    return atlas_db_bind_text_n(db, s, idx, b->data, b->len, err);
}

static atlas_status bind_i64(atlas_db *db, sqlite3_stmt *s, int idx, int64_t v, atlas_err *err) {
    if (sqlite3_bind_int64(s, idx, v) != SQLITE_OK) {
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind an integer");
    }
    return ATLAS_OK;
}

/* Binds a positive id, or SQL NULL for 0. Used for every soft reference, so
 * "no session", "no challenge" and "not superseded" are NULL rather than a
 * magic zero that a join would silently match. */
static atlas_status bind_id_opt(atlas_db *db, sqlite3_stmt *s, int idx, int64_t v,
                                atlas_err *err) {
    if (v <= 0) {
        if (sqlite3_bind_null(s, idx) != SQLITE_OK) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind NULL");
        }
        return ATLAS_OK;
    }
    return bind_i64(db, s, idx, v, err);
}

static atlas_status set_buf_from_col(atlas_buf *dst, sqlite3_stmt *s, int col, atlas_err *err) {
    const void *p = sqlite3_column_blob(s, col);
    int n = sqlite3_column_bytes(s, col);
    if (p == NULL || n <= 0) {
        atlas_buf_reset(dst);
        return ATLAS_OK;
    }
    return atlas_buf_set(dst, p, (size_t)n, err);
}

/* --- documents ------------------------------------------------------------ */

atlas_status atlas_db_decision_document_create(atlas_db *db, int64_t repo_id,
                                               const char *root_hash, const char *created_at,
                                               int64_t *id_out, char *uid_out, size_t uid_size,
                                               atlas_err *err) {
    *id_out = 0;
    if (uid_size > 0) {
        uid_out[0] = '\0';
    }
    /* The repository's durable identity, recorded on the document so that a
     * later re-registration can be checked against it rather than against the
     * path. Empty when the lineage is unknown, which permanently opts this
     * document out of automatic relinking — the fail-closed direction. */
    atlas_buf identity = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_repo_identity_hash(db, repo_id, &identity, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&identity);
        return st;
    }

    /* The row is created with a placeholder uid and the real one is written
     * once the row id exists, because the uid is derived from it. Both
     * statements are in the caller's transaction, so a document with a
     * placeholder uid is never visible to anything.
     *
     * The placeholder carries the previous rowid, so two creates inside one
     * transaction cannot collide on the UNIQUE index before the second write:
     * the first insert moves `last_insert_rowid`. */
    sqlite3_stmt *s = NULL;
    st = atlas_db_prepare(db,
                          "INSERT INTO decision_documents"
                          "(uid, repo_id, repo_root_hash, repo_identity_hash, created_at,"
                          " updated_at, latest_revision_no, current_status)"
                          " VALUES(?1, ?2, ?3, ?4, ?5, ?5, 0, 'PROPOSED');",
                          &s, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&identity);
        return st;
    }
    char placeholder[64];
    (void)snprintf(placeholder, sizeof(placeholder), "pending-%lld-%s",
                   (long long)sqlite3_last_insert_rowid(db->h), created_at);
    st = atlas_db_bind_text_opt(db, s, 1, placeholder, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 3, root_hash != NULL ? root_hash : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 4, identity.data != NULL ? identity.data : "",
                                  identity.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 5, created_at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        atlas_buf_free(&identity);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&identity);
        return st;
    }
    int64_t id = sqlite3_last_insert_rowid(db->h);

    /* The real uid, with the UNIQUE index as the collision detector.
     *
     * At 128 bits a collision means the entropy source is broken rather than
     * that Atlas got unlucky, so the retry exists to survive a transient fault
     * and the ceiling exists so a broken source is reported instead of spun on.
     * The attempt counter is mixed into the derivation, so even a source that
     * repeated would produce a different value on the next pass. */
    char uid[ATLAS_DECISION_UID_MAX];
    bool assigned = false;
    for (unsigned attempt = 0; !assigned && attempt < ATLAS_DECISION_UID_MAX_ATTEMPTS; attempt++) {
        st = atlas_decision_uid_derive(atlas_buf_cstr(&identity), id, created_at, attempt, uid,
                                       sizeof(uid), err);
        if (st != ATLAS_OK) {
            break;
        }
        sqlite3_stmt *u = NULL;
        st = atlas_db_prepare(db,
                              "UPDATE decision_documents SET uid = ?1"
                              " WHERE id = ?2 AND NOT EXISTS"
                              "   (SELECT 1 FROM decision_documents d2 WHERE d2.uid = ?1);",
                              &u, err);
        if (st != ATLAS_OK) {
            break;
        }
        st = atlas_db_bind_text_opt(db, u, 1, uid, err);
        if (st == ATLAS_OK) {
            st = bind_i64(db, u, 2, id, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, u);
            break;
        }
        st = atlas_db_step_done(db, u, err);
        if (st != ATLAS_OK) {
            break;
        }
        assigned = sqlite3_changes(db->h) == 1;
    }
    atlas_buf_free(&identity);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!assigned) {
        /* Reported, never worked around. A uid that could not be made unique in
         * eight tries at 128 bits is a broken randomness source, and inventing
         * a sequential fallback would hide it. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "could not assign a unique decision id in %d attempts; the local "
                             "randomness source is not producing distinct values",
                             ATLAS_DECISION_UID_MAX_ATTEMPTS);
    }
    *id_out = id;
    (void)snprintf(uid_out, uid_size, "%s", uid);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_find_uid(atlas_db *db, const char *uid, int64_t *id_out,
                                        int64_t *repo_id_out, bool *found_out, atlas_err *err) {
    *id_out = 0;
    *repo_id_out = 0;
    *found_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT id, repo_id FROM decision_documents WHERE uid = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, uid, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(s, 0);
        *repo_id_out = sqlite3_column_int64(s, 1);
        *found_out = true;
    } else if (rc != SQLITE_DONE) {
        atlas_db_finish(db, s);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look up a decision by id");
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_uid_of(atlas_db *db, int64_t document_id, atlas_buf *out,
                                      atlas_err *err) {
    atlas_buf_reset(out);
    if (document_id <= 0) {
        return ATLAS_OK;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st =
        atlas_db_prepare(db, "SELECT uid FROM decision_documents WHERE id = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        st = atlas_buf_set_str(out, atlas_db_col_text(s, 0), err);
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_document_note_revision(atlas_db *db, int64_t document_id,
                                                      int64_t revision_no, const char *updated_at,
                                                      atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE decision_documents"
                                       " SET latest_revision_no = ?2, updated_at = ?3"
                                       " WHERE id = ?1;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 3, updated_at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return atlas_db_step_done(db, s, err);
}

atlas_status atlas_db_decision_document_set_state(atlas_db *db, int64_t document_id,
                                                  int64_t current_revision_id, const char *status,
                                                  const char *updated_at, atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE decision_documents"
                                       " SET current_revision_id = ?2, current_status = ?3,"
                                       "     updated_at = ?4"
                                       " WHERE id = ?1;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 2, current_revision_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 3, status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 4, updated_at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return atlas_db_step_done(db, s, err);
}

atlas_status atlas_db_decision_document_set_superseded_by(atlas_db *db, int64_t document_id,
                                                          int64_t by_document_id, const char *at,
                                                          atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE decision_documents"
                                       " SET superseded_by_document_id = ?2, superseded_at = ?3,"
                                       "     updated_at = ?3"
                                       " WHERE id = ?1;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 2, by_document_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 3, at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return atlas_db_step_done(db, s, err);
}

/* --- repository identity ---------------------------------------------------
 *
 * What makes two registrations the same repository, and what does not.
 *
 * `repo_root_hash` answers "same directory". That is a location, and a location
 * is not an identity: remove a repository, `git init` an unrelated one in the
 * same path, and a path hash says they are the same project. It would attach
 * one team's approved decisions to another's code.
 *
 * What this function computes is a **path-qualified lineage fingerprint**: the
 * canonical root path, the object format, and the sorted set of root commits.
 * Both halves are load-bearing and each rules out a different wrong attachment.
 *
 * The root-commit set — commits with no parent — is the discriminating half. It
 * is stable across clones, fetches, rewrites of later history and
 * re-registration at the same path, and it differs between unrelated
 * repositories, so it is what stops an unrelated `git init` at the old path from
 * inheriting the previous project's approved decisions. Atlas already has it in
 * `commits` after any scan, so this costs one indexed query and needs no git
 * invocation, no new allowlisted subcommand and no new plumbing.
 *
 * The root path is hashed alongside it, which means the fingerprint is **not**
 * stable across a move: the same repository cloned or relocated elsewhere does
 * not reattach automatically. That is deliberate. Matching on the lineage alone
 * would silently associate every clone of a repository on the machine, and an
 * operator who cloned a colleague's tree to read it would find their approved
 * decisions attached to it. An orphan is visible in `atlas decision orphaned`
 * and recoverable; a wrong attachment is neither. Manual relinking is deferred
 * to a later phase rather than guessed at here.
 *
 * So describe it as a path-qualified lineage fingerprint and name both halves.
 * A description crediting only the lineage is wrong in the second direction,
 * and one crediting only the path is wrong in the first; automatic
 * reattachment requires the exact fingerprint.
 *
 * When there is no root commit — an unborn HEAD, or history not yet ingested —
 * the identity is **empty**, and an empty identity matches nothing. */
atlas_status atlas_db_repo_identity_hash(atlas_db *db, int64_t repo_id, atlas_buf *out,
                                         atlas_err *err) {
    atlas_buf_reset(out);
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT root_path, object_format FROM repositories WHERE id = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    atlas_buf root = ATLAS_BUF_INIT;
    atlas_buf format = ATLAS_BUF_INIT;
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        found = true;
        st = set_buf_from_col(&root, s, 0, err);
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&format, s, 1, err);
        }
    }
    atlas_db_finish(db, s);
    if (st != ATLAS_OK || !found) {
        atlas_buf_free(&root);
        atlas_buf_free(&format);
        return st;
    }

    /* The root commits, sorted, so the identity does not depend on insertion
     * order. Bounded: a repository with a pathological number of disconnected
     * root commits must not make this unbounded, and the first few are already
     * decisive. */
    sqlite3_stmt *c = NULL;
    st = atlas_db_prepare(db,
                          "SELECT oid FROM commits WHERE repo_id = ?1 AND parent_count = 0"
                          " ORDER BY oid LIMIT 64;",
                          &c, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&root);
        atlas_buf_free(&format);
        return st;
    }
    st = bind_i64(db, c, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, c);
        atlas_buf_free(&root);
        atlas_buf_free(&format);
        return st;
    }
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    static const char domain[] = "atlas.repo.identity.v1";
    atlas_sha256_update(&ctx, domain, sizeof(domain));
    /* Length-prefixed, for the reason the decision content hash is: a delimiter
     * is a byte a path can contain. */
    unsigned char lenbuf[8];
    for (size_t i = 0; i < 8u; i++) {
        lenbuf[i] = (unsigned char)((((uint64_t)root.len) >> (8u * (7u - i))) & 0xFFu);
    }
    atlas_sha256_update(&ctx, lenbuf, sizeof(lenbuf));
    atlas_sha256_update(&ctx, root.data != NULL ? root.data : "", root.len);
    atlas_sha256_update(&ctx, format.data != NULL ? format.data : "", format.len);
    atlas_sha256_update(&ctx, "|", 1u);

    int64_t roots = 0;
    while (sqlite3_step(c) == SQLITE_ROW) {
        const char *oid = atlas_db_col_text(c, 0);
        atlas_sha256_update(&ctx, oid, strlen(oid));
        atlas_sha256_update(&ctx, ",", 1u);
        roots++;
    }
    atlas_db_finish(db, c);
    atlas_buf_free(&root);
    atlas_buf_free(&format);

    if (roots == 0) {
        /* No lineage, so no identity. Reported as empty rather than as a hash
         * of "the path and nothing else", which would look like an identity and
         * would match an unrelated repository at the same path. */
        return ATLAS_OK;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_hex_encode(digest, sizeof(digest), hex);
    return atlas_buf_set_str(out, hex, err);
}

atlas_status atlas_db_decision_detach_repo(atlas_db *db, int64_t repo_id, int64_t *count_out,
                                           atlas_err *err) {
    *count_out = 0;
    /* Unconditional, and that is the whole design. A brand-new repository row
     * starts with no decisions attached whatever else is true, so forgetting to
     * call the *attach* half can only under-attach — which is recoverable and
     * visible — rather than over-attach, which is neither.
     *
     * `repo_id = 0` is the detached marker: no `repositories` row can have it,
     * because rowids start at 1. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE decision_documents SET repo_id = 0 WHERE repo_id = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *count_out = (int64_t)sqlite3_changes(db->h);
    }
    return st;
}

atlas_status atlas_db_decision_forget_legacy_origins(atlas_db *db, const char *repo_name,
                                                     int64_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    sqlite3_stmt *s = NULL;
    /* Selected through `ai_decisions` rather than through the documents, because
     * the question is "which rowids are about to disappear", not "which
     * documents belong here" — a promoted document may already have been
     * detached by an earlier registration and would be missed by the latter. */
    atlas_status st =
        atlas_db_prepare(db,
                         "UPDATE decision_revisions SET imported_from_ai_decision_id = NULL"
                         " WHERE imported_from_ai_decision_id IN ("
                         "   SELECT a.id FROM ai_decisions a"
                         "     JOIN repositories r ON r.id = a.repo_id"
                         "    WHERE r.name = ?1);",
                         &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, repo_name, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK && count_out != NULL) {
        *count_out = (int64_t)sqlite3_changes(db->h);
    }
    return st;
}

atlas_status atlas_db_decision_relink_repo(atlas_db *db, int64_t repo_id,
                                           const char *identity_hash, int64_t *count_out,
                                           atlas_err *err) {
    *count_out = 0;
    if (identity_hash == NULL || identity_hash[0] == '\0') {
        /* No identity, no attachment. This is the case for a repository whose
         * history has not been ingested and for one with an unborn HEAD, and
         * refusing is the point: the alternative is guessing. */
        return ATLAS_OK;
    }
    sqlite3_stmt *s = NULL;
    /* Matched on the identity and on nothing else. Never the path alone, never
     * a name, never a remote. Documents already attached here are skipped so
     * the count means "newly reattached". */
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE decision_documents SET repo_id = ?1"
                                       " WHERE repo_identity_hash = ?2 AND repo_id <> ?1;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, identity_hash, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *count_out = (int64_t)sqlite3_changes(db->h);
    }
    return st;
}

atlas_status atlas_db_decision_relink_after_ingest(atlas_db *db, int64_t repo_id,
                                                   int64_t *count_out, atlas_err *err) {
    *count_out = 0;
    atlas_buf identity = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_repo_identity_hash(db, repo_id, &identity, err);
    if (st != ATLAS_OK || identity.len == 0) {
        atlas_buf_free(&identity);
        return st;
    }

    /* Backfill, before attaching.
     *
     * A document proposed before the repository's history had been ingested
     * recorded an empty identity, because there was no lineage to record. It is
     * *currently attached* to this repository, so writing this repository's
     * identity onto it is not a guess — it is the identity of the repository it
     * demonstrably belongs to right now. Without this, a decision recorded on a
     * freshly registered repository would be permanently unrelinkable, which is
     * fail-closed but needlessly so.
     *
     * Only where the identity is empty. An existing identity is never
     * overwritten: that would let a replaced repository launder its way into
     * matching. */
    /* **`decision_documents` only.** This is attachment metadata: which
     * repository the document is currently considered to belong to, and what a
     * later relink must match. It is deliberately *not* the value any revision
     * hashed — every revision carries its own immutable
     * `basis_repo_identity_hash` — so backfilling here cannot change what an
     * existing revision means, cannot change its canonical hash, and cannot
     * invalidate an approval bound to it.
     *
     * There is no statement anywhere that updates
     * `decision_revisions.basis_repo_identity_hash`. */
    sqlite3_stmt *b = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE decision_documents SET repo_identity_hash = ?2"
                          " WHERE repo_id = ?1 AND repo_identity_hash = '';",
                          &b, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&identity);
        return st;
    }
    st = bind_i64(db, b, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, b, 2, identity.data, identity.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, b);
        atlas_buf_free(&identity);
        return st;
    }
    st = atlas_db_step_done(db, b, err);
    if (st == ATLAS_OK) {
        st = atlas_db_decision_relink_repo(db, repo_id, atlas_buf_cstr(&identity), count_out, err);
    }
    atlas_buf_free(&identity);
    return st;
}

/* --- revisions ------------------------------------------------------------ */

atlas_status atlas_db_decision_revision_insert(atlas_db *db, const atlas_decision_revision *r,
                                               const char *dedup_key, int64_t *id_out,
                                               bool *duplicate_out, atlas_err *err) {
    *id_out = 0;
    *duplicate_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO decision_revisions"
        "(document_id, revision_no, content_hash, title, context_text, decision_text,"
        " rationale_text, consequences_text, scope, proposed_by, session_id, session_unbound,"
        " unbound_reason, basis_head, created_at, state, imported_from_ai_decision_id, dedup_key,"
        " basis_repo_identity_hash)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, 'PROPOSED',"
        "        ?16, ?17, ?18)"
        /* Idempotency, exactly as A2 does it: a replayed request with the same
         * dedup key collides on the partial unique index and is absorbed rather
         * than creating a second revision of the same document. */
        " ON CONFLICT(document_id, dedup_key) WHERE dedup_key IS NOT NULL DO NOTHING;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, r->document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, r->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 3, r->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 4, r->title.data != NULL ? r->title.data : "",
                                  r->title.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 5,
                                  r->context_text.data != NULL ? r->context_text.data : "",
                                  r->context_text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 6,
                                  r->decision_text.data != NULL ? r->decision_text.data : "",
                                  r->decision_text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 7,
                                  r->rationale_text.data != NULL ? r->rationale_text.data : "",
                                  r->rationale_text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(
            db, s, 8, r->consequences_text.data != NULL ? r->consequences_text.data : "",
            r->consequences_text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 9, atlas_decision_scope_name(r->scope), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 10, atlas_decision_actor_name(r->proposed_by), err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 11, r->session_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 12, r->session_unbound ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 13, &r->unbound_reason, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 14, &r->basis_head, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 15, r->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 16, r->imported_from_ai_decision_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 17, dedup_key, err);
    }
    if (st == ATLAS_OK) {
        /* Bound from the revision, never from the document. See the column
         * comment in migrate.c: this value is hashed, so it must be immutable,
         * and the document's copy is not. */
        st = atlas_db_bind_text_n(db, s, 18,
                                  r->basis_repo_identity.data != NULL
                                      ? r->basis_repo_identity.data
                                      : "",
                                  r->basis_repo_identity.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_changes(db->h) == 0) {
        /* Absorbed by the dedup key. Report the row that already exists rather
         * than a bare "duplicate": a caller retrying needs the id it would have
         * been given the first time. */
        *duplicate_out = true;
        sqlite3_stmt *f = NULL;
        st = atlas_db_prepare(
            db, "SELECT id FROM decision_revisions WHERE document_id = ?1 AND dedup_key = ?2;", &f,
            err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = bind_i64(db, f, 1, r->document_id, err);
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, f, 2, dedup_key, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_finish(db, f);
            return st;
        }
        if (sqlite3_step(f) == SQLITE_ROW) {
            *id_out = sqlite3_column_int64(f, 0);
        }
        atlas_db_finish(db, f);
        return ATLAS_OK;
    }
    *id_out = sqlite3_last_insert_rowid(db->h);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_alternative_add(atlas_db *db, int64_t revision_id, int64_t ordinal,
                                               const char *text, size_t len, atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "INSERT INTO decision_alternatives(revision_id, ordinal, text) VALUES(?1, ?2, ?3);", &s,
        err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, ordinal, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 3, text != NULL ? text : "", len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return atlas_db_step_done(db, s, err);
}

atlas_status atlas_db_decision_link_add(atlas_db *db, int64_t revision_id,
                                        const atlas_decision_link *l, int64_t target_document_id,
                                        const char *created_at, atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO decision_links"
        "(revision_id, kind, path_raw, path_text, commit_oid, change_set_id, target_document_id,"
        " symbol_name, symbol_name_text, symbol_kind, symbol_line, basis_commit,"
        " file_content_hash, analyzer_name, analyzer_version, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16);",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, atlas_decision_link_kind_name(l->kind), err);
    }
    if (st == ATLAS_OK) {
        st = bind_blob_opt(db, s, 3, &l->path_raw, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 4, &l->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 5, &l->commit_oid, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 6, l->change_set_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 7, target_document_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_blob_opt(db, s, 8, &l->symbol_name, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 9, &l->symbol_name_text, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 10, &l->symbol_kind, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 11, l->symbol_line, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 12, &l->basis_commit, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 13, &l->file_content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = bind_text_buf_opt(db, s, 14, &l->analyzer_name, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 15, l->analyzer_version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 16, created_at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return atlas_db_step_done(db, s, err);
}

atlas_status atlas_db_decision_search_put(atlas_db *db, int64_t revision_id, int64_t document_id,
                                          int64_t repo_id, const char *haystack, size_t len,
                                          atlas_err *err) {
    /* External-content FTS5 does not see writes to its content table, so the
     * shadow row is maintained explicitly — the same pattern `files_fts` and
     * `commits_fts` already use.
     *
     * **Insert-only, in both tables, and the ON CONFLICT is a no-op rather than
     * an update.** A revision is immutable, so a second write for one revision
     * id can only be a replay; updating the row would be harmless but the
     * matching FTS insert would not, because FTS5 has no upsert and a second
     * `INSERT INTO decisions_fts(rowid, ...)` for one rowid corrupts the index
     * silently. So the conflict is absorbed here and the FTS half is skipped,
     * which keeps the two tables in step by construction. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "INSERT INTO decision_search"
                                       "(revision_id, document_id, repo_id, haystack)"
                                       " VALUES(?1, ?2, ?3, ?4)"
                                       " ON CONFLICT(revision_id) DO NOTHING;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, document_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 4, haystack != NULL ? haystack : "", len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st != ATLAS_OK || sqlite3_changes(db->h) == 0 || !atlas_db_fts_ready(db)) {
        return st;
    }
    sqlite3_stmt *f = NULL;
    st = atlas_db_prepare(db, "INSERT INTO decisions_fts(rowid, haystack) VALUES(?1, ?2);", &f,
                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, f, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, f, 2, haystack != NULL ? haystack : "", len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, f);
        return st;
    }
    return atlas_db_step_done(db, f, err);
}

atlas_status atlas_db_decision_revision_set_state(atlas_db *db, int64_t revision_id,
                                                  const char *from_state, const char *to_state,
                                                  bool *changed_out, atlas_err *err) {
    *changed_out = false;
    /* The one statement in this file that updates `decision_revisions`, and it
     * names exactly one column.
     *
     * `AND state = ?2` is what makes a concurrent transition lose
     * deterministically instead of last-write-wins: two writers both reading
     * PROPOSED and both writing APPROVED cannot both change a row, because the
     * second one's predicate no longer matches. The caller requires
     * `*changed_out` and fails with a typed conflict when it is false. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE decision_revisions SET state = ?3 WHERE id = ?1 AND state = ?2;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, from_state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 3, to_state, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *changed_out = sqlite3_changes(db->h) == 1;
    }
    return st;
}

/* --- the ledger ----------------------------------------------------------- */

atlas_status atlas_db_decision_event_append(atlas_db *db, int64_t document_id, int64_t revision_id,
                                            int64_t revision_no, const char *event,
                                            const char *actor, const char *content_hash,
                                            int64_t challenge_id,
                                            int64_t superseded_by_revision_id,
                                            int64_t superseded_by_document_id, const char *detail,
                                            const char *dedup_key, bool *inserted_out,
                                            atlas_err *err) {
    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO decision_events"
        "(document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
        " superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
        " ON CONFLICT(document_id, dedup_key) WHERE dedup_key IS NOT NULL DO NOTHING;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 2, revision_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 4, event, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 5, actor, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 6, content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 7, challenge_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 8, superseded_by_revision_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 9, superseded_by_document_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 10, detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 11, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 12, dedup_key, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK && inserted_out != NULL) {
        *inserted_out = sqlite3_changes(db->h) == 1;
    }
    return st;
}

/* --- the operator channel -------------------------------------------------- */

atlas_status atlas_db_decision_challenge_insert(atlas_db *db, const atlas_decision_challenge *c,
                                                int64_t *id_out, atlas_err *err) {
    *id_out = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO decision_challenges"
        "(token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
        " supersede_document_id, created_at, expires_at, consumed)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0);",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, c->token, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, c->repo_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, c->document_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 4, c->revision_id, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 5, c->revision_no, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 6, c->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 7, atlas_decision_intent_name(c->intent), err);
    }
    if (st == ATLAS_OK) {
        st = bind_id_opt(db, s, 8, c->supersede_document_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 9, c->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 10, c->expires_at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *id_out = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

atlas_status atlas_db_decision_challenge_find(atlas_db *db, const char *token,
                                              atlas_decision_challenge *out, bool *found_out,
                                              atlas_err *err) {
    atlas_decision_challenge_init(out);
    *found_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
        "       supersede_document_id, created_at, expires_at, consumed"
        "  FROM decision_challenges WHERE token = ?1;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, token, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        out->id = sqlite3_column_int64(s, 0);
        st = atlas_db_col_copy(s, 1, out->token, sizeof(out->token), "challenge token", err);
        out->repo_id = sqlite3_column_int64(s, 2);
        out->document_id = sqlite3_column_int64(s, 3);
        out->revision_id = sqlite3_column_int64(s, 4);
        out->revision_no = sqlite3_column_int64(s, 5);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 6, out->content_hash, sizeof(out->content_hash),
                                   "challenge content hash", err);
        }
        if (st == ATLAS_OK) {
            if (!atlas_decision_intent_parse(atlas_db_col_text(s, 7), &out->intent)) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "a stored approval challenge has an intent Atlas does not "
                                   "recognise, and is refused rather than guessed");
            }
        }
        out->supersede_document_id = sqlite3_column_int64(s, 8);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 9, out->created_at, sizeof(out->created_at), "created_at",
                                   err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 10, out->expires_at, sizeof(out->expires_at), "expires_at",
                                   err);
        }
        out->consumed = sqlite3_column_int64(s, 11) != 0;
        *found_out = st == ATLAS_OK;
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot look up an approval challenge");
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_challenge_consume(atlas_db *db, int64_t challenge_id,
                                                 const char *at, bool *changed_out,
                                                 atlas_err *err) {
    *changed_out = false;
    /* `AND consumed = 0` is the replay rejection, and it is here rather than in
     * a read-then-write in the caller for the usual reason: a check followed by
     * a write is two statements with a window between them, and this is one. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE decision_challenges"
                                       " SET consumed = 1, consumed_at = ?2"
                                       " WHERE id = ?1 AND consumed = 0;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, challenge_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, at, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *changed_out = sqlite3_changes(db->h) == 1;
    }
    return st;
}

atlas_status atlas_db_decision_challenges_prune(atlas_db *db, const char *now, int64_t retain,
                                                int64_t *removed_out, atlas_err *err) {
    *removed_out = 0;
    /* The only DELETE in this file.
     *
     * `consumed = 0` is not an optimisation: a consumed challenge is part of an
     * approval record and `decision_events.challenge_id` points at it, so
     * removing one would break the ledger's account of how a transition was
     * authorised. Expired, unspent capabilities are the only rows this can
     * reach, and they authorised nothing. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "DELETE FROM decision_challenges"
                                       " WHERE consumed = 0 AND expires_at < ?1"
                                       "   AND id NOT IN ("
                                       "     SELECT id FROM decision_challenges"
                                       "      WHERE consumed = 0 ORDER BY id DESC LIMIT ?2);",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, s, 1, now, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, retain, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    st = atlas_db_step_done(db, s, err);
    if (st == ATLAS_OK) {
        *removed_out = (int64_t)sqlite3_changes(db->h);
    }
    return st;
}

/* --- reads ----------------------------------------------------------------- */

atlas_status atlas_db_decision_current_revision(atlas_db *db, int64_t document_id,
                                                int64_t *revision_id_out, atlas_err *err) {
    *revision_id_out = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT COALESCE(current_revision_id, 0) FROM decision_documents WHERE id = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *revision_id_out = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_document_shape(atlas_db *db, int64_t document_id,
                                              int64_t *superseded_by_out, int64_t *proposed_out,
                                              atlas_err *err) {
    *superseded_by_out = 0;
    *proposed_out = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT COALESCE(d.superseded_by_document_id, 0),"
        "       (SELECT COUNT(*) FROM decision_revisions r"
        "         WHERE r.document_id = d.id AND r.state = 'PROPOSED')"
        "  FROM decision_documents d WHERE d.id = ?1;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *superseded_by_out = sqlite3_column_int64(s, 0);
        *proposed_out = sqlite3_column_int64(s, 1);
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_latest_revision(atlas_db *db, int64_t document_id, int64_t *id_out,
                                               int64_t *no_out, char *hash_out, size_t hash_size,
                                               char *state_out, size_t state_size,
                                               atlas_err *err) {
    *id_out = 0;
    *no_out = 0;
    if (hash_size > 0) {
        hash_out[0] = '\0';
    }
    if (state_size > 0) {
        state_out[0] = '\0';
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT id, revision_no, content_hash, state"
                                       "  FROM decision_revisions WHERE document_id = ?1"
                                       "  ORDER BY revision_no DESC LIMIT 1;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(s, 0);
        *no_out = sqlite3_column_int64(s, 1);
        st = atlas_db_col_copy(s, 2, hash_out, hash_size, "content hash", err);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(s, 3, state_out, state_size, "revision state", err);
        }
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_revision_by_no(atlas_db *db, int64_t document_id,
                                              int64_t revision_no, int64_t *id_out,
                                              bool *found_out, atlas_err *err) {
    *id_out = 0;
    *found_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT id FROM decision_revisions WHERE document_id = ?1 AND revision_no = ?2;", &s,
        err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, revision_no, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *id_out = sqlite3_column_int64(s, 0);
        *found_out = true;
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

static atlas_status load_alternatives(atlas_db *db, atlas_decision_revision *out, atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT text FROM decision_alternatives"
                                       " WHERE revision_id = ?1 ORDER BY ordinal LIMIT ?2;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, out->id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, ATLAS_DECISION_MAX_ALTERNATIVES, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        if (out->alternative_count >= ATLAS_DECISION_MAX_ALTERNATIVES) {
            break;
        }
        st = set_buf_from_col(&out->alternatives[out->alternative_count], s, 0, err);
        if (st == ATLAS_OK) {
            out->alternative_count++;
        }
    }
    atlas_db_finish(db, s);
    return st;
}

static atlas_status load_links(atlas_db *db, atlas_decision_revision *out, atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT l.kind, l.path_raw, l.path_text, l.commit_oid, l.change_set_id,"
        "       l.symbol_name, l.symbol_name_text, l.symbol_kind, l.symbol_line,"
        "       l.basis_commit, l.file_content_hash, l.analyzer_name, l.analyzer_version,"
        "       d.uid"
        "  FROM decision_links l"
        "  LEFT JOIN decision_documents d ON d.id = l.target_document_id"
        " WHERE l.revision_id = ?1 ORDER BY l.id LIMIT ?2;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, out->id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, ATLAS_DECISION_MAX_LINKS, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        if (out->link_count >= ATLAS_DECISION_MAX_LINKS) {
            break;
        }
        atlas_decision_link_kind kind = ATLAS_DECISION_LINK_PATH;
        if (!atlas_decision_link_kind_parse(atlas_db_col_text(s, 0), &kind)) {
            /* A stored kind Atlas does not recognise is a corrupt row, not a
             * new feature. Refusing beats guessing which column matters. */
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "a decision link has a kind Atlas does not recognise");
            break;
        }
        atlas_decision_link *l = &out->links[out->link_count];
        atlas_decision_link_init(l, kind);
        st = set_buf_from_col(&l->path_raw, s, 1, err);
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->path_text, s, 2, err);
        }
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->commit_oid, s, 3, err);
        }
        l->change_set_id = sqlite3_column_int64(s, 4);
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->symbol_name, s, 5, err);
        }
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->symbol_name_text, s, 6, err);
        }
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->symbol_kind, s, 7, err);
        }
        l->symbol_line = sqlite3_column_int64(s, 8);
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->basis_commit, s, 9, err);
        }
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->file_content_hash, s, 10, err);
        }
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->analyzer_name, s, 11, err);
        }
        l->analyzer_version = sqlite3_column_int64(s, 12);
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l->target_uid, s, 13, err);
        }
        if (st == ATLAS_OK) {
            out->link_count++;
        } else {
            atlas_decision_link_free(l);
        }
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_revision_load(atlas_db *db, int64_t revision_id,
                                             atlas_decision_revision *out, bool *found_out,
                                             atlas_err *err) {
    *found_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        /* The repository identity comes from the **revision**, not from the
         * owning document.
         *
         * It is a hashed input, so a rehash has to see exactly the bytes the
         * original hash saw. The document's `repo_identity_hash` is attachment
         * metadata and is backfilled when a repository's lineage first becomes
         * knowable — so joining it here made an ordinary propose-then-scan
         * change the verification input of an already-written revision, and
         * `atlas doctor` reported a healthy record as corrupt. */
        "SELECT document_id, revision_no, content_hash, title, context_text,"
        "       decision_text, rationale_text, consequences_text, scope, proposed_by,"
        "       session_id, session_unbound, unbound_reason, basis_head, created_at,"
        "       state, imported_from_ai_decision_id, basis_repo_identity_hash"
        "  FROM decision_revisions WHERE id = ?1;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, revision_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) != SQLITE_ROW) {
        atlas_db_finish(db, s);
        return ATLAS_OK;
    }
    out->id = revision_id;
    out->document_id = sqlite3_column_int64(s, 0);
    out->revision_no = sqlite3_column_int64(s, 1);
    st = atlas_db_col_copy(s, 2, out->content_hash, sizeof(out->content_hash), "content hash", err);
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->title, s, 3, err);
    }
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->context_text, s, 4, err);
    }
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->decision_text, s, 5, err);
    }
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->rationale_text, s, 6, err);
    }
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->consequences_text, s, 7, err);
    }
    if (st == ATLAS_OK && !atlas_decision_scope_parse(atlas_db_col_text(s, 8), &out->scope)) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "a decision revision has a scope Atlas does not recognise");
    }
    if (st == ATLAS_OK &&
        !atlas_decision_actor_parse(atlas_db_col_text(s, 9), &out->proposed_by)) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "a decision revision has a proposer Atlas does not recognise");
    }
    out->session_id = sqlite3_column_int64(s, 10);
    out->session_unbound = sqlite3_column_int64(s, 11) != 0;
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->unbound_reason, s, 12, err);
    }
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->basis_head, s, 13, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_col_copy(s, 14, out->created_at, sizeof(out->created_at), "created_at", err);
    }
    if (st == ATLAS_OK && !atlas_decision_state_parse(atlas_db_col_text(s, 15), &out->state)) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "a decision revision has a state Atlas does not recognise");
    }
    out->imported_from_ai_decision_id = sqlite3_column_int64(s, 16);
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->basis_repo_identity, s, 17, err);
    }
    atlas_db_finish(db, s);
    if (st != ATLAS_OK) {
        return st;
    }
    st = load_alternatives(db, out, err);
    if (st == ATLAS_OK) {
        st = load_links(db, out, err);
    }
    if (st == ATLAS_OK) {
        *found_out = true;
    }
    return st;
}

/* The head revision of a document: the approved one when there is one, the
 * newest otherwise.
 *
 * `COALESCE(d.current_revision_id, <newest>)` rather than a second cached
 * column. The correlated subquery is a seek on `idx_decision_rev_doc`, once per
 * returned row, and every caller of this shape bounds its page — so the cost is
 * a bounded number of seeks, against one more cached column that would have to
 * be kept honest by every transition. */
/* **The match set drives, and the document table is joined to it.**
 *
 * The obvious spelling — `WHERE d.repo_id = ?1 AND d.id IN (<match>)` with an
 * `ORDER BY d.id DESC LIMIT n` — reads correctly and is linear in the
 * *repository* rather than in the result. SQLite satisfies the ORDER BY by
 * walking `decision_documents` in id order, evaluating the correlated
 * head-revision subquery and the link count for every row, and testing each
 * against the match set; a query matching a handful of documents therefore
 * pays for all of them. Measured on the ten-thousand-document acceptance
 * fixture that was 1.5 seconds for a search and 0.6 seconds for one that
 * matched nothing, against a 100 ms budget. `scripts/perf-a4.sh` is what
 * caught it.
 *
 * Driving from the match set instead — a bounded inner SELECT joined to the
 * document table — makes the outer loop as short as the result, and the
 * per-row subqueries run once per returned row rather than once per document.
 * The LIMIT moves inside for the same reason: it has to bound the *scan*, not
 * the output.
 *
 * `SELECT DISTINCT document_id` inside, because a document with three matching
 * revisions is one result and a LIMIT over the undistinct set would return
 * fewer documents than were asked for. */
#define DECISION_DOC_SELECT                                                                        \
    "SELECT d.id, d.uid, d.repo_id, d.created_at, d.updated_at, d.current_status,"                 \
    "       d.latest_revision_no, COALESCE(d.current_revision_id, 0),"                             \
    "       r.id, r.revision_no, r.state, r.title, r.content_hash, r.proposed_by,"                 \
    "       sup.uid,"                                                                              \
    "       (SELECT COUNT(*) FROM decision_links dl WHERE dl.revision_id = r.id)"                  \
    "  FROM decision_documents d"                                                                  \
    "  LEFT JOIN decision_revisions r ON r.id = COALESCE(d.current_revision_id,"                   \
    "       (SELECT id FROM decision_revisions x WHERE x.document_id = d.id"                       \
    "         ORDER BY x.revision_no DESC LIMIT 1))"                                               \
    "  LEFT JOIN decision_documents sup ON sup.id = d.superseded_by_document_id"

static atlas_status emit_doc_rows(atlas_db *db, sqlite3_stmt *s, int64_t limit,
                                  atlas_decision_doc_cb cb, void *ud, int64_t *count_out,
                                  bool *more_out, atlas_err *err) {
    int64_t n = 0;
    atlas_status st = ATLAS_OK;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n >= limit) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_decision_doc_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(s, 0);
        row.uid = atlas_db_col_text(s, 1);
        row.repo_id = sqlite3_column_int64(s, 2);
        row.created_at = atlas_db_col_text(s, 3);
        row.updated_at = atlas_db_col_text(s, 4);
        row.status = atlas_db_col_text(s, 5);
        row.latest_revision_no = sqlite3_column_int64(s, 6);
        row.current_revision_id = sqlite3_column_int64(s, 7);
        row.head_revision_id = sqlite3_column_int64(s, 8);
        row.head_revision_no = sqlite3_column_int64(s, 9);
        row.head_state = atlas_db_col_text(s, 10);
        row.title = atlas_db_col_text(s, 11);
        row.content_hash = atlas_db_col_text(s, 12);
        row.proposed_by = atlas_db_col_text(s, 13);
        row.superseded_by_uid = atlas_db_col_text_opt(s, 14);
        row.link_count = sqlite3_column_int64(s, 15);
        st = cb(&row, ud, err);
        if (st != ATLAS_OK) {
            break;
        }
        n++;
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    atlas_db_finish(db, s);
    return st;
}

/* Decision documents attached to no live repository.
 *
 * `repo_id = 0` is the detached marker, and a document whose `repo_id` names a
 * repository row that no longer exists is orphaned too — the second case
 * happens when a repository is removed by something that did not go through
 * the detach path. Both are listed, because both are invisible everywhere else
 * and a canonical record that has become invisible looks exactly like one that
 * was deleted. */
atlas_status atlas_db_decision_orphans_list(atlas_db *db, int64_t limit,
                                            atlas_decision_doc_cb cb, void *ud,
                                            int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        DECISION_DOC_SELECT
        " WHERE d.repo_id = 0 OR d.repo_id NOT IN (SELECT id FROM repositories)"
        " ORDER BY d.id DESC LIMIT ?1;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, limit + 1, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return emit_doc_rows(db, s, limit, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_decision_documents_list(atlas_db *db, int64_t repo_id, const char *status,
                                              int64_t limit, atlas_decision_doc_cb cb, void *ud,
                                              int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    sqlite3_stmt *s = NULL;
    /* `?2 IS NULL OR d.current_status = ?2` rather than two statements: the
     * planner uses `idx_decision_docs_status` when the parameter is bound and
     * `idx_decision_docs_repo` when it is not, and one statement is one place
     * for the projection to be right. The LIMIT is `?3 + 1` so that "there are
     * more" is observed rather than assumed from a full page. */
    atlas_status st = atlas_db_prepare(db,
                                       DECISION_DOC_SELECT
                                       " WHERE d.repo_id = ?1"
                                       "   AND (?2 IS NULL OR d.current_status = ?2)"
                                       " ORDER BY d.id DESC LIMIT ?3;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, s, 2, status, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, limit + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return emit_doc_rows(db, s, limit, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_decision_document_row(atlas_db *db, int64_t document_id,
                                            atlas_decision_doc_cb cb, void *ud, bool *found_out,
                                            atlas_err *err) {
    *found_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, DECISION_DOC_SELECT " WHERE d.id = ?1;", &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int64_t n = 0;
    st = emit_doc_rows(db, s, 1, cb, ud, &n, NULL, err);
    *found_out = n > 0;
    return st;
}

atlas_status atlas_db_decision_for_path(atlas_db *db, int64_t repo_id, const void *path_raw,
                                        size_t path_len, int64_t limit, atlas_decision_doc_cb cb,
                                        void *ud, int64_t *count_out, bool *more_out,
                                        atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    sqlite3_stmt *s = NULL;
    /* Seeks from the path bytes through `idx_decision_links_path`, then narrows
     * to this repository's documents. The link may belong to any revision of a
     * document, not only the head: a decision whose second revision dropped a
     * path is still a decision that once concerned it, and hiding it would make
     * "which decisions touch this file?" quietly incomplete. The document is
     * reported once, at its head. */
    atlas_status st = atlas_db_prepare(db,
                                       DECISION_DOC_SELECT
                                       "  JOIN (SELECT DISTINCT rv.document_id AS did"
                                       "          FROM decision_links dl"
                                       "          JOIN decision_revisions rv"
                                       "            ON rv.id = dl.revision_id"
                                       "         WHERE dl.path_raw = ?2"
                                       "         LIMIT ?3) m ON m.did = d.id"
                                       " WHERE d.repo_id = ?1"
                                       " ORDER BY d.id DESC;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, s, 2, path_raw, path_len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, limit + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    return emit_doc_rows(db, s, limit, cb, ud, count_out, more_out, err);
}

atlas_status atlas_db_decision_revisions_list(atlas_db *db, int64_t document_id, int64_t limit,
                                              atlas_decision_rev_cb cb, void *ud,
                                              int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT id, revision_no, content_hash, title, state, proposed_by, created_at, basis_head,"
        "       session_id, session_unbound, unbound_reason, imported_from_ai_decision_id"
        "  FROM decision_revisions WHERE document_id = ?1"
        " ORDER BY revision_no DESC LIMIT ?2;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, limit + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int64_t n = 0;
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        if (n >= limit) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_decision_rev_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(s, 0);
        row.revision_no = sqlite3_column_int64(s, 1);
        row.content_hash = atlas_db_col_text(s, 2);
        row.title = atlas_db_col_text(s, 3);
        row.state = atlas_db_col_text(s, 4);
        row.proposed_by = atlas_db_col_text(s, 5);
        row.created_at = atlas_db_col_text(s, 6);
        row.basis_head = atlas_db_col_text_opt(s, 7);
        row.session_id = sqlite3_column_int64(s, 8);
        row.session_unbound = sqlite3_column_int64(s, 9) != 0;
        row.unbound_reason = atlas_db_col_text_opt(s, 10);
        row.imported_from_ai_decision_id = sqlite3_column_int64(s, 11);
        st = cb(&row, ud, err);
        n++;
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_events_list(atlas_db *db, int64_t document_id, int64_t limit,
                                           atlas_decision_event_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    sqlite3_stmt *s = NULL;
    /* Ascending: a timeline reads forwards. Every other listing in Atlas is
     * newest-first because it is a sample of a large set; this is the whole
     * history of one document and its order is its meaning. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT e.id, COALESCE(e.revision_id, 0), e.revision_no, e.event, e.actor,"
        "       e.content_hash, COALESCE(e.challenge_id, 0),"
        "       COALESCE(e.superseded_by_revision_id, 0), sup.uid, e.detail, e.created_at"
        "  FROM decision_events e"
        "  LEFT JOIN decision_documents sup ON sup.id = e.superseded_by_document_id"
        " WHERE e.document_id = ?1 ORDER BY e.id ASC LIMIT ?2;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, limit + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int64_t n = 0;
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        if (n >= limit) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_decision_event_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(s, 0);
        row.revision_id = sqlite3_column_int64(s, 1);
        row.revision_no = sqlite3_column_int64(s, 2);
        row.event = atlas_db_col_text(s, 3);
        row.actor = atlas_db_col_text(s, 4);
        row.content_hash = atlas_db_col_text_opt(s, 5);
        row.challenge_id = sqlite3_column_int64(s, 6);
        row.superseded_by_revision_id = sqlite3_column_int64(s, 7);
        row.superseded_by_uid = atlas_db_col_text_opt(s, 8);
        row.detail = atlas_db_col_text_opt(s, 9);
        row.created_at = atlas_db_col_text(s, 10);
        st = cb(&row, ud, err);
        n++;
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    atlas_db_finish(db, s);
    return st;
}

/* --- search ---------------------------------------------------------------
 *
 * FTS5 over `decision_search` when the linked SQLite build has it, and a
 * repository-filtered scan of the same narrow table when it does not.
 *
 * The query is a model's or an operator's, so neither path exposes a query
 * language: the FTS path quotes the whole query as one phrase, and the degraded
 * path is a literal substring test with LIKE's own metacharacters escaped. A
 * caller cannot reach the FTS expression grammar or the LIKE wildcard set,
 * which is the same decision A2 made about `ai.memory.search` and for the same
 * reason — a search that a caller can turn into a different query is a search
 * whose cost is not bounded by anything Atlas chose. */

static atlas_status build_like_pattern(atlas_buf *out, const char *query, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append_ch(out, '%', err);
    for (const char *p = query; st == ATLAS_OK && *p != '\0'; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') {
            st = atlas_buf_append_ch(out, '\\', err);
        }
        if (st == ATLAS_OK) {
            /* Lowercased to match the stored haystack, which is lowercased when
             * it is written. ASCII only: a locale-dependent fold would make the
             * result depend on the environment. */
            char c = *p;
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            st = atlas_buf_append_ch(out, c, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '%', err);
    }
    return st;
}

static atlas_status build_fts_phrase(atlas_buf *out, const char *query, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append_ch(out, '"', err);
    for (const char *p = query; st == ATLAS_OK && *p != '\0'; p++) {
        if (*p == '"') {
            st = atlas_buf_append_ch(out, '"', err); /* FTS5 doubles an embedded quote */
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(out, *p, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '"', err);
    }
    return st;
}

atlas_status atlas_db_decision_search(atlas_db *db, int64_t repo_id, const char *query,
                                      int64_t limit, atlas_decision_doc_cb cb, void *ud,
                                      int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    if (query == NULL || query[0] == '\0') {
        return ATLAS_OK;
    }
    atlas_buf pattern = ATLAS_BUF_INIT;
    sqlite3_stmt *s = NULL;
    atlas_status st;
    if (atlas_db_fts_ready(db)) {
        st = build_fts_phrase(&pattern, query, err);
        if (st == ATLAS_OK) {
            st = atlas_db_prepare(db,
                                  DECISION_DOC_SELECT
                                  "  JOIN (SELECT DISTINCT ds.document_id AS did"
                                  "          FROM decisions_fts f"
                                  "          JOIN decision_search ds ON ds.revision_id = f.rowid"
                                  "         WHERE f.haystack MATCH ?2 AND ds.repo_id = ?1"
                                  "         LIMIT ?3) m ON m.did = d.id"
                                  " WHERE d.repo_id = ?1"
                                  " ORDER BY d.id DESC;",
                                  &s, err);
        }
    } else {
        st = build_like_pattern(&pattern, query, err);
        if (st == ATLAS_OK) {
            st = atlas_db_prepare(db,
                                  DECISION_DOC_SELECT
                                  "  JOIN (SELECT DISTINCT ds.document_id AS did"
                                  "          FROM decision_search ds"
                                  "         WHERE ds.repo_id = ?1"
                                  "           AND ds.haystack LIKE ?2 ESCAPE '\\'"
                                  "         LIMIT ?3) m ON m.did = d.id"
                                  " WHERE d.repo_id = ?1"
                                  " ORDER BY d.id DESC;",
                                  &s, err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&pattern);
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 2, pattern.data, pattern.len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, limit + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        atlas_buf_free(&pattern);
        return st;
    }
    st = emit_doc_rows(db, s, limit, cb, ud, count_out, more_out, err);
    atlas_buf_free(&pattern);
    return st;
}

/* --- counts ---------------------------------------------------------------- */

atlas_status atlas_db_decision_repo_counts(atlas_db *db, int64_t repo_id, int64_t *proposed,
                                           int64_t *approved, int64_t *rejected,
                                           int64_t *superseded, atlas_err *err) {
    *proposed = 0;
    *approved = 0;
    *rejected = 0;
    *superseded = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT current_status, COUNT(*) FROM decision_documents"
                                       " WHERE repo_id = ?1 GROUP BY current_status;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *k = atlas_db_col_text(s, 0);
        int64_t n = sqlite3_column_int64(s, 1);
        if (strcmp(k, "PROPOSED") == 0) {
            *proposed = n;
        } else if (strcmp(k, "APPROVED") == 0) {
            *approved = n;
        } else if (strcmp(k, "REJECTED") == 0) {
            *rejected = n;
        } else if (strcmp(k, "SUPERSEDED") == 0) {
            *superseded = n;
        }
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_review_count(atlas_db *db, int64_t repo_id, int64_t *count_out,
                                            atlas_err *err) {
    *count_out = 0;
    sqlite3_stmt *s = NULL;
    /* Seeks throughout: approved documents for this repository through
     * `idx_decision_docs_status`, the effective revision by row id, its links
     * through `idx_decision_links_rev`, and the file through the unique
     * `(repo_id, path_raw)` index on `files`.
     *
     * `IS NOT` rather than `<>` on the hash comparison, because SQL's `<>` is
     * NULL for a NULL operand and a file the index has no hash for would then
     * silently count as unchanged. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT COUNT(DISTINCT d.id)"
        "  FROM decision_documents d"
        "  JOIN decision_revisions r ON r.id = d.current_revision_id"
        "  JOIN decision_links l ON l.revision_id = r.id"
        "  LEFT JOIN files f ON f.repo_id = d.repo_id AND f.path_raw = l.path_raw"
        " WHERE d.repo_id = ?1 AND d.current_status = 'APPROVED'"
        "   AND l.path_raw IS NOT NULL AND l.file_content_hash IS NOT NULL"
        "   AND (f.id IS NULL OR f.deleted = 1 OR f.content_hash IS NOT l.file_content_hash);",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *count_out = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

atlas_status atlas_db_decision_path_counts(atlas_db *db, int64_t repo_id, const void *path_raw,
                                           size_t path_len, int64_t *approved_out,
                                           int64_t *proposed_out, atlas_err *err) {
    *approved_out = 0;
    *proposed_out = 0;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT d.current_status, COUNT(DISTINCT d.id)"
                                       "  FROM decision_documents d"
                                       "  JOIN decision_revisions rv ON rv.document_id = d.id"
                                       "  JOIN decision_links dl ON dl.revision_id = rv.id"
                                       " WHERE d.repo_id = ?1 AND dl.path_raw = ?2"
                                       " GROUP BY d.current_status;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, s, 2, path_raw, path_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *k = atlas_db_col_text(s, 0);
        int64_t n = sqlite3_column_int64(s, 1);
        if (strcmp(k, "APPROVED") == 0) {
            *approved_out = n;
        } else if (strcmp(k, "PROPOSED") == 0) {
            *proposed_out = n;
        }
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

/* --- link currency ---------------------------------------------------------
 *
 * Computed here, on read, from the snapshot and the current index. Never
 * stored, and never used to change the link: Atlas reports that an anchor moved
 * and leaves the record alone. */

static atlas_status resolve_path_link(atlas_db *db, int64_t repo_id, atlas_decision_link *l,
                                      bool file_index_known, atlas_err *err) {
    if (!file_index_known) {
        l->currency = ATLAS_DECISION_LINK_UNKNOWN;
        return ATLAS_OK;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT content_hash, deleted FROM files WHERE repo_id = ?1 AND path_raw = ?2;", &s,
        err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, s, 2, l->path_raw.data, l->path_raw.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) != SQLITE_ROW) {
        l->currency = ATLAS_DECISION_LINK_MISSING;
        l->match_count = 0;
        atlas_db_finish(db, s);
        return ATLAS_OK;
    }
    const char *hash = atlas_db_col_text_opt(s, 0);
    bool deleted = sqlite3_column_int64(s, 1) != 0;
    l->match_count = 1;
    if (deleted) {
        l->currency = ATLAS_DECISION_LINK_MISSING;
    } else if (l->file_content_hash.len == 0 || hash == NULL) {
        /* Nothing recorded to compare against. "The file is there and Atlas
         * cannot say whether it is the same file" is UNKNOWN, not CURRENT. */
        l->currency = ATLAS_DECISION_LINK_UNKNOWN;
    } else if (strncmp(hash, l->file_content_hash.data, l->file_content_hash.len) == 0 &&
               strlen(hash) == l->file_content_hash.len) {
        l->currency = ATLAS_DECISION_LINK_CURRENT;
    } else {
        l->currency = ATLAS_DECISION_LINK_CHANGED;
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

/* Counts definition sites of the snapshot's symbol, optionally within one file,
 * and reports the content hash of the file the single match was found in. */
static atlas_status count_symbol_sites(atlas_db *db, int64_t repo_id,
                                       const atlas_decision_link *l, bool in_recorded_file,
                                       int64_t *count_out, atlas_buf *hash_out, atlas_err *err) {
    *count_out = 0;
    atlas_buf_reset(hash_out);
    sqlite3_stmt *s = NULL;
    /* Definition sites only. A declaration is not where a symbol lives, and
     * counting header prototypes would make almost every symbol ambiguous.
     *
     * The kind is part of the selector when the snapshot recorded one: a
     * `struct config` and a `config` variable are different anchors. An empty
     * recorded kind matches any, which is the honest behaviour for a snapshot
     * that never had one. */
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT COUNT(*), MIN(f.content_hash)"
        "  FROM code_symbols sy"
        "  JOIN code_files f ON f.id = sy.code_file_id"
        " WHERE sy.repo_id = ?1 AND sy.name = ?2 AND sy.is_definition = 1"
        "   AND (?3 = '' OR sy.kind = ?3)"
        "   AND (?4 = 0 OR f.path_raw = ?5);",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, s, 2, l->symbol_name.data, l->symbol_name.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_n(db, s, 3, l->symbol_kind.data != NULL ? l->symbol_kind.data : "",
                                  l->symbol_kind.len, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 4, in_recorded_file ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, s, 5, l->path_raw.data != NULL ? l->path_raw.data : "",
                                l->path_raw.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        *count_out = sqlite3_column_int64(s, 0);
        const char *hash = atlas_db_col_text_opt(s, 1);
        if (hash != NULL) {
            st = atlas_buf_set_str(hash_out, hash, err);
        }
    }
    atlas_db_finish(db, s);
    return st;
}

static atlas_decision_link_currency compare_hash(const atlas_decision_link *l,
                                                 const atlas_buf *found) {
    if (l->file_content_hash.len == 0 || found->len == 0) {
        /* Nothing recorded to compare against, or nothing indexed to compare
         * with. "It is there and Atlas cannot say whether it is the same" is
         * UNKNOWN, not CURRENT. */
        return ATLAS_DECISION_LINK_UNKNOWN;
    }
    if (found->len == l->file_content_hash.len &&
        memcmp(found->data, l->file_content_hash.data, found->len) == 0) {
        return ATLAS_DECISION_LINK_CURRENT;
    }
    return ATLAS_DECISION_LINK_CHANGED;
}

static atlas_status resolve_symbol_link(atlas_db *db, int64_t repo_id, atlas_decision_link *l,
                                        bool code_index_known, atlas_err *err) {
    if (!code_index_known) {
        l->currency = ATLAS_DECISION_LINK_UNKNOWN;
        return ATLAS_OK;
    }
    atlas_buf hash = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;

    /* **The recorded file is part of the selector, and is consulted first.**
     *
     * A snapshot says "the symbol `helper`, of kind `function`, in
     * `src/db/db.c`". Searching the whole repository for `helper` and calling
     * the result ambiguous would make every `static` name in a large C project
     * permanently ambiguous — which is not a bound on Atlas' knowledge, it is
     * Atlas throwing away a field it recorded precisely so this would work.
     *
     * So: if the snapshot named a file and the symbol is still defined there,
     * that is the anchor, and the only remaining question is whether the file's
     * content moved. Only when it is *not* there does the repository-wide
     * lookup run, and then purely to distinguish "gone" from "gone from here
     * and now defined elsewhere" — which is reported as MISSING with a
     * candidate count, never re-pointed. */
    if (l->path_raw.len > 0) {
        int64_t here = 0;
        st = count_symbol_sites(db, repo_id, l, true, &here, &hash, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&hash);
            return st;
        }
        if (here == 1) {
            l->match_count = 1;
            l->currency = compare_hash(l, &hash);
            atlas_buf_free(&hash);
            return ATLAS_OK;
        }
        if (here > 1) {
            /* Two definitions of one name in one file: only reachable through
             * preprocessor conditions, and exactly the case A3 refuses to
             * choose between. */
            l->match_count = here;
            l->currency = ATLAS_DECISION_LINK_AMBIGUOUS;
            atlas_buf_free(&hash);
            return ATLAS_OK;
        }
    }

    int64_t anywhere = 0;
    st = count_symbol_sites(db, repo_id, l, false, &anywhere, &hash, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&hash);
        return st;
    }
    l->match_count = anywhere;
    if (anywhere == 0) {
        /* Renamed, deleted, or moved behind a preprocessor condition. Atlas
         * will not guess which, and above all will not pick a nearby symbol
         * with a similar name. */
        l->currency = ATLAS_DECISION_LINK_MISSING;
    } else if (l->path_raw.len > 0) {
        /* The snapshot named a file and the symbol is no longer defined there.
         * It exists elsewhere — possibly moved, possibly a different symbol
         * that happens to share the name — and deciding which would be
         * inventing. The anchor is reported MISSING and the count says how many
         * other definitions exist, so a reader can go and look. */
        l->currency = ATLAS_DECISION_LINK_MISSING;
    } else if (anywhere > 1) {
        l->currency = ATLAS_DECISION_LINK_AMBIGUOUS;
    } else {
        l->currency = compare_hash(l, &hash);
    }
    atlas_buf_free(&hash);
    return ATLAS_OK;
}

static atlas_status resolve_exists(atlas_db *db, const char *sql, int64_t id_param,
                                   const char *text_param, size_t text_len,
                                   atlas_decision_link *l, atlas_decision_link_currency absent,
                                   atlas_err *err) {
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, sql, &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (text_param != NULL) {
        st = atlas_db_bind_text_n(db, s, 1, text_param, text_len, err);
        if (st == ATLAS_OK && id_param > 0) {
            st = bind_i64(db, s, 2, id_param, err);
        }
    } else {
        st = bind_i64(db, s, 1, id_param, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) == SQLITE_ROW) {
        l->currency = ATLAS_DECISION_LINK_CURRENT;
        l->match_count = 1;
    } else {
        l->currency = absent;
        l->match_count = 0;
    }
    atlas_db_finish(db, s);
    return ATLAS_OK;
}

atlas_status atlas_db_code_symbol_definition_site(atlas_db *db, int64_t repo_id, const void *name,
                                                  size_t name_len, atlas_buf *path_raw_out,
                                                  atlas_buf *content_hash_out,
                                                  int64_t *matches_out, atlas_err *err) {
    atlas_buf_reset(path_raw_out);
    atlas_buf_reset(content_hash_out);
    *matches_out = 0;
    sqlite3_stmt *s = NULL;
    /* Two definition sites are enough to know it is ambiguous, so the scan
     * stops at two rather than counting every same-named static in the
     * repository — which, for a name like `helper`, is one per file. */
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT f.path_raw, f.content_hash"
                                       "  FROM code_symbols sy"
                                       "  JOIN code_files f ON f.id = sy.code_file_id"
                                       " WHERE sy.repo_id = ?1 AND sy.name = ?2"
                                       "   AND sy.is_definition = 1"
                                       " LIMIT 2;",
                                       &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_blob(db, s, 2, name, name_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        (*matches_out)++;
        if (*matches_out == 1) {
            st = set_buf_from_col(path_raw_out, s, 0, err);
            if (st == ATLAS_OK) {
                st = set_buf_from_col(content_hash_out, s, 1, err);
            }
        } else {
            /* Ambiguous: discard what was collected, so a caller cannot mistake
             * the first row for the answer. */
            atlas_buf_reset(path_raw_out);
            atlas_buf_reset(content_hash_out);
        }
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_link_resolve(atlas_db *db, int64_t repo_id, atlas_decision_link *l,
                                            bool file_index_known, bool code_index_known,
                                            atlas_err *err) {
    l->currency = ATLAS_DECISION_LINK_UNKNOWN;
    l->match_count = 0;
    switch (l->kind) {
    case ATLAS_DECISION_LINK_PATH:
        return resolve_path_link(db, repo_id, l, file_index_known, err);
    case ATLAS_DECISION_LINK_SYMBOL:
        return resolve_symbol_link(db, repo_id, l, code_index_known, err);
    case ATLAS_DECISION_LINK_COMMIT:
        /* A commit Atlas has not ingested is UNKNOWN rather than MISSING.
         * History ingestion is bounded and incremental, so "not in the index"
         * is a statement about the index rather than about the repository. */
        return resolve_exists(db, "SELECT 1 FROM commits WHERE oid = ?1 AND repo_id = ?2;", repo_id,
                              l->commit_oid.data != NULL ? l->commit_oid.data : "",
                              l->commit_oid.len, l, ATLAS_DECISION_LINK_UNKNOWN, err);
    case ATLAS_DECISION_LINK_CHANGE_SET:
        return resolve_exists(db, "SELECT 1 FROM ai_change_sets WHERE id = ?1;", l->change_set_id,
                              NULL, 0, l, ATLAS_DECISION_LINK_MISSING, err);
    case ATLAS_DECISION_LINK_SUPERSEDES:
    case ATLAS_DECISION_LINK_REPLACED_BY:
        return resolve_exists(db, "SELECT 1 FROM decision_documents WHERE uid = ?1;", 0,
                              l->target_uid.data != NULL ? l->target_uid.data : "",
                              l->target_uid.len, l, ATLAS_DECISION_LINK_MISSING, err);
    }
    return ATLAS_OK;
}

/* --- supersession cycles ---------------------------------------------------- */

atlas_status atlas_db_decision_supersede_reaches(atlas_db *db, int64_t from_document_id,
                                                 int64_t to_document_id, bool *reaches_out,
                                                 atlas_err *err) {
    *reaches_out = false;
    if (from_document_id == to_document_id) {
        /* A document superseding itself is the shortest cycle there is. */
        *reaches_out = true;
        return ATLAS_OK;
    }
    /* Walk the existing chain forwards from `to`. If it arrives at `from`, then
     * making `from` point at `to` closes a loop.
     *
     * Iterative and bounded rather than a recursive CTE: the depth limit is the
     * point, and a WITH RECURSIVE that runs away on a corrupt chain is exactly
     * what a bound is for. Reaching the limit reports `true` — "Atlas cannot
     * prove this is acyclic" is answered by refusing, not by proceeding. */
    int64_t cursor = to_document_id;
    for (int depth = 0; depth < ATLAS_DECISION_MAX_SUPERSEDE_DEPTH; depth++) {
        sqlite3_stmt *s = NULL;
        atlas_status st = atlas_db_prepare(
            db, "SELECT COALESCE(superseded_by_document_id, 0) FROM decision_documents WHERE id = ?1;",
            &s, err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = bind_i64(db, s, 1, cursor, err);
        if (st != ATLAS_OK) {
            atlas_db_finish(db, s);
            return st;
        }
        int64_t next = 0;
        if (sqlite3_step(s) == SQLITE_ROW) {
            next = sqlite3_column_int64(s, 0);
        }
        atlas_db_finish(db, s);
        if (next <= 0) {
            return ATLAS_OK; /* the chain ends without reaching `from` */
        }
        if (next == from_document_id) {
            *reaches_out = true;
            return ATLAS_OK;
        }
        cursor = next;
    }
    *reaches_out = true;
    return ATLAS_OK;
}

/* --- ledger verification ----------------------------------------------------
 *
 * Replays a document's events and compares the result with the cached status
 * columns. **Reports, never repairs.** `atlas doctor` calls this, and doctor
 * observes and creates nothing — a diagnostic that fixes what it finds cannot
 * tell you whether the fault recurs. */

atlas_status atlas_db_decision_verify(atlas_db *db, int64_t document_id, bool *ok_out,
                                      atlas_buf *detail, atlas_err *err) {
    *ok_out = true;
    if (detail != NULL) {
        atlas_buf_reset(detail);
    }
    /* Replay: the last event for each revision determines that revision's
     * state, and the document is APPROVED exactly when one revision's last
     * event is APPROVED. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT e.revision_id, e.event FROM decision_events e"
        " WHERE e.document_id = ?1 AND e.revision_id IS NOT NULL"
        "   AND e.id = (SELECT MAX(e2.id) FROM decision_events e2"
        "                WHERE e2.document_id = e.document_id AND e2.revision_id = e.revision_id);",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int64_t replay_current = 0;
    int64_t approved_count = 0;
    int64_t rejected_count = 0;
    int64_t proposed_count = 0;
    int64_t superseded_count = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int64_t rev = sqlite3_column_int64(s, 0);
        const char *ev = atlas_db_col_text(s, 1);
        if (strcmp(ev, "APPROVED") == 0) {
            approved_count++;
            replay_current = rev;
        } else if (strcmp(ev, "REJECTED") == 0) {
            rejected_count++;
        } else if (strcmp(ev, "SUPERSEDED") == 0) {
            superseded_count++;
        } else {
            proposed_count++;
        }
    }
    atlas_db_finish(db, s);

    const char *replay_status = "PROPOSED";
    if (approved_count > 0) {
        replay_status = "APPROVED";
    } else if (proposed_count == 0 && rejected_count > 0) {
        replay_status = "REJECTED";
    } else if (proposed_count == 0 && superseded_count > 0) {
        replay_status = "SUPERSEDED";
    }

    sqlite3_stmt *d = NULL;
    st = atlas_db_prepare(db,
                          "SELECT current_status, COALESCE(current_revision_id, 0),"
                          "       COALESCE(superseded_by_document_id, 0)"
                          "  FROM decision_documents WHERE id = ?1;",
                          &d, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, d, 1, document_id, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, d);
        return st;
    }
    if (sqlite3_step(d) == SQLITE_ROW) {
        const char *cached_status = atlas_db_col_text(d, 0);
        int64_t cached_current = sqlite3_column_int64(d, 1);
        int64_t superseded_by = sqlite3_column_int64(d, 2);
        /* A document-level supersession legitimately overrides the replayed
         * status: the ledger says a revision is approved, and the document as a
         * whole was replaced by another document. Both are true, and the cached
         * status reports the stronger fact. */
        bool status_ok = strcmp(cached_status, replay_status) == 0 ||
                         (superseded_by > 0 && strcmp(cached_status, "SUPERSEDED") == 0);
        bool current_ok = cached_current == replay_current || superseded_by > 0;
        if (!status_ok || !current_ok) {
            *ok_out = false;
            if (detail != NULL) {
                atlas_err ignore;
                atlas_err_init(&ignore);
                (void)atlas_buf_appendf(detail, &ignore,
                                        "the cached status is %s at revision row %lld but the "
                                        "ledger replays to %s at revision row %lld",
                                        cached_status, (long long)cached_current, replay_status,
                                        (long long)replay_current);
            }
        }
    }
    atlas_db_finish(db, d);
    return st;
}

atlas_status atlas_db_decision_verify_all(atlas_db *db, int64_t *checked_out,
                                          int64_t *mismatched_out, int64_t *rehashed_out,
                                          int64_t *corrupt_out, atlas_err *err) {
    *checked_out = 0;
    *mismatched_out = 0;
    *rehashed_out = 0;
    *corrupt_out = 0;
    /* Collect the ids first, then verify each: `atlas_db_decision_verify`
     * prepares its own statements, and stepping an outer cursor while running
     * inner queries on one handle is the pattern that made the A1 reconciler
     * hold a read open across unbounded work. */
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(db, "SELECT id FROM decision_documents ORDER BY id;", &s,
                                       err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t *ids = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n == cap) {
            size_t next = cap == 0 ? 64u : cap * 2u;
            int64_t *grown = realloc(ids, next * sizeof(*ids));
            if (grown == NULL) {
                free(ids);
                atlas_db_finish(db, s);
                return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                     "out of memory while verifying decisions");
            }
            ids = grown;
            cap = next;
        }
        ids[n++] = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);

    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        bool ok = true;
        st = atlas_db_decision_verify(db, ids[i], &ok, NULL, err);
        if (st == ATLAS_OK) {
            (*checked_out)++;
            if (!ok) {
                (*mismatched_out)++;
            }
        }
    }
    free(ids);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The rehash sweep.
     *
     * Every revision is loaded whole — its alternatives, its links and their
     * snapshots, and the repository identity from its document — and hashed
     * again with the same canonical encoder that produced the stored value. Any
     * divergence means a column that is part of the approved meaning was
     * changed by something other than Atlas, because Atlas never updates one.
     *
     * Loading every revision is the expensive part of `atlas doctor`, and it is
     * worth it: without it, "a revision is immutable" is a claim about code
     * paths rather than a property of the data. */
    sqlite3_stmt *r = NULL;
    st = atlas_db_prepare(db, "SELECT id FROM decision_revisions ORDER BY id;", &r, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t *revs = NULL;
    size_t rn = 0, rcap = 0;
    while (sqlite3_step(r) == SQLITE_ROW) {
        if (rn == rcap) {
            size_t next = rcap == 0 ? 128u : rcap * 2u;
            int64_t *grown = realloc(revs, next * sizeof(*revs));
            if (grown == NULL) {
                free(revs);
                atlas_db_finish(db, r);
                return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                     "out of memory while verifying decision revisions");
            }
            revs = grown;
            rcap = next;
        }
        revs[rn++] = sqlite3_column_int64(r, 0);
    }
    atlas_db_finish(db, r);

    for (size_t i = 0; st == ATLAS_OK && i < rn; i++) {
        atlas_decision_revision rev;
        atlas_decision_revision_init(&rev);
        bool found = false;
        st = atlas_db_decision_revision_load(db, revs[i], &rev, &found, err);
        if (st == ATLAS_OK && found) {
            char rehash[ATLAS_SHA256_HEX_LEN + 1u];
            st = atlas_decision_content_hash(&rev, rehash, err);
            if (st == ATLAS_OK) {
                (*rehashed_out)++;
                if (strcmp(rehash, rev.content_hash) != 0) {
                    (*corrupt_out)++;
                }
            }
        }
        atlas_decision_revision_free(&rev);
    }
    free(revs);
    return st;
}

/* --- A2 compatibility -------------------------------------------------------
 *
 * The A2 tables are read here and written nowhere. `ai_decisions.approved` is
 * still CHECKed to 0 and no statement in Atlas binds it. */

atlas_status atlas_db_decision_legacy_list(atlas_db *db, int64_t repo_id, bool unimported_only,
                                           int64_t limit, atlas_decision_legacy_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT a.id, COALESCE(a.session_id, 0), a.created_at, a.provenance, a.state, a.title,"
        "       a.statement, a.rationale,"
        "       (SELECT COUNT(*) FROM ai_decision_paths p WHERE p.decision_id = a.id),"
        "       d.uid"
        "  FROM ai_decisions a"
        "  LEFT JOIN decision_revisions rv ON rv.imported_from_ai_decision_id = a.id"
        "  LEFT JOIN decision_documents d ON d.id = rv.document_id"
        " WHERE a.repo_id = ?1 AND (?2 = 0 OR rv.id IS NULL)"
        " ORDER BY a.id DESC LIMIT ?3;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, repo_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, unimported_only ? 1 : 0, err);
    }
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 3, limit + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    int64_t n = 0;
    while (st == ATLAS_OK && sqlite3_step(s) == SQLITE_ROW) {
        if (n >= limit) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_decision_legacy_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(s, 0);
        row.session_id = sqlite3_column_int64(s, 1);
        row.created_at = atlas_db_col_text(s, 2);
        row.provenance = atlas_db_col_text(s, 3);
        row.state = atlas_db_col_text(s, 4);
        row.title = atlas_db_col_text(s, 5);
        row.statement = atlas_db_col_text(s, 6);
        row.rationale = atlas_db_col_text_opt(s, 7);
        row.path_count = sqlite3_column_int64(s, 8);
        row.imported_uid = atlas_db_col_text_opt(s, 9);
        row.imported = row.imported_uid != NULL;
        st = cb(&row, ud, err);
        n++;
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    atlas_db_finish(db, s);
    return st;
}

atlas_status atlas_db_decision_legacy_get(atlas_db *db, int64_t repo_id, int64_t ai_decision_id,
                                          atlas_decision_revision *out, bool *found_out,
                                          atlas_err *err) {
    *found_out = false;
    sqlite3_stmt *s = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "SELECT title, statement, rationale, provenance, COALESCE(session_id, 0), created_at"
        "  FROM ai_decisions WHERE id = ?1 AND repo_id = ?2;",
        &s, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, s, 1, ai_decision_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, s, 2, repo_id, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, s);
        return st;
    }
    if (sqlite3_step(s) != SQLITE_ROW) {
        atlas_db_finish(db, s);
        return ATLAS_OK;
    }
    st = set_buf_from_col(&out->title, s, 0, err);
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->decision_text, s, 1, err);
    }
    if (st == ATLAS_OK) {
        st = set_buf_from_col(&out->rationale_text, s, 2, err);
    }
    if (st == ATLAS_OK) {
        /* The A2 provenance vocabulary maps onto the A4 actor vocabulary for
         * exactly the two values A2 could write. Anything else in that column
         * is a row A2 could not have produced, and is refused rather than
         * mapped to the nearest neighbour. */
        const char *prov = atlas_db_col_text(s, 3);
        if (strcmp(prov, "MODEL_PROPOSAL") == 0) {
            out->proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        } else if (strcmp(prov, "MODEL_INFERENCE") == 0) {
            out->proposed_by = ATLAS_DECISION_ACTOR_MODEL_INFERENCE;
        } else {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "an A2 decision proposal carries provenance \"%s\", which no A2 "
                               "adapter could write; it is reported as-is and not promoted",
                               prov);
        }
    }
    out->session_id = sqlite3_column_int64(s, 4);
    out->imported_from_ai_decision_id = ai_decision_id;
    out->scope = ATLAS_DECISION_SCOPE_UNKNOWN;
    atlas_db_finish(db, s);
    if (st != ATLAS_OK) {
        return st;
    }
    /* The paths the A2 proposal named become path links, so a promoted document
     * is about the same files the proposal was about. Their content hashes are
     * deliberately not backfilled: the snapshot would claim to have been taken
     * when the proposal was made, and it would not have been. */
    sqlite3_stmt *p = NULL;
    st = atlas_db_prepare(
        db, "SELECT path_raw, path_text FROM ai_decision_paths WHERE decision_id = ?1 LIMIT ?2;",
        &p, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = bind_i64(db, p, 1, ai_decision_id, err);
    if (st == ATLAS_OK) {
        st = bind_i64(db, p, 2, ATLAS_DECISION_MAX_LINKS, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, p);
        return st;
    }
    while (st == ATLAS_OK && sqlite3_step(p) == SQLITE_ROW) {
        if (out->link_count >= ATLAS_DECISION_MAX_LINKS) {
            break;
        }
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        st = set_buf_from_col(&l.path_raw, p, 0, err);
        if (st == ATLAS_OK) {
            st = set_buf_from_col(&l.path_text, p, 1, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_decision_revision_add_link(out, &l, err);
        }
        atlas_decision_link_free(&l);
    }
    atlas_db_finish(db, p);
    if (st == ATLAS_OK) {
        *found_out = true;
    }
    return st;
}

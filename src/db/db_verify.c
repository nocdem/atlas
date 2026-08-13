/* Atlas - A9.2: typed operations over the migration-14 verification tables.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The single write point for claims, actors, evidence, attestations, results,
 * outcomes, reliability and the machine lifecycle audit. That is the rule
 * `settle()`, `atlas_db_evidence_insert`, `atlas_decision_apply_in_tx` and
 * `atlas_orch_apply_in_tx` all follow, and it earns its keep here for a
 * specific reason: the forgery guards are cheap to state once and impossible to
 * keep in step if two paths reach the tables.
 *
 * Two refusals in this file are security boundaries rather than validation:
 *
 *   - an actor of a class that requires Atlas attestation cannot be written
 *     with any other identity, so "a model claims to be clang" fails at the
 *     insert. The schema CHECK says the same thing independently; the C
 *     refusal exists because it produces the message a caller can act on, and
 *     the CHECK exists because it is the guarantee;
 *   - a warrant is checked against the document, the revision, the target state
 *     *and* the content hash, and consumed by a conditional UPDATE that names
 *     the state it observed. A replayed warrant loses deterministically, which
 *     is A4's rule about every lifecycle UPDATE.
 *
 * Every statement here passes a string literal to `atlas_db_prepare`, never a
 * formatted buffer: the cache keys on the SQL pointer and confirms the text,
 * and a caller that formats into a reused buffer is the one thing that cache is
 * documented to punish.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/hmac.h"
#include "atlas/verify.h"
#include "db/db_internal.h"

/* --- uids -----------------------------------------------------------------
 *
 * 128 bits of kernel randomness, hex, behind a fixed prefix. Fails rather than
 * falling back when the CSPRNG is unavailable, for the reason
 * `atlas_decision_uid_derive` does: a uid built from a predictable input is one
 * that collides, and a silently weakened identifier is not noticed until two
 * records merge.
 *
 * An **identifier, not a secret**. Nothing treats knowing one as authorisation. */
#define VERIFY_UID_BYTES 16u

static atlas_status verify_uid(const char *prefix, atlas_buf *out, atlas_err *err) {
    unsigned char raw[VERIFY_UID_BYTES];
    atlas_status st = atlas_random_bytes(raw, sizeof raw, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char hex[VERIFY_UID_BYTES * 2u + 1u];
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

/* Borrowed C string for a buf that may be empty. */
static const char *bs(const atlas_buf *b) {
    return b != NULL && b->len > 0 ? atlas_buf_cstr(b) : "";
}

static atlas_status take_text(atlas_buf *dst, const char *src, atlas_err *err) {
    return atlas_buf_set_str(dst, src != NULL ? src : "", err);
}

/* --- actors ---------------------------------------------------------------- */

atlas_status atlas_db_verify_actor_upsert(atlas_db *db, atlas_verify_actor *a, const char *now,
                                          atlas_err *err) {
    if (db == NULL || a == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no actor to record");
    }

    /* The forgery guard. A TOOL, TEST, RUNTIME_OBSERVATION or ATLAS_VERIFIER
     * actor is worth what it is worth *because Atlas performed the act*, so an
     * identity that does not say Atlas attested it describes something else.
     *
     * Refused rather than accepted-and-discounted, deliberately. A discounted
     * forgery still appears in an evidence list, still reads as tool output to
     * somebody skimming a UI, and still has to be argued away by whoever finds
     * it. */
    if (atlas_verify_actor_class_requires_atlas_identity(a->cls) &&
        a->identity != ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "an actor of class %s records something Atlas did, so it may only be "
                             "created by Atlas itself; a submitted identity cannot claim it",
                             atlas_verify_actor_class_name(a->cls));
    }

    if (a->uid.len == 0) {
        atlas_status ust = verify_uid("atlas-actor-", &a->uid, err);
        if (ust != ATLAS_OK) {
            return ust;
        }
    }

    static const char SQL[] =
        "INSERT INTO verify_actors(uid, class, identity, name, provider, family, version, role,"
        "  session_key, run_id, parent_actor_id, first_seen_at, last_seen_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?12)"
        " ON CONFLICT(uid) DO UPDATE SET last_seen_at = excluded.last_seen_at"
        " RETURNING id;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, bs(&a->uid), err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, atlas_verify_actor_class_name(a->cls), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_verify_actor_identity_name(a->identity),
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, bs(&a->name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, bs(&a->provider), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, bs(&a->family), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, bs(&a->version), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, bs(&a->role), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, bs(&a->session_key), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, bs(&a->run_id), err);
    }
    if (st == ATLAS_OK && (sqlite3_bind_int64(stmt, 11, a->parent_actor_id) != SQLITE_OK ||
                           sqlite3_bind_text(stmt, 12, now, -1, SQLITE_TRANSIENT) != SQLITE_OK)) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the actor");
    }
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            a->id = sqlite3_column_int64(stmt, 0);
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record the actor");
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_actor_get(atlas_db *db, int64_t id, atlas_verify_actor *out,
                                       bool *found, atlas_err *err) {
    if (found != NULL) {
        *found = false;
    }
    static const char SQL[] =
        "SELECT id, uid, class, identity, name, provider, family, version, role, session_key,"
        "  run_id, parent_actor_id, first_seen_at, last_seen_at"
        " FROM verify_actors WHERE id = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the actor id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->id = sqlite3_column_int64(stmt, 0);
        st = take_text(&out->uid, (const char *)sqlite3_column_text(stmt, 1), err);
        if (st == ATLAS_OK) {
            (void)atlas_verify_actor_class_parse((const char *)sqlite3_column_text(stmt, 2),
                                                 &out->cls);
            (void)atlas_verify_actor_identity_parse((const char *)sqlite3_column_text(stmt, 3),
                                                    &out->identity);
            st = take_text(&out->name, (const char *)sqlite3_column_text(stmt, 4), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->provider, (const char *)sqlite3_column_text(stmt, 5), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->family, (const char *)sqlite3_column_text(stmt, 6), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->version, (const char *)sqlite3_column_text(stmt, 7), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->role, (const char *)sqlite3_column_text(stmt, 8), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->session_key, (const char *)sqlite3_column_text(stmt, 9), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->run_id, (const char *)sqlite3_column_text(stmt, 10), err);
        }
        if (st == ATLAS_OK) {
            out->parent_actor_id = sqlite3_column_int64(stmt, 11);
            st = take_text(&out->first_seen_at, (const char *)sqlite3_column_text(stmt, 12), err);
        }
        if (st == ATLAS_OK) {
            st = take_text(&out->last_seen_at, (const char *)sqlite3_column_text(stmt, 13), err);
        }
        if (st == ATLAS_OK && found != NULL) {
            *found = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- claims ---------------------------------------------------------------- */

atlas_status atlas_db_verify_claim_insert(atlas_db *db, atlas_verify_claim *c, const char *now,
                                          atlas_err *err) {
    if (db == NULL || c == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no claim to record");
    }
    if (c->text.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a claim needs a proposition to state");
    }
    if (c->text.len > ATLAS_VERIFY_CLAIM_TEXT_MAX) {
        /* Refused, never truncated: a shortened proposition is a different
         * proposition, and one whose scope has quietly widened. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a claim is one discrete proposition and this one is longer than %u "
                             "bytes; split it rather than shortening it",
                             (unsigned)ATLAS_VERIFY_CLAIM_TEXT_MAX);
    }
    if (c->uid.len == 0) {
        atlas_status ust = verify_uid("atlas-claim-", &c->uid, err);
        if (ust != ATLAS_OK) {
            return ust;
        }
    }
    if (c->created_at.len == 0) {
        atlas_status tst = take_text(&c->created_at, now, err);
        if (tst != ATLAS_OK) {
            return tst;
        }
    }

    static const char SQL[] =
        "INSERT INTO verify_claims(uid, repo_id, repo_identity_hash, document_id, revision_id,"
        "  domain, text, scope_note, semantics, verifier, verifier_input, basis_commit,"
        "  environment, created_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) RETURNING id;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, bs(&c->uid), err);
    if (st == ATLAS_OK && (sqlite3_bind_int64(stmt, 2, c->repo_id) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 4, c->document_id) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 5, c->revision_id) != SQLITE_OK)) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the claim");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, bs(&c->repo_identity_hash), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, bs(&c->domain), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, bs(&c->text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, bs(&c->scope_note), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9,
                                    atlas_verify_claim_semantics_name(c->semantics), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, bs(&c->verifier), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 11, bs(&c->verifier_input), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 12, bs(&c->basis_commit), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 13, bs(&c->environment), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 14, bs(&c->created_at), err);
    }
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            c->id = sqlite3_column_int64(stmt, 0);
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record the claim");
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

static atlas_status claim_from_row(sqlite3_stmt *stmt, atlas_verify_claim *out, atlas_err *err) {
    out->id = sqlite3_column_int64(stmt, 0);
    atlas_status st = take_text(&out->uid, (const char *)sqlite3_column_text(stmt, 1), err);
    if (st == ATLAS_OK) {
        out->repo_id = sqlite3_column_int64(stmt, 2);
        st = take_text(&out->repo_identity_hash, (const char *)sqlite3_column_text(stmt, 3), err);
    }
    if (st == ATLAS_OK) {
        out->document_id = sqlite3_column_int64(stmt, 4);
        out->revision_id = sqlite3_column_int64(stmt, 5);
        st = take_text(&out->domain, (const char *)sqlite3_column_text(stmt, 6), err);
    }
    if (st == ATLAS_OK) {
        st = take_text(&out->text, (const char *)sqlite3_column_text(stmt, 7), err);
    }
    if (st == ATLAS_OK) {
        st = take_text(&out->scope_note, (const char *)sqlite3_column_text(stmt, 8), err);
    }
    if (st == ATLAS_OK) {
        (void)atlas_verify_claim_semantics_parse((const char *)sqlite3_column_text(stmt, 9),
                                                 &out->semantics);
        st = take_text(&out->verifier, (const char *)sqlite3_column_text(stmt, 10), err);
    }
    if (st == ATLAS_OK) {
        st = take_text(&out->verifier_input, (const char *)sqlite3_column_text(stmt, 11), err);
    }
    if (st == ATLAS_OK) {
        st = take_text(&out->basis_commit, (const char *)sqlite3_column_text(stmt, 12), err);
    }
    if (st == ATLAS_OK) {
        st = take_text(&out->environment, (const char *)sqlite3_column_text(stmt, 13), err);
    }
    if (st == ATLAS_OK) {
        st = take_text(&out->created_at, (const char *)sqlite3_column_text(stmt, 14), err);
    }
    if (st == ATLAS_OK) {
        out->superseded_by_claim_id = sqlite3_column_int64(stmt, 15);
    }
    return st;
}

#define CLAIM_COLUMNS                                                                              \
    "id, uid, repo_id, repo_identity_hash, document_id, revision_id, domain, text, scope_note,"    \
    " semantics, verifier, verifier_input, basis_commit, environment, created_at,"                 \
    " superseded_by_claim_id"

atlas_status atlas_db_verify_claim_get(atlas_db *db, int64_t id, atlas_verify_claim *out,
                                       bool *found, atlas_err *err) {
    if (found != NULL) {
        *found = false;
    }
    static const char SQL[] = "SELECT " CLAIM_COLUMNS " FROM verify_claims WHERE id = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the claim id");
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        st = claim_from_row(stmt, out, err);
        if (st == ATLAS_OK && found != NULL) {
            *found = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_claim_find(atlas_db *db, const char *uid, atlas_verify_claim *out,
                                        bool *found, atlas_err *err) {
    if (found != NULL) {
        *found = false;
    }
    static const char SQL[] = "SELECT " CLAIM_COLUMNS " FROM verify_claims WHERE uid = ?1;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, uid, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        st = claim_from_row(stmt, out, err);
        if (st == ATLAS_OK && found != NULL) {
            *found = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_claims_for_revision(atlas_db *db, int64_t document_id,
                                                 int64_t revision_id, atlas_verify_claim_cb cb,
                                                 void *ctx, bool *truncated_out, atlas_err *err) {
    if (truncated_out != NULL) {
        *truncated_out = false;
    }
    /* A revision_id of 0 means "every claim about this document", which is what
     * a summary over a record wants; a specific revision is what a verification
     * run wants, because §64 requires an old revision's evidence not to be
     * mistaken for the current one's. */
    static const char SQL[] =
        "SELECT " CLAIM_COLUMNS
        " FROM verify_claims"
        " WHERE document_id = ?1 AND (?2 = 0 OR revision_id = ?2)"
        "   AND superseded_by_claim_id = 0"
        " ORDER BY id DESC LIMIT ?3;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* One more than the ceiling, so reaching it is *detected* rather than
     * inferred from a full page. A bound that cannot report itself is the one
     * thing this phase must not have. */
    if (sqlite3_bind_int64(stmt, 1, document_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, revision_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, (int64_t)ATLAS_VERIFY_MAX_CLAIMS + 1) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the claim query");
    }

    size_t seen = 0;
    int rc;
    while (st == ATLAS_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (seen >= ATLAS_VERIFY_MAX_CLAIMS) {
            if (truncated_out != NULL) {
                *truncated_out = true;
            }
            break;
        }
        atlas_verify_claim c;
        atlas_verify_claim_init(&c);
        st = claim_from_row(stmt, &c, err);
        if (st == ATLAS_OK && cb != NULL) {
            st = cb(&c, ctx, err);
        }
        atlas_verify_claim_free(&c);
        seen++;
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- evidence -------------------------------------------------------------- */

atlas_status atlas_db_verify_evidence_insert(atlas_db *db, atlas_verify_evidence *e,
                                             const char *now, atlas_err *err) {
    if (db == NULL || e == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no evidence to record");
    }
    if (e->cls == ATLAS_EVIDENCE_UNKNOWN) {
        /* There is no `TEXT` evidence class and no unclassified evidence.
         * Opaque provenance-free prose is exactly what this phase exists to
         * stop being counted. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "evidence must say what sort of thing it is; there is no "
                             "unclassified evidence");
    }
    if (e->uid.len == 0) {
        atlas_status ust = verify_uid("atlas-ev-", &e->uid, err);
        if (ust != ATLAS_OK) {
            return ust;
        }
    }

    static const char SQL[] =
        "INSERT INTO verify_evidence(uid, class, repo_id, commit_oid, path_raw, path_text, symbol,"
        "  line_start, line_end, content_hash, suite, test_name, result, binary_id, environment,"
        "  tool, tool_version, proof_class, target, probe, observed, deployed_revision,"
        "  observed_at, recorded_at, actor_id)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,"
        "  ?23,?24,?25) RETURNING id;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, bs(&e->uid), err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, atlas_verify_evidence_class_name(e->cls), err);
    }
    if (st == ATLAS_OK && sqlite3_bind_int64(stmt, 3, e->repo_id) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the evidence repository");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, bs(&e->commit_oid), err);
    }
    if (st == ATLAS_OK) {
        /* Paths are bytes. The raw form is the key and the safe text form is
         * for display, exactly as everywhere else in Atlas. */
        if (e->path_raw.len > 0) {
            if (sqlite3_bind_blob(stmt, 5, e->path_raw.data, (int)e->path_raw.len,
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the evidence path");
            }
        } else if (sqlite3_bind_null(stmt, 5) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the evidence path");
        }
    }
    static const struct {
        int idx;
        size_t off;
    } TEXTS[] = {
        {6, offsetof(atlas_verify_evidence, path_text)},
        {7, offsetof(atlas_verify_evidence, symbol)},
        {10, offsetof(atlas_verify_evidence, content_hash)},
        {11, offsetof(atlas_verify_evidence, suite)},
        {12, offsetof(atlas_verify_evidence, test_name)},
        {13, offsetof(atlas_verify_evidence, result)},
        {14, offsetof(atlas_verify_evidence, binary_id)},
        {15, offsetof(atlas_verify_evidence, environment)},
        {16, offsetof(atlas_verify_evidence, tool)},
        {17, offsetof(atlas_verify_evidence, tool_version)},
        {18, offsetof(atlas_verify_evidence, proof_class)},
        {19, offsetof(atlas_verify_evidence, target)},
        {20, offsetof(atlas_verify_evidence, probe)},
        {21, offsetof(atlas_verify_evidence, observed)},
        {22, offsetof(atlas_verify_evidence, deployed_revision)},
        {23, offsetof(atlas_verify_evidence, observed_at)},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof TEXTS / sizeof TEXTS[0]; i++) {
        const atlas_buf *b = (const atlas_buf *)((const char *)e + TEXTS[i].off);
        st = atlas_db_bind_text_opt(db, stmt, TEXTS[i].idx, bs(b), err);
    }
    if (st == ATLAS_OK && (sqlite3_bind_int64(stmt, 8, e->line_start) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 9, e->line_end) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 25, e->actor_id) != SQLITE_OK)) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the evidence");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 24,
                                    e->recorded_at.len > 0 ? bs(&e->recorded_at) : now, err);
    }
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            e->id = sqlite3_column_int64(stmt, 0);
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record the evidence");
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_evidence_dep_add(atlas_db *db, int64_t evidence_id,
                                              int64_t derives_from_id, const char *now,
                                              atlas_err *err) {
    if (evidence_id == derives_from_id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "evidence cannot derive from itself; a self-edge would make a piece "
                             "of evidence its own corroboration");
    }
    static const char SQL[] =
        "INSERT INTO verify_evidence_deps(evidence_id, derives_from_id, recorded_at)"
        " VALUES(?1,?2,?3) ON CONFLICT(evidence_id, derives_from_id) DO NOTHING;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, evidence_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, derives_from_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the derivation edge");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- attestations ---------------------------------------------------------- */

atlas_status atlas_db_verify_attestation_insert(atlas_db *db, atlas_verify_attestation *a,
                                                const int64_t *evidence_ids, size_t evidence_count,
                                                const char *now, atlas_err *err) {
    if (db == NULL || a == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no attestation to record");
    }
    if (a->self_confidence < -1 || a->self_confidence > 100) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                            "self-reported confidence is 0..100, or -1 for none");
    }
    if (a->uid.len == 0) {
        atlas_status ust = verify_uid("atlas-att-", &a->uid, err);
        if (ust != ATLAS_OK) {
            return ust;
        }
    }

    static const char SQL[] =
        "INSERT INTO verify_attestations(uid, claim_id, actor_id, verdict, self_confidence,"
        "  method, scope_note, created_at, supersedes_id, proposer, basis_commit, environment)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) RETURNING id;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_bind_text_opt(db, stmt, 1, bs(&a->uid), err);
    if (st == ATLAS_OK && (sqlite3_bind_int64(stmt, 2, a->claim_id) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 3, a->actor_id) != SQLITE_OK ||
                           sqlite3_bind_int(stmt, 5, a->self_confidence) != SQLITE_OK ||
                           sqlite3_bind_int64(stmt, 9, a->supersedes_id) != SQLITE_OK ||
                           sqlite3_bind_int(stmt, 10, a->proposer ? 1 : 0) != SQLITE_OK)) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the attestation");
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, atlas_verify_verdict_name(a->verdict), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, bs(&a->method), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, bs(&a->scope_note), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, a->created_at.len > 0 ? bs(&a->created_at) : now,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 11, bs(&a->basis_commit), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 12, bs(&a->environment), err);
    }
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            a->id = sqlite3_column_int64(stmt, 0);
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record the attestation");
        }
    }
    atlas_db_finish(db, stmt);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The evidence links, in the same call because they are one statement. An
     * attestation whose links failed to land would be indistinguishable from an
     * undeclared interpretation for ever — and would therefore be *grouped*
     * with every other undeclared one, silently changing what it is worth. */
    for (size_t i = 0; i < evidence_count && st == ATLAS_OK; i++) {
        static const char LSQL[] =
            "INSERT INTO verify_attestation_evidence(attestation_id, evidence_id)"
            " VALUES(?1,?2) ON CONFLICT DO NOTHING;";
        sqlite3_stmt *ls = NULL;
        st = atlas_db_prepare(db, LSQL, &ls, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (sqlite3_bind_int64(ls, 1, a->id) != SQLITE_OK ||
            sqlite3_bind_int64(ls, 2, evidence_ids[i]) != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the attestation evidence");
        } else {
            st = atlas_db_step_done(db, ls, err);
        }
        atlas_db_finish(db, ls);
    }
    return st;
}

/* --- aggregation inputs ---------------------------------------------------- */

void atlas_verify_inputs_free(atlas_verify_inputs *in) {
    if (in == NULL) {
        return;
    }
    free(in->items);
    free(in->dep_from);
    free(in->dep_to);
    memset(in, 0, sizeof *in);
}

/* Loads one claim's attestations, reduces each to the counted facts the
 * algorithm needs, and derives the correlation edges between them.
 *
 * The edges are the interesting part. Two attestations are joined when they
 * share a piece of evidence, or when one's evidence derives from the other's.
 * That is the mechanical form of "actor is not evidence": what makes two
 * sources correlated is the material they read, not who they are.
 *
 * Everything else about grouping happens in `atlas_verify_independent_groups`,
 * which additionally folds all undeclared interpretation into one shared set. */
atlas_status atlas_db_verify_inputs_load(atlas_db *db, int64_t claim_id, const char *stale_before,
                                         atlas_verify_inputs *out, atlas_err *err) {
    if (out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "nowhere to put the inputs");
    }
    memset(out, 0, sizeof *out);

    /* The true total first, so a bound that is reached can be reported rather
     * than inferred. */
    {
        static const char CSQL[] = "SELECT COUNT(*) FROM verify_attestations WHERE claim_id = ?1;";
        sqlite3_stmt *cs = NULL;
        atlas_status cst = atlas_db_prepare(db, CSQL, &cs, err);
        if (cst != ATLAS_OK) {
            return cst;
        }
        if (sqlite3_bind_int64(cs, 1, claim_id) != SQLITE_OK) {
            atlas_db_finish(db, cs);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the claim id");
        }
        if (sqlite3_step(cs) == SQLITE_ROW) {
            out->total = (size_t)sqlite3_column_int64(cs, 0);
        }
        atlas_db_finish(db, cs);
    }
    if (out->total == 0) {
        return ATLAS_OK;
    }

    size_t cap = out->total;
    if (cap > ATLAS_VERIFY_MAX_ATTESTATIONS) {
        cap = ATLAS_VERIFY_MAX_ATTESTATIONS;
        out->limit_reached = true;
    }
    out->items = calloc(cap, sizeof *out->items);
    if (out->items == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory loading attestations");
    }

    /* A superseded attestation is history and does not vote. It stays readable
     * — an actor reversing itself is a fact reliability must be able to see —
     * but the current aggregate uses the current statement. */
    static const char SQL[] =
        "SELECT a.id, a.actor_id, a.verdict, a.proposer, a.scope_note,"
        "       act.class, act.identity,"
        "       (SELECT COUNT(*) FROM verify_attestation_evidence ae WHERE ae.attestation_id = a.id),"
        "       (SELECT MIN(CASE WHEN COALESCE(NULLIF(e.observed_at,''), e.recorded_at) < ?2"
        "                        THEN 1 ELSE 0 END)"
        "          FROM verify_attestation_evidence ae JOIN verify_evidence e"
        "            ON e.id = ae.evidence_id WHERE ae.attestation_id = a.id),"
        "       (SELECT MIN(CASE e.class"
        "                     WHEN 'ATLAS_KNOWLEDGE' THEN 3 WHEN 'HUMAN_STATEMENT' THEN 3"
        "                     WHEN 'AI_ANALYSIS' THEN 3"
        "                     WHEN 'TEST' THEN 2 WHEN 'RUNTIME' THEN 2"
        "                     WHEN 'DEPLOYED_CONFIG' THEN 2"
        "                     WHEN 'UNKNOWN' THEN 0 ELSE 1 END)"
        "          FROM verify_attestation_evidence ae JOIN verify_evidence e"
        "            ON e.id = ae.evidence_id WHERE ae.attestation_id = a.id),"
        "       c.scope_note"
        "  FROM verify_attestations a"
        "  JOIN verify_actors act ON act.id = a.actor_id"
        "  JOIN verify_claims c ON c.id = a.claim_id"
        " WHERE a.claim_id = ?1"
        "   AND a.id NOT IN (SELECT supersedes_id FROM verify_attestations"
        "                     WHERE claim_id = ?1 AND supersedes_id > 0)"
        " ORDER BY a.id LIMIT ?3;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        atlas_verify_inputs_free(out);
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, claim_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, (int64_t)cap) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        atlas_verify_inputs_free(out);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the attestation query");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, stale_before != NULL ? stale_before : "", err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        atlas_verify_inputs_free(out);
        return st;
    }

    size_t n = 0;
    while (n < cap && sqlite3_step(stmt) == SQLITE_ROW) {
        atlas_verify_input *in = &out->items[n];
        in->attestation_id = sqlite3_column_int64(stmt, 0);
        in->actor_id = sqlite3_column_int64(stmt, 1);
        (void)atlas_verify_verdict_parse((const char *)sqlite3_column_text(stmt, 2), &in->verdict);
        in->proposer = sqlite3_column_int(stmt, 3) != 0;
        (void)atlas_verify_actor_class_parse((const char *)sqlite3_column_text(stmt, 5),
                                             &in->actor_class);
        (void)atlas_verify_actor_identity_parse((const char *)sqlite3_column_text(stmt, 6),
                                                &in->actor_identity);
        int64_t ev_count = sqlite3_column_int64(stmt, 7);
        /* MIN over a "is it stale" flag is 1 only when *every* piece of
         * evidence is stale. Conservative in the safe direction: one current
         * observation keeps an attestation current, and an attestation with no
         * evidence at all is not called stale — it is called undeclared, which
         * the grouping already punishes. */
        in->stale = ev_count > 0 && sqlite3_column_type(stmt, 8) != SQLITE_NULL &&
                    sqlite3_column_int(stmt, 8) == 1;
        /* The weakest family present. An attestation resting on a document and
         * a runtime probe is only as independent as its most correlated leg. */
        if (ev_count == 0 || sqlite3_column_type(stmt, 9) == SQLITE_NULL) {
            in->family = ATLAS_EVIDENCE_FAMILY_UNKNOWN;
        } else {
            int fam = sqlite3_column_int(stmt, 9);
            in->family = fam == 3   ? ATLAS_EVIDENCE_FAMILY_INTERPRETATION
                         : fam == 2 ? ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION
                         : fam == 1 ? ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT
                                    : ATLAS_EVIDENCE_FAMILY_UNKNOWN;
        }
        /* Scope match: an actor that recorded no scope is taken to have
         * examined the claim's, because saying nothing is not a mismatch — but
         * a stated scope that differs is. */
        const char *att_scope = (const char *)sqlite3_column_text(stmt, 4);
        const char *claim_scope = (const char *)sqlite3_column_text(stmt, 10);
        if (att_scope == NULL || att_scope[0] == '\0' || claim_scope == NULL ||
            claim_scope[0] == '\0') {
            in->scope_match = true;
        } else {
            in->scope_match = strcmp(att_scope, claim_scope) == 0;
        }
        in->reliability = -1; /* filled below from measured history, or left */
        in->group = -1;
        n++;
    }
    atlas_db_finish(db, stmt);
    out->count = n;
    if (st != ATLAS_OK) {
        atlas_verify_inputs_free(out);
        return st;
    }

    /* Measured reliability per actor, where any exists. Absent leaves -1 and
     * the algorithm falls back to the documented prior. */
    for (size_t i = 0; i < out->count && st == ATLAS_OK; i++) {
        int rel = -1, samples = 0;
        atlas_verify_calibration cal = ATLAS_CALIBRATION_INSUFFICIENT_DATA;
        st = atlas_db_verify_reliability_get(db, out->items[i].actor_id, "", &rel, &samples, &cal,
                                             err);
        if (st == ATLAS_OK) {
            out->items[i].reliability = rel;
        }
    }
    if (st != ATLAS_OK) {
        atlas_verify_inputs_free(out);
        return st;
    }

    /* Correlation edges: attestation pairs that share evidence, or whose
     * evidence is joined by a declared derivation edge. Indices into `items`,
     * so the algorithm never sees a row id. */
    size_t dep_cap = ATLAS_VERIFY_MAX_DEP_EDGES;
    out->dep_from = calloc(dep_cap, sizeof *out->dep_from);
    out->dep_to = calloc(dep_cap, sizeof *out->dep_to);
    if (out->dep_from == NULL || out->dep_to == NULL) {
        atlas_verify_inputs_free(out);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory loading evidence edges");
    }

    static const char DSQL[] =
        /* Shared evidence. */
        "SELECT DISTINCT x.attestation_id, y.attestation_id"
        "  FROM verify_attestation_evidence x"
        "  JOIN verify_attestation_evidence y ON y.evidence_id = x.evidence_id"
        "  JOIN verify_attestations ax ON ax.id = x.attestation_id AND ax.claim_id = ?1"
        "  JOIN verify_attestations ay ON ay.id = y.attestation_id AND ay.claim_id = ?1"
        " WHERE x.attestation_id < y.attestation_id"
        " UNION "
        /* Evidence joined by a declared derivation edge, in either direction:
         * a derivation makes both ends one root, and which way the arrow points
         * does not change that. */
        "SELECT DISTINCT x.attestation_id, y.attestation_id"
        "  FROM verify_evidence_deps d"
        "  JOIN verify_attestation_evidence x ON x.evidence_id = d.evidence_id"
        "  JOIN verify_attestation_evidence y ON y.evidence_id = d.derives_from_id"
        "  JOIN verify_attestations ax ON ax.id = x.attestation_id AND ax.claim_id = ?1"
        "  JOIN verify_attestations ay ON ay.id = y.attestation_id AND ay.claim_id = ?1"
        " WHERE x.attestation_id <> y.attestation_id"
        " LIMIT ?2;";
    sqlite3_stmt *ds = NULL;
    st = atlas_db_prepare(db, DSQL, &ds, err);
    if (st != ATLAS_OK) {
        atlas_verify_inputs_free(out);
        return st;
    }
    if (sqlite3_bind_int64(ds, 1, claim_id) != SQLITE_OK ||
        sqlite3_bind_int64(ds, 2, (int64_t)dep_cap) != SQLITE_OK) {
        atlas_db_finish(db, ds);
        atlas_verify_inputs_free(out);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the evidence edge query");
    }
    while (out->dep_count < dep_cap && sqlite3_step(ds) == SQLITE_ROW) {
        int64_t a_id = sqlite3_column_int64(ds, 0);
        int64_t b_id = sqlite3_column_int64(ds, 1);
        int64_t ai = -1, bi = -1;
        for (size_t i = 0; i < out->count; i++) {
            if (out->items[i].attestation_id == a_id) {
                ai = (int64_t)i;
            }
            if (out->items[i].attestation_id == b_id) {
                bi = (int64_t)i;
            }
        }
        if (ai >= 0 && bi >= 0) {
            out->dep_from[out->dep_count] = ai;
            out->dep_to[out->dep_count] = bi;
            out->dep_count++;
        }
    }
    atlas_db_finish(db, ds);
    return ATLAS_OK;
}

/* --- results --------------------------------------------------------------- */

/* The reason list, as a comma-separated string of names. Text rather than a
 * child table because it is an explanation of one row, read with that row and
 * never joined against — and every byte of it comes from a fixed Atlas-owned
 * vocabulary, so it carries nothing anybody else chose. */
static atlas_status reasons_text(const atlas_verify_aggregate *agg, atlas_buf *out,
                                 atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; i < agg->reason_count && st == ATLAS_OK; i++) {
        if (i > 0) {
            st = atlas_buf_append_ch(out, ',', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(out, atlas_verify_reason_name(agg->reasons[i]), err);
        }
    }
    return st;
}

atlas_status atlas_db_verify_result_insert(atlas_db *db, int64_t claim_id,
                                           const atlas_verify_aggregate *agg, const char *verifier,
                                           atlas_verify_check check, const char *now,
                                           int64_t *id_out, atlas_err *err) {
    if (agg == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no aggregate to record");
    }
    /* A result must say how it was reached; one that does not is not a result.
     * The same refusal the header states, at the write point rather than
     * remembered by callers. */
    if (!atlas_verify_basis_writable(agg->basis)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a verification result must name the mechanism that produced it");
    }

    atlas_buf reasons = ATLAS_BUF_INIT;
    atlas_status st = reasons_text(agg, &reasons, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&reasons);
        return st;
    }

    static const char SQL[] =
        "INSERT INTO verify_results(claim_id, state, basis, confidence_score, calibration,"
        "  calibrated_probability, algorithm, family_version, support_count, contradict_count,"
        "  inconclusive_count, independent_groups, independent_families, support_mass,"
        "  contradict_mass, conflict, stale, verifier, check_result, reasons, reason_total,"
        "  created_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22)"
        " RETURNING id;";
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&reasons);
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, claim_id) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 4, agg->confidence) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 8, agg->family_version) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 9, agg->support_count) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 10, agg->contradict_count) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 11, agg->inconclusive_count) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 12, agg->independent_groups) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 13, agg->independent_families) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 14, agg->support_mass) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 15, agg->contradict_mass) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 17, agg->stale ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 21, (int64_t)agg->reason_total) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the verification result");
    }
    /* NULL unless calibration actually supports a probability. The schema
     * CHECK enforces the same pairing independently, so the separation between
     * a score and a probability is a database constraint rather than a
     * convention a renderer remembers. */
    if (st == ATLAS_OK) {
        int rc = (agg->calibration == ATLAS_CALIBRATION_CALIBRATED &&
                  agg->calibrated_probability >= 0)
                     ? sqlite3_bind_int(stmt, 6, agg->calibrated_probability)
                     : sqlite3_bind_null(stmt, 6);
        if (rc != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the calibrated probability");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 2, atlas_verify_state_name(agg->state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, atlas_verify_basis_name(agg->basis), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, atlas_verify_calibration_name(agg->calibration),
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, agg->algorithm, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 16, atlas_verify_conflict_name(agg->conflict), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 18, verifier != NULL ? verifier : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 19, atlas_verify_check_name(check), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 20, bs(&reasons), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 22, now, err);
    }
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (id_out != NULL) {
                *id_out = sqlite3_column_int64(stmt, 0);
            }
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record the verification result");
        }
    }
    atlas_db_finish(db, stmt);
    atlas_buf_free(&reasons);
    return st;
}

/* --- the audit row, which is also the warrant ------------------------------ */

atlas_status atlas_db_verify_audit_insert(atlas_db *db, const atlas_verify_audit *a,
                                          const char *now, int64_t *id_out, atlas_err *err) {
    if (a == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no audit record to write");
    }
    static const char SQL[] =
        "INSERT INTO verify_lifecycle_audit(claim_id, result_id, document_id, revision_id,"
        "  content_hash, kind, from_status, to_status, basis, verdict, reasons, policy_id,"
        "  policy_hash, algorithm, prior_version, family_version, confidence_score, calibration,"
        "  calibrated_probability, independent_groups, evidence_snapshot, verifier, check_result,"
        "  binary_id, consumed, created_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,"
        "  ?23,?24,0,?25) RETURNING id;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, a->claim_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, a->result_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, a->document_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, a->revision_id) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 15, a->prior_version) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 16, a->family_version) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 17, a->confidence) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 20, a->independent_groups) != SQLITE_OK) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the lifecycle audit row");
    }
    if (st == ATLAS_OK) {
        int rc = (a->calibration == ATLAS_CALIBRATION_CALIBRATED && a->calibrated_probability >= 0)
                     ? sqlite3_bind_int(stmt, 19, a->calibrated_probability)
                     : sqlite3_bind_null(stmt, 19);
        if (rc != SQLITE_OK) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the calibrated probability");
        }
    }
    static const struct {
        int idx;
        size_t off;
    } TEXTS[] = {
        {5, offsetof(atlas_verify_audit, content_hash)},
        {6, offsetof(atlas_verify_audit, kind)},
        {7, offsetof(atlas_verify_audit, from_status)},
        {8, offsetof(atlas_verify_audit, to_status)},
        {11, offsetof(atlas_verify_audit, reasons)},
        {12, offsetof(atlas_verify_audit, policy_id)},
        {13, offsetof(atlas_verify_audit, policy_hash)},
        {14, offsetof(atlas_verify_audit, algorithm)},
        {21, offsetof(atlas_verify_audit, evidence_snapshot)},
        {22, offsetof(atlas_verify_audit, verifier)},
        {24, offsetof(atlas_verify_audit, binary_id)},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof TEXTS / sizeof TEXTS[0]; i++) {
        const char *const *p = (const char *const *)((const char *)a + TEXTS[i].off);
        st = atlas_db_bind_text_opt(db, stmt, TEXTS[i].idx, *p != NULL ? *p : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 9, atlas_verify_basis_name(a->basis), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 10, atlas_verify_policy_verdict_name(a->verdict),
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 18, atlas_verify_calibration_name(a->calibration),
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 23, atlas_verify_check_name(a->check_result), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 25, now, err);
    }
    if (st == ATLAS_OK) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (id_out != NULL) {
                *id_out = sqlite3_column_int64(stmt, 0);
            }
        } else {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot record the lifecycle audit row");
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_warrant_check(atlas_db *db, int64_t warrant_id, int64_t document_id,
                                           int64_t revision_id, const char *to_status,
                                           const char *content_hash, bool *ok_out,
                                           atlas_err *err) {
    if (ok_out != NULL) {
        *ok_out = false;
    }
    /* Every binding is in the WHERE clause rather than read back and compared,
     * so there is no window in which a caller could examine one and act on
     * another. `verdict = 'AUTO'` is what makes a shadow row permanently
     * unusable as a capability: shadow mode records what Atlas would have done
     * and can never do it. */
    static const char SQL[] =
        "SELECT 1 FROM verify_lifecycle_audit"
        " WHERE id = ?1 AND document_id = ?2 AND revision_id = ?3 AND to_status = ?4"
        "   AND content_hash = ?5 AND verdict = 'AUTO' AND consumed = 0;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, warrant_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, document_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, revision_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the warrant");
    }
    st = atlas_db_bind_text_opt(db, stmt, 4, to_status, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, content_hash, err);
    }
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW && ok_out != NULL) {
        *ok_out = true;
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_warrant_consume(atlas_db *db, int64_t warrant_id, const char *now,
                                             bool *spent_out, atlas_err *err) {
    if (spent_out != NULL) {
        *spent_out = false;
    }
    /* Names the state it observed, and the caller requires exactly one changed
     * row — A4's rule, so a replayed warrant loses deterministically instead of
     * transitioning twice. */
    static const char SQL[] =
        "UPDATE verify_lifecycle_audit SET consumed = 1, consumed_at = ?2"
        " WHERE id = ?1 AND consumed = 0 AND verdict = 'AUTO';";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, warrant_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the warrant id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    }
    atlas_db_finish(db, stmt);
    if (st == ATLAS_OK && spent_out != NULL) {
        *spent_out = sqlite3_changes(db->h) == 1;
    }
    return st;
}

/* --- reliability and outcomes ---------------------------------------------- */

atlas_status atlas_db_verify_reliability_get(atlas_db *db, int64_t actor_id, const char *domain,
                                             int *reliability_out, int *samples_out,
                                             atlas_verify_calibration *calibration_out,
                                             atlas_err *err) {
    if (reliability_out != NULL) {
        *reliability_out = -1;
    }
    if (samples_out != NULL) {
        *samples_out = 0;
    }
    if (calibration_out != NULL) {
        *calibration_out = ATLAS_CALIBRATION_INSUFFICIENT_DATA;
    }
    static const char SQL[] =
        "SELECT reliability, samples, calibration FROM verify_reliability"
        " WHERE actor_id = ?1 AND domain = ?2;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, actor_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the actor id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, domain != NULL ? domain : "", err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        /* NULL means there is no estimate, which is not the same as an estimate
         * of zero. The caller falls back to the documented prior. */
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL && reliability_out != NULL) {
            *reliability_out = sqlite3_column_int(stmt, 0);
        }
        if (samples_out != NULL) {
            *samples_out = sqlite3_column_int(stmt, 1);
        }
        if (calibration_out != NULL) {
            (void)atlas_verify_calibration_parse((const char *)sqlite3_column_text(stmt, 2),
                                                 calibration_out);
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_outcome_record(atlas_db *db, int64_t claim_id, int64_t actor_id,
                                            const char *domain, atlas_verify_verdict attested,
                                            bool truth, atlas_verify_outcome_source source,
                                            const char *now, atlas_err *err) {
    bool eligible = atlas_verify_outcome_eligible(source);

    static const char SQL[] =
        "INSERT INTO verify_outcomes(claim_id, actor_id, domain, attested, truth, source,"
        "  eligible, recorded_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"
        " ON CONFLICT(claim_id, actor_id) DO NOTHING;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, claim_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, actor_id) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 7, eligible ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the outcome");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, domain != NULL ? domain : "", err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, atlas_verify_verdict_name(attested), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, truth ? "TRUE" : "FALSE", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, atlas_verify_outcome_source_name(source), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, stmt, err);
    }
    atlas_db_finish(db, stmt);
    if (st != ATLAS_OK) {
        return st;
    }

    /* **The loop-breaker.** An ineligible outcome is stored — so the ineligible
     * case is auditable rather than absent — and moves nothing. A machine
     * transition driven by a source's own attestation can therefore be seen in
     * the outcome table and can never become evidence that the source was
     * right. Without this, a model would be teaching Atlas to trust it using
     * Atlas' trust in it, and every step of that would look reasonable. */
    if (!eligible) {
        return ATLAS_OK;
    }

    bool correct = (attested == ATLAS_ATTEST_SUPPORT && truth) ||
                   (attested == ATLAS_ATTEST_CONTRADICT && !truth);
    bool inconclusive = attested == ATLAS_ATTEST_INCONCLUSIVE;

    static const char USQL[] =
        "INSERT INTO verify_reliability(actor_id, domain, correct, incorrect, support_when_false,"
        "  contradict_when_true, inconclusive, abstained, samples, reliability, calibration,"
        "  updated_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,0,1,NULL,'INSUFFICIENT_DATA',?8)"
        " ON CONFLICT(actor_id, domain) DO UPDATE SET"
        "  correct = correct + excluded.correct,"
        "  incorrect = incorrect + excluded.incorrect,"
        "  support_when_false = support_when_false + excluded.support_when_false,"
        "  contradict_when_true = contradict_when_true + excluded.contradict_when_true,"
        "  inconclusive = inconclusive + excluded.inconclusive,"
        "  samples = samples + 1,"
        "  updated_at = excluded.updated_at;";
    sqlite3_stmt *us = NULL;
    st = atlas_db_prepare(db, USQL, &us, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(us, 1, actor_id) != SQLITE_OK ||
        sqlite3_bind_int(us, 3, (!inconclusive && correct) ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(us, 4, (!inconclusive && !correct) ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(us, 5, (attested == ATLAS_ATTEST_SUPPORT && !truth) ? 1 : 0) !=
            SQLITE_OK ||
        sqlite3_bind_int(us, 6, (attested == ATLAS_ATTEST_CONTRADICT && truth) ? 1 : 0) !=
            SQLITE_OK ||
        sqlite3_bind_int(us, 7, inconclusive ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, us);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the reliability update");
    }
    st = atlas_db_bind_text_opt(db, us, 2, domain != NULL ? domain : "", err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, us, 8, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_step_done(db, us, err);
    }
    atlas_db_finish(db, us);
    return st;
}

/* --- the reads the deterministic verifiers are built from -------------------
 *
 * Three bounded lookups, and every one of them is a **read**. No verifier in
 * A9.2 creates a process, runs a repository's build, or executes a command from
 * anywhere. That restriction is the reason a deterministic verdict can be
 * trusted without asking who supplied the claim: there is no input on any of
 * these paths that could become an instruction.
 *
 * They live here rather than in `src/verify/detverify.c` because sqlite3 types
 * do not leave `src/db` — the layering rule, not a convenience. */

atlas_status atlas_db_verify_file_hash(atlas_db *db, int64_t repo_id, const char *path_text,
                                       atlas_buf *hash_out, bool *found_out, atlas_err *err) {
    if (found_out != NULL) {
        *found_out = false;
    }
    /* A deleted file has no current content, and reporting its last known hash
     * would let a claim about bytes that are gone verify for ever. */
    static const char SQL[] =
        "SELECT content_hash FROM files"
        " WHERE repo_id = ?1 AND path_text = ?2 AND deleted_scan_id IS NULL"
        "   AND content_hash IS NOT NULL;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, path_text, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        st = take_text(hash_out, (const char *)sqlite3_column_text(stmt, 0), err);
        if (st == ATLAS_OK && found_out != NULL) {
            *found_out = true;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* Symbols of a name in the current semantic generation, and whether that
 * generation is complete enough to establish an **absence**.
 *
 * The completeness flag is the whole difficulty of `atlas.symbol_absent`. A
 * generation with failed or unsupported translation units describes part of a
 * repository, and "I did not find it" over part of a repository is not "it is
 * not there". Reporting the second would let a remediation detector close an
 * obligation that is still outstanding — the exact failure that makes an
 * automatic RESOLVED dangerous — so an incomplete generation yields
 * UNAVAILABLE and the transition does not happen. */
atlas_status atlas_db_verify_sem_symbol(atlas_db *db, int64_t repo_id, const char *name,
                                        int64_t *count_out, bool *complete_out, bool *indexed_out,
                                        atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (complete_out != NULL) {
        *complete_out = false;
    }
    if (indexed_out != NULL) {
        *indexed_out = false;
    }
    int64_t generation = 0;
    {
        static const char GSQL[] =
            "SELECT g.id, g.status, g.tu_failed, g.tu_unsupported, g.tu_partial"
            "  FROM sem_current c JOIN sem_generations g ON g.id = c.generation_id"
            " WHERE c.repo_id = ?1;";
        sqlite3_stmt *gs = NULL;
        atlas_status st = atlas_db_prepare(db, GSQL, &gs, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(gs, 1, repo_id) != SQLITE_OK) {
            atlas_db_finish(db, gs);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
        }
        if (sqlite3_step(gs) == SQLITE_ROW) {
            generation = sqlite3_column_int64(gs, 0);
            const char *status = (const char *)sqlite3_column_text(gs, 1);
            int64_t failed = sqlite3_column_int64(gs, 2);
            int64_t unsupported = sqlite3_column_int64(gs, 3);
            int64_t partial = sqlite3_column_int64(gs, 4);
            if (indexed_out != NULL) {
                *indexed_out = true;
            }
            if (complete_out != NULL) {
                *complete_out = status != NULL && strcmp(status, "COMPLETE") == 0 && failed == 0 &&
                                unsupported == 0 && partial == 0;
            }
        }
        atlas_db_finish(db, gs);
    }
    if (generation == 0) {
        return ATLAS_OK;
    }

    static const char SQL[] =
        "SELECT COUNT(*) FROM sem_symbols"
        " WHERE generation_id = ?1 AND name = ?2 AND external = 0;";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, generation) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the generation");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, name, err);
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW && count_out != NULL) {
        *count_out = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* A call edge the compiler proved. `evidence = 'PROVEN'` and nothing weaker:
 * A8-CI's rule is that PROVEN means the compiler proved it, and a CANDIDATE
 * edge — a call through a function pointer, say — never satisfies a
 * deterministic verifier however useful it is to a human reading a graph. */
atlas_status atlas_db_verify_sem_proven_edge(atlas_db *db, int64_t repo_id, const char *src,
                                             const char *dst, bool *exists_out, bool *indexed_out,
                                             atlas_err *err) {
    if (exists_out != NULL) {
        *exists_out = false;
    }
    if (indexed_out != NULL) {
        *indexed_out = false;
    }
    static const char SQL[] =
        "SELECT EXISTS("
        "  SELECT 1 FROM sem_edges e"
        "    JOIN sem_symbols s ON s.generation_id = e.generation_id AND s.usr = e.src_usr"
        "    JOIN sem_symbols d ON d.generation_id = e.generation_id AND d.usr = e.dst_usr"
        "   WHERE e.generation_id = (SELECT generation_id FROM sem_current WHERE repo_id = ?1)"
        "     AND e.kind = 'CALLS' AND e.evidence = 'PROVEN'"
        "     AND s.name = ?2 AND d.name = ?3),"
        " (SELECT COUNT(*) FROM sem_current WHERE repo_id = ?1);";
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db, SQL, &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, src, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, dst, err);
    }
    if (st == ATLAS_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        if (exists_out != NULL) {
            *exists_out = sqlite3_column_int(stmt, 0) != 0;
        }
        if (indexed_out != NULL) {
            *indexed_out = sqlite3_column_int64(stmt, 1) > 0;
        }
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_verify_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err) {
    /* A4's rule: `repositories.id` is a reused rowid, so a soft pointer left
     * behind would eventually name a different repository. Cleared inside the
     * removal's own transaction. `repo_identity_hash` stays — it is the durable
     * identity and survives re-registration, which is the whole reason it is
     * recorded beside the id. */
    static const char *const SQL[] = {
        "UPDATE verify_claims SET repo_id = 0 WHERE repo_id = ?1;",
        "UPDATE verify_evidence SET repo_id = 0 WHERE repo_id = ?1;",
    };
    for (size_t i = 0; i < sizeof SQL / sizeof SQL[0]; i++) {
        sqlite3_stmt *stmt = NULL;
        atlas_status st = atlas_db_prepare(db, SQL[i], &stmt, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
            atlas_db_finish(db, stmt);
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind the repository id");
        }
        st = atlas_db_step_done(db, stmt, err);
        atlas_db_finish(db, stmt);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

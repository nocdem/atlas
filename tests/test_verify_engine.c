/* Atlas - A9.2: the verification engine end to end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The acceptance fixtures, driven against a real database in process: no daemon
 * and no transport, because what is under test is the engine, the schema and
 * the authority boundary rather than a wire format. `tests/test_decision_kind.c`
 * is the model this follows.
 *
 * The fixtures the season names, and where each lives:
 *
 *   A  exact mechanical behaviour verified without calibration ....... below
 *   B  a bounded invariant established over a stated scope ........... below
 *   C  an obligation opened, then discharged, by a detector .......... below
 *   J  five agents cannot accept a risk .............................. below
 *   K  a faster architecture does not become the approved one ........ below
 *   L  a pre-existing root-owned rule executing is not a new judgment . below
 *
 * D, E, F and the independence attacks are in `tests/test_verify_model.c`,
 * where the aggregation can be driven directly as the pure function it is.
 */
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/verify.h"
#include "atlas/verifypolicy.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"


/* A single text column, for the audit-row assertions. There is no shared
 * helper for this in db_internal.h, and adding one to the library for a test's
 * convenience would be the wrong direction. */
static void query_text(atlas_db *db, const char *sql, atlas_buf *out, atlas_err *err) {
    sqlite3_stmt *q = NULL;
    T_OK(atlas_db_prepare(db, sql, &q, err), err);
    if (sqlite3_step(q) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(q, 0);
        T_OK(atlas_buf_set_str(out, t != NULL ? t : "", err), err);
    }
    atlas_db_finish(db, q);
}

/* --- environment ---------------------------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

/* Content digests the fixtures assert against. Macros rather than local arrays
 * because they are concatenated into verifier inputs at compile time. */
#define HASH1 "1111111111111111111111111111111111111111111111111111111111111111"
#define HASH2 "2222222222222222222222222222222222222222222222222222222222222222"
#define HASH3 "3333333333333333333333333333333333333333333333333333333333333333"
#define HASH4 "4444444444444444444444444444444444444444444444444444444444444444"
#define HASH5 "5555555555555555555555555555555555555555555555555555555555555555"
#define HASH6 "6666666666666666666666666666666666666666666666666666666666666666"
#define HASH7 "7777777777777777777777777777777777777777777777777777777777777777"

#define ROOT_COMMIT "dddddddddddddddddddddddddddddddddddddddd"

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-verify-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-verify-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);

    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO commits(repo_id, oid, parent_count, subject)"
                           " VALUES(%lld, '%s', 0, 'root');",
                           (long long)e->repo_id, ROOT_COMMIT),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* A knowledge record of a given kind, left PROPOSED. */
static void propose(env *e, atlas_decision_kind kind, const char *title, atlas_buf *uid_out,
                    atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    op.knowledge_kind = kind;
    op.knowledge_kind_given = true;
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a body for the fixture", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);
}

static int64_t document_of(env *e, const atlas_buf *uid) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t doc = 0, repo = 0;
    bool found = false;
    T_OK(atlas_db_decision_find_uid(e->db, atlas_buf_cstr(uid), &doc, &repo, &found, &err), &err);
    T_CHECK(found);
    return doc;
}

static void status_of(env *e, const atlas_buf *uid, char *out, size_t n) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_decision_document_status(e->db, document_of(e, uid), out, n, &err), &err);
}

/* A claim attached to a record's current revision. */
static int64_t claim(env *e, const atlas_buf *uid, const char *text,
                     atlas_verify_claim_semantics sem, const char *verifier, const char *input,
                     atlas_err *err) {
    int64_t doc = document_of(e, uid);
    int64_t rev = 0, no = 0;
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    char state[16];
    T_OK(atlas_db_decision_latest_revision(e->db, doc, &rev, &no, hash, sizeof hash, state,
                                           sizeof state, err),
         err);

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    c.repo_id = e->repo_id;
    c.document_id = doc;
    c.revision_id = rev;
    c.semantics = sem;
    T_OK(atlas_buf_set_str(&c.text, text, err), err);
    T_OK(atlas_buf_set_str(&c.domain, "fixture", err), err);
    if (verifier != NULL) {
        T_OK(atlas_buf_set_str(&c.verifier, verifier, err), err);
        T_OK(atlas_buf_set_str(&c.verifier_input, input != NULL ? input : "", err), err);
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_verify_claim_insert(e->db, &c, now, err), err);
    int64_t id = c.id;
    atlas_verify_claim_free(&c);
    return id;
}

/* A file in the index, so `atlas.content_hash` has something to read. */
static void seed_file(env *e, const char *path, const char *hash, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO scans(repo_id, started_at, status)"
                           "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular', '%s',"
                           "         last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');",
                           (long long)e->repo_id, (long long)e->repo_id, path, path, hash),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

/* An AI actor and its supporting attestation, resting on nothing declared —
 * the shape a model submission actually has. */
static void ai_supports(env *e, int64_t claim_id, const char *name, atlas_err *err) {
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    atlas_verify_actor a;
    atlas_verify_actor_init(&a);
    a.cls = ATLAS_ACTOR_AI_AGENT;
    a.identity = ATLAS_ACTOR_IDENTITY_SELF_DECLARED;
    T_OK(atlas_buf_set_str(&a.name, name, err), err);
    T_OK(atlas_db_verify_actor_upsert(e->db, &a, now, err), err);

    atlas_verify_attestation at;
    atlas_verify_attestation_init(&at);
    at.claim_id = claim_id;
    at.actor_id = a.id;
    at.verdict = ATLAS_ATTEST_SUPPORT;
    at.self_confidence = 98; /* stored, and never Atlas' confidence */
    T_OK(atlas_db_verify_attestation_insert(e->db, &at, NULL, 0, now, err), err);
    atlas_verify_attestation_free(&at);
    atlas_verify_actor_free(&a);
}

/* A policy built from text, so every test states the exact rules it relies on
 * rather than sharing a fixture nobody reads. */
static void policy_from(const char *text, atlas_verifypolicy *p) {
    atlas_verifypolicy_parse_buffer(text, strlen(text), p);
}

/* --- Fixture A: exact mechanical behaviour, no calibration ---------------- */

static void test_a_deterministic_claim_is_verified_without_any_calibration(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/parser.c", HASH1, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_INVARIANT, "the parser file is the reviewed one", &uid, &err);
    int64_t c = claim(&e, &uid, "src/parser.c has the reviewed content",
                      ATLAS_CLAIM_DESCRIPTIVE, "atlas.content_hash",
                      "path=src/parser.c;sha256=" HASH1, &err);

    /* Enforcement on for the deterministic path, and **nothing whatsoever**
     * about calibration configured — because there is no calibration on this
     * machine and there never will be for a claim of this shape. */
    atlas_verifypolicy p;
    policy_from("enabled = yes\n"
                "policy_id = fixture-a\n"
                "deterministic_enforce = yes\n"
                "min_confidence = 100\n"
                "allow = INVARIANT PROPOSED APPROVED atlas.content_hash\n",
                &p);
    T_REQUIRE_MSG(p.state == ATLAS_VERIFYPOLICY_ENABLED, "the fixture policy did not load: %s",
                  atlas_verifypolicy_reason_name(p.reason));

    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_EQ_INT((int)a.basis, (int)ATLAS_VERIFY_BASIS_DETERMINISTIC);
    T_EQ_INT((int)a.check, (int)ATLAS_CHECK_PASS);
    T_EQ_INT((int)a.aggregate.state, (int)ATLAS_VERIFY_VERIFIED);

    /* **The season's central claim, asserted directly.** No actor has any
     * resolved history, so calibration is INSUFFICIENT_DATA — and the
     * transition happened anyway, because how often some source has been right
     * is not an input to a mechanical evaluation of a bounded truth condition. */
    T_EQ_INT((int)a.aggregate.calibration, (int)ATLAS_CALIBRATION_INSUFFICIENT_DATA);
    T_EQ_INT(a.aggregate.calibrated_probability, -1);
    T_CHECK_MSG(a.transitioned, "a mechanically verified claim did not transition");
    T_EQ_INT((int)a.aggregate.verdict, (int)ATLAS_POLICY_AUTO);

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK_MSG(strcmp(status, "APPROVED") == 0, "record is %s, not APPROVED", status);

    /* And the calibration reason was never even reached: the gate is guarded on
     * the basis, so a deterministic verdict cannot be blocked by a statistic
     * about somebody's past. */
    for (size_t i = 0; i < a.aggregate.reason_count; i++) {
        T_CHECK_MSG(a.aggregate.reasons[i] != ATLAS_VREASON_CALIBRATION_INSUFFICIENT,
                    "a deterministic verdict was gated on calibration");
    }

    /* The ledger records *which* authority acted, distinguishably. */
    int64_t n = -1;
    T_OK(atlas_db_query_int64(
             e.db, "SELECT COUNT(*) FROM decision_events WHERE actor = 'VERIFICATION_POLICY';", &n,
             &err),
         &err);
    T_EQ_INT((int)n, 1);
    T_OK(atlas_db_query_int64(
             e.db,
             "SELECT COUNT(*) FROM decision_events WHERE actor = 'LOCAL_OPERATOR_CONFIRMED';", &n,
             &err),
         &err);
    T_CHECK_MSG(n == 0, "a machine transition was recorded as an operator one");

    /* §40: the audit row can reconstruct the decision. */
    atlas_buf pol = ATLAS_BUF_INIT;
    query_text(e.db, "SELECT policy_id FROM verify_lifecycle_audit LIMIT 1;", &pol, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&pol), "fixture-a") == 0,
                "the audit row does not name the policy: %s", atlas_buf_cstr(&pol));
    atlas_buf_free(&pol);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_the_warrant_is_single_use(void) {
    /* A replayed warrant must lose deterministically, exactly as a replayed
     * operator challenge does. The machine path binds no more loosely than the
     * human one; what differs is who can mint the capability. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/a.c", HASH2, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_INVARIANT, "replay", &uid, &err);
    int64_t c = claim(&e, &uid, "a.c is as reviewed", ATLAS_CLAIM_DESCRIPTIVE,
                      "atlas.content_hash", "path=src/a.c;sha256=" HASH2, &err);

    atlas_verifypolicy p;
    policy_from("enabled = yes\ndeterministic_enforce = yes\nmin_confidence = 100\n"
                "allow = INVARIANT PROPOSED APPROVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);
    T_CHECK(a.transitioned);

    int64_t warrant = a.audit_id;
    T_CHECK(warrant > 0);

    /* The row is spent. Checked against the *real* content hash the warrant was
     * bound to, read back from the audit row — passing an empty string here
     * would make the assertion pass for a live warrant too, which is coverage
     * that is not coverage. */
    atlas_buf bound = ATLAS_BUF_INIT;
    query_text(e.db, "SELECT content_hash FROM verify_lifecycle_audit LIMIT 1;", &bound, &err);
    T_REQUIRE_MSG(bound.len > 0, "the audit row recorded no content hash");
    bool ok = true;
    T_OK(atlas_db_verify_warrant_check(e.db, warrant, a.document_id, a.revision_id, "APPROVED",
                                       atlas_buf_cstr(&bound), &ok, &err),
         &err);
    T_CHECK_MSG(!ok, "a spent warrant still authorises a transition");
    atlas_buf_free(&bound);

    /* Driving the write point directly with the spent warrant is refused. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_AUTO_APPROVE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op.uid, uid.data, uid.len, &err), &err);
    op.warrant_id = warrant;
    T_CHECK_MSG(atlas_decision_apply(e.db, &op, &res, &err) != ATLAS_OK,
                "a spent warrant transitioned a record a second time");
    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_machine_transition_without_a_warrant_is_refused(void) {
    /* There is no request that can supply one, and the write point says so
     * rather than defaulting. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_INVARIANT, "no warrant", &uid, &err);

    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_AUTO_APPROVE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op.uid, uid.data, uid.len, &err), &err);
    op.warrant_id = 0;
    T_CHECK_MSG(atlas_decision_apply(e.db, &op, &res, &err) != ATLAS_OK,
                "an unwarranted machine transition succeeded");

    /* An invented warrant id is refused too: the check is against the document,
     * the revision, the target state and the content hash, so guessing an
     * integer buys nothing. */
    op.warrant_id = 424242;
    T_CHECK_MSG(atlas_decision_apply(e.db, &op, &res, &err) != ATLAS_OK,
                "a fabricated warrant id transitioned a record");

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK(strcmp(status, "PROPOSED") == 0);

    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- Fixture B: a bounded invariant, and semantic inflation refused ------- */

static void test_a_normative_claim_is_never_established_mechanically(void) {
    /* **Separation 4, end to end.** The verifier can see that the file has the
     * reviewed content. It cannot thereby establish that the file *shall
     * always* have it — and a policy authorising the transition does not
     * change that, because the refusal is decided before the policy is read. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/proto.c", HASH3, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_INVARIANT, "the protocol shall always emit zero", &uid, &err);
    int64_t c = claim(&e, &uid, "the protocol shall always limit outputs this way",
                      ATLAS_CLAIM_NORMATIVE, "atlas.content_hash",
                      "path=src/proto.c;sha256=" HASH3, &err);

    atlas_verifypolicy p;
    policy_from("enabled = yes\ndeterministic_enforce = yes\nmin_confidence = 0\n"
                "allow = INVARIANT PROPOSED APPROVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    /* A normative claim is a judgment however mechanical its neighbourhood. */
    T_EQ_INT((int)a.basis, (int)ATLAS_VERIFY_BASIS_JUDGMENT);
    T_CHECK_MSG(!a.transitioned, "an observation became permanent policy");
    T_EQ_INT((int)a.aggregate.verdict, (int)ATLAS_POLICY_FORBIDDEN);

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK(strcmp(status, "PROPOSED") == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_an_absence_is_not_established_over_a_partial_index(void) {
    /* The difference between "I did not find it" and "it is not there". With no
     * semantic generation published at all, `atlas.symbol_absent` reports
     * UNAVAILABLE — which is not a pass and not a fail — so an obligation whose
     * remediation condition is an absence stays open. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_OBLIGATION, "debug signing must be removed", &uid, &err);
    int64_t c = claim(&e, &uid, "no debug keystore symbol remains", ATLAS_CLAIM_DESCRIPTIVE,
                      "atlas.symbol_absent", "symbol=debug_keystore", &err);

    atlas_verifypolicy p;
    policy_from("enabled = yes\ndeterministic_enforce = yes\nmin_confidence = 0\n"
                "allow = OBLIGATION PROPOSED APPROVED atlas.symbol_absent\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_EQ_INT((int)a.check, (int)ATLAS_CHECK_UNAVAILABLE);
    T_CHECK_MSG(!a.transitioned,
                "an absence was established over a repository Atlas had not indexed");
    T_CHECK_MSG(a.aggregate.state != ATLAS_VERIFY_VERIFIED,
                "could-not-look was reported as verified");

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- Fixture C: an obligation discharged --------------------------------- */

static void test_an_obligation_resolves_when_its_condition_is_mechanically_met(void) {
    /* The remediation path: an approved obligation whose exact condition is
     * deterministically verified may be closed out automatically, and the
     * record keeps its whole history. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "release/signing.conf", HASH4, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_OBLIGATION, "release signing must use the production key",
            &uid, &err);
    int64_t doc = document_of(&e, &uid);

    /* Approve it first, by the deterministic path, so the fixture reaches
     * APPROVED without inventing an operator. */
    int64_t c1 = claim(&e, &uid, "the signing config is the reviewed one",
                       ATLAS_CLAIM_DESCRIPTIVE, "atlas.content_hash",
                       "path=release/signing.conf;sha256=" HASH4, &err);
    atlas_verifypolicy p;
    policy_from("enabled = yes\ndeterministic_enforce = yes\nmin_confidence = 0\n"
                "allow = OBLIGATION PROPOSED APPROVED atlas.content_hash\n"
                "allow = OBLIGATION APPROVED RESOLVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a1;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c1, "proj", &a1, &err), &err);
    T_REQUIRE_MSG(a1.transitioned, "the obligation was not approved by the fixture");

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_REQUIRE_MSG(strcmp(status, "APPROVED") == 0, "obligation is %s", status);

    /* Now the remediation claim, against the approved revision. */
    int64_t rev = 0;
    T_OK(atlas_db_decision_approved_revision(e.db, doc, &rev, &err), &err);
    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    c.repo_id = e.repo_id;
    c.document_id = doc;
    c.revision_id = rev;
    T_OK(atlas_buf_set_str(&c.text, "the demand has been met", &err), &err);
    T_OK(atlas_buf_set_str(&c.verifier, "atlas.content_hash", &err), &err);
    T_OK(atlas_buf_set_str(&c.verifier_input, "path=release/signing.conf;sha256=" HASH4, &err),
         &err);
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_verify_claim_insert(e.db, &c, now, &err), &err);

    atlas_verify_assessment a2;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c.id, "proj", &a2, &err), &err);
    T_CHECK_MSG(a2.transitioned, "a discharged obligation did not resolve");
    status_of(&e, &uid, status, sizeof status);
    T_CHECK_MSG(strcmp(status, "RESOLVED") == 0, "obligation is %s, not RESOLVED", status);

    /* Nothing was deleted and the history is intact: the approval and the
     * resolution are both in the ledger, both attributed to the policy. */
    int64_t n = -1;
    T_OK(atlas_db_query_int64(e.db,
                              "SELECT COUNT(*) FROM decision_events"
                              " WHERE actor = 'VERIFICATION_POLICY';",
                              &n, &err),
         &err);
    T_EQ_INT((int)n, 2);

    atlas_verify_claim_free(&c);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_decision_is_never_resolved_automatically(void) {
    /* RESOLVED is reachable only for a kind whose approved form makes a demand.
     * "This architectural decision has been resolved" is not a sentence with a
     * meaning, and the state machine — asked with the kind Atlas has stored,
     * never one a caller supplied — is what refuses it. */
    atlas_verify_reason why = ATLAS_VREASON_NONE;
    T_CHECK(atlas_verifypolicy_transition_forbidden(ATLAS_DECISION_KIND_DECISION,
                                                    ATLAS_DECISION_APPROVED,
                                                    ATLAS_DECISION_RESOLVED, &why));
    T_EQ_INT((int)why, (int)ATLAS_VREASON_TRANSITION_ILLEGAL);
}

/* --- Fixture J: five agents cannot accept a risk -------------------------- */

static void test_no_number_of_agreeing_agents_accepts_a_risk(void) {
    /* **Fixture J.** Five AI agents say a privacy leak is acceptable. The risk's
     * *existence* is an ordinary factual matter and may be verified to any
     * strength. Its *acceptance* is a claim about what the project is willing
     * to live with, and no amount of agreement or reliability supplies one. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_ACCEPTED_RISK, "the telemetry leak is acceptable", &uid, &err);
    int64_t c = claim(&e, &uid, "this privacy leak is acceptable", ATLAS_CLAIM_DESCRIPTIVE, NULL,
                      NULL, &err);

    static const char *const NAMES[] = {"agent-one", "agent-two", "agent-three", "agent-four",
                                        "agent-five"};
    for (size_t i = 0; i < 5; i++) {
        ai_supports(&e, c, NAMES[i], &err);
    }

    /* The most permissive policy that could exist, including a rule that names
     * exactly this transition. */
    atlas_verifypolicy p;
    policy_from("enabled = yes\n"
                "deterministic_enforce = yes\n"
                "empirical_enforce = yes\n"
                "min_confidence = 0\n"
                "min_evidence_groups = 1\n"
                "allow = ACCEPTED_RISK PROPOSED APPROVED atlas.content_hash\n",
                &p);
    /* The policy does not even load: a rule naming a forbidden transition is
     * malformed rather than inert. */
    T_CHECK_MSG(p.state == ATLAS_VERIFYPOLICY_DISABLED,
                "a policy authorising risk acceptance was accepted");

    /* And with a policy that does load, the transition is still FORBIDDEN. */
    policy_from("enabled = yes\ndeterministic_enforce = yes\nempirical_enforce = yes\n"
                "min_confidence = 0\nmin_evidence_groups = 1\n"
                "allow = INVARIANT PROPOSED APPROVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_CHECK_MSG(!a.transitioned, "five agreeing agents accepted a risk");
    T_EQ_INT((int)a.aggregate.verdict, (int)ATLAS_POLICY_FORBIDDEN);
    bool named = false;
    for (size_t i = 0; i < a.aggregate.reason_count; i++) {
        if (a.aggregate.reasons[i] == ATLAS_VREASON_RISK_REQUIRES_AUTHORITY) {
            named = true;
        }
    }
    T_CHECK_MSG(named, "the refusal did not say that risk acceptance needs authority");

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK(strcmp(status, "PROPOSED") == 0);

    /* And the five agents were one evidence group, not five: they declared
     * nothing, so Atlas could not demonstrate independence and did not assume
     * it. */
    T_EQ_INT(a.aggregate.support_count, 5);
    T_EQ_INT(a.aggregate.independent_groups, 1);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- Fixture K: a faster architecture is still a choice ------------------- */

static void test_a_technical_premise_does_not_carry_a_product_decision(void) {
    /* **Fixture K.** Several sources agree architecture A is faster. That
     * premise may be verified to any strength. Adopting A is a normative
     * choice, and no policy switch exists that would automate it. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_DECISION, "adopt architecture A", &uid, &err);
    int64_t c = claim(&e, &uid, "architecture A shall be the project's design",
                      ATLAS_CLAIM_NORMATIVE, NULL, NULL, &err);
    for (size_t i = 0; i < 3; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "bench-%zu", i);
        ai_supports(&e, c, name, &err);
    }

    atlas_verifypolicy p;
    policy_from("enabled = yes\ndeterministic_enforce = yes\nempirical_enforce = yes\n"
                "min_confidence = 0\nmin_evidence_groups = 1\n"
                "allow = DECISION PROPOSED APPROVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_EQ_INT((int)a.basis, (int)ATLAS_VERIFY_BASIS_JUDGMENT);
    T_CHECK_MSG(!a.transitioned, "a benchmark result adopted an architecture");
    T_EQ_INT((int)a.aggregate.verdict, (int)ATLAS_POLICY_FORBIDDEN);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- Fixture L: policy execution is not new judgment ---------------------- */

static void test_a_pre_existing_rule_executing_is_not_a_new_judgment(void) {
    /* **Fixture L.** A root-owned policy already establishes that a condition
     * is a release blocker. A verifier detects the condition. Deriving the
     * obligation is that pre-existing decision being *applied*, not a new one
     * being made — which is why it is allowed where Fixture K is not.
     *
     * The distinction is visible in the record: the policy named the kind and
     * the transition in advance, and the claim it acted on is descriptive. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "build/keystore.properties", HASH5, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_OBLIGATION, "public release is blocked while debug signing "
                                                "is present",
            &uid, &err);
    int64_t c = claim(&e, &uid, "the debug keystore configuration is present",
                      ATLAS_CLAIM_DESCRIPTIVE, "atlas.content_hash",
                      "path=build/keystore.properties;sha256=" HASH5, &err);

    atlas_verifypolicy p;
    policy_from("enabled = yes\n"
                "policy_id = release-gate-v1\n"
                "deterministic_enforce = yes\n"
                "min_confidence = 0\n"
                "allow = OBLIGATION PROPOSED APPROVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_CHECK_MSG(a.transitioned, "a pre-existing release rule did not execute");
    T_EQ_INT((int)a.basis, (int)ATLAS_VERIFY_BASIS_DETERMINISTIC);
    T_EQ_INT((int)a.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK(strcmp(status, "APPROVED") == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- shadow mode ---------------------------------------------------------- */

static void test_shadow_mode_records_a_full_verdict_and_changes_nothing(void) {
    /* Shadow is a complete result, not a silence — and it is structurally
     * incapable of becoming an action, because a warrant check requires
     * `verdict = 'AUTO'`. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/s.c", HASH6, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_INVARIANT, "shadow", &uid, &err);
    int64_t c = claim(&e, &uid, "s.c is as reviewed", ATLAS_CLAIM_DESCRIPTIVE,
                      "atlas.content_hash", "path=src/s.c;sha256=" HASH6, &err);

    /* Every gate passes; enforcement is off. */
    atlas_verifypolicy p;
    policy_from("enabled = yes\nmin_confidence = 0\n"
                "allow = INVARIANT PROPOSED APPROVED atlas.content_hash\n",
                &p);
    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, &p, c, "proj", &a, &err), &err);

    T_EQ_INT((int)a.aggregate.state, (int)ATLAS_VERIFY_VERIFIED);
    T_EQ_INT((int)a.aggregate.verdict, (int)ATLAS_POLICY_SHADOW);
    T_CHECK_MSG(!a.transitioned, "shadow mode changed a lifecycle state");

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK(strcmp(status, "PROPOSED") == 0);

    /* The row exists and records what would have happened. */
    atlas_buf verdict = ATLAS_BUF_INIT;
    query_text(e.db, "SELECT verdict FROM verify_lifecycle_audit LIMIT 1;", &verdict, &err);
    T_CHECK(strcmp(atlas_buf_cstr(&verdict), "SHADOW") == 0);
    atlas_buf_free(&verdict);

    /* And it can never be spent. */
    bool ok = true;
    T_OK(atlas_db_verify_warrant_check(e.db, a.audit_id, a.document_id, a.revision_id, "APPROVED",
                                       "", &ok, &err),
         &err);
    T_CHECK_MSG(!ok, "a shadow row was usable as a warrant");

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_no_policy_means_nothing_is_automatic(void) {
    /* Fail-closed at zero, end to end. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/n.c", HASH7, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_INVARIANT, "no policy", &uid, &err);
    int64_t c = claim(&e, &uid, "n.c is as reviewed", ATLAS_CLAIM_DESCRIPTIVE,
                      "atlas.content_hash", "path=src/n.c;sha256=" HASH7, &err);

    atlas_verify_assessment a;
    T_OK(atlas_verify_autolifecycle_run(e.db, NULL, c, "proj", &a, &err), &err);
    T_CHECK_MSG(!a.transitioned, "a record transitioned with no policy installed");
    bool named = false;
    for (size_t i = 0; i < a.aggregate.reason_count; i++) {
        if (a.aggregate.reasons[i] == ATLAS_VREASON_NO_POLICY) {
            named = true;
        }
    }
    T_CHECK(named);

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK(strcmp(status, "PROPOSED") == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- forgery -------------------------------------------------------------- */

static void test_a_model_cannot_create_a_tool_actor(void) {
    /* §44 at the write point. An AI saying "clang proves this" is not Atlas
     * running clang, and the refusal is a refusal rather than a discount. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);

    static const atlas_verify_actor_class FORGEABLE[] = {ATLAS_ACTOR_TOOL, ATLAS_ACTOR_TEST,
                                                         ATLAS_ACTOR_RUNTIME_OBSERVATION,
                                                         ATLAS_ACTOR_ATLAS_VERIFIER};
    for (size_t i = 0; i < sizeof FORGEABLE / sizeof FORGEABLE[0]; i++) {
        atlas_verify_actor a;
        atlas_verify_actor_init(&a);
        a.cls = FORGEABLE[i];
        a.identity = ATLAS_ACTOR_IDENTITY_SELF_DECLARED;
        T_OK(atlas_buf_set_str(&a.name, "clang", &err), &err);
        T_CHECK_MSG(atlas_db_verify_actor_upsert(e.db, &a, now, &err) != ATLAS_OK,
                    "a self-declared %s actor was accepted",
                    atlas_verify_actor_class_name(FORGEABLE[i]));
        atlas_verify_actor_free(&a);

        /* Peer authentication is not enough either: being able to open the
         * socket says who the process runs as, not that it ran a compiler. */
        atlas_verify_actor b;
        atlas_verify_actor_init(&b);
        b.cls = FORGEABLE[i];
        b.identity = ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED;
        T_CHECK_MSG(atlas_db_verify_actor_upsert(e.db, &b, now, &err) != ATLAS_OK,
                    "a peer-authenticated %s actor was accepted",
                    atlas_verify_actor_class_name(FORGEABLE[i]));
        atlas_verify_actor_free(&b);
    }

    int64_t n = -1;
    T_OK(atlas_db_query_int64(e.db, "SELECT COUNT(*) FROM verify_actors;", &n, &err), &err);
    T_CHECK_MSG(n == 0, "%lld forged actors were written", (long long)n);
    env_close(&e);
}

static void test_evidence_must_say_what_sort_of_thing_it_is(void) {
    /* There is no `TEXT` evidence class and no unclassified evidence: opaque
     * provenance-free prose is exactly what this phase exists to stop being
     * counted as evidence. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);

    atlas_verify_evidence ev;
    atlas_verify_evidence_init(&ev);
    ev.cls = ATLAS_EVIDENCE_UNKNOWN;
    T_CHECK_MSG(atlas_db_verify_evidence_insert(e.db, &ev, now, &err) != ATLAS_OK,
                "unclassified evidence was accepted");
    atlas_verify_evidence_free(&ev);
    env_close(&e);
}

static void test_a_claim_is_refused_rather_than_truncated(void) {
    /* A shortened proposition is a different proposition, and one whose scope
     * has quietly widened. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    for (size_t i = 0; i < ATLAS_VERIFY_CLAIM_TEXT_MAX + 10u; i++) {
        T_OK(atlas_buf_append_ch(&c.text, 'x', &err), &err);
    }
    T_CHECK_MSG(atlas_db_verify_claim_insert(e.db, &c, now, &err) != ATLAS_OK,
                "an oversized claim was accepted");
    atlas_verify_claim_free(&c);

    int64_t n = -1;
    T_OK(atlas_db_query_int64(e.db, "SELECT COUNT(*) FROM verify_claims;", &n, &err), &err);
    T_EQ_INT((int)n, 0);
    env_close(&e);
}

/* --- backward compatibility ---------------------------------------------- */

static void test_an_existing_record_with_no_claims_is_unverified(void) {
    /* §51. Every record written before this phase reports UNVERIFIED, without a
     * migration touching a single decision row, because a record with no claims
     * has no evidence and the state is derived rather than stored. Nothing
     * fabricates historical confidence. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, ATLAS_DECISION_KIND_DECISION, "an old record", &uid, &err);

    /* There is no verification column on the document at all — the guarantee is
     * structural rather than a default value somebody remembered to write. */
    int64_t n = -1;
    T_OK(atlas_db_query_int64(e.db,
                              "SELECT COUNT(*) FROM pragma_table_info('decision_documents')"
                              " WHERE name LIKE '%verif%';",
                              &n, &err),
         &err);
    T_CHECK_MSG(n == 0, "a verification column was added to decision_documents");

    /* And an aggregate over no attestations is UNVERIFIED. */
    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, NULL, 0, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_EQ_INT((int)a.state, (int)ATLAS_VERIFY_UNVERIFIED);

    char status[24];
    status_of(&e, &uid, status, sizeof status);
    T_CHECK_MSG(strcmp(status, "PROPOSED") == 0,
                "the migration changed an existing lifecycle status");

    atlas_buf_free(&uid);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a deterministic claim is verified without any calibration",
     test_a_deterministic_claim_is_verified_without_any_calibration},
    {"the warrant is single-use", test_the_warrant_is_single_use},
    {"a machine transition without a warrant is refused",
     test_a_machine_transition_without_a_warrant_is_refused},
    {"a normative claim is never established mechanically",
     test_a_normative_claim_is_never_established_mechanically},
    {"an absence is not established over a partial index",
     test_an_absence_is_not_established_over_a_partial_index},
    {"an obligation resolves when its condition is mechanically met",
     test_an_obligation_resolves_when_its_condition_is_mechanically_met},
    {"a decision is never resolved automatically", test_a_decision_is_never_resolved_automatically},
    {"no number of agreeing agents accepts a risk",
     test_no_number_of_agreeing_agents_accepts_a_risk},
    {"a technical premise does not carry a product decision",
     test_a_technical_premise_does_not_carry_a_product_decision},
    {"a pre-existing rule executing is not a new judgment",
     test_a_pre_existing_rule_executing_is_not_a_new_judgment},
    {"shadow mode records a full verdict and changes nothing",
     test_shadow_mode_records_a_full_verdict_and_changes_nothing},
    {"no policy means nothing is automatic", test_no_policy_means_nothing_is_automatic},
    {"a model cannot create a tool actor", test_a_model_cannot_create_a_tool_actor},
    {"evidence must say what sort of thing it is",
     test_evidence_must_say_what_sort_of_thing_it_is},
    {"a claim is refused rather than truncated", test_a_claim_is_refused_rather_than_truncated},
    {"an existing record with no claims is unverified",
     test_an_existing_record_with_no_claims_is_unverified},
};

ATLAS_TEST_MAIN("verify_engine", TESTS)

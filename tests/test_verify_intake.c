/* Atlas - A9.2.1: the verification intake write point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A9.2's three insert functions had no caller outside the verification tests,
 * so on a real deployment the ten verification tables stayed empty while the
 * engine that reads them passed everything it had. These tests drive the intake
 * path that closes that gap, and most of them are about what it *refuses*.
 *
 * The security cases are the point of the file:
 *
 *   - a model cannot become a compiler        (forged producer)
 *   - a model cannot become Atlas             (forged channel)
 *   - a retry is not a corroboration          (idempotency)
 *   - three readers of one document are one   (independence)
 *   - a reference to nothing is not evidence  (validation)
 *   - a claim about a tree the repository has left cannot move anything (drift)
 */
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/verify.h"
#include "atlas/verify_ops.h"
#include "atlas/verifypolicy.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#define COMMIT_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define COMMIT_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

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
    id.root = "/tmp/atlas-intake-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-intake-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);

    /* Two ingested commits and a scanned head, so source binding has something
     * to validate against and drift has something to detect. */
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO commits(repo_id, oid, parent_count, subject)"
                           "  VALUES(%lld, '%s', 0, 'a'), (%lld, '%s', 1, 'b');"
                           "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
                           (long long)e->repo_id, COMMIT_A, (long long)e->repo_id, COMMIT_B,
                           COMMIT_A, (long long)e->repo_id),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

static void set_head(env *e, const char *oid, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err, "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
                           oid, (long long)e->repo_id),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

/* One intake operation, built and applied. The channel defaults to MODEL,
 * because that is the caller every security test is about. */
static void op_init(atlas_verify_op *op, atlas_verify_op_kind kind) {
    atlas_verify_op_init(op);
    op->kind = kind;
    op->channel = ATLAS_VERIFY_CHANNEL_MODEL;
}

static atlas_status apply(env *e, atlas_verify_op *op, atlas_verify_intake_result *res,
                          atlas_err *err) {
    atlas_verify_intake_result_init(res);
    return atlas_verify_intake_apply(e->db, op, res, err);
}

/* A claim, by a named model, about COMMIT_A. */
static void make_claim(env *e, const char *text, const char *verifier, const char *vinput,
                       atlas_buf *uid_out, atlas_err *err) {
    atlas_verify_op op;
    op_init(&op, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.domain, "control-flow", err), err);
    T_OK(atlas_buf_set_str(&op.text, text, err), err);
    T_OK(atlas_buf_set_str(&op.actor_name, "claude", err), err);
    T_OK(atlas_buf_set_str(&op.actor_provider, "anthropic", err), err);
    T_OK(atlas_buf_set_str(&op.session_key, "s1", err), err);
    if (verifier != NULL) {
        T_OK(atlas_buf_set_str(&op.verifier, verifier, err), err);
        T_OK(atlas_buf_set_str(&op.verifier_input, vinput, err), err);
    }
    atlas_verify_intake_result res;
    T_OK(apply(e, &op, &res, err), err);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
}

/* --- the happy path -------------------------------------------------------- */

static void test_a_model_can_create_a_claim_and_it_binds_to_a_source_state(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    make_claim(&e, "the serializer emits zero here", NULL, NULL, &uid, &err);
    T_CHECK_MSG(uid.len > 0, "a created claim reports its public id");

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    bool found = false;
    T_OK(atlas_db_verify_claim_find(e.db, atlas_buf_cstr(&uid), &c, &found, &err), &err);
    T_CHECK_MSG(found, "the claim is readable by the id intake reported");
    /* §3/§4: a claim that named no commit is bound to the indexed head rather
     * than left to point at whichever HEAD happens to exist later. */
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&c.basis_commit), COMMIT_A) == 0,
                "a claim with no explicit commit binds to the state Atlas has indexed");
    T_CHECK_MSG(c.created_by_actor_id != 0, "a claim records which actor created it");
    T_CHECK_MSG(c.content_key.len > 0, "a claim carries a deterministic identity");

    /* The actor exists, and it is SELF_DECLARED however it described itself. */
    atlas_verify_actor a;
    atlas_verify_actor_init(&a);
    bool afound = false;
    T_OK(atlas_db_verify_actor_get(e.db, c.created_by_actor_id, &a, &afound, &err), &err);
    T_CHECK_MSG(afound, "the creating actor was recorded");
    T_CHECK_MSG(a.cls == ATLAS_ACTOR_AI_AGENT, "the model channel produces an AI_AGENT actor");
    T_CHECK_MSG(a.identity == ATLAS_ACTOR_IDENTITY_SELF_DECLARED,
                "nothing about a model's own description of itself is authenticated");
    atlas_verify_actor_free(&a);
    atlas_verify_claim_free(&c);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- §33: a model cannot become a compiler --------------------------------- */

static void test_a_model_cannot_submit_evidence_only_atlas_could_have_produced(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "f calls g", NULL, NULL, &claim, &err);

    /* The exact forgery §33 names: `class=COMPILER`, which reads to anybody
     * skimming a UI as "clang proved this". Refused rather than discounted. */
    static const atlas_verify_evidence_class FORGED[] = {
        ATLAS_EVIDENCE_COMPILER,
        ATLAS_EVIDENCE_TEST,
        ATLAS_EVIDENCE_RUNTIME,
        ATLAS_EVIDENCE_DEPLOYED_CONFIG,
    };
    for (size_t i = 0; i < sizeof FORGED / sizeof FORGED[0]; i++) {
        atlas_verify_op op;
        op_init(&op, ATLAS_VERIFY_OP_EVIDENCE_ADD);
        T_OK(atlas_buf_set(&op.claim_uid, claim.data, claim.len, &err), &err);
        op.evidence_class = FORGED[i];
        atlas_verify_intake_result res;
        atlas_status st = apply(&e, &op, &res, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a model may not submit evidence Atlas alone could produce");
        atlas_verify_intake_result_free(&res);
        atlas_verify_op_free(&op);
        T_CHECK_MSG(atlas_verify_evidence_class_requires_atlas_production(FORGED[i]),
                    "the rule is asked of the vocabulary, not restated in the test");
    }

    /* And nothing landed. A refusal that still wrote the row would be worse
     * than no refusal, because the row would read as tool output. */
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h,
                               "SELECT COUNT(*) FROM verify_evidence WHERE class IN"
                               " ('COMPILER','TEST','RUNTIME','DEPLOYED_CONFIG');",
                               -1, &stmt, NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int(stmt, 0) == 0, "no forged tool evidence was stored");
    sqlite3_finalize(stmt);

    atlas_buf_free(&claim);
    env_close(&e);
}

static void test_no_transport_can_name_the_atlas_channel(void) {
    /* The channel is what decides an actor's identity, so a request able to
     * name it could make its own evidence authentic. `_parse` is what every
     * transport would have to go through. */
    atlas_verify_channel c = ATLAS_VERIFY_CHANNEL_MODEL;
    T_CHECK_MSG(!atlas_verify_channel_parse("ATLAS", &c),
                "no transport may select the channel that makes evidence Atlas-attested");
    T_CHECK_MSG(!atlas_verify_channel_parse("UNKNOWN", &c), "UNKNOWN is not selectable either");
    T_CHECK(atlas_verify_channel_parse("MODEL", &c) && c == ATLAS_VERIFY_CHANNEL_MODEL);
    T_CHECK(atlas_verify_channel_parse("OPERATOR", &c) && c == ATLAS_VERIFY_CHANNEL_OPERATOR);

    /* UNKNOWN is zero and is refused at the write point, so a zeroed operation
     * cannot write. */
    T_CHECK(ATLAS_VERIFY_CHANNEL_UNKNOWN == 0);
    T_CHECK(atlas_verify_channel_actor_class(ATLAS_VERIFY_CHANNEL_UNKNOWN) == ATLAS_ACTOR_UNKNOWN);
    T_CHECK(atlas_verify_channel_actor_class(ATLAS_VERIFY_CHANNEL_MODEL) == ATLAS_ACTOR_AI_AGENT);
    T_CHECK(atlas_verify_channel_actor_identity(ATLAS_VERIFY_CHANNEL_MODEL) ==
            ATLAS_ACTOR_IDENTITY_SELF_DECLARED);
    T_CHECK(atlas_verify_channel_actor_identity(ATLAS_VERIFY_CHANNEL_ATLAS) ==
            ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED);
}

static void test_a_zeroed_operation_cannot_write(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_verify_op op;
    atlas_verify_op_init(&op);
    op.kind = ATLAS_VERIFY_OP_CLAIM_CREATE; /* channel deliberately left zero */
    atlas_verify_intake_result res;
    atlas_status st = apply(&e, &op, &res, &err);
    T_CHECK_MSG(st != ATLAS_OK, "an operation whose channel nobody set must not write");
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
    env_close(&e);
}

/* --- §8: a reference is validated against the index ------------------------ */

static void test_a_reference_to_something_that_is_not_there_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "the header declares it", NULL, NULL, &claim, &err);

    /* A commit Atlas has not ingested. Fail closed: a false refusal is
     * recoverable, a false acceptance is not. */
    atlas_verify_op op;
    op_init(&op, ATLAS_VERIFY_OP_EVIDENCE_ADD);
    T_OK(atlas_buf_set(&op.claim_uid, claim.data, claim.len, &err), &err);
    op.evidence_class = ATLAS_EVIDENCE_SOURCE_CODE;
    T_OK(atlas_buf_set_str(&op.commit_oid, "cccccccccccccccccccccccccccccccccccccccc", &err), &err);
    atlas_verify_intake_result res;
    T_CHECK_MSG(apply(&e, &op, &res, &err) != ATLAS_OK,
                "evidence cannot be bound to a commit Atlas has not indexed");
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);

    /* A path Atlas has not indexed. */
    atlas_verify_op op2;
    op_init(&op2, ATLAS_VERIFY_OP_EVIDENCE_ADD);
    T_OK(atlas_buf_set(&op2.claim_uid, claim.data, claim.len, &err), &err);
    op2.evidence_class = ATLAS_EVIDENCE_SOURCE_CODE;
    T_OK(atlas_buf_set_str(&op2.path_text, "src/nowhere.c", &err), &err);
    atlas_verify_intake_result res2;
    T_CHECK_MSG(apply(&e, &op2, &res2, &err) != ATLAS_OK,
                "a reference to a path that is not there is a reference to nothing");
    atlas_verify_intake_result_free(&res2);
    atlas_verify_op_free(&op2);

    /* An attestation resting on evidence that does not exist. Refused rather
     * than silently recorded as resting on less, which would change what it is
     * worth without saying so. */
    atlas_verify_op op3;
    op_init(&op3, ATLAS_VERIFY_OP_ATTESTATION_ADD);
    T_OK(atlas_buf_set(&op3.claim_uid, claim.data, claim.len, &err), &err);
    op3.verdict = ATLAS_ATTEST_SUPPORT;
    T_OK(atlas_buf_set_str(&op3.evidence_uids, "atlas-ev-nosuchthing", &err), &err);
    atlas_verify_intake_result res3;
    T_CHECK_MSG(apply(&e, &op3, &res3, &err) != ATLAS_OK,
                "an attestation cannot rest on evidence that does not exist");
    atlas_verify_intake_result_free(&res3);
    atlas_verify_op_free(&op3);

    /* A dependency naming nothing. */
    atlas_verify_op op4;
    op_init(&op4, ATLAS_VERIFY_OP_DEPENDENCY_ADD);
    T_OK(atlas_buf_set_str(&op4.derived_uid, "atlas-ev-a", &err), &err);
    T_OK(atlas_buf_set_str(&op4.source_uid, "atlas-ev-b", &err), &err);
    atlas_verify_intake_result res4;
    T_CHECK_MSG(apply(&e, &op4, &res4, &err) != ATLAS_OK,
                "a derivation edge cannot name evidence that does not exist");
    atlas_verify_intake_result_free(&res4);
    atlas_verify_op_free(&op4);

    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- §27/§28: a retry is not a corroboration ------------------------------- */

static void test_a_repeated_submission_resolves_to_the_row_it_already_made(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf first = ATLAS_BUF_INIT;
    make_claim(&e, "one discrete proposition", NULL, NULL, &first, &err);
    atlas_buf second = ATLAS_BUF_INIT;
    make_claim(&e, "one discrete proposition", NULL, NULL, &second, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&first), atlas_buf_cstr(&second)) == 0,
                "the same proposition about the same tree is one claim, not two");

    /* The same text bound to a *different* commit is a different fact and must
     * not merge — §27 names this as the case that must stay separate. */
    atlas_verify_op op;
    op_init(&op, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.domain, "control-flow", &err), &err);
    T_OK(atlas_buf_set_str(&op.text, "one discrete proposition", &err), &err);
    T_OK(atlas_buf_set_str(&op.basis_commit, COMMIT_B, &err), &err);
    atlas_verify_intake_result res;
    T_OK(apply(&e, &op, &res, &err), &err);
    T_CHECK_MSG(!res.duplicate && strcmp(atlas_buf_cstr(&res.uid), atlas_buf_cstr(&first)) != 0,
                "the same text at a different revision is a different claim");
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);

    /* An attestation repeated is one attestation. */
    for (int i = 0; i < 5; i++) {
        atlas_verify_op a;
        op_init(&a, ATLAS_VERIFY_OP_ATTESTATION_ADD);
        T_OK(atlas_buf_set(&a.claim_uid, first.data, first.len, &err), &err);
        T_OK(atlas_buf_set_str(&a.actor_name, "claude", &err), &err);
        T_OK(atlas_buf_set_str(&a.session_key, "s1", &err), &err);
        T_OK(atlas_buf_set_str(&a.method, "read the source", &err), &err);
        a.verdict = ATLAS_ATTEST_SUPPORT;
        atlas_verify_intake_result r;
        T_OK(apply(&e, &a, &r, &err), &err);
        T_CHECK_MSG(i == 0 || r.duplicate, "an actor repeating itself is one attestation");
        atlas_verify_intake_result_free(&r);
        atlas_verify_op_free(&a);
    }
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h, "SELECT COUNT(*) FROM verify_attestations;",
                               -1, &stmt, NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int(stmt, 0) == 1, "five identical submissions are one row");
    sqlite3_finalize(stmt);

    atlas_buf_free(&first);
    atlas_buf_free(&second);
    env_close(&e);
}

static void test_a_change_of_mind_is_a_new_attestation_not_a_suppressed_one(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "it holds", NULL, NULL, &claim, &err);

    static const atlas_verify_verdict VERDICTS[] = {ATLAS_ATTEST_SUPPORT, ATLAS_ATTEST_CONTRADICT};
    for (size_t i = 0; i < 2; i++) {
        atlas_verify_op a;
        op_init(&a, ATLAS_VERIFY_OP_ATTESTATION_ADD);
        T_OK(atlas_buf_set(&a.claim_uid, claim.data, claim.len, &err), &err);
        T_OK(atlas_buf_set_str(&a.actor_name, "sol", &err), &err);
        a.verdict = VERDICTS[i];
        atlas_verify_intake_result r;
        T_OK(apply(&e, &a, &r, &err), &err);
        T_CHECK_MSG(!r.duplicate,
                    "replay protection must not hide a source reversing itself, which is exactly "
                    "the fact a reliability system most needs to see");
        atlas_verify_intake_result_free(&r);
        atlas_verify_op_free(&a);
    }
    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- §12/§40: three actors, one evidence root ------------------------------ */

static void test_three_models_reading_one_document_are_one_independent_group(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "the protocol freezes the committee snapshot", NULL, NULL, &claim, &err);

    /* One document, referenced once. */
    atlas_verify_op ev;
    op_init(&ev, ATLAS_VERIFY_OP_EVIDENCE_ADD);
    T_OK(atlas_buf_set(&ev.claim_uid, claim.data, claim.len, &err), &err);
    ev.evidence_class = ATLAS_EVIDENCE_DOCUMENT;
    T_OK(atlas_buf_set_str(&ev.target, "old.md", &err), &err);
    atlas_verify_intake_result evres;
    T_OK(apply(&e, &ev, &evres, &err), &err);
    atlas_buf root = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&root, evres.uid.data, evres.uid.len, &err), &err);
    atlas_verify_intake_result_free(&evres);
    atlas_verify_op_free(&ev);

    /* Three different actors, each declaring it read that one document. */
    static const char *const ACTORS[] = {"claude", "sol", "fable"};
    for (size_t i = 0; i < 3; i++) {
        atlas_verify_op a;
        op_init(&a, ATLAS_VERIFY_OP_ATTESTATION_ADD);
        T_OK(atlas_buf_set(&a.claim_uid, claim.data, claim.len, &err), &err);
        T_OK(atlas_buf_set_str(&a.actor_name, ACTORS[i], &err), &err);
        T_OK(atlas_buf_set(&a.evidence_uids, root.data, root.len, &err), &err);
        a.verdict = ATLAS_ATTEST_SUPPORT;
        atlas_verify_intake_result r;
        T_OK(apply(&e, &a, &r, &err), &err);
        T_CHECK_MSG(!r.duplicate, "three different actors are three attestations");
        atlas_verify_intake_result_free(&r);
        atlas_verify_op_free(&a);
    }

    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    bool found = false;
    T_OK(atlas_db_verify_claim_find(e.db, atlas_buf_cstr(&claim), &c, &found, &err), &err);
    atlas_verify_assessment a;
    T_OK(atlas_verify_assess(e.db, &p, c.id, &a, &err), &err);

    T_CHECK_MSG(a.aggregate.support_count == 3, "three actors did speak");
    /* The whole point: three attestations over one root are one independent
     * group. Counting them as three is the mistake this phase exists to make
     * hard to commit. */
    T_CHECK_MSG(a.aggregate.independent_groups == 1,
                "three models reading one document are one independent source");
    atlas_verify_claim_free(&c);
    atlas_buf_free(&root);
    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- §5: SOURCE_DRIFT ------------------------------------------------------ */

static bool has_reason(const atlas_verify_assessment *a, atlas_verify_reason want) {
    for (size_t i = 0; i < a->aggregate.reason_count; i++) {
        if (a->aggregate.reasons[i] == want) {
            return true;
        }
    }
    return false;
}

static void test_a_claim_whose_tree_the_repository_has_left_cannot_move_anything(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "bound to A", NULL, NULL, &claim, &err);

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    bool found = false;
    T_OK(atlas_db_verify_claim_find(e.db, atlas_buf_cstr(&claim), &c, &found, &err), &err);

    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);

    /* Before: the claim and the index agree. */
    atlas_verify_assessment before;
    T_OK(atlas_verify_assess(e.db, &p, c.id, &before, &err), &err);
    T_CHECK_MSG(!before.source_drift, "a claim bound to the indexed head has not drifted");
    T_CHECK_MSG(strcmp(before.claim_commit, COMMIT_A) == 0,
                "the assessment records what the claim was of");
    T_CHECK_MSG(strcmp(before.evaluated_commit, COMMIT_A) == 0,
                "and what the repository was at when it ran");
    T_CHECK_MSG(!has_reason(&before, ATLAS_VREASON_SOURCE_DRIFT), "no drift reason yet");

    /* The repository moves underneath it — the O10 failure, reproduced. */
    set_head(&e, COMMIT_B, &err);

    atlas_verify_assessment after;
    T_OK(atlas_verify_assess(e.db, &p, c.id, &after, &err), &err);
    T_CHECK_MSG(after.source_drift, "the ground moved and Atlas noticed");
    T_CHECK_MSG(strcmp(after.claim_commit, COMMIT_A) == 0,
                "the result stays bound to the snapshot it examined");
    T_CHECK_MSG(strcmp(after.evaluated_commit, COMMIT_B) == 0,
                "and says what the repository had moved to");
    T_CHECK_MSG(has_reason(&after, ATLAS_VREASON_SOURCE_DRIFT), "and says so in the reasons");
    /* The enforcement, which is the part that matters: the reason carries
     * BLOCKED in REASONS[] and the fold takes the weakest, so no machine
     * transition is reachable. */
    T_CHECK_MSG(atlas_verify_reason_verdict(ATLAS_VREASON_SOURCE_DRIFT) == ATLAS_POLICY_BLOCKED,
                "drift blocks rather than merely annotating");
    T_CHECK_MSG(after.aggregate.verdict == ATLAS_POLICY_BLOCKED,
                "a drifting claim reports BLOCKED however good its evidence was");
    T_CHECK_MSG(!after.actionable, "and cannot be acted on");

    atlas_verify_claim_free(&c);
    atlas_buf_free(&claim);
    env_close(&e);
}

static void test_a_drifting_claim_is_never_transitioned(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "absent", "atlas.symbol_absent", "symbol=gone", &claim, &err);
    set_head(&e, COMMIT_B, &err);

    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    bool found = false;
    T_OK(atlas_db_verify_claim_find(e.db, atlas_buf_cstr(&claim), &c, &found, &err), &err);

    atlas_verifypolicy p;
    atlas_verifypolicy_load(&p);
    atlas_verify_assessment a;
    atlas_status st = atlas_verify_autolifecycle_run(e.db, &p, c.id, "proj", &a, &err);
    T_OK(st, &err);
    T_CHECK_MSG(!a.transitioned, "a drifting claim must never move a lifecycle state");

    /* The result is still published, and it records the drift durably so a
     * reader years later can see what it was of. */
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h,
                               "SELECT source_drift, claim_commit, evaluated_commit"
                               " FROM verify_results ORDER BY id DESC LIMIT 1;",
                               -1, &stmt, NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int(stmt, 0) == 1, "the stored result records the drift");
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 1), COMMIT_A) == 0,
                "and what it was of");
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 2), COMMIT_B) == 0,
                "and what the repository had become");
    sqlite3_finalize(stmt);

    atlas_verify_claim_free(&c);
    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- §9: Atlas-produced evidence is the only authentic kind ---------------- */

static void test_atlas_produced_evidence_carries_an_atlas_attested_actor(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "no symbol named gone exists", "atlas.symbol_absent", "symbol=gone", &claim,
               &err);

    atlas_verify_op op;
    op_init(&op, ATLAS_VERIFY_OP_EVIDENCE_PRODUCE);
    T_OK(atlas_buf_set(&op.claim_uid, claim.data, claim.len, &err), &err);
    atlas_verify_intake_result res;
    T_OK(apply(&e, &op, &res, &err), &err);

    /* The caller named a verifier and got whatever it concluded. With no
     * semantic index, that is UNAVAILABLE — and UNAVAILABLE is not FAIL: an
     * index that has not run cannot establish an absence. */
    T_CHECK_MSG(res.check == ATLAS_CHECK_UNAVAILABLE,
                "with no semantic generation, Atlas reports that it could not look");

    atlas_verify_actor a;
    atlas_verify_actor_init(&a);
    bool found = false;
    T_OK(atlas_db_verify_actor_get(e.db, res.actor_id, &a, &found, &err), &err);
    T_CHECK_MSG(found, "Atlas recorded itself as the producer");
    T_CHECK_MSG(a.identity == ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
                "evidence Atlas produced carries an Atlas-attested actor");
    T_CHECK_MSG(a.cls == ATLAS_ACTOR_ATLAS_VERIFIER, "and the verifier actor class");

    atlas_verify_actor_free(&a);
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- §6/§3: bounds refuse rather than truncate ----------------------------- */

static void test_bounds_refuse_rather_than_shorten(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* A shortened proposition is a different proposition, and one whose scope
     * has quietly widened. */
    atlas_buf big = ATLAS_BUF_INIT;
    for (size_t i = 0; i < ATLAS_VERIFY_CLAIM_TEXT_MAX + 16u; i++) {
        T_OK(atlas_buf_append(&big, "x", 1, &err), &err);
    }
    atlas_verify_op op;
    op_init(&op, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op.text, big.data, big.len, &err), &err);
    atlas_verify_intake_result res;
    T_CHECK_MSG(apply(&e, &op, &res, &err) != ATLAS_OK, "an over-long claim is refused, not cut");
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
    atlas_buf_free(&big);

    /* A domain keys reliability, so it is checked rather than escaped. */
    atlas_verify_op op2;
    op_init(&op2, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&op2.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op2.text, "fine", &err), &err);
    T_OK(atlas_buf_set_str(&op2.domain, "Not A Domain\n", &err), &err);
    atlas_verify_intake_result res2;
    T_CHECK_MSG(apply(&e, &op2, &res2, &err) != ATLAS_OK,
                "a domain that is not the shape it claims to be is refused, never reproduced");
    atlas_verify_intake_result_free(&res2);
    atlas_verify_op_free(&op2);

    /* A verifier nobody runs would read as mechanically checkable. */
    atlas_verify_op op3;
    op_init(&op3, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&op3.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op3.text, "fine", &err), &err);
    T_OK(atlas_buf_set_str(&op3.verifier, "atlas.trust_me", &err), &err);
    atlas_verify_intake_result res3;
    T_CHECK_MSG(apply(&e, &op3, &res3, &err) != ATLAS_OK,
                "a claim cannot name its own checker; the allowlist is fixed");
    atlas_verify_intake_result_free(&res3);
    atlas_verify_op_free(&op3);

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a model can create a claim and it binds to a source state",
     test_a_model_can_create_a_claim_and_it_binds_to_a_source_state},
    {"a model cannot submit evidence only Atlas could have produced",
     test_a_model_cannot_submit_evidence_only_atlas_could_have_produced},
    {"no transport can name the Atlas channel", test_no_transport_can_name_the_atlas_channel},
    {"a zeroed operation cannot write", test_a_zeroed_operation_cannot_write},
    {"a reference to something that is not there is refused",
     test_a_reference_to_something_that_is_not_there_is_refused},
    {"a repeated submission resolves to the row it already made",
     test_a_repeated_submission_resolves_to_the_row_it_already_made},
    {"a change of mind is a new attestation, not a suppressed one",
     test_a_change_of_mind_is_a_new_attestation_not_a_suppressed_one},
    {"three models reading one document are one independent group",
     test_three_models_reading_one_document_are_one_independent_group},
    {"a claim whose tree the repository has left cannot move anything",
     test_a_claim_whose_tree_the_repository_has_left_cannot_move_anything},
    {"a drifting claim is never transitioned", test_a_drifting_claim_is_never_transitioned},
    {"Atlas-produced evidence carries an Atlas-attested actor",
     test_atlas_produced_evidence_carries_an_atlas_attested_actor},
    {"bounds refuse rather than shorten", test_bounds_refuse_rather_than_shorten},
};

ATLAS_TEST_MAIN("verify_intake", TESTS)

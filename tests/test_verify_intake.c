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

/* A9.2.1/A12.1, T5: a claim naming a knowledge record by `document_uid`, and
 * carrying a deterministic verifier — the one shape whose failure can be
 * implementation drift. */
static void make_bound_claim(env *e, const char *text, const char *document_uid,
                             const char *verifier, const char *vinput, atlas_buf *uid_out,
                             atlas_err *err) {
    atlas_verify_op op;
    op_init(&op, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.domain, "control-flow", err), err);
    T_OK(atlas_buf_set_str(&op.text, text, err), err);
    T_OK(atlas_buf_set_str(&op.actor_name, "claude", err), err);
    T_OK(atlas_buf_set_str(&op.actor_provider, "anthropic", err), err);
    T_OK(atlas_buf_set_str(&op.session_key, "s1", err), err);
    T_OK(atlas_buf_set_str(&op.document_uid, document_uid, err), err);
    T_OK(atlas_buf_set_str(&op.verifier, verifier, err), err);
    T_OK(atlas_buf_set_str(&op.verifier_input, vinput, err), err);
    atlas_verify_intake_result res;
    T_OK(apply(e, &op, &res, err), err);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
}

/* A file in the index with a stated content hash, so `atlas.content_hash` has
 * something to read and something to disagree with. */
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

/* Makes the file index current, which `ATLAS_COVDIM_REPOSITORY_SNAPSHOT` reads.
 * Without this, `atlas.content_hash`'s negative conclusion is demoted to
 * UNAVAILABLE rather than reported as FAIL — A9.2.2's coverage gate, correctly
 * applied to a verifier whose failing answer is a claim of absence. */
static void seed_index_current(env *e, atlas_err *err) {
    T_OK(atlas_db_index_state_ensure(e->db, e->repo_id, err), err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "UPDATE repo_index_state SET generation = 1, last_complete_generation = 1,"
                           "  watch_state = 'watching', event_gap = 0, pending_full_reconcile = 0"
                           " WHERE repo_id = %lld;",
                           (long long)e->repo_id),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

/* A DECISION-kind knowledge record, proposed and then approved through the
 * real operator channel — challenge, then spend — so its current revision is
 * genuinely APPROVED and genuinely effective. */
static void propose_decision(env *e, const char *title, atlas_buf *uid_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    op.knowledge_kind = ATLAS_DECISION_KIND_DECISION;
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

static void approve_decision(env *e, const char *uid, atlas_err *err) {
    atlas_decision_op cop;
    atlas_decision_op_init(&cop, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&cop.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&cop.uid, uid, err), err);
    cop.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cres;
    atlas_decision_result_init(&cres);
    T_OK(atlas_decision_apply(e->db, &cop, &cres, err), err);
    atlas_buf token = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&token, cres.token.data, cres.token.len, err), err);
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    (void)snprintf(confirm, sizeof confirm, "%s", cres.confirm);
    atlas_decision_op_free(&cop);
    atlas_decision_result_free(&cres);

    atlas_decision_op aop;
    atlas_decision_op_init(&aop, ATLAS_DECISION_OP_APPROVE);
    T_OK(atlas_buf_set_str(&aop.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&aop.uid, uid, err), err);
    T_OK(atlas_buf_set(&aop.token, token.data, token.len, err), err);
    T_OK(atlas_buf_set_str(&aop.confirmation, confirm, err), err);
    atlas_decision_result ares;
    atlas_decision_result_init(&ares);
    T_OK(atlas_decision_apply(e->db, &aop, &ares, err), err);
    T_CHECK(ares.state == ATLAS_DECISION_APPROVED);
    atlas_decision_op_free(&aop);
    atlas_decision_result_free(&ares);
    atlas_buf_free(&token);
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
    T_CHECK_MSG(!atlas_verify_channel_parse("DOCUMENT", &c),
                "no transport may select the channel that mints one speaker per pasted file");
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

/* --- A12.1: transport-selectability is the predicate, not a comment -------- */

static void test_the_selectable_set_is_the_predicate_and_the_parse_cannot_drift(void) {
    /* The whole vocabulary. A member added to the enum without joining this
     * walk still fails the predicate's own switch at compile time, so the walk
     * cannot silently narrow. */
    static const atlas_verify_channel ALL[] = {
        ATLAS_VERIFY_CHANNEL_UNKNOWN, ATLAS_VERIFY_CHANNEL_MODEL,
        ATLAS_VERIFY_CHANNEL_OPERATOR, ATLAS_VERIFY_CHANNEL_ATLAS,
        ATLAS_VERIFY_CHANNEL_DOCUMENT,
    };
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        atlas_verify_channel c = ALL[i];
        bool want = c == ATLAS_VERIFY_CHANNEL_MODEL || c == ATLAS_VERIFY_CHANNEL_OPERATOR;
        T_CHECK_MSG(atlas_verify_channel_is_transport_selectable(c) == want,
                    "%s must%s be transport-selectable", atlas_verify_channel_name(c),
                    want ? "" : " not");

        /* The accept-list and the predicate are one definition: `_parse`
         * accepts a channel's own spelling exactly when the predicate accepts
         * the channel. */
        atlas_verify_channel got = ATLAS_VERIFY_CHANNEL_UNKNOWN;
        bool parsed = atlas_verify_channel_parse(atlas_verify_channel_name(c), &got);
        T_CHECK_MSG(parsed == want, "the parse and the predicate disagree about %s",
                    atlas_verify_channel_name(c));
        if (parsed) {
            T_CHECK(got == c);
        }
    }
}

/* --- A12.1: a document Atlas read can speak, and only Atlas can let it ----- */

static void test_the_document_channel_derives_a_document_actor_that_only_lowers(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "the build needs no network", NULL, NULL, &claim, &err);

    /* Built directly, as internal code does — the `intake.c` synthetic-op
     * shape. No transport can construct this op, which the parse-refusal tests
     * above and the four-case test below are about. */
    atlas_verify_op a;
    op_init(&a, ATLAS_VERIFY_OP_ATTESTATION_ADD);
    a.channel = ATLAS_VERIFY_CHANNEL_DOCUMENT;
    T_OK(atlas_buf_set(&a.claim_uid, claim.data, claim.len, &err), &err);
    T_OK(atlas_buf_set_str(&a.actor_name, "m0123456789abcdef0123456789abcdef", &err), &err);
    T_OK(atlas_buf_set_str(&a.actor_provider, "memory", &err), &err);
    a.verdict = ATLAS_ATTEST_SUPPORT;
    atlas_verify_intake_result res;
    T_OK(apply(&e, &a, &res, &err), &err);

    atlas_verify_actor actor;
    atlas_verify_actor_init(&actor);
    bool found = false;
    T_OK(atlas_db_verify_actor_get(e.db, res.actor_id, &actor, &found, &err), &err);
    T_CHECK_MSG(found, "the document actor was recorded");
    T_CHECK_MSG(actor.cls == ATLAS_ACTOR_DOCUMENT,
                "the DOCUMENT channel produces a DOCUMENT actor and nothing stronger");
    T_CHECK_MSG(actor.identity == ATLAS_ACTOR_IDENTITY_SELF_DECLARED,
                "prose in a file is asserted by whoever wrote the file; nothing authenticates "
                "the writer");
    T_CHECK(atlas_verify_channel_actor_class(ATLAS_VERIFY_CHANNEL_DOCUMENT) ==
            ATLAS_ACTOR_DOCUMENT);
    T_CHECK(atlas_verify_channel_actor_identity(ATLAS_VERIFY_CHANNEL_DOCUMENT) ==
            ATLAS_ACTOR_IDENTITY_SELF_DECLARED);

    /* The number, stated so a later change to the prior table must change this
     * test deliberately. 350 is min(DOCUMENT 400, SELF_DECLARED 350): at
     * ATLAS_ATTESTED a memory file would weigh 400, above the self-declared
     * model that wrote it, and a sentence anybody types into a memory file
     * would outweigh the model speaking directly. */
    T_CHECK_MSG(atlas_verify_prior_reliability(ATLAS_ACTOR_DOCUMENT,
                                               ATLAS_ACTOR_IDENTITY_SELF_DECLARED) == 350,
                "a memory file's prior is capped by its self-declared identity");

    atlas_verify_actor_free(&actor);
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&a);
    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- A12.1: the four-case negative — the peer's own channel always stands -- */

/* `speaking_for: "DOCUMENT"` and `"ATLAS"`, from a model peer and from an
 * operator peer, must leave the peer's own channel standing in all four cases.
 *
 * The operator half cannot be driven through a live socket by any test in this
 * suite, on any machine: `atlas_server_peer_is_operator` probes the root-owned
 * authority policy, and the probe refuses a binary the running uid can replace
 * — so every peer of a fixture daemon resolves to MODEL, deterministically
 * (`tests/test_verify_product.c`'s §10 test states the same). So this test
 * drives the exact composition `channel_for` (src/ipc/server_verify.c:81-93)
 * makes of the two public functions — the parse, then the strict authority
 * comparison — and pushes the resolved channel through the real write point,
 * reading the stored actor class back. The model-peer half is additionally
 * driven through a live daemon socket in `tests/test_verify_product.c`. The
 * residual is stated rather than solved: an edit confined to `channel_for`
 * itself that misbehaves only for an operator peer is unobservable by any
 * unprivileged test. */
static void test_speaking_for_an_internal_channel_leaves_the_peer_standing(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "the four-case negative", NULL, NULL, &claim, &err);

    static const struct {
        atlas_verify_channel peer;
        atlas_verify_actor_class cls;
        atlas_verify_actor_identity identity;
        const char *actor;
    } PEERS[] = {
        {ATLAS_VERIFY_CHANNEL_MODEL, ATLAS_ACTOR_AI_AGENT, ATLAS_ACTOR_IDENTITY_SELF_DECLARED,
         "neg-model"},
        {ATLAS_VERIFY_CHANNEL_OPERATOR, ATLAS_ACTOR_HUMAN,
         ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED, "neg-operator"},
    };
    static const char *const ASKED[] = {"DOCUMENT", "ATLAS"};

    for (size_t p = 0; p < sizeof PEERS / sizeof PEERS[0]; p++) {
        for (size_t s = 0; s < sizeof ASKED / sizeof ASKED[0]; s++) {
            /* `channel_for`'s composition, transcribed: the name is honoured
             * only when it parses and asserts strictly less than the peer. */
            atlas_verify_channel want = ATLAS_VERIFY_CHANNEL_UNKNOWN;
            atlas_verify_channel got = PEERS[p].peer;
            if (atlas_verify_channel_parse(ASKED[s], &want) &&
                atlas_verify_channel_authority(want) <
                    atlas_verify_channel_authority(PEERS[p].peer)) {
                got = want;
            }
            T_CHECK_MSG(got == PEERS[p].peer,
                        "speaking_for %s from a %s peer must leave the peer standing", ASKED[s],
                        atlas_verify_channel_name(PEERS[p].peer));

            /* And through the real write point: the stored actor class is the
             * peer's own, never DOCUMENT and never ATLAS_VERIFIER. */
            atlas_verify_op a;
            op_init(&a, ATLAS_VERIFY_OP_ATTESTATION_ADD);
            a.channel = got;
            T_OK(atlas_buf_set(&a.claim_uid, claim.data, claim.len, &err), &err);
            T_OK(atlas_buf_set_str(&a.actor_name, PEERS[p].actor, &err), &err);
            T_OK(atlas_buf_set_str(&a.session_key, ASKED[s], &err), &err);
            a.verdict = ATLAS_ATTEST_SUPPORT;
            atlas_verify_intake_result res;
            T_OK(apply(&e, &a, &res, &err), &err);
            atlas_verify_actor actor;
            atlas_verify_actor_init(&actor);
            bool found = false;
            T_OK(atlas_db_verify_actor_get(e.db, res.actor_id, &actor, &found, &err), &err);
            T_CHECK_MSG(found && actor.cls == PEERS[p].cls &&
                            actor.identity == PEERS[p].identity,
                        "the stored actor must be the peer's own, never the named channel's");
            T_CHECK(actor.cls != ATLAS_ACTOR_DOCUMENT && actor.cls != ATLAS_ACTOR_ATLAS_VERIFIER);
            atlas_verify_actor_free(&actor);
            atlas_verify_intake_result_free(&res);
            atlas_verify_op_free(&a);
        }
    }

    /* The sharp fact, asserted directly rather than resting on the refusal
     * alone: DOCUMENT ranks *below* OPERATOR, so for an operator peer the
     * strict `<` would admit `speaking_for: "DOCUMENT"` as a legitimate
     * weakening the moment the parse accepted the name. The rank is not a
     * second guard there — the parse refusal is the whole of it, which is why
     * the refusal lives in a predicate the compiler enforces. */
    T_CHECK(atlas_verify_channel_authority(ATLAS_VERIFY_CHANNEL_DOCUMENT) <
            atlas_verify_channel_authority(ATLAS_VERIFY_CHANNEL_OPERATOR));
    atlas_verify_channel c = ATLAS_VERIFY_CHANNEL_UNKNOWN;
    T_CHECK(!atlas_verify_channel_parse("DOCUMENT", &c));

    atlas_buf_free(&claim);
    env_close(&e);
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

/* --- A12.1: a memory snapshot reference is internal, and Atlas binds it ---- */

#define MEM_VERSION_UID "v0123456789abcdef0123456789abcdef"
#define MEM_SHA "1111111111111111111111111111111111111111111111111111111111111111"

/* One registered external memory source with one stored version, inserted the
 * way the fixture inserts commits: directly, because this suite is about the
 * write point and not about the pass that will populate these tables (T8). */
static void insert_memory_version(env *e, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(
             &sql, err,
             "INSERT INTO memory_sources(repo_id, source_uid, cls, path_raw, path_text,"
             "  registered_at) VALUES(%lld, 'm0123456789abcdef0123456789abcdef',"
             "  'EXTERNAL_FILE', '/home/u/notes.md', '/home/u/notes.md', 't0');"
             "INSERT INTO memory_source_versions(source_id, version_uid, commit_oid, blob_oid,"
             "  content_sha256, content_bytes, content, observed_at, recorded_at, read_by_uid)"
             " VALUES(last_insert_rowid(), '%s', '', '', '%s', 5, X'6e6f746573', 't1', 't1', 0);",
             (long long)e->repo_id, MEM_VERSION_UID, MEM_SHA),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

static void mem_evidence_op(atlas_verify_op *op, const atlas_buf *claim, const char *version_uid,
                            atlas_err *err) {
    op_init(op, ATLAS_VERIFY_OP_EVIDENCE_ADD);
    op->evidence_class = ATLAS_EVIDENCE_DOCUMENT;
    T_OK(atlas_buf_set(&op->claim_uid, claim->data, claim->len, err), err);
    T_OK(atlas_buf_set_str(&op->memory_version_uid, version_uid, err), err);
}

static void test_a_memory_snapshot_reference_is_refused_on_every_transport_channel(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "the notes say so", NULL, NULL, &claim, &err);
    insert_memory_version(&e, &err);

    /* Refused on both transport-selectable channels with the pinned sentence,
     * so no transport gains a byte of new surface — even naming a row that
     * exists. */
    static const atlas_verify_channel TRANSPORT[] = {ATLAS_VERIFY_CHANNEL_MODEL,
                                                     ATLAS_VERIFY_CHANNEL_OPERATOR};
    for (size_t i = 0; i < sizeof TRANSPORT / sizeof TRANSPORT[0]; i++) {
        atlas_verify_op op;
        mem_evidence_op(&op, &claim, MEM_VERSION_UID, &err);
        op.channel = TRANSPORT[i];
        atlas_verify_intake_result res;
        atlas_status st = apply(&e, &op, &res, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a %s-channel caller may not name a memory snapshot",
                    atlas_verify_channel_name(TRANSPORT[i]));
        T_CHECK_MSG(strstr(err.msg, "a memory snapshot is bound by the pass that read it") != NULL,
                    "the refusal names the rule; it said: %s", err.msg);
        atlas_verify_intake_result_free(&res);
        atlas_verify_op_free(&op);
    }

    /* A reference to a version that is not there is a reference to nothing,
     * exactly as a path or a commit is. */
    atlas_verify_op op;
    mem_evidence_op(&op, &claim, "vffffffffffffffffffffffffffffffff", &err);
    op.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    atlas_verify_intake_result res;
    T_CHECK_MSG(apply(&e, &op, &res, &err) != ATLAS_OK,
                "a memory version that does not exist cannot be referred to");
    T_CHECK_MSG(strstr(err.msg, "no memory source version by that id exists") != NULL,
                "the refusal says what was missing; it said: %s", err.msg);
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);

    /* And nothing landed. */
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h, "SELECT COUNT(*) FROM verify_evidence;", -1, &stmt,
                               NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int(stmt, 0) == 0, "no refused reference stored a row");
    sqlite3_finalize(stmt);

    atlas_buf_free(&claim);
    env_close(&e);
}

static void test_a_memory_snapshot_reference_takes_the_hash_from_atlas_own_row(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_claim(&e, "the notes say so", NULL, NULL, &claim, &err);
    insert_memory_version(&e, &err);

    /* On the internal channel the reference resolves, and every fact on the
     * stored evidence is Atlas' own: the hash from the version row, the path
     * from the source row, the commit from the claim's binding because this
     * version recorded none. The op has no content-hash field to lie in —
     * deliberately — so what is asserted is equality with the fixture row. */
    atlas_verify_op op;
    mem_evidence_op(&op, &claim, MEM_VERSION_UID, &err);
    op.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    atlas_verify_intake_result res;
    T_OK(apply(&e, &op, &res, &err), &err);
    T_CHECK_MSG(!res.duplicate, "the first reference is a new row");
    atlas_verify_op_free(&op);

    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h,
                               "SELECT content_hash, path_text, commit_oid FROM verify_evidence"
                               " ORDER BY id DESC LIMIT 1;",
                               -1, &stmt, NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 0), MEM_SHA) == 0,
                "the content hash is the one Atlas' own row holds");
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 1), "/home/u/notes.md") == 0,
                "the path is the source's stored path_text");
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 2), COMMIT_A) == 0,
                "an unbound version leaves the claim's own commit binding standing");
    sqlite3_finalize(stmt);

    /* §27: the same snapshot reference twice is one row. */
    atlas_verify_op again;
    mem_evidence_op(&again, &claim, MEM_VERSION_UID, &err);
    again.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    atlas_verify_intake_result res2;
    T_OK(apply(&e, &again, &res2, &err), &err);
    T_CHECK_MSG(res2.duplicate, "a repeated snapshot reference resolves to the row it made");
    T_CHECK_MSG(res2.evidence_id == res.evidence_id, "and to the same row");
    atlas_verify_intake_result_free(&res2);
    atlas_verify_op_free(&again);

    T_CHECK(sqlite3_prepare_v2(e.db->h, "SELECT COUNT(*) FROM verify_evidence;", -1, &stmt,
                               NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int(stmt, 0) == 1, "two identical references are one row");
    sqlite3_finalize(stmt);

    atlas_verify_intake_result_free(&res);
    atlas_buf_free(&claim);
    env_close(&e);
}

static void test_a_memory_snapshot_from_another_repository_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* The snapshot lives under "proj". The claim lives under a second
     * registered repository, with its own ingested commit and scanned head. */
    insert_memory_version(&e, &err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-intake-other";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-intake-other/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t other_id = 0;
    T_OK(atlas_db_repo_add(e.db, "other", &id, &other_id, &err), &err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "INSERT INTO commits(repo_id, oid, parent_count, subject)"
                           "  VALUES(%lld, '%s', 0, 'o');"
                           "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
                           (long long)other_id, COMMIT_B, COMMIT_B, (long long)other_id),
         &err);
    T_OK(atlas_db_exec_sql(e.db, atlas_buf_cstr(&sql), &err), &err);
    atlas_buf_free(&sql);

    atlas_verify_op c;
    op_init(&c, ATLAS_VERIFY_OP_CLAIM_CREATE);
    T_OK(atlas_buf_set_str(&c.repo_name, "other", &err), &err);
    T_OK(atlas_buf_set_str(&c.text, "the other tree's claim", &err), &err);
    atlas_verify_intake_result cres;
    T_OK(apply(&e, &c, &cres, &err), &err);
    atlas_buf claim = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&claim, cres.uid.data, cres.uid.len, &err), &err);
    atlas_verify_intake_result_free(&cres);
    atlas_verify_op_free(&c);

    /* Repository A's snapshot named on repository B's claim: refused, with a
     * sentence that says the snapshot belongs elsewhere — the evidence row
     * would otherwise land with B's repo_id and A's hash and path, a
     * cross-repository provenance leak at the one write point. Asserted as
     * the exact sentence, so the observed refusal is the reported one. */
    atlas_verify_op op;
    mem_evidence_op(&op, &claim, MEM_VERSION_UID, &err);
    op.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    atlas_verify_intake_result res;
    T_CHECK_MSG(apply(&e, &op, &res, &err) != ATLAS_OK,
                "another repository's snapshot must not carry this claim's evidence");
    T_CHECK_MSG(strcmp(err.msg,
                       "that memory snapshot belongs to a different repository than this claim, "
                       "so it cannot be what the evidence refers to") == 0,
                "the refusal must say the snapshot belongs elsewhere; it said: %s", err.msg);
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);

    /* And it wrote nothing. */
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h, "SELECT COUNT(*) FROM verify_evidence;", -1, &stmt,
                               NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(sqlite3_column_int(stmt, 0) == 0, "the refused cross-repository reference stored "
                                                  "a row");
    sqlite3_finalize(stmt);

    /* The same uid on the repository that owns it still resolves — the check
     * refuses the mismatch, not the mechanism. */
    atlas_buf own = ATLAS_BUF_INIT;
    make_claim(&e, "the owning tree's claim", NULL, NULL, &own, &err);
    atlas_verify_op ok;
    mem_evidence_op(&ok, &own, MEM_VERSION_UID, &err);
    ok.channel = ATLAS_VERIFY_CHANNEL_ATLAS;
    atlas_verify_intake_result okres;
    T_OK(apply(&e, &ok, &okres, &err), &err);
    atlas_verify_intake_result_free(&okres);
    atlas_verify_op_free(&ok);
    atlas_buf_free(&own);

    atlas_buf_free(&claim);
    env_close(&e);
}

/* --- A12.1, T5: implementation drift, through the write point ------------- */

#define DRIFT_STORED_HASH \
    "1111111111111111111111111111111111111111111111111111111111111111"
#define DRIFT_CLAIMED_HASH \
    "2222222222222222222222222222222222222222222222222222222222222222"

static void test_a_deterministic_fail_against_an_effective_decision_is_implementation_drift(
    void) {
    /* §Decision 4, end to end: `atlas_verify_conflict_settle`'s rule 1. An
     * approved DECISION stands, a claim binds to it by `document_uid` and
     * names `atlas.content_hash`, and the recorded content disagrees with what
     * the claim says it is — a deterministic FAIL produced through
     * EVIDENCE_PRODUCE and folded by EVALUATE. The claim is against the
     * *implementation*, and the approved record must be untouched by it: that
     * is acceptance item 3's actual content, and it is a claim about what does
     * **not** happen. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/parser.c", DRIFT_STORED_HASH, &err);
    seed_index_current(&e, &err);

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    propose_decision(&e, "the parser must keep the reviewed hash", &doc_uid, &err);
    approve_decision(&e, atlas_buf_cstr(&doc_uid), &err);

    int64_t document_id = 0, doc_repo = 0;
    bool doc_found = false;
    T_OK(atlas_db_decision_find_uid(e.db, atlas_buf_cstr(&doc_uid), &document_id, &doc_repo,
                                    &doc_found, &err),
         &err);
    T_REQUIRE_MSG(doc_found, "the proposed and approved record did not resolve by uid");

    /* Captured before EVALUATE runs, so "unchanged" is asserted against the
     * real pre-state rather than assumed from having just approved it. */
    char status_before[24];
    T_OK(atlas_db_decision_document_status(e.db, document_id, status_before, sizeof status_before,
                                           &err),
         &err);
    int64_t revision_before = 0;
    T_OK(atlas_db_decision_approved_revision(e.db, document_id, &revision_before, &err), &err);
    T_REQUIRE_MSG(revision_before > 0, "the approval did not leave an effective revision");
    T_CHECK(strcmp(status_before, "APPROVED") == 0);

    atlas_buf claim = ATLAS_BUF_INIT;
    make_bound_claim(&e, "src/parser.c has the reviewed content", atlas_buf_cstr(&doc_uid),
                     "atlas.content_hash", "path=src/parser.c;sha256=" DRIFT_CLAIMED_HASH, &claim,
                     &err);

    atlas_verify_op produce;
    op_init(&produce, ATLAS_VERIFY_OP_EVIDENCE_PRODUCE);
    T_OK(atlas_buf_set(&produce.claim_uid, claim.data, claim.len, &err), &err);
    atlas_verify_intake_result pres;
    T_OK(apply(&e, &produce, &pres, &err), &err);
    T_CHECK_MSG(pres.check == ATLAS_CHECK_FAIL,
                "the mismatched hash did not produce a deterministic FAIL");
    atlas_verify_intake_result_free(&pres);
    atlas_verify_op_free(&produce);

    atlas_verify_op eval;
    op_init(&eval, ATLAS_VERIFY_OP_EVALUATE);
    T_OK(atlas_buf_set(&eval.claim_uid, claim.data, claim.len, &err), &err);
    atlas_verify_intake_result eres;
    T_OK(apply(&e, &eval, &eres, &err), &err);

    T_CHECK(eres.assessment.aggregate.deterministic_fail);
    T_CHECK_MSG((int)eres.assessment.aggregate.conflict == (int)ATLAS_CONFLICT_IMPLEMENTATION,
                "got conflict %s, want IMPLEMENTATION",
                atlas_verify_conflict_name(eres.assessment.aggregate.conflict));
    T_CHECK_MSG(!eres.assessment.transitioned,
                "a DESCRIPTIVE finding against the implementation moved a lifecycle state");

    /* The stored row, read back rather than trusted from the in-memory
     * assessment: this is "the stored result's conflict", not merely "what the
     * function returned". */
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h,
                               "SELECT conflict, algorithm FROM verify_results"
                               " ORDER BY id DESC LIMIT 1;",
                               -1, &stmt, NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 0), "IMPLEMENTATION") == 0,
                "the stored row does not record IMPLEMENTATION");
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 1), "atlas-reliability-v2") == 0,
                "the stored row's algorithm is not the bumped one");
    sqlite3_finalize(stmt);

    /* **The claim this test exists for.** A finding against the
     * implementation does not falsify the approved record — asserted directly
     * against the database rather than inferred from `!transitioned`, because
     * that flag being false could also mean the write point refused for some
     * unrelated reason. */
    char status_after[24];
    T_OK(atlas_db_decision_document_status(e.db, document_id, status_after, sizeof status_after,
                                           &err),
         &err);
    int64_t revision_after = 0;
    T_OK(atlas_db_decision_approved_revision(e.db, document_id, &revision_after, &err), &err);
    T_CHECK_MSG(strcmp(status_before, status_after) == 0,
                "the decision's status moved: %s -> %s", status_before, status_after);
    T_CHECK_MSG(revision_before == revision_after,
                "the decision's effective revision moved: %lld -> %lld",
                (long long)revision_before, (long long)revision_after);

    atlas_verify_intake_result_free(&eres);
    atlas_verify_op_free(&eval);
    atlas_buf_free(&claim);
    atlas_buf_free(&doc_uid);
    env_close(&e);
}

/* A round-1 review finding on T5: `out->aggregate.conflict` was computed
 * unconditionally, so a claim demoted to `truth = UNKNOWN` / `SOURCE_DRIFT`
 * could still store `conflict = IMPLEMENTATION` beside it — a confident
 * finding about a tree the check did not actually run against. This drives
 * that exact scenario end to end: the same bound claim and mismatched hash as
 * above, except the repository's scanned head moves between claim creation
 * and evaluation, and the stored row must report the demotion on both axes
 * rather than only on `truth`. */
static void test_a_source_drift_demotes_the_conflict_axis_too(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    seed_file(&e, "src/parser.c", DRIFT_STORED_HASH, &err);
    seed_index_current(&e, &err);

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    propose_decision(&e, "the parser must keep the reviewed hash", &doc_uid, &err);
    approve_decision(&e, atlas_buf_cstr(&doc_uid), &err);

    /* Pins this test's own `decision_effective` precondition rather than
     * borrowing the sibling test's above: without this, a regression that
     * left the approval without an effective revision would still pass here,
     * and this test is sound only as a pair with that one. */
    int64_t document_id = 0, doc_repo = 0;
    bool doc_found = false;
    T_OK(atlas_db_decision_find_uid(e.db, atlas_buf_cstr(&doc_uid), &document_id, &doc_repo,
                                    &doc_found, &err),
         &err);
    T_REQUIRE_MSG(doc_found, "the proposed and approved record did not resolve by uid");
    int64_t revision_before = 0;
    T_OK(atlas_db_decision_approved_revision(e.db, document_id, &revision_before, &err), &err);
    T_REQUIRE_MSG(revision_before > 0, "the approval did not leave an effective revision");

    /* Bound while the repository is still at COMMIT_A — env_open's own
     * scanned head — exactly as the non-drifted fixture above. */
    atlas_buf claim = ATLAS_BUF_INIT;
    make_bound_claim(&e, "src/parser.c has the reviewed content", atlas_buf_cstr(&doc_uid),
                     "atlas.content_hash", "path=src/parser.c;sha256=" DRIFT_CLAIMED_HASH, &claim,
                     &err);

    /* The repository moves on. The claim is still bound to COMMIT_A. */
    set_head(&e, COMMIT_B, &err);

    atlas_verify_op produce;
    op_init(&produce, ATLAS_VERIFY_OP_EVIDENCE_PRODUCE);
    T_OK(atlas_buf_set(&produce.claim_uid, claim.data, claim.len, &err), &err);
    atlas_verify_intake_result pres;
    T_OK(apply(&e, &produce, &pres, &err), &err);
    T_CHECK_MSG(pres.check == ATLAS_CHECK_FAIL,
                "the mismatched hash did not produce a deterministic FAIL");
    atlas_verify_intake_result_free(&pres);
    atlas_verify_op_free(&produce);

    atlas_verify_op eval;
    op_init(&eval, ATLAS_VERIFY_OP_EVALUATE);
    T_OK(atlas_buf_set(&eval.claim_uid, claim.data, claim.len, &err), &err);
    atlas_verify_intake_result eres;
    T_OK(apply(&e, &eval, &eres, &err), &err);

    /* The in-memory assessment: the deterministic fail is still real, drift is
     * detected, and the conflict axis must not assert drift over a tree the
     * check did not run against. */
    T_CHECK(eres.assessment.aggregate.deterministic_fail);
    T_CHECK_MSG(eres.assessment.source_drift, "the moved head was not detected as drift");
    T_CHECK_MSG((int)eres.assessment.truth == (int)ATLAS_TRUTH_UNKNOWN,
                "got truth %s, want UNKNOWN", atlas_verify_truth_name(eres.assessment.truth));
    T_CHECK_MSG((int)eres.assessment.truth_reason == (int)ATLAS_TREASON_SOURCE_DRIFT,
                "got truth_reason %s, want SOURCE_DRIFT",
                atlas_verify_truth_reason_name(eres.assessment.truth_reason));
    T_CHECK_MSG((int)eres.assessment.aggregate.conflict == (int)ATLAS_CONFLICT_NONE,
                "got conflict %s, want NONE: a drifted check must not assert drift",
                atlas_verify_conflict_name(eres.assessment.aggregate.conflict));

    /* The stored row, read back: both axes together, so a reader years later
     * cannot see `IMPLEMENTATION` beside `SOURCE_DRIFT` and be misled into
     * thinking Atlas found disagreement it did not establish. */
    sqlite3_stmt *stmt = NULL;
    T_CHECK(sqlite3_prepare_v2(e.db->h,
                               "SELECT conflict, truth, truth_reason, claim_commit,"
                               " evaluated_commit, source_drift FROM verify_results"
                               " ORDER BY id DESC LIMIT 1;",
                               -1, &stmt, NULL) == SQLITE_OK);
    T_CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 0), "NONE") == 0,
                "stored conflict is not NONE: %s", (const char *)sqlite3_column_text(stmt, 0));
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 1), "UNKNOWN") == 0,
                "stored truth is not UNKNOWN: %s", (const char *)sqlite3_column_text(stmt, 1));
    T_CHECK_MSG(strcmp((const char *)sqlite3_column_text(stmt, 2), "SOURCE_DRIFT") == 0,
                "stored truth_reason is not SOURCE_DRIFT: %s",
                (const char *)sqlite3_column_text(stmt, 2));
    T_CHECK(strcmp((const char *)sqlite3_column_text(stmt, 3), COMMIT_A) == 0);
    T_CHECK(strcmp((const char *)sqlite3_column_text(stmt, 4), COMMIT_B) == 0);
    T_CHECK_MSG(sqlite3_column_int(stmt, 5) == 1, "the stored row does not record the drift");
    sqlite3_finalize(stmt);

    atlas_verify_intake_result_free(&eres);
    atlas_verify_op_free(&eval);
    atlas_buf_free(&claim);
    atlas_buf_free(&doc_uid);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a model can create a claim and it binds to a source state",
     test_a_model_can_create_a_claim_and_it_binds_to_a_source_state},
    {"a model cannot submit evidence only Atlas could have produced",
     test_a_model_cannot_submit_evidence_only_atlas_could_have_produced},
    {"no transport can name the Atlas channel", test_no_transport_can_name_the_atlas_channel},
    {"the selectable set is the predicate, and the parse cannot drift",
     test_the_selectable_set_is_the_predicate_and_the_parse_cannot_drift},
    {"the DOCUMENT channel derives a DOCUMENT actor that only lowers",
     test_the_document_channel_derives_a_document_actor_that_only_lowers},
    {"speaking for an internal channel leaves the peer standing",
     test_speaking_for_an_internal_channel_leaves_the_peer_standing},
    {"a memory snapshot reference is refused on every transport channel",
     test_a_memory_snapshot_reference_is_refused_on_every_transport_channel},
    {"a memory snapshot reference takes the hash from Atlas' own row",
     test_a_memory_snapshot_reference_takes_the_hash_from_atlas_own_row},
    {"a memory snapshot from another repository is refused",
     test_a_memory_snapshot_from_another_repository_is_refused},
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
    {"a deterministic fail against an effective decision is implementation drift",
     test_a_deterministic_fail_against_an_effective_decision_is_implementation_drift},
    {"a source drift demotes the conflict axis too",
     test_a_source_drift_demotes_the_conflict_axis_too},
};

ATLAS_TEST_MAIN("verify_intake", TESTS)

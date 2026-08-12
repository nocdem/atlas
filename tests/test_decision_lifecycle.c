/* Atlas - A4: the lifecycle, immutability, the operator channel and its limits.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These drive `atlas_decision_apply` directly against real storage, without a
 * daemon, because the properties being checked are properties of the state
 * machine and the schema rather than of the transport.
 *
 * Every negative test here proves two things, and the second is the one that is
 * easy to forget: that the forbidden transition did not happen, *and* that the
 * neighbouring valid record was left exactly as it was. A refusal that also
 * corrupts something is not a refusal.
 */
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/decision_ops.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- environment ------------------------------------------------------------ */

typedef struct env env;
/* Declared up here because the immutability test needs a rejection and the
 * rejection helper reads more naturally beside the other negative tests. */
static void reject_it(env *e, const char *uid, atlas_err *err);
/* The root commit the fixture's repository is given. The durable repository
 * identity commits to the *lineage* rather than to the path, so the fixture has
 * to have one for any relink behaviour to be exercised at all. */
#define ORIGINAL_ROOT_COMMIT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
static void seed_root_commit(env *e, int64_t repo_id, const char *oid, atlas_err *err);

struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
};

/* The fixture without a lineage: a registered repository whose history has not
 * been ingested, which is what every repository looks like between `repo add`
 * and the first completed pass. */
static void env_open_without_lineage(env *e, atlas_err *err);

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = "/tmp/atlas-decision-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-decision-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
    /* A lineage, so the repository has a durable identity. Without one every
     * document it owns records an empty identity and is permanently
     * unrelinkable — which is the correct fail-closed behaviour, and would make
     * the relink tests vacuous. */
    seed_root_commit(e, e->repo_id, ORIGINAL_ROOT_COMMIT, err);
}

static void env_open_without_lineage(env *e, atlas_err *err) {
    env_open(e, err);
    /* Undo the seeded lineage, so the repository is in the state a freshly
     * registered one is in: known to Atlas, with no ingested history. */
    T_OK(atlas_db_exec_sql(e->db, "DELETE FROM commits;", err), err);
    T_OK(atlas_db_exec_sql(e->db, "UPDATE decision_documents SET repo_identity_hash = '';", err),
         err);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* Records one parentless commit, which is what an ingested history leaves and
 * what the identity is computed from. */
static void seed_root_commit(env *e, int64_t repo_id, const char *oid, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO commits(repo_id, oid, parent_count, subject)"
                           " VALUES(%lld, '%s', 0, 'root');",
                           (long long)repo_id, oid),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

static atlas_status count_docs_cb(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    (void)row;
    (void)err;
    (void)ud;
    return ATLAS_OK;
}

static int64_t count_of(env *e, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(e->db, sql, &n, &err), &err);
    return n;
}

/* --- operation builders ------------------------------------------------------ */

static void op_repo(atlas_decision_op *op, atlas_err *err) {
    T_OK(atlas_buf_set_str(&op->repo_name, "proj", err), err);
}

/* A minimal, valid proposal. */
static void build_proposal(atlas_decision_op *op, const char *title, const char *decision,
                           atlas_err *err) {
    atlas_decision_op_init(op, ATLAS_DECISION_OP_PROPOSE);
    op_repo(op, err);
    T_OK(atlas_buf_set_str(&op->revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op->revision.decision_text, decision, err), err);
    op->revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
}

/* Proposes and returns the new document's uid, revision number and hash. */
static void propose(env *e, const char *title, const char *decision, atlas_buf *uid_out,
                    char *hash_out, atlas_err *err) {
    atlas_decision_op op;
    build_proposal(&op, title, decision, err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_CHECK(res.document_created);
    T_EQ_INT((int)res.revision_no, 1);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    if (hash_out != NULL) {
        memcpy(hash_out, res.content_hash, sizeof(res.content_hash));
    }
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

/* Issues a challenge and fills in the token and the confirmation phrase. */
static void challenge_for(env *e, const char *uid, const char *replacement_uid,
                          int64_t expect_revision_no, atlas_decision_intent intent,
                          atlas_buf *token_out, char *confirm_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_CHALLENGE);
    op_repo(&op, err);
    T_OK(atlas_buf_set_str(&op.uid, uid, err), err);
    if (replacement_uid != NULL) {
        T_OK(atlas_buf_set_str(&op.replacement_uid, replacement_uid, err), err);
    }
    op.expect_revision_no = expect_revision_no;
    op.intent = intent;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_OK(atlas_buf_set(token_out, res.token.data, res.token.len, err), err);
    (void)snprintf(confirm_out, ATLAS_DECISION_CONFIRM_MAX, "%s", res.confirm);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

/* The common case: a capability to approve. */
static void challenge(env *e, const char *uid, const char *replacement_uid,
                      int64_t expect_revision_no, atlas_buf *token_out, char *confirm_out,
                      atlas_err *err) {
    challenge_for(e, uid, replacement_uid,expect_revision_no,
                  replacement_uid != NULL ? ATLAS_DECISION_INTENT_SUPERSEDE
                                          : ATLAS_DECISION_INTENT_APPROVE,
                  token_out, confirm_out, err);
}

/* Spends a capability. Returns the status so negative tests can assert it. */
static atlas_status spend(env *e, atlas_decision_op_kind kind, const char *uid, const char *token,
                          const char *confirm, atlas_decision_result *res, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, kind);
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_buf_set_str(&op.repo_name, "proj", &ignore);
    (void)atlas_buf_set_str(&op.uid, uid, &ignore);
    (void)atlas_buf_set_str(&op.token, token, &ignore);
    (void)atlas_buf_set_str(&op.confirmation, confirm, &ignore);
    atlas_status st = atlas_decision_apply(e->db, &op, res, err);
    atlas_decision_op_free(&op);
    return st;
}

/* The whole approve dance, asserted to succeed. */
static void approve(env *e, const char *uid, int64_t revision_no, atlas_err *err) {
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(e, uid, NULL, revision_no, &token, confirm, err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(e, ATLAS_DECISION_OP_APPROVE, uid, atlas_buf_cstr(&token), confirm, &res, err), err);
    T_CHECK(res.state == ATLAS_DECISION_APPROVED);
    atlas_decision_result_free(&res);
    atlas_buf_free(&token);
}

/* --- reading back ------------------------------------------------------------- */

typedef struct doc_probe {
    char status[16];
    char head_state[16];
    int64_t head_revision_no;
    int64_t latest_revision_no;
    int64_t current_revision_id;
    atlas_buf title;
    atlas_buf superseded_by;
    bool seen;
} doc_probe;

static atlas_status doc_probe_cb(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    doc_probe *p = (doc_probe *)ud;
    (void)snprintf(p->status, sizeof(p->status), "%s", row->status);
    (void)snprintf(p->head_state, sizeof(p->head_state), "%s",
                   row->head_state != NULL ? row->head_state : "");
    p->head_revision_no = row->head_revision_no;
    p->latest_revision_no = row->latest_revision_no;
    p->current_revision_id = row->current_revision_id;
    atlas_status st = atlas_buf_set_str(&p->title, row->title != NULL ? row->title : "", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&p->superseded_by,
                               row->superseded_by_uid != NULL ? row->superseded_by_uid : "", err);
    }
    p->seen = true;
    return st;
}

static void read_doc(env *e, const char *uid, doc_probe *p, atlas_err *err) {
    memset(p, 0, sizeof(*p));
    atlas_buf_init(&p->title);
    atlas_buf_init(&p->superseded_by);
    int64_t id = 0, repo = 0;
    bool found = false;
    T_OK(atlas_db_decision_find_uid(e->db, uid, &id, &repo, &found, err), err);
    T_REQUIRE(found);
    T_OK(atlas_db_decision_document_row(e->db, id, doc_probe_cb, p, &found, err), err);
    T_REQUIRE(p->seen);
}

static void doc_probe_free(doc_probe *p) {
    atlas_buf_free(&p->title);
    atlas_buf_free(&p->superseded_by);
}

/* --- the happy path ----------------------------------------------------------- */

static void test_propose_creates_a_document_and_a_ledger_entry(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    propose(&e, "Use WAL", "Enable WAL journalling on the index.", &uid, hash, &err);

    T_CHECK(atlas_decision_uid_is_valid(atlas_buf_cstr(&uid)));
    T_EQ_INT((int)strlen(hash), (int)ATLAS_SHA256_HEX_LEN);

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK(strcmp(p.status, "PROPOSED") == 0);
    T_EQ_INT((int)p.latest_revision_no, 1);
    T_EQ_INT((int)p.current_revision_id, 0);
    doc_probe_free(&p);

    /* The ledger is canonical, so a proposal is an event and not only a row. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='PROPOSED';"), 1);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE actor='MODEL_PROPOSAL';"),
             1);
    /* And no approval appeared anywhere. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_approval_supersedes_the_predecessor_atomically(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "Original", "Do the first thing.", &uid, NULL, &err);
    approve(&e, atlas_buf_cstr(&uid), 1, &err);

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK(strcmp(p.status, "APPROVED") == 0);
    T_EQ_INT((int)p.head_revision_no, 1);
    doc_probe_free(&p);

    /* Rule 4: changing an approved decision creates a new PROPOSED revision. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "Original", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "Do the second thing instead.", &err),
             &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_EQ_INT((int)res.revision_no, 2);
        T_CHECK(res.state == ATLAS_DECISION_PROPOSED);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    /* Rule 5: revision 1 stays effective until the replacement is approved. */
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "APPROVED") == 0,
                "the approved revision must stay effective while a replacement is only proposed");
    T_EQ_INT((int)p.head_revision_no, 1);
    T_EQ_INT((int)p.latest_revision_no, 2);
    doc_probe_free(&p);

    /* Rule 6: approving the replacement supersedes the predecessor, atomically. */
    approve(&e, atlas_buf_cstr(&uid), 2, &err);

    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK(strcmp(p.status, "APPROVED") == 0);
    T_EQ_INT((int)p.head_revision_no, 2);
    doc_probe_free(&p);

    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM decision_revisions WHERE revision_no=1 AND "
                           "state='SUPERSEDED';"),
             1);
    /* Rule 9, at the schema level: never two effective revisions at once. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions WHERE state='APPROVED';"),
             1);
    /* The supersession is in the ledger, attributed to Atlas rather than to the
     * operator: the operator approved a revision, and the supersession follows
     * mechanically from that. */
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM decision_events WHERE event='SUPERSEDED' AND "
                           "actor='ATLAS_AUTOMATIC';"),
             1);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- immutability -------------------------------------------------------------- */

/* A digest of every content column of every revision, so "nothing was edited"
 * is one comparison rather than a list of columns somebody has to keep current. */
static void content_digest(env *e, char *out) {
    sqlite3_stmt *s = NULL;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_prepare(e->db,
                          "SELECT id, revision_no, content_hash, title, context_text,"
                          "       decision_text, rationale_text, consequences_text, scope,"
                          "       proposed_by, created_at, basis_head"
                          "  FROM decision_revisions ORDER BY id;",
                          &s, &err),
         &err);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    while (sqlite3_step(s) == SQLITE_ROW) {
        for (int c = 0; c < sqlite3_column_count(s); c++) {
            const unsigned char *t = sqlite3_column_text(s, c);
            int n = sqlite3_column_bytes(s, c);
            atlas_sha256_update(&ctx, t != NULL ? (const void *)t : "", n > 0 ? (size_t)n : 0u);
            atlas_sha256_update(&ctx, "\x1f", 1u);
        }
    }
    atlas_db_finish(e->db, s);
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
}


/* A digest of every column of every row of one decision table, in id order.
 * A count would pass a rebuild that rewrote a value; this would not. */
static void decision_table_digest(env *e, const char *table, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[160];
    (void)snprintf(sql, sizeof(sql), "SELECT * FROM %s ORDER BY rowid;", table);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(e->db, sql, &s, &err), &err);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    while (sqlite3_step(s) == SQLITE_ROW) {
        for (int c = 0; c < sqlite3_column_count(s); c++) {
            const void *b = sqlite3_column_blob(s, c);
            int n = sqlite3_column_bytes(s, c);
            atlas_sha256_update(&ctx, b != NULL ? b : "", n > 0 ? (size_t)n : 0u);
            atlas_sha256_update(&ctx, "\x1f", 1u);
        }
        atlas_sha256_update(&ctx, "\x1e", 1u);
    }
    atlas_db_finish(e->db, s);
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
}

static void test_content_is_immutable_across_every_transition(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT;
    propose(&e, "Kept", "This one is approved and then superseded.", &a, NULL, &err);
    propose(&e, "Refused", "This one is rejected.", &b, NULL, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    content_digest(&e, before);

    /* Every transition the state machine has, applied to real rows: an
     * approval, a rejection, and the supersession an approval implies. */
    approve(&e, atlas_buf_cstr(&a), 1, &err);
    reject_it(&e, atlas_buf_cstr(&b), &err);

    char after_first[ATLAS_SHA256_HEX_LEN + 1u];
    content_digest(&e, after_first);
    T_CHECK_MSG(strcmp(before, after_first) == 0,
                "no content column may change across an approval or a rejection");

    /* And across the supersession a second approval causes. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&a), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "Kept", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "A revised wording.", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    char after_revise[ATLAS_SHA256_HEX_LEN + 1u];
    content_digest(&e, after_revise);
    /* The digest covers every revision, so adding one changes it — which is the
     * point: a revision is added, never applied to what is there. */
    T_CHECK(strcmp(after_first, after_revise) != 0);

    approve(&e, atlas_buf_cstr(&a), 2, &err);
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    content_digest(&e, after);
    T_CHECK_MSG(strcmp(after_revise, after) == 0,
                "no content column of any revision may change across a supersession");

    atlas_buf_free(&a);
    atlas_buf_free(&b);
    env_close(&e);
}

/* --- negative: the capability -------------------------------------------------- */

/* Every negative test below asserts the refusal *and* that the record it was
 * aimed at is untouched. */
static void assert_still_proposed(env *e, const char *uid, int64_t revision_no) {
    atlas_err err;
    atlas_err_init(&err);
    doc_probe p;
    read_doc(e, uid, &p, &err);
    T_CHECK_MSG(strcmp(p.status, "PROPOSED") == 0,
                "the document must still be PROPOSED, and is %s", p.status);
    T_EQ_INT((int)p.head_revision_no, (int)revision_no);
    T_EQ_INT((int)p.current_revision_id, 0);
    doc_probe_free(&p);
    T_EQ_INT((int)count_of(e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 0);
    T_EQ_INT((int)count_of(e, "SELECT COUNT(*) FROM decision_revisions WHERE state<>'PROPOSED';"),
             0);
}

static void test_approval_without_a_challenge_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    propose(&e, "T", "D", &uid, hash, &err);

    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    atlas_decision_confirm_phrase(hash, confirm, sizeof(confirm));

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    /* No token at all. This is the shape a malicious MCP argument would take if
     * the method were reachable: correct document, correct confirmation, no
     * capability. */
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), "", confirm, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    /* An invented token. */
    atlas_decision_result_init(&res);
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid),
                       "00000000000000000000000000000000", confirm, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    assert_still_proposed(&e, atlas_buf_cstr(&uid), 1);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_challenge_is_single_use(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT;
    propose(&e, "First", "D1", &a, NULL, &err);
    propose(&e, "Second", "D2", &b, NULL, &err);

    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&a), NULL, 1, &token, confirm, &err);

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&a), atlas_buf_cstr(&token), confirm,
               &res, &err),
         &err);
    atlas_decision_result_free(&res);

    /* Replay. The same capability must not authorise a second transition. */
    atlas_decision_result_init(&res);
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&a), atlas_buf_cstr(&token),
                       confirm, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    /* And the untouched neighbour really is untouched. */
    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&b), &p, &err);
    T_CHECK(strcmp(p.status, "PROPOSED") == 0);
    doc_probe_free(&p);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 1);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;"), 1);

    atlas_buf_free(&token);
    atlas_buf_free(&a);
    atlas_buf_free(&b);
    env_close(&e);
}

static void test_a_challenge_is_bound_to_one_revision_and_one_document(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT;
    char hash_b[ATLAS_SHA256_HEX_LEN + 1u];
    propose(&e, "Target", "D1", &a, NULL, &err);
    propose(&e, "Other", "D2", &b, hash_b, &err);

    /* A capability issued for document A, spent naming document B.
     *
     * The `uid` in the request is not what the transition acts on — the
     * challenge is — so this must not approve B, and it must not approve A
     * either, because the confirmation is B's. */
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm_a[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&a), NULL, 1, &token, confirm_a, &err);

    char confirm_b[ATLAS_DECISION_CONFIRM_MAX];
    atlas_decision_confirm_phrase(hash_b, confirm_b, sizeof(confirm_b));

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&b), atlas_buf_cstr(&token),
                       confirm_b, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&b), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "PROPOSED") == 0, "B must not have been approved");
    doc_probe_free(&p);
    read_doc(&e, atlas_buf_cstr(&a), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "PROPOSED") == 0, "A must not have been approved either");
    doc_probe_free(&p);
    /* And the capability was not spent by a failed attempt. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;"), 0);

    atlas_buf_free(&token);
    atlas_buf_free(&a);
    atlas_buf_free(&b);
    env_close(&e);
}

static void test_a_stale_challenge_does_not_approve_a_newer_revision(void) {
    /* Test 10: a capability issued against revision 1, spent after revision 2
     * exists, must approve revision 1 and only revision 1 — and the operator
     * who confirmed revision 1's hash must not silently be approving revision
     * 2's text. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "The first wording.", &uid, NULL, &err);

    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &token, confirm, &err);

    /* A model revises while the operator is reading the prompt. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "A completely different wording.", &err),
             &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_EQ_INT((int)res.revision_no, 2);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&token), confirm,
               &res, &err),
         &err);
    T_CHECK_MSG((int)(res.revision_no) == 1, "the capability was bound to revision 1 and must approve revision 1");
    atlas_decision_result_free(&res);

    /* Revision 2 is still merely proposed: it was never confirmed by anyone. */
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM decision_revisions WHERE revision_no=2 AND "
                           "state='PROPOSED';"),
             1);
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM decision_revisions WHERE revision_no=1 AND "
                           "state='APPROVED';"),
             1);

    atlas_buf_free(&token);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_wrong_confirmation_changes_nothing(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);

    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &token, confirm, &err);

    /* The shapes a careless or hostile caller would try: an affirmative word, a
     * prefix, an empty string, and the confirmation with a trailing byte. */
    static const char *const wrong[] = {"yes", "y", "", "APPROVE", NULL};
    for (size_t i = 0; wrong[i] != NULL; i++) {
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid),
                           atlas_buf_cstr(&token), wrong[i], &res, &err),
                     ATLAS_ERR_INTEGRITY, &err);
        atlas_decision_result_free(&res);
    }
    {
        char truncated[ATLAS_DECISION_CONFIRM_MAX];
        (void)snprintf(truncated, sizeof(truncated), "%s", confirm);
        truncated[strlen(truncated) - 1u] = '\0';
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid),
                           atlas_buf_cstr(&token), truncated, &res, &err),
                     ATLAS_ERR_INTEGRITY, &err);
        atlas_decision_result_free(&res);
    }

    assert_still_proposed(&e, atlas_buf_cstr(&uid), 1);
    /* A failed confirmation must not burn the capability either: an operator
     * who mistypes should be able to try again rather than start over. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;"), 0);

    /* And the right one still works, which proves the refusals above were about
     * the confirmation rather than about something else being broken. */
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&token), confirm,
               &res, &err),
         &err);
    atlas_decision_result_free(&res);

    atlas_buf_free(&token);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_an_expired_challenge_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);

    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &token, confirm, &err);

    /* Expire it by moving its expiry into the past. Waiting out the real TTL
     * would put two minutes into the suite for a property that is a string
     * comparison; the stored timestamp is what the code reads, so writing it
     * is a faithful simulation of time passing. */
    T_OK(atlas_db_exec_sql(e.db,
                           "UPDATE decision_challenges SET expires_at='1970-01-01T00:00:00Z';",
                           &err),
         &err);

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                       confirm, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    assert_still_proposed(&e, atlas_buf_cstr(&uid), 1);
    atlas_buf_free(&token);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_changed_content_under_a_challenge_is_refused(void) {
    /* Revisions are immutable through Atlas, so this cannot arise from Atlas'
     * own writes. It is checked anyway, because "cannot happen" is a belief and
     * this is a check — and because the row is in a file on disk that other
     * things can open. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);

    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &token, confirm, &err);

    /* Somebody edits the stored decision behind Atlas' back, leaving the
     * recorded hash alone so only a rehash can notice. */
    T_OK(atlas_db_exec_sql(
             e.db, "UPDATE decision_revisions SET decision_text='Something else entirely.';", &err),
         &err);

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                       confirm, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 0);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;"), 0);

    atlas_buf_free(&token);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- negative: the transitions -------------------------------------------------- */

static void reject_it(env *e, const char *uid, atlas_err *err) {
    /* A rejection needs a capability issued with the reject intent: the intent
     * is part of the bound tuple, so an approval capability cannot reject and a
     * rejection capability cannot approve. */
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge_for(e, uid, NULL, 0, ATLAS_DECISION_INTENT_REJECT, &token, confirm, err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(e, ATLAS_DECISION_OP_REJECT, uid, atlas_buf_cstr(&token), confirm, &res, err), err);
    T_CHECK(res.state == ATLAS_DECISION_REJECTED);
    atlas_decision_result_free(&res);
    atlas_buf_free(&token);
}

static void test_a_rejected_revision_can_never_be_approved(void) {
    /* Rule 3, and the single most important refusal in the phase. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);
    reject_it(&e, atlas_buf_cstr(&uid), &err);

    /* A fresh, entirely valid capability against the rejected revision. */
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &token, confirm, &err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_FAILS_WITH(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                       confirm, &res, &err),
                 ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions WHERE state='REJECTED';"),
             1);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions WHERE state='APPROVED';"),
             0);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 0);

    atlas_buf_free(&token);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_second_approval_of_one_revision_loses_deterministically(void) {
    /* Rule 12. Two capabilities issued against one proposed revision: the first
     * spend wins, the second must fail with a typed conflict rather than
     * overwrite. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);

    atlas_buf t1 = ATLAS_BUF_INIT, t2 = ATLAS_BUF_INIT;
    char c1[ATLAS_DECISION_CONFIRM_MAX], c2[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &t1, c1, &err);
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &t2, c2, &err);

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&t1), c1, &res,
               &err),
         &err);
    atlas_decision_result_free(&res);

    atlas_decision_result_init(&res);
    T_FAILS_WITH(
        spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&t2), c2, &res,
              &err),
        ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);

    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 1);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions WHERE state='APPROVED';"),
             1);
    /* The losing capability was rolled back with its transaction, so it is
     * still unspent rather than consumed-without-effect. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;"), 1);

    atlas_buf_free(&t1);
    atlas_buf_free(&t2);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- supersession ----------------------------------------------------------------- */

static void supersede(env *e, const char *old_uid, const char *new_uid, atlas_status expect,
                      atlas_err *err) {
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(e, old_uid, new_uid, 0, &token, confirm, err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    atlas_status st =
        spend(e, ATLAS_DECISION_OP_SUPERSEDE, old_uid, atlas_buf_cstr(&token), confirm, &res, err);
    T_CHECK_MSG(st == expect, "supersede: expected %s, got %s (%s)", atlas_status_name(expect),
                atlas_status_name(st), atlas_err_msg(err));
    atlas_decision_result_free(&res);
    atlas_buf_free(&token);
}

static void test_supersession_and_its_cycle_refusal(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT, c = ATLAS_BUF_INIT;
    propose(&e, "A", "First policy.", &a, NULL, &err);
    propose(&e, "B", "Second policy.", &b, NULL, &err);
    propose(&e, "C", "Third policy.", &c, NULL, &err);
    approve(&e, atlas_buf_cstr(&a), 1, &err);

    supersede(&e, atlas_buf_cstr(&a), atlas_buf_cstr(&b), ATLAS_OK, &err);

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&a), &p, &err);
    T_CHECK(strcmp(p.status, "SUPERSEDED") == 0);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&p.superseded_by), atlas_buf_cstr(&b)) == 0,
                "the superseded document must name its replacement");
    doc_probe_free(&p);
    /* Nothing was deleted: A's revision is still there, marked. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_documents;"), 3);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), 3);

    /* Rule 8: the shortest cycle. */
    supersede(&e, atlas_buf_cstr(&b), atlas_buf_cstr(&b), ATLAS_ERR_INTEGRITY, &err);
    /* A longer one: B -> C, then C -> A would close A -> B -> C -> A. */
    supersede(&e, atlas_buf_cstr(&b), atlas_buf_cstr(&c), ATLAS_OK, &err);
    supersede(&e, atlas_buf_cstr(&c), atlas_buf_cstr(&a), ATLAS_ERR_INTEGRITY, &err);

    /* C is untouched by the refusal. */
    read_doc(&e, atlas_buf_cstr(&c), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "PROPOSED") == 0, "the refused supersession must change nothing");
    T_EQ_INT((int)p.latest_revision_no, 1);
    doc_probe_free(&p);

    atlas_buf_free(&a);
    atlas_buf_free(&b);
    atlas_buf_free(&c);
    env_close(&e);
}

static void test_supersession_cannot_cross_repositories(void) {
    /* Rule 7. Two repositories' decisions are two policies, and a link that
     * crossed would let a supersession in one silently retire a decision in the
     * other. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = "/tmp/atlas-decision-other";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-decision-other/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t other_id = 0;
    T_OK(atlas_db_repo_add(e.db, "other", &id, &other_id, &err), &err);

    atlas_buf mine = ATLAS_BUF_INIT, theirs = ATLAS_BUF_INIT;
    propose(&e, "Mine", "D", &mine, NULL, &err);
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
        T_OK(atlas_buf_set_str(&op.repo_name, "other", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "Theirs", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "D", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_OK(atlas_buf_set(&theirs, res.uid.data, res.uid.len, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    /* Refused at the point the capability is issued, so the operator is told at
     * the prompt rather than after confirming. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_CHALLENGE);
    op_repo(&op, &err);
    T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&mine), &err), &err);
    T_OK(atlas_buf_set_str(&op.replacement_uid, atlas_buf_cstr(&theirs), &err), &err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_FAILS_WITH(atlas_decision_apply(e.db, &op, &res, &err), ATLAS_ERR_USAGE, &err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&mine), &p, &err);
    T_CHECK(strcmp(p.status, "PROPOSED") == 0);
    doc_probe_free(&p);
    read_doc(&e, atlas_buf_cstr(&theirs), &p, &err);
    T_CHECK(strcmp(p.status, "PROPOSED") == 0);
    doc_probe_free(&p);

    atlas_buf_free(&mine);
    atlas_buf_free(&theirs);
    env_close(&e);
}

/* --- idempotency --------------------------------------------------------------- */

static void test_retries_are_idempotent(void) {
    /* Rule 11, in the two shapes a retry actually takes: a redelivered request
     * carrying the same dedup key, and a request that simply says the same
     * thing again. Either producing a second revision would fill a document
     * with copies of itself at hook-retry frequency. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "The wording.", &uid, NULL, &err);

    for (int attempt = 0; attempt < 3; attempt++) {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "A new wording.", &err), &err);
        T_OK(atlas_buf_set_str(&op.dedup_key, "turn-7", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        if (attempt > 0) {
            T_CHECK_MSG(res.duplicate, "retry %d must be absorbed", attempt);
        }
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_revisions;") == 2,
                "three identical revise requests must produce one new revision");
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events;"), 2);

    /* The same content again with no dedup key at all: the content hash
     * absorbs it. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "A new wording.", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_CHECK(res.duplicate);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), 2);

    /* But a genuinely different wording is a real revision, which proves the
     * absorption above was about content rather than about being broken. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "A third wording.", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_CHECK(!res.duplicate);
        T_EQ_INT((int)res.revision_no, 3);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- attribution ------------------------------------------------------------------ */

static void test_approval_is_never_attributed_to_a_session(void) {
    /* Test 21. An approval must be sessionless even when a session is open and
     * even when the request carries its id: attaching it would record that a
     * conversation approved something. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* An open Claude session, created the way A2 creates one. */
    int64_t client_id = 0, session_id = 0;
    bool created = false;
    T_OK(atlas_db_ai_client_upsert(e.db, "anthropic", "claude-code", &client_id, &err), &err);
    T_OK(atlas_db_ai_session_open(e.db, client_id, "sess-1", 0, NULL, NULL, NULL, &session_id,
                                  &created, &err),
         &err);

    /* A proposal from that session is attributed to it. */
    atlas_buf uid = ATLAS_BUF_INIT;
    {
        atlas_decision_op op;
        build_proposal(&op, "T", "D", &err);
        T_OK(atlas_buf_set_str(&op.provider, "anthropic", &err), &err);
        T_OK(atlas_buf_set_str(&op.client, "claude-code", &err), &err);
        T_OK(atlas_buf_set_str(&op.session_key, "sess-1", &err), &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_CHECK_MSG(!res.session_unbound, "a proposal from an open session must be attributed");
        T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions WHERE session_id IS NOT NULL;"),
             1);

    /* The approval carries the very same session key, and must still be
     * sessionless. */
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge(&e, atlas_buf_cstr(&uid), NULL, 1, &token, confirm, &err);
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_APPROVE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.token, atlas_buf_cstr(&token), &err), &err);
        T_OK(atlas_buf_set_str(&op.confirmation, confirm, &err), &err);
        T_OK(atlas_buf_set_str(&op.provider, "anthropic", &err), &err);
        T_OK(atlas_buf_set_str(&op.client, "claude-code", &err), &err);
        T_OK(atlas_buf_set_str(&op.session_key, "sess-1", &err), &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_CHECK_MSG(res.session_unbound, "an approval must be sessionless");
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    /* Nothing in the ledger names a session, because the ledger has no column
     * for one — checked here as a property of the approval event's actor. */
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED' AND "
                           "actor='LOCAL_OPERATOR_CONFIRMED';"),
             1);
    /* The session's own record gained nothing: an approval is not a session
     * event and must not inflate a session's counters. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM ai_session_events;"), 0);

    atlas_buf_free(&token);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_closed_session_yields_an_honest_gap(void) {
    /* Test 23, the `/clear` case. The MCP server keeps the id it was spawned
     * with, so after a clear that id names the conversation that ended. The
     * record is stored sessionless with a typed reason rather than attached to
     * the finished session or to a neighbour. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t client_id = 0, s1 = 0, s2 = 0;
    bool created = false;
    T_OK(atlas_db_ai_client_upsert(e.db, "anthropic", "claude-code", &client_id, &err), &err);
    T_OK(atlas_db_ai_session_open(e.db, client_id, "old", 0, NULL, NULL, NULL, &s1, &created, &err),
         &err);
    T_OK(atlas_db_ai_session_open(e.db, client_id, "new", 0, NULL, NULL, NULL, &s2, &created, &err),
         &err);
    T_OK(atlas_db_ai_session_close(e.db, s1, "cleared", &err), &err);

    atlas_decision_op op;
    build_proposal(&op, "T", "D", &err);
    T_OK(atlas_buf_set_str(&op.provider, "anthropic", &err), &err);
    T_OK(atlas_buf_set_str(&op.client, "claude-code", &err), &err);
    T_OK(atlas_buf_set_str(&op.session_key, "old", &err), &err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
    T_CHECK(res.session_unbound);
    T_CHECK_MSG(res.unbound_reason != NULL &&
                    strcmp(res.unbound_reason, ATLAS_AI_UNBOUND_SESSION_CLOSED) == 0,
                "the reason must say the session had ended");
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    /* And above all it was not attached to the *other* open session. */
    T_CHECK_MSG(count_of(&e,
                         "SELECT COUNT(*) FROM decision_revisions WHERE session_id IS NOT NULL;") == 0,
                "a record that cannot be attributed exactly must never borrow a session");

    env_close(&e);
}

/* --- the ledger is canonical ------------------------------------------------------ */

static void test_the_cache_can_be_verified_against_the_ledger(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);
    approve(&e, atlas_buf_cstr(&uid), 1, &err);

    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err), &err);
    T_EQ_INT((int)checked, 1);
    T_EQ_INT((int)mismatched, 0);

    /* Corrupt the cache and confirm the ledger disagrees. This is what `atlas
     * doctor` reports; it never repairs, so the mismatch survives the check. */
    T_OK(atlas_db_exec_sql(
             e.db, "UPDATE decision_documents SET current_status='PROPOSED', current_revision_id=NULL;",
             &err),
         &err);
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err), &err);
    T_CHECK_MSG((int)(mismatched) == 1, "a cache that disagrees with the ledger must be reported");

    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err), &err);
    T_CHECK_MSG((int)(mismatched) == 1, "verification must report rather than repair");

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_rejecting_one_revision_leaves_the_document_proposed(void) {
    /* The document's status is *derived*, and deriving it by hand at each
     * transition is how it drifts from the ledger.
     *
     * Rejecting revision 1 while revision 2 is still proposed does not make the
     * document rejected — there is an outstanding proposal. An earlier version
     * of `op_reject` set REJECTED whenever nothing was approved, which made
     * `atlas doctor` report corruption on this entirely legal sequence. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "The first wording.", &uid, NULL, &err);
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "The second wording.", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    /* Reject revision 1 specifically. */
    {
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        challenge_for(&e, atlas_buf_cstr(&uid), NULL, 1, ATLAS_DECISION_INTENT_REJECT, &token,
                      confirm, &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(spend(&e, ATLAS_DECISION_OP_REJECT, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                   confirm, &res, &err),
             &err);
        atlas_decision_result_free(&res);
        atlas_buf_free(&token);
    }

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "PROPOSED") == 0,
                "a document with an outstanding proposal is PROPOSED, and this one says %s",
                p.status);
    doc_probe_free(&p);

    /* The cache and the ledger must agree, which is what `atlas doctor` checks. */
    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err), &err);
    T_CHECK_MSG(mismatched == 0, "the cached status must match the ledger replay");

    /* Rejecting the last outstanding proposal does make the document rejected. */
    {
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        challenge_for(&e, atlas_buf_cstr(&uid), NULL, 2, ATLAS_DECISION_INTENT_REJECT, &token,
                      confirm, &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(spend(&e, ATLAS_DECISION_OP_REJECT, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                   confirm, &res, &err),
             &err);
        atlas_decision_result_free(&res);
        atlas_buf_free(&token);
    }
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK(strcmp(p.status, "REJECTED") == 0);
    doc_probe_free(&p);
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err), &err);
    T_CHECK(mismatched == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_rejecting_a_proposal_does_not_retract_what_is_approved(void) {
    /* Rule 5's other half: an approved revision stays effective when a later
     * proposal is refused. The document is still APPROVED, at revision 1. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "The approved wording.", &uid, NULL, &err);
    approve(&e, atlas_buf_cstr(&uid), 1, &err);
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "A worse idea.", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    {
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        challenge_for(&e, atlas_buf_cstr(&uid), NULL, 2, ATLAS_DECISION_INTENT_REJECT, &token,
                      confirm, &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(spend(&e, ATLAS_DECISION_OP_REJECT, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                   confirm, &res, &err),
             &err);
        atlas_decision_result_free(&res);
        atlas_buf_free(&token);
    }

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "APPROVED") == 0,
                "refusing a later proposal must not retract what is approved");
    T_CHECK(p.head_revision_no == 1);
    doc_probe_free(&p);
    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err), &err);
    T_CHECK(mismatched == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_deduped_retry_reports_the_revision_it_matched(void) {
    /* The triple a retry gets back must describe one revision.
     *
     * An earlier version filled the id from the dedup-matched row and the
     * number and hash from "the newest revision" — the same row only when
     * nothing had been added since, which is exactly not the case a redelivered
     * hook is about. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "One.", &uid, NULL, &err);

    int64_t first_id = 0;
    char first_hash[ATLAS_SHA256_HEX_LEN + 1u];
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "Two.", &err), &err);
        T_OK(atlas_buf_set_str(&op.dedup_key, "turn-2", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        first_id = res.revision_id;
        (void)snprintf(first_hash, sizeof(first_hash), "%s", res.content_hash);
        T_CHECK(res.revision_no == 2);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    /* A third revision arrives before the retry does. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "Three.", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_CHECK(res.revision_no == 3);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }

    /* Now the redelivery of turn 2. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "T", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "Two.", &err), &err);
        T_OK(atlas_buf_set_str(&op.dedup_key, "turn-2", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_CHECK(res.duplicate);
        T_CHECK_MSG(res.revision_id == first_id, "the retry must report the row it matched");
        T_CHECK_MSG(res.revision_no == 2,
                    "the retry must report revision 2, and reported %lld",
                    (long long)res.revision_no);
        T_CHECK_MSG(strcmp(res.content_hash, first_hash) == 0,
                    "the retry must report the matched revision's hash");
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), 3);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_mutating_any_hashed_field_is_detected(void) {
    /* **The immutability claim, made checkable.**
     *
     * Atlas never updates a content column, so every field below can only be
     * changed by something outside Atlas. Each one is part of the canonical
     * encoding, so each mutation must leave a revision that no longer hashes to
     * its own recorded digest — which is what `atlas doctor` reports and what
     * an approval bound to that digest depends on.
     *
     * Every class of approval-relevant field is covered: prose, scope, the
     * link's target, the link's *snapshot provenance* (the basis commit, the
     * captured file hash, the analyzer identity), the revision's basis HEAD,
     * the repository identity and the proposing actor. */
    static const char *const MUTATIONS[] = {
        "UPDATE decision_revisions SET title = 'Rewritten';",
        "UPDATE decision_revisions SET decision_text = 'Something else entirely.';",
        "UPDATE decision_revisions SET rationale_text = 'A different reason.';",
        "UPDATE decision_revisions SET context_text = 'A different problem.';",
        "UPDATE decision_revisions SET consequences_text = 'Different consequences.';",
        "UPDATE decision_revisions SET scope = 'REPOSITORY';",
        "UPDATE decision_revisions SET proposed_by = 'MODEL_INFERENCE';",
        "UPDATE decision_revisions SET basis_head = 'ffffffffffffffffffffffffffffffffffffffff';",
        "UPDATE decision_revisions SET basis_repo_identity_hash = 'a-different-repository';",
        "UPDATE decision_alternatives SET text = 'a different alternative';",
        "UPDATE decision_links SET path_raw = CAST('src/other.c' AS BLOB);",
        "UPDATE decision_links SET basis_commit = 'cccccccccccccccccccccccccccccccccccccccc';",
        "UPDATE decision_links SET file_content_hash = 'a-different-file-hash';",
        "UPDATE decision_links SET analyzer_name = 'some-other-analyzer';",
        "UPDATE decision_links SET analyzer_version = 99;",
        "UPDATE decision_links SET symbol_line = 4242;",
        NULL,
    };
    for (size_t i = 0; MUTATIONS[i] != NULL; i++) {
        atlas_err err;
        atlas_err_init(&err);
        env e;
        env_open(&e, &err);

        /* One approved decision with an alternative and a fully snapshotted
         * symbol link, so every mutation above has something to act on. */
        atlas_buf uid = ATLAS_BUF_INIT;
        {
            atlas_decision_op op;
            build_proposal(&op, "Locking", "One writer owns the writable handle.", &err);
            T_OK(atlas_buf_set_str(&op.revision.context_text, "Two writers raced.", &err), &err);
            T_OK(atlas_buf_set_str(&op.revision.rationale_text, "Because it is provable.", &err),
                 &err);
            T_OK(atlas_buf_set_str(&op.revision.consequences_text, "One writer thread.", &err),
                 &err);
            T_OK(atlas_decision_revision_add_alternative(&op.revision, "a mutex per table", 17u,
                                                         &err),
                 &err);
            atlas_decision_link l;
            atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
            T_OK(atlas_buf_set_str(&l.symbol_name, "atlas_db_open", &err), &err);
            T_OK(atlas_buf_set_str(&l.path_raw, "src/db.c", &err), &err);
            T_OK(atlas_buf_set_str(&l.basis_commit, "abcabcabcabcabcabcabcabcabcabcabcabcabca",
                                   &err),
                 &err);
            T_OK(atlas_buf_set_str(&l.file_content_hash, "hash-one", &err), &err);
            T_OK(atlas_buf_set_str(&l.analyzer_name, "atlas-lexical-c", &err), &err);
            l.analyzer_version = 1;
            l.symbol_line = 42;
            T_OK(atlas_decision_revision_add_link(&op.revision, &l, &err), &err);
            atlas_decision_link_free(&l);
            atlas_decision_result res;
            atlas_decision_result_init(&res);
            T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
            T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, &err), &err);
            atlas_decision_result_free(&res);
            atlas_decision_op_free(&op);
        }
        approve(&e, atlas_buf_cstr(&uid), 1, &err);

        /* Clean before. */
        int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
        T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
             &err);
        T_CHECK_MSG(rehashed == 1, "one revision should have been rehashed, not %lld",
                    (long long)rehashed);
        T_CHECK_MSG(corrupt == 0, "mutation %zu: the fixture must verify before it is mutated", i);

        T_OK(atlas_db_exec_sql(e.db, MUTATIONS[i], &err), &err);

        T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
             &err);
        T_CHECK_MSG(corrupt == 1,
                    "mutation %zu (%s) left the revision verifying against its own content hash; "
                    "that field is part of what was approved and must be covered by the digest",
                    i, MUTATIONS[i]);

        /* The approval is still recorded — Atlas reports the divergence rather
         * than deleting or downgrading anything. */
        T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"),
                 1);

        atlas_buf_free(&uid);
        env_close(&e);
    }
}

static void test_mutating_an_unhashed_field_is_not_flagged(void) {
    /* The other side of the line, so the test above is a statement about *what*
     * is covered rather than about everything being covered.
     *
     * Two fields, and the second is the whole point of the correction:
     *
     *   - a revision's lifecycle `state` is not content. It is where the ledger
     *     has left the row and changes legitimately on every transition.
     *     Hashing it would make an approval stop verifying the instant it was
     *     granted.
     *   - `decision_documents.repo_identity_hash` is *attachment* metadata. It
     *     starts empty on a repository whose history has not been ingested and
     *     is backfilled when the lineage becomes knowable. Hashing it meant an
     *     ordinary propose-then-scan changed the verification input of an
     *     already-written revision. The revision's own
     *     `basis_repo_identity_hash` is the hashed one, and the test above
     *     proves mutating *that* is caught. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);
    approve(&e, atlas_buf_cstr(&uid), 1, &err);

    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(corrupt == 0,
                "an approved revision must verify against its own content hash; the state is not "
                "content");
    T_CHECK(rehashed == 1);

    /* The document's attachment identity may move without touching any
     * revision. */
    T_OK(atlas_db_exec_sql(
             e.db, "UPDATE decision_documents SET repo_identity_hash = 'moved-elsewhere';", &err),
         &err);
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(corrupt == 0,
                "backfilling or relinking the document's attachment identity must not invalidate "
                "an existing revision; the revision captured its own");

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_pre_scan_revision_survives_the_identity_backfill(void) {
    /* The exact sequence that used to make `atlas doctor` report a healthy
     * record as corrupt: propose before any history has been ingested, then
     * ingest it.
     *
     * Every immutable field of the revision, and its hash, must be
     * byte-identical afterwards; a later revision must capture the identity
     * that is now knowable; and the two must legitimately differ. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open_without_lineage(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "Pre-scan", "Written before Atlas had read any history.", &uid, NULL, &err);

    /* Nothing was knowable, and that is recorded as such rather than guessed. */
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_revisions"
                             " WHERE basis_repo_identity_hash = '';") == 1,
                "a revision written before ingestion must capture an explicit empty identity");

    /* A digest of every immutable column of the revision, plus its own hash. */
    char before[ATLAS_SHA256_HEX_LEN + 1u];
    decision_table_digest(&e, "decision_revisions", before);

    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(corrupt == 0 && mismatched == 0,
                "doctor must be clean before ingestion");

    /* Ingestion, then the backfill it enables. */
    seed_root_commit(&e, e.repo_id, ORIGINAL_ROOT_COMMIT, &err);
    int64_t relinked = 0;
    T_OK(atlas_db_decision_relink_after_ingest(e.db, e.repo_id, &relinked, &err), &err);
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_documents"
                             " WHERE repo_identity_hash <> '';") == 1,
                "the document's attachment identity must have been backfilled");

    /* The revision is untouched, byte for byte. */
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    decision_table_digest(&e, "decision_revisions", after);
    T_CHECK_MSG(strcmp(before, after) == 0,
                "the backfill changed a revision; it must only touch document metadata");
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_revisions"
                             " WHERE basis_repo_identity_hash = '';") == 1,
                "the revision's captured identity must stay empty for ever");

    /* And doctor still agrees. */
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(corrupt == 0,
                "doctor reported a healthy revision as corrupt after the identity backfill");
    T_CHECK_MSG(mismatched == 0, "and the ledger must still agree");

    /* A later revision captures what is now knowable, so the two differ. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
        op_repo(&op, &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "Pre-scan", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "Written after ingestion.", &err),
             &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_revisions"
                             " WHERE revision_no = 2 AND basis_repo_identity_hash <> '';") == 1,
                "a revision written after ingestion must capture the identity");
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(DISTINCT basis_repo_identity_hash)"
                             " FROM decision_revisions;") == 2,
                "the two revisions legitimately captured different identities");

    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(corrupt == 0 && rehashed == 2, "both revisions must verify");

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_an_identity_unknown_revision_can_still_be_approved(void) {
    /* The chosen policy, exercised.
     *
     * Approval is **allowed** for a revision with no captured identity, because
     * refusing would make a decision unapprovable until a scan completed and
     * would buy nothing: the approval binds to the content hash, and that hash
     * covers the captured identity including its absence, so the record
     * verifies for ever either way. What the absence costs is durable
     * reattachment, and the operator prompt says so — `test_decision_operator.c`
     * asserts the wording. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open_without_lineage(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);
    approve(&e, atlas_buf_cstr(&uid), 1, &err);

    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "APPROVED") == 0,
                "a revision with no captured identity must still be approvable");
    doc_probe_free(&p);

    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(corrupt == 0, "and it must verify");

    /* And it is fail-closed for relinking: an empty captured identity means the
     * document's attachment identity is also empty until something ingests
     * history, and an empty identity matches nothing. */
    int64_t relinked = 0;
    T_OK(atlas_db_decision_relink_repo(e.db, e.repo_id, "", &relinked, &err), &err);
    T_CHECK_MSG(relinked == 0, "an empty identity must never match anything");

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_nothing_is_ever_deleted(void) {
    /* Rule 10, checked by doing everything that could plausibly remove
     * something and counting afterwards. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf a = ATLAS_BUF_INIT, b = ATLAS_BUF_INIT;
    propose(&e, "A", "D1", &a, NULL, &err);
    propose(&e, "B", "D2", &b, NULL, &err);
    approve(&e, atlas_buf_cstr(&a), 1, &err);
    reject_it(&e, atlas_buf_cstr(&b), &err);
    supersede(&e, atlas_buf_cstr(&a), atlas_buf_cstr(&b), ATLAS_OK, &err);

    int64_t docs = count_of(&e, "SELECT COUNT(*) FROM decision_documents;");
    int64_t revs = count_of(&e, "SELECT COUNT(*) FROM decision_revisions;");
    int64_t events = count_of(&e, "SELECT COUNT(*) FROM decision_events;");
    int64_t spent = count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;");
    T_EQ_INT((int)docs, 2);
    T_EQ_INT((int)revs, 2);
    T_CHECK(events >= 5);
    T_EQ_INT((int)spent, 3);

    /* Removing the repository must not destroy approval history. Every other
     * table in Atlas cascades from `repositories`; these deliberately do not,
     * because a decision is not a rebuildable index. */
    {
        bool removed = false;
        T_OK(atlas_db_repo_remove(e.db, "proj", &removed, &err), &err);
        T_CHECK(removed);
    }
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_documents;") == docs,
                "repo remove must not delete decision documents");
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), (int)revs);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events;"), (int)events);
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE consumed=1;") == spent,
                "a consumed challenge is part of an approval record");

    /* An *unrelated* repository registered afterwards must inherit nothing,
     * **even at the same path**.
     *
     * Two things could go wrong and both are tested here. `repositories.id` is
     * a reused rowid, so a new registration very likely gets the id the removed
     * one had. And a hash of the root *path* says "same directory", which is a
     * location rather than an identity: `rm -rf` a project and `git init` an
     * unrelated one in its place and the path hash is unchanged.
     *
     * The identity commits to the ingested root commits as well, so it
     * distinguishes them. */
    atlas_repo_identity other;
    memset(&other, 0, sizeof(other));
    other.root = "/tmp/atlas-decision-repo"; /* the *same* path, deliberately */
    other.root_len = strlen(other.root);
    other.common_dir = "/tmp/atlas-decision-repo/.git";
    other.common_dir_len = strlen(other.common_dir);
    other.git_dir = other.common_dir;
    other.git_dir_len = other.common_dir_len;
    other.object_format = "sha1";
    int64_t other_id = 0;
    T_OK(atlas_db_repo_add(e.db, "unrelated", &other, &other_id, &err), &err);
    /* An unrelated lineage. */
    seed_root_commit(&e, other_id, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &err);
    int64_t relinked = 0;
    T_OK(atlas_db_decision_relink_after_ingest(e.db, other_id, &relinked, &err), &err);
    T_CHECK_MSG(relinked == 0,
                "an unrelated repository at the same path must inherit nothing, and %lld "
                "documents were attached to it",
                (long long)relinked);

    int64_t proposed = 0, approved_n = 0, rejected = 0, superseded = 0, resolved_n = 0;
    T_OK(atlas_db_decision_repo_counts(e.db, other_id, &proposed, &approved_n, &rejected,
                                       &superseded, &resolved_n, &err),
         &err);
    T_CHECK_MSG(proposed + approved_n + rejected + superseded + resolved_n == 0,
                "a replaced repository must inherit no decisions");

    /* They are orphaned rather than deleted, and visible as such. A canonical
     * record that has become invisible looks exactly like one that was
     * destroyed. */
    int64_t orphans = 0;
    bool more = false;
    T_OK(atlas_db_decision_orphans_list(e.db, 50, count_docs_cb, &orphans, &orphans, &more, &err),
         &err);
    T_CHECK_MSG(orphans == 2, "both decisions must be listed as orphaned, and %lld were",
                (long long)orphans);

    /* And re-registering the *original* repository — same path and the same
     * lineage — reclaims them. */
    {
        bool removed = false;
        T_OK(atlas_db_repo_remove(e.db, "unrelated", &removed, &err), &err);
    }
    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = "/tmp/atlas-decision-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-decision-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t again = 0;
    T_OK(atlas_db_repo_add(e.db, "proj", &id, &again, &err), &err);
    seed_root_commit(&e, again, ORIGINAL_ROOT_COMMIT, &err);
    T_OK(atlas_db_decision_relink_after_ingest(e.db, again, &relinked, &err), &err);
    T_CHECK_MSG(relinked == 2,
                "the original repository must reclaim both decisions, and %lld were reattached",
                (long long)relinked);

    T_OK(atlas_db_decision_repo_counts(e.db, again, &proposed, &approved_n, &rejected, &superseded,
                                       &resolved_n, &err),
         &err);
    T_CHECK_MSG(proposed + approved_n + rejected + superseded + resolved_n == 2,
                "both documents must be reattached to the re-registered repository, and %lld were",
                (long long)(proposed + approved_n + rejected + superseded + resolved_n));

    atlas_buf_free(&a);
    atlas_buf_free(&b);
    env_close(&e);
}

/* --- A2 compatibility --------------------------------------------------------------- */

static void test_a2_proposals_survive_and_promote_without_being_approved(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* An A2 proposal, written the way A2 writes one — and **attributed to a real
     * session**, because that is the attribution the promotion must preserve
     * without borrowing. Without a session on the A2 row the sessionless
     * assertion below would pass for the trivial reason that there was never
     * anything to copy. */
    int64_t client_id = 0, origin_session = 0;
    T_OK(atlas_db_ai_client_upsert(e.db, "anthropic", "claude-code", &client_id, &err), &err);
    bool created = false;
    T_OK(atlas_db_ai_session_open(e.db, client_id, "historical-session", 0, NULL, NULL, NULL,
                                  &origin_session, &created, &err),
         &err);
    T_REQUIRE(origin_session > 0);

    int64_t legacy_id = 0;
    bool dup = false;
    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.session_id = origin_session;
    in.repo_id = e.repo_id;
    in.provenance = "MODEL_PROPOSAL";
    in.state = "proposed";
    in.title = "An A2 decision";
    in.statement = "Recorded before A4 existed.";
    in.rationale = "Because it seemed right.";
    T_OK(atlas_db_ai_decision_insert(e.db, &in, &legacy_id, &dup, &err), &err);
    T_OK(atlas_db_ai_decision_path_add(e.db, legacy_id, "src/a.c", 7u, "src/a.c", &err), &err);

    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROMOTE);
    op_repo(&op, &err);
    op.legacy_id = legacy_id;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
    T_CHECK(res.document_created);
    T_CHECK(res.state == ATLAS_DECISION_PROPOSED);
    atlas_buf uid = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, &err), &err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    /* A promoted document is PROPOSED, never approved. An A2 row could never
     * have been approved, and a promotion that made one look approved would be
     * the single most damaging thing this phase could do. */
    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK(strcmp(p.status, "PROPOSED") == 0);
    doc_probe_free(&p);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event='APPROVED';"), 0);

    /* The A2 row is exactly as it was: not modified, not deleted, still pinned
     * to approved = 0 by its own CHECK. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM ai_decisions;"), 1);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM ai_decisions WHERE approved=0;"), 1);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM ai_decision_paths;"), 1);
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM ai_decisions WHERE title='An A2 decision' AND "
                           "statement='Recorded before A4 existed.';"),
             1);
    /* The link back is recorded, so the two records stay connected without
     * either becoming the other. */
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM decision_revisions WHERE "
                           "imported_from_ai_decision_id IS NOT NULL;"),
             1);
    /* **And the promoted revision is sessionless.**
     *
     * The A2 row records which session proposed it and is left intact and
     * reachable. Copying that session onto the A4 revision would claim the
     * session proposed *this* record, which it did not — the promotion happened
     * later, possibly at an operator's request. `imported_from_ai_decision_id`
     * is the attribution pointer; a session id would be an attribution claim. */
    T_CHECK_MSG(count_of(&e,
                         "SELECT COUNT(*) FROM decision_revisions WHERE session_id IS NOT NULL;") ==
                    0,
                "a promoted revision must be sessionless, with imported_from as its pointer");
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_revisions"
                             " WHERE session_unbound = 1 AND unbound_reason = 'no_session_id';") ==
                    1,
                "the gap must be typed, not merely a NULL nobody explained");

    /* The original attribution is not lost — it is one join away, on the A2 row
     * the pointer names. That is the whole design: the promotion did not happen
     * in the historical session, so the A4 revision does not claim it did, and
     * anyone who wants to know who proposed the original can still find out. */
    T_CHECK_MSG(count_of(&e,
                         "SELECT COUNT(*) FROM decision_revisions r"
                         "  JOIN ai_decisions a ON a.id = r.imported_from_ai_decision_id"
                         "  JOIN ai_sessions s ON s.id = a.session_id"
                         " WHERE s.session_key = 'historical-session';") == 1,
                "the A2 origin's session must stay reachable through "
                "imported_from_ai_decision_id");
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM ai_decisions WHERE session_id IS NOT NULL;") == 1,
                "and the A2 row's own attribution must be left exactly as it was");
    /* And the paths came across. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_links WHERE kind='path';"), 1);

    /* Promoting the same row twice is refused by the unique index rather than
     * producing two documents about one proposal. */
    atlas_decision_op again;
    atlas_decision_op_init(&again, ATLAS_DECISION_OP_PROMOTE);
    op_repo(&again, &err);
    again.legacy_id = legacy_id;
    atlas_decision_result res2;
    atlas_decision_result_init(&res2);
    T_CHECK(atlas_decision_apply(e.db, &again, &res2, &err) != ATLAS_OK);
    atlas_decision_result_free(&res2);
    atlas_decision_op_free(&again);
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_documents;") == 1,
                "a failed second promote must leave no orphan document behind");

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- code anchors, and the structural rebuild ------------------------------------
 *
 * These two properties are guaranteed by the *schema* — no decision table
 * references a migration-5 table — but a guarantee nothing exercises is a
 * guarantee nobody has checked. So the tests below build a small real
 * structural graph, link decisions into it, and then take it away.
 */

/* A minimal `code_files` / `code_symbols` pair, written the way an A3 pass
 * leaves them. Hand-written rather than produced by running the indexer,
 * because what is under test is the decision layer's behaviour when the graph
 * changes, and a real indexer run would make the fixture depend on the lexer. */
static void seed_code_graph(env *e, const char *path, const char *content_hash,
                            const char *symbol, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           /* CAST to BLOB, because that is what the column is
                            * and what the production path binds. SQLite orders
                            * TEXT before BLOB and never compares them equal, so
                            * a TEXT literal here would seed rows that no lookup
                            * could ever find — and the test would fail for a
                            * reason that has nothing to do with its subject. */
                           "INSERT INTO code_files(repo_id, path_raw, path_text, basename_raw,"
                           " language, content_hash, parsed_at, parse_status)"
                           " VALUES(%lld, CAST('%s' AS BLOB), '%s', CAST('%s' AS BLOB), 'c',"
                           " '%s', 't', 'ok')"
                           " ON CONFLICT(repo_id, path_raw) DO UPDATE"
                           " SET content_hash = '%s';",
                           (long long)e->repo_id, path, path, path, content_hash, content_hash),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_reset(&sql);
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO code_symbols(repo_id, code_file_id, name, name_text, kind,"
                           " linkage, resolution, is_definition)"
                           " SELECT %lld, id, CAST('%s' AS BLOB), '%s', 'function', 'external',"
                           " 'SOURCE_EXACT', 1 FROM code_files"
                           " WHERE repo_id = %lld AND path_raw = CAST('%s' AS BLOB);",
                           (long long)e->repo_id, symbol, symbol, (long long)e->repo_id, path),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);

    /* A completed structural pass, so link currency is a real answer rather
     * than UNKNOWN-because-Atlas-has-not-looked. */
    atlas_buf state = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&state, err,
                           "INSERT INTO code_index_state(repo_id, generation,"
                           " last_complete_generation) VALUES(%lld, 1, 1)"
                           " ON CONFLICT(repo_id) DO UPDATE SET last_complete_generation = 1;",
                           (long long)e->repo_id),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&state), err), err);
    atlas_buf_free(&state);
}

/* Resolves one symbol link against the current graph and reports its currency
 * and match count. */
static void resolve_symbol(env *e, const char *symbol, const char *path,
                           const char *snapshot_hash, atlas_decision_link_currency *currency,
                           int64_t *matches, atlas_err *err) {
    atlas_decision_link l;
    atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
    T_OK(atlas_buf_set_str(&l.symbol_name, symbol, err), err);
    if (path != NULL) {
        T_OK(atlas_buf_set_str(&l.path_raw, path, err), err);
    }
    if (snapshot_hash != NULL) {
        T_OK(atlas_buf_set_str(&l.file_content_hash, snapshot_hash, err), err);
    }
    T_OK(atlas_db_decision_link_resolve(e->db, e->repo_id, &l, true, true, err), err);
    *currency = l.currency;
    *matches = l.match_count;
    atlas_decision_link_free(&l);
}

static void test_a_code_anchor_is_never_guessed(void) {
    /* A renamed, deleted or ambiguous anchor is reported, never re-pointed. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    seed_code_graph(&e, "src/db.c", "hash-one", "atlas_db_open", &err);

    atlas_decision_link_currency c;
    int64_t n = 0;

    /* The snapshot matches: the symbol is defined in the recorded file, and
     * that file's content hash is what was recorded. */
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-one", &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_CURRENT, "expected CURRENT, got %s",
                atlas_decision_link_currency_name(c));
    T_CHECK(n == 1);

    /* The file changed. The anchor still resolves; the link needs review. The
     * decision is not revoked and the link is not rewritten. */
    T_OK(atlas_db_exec_sql(e.db, "UPDATE code_files SET content_hash = 'hash-two';", &err), &err);
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-one", &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_CHANGED, "expected CHANGED, got %s",
                atlas_decision_link_currency_name(c));

    /* **The recorded file is part of the selector.** A second file defines
     * something with the same name — the normal state of a large C project —
     * and the anchor must still resolve against the file it was recorded in
     * rather than becoming permanently ambiguous. */
    seed_code_graph(&e, "src/other.c", "hash-other", "atlas_db_open", &err);
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-two", &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_CURRENT,
                "a same-named definition elsewhere must not make the recorded anchor ambiguous; "
                "got %s",
                atlas_decision_link_currency_name(c));

    /* A snapshot with no recorded file *is* ambiguous, and says how many. */
    resolve_symbol(&e, "atlas_db_open", NULL, NULL, &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_AMBIGUOUS, "expected AMBIGUOUS, got %s",
                atlas_decision_link_currency_name(c));
    T_CHECK_MSG(n == 2, "the ambiguity must be a number, and is %lld", (long long)n);

    /* The symbol is renamed: gone from the recorded file, and something of that
     * name still exists elsewhere. MISSING with a count — never silently
     * re-pointed at the other definition. */
    T_OK(atlas_db_exec_sql(e.db,
                           "DELETE FROM code_symbols WHERE code_file_id ="
                           " (SELECT id FROM code_files"
                           "   WHERE path_raw = CAST('src/db.c' AS BLOB));",
                           &err),
         &err);
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-two", &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_MISSING,
                "a symbol gone from its recorded file is MISSING, not re-pointed; got %s",
                atlas_decision_link_currency_name(c));
    T_CHECK_MSG(n == 1, "the count must say another definition exists, and is %lld", (long long)n);

    /* Deleted everywhere. */
    T_OK(atlas_db_exec_sql(e.db, "DELETE FROM code_symbols;", &err), &err);
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-two", &c, &n, &err);
    T_CHECK(c == ATLAS_DECISION_LINK_MISSING);
    T_CHECK(n == 0);

    /* A path link to a file Atlas has no row for. */
    {
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        T_OK(atlas_buf_set_str(&l.path_raw, "src/never-existed.c", &err), &err);
        T_OK(atlas_db_decision_link_resolve(e.db, e.repo_id, &l, true, true, &err), &err);
        T_CHECK(l.currency == ATLAS_DECISION_LINK_MISSING);
        atlas_decision_link_free(&l);
    }

    /* An index that has not run is UNKNOWN, not MISSING: "Atlas has not looked"
     * and "it is not there" are different facts, and only one of them is about
     * the repository. */
    {
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
        T_OK(atlas_buf_set_str(&l.symbol_name, "atlas_db_open", &err), &err);
        T_OK(atlas_db_decision_link_resolve(e.db, e.repo_id, &l, false, false, &err), &err);
        T_CHECK_MSG(l.currency == ATLAS_DECISION_LINK_UNKNOWN,
                    "an index that has never completed a pass yields UNKNOWN, got %s",
                    atlas_decision_link_currency_name(l.currency));
        atlas_decision_link_free(&l);
    }

    env_close(&e);
}

static void test_a_structural_rebuild_preserves_every_decision(void) {
    /* `atlas_db_code_clear_repo` is what a `code sync --rebuild` and an
     * analyzer-version bump both do: drop every structural row and reindex.
     *
     * Nothing in migration 6 references a migration-5 table, so this must be
     * invisible to the decision record. Checked by digesting all five decision
     * tables column by column rather than by counting rows — a rebuild that
     * rewrote a column would keep the count. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    seed_code_graph(&e, "src/db.c", "hash-one", "atlas_db_open", &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    {
        atlas_decision_op op;
        build_proposal(&op, "Locking", "One writer owns the writable handle.", &err);
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_SYMBOL);
        T_OK(atlas_buf_set_str(&l.symbol_name, "atlas_db_open", &err), &err);
        T_OK(atlas_buf_set_str(&l.symbol_name_text, "atlas_db_open", &err), &err);
        T_OK(atlas_buf_set_str(&l.path_raw, "src/db.c", &err), &err);
        T_OK(atlas_buf_set_str(&l.path_text, "src/db.c", &err), &err);
        T_OK(atlas_buf_set_str(&l.file_content_hash, "hash-one", &err), &err);
        T_OK(atlas_buf_set_str(&l.analyzer_name, "atlas-lexical-c", &err), &err);
        l.analyzer_version = 1;
        T_OK(atlas_decision_revision_add_link(&op.revision, &l, &err), &err);
        atlas_decision_link_free(&l);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    approve(&e, atlas_buf_cstr(&uid), 1, &err);

    atlas_decision_link_currency c;
    int64_t n = 0;
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-one", &c, &n, &err);
    T_CHECK(c == ATLAS_DECISION_LINK_CURRENT);

    static const char *const TABLES[] = {
        "decision_documents", "decision_revisions", "decision_alternatives",
        "decision_links",     "decision_events",    NULL,
    };
    char before[8][ATLAS_SHA256_HEX_LEN + 1u];
    size_t table_count = 0;
    for (; TABLES[table_count] != NULL; table_count++) {
        decision_table_digest(&e, TABLES[table_count], before[table_count]);
    }

    T_OK(atlas_db_code_clear_repo(e.db, e.repo_id, &err), &err);
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM code_symbols;") == 0,
                "the rebuild must have cleared the structural graph");

    for (size_t i = 0; i < table_count; i++) {
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        decision_table_digest(&e, TABLES[i], after);
        T_CHECK_MSG(strcmp(before[i], after) == 0,
                    "a structural rebuild changed %s, which it must not touch at all", TABLES[i]);
    }

    /* The decision is still approved, and its link reports MISSING rather than
     * having been rewritten or having revoked anything. */
    doc_probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p, &err);
    T_CHECK_MSG(strcmp(p.status, "APPROVED") == 0,
                "a rebuild must not change a decision's lifecycle state");
    doc_probe_free(&p);
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-one", &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_MISSING,
                "after a rebuild with no reindex the anchor is MISSING, got %s",
                atlas_decision_link_currency_name(c));

    /* Reindexed at the same content — which is what an analyzer upgrade
     * followed by an ordinary pass produces. The anchor comes back, and the
     * decision tables are still byte-identical. */
    seed_code_graph(&e, "src/db.c", "hash-one", "atlas_db_open", &err);
    resolve_symbol(&e, "atlas_db_open", "src/db.c", "hash-one", &c, &n, &err);
    T_CHECK_MSG(c == ATLAS_DECISION_LINK_CURRENT,
                "reindexing the same content must restore the anchor, got %s",
                atlas_decision_link_currency_name(c));
    for (size_t i = 0; i < table_count; i++) {
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        decision_table_digest(&e, TABLES[i], after);
        T_CHECK_MSG(strcmp(before[i], after) == 0, "reindexing changed %s", TABLES[i]);
    }

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- the evidence table --------------------------------------------------------------- */

static void test_a4_writes_no_evidence(void) {
    /* The A0 rule, unchanged for the third phase running. A4 writes no evidence
     * at all: structural and lifecycle facts carry their own vocabularies, and
     * widening `evidence` to fit them would make "how does Atlas know this?"
     * and "what did somebody decide?" one question. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    int64_t before = count_of(&e, "SELECT COUNT(*) FROM evidence;");
    atlas_buf uid = ATLAS_BUF_INIT;
    propose(&e, "T", "D", &uid, NULL, &err);
    approve(&e, atlas_buf_cstr(&uid), 1, &err);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM evidence;"), (int)before);

    /* And the reserved kinds are still unused, INFERENCE included: A4 defines
     * no deterministic inference with its own provenance, so using the kind
     * would only mean "it was available". */
    T_EQ_INT((int)count_of(&e,
                           "SELECT COUNT(*) FROM evidence WHERE kind NOT IN ('SOURCE','GIT');"),
             0);

    atlas_buf_free(&uid);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"propose creates a document and a ledger entry",
     test_propose_creates_a_document_and_a_ledger_entry},
    {"approval supersedes the predecessor atomically",
     test_approval_supersedes_the_predecessor_atomically},
    {"content is immutable across every transition",
     test_content_is_immutable_across_every_transition},
    {"approval without a challenge is refused", test_approval_without_a_challenge_is_refused},
    {"a challenge is single use", test_a_challenge_is_single_use},
    {"a challenge is bound to one revision", test_a_challenge_is_bound_to_one_revision_and_one_document},
    {"a stale challenge approves only what it named",
     test_a_stale_challenge_does_not_approve_a_newer_revision},
    {"a wrong confirmation changes nothing", test_a_wrong_confirmation_changes_nothing},
    {"an expired challenge is refused", test_an_expired_challenge_is_refused},
    {"changed content under a challenge is refused",
     test_changed_content_under_a_challenge_is_refused},
    {"a rejected revision can never be approved", test_a_rejected_revision_can_never_be_approved},
    {"a second approval loses deterministically",
     test_a_second_approval_of_one_revision_loses_deterministically},
    {"supersession, and its cycle refusal", test_supersession_and_its_cycle_refusal},
    {"supersession cannot cross repositories", test_supersession_cannot_cross_repositories},
    {"retries are idempotent", test_retries_are_idempotent},
    {"approval is never attributed to a session",
     test_approval_is_never_attributed_to_a_session},
    {"a closed session yields an honest gap", test_a_closed_session_yields_an_honest_gap},
    {"the cache can be verified against the ledger",
     test_the_cache_can_be_verified_against_the_ledger},
    {"rejecting one revision leaves the document proposed",
     test_rejecting_one_revision_leaves_the_document_proposed},
    {"rejecting a proposal does not retract what is approved",
     test_rejecting_a_proposal_does_not_retract_what_is_approved},
    {"a deduped retry reports the revision it matched",
     test_a_deduped_retry_reports_the_revision_it_matched},
    {"mutating any hashed field is detected", test_mutating_any_hashed_field_is_detected},
    {"a pre-scan revision survives the identity backfill",
     test_a_pre_scan_revision_survives_the_identity_backfill},
    {"an identity-unknown revision can still be approved",
     test_an_identity_unknown_revision_can_still_be_approved},
    {"mutating an unhashed field is not flagged",
     test_mutating_an_unhashed_field_is_not_flagged},
    {"nothing is ever deleted", test_nothing_is_ever_deleted},
    {"A2 proposals survive and promote unapproved",
     test_a2_proposals_survive_and_promote_without_being_approved},
    {"a code anchor is never guessed", test_a_code_anchor_is_never_guessed},
    {"a structural rebuild preserves every decision",
     test_a_structural_rebuild_preserves_every_decision},
    {"A4 writes no evidence", test_a4_writes_no_evidence},
};

ATLAS_TEST_MAIN("decision_lifecycle", TESTS)

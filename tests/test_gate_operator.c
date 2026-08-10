/* Atlas - human revalidation, and everything that must refuse one.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Revalidation is the only thing A6 writes, and it reuses A4's operator channel
 * exactly: an interactive terminal, a short-lived single-use capability bound to
 * one revision and content hash, and a confirmation typed against that hash.
 * A6 adds two bindings on top — the exact repository state and a digest of what
 * the decision's anchors resolve to — and therefore two more ways to be refused.
 *
 * The interactive half is driven through a real pseudo-terminal, for the reason
 * `tests/test_decision_operator.c` gives: the requirement is that a terminal is
 * needed, and a test that stubbed one out would be testing the stub.
 *
 * Everything else is driven in process against the single write point, because
 * that is where the refusals live. A refusal that only the CLI performed would
 * be a refusal an IPC client could skip.
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/decision_ops.h"
#include "atlas/gate.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- environment ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_buf uid;
} env;

static void run_cli(env *e, const char *const *extra, size_t n, int want_code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(argv, k, &out, &errout, &code, &err), &err);
    T_CHECK_MSG(code == want_code, "%s exited %d (wanted %d)\n%s\n%s", extra[0], code, want_code,
                atlas_buf_cstr(&out), atlas_buf_cstr(&errout));
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
}

static void db_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);
}

static void db_close(env *e) {
    atlas_db_close(e->db);
    e->db = NULL;
}

static void capture_hash(env *e, const char *path, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *st = NULL;
    T_OK(atlas_db_prepare(e->db, "SELECT content_hash FROM files WHERE path_raw = ?1;", &st, &err),
         &err);
    T_REQUIRE(sqlite3_bind_blob(st, 1, path, (int)strlen(path), SQLITE_TRANSIENT) == SQLITE_OK);
    T_REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    T_OK(atlas_buf_set_str(out, (const char *)sqlite3_column_text(st, 0), &err), &err);
    atlas_db_finish(e->db, st);
}

static void head_oid(env *e, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", &info, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_buf_set_str(out, info.scanned_head, &err), &err);
    atlas_repo_info_free(&info);
}

/* Proposes one decision bound to `path`, approves it, and leaves its id in
 * `e->uid`. Through the real write point; a test that reached the tables would
 * be testing a state Atlas cannot produce. */
static void env_approve(env *e, const char *path) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.title, "Bounded to one file", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "This file is the one.", &err), &err);
    op.revision.scope = ATLAS_DECISION_SCOPE_PATHS;
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_buf head = ATLAS_BUF_INIT;
    head_oid(e, &head);
    T_OK(atlas_buf_set(&op.revision.basis_head, head.data, head.len, &err), &err);
    {
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        T_OK(atlas_buf_set_str(&l.path_raw, path, &err), &err);
        T_OK(atlas_buf_set_str(&l.path_text, path, &err), &err);
        capture_hash(e, path, &l.file_content_hash);
        T_OK(atlas_buf_set(&l.basis_commit, head.data, head.len, &err), &err);
        T_OK(atlas_decision_revision_add_link(&op.revision, &l, &err), &err);
    }
    atlas_buf_free(&head);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, &err), &err);
    T_OK(atlas_buf_set(&e->uid, res.uid.data, res.uid.len, &err), &err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&e->uid), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(e->db, &ch, &cr, &err), &err);
    T_OK(atlas_buf_set(&token, cr.token.data, cr.token.len, &err), &err);
    (void)snprintf(confirm, sizeof confirm, "%s", cr.confirm);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);

    atlas_decision_op ap;
    atlas_decision_op_init(&ap, ATLAS_DECISION_OP_APPROVE);
    T_OK(atlas_buf_set_str(&ap.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ap.uid, atlas_buf_cstr(&e->uid), &err), &err);
    T_OK(atlas_buf_set(&ap.token, token.data, token.len, &err), &err);
    T_OK(atlas_buf_set_str(&ap.confirmation, confirm, &err), &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    T_OK(atlas_decision_apply(e->db, &ap, &ar, &err), &err);
    T_CHECK(ar.state == ATLAS_DECISION_APPROVED);
    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_buf_free(&token);
}

static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->db_path);
    atlas_buf_init(&e->uid);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "core.c", "int core(void){return 1;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "init", &err), &err);
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
    run_cli(e, add, 5u, 0);
    const char *scan[] = {"scan", "proj"};
    run_cli(e, scan, 2u, 0);
    /* The structural index too. Without it `atlas_code_index_current` is false,
     * and a gate that cannot compute transitive impact must not answer FRESH —
     * so every assessment here would be UNKNOWN for a reason that has nothing
     * to do with revalidation. */
    const char *code[] = {"code", "sync", "proj"};
    run_cli(e, code, 3u, 0);
    db_open(e);
    env_approve(e, "core.c");
}

static void env_close(env *e) {
    if (e->db != NULL) {
        db_close(e);
    }
    atlas_buf_free(&e->uid);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* Makes the decision stale by changing the file it is bound to. */
static void make_stale(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    db_close(e);
    T_OK(fx_write(fx_repo(&e->fx), "core.c", "int core(void){return 2;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "change it", &err), &err);
    const char *scan[] = {"scan", "proj"};
    run_cli(e, scan, 2u, 0);
    const char *code[] = {"code", "sync", "proj"};
    run_cli(e, code, 3u, 0);
    db_open(e);
}

static atlas_gate_freshness freshness_now(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_gate_report rep;
    atlas_gate_report_init(&rep);
    T_OK(atlas_gate_run_one(e->db, "proj", atlas_buf_cstr(&e->uid), NULL, &rep, &err), &err);
    T_REQUIRE(rep.item_count == 1u);
    atlas_gate_freshness f = rep.items[0].freshness;
    atlas_gate_report_free(&rep);
    return f;
}

static int64_t count_of(env *e, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *st = NULL;
    T_OK(atlas_db_prepare(e->db, sql, &st, &err), &err);
    int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    atlas_db_finish(e->db, st);
    return n;
}

/* --- issuing and spending a revalidation capability -------------------------- */

typedef struct cap {
    atlas_buf token;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
} cap;

static void cap_free(cap *c) {
    atlas_buf_free(&c->token);
}

/* Issues one, carrying the assessment the operator would have been shown. */
static void issue(env *e, cap *out) {
    atlas_err err;
    atlas_err_init(&err);
    memset(out, 0, sizeof(*out));
    atlas_buf_init(&out->token);

    atlas_gate_report rep;
    atlas_gate_report_init(&rep);
    T_OK(atlas_gate_run_one(e->db, "proj", atlas_buf_cstr(&e->uid), NULL, &rep, &err), &err);
    T_REQUIRE(rep.item_count == 1u);

    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&e->uid), &err), &err);
    op.intent = ATLAS_DECISION_INTENT_REVALIDATE;
    T_OK(atlas_buf_set_str(&op.prior_freshness,
                           atlas_gate_freshness_name(rep.items[0].freshness), &err),
         &err);
    T_OK(atlas_gate_reasons_pack(&rep.items[0], &op.prior_reasons, &err), &err);
    atlas_gate_report_free(&rep);

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, &err), &err);
    T_OK(atlas_buf_set(&out->token, res.token.data, res.token.len, &err), &err);
    (void)snprintf(out->confirm, sizeof out->confirm, "%s", res.confirm);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

static atlas_status spend(env *e, const cap *c, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVALIDATE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&e->uid), err), err);
    T_OK(atlas_buf_set(&op.token, c->token.data, c->token.len, err), err);
    T_OK(atlas_buf_set_str(&op.confirmation, c->confirm, err), err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    atlas_status st = atlas_decision_apply(e->db, &op, &res, err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
    return st;
}

/* --- 12: revalidation succeeds and establishes a new validation point --------- */

static void test_revalidation_records_a_new_validation_point(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);
    T_CHECK(freshness_now(&e) == ATLAS_GATE_STALE);

    int64_t revisions_before = count_of(&e, "SELECT COUNT(*) FROM decision_revisions;");
    int64_t events_before = count_of(&e, "SELECT COUNT(*) FROM decision_events;");

    cap c;
    issue(&e, &c);
    T_OK(spend(&e, &c, &err), &err);
    cap_free(&c);

    /* Fresh again, and by a different route: the anchors still do not match
     * what the revision recorded, but the validation point has moved to the
     * state a human checked them against. */
    T_CHECK_MSG(freshness_now(&e) == ATLAS_GATE_FRESH,
                "revalidation must establish a new validation point");

    /* And nothing about the approval changed. */
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), revisions_before);
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_events;") == events_before,
                "revalidation is not a lifecycle transition and appends no ledger event");
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_documents WHERE current_status='APPROVED';"),
             1);
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 1);

    /* The previous assessment is preserved rather than replaced. A ledger that
     * said a decision was revalidated but not what was wrong with it would be a
     * record of the answer without the question. */
    T_EQ_INT(count_of(&e,
                      "SELECT COUNT(*) FROM decision_validations WHERE prior_freshness='STALE'"
                      " AND prior_reasons LIKE '%DIRECT_EVIDENCE_CHANGED%';"),
             1);
    T_EQ_INT(count_of(&e,
                      "SELECT COUNT(*) FROM decision_validations WHERE"
                      " actor='LOCAL_OPERATOR_CONFIRMED' AND intent='revalidate';"),
             1);
    env_close(&e);
}

static void test_revalidation_is_append_only(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    for (int i = 0; i < 2; i++) {
        cap c;
        issue(&e, &c);
        T_OK(spend(&e, &c, &err), &err);
        cap_free(&c);
    }
    /* Two acts, two records. The second does not overwrite the first, so the
     * history of who checked what and when survives. */
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 2);
    T_EQ_INT(count_of(&e, "SELECT COUNT(DISTINCT challenge_id) FROM decision_validations;"), 2);
    env_close(&e);
}

/* --- 13: replay, expiry, commit drift, evidence drift ------------------------- */

static void test_a_replayed_capability_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    cap c;
    issue(&e, &c);
    T_OK(spend(&e, &c, &err), &err);
    /* The same capability again. One use, and the check is the UPDATE's own
     * `AND consumed = 0` rather than a read followed by a write. */
    T_FAILS_WITH(spend(&e, &c, &err), ATLAS_ERR_INTEGRITY, &err);
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 1);
    cap_free(&c);
    env_close(&e);
}

static void test_an_expired_capability_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    cap c;
    issue(&e, &c);
    /* Expiry is a stored timestamp compared as text, so a test can age one
     * without waiting for it. */
    T_OK(atlas_db_exec_sql(e.db,
                           "UPDATE decision_challenges SET expires_at = '2000-01-01T00:00:00Z';",
                           &err),
         &err);
    T_FAILS_WITH(spend(&e, &c, &err), ATLAS_ERR_INTEGRITY, &err);
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    cap_free(&c);
    env_close(&e);
}

static void test_commit_drift_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    cap c;
    issue(&e, &c);
    /* The index moves between the prompt and the confirmation. What the
     * operator looked at is not the state that would be recorded, so the
     * capability is spent against nothing. */
    db_close(&e);
    T_OK(fx_write(fx_repo(&e.fx), "core.c", "int core(void){return 3;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "moved on", &err), &err);
    const char *scan[] = {"scan", "proj"};
    run_cli(&e, scan, 2u, 0);
    const char *code[] = {"code", "sync", "proj"};
    run_cli(&e, code, 3u, 0);
    db_open(&e);

    T_FAILS_WITH(spend(&e, &c, &err), ATLAS_ERR_INTEGRITY, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "index moved") != NULL,
                "the refusal must say what drifted: %s", atlas_err_msg(&err));
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    T_CHECK(freshness_now(&e) == ATLAS_GATE_STALE);
    cap_free(&c);
    env_close(&e);
}

static void test_evidence_drift_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    cap c;
    issue(&e, &c);
    /* The anchors resolve differently from when the capability was issued,
     * without the head moving: the file the decision is bound to has gone. The
     * evidence digest is what notices, and it is a database read — no git and
     * no filesystem, because this check runs on the writer thread. */
    T_OK(atlas_db_exec_sql(e.db, "UPDATE files SET deleted = 1;", &err), &err);
    T_FAILS_WITH(spend(&e, &c, &err), ATLAS_ERR_INTEGRITY, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "evidence") != NULL,
                "the refusal must say what drifted: %s", atlas_err_msg(&err));
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    cap_free(&c);
    env_close(&e);
}

static void test_a_capability_for_another_intent_cannot_revalidate(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    /* An approval capability. The intent is part of the bound tuple precisely
     * so that obtaining one kind cannot be turned into another by asking
     * differently. */
    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&e.uid), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(e.db, &ch, &cr, &err), &err);

    cap c;
    memset(&c, 0, sizeof c);
    atlas_buf_init(&c.token);
    T_OK(atlas_buf_set(&c.token, cr.token.data, cr.token.len, &err), &err);
    (void)snprintf(c.confirm, sizeof c.confirm, "%s", cr.confirm);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);

    T_FAILS_WITH(spend(&e, &c, &err), ATLAS_ERR_INTEGRITY, &err);
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    cap_free(&c);
    env_close(&e);
}

static void test_revalidation_without_a_capability_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    /* No token at all. This is the shape every AI-facing caller has: it can
     * name the operation and cannot produce the evidence that authorises it.
     * The refusal is at the write point, so no adapter can skip it. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVALIDATE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&e.uid), &err), &err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_FAILS_WITH(atlas_decision_apply(e.db, &op, &res, &err), ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    T_CHECK_MSG(freshness_now(&e) == ATLAS_GATE_STALE,
                "a failed revalidation must not have moved the validation point");

    /* And the operation is one the write point knows needs a capability, which
     * is what stops a new operation kind from defaulting into the
     * unauthenticated set. */
    T_CHECK(atlas_decision_op_needs_challenge(ATLAS_DECISION_OP_REVALIDATE));
    env_close(&e);
}

static void test_only_an_approved_revision_can_be_revalidated(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    /* A second document that was never approved. Revalidating a proposal would
     * mean recording that somebody checked something that was never policy. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.title, "Only ever proposed", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "Maybe.", &err), &err);
    op.revision.scope = ATLAS_DECISION_SCOPE_PATHS;
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
    atlas_buf other = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&other, res.uid.data, res.uid.len, &err), &err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&other), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_REVALIDATE;
    T_OK(atlas_buf_set_str(&ch.prior_freshness, "UNKNOWN", &err), &err);
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_FAILS_WITH(atlas_decision_apply(e.db, &ch, &cr, &err), ATLAS_ERR_USAGE, &err);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);

    atlas_buf_free(&other);
    env_close(&e);
}

static void test_a_capability_must_name_the_assessment_it_covers(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);

    /* Without one there is nothing to preserve, and a validation record that
     * cannot say what was wrong is a record of the answer without the
     * question. */
    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&e.uid), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_REVALIDATE;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_FAILS_WITH(atlas_decision_apply(e.db, &ch, &cr, &err), ATLAS_ERR_USAGE, &err);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);

    /* And a freshness word from outside the vocabulary is refused rather than
     * stored: a caller is not the authority on what an A6 verdict is. */
    atlas_decision_op ch2;
    atlas_decision_op_init(&ch2, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch2.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch2.uid, atlas_buf_cstr(&e.uid), &err), &err);
    ch2.intent = ATLAS_DECISION_INTENT_REVALIDATE;
    T_OK(atlas_buf_set_str(&ch2.prior_freshness, "TOTALLY_FINE", &err), &err);
    atlas_decision_result cr2;
    atlas_decision_result_init(&cr2);
    T_FAILS_WITH(atlas_decision_apply(e.db, &ch2, &cr2, &err), ATLAS_ERR_USAGE, &err);
    atlas_decision_result_free(&cr2);
    atlas_decision_op_free(&ch2);

    /* As is a reason code from outside it. */
    atlas_decision_op ch3;
    atlas_decision_op_init(&ch3, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch3.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch3.uid, atlas_buf_cstr(&e.uid), &err), &err);
    ch3.intent = ATLAS_DECISION_INTENT_REVALIDATE;
    T_OK(atlas_buf_set_str(&ch3.prior_freshness, "STALE", &err), &err);
    T_OK(atlas_buf_set_str(&ch3.prior_reasons, "DIRECT_EVIDENCE_CHANGED PLEASE_PASS", &err), &err);
    atlas_decision_result cr3;
    atlas_decision_result_init(&cr3);
    T_FAILS_WITH(atlas_decision_apply(e.db, &ch3, &cr3, &err), ATLAS_ERR_INTEGRITY, &err);
    atlas_decision_result_free(&cr3);
    atlas_decision_op_free(&ch3);

    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_challenges WHERE intent='revalidate';"),
             0);
    env_close(&e);
}

/* --- the interactive channel ---------------------------------------------------
 *
 * A real pseudo-terminal, because the requirement is that a terminal is needed
 * and a stubbed one would be testing the stub. */

typedef struct pty {
    int master;
    pid_t child;
} pty;

static atlas_status pty_spawn(env *e, const char *const *args, size_t nargs, pty *out,
                              atlas_err *err) {
    memset(out, 0, sizeof(*out));
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot allocate a pty: %s", strerror(errno));
    }
    char slave_name[128];
    if (ptsname_r(master, slave_name, sizeof slave_name) != 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "ptsname_r: %s", strerror(errno));
    }
    (void)fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "fork: %s", strerror(errno));
    }
    if (pid == 0) {
        (void)setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) {
            _exit(90);
        }
        (void)dup2(slave, 0);
        (void)dup2(slave, 1);
        (void)dup2(slave, 2);
        if (slave > 2) {
            (void)close(slave);
        }
        (void)close(master);
        const char *argv[24];
        size_t k = 0;
        argv[k++] = ATLAS_BIN;
        argv[k++] = "--data-dir";
        argv[k++] = fx_data_dir(&e->fx);
        for (size_t i = 0; i < nargs; i++) {
            argv[k++] = args[i];
        }
        argv[k] = NULL;
        static char path_env[] = "PATH=/usr/bin:/bin";
        static char lc_env[] = "LC_ALL=C";
        static char tz_env[] = "TZ=UTC";
        char *envp[] = {path_env, lc_env, tz_env, NULL};
        char *xargv[24];
        for (size_t i = 0; i <= k; i++) {
            /* execve does not modify argv; the cast is the POSIX prototype's
             * long-standing wart rather than a claim about mutability. */
            union { const char *c; char *m; } u;
            u.c = argv[i];
            xargv[i] = u.m;
        }
        execve(ATLAS_BIN, xargv, envp);
        _exit(91);
    }
    out->master = master;
    out->child = pid;
    return ATLAS_OK;
}

static bool pty_expect(pty *p, const char *needle, atlas_buf *transcript) {
    atlas_err err;
    atlas_err_init(&err);
    for (int spins = 0; spins < 2000; spins++) {
        char buf[512];
        ssize_t n = read(p->master, buf, sizeof buf);
        if (n > 0) {
            (void)atlas_buf_append(transcript, buf, (size_t)n, &err);
            if (strstr(atlas_buf_cstr(transcript), needle) != NULL) {
                return true;
            }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            return strstr(atlas_buf_cstr(transcript), needle) != NULL;
        }
    }
    return false;
}

static void pty_type(pty *p, const char *line) {
    (void)write(p->master, line, strlen(line));
    (void)write(p->master, "\n", 1u);
}

static int pty_wait(pty *p, atlas_buf *transcript) {
    atlas_err err;
    atlas_err_init(&err);
    for (;;) {
        char buf[512];
        ssize_t n = read(p->master, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        (void)atlas_buf_append(transcript, buf, (size_t)n, &err);
    }
    int status = 0;
    (void)waitpid(p->child, &status, 0);
    (void)close(p->master);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* **A7 inverted this test.**
 *
 * It asserted what the interactive revalidation prompt displayed, and typed the
 * confirmation from a program to complete it. That last part is what A7 acted
 * on: a program typing a confirmation into a pseudo-terminal it allocated is
 * exactly the adversary, and the prompt was therefore proving nothing about who
 * was revalidating. Revalidation now needs operator authority, which needs an
 * OS principal separate from the caller, which no unprivileged test has.
 *
 * What is asserted instead is that a pseudo-terminal does not produce a
 * revalidation, that no prompt appears, and that the stale assessment stays
 * stale — the last part being the one that matters, because a revalidation is
 * what would have made it look fresh again. */
static void test_a_pseudo_terminal_does_not_revalidate(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    make_stale(&e);
    db_close(&e);

    const char *args[] = {"decision", "revalidate", "proj", atlas_buf_cstr(&e.uid)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(&e, args, 4u, &p, &err) == ATLAS_OK);
    atlas_buf transcript = ATLAS_BUF_INIT;
    /* Typed blind: an adversary does not wait to be asked, and the
     * confirmation is a public prefix of the content hash rather than a
     * secret. */
    pty_type(&p, "0123456789abcdef");
    bool prompted = pty_expect(&p, "Type ", &transcript);
    int code = pty_wait(&p, &transcript);
    const char *t = atlas_buf_cstr(&transcript);

    T_CHECK_MSG(!prompted, "a locked profile displayed the revalidation prompt:\n%s", t);
    T_CHECK_MSG(code != 0, "revalidation from a pseudo-terminal succeeded:\n%s", t);
    T_CHECK_MSG(strstr(t, "locked in this Atlas profile") != NULL,
                "the refusal did not explain itself:\n%s", t);

    db_open(&e);
    /* Nothing recorded, and — the point of the phase — the assessment is still
     * STALE, so the operator is still told to look. */
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    /* No *revalidation* capability was minted. The one row present is the
     * approve-intent challenge `env_approve` spent to set the fixture up, so
     * counting all challenges would assert the wrong thing and would keep
     * asserting it if the refusal later started minting one. */
    T_EQ_INT(count_of(&e,
                      "SELECT COUNT(*) FROM decision_challenges WHERE intent = 'REVALIDATE';"),
             0);
    T_CHECK_MSG(freshness_now(&e) == ATLAS_GATE_STALE,
                "a refused revalidation must leave the decision stale");
    atlas_buf_free(&transcript);
    env_close(&e);
}

static void test_yes_cannot_revalidate_and_neither_can_a_pipe(void) {
    env e;
    env_open(&e);
    make_stale(&e);
    db_close(&e);

    /* `--yes` is refused rather than ignored: a confirmation that a flag can
     * assert is not a confirmation.
     *
     * A7 changed which refusal arrives first, and deliberately. The authority
     * probe runs before argument shape is examined, so this now exits
     * ATLAS_ERR_CONFIG — the profile is locked — rather than ATLAS_ERR_USAGE.
     * Ordering it that way means a locked profile never reports on the shape of
     * a request it was never going to perform, and never reaches the terminal
     * to say so. */
    const char *yes[] = {"decision", "revalidate", "proj", atlas_buf_cstr(&e.uid), "--yes"};
    run_cli(&e, yes, 5u, (int)ATLAS_ERR_CONFIG);

    /* And without a terminal at all — which is how every non-interactive
     * caller, including one driven by a model, arrives. `fx_atlas` points the
     * child's stdin at /dev/null by design. */
    const char *plain[] = {"decision", "revalidate", "proj", atlas_buf_cstr(&e.uid)};
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[8];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e.fx);
    for (size_t i = 0; i < 4; i++) {
        argv[k++] = plain[i];
    }
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(argv, k, &out, &errout, &code, &err), &err);
    T_CHECK_MSG(code != 0, "revalidate without a terminal must fail, exited %d", code);

    db_open(&e);
    T_EQ_INT(count_of(&e, "SELECT COUNT(*) FROM decision_validations;"), 0);
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"revalidation records a new validation point",
     test_revalidation_records_a_new_validation_point},
    {"revalidation is append-only", test_revalidation_is_append_only},
    {"a replayed capability is refused", test_a_replayed_capability_is_refused},
    {"an expired capability is refused", test_an_expired_capability_is_refused},
    {"commit drift is refused", test_commit_drift_is_refused},
    {"evidence drift is refused", test_evidence_drift_is_refused},
    {"a capability for another intent cannot revalidate",
     test_a_capability_for_another_intent_cannot_revalidate},
    {"revalidation without a capability is refused",
     test_revalidation_without_a_capability_is_refused},
    {"only an approved revision can be revalidated",
     test_only_an_approved_revision_can_be_revalidated},
    {"a capability must name the assessment it covers",
     test_a_capability_must_name_the_assessment_it_covers},
    {"a pseudo-terminal does not revalidate",
     test_a_pseudo_terminal_does_not_revalidate},
    {"--yes cannot revalidate and neither can a pipe",
     test_yes_cannot_revalidate_and_neither_can_a_pipe},
};

ATLAS_TEST_MAIN("gate_operator", TESTS)

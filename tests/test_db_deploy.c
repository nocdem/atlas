/* Atlas - A17 T1: the deploy write point, against a real database.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every case drives the four `atlas_db_deploy_*_in_tx` functions against an
 * isolated fixture with its own registered, scanned repository and real
 * orchestration rows built through the ordinary A8 writers -- no daemon, no
 * socket, no process, on `tests/test_orch_run.c`'s own shape.
 *
 * The lifecycle case proves, in order: proposing from a SUCCEEDED patch job's
 * stored `changes.patch` artifact reaches PROPOSED with the right digest and
 * size; a second proposal against the same repository is refused while one is
 * already in flight; confirming with the wrong prefix is refused and changes
 * nothing; confirming while an unrelated job is not terminal is refused and
 * names the count; confirming with the correct prefix reaches CONFIRMED and
 * writes a REMOTE_OPERATOR_CONFIRMED ledger row; finishing FAILED at stage
 * BUILD reaches FAILED; the same job may be proposed again after a FAILED
 * deploy; finishing SUCCEEDED forecloses a further proposal for that job;
 * cancel only succeeds from PROPOSED and only for the proposing credential.
 *
 * The two dictionary cases prove the SQL predicates this file's caller
 * (`src/db/db_deploy.c`) writes out by hand agree with their C vocabularies
 * over the whole of each: `idx_deploys_one_active`'s state set against
 * `atlas_deploy_state_is_terminal`, and `confirm_in_tx`'s non-terminal job
 * count against `atlas_orch_state_is_terminal` -- `test_orch_run.c`'s own
 * "two spellings of one rule drift" pattern, kept.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/deploy.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- the environment -------------------------------------------------------
 *
 * `tests/test_orch_run.c`'s `env`, unchanged: one repository registered and
 * scanned through the CLI, because the durable identity is a path-qualified
 * lineage fingerprint and there is no other way to obtain one that means
 * anything.
 */
typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_buf identity;
    atlas_buf commit;
} env;

static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);
    atlas_buf_init(&e->identity);
    atlas_buf_init(&e->commit);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx),  "--name",         "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    {
        const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "proj"};
        int code = -1;
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", &ri, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_db_repo_identity_hash(e->db, ri.id, &e->identity, &err), &err);
    T_OK(atlas_buf_set_str(&e->commit, ri.scanned_head[0] != '\0'
                                           ? ri.scanned_head
                                           : "0123456789abcdef0123456789abcdef01234567",
                           &err),
         &err);
    atlas_repo_info_free(&ri);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    e->db = NULL;
    atlas_buf_free(&e->db_path);
    atlas_buf_free(&e->identity);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

/* --- building and driving orchestration operations, on test_orch_run.c's own
 * shape --------------------------------------------------------------------- */

static atlas_orch_op *submit_op(env *e, const char *key) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = 1;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e->identity.data, e->identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, e->commit.data, e->commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, "add a comment", &err), &err);
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = 1;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    if (key != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.idempotency_key, key, &err), &err);
    }
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);
    return op;
}

static void apply_ok(env *e, atlas_orch_op *op, atlas_orch_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result_init(out);
    T_OK(atlas_orch_apply(e->db, op, out, &err), &err);
    atlas_orch_op_free(op);
    free(op);
}

static atlas_orch_op *worker_op(atlas_orch_op_kind kind, const char *token) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(kind);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->token, token, &err), &err);
    return op;
}

static atlas_orch_op *lease_op(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->dispatcher_id, "d1", &err), &err);
    return op;
}

static int64_t lease_once(env *e, atlas_buf *token_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result g;
    apply_ok(e, lease_op(), &g);
    T_REQUIRE(g.granted);
    if (token_out != NULL) {
        T_OK(atlas_buf_set(token_out, g.token.data, g.token.len, &err), &err);
    }
    int64_t n = g.attempt_no;
    atlas_orch_result_free(&g);
    return n;
}

static void advance_to_running(env *e, const char *token) {
    static const atlas_orch_state FORWARD[] = {ATLAS_ORCH_STATE_PREPARING,
                                               ATLAS_ORCH_STATE_RUNNING};
    for (size_t i = 0; i < sizeof(FORWARD) / sizeof(FORWARD[0]); i++) {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, token);
        op->phase = FORWARD[i];
        atlas_orch_result r;
        apply_ok(e, op, &r);
        atlas_orch_result_free(&r);
    }
}

/* Runs one queued job to SUCCEEDED with a stored `changes.patch` artifact --
 * `tests/test_orch_lifecycle.c`'s `test_a_patch_artifact_is_recorded_and_
 * never_applied` pattern: a real sha256 digest via `atlas_sha256_hex`, exactly
 * what `atlas_db_orch_artifacts` will hand back to `atlas_db_deploy_propose_
 * in_tx`. Returns the new job's uid and the patch digest. */
static void run_patch_job_to_success(env *e, const char *patch_content, atlas_buf *job_uid_out,
                                     char digest_out[ATLAS_SHA256_HEX_LEN + 1u]) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result s;
    apply_ok(e, submit_op(e, NULL), &s);
    T_OK(atlas_buf_set(job_uid_out, s.job_uid.data, s.job_uid.len, &err), &err);
    atlas_orch_result_free(&s);

    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease_once(e, &tok);
    advance_to_running(e, atlas_buf_cstr(&tok));

    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
    op->success = true;
    op->exit_kind = ATLAS_ORCH_EXIT_OK;
    T_OK(atlas_buf_set_str(&op->driver_version, "fake/1", &err), &err);
    op->artifacts = (atlas_orch_artifact *)calloc(1u, sizeof(atlas_orch_artifact));
    T_REQUIRE(op->artifacts != NULL);
    atlas_orch_artifact_init(&op->artifacts[0]);
    op->artifact_count = 1u;
    T_OK(atlas_buf_set_str(&op->artifacts[0].name, ATLAS_ORCH_RESULT_PATCH_NAME, &err), &err);
    T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "patch", &err), &err);
    size_t n = strlen(patch_content);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(patch_content, n, hex);
    T_OK(atlas_buf_set_str(&op->artifacts[0].sha256, hex, &err), &err);
    op->artifacts[0].size_bytes = (int64_t)n;
    op->artifacts[0].content_stored = true;
    T_OK(atlas_buf_set(&op->artifacts[0].content, patch_content, n, &err), &err);

    atlas_orch_result r;
    apply_ok(e, op, &r);
    T_CHECK(r.state == ATLAS_ORCH_STATE_SUCCEEDED);
    atlas_orch_result_free(&r);

    atlas_buf_free(&tok);
    if (digest_out != NULL) {
        (void)snprintf(digest_out, ATLAS_SHA256_HEX_LEN + 1u, "%s", hex);
    }
}

/* Submits a job and leaves it QUEUED (non-terminal), for the "confirm refuses
 * while a job is not terminal" case. */
static void submit_job_left_queued(env *e, atlas_buf *job_uid_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result s;
    apply_ok(e, submit_op(e, NULL), &s);
    T_OK(atlas_buf_set(job_uid_out, s.job_uid.data, s.job_uid.len, &err), &err);
    atlas_orch_result_free(&s);
}

/* Ends a QUEUED job at FAILED, so it stops counting as "not terminal". */
static void run_job_to_failure(env *e, const char *job_uid) {
    (void)job_uid;
    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease_once(e, &tok);
    advance_to_running(e, atlas_buf_cstr(&tok));
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
    op->success = false;
    op->exit_kind = ATLAS_ORCH_EXIT_NONZERO;
    op->failure_reason = ATLAS_ORCH_REASON_WORKER_FAILURE;
    atlas_orch_result r;
    apply_ok(e, op, &r);
    atlas_orch_result_free(&r);
    atlas_buf_free(&tok);
}

static void set_submit_key_id(env *e, const char *job_uid, const char *key_id) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "UPDATE orch_jobs SET submit_key_id = '%s' WHERE job_uid = '%s';",
                           key_id, job_uid),
         &err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), &err), &err);
    atlas_buf_free(&sql);
}

/* --- thin transaction wrappers around the four T1 writers ------------------- */

static atlas_status do_propose(env *e, const char *job_uid, const char *key_id,
                               atlas_buf *uid_out, atlas_err *err) {
    atlas_status s = atlas_db_begin(e->db, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_deploy_propose_in_tx(e->db, job_uid, key_id, uid_out, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
        return s;
    }
    s = atlas_db_commit(e->db, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
    }
    return s;
}

static atlas_status do_confirm(env *e, const char *deploy_uid, const char *key_id,
                               const char *confirmation, atlas_err *err) {
    atlas_status s = atlas_db_begin(e->db, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_deploy_confirm_in_tx(e->db, deploy_uid, key_id, confirmation, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
        return s;
    }
    s = atlas_db_commit(e->db, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
    }
    return s;
}

static atlas_status do_cancel(env *e, const char *deploy_uid, const char *key_id,
                              atlas_err *err) {
    atlas_status s = atlas_db_begin(e->db, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_deploy_cancel_in_tx(e->db, deploy_uid, key_id, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
        return s;
    }
    s = atlas_db_commit(e->db, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
    }
    return s;
}

static atlas_status do_mark_spooled(env *e, const char *deploy_uid, atlas_err *err) {
    atlas_status s = atlas_db_begin(e->db, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_deploy_mark_spooled_in_tx(e->db, deploy_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
        return s;
    }
    s = atlas_db_commit(e->db, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
    }
    return s;
}

static atlas_status do_finish(env *e, const char *deploy_uid, const atlas_deploy_result *r,
                              atlas_err *err) {
    atlas_status s = atlas_db_begin(e->db, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_deploy_finish_in_tx(e->db, deploy_uid, r, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
        return s;
    }
    s = atlas_db_commit(e->db, err);
    if (s != ATLAS_OK) {
        atlas_db_rollback(e->db);
    }
    return s;
}

static void get_ok(env *e, const char *deploy_uid, atlas_deploy_row *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_deploy_row_init(out);
    bool have = false;
    T_OK(atlas_db_deploy_get(e->db, deploy_uid, out, &have, &err), &err);
    T_CHECK(have);
}

static void last_transition(env *e, const char *deploy_uid, char *from_out, char *to_out,
                            char *actor_out, char *key_out, size_t n) {
    atlas_err err;
    atlas_err_init(&err);
    static const char SQL[] = "SELECT t.from_state, t.to_state, t.actor, t.key_id"
                              "  FROM deploy_transitions t JOIN deploys d ON d.id = t.deploy_id"
                              " WHERE d.deploy_uid = ?1 ORDER BY t.id DESC LIMIT 1;";
    sqlite3_stmt *st = NULL;
    T_OK(atlas_db_prepare(e->db, SQL, &st, &err), &err);
    (void)sqlite3_bind_text(st, 1, deploy_uid, -1, SQLITE_TRANSIENT);
    T_REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    (void)snprintf(from_out, n, "%s", atlas_db_col_text(st, 0));
    (void)snprintf(to_out, n, "%s", atlas_db_col_text(st, 1));
    (void)snprintf(actor_out, n, "%s", atlas_db_col_text(st, 2));
    (void)snprintf(key_out, n, "%s", atlas_db_col_text(st, 3));
    atlas_db_finish(e->db, st);
}

/* --- 1: the full lifecycle -------------------------------------------------- */

static void test_propose_confirm_finish_and_cancel_lifecycle(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* A job that does not exist is refused, not crashed on. */
    {
        atlas_buf uid = ATLAS_BUF_INIT;
        atlas_status st = do_propose(&e, "jnosuchjob00000000000000000000000a", "key0000000000001",
                                     &uid, &err);
        T_CHECK(st != ATLAS_OK);
        T_CHECK_MSG(strstr(err.msg, "no such job") != NULL, "got \"%s\"", err.msg);
        atlas_buf_free(&uid);
    }

    static const char PATCH1[] = "--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-old\n+new one\n";
    atlas_buf job1 = ATLAS_BUF_INIT;
    char digest1[ATLAS_SHA256_HEX_LEN + 1u];
    run_patch_job_to_success(&e, PATCH1, &job1, digest1);
    set_submit_key_id(&e, atlas_buf_cstr(&job1), "key0000000000001");

    /* --- propose reaches PROPOSED with the right digest and size --------- */
    atlas_buf deploy1 = ATLAS_BUF_INIT;
    T_OK(do_propose(&e, atlas_buf_cstr(&job1), "key0000000000001", &deploy1, &err), &err);
    T_CHECK(deploy1.len == 1u + ATLAS_DEPLOY_UID_HEX);
    T_CHECK(atlas_buf_cstr(&deploy1)[0] == 'd');

    atlas_deploy_row row;
    get_ok(&e, atlas_buf_cstr(&deploy1), &row);
    T_CHECK(row.state == ATLAS_DEPLOY_PROPOSED);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&row.patch_digest), digest1) == 0, "digest %s != %s",
                atlas_buf_cstr(&row.patch_digest), digest1);
    T_EQ_INT((int)row.patch_bytes, (int)strlen(PATCH1));
    T_CHECK(strcmp(atlas_buf_cstr(&row.proposed_key_id), "key0000000000001") == 0);
    T_EQ_INT((int)row.repo_id, 1);
    T_CHECK(strcmp(atlas_buf_cstr(&row.base_commit), atlas_buf_cstr(&e.commit)) == 0);
    atlas_deploy_row_free(&row);

    /* --- a second proposal in the same repository is refused ------------- */
    {
        atlas_buf uid2 = ATLAS_BUF_INIT;
        atlas_status st =
            do_propose(&e, atlas_buf_cstr(&job1), "key0000000000001", &uid2, &err);
        T_CHECK(st != ATLAS_OK);
        T_CHECK_MSG(strstr(err.msg, "a deploy is already in flight") != NULL, "got \"%s\"",
                    err.msg);
        atlas_buf_free(&uid2);
    }

    /* --- confirm with the wrong prefix is refused; the row is untouched -- */
    {
        atlas_status st = do_confirm(&e, atlas_buf_cstr(&deploy1), "key0000000000001",
                                     "00000000", &err);
        T_CHECK(st != ATLAS_OK);
        get_ok(&e, atlas_buf_cstr(&deploy1), &row);
        T_CHECK(row.state == ATLAS_DEPLOY_PROPOSED);
        atlas_deploy_row_free(&row);
    }
    char correct_confirmation[ATLAS_DEPLOY_CONFIRMATION_HEX + 1u];
    (void)snprintf(correct_confirmation, sizeof(correct_confirmation), "%.*s",
                   (int)ATLAS_DEPLOY_CONFIRMATION_HEX, digest1);

    /* --- confirm while an unrelated job is not terminal is refused ------- */
    atlas_buf job2 = ATLAS_BUF_INIT;
    submit_job_left_queued(&e, &job2);
    {
        atlas_status st = do_confirm(&e, atlas_buf_cstr(&deploy1), "key0000000000001",
                                     correct_confirmation, &err);
        T_CHECK(st != ATLAS_OK);
        T_CHECK_MSG(strstr(err.msg, "job(s) are not terminal") != NULL, "got \"%s\"", err.msg);
        get_ok(&e, atlas_buf_cstr(&deploy1), &row);
        T_CHECK(row.state == ATLAS_DEPLOY_PROPOSED);
        atlas_deploy_row_free(&row);
    }
    run_job_to_failure(&e, atlas_buf_cstr(&job2));

    /* --- confirm with the correct prefix reaches CONFIRMED --------------- */
    T_OK(do_confirm(&e, atlas_buf_cstr(&deploy1), "key0000000000001", correct_confirmation, &err),
         &err);
    get_ok(&e, atlas_buf_cstr(&deploy1), &row);
    T_CHECK(row.state == ATLAS_DEPLOY_CONFIRMED);
    T_CHECK(strcmp(atlas_buf_cstr(&row.confirmed_key_id), "key0000000000001") == 0);
    T_CHECK(row.confirmed_at.len > 0);
    atlas_deploy_row_free(&row);

    char from[32], to[32], actor[32], key_field[32];
    last_transition(&e, atlas_buf_cstr(&deploy1), from, to, actor, key_field, sizeof(from));
    T_CHECK(strcmp(from, "PROPOSED") == 0);
    T_CHECK(strcmp(to, "CONFIRMED") == 0);
    T_CHECK(strcmp(actor, "REMOTE_OPERATOR_CONFIRMED") == 0);
    T_CHECK(strcmp(key_field, "key0000000000001") == 0);

    /* --- finish FAILED at stage BUILD reaches FAILED ---------------------- */
    atlas_deploy_result res;
    memset(&res, 0, sizeof(res));
    res.success = false;
    res.stage = "BUILD";
    res.dry_run = false;
    res.rollback = "none";
    res.head_before = atlas_buf_cstr(&e.commit);
    res.head_after = atlas_buf_cstr(&e.commit);
    res.version = "atlas 99.0.0";
    res.text = "make: *** [all] Error 1";
    T_OK(do_finish(&e, atlas_buf_cstr(&deploy1), &res, &err), &err);
    get_ok(&e, atlas_buf_cstr(&deploy1), &row);
    T_CHECK(row.state == ATLAS_DEPLOY_FAILED);
    T_CHECK(strcmp(atlas_buf_cstr(&row.result_stage), "BUILD") == 0);
    T_CHECK(row.result_dry_run == false);
    T_CHECK(row.terminal_at.len > 0);
    atlas_deploy_row_free(&row);

    /* --- after FAILED, the same job may be proposed again ----------------- */
    atlas_buf deploy2 = ATLAS_BUF_INIT;
    T_OK(do_propose(&e, atlas_buf_cstr(&job1), "key0000000000001", &deploy2, &err), &err);
    get_ok(&e, atlas_buf_cstr(&deploy2), &row);
    T_CHECK(row.state == ATLAS_DEPLOY_PROPOSED);
    atlas_deploy_row_free(&row);

    /* --- bring the second deploy to SUCCEEDED ------------------------------ */
    T_OK(do_confirm(&e, atlas_buf_cstr(&deploy2), "key0000000000001", correct_confirmation, &err),
         &err);
    atlas_deploy_result res2;
    memset(&res2, 0, sizeof(res2));
    res2.success = true;
    res2.stage = "DONE";
    res2.rollback = "none";
    res2.head_before = atlas_buf_cstr(&e.commit);
    res2.head_after = atlas_buf_cstr(&e.commit);
    res2.version = "atlas 99.0.0";
    res2.text = "restarted cleanly";
    T_OK(do_finish(&e, atlas_buf_cstr(&deploy2), &res2, &err), &err);
    get_ok(&e, atlas_buf_cstr(&deploy2), &row);
    T_CHECK(row.state == ATLAS_DEPLOY_SUCCEEDED);
    atlas_deploy_row_free(&row);

    /* --- after SUCCEEDED, the same job may not be proposed again ---------- */
    {
        atlas_buf uid3 = ATLAS_BUF_INIT;
        atlas_status st =
            do_propose(&e, atlas_buf_cstr(&job1), "key0000000000001", &uid3, &err);
        T_CHECK(st != ATLAS_OK);
        T_CHECK_MSG(strstr(err.msg, "already has a") != NULL &&
                        strstr(err.msg, "SUCCEEDED") != NULL,
                    "got \"%s\"", err.msg);
        atlas_buf_free(&uid3);
    }

    /* --- cancel only from PROPOSED, only with the proposing key ----------- */
    static const char PATCH3[] = "--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-old\n+third\n";
    atlas_buf job3 = ATLAS_BUF_INIT;
    char digest3[ATLAS_SHA256_HEX_LEN + 1u];
    run_patch_job_to_success(&e, PATCH3, &job3, digest3);
    set_submit_key_id(&e, atlas_buf_cstr(&job3), "key0000000000001");
    atlas_buf deploy3 = ATLAS_BUF_INIT;
    T_OK(do_propose(&e, atlas_buf_cstr(&job3), "key0000000000001", &deploy3, &err), &err);

    {
        atlas_status st = do_cancel(&e, atlas_buf_cstr(&deploy3), "key0000000000099", &err);
        T_CHECK(st != ATLAS_OK);
        get_ok(&e, atlas_buf_cstr(&deploy3), &row);
        T_CHECK(row.state == ATLAS_DEPLOY_PROPOSED);
        atlas_deploy_row_free(&row);
    }
    T_OK(do_cancel(&e, atlas_buf_cstr(&deploy3), "key0000000000001", &err), &err);
    get_ok(&e, atlas_buf_cstr(&deploy3), &row);
    T_CHECK(row.state == ATLAS_DEPLOY_CANCELLED);
    atlas_deploy_row_free(&row);
    {
        atlas_status st = do_cancel(&e, atlas_buf_cstr(&deploy3), "key0000000000001", &err);
        T_CHECK(st != ATLAS_OK);
    }

    atlas_buf_free(&deploy3);
    atlas_buf_free(&job3);
    atlas_buf_free(&deploy2);
    atlas_buf_free(&deploy1);
    atlas_buf_free(&job2);
    atlas_buf_free(&job1);
    env_close(&e);
}

/* --- 2: the deploy-state terminal set, SQL against C ------------------------ */

/* `idx_deploys_one_active`'s `state IN ('PROPOSED','CONFIRMED')` is the SQL
 * spelling of "not terminal" for a deploy; `atlas_deploy_state_is_terminal`
 * is the C one. Compared over the whole vocabulary, `test_orch_run.c`'s
 * pattern. */
static void test_deploy_state_sql_terminal_set_matches_the_c_one(void) {
    env e;
    env_open(&e);

    static const atlas_deploy_state ALL[] = {ATLAS_DEPLOY_PROPOSED, ATLAS_DEPLOY_CONFIRMED,
                                             ATLAS_DEPLOY_SUCCEEDED, ATLAS_DEPLOY_FAILED,
                                             ATLAS_DEPLOY_CANCELLED};
    for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        const char *name = atlas_deploy_state_name(ALL[i]);
        atlas_err err;
        atlas_err_init(&err);
        atlas_buf sql = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&sql, &err,
                               "SELECT CASE WHEN '%s' IN ('PROPOSED','CONFIRMED') THEN 1 ELSE 0"
                               " END;",
                               name),
             &err);
        int64_t sql_active = -1;
        T_OK(atlas_db_query_int64(e.db, atlas_buf_cstr(&sql), &sql_active, &err), &err);
        bool c_active = !atlas_deploy_state_is_terminal(ALL[i]);
        T_CHECK_MSG((sql_active == 1) == c_active,
                    "%s is %s in C and %s in idx_deploys_one_active's predicate", name,
                    c_active ? "active" : "terminal", sql_active == 1 ? "active" : "terminal");
        atlas_buf_free(&sql);
    }

    env_close(&e);
}

/* --- 3: the job-state "not terminal" set confirm_in_tx uses, SQL against C -- */

/* `src/db/db_deploy.c`'s `DEPLOY_SQL_JOB_NOT_TERMINAL` macro is a second,
 * file-private copy of `src/db/db_orch.c`'s `ORCH_NOT_TERMINAL_SQL` -- both
 * exist because a deploy restarts the whole daemon, so `confirm_in_tx` counts
 * every non-terminal job, not only this deploy's own repository's. Compared
 * here against `atlas_orch_state_is_terminal` over the whole orchestration
 * job-state vocabulary. */
static void test_job_not_terminal_sql_matches_atlas_orch_state_is_terminal(void) {
    env e;
    env_open(&e);

    static const atlas_orch_state ALL[] = {
        ATLAS_ORCH_STATE_QUEUED,           ATLAS_ORCH_STATE_LEASED,
        ATLAS_ORCH_STATE_PREPARING,        ATLAS_ORCH_STATE_RUNNING,
        ATLAS_ORCH_STATE_VALIDATING,       ATLAS_ORCH_STATE_SUCCEEDED,
        ATLAS_ORCH_STATE_FAILED,           ATLAS_ORCH_STATE_CANCEL_REQUESTED,
        ATLAS_ORCH_STATE_CANCELLED,        ATLAS_ORCH_STATE_TIMED_OUT,
        ATLAS_ORCH_STATE_RECOVERY_REQUIRED};

    for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        const char *name = atlas_orch_state_name(ALL[i]);
        atlas_err err;
        atlas_err_init(&err);
        atlas_buf sql = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&sql, &err,
                               "SELECT CASE WHEN '%s' NOT IN"
                               " ('SUCCEEDED','FAILED','CANCELLED','TIMED_OUT',"
                               "  'RECOVERY_REQUIRED') THEN 1 ELSE 0 END;",
                               name),
             &err);
        int64_t sql_active = -1;
        T_OK(atlas_db_query_int64(e.db, atlas_buf_cstr(&sql), &sql_active, &err), &err);
        bool c_active = !atlas_orch_state_is_terminal(ALL[i]);
        T_CHECK_MSG((sql_active == 1) == c_active,
                    "%s is %s in C and %s in confirm_in_tx's job-not-terminal predicate", name,
                    c_active ? "active" : "terminal", sql_active == 1 ? "active" : "terminal");
        atlas_buf_free(&sql);
    }

    env_close(&e);
}

/* --- 3b: the same predicate, through the compiled path, not a second literal */

/* Test 3 above retypes `DEPLOY_SQL_JOB_NOT_TERMINAL` as a second string
 * literal -- the exact shape `tests/test_orch_run.c`'s own header warns two
 * spellings of one rule can drift apart from each other without either ever
 * being compared to what actually runs. This walks one probe job through
 * every `atlas_orch_state`, via a direct `UPDATE` exactly as `test_orch_run.c`
 * does, and for each drives a freshly proposed deploy through `do_confirm` --
 * `atlas_db_deploy_confirm_in_tx` itself, the compiled query, not a retyped
 * copy of it -- checking the outcome agrees with `atlas_orch_state_is_terminal`:
 * confirm must succeed when the probe is terminal and must refuse naming
 * "job(s) are not terminal" when it is not. Each iteration's deploy is then
 * cleared (finished when confirm succeeded, cancelled when it did not) so the
 * repository's one-active-deploy slot is free for the next state. */
static void test_confirm_in_tx_job_not_terminal_matches_atlas_orch_state_is_terminal(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* The probe's state is set with a raw `UPDATE`, never through a real
     * lease/heartbeat/complete sequence, so a job in an impossible-in-
     * practice state (e.g. CANCELLED reached with no lease ever granted) is
     * exactly as legitimate a vocabulary member here as any other: this
     * checks the *predicate*, not reachability. It is created once and set to
     * a harmless terminal placeholder before the loop starts, and reset back
     * to that placeholder at the end of every iteration -- never left QUEUED
     * except for the one iteration under test -- because `lease_once` below
     * (inside `run_patch_job_to_success`) leases whichever QUEUED job is
     * oldest with no way to name one: a probe left QUEUED at the wrong moment
     * would silently steal the lease meant for that iteration's own fresh
     * patch job, exactly as it did on this test's own first attempt. */
    atlas_buf probe = ATLAS_BUF_INIT;
    submit_job_left_queued(&e, &probe);
    static const char SET_PROBE_SAFE[] =
        "UPDATE orch_jobs SET state = 'SUCCEEDED' WHERE job_uid = ?1;";
    {
        sqlite3_stmt *reset_st = NULL;
        T_OK(atlas_db_prepare(e.db, SET_PROBE_SAFE, &reset_st, &err), &err);
        (void)sqlite3_bind_text(reset_st, 1, atlas_buf_cstr(&probe), -1, SQLITE_TRANSIENT);
        T_REQUIRE(sqlite3_step(reset_st) == SQLITE_DONE);
        atlas_db_finish(e.db, reset_st);
    }

    static const atlas_orch_state ALL[] = {
        ATLAS_ORCH_STATE_QUEUED,           ATLAS_ORCH_STATE_LEASED,
        ATLAS_ORCH_STATE_PREPARING,        ATLAS_ORCH_STATE_RUNNING,
        ATLAS_ORCH_STATE_VALIDATING,       ATLAS_ORCH_STATE_SUCCEEDED,
        ATLAS_ORCH_STATE_FAILED,           ATLAS_ORCH_STATE_CANCEL_REQUESTED,
        ATLAS_ORCH_STATE_CANCELLED,        ATLAS_ORCH_STATE_TIMED_OUT,
        ATLAS_ORCH_STATE_RECOVERY_REQUIRED};

    for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        const char *name = atlas_orch_state_name(ALL[i]);

        /* The fresh patch job is submitted and driven to SUCCEEDED *before*
         * the probe is moved to this iteration's target state, so the probe
         * is never QUEUED while `run_patch_job_to_success`'s own lease runs
         * -- see the comment above. */
        char patch[128];
        (void)snprintf(patch, sizeof patch,
                       "--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-old\n+iter %zu\n", i);
        atlas_buf job = ATLAS_BUF_INIT;
        char digest[ATLAS_SHA256_HEX_LEN + 1u];
        run_patch_job_to_success(&e, patch, &job, digest);
        set_submit_key_id(&e, atlas_buf_cstr(&job), "key0000000000009");

        atlas_buf upd = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&upd, &err,
                               "UPDATE orch_jobs SET state = '%s' WHERE job_uid = '%s';", name,
                               atlas_buf_cstr(&probe)),
             &err);
        T_OK(atlas_db_exec_sql(e.db, atlas_buf_cstr(&upd), &err), &err);
        atlas_buf_free(&upd);

        atlas_buf deploy = ATLAS_BUF_INIT;
        T_OK(do_propose(&e, atlas_buf_cstr(&job), "key0000000000009", &deploy, &err), &err);

        char confirmation[ATLAS_DEPLOY_CONFIRMATION_HEX + 1u];
        (void)snprintf(confirmation, sizeof(confirmation), "%.*s",
                       (int)ATLAS_DEPLOY_CONFIRMATION_HEX, digest);

        bool c_active = !atlas_orch_state_is_terminal(ALL[i]);
        atlas_status st =
            do_confirm(&e, atlas_buf_cstr(&deploy), "key0000000000009", confirmation, &err);

        if (c_active) {
            T_CHECK_MSG(st != ATLAS_OK && strstr(err.msg, "job(s) are not terminal") != NULL,
                        "%s: expected a not-terminal refusal from confirm_in_tx, got %s", name,
                        st == ATLAS_OK ? "success" : err.msg);
            T_OK(do_cancel(&e, atlas_buf_cstr(&deploy), "key0000000000009", &err), &err);
        } else {
            T_CHECK_MSG(st == ATLAS_OK,
                        "%s: confirm_in_tx refused (%s) but the probe job is terminal", name,
                        err.msg);
            atlas_deploy_result res;
            memset(&res, 0, sizeof(res));
            res.success = true;
            res.stage = "DONE";
            res.rollback = "none";
            res.head_before = atlas_buf_cstr(&e.commit);
            res.head_after = atlas_buf_cstr(&e.commit);
            res.version = "atlas 99.0.0";
            res.text = "ok";
            T_OK(do_finish(&e, atlas_buf_cstr(&deploy), &res, &err), &err);
        }

        /* Reset the probe back to the safe placeholder before the next
         * iteration's `run_patch_job_to_success` runs its own lease -- see
         * the comment above the loop. Skipped when this iteration's own
         * target already was the placeholder value (SUCCEEDED), which is
         * harmless either way. */
        {
            sqlite3_stmt *reset_st = NULL;
            T_OK(atlas_db_prepare(e.db, SET_PROBE_SAFE, &reset_st, &err), &err);
            (void)sqlite3_bind_text(reset_st, 1, atlas_buf_cstr(&probe), -1, SQLITE_TRANSIENT);
            T_REQUIRE(sqlite3_step(reset_st) == SQLITE_DONE);
            atlas_db_finish(e.db, reset_st);
        }

        atlas_buf_free(&deploy);
        atlas_buf_free(&job);
    }

    atlas_buf_free(&probe);
    env_close(&e);
}

/* --- 4: confirmed_uid, confirmed_unspooled, mark_spooled and list ---------- */

typedef struct list_collect {
    int64_t count;
} list_collect;

static atlas_status list_count_cb(const atlas_deploy_list_row *row, void *ud, atlas_err *err) {
    (void)row;
    (void)err;
    ((list_collect *)ud)->count++;
    return ATLAS_OK;
}

static int64_t list_count(env *e, const char *key_id_or_null) {
    atlas_err err;
    atlas_err_init(&err);
    list_collect lc = {0};
    int64_t count_out = 0, cursor_out = 0;
    bool more = false;
    T_OK(atlas_db_deploy_list(e->db, key_id_or_null, 0, ATLAS_DEPLOY_LIST_MAX, list_count_cb, &lc,
                              &count_out, &cursor_out, &more, &err),
         &err);
    T_EQ_INT((int)count_out, (int)lc.count);
    return lc.count;
}

/* The four surfaces T2 calls that the lifecycle case above never exercises:
 * `confirmed_uid` (asked inside both submit write points, so a bug here
 * refuses every job submission while nothing is even CONFIRMED),
 * `confirmed_unspooled` (the daemon startup sweep's own query),
 * `mark_spooled_in_tx` (its CAS and its deliberate absence of a ledger row),
 * and `list` (both the unscoped operator view and the per-credential one). */
static void test_confirmed_uid_unspooled_mark_spooled_and_list(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Before anything is proposed, confirmed_uid is empty and both lists are
     * empty. */
    atlas_buf uid_or_empty = ATLAS_BUF_INIT;
    T_OK(atlas_db_deploy_confirmed_uid(e.db, 1, &uid_or_empty, &err), &err);
    T_CHECK(uid_or_empty.len == 0);
    T_EQ_INT((int)list_count(&e, NULL), 0);

    static const char PATCH[] = "--- a/a.c\n+++ b/a.c\n@@ -1 +1 @@\n-old\n+confirmed-uid-test\n";
    atlas_buf job = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    run_patch_job_to_success(&e, PATCH, &job, digest);
    set_submit_key_id(&e, atlas_buf_cstr(&job), "key0000000000002");

    atlas_buf deploy = ATLAS_BUF_INIT;
    T_OK(do_propose(&e, atlas_buf_cstr(&job), "key0000000000002", &deploy, &err), &err);

    /* PROPOSED does not count: only a CONFIRMED deploy blocks a root
     * submission, so confirmed_uid is still empty. */
    T_OK(atlas_db_deploy_confirmed_uid(e.db, 1, &uid_or_empty, &err), &err);
    T_CHECK(uid_or_empty.len == 0);

    /* Both lists see the PROPOSED deploy: the unscoped one, and the one
     * scoped to the proposing credential; a different credential's view is
     * empty. */
    T_EQ_INT((int)list_count(&e, NULL), 1);
    T_EQ_INT((int)list_count(&e, "key0000000000002"), 1);
    T_EQ_INT((int)list_count(&e, "key0000000000099"), 0);

    char confirmation[ATLAS_DEPLOY_CONFIRMATION_HEX + 1u];
    (void)snprintf(confirmation, sizeof(confirmation), "%.*s", (int)ATLAS_DEPLOY_CONFIRMATION_HEX,
                   digest);
    T_OK(do_confirm(&e, atlas_buf_cstr(&deploy), "key0000000000002", confirmation, &err), &err);

    /* Now CONFIRMED: confirmed_uid names it, and it is owed a spool write. */
    T_OK(atlas_db_deploy_confirmed_uid(e.db, 1, &uid_or_empty, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&uid_or_empty), atlas_buf_cstr(&deploy)) == 0,
                "confirmed_uid returned \"%s\", expected \"%s\"", atlas_buf_cstr(&uid_or_empty),
                atlas_buf_cstr(&deploy));

    atlas_buf unspooled = ATLAS_BUF_INIT;
    T_OK(atlas_db_deploy_confirmed_unspooled(e.db, &unspooled, &err), &err);
    T_CHECK(unspooled.len == 1u + ATLAS_DEPLOY_UID_HEX);
    T_CHECK_MSG(strncmp(atlas_buf_cstr(&unspooled), atlas_buf_cstr(&deploy),
                        1u + ATLAS_DEPLOY_UID_HEX) == 0,
                "confirmed_unspooled returned \"%s\", expected \"%s\"",
                atlas_buf_cstr(&unspooled), atlas_buf_cstr(&deploy));

    T_OK(do_mark_spooled(&e, atlas_buf_cstr(&deploy), &err), &err);

    /* Spooled: confirmed_unspooled no longer names it, and marking it spooled
     * a second time is refused rather than silently repeated. */
    T_OK(atlas_db_deploy_confirmed_unspooled(e.db, &unspooled, &err), &err);
    T_CHECK(unspooled.len == 0);
    {
        atlas_status st = do_mark_spooled(&e, atlas_buf_cstr(&deploy), &err);
        T_CHECK(st != ATLAS_OK);
    }

    /* Still CONFIRMED (spooling is not a state change), so it still blocks a
     * root submission and both lists still see it. */
    T_OK(atlas_db_deploy_confirmed_uid(e.db, 1, &uid_or_empty, &err), &err);
    T_CHECK(strcmp(atlas_buf_cstr(&uid_or_empty), atlas_buf_cstr(&deploy)) == 0);
    T_EQ_INT((int)list_count(&e, NULL), 1);
    T_EQ_INT((int)list_count(&e, "key0000000000002"), 1);

    /* And no `deploy_transitions` row was written for the spool event -- the
     * last transition on this deploy is still PROPOSED -> CONFIRMED. */
    char from[32], to[32], actor[32], key_field[32];
    last_transition(&e, atlas_buf_cstr(&deploy), from, to, actor, key_field, sizeof(from));
    T_CHECK(strcmp(from, "PROPOSED") == 0);
    T_CHECK(strcmp(to, "CONFIRMED") == 0);

    atlas_buf_free(&unspooled);
    atlas_buf_free(&uid_or_empty);
    atlas_buf_free(&deploy);
    atlas_buf_free(&job);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"propose reaches PROPOSED, confirm reaches CONFIRMED, finish and cancel behave, and a "
     "SUCCEEDED deploy forecloses a further proposal",
     test_propose_confirm_finish_and_cancel_lifecycle},
    {"confirmed_uid, confirmed_unspooled, mark_spooled and list agree at every step",
     test_confirmed_uid_unspooled_mark_spooled_and_list},
    {"the deploy-state terminal set in idx_deploys_one_active matches "
     "atlas_deploy_state_is_terminal",
     test_deploy_state_sql_terminal_set_matches_the_c_one},
    {"confirm_in_tx's non-terminal job count matches atlas_orch_state_is_terminal",
     test_job_not_terminal_sql_matches_atlas_orch_state_is_terminal},
    {"confirm_in_tx itself (not a retyped literal) agrees with "
     "atlas_orch_state_is_terminal over the whole job-state vocabulary",
     test_confirm_in_tx_job_not_terminal_matches_atlas_orch_state_is_terminal},
};

ATLAS_TEST_MAIN("db_deploy", TESTS)

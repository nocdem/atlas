/* Atlas - A11.0: the durable single-worker run, against a real database.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A8 shipped `orch_jobs.parent_job_uid` and resolved it nowhere. The column was
 * checked for shape at submission — 'j' plus 32 lowercase hex — and nothing
 * asked whether the parent existed, described the same repository, or already
 * had something following it. A chain of tasks was expressible and not
 * enforceable, and this file is the evidence for the half A11.0 added.
 *
 * Every case drives `atlas_orch_apply` — the one write point — against an
 * isolated fixture with its own registered repository. Nothing touches a live
 * service, a socket, the real index or a registered repository.
 *
 * What is deliberately not here: no worker is started, no driver runs, no
 * follow-up task is generated automatically, and nothing settles a run except a
 * test that calls the setter itself. A11.0 builds the chain A11.1 will use and
 * makes no decision on it.
 *
 * The six contract cases, in order: a root task created and read back; a child
 * with the right parent and attempt chain; a duplicate retry creating no second
 * task; the same active state recovered after a restart; a terminal run
 * refusing a child; and a second active task in one run refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/orch_ops.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- the environment ------------------------------------------------------
 *
 * Registered through the CLI, because that is the only way a repository is ever
 * registered, and scanned, because the durable identity is a path-qualified
 * lineage fingerprint whose lineage half comes from ingested root commits. */
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

/* Closes and reopens the database against the same file, which is what a daemon
 * restart looks like to every row underneath it. Nothing is carried across in
 * memory: the handle, its statement cache and every cached row are gone. */
static void env_restart(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_db_close(e->db);
    e->db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
}

/* --- building operations ------------------------------------------------- */

static atlas_orch_op *submit_op(env *e, const char *key, const char *parent, int64_t attempts) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    /* The submitter comes from the trusted connection at the IPC edge. There is
     * no path by which a request body reaches it, here or in the daemon. */
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
    op->spec.max_attempts = attempts;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    if (key != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.idempotency_key, key, &err), &err);
    }
    if (parent != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.parent_job_uid, parent, &err), &err);
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

/* Applies expecting refusal, and requires the message to name the thing that
 * refused. A refusal whose reason is unreadable sends an operator looking in the
 * wrong place, which is most of what these cases exist to prevent. */
static void apply_refused(env *e, atlas_orch_op *op, const char *what, const char *expect) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    atlas_status st = atlas_orch_apply(e->db, op, &r, &err);
    T_CHECK_MSG(st != ATLAS_OK, "%s was accepted", what);
    if (st != ATLAS_OK && expect != NULL) {
        T_CHECK_MSG(strstr(err.msg, expect) != NULL, "%s was refused with \"%s\", which does "
                                                         "not mention \"%s\"",
                    what, err.msg, expect);
    }
    atlas_orch_result_free(&r);
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

static int64_t count_sql(atlas_db *db, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t v = -1;
    T_OK(atlas_db_query_int64(db, sql, &v, &err), &err);
    return v;
}

/* Leases the one queued job and returns the attempt number it was given. The
 * token is copied out because the result is freed here. */
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

/* A job may not succeed straight out of LEASED — the transition table has no
 * such edge — so every success goes through the phases a real dispatcher walks. */
static void advance_to_running(env *e, const char *token) {
    static const atlas_orch_state FORWARD[] = {ATLAS_ORCH_STATE_PREPARING,
                                               ATLAS_ORCH_STATE_RUNNING};
    for (size_t i = 0; i < sizeof FORWARD / sizeof FORWARD[0]; i++) {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, token);
        op->phase = FORWARD[i];
        atlas_orch_result r;
        apply_ok(e, op, &r);
        atlas_orch_result_free(&r);
    }
}

static void finish(env *e, const char *token, bool success) {
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, token);
    op->success = success;
    atlas_orch_result r;
    apply_ok(e, op, &r);
    atlas_orch_result_free(&r);
}

/* Runs the one queued job to a terminal SUCCEEDED. */
static void run_to_success(env *e) {
    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease_once(e, &tok);
    advance_to_running(e, atlas_buf_cstr(&tok));
    finish(e, atlas_buf_cstr(&tok), true);
    atlas_buf_free(&tok);
}

/* --- 1: a root task, created and read back ------------------------------- */

static void test_a_root_task_creates_its_run_and_reads_back(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_result r;
    apply_ok(&e, submit_op(&e, NULL, NULL, 2), &r);

    /* The run is reported by the submission itself, not discovered afterwards. */
    T_CHECK(r.run_uid.len == ATLAS_ORCH_RUN_UID_HEX + 1u);
    T_CHECK(r.run_uid.data[0] == 'r');
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 1);

    atlas_orch_run_view rv;
    bool found = false;
    T_OK(atlas_db_orch_run_get(e.db, atlas_buf_cstr(&r.run_uid), &rv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(rv.status == ATLAS_ORCH_RUN_ACTIVE);
    /* The run's root is this task, and its active task is this task: a run of
     * one is both, and the two fields are not the same field. */
    T_CHECK(strcmp(rv.root_job_uid, atlas_buf_cstr(&r.job_uid)) == 0);
    T_CHECK(strcmp(rv.active_job_uid, atlas_buf_cstr(&r.job_uid)) == 0);
    T_CHECK(rv.active_state == ATLAS_ORCH_STATE_QUEUED);
    T_CHECK(strcmp(rv.repo_identity_hash, atlas_buf_cstr(&e.identity)) == 0);

    /* And the task knows its run, with no parent. */
    atlas_orch_job_view jv;
    atlas_orch_job_view_init(&jv);
    found = false;
    T_OK(atlas_db_orch_job_get(e.db, atlas_buf_cstr(&r.job_uid), &jv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(strcmp(jv.run_uid, atlas_buf_cstr(&r.run_uid)) == 0);
    T_CHECK(jv.parent_job_uid[0] == '\0');
    T_CHECK(jv.state == ATLAS_ORCH_STATE_QUEUED);
    atlas_orch_job_view_free(&jv);

    /* Nothing was dispatched, and no attempt exists before one is leased. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 0);

    atlas_orch_result_free(&r);
    env_close(&e);
}

/* --- 2: a child, its parent, and the attempt chain ----------------------- */

static void test_a_child_joins_its_parents_run_with_a_deterministic_attempt_chain(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_result root;
    apply_ok(&e, submit_op(&e, NULL, NULL, 2), &root);

    /* Attempt numbering is deterministic and per task: the first lease is 1,
     * and after a failure that leaves attempts remaining, the next is 2. It is
     * never derived from a clock or from how many attempts other tasks had. */
    atlas_buf tok = ATLAS_BUF_INIT;
    T_EQ_INT((int)lease_once(&e, &tok), 1);
    advance_to_running(&e, atlas_buf_cstr(&tok));
    finish(&e, atlas_buf_cstr(&tok), false);
    atlas_buf_free(&tok);

    T_EQ_INT((int)lease_once(&e, &tok), 2);
    advance_to_running(&e, atlas_buf_cstr(&tok));
    finish(&e, atlas_buf_cstr(&tok), true);
    atlas_buf_free(&tok);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 2);

    /* The root is terminal, so the run has room for what follows it. */
    atlas_orch_result child;
    apply_ok(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1), &child);

    /* Same run, and a second run was not created. */
    T_CHECK(strcmp(atlas_buf_cstr(&child.run_uid), atlas_buf_cstr(&root.run_uid)) == 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 1);

    atlas_orch_job_view jv;
    atlas_orch_job_view_init(&jv);
    bool found = false;
    T_OK(atlas_db_orch_job_get(e.db, atlas_buf_cstr(&child.job_uid), &jv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(strcmp(jv.parent_job_uid, atlas_buf_cstr(&root.job_uid)) == 0);
    T_CHECK(strcmp(jv.run_uid, atlas_buf_cstr(&root.run_uid)) == 0);
    atlas_orch_job_view_free(&jv);

    /* The child's own attempts start at 1. A task's attempt number counts its
     * own attempts and inherits nothing from its parent. */
    T_EQ_INT((int)lease_once(&e, NULL), 1);

    /* The run's root did not move to the child. A run has exactly one root. */
    atlas_orch_run_view rv;
    found = false;
    T_OK(atlas_db_orch_run_get(e.db, atlas_buf_cstr(&root.run_uid), &rv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(strcmp(rv.root_job_uid, atlas_buf_cstr(&root.job_uid)) == 0);
    T_CHECK(strcmp(rv.active_job_uid, atlas_buf_cstr(&child.job_uid)) == 0);

    atlas_orch_result_free(&child);
    atlas_orch_result_free(&root);
    env_close(&e);
}

/* A parent must be a task Atlas has, in the repository being submitted against.
 * A8 checked the shape of this field and nothing else. */
static void test_a_parent_that_does_not_resolve_is_refused(void) {
    env e;
    env_open(&e);

    /* Well formed — 'j' and 32 lowercase hex — and nothing. Under A8 this was
     * accepted and stored. */
    apply_refused(&e, submit_op(&e, NULL, "j00000000000000000000000000000000", 1),
                  "a child of a job that does not exist", "no job named");

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 0);
    env_close(&e);
}

/* --- 3: a duplicate retry creates no second task ------------------------- */

static void test_a_duplicate_retry_creates_no_second_task_or_run(void) {
    env e;
    env_open(&e);

    atlas_orch_result a, b;
    apply_ok(&e, submit_op(&e, "k1", NULL, 2), &a);
    T_CHECK(!a.duplicate);

    /* The same request again — the shape a client takes when it did not learn
     * whether the first one landed, which is exactly what an A9.2.6 `BUSY:`
     * leaves it not knowing. `BUSY:` says nothing was queued, so the retry is
     * the first submission that lands; if instead the first one did land, this
     * is the path that must not fork it. Both readings end here. */
    apply_ok(&e, submit_op(&e, "k1", NULL, 2), &b);
    T_CHECK(b.duplicate);
    T_CHECK(strcmp(atlas_buf_cstr(&a.job_uid), atlas_buf_cstr(&b.job_uid)) == 0);
    /* And the run it reports is the run the first submission settled, not a new
     * one and not an empty string. */
    T_CHECK(strcmp(atlas_buf_cstr(&a.run_uid), atlas_buf_cstr(&b.run_uid)) == 0);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 1);

    /* The same holds for a child: a retried follow-up resolves to the child
     * that exists rather than becoming a second one, which is what would
     * otherwise put two tasks in one run. */
    run_to_success(&e);
    atlas_orch_result c1, c2;
    apply_ok(&e, submit_op(&e, "k2", atlas_buf_cstr(&a.job_uid), 1), &c1);
    apply_ok(&e, submit_op(&e, "k2", atlas_buf_cstr(&a.job_uid), 1), &c2);
    T_CHECK(c2.duplicate);
    T_CHECK(strcmp(atlas_buf_cstr(&c1.job_uid), atlas_buf_cstr(&c2.job_uid)) == 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 2);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 1);

    atlas_orch_result_free(&c2);
    atlas_orch_result_free(&c1);
    atlas_orch_result_free(&b);
    atlas_orch_result_free(&a);
    env_close(&e);
}

/* --- 4: the same active state after a restart ---------------------------- */

static void test_a_restart_recovers_the_run_its_active_task_and_the_parent_chain(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_result root;
    apply_ok(&e, submit_op(&e, NULL, NULL, 1), &root);
    run_to_success(&e);
    atlas_orch_result child;
    apply_ok(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1), &child);
    /* Leave the child mid-flight, which is the state a restart is interesting
     * in: a queued task tells you nothing a fresh database would not. */
    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease_once(&e, &tok);
    advance_to_running(&e, atlas_buf_cstr(&tok));
    atlas_buf_free(&tok);

    env_restart(&e);

    /* Read by a handle that did not accept any of it. */
    atlas_orch_run_view rv;
    bool found = false;
    T_OK(atlas_db_orch_run_get(e.db, atlas_buf_cstr(&root.run_uid), &rv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(rv.status == ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(strcmp(rv.root_job_uid, atlas_buf_cstr(&root.job_uid)) == 0);
    /* The active task is the child, and it is still where it was. */
    T_CHECK(strcmp(rv.active_job_uid, atlas_buf_cstr(&child.job_uid)) == 0);
    T_CHECK(rv.active_state == ATLAS_ORCH_STATE_RUNNING);

    /* The parent chain is read back, not reconstructed. */
    atlas_orch_job_view jv;
    atlas_orch_job_view_init(&jv);
    found = false;
    T_OK(atlas_db_orch_job_get(e.db, atlas_buf_cstr(&child.job_uid), &jv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(strcmp(jv.parent_job_uid, atlas_buf_cstr(&root.job_uid)) == 0);
    T_CHECK(strcmp(jv.run_uid, atlas_buf_cstr(&root.run_uid)) == 0);
    T_CHECK(jv.state == ATLAS_ORCH_STATE_RUNNING);
    atlas_orch_job_view_free(&jv);

    atlas_orch_result_free(&child);
    atlas_orch_result_free(&root);
    env_close(&e);
}

/* --- 5: a terminal run takes no child ------------------------------------ */

static void test_a_terminal_run_takes_no_further_task(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_result root;
    apply_ok(&e, submit_op(&e, NULL, NULL, 1), &root);
    run_to_success(&e);

    /* A succeeding task does not settle its run. The two are different claims
     * and A11.0 derives neither from the other, so the run is still ACTIVE and
     * would still take a child at this point. */
    atlas_orch_run_view rv;
    bool found = false;
    T_OK(atlas_db_orch_run_get(e.db, atlas_buf_cstr(&root.run_uid), &rv, &found, &err), &err);
    T_REQUIRE(found);
    T_CHECK(rv.status == ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(rv.active_job_uid[0] == '\0');

    T_OK(atlas_db_orch_run_set_status(e.db, atlas_buf_cstr(&root.run_uid),
                                      ATLAS_ORCH_RUN_ACTIVE, ATLAS_ORCH_RUN_ACCEPTED, &err),
         &err);

    apply_refused(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1),
                  "a child of an ACCEPTED run", "ACCEPTED");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);

    /* Terminal is final in both directions: it cannot be reopened, and it
     * cannot be re-settled as the other answer. */
    atlas_err_init(&err);
    T_CHECK(atlas_db_orch_run_set_status(e.db, atlas_buf_cstr(&root.run_uid),
                                         ATLAS_ORCH_RUN_ACCEPTED, ATLAS_ORCH_RUN_BLOCKED,
                                         &err) != ATLAS_OK);
    atlas_err_init(&err);
    T_CHECK(atlas_db_orch_run_set_status(e.db, atlas_buf_cstr(&root.run_uid),
                                         ATLAS_ORCH_RUN_ACTIVE, ATLAS_ORCH_RUN_ACCEPTED,
                                         &err) != ATLAS_OK);

    /* A BLOCKED run refuses a child for the same reason. */
    atlas_err_init(&err);
    atlas_orch_result other;
    apply_ok(&e, submit_op(&e, NULL, NULL, 1), &other);
    run_to_success(&e);
    T_OK(atlas_db_orch_run_set_status(e.db, atlas_buf_cstr(&other.run_uid),
                                      ATLAS_ORCH_RUN_ACTIVE, ATLAS_ORCH_RUN_BLOCKED, &err),
         &err);
    apply_refused(&e, submit_op(&e, NULL, atlas_buf_cstr(&other.job_uid), 1),
                  "a child of a BLOCKED run", "BLOCKED");

    atlas_orch_result_free(&other);
    atlas_orch_result_free(&root);
    env_close(&e);
}

/* --- 6: one active task per run ------------------------------------------ */

static void test_a_run_takes_no_second_active_task(void) {
    env e;
    env_open(&e);

    atlas_orch_result root;
    apply_ok(&e, submit_op(&e, NULL, NULL, 2), &root);

    /* The root is QUEUED — not started, not leased, and not terminal. A queued
     * task is an active one: work that has been accepted and not finished. */
    apply_refused(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1),
                  "a second task in a run whose root is QUEUED", "already has an active task");

    /* Still true once it is actually running. */
    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease_once(&e, &tok);
    advance_to_running(&e, atlas_buf_cstr(&tok));
    apply_refused(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1),
                  "a second task in a run whose root is RUNNING", "already has an active task");

    /* And through CANCEL_REQUESTED, which is deliberately not terminal: a task
     * that has been asked to stop has not stopped, and a run that admitted a
     * second task at that moment would have two running at once. */
    {
        atlas_err err;
        atlas_err_init(&err);
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        op->peer_uid = 1000;
        op->actor = ATLAS_ORCH_ACTOR_CLIENT;
        T_OK(atlas_buf_set(&op->job_uid, root.job_uid.data, root.job_uid.len, &err), &err);
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == ATLAS_ORCH_STATE_CANCEL_REQUESTED);
        atlas_orch_result_free(&r);
    }
    apply_refused(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1),
                  "a second task in a run whose root is CANCEL_REQUESTED",
                  "already has an active task");

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);

    /* Once the cancellation is confirmed the run is free again. */
    finish(&e, atlas_buf_cstr(&tok), false);
    atlas_buf_free(&tok);
    atlas_orch_result child;
    apply_ok(&e, submit_op(&e, NULL, atlas_buf_cstr(&root.job_uid), 1), &child);
    T_CHECK(strcmp(atlas_buf_cstr(&child.run_uid), atlas_buf_cstr(&root.run_uid)) == 0);

    atlas_orch_result_free(&child);
    atlas_orch_result_free(&root);
    env_close(&e);
}

/* --- the two spellings of "terminal" ------------------------------------- */

/* The partial unique index that makes "one active task per run" a schema fact
 * spells the terminal set out in SQL, because SQLite cannot call
 * `atlas_orch_state_is_terminal`. Two spellings of one rule drift, so they are
 * compared here over the whole vocabulary rather than trusted to have been kept
 * in step by hand. */
static void test_the_sql_terminal_set_matches_the_c_one(void) {
    env e;
    env_open(&e);

    static const atlas_orch_state ALL[] = {
        ATLAS_ORCH_STATE_QUEUED,           ATLAS_ORCH_STATE_LEASED,
        ATLAS_ORCH_STATE_PREPARING,        ATLAS_ORCH_STATE_RUNNING,
        ATLAS_ORCH_STATE_VALIDATING,       ATLAS_ORCH_STATE_SUCCEEDED,
        ATLAS_ORCH_STATE_FAILED,           ATLAS_ORCH_STATE_CANCEL_REQUESTED,
        ATLAS_ORCH_STATE_CANCELLED,        ATLAS_ORCH_STATE_TIMED_OUT,
        ATLAS_ORCH_STATE_RECOVERY_REQUIRED};

    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
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
        /* Formatted SQL, so it is stepped directly rather than through the
         * prepared-statement cache: that cache keys on the pointer and requires
         * a string literal, and handing it a reused buffer is the defect the
         * header warns about. */
        T_OK(atlas_db_query_int64(e.db, atlas_buf_cstr(&sql), &sql_active, &err), &err);
        bool c_active = !atlas_orch_state_is_terminal(ALL[i]);
        T_CHECK_MSG((sql_active == 1) == c_active,
                    "%s is %s in C and %s in the index predicate", name,
                    c_active ? "active" : "terminal", sql_active == 1 ? "active" : "terminal");
        atlas_buf_free(&sql);
    }
    env_close(&e);
}

/* --- the run's status is its own axis ------------------------------------ */

static void test_the_run_status_vocabulary_is_closed_and_zero_is_unknown(void) {
    /* UNKNOWN is zero, and it is not terminal: "nobody filled this in" is not a
     * settled answer, and reading it as one would let a malformed row close a
     * run. */
    T_CHECK(ATLAS_ORCH_RUN_UNKNOWN == 0);
    T_CHECK(!atlas_orch_run_status_is_terminal(ATLAS_ORCH_RUN_UNKNOWN));
    T_CHECK(!atlas_orch_run_status_is_terminal(ATLAS_ORCH_RUN_ACTIVE));
    T_CHECK(atlas_orch_run_status_is_terminal(ATLAS_ORCH_RUN_ACCEPTED));
    T_CHECK(atlas_orch_run_status_is_terminal(ATLAS_ORCH_RUN_BLOCKED));

    atlas_orch_run_status s = ATLAS_ORCH_RUN_ACCEPTED;
    /* "UNKNOWN" does not parse. It is the vocabulary's zero and no stored run
     * may hold it, so a database presenting it is reporting corruption. */
    T_CHECK(!atlas_orch_run_status_parse("UNKNOWN", &s));
    T_CHECK(!atlas_orch_run_status_parse("SUCCEEDED", &s));
    T_CHECK(!atlas_orch_run_status_parse("", &s));
    T_CHECK(atlas_orch_run_status_parse("ACTIVE", &s) && s == ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(atlas_orch_run_status_parse("BLOCKED", &s) && s == ATLAS_ORCH_RUN_BLOCKED);
    T_CHECK(strcmp(atlas_orch_run_status_name(ATLAS_ORCH_RUN_UNKNOWN), "UNKNOWN") == 0);
}

static const atlas_test TESTS[] = {
    {"a root task creates its run and reads back",
     test_a_root_task_creates_its_run_and_reads_back},
    {"a child joins its parent's run with a deterministic attempt chain",
     test_a_child_joins_its_parents_run_with_a_deterministic_attempt_chain},
    {"a parent that does not resolve is refused",
     test_a_parent_that_does_not_resolve_is_refused},
    {"a duplicate retry creates no second task or run",
     test_a_duplicate_retry_creates_no_second_task_or_run},
    {"a restart recovers the run, its active task and the parent chain",
     test_a_restart_recovers_the_run_its_active_task_and_the_parent_chain},
    {"a terminal run takes no further task",
     test_a_terminal_run_takes_no_further_task},
    {"a run takes no second active task", test_a_run_takes_no_second_active_task},
    {"the SQL terminal set matches the C one", test_the_sql_terminal_set_matches_the_c_one},
    {"the run status vocabulary is closed and zero is UNKNOWN",
     test_the_run_status_vocabulary_is_closed_and_zero_is_unknown},
};

ATLAS_TEST_MAIN("orch_run", TESTS)

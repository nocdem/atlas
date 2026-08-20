/* Atlas - A11.1 step 6: the pinned-commit check made *after* the worker exits.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `drive_one` checks the pinned commit twice, and the two checks are different
 * branches answering different questions.
 *
 * The first, step 3, asks whether the tree is the one the work was authorised
 * over before anything is started. `tests/test_a11_run.c` covers it, and in that
 * case no worker ever runs — which is exactly why it says nothing about the
 * second.
 *
 * The second, step 6, asks whether the worker itself moved the repository off
 * that commit. A worker that committed, reset or checked out has invalidated
 * everything a gate could establish: the gate would then be measuring a tree
 * nobody authorised, and a pass would be an acceptance of work Atlas cannot
 * describe. So the gates are not run at all and the attempt is refused rather
 * than judged.
 *
 * This file is the evidence for that branch, and for it alone. What separates it
 * from the step 3 case is not the outcome — both refuse — but that here **a
 * worker really ran first**: one line of real driver output in the repository's
 * own tree, and a refusal on top of it.
 *
 * ## How the HEAD change is made real
 *
 * By the worker, during its attempt, and it is a genuine one: after the drive
 * the repository resolves HEAD to a second real commit, which this test asserts
 * rather than assumes. That assertion is load-bearing — an unresolvable HEAD
 * would *also* be refused by step 6, so a broken fixture could pass this test
 * vacuously while never producing the drift it claims to produce.
 *
 * The fixture places a ref at that second commit and points the branch back at
 * the first; the worker points HEAD at the ref, which is the state `git
 * checkout` leaves. `fake:movehead` is one further additive behaviour of
 * `fake-repo`, selected by an Atlas literal exactly like `fake:fail` and the
 * rest, and it changes nothing about the four that were already there.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/driver.h"
#include "atlas/orch_ops.h"
#include "atlas/rundriver.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- the environment ------------------------------------------------------
 *
 * The same shape `tests/test_a11_run.c` uses, with one addition: a gate that
 * leaves a mark when it runs. Two gates that are constants prove nothing about
 * whether a gate ran, and "the gates were not run" is half of what this file
 * exists to establish. */
static const char GATE_MAKEFILE[] =
    "pass:\n"
    "\t@true\n"
    "mark:\n"
    "\t@echo the gate ran > ATLAS_GATE_RAN.txt\n";

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_buf identity;
    atlas_buf commit;  /* the commit the task is pinned to */
    atlas_buf moved;   /* the commit the worker moves HEAD to */
} env;

static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);
    atlas_buf_init(&e->identity);
    atlas_buf_init(&e->commit);
    atlas_buf_init(&e->moved);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "Makefile", GATE_MAKEFILE, &err), &err);
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
    T_REQUIRE(ri.scanned_head[0] != '\0');
    T_OK(atlas_buf_set_str(&e->commit, ri.scanned_head, &err), &err);
    atlas_repo_info_free(&ri);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    e->db = NULL;
    atlas_buf_free(&e->db_path);
    atlas_buf_free(&e->identity);
    atlas_buf_free(&e->commit);
    atlas_buf_free(&e->moved);
    fx_close(&e->fx);
}

static int64_t count_sql(atlas_db *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) == SQLITE_OK);
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

/* What the repository resolves HEAD to right now, asked of git rather than
 * inferred from what anything wrote. */
static void head_now(env *e, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    const char *rev[] = {"rev-parse", "HEAD"};
    atlas_buf raw = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_git(&e->fx, fx_repo(&e->fx), rev, 2u, &code, &raw, &err), &err);
    T_REQUIRE(code == 0);
    size_t n = raw.len;
    while (n > 0 && (raw.data[n - 1u] == '\n' || raw.data[n - 1u] == '\r')) {
        n--;
    }
    T_OK(atlas_buf_set(out, raw.data, n, &err), &err);
    atlas_buf_free(&raw);
    T_REQUIRE(out->len > 0);
}

/* --- the transport -------------------------------------------------------- */

static atlas_status t_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                            atlas_err *err) {
    return atlas_orch_apply((atlas_db *)ud, op, out, err);
}

static atlas_status t_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out, bool *found,
                              atlas_err *err) {
    return atlas_db_orch_run_get((atlas_db *)ud, run_uid, out, found, err);
}

/* A12.0. Part of the transport's contract; nothing here loses an answer, so it
 * is never consulted. `tests/test_orch_transport.c` is where it is exercised. */
static atlas_status t_job_get(void *ud, const char *job_uid, atlas_orch_job_view *out, bool *found,
                              atlas_err *err) {
    return atlas_db_orch_job_get((atlas_db *)ud, job_uid, out, found, err);
}

/* --- submitting a root task ----------------------------------------------- */

static void start_run(env *e, const char *task, const char *gate, atlas_buf *run_out,
                      atlas_buf *job_out) {
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
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake-repo", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, task, &err), &err);
    T_OK(atlas_orch_argv_push(&op->spec.validations[0], "make", 4u, &err), &err);
    T_OK(atlas_orch_argv_push(&op->spec.validations[0], gate, strlen(gate), &err), &err);
    op->spec.validation_count = 1;
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = ATLAS_ORCH_RUN_MAX_WORKER_STARTS;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    T_OK(atlas_orch_apply(e->db, op, &r, &err), &err);
    atlas_orch_op_free(op);
    free(op);
    T_REQUIRE(r.run_uid.len > 0);
    T_OK(atlas_buf_set(run_out, r.run_uid.data, r.run_uid.len, &err), &err);
    T_OK(atlas_buf_set(job_out, r.job_uid.data, r.job_uid.len, &err), &err);
    atlas_orch_result_free(&r);
}

static void drive(env *e, const char *run_uid, atlas_rundriver_report *rep) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_rundriver_report_init(rep);
    atlas_rundriver_opts o;
    memset(&o, 0, sizeof(o));
    o.run_uid = run_uid;
    o.dispatcher_id = "test-head-drift";
    o.max_tasks = 0;
    o.transport.apply = t_apply;
    o.transport.run_get = t_run_get;
    o.transport.job_get = t_job_get;
    o.transport.ud = e->db;
    T_OK(atlas_rundriver_run(&o, rep, &err), &err);
}

/* How many workers started and did work in the repository's own tree, counted
 * from what they left there rather than from anything Atlas said about itself. */
static int64_t worker_lines(env *e) {
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/ATLAS_FAKE_DRIVER.txt", fx_repo(&e->fx));
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return 0;
    }
    int64_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            n++;
        }
    }
    (void)fclose(f);
    return n;
}

static bool gate_ran(env *e) {
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/ATLAS_GATE_RAN.txt", fx_repo(&e->fx));
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return false;
    }
    (void)fclose(f);
    return true;
}

/* Places a second real commit off to one side and returns the branch to the
 * pinned one, so that at submission time the repository is exactly where the
 * task says it is and the only thing that can move it afterwards is the worker.
 *
 * `update-ref HEAD` rather than a named branch, so this does not depend on what
 * the fixture calls its initial branch. The work tree is left holding the second
 * commit's content, which nothing in the drive path looks at: step 3 and step 6
 * compare the HEAD commit and nothing else. */
static void plant_second_commit(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_write(fx_repo(&e->fx), "b.c", "int b(void){return 1;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "second", &err), &err);
    head_now(e, &e->moved);
    T_CHECK(strcmp(atlas_buf_cstr(&e->moved), atlas_buf_cstr(&e->commit)) != 0);
    {
        const char *branch[] = {"branch", ATLAS_FAKE_MOVED_BRANCH};
        T_OK(fx_git_ok(&e->fx, fx_repo(&e->fx), branch, 2u, &err), &err);
    }
    {
        const char *back[] = {"update-ref", "HEAD", atlas_buf_cstr(&e->commit)};
        T_OK(fx_git_ok(&e->fx, fx_repo(&e->fx), back, 3u, &err), &err);
    }
    /* The premise of the whole test: the drive starts from the pinned commit, so
     * step 3 passes and step 6 is the only check that can refuse. */
    atlas_buf at = ATLAS_BUF_INIT;
    head_now(e, &at);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&at), atlas_buf_cstr(&e->commit)) == 0,
                "the fixture did not return HEAD to the pinned commit");
    atlas_buf_free(&at);
}

/* --- the case ------------------------------------------------------------- */

static void test_a_worker_that_moved_head_is_refused_not_judged(void) {
    env e;
    env_open(&e);
    plant_second_commit(&e);

    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    /* The gate is `mark`, which succeeds and says so in the work tree. If it
     * ever runs, this run would be *accepted*, and the file is there to prove
     * which of the two happened. */
    start_run(&e, "fake:movehead write the thing", "mark", &run, &job);

    atlas_rundriver_report rep;
    drive(&e, atlas_buf_cstr(&run), &rep);

    /* 1. The worker really ran. This is what makes the case the step 6 branch
     *    and not the step 3 one: a pre-worker refusal leaves no line here. */
    T_EQ_INT((int)worker_lines(&e), 1);

    /* 2. And it really moved HEAD, to a second commit the repository resolves.
     *    Asserted rather than assumed: an unresolvable HEAD would be refused by
     *    the same branch, so without this the test could pass having produced no
     *    drift at all. */
    atlas_buf after = ATLAS_BUF_INIT;
    head_now(&e, &after);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&after), atlas_buf_cstr(&e.moved)) == 0,
                "the worker did not leave the repository at the second commit");
    T_CHECK(strcmp(atlas_buf_cstr(&after), atlas_buf_cstr(&e.commit)) != 0);
    atlas_buf_free(&after);

    /* 3. The run is not accepted. Not by the driver's report, not in the row,
     *    and not anywhere in the table. */
    T_CHECK(rep.status != ATLAS_ORCH_RUN_ACCEPTED);
    T_CHECK(rep.status == ATLAS_ORCH_RUN_BLOCKED);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs WHERE status = 'ACCEPTED';"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs WHERE status = 'BLOCKED';"), 1);

    /* 4. The gates did not run. The mark the gate leaves is absent, and no
     *    attempt ever entered VALIDATING — the marker alone would be a weak
     *    negative, and the transition alone would not prove the command was
     *    never created. */
    T_CHECK_MSG(!gate_ran(&e), "the gates ran over a tree the worker had moved");
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions WHERE to_state = 'VALIDATING';"),
             0);

    /* 5. Atlas refused rather than judged. `POLICY_REFUSED` is the refusal's own
     *    reason and is not something a gate can produce, so no verdict about the
     *    work was recorded — the attempt says the tree was wrong, not that the
     *    work was bad. */
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_attempts"
                            "  WHERE failure_reason = 'POLICY_REFUSED';"),
             1);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_attempts"
                            "  WHERE failure_reason IN ('VALIDATION_FAILED','WORKER_SUCCESS');"),
             0);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions"
                            "  WHERE to_state = 'FAILED' AND reason = 'POLICY_REFUSED';"),
             1);

    /* 6. And nothing was retried and nothing was narrowed. A moved HEAD is not a
     *    failure another attempt or a smaller task can answer, so the run ends
     *    with the one task and the one worker start it had. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions WHERE to_state = 'RUNNING';"),
             1);
    T_EQ_INT((int)rep.worker_starts, 1);
    T_CHECK(!rep.busy);

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
    env_close(&e);
}

/* The control, and the reason the case above is evidence about step 6 rather
 * than about the fixture. Everything is identical except the one Atlas literal
 * that makes the worker move HEAD: the same repository, the same planted ref,
 * the same gate. The worker leaves HEAD where it found it, the gate runs, and
 * the run is accepted. */
static void test_the_same_run_without_the_move_is_accepted(void) {
    env e;
    env_open(&e);
    plant_second_commit(&e);

    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    start_run(&e, "write the thing", "mark", &run, &job);

    atlas_rundriver_report rep;
    drive(&e, atlas_buf_cstr(&run), &rep);

    T_EQ_INT((int)worker_lines(&e), 1);
    T_CHECK_MSG(gate_ran(&e), "the gate did not run when the repository had not moved");
    T_CHECK(rep.status == ATLAS_ORCH_RUN_ACCEPTED);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions WHERE to_state = 'VALIDATING';"),
             1);

    atlas_buf after = ATLAS_BUF_INIT;
    head_now(&e, &after);
    T_CHECK(strcmp(atlas_buf_cstr(&after), atlas_buf_cstr(&e.commit)) == 0);
    atlas_buf_free(&after);

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a worker that moved HEAD is refused rather than judged, and no gate runs",
     test_a_worker_that_moved_head_is_refused_not_judged},
    {"the same run whose worker leaves HEAD alone is gated and accepted",
     test_the_same_run_without_the_move_is_accepted},
};

ATLAS_TEST_MAIN("a11_head_drift", TESTS)

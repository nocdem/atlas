/* Atlas - A12.0: the run driver survives a call whose answer never arrived.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A11.1's driver retried exactly one failure — the daemon's `BUSY:`, which is a
 * promise that nothing was queued — and treated every other failure as fatal to
 * the invocation. Pilot A11.6-P lost a run to that twice, once in each of its
 * runs: a congested serve loop timed out one frame-header read on a phase call,
 * the foreground driver exited, the worker kept working, nobody renewed the
 * lease, and the attempt was reclaimed underneath a process that was still
 * editing the tree.
 *
 * A read timeout and a refusal are opposite claims and this file is the evidence
 * that the driver now tells them apart:
 *
 *   - a lost answer is asked for again, on a bounded budget;
 *   - a refusal is an answer and is never asked for twice;
 *   - and a **completion** whose answer was lost is checked against the run
 *     before it is mourned, because that one cannot be applied twice: the token
 *     it carries is consumed by the delivery that landed.
 *
 * ## What is real here and what is not
 *
 * Everything except the socket. The database, the repository, the registration,
 * the lease, the attempt, the ledger, the gates and the settlement are real, and
 * the worker is `fake-repo`, which stands to `claude-repo` exactly as A8's
 * `fake` stands to `claude`. The transport is a fixture that reaches the same
 * write point through a database handle — `tests/test_a11_run.c` carries that
 * argument in full — and here it also loses selected answers on purpose.
 *
 * A lost answer is built exactly as `atlas_ipc_call_timeout` builds one: same
 * status, same sentence, same mark. A fixture that stamped a mark the client
 * layer does not stamp would be testing itself.
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
 * The shape `tests/test_a11_run.c` uses, with the one gate these cases need. */
static const char GATE_MAKEFILE[] =
    "pass:\n"
    "\t@true\n";

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

static bool spool_exists(const char *spool, const char *job_uid) {
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/%s.1.result", spool, job_uid);
    FILE *f = fopen(path, "re");
    if (f != NULL) {
        (void)fclose(f);
    }
    return f != NULL;
}

static atlas_orch_run_status run_status(env *e, const char *run_uid) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_run_view rv;
    memset(&rv, 0, sizeof(rv));
    bool found = false;
    T_OK(atlas_db_orch_run_get(e->db, run_uid, &rv, &found, &err), &err);
    T_REQUIRE(found);
    return rv.status;
}

/* --- the transports -------------------------------------------------------- */

/* The failure both pilots died on, reproduced at the boundary the driver sees
 * it: the request went out and the answer did not come back inside the deadline.
 * Whether the daemon applied it is exactly what this failure does not say. */
static atlas_status answer_lost(atlas_err *err) {
    atlas_status st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                    "timed out while reading a frame header");
    atlas_err_mark_transport(err);
    return st;
}

/* A daemon that loses the answers to a chosen number of run reads, leases and
 * phase calls, before applying them. Nothing was written in those calls, so the
 * retry is the one that does the work. */
typedef struct lossy {
    atlas_db *db;
    int lose_read;
    int lose_lease;
    int lose_phase;
    int lost;
} lossy;

static atlas_status lossy_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                                atlas_err *err) {
    lossy *x = (lossy *)ud;
    int *budget = NULL;
    if (op->kind == ATLAS_ORCH_OP_LEASE) {
        budget = &x->lose_lease;
    } else if (op->kind == ATLAS_ORCH_OP_HEARTBEAT) {
        budget = &x->lose_phase;
    }
    if (budget != NULL && *budget > 0) {
        (*budget)--;
        x->lost++;
        return answer_lost(err);
    }
    return atlas_orch_apply(x->db, op, out, err);
}

static atlas_status lossy_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out,
                                  bool *found, atlas_err *err) {
    lossy *x = (lossy *)ud;
    if (x->lose_read > 0) {
        x->lose_read--;
        x->lost++;
        return answer_lost(err);
    }
    return atlas_db_orch_run_get(x->db, run_uid, out, found, err);
}

/* A daemon that refuses. Not `BUSY:`, not a lost answer: an answer, of the kind
 * that says the same thing however many times it is asked. */
typedef struct refuser {
    atlas_db *db;
    int leases;
} refuser;

static atlas_status refuser_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                                  atlas_err *err) {
    refuser *x = (refuser *)ud;
    if (op->kind == ATLAS_ORCH_OP_LEASE) {
        x->leases++;
        return atlas_err_set(err, ATLAS_ERR_USAGE, "that job is not in a leasable state");
    }
    return atlas_orch_apply(x->db, op, out, err);
}

static atlas_status refuser_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out,
                                    bool *found, atlas_err *err) {
    return atlas_db_orch_run_get(((refuser *)ud)->db, run_uid, out, found, err);
}

/* A daemon that is simply gone: every answer is lost, forever. */
typedef struct gone {
    atlas_db *db;
    int calls;
} gone;

static atlas_status gone_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                               atlas_err *err) {
    gone *x = (gone *)ud;
    (void)op;
    (void)out;
    x->calls++;
    return answer_lost(err);
}

static atlas_status gone_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out,
                                 bool *found, atlas_err *err) {
    return atlas_db_orch_run_get(((gone *)ud)->db, run_uid, out, found, err);
}

/* The completion, and the two ways its delivery is lost sight of.
 *
 * `applied_first` is the plain case: the daemon took the completion, committed
 * it, and the reply did not come back. `applied_second` is the one the ordering
 * makes possible — the first delivery was still queued when the driver looked,
 * so the run still named the task; the write then landed, and the redelivery was
 * refused because the attempt it named is no longer leased. Both are deliveries,
 * and reporting either as a lost result is the pilots' exact loss. */
typedef enum ack_mode { ACK_APPLIED_FIRST, ACK_APPLIED_SECOND } ack_mode;

typedef struct lost_ack {
    atlas_db *db;
    ack_mode mode;
    int completes;
} lost_ack;

static atlas_status lost_ack_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                                   atlas_err *err) {
    lost_ack *x = (lost_ack *)ud;
    if (op->kind != ATLAS_ORCH_OP_COMPLETE) {
        return atlas_orch_apply(x->db, op, out, err);
    }
    x->completes++;
    /* Recorded and continued rather than required: this runs inside the driver,
     * and abandoning a test from here would unwind through its frames. */
    T_CHECK_MSG(x->completes <= 2, "the driver kept offering a completion the daemon had taken");
    if (x->mode == ACK_APPLIED_FIRST || x->completes == 2) {
        T_CHECK_MSG(atlas_orch_apply(x->db, op, out, err) == ATLAS_OK,
                    "the fixture daemon could not take the completion");
        if (x->mode == ACK_APPLIED_FIRST) {
            return answer_lost(err);
        }
        /* The first delivery has landed by now, so the second names an attempt
         * that is no longer leased. This is what the daemon answers. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no leased attempt holds that token");
    }
    /* Queued, not yet applied. The run still names the task. */
    return answer_lost(err);
}

static atlas_status lost_ack_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out,
                                     bool *found, atlas_err *err) {
    return atlas_db_orch_run_get(((lost_ack *)ud)->db, run_uid, out, found, err);
}

/* A daemon that grants a lease and loses the answer. The grant is real and the
 * driver never heard it, which is the residual this milestone states rather than
 * removes: the invocation is lost, the run is not. */
typedef struct lost_grant {
    atlas_db *db;
    int leases;
} lost_grant;

static atlas_status lost_grant_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                                     atlas_err *err) {
    lost_grant *x = (lost_grant *)ud;
    if (op->kind == ATLAS_ORCH_OP_LEASE) {
        x->leases++;
        if (x->leases == 1) {
            T_CHECK_MSG(atlas_orch_apply(x->db, op, out, err) == ATLAS_OK,
                        "the fixture daemon could not grant the first lease");
            T_CHECK_MSG(out->granted, "the first lease was not granted");
            return answer_lost(err);
        }
    }
    return atlas_orch_apply(x->db, op, out, err);
}

static atlas_status lost_grant_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out,
                                       bool *found, atlas_err *err) {
    return atlas_db_orch_run_get(((lost_grant *)ud)->db, run_uid, out, found, err);
}

/* --- submitting a root task ----------------------------------------------- */

static void start_run(env *e, const char *task, atlas_buf *run_out, atlas_buf *job_out) {
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
    T_OK(atlas_orch_argv_push(&op->spec.validations[0], "pass", 4u, &err), &err);
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

/* Drives one run against a fixture transport, and returns the driver's status
 * rather than requiring success: what a lost answer costs is the thing under
 * test. The pause between two attempts is one millisecond because the wait is
 * not what is being proved, and a test that slept the real one would be four
 * seconds of nothing. */
static atlas_status drive(const char *run_uid, atlas_rundriver_transport xport,
                          const char *spool, atlas_rundriver_report *rep) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_rundriver_report_init(rep);
    atlas_rundriver_opts o;
    memset(&o, 0, sizeof(o));
    o.run_uid = run_uid;
    o.dispatcher_id = "test-transport";
    o.max_tasks = 0;
    o.spool_dir = spool;
    o.xport_pause_ms = 1;
    o.transport = xport;
    return atlas_rundriver_run(&o, rep, &err);
}

/* --- 1: a lost answer is asked for again ---------------------------------- */

static void test_a_lost_answer_is_asked_for_again(void) {
    env e;
    env_open(&e);
    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    start_run(&e, "write the thing", &run, &job);

    /* The run read the driver opens with, the lease, and two phase calls, each
     * with an answer that never arrives. This is the pilots' failure four times
     * over in one invocation, on every call the driver makes before the worker
     * exists. */
    lossy x = {e.db, 1, 1, 2, 0};
    atlas_rundriver_transport t = {lossy_apply, lossy_run_get, &x};
    atlas_rundriver_report rep;
    T_CHECK(drive(atlas_buf_cstr(&run), t, NULL, &rep) == ATLAS_OK);

    T_CHECK_MSG(x.lost == 4, "the driver did not meet the losses it was given");
    /* Nothing was lost and nothing happened twice: one worker, one attempt, one
     * task, and the run reached its answer. */
    T_CHECK(rep.status == ATLAS_ORCH_RUN_ACCEPTED);
    T_CHECK(run_status(&e, atlas_buf_cstr(&run)) == ATLAS_ORCH_RUN_ACCEPTED);
    T_EQ_INT((int)worker_lines(&e), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);
    T_EQ_INT((int)rep.worker_starts, 1);

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
    env_close(&e);
}

/* --- 2: a refusal is an answer, and is never asked twice ------------------- */

static void test_a_refusal_is_not_retried(void) {
    env e;
    env_open(&e);
    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    start_run(&e, "write the thing", &run, &job);

    refuser x = {e.db, 0};
    atlas_rundriver_transport t = {refuser_apply, refuser_run_get, &x};
    atlas_rundriver_report rep;
    T_CHECK(drive(atlas_buf_cstr(&run), t, NULL, &rep) != ATLAS_OK);

    /* Exactly once. A refusal that is asked again is a daemon answering the same
     * thing twice, and the second question was worth nothing. */
    T_EQ_INT(x.leases, 1);
    T_EQ_INT((int)worker_lines(&e), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 0);
    T_CHECK(run_status(&e, atlas_buf_cstr(&run)) == ATLAS_ORCH_RUN_ACTIVE);

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
    env_close(&e);
}

/* --- 3: the asking is bounded ---------------------------------------------- */

static void test_the_retry_is_bounded(void) {
    env e;
    env_open(&e);
    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    start_run(&e, "write the thing", &run, &job);

    gone x = {e.db, 0};
    atlas_rundriver_transport t = {gone_apply, gone_run_get, &x};
    atlas_rundriver_report rep;
    T_CHECK(drive(atlas_buf_cstr(&run), t, NULL, &rep) != ATLAS_OK);

    /* It tried again, and it stopped. The number is `RUN_XPORT_TRIES` in
     * `src/orch/rundriver.c`; what matters here is that neither zero nor
     * unbounded is what happens, because a loop with no end is a hang and a
     * driver that gives up at once is A11.1's defect. */
    T_CHECK_MSG(x.calls > 1, "a lost answer was not asked for again");
    T_CHECK_MSG(x.calls <= 12, "the retry of a lost answer is not bounded");
    /* And it cost the invocation, not the run. */
    T_EQ_INT((int)worker_lines(&e), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 0);
    T_CHECK(run_status(&e, atlas_buf_cstr(&run)) == ATLAS_ORCH_RUN_ACTIVE);

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
    env_close(&e);
}

/* --- 4: a completion is checked before it is mourned ---------------------- */

static void check_completion_lands(env *e, ack_mode mode, int expect_completes) {
    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    start_run(e, "write the thing", &run, &job);

    lost_ack x = {e->db, mode, 0};
    atlas_rundriver_transport t = {lost_ack_apply, lost_ack_run_get, &x};
    atlas_rundriver_report rep;
    T_CHECK_MSG(drive(atlas_buf_cstr(&run), t, fx_data_dir(&e->fx), &rep) == ATLAS_OK,
                "a delivered completion was reported as a failed invocation");

    T_EQ_INT(x.completes, expect_completes);
    /* The daemon has the result: one attempt, one worker, and the run settled on
     * the gate verdict that completion carried. */
    T_CHECK(rep.status == ATLAS_ORCH_RUN_ACCEPTED);
    T_CHECK(run_status(e, atlas_buf_cstr(&run)) == ATLAS_ORCH_RUN_ACCEPTED);
    T_EQ_INT((int)worker_lines(e), 1);
    T_EQ_INT((int)count_sql(e->db, "SELECT count(*) FROM orch_attempts;"), 1);
    T_EQ_INT((int)count_sql(e->db, "SELECT count(*) FROM orch_jobs;"), 1);

    /* The spooled result is left where it is. Clearing it is what an
     * acknowledgement buys, and this delivery was never acknowledged: the run
     * says the task is settled, not that this reply was heard. */
    T_CHECK_MSG(spool_exists(fx_data_dir(&e->fx), atlas_buf_cstr(&job)),
                "an unacknowledged completion cleared the only copy of its result");

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
}

static void test_a_completion_whose_answer_was_lost_is_not_mourned(void) {
    env e;
    env_open(&e);
    check_completion_lands(&e, ACK_APPLIED_FIRST, 1);
    env_close(&e);
}

static void test_a_refusal_after_a_lost_answer_is_a_delivery(void) {
    env e;
    env_open(&e);
    check_completion_lands(&e, ACK_APPLIED_SECOND, 2);
    env_close(&e);
}

/* --- 5: the grant nobody heard -------------------------------------------- */

/* The stated residual. A lease is a compare-and-swap against `state = 'QUEUED'`,
 * so a grant that was made and whose answer was lost cannot be re-granted: the
 * re-request finds the task held and is answered `busy`. That costs this
 * invocation and nothing else — no worker starts, no task moves, the run stays
 * ACTIVE and resumable, and the held lease expires on its own. Retrying a lease
 * is safe precisely because naming a task that is not QUEUED grants nothing and
 * is not an error. */
static void test_a_grant_that_was_never_heard_costs_the_invocation(void) {
    env e;
    env_open(&e);
    atlas_buf run = ATLAS_BUF_INIT, job = ATLAS_BUF_INIT;
    start_run(&e, "write the thing", &run, &job);

    lost_grant x = {e.db, 0};
    atlas_rundriver_transport t = {lost_grant_apply, lost_grant_run_get, &x};
    atlas_rundriver_report rep;
    T_CHECK(drive(atlas_buf_cstr(&run), t, NULL, &rep) == ATLAS_OK);

    T_CHECK_MSG(x.leases > 1, "a lost lease answer was not asked for again");
    T_CHECK(rep.busy);
    T_EQ_INT((int)rep.tasks, 0);
    T_EQ_INT((int)worker_lines(&e), 0);
    /* One attempt exists — the granted one nobody heard about — and the run is
     * neither settled nor refused. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);
    T_CHECK(run_status(&e, atlas_buf_cstr(&run)) == ATLAS_ORCH_RUN_ACTIVE);

    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run);
    atlas_buf_free(&job);
    env_close(&e);
}

/* --- 6: the mark is the classification, and it cannot be spoken ----------- */

/* The predicate the whole file rests on is not a substring of anybody's prose.
 * A daemon's refusal can quote a repository, a task or a model, and none of that
 * can produce a mark: only the layer that held the file descriptor stamps one,
 * and every `atlas_err_set` clears it again. */
static void test_a_refusal_can_never_speak_the_mark(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_CHECK(!atlas_err_is_transport(&err));

    (void)atlas_err_set(&err, ATLAS_ERR_USAGE,
                        "the worker wrote: timed out while reading a frame header");
    T_CHECK_MSG(!atlas_err_is_transport(&err),
                "a quoted sentence produced a transport classification");

    (void)answer_lost(&err);
    T_CHECK(atlas_err_is_transport(&err));

    /* And it does not survive the next error set on the same carrier. */
    (void)atlas_err_set(&err, ATLAS_ERR_DB, "something else entirely");
    T_CHECK(!atlas_err_is_transport(&err));
    atlas_err_init(&err);
    T_CHECK(!atlas_err_is_transport(&err));
}

static const atlas_test TESTS[] = {
    {"a refusal can never speak the transport mark", test_a_refusal_can_never_speak_the_mark},
    {"a lost answer on a lease or a phase call is asked for again",
     test_a_lost_answer_is_asked_for_again},
    {"a daemon refusal is an answer and is never asked twice", test_a_refusal_is_not_retried},
    {"the asking is bounded, and costs the invocation rather than the run",
     test_the_retry_is_bounded},
    {"a completion whose answer was lost is checked before it is mourned",
     test_a_completion_whose_answer_was_lost_is_not_mourned},
    {"a refusal arriving after a lost answer is a delivery, not a loss",
     test_a_refusal_after_a_lost_answer_is_a_delivery},
    {"a lease grant nobody heard costs the invocation and not the run",
     test_a_grant_that_was_never_heard_costs_the_invocation},
};

ATLAS_TEST_MAIN("orch_transport", TESTS)

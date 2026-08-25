/* Atlas - A11.6: bounded parallel tasks in one run, against a real database.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A11.0 made "one active task per run" a fact about stored rows, and A11.1
 * built a run driver on top of it. That was the right guarantee for a season
 * whose subject was a single worker and the wrong one for a run that wants a
 * second task doing something else at the same time. This file is the evidence
 * for what replaced it, and the replacement is two guarantees rather than none:
 *
 *   * at most `max_parallel` active tasks per run, fixed at the root, held by a
 *     unique index on `(run_uid, run_slot)`;
 *   * **at most one active task in the registered repository's own tree, always**
 *     — a bound cannot widen access to the one resource two workers cannot
 *     share, and a partial unique index on the repo-tree drivers says so.
 *
 * Everything here drives `atlas_orch_apply` — the one write point — against an
 * isolated fixture with its own registered repository. Nothing touches a live
 * service, a socket, the real index or a registered repository, no worker
 * process is created, and **nothing sleeps**: every interleaving below is
 * produced by issuing operations in an order this thread chose, and every clock
 * a decision depends on is supplied to the operation. A test that proved
 * concurrency by racing would be a test that passes at a different rate on a
 * different machine.
 *
 * The gates a repo-tree task declares are never executed here. They exist
 * because the write point refuses a repo-tree task with none, which is A11.1's
 * rule and is the reason `--gate` is not optional on a run.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/driver.h"
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

/* --- small readers -------------------------------------------------------- */

static int64_t count_sql(atlas_db *db, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t v = -1;
    T_OK(atlas_db_query_int64(db, sql, &v, &err), &err);
    return v;
}

/* One text column from a formatted query. Stepped directly rather than through
 * `atlas_db_prepare`, because that cache keys on the SQL *pointer* and requires
 * a string literal; handing it a reused buffer is the defect the header warns
 * about. */
static void text_sql(atlas_db *db, const char *sql, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(out, "", &err), &err);
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) == SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (t != NULL) {
            T_OK(atlas_buf_set_str(out, (const char *)t, &err), &err);
        }
    }
    sqlite3_finalize(st);
}

/* Executes raw SQL and requires it to be refused. The point of every use below
 * is that the C write point was bypassed entirely and the *schema* still said
 * no. */
static void sql_refused(atlas_db *db, const char *sql, const char *what) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = atlas_db_exec_sql(db, sql, &err);
    T_CHECK_MSG(st != ATLAS_OK, "%s was accepted by the schema", what);
}

static atlas_orch_run_view run_view(env *e, const char *run_uid) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_run_view rv;
    memset(&rv, 0, sizeof(rv));
    bool found = false;
    T_OK(atlas_db_orch_run_get(e->db, run_uid, &rv, &found, &err), &err);
    T_REQUIRE(found);
    return rv;
}

static atlas_orch_state job_state(env *e, const char *job_uid) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_job_view jv;
    atlas_orch_job_view_init(&jv);
    bool found = false;
    T_OK(atlas_db_orch_job_get(e->db, job_uid, &jv, &found, &err), &err);
    T_REQUIRE(found);
    atlas_orch_state s = jv.state;
    atlas_orch_job_view_free(&jv);
    return s;
}

/* The ledger id of the first transition of a job into `to`, or 0. The ledger's
 * AUTOINCREMENT is the ordering authority in this repository — never a
 * timestamp — so every claim about interleaving below is made with it. */
static int64_t ledger_id(env *e, const char *job_uid, const char *to) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "SELECT COALESCE(MIN(t.id), 0) FROM orch_transitions t"
                           "  JOIN orch_jobs j ON j.id = t.job_id"
                           "  WHERE j.job_uid = '%s' AND t.to_state = '%s';",
                           job_uid, to),
         &err);
    int64_t v = count_sql(e->db, atlas_buf_cstr(&sql));
    atlas_buf_free(&sql);
    return v;
}

/* The wall deadline a submission computed for itself. Read back rather than
 * recomputed, so a case about the wall clock states the moment the write point
 * stored instead of one it assumed. */
static int64_t job_deadline_ms(env *e, const char *job_uid) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "SELECT COALESCE(deadline_ms, 0) FROM orch_jobs WHERE job_uid = '%s';",
                           job_uid),
         &err);
    int64_t v = count_sql(e->db, atlas_buf_cstr(&sql));
    atlas_buf_free(&sql);
    return v;
}

/* --- building operations -------------------------------------------------- */

/* Everything a submission in this file varies. A struct rather than eight
 * positional arguments, because a call site reading `submit_op(&e, NULL, NULL,
 * 1, 0, NULL)` says nothing about what it is testing. */
typedef struct sub {
    const char *key;
    const char *parent;
    const char *driver; /* NULL is "fake", an A8 workspace driver */
    const char *task;   /* NULL is a fixed string */
    const char *commit; /* NULL is the fixture's pinned head */
    int64_t attempts;   /* 0 is 1 */
    int64_t parallel;   /* 0 is "not stated" */
    int64_t wall_ms;    /* 0 is an hour */
} sub;

static atlas_orch_op *submit_op(env *e, const sub *s) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    /* The submitter comes from the trusted connection at the IPC edge. There is
     * no path by which a request body reaches it, here or in the daemon. */
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = 1;
    op->run_max_parallel = s->parallel;
    op->spec.submitter_uid = 1000;
    const char *driver = s->driver != NULL ? s->driver : "fake";
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e->identity.data, e->identity.len, &err),
         &err);
    if (s->commit != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.source_commit, s->commit, &err), &err);
    } else {
        T_OK(atlas_buf_set(&op->spec.source_commit, e->commit.data, e->commit.len, &err), &err);
    }
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, driver, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, s->task != NULL ? s->task : "do the thing", &err),
         &err);
    /* A repo-tree task with no gate is refused at the write point, so every one
     * built here declares the same one. It is never run: this file drives the
     * state machine, not a worker. */
    if (atlas_orch_driver_is_repo_tree(driver)) {
        T_OK(atlas_orch_argv_push(&op->spec.validations[0], "make", 4u, &err), &err);
        T_OK(atlas_orch_argv_push(&op->spec.validations[0], "pass", 4u, &err), &err);
        op->spec.validation_count = 1;
    }
    /* An hour unless a case needs a wall bound it can reach, and the idle bound
     * follows it down: the spec validator asks only that idle never exceeds
     * wall, and a case shortening one should not have to remember the other. */
    op->spec.wall_timeout_ms = s->wall_ms > 0 ? s->wall_ms : 3600000;
    op->spec.idle_timeout_ms = 900000;
    if (op->spec.idle_timeout_ms > op->spec.wall_timeout_ms) {
        op->spec.idle_timeout_ms = op->spec.wall_timeout_ms;
    }
    op->spec.max_attempts = s->attempts > 0 ? s->attempts : 1;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    if (s->key != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.idempotency_key, s->key, &err), &err);
    }
    if (s->parent != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.parent_job_uid, s->parent, &err), &err);
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
        T_CHECK_MSG(strstr(err.msg, expect) != NULL,
                    "%s was refused with \"%s\", which does not mention \"%s\"", what, err.msg,
                    expect);
    }
    atlas_orch_result_free(&r);
    atlas_orch_op_free(op);
    free(op);
}

/* Submits and returns the run and job uids, which every case below needs. */
static void submit(env *e, const sub *s, atlas_buf *run_out, atlas_buf *job_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result r;
    apply_ok(e, submit_op(e, s), &r);
    if (run_out != NULL) {
        T_OK(atlas_buf_set(run_out, r.run_uid.data, r.run_uid.len, &err), &err);
    }
    if (job_out != NULL) {
        T_OK(atlas_buf_set(job_out, r.job_uid.data, r.job_uid.len, &err), &err);
    }
    atlas_orch_result_free(&r);
}

/* Leases one job. `job_uid` names it — the run driver's targeted form — and
 * `driver` is the filter the dispatcher polls with. A repo-tree job needs the
 * filter whichever form is used: an unfiltered lease means "any", and A11.1
 * narrowed what "any" covers to exclude the repository's own tree. */
static int64_t lease(env *e, const char *job_uid, const char *driver, atlas_buf *token_out,
                     int64_t *expires_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->dispatcher_id, "d1", &err), &err);
    if (job_uid != NULL) {
        T_OK(atlas_buf_set_str(&op->job_uid, job_uid, &err), &err);
    }
    if (driver != NULL) {
        atlas_orch_argv want;
        atlas_orch_argv_init(&want);
        T_OK(atlas_orch_argv_push(&want, driver, strlen(driver), &err), &err);
        T_OK(atlas_orch_validations_encode(&want, 1u, &op->lease_drivers, &err), &err);
        atlas_orch_argv_free(&want);
    }
    atlas_orch_result g;
    apply_ok(e, op, &g);
    T_REQUIRE(g.granted);
    if (token_out != NULL) {
        T_OK(atlas_buf_set(token_out, g.token.data, g.token.len, &err), &err);
    }
    if (expires_out != NULL) {
        *expires_out = g.expires_ms;
    }
    int64_t n = g.attempt_no;
    atlas_orch_result_free(&g);
    return n;
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

/* A job may not succeed straight out of LEASED — the transition table has no
 * such edge — so every attempt walks the phases a real dispatcher walks.
 *
 * `expires_out`, when given, receives the expiry the **last** of those
 * heartbeats stored. A case that wants a moment after a lease has run out has to
 * ask the renewal rather than the grant: every heartbeat renews, writing
 * `expires_ms = at_ms + ATLAS_ORCH_LEASE_MS` from the wall clock, so the expiry
 * a lease was granted with is already stale by the time the worker reports
 * RUNNING. */
static void advance_to_running_exp(env *e, const char *token, int64_t *expires_out) {
    static const atlas_orch_state FORWARD[] = {ATLAS_ORCH_STATE_PREPARING,
                                               ATLAS_ORCH_STATE_RUNNING};
    for (size_t i = 0; i < sizeof FORWARD / sizeof FORWARD[0]; i++) {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, token);
        op->phase = FORWARD[i];
        atlas_orch_result r;
        apply_ok(e, op, &r);
        if (expires_out != NULL) {
            *expires_out = r.expires_ms;
        }
        atlas_orch_result_free(&r);
    }
}

static void advance_to_running(env *e, const char *token) {
    advance_to_running_exp(e, token, NULL);
}

/* One heartbeat with the clock supplied, and the job state it answered with.
 *
 * Every decision `op_heartbeat` makes — the renewal bound, the wall deadline and
 * whether the lease is still live — is made against this one value, so a case
 * about any of them states the moment rather than waiting for it. A phase of
 * UNKNOWN is a bare renewal, which is what a run driver sends while a worker is
 * already in the phase it reported. */
static atlas_orch_state heartbeat_at(env *e, const char *token, atlas_orch_state phase,
                                     int64_t at_ms) {
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, token);
    op->phase = phase;
    op->now_ms = at_ms;
    atlas_orch_result r;
    apply_ok(e, op, &r);
    atlas_orch_state s = r.state;
    atlas_orch_result_free(&r);
    return s;
}

/* Completes an attempt and returns the run's status afterwards, which is the
 * value every settlement case is about. */
static atlas_orch_run_status finish(env *e, const char *token, bool success,
                                    atlas_orch_reason reason) {
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, token);
    op->success = success;
    op->failure_reason = reason;
    op->exit_kind = success ? ATLAS_ORCH_EXIT_OK : ATLAS_ORCH_EXIT_NONZERO;
    atlas_orch_result r;
    apply_ok(e, op, &r);
    atlas_orch_run_status s = r.run_status;
    atlas_orch_result_free(&r);
    return s;
}

/* Leases, runs and completes one attempt of a named job in one call, for the
 * cases where the attempt itself is not what is being observed. */
static atlas_orch_run_status run_one(env *e, const char *job_uid, const char *driver, bool success,
                                     atlas_orch_reason reason) {
    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease(e, job_uid, driver, &tok, NULL);
    advance_to_running(e, atlas_buf_cstr(&tok));
    atlas_orch_run_status s = finish(e, atlas_buf_cstr(&tok), success, reason);
    atlas_buf_free(&tok);
    return s;
}

/* --- 1: admission ---------------------------------------------------------- */

static void test_a_run_admits_siblings_up_to_its_bound_and_no_further(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);

    /* One sibling, admitted: the run allows two and holds one. */
    atlas_buf sib = ATLAS_BUF_INIT;
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 2);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 1);

    /* The two tasks hold different slots, which is what the unique index is
     * about. Slot numbers are an occupied set, not a counter. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(DISTINCT run_slot) FROM orch_jobs;"), 2);

    /* A third is refused, and the refusal names the count, the bound and a task
     * in the way — a bare "the run is full" sends an operator looking. */
    sub third = {.parent = atlas_buf_cstr(&root)};
    apply_refused(&e, submit_op(&e, &third), "a third task in a run that allows two",
                  "which is its bound of 2");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 2);

    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* The companion to `test_a_run_takes_no_second_active_task` in
 * `tests/test_orch_run.c`, which stays as the default-behaviour contract: a run
 * created without asking for anything allows exactly one task, and says so in
 * the words it always has. */
static void test_a_run_that_asked_for_nothing_still_allows_exactly_one(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .attempts = 3};
    submit(&e, &root_s, &run, &root);

    atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
    T_EQ_INT((int)rv.max_parallel, 1);

    sub second = {.parent = atlas_buf_cstr(&root)};
    apply_refused(&e, submit_op(&e, &second), "a second task in a default run",
                  "already has an active task");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);

    /* Stating the default explicitly is the same run, not a different one. */
    atlas_buf run2 = ATLAS_BUF_INIT;
    sub explicit_one = {.driver = "fake-repo", .parallel = 1, .attempts = 3, .task = "another"};
    submit(&e, &explicit_one, &run2, NULL);
    atlas_orch_run_view rv2 = run_view(&e, atlas_buf_cstr(&run2));
    T_EQ_INT((int)rv2.max_parallel, 1);

    atlas_buf_free(&run2);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 2: the repository's own tree stays exclusive -------------------------- */

static void test_the_repository_tree_stays_exclusive_whatever_the_bound(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 4, .attempts = 3};
    submit(&e, &root_s, &run, &root);

    /* Room for three more by the bound, and none at all for a second task in the
     * tree. The bound and the exclusivity are different rules and the refusal
     * says which one it is. */
    sub tree_child = {.parent = atlas_buf_cstr(&root), .driver = "fake-repo"};
    apply_refused(&e, submit_op(&e, &tree_child), "a second repo-tree task in one run",
                  "the tree is exclusive");

    /* A workspace sibling is admitted at the same moment, which is the whole
     * point: parallelism is workspace tasks beside the one tree task. */
    atlas_buf sib = ATLAS_BUF_INIT;
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 2);

    /* And once the tree task ends, another may take its place. */
    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", true,
                    ATLAS_ORCH_REASON_UNKNOWN) == ATLAS_ORCH_RUN_ACTIVE);
    atlas_buf next = ATLAS_BUF_INIT;
    sub next_s = {.parent = atlas_buf_cstr(&root), .driver = "fake-repo", .task = "again"};
    submit(&e, &next_s, NULL, &next);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 3);

    atlas_buf_free(&next);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 3: the schema is the guarantee --------------------------------------- */

/* Both invariants are asserted with the C write point bypassed entirely, by
 * UPDATEs that no code path in Atlas performs. The checks in `db_orch.c` exist
 * so a caller gets a sentence; these two indexes and two CHECKs are what make
 * the rules true, and that is M21's arrangement carried forward. */
static void test_the_schema_refuses_what_the_checks_would_have(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    /* (a) Two active tasks in one slot. */
    sql_refused(e.db, "UPDATE orch_jobs SET run_slot = 0 WHERE parent_job_uid <> '';",
                "two active tasks sharing a slot");

    /* (b) Two active tasks in the repository's own tree. */
    sql_refused(e.db, "UPDATE orch_jobs SET driver = 'fake-repo' WHERE parent_job_uid <> '';",
                "a second active repo-tree task");

    /* (c) The compiled ceiling, duplicated in two CHECKs because SQLite cannot
     * read a macro. Both are written from the constant so raising it here can
     * never quietly disagree with the schema. */
    {
        atlas_buf sql = ATLAS_BUF_INIT;
        atlas_err err;
        atlas_err_init(&err);
        T_OK(atlas_buf_appendf(&sql, &err, "UPDATE orch_runs SET max_parallel = %d;",
                               ATLAS_ORCH_RUN_MAX_PARALLEL + 1),
             &err);
        sql_refused(e.db, atlas_buf_cstr(&sql), "a run above the compiled ceiling");
        atlas_buf_free(&sql);
    }
    {
        atlas_buf sql = ATLAS_BUF_INIT;
        atlas_err err;
        atlas_err_init(&err);
        T_OK(atlas_buf_appendf(&sql, &err, "UPDATE orch_jobs SET run_slot = %d WHERE run_slot = 1;",
                               ATLAS_ORCH_RUN_MAX_PARALLEL),
             &err);
        sql_refused(e.db, atlas_buf_cstr(&sql), "a slot at the compiled ceiling");
        atlas_buf_free(&sql);
    }
    sql_refused(e.db, "UPDATE orch_runs SET max_parallel = 0;", "a run allowing no task");
    sql_refused(e.db, "UPDATE orch_jobs SET run_slot = -1 WHERE run_slot = 1;",
                "a negative slot");

    /* Nothing above changed a row: every one of them was refused whole. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(DISTINCT run_slot) FROM orch_jobs;"), 2);
    T_EQ_INT((int)count_sql(e.db, "SELECT max_parallel FROM orch_runs;"), 2);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_jobs WHERE driver = 'fake-repo';"),
             1);

    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 4: two tasks genuinely overlap, and the ledger says so ---------------- */

/* The latch. Two tasks of one run are simultaneously RUNNING, each holding its
 * own unreleased lease, and the interleaving is produced by the order this
 * thread issued operations in rather than by two threads happening to overlap.
 * There is no sleep, no thread and no timing assumption anywhere in it. */
static void test_two_tasks_of_one_run_are_running_at_the_same_time(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    atlas_buf root_tok = ATLAS_BUF_INIT, sib_tok = ATLAS_BUF_INIT;
    (void)lease(&e, atlas_buf_cstr(&root), "fake-repo", &root_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&root_tok));
    (void)lease(&e, NULL, "fake", &sib_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&sib_tok));

    /* Both are RUNNING at once, and both leases are unreleased at once. The
     * second is the stronger claim: the partial unique index permits one
     * unreleased lease *per job*, and before this season one run could not have
     * two jobs to hold them. */
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_RUNNING);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_RUNNING);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases WHERE released_at IS NULL;"),
             2);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'RUNNING';"), 2);

    atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
    T_EQ_INT((int)rv.active_count, 2);
    T_EQ_INT((int)rv.max_parallel, 2);
    /* The claim target is the repository-tree task, and only ever that one. */
    T_CHECK(strcmp(rv.active_job_uid, atlas_buf_cstr(&root)) == 0);

    /* The sibling ends first, and the run does not settle: something in it has
     * not said what it did. This is A11.6's deferral. */
    T_CHECK(finish(&e, atlas_buf_cstr(&sib_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(run_view(&e, atlas_buf_cstr(&run)).status == ATLAS_ORCH_RUN_ACTIVE);

    /* And now it does. */
    T_CHECK(finish(&e, atlas_buf_cstr(&root_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACCEPTED);

    /* The ledger shows the sibling's whole attempt window nested inside the
     * root's, by ledger id — the AUTOINCREMENT this repository orders history
     * by, never a timestamp. */
    int64_t root_run = ledger_id(&e, atlas_buf_cstr(&root), "RUNNING");
    int64_t sib_run = ledger_id(&e, atlas_buf_cstr(&sib), "RUNNING");
    int64_t sib_end = ledger_id(&e, atlas_buf_cstr(&sib), "SUCCEEDED");
    int64_t root_end = ledger_id(&e, atlas_buf_cstr(&root), "SUCCEEDED");
    T_CHECK(root_run > 0 && sib_run > 0 && sib_end > 0 && root_end > 0);
    T_CHECK_MSG(root_run < sib_run && sib_run < sib_end && sib_end < root_end,
                "the ledger does not show the sibling nested inside the root: "
                "root RUNNING %lld, sibling RUNNING %lld, sibling end %lld, root end %lld",
                (long long)root_run, (long long)sib_run, (long long)sib_end,
                (long long)root_end);

    atlas_buf_free(&sib_tok);
    atlas_buf_free(&root_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 5: settlement waits for every task ------------------------------------ */

static void test_a_run_settles_only_when_nothing_is_left_running(void) {
    env e;
    env_open(&e);

    /* The chain finishes first and the sibling is still going. */
    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    atlas_buf sib_tok = ATLAS_BUF_INIT;
    (void)lease(&e, NULL, "fake", &sib_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&sib_tok));

    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    /* A succeeded chain with a sibling in flight is not an accepted run, and the
     * run driver is told there is nothing here for it to claim. */
    atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
    T_CHECK(rv.status == ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(rv.active_job_uid[0] == '\0');
    T_EQ_INT((int)rv.active_count, 1);

    T_CHECK(finish(&e, atlas_buf_cstr(&sib_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACCEPTED);
    atlas_buf_free(&sib_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);

    /* The mirror: a sibling that ends badly blocks, and not one moment before
     * quiescence. A gateless workspace sibling can veto acceptance and can never
     * grant it. */
    atlas_buf run2 = ATLAS_BUF_INIT, root2 = ATLAS_BUF_INIT, sib2 = ATLAS_BUF_INIT;
    sub root2_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3, .task = "second run"};
    submit(&e, &root2_s, &run2, &root2);
    sub sib2_s = {.parent = atlas_buf_cstr(&root2)};
    submit(&e, &sib2_s, NULL, &sib2);

    atlas_buf root_tok = ATLAS_BUF_INIT;
    (void)lease(&e, atlas_buf_cstr(&root2), "fake-repo", &root_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&root_tok));

    /* The sibling exhausts its one attempt while the chain is mid-flight. The
     * run is not blocked yet: the chain has not said what it did. */
    T_CHECK(run_one(&e, NULL, "fake", false, ATLAS_ORCH_REASON_WORKER_FAILURE) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib2)) == ATLAS_ORCH_STATE_FAILED);
    T_CHECK(run_view(&e, atlas_buf_cstr(&run2)).status == ATLAS_ORCH_RUN_ACTIVE);

    T_CHECK(finish(&e, atlas_buf_cstr(&root_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_BLOCKED);

    atlas_buf_free(&root_tok);
    atlas_buf_free(&sib2);
    atlas_buf_free(&root2);
    atlas_buf_free(&run2);
    env_close(&e);
}

/* --- 6: one task's trouble is not another's -------------------------------- */

static void test_a_siblings_retry_and_failure_leave_the_chain_alone(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root), .attempts = 2};
    submit(&e, &sib_s, NULL, &sib);

    atlas_buf root_tok = ATLAS_BUF_INIT;
    (void)lease(&e, atlas_buf_cstr(&root), "fake-repo", &root_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&root_tok));

    /* The sibling's first attempt fails and it goes back to the queue. */
    T_EQ_INT((int)run_one(&e, NULL, "fake", false, ATLAS_ORCH_REASON_WORKER_FAILURE),
             (int)ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_QUEUED);
    /* The chain is untouched: same state, same unreleased lease. */
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_RUNNING);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases WHERE released_at IS NULL;"),
             1);

    /* Its second attempt exhausts it. Still not a settled run. */
    T_CHECK(run_one(&e, NULL, "fake", false, ATLAS_ORCH_REASON_WORKER_FAILURE) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_FAILED);
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_RUNNING);

    /* The chain still reaches its own verdict; the run pays for the sibling only
     * at the end. A doomed run does not stop the chain mid-run. */
    T_CHECK(finish(&e, atlas_buf_cstr(&root_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_BLOCKED);
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_SUCCEEDED);

    atlas_buf_free(&root_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

static void test_a_failed_gate_still_earns_exactly_one_follow_up_beside_a_sibling(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    atlas_buf sib_tok = ATLAS_BUF_INIT;
    (void)lease(&e, NULL, "fake", &sib_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&sib_tok));

    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", false,
                    ATLAS_ORCH_REASON_VALIDATION_FAILED) == ATLAS_ORCH_RUN_ACTIVE);

    /* Exactly one follow-up, and it continued the chain rather than the sibling.
     * The count is of the root's *repo-tree* children: the sibling is a child of
     * the root too — that is how it joined the run — so counting children alone
     * would count it as a follow-up. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 3);
    {
        atlas_buf sql = ATLAS_BUF_INIT;
        atlas_err err;
        atlas_err_init(&err);
        T_OK(atlas_buf_appendf(&sql, &err,
                               "SELECT count(*) FROM orch_jobs"
                               "  WHERE parent_job_uid = '%s' AND driver = 'fake-repo';",
                               atlas_buf_cstr(&root)),
             &err);
        T_EQ_INT((int)count_sql(e.db, atlas_buf_cstr(&sql)), 1);
        atlas_buf_free(&sql);
    }
    /* The sibling was not touched by any of it. */
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_RUNNING);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_jobs WHERE driver = 'fake-repo' AND "
                            "  state NOT IN ('SUCCEEDED','FAILED','CANCELLED','TIMED_OUT',"
                            "                'RECOVERY_REQUIRED');"),
             1);

    atlas_buf_free(&sib_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* A gate failure is not the only ending a follow-up answers, and narrowing this
 * branch to `VALIDATION_FAILED` would have quietly removed one. A repo-tree task
 * whose attempts ran out while the run still has budget gets its one follow-up
 * exactly as a failed gate does — a crashed worker says nothing specific about
 * the task, which is precisely why another one is worth starting. */
static void test_an_exhausted_repo_tree_task_still_earns_its_follow_up(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .attempts = 1};
    submit(&e, &root_s, &run, &root);

    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", false,
                    ATLAS_ORCH_REASON_WORKER_FAILURE) == ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_FAILED);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 2);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions"
                            "  WHERE to_state = 'FAILED' AND reason = 'ATTEMPTS_EXHAUSTED';"),
             1);

    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 7: cancelling one task ------------------------------------------------ */

static void test_cancelling_a_sibling_leaves_the_chain_running_and_blocks_at_the_end(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    atlas_buf root_tok = ATLAS_BUF_INIT, sib_tok = ATLAS_BUF_INIT;
    (void)lease(&e, atlas_buf_cstr(&root), "fake-repo", &root_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&root_tok));
    (void)lease(&e, NULL, "fake", &sib_tok, NULL);
    advance_to_running(&e, atlas_buf_cstr(&sib_tok));

    {
        atlas_err err;
        atlas_err_init(&err);
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(op != NULL);
        op->peer_uid = 1000;
        op->actor = ATLAS_ORCH_ACTOR_CLIENT;
        T_OK(atlas_buf_set(&op->job_uid, sib.data, sib.len, &err), &err);
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == ATLAS_ORCH_STATE_CANCEL_REQUESTED);
        atlas_orch_result_free(&r);
    }
    /* Cancellation is asked for, never signalled: the chain is not told anything
     * and keeps its lease. */
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_RUNNING);

    T_CHECK(finish(&e, atlas_buf_cstr(&sib_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_CANCELLED);
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_RUNNING);

    /* A cancelled task is an unanswered one, so the run blocks — at quiescence,
     * and not before. */
    T_CHECK(finish(&e, atlas_buf_cstr(&root_tok), true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_BLOCKED);

    atlas_buf_free(&sib_tok);
    atlas_buf_free(&root_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 8: recovery, with time supplied and never slept for ------------------- */

static void test_recovery_judges_each_task_by_its_own_rules_and_waits_for_the_rest(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    /* One attempt only, so its expiry is the ambiguous ending rather than a
     * retry: "we do not know whether this ran" and "this ran and failed" are
     * different answers and Atlas does not collapse them. */
    sub sib_s = {.parent = atlas_buf_cstr(&root), .attempts = 1};
    submit(&e, &sib_s, NULL, &sib);

    atlas_buf root_tok = ATLAS_BUF_INIT, sib_tok = ATLAS_BUF_INIT;
    /* The expiry each lease *ends up with*, read back from the last heartbeat of
     * its phase walk — not the one it was granted with. Walking the phases
     * renews the lease twice against the wall clock, so a grant-time expiry is
     * an expiry no row holds any more: the sibling is leased last, and its two
     * renewals push its stored expiry past `max(grants) + 1`, so the sweep below
     * would find only the chain's lease expired. Measured under
     * ThreadSanitizer, where a renewal costs a whole millisecond: grants at
     * ...983, stored expiry ...986, `now_ms` ...984 — `expired` 1 rather than 2,
     * and three further assertions failing behind it. On the release build the
     * renewals land inside the grant's own millisecond and it passed, which is
     * what made this look like a sanitizer problem rather than a stale read. */
    int64_t root_exp = 0, sib_exp = 0;
    (void)lease(&e, atlas_buf_cstr(&root), "fake-repo", &root_tok, NULL);
    advance_to_running_exp(&e, atlas_buf_cstr(&root_tok), &root_exp);
    (void)lease(&e, NULL, "fake", &sib_tok, NULL);
    advance_to_running_exp(&e, atlas_buf_cstr(&sib_tok), &sib_exp);
    /* A renewal that did not report one would leave a zero here and the sweep
     * would then be asked about a moment before either lease existed, which is a
     * different case that happens to fail the same assertions. */
    T_REQUIRE(root_exp > 0 && sib_exp > 0);

    {
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
        T_REQUIRE(op != NULL);
        op->actor = ATLAS_ORCH_ACTOR_ATLAS;
        op->now_ms = (root_exp > sib_exp ? root_exp : sib_exp) + 1;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_EQ_INT((int)r.expired, 2);
        T_EQ_INT((int)r.retried, 1);   /* the chain, which has attempts left */
        T_EQ_INT((int)r.recovered, 1); /* the sibling, which does not */
        atlas_orch_result_free(&r);
    }
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_QUEUED);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_RECOVERY_REQUIRED);

    /* The run is not blocked yet. Before A11.6 the sibling's RECOVERY_REQUIRED
     * would have ended it on the spot; now the chain is still queued, so there
     * is still something to wait for. */
    T_CHECK(run_view(&e, atlas_buf_cstr(&run)).status == ATLAS_ORCH_RUN_ACTIVE);

    /* The chain runs again and succeeds; the run blocks because the sibling's
     * ending was never answered. */
    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_BLOCKED);

    atlas_buf_free(&sib_tok);
    atlas_buf_free(&root_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 9: whose budget is it ------------------------------------------------- */

/* `ATLAS_ORCH_RUN_MAX_WORKER_STARTS` bounds the repo-tree chain, and a workspace
 * sibling spends none of it. Without the driver filter on the count, the two
 * sibling attempts below would leave the chain with one start instead of three,
 * and the run would be BLOCKED at the first failed gate instead of accepted at
 * the third task. */
static void test_a_siblings_attempts_do_not_spend_the_chains_budget(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root), .attempts = 3};
    submit(&e, &sib_s, NULL, &sib);

    /* Two sibling starts: one failed attempt and one that succeeds. */
    T_CHECK(run_one(&e, NULL, "fake", false, ATLAS_ORCH_REASON_WORKER_FAILURE) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(run_one(&e, NULL, "fake", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_SUCCEEDED);

    /* The chain now takes all three of its own starts: two failed gates, each
     * answered by one follow-up, and a third task that passes. */
    atlas_buf cur = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(&cur, root.data, root.len, &err), &err);
    for (int i = 0; i < 2; i++) {
        T_CHECK(run_one(&e, atlas_buf_cstr(&cur), "fake-repo", false,
                        ATLAS_ORCH_REASON_VALIDATION_FAILED) == ATLAS_ORCH_RUN_ACTIVE);
        atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
        T_REQUIRE(rv.active_job_uid[0] != '\0');
        T_OK(atlas_buf_set_str(&cur, rv.active_job_uid, &err), &err);
    }
    T_CHECK(run_one(&e, atlas_buf_cstr(&cur), "fake-repo", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACCEPTED);

    /* Five worker starts happened in this run and the bound is three, which is
     * only consistent because the bound counts the chain's. */
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions WHERE to_state = 'RUNNING';"),
             5);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions t"
                            "  JOIN orch_jobs j ON j.id = t.job_id"
                            "  WHERE t.to_state = 'RUNNING' AND j.driver = 'fake-repo';"),
             ATLAS_ORCH_RUN_MAX_WORKER_STARTS);

    atlas_buf_free(&cur);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 10: a run holds one pin ---------------------------------------------- */

static void test_a_task_pinned_to_another_commit_is_refused(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);

    sub drifted = {.parent = atlas_buf_cstr(&root),
                   .commit = "0123456789abcdef0123456789abcdef01234567"};
    apply_refused(&e, submit_op(&e, &drifted), "a sibling pinned to another commit",
                  "a run holds one pin");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);

    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 11: the bound is refused, never reduced ------------------------------ */

static void test_a_bound_outside_the_ceiling_is_refused_and_never_clamped(void) {
    env e;
    env_open(&e);

    sub too_many = {.driver = "fake-repo", .attempts = 3,
                    .parallel = ATLAS_ORCH_RUN_MAX_PARALLEL + 1};
    apply_refused(&e, submit_op(&e, &too_many), "a run above the ceiling", "is outside that");
    sub negative = {.driver = "fake-repo", .attempts = 3, .parallel = -1, .task = "negative"};
    apply_refused(&e, submit_op(&e, &negative), "a negative bound", "is outside that");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_runs;"), 0);

    /* Not stated resolves to one, and the ceiling itself is accepted. */
    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT;
    sub unstated = {.driver = "fake-repo", .attempts = 3, .parallel = 0};
    submit(&e, &unstated, &run, &root);
    T_EQ_INT((int)run_view(&e, atlas_buf_cstr(&run)).max_parallel, 1);

    atlas_buf run2 = ATLAS_BUF_INIT;
    sub at_ceiling = {.driver = "fake-repo", .attempts = 3,
                      .parallel = ATLAS_ORCH_RUN_MAX_PARALLEL, .task = "at the ceiling"};
    submit(&e, &at_ceiling, &run2, NULL);
    T_EQ_INT((int)run_view(&e, atlas_buf_cstr(&run2)).max_parallel,
             ATLAS_ORCH_RUN_MAX_PARALLEL);

    /* And it is a property of the run, so a task joining one cannot name it. */
    sub with_parent = {.parent = atlas_buf_cstr(&root), .parallel = 2};
    apply_refused(&e, submit_op(&e, &with_parent), "a bound named on a child",
                  "fixed when the run is created");

    atlas_buf_free(&run2);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 12: the SQL spellings and the C ones -------------------------------- */

/* Three rules live in both C and SQL because SQLite cannot call a C function,
 * and two spellings of one rule drift. They are compared here over the whole of
 * each vocabulary rather than trusted to have been kept in step by hand, which
 * is `test_orch_run.c`'s discipline extended to what migration 24 added. */
static void test_the_index_predicates_match_the_c_predicates(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf slot_sql = ATLAS_BUF_INIT, tree_sql = ATLAS_BUF_INIT;
    text_sql(e.db,
             "SELECT sql FROM sqlite_master WHERE type = 'index'"
             "  AND name = 'idx_orch_jobs_active_slot';",
             &slot_sql);
    text_sql(e.db,
             "SELECT sql FROM sqlite_master WHERE type = 'index'"
             "  AND name = 'idx_orch_jobs_one_active_repo_tree';",
             &tree_sql);
    T_REQUIRE(slot_sql.len > 0);
    T_REQUIRE(tree_sql.len > 0);
    /* The index migration 21 created is gone, replaced rather than joined. */
    {
        atlas_buf old = ATLAS_BUF_INIT;
        text_sql(e.db,
                 "SELECT sql FROM sqlite_master WHERE type = 'index'"
                 "  AND name = 'idx_orch_jobs_one_active_per_run';",
                 &old);
        T_CHECK(old.len == 0);
        atlas_buf_free(&old);
    }

    /* The terminal set, over the whole state vocabulary and in both indexes. */
    static const atlas_orch_state ALL[] = {
        ATLAS_ORCH_STATE_QUEUED,           ATLAS_ORCH_STATE_LEASED,
        ATLAS_ORCH_STATE_PREPARING,        ATLAS_ORCH_STATE_RUNNING,
        ATLAS_ORCH_STATE_VALIDATING,       ATLAS_ORCH_STATE_SUCCEEDED,
        ATLAS_ORCH_STATE_FAILED,           ATLAS_ORCH_STATE_CANCEL_REQUESTED,
        ATLAS_ORCH_STATE_CANCELLED,        ATLAS_ORCH_STATE_TIMED_OUT,
        ATLAS_ORCH_STATE_RECOVERY_REQUIRED};
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        char quoted[ATLAS_ORCH_NAME_MAX + 3u];
        (void)snprintf(quoted, sizeof quoted, "'%s'", atlas_orch_state_name(ALL[i]));
        bool terminal = atlas_orch_state_is_terminal(ALL[i]);
        T_CHECK_MSG((strstr(atlas_buf_cstr(&slot_sql), quoted) != NULL) == terminal,
                    "%s is %s in C and the slot index disagrees",
                    atlas_orch_state_name(ALL[i]), terminal ? "terminal" : "active");
        T_CHECK_MSG((strstr(atlas_buf_cstr(&tree_sql), quoted) != NULL) == terminal,
                    "%s is %s in C and the repo-tree index disagrees",
                    atlas_orch_state_name(ALL[i]), terminal ? "terminal" : "active");
    }

    /* The repo-tree driver list, in both directions. Every driver Atlas has is
     * checked for presence, and the number of names in the SQL clause is
     * compared with the number of repo-tree drivers — so a name in the predicate
     * that is not a driver at all is caught too. */
    const char *clause = strstr(atlas_buf_cstr(&tree_sql), "driver IN (");
    T_REQUIRE(clause != NULL);
    size_t listed = 1u;
    for (const char *p = clause; *p != '\0' && *p != ')'; p++) {
        if (*p == ',') {
            listed++;
        }
    }
    size_t n = 0;
    size_t repo_tree = 0;
    const atlas_driver *const *drivers = atlas_drivers(&n);
    for (size_t i = 0; i < n; i++) {
        char quoted[ATLAS_ORCH_NAME_MAX + 3u];
        (void)snprintf(quoted, sizeof quoted, "'%s'", drivers[i]->name);
        bool is_tree = atlas_orch_driver_is_repo_tree(drivers[i]->name);
        if (is_tree) {
            repo_tree++;
        }
        T_CHECK_MSG((strstr(clause, quoted) != NULL) == is_tree,
                    "driver %s is %s in C and the index predicate disagrees", drivers[i]->name,
                    is_tree ? "repo-tree" : "workspace");
    }
    T_CHECK_MSG(listed == repo_tree,
                "the index predicate names %zu drivers and Atlas has %zu repo-tree ones", listed,
                repo_tree);

    /* The compiled ceiling, duplicated in two table CHECKs. */
    atlas_buf runs_sql = ATLAS_BUF_INIT, jobs_sql = ATLAS_BUF_INIT;
    text_sql(e.db, "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'orch_runs';",
             &runs_sql);
    text_sql(e.db, "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'orch_jobs';",
             &jobs_sql);
    {
        char want[64];
        (void)snprintf(want, sizeof want, "max_parallel <= %d", ATLAS_ORCH_RUN_MAX_PARALLEL);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&runs_sql), want) != NULL,
                    "orch_runs does not carry the compiled ceiling as \"%s\"", want);
        (void)snprintf(want, sizeof want, "run_slot < %d", ATLAS_ORCH_RUN_MAX_PARALLEL);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&jobs_sql), want) != NULL,
                    "orch_jobs does not carry the compiled ceiling as \"%s\"", want);
    }

    atlas_buf_free(&jobs_sql);
    atlas_buf_free(&runs_sql);
    atlas_buf_free(&tree_sql);
    atlas_buf_free(&slot_sql);
    env_close(&e);
}

/* --- 13: what the run view reports --------------------------------------- */

static void test_the_run_view_names_the_claim_target_and_counts_the_rest(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 3, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
    T_CHECK_MSG(strcmp(rv.active_job_uid, atlas_buf_cstr(&root)) == 0,
                "the view named %s rather than the repository-tree task", rv.active_job_uid);
    T_CHECK(rv.active_state == ATLAS_ORCH_STATE_QUEUED);
    T_EQ_INT((int)rv.active_count, 2);
    T_EQ_INT((int)rv.max_parallel, 3);

    /* Once the chain is done the claim target is empty while the count is not,
     * and those two together are what tells a run driver to stop without telling
     * it the run is over. */
    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    rv = run_view(&e, atlas_buf_cstr(&run));
    T_CHECK(rv.active_job_uid[0] == '\0');
    T_EQ_INT((int)rv.active_count, 1);
    T_CHECK(rv.status == ATLAS_ORCH_RUN_ACTIVE);

    /* A run whose root is a workspace driver keeps A11.0's answer: its first
     * active task, and no settlement at all. */
    atlas_buf plain_run = ATLAS_BUF_INIT, plain_root = ATLAS_BUF_INIT;
    sub plain = {.parallel = 2, .task = "a plain A8 run"};
    submit(&e, &plain, &plain_run, &plain_root);
    atlas_orch_run_view pv = run_view(&e, atlas_buf_cstr(&plain_run));
    T_CHECK(strcmp(pv.active_job_uid, atlas_buf_cstr(&plain_root)) == 0);
    T_EQ_INT((int)pv.active_count, 1);
    T_CHECK(run_one(&e, atlas_buf_cstr(&plain_root), "fake", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(run_view(&e, atlas_buf_cstr(&plain_run)).status == ATLAS_ORCH_RUN_ACTIVE);

    atlas_buf_free(&plain_root);
    atlas_buf_free(&plain_run);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

/* --- 14: every terminal producer settles ----------------------------------
 *
 * A11.6 says a run settles exactly when its last task goes terminal, and for a
 * season only three paths honoured it: a completion, and recovery's two sweeps.
 * Five others wrote a terminal state and settled nothing, so a run whose last
 * task ended on one of them stayed ACTIVE with nothing in it — forever, because
 * the only thing that would ever have settled it was a task that had already
 * ended.
 *
 * Pilot A11.6-P is where that stopped being theoretical: a sibling reached its
 * wall deadline on a heartbeat, the run had no other active task, and it was
 * still ACTIVE hours later with an operator waiting on it. The two cases below
 * are the two producers a run driver meets in ordinary use — the wall deadline
 * on a check-in, and a cancellation of a task that was never leased — and both
 * assert the run's verdict rather than only the task's, because the task's was
 * already right.
 */
static void test_a_task_that_hits_its_wall_on_a_heartbeat_blocks_its_run(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    /* A wall bound short enough to fall inside one lease, so every clock this
     * case depends on is a value it states and none of it is waited for. */
    sub sib_s = {.parent = atlas_buf_cstr(&root), .wall_ms = 30000};
    submit(&e, &sib_s, NULL, &sib);

    /* The chain finishes well, and the run does not settle: the sibling has not
     * said what it did. */
    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&root)) == ATLAS_ORCH_STATE_SUCCEEDED);
    T_EQ_INT((int)run_view(&e, atlas_buf_cstr(&run)).active_count, 1);

    /* The sibling runs, one heartbeat short of its wall each time. */
    atlas_buf sib_tok = ATLAS_BUF_INIT;
    (void)lease(&e, NULL, "fake", &sib_tok, NULL);
    int64_t wall = job_deadline_ms(&e, atlas_buf_cstr(&sib));
    T_REQUIRE(wall > 0);
    (void)heartbeat_at(&e, atlas_buf_cstr(&sib_tok), ATLAS_ORCH_STATE_PREPARING, wall - 2);
    (void)heartbeat_at(&e, atlas_buf_cstr(&sib_tok), ATLAS_ORCH_STATE_RUNNING, wall - 1);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_RUNNING);
    T_CHECK(run_view(&e, atlas_buf_cstr(&run)).status == ATLAS_ORCH_RUN_ACTIVE);

    /* And the next one arrives past it. The wall clock is what finally stops a
     * job, and it stops this one without any completion ever being sent. */
    T_CHECK(heartbeat_at(&e, atlas_buf_cstr(&sib_tok), ATLAS_ORCH_STATE_UNKNOWN, wall + 1) ==
            ATLAS_ORCH_STATE_TIMED_OUT);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_TIMED_OUT);
    /* The wall branch and not the renewal bound, which ends a task the same way
     * for a different reason and would prove the wrong thing here. */
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions"
                            "  WHERE to_state = 'TIMED_OUT' AND reason = 'WALL_TIMEOUT';"),
             1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases WHERE released_at IS NULL;"),
             0);

    /* The run is BLOCKED, and this is the assertion the pilot was missing: with
     * nothing active left, a run that stayed ACTIVE here would stay ACTIVE for
     * good — a timed-out task is one nobody answered. */
    atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
    T_EQ_INT((int)rv.active_count, 0);
    T_CHECK_MSG(rv.status == ATLAS_ORCH_RUN_BLOCKED,
                "the run is %s with no active task in it",
                atlas_orch_run_status_name(rv.status));

    atlas_buf_free(&sib_tok);
    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

static void test_a_cancelled_queued_task_settles_its_run_at_quiescence(void) {
    env e;
    env_open(&e);

    atlas_buf run = ATLAS_BUF_INIT, root = ATLAS_BUF_INIT, sib = ATLAS_BUF_INIT;
    sub root_s = {.driver = "fake-repo", .parallel = 2, .attempts = 3};
    submit(&e, &root_s, &run, &root);
    sub sib_s = {.parent = atlas_buf_cstr(&root)};
    submit(&e, &sib_s, NULL, &sib);

    /* The chain finishes well while the sibling is still queued. Nothing has
     * ever leased it, so no worker will ever complete it. */
    T_CHECK(run_one(&e, atlas_buf_cstr(&root), "fake-repo", true, ATLAS_ORCH_REASON_UNKNOWN) ==
            ATLAS_ORCH_RUN_ACTIVE);
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_QUEUED);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);

    /* Cancelling a queued task is the whole of its ending: there is nothing
     * running to ask to stop, so it goes straight to CANCELLED. */
    {
        atlas_err err;
        atlas_err_init(&err);
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(op != NULL);
        op->peer_uid = 1000;
        op->actor = ATLAS_ORCH_ACTOR_CLIENT;
        T_OK(atlas_buf_set(&op->job_uid, sib.data, sib.len, &err), &err);
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == ATLAS_ORCH_STATE_CANCELLED);
        atlas_orch_result_free(&r);
    }
    T_CHECK(job_state(&e, atlas_buf_cstr(&sib)) == ATLAS_ORCH_STATE_CANCELLED);

    /* Immediately, in the cancellation's own transaction: the run is at
     * quiescence the moment that row is written, and a cancelled task is an
     * unanswered one. */
    atlas_orch_run_view rv = run_view(&e, atlas_buf_cstr(&run));
    T_EQ_INT((int)rv.active_count, 0);
    T_CHECK_MSG(rv.status == ATLAS_ORCH_RUN_BLOCKED,
                "the run is %s with no active task in it",
                atlas_orch_run_status_name(rv.status));

    atlas_buf_free(&sib);
    atlas_buf_free(&root);
    atlas_buf_free(&run);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a run admits siblings up to its bound and no further",
     test_a_run_admits_siblings_up_to_its_bound_and_no_further},
    {"a run that asked for nothing still allows exactly one",
     test_a_run_that_asked_for_nothing_still_allows_exactly_one},
    {"the repository tree stays exclusive whatever the bound",
     test_the_repository_tree_stays_exclusive_whatever_the_bound},
    {"the schema refuses what the checks would have",
     test_the_schema_refuses_what_the_checks_would_have},
    {"two tasks of one run are running at the same time",
     test_two_tasks_of_one_run_are_running_at_the_same_time},
    {"a run settles only when nothing is left running",
     test_a_run_settles_only_when_nothing_is_left_running},
    {"a sibling's retry and failure leave the chain alone",
     test_a_siblings_retry_and_failure_leave_the_chain_alone},
    {"a failed gate still earns exactly one follow-up beside a sibling",
     test_a_failed_gate_still_earns_exactly_one_follow_up_beside_a_sibling},
    {"an exhausted repo-tree task still earns its follow-up",
     test_an_exhausted_repo_tree_task_still_earns_its_follow_up},
    {"cancelling a sibling leaves the chain running and blocks at the end",
     test_cancelling_a_sibling_leaves_the_chain_running_and_blocks_at_the_end},
    {"recovery judges each task by its own rules and waits for the rest",
     test_recovery_judges_each_task_by_its_own_rules_and_waits_for_the_rest},
    {"a sibling's attempts do not spend the chain's budget",
     test_a_siblings_attempts_do_not_spend_the_chains_budget},
    {"a task pinned to another commit is refused",
     test_a_task_pinned_to_another_commit_is_refused},
    {"a bound outside the ceiling is refused and never clamped",
     test_a_bound_outside_the_ceiling_is_refused_and_never_clamped},
    {"the index predicates match the C predicates",
     test_the_index_predicates_match_the_c_predicates},
    {"the run view names the claim target and counts the rest",
     test_the_run_view_names_the_claim_target_and_counts_the_rest},
    {"a task that hits its wall on a heartbeat blocks its run",
     test_a_task_that_hits_its_wall_on_a_heartbeat_blocks_its_run},
    {"a cancelled queued task settles its run at quiescence",
     test_a_cancelled_queued_task_settles_its_run_at_quiescence},
};

ATLAS_TEST_MAIN("orch_parallel", TESTS)

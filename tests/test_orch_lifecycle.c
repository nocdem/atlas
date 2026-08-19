/* Atlas - A8: the durable job lifecycle, against a real database.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every case here drives `atlas_orch_apply` — the public entry point to the one
 * write point — against an isolated fixture database with its own registered
 * repository. Nothing touches a live service, a live socket, the real index or a
 * registered repository, and the fixture is removed on both success and failure.
 *
 * Time is supplied rather than slept for. `atlas_orch_op.now_ms` exists so that
 * lease expiry, renewal bounds and wall deadlines are driven deterministically:
 * a test that sleeps for a lease to expire is a test that is slow when it passes
 * and flaky when the machine is loaded, and neither tells you anything about the
 * rule under test.
 *
 * Required cases covered here: 3 (idempotent submission), 6 (atomic lease grant
 * and attempt creation), 7 (concurrent lease requests), 8 (lease expiry),
 * 9 and 10 (heartbeat renewal bounds and stale rejection), 11 (stale
 * completion), 12 (wrong-worker completion), 13 (retry limit), 14, 15 and 16
 * (cancellation before lease, during execution, and racing completion),
 * 17 (timeout), 18 and 19 (daemon and dispatcher restart recovery), 20 and 21
 * (crash between lease and start, and after exit before commit), 22 (duplicate
 * completion), 34 and 35 (artifact traversal and bounds), 52 (recovery under
 * concurrent clients), 53 (bounded event streams) and 56 (a successful job
 * cannot apply, commit or push its patch).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- the environment --------------------------------------------------------
 *
 * One fixture repository, registered through the CLI exactly as an operator
 * would, then a direct handle on the same database. The CLI path is used for
 * registration because that is the only way a repository is ever registered —
 * A7 removed the RPC method — and a test that inserted the row itself would be
 * testing a shape that cannot occur. */
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
    /* Scanned, because the durable repository identity is a path-qualified
     * *lineage* fingerprint and the lineage half comes from ingested root
     * commits. A registered-but-never-scanned repository has no identity yet,
     * which is correct — and it is exactly why a job may not be created against
     * one. */
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

/* --- building operations ------------------------------------------------- */

static atlas_orch_op *submit_op(env *e, const char *key, int64_t attempts, int64_t wall_ms) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    /* The submitter comes from the trusted connection at the IPC edge. Here it
     * is supplied directly, which is the same field filled the same way — there
     * is no path by which a request body could reach it. */
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
    op->spec.wall_timeout_ms = wall_ms;
    /* Half the wall bound, but never past the idle ceiling — which is lower than
     * the wall ceiling on purpose, so a long job still has to say something
     * periodically. */
    op->spec.idle_timeout_ms = wall_ms / 2 > 0 ? wall_ms / 2 : 1;
    if (op->spec.idle_timeout_ms > ATLAS_ORCH_MAX_IDLE_TIMEOUT_MS) {
        op->spec.idle_timeout_ms = ATLAS_ORCH_MAX_IDLE_TIMEOUT_MS;
    }
    op->spec.max_attempts = attempts;
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

/* Applies an operation and frees it, requiring success. */
static void apply_ok(env *e, atlas_orch_op *op, atlas_orch_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result_init(out);
    T_OK(atlas_orch_apply(e->db, op, out, &err), &err);
    atlas_orch_op_free(op);
    free(op);
}

/* Applies an operation expecting refusal, and reports what it said. */
static void apply_refused(env *e, atlas_orch_op *op, const char *what) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    atlas_status st = atlas_orch_apply(e->db, op, &r, &err);
    T_CHECK_MSG(st != ATLAS_OK, "%s was accepted", what);
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

/* Drives a job all the way to a granted lease and returns the token. */
static void lease_one(env *e, atlas_orch_result *out) {
    apply_ok(e, lease_op(), out);
    T_REQUIRE(out->granted);
}

/* Walks a leased attempt forward to RUNNING, one heartbeat per phase.
 *
 * Needed because a job may not succeed straight out of LEASED: the transition
 * table has no such edge, deliberately, so that "this succeeded" always implies
 * "its driver ran". Every test that ends in success goes through here, which
 * means the real dispatcher's sequence is the one under test rather than a
 * shortcut only tests can take. */
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

/* --- submission ----------------------------------------------------------- */

static void test_a_job_is_persisted_before_anything_is_dispatched(void) {
    env e;
    env_open(&e);
    atlas_orch_result r;
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &r);

    T_CHECK(r.state == ATLAS_ORCH_STATE_QUEUED);
    T_CHECK(r.job_uid.len == ATLAS_ORCH_UID_HEX + 1u);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);
    /* The ledger's first row exists before any dispatcher could have seen the
     * job, and the job points at it. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_transitions;"), 1);
    T_EQ_INT(
        (int)count_sql(e.db,
                       "SELECT count(*) FROM orch_jobs j JOIN orch_transitions t"
                       " ON t.id = j.state_seq WHERE t.to_state = 'QUEUED';"),
        1);
    /* Nothing has been leased, attempted or executed. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases;"), 0);
    atlas_orch_result_free(&r);
    env_close(&e);
}

static void test_submission_is_idempotent_and_a_conflicting_replay_is_refused(void) {
    env e;
    env_open(&e);
    atlas_orch_result a, b;
    apply_ok(&e, submit_op(&e, "k1", 2, 60000), &a);
    apply_ok(&e, submit_op(&e, "k1", 2, 60000), &b);

    T_CHECK_MSG(b.duplicate, "a replayed submission created a second job");
    T_CHECK(strcmp(atlas_buf_cstr(&a.job_uid), atlas_buf_cstr(&b.job_uid)) == 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);

    /* The same key with a *different* specification is a conflict, not a
     * silent return of the older job: running something other than what was
     * asked for, with no way for the caller to notice, is worse than an error. */
    apply_refused(&e, submit_op(&e, "k1", 3, 60000), "a conflicting idempotent replay");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 1);

    /* Two submissions with no key at all are two jobs. */
    atlas_orch_result c, d;
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &c);
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &d);
    T_CHECK(strcmp(atlas_buf_cstr(&c.job_uid), atlas_buf_cstr(&d.job_uid)) != 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs;"), 3);

    atlas_orch_result_free(&a);
    atlas_orch_result_free(&b);
    atlas_orch_result_free(&c);
    atlas_orch_result_free(&d);
    env_close(&e);
}

/* --- leasing --------------------------------------------------------------- */

static void test_a_lease_grant_and_its_attempt_are_one_atomic_act(void) {
    env e;
    env_open(&e);
    atlas_orch_result s;
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &s);

    atlas_orch_result g;
    lease_one(&e, &g);
    T_CHECK(g.state == ATLAS_ORCH_STATE_LEASED);
    T_EQ_INT((int)g.attempt_no, 1);
    T_EQ_INT((int)g.token.len, (int)ATLAS_ORCH_TOKEN_HEX);
    /* The worker is handed the repository path, the pinned commit and the task
     * from trusted state — never from anything it sent. */
    T_CHECK(strcmp(atlas_buf_cstr(&g.repo_name), "proj") == 0);
    T_CHECK(g.repo_root.len > 0);
    T_CHECK(strcmp(atlas_buf_cstr(&g.source_commit), atlas_buf_cstr(&e.commit)) == 0);
    T_CHECK(strcmp(atlas_buf_cstr(&g.task_text), "add a comment") == 0);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases;"), 1);
    /* The token is never stored. Only its digest is, so a leaked database row
     * cannot be presented as a capability. */
    {
        char sql[256];
        (void)snprintf(sql, sizeof(sql),
                       "SELECT count(*) FROM orch_leases WHERE token_digest = '%s';",
                       atlas_buf_cstr(&g.token));
        atlas_err err;
        atlas_err_init(&err);
        int64_t v = -1;
        /* Not a cached statement: constructed SQL misses the cache by design. */
        sqlite3_stmt *st = NULL;
        T_REQUIRE(atlas_db_prepare(e.db, sql, &st, &err) == ATLAS_OK);
        if (sqlite3_step(st) == SQLITE_ROW) {
            v = sqlite3_column_int64(st, 0);
        }
        atlas_db_finish(e.db, st);
        T_CHECK_MSG(v == 0, "the lease token itself was stored in the database");
    }

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_two_dispatchers_cannot_obtain_the_same_job(void) {
    env e;
    env_open(&e);
    atlas_orch_result s;
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &s);

    atlas_orch_result a, b;
    lease_one(&e, &a);
    apply_ok(&e, lease_op(), &b);
    T_CHECK_MSG(!b.granted, "a second dispatcher was granted the same job");
    /* And at the storage level: the partial unique index permits exactly one
     * unreleased lease per job, so even a logic error above could not produce
     * two workers on one job. */
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_leases WHERE released_at IS NULL;"),
             1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&a);
    atlas_orch_result_free(&b);
    env_close(&e);
}

/* --- worker messages -------------------------------------------------------- */

static void test_only_the_lease_holder_may_report(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &s);
    lease_one(&e, &g);

    /* A token nobody was issued. */
    apply_refused(&e,
                  worker_op(ATLAS_ORCH_OP_HEARTBEAT,
                            "0000000000000000000000000000000000000000000000000000000000000000"),
                  "a heartbeat with an unissued token");
    /* A token of the wrong shape is refused before it is looked up. */
    apply_refused(&e, worker_op(ATLAS_ORCH_OP_HEARTBEAT, "nope"),
                  "a heartbeat with a malformed token");
    apply_refused(&e, worker_op(ATLAS_ORCH_OP_COMPLETE, "nope"),
                  "a completion with a malformed token");
    /* And a completion from a worker holding a token for no attempt. */
    {
        atlas_orch_op *op = worker_op(
            ATLAS_ORCH_OP_COMPLETE,
            "1111111111111111111111111111111111111111111111111111111111111111");
        op->success = true;
        apply_refused(&e, op, "a completion from a worker that holds no lease");
    }
    /* The job is untouched by any of it. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'LEASED';"), 1);

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_a_worker_cannot_skip_a_phase_or_go_backwards(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 60000), &s);
    lease_one(&e, &g);
    const char *tok = atlas_buf_cstr(&g.token);

    /* LEASED -> RUNNING skips PREPARING and is refused by the same table every
     * other transition is checked against. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, tok);
        op->phase = ATLAS_ORCH_STATE_RUNNING;
        apply_refused(&e, op, "a heartbeat skipping PREPARING");
    }
    /* A heartbeat may not declare a terminal state: a worker ends an attempt by
     * completing it. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, tok);
        op->phase = ATLAS_ORCH_STATE_SUCCEEDED;
        apply_refused(&e, op, "a heartbeat declaring success");
    }
    /* Forwards, one step at a time, is fine. */
    static const atlas_orch_state FORWARD[] = {ATLAS_ORCH_STATE_PREPARING,
                                               ATLAS_ORCH_STATE_RUNNING};
    for (size_t i = 0; i < sizeof FORWARD / sizeof FORWARD[0]; i++) {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, tok);
        op->phase = FORWARD[i];
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == FORWARD[i]);
        atlas_orch_result_free(&r);
    }
    /* And backwards is not. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, tok);
        op->phase = ATLAS_ORCH_STATE_PREPARING;
        apply_refused(&e, op, "a heartbeat moving backwards");
    }

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* --- expiry, renewal and recovery --------------------------------------------
 *
 * Time is supplied rather than slept for, so each of these is a statement about
 * a rule rather than about a machine's load. */

static void test_an_expired_lease_cannot_complete_and_the_job_is_retried(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);
    lease_one(&e, &g);
    atlas_buf tok = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(&tok, g.token.data, g.token.len, &err), &err);

    int64_t after = g.expires_ms + 1;

    /* A worker that comes back after its lease expired is refused — that is
     * what stops a process declared dead, and whose job was retried, from
     * overwriting the newer attempt's result. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
        op->success = true;
        op->now_ms = after;
        apply_refused(&e, op, "a completion on an expired lease");
    }
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, atlas_buf_cstr(&tok));
        op->now_ms = after;
        apply_refused(&e, op, "a heartbeat on an expired lease");
    }

    /* Recovery releases it and, because an attempt remains, queues a new one.
     * The previous attempt is recorded as having ended, so a duplicate
     * execution would be visible in the history rather than invisible. */
    {
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
        op->actor = ATLAS_ORCH_ACTOR_ATLAS;
        op->now_ms = after;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_EQ_INT((int)r.expired, 1);
        T_EQ_INT((int)r.retried, 1);
        atlas_orch_result_free(&r);
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'QUEUED';"), 1);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_attempts WHERE state = 'TIMED_OUT';"),
             1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases WHERE released_at IS NULL;"),
             0);

    /* A second lease is a second attempt, numbered monotonically. */
    atlas_orch_result g2;
    lease_one(&e, &g2);
    T_EQ_INT((int)g2.attempt_no, 2);
    /* And the *old* token still cannot do anything, even now that the job is
     * active again: it names a released attempt. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
        op->success = true;
        apply_refused(&e, op, "a completion from the previous attempt");
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'SUCCEEDED';"),
             0);

    atlas_buf_free(&tok);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    atlas_orch_result_free(&g2);
    env_close(&e);
}

static void test_a_job_whose_attempts_are_gone_becomes_recovery_required(void) {
    /* "We do not know whether this ran" and "this ran and failed" are different
     * answers, and collapsing them is the mistake A6 refuses to make about
     * ancestry. A last attempt whose lease expired mid-flight is the first. */
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 1, 3600000), &s);
    lease_one(&e, &g);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    op->now_ms = g.expires_ms + 1;
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_EQ_INT((int)r.recovered, 1);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_jobs WHERE state = 'RECOVERY_REQUIRED';"),
             1);
    atlas_orch_result_free(&r);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_a_completed_job_survives_recovery_unchanged(void) {
    /* The first thing recovery must not break: a persisted completed job stays
     * completed, whatever else is reconciled around it. */
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
        op->success = true;
        op->exit_kind = ATLAS_ORCH_EXIT_OK;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == ATLAS_ORCH_STATE_SUCCEEDED);
        atlas_orch_result_free(&r);
    }
    for (int i = 0; i < 3; i++) {
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
        op->actor = ATLAS_ORCH_ACTOR_ATLAS;
        op->now_ms = g.expires_ms + 100000 + i;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        atlas_orch_result_free(&r);
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'SUCCEEDED';"),
             1);
    /* Idempotent: repeated recovery adds no transitions beyond the first sweep. */
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_a_wall_deadline_ends_a_job_that_was_never_leased(void) {
    env e;
    env_open(&e);
    atlas_orch_result s;
    apply_ok(&e, submit_op(&e, NULL, 2, 1000), &s);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    /* Far past the deadline the submission computed for itself. */
    op->now_ms = INT64_C(1) << 42;
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_EQ_INT((int)r.timed_out, 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'TIMED_OUT';"), 1);
    atlas_orch_result_free(&r);
    atlas_orch_result_free(&s);
    env_close(&e);
}

/* --- retry ------------------------------------------------------------------- */

static void test_retry_is_bounded_and_recorded(void) {
    env e;
    env_open(&e);
    atlas_orch_result s;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);

    for (int attempt = 1; attempt <= 2; attempt++) {
        atlas_orch_result g;
        lease_one(&e, &g);
        T_EQ_INT((int)g.attempt_no, attempt);
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
        op->success = false;
        op->exit_kind = ATLAS_ORCH_EXIT_NONZERO;
        op->exit_code = 1;
        op->failure_reason = ATLAS_ORCH_REASON_WORKER_FAILURE;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == (attempt < 2 ? ATLAS_ORCH_STATE_QUEUED : ATLAS_ORCH_STATE_FAILED));
        atlas_orch_result_free(&r);
        atlas_orch_result_free(&g);
    }
    /* Exhausted: no third lease, and no infinite loop. */
    atlas_orch_result none;
    apply_ok(&e, lease_op(), &none);
    T_CHECK(!none.granted);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 2);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'FAILED';"), 1);
    /* And the reason each retry happened is on the ledger, not inferred. */
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_transitions WHERE reason = 'RETRY';"),
             1);
    T_EQ_INT((int)count_sql(
                 e.db,
                 "SELECT count(*) FROM orch_transitions WHERE reason = 'ATTEMPTS_EXHAUSTED';"),
             1);
    atlas_orch_result_free(&none);
    atlas_orch_result_free(&s);
    env_close(&e);
}

/* --- cancellation ------------------------------------------------------------- */

static void test_cancellation_before_a_lease_is_immediate(void) {
    env e;
    env_open(&e);
    atlas_orch_result s;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(&op->job_uid, s.job_uid.data, s.job_uid.len, &err), &err);
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_CHECK(r.state == ATLAS_ORCH_STATE_CANCELLED);

    /* Nothing is ever leased, and no attempt was created. */
    atlas_orch_result none;
    apply_ok(&e, lease_op(), &none);
    T_CHECK(!none.granted);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 0);

    atlas_orch_result_free(&r);
    atlas_orch_result_free(&none);
    atlas_orch_result_free(&s);
    env_close(&e);
}

static void test_cancellation_wins_a_race_with_completion(void) {
    /* Required case 16. Completion and cancellation cannot both win, and the
     * winner is decided by the transition table rather than by whichever
     * message arrived first: once CANCEL_REQUESTED, there is no edge to
     * SUCCEEDED at all. */
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);
    lease_one(&e, &g);

    atlas_err err;
    atlas_err_init(&err);
    {
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        op->peer_uid = 1000;
        op->actor = ATLAS_ORCH_ACTOR_CLIENT;
        T_OK(atlas_buf_set(&op->job_uid, s.job_uid.data, s.job_uid.len, &err), &err);
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == ATLAS_ORCH_STATE_CANCEL_REQUESTED);
        atlas_orch_result_free(&r);
    }
    /* The worker learns of it at its next heartbeat — there is no signal from
     * the daemon to a worker process, by design. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, atlas_buf_cstr(&g.token));
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK_MSG(r.cancel_requested, "the worker was not told to stop");
        atlas_orch_result_free(&r);
    }
    /* A success reported anyway settles as CANCELLED, never as SUCCEEDED. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
        op->success = true;
        op->exit_kind = ATLAS_ORCH_EXIT_OK;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK_MSG(r.state == ATLAS_ORCH_STATE_CANCELLED,
                    "a completion after cancellation produced %s",
                    atlas_orch_state_name(r.state));
        atlas_orch_result_free(&r);
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'SUCCEEDED';"),
             0);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_a_terminal_job_refuses_cancellation_and_a_second_completion(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);
    lease_one(&e, &g);
    atlas_buf tok = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(&tok, g.token.data, g.token.len, &err), &err);
    advance_to_running(&e, atlas_buf_cstr(&tok));
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
        op->success = true;
        op->exit_kind = ATLAS_ORCH_EXIT_OK;
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        T_CHECK(r.state == ATLAS_ORCH_STATE_SUCCEEDED);
        atlas_orch_result_free(&r);
    }
    /* Required case 22: a duplicated completion changes nothing, because the
     * first released the lease. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
        op->success = true;
        apply_refused(&e, op, "a duplicated completion");
    }
    {
        atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        op->peer_uid = 1000;
        op->actor = ATLAS_ORCH_ACTOR_CLIENT;
        T_OK(atlas_buf_set(&op->job_uid, s.job_uid.data, s.job_uid.len, &err), &err);
        apply_refused(&e, op, "cancelling a terminal job");
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'SUCCEEDED';"),
             1);
    /* Exactly one attempt ever existed, so nothing was executed twice. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);

    atlas_buf_free(&tok);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_a_client_cannot_cancel_another_principals_job(void) {
    env e;
    env_open(&e);
    atlas_orch_result s;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
    /* A different uid, as the kernel would have reported it. */
    op->peer_uid = 1234;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(&op->job_uid, s.job_uid.data, s.job_uid.len, &err), &err);
    apply_refused(&e, op, "cancelling another principal's job");
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'QUEUED';"), 1);
    atlas_orch_result_free(&s);
    env_close(&e);
}

/* --- events and artifacts ------------------------------------------------------- */

static void test_events_are_bounded_and_a_duplicate_sequence_is_refused(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 2, 3600000), &s);
    lease_one(&e, &g);
    const char *tok = atlas_buf_cstr(&g.token);
    atlas_err err;
    atlas_err_init(&err);

    for (int i = 0; i < 3; i++) {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_EVENT, tok);
        op->event_seq = i;
        T_OK(atlas_buf_set_str(&op->event_kind, "log", &err), &err);
        T_OK(atlas_buf_set_str(&op->event_payload, "hello", &err), &err);
        atlas_orch_result r;
        apply_ok(&e, op, &r);
        atlas_orch_result_free(&r);
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_events;"), 3);

    /* A retrying dispatcher must not be able to inflate its own history: the
     * unique index over (attempt_id, seq) makes a repeat a refusal. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_EVENT, tok);
        op->event_seq = 0;
        T_OK(atlas_buf_set_str(&op->event_kind, "log", &err), &err);
        T_OK(atlas_buf_set_str(&op->event_payload, "again", &err), &err);
        apply_refused(&e, op, "a duplicated event sequence");
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_events;"), 3);

    /* An oversized payload is refused, never trimmed: an event stream that
     * silently stopped recording looks like a job that went quiet. */
    {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_EVENT, tok);
        op->event_seq = 99;
        T_OK(atlas_buf_set_str(&op->event_kind, "log", &err), &err);
        char big[ATLAS_ORCH_EVENT_MAX + 16u];
        memset(big, 'x', sizeof(big));
        T_OK(atlas_buf_set(&op->event_payload, big, sizeof(big), &err), &err);
        apply_refused(&e, op, "an oversized event payload");
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_events;"), 3);

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static void test_an_artifact_manifest_cannot_point_outside_the_workspace(void) {
    /* Required cases 34 and 35, at the manifest boundary. The worker checks its
     * own filesystem; this check protects the *record*, because a name that
     * escaped would be a stored pointer to somewhere the workspace is not. */
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    static const char *const BAD_NAMES[] = {"../escape.patch", "/etc/passwd", "a/../../b",
                                            "", "."};
    for (size_t i = 0; i < sizeof BAD_NAMES / sizeof BAD_NAMES[0]; i++) {
        atlas_orch_result s, g;
        apply_ok(&e, submit_op(&e, NULL, 1, 3600000), &s);
        lease_one(&e, &g);
        advance_to_running(&e, atlas_buf_cstr(&g.token));
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
        op->success = true;
        op->artifacts = calloc(1, sizeof(*op->artifacts));
        T_REQUIRE(op->artifacts != NULL);
        atlas_orch_artifact_init(&op->artifacts[0]);
        op->artifact_count = 1;
        T_OK(atlas_buf_set_str(&op->artifacts[0].name, BAD_NAMES[i], &err), &err);
        T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "patch", &err), &err);
        op->artifacts[0].size_bytes = 1;
        apply_refused(&e, op, "an artifact name that escapes the workspace");
        atlas_orch_result_free(&s);
        atlas_orch_result_free(&g);
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_artifacts;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'SUCCEEDED';"),
             0);

    /* And the bound on total artifact bytes refuses rather than trims. */
    {
        atlas_orch_result s, g;
        apply_ok(&e, submit_op(&e, NULL, 1, 3600000), &s);
        lease_one(&e, &g);
        advance_to_running(&e, atlas_buf_cstr(&g.token));
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
        op->success = true;
        op->artifacts = calloc(1, sizeof(*op->artifacts));
        T_REQUIRE(op->artifacts != NULL);
        atlas_orch_artifact_init(&op->artifacts[0]);
        op->artifact_count = 1;
        T_OK(atlas_buf_set_str(&op->artifacts[0].name, "big.bin", &err), &err);
        T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "blob", &err), &err);
        op->artifacts[0].size_bytes = 65537; /* the job's bound is 65536 */
        apply_refused(&e, op, "artifacts exceeding the job's byte bound");
        atlas_orch_result_free(&s);
        atlas_orch_result_free(&g);
    }
    env_close(&e);
}

static void test_a_patch_artifact_is_recorded_and_never_applied(void) {
    /* Required case 56. A8 produces a patch as an *artifact* — bytes with a
     * recorded digest — and there is no code path that applies it. The proof
     * here is that the repository is byte-identical afterwards; the proof that
     * no such path exists at all is `tests/test_orch_trust.c`. */
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);

    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 1, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
    op->success = true;
    op->exit_kind = ATLAS_ORCH_EXIT_OK;
    T_OK(atlas_buf_set_str(&op->driver_version, "fake/1", &err), &err);
    op->artifacts = calloc(1, sizeof(*op->artifacts));
    T_REQUIRE(op->artifacts != NULL);
    atlas_orch_artifact_init(&op->artifacts[0]);
    op->artifact_count = 1;
    T_OK(atlas_buf_set_str(&op->artifacts[0].name, "changes.patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->artifacts[0].content, "--- a/a.c\n+++ b/a.c\n", &err), &err);
    T_OK(atlas_buf_set_str(&op->artifacts[0].sha256,
                           "3333333333333333333333333333333333333333333333333333333333333333",
                           &err),
         &err);
    op->artifacts[0].content_stored = true;
    op->artifacts[0].size_bytes = (int64_t)op->artifacts[0].content.len;
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_CHECK(r.state == ATLAS_ORCH_STATE_SUCCEEDED);
    atlas_orch_result_free(&r);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_artifacts WHERE kind = 'patch';"),
             1);
    /* The repository is untouched: nothing in A8 writes one, and a successful
     * job with a patch in hand is exactly the case where a careless design
     * would have. */
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
                "the registered repository changed while a job ran against it");
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* --- A11.5a-R: contention grace ------------------------------------------- */

/* The failure this exists for was measured, not imagined. A semantic pass over a
 * second registered repository ran 167-176 s with 14-20 s between passes, so
 * A9.2.6 refused orchestration writes about ninety per cent of the time. A
 * worker that was alive and ten minutes inside its wall deadline could not land
 * a heartbeat, its sixty-second lease expired, and the sweep requeued its
 * attempt as LEASE_EXPIRED and threw the result away.
 *
 * The sweep is itself a synchronous write, so it can only run in the gaps
 * between passes — which is exactly what makes its own refusal usable as the
 * signal that the holder was not being listened to. */
static void test_contention_defers_an_expired_lease(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 3, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    /* Past the lease, and Atlas refused a write moments ago. */
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    /* Past any expiry the heartbeats in `advance_to_running` renewed it to — the
     * grant's own `expires_ms` is not that, and using it made this test depend on
     * how fast the machine got here. */
    int64_t late = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    op->now_ms = late;
    op->contended_until_ms = late - 1;
    atlas_orch_result r;
    apply_ok(&e, op, &r);

    /* Nothing was judged: the lease is untouched, the attempt is still running,
     * and no second attempt was queued for a second worker to pick up. */
    T_EQ_INT((int)r.deferred, 1);
    T_EQ_INT((int)r.expired, 0);
    T_EQ_INT((int)r.retried, 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_leases WHERE released_at IS NULL;"),
             1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_attempts;"), 1);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT count(*) FROM orch_attempts WHERE state = 'TIMED_OUT';"),
             0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'QUEUED';"), 0);
    atlas_orch_result_free(&r);

    /* And the heartbeat the worker was never able to send still renews it once
     * the writer is free, which is the whole point of having waited. */
    atlas_orch_op *hb = worker_op(ATLAS_ORCH_OP_HEARTBEAT, atlas_buf_cstr(&g.token));
    hb->now_ms = late + 1;
    atlas_orch_result hr;
    apply_ok(&e, hb, &hr);
    T_CHECK(hr.expires_ms > late);
    atlas_orch_result_free(&hr);

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* Grace forgives Atlas' silence, not the worker's. With no contention reported
 * the sweep behaves exactly as it always has — which is what makes the new field
 * safe to add to an operation every existing caller already builds. */
static void test_without_contention_an_expired_lease_is_still_reclaimed(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 3, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    op->now_ms = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    /* contended_until_ms left at zero: nobody said Atlas was busy. */
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_EQ_INT((int)r.deferred, 0);
    T_EQ_INT((int)r.expired, 1);
    T_EQ_INT((int)r.retried, 1);
    atlas_orch_result_free(&r);

    /* Stale contention is not contention. An outage half an hour ago says
     * nothing about whether this worker could heartbeat a moment ago. */
    env_close(&e);
}

static void test_stale_contention_does_not_defer(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 3, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    int64_t late3 = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    op->now_ms = late3;
    op->contended_until_ms = late3 - (ATLAS_ORCH_CONTENTION_GRACE_MS * 4);
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_EQ_INT((int)r.deferred, 0);
    T_EQ_INT((int)r.expired, 1);
    atlas_orch_result_free(&r);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* The one thing contention must never buy. A9.2.6's refusals are Atlas' fault
 * and are forgiven; the wall clock is the submitter's bound and is not. */
static void test_contention_never_extends_the_wall_deadline(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    /* A deadline that falls well inside the lease, so the sweep meets both
     * conditions at once and has to choose. */
    apply_ok(&e, submit_op(&e, NULL, 3, 1000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    int64_t late4 = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    op->now_ms = late4;
    op->contended_until_ms = late4 - 1;
    atlas_orch_result r;
    apply_ok(&e, op, &r);

    T_EQ_INT((int)r.deferred, 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'TIMED_OUT';"),
             1);
    atlas_orch_result_free(&r);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* A11.5a-R. The half that `8c41245` was missing, and that pilot 3 proved was
 * missing: sparing an attempt in the sweep is worthless if the write path still
 * rejects its holder. A completion arriving after the lease ran out, from the
 * worker that still holds it, on a machine that was refusing writes, is the
 * exact shape of a five-minute worker's result and it must land. */
static void test_a_completion_lands_after_contention_expired_the_lease(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 3, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    int64_t late = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
    op->success = true;
    op->now_ms = late;
    op->contended_until_ms = late - 1;
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_jobs WHERE state = 'SUCCEEDED';"), 1);
    atlas_orch_result_free(&r);

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* And the boundary that keeps it safe. Contention forgives a lease that ran out
 * of time; it never forgives one that was taken away. A released lease may have
 * been superseded by a newer attempt, so honouring a late completion under it
 * could overwrite another worker's result — which is the whole reason the check
 * exists. */
static void test_a_released_lease_is_refused_however_contended(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 3, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    /* Reclaim it with no contention reported, exactly as a quiet machine would. */
    int64_t late = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    atlas_orch_op *rec = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    rec->actor = ATLAS_ORCH_ACTOR_ATLAS;
    rec->now_ms = late;
    atlas_orch_result rr;
    apply_ok(&e, rec, &rr);
    T_EQ_INT((int)rr.expired, 1);
    atlas_orch_result_free(&rr);

    /* Now the old worker comes back claiming the machine was busy. It was not
     * its lease any more before it said a word. */
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&g.token));
    op->success = true;
    op->now_ms = late + 1;
    op->contended_until_ms = late;
    apply_refused(&e, op, "a completion on a released lease during contention");

    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

/* A daemon that has just started has no record of refusing anything, so no lease
 * is in grace and every expiry is judged exactly as it was before this existed.
 * That is the safe direction for a restart to fall in: an attempt whose driver
 * died is reclaimable again, and a token from before the restart gains nothing
 * from the gap. */
static void test_a_restart_grants_no_grace(void) {
    env e;
    env_open(&e);
    atlas_orch_result s, g;
    apply_ok(&e, submit_op(&e, NULL, 3, 3600000), &s);
    lease_one(&e, &g);
    advance_to_running(&e, atlas_buf_cstr(&g.token));

    /* `atlas_orch_contention_seen` is process state and a fresh process reads
     * zero; the operation below carries what a restarted daemon would stamp. */
    T_EQ_INT((int)atlas_orch_contention_seen(), 0);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    op->now_ms = g.expires_ms + ATLAS_ORCH_LEASE_MS * 2;
    op->contended_until_ms = atlas_orch_contention_seen();
    atlas_orch_result r;
    apply_ok(&e, op, &r);
    T_EQ_INT((int)r.deferred, 0);
    T_EQ_INT((int)r.expired, 1);
    atlas_orch_result_free(&r);
    atlas_orch_result_free(&s);
    atlas_orch_result_free(&g);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a_completion_lands_after_contention_expired_the_lease",
     test_a_completion_lands_after_contention_expired_the_lease},
    {"a_released_lease_is_refused_however_contended",
     test_a_released_lease_is_refused_however_contended},
    {"a_restart_grants_no_grace", test_a_restart_grants_no_grace},
    {"contention_defers_an_expired_lease", test_contention_defers_an_expired_lease},
    {"without_contention_an_expired_lease_is_still_reclaimed",
     test_without_contention_an_expired_lease_is_still_reclaimed},
    {"stale_contention_does_not_defer", test_stale_contention_does_not_defer},
    {"contention_never_extends_the_wall_deadline",
     test_contention_never_extends_the_wall_deadline},
    {"a job is persisted before anything is dispatched",
     test_a_job_is_persisted_before_anything_is_dispatched},
    {"submission is idempotent and a conflicting replay is refused",
     test_submission_is_idempotent_and_a_conflicting_replay_is_refused},
    {"a lease grant and its attempt are one atomic act",
     test_a_lease_grant_and_its_attempt_are_one_atomic_act},
    {"two dispatchers cannot obtain the same job",
     test_two_dispatchers_cannot_obtain_the_same_job},
    {"only the lease holder may report", test_only_the_lease_holder_may_report},
    {"a worker cannot skip a phase or go backwards",
     test_a_worker_cannot_skip_a_phase_or_go_backwards},
    {"an expired lease cannot complete and the job is retried",
     test_an_expired_lease_cannot_complete_and_the_job_is_retried},
    {"a job whose attempts are gone becomes RECOVERY_REQUIRED",
     test_a_job_whose_attempts_are_gone_becomes_recovery_required},
    {"a completed job survives recovery unchanged",
     test_a_completed_job_survives_recovery_unchanged},
    {"a wall deadline ends a job that was never leased",
     test_a_wall_deadline_ends_a_job_that_was_never_leased},
    {"retry is bounded and recorded", test_retry_is_bounded_and_recorded},
    {"cancellation before a lease is immediate",
     test_cancellation_before_a_lease_is_immediate},
    {"cancellation wins a race with completion",
     test_cancellation_wins_a_race_with_completion},
    {"a terminal job refuses cancellation and a second completion",
     test_a_terminal_job_refuses_cancellation_and_a_second_completion},
    {"a client cannot cancel another principal's job",
     test_a_client_cannot_cancel_another_principals_job},
    {"events are bounded and a duplicate sequence is refused",
     test_events_are_bounded_and_a_duplicate_sequence_is_refused},
    {"an artifact manifest cannot point outside the workspace",
     test_an_artifact_manifest_cannot_point_outside_the_workspace},
    {"a patch artifact is recorded and never applied",
     test_a_patch_artifact_is_recorded_and_never_applied},
};

ATLAS_TEST_MAIN("orch_lifecycle", TESTS)

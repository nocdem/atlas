/* Atlas - A12.0: migration 25, the plan's one write point, and the derived status.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Three claims are under test here, and they are separable on purpose.
 *
 *   **The schema is the guarantee.** Every bound migration 25 states is asserted
 *   by driving a value one past it straight at SQLite with the C write point
 *   bypassed entirely. The checks in `db_plan.c` exist so a caller gets a
 *   sentence; the CHECKs are what make the rules true, and that is M21's and
 *   M24's arrangement carried forward.
 *
 *   **Only a planner-role job can produce a revision.** Six binding refusals,
 *   each driven through the real write point against real `orch_jobs`,
 *   `orch_attempts` and `orch_artifacts` rows produced by the real
 *   orchestration write point. An executor job's artifact can never become a
 *   plan, and the reason it cannot is that every one of these checks lives
 *   inside the transaction that would write the revision.
 *
 *   **A plan's status is derived and nothing writes it.** Every status value is
 *   produced from stored rows, and the last case in the file asserts that
 *   exactly one file in `src/` writes the three tables at all.
 *
 * Nothing here sleeps, creates a worker process, touches a live service, a
 * socket, the real index or a registered repository. Every interleaving is
 * produced by issuing operations in an order this thread chose.
 *
 * **Every specification built here is validated.** The submit helper calls
 * `atlas_orch_spec_validate` on every plan-correlated specification, which is
 * the check a real submission meets at the IPC edge *and* the check
 * `spawn_follow_up` applies to the correlation a follow-up inherits. A
 * correlation this suite can store is therefore one a repo-tree plan task could
 * carry into the follow-up a failed gate earns. The worst case — the full
 * identifier, the last revision and a 32-character key, 62 bytes — has a case of
 * its own.
 *
 * Every correlation in this file is built through
 * `atlas_plan_correlation_planner` / `atlas_plan_correlation_task` and never
 * written out, so the format has exactly one spelling and moving it moves those
 * two functions and no test.
 */
#define _GNU_SOURCE 1

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/driver.h"
#include "atlas/orch_ops.h"
#include "atlas/plan.h"
#include "atlas/sha256.h"
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

static bool object_exists(atlas_db *db, const char *type, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM sqlite_schema WHERE type = ?1 AND name = ?2;", &q,
                         &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, type, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
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

static void sql_refused_f(atlas_db *db, const char *what, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void sql_refused_f(atlas_db *db, const char *what, const char *fmt, ...) {
    char sql[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(sql, sizeof sql, fmt, ap);
    va_end(ap);
    sql_refused(db, sql, what);
}

/* --- the operator's gate floor --------------------------------------------- */

/* `n` gates, encoded exactly as `--gate` would arrive from the CLI. `true` is on
 * the validation allowlist and is never executed here: this file drives the
 * write point, not a gate runner. */
static void floor_of(int n, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_argv v[ATLAS_ORCH_MAX_VALIDATIONS];
    memset(v, 0, sizeof v);
    T_REQUIRE(n >= 0 && n <= (int)ATLAS_ORCH_MAX_VALIDATIONS);
    for (int i = 0; i < n; i++) {
        T_OK(atlas_orch_argv_push(&v[i], "make", 4u, &err), &err);
        char arg[32];
        (void)snprintf(arg, sizeof arg, "floor%d", i);
        T_OK(atlas_orch_argv_push(&v[i], arg, strlen(arg), &err), &err);
    }
    T_OK(atlas_orch_validations_encode(v, (size_t)n, out, &err), &err);
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&v[i]);
    }
}

/* --- creating a plan -------------------------------------------------------- */

typedef struct plan_req {
    const char *goal;    /* NULL is a fixed sentence */
    int gates;           /* -1 means "no floor at all"; 0..8 otherwise */
    int parallel;        /* 0 is "not stated" */
    const char *repo;    /* NULL is "proj" */
    bool no_identity;
} plan_req;

static void create_op(env *e, const plan_req *r, atlas_plan_op *op) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plan_op_init(op, ATLAS_PLAN_OP_CREATE);
    /* From SO_PEERCRED at the IPC edge, never from a request body. */
    op->submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->repo_name, r->repo != NULL ? r->repo : "proj", &err), &err);
    if (!r->no_identity) {
        T_OK(atlas_buf_set(&op->repo_identity_hash, e->identity.data, e->identity.len, &err), &err);
    }
    T_OK(atlas_buf_set_str(&op->goal_text,
                           r->goal != NULL ? r->goal : "make the thing work end to end", &err),
         &err);
    if (r->gates >= 0) {
        floor_of(r->gates, &op->gate_floor);
    }
    op->max_parallel = r->parallel;
}

static void plan_create(env *e, const plan_req *r, atlas_buf *uid_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plan_op op;
    create_op(e, r, &op);
    atlas_plan_result res;
    atlas_plan_result_init(&res);
    T_OK(atlas_plan_apply(e->db, &op, &res, &err), &err);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.plan_uid.data, res.plan_uid.len, &err), &err);
    }
    atlas_plan_result_free(&res);
    atlas_plan_op_free(&op);
}

static void plan_create_refused(env *e, const plan_req *r, const char *what, const char *expect) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plan_op op;
    create_op(e, r, &op);
    atlas_plan_result res;
    atlas_plan_result_init(&res);
    atlas_status st = atlas_plan_apply(e->db, &op, &res, &err);
    T_CHECK_MSG(st != ATLAS_OK, "%s was accepted", what);
    if (st != ATLAS_OK && expect != NULL) {
        T_CHECK_MSG(strstr(atlas_err_msg(&err), expect) != NULL,
                    "%s was refused with \"%s\", which does not mention \"%s\"", what,
                    atlas_err_msg(&err), expect);
    }
    /* A refused creation is not a document refusal, so nothing typed comes back
     * with it: the two kinds of refusal are told apart by exactly this. */
    T_CHECK_MSG(res.refusal.len == 0, "%s produced a document refusal: %s", what,
                atlas_buf_cstr(&res.refusal));
    atlas_plan_result_free(&res);
    atlas_plan_op_free(&op);
}

/* --- submitting a job that carries a plan correlation ----------------------- */

typedef struct sub {
    const char *key;
    const char *parent;
    const char *driver;      /* NULL is "fake", an A8 workspace driver */
    const char *correlation; /* NULL is none */
    int64_t attempts;        /* 0 is 1 */
    int64_t parallel;        /* 0 is "not stated" */
    int64_t artifact_bytes;  /* 0 is 65536 */
} sub;

static atlas_orch_op *submit_op(env *e, const sub *s) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = 1;
    op->run_max_parallel = s->parallel;
    op->spec.submitter_uid = 1000;
    const char *driver = s->driver != NULL ? s->driver : "fake";
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e->identity.data, e->identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, e->commit.data, e->commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, driver, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, "do the thing", &err), &err);
    /* A repo-tree task with no gate is refused at the write point, which is
     * A11.1's rule. It is never run here. */
    if (atlas_orch_driver_is_repo_tree(driver)) {
        T_OK(atlas_orch_argv_push(&op->spec.validations[0], "make", 4u, &err), &err);
        T_OK(atlas_orch_argv_push(&op->spec.validations[0], "pass", 4u, &err), &err);
        op->spec.validation_count = 1;
    }
    op->spec.wall_timeout_ms = 3600000;
    op->spec.idle_timeout_ms = 900000;
    op->spec.max_attempts = s->attempts > 0 ? s->attempts : 1;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = s->artifact_bytes > 0 ? s->artifact_bytes : 65536;
    op->spec.max_artifact_count = 8;
    if (s->key != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.idempotency_key, s->key, &err), &err);
    }
    if (s->parent != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.parent_job_uid, s->parent, &err), &err);
    }
    if (s->correlation != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.correlation, s->correlation, &err), &err);
    }
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    /* The check a real submission meets at the IPC edge, and the same one
     * `spawn_follow_up` applies to a correlation a follow-up inherits. Asserted
     * on every specification this file builds, so a plan correlation that could
     * not survive a gate failure could not be stored here either. */
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);
    return op;
}

/* The correlation format has to fit inside what a job specification may carry:
 * `is_name`, at most `ATLAS_ORCH_NAME_MAX`. This is the worst case the builders
 * can produce — the full plan identifier, the last revision, and a task key at
 * the parser's own ceiling — and it is asserted against the validator rather
 * than against a number this test chose. */
static void test_the_worst_case_correlation_fits_a_specification(void) {
    atlas_err err;
    atlas_err_init(&err);

    static const char UID[] = "p0123456789abcdef0123456789abcdef";
    static const char KEY[] = "abcdefghij-klmnopqrst-uvwxyz0123"; /* exactly 32 */
    T_EQ_INT((int)strlen(UID), 33);
    T_EQ_INT((int)strlen(KEY), 32);

    atlas_buf corr = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_task(UID, ATLAS_PLAN_MAX_REVISIONS, KEY, &corr, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&corr), "plan.p0123456789abcdef0123.r3.abcdefghij-klmnopqrst-uvwxyz0123");
    T_EQ_INT((int)corr.len, 62);
    T_CHECK_MSG(corr.len <= ATLAS_ORCH_NAME_MAX, "the worst-case correlation is %zu bytes against "
                                                 "a bound of %u",
                corr.len, (unsigned)ATLAS_ORCH_NAME_MAX);

    /* Against the validator itself, not against the arithmetic above: the bound
     * and the charset are its rules, and a test that restated them would pass by
     * agreeing with itself. */
    {
        atlas_orch_spec s;
        atlas_orch_spec_init(&s);
        s.spec_version = ATLAS_ORCH_SPEC_VERSION;
        s.submitter_uid = 1000;
        T_OK(atlas_buf_set_str(&s.repo_name, "proj", &err), &err);
        T_OK(atlas_buf_set_str(&s.repo_identity_hash,
                               "0123456789abcdef0123456789abcdef"
                               "0123456789abcdef0123456789abcdef",
                               &err),
             &err);
        T_OK(atlas_buf_set_str(&s.source_commit, "0123456789abcdef0123456789abcdef01234567", &err),
             &err);
        T_OK(atlas_buf_set_str(&s.mode, "patch", &err), &err);
        T_OK(atlas_buf_set_str(&s.driver, "fake", &err), &err);
        T_OK(atlas_buf_set_str(&s.task_text, "do the thing", &err), &err);
        s.wall_timeout_ms = 3600000;
        s.idle_timeout_ms = 900000;
        s.max_attempts = 1;
        s.max_output_bytes = 65536;
        s.max_artifact_bytes = 65536;
        s.max_artifact_count = 8;
        T_OK(atlas_buf_set(&s.correlation, corr.data, corr.len, &err), &err);
        /* The same string serves the idempotency key, which carries the same
         * bound and the same charset. */
        T_OK(atlas_buf_set(&s.idempotency_key, corr.data, corr.len, &err), &err);
        T_OK(atlas_orch_spec_validate(&s, &err), &err);
        atlas_orch_spec_free(&s);
    }

    /* The planner form, and the guard both builders share: the identifier is
     * spelled into a name here and nowhere else, so this is where a uid that is
     * not `'p'` plus hex has to be refused. */
    T_OK(atlas_plan_correlation_planner(UID, ATLAS_PLAN_MAX_PLANNER_JOBS, &corr, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&corr), "plan.p0123456789abcdef0123.planner.5");
    T_EQ_INT((int)corr.len, 36);

    T_FAILS_WITH(atlas_plan_correlation_planner("p0123", 1, &corr, &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_correlation_planner("r0123456789abcdef0123456789abcdef", 1, &corr,
                                                &err),
                 ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_correlation_task("p0123456789abcdefzzzz", 1, "k", &corr, &err),
                 ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_plan_correlation_task(UID, 1, "has.a.dot", &corr, &err), ATLAS_ERR_USAGE,
                 &err);
    T_FAILS_WITH(atlas_plan_correlation_task(UID, ATLAS_PLAN_MAX_REVISIONS + 1, "k", &corr, &err),
                 ATLAS_ERR_USAGE, &err);
    atlas_buf_free(&corr);
}

static void apply_ok(env *e, atlas_orch_op *op, atlas_orch_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result_init(out);
    T_OK(atlas_orch_apply(e->db, op, out, &err), &err);
    atlas_orch_op_free(op);
    free(op);
}

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

static int64_t lease(env *e, const char *job_uid, const char *driver, atlas_buf *token_out) {
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
 * such edge — so every attempt walks the phases a real dispatcher walks. */
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

/* What a completion carries as its artifact, when it carries one. */
typedef struct art {
    const char *name;  /* NULL is none at all */
    const char *bytes; /* the document, when stored inline */
    bool stored;
    int64_t size;      /* 0 means "the length of `bytes`" */
} art;

static void finish(env *e, const char *token, bool success, const art *a) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, token);
    op->success = success;
    op->failure_reason = success ? ATLAS_ORCH_REASON_UNKNOWN : ATLAS_ORCH_REASON_WORKER_FAILURE;
    op->exit_kind = success ? ATLAS_ORCH_EXIT_OK : ATLAS_ORCH_EXIT_NONZERO;
    if (a != NULL && a->name != NULL) {
        op->artifacts = (atlas_orch_artifact *)calloc(1u, sizeof(atlas_orch_artifact));
        T_REQUIRE(op->artifacts != NULL);
        atlas_orch_artifact_init(&op->artifacts[0]);
        op->artifact_count = 1u;
        T_OK(atlas_buf_set_str(&op->artifacts[0].name, a->name, &err), &err);
        T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "plan", &err), &err);
        size_t n = a->bytes != NULL ? strlen(a->bytes) : 0u;
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(a->bytes != NULL ? (const void *)a->bytes : "", n, hex);
        T_OK(atlas_buf_set_str(&op->artifacts[0].sha256, hex, &err), &err);
        op->artifacts[0].size_bytes = a->size > 0 ? a->size : (int64_t)n;
        op->artifacts[0].content_stored = a->stored;
        if (a->stored && a->bytes != NULL) {
            T_OK(atlas_buf_set(&op->artifacts[0].content, a->bytes, n, &err), &err);
        }
    }
    atlas_orch_result r;
    apply_ok(e, op, &r);
    atlas_orch_result_free(&r);
}

/* --- a planner job, from submission to a stored artifact -------------------- */

typedef struct planner {
    int k;                   /* 1..ATLAS_PLAN_MAX_PLANNER_JOBS */
    const char *driver;      /* NULL is "fake-plan", the planner-role test driver */
    bool succeed;            /* false leaves the job FAILED */
    bool stop_before_finish; /* leave it RUNNING */
    const art *artifact;     /* NULL is none */
    int64_t artifact_bytes;  /* the job's own bound; 0 is 65536 */
    const char *correlation; /* NULL is this plan's planner correlation for k */
} planner;

static void planner_job(env *e, const char *plan_uid, const planner *p, atlas_buf *job_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf corr = ATLAS_BUF_INIT;
    if (p->correlation != NULL) {
        T_OK(atlas_buf_set_str(&corr, p->correlation, &err), &err);
    } else {
        T_OK(atlas_plan_correlation_planner(plan_uid, p->k, &corr, &err), &err);
    }
    const char *driver = p->driver != NULL ? p->driver : "fake-plan";
    atlas_buf job = ATLAS_BUF_INIT;
    sub s = {.driver = driver,
             .correlation = atlas_buf_cstr(&corr),
             .artifact_bytes = p->artifact_bytes};
    submit(e, &s, NULL, &job);

    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease(e, atlas_buf_cstr(&job), driver, &tok);
    advance_to_running(e, atlas_buf_cstr(&tok));
    if (!p->stop_before_finish) {
        finish(e, atlas_buf_cstr(&tok), p->succeed, p->artifact);
    }
    atlas_buf_free(&tok);
    if (job_out != NULL) {
        T_OK(atlas_buf_set(job_out, job.data, job.len, &err), &err);
    }
    atlas_buf_free(&job);
    atlas_buf_free(&corr);
}

/* --- the canned plan documents ---------------------------------------------
 *
 * One stage, one tree task and one workspace sibling: with `max_parallel = 2`
 * the side bound is `min(3, max_parallel - 1)`, which is one. */
static const char PLAN_ONE_STAGE[] = "atlas-plan-1\n"
                                     "stage: 1\n"
                                     "task: build\n"
                                     "kind: tree\n"
                                     "title: Build the thing\n"
                                     "gate: make test\n"
                                     "prompt<<\n"
                                     "do the work\n"
                                     ">>\n"
                                     "task: notes\n"
                                     "kind: side\n"
                                     "title: Take notes\n"
                                     "prompt<<\n"
                                     "write it down\n"
                                     ">>\n";

/* Same document with no gate at all on the tree task, for the merged-bound case:
 * the floor alone is what pushes it over. */
static const char PLAN_ONE_GATE[] = "atlas-plan-1\n"
                                    "stage: 1\n"
                                    "task: build\n"
                                    "kind: tree\n"
                                    "title: Build the thing\n"
                                    "gate: ctest\n"
                                    "prompt<<\n"
                                    "do the work\n"
                                    ">>\n";

static const char PLAN_REFUSED[] = "atlas-plan-1\n"
                                   "stage: 1\n"
                                   "task: build\n"
                                   "kind: sideways\n"
                                   "title: Build the thing\n"
                                   "prompt<<\n"
                                   "do the work\n"
                                   ">>\n";

/* --- ingesting a revision --------------------------------------------------- */

static void revision_op(atlas_plan_op *op, const char *plan_uid, const char *job_uid, int rev_no,
                        atlas_plan_revision_reason reason) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plan_op_init(op, ATLAS_PLAN_OP_REVISION_ADD);
    op->submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->plan_uid, plan_uid, &err), &err);
    T_OK(atlas_buf_set_str(&op->planner_job_uid, job_uid, &err), &err);
    op->rev_no = rev_no;
    op->reason = reason;
}

static void revision_add(env *e, const char *plan_uid, const char *job_uid, int rev_no,
                         atlas_plan_revision_reason reason, atlas_plan_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plan_op op;
    revision_op(&op, plan_uid, job_uid, rev_no, reason);
    atlas_plan_result_init(out);
    T_OK(atlas_plan_apply(e->db, &op, out, &err), &err);
    atlas_plan_op_free(&op);
}

/* Applies expecting refusal. `document` says whether the refusal should have
 * come back typed — a planner's mistake the driver can quote into a retry
 * prompt — or as a caller's error with nothing typed beside it. */
static void revision_refused(env *e, const char *plan_uid, const char *job_uid, int rev_no,
                             atlas_plan_revision_reason reason, bool document, const char *what,
                             const char *expect, atlas_plan_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plan_op op;
    revision_op(&op, plan_uid, job_uid, rev_no, reason);
    atlas_plan_result res;
    atlas_plan_result_init(&res);
    atlas_status st = atlas_plan_apply(e->db, &op, &res, &err);
    T_CHECK_MSG(st != ATLAS_OK, "%s was accepted", what);
    if (st != ATLAS_OK && expect != NULL) {
        T_CHECK_MSG(strstr(atlas_err_msg(&err), expect) != NULL,
                    "%s was refused with \"%s\", which does not mention \"%s\"", what,
                    atlas_err_msg(&err), expect);
    }
    T_CHECK_MSG((res.refusal.len > 0) == document,
                "%s: a document refusal was %sexpected, and the typed sentence was \"%s\"", what,
                document ? "" : "not ", atlas_buf_cstr(&res.refusal));
    /* Whatever was refused, nothing was written. */
    if (out != NULL) {
        *out = res;
    } else {
        atlas_plan_result_free(&res);
    }
    atlas_plan_op_free(&op);
}

/* --- 1: the migration ------------------------------------------------------- */

static void test_migration_25_adds_its_tables_and_the_correlation_index(void) {
    env e;
    env_open(&e);

    atlas_err err;
    atlas_err_init(&err);
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(ATLAS_SCHEMA_VERSION, 25);

    T_CHECK(object_exists(e.db, "table", "orch_plans"));
    T_CHECK(object_exists(e.db, "table", "orch_plan_revisions"));
    T_CHECK(object_exists(e.db, "table", "orch_plan_tasks"));
    T_CHECK(object_exists(e.db, "index", "idx_orch_plans_repo"));
    T_CHECK(object_exists(e.db, "index", "idx_orch_plan_tasks_rev"));
    /* On `orch_jobs`, and the only thing migration 25 adds outside its own
     * tables: it is what makes the plan-to-job mapping a cheap derived read. */
    T_CHECK(object_exists(e.db, "index", "idx_orch_jobs_correlation"));

    /* Migration 25 is additive. Nothing that existed before it moved, and in
     * particular no run and no job was backfilled into a plan. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plans;"), 0);
    T_CHECK(object_exists(e.db, "index", "idx_orch_jobs_active_slot"));
    T_CHECK(object_exists(e.db, "index", "idx_orch_jobs_one_active_repo_tree"));

    env_close(&e);
}

/* --- 2: the schema is the guarantee ----------------------------------------- */

static void test_the_constants_and_the_checks_agree(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);
    int64_t plan_id = count_sql(e.db, "SELECT id FROM orch_plans;");
    T_REQUIRE(plan_id > 0);

    /* Every literal in migration 25 is written from the constant that governs
     * it, so raising one here can never quietly disagree with the schema. */
    sql_refused_f(e.db, "a plan above the compiled parallelism ceiling",
                  "UPDATE orch_plans SET max_parallel = %d;", ATLAS_ORCH_RUN_MAX_PARALLEL + 1);
    sql_refused(e.db, "UPDATE orch_plans SET max_parallel = 0;", "a plan allowing no task");

    sql_refused_f(e.db, "a revision past the compiled revision bound",
                  "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason,"
                  "  content, content_sha256, created_at)"
                  "  VALUES(%lld, %d, 'j1', 'INITIAL', x'00', 'h', 't');",
                  (long long)plan_id, ATLAS_PLAN_MAX_REVISIONS + 1);
    sql_refused_f(e.db, "a revision numbered zero",
                  "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason,"
                  "  content, content_sha256, created_at)"
                  "  VALUES(%lld, 0, 'j1', 'INITIAL', x'00', 'h', 't');",
                  (long long)plan_id);
    /* The superseded vocabulary. A refused parse writes no revision at all, so
     * there is no reason for one — and the CHECK says so rather than the
     * comment alone. */
    sql_refused_f(e.db, "a revision claiming a refused parse",
                  "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason,"
                  "  content, content_sha256, created_at)"
                  "  VALUES(%lld, 1, 'j1', 'PARSE_REFUSED', x'00', 'h', 't');",
                  (long long)plan_id);

    /* One real revision, so the task CHECKs have a parent to hang off. */
    {
        char sql[512];
        atlas_err err2;
        atlas_err_init(&err2);
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason,"
                       "  content, content_sha256, created_at)"
                       "  VALUES(%lld, 1, 'j1', 'INITIAL', x'00', 'h', 't');",
                       (long long)plan_id);
        T_OK(atlas_db_exec_sql(e.db, sql, &err2), &err2);
    }
    int64_t rev_id = count_sql(e.db, "SELECT id FROM orch_plan_revisions;");

    sql_refused_f(e.db, "a stage past the compiled stage bound",
                  "INSERT INTO orch_plan_tasks(revision_id, plan_id, stage_no, task_key, kind,"
                  "  title, prompt, validations) VALUES(%lld, %lld, %d, 'k', 'TREE', 't', 'p', '');",
                  (long long)rev_id, (long long)plan_id, ATLAS_PLAN_MAX_STAGES + 1);
    sql_refused_f(e.db, "a stage numbered zero",
                  "INSERT INTO orch_plan_tasks(revision_id, plan_id, stage_no, task_key, kind,"
                  "  title, prompt, validations) VALUES(%lld, %lld, 0, 'k', 'TREE', 't', 'p', '');",
                  (long long)rev_id, (long long)plan_id);
    sql_refused_f(e.db, "a task of a kind that is neither",
                  "INSERT INTO orch_plan_tasks(revision_id, plan_id, stage_no, task_key, kind,"
                  "  title, prompt, validations)"
                  "  VALUES(%lld, %lld, 1, 'k', 'OTHER', 't', 'p', '');",
                  (long long)rev_id, (long long)plan_id);

    /* Nothing above changed a row: every one was refused whole. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_tasks;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT max_parallel FROM orch_plans;"), 2);

    atlas_buf_free(&plan);
    env_close(&e);
}

/* --- 3: creating a plan ------------------------------------------------------ */

static void test_a_plan_stores_what_the_operator_brought(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.goal = "make the parser total", .gates = 2, .parallel = 3};
    plan_create(&e, &r, &plan);

    /* 'p' plus 32 lowercase hex, so a plan can never be mistaken for a run, a
     * job, a commit or a content hash. */
    T_EQ_INT((int)plan.len, 33);
    T_CHECK(atlas_buf_cstr(&plan)[0] == 'p');
    for (size_t i = 1; i < plan.len; i++) {
        char c = atlas_buf_cstr(&plan)[i];
        T_CHECK_MSG((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
                    "plan uid %s is not lowercase hex", atlas_buf_cstr(&plan));
    }

    atlas_buf got = ATLAS_BUF_INIT;
    text_sql(e.db, "SELECT goal_text FROM orch_plans;", &got);
    T_EQ_STR(atlas_buf_cstr(&got), "make the parser total");
    text_sql(e.db, "SELECT repo_name FROM orch_plans;", &got);
    T_EQ_STR(atlas_buf_cstr(&got), "proj");
    T_EQ_INT((int)count_sql(e.db, "SELECT max_parallel FROM orch_plans;"), 3);
    T_EQ_INT((int)count_sql(e.db, "SELECT submitter_uid FROM orch_plans;"), 1000);

    /* A plan with no stated bound runs its stages two at a time, which is what
     * lets a stage have the workspace sibling a plan exists to describe. */
    atlas_buf second = ATLAS_BUF_INIT;
    plan_req plain = {.gates = 1};
    plan_create(&e, &plain, &second);
    T_EQ_INT((int)count_sql(e.db,
                            "SELECT max_parallel FROM orch_plans ORDER BY id DESC LIMIT 1;"),
             ATLAS_PLAN_DEFAULT_PARALLEL);

    atlas_buf_free(&second);
    atlas_buf_free(&got);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_plan_without_an_operator_gate_is_refused(void) {
    env e;
    env_open(&e);

    /* The season's whole authority argument, stated as a refusal: a plan with
     * no operator gate could only ever be accepted on a model's word. */
    plan_req none = {.gates = -1};
    plan_create_refused(&e, &none, "a plan with no gate floor at all", "at least one gate");
    plan_req empty = {.gates = 0};
    plan_create_refused(&e, &empty, "a plan with an empty gate floor", "at least one gate");

    plan_req no_goal = {.goal = "", .gates = 1};
    plan_create_refused(&e, &no_goal, "a plan with no goal", "needs a goal");

    plan_req no_repo = {.repo = "", .gates = 1};
    plan_create_refused(&e, &no_repo, "a plan naming no repository", "names the repository");

    plan_req no_id = {.gates = 1, .no_identity = true};
    plan_create_refused(&e, &no_id, "a plan with no repository identity", "durable identity");

    /* Refused, never clamped: a discarded number nobody is told about is a plan
     * that runs differently from the one that was asked for. */
    plan_req too_wide = {.gates = 1, .parallel = ATLAS_ORCH_RUN_MAX_PARALLEL + 1};
    plan_create_refused(&e, &too_wide, "a plan above the parallelism ceiling", "between 1 and");
    plan_req negative = {.gates = 1, .parallel = -1};
    plan_create_refused(&e, &negative, "a plan with a negative bound", "between 1 and");

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plans;"), 0);
    env_close(&e);
}

static void test_an_over_long_goal_is_refused_rather_than_truncated(void) {
    env e;
    env_open(&e);

    char *big = (char *)malloc((size_t)ATLAS_PLAN_GOAL_MAX + 2u);
    T_REQUIRE(big != NULL);
    memset(big, 'g', (size_t)ATLAS_PLAN_GOAL_MAX + 1u);
    big[ATLAS_PLAN_GOAL_MAX + 1] = '\0';
    plan_req r = {.goal = big, .gates = 1};
    plan_create_refused(&e, &r, "a goal one byte past the bound", "at most");
    free(big);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plans;"), 0);
    env_close(&e);
}

/* --- 4: only a planner-role job can produce a revision ----------------------- */

static void test_a_planners_artifact_compiles_into_tasks(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    art a = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE, .stored = true};
    planner p = {.k = 1, .succeed = true, .artifact = &a};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &job);

    atlas_plan_result res;
    revision_add(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                 ATLAS_PLAN_REVISION_INITIAL, &res);
    T_EQ_INT(res.rev_no, 1);
    T_EQ_INT(res.task_count, 2);
    T_EQ_STR(atlas_buf_cstr(&res.plan_uid), atlas_buf_cstr(&plan));
    T_CHECK(res.refusal.len == 0);
    atlas_plan_result_free(&res);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 1);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_tasks;"), 2);

    /* The revision keeps the planner's bytes verbatim, with their digest. */
    T_EQ_INT((int)count_sql(e.db, "SELECT length(content) FROM orch_plan_revisions;"),
             (int)strlen(PLAN_ONE_STAGE));
    {
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(PLAN_ONE_STAGE, strlen(PLAN_ONE_STAGE), hex);
        atlas_buf got = ATLAS_BUF_INIT;
        text_sql(e.db, "SELECT content_sha256 FROM orch_plan_revisions;", &got);
        T_EQ_STR(atlas_buf_cstr(&got), hex);
        text_sql(e.db, "SELECT planner_job_uid FROM orch_plan_revisions;", &got);
        T_EQ_STR(atlas_buf_cstr(&got), atlas_buf_cstr(&job));
        atlas_buf_free(&got);
    }

    /* **The floor is prepended verbatim and first.** The planner's own gate
     * follows it; nothing replaced it and nothing reordered it. */
    {
        atlas_buf enc = ATLAS_BUF_INIT;
        text_sql(e.db, "SELECT validations FROM orch_plan_tasks WHERE kind = 'TREE';", &enc);
        atlas_orch_argv v[ATLAS_ORCH_MAX_VALIDATIONS];
        memset(v, 0, sizeof v);
        size_t n = 0;
        atlas_err err;
        atlas_err_init(&err);
        T_OK(atlas_orch_validations_decode(atlas_buf_cstr(&enc), v, ATLAS_ORCH_MAX_VALIDATIONS, &n,
                                           &err),
             &err);
        T_EQ_INT((int)n, 2);
        T_EQ_STR(atlas_buf_cstr(&v[0].args[0]), "make");
        T_EQ_STR(atlas_buf_cstr(&v[0].args[1]), "floor0");
        T_EQ_STR(atlas_buf_cstr(&v[1].args[0]), "make");
        T_EQ_STR(atlas_buf_cstr(&v[1].args[1]), "test");
        for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
            atlas_orch_argv_free(&v[i]);
        }
        atlas_buf_free(&enc);
    }

    /* A side task declares no gate and one is not invented for it: it runs in a
     * workspace it cannot leave, so a gate there would prove nothing about the
     * repository. */
    {
        atlas_buf enc = ATLAS_BUF_INIT;
        text_sql(e.db, "SELECT validations FROM orch_plan_tasks WHERE kind = 'SIDE';", &enc);
        T_EQ_STR(atlas_buf_cstr(&enc), "");
        atlas_buf_free(&enc);
    }

    /* The same ingest offered twice is refused rather than writing a second
     * revision holding the same document. */
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                     ATLAS_PLAN_REVISION_INITIAL, false, "a repeated ingest of revision 1",
                     "the next one is 2", NULL);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 1);

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_only_a_planner_job_of_this_plan_can_produce_a_revision(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT, other = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);
    plan_create(&e, &r, &other);

    art good = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE, .stored = true};

    /* (a) A plan nobody created. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 1, .succeed = true, .artifact = &good};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, "pdeadbeef", atlas_buf_cstr(&job), 1, ATLAS_PLAN_REVISION_INITIAL,
                         false, "a revision for a plan that does not exist", "no plan named", NULL);
        /* (b) The right job, offered to the wrong plan: the correlation names
         * the plan it was submitted for, and nothing can move it afterwards. */
        revision_refused(&e, atlas_buf_cstr(&other), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false,
                         "another plan's planner job", "is not a planner job of plan", NULL);
        atlas_buf_free(&job);
    }

    /* (c) A job with no plan correlation at all. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 1, .succeed = true, .artifact = &good, .correlation = "unrelated"};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false, "a job carrying no plan correlation",
                         "is not a planner job of plan", NULL);
        atlas_buf_free(&job);
    }

    /* (d) An executor job, correctly correlated, holding a perfectly good plan
     * document. The role is asked of the driver name the job **stored**. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 2, .driver = "fake", .succeed = true, .artifact = &good};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false, "an executor job's artifact",
                         "is not a planner", NULL);
        atlas_buf_free(&job);
    }

    /* (e) A planner job that failed. A zero exit is not a success claim
     * anywhere in Atlas, and this is the state Atlas itself recorded. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 3, .succeed = false, .artifact = &good};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false, "a planner job that FAILED",
                         "only a job that SUCCEEDED", NULL);
        atlas_buf_free(&job);
    }

    /* (f) A planner job still running. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 4, .stop_before_finish = true};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false, "a planner job still running",
                         "only a job that SUCCEEDED", NULL);
        atlas_buf_free(&job);
    }

    /* (g) A job that does not exist at all. */
    revision_refused(&e, atlas_buf_cstr(&plan), "jffffffffffffffffffffffffffffffff", 1,
                     ATLAS_PLAN_REVISION_INITIAL, false, "a job that does not exist",
                     "no job named", NULL);

    /* (h) A revision that records no reason. UNKNOWN is the vocabulary's zero. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 5, .succeed = true, .artifact = &good};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_UNKNOWN, false, "a revision with no reason",
                         "records why it exists", NULL);
        atlas_buf_free(&job);
    }

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_tasks;"), 0);

    atlas_buf_free(&other);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_the_artifact_must_be_present_stored_and_within_bounds(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    /* (a) No artifact at all. **Typed**, because it is the planner that failed
     * to produce the file it was asked for, and the driver answers a typed
     * refusal with one more planner rather than by ending the plan. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        planner p = {.k = 1, .succeed = true};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        atlas_plan_result res;
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, true, "a planner job with no artifact",
                         "produced no artifact named", &res);
        T_CHECK_MSG(res.refusal_line == 0, "the missing artifact named line %d", res.refusal_line);
        T_CHECK(strstr(atlas_buf_cstr(&res.refusal), "produced no artifact named") != NULL);
        atlas_plan_result_free(&res);
        atlas_buf_free(&job);
    }

    /* (b) An artifact under a different name. Only one name is read, and a plan
     * written *anywhere* but the collected one reads exactly like this: it is
     * the shape the live pilot produced, and it is the planner's to answer. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        art wrong = {.name = "notes.txt", .bytes = PLAN_ONE_STAGE, .stored = true};
        planner p = {.k = 2, .succeed = true, .artifact = &wrong};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, true, "an artifact under another name",
                         "produced no artifact named", NULL);
        atlas_buf_free(&job);
    }

    /* (c) Described but not stored, which is what happens above the inline
     * ceiling. Atlas has the name, the size and the digest and not the bytes. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        art described = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE,
                         .stored = false};
        planner p = {.k = 3, .succeed = true, .artifact = &described};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false, "an artifact whose bytes were not kept",
                         "without storing its bytes", NULL);
        atlas_buf_free(&job);
    }

    /* (d) A document past the format's own ceiling, declared by the completion
     * and never trusted from the parser's side alone. */
    {
        atlas_buf job = ATLAS_BUF_INIT;
        art big = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE, .stored = true,
                   .size = (int64_t)ATLAS_PLAN_MAX_BYTES + 1};
        planner p = {.k = 4, .succeed = true, .artifact = &big,
                     .artifact_bytes = (int64_t)ATLAS_PLAN_MAX_BYTES * 4};
        planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
        revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                         ATLAS_PLAN_REVISION_INITIAL, false, "an over-long plan document",
                         "at most", NULL);
        atlas_buf_free(&job);
    }

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 0);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_refused_document_comes_back_typed_and_writes_nothing(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    art bad = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_REFUSED, .stored = true};
    planner p = {.k = 1, .succeed = true, .artifact = &bad};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &job);

    atlas_plan_result res;
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                     ATLAS_PLAN_REVISION_INITIAL, true, "an unparseable plan document", NULL, &res);
    /* The line travels apart from the sentence, so the driver never has to read
     * Atlas' prose to recover it. `kind: sideways` is the fourth line. */
    T_EQ_INT(res.refusal_line, 4);
    T_CHECK_MSG(res.refusal.len > 0, "a refused document produced no sentence");
    atlas_plan_result_free(&res);

    /* A refused parse aborts the transaction, so there is no revision — which is
     * precisely why the refused state has to be *derived* from a planner job no
     * revision names. */
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_tasks;"), 0);

    /* Deterministic: the same stored bytes give the same refusal on every read,
     * which is what lets a resumed driver re-obtain it instead of remembering
     * it. */
    atlas_plan_result again;
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                     ATLAS_PLAN_REVISION_INITIAL, true, "the same document offered again", NULL,
                     &again);
    T_EQ_INT(again.refusal_line, 4);
    atlas_plan_result_free(&again);

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_the_merged_gate_list_is_bounded_and_the_refusal_does_the_arithmetic(void) {
    env e;
    env_open(&e);

    /* A floor at the ceiling. The planner's document adds one gate, which is
     * within its own bound and one past the merged one. */
    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = (int)ATLAS_ORCH_MAX_VALIDATIONS, .parallel = 2};
    plan_create(&e, &r, &plan);

    art a = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_GATE, .stored = true};
    planner p = {.k = 1, .succeed = true, .artifact = &a};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &job);

    atlas_plan_result res;
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                     ATLAS_PLAN_REVISION_INITIAL, true, "a merged gate list past the bound",
                     "would run 9 gates", &res);
    /* A refusal about the document as a whole, not about a line: the floor is
     * not in the document, so no line of it is at fault. */
    T_EQ_INT(res.refusal_line, 0);
    /* The sentence does the arithmetic, because "too many gates" sends an
     * operator to count them by hand. */
    const char *s = atlas_buf_cstr(&res.refusal);
    T_CHECK_MSG(strstr(s, "floor of 8") != NULL, "the refusal does not name the floor: %s", s);
    T_CHECK_MSG(strstr(s, "plus 1") != NULL, "the refusal does not name the additions: %s", s);
    T_CHECK_MSG(strstr(s, "at most 8") != NULL, "the refusal does not name the bound: %s", s);
    atlas_plan_result_free(&res);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"), 0);
    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_tasks;"), 0);

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_plan_takes_no_more_revisions_than_its_bound(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);
    int64_t plan_id = count_sql(e.db, "SELECT id FROM orch_plans;");

    /* The three revisions this plan may hold, written straight in: what is under
     * test is the bound, not the ingest, and three real planner jobs would only
     * make the case slower. */
    for (int i = 1; i <= ATLAS_PLAN_MAX_REVISIONS; i++) {
        char sql[512];
        atlas_err err;
        atlas_err_init(&err);
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason,"
                       "  content, content_sha256, created_at)"
                       "  VALUES(%lld, %d, 'j%d', 'REPLAN', x'00', 'h', 't');",
                       (long long)plan_id, i, i);
        T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
    }

    art a = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE, .stored = true};
    planner p = {.k = 1, .succeed = true, .artifact = &a};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &job);

    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job),
                     ATLAS_PLAN_MAX_REVISIONS + 1, ATLAS_PLAN_REVISION_REPLAN, false,
                     "a fourth revision", "takes no further one", NULL);
    /* And a number that is neither the next one nor past the bound. */
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 2,
                     ATLAS_PLAN_REVISION_REPLAN, false, "a revision number already used",
                     "the next one is 4", NULL);

    T_EQ_INT((int)count_sql(e.db, "SELECT count(*) FROM orch_plan_revisions;"),
             ATLAS_PLAN_MAX_REVISIONS);

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

/* --- 5: the derived status ---------------------------------------------------
 *
 * Every case builds the rows and asks the one reader. Nothing writes a status,
 * because there is nowhere to write one. */

static atlas_plan_status derive(env *e, const char *plan_uid, atlas_plan_state *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_plan_state_derive(e->db, plan_uid, out, &err), &err);
    return out->status;
}

/* Submits the job one task of the latest revision becomes, with the correlation
 * that binds it. A tree task is the run's root and a side task is its sibling. */
static void task_job(env *e, const char *plan_uid, int rev, const char *key, bool tree,
                     const char *parent, atlas_buf *run_out, atlas_buf *job_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf corr = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_task(plan_uid, rev, key, &corr, &err), &err);
    sub s = {.driver = tree ? "fake-repo" : "fake",
             .correlation = atlas_buf_cstr(&corr),
             .parent = parent,
             .parallel = tree ? 2 : 0};
    submit(e, &s, run_out, job_out);
    atlas_buf_free(&corr);
}

static void force_run(env *e, const char *run_uid, atlas_orch_run_status want) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_orch_run_set_status(e->db, run_uid, ATLAS_ORCH_RUN_ACTIVE, want, &err), &err);
}

/* A plan with one compiled revision, ready for its tasks to be submitted. */
static void plan_with_revision(env *e, atlas_buf *plan_out, atlas_buf *planner_out) {
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(e, &r, plan_out);
    art a = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE, .stored = true};
    planner p = {.k = 1, .succeed = true, .artifact = &a};
    planner_job(e, atlas_buf_cstr(plan_out), &p, planner_out);
    atlas_plan_result res;
    revision_add(e, atlas_buf_cstr(plan_out), atlas_buf_cstr(planner_out), 1,
                 ATLAS_PLAN_REVISION_INITIAL, &res);
    atlas_plan_result_free(&res);
}

static void test_a_plan_with_no_planner_job_is_planning(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    atlas_plan_state st;
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_PLANNING);
    T_EQ_INT(st.rev_no, 0);
    T_EQ_INT(st.planner_jobs_seen, 0);
    T_EQ_INT(st.task_count, 0);
    T_CHECK(!st.replan_wanted);
    T_CHECK(st.planner_job_uid[0] == '\0');

    /* A plan nobody created is a refusal, never an UNKNOWN status: UNKNOWN is
     * the vocabulary's zero and means nobody derived this. */
    {
        atlas_err err;
        atlas_err_init(&err);
        atlas_plan_state gone;
        T_FAILS_WITH(atlas_db_plan_state_derive(e.db, "pnope", &gone, &err), ATLAS_ERR_USAGE, &err);
    }

    /* A planner job in flight is still PLANNING, and the plan now names it. */
    planner p = {.k = 1, .stop_before_finish = true};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_PLANNING);
    T_EQ_INT(st.planner_jobs_seen, 1);
    T_EQ_STR(st.planner_job_uid, atlas_buf_cstr(&job));
    T_CHECK(!st.replan_wanted);

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_planner_job_no_revision_names_wants_the_driver(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    /* The refused-artifact half of `replan_wanted`, which has no row of its own:
     * a refused parse aborts its transaction, so the state is derived as "the
     * newest planner job SUCCEEDED and no revision names it". The rows cannot
     * tell that from a driver that died between the completion and the ingest,
     * and do not have to. */
    art bad = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_REFUSED, .stored = true};
    planner p = {.k = 1, .succeed = true, .artifact = &bad};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &job);
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                     ATLAS_PLAN_REVISION_INITIAL, true, "an unparseable document", NULL, NULL);

    atlas_plan_state st;
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_PLANNING);
    T_CHECK_MSG(st.replan_wanted, "a refused artifact with budget left wants the driver");
    T_EQ_INT(st.rev_no, 0);

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_spent_planner_budget_with_nothing_compiled_is_blocked(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    /* Every planner job this plan may have, each one failing. After the last
     * there is no path forward: no revision compiled and no budget to try
     * again. */
    atlas_plan_state st;
    for (int k = 1; k <= ATLAS_PLAN_MAX_PLANNER_JOBS; k++) {
        planner p = {.k = k, .succeed = false};
        planner_job(&e, atlas_buf_cstr(&plan), &p, NULL);
        atlas_plan_status s = derive(&e, atlas_buf_cstr(&plan), &st);
        T_EQ_INT(st.planner_jobs_seen, k);
        if (k < ATLAS_PLAN_MAX_PLANNER_JOBS) {
            T_CHECK_MSG(s == ATLAS_PLAN_STATUS_PLANNING,
                        "planner job %d of %d left the plan %s", k, ATLAS_PLAN_MAX_PLANNER_JOBS,
                        atlas_plan_status_name(s));
        } else {
            T_CHECK_MSG(s == ATLAS_PLAN_STATUS_BLOCKED,
                        "the last planner job left the plan %s", atlas_plan_status_name(s));
        }
    }
    T_CHECK(!st.replan_wanted);

    atlas_buf_free(&plan);
    env_close(&e);
}

/* The companion to the case above, and the one that pins what the *last* planner
 * job's document leaves behind.
 *
 * **A format-refused fifth document leaves the plan PLANNING durably, and that is
 * a stated cost rather than a defect.** A refusal writes no row — the transaction
 * that would have written the revision is rolled back — so a refused document is
 * only ever visible as a planner job no revision names. At k < 5 the *next*
 * planner job is the durable evidence that the previous one was refused: the
 * count moves, and the plan progresses. At k = 5 there is no next job, so nothing
 * durable records that the last document was read and rejected, and the plan
 * keeps answering PLANNING with an unconsumed artifact behind it. The driver
 * re-runs the ingest on every resume and re-prints the same deterministic refusal
 * from the same stored bytes.
 *
 * The alternative — a refusal row — would give a model's rejected output a
 * durable record of its own, and the season's documents record this as a stated
 * cost instead. What the plan must *not* do here is answer BLOCKED, which is the
 * case below: the derive cannot tell a refused fifth document from a valid one
 * nobody has offered yet, and closing the door on the second would strand a
 * paid-for plan. */
static void test_the_last_planner_jobs_refused_document_leaves_the_plan_planning(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    /* The first four fail, which spends budget without producing anything. */
    for (int k = 1; k < ATLAS_PLAN_MAX_PLANNER_JOBS; k++) {
        planner p = {.k = k, .succeed = false};
        planner_job(&e, atlas_buf_cstr(&plan), &p, NULL);
    }
    atlas_plan_state st;
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_PLANNING);
    T_EQ_INT(st.planner_jobs_seen, ATLAS_PLAN_MAX_PLANNER_JOBS - 1);

    /* The last one succeeds and writes a document Atlas refuses. */
    art bad = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_REFUSED, .stored = true};
    planner last = {.k = ATLAS_PLAN_MAX_PLANNER_JOBS, .succeed = true, .artifact = &bad};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &last, &job);
    revision_refused(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                     ATLAS_PLAN_REVISION_INITIAL, true, "the last planner job's document", NULL,
                     NULL);

    T_CHECK_MSG(derive(&e, atlas_buf_cstr(&plan), &st) == ATLAS_PLAN_STATUS_PLANNING,
                "the last planner job's refused document derived %s",
                atlas_plan_status_name(st.status));
    T_EQ_INT(st.planner_jobs_seen, ATLAS_PLAN_MAX_PLANNER_JOBS);
    T_EQ_INT((int)st.planner_job_state, (int)ATLAS_ORCH_STATE_SUCCEEDED);
    T_EQ_STR(st.planner_job_uid, atlas_buf_cstr(&job));
    T_EQ_INT(st.rev_no, 0);
    /* The ingest gate. The driver reads this to decide whether there is a stored
     * document to offer, and the answer has to be yes here for the same reason
     * the status is PLANNING: the rows cannot tell this artifact from a valid one
     * nobody has offered yet. */
    T_CHECK_MSG(st.replan_wanted, "the unconsumed artifact is not offered to the driver");

    /* Durable: the same answer on every read, because nothing recorded the
     * refusal and nothing was going to. */
    T_CHECK_MSG(derive(&e, atlas_buf_cstr(&plan), &st) == ATLAS_PLAN_STATUS_PLANNING,
                "the second read derived %s", atlas_plan_status_name(st.status));

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

/* The dead end T6's review found, driven end to end.
 *
 * The fifth planner job is the last one this plan may ever start, and it wrote a
 * **valid** plan. Its bytes are already on its artifact row and were already paid
 * for. Compiling them costs no planner start — the ingest is a pure function of
 * stored bytes — so the spent start budget has nothing to say about whether they
 * may be read.
 *
 * Until this case existed the derive answered BLOCKED here, the driver's ingest
 * gate requires PLANNING, and the plan therefore stranded holding the very
 * document that would have moved it. */
static void test_the_last_planner_jobs_valid_document_is_still_ingestible(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT;
    plan_req r = {.gates = 1, .parallel = 2};
    plan_create(&e, &r, &plan);

    /* Four starts spent on nothing. */
    for (int k = 1; k < ATLAS_PLAN_MAX_PLANNER_JOBS; k++) {
        planner p = {.k = k, .succeed = false};
        planner_job(&e, atlas_buf_cstr(&plan), &p, NULL);
    }

    /* The fifth succeeds and writes a plan Atlas accepts. */
    art good = {.name = ATLAS_PLAN_ARTIFACT_NAME, .bytes = PLAN_ONE_STAGE, .stored = true};
    planner last = {.k = ATLAS_PLAN_MAX_PLANNER_JOBS, .succeed = true, .artifact = &good};
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &last, &job);

    /* PLANNING, not BLOCKED: there is a document here to read. */
    atlas_plan_state st;
    T_CHECK_MSG(derive(&e, atlas_buf_cstr(&plan), &st) == ATLAS_PLAN_STATUS_PLANNING,
                "a valid document from the last planner job derived %s",
                atlas_plan_status_name(st.status));
    T_EQ_INT(st.planner_jobs_seen, ATLAS_PLAN_MAX_PLANNER_JOBS);
    T_EQ_INT(st.rev_no, 0);
    T_CHECK_MSG(st.replan_wanted, "the last planner job's document is not offered to the driver");

    /* And it compiles. The start budget is spent and the revision budget is not,
     * which are different bounds with different subjects. */
    atlas_plan_result res;
    revision_add(&e, atlas_buf_cstr(&plan), atlas_buf_cstr(&job), 1,
                 ATLAS_PLAN_REVISION_INITIAL, &res);
    T_EQ_INT(res.rev_no, 1);
    T_EQ_INT(res.task_count, 2);
    atlas_plan_result_free(&res);

    /* The door is open: the plan now holds the revision it paid for and has work
     * to do. */
    T_CHECK_MSG(derive(&e, atlas_buf_cstr(&plan), &st) == ATLAS_PLAN_STATUS_EXECUTING,
                "the compiled revision left the plan %s", atlas_plan_status_name(st.status));
    T_EQ_INT(st.rev_no, 1);
    T_EQ_INT(st.task_count, 2);
    T_EQ_STR(st.tasks[0].task_key, "build");
    T_CHECK_MSG(!st.replan_wanted, "an ingested document is still being offered");

    atlas_buf_free(&job);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_compiled_revision_executes_completes_and_says_which(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT, pjob = ATLAS_BUF_INIT;
    plan_with_revision(&e, &plan, &pjob);

    /* Nothing submitted yet: the revision names work that has not started. */
    atlas_plan_state st;
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_EXECUTING);
    T_EQ_INT(st.rev_no, 1);
    T_EQ_INT(st.task_count, 2);
    /* Ordered by (stage_no, id), which is the order the document listed them
     * in: the replan composer renders completed work in array order. */
    T_EQ_STR(st.tasks[0].task_key, "build");
    T_CHECK(st.tasks[0].is_tree);
    T_EQ_STR(st.tasks[0].title, "Build the thing");
    T_EQ_STR(st.tasks[1].task_key, "notes");
    T_CHECK(!st.tasks[1].is_tree);
    T_CHECK(st.tasks[0].job_uid[0] == '\0');

    atlas_buf run = ATLAS_BUF_INIT, tree = ATLAS_BUF_INIT, side = ATLAS_BUF_INIT;
    task_job(&e, atlas_buf_cstr(&plan), 1, "build", true, NULL, &run, &tree);
    task_job(&e, atlas_buf_cstr(&plan), 1, "notes", false, atlas_buf_cstr(&tree), NULL, &side);

    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_EXECUTING);
    T_EQ_STR(st.tasks[0].job_uid, atlas_buf_cstr(&tree));
    T_EQ_STR(st.tasks[0].run_uid, atlas_buf_cstr(&run));
    T_EQ_INT((int)st.tasks[0].run_status, (int)ATLAS_ORCH_RUN_ACTIVE);
    T_EQ_STR(st.tasks[1].job_uid, atlas_buf_cstr(&side));
    /* A side task's outcome is its job's; the run belongs to the tree task's
     * row, so the struct's contract stays exact. */
    T_CHECK(st.tasks[1].run_uid[0] == '\0');
    T_EQ_INT(st.stages_accepted, 0);

    /* The sibling succeeds. The stage is not finished until Atlas settles the
     * run, so the plan is still executing. */
    {
        atlas_buf tok = ATLAS_BUF_INIT;
        (void)lease(&e, atlas_buf_cstr(&side), "fake", &tok);
        advance_to_running(&e, atlas_buf_cstr(&tok));
        finish(&e, atlas_buf_cstr(&tok), true, NULL);
        atlas_buf_free(&tok);
    }
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_EXECUTING);

    /* Atlas settles the stage's run. Every tree task ACCEPTED and every side
     * job SUCCEEDED is the whole of COMPLETED. */
    force_run(&e, atlas_buf_cstr(&run), ATLAS_ORCH_RUN_ACCEPTED);
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_COMPLETED);
    T_EQ_INT(st.stages_accepted, 1);
    T_CHECK(!st.replan_wanted);

    atlas_buf_free(&side);
    atlas_buf_free(&tree);
    atlas_buf_free(&run);
    atlas_buf_free(&pjob);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_blocked_stage_run_asks_for_a_replan_and_waits_for_quiescence(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT, pjob = ATLAS_BUF_INIT;
    plan_with_revision(&e, &plan, &pjob);

    atlas_buf run = ATLAS_BUF_INIT, tree = ATLAS_BUF_INIT, side = ATLAS_BUF_INIT;
    task_job(&e, atlas_buf_cstr(&plan), 1, "build", true, NULL, &run, &tree);
    task_job(&e, atlas_buf_cstr(&plan), 1, "notes", false, atlas_buf_cstr(&tree), NULL, &side);

    /* A11.6's rule one layer up: one task's failure must not break another
     * task's execution. The stage-run is BLOCKED and the sibling is still
     * going, so the plan is EXECUTING and doomed rather than asking for
     * anything yet. */
    force_run(&e, atlas_buf_cstr(&run), ATLAS_ORCH_RUN_BLOCKED);
    atlas_plan_state st;
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_EXECUTING);
    T_CHECK_MSG(!st.replan_wanted, "a replan was asked for before quiescence");

    /* The sibling ends. Now the revision's submitted work is terminal and one
     * of it is bad, and both budgets remain. */
    {
        atlas_buf tok = ATLAS_BUF_INIT;
        (void)lease(&e, atlas_buf_cstr(&side), "fake", &tok);
        advance_to_running(&e, atlas_buf_cstr(&tok));
        finish(&e, atlas_buf_cstr(&tok), true, NULL);
        atlas_buf_free(&tok);
    }
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_NEEDS_REPLAN);
    T_CHECK(st.replan_wanted);
    T_EQ_INT(st.rev_no, 1);

    /* Once the replan's planner job is in flight the plan is PLANNING again:
     * the driver has already acted, so the cue is spent. */
    planner p = {.k = 2, .stop_before_finish = true};
    atlas_buf second = ATLAS_BUF_INIT;
    planner_job(&e, atlas_buf_cstr(&plan), &p, &second);
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_PLANNING);
    T_CHECK(!st.replan_wanted);

    atlas_buf_free(&second);
    atlas_buf_free(&side);
    atlas_buf_free(&tree);
    atlas_buf_free(&run);
    atlas_buf_free(&pjob);
    atlas_buf_free(&plan);
    env_close(&e);
}

static void test_a_failed_sibling_blocks_a_plan_whose_revisions_are_spent(void) {
    env e;
    env_open(&e);

    atlas_buf plan = ATLAS_BUF_INIT, pjob = ATLAS_BUF_INIT;
    plan_with_revision(&e, &plan, &pjob);

    /* The plan's remaining revisions, spent. What is under test is the budget
     * gate, not the ingest. */
    int64_t plan_id = count_sql(e.db, "SELECT id FROM orch_plans;");
    for (int i = 2; i <= ATLAS_PLAN_MAX_REVISIONS; i++) {
        char sql[512];
        atlas_err err;
        atlas_err_init(&err);
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason,"
                       "  content, content_sha256, created_at)"
                       "  VALUES(%lld, %d, 'jspent%d', 'REPLAN', x'00', 'h', 't');",
                       (long long)plan_id, i, i);
        T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
    }
    /* The tasks of the latest revision are the ones that matter, and revision 3
     * has none: an empty revision reads as work that has not started, which is
     * EXECUTING and not a verdict. */
    atlas_plan_state st;
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_EXECUTING);
    T_EQ_INT(st.rev_no, ATLAS_PLAN_MAX_REVISIONS);
    T_EQ_INT(st.task_count, 0);

    /* Give revision 3 the tasks revision 1 had, and fail the sibling. A gateless
     * workspace sibling can veto acceptance and can never grant it. */
    {
        char sql[1024];
        atlas_err err;
        atlas_err_init(&err);
        int64_t rev3 = count_sql(e.db, "SELECT id FROM orch_plan_revisions ORDER BY rev_no DESC"
                                       "  LIMIT 1;");
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO orch_plan_tasks(revision_id, plan_id, stage_no, task_key, kind,"
                       "  title, prompt, validations)"
                       "  VALUES(%lld, %lld, 1, 'build', 'TREE', 'Build', 'p', '');",
                       (long long)rev3, (long long)plan_id);
        T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO orch_plan_tasks(revision_id, plan_id, stage_no, task_key, kind,"
                       "  title, prompt, validations)"
                       "  VALUES(%lld, %lld, 1, 'notes', 'SIDE', 'Notes', 'p', '');",
                       (long long)rev3, (long long)plan_id);
        T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
    }

    atlas_buf run = ATLAS_BUF_INIT, tree = ATLAS_BUF_INIT, side = ATLAS_BUF_INIT;
    task_job(&e, atlas_buf_cstr(&plan), ATLAS_PLAN_MAX_REVISIONS, "build", true, NULL, &run, &tree);
    task_job(&e, atlas_buf_cstr(&plan), ATLAS_PLAN_MAX_REVISIONS, "notes", false,
             atlas_buf_cstr(&tree), NULL, &side);
    {
        atlas_buf tok = ATLAS_BUF_INIT;
        (void)lease(&e, atlas_buf_cstr(&side), "fake", &tok);
        advance_to_running(&e, atlas_buf_cstr(&tok));
        finish(&e, atlas_buf_cstr(&tok), false, NULL);
        atlas_buf_free(&tok);
    }
    force_run(&e, atlas_buf_cstr(&run), ATLAS_ORCH_RUN_ACCEPTED);

    /* The tree task's run was accepted and the sibling failed. A replan would
     * fix it and the revision budget is spent, so the answer is BLOCKED — and
     * because it is derived rather than stored, it is not a trap. */
    T_EQ_INT((int)derive(&e, atlas_buf_cstr(&plan), &st), (int)ATLAS_PLAN_STATUS_BLOCKED);
    T_CHECK(!st.replan_wanted);
    T_EQ_INT(st.stages_accepted, 1);

    atlas_buf_free(&side);
    atlas_buf_free(&tree);
    atlas_buf_free(&run);
    atlas_buf_free(&pjob);
    atlas_buf_free(&plan);
    env_close(&e);
}

/* --- 6: nothing else writes these tables ------------------------------------- */

static void read_source(const char *path, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(out, "", &err), &err);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return;
    }
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1u, sizeof chunk, fp)) > 0) {
        T_OK(atlas_buf_append(out, chunk, n, &err), &err);
    }
    (void)fclose(fp);
}

typedef struct writer_scan {
    const char *needle;
    size_t files;
    atlas_buf names;
} writer_scan;

static void scan_file(const char *path, writer_scan *sc) {
    atlas_buf text = ATLAS_BUF_INIT;
    read_source(path, &text);
    if (strstr(atlas_buf_cstr(&text), sc->needle) != NULL) {
        atlas_err err;
        atlas_err_init(&err);
        sc->files++;
        T_OK(atlas_buf_appendf(&sc->names, &err, "%s ", path), &err);
    }
    atlas_buf_free(&text);
}

static void scan_dir(const char *dir, writer_scan *sc) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char path[4096];
        (void)snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        struct stat sb;
        if (stat(path, &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            scan_dir(path, sc);
            continue;
        }
        size_t len = strlen(path);
        if (len > 2u && path[len - 2u] == '.' && (path[len - 1u] == 'c' || path[len - 1u] == 'h')) {
            scan_file(path, sc);
        }
    }
    (void)closedir(d);
}

/* `expect` files in `src/` may contain `needle`, and when one may, it is
 * `db_plan.c`. Scanning the tree rather than a fixed list is the point: a fixed
 * list would be satisfied by a new file the list does not name. */
static void plan_writers(const char *needle, size_t expect) {
    writer_scan sc = {needle, 0u, ATLAS_BUF_INIT};
    scan_dir(ATLAS_SRC_DIR "/src", &sc);
    T_CHECK_MSG(sc.files == expect,
                "expected %zu file(s) in src/ to write with \"%s\", found %zu: %s", expect, needle,
                sc.files, atlas_buf_cstr(&sc.names));
    if (expect == 1u) {
        T_CHECK_MSG(strstr(atlas_buf_cstr(&sc.names), "/src/db/db_plan.c") != NULL,
                    "\"%s\" is written outside db_plan.c: %s", needle,
                    atlas_buf_cstr(&sc.names));
    }
    atlas_buf_free(&sc.names);
}

static void test_only_the_write_point_writes_the_plan_tables(void) {
    /* The claim in `atlas/plan.h` is that `atlas_plan_apply_in_tx` is the only
     * function that writes `orch_plan*`, and a claim about a call graph decays
     * the moment somebody adds a writer — nothing about the new call site would
     * look wrong, it would look like reuse. So it is checked rather than
     * asserted in prose.
     *
     * `migrate.c` creates the tables and matches none of these needles, which is
     * why the needles are the verbs and not the table name. */
    plan_writers("INSERT INTO orch_plan", 1u);
    /* **Nothing anywhere updates or deletes one of these rows.** A plan is what
     * an operator brought and a revision is a planner's bytes with the digest of
     * them; neither is edited, and all three tables are CANONICAL in
     * `RETENTION[]`, so nothing prunes them either. The count is zero rather
     * than one, and that is a stronger claim than "only the write point does
     * it". */
    plan_writers("UPDATE orch_plan", 0u);
    plan_writers("DELETE FROM orch_plan", 0u);

    /* And there is no status to write. A plan's status is derived, so the one
     * thing a model payload could aim at does not exist as a column. */
    writer_scan sc = {"plan_status", 0u, ATLAS_BUF_INIT};
    scan_dir(ATLAS_SRC_DIR "/src/db", &sc);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&sc.names), "migrate.c") == NULL,
                "migration 25 defines a plan status column: %s", atlas_buf_cstr(&sc.names));
    atlas_buf_free(&sc.names);
}

static const atlas_test TESTS[] = {
    {"migration 25 adds its tables and the correlation index",
     test_migration_25_adds_its_tables_and_the_correlation_index},
    {"the worst-case correlation fits a specification",
     test_the_worst_case_correlation_fits_a_specification},
    {"the compiled constants and the schema's CHECKs agree",
     test_the_constants_and_the_checks_agree},
    {"a plan stores what the operator brought", test_a_plan_stores_what_the_operator_brought},
    {"a plan without an operator gate is refused",
     test_a_plan_without_an_operator_gate_is_refused},
    {"an over-long goal is refused rather than truncated",
     test_an_over_long_goal_is_refused_rather_than_truncated},
    {"a planner's artifact compiles into tasks with the floor first",
     test_a_planners_artifact_compiles_into_tasks},
    {"only a planner job of this plan can produce a revision",
     test_only_a_planner_job_of_this_plan_can_produce_a_revision},
    {"the artifact must be present, stored and within bounds",
     test_the_artifact_must_be_present_stored_and_within_bounds},
    {"a refused document comes back typed and writes nothing",
     test_a_refused_document_comes_back_typed_and_writes_nothing},
    {"the merged gate list is bounded and the refusal does the arithmetic",
     test_the_merged_gate_list_is_bounded_and_the_refusal_does_the_arithmetic},
    {"a plan takes no more revisions than its bound",
     test_a_plan_takes_no_more_revisions_than_its_bound},
    {"a plan with no planner job is planning", test_a_plan_with_no_planner_job_is_planning},
    {"a planner job no revision names wants the driver",
     test_a_planner_job_no_revision_names_wants_the_driver},
    {"a spent planner budget with nothing compiled is blocked",
     test_a_spent_planner_budget_with_nothing_compiled_is_blocked},
    {"the last planner job's refused document leaves the plan planning",
     test_the_last_planner_jobs_refused_document_leaves_the_plan_planning},
    {"the last planner job's valid document is still ingestible",
     test_the_last_planner_jobs_valid_document_is_still_ingestible},
    {"a compiled revision executes, completes and says which",
     test_a_compiled_revision_executes_completes_and_says_which},
    {"a blocked stage-run asks for a replan and waits for quiescence",
     test_a_blocked_stage_run_asks_for_a_replan_and_waits_for_quiescence},
    {"a failed sibling blocks a plan whose revisions are spent",
     test_a_failed_sibling_blocks_a_plan_whose_revisions_are_spent},
    {"only the write point writes the plan tables",
     test_only_the_write_point_writes_the_plan_tables},
};

ATLAS_TEST_MAIN("plan_db", TESTS)

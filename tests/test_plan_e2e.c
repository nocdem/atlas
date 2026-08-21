/* Atlas - A12.0: the planned run, end to end, over the shipped socket.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What is real here, and it is nearly everything
 *
 * A goal becomes a planner job, the planner's artifact becomes a revision, the
 * revision's stages become ordinary runs, and the runs settle — driven by
 * `atlas_plandriver_run` over **the production transport**, the same
 * `plan_xport` the shipped `atlas plan run` builds. Every request is framed and
 * carried over a real Unix socket to `atlas_server_serve`, answered by the real
 * method table, gated by the real `require_submitter` and `require_dispatcher`,
 * applied by the real writer thread through the real write point, into a real
 * migrated index over a real registered and scanned git repository.
 *
 * Three substitutions, each stated rather than assumed:
 *
 *   1. **The policy's provenance.** `atlas_orchpolicy_load` walks a root-owned
 *      path with no override, so a suite running as an ordinary uid cannot
 *      install one anywhere. The policy here is parsed by the shipped parser
 *      through `atlas_orchpolicy_parse_bytes` and its *content* is therefore
 *      checked exactly as a deployment's is; only where the bytes came from
 *      differs. `tests/test_plan_rpc.c` establishes this substitution and
 *      `tests/test_orch_model.c` is what proves the loader refuses a policy an
 *      ordinary uid could have written.
 *
 *   2. **The serve loop's host.** `atlas_server_serve` runs on a thread of this
 *      process instead of in `atlasd`. It is the same function, on a socket
 *      created by `atlas_ipc_listen` and accepted by `atlas_ipc_accept`, so the
 *      peer uid every method is gated on is still the kernel's `SO_PEERCRED`
 *      answer and never a claim in a request body.
 *
 *   3. **One transport member.** `drive_workspace_job` is substituted, and the
 *      reason is arithmetic in the shipped policy rather than a weakness in the
 *      test: `atlas_service_orch_driver_filter(pol, true, ...)` is the *model*
 *      partition, which holds exactly the drivers with `needs_live_model`, and
 *      none of the fakes has it. In production a planner job runs `claude-plan`
 *      and a sibling runs `claude`, both of which do; a fixture that must call
 *      no model cannot be on that partition at all. The substitute calls the
 *      shipped `atlas_dispatch_run_one` with a filter naming the fakes, so the
 *      lease, the snapshot, the workspace, the driver, the heartbeat and the
 *      completion are all still shipped code over the same socket. It does not
 *      touch the assertion this file exists for: the merged gate list travels
 *      `job_submit` and the daemon's write path, which are not substituted.
 *
 * A repo-tree task is never leased through a filter at all — the run driver
 * claims it by name, which is A11.1's rule — so stages are unaffected either
 * way.
 *
 * ## The three cases
 *
 *   1. A two-stage plan reaches COMPLETED, both stage-runs ACCEPTED, and each
 *      tree job's `orch_jobs.validations` is **byte-identical** to its
 *      `orch_plan_tasks.validations` — asserted with a `%` inside a gate
 *      argument, because `%` is the one byte `atlas-safe-1` rewrites and
 *      ordinary words cannot tell a correct round trip from a missing one.
 *   2. A driver killed between a planner job's success and `plan.revision_add`,
 *      and again between a stage's submission and its drive, is resumed: the
 *      same revision lands exactly once and no correlation names two jobs.
 *   3. A stage-run that settles BLOCKED produces exactly one replan, whose
 *      composed prompt names the blocked task; revision 2 compiles and its
 *      stages complete, with revision 1 still present.
 *
 * ## The fake planner's document
 *
 * `fake-plan` writes the part of its task text between `fake-plan-artifact:` and
 * `fake-plan-artifact-end:`. The marker pair lives in the operator's **goal**,
 * which is the only text a fixture controls that reaches a composed planner
 * prompt — and it reaches it in the middle, which is why the closing marker
 * exists. The consequence is deliberate and is what makes case 3 possible: the
 * goal is frozen when the plan is created, so every revision of one plan is the
 * *same document*, and a stage that failed can only pass on a later revision
 * because the world moved, never because the planner was told a better answer.
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/dispatch.h"
#include "atlas/driver.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas/plan.h"
#include "atlas/plandriver.h"
#include "atlas/service.h"
#include "atlas/workers.h"
#include "atlas_test.h"
#include "core/service_internal.h"
#include "daemon/daemon_internal.h"
#include "db/db_internal.h"
#include "orch/policy_internal.h"
#include "support/fixture.h"

/* --- the fixture repository's gates ------------------------------------------
 *
 * `pass` is a constant. `four` counts the lines `fake-repo` has appended to the
 * work tree, one per worker start, so a gate can distinguish "three workers have
 * run in this repository" from "a fourth has". That is what lets case 3 show a
 * run spending its whole budget and blocking, and the *next* revision's run —
 * the same document, one start later — passing the same gate.
 *
 * Recipes are tab-indented because make requires it. Nothing here is a shell
 * Atlas creates: `make` is on the validation allowlist and runs its own recipes,
 * which is true of every gate an operator can declare. */
static const char GATE_MAKEFILE[] =
    "pass:\n"
    "\t@true\n"
    "four:\n"
    "\t@test -f ATLAS_FAKE_DRIVER.txt && test $$(wc -l < ATLAS_FAKE_DRIVER.txt) -ge 4\n";

/* --- the environment ---------------------------------------------------------- */

typedef struct e2e {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db; /* this test's own handle, for the SQL assertions only */
    atlas_buf runtime_dir;
    atlas_buf sock;
    atlas_buf worker_root;       /* the ordinary dispatcher's, unused and required */
    atlas_buf model_worker_root; /* where a workspace attempt runs here */
    atlas_workers *workers;
    atlas_writer *writer;
    atlas_server_ctx ctx;
    atlas_orchpolicy pol; /* the client side's copy, as a deployment has two */
    int listen_fd;
    atomic_bool stop;
    pthread_t thread;
    bool serving;
} e2e;

/* The environment the one substituted transport member works in.
 *
 * `atlas_plandriver_transport` carries a single `ctx`, and every other member of
 * it is the production one and needs the production `plan_xport` there. Rather
 * than wrap all nine to smuggle a second pointer — which would substitute nine
 * members to substitute one — the substitute reaches its environment through
 * this. One test runs at a time and the driver runs on this thread. */
static e2e *g_here;

static void mkdir_or_die(const char *path) {
    T_REQUIRE(mkdir(path, S_IRWXU) == 0 || errno == EEXIST);
}

/* The policy this deployment would have.
 *
 * Parsed by the shipped parser, which is what checks it: the disjointness of the
 * dispatcher and the submitters, the model dispatcher's separate worker root,
 * the vocabulary of every driver and mode, and every ceiling. Only the state is
 * set here, because the one `ENABLED` assignment in Atlas is the loader's last
 * statement after a root-owned path walk this process cannot satisfy. */
static void e2e_policy(e2e *g, atlas_orchpolicy *out) {
    char text[1024];
    /* The ordinary dispatcher is a uid nothing in this file ever connects as; it
     * exists because a policy without one is not a policy, and because the model
     * dispatcher may not be the same uid. */
    (void)snprintf(text, sizeof text,
                   "dispatcher_uid = %lld\n"
                   "submitter_uid = %lld\n"
                   "model_dispatcher_uid = %lld\n"
                   "repo = proj\n"
                   "mode = patch\n"
                   "driver = fake\n"
                   "driver = fake-plan\n"
                   "driver = fake-repo\n"
                   "worker_root = %s\n"
                   "model_worker_root = %s\n"
                   "model_credential = operator_session\n"
                   "live_model = off\n"
                   "max_attempts = 3\n",
                   (long long)getuid() + 1, (long long)getuid(), (long long)getuid(),
                   atlas_buf_cstr(&g->worker_root), atlas_buf_cstr(&g->model_worker_root));
    memset(out, 0, sizeof(*out));
    atlas_orchpolicy_reason why = atlas_orchpolicy_parse_bytes(text, strlen(text), out);
    T_REQUIRE(why == ATLAS_ORCHPOLICY_REASON_ACTIVE);
    out->reason = why;
    out->state = ATLAS_ORCHPOLICY_ENABLED;
}

static void *serve_thread(void *ud) {
    e2e *g = (e2e *)ud;
    atlas_err err;
    atlas_err_init(&err);
    /* No signal fd: `poll` ignores a negative descriptor, and this loop is
     * stopped by the flag rather than by a signal. */
    (void)atlas_server_serve(&g->ctx, g->listen_fd, -1, &g->stop, &err);
    return NULL;
}

static void e2e_open(e2e *g) {
    atlas_err err;
    atlas_err_init(&err);
    memset(g, 0, sizeof(*g));
    g->listen_fd = -1;
    T_OK(fx_open(&g->fx, &err), &err);
    atlas_buf_init(&g->db_path);
    atlas_buf_init(&g->runtime_dir);
    atlas_buf_init(&g->sock);
    atlas_buf_init(&g->worker_root);
    atlas_buf_init(&g->model_worker_root);

    T_OK(fx_init_repo(&g->fx, fx_repo(&g->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&g->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&g->fx), "Makefile", GATE_MAKEFILE, &err), &err);
    T_OK(fx_add_all(&g->fx, fx_repo(&g->fx), &err), &err);
    T_OK(fx_commit(&g->fx, fx_repo(&g->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&g->fx), "repo", "add",
                             fx_repo(&g->fx),  "--name",         "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    {
        const char *scan[] = {"--data-dir", fx_data_dir(&g->fx), "scan", "proj"};
        int code = -1;
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    /* The endpoint. `XDG_RUNTIME_DIR` is exported before any thread exists, and
     * the socket *scope* is named explicitly so that a machine carrying a
     * root-owned system policy resolves this process to the fixture's own
     * per-user socket rather than to the deployment's. */
    T_OK(atlas_buf_appendf(&g->runtime_dir, &err, "%s/run", atlas_buf_cstr(&g->fx.root)), &err);
    mkdir_or_die(atlas_buf_cstr(&g->runtime_dir));
    T_REQUIRE(setenv("XDG_RUNTIME_DIR", atlas_buf_cstr(&g->runtime_dir), 1) == 0);
    atlas_ipc_socket_scope_set(fx_data_dir(&g->fx));
    {
        atlas_buf dir = ATLAS_BUF_INIT;
        T_OK(atlas_ipc_runtime_dir(&dir, &err), &err);
        T_OK(atlas_ipc_ensure_runtime_dir(atlas_buf_cstr(&dir), NULL, &err), &err);
        atlas_buf_free(&dir);
    }
    T_OK(atlas_ipc_socket_path(&g->sock, &err), &err);

    T_OK(atlas_buf_appendf(&g->worker_root, &err, "%s/worker", atlas_buf_cstr(&g->fx.root)), &err);
    T_OK(atlas_buf_appendf(&g->model_worker_root, &err, "%s/model", atlas_buf_cstr(&g->fx.root)),
         &err);
    mkdir_or_die(atlas_buf_cstr(&g->worker_root));
    mkdir_or_die(atlas_buf_cstr(&g->model_worker_root));

    T_OK(atlas_buf_appendf(&g->db_path, &err, "%s/atlas.db", fx_data_dir(&g->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&g->db_path), &g->db, &err), &err);
    T_OK(atlas_db_migrate(g->db, &err), &err);

    T_OK(atlas_workers_start(2u, &g->workers, &err), &err);
    T_OK(atlas_writer_start(atlas_buf_cstr(&g->db_path), atlas_buf_cstr(&g->sock), g->workers,
                            NULL, &g->writer, &err),
         &err);

    g->ctx.db_path = atlas_buf_cstr(&g->db_path);
    g->ctx.data_dir = fx_data_dir(&g->fx);
    g->ctx.socket_path = atlas_buf_cstr(&g->sock);
    g->ctx.writer = g->writer;
    e2e_policy(g, &g->ctx.orchpolicy);
    e2e_policy(g, &g->pol);

    T_OK(atlas_ipc_listen(atlas_buf_cstr(&g->sock), NULL, &g->listen_fd, &err), &err);
    atomic_init(&g->stop, false);
    T_REQUIRE(pthread_create(&g->thread, NULL, serve_thread, g) == 0);
    g->serving = true;
    g_here = g;
}

static void e2e_close(e2e *g) {
    if (g->serving) {
        atomic_store(&g->stop, true);
        (void)pthread_join(g->thread, NULL);
        g->serving = false;
    }
    if (g->listen_fd >= 0) {
        (void)close(g->listen_fd);
        g->listen_fd = -1;
    }
    atlas_writer_stop(g->writer);
    g->writer = NULL;
    atlas_workers_stop(g->workers);
    g->workers = NULL;
    atlas_db_close(g->db);
    g->db = NULL;
    g_here = NULL;
    atlas_buf_free(&g->db_path);
    atlas_buf_free(&g->runtime_dir);
    atlas_buf_free(&g->sock);
    atlas_buf_free(&g->worker_root);
    atlas_buf_free(&g->model_worker_root);
    fx_close(&g->fx);
}

/* --- reading rows back -------------------------------------------------------
 *
 * The plan's own surfaces are asserted through `plan.get`, which is what a
 * client sees. These go underneath it, to the columns, because "the job the
 * daemon stored carries the same bytes as the task row it was built from" is a
 * claim about storage and cannot be made from a rendering of either. */

static sqlite3_stmt *q(e2e *g, const char *sql, const char *p1, const char *p2) {
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(g->db->h, sql, -1, &st, NULL) == SQLITE_OK);
    if (p1 != NULL) {
        T_REQUIRE(sqlite3_bind_text(st, 1, p1, -1, SQLITE_STATIC) == SQLITE_OK);
    }
    if (p2 != NULL) {
        T_REQUIRE(sqlite3_bind_text(st, 2, p2, -1, SQLITE_STATIC) == SQLITE_OK);
    }
    return st;
}

static int64_t one_int(e2e *g, const char *sql, const char *p1, const char *p2) {
    sqlite3_stmt *st = q(g, sql, p1, p2);
    int64_t v = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

/* The first column of the first row as raw bytes, or an empty buffer when there
 * is no row. Bytes rather than a C string: a netstring-encoded gate list is
 * compared for byte equality, and a comparison that stopped at a NUL would call
 * two different lists equal. */
static void one_blob(e2e *g, const char *sql, const char *p1, const char *p2, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *st = q(g, sql, p1, p2);
    atlas_buf_reset(out);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *b = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (b != NULL && n > 0) {
            T_OK(atlas_buf_set(out, b, (size_t)n, &err), &err);
        }
    }
    sqlite3_finalize(st);
}

static const char SQL_JOB_VALIDATIONS[] =
    "SELECT validations FROM orch_jobs WHERE correlation = ?";
static const char SQL_TASK_VALIDATIONS[] =
    "SELECT t.validations FROM orch_plan_tasks t"
    " JOIN orch_plan_revisions r ON r.id = t.revision_id"
    " JOIN orch_plans p ON p.id = r.plan_id"
    " WHERE p.plan_uid = ? AND t.task_key = ?";
static const char SQL_JOBS_WITH_CORRELATION[] =
    "SELECT COUNT(*) FROM orch_jobs WHERE correlation = ?";
static const char SQL_REVISIONS[] =
    "SELECT COUNT(*) FROM orch_plan_revisions r JOIN orch_plans p ON p.id = r.plan_id"
    " WHERE p.plan_uid = ?";
static const char SQL_TASK_TEXT[] = "SELECT task_text FROM orch_jobs WHERE correlation = ?";

/* The correlation a plan's job carries, from the one builder pair. Spelling one
 * by hand here would be a second answer to "is this job this plan's", which is
 * exactly what `atlas_plan_correlation_*` exists to prevent. */
static void corr_planner(const char *plan_uid, int k, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_plan_correlation_planner(plan_uid, k, out, &err), &err);
}

static void corr_task(const char *plan_uid, int rev_no, const char *key, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_plan_correlation_task(plan_uid, rev_no, key, out, &err), &err);
}

/* --- the transport ----------------------------------------------------------- */

/* The substituted member. See this file's header for why it is substituted and
 * for what it deliberately does not weaken. Everything inside it is shipped
 * code: `atlas_dispatch_run_one` makes the same targeted lease, provisions the
 * same workspace, runs the same driver, heartbeats the same way and reports the
 * same completion, over the same socket. */
static atlas_status ws_here(void *ctx, const char *job_uid, atlas_err *err) {
    (void)ctx;
    e2e *g = g_here;
    T_REQUIRE(g != NULL);
    atlas_dispatch_opts o;
    memset(&o, 0, sizeof(o));
    o.socket_path = atlas_buf_cstr(&g->sock);
    o.worker_root = atlas_buf_cstr(&g->model_worker_root);
    o.dispatcher_id = "atlas-plan-e2e";
    o.drivers = "fake,fake-plan";
    o.operator_session = false;
    o.live_model = false;
    o.heartbeat_ms = ATLAS_ORCH_LEASE_MS / 4;
    bool ran = false;
    return atlas_dispatch_run_one(&o, job_uid, &ran, err);
}

/* A driver that died where a crash is most expensive: after a planner job
 * SUCCEEDED and its artifact was stored, before anything turned it into a
 * revision. Deliberately *not* marked as a transport failure, so the driver
 * gives up at once rather than spending its retry budget on a fixture. */
static atlas_status dead_revision_add(void *ctx, const char *plan_uid, const char *planner_job,
                                      int rev_no, const char *reason, atlas_plan_refusal *ref,
                                      atlas_err *err) {
    (void)ctx;
    (void)plan_uid;
    (void)planner_job;
    (void)rev_no;
    (void)reason;
    (void)ref;
    return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the plan driver was killed here");
}

/* The same, one step later: the stage's tasks are submitted and the run exists,
 * and nothing has driven it. */
static atlas_status dead_drive_run(void *ctx, const char *run_uid, atlas_err *err) {
    (void)ctx;
    (void)run_uid;
    return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the plan driver was killed here");
}

typedef struct drive_opts {
    const char *plan_uid; /* resume, or NULL/empty to create */
    const char *goal;
    const char *const *gates;
    size_t gate_count;
    /* Substituted after wiring, to stand in for a driver that died at a point. */
    bool kill_at_revision_add;
    bool kill_at_drive_run;
} drive_opts;

/* One `atlas plan run`, minus the policy load and the rendering. Everything from
 * the transport down is what the shipped command runs. */
static atlas_status drive(e2e *g, const drive_opts *d, atlas_plandriver_report *rep,
                          atlas_err *err) {
    plan_xport *x = NULL;
    atlas_status st = atlas_service_plan_xport_new(&g->pol, NULL, &x, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_plandriver_opts po;
    memset(&po, 0, sizeof(po));
    atlas_service_plan_xport_wire(x, &po.transport);
    po.transport.drive_workspace_job = ws_here;
    if (d->kill_at_revision_add) {
        po.transport.plan_revision_add = dead_revision_add;
    }
    if (d->kill_at_drive_run) {
        po.transport.drive_run = dead_drive_run;
    }
    po.plan_uid = d->plan_uid;
    po.repo = (d->plan_uid != NULL && d->plan_uid[0] != '\0') ? NULL : "proj";
    po.goal = d->goal;
    po.gate_floor = d->gates;
    po.gate_count = d->gate_count;
    /* The production driver names' fakes. The policy still authorises every one
     * of them: naming a driver narrows, it never permits. */
    po.planner_driver = "fake-plan";
    po.tree_driver = "fake-repo";
    po.side_driver = "fake";
    po.mode = NULL;
    st = atlas_plandriver_run(&po, rep, err);
    T_CHECK_MSG(!atlas_service_plan_xport_saw_busy(x), "the daemon answered BUSY to something");
    atlas_service_plan_xport_free(x);
    return st;
}

/* The plan as a client reads it: one `plan.get` with the per-task detail, parsed
 * by the production readers. */
static void read_state(const char *plan_uid, atlas_plan_state *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_response *r = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    T_OK(atlas_plan_wire_get(NULL, plan_uid, 0, true, &r, &raw, &err), &err);
    T_OK(atlas_plan_read_state(r, out, &err), &err);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
}

static const atlas_plan_task_view *task_of(const atlas_plan_state *s, const char *key) {
    for (int i = 0; i < s->task_count; i++) {
        if (strcmp(s->tasks[i].task_key, key) == 0) {
            return &s->tasks[i];
        }
    }
    return NULL;
}

/* --- case 1: a plan completes, and the gate list survives the whole path ------ */

/* The floor carries a `%`, which is the byte `atlas-safe-1` rewrites. A gate
 * argument is not a make target, so the character never reaches make's pattern
 * rules: `true` takes an argument and ignores it, which is what makes this a
 * pure carriage test. */
static const char *const FLOOR_PCT[] = {"true covered-50%"};

static const char GOAL_TWO_STAGE[] =
    "carry out the two stages below.\n"
    "fake-plan-artifact:\n"
    "atlas-plan-1\n"
    "stage: 1\n"
    "task: one-tree\n"
    "kind: tree\n"
    "title: edit the tree in stage one\n"
    "gate: true added-25%\n"
    "prompt<<\n"
    "stage one, in the repository's own tree.\n"
    ">>\n"
    "task: one-side\n"
    "kind: side\n"
    "title: a workspace sibling for stage one\n"
    "prompt<<\n"
    "stage one, in a workspace.\n"
    ">>\n"
    "stage: 2\n"
    "task: two-tree\n"
    "kind: tree\n"
    "title: edit the tree in stage two\n"
    "prompt<<\n"
    "stage two, in the repository's own tree.\n"
    ">>\n"
    "fake-plan-artifact-end:\n";

/* Asserts that the job the daemon stored for one tree task carries exactly the
 * bytes the plan write point merged when the revision compiled. */
static void gates_match(e2e *g, const char *plan_uid, int rev_no, const char *key,
                        const char *needle) {
    atlas_buf corr = ATLAS_BUF_INIT;
    atlas_buf from_job = ATLAS_BUF_INIT;
    atlas_buf from_task = ATLAS_BUF_INIT;
    corr_task(plan_uid, rev_no, key, &corr);
    one_blob(g, SQL_JOB_VALIDATIONS, atlas_buf_cstr(&corr), NULL, &from_job);
    one_blob(g, SQL_TASK_VALIDATIONS, plan_uid, key, &from_task);
    T_CHECK_MSG(from_task.len > 0, "task %s stored no merged gate list", key);
    T_CHECK_MSG(from_job.len == from_task.len &&
                    (from_job.len == 0 ||
                     memcmp(from_job.data, from_task.data, from_job.len) == 0),
                "task %s: the job's gate list is \"%s\" and the task row's is \"%s\"", key,
                atlas_buf_cstr(&from_job), atlas_buf_cstr(&from_task));
    /* The `%` is what makes the comparison mean something: an encode that was
     * never undone, or a decode applied twice, changes exactly this byte and
     * leaves every ordinary word identical. */
    T_CHECK_MSG(memmem(from_job.data, from_job.len, needle, strlen(needle)) != NULL,
                "task %s: \"%s\" is not in the submitted gate list \"%s\"", key, needle,
                atlas_buf_cstr(&from_job));
    atlas_buf_free(&corr);
    atlas_buf_free(&from_job);
    atlas_buf_free(&from_task);
}

static void test_a_two_stage_plan_completes_through_the_production_transport(void) {
    atlas_err err;
    atlas_err_init(&err);
    e2e g;
    e2e_open(&g);

    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    drive_opts d;
    memset(&d, 0, sizeof(d));
    d.goal = GOAL_TWO_STAGE;
    d.gates = FLOOR_PCT;
    d.gate_count = 1u;
    T_OK(drive(&g, &d, &rep, &err), &err);
    T_REQUIRE(rep.plan_uid.len > 0);
    const char *plan_uid = atlas_buf_cstr(&rep.plan_uid);

    T_CHECK_MSG(rep.status == ATLAS_PLAN_STATUS_COMPLETED, "the plan ended %s",
                atlas_plan_status_name(rep.status));
    T_CHECK(rep.rev_no == 1);
    T_CHECK_MSG(rep.stages_accepted == 2, "%d stage-runs were accepted", rep.stages_accepted);
    T_CHECK(rep.planner_jobs == 1);

    /* Read back the way a client reads it, through the production response
     * readers rather than through the report the invocation returned. */
    atlas_plan_state st;
    read_state(plan_uid, &st);
    T_CHECK(st.status == ATLAS_PLAN_STATUS_COMPLETED);
    T_CHECK(st.rev_no == 1);
    T_CHECK_MSG(st.task_count == 3, "the revision holds %d tasks", st.task_count);

    const atlas_plan_task_view *t1 = task_of(&st, "one-tree");
    const atlas_plan_task_view *s1 = task_of(&st, "one-side");
    const atlas_plan_task_view *t2 = task_of(&st, "two-tree");
    T_REQUIRE(t1 != NULL && s1 != NULL && t2 != NULL);
    T_CHECK(t1->is_tree && t2->is_tree && !s1->is_tree);
    T_CHECK(t1->stage_no == 1 && s1->stage_no == 1 && t2->stage_no == 2);
    T_CHECK_MSG(t1->run_status == ATLAS_ORCH_RUN_ACCEPTED, "stage 1 settled %s",
                atlas_orch_run_status_name(t1->run_status));
    T_CHECK_MSG(t2->run_status == ATLAS_ORCH_RUN_ACCEPTED, "stage 2 settled %s",
                atlas_orch_run_status_name(t2->run_status));
    T_CHECK(t1->job_state == ATLAS_ORCH_STATE_SUCCEEDED);
    T_CHECK(s1->job_state == ATLAS_ORCH_STATE_SUCCEEDED);
    T_CHECK(t2->job_state == ATLAS_ORCH_STATE_SUCCEEDED);
    /* The planner's own words came back through the same read, decoded once. */
    T_CHECK_MSG(strcmp(t1->title, "edit the tree in stage one") == 0, "title read \"%s\"",
                t1->title);

    /* One task row, read by key, with the merged list and the prompt on it. This
     * is the read the driver makes before every submission. */
    {
        atlas_ipc_response *r = NULL;
        atlas_buf raw = ATLAS_BUF_INIT;
        atlas_plandriver_task row;
        atlas_plandriver_task_init(&row);
        T_OK(atlas_plan_wire_get(NULL, plan_uid, 0, true, &r, &raw, &err), &err);
        T_OK(atlas_plan_read_task(r, plan_uid, 1, "one-tree", &row, &err), &err);
        T_CHECK(row.is_tree && row.stage_no == 1);
        T_CHECK_MSG(row.prompt.len > 0 && memmem(row.prompt.data, row.prompt.len, "stage one",
                                                 9u) != NULL,
                    "the task's prompt read \"%s\"", atlas_buf_cstr(&row.prompt));
        T_CHECK(row.validations.len > 0);
        atlas_plandriver_task_free(&row);
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
    }

    /* The pin this case exists for. Both tree jobs, through the production
     * transport, byte for byte against the rows the write point merged. */
    gates_match(&g, plan_uid, 1, "one-tree", "covered-50%");
    gates_match(&g, plan_uid, 1, "one-tree", "added-25%");
    gates_match(&g, plan_uid, 1, "two-tree", "covered-50%");
    /* The floor is the floor: stage 2's planner added nothing, so its merged list
     * is exactly one command and holds no addition. */
    {
        atlas_buf from_task = ATLAS_BUF_INIT;
        one_blob(&g, SQL_TASK_VALIDATIONS, plan_uid, "two-tree", &from_task);
        T_CHECK_MSG(memmem(from_task.data, from_task.len, "added-25%", 9u) == NULL,
                    "stage 2 inherited stage 1's addition: \"%s\"", atlas_buf_cstr(&from_task));
        atlas_buf_free(&from_task);
    }

    /* A side task declares no gate and is stored with none. */
    {
        atlas_buf from_task = ATLAS_BUF_INIT;
        one_blob(&g, SQL_TASK_VALIDATIONS, plan_uid, "one-side", &from_task);
        T_CHECK_MSG(from_task.len == 0, "a side task stored a gate list: \"%s\"",
                    atlas_buf_cstr(&from_task));
        atlas_buf_free(&from_task);
    }

    atlas_plandriver_report_free(&rep);
    e2e_close(&g);
}

/* --- case 2: a driver killed twice, resumed, writes nothing twice ------------- */

static const char *const FLOOR_PASS[] = {"make pass"};

static const char GOAL_ONE_STAGE[] =
    "do the one thing below.\n"
    "fake-plan-artifact:\n"
    "atlas-plan-1\n"
    "stage: 1\n"
    "task: only\n"
    "kind: tree\n"
    "title: the only task\n"
    "prompt<<\n"
    "the only task, in the repository's own tree.\n"
    ">>\n"
    "fake-plan-artifact-end:\n";

static void test_a_killed_driver_resumes_without_writing_anything_twice(void) {
    atlas_err err;
    atlas_err_init(&err);
    e2e g;
    e2e_open(&g);
    atlas_buf plan = ATLAS_BUF_INIT;

    /* Killed between the planner job's success and the revision. */
    {
        atlas_plandriver_report rep;
        atlas_plandriver_report_init(&rep);
        drive_opts d;
        memset(&d, 0, sizeof(d));
        d.goal = GOAL_ONE_STAGE;
        d.gates = FLOOR_PASS;
        d.gate_count = 1u;
        d.kill_at_revision_add = true;
        atlas_status st = drive(&g, &d, &rep, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a driver that could not add a revision reported success");
        atlas_err_init(&err);
        T_REQUIRE(rep.plan_uid.len > 0);
        T_OK(atlas_buf_set(&plan, rep.plan_uid.data, rep.plan_uid.len, &err), &err);
        atlas_plandriver_report_free(&rep);
    }
    const char *uid = atlas_buf_cstr(&plan);
    T_CHECK_MSG(one_int(&g, SQL_REVISIONS, uid, NULL) == 0,
                "a revision landed although the driver died before adding one");
    {
        atlas_buf c = ATLAS_BUF_INIT;
        corr_planner(uid, 1, &c);
        T_CHECK_MSG(one_int(&g, SQL_JOBS_WITH_CORRELATION, atlas_buf_cstr(&c), NULL) == 1,
                    "planner job 1 is not the one job its correlation names");
        atlas_buf_free(&c);
    }

    /* Resumed, and killed again once the stage's task is submitted and its run
     * exists. The planner job is not re-run: it already SUCCEEDED, and the
     * revision is compiled from the artifact it already stored. */
    {
        atlas_plandriver_report rep;
        atlas_plandriver_report_init(&rep);
        drive_opts d;
        memset(&d, 0, sizeof(d));
        d.plan_uid = uid;
        d.kill_at_drive_run = true;
        atlas_status st = drive(&g, &d, &rep, &err);
        T_CHECK_MSG(st != ATLAS_OK, "a driver that could not drive a run reported success");
        atlas_err_init(&err);
        atlas_plandriver_report_free(&rep);
    }
    T_CHECK_MSG(one_int(&g, SQL_REVISIONS, uid, NULL) == 1,
                "the resumed driver did not compile exactly one revision");
    {
        atlas_buf c = ATLAS_BUF_INIT;
        corr_task(uid, 1, "only", &c);
        T_CHECK_MSG(one_int(&g, SQL_JOBS_WITH_CORRELATION, atlas_buf_cstr(&c), NULL) == 1,
                    "the stage's task is not the one job its correlation names");
        atlas_buf_free(&c);
    }

    /* Resumed a second time, with nothing substituted. The plan proceeds from
     * exactly where it was: the same revision, the same job. */
    {
        atlas_plandriver_report rep;
        atlas_plandriver_report_init(&rep);
        drive_opts d;
        memset(&d, 0, sizeof(d));
        d.plan_uid = uid;
        T_OK(drive(&g, &d, &rep, &err), &err);
        T_CHECK_MSG(rep.status == ATLAS_PLAN_STATUS_COMPLETED, "the resumed plan ended %s",
                    atlas_plan_status_name(rep.status));
        T_CHECK(rep.rev_no == 1);
        T_CHECK(rep.planner_jobs == 1);
        atlas_plandriver_report_free(&rep);
    }

    /* Three invocations, one revision, one job per correlation. The idempotency
     * key and the correlation are the same string from the same builder, which
     * is what makes a resumed submission a lookup rather than a second job. */
    T_CHECK_MSG(one_int(&g, SQL_REVISIONS, uid, NULL) == 1, "the plan holds more than one revision");
    {
        atlas_buf c = ATLAS_BUF_INIT;
        corr_task(uid, 1, "only", &c);
        T_CHECK(one_int(&g, SQL_JOBS_WITH_CORRELATION, atlas_buf_cstr(&c), NULL) == 1);
        corr_planner(uid, 1, &c);
        T_CHECK(one_int(&g, SQL_JOBS_WITH_CORRELATION, atlas_buf_cstr(&c), NULL) == 1);
        corr_planner(uid, 2, &c);
        T_CHECK_MSG(one_int(&g, SQL_JOBS_WITH_CORRELATION, atlas_buf_cstr(&c), NULL) == 0,
                    "a second planner job was asked for and none was needed");
        atlas_buf_free(&c);
    }

    atlas_buf_free(&plan);
    e2e_close(&g);
}

/* --- case 3: a blocked stage-run is answered with exactly one replan ---------- */

/* Stage 1's gate is `make four`, which fails until a fourth worker has run in
 * this repository. Revision 1's run spends its whole three-start budget failing
 * it and settles BLOCKED; revision 2 is the *same document* — the goal is frozen
 * when the plan is created — and its stage 1 is the fourth start, so it passes.
 *
 * That is the point rather than a convenience: a planner cannot be told a better
 * answer here, so nothing about the replan depends on what the planner wrote.
 * The replan is triggered by Atlas' own verdict, and the work that then succeeds
 * succeeds because the world moved. */
static const char GOAL_BLOCKING[] =
    "get the counting gate to pass.\n"
    "fake-plan-artifact:\n"
    "atlas-plan-1\n"
    "stage: 1\n"
    "task: first\n"
    "kind: tree\n"
    "title: the stage that has to run four times\n"
    "gate: make four\n"
    "prompt<<\n"
    "stage one, in the repository's own tree.\n"
    ">>\n"
    "stage: 2\n"
    "task: second\n"
    "kind: tree\n"
    "title: the stage after it\n"
    "prompt<<\n"
    "stage two, in the repository's own tree.\n"
    ">>\n"
    "fake-plan-artifact-end:\n";

static void test_a_blocked_stage_run_is_answered_with_one_replan(void) {
    atlas_err err;
    atlas_err_init(&err);
    e2e g;
    e2e_open(&g);

    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    drive_opts d;
    memset(&d, 0, sizeof(d));
    d.goal = GOAL_BLOCKING;
    d.gates = FLOOR_PASS;
    d.gate_count = 1u;
    T_OK(drive(&g, &d, &rep, &err), &err);
    T_REQUIRE(rep.plan_uid.len > 0);
    const char *uid = atlas_buf_cstr(&rep.plan_uid);

    /* Two revisions, both present: a plan's history is immutable and a replan
     * adds to it rather than replacing it. */
    T_CHECK_MSG(one_int(&g, SQL_REVISIONS, uid, NULL) == 2, "the plan holds %lld revisions",
                (long long)one_int(&g, SQL_REVISIONS, uid, NULL));
    T_CHECK_MSG(rep.rev_no == 2, "the latest revision is %d", rep.rev_no);
    T_CHECK_MSG(rep.planner_jobs == 2, "the plan spent %d planner jobs", rep.planner_jobs);
    {
        sqlite3_stmt *st = q(&g,
                             "SELECT r.rev_no, r.reason FROM orch_plan_revisions r"
                             " JOIN orch_plans p ON p.id = r.plan_id"
                             " WHERE p.plan_uid = ? ORDER BY r.rev_no",
                             uid, NULL);
        int seen = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *reason = (const char *)sqlite3_column_text(st, 1);
            int rev = sqlite3_column_int(st, 0);
            seen++;
            T_CHECK_MSG(rev == seen, "revision %d is numbered %d", seen, rev);
            T_CHECK_MSG(strcmp(reason, seen == 1 ? "INITIAL" : "REPLAN") == 0,
                        "revision %d was recorded as %s", rev, reason);
        }
        sqlite3_finalize(st);
        T_CHECK(seen == 2);
    }

    /* Revision 1's stage 1 spent the run's whole budget and blocked. The three
     * starts are the repo-tree chain's budget, not the plan's. */
    {
        atlas_buf c = ATLAS_BUF_INIT;
        corr_task(uid, 1, "first", &c);
        int64_t starts = one_int(&g,
                                 "SELECT COUNT(*) FROM orch_attempts a JOIN orch_jobs j"
                                 " ON j.id = a.job_id WHERE j.run_uid ="
                                 " (SELECT run_uid FROM orch_jobs WHERE correlation = ?)",
                                 atlas_buf_cstr(&c), NULL);
        T_CHECK_MSG(starts == ATLAS_ORCH_RUN_MAX_WORKER_STARTS,
                    "revision 1's stage-1 run started %lld workers", (long long)starts);
        int64_t blocked = one_int(&g,
                                  "SELECT COUNT(*) FROM orch_runs WHERE status = 'BLOCKED'"
                                  " AND run_uid = (SELECT run_uid FROM orch_jobs"
                                  " WHERE correlation = ?)",
                                  atlas_buf_cstr(&c), NULL);
        T_CHECK_MSG(blocked == 1, "revision 1's stage-1 run did not settle BLOCKED");
        atlas_buf_free(&c);
    }

    /* The replan the driver composed. The blocked task is named from Atlas' own
     * rows; the failed gate is not, because `job.get` exposes no failed-gate
     * index — the composer says "(none recorded)" rather than naming a gate
     * nobody established, and carries an excerpt only when a `gate.log` artifact
     * was stored inline. Either is honest and this asserts exactly that. */
    {
        atlas_buf c = ATLAS_BUF_INIT;
        atlas_buf text = ATLAS_BUF_INIT;
        corr_planner(uid, 2, &c);
        one_blob(&g, SQL_TASK_TEXT, atlas_buf_cstr(&c), NULL, &text);
        T_REQUIRE(text.len > 0);
        const char *s = atlas_buf_cstr(&text);
        T_CHECK_MSG(strstr(s, "blocked-task: first") != NULL,
                    "the replan prompt does not name the blocked task");
        T_CHECK_MSG(strstr(s, "failed-gate:") != NULL,
                    "the replan prompt states no failed-gate line");
        T_CHECK_MSG(strstr(s, "failed-gate: (none recorded)") != NULL ||
                        strstr(s, "gate-output") != NULL,
                    "the replan prompt neither named a gate nor said none was recorded");
        T_CHECK_MSG(strstr(s, "completed-work (Atlas facts):") != NULL,
                    "the replan prompt carries no completed-work section");
        atlas_buf_free(&text);
        atlas_buf_free(&c);
    }

    /* Revision 2's stages ran and the plan completed. Its stage 1 is the fourth
     * worker start in this repository, so the same gate passes. */
    atlas_plan_state st;
    read_state(uid, &st);
    T_CHECK_MSG(st.status == ATLAS_PLAN_STATUS_COMPLETED, "the plan ended %s",
                atlas_plan_status_name(st.status));
    T_CHECK(st.rev_no == 2);
    const atlas_plan_task_view *a = task_of(&st, "first");
    const atlas_plan_task_view *b = task_of(&st, "second");
    T_REQUIRE(a != NULL && b != NULL);
    T_CHECK_MSG(a->run_status == ATLAS_ORCH_RUN_ACCEPTED, "revision 2's stage 1 settled %s",
                atlas_orch_run_status_name(a->run_status));
    T_CHECK_MSG(b->run_status == ATLAS_ORCH_RUN_ACCEPTED, "revision 2's stage 2 settled %s",
                atlas_orch_run_status_name(b->run_status));

    /* Exactly one replan: a third planner job was never asked for. */
    {
        atlas_buf c = ATLAS_BUF_INIT;
        corr_planner(uid, 3, &c);
        T_CHECK_MSG(one_int(&g, SQL_JOBS_WITH_CORRELATION, atlas_buf_cstr(&c), NULL) == 0,
                    "a third planner job was submitted");
        atlas_buf_free(&c);
    }

    atlas_plandriver_report_free(&rep);
    e2e_close(&g);
}

static const atlas_test TESTS[] = {
    {"a two-stage plan completes through the production transport",
     test_a_two_stage_plan_completes_through_the_production_transport},
    {"a killed driver resumes without writing anything twice",
     test_a_killed_driver_resumes_without_writing_anything_twice},
    {"a blocked stage-run is answered with exactly one replan",
     test_a_blocked_stage_run_is_answered_with_one_replan},
};

ATLAS_TEST_MAIN("plan_e2e", TESTS)

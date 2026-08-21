/* Atlas - A12.0: the plan driver's refusals, and one iteration of each arm.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The full lifecycle — a real database, a real repository, `fake-plan` writing a
 * real artifact, real stage-runs and real settlement — is T8's suite. What is
 * proved here is the loop's own contract, against a transport faked **entirely
 * in memory**: no database, no fixture, no repository, no socket and no process.
 *
 * That is deliberate rather than lazy. Every case below is about what the driver
 * does with what it was handed, and hosting it on a real daemon would prove the
 * daemon instead. The cases are:
 *
 *   - a driver with no transport refuses rather than dereferencing one;
 *   - `--resume` names the plan and nothing else, exactly as `--memory --resume`
 *     is refused rather than ignored;
 *   - a plan needs a goal and at least one operator gate, refused before
 *     anything is created;
 *   - a plan that is already COMPLETED is reported and **nothing is started**;
 *   - a plan with no planner job gets exactly one, with the correlation and the
 *     idempotency key the scheme says and the bounds the *plan row* says;
 *   - a stage is submitted whole and then driven: the tree task as a run root
 *     carrying the *stored* merged gate list, its sibling joined to that run,
 *     and whatever is still queued afterwards carried here in key order;
 *   - a daemon that took nothing is reported as busy, with nothing else
 *     attempted;
 *   - and an iteration that moves nothing ends the invocation instead of
 *     spending the defect guard.
 *
 * The fake transport records every call, so a case can assert what was *not*
 * done as readily as what was — which is most of what these contracts are.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/plan.h"
#include "atlas/plandriver.h"
#include "atlas_test.h"

/* --- the fake transport ---------------------------------------------------- */

#define FAKE_PLAN_UID "p0123456789abcdef0123456789abcdef"
#define FAKE_RUN_UID "r0123456789abcdef0123456789abcdef"

/* One recorded submission. Copied out of the request, because the request's
 * strings are the driver's and do not outlive the call. */
typedef struct submitted {
    char driver[64];
    char correlation[128];
    char parent[64];
    char validations[256];
    int64_t max_attempts;
    int64_t run_max_parallel;
    atlas_buf task;
} submitted;

/* One task of the revision, as the fake daemon holds it. The correlation is
 * filled by the test with the one builder, so a submission is matched to a task
 * exactly the way `atlas_db_plan_state_derive` matches a job to one. */
typedef struct fake_task {
    char key[33];
    int stage;
    bool is_tree;
    char corr[128];
    /* The widths `atlas_plan_task_view` uses, so a row copied into one cannot be
     * truncated on the way. */
    char job[36];
    atlas_orch_state job_state;
    char run[36];
    atlas_orch_run_status run_status;
} fake_task;

#define FAKE_MAX_SUBS 4

typedef struct fake {
    /* What `plan_state` answers, in order. The last one is repeated, so a loop
     * that asks more often than the script provides sees the final state rather
     * than falling off the end. */
    atlas_plan_status script[8];
    int script_len;
    int state_calls;

    /* The plan row `plan_get` hands back. */
    int max_parallel;
    int rev_no;
    int stages_accepted;
    int planner_jobs_seen;

    fake_task tasks[ATLAS_PLAN_MAX_TASKS];
    int task_count;

    /* Recorded work. */
    int create_calls;
    int submit_calls;
    int drive_run_calls;
    int drive_ws_calls;
    char drove_run[ATLAS_ORCH_RUN_UID_MAX];
    char drove_ws[FAKE_MAX_SUBS][ATLAS_ORCH_UID_MAX];
    submitted subs[FAKE_MAX_SUBS];

    /* Faults the case wants. */
    bool submit_busy;
} fake;

static void fake_init(fake *f) {
    memset(f, 0, sizeof(*f));
    f->max_parallel = 2;
    for (size_t i = 0; i < FAKE_MAX_SUBS; i++) {
        atlas_buf_init(&f->subs[i].task);
    }
}

static void fake_free(fake *f) {
    for (size_t i = 0; i < FAKE_MAX_SUBS; i++) {
        atlas_buf_free(&f->subs[i].task);
    }
}

/* Adds one task of the latest revision. The correlation comes from the builder
 * the driver will use, not from a second spelling of the format. */
static void fake_add_task(fake *f, const char *key, int stage, bool is_tree) {
    atlas_err err;
    atlas_err_init(&err);
    fake_task *t = &f->tasks[f->task_count++];
    memset(t, 0, sizeof(*t));
    (void)snprintf(t->key, sizeof t->key, "%s", key);
    t->stage = stage;
    t->is_tree = is_tree;
    atlas_buf corr = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_task(FAKE_PLAN_UID, f->rev_no, key, &corr, &err), &err);
    (void)snprintf(t->corr, sizeof t->corr, "%s", atlas_buf_cstr(&corr));
    atlas_buf_free(&corr);
}

static atlas_status fk_plan_create(void *ctx, const atlas_plan_create_req *req,
                                   char plan_uid_out[ATLAS_ORCH_UID_MAX], atlas_err *err) {
    fake *f = (fake *)ctx;
    f->create_calls++;
    T_CHECK(req->repo != NULL && req->repo[0] != '\0');
    T_CHECK(req->goal != NULL && req->goal[0] != '\0');
    T_CHECK(req->gate_count > 0);
    (void)snprintf(plan_uid_out, ATLAS_ORCH_UID_MAX, "%s", FAKE_PLAN_UID);
    (void)err;
    return ATLAS_OK;
}

static atlas_status fk_plan_get(void *ctx, const char *plan_uid, atlas_plandriver_plan *out,
                                atlas_err *err) {
    fake *f = (fake *)ctx;
    (void)snprintf(out->plan_uid, sizeof out->plan_uid, "%s", plan_uid);
    (void)snprintf(out->repo, sizeof out->repo, "%s", "demo");
    out->max_parallel = f->max_parallel;
    atlas_status st = atlas_buf_set_str(&out->goal, "make the thing work", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&out->gate_floor_text, "make test\n", err);
    }
    return st;
}

static atlas_status fk_plan_state(void *ctx, const char *plan_uid, atlas_plan_state *out,
                                  atlas_err *err) {
    fake *f = (fake *)ctx;
    (void)plan_uid;
    (void)err;
    int i = f->state_calls < f->script_len ? f->state_calls : f->script_len - 1;
    f->state_calls++;
    memset(out, 0, sizeof(*out));
    out->status = f->script[i];
    out->rev_no = f->rev_no;
    out->stages_accepted = f->stages_accepted;
    out->planner_jobs_seen = f->planner_jobs_seen;
    out->task_count = f->task_count;
    for (int k = 0; k < f->task_count; k++) {
        const fake_task *t = &f->tasks[k];
        atlas_plan_task_view *v = &out->tasks[k];
        (void)snprintf(v->task_key, sizeof v->task_key, "%s", t->key);
        v->stage_no = t->stage;
        v->is_tree = t->is_tree;
        (void)snprintf(v->job_uid, sizeof v->job_uid, "%s", t->job);
        v->job_state = t->job_state;
        (void)snprintf(v->run_uid, sizeof v->run_uid, "%s", t->run);
        v->run_status = t->run_status;
    }
    return ATLAS_OK;
}

static atlas_status fk_plan_task(void *ctx, const char *plan_uid, int rev_no,
                                 const char *task_key, atlas_plandriver_task *out,
                                 atlas_err *err) {
    fake *f = (fake *)ctx;
    (void)plan_uid;
    (void)rev_no;
    (void)snprintf(out->task_key, sizeof out->task_key, "%s", task_key);
    out->stage_no = 1;
    out->is_tree = false;
    for (int i = 0; i < f->task_count; i++) {
        if (strcmp(f->tasks[i].key, task_key) == 0) {
            out->stage_no = f->tasks[i].stage;
            out->is_tree = f->tasks[i].is_tree;
            break;
        }
    }
    (void)snprintf(out->title, sizeof out->title, "%s", "a title the planner wrote");
    atlas_status st = atlas_buf_set_str(&out->prompt, "do the work", err);
    if (st == ATLAS_OK && out->is_tree) {
        /* The merged list as `orch_plan_tasks` stores it: the operator's floor
         * first, then the planner's one addition. The driver must carry these
         * bytes through untouched. */
        st = atlas_buf_set_str(&out->validations, "2:2:4:make,4:test,2:4:make,4:lint,", err);
    }
    return st;
}

static atlas_status fk_plan_revision_add(void *ctx, const char *plan_uid, const char *planner_job,
                                         int rev_no, const char *reason, atlas_plan_refusal *ref,
                                         atlas_err *err) {
    (void)ctx;
    (void)plan_uid;
    (void)planner_job;
    (void)rev_no;
    (void)reason;
    (void)ref;
    return atlas_err_set(err, ATLAS_ERR_USAGE, "no case here drives an ingest");
}

static atlas_status fk_job_submit(void *ctx, const atlas_plan_job_req *req,
                                  char job_uid_out[ATLAS_ORCH_UID_MAX], atlas_err *err) {
    fake *f = (fake *)ctx;
    if (f->submit_busy) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "BUSY: the Atlas daemon is busy; nothing was queued");
    }
    T_REQUIRE(f->submit_calls < FAKE_MAX_SUBS);
    submitted *s = &f->subs[f->submit_calls];
    f->submit_calls++;
    (void)snprintf(s->driver, sizeof s->driver, "%s", req->driver != NULL ? req->driver : "");
    (void)snprintf(s->correlation, sizeof s->correlation, "%s",
                   req->correlation != NULL ? req->correlation : "");
    (void)snprintf(s->parent, sizeof s->parent, "%s",
                   req->parent_job_uid != NULL ? req->parent_job_uid : "");
    (void)snprintf(s->validations, sizeof s->validations, "%s",
                   req->validations != NULL ? req->validations : "");
    s->max_attempts = req->max_attempts;
    s->run_max_parallel = req->run_max_parallel;
    atlas_status st = atlas_buf_set_str(&s->task, req->task != NULL ? req->task : "", err);

    /* One fresh job uid, and — for a task of the revision — the row the daemon
     * would then hold for it. */
    (void)snprintf(job_uid_out, ATLAS_ORCH_UID_MAX, "j0000000000000000000000000000000%d",
                   f->submit_calls);
    for (int i = 0; i < f->task_count; i++) {
        if (strcmp(f->tasks[i].corr, s->correlation) != 0) {
            continue;
        }
        (void)snprintf(f->tasks[i].job, sizeof f->tasks[i].job, "%s", job_uid_out);
        f->tasks[i].job_state = ATLAS_ORCH_STATE_QUEUED;
        if (f->tasks[i].is_tree) {
            (void)snprintf(f->tasks[i].run, sizeof f->tasks[i].run, "%s", FAKE_RUN_UID);
            f->tasks[i].run_status = ATLAS_ORCH_RUN_ACTIVE;
        }
        break;
    }
    return st;
}

static atlas_status fk_job_get(void *ctx, const char *job_uid, atlas_plan_job_view *out,
                               atlas_err *err) {
    (void)ctx;
    (void)job_uid;
    (void)err;
    out->state = ATLAS_ORCH_STATE_FAILED;
    return ATLAS_OK;
}

/* The run driver, faked: it carries the run's repo-tree chain to a settled
 * answer, which here is the tree task succeeding and its run being accepted. */
static atlas_status fk_drive_run(void *ctx, const char *run_uid, atlas_err *err) {
    fake *f = (fake *)ctx;
    (void)err;
    f->drive_run_calls++;
    (void)snprintf(f->drove_run, sizeof f->drove_run, "%s", run_uid);
    for (int i = 0; i < f->task_count; i++) {
        if (f->tasks[i].is_tree && strcmp(f->tasks[i].run, run_uid) == 0) {
            f->tasks[i].job_state = ATLAS_ORCH_STATE_SUCCEEDED;
            f->tasks[i].run_status = ATLAS_ORCH_RUN_ACCEPTED;
        }
    }
    return ATLAS_OK;
}

static atlas_status fk_drive_ws(void *ctx, const char *job_uid, atlas_err *err) {
    fake *f = (fake *)ctx;
    (void)err;
    if (f->drive_ws_calls < FAKE_MAX_SUBS) {
        (void)snprintf(f->drove_ws[f->drive_ws_calls], ATLAS_ORCH_UID_MAX, "%s", job_uid);
    }
    f->drive_ws_calls++;
    for (int i = 0; i < f->task_count; i++) {
        if (strcmp(f->tasks[i].job, job_uid) == 0) {
            f->tasks[i].job_state = ATLAS_ORCH_STATE_SUCCEEDED;
        }
    }
    return ATLAS_OK;
}

static void wire(atlas_plandriver_opts *o, fake *f) {
    memset(o, 0, sizeof(*o));
    o->transport.ctx = f;
    o->transport.plan_create = fk_plan_create;
    o->transport.plan_get = fk_plan_get;
    o->transport.plan_state = fk_plan_state;
    o->transport.plan_task = fk_plan_task;
    o->transport.plan_revision_add = fk_plan_revision_add;
    o->transport.job_submit = fk_job_submit;
    o->transport.job_get = fk_job_get;
    o->transport.drive_run = fk_drive_run;
    o->transport.drive_workspace_job = fk_drive_ws;
    /* The fakes, so no case can reach a live model by omission. */
    o->planner_driver = "fake-plan";
    o->tree_driver = "fake-repo";
    o->side_driver = "fake";
}

static const char *const ONE_GATE[] = {"make test"};

static void creating(atlas_plandriver_opts *o) {
    o->repo = "demo";
    o->goal = "make the thing work";
    o->gate_floor = ONE_GATE;
    o->gate_count = 1;
}

/* --- the cases -------------------------------------------------------------- */

/* A transport is not optional, and a missing one is a defect in Atlas rather
 * than a mistake an operator made — the same claim, in the same words, that the
 * run driver makes about its own. */
static void test_no_transport(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_opts o;
    memset(&o, 0, sizeof(o));
    creating(&o);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_FAILS_WITH(atlas_plandriver_run(&o, &rep, &err), ATLAS_ERR_INTERNAL, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "no transport") != NULL,
                "expected a missing-transport sentence, got: %s", atlas_err_msg(&err));
    atlas_plandriver_report_free(&rep);
}

/* A10.1's rule for `--memory --resume`, one layer up: a flag that was quietly
 * dropped reads exactly like one that was honoured, so naming a goal, a gate or
 * a parallelism beside a resumed plan is refused. */
static void test_resume_names_the_plan_and_nothing_else(void) {
    struct {
        const char *what;
        const char *goal;
        size_t gates;
        int parallel;
    } cases[] = {
        {"a goal", "make the thing work", 0, 0},
        {"a gate", NULL, 1, 0},
        {"a parallelism", NULL, 0, 4},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fake f;
        fake_init(&f);
        atlas_plandriver_opts o;
        wire(&o, &f);
        o.plan_uid = FAKE_PLAN_UID;
        o.goal = cases[i].goal;
        o.gate_floor = cases[i].gates > 0 ? ONE_GATE : NULL;
        o.gate_count = cases[i].gates;
        o.max_parallel = cases[i].parallel;

        atlas_err err;
        atlas_err_init(&err);
        atlas_plandriver_report rep;
        atlas_plandriver_report_init(&rep);
        T_FAILS_WITH(atlas_plandriver_run(&o, &rep, &err), ATLAS_ERR_USAGE, &err);
        T_CHECK_MSG(strstr(atlas_err_msg(&err), "resume") != NULL,
                    "%s: expected the resume refusal, got: %s", cases[i].what,
                    atlas_err_msg(&err));
        /* Refused before anything was read or created. */
        T_EQ_INT(f.create_calls, 0);
        T_EQ_INT(f.state_calls, 0);
        atlas_plandriver_report_free(&rep);
        fake_free(&f);
    }
}

/* The operator brings the goal and the gate floor. Both refusals happen before
 * `plan.create` is called, so a refused invocation leaves no plan behind. */
static void test_a_plan_needs_a_goal_and_a_floor(void) {
    fake f;
    fake_init(&f);
    atlas_plandriver_opts o;
    wire(&o, &f);
    o.repo = "demo";
    o.gate_floor = ONE_GATE;
    o.gate_count = 1;

    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_FAILS_WITH(atlas_plandriver_run(&o, &rep, &err), ATLAS_ERR_USAGE, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "goal") != NULL, "expected a goal refusal, got: %s",
                atlas_err_msg(&err));
    atlas_plandriver_report_free(&rep);

    o.goal = "make the thing work";
    o.gate_floor = NULL;
    o.gate_count = 0;
    atlas_err_init(&err);
    atlas_plandriver_report_init(&rep);
    T_FAILS_WITH(atlas_plandriver_run(&o, &rep, &err), ATLAS_ERR_USAGE, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "gate") != NULL, "expected a gate refusal, got: %s",
                atlas_err_msg(&err));
    atlas_plandriver_report_free(&rep);

    T_EQ_INT(f.create_calls, 0);
    fake_free(&f);
}

/* A plan that has already finished is reported and touched not at all — the
 * shape a terminal run gets from A11.1's driver. */
static void test_a_completed_plan_starts_nothing(void) {
    fake f;
    fake_init(&f);
    f.script[0] = ATLAS_PLAN_STATUS_COMPLETED;
    f.script_len = 1;
    f.rev_no = 1;
    f.stages_accepted = 2;
    f.planner_jobs_seen = 1;

    atlas_plandriver_opts o;
    wire(&o, &f);
    creating(&o);

    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_OK(atlas_plandriver_run(&o, &rep, &err), &err);

    T_EQ_STR(atlas_buf_cstr(&rep.plan_uid), FAKE_PLAN_UID);
    T_EQ_INT(rep.status, ATLAS_PLAN_STATUS_COMPLETED);
    T_EQ_INT(rep.rev_no, 1);
    T_EQ_INT(rep.stages_accepted, 2);
    T_EQ_INT(rep.planner_jobs, 1);
    T_CHECK(!rep.busy);
    /* One plan was created, one state read taken, and nothing else happened. */
    T_EQ_INT(f.create_calls, 1);
    T_EQ_INT(f.state_calls, 1);
    T_EQ_INT(f.submit_calls, 0);
    T_EQ_INT(f.drive_run_calls, 0);
    T_EQ_INT(f.drive_ws_calls, 0);

    atlas_plandriver_report_free(&rep);
    fake_free(&f);
}

/* One iteration of the loop's PLANNING arm.
 *
 * The correlation and the idempotency key are one string from
 * `atlas_plan_correlation_planner`, the bound the planner is shown comes from
 * the **plan row** — the same number the daemon will parse the answer with, T4's
 * handoff 3 — and the job is a single-attempt workspace job with no gate and no
 * parent. */
static void test_planning_submits_one_planner_job(void) {
    fake f;
    fake_init(&f);
    f.script[0] = ATLAS_PLAN_STATUS_PLANNING;
    f.script[1] = ATLAS_PLAN_STATUS_COMPLETED;
    f.script_len = 2;

    atlas_plandriver_opts o;
    wire(&o, &f);
    creating(&o);

    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_OK(atlas_plandriver_run(&o, &rep, &err), &err);

    T_REQUIRE(f.submit_calls == 1);
    T_EQ_STR(f.subs[0].driver, "fake-plan");
    T_EQ_INT(f.subs[0].max_attempts, 1);
    T_EQ_INT(f.subs[0].run_max_parallel, 0);
    T_EQ_STR(f.subs[0].parent, "");
    T_EQ_STR(f.subs[0].validations, "");

    /* The one builder, asked here too rather than spelled out: two spellings of
     * one format are two answers to "is this job this plan's". */
    atlas_buf want = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_planner(FAKE_PLAN_UID, 1, &want, &err), &err);
    T_EQ_STR(f.subs[0].correlation, atlas_buf_cstr(&want));
    atlas_buf_free(&want);

    const char *task = atlas_buf_cstr(&f.subs[0].task);
    T_CHECK_MSG(strncmp(task, "atlas-plan-request:", 19) == 0,
                "expected the planner request header, got: %.32s", task);
    /* `max_parallel` 2 on the plan row means one side task per stage. If the
     * driver had composed against its own opts (which name none) the sentence
     * would read differently, and the daemon would parse the answer against 2. */
    T_CHECK_MSG(strstr(task, "side tasks per stage: at most 1") != NULL,
                "the planner was shown a bound that is not the plan row's");

    /* It was driven in this process, not left for a dispatcher to find. */
    T_EQ_INT(f.drive_ws_calls, 1);
    T_EQ_STR(f.drove_ws[0], "j00000000000000000000000000000001");

    T_EQ_INT(rep.status, ATLAS_PLAN_STATUS_COMPLETED);
    atlas_plandriver_report_free(&rep);
    fake_free(&f);
}

/* One iteration of the loop's EXECUTING arm.
 *
 * A stage is admitted whole and then driven: the tree task first, as the root of
 * its own run and carrying the merged gate list the revision compiled; then the
 * sibling, joined to that run by naming the tree job as its parent; then the run
 * driver over the chain; then whatever is still queued, carried here. */
static void test_a_stage_is_submitted_whole_then_driven(void) {
    fake f;
    fake_init(&f);
    f.rev_no = 1;
    f.script[0] = ATLAS_PLAN_STATUS_EXECUTING;
    f.script[1] = ATLAS_PLAN_STATUS_COMPLETED;
    f.script_len = 2;
    fake_add_task(&f, "build", 1, true);
    fake_add_task(&f, "notes", 1, false);

    atlas_plandriver_opts o;
    wire(&o, &f);
    creating(&o);

    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_OK(atlas_plandriver_run(&o, &rep, &err), &err);

    T_REQUIRE(f.submit_calls == 2);
    /* The tree task: a run root, the chain's worker-start budget, the plan's
     * parallelism, and the stored merged list carried through byte for byte. */
    const submitted *tree = &f.subs[0];
    T_EQ_STR(tree->driver, "fake-repo");
    T_EQ_INT(tree->max_attempts, ATLAS_ORCH_RUN_MAX_WORKER_STARTS);
    T_EQ_INT(tree->run_max_parallel, 2);
    T_EQ_STR(tree->parent, "");
    T_EQ_STR(tree->validations, "2:2:4:make,4:test,2:4:make,4:lint,");
    T_CHECK_MSG(strncmp(atlas_buf_cstr(&tree->task), "atlas-plan-task:", 16) == 0,
                "expected the executor header, got: %.32s", atlas_buf_cstr(&tree->task));

    /* The sibling: one attempt, no gate of its own, and the tree job as its
     * parent — which is how it joins the stage's run rather than starting one. */
    const submitted *side = &f.subs[1];
    T_EQ_STR(side->driver, "fake");
    T_EQ_INT(side->max_attempts, 1);
    T_EQ_INT(side->run_max_parallel, 0);
    T_EQ_STR(side->parent, "j00000000000000000000000000000001");
    T_EQ_STR(side->validations, "");

    atlas_buf want = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_task(FAKE_PLAN_UID, 1, "build", &want, &err), &err);
    T_EQ_STR(tree->correlation, atlas_buf_cstr(&want));
    T_OK(atlas_plan_correlation_task(FAKE_PLAN_UID, 1, "notes", &want, &err), &err);
    T_EQ_STR(side->correlation, atlas_buf_cstr(&want));
    atlas_buf_free(&want);

    /* The chain was driven as a run, and the sibling that was still queued
     * afterwards was carried here. */
    T_EQ_INT(f.drive_run_calls, 1);
    T_EQ_STR(f.drove_run, FAKE_RUN_UID);
    T_EQ_INT(f.drive_ws_calls, 1);
    T_EQ_STR(f.drove_ws[0], "j00000000000000000000000000000002");

    atlas_plandriver_report_free(&rep);
    fake_free(&f);
}

/* `BUSY:` is the daemon's promise that it took nothing. The invocation is over,
 * the plan is untouched and resumable, and it is emphatically not BLOCKED. */
static void test_busy_is_reported_and_is_not_a_verdict(void) {
    fake f;
    fake_init(&f);
    f.script[0] = ATLAS_PLAN_STATUS_PLANNING;
    f.script_len = 1;
    f.submit_busy = true;

    atlas_plandriver_opts o;
    wire(&o, &f);
    creating(&o);

    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_OK(atlas_plandriver_run(&o, &rep, &err), &err);
    T_CHECK(rep.busy);
    T_CHECK_MSG(rep.status != ATLAS_PLAN_STATUS_BLOCKED, "busy was reported as a verdict");
    T_EQ_INT(f.submit_calls, 0);
    T_EQ_INT(f.drive_ws_calls, 0);
    atlas_plandriver_report_free(&rep);
    fake_free(&f);
}

/* An iteration that moves nothing ends the invocation.
 *
 * Here the fake daemon never records the planner job it was handed, so the
 * derived state is identical on the second read. Without the guard the loop
 * would submit once per iteration until the defect ceiling — eighty-five
 * submissions — which is precisely the shape of "a process pretending to make
 * progress". */
static void test_a_stalled_iteration_ends_the_invocation(void) {
    fake f;
    fake_init(&f);
    f.script[0] = ATLAS_PLAN_STATUS_PLANNING;
    f.script_len = 1;

    atlas_plandriver_opts o;
    wire(&o, &f);
    creating(&o);

    atlas_err err;
    atlas_err_init(&err);
    atlas_plandriver_report rep;
    atlas_plandriver_report_init(&rep);
    T_OK(atlas_plandriver_run(&o, &rep, &err), &err);
    T_EQ_INT(f.submit_calls, 1);
    T_EQ_INT(f.state_calls, 2);
    T_EQ_INT(rep.status, ATLAS_PLAN_STATUS_PLANNING);
    T_CHECK(!rep.busy);
    atlas_plandriver_report_free(&rep);
    fake_free(&f);
}

static const atlas_test TESTS[] = {
    {"a driver with no transport refuses", test_no_transport},
    {"a resume names the plan and nothing else", test_resume_names_the_plan_and_nothing_else},
    {"a plan needs a goal and a gate floor", test_a_plan_needs_a_goal_and_a_floor},
    {"a completed plan is reported and starts nothing", test_a_completed_plan_starts_nothing},
    {"planning submits and drives one planner job", test_planning_submits_one_planner_job},
    {"a stage is submitted whole then driven", test_a_stage_is_submitted_whole_then_driven},
    {"a busy daemon is reported and is not a verdict", test_busy_is_reported_and_is_not_a_verdict},
    {"a stalled iteration ends the invocation", test_a_stalled_iteration_ends_the_invocation},
};

ATLAS_TEST_MAIN("plan_driver", TESTS)

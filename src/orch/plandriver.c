/* Atlas - A12.0: the operator's foreground plan driver.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `atlas/plandriver.h` for what this is and, more importantly, for what it
 * deliberately is not.
 *
 * The loop is one sentence long: **re-derive the plan's state, act once on what
 * it says, and go round again.** Everything else here follows from that.
 *
 *   1. There is no local state that must survive a crash. The plan's status, its
 *      revision, its planner jobs and every task's job are read afresh from
 *      `plan_state` at the top of every iteration, and a driver killed anywhere
 *      and started again reaches the same place.
 *   2. Every write is idempotent. A submission carries an idempotency key that
 *      is the same string as its correlation, both built by the one builder pair
 *      in `db_plan.c`, so a re-issued submission is handed the job that already
 *      exists rather than creating a second one. Ingesting a revision is
 *      deterministic over stored bytes, so a re-run either compiles the same
 *      revision or reproduces the same refusal.
 *   3. Nothing here decides anything. A stage's verdict is its *run's* status,
 *      which the daemon derived from a completion Atlas classified; the plan's
 *      status is derived on read from stored rows. The replan trigger is a
 *      stage-run that settled BLOCKED — Atlas' own verdict — and never a
 *      sentence a worker wrote.
 *   4. It stops when nothing it can do would move the plan. An iteration that
 *      leaves the derived state byte-for-byte identical to the previous one is
 *      the end of this invocation: the work that remains belongs to an
 *      operator's dispatcher, or to a driver that already holds the task, and
 *      spinning would be this process pretending to make progress.
 */
#define _GNU_SOURCE 1

#include "atlas/plandriver.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/rundriver.h"

void atlas_plan_job_view_init(atlas_plan_job_view *v) {
    memset(v, 0, sizeof(*v));
    atlas_buf_init(&v->gate_output);
}

void atlas_plan_job_view_free(atlas_plan_job_view *v) {
    if (v != NULL) {
        atlas_buf_free(&v->gate_output);
    }
}

void atlas_plandriver_task_init(atlas_plandriver_task *t) {
    memset(t, 0, sizeof(*t));
    atlas_buf_init(&t->prompt);
    atlas_buf_init(&t->validations);
}

void atlas_plandriver_task_free(atlas_plandriver_task *t) {
    if (t != NULL) {
        atlas_buf_free(&t->prompt);
        atlas_buf_free(&t->validations);
    }
}

void atlas_plandriver_plan_init(atlas_plandriver_plan *p) {
    memset(p, 0, sizeof(*p));
    atlas_buf_init(&p->goal);
    atlas_buf_init(&p->gate_floor_text);
}

void atlas_plandriver_plan_free(atlas_plandriver_plan *p) {
    if (p != NULL) {
        atlas_buf_free(&p->goal);
        atlas_buf_free(&p->gate_floor_text);
    }
}

void atlas_plandriver_report_init(atlas_plandriver_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->plan_uid);
}

void atlas_plandriver_report_free(atlas_plandriver_report *r) {
    if (r != NULL) {
        atlas_buf_free(&r->plan_uid);
    }
}

/* --- the defect guard -------------------------------------------------------
 *
 * The season's stated worst case, plus a margin: at most
 * `ATLAS_PLAN_MAX_PLANNER_JOBS` planner starts, and per compiled revision at
 * most `ATLAS_PLAN_MAX_STAGES` stage-runs each costing at most a repo-tree
 * chain's budget plus its siblings. It is not a budget — the planner-job
 * ceiling, the revision ceiling and each run's own worker-start budget are the
 * bounds that matter and the daemon enforces all three — it is the guard that
 * keeps a defect below from becoming an unbounded loop, which is the one failure
 * mode a driver must not have. The rundriver's pattern, one layer up. */
#define PLAN_ITERATION_CEILING                                                             \
    ((int64_t)(ATLAS_PLAN_MAX_PLANNER_JOBS +                                               \
               ATLAS_PLAN_MAX_REVISIONS * ATLAS_PLAN_MAX_STAGES *                          \
                   (ATLAS_ORCH_RUN_MAX_WORKER_STARTS + ATLAS_PLAN_MAX_SIDE_PER_STAGE) +    \
               8))

static void say(const atlas_plandriver_opts *o, const char *fmt, ...) {
    if (o->log == NULL) {
        return;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(o->log, "%s plan ", at);
    (void)vfprintf(o->log, fmt, ap);
    (void)fputc('\n', o->log);
    va_end(ap);
    (void)fflush(o->log);
}

static void nap(int64_t ms) {
    if (ms > 0) {
        struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
        (void)nanosleep(&ts, NULL);
    }
}

/* Whether a failed call should be asked again — and, when it should, the pause
 * before it is.
 *
 * The classification is `atlas_err_is_transport`, stamped by the client layer
 * that held the file descriptor, on the same budget the run driver uses:
 * `ATLAS_RUN_XPORT_TRIES` attempts and `ATLAS_RUN_XPORT_PAUSE_MS` between them,
 * from `atlas/rundriver.h`, because two spellings of one budget are two budgets.
 *
 * `BUSY:` is deliberately **not** retried here. It is the daemon's promise that
 * it took nothing, and this driver's answer to that is to report `busy` and
 * return: the plan is untouched and the same invocation may simply be repeated,
 * which is A11.1's contract for a run and is the contract the brief fixes for a
 * plan. Retrying it here would turn a resumable invocation into a wait. */
static bool xport_again(const atlas_plandriver_opts *o, const char *what, int *tries,
                        atlas_err *err) {
    if (!atlas_err_is_transport(err) || *tries >= ATLAS_RUN_XPORT_TRIES) {
        return false;
    }
    (*tries)++;
    say(o, "%s: the answer did not arrive (%s); asking again (%d/%d)", what, atlas_err_msg(err),
        *tries, ATLAS_RUN_XPORT_TRIES);
    nap(o->xport_pause_ms > 0 ? o->xport_pause_ms : ATLAS_RUN_XPORT_PAUSE_MS);
    atlas_err_init(err);
    return true;
}

static bool is_busy(const atlas_err *err) {
    return atlas_ipc_message_is_busy(err->msg);
}

/* --- reads ------------------------------------------------------------------ */

static atlas_status read_state(const atlas_plandriver_opts *o, const char *plan_uid,
                               atlas_plan_state *out, atlas_err *err) {
    int tries = 0;
    atlas_status st;
    do {
        memset(out, 0, sizeof(*out));
        st = o->transport.plan_state(o->transport.ctx, plan_uid, out, err);
    } while (st != ATLAS_OK && xport_again(o, "the plan's state", &tries, err));
    return st;
}

static atlas_status read_plan(const atlas_plandriver_opts *o, const char *plan_uid,
                              atlas_plandriver_plan *out, atlas_err *err) {
    int tries = 0;
    atlas_status st;
    do {
        st = o->transport.plan_get(o->transport.ctx, plan_uid, out, err);
    } while (st != ATLAS_OK && xport_again(o, "the plan", &tries, err));
    return st;
}

/* --- what the loop can act on ----------------------------------------------- */

/* The lowest stage whose tree task has not been accepted.
 *
 * A stage is finished when its **run** settled ACCEPTED, not when its job
 * succeeded: a repo-tree job that failed its gate has a follow-up coming, and a
 * run that succeeded everywhere still waits for its workspace siblings to end
 * before it settles. Reading the job instead would call a stage finished before
 * Atlas had decided anything about it. */
static const atlas_plan_task_view *lowest_open_tree(const atlas_plan_state *st) {
    const atlas_plan_task_view *best = NULL;
    for (int i = 0; i < st->task_count; i++) {
        const atlas_plan_task_view *v = &st->tasks[i];
        if (!v->is_tree || v->run_status == ATLAS_ORCH_RUN_ACCEPTED) {
            continue;
        }
        if (best == NULL || v->stage_no < best->stage_no) {
            best = v;
        }
    }
    return best;
}

static const atlas_plan_task_view *tree_of_stage(const atlas_plan_state *st, int stage_no) {
    for (int i = 0; i < st->task_count; i++) {
        if (st->tasks[i].is_tree && st->tasks[i].stage_no == stage_no) {
            return &st->tasks[i];
        }
    }
    return NULL;
}

/* The task a replan prompt is about.
 *
 * A job that ended FAILED is preferred over a run that settled BLOCKED, because
 * the failure is the *cause* and the settlement is its consequence: when a
 * workspace sibling vetoed a stage the tree job may well have succeeded, and
 * naming the tree task there would point a planner at work that went fine. When
 * nothing failed outright — a cancelled task, a recovery — the first task the
 * revision holds that is neither running nor successful is named instead.
 * Deterministic either way: the state's tasks are ordered by `(stage_no, id)`.
 *
 * A **tree** task whose run settled ACCEPTED is never named, whatever its job
 * says. The view carries the *first* job of the task's chain — a follow-up
 * inherits its parent's correlation verbatim — so a stage that failed its gate
 * and was recovered by that follow-up still reads FAILED here while Atlas has
 * accepted it. Naming it would point the planner at work that stands, which is
 * the same mistake as naming a tree task a sibling vetoed. `lowest_open_tree`
 * already reads a stage this way; so does the derivation. */
static const atlas_plan_task_view *blocking_task(const atlas_plan_state *st) {
    const atlas_plan_task_view *first_bad = NULL;
    for (int i = 0; i < st->task_count; i++) {
        const atlas_plan_task_view *v = &st->tasks[i];
        if (v->job_uid[0] == '\0') {
            continue;
        }
        if (v->is_tree && v->run_status == ATLAS_ORCH_RUN_ACCEPTED) {
            continue;
        }
        if (v->job_state == ATLAS_ORCH_STATE_FAILED) {
            return v;
        }
        const bool bad = v->is_tree ? (v->run_status == ATLAS_ORCH_RUN_BLOCKED)
                                    : (atlas_orch_state_is_terminal(v->job_state) &&
                                       v->job_state != ATLAS_ORCH_STATE_SUCCEEDED);
        if (bad && first_bad == NULL) {
            first_bad = v;
        }
    }
    return first_bad;
}

/* A fingerprint of everything the loop acts on.
 *
 * Compared between iterations to answer one question: did the previous iteration
 * move anything? Built from the derived state alone, because that is the only
 * thing the loop reads — a value that changed and is not in here is a value no
 * decision below depends on. */
static atlas_status fingerprint(const atlas_plan_state *st, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status s = atlas_buf_appendf(out, err, "%d|%d|%d|%s|%d|%d|%d",
                                       (int)st->status, st->rev_no, st->planner_jobs_seen,
                                       st->planner_job_uid, (int)st->planner_job_state,
                                       st->task_count, st->replan_wanted ? 1 : 0);
    for (int i = 0; s == ATLAS_OK && i < st->task_count; i++) {
        const atlas_plan_task_view *v = &st->tasks[i];
        s = atlas_buf_appendf(out, err, "|%s:%d:%s:%d:%s:%d", v->task_key, v->stage_no, v->job_uid,
                              (int)v->job_state, v->run_uid, (int)v->run_status);
    }
    return s;
}

/* --- submitting one of the revision's tasks ---------------------------------
 *
 * The prompt and the merged gate list come from the stored task row, never from
 * a re-parse of the document and never from a merge performed here: the merge is
 * the plan write point's, it happened once when the revision compiled, and a
 * second implementation of it in a driver would be a second answer to what an
 * accepted stage was gated on.
 */
static atlas_status submit_task(const atlas_plandriver_opts *o, const atlas_plandriver_plan *plan,
                                int rev_no, const atlas_plan_task_view *v, const char *parent,
                                char job_out[ATLAS_ORCH_UID_MAX], bool *busy_out, atlas_err *err) {
    atlas_plandriver_task row;
    atlas_plandriver_task_init(&row);
    atlas_buf text = ATLAS_BUF_INIT;
    atlas_buf corr = ATLAS_BUF_INIT;

    int tries = 0;
    atlas_status st;
    do {
        st = o->transport.plan_task(o->transport.ctx, plan->plan_uid, rev_no, v->task_key, &row,
                                    err);
    } while (st != ATLAS_OK && xport_again(o, "a plan task", &tries, err));

    if (st == ATLAS_OK) {
        /* A borrowed view of the row: `prompt` aliases the row's buffer and is
         * never freed through this struct. `atlas_plan_compose_executor` reads
         * it and owns nothing. The gates are deliberately left empty — the
         * composer does not read them, and a half-filled gate list here would be
         * a second place the merged list appeared to live. */
        atlas_plan_doc_task doc;
        memset(&doc, 0, sizeof(doc));
        (void)snprintf(doc.key, sizeof doc.key, "%s", row.task_key);
        doc.stage_no = row.stage_no;
        doc.is_tree = row.is_tree;
        (void)snprintf(doc.title, sizeof doc.title, "%s", row.title);
        doc.prompt = row.prompt;
        st = atlas_plan_compose_executor(plan->plan_uid, rev_no, &doc, &text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_plan_correlation_task(plan->plan_uid, rev_no, v->task_key, &corr, err);
    }
    if (st == ATLAS_OK) {
        atlas_plan_job_req req;
        memset(&req, 0, sizeof(req));
        req.repo = plan->repo;
        req.driver = row.is_tree ? (o->tree_driver != NULL && o->tree_driver[0] != '\0'
                                        ? o->tree_driver
                                        : "claude-repo")
                                 : (o->side_driver != NULL && o->side_driver[0] != '\0'
                                        ? o->side_driver
                                        : "claude");
        req.mode = o->mode;
        req.task = atlas_buf_cstr(&text);
        /* One string for both, from the one builder. */
        req.correlation = atlas_buf_cstr(&corr);
        req.parent_job_uid = parent;
        /* A workspace sibling declares no gate, and the write point refuses a
         * repo-tree task that has none — which is why only the tree task carries
         * a list at all. */
        req.validations = row.is_tree ? atlas_buf_cstr(&row.validations) : "";
        /* The repo-tree chain's budget is the run's three worker starts; a
         * sibling is bounded by its own single attempt and spends none of it. */
        req.max_attempts = row.is_tree ? ATLAS_ORCH_RUN_MAX_WORKER_STARTS : 1;
        req.wall_timeout_ms = o->wall_timeout_ms;
        req.idle_timeout_ms = o->idle_timeout_ms;
        /* Fixed at the root and refused on a child. A stage's tree task is the
         * root of its run; a sibling names nothing. */
        req.run_max_parallel = (parent == NULL || parent[0] == '\0') ? plan->max_parallel : 0;
        st = o->transport.job_submit(o->transport.ctx, &req, job_out, err);
        if (st != ATLAS_OK && is_busy(err)) {
            *busy_out = true;
            atlas_err_init(err);
            st = ATLAS_OK;
            job_out[0] = '\0';
        } else if (st == ATLAS_OK) {
            say(o, "stage %d task %s submitted as %s (%s)", row.stage_no, row.task_key, job_out,
                req.driver);
        }
    }

    atlas_buf_free(&corr);
    atlas_buf_free(&text);
    atlas_plandriver_task_free(&row);
    return st;
}

static atlas_status drive_ws(const atlas_plandriver_opts *o, const char *job_uid,
                             atlas_err *err) {
    int tries = 0;
    atlas_status st;
    do {
        st = o->transport.drive_workspace_job(o->transport.ctx, job_uid, err);
    } while (st != ATLAS_OK && xport_again(o, "a workspace attempt", &tries, err));
    return st;
}

/* --- the EXECUTING arm ------------------------------------------------------ */

/* Sorts the stage's side tasks by key, in place, over borrowed pointers. An
 * insertion sort over at most `ATLAS_PLAN_MAX_SIDE_PER_STAGE` elements: the
 * order has to be *stated* — the brief says task-key order — because two runs of
 * one driver that drove siblings in different orders would be two different
 * executions of one plan. */
static void sort_by_key(const atlas_plan_task_view **v, size_t n) {
    for (size_t i = 1; i < n; i++) {
        const atlas_plan_task_view *cur = v[i];
        size_t j = i;
        while (j > 0 && strcmp(v[j - 1]->task_key, cur->task_key) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = cur;
    }
}

static atlas_status step_executing(const atlas_plandriver_opts *o,
                                   const atlas_plandriver_plan *plan, const atlas_plan_state *st,
                                   atlas_plandriver_report *rep, atlas_err *err) {
    const atlas_plan_task_view *tree = lowest_open_tree(st);
    if (tree == NULL) {
        say(o, "revision %d has no stage left to start", st->rev_no);
        return ATLAS_OK;
    }
    if (tree->run_status == ATLAS_ORCH_RUN_BLOCKED) {
        /* Nothing to drive: the run is terminal and the plan's own state will
         * ask for a replan as soon as everything else is quiescent. */
        say(o, "stage %d settled BLOCKED; this driver starts nothing more in it", tree->stage_no);
        return ATLAS_OK;
    }
    const int stage = tree->stage_no;

    char tree_job[ATLAS_ORCH_UID_MAX];
    memset(tree_job, 0, sizeof tree_job);
    atlas_status s = ATLAS_OK;
    if (tree->job_uid[0] == '\0') {
        s = submit_task(o, plan, st->rev_no, tree, NULL, tree_job, &rep->busy, err);
    } else {
        (void)snprintf(tree_job, sizeof tree_job, "%s", tree->job_uid);
    }
    if (s != ATLAS_OK || rep->busy) {
        return s;
    }
    if (tree_job[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the daemon accepted a stage's task without naming it");
    }

    /* The siblings, submitted before anything is driven so that every admission
     * this stage will make is made inside the run's bounds at once. The parser
     * bounded them at `max_parallel - 1`, so the tree task and its siblings
     * together fit the run's own ceiling by construction. */
    for (int i = 0; s == ATLAS_OK && !rep->busy && i < st->task_count; i++) {
        const atlas_plan_task_view *v = &st->tasks[i];
        if (v->is_tree || v->stage_no != stage || v->job_uid[0] != '\0') {
            continue;
        }
        char side_job[ATLAS_ORCH_UID_MAX];
        memset(side_job, 0, sizeof side_job);
        s = submit_task(o, plan, st->rev_no, v, tree_job, side_job, &rep->busy, err);
    }
    if (s != ATLAS_OK || rep->busy) {
        return s;
    }

    /* Which run the stage became is the daemon's answer, read back rather than
     * returned by the submission: a resumed driver has to be able to find it the
     * same way, and one path that works in both cases is better than two. */
    atlas_plan_state now;
    s = read_state(o, plan->plan_uid, &now, err);
    if (s != ATLAS_OK) {
        return s;
    }
    const atlas_plan_task_view *tree_now = tree_of_stage(&now, stage);
    if (tree_now != NULL && tree_now->run_uid[0] != '\0') {
        int tries = 0;
        say(o, "driving stage %d as run %s", stage, tree_now->run_uid);
        do {
            s = o->transport.drive_run(o->transport.ctx, tree_now->run_uid, err);
        } while (s != ATLAS_OK && xport_again(o, "the stage's run", &tries, err));
        if (s != ATLAS_OK) {
            return s;
        }
    }

    /* Whatever is still queued beside the chain is carried here, in task-key
     * order. This is the single-process progress guarantee and nothing more:
     * genuine overlap comes from dispatcher processes an operator started, and a
     * job that belongs to one of those is simply not granted to us. */
    s = read_state(o, plan->plan_uid, &now, err);
    if (s != ATLAS_OK) {
        return s;
    }
    const atlas_plan_task_view *queued[ATLAS_PLAN_MAX_TASKS];
    size_t nq = 0;
    for (int i = 0; i < now.task_count; i++) {
        const atlas_plan_task_view *v = &now.tasks[i];
        if (!v->is_tree && v->stage_no == stage && v->job_uid[0] != '\0' &&
            v->job_state == ATLAS_ORCH_STATE_QUEUED) {
            queued[nq++] = v;
        }
    }
    sort_by_key(queued, nq);
    for (size_t i = 0; s == ATLAS_OK && i < nq; i++) {
        say(o, "carrying workspace task %s (%s) here", queued[i]->task_key, queued[i]->job_uid);
        s = drive_ws(o, queued[i]->job_uid, err);
    }
    return s;
}

/* --- the PLANNING and NEEDS_REPLAN arm --------------------------------------- */

/* Asks for one more plan. `k` is derived from the correlations the daemon can
 * see, so a resumed driver asks for the same one rather than a further one. */
static atlas_status submit_planner(const atlas_plandriver_opts *o,
                                   const atlas_plandriver_plan *plan, int k, const atlas_buf *task,
                                   atlas_plandriver_report *rep, atlas_err *err) {
    atlas_buf corr = ATLAS_BUF_INIT;
    char job[ATLAS_ORCH_UID_MAX];
    memset(job, 0, sizeof job);
    atlas_status s = atlas_plan_correlation_planner(plan->plan_uid, k, &corr, err);
    if (s == ATLAS_OK) {
        atlas_plan_job_req req;
        memset(&req, 0, sizeof(req));
        req.repo = plan->repo;
        req.driver = o->planner_driver != NULL && o->planner_driver[0] != '\0' ? o->planner_driver
                                                                              : "claude-plan";
        req.mode = o->mode;
        req.task = atlas_buf_cstr(task);
        req.correlation = atlas_buf_cstr(&corr);
        req.parent_job_uid = NULL;
        /* A planner job declares no gate and asks for no parallelism: it is a
         * workspace job that writes one artifact, and it settles nothing. */
        req.validations = "";
        req.max_attempts = 1;
        req.wall_timeout_ms = o->wall_timeout_ms;
        req.idle_timeout_ms = o->idle_timeout_ms;
        req.run_max_parallel = 0;
        s = o->transport.job_submit(o->transport.ctx, &req, job, err);
        if (s != ATLAS_OK && is_busy(err)) {
            rep->busy = true;
            atlas_err_init(err);
            s = ATLAS_OK;
            job[0] = '\0';
        }
    }
    if (s == ATLAS_OK && job[0] != '\0') {
        say(o, "planner job %d submitted as %s", k, job);
        s = drive_ws(o, job, err);
    }
    atlas_buf_free(&corr);
    return s;
}

static atlas_status step_planning(const atlas_plandriver_opts *o,
                                  const atlas_plandriver_plan *plan, const atlas_plan_state *st,
                                  atlas_plandriver_report *rep, atlas_err *err) {
    /* A planner job that has not finished is carried, never replaced. */
    if (st->planner_job_uid[0] != '\0' && !atlas_orch_state_is_terminal(st->planner_job_state)) {
        say(o, "planner job %s is %s; carrying it here", st->planner_job_uid,
            atlas_orch_state_name(st->planner_job_state));
        return drive_ws(o, st->planner_job_uid, err);
    }

    atlas_plan_refusal ref;
    memset(&ref, 0, sizeof ref);

    /* A planner job that SUCCEEDED and that no revision names. Its artifact is
     * already stored; ingesting it is deterministic over those bytes, which is
     * what makes this safe to re-run on every resume and what makes the refusal
     * below reproducible rather than remembered. */
    if (st->status == ATLAS_PLAN_STATUS_PLANNING && st->replan_wanted &&
        st->planner_job_state == ATLAS_ORCH_STATE_SUCCEEDED) {
        const char *reason = st->rev_no == 0 ? "INITIAL" : "REPLAN";
        int tries = 0;
        atlas_status is;
        do {
            is = o->transport.plan_revision_add(o->transport.ctx, plan->plan_uid,
                                                st->planner_job_uid, st->rev_no + 1, reason, &ref,
                                                err);
        } while (is != ATLAS_OK && xport_again(o, "the revision", &tries, err));
        if (is == ATLAS_OK) {
            say(o, "revision %d compiled from planner job %s", st->rev_no + 1,
                st->planner_job_uid);
            return ATLAS_OK;
        }
        if (is_busy(err)) {
            rep->busy = true;
            atlas_err_init(err);
            return ATLAS_OK;
        }
        if (ref.sentence[0] == '\0') {
            /* Not the document's fault: the plan does not exist, the job is not
             * a planner job, the artifact is missing. A caller's problem, and
             * asking a planner to try again would answer the wrong question. */
            return is;
        }
        say(o, "the planner's document was refused at line %d: %s", ref.line, ref.sentence);
        atlas_err_init(err);
        /* Fall through, and quote Atlas' own refusal back to the next planner. */
    }

    if (st->planner_jobs_seen >= ATLAS_PLAN_MAX_PLANNER_JOBS) {
        say(o, "this plan has spent its %d planner jobs", ATLAS_PLAN_MAX_PLANNER_JOBS);
        return ATLAS_OK;
    }
    const int k = st->planner_jobs_seen + 1;

    const char *goal = atlas_buf_cstr(&plan->goal);
    const char *floor = atlas_buf_cstr(&plan->gate_floor_text);
    atlas_buf task = ATLAS_BUF_INIT;
    atlas_plan_job_view jv;
    atlas_plan_job_view_init(&jv);
    atlas_status s = ATLAS_OK;

    if (ref.sentence[0] != '\0') {
        /* The refusal travels as a sentence and a line, and the two are rendered
         * here rather than read back out of Atlas' prose. The refused bytes are
         * not quoted: they are the planner job's stored artifact, which no read
         * on this path serves, and the composer states the sentence without
         * them. */
        char line[320];
        (void)snprintf(line, sizeof line, "line %d: %s", ref.line, ref.sentence);
        s = atlas_plan_compose_planner_retry(goal, floor, plan->max_parallel, line, NULL, 0u,
                                             &task, err);
    } else if (st->rev_no >= 1) {
        const atlas_plan_task_view *bad = blocking_task(st);
        if (bad == NULL) {
            /* Nothing this plan holds names a failure, so there is nothing
             * honest to tell a planner about one. Ask for a plan rather than
             * inventing a blocked task. */
            say(o, "a replan is wanted and no task names a failure; asking for a plan");
            s = atlas_plan_compose_planner(goal, floor, plan->max_parallel, &task, err);
        } else {
            int tries = 0;
            atlas_status js;
            do {
                js = o->transport.job_get(o->transport.ctx, bad->job_uid, &jv, err);
            } while (js != ATLAS_OK && xport_again(o, "the blocked task", &tries, err));
            if (js != ATLAS_OK) {
                /* Evidence Atlas could not read is evidence it does not state.
                 * The replan still happens, with the gate unnamed. */
                say(o, "the blocked task could not be read (%s); the replan names no gate",
                    atlas_err_msg(err));
                atlas_err_init(err);
            }
            say(o, "asking for a replan; blocked task %s", bad->task_key);
            s = atlas_plan_compose_replan(goal, floor, plan->max_parallel, st, bad->task_key,
                                          jv.failed_gate, jv.gate_output.data, jv.gate_output.len,
                                          &task, err);
        }
    } else {
        s = atlas_plan_compose_planner(goal, floor, plan->max_parallel, &task, err);
    }

    if (s == ATLAS_OK) {
        s = submit_planner(o, plan, k, &task, rep, err);
    }
    atlas_plan_job_view_free(&jv);
    atlas_buf_free(&task);
    return s;
}

/* --- the loop ---------------------------------------------------------------- */

static atlas_status check_opts(const atlas_plandriver_opts *o, bool *resuming, atlas_err *err) {
    const atlas_plandriver_transport *t = &o->transport;
    if (t->plan_create == NULL || t->plan_get == NULL || t->plan_state == NULL ||
        t->plan_task == NULL || t->plan_revision_add == NULL || t->job_submit == NULL ||
        t->job_get == NULL || t->drive_run == NULL || t->drive_workspace_job == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the plan driver has no transport");
    }
    *resuming = o->plan_uid != NULL && o->plan_uid[0] != '\0';
    if (*resuming) {
        /* Refused rather than ignored, which is A10.1's `--memory --resume` rule
         * exactly: a plan's goal, its gate floor and its parallelism are fixed
         * when it is created, and a flag that was quietly dropped reads exactly
         * like one that was honoured. */
        if ((o->goal != NULL && o->goal[0] != '\0') || o->gate_count > 0 || o->max_parallel > 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a resume names the plan and nothing else: the goal, the gate "
                                 "floor and the parallelism were fixed when it was created");
        }
        return ATLAS_OK;
    }
    if (o->repo == NULL || o->repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    if (o->goal == NULL || o->goal[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan needs a goal");
    }
    if (o->gate_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan needs at least one gate; the operator brings the gate floor "
                             "and the planner may only add to it");
    }
    return ATLAS_OK;
}

atlas_status atlas_plandriver_run(const atlas_plandriver_opts *o, atlas_plandriver_report *rep,
                                  atlas_err *err) {
    bool resuming = false;
    atlas_status st = check_opts(o, &resuming, err);
    if (st != ATLAS_OK) {
        return st;
    }

    char uid[ATLAS_ORCH_UID_MAX];
    memset(uid, 0, sizeof uid);
    if (resuming) {
        (void)snprintf(uid, sizeof uid, "%s", o->plan_uid);
    } else {
        atlas_plan_create_req req;
        memset(&req, 0, sizeof(req));
        req.repo = o->repo;
        req.goal = o->goal;
        req.max_parallel = o->max_parallel;
        req.gates = o->gate_floor;
        req.gate_count = o->gate_count;
        int tries = 0;
        do {
            st = o->transport.plan_create(o->transport.ctx, &req, uid, err);
        } while (st != ATLAS_OK && xport_again(o, "the plan", &tries, err));
        if (st != ATLAS_OK && is_busy(err)) {
            /* Nothing was queued and nothing will run. The invocation is over
             * and may simply be repeated; no plan exists to resume. */
            rep->busy = true;
            atlas_err_init(err);
            return ATLAS_OK;
        }
        if (st != ATLAS_OK) {
            return st;
        }
        if (uid[0] == '\0') {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the Atlas daemon created a plan and did not name it");
        }
        say(o, "plan %s created", uid);
    }
    st = atlas_buf_set_str(&rep->plan_uid, uid, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_plandriver_plan plan;
    atlas_plandriver_plan_init(&plan);
    atlas_buf mark = ATLAS_BUF_INIT, prev = ATLAS_BUF_INIT;
    st = read_plan(o, uid, &plan, err);

    const int64_t ceiling = o->max_iterations > 0 ? o->max_iterations : PLAN_ITERATION_CEILING;
    bool settled = false;
    for (int64_t i = 0; st == ATLAS_OK && !settled && i < ceiling; i++) {
        atlas_plan_state state;
        st = read_state(o, uid, &state, err);
        if (st != ATLAS_OK) {
            break;
        }
        rep->status = state.status;
        rep->rev_no = state.rev_no;
        rep->stages_accepted = state.stages_accepted;
        rep->planner_jobs = state.planner_jobs_seen;

        st = fingerprint(&state, &mark, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (i > 0 && mark.len == prev.len &&
            (mark.len == 0 || memcmp(mark.data, prev.data, mark.len) == 0)) {
            /* The previous iteration moved nothing. Whatever remains belongs to
             * a dispatcher this process is not, or to a driver that already
             * holds the task; going round again would be this process pretending
             * to make progress. */
            say(o, "nothing this process can do would move plan %s; it is %s and resumable", uid,
                atlas_plan_status_name(state.status));
            break;
        }
        st = atlas_buf_set(&prev, mark.data, mark.len, err);
        if (st != ATLAS_OK) {
            break;
        }

        switch (state.status) {
        case ATLAS_PLAN_STATUS_COMPLETED:
        case ATLAS_PLAN_STATUS_BLOCKED:
            /* An answer. Touched not at all: no job is created, no worker is
             * started, and saying so is the whole of what happens here. */
            say(o, "plan %s is %s", uid, atlas_plan_status_name(state.status));
            settled = true;
            break;
        case ATLAS_PLAN_STATUS_PLANNING:
        case ATLAS_PLAN_STATUS_NEEDS_REPLAN:
            st = step_planning(o, &plan, &state, rep, err);
            break;
        case ATLAS_PLAN_STATUS_EXECUTING:
            st = step_executing(o, &plan, &state, rep, err);
            break;
        case ATLAS_PLAN_STATUS_UNKNOWN:
            /* Never stored, never parsed, and never derived for a plan that
             * exists. Reaching it means the derivation is wrong, which is a
             * defect in Atlas rather than an answer about this plan. */
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "plan %s derived no status", uid);
            break;
        }
        if (st == ATLAS_OK && rep->busy) {
            say(o, "the daemon is busy and took nothing; plan %s is untouched and resumable", uid);
            break;
        }
    }

    atlas_buf_free(&mark);
    atlas_buf_free(&prev);
    atlas_plandriver_plan_free(&plan);
    return st;
}

/* Atlas - A12.0: the operator's foreground plan driver.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What this is
 *
 * One loop, started deliberately by an operator, that turns a goal into a
 * planner job, ingests the revision that job produced, walks the revision's
 * stages as **ordinary runs**, and answers a stage-run that settled BLOCKED with
 * one bounded plan revision. It is A11.1's run driver one layer up, and it holds
 * exactly the same shape: a foreground process an operator started, no thread,
 * no timer, no polling, no scheduler, no second submit path.
 *
 * ## What it does not do
 *
 * It decides nothing. Every stage is submitted through the one submit path and
 * runs under the existing refusals, budgets, leases, gates and settlement; a
 * stage's verdict is the *run's* status, which the daemon derived from a
 * completion Atlas classified. The plan's own status is derived on every read by
 * `atlas_db_plan_state_derive` from stored rows, and this loop reads it rather
 * than keeping one: there is no plan status to write, no CAS to win and no
 * method to call, so "a model payload cannot declare a plan complete" stays true
 * because the verb does not exist.
 *
 * **The replan trigger is Atlas' own verdict and never a sentence a worker
 * wrote.** A stage-run that settled BLOCKED is what asks for a new plan — not a
 * blocker artifact, not a line in a worker's log, not a claim in a planner's
 * document.
 *
 * ## Why every step is idempotent
 *
 * The loop keeps **no state that must survive a crash**. Everything it acts on
 * is re-derived from `plan_state` at the top of each iteration, and every
 * submission carries an idempotency key built by
 * `atlas_plan_correlation_planner` / `atlas_plan_correlation_task` — the same
 * string as the correlation, which is what makes the plan-to-jobs mapping
 * derivable without a bind call, a column or an update. A driver killed at any
 * point and started again re-issues the same submissions and is handed the same
 * jobs back.
 *
 * The one apparent exception is the refusal a planner's document earned, and it
 * is not one: the refusal is obtained inside the same iteration that answers it,
 * by re-running `plan_revision_add` against the same stored bytes, which is
 * deterministic and produces the same sentence and the same line every time.
 */
#ifndef ATLAS_PLANDRIVER_H
#define ATLAS_PLANDRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/orch.h"
#include "atlas/plan.h"

/* --- what a document refusal is, as a caller receives it --------------------
 *
 * Two values rather than one sentence, because the driver renders them back to a
 * planner as `line %d: %s` and folding the number into Atlas' prose would make
 * the driver parse that prose to recover it. `sentence[0] == '\0'` is the
 * discriminator: every refusal that came from the *document* fills it, and every
 * other refusal — the plan does not exist, the job is not a planner job, the
 * artifact is missing — leaves it empty, exactly as `atlas_plan_result` does. */
typedef struct atlas_plan_refusal {
    char sentence[256];
    /* The 1-based line the refusal is about, or 0 for a refusal about the
     * document as a whole. Zero is also what an unfilled struct carries, which
     * is why `sentence` and not this is the discriminator. */
    int line;
} atlas_plan_refusal;

/* --- what the loop asks the transport to create ----------------------------- */

/* An operator's plan. The goal and the gate floor are the operator's own words
 * and the operator's own commands; nothing here comes from a model. */
typedef struct atlas_plan_create_req {
    const char *repo;
    const char *goal;
    /* 1..ATLAS_ORCH_RUN_MAX_PARALLEL, or 0 for "not stated", which the daemon
     * resolves to `ATLAS_PLAN_DEFAULT_PARALLEL`. Refused rather than clamped. */
    int max_parallel;
    /* The floor, one command line each, split by `atlas_orch_gate_split` at the
     * transport. At least one: a plan with no operator gate could only ever be
     * accepted on a model's word. */
    const char *const *gates;
    size_t gate_count;
} atlas_plan_create_req;

/* One job the plan layer submits. Every member is either the operator's, Atlas'
 * own, or a bounded value read back out of a stored row — there is no member a
 * planner can set that changes who may run this or what will judge it. */
typedef struct atlas_plan_job_req {
    const char *repo;
    /* From the driver's own vocabulary, and still subject to the root-owned
     * policy at the write point: naming one here narrows, never permits. */
    const char *driver;
    /* NULL leaves the daemon to pick the policy's mode, exactly as an absent
     * `--mode` does on `job submit`. */
    const char *mode;
    /* The composed prompt, raw bytes. Delivered to a worker as-is with its
     * provenance stated inside it, which is the existing lease contract. */
    const char *task;
    /* The one string that is *both* the correlation and the idempotency key,
     * from `atlas_plan_correlation_planner` / `_task` and from nowhere else.
     * Two spellings of one format are two answers to "is this job this plan's",
     * so this layer never formats one by hand. */
    const char *correlation;
    /* Empty or NULL for a run root; the stage's tree job for a sibling, which is
     * how a side task joins the stage's run. */
    const char *parent_job_uid;
    /* The **merged** validations for a repo-tree stage task — the operator's
     * floor verbatim and first, then the planner's additions — exactly as
     * `orch_plan_tasks.validations` stores them, in the canonical netstring
     * encoding. Carried opaquely and never re-merged here: the merge and its
     * bound are the plan write point's, applied once when the revision compiled,
     * and a second implementation of it in a driver would be a second answer to
     * what an accepted stage was gated on. Empty for a planner job and for a
     * workspace sibling, neither of which declares a gate. */
    const char *validations;
    int64_t max_attempts;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    /* Root submissions only; 0 means "not stated". Naming it on a child is
     * refused at the write point, not ignored. */
    int64_t run_max_parallel;
} atlas_plan_job_req;

/* What the loop needs to know about one job it submitted.
 *
 * `failed_gate` and `gate_output` are best-effort evidence for a replan prompt
 * and are allowed to be empty: `atlas_plan_compose_replan` states "(none
 * recorded)" rather than naming a gate nobody established, and omits the excerpt
 * section entirely when there is nothing to excerpt. A transport that cannot
 * reach them leaves them empty rather than guessing. */
typedef struct atlas_plan_job_view {
    atlas_orch_state state;
    char run_uid[ATLAS_ORCH_RUN_UID_MAX];
    atlas_orch_run_status run_status;
    /* The failing gate rendered from the job's own stored validations, or empty.
     * Atlas' own value: an argv Atlas holds, never a name a caller supplied. */
    char failed_gate[256];
    /* UNTRUSTED_DATA. A compiler's or a test runner's bytes, bounded by whoever
     * filled this in and bounded again by the composer. Owned. */
    atlas_buf gate_output;
} atlas_plan_job_view;

void atlas_plan_job_view_init(atlas_plan_job_view *v);
void atlas_plan_job_view_free(atlas_plan_job_view *v);

/* One task of one revision, as `orch_plan_tasks` stores it.
 *
 * Read back rather than re-parsed from the document. The task row is where the
 * merged gate list exists at all — the parser cannot produce one, because it is
 * pure and has never seen the operator's floor — and reading the prompt from the
 * same row keeps one source for the whole task instead of two that could
 * disagree about which revision's words a worker was shown. */
typedef struct atlas_plandriver_task {
    char task_key[33];
    int stage_no;
    bool is_tree;
    /* UNTRUSTED_DATA, both of these: a model wrote them. */
    char title[ATLAS_PLAN_TITLE_MAX + 1];
    atlas_buf prompt;
    /* The merged list for a tree task, empty for a side task. See
     * `atlas_plan_job_req.validations`. */
    atlas_buf validations;
} atlas_plandriver_task;

void atlas_plandriver_task_init(atlas_plandriver_task *t);
void atlas_plandriver_task_free(atlas_plandriver_task *t);

/* The plan row itself: what the composers need and what a resumed invocation
 * cannot have been told.
 *
 * `max_parallel` is read here and used for **both** the bound a planner is shown
 * and the bound a job is submitted under, because the daemon parses the
 * planner's answer with this same number. A driver that composed against one
 * value and let the daemon parse against another could tell a planner it may
 * write three side tasks and then refuse it at two. */
typedef struct atlas_plandriver_plan {
    char plan_uid[ATLAS_ORCH_RUN_UID_MAX];
    char repo[ATLAS_ORCH_NAME_MAX + 1u];
    int max_parallel;
    /* The operator's own words. Owned. */
    atlas_buf goal;
    /* The floor as one block, one command per line, arguments joined by a single
     * space — what the composers show a planner as the list it may add to and
     * may not replace. Owned. */
    atlas_buf gate_floor_text;
} atlas_plandriver_plan;

void atlas_plandriver_plan_init(atlas_plandriver_plan *p);
void atlas_plandriver_plan_free(atlas_plandriver_plan *p);

/* --- how the driver reaches the daemon --------------------------------------
 *
 * The same seam `atlas_rundriver_transport` is, for the same reason: in the
 * shipped binary every one of these is a socket round trip, because plan and
 * orchestration state live in the index and `atlasd` is its only writer — a
 * driver that opened the database itself would be a second writer, which A1
 * forbids and A7.1 makes impossible. A test hosts the same loop against a
 * fixture, which is what lets the loop's contracts be proved without a daemon, a
 * model or a network.
 *
 * It loosens no production check. What the IPC edge refuses — the peer uid, the
 * orchestration policy, the method group, every submit refusal and every binding
 * check on a revision — it still refuses. What this abstracts is the *carriage*
 * of a call, never its validation.
 *
 * **Every out-parameter is caller-initialised and is *set*, not appended to.**
 * The driver calls the matching `_init` before the call and the matching `_free`
 * after it, so an implementation writes into live buffers and owns none of them;
 * an implementation that fails part-way leaves whatever it wrote, which the
 * driver's `_free` still releases.
 *
 * **Every string a transport fills in is raw.** The daemon safe-encodes a goal,
 * a title, a prompt and the gate floor block on the wire, and the production
 * implementation performs that single `atlas-safe-1` decode as it fills these
 * structs — here and nowhere else. The loop therefore never decodes and never
 * re-encodes, and a fixture that reads rows directly has nothing to decode and
 * hands over the same bytes. A driver-side decode would corrupt a goal
 * containing a literal `%` the moment a transport delivered raw bytes.
 */
typedef struct atlas_plandriver_transport {
    void *ctx;

    /* Creates the plan. Refuses a repository whose durable identity Atlas does
     * not hold — a registered repository whose history has never been ingested
     * has no fingerprint, and a plan carries one. */
    atlas_status (*plan_create)(void *ctx, const atlas_plan_create_req *req,
                                char plan_uid_out[ATLAS_ORCH_UID_MAX], atlas_err *err);
    /* The plan row: goal, floor, parallelism, repository. */
    atlas_status (*plan_get)(void *ctx, const char *plan_uid, atlas_plandriver_plan *out,
                             atlas_err *err);
    /* The derived state. The one implementation of "what is this plan doing?",
     * asked afresh at the top of every iteration. */
    atlas_status (*plan_state)(void *ctx, const char *plan_uid, atlas_plan_state *out,
                               atlas_err *err);
    /* One task row of one revision, by key. */
    atlas_status (*plan_task)(void *ctx, const char *plan_uid, int rev_no, const char *task_key,
                              atlas_plandriver_task *out, atlas_err *err);
    /* Turns a planner job's own stored artifact into a revision. A document
     * refusal fills `ref` and returns non-OK; every other refusal leaves `ref`
     * as the caller left it. */
    atlas_status (*plan_revision_add)(void *ctx, const char *plan_uid, const char *planner_job,
                                      int rev_no, const char *reason, atlas_plan_refusal *ref,
                                      atlas_err *err);
    atlas_status (*job_submit)(void *ctx, const atlas_plan_job_req *req,
                               char job_uid_out[ATLAS_ORCH_UID_MAX], atlas_err *err);
    atlas_status (*job_get)(void *ctx, const char *job_uid, atlas_plan_job_view *out,
                            atlas_err *err);
    /* A11.1's run driver, on one stage's run. It claims the run's active
     * repo-tree task, starts one worker in the registered repository's own tree,
     * runs the gates and reports; the daemon settles. */
    atlas_status (*drive_run)(void *ctx, const char *run_uid, atlas_err *err);
    /* One named workspace job's attempt, in this process, once.
     *
     * A job whose driver partition does not belong to this process is a **clean
     * refusal**, not an error: the lease is simply not granted, and the loop
     * treats that as "an operator's dispatcher will take this" rather than as a
     * failure of the plan. That is A11.6's stated architecture unchanged — a
     * single process gives a sequential progress guarantee, and genuine overlap
     * comes from dispatcher processes an operator started. */
    atlas_status (*drive_workspace_job)(void *ctx, const char *job_uid, atlas_err *err);
} atlas_plandriver_transport;

typedef struct atlas_plandriver_opts {
    atlas_plandriver_transport transport;

    /* The plan to carry on, or empty to create one. A resume names the plan and
     * nothing else: naming a goal, a gate or a parallelism beside it is refused
     * rather than ignored, which is A10.1's `--memory --resume` rule — a flag
     * that was quietly dropped reads exactly like one that was honoured. */
    const char *plan_uid;

    /* Creation only. */
    const char *repo;
    const char *goal;
    const char *const *gate_floor;
    size_t gate_count;
    int max_parallel;

    /* Which driver each of the three kinds of job runs under. Empty takes the
     * production default beside it. The *policy* still authorises the name: a
     * driver named here is a narrowing and never a permission. */
    const char *planner_driver; /* default "claude-plan" */
    const char *tree_driver;    /* default "claude-repo" */
    const char *side_driver;    /* default "claude" */
    /* NULL leaves the daemon to pick the policy's mode. */
    const char *mode;
    /* The ceilings a submitted job is given, or zero for the daemon's own. */
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;

    /* A defect guard above the season's stated worst case, not a budget: the
     * real bounds are the planner-job ceiling, the revision ceiling and each
     * run's own worker-start budget, and the daemon enforces all three. Zero
     * takes the compiled default. */
    int64_t max_iterations;
    /* How long to wait between two attempts at a call whose answer never
     * arrived, or zero for `ATLAS_RUN_XPORT_PAUSE_MS`. The number of attempts is
     * compiled in and is not settable: a bound a caller can widen is not a
     * bound. */
    int64_t xport_pause_ms;

    FILE *log;
} atlas_plandriver_opts;

typedef struct atlas_plandriver_report {
    atlas_buf plan_uid;
    /* The plan's derived status when the loop stopped. Never written anywhere. */
    atlas_plan_status status;
    /* The latest compiled revision, or 0 when none has compiled. */
    int rev_no;
    /* Stage-runs of that revision that settled ACCEPTED. */
    int stages_accepted;
    /* Planner jobs this plan has, derived from correlations. */
    int planner_jobs;
    /* The daemon was busy and took nothing.
     *
     * Neither an acceptance nor a refusal, and emphatically not BLOCKED: nothing
     * was written, the plan is untouched and resumable, and the same invocation
     * may simply be repeated. A11.1's contract, one layer up. */
    bool busy;
} atlas_plandriver_report;

void atlas_plandriver_report_init(atlas_plandriver_report *r);
void atlas_plandriver_report_free(atlas_plandriver_report *r);

/* Drives the plan until it is COMPLETED or BLOCKED, until nothing this process
 * can do would move it, or until the defect guard is reached.
 *
 * Returns non-OK only for a failure of Atlas itself. A plan that ended BLOCKED
 * is an *answer* and is reported in `rep->status`, exactly as a BLOCKED run is. */
atlas_status atlas_plandriver_run(const atlas_plandriver_opts *o, atlas_plandriver_report *rep,
                                  atlas_err *err);

#endif /* ATLAS_PLANDRIVER_H */

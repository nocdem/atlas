/* Atlas - A12.0: the planned run — the `atlas-plan-1` document and its prompts.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A plan is a **proposal**, never a verdict. A planner-role worker writes one
 * artifact, `plan.atlas-plan`, in the line-based `atlas-plan-1` format below;
 * Atlas parses it here, refuses it here, and compiles what survives into
 * ordinary jobs that run under the existing submit refusals, budgets, leases,
 * gates and settlement. Compiling a plan grants nothing. Nothing in this header
 * accepts a run, approves a decision or relaxes a gate, and there is no field a
 * planner can set that would.
 *
 * Everything a planner wrote is UNTRUSTED_DATA at every point: the task keys,
 * the titles and the prompts are a model's bytes. They are bounded, checked for
 * shape, and never interpreted as an instruction to Atlas.
 *
 * **The operator brings the goal and the gate floor; the planner may only add
 * gates.** A `gate:` line contributes an *addition*, appended after the
 * operator's floor and never in place of it; a gate program outside the
 * binary's own allowlist is refused. That allowlist has exactly one
 * implementation — `atlas_validation_program_allowed` in `atlas/validate.h` —
 * and this layer calls it rather than carrying a second copy, because two
 * copies of an allowlist are two places for one of them to grow.
 *
 * `src/orch/plan.c` is **pure**: no database handle, no process, no file, no
 * clock. That is what makes a refusal checkable — the same bytes and the same
 * `max_parallel` give the same refusal and the same line number on every
 * machine, so a test can assert a sentence and a driver can quote one back to a
 * planner as a retry prompt.
 */
#ifndef ATLAS_PLAN_H
#define ATLAS_PLAN_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/orch.h"

/* --- bounds ---------------------------------------------------------------
 *
 * Every one of these is a compiled-in ceiling, not a default and not a
 * suggestion. A plan that exceeds one is refused with the bound named; nothing
 * here is clamped, because a clamp turns "you asked for too much" into "you got
 * something other than what you asked for" and nobody reads the difference.
 *
 * The numeric values are restated inside the format specification the planner
 * prompt carries (`src/orch/plan.c`), because a model reads a number and cannot
 * read a macro. `_Static_assert`s in that file pin the two together, so moving
 * a bound here fails the build until the sentence the planner is shown moves
 * with it.
 */

/* The whole artifact. 64 KiB is far more than a four-stage plan needs and far
 * less than a model can be induced to emit. */
#define ATLAS_PLAN_MAX_BYTES 65536

/* Tasks in one plan, across every stage. */
#define ATLAS_PLAN_MAX_TASKS 8

/* Stages. Each stage becomes one ordinary run: a repo-tree root task plus its
 * workspace siblings. */
#define ATLAS_PLAN_MAX_STAGES 4

/* Workspace siblings beside one stage's tree task. Bounded again, per plan, by
 * `max_parallel - 1`: the run's own parallelism ceiling already says how much
 * may be in flight, and a plan may not talk its way past it. */
#define ATLAS_PLAN_MAX_SIDE_PER_STAGE 3

/* One task's prompt, the free text handed to the executor. */
#define ATLAS_PLAN_TASK_PROMPT_MAX 16384

/* One task's title: one line, for a human reading `plan show`. */
#define ATLAS_PLAN_TITLE_MAX 200

/* Compiled revisions of one plan. A replan is bounded for the same reason a
 * worker start is: the ceiling exists so nobody discovers it in a bill. */
#define ATLAS_PLAN_MAX_REVISIONS 3

/* Planner jobs across the whole plan — initial, parse retries and replans
 * together. */
#define ATLAS_PLAN_MAX_PLANNER_JOBS 5

/* The operator's goal text. */
#define ATLAS_PLAN_GOAL_MAX 16384

/* One document line. Bounded so a single line cannot be the whole budget, and
 * checked against the bytes between newlines *before* a trailing carriage
 * return is stripped: the bound is on what the document contains. */
#define ATLAS_PLAN_MAX_LINE 4096

/* The artifact the planner must write, and the only name this layer reads. */
#define ATLAS_PLAN_ARTIFACT_NAME "plan.atlas-plan"

/* --- the plan's derived status --------------------------------------------
 *
 * Derived on every read from stored rows and written nowhere. There is no
 * status column, no `plan.settle`, no CAS to reach and therefore nothing for a
 * model payload to move — A11.0's authority-by-absence, one layer up.
 */
typedef enum atlas_plan_status {
    /* Zero. Never stored, never parsed, never a claim. A zeroed state struct
     * reads UNKNOWN, which is what "nobody derived this" should look like. */
    ATLAS_PLAN_STATUS_UNKNOWN = 0,
    /* No usable revision yet, and planner budget remains. */
    ATLAS_PLAN_STATUS_PLANNING,
    /* The latest revision has work that is not terminal. */
    ATLAS_PLAN_STATUS_EXECUTING,
    /* A stage-run of the latest revision settled BLOCKED, or a planner artifact
     * was refused, and budgets remain. The driver acts on this. */
    ATLAS_PLAN_STATUS_NEEDS_REPLAN,
    /* Every stage-run of the latest revision ACCEPTED and every side job of it
     * SUCCEEDED. */
    ATLAS_PLAN_STATUS_COMPLETED,
    /* A budget is exhausted, or the planner's last job failed terminally with
     * no budget left. */
    ATLAS_PLAN_STATUS_BLOCKED
} atlas_plan_status;

/* The name for a status. UNKNOWN answers "UNKNOWN"; the switch has no
 * `default:`, so adding a member is a build failure here rather than a silent
 * "UNKNOWN" on a surface somebody trusts. */
const char *atlas_plan_status_name(atlas_plan_status s);

/* --- the parsed document ---------------------------------------------------
 *
 * What a planner proposed, after every bound and every shape check. Not yet
 * compiled: the operator's gate floor is prepended per tree task at the write
 * point, which is where the merged list is bounded again.
 */
typedef struct atlas_plan_doc_task {
    /* `[a-z0-9-]{1,32}`, unique across the whole plan. Deliberately narrow: a
     * key travels into an idempotency key and a correlation string, and a key
     * that could contain a colon would be a key that could impersonate a
     * different plan's job. */
    char key[33];
    int stage_no;
    /* True for the stage's single repository-tree task; false for a workspace
     * sibling. Exactly one tree task per stage. */
    bool is_tree;
    char title[ATLAS_PLAN_TITLE_MAX + 1];
    /* UNTRUSTED_DATA, owned, released by `atlas_plan_doc_free`. Arbitrary bytes
     * — a prompt is not required to be UTF-8 and is never parsed as anything. */
    atlas_buf prompt;
    /* The planner's *additions* only. The operator's floor is not here and is
     * never merged in this layer: a structure that held both would be a
     * structure in which the difference between them could be lost. */
    atlas_orch_argv gates[ATLAS_ORCH_MAX_VALIDATIONS];
    size_t gate_count;
} atlas_plan_doc_task;

typedef struct atlas_plan_doc {
    int stage_count;
    size_t task_count;
    atlas_plan_doc_task tasks[ATLAS_PLAN_MAX_TASKS];
} atlas_plan_doc;

/* Releases everything a parsed document owns and leaves it zeroed. Idempotent,
 * and safe on a document that was never filled: a zeroed `atlas_plan_doc` holds
 * only zeroed `atlas_buf`s, which is exactly `ATLAS_BUF_INIT`. */
void atlas_plan_doc_free(atlas_plan_doc *d);

/* Parses and validates `len` untrusted bytes.
 *
 * On refusal: ATLAS_ERR_USAGE with a sentence naming what and where, and — when
 * `line_out` is not NULL — the 1-based line number the refusal is about, or 0
 * for a refusal about the document as a whole. `*line_out` is set on every
 * refusal path, so a caller never has to distinguish "line zero" from "nobody
 * said".
 *
 * `max_parallel` is the plan's own parallelism ceiling and bounds side tasks per
 * stage at `min(ATLAS_PLAN_MAX_SIDE_PER_STAGE, max_parallel - 1)`. A value below
 * one is refused rather than resolved to a default: this function has no
 * defaults, because a pure function that invents one is a pure function whose
 * answer depends on something the caller cannot see.
 *
 * **Never partially fills.** `out` is written once, at the end, from a document
 * that passed every check; a refused parse leaves it exactly as the caller left
 * it. A caller that initialised `out` to zero and then read it after a refusal
 * sees no tasks rather than half a plan. */
atlas_status atlas_plan_parse(const void *bytes, size_t len, int max_parallel,
                              atlas_plan_doc *out, int *line_out, atlas_err *err);

/* --- the derived state -----------------------------------------------------
 *
 * Filled by `atlas_db_plan_state_derive` (see the DB section below) and read by
 * `plan.get`, the plan driver's loop and the replan composer. Declared here
 * rather than beside the reader because it is the shape three layers agree on,
 * and above the composers because one of them takes it.
 */
typedef struct atlas_plan_task_view {
    char task_key[33];
    int stage_no;
    bool is_tree;
    /* Empty uid / UNKNOWN state = this task has not been submitted yet. */
    char job_uid[36];
    atlas_orch_state job_state;
    /* Tree tasks only: the run the task's stage became. */
    char run_uid[36];
    atlas_orch_run_status run_status;
    /* A12.0 ruling 3. The planner's title, so a replan prompt can render the
     * completed-work section from the state alone rather than re-reading the
     * revision's task rows. UNTRUSTED_DATA; excerpted and labelled wherever it
     * is shown. */
    char title[ATLAS_PLAN_TITLE_MAX + 1];
} atlas_plan_task_view;

typedef struct atlas_plan_state {
    atlas_plan_status status;
    int rev_no;               /* 0 = none compiled yet */
    int planner_jobs_seen;    /* k count derived from correlations */
    char planner_job_uid[36]; /* latest planner job, if any */
    atlas_orch_state planner_job_state;
    int task_count;
    atlas_plan_task_view tasks[ATLAS_PLAN_MAX_TASKS];
    int stages_accepted; /* runs of the latest revision that are ACCEPTED */
    /* A BLOCKED stage-run, or a refused planner artifact, with budget left.
     *
     * The refused-artifact half has no row of its own: a refused parse aborts
     * the transaction, so *no* revision row exists. It is derived as "the latest
     * planner job SUCCEEDED and no matching revision exists". The driver either
     * still holds the refusal it was handed synchronously, or re-runs
     * `plan.revision_add` on resume and obtains the same deterministic refusal
     * from the same stored bytes. */
    bool replan_wanted;
} atlas_plan_state;

/* --- the prompt composers --------------------------------------------------
 *
 * Deterministic, byte-exact and tested against goldens. Composed the way
 * `follow_up_text` is composed in `src/db/db_orch.c`: fixed sentences, and every
 * untrusted excerpt bounded and labelled as it is appended.
 *
 * Every composer emits the same four parts in the same order — the request
 * header with the goal, the immutable gate floor and the bounds; then whatever
 * this particular situation adds; then the required output and the frozen
 * format specification; then the constraints block. The constraints block is
 * last in all five forms, because it is the boundary of what is being asked for
 * and a boundary a later paragraph can revise is not one.
 *
 * The constraints say, in every form: this is instruction and not enforcement;
 * Atlas runs the gates itself and settles the run itself; the worker's output
 * grants nothing. All three are true because of what is absent elsewhere, not
 * because of these sentences — which is why they can be stated to a model at
 * all.
 *
 * `out` is caller-initialised and is *set*, not appended to. `gate_floor_text`
 * is the human-readable floor, one command per line, rendered by the service
 * layer from the stored encoding: composers never decode netstrings, so no
 * stored form has a second reader here.
 */

/* The initial request: goal, floor, bounds, the required artifact and the
 * format. Refuses a missing or over-long goal and a missing gate floor — a plan
 * with no operator gate could only ever be accepted on a model's word. */
atlas_status atlas_plan_compose_planner(const char *goal, const char *gate_floor_text,
                                        int max_parallel, atlas_buf *out, atlas_err *err);

/* The same request, plus the refusal the previous artifact earned and a bounded
 * excerpt of the bytes that earned it. The refusal sentence is Atlas' own, from
 * `atlas_plan_parse`; the excerpt is the planner's bytes and is labelled. */
atlas_status atlas_plan_compose_planner_retry(const char *goal, const char *gate_floor_text,
                                              int max_parallel, const char *refusal,
                                              const void *refused, size_t refused_len,
                                              atlas_buf *out, atlas_err *err);

/* The replan request. What Atlas established — which tasks SUCCEEDED — is
 * stated as fact; the blocked task, the gate that failed and a bounded excerpt
 * of its output are stated beside it; and the instruction is a complete new
 * plan for the *remaining* work, with the completed work standing.
 *
 * The trigger is never a sentence a worker wrote. A replan happens because a
 * stage-run settled BLOCKED, which is Atlas' own verdict. */
atlas_status atlas_plan_compose_replan(const char *goal, const char *gate_floor_text,
                                       int max_parallel, const atlas_plan_state *st,
                                       const char *blocked_key, const char *failed_gate,
                                       const void *gate_excerpt, size_t excerpt_len,
                                       atlas_buf *out, atlas_err *err);

/* One task's prompt for the executor. The tree form locks scope to the
 * registered repository's own root; the side form locks it to the provided
 * workspace and says the repository is out of reach — which it is, by OS
 * authority and by the driver's own mode, not by this sentence. `t->is_tree`
 * chooses between them. */
atlas_status atlas_plan_compose_executor(const char *plan_uid, int rev_no,
                                         const atlas_plan_doc_task *t, atlas_buf *out,
                                         atlas_err *err);

/* ---------------------------------------------------------------------------
 * T3 (migration 25 and the plan tables' write point) owns everything below this
 * line: `atlas_plan_op`, `atlas_plan_result`, `atlas_plan_apply_in_tx`,
 * `atlas_plan_apply` and `atlas_db_plan_state_derive`. Nothing above it is the
 * DB layer's to change — the structs above are the format's, and
 * `src/orch/plan.c` must stay free of a database handle.
 * ------------------------------------------------------------------------ */

#endif /* ATLAS_PLAN_H */

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
     * key travels into an idempotency key and a correlation string, both of
     * which are dot-separated and both of which must satisfy `is_name` after
     * they are stored. A key holding a dot could spell a different plan's
     * revision; a key holding anything outside this set could make the whole
     * correlation fail validation on the follow-up that inherits it. */
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

/* Declared rather than included, exactly as `atlas/orch_ops.h` declares it: a
 * database handle is opaque everywhere outside `src/db`, and pulling `atlas/db.h`
 * in here would give the pure format layer a reason to see it. */
typedef struct atlas_db atlas_db;

/* --- the plan domain's one write point -------------------------------------
 *
 * `atlas_plan_apply_in_tx` is the only function that writes `orch_plans`,
 * `orch_plan_revisions` or `orch_plan_tasks`, and it is the whole of what may
 * write them. This mirrors `atlas_orch_apply_in_tx` and `atlas_decision_apply_in_tx`
 * for the reason both give: every binding check a forger would want somewhere
 * else — the correlation, the driver's role, the job's state, the artifact's
 * name and bounds, and the format itself — lives behind this one call, and a
 * second implementation reaching the tables would bypass all of them at once.
 * `tests/test_plan_db.c` asserts the count of files that write these tables.
 *
 * Two operations, and deliberately no third. **There is no operation that writes
 * a plan's status**, because no plan has one: the status is derived on every
 * read by `atlas_db_plan_state_derive` from rows nothing in this vocabulary can
 * reach. That is A11.0's authority-by-absence one layer up — there is no CAS to
 * win, no column to set and no method to call, so "a model payload cannot
 * declare a plan complete" is true because the verb does not exist.
 */
typedef enum atlas_plan_op_kind {
    /* Zero is not an operation. A zeroed op applies nothing. */
    ATLAS_PLAN_OP_NONE = 0,
    /* An operator creates a plan: the goal, the immutable gate floor and the
     * parallelism the stages will run under. Creates no job and starts nothing. */
    ATLAS_PLAN_OP_CREATE,
    /* A planner job's own stored artifact becomes a revision, if — and only if —
     * every binding check below passes. */
    ATLAS_PLAN_OP_REVISION_ADD
} atlas_plan_op_kind;

/* Why a revision exists. A closed two-member vocabulary, stored, with UNKNOWN
 * as the zero that no stored row may hold.
 *
 * There is deliberately no `PARSE_REFUSED` member. A refused parse aborts the
 * transaction and writes **no row at all**, so a reason describing one would be
 * a reason for a revision that does not exist; the refused-artifact state is
 * derived instead, from a planner job whose uid no revision names. */
typedef enum atlas_plan_revision_reason {
    ATLAS_PLAN_REVISION_UNKNOWN = 0,
    ATLAS_PLAN_REVISION_INITIAL,
    ATLAS_PLAN_REVISION_REPLAN
} atlas_plan_revision_reason;

const char *atlas_plan_revision_reason_name(atlas_plan_revision_reason r);

typedef struct atlas_plan_op {
    atlas_plan_op_kind kind;

    /* A trusted connection fact, filled from `SO_PEERCRED` at the IPC edge and
     * from nowhere else. A request body that carried a uid would be a caller
     * describing itself to a field Atlas does not have. */
    long long submitter_uid;

    /* CREATE. Resolved from the registry by the caller, never supplied by a
     * model or read out of a plan document. */
    atlas_buf repo_name;
    atlas_buf repo_identity_hash;
    /* CREATE. The operator's own words. Bounded at ATLAS_PLAN_GOAL_MAX. */
    atlas_buf goal_text;
    /* CREATE. The operator's gate floor, in the canonical netstring encoding
     * `atlas_orch_validations_encode` produces, and at least one command. A
     * plan with no operator gate could only ever be accepted on a model's word,
     * so an empty floor is a refusal rather than a default. */
    atlas_buf gate_floor;
    /* CREATE. 1..ATLAS_ORCH_RUN_MAX_PARALLEL; 0 means "not stated" and resolves
     * to ATLAS_PLAN_DEFAULT_PARALLEL. Anything else is refused, never clamped. */
    int max_parallel;

    /* REVISION_ADD. */
    atlas_buf plan_uid;
    atlas_buf planner_job_uid;
    atlas_plan_revision_reason reason;
    /* The revision number the caller believes this will be. Compared against the
     * plan's stored maximum plus one inside the transaction, for the reason every
     * check here is inside it: a number that was right when it was read and wrong
     * when it was written is not a check. */
    int rev_no;
} atlas_plan_op;

void atlas_plan_op_init(atlas_plan_op *op, atlas_plan_op_kind kind);
void atlas_plan_op_free(atlas_plan_op *op);

/* What a plan without a stated bound runs its stages under.
 *
 * Two rather than one, because a plan exists to describe work that has a
 * workspace sibling beside the stage's repository task; a default of one would
 * make every plan's `side` tasks refusable by arithmetic the operator never saw.
 * It is still a *default*: a plan may name anything from 1 to
 * ATLAS_ORCH_RUN_MAX_PARALLEL, and a value outside that is refused. */
#define ATLAS_PLAN_DEFAULT_PARALLEL 2

/* What an applied operation reports back.
 *
 * The two refusal members are the reason this is a typed result rather than an
 * `atlas_err` alone. A planner document that fails to parse is not a caller's
 * mistake — it is a *model's* mistake, and the driver answers it by composing a
 * retry prompt out of Atlas' own refusal sentence and the line it happened on.
 * Folding the line into the sentence would make the driver parse Atlas' prose to
 * recover it, so the two travel apart.
 *
 * **A non-empty `refusal` is the discriminator.** Every refusal that came from
 * the document — a parse refusal from `atlas_plan_parse`, or the merged-gate
 * bound this layer applies on top of it — sets it, always to a non-empty
 * sentence. Every other refusal (the plan does not exist, the job is not a
 * planner job, the artifact is missing) leaves it empty and is a caller's
 * problem, not a planner's. Both return a non-OK status and both leave the
 * transaction rolled back with no row written; the members survive the rollback
 * because nothing on the failure path resets them. */
typedef struct atlas_plan_result {
    /* CREATE: the identifier Atlas generated. REVISION_ADD: the plan named. */
    atlas_buf plan_uid;
    int64_t plan_id;
    /* REVISION_ADD: the revision written, and how many task rows it holds. */
    int rev_no;
    int task_count;

    /* The refused document's sentence, or empty. UNTRUSTED_DATA it is not — it
     * is Atlas' own prose about untrusted bytes, and it quotes none of them. */
    atlas_buf refusal;
    /* The 1-based line the refusal is about, or 0 for a refusal about the
     * document as a whole. Zero is also what an un-refused result carries, which
     * is why `refusal` and not this is the discriminator. */
    int refusal_line;
} atlas_plan_result;

void atlas_plan_result_init(atlas_plan_result *r);
void atlas_plan_result_free(atlas_plan_result *r);

/* Begin, apply, commit, with rollback on failure. Call with no transaction
 * open. */
atlas_status atlas_plan_apply(atlas_db *db, const atlas_plan_op *op, atlas_plan_result *out,
                              atlas_err *err);

/* The single write point. Assumes the caller owns a transaction, and has exactly
 * one caller — `atlas_plan_apply`. Adding a second means arguing that it owns a
 * genuinely wider unit of work, which is the argument A4 requires for
 * `atlas_decision_apply_in_tx`; adding a second *implementation* is what the
 * rule forbids. */
atlas_status atlas_plan_apply_in_tx(atlas_db *db, const atlas_plan_op *op, atlas_plan_result *out,
                                    atlas_err *err);

/* --- the derived state, read ------------------------------------------------
 *
 * The one implementation of "what is this plan doing?". `plan.get`, the plan
 * driver's loop and the replan composer all ask this function, because three
 * derivations of one status are three answers that can disagree about whether a
 * plan is finished.
 *
 * Read-only and pure with respect to the database: it writes nothing, takes no
 * lock and creates no process, and the same rows give the same answer on every
 * call. Every rule it applies is documented beside the code with the row shape
 * it reads.
 *
 * A plan that does not exist is a refusal, not an UNKNOWN status: UNKNOWN is the
 * vocabulary's zero and means "nobody derived this", so returning it for a plan
 * Atlas looked for and did not find would make a missing plan indistinguishable
 * from an unfilled struct. Every plan that *does* exist derives one of the five
 * real statuses. */
atlas_status atlas_db_plan_state_derive(atlas_db *db, const char *plan_uid, atlas_plan_state *out,
                                        atlas_err *err);

/* The correlation that binds one job to one plan, which is the whole of the
 * plan↔job mapping: there is no bind RPC, no `plan_id` column on `orch_jobs` and
 * no update after submission.
 *
 *   `plan.<uid21>.planner.<k>`   for planner job k        (36 bytes)
 *   `plan.<uid21>.r<rev>.<key>`  for a revision's task    (62 bytes at worst)
 *
 * `<uid21>` is the plan identifier **shortened for this purpose only**: the
 * `'p'` and the first 20 hex digits, 80 bits. The full 33-character uid is what
 * `orch_plans` stores and what every other surface uses; it is shortened here
 * because a correlation has to fit inside what a job specification may carry.
 * `atlas_orch_spec_validate` holds `correlation` and `idempotency_key` to
 * `is_name` — `[a-z0-9._-]`, at most `ATLAS_ORCH_NAME_MAX` — and a full uid
 * beside a 32-byte task key does not fit in 64 bytes with any separators.
 *
 * That bound is a constraint on the *stored* value and not merely on what a
 * client may send: `spawn_follow_up` validates the correlation a follow-up
 * inherits from its parent, so a correlation that would not validate is one
 * whose repo-tree task could not create the follow-up a failed gate earned.
 *
 * Both refuse a `plan_uid` that is not `'p'` followed by hex, and the task form
 * refuses a key outside `[a-z0-9-]{1,32}`: this is the one place either is
 * spelled into a name, so it is the one place that has to check.
 *
 * Exposed because three layers build the same string — the write point checking
 * a planner job's binding, the derived reader finding a plan's jobs, and the
 * driver submitting them — and a second spelling of one format is a second
 * answer to "is this job this plan's". They serve the idempotency keys too, for
 * the same reason. `out` is set, not appended to. */
atlas_status atlas_plan_correlation_planner(const char *plan_uid, int k, atlas_buf *out,
                                            atlas_err *err);
atlas_status atlas_plan_correlation_task(const char *plan_uid, int rev_no, const char *task_key,
                                         atlas_buf *out, atlas_err *err);

#endif /* ATLAS_PLAN_H */

# A12.0 — The Planned Run: planner and executor roles — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development.
> Tasks are dispatched to executor-model subagents one at a time (T1/T2/T4 may run in
> parallel); the planner/reviewer session reviews every diff before the next task starts.
>
> **Deviation from the writing-plans skill, stated deliberately:** the operator has
> assigned roles — the planner (Fable) writes this plan and reviews; the executors
> (Opus) write all production code. Therefore this plan pins *interfaces, SQL,
> formats, templates, refusal sentences and test obligations* exactly, and leaves C
> function bodies to the executor. Where the skill says "show the code", this plan
> shows the contract the code must satisfy instead. Everything else in the skill
> applies: bite-sized steps, TDD, frequent commits, no placeholders.

**Goal:** Given only a high-level goal, a gate floor and a bound, Atlas runs an
end-to-end planned run: it invokes a planner-role model to produce a durable,
re-readable plan; compiles the plan into staged tasks; executes them with
executor-role model workers (parallel where the plan allows); runs every gate
itself; routes blockers back to the planner as bounded plan revisions; and
reaches a terminal verdict through its own settlement, surviving restarts.

**Architecture:** A plan layer *above* runs, reusing A11.6's proven shape verbatim.
Each plan revision compiles to a sequence of **stage-runs**: one repo-tree task as
the run's root plus 0..k workspace siblings, executed by the existing run driver,
dispatchers, leases, gates, follow-ups, budgets and quiescence settlement. Plan
state is stored in three new tables (migration 25); plan *status* is derived on
every read (A6 discipline: nothing cached, no settle writer, no new authority).
Model roles are named per driver by the root-owned orchestration policy
(`planner_model`, `executor_model`) — never hard-coded.

**Sentence the season exists for:**
> **AN OPERATOR WHO HAD TO SPLIT THE GOAL BY HAND WAS DOING THE ORCHESTRATOR'S JOB.**

**Tech stack:** C17, SQLite (numbered migrations), first-party line parser (no
yyjson — its contract stays "IPC boundary only"), existing orch machinery.

**Spec:** this document is the spec; §Design carries the full argument.

## Global constraints (repo-wide, every task inherits these)

- Warnings are errors (`ATLAS_WERROR=ON`). No new third-party deps. No shell —
  `atlas_proc_run` with explicit argv only. No Python/Node in build/tests.
- Never modify a registered target repository from Atlas code; the only writer is
  a worker an operator started, under a repo-tree driver, per A11.1.
- Tests always override the data dir (`fx_open`/`--data-dir`); daemon tests
  override `XDG_RUNTIME_DIR`; never touch the real DB/socket; never start real
  systemd units.
- New `.c` files go in the explicit `atlas_core` list in `CMakeLists.txt`; new
  tests go in `ATLAS_TESTS` **and** a `set_tests_properties(... LABELS ...)` line
  in `tests/CMakeLists.txt`.
- A new command touches five places: service fn, `atlas_renderer_vtbl` method in
  `render.h`, implementations in **both** `render_human.c` and `render_json.c`
  (same positional offset in both initializers!), dispatch+help in `cli.c`, and
  `COMMANDS[]` in `is_a_command`. Then run the built binary once.
- Untrusted text (worker output, planner prose, goal read back) is safe-encoded
  at the daemon (`atlas_safe`), labelled (`atlas-safe-1`, `UNTRUSTED_DATA`), and
  printed as-is by renderers. Values delivered *to* a worker are raw bytes with a
  provenance label beside them.
- Single write point per domain; every state change is a CAS; ledger id is the
  ordering authority; UNKNOWN is zero, stored nowhere, parsed never.
- No new RPC group, no MCP tool, no gateway route, no second submit path, no
  thread, no timer, no background loop. The plan driver is a foreground loop an
  operator starts, exactly like the run driver.
- Commit after every green task with the repo's commit style
  (`feat(a12): ...` / `fix(a12): ...` / `docs(a12): ...`; trailer
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`).
- **No commits of unrelated changes; never push until the final task.**

---

# Design

## What exists (verified against the tree at 4d2c134)

- `atlas job run --repo R --task T --gate C...` starts a foreground run driver
  (`src/orch/rundriver.c`) that submits a root repo-tree task through the one
  submit path (`job.submit`), claims it by name, pins HEAD, records RUNNING
  before exec, starts one `claude` CLI worker
  (`claude --print --output-format stream-json --permission-mode acceptEdits <task>`
  — compiled argv in `src/orch/driver.c:589-611`, **no model flag**), re-checks
  HEAD, runs gates itself (`src/orch/validate.c`, allowlist
  `{make, ctest, cmake, true, false}`), and reports a completion. Gate failure
  spawns exactly one follow-up (`db_orch.c:2330`, key `a11.<parent>.<attempt>`),
  budget 3 repo-tree worker starts per run derived from the ledger.
- A11.6: a run admits up to `orch_runs.max_parallel` (1..8) active tasks, at most
  one in the repo tree (two partial unique indexes, M24). Siblings are workspace
  jobs (`job submit --parent`), driven by dispatcher processes; a sibling can
  veto acceptance, never grant it. Settlement is quiescence + scan
  (`settle_run_at_quiescence`, `db_orch.c:2523`), CAS through
  `atlas_db_orch_run_set_status` (no RPC/MCP/gateway caller — authority by
  absence). Settle-eligibility is the **root** task's driver: a workspace-rooted
  run never settles (existing semantics; a stated cost below).
- Policy (`/etc/atlas/orchestration.conf`, root-owned, compiled-in path, unknown
  key = MALFORMED): dispatcher/submitter uids, repos, modes, drivers, ceilings,
  `model_dispatcher_uid`/`model_worker_root`/`model_credential`/`live_model`.
  **No model-name key exists**; the backlog (docs/backlog.md:1019-1027) records
  "a worker's model is the operator session's default, and nothing can choose
  it" as an open residual — this season closes it.
- Usage (M22 `orch_usage`): per-attempt provider/model/token/cost telemetry from
  the final stream-json `result` record; run totals derived on read.
- Both pilots died once each on the run driver's transport fragility: `apply_op`
  retries only `BUSY:`; one frame-header read timeout killed the invocation
  (docs/backlog.md:1000-1006). The plan driver sits above the same transport, so
  this is fixed first (T1).

## What A12.0 adds

1. **Role→model selection in the root-owned policy** (`planner_model`,
   `executor_model`, both optional; absent = today's behavior), carried to the
   worker as `--model <value>`; role is a property of the driver
   (`claude-plan` → PLANNER; `claude`, `claude-repo` → EXECUTOR; fakes → NONE).
2. **Two drivers**: `claude-plan` (workspace, live-model, planner role) and
   `fake-plan` (workspace, no model, emits a canned plan artifact from its task
   text — the test vehicle). Neither is repo-tree ⇒ **no index migration**.
3. **Migration 25**: `orch_plans`, `orch_plan_revisions`, `orch_plan_tasks`,
   plus `idx_orch_jobs_correlation`. Plan status is **derived on read** — no
   status column, no settle writer, nothing for a model payload to reach.
4. **The `atlas-plan-1` format**: a bounded, line-based plan document the
   planner emits as a workspace artifact named `plan.atlas-plan`; a first-party
   parser/validator/compiler (pure functions, adversarially tested).
5. **Four RPC methods** in the existing orch client group: `plan.create`,
   `plan.revision_add`, `plan.get`, `plan.list`. `plan.revision_add` reads the
   planner job's own stored artifact from `orch_artifacts` — the model's bytes
   never travel a second path.
6. **The plan driver** (`atlas plan run`): a foreground loop that submits the
   planner job, ingests the revision, then walks stages — each stage is one
   ordinary run (tree root + siblings) driven by the existing run driver —
   and answers a BLOCKED stage-run with one bounded plan revision.
7. **CLI**: `atlas plan run|status|show|list` with both renderers.
8. **Live pilot A12-P**: planner=fable, executor=opus, chosen by policy, on a
   small real goal, with restart-survival and parallel-overlap evidence.

## Authority argument (the season's non-negotiables)

- **The operator brings the goal and the gate floor; the planner may only add
  gates, never remove or replace one.** `plan run` requires ≥1 `--gate` exactly
  as `job run` does; the floor is stored on the plan row and prepended verbatim
  to every tree task's validations. A model choosing its own verification would
  be acceptance on the model's word.
- **A plan is a proposal, never a verdict.** Compiling a plan grants nothing:
  every task still runs under the existing submit refusals, budgets, leases,
  gates and settlement. Planner and executor prose is UNTRUSTED_DATA end to end.
- **Plan status has no writer.** It is derived on every read from stored rows
  (revisions, correlations, job states, run statuses). There is no
  `plan.settle`, no status column, no CAS to reach — authority by absence,
  A11.0's own pattern one layer up.
- **Only a planner-role job can produce a revision.** `plan.revision_add`
  verifies, inside the write transaction: the named job's `correlation` binds it
  to this plan as planner job k; its driver's role is PLANNER; it SUCCEEDED; the
  artifact `plan.atlas-plan` is stored inline and within bounds. An executor
  job's artifact can never become a plan.
- **Model prose never routes control flow.** The replan trigger is Atlas' own
  verdict — a stage-run that settled BLOCKED — never a sentence a worker wrote.
  (A blocker-artifact fast-path is recorded as a backlog residual: it could only
  veto earlier, never grant.)
- **Roles live in the root-owned policy, not in code.** No model name appears in
  `src/`; `planner_model = fable` / `executor_model = opus` are this machine's
  current choices, not Atlas'.
- **Stated cost:** a standalone workspace-rooted run (each planner job's run)
  stays ACTIVE forever — pre-existing semantics, now produced on purpose.
  Documented, surfaced distinctly in `plan status`, and recorded in the backlog
  rather than "fixed" by letting a gateless run settle.
- **Stated cost (pre-existing, restated):** in `model_credential =
  operator_session` mode a model worker runs as the operator uid and could reach
  the client RPC group, including `plan.create` — exactly as it can reach
  `job.submit` today. A7.1's adversary is `atlas-worker`; the operator account
  is trusted by design. The revision-add binding checks are what keep even that
  worker from forging a *revision*.

## Worst-case cost ceiling (stated, in the repo's style)

Model-worker starts per plan, all bounds compiled in:
planner jobs ≤ `ATLAS_PLAN_MAX_PLANNER_JOBS` (5), each `max_attempts = 1` ⇒ ≤ 5
planner starts. Per compiled revision: ≤ `ATLAS_PLAN_MAX_STAGES` (4) stage-runs;
each stage-run: ≤ 3 repo-tree starts (existing budget) + ≤
`ATLAS_PLAN_MAX_SIDE_PER_STAGE` (3) siblings × `max_attempts = 1`. Revisions that
compiled ≤ `ATLAS_PLAN_MAX_REVISIONS` (3), and completed stages are never re-run,
but the stated ceiling ignores that: **≤ 5 + 3 × 4 × (3 + 3) = 77 worker
starts**. The practical small-goal case is one revision, one or two stages —
roughly 4–8 starts. The number exists so nobody discovers it in a bill.

## The `atlas-plan-1` format (frozen by this plan)

The planner writes exactly one artifact named `plan.atlas-plan` (bytes ≤
`ATLAS_PLAN_MAX_BYTES` = 65536). Line-based; lines ≤ 4096 bytes; `\r` stripped;
UTF-8 not assumed; unrecognised line = refusal naming the line number.

```
atlas-plan-1
stage: 1
task: <key>            # [a-z0-9-]{1,32}, unique across the whole plan
kind: tree             # exactly one tree task per stage
title: <one line, ≤ 200 bytes>
gate: <cmd>            # tree only, 0..n; appended AFTER the operator floor;
                       # same parsing as --gate (space-split argv, allowlist
                       # make/ctest/cmake/true/false); floor+additions ≤ 8 total
prompt<<
<free text for the executor, ≤ 16384 bytes>
>>
task: <key2>
kind: side             # 0..3 per stage; no gate: lines allowed on side tasks
title: ...
prompt<<
...
>>
stage: 2
...
```

Validation (all refusals, each with a sentence naming what and where):
- header line exactly `atlas-plan-1`;
- stages numbered 1..N ascending, no gaps, N ≤ `ATLAS_PLAN_MAX_STAGES`;
- exactly one `kind: tree` per stage; side tasks per stage ≤
  `ATLAS_PLAN_MAX_SIDE_PER_STAGE` **and** ≤ (plan's `max_parallel` − 1);
- total tasks ≤ `ATLAS_PLAN_MAX_TASKS` (8); keys unique; every field present
  for every task (`task`, `kind`, `title`, `prompt`); `gate:` under `kind: side`
  is a refusal; a gate program outside the allowlist is a refusal;
- merged validations per tree task = operator floor (verbatim, first) +
  planner additions (in order) ≤ `ATLAS_ORCH_MAX_VALIDATIONS` (8).

## Correlation and idempotency scheme (restart safety without new writes)

Every job the plan layer submits carries a `correlation` (existing spec field)
and an idempotency key; the mapping plan↔jobs is *derived* from these — no bind
RPC, no job_uid column updates:

| job | correlation | idempotency key |
| --- | --- | --- |
| planner job k (k = 1..5) | `plan:<plan_uid>:planner:<k>` | `plan.<plan_uid>.planner.<k>` |
| tree task of stage S, revision R | `plan:<plan_uid>:r<R>:<task_key>` | `plan.<plan_uid>.r<R>.<task_key>` |
| side task, revision R | `plan:<plan_uid>:r<R>:<task_key>` | `plan.<plan_uid>.r<R>.<task_key>` |

A crashed driver resumed at any point re-issues the same submissions; the
idempotency table returns the existing job. `idx_orch_jobs_correlation` (M25)
makes the derived read cheap.

## Derived plan status (one implementation, two askers)

`atlas_db_plan_state_derive(db, plan_uid, &out, err)` — read-only; used by both
`plan.get` and the plan driver's loop. Output struct carries: status, current
revision no, planner job k + its state, per-task rows (key, stage, kind, job
state, run uid + run status for tree tasks), and counts.

Status vocabulary `atlas_plan_status` (zero = UNKNOWN, never stored, never
parsed): `PLANNING` (no usable revision yet, planner budget remains),
`EXECUTING` (latest revision has non-terminal work), `NEEDS_REPLAN` (a stage-run
of the latest revision settled BLOCKED or a planner artifact was refused, and
budgets remain — the driver acts on this), `COMPLETED` (every stage-run of the
latest revision ACCEPTED and every side job of it SUCCEEDED), `BLOCKED` (a
budget is exhausted or the planner's last job failed terminally with no budget
left). Derivation rules are pure functions of stored rows; the same inputs give
the same answer on every read.

## Prompt templates (deterministic, composed like `follow_up_text`)

Five composers in `src/orch/plan.c`, byte-exact under test. All bounded inputs;
all untrusted excerpts labelled; all end with a fixed constraints block stating:
instruction is not enforcement, Atlas runs the gates and settles, the worker
grants nothing.

1. **planner-initial**: `atlas-plan-request:` header; the operator goal
   (bounded, labelled untrusted on read-back surfaces only — the worker gets raw
   bytes per the existing lease contract); the frozen format spec above,
   verbatim; the operator gate floor listed as immutable; the bounds (stages,
   tasks, side-per-stage = min(3, max_parallel−1)); the required artifact name.
2. **planner-parse-retry**: planner-initial + `previous-plan-refused:` + the
   refusal sentence + a bounded excerpt (≤ 4096) of the refused artifact.
3. **planner-replan**: planner-initial's header and goal + a completed-work
   section (per completed task: key, title-as-untrusted-excerpt, SUCCEEDED) +
   `blocked-task:` key + `failed-gate:` name (from the job's own stored
   validations by index) + bounded gate-output excerpt (≤ 4096) +
   "produce a complete new plan for the remaining work; completed work stands."
4. **executor-tree**: `atlas-plan-task:` header; plan uid, revision, stage, task
   key; the planner's title+prompt as a labelled untrusted block; then the fixed
   scope-lock: work only in the repository root; apply exactly this task; do not
   change scope, plan, or gates; no commit/push/deploy/reset; report honestly.
5. **executor-side**: as (4) with the workspace variant: work in the provided
   workspace; produce results as files under `artifacts/`; you cannot and must
   not try to edit the repository itself.

---

# File structure

| path | status | owns |
| --- | --- | --- |
| `include/atlas/plan.h` | new (T4) | format structs, bounds, parser/compiler/composer and derived-state API |
| `src/orch/plan.c` | new (T4) | parser, validator, prompt composers (pure; no DB, no process, no clock) |
| `src/db/db_plan.c` | new (T3) | migration-25 tables' single write point + derived-state reader |
| `src/db/migrate.c` | modify (T3) | `M25_*` literals + `MIGRATIONS[]` entry |
| `include/atlas/db.h` | modify (T3) | `ATLAS_SCHEMA_VERSION` 24 → 25 |
| `include/atlas/orchpolicy.h`, `src/orch/policy.c` | modify (T2) | `planner_model`, `executor_model` |
| `include/atlas/driver.h`, `src/orch/driver.c` | modify (T2) | `model_role`, `req->model`, `--model` argv, `claude-plan`, `fake-plan` |
| `src/orch/rundriver.c` | modify (T1, T6) | transport recovery; `req.model` fill |
| `src/orch/dispatch.c`, `src/core/service_orch.c` | modify (T2, T6) | model fill for workspace attempts; targeted attempt runner seam |
| `src/ipc/server_orch.c` (or new `server_plan.c`) | modify/new (T5) | four `plan.*` methods in the client group |
| `src/orch/plandriver.c`, `include/atlas/plandriver.h` | new (T6) | the foreground plan loop |
| `src/core/service_plan.c` | new (T7) | `atlas_service_plan_run/status/show/list` |
| `src/cli/cli.c`, `src/cli/render.h`, `render_human.c`, `render_json.c` | modify (T7) | `plan` command, `plan_item` renderer method |
| `deploy/a8/orchestration.conf.template` | modify (T2) | the two new keys, commented |
| `tests/test_plan_format.c` | new (T4) | parser/validator/composer units |
| `tests/test_plan_db.c` | new (T3) | migration, write point, derived state |
| `tests/test_plan_rpc.c` | new (T5) | methods, groups, binding refusals |
| `tests/test_plan_driver.c` | new (T8) | full lifecycle with fake drivers |
| docs (T9) | modify | orchestration.md, roadmap.md, CLAUDE.md, extending.md, backlog.md, README |

---

# Tasks

### Task T1: transport recovery in the run driver's apply path

Both pilots lost an invocation to one transport hiccup; the plan driver stacks a
second loop on the same transport. Root cause: `apply_op` (rundriver.c:228-243)
treats every non-`BUSY:` error as fatal, including connection-level failures
where the daemon may or may not have processed the request.

**Files:** modify `src/orch/rundriver.c`; test additions in the existing rundriver
coverage (`tests/test_orch_run.c` or a new `tests/test_orch_transport.c` if the
fixture shape demands it — executor's call, registered in both CMake places).

**Interfaces produced:** internal only; the observable contract is:
- LEASE / HEARTBEAT / EVENT ops: on a transport-classified failure (an
  `atlas_err` that is not `BUSY:` and not a daemon-sent refusal — classification
  by a new small predicate on the error, e.g. connect/`read`/timeout failures
  from the IPC client layer; find the exact error shapes in `src/ipc/` client
  code and enumerate them, never substring-match daemon sentences), retry
  bounded: `RUN_XPORT_TRIES = 5`, pause 2000 ms, then give up with the original
  error. A daemon *refusal* is never retried.
- COMPLETE: keep the existing 300 s BUSY budget; add the same transport
  classification, and after any transport failure re-read the job
  (`xport_run_get` / `job.get`): if the attempt's outcome is already terminal
  with our result, treat the completion as delivered (the pilots' exact loss).
- A lease re-request after a transport failure that actually landed is already
  safe: naming a non-QUEUED job grants nothing and is not an error.

**Steps:**
- [ ] Failing test: fixture transport that fails N times with a transport-shaped
  error then succeeds; assert `drive_one`'s op path survives; assert a
  daemon-refusal error is NOT retried (fails fast).
- [ ] Failing test: completion path — transport dies after the daemon applied the
  completion; assert the driver re-reads and reports delivered, not dead.
- [ ] Implement; run `ctest -R 'test_orch_(run|transport)' --output-on-failure`.
- [ ] Commit `fix(a12): the run driver survives a transport hiccup, and a completion is checked before it is mourned`.

### Task T2: model roles in policy and drivers

**Files:** modify `include/atlas/orchpolicy.h`, `src/orch/policy.c`,
`include/atlas/driver.h`, `src/orch/driver.c`, `src/orch/rundriver.c` (fill
`req.model` for tree attempts), `src/core/service_orch.c` + `src/orch/dispatch.c`
(fill for workspace attempts), `deploy/a8/orchestration.conf.template`; tests in
`tests/test_orch_model.c`, `tests/test_orch_driver.c`.

**Interfaces produced:**
- `atlas_orchpolicy` gains `char planner_model[65]; char executor_model[65];`
  (empty = unset). Parser: keys `planner_model`, `executor_model`, each at most
  once; value token `[a-z0-9._-]{1,64}` else MALFORMED; absent keys leave empty.
  Both keys optional — an old policy stays valid; ceilings/completeness rules
  unchanged.
- `atlas_driver` gains `atlas_driver_role role;` with
  `typedef enum { ATLAS_DRIVER_ROLE_NONE = 0, ATLAS_DRIVER_ROLE_PLANNER, ATLAS_DRIVER_ROLE_EXECUTOR } atlas_driver_role;`
  `claude` → EXECUTOR, `claude-repo` → EXECUTOR, new `claude-plan` → PLANNER,
  `fake`/`fake-repo`/new `fake-plan` → NONE.
- `atlas_driver_req` gains `const char *model;` (NULL/empty = no flag).
  `claude_exec` inserts `"--model", req->model` into the argv when non-empty.
  Extract argv construction into a small pure function
  (`claude_build_argv(req, exe, argv_out, ...)`) so a unit test can assert the
  vector without exec'ing anything.
- New driver `claude-plan`: version 1, `needs_live_model = true`, run fn =
  `claude_run` (workspace variant), role PLANNER.
- New driver `fake-plan`: version 1, no live model, role NONE; its run fn scans
  the task text for a line `fake-plan-artifact:` and writes everything after
  that line, verbatim, to `<ws>/artifacts/plan.atlas-plan`, then succeeds; if
  the marker is absent it writes nothing and fails. This is the test vehicle for
  the whole plan pipeline.
- Callers fill `req.model` from the loaded policy by the driver's role:
  PLANNER → `planner_model`, EXECUTOR → `executor_model`, NONE → empty.
  The value flows: policy → (rundriver | dispatch) → `atlas_driver_req.model`.
- `atlas_orchpolicy_load_at` seam unchanged; tests parse policies with and
  without the new keys, with duplicates (MALFORMED), with an over-long or
  bad-charset value (MALFORMED).

**Steps:**
- [ ] Failing tests: policy parse cases; argv builder cases (no model → no flag;
  model set → `--model <v>` before the task element; task stays last).
- [ ] Failing test: `fake-plan` writes the canned artifact through the real
  workspace attempt path (pattern: existing `fake` driver tests in
  `test_orch_driver.c`).
- [ ] Implement; run `ctest -R 'test_orch_(model|driver)' --output-on-failure`.
- [ ] Update the policy template with both keys, commented out, with the season's
  one-line rationale.
- [ ] Commit `feat(a12): the root-owned policy names a model per role, and two plan drivers exist`.

### Task T3: migration 25 and the plan tables' write point

**Files:** modify `src/db/migrate.c`, `include/atlas/db.h`; new `src/db/db_plan.c`
(+ its declarations — put them in `include/atlas/plan.h`, created in T4; if T3
lands first, create the header with the DB-facing section only and a comment
naming T4 as the other owner); wire `db_plan.c` into `atlas_core` in
`CMakeLists.txt`; new `tests/test_plan_db.c` (both CMake registrations, label
`integration`).

**Interfaces produced:**
- Migration 25, name `"the planned run: plans, revisions, plan tasks"`,
  `foreign_keys_off = false`, statements:

```sql
CREATE TABLE orch_plans (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  plan_uid TEXT NOT NULL UNIQUE,
  repo_name TEXT NOT NULL,
  repo_identity_hash TEXT NOT NULL,
  goal_text TEXT NOT NULL,
  gate_floor TEXT NOT NULL,
  max_parallel INTEGER NOT NULL DEFAULT 2
    CHECK(max_parallel >= 1 AND max_parallel <= 8),
  submitter_uid INTEGER NOT NULL,
  created_at TEXT NOT NULL,
  created_ms INTEGER NOT NULL
);
CREATE INDEX idx_orch_plans_repo ON orch_plans(repo_identity_hash, id);

CREATE TABLE orch_plan_revisions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  plan_id INTEGER NOT NULL REFERENCES orch_plans(id) ON DELETE CASCADE,
  rev_no INTEGER NOT NULL CHECK(rev_no >= 1 AND rev_no <= 3),
  planner_job_uid TEXT NOT NULL,
  reason TEXT NOT NULL CHECK(reason IN ('INITIAL','REPLAN')),
  content BLOB NOT NULL,
  content_sha256 TEXT NOT NULL,
  created_at TEXT NOT NULL,
  UNIQUE(plan_id, rev_no)
);

CREATE TABLE orch_plan_tasks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  revision_id INTEGER NOT NULL REFERENCES orch_plan_revisions(id) ON DELETE CASCADE,
  plan_id INTEGER NOT NULL REFERENCES orch_plans(id) ON DELETE CASCADE,
  stage_no INTEGER NOT NULL CHECK(stage_no >= 1 AND stage_no <= 4),
  task_key TEXT NOT NULL,
  kind TEXT NOT NULL CHECK(kind IN ('TREE','SIDE')),
  title TEXT NOT NULL,
  prompt TEXT NOT NULL,
  validations TEXT NOT NULL,
  UNIQUE(revision_id, task_key)
);
CREATE INDEX idx_orch_plan_tasks_rev ON orch_plan_tasks(revision_id, stage_no, id);

CREATE INDEX idx_orch_jobs_correlation ON orch_jobs(correlation, id);
```
  Literal bounds 3/4/8 mirror `ATLAS_PLAN_MAX_REVISIONS` /
  `ATLAS_PLAN_MAX_STAGES` / (`max_parallel` ceiling) because SQLite cannot read
  a macro; `tests/test_plan_db.c` asserts constants and CHECKs agree (the
  `test_orch_parallel.c` pattern). Additive only; nothing rebuilt; no backfill.
- `ATLAS_SCHEMA_VERSION` 24 → 25.
- Write point `atlas_plan_apply_in_tx(atlas_db*, const atlas_plan_op*, atlas_plan_result*, atlas_err*)`
  in `db_plan.c`, wrapper `atlas_plan_apply` (begin/commit/rollback), op kinds:
  - `ATLAS_PLAN_OP_CREATE`: inputs repo_name, repo_identity_hash, goal_text
    (non-empty, ≤ 16384, no NUL), gate_floor (encoded validations, ≥ 1 gate),
    max_parallel (1..8, 0 = default 2), submitter_uid (peer, never body).
    Generates `plan_uid` = `'p'` + 32 lowercase hex (the run-uid generator's
    pattern). Refusals name what and why.
  - `ATLAS_PLAN_OP_REVISION_ADD`: inputs plan_uid, planner_job_uid, reason,
    expected rev_no. In the transaction, verify and refuse otherwise: plan
    exists; rev_no == stored max + 1 and ≤ 3; the named job's correlation is
    exactly `plan:<plan_uid>:planner:<k>` for some k ≤ 5; the job's driver has
    role PLANNER (ask `atlas_driver_find` → role — the stored driver *name* is
    what's checked, the C predicate answers it); job state SUCCEEDED; artifact
    named `plan.atlas-plan` exists for its successful attempt with
    `content_stored = 1` and `size_bytes ≤ ATLAS_PLAN_MAX_BYTES`. Then parse +
    validate + compile via T4's pure functions (`atlas_plan_parse` on the
    artifact bytes; merge the plan's stored gate floor per tree task); a parse
    or validation refusal aborts the transaction and returns the refusal
    sentence + line number in `atlas_plan_result` (typed field, not just err) —
    the driver builds the parse-retry prompt from it. On success: insert the
    revision row (content = verbatim artifact bytes, sha256) + all task rows.
- Derived-state reader `atlas_db_plan_state_derive(atlas_db*, const char *plan_uid, atlas_plan_state*, atlas_err*)`
  per the Design section: reads plan row, revisions, tasks, and joins jobs by
  correlation (planner k's and revision tasks'), runs by `run_uid` for tree
  tasks. Struct (in `plan.h`):

```c
typedef struct atlas_plan_task_view {
  char task_key[33]; int stage_no; bool is_tree;
  char job_uid[36]; atlas_orch_state job_state;   /* empty/UNKNOWN = not submitted */
  char run_uid[36]; atlas_orch_run_status run_status; /* tree only */
} atlas_plan_task_view;
typedef struct atlas_plan_state {
  atlas_plan_status status;
  int rev_no;                       /* 0 = none compiled yet */
  int planner_jobs_seen;            /* k count derived from correlations */
  char planner_job_uid[36];         /* latest planner job, if any */
  atlas_orch_state planner_job_state;
  int task_count;
  atlas_plan_task_view tasks[ATLAS_PLAN_MAX_TASKS];
  int stages_accepted;              /* runs of latest revision ACCEPTED */
  bool replan_wanted;               /* BLOCKED stage-run or refused artifact, budget left */
} atlas_plan_state;
```
  (Also needed for the refused-artifact half of `replan_wanted`: a refused parse
  leaves *no* revision row — derive "latest planner job SUCCEEDED but no
  matching revision exists" as the refused state; the driver re-prompts with the
  stored refusal it got synchronously, or re-runs `plan.revision_add` on resume
  to re-obtain the same deterministic refusal. State it in a comment.)
- A trust test (pattern `test_orch_trust.c`): `atlas_plan_apply_in_tx` is the
  only writer of `orch_plan*` tables.

**Steps:**
- [ ] Failing tests: migrate a fixture DB 0→25, assert tables/indexes exist and
  constants agree; CREATE op happy + refusals; REVISION_ADD binding refusals
  (wrong correlation, non-planner driver, unsuccessful job, missing artifact,
  oversized artifact, rev_no gap, rev_no > 3); derived state over hand-built
  rows for every status value.
- [ ] Implement; `ctest -R test_plan_db --output-on-failure`; also run one
  daemon-suite smoke (`ctest -R test_scan`) to catch schema fallout.
- [ ] Commit `feat(a12): migration 25 - the plan, its revisions and its tasks, with one write point and a derived status`.

### Task T4: the plan format — parser, validator, compiler, composers

**Files:** new `include/atlas/plan.h` (format+bounds section; merge with T3's DB
section — whichever task lands second reconciles, both sections are specified
here), new `src/orch/plan.c` (add to `atlas_core`), new
`tests/test_plan_format.c` (label `unit`).

**Interfaces produced (exact):**
```c
#define ATLAS_PLAN_MAX_BYTES        65536
#define ATLAS_PLAN_MAX_TASKS        8
#define ATLAS_PLAN_MAX_STAGES       4
#define ATLAS_PLAN_MAX_SIDE_PER_STAGE 3
#define ATLAS_PLAN_TASK_PROMPT_MAX  16384
#define ATLAS_PLAN_TITLE_MAX        200
#define ATLAS_PLAN_MAX_REVISIONS    3
#define ATLAS_PLAN_MAX_PLANNER_JOBS 5
#define ATLAS_PLAN_GOAL_MAX         16384
#define ATLAS_PLAN_ARTIFACT_NAME    "plan.atlas-plan"

typedef enum { ATLAS_PLAN_STATUS_UNKNOWN = 0, ATLAS_PLAN_STATUS_PLANNING,
  ATLAS_PLAN_STATUS_EXECUTING, ATLAS_PLAN_STATUS_NEEDS_REPLAN,
  ATLAS_PLAN_STATUS_COMPLETED, ATLAS_PLAN_STATUS_BLOCKED } atlas_plan_status;
const char *atlas_plan_status_name(atlas_plan_status s); /* no default: */

typedef struct atlas_plan_doc_task {
  char key[33]; int stage_no; bool is_tree;
  char title[ATLAS_PLAN_TITLE_MAX + 1];
  atlas_buf prompt;                       /* owned; _free via atlas_plan_doc_free */
  atlas_orch_argv gates[ATLAS_ORCH_MAX_VALIDATIONS]; size_t gate_count; /* planner additions only */
} atlas_plan_doc_task;
typedef struct atlas_plan_doc {
  int stage_count; size_t task_count;
  atlas_plan_doc_task tasks[ATLAS_PLAN_MAX_TASKS];
} atlas_plan_doc;
void atlas_plan_doc_free(atlas_plan_doc *d);

/* Parse+validate bytes (untrusted). On refusal: ATLAS_ERR_USAGE, and when
   line_out != NULL the 1-based line number (0 = document-level). max_parallel
   bounds side tasks per stage. Never partially fills on failure. */
atlas_status atlas_plan_parse(const void *bytes, size_t len, int max_parallel,
                              atlas_plan_doc *out, int *line_out, atlas_err *err);

/* Deterministic composers; every untrusted excerpt bounded + labelled. */
atlas_status atlas_plan_compose_planner(const char *goal, const char *gate_floor_text,
    int max_parallel, atlas_buf *out, atlas_err *err);
atlas_status atlas_plan_compose_planner_retry(const char *goal, const char *gate_floor_text,
    int max_parallel, const char *refusal, const void *refused, size_t refused_len,
    atlas_buf *out, atlas_err *err);
atlas_status atlas_plan_compose_replan(const char *goal, const char *gate_floor_text,
    int max_parallel, const atlas_plan_state *st, const char *blocked_key,
    const char *failed_gate, const void *gate_excerpt, size_t excerpt_len,
    atlas_buf *out, atlas_err *err);
atlas_status atlas_plan_compose_executor(const char *plan_uid, int rev_no,
    const atlas_plan_doc_task *t, atlas_buf *out, atlas_err *err);
```
`gate_floor_text` is the human-readable floor list (one command per line) the
service layer renders from the stored encoded floor; composers never decode
netstrings themselves. Pure: no DB, no process, no file, no clock (the memory.c
discipline). Gate parsing reuses the exact `--gate` split + the
`atlas_validations_run` allowlist by calling the existing helpers — no second
allowlist (`validate.c`'s list is the one; expose a tiny
`atlas_validation_program_allowed(const char*)` from `validate.c` if not already
public, instead of copying the array).

**Steps:**
- [ ] Failing tests (adversarial): valid two-stage doc round-trips; refused: bad
  header, stage gap, two tree tasks in a stage, zero tree tasks, side gate, task
  nine, key dup, key charset, over-long title/prompt/line, missing `>>`,
  non-allowlisted gate, side count > max_parallel−1, 64 KiB+1 input, embedded
  NUL, CRLF tolerated, non-UTF-8 bytes in prompt tolerated; line numbers
  asserted on three of them.
- [ ] Failing tests: each composer byte-exact against a golden string for a small
  fixed input (goldens in the test file, not files on disk).
- [ ] Implement; `ctest -R test_plan_format --output-on-failure`.
- [ ] Commit `feat(a12): the atlas-plan-1 format - a bounded plan a model proposes and Atlas compiles`.

### Task T5: the four plan RPC methods

**Files:** modify `src/ipc/server_orch.c` (or new `src/ipc/server_plan.c` wired
into `atlas_core` + the dispatch chain in `src/ipc/server.c` — follow whichever
shape `server_orch.c`'s size suggests; the client-group table is the one that
grows), `src/core/service_orch.c` gains the client-side wire helpers (or new
`service_plan.c` started here and finished in T7); new `tests/test_plan_rpc.c`
(label `daemon` if it forks the daemon; follow `test_orch_rpc.c`'s fixture).

**Interfaces produced:**
- Client-group methods (submitter-gated like `job.submit`, same
  "orchestration is not enabled" honest refusal):
  - `plan.create` params `{repo, goal, gate_floor (the --gate wire form used by
    job.submit), max_parallel}` → `{plan}` (uid). Resolves repo through the
    registry (name → identity hash) exactly as submit does; refuses an
    unregistered repo.
  - `plan.revision_add` params `{plan, planner_job, reason, rev_no}` →
    `{rev_no}` on success; on a *format* refusal, a structured error payload
    carrying the refusal sentence + line (the CLI/driver needs both; reuse the
    error-document shape with `detail`).
  - `plan.get` params `{plan}` → the derived state: plan row fields (goal
    safe-encoded + `goal_encoding: atlas-safe-1`), revisions (rev_no, reason,
    sha256, planner_job, created_at; content *not* inlined here), tasks with
    job/run states, status name, planner_jobs_seen, and a usage rollup per task
    where `orch_usage` rows exist (model, cost_micro_usd, turns — the
    role-evidence surface).
  - `plan.list` → rows (plan uid, repo, status, created_at).
  - Plan *content* read-back: `plan.get` with `{plan, rev_no}` also returns
    `content` safe-encoded + `content_encoding: atlas-safe-1` +
    `content_provenance: UNTRUSTED_DATA` when a rev_no is named (the
    `plan show` path; mirrors `job.artifact`'s encoding discipline).
- Writer-thread usage follows `job.submit`'s exact pattern (same job kind /
  sync-write path; if a new `atlas_job_kind` member is unavoidable, follow the
  A9.2.6 checklist in docs/extending.md: drainable = true — plan tables are
  disjoint from any pass — no `default:` anywhere).
- Refusal tests through the transport (the season rule): an ordinary client that
  is not a submitter; `revision_add` with an executor job; `revision_add` for a
  job whose artifact is absent; unknown method name intact for the dispatcher
  group.

**Steps:**
- [ ] Failing tests first (transport-level, fixture daemon).
- [ ] Implement; `ctest -R test_plan_rpc --output-on-failure`.
- [ ] Commit `feat(a12): plan.create, plan.revision_add, plan.get, plan.list - the plan travels the one client surface`.

### Task T6: the plan driver

**Files:** new `src/orch/plandriver.c` + `include/atlas/plandriver.h` (add to
`atlas_core`); modify `src/core/service_orch.c`/`src/orch/dispatch.c` only for
the seam below; tests land in T8 (this task still adds a compile-and-link smoke
use so the file is exercised).

**Interfaces produced:**
```c
typedef struct atlas_plandriver_transport {  /* function-pointer seam, fixture-hostable */
  void *ctx;
  atlas_status (*plan_create)(void*, const atlas_plan_create_req*, char plan_uid[36], atlas_err*);
  atlas_status (*plan_state)(void*, const char *plan_uid, atlas_plan_state*, atlas_err*);
  atlas_status (*plan_revision_add)(void*, const char *plan_uid, const char *planner_job,
                                    int rev_no, const char *reason, atlas_plan_refusal *ref, atlas_err*);
  atlas_status (*job_submit)(void*, const atlas_plan_job_req*, char job_uid[36], atlas_err*);
  atlas_status (*job_get)(void*, const char *job_uid, atlas_plan_job_view*, atlas_err*);
  atlas_status (*drive_run)(void*, const char *run_uid, atlas_err*);       /* rundriver on a tree stage */
  atlas_status (*drive_workspace_job)(void*, const char *job_uid, atlas_err*); /* targeted inline attempt */
} atlas_plandriver_transport;
atlas_status atlas_plandriver_run(const atlas_plandriver_opts *o,
                                  atlas_plandriver_report *rep, atlas_err *err);
```
  (`atlas_plan_create_req`, `atlas_plan_job_req`, `atlas_plan_job_view`,
  `atlas_plan_refusal {char sentence[256]; int line;}`,
  `atlas_plandriver_opts {transport, plan_uid_or_empty, repo, goal, gate_floor,
  max_parallel, log FILE*, max_iterations}`,
  `atlas_plandriver_report {plan_uid, status, rev_no, stages_accepted,
  planner_jobs, busy}` — all in `plandriver.h`.)
- **The `drive_workspace_job` seam (production impl):** extract from
  `dispatch.c` a function that runs exactly one *named* job's attempt in this
  process — targeted lease (`PICK_ONE` via the existing lease op with the job
  named and driver filter from the policy partition), workspace, driver, gates
  (workspace cwd), completion — reusing `run_attempt`'s body (make the static
  fn shareable; do not duplicate it). Only jobs whose driver partition matches
  this process' uid per policy (the model dispatcher partition) are eligible;
  a mismatch is a clean refusal the loop treats as "wait for a dispatcher".
- **The loop** (every step idempotent; everything derived from `plan_state`):
  1. No `plan_uid`: `plan_create` (goal, floor, parallel) — resume passes the
     uid instead; refusals for `--resume` with goal/gate/parallel mirror
     `--memory --resume`'s.
  2. Ask `plan_state`; switch on status:
     - PLANNING: ensure planner job k = planner_jobs_seen(+1 if last terminal
       or none) submitted — spec: driver `claude-plan` (or the opts' planner
       driver name so tests use `fake-plan`; the *policy* still authorises the
       name), workspace mode, `max_attempts = 1`, wall/idle from policy
       ceilings, correlation + idempotency per the scheme, task text from
       `atlas_plan_compose_planner` (or `_retry` when a stored refusal exists).
       Drive it: `drive_workspace_job`; on SUCCEEDED → `plan_revision_add`;
       format refusal → remember `atlas_plan_refusal`, loop (next k, bounded by
       5); job FAILED → next k (bounded); budget gone → report (state derives
       BLOCKED).
     - EXECUTING: find the lowest stage with unfinished work. Ensure its tree
       job is submitted as a **run root**: driver `claude-repo` (or opts'
       executor tree driver for tests = `fake-repo`), merged validations from
       the task row, `max_attempts = ATLAS_ORCH_RUN_MAX_WORKER_STARTS`,
       `run_max_parallel` = plan's, task text via `atlas_plan_compose_executor`.
       Ensure the stage's side jobs are submitted with `parent = tree job` (join
       the run; driver `claude` / `fake` for tests; `max_attempts = 1`; executor
       composer). Then drive: `drive_run(run_uid)`; after it returns, any side
       job still QUEUED → `drive_workspace_job` each in key order (the
       single-process progress guarantee; genuine overlap comes from operator
       dispatcher processes, the A11.6 residual unchanged). Stage run ACCEPTED →
       loop (next stage or COMPLETED). BLOCKED → loop (state derives
       NEEDS_REPLAN or BLOCKED).
     - NEEDS_REPLAN: submit planner job k+1 with `atlas_plan_compose_replan`
       (inputs from the state + the blocked run's failed-gate evidence via
       `job_get`), reason REPLAN, then as PLANNING.
     - COMPLETED / BLOCKED: fill report, return.
  3. Iteration ceiling: `o->max_iterations ?: (5 + 3*4*(3+3)) + 8` — a defect
     guard above the stated cost ceiling, the rundriver's pattern.
- `BUSY` from any submit: report `busy`, return cleanly (retryable invocation,
  the A11.1 contract). Transport hiccups: already survived by T1 under
  `drive_run`; the plan driver's own calls go through the same retry predicate.

**Steps:**
- [ ] Header + skeleton compiling with a failing-by-construction smoke test that
  calls `atlas_plandriver_run` with an empty transport and asserts the usage
  refusal sentences (no fixture yet — T8 carries the real coverage).
- [ ] Implement the loop against the transport seam.
- [ ] `ctest -L unit` stays green; commit
  `feat(a12): the plan driver - a foreground loop that walks a plan's stages through ordinary runs`.

### Task T7: CLI, service and renderers

**Files:** new `src/core/service_plan.c` (add to `atlas_core`); modify
`src/cli/cli.c` (dispatch, flags, help, `COMMANDS[]`), `src/cli/render.h`,
`src/cli/render_human.c`, `src/cli/render_json.c`.

**Interfaces produced:**
- `atlas plan run --repo NAME --goal TEXT --gate CMD [--gate ...]
  [--parallel N] | --resume p<uid>`; `atlas plan status P`;
  `atlas plan show P --rev N`; `atlas plan list`. Goal also accepted as
  `--goal-file PATH`? **No** — YAGNI; `--goal TEXT` only, ≤ 16384 bytes,
  refused not truncated.
- `atlas_service_plan_run` mirrors `atlas_service_job_run`: local refusals
  (goal xor resume; ≥1 gate unless resume; parallel 1..8; refuse
  resume+goal/gate/parallel), policy load + ENABLED, wires the production
  transport (plan.* + job.* IPC calls, `drive_run` → `atlas_rundriver_run` with
  the existing xport, `drive_workspace_job` → the T6 dispatch seam), runs
  `atlas_plandriver_run` in the foreground, logs to stderr under `--json`.
- `atlas_service_plan_status/show/list` are thin `plan.get`/`plan.list` reads.
- Renderer: one vtbl method `plan_item(const atlas_plan_render*)` (the
  `job_item` philosophy: one method, list row vs detail by depth). Human
  detail prints: plan, repo, status, revision r of 3, planner jobs k of 5,
  `goal (untrusted, atlas-safe-1)`, per-task lines
  `stage N task <key> [tree|side] <job state> [run r... <status>]`, per-task
  model+cost when usage exists, and for `show` the revision content under a
  `plan document (untrusted, atlas-safe-1)` label. JSON mirrors, with
  `goal_encoding`/`content_encoding` labels; run/job fields omitted when empty,
  never empty-string. **Both initializer lists get the method at the same
  offset; grep both files' initializers side by side in review.**
- Help text + `COMMANDS[]` + run the built binary once:
  `./build/atlas plan` must print usage, and `./build/atlas plan run` with
  missing args must exit 2 — the A9.2 lesson.

**Steps:**
- [ ] Failing test: extend `tests/test_plugin.c`-style CLI smoke or the service
  tests minimally — at minimum a direct binary invocation in `make smoke`'s
  script if that is where CLI surface is asserted (executor: check
  `scripts/smoke.sh` and add `plan` usage-exit checks there).
- [ ] Implement all five wiring places + renderers.
- [ ] `make && ./build/atlas plan; ./build/atlas plan run; ./build/atlas plan list --json` against a temp data dir; `ctest -L unit`.
- [ ] Commit `feat(a12): atlas plan run, status, show, list - one service result, two renderers`.

### Task T8: integration tests — the whole lifecycle on fake drivers

**Files:** new `tests/test_plan_driver.c` (labels: `integration`; if it forks
the daemon, `daemon`). Fixture pattern: `fx_open`, real git repo, fixture DB or
forked daemon per what `test_orch_driver.c` does for the full pipeline.

**Test obligations (each its own test fn):**
- [ ] **Happy path**: goal → `fake-plan` planner emits a 2-stage plan (stage 1:
  tree task + one side task; stage 2: tree task) → runs execute under
  `fake-repo`/`fake` → both stage-runs ACCEPTED → derived status COMPLETED;
  assert revisions/tasks/jobs re-readable via `plan.get`; assert the tree
  jobs' validations = floor + planner additions in order; assert
  `fx_tree_digest` unchanged where it must be.
- [ ] **Parse-refusal retry**: first `fake-plan` artifact malformed (bad stage
  numbering) → refusal recorded, planner job 2 submitted with `_retry` prompt
  (assert the composed task text contains the refusal sentence + line) →
  second artifact valid → EXECUTING.
- [ ] **Replan on BLOCKED**: stage-1 tree task's gates fail through the
  follow-up chain (fake-repo + a `false` gate) → run BLOCKED → NEEDS_REPLAN →
  planner job with REPLAN prompt (assert failed-gate name + excerpt present) →
  revision 2 compiles → its stage runs → COMPLETED. Assert revision 2's rows
  and revision 1's both present (history immutable).
- [ ] **Budget exhaustion**: planner emits only invalid artifacts → after job 5,
  derived BLOCKED; `plan run` exits reporting it; no sixth submission
  (idempotency + derivation asserted).
- [ ] **Restart survival**: kill the driver between planner success and
  revision_add; re-run `plan run --resume` → same revision lands exactly once
  (UNIQUE(plan_id, rev_no)); kill between tree submit and drive → resume
  re-claims, no duplicate job (idempotency key asserted by counting jobs with
  the correlation).
- [ ] **Parallel admission**: a stage with two side tasks and `--parallel 3` —
  assert both siblings + tree job active rows coexist (the
  `test_orch_parallel.c` two-leases assertion pattern), and the run settles
  only at quiescence.
- [ ] Commit `test(a12): the planned run proved end to end on fake drivers, restart and all`.

### Task T9: documentation and season rules

**Files:** modify `docs/orchestration.md` (new A12.0 section: the flow, the
format spec verbatim, the authority argument, the cost ceiling, the stated
costs), `docs/roadmap.md` (A12.0 section + pilot placeholder), `CLAUDE.md`
(season stack table row + "A12.0 — the planned run" rules section, one line
each; update the header paragraph), `docs/extending.md` (checklists: adding a
plan format field ⇒ new format version; adding a driver role; changing a plan
bound; adding a plan RPC method), `docs/backlog.md` (residuals: planner runs
stay ACTIVE; blocker-artifact fast-path; overlap needs dispatcher processes;
`--memory` not exposed on plan runs), `README.md` (status + one usage block),
and the phase constant (find it — commit b9ae766's diff shows where; likely
`include/atlas/version.h` or similar — set phase `A12.0`).

**Steps:**
- [ ] Write all sections; keep every number equal to the constants; quote the
  format spec from `plan.h`'s comment or this plan verbatim.
- [ ] `make test` targeted docs checks if any test scans docs (e.g.
  `test_decision_mcp.c` FORBIDDEN scan — confirm no new forbidden phrasing).
- [ ] Commit `docs(a12): the planned run written down - format, authority, costs and the season rules`.

### Task T10: live pilot A12-P, full suite, ship

Run by the planner/reviewer session (operator acts), not an executor subagent —
it touches the live machine.

- [ ] Full release suite once on the final tree: `make test` (plus
  `make smoke`); fix-forward loop via executors if anything is red.
- [ ] `atlas backup create` against the real data dir (operator).
- [ ] Install: `sudo make install`; restart the daemon (systemd unit on this
  machine — inspect `systemctl status atlasd`/user unit first); verify
  migration 25 applied (`atlas status` / schema version).
- [ ] Policy: append `driver = claude-plan`, `planner_model = fable`,
  `executor_model = opus` to `/etc/atlas/orchestration.conf` (root edit,
  **after** the new binary is installed — an old parser reads the new keys as
  MALFORMED and disables orchestration machine-wide).
- [ ] Pilot, frozen before launch: repo `atlas`, goal — a small real uncovered
  unit-test goal (exact goal text fixed at execution; the A11.6-P shape), floor
  gates `make` + a targeted `ctest -R`, `--parallel 2`. One restart mid-run is
  part of the protocol (kill the driver once, resume). Evidence to capture:
  `plan.get --json`, `orch_transitions` overlap windows, `orch_usage` model
  names per role (fable vs opus rows), gate runs from the driver log, terminal
  COMPLETED, and the accepted product reviewed before commit.
- [ ] Pilot record → `docs/roadmap.md` A12-P subsection + backlog residuals.
- [ ] Final commits; push once (`git push origin main`) — the user asked for
  commit/push status in the final report; push happens here and only here.
- [ ] Final report to the operator: what existed, what was added,
  planner–executor authority separation, live E2E evidence, test results, cost,
  residuals, commit/deploy/push + git status.

---

# Self-review (planner)

- **Spec coverage:** operator gives only goal+floor+bound (T7 CLI); Atlas calls
  planner (T6); plan durable+re-readable (T3/T5 `plan.get`/`show`); tasks with
  dependencies/parallelism/gates derived (T4 compiler → stages); executor
  workers via existing dispatch (T6); executor cannot change scope/plan
  (authority §; revision binding in T3); blockers → revisions, bounded (T6);
  Atlas manages retry/recovery/gates/evidence (existing machinery, reused);
  settlement only Atlas' (unchanged, plus derived plan status with no writer);
  restart-safe (correlation/idempotency scheme, T8 proves); roles not
  hard-coded (T2 policy); no new memory system, no GUI, no framework, no
  orchestration rewrite (absent by design).
- **Placeholder scan:** none found; every task names files, interfaces, tests.
- **Type consistency:** `atlas_plan_state`/`atlas_plan_doc`/refusal struct
  defined once (T3/T4 sections) and consumed by T5–T8 under those exact names.
- **Known open ends, stated:** exact `server_orch.c` vs `server_plan.c` split
  and the writer job-kind reuse are executor calls with the checklist named;
  `atlas_plan_refusal` durability across a crash is handled by re-deriving via
  a second `plan.revision_add` call (deterministic refusal).

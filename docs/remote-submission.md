# Remote submission: a job an operator submits from wherever they are (A14)

A14 is remote submission: a job submitted via a bearer credential the daemon
verifies itself, under bounds the policy sets and the request cannot. Its two
sentences:

> **THE GATEWAY CANNOT SUBMIT WORK BECAUSE OF WHO IT RUNS AS, AND THE OBVIOUS
> FIX IS TO STOP THAT BEING TRUE.**

> **A CREDENTIAL IN FLIGHT IS THE ONLY AUTHORITY THE GATEWAY EVER HOLDS, AND
> THE POLICY — NOT THE REQUEST — DECIDES WHAT A SUBMISSION MAY COST.**

## The honest paragraph, in the same breath as the capability

Remote submission reaches the operator's own account from wherever the bearer
credential can be presented — a browser on a phone, an external model's MCP
call. That account owns files, the daemon, and the policy files that govern
what runs. The gateway has no authority of its own; it is the carrier of a
credential the daemon verifies in the write transaction. The policy — not the
request — decides the driver, the mode, the gate floor, the per-credential
active bound and the daily bound. A task is a prompt. On this deployment it
runs as the operator's own account, with that account's files readable to it
and its edits accepted in a workspace; Atlas bounds how many tasks a credential
may queue and what checks they run under, not what one task may ask a worker to
read. Nothing in this surface applies a patch, changes a lifecycle state,
commits, pushes, accepts a run or approves a decision. A finished job's output
is a file read on the Atlas machine.

This channel is weaker than local submission in two ways: the credential
travels on a network the operator does not fully control, and Atlas cannot
verify what is in front of the gateway. The ledger records which credential
and which channel were used, not which person was present. `REMOTE_OPERATOR_CONFIRMED`
does not appear here: remote submission queues a job; the decision lifecycle is
not touched. What the channel establishes: a named credential was presented and
verified by the daemon in the same transaction that created the job, within the
bounds the root-owned policy states.

## 1. What is true today, measured against the tree at `3e91f1f`

**The four routes** added to `API_WRITE_ROUTES[]` in `src/gw/gateway.c`:

| Route | Method | Scope |
| --- | --- | --- |
| `/api/v1/job/submit` | `job.remote_submit` | `ATLAS_SCOPE_JOBS_SUBMIT` |
| `/api/v1/job/get` | `job.remote_get` | `ATLAS_SCOPE_JOBS_SUBMIT` |
| `/api/v1/job/list` | `job.remote_list` | `ATLAS_SCOPE_JOBS_SUBMIT` |
| `/api/v1/job/cancel` | `job.remote_cancel` | `ATLAS_SCOPE_JOBS_SUBMIT` |

**The four MCP tools** added to `TOOLS[]` in `src/mcp/mcp_tools.c`, each
`remote_only = true` (absent from the stdio adapter, present only on `/mcp`):
`atlas_job_submit`, `atlas_job_status`, `atlas_job_list`, `atlas_job_cancel`.
They carry `ATLAS_SCOPE_JOBS_SUBMIT`, which is not grantable through
`atlas api-key create`. The bearer travels on the server struct for one
exchange and is wiped at teardown; no schema declares `token`.

**The three conditions** under which the remote-submit method group is offered
(checked in `atlas_server_remote_submit_offered`):

1. `atlas_server_peer_is_gateway(ctx, peer_uid)` — the gateway uid from
   `SO_PEERCRED` against the root-owned policy.
2. `ctx->gwpolicy.remote_submit_count > 0` — at least one submit key named.
3. `ctx->gwpolicy.tls_mode == ATLAS_GWPOLICY_TLS_REVERSE_PROXY` or
   `ctx->gwpolicy.cleartext_submission_accepted` — TLS in front is the shape
   the gate prefers; the acceptance key is this deployment's written departure.

Every other peer, and the gateway peer under any other policy, gets `unknown
method`. `require_submitter` is never called on this path; `src/ipc/server_orch_remote.c`
never consults `atlas_orchpolicy_permits_submitter`. The credential is the
authority, not the uid.

**The eight policy keys** in `/etc/atlas/gateway.conf`:
`remote_submit_key` (repeatable, ≤ 4), `remote_submit_driver`,
`remote_submit_mode`, `remote_submit_gate` (repeatable, ≤ 8, ≥ 1),
`remote_submit_max_attempts`, `remote_submit_max_active`,
`remote_submit_max_per_day`, `operator_accepts_cleartext_submission`.
All eight submission lines must be present together or none; MALFORMED if any
appears alone. Editing them means restarting both the daemon and the gateway.

**The two columns** migration 32 added: `submit_key_id TEXT NOT NULL DEFAULT ''`
on `orch_jobs` and `key_id TEXT NOT NULL DEFAULT ''` on `orch_transitions`.
Empty string means a local job. Written by the write point from its own
verification, never from a parameter.

**The scope** `jobs:submit` (`ATLAS_SCOPE_JOBS_SUBMIT`): `grantable = false`,
derived by the daemon for exactly the credentials `remote_submit_key` lines
name. `atlas api-key create --scope jobs:submit` is refused.

## 2. The decisions and their chains

**Decision 1 — its own scope.** Submitting work is a different capability from
disposing of a record. `jobs:submit` is in `SCOPES[]` with `grantable = false`;
it is derived for named keys only; `remote_submit_key` and `remote_dispose_key`
may never name the same id. Decision 6's three reasons apply here: the gateway
policy file is the right place for submission keys because its stated purpose
is to constrain the gateway's principal, the daemon already loads it alongside
the orchestration policy, and a malformed gateway policy disables the gateway
and nothing else — not all orchestration.

**Decision 2 — credential verified by the daemon, inside the transaction for
writes.** Three copies of one check exist and the reason for each is written at
its head: `atlas_orch_remote_verify` in `src/orch/remote.c`, called from
`src/ipc/server_orch_remote.c` (which passes the bearer from the op), and from
the two write methods inside `op_submit` and `op_cancel`. A key revoked between
`gateway.auth` and the writer's turn queues nothing.

**Decision 3 — the audit row names the key, never a claimed value.**
`gw_audit.key_id` is the verified selector; `orch_jobs.submit_key_id` and
`orch_transitions.key_id` are written by the write point; `submitter_uid` stays
`SO_PEERCRED`'s answer; no audit row holds task text or a token.

**Decision 4 — a request that names a policy-owned field is refused, never
ignored.** Driver, mode, gate floor, attempts, both budgets and every
`dispatch.` name are absent from the request vocabulary. A request naming any
of them is refused: `a remote submission names the repository, the task and an
idempotency key; %s is decided by the root-owned policy`.

**Decision 5 — a budget per credential, checked in the transaction.** Two
bounds: active jobs (at this moment) and root submissions per UTC day. Read
from root-owned lines, counted from stored rows inside `op_submit`, refused
with the count and the bound named, never clamped. A duplicate idempotency key
spends no quota. `atlas api-key revoke` takes effect on the next transaction
verification.

**Decision 6 — policy file placement.** Three sufficient reasons: the file's
stated purpose constrains the gateway's principal and a credential is exactly
that principal; the daemon loads it at start alongside the orchestration policy
(`daemon.c:224` and `:212`); a malformed gateway policy disables the gateway
alone, while a malformed orchestration policy disables all orchestration.

**Decision 7 — no remote read of a worker's output.** No artifact, no log, no
gate output travels a remote route or tool; state, reason, attempts and cost
do. The transcript chain that this does not close: a credential holder can read
`job.remote_get`'s `reason` and `usage` fields but never the worker's prose.
The chain is this season's stated cost; no fix is offered.

**Decision 8 — what the gateway cannot do stays true because of who it runs
as.** The gateway's uid reaches four new names only when a root-owned policy
names a credential, and each of those names does nothing without that
credential verified by the daemon. `submitter_uid` in `orchestration.conf` is
never the gateway's; `require_submitter` is never on the remote path. No check
in `src/gw` is the boundary; a bug there cannot make the daemon offer a job
method to the gateway for free.

**Decision 9 — idempotency namespace.** `remote.<key_id>.<client>`, built by
`atlas_orch_remote_idempotency_key` (`src/orch/remote.c`), is the only
namespace for remote submissions. The client part is at most 40 bytes; the
whole fits `ATLAS_ORCH_NAME_MAX`. Namespacing by key id means two credentials
cannot collide even if they use the same client-supplied key.

**Decision 10 — four MCP tools, remote-only, absent from the stdio adapter.**
A local Claude session has a shell and `atlas job submit`; a remote-only tool
that reached `job.submit` as the operator's uid from inside a stdio session
would be a second submit surface with no gate floor. The bearer reaches the
four tools through `s->remote_token`, set for one exchange and wiped at
teardown. This season's MCP audit cost: every remote `tools/call` audits as
`operation = "mcp"`, not by the tool name — `docs/backlog.md` carries it.

**Decision 11 — a second written acceptance, its own chain.** 
`operator_accepts_cleartext_submission = yes` is MALFORMED under
`tls_mode = REVERSE_PROXY` and MALFORMED without a submit key. The disposal
acceptance implies nothing about it: a submission starts a worker that runs as
the operator's account, which is a different consequence from moving a record.

**Decision 12 — seven keys refused rather than clamped, cross-checked at
submit.** The gateway policy names a driver and a mode the orchestration policy
is the authority on; the daemon cross-checks at every submit and logs at start.
`atlas gateway status` says what the gateway policy states and cannot know what
the orchestration policy states, and its `submit:` line says `(checked at
submit)`.

**Decision 13 — the operator's terminal sees, cancels and drives remote jobs.**
`job.list --remote`, `job.get`, `job.cancel` and `job.run_status` widen by a
disjunct: a job whose `submit_key_id` is non-empty is visible to the operator
peer. `peer_is_operator` is derived from `SO_PEERCRED`, never from a parameter.
Provable at the write point in process and on the deployment; the fixture limit
means `tests/test_orch_remote.c` drives `atlas_orch_apply` directly.

**Decision 14 — every remote submission creates a run, and what that run does
is the driver's existing behaviour.** Under `claude` the run is workspace-rooted
and never settles — A12.0's stated cost for planner jobs, produced on purpose
for every unattended remote job. `run: ACTIVE` beside a terminal job state is
not a claim; it is A11.0's semantics unchanged. Under `claude-repo` the run is
a real A11.1 run the operator drives. Neither is changed by this season.

**Decision 15 — two injection channels, never a flag or path override.** Daemon
method tests use the in-process edge. HTTP tests use a real daemon with
`atlas_daemon_opts.orchpolicy_text`, a third argument to `atlas-gw-daemon`. The
orchestration policy injected names the test's own uid as `model_dispatcher_uid`
and `fake` as the only driver, so a remotely submitted job is leased and
completed by the test with no model, no network and no money.

**Decision 16 — Mission Control's Jobs view.** A new `jobs` entry in `VIEWS`;
a password field for the submission key (memory-only, per A16's answered row 3);
a repository picker from `loadRepos()`; a task textarea bounded at 65536; a
`Submit` button that POSTs through `apiWrite` with `credentials: "omit"`; a
job list with state, driver, created time, attempts and cost; `Cancel` while
non-terminal. The fixed sentences are in `docs/plans/2026-09-04-remote-submission.md`
§Frozen formats; the cleartext sentence appears when `/auth/me` reports
`cleartext_submission: true`.

**Decision 17 — audit, log and status say what happened without saying more.**
Every bearer-table request appends its `gw_audit` row through `audit()`. The
daemon logs one safe-encoded line per remote submit, cancel and refusal, naming
the key id, the repository, the job uid and the driver — never the task text.

## 3. The two submission shapes

| Shape | Driver | What happens |
| --- | --- | --- |
| Unattended | `claude` | A model dispatcher running as the operator's account leases and completes the job; the run never settles; the operator reads the patch with `atlas job artifact`. |
| Deferred | `claude-repo` | The task sits queued, pinned to the commit the repository was at on arrival; nothing is spent until the operator runs `atlas job run --resume RUN`; if the repository moved first, the task is refused at lease as pinned to a moved tree. |

**This deployment runs the unattended shape.** `remote_submit_driver = claude`,
as the operator answered 2026-09-04: *"başlasın"* (let it start). A background
model dispatcher process runs as the operator's account and leases every model
job the queue holds.

## 4. Stated costs

**Three copies of one credential check** (Decision 2): `atlas_orch_remote_verify`
is called from the gateway-forwarding code, from `method_remote_submit` and from
`method_remote_cancel`. Each carries its reason at the head. This is the residual
`docs/backlog.md` records; the cost of eliminating it is a new shared-state path
that could miss a revocation between calls.

**Remote `tools/call` audits as `mcp`** (Decision 10): the audit row for every
remote MCP call records `operation = "mcp"`, not the tool name. A reader of the
audit trail sees that a model MCP call happened; they do not see which of the
four tools it reached. This is a stated cost, not an oversight; fixing it would
thread the tool name back through `mcp_exchange`, and no task in this plan did
it.

**Gate additions from a remote request are refused** (Decision 4): a requester
cannot add a gate; the operator's `remote_submit_gate` lines are the floor and
the ceiling. The residual is that the floor is fixed per policy per key; per-key
differentiation is `docs/backlog.md`'s entry for per-key drivers.

**The daily bound is keyed on a UTC clock** (Decision 5): `remote_today` is
counted as rows submitted since midnight UTC. A credential presented at 23:59
and again at 00:01 spends from two budgets; this is correct and deliberate.
`docs/backlog.md` records it.

**The two policy files can disagree** (Decision 12): the gateway policy names
a driver and a mode the orchestration policy is the authority on. The daemon
cross-checks at every submit and logs a disagreement at start; `atlas gateway
status` cannot know the orchestration policy's contents and says so. If the
gateway policy and the orchestration policy are not in agreement, every remote
submission is refused until one of the two files changes. Editing either file
requires restarting both the daemon and the gateway.

**The never-settling workspace run** (Decision 14): a job submitted under
`claude` creates a run with no settler. The run stays ACTIVE indefinitely after
the job ends; `job.run_status` reports it. This is A12.0's own stated cost for
planner jobs, produced on purpose here; it is not a defect and there is no fix
in this plan.

**The deferred shape's pin** (Decision 14): a `claude-repo` job is pinned to
the commit at submission time. If the repository receives a commit before the
operator drives the job, `atlas job run --resume RUN` fails the pin check and
the task must be resubmitted.

**The transcript chain** (Decision 7): a credential holder cannot read a
worker's prose output, patch, log or gate output remotely. They see state,
reason, attempts and cost. The path from a finished job to its output is the
Atlas machine and the terminal command `atlas job artifact`. No fix is offered
in this season; `docs/backlog.md` carries it.

## 5. The cleartext chain, verbatim

**On this deployment a submission credential presented by a browser travels in
the clear.** The gateway listens on `192.168.0.198:8799` with `tls_mode = NONE`,
and the four submission routes carry the credential as a bearer header on every
request, so anyone able to observe traffic on that network segment can read it.
An Atlas API credential has no expiry, so a credential captured once queues
work — a worker that runs as the operator's own account, within the policy's
daily bound — until the operator notices and runs `atlas api-key revoke`. The
credential the MCP tunnel presents does not cross that segment: the tunnel
client runs on this host and posts to this host's own address, so its exposure
is the file it is read from in the operator's home directory — readable by
exactly the account a remotely submitted worker runs as — and the far side of
the tunnel. The operator was shown this chain on 2026-09-04 and accepted it for
this network by writing `operator_accepts_cleartext_submission = yes` into the
root-owned gateway policy; `atlas gateway status` prints that acceptance on
every run. Atlas states this cost and does not judge the trade; the same key on
a listener reachable from a network the operator does not control is a different
decision using the same mechanism.

### The operator's answers, quoted and dated

All three rows from `docs/plans/2026-09-04-remote-submission.md` were answered
before T1 was dispatched, on 2026-09-04.

- **Row 1 — which driver.** `remote_submit_driver = claude` (start while away).
  The operator's words: *"başlasın"*. A background model dispatcher runs as
  their account and leases every model job the queue holds.
- **Row 2 — cleartext accepted.** `operator_accepts_cleartext_submission = yes`.
  The operator's words: *"evet şu anda olabilir ileride bunu daha güvenli hale
  getiririz"* — yes for now, we make it more secure later. Recorded as a
  decision with an intent attached, not as a permanent shape. The chain above is
  unchanged and still true; the key stays a written acceptance that `atlas
  gateway status` prints, so the day they change their mind the line comes out
  and the season's own gate refuses again.
- **Row 3 — which credentials and which numbers.** Both keys as the plan
  proposed: `key_b2578f48143c06d3` (`chatgpt-tunnel`, the model) and one new
  `--no-scopes` key minted for the browser — never the `mission-control` login
  key, so a session cookie and a submission credential remain different secrets.
  Numbers: `remote_submit_mode = patch`, gate floor `make`,
  `remote_submit_max_attempts = 1`, `remote_submit_max_active = 2`,
  `remote_submit_max_per_day = 6`.

## 6. What execution established that the plan did not claim

*(T11 records go here after the live acceptance.)*

## 7. The live acceptance, 2026-09-05 — what one pass observed

Run on this machine, gateway at `192.168.0.198:8799`, `tls_mode = NONE`, policy
as row 3 answered it: two credentials named (`key_b2578f48143c06d3`, the
tunnel's; `key_01364e94e1dcbad4`, minted `--no-scopes` for the browser),
`mode = patch`, gate floor `make`, `max_attempts = 1`, `max_active = 2`,
`max_per_day = 6`, and `operator_accepts_cleartext_submission = yes`.
**Nothing about one live pass is a general result**; every line below is an
observation of one run.

**The mechanism the season exists for worked.** A `POST` to
`/api/v1/job/submit` carrying only a bearer credential queued a job: the
response named the job, its run, `driver claude`, `key_id 01364e94e1dcbad4`
and the budget it had just spent (`active 1/2, today 1/6`). `atlas job list
--remote` at the terminal showed the same row with `credential:
key_01364e94e1dcbad4` beside it. A second submission under the same
idempotency key returned the same job with `duplicate: true` and wrote no
second row. A bearer that authenticated as no credential was refused `401`
before any daemon call. The gateway acquired nothing: the row records the
kernel's peer uid *and* the credential the daemon verified for itself.

**Four observations the tests could not have made, each a real interaction
this deployment has and the suite does not:**

1. **A submission lands during semantic maintenance; the dispatcher's next
   write may not.** Two deploys in succession left the daemon minutes deep in
   an unbounded semantic pass. A9.2.7's yield let `job.remote_submit` through
   — the job was queued — and then refused `dispatch.heartbeat` on one attempt
   and `dispatch.snapshot.open` on another. With `max_attempts = 1` a refused
   dispatcher write is a dead job: two free-driver jobs ended `FAILED` this
   way, ten minutes apart, for a reason that has nothing to do with the task
   they carried. The bound is doing what it was set to do; what this measured
   is that **`max_attempts = 1` and an unbounded writer are a bad pair**, and an
   operator choosing 1 for cost is also choosing "a busy daemon kills my job".
2. **The gate floor `make` cannot run under the system dispatcher at all.**
   `/etc/systemd/system/atlas-dispatcher.service` carries
   `SystemCallFilter=~@privileged`; GNU make calls `setresuid` (syscall 117),
   the kernel answers `SIGSYS`, and `make` dies with "Bad system call" before
   cmake has finished. Every job with a `make` gate leased by uid 993 fails its
   gate for this reason, and has since that unit was hardened — A14 only made
   it visible, because A14 is the first thing that ran a gated job through that
   dispatcher on this machine in a while. The model dispatcher
   (`~/.config/systemd/user/atlas-model-dispatcher.service`, uid 1000) carries
   no such filter, which is why the real job below reached its worker at all.
   Recorded in `docs/backlog.md`; the fix is one re-allowed syscall and it is
   not this season's.
3. **The one real job timed out with its work finished and unclaimed.** A
   `claude`-driver job against a `docs/backlog.md` item ran 15 minutes and
   ended `TIMED_OUT` — `max_wall_timeout_ms = 900000` exactly. The worker had
   made the change and was **running the repository's own non-daemon test
   suite** when the wall clock killed it (`exit 137`); that suite alone is
   about five minutes here, on top of a build. The workspace still held four
   modified files, so the spend produced a patch that Atlas never accepted:
   the gate never ran, no artifact was packaged, and the operator read the
   diff out of the workspace by hand. **A worker that verifies as thoroughly
   as this repository asks does not fit in fifteen minutes**, and `patch` mode
   plus `max_attempts = 1` gives it no second chance.
4. **A killed worker reports no cost.** `usage` came back `present: true,
   has_cost: false`, and the model's own final message — the one carrying
   `total_cost_usd` — is what the kill prevented. So a timed-out job spends
   money that no Atlas surface can name afterwards. The provider's own billing
   is the only record, and `atlas job get` saying nothing about cost must not
   be read as "nothing was spent".

**What was not exercised.** No submission was made from a phone browser or
from the external model over `/mcp` in this pass; both were made with the same
credentials from this host, which tests the routes and the write point and
says nothing about the two clients. The Jobs view was not opened. Those
remain to be observed.

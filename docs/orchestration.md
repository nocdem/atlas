# A8 — the durable orchestration control plane

Atlas A8 accepts a **bounded job**, records it durably before anything is
dispatched, hands it to exactly one unprivileged worker under an expiring lease,
survives a crash on either side, and keeps a complete auditable history of what
happened.

That is the whole claim. What follows says precisely what is implemented, what
each rule is for, and — in the last section — what is **not** implemented yet, so
that nothing here reads as a promise Atlas does not keep.

## What A8 is not

A completed job is **not** an authority.

* It approves nothing. A7's lifecycle authority is untouched: no orchestration
  method mints or spends a capability, and the dispatcher cannot reach one.
* It applies nothing. The patch a job produces is an artifact — bytes with a
  recorded name, size and digest — and there is no code path in Atlas that
  applies it to a registered repository.
* It commits, pushes, branches, merges and opens nothing. Those verbs do not
  appear in any method name, and `tests/test_orch_rpc.c` asserts that from both
  a client and a dispatcher connection.

Confusing "a model finished a job" with "a decision was approved" is the single
mistake this phase must never make, so the two vocabularies do not meet anywhere
in the code.

## The two principals

`atlasd` owns every orchestration row and is the only writer of one. It validates
a specification, persists the job, grants leases, enforces deadlines and
reconciles after a crash.

`atlas-worker` runs the dispatcher. It holds no database handle — not even
read-only — cannot open the index (0700 `atlasd`), and reaches Atlas only over
the A7.1 socket.

The split is enforced by the kernel first and by Atlas' checks second, which is
the ordering A7.1 established and A8 keeps.

## Fail-closed at zero

Every A8 enum keeps its unknown or refusing value at zero, for the reason A6
keeps `UNKNOWN` and `BLOCKED` there:

| Enum | Zero | Meaning |
| --- | --- | --- |
| `atlas_orch_state` | `UNKNOWN` | a row nobody wrote correctly; no transition leaves it |
| `atlas_orch_reason` | `UNKNOWN` | no reason recorded |
| `atlas_orch_actor` | `UNKNOWN` | nobody |
| `atlas_orch_exit_kind` | `UNKNOWN` | the driver said nothing usable |
| `atlas_orchpolicy_state` | `DISABLED` | orchestration is off |
| `atlas_orch_op_kind` | `NONE` | applies nothing |

A `memset` must not be able to produce a runnable job. The schema enforces the
same thing independently: every state `CHECK` omits `UNKNOWN`, so a persisted job
may never be in it.

## The job specification and its canonical identity

A specification is everything immutable that changes what was asked for. It is
canonicalised, then digested with `ATLAS_ORCH_SPEC_DOMAIN`, **domain-separated
and length-prefixed** — never delimited, for A4's reason exactly: with a
single-byte delimiter a mode of `a` with a driver of `b` would encode identically
to a mode of `a|b` with an empty driver.

### What the digest covers, and why

| Field | Covered | Reason |
| --- | --- | --- |
| `spec_version` | yes | a different encoding is a different request |
| `submitter_uid` | yes | two principals sending identical text are not making the same request |
| `repo_name` | yes | part of what was asked for |
| `repo_identity_hash` | yes | the durable identity; a path is chosen by whoever made the directory |
| `source_commit` | yes | exact and pinned; the tree the job was authorised over |
| `mode`, `driver` | yes | what will run, and how |
| `task_text` | yes | the request itself |
| `allowed_paths` | yes | a **set**: sorted and deduplicated, so order does not change identity |
| `validations` | yes | a **list**: order is part of what was asked for |
| every bound | yes | a job with a different timeout is a different job |
| `correlation`, `parent_job_uid`, `idempotency_key` | yes | caller-chosen identity |
| job id | **no** | assigned after the digest |
| `created_at`, `state`, `attempts_started`, `state_seq` | **no** | these change; the request does not |
| every lease, attempt, event and artifact | **no** | consequences, not the request |

Adding a field means adding a row here with a reason and bumping
`ATLAS_ORCH_SPEC_DOMAIN` — every stored digest means something different
afterwards, which is the point of it being in the string.

### What a specification may not contain

Refused at `atlas_orch_spec_validate`, with the bound named and nothing clamped:

* an arbitrary source path — a repository is *named* and resolved through the
  registry, so an unregistered directory cannot be reached at all;
* a moving branch — the commit is resolved and pinned from the index before the
  job is persisted, because a moving reference in a stored specification is a job
  whose source depends on when it happens to run;
* a relative traversal or a symlink escape in a declared path;
* a NUL in any field;
* an unlimited timeout, attempt count, output size or artifact budget;
* a client-asserted uid, role or authority — there is no such field.

**Shell syntax in task text is explicitly allowed**, and that is deliberate.
Nothing in A8 passes task text to a shell, and refusing a dollar sign would imply
the opposite. A validation *command* is a vector of counted arguments, never a
string: there is no field anywhere that could hold a shell fragment.

## The state machine

One explicit persisted machine. `atlas_orch_transition_allowed` is the single
authority on what may follow what, and it is a *function* rather than a table a
test can copy — `tests/test_orch_model.c` checks all 144 pairs against an
independently written table.

```
                    ┌──────────────────────────── retry ─────────────────┐
                    │                                                     │
  submit ──▶ QUEUED ──▶ LEASED ──▶ PREPARING ──▶ RUNNING ──▶ VALIDATING ──┤
               │          │            │            │            │        │
               │          └────────────┴────────────┴────────────┘        │
               │                       │                                  │
               │              CANCEL_REQUESTED                            │
               │                       │                                  │
               ▼                       ▼                                  ▼
          CANCELLED               CANCELLED                    SUCCEEDED / FAILED
          TIMED_OUT               TIMED_OUT                    TIMED_OUT
          FAILED                  RECOVERY_REQUIRED            RECOVERY_REQUIRED
          RECOVERY_REQUIRED
```

The load-bearing edges, and the edges that are deliberately absent:

* **`RUNNING → SUCCEEDED` exists; `LEASED → SUCCEEDED` does not.** A job cannot
  succeed before its driver has run. A job that declared no validation commands
  completes from `RUNNING`; one that declared some completes from `VALIDATING`.
* **`CANCEL_REQUESTED → SUCCEEDED` does not exist.** This is how "completion and
  cancellation cannot both win" is decided by the machine rather than by whichever
  message arrived first. A success reported after a cancellation settles as
  `CANCELLED`.
* **Nothing leaves a terminal state.** Checked centrally, so "a persisted
  completed job stays completed" is a property of the machine.
* **A self-transition is not a transition.** A no-op write is not a change.
* **`RECOVERY_REQUIRED` is not `FAILED`.** "We do not know whether this ran" and
  "this ran and failed" are different answers, and collapsing them is the mistake
  A6 refuses to make about ancestry.

### Invariants

* **Every state change is a compare-and-swap.** The `UPDATE` names the state it
  observed and requires that exactly one row changed, so a concurrent transition
  loses deterministically instead of last-write-wins.
* **Ordering is the ledger's `AUTOINCREMENT` id, never a timestamp.** Two events
  in the same millisecond are ordered; a clock that steps backwards cannot
  reorder history. Timestamps are evidence. The one decision that genuinely is
  about time — has this lease expired — uses one.
* **`orch_transitions` is append-only.** No `UPDATE`, no `DELETE`, no `_clear`.
* **Attempt numbers are monotonic**, derived from the job's own counter rather
  than from `count(*)`, and `UNIQUE(job_id, attempt_no)` makes a duplicate
  impossible.
* **At most one unreleased lease per job**, as a partial unique index rather
  than as care. That is the whole concurrency guarantee, in the shape A4 uses for
  "at most one approved revision per document".

## Lease, heartbeat and recovery

A lease token is 32 bytes from `/dev/urandom`, handed to the dispatcher once at
grant and **never stored** — only a domain-separated digest of it is, so a leaked
database row cannot be presented as a capability. There is no fallback to a
weaker source: a machine that cannot produce randomness refuses to create jobs.

* A lease is short (`ATLAS_ORCH_LEASE_MS`) and renewable, because a long lease is
  a long window in which a dead worker's job is not retried, and an unrenewable
  one makes a slow but healthy job indistinguishable from a dead one.
* Renewals are bounded, so a wedged worker cannot hold a job forever by
  heartbeating. The wall deadline is the separate bound that finally stops it.
* A worker is identified by its **token and nothing else**. Its claimed pid is
  recorded as its claim and used for nothing that matters; a worker describing
  itself is not evidence about itself.
* An expired or released lease can do nothing. That is what stops a process
  declared dead, whose job was retried, from overwriting the newer attempt's
  result.
* A worker learns of a cancellation **at its next heartbeat**. There is no signal
  from the daemon to a worker process and there must not be one: the daemon has
  no path into the worker's process tree, which is the isolation A8 is for.

Recovery is driven by the daemon's own timer and by startup reconciliation.
Nothing outside Atlas can ask for it, so one worker cannot cause another's job to
be expired. On an expired lease it releases the lease, ends the attempt as
`TIMED_OUT`, and then:

| Condition | Outcome |
| --- | --- |
| cancellation was requested | `CANCELLED` |
| past the wall deadline | `TIMED_OUT` |
| attempts remain | `QUEUED` — a new attempt and a new lease |
| attempts exhausted | `RECOVERY_REQUIRED` |
| the job already ended | lease released, outcome untouched |

Recovery is idempotent, and a completed job survives any number of sweeps
unchanged.

**The sweep runs on the watcher's timer**, every 20 s, on a daemon serving the
system index under an active policy and on no other. Nothing outside Atlas can
ask for it, which is what stops one worker expiring another's job — and also
means it runs if and only if the daemon calls it. It did not, for the whole of
A8's first deployment: `op_recover` was implemented, documented as timer-driven
and exercised by four tests, and never wired to a caller. A job whose worker lost
its lease sat in `PREPARING` indefinitely, which is how it was found. Every
recovery behaviour in the table above depends on that one call.

Heartbeats keep a lease alive during *every* long phase, not only while a driver
is running: the snapshot transfer heartbeats on the same throttle, because
copying a large tree is progress and a lease that expires mid-copy loses the
attempt.

## The RPC surface, and the line through it

Two groups, selected by the peer's uid from `SO_PEERCRED` and by nothing else:

| Group | Methods | Reachable from |
| --- | --- | --- |
| `job.` | `submit`, `get`, `list`, `cancel`, `artifact` | a uid the policy lists as a submitter |
| `dispatch.` | `lease`, `heartbeat`, `event`, `complete` | a uid the policy names as a dispatcher — `dispatcher_uid`, or A8.1's `model_dispatcher_uid` |

A `dispatch.` name is simply **not found** for a peer the policy does not name —
the same answer as for a name that does not exist. A refusal distinguishing "you
may not" from "there is no such thing" would tell a caller what to try next. A
`job.` name is always found and refuses with *orchestration is not enabled*: a
submitter learns nothing it could not learn by reading the root-owned policy
path, and the honest answer is what lets an operator tell a disabled policy apart
from a binary too old to have the method.

The two sets are consulted **additively**, so a uid that is both a submitter and
a dispatcher holds both. A8.1 made that case real — the operator's account
submits jobs and runs the model dispatcher — and an either/or lookup silently
took `job.submit` away from it. Routing is not authorisation: every method still
runs its own `require_submitter` or `require_dispatcher`.

### Why a dispatcher tier is not a privileged tier

A7.1's `syspolicy.h` says the socket carries no authority, and it still does not:
the lifecycle, registry, backup, restore and maintenance methods **do not exist in
the protocol**, so there is nothing on this socket for a privileged tier to
unlock. The `dispatch.` group is *disjoint* from the client group rather than a
superset of it. It concerns leases, heartbeats, events and results for jobs an
operator already created; it can neither create work nor read another principal's;
every one of its methods additionally requires a bearer token, so the uid is
necessary and never sufficient; and membership is a root-owned configuration fact
that neither `atlasd` nor `atlas-worker` can write.

Client reads are scoped to the calling principal. Whether another principal's job
exists is itself information, so a caller who may not act on it is told "no such
job" rather than "forbidden".

## The orchestration policy

`/etc/atlas/orchestration.conf`, a compiled-in path with no environment override
and no flag — the rule `ATLAS_AUTHORITY_POLICY_PATH` and `ATLAS_SYSPOLICY_PATH`
follow, for the same reason: a caller that can choose the policy has written the
policy. It is reached through `atlas_rootpath_open`: from `/`, no symlink
traversed, every component owned by uid 0 and writable by nobody else.

| Key | Meaning |
| --- | --- |
| `dispatcher_uid` | the one uid that may act as the dispatcher |
| `submitter_uid` | repeatable; who may create, cancel and read jobs |
| `repo` | repeatable; registered repository names that may be snapshotted |
| `mode`, `driver` | repeatable; the vocabularies a job may name |
| `max_*` | ceilings, which may only **lower** the compiled-in absolutes |
| `worker_root` | where the dispatcher owns its workspaces |
| `live_model` | whether a driver that calls a live model may run at all |

Anything missing, malformed, symlinked, group-writable or non-root-owned leaves
orchestration **disabled** with a reason. An unrecognised key is an error, not
something skipped — A7.1's rule, for A7.1's reason: a policy Atlas half-understands
is one whose author believes they configured something Atlas never read, and one
day that something will be a restriction.

The dispatcher may not also be a submitter. Keeping them disjoint is what makes
"a client cannot forge a dispatcher message, and a dispatcher cannot create its
own work" a property of the configuration rather than a hope about it.

## Schema 8

Eight tables, all **canonical** and none prunable. Nothing rebuilds an
orchestration row: the repository never held what was asked for, who asked, what
was granted, what ran or what came back. `RETENTION[]` carries a written reason
for each.

| Table | Holds |
| --- | --- |
| `orch_jobs` | the specification, its digest, the submitter, the current state |
| `orch_attempts` | one row per execution attempt, and how it ended |
| `orch_leases` | the exclusive right to execute, as a token digest and an expiry |
| `orch_transitions` | the append-only ledger, and the ordering authority |
| `orch_events` | what the worker reported, recorded as the worker's claim |
| `orch_artifacts` | the artifact manifest: name, size and digest |
| `orch_idempotency` | the key-to-job mapping that makes resubmission replay-safe |
| `orch_observations` | the worker's own account of its phase, on each change |

`orch_jobs.repo_id` is a **soft reference with no foreign key**, exactly like
`decision_documents.repo_id` and for the same reason: an FK would make `repo
remove --yes` destroy execution history. Because `repositories.id` is a reused
rowid, the pointer is cleared inside `atlas_db_repo_remove`'s transaction by
`atlas_db_orch_forget_repo`. `repo_identity_hash` stays — it is what the history
is *about*.

Migration 7 → 8 is atomic, idempotent as a set, creates no rows, and leaves every
pre-existing table byte-for-byte identical. `tests/test_migrate8.c` proves all
four, and proves that a failed migration 8 leaves a schema-7 database untouched
and still upgradable.

## Extending A8 safely

* **A new state** means editing `atlas_orch_state`, the schema `CHECK`s on
  `orch_jobs.state` and `orch_attempts.state`, `atlas_orch_transition_allowed`,
  and the enumerated table in `tests/test_orch_model.c`. The transition table is
  a *function* precisely so a test cannot pass by agreeing with a second copy.
* **A new reason or actor** means a member, a case in its name function, and a
  row in the ledger vocabulary. Keep `UNKNOWN` at zero.
* **A new operation that must be atomic with something else** uses
  `atlas_orch_apply_in_tx` and owns the transaction itself. Adding a second
  *implementation* is what the rule forbids.
* **A new policy key** means a branch in `atlas_orchpolicy_load_at`, a field, a
  line in the template explaining it, and a case in the malformed matrix. An
  unknown key must stay an error.
* **A new RPC method** goes in one of the two tables in `server_orch.c`, and the
  choice of table is the security decision. If it is plausibly an authority or
  mutation verb, add its name to the negative enumeration in
  `tests/test_orch_rpc.c` so the list keeps pace with the vocabulary.
* **A new bound** goes in `atlas/orch.h` with a written reason, and must refuse
  rather than clamp.

## The worker: workspace, execution and drivers

### Layout

Every attempt gets `<worker_root>/jobs/<job-id>/<attempt>/` containing
`spec.json`, `source/` (pristine), `work/` (writable), `driver/`, `logs/`,
`tests/`, `artifacts/` and `events`. Every component of that path is chosen by
Atlas — a validated worker root, an Atlas-generated job id and an integer — so
"a job cannot reach another job's workspace" is a property of construction. The
one function that writes into a workspace refuses absolute paths, traversal and
NULs, and every descent uses `openat` with `O_NOFOLLOW` from a descriptor
validated once.

### The daemon reads; the worker receives

**The worker never opens a registered repository.** A8's first cut had it read
the repository itself, which was wrong twice over: it failed in practice — git
refuses a repository owned by another uid, and A7.1 deliberately splits the
principals — and it was wrong in principle, because it required the untrusted
account to hold a read path to `/opt`.

So the direction is inverted. `atlasd` resolves the repository from its own
registry, validates that the pinned commit belongs to it, enumerates the
committed tree, and streams a canonical bounded snapshot to whichever worker
holds the current valid lease. The dispatcher unit sets
`InaccessiblePaths=/opt` — the repositories are not merely read-only to the
worker, they are absent.

| | |
| --- | --- |
| Protocol | `ATLAS_SNAPSHOT_PROTOCOL` = 1 |
| Methods | `dispatch.snapshot.open`, `dispatch.snapshot.chunk` (dispatcher-only, lease-bound) |
| Chunk | 256 KiB of content, hex-encoded on the wire |
| Bounds | 8 MiB per file, 20 000 entries, 256 MiB total |
| Identity | domain-separated, length-prefixed digest over the *manifest* |

Neither method accepts a repository, a commit or a path: the lease token is the
whole request and the daemon resolves everything else from persisted state.
There is no field that could carry a host path, so "the worker cannot supply or
replace the repository" is a property of the signature rather than a check.

The manifest is **persisted**, so a dispatcher that restarts mid-transfer resumes
against the same snapshot identity — a re-enumeration could legitimately differ,
and a worker holding the first half of one tree and the second half of another
would have something that never existed. No content is stored: SQLite is Atlas'
rebuildable index, and repository bytes in it would make it something else.

What arrives is verified rather than trusted. Every path is re-checked before it
is materialised, every entry's content digest is recomputed from the bytes that
landed, the offset answered must be the offset asked for, and the snapshot digest
is recomputed from the manifest the worker itself assembled. A stream that lost,
duplicated or reordered an entry cannot produce a match, so a partial
materialisation is never accepted as complete.

### The snapshot has no git metadata at all

There is no `.git` under an attempt, so:

* there is no repository configuration to be hostile;
* there are no hooks, because there is no hook directory;
* there are no alternates, so nothing can write objects into the source;
* there is no index or lock, so nothing under a registered repository is touched;
* **submodules and LFS are not "disabled" — they are absent.** A gitlink entry is
  refused at listing time and no machinery exists to act on one.

A file **larger than the per-file bound is refused and counted**, from the
listing, before the object is ever opened. `git ls-tree -l` reports the size, so
the decision is made without reading the blob; the read bound stays as a second
layer. This is not truncation and must not be described as one: the file is
excluded whole, counted, and named in the snapshot event beside the symlink and
gitlink counts, so a driver sees a work tree missing it and an operator is told
why. The alternative — raising the bound until one repository's largest file
fits — buffers that file in the daemon and moves a limit to make a run pass. A
real 68 MB test vector is what produced this: before it, one oversized file
aborted the entire snapshot and reported a child-process output error rather
than a fact about the tree.

A tracked **symlink is refused and counted**, never recreated: a materialised
link is a path that leaves the workspace the moment it is followed. A **gitlink
is refused at enumeration**, so submodules are absent rather than disabled. Two
copies are written rather than a link, so a driver's edit cannot rewrite the
pristine side and make the patch look empty.

The commit must be an exact object id, and it is resolved *against the repository
the registry named* — a commit that exists elsewhere is not this project's
history. A branch, a short id, a commit that no longer resolves, a repository
that is no longer registered and a repository whose durable identity has changed
are all refused.

### The trusted repository-access layer

One layer, used by repository scanning, pinned-commit validation, tree
enumeration and snapshot production alike — there are not two git trust
implementations to drift apart.

Every repository invocation carries `-c safe.directory=<canonical root>`, built
from the path Atlas resolved from its own registry. Never from request text, a
worker message, an environment variable or any configuration file: global and
system config remain unread (`GIT_CONFIG_GLOBAL` and `GIT_CONFIG_SYSTEM` are
`/dev/null`, `GIT_CONFIG_NOSYSTEM` is 1), so an operator's or a repository's own
`safe.directory` still cannot influence anything. The declaration names exactly
one directory and git checks it exactly — a declaration for one path does not
admit another.

**This corrects a documented claim.** Atlas previously stated that git ignores
`safe.directory` from `-c`. Measured on git 2.39.5: the bare invocation refuses,
the `-c` invocation succeeds, and a `-c` naming a different directory still
refuses. The old claim had a real consequence — under A7.1 the daemon could not
open *any* registered repository, and served a scan taken before the cutover.

### The patch

`git diff --no-index --binary source work` — no repository is ever created in the
workspace. The patch is an artifact with a recorded digest. **There is no
function in Atlas that applies one**, and adding one is deferred past A8.

Changed files are computed by comparing the two trees, not by asking the driver:
a driver reporting its own changes is a driver describing itself.

### Execution

One runner, `atlas_proc_run` — the only process-creation path in Atlas, extended
for A8 rather than duplicated. A job command gets structured argv, an exact
working directory, an explicitly built environment, a wall bound, an **idle**
bound, output ceilings, and termination of the whole process **group** so a
grandchild cannot survive.

* There is no `sh -c`, no `eval` and no string interpolation anywhere. Shell
  syntax in an argument reaches the child as bytes, which `tests/test_orch_driver.c`
  proves by echoing it back.
* Nothing is inherited: no SSH agent, no sudo askpass, no operator configuration,
  no credential.
* A validation command's `argv[0]` is resolved against a fixed allowlist compiled
  into the binary — an operator's decision, not a submitter's.
* Cancellation is *asked for*, never signalled: the daemon has no path into the
  worker's process tree, so a running child learns of a cancellation through the
  dispatcher's heartbeat.

### Drivers

| Driver | Runs | Needs a model |
| --- | --- | --- |
| `fake` | in process, deterministic: success, failure, timeout, cancellation, malformed result | no |
| `claude` | Claude Code noninteractively (`--print --output-format json`) in `work/` | yes |

A driver needing a model additionally requires `live_model` in the policy, and —
in `operator_session` mode — a dispatcher the policy named for the purpose. Two
independent gates, neither of which a worker can set.

A driver is given a workspace, a task and bounds — not the lease token, the job
identity, the repository path, the pinned commit or the retry limit, because it
must not be able to change any of them.

**A zero exit is not a success claim Atlas accepts.** The Claude driver requests
JSON and classifies a zero exit that produced something else as
`MALFORMED_RESULT`. The check is structural — is this a JSON document? — and
deliberately shallow, because a model's output is never parsed as authority.
`--output-format json` emits a top-level *array*, so both an array and an object
are accepted; nothing inside either is read, and the token and cost fields stay
zero, which the header documents as "not reported" rather than "free".

**A8.1: the model dispatcher and the operator session.** A driver that needs a
live model has to authenticate, and the only Claude Code credential on this
machine is a session in a person's home directory. Atlas will not copy, read,
print or store one. So the root-owned policy may name a *second* dispatcher uid,
`model_dispatcher_uid`, with its own `model_worker_root`, permitted to lease
**only** jobs whose driver needs a model; with `model_credential =
operator_session` that driver runs under the dispatcher's own account and HOME
and the CLI authenticates itself. Atlas sets a working directory and executes.

The cost is stated rather than hedged: those jobs hold the operator's filesystem
authority, not `atlas-worker`'s, so A7.1's OS isolation does not cover them.
Everything else is unchanged — the job record, the lease, the bounds, the
snapshot, the ledger, and the rule that a completed job is not an authority. The
key is absent by default, and absent it A8 is exactly as it was. The alternative
`model_credential = service` requires the root-installed `/etc/atlas/claude.env`
and refuses without it.

Logs are redacted for credential *shapes* before they are stored. That is a
mitigation and is documented as one: a secret Atlas has never seen the shape of
passes through, which is why no credential is ever placed in a workspace, an
environment or a job specification in the first place.

### Retention

A successful attempt's workspace is removed; a failed one is kept as evidence.
Removal descends with `openat`/`O_NOFOLLOW`, unlinks symlinks rather than
following them, refuses anything that is not a directory or regular file, and
takes only the same three validated components `atlas_ws_open` takes. **Atlas has
no "remove this path recursively" primitive and must not grow one.**

## The run (A11.0)

A8 gave a job a `parent_job_uid` and resolved it nowhere. The column was checked
for shape at submission — `'j'` plus 32 lowercase hex — and nothing asked whether
the parent existed, whether it described the same repository, or whether anything
already followed it. A submission naming `j00000000000000000000000000000000` as
its parent was accepted and stored, and every later reader of that chain would
have been wrong about it.

> **A CHAIN OF TASKS WAS EXPRESSIBLE AND NOT ENFORCEABLE.**

A **run** is the durable grouping one chain of tasks belongs to: a root task, and
whatever follows it, under one identity that survives a restart.

### The state, and where each field lives

| What A11.1 needs | Where it is | Notes |
| --- | --- | --- |
| `run_id` | `orch_runs.run_uid`, and `orch_jobs.run_uid` on every task in it | `'r'` plus 32 lowercase hex; a different prefix from a job's `'j'` so the two cannot be confused |
| `task_id` | `orch_jobs.job_uid` | A8's, unchanged |
| `parent_task_id` | `orch_jobs.parent_job_uid` | A8's column; A11.0 is what resolves it |
| `attempt_number` | `orch_attempts.attempt_no` | monotonic per task, `UNIQUE(job_id, attempt_no)`; a child's attempts start at 1 and inherit nothing from its parent |
| task `status` | `orch_jobs.state` | A8's eleven-state machine |
| run `status` | `orch_runs.status` | `ACTIVE`, `ACCEPTED`, `BLOCKED` — its own axis |
| the repository and source | `orch_runs.repo_identity_hash`, and each task's `repo_identity_hash` and pinned `source_commit` | every task in a run must agree with the run |

The run identity is **derived, never supplied**. It is not a member of
`atlas_orch_spec`, so `ATLAS_ORCH_SPEC_DOMAIN` did not move and every stored
`spec_digest` means exactly what it meant before A11.0. A root task — one
submitted with no parent — creates its run. A child inherits its parent's.

### What submission refuses

All four checks run inside the transaction that inserts the job, for the reason
the idempotency check does: a check that a run is still ACTIVE is worthless if a
second submission can land between the check and the insert. Every one refuses
the whole submission; none repairs anything.

1. **The parent must exist.** `no job named %s exists to be a parent`.
2. **The parent must belong to a run.** A job from before migration 21 does not,
   and inventing one for it now would be the backfill the migration refused.
3. **The parent must describe the same repository.** A chain that changes
   repository midway is two chains, and joining them would let a child inherit a
   run whose source identity it does not share.
4. **The run must be ACTIVE, and must have no other active task.**

### One active task per run

This is a partial unique index on `orch_jobs(run_uid)` over the non-terminal
states, not only a checked `SELECT`. It follows `M8_LEASES`' precedent for "at
most one unreleased lease per job", and it is load-bearing rather than
decorative: with the C check disabled the submission is still refused, by the
constraint. The check in `submit_resolve_run` exists so the caller gets a
sentence naming the task in the way instead of a constraint violation it cannot
act on.

**CANCEL_REQUESTED is deliberately not terminal**, on either side. A task that
has been asked to stop has not stopped, and a run that admitted a second task at
that moment would have two. The index predicate spells the terminal set out in
SQL because SQLite cannot call `atlas_orch_state_is_terminal`;
`tests/test_orch_run.c` compares the two over the whole vocabulary rather than
trusting they were kept in step by hand.

### The run's status is its own axis

A task ending SUCCEEDED **does not** accept its run. A task ending FAILED **does
not** block one. "This attempt finished" and "this line of work is settled" are
different claims, and no code path derives either from the other — the separation
A9.2 keeps between a verification state and a lifecycle status, one layer out. A
run whose only task succeeded is still `ACTIVE`, which is exactly what lets a
follow-up task join it.

`UNKNOWN` is zero, is not terminal, and **does not parse**: it is the
vocabulary's zero, no stored run may hold it, and a database presenting it is
reporting corruption rather than a state.

`atlas_db_orch_run_set_status` is a compare-and-swap — the caller names the
status it believes the run holds, and exactly one row must change. Only
`ACTIVE -> ACCEPTED` and `ACTIVE -> BLOCKED` are permitted; a terminal run is
final in both directions.

### What A11.0 deliberately does not do

- **It starts no worker**, runs no driver, and generates no follow-up task.
- **Nothing in production settles a run.** `ACCEPTED` and `BLOCKED` have no
  producer outside a test. There is **no RPC method, no MCP tool and no gateway
  route** that reaches `atlas_db_orch_run_set_status`, which is what makes "a
  model payload cannot accept a run" true by absence rather than by a check —
  the house form of the claim, as A9.2's `AUTO_REJECT` and A9's remote credential
  administration are absent rather than refused. Who may settle a run is A11.1's
  question, and answering it here would have been inventing it.
- **It backfills nothing.** Every job that existed before migration 21 keeps an
  empty `run_uid` and belongs to no run.

### Reading it back

`atlas_db_orch_run_get` returns the run whole: its identity, its root, the
repository it is bound to, its status, and the one task in it that is still
active with that task's state. `active_job_uid` is part of the view rather than a
second query because "which task is this run waiting on?" is the question a
caller resuming after a restart actually has, and answering it in two reads would
let the two disagree. An empty `active_job_uid` is an ordinary answer — a run
between tasks — and never an error.

`atlas_orch_job_view` carries `run_uid` and `parent_job_uid`, so the chain is
read back rather than reconstructed.

## Status: what is implemented, and what is not

Everything A8 set out to build is implemented and tested: the job model and its
canonical digest, the persisted state machine, schema 8 and its migration, the
one write point, leases, heartbeats, retry, cancellation and crash recovery, the
root-owned policy, the two RPC groups, the dispatcher process, workspace
provisioning and source snapshotting, bounded command execution, both drivers,
patch and artifact collection, log redaction, the CLI surface and the systemd
unit.

**A11.0 added the run** — see the section above. Its two terminal statuses have no
producer in production code, and settling a run is reachable from no RPC method,
MCP tool or gateway route. That absence is the deferral, not an oversight.

**Deferred past A8, and absent rather than refused:** applying a generated patch,
autonomous commits, branch creation, push, GitHub issue ingestion, PR creation,
review handling, merge, multi-model routing and automatic decision approval.
Their absence is the deferral — `tests/test_orch_rpc.c` asks a live daemon for
every name they would plausibly have and requires each to answer `unknown
method`.

**Live model execution requires `live_model = on` in the policy and one of two
credential arrangements**, and Atlas never creates, reads, copies, prints or
stores either.

* `model_credential = service` (the default): a worker service credential
  installed by root at `/etc/atlas/claude.env`, used by the `atlas-worker`
  dispatcher. Without it the `claude` driver refuses cleanly and everything else
  works.
* `model_credential = operator_session` (A8.1): the policy also names
  `model_dispatcher_uid` and `model_worker_root`, and a second dispatcher runs
  under that account, using whatever session already lives in its HOME. Atlas
  sets a working directory and executes; it never touches the credential. Those
  jobs carry that account's filesystem authority — see the note in **Drivers**
  and `docs/security/A7_1_THREAT_MODEL.md`.

Deferred here too, and for the same reason: A8.1 changes which OS principal a
model driver runs as, and nothing else. It applies no patch, mints no lifecycle
capability, adds no RPC method and adds no schema migration.

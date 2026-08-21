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

This is a partial unique index over the non-terminal states, not only a checked
`SELECT`. It follows `M8_LEASES`' precedent for "at most one unreleased lease per
job", and it is load-bearing rather than decorative: with the C check disabled
the submission is still refused, by the constraint. The check in
`submit_resolve_run` exists so the caller gets a sentence naming the task in the
way instead of a constraint violation it cannot act on.

Migration 24 **narrowed** this rather than removing it, and the narrowing is
described under "Bounded parallel tasks (A11.6)" below: a run allows up to its
own `max_parallel` active tasks, and **at most one of them may work in the
registered repository's own tree, always**. A run created without asking for
anything allows one task, which is what every run allowed before.

**CANCEL_REQUESTED is deliberately not terminal**, on either side. A task that
has been asked to stop has not stopped, and a run that admitted a second task at
that moment would have two. The index predicates spell the terminal set out in
SQL because SQLite cannot call `atlas_orch_state_is_terminal`;
`tests/test_orch_run.c` and `tests/test_orch_parallel.c` compare the spellings
over the whole vocabulary rather than trusting they were kept in step by hand.

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

## Bounded parallel tasks (A11.6)

A11.0's "one active task per run" was the right guarantee for a season whose
whole subject was a single worker, and the wrong one for a run that wants a
second task doing something else at the same time. A11.6 replaces it with two
guarantees rather than with none.

### The bound is fixed at the root and is never clamped

`orch_runs.max_parallel` is written once, in the transaction that creates the
run, from `--parallel N` on `atlas job submit` or `atlas job run`. It defaults
to **1**, which is exactly what every run was before it existed. Naming it on a
task that *joins* a run is refused, not ignored — a run's parallelism is a
property of the run — and so is naming it on `--resume`. A value outside
`1..ATLAS_ORCH_RUN_MAX_PARALLEL` is refused with the bound named rather than
reduced, which is this file's rule for every bound since A8: a number quietly
lowered is a run that behaves differently from the one that was asked for and
nobody is told.

It travels on `atlas_orch_op`, never on `atlas_orch_spec`. `ATLAS_ORCH_SPEC_DOMAIN`
did not move and no stored `spec_digest` means anything different than it did —
A10.1's arrangement for `memory_mode`, for A10.1's reason.

### The repository's own tree stays exclusive

A run may hold up to `max_parallel` active tasks and **at most one active
repo-tree task, whatever that bound says**. Two workers editing the registered
repository's one working tree at the same time is not a faster run, it is an
incoherent one, and no bound may widen access to it. Parallel siblings are
workspace-driver tasks — `fake`, `claude` — which already run in isolated A8
snapshot workspaces provisioned by the existing dispatchers. **A11.6 adds no
isolation mechanism and no execution machinery**, and creates no thread, process
or background loop anywhere.

Both rules are in the schema, which is what makes them true:

| Guarantee | Where |
| --- | --- |
| at most `max_parallel` active tasks per run | `idx_orch_jobs_active_slot`, unique over `(run_uid, run_slot)` for non-terminal rows; the slot is assigned in the submit transaction |
| at most one active repo-tree task per run | `idx_orch_jobs_one_active_repo_tree`, unique over `run_uid` for non-terminal rows whose driver is a repo-tree one |
| the compiled ceiling | `CHECK(max_parallel >= 1 AND max_parallel <= 8)` and `CHECK(run_slot >= 0 AND run_slot < 8)` |

The driver list in the second index duplicates `atlas_orch_driver_is_repo_tree`
because SQLite cannot call C. `tests/test_orch_parallel.c` compares them in both
directions over `atlas_drivers()`, and the cost is stated rather than hidden:
**adding a repo-tree driver now requires a migration.**

### A run holds one pin

Every task in a run is authorised over the same tree, so a child's
`source_commit` must equal the **root** task's. Two pins would make ACCEPTED
ambiguous — it would mean "the gates passed over this tree" for one task and over
a different one for another, and no reader could tell which. Compared against the
root rather than the immediate parent, so a chain cannot drift a commit at a
time.

### Settlement waits for quiescence

A run is **never** ACCEPTED or BLOCKED while any task in it is non-terminal. A
task that has not ended has not said what it did, and a verdict over it would be
a verdict on work nobody has seen the end of. When the last task ends, the run
settles by scanning its tasks:

- **ACCEPTED** when every task either SUCCEEDED, or FAILED **and has a child in
  the same run** — a failure that was answered — *and* the repository still has
  the identity the run was created against, re-checked from the root task at the
  moment of settlement.
- **BLOCKED** otherwise: anything CANCELLED, TIMED_OUT, RECOVERY_REQUIRED, or a
  FAILED task nobody answered.

**Every terminal producer settles.** A task reaches a terminal state at eight
places, not one: a completion, the recovery sweep's two, a heartbeat that runs
out of renewals, a heartbeat past its wall deadline, a cancellation of a task
that was never leased, and the two refusals a lease can make — a repository whose
identity moved, and a task whose attempts are gone. Only three of the eight
checked for quiescence at first — the completion and recovery's two — so a run
whose *last* task ended at one of the other five stayed ACTIVE with nothing in it
— permanently, because the only event that would have settled it had already
happened. Pilot A11.6-P found it in production: a sibling hit its wall on a
heartbeat, the run had no other active task, and it was still ACTIVE hours
afterwards with an operator waiting on it. All eight now call the same
settlement, and the seven that carry no completion take the `op`-less form, which
spawns no follow-up: a task that ended without being answered leaves the run
BLOCKED rather than unsettled.

Two consequences are worth stating outright, because they are the shape of the
design rather than accidents of it:

- **A gateless workspace sibling can veto acceptance and can never grant it.**
  ACCEPTED still flows only from the gated repo-tree chain succeeding; a sibling
  adds the requirement that it too ended well, and adds nothing else. A run whose
  repo-tree chain never passed a gate cannot be accepted by siblings, however
  many of them succeeded.
- **A doomed run does not stop the chain mid-run.** A cancelled or failed sibling
  does not interrupt the repo-tree task that is running beside it; the run spends
  at most its bounded budget and then settles BLOCKED. One task's failure must
  not break another task's execution, and a run that killed work in flight to
  reach a verdict a few seconds earlier would be doing exactly that.

### Whose budget is three

`ATLAS_ORCH_RUN_MAX_WORKER_STARTS` stays 3 and now names its subject explicitly:
**worker starts of the run's repo-tree chain**, counted from the ledger's
transitions into RUNNING for the run's repo-tree jobs. A workspace sibling spends
none of it and is bounded by its own `max_attempts`, which is A8's semantics
untouched. Letting a sibling eat the chain's budget would mean a run could be
denied the follow-up its gate failure had earned because something else was busy.

This changes no existing run's count: before parallelism every job in a
repo-tree run was a repo-tree job, because a follow-up inherits its parent's
driver.

### What the run view reports

`atlas_db_orch_run_get` gains two fields, and `job.run_status` gains the two
keys `active_count` and `max_parallel` to carry them.

`active_job_uid` is the task a **run driver may claim**. For a run whose root
works in the repository's own tree that is the active repo-tree task — there is
at most one by construction — and it is empty when the chain is done while a
sibling is still going. That is now an ordinary mid-run answer rather than an
ending, which is why `active_count` exists beside it: a reader that inferred one
from the other would read such a run as idle. For every other run
`active_job_uid` is the run's first active task, exactly as it has been since
A11.0.

Both keys are **absent from an older daemon**, and the CLI leaves both at zero
when it does not receive them — A9.2.5's rule for an absent key. Zero is the
conservative value in both directions: it is never a claim that a run has no
active task, and never a claim that its bound is nothing, because a stored run's
bound is at least one. The renderers print the line only when the bound is
present, so an absent key never renders as a claim.

### What A11.6 deliberately does not do

- **No general task DAG.** A run is still a chain plus siblings; there is no
  dependency edge between two tasks beyond `parent_job_uid`.
- **No N-way concurrency in the repository's tree.** That is the one thing the
  second index exists to make impossible.
- **No new isolation mechanism.** Siblings run under A8's existing workspace
  isolation, provisioned by the existing dispatchers.
- **No new RPC method, MCP tool or gateway route**, and no second submit path.
  `atlas_db_orch_run_set_status` still has no caller outside `src/db/db_orch.c`.
- **No thread, process, timer or background loop.**

A residual worth knowing: a single dispatcher process runs one workspace attempt
at a time, so N siblings genuinely overlapping needs N dispatcher processes. That
is already safe — the lease compare-and-swap is what makes two dispatchers unable
to take one job — and dispatcher-internal concurrency slots are recorded in
`docs/backlog.md` as a non-goal of this change rather than as an oversight.

## The foreground run driver (A11.1)

A11.0 built the chain and settled nothing. A11.1 is the thing that carries it:
one operator-started command that drives a run to a settled answer, starting at
most three workers along the way and deciding nothing a model could reach.

### The loop, and why it is in this order

`atlas_rundriver_run` reads the run and, if it is not terminal and has an active
task, hands it to `drive_one`. Each step below is a safety property, and the
comment in `src/orch/rundriver.c` says what moving it would cost.

1. **Read the run.** A terminal run is touched not at all: no lease is asked for,
   no task is created, no worker starts.
2. **Claim the run's active task by name.** The lease request carries a
   `job` narrowing, so the driver claims *its* run's task or nothing. Two
   drivers racing here produce one grant, because the grant is a
   compare-and-swap against `state = 'QUEUED'` and `idx_orch_leases_active`
   permits one unreleased lease per job. The loser is told `busy` and writes
   nothing.
3. **Check the pinned commit, before anything starts.** A repository that has
   moved is not the one the work was authorised over.
4. **Record RUNNING.** Durable before the worker exists, which is what makes the
   run's budget count real starts rather than completed ones.
5. **Start exactly one worker**, in the repository's own root.
6. **Check the pinned commit again.** A worker that committed, reset or checked
   out has invalidated everything a gate could tell us, so the gates are not run
   at all and the run is refused rather than judged.
7. **Run the gates**, in the repository's own root, from the task's stored list.
8. **Report.** The daemon decides what the run is.

While steps 5 and 7 run, `driver_should_stop` renews the lease from inside
`atlas_proc_run`'s wait loop and carries a cancellation back. A heartbeat that
names the phase the attempt is already in renews without transitioning, which is
how the A8 dispatcher has always done it.

### Where the two drivers run

| Driver | Working directory | Log | Granted to |
| --- | --- | --- | --- |
| `fake` | the workspace's `work/` | a file in the workspace | any lease |
| `claude` | the workspace's `work/` | a file in the workspace | any lease |
| `fake-repo` | the registered repository's root | carried inline as an artifact | only a lease naming it |
| `claude-repo` | the registered repository's root | carried inline as an artifact | only a lease naming it |

`claude` and `claude-repo` share one implementation, `claude_exec`. What differs
between them is *where the child runs* and *where its log goes*, and both were
already parameters. Two copies would be two places for the environment
construction, the credential handling and the exit classification to drift, and
the exit classification is the part that decides whether a zero exit is read as
success.

`fake-repo` stands to `claude-repo` exactly as `fake` stands to `claude`: it is
what lets every part of A11.1 above the driver be exercised without a model, a
network or a credential. It appends one line per start to a single file in the
work tree, which is what lets one test show a failing gate producing exactly one
follow-up whose worker then passes.

### What settles a run

`settle_run_after_complete`, inside `atlas_orch_apply_in_tx`, and nothing else.
It runs only for a job whose driver `atlas_orch_driver_is_repo_tree` names — a
run whose task ran under an A8 workspace driver is not settled at all, and
A11.0's statement about it stands unchanged.

| The task ended | The run becomes |
| --- | --- |
| SUCCEEDED, and the repository still has the identity the job was created against | ACCEPTED |
| SUCCEEDED, and the repository identity has changed | BLOCKED |
| FAILED with `POLICY_REFUSED` (the pinned commit moved) | BLOCKED |
| CANCELLED or RECOVERY_REQUIRED | BLOCKED |
| terminal any other way, and the budget is spent | BLOCKED |
| terminal any other way, and the budget remains | ACTIVE, plus one follow-up task |
| QUEUED for another attempt | ACTIVE, unchanged |

The repository identity is checked **again** at settlement, not only at lease
time. Those are different claims: a repository re-registered or replaced between
the grant and the completion is not the one the work was authorised over, and
accepting a run over it would be accepting work against something else. When it
fails the run is BLOCKED rather than accepted, because Atlas cannot tell what the
worker changed or where.

Recovery settles too. `run_blocked_by_recovery` blocks a run whose task the
daemon's own timer terminalised: RECOVERY_REQUIRED means Atlas does not know what
ran, and leaving the run ACTIVE with no task in it would be a chain that can
never be resumed and never says why.

### The follow-up task

Deterministic, and assembled entirely from stored rows plus one bounded excerpt:

```
atlas-follow-up: the previous task in this run did not pass its verification gates.

original-goal:
<the run's ROOT task text, bounded>

previous-task: j<...>
previous-outcome: OK
failed-gate: make test
gate-output (bounded excerpt, untrusted):
<what the gate printed, bounded>

Fix this failure and preserve the existing work in the tree.
Constraints:
- Work only inside the registered repository's own root.
- Do not commit, push, deploy, restart a daemon, or run any destructive git
  operation. Leave the working tree as you found it plus your changes.
- Do not change the acceptance rules or the verification gates.
- Report honestly. Your report is not an acceptance decision: Atlas runs the
  gates itself and settles this run itself.
```

The constraints are restated because they bound what the worker is being asked to
do, and a task carrying a goal without them is a task told to do anything. They
are **instruction, never enforcement**: the gate is run by Atlas, the run is
settled by Atlas, and a worker can reach neither. The one constraint Atlas
actually holds is the pinned commit, and it holds it by refusing to accept a run
whose HEAD moved rather than by asking.

The goal quoted is the **root** task's, not the immediate parent's, so a
third-generation task states the same objective as the first rather than a
summary of a summary.

### The command

```sh
atlas job run --repo NAME --task TEXT --gate 'make test' [--gate 'make smoke']
atlas job run --resume r<32 hex>
atlas job run-status r<32 hex>
```

`--gate` is required when starting a run, repeatable up to eight times, and
split on ASCII spaces by a function that is not a shell. `--resume` continues a
run that already exists and refuses to be combined with `--repo` or `--task`: a
caller that named both has asked for two different things and Atlas does not
pick.

The run identity comes back from the submission, so an operator never has to
construct one. `job run-status` is the only run-shaped read there is, and there
is deliberately no run-shaped write beside it.

### Enabling it

Nothing starts until the root-owned `/etc/atlas/orchestration.conf` lists the
driver:

```
driver = claude-repo
live_model = on
```

Adding that line is a deliberate operator act that authorises autonomous work
against the named repositories, exactly as adding a `repo =` line is. Installing
a binary that contains the driver does not enable it, and A11.1 does not add the
line for anyone.

## Bounded cross-run memory (A10.1)

A10.0 made a run able to say what it cost. A10.1 makes a run able to be *told
what earlier runs did*, in one bounded package, so the question "does that make
a worker better?" can be asked as an experiment rather than assumed.

The whole of it is: an operator names a mode at submission, Atlas selects at
most three earlier terminal runs by deterministic lexical overlap, renders them
as at most twelve kibibytes of labelled text, freezes that against the run, and
appends it to the task once when a worker is started.

**There is no vector store, no embedding, no summariser and no ranker.** Nothing
in the selection calls a model. That is not a simplification to be revisited: a
selection a model made would be one no reader could reproduce, and an experiment
whose treatment arm cannot be re-derived measures nothing.

### The mode

`atlas job submit` and `atlas job run` take `--memory off|bounded`. Absent means
`off`, and an unrecognised spelling is refused rather than skipped — the A7.1
rule for a policy key, for the same reason.

The mode travels on `atlas_orch_op`, beside `peer_uid`, and **never on
`atlas_orch_spec`**. Adding it to the specification would move
`ATLAS_ORCH_SPEC_DOMAIN`, and every `spec_digest` already stored would then mean
something different than it did. It is bound durably to the *run* instead, by
the manifest.

`job.submit` is in the submitter RPC group. There is no MCP tool and no gateway
route that reaches it, so a model payload has no surface on which to turn memory
on, turn it off, or name a source — absent, rather than refused.

`--memory` on `job run --resume` is **refused**, not ignored. The package is
frozen against the run, so honouring the flag would be a lie and dropping it
silently would be a worse one.

### Which runs count as the same repository

Not `repo_identity_hash`. That is A4's **path-qualified** lineage fingerprint,
and it therefore differs between a repository and a linked worktree of it — the
exact case an isolated experiment runs in.

Memory asks a narrower, differently-shaped question — "is this the same git
history?" — and answers it with its own value under its own domain,
`atlas.orch.memory.lineage.v1`, built from the object format and the sorted set
of ingested root commits. **It is never a redescription of `repo_identity_hash`
and never a substitute for it**: nothing authorises, admits or refuses anything
on this value. It selects hints. Every existing check is untouched.

A candidate run stores the path-qualified identity, so its lineage is resolved
through a live registry row that still carries that identity. A run whose
repository has been removed or moved resolves to nothing and is **not a
candidate** — absent, never guessed.

### Selection

1. Terminal runs only — `ACCEPTED` or `BLOCKED` — newest first, at most 64
   examined. A bound that is reached is reported, not silent.
2. Same lineage, as above.
2b. **Never a run that carries a memory manifest of its own.** A run with a
   manifest was created by an invocation that made a deliberate choice about
   memory, so it was part of a memory arm — either arm. This is one predicate
   with no list of identifiers in it, and it is what makes "the runs an
   experiment created are not candidates" a property of the query rather than of
   the order somebody ran things in.

   Freeze ordering alone cannot do this across several pairs. A task's wall
   deadline is `created_ms + wall_timeout_ms`, capped by the policy, so a run
   left queued past it is timed out and its run blocked — several pairs
   therefore cannot all be created before any of them runs, and a later pair is
   necessarily created after an earlier pair has ended.

   **The cost is stated rather than hidden: bounded memory does not compound.** A
   run that was shown memory does not itself become memory, so the candidate
   universe stays the runs that predate this mechanism. For a milestone whose
   purpose is to measure the mechanism that is the conservative direction — a
   corpus already shaped by memory cannot measure memory — and it is the first
   thing to revisit if the answer turns out to be that memory helps.
3. Score is the count of distinct shared tokens between the new task text and
   the candidate's root task text, plus a commit-relation bonus. A token is at
   least four bytes, lowercased, and `_`, `.`, `/` and `-` are inside a token
   rather than delimiters — so `src/orch/rundriver.c` stays one token, which is
   why a lexical rule works on this material at all.
4. The commit relation is `EXACT` (the same pinned commit, bonus 4), `INDEXED`
   (a commit this repository's index has ingested, bonus 1) or `UNKNOWN`.
   **`INDEXED` is not an ancestry claim.** Atlas has no git process inside a
   write transaction and does not claim what it did not establish. Everything
   that is not `EXACT` is marked `STALE` in the rendered package.
5. **A candidate with no shared token is never selected**, whatever its commit
   relation. Preferring an exact commit is a preference among relevant runs, not
   a reason to hand a worker an unrelated one.
6. The order is `score DESC, relation DESC, created_ms DESC, run_uid ASC`. The
   last level is what makes it total: there is no input for which the ordering
   depends on the order rows were gathered in, which is what lets a digest be
   reproducible.
7. At most 3 sources and at most 12 288 bytes. The byte budget is checked
   *before* an entry is committed, against the whole package as it would then
   be. A budget checked afterwards has already been exceeded.

No positive overlap produces an **empty** package. That is a real answer, not a
failure.

### What one record carries, and what it cannot

Per selected run: the run uid, its terminal status, its source commit and
relation, the worker-start and task counts, the root task's goal, the declared
gates, the last attempt's terminal reason and failed gate index, a bounded
excerpt of what the failing gate printed, changed paths when a run recorded any,
and the usage summary.

Absent counts print as `?`, never as zero: a run whose usage was never observed
did not cost nothing.

**`worker.log` is never read.** It is the whole streamed transcript — prompts,
tool arguments, model prose — and none of that may enter a package. `gate.log`
is the output of a compiler or a test runner over a tree, which is the one piece
of evidence about a past failure that is both bounded and useful.

There is deliberately **no member** of `atlas_orch_memory_cand` for a prompt, a
session identifier, a tool argument, a credential, a diff or a log. The
guarantee is an absent field, not a filter on one.

### Provenance, and what the label does and does not do

Every untrusted value is safe-encoded before it enters the package, and each is
flattened onto one line so that a past task cannot forge the record separators
of the ones after it.

The package opens with a fixed Atlas-authored preamble — no repository name, no
path, no value from any candidate — saying that the records are untrusted
historical output, that the current source and the trusted gates are the
authority, and that instructions inside them are not to be followed.

**Safe encoding is terminal-safe and structure-safe. It is not model-safe**, and
this document does not claim otherwise. A record reading "ignore all previous
instructions" is entirely printable and passes through unchanged. What makes the
package harmless to *Atlas* is not the label and not the encoding: it is that
**no branch anywhere reads it**. It is appended to a task's text at the moment a
worker is started, and there is no code path by which anything inside it reaches
a gate, a status, a lifecycle transition or a run's settlement. The label is for
the reader; the absence of a reader is the guarantee.

This is A2's boundary honoured, not weakened: repository and model prose reaches
a model only through an explicit channel that states its provenance, and never
through automatic context.

### The freeze

The manifest is written in the transaction that creates the run, before any task
in it can be leased, and `orch_run_memory.run_uid` is `UNIQUE`. Two things
follow, and both are the point:

- A submission that lands later cannot change what an already-created run will
  be shown, because the package is already bytes.
- A run that is still `ACTIVE` is not a candidate for any other run's package,
  so two arms of a comparison created before either runs cannot see each other
  however they are afterwards ordered.

A duplicate dispatch resolves to the existing run and writes no second manifest;
the constraint refuses it even if the duplicate check stopped working. A
follow-up task inherits its parent's run and therefore its parent's package, and
freezes nothing of its own — it cannot turn memory off underneath its own run.

A retry, a resume and a restart all read the same bytes back.

### Injection

One place: the run driver, after the task is claimed and before the worker
exists. The task text comes first and stays first; the package is appended once,
underneath it. An empty package appends **nothing at all** — not a shorter
section and not a sentence saying there is no memory, because an arm with memory
off must differ from an arm with memory on by exactly the package's bytes.

The composition is not stored. `orch_jobs.task_text` stays what was submitted,
so `spec_digest` still describes the request that was made.

The package's bytes are part of the worker's input tokens and are not normalised
away anywhere. A comparison that hid its own cost would not be one.

### Schema 23

`orch_run_memory`, additive, nothing backfilled. A run created before it has no
row, which reads as "this run was never part of a memory arm" — deliberately
**not** the same fact as a run that ran with memory `OFF`, and the two never
share a line on any surface.

It hangs off the run rather than off an attempt because it must exist before any
attempt does. `orch_artifacts` is keyed by `attempt_id` and there is no attempt
to hang it on at freeze time; inventing one would be a row describing an
execution that had not happened.

`RETENTION[]` marks it `CANONICAL` and not prunable: the candidates it was
rendered from move as later runs land, so a pruned package cannot be reproduced.

### The surface

`job run-status` gains `memory_mode`, `memory_package_status`,
`memory_package_digest`, `memory_package_bytes`, `memory_source_count`,
`memory_candidates_truncated` and `memory_sources` — additively, on both
renderers, from one service result.

The package's **bytes** are not on that surface. It is a status read, the
package is up to twelve kibibytes of untrusted historical output, and a reader
checking that two arms differed needs the digest, not the text.

`job run-status` has no offline form and never had one: orchestration state
lives in the index and `atlasd` is its only writer.

**No new RPC method, no MCP tool and no gateway route.** The mode arrives on a
submission an operator made; the package leaves on a lease Atlas granted.

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

## A12.0 — the planned run

A11.6 could carry a chain of tasks an operator wrote. A12.0 is the season where
the *chain itself* is proposed by a model — and the whole design exists to make
that proposal harmless.

> **A PLAN A MODEL WROTE IS A PROPOSAL, NOT A VERDICT.**

Nothing about compiling a plan grants anything. Every task a plan produces is
submitted through the one submit path, runs under the same refusals, the same
lease exclusivity, the same worker-start budget, the same gates and the same
settlement as a task an operator typed. What a plan changes is *who suggested the
work*, and that is deliberately the only thing it changes.

### The flow

```
atlas plan run --repo NAME --goal TEXT --gate CMD [--gate CMD]... [--parallel N]

  goal + gate floor            → plan.create        → orch_plans row
  composed planner prompt      → job.submit         → planner job k (workspace)
  planner writes one artifact  → dispatch.complete  → plan.atlas-plan, stored
  the job's own stored bytes   → plan.revision_add  → orch_plan_revisions + tasks
  stage S: tree task + siblings → job.submit ×n     → one ordinary A11.6 run
  the run driver               → drives the run     → ACCEPTED or BLOCKED
  a BLOCKED stage-run          → one more planner job with reason REPLAN
  every stage-run ACCEPTED     → the plan derives COMPLETED
```

`atlas plan run` is a **foreground command an operator started**, exactly as
`atlas job run` is. It starts no thread, no timer, no scheduler and no background
loop, it polls nothing, and it holds no state that must survive a crash: the
plan's state is re-derived at the top of every iteration and every write it makes
is idempotent.

### Plan status is derived, and has no writer

There is no status column on `orch_plans`. `atlas_db_plan_state_derive` computes
the answer on every read from stored rows — the revisions, the correlations, the
job states and the run statuses — and both `plan.get` and the plan driver ask
that one function.

| Status | What it means |
| --- | --- |
| `PLANNING` | no usable revision yet, and planner budget remains |
| `EXECUTING` | the latest revision has non-terminal work |
| `NEEDS_REPLAN` | a stage-run of the latest revision settled BLOCKED, and budgets remain |
| `COMPLETED` | every stage-run of the latest revision ACCEPTED and every side job SUCCEEDED |
| `BLOCKED` | a budget is exhausted, or the last planner job failed terminally with none left |

`UNKNOWN` is zero, is never stored and does not parse. A plan that derives it is
reporting a defect in the derivation rather than an answer about the plan.

This is A11.0's authority-by-absence, one layer up: there is no `plan.settle`, no
status to compare-and-swap, no MCP tool and no gateway route, so **"a model
payload cannot declare a plan complete" is true because the verb does not
exist**, not because a check refuses it.

### The authority argument

- **The operator brings the goal and the gate floor. The planner may only add.**
  `plan run` requires at least one `--gate`, refused locally before the policy is
  even read. The floor is stored on the plan row and is prepended verbatim and
  first to every tree task's merged validations; a planner's `gate:` lines are
  appended after it and can never replace it. Floor plus additions is at most
  `ATLAS_ORCH_MAX_VALIDATIONS` (8) and a plan whose merged list exceeds it is
  refused when it is compiled, even though the additions alone were in bounds. A
  plan with no operator gate could only ever be accepted on a model's word.
- **Only a planner-role job can produce a revision.** `plan.revision_add`
  verifies, inside the write transaction, that the named job's `correlation`
  binds it to *this* plan as planner job k, that its driver's role is PLANNER,
  that it SUCCEEDED, and that its successful attempt stored an artifact named
  `plan.atlas-plan` inline and within bounds. The bytes are read from
  `orch_artifacts` and from nowhere else: the model's document never travels a
  second path into the write point.
- **A role is a property of the driver, never of a job.** `claude-plan` and
  `fake-plan` are PLANNER; `claude` and `claude-repo` are EXECUTOR; `fake` and
  `fake-repo` are NONE. A submitter cannot assert a role.
- **Model prose never routes control flow.** The replan trigger is Atlas' own
  verdict — a stage-run that settled BLOCKED — and never a sentence a worker
  wrote. There is no `strstr` over a plan document, a worker log or an artifact
  anywhere on that path. A blocker-artifact fast-path is recorded in
  `docs/backlog.md` as a residual, and could only ever veto earlier, never grant.
- **Which model each role runs under is the operator's, in the root-owned
  policy.** `planner_model` and `executor_model` are optional keys in
  `/etc/atlas/orchestration.conf`; unset passes no model flag at all and leaves a
  worker on the account's own default, which is what every run before A12.0 did.
  **No model name appears in `src/`.**

### The `atlas-plan-1` format

The planner writes exactly one artifact, `artifacts/plan.atlas-plan`, at most
`ATLAS_PLAN_MAX_BYTES` (65536) bytes. The specification below is
`PLAN_FORMAT_SPEC` in `src/orch/plan.c`, quoted verbatim — it is the text the
planner is shown and the rules the parser enforces, and the numbers in it are
pinned to the constants by `_Static_assert`.

```
plan-format: atlas-plan-1

The document is line-based. Lines are at most 4096 bytes. One trailing
carriage return per line is stripped. UTF-8 is not assumed. Any line the
parser does not recognise is a refusal naming the line number.

atlas-plan-1
stage: 1
task: <key>            # [a-z0-9-]{1,32}, unique across the whole plan
kind: tree             # exactly one tree task per stage
title: <one line, at most 200 bytes>
gate: <cmd>            # tree only, 0..n; appended AFTER the operator floor;
                       # same parsing as --gate (space-split argv, allowlist
                       # make/ctest/cmake/true/false); the floor and these
                       # together are at most 8
prompt<<
<free text for the executor, at most 16384 bytes>
>>
task: <key2>
kind: side             # 0..3 per stage; no gate: lines allowed on side tasks
title: ...
prompt<<
...
>>
stage: 2
...

Every refusal names what and where:
- the header line is exactly `atlas-plan-1`;
- stages are numbered 1..N ascending with no gaps, N at most 4, and there is
  at least one;
- exactly one `kind: tree` per stage; side tasks per stage at most 3 and at
  most (max_parallel - 1), which for this plan is the bound stated above;
- at most 8 tasks in total; keys unique across the plan; every task carries
  all four of `task`, `kind`, `title` and `prompt`;
- `gate:` under `kind: side` is a refusal, and so is a `gate:` line this
  task's `kind: tree` line has not yet been reached;
- a gate program outside the allowlist is a refusal;
- the operator's immutable gate floor listed above is prepended, verbatim and
  first, to every tree task's gates; it is never replaced by them. The floor
  count plus your additions for one tree task must total at most 8. The floor
  is listed above, so count it: a plan whose merged list exceeds 8 is refused
  when it is compiled, even though the additions alone were within bounds;
- `title:`, `gate:` and `prompt<<` before this task's `task:` line are
  refusals;
- a repeated `kind:`, `title:` or `prompt<<` inside one task is a refusal;
- a field line's prefix is exactly `stage: `, `task: `, `kind: `, `title: `
  or `gate: `, and a heredoc opens on the whole line `prompt<<`;
- outside a heredoc, a blank line is a refusal, as is any other
  unrecognised line;
- the heredoc terminator is the line `>>` exactly: `>> ` with a trailing
  space is prompt content, and a heredoc that is never terminated is a
  refusal;
- a prompt is required and is at most 16384 bytes; each body line
  contributes its own bytes and one newline;
- a NUL byte anywhere in the document is a refusal;
- the whole document is at most 65536 bytes.
```

A refusal travels as **a sentence and a line number**, apart rather than folded
together, so the driver renders `line %d: %s` back to the next planner without
parsing Atlas' own prose to recover the number.

### Correlation and idempotency

Every job the plan layer submits carries a `correlation` that is *also* its
idempotency key, and the plan-to-jobs mapping is derived from it. There is no
bind RPC, no `plan_id` column on `orch_jobs` and no update.

| job | correlation, and idempotency key |
| --- | --- |
| planner job k (k = 1..5) | `plan.<uid21>.planner.<k>` |
| task `<key>` of revision R | `plan.<uid21>.r<R>.<key>` |

`<uid21>` is `'p'` plus the first 20 hex characters of the plan uid — 80 bits.
The full uid would not fit: a correlation is bounded at 64 bytes and validated by
`is_name`, which admits `[a-z0-9._-]` and therefore no colon. The worst case is
62 bytes. `atlas_plan_correlation_planner` and `atlas_plan_correlation_task` are
the **only** producers of either string, because three layers build it — the
write point checking a planner job's binding, the derived reader finding a plan's
jobs, and the driver submitting them — and two spellings of one format are two
answers to "is this job this plan's".

A driver killed at any point and started again re-issues the same submissions and
is handed the same jobs back. `idx_orch_jobs_correlation` (migration 25) makes
the derived read cheap.

### The budgets, and the worst case

Every bound is compiled in; none has a flag or a policy key.

| Bound | Constant | Value |
| --- | --- | --- |
| planner jobs per plan | `ATLAS_PLAN_MAX_PLANNER_JOBS` | 5 |
| compiled revisions per plan | `ATLAS_PLAN_MAX_REVISIONS` | 3 |
| stages per revision | `ATLAS_PLAN_MAX_STAGES` | 4 |
| tasks per revision | `ATLAS_PLAN_MAX_TASKS` | 8 |
| side tasks per stage | `ATLAS_PLAN_MAX_SIDE_PER_STAGE` | 3, and at most `max_parallel − 1` |
| plan document bytes | `ATLAS_PLAN_MAX_BYTES` | 65536 |
| goal bytes | `ATLAS_PLAN_GOAL_MAX` | 16384 |
| repo-tree worker starts per stage-run | `ATLAS_ORCH_RUN_MAX_WORKER_STARTS` | 3 |

Model-worker starts per plan, worst case: at most 5 planner jobs, each with
`max_attempts = 1`; then per compiled revision at most 4 stage-runs, each costing
at most 3 repo-tree starts plus at most 3 siblings at one attempt each.

**5 + 3 × 4 × (3 + 3) = 77 worker starts.**

Completed stages are never re-run and a revision that never compiled spends no
stage budget, so the practical small-goal case is one revision and one or two
stages — roughly four to eight starts. The stated ceiling ignores both. The
number exists so that nobody discovers it in a bill.

### The commands

```sh
atlas plan run --repo NAME --goal TEXT --gate CMD [--gate CMD]... [--parallel N]
atlas plan run --resume PLAN
atlas plan status PLAN
atlas plan show PLAN --rev N
atlas plan list
```

`--resume` names an existing plan **and nothing else**: naming a goal, a gate or
a parallelism beside it is refused rather than ignored, which is A10.1's
`--memory --resume` rule and A11.6's `--parallel --resume` rule for the same
reason — a flag that was quietly dropped reads, afterwards, exactly like one that
was honoured. `plan show` prints the planner's document labelled
`UNTRUSTED_DATA`; every read-back surface safe-encodes a goal, a title and a
prompt.

### The run driver's transport, and the task-level owed-check (A12.0/T1)

The plan driver sits above the same transport `atlas job run` uses, and both
pilots of A11.6 died on its fragility: `apply_op` retried only `BUSY:` and treated
every other failure as fatal, so one timed-out frame-header read killed a
foreground driver while its worker kept editing the tree.

A12.0 separates the two claims. `BUSY:` is the daemon saying it took nothing — an
answer, and asking again gets the same one. A *transport* failure says nothing at
all about whether the request was processed, so it is retried on its own bounded
budget: `ATLAS_RUN_XPORT_TRIES` (5) attempts, `ATLAS_RUN_XPORT_PAUSE_MS` (2000)
apart. The classification is `atlas_err_is_transport`, stamped by the client layer
that held the file descriptor; it cannot travel the socket, so nothing a daemon
says — or quotes back from a repository, a task or a model — can produce one.

`atlas_rundriver_transport` therefore has **three** members rather than two:
`apply`, `run_get` and, new in A12.0, `job_get`. `job_get` is not optional,
because a completion whose answer was lost is only *delivered* if the task ended
in the way that completion asked for, and the **run** cannot say that: "this run
no longer holds this task open" is equally what an expired lease, a cancellation
and a recovery sweep produce. A lease is a minute and a refused completion is
offered for five, so a driver that read only the run would report a genuinely lost
result as delivered exactly when a recovery had just thrown it away. A transport
without `job_get` cannot answer the question, and the alternative to refusing one
at construction is a check that silently degrades into a guess.

The owed-check establishes **"nothing is owed"**, not "recorded with our result".
That distinction is stated rather than closed: a completion that landed and
*requeued* the task is read as still owed, which is the pessimistic direction.

### What A12.0 deliberately does not do

- **No thread, no process, no timer, no background loop.** `atlas plan run` is a
  foreground command and stops when nothing it can do would move the plan.
- **No new method group, no MCP tool, no gateway route, no second submit path.**
  The four `plan.` methods live in the existing orchestration *client* group,
  gated by `require_submitter` like `job.submit`, and none of them carries an
  authority verb. `tests/test_plan_rpc.c` asks a live daemon for every name a
  deciding method would plausibly have and requires `unknown method`.
- **No `plan.settle`, and no status column.** See above.
- **No general task DAG.** A revision is stages in order, each a chain plus
  siblings — A11.6's shape, chosen by a planner instead of by an operator.
- **No automatic anything.** Nothing runs a plan that an operator did not start.

### Stated costs and residuals

- **A planner job's own run stays ACTIVE forever.** A planner job is a workspace
  job, so it is the root of a workspace-rooted run, and a workspace-rooted run
  never settles — pre-existing A11.6 semantics, now produced on purpose. `plan
  status` names the planner job and its state, and says how many of the five have
  been spent; the run it roots is not surfaced at all, and a `job list` will show
  one ACTIVE run per planner job forever. `docs/backlog.md` carries it.
- **A plan whose fifth planner document is format-refused stays PLANNING
  durably.** A refusal leaves no row: at k < 5 the *next* planner job is the
  durable evidence that a refusal happened, and at k = 5 there is no next job. The
  plan is resumable and every resume re-prints the same refusal, deterministically,
  because it is recomputed from the same stored bytes. Documented rather than
  solved: the alternative is a status that says BLOCKED about a plan whose paid,
  valid document could still be ingested.
- **A failed gate's name does not reach a replan prompt.** `job.get` exposes no
  failed-gate index, so `atlas_plan_compose_replan` writes `failed-gate: (none
  recorded)` rather than naming a gate nobody established, and carries an excerpt
  only when a `gate.log` artifact was stored inline.
- **A refused-document retry loses the completed-work section.** There are five
  composers, and the parse-retry form is not the replan form; a plan that both
  needs a replan *and* had its replacement document refused is asked again with
  the refusal but without the list of what already succeeded.
- **`plan.revision_add` does not compare the planner job's `repo_identity_hash`
  to the plan's.** The correlation binds the job to the plan and the submit path
  already refuses a repository the policy does not permit, so this is a narrowing
  that is not yet made rather than a hole; it is recorded in `docs/backlog.md`.

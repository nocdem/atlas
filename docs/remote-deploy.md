# Remote deploy: a root agent applies Atlas' own stored patch, from a browser confirmation (A17)

A14 let a credential in flight queue a job. A14R let that same session read
what the job produced — three artifacts Atlas composed, `result.txt`,
`changes.patch` and `validations.txt` — because Decision 7's stated
alternative, `atlas job artifact`, was not a real command and an executor
attempt's bytes were destroyed with its workspace anyway. What A14R did not
give back was a way to *do* anything with `changes.patch` once it was read.
The steward could see the diff and could not make it real. A17 closes that
gap, narrowly: a second credential, distinct from the one that proposed the
job, confirms — with a typed digest prefix, from a browser, A16's pattern
carried over — that the stored patch should be applied, and a root-owned
agent that has never been reachable from any socket does the applying,
building, testing, installing and restarting, entirely outside Atlas' own
process.

The two sentences it exists for:

> **A CALLER MAY NEVER APPLY A WORKER'S OUTPUT; A ROOT AGENT MAY APPLY
> ATLAS' OWN STORED PATCH, UNDER A CONF THE REQUEST NEVER TOUCHES AND A
> CREDENTIAL THAT NEVER ALSO PROPOSED IT.**

and

> **A CAPTURED DEPLOY CREDENTIAL INSTALLS CODE THAT RUNS AS ROOT, WHICH IS A
> DIFFERENT COST FROM QUEUING A JOB OR DISPOSING OF A RECORD — SO IT GETS ITS
> OWN WRITTEN ACCEPTANCE, NEVER BORROWED FROM THE OTHER TWO.**

## What this reverses, and why it is narrower

A14R refused four names outright — `job.remote_apply`, `job.remote_artifact`,
`job.remote_log`, `job.remote_run` — and `tests/test_orch_rpc.c:107` has
scanned for them since. The reasoning at the time: a caller who could reach
any of them could make a worker's own output run, and a worker's output is
untrusted text a request paid to produce, not something Atlas should ever
execute or apply on a caller's say-so. A17 is exactly the capability that
reasoning refused — applying a worker's output to a tree — so it is a
**reversal**, and it earns that word only by being narrower than the
capability that was refused, in four ways that each hold on their own:

- **The bytes applied are not a request field.** They are `changes.patch`,
  the artifact Atlas already stored under A14R's own contract when the
  originating job's attempt SUCCEEDED in patch mode. Its digest is pinned at
  proposal time (`atlas_db_deploy_propose_in_tx`, `patch_digest`), and
  confirmation is checked against that stored digest, never against anything
  a request supplies fresh.
- **Which tree, which owner, which units, which test are not request
  fields either.** They live in `/etc/atlas/deploy.conf`, a root-owned file
  the daemon and the gateway never open. A request names a job and, at
  confirm, a challenge and a confirmation string; the agent that reads the
  resulting queue file refuses anything whose `repo_root` does not match its
  own `tree` line before it does anything else.
- **The identity that proposes and the identity that confirms must differ.**
  `remote_submit_key` queues the job whose patch this is; `remote_deploy_key`
  confirms applying it. `gwpolicy.c` refuses a policy naming the same
  credential as both `remote_deploy_key` and any `remote_submit_key` — or as
  `remote_dispose_key` — the same "one credential, one power" argument A16
  and A14 already made for their own pair, extended to a third. The deploy
  credential itself holds no stored scope at all, either — A16's dispose
  rule, carried to this third credential rather than restated for it.
  `verify_deploy_credential` (`src/ipc/server_deploy_remote.c`) reads the
  credential's row a second time after `atlas_orch_remote_verify` proves it
  real, and refuses a non-zero stored mask with the sentence "the deploy
  credential holds no other power"; `gateway.auth` derives `deploys:confirm`
  only for the one credential the policy's `remote_deploy_key` line names,
  exactly as it derives `decisions:dispose` for A16's own dispose key. A
  credential minted with any ordinary read scope can never confirm a
  deploy.
- **The process that runs as root is not Atlas.** It is
  `deploy/a17/atlas-deploy-agent.sh`, an operator-installed script triggered
  by a systemd path unit watching a spool directory, that opens no SQLite
  handle and never touches the daemon's socket. Root sees a request file and
  a patch file; it never sees a credential, a database, or Atlas' own
  process.

The "no method applies anything" sentence survives this season rather than
being quietly narrowed to "no *daemon* method": `deploy.remote_propose`,
`_get`, `_list`, `_cancel`, `_challenge` and `_confirm` are six new methods
and none of them starts a process, writes to the target tree, or reads a
byte from it. `deploy.remote_apply`, `deploy.remote_install`,
`deploy.remote_restart` and `deploy.remote_run` join the forbidden set in
`tests/test_orch_rpc.c`, so the same test that has protected A14R's boundary
protects this one.

### The honest paragraph, in the same breath as the capability

A16's disposal credential, captured, disposes of a knowledge record — a
write to one table, reversible in the sense that the record's history still
exists. A14's submission credential, captured, queues a worker that runs as
the operator's own account within a daily bound. **A captured deploy
credential installs code that runs as root on this machine, unbounded by
anything Atlas checks, over cleartext LAN unless something terminates TLS in
front of the gateway.** `tls_mode = NONE` is this deployment's actual
setting today. That is a materially larger cost than either of the other two
capabilities, and neither of the operator's existing written acceptances —
`operator_accepts_cleartext_disposal`, `operator_accepts_cleartext_submission`
— implies it. `operator_accepts_cleartext_deploy = yes` is therefore its own
line, refused by `gwpolicy.c` whenever a `remote_deploy_key` is named without
it under `tls_mode = NONE` (or absent), refused under `tls_mode =
REVERSE_PROXY` (nothing to accept), and printed by `atlas gateway status` as
`clear-deploy:` — so the sentence is not a comment nobody reads, it
is a condition the policy loader enforces and a line an operator had to type
on purpose.

## Methods, routes and tools

Six daemon methods, one file (`src/ipc/server_deploy_remote.c`), offered to
the gateway uid only when `atlas_server_remote_deploy_policy_ready` holds —
the same four-condition shape A16 and A14 each use, widened here because two
different credential pools can each make the group reachable:

| Method | Params | Authorised by |
| --- | --- | --- |
| `deploy.remote_propose` | `job` | the submit credential that queued the job |
| `deploy.remote_get` | `deploy` | the submit credential or the deploy credential |
| `deploy.remote_list` | `cursor?`, `limit?` (both integers, like `job.remote_list`'s `after`/`limit`) | the submit credential or the deploy credential |
| `deploy.remote_cancel` | `deploy` | the proposing submit credential |
| `deploy.remote_challenge` | `deploy` | the deploy credential |
| `deploy.remote_confirm` | `deploy`, `challenge`, `confirmation` | the deploy credential |

Each name is offered independently of the others, not as one group:
`deploy.remote_challenge` and `deploy.remote_confirm` are offered only when
`remote_deploy_key` is actually set, and `deploy.remote_propose`,
`_get`, `_list` and `_cancel` are offered only when a submit key is ready.
A policy naming a deploy key with no submit key at all is therefore inert —
all four submit-pool names answer `unknown method`, because nothing can ever
be proposed for the deploy credential to confirm. `_get` and `_list` being
readable by either credential is a fact about the write route's scope check
(`route_scope_granted` against `API_WRITE_ROUTE_ALTS[]`) and about the
daemon's own union verify (`verify_submit_or_deploy_credential`) — it says
nothing about which pool makes the *name* reachable in the first place, which
is the narrower, per-method question `atlas_server_remote_deploy_method_offered`
answers.

Unlike `job.remote_submit`, which carries its bearer token into
`atlas_orch_apply_in_tx` and verifies it inside the write transaction, these
six verify on the calling thread before a write is ever queued —
`atlas_db_deploy_propose_in_tx`/`_confirm_in_tx`/`_cancel_in_tx` all take an
already-resolved credential id, never a token, because the daemon's serve
loop finishes one request's whole handling before starting the next, so
there is no interleaving between the check and the write to reason about.
`confirmation` is checked against `patch_digest`, never against anything the
challenge response itself returns — the same "the response never carries
its own answer" rule A16 built for its revision-hash challenge.

The design fixes six matching gateway routes,
`/api/v1/deploy/{propose,get,list,cancel,challenge,confirm}`, live in
`API_WRITE_ROUTES[]`, and a `deploy:`/`clear-deploy:` pair of lines
`atlas gateway status` prints, human and JSON alike — see §What implementation
changed below for where each landed. `/api/v1/deploy/get` and
`/api/v1/deploy/list` carry `jobs:submit` as their primary scope and
`deploys:confirm` as an alternate (`API_WRITE_ROUTE_ALTS[]`); a credential
holding neither gets a 403 naming both — "this credential does not hold the
`jobs:submit` or `deploys:confirm` scope" — so a deploy-credential holder is
never told it lacks a scope that credential must never carry.
`/api/v1/deploy/confirm` has no alternate: a submit credential presented
there gets a 403 naming `deploys:confirm` alone.

Four MCP tools exist, `remote_only = true`, absent from the stdio adapter,
carrying `ATLAS_SCOPE_JOBS_SUBMIT` (`jobs:submit`, the only scope a
submit-credential MCP session can hold — `deploys:confirm`, this season's
own ungrantable scope, authorises the daemon methods but has no tool):
`atlas_deploy_propose`, `atlas_deploy_status`, `atlas_deploy_list`,
`atlas_deploy_cancel`. **There is no fifth tool for challenge or confirm,
deliberately** — mirroring A16's own choice to keep disposal confirmation off
MCP entirely, applying a credential to install root code is a decision this
season keeps in the browser and out of anything a model session can drive.

## The flow, from the steward's session

1. The steward submits a job through the existing A14 surface
   (`job.submit`/`job.remote_submit`); it SUCCEEDS in `patch` mode and Atlas
   stores `changes.patch` under A14R's own contract.
2. The steward calls `deploy.remote_propose` (or `atlas_deploy_propose` over
   MCP), naming the job. `atlas_db_deploy_propose_in_tx` refuses unless the
   job SUCCEEDED, is `patch` mode, carries a stored non-empty patch, was
   queued by the calling credential, and this repository has no deploy
   already `PROPOSED` or `CONFIRMED`. On success it returns `deploy d…`, the
   patch's sha256 and its byte count.
3. The operator, in a browser holding the *deploy* credential — never the
   submit credential — opens the row in Mission Control, sees the job, the
   base commit, the full digest and the byte count, and the weakness
   paragraph above rendered on screen, and types the first eight hex of the
   digest as confirmation.
4. The root agent, woken by the path unit, applies, builds, tests, installs
   and restarts every unit named in its conf — including the gateway itself,
   under a system deployment.
5. The steward polls `deploy.remote_get`/`atlas_deploy_status` once the
   connection returns; a `CONFIRMED` deploy with no result yet is reported
   with its age, never with a manufactured verdict.

## The state machine

```
PROPOSED ──confirm──▶ CONFIRMED ──result──▶ SUCCEEDED | FAILED
    └──cancel──▶ CANCELLED
```

Migration 33 adds `deploys` and `deploy_transitions`, described in the
migrations table as *"a deploy a credential proposed, a different credential
confirmed, and a root agent reported"* — the state machine's whole shape in
one sentence. `deploys.state` carries a `CHECK` over exactly the five names
above; `deploy_transitions.actor` carries a `CHECK` over
`REMOTE_CREDENTIAL`, `REMOTE_OPERATOR_CONFIRMED`, `DEPLOY_AGENT`. Every
transition is a compare-and-swap against the state the caller observed —
A8's `atlas_orch_apply_in_tx` discipline, kept for a second table rather than
folded into the first, because a deploy is not a job.

- **`PROPOSED`.** Written by `atlas_db_deploy_propose_in_tx`, actor
  `REMOTE_CREDENTIAL`, from the submit credential that queued the underlying
  job. `idx_deploys_one_active` (a unique index on `deploys(repo_id) WHERE
  state IN ('PROPOSED','CONFIRMED')`) is the schema's own guarantee that a
  second proposal for the same repository is refused while one is already in
  flight; `idx_deploys_one_per_job` is the parallel guarantee that one job's
  patch deploys successfully at most once, while a `FAILED` or `CANCELLED`
  attempt may be re-proposed. `dry_run` is never a request field on this or
  any other method — it lives only in the agent's own conf and is reported
  back in the result.
- **`CONFIRMED`.** Reached only from `PROPOSED`, refused inside the same
  transaction unless: the confirmation is exactly the first eight hex
  characters of the *stored* patch digest (a length mismatch refuses rather
  than comparing a prefix of a shorter string); no `orch_jobs` row anywhere
  is non-terminal at that instant (`atlas_orch_state_is_terminal`'s
  vocabulary, checked globally rather than scoped to this deploy's own
  repository, because the agent's restart step ends every job the daemon is
  tracking, whichever repository it targets); and no other deploy is already
  active. **The mirror of that same fact is enforced the other way too**: for
  the whole time a deploy sits `CONFIRMED`, both `job.submit` and
  `job.remote_submit` refuse a new *root* task for that repository, inside
  the same submit transaction A11.0 already opens, with the deploy's own uid
  named in the refusal —

  > `deploy %s is confirmed for this repository and awaiting the agent's
  > restart; no root job may be submitted until it reports a result`

  A follow-up task belonging to a run whose root already passed this gate is
  not checked again; a run cannot un-pass a gate its root already cleared.
  This exists because everything between `CONFIRMED` and the agent's
  `RESTART` stage — `APPLY_CHECK`, `APPLY`, `BUILD`, `TEST`, `INSTALL` — is a
  window during which a root job accepted and then killed by the eventual
  restart would die mid-task for a reason nothing in its own record would
  explain; the build and test stages alone are the kind of work that takes
  minutes, not a measured figure this document claims to have timed.
  On success, `CONFIRMED` composes a request file and writes it to the
  spool; `spooled_at` records whether that write succeeded. A write failure
  leaves the row `CONFIRMED` rather than failing the confirmation outright,
  because the confirmation itself — the credential check and the digest
  match — already committed; a daemon-start sweep retries writing the
  request file for every `CONFIRMED` row with no `spooled_at`.
- **`SUCCEEDED` / `FAILED`.** Reached only from a `.res` file the daemon
  itself parsed out of `results/`, actor `DEPLOY_AGENT`, never from a request
  parameter — the daemon does not trust a claimed outcome any more than it
  trusts a claimed actor anywhere else in Atlas. **The daemon never moves a
  deploy to a state it did not observe.** A `CONFIRMED` deploy with no
  matching result file is not inferred as failed, not timed out, and not
  guessed at: `deploy.remote_get` reports it as `CONFIRMED`, with its age
  since confirmation, for as long as that remains true. This is deliberate
  and it has a cost: if the path unit was never enabled, or the agent died
  before it could write a result, nothing in the daemon ever resolves the
  row, and `idx_deploys_one_active` then refuses every later proposal for
  that repository. There is no timer that fixes this, on purpose — a
  fabricated timeout would be exactly the kind of claim about a process
  Atlas never observed that this rule exists to forbid.

  **The one hand-written escape is `ABANDONED`, and it is not new
  machinery.** An operator who finds a `CONFIRMED` deploy that will never
  produce a result — the path unit was never enabled, or the agent process
  is confirmed dead — writes a result file by hand, as root, into
  `results/<uid>.res`:

  ```
  atlas-deploy-result 1
  deploy <uid>
  outcome FAILED
  stage ABANDONED
  text_bytes 0
  --
  ```

  mode `0644`. This is the parser's own **minimal required set** —
  `deploy`, `outcome`, `stage`, `text_bytes`, then the `--` separator with a
  body whose length matches `text_bytes` (`0` here, so nothing follows) —
  `deploy_spool.c`'s own `res_key_required` names exactly these four as
  required and every other key (`dry_run`, `head_before`, `head_after`,
  `installed_version`, `units`, `rollback`) as optional, defaulting to empty
  or `no`. The daemon's next ingest pass parses this exactly as it would
  parse the agent's own file — the format does not know or care who wrote
  it — and the deploy moves to `FAILED`, `stage ABANDONED`, actor
  `DEPLOY_AGENT`, unblocking `idx_deploys_one_active` for the next proposal.
  `ABANDONED` is a member of the agent's stage vocabulary reserved for this
  one case (§The root agent below); it is written only by a human and never
  by the script.
- **`CANCELLED`.** Reached only from `PROPOSED`, only by the credential that
  proposed it — a deploy that has already been confirmed cannot be
  cancelled, because a confirmation may already have produced a spooled
  request the agent could be reading.

One sentence on the actor vocabulary: `deploy_transitions.actor`'s
`REMOTE_OPERATOR_CONFIRMED` carries the same meaning A16 gave
`REMOTE_OPERATOR_CONFIRMED` in the decision ledger — a channel, never a
person, exactly as `LOCAL_OPERATOR_CONFIRMED` never proved one either — but
it is a **separate vocabulary**: `atlas_deploy_actor` and
`atlas_decision_channel`/the decision ledger's own actor enum are two
different C types over two different tables, and nothing in this season
merges them or reads one to produce the other.

## The queue files, verbatim

The directory is `<data-dir>/deploy/{requests,results}` — composed from
whatever data directory the daemon was started with, the same resolution
order every other Atlas path uses — created at daemon start, mode `0700`.
On this deployment `atlasd`'s data directory is `/var/lib/atlas`, so the
effective paths are `/var/lib/atlas/deploy/requests` and
`/var/lib/atlas/deploy/results`; task 7's own read-only check on this
machine confirmed the parent exists (`/var/lib/atlas`, owned
`atlasd:atlas-clients`, mode `0700`) and that `deploy/` under it does not,
today, because no daemon carrying this season's code has started yet.

These two file formats are a contract between the daemon's own parser and a
shell script that has never linked against Atlas — every field name, every
line order requirement, and every bound is load-bearing.

**`requests/<deploy_uid>.req`** (the daemon writes this, once, on the
transaction that moves a deploy to `CONFIRMED`), beside
**`requests/<deploy_uid>.patch`** (the stored patch bytes, verbatim):

```
atlas-deploy-request 1
deploy d…
job j…
repo_root /opt/atlas
base_commit 2922214…
patch_sha256 <64 hex>
patch_bytes <n>
confirmed_by <key_id>
confirmed_at <iso8601>
```

**`results/<deploy_uid>.res`** (the agent writes this; the daemon parses it,
bounded, and treats anything it cannot parse as `FAILED`, stage `INGEST`,
with the parse failure's own reason as the result text — never as
`ABANDONED`, which is reserved for the hand-written case above):

```
atlas-deploy-result 1
deploy d…
outcome SUCCEEDED|FAILED
stage PREFLIGHT|APPLY_CHECK|APPLY|BUILD|TEST|INSTALL|RESTART|VERIFY|DONE|ABANDONED
dry_run yes|no
head_before <sha1>
head_after <sha1>
tree_dirty_before yes|no
installed_version <first line of atlas --version>
units atlas.service=active atlas-gateway.service=active …
rollback none|binary|patch
text_bytes <n>
--
<at most 32 KiB: the agent's stage lines, then the tail of its last command's output>
```

Two rules on these files never change, and each has a concrete failure chain
behind it rather than being a style preference:

- **The agent writes `.res` explicitly `0644`.** A root process's umask can
  be `077`; a `0600` file owned by root sitting in `atlasd`'s directory would
  be a file the daemon's own uid cannot read, and the deploy would stay
  `CONFIRMED` forever with no error anywhere — silently, because nothing
  refuses to *write* an unreadable file, only fails later to read it back.
- **The agent removes `.req` and `.patch` on every exit path** — on success,
  and on a parse failure it cannot even attribute to a deploy uid (renamed to
  `<name>.bad` instead). `atlas-deploy.path`'s `PathExistsGlob` re-arms
  itself every time the glob's match set changes, so a request file the
  agent leaves behind after processing it is a request file the path unit
  hands back to the service on its very next check — the same job run again,
  forever, against a tree the first run may already have modified.

Both are documented in `deploy/a17/atlas-deploy-agent.sh`'s own header as
unchanging rules of the D.3 contract, not merely conventions this document
restates.

## The root agent

`deploy/a17/atlas-deploy-agent.sh` (installed to
`/usr/local/libexec/atlas/atlas-deploy-agent`) reads `/etc/atlas/deploy.conf`
once per invocation (root, mode `0644`, unrecognised key refused), opens no
SQLite handle, and touches the daemon only through the two spool
directories. It runs nine stages in order, writing one line of the result
per stage, and a hand-written tenth (`ABANDONED`) exists only for the escape
above:

| # | Stage | What it does | Failure |
| --- | --- | --- | --- |
| 1 | `PREFLIGHT` | Parses conf and request; recomputes `patch_sha256` and compares; refuses if `repo_root != tree`; records `head_before` and `tree_dirty_before`. A HEAD that has moved past `base_commit` is *recorded*, not refused here — `APPLY_CHECK` is what decides. | Conf/request mismatch, wrong tree |
| 2 | `APPLY_CHECK` | `git apply --check` as `owner` (via `runuser`, skipped when the agent already runs as that account) — **never `--3way`**. | Any hunk that would not apply cleanly; the tree is untouched |
| 3 | `APPLY` | `git apply` for real. | — |
| 4 | `BUILD` | The conf's `build` command, as `owner`. | Build failure |
| 5 | `TEST` | The conf's `test` command (unset by default — the agent refuses every request at `PREFLIGHT` until the operator picks one). A `dry_run = yes` run stops here: the patch is **always** reversed with `git apply -R` regardless of `reverse_on_failure`, so a dry run never reaches `INSTALL` and never leaves the tree modified. | Test failure; `dry_run` outcome recorded either way |
| 6 | `INSTALL` | `cp -p <binary> <binary>.prev`, then the conf's `install` command as root — never `sudo make install`, which would re-run the build as root and leave root-owned files in `build/`. Records `<binary> --version`. | Install failure |
| 7 | `RESTART` | System units via `systemctl restart <unit>`; user units under the owner's own systemd instance. | Restart failure |
| 8 | `VERIFY` | Every unit's `is-active`; every `MainPID`'s `/proc/<pid>/exe` does not end `(deleted)`; the conf's `ping` command as owner, bounded to 40 s. | `<binary>.prev` is restored, units restarted again, `rollback binary` |
| 9 | `DONE` | Result written (tmp then rename, `0644`); request and patch removed; exit 0. | — |
| — | `ABANDONED` | Written only by a human, only for a `CONFIRMED` deploy the agent can never finish (§The state machine). | — |

**Rollback matrix**, the whole of it: a dry run always reverses with `git
apply -R` regardless of anything else; a real run's `BUILD`/`TEST` failure
leaves the tree with the patch applied (`rollback none`) unless
`reverse_on_failure = yes`, in which case it reverses the patch alone
(`rollback patch`) — a defence stated precisely, because it removes only the
bytes the agent itself added minutes earlier, and is never a `reset`,
`checkout` or `clean` that could touch the operator's own unrelated
uncommitted work elsewhere in the tree; a `VERIFY` failure always restores
the previous binary and restarts (`rollback binary`), because a tree with a
new binary that will not answer `ping` is a tree that must not stay
installed regardless of the patch's own state.

Conf keys, verbatim from `deploy/a17/deploy.conf.template`: `tree`, `owner`,
`spool`, `build`, `test` (unset by default, refused until the operator
picks one), `install`, `binary`, `units_system` (space-separated), `units_user`
(space-separated `owner:unit` pairs), `ping`, `dry_run`, `reverse_on_failure`.

## The HEAD ≠ tree accumulation, and its consequence

A worker's snapshot is taken at `HEAD`; `changes.patch` is a diff against
`HEAD`, not against the working tree. The agent applies that patch to the
working tree and **does not commit** — a rule as firm as the ones that
forbid `reset`, `checkout` or `clean` on a target repository, because
committing on the operator's behalf is a decision about their history that
belongs to them. The consequence: after one deploy, the tree carries
uncommitted changes that `HEAD` does not know about. A second worker's
snapshot is still taken at the (unmoved) `HEAD`, so its own patch is built
against a tree state the first deploy has already left behind — the second
patch can either duplicate the first deploy's hunks or conflict with them,
and `git apply --check` is what decides which, honestly, at `APPLY_CHECK`
rather than corrupting anything silently. This is not hypothetical for this
tree specifically: `docs/plans/2026-09-07-remote-deploy.md`'s own reversal
argument (§A) counted this working tree at 28 files of uncommitted A14R-and-
later work on the day it was written, and the count only grows while several
in-progress tasks land at once — this session's own `git status` shows well
over that. Whichever count is current on the day a deploy is confirmed, the
first real deploy to touch any file already modified underneath it is
refused at `APPLY_CHECK` before anything runs. Who commits after a deploy —
the operator, by hand, or the
agent under an explicit future policy key — is §Open operator decisions
item 1 below, and it is open.

## First activation (operator, on the machine)

A17 ships no live installation and no live restart; the following is what an
operator does once the code above is deployed, in order:

1. **Deploy Atlas itself, one last time by the existing manual path**:
   `bash /opt/atlas/deploy.local.sh` (restarts the daemon only), then by hand
   `sudo systemctl restart atlas-gateway atlas-dispatcher atlas-scanner` and
   `systemctl --user restart atlas-model-dispatcher`. The new daemon creates
   `/var/lib/atlas/deploy/{requests,results}` on this start.
2. **Mint the deploy credential**: `atlas api-key create --label deploy
   --no-scopes` — not a convention but the one shape the daemon will accept:
   `verify_deploy_credential` refuses this credential outright the moment it
   is minted with any stored scope. The secret is shown once; it stays in
   the operator's own browser or password manager and is never handed to the
   steward.
3. **Write the two policy lines** into `/etc/atlas/gateway.conf` (root,
   following the file's existing `.pre-*` backup convention):
   `remote_deploy_key = key_<selector>` and
   `operator_accepts_cleartext_deploy = yes` (required as long as `tls_mode`
   is not `REVERSE_PROXY` — §Open operator decisions item 4).
4. **Install the agent**: `install -o root -g root -m 0755
   deploy/a17/atlas-deploy-agent.sh
   /usr/local/libexec/atlas/atlas-deploy-agent`;
   `install -m 0644 deploy/a17/deploy.conf.template /etc/atlas/deploy.conf`
   and edit its values for this machine (the `test =` line is unset in the
   template on purpose — §Open operator decisions item 3); `install -m 0644
   deploy/a17/atlas-deploy.{path,service} /etc/systemd/system/`; then
   `systemctl daemon-reload && systemctl enable --now atlas-deploy.path`
   **only after** the daemon that creates `/var/lib/atlas/deploy` has already
   run once — a `PathExistsGlob=` unit does not reliably arm against a glob
   whose parent directory does not yet exist (verified against `man
   systemd.path` on this machine, task 7's own finding).
5. **Restart the daemon and the gateway** so the policy is re-read:
   `sudo systemctl restart atlas.service atlas-gateway.service`; `atlas
   gateway status` then shows the `deploy:`/`clear-deploy:` lines this
   policy now produces.
6. **Dry run**: with `dry_run = yes` in `/etc/atlas/deploy.conf`, the steward
   proposes a small job, the operator confirms from the browser, the agent
   applies/builds/tests and reverses the patch, and the steward reads
   `atlas_deploy_status` showing `dry_run yes`, `stage TEST`. Only then is
   `dry_run` set back to `no`.
7. **A stuck deploy**: if a deploy reaches `CONFIRMED` while the path unit
   was never enabled, or while the agent process is confirmed dead, it is
   closed by hand, as root: write
   `/var/lib/atlas/deploy/results/<uid>.res` with `atlas-deploy-result 1`,
   `deploy <uid>`, `outcome FAILED`, `stage ABANDONED`, `text_bytes 0`, then
   the `--` line, mode `0644`. The daemon's next ingest pass processes it
   exactly as it would the agent's own file, and the steward reads
   `FAILED`/`ABANDONED`.

### Every update after the first

Steward: submit → propose → (operator: confirm from the browser, eight
typed hex) → `atlas_deploy_status` once the connection returns. The operator
does nothing on the machine itself for an ordinary update — the exception is
whichever answer §Open operator decisions item 1 eventually gets. Editing
the policy, the agent conf, the agent script itself, or the systemd units
remains an operator act by design; no route, method or tool this season adds
can reach any of them.

## Open operator decisions

One question this season's design raised was answered before any code ran,
and it does not reopen: **the confirmation channel is a browser, holding a
credential distinct from the one that proposed the deploy, with the typed
digest prefix Mission Control shows on screen** — A16's pattern, carried
over rather than re-litigated. Four more are genuinely open; none of them
has been decided, and no live installation or restart has been approved:

1. **Who commits after a deploy?** The agent never commits, by design. The
   choices are an operator doing it by hand after every deploy, or the agent
   doing it under an explicit, written `commit_after_deploy = yes` conf key
   with a fixed message template touching only the files the patch touched.
   The second option is not built in this season; if the first question is
   never answered, the second deploy onward starts hitting `APPLY_CHECK`
   refusals as described above.
2. **Which device carries the deploy credential?** The confirming browser is
   whatever device the operator uses for it; it is never handed to the
   steward's own connector.
3. **Which tests run before install?** `ctest -L unit` finishes in seconds;
   `make test` runs the full suite, estimated at roughly ten minutes in the
   plan this season executed (not independently timed here), during which
   the gateway stays up (install and restart are the last two stages). The
   `test =` line in `deploy.conf` is where this is decided, and the template
   ships it unset.
4. **Is a channel that installs root code acceptable over cleartext LAN at
   all?** The acceptance line is the written form of "yes, on this network."
   The alternative is accepting deploy confirmations only from behind
   `tls_mode = REVERSE_PROXY` — A9's own already-documented shape — which is
   outside this season's scope.

## What implementation changed, and why

New tables (migration 33, `src/db/migrate.c`): `deploys` and
`deploy_transitions`, described above under §The state machine.

New files present in the tree at the time this document was written:

- `include/atlas/deploy.h`, `src/db/db_deploy.c` — the state and actor
  vocabularies, the row types, and the four `_in_tx` write functions
  (`propose`, `confirmed_uid`, `confirm`, `cancel`, `finish`), following A8's
  "every write is a compare-and-swap inside a transaction its caller opened"
  discipline.
- `include/atlas/deploy_spool.h`, `src/daemon/deploy_spool.c` — the spool
  directory creation, the request writer (tmp then rename), and the bounded
  `.res` parser that produces `INGEST` on anything it cannot read. A fix-round
  finding here: the ingest pass sorted pending `.res` filenames with a
  comparator that read a fixed-width filename buffer as a pointer and
  dereferenced it, a deterministic SIGSEGV on the writer thread whenever two
  or more `.res` files were pending in one pass; the comparator is now a
  plain `strcmp` over the buffer itself, exactly as `src/memory/read.c`
  already does for the same fixed-width shape.
- `src/ipc/server_deploy_remote.c` — the six `deploy.remote_*` daemon
  methods, the credential-verification predicate
  (`atlas_server_remote_deploy_policy_ready`), and the in-process,
  file-static mint-then-spend challenge (no new table — migration 33 adds
  none for it — bounded, lost on a daemon restart, and costing an operator
  one re-mint when that happens). Every string a `.res` file contributes to
  `deploy.remote_get`'s response — stage, rollback, both head hashes, the
  installed version, the result text — is safe-encoded at the point of
  output: the root agent's own prose is exactly as untrusted as any other
  value read raw from outside Atlas' own process, and CLAUDE.md's rule on
  untrusted text applies to it unchanged.
- `deploy/a17/atlas-deploy-agent.sh`, `deploy/a17/deploy.conf.template`,
  `deploy/a17/atlas-deploy.path`, `deploy/a17/atlas-deploy.service` — the
  root agent and its units, described above.
- `src/mcp/mcp_tools.c` gained four remote-only tools,
  `atlas_deploy_propose/status/list/cancel`, carrying
  `ATLAS_SCOPE_JOBS_SUBMIT` (the only scope available to a submit-credential
  session; the two confirmation methods have no MCP tool at all — disposal
  and deploy confirmation are both browser-only capabilities by design, never
  reachable from a model).
- `src/gw/gwpolicy.c` parses and validates `remote_deploy_key` and
  `operator_accepts_cleartext_deploy`, refusing every malformed combination
  described above under §What this reverses.
- `src/gw/gateway.c` carries the six `/api/v1/deploy/*` routes in
  `API_WRITE_ROUTES[]`, the `route_alt_scope`/`route_scope_granted` pair that
  lets `/get` and `/list` accept either credential, and the `deploy:`/
  `clear-deploy:` lines `atlas gateway status` prints, human and JSON alike.
- `src/gw/ui/mission-control.html` gained a Deploys panel (`viewDeploys`):
  a list from `deploy/list`, a detail view from `deploy/get`, and the deploy
  credential's own confirm dialog driving `deploy/challenge` then
  `deploy/confirm` with the typed eight-hex prefix, A16's pattern carried
  over.

Every route, status line and UI panel described in §Methods, routes and
tools and in §The flow above is live on this tree: the six
`/api/v1/deploy/*` gateway routes, the `deploy:`/`clear-deploy:` lines, and
Mission Control's Deploys view all landed after this document's first
sections were written, and nothing above should be read as a forward
description any longer.

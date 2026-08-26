# A13 Plan 7 — every reader that should see the mirror, and the two that must not

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Every daemon-side read of a repository resolves through the same
mirror-aware open, so a tree the daemon cannot read is not merely reconcilable
(Plan 6) but *watchable, searchable and semantically indexable*.

**Architecture:** Plan 6 put the fallback at one site inside `src/daemon`. Two
things are wrong with leaving it there: `src/core` may not depend on
`src/daemon`, and five other call sites still open a repository by its
registered root. This plan moves the helper down a layer and repoints the reads
that should follow it — and states, with reasons, the two that must not.

**Spec:** `docs/superpowers/specs/2026-08-26-per-user-scanner-design.md`
**Builds on:** Plans 1–6.

## Global Constraints

Same as Plan 1.

## Verified surface

Read from the tree at `389fb27`. Every row was read from the file named, not
recalled.

| Fact | Where |
| --- | --- |
| `atlas_daemon_open_index_root(data_dir, repo_id, root_path, &g, &from_mirror, err)` — tries the real root, then the mirror | `src/daemon/mirror.c:174` |
| It lives in `src/daemon`, which `src/core` may not depend on | `CLAUDE.md`, the layer map |
| `atlas_service_open_repo_git(info, out, err)` is the shared helper, and it has **five** callers | `src/core/service.c:226`; `diff.c:491`, `service_daemon.c:342`, `service_code.c:538`, `service.c:828`, `service_gate.c:245` |
| After opening, it enforces **two** identity checks: `atlas_git_root(*out)` must equal `info->root_path`, and `atlas_git_dir(*out)` must equal `info->git_dir` | `src/core/service.c:232-259` |
| A mirror fails both by construction — its root is `<data_dir>/mirror/<id>` and its git dir is that root's `.git` | Plan 3, Plan 5 |
| The watcher takes its root once, at `atlas_buf_set(&rw->root, ri->root_path.data, ri->root_path.len, err)`, and every later path comparison is a prefix test against it | `src/daemon/watch.c:1456`, compared at `825-830` |
| The watcher separately opens the repository to refresh its ignore inventory | `src/daemon/watch.c:1500` |
| `src/core/service_sem.c:881` opens by `repo.root_path` for the semantic pass | read at that line |
| `src/core/service.c:864` opens by `out->repo.root_path` for the live-HEAD observation | read at that line |
| `src/orch/rundriver.c:91` is `head_commit`, the run driver's pinned-commit read | read at `84-95` |
| `src/orch/snapshot.c:298` and `:439` build the workspace a worker is given | read at `292-300` |

---

## The two that must not move, and why

This is the load-bearing half of the plan. Both are in `src/orch`, and both
would look like ordinary reads to anyone repointing call sites mechanically.

**`src/orch/rundriver.c:91` — the pinned commit.** `CLAUDE.md` states the A11.1
rule: *"The pinned commit is checked before the worker and again after it. A
moved HEAD is refused rather than judged."* A worker edits the **real** tree.
If the check read a mirror, it would compare the real tree's work against a
copy taken before it — and a HEAD that moved underneath the worker would
compare equal. The guarantee would not fail loudly; it would silently always
pass, which is the worse failure.

**`src/orch/snapshot.c:298,439` — the worker's workspace.** The snapshot is
what a worker is given to work from. Building it from a mirror would hand the
worker a tree lagging the repository by the scanner's cycle, and every
gate verdict downstream would describe work done against the wrong base.

Neither exclusion is an oversight to be tidied up later. A13's premise is that
the *daemon* need not read the tree; it was never that *nothing* reads it. The
orchestrator runs a worker in the real tree by design, under the three-way
alignment `CLAUDE.md` describes, and that worker's principal can read it.

---

### Task 1: Move the helper below `src/core`

**Files:**
- Create: `include/atlas/mirror.h`, `src/core/mirror_open.c`
- Modify: `src/daemon/mirror.c` (drop the moved function), `src/daemon/daemon_internal.h` (drop the declaration), `src/daemon/writer.c` (call the new name), `CMakeLists.txt`
- Test: `tests/test_mirror_fallback.c` — the three existing cases must pass unchanged against the new name

- [ ] **Step 1:** Move `atlas_daemon_open_index_root` verbatim into
  `src/core/mirror_open.c` under the name `atlas_repo_open_git`, declared in
  `include/atlas/mirror.h`. Behaviour does not change in this task; only where
  it lives does.

- [ ] **Step 2:** Extract the path construction into
  `atlas_mirror_repo_path(data_dir, repo_id, out, err)` in the same file, and
  have `src/daemon/mirror.c`'s `atlas_mirror_open_repo` use it. Two spellings
  of `<data_dir>/mirror/<id>` are two answers to "where is the mirror".

- [ ] **Step 3:** Add `src/core/mirror_open.c` to the explicit `atlas_core`
  source list in `CMakeLists.txt`. Nothing is globbed; a file not listed fails
  as a link error, not a build error.

- [ ] **Step 4:** `make test` — the three fallback cases pass unchanged.

- [ ] **Step 5:** Commit.

---

### Task 2: The shared helper, and the check that does not apply

**Files:**
- Modify: `src/core/service.c` (`atlas_service_open_repo_git`), plus the five callers
- Test: extend `tests/test_mirror_fallback.c`

**The argument that must be written at the call site.** The two identity
checks assert that *the registered root still resolves to itself*. That is a
claim about the real tree. When the mirror answered, the real tree was not
opened, so the claim is not false — it is **unasked**, and asserting it against
the mirror would refuse a correct answer.

What stands in its place is not nothing, and the comment must say so: the
mirror is at a path Atlas derives from `repo_id` inside its own data directory,
and only a uid the repository's row names may write there (Plan 1's
`scanner_uid`, enforced in `src/ipc/server_scanner.c`'s `peer_owns`). The
warrant moved from "the path still resolves here" to "the row still names this
writer". **Write that down at the skip, or a later reader will delete the skip
or the checks.**

- [ ] **Step 1: Write the failing test.** A repository whose real root is
  unopenable, opened through `atlas_service_open_repo_git`, succeeds and does
  not raise `ATLAS_ERR_INTEGRITY`. Assert the *status*, not just success — the
  bug this guards against returns exactly that.

- [ ] **Step 2:** Run it; expect the integrity refusal.

- [ ] **Step 3:** Implement. The helper needs `data_dir` and `repo_id`;
  `atlas_repo_info` already carries `id`, so confirm whether `data_dir` can
  come from the same place the callers already have it rather than widening
  five signatures. **Check `atlas_ctx` before widening anything** — if the
  context already resolves the data directory, four of the five callers need no
  change at all.

- [ ] **Step 4:** Run the full non-daemon suite. Five callers move; a
  regression in `diff`, `gate`, `code` or `status` is what a mistake here looks
  like.

- [ ] **Step 5:** Commit.

---

### Task 3: The watcher

**Files:**
- Modify: `src/daemon/watch.c`
- Test: `tests/test_watch_budget.c` already starts a writer with a data dir

**What this does and does not claim.** Watching the mirror means the daemon
learns of a change when *the scanner writes it*, not when the developer saves
the file. The index is then current as of the scanner's last pass, and that is
a **weaker claim than the one the watcher makes today** for a readable tree.
It must be stated in `docs/watcher-consistency.md` rather than implied, and it
must not be described as equivalent.

Moving `src/daemon/watch.c` into the scanner — 3,486 lines, P0's budget
arithmetic re-derived per uid, `IN_Q_OVERFLOW` handling duplicated — is the
alternative, and it is a season of its own rather than a task in this one.

- [ ] **Step 1:** At `watch.c:1456`, choose the root the same way Plan 6 does:
  real first, mirror on failure. Every later prefix comparison follows from
  that one buffer, so nothing at `825-830` changes.

- [ ] **Step 2:** The ignore refresh at `1500` opens the repository again;
  route it through the same choice rather than re-deriving it.

- [ ] **Step 3:** A repository watched through its mirror **owes an event gap
  until a content-verifying pass completes**, exactly as P0 requires of a
  queued directory or a rebuilt watch set. Confirm the existing gap machinery
  covers this rather than assuming it does.

- [ ] **Step 4:** `make test`, then commit.

---

### Task 4: The gates and the documents

- [ ] `make test && make asan && make ubsan && make tsan && make adversarial`
- [ ] `docs/watcher-consistency.md`: what watching a mirror claims, and what it
  does not.
- [ ] `CLAUDE.md`: the A13 season line, and the two orchestration exclusions.
- [ ] Record a change reason, then report.

---

## What this plan deliberately does not do

- **The record still does not say which principal produced the facts.** Carried
  forward from Plan 6, unclosed, and still needing a column and a migration.
- No move of `src/daemon/watch.c` into the scanner. Stated above as its own
  season.
- No change to `src/orch`. Stated above, with reasons, as the point of the plan
  rather than an omission from it.

---

## Corrections folded in before implementation

**1. The helper never asks whether the row names a scanner.** Plan 6's
`atlas_daemon_open_index_root` consults the mirror on any failure of the real
root, with no reference to `scanner_uid`. Task 1 takes the whole
`atlas_repo_info` (it already carries `id`, `scanner_uid` and `root_path`) and
refuses the mirror when `scanner_uid == 0`.

**This is defence in depth, not a live bug fix, and the difference matters.**
Checked before writing this: `atlas_scanner_uid_refusal` refuses uid 0 at
assignment — *"uid 0 is how Atlas records \"no scanner assigned\", so it cannot
also name one"* (`src/core/scanner_uid.c:42`) — so a repository cannot be
walked back to 0 through `repo scanner`, and a mirror can only be written by a
peer that `peer_owns` admitted, which requires a non-zero `scanner_uid`. The
reachable path the check would close does not currently exist.

It goes in anyway, for the reason M21's C checks went in beside the index: the
warrant printed at the skip in Task 2 is *"the row still names this writer"*,
and that sentence should be true because the code asks, not because a refusal
in a different file makes the alternative unreachable. One comparison.

**2. Stale mirrors are a real gap and this plan does not close one.** A scanner
that stops running leaves a frozen mirror, and the daemon will keep indexing it
and keep reporting the index current — against bytes that may be arbitrarily
old. Nothing in Plans 1–7 bounds that. It is a freshness question, not an
authority one, and it needs a recorded observation time and a staleness rule of
its own. **Written here because it is the largest thing A13 leaves open.**

**3. Two readers were missing from the tasks.** The goal sentence promises
"searchable and semantically indexable", and neither `src/core/service_sem.c:881`
nor `src/core/service.c:864` goes through `atlas_service_open_repo_git`. Both
open by `root_path` directly. They join Task 2 as callers of the widened
helper.

**4. `src/core/service.c:597` is a third exclusion.** It is the open performed
at registration, before any row exists. No row means no `repo_id`, which means
no mirror can exist to consult. Real tree only, by construction.

**5. The helper's signature.** One helper, widened — not a `_mirrored` sibling.
Two spellings of one question drift, and a per-call-site choice of "is this the
daemon or the CLI?" is the inference A7 forbids making. The shape is
`atlas_service_open_repo_git(const atlas_repo_info *info, const char *data_dir,
atlas_git **out, atlas_err *err)`, and **`data_dir == NULL` means the tree
itself is the only acceptable source** — absence as the guarantee, so a call
site that passes nothing gets today's behaviour rather than a surprise.
`diff.c:487` and `service_gate.c:173` take no `ctx`: **check whether their own
callers hold one before threading it**, and if a chain has none, that reader
passes NULL and the plan says so.

**6. Task 3 owes a gap on the *switch*, not only on the watch build.**
Reconcile chooses its root per pass; the watcher chooses per watch build. A
repository whose readability changes between the two has them reading different
sources until the watch is rebuilt. Any change in which source answered owes an
event gap until a content-verifying pass completes — the same rule P0 applies
to a rebuilt watch set.

**7. Verified: neither `diff` nor `gate` has a `ctx` anywhere in its chain.**
`atlas_service_diff_repo` is reached from `atlas_service_diff_remote(const char
*name, ...)` (`src/core/service_remote.c:982`) and `build_env` from
`atlas_gate_run(atlas_db *db, ...)` (`src/core/service_gate.c:269`). Both take
a database or a name and no context.

So both pass `NULL` and stay real-tree-only. For `diff` that is unremarkable —
it is a read an operator runs. For the **gate it is a real limitation and must
be written down**: a daemon-side gate against a tree the daemon cannot read
fails closed, and A6 says a gate that cannot establish its facts refuses rather
than guesses, so it fails in the correct direction — but it fails.

Closing it means widening `atlas_gate_run`'s signature, which is a public
contract shared by the RPC surface, and A6's own rule is that the gate takes a
database and creates nothing. That is a deliberate change to an A6 contract and
belongs in its own change with its own argument, not as a side effect of
threading a parameter through seven files.

**8. Verified: both readers added in correction 3 can be supplied, and one of
their two call chains should not be.**

`atlas_sem_index_on(atlas_db *db, const atlas_repo_info *repo_in, ...)`
(`src/core/service_sem.c:857`) has exactly two callers, and **both already hold
a data directory**: `service_sem.c:837` passes `atlas_ctx_db(ctx)` so it has the
`ctx`, and `src/daemon/writer.c:987` has `w`, which Plan 6 gave `data_dir`.
Widening it is a two-call-site change.

`atlas_service_status_observe_live` has two callers and they differ:
`atlas_service_status(atlas_ctx *ctx, ...)` (`service.c:883`) can supply one;
`atlas_service_status_remote(const char *name, ...)` (`service_daemon.c:561`)
cannot — and **should not**. That is the remote path, where the index facts
arrive over the socket and the live HEAD is observed by the *client*, which
runs as the operator and can read the tree. Passing NULL there is the correct
answer rather than a limitation, and `service.c:860`'s own comment already says
the observation needs nothing but the report's `root_path`.

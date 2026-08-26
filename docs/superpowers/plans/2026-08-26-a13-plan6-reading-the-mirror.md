# A13 Plan 6 — reading the mirror

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** A repository the daemon cannot open is indexed from its mirror, so
`index_current` can become true for a tree the daemon has never read.

**Architecture:** One fallback at one site. `run_reconcile` opens the real root
as it does today; when that fails and a mirror exists, it opens the mirror
instead and runs the identical pass. Reconcile, A3, the semantic layer,
snapshots and gates are untouched — Plan 5 made the mirror a repository so that
nothing above it would have to change.

**Spec:** `docs/superpowers/specs/2026-08-26-per-user-scanner-design.md`
**Builds on:** Plans 1–5.

## Global Constraints

Same as Plan 1.

## Verified surface

Read from the tree at `62c6ccd`.

| Fact | Where |
| --- | --- |
| The daemon opens a repository for reconcile with `atlas_git_open(atlas_buf_cstr(&info.root_path), &g, &err)` and, on failure, logs `"repository %s cannot be opened: %s"` and calls `atlas_db_index_state_set_error` | `src/daemon/writer.c:559-568` |
| `atlas_reconcile_run(w->db, g, j->repo_id, &opts, &sum, &perr)` takes the `atlas_git *` and nothing else about the root | `src/daemon/writer.c:606` |
| Reconcile's root descriptor is `atlas_git_root_fd(g)` throughout | `src/core/reconcile.c:1247, 1258, 1452` |
| The mirror lives at `<data_dir>/mirror/<repo_id>` | `src/daemon/mirror.c` (Plan 3) |
| `atlas_server_ctx.data_dir` is a `const char *`; the writer has `w->...` — **confirm what the writer actually holds** before using it | `src/daemon/daemon_internal.h:622-654` |
| The measured failure this fixes: `git rev-parse HEAD` as `atlasd` on `/opt/atlas` answers `fatal: not a git repository`, and the daemon logged `repository atlas cannot be opened` every ten seconds | this session |

**Confirm before implementing** what the writer has access to for the data
directory. `run_reconcile` is in `src/daemon/writer.c`; find the field rather
than assuming `w->data_dir` exists.

---

### Task 1: Fall back to the mirror

**Files:**
- Modify: `src/daemon/writer.c`
- Test: a daemon-labelled test, or extend an existing one

- [ ] **Step 1: Write the failing test**

The claim, end to end: a repository whose real root the daemon cannot open, but
whose mirror exists, reconciles from the mirror and reports files.

Simulating "cannot open" in a fixture is the hard part. The honest simulation is
a registered repository whose `root_path` points somewhere that is not a git
repository at all — that is the same failure `atlas_git_open` produces, from the
same call, without needing a second uid.

- [ ] **Step 2: Run it and watch it fail**

- [ ] **Step 3: Implement**

In `run_reconcile`, when `atlas_git_open` on the real root fails:

1. Build `<data_dir>/mirror/<repo_id>`.
2. `atlas_git_open` it. If that fails too, log and set the error exactly as
   today — the current behaviour is the fallback's fallback.
3. If it succeeds, log **which source was used**, at info rather than warn: a
   repository indexed from its mirror is working as designed, not degraded.

The rest of the function is unchanged. `atlas_reconcile_run` takes the
`atlas_git *` and never learns which root it came from, which is the whole
point of Plan 5.

- [ ] **Step 4: Run it, then drive it end to end**

- [ ] **Step 5: Commit**

---

### Task 2: The gates

- [ ] `make test && make asan && make ubsan && make tsan && make adversarial`
- [ ] Record a change reason, then report.

---

## What this plan deliberately does not do

- **The record does not yet say which principal produced the facts.** The spec's
  refined invariant is "file facts are produced by a principal that can read the
  files, **and the record says which principal produced them**." This plan logs
  it; it does not store it. That is a real gap against the invariant, it needs a
  column and therefore a migration, and inventing one inside a fallback would be
  the wrong place. It is written here so it is not lost.
- No watcher change. A mirrored repository is reconciled on the daemon's own
  schedule and its watch state is whatever the watcher already made it. Moving
  the watcher is Plan 7.
- No preference change for repositories the daemon *can* read. Those still use
  the real root, because reading the thing itself is better evidence than
  reading a copy of it.

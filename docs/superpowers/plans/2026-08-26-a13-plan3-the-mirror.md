# A13 Plan 3 — the mirror

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A scanner hands the daemon a file's bytes, and the daemon ends up
holding a readable copy of it under its own data directory.

**Architecture:** One method, `scanner.put`, carrying a repository, a relative
path and a chunk of content. The daemon writes it into
`<data-dir>/mirror/<repo-id>/<path>` through `openat`/`O_NOFOLLOW` from a
descriptor validated once — the arrangement `src/orch/workspace.c` already uses
to materialise a snapshot. Nothing reads the mirror yet: this plan is the
write path and the safety around it.

**Tech Stack:** C17, CMake ≥ 3.16, `tests/atlas_test.h`. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-08-26-per-user-scanner-design.md`
**Builds on:** Plan 1 (`scanner_uid`), Plan 2 (`scanner.` group,
`require_scanner`).

## Global Constraints

Same as Plan 1 — see
`docs/superpowers/plans/2026-08-26-a13-plan1-record-and-identity.md`. Every rule
there applies unchanged.

## Verified surface

Read from the tree at `b40f677` before this plan was written.

| Fact | Where |
| --- | --- |
| `bool atlas_snapshot_path_ok(const void *path, size_t len)` — rejects absolute, empty, NUL, `.`, `..`, over-long | `include/atlas/snapshot.h:167`, `src/orch/snapshot.c:38-60` |
| A workspace opens every component with `openat(..., O_RDONLY\|O_DIRECTORY\|O_CLOEXEC\|O_NOFOLLOW)` from a descriptor validated once, never a path string | `src/orch/workspace.c:6, 96` |
| `mkdirat(parent, name, 0700)` tolerating `EEXIST`, then `openat` with `O_NOFOLLOW` | `src/orch/workspace.c:130-136` |
| First chunk creates with `O_WRONLY\|O_CREAT\|O_EXCL\|O_CLOEXEC\|O_NOFOLLOW, 0600` | `src/orch/workspace.c:286` |
| Later chunks append with `O_WRONLY\|O_APPEND\|O_CLOEXEC\|O_NOFOLLOW` and **no** `O_CREAT`, so a chunk for a file that does not exist is a broken stream rather than a new file | `src/orch/workspace.c:313-322` |
| A chunked wire method reads `token`, `index`, `offset` with `atlas_ipc_param_str/int` and refuses each absent one by name | `src/ipc/server_orch.c:2293-2311` |
| `dispatch_state` carries `ctx` (with `data_dir`), `db`, `j`, `safe`, `peer_uid` | `src/ipc/server_internal.h:29-55` |
| `atlas_server_ctx.data_dir` is a `const char *` | `src/daemon/daemon_internal.h:622-654` |
| `require_scanner(dispatch_state *, atlas_err *)` and `peer_owns()` | `src/ipc/server_scanner.c` (Plan 2) |
| `atlas_status atlas_db_repo_get_by_id(atlas_db *db, int64_t repo_id, atlas_repo_info *out, bool *found, atlas_err *err)` | `include/atlas/db.h:331` |
| `atlas_repo_info.scanner_uid` is `int64_t` | `include/atlas/db.h` (Plan 1) |

### What the verification settled

**The mirror does not need a new write mechanism.** `src/orch/workspace.c`
already materialises attacker-influenced paths into a directory the daemon
owns, and its rule is stated at the top of the file: *every component opened
with `openat` and `O_NOFOLLOW` from a descriptor that was validated once, never
a path string.* The mirror is the same problem with a different destination, so
it gets the same treatment rather than a second one.

**Chunking is not invented here either.** `dispatch.snapshot.open` /
`dispatch.snapshot.chunk` already carry bytes over this socket in pieces, and
the `O_EXCL`-then-append split at `src/orch/workspace.c:286` and `313-322` is
the shape that makes a resumed stream detectable: a chunk whose file does not
exist means the stream broke, and `O_CREAT` on the append path would silently
paper over it.

---

### Task 1: The mirror root, and one file into it

**Files:**
- Create: `src/daemon/mirror.c`, `src/daemon/mirror.h`
- Modify: `CMakeLists.txt`
- Test: `tests/test_mirror.c` (create)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:

```c
/* Opens `<data_dir>/mirror/<repo_id>`, creating it if absent, and returns a
 * descriptor the caller closes. Every component is created with `mkdirat` at
 * 0700 and opened with `O_NOFOLLOW`, so a symlink anywhere on the way refuses
 * rather than redirects. */
atlas_status atlas_mirror_open_repo(const char *data_dir, int64_t repo_id, int *fd_out,
                                    atlas_err *err);

/* Writes `len` bytes of `data` at `rel` beneath `root_fd`.
 *
 * `first` creates the file with O_EXCL, replacing any previous content by
 * unlinking first; a later chunk appends and refuses if the file is absent,
 * because that means the stream broke rather than that a new file began.
 * `rel` must satisfy `atlas_snapshot_path_ok`; a path that does not is refused
 * without being opened. */
atlas_status atlas_mirror_put(int root_fd, const void *rel, size_t rel_len, bool first,
                              const void *data, size_t len, atlas_err *err);
```

- [ ] **Step 1: Write the failing test**

Create `tests/test_mirror.c`. The cases, in this order:

1. **A file round-trips.** Open a mirror root under a fixture data dir, put
   `a/b/c.txt` in one chunk, then read it back from the filesystem and compare
   bytes.
2. **Two chunks append.** `first=true` then `first=false`; the file holds the
   concatenation.
3. **A second `first=true` replaces rather than appends.** The same path put
   twice from the start holds only the second content — a rescanned file must
   not accumulate.
4. **An unsafe path is refused and nothing is created.** `../escape`,
   `/absolute`, `a//b`, `a/./b`, `a/../b` and a path with an embedded NUL each
   return non-`ATLAS_OK`; after all of them the mirror root holds no entry
   beyond what case 1 put there.
5. **A symlinked component refuses.** Create `a` as a symlink to `/tmp` inside
   the mirror root, then put `a/x`; it must fail rather than write through the
   link. This is the case the whole `O_NOFOLLOW` discipline exists for, and it
   is the one a careless rewrite would lose.
6. **An append to a file that does not exist refuses.** `first=false` on an
   unknown path fails; it does not create.

- [ ] **Step 2: Run it and watch it fail**

```sh
cd /opt/atlas && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j 4 --target test_mirror
./build/tests/test_mirror
```

Expected: FAIL — `atlas/mirror.h` does not exist.

- [ ] **Step 3: Implement**

Follow `src/orch/workspace.c` rather than writing a second discipline: walk the
path component by component with `openat(parent, comp, O_RDONLY|O_DIRECTORY|
O_CLOEXEC|O_NOFOLLOW)`, creating each with `mkdirat(parent, comp, 0700)` and
tolerating `EEXIST`. Open the leaf with `O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|
O_NOFOLLOW, 0600` when `first`, unlinking any previous entry with `unlinkat`
first; otherwise `O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW` with **no** `O_CREAT`.

Refuse before opening anything when `atlas_snapshot_path_ok` says no.

Add `src/daemon/mirror.c` to the `atlas_core` source list in `CMakeLists.txt`.

- [ ] **Step 4: Run it and watch it pass, then the subset**

```sh
cmake --build build -j 4 && ./build/tests/test_mirror
cd build && ctest -LE daemon -j 4
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(a13): the mirror - bytes the daemon can read, written the way a workspace is"
```

---

### Task 2: `scanner.put`

**Files:**
- Modify: `src/ipc/server_scanner.c`
- Test: `tests/test_scanner_rpc.c` (extend)

**Interfaces:**
- Consumes: Task 1's mirror, Plan 2's `require_scanner` and `peer_owns`.
- Produces: `scanner.put` taking `repo` (int), `path` (string), `first` (bool)
  and `data` (string), and answering `{"written":N}`.

- [ ] **Step 1: Write the failing test**

Extend `tests/test_scanner_rpc.c`. Two cases, and the second is the one that
matters:

1. **A scanner writes into its own repository's mirror.** `scanner.put` with
   the fixture's own repository id succeeds, and the bytes appear under
   `<data-dir>/mirror/<id>/`.
2. **A scanner may not write into another repository's mirror.** The same call
   naming `theirs` — the repository whose `scanner_uid` is `getuid() + 1` —
   is refused, and **nothing is created under that repository's mirror path**.
   Assert the absence on the filesystem, not only the refusal: a refusal that
   had already written would be the whole design failing quietly.

- [ ] **Step 2: Run it and watch it fail**

Expected: FAIL — `unknown method "scanner.put"`.

- [ ] **Step 3: Implement**

`method_scanner_put` reads its four parameters, refusing each absent one by
name as `method_snapshot_chunk` does. Then, in this order and no other:

1. `require_scanner(ds, err)` — is this peer any repository's scanner?
2. `atlas_db_repo_get_by_id` for the named repository.
3. `peer_owns(ds, &ri)` — **is it this one's?** The refusal here is the load-
   bearing one; without it a scanner could write into a mirror belonging to a
   repository it does not own.
4. `atlas_snapshot_path_ok` on the path.
5. `atlas_mirror_open_repo` and `atlas_mirror_put`.

Steps 1-4 all precede any filesystem work, so a refused call leaves nothing
behind.

- [ ] **Step 4: Run it, then the subset**

```sh
cmake --build build -j 4 && ./build/tests/test_scanner_rpc
cd build && ctest -LE daemon -j 4
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(a13): scanner.put writes into the mirror of a repository the peer owns"
```

---

### Task 3: The gates

- [ ] **Step 1: Full matrix**

```sh
cd /opt/atlas && make test && make asan && make ubsan && make tsan && make adversarial
```

The adversarial suite matters more here than in Plans 1 and 2: this is the
first A13 code that creates files from attacker-influenced names.

- [ ] **Step 2: Record a change reason, then report.**

---

## What this plan deliberately does not do

- No observations. `scanner.put` carries content and nothing about identity,
  hashes or generations; the file index is untouched.
- Nothing reads the mirror. `src/code`, `src/sem` and `src/orch` still open the
  repository root, and this plan does not repoint them.
- No deletion, no garbage collection, no size accounting. A mirror that has
  grown stale is a later plan's problem, and pretending otherwise here would be
  a bound nobody implemented.

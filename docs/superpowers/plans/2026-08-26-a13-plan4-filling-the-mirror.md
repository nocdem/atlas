# A13 Plan 4 — filling the mirror

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After one `atlas scanner run --once`, the daemon holds a readable copy
of every tracked file in a repository it cannot read itself.

**Architecture:** The scanner opens its repository as the owner, asks git for
the tracked paths, reads each file through `openat`/`O_NOFOLLOW`, and sends the
bytes with `scanner.put`. Nothing in the daemon changes: Plan 3's mirror and
`scanner.put` already accept them. This closes the loop end to end without
touching A1's rules, which is deliberate — the file index still comes from the
daemon's own reads, and moving that is the next plan's work, not this one's.

**Tech Stack:** C17, CMake ≥ 3.16, `tests/atlas_test.h`. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-08-26-per-user-scanner-design.md`
**Builds on:** Plan 2 (`scanner.poll`, `atlas scanner run`), Plan 3 (the mirror,
`scanner.put`).

## Global Constraints

Same as Plan 1 — see
`docs/superpowers/plans/2026-08-26-a13-plan1-record-and-identity.md`.

## Verified surface

Read from the tree at `a79fb1b` before this plan was written.

| Fact | Where |
| --- | --- |
| `atlas_status atlas_git_open(const char *path, atlas_git **out, atlas_err *err)` / `atlas_git_close` / `const char *atlas_git_root(const atlas_git *g)` | `include/atlas/git.h:34-37` |
| `atlas_status atlas_git_ls_files(atlas_git *g, atlas_git_index_cb cb, void *ud, atlas_err *err)` | `include/atlas/git.h:138` |
| `atlas_git_index_entry { const char *mode; const char *oid; int stage; const void *path; size_t path_len; }` — path is **raw bytes**, not a C string | `include/atlas/git.h:127-133` |
| `atlas_status atlas_path_open_nofollow(int root_fd, const char *rel, size_t rel_len, atlas_path_open_result *result_out, int *fd_out, struct stat *st_out, int *errno_out, atlas_err *err)` | `include/atlas/pathrep.h:53-55` |
| `atlas_status atlas_ipc_call(const char *socket_path, const char *method, const char *params_json, atlas_buf *response_out, atlas_err *err)` | `include/atlas/ipc.h:214-215` |
| `bool atlas_ipc_result_arr_len(const atlas_ipc_response *r, const char *arr_key, size_t *out)` and `atlas_ipc_result_arr_obj_str(..., size_t index, const char *key, const char **out)` | `include/atlas/ipc.h:340, 306` |
| `scanner.put` takes `repo` (int), `path` (string), `first` (bool), `data` (string) | `src/ipc/server_scanner.c` (Plan 3) |
| `scanner.poll` answers `repositories[]` with `id`, `name`, `root` | `src/ipc/server_scanner.c` (Plan 2) |
| `atlas_service_scanner_run(bool once, FILE *log, atlas_err *err)` | `include/atlas/service.h:1266` (Plan 2) |

### The one thing this plan must not get wrong

**`scanner.put` takes `data` as a JSON string**, and a source file is arbitrary
bytes. A file containing a quote, a backslash, a newline or a byte that is not
valid UTF-8 must survive the round trip or the mirror is a copy of something
else.

The plan's first task is therefore the encoding, not the walk, and its test is
a file of deliberately hostile bytes. If the encoding cannot carry them, the
right answer is to change the wire format now — before a walk is written on top
of it — rather than to quietly mirror only the files that happen to be tidy.

---

### Task 1: Prove what the wire can carry, and pick the encoding

**Files:**
- Test: `tests/test_scanner_rpc.c` (extend)
- Possibly modify: `src/ipc/server_scanner.c`

- [ ] **Step 1: Write the failing test**

Extend `tests/test_scanner_rpc.c` with a case that puts a file whose content is

```c
static const char HOSTILE[] = "a\"b\\c\nd\te\x01\xff\xfe";
```

through `scanner.put`, then reads the mirrored file back from the filesystem
and compares byte for byte. Note `\xff\xfe` is not valid UTF-8 and `\x01` is a
C0 control — both are legal in a source file and both are what a JSON string
will not carry unchanged.

- [ ] **Step 2: Run it and watch it fail**

```sh
cd /opt/atlas && cmake --build build -j 4 --target test_scanner_rpc && ./build/tests/test_scanner_rpc
```

Expected: FAIL. Record **how** it fails — a refused request, a truncated file
or altered bytes are three different answers and they lead to different fixes.

- [ ] **Step 3: Change the wire format to carry bytes**

`data` becomes a **hex** string: two lowercase hex digits per byte, and the
method refuses an odd length or a non-hex digit rather than guessing. Hex
rather than base64 because Atlas already has `atlas_hex_encode` and a decoder
is ten lines, and because a wire value that is obviously not text is harder to
mistake for one.

Check `include/atlas/sha256.h` (or wherever `atlas_hex_encode` is declared) for
the exact name and signature before using it — do not assume.

Add a `bytes` count to the response so a caller can tell a short write from a
successful one.

- [ ] **Step 4: Run it and watch it pass, then the whole subset**

```sh
cmake --build build -j 4 && ./build/tests/test_scanner_rpc
cd build && ctest -LE daemon -j 4
```

- [ ] **Step 5: Commit**

```bash
git commit -m "fix(a13): scanner.put carries bytes, not text"
```

---

### Task 2: The scanner walks and mirrors

**Files:**
- Modify: `src/core/service_scanner.c`
- Test: a new case in `tests/test_scanner_rpc.c`, or a daemon-labelled test if
  the walk needs a live daemon — decide from what the existing harness supports
  and say which you chose.

**Interfaces:**
- Consumes: `atlas_git_open`, `atlas_git_ls_files`, `atlas_path_open_nofollow`,
  `scanner.put`.
- Produces: `atlas scanner run --once` mirrors every tracked file of every
  repository `scanner.poll` names, and reports counts.

- [ ] **Step 1: Write the failing test**

The end-to-end claim: after a run, `<data-dir>/mirror/<id>/<path>` exists for
each tracked file and holds the same bytes as the repository's copy.

- [ ] **Step 2: Run it and watch it fail**

- [ ] **Step 3: Implement**

For each repository `scanner.poll` names:

1. `atlas_git_open` on its root. A repository that cannot be opened is
   **reported and skipped**, not fatal: one unreadable repository must not stop
   the others, and the reason belongs in the log where an operator will look.
2. `atlas_git_ls_files` for the tracked paths. `e->path` is raw bytes of
   `e->path_len` — it is not NUL-terminated, and treating it as a C string is
   the bug this note exists to prevent.
3. For each path, `atlas_path_open_nofollow` from the repository root
   descriptor. A symlink is **not** followed and its content is not mirrored;
   Atlas hashes a tracked symlink's link text rather than its target, and
   mirroring the target would import a file from outside the repository.
4. Read bounded — refuse a file over a stated ceiling rather than reading it
   into memory whole — and send it with `scanner.put`.

State the ceiling as a named constant with its reason beside it.

- [ ] **Step 4: Run it, then drive the built binary against a real daemon**

The daemon on this machine runs an older binary and will answer
`unknown method`. That is a correct observation, not a failure of this plan;
say so rather than working around it.

- [ ] **Step 5: Commit**

---

### Task 3: The gates

- [ ] **Step 1:** `make test && make asan && make ubsan && make tsan && make adversarial`
- [ ] **Step 2:** Record a change reason, then report.

---

## What this plan deliberately does not do

- **The file index is untouched.** It still comes from the daemon's own reads,
  and a repository the daemon cannot read is still not current. Moving that is
  the next plan.
- No deletion. A file removed from the repository stays in the mirror; the
  mirror is append-and-replace only, and pretending otherwise would be a
  guarantee nobody implemented.
- No untracked files, no `.git` contents. Tracked content only, which is what
  the spec's 13 MB measurement was of.

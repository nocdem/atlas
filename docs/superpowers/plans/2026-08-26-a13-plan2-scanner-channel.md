# A13 Plan 2 — the scanner channel

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A process running as a repository's owner can connect to the daemon
and be told which repositories are its own — and no others.

**Architecture:** A new `scanner.` method group on the existing socket, with one
method. The group is dispatchable by name and refuses honestly; the real check
is `peer_uid == repositories.scanner_uid`, made inside the method where the
database is open. A new `atlas scanner run --once` connects, asks, and prints.
Nothing is scanned yet: this plan ships the channel and the identity check that
every later plan rests on.

**Tech Stack:** C17, SQLite3, CMake ≥ 3.16, `tests/atlas_test.h`. No new
dependencies.

**Spec:** `docs/superpowers/specs/2026-08-26-per-user-scanner-design.md`
**Builds on:** Plan 1 (`repositories.scanner_uid`, migration 27).

## Global Constraints

Same as Plan 1 — see
`docs/superpowers/plans/2026-08-26-a13-plan1-record-and-identity.md`, "Global
Constraints". Every rule there applies here unchanged.

## Verified surface

Every symbol below was read from the tree at `d6bb8c6` before this plan was
written. Nothing here is remembered or assumed.

| Fact | Where |
| --- | --- |
| `typedef struct atlas_method_entry { const char *name; atlas_method_fn fn; }` | `src/ipc/server_internal.h:85-88` |
| `typedef atlas_status (*atlas_method_fn)(dispatch_state *ds, const atlas_ipc_request *req, atlas_err *err)` | `src/ipc/server_internal.h:82-83` |
| `dispatch_state` carries `ctx`, `db` (read-only), `j`, `safe`, `peer_uid`, `peer_pid` | `src/ipc/server_internal.h:29-55` |
| Group accessor shape `const atlas_method_entry *atlas_server_X_methods(size_t *count_out)` | `src/ipc/server_orch.c:2386-2399` |
| Group accessors are declared in `server_internal.h` | `src/ipc/server_internal.h:91-199` |
| Method lookup is **additive** over groups, gated by `peer_uid` | `src/ipc/server.c:1176-1240` |
| **The database is opened at `src/ipc/server.c:1264`, after the method lookup** | `src/ipc/server.c:1264` |
| `bool atlas_ipc_param_str/int/bool(const atlas_ipc_request *req, const char *key, T *out)` | `include/atlas/ipc.h:187-189` |
| `require_dispatcher(dispatch_state *ds, atlas_err *err)` is the gate pattern | `src/ipc/server_orch.c:101-113` |
| A long-lived client is dispatched before any `atlas_ctx` is opened | `src/cli/cli.c:3786-3794` |
| `atlas_status atlas_service_dispatcher_run(bool once, FILE *log, atlas_err *err)` | `include/atlas/service.h:1256` |
| It refuses to start on misconfiguration rather than idling | `src/core/service_orch.c:405-412` |
| `atlas_status atlas_ipc_socket_path(atlas_buf *out, atlas_err *err)` | `include/atlas/ipc.h:99` |
| `atlas_status atlas_ipc_call(const char *socket_path, const char *method, const char *params_json, atlas_buf *response_out, atlas_err *err)` | `include/atlas/ipc.h:214-215` |
| `atlas_status atlas_db_repo_scanner_uid(atlas_db *db, int64_t repo_id, int64_t *out, atlas_err *err)` | `include/atlas/db.h` (Plan 1) |
| `atlas_status atlas_db_repo_list(atlas_db *db, atlas_repo_cb cb, void *ud, atlas_err *err)` | `include/atlas/db.h` |
| `atlas_repo_info.scanner_uid` is `int64_t`, 0 when unassigned | `include/atlas/db.h` (Plan 1) |

### The one design decision this verification forced

The spec says the `scanner.` group is "hidden the way the dispatcher group is".
**It cannot be, and it must not try.** Hiding a group requires a predicate over
`peer_uid` that can be answered *before* the method lookup — and the dispatcher's
is, because it reads a root-owned policy already in memory. The scanner's
identity is not in a policy; it is `repositories.scanner_uid`, in the database,
which `src/ipc/server.c:1264` does not open until after the lookup has finished.

So the group follows the **orchestration client group's** precedent instead,
which the dispatcher comment at `src/ipc/server.c:1137-1160` already describes:
always dispatchable by name, refusing honestly. Nothing is leaked by that here.
A caller learning that `scanner.poll` exists learns what reading the binary
would tell them, and the refusal — "no registered repository names uid N as its
scanner" — tells a caller only about its own uid, which it already knows.

`src/ipc/server.c:1137` states the governing rule outright: *"Routing is not
authorisation. Each method still calls `require_submitter` or
`require_dispatcher` for itself, so reaching a name is never the same as being
allowed to use it."*

---

### Task 1: `scanner.poll` and the uid that carries the design

**Files:**
- Create: `src/ipc/server_scanner.c`
- Modify: `src/ipc/server_internal.h` (declare the group accessor)
- Modify: `src/ipc/server.c` (consult the group)
- Modify: `CMakeLists.txt` (add the new `.c` to the `atlas_core` source list)
- Test: `tests/test_scanner_rpc.c` (create)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Plan 1's `repositories.scanner_uid` and `atlas_repo_info.scanner_uid`.
- Produces:

```c
/* The A13 scanner group. Always dispatchable by name; each method checks the
 * peer for itself, because the identity lives in the database and the database
 * is not open when the method lookup runs. */
const atlas_method_entry *atlas_server_scanner_methods(size_t *count_out);
```

  and one method, `scanner.poll`, whose response is:

```json
{"repositories":[{"id":2,"name":"atlas","root":"/opt/atlas"}],"scanner_uid":1000}
```

  `root` is the repository's `root_path_text` — already `%XX`-encoded in the
  database, so it is emitted as-is and **not** encoded a second time.

- [ ] **Step 1: Write the failing test**

Create `tests/test_scanner_rpc.c`. It drives the server dispatch directly rather
than over a socket, the way `tests/test_orch_rpc.c` does — read that file first
and follow its harness exactly; do not invent one.

```c
/* Atlas - A13: the scanner channel, and the uid check that carries it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0. */
```

The three cases, in this order:

1. **A scanner is told its own repositories.** Register two repositories,
   set one's `scanner_uid` to `getuid()` and the other's to `getuid() + 1`.
   Dispatch `scanner.poll` with `peer_uid = getuid()`. The response lists
   exactly one repository, and it is the first.
2. **A uid that owns nothing is refused, and the refusal names no repository.**
   Dispatch `scanner.poll` with a `peer_uid` no repository names. The call
   fails; the error message contains neither registered repository's name.
   This is the case that keeps the refusal from becoming an inventory.
3. **Uid 0 is never a scanner.** A repository whose `scanner_uid` is 0 is
   unassigned, so a peer arriving as uid 0 must not be handed it. Dispatch with
   `peer_uid = 0` against a database whose only repository has `scanner_uid = 0`
   and require a refusal.

- [ ] **Step 2: Run it and watch it fail**

```sh
cd /opt/atlas && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j 4 --target test_scanner_rpc
./build/tests/test_scanner_rpc
```

Expected: FAIL — `unknown method "scanner.poll"`.

- [ ] **Step 3: Implement `src/ipc/server_scanner.c`**

```c
/* A13. The scanner channel.
 *
 * A scanner runs as a repository's owner and reads a tree the daemon cannot.
 * What it may report about is decided here, and by one comparison: the peer's
 * uid from `SO_PEERCRED` against `repositories.scanner_uid`.
 *
 * The group is dispatchable by name rather than hidden. Hiding needs a
 * predicate answerable before the method lookup, and this one is not: the
 * identity is in the database, which `dispatch()` does not open until after the
 * lookup. Nothing is leaked by answering honestly — a caller learns only about
 * its own uid, which it already knows. `src/ipc/server.c:1137` is the rule this
 * follows: routing is not authorisation. */
```

`require_scanner(dispatch_state *ds, atlas_err *err)` walks `atlas_db_repo_list`
and returns `ATLAS_OK` when at least one repository has
`ri->scanner_uid == ds->peer_uid` **and** `ds->peer_uid != 0`; otherwise
`ATLAS_ERR_INTEGRITY` with a message that names no repository.

`method_scanner_poll` calls it, then walks the list again emitting only the
matching repositories.

Add to `src/ipc/server_internal.h` beside the other accessors:

```c
/* A13. The scanner group. Dispatchable by name; the peer check is per method,
 * because the identity is in the database and the database is not open when the
 * lookup runs. */
const atlas_method_entry *atlas_server_scanner_methods(size_t *count_out);
```

In `src/ipc/server.c`, after the orchestration-client group's block and in the
same additive shape, with no `peer_uid` predicate:

```c
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *g = atlas_server_scanner_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), g[i].name) == 0) {
                fn = g[i].fn;
                break;
            }
        }
    }
```

Add `src/ipc/server_scanner.c` to the `atlas_core` source list in
`CMakeLists.txt`. There is no glob; an unlisted file is a link error.

- [ ] **Step 4: Run it and watch it pass**

```sh
cmake --build build -j 4 --target test_scanner_rpc && ./build/tests/test_scanner_rpc
```

- [ ] **Step 5: Wire the test in and run the subset**

`tests/CMakeLists.txt`: `test_scanner_rpc` into `ATLAS_TESTS` and the `unit`
`LABELS` line if it needs no repository, the `integration` line if it does.
Decide from what the harness in `tests/test_orch_rpc.c` actually requires.

```sh
cd build && ctest -LE daemon -j 4
```

- [ ] **Step 6: Commit**

```bash
git add src/ipc/server_scanner.c src/ipc/server_internal.h src/ipc/server.c \
        CMakeLists.txt tests/test_scanner_rpc.c tests/CMakeLists.txt
git commit -m "feat(a13): the scanner channel, and the uid comparison that carries it"
```

---

### Task 2: `atlas scanner run --once`

**Files:**
- Modify: `include/atlas/service.h` (declare the entry point)
- Create: `src/core/service_scanner.c`
- Modify: `CMakeLists.txt`
- Modify: `src/cli/cli.c` (dispatch, help, `COMMANDS[]`)
- Test: extend `tests/test_scanner_rpc.c` or add a CLI case where the existing
  CLI suite lives — check `tests/test_cli.c` for the established pattern before
  choosing.

**Interfaces:**
- Consumes: Task 1's `scanner.poll`, `atlas_ipc_socket_path`, `atlas_ipc_call`.
- Produces:

```c
/* A13. Connects to the daemon and reports which repositories this uid may
 * scan. `once` polls a single time and returns; without it the loop is a later
 * plan's, and this plan refuses rather than idling. Logs to `log` so a systemd
 * user unit captures them in the journal. */
atlas_status atlas_service_scanner_run(bool once, FILE *log, atlas_err *err);
```

- [ ] **Step 1: Write the failing test**

A test that runs the built binary through `fx_atlas` — read
`tests/support/fixture.h` for the exact signature before writing the call, and
pass `--data-dir` explicitly: `fx_atlas` does not add it, and without it the
test opens the developer's real database.

Assert: with no daemon running, `atlas scanner run --once` fails with a message
naming the socket, and exits non-zero. That is the honest failure and it is
what a misconfigured unit will show in the journal.

- [ ] **Step 2: Run it and watch it fail**

Expected: FAIL — `unknown command`, because `COMMANDS[]` does not list
`scanner`.

- [ ] **Step 3: Implement**

`atlas_service_scanner_run` resolves the socket with `atlas_ipc_socket_path`,
calls `scanner.poll` with `atlas_ipc_call`, and prints one line per repository.
Without `--once` it returns a usage error saying the loop arrives in a later
plan — a process that idles silently looks healthy in `systemctl status` while
doing nothing, which `src/core/service_orch.c:405-412` refuses for the
dispatcher and this refuses for the same reason.

Five wiring places, and check each against Plan 1's experience:

1. `include/atlas/service.h` — the declaration.
2. `src/core/service_scanner.c` — the implementation, added to `CMakeLists.txt`.
3. `src/cli/cli.c` — dispatch, **before any `atlas_ctx` is opened**, exactly as
   `dispatcher` is at `src/cli/cli.c:3786`. A scanner talks over the socket and
   must not take the writer lock.
4. `src/cli/cli.c` — the help text.
5. `src/cli/cli.c` — **`COMMANDS[]` in `is_a_command`**. `scanner` is a new
   top-level command, unlike `repo scanner`, so this one is required. Plan 1's
   `--scanner-uid` was accepted everywhere and still answered `unknown option`
   until the argument parser's own allowlist learned it; check for the
   equivalent here by running the built binary.

No renderer method: the output is a list of names, and `renderer_open` is for
commands that produce a document. Follow whatever `dispatcher run` does — read
it rather than assuming.

- [ ] **Step 4: Run it, then drive the built binary**

```sh
cmake --build build -j 4
./build/atlas scanner run --once --data-dir /tmp/a13-p2 ; echo "exit=$?"
./build/atlas scanner --help 2>&1 | head -3
```

Expected: a clear failure naming the socket, non-zero exit, and `scanner`
appearing in help.

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(a13): atlas scanner run --once asks the daemon which repositories are this uid's"
```

---

### Task 3: The gates

**Files:** none — this task runs what exists.

- [ ] **Step 1: Full matrix**

Production code changed, so the whole matrix applies.

```sh
cd /opt/atlas
make test && make asan && make ubsan && make tsan && make adversarial
```

Expected: every suite passes, 0 leaks, 0 runtime errors, 0 races, adversarial
0 failed. Record the counts as observations, not adjectives.

- [ ] **Step 2: Record a change reason**

Atlas indexes itself. Record why these paths changed, truthfully, or `UNKNOWN`.

- [ ] **Step 3: Report**

State the new method and its group, the uid check and where it is made, the new
command and its five wiring places, and the gate counts.

---

## What this plan deliberately does not do

- No observations, no mirror, no file reads. `scanner.poll` answers "which
  repositories are mine" and nothing else.
- No liveness, no directives, no generation tokens. The daemon does not yet know
  or care whether a scanner is running, and no repository's currency depends on
  one.
- No loop. `--once` or a refusal.
- No migration. Plan 1's column is the only schema this plan needs.

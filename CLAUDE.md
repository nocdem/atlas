# Atlas — working notes for Claude Code

Atlas is a generic, headless engineering-memory and repository-intelligence CLI in
C17. Phase **A1**: a daemon, local IPC, and incremental indexing on top of the A0
read-only foundation. Not DNA-specific; DNA will later be its first indexed
repository.

## Build and test

```sh
make            # release -> build/atlas
make debug      # -> build-debug/atlas
make test       # release build + full CTest suite
make smoke      # CLI smoke test; uses the compiled C JSON checker, never Python
make adversarial# hostile-repository hardening checks under strace
make asan       # ASan + LSan build, then the suite
make ubsan      # UBSan build, then the suite
make tsan       # ThreadSanitizer build, then the suite (A1 is threaded)
make verify-vendor  # re-check vendored third-party digests
make install    # honours PREFIX, default /usr/local/bin/atlas
make clean
make compiledb  # refresh the top-level compile_commands.json symlink

sh scripts/perf.sh build   # A1 performance acceptance measurements
```

CMake is canonical; the Makefile is a wrapper. Run one suite with
`cd build && ctest -R test_scan --output-on-failure`. The live-daemon suite is
labelled and serialised: `ctest -L daemon`, `ctest -LE daemon`.

Requires: C17 compiler, CMake ≥ 3.16, Make, pkg-config, SQLite3 dev headers, Git,
pthreads. Nothing is downloaded at build time. **No Python, Node, Go, Rust, pip,
npm, or virtualenvs** anywhere in the build, tests, or runtime.

## Third-party code

Exactly one dependency is vendored: **yyjson**, used only to parse untrusted JSON
on the IPC boundary. Byte-for-byte upstream, pinned by tag and digest in
`third_party/yyjson/PROVENANCE.md`, verified by `scripts/verify_third_party.sh`
which runs as part of the suite. `ATLAS_WERROR` is deliberately not applied to
it — editing upstream source to silence a warning would break the digest
guarantee. First-party and third-party LOC are counted separately.

Responses are **not** built with yyjson. They go through the first-party
streaming writer, so A0's escaping contract is the one the daemon speaks.

## Hard rules

- **Never modify a registered target repository.** Read-only, always.
- **No shell.** No `system()`, `popen()`, or `/bin/sh -c`. Create processes only
  through `atlas_proc_run` with an explicit argv array and an absolute `argv[0]`.
- **No commits, pushes, amends, rebases, resets, or checkouts** in this repo unless
  the user explicitly asks.
- **Warnings are errors** in first-party code (`ATLAS_WERROR=ON`). Fix the cause;
  never suppress warnings globally to make a build pass.
- **No new third-party dependencies**, no `FetchContent`, no network at build time.
  A missing system dependency must halt with a clear message. Vendoring anything
  new needs an exact upstream tag, an archive digest, per-file digests, the
  upstream licence, and an entry in `scripts/verify_third_party.sh`.
- Tests must always override the data directory with a temporary path and must
  never open the real user database. The daemon suite additionally overrides
  `XDG_RUNTIME_DIR` into the fixture, so no test ever touches the real socket.
- **Never install, enable or start a real systemd service** from code or from a
  test. `atlas service install` writes a unit and does nothing else; the suite
  exercises it against temporary XDG fixtures only.

## Architecture invariants

1. SQLite is a **rebuildable index**, never the canonical record of history.
2. Git and repository contents are **authoritative** for source and history facts.
3. Every result **preserves provenance**.
4. Evidence types: `SOURCE`, `GIT`, `DECISION`, `USER_STATEMENT`, `INFERENCE`,
   `UNKNOWN`. **A0 and A1 may write only `SOURCE` and `GIT`** — enforced in
   `atlas_db_evidence_insert`, not by convention.
5. **Atlas never infers a historical reason.** A reason request returns
   `UNKNOWN`. A commit subject is `GIT` evidence of what was written, not a
   reason.
6. Repository contents, filenames and Git metadata are **untrusted input**.
7. All parsers are **bounded** and fail clearly on malformed input.
8. Schema changes use **numbered transactional migrations**.
9. Human and JSON output consume **identical service results**.
10. The CLI and any future adapter share **one service layer**.
11. **Exactly one process writes the index at a time**, enforced by an advisory
    lock on `<data-dir>/atlas.lock`, not by convention.
12. **Atlas never claims the index is current when it cannot prove it.** An
    event gap makes `index_current` false until a full pass resolves it, and only
    a full pass may clear it.

## A1 concurrency rules — these are not negotiable

- **One writer thread owns the only writable SQLite handle.** It creates it and
  never shares it. Every write in the daemon happens there.
- **A SQLite connection is never shared between threads.** Readers open their own
  with `atlas_db_open_readonly`, one per IPC request.
- **No git process and no file read happens inside a write transaction.** Passes
  observe, select, hash, then apply in batches of `ATLAS_DB_BATCH_MAX`.
- **Never hold `BEGIN IMMEDIATE` across unbounded work.** If a loop can run long,
  the transaction is per batch, not per loop.
- **Do not hide `SQLITE_BUSY` behind a longer `busy_timeout`.** With one writer it
  should not arise; if it does, something outside the daemon took the write lock,
  which is what the data-directory lock exists to prevent.
- **Worker jobs touch no database handle and create no process.** They index the
  caller's array by job index and write only their own slot.
- **Freeze git runtime state before creating threads** (`atlas_git_runtime_init`).
- A pass re-reads HEAD before committing. If it moved, the pass is **abandoned**,
  not committed. A branch switch must never leave the index describing a mixture.

## A1 cache-hit rules — do not weaken these

- **The filesystem identity is all eight fields**: device, inode, size, mode,
  mtime sec+nsec, **ctime sec+nsec**. Any missing field means *unknown*, not
  *unchanged*. Never drop ctime: mtime is writable via `utimensat`, so without
  ctime a same-length in-place edit with the mtime restored compares as unchanged
  forever and the stale hash is served indefinitely. Nothing in userspace can set
  ctime.
- **There is one identity type**, `atlas_fs_identity`, assigned whole. Do not
  introduce a parallel struct and copy field by field — that is exactly how ctime
  went missing between the stat and the database.
- **A path the watcher named is always hashed.** Metadata equality must never
  suppress an explicit event: an event is evidence, a metadata tuple is an
  inference the file's writer can manipulate.
- **"Full" means content verification, not a thorough stat.** Only a pass that
  read every eligible file may clear an event gap, and the gate is the pass's own
  `content_verified`, computed from what it did — not from `opts->full`, which is
  only what was asked for.
- **A racy observation is stored as unknown, not as a value.** See
  `docs/watcher-consistency.md` for the exact rule.

## A1 layers — additions

```
src/ipc      frame codec, socket policy, request parsing (yyjson), serve loop
src/daemon   writer thread, worker pool, inotify watcher, run loop
src/core     reconcile.c (the incremental pass), lock.c, unit.c,
             service_daemon.c
```

The serve loop is non-blocking with per-connection state. Do not "simplify" it
into a blocking read: one client that sends a partial header would then stall
every other client, and there is a test for that.

## Layers — do not short-circuit these

```
src/cli      argument parsing + renderers (no SQL, no git, no formatting logic
             beyond presentation)
src/core     service.c (all command behaviour), scan.c, buffers, errors, paths,
             sha256, proc
src/db       schema, migrations, typed operations; sqlite3 types never leave here
src/git      read-only git adapter + parsers
src/output   streaming JSON writer
```

A renderer never queries anything. The service layer never formats output. Adding
a command means adding a service function plus a method on both renderers.

## Memory ownership

- `atlas_buf` owns its allocation: `ATLAS_BUF_INIT` → `atlas_buf_free`. Never
  transfer ownership by assignment; use `atlas_buf_detach`.
- Opaque handles (`atlas_ctx`, `atlas_db`, `atlas_git`, `atlas_json`) are created
  by `_open`/`_new` and destroyed by the matching `_close`/`_free`. A partially
  built handle is destroyed by its own destructor, so error paths call `_close` on
  it rather than unwinding by hand.
- **Row callbacks receive borrowed pointers** valid only for the call — they point
  into a live SQLite statement. Copy anything you need to outlive the callback.
- `atlas_json_finish` frees the writer; on a failure path call `atlas_json_free`
  instead. Exactly one of the two runs.
- Structs with owned members have `_init`/`_free` pairs (`atlas_repo_info`,
  `atlas_doctor_report`, `atlas_status_report`).
- Every fallible function returns `atlas_status` and takes an `atlas_err *`.
  Cleanup is one exit path per function; no early `return` that skips a release.

## Untrusted text: repository content is data, never terminal commands

Filenames, commit subjects and bodies, author identities, branch names and git
error text are untrusted. Before any of it reaches a terminal or a JSON document,
encode it with `atlas_safe()` / `atlas_text_encode_safe()` (see
`include/atlas/safetext.h`). That escapes C0 and C1 controls, DEL, line and
paragraph separators, bidi overrides, invalid UTF-8 and `%`, reversibly.

**Safe text is terminal-safe, JSON-structure-safe and reversible. It is not
model-safe.** A commit message reading "ignore all previous instructions" is
entirely printable and passes through unchanged. Printable repository prose stays
semantically untrusted. Do not build AI integration in A1, and when A2 does,
raw repository prose must not be injected as trusted instructions — see
`docs/ai-trust-boundary.md`.

**Do not double-encode.** Values already stored encoded (`path_text`,
`root_path_text`, `old_path_text`, and diff entry paths) are printed as-is. Values
read raw from git or the database (subjects, authors, branches, `git_common_dir`,
`git_dir`, error text) must be encoded at the point of output. Both renderers
document which is which at the top of the file.

When adding a renderer field, decide which category it is in and say so.

## Paths are bytes, not text

Repository paths may contain spaces, tabs, newlines and invalid UTF-8. Store and
look up by `path_raw` (BLOB, exact bytes). `path_text` is a lossless `%XX`
encoding for display, search and JSON, and is accepted as input. Never split a
path on whitespace, and never assume it is UTF-8. Parse git `-z` output only.

## Git safety when touching src/git

**Every git invocation is built in `src/git/git_harden.c`. Do not create a git
process anywhere else.** `src/git/git.c` has exactly one function that forks.

The argv allowlist is the weakest of four layers and is not what stops code
execution. Git can be *configured* to run helpers during a read: `core.fsmonitor`
runs on `git status` AND `git ls-files`, which every scan performs. Only
`-c core.fsmonitor=false` blocks it, not `--no-optional-locks`. The layers are:

1. A constructed child environment, never inherited. Nothing outside the fixed list
   in `ATLAS_GIT_ENV` reaches git; `atlas_git_env_is_sanitized()` asserts it on
   every invocation. Never forward `HOME`, `GIT_DIR`, `GIT_CONFIG_*`,
   `GIT_EXTERNAL_DIFF`, `GIT_ASKPASS`, `GIT_TRACE*` or anything else on the
   forbidden list. Never mutate Atlas' own environment.
2. The `-c` prefix disabling fsmonitor, hooks, external diff, pager, askpass,
   signature verification, auto gc and transports.
3. Per-command flags AFTER the subcommand (they are subcommand options, not global):
   `--no-ext-diff --no-textconv --ignore-submodules=all` for diff/log,
   `--ignore-submodules=all` for status. Use `atlas_git_cmd_flags()`.
4. The read-only subcommand allowlist, checked before the fork.

Resolve the executable with `atlas_git_executable()`, which searches PATH once per
process. Address repositories with `git -C <canonical-root>`; never `chdir`. Never
traverse a symlink inside a repository: use `atlas_path_open_nofollow`, and hash a
tracked symlink's **link text**.

Partial (promisor) repositories are refused at `atlas_git_open` because git 2.39
cannot be told to refuse a lazy fetch. Do not add a bypass.

**Detection is exact and fail-closed.** A0 read the first 64 KiB of `.git/config`
and looked for substrings; that missed a marker beyond 64 KiB, one straddling the
boundary, `config.worktree`, and included config files, and it over-refused a
repository that merely mentioned the word. It now asks git through three
allowlisted `git config --includes --get-regexp` queries plus a bounded pack
scan, and **any** ambiguity — a timeout, truncated output, or an undocumented
exit code — refuses. Do not replace this with a file read.

`config` is on the subcommand allowlist but is **not covered by it**: `git config
a.b c` writes. Every `config` invocation is matched against a positive allowlist
of complete argument vectors in `src/git/git.c`. Adding a query means adding a
vector there deliberately.

Adding a git call site means: pick the right `atlas_git_cmd_kind`, and add an
adversarial case to `tests/test_git_hardening.c` if it opens a new vector.

Full detail: `docs/git-safety.md`.

## Repository and worktree identity

A `repositories` row identifies one **worktree**: unique `root_path`, shared
`git_common_dir`, own `git_dir`, plus `is_linked_worktree`. Worktrees of one
repository share the common dir and differ in git dir; everything else
(`scanned_head`, branch, dirty state, files, commits, evidence) is per row. `scan`
verifies both root and git dir against the registration and refuses with exit 7 if
either moved. Details in `docs/data-model.md`.

## Exit codes (stable contract)

`0` ok · `1` internal · `2` usage · `3` config · `4` repository · `5` database ·
`6` git · `7` integrity/safety

Unchanged in A1, and the IPC error document uses the same numbering in its
`status` field so a caller has one vocabulary rather than two.

`atlas daemon ping` exits `3` when the daemon is not answering, after printing a
complete document. That is the one place a non-zero exit accompanies valid
output; `cli_state.rendered` suppresses the error document so `--json` never puts
two documents on stdout.

## Where things are documented

`README.md` (usage, limitations) · `SECURITY.md` (threat model) ·
`docs/architecture.md` · `docs/data-model.md` · `docs/provenance.md` ·
`docs/git-safety.md` · `docs/daemon-and-ipc.md` ·
`docs/watcher-consistency.md` · `docs/systemd-user-service.md` ·
`docs/ai-trust-boundary.md` · `docs/backlog.md` · `docs/roadmap.md` ·
`third_party/yyjson/PROVENANCE.md`

Keep these current when behaviour changes. If you change the JSON shape, the
schema, or an exit code, that is a contract change — update the docs in the same
change.

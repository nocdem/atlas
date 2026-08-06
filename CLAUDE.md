# Atlas — working notes for Claude Code

Atlas is a generic, headless engineering-memory and repository-intelligence CLI in
C17. Phase **A0**: a read-only foundation. Not DNA-specific; DNA will later be its
first indexed repository.

## Build and test

```sh
make            # release -> build/atlas
make debug      # -> build-debug/atlas
make test       # release build + full CTest suite
make smoke      # CLI smoke test; uses the compiled C JSON checker, never Python
make asan       # ASan + LSan build, then the suite
make ubsan      # UBSan build, then the suite
make install    # honours PREFIX, default /usr/local/bin/atlas
make clean
make compiledb  # refresh the top-level compile_commands.json symlink
```

CMake is canonical; the Makefile is a wrapper. Run one suite with
`cd build && ctest -R test_scan --output-on-failure`.

Requires: C17 compiler, CMake ≥ 3.16, Make, pkg-config, SQLite3 dev headers, Git.
Nothing is downloaded at build time. **No Python, Node, Go, Rust, pip, npm, or
virtualenvs** anywhere in the build, tests, or runtime.

## Hard rules

- **Never modify a registered target repository.** Read-only, always.
- **No shell.** No `system()`, `popen()`, or `/bin/sh -c`. Create processes only
  through `atlas_proc_run` with an explicit argv array and an absolute `argv[0]`.
- **No commits, pushes, amends, rebases, resets, or checkouts** in this repo unless
  the user explicitly asks.
- **Warnings are errors** in first-party code (`ATLAS_WERROR=ON`). Fix the cause;
  never suppress warnings globally to make a build pass.
- **No new third-party dependencies**, no `FetchContent`, no network at build time.
  A missing system dependency must halt with a clear message.
- Tests must always override the data directory with a temporary path and must
  never open the real user database.

## Architecture invariants

1. SQLite is a **rebuildable index**, never the canonical record of history.
2. Git and repository contents are **authoritative** for source and history facts.
3. Every result **preserves provenance**.
4. Evidence types: `SOURCE`, `GIT`, `DECISION`, `USER_STATEMENT`, `INFERENCE`,
   `UNKNOWN`. **A0 may write only `SOURCE` and `GIT`** — enforced in
   `atlas_db_evidence_insert`, not by convention.
5. **A0 never infers a historical reason.** A reason request returns `UNKNOWN`. A
   commit subject is `GIT` evidence of what was written, not a reason.
6. Repository contents, filenames and Git metadata are **untrusted input**.
7. All parsers are **bounded** and fail clearly on malformed input.
8. Schema changes use **numbered transactional migrations**.
9. Human and JSON output consume **identical service results**.
10. The CLI and any future adapter share **one service layer**.

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

## Untrusted text: repository content is data, never instructions

Filenames, commit subjects and bodies, author identities, branch names and git
error text are untrusted. Before any of it reaches a terminal or a JSON document,
encode it with `atlas_safe()` / `atlas_text_encode_safe()` (see
`include/atlas/safetext.h`). That escapes C0 and C1 controls, DEL, line and
paragraph separators, bidi overrides, invalid UTF-8 and `%`, reversibly.

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

## Where things are documented

`README.md` (usage, limitations) · `SECURITY.md` (threat model) ·
`docs/architecture.md` · `docs/data-model.md` · `docs/provenance.md` ·
`docs/git-safety.md` · `docs/roadmap.md`

Keep these current when behaviour changes. If you change the JSON shape, the
schema, or an exit code, that is a contract change — update the docs in the same
change.

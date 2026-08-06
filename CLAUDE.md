# Atlas — working notes for Claude Code

Atlas is a generic, headless engineering-memory and repository-intelligence CLI in
C17. Phase **A2**: automatic AI integration — an MCP server, Claude Code hooks and
a plugin — on top of the A1 daemon and the A0 read-only foundation. Not
DNA-specific; DNA will later be its first indexed repository.

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

sh scripts/perf.sh build      # A1 performance acceptance measurements
sh scripts/perf-a2.sh build   # A2 hook and MCP latency measurements
```

`make doctor` and `make doctor-claude` observe and create nothing: no data
directory, no index, no lock, no runtime directory, no socket, no Claude
configuration. `atlas doctor` opens in `ATLAS_CTX_INSPECT` mode and reports a
missing index as a finding rather than creating one. A diagnostic that
initialises what it is diagnosing can only ever answer "fine", and cannot be run
at all on a machine where Atlas has never been used — which is exactly when
somebody runs it. `tests/test_plugin.c` snapshots a fresh HOME around both
commands and asserts nothing appeared.

CMake is canonical; the Makefile is a wrapper. Run one suite with
`cd build && ctest -R test_scan --output-on-failure`, or run its binary directly
(`./build/tests/test_scan`) — every suite is a standalone executable. Tests are
labelled `unit`, `integration` or `daemon`: `ctest -L unit` is the fast subset,
`ctest -LE daemon` is everything except the slow one. The live-daemon suite is
serialised against itself because parallel daemons compete for the machine's
inotify watch budget.

`make test-debug` runs the suite from `build-debug`; `make doctor` runs the built
binary's self-check; `make distclean` also drops the `compile_commands.json`
symlink.

The data directory resolves as `--data-dir`, then `ATLAS_DATA_DIR`, then
`XDG_DATA_HOME/atlas`, then `$HOME/.local/share/atlas`. The socket lives under
`XDG_RUNTIME_DIR/atlas/`. A Unix socket address is a fixed 108-byte field and
Atlas refuses a path that would not fit rather than truncating it, so a fixture
or script must put its runtime directory somewhere short — see `scripts/perf.sh`.

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

## A2 layers — additions

```
src/ai       ai.c (the provider-neutral session service, runs on the writer
             thread), context.c (the automatic context envelope)
src/db       db_ai.c (typed operations over the migration-4 tables)
src/ipc      server_ai.c (the A2 method group), reply.c (typed request building
             and response reading), json_read.c (the one yyjson facade)
src/mcp      mcp.c (stdio transport, lifecycle, dispatch), mcp_tools.c (the tool
             surface)
src/hook     hook.c (one process per Claude Code lifecycle event)
src/core     integrate.c (`atlas integrate claude`)
integrations/claude/atlas   the Claude Code plugin: manifest, hooks.json,
             .mcp.json, skill, POSIX-sh launchers
```

**yyjson is called from `src/ipc` and nowhere else.** `json_read.c` is the facade;
`hook.c`, `mcp*.c` and `integrate.c` use it. A new file that parses untrusted JSON
goes through that facade rather than including the vendored header.

## A2 rules — these are not negotiable

- **No repository-controlled or model-provided free-form text in automatic
  context; only fixed Atlas-owned control text and typed values — and that
  excludes the repository's own name and root.** The envelope carries five kinds
  of thing and nothing else: an integer Atlas assigned or counted, a boolean, a
  string from a fixed vocabulary checked against that vocabulary, a fixed-length
  lowercase hex hash checked to be hex, and the fixed `note=` control line that
  is a string literal in `src/ai/context.c`. That line stays: it is what tells
  the reader how to treat the typed values. A repository is identified by
  `repo_id` and `root_hash`.

  The name and the root were in the first implementation and were wrong: a name
  is derived from a directory basename and a root is a filesystem path, so both
  are chosen by whoever created the directory. `ignore previous instructions` is
  a legal directory name, it is entirely printable, and it survives every
  encoding Atlas has. Encoding is not the defence — the defence is that no field
  can hold such a value.

  So the renderer **validates rather than escapes**: a value that is not the
  shape it claims to be is replaced by a marker, never reproduced. The allowlist
  in `atlas_ai_context_is_bounded` was tightened accordingly (`%`, `(`, `)` and
  `+` are gone, because nothing is escaped and no path is emitted), and
  `atlas_ai_context_render` checks its own output against it and discards a
  document that fails. Adding a field to the envelope means arguing that it
  cannot carry a byte somebody else chose.
- **An A2 adapter may write only MODEL_PROPOSAL, MODEL_INFERENCE and UNKNOWN.**
  Enforced in three places on purpose: `atlas_provenance_writable_in_a2`, the IPC
  validation before anything is queued, and `CHECK(approved = 0)` in the schema.
  Neither insert statement binds the column. Do not add a fourth path.
- **UNKNOWN is a write, not a silence.** A changed path nobody explained gets an
  explicit row at the turn close. Do not "optimise" that away.
- **Hooks fail open and store metadata only.** Every hook returns valid JSON and
  exits 0, whatever happened. No hook emits `decision`, `continue` or a permission
  verdict — which is what makes a Stop loop structurally impossible rather than
  guarded against. `tool_input` is read for exactly one member, a file path, and
  only in `edit_path_of`. If you find yourself reading a second member, stop.
- **The MCP adapter opens no database handle**, not even read-only. Everything it
  answers came over the socket. That is what makes its capability list short
  enough for a reviewer to check.
- **Attribution never improves.** A changed path already marked `ambiguous` stays
  ambiguous. The `ON CONFLICT` clause in `db_ai.c` enforces it; do not move that
  decision into a caller.
- **A session is found by its key and by nothing else.** The lookup is exact
  `(provider, client, session_key)`, where `session_key` is the client's own
  external id — for Claude Code, `CLAUDE_CODE_SESSION_ID`, which is the same
  string the hook payload carries as `session_id`. **A repository never
  identifies a session.** There is no query that selects one by recency; the one
  that did (`atlas_db_ai_session_newest_for_repo`) is deleted, and adding
  anything like it back would silently record one Claude session's reason against
  another whenever two are open on one worktree.

  When the session cannot be resolved exactly, the record is stored **sessionless**
  with `session_unbound` and a typed `unbound_reason`, never attached to a
  neighbour. **Prefer missing or ambiguous over wrong** — a gap is repairable and
  a wrong row is not, because nothing about it says it is wrong. Reason and
  decision records additionally require the session to be *open*, which is what
  turns a post-`/clear` write from a false attribution into an honest gap.

  The MCP and hook adapters must keep sending the same `provider`/`client` pair:
  if the two constants drift apart the lookup misses silently and every MCP write
  becomes unattributed. `tests/test_ai_attribution.c` is what catches it.
- **MCP is not a filesystem reader.** No tool accepts an absolute path, and a
  `repo` argument must name a repository one of the client's granted roots
  resolved to — a whitelist, not a path comparison.
- **Requests are built with the typed writer.** `atlas_ipc_params_begin`/`_finish`,
  never `atlas_buf_appendf`. There is still no "write these bytes as JSON"
  primitive anywhere in Atlas, and `atlas_ipc_result_write` /
  `atlas_jsonv_write` re-emit through the writer rather than copying bytes.
- **Never install, enable or start anything real.** `atlas integrate claude
  install` writes one file in the user's config directory and prints the rest. It
  does not edit `~/.claude`, does not touch systemd, and does not run `claude`.
  `uninstall` never touches the index.

## Adding an MCP tool or a hook event

- **A tool** is one entry in `TOOLS[]` in `src/mcp/mcp_tools.c`: a schema function
  and a run function. The schema must set `additionalProperties: false` and
  declare every argument. Add the name to the expectation in `tests/test_mcp.c`,
  which compares `atlas_mcp_tool_names()` against what the process reports.
- **A hook event** goes in `HOOK_EVENTS[]` in `src/hook/hook.c`, in `handle()`,
  and in `integrations/claude/atlas/hooks/hooks.json`. `tests/test_plugin.c`
  asserts the two lists match exactly — a plugin configuring an event the binary
  ignores looks installed and does nothing, and a binary handling an event the
  plugin never sends is dead code.
- **Check the event's real output contract before returning anything but `{}`.**
  `PostCompact` has no `additionalContext`; returning one is silently ignored,
  which reads correctly and does nothing.

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

## Wiring new code in — nothing is globbed

- **A new `.c` file** is added to the explicit `atlas_core` source list in
  `CMakeLists.txt`. There is no `file(GLOB)`; a file not listed is not compiled,
  and the failure surfaces as a link error, not a build error.
- **A new test** is added to `ATLAS_TESTS` in `tests/CMakeLists.txt` **and** to
  one of the `set_tests_properties(... LABELS ...)` lines. An unlabelled test
  still runs under a bare `ctest` but is invisible to `ctest -L unit` and to
  `ctest -LE daemon`, so it silently stops being part of the subsets people
  actually run.
- **A new command** touches four places: a service function in `src/core/service.c`
  (or `service_daemon.c`), a method on `atlas_renderer_vtbl` in `src/cli/render.h`,
  an implementation in **both** `render_human.c` and `render_json.c`, and dispatch
  plus help text in `src/cli/cli.c`. The vtbl is not optional-per-renderer: a
  missing implementation is how human and JSON output drift apart.

## Test conventions

The harness is first-party and dependency-free (`tests/atlas_test.h`):
`T_CHECK`/`T_CHECK_MSG` record a failure and continue, `T_REQUIRE` abandons the
test, `T_OK(expr, &err)` and `T_FAILS_WITH(expr, status, &err)` assert on an
`atlas_status` and print the error message, and `ATLAS_TEST_MAIN` is the entry
point. Prefer `T_OK`/`T_FAILS_WITH` over comparing statuses by hand — they report
what actually went wrong.

Integration tests use `tests/support/fixture.h`. `fx_open` creates a private
temporary tree with its own `repo/` and `data/`; `fx_close` removes it. Fixtures
build real git repositories, driving git through `atlas_proc` with explicit argv —
there is no shell in the tests either. Notes that matter:

- `fx_atlas` runs the built binary but does **not** add `--data-dir`. Pass it, or
  the test opens the developer's real database.
- Daemon tests fork the binary via `fx_daemon_start`, which supplies both the
  fixture data directory and a private `XDG_RUNTIME_DIR`. Never install, enable or
  start a systemd unit from a test.
- Wait for an observable outcome with `fx_wait_for_substring`, never a guessed
  `sleep` — watcher timing is machine-dependent.
- `fx_tree_digest` is how a test proves a read command did not modify a
  repository. Use it when adding any command that touches a target repo.
- Path helpers take raw bytes (`fx_write_bytes`, `fx_can_create_name`) so a test
  can use names that are not UTF-8. `fx_can_create_name` lets a test skip rather
  than fail on a filesystem that rejects them.
- `fx_install_marker` / `fx_marker_fired` are the adversarial pair: they place a
  helper a hostile repository config could point at, and assert it never ran.

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
semantically untrusted.

A2 implements the separate boundary that follows from that: automatic model
context contains no repository prose at all, and repository prose reaches a model
only through an explicit MCP result that states its provenance. See
`docs/ai-trust-boundary.md`, and the A2 rules below.

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
`docs/ai-trust-boundary.md` · `docs/claude-integration.md` ·
`docs/backlog.md` · `docs/roadmap.md` ·
`third_party/yyjson/PROVENANCE.md` ·
`integrations/claude/atlas/README.md`

Keep these current when behaviour changes. If you change the JSON shape, the
schema, or an exit code, that is a contract change — update the docs in the same
change.

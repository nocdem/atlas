# Atlas — working notes for Claude Code

Atlas is a generic, headless engineering-memory and repository-intelligence CLI in
C17. Phase **A8**: the durable orchestration control plane — a job queue, an
explicit state machine, expiring leases, crash recovery, an unprivileged
dispatcher running as `atlas-worker`, isolated per-attempt workspaces, bounded
command execution and a versioned driver interface. See `docs/orchestration.md`.
On top of A7.1: OS authority separation — the daemon runs as `atlasd` and
solely owns the index and backups, `atlas-worker` is the untrusted account every
persistent model process runs as, and a root-owned policy decides who may open
the shared socket. On top of the A7 dedicated security review and trust-boundary
hardening — the model-facing surface reduced to reads and proposals, automatic
repository registration removed, and operator authority made a configured OS
fact rather than an inference from a terminal — the A6
deterministic impact gates and stale-decision detection,
the A5 verified online backups, atomic restore and written retention
classification, the A4 decision documents,
the A3 structural code intelligence, the A2 AI integration, the A1 daemon and the
A0 read-only foundation. Not DNA-specific; DNA is its first real indexed
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

sh scripts/perf.sh build      # A1 performance acceptance measurements
sh scripts/perf-a2.sh build   # A2 hook and MCP latency measurements
sh scripts/perf-a3.sh build   # A3 structural-indexing acceptance (~7 minutes)
sh scripts/perf-a4.sh build   # A4 decision-lifecycle acceptance (~2 minutes)
sh scripts/perf-a5.sh build   # A5 backup/verify/restore acceptance (~3 minutes)
sh scripts/perf-a6.sh build   # A6 gate-latency acceptance (~4 minutes)
```

`perf-a3.sh` measures peak RSS from `/proc/<pid>/VmHWM` rather than from
`time(1)`, because `busybox time -v` reports `ru_maxrss` multiplied by the page
size — every RSS it prints on this machine is four times the truth. A
measurement wrong by a constant factor is worse than none, because it still
looks like a measurement.

It also **asserts its own scale floors and its own limit**, and exits non-zero
rather than printing a number nobody checks: at least 5 000 files, 500 000
lines, 50 000 symbols and 200 000 relations, and every one of three independent
initial passes — each against a data directory that never existed before — under
60 s. Lines are newlines in every `.c` and `.h` in the tree, counted before
anything is timed. Shrinking the fixture or moving the limit to make a run pass
is the one failure mode a performance gate cannot detect about itself, so the
script does not leave either to a reader.

`perf-a4.sh` follows the same discipline and for the same reason: it builds a
deterministic corpus of **10 000 documents, 25 000 revisions and 100 000 links
with all four lifecycle states present**, asserts those floors, and asserts its
own limits — every bounded read p95 required under 100 ms, the passive hook
under 20 ms — exiting non-zero rather than printing a number nobody checks. It
earned that on its first run by catching a 1 474 ms search.

Those are the **required limits**, not the observations. Observed p95 on this
machine is 4–15 ms for the bounded reads and 2 ms for the hook, and it moves by
several milliseconds between runs with nothing but machine load. Report an
observation as an observation: "7 ms observed", never "under 7 ms", which reads
as a bound Atlas does not hold. `docs/decision-lifecycle.md` carries the table,
and every figure in it comes from one run of the script.

`perf-a5.sh` follows the same discipline again: it builds the A4 decision corpus
*plus* a structural graph and A2 session rows, asserts its own floors — at least
10 000 documents, 25 000 revisions, 100 000 links, all four lifecycle states, and
a database of at least 100 MiB — and asserts its own limits: backup create,
backup verify and an isolated restore each under 10 s, each under 256 MiB peak
RSS read from `/proc/<pid>/VmHWM`. It exits non-zero rather than printing a
number nobody checks, and it verifies the restored copy table by table rather
than trusting that it finished.

The limits are the required bounds, not the observations. Report an observation
as an observation: "3 s observed", never "under 3 s", which reads as a bound
Atlas does not hold. `docs/operations.md` carries the operational contract and
every measured figure in it comes from one run of the script.

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
`XDG_RUNTIME_DIR/atlas/`, or under `/run/user/<uid>/atlas/` when that variable is
unset and that directory proves to be a private one this user owns — checked with
`lstat`, never followed, and never `/tmp`. A Unix socket address is a fixed 108-byte field and
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

## A3 layers — additions

```
src/code     extract.c (the bounded lexical C indexer), compdb.c (the compile
             database, read as data), resolve.c (deterministic resolution),
             index.c (the pass), query.c (bounded traversal), code.c (the
             vocabularies)
src/db       db_code.c (typed operations over the migration-5 tables)
src/core     service_code.c (the `code` command behaviour)
src/ipc      server_code.c (the seven-method A3 group)
```

## A3 rules — these are not negotiable

- **Atlas is not a compiler and does not pretend to be one.** Every structural
  fact carries a `resolution` from a closed vocabulary, and the distinctions the
  vocabulary exists to keep are the ones it would be easiest to lose: a
  `#include` directive is a source fact and *resolving* it is a separate one;
  `identifier(` is a call **candidate**, not a proven call; `UNIQUE_LEXICAL`
  means one lexical match, not one truth; a function pointer, a macro-produced
  call and a definition under an unevaluated `#if` never become exact edges;
  several definitions of a name stay `AMBIGUOUS` **with the candidate set
  recorded**, because choosing would be inventing. A missing compile database is
  unknown, not false. Impact results are candidates to review.
- **The `evidence` table is untouched.** A3 writes no evidence at all;
  `tests/test_code_trust.c` asserts the table gained nothing but `SOURCE` and
  `GIT` after a structural pass. Structural facts carry their own `resolution`
  and `provenance` columns, which is the same separation A2 made and for the
  same reason.
- **`atlas_code_resolution_writable_in_a3` refuses `MODEL_PROPOSAL`**, mirroring
  `atlas_provenance_writable_in_a2`. `settle()` in `resolve.c` is the single
  write point and checks it. Do not add a second.
- **`compile_commands.json` is data, never a command.** The `command` string is
  SHA-256'd and discarded — never executed, never passed to a shell, never
  stored. Arguments are read through a positive allowlist (include dirs, system
  include dirs, defines and undefines, the standard, the source, the output) and
  nothing else; `@response-files` and `-fplugin=` are recognised only well enough
  to be ignored. An include directory outside the repository is recorded with
  `external = 1` and **never opened**. `tests/test_code_compdb.c` plants an
  executable marker in four places and asserts it never ran.
- **No new dependency, ever, for this.** No Clang, no libclang, no tree-sitter,
  no ctags, no Python, no Node. The lexer is first-party and bounded, and every
  ceiling it reaches is reported rather than silently applied.
- **The structural stage inherits A1's rules unchanged.** Workers touch no
  database handle and create no process; nothing forks off the writer thread; no
  transaction is held across unbounded work; the select/parse/apply loop is
  chunked by `ATLAS_CODE_PARSE_CHUNK` so the stage's memory is a property of the
  constant rather than of the repository.
- **Selection compares content hashes, not pass activity.** The candidate set is
  "files whose `content_hash` differs from the hash the stored graph facts were
  extracted from". Never "was this file hashed by this pass" — a full
  content-verifying pass rehashes every byte and finds the same hash, and keying
  off activity would make the periodic full pass reparse the world every five
  minutes.
- **Resolution runs over a described scope, and each field of
  `atlas_code_resolve_scope` is a correctness argument.** `files` because a
  reparsed file's edges were rewritten unresolved; `names` because a call
  resolves by name and by nothing else; `file_set_changed` because include
  resolution reads the *set of paths* and an edit to an existing file changes
  none of its inputs; `full` because an overflowed scope is an unknown one, not
  a smaller one. **Internal linkage is excluded from `names`** — a `static`
  definition cannot change how anything outside its own file resolves, and that
  file is swept by id. Widening the scope is always safe and always expensive;
  narrowing it needs an argument of this shape.
- **`code_index_state.resolve_settled` is cleared before the work and set after
  it.** That ordering is the whole reason it is durable rather than inferred: a
  pass that died during resolution leaves it false and the next pass sweeps the
  repository. Do not "simplify" it into a check of whether the last pass
  completed.
- **Invalidation is targeted, not scanned.** Before a file's rows are replaced,
  `atlas_db_code_relations_unsettle_for_file` unsettles the edges that resolved
  into it, seeking from the ids about to disappear. Afterwards only a left join
  over every relation could find the damage, and that scan costs the same
  whether it finds one row or none — it is kept for the rebuild path only.
- **`ATLAS_CODE_ANALYZER_ID` and `ATLAS_CODE_ANALYZER_VERSION` are the graph's
  producer, and the version is an epoch you must bump.** Bump it whenever a pass
  would produce different facts from identical bytes — a lexer fix, a resolution
  rule change, a different set of materialised edges — and not for a refactor
  that cannot change an output. A mismatch makes the graph stale and the next
  pass rebuilds it. Stored normalized: `code_analyzers` interns the pair and
  `code_index_state.analyzer_id` references one row, never a string per
  relation. Both values are compiled-in constants; nothing repository-controlled
  or model-controlled may reach that column, which is why they may be reported.
- **A structural rebuild deletes derived rows and nothing else.**
  `atlas_db_code_clear_repo` names `code_files` and `code_units`; sessions,
  reasons, decisions, evidence, commits and the file index are untouched, and
  `tests/test_code_analyzer.c` asserts it row by row. Do not widen it.
- **`symbol_contains_occurrence` is recognised and never written.** The fact is
  `code_occurrences.enclosing_id`. Materialising it as an edge stored the same
  thing twice — 38 % of the relation table on the acceptance fixture, read by
  nothing. Do not reinstate it; a producer without an occurrence table may write
  the kind, which is why it stays in the vocabulary.
- **The include suffix lookup says `INDEXED BY idx_code_files_basename`**, and
  it is a hard constraint rather than a hint: `code_files` has two indexes
  starting with `repo_id` and SQLite picks the wrong one, turning the lookup
  into a scan of the repository. Removing the clause is a thirty-seven-million
  row regression; `tests/test_code_graph.c` asserts the plan.
- **Nothing is silently truncated**, including an ambiguity. `candidate_count`
  reports the true number even when more candidates existed than the ceiling
  keeps, because a bound that makes an ambiguity look smaller than it is is a
  bound that lies.

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

## A4 layers — additions

```
src/decision decision.c (the vocabularies, the canonical content hash,
             validation), lifecycle.c (the state machine and the operator
             channel — the only write point)
src/db       db_decision.c (typed operations over the migration-6 tables)
src/core     service_decision.c (the `decision` command behaviour and the
             interactive confirm flow), terminal.c (the operator-only channel)
src/ipc      server_decision.c (the ten-method A4 group)
```

## A4 rules — these are not negotiable

- **State the approval contract precisely, and never more than it.** The whole
  of what Atlas may claim is:

  > Atlas exposes no approval, rejection or supersession capability through MCP,
  > hooks or any AI-facing method. Conversation text and model-generated RPC
  > arguments cannot change a lifecycle state. The local operator channel
  > requires an interactive terminal and a deliberate confirmation. A same-UID
  > process that can drive a pseudo-terminal — **including an AI agent with
  > shell access** — may imitate that channel. `LOCAL_OPERATOR_CONFIRMED`
  > identifies the channel, not a person: it is not cryptographic identity, does
  > not establish that a person was present, is not a signature, and provides no
  > non-repudiation.

  Anything stronger is false. The forbidden phrasings are enumerated in one
  place — `FORBIDDEN[]` in `tests/test_decision_mcp.c` — and that test scans the
  documentation, the headers, the skill and the source and fails on any of them.
  The list is not repeated here on purpose: a second copy would drift, and this
  file is one of the files the scan covers.

  That tripwire exists because the overclaim was in the shipped text of this
  very phase. Also avoid "approved by the user" and "signed off" in prose about
  a decision; say "approved in Atlas". A2's `USER_APPROVED_DECISION` stays in
  the vocabulary and stays **unwritten**, because it names a person.
- **Approval changes a status, never the nature of the bytes.** Approved
  decision prose is accepted project policy *and* still `UNTRUSTED_DATA`. It is
  encoded wherever it reaches a terminal or a JSON document, it is labelled
  wherever it reaches a model, and it never enters automatic context at any
  status. Conflating the two would turn the approval prompt into a
  prompt-injection channel.
- **`atlas_decision_apply_in_tx` is the only function that writes a lifecycle
  transition.** The actor restriction, the transition table, the challenge
  consumption, the atomic approve-and-supersede, the cycle check and the cache
  update all live behind it, and every one would be bypassable if a second path
  reached the tables. This is the same rule `settle()` and
  `atlas_db_evidence_insert` follow.

  It has **exactly two callers**: `atlas_decision_apply`, the public entry
  point, which adds only `BEGIN`, `COMMIT` and rollback; and
  `op_decision_locked` in `src/ai/ai.c`, the A2 bridge, which already owns a
  transaction because its A2 row and its A4 document must commit together. A
  nested transaction there would not work — `atlas_db_begin` counts depth, its
  rollback does not, and a failed transition would silently discard the caller's
  work. Adding a third caller means arguing that it genuinely owns a wider unit
  of work; adding a second *implementation* is what the rule forbids.
- **`atlas_decision_actor_writable_by_adapter` refuses
  `LOCAL_OPERATOR_CONFIRMED` and `ATLAS_AUTOMATIC`**, mirroring
  `atlas_provenance_writable_in_a2` and
  `atlas_code_resolution_writable_in_a3`. Checked at the IPC edge *and* at the
  write point: the edge produces the better message, the write point is the
  guarantee.
- **A revision is immutable.** No `UPDATE` in `db_decision.c` names a content
  column; the one statement that touches `decision_revisions` sets `state` and
  nothing else. A change is a new revision. Adding an in-place edit path would
  make every prior approval's content hash a claim about bytes that are gone.
- **The ledger is canonical; the status columns are a cache.** They are written
  in the same transaction as the event that justifies them, and
  `atlas_db_decision_verify` replays the ledger to check them. It **reports,
  never repairs** — `atlas doctor` calls it, and a diagnostic that fixes what it
  finds cannot tell you whether the fault recurs. A transition that reasons
  about the document's status independently will disagree with the replay; use
  `recompute_status()`.
- **Every transition's `UPDATE` names the state it observed.** `... WHERE id = ?
  AND state = ?`, and the caller requires that exactly one row changed. That is
  what makes a concurrent transition lose deterministically instead of
  last-write-wins. Never replace it with a read followed by an unconditional
  write.
- **At most one approved revision per document is a schema constraint**, not
  care: `CREATE UNIQUE INDEX ... ON decision_revisions(document_id) WHERE state
  = 'APPROVED'`. It makes a wrong approve/supersede ordering a hard failure
  instead of two effective revisions that every later read quietly picks
  between. Do not remove it to "simplify" the ordering.
- **Nothing deletes a decision record.** The only `DELETE` in `db_decision.c`
  removes *expired, unconsumed* challenges; a consumed one is part of an
  approval record and the event points at it. There is no `_clear` for these
  tables and there must not be one.
- **Decision tables do not cascade from `repositories`**, because an FK would
  make `repo remove --yes` destroy approval history. `repo_id` is a soft
  reference and `repo_identity_hash` is the durable identity.
- **A path hash is not a repository identity.** `repo_identity_hash` is a
  **path-qualified lineage fingerprint**: the canonical root path, the object
  format **and the sorted set of ingested root commits**. Without the lineage,
  `git init` of an unrelated project at the same path inherits the previous
  one's approved decisions. Because the path is hashed too, the converse also
  holds and is deliberate — the same lineage at another path does not reattach
  automatically. Automatic reattachment requires the exact fingerprint; manual
  relinking is deferred. Always describe it as a path-qualified lineage
  fingerprint and name both halves: a description that credits only the lineage
  is wrong in the second direction, and one that credits only the path is wrong
  in the first. `tests/test_decision_mcp.c` scans for the shorter phrasings.
- **Detach at registration, attach after ingestion, and never guess.**
  `atlas_db_decision_detach_repo` runs unconditionally inside
  `atlas_db_repo_add`, needs no git, and cannot be forgotten — `repositories.id`
  is a reused rowid. `atlas_db_decision_relink_after_ingest` runs from
  `atlas_db_scan_finish` on a successful pass and attaches only on an exact,
  non-empty identity match. Splitting them makes the failure mode fail-closed by
  construction: a forgotten attach orphans, and orphaning is visible and
  recoverable. Never relink on a name, a remote, a branch or a judgement, and
  never overwrite an existing identity.
- **An orphan must stay visible.** `atlas decision orphaned` exists because a
  canonical record that has become invisible looks exactly like one that was
  deleted.
- **No A4 column may hold a rowid that outlives the row.** A4 records do not
  cascade and `ai_decisions` does, so a promoted revision's
  `imported_from_ai_decision_id` survived its target — and SQLite reuses rowids,
  so the next A2 record took an id an orphan still pointed at. That failed the
  unique index and made `atlas_record_decision` impossible after any
  `repo remove`; without the index it would instead have resolved silently to
  another repository's proposal. `atlas_db_repo_remove` clears the pointers in
  the same transaction as the delete, via
  `atlas_db_decision_forget_legacy_origins`. Adding a cross-model reference
  means asking what happens when the far side is deleted **and its id is handed
  to somebody else**; "there is a foreign key" is not an answer when only one
  side cascades.
- **No decision link is a foreign key into a migration-5 table.** A symbol link
  is a durable selector snapshot (name, kind, file, line, basis commit, file
  content hash, analyzer name and version). Currency is computed on read and
  never stored, and Atlas **never re-points a link**: a rename is `MISSING`,
  several matches are `AMBIGUOUS` with the count, and an index that has not run
  is `UNKNOWN` rather than `MISSING`.
- **The canonical content hash covers everything immutable that changes what was
  approved, and nothing database-local or recomputed.** That includes each
  link's whole snapshot — basis commit, captured file content hash, analyzer
  name and version — plus the revision's `basis_head`, the durable repository
  identity and `proposed_by`. It excludes row ids, `revision_no`, `created_at`,
  the session binding, `state`, the dedup key, the import pointer, the derived
  `%XX` display encodings, and every live currency result. The field-by-field
  table is in `docs/decision-lifecycle.md` and adding a field means adding a row
  to it with a reason.

  Domain-separated and **length-prefixed**, never delimited: with any
  single-byte delimiter a title of `a|b` with a decision of `c` encodes
  identically to a title of `a` with a decision of `b|c`. Links hash in a
  canonical order (a set); alternatives keep theirs (a list). Changing the
  encoding means bumping `ATLAS_DECISION_HASH_DOMAIN`.
- **`atlas doctor` rehashes every revision.** Atlas never updates a content
  column, so a mismatch means something outside Atlas did — and any approval
  bound to that digest now covers bytes that are not there. Reported, never
  repaired.
- **A4 writes no evidence, and `INFERENCE` stays unused.** The reserved kind is
  not used merely because it exists: A4 defines no deterministic inference with
  its own provenance. `DECISION` and `USER_STATEMENT` stay unused too.
- **No MCP tool may approve, reject or supersede, and no tool schema may declare
  a `token` or a `confirmation`.** The absence is structural — every schema sets
  `additionalProperties: false` — rather than guarded.
  `tests/test_decision_mcp.c` asserts the whole inventory and rejects any tool
  name containing an approval verb.
- **Approvals are sessionless.** `atlas_decision_apply` clears the session
  binding unconditionally for every operation that consumes a capability, even
  when the request carried a valid open session key. Attaching one would record
  that a conversation approved something.

## A5 layers — additions

```
src/db       db_backup.c (the SQLite online copy and every record check),
             db_maintenance.c (counting and the one bounded delete)
src/core     service_backup.c (path safety, atomic publication, restore),
             service_maintenance.c (RETENTION[]: the whole retention policy)
```

There is deliberately **no `src/ipc` file here**, and no entry in any method
table. That absence is the A5 guarantee, not an omission.

## A5 rules — these are not negotiable

- **Backup, restore and maintenance are local CLI operations with no RPC
  method.** Nothing reachable over the socket — and so nothing reachable from
  MCP or a hook — can create, read or restore a backup, or plan or apply a
  prune. A model that can call every method Atlas exposes still cannot replace
  or prune the index. Adding an RPC method for any of them would delete the
  guarantee; `tests/test_backup_live.c` asks a live daemon for each name such a
  method would plausibly have and requires every one to fail.

  They are also dispatched in `cli.c` **before any `atlas_ctx` is opened**, for a
  second reason: a context in AUTO mode takes the writer lock when it is free,
  and a backup must never take it. Restore and prune take it themselves,
  exclusively, which is what makes "the daemon must be stopped" a fact the
  kernel enforces rather than an instruction in a manual.
- **A backup is one self-contained file, never a copy of the three.**
  `atlas.db`, `atlas.db-wal` and `atlas.db-shm` are meaningful only together and
  only at an instant no external reader can name. The copy goes through
  `sqlite3_backup_step(-1)` — one step, not a loop: stepping incrementally lets
  a writer commit between steps and SQLite restarts the copy from the beginning,
  which against a busy daemon is unbounded. The finished copy is switched to
  rollback journalling before publication, which is what lets `backup verify`
  open it read-only and create nothing.
- **Nothing partial is ever published.** Every write goes to a mode-0600
  `O_EXCL` temporary file in the destination directory, is verified *in full* by
  the same code `backup verify` runs, is `fsync`ed, and only then renamed. The
  mode is set explicitly because SQLite would otherwise create the file 0644 or
  worse under a permissive umask. A backup that would not restore is never
  written.
- **No path is ever resolved through a symlink.** Every component is opened from
  `/` with `O_NOFOLLOW`; a symlinked component refuses the operation rather than
  being followed. `realpath(3)` is the wrong tool here and must not appear: it
  resolves links, which names a directory the operator did not write down. The
  lexical `..` collapse in `normalise_abs` is sound *only* because of that walk.
- **A failed restore leaves the original database byte-identical.** Everything
  before the commit fails safely, and the commit itself is reversible: the
  previous write-ahead log is **renamed aside, not deleted**, so a failed rename
  puts it back. It must not survive the rename — SQLite would apply it to the
  restored file — and the consistent snapshot taken beforehand is what covers
  the two-rename window. Do not "simplify" that into an `unlink`.
- **`atlas_db_backup_inspect` never opens the file as an `atlas_db` for the
  structural checks.** `atlas_db_open` migrates, and a diagnostic that upgrades
  the artefact it was asked about has destroyed the evidence. The A4 record
  checks use `atlas_db_open_readonly`, which cannot.
- **Verification checks the declared length against the actual one**, before
  anything else. `PRAGMA integrity_check` walks the pages the b-trees reach, so
  a file truncated in unallocated space or by less than a page passes it — and a
  backup missing its tail is exactly the failure an operator has. Removing that
  check makes truncation verify as ok.
- **Say what verification cannot do.** SQLite has no per-page checksum, so a
  byte flipped inside an ordinary value leaves a structurally valid database and
  nothing Atlas runs will find it. Decision revisions are the exception, because
  every one is rehashed from its stored content.
  `tests/test_backup.c` asserts all three cases *including the undetected one*,
  so the limitation cannot quietly vanish from the documentation while remaining
  true of the code.

  The overclaims A5 forbids — about encryption, signatures, durability under
  hardware failure, exhaustive corruption detection, differential copies and
  portability — are enumerated in one place, `FORBIDDEN[]` in
  `test_no_operational_claim_is_stronger_than_the_implementation`, together with
  the wording that is *required* to stay. The list is not repeated here for the
  same reason A4's is not: a second copy would drift, and this file is one of the
  files the scan covers.
- **`RETENTION[]` in `service_maintenance.c` is the whole retention policy, and
  every table has a row with a written reason.** A table added without one is a
  test failure, checked in both directions against `sqlite_schema`. The reason
  is the deliverable: a classification without one is a label, and a label is
  what lets a later phase quietly reclassify a table because deleting from it
  would have been convenient.
- **Exactly one table is prunable, and widening that needs an argument.**
  `repo_events`, because it already carried a documented per-repository ceiling,
  its `id` is `AUTOINCREMENT` so no cursor can be re-pointed by a deletion, and
  the durable evidence lives elsewhere. `scans` is *not* prunable and the reason
  is A4's: `files.first_seen_scan_id` and friends hold `scans.id`, a plain rowid
  SQLite reuses. Derived tables are not prunable by age either — a half-aged
  derived table is not a smaller index, it is a wrong one, and nothing in it
  records that rows are missing.
- **There is no background deleter, and A5 must not grow one.** Nothing prunes
  on a timer, at startup, on low disk, or as a side effect of another command.
  A row goes away when an operator runs `maintenance prune --apply`, and at no
  other moment.
- **Bounds are checked, never clamped.** A negative `--older-than` is a usage
  error, not a silent default: a discarded number nobody is told about deletes
  more than was asked for. Zero means "not given" and takes the documented
  default.
- **The delete is per batch, not per loop.** `atlas_db_maintenance_events_prune`
  opens and commits one transaction per bounded batch, which is A1's rule about
  never holding a write transaction across unbounded work. A failure rolls that
  batch back whole and the operation is idempotent, so re-running finishes it.
- **`ATLAS_BACKUP_FAULT` is compiled into every build on purpose.** An `#ifdef`
  would mean the shipped binary is not the one the failure tests ran against. It
  can only ever cause an operation to *abort*: there must never be a fault point
  that skips a check, weakens a guarantee or publishes something.

## A6 layers — additions

```
src/gate     gate.c (the vocabularies, the fold, the packed reason list),
             assess.c (the deterministic assessment and the evidence digest)
src/db       db_gate.c (bounded ancestry, the change range, the append-only
             revalidation ledger)
src/core     service_gate.c (the snapshot discipline and the `gate` command)
```

The one A6 write goes through `atlas_decision_apply_in_tx`, unchanged. There is
no second write point, and `op_revalidate` is a case in the existing switch
rather than a new path.

## A6 rules — these are not negotiable

- **An assessment is an observation, never a judgement about the decision.**
  STALE means the anchors moved and a human has to look; it does not mean the
  decision was wrong, has been revoked or no longer applies. IMPACTED means a
  bounded walk from the anchors reached something that moved. Both are review
  signals. Atlas cannot know whether an architectural decision survives a change
  to the code it concerns — that is a question about intent, and Atlas holds
  bytes and graph edges. The overclaims A6 forbids and the wording that must
  stay are enumerated in one place, `FORBIDDEN[]` and `REQUIRED[]` in
  `tests/test_gate_trust.c`, which scans the documentation, the headers and the
  source. The lists are not repeated here for the reason A4's and A5's are not:
  a second copy would drift, and this file is one of the files the scan covers.
- **UNKNOWN is zero and BLOCKED is zero.** A zeroed assessment is one nobody
  filled in, and the safe reading of that is not "fresh" and not "pass". Moving
  either zero would make a `memset` produce a permissive default.
  `atlas_gate_report_init` sets BLOCKED, so the engine must assert PASS
  deliberately at the point it commits to a real report — BLOCKED absorbs in
  `atlas_gate_fold`, and a report that started at its safe default could never
  be lifted out of it.
- **A verdict is the weakest of its reasons, by construction.**
  `atlas_gate_assessment_note` is the only way freshness is ever set, and it
  folds before it records — so a reason that does not fit in the list still
  weakens the answer, and a decision with thirteen problems cannot report a
  better verdict than one with twelve. `atlas_gate_reason_freshness` is the
  single authority on what each reason implies, asked by the tests rather than
  restated in them.
- **Nothing is cached.** Freshness is recomputed on every read, for the reason
  A4 gives about link currency. The only stored assessment is the one a
  revalidation captured, and it is stored because it is history rather than
  state.
- **A limit is never absorbed.** A truncated walk cannot report that it found
  nothing, and a change set that stopped being collected must not be tested for
  membership at all — every miss would be indistinguishable from a path that was
  never in it. Hitting any bound is TRAVERSAL_LIMIT, which is UNKNOWN, which is
  BLOCKED.
- **The snapshot order is the consistency argument, and it is deliberate.**
  Open a read transaction and read the repository row first, so SQLite's
  deferred snapshot is taken; then ask Git for the live HEAD; then assess. A
  commit that lands between the two makes them disagree and the answer is
  BLOCKED — the race costs a refusal, never a pass on a state Atlas has not
  seen. Reversed, the failure is silent: Git first, then a snapshot taken after
  the daemon indexed the commit Git had just reported, and the two agree about a
  state neither measured together.
- **The gate takes no lock, writes no row and creates no process.** That is what
  makes "normal read-only indexing is never blocked by the gate" a property of
  the code rather than a promise: the gate has nothing with which to block it.
- **Ancestry and the change range are computed from the index, never from a new
  git call.** A6 adds no git call site and no allowlist vector. The walk over
  `commits.parents` keeps three non-answers apart on purpose: LIMIT and UNKNOWN
  mean Atlas stopped before it could tell, and NOT_ANCESTOR is the only value
  that asserts anything — produced only when every reachable commit was expanded
  without meeting a parent that was never ingested. Collapsing them would turn
  "we do not hold that much history" into "your history was rewritten".
- **Which baseline the direct-evidence question uses depends on whether the
  decision has been revalidated, and this is load-bearing.** A revision is
  immutable, so its link snapshots can never be updated; if they stayed the
  baseline, a decision an operator had just checked would report STALE for ever.
  Without a revalidation the baseline is each link's own snapshot; with one it
  is the evidence digest that revalidation recorded. `EVIDENCE_UNRESOLVED` is
  derived the same way under both, because "Atlas could not look" never
  establishes that the evidence still resolves.
- **The evidence digest is domain-separated and length-prefixed**, for A4's
  reasons exactly. It covers what the anchors resolve to *now*, which is the
  opposite of what the content hash covers, and the two must never be confused:
  the content hash is what an approval bound and never changes; this one is
  expected to change and the point of computing it is to notice when it has.
- **Revalidation changes no lifecycle state.** No `decision_events` row, no
  status change, no edit to the approved revision. The ledger replay is over
  exactly the vocabulary it was over before. `prior_freshness` and
  `prior_reasons` preserve the assessment the operator was shown — recorded at
  challenge issue rather than recomputed at consume, so what is kept is what was
  actually seen.
- **Both A6 drift checks are database reads.** Consumption runs on the writer
  thread inside the transaction that spends the capability, where A1 forbids
  creating a process or reading a file. The indexed commit comes from the
  repository row and the digest from the stored index. Do not add a git call or
  a filesystem read there.
- **A6's model-facing surface is one read.** `atlas_gate_check` over
  `gate.check`. There is no RPC method, MCP tool, hook or plugin command that
  clears, overrides, caches or recomputes a freshness result, and none that
  revalidates. `decision.revalidate` sits beside `decision.approve` over IPC and
  is equally useless without a capability only the terminal channel can obtain.
  A4's honesty limits about that channel apply word for word.
- **`decision_validations` is append-only.** `src/db/db_gate.c` contains no
  UPDATE and no DELETE that touches it, and there is no `_clear`, no `_prune`
  and no `_forget`. Both A6 tables are CANONICAL and not prunable in
  `RETENTION[]`; an age-pruned validation history would silently move every
  surviving decision's validation point backwards.
- **`atlas doctor` checks the ledger's structure and not its evidence.** Rows
  must reference a revision, a document and a consumed revalidation challenge
  that exist, and must carry the digest their revision carries. It must **not**
  re-derive evidence digests against the live index: those are meant to drift,
  and a diagnostic that reported ordinary code changes as corruption would teach
  everybody to ignore it.

## A7 layers — additions

```
src/core     authority.c (the operator-authority probe and the one refusal)
docs/security A7_THREAT_MODEL.md, A7_SECURITY_REVIEW.md
```

There is deliberately **no `src/ipc` file here and no MCP tool**. A profile's
authority state is inspected, never set, and never over a socket.

## A7 rules — these are not negotiable

- **A terminal is not authority, and Atlas must never act as though it is.**
  Nothing observable from inside a process distinguishes a human from a program
  running as the same uid: not `isatty`, not `/dev/tty`, not pseudo-terminal
  ownership, not environment variables, not parent-process names, not session
  ids, not a typed confirmation, not timing. `tests/test_decision_operator.c`
  allocates a pty and types into it, which is the demonstration rather than a
  claim about one. Do not add a check of this shape and do not reintroduce one
  that was removed.
- **Authority is configured outside the reach of the principal it constrains, or
  it does not exist.** All four conditions in `atlas/authority.h` hold or the
  profile is LOCKED: a root-anchored policy reached without traversing a
  symlink, root ownership with no other writer on every component, an
  `operator_uid` matching `getuid()`, and a root-owned non-writable executable.
  The last one is not decoration — a check running from a binary the constrained
  uid can replace reports whatever that uid last compiled.
- **`ATLAS_AUTHORITY_POLICY_PATH` is a compiled-in constant.** No environment
  override, no flag, no data-directory-relative variant. A caller that can
  choose the policy is not constrained by it, and adding one deletes the phase.
- **LOCKED is zero**, for the reason A6 keeps UNKNOWN and BLOCKED at zero. There
  is exactly one `state = ATLAS_AUTHORITY_GRANTED` assignment and it is the last
  statement of the probe; every other path leaves what `memset` left.
- **The guarded set is the decision lifecycle and nothing else, and widening it
  needs the argument in `atlas/authority.h`.** Backup create, backup restore,
  maintenance prune and repository registration were considered and deliberately
  excluded: against a process running as the uid that owns the data directory,
  `cp`, `mv`, `rm` and `sqlite3` reach the same bytes, so a check there reads as
  protection in a review and provides none — and in a separated deployment the
  filesystem already refuses. **A check an adversary walks around is worse than
  no check.** The lifecycle is different because of what Atlas *produces*: it
  mints a coherent record — consumed challenge, ledger event, status cache,
  `LOCAL_OPERATOR_CONFIRMED` — that nothing downstream can distinguish from a
  human's. Refusing converts an undetectable forgery into one that disagrees
  with the ledger and fails `atlas doctor`. That is the whole claim.
- **The authority check runs before the terminal is opened, before a capability
  is minted and before a prompt is printed.** A prompt in a locked profile is a
  question whose answer the caller can supply. Ordering it first also means a
  locked profile never reports on the shape of a request it was not going to
  perform.
- **No RPC method, MCP tool, hook or plugin command mints or spends a lifecycle
  capability, or changes the registry.** `decision.challenge`, `decision.approve`,
  `decision.reject`, `decision.supersede`, `decision.revalidate`, `repo.add`,
  `repo.ensure` and `repo.remove` were **deleted**, not left refusing — an absent
  method is answered by the dispatcher's unknown-method case, and a refusing one
  is a refusal a later edit can weaken. `tests/test_a7_authority.c` asks a live
  daemon for 34 names, including case variants and aliases, and requires every
  one to answer `unknown method` rather than merely to fail.
- **Nothing registers a repository except an operator.** Already-registered
  repositories are discovered and attached; an unknown directory is reported and
  left alone. Never restore auto-registration to a hook, an MCP root grant or a
  session event: those inputs are chosen by, or influenced by, the model.
- **`atlas doctor` reports the profile and never treats a locked one as a
  fault.** A locked profile is the correct state of an unseparated machine. It
  does not affect `ok`.
- **Do not claim A7 protects the database.** It does not. A process running as
  the uid that owns `atlas.db` can write any row with SQLite and no Atlas code
  path. Only a separate OS principal protects the record; the review says
  exactly what that deployment involves.

## A7.1 layers — additions

```
src/core     rootpath.c (the root-anchored walk, moved out of authority.c),
             syspolicy.c (the system-deployment policy)
src/ipc      sock.c gains system-mode socket ownership and peer authorization
deploy/a71   atlas.service, system.conf.template, authority.conf.template
scripts      a71-preflight.sh, a71-deploy.sh, a71-verify.sh, a71-rollback.sh
docs/security A7_1_THREAT_MODEL.md, A7_1_OPERATIONS.md
```

## A7.1 rules — these are not negotiable

- **`nocdem` and root are trusted by design and Atlas does not defend against
  them.** The operator holds passwordless root; any process intentionally
  launched as that account — including an AI session — is outside Atlas' OS
  isolation guarantee, by the operator's explicit decision. **Never write a test
  asserting `nocdem` cannot do something**, and never claim in prose that it is
  constrained. The adversary is `atlas-worker`.
- **Every persistent or autonomous model process runs as `atlas-worker`, never
  as `nocdem`.** That is the architectural commitment the separation rests on.
  A8's dispatcher inherits it; if it is broken the guarantee is void and no code
  change restores it.
- **The guarantees that matter are kernel-enforced, not Atlas-enforced.**
  `atlas-worker` cannot read the index or the backups (0700 `atlasd`), cannot
  replace the binary or the policies (root-owned), and cannot stop the service
  (no sudo). Atlas' own checks are the second layer, not the first. Do not
  replace a filesystem guarantee with a check in C.
- **The socket is the one place the two principals meet, so everything on it is
  Atlas' problem.** Peer identity is `SO_PEERCRED` and nothing else — never a
  uid, gid, pid or role from the request body, the environment or `/proc`. A
  client describing itself is not evidence about itself.
- **`ATLAS_SYSPOLICY_PATH` is a compiled-in constant**, like
  `ATLAS_AUTHORITY_POLICY_PATH` and for the same reason. No environment
  override, no flag, no data-directory-relative variant.
- **LEGACY is zero.** A zeroed `atlas_syspolicy` serves the daemon's own uid and
  nobody else. There is one `state = ATLAS_SYSPOLICY_SYSTEM` assignment and it
  is the loader's last statement. Anything missing, malformed, symlinked,
  group-writable or non-root-owned is legacy mode with a reason.
- **An unrecognised policy key is an error, not something skipped.** A policy
  Atlas half-understands is one whose author believes they configured something
  Atlas never read — and one day that something will be a restriction.
- **The socket's owner, group and mode are set explicitly and then read back.**
  A mismatch unlinks the socket and refuses to start. A socket more open than
  intended is worse than no daemon.
- **There is no fallback from the system index to the per-user one.** With a
  policy active, `ATLAS_DATA_DIR` and `$HOME` stop selecting an index;
  `--data-dir` still wins because it is explicit. A client that cannot reach the
  daemon must fail, not quietly read the pre-cutover database that A7.1 leaves
  in place as a rollback target.
- **The old per-user database is never modified, including to mark it
  non-authoritative.** A rollback target that has been edited is not one.
- **Terminal presence stays a UX confirmation and must never be described or
  tested as the security boundary.** It protects against approving the wrong
  revision; it proves nothing about who typed it.
- **`operator_uid` is the `atlasd` uid**, because no other principal can open
  the index. The human path is the documented offline ceremony in
  `docs/security/A7_1_OPERATIONS.md`, and it is not exposed through MCP, the
  plugin or any model-callable helper.
- **Deployment tooling never uses `eval`, never interpolates repository text as
  shell, never recursively deletes or chowns, and never names `/opt/dna` or
  `/opt/swapper` except to read.** Dry-run is the default and `--apply` is a
  second deliberate invocation.

## A8 layers — additions

```
src/orch      orch.c (the vocabularies, the state machine, the canonical job
              digest, the identifiers), policy.c (the root-owned orchestration
              policy)
src/db        db_orch.c (the one write point over the migration-8 tables, and
              the bounded reads)
src/ipc       server_orch.c (two disjoint method groups, selected by the peer's
              uid from SO_PEERCRED)
src/orch      workspace.c (the per-attempt tree, the snapshot, artifacts,
              bounded removal, redaction), driver.c (the versioned driver
              interface, the deterministic fake and the Claude Code driver),
              dispatch.c (the loop that runs as `atlas-worker`)
src/core      service_orch.c (the `job` and `dispatcher` commands),
              proc.c gains an idle bound, a cancel callback and a working
              directory — the one process-creation path, extended not duplicated
src/git       git.c gains ls-tree, cat-file blob and diff --no-index, the three
              reads a snapshot needs
deploy/a8     atlas-dispatcher.service, orchestration.conf.template
scripts       a8-deploy.sh, a8-rollback.sh
docs          orchestration.md
```

`docs/orchestration.md` ends with a status section naming exactly what exists
and what is deferred; keep it truthful. Applying a patch, committing, pushing,
branching and every GitHub verb are **absent rather than refused**, and their
absence is the deferral.

## A8 rules — these are not negotiable

- **A completed job is not an authority.** It approves nothing, applies nothing
  and commits nothing. The patch is an artifact with a recorded digest, and
  there is no code path that applies it to a registered repository. A7's
  lifecycle authority is untouched: no orchestration method mints or spends a
  capability. `tests/test_orch_rpc.c` asks a live daemon for every name such a
  method would plausibly have — including `job.apply`, `job.commit`,
  `job.push` and `job.merge` — and requires every one to answer `unknown
  method`. Their absence is the deferral.
- **`atlas_orch_apply_in_tx` is the only function that writes an orchestration
  row.** The transition check, the lease check, the attempt allocation, the
  ledger append and the status-cache update all live behind it, and every one
  would be bypassable if a second path reached the tables. It has exactly one
  caller. That is the rule `settle()`, `atlas_db_evidence_insert` and
  `atlas_decision_apply_in_tx` follow.
- **Every state change is a compare-and-swap that names the state it observed**,
  and requires exactly one changed row — A4's rule, so a concurrent transition
  loses deterministically instead of last-write-wins.
- **Ordering is the ledger's AUTOINCREMENT id, never a timestamp.** Wall-clock
  times are evidence. The single decision that is genuinely about time is
  whether a lease has expired.
- **UNKNOWN is zero and DISABLED is zero**, for the reason A6 keeps UNKNOWN and
  BLOCKED there. A `memset` must not produce a runnable job or an enabled
  policy. The schema enforces it independently: every state CHECK omits
  `UNKNOWN`.
- **There is no edge from CANCEL_REQUESTED to SUCCEEDED.** That is how
  "completion and cancellation cannot both win" is decided by the machine rather
  than by whichever message arrived first. Do not add one.
- **At most one unreleased lease per job is a schema constraint**, a partial
  unique index, not care — the shape A4 uses for "at most one approved revision
  per document". It is what makes concurrent execution a hard failure.
- **A lease token is never stored.** Only a domain-separated digest of it is,
  and the token leaves the daemon once, at grant. A worker is identified by its
  token and by nothing else; its claimed pid and uid are recorded as claims and
  used for nothing, because a client describing itself is not evidence about
  itself.
- **The two RPC groups are selected by SO_PEERCRED and by nothing else**, and
  they are disjoint rather than nested. A name in the group a peer is not in
  answers `unknown method`, the same as a name that does not exist: a refusal
  that distinguished "you may not" from "there is no such thing" would tell a
  caller what to try next. A7.1's "the socket carries no authority" still holds
  — the dispatcher group confers none, and membership is a root-owned fact.
- **`ATLAS_ORCHPOLICY_PATH` is a compiled-in constant**, like
  `ATLAS_AUTHORITY_POLICY_PATH` and `ATLAS_SYSPOLICY_PATH`, and for the same
  reason. An unrecognised key is an error, not something skipped.
- **Bounds refuse, never clamp.** A5's rule about `--older-than`: a discarded
  number nobody is told about produces a job unlike the one that was asked for.
- **A validation command is a vector of counted arguments, never a string.**
  There is no field in the protocol that could hold a shell fragment. Shell
  syntax in *task text* is explicitly allowed and must stay allowed — nothing
  passes it to a shell, and refusing a dollar sign would imply the opposite.
- **Orchestration tables are CANONICAL and none is prunable.** Nothing rebuilds
  a job record; the repository never held it. `RETENTION[]` carries a written
  reason for each of the eight.
- **`orch_jobs.repo_id` is a soft reference with no foreign key**, and it is
  cleared inside `atlas_db_repo_remove`'s transaction. `repositories.id` is a
  reused rowid; a pointer left behind would eventually name a different
  repository. That is the A4 defect, and it is not repeated.

- **The daemon reads registered repositories; the worker never does.**
  `atlasd` enumerates the committed tree and streams a canonical bounded
  snapshot over the socket; the dispatcher unit sets `InaccessiblePaths=/opt`.
  A8's first cut had the worker read the repository itself, which required the
  untrusted account to hold a read path to `/opt` and, on a machine where the
  repositories belong to somebody else, git refused outright. Do not reintroduce
  worker-side repository access in any form.
- **Every repository invocation carries `-c safe.directory=<canonical root>`,**
  built from the path Atlas resolved from its own registry and from nothing
  else. Global and system config stay unread, so an operator's or a
  repository's own declaration cannot influence anything. **The older claim that
  git ignores `safe.directory` from `-c` is wrong for git 2.39.5** — measured
  directly — and believing it left the A7.1 daemon unable to open any registered
  repository for the whole of its deployment.
- **A snapshot carries no git metadata.** No `.git` under an attempt, so there
  is no hostile configuration, no hook, no alternate, no index and no submodule
  or LFS machinery. A tracked symlink is refused and counted, never recreated; a
  gitlink is refused at listing time. Do not "add submodule support" by
  initialising one — that is a new phase's argument, not a flag.
- **A workspace path is never taken from anywhere but Atlas.** A validated
  worker root, an Atlas-generated job id, an integer attempt. Every descent is
  `openat` with `O_NOFOLLOW` from a descriptor validated once, never a path
  re-resolved from a string. There is no "remove this path recursively"
  primitive and there must not be one.
- **`atlas_proc_run` is still the only process-creation path.** A8 extended it
  with an idle bound, a cancel callback and a working directory rather than
  writing a second runner. Adding a second fork/exec anywhere would break the
  rule the whole git-safety argument rests on.
- **Cancellation is asked for, never signalled.** The daemon has no path into
  the worker's process tree, by design; a running child learns of a cancellation
  through the dispatcher's heartbeat. Do not add a signal path.
- **A zero exit is not a success claim.** A driver that exits zero having
  produced something that is not a result document is `MALFORMED_RESULT`. The
  check is structural and deliberately shallow: a model's output is never parsed
  as authority.
- **Log redaction is a mitigation and must be described as one.** It catches
  shapes it knows. The real defence is that no credential is ever placed in a
  workspace, an environment or a job specification. Never write "logs are
  redacted" without that second sentence.
- **The Claude driver uses a root-installed service credential or nothing.**
  `/etc/atlas/claude.env`, root-owned, reached through `atlas_rootpath_open`.
  Atlas never creates it, never prints it, and never reaches for an operator's
  personal session — and `live_model` must be on as well, so there are two
  independent gates.

## Extending A8 safely

- **A new state** means editing `atlas_orch_state`, both schema CHECKs,
  `atlas_orch_transition_allowed`, and the enumerated table in
  `tests/test_orch_model.c`, which checks all 144 pairs. The transition table is
  a *function* precisely so a test cannot pass by agreeing with a second copy.
- **A new RPC method** goes in one of the two tables in `server_orch.c`, and
  which table is the security decision. If it is plausibly an authority or
  mutation verb, add its name to the negative enumeration in
  `tests/test_orch_rpc.c`.
- **A new field in the job digest** changes what every stored `spec_digest`
  means, so it invalidates every idempotency record. Bump
  `ATLAS_ORCH_SPEC_DOMAIN`, and add a row to the table in
  `docs/orchestration.md` with a reason.
- **A new policy key** means a branch in `atlas_orchpolicy_load_at`, a field, a
  documented line, and a malformed-matrix case. An unknown key stays an error.

## Extending A7.1 safely

- **A new policy key** means a branch in `atlas_syspolicy_load_at`, a field, a
  line in `deploy/a71/system.conf.template` explaining it, and a case in the
  malformed matrix in `scripts/a71-verify.sh`. An unknown key must stay an
  error.
- **A new client identity** is a `client_uid` line an operator adds. Never a
  group check in C, never a name lookup at accept time, and never a role
  supplied by the client.
- **A new writable path for the service** means an argued edit to
  `ReadWritePaths` in `deploy/a71/atlas.service`. The plugin, both home
  directories, the backups, the binary, the policies and every indexed
  repository must stay out of it — that absence is ATLAS-A7-006's fix.

## Extending A7 safely

- **A new guarded operation** means a member of `atlas_authority_op`, a case in
  `atlas_authority_op_name`, a call at the CLI entry point, and — the part that
  is not optional — a written argument that refusing it stops something a shell
  builtin does not already do. Without that argument it is theatre.
- **A new authority reason** means a member of `atlas_authority_reason`, a name,
  a one-sentence explanation that says what would change it, and a case in
  `test_no_unprivileged_shape_grants_authority`. Keep UNKNOWN at zero.
- **A new RPC method** must be a read. If it is plausibly an authority verb, add
  its name to the negative enumeration in `tests/test_a7_authority.c` so the
  list keeps pace with the vocabulary.

## Extending A6 safely

- **A new reason code** means a member of `atlas_gate_reason`, a row in
  `REASONS[]` in `src/gate/gate.c` carrying its name *and* the freshness it
  implies, a row in the table in `docs/impact-gates.md`, and nothing else — the
  verdict follows from the reason rather than being chosen beside it. A member
  with no row falls through to the placeholder name and
  `tests/test_gate_model.c` fails on it.
- **A new freshness value** means editing `atlas_gate_freshness`, `strength()`
  in `src/gate/gate.c`, `atlas_gate_fold`, the CHECK on
  `decision_validations.prior_freshness` and the challenge's, and the enumerated
  table in `tests/test_gate_model.c`. Keep UNKNOWN at zero.
- **A new bound** goes in `include/atlas/limits.h` under the A6 section with a
  written reason, is reported through `limit_reached` and `limit_detail`, and
  notes `TRAVERSAL_LIMIT`. A bound that silently trims a result is the one thing
  A6 must never have.
- **A new field in the evidence digest** changes what every stored
  `evidence_digest` means, so it invalidates every outstanding capability and
  every revalidation baseline. Bump `ATLAS_GATE_EVIDENCE_DOMAIN` when it
  changes.
- **A new A6 RPC method** must be a read, and adding a mutating one deletes the
  phase's guarantee. `tests/test_gate_trust.c` asks a live daemon for the names
  such a method would plausibly have; add the new name there if you add a read,
  so the negative list keeps pace.

## Extending A5 safely

- **A new verification check** goes in `atlas_db_backup_inspect`, before the
  checks that read rows, and gets its own verdict only if an operator would act
  differently on it. Add the case to `tests/test_backup.c`; if the check cannot
  detect something a reader would assume it detects, say so in
  `docs/operations.md` and add the assertion that it is *not* detected.
- **A new prunable table** means a row in `RETENTION[]` with `prunable = true`,
  an eligibility predicate whose count and delete select the same set, and an
  argument that survives being read back: what holds a rowid into it, what
  cursor points at it, and what is lost that cannot be rebuilt. It also fails
  `test_exactly_one_table_is_prunable` until somebody updates it deliberately,
  which is the point.
- **A new backup field** is reported, never stored: A5 adds no migration and the
  schema stays 6. Do not bump it to record a timestamp that can be observed from
  outside the database.
- **A new fault point** is a string in `fault()` and a case in
  `test_every_injected_failure_leaves_the_original_untouched`. It must abort,
  never weaken.

## Extending A4 safely

- **A new lifecycle state** means editing `atlas_decision_state`, the schema
  CHECKs on `decision_revisions.state`, `decision_documents.current_status` and
  `decision_events.event`, `atlas_decision_transition_allowed`, the replay in
  `atlas_db_decision_verify`, `recompute_status()`, and the enumerated table in
  `tests/test_decision_model.c`. The transition table is a *function* precisely
  so a test cannot pass by agreeing with a second copy of the rules.
- **A new operation that must be atomic with something else** uses
  `atlas_decision_apply_in_tx` and owns the transaction itself.
  `atlas_decision_apply` is begin + that + commit; calling it from inside
  another transaction would nest, and its rollback would discard the caller's
  work. **Never add a second `atlas_db_begin` inside `apply_in_tx`** — a stray
  one made `decision propose` report success and write nothing, because the
  nested commit only decremented the depth counter.
- **A new writer payload** goes in `atlas_decision_op`, is freed in
  `atlas_decision_op_free`, is copied field by field in
  `atlas_writer_decision`'s result block, and is serialised in `op_to_params`
  for the daemon path. The service layer routes a write locally when this
  process holds the lock and over the socket when it does not; both must carry
  it or the two paths behave differently.
- **A new RPC method** goes in `DECISION_METHODS[]` in `server_decision.c`.
  Decide explicitly whether it consumes a capability, and if it does, add it to
  `atlas_decision_op_needs_challenge` — that function is asked by
  `atlas_decision_apply` itself, so a new kind cannot default into the
  unauthenticated set.
- **A new MCP tool** follows the A2 rule, plus: it must not accept a capability
  argument, and adding it changes the pinned count in `tests/test_plugin.c`.
- **A new renderer field** carrying decision prose is already safe-encoded by
  the service layer — do not encode again — and both renderers say so at the
  top. Anything copied out of a result struct must be copied, not aliased:
  row callbacks hand out borrowed pointers.
- **A new envelope line** must be added to the `KEYS` list in
  `tests/test_ai_trust.c`. That list is the envelope's closed vocabulary and has
  now caught two phases in a row.
- **A new claim about approval** goes through the tripwire in
  `tests/test_decision_mcp.c`: the forbidden-phrase list and the
  required-wording list are both there, and both are the point.

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

## The prepared-statement cache

`atlas_db_prepare` caches by the SQL **pointer**, because every call site passes
a string literal. It now also confirms the text against a copy the cache owns.

That is not belt and braces: a caller that formats SQL into a reused buffer
presents the same address with different text on the next call, and without the
confirmation is handed the previous statement — which then executes the wrong
query against the right bindings. A test in this repository did exactly that and
got an answer about the wrong session. Pass a string literal and bind
parameters; anything else prepares fresh, which is what the header has always
said happens.

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
`docs/code-intelligence.md` · `docs/decision-lifecycle.md` ·
`docs/operations.md` · `docs/impact-gates.md` ·
`docs/security/A7_THREAT_MODEL.md` · `docs/security/A7_SECURITY_REVIEW.md` ·
`docs/security/A7_1_THREAT_MODEL.md` · `docs/security/A7_1_OPERATIONS.md` ·
`docs/orchestration.md` · `docs/git-safety.md` · `docs/daemon-and-ipc.md` ·
`docs/watcher-consistency.md` · `docs/systemd-user-service.md` ·
`docs/ai-trust-boundary.md` · `docs/claude-integration.md` ·
`docs/backlog.md` · `docs/roadmap.md` ·
`third_party/yyjson/PROVENANCE.md` ·
`integrations/claude/atlas/README.md`

Keep these current when behaviour changes. If you change the JSON shape, the
schema, or an exit code, that is a contract change — update the docs in the same
change.

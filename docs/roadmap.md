# Roadmap

Atlas is built in phases, and each phase has to earn the next one. The order is
deliberate: nothing that interprets meaning is built before the layer that records
facts is trustworthy.

> **Renumbering note.** The A0 roadmap listed decisions and ADRs as A1 and
> compile-database parsing as A2. A1 was redirected to the daemon: an index that
> has to be refreshed by hand is not one anybody keeps current, and every later
> phase reads from it. Decisions and ADRs have moved to A3, and the phases after
> it have shifted by one. The invariants at the bottom are unchanged.

## A0 — native C foundation (done)

A tested, read-only CLI that registers Git repositories, scans tracked files,
indexes history into SQLite, and searches the index with stable human and JSON
output. Every fact carries `SOURCE` or `GIT` provenance, and any request for a
reason returns `UNKNOWN`.

Done when: all builds are warning-free, the suite passes under ASan and UBSan, and
the read-only guarantee is proven by tree-digest tests.

## A1 — daemon, IPC and incremental indexing (done)

Make the index stay current without being told to.

- `atlas daemon run`: one foreground process, the same binary, supervised by a
  systemd **user** service
- inotify watching of every registered worktree, its git directory and the shared
  refs, with debouncing, coalescing and periodic reconciliation
- incremental indexing: an unchanged repository is examined without reading a
  single file's content, and one changed file costs one file
- incremental history, with force-push and rebase detected rather than walked past
- per-file discovery inside new untracked directories, honouring `.gitignore`
- a bounded, versioned, length-framed local IPC protocol on a 0600 Unix socket,
  peer-credential checked
- exactly one writer, enforced by an advisory lock rather than by convention
- a durable, monotonic event journal with an explicit cursor, for A2
- an honest currency model: Atlas never reports the index as current when it
  cannot prove it observed every change

Done when: the suite passes under Debug, Release, ASan/LSan, UBSan **and
ThreadSanitizer**; the smoke and adversarial suites stay green; the performance
acceptance in `scripts/perf.sh` shows zero content reads on an unchanged pass;
and registered repositories are byte-identical throughout.

### The A0 limitation A1 was required to close

A0's `atlas diff` reported a wholly untracked directory as one collapsed entry
and did not descend into it, so files inside a newly created directory had no
individual path, size or hash. A newly created directory is exactly where new
work appears, so a change-session recorder that could not see inside one would
miss the beginning of a piece of work.

All six acceptance criteria the A0 roadmap set are met:

1. **Per-file discovery, recursively** — `atlas_git_ls_untracked` enumerates every
   untracked non-ignored path, and each is recorded with its safe path, size,
   SHA-256 hash and `SOURCE` evidence, exactly like a tracked file.
2. **Bounded, and it says so** — `ATLAS_WATCH_MAX_DISCOVER_FILES` and
   `ATLAS_RECONCILE_MAX_FILES`, both reported through `truncated` with a reason.
   Never a silent stop.
3. **Respects `.gitignore`, and separates the reasons** — the enumeration is
   git's own, and ignored roots are counted separately in `ignored_paths`, so
   "skipped because ignored" is distinguishable from "skipped because a ceiling
   was reached".
4. **Never follows a symlink out of the repository** — `atlas_path_open_nofollow`
   throughout; a symlink is hashed by its link text and never read through.
5. **The collapsed entry remains** — `atlas diff` is unchanged. Per-file
   discovery is additive, in the indexer, not a replacement for the cheap answer.
6. **Tested** — a new directory with several files; nested new directories; an
   ignored subtree; a symlink out of the tree; and the truncation path, in
   `tests/test_reconcile.c` and `tests/test_daemon.c`.

## A2 — Claude Code integration

The phase that makes Atlas useful to an agent, and the first one where
repository text reaches something that interprets it.

- a skill or adapter driving the CLI and the IPC event cursor, so an agent can
  ask "what changed since I last looked" and get a bounded, resumable answer
- **a model-context trust boundary**, specified in
  [ai-trust-boundary.md](ai-trust-boundary.md) before any of it is built

The rule that governs this phase: **Atlas' safe-text encoding is not a defence
against prompt injection and cannot be extended into one.** Encoded repository
prose is terminal-safe and JSON-safe and still semantically untrusted. Raw
repository text must not be injected as trusted instructions; it enters model
context as quoted, attributed evidence with its provenance visible, and what an
adapter is *able* to do is what constrains it, not how carefully it is prompted.

`UNKNOWN` survives into this phase, and it is a safety property here as much as
an honesty one: a model that must answer will be pushed toward whatever the
repository text suggests.

## A3 — decisions, ADRs and change reasons

Give Atlas something honest to say when asked "why".

- discover and parse Markdown decision records and ADRs in the repository
- link decisions to the paths, commits and symbols they concern
- introduce `DECISION` and `USER_STATEMENT` evidence, and lift the restriction in
  `atlas_db_evidence_insert` to exactly those two additional kinds
- `atlas why PATH` answers with linked decisions, or `UNKNOWN` when there are none

The rule that survives from A0: an unlinked commit message is still not a reason.
A decision has to be recorded to be reported as one.

## A4 — compile_commands.json and clangd

- parse the compile database A0 already records, with the same bounded-memory and
  hostile-input discipline as the Git parsers
- resolve translation units, include paths and defines
- integrate `clangd` as a subprocess through the existing safe process API, with
  the same argv allowlist treatment Git gets
- report toolchain and compile-database drift in `atlas doctor`

## A5 — symbols, calls and the dependency graph

- extract symbol definitions and references per translation unit
- build a call graph and a file/module dependency graph
- attach `SOURCE` evidence to every symbol fact and `INFERENCE` evidence, with the
  derivation stated, to anything computed rather than read
- `atlas symbol NAME`, `atlas callers NAME`, `atlas deps PATH`

`INFERENCE` arrives here, and it arrives with its reasoning attached. An inference
that cannot say how it was derived is not reportable.

## A6 — impact analysis and stale-document gates

- given a change, report the symbols, files, tests and decisions it touches
- flag decision documents whose subject has changed since the decision was
  recorded
- an exit-code contract usable as a CI gate, so a stale document can fail a build

## A7 — optional MCP adapter

Only if the CLI, the IPC protocol and the A2 skill turn out to be insufficient.
An MCP adapter would reuse the same service layer everything else uses, with no
separate query logic. If A2 is enough, A7 does not get built.

## Invariants that outlive every phase

1. SQLite is a rebuildable index, never the canonical record of history.
2. Git and repository contents are authoritative for source and history facts.
3. Every result preserves provenance.
4. Atlas never invents a reason. `UNKNOWN` is a valid, first-class answer.
5. No Atlas command modifies a target repository.
6. Repository contents are untrusted input — as bytes reaching a terminal, and
   as prose reaching a model. Those are different defences.
7. Human and machine output come from one service layer.
8. Schema changes are numbered, transactional migrations.
9. Exactly one process writes the index at a time.
10. Atlas never claims the index is current when it cannot prove it.

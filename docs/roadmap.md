# Roadmap

Atlas is built in phases, and each phase has to earn the next one. The order is
deliberate: nothing that interprets meaning is built before the layer that records
facts is trustworthy.

## A0 — native C foundation (current)

A tested, read-only CLI that registers Git repositories, scans tracked files,
indexes history into SQLite, and searches the index with stable human and JSON
output. Every fact carries `SOURCE` or `GIT` provenance, and any request for a
reason returns `UNKNOWN`.

Done when: all builds are warning-free, the suite passes under ASan and UBSan, and
the read-only guarantee is proven by tree-digest tests. See the README for what A0
deliberately excludes.

## A1 — decisions, ADRs and change reasons

Give Atlas something honest to say when asked "why".

- discover and parse Markdown decision records and ADRs in the repository
- link decisions to the paths, commits and symbols they concern
- introduce `DECISION` and `USER_STATEMENT` evidence, and lift the A0 restriction
  in `atlas_db_evidence_insert` to exactly those two additional kinds
- `atlas why PATH` answers with linked decisions, or `UNKNOWN` when there are none

The rule that survives from A0: an unlinked commit message is still not a reason.
A decision has to be recorded to be reported as one.

## A2 — compile_commands.json and clangd

- parse the compile database A0 already records, with the same bounded-memory and
  hostile-input discipline as the Git parsers
- resolve translation units, include paths and defines
- integrate `clangd` as a subprocess through the existing safe process API, with
  the same argv allowlist treatment Git gets
- report toolchain and compile-database drift in `atlas doctor`

## A3 — symbols, calls and the dependency graph

- extract symbol definitions and references per translation unit
- build a call graph and a file/module dependency graph
- attach `SOURCE` evidence to every symbol fact and `INFERENCE` evidence, with the
  derivation stated, to anything computed rather than read
- `atlas symbol NAME`, `atlas callers NAME`, `atlas deps PATH`

`INFERENCE` arrives here, and it arrives with its reasoning attached. An inference
that cannot say how it was derived is not reportable.

## A4 — impact analysis and stale-document gates

- given a change, report the symbols, files, tests and decisions it touches
- flag decision documents whose subject has changed since the decision was
  recorded
- an exit-code contract usable as a CI gate, so a stale document can fail a build

## A5 — Claude Code project skill

A skill that drives the `atlas` CLI. No new protocol, no server: the CLI is the
interface, and its stable JSON output is the contract. This is the phase that makes
Atlas useful to an agent, and it is intentionally late, because an agent
amplifying an untrustworthy index is worse than no index.

## A6 — optional MCP adapter

Only if the CLI and skill turn out to be insufficient. An MCP adapter would reuse
the same service layer that the CLI uses, with no separate query logic. If A5 is
enough, A6 does not get built.

## Invariants that outlive every phase

1. SQLite is a rebuildable index, never the canonical record of history.
2. Git and repository contents are authoritative for source and history facts.
3. Every result preserves provenance.
4. Atlas never invents a reason. `UNKNOWN` is a valid, first-class answer.
5. No Atlas command modifies a target repository.
6. Repository contents are untrusted input.
7. Human and machine output come from one service layer.
8. Schema changes are numbered, transactional migrations.

## Mandatory A1 acceptance criteria carried forward from A0

These are not ideas for A1; they are conditions A1 must satisfy before it can be
called done. Each exists because A0 shipped a deliberate, documented limitation.

### A1 watcher: exact per-file discovery inside new directories

**A0 behaviour.** `atlas diff` reports untracked paths using
`git status --porcelain=v2 --untracked-files=normal`, which collapses a directory
containing only untracked files into a single entry ending in `/`. Atlas marks it
`is_directory` and does not descend: no per-file path, size or content hash is
recorded for anything inside it. That keeps the report bounded when a large build
tree or dependency directory appears, and it is what `git status` shows a human.

**Why it is a limitation.** A newly created directory is exactly where new work
appears. A change-session recorder that cannot see the files inside it will miss
the beginning of a piece of work, and a decision linked to "the files added in this
session" would be linked to a directory name instead.

**A1 acceptance criteria.**

1. The watcher discovers every individual file inside a newly created untracked
   directory, recursively, and records for each the safe path representation, the
   size and the content hash, with the same `SOURCE` evidence as any other
   working-tree read.
2. Discovery is bounded and says so: a per-directory and per-session file ceiling,
   with a `truncated` flag and a reason when it is hit, never a silent stop.
3. Discovery respects `.gitignore`, and reports separately what was skipped because
   it is ignored versus what was skipped because a ceiling was reached.
4. Discovery never follows a symlink out of the repository, reusing
   `atlas_path_open_nofollow`; a symlinked directory is refused and reported, not
   descended into.
5. The collapsed-directory entry remains available, so a caller can still ask the
   cheap question. Per-file discovery is additive, not a replacement.
6. Tests cover: a new directory with several files; nested new directories; a new
   directory containing an ignored subtree; a new directory whose entry is a
   symlink; and the ceiling being hit.

Until all six hold, `atlas diff` continues to report the collapsed entry, and that
is documented in the README rather than implied.

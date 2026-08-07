# Roadmap

Atlas is built in phases, and each phase has to earn the next one. The order is
deliberate: nothing that interprets meaning is built before the layer that records
facts is trustworthy.

> **Renumbering note.** The A0 roadmap listed decisions and ADRs as A1 and
> compile-database parsing as A2. A1 was redirected to the daemon: an index that
> has to be refreshed by hand is not one anybody keeps current, and every later
> phase reads from it. Decisions and ADRs moved to A3, and the phases after it
> shifted by one.
>
> **Second renumbering.** A3 then became structural code intelligence — the old
> A4 (compile databases) and A5 (symbols and the dependency graph) merged into
> one phase — and decisions, ADRs and the approval workflow moved to A4. The
> reason is the order of dependency rather than preference: a decision document
> is only useful once Atlas can say what it is *about*, and "about" means files,
> symbols and the relations between them. Linking a decision to a path Atlas
> knows nothing structural about is a weaker link than the one A4 can now make.
>
> The invariants at the bottom are unchanged, and so is the rule that governs
> both phases: an unlinked commit message is not a reason, and a graph edge is
> not one either.

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

## A2 — automatic AI integration (done)

The phase that makes Atlas useful to an agent, and the first one where repository
text reaches something that interprets it.

Delivered, in the same binary:

- `atlas mcp` — a stdio Model Context Protocol server with ten tools, holding no
  database handle of its own and reaching the index only through the A1 socket
- `atlas hook <event>` — fifteen Claude Code lifecycle hooks, each of which fails
  open and stores metadata rather than content
- `atlas integrate claude print|doctor|install|uninstall` — the Atlas half of a
  one-time setup, which writes one file and prints the rest
- migration 4: AI clients, sessions, change sets, attributed changed paths,
  change-reason and decision proposals, plus a per-path working-tree change
  snapshot so an adapter never has to run git
- a Claude Code plugin in `integrations/claude/atlas`, validated by
  `claude plugin validate --strict`
- **the model-context trust boundary**, now implemented rather than specified:
  see [ai-trust-boundary.md](ai-trust-boundary.md) and
  [claude-integration.md](claude-integration.md)

The rule that governed this phase held: **Atlas' safe-text encoding is not a
defence against prompt injection and cannot be extended into one.** So automatic
context contains no repository prose at all — not escaped prose, none — and is
checked against a fixed character allowlist and a 4 KiB ceiling before it leaves.
Repository prose reaches a model only through an explicit tool call, labelled
with its provenance. What constrains the adapter is what it is *able* to do: the
MCP server cannot open the index, start a daemon, scan a repository, write a file
or create a process.

`UNKNOWN` survived, and is now a first-class write: `atlas_record_unknown_reason`
is a tool, and the `Stop` hook records one automatically for every changed path
nobody explained.

Two things A2 deliberately did **not** do: it records model proposals and has no
approval workflow, because it cannot prove a human agreed to anything; and it
does not parse decision documents from the repository, which is A4.

## A3 — structural code intelligence and the relationship graph (done)

Make Atlas able to answer what a file is, what it is connected to, and what might
be affected by changing it — without ever claiming to be a compiler.

Delivered, in the same binary:

- a first-party, bounded, dependency-free lexical C indexer: includes, macros,
  function definitions and declarations, typedefs, tags, enum constants,
  file-scope objects, linkage, and lexical call candidates with their enclosing
  symbol
- `compile_commands.json` ingested as **data**: an argument allowlist, paths
  normalised lexically and checked against the repository, and the `command`
  string hashed rather than stored. Nothing in it is ever executed, and there is
  a test that plants a runnable helper in every field and asserts it never ran.
- migration 5: the structural graph — files with typed evidence-backed roles,
  translation units and their configurations, symbols as *sites*, call
  candidates, one relation table with a resolution class on every edge, an
  ambiguity candidate set, and bounded indexing errors
- structural indexing as a **stage of the A1 pass**, selected by comparing
  content hashes, so an unchanged pass parses zero files even when it is a full
  content-verifying pass
- `code.*` daemon methods, `atlas code status|sync|file|symbol|search|deps|impact`,
  and six MCP tools
- typed structural counters in the automatic context envelope, and not one
  symbol name, path or include spelling

The rule that governed this phase: **every fact carries how it was arrived at.**
`SOURCE_EXACT`, `BUILD_METADATA`, `UNIQUE_LEXICAL`, `AMBIGUOUS`, `UNRESOLVED`,
`CONDITIONAL`, `UNKNOWN` — and `MODEL_PROPOSAL`, which the indexer may not write,
enforced the same way A2's approval restriction is. Two files' identically named
statics stay two symbols. Two definitions of one external name stay a conflict.
An impact result is a set of graph paths with the path shown, never a prediction.

`evidence` is untouched: A3 writes none, and `atlas_db_evidence_insert` still
refuses everything but `SOURCE` and `GIT`. Full detail and the explicit
non-claims are in [code-intelligence.md](code-intelligence.md).

## A4 — decision documents, immutable history and operator approval (done)

Give Atlas something honest to say when asked "why", now that it can say what a
thing *is*.

Delivered, in the same binary:

- **decision documents with immutable revisions**: bounded structured content —
  context, decision, rationale, alternatives, consequences, scope — each
  revision identified by a domain-separated, length-prefixed canonical content
  hash. A revision is never edited; a change is a new revision.
- **an append-only lifecycle ledger** over a closed four-state vocabulary:
  `PROPOSED`, `APPROVED`, `REJECTED`, `SUPERSEDED`. The ledger is canonical and
  the status columns are a cache of it that `atlas doctor` checks by replay and
  never repairs.
- **durable links** to paths, commits, change sets, symbols and other decisions,
  with currency computed on read — `CURRENT`, `CHANGED`, `MISSING`, `AMBIGUOUS`,
  `UNKNOWN`. No link is a foreign key into A3's tables, so a structural rebuild
  or an analyzer upgrade preserves every decision exactly, and Atlas never
  re-points a renamed or ambiguous anchor.
- **the approval workflow A2 deliberately did not fake**: `atlas decision
  approve|reject|supersede` on a real terminal, gated by a short-lived
  single-use daemon capability bound to one repository, document, revision and
  content hash.
- migration 6, ten `decision.*` daemon methods, eleven CLI subcommands with
  human and JSON output, Markdown export to stdout, and four MCP tools with
  progressive disclosure.

**What A4 deliberately did not do**, against the original sketch above:

- It does **not** parse Markdown ADRs out of a repository or write documents
  into one. Atlas is read-only with respect to a registered worktree, and a
  decision document is Atlas' record rather than the project's file.
- It does **not** lift `CHECK(approved = 0)` on `ai_decisions`. That would have
  made an approval something that happens *to* a model's own row, in the table
  the model writes, distinguished from a proposal by one integer. A4 approval is
  a separate record about a separate object, so A2's statement stays literally
  true. Legacy proposals are preserved and explicitly promotable.
- It introduces **no** `DECISION` or `USER_STATEMENT` evidence and leaves
  `INFERENCE` unused. A4 writes no evidence at all: a decision carries its own
  actor vocabulary, and folding it into `evidence` would make "how does Atlas
  know this?" and "what did somebody decide?" one question.
- Natural-language subsystem summaries are not built.

The rule that governed this phase: **Atlas states only what it can support.**
`LOCAL_OPERATOR_CONFIRMED` names a channel — a terminal, a single-use
capability, a confirmation typed against a content hash — and explicitly not a
person, not a presence, and not a signature. A same-UID process that can drive a
pseudo-terminal, including an AI agent with shell access, may imitate it; the
test suite demonstrates that rather than hiding it. What Atlas does guarantee is
about its own surface: no approval capability through MCP, hooks or any
AI-facing method, and no lifecycle change from conversation text.

Approval makes a document accepted project policy; it does not make its prose
trustworthy, and no decision text enters automatic model context at any status.

The rule that survives from A0: an unlinked commit message is still not a reason,
and neither is a graph edge. A decision has to be recorded to be reported as one.

Full detail and the explicit non-claims are in
[decision-lifecycle.md](decision-lifecycle.md).

## A5 — operational durability (shipped)

A5 was renumbered, and the reason is worth recording. The slot originally held
"clangd and toolchain truth". By the end of A4 Atlas held something no
repository could rebuild — decision documents, immutable revisions, a lifecycle
ledger, AI reasons and their attribution — and had no way to back any of it up
that was not "copy `atlas.db` and hope the daemon was idle". Making the
structural graph more precise before making the record survivable would have
been improving the part that was already reconstructible.

What shipped:

- `atlas backup create|verify|restore` — an online snapshot through SQLite's
  backup API from a read-only connection, so a running daemon keeps writing;
  verification that creates nothing and rehashes every decision revision; and an
  atomic restore that keeps what it displaced and leaves the original
  byte-identical through every failure.
- `atlas maintenance plan|prune` — every table classified with a written reason,
  exactly one of them prunable, no background deleter.
- No RPC method, no MCP tool and no hook for any of it. A model cannot back up,
  restore or prune the index, and the absence is structural.
- No schema change. A5 stays at schema 6.

Full detail and the explicit non-claims are in [operations.md](operations.md).

## Future: clangd and toolchain truth

Deferred from A5 rather than dropped:

- integrate `clangd` as a subprocess through the existing safe process API, with
  the same argv allowlist treatment Git gets
- upgrade the relations A3 resolves lexically to compiler-proven ones where
  clangd can supply them, keeping the resolution class honest about which is
  which
- report toolchain and compile-database drift in `atlas doctor`

## A6 — impact gates and stale-document detection

- flag decision documents whose subject has changed since the decision was
  recorded
- an exit-code contract usable as a CI gate, so a stale document can fail a build

## A7 — optional MCP adapter (absorbed into A2)

A7 was conditional: an MCP adapter only if a skill driving the CLI turned out to
be insufficient. It turned out to be insufficient before it was written, for a
reason worth recording — a skill driving a CLI cannot *participate* in a session.
It has no way to notice that a tool ran, no way to correlate what the index
observed with who was in a position to cause it, and no way to record an
`UNKNOWN` for something nobody explained. All of that needs hooks, and once the
hooks exist the query surface may as well be MCP, which the same daemon already
supports for free.

So MCP shipped in A2, and it obeys the constraint A7 set for it: it reuses the
service layer through the existing IPC methods and contains no query logic of its
own. Nothing is deferred here.

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

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

## A6 — impact gates and stale-decision detection (shipped)

Both of the original bullets, and the parts that turned out to matter more than
either of them.

- **Freshness per approved revision**: `FRESH`, `STALE`, `IMPACTED`, `UNKNOWN`,
  with a closed vocabulary of stable reason codes. Deterministic, from stored
  Atlas facts and stored Git facts; no model, and nothing cached.
- **An exit-code contract usable as a CI gate**: `0` for `PASS`, `8` for
  `REVIEW_REQUIRED`, `9` for `BLOCKED`, extending rather than changing `0`–`7`.
- **`UNKNOWN` fails closed.** This is the part the original two bullets did not
  anticipate and the part the phase mostly consists of: a gate that answered
  `PASS` on incomplete information would be worse than no gate, because the
  answer has the same shape as a real one. Index lag, an unreachable base, a
  rewritten history, a truncated walk and inconsistent stored state are all
  `BLOCKED`.
- **`STALE` means a human has to look again, not that the decision is wrong.**
  Atlas observed that the anchors moved. Whether an architectural decision
  survives a change to the code it concerns is a question about intent.
- **Human revalidation**, reusing A4's terminal-only single-use capability
  unchanged and adding two bindings — the indexed commit and an evidence digest
  — so commit drift and evidence drift are refusals. Append-only; it edits no
  approved revision and changes no lifecycle state.
- **Nothing a model can reach may change any of it.** One read-only MCP tool
  over one read-only RPC method, and no operation anywhere that clears,
  overrides or caches a freshness result.
- Schema 7: `decision_validations` added, `decision_challenges` rebuilt.

Deliberately **not** in A6: any orchestration. A6 provides a reusable gate
evaluator for a future control plane and implements none of it.

Full contract: `docs/impact-gates.md`.

## Deferred, and still deferred after A6

None of these was started in A6 and none is claimed. They are listed here rather
than only in a phase document because the roadmap is where a reader looks for
what has not happened yet.

- ~~**A full dedicated security review.**~~ **Done — see the A7 section below.**
- **clangd and toolchain truth** (see the section above; deferred from A5,
  unchanged by A6 and by A7).
- **The Atlas orchestration / control plane.**
- **The Claude dispatcher.**
- **The GitHub issue / PR / review loop.**
- **Model routing.**
- **Testnet 2 automation.**

## A7 — dedicated security review and trust-boundary hardening (shipped)

The review deferred from every previous phase, done as its own phase rather than
as a section of one. `docs/security/A7_THREAT_MODEL.md` states who Atlas defends
against and what it does not defend; `docs/security/A7_SECURITY_REVIEW.md`
records every finding with its reproduction, fix, regression test and residual
risk.

What it found and fixed:

- **One CRITICAL.** `decision.challenge` was an ordinary RPC method, so any
  process able to open the socket could mint the capability that
  `decision.approve` spends — with no terminal — and the resulting record said
  `LOCAL_OPERATOR_CONFIRMED`. The five operator-channel methods are deleted from
  the protocol, not left refusing.
- **Two HIGH.** `repo.ensure` let a session hook or an MCP root grant register
  any directory as a trusted repository, and `repo.add`/`repo.remove` were
  ordinary RPC methods. All three are gone; registration is a local operation
  under the write lock.
- **One MEDIUM, resolved by design change.** Terminal presence was being treated
  as operator authority. It is not, and A4's own test suite had been
  demonstrating that since A4 by typing into a pseudo-terminal it allocated.
  Authority is now a configured OS fact — a root-anchored policy naming an
  `operator_uid`, plus a root-owned executable — and where that does not exist
  the profile is locked and the lifecycle verbs refuse.

What it deliberately did **not** do: guard backup, restore, prune and
registration behind the same lock. Against a process running as the uid that owns
the data directory, `cp`, `mv`, `rm` and `sqlite3` reach the same bytes, so such
a guard reads as protection in a review and provides none — while breaking the
ordinary single-user install. The reasoning is in `atlas/authority.h` and the
review; a future phase that wants to widen the guard has to answer it.

A7 added no orchestration, no dispatcher, no work queue and no model routing.

## A7.1 — OS authority separation (shipped)

A7 ended with an honest gap: the lock it added protected the Atlas-mediated
route and nothing else, because one uid owned the daemon, the database, the
binary and the shell. A7.1 closes that with the operating system rather than
with more C.

- `atlasd` — a no-login system account that runs the daemon and solely owns
  `/var/lib/atlas` (0700) and `/var/backups/atlas` (0700).
- `atlas-worker` — the untrusted account every persistent model process runs as,
  and the principal A7.1 defends against. It cannot read the index or the
  backups, replace the root-owned binary or policies, or stop the service.
- `atlas-clients` — a group whose only privilege is opening the socket.
- A root-owned `/etc/atlas/system.conf` that decides the socket, the index and
  the client allowlist; anything missing, malformed, symlinked or non-root-owned
  leaves Atlas in per-user mode.
- Cross-uid service with `SO_PEERCRED` checked against that allowlist before a
  byte is read, and the A7 model-safe method surface unchanged behind it.
- A copy-migrate-switch cutover to schema 7, leaving the original schema-6
  per-user database byte-identical as a rollback target.

**The operator account and root remain trusted by design** and are explicitly outside the
isolation guarantee, at the operator's decision. A7.1 defends against
`atlas-worker`; see `docs/security/A7_1_THREAT_MODEL.md`.

A7.1 added no orchestration.

## A9.2.5 — semantic index trust closure (CLOSED)

The sentence the season exists for:

> **A SEMANTIC READ THAT FOUND NOTHING HAS NOT ESTABLISHED THAT THERE IS
> NOTHING.**

A9.2.2 proved that for claims and built the coverage model an absence rests on;
A9.2.3 gave a generation a coverage manifest; A9.2.4 gave it a discovery verdict.
None of it reached the answer to `callers of X`, which replied with its rows plus
freshness and stopped — so `zero rows` and `zero rows over a tree Atlas read a
third of` were the same document. This repository answered exactly that,
`CURRENT` and `0 reached`, with a PROVEN caller in a file the compilation
database never named.

Shipped:

- `result_verdict` — PRESENT, ABSENT or UNKNOWN — plus twenty trust fields on
  every load-bearing semantic read, from one `atlas_sem_trust_write_json` that
  the CLI renderer and the IPC server both call. UNKNOWN is the enum's zero and
  never means "no".
- A9.2.2's asymmetry one layer out: one row settles PRESENT whatever the
  coverage; zero rows settle ABSENT only over a universe Atlas can vouch for.
- `sem_discovery_obstacles` (migration 20): every place a walk could not look,
  with its exact `%XX`-encoded path and a fixed reason, sorted and deduplicated.
  A9.2.4 kept the first reason and no path, so one `--exclude` masked the rest.
- A FAILED translation unit is never carried forward as COMPLETE, and a
  transient parse failure gets one bounded retry per unit and a bounded total
  per pass.
- `repo_identity_hash` is compared on the read path; `INCOMPLETE` is no longer
  held with `HOLD_CURRENT`; the context package states its coverage gaps.
- A symbol that is not in the index is no longer a usage error. Ambiguity is.

Deliberately not done: Atlas still does not guess which sources are tests, and
declaring one test root is not evidence that every test root was declared, so
`ATLAS_COVDIM_TESTS` stays UNKNOWN and a production-scope absence stays
UNAVAILABLE.

## A9.2.6 — daemon responsiveness (CLOSED)

The sentence the season exists for:

> **THE DEADLINE WAS NEVER THE BOUND; THE SHORT JOB WAS.**

Carried the one finding A9.2.5 established and deliberately did not fix: a
semantic index pass could leave the daemon answering nothing. The cause was
recorded then as **SUPPORTED BUT INCOMPLETE** — the WAL explanation measured and
disproven, the serial serve loop established, and *which* request held it
explicitly not established. This season established it, and the answer was not
the one the earlier evidence pointed at.

Reproduced under control, then sampled rather than reasoned about. The serve loop
in `src/ipc/server.c` dispatches each request synchronously, so it was sitting in

```
atlas_server_serve -> atlas_server_dispatch -> method_session_open
                   -> atlas_writer_ai -> pthread_cond_timedwait
```

while the writer thread sat in `atlas_sem_index_on -> atlas_sem_parse_unit`. Not
a SQLite lock at all: a synchronous writer call, waiting out its whole timeout
for a job queued behind a minutes-long pass on the single writer thread. Measured
on this repository: `daemon ping` 26 ms idle, **3.9 s for every write that arrived
during a pass**, with the write itself failing after 4027 ms. Claude Code fires a
hook on every event and every hook opens a session, so on any repository with
automatic semantic maintenance this was the ordinary case rather than a corner.

Every synchronous writer call already waited with a timeout, and the comment
saying why has always claimed the timeout is what stops one slow mutation
stalling every other client. That held only while every job on the queue was a
handful of statements. A9.2.4 put an automatic, minutes-long pass on the same
thread and the same FIFO, and the premise stopped holding without a line of the
waiting code changing.

The fix is that a waiter can now ask what the writer is doing.
`job_kind_is_unbounded` is the question, asked of the job *kind* rather than of
elapsed time — elapsed time cannot tell a job nearly finished from one barely
started, and guessing wrong that way abandons a write about to succeed. Two kinds
answer yes: the pass that runs a compiler, and the walk that looks for build
descriptions. `writer_wait_locked` is the one implementation of "a caller waits
for the writer", replacing nine identical copies, and it waits in slices so the
condition can be re-asked. A job still queued is taken back out and the caller is
told `BUSY:` — **nothing was queued and nothing will run**, which is what makes
retrying safe and is the claim the existing timeout deliberately does not make.

No schema change, no migration, no new thread, no new queue, no exit code added,
and nothing in A9.2.5's PRESENT/ABSENT/UNKNOWN rules moved: this season is about
when a caller stops waiting, not about what any answer means. Ordering did not
move either — `queue_remove` excises one never-started job and shifts the rest up
by one, because the orchestration ledger and the decision lifecycle both depend on
writes applying in the order they were accepted.

Deliberately not done, and written down rather than discovered later:

- **Reconciliation is not treated as unbounded.** An incremental pass finishes
  well inside these timeouts, and a hook write refused during one would be
  *dropped* rather than delayed, because hooks fail open. A first full pass over a
  large tree can therefore still hold the loop up to a caller's timeout, and a
  sanitiser build makes that visible.
- **Snapshot and maintenance are not treated as unbounded either**, for the same
  reason and with the same residual.
- **Writes are refused, not deferred, for the duration of a pass.** That is the
  trade the season makes: a fast, retryable refusal in place of a stalled daemon.
  **A9.2.7 replaced this trade** — the pass yields and the latency-critical
  writes are served during it — leaving a much smaller residual, one translation
  unit's parse. The sentence is left as written because the trade it describes
  was real and what it cost is why the next section exists.

`docs/engineering-rules.md` carries the rules and the full argument;
`docs/daemon-and-ipc.md` carries the behaviour; `docs/backlog.md` carries the
original incident with this resolution appended to it.

## A9.2.7 — the writer yields (shipped)

The sentence the season exists for:

> **A REFUSAL A CALLER HAD TO KEEP REPEATING WAS AN ANSWER, NOT A WRITE.**

A9.2.6 stopped one slow write taking every client with it, and left the write
itself refused for the whole of a pass. That was honest and it was measured to be
expensive. `docs/backlog.md` records the bill: Atlas' own recovery sweep failing
every twenty seconds for a whole pilot window; a submission needing sixteen
attempts across forty-seven seconds to land; and an A10.1 experiment arm losing a
finished worker's completion outright, so that a correctly edited tree was
evidence of work Atlas had no record of. The backlog also names the fix, in the
A9.2.4 entry, in the words this season implements: *the pass already chunks its
work and already commits per batch; what it does not do is let another writer job
run between chunks.*

**What was added.**

- **Yield points, at the places where nothing is open.** `atlas_sem_index_opts`
  gains `yield`/`yield_ud` beside the existing `cancel` pair, polled between
  translation units, once before the generation is opened, and once after the
  unit loop ends. The discovery walk gains the same pair, polled every
  `ATLAS_SEM_DISCOVER_YIELD_EVERY` directory entries. It crosses layers as a bare
  function pointer and a `void *`, so nothing in `src/sem` or `src/core` names a
  daemon type.
- **A drain.** `writer_yield` scans the queue front to back, takes the first
  drain-eligible job, runs it, and repeats until none is left. It records nothing
  durable: this is scheduling, not state.
- **`job_kind_is_drainable`**, an explicit function with no `default:` and one
  reason per exclusion. At this season's ship, `true` for `ORCH`, `AI`,
  `DECISION`, `VERIFY`, `GW_AUDIT`, `APIKEY` — the latency-critical writes
  whose tables are disjoint from anything a pass or a walk touches. Later
  seasons added to this set on the same two conditions — A12.0's `PLAN`,
  A12.1's `MEMORY` — which is exactly why this list is a snapshot of what
  shipped here, never a substitute for reading `job_kind_is_drainable`
  (`src/daemon/writer.c`) itself for the current membership.
- **A grace.** `ATLAS_WRITER_YIELD_GRACE_MS` (2000). A waiter that observes an
  unbounded job gives it that long to reach a yield point before backing out, and
  the grace is measured from the waiter's *first observation*, never from queue
  time.
- **One shared job-execution helper.** `writer_run_job` is the single
  implementation of claim → run → complete → settle ownership, used by the main
  loop and by the drain. A second copy of the three-exit ownership contract is
  the defect this file's history predicts.

**What deliberately was not done.**

- **No second writer, and A1's rule did not move.** There is still exactly one
  writer thread, one writable handle, one process writing the index. The
  backlog's second shape stays open.
- **The serve loop is untouched.** `src/ipc/server.c` still dispatches one
  request at a time; that serialism is design, and this season removes the reason
  it hurt rather than the design.
- **`job_kind_is_unbounded` still answers the same two kinds**, and
  reconciliation is still `false` for A9.2.6's reason: a hook write refused
  during an incremental pass would be *dropped*, and that is the worse failure.
- **No yield inside a translation unit.** The per-unit transaction deliberately
  spans the parse child, and this season does not touch the pass's transaction
  structure — per-unit transaction, `batch_checkpoint` cadence, sealing,
  publication, all unchanged.
- **No schema change, no migration, no new thread, no timer, no RPC method, no
  MCP tool, no gateway route**, and `WRITER_BUSY_MSG` is the same sentence it
  was, because it is still exactly true.

**The residual, stated rather than discovered.** A single translation unit that
parses for up to `ATLAS_SEM_PARSE_TIMEOUT_MS` is a stretch with no yield in it. A
write that arrives inside one is refused at grace expiry exactly as every write
was refused before this change — and the refusal costs about two seconds now
rather than about a tenth of one, which is the price of the grace and is paid
only there. Every synchronous deadline on the writer path is at least 4000 ms, so
the back-out still precedes the timeout and the two answers stay distinguishable.

**Before, measured.** The figures above are `docs/backlog.md`'s, from the A11.5a
pilot and the A10.1 experiment: a sweep refused every twenty seconds, a
submission that took sixteen attempts and forty-seven seconds, and one lost
completion. **After: not yet measured on a production workload.** The suite shows
a verification write landing during a live pass over a 160-unit fixture without a
`BUSY` token, which is the property rather than a figure; the operational
before/after belongs to a smoke run on a real repository and is not filled in
here from anything less.

## O10 — production evidence ingestion (CLOSED)

The sentence the season exists for:

> **THE SURFACE WAS ALREADY THERE; NOBODY HAD PROVED A CLIENT COULD RELY ON IT.**

O10 was named as next in the A9.2.5 closure notes and its scope was deliberately
left unwritten. Written down, it turned out to be a milestone whose central
demand had already been met — and saying so plainly is the whole value of the
section, because a milestone that quietly re-ships what exists is how a roadmap
stops describing the system.

**What was already delivered, and by which season.** Production ingestion —
"real agent and MCP work can write claims, evidence and attestations into the
verification ledger, and the test-only insert functions are on a production path"
— is exactly the gap A9.2.1 closed. `tests/test_verify_intake.c` states it in its
own header: A9.2's three insert functions had no caller outside the tests, so on
a real deployment the ten verification tables stayed empty while the engine that
reads them passed everything it had. The intake write point, the nine RPC
methods, the eight MCP tools, the channel derivation and every refusal that hangs
off it shipped then. So did the parts O10's scope names one by one:

| The requirement | Where it already lived |
| --- | --- |
| a bounded authenticated submit surface | `src/ipc/server_verify.c`, `TOOLS[]` in `src/mcp/mcp_tools.c` |
| the actor derived from the channel, never the payload | `channel_for` and `atlas_verify_channel_actor_class` |
| a model that cannot become HUMAN, OPERATOR, TOOL, COMPILER, TEST or RUNTIME | `atlas_verify_channel_parse` and `atlas_verify_evidence_class_requires_atlas_production` |
| everything a model sends recorded as a model's | `ATLAS_ACTOR_AI_AGENT` with `SELF_DECLARED` identity; `AI_ANALYSIS` for what it read |
| real tool evidence only from a verifier Atlas ran | `verify.evidence_produce` and `src/verify/detverify.c` |
| idempotent submission | the §27 content keys in `src/verify/intake.c` |
| an explicit retryable refusal rather than a silent drop | A9.2.6's `writer_wait_locked`, which `ATLAS_JOB_VERIFY` already used |
| no authority acquired by submitting | the absent verbs, and `VERIFICATION_POLICY` |
| provenance carrying repo, actor, session, run and time | `atlas_verify_op`'s `session_key`, `run_id` and the actor fields |

**What O10 added.** Three properties a production client depends on that no test
asserted, all three at the boundary a client actually reaches rather than at the
write point below it:

1. **A repeated submission through the transport makes one row.**
   `tests/test_verify_intake.c` proved the write point resolves a repeat to the
   row it already made; that is the rule, and it is not the same statement as
   "the surface a retrying client uses behaves that way". The reply says
   `duplicate` rather than staying silent, because a client that cannot tell a
   fresh row from a resolved one has to guess, and it guesses wrong exactly when
   a confidence score moved.
2. **A recorded claim survives a daemon restart.** Invariant 1 — SQLite is a
   rebuildable index and never the canonical record — is right about files and
   commits and is *not* right about this: a claim, its evidence and its
   attestations exist nowhere else and nothing rebuilds them. So "accepted" has
   to mean committed and rediscoverable by a daemon that did not accept it, read
   back through MCP rather than out of the file, because surviving and being
   findable again are two claims.
3. **A submission refused while the daemon is busy wrote nothing.** A9.2.6 made a
   caller stop waiting and did not say what became of the record it was trying to
   make. For a hook that has a written answer — hooks fail open and the metadata
   is lost on purpose. A verification submission is not metadata. The test checks
   the read surface *at the instant of the refusal*, because totalling the rows
   at the end cannot discriminate: a refusal that silently stored the row would
   still total one, the retry having resolved to it by content key.

Two things were learned by writing them, and both are recorded rather than
smoothed over. `verify.show` lists the evidence an attestation *relied on*, so a
free-standing evidence row is stored and not yet shown — a row nobody cited has
not yet borne on the claim. And a model's SUPPORT attestation moves the claim to
`SUPPORTED` on the **verification** axis while the lifecycle status stays
`PROPOSED`; asserting `UNVERIFIED` there would have been asserting the wrong
thing, and A9.2's orthogonality rule is what says which axis carries authority.

No schema change, no migration, no new RPC method, no new MCP tool, no new job
kind, no exit code, and not one line of `src/` — the season is three tests, and
that it is three tests is the finding.

Deliberately not done:

- **No second submit surface.** A parallel path would have bypassed
  `atlas_verify_intake_apply_in_tx`, and the checks there are exactly the ones a
  forger would want somewhere else.
- **No caller-supplied submission id.** The content key is stronger where it
  matters — the actor is folded into the evidence and attestation keys, and the
  claim key deliberately omits it, so two actors stating one proposition state
  one claim and the second attests rather than forks.
- **The residuals A9.2.6 wrote down are unchanged.** A submission arriving during
  a reconciliation, snapshot or maintenance job can still wait out its caller's
  timeout.

## A11.0 — the durable single-worker run (CLOSED)

The sentence the season exists for:

> **A CHAIN OF TASKS WAS EXPRESSIBLE AND NOT ENFORCEABLE.**

A11.0 was named as next in the O10 closure notes with its scope deliberately
unwritten. Written down, it turned out to be a milestone whose task half already
existed and whose run half did not exist at all — and the interesting part is
exactly where the line fell, because it was not where the field names suggested.

**What was already delivered, and by which season.** A8's control plane already
held most of the state A11.0 was asked to establish, and it holds it durably:

| The requirement | Where it already lived |
| --- | --- |
| `task_id` | `orch_jobs.job_uid` — random, unguessable, unique |
| `attempt_number` | `orch_attempts.attempt_no`, `UNIQUE(job_id, attempt_no)`, monotonic per task |
| `status` | the eleven-state machine, `atlas_orch_transition_allowed`, and the `orch_transitions` ledger |
| the repository and source a task is bound to | `repo_identity_hash`, `repo_name` and a pinned `source_commit` — never a branch |
| duplicate and retry collapsing to one task | `orch_idempotency`, keyed per submitter, checked inside the submit transaction |
| a model payload producing no authority | A8's design: the submitter is `SO_PEERCRED`, every reason is a closed vocabulary, and a completed job approves, applies and commits nothing |
| a refusal that does not become a silent loss | A9.2.6's `BUSY:`, which says nothing was queued and nothing will run |

None of that was rebuilt, and no vocabulary was invented beside one that already
worked. `parent_task_id` maps onto a column A8 already had.

**What A11.0 added.** The column A8 had was `parent_job_uid`, and it was checked
for *shape* at submission — `'j'` plus 32 lowercase hex — and for nothing else.
Nothing asked whether the parent existed. A submission naming
`j00000000000000000000000000000000` as its parent was accepted and stored, and
every later reader of that chain would have been wrong about it. The gap was not
a missing field; it was a field nobody resolved.

So A11.0 is the resolution, and the run it needs to be resolvable against:

1. **The run.** `orch_runs` — a run uid, the one task it was created for, the
   repository identity it is bound to, and a status. A root task creates its run;
   a child inherits its parent's. The run identity is *derived*, never supplied:
   it is not a field of `atlas_orch_spec`, so `ATLAS_ORCH_SPEC_DOMAIN` did not
   move and no stored `spec_digest` means anything different than it did.
2. **Four refusals at submission**, all inside the transaction that inserts the
   job, because a check that a run is still ACTIVE is worthless if a second
   submission can land between the check and the insert: the parent must exist,
   it must describe the same repository, its run must not be terminal, and the
   run must have no other active task.
3. **One active task per run, in the schema.** A partial unique index on
   `orch_jobs(run_uid)` over the non-terminal states, following `M8_LEASES`'
   precedent for "at most one unreleased lease per job". This is not belt and
   braces: with the C check disabled the submission is still refused, by the
   constraint. The check exists so the caller gets a sentence naming the task in
   the way instead of a constraint violation it cannot act on.

**What A11.0 deliberately did not build.** No worker is started, no driver runs,
no follow-up task is generated, and nothing settles a run automatically.

The run's status is **its own axis and is derived from nothing**. A task ending
SUCCEEDED does not accept its run and a task ending FAILED does not block one,
because "this attempt finished" and "this line of work is settled" are different
claims — the separation A9.2 keeps between a verification state and a lifecycle
status, one layer out. `atlas_db_orch_run_set_status` exists so that a caller
which has decided can record the decision; **A11.0 calls it from no automatic
path**, and there is no RPC method, no MCP tool and no gateway route that reaches
it. That is what makes "a model payload cannot accept a run" true by absence
rather than by a check, which is the house form of the claim.

**Residuals, written down rather than left to be discovered.**

- **`ATLAS_ORCH_RUN_ACCEPTED` and `ATLAS_ORCH_RUN_BLOCKED` have no producer in
  production code.** They exist, the schema stores them, the submit path refuses
  a child against them, and only a test moves a run into one. Who may settle a
  run is A11.1's question and answering it here would have been inventing it.
- **No job that existed before migration 21 belongs to a run**, and none was
  backfilled. `run_uid` is empty for every one of them, which reads as "this job
  belongs to no run" and never as "this job is the root of its own run". Such a
  job is refused as a parent, with a message saying so.
- **The run is per single worker, as the name says.** Nothing here makes two
  tasks in one run safe to run at once; the index makes it impossible instead.

## A11.1–A11.4 — the single-worker orchestrator loop (CLOSED)

A11.0 left a resolved chain and two statuses nothing produced. A11.1 through
A11.4 were the milestone that carries it, and they closed together because they
are one loop: there is no useful state in which Atlas can start a worker but not
gate it, or gate it but not answer with a follow-up, or produce follow-ups
without a bound that ends them.

The sentence the milestone exists for is

> **A RESOLVED CHAIN THAT NOBODY COULD CARRY WAS STILL A DESCRIPTION OF WORK,
> NOT WORK.**

**What was already there.** Almost all of the machinery. A8 shipped the job
model, the canonical digest, the persisted state machine, the one write point,
leases with a partial unique index, heartbeats, retry, cancellation, crash
recovery, the root-owned policy, the two RPC groups, workspace provisioning,
source snapshotting, bounded command execution with no shell anywhere, the driver
interface, a Claude Code driver and a deterministic fake beside it, and the
validation runner. A8.1 added the operator's own model dispatcher. A11.0 added
the run, the resolved parent chain, and one active task per run. A9.2.6 added the
`BUSY` refusal that says, in the message, that nothing was queued.

**What A11.1 added**, and it is deliberately small against that list:

- **A foreground run driver** (`src/orch/rundriver.c`), started by an operator
  and by nothing else. It claims the run's active task *by name*, checks the
  pinned commit before starting anything, records RUNNING before it execs,
  starts one worker, checks the pinned commit again, runs the gates, and
  reports. It polls no queue, schedules nothing and leaves nothing running.
- **Two drivers that work in the registered repository's own tree**,
  `claude-repo` and `fake-repo`, sharing A8's implementation and differing only
  in where the child runs and where its log goes.
- **Run settlement, inside `atlas_orch_apply_in_tx`**, derived from the state the
  task machine reached and from the ledger's count of worker starts.
- **One follow-up task per failed gate**, created through the same submit path
  with a deterministic idempotency key.
- **A bound of three worker starts per run**, counted in the ledger rather than
  stored.
- **`atlas job run`, `atlas job run --resume` and `atlas job run-status`** — the
  whole new command surface, all of it under the existing `job` family.

**No migration.** Schema stays at 21. Everything the milestone needed was
already a column: the gates are `orch_jobs.validations`, the chain is
`run_uid` and `parent_job_uid`, the evidence is `orch_artifacts`, and the budget
is `orch_transitions`. A season that can be built without a migration should be.

**Who may settle a run — A11.0's open question, answered.** The trusted
in-process driver, and only through a completion. There is no `job.run_settle`,
no MCP tool and no gateway route; `atlas_db_orch_run_set_status` still has no
caller outside `src/db/db_orch.c`. The completion carries an exit classification
Atlas computed and a gate verdict Atlas ran — never a claim the worker made, and
there is no field on the wire through which one could arrive. A worker that exits
zero and fails its gate ends a task and does not accept a run.

**Invariant 5 is narrowed, precisely, and only here.** "No Atlas command modifies
a target repository" was unqualified and is no longer true without a
clause. Every Atlas *read* — `scan`, the index passes, the watcher, every
`src/git` invocation — is still read-only and writes nothing. What changed is
that an operator running `atlas job run` — or, since A12.0, `atlas plan run`,
which starts its workers through exactly the same machinery — may start a child
process whose purpose is to edit the tree, in a directory Atlas resolved from its
own registry, under a driver the root-owned policy named. Three things must line up and removing any
one stops it. The working tree is expected to be dirty afterwards: it is the
first worker's output and the second worker's input, and nothing on any path
cleans, resets, checks out, stashes or reverts it.

**Residuals, written down rather than left to be discovered.**

- **Nothing is enabled by installing the binary.** `/etc/atlas/orchestration.conf`
  on this machine lists `driver = fake` and `driver = claude`, and A11.1 did not
  add `claude-repo` to it. Until an operator does, `atlas job run` against the
  real daemon is refused with "that driver is not configured". That is the first
  step of the A11.5a pilot and it is deliberately not taken here.
- **"Do not commit, do not push" is instruction, not a control.** What Atlas
  enforces is that a run whose HEAD moved off its pinned commit is refused rather
  than judged, and is never accepted. Everything else in the constraint list the
  worker is given is a sentence to a model.
- **The HEAD-moved-after-the-worker path is proved only for the before case.**
  `fake-repo` cannot commit, so the second check is exercised by inspection and
  by the first check's test rather than by its own.
- **A run driver that is killed mid-attempt leaves the task claimed** until the
  daemon's recovery timer frees it, which is about one lease. That is honest —
  the attempt genuinely is of unknown fate — and a resume before then reports
  `busy` rather than starting a second worker.
- **Under `operator_session` the worker runs as the operator**, which is A8.1's
  documented cost and is unchanged. Atlas does not claim OS isolation between the
  run driver and the model it starts; what it claims is that no *output* of that
  model reaches a decision.

## A11.5a — Atlas-on-Atlas pilot, cross-run memory off (CLOSED)

The first real pilot: one operator-supervised run against this repository, with
`claude-repo` enabled in the root-owned policy and cross-run memory deliberately
**off**. A pilot rather than a feature, because what was unproven was not the
machinery — A11.1's fourteen acceptance cases are that — but whether a task text,
a gate list and a three-worker bound are enough structure for a real change to
land. That question could not be answered by a test, and the answer is yes.

**It took four runs, and the first three are the milestone's real content.**
Every one of them was blocked by a defect in Atlas rather than by the model, and
none of the three was visible to the suite that existed at the time.

1. **Three `SPAWN_FAILED` in one second.** The worker executable is resolved
   against a fixed `PATH`, deliberately, because the child's environment is
   constructed rather than inherited. On this machine `claude` lived in a home
   directory, so no worker could start and the run's whole budget was spent
   inside a second. Deployment prerequisite, not a code defect; recorded in
   `docs/backlog.md`.
2. **Gates never reached the daemon intact.** The sender encoded each one as the
   canonical single-command vector and the daemon wrapped it in a *second* count
   before decoding, so the sender's count was read as the argument count.
   `cmake --build build --target t -j 4` was stored as the one-element vector
   `["5:cmake"]`; a one-word gate did not decode at all. Since a repository-tree
   task must declare a gate and can only be accepted by passing one, **no run
   submitted through the CLI could ever reach ACCEPTED.** It survived because
   every A11 test builds its operation in process and none travelled the wire.
   Fixed, with the wire form given one reader and a suite that fails against the
   reintroduced wrapper.
3. **A live worker judged for a silence that was Atlas' own.** A heartbeat is an
   ordinary synchronous write and A9.2.6 refuses those for the whole of an
   unbounded semantic pass. Measured here: passes over a second registered
   repository ran 167–176 s with 14–20 s between them, so orchestration writes
   were refused about ninety per cent of the time and a sixty-second lease could
   not survive. Separately, a repository-tree attempt has no workspace, so a
   refused completion destroyed the only copy of the result — one attempt left
   `orch_events` empty for a worker that had run five minutes.
4. **A worker killed at 301 s while working.** `--output-format json` says
   nothing on stdout until it finishes, so an idle bound measured in bytes read
   the format as idleness. The ceiling was not raised: idle is now measured in
   recognised progress records, the worker runs under `--output-format
   stream-json`, and every record is appended to a durable per-attempt log.

**The run that closed it.** Run `r29b70dcfd7839a22ae36cba103a7dd25`, one worker
start of three, eleven minutes twelve seconds, 215 progress events, longest
genuine silence 173 s, both gates run by Atlas and passed, no follow-up, no
duplicate attempt, HEAD unmoved, nothing committed by the worker. What it
produced — `tests/test_a11_head_drift.c` — was reviewed before it was committed
and verified to fail, on nine assertions, when the branch it covers is disabled.

**What the pilot did not establish.** It is one task on one repository, and there
is no baseline arm, so nothing here says what Atlas' orchestration costs or saves
against a person or against an unorchestrated session. Token usage for the
accepted run had to be recovered from the worker's own session transcript,
because a worker log above the inline artifact ceiling is discarded once the
completion lands — recorded as a residual in `docs/backlog.md`, and the thing to
fix before any comparison is attempted.

A11.5a held constant what it said it would: no cross-run memory, no second model
role, one worker at a time, and nothing learned in a run became an input to a
later one.

## A10.0 — durable run usage telemetry (CLOSED)

A11.5a closed with a residual that made the milestone after it impossible: Atlas
could not report what a successful run cost. The worker log carrying the figures
is dropped above the inline artifact ceiling, the result spool holding a second
copy is cleared the moment a completion is accepted, and the numbers for the
accepted pilot survived only in the worker's own session transcript — a file that
exists because A8.1's model dispatcher borrows the operator's login and would not
exist for a worker running as `atlas-worker`.

**What was established before anything was written.** The canonical source is the
final `result` record of a `stream-json` run and nothing else: its `usage` block
is the attempt total already. A real twelve-turn run reported 7 275 output tokens
there while the per-message records in the same stream summed to 274, which
settles the double-counting question by measurement rather than by argument. The
record is present on failures too — an interrupted run still carried its turn
count, its cost and a full usage block — so a failed attempt can still say what
it spent.

**The three states are the milestone.** `UNKNOWN` is not zero: an attempt whose
usage was never observed did not cost nothing, and every count is stored
nullable so absence stays absence. `PARTIAL` is a record that arrived with
something missing or refused. Cost is provider-reported or nothing at all —
Atlas estimates no price from token counts, because an estimate that reads like a
measurement is worse than an absent one. A run total is derived on every read
with the denominator taken from the ledger, so a run whose worker never spawned
reads `UNKNOWN` rather than a tidy zero, and there is no field called
`total_cost` for a run one of whose attempts reported no price.

Migration 22 is additive and backfills nothing, deliberately. A11.5a's runs are
left without rows rather than reconstructed from a transcript: a baseline
assembled by hand is not one an experiment can compare against.

## A10.1 — bounded cross-run memory, and what measuring it established (CLOSED)

The question A10.0 made askable: does handing a worker a bounded summary of
earlier runs make it better, and by how much. **The verdict is `USEFUL`, and the
memory default stays `OFF` anyway.** The second half is not a hedge on the first:
this milestone's own contract says a proved benefit does not turn a default on,
and one measurement of two pairs is not what a default rests on.

### What was built

An operator names a mode at submission; Atlas selects at most three earlier
terminal runs by deterministic lexical overlap, renders them as at most twelve
kibibytes of labelled text, freezes that against the run in the transaction that
creates it, and appends it to the task once when a worker starts. No vector
store, no embedding, no summariser, no ranker; nothing in the selection calls a
model. `docs/orchestration.md` carries the contract and `docs/extending.md` the
checklists.

Three decisions are worth repeating here because each one was forced.

**The mode travels on `atlas_orch_op`, never on `atlas_orch_spec`.** Putting it
in the specification would move `ATLAS_ORCH_SPEC_DOMAIN` and every stored
`spec_digest` would silently mean something else.

**`repo_identity_hash` could not be the selection key.** It is A4's
*path-qualified* lineage fingerprint, so a repository and a linked worktree of it
have different ones — and an isolated experiment runs in worktrees. Memory got
its own value under its own domain, built from the object format and the sorted
ingested root commits. Nothing is authorised, admitted or refused on it; it
selects hints, and that is the only reason a second identity-shaped value is
tolerable at all.

**A run carrying a memory manifest is never a source.** Freezing at creation
keeps two arms created before either runs from seeing each other, and that was
the whole argument until the deadline arithmetic broke it: a task's wall
deadline is `created_ms + wall_timeout_ms`, so several pairs cannot all be
created up front, and a later pair is necessarily created after an earlier pair
has ended. The exclusion is one predicate with no identifiers in it. **Its cost
is stated rather than discovered later: bounded memory does not compound.** A run
that was shown memory does not become memory, so the corpus stays the runs that
predate the mechanism.

### The experiment

Base commit `65ad5a3`, one binary
(`3fcfa0d68aa7a6ef398bd841b6fcb8dfb28dfb7001a170bf2433bdc5a05df0b9`), one model,
fresh session per arm, three worker starts per run, isolated git worktrees of
this repository, gates fixed at submission and identical across arms. Both arms
of a pair were submitted before either was driven, so both manifests were frozen
against the same world. Every task was a small real one drawn from an uncovered
branch this repository actually has.

| | pair 1 — the gate allowlist | pair 2 — the validation bounds |
| --- | --- | --- |
| treatment run | `r527bd9caa91e41ef281328cf1985dd2d` | `rc9417e37925a3fe00313207c48c9287a` |
| control run | `rda50b67d74ae38df463a20545a6fb1fd` | `r0066c3faf5088d692ed3ae932f7f8ad1` |
| memory digest | `5512e516…` — 2 832 bytes, 3 sources | `5512e516…` — the same |
| verdict | ACCEPTED / ACCEPTED | ACCEPTED / ACCEPTED |

| metric | treatment | control | delta |
| --- | ---: | ---: | ---: |
| ACCEPTED runs | 2 | 2 | 0 |
| worker starts | 2 | 2 | 0 |
| follow-ups | 0 | 0 | 0 |
| provider cost | $10.253520 | $10.423908 | −1.6 % |
| worker duration | 870 301 ms | 1 085 233 ms | −19.8 % |
| turns | 51 | 55 | −7.3 % |
| output tokens | 33 113 | 37 203 | −11.0 % |
| cache read | 4 096 414 | 4 059 818 | +0.9 % |

Per pair: duration −25.4 % and −11.2 %, turns −5.9 % and −9.5 %, cost +0.8 % and
−4.5 %. Every arm's usage read `AVAILABLE`; none was `PARTIAL`. Neither treatment
output contains any trace of the package — no copied historical text, no stale
gate string, nothing steered wrong by it.

### Why the verdict is USEFUL, and exactly how far it reaches

The milestone's own criterion for `USEFUL` is met and it is met on the strict
reading: treatment accepted no fewer tasks than control (two and two), no
memory-caused wrong or stale steering appeared in either treatment output, and at
least one secondary metric moved by ten per cent or more at equal success —
**worker duration, −19.8 %**. The direction is the same in both pairs (−25.4 %
and −11.2 %) and turns move with it (−5.9 % and −9.5 %), which is the
"benefit in the same direction in more than one pair" the criterion asks for
where it can be had.

Three limits are recorded beside it, because a verdict whose limits are not
written down is one somebody will later quote without them.

**Pair 2's arms did not run under equal machine conditions.** Between its control
arm and its treatment arm, nine experiment worktrees were unregistered to stop
build-input discovery starving orchestration writes. Worker duration is exactly
the metric machine load contaminates, and the intervention was in the treatment
arm's favour. Pair 1 ran with both arms under identical conditions and carries
the larger effect (−25.4 %) on its own, which is why the verdict does not rest on
pair 2 — but pair 2's −11.2 % is corroboration, not independent evidence.

**Cost, the metric nobody can confound, moved 1.6 %.** Treatment was cheaper, and
by far less than the ten per cent the milestone set as its own threshold. So the
claim this verdict supports is about *how long a worker takes and how many turns
it uses*, and it is **not** a claim that memory makes a run cheaper. Anyone
reading "USEFUL" as "it saves money" is reading something this experiment did not
measure.

**Two pairs is a small number and this milestone said so in advance.** The
instruction it set for itself was to read two pairs conservatively. The verdict
here is the criterion applied as written; it is not a claim of statistical
certainty and must not be presented as one. Three pairs were frozen and two were
run — task 3 was dropped by an explicit scope decision, not because of what the
first two showed.

### What the calibration cost, and what it bought

Four arms were run and discarded, and **all four failures were the measurement
harness, not Atlas and not the model**. They are recorded because a milestone
that reports only its successful runs is reporting a filtered sample.

1. **A wall deadline consumed by the arm before it.** Both arms of a pair were
   submitted together to freeze both manifests, and `deadline_ms` is computed
   from *submission*. The second arm to run had already spent seven of its
   fifteen minutes waiting. Fixed by raising the policy's wall ceiling so a pair
   fits inside one budget.
2. **The harness killed the run driver.** A driver that dies stops heartbeating,
   the lease expires, and Atlas correctly reclaims an attempt whose worker was
   alive and mid-edit. Fixed with `setsid`.
3. **A `BUSY`-retry wrapper applied to `job run`.** That wrapper is right for
   `job submit`, which an idempotency key resolves to one request. Re-invoking
   `job run --resume` is not the same command twice; it is a second run driver
   against a live attempt. Five re-invocations later the `dispatch.lease` call
   timed out.
4. **Build-input discovery over thirteen repositories.** Registering seven
   worktrees turned A11.5a's fourth finding — "one repository's churn starves
   orchestration everywhere" — into near-continuous unbounded writer jobs, and
   an arm lost its completion to it. This is the one that is *not* only a harness
   defect: the scaling property is real, it is in `docs/backlog.md`, and nothing
   in Atlas bounds it.

What they bought is worth stating. The **third** of them — a `fake-repo` dry run
costing nothing — found a genuine production defect before a single token was
spent on it: `atlas_writer_orch` copies the writer thread's result into the
caller's field by field, by hand, and the memory fields were not on the list. The
manifest was frozen correctly, the lease read it inside its own transaction, the
daemon was ready to emit it, and **the worker was handed nothing**. Every
in-process test passed, because every one of them applies the operation directly
and none crosses that boundary. A11.5a lost most of a milestone to the same shape
of defect in the gate encoding. The lesson is A11.5a's, unlearned once and now
paid for twice: *a rule proved at the write point is not a property at the
boundary a client reaches.*

### What this did not establish

One repository, one model, two tasks of one shape — "add a targeted regression
test" — and a memory corpus of three runs that happened to be about a single
subject. Both treatment arms received a byte-identical package, because the
corpus was small enough that all three eligible runs were selected whatever the
task said; the scorer's ability to *discriminate* was therefore never tested by
this experiment, only its determinism. Nothing here says what memory does for a
task unlike the ones already in the corpus, and nothing says what it does once
the corpus is large enough that selection matters.

## After A12.1: A11.5b — Atlas sustained pilot with bounded memory

The verdict is `USEFUL`, so the ordering this milestone set for itself applies:
**the memory default stays `OFF`**, and the next step is a sustained pilot that
uses bounded memory deliberately rather than a default that turns it on for
everybody.

Two things should be fixed before that pilot draws any conclusion of its own, and
neither needs new mechanism.

**Equal machine conditions, enforced rather than hoped for.** Both arms of any
comparison must run with the same set of registered repositories and the same
daemon load, and the run should record enough about that load for a reader to
check it. The cheapest version is to register nothing but the arms themselves.
A10.1's own pair 2 is the argument.

**A corpus large enough that selection discriminates.** Both treatment arms here
received a byte-identical package, because three eligible runs about one subject
are selected whatever the task says. This experiment therefore tested the
scorer's *determinism* thoroughly and its ability to *discriminate* not at all.
Until different tasks select different sources, "does relevant memory help" and
"does any memory help" are the same question.

And one thing to reconsider deliberately rather than by drift: **bounded memory
does not compound.** A run that was shown memory is never a source, so the corpus
is frozen at the runs predating the mechanism. That was the conservative
direction for an experiment measuring the mechanism. For a sustained pilot it is
the first constraint to revisit, and revisiting it means deciding what a corpus
shaped by memory is evidence of.

## A11.6 — bounded parallel tasks in a run (shipped)

A11.0 made "one active task per run" a fact about stored rows, and it was the
right guarantee for a season whose whole subject was a single worker. It was
carrying two arguments at once, though, and only one of them survives inspection.

The first is about the repository: the registered tree is one resource, and two
workers editing it at the same time is not a faster run, it is an incoherent one.
That argument is unanswerable and A11.6 makes it *stricter* — it now names the
repo-tree drivers specifically, in `idx_orch_jobs_one_active_repo_tree`, and no
bound may widen it.

The second is about how much a run may have in flight, and one is not a
principled answer to that. It becomes `orch_runs.max_parallel`: fixed at the root
task, defaulting to 1, refused rather than clamped outside `1..8`, and held by a
unique index on `(run_uid, run_slot)`. A run created without asking for anything
behaves exactly as every run behaved before.

**What was added.** Migration 24 — two columns, one index dropped, two created,
no table rebuilt. Bounded admission of sibling tasks at the submit transaction,
with the four A11.0 refusals joined by three more: a run holds one pin, the run's
bound is not exceeded, and the tree stays exclusive. Settlement generalised from
"the task succeeded" to **quiescence plus a scan**: a run is never ACCEPTED or
BLOCKED while anything in it is unfinished, and when nothing is left it is
ACCEPTED iff every task SUCCEEDED or FAILED-with-a-child, plus the existing
repository-identity re-check. `ATLAS_ORCH_RUN_MAX_WORKER_STARTS` stays 3 and now
counts the repo-tree chain's starts specifically. `--parent` and `--parallel` on
`atlas job submit`, `--parallel` on `atlas job run`, and `active_count` /
`max_parallel` on `job.run_status`.

**What was deliberately not added.** No general task DAG — a run is a chain plus
siblings, and there is no dependency edge beyond `parent_job_uid`. No N-way
concurrency in the repository's own tree; that is the one thing the second index
exists to make impossible. No new isolation mechanism: a sibling is a workspace
task under A8's existing isolation, provisioned by the dispatchers that already
exist. No thread, process, timer or background loop; no new RPC method, MCP tool
or gateway route; no second submit path, and `atlas_db_orch_run_set_status` still
has no caller outside `src/db/db_orch.c`.

**Two properties stated rather than discovered later.** A gateless workspace
sibling can **veto** acceptance and can never **grant** it — ACCEPTED still flows
only from the gated repo-tree chain succeeding, and a sibling only adds the
requirement that it too ended well. And a doomed run does not stop the chain
mid-run: a cancelled or failed sibling does not interrupt the repo-tree task
beside it, so the run spends at most its bounded budget before settling BLOCKED.
One task's failure must not break another task's execution, and killing work in
flight to reach a verdict a few seconds sooner would be doing exactly that.

**The residual, and it is a real one.** A single dispatcher process runs one
workspace attempt at a time, so N siblings genuinely overlapping needs N
dispatcher processes. That is already safe — the lease compare-and-swap is what
makes two dispatchers unable to take one job, and
`tests/test_orch_parallel.c` proves two tasks of one run holding two unreleased
leases simultaneously — but it means the *bound* a run carries is an admission
limit rather than a promise of throughput. Dispatcher-internal concurrency slots
are recorded in `docs/backlog.md` as a non-goal of this change rather than as an
oversight.

**The cost that was accepted.** Adding a repo-tree driver now requires a
migration, because the index predicate carries the driver list and SQLite cannot
call `atlas_orch_driver_is_repo_tree`. `tests/test_orch_parallel.c` compares the
two spellings in both directions over `atlas_drivers()`, so the obligation cannot
be forgotten silently — only fixed deliberately.

### Pilot A11.6-P — one real two-worker run (CLOSED the season)

One pilot, defined and frozen before launch: a real `claude-repo` root task in
this repository's own tree (verify a unit test for `atlas_db_in_transaction`,
gates `make` and `ctest -R test_db`) beside a real `claude` workspace sibling
(a focused adversarial review of the writer-yield functions, reported as an
artifact), both in one run with `--parallel 2`. Success required, among six
criteria, that the two workers' attempt windows genuinely overlap in the ledger
and that the run settle only at quiescence from Atlas' own gates.

**It took two runs, and the first one's failure was the more valuable half.**
Pilot 1 (run `rbbd47da…`) failed on three findings: the foreground run driver
dies on an IPC frame-header read timeout — `apply_op` retries only `BUSY:`, so
one congested read killed the invocation, the lease expired, and the requeued
task then sat driverless past its wall; and the run it left behind exposed a
shipped defect — **five terminal-state producers never settled their run**, so
a sibling that hit its wall on a heartbeat left the run ACTIVE forever at zero
active tasks. That defect was fixed the same evening (every terminal producer
now settles; two regression cases; the fix commit names this pilot), and the
worker's own product — the test it wrote — stayed in the tree, exactly as
A11.1's tree semantics intend.

Pilot 2 (run `r2ba3e8e…`) ran under an operator-side supervisor that re-resumes
a dead driver, and **was ACCEPTED at 17:52:50Z**: the sibling ran from 17:44:58
to 17:52:50 and the root's whole successful attempt (17:45:04 → 17:48:19,
gates passed) sat strictly inside that window — two live model workers, one
run, measured from `orch_transitions` and never from worker output. Zero
unreleased leases at the end; the repo-tree chain spent two of its three
worker starts (the first on the driver death, honestly); the root's accepted
attempt recorded AVAILABLE usage ($3.00, 12 turns, 188 s); the sibling produced
a 4 385-byte review report whose existence, size and digest are recorded.

**Residual findings, recorded rather than argued away:** the driver's
transport-timeout fragility killed an invocation in both pilots and is the
next thing to harden (`docs/backlog.md`); a workspace artifact's bytes are not
retrievable after the workspace is removed when the content was not inlined —
the sibling's report manifest survives, its bytes do not; and a worker's model
is the operator session's default with no selection surface, which made both
pilots run on the most expensive model available.

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

## A12.0 — the planned run (shipped)

Every season up to A11.6 could carry a chain of tasks *an operator wrote*. A12.0
is the season where the chain itself is proposed by a model, and the whole design
exists so that the proposal changes nothing about who decides.

> **A PLAN A MODEL WROTE IS A PROPOSAL, NOT A VERDICT.**

**What was added.** Migration 25 — `orch_plans`, `orch_plan_revisions`,
`orch_plan_tasks` and `idx_orch_jobs_correlation`; three new tables, no table
rebuilt. The `atlas-plan-1` document format, with a first-party bounded parser
whose every refusal names a sentence and a line. Two drivers, `claude-plan` and
`fake-plan`, both workspace and both PLANNER-role — so no repo-tree driver was
added and the index predicate that keeps the registered tree exclusive did not
have to move. A `role` on `atlas_driver`, and `planner_model` / `executor_model`
in the root-owned policy, so which model a role runs under is the operator's
choice and no model name appears in `src/`. Four RPC methods in the existing
orchestration client group: `plan.create`, `plan.revision_add`, `plan.get`,
`plan.list`. `atlas_plandriver_run`, a foreground loop that submits a planner
job, ingests the revision its artifact compiles to, walks the revision's stages
as ordinary A11.6 runs, and answers a stage-run that settled BLOCKED with one
bounded replan. `atlas plan run|status|show|list` on both renderers. And, out of
T1, the run driver's transport recovery: a lost answer is retried on its own
bounded budget and is never confused with a refusal, with a third transport
member `job_get` so a lost completion can be asked about the *task* rather than
the run.

**What was deliberately not added.** No status column and no `plan.settle` — a
plan's status is derived on every read by `atlas_db_plan_state_derive`, so there
is no CAS to win and no verb to reach, which is A11.0's authority-by-absence one
layer up. No general task DAG: a revision is stages in order, each a chain plus
siblings, which is A11.6's shape chosen by a planner instead of by an operator.
No blocker-artifact fast-path: the replan trigger is Atlas' own verdict about a
run and never a sentence a worker wrote. No thread, process, timer or background
loop; no new method group, MCP tool, gateway route or second submit path; no
model name in code; and nothing automatic — a plan runs because an operator ran
`atlas plan run`.

**The stated ceiling.** 5 planner jobs, 3 compiled revisions, 4 stages, 8 tasks,
3 side tasks per stage, and each stage-run's existing 3 repo-tree worker starts:
worst case **5 + 3 × 4 × (3 + 3) = 77 worker starts per plan**. The practical
small-goal case is one revision and one or two stages, roughly four to eight. The
number is written down so that nobody discovers it in a bill.

**What the end-to-end test found, and it is the reason the test exists.** Two
blocking defects that every unit and edge test in the season passed straight
over. A planner's artifact was *described* to the daemon and never carried to it
— the A8 dispatcher sends a four-field manifest, so `content_stored` was 0 for
every workspace artifact, and `plan.revision_add` compiles from stored bytes and
from nothing else; every production plan would have refused every document it
paid for and burned all five planner jobs doing it. And `fake-plan` could not
stand where `claude-plan` stands: it wrote everything after its marker line,
which is the whole document only while the marker is last, and a composed planner
prompt embeds the operator's goal in the middle. Both are fixed; neither was
reachable without running a goal through to a settled plan over a real socket.

**Residual findings, recorded rather than argued away:** a planner job's own run
is workspace-rooted and never settles, so it stays ACTIVE forever — pre-existing
semantics, now produced on purpose; a plan whose *fifth* planner document is
format-refused stays PLANNING durably, because a refusal leaves no row and at
k = 5 there is no next planner job to be the evidence of one; a failed gate's
name does not reach a replan prompt, which therefore says `(none recorded)`; a
refused-document retry loses the completed-work section; `plan.revision_add` does
not compare the planner job's `repo_identity_hash` to the plan's; a background
dispatcher reads the policy once at start, so the new model keys need a restart;
and the general case of pilot A11.6-P2's finding — a workspace artifact's bytes
not surviving the workspace — is closed only for PLANNER-role drivers. All are in
`docs/backlog.md`.

### Pilot A12-P — one real planned run, planner and executor as different models (CLOSED the season)

Frozen before launch: repo `atlas`, goal "add focused unit tests for
`atlas_buf_appendf` edge cases to `tests/test_core.c` … at most one stage, one
tree task, at most one side task", floor gates `make` and
`ctest --test-dir build -R test_core --no-tests=error`, `--parallel 2`,
`planner_model = fable`, `executor_model = opus` in the root-owned policy, and
one deliberate driver kill mid-planner as the restart protocol.

**The run that closed it.** Plan `pa6d5f55…`, ACTIVE 03:28:14Z → COMPLETED
~04:33Z, four planner jobs of five, two revisions of three, one stage of one
ACCEPTED. The planner ran with `--model fable` (observed in the worker argv);
the accepted tree attempt recorded `claude-opus-5`, $5.31, 34 turns in
`orch_usage` — two roles, two models, both chosen by the policy and neither
named in `src/`. Both stage runs held a tree task and a workspace sibling
RUNNING concurrently. The driver was killed at 03:30:24 mid-planner and
`--resume` re-derived everything from rows and submitted planner k=2 — nothing
was lost. The accepted product — three `atlas_buf_appendf` edge-case tests —
was reviewed and committed after Atlas' own gates passed it.

**What the pilot found, fixed forward the same hour:** the composed planner
prompt said `artifacts/plan.atlas-plan` while the worker's cwd is the sibling
`work/`, so a real planner's plan landed where nothing collects — the suite
never saw it because `fake-plan` writes through `atlas_ws_write` directly. The
prompt now states the path from the worker's seat and a missing plan file earns
a typed, budgeted retry instead of a dead invocation; planner k=3 compiled
revision 1 on the corrected instruction with the refusal quoted in its prompt.

**What the pilot found and left honest:** revision 1's sibling finished its
work but its completion arrived during post-restart semantic maintenance; the
background dispatcher's completion retry is shorter than the run driver's T1
budget, the lease expired under sustained `BUSY`, recovery wrote
RECOVERY_REQUIRED, and the sibling veto settled the run BLOCKED with the tree
task SUCCEEDED and its gates passed — after which the driver asked for a replan
and revision 2 completed cleanly. Two residuals filed in `docs/backlog.md`:
the dispatcher's completion deserves the run driver's 300 s discipline, and
`blocking_task` names the wrong task when the blocker ended
RECOVERY_REQUIRED rather than FAILED (money, never authority).

## A12.1 — reconciled model memory and revision-bound task context (items 1–7 and 9 shipped; item 8 outstanding)

A12.0 proved that Atlas can ask one model for a plan and another to execute it,
while keeping acceptance in Atlas' gates and the operator's policy. It did not
prove that either model entered the run with a coherent account of the project.
Claude Code project memory, user memory, generated context and copied notes can
all preserve different revisions of the same assertion. More memory in that
state is not more context; it is a larger unlabelled conflict.

This is the urgent prerequisite to A11.5b. The bounded cross-run memory default
remains `OFF` until this season closes and the sustained pilot can consume
reconciled rather than merely retrieved memory.

> **MODEL MEMORY IS AN ATTESTATION, NOT PROJECT TRUTH.**

**The job.** Add one dedicated `CONTEXT_RECONCILE` path. Root-owned policy
explicitly registers the memory sources it may read: Claude Code repository,
project and user memory first, with later adapters using the same contract.
Every source is preserved with origin, scope, observed time and content hash.
A Git-tracked source is bound to its commit and blob and its textual history is
read from Git rather than copied into a second diff store. A registered source
outside Git receives an Atlas-owned snapshot/version chain. Hidden provider
memory that Atlas cannot read is outside the claim.

The reconciler extracts discrete propositions into the existing A9.2 claim and
attestation model; it must not create a competing truth database. It evaluates
each proposition according to what the proposition means: current source,
build wiring and tests describe implementation; the effective approved decision
revision describes authorized intent; revision-bound runtime evidence describes
a deployment. If code and an approved decision disagree, the result is
implementation drift. It is never resolved by silently rewriting the decision,
and a later timestamp alone never supersedes anything.

Every observed working-tree change makes the affected memory view dirty. Every
accepted source revision, effective decision revision or registered-memory
revision produces a new monotonic `memory_generation`, incrementally
re-evaluates the affected claims and records a semantic diff: claims added,
changed, supported, contradicted, made stale or impacted, superseded with
provenance, or left `UNKNOWN`. Git remains canonical for the textual source
diff; Atlas records what that diff changed in project knowledge and why the
conclusion follows.

**Commit provenance.** When Atlas prepares a commit, it appends a compact,
machine-readable Git trailer block rather than expanding the prose subject. The
root-owned policy chooses the public fields; the baseline is:

```text
Atlas-Provenance: v1
Atlas-Run: <run uid>
Atlas-Memory-Generation: <generation>
Atlas-Context-Digest: sha256:<digest>
Atlas-Decision-Set-Digest: sha256:<digest>
Atlas-Change-Reason: <record uid>
```

A trailer is a pointer, never proof and never authority. On ingestion Atlas
resolves every referenced record, verifies each digest and binds the association
to the commit tree. Missing, unknown or tampered references remain `UNKNOWN`;
they cannot manufacture an approval or gate result. Prompts, memory bodies,
credentials, secrets, model names and costs never enter a public commit message.
A normal commit without Atlas trailers remains valid Git history, merely without
this additional provenance.

Before a planner or executor starts, Atlas emits a bounded Canonical Context
Pack pinned to at least:

- repository identity and exact commit or candidate-tree digest;
- the effective decision revisions it depends on;
- `memory_generation` and the registered source versions;
- included claims, supporting and contradicting evidence, freshness and gaps.

The same pinned inputs must reproduce the same pack. A pack whose repository,
decision or memory generation has moved is not reported as current. Relevant
unresolved disagreement is exposed as `CONTEXT_CONFLICT`; unrelated stale
material is reported and excluded rather than turning every run into a gate.
The worker's output is checked after the run for reliance on a claim that the
pack identified as stale, contradicted or unknown.

**What may update automatically.** Atlas-owned derived memory projections and
task packs may be regenerated mechanically after their inputs move. Human- or
operator-authored memory receives a proposed patch and a diff; it is never
silently rewritten. The reconciler cannot approve, reject or supersede a
decision, modify root-owned policy, upgrade its own trust, or treat a model's
summary as evidence merely because that model produced it.

**Acceptance is evidence, not a demo.** Close the season only when a controlled
repository proves all of the following:

1. three copies of one assertion, including an older stale version and a genuine
   contradiction, retain their individual provenance and cannot collapse into
   one confidence score;
2. a source commit invalidates only the claims in its bounded impact set, emits
   both the Git reference and the semantic memory diff, and leaves unrelated
   claims byte-for-byte stable;
3. a code/decision mismatch is reported as implementation drift, while an
   implementation fact and a normative decision can both remain accurately
   represented;
4. Git-tracked Claude memory survives history reconstruction from commit/blob
   identity, and a non-Git registered memory source survives restart with its
   snapshot and diff chain intact;
5. an Atlas-owned projection updates mechanically, while an equivalent
   hand-authored memory file produces only a proposed patch;
6. a task cannot receive a pack falsely labelled current after any pinned input
   moves, and unresolved material is `UNKNOWN` or `CONTEXT_CONFLICT`, never
   silently selected by recency;
7. an adversarial memory file cannot smuggle instructions into policy, approve
   itself, alter a decision or cause a read/reconciliation pass to write source;
8. one frozen pilot compares the same real task with the existing bounded-memory
   retrieval and with the reconciled Context Pack, recording correctness,
   contradictions caught, omissions, token cost and model usage without calling
   one successful run a general result;
9. rebuilding from a Git trailer resolves every reference against Atlas' own
   rows and never adds authority: a malformed, unknown or deliberately
   altered field produces explicit `UNKNOWN` rather than a wrong answer.
   Ingestion recomputes no digest from content — five of the trailer's six
   lines are stored-value comparisons against one of three canonical rows
   that a rebuild does not reproduce (`memory_context_packs`, `orch_runs`,
   `ai_reasons`), so a rebuild that does not carry those rows forward sends
   `Atlas-Memory-Generation`, `Atlas-Context-Digest` and
   `Atlas-Decision-Set-Digest` to `UNKNOWN` together (all three are checked
   against one frozen pack row), and sends `Atlas-Run` or
   `Atlas-Change-Reason` to `UNKNOWN` independently. Only
   `Atlas-Change-Reason` can bind to a *different* record rather than to
   none — it is a bare integer-id existence check with no content compared,
   so an emptied-and-repopulated table could reuse the id; the other four
   are keyed by values nothing else can be assigned, so a reader seeing
   `UNKNOWN` on those must read it as one of the three rows not surviving,
   never as tampering. The sixth line, `Atlas-Provenance`, is a fixed marker
   with no value to lose.

**Deliberate non-goals for this season.** Reading a model provider's hidden
internal memory; mass-rewriting every note on every keystroke; using GitHub as a
second source of Git truth; automatically adopting a design because current code
implements it; and blocking unrelated work merely because some historical
memory remains stale.

**Season status.** Items 1 through 7 and item 9 are shipped, each with the
assertion named in `docs/context-reconciliation.md`'s acceptance table.
**Item 8 is outstanding, not partially met and not waived**: the one frozen
pilot this item requires — one real task run twice at a pinned commit,
comparing the previous season's bounded retrieval against this season's
Context Pack — spends real money on a real model and was deferred by the
operator to a later season. No measurement exists yet, and the bounded
cross-run memory default from the previous season stays `OFF` until that
pilot runs: this project's own rule that no evidence of a result is not
evidence against one applies to its own acceptance, and this document does
not pre-write the shape of a finding nobody has measured yet.

## Later: A14 — a job an operator submits from wherever they are

The sentence it exists for is

> **THE GATEWAY CANNOT SUBMIT WORK BECAUSE OF WHO IT RUNS AS, AND THE OBVIOUS FIX
> IS TO STOP THAT BEING TRUE.**

Atlas' purpose is that an operator connects a client they like — Claude Code, a
browser, a model over MCP — and work gets done in a repository under Atlas' gates.
Every half of that exists except the last: **no surface outside the local socket
can submit a job.**

### The constraint, verified rather than assumed

Two lines of root-owned policy decide it:

```
/etc/atlas/orchestration.conf:  submitter_uid = 1000    (the operator)
/etc/atlas/gateway.conf:        gateway_uid   = 992     (atlas-gateway)
```

The orchestration RPC group is selected by `SO_PEERCRED` against that policy, so
the gateway does not fail a check — **it is not in the set at all**, and it speaks
on the socket as 992 whatever its code intends. That is A7.1's own sentence
working exactly as written: *what the gateway cannot do is true because of who it
runs as, not because of anything in `src/gw`.*

The remote surface is otherwise ready. `remote_mcp = yes` is served at
`/mcp`, authentication is a Bearer API key rather than a uid, and keys already
carry **scopes** — the `chatgpt-tunnel` key holds five, all read-only. So the
identity mechanism and the per-key authorisation vocabulary both exist. What is
missing is a scope that means "may submit", a route that carries it, and — the
whole question — a way for that request to reach the writer without becoming the
gateway's own authority.

### The tempting fix is the one that must not be taken

Adding `submitter_uid = 992` makes it work in one line, and destroys the argument
A7.1 is built on. From that moment the gateway can submit anything, and the only
thing between a remote caller and a job running on the machine is code in
`src/gw` — which is precisely the place A9 says a boundary must never live,
because a check there is one an attacker walks around while the process keeps its
full authority.

**If A14 ends with that line in the policy, it has failed even if everything
passes.**

### Three shapes, none free

1. **The request carries its own principal.** The gateway forwards a submission
   that names the key it arrived under, and the submit path authorises on that
   rather than on the peer. Cheapest to build; hardest to argue, because the
   gateway now asserts an identity instead of having one, and every A7 rule about
   `SO_PEERCRED` exists because a caller describing itself is not evidence about
   itself.
2. **A broker with its own uid.** A third principal the gateway can hand a request
   to and cannot impersonate, which holds the submitter right and applies the
   per-key policy. Keeps identity kernel-asserted at every hop; costs a process, a
   socket and a policy section.
3. **A remote submission is a proposal, not a job.** It lands as a durable request
   an operator disposes of — the shape A4's approval channel and A12.0's plan
   revisions already have. Weakest capability and strongest argument: nothing
   remote ever starts work, and "an operator was in the loop" stays literally
   true. It also may not be what the operator wants, which is the point of writing
   it down as a choice rather than picking it here.

### What must be true whichever shape wins

- **A budget per key.** A job spends real money and runs a worker on the machine.
  Today no per-client bound exists, because the only submitter was the operator
  and the operator is trusted by design. Opening submission ends that premise, and
  A11.1's three-starts-per-run budget is a bound on a *chain*, not on a caller.
- **A9's absences stay absences.** Remote credential administration is absent, not
  refused; no MCP tool name carries an authority verb; a model payload cannot
  accept a run. Submission is none of those — the gates still run and settlement
  is still Atlas' — and the season's first job is to say precisely why, in the
  rules, before writing the route.
- **The audit row names the key, never a claimed value**, the way `gw_audit`
  already refuses to store what a caller says about itself.

## Next: A15 — the review surface, and where a proposal is disposed of

A4 gave Atlas a lifecycle and one channel that may move it. A9 gave it a web
surface that may only read. Today those two facts meet at an inconvenience an
operator feels on every proposal: the record is written by a model over MCP, and
the only way to look at it properly is a terminal, while the only surface that
renders it well can do nothing about it.

> **A PROPOSAL NOBODY CAN REVIEW COMFORTABLY IS A PROPOSAL NOBODY REVIEWS.**

**The job.** Make Mission Control the place a proposal is *read* — every revision,
its evidence and counter-evidence, the aggregate and the reasons behind it, the
gate results, the impact set, and, where a claim's aggregate `conflict` is
`IMPLEMENTATION`, A12.1's one producible drift shape — and then decide,
deliberately, how far it may go towards disposing of one.

**What is true today, measured rather than assumed** (2026-09-01). The gateway
serves 26 routes, counted by property rather than by a number kept in a fourth
place, and **not one of them mutates a decision, an evidence row or a claim**:
no `decision.approve`, `decision.reject` or `decision.supersede` appears in
`API_ROUTES[]`, and no row names any operator-only method. The only two writes
this listener can produce are its own bookkeeping — an appended `gw_audit` row
per request, and the in-memory session table `/auth/login` and `/auth/logout`
mutate — and beneath the table sits the fact that actually holds: the operator
method group is offered only to the `SO_PEERCRED` uid a root-owned policy names
as the operator, so a route naming `decision.approve` would still answer
`unknown method` to the gateway's own uid whatever the table said. MCP tool
names are scanned against a forbidden-verb list — `approve`, `approval`, `reject`,
`supersede`, `confirm`, `sign`, `resolve`, `revalidate` — in
`tests/test_decision_mcp.c`, so a lifecycle transition is not reachable from a
model by name. And the same test scans `CLAUDE.md` itself against a second list,
which forbids Atlas from ever claiming that its local channel establishes that a
natural person is present.

That second list is the one that decides the shape of this season, and it points
the opposite way from the intuition. **There is no human-proof gate to weaken.**
`LOCAL_OPERATOR_CONFIRMED` identifies a channel, not a person, and A7.1 states
plainly that a same-uid process driving a pseudo-terminal — an AI agent with a
shell included — reaches it exactly as a person does. What the local channel
actually buys is narrower and worth naming precisely: the caller must be a
*local process running as the operator's own uid*, holding a single-use capability
bound to that revision's content hash. Every honest comparison in this season is
against that sentence and not against a stronger one nobody ever had.

**The concrete risk, which is deployment-specific and current.** This machine's
gateway listens on one address and carries two surfaces at once —
`remote_mcp=yes web_gui=yes` — and the MCP half is connected to an external model
through a tunnel. Any approval capability placed on that listener is one scope
check away from the model that proposed the record. A9 already holds the
precedent for the answer rather than for the prohibition: `memory:write` is *in
the vocabulary and is not grantable*.

**Three tiers, in increasing order of what they cost the threat model. The choice
between them is the season's opening decision and is deliberately not made here.**

1. **The UI reviews; the terminal disposes.** Mission Control renders everything
   needed to decide and maintains a queue — "I would approve this" — that stores
   no authority at all. One local command walks the queue and confirms each
   through the existing content-hash capability. The threat model does not move
   by one line, nothing is added to the listener the model reaches, and the
   expensive half of the work — reading — moves to the surface that is good at it.
2. **The UI drives the challenge; the operator completes it locally.** Tier 1,
   plus the UI showing the single-use capability so the flow is led from the
   browser and finished in the terminal.
3. **A genuine remote operator channel.** If this is chosen, six requirements are
   not negotiable, and a design missing any one of them should be refused rather
   than shipped:
   - **Its own channel identity** — `REMOTE_OPERATOR_CONFIRMED` or similar,
     **never** `LOCAL_OPERATOR_CONFIRMED`. Reusing the local name would make every
     audit row ever written ambiguous in retrospect, which is the one cost that
     cannot be paid back later.
   - **Its own credential scope, ungrantable to any model credential**, on the
     `memory:write` precedent.
   - **Absent from the MCP surface entirely**, so the two halves of the listener
     do not share a door.
   - **TLS in front.** Atlas terminates none and must never be described as
     providing it; today this listener is cleartext.
   - **Replay protection bound to the revision's content hash**, reusing A4's
     existing shape rather than inventing a second one.
   - **A root-owned policy deciding which `kind`s it may act on at all.** A
     defensible default: `OPERATIONAL_FACT` and `PARKED` remotely, with `POLICY`,
     `INVARIANT` and `ACCEPTED_RISK` staying local-only.

**What must not happen, whichever tier is chosen.** No approval verb appears in
an MCP tool name, ever — the scanner enforces it and the scanner is right. No
route gains the ability to mutate a lifecycle without its own channel identity in
the audit row. And nothing written in `CLAUDE.md` or in the UI may claim that any
channel establishes that a natural person acted; the honest sentence names the
channel and its reachability, and `tests/test_decision_mcp.c` already fails a
build that forgets.

**Deliberate non-goals.** Approving from a model surface under any framing;
a shared credential that reads and disposes; and describing tier 3 as equivalent
in strength to the local channel — it is weaker by construction, and the season
that ships it says so in the same paragraph that announces it.

**Tier 1 is what shipped, and the chain for choosing it, tier 3's full priced
cost, and every finding running this plan produced are in
`docs/review-surface.md`.** The threat model moved by nothing: no route,
scope, method, actor or migration was added, and the property `tests/test_gateway.c`
checks on the route table is what keeps that true after this season rather than
only at the moment it shipped. Tier 3 remains absent, not refused, and is
costed rather than built so a later season can choose it with the bill already
in view.

## Invariants that outlive every phase

1. SQLite is a rebuildable index, never the canonical record of history.
2. Git and repository contents are authoritative for source and history facts.
3. Every result preserves provenance.
4. Atlas never invents a reason. `UNKNOWN` is a valid, first-class answer.
5. No Atlas *read* modifies a target repository, ever. Scanning, indexing,
   watching and every `src/git` invocation are read-only. The one thing that
   writes is a worker an operator started with `atlas job run`, under a driver
   the root-owned policy named — see A11.1, which narrowed this line and says
   exactly how far.
6. Repository contents are untrusted input — as bytes reaching a terminal, and
   as prose reaching a model. Those are different defences.
7. Human and machine output come from one service layer.
8. Schema changes are numbered, transactional migrations.
9. Exactly one process writes the index at a time.
10. Atlas never claims the index is current when it cannot prove it.

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

`docs/engineering-rules.md` carries the rules and the full argument;
`docs/daemon-and-ipc.md` carries the behaviour; `docs/backlog.md` carries the
original incident with this resolution appended to it.

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
that an operator running `atlas job run` may start a child process whose purpose
is to edit the tree, in a directory Atlas resolved from its own registry, under a
driver the root-owned policy named. Three things must line up and removing any
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

## Next: A11.5a — Atlas-on-Atlas pilot, cross-run memory off

The next milestone is the first real pilot: one operator-supervised run against
this repository, with `claude-repo` enabled in the root-owned policy, cross-run
memory deliberately **off**.

It is a pilot rather than a feature because what is unproven is not the
machinery — that is what A11.1's fourteen acceptance cases are — but whether a
task text, a gate list and a three-worker bound are enough structure for a real
change to land. That question cannot be answered by a test.

What A11.5a must not do: enable cross-run memory, add a second model role, run
more than one worker at a time, or let anything it learns become an automatic
input to a later run. Those are what the milestone is holding constant so that
the one variable it is measuring stays readable.

**A10 is not closed and is not cancelled**, and the ordering is deliberate rather
than an oversight. A10 is the Experience Learning phase, and the contract it must
satisfy is already written: `docs/verification.md` carries "The A10 prerequisite
contract" — the table a reviewer of A10 checks against, whose load-bearing line
is that **A10 must treat `UNKNOWN` as epistemic uncertainty and must never fold
it into a negative fact.** Atlas can guarantee it never *produces* an unjustified
`ABSENT`; it cannot guarantee a later phase reads `UNKNOWN` correctly, which is
what makes that a contract on the consumer and not a property of this codebase.
Nothing here weakens it, and a reader who arrives at A11.1 should not conclude
that the phase before it was absorbed.

## A7 (original plan) — optional MCP adapter (absorbed into A2)

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

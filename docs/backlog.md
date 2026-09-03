# Known engineering and security backlog

Everything Atlas knows about itself that is not fixed. Recorded rather than
carried silently, so that a later phase inherits a list instead of a surprise.

Nothing here blocks A1 correctness. Items that did were fixed in A1 and are
listed at the bottom for the record.

## Deferred from the A0 security audit

These were identified during the A0 review and deliberately not investigated
further in A1: A1's scope was the daemon, and reopening each of them would have
turned it into a second security season. They are listed with what is known,
not with a guess at severity.

### 1. Symlink hardening beyond the working tree

`atlas_path_open_nofollow` refuses to traverse a symlink at any component when
reading repository content, and a tracked symlink is hashed by its link text.
That covers the working tree.

Not covered: the paths Atlas is *given* rather than the ones it walks. A data
directory reached through a symlink, a `--data-dir` pointing at one, or an
intermediate symlink in the runtime directory's parents are accepted. The
immediate parents are checked (`atlas_ipc_ensure_runtime_dir` refuses a symlinked
runtime directory; `lock.c` and `unit.c` refuse a symlinked target file), but the
chain above them is not.

*Why it is deferred:* an attacker able to plant symlinks in the parents of the
Atlas data directory already has write access to the directory containing the
index. The lock and the socket are not the interesting target at that point.

*What would close it:* resolve the data directory once with `openat` from a
verified root and hold a directory descriptor for the process's lifetime, using
`*at` calls throughout instead of absolute paths.

### 2. Defensive result initialisation

Several functions write their `out` parameter only on success. Callers all check
the status first, so no live path reads an uninitialised value, and the
sanitiser builds are clean. It is still a shape where a future caller that
forgets the check gets garbage instead of a defined value.

*What would close it:* every fallible function zeroes or initialises its outputs
before doing anything that can fail, uniformly, with the convention stated in
`CLAUDE.md`.

### 3. Database and data-directory symlink hardening

`atlas_db_open` creates the database file with `open(O_RDWR|O_CREAT)` and no
`O_NOFOLLOW`, so a pre-existing symlink at `atlas.db` would be followed. The WAL
and SHM sidecars are the same. Related to item 1 and with the same
already-has-write-access reasoning.

*What would close it:* `O_NOFOLLOW` on the database open, and `openat` relative
to a held data-directory descriptor.

### 4. Evidence retention and database growth

`evidence` rows accumulate and are never pruned. A1 bounds the *raw event*
journal (`ATLAS_EVENTS_RETAIN_PER_REPO`) but deliberately does not prune
evidence: evidence is the durable provenance record, and silently discarding it
would undermine the invariant that every result preserves provenance.

A repository churning for months therefore grows the evidence table without
limit. Measured behaviour today: a pass over an unchanged repository adds no
evidence at all (there is a test), so growth is proportional to real change, not
to time.

*What would close it:* a retention policy that is explicit about what is lost —
for instance, collapsing repeated `SOURCE` evidence for one path into a first-seen
and last-seen pair — plus a `doctor` report of index size and a documented
`atlas compact`.

### 5. Submodule visibility

A gitlink is recorded as `file_type = other` with a note saying its contents
belong to another repository, and `--ignore-submodules=all` keeps git from
descending into one. Atlas therefore knows a submodule exists and nothing about
what is in it.

*Why it is deferred:* indexing a submodule means registering it as its own
repository, which is a modelling decision (does a change inside a submodule
belong to the parent's history?) rather than a bug.

*What would close it:* an explicit `atlas repo add --with-submodules` that
registers each one separately and records the parent/child relation.

### 6. Lazy-fetch assumptions

Atlas refuses partial (promisor) repositories outright because git 2.39 cannot be
told to refuse a lazy fetch. `GIT_NO_LAZY_FETCH=1` is set and is honoured by git
2.41 and later, but Atlas does not currently *detect* the git version and relax
the refusal accordingly.

The A1 detection is now exact and fail-closed (see below), so the refusal is
correct. It is also stricter than it needs to be on a modern git.

*What would close it:* parse the git version at `atlas_git_probe` time and, on
2.41 or later, accept a promisor repository while relying on `GIT_NO_LAZY_FETCH`
— with a test that proves the variable is actually honoured rather than assumed.

### 7. Exact scope of the read-only digest proofs

`fx_tree_digest` hashes relative paths, entry types, permission bits, symlink
targets and file contents, and the smoke and adversarial suites use it to prove
that Atlas does not modify a repository. It does not cover extended attributes,
ACLs, ownership, or timestamps.

Timestamps in particular are *expected* to change: reading a file updates its
`atime` unless the filesystem is mounted `noatime`. Including them would make the
proof fail for a reason that is not a modification.

*What would close it:* state the scope in `SECURITY.md` (done), and add xattr and
ownership to the digest where the platform supports it.

## New in A1, not fixed

### 8. The watcher cannot see everything, by construction

Documented in full in `docs/watcher-consistency.md`. inotify does not report
changes made through some bind mounts or on network filesystems that do not
implement it, and Atlas has no way to detect that in advance. Such repositories
are covered only by periodic reconciliation.

This is not closable — it is a property of the mechanism. What Atlas does about
it is refuse to *claim* currency it cannot prove.

### 9. `mtime` granularity

A file rewritten within the filesystem's timestamp granularity, to exactly the
same size, inode and mode, compares as unchanged and is not rehashed. Every Linux
filesystem Atlas targets reports nanoseconds. The periodic full pass is the
backstop where one does not.

*What would close it:* an optional "always hash" mode for filesystems known to
have coarse timestamps, selected by measuring the granularity at registration.

### 10. Reconciliation is per repository, not per path

An event on one file triggers a pass over the whole repository's file list. The
pass is cheap — one `lstat` per file, no content read — but it is O(files), not
O(changed). On the 5000-file fixture that is about 480 ms.

*Why it is not fixed:* a path-targeted pass has to decide what else the change
could have invalidated (a `.gitignore` edit changes the classification of
everything below it), and getting that wrong means a stale index that looks
current. The whole-repository pass is the version that is obviously correct.

*What would close it:* a fast path for the common case — a modification to an
already-tracked file with no `.gitignore` involvement — falling back to a full
pass otherwise.

### 11. `repo.add` over IPC refuses awkward paths — *partly closed in A2*

A repository path containing a control byte, a quote or a backslash is refused
when routed through the daemon, because the CLI builds that request's JSON by
hand. The offline path takes the same value as an argv operand and accepts it.

A2 built the missing piece: `atlas_ipc_params_begin`/`_finish` construct request
documents through the first-party streaming writer, and every A2 method uses it —
so the AI adapters send arbitrary filesystem paths without the restriction.

*Still open:* `src/cli/cli.c` was not converted. Its two hand-built requests and
their byte check are untouched, so `atlas repo add` through a running daemon still
refuses an awkward path and still tells the user to register it offline. The fix
is now mechanical rather than a design question.

## New in A2, not fixed

### 12. A change set is correlated at two points, not continuously

`PostToolBatch` queues a reconciliation it cannot wait for — the pass is a job
behind it on the same writer thread, so waiting would be the writer waiting on
itself — and therefore correlates against the snapshot as it was before the
edits. The turn close sweeps again, by which time the pass has normally
published.

Consequence: a file created and deleted between the batch and the turn close is
attributed to nothing. A file changed after the last turn close of a session and
before the next event is picked up by the following turn rather than the one that
caused it.

*Why it is not fixed:* the alternative is a completion handshake between the
correlate job and the reconcile job on one thread, which is a deadlock waiting to
be written. The two-point sweep is the version that is obviously correct.

*What would close it:* have the reconciliation pass itself notify open change
sets when it publishes, so correlation is driven by the pass rather than polled
by the adapter.

### 13. Attribution cannot see anything but Atlas' own clients
<!-- Items 15 and 16 were here and are gone: percent-encoded root URIs and
     MCP-only registration are both implemented. See the note at the end. -->

`ambiguous` means *another Atlas session* had the repository open. A person
editing in a text editor, a build script, or a second tool with no Atlas
integration is invisible: their change is attributed `direct_edit` to whichever
session named the path, or `observed` if none did.

*Why it is not fixed:* Atlas cannot know who wrote a file. inotify reports that a
path changed, not which process changed it, and reading that from `/proc` would
be both racy and a much larger privilege story.

*What would close it, partly:* record the wall-clock gap between a session's edit
intent and the index observing the change, and downgrade attribution when it is
implausibly large. That is a heuristic, so it needs its own provenance class
before it is worth having.

### 14. There is no approval workflow, by design — and no path to one yet (closed in A4)

A2 records proposals. `approved` is pinned to 0 by a schema `CHECK`, refused by
`atlas_provenance_writable_in_a2`, and never bound by either insert statement.

That is correct for A2 and it is not a finished story: there is currently no way
for a human to approve a proposal at all, so `USER_APPROVED_DECISION` exists in
the vocabulary with nothing able to produce it.

*Closed in A4*, though not the way this predicted. `atlas decision approve
<repo> <id>` exists and requires a real terminal, a single-use capability bound
to one revision's content hash, and a confirmation typed against that hash.

The `CHECK` was **not** lifted. Doing so would have made an approval something
that happens to a model's own row, in the table the model writes, distinguished
from a proposal by one integer; A4 approval is a separate record about a
separate object — an immutable revision — with its own actor vocabulary and its
own ledger. `USER_APPROVED_DECISION` is still unwritten, and now for a sharper
reason than "A2 cannot": it names a *person*, and Atlas cannot establish one.
What A4 records is `LOCAL_OPERATOR_CONFIRMED`, which names a channel.

*What remains open:* a same-uid process can imitate an operator. Closing that
needs an identity Atlas does not have and A4 deliberately did not invent — see
item 16.

### 16. The operator channel is a channel, not an identity

`LOCAL_OPERATOR_CONFIRMED` records that Atlas' interactive channel was used: a
controlling terminal, a single-use capability bound to one revision's content
hash, and a confirmation typed against that hash. It does not identify a person
and does not prove one was present.

Any process running as the same local user can allocate a pseudo-terminal, run
the CLI against it and type the confirmation. `tests/test_decision_operator.c`
does exactly that, deliberately: a suite that could not would be claiming more
than the code supports.

*Why it is deferred:* closing it needs a real identity — a signing key, a
hardware token, or a platform authentication agent — and every one of those is a
security subsystem with its own lifecycle, its own failure modes and its own
storage. A4's scope was the lifecycle, and adding the vocabulary of attestation
without the mechanism would have been worse than the honest current claim.

*What would close it, partly:* an operator key held outside the data directory,
with the approval event carrying a signature over `(document, revision, content
hash)`. That is a phase, not a patch, and it changes what `APPROVED` means —
which is exactly why it should not be smuggled in.

### 17. An orphaned decision is invisible until its root is registered again

`repo remove` does not delete decisions — they are the one canonical record in
the index — but it does detach them, and a detached document appears in no
listing. Registering the same canonical root reattaches them by root hash.

*Why it is deferred:* the alternative is an `atlas decision list --orphaned`
surface, which is a small amount of code and a real addition to the command
vocabulary for a case that has an obvious remedy. It is documented in
`docs/decision-lifecycle.md` under Recovery rather than left to be discovered.

## Carried out of A3

### 15. Structural indexing has one producer, and mixing them is future work

`code_analyzers` interns a producer identity and `code_index_state.analyzer_id`
records which one built a repository's graph — one integer per repository,
because a structural pass has exactly one producer. A future importer that mixes
sources, an optional SCIP index for the files it covers with the lexical
analyzer for the rest, needs the same reference on `code_relations`.

*What would close it:* `analyzer_id INTEGER REFERENCES code_analyzers(id)` on
`code_relations`, populated by whichever producer wrote the row, and a currency
rule that is per-producer rather than per-repository. The schema is shaped for
it — one integer per row against a vocabulary already interned — and nothing in
A3 implements any of it. No SCIP, no Clang, no LSP, no plugin loader.

### 16. The initial structural index was over its own target — closed

Kept for the record because the three things that fixed it are the three things
to look for next time.

On a 5 444-file fixture the first pass took **62.4 s** against a 60 s target,
split evenly between applying rows and resolving them. It is now **45–48 s on a
larger fixture** — 5 988 files and 515 822 lines — and none of it came from
relaxing anything:

- **A relation kind that duplicated an indexed column.**
  `symbol_contains_occurrence` restated `code_occurrences.enclosing_id` as an
  edge: 38 % of the relation table, five index insertions each, read by no
  query in Atlas. Not written any more; the kind stays in the vocabulary.
- **An index the planner would not use.** The include suffix lookup could seek
  `idx_code_files_basename` and instead scanned every file in the repository,
  because `UNIQUE(repo_id, path_raw)` gives a competing index with the same
  first column. `INDEXED BY` settles it — a hard constraint, not a hint, so the
  statement fails loudly if the index ever goes away.
- **A sort of zero rows, a quarter of a million times.** Candidate lookup
  ordered its results with an ORDER BY no index could satisfy, so SQLite built a
  temporary B-tree per lookup — almost always to sort nothing. It now asks
  unordered and asks again, ordered, only when there turns out to be more than
  one candidate, which is the only case where the order is reported.

None of these traded correctness for speed and none is a planner trick: the
first removes duplication, the second is checked by a test that asserts the
query plan, and the third runs the identical query on the only path where its
answer is observable.

### 17. A shared-header edit costs its true blast radius, which is large

Editing a header that every module includes takes **3.5 s**, against 0.75 s for
an implementation file. That is not a defect — every call site naming a changed
declaration genuinely has to be re-resolved, and it is resolution rather than
reparsing, which is the property A3 promised. But it is the one incremental case
that is seconds rather than milliseconds, and a real project's `common.h` looks
exactly like this.

*What would close it:* nothing cheap. Re-resolving fewer edges means deciding
that some call sites cannot have changed, and the only sound basis for that is
knowing the declaration's *content* did not change — a diff at symbol
granularity rather than at file granularity. That is a real feature, not a
tuning knob.

### 18. Structural indexing has no per-repository opt-out

Every registered repository is structurally indexed on every reconciliation
pass. There is no way to say "index this one's files but not its structure", and
for a repository with half a million lines of C that nobody asks structural
questions about, the first pass is a minute of work for nothing.

*What would close it:* a per-repository flag honoured by
`atlas_reconcile_opts.skip_code`, which already exists and is currently only set
by tests.

## Fixed during the A2 attribution pass

- **An MCP write chose its session by recency.** With no session key, a recorded
  reason or decision attached to the newest open session for the repository. Two
  Claude Code sessions on one worktree therefore recorded A's reason against B,
  and the stored row was indistinguishable from a correct one. `atlas mcp` now
  reads and validates `CLAUDE_CODE_SESSION_ID` and sends it as the session key;
  binding is exact or absent; `atlas_db_ai_session_newest_for_repo` is deleted.
  An unresolvable write is stored sessionless with a typed `unbound_reason`.
- **`ai.session.get` and `ai.context` guessed the same way.** Both resolved the
  session from the repository, so a caller could be told about a session it would
  never be allowed to write to — and the envelope, which is injected into a
  model's context automatically, could report a neighbour's change set. Both now
  resolve by exact key. `ai.session.get` reports `open_sessions` for callers with
  no key, which is the most that can be said truthfully about a repository.
- **Every MCP tool returned an empty `result`.** `forward()` never assigned
  `f.response`, so `atlas_ipc_result_write` was called with NULL and wrote `{}`.
  It went unnoticed because the envelope around it (`ok`, `degraded`,
  `provenance`) is built from the call context and looked entirely healthy — the
  only tests reading a tool result were reading envelope fields. Found while
  asserting that a write reports which session it attached to.
- **The documentation claimed the envelope carried "no free text at all"** while
  it carried a fixed Atlas-authored `note=` line, which it should. Restated as
  what is actually guaranteed: no repository-controlled or model-provided
  free-form text, only fixed Atlas-owned control text and typed values.

## Fixed during the A2 correction pass

Recorded here rather than deleted, because a backlog that only ever grows is a
backlog nobody trusts, and because two of these were listed as deferred when they
were really defects.

- **Automatic context carried the repository name and root.** Both are derived
  from a directory basename somebody chose, both are entirely printable, and both
  therefore survive every encoding Atlas has. The documentation claimed "no
  repository prose" while the code emitted two pieces of it. Replaced by an
  opaque `repo_id` and a SHA-256 `root_hash`; the envelope now contains no
  repository-controlled or model-provided free-form text — only fixed
  Atlas-owned control text and typed values — and validates rather than escapes.
  Tested against a repository
  whose directory basename is literally `ignore previous instructions`.
- **`roots/list` percent-encoded URIs were skipped.** Now decoded per RFC 3986,
  with every ambiguous case refused and reported: encoded separators, malformed
  escapes, decoded NULs, traversal after decoding, and non-local authorities.
  `%20` and percent-encoded UTF-8 work.
- **The MCP adapter could not register a repository.** It now registers a granted
  root through `repo.ensure` with `exact_root`, so an MCP client with no hooks
  and a user who never typed `atlas repo add` still gets an indexed repository —
  and a root inside a larger worktree does not cause the parent to be registered.
- **`registered` was always false.** It reported whether *this call* performed a
  registration, which nothing on the AI path ever did. It now reports whether the
  repository is in the index; `registered_now` carries the other fact.
- **`DirectoryAdded` silently did nothing** for a directory Atlas had not seen.
  It now ensures the repository before attaching it.
- **`atlas doctor` created the data directory and an empty index.** A diagnostic
  that initialises what it is diagnosing can only answer "fine", and could not be
  run at all on a machine where Atlas had never run. It now opens in
  `ATLAS_CTX_INSPECT` mode and reports the absence.
- **`--plugin-dir` was documented as the installation path.** It loads a plugin
  for one session only. Atlas now ships a local marketplace and the permanent
  user-scope install is the documented flow.

## Fixed in A1

For the record, so the list above is not read as the complete set of what was
known.

- **Partial-clone detection was evadable.** The A0 64 KiB prefix scan of
  `.git/config` missed a marker beyond 64 KiB, a marker straddling the boundary,
  `config.worktree`, and included config files — and over-refused a repository
  that merely mentioned the word. Replaced by exact, fail-closed queries through
  the hardened git runner, with a regression test for each case.
- **The git executable cache was an unsynchronised global.** Correct while Atlas
  was single-threaded; a data race the moment it was not. Now immutable after
  publication with every access serialised, frozen before any thread is created,
  and verified under ThreadSanitizer.
- **The documentation implied safe text prevented prompt injection.** Corrected
  in `SECURITY.md` and expanded in `docs/ai-trust-boundary.md`.
- **The IPC serve loop blocked on a slow client.** A client that sent three bytes
  of a header and stopped held the loop for the read timeout, stalling every
  other client. Found by a test written for it; the loop is now non-blocking with
  per-connection state.
- **The recursive watch installer walked a truncated tree.** It held a pointer
  into the buffer it was appending to, so the first reallocation left it
  dangling and the walk stopped early — 6 directories watched out of 51. Found
  by the performance script; there is now a regression test.


## After A6

A6 shipped impact gates and stale-decision detection. Explicitly **not** started
and **not** claimed, carried forward unchanged:

- the full dedicated security review;
- clangd integration and toolchain truth;
- the Atlas orchestration / control plane;
- the Claude dispatcher;
- the GitHub issue / PR / review loop;
- model routing;
- Testnet 2 automation.

A6 deliberately provides a reusable gate evaluator for a future orchestration
layer and implements no orchestration itself.

Two smaller things A6 chose not to do, recorded so the choice is visible rather
than forgotten:

- **Rename tracking.** A renamed symbol is reported `MISSING`, never followed.
  Atlas has no deterministic identity evidence that a new name is the old object,
  and Git rename detection is a heuristic over content similarity — which is
  exactly the kind of guess a decision anchor must not be re-pointed by.
- **Content-level transitive evidence.** Impact is path-level, because only
  direct anchors carry a content snapshot. The asymmetry is documented in
  `docs/impact-gates.md` rather than hidden: a direct anchor that changed and
  changed back is `FRESH`, a dependency that did the same is still `IMPACTED`.

## A9.2.3: a production-scope caller verifier

A9.2.3 records the test/production split on every semantic generation
(`tu_test`, `tu_production`, `test_scope_known`, from operator-declared test
roots) and reports it on every surface. **Nothing consumes it.**

There is no verifier for "does any *production* caller of X exist", so §45's
distinction — "no production caller" versus "no caller anywhere including tests"
— is inexpressible today. That is the safe half of the two: Atlas cannot answer
the narrower question wrongly, because it cannot be asked at all.
`atlas.no_proven_caller` answers the wider question over the whole indexed scope
and its scope sentence says so.

Adding one means: a member of `atlas_verify_verifier`, a row in `VERIFIERS[]`, a
scope sentence, a written argument that it is a read, a row in
`atlas_verify_verifier_absence_dims` that includes `ATLAS_COVDIM_TESTS`, and a
source for that dimension in `sem_coverage` — which would be
`test_scope_known` together with the scope manifest, since a production-scope
absence needs both "we know which sources are tests" and "we read every
production source".

`ATLAS_COVDIM_TESTS` is UNKNOWN until that exists. No absence rests on it today,
so this is a missing capability rather than a defect.

## A9.2.4: decayed semantic graphs heal only on a full rebuild

The incremental carry-forward defect described in
`docs/semantic-discovery.md` is fixed, and `ATLAS_SEM_ANALYZER_VERSION = 2`
makes every affected generation stale so the daemon rebuilds it once. That is the
repair and it is automatic.

What it does **not** do is tell anybody which generations were affected or by how
much. A generation built before the fix carried a call graph that had lost an
unknown fraction of its edges, and nothing recorded the loss — the symbol count
was untouched, the unit counts were untouched, and `tu_complete == tu_total`
throughout. Any conclusion drawn from such a graph before the rebuild was drawn
from an index that was quietly incomplete.

Two things a later phase could add, neither of which belongs here:

- **A structural check in `atlas doctor`**: no `sem_edges` row may reference a
  unit outside its own generation. It is one query, it is exactly the invariant
  the fix establishes, and `doctor` already replays the decision ledger for the
  same kind of reason. It would report, never repair — A4's rule.
- **An edge-count sanity signal on a generation**: a rebuild whose edge count
  falls by an order of magnitude against the generation it replaced is either a
  large deletion or a defect, and Atlas cannot currently tell an operator that it
  happened at all.

## A9.2.4: semantic indexing occupies the writer thread

A8-CI put the semantic index pass on the writer thread, which is correct — it is
the one thread that owns a writable handle — and A9.2.3 was content with it
because a pass ran only when an operator asked or when a repository the operator
had explicitly enabled changed.

**A9.2.4 makes automatic maintenance the default, which increases exposure to a
cost that was always there.** While a pass runs, no other write to the index
proceeds: no reconciliation, no session bookkeeping, no decision write, no
credential operation. On a repository with several build configurations that is
minutes. Observed on this machine: 235 s for one configuration of 198
translation units, and a repository presenting five such configurations is five
times that on its first build.

What already limits it, and why none of it is the fix:

- one build per repository at a time (`atlas_writer_sem_index_pending`) — bounds
  concurrency, not duration;
- coalescing, which falls out of the derived plan — bounds how *many* builds
  happen, not how long one takes;
- the retry governor — stops a failing repository spinning, and a succeeding one
  is not spinning;
- unit reuse across generations — makes the *steady state* cheap and does
  nothing for a first build or for an edit to a widely-included header.

The fix is to stop holding the writer for the whole pass. Two shapes, neither of
which belongs in a season about discovery:

1. **Yield between units.** The pass already chunks its work and already commits
   per batch; what it does not do is let another writer job run between chunks.
   That is a scheduling change in `src/daemon/writer.c` plus an argument about
   what a half-built generation means to a concurrent write — nothing, since a
   generation is invisible until `atlas_db_sem_publish`, which is why this is
   tractable at all.
2. **A second writer for derived data.** Larger, and it reopens "exactly one
   process writes the index", which is A1's rule and is not to be reopened
   casually.

> **Shape 1 shipped in A9.2.7 (2026-08-19); shape 2 stays open.** The pass yields
> at the points where nothing is open and the writer drains the latency-critical
> kinds before resuming, on exactly the argument this entry gives: a half-built
> generation means nothing to a concurrent write because it is invisible until
> `atlas_db_sem_publish`. `job_kind_is_drainable` is where the list lives and
> every exclusion carries its reason. **Shape 2 is untouched and A1's rule is
> unmoved** — there is still exactly one writer thread, one writable handle and
> no second process.

Until then the honest statement is the one `docs/semantic-discovery.md` makes: a
first automatic build of a large repository will make the daemon unresponsive to
*writes* for its duration, reads are unaffected, and an operator who cannot
accept that sets `semantic_auto_default = DISABLED` or disables the repository.

> **A9.2.6: "reads are unaffected" was false, and measurably so.** The serve loop
> dispatches one request at a time, so a write blocked behind a pass blocked every
> other client with it — a second client's ping was measured at 4009 ms. Writes
> during a pass are now refused quickly and retryably rather than waiting, and
> reads are genuinely unaffected. The rest of the paragraph stands: an operator
> who cannot accept a pass at all still disables it.

## A9.2.5: a semantic index pass makes the daemon unreachable to every client

> **RESOLVED IN A9.2.6.** The resolution and the two corrections it forces are
> appended at the end of this entry. Everything above that appendix is left as it
> was written, including the parts measurement later contradicted: what was
> believed at the time is part of the record, and an entry silently edited to
> agree with the answer teaches nobody how the answer was reached.

**IMMEDIATE OPERATIONAL ITEM — to be resolved before O10.** Not an ordinary
low-priority backlog line: the observed effect is a daemon that answered no read
for roughly 25 minutes. It does not break trust correctness — every answer it
did give was correct — but a repository-intelligence daemon nothing can query is
indistinguishable from one that is down.

### What was measured

- **Correlation, established.** A 400 s monitor recorded nine consecutive samples
  from 11:39:14 to 11:40:33 with `daemon ping` at the client ceiling
  (`rpc_ms 6008-6020`) while one daemon thread held ~100 % CPU, and recovery the
  moment the burst ended. Idle samples either side: 22-52 ms.
- **The burst is a semantic index pass.** A bounded 15 s `strace` during it shows
  thread 2783442 doing 19 244 `pread64` + 14 236 `pwrite64` with libclang parse
  children spawning and reaping.
- **The serve loop stalls inside a lock wait.** The main thread
  (`accept4(4,...)`) shows `restart_syscall = -1 ETIMEDOUT` after ~5 s — the
  configured `sqlite3_busy_timeout` — then answers `{"id":"cli","ok":false}`.
- **The serve loop is single-threaded.** `src/ipc/server.c` is one `poll()` loop
  that dispatches each request synchronously in `client_step_read`. One slow
  request therefore blocks every other client, including `daemon ping`, which
  touches no repository at all.

### The WAL explanation was wrong — DISPROVEN

An earlier draft of this entry blamed an un-checkpointed 253 MB WAL. Measured
against the live daemon, that is false:

```
PRAGMA page_size            -> 4096
PRAGMA journal_mode         -> wal
PRAGMA wal_autocheckpoint   -> 1000        (the default IS active)
PRAGMA wal_checkpoint(PASSIVE) -> 0|22|22  (busy=0, log=22, checkpointed=22)
```

The WAL *file* is 255 MB but holds **22 live frames**. SQLite does not truncate
the WAL below its high-water mark without `journal_size_limit` or a TRUNCATE
checkpoint, so a large file is expected and harmless. `busy=0` means no reader
was blocking a checkpoint. And the decisive comparison: **with the WAL still at
255 MB, `daemon ping` measures 22-23 ms.** WAL size does not correlate with
latency.

### Verdict: SUPPORTED BUT INCOMPLETE

Established: the stall is coincident with a semantic pass; the serve loop is
serial, so any one slow request blocks all clients; the serve loop did exhaust a
5 s SQLite busy timeout during the burst.

**Not established:** which specific lock the timeout was on, which request was
slow, and whether the cause is lock contention or plain I/O saturation from
33 000 syscalls in 15 s. The `fcntl(F_SETLK, F_WRLCK) = -1 EBADF` seen just
before the refusal is unexplained.

### Why it is worse now

A9.2.4 made semantic maintenance the default, so the condition is no longer
something an operator opts into. A9.2.5 adds nothing to the serve loop's cost —
`atlas_sem_trust_now` replaces `atlas_sem_freshness_now` rather than joining it —
but it does not reduce it either.

### Candidate fixes, none implemented

- **The serial serve loop is the real lever.** A8-CI's own rule is that "an
  operation that can outlast a client's patience does not run in the serve loop";
  indexing was moved out, but semantic *reads* were not, and under a concurrent
  pass they can exceed a client timeout. A cheap first step is to answer
  `daemon.ping` before any database work, so liveness stops depending on the
  index being queryable.
- A typed IPC refusal ("the index is busy, retry") instead of a bare frame-header
  timeout, so a client can tell "Atlas is loaded" from "Atlas is gone".
- Measure `atlas_sem_source_identity` cost per semantic read on a large
  repository; it is the one part of a read that scales with the tree.
- `journal_size_limit` if the 255 MB file is itself unwanted — cosmetic, and
  explicitly **not** a fix for this.

### What must not be repeated

The suite passed 79/79 while the daemon was unreachable. Nothing asserts daemon
liveness under load, and that is the test this entry most needs.

### Resolution — A9.2.6

The missing half was found by reproducing the stall under control and taking a
stack sample rather than reasoning from the strace. The serve loop was here:

```
atlas_server_serve -> atlas_server_dispatch -> method_session_open
                   -> atlas_writer_ai -> pthread_cond_timedwait
```

and the writer thread here:

```
atlas_sem_index_on -> atlas_sem_index_run -> atlas_sem_parse_unit
```

So the answer to "which lock" is **no lock**: it is the writer's completion
condition variable. A synchronous writer call queues a job on the single writer
thread and waits with a timeout; a minutes-long semantic pass was ahead of it in
the FIFO; and because the serve loop dispatches one request at a time, every
other client — `daemon ping` included — waited behind that wait. Measured on this
repository: ping 26 ms idle, **3.9 s per such write**, the write itself failing
after 4027 ms. See `docs/engineering-rules.md` for the rules and the fix.

**Correction 1 — "reads are unaffected" was wrong.** Both this file and
`docs/semantic-discovery.md` said a pass costs the daemon its *writes* while
reads continue. It does not: the cost is head-of-line, so a blocked write takes
every client with it. A second client's `daemon ping` was measured at 4009 ms
beside one blocked write, and the regression test asserts exactly that.

**Correction 2 — the 5 s `ETIMEDOUT` was probably never SQLite's busy timeout,
and this is stated as unestablished rather than corrected.** The original
incident's strace was not re-run and cannot be, so what follows is inference
about that incident from a mechanism proved in another. `writer_call_impl`'s
default wait is 5000 ms — the same figure as `sqlite3_busy_timeout`, which is
why the coincidence was persuasive. But a single `restart_syscall = -1
ETIMEDOUT` after one uninterrupted interval is the shape of one
`pthread_cond_timedwait`, whereas SQLite's busy handler sleeps in repeated small
increments and would have left many short waits. The condvar is therefore the
likelier reading of that trace, and the `fcntl(F_SETLK, F_WRLCK) = -1 EBADF` it
also recorded remains unexplained by anything found here.

**The test this entry said it most needs now exists.**
`tests/test_daemon_responsive.c` drives a live daemon through a semantic pass and
asserts liveness under exactly that load. It fails without the fix, with the
figures above.


## Migration wind-back fixtures are maintained by hand, and A11.0 proved it

**Found during A11.1, fixed there, recorded here because the class outlives the
instance.**

`tests/test_decision_kind.c`'s `wind_back_to_schema_12` rewrites a live database
back to schema 12 so that migration 13's rebuild can be measured against real
rows. To do that it must drop every table and column that migrations 14 and
later added — and it does so with a hand-written list, under a comment that says
so: "A new table means a line here, which is the same 'nothing is globbed'
discipline the source list follows."

A11.0 added `orch_runs`, `orch_jobs.run_uid` and two indexes in migration 21 and
did not add the lines. The result was not a subtle wrongness: the forward
migration failed outright with `table orch_runs already exists`, so the case
never reached a single one of migration 13's assertions. It was reproduced at
`ee29f34` — the commit before A11.1 — from a clean `git archive` build, so it is
established as pre-existing rather than inferred to be.

**What is worth acting on is not the missing line.** It is that this fixture has
no way to notice it is incomplete. The failure surfaced at the *next* season's
full-suite run rather than at the one that caused it, and it surfaced as a
migration error rather than as "the wind-back is out of date", which is a
different thing to go looking for.

Two shapes would end the class, and neither was in A11.1's scope:

- **Assert the shape rather than the version.** The wind-back already checks
  that the schema version reads 12 and that four A9.1 artefacts are gone. It
  could instead compare the object list against migration 12's own expected set
  and fail naming what is left over, which is a check that cannot go stale.
- **Derive the drop list.** Every migration's statements are already data in
  `MIGRATIONS[]`; a rewind that read them would be the same list rather than a
  second one. That is a larger change and the usual argument applies — a derived
  list is only better if the derivation is simpler than the list.

Until one of them exists, **adding a table or a column in a migration means
adding a line to `wind_back_to_schema_12`**, and the way you find out you forgot
is a full-suite run.

## A11.5a found four things it deliberately did not fix

The A11.5a pilot — the first attempt to run a real Claude worker against Atlas'
own tree — found one defect that blocked it outright and four that did not. The
blocker (the gate wire encoding, `4f796a2`) was fixed under the pilot's own
defect procedure. These four were left alone, because a pilot that repairs
everything it trips over stops being a measurement.

**`job submit` accepted `--gate` and dropped it — CLOSED the same day, and this
entry stood stale for nine.** The argument parser filled `st->opts.job.gates`,
and the `submit` arm of `src/cli/cli.c` copied eight fields into
`atlas_job_submit_opts` without copying `gates` or `gate_count` — though the
struct carried both and `service_orch.c` knew how to encode them. The `run` arm,
twenty lines below, did copy them. The symptom was a submission refused for
declaring no verification command while the operator was looking at the `--gate`
flags they had typed. That is the fifth-wiring-place failure `CLAUDE.md`
describes.

It was recorded here at 05:56 on 2026-08-19 (`b3a9c5f`) as deliberately not
fixed, because `job run` was A11.1's surface and was unaffected. It was fixed at
13:48 the same day, in `88e7472`, because A10.1 could not proceed without it:
freezing a memory package before either arm of a comparison runs means creating
both runs first and driving them afterwards, and a repository-tree task with no
gate cannot be created at all. `src/cli/cli.c` carries the copy in all three
arms — submit, run and plan — and the comment at the submit arm says why it was
made rather than left here. The bound is safe on both sides: the parser refuses
a ninth gate and both arrays are `[8]`.

**Nobody updated this paragraph, and that is the part worth keeping.** For nine
days it told a reader to make a three-line change that already existed. A
backlog that records a fix and not its closing is a backlog that costs a second
investigation to answer a question already answered — which is what happened on
2026-08-28, when it was re-opened, re-read and closed again from the commits.

**The residual is real and is not the code.** Nothing in the suite drives the
argument parser for this path, which is the same absence that let the defect
exist: `tests/test_orch_parallel.c` and its neighbours call the service layer
directly, so they would pass with the arm still dropping the field. Closing that
needs a decision rather than a test. `job.submit` is refused outright on a
machine with no orchestration policy, the policy path is a compiled-in constant
with no override, and a test may not install a root-owned file — so an
end-to-end CLI assertion would need a test channel into the policy, which is
exactly the shape P0's rule forbids for the watcher's budget and forbids here
for a stronger reason: this one is a security boundary.

**A failed spawn spends a worker start.** A11.1 records RUNNING before the exec
so that a crash cannot be retried for free, and a process that never existed is
classified the same way. When the worker executable could not be resolved, all
three starts of run `re74398e9147dd6ae42ed10ef2102f9c5` were spent inside one
second and the run was BLOCKED. That is the documented behaviour working as
written, and it is still worth asking whether `SPAWN_FAILED` — which establishes
that no worker ran, wrote or observed anything — belongs in the same class as a
worker that died halfway. The answer is not obviously "no": an executable that
disappears mid-run is indistinguishable from one that was never there, and a
budget that refills on a spawn failure is a retry loop with no bound.

**The worker executable is resolved against a fixed PATH.** `claude_exec` uses
`atlas_proc_which("claude", "/usr/local/bin:/usr/bin:/bin")`, deliberately: the
child's environment is constructed rather than inherited, for the reason
`src/git` constructs its own. The consequence is a deployment prerequisite
nothing states — a machine whose `claude` lives in a home directory cannot start
a worker, and the failure arrives as `SPAWN_FAILED` with no path in it. Either
the requirement belongs in `docs/orchestration.md`, or the refusal should name
the program it looked for and where.

**One repository's churn starves orchestration everywhere.** Throughout the
pilot a second registered repository was being edited continuously; every change
scheduled a semantic pass, and a semantic pass answers every orchestration write
with `BUSY`. Atlas' own recovery sweep failed on that basis every twenty seconds
for the whole window, and the pilot's first submission needed sixteen attempts
over forty-seven seconds to land. Nothing here is incorrect — A9.2.6's `BUSY`
contract is exactly what makes retrying safe — but "unbounded semantic
maintenance is per-daemon while repositories are many" is a scaling property
nobody has written down, and the operator-visible symptom is that orchestration
appears broken on a busy machine.

> **ADDRESSED IN A9.2.7 (2026-08-19).** The mechanism is a yield, not a second
> writer: an unbounded job now hands the writer thread back at the points where
> nothing is open — between translation units, either side of the unit loop, and
> every `ATLAS_SEM_DISCOVER_YIELD_EVERY` directory entries during a walk — and
> the drain runs whatever latency-critical writes are queued before the pass
> resumes. `ATLAS_JOB_ORCH` is on that list, so a recovery sweep, a lease and a
> completion are served *during* a pass instead of being refused for its
> duration. A waiter gives the pass `ATLAS_WRITER_YIELD_GRACE_MS` to reach a
> yield point before it backs out, so `BUSY` is now the exception rather than the
> rule.
>
> **The residual is stated rather than closed.** A single translation unit that
> parses for up to `ATLAS_SEM_PARSE_TIMEOUT_MS` is a stretch with no yield in it,
> and a write that arrives inside one is refused at grace expiry exactly as
> before. The scaling property this entry names is *reduced*, not removed:
> maintenance is still per daemon while repositories are many, and the thirteen
> registered worktrees below would still spend more of the writer thread than one
> would.

## A11.5a closed with two residuals, and both are about evidence

The pilot reached `ACCEPTED`. These are what it could not close on the way, kept
separate from the milestone because a measurement that quietly repairs what it
measures is not one.

**A large worker log is discarded on the success path.** `report()` attaches the
worker's streams as inline artifacts and skips any body over
`ATLAS_ORCH_ARTIFACT_INLINE_MAX`; the durable result spool is cleared once the
daemon accepts the completion. Each half is right on its own and together they
lose the whole log exactly when the run *worked*: a refused completion keeps
everything, an accepted one keeps nothing above 256 KiB. The accepted pilot run
produced a log well past that, so its token and cost figures — which the CLI puts
in the result record inside that log — had to be recovered from the worker's own
session transcript under the operator's home directory.

That recovery is not a mechanism, it is a coincidence. It worked only because
A8.1's model dispatcher runs the worker under the operator's session; a worker
running as `atlas-worker` has a private home inside its workspace and leaves no
such file. **So Atlas currently cannot report what a successful run cost**, and
the next milestone is an A/B experiment whose control arm is exactly that. Two
shapes would fix it and both are small: carry the usage fields out of the result
document as completion metadata, or keep the spool when the log exceeded the
inline ceiling rather than dropping both copies.

**Cross-crash completion settlement is still not possible.** A driver that dies
after its worker finished but before the daemon accepts the completion leaves a
durable result that nothing can deliver: `op_complete` requires a lease token,
and A8's rule is that a token is never stored, only a digest of it. The spool
therefore guarantees the result still exists to be *read*, not that another
process may present it.

This was left alone deliberately rather than solved badly. Every cheap route runs
through putting a bearer credential on disk, and the alternatives — a daemon-side
predicate that accepts a late completion for an attempt no newer lease has
superseded, or a resume that re-claims the same attempt and receives a fresh
token — are changes to the lease model, not minimum fixes. A season that wants
this should decide the authority question first and write it down; until then the
honest statement is that a crashed driver costs an attempt, and the wall deadline
bounds what that costs.

## A10.0 closed one of A11.5a's two residuals

The usage residual is gone: an attempt's cost is now read from the worker's final
streamed record, made durable before the completion is offered, and stored per
attempt in a table whose counts are nullable so that `UNKNOWN` and `0` stay
different answers. A large worker log being dropped no longer takes the figures
with it.

**The second residual is unchanged.** Cross-crash completion settlement is still
impossible without putting a bearer token on disk, and A10.0 did not touch it.
Note that a usage row is written *inside* the completion transaction, so an
attempt whose completion never lands has a durable `.usage` file and no row —
which is the correct shape, and the same authority question decides whether
anything may ever deliver it.

**One thing A10.0 did not fix, and did not try to.** Atlas still records nothing
about what a *gate* cost. Gates run real builds and test suites, and on the
accepted A11.5a pilot they ran twice; the wall clock covers them but no token or
CPU figure does. That is fine for comparing model arms against each other, which
is what A10.1 needs, and it would not be fine for a claim about what a run costs
in total. Worth stating before somebody reads a run's usage as the whole bill.

## A10.1 measured the starvation A11.5a described, and it is worse than stated

A11.5a's fourth finding was that one repository's churn starves orchestration
everywhere. A10.1 registered seven throwaway worktrees of this repository to run
an A/B experiment in isolation, and turned that finding into the thing that
decided the experiment's verdict.

**What it looks like.** With thirteen registered repositories the daemon runs
build-input discovery walks near-continuously. `ATLAS_JOB_SEM_DISCOVER` is one of
the two job kinds `job_kind_is_unbounded` answers yes for, so every orchestration
write during one is refused with `BUSY`. A worker that has finished its work and
whose driver cannot land a completion loses the whole attempt: one experiment arm
did exactly that, and the tree it had correctly edited was evidence of work Atlas
had no record of.

**Why it is not only an operator's problem.** Registering a repository is a
deliberate act, so "do not register thirteen" is fair advice. But nothing in
Atlas bounds the aggregate: discovery is per repository, its interval is per
repository, and the writer thread is per daemon. The cost of one more registered
repository is paid by every orchestration client of that daemon, and no surface
says so. An operator who adds a tenth repository is not told they have made
autonomous work less likely to complete.

**What was measured.** Removing nine of the thirteen registrations changed a
worker's observed duration by enough to confound a comparison between two arms
of an experiment — which is how it was noticed. Numbers are in the A10.1 section
of `docs/roadmap.md`.

**Not fixed here, deliberately.** Every plausible fix is a design decision this
milestone had no mandate for: a per-daemon budget for unbounded maintenance, a
discovery interval that scales with the repository count, a second writer for
maintenance, or making discovery bounded. A10.1 was an experiment, and an
experiment that repairs the thing it is standing on stops being a measurement.

> **ADDRESSED IN A9.2.7 (2026-08-19), by a fifth shape this entry did not list.**
> The writer yields. A discovery walk offers the thread back every
> `ATLAS_SEM_DISCOVER_YIELD_EVERY` directory entries and a semantic pass offers
> it between translation units, and `ATLAS_JOB_ORCH` is one of the kinds the
> drain serves — so the completion whose loss decided this experiment's verdict
> would now be applied during the walk that refused it, and the arm would not
> have lost a finished worker's attempt.
>
> **What is not fixed, and is the honest reading of this entry.** Nothing bounds
> the *aggregate* yet: discovery is still per repository, its interval is still
> per repository, and the writer thread is still per daemon. Thirteen
> repositories still spend more of one thread than one does, and no surface says
> so. What changed is that the cost is now latency inside a pass rather than a
> window in which orchestration cannot be written at all. The residual is the
> non-yielding stretch of a single large translation unit, and `BUSY` still means
> exactly what it meant.

## A10.1 left the run driver's completion still unable to survive its own driver

A10.1 hit A11.5a's second residual twice, from two directions, and it is
unchanged: a driver that dies after its worker finished leaves a durable result
nothing can deliver, because `op_complete` requires a lease token and A8 never
stores one.

Both times the driver died for a reason outside Atlas — once killed by a test
harness, once exiting after a `BUSY` window outlasted its retries — and both
times a worker's completed work became an expired lease and a requeued task. The
authority question that blocks a fix has not moved: every cheap route puts a
bearer credential on disk. It is worth restating that the cost is not
hypothetical and is measured in whole worker runs.

## Dispatcher-internal concurrency slots

**Recorded as a non-goal of A11.6, not as an oversight.**

A11.6 lets a run hold up to `max_parallel` active tasks and proves two of them
genuinely overlap — two unreleased leases, two RUNNING rows, one run, with the
interleaving visible in the ledger. What it does not change is the *dispatcher*:
`atlas_service_dispatcher_run` takes one job, provisions one workspace, runs one
attempt and completes it before asking for another. N siblings actually executing
at once therefore needs N dispatcher processes.

That is already safe rather than merely usual: the lease is a compare-and-swap
and the partial unique index permits one unreleased lease per job, so two
dispatchers polling the same queue cannot take the same task. Running several is
an operator decision with no code change behind it.

What a fix would be is the reason it is not here. "One dispatcher process running
K attempts concurrently" means K workspaces, K child processes and K live leases
inside one process, and every one of those is a resource the A7.1 boundary
currently accounts for per process: the workspace root, the byte budget, the
model dispatcher's session, and the log redaction path. Deciding K, deciding who
sets it, and deciding what a partially-failed slot does to the others are three
design questions A11.6 had no mandate for — and a bound whose enforcement lives
in a loop counter rather than in the schema is the shape this repository has
spent two seasons moving away from.

The honest reading of the current state: **`max_parallel` is an admission limit,
not a throughput promise.** A run may hold four tasks; whether four workers exist
to run them is the operator's arrangement of dispatchers.

## `test_apikey` failed once under full-suite load, and its output was lost

During A11.6's one full-suite run, `test_apikey` failed while 88 tests shared a
machine that was also running the production daemon and a semantic pass. It then
passed thirteen consecutive re-runs — twelve of them started concurrently, plus
one beside three live-daemon suites — with all six of its cases green each time,
on the same binary and the same schema. Nothing A11.6 touched is on its path:
the suite exercises A9's credential surface, and the only shared code is the
core library and the migration chain, which those thirteen runs applied every
time.

The finding worth keeping is not the flake, it is that **the failure text was
lost**: the suite's output travelled through a `tail` pipe and the re-run
overwrote `LastTest.log`, so what actually failed is unrecoverable. A flaky test
whose one failure cannot be read is a test nobody can fix. Next time it fires,
capture `Testing/Temporary/LastTest.log` before running anything else; if it
does not fire again, this entry is the record that one failure on 2026-08-19 was
observed under load and never explained.

## The run driver dies on a transport timeout it could survive (CLOSED in A12.0)

**Closed by A12.0's T1.** A lost answer and a refusal are now different claims:
`BUSY:` still means the daemon took nothing and asking again gets the same
answer, while a transport failure — the connect, the send, the read, a reply that
was never a reply — is retried on its own bounded budget of
`ATLAS_RUN_XPORT_TRIES` (5) attempts `ATLAS_RUN_XPORT_PAUSE_MS` (2000) apart. The
classification is `atlas_err_is_transport`, stamped by the client layer that held
the file descriptor, so it cannot travel the socket and nothing a daemon says can
produce one. A completion whose answer was lost is additionally owed-checked
against the **task** — the new third transport member `job_get` — because "this
run no longer holds this task open" is equally what an expired lease, a
cancellation and a recovery sweep produce. `tests/test_orch_transport.c` is the
evidence; `docs/orchestration.md`'s A12.0 section states what the owed-check does
and does not establish. Two residuals carried forward from the fix: a lost lease
*grant* costs the invocation (busy, resumable), and a completion that landed and
requeued the task reads as still owed, which is the pessimistic direction.

The original finding, kept for the record:

Found by pilot A11.6-P, twice — once in each of its runs. `apply_op` in
`src/orch/rundriver.c` retries a call the daemon refused with `BUSY:` and treats
every other failure as fatal to the invocation. A congested serve loop can time
out one read ("timed out while reading a frame header") on a heartbeat or a
phase call, and one such timeout kills the whole foreground driver: the worker
keeps running, nobody heartbeats, the lease expires, the attempt is lost and the
requeued task sits driverless until an operator resumes it — in pilot 1 it sat
until its wall consumed it. The BUSY token was A9.2.6's contract for "nothing
was queued"; a read timeout is the *other* claim, and the driver treats both as
if they were a third thing.

What a fix must keep straight: a timed-out **heartbeat** is already survivable
(`driver_should_stop` ignores renewal failures) and a timed-out **completion**
already has its own five-minute budget; the gap is the phase transitions and the
lease claim between them. The operator-side mitigation that carried pilot 2 — a
supervisor loop that re-runs `job run --resume` whenever the driver exits with
the run still ACTIVE — is written in the pilot record and works, but it is a
wrapper around a fragility, not an answer to it.

## A workspace artifact's bytes do not survive the workspace (NARROWED in A12.0)

**A12.0 closed one slice of this and left the rest open**, and the slice is worth
naming because the general case is unchanged. A PLANNER-role driver's artifacts
are now carried inline on the completion — the manifest's optional fifth field,
the same lowercase hex the run driver has always sent — because a plan revision is
compiled from stored bytes and from nothing else, and the workspace is removed
when the attempt succeeds. Asked of the driver's **role** rather than of an
artifact's name, in `artifacts_travel_inline` (`src/orch/dispatch.c`).

Everything below still holds for `fake`, `claude` and every future executor
driver: their artifacts are described and not kept, and an artifact row whose
bytes are unreachable an hour later is not what "collected artifacts" reads as
promising. Whichever behaviour is intended for those, it is still undecided.

The original finding:

Pilot A11.6-P2's sibling wrote a 4,385-byte review report to its workspace
`artifacts/` directory. The completion recorded its name, size and digest;
`content_stored` is 0; the dispatcher then removed the workspace, as it always
does. The manifest therefore points at bytes that no longer exist anywhere. The
root's `worker.log`, carried inline by the run driver's completion, survived
fine — the asymmetry is between the two artifact paths, and whichever behaviour
is intended, an artifact row whose bytes are unreachable an hour later is not
what "collected artifacts" reads as promising.

## A worker's model is the operator session's default, and nothing can choose it (CLOSED in A12.0)

**Closed by A12.0's T2**, and closed where the finding said it belonged: in the
root-owned policy. `planner_model` and `executor_model` are optional keys in
`/etc/atlas/orchestration.conf`, each one token of `[a-z0-9._-]` at most 64
characters, and which of the two an attempt uses is decided by the **driver's
role** and by nothing else — `atlas_driver_model_for`. A submitter cannot name a
model, a job does not carry one, and **no model name appears in `src/`**: unset
passes no flag at all and leaves a worker on the account's own default, which is
what every run before A12.0 did. `deploy/a8/orchestration.conf.template` carries
both keys, commented.

The original finding:


Both pilot workers ran on the operator session's default model — the most
expensive one available — because the `claude` and `claude-repo` drivers invoke
the CLI with no model argument and no surface (spec, policy or flag) selects
one. Pilot 1's sibling spent $7.14 before its wall killed it. A model choice is
an authority-relevant knob (the policy names which drivers may run at all), so
it likely belongs in the root-owned policy rather than on the submission; that
decision is deliberately not made here.

## A12.0 left seven residuals, and none of them is a surprise

Each is a thing the season chose not to do, stated where the choice was made.

**A planner job's own run stays ACTIVE forever.** A planner job is a workspace
job, so it is the root of a workspace-rooted run, and a workspace-rooted run
never settles — A11.6's `settle_run_at_quiescence` asks the *root* task's driver
and refuses to settle a run with no repo-tree root. That is pre-existing
behaviour, but A12.0 is the first thing that produces it on purpose and at a rate
of up to five per plan. `job list` will show them; nothing cleans them up, and
letting a gateless run settle would be a worse answer than an untidy list.

**A `k = 5` refused document leaves the plan PLANNING durably.** A format refusal
leaves no row anywhere: at k < 5 the *next* planner job is the durable evidence
that a refusal happened, and at k = 5 there is no next job to be it. The plan is
resumable and every resume recomputes the same refusal from the same stored
bytes, deterministically, and re-prints it. Ruling 6 of the season made this
choice on purpose — the alternative was a status reading BLOCKED about a plan
whose paid, valid document could still be ingested — and one pathological corner
reads non-terminal forever.

**A failed gate's name does not reach a replan prompt.** `job.get` exposes no
failed-gate index and no failure detail, so `atlas_plan_compose_replan` writes
`failed-gate: (none recorded)` rather than naming a gate nobody established, and
carries an excerpt only when a `gate.log` artifact was stored inline. The fix is
a `failed_gate` field on `job.get`; it is optional and was not in the season.

**A refused-REPLAN retry loses the completed-work section.** There are five
composers and the parse-retry form is not the replan form, so a plan that both
needs a replan *and* had its replacement document format-refused is asked again
with the refusal but without the list of what already succeeded. A design gap
rather than a defect: nothing is lost from the plan, only from one prompt.

**`plan.revision_add` does not compare the planner job's `repo_identity_hash` to
the plan's.** The correlation already binds the job to the plan, and the submit
path already refuses a repository the orchestration policy does not permit, so
this is a narrowing that has not been made rather than a hole. Carried from T3,
T5, T6 and T7 unchanged.

**A background dispatcher reads the policy once at start.** Adding
`planner_model` or `executor_model` to `/etc/atlas/orchestration.conf` therefore
needs a dispatcher restart before a workspace attempt runs under it. That is the
same rule the daemon follows for the same reason — a policy edit takes effect on
restart, which is a fact an operator can reason about — and it is stated here
because a key that appears to have been ignored reads exactly like one that was.

**No blocker-artifact fast-path.** A worker that discovers mid-task that the plan
cannot work has no way to say so: the replan trigger is a stage-run that settled
BLOCKED, which costs the stage its whole three-start budget first. A blocker
artifact could only ever *veto* earlier and could never grant anything, so it is
compatible with the season's rules — but it is a path from model prose into
control flow, and A12.0 deliberately has none of those.

## Two documents on stdout when the daemon is absent — CLOSED, and it was worse than this said

**The original entry, kept because its understatement is the lesson.** It read:
`atlas plan list --json` against an absent daemon prints the empty result
document and then the error document, and `atlas plan run`'s local refusals do
the same; `atlas job list` and `atlas job run` have always behaved this way;
`cli_state.rendered` suppresses the second document on the one path that was
fixed for it — `atlas daemon ping` — and the pattern was never generalised. A
caller had to tolerate two documents or check the exit code first.

**Measured 2026-08-28, and it was not two documents.** Run against a data
directory with no daemon, `job list`, `job get`, `job cancel`, `plan list` and
`plan status` each emitted output that is **not JSON at all**. The suite's own
validator rejects every one of them, at the byte where the second document
starts. What was actually produced was one *unclosed* document carrying
`"ok":true`, with the error document written inside its open array and no
closing bracket or brace anywhere:

```
{"command":"job list","ok":true,...,"jobs":[{"command":"job","ok":false,...,"error":{...}}
```

The difference matters: two documents can be read one after the other, and this
cannot be read at all. A caller "tolerating two documents" would not have
worked.

**The chain.** `renderer_open` writes the header, `"ok":true` included, and a
list arm then opens its array — all before the daemon has been reached. The JSON
writer emits straight to the `FILE*` and buffers nothing, so those bytes cannot
be taken back; `renderer_abort` cannot unwrite them. `main` then finds a non-OK
status with `cli_state.rendered` false and writes a complete error document into
the stream, landing inside the open array, and nothing closes the outer
document.

**The fix, and why it is cheap.** None of these calls streams: the service layer
completes the whole IPC round trip and only then forwards rows out of the
response it already holds. So a connection failure is known before any row
exists. The renderer is now opened at the *first row* instead of before the
call, and a list arm opens it explicitly afterwards when there were no rows, so
an empty list is still an answer. When the call fails first, nothing was
written, and the one error document `main` already produces is the whole output.
The rows could not be buffered instead: `atlas_job_render` is borrowed pointers
into the live response.

`tests/test_cli.c` asserts the contract for all five commands with no daemon
running, using `tjson_valid` — which is true only when the *whole* input is
exactly one well-formed value, so it fails both on a truncated document and on a
second one appended.

## A12-P pilot residuals (2026-08-21)

- **The background dispatcher's completion deserves the run driver's
  discipline.** Revision 1's sibling finished its work and offered its
  completion during post-restart semantic maintenance; `dispatch.c`'s
  completion retry gave up where `rundriver.c`'s 300 s budget (A12's T1)
  would have carried it, the lease expired under sustained `BUSY`, and a
  finished attempt died as RECOVERY_REQUIRED. The fix is the same shape T1
  already built: classify, retry within a budget, and after a lost answer ask
  whether the job ended with this result before mourning it.
- **`blocking_task` misattributes a RECOVERY_REQUIRED blocker.** The replan
  prompt named the SUCCEEDED tree task because the sibling ended
  RECOVERY_REQUIRED, which the FAILED-selection branch does not match, and the
  fallback picked the tree task of the blocked run. Same family as the final
  review's Finding 1, one state wider. Money, never authority: the planner
  re-planned work that stood, and the second stage-run's gates passed over it.
- **Post-restart semantic maintenance collides with pilots.** Two daemon
  restarts in one hour queued semantic rebuilds exactly when the pilot's
  completions arrived. An operator running a planned run right after a deploy
  should expect `BUSY`-shaped friction; the writer's yield (A9.2.7) bounds it
  but does not remove it.

## Atlas instruments writes and not reads, so it cannot see whether it was consulted (2026-09-01)

Measured in a session, not supposed. The Atlas plugin's hook registration
(`integrations/claude/atlas/hooks/hooks.json`, and the installed copy under the
marketplace directory) matches writes and only writes:

```json
"PreToolUse":  [ { "matcher": "Edit|Write|MultiEdit|NotebookEdit", ... } ],
"PostToolUse": [ { "matcher": "Edit|Write|MultiEdit|NotebookEdit", ... } ]
```

`Grep`, `Read` and `Bash` appear in no matcher. The consequence is exact: an
agent that reads the whole tree with `grep` and writes one file is, to Atlas,
**indistinguishable** from one that queried the index thoroughly first. Both
produce the same change set, the same session row and the same context block.

`CLAUDE.md` tells a reader to ask Atlas "before changing unfamiliar code", and
that sentence has the same shape as the matcher — it names the write. The
failure it leaves open is the read: on 2026-09-01 a session opened the A12.1
season, read a dozen unfamiliar files in `src/verify`, `src/orch`, `src/db` and
`src/ipc` to pin interfaces into a plan, and queried the index **zero** times.
Nothing in Atlas, the hooks or the context block registered that, because
nothing is watching for it.

**Why the omission survives, and why it is this project's own failure class.**
A skipped read has no visible failure mode. What Atlas offers a reader —
recorded reasons, impact candidates, the decisions that govern a path — is
invisible to whoever never asks, so the omission produces plausible output and
no error signal at all. That is the shape of `ATLAS_SEM_ANALYZER_VERSION`'s
bumps being no-ops for every repository nobody rebuilt by hand, and of the call
graph decaying from 475,741 edges to 10,631 with the symbol count untouched
throughout: in both, what was produced looked right, and the missing part was
unobservable from inside.

**It was not harmless in the measured case.** Two facts arrived only after the
index was finally asked, both of which the tree had held all along:
`derive_actor` has exactly two call sites, one of which (`src/verify/intake.c:873`)
is an already-reviewed precedent for constructing a synthetic ATLAS-channel op to
derive an actor of a chosen class; and the forgery guard on `verify_actors` —
`CHECK(class NOT IN ('TOOL','TEST','RUNTIME_OBSERVATION','ATLAS_VERIFIER') OR
identity = 'ATLAS_ATTESTED')` — does **not** cover `DOCUMENT`, so a
DOCUMENT/SELF_DECLARED actor is already insertable. The second removed a
migration from A12.1's plan while it was still being written.

**The symmetric gap, stated because A12.1 is next to it and deliberately does
not close it.** A12.1 pins what a *worker was shown*: a Context Pack bound to a
commit, a decision set and a memory generation, frozen per run. Nothing pins
what a *reader consulted*. "This plan was written without consulting the index"
is exactly the sort of absence the season stack exists to make positively
establishable, and today it is not recorded anywhere. The operator's decision on
2026-09-01 was to keep this out of A12.1's scope — the season already carries
nine acceptance items — and to record it here instead.

**Candidate fixes, none implemented.**

1. Widen the plugin's `PreToolUse` matcher so Atlas can *observe* reads. Cheap,
   and it makes the fact recordable rather than merely true.
2. Surface consultation in the context block the `UserPromptSubmit` hook already
   injects — a session that has asked the index nothing would say so, next to
   the change count it already reports. **Whether the daemon currently records
   which read methods a session called is not established here**; it is on the
   path of every MCP call, so the fact is reachable, but this entry does not
   claim the row exists.
3. Let a change reason carry a digest of the index facts its author consulted,
   which is the Context Pack pointed at the reader instead of the worker.

**What must not be done.** Not a reminder that fires on every read — a warning
everyone learns to skip past is the conflict list A9.2.5 warns about, one layer
out. And not a permission gate: refusing a read until the index has been
consulted would block the one operation Atlas guarantees is safe, and it is a
check an adversary — or an impatient operator — walks around in one step.

## The CLI's local `--json` hand-builds a document the daemon's writer already builds (2026-09-01)

Found while fixing an unencoded untrusted field during A12.1, and recorded because
the class outlives the instance.

`src/ipc/server_verify.c:505` answers the socket with
`atlas_service_verify_write_report`, the shared writer in `src/core/service_verify.c`
that encodes every untrusted field at the point of output. The CLI's local `--json`
path does **not** use it for the report: `j_verify` in `src/cli/render_json.c` is 242
lines that build the same document by hand. The same function *does* call
`atlas_service_verify_write_detail` for the detail block, so one JSON document is
half shared and half copied.

**The instance this produced.** `claim_text` existed only in the inline copy, and it
was the one field emitted without `atlas_safe()` — through `atlas_json_key_str`,
which gives A0's structure escaping and nothing more. `src/cli/render_human.c` had
the same field, plus `evidence.observed` and `attestation.actor_name`, printed to a
terminal with a bare `fprintf` under a `"(untrusted project text)"` label. A label is
not an encoding. `include/atlas/safetext.h` names what was uncovered: C0 and C1
control bytes, DEL, line and paragraph separators, bidirectional controls, invalid
UTF-8. Fixed at all four sites in `08eaa75`, with a CLI-level test that submits a
claim carrying U+0085 and U+202E and asserts both surfaces arrive percent-encoded —
proved by reverting the fix and watching it fail with the raw bytes found verbatim.

**Why the class is the finding.** `CLAUDE.md` states that human and JSON output
consume identical service results, and that a missing per-renderer implementation is
how the two drift apart. Here they did not drift by omission but by duplication: two
implementations of one document, and the field that escaped review was precisely the
one that existed in only one of them. Every future field added to the verification
report has to be added twice and encoded twice, and nothing fails when it is not.

**Candidate fix, not implemented.** Have `j_verify` call
`atlas_service_verify_write_report` the way it already calls `..._write_detail`, so
the local and socket paths emit one document from one writer. That is a larger change
than the defect that exposed it and it was deliberately not attempted mid-season;
the usual argument applies — a shared writer is only better if the sharing is
genuinely the same document, and establishing that is the work.

## `verifier_input` is unvalidated at the write point, and A12.1 gave that a new consequence (2026-09-02)

Found while closing A12.1's T7, whose Critical was the same grammar reached from a
different direction, and recorded because the surface is pre-existing while one of its
consequences is not.

**What is verified.** `src/verify/intake.c` copies `verifier_input` from the request
into the stored claim (`:613`) and hashes it into the content key (`:642`). It is
bounded for length and **nothing checks its grammar**. Meanwhile `input_field`
(`src/verify/detverify.c:101`) splits that string on `;` and takes the **first** match
per key. And `op->document_uid` (`include/atlas/verify_ops.h:248`) lets a submitter
bind its claim to an existing knowledge record, resolved at `intake.c:571-575`.

**The chain, written out.** A model reaching `verify.claim_create` supplies both the
`verifier` and the `verifier_input`. It can therefore submit
`path=<a real file>;sha256=<a value it chose>;sha256=<anything>`, and the deterministic
verifier will compare that real file against the chosen value and store a **FAIL**.
Because the same call may bind `document_uid`, the claim can be attached to an
**approved** decision — and with A12.1's T5, `atlas_verify_conflict_settle`'s first
rule then reads `deterministic_fail && decision_bound && decision_effective` and
produces `ATLAS_CONFLICT_IMPLEMENTATION`. A model can thus manufacture *implementation
drift against a decision it did not like*, out of a claim it wrote itself.

**What this is not, stated so the finding is not read as worse than it is.** The
verdict lands on the model's **own** claim; no other claim's aggregate moves. Nothing
transitions a lifecycle on it — `autolifecycle.c`'s Gate 3 keys on `deterministic_fail`
and folds to BLOCKED, which is the conservative direction, and
`ATLAS_CONFLICT_IMPLEMENTATION` has no automatic consumer today. A9.2 already says a
model's submission is an attestation and never authority. So this is a way to put a
misleading row on a surface an operator reads, not a way to move a decision.

**Why it is worth an entry anyway.** The surface has been open since A9.2.1 and cost
nothing while the conflict axis had no producer — every claim reported `CONFLICT_NONE`
regardless. T5 gave the axis its first producer, so a stored row that reads
`IMPLEMENTATION` is now a thing an operator can be shown, and its most direct producer
is an unvalidated request field. **A pre-existing surface whose consequence is new is
exactly the shape that outlives the season that created it**, which is why it is here
rather than in a task's report.

**Candidate fixes, none implemented.** Validate `verifier_input` at the write point
against the grammar its consumers parse — the check A12.1's extractor now performs for
itself (`src/memory/extract.c`, refusing rather than filtering, with the reason
recorded on the proposition). Or give the verifier input a structured representation
that has no in-band separator to confuse. The second is larger and closes the class;
the first closes the instance and matches what the tree already does elsewhere.

**Related and separate:** the length bound that actually bites a CONTENT_HASH input is
`arg_a[512]` in `detverify.c`, refused rather than truncated, while the extractor
checks against `ATLAS_VERIFY_VERIFIER_INPUT_MAX` (2048). A path of roughly 500 to 1970
bytes therefore passes every check, is stored, and is permanently UNAVAILABLE with
nothing recording that its length was the cause.

## The yield grace is argued against a deadline the caller does not wait for (2026-09-02)

Found while measuring A12.1's reconciliation pass against the writer thread, and
recorded because it is A9.2.7's reasoning rather than that season's code.

`ATLAS_WRITER_YIELD_GRACE_MS` is 2000 ms, and `include/atlas/limits.h:90-102` argues
the value: it must sit between the gap between two yield points and "the smallest
synchronous deadline on the writer path", which the comment names as "a hook's
`AI_WRITE_TIMEOUT_MS` of 4000 ms" — concluding that "a waiter that spends its whole
grace still backs out with time left to report the refusal rather than timing out."

**The hook does not wait 4000 ms.** `ATLAS_HOOK_IPC_TIMEOUT_MS` (`limits.h:413`) is
**2000 ms**, and its own comment says what happens at that point: the hook gives up and
**fails open**, which for a hook means its write is lost rather than refused.
`AI_WRITE_TIMEOUT_MS` (`src/ipc/server_ai.c:34`) is the *daemon-side* deadline on the
same request, reached at `:425` and `:483`.

So the two figures are 2000 and 2000. A waiter that spends its whole grace reports
`BUSY` at the same instant the hook has already gone, and the refusal the grace exists
to deliver races the caller that was supposed to receive it.

**The nuance, stated so this is not read as worse than it is.** The comment is true
*about the writer path*: from inside the daemon, 4000 ms is the deadline it knows, and
the hook's own give-up is outside its view. What reaches past the writer path is the
comment's **conclusion** — "with time left to report the refusal" — because the thing
that receives the refusal is the hook, and it is not there. A9.2.7 draws a careful
distinction between backing out and timing out, on the ground that the two answers mean
opposite things about whether the write is still on its way; that distinction is only
worth what the caller can observe.

**How it surfaced.** A12.1's reconciliation pass was measured at **2429.9 ms** for its
worst case at the compiled ceiling — 16 sources, 128 candidates each, 2048 claims, one
transaction. That exceeds both figures, which is what made the question worth asking at
all: while every unbounded job on that thread yielded within milliseconds, nothing had
tested the boundary.

**Where the sentence lives, because a fix that lands on one of them leaves six.** The
claim was recorded above against `limits.h:95` alone, which understates it. It has
**seven** homes in first-party text, four of them repeating the misattribution verbatim
and three of them in code comments a reader reaches without opening a document:

- `include/atlas/limits.h:95` — "a hook's `AI_WRITE_TIMEOUT_MS` of 4000 ms"
- `src/daemon/writer.c:303` and `:456` — "A hook's session write ... sits out its whole
  four seconds and *then* fails"
- `src/daemon/writer.c:396` — the `ATLAS_JOB_AI` drainable case: "a hook, which has four
  seconds and fails open"
- `docs/engineering-rules.md:2535` — the rule's own statement of the derivation
- `docs/extending.md:740` — the checklist consulted *before moving the grace*, which is
  the one place the wrong figure would be read at the moment it decides something
- `docs/roadmap.md:550` — "**Every** synchronous deadline on the writer path is at least
  4000 ms"

**The roadmap's form is the one that is false rather than merely misattributed.**
`ATLAS_HOOK_TEARDOWN_TIMEOUT_MS` is **700 ms** (`limits.h:416`): a `SessionEnd` hook
gives up at 700 ms, so it does not merely race the back-out — it is gone before a third
of the grace has elapsed, on every refusal, always. Nothing in that path is at least
4000 ms.

**What is actually wrong is an attribution, not a number.** Every *daemon-side* deadline
on the writer path is indeed ≥ 4000 ms — 4000 (`ai.*`), 5000 (`decision`), 10000 (one
decision path via `ATLAS_IPC_WRITE_TIMEOUT_MS`), 15000 (`verify`), 30000 (`ai.register`)
— so each sentence is true of the side it was written from. `AI_WRITE_TIMEOUT_MS` is not
"a hook's" anything; it is the daemon's wait on its own writer. Naming it as the hook's
is what carries the true half across the socket and makes the conclusion false, because
the entity that must receive the refusal is on the other side.

**Candidate fixes, none implemented.** In all seven homes, say which side each figure is
on: `AI_WRITE_TIMEOUT_MS` (4000 ms) is the daemon's wait on the writer;
`ATLAS_HOOK_IPC_TIMEOUT_MS` (2000 ms, and `ATLAS_HOOK_TEARDOWN_TIMEOUT_MS` 700 ms at
`SessionEnd`) is the hook's wait on the daemon; and the grace must sit below the smaller
of the two for a back-out to reach the caller that asked for it. Then re-derive whether
2000 is still the right grace — the honest answer may be that it must be meaningfully
below the hook's give-up rather than equal to it, and that no single value clears 700 ms
and the yield-point gap at once, which would make the residual a stated cost rather than
a solved problem. Separately, A12.1's T10 must classify its reconciliation job against
the *measured* 2429.9 ms rather than against any of these comments.

## `verify_claims.superseded_by_claim_id` is read, filtered three times, and never written (2026-09-02)

Found by A12.1's T9 while deriving a `SUPERSEDED` diff kind, and verified. This is the
**second** column of this exact shape the season has turned up, after
`atlas_verify_aggregate.conflict`.

The column exists in the schema with `DEFAULT 0` (`src/db/migrate.c:2774`), is read into
`atlas_verify_claim` (`src/db/db_verify.c:357`), appears in the select list (`:369`), and
is filtered `= 0` at **three** read sites — `:432`, `:730`, and `:2235`. Nothing anywhere
in `src/` writes it: there is no `UPDATE` and no `INSERT` that sets it to a non-zero
value.

So every claim in every Atlas deployment is permanently live, the three filters exclude
nothing, and `include/atlas/verify.h:725`'s "0 when live" documents a state nothing can
produce. **The capability to supersede a claim does not exist**, while three queries are
written as though it does.

**Why this is not merely tidy-up.** `verify.h`'s own claim contract says a claim "has no
lifecycle, no approval and no revisions: it says one thing, it is bound to the artifacts
that make it checkable, and evidence accumulates against it" — and supersession is how a
proposition that has been replaced stops competing with the one that replaced it.
Without a writer, an old claim and its replacement both stay live and both keep
attracting evidence, which is precisely the "two readings of one document
indistinguishable" problem the A9.2 line of seasons exists to end. A12.1 makes it
reachable in ordinary use: a memory file's sentence edited in place produces a second
claim, and nothing marks the first as replaced.

**Same class as the conflict axis, and worth naming as a class.** Both are columns that
are stored, read, filtered or rendered, documented as meaningful, and never produced.
Both cost nothing while unproduced and become wrong the moment a consumer believes them.
The general check is cheap and this season has now run it twice by accident: for each
column a read path filters on, ask what writes it.

**Candidate fixes, none implemented.** Give the column a writer at the one verification
write point, so supersession is an intake operation like every other claim fact — that is
the shape `atlas_verify_intake_apply_in_tx` already enforces for everything else. Or
remove the column and its three filters, and say in the header that a claim is never
superseded, which is what the code actually does today. The first is more work and
matches the header's stated intent; the second is honest about the present. **What must
not happen is a third season reading it as meaningful.**

## `atlas_db_rollback` is not depth-aware while `begin` and `commit` are (2026-09-02)

Found while A12.1's T9 added the first in-transaction `EVALUATE` in Atlas, and verified
by reading all three functions.

`atlas_db_begin` counts nesting — `tx_depth > 0` increments rather than issuing a second
`BEGIN` (`src/db/db.c:493-499`). `atlas_db_commit` mirrors it: at depth greater than one
it decrements and returns OK, and only the outermost call issues `COMMIT` (`:505-514`).
**`atlas_db_rollback` does neither.** At any depth it issues one `ROLLBACK` and sets
`tx_depth = 0` (`:540-546`).

So an inner caller that rolls back destroys the outer transaction as well. The outer
caller's later commit then finds `tx_depth == 0` and returns `ATLAS_ERR_INTERNAL`,
"commit without an open transaction" — **fail-closed, and nothing believes it committed**,
which is the important half and is why this is a design asymmetry rather than a
corruption bug.

**What it costs, concretely, and why A12.1 is where it shows.** T8 spent a fix round
giving the reconciliation pass per-source isolation: each source runs inside a named SQL
`SAVEPOINT` so one poisoned source rolls back alone and the other fifteen survive. T9
then added the first in-transaction `EVALUATE`, which reaches
`atlas_verify_autolifecycle_run` — and that function has seven rollback sites. Any of
them firing discards the **whole pass**, not the one source, and the savepoint rollback
that follows then fails naming a savepoint whose transaction is already gone. The
isolation is still in the code and no longer does anything on that path.

**Why it was not fixed in the season that found it.** It is cross-cutting: every caller
that nests transactions is affected, not only the memory pass, and `atlas_db_rollback`'s
current shape may well be deliberate for callers that want a hard reset. Deciding that
needs a wider look than a task with four pinned files can give it, and T9 reported it
rather than reaching outside its scope — which was the right call.

**Candidate fixes, none implemented.** Make rollback depth-aware to match its siblings,
so an inner rollback unwinds one level and only the outermost issues `ROLLBACK` — and
then decide what an inner rollback *means*, since a partial rollback is not what most of
today's seven autolifecycle sites intend. Or keep the hard reset and give it a name that
says so, adding a separate depth-aware form for callers that nest. The first is more
faithful to `begin`/`commit`; the second is more honest about what today's callers want.
**What must not happen is a caller building isolation on top of it without knowing**,
which is exactly what T8 did.

## A `REPO_DIR` memory source records which bytes were read and never which file they came from (2026-09-02)

Found by A12.1's T9 while withdrawing a safety argument that rested on counting rows, and
verified in the schema. It is the same root cause as that argument's failure, one layer
down.

**`memory_source_versions` has no `rel_path` column** — `src/db/migrate.c:4347-4367`. A
version row carries `source_id`, `content_sha256`, `content_bytes`, the git binding where
there is one, and the bytes otherwise. Its uniqueness is
`UNIQUE(source_id, content_sha256, observed_at)`. For a `REPO_FILE` source that is
complete, because the source *is* the file. For a `REPO_DIR` source, whose children
`src/memory/read.c` lists and reads one by one, the file each version came from is not
recorded anywhere: `memory_unanchored` references `source_version_id`
(`migrate.c:4414-4415`), so a proposition can be traced back to bytes and stops there.

**Three consequences follow, and each is checkable rather than argued.**

1. **Two children with identical content are one version row.** The uniqueness key has no
   path in it, so a directory of N files holds at most as many versions as it has distinct
   contents. Two empty `.md` files are one row.
2. **`atlas_db_memory_dir_hash_mismatch` cannot see a change that permutes content among
   children.** It asks, per indexed row, whether *some* version exists for that row's
   content hash (`atlas_db_memory_version_exists`, `src/db/db_memory.c:256-293`, matching
   `(source_id, content_sha256)`). Swap the contents of two children and every hash is
   still present, so `changed_out` is false while the directory has genuinely changed.
3. **The same keying is why that function's `LIMIT` cannot be argued safe by counting.**
   A12.1's round 4 wrote a pigeonhole argument at the `LIMIT` — at most 64 rows are ever
   versioned, so a 65th must be unversioned — and it fails on this and on the version
   table being cumulative. `cp a.md a-copy.md` in a 64-file source is the whole repro. The
   argument was withdrawn in round 5; the keying it rested on is this entry.

**What is not established, and must not be assumed either way.** Whether this is a defect
or the design working is genuinely open. The deliberate reading is defensible — a memory
source is a body of knowledge, and the same sentence in two files is one fact, which is
exactly the posture `verify_claims`' content key already takes for one proposition stated
twice. The reading that makes it a defect is that `read.c` lists children individually,
bounds them individually (`ATLAS_MEMORY_MAX_DIR_ENTRIES`), and reports obstacles per item,
so every *other* layer treats a child as a thing — and provenance that stops at bytes
cannot answer "which file said this", which is the first question anybody asks of a claim
they disagree with.

**Candidate fixes, none implemented.** If a child is meant to be identifiable: add
`rel_path` to `memory_source_versions` and to its uniqueness, which is a migration and
changes what a re-read of an unchanged directory costs. If it is not: say so at the table,
because three call sites currently read as though a version had a file. Either way,
consequence 2 should be stated where `dir_hash_mismatch` is documented rather than left
for the next person to construct.

## Nothing prunes a claim's anchors, so `memory_claim_anchors` only grows (2026-09-02)

Found by A12.1's T12 fix re-review while correcting a mechanism three of us had asserted
wrongly, and verified in the tree.

**The only deleter is `atlas_db_memory_anchor_prune_one`** (`src/db/db_memory.c:642`), it
has **one caller** (`src/memory/reconcile.c:864`), and that caller prunes only the
*predecessor* claim's anchors when a proposition is re-minted onto a new uid. Nothing
prunes the anchors of a claim that keeps its uid.

**So anchors accumulate by union across passes.** A claim's uid is stable while its
proposition's identity holds; its text can change from pass to pass and resolve to
different anchors; `UNIQUE(claim_uid, kind, value)` (`src/db/migrate.c:4376`) deduplicates
identical tuples but admits every new one. Over a repository's life the set for one
long-lived claim is monotonically increasing, and the vanished-anchor sweep — which
*reports* on anchors that no longer resolve — deletes none of them.

**How it surfaced, and the correction it forced.** A12.1's T12 review found that
`anchor_collect_cb` dropped a claim's anchors past a bound while calling the path
unreachable, and explained the reachability through document merging: O10's content key
omits the actor, so two documents stating one proposition resolve to one claim, and the
anchor write is not gated on the claim being new. **That mechanism is wrong.**
`atlas_memory_anchor_resolve` (`src/memory/extract.c:426`) resolves from the proposition's
text alone — nothing document-relative reaches it — so two documents stating the same
proposition produce *identical* tuples, which the UNIQUE constraint collapses. Two
documents can never exceed one document's bound. The reviewer, the fix, and the
controller's own ledger and commit message all carried the same leap; the accumulation
route is the real one.

**The consequence that was understated.** A12.1 now refuses a pack build when a claim
exceeds `ATLAS_MEMORY_PACK_MAX_ANCHORS_PER_CLAIM`, and the refusal is repository-wide and
task-independent. It was documented as lasting "until the vanished-anchor sweep prunes a
row" — **the sweep prunes nothing**, so there is no exit: once one long-lived claim
crosses the bound, that repository has no working pack until somebody deletes rows by hand
or the claim is re-minted onto a new uid.

**Candidate fixes, none implemented.** Give the anchor set a lifecycle: prune tuples a
pass no longer resolves for a claim it kept, which is the same "carry forward what is
still true" shape the generation logic already has, and would make the bound a bound on a
*current* set rather than on a repository's history. Alternatively bound the pack's read
rather than the claim's storage, so an over-anchored claim costs one truncated claim —
reported, never silent — instead of every pack in the repository. Either way the "no exit"
property must stop being true before A12.1 can call the refusal a bound rather than a
trap.

## A10.1's memory package reached one of the two attempt paths (2026-09-02)

Found by A12.1's T13 while wiring its own composer into both call sites, and verified
against the tree at `36b7509`: `src/orch/dispatch.c` contained **no call to
`atlas_orch_memory_compose` at all**, so a workspace attempt's task was handed to a worker
exactly as submitted while a run driver's jobs received the package.

**Why nothing caught it.** A10.1's own A/B experiment ran through the run driver, which
does inject, so the measured verdict — `USEFUL` on worker duration and turns, not on cost —
is about the path that worked and stands unaffected. The season's rules are about what the
package *contains* and how it is *selected*; none of them asserts that every path a job
can take injects it. And `OFF` appending nothing is indistinguishable, from outside, from a
path that appends nothing because it never asks.

**What it cost.** A workspace-attempt job in a run carrying a frozen manifest saw none of
it, silently — the manifest was frozen, stored, and never delivered. `orch_runs`' memory
rows for such runs describe a package no worker read.

**Fixed in passing rather than left**, because A12.1's T13 had to put its own composer at
both sites and the missing A10.1 call was in the way: both paths now go through
`atlas_memory_pack_compose`, which composes the task, the optional A10.1 package and the
optional context pack. What remains for a reader of this entry is that **the fix's
correctness now rests on there being exactly two call sites**, and nothing asserts that
number — a third attempt path added later would repeat this defect exactly.

**Candidate follow-up, not implemented.** Make the composer the only way a task reaches a
worker, structurally rather than by convention — the shape `atlas_decision_apply_in_tx`
has, where the write point is one function with a counted set of callers and a test that
compares the count. Today the property is stated in a comment at both sites and checked by
nothing.

## `memory pack`/`diff`/`patch`/`trailer` have no remote form (2026-09-03)

Found by A12.1's T16 while wiring the `atlas memory` CLI family. Every other read-only
command family in this tree has both a local form (`atlas_ctx_db(ctx)`) and a `_remote`
twin served over the socket — `code`, `decision`, `sem`, `gate`, `context build`, and now
`memory status` — because under A7.1 the index is 0700 `atlasd` and an operator's own
account has no local handle at all. `memory status`, `scan` and `reconcile` have this: the
first reads through T11's existing `memory.status` operator method when `ctx` is NULL, and
the other two are unconditionally daemon-served already (T11's own design — the writer-
thread job queue `memory.put`/`memory.reconcile` submit to has no local equivalent to fall
back to).

`memory pack`, `memory diff`, `memory patch` and `memory trailer` do not: they read
`memory_context_packs`, `memory_claim_diffs`, `memory_sources` and `memory_trailer_bindings`
through `atlas_ctx_db(ctx)` only, and refuse with a stated reason
(`src/core/service_memory.c`'s `MEMORY_NO_LOCAL_HANDLE`) when `ctx` is NULL. Under
`index_is_foreign` (`src/cli/cli.c`) they are absent from `remote_serves()`, so they fall
through to the pre-existing generic refusal rather than the built binary's
`memory_item`/NULL-`ctx` crash the vtbl's own comment warns a new command's incomplete
sibling would cause — the failure mode is a clean refusal, not a defect, but it is the
first family in this tree where four of seven forms carry the gap the others do not.

**Why it was not closed in T16.** Closing it needs four new RPC methods
(`memory.pack`/`memory.diff`/`memory.patch`/`memory.trailer`) in the operator group beside
`memory.status`, in `src/ipc/server_memory.c` — a T11 file T16's own plan section did not
list for modification, and new operator-uid surface that deserves its own review rather
than arriving as a side effect of a CLI task. Recorded here rather than worked around.

**Candidate follow-up, not implemented.** Add the four methods, following `memory.status`'s
own shape: read-only, no job queue, answered synchronously off the daemon's own db handle.
`atlas_memory_pack_build` and `atlas_memory_patch_build` both read files and must not run
inside a transaction (their own header comments say so), which the daemon's ordinary
dispatch path already respects for other reads — nothing about that constraint is new to a
socket-served form.

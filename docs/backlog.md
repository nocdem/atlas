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

Until then the honest statement is the one `docs/semantic-discovery.md` makes: a
first automatic build of a large repository will make the daemon unresponsive to
*writes* for its duration, reads are unaffected, and an operator who cannot
accept that sets `semantic_auto_default = DISABLED` or disables the repository.

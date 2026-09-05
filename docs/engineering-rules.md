# Atlas — the layer map and the season rules

This is where the per-season **layer maps** and **"these are not negotiable"
rules** live. They were in `CLAUDE.md` until A9.2.4 moved them, for one reason
that is worth stating rather than leaving to be inferred:

`CLAUDE.md` is loaded into a coding agent's context on **every session, before
anything is read**. It had grown to 175 KB, past the limit at which it is loaded
at all — so a file whose whole purpose is to be read first had stopped being
read. What belongs there is what must apply from the first turn: the hard rules,
the architecture invariants, the concurrency and safety contracts, and the
pointers to everything else. What belongs *here* is the reasoning behind each
rule, which is what a reader needs at the moment they are about to change the
code the rule is about — and not a moment earlier.

**Nothing was deleted.** Every rule below is the text that was in `CLAUDE.md`,
verbatim, and `CLAUDE.md` carries a one-line statement of each operative rule
with a pointer here. If the two ever disagree, this file is the longer statement
and `CLAUDE.md` is the one that has to be corrected — a rule that cannot be
stated in a line is a rule nobody will remember under pressure.

Read the section for the layer you are about to change. The rules are cumulative:
a later season's rules do not repeal an earlier season's unless they say so in
those words, and every deliberate reversal in Atlas is written down as one.

## Reporting rules — these are not negotiable

These two are older than any season and apply to every one of them, which is why
they sit above the season sections rather than inside one. They govern how a
finding reaches a reader: a question, a report, a commit message, a note file, a
code comment. They are not style preferences. Each exists because breaking it
produced a **wrong answer that survived review**, and each was measured on this
project rather than reasoned about.

### A LABEL YOU COINED IS NOT VOCABULARY

**Never ask a question, or state a finding, in an identifier you invented.**
`RC-A`, `F-10`, `DG-3`, `Faz 4b` are search keys — useful for finding a passage
again, worthless for communicating one. The reader has no way to decode such a
label and, more importantly, **no way to check it**: a sentence whose subject is
an opaque token cannot be disagreed with, so it passes review by being
unreviewable.

A label may *index* an explanation. It may never *replace* one. The first time it
appears in any message, the thing it names is stated in full, in the reader's
terms. After that it is shorthand for something the reader now holds.

**A citation is not an explanation either.** "See `docs/watcher-consistency.md`"
names where an argument lives; it does not make the argument. A reader who has to
go and find it has been handed a lookup instead of an answer, and the writer has
been spared the one step — restating the claim in their own words — where an
error in it would have shown. Cite *after* explaining, as the source, never
instead of explaining.

**Measured.** In the 2026-08-28 session the label `RC-A` was defined once and
then used a dozen times across reports and questions — "RC-A kapandı", "RC-A'nın
nüksü değil", "bugs.md 9", "#8". The operator read *that something closed* and
could not read *what*. The finding underneath was correct; the reporting made it
uncheckable, which for a reader is the same as not having it. The remedy is
mechanical: state the mechanism, then the tag.

### EVERY EVENT GETS ITS CAUSAL CHAIN, WRITTEN OUT

**No event happens without a cause, so the cause chain is written.** Not to
lengthen a report — length is a cost, and this rule pays it for one reason:
**the chain is the instrument that catches the writer's own error.** Prose can
assert an outcome with no mechanism behind it and read perfectly well. A chain
cannot: every link has to name a thing that acts on the next one, and a missing
link is visible as a gap rather than hidden as a smooth sentence.

From which the operative half follows: **if you cannot write the cause, the
scenario is not real.** An unwritten chain is not a chain the writer has and
chose to omit; it is a chain the writer does not have. Reporting such a scenario
as real is the defect — not the omission of detail, but the claim itself. Write
the chain first, then decide whether the finding survives it. Findings that do
not survive are the point of the exercise.

This is the same discipline the A9.2.2 rules apply to absence, one level up: **no
evidence of a mechanism is not evidence of a mechanism.** A verdict with no chain
under it is an inference presented as an observation.

**Measured, twice, in the 2026-08-28 session.**

A fix to `/opt/dna/.gitignore` was reported as having "broken the root cause at
its source". The measurement behind that was sound — 75,887 untracked files fell
to 275, and a `git ls-files --others` that had been exceeding a 60-second timeout
returned in 12 ms. The *permanence* half was asserted without a chain. Writing
one would have taken four links — `.gitignore` is a tracked file → the repository
had concurrent uncommitted work → an ordinary `git checkout` restores tracked
files → an uncommitted rule does not survive one — and the fourth link is the
refutation. It was not written, so the claim shipped. Twenty minutes later the
rule was gone and a scanner pass mirrored the full 98,992 files again. The
measurement was right; the unwritten chain is what made the conclusion wrong.

The same session's first deploy script asserted a failed installation from a
digest mismatch. The chain was never written, because writing it would have
required naming what `cmake --install` does to a binary — it strips the RPATH —
at which point the mismatch is the expected result of a *successful* install. The
script reported a failure that had not happened, and the operator would have
started debugging a working system.

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
  SHA-256'd, **word-split without a shell**, and otherwise discarded — never
  executed, never passed to a shell, never stored verbatim. Arguments from
  either form are read through one positive allowlist (include dirs, system
  include dirs, defines and undefines, the standard, the source, the output) and
  nothing else; `@response-files` and `-fplugin=` are recognised only well enough
  to be ignored.

  **A8-CI reversed A3's refusal to read the `command` string at all.** A3
  declined on the grounds that splitting it would be "the beginning of
  interpreting a command line". The reasoning was sound and the consequence was
  not: CMake writes the string form, so Atlas could not read the compilation
  database of most real repositories — it extracted nothing, every translation
  unit parsed without its include paths, and the result was an index that looked
  built and described almost nothing.

  What happens now is **word splitting and nothing else**: backslash, single
  quotes and double quotes, and no expansion of any kind — no variables, no
  command substitution, no globbing, no tilde. `$(id)` becomes six ordinary
  characters inside one argument, which is then dropped for not being on the
  allowlist. Both forms feed the *same* allowlist, so the string form reaches
  nothing the array form could not. The cost is stated in
  `tests/test_code_compdb.c`: a `-DSECRET=` value is now recorded from a command
  string exactly as it always was from an `arguments` array, because Atlas
  cannot tell a secret define from an ordinary one. An include directory outside the repository is recorded with
  `external = 1` and **never opened**. `tests/test_code_compdb.c` plants an
  executable marker in four places and asserts it never ran.
- **A3 adds no dependency, and its lexer never will.** No tree-sitter, no
  ctags, no Python, no Node. The lexer is first-party and bounded, and every
  ceiling it reaches is reported rather than silently applied.

  **A8-CI reversed the "no libclang, ever" half of this rule, deliberately, and
  the reversal is confined to a separate layer.** A3 still reads bytes with its
  own lexer and still produces its own resolution classes; nothing about it
  changed. What A8-CI added is a *second* layer beside it that asks a compiler,
  in `src/sem/`, linking libclang from exactly one translation unit
  (`src/sem/clangparse.c`). The two never merge and neither promotes the other:
  a UNIQUE_LEXICAL edge stays lexical even when Clang proves the same call,
  because the bytes that produced it did not become more true. See the A8-CI
  rules below.
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
  `repo` argument must name a repository **the persistent registry holds** — a
  whitelist, not a path comparison.

  **This reverses A2's original rule, which made the client's granted roots the
  allowlist, and the reason it was wrong is worth keeping.** A root is *where
  the client happens to be looking*; it says nothing about what an operator has
  authorised Atlas to hold. Coupling them meant that starting a session inside
  one registered repository made every other registered repository unreadable —
  which protected nothing, because an operator had already registered both and
  the model reached the second one simply by being started somewhere else.

  What constrains the call is unchanged and lives elsewhere: only an operator
  registers a repository; `repo.add`, `repo.ensure` and `repo.remove` do not
  exist as RPC methods at all; no tool accepts a path; and a name must match a
  registered repository *exactly* or the answer is `NOT_REGISTERED` — the same
  answer whether the directory exists, is a git repository, or is nothing.
  Atlas does not look at the filesystem to produce it.

  Roots keep one honest job: choosing a *default* when the caller names no
  repository. `tests/test_registry.c` pins all of this, including the case the
  old rule got wrong — a session granting one repository and asking about
  another must answer.
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

- **Restore is a local CLI operation with no RPC method, and nothing in the
  ordinary group creates, reads, restores or prunes.**

  **A7.1 broke this rule's premise twice, and both are now corrected in
  writing.** A5 reasoned that the uid owning the index could copy or prune the
  file anyway, so an RPC method added nothing. Under a system deployment the
  index is `0700 atlasd` and the operator is a different uid — so the operator
  account could not back up, verify, plan or prune its own index at all, and the
  documented remedy was to become the service account. That is manual
  impersonation standing in for a missing feature. `backup.create`,
  `backup.verify`, `maintenance.plan` and `maintenance.prune` are served in the
  **operator-uid** group, gated on `SO_PEERCRED` against the root-owned policy;
  every other peer, including `atlas-worker` and every MCP client, is told the
  method does not exist. `backup.restore` deliberately has no RPC form: replacing
  the record should require stopping the daemon.

  What A5 actually wanted is untouched — nothing a model can reach may replace or
  prune the index — and so is everything else: `--apply` is required, the delete
  is per batch, bounds are checked rather than clamped, and there is no
  background deleter. The prune runs *on* the writer thread rather than taking
  the lock, so "Atlas has exactly one writer" is still true of it.

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
- **Exactly two tables are prunable, and widening that needs an argument.**
  `repo_events` since A5, because it already carried a documented per-repository ceiling,
  its `id` is `AUTOINCREMENT` so no cursor can be re-pointed by a deletion, and
  the durable evidence lives elsewhere. `gw_audit` since A9, because nothing holds
  a rowid into it, its `id` is `AUTOINCREMENT` so a deleted row can never hand its
  id to a later one, and every fact a request produced — a decision, a revision, a
  job, a reason — lives in a canonical table that is not prunable. `scans` is
  *not* prunable and the reason is A4's: `files.first_seen_scan_id` and friends
  hold `scans.id`, a plain rowid SQLite reuses. Derived tables are not prunable by age either — a half-aged
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
- **No MCP tool, hook or plugin command mints or spends a lifecycle capability,
  and no RPC method changes the registry.** `repo.add`, `repo.ensure` and
  `repo.remove` were **deleted**, not left refusing — an absent method is
  answered by the dispatcher's unknown-method case, and a refusing one is a
  refusal a later edit can weaken. `tests/test_a7_authority.c` asks a live
  daemon for the names, including case variants and aliases, and requires every
  one to answer `unknown method` rather than merely to fail.

  **The five operator-channel methods are the one deliberate reversal, and it
  must be described as one.** `decision.challenge`, `decision.approve`,
  `decision.reject`, `decision.supersede` and `decision.revalidate` were deleted
  by A7 and are back, in a group offered only to the peer whose `SO_PEERCRED`
  uid equals the `operator_uid` in the root-owned policy — checked by the same
  probe the local path runs, so a policy that is missing, symlinked,
  group-writable or not root-owned, or a daemon running from a replaceable
  binary, locks the group exactly as it locks the local channel. Every other
  peer gets `unknown method`.

  A7's defect was not that the methods existed: it was that `decision.challenge`
  took no capability and asked for no terminal — it *was* the capability source
  — so any process able to open the socket could mint one. The identity now
  comes from the kernel and the constraint from a root-owned file, and a uid
  written into a request reaches the decision at no point.

  What this bought and what it cost, both stated: the deployment's human
  operator is a different uid from the account that owns the index, and giving
  that account the index was not acceptable, so the choice was between an
  operator who cannot approve and an operator the kernel identifies over the
  socket. **It does not distinguish a person from a program running as that
  uid.** Nothing can — `tests/test_decision_operator.c` has demonstrated that
  since A4 by allocating a pty and typing into it. A model with a shell as the
  operator's account reaches these methods exactly as a human does; that it must
  not is an orchestration rule, not something the kernel enforces here. Every
  honesty limit A4 states about `LOCAL_OPERATOR_CONFIRMED` holds word for word.
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

- **The operator account and root are trusted by design and Atlas does not defend against
  them.** The operator holds passwordless root; any process intentionally
  launched as that account — including an AI session — is outside Atlas' OS
  isolation guarantee, by the operator's explicit decision. **Never write a test
  asserting the operator account cannot do something**, and never claim in prose that it is
  constrained. The adversary is `atlas-worker`.
- **Every persistent or autonomous model process runs as `atlas-worker`, never
  as the operator account, unless a root-owned policy names an exception.** That is the
  architectural commitment the separation rests on, and A8's worker dispatcher
  inherits it unchanged.

  **A8.1 is the one configured exception, and it must be described as one.** A
  second dispatcher — `model_dispatcher_uid` in `/etc/atlas/orchestration.conf`
  — may run drivers that need a live model as the operator's own account,
  because Claude Code authenticates with a session that lives in a person's home
  directory and Atlas must not copy, read or relocate one. What it costs is
  stated plainly: a job that dispatcher runs holds the operator's filesystem
  authority, not `atlas-worker`'s, so for those jobs A7.1's OS isolation does
  not apply. Everything else about the job — record, lease, bounds, snapshot,
  ledger — is unchanged. Absent that key, which is the default, A8 is exactly as
  it was. Never widen the exception to drivers that do not need a model, and
  never let it reach the lifecycle: `atlas_authority_probe` is untouched and
  A8.1 mints no capability.
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
  shell, never recursively deletes or chowns, and never names an indexed
  repository except to read.** Dry-run is the default and `--apply` is a
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

## A8-CI layers — additions

```
src/sem       sem.c (the evidence vocabulary and the configuration digest),
              clangparse.c (the libclang reader — the ONE file that includes
              clang-c), parse.c (spawning and reading the bounded child),
              index.c (generations, incremental, publication),
              query.c (the bounded walk and the trace),
              context.c (impact, candidate tests, the task context package)
src/db        db_sem.c (the one write point over the migration-11 tables)
src/core      service_sem.c (the `code` semantic commands and `context build`)
src/ipc       server_sem.c (six reads; no method builds an index)
src/daemon    ops.c (long operations: accepted, polled, terminal for ever)
```

libclang is a **system dependency, located not downloaded**, and optional: a
build without it still indexes, answers decisions and serves every A0..A8
command, and every semantic entry point reports the absence rather than
returning an empty result. `atlas_sem_available()` is the one place that says
so.

## A8-CI rules — these are not negotiable

- **PROVEN means the compiler proved it, and nothing else earns the word.** A
  direct call to a named function is PROVEN. A call through a function pointer
  is CANDIDATE at best and is capped at CANDIDATE by
  `atlas_sem_edge_kind_max_evidence`, which is asked at the write point rather
  than trusted from the extractor — so a bug in the parser cannot mint a proven
  indirect call. **Atlas never claims to know every target of a function
  pointer.** C has no such property without whole-program analysis this season
  excludes, and every traversal that crosses an indirect call says so.
- **A path is as strong as its weakest edge.** `atlas_sem_evidence_weaker` folds
  the class along a walk and is the only place a reached node's evidence is
  decided. A chain crossing one indirect call is a candidate chain however many
  proven edges surround it. UNKNOWN is zero, for the reason A6 keeps UNKNOWN and
  BLOCKED there.
- **A3's facts and A8-CI's facts are never merged and never promoted.** A
  UNIQUE_LEXICAL edge stays lexical even when Clang proves the same call. They
  live in different tables and a query that reports both says which is which.
- **Identity is Clang's USR.** Atlas invents no mangling. A USR already
  distinguishes two files' `static void helper(void)`, two scopes' `i`, and a
  struct from a typedef of the same name. A declaration and its definition share
  a USR — correctly, they are one entity — so decl-versus-def is a property of
  the row, and the relationship is answered by the rows that share a USR.
- **Parsing happens in a bounded child process, never on the writer thread.**
  It is this same binary re-executed through `atlas_proc_run` — still the one
  process-creation path — with an empty environment, an `RLIMIT_AS` ceiling, a
  wall clock and an idle bound. A malformed input that crashes a compiler front
  end costs one translation unit, not the daemon that owns the index. The child
  holds no database handle and takes no lock; `atlas sem-parse` is dispatched
  from raw argv before any `atlas_ctx` exists, and that absence is the guarantee.
- **Compiler diagnostics are counted, never reproduced.** A diagnostic quotes
  untrusted repository source. The count is the fact; the text is not Atlas' to
  relay, and the child's stderr is captured and discarded.
- **Compilation databases are named, never discovered.** Atlas does not search a
  repository for a file that tells it how to compile things. `--compdb` is
  explicit, repository-relative, and validated inside the root.
- **Publication is one statement.** A generation's rows are written while the
  previous generation is still being served, and `atlas_db_sem_publish` marks it
  COMPLETE and repoints `sem_current` in one transaction. There is no path that
  makes a partially written generation visible, and a crash leaves a RUNNING
  generation nobody points at.
- **The input digest is sealed once, at the end of a pass, over the finished
  generation.** A unit's digest covers its *transitive* include closure, and
  that closure is assembled from rows every unit contributes — so computing it
  when the unit was parsed measured whatever had been recorded by then and made
  the digest depend on processing order. Incremental indexing then never
  converged: a fixed set of units was reparsed for ever. The closure walk is
  `WITH RECURSIVE` bounded by `ATLAS_SEM_MAX_INCLUDE_DEPTH`; a two-level walk
  would miss a header four levels down and carry a stale unit forward while
  reporting it COMPLETE.
- **Freshness is recomputed on every read, never cached** — A6's rule about
  freshness and A4's about link currency. ABSENT and STALE are different
  answers and stay different: "nobody has indexed this" and "what was indexed no
  longer describes the code" call for different actions.
- **Every bound that is reached is reported.** A truncated walk cannot say it
  found nothing, an ambiguity never looks smaller than it is (`candidate_total`
  records the true number even when fewer were kept), and an index that
  describes nine tenths of a repository is never displayed the way one
  describing all of it is — COMPLETE, PARTIAL, FAILED and UNSUPPORTED are
  reported separately and never summed.
- **Every selected item says how it was found.** Impact and context items carry
  an evidence class *and* a fixed selection reason from a closed vocabulary. A
  test found by reading a filename is LEXICAL however useful it turns out to be;
  a test that references the subject is PROVEN about the reference and lexical
  only about being a test. C offers no proven test-to-code relationship and
  Atlas does not invent one.
- **The context builder is deterministic and reads only.** Ranking uses counted,
  comparable facts and a total tie-break on (file, line, name), so the same
  request over one generation produces the same package. Task text ranks
  evidence and authorises nothing: there is no code path from it to a mutation,
  which is an absence rather than a check. A task longer than its ceiling is
  refused, not truncated.
- **No MCP tool and no *ordinary* RPC method builds an index.** Indexing runs a
  compiler over repository source, so it is an authorised operator action. A
  model holding every Atlas tool still cannot cause a compiler to run.

  **The A8-CI closeout added `code.index`, and it must be described as the
  narrow exception it is.** The original rule said no RPC method at all, and
  the consequence was not the one intended: under A7.1 the index is 0700
  `atlasd`, so with no method the *only* way to reindex was to stop
  `atlas.service` and run the command as the service account. That is a
  documented workaround standing in for a missing feature, and it asks an
  operator to do by hand the one thing the separation exists to prevent.
  `service.h` had meanwhile described the method as already existing, so the
  documentation was wrong as well.

  What changed is the group, not the guarantee. `code.index` sits in the
  operator-uid table beside `backup.create` — offered only to the peer whose
  `SO_PEERCRED` uid equals the `operator_uid` in the root-owned policy, and
  answered with `unknown method` for everybody else, including `atlas-worker`
  and every MCP client. There is still no MCP tool that can build an index and
  no ordinary method that can. The work is queued to the writer thread, which
  is the daemon's one serialized writer path, so this adds no second writer.
- **One implementation per answer.** `atlas_sem_impact_on` and
  `atlas_sem_context_on` take a raw handle and a resolved repository; the CLI
  and the daemon both call them. Parity between the surfaces is structural
  rather than two functions somebody keeps in step.

## Long operations — the A8-CI closeout

- **An operation that can outlast a client's patience does not run in the serve
  loop.** `atlas_server_dispatch` runs inline, so a thirty-second backup stalled
  every other client for thirty seconds — which the serve loop is written to
  prevent, and which the backup path simply was not covered by. Backups run on
  the operations thread; semantic indexes run on the writer thread. Neither runs
  where a request is dispatched.
- **The client is answered when the work is accepted, not when it is done**, and
  polls `operation.get` for the rest. This is the shape `repo.sync` already
  used; it is now general. A backup of a 437 MiB index failed at 10.022 s with
  "timed out while reading a frame header" and exit 1 while the daemon wrote and
  verified a perfectly good one. **A success reported as a failure is worse than
  a failure**, because the next thing anybody does about it is re-run the
  operation or work around it.
- **A terminal record never changes.** `atlas_ops_finish` ignores a second
  transition, which is what makes polling idempotent and what lets a client
  killed mid-poll simply ask again. It is reachable from an error path that can
  itself run twice.
- **A failed poll is not a failed operation.** The client retries to
  `ATLAS_OPS_CLIENT_WAIT_MS` and reports "still running, ask again" if it gets
  there. Running out of patience is a statement about the client. This is the
  same mistake as the original one, one layer down, and it happened: a semantic
  index makes the daemon write hard for minutes and the poll's own frame read
  hit the transport timeout.
- **The client is not the operation.** The work holds no reference to the
  connection, so a disconnected, killed or merely bored client neither cancels
  nor corrupts it — verified by killing a client mid-backup.
- **An id is never reused by a later daemon.** The table is in memory, so the
  counter restarted at 1 and an id issued before a restart named a different
  operation afterwards — a client polling it got another operation's verdict,
  which is a confident wrong answer rather than the "unknown" the contract
  promises, and is exactly the failure this layer exists to prevent. Each
  daemon seeds its ids above every id any previous one issued.
- **The table is in memory and a restart forgets it, deliberately.** The
  underlying operations already have deterministic crash behaviour a durable
  record could only describe: a backup publishes atomically or leaves nothing,
  and a generation publishes atomically or leaves a RUNNING one nothing points
  at while the last valid one is still served. An unknown id is reported as
  unknown — the same answer eviction gives — and the message points at the
  artefact, which is what actually survives.
- **A second concurrent request is refused, naming the first.** Deterministic
  refusal beats queueing: two backups of one index, or two indexes of one
  repository, differ only in which artefact somebody ends up looking at.
- **A generation reports the rows it holds, not the work the pass did.** The
  counts were accumulated per translation unit, so a symbol declared in a header
  was added once per including unit: DNA generation 1 stored 520,925 symbols and
  978,122 edges for a generation holding 22,305 and 325,218, and the full and
  incremental paths disagreed by more than twenty times about identical content.
  `atlas_db_sem_generation_counts` measures them once, at publication, from the
  rows. Any figure quoted for DNA from before that fix is the inflated one.

## A8 final closure — migration 10 and the account of an edge

- **A relation's reason lives outside the revision that carries it, and that
  placement is the design.** A revision is immutable and its links are covered
  by the canonical content hash, so a rationale inside one would either change
  `ATLAS_DECISION_HASH_DOMAIN` — making all 54 approved digests disagree with
  their content, which `atlas doctor` reports as tampering — or force a new
  revision and a fresh approval for every document that ever gained an edge. A
  reason written after an approval was not part of what was approved. It is
  evidence *about* the edge, the same separation A6 makes between the content
  hash and the evidence digest, and `docs/decision-lifecycle.md` carries the
  row saying so.
- **`decision_edge_events` is keyed by the semantic edge, never by a
  `decision_links.id`.** A link row is rewritten with a fresh id on every
  revision, so an id-keyed reason would be silently lost by the next revise —
  which is the failure the table exists to end. It is append-only: no UPDATE, no
  DELETE, no `_clear` and no `_prune`; a correction appends and the highest id
  wins. Ordering is the AUTOINCREMENT id and never a timestamp, which is A8's
  rule. It is CANONICAL in `RETENTION[]` and not prunable.
- **What is live is decided by the current revision's links, never by the
  ledger.** `active` is computed on read, for the reason A6 recomputes freshness
  and A4 recomputes link currency. A cached flag would be a second answer to a
  question that already has one.
- **`link remove` deletes nothing.** It writes a new proposed revision asserting
  one relation fewer; the revision that carried it keeps it verbatim, with its
  creation event and its rationale. That is why withdrawing costs no history.
  Removing a relation that was never drawn reports `removed: false` and writes
  nothing. A removal never rewrites prose, never fabricates a revision and never
  reaches a lifecycle state.
- **`--why` is required to withdraw and optional to draw.** A reason not
  recorded at removal is not recorded at all; an addition can still be explained
  afterwards by annotating the edge.
- **A note that is a decision id is refused.** That is the A8.2 defect — prose
  and a document id sharing a meaning — made structural rather than detected.
  Every new prose field gets its own key beside the id it accompanies:
  `edge_target` names the relation, `edge_note` explains it.
- **A routed op and a named method are the same operation and must validate
  identically.** A client holding a context but not the writer lock sends its
  whole typed op through `decision.revise`, not through `decision.link_add`, so
  both paths call `take_edge_fields`. A reason that reached one and not the
  other made the two ways of recording the same relation disagree — which is
  exactly what `tests/test_decision_edge.c` caught.
- **A database from a newer Atlas is refused on the writable path.** The
  migration loop only ever adds, so a future schema fell straight through it and
  was reported as migrated; an older binary then writing under constraints it
  does not know is how a rebuildable index stops being rebuildable. The
  read-only path already refused; both do now.
- **`atlas doctor` distinguishes "there is no index" from "there is an index I
  may not read".** Under A7.1 the index is 0700 `atlasd`, so from the operator's
  account the second looked exactly like the first — "Atlas has never run here"
  — which is false and is the one command somebody runs to tell them apart. It
  is a reported finding, not a problem, and does not affect `ok`: an index the
  operator cannot read is the correct state of a separated deployment.

## A9 layers — additions

```
src/gw       apikey.c (the credential format, the verifier, the scope
             vocabulary), gwpolicy.c (the root-owned gateway policy),
             http.c (the bounded HTTP/1.1 reader and the one response writer),
             gateway.c (routing, authentication, scopes, audit, the listener,
             browser sessions, the web API)
src/gw/ui    mission-control.html — one page, embedded in the binary
src/db       db_gw.c (typed operations over the migration-12 tables)
src/core     hmac.c (RFC 2104 HMAC-SHA256, constant-time compare, kernel
             randomness), service_apikey.c (the `api-key` command behaviour,
             local and over the socket)
src/ipc      server_apikey.c (three operator-gated credential methods),
             server_gw.c (three gateway-only methods)
tools        atlas_embed.c (the first-party asset embedder)
deploy/a9    atlas-gateway.service, gateway.conf.template
docs         remote-access.md
```

## A9 rules — these are not negotiable

- **What the gateway cannot do is true because of who it runs as, not because
  of anything in `src/gw`.** It runs as `gateway_uid` from the root-owned
  policy, which is neither the operator uid nor a dispatcher uid, so
  `decision.approve`, `backup.create`, `code.index`, `apikey.*`, every `job.`
  and every `dispatch.` answer it `unknown method` — and under A7.1 it cannot
  open the index at all. A bug in the gateway cannot make that false. **Never
  write a check in `src/gw` and describe it as the boundary.**

  On an unseparated machine the guarantee does not apply, and
  `docs/remote-access.md` says so in those words rather than letting a reader
  infer A7.1's separation.

- **Atlas terminates no TLS, and must never be described as providing it.** An
  in-process stack would be a new third-party dependency, which the hard rules
  forbid. `tls_mode = REVERSE_PROXY` records the operator's statement that
  something in front terminates it. The default bind is loopback and anything
  wider requires an explicit `tls_mode` — even `NONE`, which is then a decision
  an auditor can find rather than the consequence of an absent key.

- **The secret is shown once because after that no copy exists.** The index
  holds `HMAC-SHA256(salt, secret)`; `atlas_apikey_record` has no field that
  could hold a plaintext, no read returns one, and no method local or remote
  could produce one. `tests/test_apikey.c` searches the database as **raw
  bytes**, because a query only finds a leak in a column somebody thought to
  check. Adding a column, a field or a method that could carry a plaintext
  deletes the guarantee.

- **One HMAC pass, not a slow KDF, and the argument is in `include/atlas/hmac.h`.**
  PBKDF2 defends a guessable secret; an Atlas key is 256 bits of `/dev/urandom`.
  An iteration count buys nothing against the only attack that exists and hands
  an attacker a per-request CPU amplifier on an Internet-facing endpoint. If a
  future phase accepts a credential a human chose, it must not use this path.

- **`memory:write` is in the vocabulary and is not grantable.** Every tool that
  records something durable maps to it, so denying a remote write is the
  ordinary scope check finding a clear bit rather than a rule each tool must
  remember. The scope is a field on `tool_def`, so a tool added without deciding
  what it exposes does not compile.

- **Hiding is not authorisation.** `tools/list` and the GUI omit what a
  credential cannot reach, and both are conveniences: naming a hidden tool or
  route directly meets the same check and is refused. Never replace the check
  with the filter.

- **Remote credential administration does not exist in A9 — it is absent, not
  refused.** No MCP tool, no gateway route, and no method the gateway's uid can
  reach creates, lists, rotates or revokes a credential.
  `tests/test_gw_remote.c` asks for every name such a tool would plausibly have
  and requires `unknown tool`.

- **The credential methods are gated by their own predicate, and it is not
  `atlas_server_peer_is_operator`.** See `src/ipc/server_apikey.c`: the
  policy's operator uid where one exists, refused under a system deployment
  that names none, and the daemon's own uid in legacy mode — because that
  account already owns `atlas.db` and can write `api_keys` with `sqlite3`, so
  refusing it relocates the verb and protects nothing while breaking revocation
  on every ordinary machine.

- **Revocation must never require stopping the daemon.** The local path takes
  the writer lock a running daemon holds, so `api-key revoke` routes over the
  socket when a daemon owns the directory or the index is foreign. "Stop the
  service to revoke a leaked credential" is not an answer.

- **There is one peer test per gate, asked wherever it is needed.** The static
  helper in `server_gw.c` delegates to `atlas_server_peer_is_gateway` rather
  than repeating the rule — the two disagreed once about legacy mode and every
  gateway request failed as "unauthenticated" with nothing saying why. Asked
  twice is fine and correct: the dispatcher decides whether a *name* is offered
  and the method decides whether the *operation* runs.

- **Audit failure does not break request handling, structurally.** The row is
  queued to the writer and the gateway never learns whether it landed. Atlas
  prefers answering with a gap in the trail to refusing a request because it
  could not write one, and `docs/remote-access.md` states the trade.

- **`key_id` in an audit row means "the principal Atlas authenticated" and must
  never hold a value somebody merely claimed.** A denied row names the
  presented selector in `detail` instead — the selector is not secret and an
  operator needs to know *which* credential was rejected, but the column that
  identifies a principal must stay trustworthy.

- **One CSP header, never two.** `atlas_http_response.csp` exists because a
  browser enforces the intersection of every policy it receives: a page that
  added `connect-src 'self'` beside a default of `default-src 'none'` had no
  connect permission at all, and the GUI could not call its own API. A route
  needing a different policy sets the field; it must not add a second header
  through `extra`.

- **No route becomes a socket message unless it matched the fixed table.** The
  API route table names the daemon method, the scope and the parameters it will
  forward; everything else in a query string is ignored rather than passed on.
  A client cannot name an Atlas method or add a parameter to a daemon call.

- **The gateway has no filesystem read path.** The page is embedded in the
  binary by a first-party C generator. Paths are never decoded and never joined
  to a filesystem path, so an encoded traversal is a route that matches nothing.

- **Browser sessions live in gateway memory and a restart forgets them.** The
  reason A8-CI's operations table does. There is no `gw_sessions` migration and
  there must not be one; a durable session needs a durable secret. A session and
  a bearer token map to the same principal, scope mask and audit identity.

- **`gw_audit` is the second prunable table in Atlas**, and A5's rule about
  widening that applies: the argument is in `RETENTION[]` and must survive being
  read back. `api_keys` is CANONICAL — a credential goes away when an operator
  revokes it, never by age.

## A9.1 layers — additions

There are no new files and no new tables. A9.1 is one column, one lifecycle state
and one vocabulary, threaded through the layers that already existed:

```
include/atlas/decision.h  atlas_decision_kind, ATLAS_DECISION_RESOLVED,
                          ATLAS_DECISION_INTENT_RESOLVE, and a kind-aware
                          atlas_decision_transition_allowed
src/decision/decision.c   KINDS[]: the vocabulary, one written meaning per kind,
                          and the single lifecycle consequence a kind has
src/decision/lifecycle.c  op_resolve, the kind on propose, the revise refusal,
                          RESOLVED in recompute_status
src/db/migrate.c          migration 13, and the one migration in Atlas that runs
                          with foreign keys off
src/db/db_decision.c      the kind column, both filters, the kind counts,
                          atlas_db_decision_approved_revision, RESOLVED in the
                          ledger replay
src/sem/context.c         recorded knowledge as a context item, which
                          ATLAS_SEM_SEL_DECISION had described since A8-CI and
                          nothing produced
```

## A9.1 rules — these are not negotiable

- **A kind is not a status, and no code path may derive one from the other.** An
  APPROVED INVARIANT, an APPROVED ACCEPTED_RISK and an APPROVED DECISION are one
  status and three kinds. Every surface reports both in separate fields; the human
  renderer gives them separate columns and the GUI gives them visually different
  treatments. A single badge carrying both is the one presentation this season
  exists to prevent.
- **DECISION is zero, and it is the one Atlas vocabulary whose zero is not
  "unknown".** A zeroed struct, an omitted argument and an absent column all mean
  DECISION, because every record written before this vocabulary existed *was* a
  decision. There is no such thing as a knowledge record whose kind Atlas does not
  know, so an UNKNOWN member would be a value nothing could ever legitimately
  hold. Backward compatibility is exact for that reason rather than approximate.
- **The kind lives on the document, is immutable, and is not part of the canonical
  content hash.** No `UPDATE` in `db_decision.c` names the column — the same
  guarantee a revision's prose has. Hashing it would move every digest already
  approved, and `atlas doctor` reports a moved digest as tampering; and the kind is
  identity-like rather than content, fixed before revision 1 exists and
  unchangeable under an approval, so the approval covers it by construction.
  Reclassifying is superseding with a record of the right kind, which keeps the
  history of how the knowledge used to be classified. The field-by-field table in
  `docs/decision-lifecycle.md` carries the row.
- **A revision cannot reclassify a document**, and an absent kind is not an
  assertion. `knowledge_kind_given` is what separates "asked for DECISION" from
  "said nothing", so a client that has never heard of kinds can still revise a
  POLICY. Losing that distinction would break every pre-A9.1 client's revise.
- **The transition table is asked with the kind Atlas has stored, never with one a
  caller supplied.** `transition()` reads it from the document. That is what makes
  the single write point the authority rather than the request: a caller that could
  name a resolvable kind for a record that is not one would be describing the
  record it is changing.
- **RESOLVED is reachable only from APPROVED and only for a kind whose approved
  form makes a demand** — OBLIGATION and ACCEPTED_RISK. It is terminal, it names no
  successor, it deletes nothing and it does not say the record was wrong. Reopening
  is a new revision approved through the channel, not a transition back.
- **`recompute_status` and the ledger replay in `atlas_db_decision_verify` must
  agree exactly about where RESOLVED sits in the precedence.** The replay is what
  decides whether the cached status is honest, so a replay with its own opinion is
  not a check. `recompute_status` also derives the approved revision from
  `decision_revisions` rather than from the cache it is about to write — resolving
  is the first operation that invalidates that pointer, and a recomputation that
  trusted it would report a discharged obligation as effective for ever.
- **Resolving is an operator action with A4's honesty limits word for word.**
  `decision.resolve` is in the operator-uid group beside `decision.approve`, spends
  a single-use capability bound to one revision and one content hash, and has no
  MCP tool and no gateway route. Closing an obligation is a claim that work was
  done. `LOCAL_OPERATOR_CONFIRMED` on a resolution says the channel was used and
  nothing more.
- **`atlas_revise_decision` is a gap fix and must be described as one.**
  `decision.revise` has existed since A4 and writes a PROPOSED revision by a
  MODEL_PROPOSAL actor — exactly what `atlas_propose_decision` writes — so MCP
  being unable to express it meant a model could only write a *new* record beside
  an out-of-date one, leaving two documents about one subject with no relation
  between them. Its scope is `memory:write`, which cannot be granted, so no remote
  credential reaches it. What stays absent is every lifecycle verb.
- **Migration 13 rebuilds four tables and is the one migration that runs with
  `PRAGMA foreign_keys=OFF`.** The reason is measured, not hypothetical:
  `decision_links.revision_id` declares `ON DELETE CASCADE`, so the implicit delete
  that `DROP TABLE decision_revisions` performs with foreign keys enabled empties
  the link table *silently and successfully* — verified directly. `defer_foreign_keys`
  does not help; nothing decrements the violations the implicit delete counts, so
  the COMMIT fails even after the rows are back. The flag is a field on
  `atlas_migration`, written out `false` for every other migration so the table
  answers "which run unchecked?" by being read.
- **A migration that rebuilds a table verifies its own row preservation before it
  commits.** Migration 13 records every affected and child table's count first,
  then requires all nine unchanged, every document id/uid pair and revision
  id/hash/state triple preserved, the events sequence still covering the highest id
  it issued, and `pragma_foreign_key_check` silent. The named CHECK
  `no_decision_row_may_be_lost_in_migration_13` is the error message. Every index is
  recreated by name; missing `idx_decision_rev_current` would delete the only
  enforcement of "at most one approved revision per document" without any statement
  failing.
- **A path-anchored knowledge record in a context package is LEXICAL evidence, and
  its kind is reported but never ranked on.** A path link is a path somebody wrote
  down and matching it is a byte comparison; no compiler established that the record
  governs the code, and A8-CI's rule is that PROVEN means the compiler proved it.
  Atlas also has no basis for deciding that an invariant matters more than an
  accepted risk — that is a judgement about the reader's task, so what Atlas does is
  say which is which.
- **The gate filters by status and by nothing else.** Every kind is assessed the
  same way: an approved INVARIANT and an approved OPERATIONAL_FACT both have anchors
  that can move, and a gate that quietly skipped a kind would report a clean
  assessment of a repository it had only partly assessed. A RESOLVED record drops
  out for free and correctly — it is no longer effective, so nothing is left to have
  gone stale.

## A9.2 layers — additions

```
include/atlas/verify.h        the vocabularies, the claim/actor/evidence/
                              attestation model, the aggregate and the db API
include/atlas/verifypolicy.h  the root-owned policy and the assessment
src/verify/verify.c           the vocabularies, the priors, the union-find and
                              `atlas-reliability-v2`; touches no database.
                              A12.1, T5 added `atlas_verify_conflict_settle`,
                              the conflict axis's first producer, here
src/verify/policy.c           the root-owned policy loader
src/verify/detverify.c        the four deterministic verifiers, every one a read
src/verify/autolifecycle.c    the one place a verification result becomes a
                              lifecycle transition; the third caller of
                              `atlas_decision_apply_in_tx`
src/db/db_verify.c            the single write point over migration 14
src/core/service_verify.c     the `verify` command behaviour
src/db/migrate.c              migration 14 (ten tables, additive) and
                              migration 15 (widens one CHECK on a leaf table)
```

## A9.2 rules — these are not negotiable

- **Deterministic verification does not require historical calibration, and
  making it wait for one is a category error rather than caution.** If a
  proposition has a complete mechanical truth condition and Atlas evaluated it,
  how often some model has been right in the past is not an input to the answer.
  `atlas_verify_basis_requires_calibration` is a function precisely so a test can
  assert it and no policy path can quietly reintroduce the coupling; the
  calibration gate in `autolifecycle.c` is guarded on the basis for the same
  reason. The converse holds just as hard: **reliability never substitutes for
  authority**, and no score, sample count or number of agreeing sources accepts a
  risk or adopts an architecture.
- **The three axes are orthogonal and no code path derives one from another.**
  Kind (A9.1), status (A4) and verification state (A9.2). `INVARIANT · PROPOSED ·
  VERIFIED` and `DECISION · APPROVED · INCONCLUSIVE` are both legal and both
  mean something. Every surface reports all three in separate fields; a single
  badge carrying more than one is the presentation these seasons exist to
  prevent.
- **An actor is not evidence.** Three models reading one document are three
  attestations over one evidence root. Within an independent group only the
  strongest attestation counts, never the sum, so repetition contributes exactly
  nothing — `tests/test_verify_model.c` drives forty duplicated attestations and
  requires the score not to move. Independence is **never assumed**: an
  interpretation that declares no source joins one shared group rather than
  becoming a root, which is what defeats both the many-models-one-document case
  and an orchestrator's fleet of subagents.
- **A confidence score is not a probability, and they are different fields with
  different printers.** `confidence_score` is an integer out of 100 and carries
  no percent sign in either renderer; `calibrated_probability` is emitted only
  when calibration supports it and is **absent** rather than null or zero
  otherwise. A schema CHECK enforces the pairing independently. Never write
  "94% probability" of an uncalibrated score, in code, output or prose.
- **A deterministic verifier may only establish a DESCRIPTIVE claim.**
  `atlas_verify_basis_may_verify_semantics` refuses DETERMINISTIC + NORMATIVE,
  and that one cell is the whole enforcement of "descriptive truth is not
  normative adoption". Allowing it would let any observation of the current
  implementation become permanent policy with an impeccable audit trail.
- **Every deterministic verifier is a read.** None creates a process, runs a
  repository's build, executes a command or opens a file the repository controls.
  That is a deliberate restriction: a verifier running a command named in
  configuration is a code-execution path with an audit trail attached. Adding one
  needs the argument in writing and A8-CI's bounded-child sandbox first.
- **UNAVAILABLE is not FAIL.** An index that has not run cannot establish an
  absence, and reporting "could not look" as "it is not there" closes obligations
  that are still outstanding. `atlas.symbol_absent` requires a *complete*
  generation; `atlas.symbol_present` does not, and the asymmetry is the point.
- **`atlas_verify_assess` writes nothing.** Asking what Atlas thinks cannot change
  what Atlas thinks — A6's property, for A6's reason. Only
  `atlas_verify_autolifecycle_run` writes, and it owns its transaction.
- **`autolifecycle.c` is the third and last caller of
  `atlas_decision_apply_in_tx`, and it earns that by owning a wider unit of
  work.** A machine transition and the audit row justifying it are one fact: an
  audit row with no transition describes something that did not happen, and a
  transition with no audit row is an automatic change to project knowledge with
  no recoverable reason. `tests/test_decision_mcp.c` pins the count at three and
  names all three files.
- **The audit row is the warrant, and it binds exactly as tightly as an operator
  challenge.** One document, one revision, one target state, one content hash,
  single-use, consumed by an UPDATE that names the state it observed. Compare
  `op_auto` with `spend_challenge`: what differs is *who can mint one*, never how
  loosely it binds. A path that bound more loosely would make every gate in front
  of it argue about a capability easier to satisfy than a person's.
- **`VERIFICATION_POLICY` is not `ATLAS_AUTOMATIC` and not
  `LOCAL_OPERATOR_CONFIRMED`.** It says a root-owned policy named this exact
  transition and its gates were met. It does not say the record is true and does
  not say a person agreed. It is unwritable by any adapter, has no RPC method, no
  MCP tool and no gateway route, and `method_for` returns NULL for the AUTO ops —
  an absent method rather than a refusing one, which is A7's pattern.
- **Refusals no policy can lift are checked before the policy is read.** A
  JUDGMENT basis, an ACCEPTED_RISK approval, a normative claim reached
  deterministically, and a transition the kind-aware state machine refuses. "Root
  wrote it" establishes that an instruction is authentic, not that it is one Atlas
  should carry out — and a FORBIDDEN answer must never depend on configuration,
  or it would read as something more evidence could change.
- **A machine transition is never ground truth for reliability.** A model
  supports a claim, the aggregate likes it, Atlas transitions, the transition
  becomes a label, the model's reliability rises. Every step looks reasonable and
  the system has taught itself to trust a source using that source's own output.
  `atlas_verify_outcome_eligible` refuses `MACHINE_TRANSITION`, and an ineligible
  outcome is stored-and-not-counted so the case stays auditable.
- **A model cannot become a tool.** `TOOL`, `TEST`, `RUNTIME_OBSERVATION` and
  `ATLAS_VERIFIER` may only exist with `ATLAS_ATTESTED` identity — refused in C
  for the message and by a schema CHECK for the guarantee. Refused rather than
  discounted: a discounted forgery still reads as tool output to somebody
  skimming a UI.
- **All ten A9.2 tables are CANONICAL and none is prunable.** None is rebuildable
  — a repository does not remember that anybody spoke — and, more sharply, these
  tables are the input to a *count*. A half-aged evidence table is not a smaller
  one, it is a wrong one, and every score computed afterwards would be
  confidently wrong with nothing recording why.
- **Migration 14 is additive and migration 15 rebuilds a leaf.** No decision row
  is written by 14, so no content hash moves. 15 widens one CHECK on
  `decision_events`, which nothing references, so foreign keys stay enforced —
  and `decision_revisions.proposed_by` keeps its four-value CHECK unchanged,
  because a policy that could author a revision would be a policy that could
  write project knowledge.
- **`AUTO_REJECT` and `AUTO_SUPERSEDE` are absent, not refused.** Auto-rejection
  needs an explicit falsification condition and low confidence is not one;
  supersession needs a mechanical test for "same subject, compatible scope" that
  Atlas does not have, and a timestamp is emphatically not one. An absent path
  cannot be weakened by a later edit the way a refusing one can.

## A9.2.1 layers — additions

A9.2.1 adds no table and no migration. It is the product wiring for the engine
A9.2 shipped with no way to feed it:

```
src/mcp/mcp_tools.c   eight verification tools; the channel is sent as MODEL and
                      honoured only because it asserts less than the peer uid
src/cli/cli.c         six intake verbs, routed local-or-socket on whether this
                      process holds the writer lock
src/core/service_remote.c  the client for all nine RPC methods, reading the
                      daemon's shape back into the struct the local path fills
src/db/db_verify.c    atlas_db_verify_detail_load: the readable evidence and
                      attestations, display-only and never an aggregation input
src/gw/gateway.c      three read-only API routes; intake is deliberately absent
src/gw/ui/mission-control.html  the Verification view
```

## A9.2.1 rules — these are not negotiable

- **The peer uid is a ceiling, not the answer.** A request may name its channel
  and the name is honoured **only when it asserts less** than the uid would.
  Deriving the channel from `SO_PEERCRED` alone stored every locally-run model's
  attestations as `HUMAN` / `PEER_AUTHENTICATED`, because A7.1 permits running a
  model from the operator's account and on an unseparated machine there is no
  other account — so Atlas was minting the forged-human rows this season exists
  to refuse. The two mechanisms guard different things and neither covers the
  other: `atlas_verify_channel_authority` refuses every raise, and
  `atlas_verify_channel_parse` — accepting a name exactly when
  `atlas_verify_channel_is_transport_selectable` accepts the channel — is the
  **whole** guard for the internal channels, because the rank admits any
  below-rank name and A12.1's `DOCUMENT` ranks below `OPERATOR`. Never add an
  internal channel expecting the rank to exclude it; only the parse does.
  Claiming less authority than you hold is never a forgery; it is the accurate
  statement.
- **A locked authority profile costs a label, never the ability to record.** The
  local CLI asks `atlas_authority_probe` rather than assuming it is the operator,
  so a profile that grants nothing produces a `MODEL` actor instead of an
  unearned `PEER_AUTHENTICATED` one. Both paths ask the same probe; a surface
  that assumed would disagree with the one that asks.
- **A model may reference evidence and may not have produced it.** `COMPILER`,
  `TEST`, `RUNTIME` and `DEPLOYED_CONFIG` are refused on the reference path, not
  discounted — a discounted forgery still reads as tool output to somebody
  skimming a UI. `atlas_verify_produce` is the honest route, and it has no
  parameter for the verdict and must never grow one.
- **Intake is absent from the gateway, and the absence is the argument.** A9's
  rule says a mutating route needs a write scope no credential can hold;
  `verify.evaluate` can move a lifecycle state, so a leaked bearer token must not
  reach it. The three routes that exist are reads. MCP over a Unix socket, where
  the peer uid is a kernel fact, is the trust position intake requires.
- **Every surface that shows a score shows its evidence**, through the one
  writer `atlas_service_verify_write_detail`. Two pairs never collapse: `class`
  with `producer_identity`, and `actor` with `group`. A view printing the first
  of either pair alone tells somebody a model is a compiler, or reads an actor
  count as an evidence count.
- **The detail is display, never an input.** Nothing `atlas_db_verify_detail_load`
  returns reaches the aggregation, so a wrong value misleads a reader without
  moving a verdict. The independent-group partition shown beside an attestation
  is recomputed by the same pure function the score used, never threaded out of
  the aggregation — the first thing a later edit would otherwise be tempted to do
  is feed something from the display back in.
- **The local/remote choice is `atlas_ctx_is_writer`, never `ctx != NULL`.** With
  a daemon running, `atlas_ctx` in AUTO mode still opens read-only, so a write
  routed on the weaker test failed with "attempt to write a readonly database".
- **Intake verbs open no terminal and mint no capability.** Stating a claim,
  citing evidence and attesting change no record and no lifecycle state;
  requiring a `/dev/tty` for them would be ceremony borrowed from an operation
  with entirely different consequences. `decision approve` still needs everything
  it always needed.
- **No MCP tool name may contain an authority verb**, which is why the tools are
  `atlas_verify_evidence`, `_attest` and `_depend` rather than `_add` spellings.
  `tests/test_registry.c` enforces it and must not grow an exception; the names
  now mirror the CLI subcommands one-to-one, which is what §29 parity should look
  like.
- **Security refusals are tested through the transport.** A refusal that exists
  only below it is one an attacker never meets, so `tests/test_verify_product.c`
  drives JSON on stdin through the real MCP adapter against a live daemon.

## A9.2.2 layers — additions

A9.2.2 adds one migration and no new file. It is a fourth axis and a coverage
model, threaded through the layers that already existed:

```
include/atlas/verify.h    atlas_verify_truth, atlas_verify_coverage,
                          atlas_verify_coverage_dim, atlas_verify_truth_reason,
                          atlas_verify_truth_of — the one producer of ABSENT —
                          and atlas_verify_truth_change_of
src/verify/verify.c       the vocabularies, COVERAGE_DIMS[], TRUTH_REASONS[],
                          the polarity table and the absence-proof rule
src/verify/detverify.c    settle(): every check now goes through one place that
                          applies the positive/negative asymmetry
src/db/db_verify.c        sem_generation_state(), atlas_db_verify_sem_callers,
                          atlas_db_verify_index_current, the truth columns
src/db/db_state.c         atlas_index_state_is_current, moved out of src/ipc so
                          one rule serves the serve loop and the verifier
src/db/migrate.c          migration 17 (six columns, additive)
src/sem/context.c         knowledge_truth on a context item — §24
src/gw/ui/mission-control.html  the truth chip and the coverage table
```

## A9.2.2 rules — these are not negotiable

- **NO EVIDENCE OF X IS NOT EVIDENCE OF NO X.** ABSENCE requires positive proof
  that the observation coverage is sufficient for the bounded claim. Where
  coverage is insufficient, incomplete, stale, unsupported or unknown, the answer
  is UNKNOWN — never ABSENT. Everything below is that sentence made structural.
- **`atlas_verify_truth_of` is the only producer of `ATLAS_TRUTH_ABSENT`.**
  Nothing else assigns it, no caller passes it in, no transport carries a field
  that could hold it, and no intake verb accepts one. That is the shape
  `settle()`, `atlas_db_evidence_insert`, `atlas_decision_apply_in_tx` and
  `atlas_orch_apply_in_tx` have, applied to a value rather than to a table.
  `tests/test_verify_absence.c` scans `atlas_verify_op` for a field that could
  carry truth or coverage in from a caller: **the guarantee is an absent
  parameter, not a check on one.**
- **The four axes are orthogonal and no code path derives one from another.**
  Kind (A9.1), status (A4), verification state (A9.2), truth (A9.2.2). An
  APPROVED OBLIGATION that is VERIFIED and ABSENT is exactly what discharges it;
  a PROPOSED INVARIANT can be VERIFIED and PRESENT. Every surface reports all
  four in separate fields, and a single badge carrying more than one is the
  presentation these seasons exist to prevent.
- **The asymmetry is the shape of the world, not a convenience.** Finding one
  caller proves a caller exists however incomplete the index, because an
  incomplete index cannot conjure a call that is not there. Finding zero callers
  proves nothing unless Atlas can show it looked everywhere a caller could have
  been. One direction is monotone in coverage and the other is not. A gate
  applied to *both* directions would make Atlas uselessly cautious rather than
  correctly cautious, and `test_fixture_a_...` is what notices.
- **The coverage gate moves the check, not only the truth.** A negative
  conclusion with insufficient coverage returns UNAVAILABLE from the verifier,
  not FAIL. If the gate lived only in `truth_of`, one row would carry
  `state = CONTRADICTED` and `truth = UNKNOWN` — the same mechanical evaluation
  contradicting itself across two fields, which is worse than the defect it was
  meant to fix. `settle()` in `detverify.c` is the one place that decides, and it
  asks the polarity table rather than assuming which of PASS/FAIL is the
  negative.
- **UNKNOWN is zero on the truth axis and on every coverage dimension.** A zero
  that meant ABSENT would make `memset` assert non-existence. **UNKNOWN coverage
  is never sufficient** — that single line in
  `atlas_verify_coverage_sufficient` is the whole difference between "I found no
  evidence of X" and "X is absent".
- **Coverage is never a percentage.** A denominator Atlas cannot state is one
  that makes a number up, and `coverage = 87%` reads as precision about exactly
  the thing that is unknown. Explicit dimensions with a five-value vocabulary,
  and `NOT_APPLICABLE` is asserted from a mechanical fact — an unconsidered
  dimension is UNKNOWN.
- **Empirical evidence never establishes PRESENT or ABSENT**, however high the
  score and however many sources agree. Five agents failing to find X is a fact
  about five agents. Checked on the basis in `atlas_verify_truth_of` before any
  coverage is consulted.
- **NOT_VERIFIABLE is not a fifth axis and UNKNOWN is not its substitute.** It is
  derived from `semantics == NORMATIVE || basis == JUDGMENT`. UNKNOWN says "more
  evidence would settle it"; for a normative proposition none would, and saying
  UNKNOWN invites somebody to go and look.
- **Repository absence is not operational absence.** No verifier observes a
  running system or reads deployed configuration, so `runtime_state` and
  `deployed_config` are UNKNOWN for every one of them — which makes a claim whose
  negative rests on either UNKNOWN **by construction**, with no rule anywhere
  deciding that it must not. Adding a runtime probe is a code-execution path and
  needs the argument every verifier's read-only restriction already demands.
- **"No PROVEN direct caller" and "no caller" stay different claims.**
  `atlas.proven_edge` answers the first, `atlas.no_proven_caller` the second.
  Two verifiers rather than one with a footnote, because that is what makes the
  distinction survive being read quickly. The second is bounded by three
  mechanical questions — direct callers, whether the address ever escapes, and
  linkage — and its scope sentence says what it does **not** claim: nothing about
  dynamic symbol lookup or code outside the indexed repository.
- **An escaping address is what makes indirect calls unresolvable, and that is
  the whole argument.** A C function cannot be reached through a pointer, a
  dispatch table, a callback or a dynamic registration unless its address is
  taken, and `ADDRESS_TAKEN` is a PROVEN edge naming it — so zero address-takes
  over a *complete* generation excludes every one of those at once. The
  completeness is not optional: a translation unit that failed to parse could
  hold the address-take.
- **Linkage is treated as external unless every definition is established
  INTERNAL.** UNKNOWN linkage is the dangerous case, not the convenient one.
- **An ABSENT result never survives the source moving.** It stays bound to its
  snapshot as history; the current truth is recomputed on every read and a moved
  repository yields UNKNOWN until something re-establishes it. Both directions are
  demoted across a drift, not just the negative one — a symbol found at commit Y
  may have been added after the claim's commit X.
- **UNKNOWN must never satisfy a policy condition that requires ABSENT.**
  Structurally true already, and checked anyway via
  `ATLAS_VREASON_COVERAGE_INSUFFICIENT`, for A6's reason about asserting a
  permissive verdict deliberately: a guarantee that holds only because three
  other gates catch it is one a later edit to any of the three deletes silently.
- **UNKNOWN → PRESENT is knowledge acquisition, not a verifier error.** Charging
  it would penalise a verifier for having been honest about the limits of its
  coverage, which is the behaviour this season exists to encourage. Only an
  established answer contradicted **at the same bound snapshot** is an error, and
  `atlas_verify_truth_change_of` decides that from the binding, never from
  elapsed time.
- **Migration 17 is additive and relabels nothing.** Every column defaults to its
  vocabulary's zero, so every pre-A9.2.2 result reads UNKNOWN. A result written
  before the coverage model existed carries no information from which its truth
  could be reconstructed, and inventing one would be the exact error this season
  exists to prevent.
- **`atlas_index_state_is_current` has one implementation.** It moved from
  `src/ipc/server.c` to `src/db/db_state.c` because the coverage model asks the
  same question from `src/verify`; `atlas_server_index_current` delegates. Two
  copies of a currency rule is how a verifier comes to believe a stale snapshot
  is current — and then reports "the bytes differ" for bytes it never read.

## A9.2.3 layers — additions

A9.2.3 adds one migration, one file and one header. It is a lifecycle the daemon
owns and a coverage manifest a generation carries, threaded through the layers
that already existed:

```
include/atlas/sem_schedule.h  atlas_sem_activity, atlas_sem_plan, the
                              ATLAS_SEM_HOLD_* vocabulary
src/sem/schedule.c            atlas_sem_plan_for — the whole derived state, and
                              a read: no transaction, no lock, no process
include/atlas/sem.h           atlas_sem_scope_discovery, atlas_sem_config, the
                              coverage manifest and `source_identity` on a
                              generation, ATLAS_SEM_STALE_SOURCE
src/sem/index.c               atlas_sem_source_identity, the live compdb digest,
                              atlas_sem_freshness_now (one implementation of the
                              freshness question), the manifest at publication,
                              and the no-change re-stamp
src/db/migrate.c              migration 18 (one table, eight columns, additive)
src/db/db_sem.c               sem_repo_config, the scope counts, the test split
src/db/db_verify.c            unchanged; the verify layer now asks the plan
src/verify/detverify.c        sem_state(), and the units/scope split across the
                              coverage dimensions
src/daemon/watch.c            sem_sweep() on the watcher's timer
src/daemon/writer.c           the automatic attempt's governor record,
                              atlas_writer_sem_index_pending
src/ipc/server_backup.c       code.sem_config, in the operator-uid table
src/ipc/server_sem.c          atlas_server_write_sem_config, one writer for the
                              derived state on every surface
src/cli/cli.c                 code sem-config, --test-root, --auto/--no-auto
src/gw/ui/mission-control.html  state, freshness and coverage as three rows
```

## A9.2.3 rules — these are not negotiable

- **A SEMANTIC INDEX CAN BE SOURCE-CURRENT BUT COVERAGE-INCOMPLETE.** Freshness
  and coverage are two axes and neither is derived from the other. `INCOMPLETE`
  is the state that proves it: a generation built from exactly the current source
  that describes half the tree. Every surface reports both, and a single badge
  carrying both is the presentation A9.1, A9.2, A9.2.2 and this season each exist
  to prevent. **`CURRENT` never means "a semantic index exists".**
- **There is no dirty bit and there must not be one.** Freshness is recomputed on
  every read — A6's rule about gate freshness and A4's about link currency — so a
  stored flag would be a second answer to a question that already has one.
  `atlas_sem_plan_for` is a pure read, which is what lets the daemon's scheduler
  and `code sem-status` be the same function rather than two that agree by
  inspection.
- **`atlas_sem_source_identity` is what makes an uncommitted edit visible.**
  Every A8-CI staleness check compares something that moves with a *commit*, and
  Atlas indexes the working tree — so a source could be edited, added or deleted
  with the head standing still and all four agreed the index was current. It
  covers every live C source and header the file index holds by path and content
  hash, the live compilation-database digest, the compiler version and the
  analyzer epoch, domain-separated and length-prefixed for A4's reason. A file
  whose hash Atlas does not hold contributes a fixed marker rather than being
  skipped: skipping it would make a file Atlas could not read compare equal to
  one that was never there.
- **An empty stored identity never makes a generation stale, and neither does an
  empty live one.** "This index did not record what it was built from" and "Atlas
  could not look" are both absences of evidence, not evidence of change. That is
  what makes migration 18 conservative by construction rather than by a rule.
- **A9.2.3 reverses A8-CI's decision not to recompute the compilation-database
  digest on a read, and the reversal must be described as one.** Every caller
  passed NULL for it, so the branch reporting a changed build description was
  unreachable. A8-CI's reasoning was sound while a person ran the next index —
  the check only decided how a status line read. It stops being sound once the
  daemon schedules by *noticing*: a check that never fires is a repository whose
  build description can change without anything ever rebuilding it.
- **`atlas_sem_freshness_now` is the one implementation of the freshness
  question.** Before A9.2.3 four call sites each assembled their own arguments and
  disagreed: the CLI and the daemon passed NULL for the compdb digest, and the
  context builder passed NULL *and* hard-coded `file_index_current` to true — so a
  context package could report a stale index as current, which is the one
  statement a model must never be handed. `src/verify` asks the same question
  through `atlas_sem_plan_for`; it used to derive currency from SQL of its own,
  and the two disagreed the moment freshness learned about the working tree.
- **Coverage is measured against the tree, never against the compilation
  database's own contents, and is never a percentage.** `tu_complete == tu_total`
  says every unit the database named was parsed; `scope_uncovered` is the only
  number that can refuse an absence. A denominator Atlas cannot state is one that
  would be made up. `UNKNOWN` scope discovery is never sufficient for an absence.
- **Atlas does not guess which sources are tests.** A directory called `tests` is
  a directory somebody named. An operator declares roots; without them
  `test_scope_known` is false, which is "Atlas does not know" and is a different
  statement from "there are no test units". A declared root matches on a **path
  component boundary** — `tests` must not match `tests_helper.c`, because a
  production source misclassified as a test is wrong in the one direction that
  matters.
- **`ATLAS_COVDIM_GENERATED_SOURCE` follows the units, not the scope manifest,
  and the argument is written down.** A generated source that is compiled *is* an
  entry in the compilation database; the file index cannot see it under an
  ignored build directory, so asking the manifest about it would be asking a
  denominator blind to exactly those files. What bounds the claim is that the
  database must be current — which until this season was a check that could never
  fire. The honest limit stays stated: a source some build compiles but this
  repository's database does not name is invisible to both dimensions.
- **`sem_repo_config` is the authority opt-in, and that is not a secondary use.**
  A8-CI's rule is that indexing runs a compiler and is therefore an authorised
  operator action. Making a repository change a rebuild trigger would delete that
  rule for every registered repository at once; it does not, because
  `auto_rebuild` defaults to 0, only an operator writes the row, and migration 18
  enables nothing that was not enabled before it ran. `code.sem_config` is in the
  **operator-uid** table beside `code.index` and for a stronger reason: that runs
  a compiler once when asked, this decides whether one runs on every change. No
  MCP tool, no gateway route.
- **A pass that finds nothing to do still records that it looked.** A repository
  holds sources the compilation database does not name. Editing one moves the
  live identity, moves no unit digest and moves no scope count, so the pass
  publishes nothing — and without re-stamping the stored identity the repository
  is stale again on the next tick and rebuilds every sweep for ever. Re-stamping
  is honest because the pass has just verified every content-determining input is
  identical; it is the same move as sealing a unit's input digest.
- **The identity is measured after the pass and before the publishing
  transaction.** After, because what a generation may claim is the tree as it
  stood when the last unit was read. Before, because computing it reads files and
  A1 forbids a file read inside a write transaction. A generation is never
  published as describing a state it did not observe: when the tree moves
  mid-build the later identity is recorded, the generation publishes, and the next
  read reports STALE.
- **The retry governor compares identities, never elapsed time.** A deterministic
  failure retried on an interval runs a compiler over the whole tree for ever and
  achieves nothing. What makes another attempt worth making is that the inputs
  changed — which is exactly what fixing the fault does, so recovery is automatic.
  The identity recorded is the one the attempt *started* from, so a tree that
  moved during a failed build is retried. An operator's `code index` is not
  governed by this and its failure is reported every time.
- **A scheduler must not derive its own liveness from a value it supplied.** The
  first cut kept an in-flight flag on the watcher and passed it into the plan it
  then used to decide whether to clear it, so the plan always said BUILDING, the
  flag never cleared, and the repository reported DIRTY for ever having rebuilt
  exactly once. Every unit test would have passed. `atlas_writer_sem_index_pending`
  asks the writer, which owns the queue and runs the job.
- **Coalescing falls out of the derivation rather than being implemented.** The
  scheduler always builds *now* — there is no queue of source states, only one
  question asked again on the next tick. Six saves during a build produce one
  further build and none is lost. **Correctness never depends on timing**; only
  how soon it converges does, which is why the sweep interval is a compiled-in
  constant and not a policy key.
- **The sweep holds while the file index is behind.** The source identity is
  computed from its content hashes, so a generation built then would describe
  hashes nobody can vouch for and be stale the moment the reconciliation pass
  completes. The pass is already scheduled; waiting is the ordering, not a delay.
- **Manual and automatic rebuild are one pipeline.** Both reach
  `atlas_sem_index_on`. There is not an automatic implementation and a manual one
  with divergent semantics, and `code index` stays diagnostic and administrative.
- **One shape on every surface.** The CLI's `--json`, `sem.status`, the gateway
  route and MCP emit the same keys for the derived state. The first cut nested it
  under `semantic_state` in the CLI and left it flat on the wire, which would have
  made a client have to know which produced a document.
- **All of `sem_repo_config` is CANONICAL and not prunable.** Every other `sem_`
  table is rebuilt by indexing; this one is what says indexing may run at all. A
  repository does not record that an operator authorised a compiler to be run over
  it, and an aged-out row would silently stop a repository being maintained —
  which looks exactly like a repository nobody ever configured. It is *deleted*
  rather than zeroed on `repo remove`, because `repositories.id` is a reused rowid
  and a row left behind would hand the next repository somebody else's opt-in.

## What the A9.2.1 closure found — four rules

- **A result field nobody assigns is not blank; it is the zero value, and every
  Atlas zero means something.** `atlas_decision_result.knowledge_kind` was never
  set by any transition, so `decision approve` reported `kind: DECISION` over an
  APPROVED `INVARIANT` and an APPROVED `OBLIGATION` — while the document,
  `decision show`, `decision list` and the JSON surface all carried the right
  kind, and `--json` is refused for the interactive commands so there was no
  second view. It is filled in `spend_challenge`, which approve, reject, resolve,
  supersede and revalidate all reach, so the five cannot answer differently.
  **A surface reporting the wrong kind is worse than one reporting none.**
- **The three axes must be in the shape the *daemon* sends, not only the one the
  CLI renders.** `verify.show` omitted `kind` and `status`, so MCP (which relays
  it verbatim), the gateway (which forwards it) and Mission Control (which binds
  `c.kind` and `c.status`) all lost two of the three — and on a system
  deployment, where the socket is the only path, `atlas verify show --json`
  answered `DECISION` / `PROPOSED` for an APPROVED OBLIGATION. Add such a field
  to `atlas_service_verify_write_assessment` **and** read it back in
  `service_remote.c`; one without the other is how the two paths start
  disagreeing.
- **A claim is named by its uid on every surface that reports one, so every
  command taking a claim accepts both spellings.** `verify show` grew uid
  resolution and `verify run` did not, so the one command that performs a machine
  transition answered "no claim has that id" to the id `verify claim` had just
  printed. `resolve_claim()` in `service_verify.c` is the one implementation;
  a uid never parses as a number, so the two cannot collide.
- **Verify that an install installed, and that the services are running it.**
  `make install` reported `-- Up-to-date: /usr/local/bin/atlas` while the
  installed file differed from `build/atlas`; use `cmp` rather than the report.
  Separately, `atlas-dispatcher.service` was found running a **deleted inode** of
  a previous binary, so `readlink /proc/<MainPID>/exe` ending in `(deleted)` is
  part of checking that the installed binary is the one being executed.

## A9.2.5 layers — additions

| File | Owns |
| --- | --- |
| `src/sem/sem.c` | the verdict vocabulary, `atlas_sem_trust_settle`, `atlas_sem_trust_write_json`, `atlas_sem_coverage_gap`, `atlas_sem_why_is_transient` |
| `src/sem/index.c` | `atlas_sem_trust_now`, the repository-identity freshness check, the bounded per-unit retry |
| `src/sem/schedule.c` | the `INCOMPLETE` hold split, `coverage_gap`, `operator_action_required`, the interrupted-pass exception |
| `src/sem/discover.c` | per-path obstacles: `note_obstacle`, `note_partial_at`, the deterministic sort |
| `src/db/db_sem.c` | `sem_discovery_obstacles` replace/get/forget |
| `src/db/migrate.c` | migration 20 |
| `src/core/service_sem.c`, `src/sem/context.c` | settling the verdict once the result set is final |
| `src/ipc/server_sem.c`, `src/cli/render_json.c` | calling the one trust writer; **never writing the block themselves** |
| `src/core/service_remote.c` | `take_trust`, which must leave the conservative value for every absent key |

`docs/semantic-trust.md` carries the full argument. **A9.2.4 has no section in
this file**; that is a gap in the previous season's documentation rather than a
statement that its rules are weaker, and it is worth closing.

## A9.2.5 rules — these are not negotiable

### A semantic read that found nothing has not established that there is nothing

A9.2.2 proved this for *claims* and built the coverage model an absence rests on.
A9.2.3 gave a generation a coverage manifest; A9.2.4 gave it a discovery verdict.
None of it reached `callers of X`, which answered with its rows plus
`{freshness, stale_reason, generation_id, indexed_commit}` and stopped.

So `zero rows` and `zero rows over a tree Atlas read a third of` were the same
document, and the repository that produced this season answered exactly that with
a PROVEN caller sitting in a file the compilation database never named. The
information needed to refuse the conclusion existed, in the same process, one
function away, and was not on the answer.

**Every load-bearing semantic answer now carries the evidence for its own
verdict.** A surface that omits the trust block is not "less detailed"; it is one
whose answers cannot be reasoned about.

### UNKNOWN is zero, and UNKNOWN does not mean "no"

`atlas_sem_trust_settle` is the only producer of `ATLAS_SEM_VERDICT_ABSENT`, and
a `memset` must never produce an absence proof. `atlas_sem_verdict_parse` refuses
anything unrecognised rather than falling back, because defaulting an unparsed
verdict to ABSENT is the one error that would matter.

### The asymmetry is A9.2.2's, applied one layer out

One row settles PRESENT and nothing else is consulted: a caller Atlas *found*
exists whatever it failed to look at. Coverage bounds a negative conclusion and
bounds nothing about a positive one — which is why positive rows from a **stale**
generation are still emitted, with the generation that produced them beside them.
Suppressing them would discard evidence; presenting them without their generation
would turn evidence about one tree into a claim about another.

### The verdict rests on the generation's discovery, never the live one

`generation_discovery` is what the index being served was built under;
`discovery` is what Atlas can account for now. Both are reported and they differ
exactly when a rebuild is due. Reading the live value in `settle` would let an
improvement nobody has indexed vouch for an index that predates it.

### A repository nobody maintains cannot settle an absence

A freshness value is only ever a statement about the instant it was computed.
`atlas_sem_auto_effective` decides, and the reason names the remedy —
`code sem-config --auto` — rather than sending an operator to look at their
compilation database. The cost is stated rather than hidden: a repository built
by hand with `--no-auto` can never answer ABSENT.

### One implementation of "is this coverage complete?"

`atlas_sem_coverage_gap` returns *which* dimension failed rather than a boolean,
because the four are four different problems with four different remedies. The
scheduler (`coverage_gap_of`), the verdict (`settle`) and the gatherer
(`atlas_sem_trust_now`) all ask it. A repository the scheduler calls INCOMPLETE
and a query that answers UNKNOWN therefore name the same dimension because they
consulted the same function, not because three copies are kept in step.

### One writer for the trust block, and it is a placement decision

`src/cli/render_json.c` and `src/ipc/server_sem.c` were two independently
maintained serializers of the same answers and had **already drifted**:
`have_generation` was on the RPC document and not the CLI's. Adding twenty fields
to two writers by hand would have made that certain rather than likely.

`atlas_sem_trust_write_json` lives in the sem layer and both call it. Do not add a
trust field to either serializer directly; that is the drift the function exists
to make impossible. Every value it emits is an Atlas integer, an Atlas boolean, a
string from a checked Atlas vocabulary or a checked hex digest — A2's five kinds
— so nothing in it needs `atlas_safe`, and nothing that would may be added.

**It is written after the results.** A verdict is a statement about a result set,
so on a streaming writer it cannot precede it. Key order is not a JSON contract
and the guarantee that mattered — that no answer can be read without its currency
— is unchanged, because the block is in the same document.

### The remote parser fails closed on every absent key

A newer CLI against an older daemon finds no `result_verdict`, no coverage and no
discovery. `take_trust` leaves UNKNOWN with `coverage_complete` false, never
errors, and never defaults to a value that would let an absence be believed. A
missing key is Atlas not having been told, which is what UNKNOWN means.

### A symbol that is not in the index is not a usage error

"You asked for something that does not exist" and "Atlas did not read the file it
is in" are different claims, and exit 2 — the operator-typo class — merged them.
Over an incomplete generation Atlas cannot make the second claim at all. It is an
ordinary empty result set and settles like any other. **Ambiguity stays exit 2**,
because a name that resolves to three symbols is a question Atlas cannot answer
as asked.

### INCOMPLETE is never held with HOLD_CURRENT, and is still a hold

`the_published_generation_describes_the_current_source` is *true* of an
INCOMPLETE repository and conceals the half that decides whether any absence over
it means anything. On `/opt/atlas` itself the state is permanent and its only
cause is the operator's own `--exclude`; that one sentence was everything anybody
ever saw about it.

It is still a hold rather than a rebuild trigger. Rebuilding cannot widen a
compilation database, cannot un-exclude a subtree, and cannot make a unit that
failed on these bytes succeed on them, so scheduling one would spin without
converging. `coverage_gap` names the dimension and `operator_action_required`
says that waiting will not help.

### Discovery records every obstacle, with its exact path

A9.2.4 kept the **first** reason and no path, so one declared `--exclude`
consumed the only slot and masked every unreadable directory for the rest of the
walk. The objection it raised against recording the path — "a path is bytes a
repository chose" — was already answered by `encode_rel`, which every accepted
and rejected candidate's path goes through, twelve lines from where the reason
was recorded.

The reason and the path are separate columns and separate JSON keys. Never
concatenate them: a value an operator reads must stay one Atlas owns. Obstacles
are sorted by path so two walks over an unchanged tree agree whatever order
`readdir` returned, and reaching `ATLAS_SEM_DISCOVERY_MAX_OBSTACLES` sets
`obstacles_truncated` — a list trimmed without saying so would recreate the
invisible hole it exists to close.

`sem_discovery_obstacles` is DERIVED and **never prunable by age**: a
partly-deleted list of what was missed reads as a search that missed less.

### repo_identity_hash is compared, before the commit

It has been written onto every generation since A8-CI and compared by nothing,
while `src/gate/assess.c` compares exactly this value and revalidates on a
mismatch. "This index describes a different repository" outranks "this index
describes an older commit of the same one".

The source identity cannot stand in for it: it is built from repository-*relative*
paths and content hashes, so a tree with identical content under a different
canonical root produces an identical value. Both empty-value guards hold — an
empty stored identity is a generation built before this was recorded, an empty
live one is Atlas not having looked, and neither is evidence of change.

### A transient failure is not permanent coverage loss, and both bounds are hard

`tu_failed > 0` makes coverage incomplete for ever, because the retry governor
compares *identities* and identical bytes never retry. A parse child that was
OOM-killed therefore cost a repository the ability to state an absence until
somebody happened to edit a file.

- **Per unit**: `ATLAS_SEM_UNIT_TRANSIENT_RETRIES` further attempts, inside the
  pass that is already running, and only for a reason
  `atlas_sem_why_is_transient` accepts. **Nothing durable records that a retry
  happened**, so a restart has no half-finished state to interpret and no timer
  can wake up and try again. Re-inserting facts is safe because every fact insert
  is `ON CONFLICT … DO NOTHING` on a natural key, and the generation's published
  counts come from `atlas_db_sem_generation_counts` reading the rows that exist.
- **Per pass**: `ATLAS_SEM_WHY_PASS_INTERRUPTED` — `ATLAS_ERR_INTERNAL` or
  `ATLAS_ERR_DB`, a failure of the machine rather than of the inputs — permits
  exactly one further attempt with the source unmoved, bounded by the durable
  `fail_count`.

Everything else is a property of the input and is never retried. Adding a timer,
or widening the transient set to a compiler error, would replace a bound that is
provably storm-free with one that has to be argued about.

### Atlas still does not guess which sources are tests

A declared root matches from the start of the path on a component boundary, so a
nested test directory must be declared by its full relative path. A heuristic that
guessed would classify a production source as a test the first time a repository
used the word differently, and a production source excluded from a
production-scope absence is the failure that matters.

What follows is the rule: **an operator declaring a test root is not evidence
that they declared every test root** — the same shape as A9.2.4's rule that a
pinned compilation-database list is not a completeness claim. `ATLAS_COVDIM_TESTS`
is therefore established by no verifier and stays UNKNOWN, and any absence that
would depend on the test/production split is UNAVAILABLE. The failure mode is
silent — somebody sets the dimension COMPLETE from a non-empty root list and a
whole class of negative answers quietly becomes believable — so it is tested
rather than left to hold by accident.

### The structural and semantic trust surfaces stay apart

A repository whose A3 structural index is current may have a semantic index that
is not. A query answer inherits the **semantic** verdict, never the structural
index's currency, and no code path derives one from the other.

## A9.2.6 layers — additions

| File | Owns |
| --- | --- |
| `src/daemon/writer.c` | `job_kind_is_unbounded`, `writer_wait_locked`, `queue_remove`, `WRITER_BUSY_MSG`, the `running`/`running_kind` claim |
| `include/atlas/limits.h` | `ATLAS_WRITER_WAIT_SLICE_MS` |
| `tests/test_daemon_responsive.c` | liveness under load, driven against a live daemon |

Nothing outside `src/daemon/writer.c` changed. The serve loop in
`src/ipc/server.c` is untouched, deliberately: the defect was never that it
dispatches serially — that is its design, and a second dispatcher would have been
a new architecture for a bug with a smaller cause.

## A9.2.6 rules — these are not negotiable

### The deadline was never the bound; the short job was

Every synchronous writer call in `src/daemon/writer.c` waits with a timeout, and
the comment explaining why has always said the timeout is what stops one slow
mutation stalling every other client. Read carefully, that was never true. The
timeout bounds the stall *at the timeout* — four or five seconds, or three
hundred for a maintenance call. What made it acceptable was that no job on the
writer's queue ever took that long: the bound nobody wrote down was the job, not
the deadline.

A9.2.4 put an automatic, minutes-long semantic pass on the same thread and the
same FIFO. Nothing in the waiting code changed, and it stopped being correct.
This is the shape of failure worth remembering: **a premise that lives in a
comment rather than in a check does not fail loudly when a different season
invalidates it.**

### A caller waiting on the writer is every client waiting

The serve loop dispatches one request at a time. That is A1's design and it is
fine — until a handler blocks, at which point the cost is not paid by that
request, it is paid by every client with a connection and every client trying to
open one. Measured on this repository before the fix: `daemon ping`, which
touches no database and takes no lock, went from 26 ms to 3.9 s, and a second
client's ping was measured at 4009 ms beside a single blocked write.

So the rule for anything added to a serve-loop handler is not "is this fast
enough for its caller" but **"is this fast enough for everybody else"**.

### The question is asked of the kind, never of elapsed time

`job_kind_is_unbounded` answers whether a job has a duration Atlas can state.
Elapsed time was the obvious alternative and is wrong: it cannot distinguish a
job that is nearly finished from one that has barely started, and a waiter
guessing wrong in the second direction abandons a write that was about to
succeed. The switch has **no `default:`**, so adding a job kind will not compile
until somebody decides which side it is on.

### Reconciliation deliberately answers no, and this is the season's trade

Only two kinds answer yes: the pass that runs a compiler and the walk that looks
for build descriptions.

Reconciliation was considered and excluded on evidence. A first full pass over a
large tree genuinely can exceed these timeouts — it was measured doing so under a
ThreadSanitizer build, and that is why the regression test scopes its timings to
the semantic window rather than pretending otherwise. But the common case is an
incremental pass that finishes well inside them, reconciliation fires on every
file change, and a hook write refused during one would be **dropped rather than
delayed**, because hooks fail open and store metadata only. Refusing a write that
would have succeeded is the worse failure and it would have been the frequent
one. `ATLAS_JOB_SNAPSHOT` and `ATLAS_JOB_MAINTENANCE` are excluded for the same
reason and carry the same residual.

**The residuals, written down rather than discovered later:**

1. During a semantic pass or a discovery walk, writer-bound ordinary writes are
   **refused, not deferred**, and hook session records for its duration are lost.
   That is the trade: a fast retryable refusal in place of a stalled daemon.
   **A9.2.7 replaced this residual with a much smaller one** — the pass yields and
   the latency-critical writes are served — and the refusal now happens only
   inside a stretch with no yield in it. See the A9.2.7 section below; the
   sentence above is left as it was written because the trade it describes was
   real and the record of it is the point.
2. A reconciliation, snapshot or maintenance job can still hold the serve loop up
   to the waiting caller's own timeout. Nothing here fixes that, and nothing here
   pretends to. **Still true after A9.2.7**: none of those three is drainable
   either, and reconciliation is still not unbounded.

### Backing out and timing out are different claims and never one message

A synchronous writer call now fails in two ways that mean opposite things:

- `BUSY:` — the job was **taken back out of the queue before anything looked at
  it**. Nothing ran and nothing will, so sending the request again is safe.
- the existing timeout — the job was accepted and **will run**; only the result
  was abandoned. Retrying duplicates the write.

A caller cannot tell these apart from a status code, which is why the difference
is spelled out in the message rather than left to be inferred. Merging them into
one wording would be a correctness bug in the caller, not a wording preference.
No exit code was added: the exit-code vocabulary is a stable contract, and Atlas
already types refusals with an uppercase token in the message.

### The back-out condition is a conjunction, in one lock hold

Backing out requires **both** that the writer is inside an unbounded job **and**
that `queue_remove` found this job still queued. If it is not found, the writer
has dequeued it and is running it: there is nothing to take back, and reporting a
refusal would be reporting one for a write already in progress. Checking either
half alone, or the two across separate lock holds, reintroduces exactly that.

### A job that gives up never overtakes anything

`queue_remove` excises one never-started job and shifts the rest up by one. The
queue stays first-in-first-out, and that is load-bearing well outside this file:
the orchestration ledger and the decision lifecycle both depend on writes applying
in the order they were accepted. A queue that let one job jump another to save a
caller some latency would break them silently.

**A9.2.7 narrowed which FIFO claim Atlas makes, in one direction and
deliberately**, and the narrowing is stated under its own heading below rather
than left to be inferred from this one. The excision rule here did not change.

### One implementation of "a caller waits for the writer"

`writer_wait_locked` replaced nine identical wait loops. Nine copies of a waiting
rule is nine places to fix it, and a daemon that is responsive on eight of nine
methods is indistinguishable from one that is intermittently broken. Each caller
still handles its own ownership and its own detach, because those genuinely
differ — three of them clear pointers into caller-owned result structs, and one
of those is a freshly minted credential that must never be written into a struct
whose owner has gone.

The wait is **sliced** rather than made in one call, so the condition can be
asked again: a pass that starts *after* a job is queued blocks it exactly as much
as one already running, and a single `pthread_cond_timedwait` would sleep
straight through the difference. A slice expiring is not a timeout; only the
caller's deadline is.

### Ownership is settled under the lock that completes a job

The writer reads `wants_result` in the same hold that sets `done` and clears the
running claim. A waiter that has given up clears `wants_result` under that same
lock, so reading it outside the hold could observe a waiter that has already
returned and hand it the job to free.

### A gate that cannot fail is worse than no gate

`tests/test_daemon_responsive.c` asserts that it observed a pass in flight and
that at least one write reached the daemon during one, and fails saying so if
neither happened. Without those two assertions a fixture that finished before
anything could look at it would let every other assertion pass while testing
nothing — which is precisely how the suite passed 79/79 while the daemon was
unreachable for twenty-five minutes.

A9.2.7 changed the second half of that pair rather than removing it: what has to
be observed now is a write **served** during a pass, not one refused during one.
Requiring a refusal would be requiring the residual, and a season whose gate
depends on its own residual firing is one that breaks when the residual gets
smaller.

## A9.2.7 layers — additions

| File | Owns |
| --- | --- |
| `src/daemon/writer.c` | `job_kind_is_drainable`, `writer_run_job`, `writer_yield`, `writer_yield_cb`, the grace in `writer_wait_locked` |
| `include/atlas/limits.h` | `ATLAS_WRITER_YIELD_GRACE_MS`, `ATLAS_SEM_DISCOVER_YIELD_EVERY` |
| `include/atlas/sem.h` | `atlas_sem_index_opts.yield` / `.yield_ud` |
| `src/sem/index.c` | the three yield points in the pass, and carrying the pair into the walk |
| `src/sem/discover.c` | the per-entry yield in `walk_dir` |
| `src/db/db.c` | `atlas_db_in_transaction`, the drain's belt-and-braces guard |
| `tests/test_daemon_responsive.c` | a write landing *during* a pass, and the two classifications compared over the enum |

The serve loop in `src/ipc/server.c` is untouched again, and for the same reason
it was untouched in A9.2.6: its serial dispatch is the design, and the cost of
that design is what this season removes rather than the design itself.

## A9.2.7 rules — these are not negotiable

### A refusal a caller had to keep repeating was an answer, not a write

A9.2.6 is a complete answer to "why is the daemon unreachable" and only half an
answer to "why did my write not happen". For the length of a pass **nothing else
was written at all**, and the bill is in `docs/backlog.md`: a recovery sweep
refused every twenty seconds for a whole pilot window, a submission that needed
sixteen attempts across forty-seven seconds, and an experiment arm that lost a
finished worker's completion — so a tree the worker had correctly edited was
evidence of work Atlas had no record of.

This is the shape worth remembering: **a mechanism can be correct, honest, and
well documented and still be the thing that is wrong.** Nothing about `BUSY:` was
untrue. It was simply the only thing that ever happened.

### The yield is offered where nothing is open, and nowhere else

Three points in a semantic pass — between translation units, once before the
generation is opened, once after the unit loop ends — and every
`ATLAS_SEM_DISCOVER_YIELD_EVERY` directory entries in a walk. Every one of them
is outside any transaction, which is the whole safety argument: A1 forbids
holding `BEGIN IMMEDIATE` across unbounded work, and a callback offered from
inside one could reach a write.

**There is no yield inside a translation unit.** The per-unit transaction
deliberately spans the parse child, and that structure is not this mechanism's to
change — so the largest gap between two yields is one unit's parse, and that gap
is the season's residual rather than something to engineer around.

`atlas_db_in_transaction` is asked anyway at the top of the drain. It is belt and
braces rather than a branch anybody expects to take: a yield point inside a
transaction would already be a bug, and this turns it into a no-op instead of a
nested write. It answers from Atlas' own nesting counter *and* from SQLite's
autocommit flag, because either alone can miss and a false "no" here would be
worse than not asking.

### There is one implementation of the completion protocol

`writer_run_job` claims the job, runs it, completes it and settles who frees it.
The main loop calls it; so does the drain. A second copy of the ownership exit —
`running` cleared, `wants_result` read and `done` set in one lock hold — is the
defect this file's history predicts, and it would agree with the original until
the day one of them was edited.

It **saves and restores** the claim rather than setting and clearing it, and that
is what makes the reuse honest rather than convenient. From the main loop the
writer was idle, so the restore is identical to clearing. From a drain the writer
was already inside an unbounded job, so `running_kind` truthfully names the
bounded kind for the length of the drained job and the pass's kind is put back in
the same hold that completes it. That is what keeps `writer_wait_locked` correct
with no special case: mid-drain the running kind is bounded, so nothing backs
out.

One window follows and is not a defect: between two drained jobs the restored
kind is the pass's, so a waiter whose grace has already expired can back out at
the very moment the drain would have reached it. The refusal is still exactly
true — its job is removed and never ran — so the cost is one retry, not a lost
write.

### The drainable set is decided per kind, with a reason at the case

`job_kind_is_drainable` has **no `default:`**, like `job_kind_is_unbounded` beside
it, so a new job kind does not compile until somebody has answered both
questions. `true` requires two things together: the caller is latency-critical,
and the tables the job writes are disjoint from everything a semantic pass or a
discovery walk touches. Disjointness is what makes the interleave reasonable
rather than merely tested — a drained job cannot see the half-built generation
the pass is assembling, which is invisible until `atlas_db_sem_publish` in any
case.

`false` is not "unimportant" and every one of them carries its reason at the
case. `tests/test_daemon_responsive.c` walks the whole enum and compares both
answers against the documented sets, because a switch that compiles is not a
switch anybody thought about, and asserts the one combination that can never be
right: **an unbounded kind is never drainable.**

### FIFO is narrowed in exactly one direction, and it is written down

Preserved: first-in-first-out **among drainable kinds** — the drain scans the
queue front to back — and **within every kind**. Given up: first-in-first-out
between a drained bounded job and a *queued* unbounded one. A lease renewal that
arrives behind a queued semantic pass is now served before it.

Refusing that would reimpose the starvation the season exists to end, so it is a
choice rather than an oversight. The two orderings that are load-bearing outside
`writer.c` survive because both are per domain and every drained domain keeps its
own order: the orchestration ledger still applies in acceptance order, and so
does the decision lifecycle. **If a new kind's ordering against a queued semantic
pass is load-bearing, it is not drainable.**

### The grace is measured from the waiter's first observation

Not from queue time. A job queued a second before a pass starts has not yet been
made to wait for anything, and charging it for that second would refuse writes
that were about to be served.

It is a local in `writer_wait_locked`, set once and never cleared, so the grace
is per waiter rather than per pass: a waiter that has already spent it does not
earn a second one because a *new* pass started. What it protects against is its
own latency, not the identity of what is holding the thread.

`ATLAS_WRITER_YIELD_GRACE_MS` has to sit between two figures Atlas already holds:
above the gap between two yield points, or a yielding pass is refused anyway, and
below the smallest synchronous deadline on this path — a hook's
`AI_WRITE_TIMEOUT_MS` of 4000 ms — so that a back-out still precedes a timeout.
Those two answers mean opposite things about whether the write is on its way, and
turning every refusal into a timeout would be a correctness bug in the caller.

### The first observation must not remove the job

`queue_remove` used to be the *test*: it asked "is this job still queued?" and
answered by taking it out. That is exactly wrong once there is a grace to serve,
because the whole point is that the drain may still reach the job. The
observation and the removal are two steps now, and the queue is touched only once
the grace has run out. The conjunction A9.2.6 wrote down is unchanged: a job that
`queue_remove` cannot find is one the writer has already taken, and the waiter
keeps waiting.

### `WRITER_BUSY_MSG` did not change, because it is still exactly true

A back-out is still the job being removed before anything looked at it. Nothing
about what a refusal *means* moved; only how often a caller reaches one.

## O10 layers — additions

| File | Owns |
| --- | --- |
| `tests/test_verify_product.c` | idempotency and durability through the transport a client reaches |
| `tests/test_daemon_responsive.c` | what a busy refusal did *not* write |

Nothing in `src/` changed. That is the season's finding rather than an omission:
the production ingestion path shipped in A9.2.1, and what was missing was the
evidence that a client could rely on it. See the O10 section in
`docs/roadmap.md` for the table of which season delivered which requirement.

## O10 rules — these are not negotiable

### The surface was already there; nobody had proved a client could rely on it

Before adding a submit path, read `src/ipc/server_verify.c` and `TOOLS[]` in
`src/mcp/mcp_tools.c`. Verification intake is complete: nine RPC methods, eight
MCP tools, one write point. A second path would bypass
`atlas_verify_intake_apply_in_tx`, and the checks there are exactly the ones a
forger would want to be somewhere else. **There is no second submit surface, and
adding one is not an extension of this milestone.**

### A rule proved at the write point is not a property proved at the boundary

`tests/test_verify_intake.c` drives `atlas_verify_intake_apply_in_tx` directly
and proves what it refuses. That is necessary and it is not sufficient: a client
does not call that function, it sends JSON over a socket to a daemon that may be
busy, may be restarted, and may already hold the row being submitted. Every one
of those is a place a correct rule can fail to reach a caller. Where a property
is what a client depends on, assert it through the transport.

### A refusal is checked at the moment of the refusal, never totalled afterwards

The obvious test — resubmit until it lands, then count the rows — cannot
discriminate. A refusal that silently stored a row would still total one, because
the retry resolves to it by content key. So the check is made **at the instant of
the refusal**, against the read surface, which is answerable then because a read
never touches the writer. Counting at the end is the weaker test that looks like
the stronger one.

### A verification record is not rebuildable, and invariant 1 does not cover it

SQLite is a rebuildable index and never the canonical record of history — true
of files, commits and the structural graph, all of which git or a pass can
produce again. A claim, its evidence and its attestations exist nowhere else. Nothing
rebuilds them, so **accepted must mean committed and rediscoverable by a process
that did not accept it**, and the read that proves it goes through the client's
own surface rather than through the file.

### The axes stay apart when asserting that nothing was acquired

A model's SUPPORT attestation moves a claim to `SUPPORTED`. That is the
**verification** axis and it is the honest reading — an actor did attest. The
axis that carries authority is the **lifecycle status**, and it stays `PROPOSED`.
A9.2's orthogonality rule is not decoration here: a test asserting `UNVERIFIED`
to mean "no authority" asserts the wrong axis, passes for the wrong reason, and
breaks the first time a model legitimately attests. Assert the status, the
absence of a transition, and the absence of a lifecycle audit row.

### Evidence nobody cited is stored and is not shown

`verify.show` lists the evidence an attestation *relied on*, by way of
`verify_attestation_evidence`. A free-standing evidence row is durable and does
not appear until something cites it, because a row nobody cited has not yet borne
on the claim. This is the designed semantics and not a gap; a test that submits
evidence and expects to read it back must attest to it as well.

### The claim key omits the actor deliberately, and the others do not

Two people stating the same proposition about the same tree are stating one
claim, and the second should attest to it rather than fork it — so the claim
content key covers the repository, the record revision, the domain, the text,
the scope, the semantics, the verifier and its input, the commit and the
environment, and not the author or the clock. The **evidence and attestation**
keys do fold in the actor, because two actors having read the same file are two
observations and two attestations are two votes. Do not "fix" the asymmetry: it
is the design.

## A11.1 layers — additions

| File | What it owns |
| --- | --- |
| `include/atlas/rundriver.h` | what the foreground run driver is, and what it is not |
| `src/orch/rundriver.c` | the loop: claim, pin-check, start one worker, gate, report |
| `include/atlas/validate.h`, `src/orch/validate.c` | the one implementation of "run this job's declared verification commands" |
| `src/orch/driver.c` | `claude-repo` and `fake-repo`, and the shared Claude Code execution |
| `src/orch/orch.c` | `atlas_orch_driver_is_repo_tree` — the one list of drivers that work in the repository's own tree |
| `src/db/db_orch.c` | run settlement and follow-up creation, inside `atlas_orch_apply_in_tx` |
| `src/ipc/server_orch.c` | `job.run_status`, the lease's `job` narrowing, the completion's gate verdict |
| `src/core/service_orch.c` | the socket transport for the driver, and `atlas job run` |

## A11.1 rules — these are not negotiable

### A chain of tasks was resolvable and nobody could carry it

A11.0 made the chain a fact about stored rows: a parent that resolves, a run that
groups, one active task enforced by a partial unique index. (A11.6 narrowed that
last one rather than removing it — **one active *repo-tree* task per run** in the
schema, with the run's total active tasks bounded by its own `max_parallel`,
slots in the schema and the bound checked in C. See the A11.6 rules below.) It
started no worker,
settled no run, and said so — `ACCEPTED` and `BLOCKED` had no producer in
production code, and *who may decide* was named as A11.1's question rather than
guessed at.

A11.1 answers it, and the shape of the answer is the answer's content. Read the
four rules below as one argument: settlement travels on a completion, the
completion carries only facts Atlas computed, the gates are fixed before any
worker runs and inherited unchanged, and the number of workers is bounded by the
ledger rather than by a counter anybody writes.

### Every settlement travels on a COMPLETE

There is no `job.run_settle`, no `job.run_accept`, no `job.run_block`, no MCP
tool and no gateway route. `atlas_db_orch_run_set_status` still has no caller
outside `src/db/db_orch.c`, and the two callers it has there are
`settle_run_after_complete` and `run_blocked_by_recovery` — both inside
`atlas_orch_apply_in_tx`, both in the same transaction as the task transition
that justifies them.

That is what keeps "a model payload cannot accept a run" true **by absence**
rather than by a check. A check can be reached and argued with; a method that
does not exist cannot. If a run's acceptance ever needs its own surface, A11.0's
entry in `docs/extending.md` still applies: it belongs in the operator-uid method
group beside `code.index`, never in the ordinary group and never in `TOOLS[]`.

### The completion carries no claim the worker made

`op->success` is not the worker's opinion. It is the conjunction of two things
Atlas did: the exit classification `atlas_driver` computed from the process's
actual fate, and the verdict `atlas_validations_run` reached by running the
job's own stored commands. The worker's stdout, its result document, its exit
code and its prose are evidence *about a process* and are read by nothing that
decides.

**A zero exit is still not a success claim.** That is A8's rule and A11.1 does
not weaken it: a worker that exits zero and fails its gate ends a task, and the
run is not accepted. `tests/test_a11_run.c` submits a task whose text and whose
output assert every authority word Atlas has — `ACCEPTED`, an actor of
`LOCAL_OPERATOR_CONFIRMED`, `all gates passed` — alongside a gate that fails, and
requires the run not to be accepted.

The one piece of worker-adjacent text that survives is `op->failure_detail`, the
bounded excerpt of what a *gate* printed. It reaches exactly one place: quoted,
labelled, into a follow-up task's text, where it is untrusted data given to a
model rather than an input to a decision.

### The gates are fixed before any worker runs and inherited verbatim

A run's verification commands are the root task's `validations`, written at
submission by whoever created the run. A follow-up does not receive a list — it
receives its *parent's*, decoded and re-encoded by the same canonical functions
inside the same transaction. There is no field a worker can set, no message it
can send and no file it can write that changes what runs.

**A task that works in the repository's own tree must declare at least one gate,
and that is checked at the write point.** `op_submit` refuses a repo-tree job
with `validation_count == 0`. The reason is what acceptance would otherwise
mean: a task with no gates succeeds on its process outcome alone, which this
repository has said since A8 is not a success claim. Putting the check on the
command that usually creates such a job would leave the side door open, so it is
where the row is written — the placement `atlas_verify_intake_apply_in_tx` uses
for the same reason.

`argv[0]` is resolved against a fixed allowlist compiled into the binary, never
against `PATH`. A gate command line is split on ASCII spaces by
`split_words`, which is **not a shell and deliberately not shell-like**: no
quoting, no escaping, no expansion, no glob. An argument containing a space
cannot be expressed. That is a limitation rather than an oversight — the
alternative is a miniature quoting language on the path to `execve`, which is how
a gate eventually runs something other than what it reads like.

### The bound counts worker starts, and it counts them in the ledger

`ATLAS_ORCH_RUN_MAX_WORKER_STARTS` is three: the root task's worker and at most
two follow-ups. It is compiled in, has no policy key and no flag, because a bound
a caller can raise is not a bound.

It is **derived, never stored**: `run_worker_starts` counts transitions to
RUNNING across every task in the run. RUNNING is the state the driver records
*immediately before it execs*, so the count is durable before the worker exists.
Three consequences fall out rather than being arranged:

- a crashed worker spends budget exactly as a finished one does;
- a refusal that never reached a lease spends none, which is what makes retrying
  a `BUSY` safe rather than merely probably safe;
- there is no counter for a process to die between incrementing and using.

`ATLAS_ORCH_MAX_ATTEMPTS` is a different bound with a different subject — how
many attempts one *task* may make — and both apply, with the tighter one winning.
They are never compared against each other.

### A crash is retried; a failed gate is not

The two failures are answered differently because they say different things. A
worker that crashed or timed out has said nothing about the task, so the task is
retried within its own attempt bound. A gate that failed has said something
specific about it, and repeating the identical task over the identical tree
would fail the identical gate — so the run's answer is a *different, narrower
task*, with the original goal, the failing gate's name and a bounded excerpt of
what it printed.

Cancellation and `RECOVERY_REQUIRED` are answered by neither. Cancellation was
asked for; `RECOVERY_REQUIRED` is Atlas saying it does not know what ran, and
starting more work on top of that is the opposite of what the state means. Both
block the run.

### Exactly one follow-up per failure, three ways over

The follow-up is created through `op_submit` — the same write point, the same
canonical digest, the same A11.0 refusals — inside the completion transaction,
with the parent already terminal. Three independent mechanisms would all have to
fail together for a run to sprout two follow-ups for one failure:

1. the transaction, which makes the completion and the creation one act;
2. `idx_orch_jobs_one_active_repo_tree`, which permits one non-terminal task in
   the repository's own tree (before A11.6 this was
   `idx_orch_jobs_one_active_per_run`, which permitted one non-terminal task of
   any kind; the guarantee a follow-up rests on is the repo-tree half and that
   half is unchanged);
3. the idempotency key `a11.<parent>.<attempt>`, derived from the failure it
   answers, so a resumed or replayed completion resolves to the task that already
   exists rather than making a second one.

The follow-up quotes the **root** task's goal rather than its immediate parent's,
so a third-generation task states the same objective as the first instead of a
summary of a summary — and so its text is a function of (root, parent, attempt)
and of nothing else, which is what lets the key be one too.

### A repo-tree driver is never granted to a lease that did not name it

`atlas_orch_driver_is_repo_tree` is one list, in `src/orch/orch.c`, and it is
**not** a flag on `atlas_driver`: the daemon has to answer it about a stored name
at the moment a lease would be granted, without linking the driver table's `run`
functions into that decision.

Two refusals follow, and both are needed:

- **`op_lease` skips a repo-tree job for any lease with an empty driver filter.**
  An empty filter means "any", and the A8 dispatcher polls exactly that way.
- **`atlas_service_dispatcher_run` never puts one on a background dispatcher's
  derived filter**, however the root-owned policy lists it. Without this the
  first refusal is bypassed by a filter that names the driver because the policy
  did.

What both prevent is the same thing: a background dispatcher provisioning a
workspace the driver does not use, running it somewhere it was not meant to run,
and completing the task — settling a run with no gate having run where the
changes are.

**Settlement is scoped to these drivers too.** A run whose task ran under an A8
workspace driver is not settled at all: nothing decided anything about it, and
A11.0's statement that its status is its own axis still holds for it unchanged.

### Atlas may now start a process that edits a registered repository

This is the season's one reversal and it must be stated in full rather than
softened.

A8's worker runs on a snapshot in a worker-owned workspace and Atlas applies
nothing it produces. A11.1's runs in the registered repository's **own tree**,
because a chain of tasks that build on each other's changes cannot be built out
of workers who cannot see them: a follow-up that could not see the first
worker's work would be a follow-up to nothing.

What is unchanged: every one of Atlas' own reads. `scan`, the index passes, the
watcher, `src/git`'s every invocation and every command in `src/core` still open
a registered repository read-only, and nothing in this milestone writes a byte
into one. What changed is that an **operator running a foreground command** may
now start a child process whose purpose is to edit the tree, in a directory Atlas
resolved from its own registry.

The scope is three things, all of which have to line up: the driver must be one
`atlas_orch_driver_is_repo_tree` names, the lease must have asked for that driver
by name, and the root-owned orchestration policy must list it. Remove any one and
nothing starts.

**The working tree is expected to be dirty afterwards.** It is the first worker's
output and the second worker's input. Nothing in `src/orch/rundriver.c` cleans,
resets, checks out, stashes or reverts anything, on any path including every
failure path, and nothing should be added that does.

### The pinned commit is checked twice, and a moved HEAD is refused rather than judged

Before the worker starts: a tree that has moved off the task's pinned commit is
not the tree the work was authorised over, and continuing would produce changes
against something else. After it exits: a worker that committed, reset or checked
out has invalidated everything a gate could tell us, so the gates are **not run
at all**.

Both refuse with `ATLAS_ORCH_REASON_POLICY_REFUSED`, which the daemon reads as
non-retryable — no retry and no narrower task answers a moved HEAD — and the run
is BLOCKED.

This is also the only *enforceable* part of the constraint list the worker is
given. "Do not commit, do not push, do not run a destructive git operation" is
instruction, and instruction to a model is not a control. What Atlas actually
holds is that a run whose HEAD moved is never accepted. Say it that way.

### A `BUSY` refusal is retried, and it is not a `BLOCKED` run

A9.2.6's refusal says, in the message itself, that nothing was queued and nothing
will run. `ATLAS_IPC_BUSY_TOKEN` is the machine-readable half of that sentence
and `atlas_ipc_message_is_busy` is the one reader of it.

A11.1 is the first caller that must act on it, because a completion refused this
way carries a worker's entire result: abandoning it means the lease expires, the
task is requeued and a second worker does the same work. So every write the
driver makes is retried under `BUSY`, boundedly. When the budget is spent the
run stays **ACTIVE and resumable** and the invocation reports what happened — a
lost invocation, never a lost run and never a `BLOCKED` one.

The same distinction applies to a task another driver already holds. `busy` on
the report is neither an acceptance nor a refusal: nothing was claimed, nothing
was written, and repeating the command is safe.

### The lease is renewed while the worker works

`ATLAS_ORCH_LEASE_MS` is one minute and a real worker runs for many. A heartbeat
that names the phase the attempt is **already in** renews the lease without
transitioning, and `driver_should_stop` issues one from inside `atlas_proc_run`'s
wait loop — for the worker and for every gate, because `make test` outlives a
lease as readily as a model does.

Without it the daemon's recovery timer reclaims the attempt underneath a healthy
worker, marks it timed out, requeues the task, and refuses the completion as an
unknown token. That is not a lost message; it is a second worker on the same
task, which is the one thing this milestone must not do.

A failed renewal does **not** stop the child. The attempt may already be lost,
but killing a worker mid-edit on the strength of one unanswered call would leave
the tree in a state nobody chose. Cancellation still arrives as the daemon's
answer to a heartbeat and never as a signal: Atlas has no path into the worker's
process tree and must not grow one.

### The run driver starts nothing in the background

No scheduler, no queue polling, no timer, no daemon loop, no provider router and
no second submit path. `atlas job run` is a foreground command an operator typed,
it drives one run, and when it returns nothing of it is still running.

A11.1 also adds no model selection of any kind. There is one model driver,
`claude-repo`, it executes the installed Claude Code CLI, and which model that
CLI uses is the CLI's business and the operator's.

## A11.6 layers — additions

| File | What it owns |
| --- | --- |
| `src/db/migrate.c` | migration 24: `orch_runs.max_parallel`, `orch_jobs.run_slot`, and the two partial unique indexes that replace M21's one |
| `src/db/db_orch.c` | slot assignment, the admission refusals, the repo-tree budget filter, and settlement at quiescence |
| `src/ipc/server_orch.c` | the `parallel` submit parameter, and the `active_count`/`max_parallel` keys on `job.run_status` |
| `src/core/service_orch.c` | `--parent`/`--parallel` on the wire, the resume refusal, and the conservative remote parse |

## A11.6 rules — these are not negotiable

### One active task per run was a bound on the wrong thing

A11.0 bounded a run at one active task, and that bound was carrying two different
arguments at once. One of them is about the *repository*: the registered tree is
a single resource and two workers editing it at the same time is incoherent, not
merely slow. The other is about *how much a run may have in flight*, which is a
resource question with no principled answer of one.

A11.6 separates them. The repository argument keeps a partial unique index and
becomes stricter in what it names — `idx_orch_jobs_one_active_repo_tree`, over
the repo-tree drivers only, and no bound may widen it. The in-flight argument
becomes `orch_runs.max_parallel`, fixed at the root task, defaulting to 1, and
held by a unique index on `(run_uid, run_slot)`.

**Both live in the schema.** The checks in `submit_resolve_run` exist so a caller
gets a sentence naming the task in the way rather than a constraint violation it
cannot act on, which is M21's arrangement and `M8_LEASES`' before it. With the C
checks disabled the submissions are still refused; `tests/test_orch_parallel.c`
proves that by bypassing the write point entirely.

### The bound is refused, never clamped, and never on a child

Outside `1..ATLAS_ORCH_RUN_MAX_PARALLEL` is a refusal with the bound named — A5's
rule about `--older-than`, which A8 already applies to every other bound in
`orch.h`. Zero means "not stated" and resolves to one.

Naming it on a task that joins a run is refused, and so is naming it on
`--resume`. That is A10.1's `--memory --resume` rule and its reason: a run's
parallelism is frozen when the run is created, so honouring the flag afterwards
would be a lie and dropping it silently would be worse — a dropped flag reads, in
a transcript, exactly like an honoured one.

It travels on `atlas_orch_op` and never on `atlas_orch_spec`, so
`ATLAS_ORCH_SPEC_DOMAIN` did not move and no stored `spec_digest` means anything
different than it did.

### A run holds one pin

A child's `source_commit` must equal the **root's**, not its immediate parent's.
Two pins in one run would make ACCEPTED ambiguous — the gates passed over *which*
tree? — and comparing against the parent would let a chain drift a commit at a
time until nothing in it shared a tree with anything else.

### A run settles only at quiescence, and the scan is the verdict

Nothing is ACCEPTED or BLOCKED while any task in the run is non-terminal: a task
that has not ended has not said what it did. At zero active tasks the run settles
by scanning every task in it — SUCCEEDED, or FAILED with a child in the same run
(a failure that was answered) — and by re-checking the repository identity from
the root, at the moment of settlement, because it was checked at lease time and
that is not the same claim.

Settle-eligibility is the **root** task's driver, asked in C and never in SQL. It
is the root's rather than the completing task's because a workspace sibling's
completion must be able to bring its run to quiescence, while a plain A8
workspace run must still settle nothing at all — A11.0's answer for those runs is
unchanged.

Two consequences, both deliberate:

- **A gateless workspace sibling can veto acceptance and can never grant it.**
  ACCEPTED still flows only from the gated repo-tree chain succeeding.
- **A doomed run does not stop the chain mid-run.** A cancelled or failed sibling
  does not interrupt the repo-tree task beside it; the run spends at most its
  bounded budget and then settles BLOCKED. One task's failure must not break
  another task's execution.

### Three worker starts is the chain's budget, not the run's

`ATLAS_ORCH_RUN_MAX_WORKER_STARTS` is unchanged at 3 and now says what it counts:
transitions into RUNNING for the run's **repo-tree** jobs. A workspace sibling
spends none of it and is bounded by its own `max_attempts` — A8's semantics,
untouched. A sibling that could eat the chain's budget would let a run be denied
the follow-up its gate failure had earned because something else happened to be
busy.

No existing count moves: before parallelism every job in a repo-tree run was a
repo-tree job, because a follow-up inherits its parent's driver.

### The view's active task is a claim target, not a census

`active_job_uid` names the task a run driver may claim, and for a repo-tree run
that is the repo-tree task specifically. It is empty while the chain is done and
a sibling is still going, which is an ordinary mid-run answer rather than an
ending — so `active_count` travels beside it, and a reader that inferred one from
the other would read such a run as idle.

Both new keys are absent from an older daemon and both parse to zero, which is
the conservative value in either direction: zero is never a claim that a run is
idle and never a claim that its bound is nothing. A9.2.5's rule for an absent
key, and the renderers print the line only when the bound is present.

### A11.6 adds no mechanism

No thread, no process, no timer, no background loop, no scheduler. No new RPC
method, MCP tool or gateway route, and no second submit path;
`atlas_db_orch_run_set_status` still has no caller outside `src/db/db_orch.c`. No
new isolation: a parallel sibling is a workspace task under A8's existing
isolation, provisioned by the dispatchers that already exist. What A11.6 changed
is which submissions are admitted and when a run is allowed to decide.

## A12.0 layers — additions

| File | What it owns |
| --- | --- |
| `src/db/migrate.c` | migration 25: `orch_plans`, `orch_plan_revisions`, `orch_plan_tasks`, and `idx_orch_jobs_correlation` |
| `src/db/db_plan.c` | the plan write point, the correlation builders, the per-plan readers, and `atlas_db_plan_state_derive` |
| `src/orch/plan.c` | the `atlas-plan-1` parser and validator, and the five prompt composers — pure: no DB, no process, no clock |
| `include/atlas/plan.h` | the format bounds, `atlas_plan_status`, the document structs, the composer API, the derived-state structs |
| `src/orch/plandriver.c` | the foreground loop: submit a planner job, ingest its revision, walk the stages, answer one BLOCKED stage-run |
| `src/orch/driver.c` | `claude-plan` and `fake-plan`, `atlas_driver_role`, and `--model` from the policy's per-role names |
| `src/orch/policy.c` | `planner_model` and `executor_model`, optional, one token each |
| `src/orch/dispatch.c` | `atlas_dispatch_run_one`, and `artifacts_travel_inline` — a planner's artifacts are carried, not described |
| `src/orch/rundriver.c` | the transport's third member `job_get`, and the retry that separates a lost answer from a refusal |
| `src/ipc/server_orch.c` | the four `plan.` methods, in the existing client group, and the typed refusal detail a document earns |
| `src/core/service_plan.c` | the four wire calls, the production plan transport, and `plan run|status|show|list` |

## A12.0 rules — these are not negotiable

### A plan a model wrote is a proposal, not a verdict

This is the whole season in one line, and everything below is a consequence of
it. A12.0 is the first thing in Atlas where the *shape of the work* is proposed
by a model rather than typed by an operator, and the design exists so that the
proposal changes exactly one thing: who suggested it.

Compiling a plan grants nothing. Every task a revision produces is submitted
through the one submit path, admitted by the same A11.6 refusals, leased under
the same exclusivity, bounded by the same worker-start budget, gated by the same
`atlas_validations_run` and settled by the same `settle_run_at_quiescence` as a
task an operator typed. There is no code path anywhere that a plan reaches and a
hand-written job does not.

### The operator brings the gate floor, and the planner may only add to it

`plan run` requires at least one `--gate`, refused locally before the policy is
even read, in the same words `job run` refuses it in — two spellings of one rule
read as two rules. The floor is stored on `orch_plans.gate_floor` in the same
netstring encoding `orch_jobs.validations` uses, and is prepended **verbatim and
first** to every tree task's merged list at the moment a revision compiles. A
planner's `gate:` lines are appended after it and can never replace, reorder or
remove one.

Floor plus additions is at most `ATLAS_ORCH_MAX_VALIDATIONS` (8), refused at
compile time — and the specification says so to the planner, including the
instruction to *count the floor*, because a document whose additions alone were
in bounds can still exceed the merged bound and the planner is the only party
that can avoid that.

A plan with no operator gate could only ever be accepted on a model's word, which
is the one thing this season exists to keep impossible.

### The merged list is built once and carried opaquely thereafter

`orch_plan_tasks.validations` is the merge, performed at the write point when the
revision compiled. The plan driver reads it out of the row and hands it to
`job.submit` without splitting, merging, decoding, re-encoding or displaying it.
A second implementation of the merge in a driver would be a second answer to
what an accepted stage was gated on.

`tests/test_plan_e2e.c` pins it byte for byte through the production transport
with a `%` inside a gate argument, because `%` is the one byte `atlas-safe-1`
rewrites and ordinary words cannot distinguish a correct round trip from an
encode that was never undone.

### Plan status has no writer, and that is the authority argument

There is no status column on `orch_plans`, no `plan.settle`, no MCP tool and no
gateway route. `atlas_db_plan_state_derive` computes the answer on every read
from stored rows — revisions, correlations, job states, run statuses — and both
`plan.get` and the driver's loop ask that one function, so the surface an
operator sees and the value the loop acts on cannot disagree.

This is A11.0's pattern one layer up: "a model payload cannot declare a plan
complete" is true **because the verb does not exist**, not because a check
refuses it. `UNKNOWN` is zero, is never stored and does not parse; a plan that
derives it is reporting a defect in the derivation rather than an answer.

`PLANNING` at every k, including k = 5, for a planner job that SUCCEEDED and that
no revision names. Ingesting a stored document spends no planner start, so a paid
valid document must stay ingestible; `BLOCKED` requires the last planner job to
have failed terminally with no budget left. The cost of that choice is stated
below.

### Only a planner-role job can produce a revision

`plan.revision_add` verifies four things **inside the write transaction**, for the
reason every A11.0 admission check is inside one: a check is worthless if a
second call can land between it and the insert.

1. The named job's `correlation` binds it to *this* plan as planner job k.
2. Its driver's role is `ATLAS_DRIVER_ROLE_PLANNER`.
3. It SUCCEEDED.
4. Its successful attempt stored an artifact named `plan.atlas-plan` **inline**
   and within `ATLAS_PLAN_MAX_BYTES`.

The bytes come from `orch_artifacts` and from nowhere else. There is no parameter
carrying a document, which is what makes "a model's plan never travels a second
path into the write point" an absent field rather than a check on one. An
executor job's artifact can never become a plan.

A role is a property of the **driver**, never of a job: a submitter that could
assert a role could assert its way into the answer. There is no
`atlas_driver_role_name`, because a role is never rendered, never sent and never
parsed.

### Model prose never routes control flow

The replan trigger is Atlas' own verdict — a stage-run that settled `BLOCKED` —
and never a sentence a worker wrote. There is no `strstr` over a plan document, a
worker log or an artifact anywhere on that path, and `atlas_plandriver_run`
branches on nothing but `atlas_plan_state`.

A planner's title and prompt travel from a stored row into a composer that
labels them and into a lease that delivers them, and are compared with nothing on
the way. A gate excerpt travels from `job.get`'s stored `gate.log` into a composer
that bounds and labels it. One `strstr` over any of them would end this argument,
which is A10.1's rule about the memory package, restated where it now also
applies.

A blocker-artifact fast-path is a backlog residual rather than a refused feature:
it could only ever veto a stage earlier and could never grant anything, so it is
compatible with the rule — and it is still a path from model prose into control
flow, which is why A12.0 has none.

### Roles and models are the operator's, in the root-owned policy

`planner_model` and `executor_model` are optional keys in
`/etc/atlas/orchestration.conf`, each one token of `[a-z0-9._-]` at most 64
characters, reached through the compiled-in root-owned path with no override.
Which of the two an attempt uses is decided by `atlas_driver_model_for` from the
driver's role and by nothing else. Unset passes no flag at all, which is what
every run before A12.0 did.

**No model name appears in `src/`.** `planner_model = fable` and
`executor_model = opus` are one machine's current choices, not Atlas'.

### One builder for a correlation, and the mapping is derived

`atlas_plan_correlation_planner` and `atlas_plan_correlation_task` are the only
producers of `plan.<uid21>.planner.<k>` and `plan.<uid21>.r<R>.<key>`, and the
same string is both the correlation and the idempotency key. Three layers build
it — the write point checking a planner job's binding, the derived reader finding
a plan's jobs, and the driver submitting them — and two spellings of one format
are two answers to "is this job this plan's".

The plan-to-jobs mapping is therefore **derived**: no bind RPC, no column on
`orch_jobs`, no update. `idx_orch_jobs_correlation` makes the read cheap.

`<uid21>` is `'p'` plus the first 20 hex characters of the plan uid, and the
truncation is forced rather than chosen: a correlation is bounded at 64 bytes and
validated by `is_name`, which admits `[a-z0-9._-]` and therefore no colon. The
worst case is 62 bytes and the collision probability is 2⁻⁸⁰.

### The loop keeps no state that must survive a crash

Everything `atlas_plandriver_run` acts on is re-derived from `plan_state` at the
top of every iteration. Every submission is idempotent by key, so a re-issued one
is handed the job that already exists. Ingesting a revision is deterministic over
stored bytes, so a re-run either compiles the same revision or reproduces the
same refusal — which is why a document's refusal is obtained inside the iteration
that answers it rather than remembered from the one that earned it.

An iteration that leaves the derived state byte-for-byte identical to the
previous one ends the invocation: whatever remains belongs to a dispatcher this
process is not, or to a driver that already holds the task, and going round again
would be this process pretending to make progress.

### A refusal names what and where, and the two travel apart

A document refusal is a sentence *and* a 1-based line number, carried as separate
members through `atlas_plan_refusal` and as a typed detail on the error document.
Folding the number into Atlas' prose would make the driver parse that prose to
recover it.

### A lost answer and a refusal are different claims

`BUSY:` is the daemon saying it took nothing: an answer, and asking again gets
the same one. A transport failure — the connect, the send, the read, a reply that
was never a reply — says nothing at all about whether the request was processed,
so it is retried on its own bounded budget of `ATLAS_RUN_XPORT_TRIES` attempts
`ATLAS_RUN_XPORT_PAUSE_MS` apart. `atlas_err_is_transport` is stamped by the
client layer that held the file descriptor and cannot travel the socket, so
nothing a daemon says — or quotes back from a repository, a task or a model —
can produce one.

`atlas_rundriver_transport` has a third member, `job_get`, and it is **not
optional**. A completion whose answer was lost is only delivered if the task
ended in the way that completion asked for, and the run cannot say that: "this
run no longer holds this task open" is equally what an expired lease, a
cancellation and a recovery sweep produce. A transport without `job_get` cannot
answer the question, and the alternative to refusing one at construction is a
check that silently degrades into a guess. The owed-check establishes **"nothing
is owed"** and not "recorded with our result"; a completion that landed and
requeued the task reads as still owed, which is the pessimistic direction.

### A planner's artifact is carried, not described

`artifacts_travel_inline` (`src/orch/dispatch.c`) sends the completion manifest's
optional fifth field for a PLANNER-role driver's artifacts and for nothing else.
A8's rule is unchanged everywhere else — a dispatcher's artifacts live in a
workspace it owns and Atlas describes them — and it assumes the reader is a
person with access to that directory. A plan revision is compiled from stored
bytes, and the workspace is removed when the attempt succeeds, so a planner's
document only described is a plan nobody can ever compile.

Asked of the **role** rather than of an artifact's name, because "a planner
produces documents Atlas compiles" is the property that makes it true and a name
test would stop applying the moment the plan layer named a second file. The
daemon still re-digests what arrives and refuses a manifest that describes one
thing and carries another.

Found by `tests/test_plan_e2e.c`, and reachable by nothing smaller: every unit
and edge test in the season built its artifact rows directly.

### The bounds are compiled in and the worst case is written down

Five planner jobs, three compiled revisions, four stages, eight tasks, three side
tasks per stage, 65536 bytes of document, 16384 of goal, and each stage-run's
existing three repo-tree worker starts. None has a flag, a policy key or a wire
parameter, because a bound a caller can raise is not a bound; `rev_no` and
`stage_no` are additionally bounded by `CHECK`s in migration 25, following M21's
arrangement — the schema is the guarantee and the C check is there so a caller
gets a sentence.

**5 + 3 × 4 × (3 + 3) = 77 worker starts** is the stated worst case per plan. It
ignores that completed stages are never re-run and that a revision which never
compiled spends no stage budget, both of which make the real figure far smaller.
The number exists so that nobody discovers it in a bill.

`PLAN_ITERATION_CEILING` is derived from the same constants plus a margin. It is
a **defect guard and not a budget** — the planner-job ceiling, the revision
ceiling and each run's own worker-start budget are the bounds that matter and the
daemon enforces all three — and it exists so that a defect below cannot become an
unbounded loop.

### The stated costs

Two, and both are documented rather than solved, because every way of solving
them is worse.

**A planner job's own run stays ACTIVE forever.** A planner job is a workspace
job and therefore the root of a workspace-rooted run, and A11.6's settlement asks
the *root* task's driver and refuses to settle a run with no repo-tree root. That
is pre-existing behaviour; A12.0 is the first thing that produces it deliberately
and at a rate of up to five per plan. Letting a gateless run settle would mean a
run reaching ACCEPTED with no gate having run anywhere, which is a far worse
answer than an untidy `job list`.

**A plan whose fifth planner document is format-refused stays PLANNING durably.**
A refusal leaves no row: at k < 5 the next planner job is the durable evidence
that one happened, and at k = 5 there is no next job to be it. The plan is
resumable, and every resume recomputes the same refusal from the same stored
bytes and re-prints it. The alternative — deriving BLOCKED — would say a plan is
finished while a paid, valid document of it could still be ingested.

### A12.0 adds no mechanism

No thread, no process, no timer, no background loop, no scheduler, no signal
handler. No new method group: the four `plan.` names live in the existing
orchestration *client* group, gated by the same `require_submitter` as
`job.submit`, and none of them carries a verb from `VERBS[]`. No MCP tool, no
gateway route, no second submit path. No new isolation: a planner job and a side
task are workspace jobs under A8's existing isolation, and a stage's tree task is
an ordinary A11.1 repo-tree task. No repo-tree driver was added, which is why the
index predicate that keeps the registered tree exclusive did not have to move.

## P0 layers — additions

```
src/daemon/watch.c      the whole of it: the subscriber-set watch map, the budget
                        resolution, the two-phase build, the resumable frontier,
                        the ignore inventory and the pending-decision queue
src/db/db_state.c       the watch reason vocabulary, and the one writer of a
                        watch outcome
src/db/migrate.c        migration 26: `priming`, `watch_reason`, and the split
                        counts, by table rebuild with row-preservation verified
src/core/syspolicy.c    `watch_max_dirs_total`, and the kernel share the
                        deployment shape implies
```

Nothing outside `src/daemon/watch.c` decides how many watches exist. The policy
states a number, the kernel enforces its own, and the watcher does the
arithmetic in one place.

## P0 rules — these are not negotiable

- **A DOCUMENTED BOUND THAT IS NOT THE IMPLEMENTED BOUND IS WORSE THAN NO
  BOUND.** `ATLAS_WATCH_MAX_DIRS` was documented as 8192 per repository and
  enforced as `w->map.count + 1 >= 8192` against the *daemon-global* count. It
  was therefore 8191, daemon-wide, and the repository that lost was chosen by
  `ORDER BY name`. On a machine offering 122,910 watches Atlas stopped at 8,191
  and told the losing repository it had more directories than Atlas will watch —
  a sentence with no true clause in it. The number was in `daemon status` the
  whole time and read as a configuration, not as a defect.
- **Compare with `>=`, never `+ 1 >=`.** Exactly N watches install at a budget of
  N. `tests/test_watch_budget.c` asserts the equality rather than a bound,
  because `<= N` would have passed against the defect.
- **A bound on work and a bound on a resource are different bounds, and each
  says which it is.** The walk's visit ceiling and the watch budget shared one
  boolean and produced one sentence. `atlas_watch_reason` is a closed vocabulary
  precisely so "raise the sysctl", "this daemon is out of budget" and "this walk
  went too far" can never again be the same answer. `UNKNOWN` is zero and means
  *no reason was stated*; `NONE` is the positive claim and only a complete watch
  set may carry it.
- **The budget is derived from the kernel, not compiled.** A watch budget written
  into a header is a guess about a machine its author never saw, and the guess
  Atlas shipped was 8192. The share is 50% under a root-owned system policy and
  20% otherwise, because A7.1's `atlasd` has no other consumer of its inotify
  budget and a per-user daemon shares the uid with every editor the operator
  runs. A policy may state a total; out of range is **MALFORMED, never clamped**,
  and a malformed policy falls back to legacy mode — which removes every
  `client_uid` from the socket, so the binary is installed before the key.
- **Correctness-critical watches are installed first, for every repository,
  before any source tree.** Metadata went in *after* the recursive source walk,
  so a repository large enough to exhaust the budget stopped watching its own
  `HEAD`. Branch correctness must not be contingent on the source tree fitting.
  The reserve is a floor and `ATLAS_WATCH_META_MAX_PER_REPO` is the ceiling;
  making the reserve the cap would recreate 8192 one layer down.
- **There is no fixed per-repository cap, and allocation is order-independent.**
  Each round divides the remaining pool among the repositories that still want
  watches, and one that finishes under its share returns the remainder. A single
  large repository alone gets the whole budget; several large ones share it
  without any of them being told it is too big.
- **A physical watch descriptor and a logical subscription are different things.**
  `inotify_add_watch` on an already-watched path returns the same descriptor. The
  old map stored one `repo_id` per descriptor and the last installer overwrote
  it, so of two registered worktrees of one repository only one ever received a
  shared-ref event, and removing either released a descriptor the survivor was
  relying on. Charge on a new subscription, release at the last one, fan out to
  all of them, and **never claim `sum(per-repo) == total`** — the relation is
  `>=` and `watched_shared` is what explains it.
- **THE IGNORE INVENTORY IS NOT AN AUTHORITY ON A PATH THAT DID NOT EXIST WHEN IT
  WAS READ.** `git ls-files` enumerates the filesystem, so a `build/` rule with
  no `build/` on disk produces no entry. Carrying the inventory forward — the
  obvious fix, and the one the first review caught — would have answered "not
  ignored" with confidence for exactly the case the mechanism exists to handle.
  A directory Atlas has not seen is not watched and not descended into; it waits
  in a bounded queue while **one** `git ls-files` per debounce tick answers for
  the whole queue. `git check-ignore` is not used and is not on the allowlist:
  its `-z` requires `--stdin`, and `atlas_proc` gives every child `/dev/null` for
  stdin.
- **Waiting is an event gap, and it is recorded as one.** Nothing under a queued
  directory is watched while it waits. While the queue is non-empty the
  repository is `priming` and its index is **not current**; a directory that
  turns out to be visible leaves the repository owing a content-verifying pass.
  The same applies to every window in which the watch set is being rebuilt — a
  repository-set change, an ignore-rule change — and none of them may end in
  `watching`, `event_gap=false` or `index_current=true` until a full pass has
  actually completed.
- **`info/exclude` needs its own subscription.** An inotify directory watch
  reports its direct children only: watching `.git` produces events for
  `.git/config` and **nothing at all** for `.git/info/exclude`. Verified by
  experiment before the code was written, because the opposite was assumed in a
  draft of this plan and was wrong. It resolves to the *common* directory even
  from a linked worktree, so one descriptor serves every worktree sharing it —
  which the subscriber set makes safe.
- **`core.excludesFile` is a stated cost, not a solved problem.** It normally
  lives outside the repository root and Atlas never watches outside a repository
  root. A change to it is picked up by the periodic pass.
- **Priming is resumable and yields.** The watcher does not poll inotify while it
  walks, and `IN_Q_OVERFLOW` is global to the instance — one repository's priming
  could gap every repository at once. The frontier is depth-first so popping
  truncates and reclaims, and it lives on the repository so a walk can be
  suspended; a walk that runs to completion inside one call gets its **own**
  frontier, because borrowing the repository's would silently discard a suspended
  one. The measured 0.15 s walk of a 31,611-directory tree is **not** the reason
  this is safe; chunking is.
- **`priming` is neither watched nor degraded.** Counting it as watched would let
  `watching == repositories` be true with part of a tree unobserved; counting it
  as degraded would make every ordinary startup look like a fault.
- **The proven envelope and the hard ceiling are different fields.**
  `ATLAS_WATCH_DIRS_HARD_CEILING` is where a configured value is refused;
  `ATLAS_WATCH_PROVEN_ENVELOPE_DIRS` is what was measured, and it is the only
  figure documentation may claim — and only where the resolved budget actually
  reaches it. A machine whose kernel or policy resolves lower reports its own
  effective envelope and claims nothing beyond it.
- **The test channel is not a public surface.** A boundary test needs a small
  budget, and a CLI flag or environment variable for it would be a second answer
  to a question `/etc/atlas/system.conf` owns, reachable by anyone who can start
  a daemon. It travels on `atlas_daemon_opts` and reaches the watcher's own
  parameter; an injected value replaces the resolved total and nothing else, so
  the comparison, allocation and accounting under test are production's.
- **One writer for a watch outcome.** The healthy path wrote a count and the
  degraded path did not, so a degraded repository reported whatever it held the
  last time things went well. State, reason, detail and all three counts travel
  as one struct through one function, and the gap flags are written in the same
  job so they cannot reorder against it.
- **No new thread, process, timer or background loop; no new RPC method, MCP tool
  or gateway route.** Every new field is additive on the wire except
  `watched_directories`, whose *value* semantics changed — and that is recorded
  as a compatibility change rather than described as additive.

## A13 layers — additions

```
include/atlas/mirror.h    the read side's answer to "which bytes is this
                          repository read from?"
include/atlas/scanner_uid.h  who may be a scanner, and who may never be
src/core/mirror_open.c    atlas_repo_open_git — real root first, mirror second
src/core/scanner_uid.c    the owner of the root, and the refusals
src/core/service_scanner.c the scanner's walk: the tracked tree and .git
src/daemon/mirror.c       the write side: openat, O_NOFOLLOW, one repo at a time
src/ipc/server_scanner.c  scanner.poll and scanner.put, and peer_owns
```

`src/db/migrate.c` gains migration 27, which adds `repositories.scanner_uid`
with `DEFAULT 0`. It is **the first migration since 2 to alter `repositories`**,
which is why four test helpers that wind the schema back needed
`ALTER TABLE repositories DROP COLUMN scanner_uid` and four schema-version
tripwires fired exactly as designed.

## A13 rules — these are not negotiable

The one-line forms are in `CLAUDE.md`. The reasoning that is not obvious from
the code, and that a later reader is most likely to undo:

**Why the mirror is a whole git repository rather than an observation stream.**
The spec had the scanner report observations — file identities, hashes, status
entries — and the daemon rebuild the index from them. Counting before
implementing settled it: `src/core/reconcile.c` calls **twenty distinct git
operations**. Reproducing them over a socket does not move data; it moves the
*execution of A1's cache-hit rules* — the ones this document marks "do not
weaken these" — into a process the daemon cannot audit. The eight-field
filesystem identity, the "a path the watcher named is always hashed" rule and
the `content_verified` gate would all have become claims a client makes rather
than facts the daemon establishes.

Measured instead: git works unchanged on a copied `.git`. `rev-parse HEAD`
returns the same commit, `ls-files` the same 410 paths, and `status` reports
zero changes — that last one mattered, because an index carrying the *source*
worktree's stat data could have made every file read as dirty. The cost is
256 MB against a 3.0 GB index, about 8 %.

**Why the real root is tried first rather than the mirror being preferred for
repositories that have one.** Reading the thing itself is better evidence than
reading a copy of it. A preference for the mirror would also make the daemon's
answer depend on whether a scanner had run recently, which is a freshness
property, rather than on what the repository contains.

**Why `data_dir == NULL` is the guarantee.** A boolean parameter would have to
be passed correctly at every call site to preserve pre-A13 behaviour; an absent
argument preserves it by default. The three readers that pass NULL do so for
reasons that differ and are each written at their call site.

**Why the two identity checks are skipped rather than adapted.** They assert
that the registered root still resolves to itself. When the mirror answered, the
registered root was not opened, so the assertion has no subject — it is unasked,
not failed. Adapting them to compare against the mirror's path would assert
something trivially true (Atlas built that path) and would read like a check.

**Why orchestration never reads a mirror, in the terms A11.1 uses.** A repo-tree
worker edits the real tree. `head_commit` is what makes "the pinned commit is
checked before the worker and again after it" enforceable. Reading a mirror
there would compare the worker's real work against a copy taken before it, so a
HEAD that moved underneath the worker would compare equal — the guarantee would
not fail loudly, it would **silently always pass**. A snapshot built from a
mirror would hand a worker a lagging base and make every downstream gate verdict
describe work done against the wrong tree.

**Why a mirror-backed watch owes an event gap on every build.** The watch set is
rebuilt from scratch and keeps no memory of which tree answered last time, so
"only on a change of source" is not a question this code can ask. Owing one
unconditionally is the conservative direction and is the obligation P0 already
takes for a rebuilt watch set.

**What is not solved, stated so nobody discovers it as a surprise.** A scanner
that stops running leaves a frozen mirror, and nothing bounds its age: the
daemon keeps indexing it and keeps reporting the index current. Closing it needs
a recorded observation time and a staleness rule. The stored record also does
not say which principal produced a repository's facts — it is logged, not
stored, and storing it needs a column and therefore a migration.

## A12.1 layers — additions

```
include/atlas/memory.h    the five vocabularies, the bounds, every public
                          atlas_memory_* entry point
src/memory/source.c       vocabulary name/parse pairs, the policy value
                          parser, class -> reading-principal map
src/memory/read.c         reading one source's current bytes, by the right
                          principal
src/memory/extract.c      candidate split, normalisation, anchor resolution,
                          verifier assignment
src/memory/reconcile.c    observe phase, apply phase, generation, diff,
                          trailer scan
src/memory/pack.c         Context Pack build, freeze, freshness, render,
                          compose, the reliance match
src/memory/trailer.c      trailer compose and ingest
src/memory/patch.c        the proposed-deletion patch for a hand-authored
                          source
src/db/db_memory.c        typed operations over migration 29's tables
src/core/service_memory.c the service layer for the CLI family
src/ipc/server_memory.c   memory.put, memory.status, memory.reconcile
```

`src/db/migrate.c` gains **migration 29**, eight tables (`memory_sources`,
`memory_source_versions`, `memory_claim_anchors`, `memory_generations`,
`memory_claim_diffs`, `memory_unanchored`, `memory_context_packs`,
`memory_trailer_bindings`), and **migration 30**, two `ADD COLUMN`s:
`repositories.trailer_scan_high` and `memory_trailer_bindings.bound_hit`.
Migration 30 arrived inside a fix round, after two earlier notes in this
season's own working record had said no further migration would be needed —
both were wrong, and are corrected rather than repeated.

Modified rather than created: `include/atlas/verify_ops.h` and
`src/verify/intake.c` (`ATLAS_VERIFY_CHANNEL_DOCUMENT`, the
`memory_version_uid` reference field); `include/atlas/verify.h`,
`src/verify/verify.c` and `src/verify/autolifecycle.c` (the conflict
producer, the aggregation algorithm's version bump); `src/daemon/writer.c`,
`src/daemon/daemon_internal.h` and `src/daemon/watch.c` (the two job kinds,
the watcher-tick sweep); `src/ipc/server.c` (dispatch and the operator method
table); `include/atlas/orch_ops.h`, `src/db/db_orch.c`, `src/ipc/server_orch.c`,
`src/orch/rundriver.c` and `src/orch/dispatch.c` (pack freeze, delivery,
injection, the completion's `touched_paths`); and the CLI family across
`src/cli/cli.c`, `src/cli/render.h`, `src/cli/render_human.c` and
`src/cli/render_json.c`.

## A12.1 rules — these are not negotiable

The one-line forms are in `CLAUDE.md`; the full argument for every decision
this season made is in `docs/context-reconciliation.md`. What follows here is
the reasoning that is easiest to get wrong by rediscovering it from the code
alone — the places a later editor is most likely to "fix" something that was
already deliberate.

**Why `SELF_DECLARED`, not `ATLAS_ATTESTED`, for a document actor.** Atlas
genuinely did perform the read — opening the file, hashing it, resolving its
commit — and that fact is recorded honestly as `ATLAS`-channel evidence. But
the *identity* axis asks something different: how well Atlas knows who is
speaking. Inside a memory file the speaker is unestablished by construction,
and choosing the stronger identity because Atlas trusts its own read pipeline
would confuse "Atlas read this" with "Atlas vouches for what it says." The
arithmetic makes the stakes concrete: `ATLAS_ATTESTED` would give a memory
file a prior of 400 — ahead of a self-declared AI agent's 350, but behind
`ATLAS_VERIFIER`'s 900, `TOOL`'s and `TEST`'s 700, `RUNTIME_OBSERVATION`'s
and `REPOSITORY_EVIDENCE`'s 650, and `HUMAN`'s 500, seventh of the nine
class bases rather than second — a sentence typed into a project's own
memory file would still outrank the model that wrote it speaking for
itself, and every additional memory file would mechanically raise the
ceiling on how confident Atlas could become about anything. `SELF_DECLARED`
gives 350, the same tier as the model, which is the honest answer to "how
sure is Atlas who is talking."

**Why the anchor bound is reachable by accumulation and not by merging, and
why that changes what the bound protects against.** The first explanation
this season gave for how a claim could exceed its per-proposition anchor
limit was that two memory documents asserting one proposition merge onto a
shared claim and each contributes its own anchors. That explanation was
wrong, checked and replaced: anchor resolution runs off a proposition's own
text alone, with no reference to which document produced it, so two
documents stating one proposition already produce byte-identical anchor
tuples that a uniqueness constraint collapses before the claim-level bound
could ever see two contributions. What actually grows without limit is the
union *across reconciliation passes* on one claim identity that never
changes: nothing deletes an anchor except the one narrow case of a
proposition being re-minted under a new claim uid, so a claim whose text
drifts slightly every few passes keeps every anchor it has ever resolved,
forever. The distinction matters operationally, not just for accuracy: a fix
aimed at "two documents" would have done nothing to the failure that is
actually reachable, which needs only one source, read repeatedly, over time.

**Why the pack's decision-set and source-set digests are computed live at
build and freshness time, rather than read off the last stored generation
row.** Reading them from the generation looks cheaper and is wrong for a
reason a test would have caught but a design review might not: it couples
two of the six pins together, so both move only when the generation moves,
which collapses two of this project's required freshness scenarios into one
and makes which reason gets reported depend on the order comparisons happen
to run in rather than on what actually changed. Six independent pins need six
independently computable values.

**Why freshness is computed after the freeze transaction commits, not
inside it.** The freeze itself must be a pure read so it can run inside the
transaction that creates a run — no process, no file read. Freshness is a
different question asked at a different moment, and one of its six
comparands, the live source identity, can require opening the tree. Doing
that inside the freezing transaction would violate the rule that no file
read happens inside a write transaction; doing it strictly before the freeze
would only make the answer stale sooner, never fresher, because five of the
six comparands are rows only the writer thread itself ever writes and are
therefore already exactly what an in-transaction read would have found.
Computing it immediately after the commit is the only placement that is both
legal and no worse than any alternative.

**The "value read through a mirror, returned without the fact that it came
from a mirror" defect recurred four times, at four different places to put
the answer, and the pattern is worth remembering on its own.** A whole
gitignored source directory could be silently invisible. One directory level
in, a directory holding one tracked and one ignored file could report the
tracked child with no sign anything was missing. One level further, a
directory holding *only* ignored files produced zero items and a plain
success, with no item left to carry the missing fact on. And finally, a
caller of the read function could simply decline to receive the fact even
where a place to put it existed. Each fix closed the specific case that had
just been found, and the next case that turned up was the one with even less
room to carry the same answer — until the last fix was to refuse a caller
that will not accept the answer at all. Fixing an instance of a defect is not
the same project as fixing the sentence the defect is an instance of, and the
cheapest moment to ask "is there a smaller version of this same shape" is
immediately after the current one is closed, not after the next one is
found.

**Why the trailer-ingestion cursor had to move off the generation row, and
what migration 30 actually buys.** The first design stored the trailer scan's
progress on the same row a reconciliation pass creates only when it finds
something to record — which means a repository whose next several hundred
commits carry no trailer block and no source change would never create a
generation, never advance the cursor, and rescan the same window forever:
*advancing required finding, and finding required advancing.* The fix gives
the cursor its own column on `repositories`, written unconditionally on every
scan regardless of what was found, which is what a cursor has to mean to
behave like one. The bound this buys is statable: a repository with any
finite commit history reaches its own `HEAD` for trailer purposes within a
number of passes equal to its commit count divided by the per-pass scan
bound, rounded up — never "eventually, if something happens to be found."

**Why the deletion predicate for a hand-authored memory file routes every
arm through one function, and why that structure is itself the fix.** The
season's one specification-level defect was here: a plan's own interface
comment allowed a claim's most recent diff being recorded as superseded to
justify a deletion with no accompanying requirement that the claim also be
descriptive, unconflicted and non-normative — an *open* `or`, sitting beside
three absolute exclusions the same comment stated in the same breath. Because
nothing yet produces that diff kind, the gap was unreachable in the shipped
tree, which is exactly the condition under which a gap like this survives
undetected the longest. The fix does not patch the one arm; it makes every
arm's kind-specific test an argument to a single call that enforces the three
absolutes unconditionally, so a fourth arm added later inherits them by
having to go through that call to reach a deletion at all, rather than by
whoever adds it remembering to copy three conditions by hand.

## A15 layers — additions

| File | What it owns |
| --- | --- |
| `include/atlas/review.h` | the sheet model, the verdict vocabulary, the three review-sheet bounds, `atlas_review_sheet_parse`, `atlas_review_intent_allowed` |
| `src/core/review.c` | the grammar parser and the name/parse pairs; no I/O, no database, no git, no process |
| `src/core/service_review.c` | `atlas_service_review_apply`: authority, the terminal check, the file read, the pre-check read, and the loop over `atlas_service_decision_confirm` |
| `include/atlas/gateway.h`, `src/gw/gateway.c` | the route-view accessor `atlas_gateway_api_routes` and the three edited rows (`decision.get`'s `revision`, `gate.check`'s `decision`, `code.impact`'s `symbol`) |
| `src/gw/ui/mission-control.html` | the Review view: the record list, the detail composed from five existing read routes, and the review sheet held in `localStorage` |
| `src/cli/cli.c`, `src/cli/render.h`, `src/cli/render_human.c`, `src/cli/render_json.c` | `atlas review apply`, its five ordinary places, and `COMMANDS[]` |
| `tests/support/pty.h`, `tests/support/pty.c` | the pseudo-terminal helpers, moved out of `tests/test_decision_operator.c` unchanged so `tests/test_review_apply.c` can share them |

**A15 adds no migration.** `MIGRATIONS[]` still ends at 30 — A12.1's. Nothing
this season needed a new table or column: a review sheet exists only as text
in a browser and as a file an operator hands to a local command, never as a
database row, and the operator channel it walks already existed in full.

## A15 rules — these are not negotiable

The one-line forms are in `CLAUDE.md`; the full argument for every decision
this season made, the tier-3 cost list in full, and every finding the
execution of this plan produced beyond what the plan itself claimed are in
`docs/review-surface.md`. What follows here is the reasoning that is easiest
to get wrong by rediscovering it from the code alone.

**Why the queue lives in the browser and not in the daemon.** A queue held by
the daemon needs a write route — the first one on `/api/` — under a scope
some model credential could equally hold, and the gateway's own comment says
outright that its authorization engine cannot tell a model's bearer token
from an operator's browser session apart: both resolve to one `principal`.
A model holding that scope could place "I would approve this" on its own
proposal, and it would appear in the operator's own queue looking exactly
like the operator's own intent — not merely readable by the model, but
*attributable to the operator* the moment it was walked. Keeping the queue in
`localStorage`, under the gateway's own origin, makes that specific forgery
impossible rather than merely inconvenient: the sheet is text the operator
copied out of a page they read, never a row Atlas wrote on anyone's behalf.

**Why a sheet's fifth field is the public prefix and not a confirmation, and
why a sixth field is refused rather than ignored.** The fifth field *is* the
first eight hex of the reviewed revision's content hash — the same string an
operator types on `/dev/tty` — so the sheet is not silent about what will be
typed; what it has none of is a field the walker or the browser ever reads in
place of that typing. A parser that silently dropped an unrecognised sixth
field would make a sheet with a forged confirmation column parse identically
to one without it — the field would still not do anything, but nothing would
tell a reader it had been added. Refusing the whole sheet on a sixth field is
the sheet-grammar mirror of `tests/test_decision_mcp.c`'s rule that no MCP
tool schema may declare a `"confirmation":` property: in both places, the
shape of the input is itself part of the guarantee, not merely its content.

**Why the walker re-reads the record before minting anything, on every
entry, rather than trusting what the browser showed.** The window between a
browser rendering a revision and an operator running `atlas review apply`
minutes or hours later is real and cannot be closed by anything client-side:
a model can revise or a second reviewer can dispose of the same record in
that window. The walker's pre-check — newest revision matches, content hash
prefix matches, status matches what the intent needs — costs no challenge
row when it fails, which is what makes an abandoned sheet cheap rather than
merely safe: a reviewer who queued sixty-four records and let the sheet sit
overnight does not spend sixty-four unconsumed challenges finding that out.

**Why three rows gained a parameter instead of a fourth route being added.**
`gate?decision=`, `decision?revision=` and `code/impact?symbol=` each forward
a name the daemon-side method already read; the alternative — a new route
whose handler reads the same parameter — would be a second way to reach one
daemon method; the table would grow without the read surface growing with it.
Every row still forwards to exactly one method, and forwarding a parameter
never changes which method a row names — so it never touches
`tests/test_gateway.c`'s actual guarantee, the positive allowlist
`READ_METHODS[]` that names every method any row may forward to, and it stays
unmoved by every negative check kept beside it (no operator method, no
`gateway.auth`/`gateway.audit`, no backup-, apikey- or orchestration-group
method, every scope grantable). A row edited to forward to a *different*
method than before is the case that does touch it, and the checklist entry
above says so.

**Why A12.1's finding is shown as one label and not as a general "drift"
indicator.** The only A12.1 output the gateway uid can read at all is
`verify.show`'s `conflict` field, and A12.1 itself established that its
reconciler produces `IMPLEMENTATION` for exactly one shape: a claim carrying
both a decision anchor and a symbol anchor, extracted while the symbol
resolved, later found not to resolve in a semantic generation the vanish
sweep can show is coverage-complete. A page that rendered every other value
of `conflict` as if it too meant "drift found" would be claiming a detector
this season did not build; saying nothing about the other values is the
honest reading of what one field, produced by one algorithm, for one shape,
actually establishes.

**Why the suite greps the served page instead of running it.** Nothing in
this tree runs a browser, and adding one to test eight lines of DOM
manipulation would be a disproportionate answer to a page this small. What
the grep-and-drive-the-routes test can prove is real and stated precisely:
the served bytes carry the identifiers and sentences each panel depends on,
and the routes those bindings name answer through a real session cookie. What
it cannot prove is that a click renders what the source says it renders — and
`docs/review-surface.md` records one place that gap actually matters: the
review-sheet functions' reliance on holding no `await` between a load and a
save, which no grep can see and no route-driving test exercises.
## A16 layers — additions

| File | What it owns |
| --- | --- |
| `include/atlas/orch_remote.h`, `src/orch/remote.c` | the remote-submission policy reader, the policy-key validation, `atlas_orch_remote_policy_check` |
| `src/ipc/server_orch_remote.c` | the four `job.remote_*` daemon-side method implementations |
| `src/gw/gateway.c` | the four new write routes, the `remote_dispose_key` credential verification path, the `A16` section of `API_WRITE_ROUTES[]` |
| `src/gw/ui/mission-control.html` | the Jobs view (submit, status, list, cancel) and the disposal digest field in the decision detail pane |
| `include/atlas/api_key.h`, `src/core/api_key.c`, `src/db/db_api_key.c` | the `remote_dispose_scope` derivation; `ATLAS_SCOPE_DISPOSE` marked not grantable |
| `src/db/migrations.c` | migration 31: `ALTER TABLE orch_jobs ADD COLUMN dispose_key_id TEXT NOT NULL DEFAULT ''` |

**A16 added migration 31.**

## A16 rules — these are not negotiable

The one-line forms are in `CLAUDE.md`; the full argument and the honest paragraph
are in `docs/browser-disposal.md`.

**Why `remote_dispose_key` is a separate policy key and not a scope the
credential declares.** If `dispose:` were grantable, a credential that leaked
could be read at creation — by anyone who asked the operator to create a disposal
credential and then read it — and reused to dispose of records until it was
revoked. A policy key that names one credential makes the link at the daemon level,
with no scope the credential itself carries. The credential looks like any bearer
token on the wire; only the daemon's policy lookup gives it the capability.

**Why `remote_dispose_key` and `remote_submit_key` may never name the same id.**
A single credential that both submits jobs and disposes of records is a single
capture that grants both powers. The separation forces two captures to achieve
both; that is the whole of what the rule buys, and it buys it at the cost of one
more credential to revoke.

**Why `REMOTE_OPERATOR_CONFIRMED` is in the ledger and `LOCAL_OPERATOR_CONFIRMED`
is not.** A reader of the ledger who finds `LOCAL_OPERATOR_CONFIRMED` knows the
operator was physically present on this machine, with a terminal, typing a
confirmation by hand. A reader who finds `REMOTE_OPERATOR_CONFIRMED` knows only
that the credential named in the policy was used — not who was holding it or
where. The distinction matters; the label exists to preserve it.

## A14 layers — additions

| File | What it owns |
| --- | --- |
| `include/atlas/orch_remote.h` | the submission policy model: driver, mode, gate, bounds, the `remote_submit_key` lookup, `atlas_orch_remote_submit_policy_of` |
| `src/orch/remote.c` | the submission policy reader and the cross-check helpers |
| `src/ipc/server_orch_remote.c` | the four `job.remote_*` daemon-side method implementations; head comment carries the `require_submitter` honesty sentence |
| `src/gw/gateway.c` | the four new write routes under `API_WRITE_ROUTES[]`, the `A14` block, `operator_accepts_cleartext_submission` handling |
| `src/gw/ui/mission-control.html` | the Jobs view: submit form, status card, list and cancel; the cleartext-submission warning rendered from `/auth/me` |
| `include/atlas/api_key.h`, `src/core/api_key.c`, `src/db/db_api_key.c` | the `jobs:submit` scope marked `grantable = false`; derivation for keys named in `remote_submit_key` |
| `src/db/migrations.c` | migration 32: `submit_key_id` on `orch_jobs`, `key_id` on `orch_transitions` |
| `src/mcp/mcp_tools.c` | the four remote-only MCP tools (`atlas_job_submit`, `atlas_job_status`, `atlas_job_list`, `atlas_job_cancel`) with `remote_only = true` |
| `docs/remote-submission.md` | the season's own document: the honest paragraph, the cleartext chain verbatim, the operator's three decision rows |

**A14 added migration 32.**

## A14 rules — these are not negotiable

The one-line forms are in `CLAUDE.md`; the full argument, the cleartext chain
verbatim, and the operator's decision rows are in `docs/remote-submission.md`.

**Why `require_submitter` is never called on this path.** `require_submitter`
guards the gate methods for the operator-uid group, checking that the submitting
peer is the expected user. The remote submission path is not in the operator-uid
group; it is in the gateway-uid group, validated by a credential, and the daemon
grants the capability through the policy, not through the peer uid. Calling
`require_submitter` there would be a check against the gateway uid — always
true and therefore meaningless.

**Why the policy — not the request — decides the driver, mode, gate floor and
bounds.** A request that could name its own driver or lift its own gate floor
would be a request that could choose its own verification. The policy is
root-owned and the request is model-controlled; keeping them separate is the
whole of what this rule buys.

**Why `jobs:submit` is not grantable.** If it were, an operator could issue
a credential that grants submission to anyone who held it — including an MCP
session, which can observe the credential and relay it. The non-grantable design
means the only credentials with the scope are the ones the policy names by id,
and naming a credential requires editing a root-owned file.

**Why the unattended shape and the deferred shape both run as
`model_dispatcher_uid`.** That is what `model_dispatcher_uid` means: the account
a worker runs as, named in the policy, irrespective of how the job was submitted.
The gateway queues the job and the daemon starts the worker; neither changes which
account the worker inherits. Stating that a worker "runs as the operator's own
account" is true on this deployment and must be stated truthfully.

**Why the cleartext chain is stated, not fixed.** Atlas does not terminate TLS,
the listener is `tls_mode = NONE` on this machine, and the operator accepted the
chain. The alternative — refusing to build the feature without TLS — would leave
the capability absent not because it is harmful but because nobody resolved the
deployment constraint. The honest answer is to state the chain and let the
operator decide; they did, on 2026-09-04, with the chain in front of them.

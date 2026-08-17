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
                              `atlas-reliability-v1`; touches no database
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
  to refuse. The two failure directions are closed by different mechanisms:
  `atlas_verify_channel_parse` refuses the `ATLAS` name, and
  `atlas_verify_channel_authority` refuses every raise. Claiming less authority
  than you hold is never a forgery; it is the accurate statement.
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

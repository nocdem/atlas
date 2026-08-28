# Atlas — extension checklists

One checklist per extensible vocabulary, table, method table or bound. They were
in `CLAUDE.md` until A9.2.4 moved them; see `docs/engineering-rules.md` for why.

Each list exists because the thing it describes is edited in more places than
anybody remembers, and the failure mode of missing one is almost never a build
error. It is a value that silently reads as its zero, a socket path and a local
path that quietly disagree, a test that stops being in the subset people run, or
a command that is fully wired and answers `unknown command`. Every entry below
was written after one of those actually happened.

The rule underneath all of them: **when you add a member to a closed vocabulary,
find every place that switches on it, and every place that stores it.** The
compiler finds the first kind and not the second.

## Extending A8-CI safely

- **A new evidence class** means editing `atlas_sem_evidence`, `strength()` in
  `src/sem/sem.c`, `atlas_sem_evidence_weaker`, every CHECK naming the class in
  migration 11, and the enumerated expectations in `tests/test_sem.c`. Keep
  UNKNOWN at zero.
- **A new edge kind** means a member, a name, a row in
  `atlas_sem_edge_kind_max_evidence` stating the strongest class it may carry,
  and the CHECK on `sem_edges.kind`. A kind whose ceiling is not stated cannot
  be written.
- **A new fact the parser emits** changes what identical bytes produce, so bump
  `ATLAS_SEM_ANALYZER_VERSION`. The next pass then rebuilds and the graph is
  reported stale until it does.
- **A new bound** goes in `include/atlas/limits.h` under the A8-CI section with
  a written reason, and is reported when reached. A bound that silently trims a
  result is the one thing this layer must never have.
- **A new RPC method** goes in `SEM_METHODS[]` and must be a read. If it is
  plausibly a mutation verb, add its name to the negative enumeration in
  `tests/test_orch_rpc.c`. A method that *mutates* goes in the operator-uid
  table instead, and which table it goes in is the security decision.
- **A new MCP tool** follows the A2 rule and changes the pinned count in
  `tests/test_plugin.c`. It must not be able to build or invalidate an index.

## Extending A9.2.3 safely

- **A new semantic activity state** means a member of `atlas_sem_activity`, a
  case in `atlas_sem_activity_name`, the parse table in `read_sem_plan`
  (`service_remote.c`), both renderers, the Mission Control row, and the table in
  `docs/semantic-freshness.md`. Keep UNKNOWN at zero, and decide explicitly
  whether `should_build` may be true in it.
- **A new hold reason** means a `ATLAS_SEM_HOLD_*` literal, a row in the table in
  `atlas_sem_hold_intern`, and one written sentence of meaning. A value that
  arrives over a socket is a *matching* string and must be interned to Atlas' own
  literal before it reaches an operator or a model.
- **A new staleness reason** means an `ATLAS_SEM_STALE_*` literal, a row in
  `atlas_sem_stale_reason_intern`, and a branch in `atlas_sem_freshness_of`
  placed deliberately: the order is the order an operator wants to be told, and
  the broadest check goes last so a repository is told *why* in the most useful
  terms available.
- **A new input to the source identity** changes what every stored identity
  means, so it invalidates every generation's freshness comparison at once. Bump
  the domain string in `atlas_sem_source_identity`, and add a row to the table in
  `docs/semantic-freshness.md` with a reason.
- **A new coverage manifest field** goes on `atlas_sem_generation`, in migration
  N with its vocabulary's zero as the default, in `GEN_COLUMNS` and
  `read_generation`, in `atlas_db_sem_scope_set`, on the wire in
  `server_sem.c`, **read back** in `service_remote.c`, and in both renderers.
  Missing the read-back is how the socket path and the local path start
  disagreeing.
- **A new coverage dimension's source** — deciding whether it follows the units
  or the scope manifest — needs the argument in `sem_coverage`. "It is more
  conservative" is not one on its own: a gate applied where it does not belong
  makes Atlas uselessly cautious rather than correctly cautious, and the
  asymmetry rule exists because that is a real failure mode.
- **A new field on the build description** means a column in migration N, the
  struct, `atlas_db_sem_config_get`/`_set`, the job payload, the CLI flag, the
  RPC parameter and the negative enumeration if it is plausibly an authority
  verb. Decide whether an absent value means "leave alone" or "clear" and make
  the two expressible separately.

## Extending A9.2.2 safely

- **A new verifier** additionally needs a row in
  `atlas_verify_verifier_truth_of_check` and one in
  `atlas_verify_verifier_absence_dims`. `tests/test_verify_absence.c` fails a
  verifier that can conclude ABSENT while declaring no dimension, and one whose
  PASS and FAIL mean the same thing.
- **A new coverage dimension** means a member, a row in `COVERAGE_DIMS[]` with
  its name and the truth reason its insufficiency implies, a wider
  `ATLAS_VERIFY_COVERAGE_DIMS` — a static assertion refuses otherwise — and a
  decision for every verifier about whether its negative depends on it.
- **A new truth value** means editing the enum,
  `atlas_verify_truth_is_established`, `_contradicts`, `_change_of`, the CHECKs on
  `verify_results.truth` and `verify_outcomes.prior_truth` — so a **migration** —
  and the enumerations in `tests/test_verify_absence.c`. Keep UNKNOWN at zero.
- **A new field on the assessment** goes in
  `atlas_service_verify_write_assessment` **and** is read back in
  `service_remote.c`, and is rendered by both renderers. Missing the read-back is
  how the socket path and the local path start disagreeing.

## Extending A9.2.1 safely

- **A new MCP verification tool** states its scope in `tool_def`, keeps the
  authority-verb ban, and changes the pinned count in `tests/test_plugin.c`. If it
  writes, it maps to `ATLAS_SCOPE_MEMORY_WRITE`, which no credential can hold.
- **A new gateway route** must be a read, for the reason above. A mutating one
  needs an argument that survives the "leaked bearer token" test, and intake has
  not survived it.
- **A new field on the readable detail** goes in `atlas_verify_evidence_detail` or
  `atlas_verify_attestation_detail`, is written by
  `atlas_service_verify_write_detail`, is read back by `read_detail` in
  `service_remote.c`, and is rendered by both renderers. Missing the read-back is
  how the socket path and the local path start disagreeing.

## Extending A9.2 safely

- **A new deterministic verifier** means a member of `atlas_verify_verifier`, a
  row in `VERIFIERS[]` with a written description, a case in
  `atlas_verify_run_verifier`, a scope sentence in `scope_of`, and — the part
  that is not optional — a written argument that it is a **read**.
- **A new basis** means editing `atlas_verify_basis_writable`,
  `atlas_verify_basis_requires_calibration` and
  `atlas_verify_basis_may_verify_semantics`, plus the CHECK on
  `verify_results.basis`. Deciding whether it requires calibration is the whole
  point of adding it. Keep UNKNOWN at zero and unwritable.
- **A new reason** means a member and a row in `REASONS[]` carrying its name, the
  verdict it implies on its own, and one written sentence of meaning. A6's
  arrangement: the verdict follows from the reason rather than being chosen
  beside it.
- **A new evidence class** means a member, a name, a row in
  `atlas_verify_evidence_family_of`, the CHECK on `verify_evidence.class`, and a
  decision about whether it may be a root. Bump `ATLAS_VERIFY_FAMILY_VERSION`
  when the map changes, so stored results are not reinterpreted.
- **A change to the aggregation** means a new `ATLAS_VERIFY_ALGORITHM` string,
  because a stored result records the algorithm that produced it and a future
  version must not silently reinterpret a past one. Same for
  `ATLAS_VERIFY_PRIOR_VERSION`.
- **A new policy key** means a branch in `atlas_verifypolicy_parse_buffer`, a
  field, a documented line in `deploy/a92/verification.conf.template`, and a case
  in the malformed matrix. An unknown key stays an error.
- **A new struct field on `atlas_migration`** means auditing every construction
  site: `tests/test_db.c` built two of them field by field and silently stopped
  being complete when A9.1 added `foreign_keys_off`, which UBSan reported only
  once A9.2's schema bump changed the stack layout. Zero first, assign after.

## Extending A9.1 safely

- **A new knowledge kind** means a member of `atlas_decision_kind`, a row in
  `KINDS[]` carrying its name, whether it is resolvable and one written sentence of
  meaning, a wider `ATLAS_DECISION_KIND_MAX` (a static assertion in
  `src/decision/decision.c` refuses otherwise), the string in
  `atlas_decision_kind_list`, the CHECK on `decision_documents.kind` — which means a
  **migration**, because SQLite cannot widen a CHECK in place — the `KIND_ENUM[]`
  schema list and the `<select>` in `mission-control.html`, the table in
  `docs/decision-lifecycle.md`, and the enumerated expectations in
  `tests/test_decision_kind.c` and `tests/test_decision_model.c`. Keep DECISION at
  zero.
- **A new lifecycle state** is A4's checklist plus this one: the precedence in
  `recompute_status` and in `atlas_db_decision_verify` must be edited together, and
  the 25-pair enumeration in `tests/test_decision_model.c` grows by a row and a
  column.
- **A new migration that rebuilds a table** decides `foreign_keys_off` deliberately
  and states why in the row. If any child declares `ON DELETE CASCADE`, the flag is
  required and the rebuild must verify its own row counts before committing.
- **A new envelope field** goes in the `KEYS` list in `tests/test_ai_trust.c`, which
  A9.1 widened from line prefixes to every `key=` on a line — appending a field to a
  line somebody already listed used to pass unnoticed.

## Extending A9 safely

- **A new scope** means a member of `atlas_apikey_scope`, a row in `SCOPES[]`
  stating whether an operator may grant it, the table in
  `docs/remote-access.md`, and a decision about every existing tool. Keep
  UNKNOWN at zero.
- **A new MCP tool** must state its scope in `tool_def`; the initialiser does
  not compile without one. A write tool maps to `ATLAS_SCOPE_MEMORY_WRITE`.
- **A new API route** is a row in `API_ROUTES[]` naming its daemon method, its
  scope and the parameters it forwards. It must be a read. A route that mutates
  needs a write scope no A9 credential can hold, which is the argument it has to
  survive.
- **A new gateway policy key** means a branch in `atlas_gwpolicy_parse_buffer`,
  a field, a documented line in `deploy/a9/gateway.conf.template`, and a case in
  the malformed matrix in `tests/test_gateway.c`. An unknown key stays an error,
  and a ceiling may only lower the compiled-in bound.
- **A new response header** goes in `atlas_http_write_head`, which is the one
  writer, so no route can invent a header set or forget the security ones.

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

## Extending A9.2.4 safely

- **A new discovery state** means a member of `atlas_sem_discovery`, a case in
  `atlas_sem_discovery_name` and `_parse`, a wider CHECK on
  `sem_repo_config.discovery_state` **and** `sem_generations.discovery` — which
  means a **migration**, because SQLite cannot widen a CHECK in place — the fold
  in `coverage_is_complete`, the mapping in `sem_coverage`
  (`src/verify/detverify.c`), both renderers, the Mission Control row, and the
  table in `docs/semantic-discovery.md`. Keep UNKNOWN at zero, and decide
  explicitly whether an absence may rest on it.
- **A new rejection reason** means an `ATLAS_SEM_REJECT_*` literal, a row in the
  table in `atlas_sem_reject_intern`, a row in the table in
  `docs/semantic-discovery.md`, and one written sentence of meaning. A value that
  arrives over a socket is a *matching* string and must be interned to Atlas' own
  literal before it reaches an operator or a model.
- **A new discovery bound** goes in `include/atlas/limits.h` under the A9.2.4
  section with a written reason, is reported through `note_limit`, and makes the
  search PARTIAL when it is reached. A bound that silently trims a search is the
  one thing this layer must never have — a smaller search that says nothing is
  indistinguishable from a repository with nothing more to find.
- **A new input to the discovery identity** changes what every stored identity
  means, so bump `ATLAS_SEM_DISCOVERY_DOMAIN` *and* the domain string in
  `atlas_sem_source_identity`, and add a row to the table in
  `docs/semantic-discovery.md`.
- **A new activation intent or provenance value** means a member, a name, a
  parse, a wider CHECK on `sem_repo_config.auto_intent` / `auto_intent_by` — a
  **migration** — a case in `atlas_sem_auto_effective`, and a decision about what
  a migrated row becomes. Keep UNSET and DEFAULT at zero, and never let a
  migration invent an intent nobody expressed.
- **A new `sem-config` field** means a column in migration N, a field on
  `atlas_sem_config`, `atlas_db_sem_config_get`/`_set`, a field on
  `atlas_sem_config_job` **and** `atlas_sem_config_request`, the CLI flag and its
  `--no-` clearing spelling, the RPC parameter in `server_backup.c`, the wire
  shape in `server_sem.c`, the **read-back** in `service_remote.c`, both
  renderers, and the negative enumeration in `tests/test_orch_rpc.c` if it is
  plausibly an authority verb. Decide whether an absent value means "leave alone"
  or "clear" and make the two expressible separately.
- **A new coverage dimension** follows the A9.2.2 checklist above, and
  additionally needs a decision, per verifier, about whether its *negative*
  depends on the new dimension. "It is more conservative" is not an argument on
  its own: a gate applied where it does not belong makes Atlas uselessly cautious
  rather than correctly cautious.
- **A new syspolicy key** means a branch in `atlas_syspolicy_parse_buffer`, a
  field, a documented line in `deploy/a71/system.conf.template`, and a case in
  the malformed matrix. An unknown key stays an error, and an unrecognised
  *value* is malformed rather than silently taken as one of the known ones.

## A11.0 — the run, and what may join one

### Adding a value to `atlas_orch_run_status`

The vocabulary is `UNKNOWN`, `ACTIVE`, `ACCEPTED`, `BLOCKED`. Adding a member
means all of:

1. The enum in `include/atlas/orch.h`. **UNKNOWN stays zero.**
2. `atlas_orch_run_status_name` and `atlas_orch_run_status_is_terminal` in
   `src/orch/orch.c`. Neither switch has a `default:`, so both are compile errors
   until you handle the new member — which is the point.
3. `atlas_orch_run_status_parse`'s table, **unless the new member is another
   "nobody filled this in" value**, which must not parse for the same reason
   `UNKNOWN` does not.
4. The `CHECK` on `orch_runs.status`, in a new additive migration. Migration 21's
   own `CHECK` is not edited: an existing database keeps the constraint it was
   created with, and a migration that rewrites a table to widen a `CHECK` is a
   rebuild with all of A9.1's migration-13 obligations.
5. `atlas_db_orch_run_set_status`'s permitted-target test, deliberately, because
   it is a positive list rather than a "not terminal" check.
6. `tests/test_orch_run.c`'s vocabulary case.

**If the new member is terminal**, `submit_resolve_run` already refuses a child
against it through `atlas_orch_run_status_is_terminal` — nothing there needs
editing, which is why that predicate exists rather than a comparison against two
named constants.

### Adding a state to `atlas_orch_state`

A8's checklist still applies, and A11.0 adds one obligation: **the partial unique
indexes spell the terminal set out in SQL**, because SQLite cannot call
`atlas_orch_state_is_terminal`. A new terminal state must be added to those
predicates in a new migration that drops and recreates the indexes.

Since migration 24 there are **two** of them —
`idx_orch_jobs_active_slot` and `idx_orch_jobs_one_active_repo_tree`; migration
21's `idx_orch_jobs_one_active_per_run` no longer exists.
`tests/test_orch_run.c` compares the terminal set against the C predicate and
`tests/test_orch_parallel.c` compares both index predicates over the whole
vocabulary, so this cannot be forgotten silently — but it can only be *fixed* in
a migration, not by editing an old one.

### Who may settle a run

**Nothing in production calls `atlas_db_orch_run_set_status`.** Before adding the
first caller, the question to answer is not "where does this go?" but "whose
authority is this?" — and the answer must not be a model's. A8's rule holds
underneath: a completed job is not an authority, and it approves, applies and
commits nothing. If a run's acceptance ever becomes reachable over the socket,
it belongs in the operator-uid method group, alongside `code.index` and
`code.sem_config`, and never in the ordinary group or in `TOOLS[]`.

### Bounds this season added

| Bound | Where | What happens when it is reached |
| --- | --- | --- |
| a run's active tasks | `orch_runs.max_parallel` (default 1), `idx_orch_jobs_active_slot` and `submit_resolve_run` | the submission is refused, naming the count, the bound and a task in the way |
| one active repo-tree task per run | `idx_orch_jobs_one_active_repo_tree` and `submit_resolve_run` | the submission is refused, naming the task holding the tree |
| one root per run | `orch_runs.root_job_uid`, written once at creation | nothing rewrites it; a run has exactly one root |
| a run's repository | `orch_runs.repo_identity_hash`, compared against every child | a child describing a different repository is refused |
| a run's pinned commit | the root task's `source_commit`, compared against every child | a child pinned elsewhere is refused, naming both commits |

### `ATLAS_ORCH_RUN_MAX_PARALLEL`

The absolute ceiling on how many tasks one run may hold active at once, in
`include/atlas/orch.h`. A run's own bound is fixed at its root task and may be
anything from 1 to this; the default is 1.

Raising it means changing **four** things together, and none of them can be
skipped:

1. the constant itself;
2. `CHECK(max_parallel >= 1 AND max_parallel <= N)` on `orch_runs`, in a **new
   migration** — a CHECK cannot be edited in place;
3. `CHECK(run_slot >= 0 AND run_slot < N)` on `orch_jobs`, in the same migration;
4. nothing in `run_pick_slot`, which reads the constant — but check that the slot
   bitmask type still holds `N` bits.

`tests/test_orch_parallel.c` reads both CHECKs out of `sqlite_master` and
compares them against the constant, so a raise that forgets the migration fails
rather than silently keeping the old ceiling. It also proves both indexes refuse
what the C checks would have, with the write point bypassed entirely.

Lowering it is a different question and is not covered here: stored runs may
already hold a higher bound, and a ceiling that a persisted row violates is a
migration problem rather than a constant change.

## A11.1 — the run driver, the repo-tree drivers, and the gates

### Adding a driver that works in the registered repository's own tree

This is the one extension in Atlas that widens where a child process may write,
so it has the longest checklist and every item is a refusal that has to be
opened deliberately.

1. Add the entry to `DRIVERS[]` in `src/orch/driver.c`, with a `run` function
   that **refuses without an absolute `work_dir` Atlas resolved**. A relative
   path, the caller's cwd or a workspace is a different repository than the one
   the job was authorised over.
2. Add the name to `REPO_TREE[]` in `atlas_orch_driver_is_repo_tree`
   (`src/orch/orch.c`). This is the one list; it is not a flag on
   `atlas_driver`, because the daemon must answer it about a stored name before
   granting a lease without linking the driver table into that decision.
3. Update the count in `tests/test_a11_run.c`'s agreement case, which walks every
   shipped driver.
3a. **Add the name to `ORCH_SQL_REPO_TREE_DRIVER` in `src/db/db_orch.c` and to
   `idx_orch_jobs_one_active_repo_tree` — the latter in a new migration**, because
   an index predicate cannot be edited in place. Since A11.6 the schema is what
   keeps the registered repository's tree exclusive, and a repo-tree driver
   missing from that predicate is one two tasks of a run could hold at once. It
   also feeds the repo-tree worker-start budget. `tests/test_orch_parallel.c`
   compares the SQL list against `atlas_orch_driver_is_repo_tree` over
   `atlas_drivers()` in both directions and fails if they disagree, so this
   cannot be forgotten silently — but it can only be fixed in a migration.
4. Handle `req->ws == NULL`: a repo-tree driver has no workspace, so its log goes
   into `res->log` and it must not write into the repository.
5. The operator must add `driver = <name>` to `/etc/atlas/orchestration.conf`
   before anything can be submitted with it. **Do not add it for them.**

What you get for free, and must not undo: `op_lease` refuses to grant it to a
lease with an empty driver filter, `atlas_service_dispatcher_run` keeps it off
every background dispatcher's derived filter, `op_submit` refuses it without a
gate, and `settle_run_after_complete` settles the run its task belongs to.

### Adding a program to the validation allowlist

`ALLOWED[]` in `atlas_validation_program_allowed` (`src/orch/validate.c`). It is
deliberately tiny and deliberately contains no shell.

Before adding one, the question is not "is this program safe?" but "is this the
operator's decision or the submitter's?" — the allowlist exists so that a job
cannot name an arbitrary program and a planted `PATH` cannot select one.
`argv[0]` is resolved against a fixed search path, never against the
environment's.

A program that reads its arguments as a script, or that can be made to execute
one, does not belong here whatever it is called.

### Changing the worker-start bound

`ATLAS_ORCH_RUN_MAX_WORKER_STARTS` in `include/atlas/orch.h`. It is compiled in,
has no policy key and no flag, because a bound a caller can raise is not a bound.

It counts transitions to RUNNING in the ledger and is stored nowhere. If you ever
find yourself adding a column for it, stop: the reason it is derived is that a
process can die between incrementing a counter and using it, and the ledger row
is written before the exec that it counts.

`ATLAS_ORCH_MAX_ATTEMPTS` is a different bound with a different subject — per
task, not per run. Both apply and the tighter wins; never compare them.

### Adding a field to the completion's artifact manifest

The wire form is `<name>\x1f<kind>\x1f<sha256>\x1f<size>` with an **optional
fifth field**, the content as lowercase hex. Four fields and five are both valid,
which is what keeps an A8 dispatcher speaking to an A11 daemon unchanged; a
sixth must preserve that property the same way.

Who sends the fifth field, and why each one does:

- **The A8 dispatcher does not**, for an executor driver: its artifacts live in a
  workspace it owns and Atlas describes them.
- **The run driver always does**, because it has no workspace at all and an
  artifact it does not carry is one nobody can ever read.
- **The dispatcher does, for a PLANNER-role driver** (A12.0,
  `artifacts_travel_inline` in `src/orch/dispatch.c`). A plan revision is compiled
  from stored bytes and from nothing else, deliberately, and the workspace is
  removed when the attempt succeeds — so a planner's document only described is a
  plan nobody can ever compile. Asked of the **role**, not of an artifact's name,
  because "a planner produces documents Atlas compiles" is the property that makes
  it true and a name test would stop applying the moment the plan layer named a
  second file.

The daemon recomputes the digest from the bytes and refuses a manifest whose
declared size or digest does not match what arrived — a record that describes one
thing and carries another cannot be checked afterwards.

### Adding a client method to the orchestration group

`ORCH_CLIENT_METHODS[]` in `src/ipc/server_orch.c`. Two constraints
`tests/test_orch_rpc.c` enforces and one that is not mechanical:

1. The name must begin `job.` — which group a method is in is visible in the name
   a caller types. (`job.run_status` is named that way for exactly this reason.)
2. It must contain no verb from `VERBS[]`: approve, apply, commit, push, merge,
   grant, restore and the rest.
3. **A method that writes a run's status does not belong here at all.** A11.1's
   settlement travels on `dispatch.complete`, in the transaction that justifies
   it, which is what makes "a model payload cannot accept a run" true by absence.
   Adding `job.run_settle` would undo that in one line.

### Acting on the daemon's `BUSY` refusal

`ATLAS_IPC_BUSY_TOKEN` and `atlas_ipc_message_is_busy` in
`include/atlas/ipc.h`. The token is the contract; the prose after it is for a
human.

A caller may retry a `BUSY` refusal because the daemon took the write back out of
the queue before anything looked at it. A caller may **not** retry a timeout the
same way: that leaves the job queued and running, so the write does happen and
the caller simply never hears the outcome. Same status, opposite facts. If you
add a retry loop, gate it on this function and on nothing else, and bound it.

### Bounds this season added

| Bound | Where | What happens when it is reached |
| --- | --- | --- |
| three worker starts per run | `ATLAS_ORCH_RUN_MAX_WORKER_STARTS`, counted in the ledger | the run is BLOCKED |
| at least one gate per repo-tree task | `op_submit` | the submission is refused |
| eight gates per run | the CLI's `--gate` parser | the flag is refused, naming the bound |
| 4 KiB of a failing gate's output | `ATLAS_ORCH_GATE_EXCERPT_MAX` | the excerpt says how much it did not show |
| twelve `BUSY` retries per write | `RUN_BUSY_TRIES` | the invocation ends; the run stays ACTIVE and resumable |

## A9.2.5 — the semantic verdict, and where a walk could not look

### Adding a value to `atlas_sem_verdict`

Don't, without a written argument. Three values are the whole model — found, not
found over a universe Atlas can vouch for, and cannot say — and a fourth would
have to answer a question the other three cannot. `UNKNOWN` must stay zero.

If you do: `VERDICT_NAMES[]` in `src/sem/sem.c`, `atlas_sem_verdict_parse`
(which must keep refusing anything unrecognised rather than falling back), the
truth table in `docs/semantic-trust.md`, and `test_verdict_vocabulary`.

### Adding an `ATLAS_SEM_UNK_*` reason

1. The macro in `include/atlas/sem.h`, beside the others, in precedence order.
2. **The `REASONS[]` array in `atlas_sem_unknown_reason_intern`.** A reason not in
   that array is dropped at every surface boundary — the JSON writer, the socket,
   the remote parser — so the answer reaches an operator with a verdict and no
   explanation. This is the step that gets forgotten.
3. The check in `atlas_sem_trust_settle`, placed where an operator would want to
   be told about it: most actionable first.
4. A case in `test_verdict_reason_precedence_is_the_most_actionable`, or the
   ordering can be quietly reshuffled by a later edit.

Note that the four *coverage* reasons are produced by `atlas_sem_coverage_gap`
and not by `settle` directly, because the scheduler asks the same function. A new
coverage dimension goes there, and `coverage_gap_of` in `src/sem/schedule.c` and
`atlas_sem_trust_now` both inherit it.

### Adding a field to the trust block

One struct, one writer, four readers.

1. `atlas_sem_trust` in `include/atlas/sem.h`. The zero must be the safe reading.
2. `atlas_sem_trust_write_json` in `src/sem/sem.c` — the **only** writer. Do not
   add it to `src/cli/render_json.c` or `src/ipc/server_sem.c`; that is the drift
   this function exists to make impossible.
3. `atlas_sem_trust_now_with_default` in `src/sem/index.c`, which must fill it
   from facts the **existing** `live_facts` pass already has. A field that needs a
   new database read puts that read on every semantic query; measure it or find
   another way.
4. `take_trust` in `src/core/service_remote.c`. **An absent key must leave the
   conservative value** — never error, never default to a value that would let an
   absence be believed.
5. The human renderer only if an operator would act on it. Not everything in the
   block belongs on a terminal.

Every value must be an Atlas integer, an Atlas boolean, a string from a checked
Atlas vocabulary, or a checked hex digest — A2's five kinds. Nothing in this
block needs `atlas_safe`, and nothing that would may be added to it.

### Adding an `ATLAS_SEM_OBSTACLE_*` reason

1. The macro in `include/atlas/sem_discover.h`.
2. **`REASONS[]` in `atlas_sem_obstacle_intern`** — interned on the way into the
   database and on the way out of it, so an unlisted reason is silently dropped
   at both ends.
3. The `note_partial_at` call site in `src/sem/discover.c`, with the **path** the
   obstacle is about. A pathless obstacle is the A9.2.4 defect.
4. A case in `tests/test_sem_discovery.c`.

The reason and the path are separate columns and separate JSON keys. Never
concatenate them: a value an operator reads must stay one Atlas owns.

### Bounds this season added

| Bound | Where | Reached ⇒ |
| --- | --- | --- |
| `ATLAS_SEM_DISCOVERY_MAX_OBSTACLES` | `limits.h` | `obstacles_truncated`, reported |
| `ATLAS_SEM_UNIT_TRANSIENT_RETRIES` | `limits.h` | the unit is recorded failed |

Raising either needs an argument about storms. The unit retry bound is
compile-time, per unit and per pass, and nothing durable records that a retry
happened — that is the whole guarantee, and a durable retry counter would replace
it with one that has to be reasoned about after a restart.

## A9.2.6 — how long a caller waits for the writer

### Adding a job kind to `atlas_job_kind`

There are **two** questions now, `job_kind_is_unbounded` and
`job_kind_is_drainable`, both in `src/daemon/writer.c` and **neither with a
`default:`** — so the build fails until the new kind is placed on one side of
each. That is the checklist entry: it is not optional and it cannot be skipped,
by construction. `tests/test_daemon_responsive.c` walks the whole enum and
compares both answers against the documented sets, because a switch that
compiles is not a switch anybody thought about.

**Is it unbounded?** Answer `true` only if the job has no duration Atlas can
state — it runs a compiler, walks a tree, or otherwise does work bounded by the
repository rather than by a statement count. Everything else answers `false`.

What answering `true` costs: a synchronous writer call that meets that job waits
`ATLAS_WRITER_YIELD_GRACE_MS` for it to reach a yield point, and is refused with
`BUSY:` if none comes. For a hook session write a refusal means the record is
*lost*, because hooks fail open. Answering `true` for a frequent job is therefore
not a conservative choice — it is a data-loss choice, which is why reconciliation
answers `false`. See the A9.2.6 section of `docs/engineering-rules.md` for that
argument in full.

What answering `false` costs: a caller — and therefore every other client, since
the serve loop dispatches serially — can wait up to that caller's own timeout.

**May it be drained?** `job_kind_is_drainable` answers whether the kind may run
*inside* a yield of an unbounded job. Answer `true` only when both hold: the
caller is latency-critical, and the tables the job writes are disjoint from
everything a semantic pass or a discovery walk touches. Every `false` gets its
one-line reason written at the case; "not obviously safe" is a reason and is the
right default.

Answering `true` when it is unbounded is the one combination that is never
correct — an unbounded job inside another one has no bound at all — and the
agreement test asserts the implication rather than trusting it. Answering `true`
also means the job may be applied **before** a queued unbounded job that was
accepted earlier: first-in-first-out is preserved among drainable kinds and
within every kind, and deliberately not between the two groups. If the new kind's
ordering against a *queued semantic pass* is load-bearing, it is not drainable.

**The branch that is deliberately untested, recorded rather than left to be
discovered.** Grace-expiry `BUSY` under a *real* non-yielding pass is not
deterministically reachable in a test: it needs a translation unit large enough
to parse for longer than the grace on whatever machine is running the suite, and
a fixture that has to be that big is a fixture that is one compiler upgrade away
from never firing. A test that cannot fail is worse than none. The refusal
contract stays covered by the transport-simulated `BUSY` cases in
`tests/test_a11_run.c` and `tests/test_orch_lifecycle.c`, and by the
refused-then-retried case in `tests/test_daemon_responsive.c`, which simulates
the refusal by not reaching the write point at all — which is exactly what a
back-out is.

### Adding a synchronous writer call

Use `writer_wait_locked`. Do not write a fresh `while (!j->done)` loop: it will be
the tenth copy of a rule that took a season to get right, and the first one to
miss the next change to it.

The caller keeps its own ownership handling, and there are three exits to get
right:

| Outcome | Job ran? | Who frees it |
| --- | --- | --- |
| `j->done` | yes | the caller, after reading the result |
| backed out (`writer_wait_locked` returned `true`) | **no** | the caller, immediately |
| deadline passed | **yes, eventually** | the writer; the caller clears `wants_result` and any pointers into its own structs |

Report the back-out with `WRITER_BUSY_MSG` and nothing else. Its second sentence
— nothing was queued and nothing will run — is what makes a retry safe, and it is
false for the deadline case, which is why the two messages are separate.

### Bounds this season added

| Bound | Where | Reached ⇒ |
| --- | --- | --- |
| `ATLAS_WRITER_WAIT_SLICE_MS` | `limits.h` | nothing; the waiter re-asks the busy question |
| `ATLAS_WRITER_YIELD_GRACE_MS` | `limits.h` | the waiter backs out and reports `BUSY:` |
| `ATLAS_SEM_DISCOVER_YIELD_EVERY` | `limits.h` | nothing; the walk offers the writer thread |

The slice and the yield interval are responsiveness bounds, never correctness
ones: a waiter that never woke early would still return the right answer, just
later, and a walk that never offered would produce the same candidate list.
Lowering the slice wakes an idle daemon more often for nothing; raising it
lengthens the head-of-line stall before a waiter notices it should stop waiting.

The grace is the one to think about before moving. It is measured from a waiter's
**first observation** of an unbounded stretch, never from queue time, and it has
to stay between two figures Atlas already holds: above the gap between two yield
points — one translation unit's parse — or a yielding pass is refused anyway, and
below the smallest synchronous deadline on the writer path, which is a hook's
`AI_WRITE_TIMEOUT_MS` of 4000 ms. Raising it past that would turn every refusal
into a timeout, and those two answers mean opposite things about whether the
write is still on its way.

## A10.1 — the bounded cross-run memory package

### Adding a value to `atlas_orch_memory_mode`

There are two, and `UNKNOWN` is the zero. Before adding a third:

1. Say what it means for *a package's bytes*, not for a policy. `OFF` produces
   no bytes; `BOUNDED` produces at most 12 KiB from at most three runs. A mode
   that does not answer "what does a worker see" is a policy question wearing a
   mode's name.
2. Add it to `atlas_orch_memory_mode_name`, to `atlas_orch_memory_mode_parse`
   and to the `CHECK(mode IN ...)` in `M23_MEMORY` — the switch has no
   `default:`, so the first two stop compiling and the third refuses the write.
   **Do not make `UNKNOWN` parse.** It is the zero and means "nobody filled this
   in"; a stored manifest may never hold it.
3. Decide what an unset operation does. Today `UNKNOWN` is read as `OFF` at the
   write point, and a new mode must not change that: a default that quietly
   enabled memory is the one mistake this milestone cannot make.
4. Extend `tests/test_orch_memory.c` with the OFF/BOUNDED difference case for
   the new mode. "Differs by exactly the package's bytes" is the property, and
   it is the only one an A/B arm rests on.

### Adding a field to a memory record

Every field is either an Atlas-chosen number, a name from a checked vocabulary,
or a bounded excerpt of untrusted text. Before adding one, answer all four:

1. **Could it carry a prompt, a tool argument, a session identifier, a
   credential or a full diff or log?** If it could, the answer is no. The
   guarantee is that `atlas_orch_memory_cand` has no member for those, not that
   something filters them — a filter is a thing that can be wrong.
2. **What is its per-field ceiling, and is truncation announced?** Add an
   `ATLAS_ORCH_MEMORY_*_MAX` beside the others and render through
   `put_untrusted` or `put_untrusted_flat`, which truncate the *encoded* output
   and say that they did.
3. **Does it stay on one line?** A field that can span lines lets a past record
   forge the separators of the ones after it. Use `put_untrusted_flat`.
4. **Does it change the digest?** It does. Say so in the milestone's notes: a
   package rendered by a newer Atlas will not match a digest frozen by an older
   one, which is correct and is why the bytes are stored rather than re-derived.

### Reading the memory package anywhere

Don't. There is exactly one consumer and it is `atlas_orch_memory_compose`,
called once by the run driver to append the package to a task. Nothing branches
on its contents.

That absence is the whole safety argument. The preamble labels the records and
the encoding makes them terminal- and structure-safe, but neither makes
printable prose model-safe — A2 is explicit about that. What makes the package
harmless to Atlas is that no code reads it. A single `strstr` over it, anywhere,
would end that and would not be recoverable by adding a check.

### Changing which runs are candidates

`atlas_orch_memory_lineage` — the object format plus the sorted ingested root
commits, under `atlas.orch.memory.lineage.v1` — is the equivalence. Before
widening it:

1. It is **not** `repo_identity_hash` and must never be described as a half of
   it. A4's rule stands: a path hash is not a repository identity, and
   `repo_identity_hash` is the path-qualified fingerprint, whole.
2. Nothing may ever be authorised, admitted or refused on the lineage value. It
   selects hints. If a change would make it decide something, it is the wrong
   value for that decision.
3. A candidate whose identity resolves to no live registry row stays absent
   rather than being matched approximately.

### Changing the ordering

`cand_cmp` must stay a **total** order over any input. The last level is
`run_uid`, which is unique, and removing it would make the digest depend on the
order rows were gathered in — which is not a property of the data. Any new level
goes *above* `run_uid`, never below it, and
`test_selection_is_deterministic` must still pass with the candidates reversed.

### Bounds this season added

| Bound | Where | Reached ⇒ |
| --- | --- | --- |
| three source runs per package | `ATLAS_ORCH_MEMORY_MAX_SOURCES` | later candidates are not rendered |
| 12 KiB per package | `ATLAS_ORCH_MEMORY_MAX_BYTES` | the entry that would exceed it is not committed |
| 64 terminal runs examined | `ATLAS_ORCH_MEMORY_MAX_CANDIDATES` | `memory_candidates_truncated` is reported true |
| 320 bytes of a goal | `ATLAS_ORCH_MEMORY_GOAL_MAX` | the excerpt says Atlas truncated it |
| 480 bytes of a proved failure | `ATLAS_ORCH_MEMORY_DETAIL_MAX` | the excerpt says Atlas truncated it |
| 320 bytes of changed paths | `ATLAS_ORCH_MEMORY_FILES_MAX` | the excerpt says Atlas truncated it |
| 256 distinct tokens per text | `TOKEN_MAX` in `src/orch/memory.c` | scoring uses the first 256; the ceiling is constant so candidates stay comparable |

Every one of them is compile-time. There is no timer anywhere in this season and
no bound derived from elapsed time.

## A12.0 — the planned run

### Adding a driver role

`atlas_driver_role` in `include/atlas/driver.h`, and the `.role` field on every
entry in `DRIVERS[]` (`src/orch/driver.c`).

`ATLAS_DRIVER_ROLE_NONE` is zero and means "no model, no role" — the fakes that
need neither. Before adding a member, answer the question the vocabulary exists
to answer: **which model does this run under, and what may its output become?**
A role is a property of the *driver* and never of a job, because a submitter that
could assert a role could assert its way into the answer.

1. Add the member. `ATLAS_DRIVER_ROLE_NONE` stays zero, and there is deliberately
   no `_name` function: a role is never rendered, never sent over the wire and
   never parsed from one, so nothing can assert its way into holding one.
2. Decide whether the root-owned policy needs a `<role>_model` key beside
   `planner_model` and `executor_model` — see the next checklist but one — and
   whether `atlas_driver_model_for` should return it.
3. **Decide what a job of this role may produce.** PLANNER is the only role whose
   artifact Atlas itself reads: `plan.revision_add` requires it, and
   `artifacts_travel_inline` (`src/orch/dispatch.c`) carries its artifacts to the
   daemon rather than describing them. If the new role's output is something
   Atlas will compile, it needs both; if it is something a person collects, it
   needs neither.
4. A role is **not** an authorisation. Which drivers may run at all is still the
   policy's `driver =` list, and which one a lease may be granted is still the
   dispatcher's filter. Never write a check that reads a role as permission.

### Adding a field to the `atlas-plan-1` format

**You cannot.** Adding, removing or reinterpreting a field is a **new format
version**, `atlas-plan-2`, and the header line is what makes that enforceable: the
parser requires exactly `atlas-plan-1` and refuses anything else by name.

The reason is not caution about parsers. A planner is *shown* the specification —
`PLAN_FORMAT_SPEC` in `src/orch/plan.c` is quoted verbatim into every planner
prompt — so the format is part of the prompt a model answered, and a stored
revision is a document somebody was asked for in a particular vocabulary. A
field added in place would make an old revision and a new one two different
documents that claim to be the same kind of thing.

If you are adding a version:

1. Add the new header literal and keep the old one parsing. A stored revision is
   immutable and is re-read by `plan show` forever.
2. `PLAN_FORMAT_SPEC` is pinned to the constants by `_Static_assert`s at the top
   of `src/orch/plan.c`. Every number in the prose has one. Add yours.
3. Quote the new spec verbatim into `docs/orchestration.md`'s A12.0 section, the
   same way the current one is. Two copies of a specification that can drift are
   two specifications.
4. Every refusal names **what and where**: a sentence and a 1-based line number,
   travelling apart, so the driver renders `line %d: %s` without parsing Atlas'
   prose to recover the number.

### Changing a plan bound

Every one is a compile-time constant in `include/atlas/plan.h` with no flag, no
policy key and no wire parameter, because a bound a caller can raise is not a
bound.

| Bound | Constant | Reached ⇒ |
| --- | --- | --- |
| planner jobs per plan | `ATLAS_PLAN_MAX_PLANNER_JOBS` (5) | the driver stops; the plan derives BLOCKED once the last one failed terminally |
| compiled revisions per plan | `ATLAS_PLAN_MAX_REVISIONS` (3) | `plan.revision_add` refuses; `rev_no <= 3` is also a `CHECK` in migration 25 |
| stages per revision | `ATLAS_PLAN_MAX_STAGES` (4) | the document is refused, naming the line; `stage_no <= 4` is also a `CHECK` |
| tasks per revision | `ATLAS_PLAN_MAX_TASKS` (8) | the document is refused, naming the line |
| side tasks per stage | `ATLAS_PLAN_MAX_SIDE_PER_STAGE` (3) | the document is refused; the effective bound is `min(3, max_parallel - 1)` |
| plan document bytes | `ATLAS_PLAN_MAX_BYTES` (65536) | `plan.revision_add` refuses the artifact |
| one prompt's bytes | `ATLAS_PLAN_TASK_PROMPT_MAX` (16384) | the document is refused |
| one title's bytes | `ATLAS_PLAN_TITLE_MAX` (200) | the document is refused |
| one line's bytes | `ATLAS_PLAN_MAX_LINE` (4096) | the document is refused |
| goal bytes | `ATLAS_PLAN_GOAL_MAX` (16384) | `plan run` refuses locally, rather than truncating a goal nobody wrote |
| default parallelism | `ATLAS_PLAN_DEFAULT_PARALLEL` (2) | what an unstated `--parallel` resolves to |

Three things to keep straight when you move one:

1. **`_Static_assert` and `PLAN_FORMAT_SPEC`.** The specification a planner is
   shown states these as digits. Moving a constant without moving the prose makes
   the build fail, which is the point; moving the prose without moving the
   `_Static_assert` is how they drift.
2. **Migration 25's `CHECK`s.** `rev_no` and `stage_no` are bounded in the schema
   as well as in C, following M21's precedent: the schema is the guarantee and
   the C check is there so a caller gets a sentence instead of a constraint
   violation. Widening either needs a migration, because a `CHECK` cannot be
   edited in place.
3. **The worst case is documented arithmetic.** `5 + 3 × 4 × (3 + 3) = 77` worker
   starts appears in `CLAUDE.md`, `README.md`, `docs/roadmap.md` and
   `docs/orchestration.md`. If you move a factor, move the product everywhere,
   and remember that `PLAN_ITERATION_CEILING` in `src/orch/plandriver.c` is
   derived from the same constants — it is a defect guard, not a budget, and it
   must stay above the real bounds.

### Adding a `plan.*` method

`ORCH_CLIENT_METHODS[]` in `src/ipc/server_orch.c` — the same table `job.*` lives
in, gated by the same `require_submitter`. There is deliberately no plan method
group: a second group would be a second answer to "who may reach this".

`tests/test_plan_rpc.c` enforces three things and asks a live daemon about a
fourth:

1. The name is in the **client** group and in neither the dispatcher group nor
   the operator-uid table.
2. It contains no verb from `VERBS[]` — settle, accept, block, approve, apply,
   commit, push, authorize, grant, resolve. The existing four are create, add,
   get and list, and none of them decides anything.
3. A machine with no orchestration policy refuses it with a sentence that says so
   rather than pretending the method is missing.
4. Every name a *deciding* plan method would plausibly have —
   `FORBIDDEN_PLAN_METHODS[]`, twenty-four of them — must answer `unknown
   method` from a live daemon. Add yours to that list if it is one you are
   choosing not to build.

**A method that writes a plan's status does not belong here at all**, and neither
does one that writes a run's. A plan's status is derived on every read by
`atlas_db_plan_state_derive` from stored rows; there is no column and no
compare-and-swap, which is what makes "a model payload cannot declare a plan
complete" true because the verb does not exist. Adding `plan.settle` would undo
that in one line, exactly as `job.run_settle` would undo A11.1's.

Two further rules for a method that writes anything:

- **Every binding check happens inside the write transaction.** A check that a
  job is this plan's planner job k is worthless if a second call can land between
  the check and the insert. `plan.revision_add` is the model: correlation, role,
  state and artifact, all inside `atlas_plan_apply_in_tx`.
- **A correlation or an idempotency key is built by
  `atlas_plan_correlation_planner` / `_task` and by nothing else.** Three layers
  build the same string, and two spellings of one format are two answers to "is
  this job this plan's".

### Bounds this season added

| Bound | Where | Reached ⇒ |
| --- | --- | --- |
| five planner jobs per plan | `ATLAS_PLAN_MAX_PLANNER_JOBS` | the driver stops asking; BLOCKED once the last failed terminally |
| three compiled revisions | `ATLAS_PLAN_MAX_REVISIONS` | `plan.revision_add` refuses, and so does migration 25's `CHECK` |
| four stages, eight tasks, three side tasks per stage | `ATLAS_PLAN_MAX_STAGES` / `_TASKS` / `_SIDE_PER_STAGE` | the document is refused, naming the line |
| 65536-byte document, 16384-byte prompt, 4096-byte line, 200-byte title | `ATLAS_PLAN_MAX_BYTES` and friends | the document is refused, naming what and where |
| 16384-byte goal | `ATLAS_PLAN_GOAL_MAX` | `plan run` refuses rather than truncating |
| 85 driver iterations | `PLAN_ITERATION_CEILING` | the invocation ends; the plan is untouched and resumable |
| five transport attempts, 2 s apart | `ATLAS_RUN_XPORT_TRIES` / `_PAUSE_MS` | the invocation ends; nothing was decided |

Every one is compile-time. There is no timer anywhere in this season, no bound
derived from elapsed time, and no policy key or flag that widens any of them.

## Extending the P0 watcher safely

### A new watch reason

`atlas_watch_reason` is a stored vocabulary, so adding a member touches four
places and only one of them is a compile error:

1. The enum in `include/atlas/db.h`, keeping `UNKNOWN` at zero. `UNKNOWN` means
   *the writer stated no reason*, never *there is none* — `NONE` is the positive
   statement and only a complete watch set may carry it.
2. `atlas_watch_reason_name`, `_parse` and `_explain` in `src/db/db_state.c`.
   The two switches have no `default:`, so the compiler finds them; `_parse`'s
   table does not, which is why the round-trip is asserted in
   `tests/test_watch_budget.c`.
3. **A new migration widening the `CHECK` on `repo_index_state.watch_reason`.**
   A member the enum knows and the schema does not is refused at the first write,
   in production, by a constraint violation nobody can act on.
4. One sentence in `_explain`. It reaches an operator and, through `ai.context`,
   a model — so it is a fixed Atlas string and never assembled from anything a
   repository can influence. The `kernel_limit` sentence is the one that must not
   send an operator to an Atlas setting: its remedy is a sysctl.

### A new watch state

`atlas_watch_state` is CHECK-constrained too, so a member needs a
**table-rebuild migration** — SQLite cannot widen a CHECK in place — and that
migration verifies its own row preservation before it commits, migration 13's
discipline. Migration 26 is the worked example.

Then find every consumer, because `-Werror=switch-enum` finds the switches and
nothing finds the comparison chains:

- `atlas_index_state_is_current` in `src/db/db_state.c` — the authority on
  whether an index is current. Decide explicitly whether the new state is.
- `derive_index_current` in `src/core/service_daemon.c`, the remote path's copy
  of the same question.
- `tally_repo` in `src/core/service_daemon.c` and `tally` in `src/ipc/server.c`,
  which count repositories by state. A state counted as neither watched nor
  degraded needs its own counter, or one of those numbers becomes a lie.
- Both renderers, and `docs/watcher-consistency.md`.

`src/core/service_gate.c` used to hold a *second* copy of the currency
predicate — under a comment saying two answers to one question is one too many —
and P0 removed it by delegating. Do not reintroduce one.

### A new watch budget or bound

Every bound goes in the P0 block of `include/atlas/limits.h` with its reason
beside it, and **is reported when reached** rather than silently trimming a tree.
That means a reason code, which means the checklist above.

Two rules that are easy to get wrong and were both got wrong before P0:

- **Compare with `>=`, never `+ 1 >=`.** The old check refused the Nth watch at a
  budget of N, so a documented 8192 was in fact 8191 — and the daemon reported
  exactly that for weeks.
- **A bound on work and a bound on a resource are different bounds.** The walk's
  visit ceiling and the watch budget shared one flag and produced one sentence,
  so "this repository is too big" and "this daemon is out of watches" were
  indistinguishable to an operator *and* to Atlas.

### Adding anything that counts watches

Physical descriptors and logical subscriptions are different numbers.
`inotify_add_watch` on an already-watched path returns the **same** descriptor,
and two registered worktrees of one repository share every descriptor on their
common git directory. So:

- charge a repository only when `wd_map_put` reports a **new subscription**;
- release the kernel descriptor only when the **last** subscriber leaves;
- fan an event out to **every** subscriber;
- and never write `sum(per-repo) == total`. The relation is `>=`, and
  `watched_shared` is what explains it.

### The proven envelope is not the ceiling

`ATLAS_WATCH_DIRS_HARD_CEILING` is where a configured value is refused.
`ATLAS_WATCH_PROVEN_ENVELOPE_DIRS` is what `scripts/perf-watch.sh` actually
measured, in a deep and a wide tree shape, and it is the only figure
documentation may claim. Raising the second needs a measurement, not an edit.

### Bounds this season added

| Bound | Where | Reached ⇒ |
| --- | --- | --- |
| daemon-wide watches | derived; `watch_max_dirs_total` may state it | `total_budget` |
| one repository's share of a round | `remaining pool / repositories still wanting` | `repo_budget`, and the next round may give more |
| git metadata per repository | `ATLAS_WATCH_META_RESERVE_PER_REPO` (a floor) / `_MAX_PER_REPO` (a ceiling) | `meta_budget` |
| repositories observed | `ATLAS_WATCH_MAX_REPOS` | `repo_limit`; registration is unchanged |
| priming frontier | `ATLAS_WATCH_FRONTIER_MAX_BYTES` | `frontier_overflow` |
| directories awaiting an ignore decision | `ATLAS_WATCH_MAX_PENDING_IGNORE` / `_BYTES` | `pending_ignore_overflow` |
| ignored entries per repository | `ATLAS_WATCH_MAX_IGNORED_DIRS` / `_BYTES` | `ignore_overflow` — fails closed |
| directories visited per priming pass | `budget x ATLAS_WATCH_DISCOVER_FACTOR` | `discovery_bound` |
| kernel refusal | `fs.inotify.max_user_watches` | `kernel_limit` — not an Atlas setting |

## A13 — the scanner channel, and what a new method there costs

The scanner method group is small on purpose: `scanner.poll`, `scanner.put`,
`scanner.keep`, `scanner.state`. Adding to it touches more than the table.

**Every method authorises the same way, and none of them may skip it.** The
first thing each does is `require_scanner`, and the second is resolve the named
repository and check `peer_owns` — `SO_PEERCRED` against
`repositories.scanner_uid`, one comparison, and that is the whole
authorisation. A method that takes a `repo` and does not do both is a method
that lets one uid's scanner write into another's mirror. The group is
dispatchable by name rather than hidden, so being in the table is not a
permission.

**A refusal must not become an inventory.** `require_scanner` answers about the
caller's own uid and names no repository, because a list of what exists handed to
whoever asked is not a refusal. Keep new refusals to that shape.

**Ask what the method asserts, and refuse to overload an existing one.**
`scanner.keep` is not a flag on `scanner.put` for the reason `cancel` is not an
overload of `yield`: a put says "here are the bytes" and a keep says "you already
have them", and only the second can be answered by the daemon simply not having
the file. Two assertions, two methods.

**A method that takes the client's word for something about the daemon's own
state is the defect to look for.** `scanner.keep` is the shape to copy: the
scanner's memory decides only whether to *ask*, and the daemon answers from what
its own published generation holds — `kept: false` when it holds nothing. Written
the other way round, with the daemon trusting that the caller's file is unchanged,
the same feature would let a stale client produce a mirror whose contents nobody
put there.

**A filesystem operation and a database write are ordered, never combined.**
`scanner.state` publishes the generation and *then* queues the row, so a crash
between them leaves a readable mirror the row does not vouch for — which costs a
refusal — rather than a row vouching for a generation that was never published.
Anything new that touches both belongs on the same side of that order.

**The client is answered when the write is accepted.** Do not add a waiting form
of anything here: a caller that waited would need a bound on how long the writer
may be busy, and Atlas has none. `scanner.poll` is the confirmation channel,
because its directive is `full` while no complete mirror exists — so a job that
failed is answered by being asked for again.

**Anything the scanner remembers between passes stays in the process.** A
manifest on disk would be a promise about a mirror this process did not build,
which is the shape of the cadence the scanner was once allowed to declare and
which was written and reverted. Losing in-process memory must cost a full pass
and nothing else, and a `full` directive must drop it.

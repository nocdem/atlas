# A9.2.5 — Semantic Index Trust Closure

**The sentence this season exists for:**

> **A SEMANTIC READ THAT FOUND NOTHING HAS NOT ESTABLISHED THAT THERE IS
> NOTHING.**

and its consequence, which is the part that had to be built:

> **EVERY LOAD-BEARING SEMANTIC ANSWER CARRIES THE EVIDENCE FOR ITS OWN
> VERDICT.**

---

## 1. What was wrong

A9.2.2 established that a *claim* may not be answered ABSENT unless the coverage
dimensions an absence rests on were shown sufficient — `atlas_verify_truth_of`,
and the `settle()` gate in `src/verify/detverify.c`. A9.2.3 gave a generation a
coverage manifest. A9.2.4 gave it a build-input discovery verdict.

None of it reached the answer to `callers of X`.

Every semantic query replied with its rows plus
`{freshness, stale_reason, generation_id, indexed_commit}` and stopped. So this
document:

```json
{"repo":"fx","freshness":"CURRENT","stale_reason":null,"generation_id":2,
 "query":"orphan","direction":"callers","nodes":[],
 "summary":{"visited":1,"emitted":0,"proven":0,"truncated":false}}
```

and the human form under it —

```
  index             CURRENT (generation 2)
  0 reached — proven 0, candidate 0, unknown 0
```

— were produced by a repository in which `orphan` had a **PROVEN** caller, in a
file the compilation database did not name. `code sem-status` at that same
moment said `scope covered 1 of 3 source files`. The information needed to
refuse the conclusion existed, in the same process, one function away, and was
not on the answer.

That is reproduced end to end by `test_e2e_zero_rows_over_partial_coverage_is_unknown`
in `tests/test_sem_trust.c`, which fails on `80f41bf`.

## 2. The verdict

Every load-bearing semantic read now carries `result_verdict`:

| Verdict | Means | Earned when |
| --- | --- | --- |
| `PRESENT` | At least one row was found. | `rows > 0`, whatever the coverage. |
| `ABSENT` | Nothing was found, over a universe Atlas can vouch for. | Every condition in §3. |
| `UNKNOWN` | Atlas cannot say. Evidence in neither direction. | Anything else. |

**`UNKNOWN` does not mean "no".** It means Atlas did not establish an answer, and
a consumer that reads it as absence has made exactly the error the season exists
to prevent. `UNKNOWN` is the enum's zero, so a `memset` cannot produce an
absence proof.

### The asymmetry is A9.2.2's, applied one layer out

One row settles `PRESENT` and nothing else is consulted. A caller Atlas *found*
exists whatever it failed to look at; coverage bounds a negative conclusion and
bounds nothing about a positive one. A **stale** generation that found a caller
is still evidence that the caller existed in the tree that generation described —
so positive rows are emitted whatever the trust facts say, and the generation
they came from travels beside them. Nothing is suppressed and nothing silently
becomes a statement about the current tree.

Zero rows settle `ABSENT` only when the universe searched can be shown to have
been the whole of the relevant one.

## 3. What `ABSENT` requires

`atlas_sem_trust_settle` — one pure function, called by the CLI, the RPC server
and the daemon, so they agree because they call it rather than because three
copies of a rule are kept in step. The order is the order an operator would want
to be told, most actionable first:

| Check | `unknown_reason` when it fails |
| --- | --- |
| libclang is available | `this_atlas_was_built_without_libclang` |
| a generation is published | `no_semantic_generation_has_been_published_…` |
| not currently rebuilding | `a_semantic_generation_is_being_built_right_now` |
| freshness is `CURRENT` | `the_semantic_generation_does_not_describe_the_current_source` |
| automatic maintenance is on | `automatic_semantic_maintenance_is_disabled_…` |
| the generation has a coverage manifest | `the_generation_recorded_no_coverage_manifest` |
| the generation's discovery was `COMPLETE` | `build_input_discovery_cannot_say_it_found_every_…` |
| every unit was fully described | `a_translation_unit_was_not_fully_described` |
| no candidate source was uncovered | `the_generation_did_not_cover_every_candidate_source` |
| the walk was not truncated | `the_query_reached_a_bound_before_it_finished` |

Two of these are worth their own note.

**The verdict rests on the *generation's* discovery, never the live one.** A walk
that has since completed says nothing about a generation built while it had not.
Both values are reported — `generation_discovery` and `discovery` — and they
differ exactly when a rebuild is due.

**A repository nobody maintains cannot settle an absence.** A freshness value is
only ever a statement about the instant it was computed, and
`atlas_sem_auto_effective` is what decides. The remedy is
`code sem-config NAME --auto`, and the reason says so rather than sending an
operator to look at their compilation database.

The four coverage conditions come from **one** function,
`atlas_sem_coverage_gap`, which the scheduler also asks. A repository the
scheduler calls `INCOMPLETE` and a query that answers `UNKNOWN` therefore name
the same dimension.

## 4. The trust block

One writer, `atlas_sem_trust_write_json`, called by `src/cli/render_json.c` and
`src/ipc/server_sem.c`. It carries:

`result_verdict` · `unknown_reason` · `freshness` · `stale_reason` ·
`have_generation` · `generation_id` · `indexed_commit` · `generation_identity` ·
`live_identity` · `coverage_complete` · `units_complete` · `scope_discovery` ·
`scope_candidates` · `scope_covered` · `scope_uncovered` ·
`generation_discovery` · `discovery` · `inputs_accepted` · `inputs_rejected` ·
`auto_maintenance` · `libclang_available`

on every load-bearing semantic read: `code semantic`, `code callers`,
`code callees`, `code trace`, `code sem-impact`, `code tests`, `code explain`,
`context build` and `code sem-status` — and on their RPC and MCP forms.
`test_e2e_every_load_bearing_surface_carries_the_trust_block` enumerates the keys
against all nine, because nothing but an enumeration holds that many surfaces to
one shape.

**What the verdict means on `sem-status`.** `sem-status` reports the index rather
than searching it, so it always settles with zero rows. Its verdict is therefore
the answer to a different question from a query's: *could a negative conclusion
rest on this index right now?* `ABSENT` there means "yes — this index could
support an absence", not "there is nothing in this repository". `UNKNOWN` means
what it always means, and `unknown_reason` names what would have to change.

It is one struct with one writer because seven surfaces keeping twenty-one fields
in step by hand is how `have_generation` came to be on the RPC document and not
the CLI's. MCP is a pass-through of the RPC document, so MCP↔RPC parity is
structural and free; CLI↔RPC parity is now structural too.

### Two deliberate consequences

**The block is written after the results.** A verdict is a statement about a
result set, so on a streaming writer it cannot precede it. Key order is not a
JSON contract, and the guarantee that mattered — that no answer can be read
without its currency — is unchanged, because the block is in the same document.

**Nothing was removed.** Every key the old documents carried is still emitted,
by the shared writer. `have_generation` is new to the CLI. Everything else in the
list is additive.

### Compatibility

The CLI's remote parser leaves the **conservative** value for every absent key.
A newer CLI against an older daemon finds no `result_verdict`, no coverage and no
discovery, and reports `UNKNOWN` with `coverage_complete` false. It never errors
on a missing key and never defaults to `ABSENT`. A missing key is Atlas not
having been told, which is what `UNKNOWN` means.

## 5. A symbol that is not in the index is not a usage error

`no symbol named "X" is in the semantic index` used to be `ATLAS_ERR_USAGE`,
exit 2 — the operator-typo class — for what is a statement about the index's
contents. Over an incomplete generation it cannot even be that: the symbol may
sit in a file the compilation database never named.

It is now an ordinary empty result set and settles like any other: `ABSENT` over
a generation that read the whole tree, `UNKNOWN` over one that did not.
**Ambiguity stays exit 2**, because a name that resolves to three symbols is a
question Atlas cannot answer as asked.

## 6. `INCOMPLETE` is no longer held with `HOLD_CURRENT`

A source-current, coverage-incomplete repository used to be held with
`the_published_generation_describes_the_current_source`. True — and it conceals
the half that decides whether any absence over the index means anything. On
`/opt/atlas` itself the state is permanent and its only cause is the operator's
own `--exclude`, which makes discovery `PARTIAL`, which makes coverage
incomplete, which no rebuild can change. That one sentence was everything anybody
ever saw about it.

It is now `ATLAS_SEM_HOLD_COVERAGE_INCOMPLETE`, with `coverage_gap` naming the
dimension and `operator_action_required` saying that waiting will not help.

**It is still a hold, not a rebuild trigger.** Rebuilding cannot widen a
compilation database, cannot un-exclude a subtree, and cannot make a unit that
failed on these bytes succeed on them. Scheduling one would spin without
converging, which is the rebuild storm this season is required not to create.

## 7. Discovery says *where* it could not look

A9.2.4 kept the **first** reason a walk fell short and no path at all. One
declared `--exclude` therefore consumed the only slot and masked every unreadable
directory for the rest of the walk. On `/opt/atlas`, `discovery_limit` read
`an operator excluded a subtree from the search` and nothing could say what else
had been missed.

`sem_discovery_obstacles` (migration 20) records every obstacle with its exact
`%XX`-encoded repository-relative path and a fixed Atlas reason, sorted by path
so two walks over an unchanged tree agree whatever order `readdir` returned. The
classes are kept apart because the remedies differ:

`an_operator_excluded_this_subtree_from_the_search` ·
`this_directory_could_not_be_entered` ·
`this_directory_could_not_be_read_to_the_end` ·
`the_walk_reached_its_depth_ceiling_below_this_directory` ·
`the_walk_reached_its_entry_ceiling_at_this_directory` ·
`a_path_below_this_directory_exceeded_the_path_ceiling` ·
`a_path_below_this_directory_could_not_be_represented` ·
`more_compilation_databases_were_found_than_one_walk_may_hold` ·
`there_was_not_enough_memory_to_walk_this_directory`

The objection A9.2.4 raised against recording the path — "a path is bytes a
repository chose" — was already answered by `encode_rel`, which every accepted
and rejected candidate's path goes through, twelve lines from where the reason
was recorded.

The table is `DERIVED` and **never prunable by age**: a partly-deleted list of
what was missed reads as a search that missed less.

## 8. Repository identity is compared

`repo_identity_hash` — A4's path-qualified lineage fingerprint — has been written
onto every generation since A8-CI and compared by nothing. `src/gate/assess.c`
compares exactly this value and revalidates on a mismatch; the semantic layer
wrote it and forgot it.

`atlas_sem_freshness_of` now compares it, **before** the commit: "this index
describes a different repository" outranks "this index describes an older commit
of the same one". The source identity cannot stand in for it — it is built from
repository-*relative* paths and content hashes, so a tree with identical content
under a different canonical root produces an identical value.

Both empty-value guards hold: an empty stored identity is a generation built
before this was recorded, and an empty live one is Atlas not having looked.
Neither is evidence of change.

## 9. A transient failure is not permanent coverage loss

`tu_failed > 0` makes a generation's coverage incomplete for ever, because the
retry governor compares *identities* and identical bytes never retry. So a parse
child that was OOM-killed cost a repository the ability to state an absence until
somebody happened to edit a file, and nothing recorded that this had happened.

Two bounded recoveries, and both bounds are what make a storm impossible rather
than unlikely.

**Per unit.** A unit whose failure reason is transient —
`the_parser_process_did_not_report_a_result` or
`the_per_unit_time_bound_was_reached`, and *only* those — gets
`ATLAS_SEM_UNIT_TRANSIENT_RETRIES` (one) further attempt, inside the pass that is
already running. Nothing durable records that a retry happened, so a restart has
no half-finished state to interpret, and no timer exists that could wake up and
try again. `units_retried` travels on the operation's detail line beside
`units_parsed` and `units_reused`, because like them it describes the pass rather
than the rows the generation holds.

It is tested **end to end**, against a parse child that really fails:
`tests/tools/atlas_flaky_parse.c`, supplied through
`atlas_sem_index_opts.atlas_exe` — the parameter the child's executable path has
come from since A8-CI, so nothing in production is relaxed for a test. Three
cases: a transient failure recovers on the second attempt and publishes a
*complete* generation after exactly two child invocations; two failures leave the
unit failed after exactly two invocations, with the scheduler stable on an
unmoved source identity; and a real compiler error spends no retry at all.

**Per pass.** A pass that failed from `ATLAS_ERR_INTERNAL` or `ATLAS_ERR_DB` —
out of memory, a database error — records
`the_semantic_index_pass_was_interrupted` rather than
`the_semantic_index_pass_did_not_complete`, and the governor allows exactly one
further attempt with the source unmoved. The bound is `fail_count`, which is
durable, so the second interruption holds and a daemon restart reads the same
count and reaches the same answer.

Everything else is a property of the input and is not retried: a compiler that
reported errors will report them again from identical bytes.

## 10. What A9.2.5 deliberately did not do

**It did not teach Atlas to guess which sources are tests.** A declared test root
is matched from the start of the path on a component boundary, so a nested test
directory must be declared by its full relative path. `nodus/tests` declared
matches `nodus/tests/a.c`; `tests` declared does not. That is the model working
as specified, and a heuristic that guessed would classify a production source as
a test the first time a repository used the word differently — wrong in the one
direction that matters.

What follows from it is the rule that **an operator declaring a test root is not
evidence that they declared every test root** — the same shape as A9.2.4's rule
that a pinned compilation-database list is not a completeness claim. So
`ATLAS_COVDIM_TESTS` is established by no verifier, stays `UNKNOWN`, and any
absence that would depend on the test/production split is `UNAVAILABLE` rather
than `ABSENT`. `test_declaring_one_test_root_does_not_prove_they_are_all_declared`
pins that, because the failure mode is silent.

**It did not merge the structural and semantic trust surfaces.** A repository
whose A3 structural index is current may have a semantic index that is not, and a
query answer inherits the semantic surface's verdict, never the structural
surface's currency.

**It added no MCP tool, no gateway route and no ordinary RPC method**, and it did
not bump `ATLAS_SEM_ANALYZER_VERSION`: nothing here changes the facts a pass
derives from identical bytes.

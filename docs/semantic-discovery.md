# Semantic build-input discovery and activation (A9.2.4)

> **COMPLETE PROCESSING OF CONFIGURED INPUTS DOES NOT PROVE COMPLETE DISCOVERY OF
> RELEVANT INPUTS.**

> **SEMANTIC AUTO-MAINTENANCE MUST NOT DEPEND ON AN OPERATOR REMEMBERING TO
> REBUILD, EXCEPT WHERE THE OPERATOR HAS EXPLICITLY DISABLED IT.**

Those two sentences are the whole season. Everything below is one of them made
structural.

## 1. What went wrong, twice

A9.2.3 gave a semantic generation a coverage manifest, which turned
`416/416 translation units complete` into the honest `369 of 761 C sources
covered`. That was a real advance and it left two things unanswerable.

**The first.** A repository was reported

```
structural = CURRENT
semantic   = STALE
activity   = DISABLED
reason     = no operator has enabled automatic rebuild for this repository
```

after a season whose entire purpose was daemon-owned automatic freshness. Nothing
was broken. `sem_repo_config.auto_rebuild` defaulted to 0, only an operator ever
wrote it, and nobody had — so the daemon correctly did nothing, for ever, and
said so in words that read like a diagnosis rather than a policy.

**The second.** The same repository's build description named two compilation
databases. A third existed on disk, describing a whole first-party subsystem.
Every count Atlas reported was true — both databases were fully processed, every
unit parsed — and the index simply did not describe that subsystem. Nothing could
say so, because A9.2.3's rule was that compilation databases are **named, never
discovered**: the answer to "what are this repository's build inputs?" was
whatever somebody had typed.

The two failures have the same shape. In both, Atlas reported a state it had
correctly derived from a question it had never asked.

## 2. The four axes, and why discovery is a new one

| Axis | Question | Since |
| --- | --- | --- |
| Freshness | does the published generation describe the current source? | A8-CI, working-tree-aware in A9.2.3 |
| Scope coverage | of the sources the tree holds, how many did it read? | A9.2.3 |
| **Build-input discovery** | **were those all the compilation databases?** | **A9.2.4** |
| Activity | what is the daemon doing about it, and why not more | A9.2.3, widened here |

None is derived from another, and every surface reports them separately. A
generation can be perfectly source-current, cover every source its databases
named, and still have been built without knowing whether another database
exists — which is exactly the state that produced this season, and exactly the
state a single green badge would hide.

## 3. The bounded search universe

Atlas says `COMPLETE` about a search only when it can name what it searched.
The universe is:

- the registered repository root and nothing above or beside it;
- **not** `.git`, which is the one directory whose meaning Atlas already knows
  and the one `src/git/git_harden.c` exists to keep away from;
- **not** any operator-declared exclusion;
- within the ceilings in `include/atlas/limits.h`.

Every descent is `openat` with `O_NOFOLLOW` from a descriptor validated once, so
**no symlink is ever followed** and no walk can be pointed outside the repository
by a planted link. The filename Atlas recognises is fixed —
`compile_commands.json`, the name the JSON Compilation Database format is
published under — because letting a repository nominate other names would widen
the set of files a walk opens on the repository's own say-so.

### The bounds, and what each one costs

| Bound | Value | Reached ⇒ |
| --- | --- | --- |
| `ATLAS_SEM_DISCOVERY_MAX_DEPTH` | 12 | PARTIAL |
| `ATLAS_SEM_DISCOVERY_MAX_ENTRIES` | 400 000 | PARTIAL |
| `ATLAS_SEM_DISCOVERY_MAX_CANDIDATES` | 128 | PARTIAL |
| `ATLAS_SEM_MAX_COMPDBS` (accepted into one generation) | 32 | PARTIAL, excess shown as rejected |
| `ATLAS_SEM_MAX_PATH_BYTES` | 1024 (encoded) | PARTIAL |

Every one is **reported when it is reached**, and reaching any of them makes the
search PARTIAL rather than silently smaller. A walk that stopped early and said
nothing would be indistinguishable from a repository with nothing more to find,
which is the indistinguishability this season exists to end.

### The epistemic rule

> **DID NOT DISCOVER is not PROVEN NOT TO EXIST.**

`COMPLETE` is a claim about the bounded universe and about nothing wider, and the
universe is reported beside the verdict rather than left for a reader to assume.

## 4. Discovery states

```
UNKNOWN   Atlas did not look, or looking failed.        Never sufficient for an absence.
PARTIAL   Atlas looked and stopped early.               Never sufficient for an absence.
COMPLETE  Atlas walked the whole bounded universe.      The only state an absence may rest on.
```

`UNKNOWN` is zero. A `memset` never asserts that a search universe was covered.

### Why a pinned list is UNKNOWN

`discovery_mode = MANUAL` uses only the operator's pinned list and leaves the
verdict UNKNOWN — even though the operator named an exact set. That looks harsh
and is the one lesson this season had to buy: the repository that exposed the
problem had a hand-written list of two databases, the list was wrong, and **no
flag the operator could have ticked would have made it right**, because the
operator did not know either.

§7 of the season brief lists "operator declaration of bounded roots" as a
legitimate completeness mechanism. It is rejected as a *completeness claim* for
that reason, and the rejection is recorded here so that a later phase re-adding
an assertion flag has the counterexample in front of it. An assertion of
completeness by somebody who has not looked is not evidence of completeness.

## 5. Candidates: accepted, rejected, and always shown

Every candidate is recorded with its origin and its outcome:

| Origin | Meaning |
| --- | --- |
| `PINNED` | named by an operator with `--compdb` |
| `DISCOVERED` | found by the walk |
| `BOTH` | named and independently found |

A candidate that cannot be used is **recorded with a reason**, never skipped:

| Reason | When |
| --- | --- |
| `an_operator_excluded_this_path_from_discovery` | a pinned path under an exclusion |
| `not_a_readable_regular_file_inside_the_repository` | missing, unreadable, not regular |
| `a_symlinked_path_is_never_followed` | the candidate is a link |
| `the_compilation_database_exceeds_the_size_bound` | refused, never truncated |
| `the_compilation_database_could_not_be_parsed` | not JSON, or not a JSON array |
| `the_same_file_was_already_accepted_under_another_path` | deduplicated |
| `more_compilation_databases_were_found_than_one_generation_may_hold` | ceiling |

**A rejected candidate nobody is shown is indistinguishable from one that does
not exist.** That indistinguishability is what kept a third compilation database
invisible for a season, so the rejected ones are on every surface: the human
renderer, `--json`, `sem.status`, the gateway route, MCP and Mission Control.

Zero units is **not** a rejection. An empty compilation database is a build that
has produced nothing yet, which is a different fact from one Atlas could not
read, and the two stay different.

### Identity

One file is one input however many paths reach it. Deduplication is by
`(device, inode)` — what the kernel means by "the same file" — not by path,
because a path comparison cannot see that a symlink, a relative path and a
canonical path name one file. The **path** is still what is reported, because an
operator needs to know which name Atlas used.

In the repository this season was developed in, the top-level
`compile_commands.json` is a symlink into `build/`. It appears as a *refused*
candidate with reason `a_symlinked_path_is_never_followed`, and the file it
points at is discovered on its own. That is the ordinary case, not an exotic one.

## 6. Discovery feeds freshness. There is no second scheduler.

```
bounded walk
   ↓ (accepted set, contents, verdict)
atlas_sem_repo_discovery_identity
   ↓
atlas_sem_source_identity            ← domain bumped to v2
   ↓
atlas_sem_freshness_of  →  STALE, reason: the set of discovered build inputs changed
   ↓
atlas_sem_plan_for      →  should_build
   ↓
the A9.2.3 daemon scheduler, unchanged
```

A9.2.3 folded a digest over the compilation databases *an operator had named*
into the source identity, so a database appearing on disk that nobody had named
moved nothing and the generation stayed CURRENT. Discovery replaces that input,
and the whole of §17 falls out of one digest rather than out of a new mechanism.

`atlas.sem.source-identity.v2` is the bumped domain. Every stored identity from
before this season means something different from one computed now, so comparing
them would report a change nobody made; the bump makes every pre-A9.2.4
generation stale exactly once. That is the honest outcome — those generations
were built without knowing whether their input set was complete.

The **discovery state** is part of the identity as well. A PARTIAL walk that
later completes with an identical accepted set still moves it and still rebuilds,
which is the only way a generation's *sealed* manifest can be upgraded from
"Atlas could not account for the whole universe" to "it could" — and it costs
almost nothing, because every unit's input digest is unchanged and every unit is
reused.

### Where the walk runs, and where it must not

`atlas_sem_plan_for` runs on every status read, every verification and every
sweep tick. A directory walk cannot live there.

- **Membership** is persisted in `sem_build_inputs` and refreshed on
  `ATLAS_SEM_DISCOVERY_INTERVAL_MS` (300 s), on a `sem-config` write, and by an
  operator's explicit command.
- **Content** of each accepted database is digested live on every identity
  computation — a handful of files.

So an *edited* or *deleted* database moves the identity at once, and a *newly
created* one is noticed at the next walk. That is **convergence, not
correctness**: nothing is ever wrong in between, only later. It is the same shape
as A9.2.3's semantic sweep holding while the file index catches up.

### One pass, and what an unreadable input means

Every live value — the compilation-database digest, the discovery digest and the
source identity — comes from a single pass over the accepted set
(`live_facts` in `src/sem/index.c`). The databases now feed two digests, so
reading them once per digest would have reintroduced the exact cost A9.2.3's
closure measured and fixed: `sem.status` hashing every declared database twice
per response, with the second hazard that two computations within one document
could disagree if the tree moved between them.

An accepted input that can no longer be read contributes a **fixed marker** to
both digests rather than being skipped. Skipping it would make a repository that
has just lost its second build description compare equal to one that only ever
had the first — and the trade is asymmetric: a transient read failure costs one
unnecessary rebuild, which is self-correcting, while an input that disappeared
without moving the digest leaves the index describing build descriptions that are
gone, with nothing saying so.

This reverses A9.2.3's `atlas_sem_live_compdb_digest`, which returned an empty
digest for the whole set the moment one file could not be read. That was right
for a set of paths an operator *typed*, where an unreadable one is usually a typo;
it is wrong for a set Atlas *discovered and accepted*, where an unreadable one
usually means the build tree was removed. The function had no callers left after
the consolidation and was deleted rather than kept with a stale contract.

### The defect this exposed underneath it

Making rebuilds automatic reached a latent A8-CI defect that nothing had been
able to reach before, because nothing had ever rebuilt a repository twice
without being asked.

`atlas_db_sem_copy_unit` carried `sem_edges.unit_id` across verbatim when it
reused a translation unit, so every carried edge pointed at a unit row belonging
to the **ancestor** generation. The next incremental pass selects the edges to
carry by joining `sem_units` — and once the ancestor's rows were pruned, the join
found nothing. The call graph decayed on every rebuild:

| generation | units | symbols | edges |
| --- | --- | --- | --- |
| full build | 775 | 26 958 | 475 741 |
| four incrementals later | 775 | 26 961 | 10 631 |

3 479 of the survivors referenced unit rows that no longer existed. **The symbol
count never moved**, which is why nothing looked wrong: a symbol lookup answered
correctly while the graph underneath it emptied.

The fix is three changes, and the second and third are as load-bearing as the
first:

1. the unit row is written **before** its facts, so a carried edge has a row in
   its own generation to point at — the old ordering made that impossible, which
   is why the ancestor's id was carried instead;
2. that row's id is **looked up**, never taken from `sqlite3_last_insert_rowid`,
   because the write is an upsert and on its update branch the rowid belongs to
   some other table — a wrong id here attaches one unit's edges to another,
   silently, on the replay path a crash produces;
3. `ATLAS_SEM_ANALYZER_VERSION` is bumped to **2**. Identical bytes now produce a
   different and correct graph, which is what the epoch is for — and it is also
   the repair, because nothing short of a full rebuild restores a graph that has
   already decayed and a stale epoch is what makes the daemon rebuild every
   affected repository once.

### And the epoch itself did not work

Verifying that repair found a fifth defect underneath the fourth. The bump made
every generation STALE and scheduled a rebuild, and the rebuild **reused 203 of
203 units** and republished the same decayed graph under the new version.

A unit's input digest covered its include closure's content and its compilation
flags, and nothing about the *producer*. So A3's rule — bump the epoch and the
next pass rebuilds — was only ever half true: the staleness was real, the
rebuilding was not. Every analyzer bump in Atlas' history was a no-op for any
repository nobody rebuilt from nothing by hand. The compiler version had the same
hole, for the same reason, with the same consequence.

`input_digest` now folds in the analyzer id, the analyzer version and the
compiler version, and its domain is `atlas.sem.unit.v2`. A machine upgrading from
A9.2.3 gets the repair without being told: its generations carry
`analyzer_version = 1` so they are stale, and their stored digests carry the v1
domain so nothing is reused.

**There is no unit test for this**, and the reason is stated rather than worked
around: exercising it needs two analyzer versions in one process. A placeholder
that asserted the digest was hex was written and deleted — a test that cannot
fail reads as coverage and is worse than none.

### Flapping self-heals

A build system regenerating a byte-identical `compile_commands.json` moves no
digest and causes no rebuild. One caught half-written fails to parse, is recorded
as a rejected candidate, and is accepted the moment the finished file changes the
digest. Neither needs a special case.

## 7. Activation

### The states

| Activity | Meaning |
| --- | --- |
| `EXPLICITLY_DISABLED` | an operator refused. Never lifted by a policy change. |
| `DISABLED` | the root-owned default says no and nobody has said otherwise. |
| `NO_INPUTS` | maintenance is on and discovery accepted nothing. |
| `UNAVAILABLE` | nothing has ever been indexed. |
| `BUILDING` / `DIRTY` / `CURRENT` / `INCOMPLETE` / `FAILED` | A9.2.3, unchanged. |

A9.2.3's `DISABLED` covered three unrelated situations with one word — a policy,
a configuration gap and a missing file — which is what made `activity = DISABLED`
unreadable in the field.

`NO_INPUTS` carries two different reasons, because *"I looked and there is
nothing here"* and *"I have not looked yet"* are different statements:
`build_input_discovery_accepted_no_compilation_database` and
`build_input_discovery_has_not_run_for_this_repository`.

### The policy

```
auto_intent = DISABLED  ->  off, for ever, until an operator says otherwise
auto_intent = ENABLED   ->  on
auto_intent = UNSET     ->  syspolicy.semantic_auto_default
                            (absent ⇒ ATLAS_SEM_AUTO_DEFAULT, which is ENABLED)
```

`atlas_sem_auto_effective` is the whole policy, in one pure function, so the
daemon, the CLI and every status surface answer identically because they call it
rather than because somebody keeps three copies in step.

### The reversal, stated as one

A9.2.3's rule was that `auto_rebuild` defaults to 0 so that no compiler runs over
a repository nobody has spoken about. **A9.2.4 reverses that default.** What is
preserved and what is given up:

- **What the opt-in protected was never code execution.** libclang *parses*
  repository text. The `command` string is word-split and never executed;
  arguments pass a positive allowlist; include directories outside the repository
  are recorded and never opened; the parse runs in a bounded child with an empty
  environment and an address-space ceiling. The opt-in was authority and resource
  policy, and it is replaced by authority and resource policy rather than removed.
- **Registering a repository is already an operator act** that no model can
  perform, and it is the act that now carries the consent.
- **Whether this daemon runs a compiler on its own initiative** for a repository
  nobody has spoken about is a **root-owned** decision — `semantic_auto_default`
  in `/etc/atlas/system.conf` — rather than a compiled-in fact.
- **Nothing model-facing changed.** No MCP tool, no gateway route and no ordinary
  RPC method enables, disables or triggers semantic maintenance;
  `code.sem_config` stays in the operator-uid table, gated on `SO_PEERCRED`.

What it costs, plainly: a model that can change a registered repository's source
can now cause a compiler to parse that source, where before it could only do so
for repositories an operator had enabled. The parse is bounded, sandboxed and
reads only; the cost is CPU and the writer thread's time. An operator who does not
accept that trade sets `semantic_auto_default = DISABLED` machine-wide, or
`code sem-config REPO --no-auto` per repository.

### The machine-wide key

```
# /etc/atlas/system.conf
semantic_auto_default = ENABLED   # or DISABLED
```

Root-owned, read through `atlas_rootpath_open`, no environment override and no
flag. An unrecognised value is malformed rather than treated as one of the two.
An absent key is the absence of a statement, and the documented default is what a
missing statement means.

> **Cutover order matters, and getting it wrong degrades the deployment.**
> A7.1's rule is that *an unrecognised policy key is an error, not something
> skipped* — so a binary that predates this key reads a policy containing it as
> **malformed**, falls back to legacy per-user mode, and starts resolving a
> stale per-user database instead of `/var/lib/atlas`. Measured, not
> hypothetical: adding the key while an A9.2.3 binary was installed made
> `atlas doctor` report `per-user (MALFORMED)` and `schema version 6` on a
> machine whose real index is at 19.
>
> The key is therefore added **after** the binary that understands it is
> installed, never before. The safe cutover is:
>
> 1. install the new binary and verify it (`cmp`, then `/proc/<pid>/exe`);
> 2. add `semantic_auto_default = DISABLED`;
> 3. restart the daemon **and** every other service running the same binary;
> 4. let it migrate and discover — it builds nothing while the key says no;
> 5. record the per-repository decisions (`--exclude`, `--no-auto`) at leisure;
> 6. flip the key to `ENABLED`. It takes effect on the next sweep; no restart.
>
> Step 4 is what makes the rest unhurried. Without it the first watcher tick
> discovers every build tree on the machine and the tick after it starts
> building them, which is a race an operator should not have to win.

## 8. Migration: intent, and who expressed it

`sem_repo_config.auto_rebuild` was written unconditionally as 0 or 1, so **a
stored 0 cannot distinguish an operator's `--no-auto` from nobody ever having
said anything**. Migration 19 does not invent the difference:

| Stored | Becomes | Why |
| --- | --- | --- |
| `auto_rebuild = 1` | `auto_intent = ENABLED`, `auto_intent_by = OPERATOR` | only an operator ever wrote a 1 |
| `auto_rebuild = 0` | `auto_intent = UNSET`, `auto_intent_by = MIGRATION` | the unconditional default carries no information |
| no row at all | `UNSET` / `DEFAULT` | nobody has spoken |

A 0 that was the default is evidence of nothing. Migrating it to `DISABLED` would
be Atlas asserting a refusal nobody expressed — and that refusal would then be
honoured for ever, which is precisely the failure mode the intent field exists to
prevent. Every surface reports the provenance beside the intent, so a reader can
tell a decision from a default, and the state never reads "operator disabled" for
a migrated row.

Consequence, stated rather than left to happen: **after migration, repositories
that were silently disabled become automatically maintained.** On a machine where
that is not wanted, the two remedies above are one line each.

## 9. Coverage and negative knowledge

`ATLAS_COVDIM_BUILD_INPUT_DISCOVERY` is a first-class coverage dimension, the
twelfth, and **every negative verifier depends on it**:

| Verifier | Depends on discovery |
| --- | --- |
| `atlas.symbol_absent` / `atlas.symbol_present` | yes |
| `atlas.proven_edge` | yes |
| `atlas.no_proven_caller` | yes |
| `atlas.content_hash` | no — it reads a file, not an index |

An undiscovered compilation database could name the source that defines the
symbol, so even the narrowest negative rests on the search having been complete.

The fold is in `coverage_is_complete`: a generation whose recorded `discovery` is
not COMPLETE is not coverage-complete, whatever else is true of it. That is a
fourth different problem folded into one boolean, alongside a unit that failed to
parse, a source the databases never named, and an enumeration Atlas cannot vouch
for — and every one of them means the same thing about a negative conclusion.

**A9.2.2 is not weakened.** It gains one more way to be honest: where discovery
is UNKNOWN or PARTIAL, a negative result is `UNAVAILABLE` and the truth stays
`UNKNOWN`, never `ABSENT`.

## 10. Classification, and the limit that is stated rather than implied

`scope_candidates` is still the denominator A9.2.3 established — the C sources
and headers the file index enumerates — because it is the only denominator Atlas
can state. A build-graph denominator is **not** invented here.

What A9.2.4 adds is one operator-declared classification: `--vendor-root`.
Candidates under a declared vendor prefix are counted as `scope_excluded` and are
**not** counted as uncovered, because an operator saying "this subtree is somebody
else's code" is a classification and treating it as a coverage failure would make
every repository with a vendored dependency permanently unable to state an
absence about its own code.

Atlas guesses at no point. A directory called `vendor` is a directory somebody
named, exactly as a directory called `tests` is. Every other classification is
UNKNOWN rather than assumed production — a production source misclassified as a
test is wrong in the one direction that matters.

The honest limit, unchanged from A9.2.3 and worth repeating: **a source that some
build compiles but that no discovered compilation database names is invisible to
every coverage dimension.** Atlas answers from the build descriptions it can
find, and `code sem-config --compdb` is where an operator adds one it cannot.

### Target class, and why it is UNKNOWN

§11 of the season brief asks for discovered sources to be classified generically:
first-party production target, library, executable, test target, tool, generated,
vendor, unbuilt, unsupported. What Atlas can actually establish is:

| Class | Established from | Status |
| --- | --- | --- |
| test | operator-declared `--test-root` | `tu_test`, `test_scope_known` |
| vendor / third-party | operator-declared `--vendor-root` | `scope_excluded` |
| built at all | membership of an accepted compilation database | `scope_covered` |
| unbuilt or invisible | the difference | `scope_uncovered` |
| generated | membership of a database, since a generated source that is compiled *is* an entry in one | follows the units |
| **library / executable / tool** | — | **UNKNOWN** |

The last row is a real limitation and it is stated rather than approximated. A
JSON Compilation Database records a directory, a command and a file; it does not
say which *target* a unit belongs to. The information is often recoverable from
the `-o` path — `build/CMakeFiles/foo.dir/x.c.o` names a target — but that is a
convention of one build system encoded in a path, and inferring a target class
from a path shape is exactly the guessing `--test-root` and `--vendor-root` exist
to avoid. A production source misclassified as a tool is wrong in the one
direction that matters.

So Atlas reports UNKNOWN rather than assuming production, which is what §12
requires, and the consequence is stated plainly: **the query surface cannot ask
production-only caller questions.** What it can do — and does — is refuse to
claim repository-wide coverage from production-only coverage, because
`test_scope_known = false` is carried on the generation and folds into every
absence the coverage model gates.

## 11. The operator surface

```
atlas code sem-status NAME [--json]
atlas code sem-config NAME [--compdb PATH]...        # pinned, in addition to discovery
                           [--test-root PATH]...     [--no-test-roots]
                           [--vendor-root PATH]...   [--no-vendor-roots]
                           [--exclude PATH]...       [--no-excludes]
                           [--discover|--no-discover]
                           [--auto|--no-auto]
```

Every list flag is a **replacement** of that list, never an addition to it, and
each is independent: adjusting the test roots never drops the exclusions. Every
tri-state left unspecified is left alone — an operator adjusting a path list must
not turn maintenance on or off as a side effect.

`--compdb` no longer means "these are the compilation databases". It means "these
as well as whatever discovery finds", unless `--no-discover` is also given.

`--auto` / `--no-auto` record an **operator intent** with OPERATOR provenance,
which no machine-wide default overrules in either direction.

Writing the configuration re-walks immediately, because the write just changed
what the walk would do. That is also how an operator asks for a walk *now*
without changing anything else: `code sem-config REPO --discover` restates the
default mode and re-walks as a side effect of the write.

**`code sem-status` never walks.** It is a read, and a status command that
mutated could not be the same function the scheduler calls — which is the
property the whole derived-state model rests on. A repository nobody has walked
therefore reads `discovery = UNKNOWN` and holds with
`build_input_discovery_has_not_run_for_this_repository`, which is a different
statement from "there is nothing here" and is what the daemon's discovery sweep
resolves within `ATLAS_SEM_DISCOVERY_INTERVAL_MS`.

`code index REPO` with no `--compdb` walks first and then indexes what the walk
accepted, so an operator never has to name a database Atlas can find.

### What is read-only, and by whom

| Surface | Reads discovery state | Can change it |
| --- | --- | --- |
| `code sem-status`, `--json` | yes | no |
| `sem.status` RPC (ordinary group) | yes | no |
| `/api/v1/sem/status` (gateway) | yes | no |
| `atlas_sem_status` (MCP) | yes | no |
| `code.sem_config` RPC | yes | **operator uid only** |

A model holding every Atlas tool can read the whole of this and change none of
it. There is no MCP tool and no gateway route that enables maintenance, clears an
explicit disable, declares a search complete, adds a compilation database, marks
a generation current or triggers a rebuild.

## 12. Storage

Migration 19, additive:

| Table | Column | Meaning |
| --- | --- | --- |
| `sem_repo_config` | `auto_intent` | UNSET / ENABLED / DISABLED |
| | `auto_intent_by` | DEFAULT / OPERATOR / MIGRATION |
| | `discovery_mode` | AUTOMATIC / MANUAL |
| | `excludes` | prefixes the walk does not enter |
| | `vendor_roots` | prefixes declared to be third-party |
| | `discovery_state` | the last walk's verdict (derived) |
| | `discovered_at` | when it ran (derived) |
| | `discovery_limit` | which ceiling stopped it (derived) |
| `sem_generations` | `discovery` | the verdict this generation was sealed under |
| | `input_count` | compilation databases accepted |
| | `scope_excluded` | candidates under a declared vendor root |
| `sem_build_inputs` | *(new table)* | one row per candidate, accepted or rejected |

`sem_build_inputs` is **DERIVED** in `RETENTION[]` — another walk reproduces it —
and **not prunable by age**, for A5's reason about derived tables: a half-aged
candidate list is not a smaller search, it is a wrong one, and nothing in the
surviving rows would record that some are missing. It is deleted on `repo remove`
because `repositories.id` is a reused rowid.

## 13. Reading a status

```
  build-input discovery COMPLETE
    mode                AUTOMATIC
    candidates          3 accepted, 1 rejected
    last walked         2026-08-17T05:40:11Z
  automatic maintenance enabled
    operator intent     UNSET (DEFAULT)
    policy default      enabled
  build inputs          build/a/compile_commands.json  [DISCOVERED, 200 units]
                        build/b/compile_commands.json  [DISCOVERED, 216 units]
                        build/c/compile_commands.json  [DISCOVERED, 96 units]
                        compile_commands.json  [rejected: a_symlinked_path_is_never_followed]
```

A PARTIAL search additionally prints `stopped by` with the ceiling that stopped
it — every bound that is reached is reported, and a PARTIAL verdict without the
reason tells an operator something was missed without telling them what.

Read it as four separate answers. `COMPLETE` bounds the claim to the search
universe. `UNSET (DEFAULT)` says nobody has expressed an intent and the policy
decided. The rejected candidate is shown because a candidate nobody sees is
indistinguishable from one that is not there.

## 14. Extending this layer

See `docs/extending.md`. In brief: a new discovery state, hold reason, stale
reason, configuration field or manifest field each has a checklist, and every one
of them names the **read-back in `src/core/service_remote.c`** — missing that is
how the socket path and the local path start disagreeing.

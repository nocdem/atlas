# Semantic index freshness, coverage and automatic rebuild (A9.2.3)

Two sentences carry this whole document, and everything else is an argument for
one of them.

> **A SEMANTIC INDEX CAN BE SOURCE-CURRENT BUT COVERAGE-INCOMPLETE.**

> **ZERO RESULTS ARE ONLY NEGATIVE PROOF WHEN THE CLAIM'S REQUIRED SEMANTIC
> COVERAGE IS COMPLETE.**

A9.2.2 made Atlas safe about absence: where coverage could not be shown
sufficient, the answer became `UNKNOWN` rather than "there is no X". That solved
the epistemic problem and left an operational one. Atlas could reach a state
where the repository had moved on, the structural index had caught up, the
semantic index had not, and the only way out was a person remembering to run a
command. Every negative question then correctly answered `UNKNOWN` — safely, and
uselessly.

A9.2.3 makes the daemon own the difference, and makes a generation say what it
actually covered.

---

## 1. The two axes, and why they are two

`atlas_sem_freshness` (A8-CI) asks: **does this generation describe the current
source?** Its answers are `ABSENT`, `CURRENT`, `STALE` and `REBUILDING`.

Coverage (A9.2.3) asks: **how much of the repository did it read?** Its answer is
the coverage manifest on the generation, folded into one boolean where a
negative conclusion has to rest on it.

Neither is derived from the other, and the state that proves it is `INCOMPLETE`:
a generation built from exactly the current source that describes half the tree.
Before A9.2.3 that state was reported as `CURRENT`, and negative questions were
answered from it.

Every surface reports both. A single badge carrying both is the presentation
this season exists to prevent, which is the same rule A9.1 makes about kind and
status, A9.2 about verification state, and A9.2.2 about truth.

## 2. The lifecycle

`atlas_sem_activity` folds freshness, coverage and the build description into the
one value an operator or a model acts on. It is reported *beside* the two axes,
never instead of them.

| State | Meaning | What an operator does |
|---|---|---|
| `UNKNOWN` | nothing established — a read failed, or nobody asked | look at the error |
| `DISABLED` | no build description, or automatic rebuild off | `code sem-config` if this repository should be maintained |
| `UNAVAILABLE` | no usable generation exists | nothing; the daemon builds one |
| `CURRENT` | describes the current source *and* coverage is complete | nothing |
| `INCOMPLETE` | source-current, coverage not complete | widen the compilation database, or fix a failing unit |
| `BUILDING` | a generation is being built; the previous one is still served | nothing |
| `DIRTY` | the source has moved past the published generation | nothing; the daemon rebuilds |
| `FAILED` | the last automatic attempt failed and the source has not moved since | fix the fault; recovery is automatic |

`UNKNOWN` is zero, so a zeroed struct never describes a healthy index and never
causes a compiler to run.

**`CURRENT` never means "a semantic index exists".** It means the generation's
source identity matches the tree, its compilation database matches, its compiler
and analyzer match, the file index is current, every unit parsed, and every
tracked source in scope was read.

## 3. The whole state is derived

There is no dirty bit and there must not be one. Freshness is recomputed on every
read — A6's rule about gate freshness and A4's about link currency — and a stored
flag would be a second answer to a question that already has one, free to
disagree with it.

`atlas_sem_plan_for` is a pure read over the generation rows, the build
description and the repository row. It opens no transaction, takes no lock,
creates no process and writes no row, which is what lets the daemon's scheduler
and `atlas code sem-status` be **the same function**. The scheduler and the
status page agree because they compute the same thing, not because somebody
keeps two things in step.

## 4. Source identity: what invalidates a generation

Every staleness check A8-CI had compares something that moves with a *commit* —
the head, the compilation database, the compiler, the analyzer. Atlas indexes the
**working tree**. A source could be edited, added or deleted with the head
standing still, and all four checks agreed the index was current — which is the
state a developer is in for most of a working day, and precisely the state the
daemon has to notice.

`atlas_sem_source_identity` is one comparable value covering, domain-separated
and length-prefixed:

| Input | Why |
|---|---|
| every live C source and header the file index holds, by path and content hash | the working tree, which is what Atlas indexes |
| the live compilation-database digest | a build description change changes what is compiled |
| the compiler version | the same bytes parse differently under a different front end |
| the analyzer id and version | the same bytes produce different facts under a different algorithm |

A file whose content hash the index does not hold contributes a fixed marker
rather than being skipped: skipping it would make a file Atlas could not read
compare equal to one that was never there, and those are different states.

**An empty stored identity never makes a generation stale.** A generation built
before this season recorded nothing to compare, and "this index did not record
what it was built from" is not evidence that anything moved. It is rebuilt on its
next automatic pass and records one.

### The compilation-database check was dead code until now

Every caller of `atlas_sem_freshness_of` passed `NULL` for the live compilation
database digest, so the branch reporting a changed build description was
unreachable. A8-CI's reason was sound at the time — hashing every database on
every read is a real cost, and a changed one would be caught by the next index.
It stops being sound once the daemon schedules by *noticing*: a check that never
fires is a repository whose build description can change without anything ever
rebuilding it. The decision is reversed, and the cost is one bounded read and one
SHA-256 per declared database per freshness read, of files an operator named.

## 5. The coverage manifest

`tu_complete == tu_total` says every translation unit the compilation database
named was parsed. It says **nothing** about whether that database named every
source in the repository — so on its own it is a statement about the
denominator's own contents. `198/198` was never a coverage claim.

The denominator Atlas can state is the one A0/A1 established by enumerating the
tree.

| Field | Meaning |
|---|---|
| `scope_discovery` | `DECLARED` when the file index was current at publication, `UNKNOWN` otherwise |
| `scope_candidates` | source files the file index holds for this repository |
| `scope_covered` | of those, how many this generation parsed as a translation unit |
| `scope_uncovered` | the difference — **the only number that can refuse an absence** |
| `tu_test` / `tu_production` | the split, from declared test roots and nothing else |
| `test_scope_known` | false when no test roots are declared |

Coverage is **never a percentage**. A denominator Atlas cannot state is one that
would be made up, and `coverage: 87%` reads as precision about exactly the thing
that is unknown.

`UNKNOWN` scope discovery is never sufficient for an absence, which is what makes
every pre-A9.2.3 generation conservative by construction rather than by a rule
that says so.

### Test scope

Atlas does not guess. A directory called `tests` is a directory somebody named,
and classifying on that basis would invent the scope information a
production-only absence rests on. An operator declares test roots with
`--test-root`, and without them `test_scope_known` is false — which is "Atlas
does not know which sources are tests", a different statement from "there are no
test units", and the one that makes a production-scope claim unanswerable rather
than wrong.

A declared root matches on a **path-component boundary**: `tests` matches
`tests/a.c` and not `tests_helper.c`. A production source misclassified as a test
is wrong in the one direction that matters — it would let "no production caller"
be answered while a production caller sits in the file it excluded.

### Generated, ignored and vendored source

Build-generated sources routinely live under an ignored build directory. The file
index never enumerates them, so they are outside the denominator entirely —
counted neither as covered nor as uncovered.

That is why `ATLAS_COVDIM_GENERATED_SOURCE` follows the **units** rather than the
scope manifest: a generated source that is compiled *is* an entry in the
compilation database, and a generated `.c` there is parsed exactly like any
other. Asking the scope manifest about it would be asking a denominator that is
blind to exactly those files. What bounds the claim is that the compilation
database must be current — which, until this season, was a check that could never
fire.

**The honest limit, stated rather than implied:** a source that some build
compiles but that this repository's compilation database does not name is
invisible to both dimensions. Atlas answers from the description it was given,
and `code sem-config` is where an operator gives it.

## 6. The durable build description

Until A9.2.3 a compilation database reached the indexer only as an argument to
the command that ran it. Nothing durable said which build description a
repository has, so the daemon could see that a generation was stale and had no
way to know what to read.

`sem_repo_config` is that description — and it is also the **authority opt-in**,
which is not a secondary use. A8-CI's rule is that indexing runs a compiler over
repository source, so it is an authorised operator action and no model may cause
one. Making a repository change a rebuild trigger would delete that rule for
every registered repository at once. It does not, because `auto_rebuild` defaults
to 0, only an operator writes the row, and migration 18 enables nothing that was
not enabled before it ran.

```sh
atlas code sem-config NAME                       # read
atlas code sem-config NAME --compdb build/compile_commands.json --auto
atlas code sem-config NAME --test-root tests --test-root spec
atlas code sem-config NAME --no-auto             # stop maintaining it
atlas code sem-config NAME --no-test-roots       # clear the declared roots
```

Every flag **replaces** a list rather than adding to one, which is why the
no-flag form reads: an operator sees the description before changing it. A flag
that is not given leaves its value alone — `--auto`/`--no-auto` is a tri-state
for exactly that reason, so adjusting a path list cannot silently turn automatic
rebuilding on or off.

`code.sem_config` is in the **operator-uid** RPC group beside `code.index`, and
for a stronger reason: `code.index` runs a compiler once when an operator asks,
and this decides whether the daemon runs one every time the repository changes.
There is no MCP tool and no gateway route. Every other peer — including
`atlas-worker` and every MCP client — is told the method does not exist.

## 7. Automatic rebuild

```
    relevant repository change
        ↓  (freshness, recomputed)
    DIRTY
        ↓  (the watcher's timer, every ATLAS_SEM_SWEEP_INTERVAL_MS)
    queued to the writer thread — the same job `code.index` queues
        ↓
    BUILDING  (the previous generation is still served throughout)
        ↓
    validated, manifest sealed
        ↓
    atomic publication — one transaction
        ↓
    CURRENT
```

The sweep runs on the watcher's thread because **the watcher is the daemon's
timer**, which is the argument A8's recovery sweep already makes. It holds a
read-only handle, so it cannot write and does not need to.

### Coalescing falls out rather than being implemented

The scheduler always builds *now*. There is no queue of source states to build
from — there is one question, asked again on the next tick. A developer saving
six files during a build produces one further build, not six, and no save is
lost: the build that follows is a build of whatever the tree holds when it
starts. If that has moved again by the time it publishes, the next tick sees
STALE and builds again. The system converges on the newest state without ever
building an intermediate one, and **correctness never depends on timing** — only
how soon it converges does.

### Ordering against the file index

The sweep **holds** while the file index is behind, with reason
`the_file_index_has_not_caught_up_with_the_working_tree`. The source identity is
computed from the file index's content hashes, so a generation built now would
describe hashes nobody can vouch for and would be stale the moment the
reconciliation pass completes. The pass is already scheduled; waiting is the
ordering, not a delay.

### Backpressure and concurrency

The job goes through `atlas_writer_submit_sem_index` — the same entry point
`code.index` uses — so a rebuild is serialized against every other write exactly
as a manual one is. A full queue leaves the repository dirty and the next sweep
tries again. **At most one semantic index runs at a time across the daemon**,
because the writer thread is the one serialized writer; that is the existing
architecture rather than a new scheduler.

One repository failing never blocks another from being considered: the sweep asks
about each independently, and the answer for a failing one is a hold.

`atlas_writer_sem_index_pending` is the guard against queueing a second build.
The durable record cannot answer it — a job dequeued but not yet at the point of
opening a generation leaves no `RUNNING` row — and a flag on the scheduler's side
cannot either, because nothing tells the scheduler when a job finished. **The
daemon's first cut kept such a flag and passed it into the plan it then used to
decide whether to clear it**, so the plan always said `BUILDING`, the flag never
cleared, and the repository reported `DIRTY` for ever having rebuilt exactly
once. The writer owns the queue and runs the job, so the writer is asked.

## 8. Generation isolation and atomic publication

Unchanged from A8-CI, and A9.2.3's job was to prove it rather than rebuild it. A
generation's rows are written while the previous generation is still being
served; `atlas_db_sem_publish` marks it `COMPLETE` and repoints `sem_current` in
one transaction. There is no path that makes a partially written generation
visible.

The coverage manifest and the source identity are written **inside that
transaction**, so a reader can never see a published generation whose coverage is
still zero and read it as "nothing was covered". Both are *measured* before the
transaction opens, because computing the source identity reads files and A1
forbids a file read inside a write transaction.

A failure at any point leaves the generation `FAILED` and the previous one
current. A crash leaves a `RUNNING` generation nothing points at, which the next
pass reports and reaps.

### A pass that finds nothing to do still records that it looked

A repository holds sources the compilation database does not name — on a real
tree, hundreds. Editing one moves the live source identity, moves no unit digest
and moves no scope count, so the pass finds nothing to do and publishes no
generation. Without re-stamping the stored identity the repository is stale again
on the next tick and rebuilds **every sweep, for ever**.

Re-stamping is honest rather than a paper over. The pass has just verified that
every input determining what the generation would contain is identical: each
unit's digest over its transitive include closure, the compilation-database
digest, the compiler, the analyzer, and the scope counts. The generation
therefore describes the new tree to exactly the same extent it described the old
one. It is the same move as sealing a unit's input digest at the end of a pass —
recording what was measured, once the measurement is complete.

## 9. Source changes during a build

The identity is measured **after** the pass and before the publishing
transaction. What a generation may claim to describe is the tree as it stood when
the last unit was read; recording the identity Atlas saw at the start would claim
a tree the pass never finished looking at.

When the tree moves mid-build the outcome is the honest one: the later identity
is recorded, the generation publishes, and the very next freshness read compares
it against a tree that has moved again and reports `STALE`. The scheduler then
rebuilds. **A generation is never published as describing a state it did not
observe.**

## 10. Failure and the retry governor

A failed automatic attempt records the source identity it was made at, a fixed
Atlas reason and a timestamp, and increments a count. A further automatic attempt
is allowed only once the source identity has **moved past** the one that failed.

Never after an interval: that would retry an unbuildable tree for ever, running a
compiler over the whole repository every time and achieving nothing. What
legitimately makes another attempt worth making is that the inputs changed — and
fixing the fault is exactly that, so recovery is automatic.

A rebuild an operator asks for explicitly is *not* governed by this. `code index`
is a different decision by a different principal, and its failure is reported to
them every time rather than held.

The identity recorded is the one the attempt **started** from. If the tree moves
during a failed build, recording the later identity would block a retry that has
every reason to succeed.

## 11. A9.2.2 integration

A9.2.3 creates no second epistemic engine. It feeds the coverage dimensions
A9.2.2 already had.

| Dimension | Follows |
|---|---|
| `semantic_generation` | the units: every unit the compilation database named parsed |
| `direct_calls` | the units |
| `generated_source` | the units, via a current compilation database (§5) |
| `tracked_source` | the **scope manifest** — every tracked source in scope was read |
| `indirect_calls` | the scope manifest, and zero address-takes |
| `tests` | **nothing — always `UNKNOWN`** (see below) |

### The tests dimension, and what is honestly missing

`ATLAS_COVDIM_TESTS` is set by nothing and is always `UNKNOWN`. No verifier's
absence rests on it, so this costs no correctness — but it is worth stating
rather than leaving to be discovered.

A9.2.3 records the test/production split **on the generation** (`tu_test`,
`tu_production`, `test_scope_known`) and reports it on every surface, and
**nothing consumes it**. There is no production-scope caller verifier: Atlas
cannot currently be asked "does any *production* caller of X exist" at all.

So the §45 distinction — "no production caller" versus "no caller anywhere
including tests" — is **inexpressible today rather than wrongly answerable**,
which is the safe half of the two. `atlas.no_proven_caller` answers the second
question over the whole indexed scope, and says so in its own scope sentence.
The scope information a production-only claim would need now exists and is
recorded; the verifier that would consume it does not, and adding one means
adding a row to `atlas_verify_verifier_absence_dims` that includes
`ATLAS_COVDIM_TESTS` and a source for that dimension.

### One currency rule

Before A9.2.3 the verify layer derived currency from `generation.commit_id ==
repositories.scanned_head` in SQL while every other surface asked
`atlas_sem_freshness_of`. The moment freshness learned about the working tree the
two disagreed: after an uncommitted edit the verifier called a generation current
and answered negative questions from it. It now asks `atlas_sem_plan_for`, which
is a read. **One implementation of one currency rule.**

The consequences, as fixtures:

| Fixture | State | Result |
|---|---|---|
| A | one caller, unit-partial index | `PRESENT` — the asymmetry |
| B | zero callers, address escapes | `UNKNOWN`, `INDIRECT_CALLS_UNRESOLVED` |
| C | zero callers, complete bounded scope | `ABSENT` |
| C2 | zero callers, source-current, `scope_uncovered > 0` | `UNKNOWN`, `COVERAGE_PARTIAL` |
| D2 | one caller, `scope_uncovered > 0` | `PRESENT` |
| E | zero callers, stale generation | `UNKNOWN`, `SEMANTIC_INDEX_STALE` |
| F | "no PROVEN direct caller", complete direct enumeration | `ABSENT` |

The asymmetry is the shape of the world, not a convenience: finding one caller
proves a caller exists however incomplete the index, because an incomplete index
cannot conjure a call that is not there. A gate applied to both directions would
make Atlas uselessly cautious rather than correctly cautious.

## 12. Reading the state

```sh
atlas code sem-status NAME            # freshness, coverage, state, failures
atlas code sem-status NAME --json     # the same, machine-readable
atlas code sem-config NAME            # the build description and what is due
```

Over the socket: `sem.status` (ordinary read), `code.sem_config` (operator-uid).
Over the gateway: `/api/v1/sem/status`, read-only, `graph:read`.
Over MCP: `atlas_sem_status`, which relays `sem.status` verbatim — so a model
sees the state and the manifest, and there is no tool that can change either.

**The CLI's `--json`, the socket, MCP and the gateway all emit the same keys.**
The first cut nested the derived state under `semantic_state` in the CLI and left
it flat on the wire, which would have made a client have to know which produced a
document.

## 13. Reason codes

Fixed Atlas strings, never assembled from repository bytes or compiler output.

*Why a generation is stale* (`ATLAS_SEM_STALE_*`): the repository moved; a
compilation database changed; the compiler changed; the analyzer changed; the
file index is not current; the last generation did not complete; **the working
tree changed** (A9.2.3).

*Why the scheduler is not building* (`ATLAS_SEM_HOLD_*`): no operator has enabled
automatic rebuild; the build description names no compilation database; a
generation is already being built; the last attempt failed and the source has not
changed since; this Atlas was built without libclang; the published generation
describes the current source; the file index has not caught up.

A value that arrives over a socket is a *matching* string, not Atlas' string, and
is interned to Atlas' own literal before it reaches an operator or a model.

## 14. Manual rebuild

`atlas code index NAME --compdb PATH` still exists and is unchanged. It uses
**the same pipeline** — `atlas_sem_index_on`, the same generation build,
validation and publication — as the daemon's automatic rebuild. There is not an
automatic implementation and a manual one with divergent semantics.

It remains diagnostic and administrative. Normal semantic freshness does not
depend on anybody remembering to run it.

## 15. Observed costs

Observations on one machine, from one run each. **They are observations, not
bounds** — a different tree, a larger compilation database or a busier machine
produces different numbers, and nothing here is a limit Atlas holds.

| Measurement | Observed |
|---|---|
| `code sem-status` on a repository with no build description | 23 ms per invocation |
| `code sem-status` on a repository with two databases totalling 485 KiB and 1,208 C sources and headers | 42 ms per invocation |
| a first semantic index of a 3-unit repository, scheduled and built unaided | 4 s from the edit to `CURRENT` |
| an automatic rebuild after an uncommitted edit, same repository | published within 4 s of the edit |

Both figures include process start, which dominates the first. The difference —
about 19 ms — is what this season adds to a semantic read of that repository:
one bounded read and SHA-256 of each declared compilation database, and a digest
over every C source and header the file index holds.

It is paid on **every** semantic read of a configured repository, and it scales
with the size of the compilation databases. A first cut paid it several times per
response, because `sem.status` computed freshness once for its header and again
for the derived state; collapsing that to one computation took the same
measurement from 61 ms to 42 ms. Two computations within one response could also
have disagreed if the tree moved between them.

A repository with no build description pays nothing: there is nothing declared to
hash.

## 16. Observed on a real repository

The finding this season exists for, measured on a 761-source C repository whose
semantic index was built before A9.2.3:

```
  translation units 416
    complete        416
    ...
  scope discovery   UNKNOWN
```

Under A9.2.2 that generation asserted `tracked_source = COMPLETE` — every unit
the compilation database named had parsed — and could therefore support "there is
no caller of X". Counted against the tree the file index enumerated, it covers
**369 of 761** C sources. Nearly 400 sources it had never read.

Nothing was wrong with the index. `416/416` was true. It simply was not a
coverage claim, and reading it as one is the error the manifest exists to end.

The same generation now reads `scope_discovery: UNKNOWN`, because it was built
before the manifest existed and recorded nothing from which its scope could be
reconstructed — so it supports no absence at all until it is rebuilt. That is
migration 18's conservatism, and it is the correct answer rather than a gap.

## 17. What is not claimed

- **Atlas does not observe a running system.** No verifier and no part of this
  layer reads deployed configuration or runs the software. Repository absence is
  not operational absence.
- **A compilation database is the authority on what is compiled, and Atlas does
  not discover one.** A build Atlas was not told about is a build Atlas cannot
  describe.
- **A successful build does not imply complete coverage.** That sentence is the
  reason `scope_uncovered` exists.
- **Atlas does not guess which sources are tests**, and **no verifier consumes
  the split it records**. "No production caller exists" is not a question Atlas
  can currently be asked — inexpressible rather than wrongly answerable, which
  is the safe half. See §11.
- **The sweep interval is not a correctness property.** A sweep that happens late
  converges late. Nothing about the model depends on how quickly it runs, which
  is why it is a compiled-in constant and not a policy key.

# Impact gates and stale-decision detection (A6)

A4 made it possible to record that something was decided and to bind that record
to the bytes it was decided about. A6 asks the question those bindings were
recorded for: **is this decision still about the code that is there now?**

It answers deterministically, from stored Atlas facts and stored Git facts. No
model is involved, nothing is cached, and the same database with the same
arguments produces the same answer on any machine and in any order.

---

## What the words mean

### Freshness, per approved revision

| Verdict | What Atlas observed | What it does **not** mean |
|---|---|---|
| `FRESH` | Every anchor still resolves to what it was validated against, and nothing in the change range since the last validation point reached the decision by any path Atlas walked. | That the decision is correct. Atlas checked bytes and edges, not intent. |
| `STALE` | Something the decision is directly bound to moved: content changed, an anchor disappeared, or a name that resolved to one thing now resolves to several. | That the decision is wrong. `STALE` **requires human revalidation** and nothing more. |
| `IMPACTED` | The direct anchors still hold, but a bounded walk from them reached a file that changed in the range. | That anything is broken. It is a conservative review signal. |
| `UNKNOWN` | Atlas could not prove a safe answer. | That something is wrong with the decision. It usually means something is missing from the *index*. |

**`STALE` does not mean the decision is wrong.** Atlas has no way to know whether
an architectural decision survives a change to the code it concerns — that is a
question about intent, and Atlas holds bytes and graph edges. The whole of what
`STALE` claims is "the anchors moved". The whole of what `IMPACTED` claims is "a
bounded walk from the anchors reached something that moved". Both are requests
for a person to look.

`UNKNOWN` **fails closed**. A gate that answered `PASS` on incomplete
information would be worse than no gate, because the answer has the same shape
as a real one.

### The gate, per query

    any UNKNOWN                 -> BLOCKED
    else any STALE or IMPACTED  -> REVIEW_REQUIRED
    else                        -> PASS

`BLOCKED` is absorbing: once a query has failed to prove one thing, proving the
next does not make it safe. The fold is a function — `atlas_gate_fold` — rather
than prose, so the tests ask it instead of agreeing with a second copy of it.

---

## Reason codes

Stable, machine-readable, and a closed Atlas vocabulary: nothing
repository-derived and nothing model-derived ever becomes one. Every reason
implies exactly one freshness, and an assessment's verdict is the **weakest**
freshness among its reasons — so a verdict can never disagree with its own
explanation.

| Reason | Freshness | Meaning |
|---|---|---|
| `NO_RELEVANT_CHANGE` | FRESH | Every check declined to weaken the answer. The only route to `FRESH`. |
| `DIRECT_EVIDENCE_CHANGED` | STALE | A bound anchor's content differs from the validated baseline. |
| `LINKED_PATH_MISSING` | STALE | A bound path is gone or deleted. |
| `LINKED_SYMBOL_MISSING` | STALE | A bound symbol no longer resolves — including after a rename. |
| `LINKED_SYMBOL_AMBIGUOUS` | STALE | The name now names several definitions. Atlas will not choose. |
| `LINKED_COMMIT_MISSING` | STALE | A bound commit is not in the index. |
| `DEPENDENCY_CHANGED` | IMPACTED | A bounded walk from the anchors reached a path in the change range. |
| `INDEX_LAG` | UNKNOWN | Git HEAD is ahead of the indexed head, the index is not current, or a requested state was never indexed. |
| `STRUCTURAL_INDEX_STALE` | UNKNOWN | The structural graph is behind the file index, so transitive impact cannot be computed. |
| `UNREACHABLE_BASE` | UNKNOWN | The walk ran out of ingested history before reaching the validation point. |
| `HISTORY_REWRITTEN` | UNKNOWN | Every reachable commit was expanded and the validation point was not among them. |
| `TRAVERSAL_LIMIT` | UNKNOWN | A bound was reached, so the answer would be a subset of an answer. |
| `EVIDENCE_UNRESOLVED` | UNKNOWN | An anchor resolved to nothing Atlas can compare. |
| `MISSING_VALIDATION_POINT` | UNKNOWN | No basis and no revalidation, so there is no point to measure a range from. |
| `REPOSITORY_AMBIGUOUS` | UNKNOWN | The decision's durable identity names a different repository. |
| `CONTENT_HASH_MISMATCH` | UNKNOWN | Stored content no longer hashes to its recorded digest. |
| `SCOPE_NOT_ASSESSABLE` | UNKNOWN | No anchor into the code, and no claim to the whole repository. |

---

## The two kinds of evidence, and the asymmetry between them

A **direct anchor** carries a content hash captured when the decision was
written, so Atlas can compare bytes.

Everything reached by **traversal** has no such snapshot. The only thing Atlas
can ask about it is whether the path appears in the range of commits since the
validation point.

The consequence is deliberate and is reported rather than smoothed over:

* A direct anchor that changed and **changed back** compares equal and is
  `FRESH`. The code is byte-identical to what was validated.
* A *dependency* that changed and changed back is still `IMPACTED`, because
  path-level history is all there is to go on — and `IMPACTED` is a request to
  look rather than a finding.

## Which direction the walk runs

**Outbound**: from the decision's anchors to what they depend on. A decision
recorded about a file is a statement whose basis includes what that file depends
on; if a dependency moved, the basis may have moved with it. The other direction
— what depends on the file — describes code whose *own* basis may have changed,
and those are that code's decisions to worry about.

---

## The validation point

Every assessment measures its change range from the last point a human validated
the revision against:

1. the newest revalidation record for this revision **in this repository
   identity**, if there is one;
2. otherwise the revision's own `basis_head` — the commit it was proposed and
   approved at.

The fallback is the conservative one. An approval happens after a proposal, so
measuring from the proposal's basis can only widen the range, and a wider range
produces more review rather than less.

**Which baseline the direct-evidence question uses depends on whether the
decision has been revalidated**, and this is load-bearing. A revision is
immutable, so its link snapshots can never be updated. If they stayed the
baseline, a decision an operator had just checked against the current code would
report `STALE` for ever. So:

* **No revalidation** → each link's own snapshot is the baseline.
* **A revalidation** → the evidence digest that revalidation recorded is the
  baseline.

One reason is derived the same way under both: an anchor Atlas cannot resolve at
all is `EVIDENCE_UNRESOLVED`. `FRESH` claims the required evidence still
resolves, and "Atlas could not look" never establishes that.

---

## Human revalidation

    atlas decision revalidate NAME DECISION-ID

Reuses A4's operator channel unchanged: an interactive terminal, a short-lived
single-use capability bound to this exact revision and content hash, and a
confirmation typed against that hash. A6 binds two more things into the
capability and therefore adds two more refusals.

A revalidation capability is bound to:

* the repository (id and durable identity), the document, the revision and its
  content hash — A4's tuple, unchanged;
* the **indexed commit** at the moment the capability was issued;
* an **evidence digest** of what the revision's anchors resolved to at that
  moment;
* the assessment the operator was shown — freshness and reason codes — so the
  record preserves what was actually seen rather than what a later recomputation
  would have produced;
* an expiry, and a single-use nonce.

It is refused on: replay, expiry, a different revision, a different intent, a
different repository, a content hash that moved, a revision that is no longer
the approved one, **commit drift** (the indexed head is not what it was) and
**evidence drift** (the anchors resolve differently).

Both A6 refusals are pure database reads. Consumption happens on the writer
thread inside the transaction that spends the capability, where A1 forbids
creating a process or reading a file — so drift is detected without Git and
without the filesystem.

### What revalidation does and does not do

It **appends** a `decision_validations` row. It does not edit the approved
revision, does not change its lifecycle status, appends no `decision_events`
row, and does not withdraw the assessment: `prior_freshness` and
`prior_reasons` are stored alongside, because a ledger that said a decision was
revalidated but not what was wrong with it would be a record of the answer
without the question.

`LOCAL_OPERATOR_CONFIRMED` on a validation row means the operator channel was
used. Read it literally, exactly as A4 says: it does not name a person, does not
prove one was present, and is not a signature. A same-UID process that can drive
a pseudo-terminal — including an AI agent with shell access — may imitate the
channel.

---

## The AI trust boundary

Atlas exposes **no** operation through MCP, hooks or any AI-facing method that
clears, overrides, caches or otherwise changes a freshness result, and none that
revalidates a decision. There is no such operation anywhere to expose.

A model may **read**: one MCP tool, `atlas_gate_check`, forwarding to one RPC
method, `gate.check`. Both are reads. `decision.revalidate` exists over IPC
beside `decision.approve` and is equally useless without a capability that only
the terminal channel can obtain.

When a decision is `STALE`, `IMPACTED` or `UNKNOWN`, the result says so in a
closed vocabulary with reason codes and the exact repository state, and decision
prose in the result is labelled `UNTRUSTED_DATA`. Approved decision prose is
accepted project policy *and* still untrusted data — A4's rule, unchanged, and
freshness does not alter it in either direction. No freshness result enters the
automatic context envelope.

`tests/test_gate_trust.c` asks a live daemon for every method name such an
operation would plausibly have and requires every one to fail, checks that no
MCP tool name carries a mutating verb, and checks that no schema declares a
capability argument.

---

## CLI and JSON

    atlas gate check NAME [--at OID] [--path P]... [--depth N] [--json]
    atlas gate show NAME DECISION-ID [--at OID] [--json]

`--at` names the exact repository state to assess. A state Atlas has not indexed
is `INDEX_LAG` and therefore `BLOCKED`, never an extrapolation. `--depth` is
refused above the maximum rather than clamped, because a silently reduced depth
is a silently smaller answer. Output is ordered by decision id, so two runs over
one database emit the same document.

### Exit codes

`0`–`7` keep their meanings exactly (see `README.md`). A gate outcome is not an
error and is never reported as one; it gets its own codes:

| Code | Meaning |
|---|---|
| `0` | `PASS` |
| `8` | `REVIEW_REQUIRED` |
| `9` | `BLOCKED` |

They are distinct because an automation that treats "a human should look at
this" and "Atlas could not tell" identically will eventually be handed the
second and behave as though it got the first. A non-zero gate exit still writes
exactly one complete document to stdout and no error document — the same
contract `atlas daemon ping` follows.

---

## Consistency and the snapshot model

One assessment is computed from one database snapshot. The order is:

1. Open a read transaction and read the repository row. SQLite's deferred
   transaction takes its snapshot at the first read, so from there every query —
   decisions, links, the commit graph, the structural relations — sees one
   database whatever the daemon commits meanwhile.
2. Ask Git for the live HEAD **after** the snapshot. A commit that lands between
   the two makes them disagree, and a disagreement is `BLOCKED`. The race costs
   a refusal, never a pass on a state Atlas has not seen.
3. Assess every decision against that fixed pair.

The gate takes **no lock**, writes no row and creates no process. Normal
read-only indexing is never blocked by it, because the gate has nothing with
which to block it.

---

## Traversal limits

| Bound | Value | On reaching it |
|---|---|---|
| Ancestry commits walked | 20 000 | `TRAVERSAL_LIMIT` → UNKNOWN |
| Distinct changed paths collected | 50 000 | `TRAVERSAL_LIMIT` → UNKNOWN |
| Structural walk depth | 3 (max 8) | — (configured, not reached) |
| Structural walk nodes | 2 000 | `TRAVERSAL_LIMIT` → UNKNOWN |
| Decisions assessed per query | 2 000 | report-level limit → BLOCKED |
| Path prefixes per query | 64 | usage error |

Reaching a bound is never absorbed. A walk that stopped early cannot report that
it found nothing, and a change set Atlas could not finish enumerating must not
be tested for membership at all — every miss would be indistinguishable from a
path that was never in it.

---

## Measured cost

`scripts/perf-a6.sh` builds a repository at roughly DNA's scale — 5 988 `.c` and
`.h` files, 79 392 symbols, 413 920 relations — indexes it, and generates a
decision corpus through the real write path, of which 500 documents are approved
and 455 are assessable. It asserts its own scale floors and its own limits and
exits non-zero rather than printing a number nobody checks.

**The limits are the required bounds. The figures below are observations from
one run on one machine, and they move by tens of milliseconds between runs with
nothing but load.** Report them as observations.

| Query | Observed | Peak RSS observed | Required limit |
|---|---|---|---|
| Fresh, nothing changed since the index was built | 78 ms | 6.9 MiB | 2 000 ms / 256 MiB |
| After a direct anchor changed (101 decisions went stale) | 101 ms | 7.0 MiB | 2 000 ms / 256 MiB |
| After a dependency changed (transitive walk runs) | 105 ms | 7.0 MiB | 2 000 ms / 256 MiB |
| One decision (`gate show`) | 23 ms | 4.4 MiB | 2 000 ms / 256 MiB |
| All 455 decisions (`gate check`) | 102 ms | 7.2 MiB | 2 000 ms / 256 MiB |

Two properties matter more than any of those numbers, and the script asserts
both rather than describing them:

* **A single-decision query does not cost what the whole repository costs** —
  23 ms against 102 ms. The uid filter runs before the revision is loaded and
  long before anything is walked, so asking about one decision is one
  assessment's work.
* **A scan runs to completion while a gate query is in flight.** The gate opens
  in `ATLAS_CTX_READ` and never takes the writer lock, even when it is free.

There is no caching and no incremental state. Every assessment is recomputed in
full, which is the correctness property the phase is built on; the reason it is
affordable is that the work is bounded per decision rather than proportional to
the index.

## Schema and retention

A6 adds **migration 7**, taking the schema to 7:

* `decision_challenges` is rebuilt to widen its `intent` CHECK with
  `revalidate` and to add `indexed_commit`, `evidence_digest`,
  `prior_freshness` and `prior_reasons`. Row ids are preserved exactly, because
  `decision_events.challenge_id` is a soft reference and a renumbering would
  re-point every approval record at somebody else's capability.
* `decision_validations` is new: the append-only revalidation ledger.

Both are classified `CANONICAL` and **not prunable** in `RETENTION[]`. An
age-pruned validation history would silently move every surviving decision's
validation point backwards — widening its change range — and, for decisions
whose every record went, remove the only proof that anybody ever looked.

A6 adds no background deleter and no automatic cleanup. Nothing prunes on a
timer, at startup, on low disk, or as a side effect of another command.

Schema-6 backups remain verifiable: `atlas backup verify` opens the file
read-only and never migrates it, which is A5's rule and is unchanged. A restored
schema-6 database migrates forward to 7 through the ordinary supported path.

---

## What `atlas doctor` checks

The revalidation ledger's **structure**: every row must name a revision and a
document that exist, must be bound to the digest that revision carries, must
record a repository state, and must point at a challenge that was issued for a
revalidation and actually consumed.

It deliberately does **not** re-derive evidence digests against the live index.
Those are meant to drift — that is the entire phase — and a diagnostic that
reported ordinary code changes as corruption would teach everybody to ignore it.
Reported, never repaired, like everything else `doctor` finds.

---

## Known limitations

* **Atlas is not a compiler.** The structural graph carries A3's resolution
  vocabulary, and a decision whose impact depends on a macro-produced call or a
  function pointer will not have that edge. Impact results are candidates.
* **Rename is not tracked.** A renamed symbol is `MISSING`, never followed.
  Atlas has no deterministic identity evidence that the new name is the old
  object, and choosing would be inventing.
* **History is only as complete as the ingest.** `commits` may be bounded by a
  past `--max-commits`, and the ancestry walk cannot distinguish "not an
  ancestor" from "we do not hold that much history" — so it reports the second
  whenever a parent it needed was never ingested.
* **Transitive impact is path-level.** See the asymmetry above.
* **A decision with no anchors is not assessable** unless its scope is
  `REPOSITORY`.
* **The structural index must be current.** Without it, transitive impact cannot
  be computed and no decision can be `FRESH`. Run `atlas code sync`.

## Recovery

| Symptom | What it means | What to do |
|---|---|---|
| Everything `BLOCKED` with `INDEX_LAG` | The index is behind the working tree. | `atlas scan NAME` (or start the daemon). |
| Everything `BLOCKED` with `STRUCTURAL_INDEX_STALE` | The graph is behind the file index. | `atlas code sync NAME`. |
| `UNREACHABLE_BASE` | Ingested history does not reach the validation point. | Re-scan without `--max-commits`, or revalidate to move the point forward. |
| `HISTORY_REWRITTEN` | The branch was rebased or force-pushed. | Revalidate against the current state. |
| `CONTENT_HASH_MISMATCH` | Something outside Atlas edited a stored revision. | `atlas doctor`; restore from a verified backup. |
| A `doctor` finding about the validation ledger | Structural damage. | Restore from a verified backup. See `docs/operations.md`. |

## Deferred

Not started, not claimed, and explicitly out of A6's scope: the full dedicated
security review, clangd integration, the Atlas orchestration/control plane, the
Claude dispatcher, the GitHub issue/PR/review loop, model routing, and Testnet 2
automation. A6 provides a reusable gate evaluator for a future orchestration
layer; it implements no orchestration.

# A9.2 closure report

**STATE: A9.2 CLOSED — DETERMINISTIC ENFORCEMENT VERIFIED / EMPIRICAL SHADOW**

Deterministic automatic lifecycle is implemented, tested, deployed and **enabled
on the installed system**, with historical source calibration unavailable and
correctly not required. Empirical verification is complete as machinery and
closes in shadow, because there is no eligible resolved history to calibrate
against. Judgment remains operator-only and always will.

---

## A. Source lock

| | |
|---|---|
| starting HEAD | `86cb07892e5ccb6f34d1b8d741670ed1057df5a9` |
| parent | `897395c57bdcaddbe9000ffe0f771da2187e6b31` |
| branch | `a9-remote-mcp-web` |
| tree at start | clean — no modified, staged or untracked files |

## B. Installed baseline, before

| | |
|---|---|
| binary | `/usr/local/bin/atlas`, sha256 `aa3ad73f…40727e2`, phase A9.1 |
| schema | 13 |
| `atlas.service` | active |
| `atlas-dispatcher.service` | active |
| `atlas-gateway.service` | inactive (unchanged by this season) |
| registered repositories | `atlas` (id 2), `dna` (id 1), `swapper` (id 3) |
| index | 1,220,628,480 bytes |
| decision documents / revisions / events / links | 105 / 119 / 188 / 535 |

Knowledge counts before, by repository:

| repo | count | proposed | approved | rejected | superseded | resolved |
|---|---|---|---|---|---|---|
| atlas | 2 | 2 | 0 | 0 | 0 | 0 |
| dna | 50 | 44 | 56 | 1 | 0 | 0 |
| swapper | 0 | 0 | 0 | 0 | 0 | 0 |

DNA kinds before: DECISION 84, POLICY 2, INVARIANT 6, OPERATIONAL_FACT 2,
ACCEPTED_RISK 1, OBLIGATION 1, PARKED 4, REJECTED_ALTERNATIVE 1.

**Pre-deployment backup:** `pre-a92-schema13.db`, 1,220,628,480 bytes, sha256
`7e6261cc…9c8bc60`, schema 13, `integrity_check ok`, `foreign_key_check ok`,
105 documents checked, 119 revisions rehashed, **0 corrupt, 0 ledger
mismatches**. This is the rollback target.

## C. Data model

Ten new tables (migration 14, purely additive) and one CHECK widened
(migration 15).

| table | holds |
|---|---|
| `verify_actors` | who spoke, and how well Atlas knows it is who it says |
| `verify_claims` | one discrete proposition, bound to a revision and artifacts |
| `verify_evidence` | where a fact came from, in columns rather than a blob |
| `verify_evidence_deps` | declared derivation edges — the input to independence |
| `verify_attestations` | one actor's verdict at one moment; never overwritten |
| `verify_attestation_evidence` | which evidence each attestation rests on |
| `verify_results` | append-only: what an aggregation concluded, and under which algorithm |
| `verify_outcomes` | resolved ground truth, with an eligibility flag |
| `verify_reliability` | actor × domain accuracy, never one global percentage |
| `verify_lifecycle_audit` | why Atlas itself changed a state — **and the warrant** |

**Verification state is derived on read, never stored on a decision.** There is
no `verification_state` column on `decision_documents`, and
`tests/test_verify_engine.c` asserts that no column matching `%verif%` was added
to it. That is A6's rule about freshness and A4's about link currency: a cached
judgement is wrong between the change and the recomputation.

## D. A9.1 compatibility

Migration 13 → 15 was exercised against a database built by the **installed
A9.1 binary** carrying one record of each of the eight kinds, then against the
1.16 GiB production index.

- revision rows byte-identical before and after (id, content_hash, state)
- event rows byte-identical
- all eight kinds preserved
- `sqlite_sequence` for `decision_events` preserved
- `PRAGMA foreign_key_check` clean, `integrity_check ok`
- production after migration: 105 / 119 / 188 / 535 — unchanged
- knowledge counts after migration identical to the table in §B

**No existing record became verified.** A record with no claims aggregates to
`UNVERIFIED`, and production holds zero claims. Nothing fabricates historical
confidence; no lifecycle status changed during migration.

## E. Authority

| authority | who holds it | what it can do |
|---|---|---|
| model proposal | any adapter — MCP, hook, CLI | write PROPOSED revisions. Unchanged. |
| operator | the operator channel, or the operator uid over the socket | approve, reject, supersede, resolve, revalidate. Unchanged. |
| verification policy | a root-owned file the running process cannot edit | the transitions that file names, and nothing else |

`ATLAS_DECISION_ACTOR_VERIFICATION_POLICY` is a fifth actor. It is **not**
`ATLAS_AUTOMATIC` (which means "mechanically implied by another Atlas
operation") and **not** `LOCAL_OPERATOR_CONFIRMED`. It is refused by
`atlas_decision_actor_writable_by_adapter`, and
`decision_revisions.proposed_by` deliberately **keeps its four-value CHECK**: a
policy may move a record between states, never author one.

### The honesty limit, stated as the code states it

> `VERIFICATION_POLICY` says a policy Atlas could not itself edit named this
> exact transition, and the gates that policy set were met. It does **not** say
> the record is true, does **not** say a person agreed, and confers nothing
> beyond the one transition the warrant named.

The limits mirror the operator channel's. There, Atlas cannot prove a person
acted. Here, Atlas can prove precisely *what* acted — a named policy at a
recorded hash, over a recorded result — and cannot prove the policy was *wise*.

## F. Deterministic verification

Four verifiers, **every one a read**: `atlas.content_hash`,
`atlas.symbol_present`, `atlas.symbol_absent`, `atlas.proven_edge`. None creates
a process, runs a build or executes a command. That restriction is deliberate;
`docs/verification.md` names the sandbox a command-running verifier would have to
reuse and states that the argument for one belongs to whoever needs it.

Scope is preserved: every verifier writes the sentence describing what it
actually established, and `UNAVAILABLE` is never reported as `FAIL` —
`atlas.symbol_absent` requires a **complete** generation, because "I did not find
it" over part of a repository is not "it is not there".

Supported transitions: `PROPOSED → APPROVED` and `APPROVED → RESOLVED`, subject
to the kind-aware state machine. Approving over an existing approved revision is
refused (that is a supersession).

## G. Empirical verification

`atlas-reliability-v1`. Integer arithmetic throughout, no floating point, so
reproducibility is a property of the code. Independence is union-find over
declared derivation edges plus shared evidence, with undeclared interpretation
folded into one shared group. **Within a group only the strongest attestation
counts** — the anti-inflation rule.

Verified by test: forty duplicated attestations produce the same score as one;
three differently-named agents over one root produce one evidence group; a
declared edge collapses two groups into one; a lone source cannot reach
certainty; a proposer supporting itself is discounted.

## H. Reliability

`actor × domain`, never one permanent global figure. Priors are explicit,
versioned (`ATLAS_VERIFY_PRIOR_VERSION = 1`), vendor-neutral and documented in a
table. Nothing is `human = 100` or `model X = 95`; identity dominates class,
because "Atlas ran the compiler" versus "something told Atlas the compiler said
so" is the largest real difference available and the one an attacker controls.
Nothing reaches full scale, so certainty must be assembled from independent
groups.

## I. Calibration

**`INSUFFICIENT_DATA` for every actor and every domain. Sample count: 0.**

The reason is worth stating precisely, because it is not "nothing has happened".
Production holds **56 approved and 1 rejected** knowledge records. None is an
eligible calibration label:

- an APPROVED record is a *normative* acceptance, not a factual outcome. §21
  forbids training reliability on approvals as if "approved" meant "true", and
  `atlas_verify_outcome_eligible` admits only `DETERMINISTIC_VERIFIER`,
  `OPERATOR_RESOLUTION` and `RUNTIME_OBSERVATION`;
- there are **zero** RESOLVED records, so there are zero operator resolutions;
- no deterministic verifier had run before this season.

So the dataset is empty for a principled reason rather than an accidental one,
and manufacturing labels to fill it is exactly what §69 forbids.

## J. Confidence semantics

| value | type | on this machine |
|---|---|---|
| `confidence_score` | integer 0..100, from `atlas-reliability-v1` | present |
| `calibrated_probability` | a probability | **absent everywhere** |

The human renderer prints `100/100 (score, not a probability)` and, where a
probability is absent, says so explicitly rather than leaving a blank a reader
would fill in. The JSON key is `confidence_score`, never `confidence`.
`calibrated_probability` is omitted from JSON entirely when unavailable — not
null, not zero. A schema CHECK enforces the pairing independently and was
demonstrated refusing a probability written without calibration.

## K. Conflict model

Six kinds, with `SCOPE_MISMATCH` and `IMPLEMENTATION` kept distinct from
`CONTRADICTION`. **An implementation violation does not falsify the approved
record** — collapsing it into a contradiction would let a broken implementation
retract the design it violates. Stale evidence is weighted at a quarter and
never deleted: it loses current force and keeps its historical record.

## L. MCP proposal kind — verified already present

**§41's premise is stale.** A9.1 already shipped it. `schema_propose_decision`
in `src/mcp/mcp_tools.c` calls `prop_enum(j, "kind", KIND_HELP, KIND_ENUM, err)`,
the tool description states that `kind` defaults to DECISION and can never be
changed afterwards, and `tests/test_decision_kind.c` covers all eight kinds. All
model-originated proposals still land in PROPOSED; supplying `kind` grants no
lifecycle authority. No change was needed and none was made.

## M. MCP proposal revision — verified already present

**§42's premise is also stale.** `atlas_revise_decision` exists, is registered in
`TOOLS[]`, writes a PROPOSED revision by a MODEL_PROPOSAL actor, and carries
scope `ATLAS_SCOPE_MEMORY_WRITE`, which no remote credential can be granted. Its
description states that the kind cannot be changed by a revision. No change was
needed.

## N. Security

- No RPC method, MCP tool or gateway route reaches the verification engine.
  `method_for` returns NULL for both machine operations — an **absent** method,
  A7's pattern, not a refusing one — and `atlas_decision_op_is_machine` refuses
  the wire path independently.
- The warrant binds exactly as tightly as an operator challenge: one document,
  one revision, one target state, one content hash, single-use, consumed by an
  UPDATE naming the state it observed. Demonstrated: a spent warrant is refused,
  a fabricated warrant id is refused, an absent warrant is refused.
- A shadow row can never be spent (`verdict = 'AUTO'` is required).
- A self-declared or peer-authenticated actor cannot be TOOL, TEST,
  RUNTIME_OBSERVATION or ATLAS_VERIFIER — refused in C and by a schema CHECK,
  both demonstrated.
- Risk acceptance, normative claims on the deterministic path, rejection as a
  target, and state-machine-illegal transitions are refused **before** the policy
  file is read, so a mistaken root-owned rule is refused rather than obeyed. A
  policy naming one is malformed, not inert.
- A machine transition can never become a reliability label.

## O. Deterministic automatic lifecycle

**ENABLED on the installed system.**

| | |
|---|---|
| policy | `/etc/atlas/verification.conf`, root-owned, 0644 |
| policy id | `atlas-a92-obligation-remediation-v1` |
| policy hash | `b6e345f16e36addc6700d5fe7c29669d71e38c4708ded0b7154770bfe6fc850d` |
| rules | exactly one: `OBLIGATION APPROVED RESOLVED atlas.symbol_absent` |
| thresholds | `min_confidence = 100`, `min_evidence_groups = 2`, `max_evidence_age = 86400` |

The narrowest useful transition: it closes out work a person already approved,
establishes no new policy, and refuses over an incomplete index.

**Historical source calibration was NOT a prerequisite and is NOT listed as
one.** The live acceptance run recorded `calibration: INSUFFICIENT_DATA` on the
same result that transitioned. The calibration gate is guarded on the basis, so a
deterministic verdict cannot be blocked by a statistic about anybody's past — and
`test_a_deterministic_claim_is_verified_without_any_calibration` asserts that no
calibration reason was even recorded.

Test evidence: 16 engine tests and 27 model tests, plus the live run below.

## P. Empirical automatic lifecycle

**SHADOW.** `empirical_enforce = no`.

The machinery is complete and tested — scoring, dependency accounting, actor
storage, reliability storage, calibration state, insufficient-data behaviour —
and no unsafe lifecycle mutation can occur, because the calibration gate blocks
before enforcement is consulted. No metrics are reported because the dataset is
empty (§I); reporting precision or a Brier score over zero samples would be a
statistically meaningless number presented as a measurement.

## Q. Judgment lifecycle

Operator-only, always. There is no policy switch for it and none to add. Covered:
`ACCEPTED_RISK → APPROVED` refused with five agreeing agents; a normative
architecture claim refused with three supporting sources; a normative claim
refused even under a policy that names its transition and a verifier that passes.

## R. Historical replay

**Not run — no eligible dataset.** See §I. Building one would require
manufacturing labels, which §69 forbids. Baseline comparisons against majority
vote and raw source counting are therefore not reported as measurements; the
*behavioural* difference is demonstrated directly instead
(`test_confidence_does_not_grow_by_repetition`,
`test_three_models_reading_one_document_count_once`), which is the property the
comparison would have been evidence for.

## S. Test matrix

| build | result |
|---|---|
| Release | **73 / 73 passed** |
| ASan + LSan | **73 / 73 passed**, no leaks |
| UBSan | **73 / 73 passed**, 0 runtime errors |
| TSan | **73 / 73 passed**, 0 data races |
| CLI smoke | **47 checks, 0 failed** |

New: `test_verify_model` (27 tests, unit) and `test_verify_engine` (16 tests,
integration). No intermittent failures observed.

**One pre-existing defect was found and fixed.** UBSan reported `load of value
192, which is not a valid value for type '_Bool'` in `migrate.c`.
`tests/test_db.c` built two `atlas_migration` structs field by field and silently
stopped being complete when A9.1 added `foreign_keys_off`; the schema bump
changed the stack layout enough to surface it. Both sites now zero first.

## T. Backup and restore

Round-trip on a database holding live A9.2 state: 1 claim, 2 results, 2 audit
rows (one a consumed warrant), and a RESOLVED obligation.

- `backup create` → schema 15, 1,097,728 bytes
- `backup verify` → `verdict ok`, `usable`, `integrity ok`, `fk ok`, 1 revision
  rehashed, 0 corrupt
- rows deleted from the live index, then `backup restore --yes`
- after restore: claims 1, results 2, audit 2, status RESOLVED, warrant still
  `verdict=AUTO consumed=1 policy_id=atlas-a92-obligation-remediation-v1`

Production backup verified before deployment (§B) and remains the rollback
target.

## U. Installed-system acceptance

Run with `/usr/local/bin/atlas` (sha256 `0a4e1da4…0919f22`, phase A9.2) against
the installed root-owned policy, on a **temporary data directory** so that no
acceptance record reached the production index.

1. approved OBLIGATION, no semantic index → `atlas.symbol_absent` reports
   `UNAVAILABLE`, verdict `BLOCKED`, status stays APPROVED
2. semantic index built, COMPLETE, 1 unit, 0 failed/partial/unsupported
3. `verify show` → `VERIFIED · DETERMINISTIC · 100/100 · INSUFFICIENT_DATA ·
   AUTO`, and the status is **unchanged** afterwards (the read writes nothing)
4. `verify run` → `APPROVED → RESOLVED, recorded as VERIFICATION_POLICY`
5. ledger shows three distinct authorities on one record: `MODEL_PROPOSAL`
   (proposal), `LOCAL_OPERATOR_CONFIRMED` (approval), `VERIFICATION_POLICY`
   (resolution)
6. audit row reconstructs the decision: policy id, policy hash, algorithm,
   score, calibration, verifier, check result, evidence snapshot, binary id,
   `consumed = 1`
7. second `verify run` → `BLOCKED`, exactly one `VERIFICATION_POLICY` event,
   status unchanged

Services confirmed active on the installed binary; no service runs from a build
tree.

## V. Performance

Observations, not bounds.

| operation | observed |
|---|---|
| `verify show` (installed binary, cold process, 20 runs) | 23 ms per call |
| `verify policy` | 22 ms per call |

Both include process start and index open. A9.2 schema overhead on the
production index: **106,496 bytes** — 0.009 % of 1.16 GiB, with all ten tables
empty. Aggregation is bounded by `ATLAS_VERIFY_MAX_ATTESTATIONS` (512),
`ATLAS_VERIFY_MAX_DEP_EDGES` (4096) and `ATLAS_VERIFY_MAX_CLAIMS` (256); every
bound reports when reached.

## W. Non-interference

| | before | after |
|---|---|---|
| `/opt/dna` HEAD | `5e34e1f6…` | `5e34e1f6…` |
| `/opt/dna` tracked content | `8a834c32…37876` | `8a834c32…37876` |
| `/opt/swapper` HEAD | `48414bb3…` | `48414bb3…`, tree clean |
| production `verify_*` rows | — | **0 in all ten tables** |
| production `VERIFICATION_POLICY` events | — | **0** |
| registered repositories | 3 | 3, unchanged |

No repository was auto-registered, no production knowledge record was mutated, no
credential was created, and no stray daemon was left running.

**One disclosure.** During the initial source lock I ran `git write-tree` in
`/opt/dna` to capture a tree hash. It writes tree objects into DNA's object
store; it does not touch HEAD, the index or the working tree, and the tree it
named already existed. It was not repeated — subsequent checks used
`rev-parse` + `status --porcelain` + `ls-files -s | sha256sum`, which write
nothing.

## X. Product findings

1. **`CLAUDE.md` understated the new-command checklist, and a real deployment
   found it.** A command needs a **fifth** registration in `COMMANDS[]` inside
   `is_a_command`; without it every other wiring is correct and the binary
   answers `unknown command`. `atlas verify` shipped to the installed system in
   that state before it was caught. CLAUDE.md now says five and says to run the
   command once from the built binary.
2. **A struct field added to `atlas_migration` in A9.1 left two test
   construction sites incomplete**, undetected until UBSan under a changed stack
   layout. Fixed, and recorded in CLAUDE.md as a rule about field-by-field
   construction.
3. **The `_verify_` schema forced a real design decision the prompt did not
   anticipate**: a distinct machine actor needs a CHECK widened, which needs a
   table rebuild. It was affordable only because `decision_events` is a leaf.
   Had `decision_revisions.proposed_by` also needed widening, the migration would
   have required `foreign_keys_off` and migration 13's full row-preservation
   discipline — which is the argument for keeping a policy unable to author
   revisions.
4. **Recording a policy conclusion inside the evidence fold was a genuine
   defect**, caught by the acceptance fixtures: `aggregate_compute` noted
   `NOT_VERIFIED` for zero attestations, and because the verdict fold is
   deliberately monotonic a later mechanical PASS could not lift it. Every
   deterministic claim with no attestations was silently unactionable. The fold's
   one-way property is correct; mixing the two layers was the mistake.

## Y. Remaining limitations

These are genuine gaps, not deliberate security contracts.

1. **No model-facing attestation surface.** No MCP tool submits an attestation,
   and there are no RPC or gateway reads of the verification state. This belongs
   with the empirical path, which is shadow-only: a model's attestation can move
   an empirical score and an empirical score currently authorises nothing, while
   the enforcing deterministic path takes no model input at all. Adding it needs
   an RPC group (reads ordinary, `verify.run` in the operator-uid table beside
   `code.index`), a tool whose actor identity is assigned by the surface and
   never read from the request, and a stable actor key derived from
   provider/client/session so reliability can accumulate per source.
2. **`atlas verify` cannot reach the production index from the operator
   account.** Under A7.1 the index is `0700 atlasd`, and A9.2 added no RPC
   method, so the command must currently run as the service account. This is the
   same gap A8-CI closed for `code.index` by adding an operator-uid method, and
   it is the strongest argument for doing (1).
3. **Mission Control renders none of the new fields.** The JSON surface carries
   all of them; the page was not extended.
4. **No calibration, and therefore no replay, metrics or baseline comparison.**
   §I explains why the dataset is empty and why filling it would be wrong.
5. **Only four deterministic verifiers, all reads over Atlas' own index.**
   Anything requiring execution — a serializer fixture, a parser round-trip, a
   runtime probe — is empirical here and therefore shadow-only.
6. **`README.md` still says "Status: phase A6"** in a later section. Pre-existing
   and untouched by this season.

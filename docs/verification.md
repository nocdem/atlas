# Verification — claims, evidence and automatic lifecycle (A9.2)

A9.1 made a durable record say *what sort of knowledge it is* and *how far
through the approval workflow it got*. Neither axis says anything about whether
the record is **true**, and nothing in A0..A9.1 could: an approved INVARIANT and
an approved guess were the same row.

A9.2 adds the third axis and the machinery that keeps it honest.

```
kind    — what sort of knowledge is this?          A9.1, on the document
status  — how far through approval did it get?     A4, from the ledger
verify  — what evidence bears on whether it holds? A9.2, derived on read
```

**No code path derives any one of the three from another.** `INVARIANT ·
PROPOSED · VERIFIED` is legal and useful: Atlas mechanically established the
proposition and nobody has adopted it as project policy. So is `DECISION ·
APPROVED · INCONCLUSIVE`: somebody with authority chose a direction and the
evidence that it was carried out is incomplete.

---

## The central rule

> **Deterministic verification does not require historical calibration.**

If a proposition has a complete mechanical truth condition and Atlas evaluated
it, how often some model has been right in the past is not an input to the
answer, and must not be made into a precondition for acting on it.

This runs against the intuition that more caution is always safer. It is not
caution: blocking a proven fact on an unrelated statistic is a category error
that happens to look like conservatism. `atlas_verify_basis_requires_calibration`
is a function rather than a comment precisely so that the rule can be asserted
by a test and cannot be quietly reintroduced somewhere else.

The converse rule is equally load-bearing:

> **Reliability never substitutes for authority.**

A source that has been right about control flow a thousand times has thereby
gained no power to accept a privacy risk on the project's behalf.

---

## The five separations

### 1. An actor is not evidence

```
              old-plan.md
             /     |      \
          agent-1 agent-2 agent-3
```

Three attestations. **One** evidence root.

Counting them as three independent corroborations is the single most attractive
mistake available to a system like this, because it is what makes a confident
wrong answer cheap to manufacture. Within one independent group only the
strongest attestation counts, never the sum — so repetition contributes exactly
nothing, and forty duplicated attestations score the same as one.

### 2. Reliability is not authority

Reliability is a property of a source's past factual accuracy in a domain.
Authority is a property an operator confers. They are stored separately, they
are reported separately, and no quantity of the first produces the second.

### 3. A confidence score is not a probability

`confidence_score` is an integer 0..100 from a named, versioned, reproducible
aggregation over the evidence Atlas holds *now*. It becomes a probability only
after calibration this phase ships the machinery for and — on any machine where
A9.2 has just been installed — has no data for.

They are different fields with different printers. The human renderer prints
`94/100 (score, not a probability)` and never a percent sign;
`calibrated_probability` is emitted only when calibration supports one, and is
**absent** rather than null or zero otherwise. A schema CHECK enforces the same
pairing independently:

```sql
CHECK(calibrated_probability IS NULL OR calibration = 'CALIBRATED')
```

### 4. Descriptive truth is not normative adoption

Atlas can mechanically establish *"the serializer emits zero here"*. It cannot
thereby establish *"the protocol shall always emit zero here"* — that is a
choice somebody makes.

`atlas_verify_basis_may_verify_semantics` is where this is enforced, and
DETERMINISTIC + NORMATIVE is **false**. Allowing it would let any observation of
the current implementation become permanent policy, with an audit trail that
looks impeccable.

### 5. Deterministic verification does not wait for calibration

See above. This is the rule the phase is built around.

---

## Core concepts

| Concept | What it answers | Where it lives |
|---|---|---|
| **Claim** | one discrete proposition, bound to artifacts | `verify_claims` |
| **Attestation** | one actor's verdict on one claim at one moment | `verify_attestations` |
| **Actor** | who spoke, and how well Atlas knows it is who it says | `verify_actors` |
| **Evidence** | where a fact came from, in checkable form | `verify_evidence` |
| **Evidence dependency** | which evidence derives from which | `verify_evidence_deps` |
| **Verification result** | what an aggregation concluded, at a moment | `verify_results` |
| **Outcome** | resolved ground truth reliability may learn from | `verify_outcomes` |
| **Reliability** | actor × domain accuracy | `verify_reliability` |
| **Lifecycle audit** | why Atlas itself changed a state | `verify_lifecycle_audit` |

### Verification state

`UNVERIFIED` · `VERIFYING` · `SUPPORTED` · `VERIFIED` · `CONTRADICTED` ·
`INCONCLUSIVE` · `STALE`

**UNVERIFIED is zero**, which is what makes backward compatibility exact rather
than approximate: a record with no claims has no evidence, so every record
written before this phase reports UNVERIFIED without a migration touching a
single decision row.

`UNVERIFIED` and `INCONCLUSIVE` stay different answers on purpose. "Nobody
looked" and "we looked and the evidence does not decide" call for different
actions, and collapsing them loses the more useful half.

### Verification basis

| Basis | Means | Calibration |
|---|---|---|
| `DETERMINISTIC` | a reproducible mechanical verifier evaluated an explicitly bounded truth condition | **not required** |
| `EMPIRICAL` | the proposition depends materially on aggregating evidence of varying quality from sources of varying reliability | **required** |
| `JUDGMENT` | a normative choice rather than a discoverable fact | no automation at all |

`UNKNOWN` is zero and is **not writable**: a result that does not say how it was
reached is not a result.

### What DETERMINISTIC is allowed to mean

> Within the claim's declared scope, Atlas has a pass/fail procedure another run
> over the same artifacts reproduces.

**That is all.** It does not mean the proposition is proven for every
implementation for all time. A unit test establishing `f rejects y at commit z`
establishes exactly that; a claim whose text says "the parser is safe" is not the
claim the verifier checked. Every verifier writes the scope it actually
established into its result, and a claim that outruns its verifier is a claim
with an unverified remainder.

This is A8-CI's rule about PROVEN, one layer up: no semantic inflation.

---

## The deterministic verifiers

Four ship in V1, and **every one is a read**.

| Verifier | Establishes |
|---|---|
| `atlas.content_hash` | the content of a repository path at a bound commit hashes to a stated value |
| `atlas.symbol_present` | a named symbol exists in the current semantic generation |
| `atlas.symbol_absent` | no symbol of a given name exists **in a complete generation** |
| `atlas.proven_edge` | the compiler proved a direct call edge between two named symbols |

None creates a process, runs a repository's build, executes a command, or opens
a file the repository controls. That is a **deliberate V1 restriction, not an
unfinished one**: a verifier that ran a command named in configuration would be
a code-execution path with an audit trail attached. The argument for adding one
belongs to whoever needs it, in writing, with the sandbox already built —
A8-CI's bounded-child pattern (`atlas_proc_run`, an explicit argv vector, an
empty environment, `RLIMIT_AS`, a wall clock and an idle bound) is what it would
have to reuse.

The practical consequence, stated plainly: Atlas can mechanically establish facts
about **what is recorded in its own index** and cannot mechanically establish
facts that require running the software. The second kind is empirical here, and
empirical is in shadow.

### UNAVAILABLE is not FAIL

The most dangerous confusion available to this layer. An index that has not run
cannot establish that a symbol is absent; reporting "could not look" as "it is
not there" is how a remediation detector closes an obligation that is still
outstanding.

`atlas.symbol_absent` therefore requires a **complete** generation — no failed,
partial or unsupported translation units. `atlas.symbol_present` does not, and
the asymmetry is the point: finding it is finding it, and an incomplete index
cannot conjure a symbol that is not there.

---

## Independence

Independence is computed by **union-find over declared derivation edges**, with
three conservative closures:

1. evidence connected by any derivation edge is one group;
2. attestations that share a piece of evidence are one group;
3. **INTERPRETATION-family evidence declaring no roots joins one shared group** —
   not one group each.

Rule 3 is what makes three models reading one document count once even when none
of them declared the document. It is also what defeats an orchestrator's fleet of
subagents: the two are the same failure and get the same answer, because Atlas
could not demonstrate independence and therefore does not assume it.

> **Independence is never assumed.** If it cannot be demonstrated from declared
> structure, evidence is treated as correlated — which costs confidence and never
> manufactures it.

### Evidence families (version 1)

| Family | Classes | Can be a root |
|---|---|---|
| `STATIC_ARTIFACT` | source, compiler, git history, specification, document | yes |
| `DYNAMIC_OBSERVATION` | test, runtime, deployed config | yes |
| `INTERPRETATION` | AI analysis, human statement, existing Atlas knowledge | **no** |

An interpretation is by definition *of* something, so one that declares no source
is an undeclared derivation rather than a fresh observation of the world.

`ATLAS_VERIFY_FAMILY_VERSION` is stored on every result, so an old verdict is
never silently reinterpreted under a new taxonomy.

---

## The aggregation algorithm

`atlas-reliability-v1`. Integer throughout — there is no floating point anywhere
in `src/verify/verify.c` — so "the same inputs produce the same result" is a
property of the code rather than a hope about rounding. A machine transition has
to be reproducible from its audit row years later on a different machine.

1. every attestation gets a weight from actor class, identity authenticity,
   measured reliability (or the documented prior), freshness and scope match;
2. attestations are partitioned into independent groups;
3. **within a group only the single strongest weight counts**, for support and
   contradiction separately;
4. `score = 100 × support / (support + contradiction + PRIOR_MASS)`.

`PRIOR_MASS` is the weight of *not knowing*. It never goes away, so 100 is
unreachable by accumulation and a lone source — however good — lands near 80
rather than at certainty. Integer division truncates, which biases every score
downward: the correct direction for a system that prefers abstention to
unjustified confidence.

**What it deliberately does not do:** majority vote, arithmetic means of
self-reported model confidence, or treating differently-named sources as
independent.

### Self-reported confidence

Stored as `self_confidence`, reported, and **never** used directly as Atlas'
confidence. A source's opinion of itself is data about the source. It alone can
never satisfy auto-finalization.

### Conservative priors (version 1)

| Actor class | Atlas-attested | Peer-authenticated | Self-declared |
|---|---|---|---|
| `ATLAS_VERIFIER` | 900 | — | — |
| `TOOL`, `TEST` | 700 | — | — |
| `RUNTIME_OBSERVATION`, `REPOSITORY_EVIDENCE` | 650 | — | — |
| `HUMAN` | 500 | 500 | 350 |
| `DOCUMENT` | 400 | 400 | 350 |
| `AI_AGENT` | — | 350 | 350 |
| `UNKNOWN` | 100 | 100 | 100 |

Three things about this table's shape are deliberate:

- **A human is not at the top.** These weights concern claims of *fact*, and on
  questions of fact a person's recollection of what a system does is ordinary
  evidence. Human authority is real and lives entirely elsewhere — in the
  operator channel, which no weight here substitutes for.
- **Identity dominates class.** A self-declared actor is capped at 350 whatever
  it says it is, because the difference between "Atlas ran the compiler" and
  "something told Atlas the compiler said so" is the largest real difference
  available, and the one an attacker controls.
- **Nothing reaches full scale.** The maximum is 900 of 1000, so certainty has to
  be assembled out of independent groups.

Once a source has resolved outcomes, its measured reliability replaces the prior.
These are starting points, not settings.

---

## Actor identity and forgery

| Identity | Means |
|---|---|
| `SELF_DECLARED` | the submitter said so — a claim about itself, which is not evidence about itself |
| `PEER_AUTHENTICATED` | the kernel or a credential established it |
| `ATLAS_ATTESTED` | Atlas created this actor because Atlas performed the act |

`TOOL`, `TEST`, `RUNTIME_OBSERVATION` and `ATLAS_VERIFIER` may **only** exist
with `ATLAS_ATTESTED` identity. An AI saying *"clang proves this"* is not
equivalent to Atlas running clang, and the attempt is refused at the write point
— by C for the message and by a schema CHECK for the guarantee:

```sql
CHECK(class NOT IN ('TOOL','TEST','RUNTIME_OBSERVATION','ATLAS_VERIFIER')
      OR identity = 'ATLAS_ATTESTED')
```

Refused rather than accepted-and-discounted, deliberately. A discounted forgery
still appears in an evidence list, still reads as tool output to somebody
skimming a UI, and still has to be argued away by whoever finds it.

---

## Machine verification authority

A new lifecycle actor: **`VERIFICATION_POLICY`**.

It sits beside `ATLAS_AUTOMATIC` and is deliberately not the same thing:

| Actor | Means |
|---|---|
| `ATLAS_AUTOMATIC` | a transition that follows mechanically from another Atlas operation — the supersession an approval implies. No policy involved. |
| `VERIFICATION_POLICY` | a root-owned policy authorised this transition, justified by a verification result, spending a single-use warrant |

Collapsing them would make *"which lifecycle changes did Atlas make on its own
authority?"* unanswerable by reading the ledger, which is the first question an
auditor of an automating system asks.

### The honesty limits

Read the name as literally as `LOCAL_OPERATOR_CONFIRMED` must be read.

> `VERIFICATION_POLICY` says a policy Atlas could not itself edit named this
> exact transition, and that the gates that policy set were met. It does **not**
> say the record is true, does **not** say a person agreed, and confers nothing
> beyond the one transition the warrant named.

The limits are the mirror image of the operator channel's. There, Atlas cannot
prove a person acted. Here, Atlas can prove precisely what acted — a named policy
at a recorded hash, over a recorded verification result — and cannot prove that
the policy was *wise*. An operator who writes a rule authorising too much has
authorised too much, and every transition that follows will be correctly recorded
as policy-authorised.

`VERIFICATION_POLICY` is not writable by any adapter, exactly as
`LOCAL_OPERATOR_CONFIRMED` is not.

### The warrant

The audit row **is** the capability. A row with `verdict = 'AUTO'` and
`consumed = 0` authorises exactly one transition of exactly one revision at
exactly one content hash.

That is deliberately the shape `decision_challenges` has, because **the machine
path must bind no more loosely than the operator path**. Compare `op_auto` in
`lifecycle.c` with `spend_challenge` line by line: both bind to one document, one
revision and one content hash; both rehash the stored content and refuse a
mismatch; both consume with an UPDATE that names the state it observed, so a
replay loses deterministically. What differs is *who can mint one* — an operator
at a terminal, or the engine under a policy neither Atlas nor any model can edit.

If the machine path bound more loosely, every gate in front of it would be arguing
about which evidence justifies a capability that is easier to satisfy than the one
a person needs.

### The gate order

Refusals no policy can lift come **first**, before the policy file is read:

1. a `JUDGMENT` basis;
2. an `ACCEPTED_RISK` approval;
3. a normative claim reached deterministically;
4. a transition the kind-aware state machine refuses.

Two reasons for the ordering. An operator who wrote a rule authorising one of
these has made a mistake Atlas should refuse rather than obey — "root wrote it"
establishes that the instruction is authentic, not that it is one Atlas should
carry out. And a `FORBIDDEN` answer should never depend on configuration, because
then it would read as something more evidence could change.

Then: the policy, the evidence thresholds, calibration (**empirical only**), and
finally enforcement.

---

## The three paths

### Path A — deterministic

Calibration is **not** required. A transition may occur when the basis is
`DETERMINISTIC`, the truth condition is mechanically established, a root-owned
rule names the kind and transition and verifier, evidence is current, no blocking
contradiction exists, and the state-machine transition is legal.

Historical source reliability does not block this path.

### Path B — empirical

Calibration **is** required. Where calibration is insufficient, Atlas still
computes the score, the supporting and contradicting evidence and the
independence count, and records a shadow verdict — but performs no transition.

### Path C — judgment

Reliability cannot authorise the decision. Atlas may verify the *premises* of a
judgment to any strength and still have established nothing about the judgment.

**Risk acceptance in particular:** Atlas may verify that a risk exists, and that
it has been mitigated. It must never infer that the risk is *accepted*, from high
confidence, from multiple agreeing sources, or from any source's reliability.

---

## Shadow mode is per path, not global

```
DETERMINISTIC:  enforce = yes
EMPIRICAL:      shadow  (calibration unavailable)
JUDGMENT:       operator only, always
```

There is no judgment switch, and its absence is the design.

A shadow verdict writes a full audit row recording exactly what Atlas would have
done. `atlas_db_verify_warrant_check` requires `verdict = 'AUTO'`, so a shadow
row **can never be spent**: shadow mode is a complete result that is structurally
incapable of becoming an action.

---

## What is absent, and why

| Absent | Why |
|---|---|
| `AUTO_REJECT` | §34: automatic rejection needs an explicit falsification condition, and low confidence is not one — it usually means missing evidence, an index that has not run, or a scope mismatch. Rejecting a legitimate proposal is not the mirror of failing to approve one: the first destroys work and is terminal, the second costs a wait. |
| `AUTO_SUPERSEDE` | Supersession must establish that two records concern the same subject at compatible scope. Atlas has no mechanical test for that; a newer record existing is not one and a timestamp emphatically is not. Deciding wrongly would retire a live decision in favour of an unrelated one, with an impeccable audit trail. |
| Approving over an existing approved revision | That is a supersession. Refused by the engine and again by the write point. |
| A `REJECTED` target in policy syntax | Not expressible in a rule at all, and refused independently. |

All **absent rather than refused** — the house pattern, because an absent path
cannot be weakened by a later edit the way a refusing one can.

---

## The root-owned policy

`/etc/atlas/verification.conf`, root-owned, reached through
`atlas_rootpath_open` from `/` with no symlink traversed.
`ATLAS_VERIFYPOLICY_PATH` is a **compiled-in constant** — no environment
override, no flag, no data-directory-relative variant. A caller that can choose
the policy is not constrained by it.

```conf
enabled = yes
policy_id = release-gate-v1

# Per path, never one flag. This is the structural expression of the central
# rule: deterministic transitions may enforce on a machine with no calibration
# data whatsoever, while the empirical path stays in shadow.
deterministic_enforce = yes
empirical_enforce = no

min_confidence = 100
min_evidence_groups = 1
max_evidence_age = 86400
min_calibration_samples = 100

# allow = KIND FROM TO VERIFIER   (four tokens, exactly)
allow = OBLIGATION APPROVED RESOLVED atlas.symbol_absent
```

- **Fail-closed at zero.** A zeroed struct automates nothing; thresholds default
  to their most demanding values, so an absent key never loosens anything.
- **An unrecognised key is an error**, not something skipped. The thing an author
  most plausibly believes they configured, and Atlas most plausibly never read,
  is a restriction.
- **A rule with a missing verifier is refused**, never defaulted: a deterministic
  rule with no named verifier is a rule any evidence at all satisfies.
- **A rule naming a forbidden transition is malformed**, not inert. An inert rule
  reads, to whoever wrote it, exactly like one that works.
- A rule cannot name a document, an actor, a model or a repository. A policy that
  could bless one specific record would launder a single approval through a file;
  one that could bless a specific *source* would be reliability written down as
  authority.

---

## Circular ground truth

The forbidden loop is short and easy to build by accident:

```
a model supports a claim
  → the aggregate likes it
    → Atlas transitions the record
      → the transition is counted as ground truth
        → the model's reliability rises
          → the next claim clears the bar more easily
```

Every step looks reasonable and the result is a system that has taught itself to
trust a source using that source's own output.

Ground truth may therefore come only from resolution classes that do not depend
on the aggregation:

| Source | Eligible |
|---|---|
| `DETERMINISTIC_VERIFIER` | yes |
| `OPERATOR_RESOLUTION` | yes |
| `RUNTIME_OBSERVATION` | yes |
| `MACHINE_TRANSITION` | **no** |
| `UNKNOWN` | no |

An ineligible outcome is **stored and not counted**, so the ineligible case is
auditable rather than absent. The loop is broken structurally, not by a rule
somebody has to remember.

---

## Conflicts

| Kind | Means |
|---|---|
| `CONTRADICTION` | same subject, same scope, same binding, incompatible — the only genuine contradiction |
| `SUPERSESSION` | both were true, at different times; the older is history and stays |
| `SCOPE_MISMATCH` | both are true, of different things — a compiled default of `false` and a deployment override of `true` are two correct claims at two scopes |
| `IMPLEMENTATION` | approved knowledge says one thing and the implementation does another |
| `STALE_EVIDENCE` | one side's evidence no longer describes the present |
| `COMPETING_NORMATIVE` | two normative alternatives; nothing observable decides between them |

**`IMPLEMENTATION` does not falsify the approved record.** It is a finding
against the implementation, and under policy it opens an obligation. Collapsing
it into `CONTRADICTION` would let a broken implementation retract the design it
violates, which is exactly backwards.

## Freshness

Stale evidence loses **current force** and keeps its **historical record**. It is
weighted at a quarter, never deleted: a July runtime observation should not decide
an August question, and it is still true that it was observed in July.

---

## Retention

All ten A9.2 tables are `CANONICAL` and **none is prunable**. Two arguments:

1. none of it is rebuildable — the repository remembers its own bytes, but it
   does not remember that anybody spoke about them;
2. these tables are the input to a *count*. A half-aged evidence table is not a
   smaller evidence table, it is a **wrong** one, and nothing in it records that
   rows are missing — so every confidence score computed afterwards would be
   confidently wrong. That is the failure this phase exists to prevent, so the
   tables it stores its answers in must not be able to cause it.

---

## Migrations

| # | What | Rebuilds | FKs off |
|---|---|---|---|
| 14 | ten new tables | nothing | no |
| 15 | widens `decision_events.actor` for `VERIFICATION_POLICY` | `decision_events` | no |

Migration 14 is **purely additive**, so no decision row is written, no content
hash moves, and `atlas doctor` has nothing new to report.

Migration 15 rebuilds one table to widen a CHECK. It needs no `foreign_keys_off`
— unlike migration 13 — because `decision_events` is a **leaf**: nothing in the
schema references it, so dropping it cascades nowhere. It verifies its own row
preservation before committing, with a named CHECK
(`no_decision_event_may_be_lost_in_migration_15`) as the error message.

`decision_revisions.proposed_by` keeps its four-value CHECK **unchanged**. A
verification policy never proposes anything: it can move a record that already
exists between states its own state machine permits, and nothing more. A policy
able to author a revision would be a policy able to write project knowledge,
which is a far larger authority than this phase grants — and the narrower CHECK
is what makes that structural.

---

## Extending A9.2 safely

- **A new verification state** means a member, a row in `STATE_NAMES`, the CHECK
  on `verify_results.state`, and the enumerated expectations in
  `tests/test_verify_model.c`. Keep `UNVERIFIED` at zero.
- **A new basis** means editing `atlas_verify_basis`,
  `atlas_verify_basis_writable`, `atlas_verify_basis_requires_calibration`,
  `atlas_verify_basis_may_verify_semantics`, the CHECK on `verify_results.basis`,
  and the enumerated tests. Deciding whether it requires calibration is the whole
  point of adding it.
- **A new evidence class** means a member, a name, a row in
  `atlas_verify_evidence_family_of`, the CHECK on `verify_evidence.class`, and a
  decision about whether it may be a root. A class with no family cannot be
  written.
- **A new deterministic verifier** means a member, a row in `VERIFIERS[]` with a
  written description, a case in `atlas_verify_run_verifier`, and — the part that
  is not optional — a written argument that it is a **read**. A verifier that
  creates a process needs the sandbox first.
- **A new reason** means a member and a row in `REASONS[]` carrying its name, the
  verdict it implies **on its own**, and one written sentence of meaning. The
  verdict follows from the reason rather than being chosen beside it.
- **A new policy key** means a branch in `atlas_verifypolicy_parse_buffer`, a
  field, a documented line in the template, and a case in the malformed matrix.
  An unknown key stays an error, and a ceiling may only lower a compiled-in bound.
- **A new bound** goes in `include/atlas/limits.h` with a written reason and is
  reported when reached.

---

## Status

**Implemented and tested:** the schema, the vocabularies, the independence
engine, the aggregation algorithm, the four deterministic verifiers, the
root-owned policy, the automatic lifecycle engine with its warrant and audit,
the `atlas verify` command on both renderers, and the acceptance fixtures.

**Deliberately absent:** `AUTO_REJECT`, `AUTO_SUPERSEDE`, command-running
verifiers, and any way for a model to mint or spend a lifecycle capability.

**Deferred, with reasons:**

- **Historical replay and calibration metrics.** There is no dataset — see below.

**Calibration on this machine: `INSUFFICIENT_DATA`, for every actor and domain.**
The production index holds zero approved, rejected, superseded or resolved
records, so there is no resolved history to learn from. That is reported plainly
rather than papered over, and it is **not** a blocker for the deterministic path
— which is the whole point of the phase.

---

# A9.2.1 — the product wiring

A9.2 built the engine and shipped no way to put evidence in. The three insert
functions had no caller outside the tests, so on a real deployment all ten
verification tables stayed empty while every test that read them passed. A9.2.1
is the missing half: the surfaces a model and an operator actually reach.

Nothing about the engine changed. What changed is that it can now be fed, and
that everything it refuses, it refuses **at the boundary a caller meets**.

## The surfaces

| Operation | MCP tool | CLI | RPC method | Gateway |
|---|---|---|---|---|
| state a claim | `atlas_verify_claim_create` | `verify claim` | `verify.claim_create` | — |
| reference evidence | `atlas_verify_evidence` | `verify evidence` | `verify.evidence_add` | — |
| have Atlas check it | `atlas_verify_produce` | `verify produce` | `verify.evidence_produce` | — |
| attest | `atlas_verify_attest` | `verify attest` | `verify.attestation_add` | — |
| declare a derivation | `atlas_verify_depend` | `verify depend` | `verify.dependency_add` | — |
| evaluate | `atlas_verify_evaluate` | `verify evaluate` | `verify.evaluate` | — |
| read one claim | `atlas_verify_show` | `verify show` | `verify.show` | `GET /api/v1/verify/claim` |
| list claims | `atlas_verify_claims` | — | `verify.claims` | `GET /api/v1/verify/claims` |
| read the policy | — | `verify policy` | `verify.policy` | `GET /api/v1/verify/policy` |
| evaluate and enforce | — | `verify run` | *(operator group)* | — |

Nine RPC methods, eight MCP tools, three gateway routes — **all three of them
reads**.

**Intake is deliberately absent from the gateway.** A9's rule is that a mutating
route needs a write scope no A9 credential can hold, "which is the argument it
has to survive". Intake has not survived it: `verify.evaluate` can cause Atlas to
move a lifecycle state, and a leaked bearer token must not reach the one path
that transitions a record without a person. A local model reaches intake through
MCP over a Unix socket, where the peer uid is a kernel fact. That is a different
trust position, and it is the one intake requires.

## Actor identity: the channel is derived, never asserted

The three facts that decide what an attestation is *worth* — the actor's class,
its identity, and whether evidence was produced by something Atlas ran — are
never read from a request. They come from `atlas_verify_channel`, which the
transport edge sets.

| Channel | Actor class | Identity | Set by |
|---|---|---|---|
| `MODEL` | `AI_AGENT` | `SELF_DECLARED` | an MCP tool, or an ordinary RPC peer |
| `OPERATOR` | `HUMAN` | `PEER_AUTHENTICATED` | a peer whose uid the root-owned policy names |
| `ATLAS` | `ATLAS_VERIFIER` etc. | `ATLAS_ATTESTED` | Atlas' own code, having performed the act |

`ATLAS` is unreachable from every transport: no request parser sets it and
`atlas_verify_channel_parse` does not accept its name.

**The uid is a ceiling, not the answer.** Deriving the channel from `SO_PEERCRED`
alone was right about forgery and wrong about the ordinary case. A7.1 permits a
person to run a model from their own account, and on an unseparated machine
there is no other account to run it from — so a local MCP session speaks from the
operator uid, and every attestation a model made was stored as a `HUMAN` actor
with `PEER_AUTHENTICATED` identity. Atlas was minting the forged-human rows this
season exists to refuse.

A request may therefore name its channel, and the name is honoured **only when it
asserts less** than the uid would. The two failure directions are closed by
different mechanisms: a caller cannot become Atlas by naming it (the parser
refuses the name), and cannot become the operator by out-ranking the kernel (the
rank comparison refuses every raise). Claiming less authority than you hold is
never a forgery — it is the accurate statement, and the only one a model is
entitled to make.

Everything else a speaker says about itself — name, provider, family, version,
role, run, orchestrator — is **asserted metadata**, stored as asserted. The
transport carries no cryptographic statement about which model is speaking, and
the row's `identity` column is what says so. Never describe it as authenticated.

## Referenced evidence versus produced evidence

This is the distinction the whole intake path exists to keep.

**A model may point Atlas at evidence.** `atlas_verify_evidence` records what the
caller read: a file at a commit, a symbol, a specification, a document, or its
own analysis of them. That is a claim about what it looked at.

**A model may not have produced evidence.** `COMPILER`, `TEST`, `RUNTIME` and
`DEPLOYED_CONFIG` derive their entire weight from Atlas having *performed* the
act. Naming one on the reference path is **refused, not discounted** — a
discounted forgery still appears in the evidence list, still reads as tool output
to somebody skimming a UI, and still has to be argued away by whoever finds it.

**The honest route is `atlas_verify_produce`**: Atlas runs a named allowlisted
verifier and records what it found. The caller chooses which verifier applies; it
does not choose the verdict, and there is no parameter that could carry one. An
index that has not run answers `UNAVAILABLE`, which is not `FAIL`.

## Reading the evidence back

Every surface that reports a confidence score reports the evidence behind it. A
score whose evidence a reader cannot see is a number taken on trust, which is the
opposite of what recording evidence is for.

Two pairs of fields are kept separate on purpose, and a renderer that collapses
either is wrong:

- **`class` and `producer_identity`.** `AI_ANALYSIS` produced by a
  `SELF_DECLARED` actor and `COMPILER` evidence that is `ATLAS_ATTESTED` are both
  legitimate rows meaning entirely different things. A view that prints the first
  without the second is telling somebody a model is a compiler.
- **`actor` and `group`.** Attestations sharing a group rest on the same evidence
  and corroborate one another not at all. Printing an actor count as though it
  were an evidence count is the error this season exists to prevent.

The detail is display only. Nothing loaded for it reaches the aggregation, so a
wrong value there misleads a reader without moving a verdict — and
`atlas_verify_assess` still writes nothing at all.

## Confidence is still not a probability

`confidence_score` is an integer out of 100 and carries no percent sign in any
renderer. `calibrated_probability` is emitted **only** when calibration supports
it, and is absent rather than null or zero otherwise — zero is a probability.
Mission Control says so in words beside the number, because a figure next to the
word "confidence" is read as a percentage by everybody who has not been told
otherwise.

Calibration on this machine remains `INSUFFICIENT_DATA` for every actor and
domain, so the empirical path stays in shadow. The deterministic path does not
wait on it, and that separation is the point of the phase.

## What is still absent

- **No MCP tool, CLI verb, RPC method or gateway route approves, rejects,
  supersedes, resolves or revalidates anything.** Absent rather than refusing,
  which is A7's pattern: an absent name is answered by the dispatcher's
  unknown-name case, and a refusing one is a refusal a later edit can weaken.
- **Nothing mints or spends a warrant**, edits the verification policy, lowers a
  threshold, or states a verifier's verdict.
- **No remote credential administration**, unchanged from A9.
- **Historical replay and calibration metrics.** There is still no dataset.

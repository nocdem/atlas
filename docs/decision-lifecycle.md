# Decision documents, immutable revisions and operator approval

A4 is the phase in which a proposal can become project policy. The whole
difficulty of the phase is in what that sentence is allowed to mean, so this
document leads with the limits rather than ending with them.

**A9.1 added a second, orthogonal dimension: the knowledge kind.** Everything
below about approval, immutability, the ledger and the operator channel is
unchanged by it. What changed is that a durable record now says *what sort of
knowledge it is* as well as *how far through the approval workflow it got*, and
the two are separate fields that no code path derives from each other. See
[kind and status](#a91-kind-and-status) — read that section before the state
machine if you are new to the model, because "decision" now names one kind among
eight rather than the whole category.

## The claim, and the non-claim

When Atlas reports a decision as `APPROVED`, it means exactly this:

> An explicit action arrived through Atlas' local operator channel — a
> controlling terminal, a short-lived single-use capability bound to one
> repository, one document, one revision and that revision's content hash, and
> a confirmation typed against that hash.

It does **not** mean, and Atlas never says, any of the following:

- that a particular person acted;
- that any person acted at all;
- that the action is non-repudiable, signed, or cryptographically attested;
- that the approved text is safe to follow as an instruction.

Any process running as the same local user can allocate a pseudo-terminal, run
`atlas decision approve` against it and type the confirmation — that is, a
same-UID process **may imitate** the operator channel. **That includes an AI
agent with shell access.** `tests/test_decision_operator.c` does precisely
that, on purpose: a test suite that could not demonstrate this would be a suite
whose subject was making a stronger claim than the code supports.

**A7 acted on that sentence rather than only publishing it.** A terminal is no
longer the gate, because nothing observable from inside a process distinguishes
a person from a program running as the same uid. Approving, rejecting,
superseding and revalidating now require *operator authority*: a root-anchored
policy naming an `operator_uid`, matched against the caller, with the `atlas`
executable itself owned by root and not writable by anyone else. Where those do
not hold, the profile is **locked** and the four verbs are refused before a
terminal is opened, before a capability is minted and before a prompt is
printed. `atlas doctor` reports the profile state and the reason on every run.

A locked profile does not stop a same-uid process from writing
`state='APPROVED'` into SQLite by hand — nothing in userspace could. What it
stops is Atlas *manufacturing a coherent record* for it: a forged row no longer
agrees with the append-only ledger, so `atlas_db_decision_verify` reports the
disagreement and `atlas doctor` surfaces it. An undetectable forgery becomes a
detectable one, and that is the whole of the claim. See
`docs/security/A7_SECURITY_REVIEW.md` (ATLAS-A7-001, ATLAS-A7-004) for the
reproduction, the scope argument, and the exact deployment that lifts the lock.

So the claim Atlas makes is narrow and true: **Atlas hands a model no capability
that approves a decision.** It deliberately does not make the broader claim that
sounds similar — that a model is unable to approve one — because that would be a
security guarantee about the whole machine, and Atlas can only speak for its own
surface. An agent that can run arbitrary local commands is outside that surface.

The actor recorded is therefore `LOCAL_OPERATOR_CONFIRMED`, which names a
channel. A2's `USER_APPROVED_DECISION` remains in the provenance vocabulary and
remains **unwritten**, because it names a person and Atlas cannot establish one.

### What the channel does exclude

The exclusions are real, checkable, and are the whole of what the mechanism
buys. An approval cannot be produced by:

- a model's text, in any field, at any length;
- a hook payload;
- an MCP tool call — there is no approval tool, and no tool schema declares a
  `token` or a `confirmation` argument;
- a repository file;
- an environment variable;
- `--yes`, which is refused explicitly rather than ignored;
- piped standard input, or standard input of any kind: the confirmation is read
  from `/dev/tty`;
- redirected standard output: the prompt must be visible, so both ends are
  required to be terminals;
- a replayed request: a capability is single-use and expires in two minutes;
- a capability for a different document, a different revision, a different
  repository or a different intent.

There are no signing keys, no hardware-token support and no general security
subsystem in A4. Adding the vocabulary without the mechanism would be worse than
the current claim.

## Proposal versus approval

| | proposal | approval |
| --- | --- | --- |
| who may create it | any adapter: MCP, a hook, the CLI | the operator channel only |
| what it records | that something was written down | that it became accepted policy |
| actor | `MODEL_PROPOSAL` or `MODEL_INFERENCE` | `LOCAL_OPERATOR_CONFIRMED` |
| effect on the text | none | none |
| reachable from a model | yes | no |

The last row is the one that matters most and is the easiest to lose. **Approval
changes a record's status. It does not change the nature of its bytes.** An
approved decision is accepted project policy expressed in prose that somebody —
often a model — wrote. It is still `UNTRUSTED_DATA` wherever it is reported, it
never enters automatic model context, and a body shaped like an instruction is
text in a database rather than a directive.

Conflating those two would turn an approval button into a prompt-injection
channel: propose a document containing instructions, get it approved on the
strength of a plausible title, and have it treated as authoritative thereafter.

## A9.1: kind and status

Two dimensions, and keeping them apart is the whole of what this season is
about.

**Status** answers *how far through the approval workflow is this record?* It is
`atlas_decision_state`: PROPOSED, APPROVED, REJECTED, SUPERSEDED and — new in
A9.1 — RESOLVED.

**Kind** answers *what sort of durable engineering knowledge is this?* It is
`atlas_decision_kind`, eight values, and it has almost no effect on anything
Atlas does. It is a label a reader acts on.

An **APPROVED INVARIANT**, an **APPROVED ACCEPTED_RISK** and an **APPROVED
DECISION** are one status and three kinds. Every surface reports both, in
separate fields, and none of them folds one into the other.

### Why the dimension exists

A4 had one semantic category and called it a decision, so every durable fact an
operator wanted to keep had to be dressed as a choice between alternatives. A
real consolidation exercise on an indexed repository broke that: a consensus
constant implementations must preserve is not a choice, a release rule is not an
architecture, a currently deployed chain id is not permanent, and an approach
that was built and abandoned is valuable precisely because it is *not* current
direction. Recording all of them as decisions did not lose the prose. It lost the
reason a later reader should treat them differently.

### The kinds

| kind | means | resolvable |
| --- | --- | --- |
| `DECISION` | a choice between alternatives that establishes project direction or architecture | no |
| `POLICY` | a rule governing development, release, operation or process | no |
| `INVARIANT` | a technical property implementations must preserve | no |
| `OPERATIONAL_FACT` | a mutable, environment-specific fact about what is currently deployed or relevant | no |
| `ACCEPTED_RISK` | a security, privacy, reliability or operational risk that has been explicitly accepted | **yes** |
| `OBLIGATION` | required future work: a remediation, a blocker, a release gate | **yes** |
| `PARKED` | work or architecture intentionally deferred and not currently active | no |
| `REJECTED_ALTERNATIVE` | an approach considered or built and deliberately rejected, recorded with why | no |

`DECISION` is **zero**, deliberately, and it is the one Atlas vocabulary whose
zero is not "unknown". A zeroed struct, an omitted argument and an absent column
all mean DECISION, because every record written before this vocabulary existed
*was* a decision. There is no such thing as a knowledge record whose kind Atlas
does not know.

### Semantics worth stating precisely

**OPERATIONAL_FACT carries the least permanence of any kind, and that is a
reporting obligation rather than a storage difference.** The record is exactly as
durable as any other; what it *asserts* is only about now. Replacing one is the
ordinary supersede path, which is why the kind needs no special machinery. Never
present one with the permanence of an architectural decision.

**Discovering a risk does not accept it.** A PROPOSED `ACCEPTED_RISK` is a risk
somebody wrote down and nobody accepted. Acceptance is the ordinary approval,
through the operator channel, and there is no path by which recording a risk
approves it. The kind name describes what an *approved* one means; the status is
what says whether it has been.

**PARKED is not REJECTED.** An approved PARKED record is an accepted statement
that something is deliberately not being done now — a more useful fact than
silence, and a different one from a refusal.

**REJECTED_ALTERNATIVE is where conflating the two dimensions is easiest, so read
them separately.** An **APPROVED** REJECTED_ALTERNATIVE means "it is accepted
knowledge that we rejected this approach" — the normal, useful state, and the one
that stops a later agent rediscovering it. A **REJECTED** REJECTED_ALTERNATIVE
means the record itself was refused: somebody wrote down that an approach was
rejected and that claim was not accepted. Both are expressible and they mean
different things.

### A kind never changes

The kind lives on the **document**, is written by the INSERT that creates it, and
no `UPDATE` in `db_decision.c` names the column — the same guarantee a revision's
prose has. A revision that asserts a different kind is refused, naming the
remedy: propose a record of the right kind and supersede the old one with it.

That is not a limitation dressed as a principle. Reclassifying by supersession
keeps the record of how the knowledge *used to be* classified, which is exactly
what a durable record is for; rewriting the label in place would erase it.

**The kind is therefore not part of the canonical content hash**, and that needs
saying explicitly because everything else immutable about a revision is hashed.
Two reasons, both load-bearing:

1. Hashing it would move every digest that has already been approved. `atlas
   doctor` rehashes every revision and reports a mismatch as tampering — which
   is correct, and which would then fire on every healthy record in every
   existing database.
2. The kind is identity-like rather than content: it is fixed before revision 1
   exists and cannot change under an approval. An approval binds document uid +
   revision number + content hash, and the document's kind is immutable, so the
   approval covers it by construction. The document's `uid` is outside the hash
   for the same reason.

### The surfaces

Every surface names the two dimensions identically, so a client written against
one reads the others:

| surface | kind | status |
| --- | --- | --- |
| CLI human | its own `kind:` line and its own column | its own `status:` line and column |
| CLI `--json` | `"kind"` | `"status"` |
| RPC `decision.list` / `decision.get` | `"kind"` on the document object | `"status"` |
| MCP `atlas_decisions` / `atlas_decision` | `"kind"` | `"status"` |
| Web API `/api/v1/decisions` | `"kind"` | `"status"` |
| Mission Control | a filled chip, its own column, its own filter | an outlined traffic-light tag, its own column, its own filter |
| `context build` items | `"knowledge_kind"` | `"knowledge_status"` |

Filtering is per axis and the axes are independent: `--kind INVARIANT`,
`--status APPROVED`, or both. Totals are reported per axis too —
`total_by_kind` beside the five `total_*` status counts — and are never narrowed
by the filters, because they are the denominator a filtered page is read
against.

A client that omits `kind` creates a DECISION. A client that has never heard of
kinds is unaffected in every direction: it proposes decisions, it can still
revise a POLICY (an absent `kind` is not an assertion), and it reads a `kind`
field it can ignore.

## The state machine

Five states, one closed vocabulary, and an append-only ledger.

```
                  approve
   PROPOSED ─────────────────────▶ APPROVED
      │                            │    │
      │ reject                     │    │ resolve, for an OBLIGATION or an
      ▼                            │    │ ACCEPTED_RISK whose demand was met
   REJECTED  (terminal)            │    ▼
                                   │  RESOLVED  (terminal)
                                   │
                                   │ a later revision of the same document
                                   │ is approved, or the document is
                                   │ superseded by another document
                                   ▼
                              SUPERSEDED  (terminal)
```

The complete transition table, which `atlas_decision_transition_allowed` is the
sole authority on. A9.1 gave it the document's kind, and the kind widens the
table in exactly one cell and narrows it nowhere:

| from | to | allowed | why |
| --- | --- | --- | --- |
| PROPOSED | APPROVED | yes | the point of the phase |
| PROPOSED | REJECTED | yes | refusing is a first-class outcome, for every kind |
| PROPOSED | SUPERSEDED | **no** | superseding something never effective records that policy changed when none existed |
| PROPOSED | RESOLVED | **no** | discharging a demand nobody accepted would make recording it and satisfying it one step |
| APPROVED | SUPERSEDED | yes | the only way out of effective that names a replacement |
| APPROVED | RESOLVED | **kind-dependent** | yes for OBLIGATION and ACCEPTED_RISK, no for the other six |
| APPROVED | REJECTED | **no** | retracting means approving a replacement, which leaves a record of what replaced it |
| REJECTED | APPROVED | **no** | "we said no and then it quietly became policy" is the failure the ledger exists to prevent |
| REJECTED | anything | **no** | terminal |
| SUPERSEDED | anything | **no** | terminal |
| RESOLVED | anything | **no** | terminal |

### RESOLVED, precisely

An approved OBLIGATION whose demand has been met is **not superseded** — nothing
replaced it — and **not rejected**, because it was accepted and it was real.
Without a fifth state, closing one out meant either lying about a replacement or
leaving a discharged obligation reported as outstanding for ever.

What resolving does: moves one revision from APPROVED to RESOLVED, appends one
`decision_events` row, and stops the document being effective. What it does not
do: delete anything, edit any prose, name any replacement, or say the record was
wrong. A resolved obligation was a real obligation and its rationale stays
readable, which is the point — the next person can see why the work was
required.

It is **not** a synonym for "we changed our mind". Withdrawing a record that
still stands is a supersession, as it always was.

**Resolving is an operator action.** It consumes a single-use capability bound to
one revision and one content hash, obtained through the same interactive channel
approval uses, with the same honesty limits (see [the claim](#the-claim-and-the-non-claim)
— every word of it applies to `resolve` unchanged). `decision.resolve` sits in
the operator-uid RPC group beside `decision.approve`; there is no MCP tool for
it and no gateway route. Closing an obligation is a claim that work was done, and
a model must not be able to make it.

**Reopening is possible and is not a transition.** A resolved revision stays
resolved for ever. Proposing a new revision and approving it makes the document
effective again — so reopening leaves exactly the trail an approval does, rather
than a state that quietly comes back.

`RESOLVED` sits between an outstanding proposal and a refusal in the status
precedence, in both `recompute_status` and the ledger replay in
`atlas_db_decision_verify`. The two must agree exactly: this is the replay that
decides whether the cached status is honest, and a replay with its own opinion is
not a check. A document with a resolved revision *and* an outstanding proposal
reads as PROPOSED, for the same reason rejecting one revision never made a
document REJECTED while another was pending — what a reader needs to know first
is that there is something to look at.

### The twelve rules, and where each is enforced

1. **A proposal may become approved or rejected.** —
   `atlas_decision_transition_allowed`.
2. **Approved content is immutable.** — no `UPDATE` statement in
   `db_decision.c` names a content column; the only one that touches
   `decision_revisions` sets `state`.
3. **A rejected revision can never be approved.** — the transition table, and
   the conditional `UPDATE ... WHERE state = ?`.
4. **Changing an approved decision creates a new proposed revision.** —
   `op_revise` always writes a new row with the next `revision_no`.
5. **The old approved revision remains effective until the replacement is
   approved.** — `op_revise` never touches `current_revision_id`.
6. **Approving the replacement and superseding the previous revision are
   atomic.** — one writer transaction in `atlas_decision_apply`.
7. **Supersession cannot cross repositories.** — checked when the capability is
   issued *and* when it is spent.
8. **Supersession cycles are impossible.** —
   `atlas_db_decision_supersede_reaches` walks the chain to a bounded depth and
   refuses when it cannot prove acyclicity.
9. **At most one current approved revision per document.** — a partial
   `UNIQUE INDEX ... WHERE state = 'APPROVED'`, so a wrong ordering fails loudly
   instead of leaving two effective revisions.
10. **Nothing is ever physically deleted.** — the only `DELETE` in the group
    removes *expired, unconsumed* capabilities.
11. **Repeated requests and hook retries are idempotent.** — a dedup key, plus
    content-hash equality against the newest revision.
12. **Conflicting concurrent transitions fail deterministically.** — every
    transition's `UPDATE` names the state it observed; the loser changes no rows
    and gets a typed conflict rather than winning by being second.

## Immutable revisions and the canonical content hash

A revision is never edited. Any content change is a new revision with the next
number, and the previous one keeps its own state and its own place in the
ledger.

Approval binds to a **content hash** rather than to a row, so a reader can see
which bytes were approved without having to trust that the row is the one that
was there. The hash is domain-separated and length-prefixed:

```
SHA-256( "atlas.decision.revision.v1\0"
         || field("title",    len, bytes)
         || field("context",  len, bytes)
         || field("decision", len, bytes)
         || field("rationale",len, bytes)
         || field("consequences", len, bytes)
         || field("scope",    len, bytes)
         || field("alternatives", count) || field("alternative", …) × count
         || field("links", count) || <each link, in canonical order> )

field(name, len, bytes) := name || '\0' || u64be(len) || bytes
```

Three properties, each of which is a decision somebody could get wrong:

- **Length-prefixed, not delimited.** With any single-byte delimiter, a title of
  `a|b` with a decision of `c` encodes identically to a title of `a` with a
  decision of `b|c`. Two different documents sharing a digest is not an
  identity, and an approval bound to it would be an approval of either.
- **Domain-separated.** Atlas computes SHA-256 over file contents, canonical
  root paths and compile-command strings. A bare digest says nothing about what
  was hashed, and all of them live in one database.
- **Links are a set; alternatives are a list.** Links are sorted into a
  canonical order, because naming three paths in a different order is the same
  decision. Alternatives keep their order, because a list of alternatives is
  ordered by the proposer's judgement.

### What is hashed, field by field

The rule is one sentence: **everything immutable that changes what was approved
is hashed; everything database-local or recomputed is not.**

The line is not obvious, so every field is listed. "Immutable" means Atlas never
updates the column after the row is written; "derived" means the value is
computed from something else, either at write time or at read time.

| field | immutable? | hashed? | why |
| --- | --- | --- | --- |
| `title` | immutable | **yes** | content |
| `context_text` | immutable | **yes** | content |
| `decision_text` | immutable | **yes** | content |
| `rationale_text` | immutable | **yes** | content |
| `consequences_text` | immutable | **yes** | content |
| `scope` | immutable | **yes** | how broad the decision claims to be |
| alternatives (count) | immutable | **yes** | so one empty alternative cannot encode as none |
| alternatives (text, in order) | immutable | **yes** | content; order is the proposer's judgement |
| `proposed_by` | immutable | **yes** | mutating `MODEL_INFERENCE` to `MODEL_PROPOSAL` would upgrade an approved record's apparent standing without changing a word |
| `basis_head` | immutable | **yes** | a decision taken against commit X is not the one taken against commit Y |
| `decision_revisions.basis_repo_identity_hash` | immutable, **captured on the revision** | **yes** | "about *that* repository" is part of what was approved. The identity hash and not the row id, because a row id is reused and is not comparable across databases. Written once, at insert, from whatever the identity was at that moment — **including an explicit empty string when there was none** — and never updated afterwards by any statement in Atlas |
| `decision_documents.repo_identity_hash` | **mutable** — document-level attachment metadata | **no** | a different thing that reads like the same one. It governs where the *document* is currently attached and which repository may automatically relink it, it starts empty and is backfilled when the lineage first becomes knowable, and it is deliberately outside the hash |
| `decision_documents.kind` (A9.1) | immutable — written by the INSERT that creates the document, named by no `UPDATE` | **no** | it is *identity-like rather than content*: fixed before revision 1 exists, unchangeable under an approval, and therefore covered by the approval by construction — an approval binds document uid + revision number + hash, and the uid's kind cannot move. Hashing it would also move every digest already approved, and `atlas doctor` reports a moved digest as tampering. Reclassifying is superseding with a record of the right kind, which keeps the history of how the knowledge used to be classified rather than erasing it |
| A9.2 claims, attestations, evidence, verification results (`verify_*`) | mutable — evidence accumulates and a verdict is recomputed on every read | **no** | verification is *about* a revision and is not part of it. Hashing it would be incoherent in both directions: the digest would move whenever anybody attested anything, so every approval would report as tampering the moment a claim was examined; and a revision's bytes have to be hashable before any evidence about them exists. This is the separation A6 makes between the content hash and the evidence digest, and A8's final closure makes between a revision and the reason recorded for one of its edges — evidence about a thing is not the thing |
| `verify_lifecycle_audit.content_hash` | immutable — the digest a warrant was bound to | **no** | it *records* a content hash rather than contributing to one; a warrant binds to the revision's existing digest exactly as an operator challenge does |
| link kind | immutable | **yes** | selector |
| link `path_raw` | immutable | **yes** | selector; raw bytes, because a path is bytes |
| link `commit_oid` | immutable | **yes** | selector |
| link `symbol_name` | immutable | **yes** | selector |
| link `symbol_kind` | immutable | **yes** | selector: a `struct config` and a `config` variable are different anchors |
| link `symbol_line` | immutable | **yes** | selector |
| link `target_document_id` (as uid) | immutable | **yes** | selector |
| link `change_set_id` | immutable | **yes** | selector |
| link `basis_commit` | immutable | **yes** | **snapshot**: what the link was taken against |
| link `file_content_hash` | immutable | **yes** | **snapshot**: without it, a `CHANGED` link could be rewritten into a `CURRENT` one under an approved record |
| link `analyzer_name` | immutable | **yes** | **snapshot**: distinguishes "the code changed" from "Atlas changed its mind about the code" |
| link `analyzer_version` | immutable | **yes** | **snapshot**, same reason |
| `document_id` | immutable | no | database-local row id; exactness comes from binding document uid + revision number + hash |
| `revision_id` | immutable | no | database-local row id |
| `revision_no` | immutable | no | bound at approval alongside the hash, and guaranteed unique per document by the schema |
| `created_at` | immutable | no | when it was written, not what was decided; changing it alters no approved meaning |
| `session_id` | immutable | no | database-local, and *attribution* rather than content — see below |
| `session_unbound`, `unbound_reason` | immutable | no | derived from the attribution attempt |
| `imported_from_ai_decision_id` | immutable | no | a pointer to the A2 origin, not a statement about the decision |
| `dedup_key` | immutable | no | an idempotency token supplied by the caller, not content |
| `state` | **mutable** | no | where the ledger has left the row. Hashing it would make an approval stop verifying the instant it was granted |
| `path_text`, `symbol_name_text` | derived | no | the `%XX` display encodings of the raw bytes beside them; hashing both would hash the same information twice and tie identity to the encoder |
| link currency (`CURRENT`…) | derived, not stored | no | an observation made when the link is read. Hashing it would make an approved revision's identity change whenever the code changed, which is exactly backwards |
| link `match_count` | derived, not stored | no | same |

Three entries deserve their reason spelled out.

**The two repository-identity rows are not a duplication.** They are the same
value at two different times, and the distinction is the whole reason the first
one exists.

`decision_revisions.basis_repo_identity_hash` is a *capture*: what the identity
was when the revision was written, frozen into the row and hashed with it. The
document's `repo_identity_hash` is *attachment metadata*: where the document
currently lives, backfilled the first time a scan makes the lineage knowable,
and consulted only when deciding whether a re-registered repository may reclaim
it.

Hashing the document's copy is not a hypothetical mistake — it is what the first
implementation did, and it was wrong in a way that only showed up later. A
revision proposed before the repository had been scanned recorded an empty
identity. The next scan backfilled the document. The canonical hash was then
recomputed over a value that had changed since the revision was written, so
`atlas doctor` reported a perfectly healthy record — one nobody had touched — as
corrupt. An ordinary propose-then-scan was enough to trigger it.

The rule that follows is absolute: **a revision's stored hash is never
"repaired".** If a revision does not verify, that is the finding, not a problem
to be edited away. Which is why the fix was to give the revision its own
immutable column rather than to recompute anything.

Some consequences, all of them intended:

- A revision written before any history was ingested keeps an **empty** captured
  identity for ever. Empty is a value here, not a null — "no identity was
  knowable at the time" is itself a fact worth freezing, and it hashes as the
  empty string rather than being skipped.
- **An identity-unknown revision can still be proposed, approved, rejected and
  superseded.** Nothing in the lifecycle consults the captured identity, so
  refusing to approve one would be inventing a restriction to make a field look
  tidier. `atlas decision approve` names the gap at the prompt, in these words
  and no more:

  > This revision has no captured repository identity. Approval covers the
  > displayed content, but does not bind it to a non-empty repository identity
  > and cannot provide automatic reattachment after repository removal.

  That is a statement about what the approval does and does not bind, and it is
  deliberately the whole of it. The prompt makes no claim about whether the
  decision should be approved, and offers no reassurance of any kind: the
  content is untrusted project prose that Atlas is about to display and has not
  judged, so a general endorsement printed beside it would be Atlas vouching for
  something outside what it can know. Naming the missing guarantee is inside
  that boundary; endorsing the decision is not.
- **A later revision of the same document captures the identity that is
  available to it.** Revisions are independent captures, not inherited ones, so
  revision 1 of a document may hold an empty identity while revision 2 holds a
  real one. That is not an inconsistency; it is the record correctly saying when
  each was written.
- `atlas doctor` verifies each revision against **its own** captured value, so
  the backfill is invisible to it. `tests/test_decision_lifecycle.c` proves both
  directions: mutating the revision's copy is detected, and mutating the
  document's copy is not flagged, because it is not part of what was approved.

**`session_id` is excluded** because it is attribution rather than approved
meaning: it records which conversation proposed the revision, and a decision
says the same thing regardless. It is also a database-local row id, so including
it would make the digest incomparable across machines for no gain. Mutating it
would misattribute a record, which is a real fault — but it is not one an
*approval* covers, and the honest thing is to say so rather than to widen the
digest until the word "approved" stops meaning anything specific.

**A relation's rationale is excluded**, and the exclusion is what makes it
possible to record one at all. A rationale explains *why* one decision was
related to another. It is written whenever somebody can explain the edge, which
is routinely long after the revision carrying that edge was approved — so it is
not part of what was approved, and hashing it would say that it was. It would
also make the record unusable in practice, twice over: adding the field would
change the encoding of every existing revision, so every stored digest would
disagree with its content and `atlas doctor` would report all of them as
tampered with; and attaching a reason afterwards would require a new revision
and a fresh approval for every document that ever gained an edge.

So the reason lives in `decision_edge_events`, keyed by the semantic edge —
source document, target document, kind — and never by a `decision_links.id`,
because a link row is rewritten with a fresh id on every revision and an
id-keyed reason would be silently lost by the next revise. The table is
append-only: `ADDED` and `ANNOTATED` carry the rationale, `REMOVED` carries the
reason an edge was withdrawn, ordering is the `id` and never a timestamp, and a
correction appends rather than overwrites. What is *live* is still decided by
the current revision's links, which are canonical for that; this table is the
account of how they came to be. It is the same separation A6 draws between the
content hash, which never changes, and the evidence digest, which is expected
to.

**Live currency is excluded** and this is the load-bearing exclusion. If a
link's currency were hashed, editing a linked file would change an approved
revision's digest, and every approval would appear corrupt the first time
anybody touched the code. What must be covered is the *snapshot* — what the link
was recorded against — and that is covered.

### Verifying it

`atlas doctor` rehashes **every** revision and compares against the stored
digest. Since Atlas never updates a content column, a mismatch means something
outside Atlas changed one — and any approval bound to that digest now covers
bytes that are not there. It is reported, never repaired.

`tests/test_decision_lifecycle.c` mutates each class of hashed field in turn,
one per fixture, and requires that the rehash catches every one. It also mutates
an unhashed field and requires that it does *not* — so the test is a statement
about where the line is, not just that a line exists.

## The ledger is canonical

`decision_events` is append-only and is the record. The `current_status` and
`current_revision_id` columns on `decision_documents` are a **cache** of it,
written in the same transaction as the event that justifies them.

`atlas_db_decision_verify` replays the ledger and compares. It **reports, never
repairs** — `atlas doctor` calls it, and a diagnostic that fixes what it finds
cannot tell you whether the fault recurs. `atlas decision history` reports
`ledger_agrees` for the same reason.

## Bounded content

| field | limit | notes |
| --- | --- | --- |
| title | 200 bytes | single line; a multi-line title in a confirmation display is the beginning of a forged prompt |
| context, decision, rationale, consequences | 4096 bytes each | newlines and tabs allowed |
| alternatives | 16 × 512 bytes | refused past the ceiling, never truncated |
| links | 128 per revision | of every kind together |
| revisions | 1000 per document | a decision needing more is two decisions |

Every field is strict UTF-8. Rejected outright: NUL, C0 controls other than
newline and tab, DEL, C1 controls, `U+2028`/`U+2029`, and the bidi overrides and
isolates `U+202A`–`U+202E` and `U+2066`–`U+2069`.

The bidi set is the Trojan Source set: it reorders displayed text without
changing the bytes, so an approval prompt could show one decision while the
record held another. These are *refused* rather than escaped, because a decision
document is durable, canonical, and read by a person who is about to approve it
— and validating beats escaping wherever a value can be required to have a
shape. The ordinary directionality marks used in real right-to-left prose are
not in this set and are accepted.

## Code links, and what Atlas will not guess

A revision may link to paths, commits, change sets, symbols, and other decision
documents.

**No link is a foreign key into the A3 structural tables.** A `code_symbols` row
is derived data that a rebuild deletes and an analyzer upgrade replaces; a
decision is not, and a durable record whose subject evaporates when a cache is
rebuilt is not durable. A symbol link is a *selector snapshot*: the name bytes,
the kind, the file, the line, the basis commit, that file's content hash at the
time, and the analyzer name and version.

Currency is computed **when the link is read**, never stored:

| currency | means |
| --- | --- |
| `CURRENT` | the anchor resolves and its recorded content identity still matches |
| `CHANGED` | it resolves, and the content has changed since the snapshot |
| `MISSING` | it no longer resolves: the path is gone, or no symbol of that name and kind is recorded any more |
| `AMBIGUOUS` | several anchors match, and Atlas will not choose |
| `UNKNOWN` | Atlas has not looked — the relevant index has never completed a pass — or nothing was recorded to compare against |

A cached currency is wrong between the change and the recomputation, and "is
this decision still about this code?" is exactly the question a stale cache must
not answer.

**Atlas never re-points a link.** A rename yields `MISSING`, not a quiet new
target. Several matches yield `AMBIGUOUS` with the count. Choosing would be
inventing, which is the same rule A3 applies to a same-named symbol.

A symbol snapshot's recorded *file* is part of the selector and is consulted
first, so a common `static` name resolves against the file it was recorded in
rather than being permanently ambiguous. Only when it is no longer defined there
does the repository-wide lookup run, and then only to distinguish "gone" from
"gone from here".

**A changed link never revokes an approval.** The decision still stands; the
link is reported as needing review. `atlas decision show` says how many, and the
automatic context envelope carries the count as an integer.

### Structural rebuilds and analyzer upgrades

`atlas code sync --rebuild` and an `ATLAS_CODE_ANALYZER_VERSION` bump both
delete and recreate every `code_*` row. Because nothing in migration 6
references those tables, every decision, revision, event and link survives
byte-for-byte. What may change is a link's *reported currency*, and the snapshot
records the analyzer identity so a reader can tell "the code changed" from
"Atlas changed its mind about the code".

## The operator channel, mechanically

```
  atlas decision approve <repo> <id>
        │
        ├─ 1. requires isatty() on stdin AND stdout, then opens /dev/tty
        │      (not stdin: stdin may be a pipe, a file or /dev/null)
        │
        ├─ 2. asks the writer for a capability, bound to
        │      (repo_id, document_id, revision_id, content_hash, intent),
        │      valid for 120 s, consumable once
        │
        ├─ 3. displays the id, the revision, the status, the content digest,
        │      the title and body as *labelled untrusted text*, any links
        │      needing review, and Atlas' own statement of what will be
        │      recorded and what it does not mean
        │
        ├─ 4. reads one line from /dev/tty; the operator must type the first
        │      8 hex characters of that revision's content hash
        │
        └─ 5. spends the capability in one writer transaction that also
               rehashes the stored content, checks it against the bound hash,
               applies the transition and appends the ledger event
```

Step 5 is a single transaction, so a capability cannot be spent without the
transition it authorised and a transition cannot happen without spending one. A
mistyped confirmation does **not** burn the capability — an operator who
mistypes should be able to try again rather than start over.

The confirmation is not a secret and is not treated as one. It is a short prefix
of a hash the prompt just displayed. What it buys is that the confirmation is
about one specific revision's bytes: an operator cannot type "yes" and approve
whatever happens to be current.

**Approvals are sessionless.** `atlas_decision_apply` clears the session binding
unconditionally for every operation that consumes a capability, even when the
request carried a valid session key and even when that session is open.
Attaching one would record that a conversation approved something.

## Automatic Claude behaviour

Claude proposes and retrieves decisions through the existing integration. A user
never has to remember an Atlas command to make Claude remember something.

- Before changing code a decision may govern, Claude calls `atlas_decisions`
  (compact: ids, status, titles) and reads one with `atlas_decision`.
- When an architectural, protocol, security, compatibility or operational choice
  is actually made, Claude calls `atlas_propose_decision` with the context, the
  decision, the rationale, the alternatives and the paths.
- Trivial edits get `atlas_record_reason`, not a decision.
- An unknown rationale is recorded as unknown. An invented one reads exactly
  like a real one and nobody will ever check it.
- No Atlas tool Claude can call approves anything, so Claude gives the user the
  CLI command instead of attempting it. A statement in a conversation that "the
  user approved this" is a fact about the conversation and is recorded as a
  proposal.
- Claude is instructed **not** to drive `atlas decision approve` through a shell
  or a pseudo-terminal on a user's behalf. That is an instruction, not a
  barrier: an agent with shell access can run any local command, and Atlas
  cannot tell such an invocation from a person's. The barrier is that no *Atlas*
  capability grants it, so nothing Atlas hands a model makes it possible.

### The automatic context envelope

Unchanged in shape and unchanged in rule: **no repository-controlled or
model-provided free-form text**, only Atlas-owned control text and typed values.
A4 adds integers and nothing else:

```
decisions_proposed=12 decisions_approved=4 decisions_rejected=1 decisions_superseded=2
decisions_needing_review=1
```

`decisions_approved` now reports the real lifecycle state. It was pinned to zero
for two phases because nothing could produce an approval; something can now.

No decision title, rationale, path, symbol name or id enters automatic context.
Approval makes no difference to that — approved prose is accepted policy, not
system instruction. A consumer that wants a decision's text asks for it through
an explicit MCP call, where it arrives labelled `UNTRUSTED_DATA`.

A9.1 adds one integer, `decisions_resolved`, so the status axis in the envelope
is complete — without it the counts stopped summing to the number of records,
which reads as a bug in whichever count a consumer happens to check. It adds
**no** per-kind counts: a knowledge kind is a fixed Atlas vocabulary and so would
be safe to emit, but eight more integers is a lot of envelope for a question one
MCP call answers exactly, and the envelope's job is to be small enough to read.

`tests/test_ai_trust.c` enumerates the envelope's complete field vocabulary and
fails on any `key=` Atlas did not list, which is how A4's and A9.1's additions
were forced to be deliberate. A9.1 widened that check from line *prefixes* to
every field on a line — it had been possible to append a field to a line somebody
had already listed and never list it, which is what `decisions_rejected=`,
`decisions_superseded=` and `decisions_resolved=` all did.

## CLI

```sh
atlas decision list NAME [--status APPROVED] [--kind INVARIANT] [--limit N]
atlas decision show NAME ID [--revision N]
atlas decision search NAME QUERY [--kind OBLIGATION]
atlas decision history NAME ID
atlas decision links NAME ID                # the account of this decision's relations
atlas decision for-file NAME PATH [--kind POLICY]
atlas decision propose NAME --title T --decision D \
      [--kind DECISION|POLICY|INVARIANT|OPERATIONAL_FACT|ACCEPTED_RISK|OBLIGATION|PARKED|REJECTED_ALTERNATIVE] \
      [--context C] [--rationale R] [--consequences Q] [--scope PATHS] \
      [--alternative A]... [--path P]... [--commit OID]... [--symbol-link S]...
atlas decision revise  NAME ID --title T --decision D [...]   # --kind is checked, never applied
atlas decision approve NAME ID              # interactive; needs a terminal
atlas decision reject  NAME ID              # interactive
atlas decision supersede NAME ID --by ID2   # interactive
atlas decision resolve NAME ID              # interactive; OBLIGATION and ACCEPTED_RISK only
atlas review apply FILE [--check] [--json]  # interactive; walks a review sheet, entry by entry
atlas decision link add    NAME SOURCE TARGET [--why TEXT]
atlas decision link remove NAME SOURCE TARGET  --why TEXT
atlas decision link note   NAME SOURCE TARGET  --why TEXT [--provenance P] [--event E]
atlas decision export NAME ID [--format markdown|json]
atlas decision legacy NAME                  # A2 proposals, and which were promoted
atlas decision promote NAME LEGACY-ID       # make an A4 document from an A2 proposal
```

Worked example:

```
$ atlas decision propose proj --title "Use WAL journalling" \
      --decision "Enable WAL on the index database." \
      --rationale "Readers must not block the writer." \
      --alternative "Keep the rollback journal" --path src/db/db.c
decision:     atlas-dec-e9c67a2cb2288fe7
repository:   proj
revision:     1
state:        PROPOSED
content hash: 634eaddde7fc29accd448a9e041b0e7304c453b3db2a415d7352b3b655abf984

This is a proposal, not an approval. Approve it with
  atlas decision approve proj atlas-dec-e9c67a2cb2288fe7
on a terminal.

$ atlas decision approve proj atlas-dec-e9c67a2cb2288fe7

Atlas decision approve
  repository : proj
  decision   : atlas-dec-e9c67a2cb2288fe7
  revision   : 1
  status     : PROPOSED
  digest     : 634eaddde7fc29accd448a9e041b0e7304c453b3db2a415d7352b3b655abf984

  title (untrusted project text):
    Use WAL journalling

Atlas will record this as LOCAL_OPERATOR_CONFIRMED. That means the action came
through this interactive channel. It does not identify you, does not prove a
person was present, and is not a signature.

Type 634eaddd to approve this exact revision, or anything else to abandon:
```

`--json` is refused for the three interactive verbs: the prompt goes to the
terminal and the result to stdout, and interleaving a human prompt with a
machine document serves neither.

### `atlas review apply`: a sheet walks the same channel, one entry at a time

A15 adds one more way to reach the operator channel above, and it is not a
second channel: `atlas review apply FILE` reads a **review sheet** — a
plain-ASCII list an operator builds by clicking through Mission Control's
Review view and copies out as text — and loops
`atlas_service_decision_confirm` once per entry, with the reviewed revision
pinned. Every entry still ends at the same prompt shown above, on the same
`/dev/tty`, with the same typed hash prefix; the sheet only says which
records to walk and in which order. The full grammar, its refusal sentences
and the command's output shapes are pinned in
`docs/plans/2026-09-03-review-surface.md` §Frozen formats and implemented in
`include/atlas/review.h` and `src/core/review.c` — they are not repeated here
in a form that could drift from either.

Before minting anything for an entry, the walker re-reads the record and
refuses the entry — costing no challenge at all — when the newest revision is
no longer the one the sheet names, when that revision's content hash no
longer starts with the sheet's prefix, or when the record's status is no
longer the one the intent needs. A sheet is a plain list with **no field for
a confirmation**: queuing a record in a browser stores no authority anywhere,
and the only thing that ever disposes of one is this same interactive prompt,
which Atlas records as `LOCAL_OPERATOR_CONFIRMED` — that names the channel,
not a person, exactly as every other transition on this page does.

### `--revision N`, and a pinned revision that is not the newest

`decision approve`, `reject` and `resolve` accept `--revision N`
(`src/cli/cli.c:848-858`, parsed into `st->opts.decision.revision` and passed
to `atlas_service_decision_confirm` at `cli.c:2069-2071`), which pins the
capability to one existing revision rather than the document's current one —
the same mechanism `atlas review apply` uses on every sheet entry. **What the
*lifecycle* does with a pinned revision that exists but is not the newest was
established by running the code, not by reading it**: `op_challenge`
(`src/decision/lifecycle.c:911-919`) refuses a pinned revision only when it
does not exist at all, and nothing in `op_approve` compares the pinned
revision against "the latest one". Approving an older, pinned revision
**succeeds**: that revision becomes the document's effective, `APPROVED` one,
and a newer `PROPOSED` revision sitting beside it is left completely
untouched — not superseded, not rejected, not silently promoted. Nothing in
the lifecycle warns about the resulting state or offers a path out of it
beyond an operator noticing and superseding or resolving the older revision
by hand. `docs/backlog.md` records a related defect this also exposed, in the
message `op_approve` writes when it supersedes a previously-effective
revision.

That result was measured with `tests/test_decision_operator.c`'s
`test_a_pinned_revision_that_is_not_the_newest`, and only the lifecycle half
of it: the test drives `atlas_decision_apply` directly, minting and spending
the challenge at the write point, for the same reason
`approve_through_the_write_point` does — the interactive form is refused in a
locked profile, which is the only profile an unprivileged test can run in. It
never runs `--revision N` through `run_decision_confirm` or
`atlas_service_decision_confirm`. **So the flag's own plumbing — that
`--revision`'s parsed value actually reaches the service call unchanged — is
read from the source above, not measured by any test**; what was measured is
what the lifecycle does once a pinned, non-newest revision's challenge is
spent, however it got minted.

### The gate, and a record that is not `APPROVED`

`atlas gate check` and the remote `gate.check` route assess `APPROVED`
records only (`src/core/service_gate.c:365`); a proposal has never been
policy, so there is nothing about it that could have gone stale. Naming any
other record — `PROPOSED`, `REJECTED`, `SUPERSEDED` or `RESOLVED` — narrows
the assessment to zero candidates, and `atlas_gate_narrow_to_one`
(`service_gate.c:456`) treats zero candidates as a refusal: exit code 4
locally, HTTP 404 remotely, with the message
`no approved decision "<uid>" is attached to this repository`. **The filter
is "not `APPROVED`", never "`PROPOSED`"**: a `PROPOSED` uid, a `REJECTED` one,
a `SUPERSEDED` one and a uid that does not exist at all all produce the
identical answer, and a caller cannot tell any of them apart from the
response alone.

### Relating one decision to another, and withdrawing it

`link add` writes a **new proposed revision** carrying one more relation. It has
to: a revision is immutable and its links are covered by the content hash, so
there is no in-place edit and there must not be one. It is idempotent — a target
already related is reported and nothing is written — and it is a proposal, not
an operator action: it mints no capability, moves no status, and alters no
prose.

`--why` records the durable reason. When the relation is **already there**, a
`--why` attaches the reason to the existing edge and writes **no revision at
all**, which is the only honest way to explain the relations of a decision that
is already approved: a reason written now was not part of what was approved
then, and minting a revision for it would move a content hash to cover something
the approval never saw.

`link remove` is the exact mirror and **deletes nothing**. It writes a new
proposed revision asserting one relation fewer; the revision that carried the
relation keeps it verbatim, together with its creation event and its rationale.
So a withdrawn edge stays fully explicable — `atlas decision links` shows it
with `active: false`, the reason it was drawn, and the reason it was withdrawn,
in the order they happened. Withdrawing a relation that was never drawn reports
`removed: false` and writes nothing, so a repeated removal is a no-op rather
than a stream of empty revisions.

`--why` is **required** to withdraw and optional to draw. The asymmetry is
deliberate: an addition that arrives without a reason can be explained later by
annotating the edge, but a removal is the last thing that happens to it, so a
reason not recorded then is not recorded at all.

`link note` records one event about an edge and touches **no link at all** — not
even to check that the edge is live. It is the only honest way to write down
what happened to a relation that has already been withdrawn: there is nothing
left to add or remove, and the remaining act is to say so. It writes no
revision, moves no status and mints no capability. `--provenance` says where the
reason came from (`OPERATOR`, `D1_MANIFEST`, `D3_REPAIR`, `UNKNOWN`) and
`--event` which kind of event it records; both are checked against their closed
vocabularies at the write point, because a request is not the authority on what
a provenance is.

**A withdrawal without a reason is refused at both write paths**, not only in
the CLI. A check a client runs on itself is not a boundary, and a socket caller
that skipped it would otherwise remove a relation and write no removal event —
leaving the edge gone from the current revision with nothing saying why, which
is the one outcome removal must never produce.

**On an approved document, `link remove` does not withdraw the relation
immediately.** It writes a *proposed* revision asserting one relation fewer, and
the approved revision stays effective until the replacement is approved — which
is rule 5 of the lifecycle, unchanged. So the edge continues to read as active
until an operator approves the new revision. That is not a failed removal; it is
the same thing every revise does, and it is why removal cannot quietly change
what is already policy.

A rationale is untrusted prose like every other decision text. It is stored raw,
safe-encoded once on the way out, bounded at `ATLAS_DECISION_EDGE_NOTE_MAX` and
**refused rather than truncated** when it does not fit. A rationale that is
itself a decision id is refused: that is the A8.2 confusion — prose and a
document id sharing a meaning — and it is refused structurally rather than
detected afterwards.

## Export

`atlas decision export` writes Markdown (or `--format json`) **to stdout**. It
never writes into the target repository: Atlas is read-only with respect to a
registered worktree, and a decision document is Atlas' record rather than the
project's file. Writing one would make Atlas the author of something the project
then has to maintain.

Every export carries the trust statement with it, because an exported decision
is a file somebody will paste somewhere.

## Migration 5 → 6, and A2 compatibility

Migration 6 is forward-only, transactional, idempotent and purely additive.
Seven new tables; no existing table recreated, no column altered, no CHECK
relaxed, no row touched. `tests/test_migrate6.c` seeds a populated schema-5
database through the shipped migrations, digests every A0–A3 table column by
column, migrates, and compares.

**The A2 restriction is deliberately left in place.** `ai_decisions.approved`
still CHECKs `approved = 0`, `atlas_provenance_writable_in_a2` still refuses
`USER_APPROVED_DECISION`, and neither A2 insert binds the column. Lifting that
CHECK was the obvious way to build this phase and is the wrong one: it would
make an approval something that happens *to* a model's own row, in the table the
model writes, distinguished from a proposal by one integer.

A2 proposals are therefore:

- **preserved** — untouched by the migration, and still listed;
- **representable** — `atlas_db_decision_legacy_list` reports them alongside
  their imported document, if any;
- **promotable** — `atlas decision promote NAME LEGACY-ID` (daemon method
  `decision.promote`) creates an A4 document whose revision 1 carries
  `imported_from_ai_decision_id`, the A2 row's title, statement, rationale and
  paths. `atlas decision legacy NAME` lists the proposals and shows which have
  been promoted and into what.

  Promotion is a CLI and RPC operation and deliberately **not** an MCP tool: it
  is a curation act over historical records rather than something a model should
  do in the middle of a turn.

### New `atlas_record_decision` calls bridge automatically

A2's tool keeps its schema and its response, because clients installed before A4
still call it. What it does **not** keep is A2's outcome. A successful call now
materialises a real A4 decision document as part of the same call:

1. the A2 row is written, exactly as before, still `approved = 0`;
2. the A4 document, its revision and the `imported_from_ai_decision_id` origin
   link are created through the ordinary promote path — the same single write
   point, the same validation, the same `PROPOSED` outcome;
3. **both happen in one transaction**, so a half-written pair — a legacy row
   that looks unpromoted beside a document with no origin — cannot exist;
4. the response gains a `decision` member carrying the new document's id.
   Additive: an A2-era client that ignores it is unaffected, and one that reads
   it no longer has to tell a user to run `atlas decision promote`.

An official client must not keep producing records that only exist in the legacy
tables. That would make the A4 decision model something a user opted into rather
than something they had.

**Attribution** comes from the request, resolved by exact key, so the document is
attributed to the session that actually made the call — or, when it cannot be
resolved exactly, to none. Never to a neighbour. The A2 row keeps its own
attribution, and the A4 revision points at it, so the origin provenance is
preserved on both sides. An explicit `decision promote` of a *historical* row is
different and is sessionless: the promoting act has no session, and borrowing
the A2 row's would claim a session proposed a record created years later.

**Retries** are absorbed by a content-derived idempotency key, so a redelivered
call creates neither a second legacy row nor a second document.

### The idempotency domain

The key is a SHA-256 built in `put_decision_dedup` (`src/mcp/mcp_tools.c`) over
the domain string `atlas.a2.decision.dedup.v2`. Every component is
**length-prefixed** rather than delimiter-joined, for the same reason the
revision content hash is: with a separator, two different inputs can be arranged
to produce identical bytes, and a dedup key that collides discards somebody's
record. The components, in order:

| # | component | why it is in the key |
| --- | --- | --- |
| 1 | provider — `anthropic` | a second adapter must change this deliberately rather than inherit a namespace |
| 2 | client — `claude-code` | same |
| 3 | **scope**: either the marker `session` followed by the exact session key, or the marker `sessionless` followed by an empty field | the two are different scopes, and typing them apart stops "sessionless" from colliding with "a session whose key is empty" |
| 4 | **repository** | one session is routinely attached to several repositories |
| 5 | title, statement, rationale | the decision itself |
| 6 | the path **count**, then the paths **in order** | the count first so no list can encode as a different one; in order because order is the proposer's own arrangement |

Session scope (3) is deliberate: a key scoped only to the repository and the
content would make two different sessions that reached the same decision
collide, and the second would silently lose its own attribution.

Repository scope (4) is equally deliberate, and is enforced twice
independently. The key carries the repository name, and the store is scoped as
well — `idx_ai_decisions_dedup` is UNIQUE over `(repo_id, dedup_key)` and the
duplicate lookup binds `repo_id`. **Either one alone is sufficient**, which is
why removing just one changes no behaviour. Both are present so the property
does not depend on which layer somebody edits next; `tests/test_decision_bridge.c`
removes both and shows a proposal about the second repository come back
`"duplicate": true` and leave nothing behind.

Every field that carries decision meaning is in the key, so changing any one of
them — title, statement, rationale or the path list — is a new proposal rather
than an absorbed retry. That direction is tested too.

**A generic MCP client** with no session id may still record, and its document is
stored sessionless with a typed reason — never attached to whichever Claude
session happens to be open. Its key uses the `sessionless` scope, so the
repository and payload still separate its records.

### What deduplication cannot tell apart

Content deduplication cannot distinguish a **transport retry** from a
**deliberate second proposal of identical text**. Two byte-identical payloads in
the same scope produce one record, whichever they were. For a client that
supplies a session key this is narrow: the same session, the same repository and
the same words, which is far more likely to be a redelivery than an intention.

For a **sessionless** client it is wider, and worth stating plainly rather than
leaving to be discovered: two *different* generic clients sending identical text
about one repository share the `sessionless` scope and are indistinguishable
from one client retrying, so Atlas keeps one record. Atlas has no stable request
identity for such a client to key on, and inventing one — a timestamp, a
counter, a connection id — would defeat the deduplication that the same clients
depend on for redelivery. A client that needs two identical proposals kept apart
should either identify its session or send text that differs.

Atlas does not claim otherwise anywhere: the key is described as content-derived
throughout, and content is what it deduplicates.

`tests/test_decision_bridge.c` proves all of this end to end through a live
daemon and the real MCP process, with a second session open throughout as the
wrong neighbour, and with one session attached to two repositories.

A promoted document is `PROPOSED`. An A2 row could never have been approved, and
a promotion that made one look approved would be the most damaging thing this
phase could do.

**Whether the promoted revision carries a session depends on which of the two
promotions it is, and the rule producing both answers is the same one: the
session comes from the request, never from the A2 row.**

| promotion | session on the A4 revision | why |
| --- | --- | --- |
| automatic, inside `atlas_record_decision` | **the calling session**, resolved by exact key | that session really is proposing this record, now, in this call |
| explicit `atlas decision promote` of a historical row | **sessionless**, `unbound_reason = no_session_id` | the promotion is happening now, at an operator's request; the historical session did not ask for it |

In the second case the original attribution is not lost — it is one join away,
through `imported_from_ai_decision_id` to the A2 row, which is left exactly as
it was and still records which session proposed the original. Copying that
session onto the new revision would claim it proposed a record created later at
somebody else's request. A2's rule decides it: a gap with a pointer beats a
plausible attribution.

Both are asserted directly against the stored `decision_revisions.session_id`
rather than through a count — `tests/test_decision_bridge.c` for the automatic
case and `tests/test_decision_lifecycle.c` for the explicit one.

There is no SQL-side backfill, because there is no SHA-256 in SQLite and a
canonical content hash cannot be computed inside a migration. Promotion is an
explicit act that goes through the ordinary write path.

## Recovery

| symptom | what to do |
| --- | --- |
| `atlas doctor` reports a cache/ledger disagreement | the ledger is canonical; the cached status is what is wrong. Report it — Atlas does not repair it automatically, on purpose. |
| a decision has vanished from `decision list` | its repository was removed. Decisions are not deleted by `repo remove`; they are detached. Register the same canonical root again and they reattach by root hash. |
| a link reports `MISSING` after a rename | expected. Atlas will not guess the new target. Propose a revision with the new path. |
| a link reports `UNKNOWN` on a fresh index | Atlas has not looked yet. Run `atlas scan` or let the daemon complete a pass. |
| an approval fails with "no longer PROPOSED" | somebody else transitioned it first. Read it again and decide. |
| a challenge expires while you read the prompt | ask for another; the content is re-displayed and re-hashed. |

## Two invariants this phase bends, deliberately

### Invariant 1: "SQLite is a rebuildable index, never the canonical record"

A4 introduces the first Atlas data that is **not** rebuildable from anything. A
decision document, its revisions and its approval ledger exist nowhere else:
they are not derived from git, from the filesystem, or from a compile database.

The exception is narrow and stated rather than quietly taken. Everything else in
the schema remains a rebuildable index. What follows from it:

- these tables are the ones worth backing up;
- nothing deletes a decision record, which is why rule 10 exists;
- `decision_search` is the one decision table that *is* derived, and may be
  rebuilt.

## Repository identity: what makes two registrations the same repository

`repo_root_hash` answers "same directory". A directory is a **location**, not an
identity. Remove a project, `git init` an unrelated one in its place, register
that — and a hash of the path says they are the same repository, so one team's
approved decisions attach to another team's code.

So a decision document also records `repo_identity_hash`, the only thing an
automatic relink matches on. It is a **path-qualified lineage fingerprint**,
committing to:

- the canonical root path's raw bytes,
- the object format, and
- **the sorted set of root commits Atlas has ingested for that repository** —
  commits with no parent, which is the repository's lineage.

The root-commit set is the discriminating component. It survives clones,
fetches, re-registration at the same path and any amount of later history
rewriting, and it differs between unrelated repositories. It costs one indexed
query over `commits`, so it needs no git invocation, no new allowlisted
subcommand and no new plumbing.

The root path is hashed alongside it, and that qualification cuts both ways:

| situation | reattached automatically? | why |
| --- | --- | --- |
| same path, same lineage — a `repo remove` / `repo add` cycle | yes | the whole fingerprint matches |
| same path, unrelated history | **no** | the root commits differ |
| same lineage, different path — a clone or a move | **no** | the root path differs |
| either side has no ingested history | **no** | an empty identity matches nothing |

The third row is a limitation, stated rather than glossed. Matching on lineage
alone would reattach a project's approved decisions to every clone of it on the
machine, including one an operator made only to read. Automatic reattachment
therefore requires the exact path-qualified identity, a move leaves an orphan
that `atlas decision orphaned` lists, and manual relinking is deferred to a
later phase rather than guessed at.

### Fail-closed, by construction

| event | what happens |
| --- | --- |
| a repository row is created | **every** document carrying that `repo_id` is detached, unconditionally |
| a scan or reconciliation pass completes | documents whose recorded identity matches this repository exactly are reattached |
| the identity is unknown on either side | nothing is attached |
| the identity differs | nothing is attached |
| anything at all | nothing is deleted |

The detach happens in `atlas_db_repo_add`, needs no git and no history, and
cannot be forgotten. The attach happens after ingestion, because the lineage is
not knowable before it. Splitting them that way makes the failure mode
fail-closed *by construction*: forgetting the attach can only leave decisions
orphaned, which is visible and recoverable, and can never attach them to the
wrong repository, which is neither.

The detach also closes a separate hole: `repositories.id` is a rowid and **rowids
are reused**, so a new registration very likely gets the id a removed one had.

### Reused row ids, and the pointer that outlived its row

Rowid reuse reaches further than `repositories.id`, and the second case was
found by writing the test for the first.

`ai_decisions` **does** cascade from `repositories`, so `repo remove` deletes a
repository's A2 records along with their idempotency keys. That is what makes a
reused `repositories.id` harmless for deduplication: there is no retained record
for the new repository to collide with, and the key itself carries the
repository *name* as a second, independent scope.

A promoted A4 revision, however, holds that A2 row's id in
`imported_from_ai_decision_id` — and A4 records deliberately do not cascade, so
the pointer outlived its target. `ai_decisions` rowids are reused too, so the
next A2 record written anywhere took an id an orphaned revision was already
pointing at:

- the unique index on that column rejected the insert, and the whole
  `atlas_record_decision` call failed with `UNIQUE constraint failed:
  decision_revisions.imported_from_ai_decision_id` — **recording became
  impossible after any `repo remove`**; and
- had the index not been there, the orphan's pointer would have quietly resolved
  to an unrelated repository's proposal. That is worse: a hard failure is
  visible, a false attribution is not.

`atlas_db_repo_remove` now clears those pointers before deleting the repository,
in the same transaction, through
`atlas_db_decision_forget_legacy_origins`. Nothing real is lost: the row the
pointer named is being deleted either way, and the revision already carries its
own copy of the promoted content. The gap is honest and the alternative was a
pointer to somebody else's record.

`tests/test_decision_bridge.c` reproduces the whole sequence — record, remove,
re-register the *other* tree under the freed name so both the repository row id
and the dedup key are identical, record again — and asserts the orphan keeps its
content without an origin while the new revision owns the reused id.

A document proposed before its repository had any ingested history records an
empty identity. The next completed pass backfills it — the document is
*currently attached* to that repository, so recording that repository's identity
is not a guess. An existing identity is never overwritten, so a replaced
repository cannot launder its way into matching.

Relinking is never done on a name, a remote URL, a branch name or a judgement.

### Orphan visibility

`atlas decision orphaned` lists decision documents attached to no live
repository, and prints how to get them back. This exists because a canonical
record that has become invisible looks exactly like one that was destroyed: a
user who removed a repository and then could not find their approval history
would reasonably conclude Atlas had deleted it.

Explicit manual relinking — "attach this orphan to that repository, I know they
are the same" — is **deferred**. The immediate requirement is safe
non-attachment plus visibility, and both are met. Re-registering the original
repository and scanning it reattaches automatically.

### `repo remove` is no longer a pure cascade

Every other table references `repositories(id) ON DELETE CASCADE`. Decision
documents do not: `repo_id` is a soft reference and `repo_root_hash` is the
durable identity. A foreign key here would make `atlas repo remove --yes`
silently destroy approval history.

The consequence is that removing a repository **orphans** its decisions rather
than deleting them, and an orphaned decision is invisible to every listing until
the same canonical root is registered again. That is a real and deliberate
trade: invisible-but-recoverable beats gone.

`atlas_db_decision_relink_repo` runs inside `atlas_db_repo_add`, which is the
single point where a repository row is created. It has two halves, and the
second closes a genuine hole: `repositories.id` is a rowid and **rowids are
reused**, so an unrelated repository registered after a removal would otherwise
inherit the previous one's decisions. Documents whose root hash matches are
attached; documents carrying this id whose root hash does not are detached.

## The public identifier

`atlas-dec-` followed by **32 lowercase hex characters — 128 bits**.

The derivation is domain-separated (`atlas.decision.uid.v2`) over the repository
identity hash, the document's row id, the creation timestamp, a retry counter,
and 16 bytes read from the kernel CSPRNG. Every input is Atlas-chosen, so the
result carries no byte a repository or a model selected — which is what lets it
appear in automatic model context, where nothing else decision-shaped may.

Three properties, and one non-property:

- **128 bits, not 64.** These identifiers are durable, they are exported into
  Markdown and JSON that leaves the machine, and databases get merged, restored
  and compared. The birthday bound puts a fifty-percent collision at roughly
  four billion identifiers at 64 bits and roughly 2.6 × 10²⁰ at 128.
- **Entropy, not just derivation.** An earlier version hashed only (root hash,
  row id, timestamp), which is reproducible — so two machines indexing the same
  repository would mint the same identifier for two unrelated decisions created
  in the same second.
- **Uniqueness enforced by the database.** The column is `TEXT NOT NULL UNIQUE`.
  Assignment retries up to 8 times with the attempt counter mixed in, and then
  **fails loudly**: a collision at 128 bits means the entropy source is broken,
  and inventing a sequential fallback would hide that.
- **It is an identifier, not a secret.** Nothing anywhere treats knowing one as
  authorisation. Approval needs a capability bound to a content hash, and the
  uid is displayed in every listing.

Validation is exact: the fixed prefix, then exactly 32 characters from
`[0-9a-f]`. Mixed case, short, long, and anything else is rejected — one
identifier must not have two spellings.

## Evidence, and what A4 does not write

A4 writes **no evidence at all**. `atlas_db_evidence_insert` still refuses
everything except `SOURCE` and `GIT`, and
`tests/test_decision_lifecycle.c` asserts the table gains nothing across a full
lifecycle.

The reserved `INFERENCE` kind stays **unused**. A4 introduces no deterministic
inference with a defined provenance — link currency is a comparison of recorded
values, not an inference, and it is reported through its own closed vocabulary —
so using the kind would only mean "it was available". `DECISION` and
`USER_STATEMENT` also stay unused: a decision is a record in its own tables with
its own actor vocabulary, and folding it into `evidence` would make "how does
Atlas know this?" and "what did somebody decide?" one question.

Approval does not promote anything anywhere. An A3 lexical relation stays
`UNIQUE_LEXICAL` however approved the decision that mentions it, and a
`MODEL_INFERENCE` stays an inference. Nothing writes `MODEL_PROPOSAL` into the
structural graph.

## Performance

`scripts/perf-a4.sh` builds a deterministic corpus of 10 000 documents, 25 000
revisions and 100 000 links with all four lifecycle states present, and asserts
its own floors and its own limits rather than printing numbers nobody checks.

Measured on the development machine, p95 of 21 invocations per query, each
figure including process startup because that is what a caller pays. **Every
number below is from one single run of the script**, not a best-of assembled
from several — a table that mixes runs cannot be checked against anything.

| query | observed p95 | required |
| --- | --- | --- |
| `decision list` | 15 ms | < 100 ms |
| `decision list --status APPROVED` | 9 ms | < 100 ms |
| compact search | 7 ms | < 100 ms |
| compact search, no match | 6 ms | < 100 ms |
| full retrieval | 5 ms | < 100 ms |
| history / timeline | 4 ms | < 100 ms |
| decisions for a file | 8 ms | < 100 ms |
| export | 6 ms | < 100 ms |
| passive hook | 2 ms | < 20 ms |

So: **bounded A4 reads observed at 4–15 ms p95 against a required limit of
100 ms, and the passive hook at 2 ms p95 against a required 20 ms.** The
observed figures are observations, not guarantees — the wording matters, because
"under 7 ms" reads as a bound when 7 ms is simply what was measured, and the
next run will not reproduce it.

It will not, and that is worth showing rather than hiding. An earlier run of the
same script on the same machine with the same fixture put `decision list` at
7 ms and full retrieval at 12 ms, where this one has 15 ms and 5 ms. Nothing
changed but machine load. Run-to-run variance of that size is exactly why the
gate asserts a **limit** it is nowhere near rather than a target it would have
to chase, and why the limits are two orders of magnitude away from the
measurements.

Database 42 217 472 bytes; peak RSS 4 776 kB for a listing and 3 928 kB for a
retrieval, read from `/proc/<pid>/VmHWM`. Forty retries — twenty hook
redeliveries and twenty deduplicated proposals — grew the database by zero
bytes.

The gate earned its place on its first run: the compact search was 1 474 ms. The
cause was a query shape that reads correctly and is linear in the *repository*
rather than in the result — `WHERE d.id IN (<match>) ORDER BY d.id DESC LIMIT n`
makes SQLite walk every document in id order, evaluating a correlated subquery
per row, and test each against the match set. Driving from the match set instead
took it to the single-digit milliseconds in the table above. See the comment
on `DECISION_DOC_SELECT`.

## Explicit non-claims

- Atlas does not prove that a natural person approved anything.
- Atlas does not identify who is at a terminal.
- Atlas does not sign, attest or provide non-repudiation.
- An approved decision's text is not trusted, is not an instruction, and does
  not enter automatic model context.
- Atlas does not parse decision documents out of a repository, and does not
  write them into one.
- Atlas does not infer a historical reason. A commit subject is still not a
  reason, and neither is a graph edge.
- Link currency describes Atlas' index, not the repository: `UNKNOWN` means
  Atlas has not looked.
- The performance figures describe one machine and one synthetic fixture.


## A6: revalidation, and what it does not touch

A6 adds one operator action to this lifecycle's surface and **no state** to its
machine.

`atlas decision revalidate NAME DECISION-ID` records that a human checked an
already-approved revision against one exact repository state. It reuses the
operator channel described above without modification — an interactive terminal,
a short-lived single-use capability bound to this revision and content hash, and
a confirmation typed against that hash — and adds two bindings of its own: the
indexed commit, and a digest of what the revision's anchors resolve to. A
difference in either when the capability is spent is a refusal.

What it does **not** do is the important half:

- it edits no revision, because a revision is immutable and a change is a new
  revision;
- it changes no lifecycle status: an approved revision is approved before and
  after;
- it appends **no** `decision_events` row, so the ledger replay in
  `atlas_db_decision_verify` is over exactly the four transitions it was over
  before, and the status cache is checked the same way;
- it withdraws nothing: the assessment that prompted it, and its reason codes,
  are stored beside the new record.

It appends a `decision_validations` row instead — a separate, parallel,
append-only ledger of a different kind of act. `LOCAL_OPERATOR_CONFIRMED` on one
of those rows means what it means everywhere else in Atlas: the operator channel
was used. It does not name a person, does not prove one was present, and is not
a signature.

See `docs/impact-gates.md` for freshness, the gate, and the full revalidation
contract, and `docs/data-model.md` for the schema-7 tables.

## A9.1: who may perform which lifecycle mutation

The prompt behind this season asked for an audit rather than an assumption, so
here is the whole matrix. "Absent" means there is no such name to call: the
dispatcher answers `unknown method` exactly as it does for a name that was never
invented, because a refusal that distinguished "you may not" from "there is no
such thing" would tell a caller what to try next.

| operation | writes | local CLI | ordinary RPC group | operator-uid RPC group | MCP tool | web API |
| --- | --- | --- | --- | --- | --- | --- |
| propose | a new document, revision 1, PROPOSED | yes | `decision.propose` | — | `atlas_propose_decision` | absent |
| revise | a new PROPOSED revision | yes | `decision.revise` | — | `atlas_revise_decision` (A9.1) | absent |
| link add / remove / note | a revision, or one append-only edge row | yes | `decision.link_*` | — | absent | absent |
| promote | an A4 document from an A2 proposal | yes | `decision.promote` | — | absent | absent |
| challenge | a single-use capability | yes, with a terminal | absent | `decision.challenge` | absent | absent |
| **approve** | PROPOSED → APPROVED | yes, with a terminal | absent | `decision.approve` | **absent** | absent |
| **reject** | PROPOSED → REJECTED | yes, with a terminal | absent | `decision.reject` | **absent** | absent |
| **supersede** | APPROVED → SUPERSEDED | yes, with a terminal | absent | `decision.supersede` | **absent** | absent |
| **revalidate** | a validation record, no state change | yes, with a terminal | absent | `decision.revalidate` | **absent** | absent |
| **resolve** (A9.1) | APPROVED → RESOLVED | yes, with a terminal | absent | `decision.resolve` | **absent** | absent |
| set or change a kind | — | only at propose | only at propose | — | only at propose | absent |

Read the rows in two groups. Everything above `challenge` writes a **proposal**:
it changes no lifecycle state, needs no capability, and records
`MODEL_PROPOSAL` — which is why an MCP tool for it is a convenience rather than a
grant. Everything from `challenge` down is the operator channel: it consumes a
capability, it is offered over the socket only to the peer whose `SO_PEERCRED`
uid equals the `operator_uid` in the root-owned policy, and it has no MCP tool
and no web route at all.

### What A9.1 changed here, and what it deliberately did not

**Added:** `decision.resolve` in the operator group, and `atlas_revise_decision`
in MCP.

The MCP addition is the one worth justifying, because "do not grant new mutation
authority" and "eliminate accidental surface gaps" pull against each other.
`decision.revise` has existed since A4 and writes exactly what
`atlas_propose_decision` writes — a PROPOSED revision by a `MODEL_PROPOSAL`
actor — differing only in whether a document already exists. MCP could express
the second and not the first, so a model that noticed an approved record had
gone out of date could only write a *new* record beside it, leaving two documents
about one subject with no relation between them. That is a worse record than the
one it was trying to improve, and nothing about the gap was a boundary: the
operator still has to approve the revision before it means anything.

**Not added, on purpose:** no MCP tool and no web route approves, rejects,
supersedes, revalidates or resolves. Closing an obligation is a claim that work
was done, and a model must not be able to make it. `tests/test_decision_mcp.c`
asserts the whole tool inventory and rejects any tool name containing an approval
verb; `tests/test_orch_rpc.c` asks a live daemon for the names such methods would
plausibly have.

**Unchanged:** every honesty limit in [the claim](#the-claim-and-the-non-claim).
`LOCAL_OPERATOR_CONFIRMED` on a resolution says the channel was used. It does not
name a person, does not prove one was present, and is not a signature — and a
same-uid process, including an AI agent with shell access, may imitate the
channel exactly as it may for an approval.

## A9.1: provenance, per kind

Classification destroys no provenance: every knowledge record keeps the whole A4
and A6 apparatus — the immutable revision, the append-only ledger, the link
snapshots, the edge accounts, the validation records. What follows is where each
kind's particular provenance question is answered.

**An ACCEPTED_RISK distinguishes discovery from acceptance by its status, and
only by its status.** Recording the risk writes a PROPOSED revision with a
`MODEL_PROPOSAL` or operator actor; accepting it is an APPROVED event in the
ledger with `LOCAL_OPERATOR_CONFIRMED` and a consumed capability. The two are
different rows recording different acts, and there is no path from the first to
the second. `atlas decision history` shows both.

**An OPERATIONAL_FACT identifies its source through the ordinary link and
evidence machinery**: path links, commit links, symbol snapshots and
`basis_head`, each with its own currency computed on read. Because the kind
asserts something about *now*, the useful reading is the currency: a link that
reports `CHANGED` on an operational fact is the signal that the fact may have
moved on, which is the same mechanism A6 uses and needs no new field.

**A REJECTED_ALTERNATIVE preserves why it was rejected in its own prose** —
`rationale` and `consequences` are the fields for it, and an `--alternative` list
records what was considered instead. Where the rejection is a *relation* to
another record, `decision link add --why` and `decision link note --why` put the
reason in `decision_edge_events`, which is append-only and outlives every
revision.

**A SUPERSEDED record identifies its successor** through the `replaced_by` link
recorded on the superseded side and `superseded_by_document_id` on the document,
so a reader of the old record is told where to look without a join.

**A RESOLVED record identifies nothing as its successor, deliberately, because
there is none.** What it has instead is the ledger: the APPROVED event that
accepted the demand and the RESOLVED event that closed it, in order, with the
content hash on both. The historical obligation is retained in full — nothing is
deleted and no prose is rewritten — which is what "resolved without rewriting
history" means. If a resolution needs an explanation, the edge-note path records
one; the ledger's `detail` stays a fixed Atlas vocabulary, as it does for every
other transition, so that no operator prose can forge a line in it.

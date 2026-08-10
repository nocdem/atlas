# Decision documents, immutable revisions and operator approval

A4 is the phase in which a proposal can become project policy. The whole
difficulty of the phase is in what that sentence is allowed to mean, so this
document leads with the limits rather than ending with them.

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

## The state machine

Four states, one closed vocabulary, and an append-only ledger.

```
                  approve
   PROPOSED ─────────────────────▶ APPROVED
      │                               │
      │ reject                        │ a later revision of the same document
      ▼                               │ is approved, or the document is
   REJECTED  (terminal)               │ superseded by another document
                                      ▼
                                 SUPERSEDED  (terminal)
```

The complete transition table, which `atlas_decision_transition_allowed` is the
sole authority on:

| from | to | allowed | why |
| --- | --- | --- | --- |
| PROPOSED | APPROVED | yes | the point of the phase |
| PROPOSED | REJECTED | yes | refusing is a first-class outcome |
| PROPOSED | SUPERSEDED | **no** | superseding something never effective records that policy changed when none existed |
| APPROVED | SUPERSEDED | yes | the only way out of effective |
| APPROVED | REJECTED | **no** | retracting means approving a replacement, which leaves a record of what replaced it |
| REJECTED | APPROVED | **no** | "we said no and then it quietly became policy" is the failure the ledger exists to prevent |
| REJECTED | anything | **no** | terminal |
| SUPERSEDED | anything | **no** | terminal |

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

`tests/test_ai_trust.c` enumerates the envelope's complete line vocabulary and
fails on any line Atlas did not start, which is how A4's additions were forced
to be deliberate.

## CLI

```sh
atlas decision list NAME [--status APPROVED] [--limit N]
atlas decision show NAME ID [--revision N]
atlas decision search NAME QUERY
atlas decision history NAME ID
atlas decision for-file NAME PATH
atlas decision propose NAME --title T --decision D \
      [--context C] [--rationale R] [--consequences Q] [--scope PATHS] \
      [--alternative A]... [--path P]... [--commit OID]... [--symbol-link S]...
atlas decision revise  NAME ID --title T --decision D [...]
atlas decision approve NAME ID              # interactive; needs a terminal
atlas decision reject  NAME ID              # interactive
atlas decision supersede NAME ID --by ID2   # interactive
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

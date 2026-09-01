/* Atlas - A9.2: claims, attestations, evidence and self-verifying memory.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A9.1 made a durable record say *what sort of knowledge it is* and *how far
 * through the approval workflow it got*. Neither axis says anything about
 * whether the record is **true**, and nothing in A0..A9.1 could: an approved
 * INVARIANT and an approved guess are the same row.
 *
 * A9.2 adds the third axis. It answers a different question from either
 * existing one, and the whole difficulty of the phase is in keeping the three
 * apart:
 *
 *   kind    — what sort of knowledge is this?          (A9.1, on the document)
 *   status  — how far through approval did it get?     (A4, from the ledger)
 *   verify  — what evidence bears on whether it holds? (A9.2, derived on read)
 *
 * `INVARIANT + PROPOSED + VERIFIED` is a legal and useful combination: Atlas
 * has mechanically established the proposition and nobody has adopted it as
 * project policy. So is `DECISION + APPROVED + INCONCLUSIVE`: somebody with
 * authority chose a direction and the evidence that it was carried out is
 * incomplete. **No code path derives any one of the three from another**, which
 * is the same rule A9.1 states about kind and status, extended to three.
 *
 * ## The five separations this header exists to enforce
 *
 * 1. **An actor is not evidence.** Three models reading one document are three
 *    attestations over one evidence root. Counting them as three independent
 *    corroborations is the single most attractive mistake available to a system
 *    like this, because it is what makes a confident wrong answer cheap to
 *    manufacture. See `atlas_verify_aggregate` and the union-find in
 *    `src/verify/verify.c`: within one independent group only the strongest
 *    attestation counts, never the sum.
 *
 * 2. **Reliability is not authority.** A source that has been right about
 *    control flow a thousand times has thereby gained no power to accept a
 *    privacy risk on the project's behalf. Reliability is a property of a
 *    source's past factual accuracy in a domain; authority is a property an
 *    operator confers. `atlas_verify_basis` separates the two questions and
 *    `ATLAS_VERIFY_BASIS_JUDGMENT` is the class for which no amount of the
 *    former substitutes for the latter.
 *
 * 3. **A confidence score is not a probability.** `confidence` is an integer
 *    0..100 produced by a named, versioned, reproducible aggregation over the
 *    evidence Atlas holds *now*. It becomes a probability only after the
 *    calibration this phase ships the machinery for and — on this machine, at
 *    this moment — has no data for. Reporting `94/100` as `94%` is the lie this
 *    vocabulary is shaped to make hard to tell: they are different types with
 *    different printers, and `atlas_verify_calibration` gates the second.
 *
 * 4. **Descriptive truth is not normative adoption.** Atlas can mechanically
 *    establish "the serializer emits zero here". It cannot thereby establish
 *    "the protocol shall always emit zero here" — that is a choice somebody
 *    makes, and a verifier that blurred the two would let any observation of
 *    the current implementation quietly become permanent policy.
 *    `atlas_verify_claim_semantics` is where the distinction lives, and
 *    `atlas_verify_basis_may_verify_semantics` is where it is enforced: a
 *    deterministic verifier may only ever establish a DESCRIPTIVE claim.
 *
 * 5. **Deterministic verification does not wait for calibration.** This is the
 *    rule the phase is built around and it runs against the intuition that more
 *    caution is always safer. If a proposition has a complete mechanical truth
 *    condition and Atlas evaluated it, how often some model has been right in
 *    the past is not an input to the answer and must not be made into a
 *    precondition for acting on it. Blocking a proven fact on an unrelated
 *    statistic is not conservatism; it is a category error that happens to look
 *    like conservatism. See `atlas_verify_basis_requires_calibration`.
 *
 * ## What is stored and what is derived
 *
 * Stored: claims, actors, attestations, evidence, the evidence dependency
 * edges, append-only verification results, reliability outcomes and the machine
 * lifecycle audit. All of it is canonical — none of it is rebuildable from the
 * repository, because an attestation is a statement somebody made and a
 * repository does not remember that anybody spoke.
 *
 * Derived on read: the verification state, the confidence score, the
 * independent-group count, staleness and the policy verdict. This is A6's rule
 * about freshness and A4's about link currency, for their reason — a cached
 * verdict is a value that is wrong between the change and the recomputation,
 * and "does the evidence still support this?" is exactly the question that must
 * not be answered from a stale cache.
 *
 * A **verification result** row is the one apparent exception and is not one:
 * it records what an aggregation concluded *at a moment*, with the algorithm
 * version and the evidence snapshot that produced it, because a machine
 * lifecycle transition must be reconstructable years later. It is history, not
 * state — the same argument A6 makes for `decision_validations`.
 *
 * ## Backward compatibility is exact rather than approximate
 *
 * `ATLAS_VERIFY_UNVERIFIED` is zero and a record with no claims has no
 * evidence, so every record written before this phase existed reports
 * UNVERIFIED without a migration touching a single decision row. Migration 14
 * is purely additive for that reason: it adds tables and changes nothing, so no
 * content hash moves and `atlas doctor` has nothing new to report.
 */
#ifndef ATLAS_VERIFY_H
#define ATLAS_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/decision.h"
#include "atlas/error.h"
#include "atlas/limits.h"

/* Forward-declared rather than pulling in `atlas/db.h`, the way `orch_ops.h`
 * and `snapshot.h` do: sqlite3 types must not escape src/db, and a vocabulary
 * header should not drag the whole schema API behind it. */
typedef struct atlas_db atlas_db;

/* --- the third axis -------------------------------------------------------
 *
 * What Atlas currently knows about whether a proposition holds. Derived from
 * the claims attached to a record and the evidence attached to those claims,
 * recomputed on every read.
 *
 * **UNVERIFIED is zero**, which is A6's rule about UNKNOWN and A8's about
 * DISABLED: a zeroed struct, an absent row and a record nobody has looked at
 * must all read as "Atlas has not established this", never as "fine". */
typedef enum atlas_verify_state {
    /* No claim, or no attestation on any claim. The state of every record
     * Atlas held before this phase, and the state a new proposal starts in. */
    ATLAS_VERIFY_UNVERIFIED = 0,
    /* A verification run is in flight. Transient and never terminal; a crash
     * leaves it here and the next read recomputes from what actually landed. */
    ATLAS_VERIFY_VERIFYING,
    /* Evidence leans in favour and does not meet the bar for VERIFIED. The
     * ordinary outcome of the empirical path, and never sufficient on its own
     * for a machine lifecycle transition. */
    ATLAS_VERIFY_SUPPORTED,
    /* Established. For a DETERMINISTIC basis this means a mechanical verifier
     * evaluated the claim's stated truth condition and it passed — within the
     * claim's declared scope and no further. For an EMPIRICAL basis it means
     * the aggregate cleared the policy's threshold with the required number of
     * genuinely independent evidence groups. */
    ATLAS_VERIFY_VERIFIED,
    /* Evidence against. Not the same as "the record is wrong": a contradicted
     * claim about an approved decision usually means the implementation
     * diverged from what was approved, which is `ATLAS_CONFLICT_IMPLEMENTATION`
     * and an obligation, not grounds to retract the decision. */
    ATLAS_VERIFY_CONTRADICTED,
    /* Looked, could not tell. Distinct from UNVERIFIED on purpose: "nobody has
     * examined this" and "we examined it and the evidence does not decide"
     * call for different actions, and collapsing them loses the more useful
     * half. */
    ATLAS_VERIFY_INCONCLUSIVE,
    /* There was a verdict and its evidence no longer describes the present —
     * the commit moved, the deployment changed, the observation aged past what
     * policy allows for this kind. The verdict is kept and reported as stale;
     * nothing is deleted. A9.2 never silently re-verifies. */
    ATLAS_VERIFY_STALE
} atlas_verify_state;

const char *atlas_verify_state_name(atlas_verify_state s);
bool atlas_verify_state_parse(const char *name, atlas_verify_state *out);

/* --- which epistemic mechanism produced the verdict -----------------------
 *
 * The most load-bearing enum in the phase, because it decides which of three
 * completely different rulebooks applies.
 *
 * **UNKNOWN is zero and is not writable.** A verification result must say how
 * it was reached; one that does not is not a result. `atlas_verify_basis_
 * writable` refuses it at the write point, mirroring
 * `atlas_provenance_writable_in_a2`, `atlas_code_resolution_writable_in_a3` and
 * `atlas_decision_actor_writable_by_adapter`. */
typedef enum atlas_verify_basis {
    ATLAS_VERIFY_BASIS_UNKNOWN = 0,
    /* A reproducible mechanical verifier evaluated an explicitly bounded truth
     * condition over explicitly bound artifacts.
     *
     * **This does not mean "proven for all implementations for all time".** It
     * means: within the scope the claim declares, Atlas has a pass/fail
     * procedure that another run over the same artifacts reproduces. A unit
     * test establishing `f rejects y at commit z` is deterministic about
     * exactly that and about nothing wider, and a claim whose text outruns its
     * verifier's scope is the failure mode this phase watches for — see
     * `atlas_verify_claim.scope_note` and A8-CI's rule that PROVEN means the
     * compiler proved it.
     *
     * Historical source reliability is not an input here and must never become
     * a precondition. */
    ATLAS_VERIFY_BASIS_DETERMINISTIC,
    /* The proposition depends materially on aggregating evidence of varying
     * quality from sources of varying reliability. Several documents to
     * reconcile, several interpretations to weigh, no single mechanical
     * verifier that settles it.
     *
     * This is the path for which calibration is genuinely required, because the
     * number it produces is a claim about how often sources of this kind have
     * been right — and with no history that claim has no content. */
    ATLAS_VERIFY_BASIS_EMPIRICAL,
    /* A normative choice rather than a discoverable fact. Which architecture to
     * adopt, what to call the product, whether a known risk is acceptable.
     *
     * Evidence informs it and never settles it, because there is nothing for a
     * verifier to measure the proposition against: the question is not what is
     * so but what the project shall do. Atlas may verify the *premises* of a
     * judgment to any strength and still have established nothing about the
     * judgment. No confidence score authorises a JUDGMENT transition. */
    ATLAS_VERIFY_BASIS_JUDGMENT
} atlas_verify_basis;

const char *atlas_verify_basis_name(atlas_verify_basis b);
bool atlas_verify_basis_parse(const char *name, atlas_verify_basis *out);
/* Refuses UNKNOWN. Asked at the single write point, not remembered by callers. */
bool atlas_verify_basis_writable(atlas_verify_basis b);

/* Whether a verdict on this basis needs historical calibration before it may
 * drive a lifecycle transition.
 *
 * True for EMPIRICAL and false for DETERMINISTIC, and that asymmetry is the
 * phase's central rule rather than an optimisation. It is a *function* so that
 * the tests can assert it and no policy path can quietly reintroduce the
 * coupling: a deterministic verdict blocked on an actor's sample count would be
 * blocked on a statistic that is not an input to it.
 *
 * JUDGMENT returns false too, and for a reason that is not the same reason:
 * calibration is irrelevant there because no automatic transition happens at
 * all. Never read a false here as "may proceed". */
bool atlas_verify_basis_requires_calibration(atlas_verify_basis b);

/* --- descriptive versus normative ----------------------------------------
 *
 * The mechanism behind separation 4. A claim says which of two utterly
 * different things it asserts, and a verifier is only allowed to establish the
 * one it can actually observe.
 *
 * DESCRIPTIVE is zero because it is the safe default: a claim that does not say
 * asserts the weaker thing. Recording an observation as normative would be
 * the direction that manufactures policy out of a measurement, so that is the
 * direction that must be stated explicitly. */
typedef enum atlas_verify_claim_semantics {
    /* "The system currently does X." Observable, falsifiable, and true only of
     * the artifacts and moment it is bound to. */
    ATLAS_CLAIM_DESCRIPTIVE = 0,
    /* "The system shall do X." A rule. No measurement of the present
     * establishes it, because it is not about the present. */
    ATLAS_CLAIM_NORMATIVE
} atlas_verify_claim_semantics;

const char *atlas_verify_claim_semantics_name(atlas_verify_claim_semantics s);
bool atlas_verify_claim_semantics_parse(const char *name, atlas_verify_claim_semantics *out);

/* Whether a verdict on this basis may establish a claim with these semantics.
 *
 * DETERMINISTIC + NORMATIVE is **false**, and that single cell is the whole of
 * separation 4's enforcement. A mechanical verifier reads the world; a
 * normative claim is not about the world; so there is nothing for it to read.
 * Allowing the pair would mean that observing an implementation detail once,
 * with a policy that auto-approves deterministic INVARIANTs, silently converts
 * "it happens to do this" into "it must always do this" — with an audit trail
 * that looks impeccable. */
bool atlas_verify_basis_may_verify_semantics(atlas_verify_basis b,
                                             atlas_verify_claim_semantics s);

/* --- what one actor said about one claim ---------------------------------
 *
 * INCONCLUSIVE is zero: an attestation nobody filled in asserts nothing. */
typedef enum atlas_verify_verdict {
    ATLAS_ATTEST_INCONCLUSIVE = 0,
    ATLAS_ATTEST_SUPPORT,
    ATLAS_ATTEST_CONTRADICT
} atlas_verify_verdict;

const char *atlas_verify_verdict_name(atlas_verify_verdict v);
bool atlas_verify_verdict_parse(const char *name, atlas_verify_verdict *out);

/* --- who is speaking ------------------------------------------------------
 *
 * Vendor-neutral by construction. There is no `CLAUDE`, no `ANTHROPIC` and no
 * `OPENAI` in this enum and there must not be: a model's name is a string an
 * operator or a model supplies, and a schema that enumerated vendors would be
 * a schema that has to change when the market does, and — worse — one that
 * invites a reliability rule keyed on a name anybody can type.
 *
 * UNKNOWN is zero. An actor whose class was never established is the weakest
 * thing that can speak, not a neutral one. */
typedef enum atlas_verify_actor_class {
    ATLAS_ACTOR_UNKNOWN = 0,
    /* A person, identified by a stable reference an operator configured.
     * Authority roles are *not* here — see the note on authority below. */
    ATLAS_ACTOR_HUMAN,
    /* A model. Its provider, family, version, role, session and orchestrator
     * are metadata on the row and are self-declared unless authenticated. */
    ATLAS_ACTOR_AI_AGENT,
    /* A program Atlas ran, such as a compiler or a static analyser. */
    ATLAS_ACTOR_TOOL,
    /* A test suite Atlas executed. */
    ATLAS_ACTOR_TEST,
    /* A probe against a running system, at a moment, against a deployed
     * revision. */
    ATLAS_ACTOR_RUNTIME_OBSERVATION,
    /* The repository itself: bytes at a commit. */
    ATLAS_ACTOR_REPOSITORY_EVIDENCE,
    /* A document, at a revision. */
    ATLAS_ACTOR_DOCUMENT,
    /* Atlas' own verification engine, executing a named deterministic
     * verifier. The only actor that may carry a DETERMINISTIC verdict. */
    ATLAS_ACTOR_ATLAS_VERIFIER
} atlas_verify_actor_class;

const char *atlas_verify_actor_class_name(atlas_verify_actor_class c);
bool atlas_verify_actor_class_parse(const char *name, atlas_verify_actor_class *out);

/* --- how well Atlas knows the speaker is who it says ----------------------
 *
 * Separation: **self-description is not identification**. A model submitting an
 * attestation may say it is an AI agent called anything it likes; it may not
 * thereby become a compiler.
 *
 * SELF_DECLARED is zero, because an identity nobody checked is the default and
 * must be the weakest. */
typedef enum atlas_verify_actor_identity {
    /* The submitter said so. Every field is a claim about itself, which is not
     * evidence about itself — A7.1's rule about peer identity, one layer up. */
    ATLAS_ACTOR_IDENTITY_SELF_DECLARED = 0,
    /* The kernel or a credential established it: an `SO_PEERCRED` uid on the
     * socket, or an authenticated gateway credential. */
    ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED,
    /* Atlas created this actor because Atlas performed the act. The only way a
     * TOOL, TEST, RUNTIME_OBSERVATION or ATLAS_VERIFIER actor comes into
     * existence. */
    ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED
} atlas_verify_actor_identity;

const char *atlas_verify_actor_identity_name(atlas_verify_actor_identity i);
bool atlas_verify_actor_identity_parse(const char *name, atlas_verify_actor_identity *out);

/* Whether this actor class may only exist with `ATLAS_ACTOR_IDENTITY_ATLAS_
 * ATTESTED` identity.
 *
 * True for TOOL, TEST, RUNTIME_OBSERVATION and ATLAS_VERIFIER — the four
 * classes whose whole evidentiary weight comes from Atlas having *done* the
 * thing. This is the enforcement for "an AI saying `clang proves this` is not
 * equivalent to Atlas running clang": a model-submitted actor is always
 * SELF_DECLARED, so naming one of these classes is refused at the write point
 * rather than accepted and quietly discounted.
 *
 * Discounting would not be enough. A discounted forgery still appears in the
 * evidence list, still reads as tool output to a human skimming a UI, and still
 * has to be argued away by whoever finds it. */
bool atlas_verify_actor_class_requires_atlas_identity(atlas_verify_actor_class c);

/* --- where a piece of evidence came from ----------------------------------
 *
 * Every evidence row answers "where did this come from?" in a form somebody can
 * go and check. There is deliberately no `TEXT` class: opaque prose with no
 * provenance is what this phase exists to stop being counted as evidence. */
typedef enum atlas_verify_evidence_class {
    ATLAS_EVIDENCE_UNKNOWN = 0,
    /* Bytes in a repository at a commit, optionally a path, symbol and range. */
    ATLAS_EVIDENCE_SOURCE_CODE,
    /* A compiler or static analyser's finding, carrying A8-CI's own evidence
     * class so PROVEN and CANDIDATE do not get flattened into each other. */
    ATLAS_EVIDENCE_COMPILER,
    /* A named test in a named suite, at a commit, with a result. */
    ATLAS_EVIDENCE_TEST,
    /* An observation of a running system at a timestamp. */
    ATLAS_EVIDENCE_RUNTIME,
    /* A value read from deployed configuration, with the config's identity. */
    ATLAS_EVIDENCE_DEPLOYED_CONFIG,
    /* A commit, a range, an authorship fact. */
    ATLAS_EVIDENCE_GIT_HISTORY,
    /* A protocol or specification document at a revision. Describes intent,
     * which is why it is not the same class as source. */
    ATLAS_EVIDENCE_SPECIFICATION,
    /* Any other document at a revision. */
    ATLAS_EVIDENCE_DOCUMENT,
    /* An existing Atlas knowledge record. Derived, never a root: what Atlas
     * already believes cannot be independent corroboration of itself. */
    ATLAS_EVIDENCE_ATLAS_KNOWLEDGE,
    /* A person said so. */
    ATLAS_EVIDENCE_HUMAN_STATEMENT,
    /* A model's analysis. Always an interpretation of something else, which is
     * why it must declare what it read. */
    ATLAS_EVIDENCE_AI_ANALYSIS
} atlas_verify_evidence_class;

const char *atlas_verify_evidence_class_name(atlas_verify_evidence_class c);
bool atlas_verify_evidence_class_parse(const char *name, atlas_verify_evidence_class *out);

/* --- evidence families ----------------------------------------------------
 *
 * Which evidence classes share failure modes, versioned so that a later phase
 * can revise the taxonomy without silently reinterpreting stored results.
 *
 * The point is not to model causation. It is the single conservative rule that
 * **known shared roots must not be double-counted**, plus a coarse statement of
 * which kinds of evidence can fail together. A test generated from the
 * implementation it tests shares failure modes with reading that
 * implementation; a runtime probe against a deployed binary genuinely does not.
 *
 * V1 is three families and no probabilistic machinery, because an auditable
 * rule that is right about the obvious cases beats an elaborate one nobody can
 * check. */
typedef enum atlas_verify_evidence_family {
    ATLAS_EVIDENCE_FAMILY_UNKNOWN = 0,
    /* Artifacts at rest: source, compiler output over that source, git history,
     * documents, specifications. All ultimately readings of a tree. */
    ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT,
    /* Behaviour observed by execution: tests, runtime probes, deployed
     * configuration as actually loaded. */
    ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION,
    /* Somebody's reading of something else: AI analysis, human statement, an
     * existing Atlas record. Never a root. */
    ATLAS_EVIDENCE_FAMILY_INTERPRETATION
} atlas_verify_evidence_family;

const char *atlas_verify_evidence_family_name(atlas_verify_evidence_family f);
/* The class-to-family map, as a function so no caller keeps a second copy. */
atlas_verify_evidence_family atlas_verify_evidence_family_of(atlas_verify_evidence_class c);
/* Bumped when the map changes. Stored on every verification result, so an old
 * result is never reinterpreted under a new taxonomy. */
#define ATLAS_VERIFY_FAMILY_VERSION 1

/* Whether evidence of this class can ever stand as an independent root.
 *
 * False for the INTERPRETATION family. An interpretation is by definition of
 * something, so an interpretation that declares no source is not a fresh
 * observation of the world — it is an undeclared derivation. Treating it as a
 * root is precisely how three models reading one document become "three
 * independent sources", so they are all folded into one shared group instead.
 * See `atlas_verify_independent_groups`. */
bool atlas_verify_evidence_class_may_be_root(atlas_verify_evidence_class c);

/* --- how confident, and whether that word means anything ------------------
 *
 * INSUFFICIENT_DATA is zero. With no history, the honest report is that there
 * is no history — never a default probability, and never silence. */
typedef enum atlas_verify_calibration {
    /* Fewer resolved outcomes than policy requires to say anything. The state
     * of every actor and domain on a machine where this phase has just been
     * installed, and reporting it plainly is the correct behaviour rather than
     * a deficiency to be papered over. */
    ATLAS_CALIBRATION_INSUFFICIENT_DATA = 0,
    /* Outcomes exist, none has been folded into a reliability estimate yet. */
    ATLAS_CALIBRATION_UNCALIBRATED,
    /* Enough to estimate, not enough for the quality bar policy sets. */
    ATLAS_CALIBRATION_CALIBRATING,
    /* Sample count and calibration quality both satisfy policy. Only in this
     * state may a score be reported as a probability, and only then does the
     * empirical path have anything to enforce with. */
    ATLAS_CALIBRATION_CALIBRATED
} atlas_verify_calibration;

const char *atlas_verify_calibration_name(atlas_verify_calibration c);
bool atlas_verify_calibration_parse(const char *name, atlas_verify_calibration *out);

/* --- what kind of disagreement this is ------------------------------------
 *
 * §28's taxonomy. Two claims that disagree textually are usually not a
 * contradiction, and reporting them as one is how a conflict engine becomes
 * noise nobody reads.
 *
 * NONE is zero. */
typedef enum atlas_verify_conflict {
    ATLAS_CONFLICT_NONE = 0,
    /* Same subject, same scope, same binding, incompatible. The only kind that
     * is a genuine contradiction. */
    ATLAS_CONFLICT_CONTRADICTION,
    /* Both were true, at different times. The older is history and stays. */
    ATLAS_CONFLICT_SUPERSESSION,
    /* Both are true, of different things. The compiled default is false and
     * the deployment sets it true: two correct claims at two scopes, and
     * reporting them as a contradiction would train everybody to ignore the
     * conflict list. */
    ATLAS_CONFLICT_SCOPE_MISMATCH,
    /* Approved knowledge says one thing and the implementation does another.
     * **This does not falsify the approved record.** It is a finding against
     * the implementation, and under policy it opens an obligation. Collapsing
     * it into CONTRADICTION would let a broken implementation retract the
     * design it violates, which is exactly backwards. */
    ATLAS_CONFLICT_IMPLEMENTATION,
    /* One side's evidence no longer describes the present. */
    ATLAS_CONFLICT_STALE_EVIDENCE,
    /* Two normative alternatives. Not a factual conflict at all: nothing
     * observable decides between them, and the resolution is a judgment. */
    ATLAS_CONFLICT_COMPETING_NORMATIVE
} atlas_verify_conflict;

const char *atlas_verify_conflict_name(atlas_verify_conflict c);
bool atlas_verify_conflict_parse(const char *name, atlas_verify_conflict *out);

/* --- what the policy engine decided ---------------------------------------
 *
 * The verdict about *acting*, kept separate from the verdict about *truth*.
 * A claim can be VERIFIED and still produce `ATLAS_POLICY_FORBIDDEN` here,
 * which is the normal and correct outcome for an ACCEPTED_RISK.
 *
 * NEEDS_REVIEW is zero: with no policy, nothing is automatic and a human
 * looks. */
typedef enum atlas_verify_policy_verdict {
    ATLAS_POLICY_NEEDS_REVIEW = 0,
    /* Every gate passed and enforcement is on: Atlas performed the transition. */
    ATLAS_POLICY_AUTO,
    /* Every gate passed and enforcement is off for this path. Atlas records
     * what it would have done and does nothing. This is what shadow mode
     * produces, and it is a full result rather than a failure. */
    ATLAS_POLICY_SHADOW,
    /* A gate failed. `reason` says which. */
    ATLAS_POLICY_BLOCKED,
    /* No policy could ever allow this: a JUDGMENT transition, an ACCEPTED_RISK
     * approval, a normative claim reached by a deterministic verifier.
     * Distinct from BLOCKED because BLOCKED invites "add more evidence" and
     * this does not. */
    ATLAS_POLICY_FORBIDDEN
} atlas_verify_policy_verdict;

const char *atlas_verify_policy_verdict_name(atlas_verify_policy_verdict v);
bool atlas_verify_policy_verdict_parse(const char *name, atlas_verify_policy_verdict *out);

/* Why the policy engine said what it said. One closed vocabulary, so an
 * explanation is machine-readable rather than a sentence somebody wrote.
 *
 * A9.2 follows A6's rule here: the *verdict follows from the reason* rather
 * than being chosen beside it, so a reason added without deciding what it
 * implies cannot exist. See `REASONS[]` in `src/verify/verify.c`. */
typedef enum atlas_verify_reason {
    ATLAS_VREASON_NONE = 0,
    /* No root-owned verification policy is installed, or it is disabled. */
    ATLAS_VREASON_NO_POLICY,
    /* The policy has no rule allowing this kind and transition. */
    ATLAS_VREASON_NOT_ALLOWED,
    /* Enforcement is off for this basis; the gates otherwise passed. */
    ATLAS_VREASON_SHADOW_MODE,
    /* The claim is not verified. */
    ATLAS_VREASON_NOT_VERIFIED,
    /* Confidence below the policy threshold. */
    ATLAS_VREASON_LOW_CONFIDENCE,
    /* Fewer genuinely independent evidence groups than policy requires. */
    ATLAS_VREASON_INSUFFICIENT_INDEPENDENCE,
    /* Evidence older than policy allows for this kind. */
    ATLAS_VREASON_STALE_EVIDENCE,
    /* A blocking conflict exists. */
    ATLAS_VREASON_CONFLICT,
    /* The empirical path, with calibration that does not meet policy. Never
     * produced on the deterministic path — that is the point of the phase. */
    ATLAS_VREASON_CALIBRATION_INSUFFICIENT,
    /* The state machine does not permit this transition for this kind. */
    ATLAS_VREASON_TRANSITION_ILLEGAL,
    /* A JUDGMENT-basis claim. No policy enables these. */
    ATLAS_VREASON_JUDGMENT_REQUIRES_AUTHORITY,
    /* An ACCEPTED_RISK approval. Verifying that a risk exists, or that it has
     * been mitigated, establishes nothing about whether the project accepts
     * it — that is a decision with an owner. */
    ATLAS_VREASON_RISK_REQUIRES_AUTHORITY,
    /* A deterministic verifier reached a normative claim. Separation 4. */
    ATLAS_VREASON_NORMATIVE_CLAIM,
    /* A9.2.1, §5. The claim is bound to one repository state and the repository
     * is at another, so the evidence Atlas weighed describes a tree the
     * repository has since left.
     *
     * This is BLOCKED rather than NEEDS_REVIEW, and the distinction is the
     * whole point of the reason existing. NEEDS_REVIEW says the answer is
     * probably right and somebody should confirm it. What Atlas actually has
     * here is an answer about commit X being asked to justify a transition
     * about commit Y, and it has no basis at all for the second — the code the
     * claim describes may have been deleted between them. Treating a stale
     * mechanical proof as weak-but-usable evidence about the current tree is
     * precisely the silent republication §5 forbids.
     *
     * The result is still *published*, bound to the snapshot it examined, and
     * still readable: it remains true of commit X and saying so costs nothing.
     * What it may not do is move a lifecycle state. */
    ATLAS_VREASON_SOURCE_DRIFT,
    /* A9.2.2. The transition would rest on a proposition Atlas has not
     * established on the truth axis — the subject is neither shown present nor
     * shown absent, because a coverage dimension the conclusion depends on is
     * partial, stale or was never established.
     *
     * §18's requirement made explicit rather than left implicit. The gates
     * above already make it *structurally* hard to reach a transition on an
     * unestablished truth — a negative conclusion with insufficient coverage
     * comes back UNAVAILABLE from the verifier, which is not VERIFIED, which is
     * already BLOCKED. This reason exists so the rule is **checked** rather
     * than merely implied, and so the audit row says which of the two it was:
     * "the evidence was thin" and "Atlas never looked everywhere it would have
     * had to" are different findings and want different remedies.
     *
     * BLOCKED rather than NEEDS_REVIEW, for SOURCE_DRIFT's reason: this is not
     * an answer that is probably right and wants confirming. */
    ATLAS_VREASON_COVERAGE_INSUFFICIENT,
    /* Every gate passed. */
    ATLAS_VREASON_OK
} atlas_verify_reason;

const char *atlas_verify_reason_name(atlas_verify_reason r);
bool atlas_verify_reason_parse(const char *name, atlas_verify_reason *out);
/* One fixed Atlas-owned sentence per reason. No repository byte and no model
 * byte reaches it, which is why it may be reported to a model unencoded. */
const char *atlas_verify_reason_description(atlas_verify_reason r);
/* The verdict this reason implies, on its own. The engine folds these the way
 * A6 folds freshness — weakest wins — so a reason that does not fit in a
 * result's list still weakens the answer. */
atlas_verify_policy_verdict atlas_verify_reason_verdict(atlas_verify_reason r);
size_t atlas_verify_reason_count(void);
atlas_verify_reason atlas_verify_reason_at(size_t index);

/* Folds two verdicts to the weaker. FORBIDDEN absorbs everything, then
 * BLOCKED, then NEEDS_REVIEW, then SHADOW, and AUTO is the only value that can
 * be reached by nothing going wrong. */
atlas_verify_policy_verdict atlas_verify_verdict_fold(atlas_verify_policy_verdict a,
                                                      atlas_verify_policy_verdict b);

/* --- the aggregation algorithm -------------------------------------------
 *
 * Named and versioned, and stored on every result, so that a future algorithm
 * cannot silently reinterpret a past one. The same inputs produce the same
 * output: the arithmetic is integer throughout, with no floating point
 * anywhere, so "deterministic" is a property of the code rather than a hope
 * about rounding.
 *
 * V1 is deliberately simple and auditable rather than Bayesian. What it does:
 *
 *   - every attestation gets an integer weight from actor class, identity
 *     authenticity, declared reliability for the domain, freshness and whether
 *     its scope matches the claim's;
 *   - attestations are partitioned into independent evidence groups by
 *     union-find over declared derivation edges;
 *   - **within a group only the single strongest weight counts**, for support
 *     and for contradiction separately. This is the anti-inflation rule and it
 *     is why the algorithm cannot be fooled by repetition;
 *   - the score is `100 * support / (support + contradiction + PRIOR_MASS)`,
 *     where the prior mass is a fixed constant standing for "absence of
 *     evidence", so one weak attestation cannot reach certainty.
 *
 * What it deliberately does not do: majority vote, arithmetic means of
 * self-reported model confidence, or treating differently-named sources as
 * independent. Its limitations are written in `docs/verification.md`.
 *
 * V2 (A12.1, T5) adds nothing to the score, the state or the weights above —
 * it adds the first producer of `atlas_verify_aggregate.conflict`, an axis
 * that had been stored, transported and rendered since A9.2 without ever
 * being assigned. See `atlas_verify_conflict_settle`. The bump is required
 * because rule (3) there newly reports `ATLAS_CONFLICT_CONTRADICTION` for a
 * support/contradict shape V1 would have left at `CONFLICT_NONE`, and
 * `docs/extending.md` requires a new string whenever the aggregation changes
 * so a stored V1 result is never reinterpreted under V2's rules.
 *
 * **A consequence this bump has beyond the aggregation, noted rather than
 * designed around:** `src/verify/intake.c` also uses this constant as the
 * synthetic ATLAS_VERIFIER actor's `actor_version` and as produced evidence's
 * `tool_version`, and the actor uid is a hash that includes `actor_version`
 * (`derive_actor`). So this bump mints a *new* ATLAS_VERIFIER actor row rather
 * than reusing the one V1 wrote to, and every attestation and reliability
 * figure accumulated under the old actor stays where it is, addressed by the
 * old uid. On a machine with no calibrated history yet — every deployment
 * today — this costs nothing observable. It is still a coupling nobody chose
 * on purpose: the algorithm version is not otherwise a property of *who*
 * produced deterministic evidence, only of *how the aggregation reads it*,
 * and conflating the two is a candidate for the backlog rather than for this
 * fix. */
#define ATLAS_VERIFY_ALGORITHM "atlas-reliability-v2"

/* The fixed-point scale for internal weights. Weights are integers in units of
 * 1/1000, so a "full strength" attestation is 1000. Nothing here is a
 * probability and none of it is exposed as one. */
#define ATLAS_VERIFY_WEIGHT_SCALE 1000

/* The absence-of-evidence mass, in the same units. One perfectly weighted
 * supporting attestation from a single group therefore yields 1000/(1000+250)
 * = 80, not 100: a lone source, however good, is not certainty. Reaching a high
 * score needs several genuinely independent groups, which is the behaviour the
 * whole phase is for. */
#define ATLAS_VERIFY_PRIOR_MASS 250

/* --- structures ----------------------------------------------------------- */

/* One discrete proposition.
 *
 * Claims are verification objects attached to knowledge, not a second knowledge
 * store. A claim has no lifecycle, no approval and no revisions: it says one
 * thing, it is bound to the artifacts that make it checkable, and evidence
 * accumulates against it. The knowledge record remains the canonical statement
 * of what the project believes; the claim is how Atlas checks up on it.
 *
 * Small claims verify and large ones do not. `require_peer_auth defaults to
 * false at commit abc` is checkable; `the protocol is secure` is not a claim,
 * it is a topic. */
typedef struct atlas_verify_claim {
    int64_t id;
    atlas_buf uid; /* `atlas-claim-<32 hex>`, stable and public */
    int64_t repo_id;
    atlas_buf repo_identity_hash; /* durable, survives re-registration */

    /* The knowledge record and the exact revision this claim is about. A
     * revision rather than a document, because §64: an old revision of an
     * inflation schedule and the current one are different propositions and
     * must not share a verdict. 0 for a claim not yet attached. */
    int64_t document_id;
    int64_t revision_id;

    /* A short fixed-vocabulary label for the area of knowledge, used to key
     * reliability. Free text from a bounded charset rather than an enum,
     * because the useful partition is project-specific and an enum would make
     * every project use somebody else's. Validated to be short, printable and
     * lowercase — never rendered as control bytes. */
    atlas_buf domain;

    /* The proposition, and the scope that bounds it. Both UNTRUSTED_DATA
     * wherever they are reported: a claim's text is written by whoever wrote
     * it, and approval never changes the nature of bytes. */
    atlas_buf text;
    atlas_buf scope_note;

    atlas_verify_claim_semantics semantics;

    /* What makes it mechanically checkable, when it is. The verifier is named
     * from a closed Atlas-owned table and is chosen by root-owned policy, never
     * by a model: `verifier` here records which one applies, and
     * `verifier_input` its bounded, structured argument. Empty means no
     * deterministic verifier applies and the claim is empirical at best. */
    atlas_buf verifier;
    atlas_buf verifier_input;

    /* Temporal and revision binding. What the claim is true *of*. */
    atlas_buf basis_commit;
    atlas_buf environment; /* deployment identity, empty for repository claims */

    atlas_buf created_at;
    int64_t superseded_by_claim_id; /* 0 when live */

    /* A9.2.1. The deterministic identity of this claim's immutable content,
     * scoped by repository, document revision and basis commit. Empty for a
     * claim written before A9.2.1, and the UNIQUE index is partial for that
     * reason. §27: a retry resolves to the existing row rather than creating a
     * second proposition that would be counted beside the first. */
    atlas_buf content_key;
    /* §3. Which actor created it. 0 for a claim written before the column
     * existed — honestly absent rather than attributed to a guess. */
    int64_t created_by_actor_id;
} atlas_verify_claim;

void atlas_verify_claim_init(atlas_verify_claim *c);
void atlas_verify_claim_free(atlas_verify_claim *c);

/* One actor. See the two identity enums above: `identity` is how well Atlas
 * knows this is who it says, and it is not something the actor supplies. */
typedef struct atlas_verify_actor {
    int64_t id;
    atlas_buf uid;
    atlas_verify_actor_class cls;
    atlas_verify_actor_identity identity;

    atlas_buf name;     /* display identity, UNTRUSTED_DATA */
    atlas_buf provider; /* AI: vendor, self-declared */
    atlas_buf family;   /* AI: model family */
    atlas_buf version;  /* AI: model version; TOOL: tool version */
    atlas_buf role;
    atlas_buf session_key; /* AI: the session it spoke in */
    atlas_buf run_id;
    int64_t parent_actor_id; /* orchestrator, 0 for none */

    atlas_buf first_seen_at;
    atlas_buf last_seen_at;
} atlas_verify_actor;

void atlas_verify_actor_init(atlas_verify_actor *a);
void atlas_verify_actor_free(atlas_verify_actor *a);

/* One piece of evidence, with the provenance that makes it checkable.
 *
 * The fields are a union in spirit rather than in C: which ones are meaningful
 * depends on `cls`, and the ones that are not are empty. A struct rather than a
 * blob because "where did this come from?" must be queryable, and a JSON
 * document in a column is a place for provenance to go missing. */
typedef struct atlas_verify_evidence {
    int64_t id;
    atlas_buf uid;
    atlas_verify_evidence_class cls;
    int64_t repo_id;

    /* Repository binding: commit, path, symbol, line range, content identity. */
    atlas_buf commit_oid;
    atlas_buf path_raw;
    atlas_buf path_text;
    atlas_buf symbol;
    int64_t line_start;
    int64_t line_end;
    atlas_buf content_hash;

    /* TEST: suite, name, result, binary identity, environment. */
    atlas_buf suite;
    atlas_buf test_name;
    atlas_buf result;
    atlas_buf binary_id;
    atlas_buf environment;

    /* COMPILER: the A8-CI evidence class this finding carries, so PROVEN and
     * CANDIDATE stay distinguishable rather than both becoming "the compiler
     * said so". */
    atlas_buf tool;
    atlas_buf tool_version;
    atlas_buf proof_class;

    /* RUNTIME / DEPLOYED_CONFIG: what was probed and what came back. */
    atlas_buf target;
    atlas_buf probe;
    atlas_buf observed;
    atlas_buf deployed_revision;

    /* When the evidence describes, which is not when the row was written. */
    atlas_buf observed_at;
    atlas_buf recorded_at;

    /* Who produced it. An Atlas-attested actor for anything Atlas ran. */
    int64_t actor_id;

    /* A9.2.1, §27. Deterministic identity over the immutable reference: the
     * class, the repository, the commit, the path, the symbol, the range and —
     * for a dynamic observation — the instant. The commit is in the key because
     * the same lines at a different revision are a different fact, and merging
     * them would let last week's reading corroborate this week's claim. */
    atlas_buf content_key;
    /* A9.2.1, §30. Which A8-CI semantic generation produced this, for the
     * classes where that is the difference between current and merely recent.
     * 0 when no generation was involved. Evidence that did not record its
     * generation would be silently reinterpreted as current the next time the
     * index was rebuilt. */
    int64_t sem_generation;
} atlas_verify_evidence;

void atlas_verify_evidence_init(atlas_verify_evidence *e);
void atlas_verify_evidence_free(atlas_verify_evidence *e);

/* One actor's verdict on one claim at one moment.
 *
 * **Never overwritten.** An actor that changes its mind writes a second
 * attestation naming the first in `supersedes_id`; both stay readable, because
 * "this source reversed itself" is exactly the kind of fact a reliability
 * system must be able to see. */
typedef struct atlas_verify_attestation {
    int64_t id;
    atlas_buf uid;
    int64_t claim_id;
    int64_t actor_id;
    atlas_verify_verdict verdict;

    /* What the actor says about its own confidence, 0..100, or -1 for none.
     * Stored, reported, and **never** used directly as Atlas' confidence. A
     * source's opinion of itself is data about the source. */
    int self_confidence;

    /* How the actor reached the verdict. Free text, bounded, UNTRUSTED_DATA. */
    atlas_buf method;
    /* The scope the actor says it examined, compared against the claim's. */
    atlas_buf scope_note;

    atlas_buf created_at;
    int64_t supersedes_id;   /* 0 for none */
    bool proposer;           /* this actor proposed the record; §26 */
    atlas_buf basis_commit;  /* the revision the actor examined */
    atlas_buf environment;

    /* A9.2.1, §27/§28. Deterministic identity over claim, actor, verdict,
     * method, scope and examined commit. One actor repeating itself is one
     * attestation; the same actor reaching the same verdict about a *different*
     * commit is a second, because it examined a different tree.
     *
     * This bounds replay, and it does so without suppressing a genuine change
     * of mind: a different verdict is a different key, so it lands as the new
     * row `supersedes_id` is designed to carry. */
    atlas_buf content_key;
} atlas_verify_attestation;

void atlas_verify_attestation_init(atlas_verify_attestation *a);
void atlas_verify_attestation_free(atlas_verify_attestation *a);

/* --- aggregation ---------------------------------------------------------- */

/* Everything the aggregation concluded, and enough to explain every part of it.
 *
 * §25: a user must be able to ask "why 96?" and get structured reasons. That is
 * why this is a struct of counted facts rather than a number and a sentence.
 * There is no prose explanation field at all — an LLM-written justification of
 * a machine verdict is the one thing that must not be the explanation. */
typedef struct atlas_verify_aggregate {
    atlas_verify_state state;
    atlas_verify_basis basis;

    /* 0..100. A **score**, not a probability, and the JSON key is
     * `confidence_score` precisely so nobody can read it as one. */
    int confidence;

    atlas_verify_calibration calibration;
    /* 0..100, and meaningful only when `calibration == CALIBRATED`. -1
     * otherwise, which is what every read on this machine returns today. A
     * separate field from `confidence` rather than a flag on it, because two
     * things that must never be confused should not share a slot. */
    int calibrated_probability;

    const char *algorithm; /* ATLAS_VERIFY_ALGORITHM */
    int family_version;

    int support_count;     /* attestations, not groups */
    int contradict_count;
    int inconclusive_count;
    int independent_groups;  /* union-find components; the honest count */
    int independent_families;
    int evidence_count;
    int actor_count;

    /* Support and contradiction mass in ATLAS_VERIFY_WEIGHT_SCALE units, after
     * per-group collapse. Exposed because they are what the score is computed
     * from and an explanation that omits them is not one. */
    int64_t support_mass;
    int64_t contradict_mass;

    atlas_verify_conflict conflict;
    bool stale;
    /* The strongest deterministic verdict present, if any. A deterministic
     * PASS is not a vote among others: it settles the claim. */
    bool deterministic_pass;
    bool deterministic_fail;

    /* Reasons, weakest-wins folded. */
    atlas_verify_policy_verdict verdict;
    atlas_verify_reason reasons[ATLAS_VERIFY_MAX_REASONS];
    size_t reason_count;
    size_t reason_total; /* the true number, even when more than fit */
} atlas_verify_aggregate;

void atlas_verify_aggregate_init(atlas_verify_aggregate *a);

/* Records a reason, folding the verdict before storing it.
 *
 * A6's rule exactly: the fold happens first, so a reason that does not fit in
 * the list still weakens the answer and a claim with thirteen problems cannot
 * report a better verdict than one with twelve. */
void atlas_verify_aggregate_note(atlas_verify_aggregate *a, atlas_verify_reason r);

/* One attestation reduced to what the aggregation needs. Built by the db layer
 * so the algorithm is a pure function of counted inputs and can be tested
 * without a database. */
typedef struct atlas_verify_input {
    int64_t attestation_id;
    int64_t actor_id;
    atlas_verify_actor_class actor_class;
    atlas_verify_actor_identity actor_identity;
    atlas_verify_verdict verdict;
    bool proposer;
    bool scope_match;
    bool stale;
    /* The evidence group this attestation lands in, assigned by
     * `atlas_verify_independent_groups`. */
    int group;
    atlas_verify_evidence_family family;
    /* Declared reliability for this actor and domain in weight units, or -1
     * when there is no calibrated estimate — which is the case for everything
     * on a fresh install, and is handled by falling back to the documented
     * conservative prior rather than to an invented number. */
    int reliability;
} atlas_verify_input;

/* Partitions inputs into independent evidence groups.
 *
 * Union-find over declared derivation edges, plus the conservative rules that
 * make it safe:
 *
 *   - evidence connected by any derivation edge is one group;
 *   - INTERPRETATION-family evidence declaring no roots joins **one shared
 *     group**, not one group each. This is what makes three models reading one
 *     document count once even when none of them declared the document;
 *   - an attestation with no evidence at all joins that same group.
 *
 * Independence is never assumed. If it cannot be demonstrated from declared
 * structure, the evidence is treated as correlated — which costs confidence and
 * never manufactures it. Writes `group` on each input and returns the number of
 * components. */
int atlas_verify_independent_groups(atlas_verify_input *inputs, size_t count,
                                    const int64_t *dep_from, const int64_t *dep_to,
                                    size_t dep_count);

/* The algorithm. Pure, integer, deterministic; the same inputs always produce
 * the same aggregate. */
void atlas_verify_aggregate_compute(atlas_verify_aggregate *out, atlas_verify_input *inputs,
                                    size_t count, atlas_verify_basis basis);

/* --- the conflict axis's one producer --------------------------------------
 *
 * A12.1, T5. `atlas_verify_aggregate.conflict` (the vocabulary is above, with
 * `atlas_verify_conflict_name`/`_parse`) had no producer from A9.2 through
 * A12.0: every stored, transported and rendered `conflict` was the constant
 * `ATLAS_CONFLICT_NONE`. This is the first one, and — like
 * `atlas_verify_aggregate_compute` beside it — it is pure: no database handle,
 * no clock, no process, no file, no repository prose compared, and it does not
 * touch the decision it reads about. `atlas_verify_assess` is its only caller,
 * with `decision_bound` and `decision_effective` derived there from stored
 * rows: the claim's own `document_id` resolving to a decision-lifecycle
 * document, and `atlas_db_decision_approved_revision` finding an approved
 * revision of it.
 *
 * Rules, in order — first match wins, because (1) and (2) can both hold and
 * (1) is the more specific finding:
 *
 *   1. deterministic_fail && decision_bound && decision_effective
 *        -> ATLAS_CONFLICT_IMPLEMENTATION. Atlas mechanically established that
 *           the implementation-side fact is false while the named decision's
 *           effective approved revision stands. This is "approved knowledge
 *           says one thing and the implementation does another" exactly, and
 *           it is a finding *against the implementation* — see the comment on
 *           `ATLAS_CONFLICT_IMPLEMENTATION` above, which this must not
 *           weaken: the decision's status and its effective revision are
 *           never touched by this function or by its caller.
 *   2. deterministic_fail && support_count > 0
 *        -> ATLAS_CONFLICT_CONTRADICTION. A mechanical fail against evidence
 *           that supported the claim, with no decision in the picture (or one
 *           that is bound but no longer effective — rule 1 already covers the
 *           case where it is).
 *   3. support_count > 0 && contradict_count > 0
 *        -> ATLAS_CONFLICT_CONTRADICTION. Ordinary attestation disagreement,
 *           independent of any deterministic verdict.
 *   4. otherwise -> ATLAS_CONFLICT_NONE.
 *
 * The four conflicts this leaves without a producer — SUPERSESSION,
 * SCOPE_MISMATCH, STALE_EVIDENCE, COMPETING_NORMATIVE — are exactly that: a
 * documented gap, not something this function reaches for on the way past. */
atlas_verify_conflict atlas_verify_conflict_settle(const atlas_verify_aggregate *a,
                                                   bool decision_bound, bool decision_effective);

/* --- conservative priors --------------------------------------------------
 *
 * §20. Explicit, documented, versioned, vendor-neutral, and dominated by
 * empirical outcomes once there are any.
 *
 * Nothing here is `human = 100` or `model X = 95`. The prior is a statement
 * about *what sort of thing is speaking and how well Atlas knows it did*, which
 * is the only thing Atlas actually observes on a fresh install. A human's
 * factual recollection is not weighted above a compiler's proof, because on
 * questions of fact it should not be. */
int atlas_verify_prior_reliability(atlas_verify_actor_class cls,
                                   atlas_verify_actor_identity identity);
#define ATLAS_VERIFY_PRIOR_VERSION 1

/* --- eligibility for calibration -----------------------------------------
 *
 * §45 and §65: which resolved outcomes may become reliability labels.
 *
 * The forbidden loop is short and easy to build by accident: a model supports a
 * claim, the aggregate likes it, Atlas auto-approves, the approval is counted
 * as ground truth, the model's reliability rises, and the next claim clears the
 * bar more easily. Every step looks reasonable and the result is a system that
 * has taught itself to trust a source using that source's own output.
 *
 * So ground truth must come from resolution classes that do not depend on the
 * aggregation: a deterministic verifier's pass or fail, an operator's explicit
 * resolution through the terminal channel, or an observed outcome recorded by
 * an Atlas-attested actor. An empirical auto-transition is **not** eligible,
 * and neither is a machine transition on any basis, which is what breaks the
 * loop structurally rather than by a rule somebody has to remember. */
typedef enum atlas_verify_outcome_source {
    ATLAS_OUTCOME_UNKNOWN = 0,
    ATLAS_OUTCOME_DETERMINISTIC_VERIFIER,
    ATLAS_OUTCOME_OPERATOR_RESOLUTION,
    ATLAS_OUTCOME_RUNTIME_OBSERVATION,
    /* Recorded so the ineligible case is representable and auditable rather
     * than absent. Never counted. */
    ATLAS_OUTCOME_MACHINE_TRANSITION
} atlas_verify_outcome_source;

const char *atlas_verify_outcome_source_name(atlas_verify_outcome_source s);
/* Whether an outcome from this source may update a source's reliability.
 * False for MACHINE_TRANSITION and UNKNOWN. */
bool atlas_verify_outcome_eligible(atlas_verify_outcome_source s);

/* --- deterministic verifiers ---------------------------------------------
 *
 * A closed, Atlas-owned table. A verifier is named by root-owned policy and by
 * nothing else: not by a claim's text, not by a model, not by a request
 * argument. That is what makes "a model cannot forge deterministic evidence" a
 * property of the surface rather than a check somebody could weaken.
 *
 * V1 ships four, and every one is a **read**. None creates a process, none runs
 * a repository's own build, and none executes a command from anywhere. That is
 * a deliberate restriction rather than an unfinished one: a verifier that runs
 * a command named in configuration is a code-execution path with an audit trail
 * attached, and the argument for adding one belongs to whoever needs it, in
 * writing, with the sandbox already built. `docs/verification.md` says so and
 * names the bounded-child pattern A8-CI already has for when that day comes. */
typedef enum atlas_verify_verifier {
    ATLAS_VERIFIER_NONE = 0,
    /* The content of a repository path at a bound commit hashes to a stated
     * value. Establishes exactly "these bytes were there", which is a small
     * claim and a completely mechanical one. */
    ATLAS_VERIFIER_CONTENT_HASH,
    /* A named symbol exists in the current semantic generation, with the
     * A8-CI evidence class the index recorded. PROVEN and CANDIDATE stay
     * distinct: a candidate never satisfies a deterministic verifier. */
    ATLAS_VERIFIER_SYMBOL_PRESENT,
    /* No symbol of a given name exists. The remediation detector: an
     * obligation whose condition is "this must be gone" is discharged by its
     * absence, and absence in a *complete* generation is a mechanical fact.
     * A PARTIAL generation cannot establish it, and saying so is the whole
     * difficulty — see the implementation. */
    ATLAS_VERIFIER_SYMBOL_ABSENT,
    /* A direct call edge the compiler proved exists between two symbols. */
    ATLAS_VERIFIER_PROVEN_EDGE,
    /* A9.2.2. No caller reaches a symbol.
     *
     * The one verifier whose *whole purpose* is a negative conclusion, which is
     * why it is the one that needed the coverage model before it could exist.
     * "Nothing calls X" cannot be established from an empty result set: a
     * translation unit that failed to parse may hold the call, a function
     * pointer may reach it, and an external-linkage symbol may be called from
     * code Atlas never indexed.
     *
     * So it is bounded by three mechanical questions rather than by a caveat —
     * see `atlas_verify_verifier_absence_dims` and the implementation. Every
     * one is a read over `sem_edges` and `sem_symbols`. */
    ATLAS_VERIFIER_NO_PROVEN_CALLER
} atlas_verify_verifier;

const char *atlas_verify_verifier_name(atlas_verify_verifier v);
bool atlas_verify_verifier_parse(const char *name, atlas_verify_verifier *out);
const char *atlas_verify_verifier_description(atlas_verify_verifier v);
size_t atlas_verify_verifier_count(void);
atlas_verify_verifier atlas_verify_verifier_at(size_t index);

/* What a deterministic verifier concluded.
 *
 * `PASS` and `FAIL` are both results. `UNAVAILABLE` is not a fail: an index
 * that has not run cannot establish absence, and reporting "could not look" as
 * "it is not there" is how a remediation detector closes an obligation that is
 * still outstanding. */
typedef enum atlas_verify_check {
    ATLAS_CHECK_UNAVAILABLE = 0,
    ATLAS_CHECK_PASS,
    ATLAS_CHECK_FAIL
} atlas_verify_check;

const char *atlas_verify_check_name(atlas_verify_check c);
bool atlas_verify_check_parse(const char *name, atlas_verify_check *out);

/* ==========================================================================
 * A9.2.2 — epistemic absence and coverage semantics
 *
 * The invariant this section exists to make structural:
 *
 *   **NO EVIDENCE OF X IS NOT EVIDENCE OF NO X.**
 *
 * and its operational half:
 *
 *   **ABSENCE requires positive proof that the observation coverage is
 *   sufficient for the bounded claim.** Where coverage is insufficient,
 *   incomplete, stale, unsupported or unknown, the answer is UNKNOWN — never
 *   ABSENT.
 *
 * ## Why a fourth axis rather than a wider `atlas_verify_check`
 *
 * `atlas_verify_check` answers "was this verifier's truth condition met?", and
 * that is not the same question as "does the thing exist?". PASS means *absent*
 * for `atlas.symbol_absent` and *present* for `atlas.symbol_present`, so a
 * reader holding a result cannot say which without knowing which verifier ran
 * and inverting by hand. Every surface that reported a check therefore reported
 * something a person or a model had to decode, and decoding it wrongly in the
 * safe-looking direction is exactly the failure this season is about.
 *
 * So Atlas now carries four orthogonal axes, and **no code path derives any one
 * of them from another**:
 *
 *   kind     — what sort of knowledge is this?          (A9.1, on the document)
 *   status   — how far through approval did it get?     (A4, from the ledger)
 *   verify   — what evidence bears on whether it holds? (A9.2, derived on read)
 *   truth    — is the thing there, not there, or unknown? (A9.2.2, derived)
 *
 * ## The asymmetry, stated once
 *
 * Positive evidence needs less coverage than negative evidence, and this is not
 * a convenience — it is the shape of the world. Finding one caller proves a
 * caller exists however incomplete the index, because an incomplete index
 * cannot conjure a call that is not there. Finding zero callers proves nothing
 * at all unless Atlas can show it looked everywhere a caller could have been.
 *
 * One direction is monotone in coverage and the other is not. Everything below
 * follows from that sentence.
 * ========================================================================== */

/* What Atlas knows about whether the proposition's subject is there.
 *
 * **UNKNOWN is zero**, which is the house rule (A6's UNKNOWN, A8's DISABLED,
 * A9.2's UNVERIFIED) and here it is load-bearing rather than conventional: a
 * zeroed struct, an absent column and a row written before this vocabulary
 * existed must all read as "Atlas has not established this". A zero that meant
 * ABSENT would make `memset` assert non-existence. */
typedef enum atlas_verify_truth {
    /* Epistemic uncertainty. **Not a negative fact**, and every consumer —
     * including a future A10 — must treat it as "Atlas has not established
     * this", never as "no". This is the value a zero-result search produces,
     * and producing it is the whole point of the season. */
    ATLAS_TRUTH_UNKNOWN = 0,
    /* Established to be there, within the claim's declared scope. */
    ATLAS_TRUTH_PRESENT,
    /* Established **not** to be there, within the claim's declared scope, with
     * every coverage dimension the verifier requires for a negative shown to be
     * sufficient. Reachable through exactly one function — see
     * `atlas_verify_truth_of` — and through no caller, transport or intake
     * parameter. */
    ATLAS_TRUTH_ABSENT,
    /* Not a bounded factual question at all: a normative choice, or a judgment.
     * Distinct from UNKNOWN on purpose and this is §13's requirement rather
     * than a nicety — UNKNOWN says "more evidence would settle it", and for
     * "architecture A will be the best design in 2030" no evidence would.
     * Reporting a normative proposition as UNKNOWN invites somebody to go and
     * look, which is a category error dressed as diligence.
     *
     * Derived from `semantics == NORMATIVE` or `basis == JUDGMENT` rather than
     * stored independently: it is a projection of facts Atlas already holds
     * onto this axis, not a fifth axis. */
    ATLAS_TRUTH_NOT_VERIFIABLE
} atlas_verify_truth;

const char *atlas_verify_truth_name(atlas_verify_truth t);
bool atlas_verify_truth_parse(const char *name, atlas_verify_truth *out);
/* Whether this value asserts something about the world. False for UNKNOWN and
 * NOT_VERIFIABLE. Asked by the policy layer so "UNKNOWN must never satisfy a
 * condition requiring ABSENT" is one check rather than a rule each rule
 * remembers. */
bool atlas_verify_truth_is_established(atlas_verify_truth t);

/* --- coverage -------------------------------------------------------------
 *
 * How much of what would have to be looked at was actually looked at, per
 * dimension. **Never a percentage.** A denominator Atlas cannot state is a
 * denominator that makes a number up, and `coverage = 87%` reads as precision
 * about exactly the thing that is unknown. A small closed vocabulary an auditor
 * can check beats a figure nobody can reproduce.
 *
 * UNKNOWN is zero: a dimension nobody established is not a satisfied one. */
typedef enum atlas_verify_coverage {
    /* Nothing established this dimension. The default, and insufficient. */
    ATLAS_COVERAGE_UNKNOWN = 0,
    /* Everything in scope for this dimension was observed. */
    ATLAS_COVERAGE_COMPLETE,
    /* Some of it was observed and Atlas can say that some was not. */
    ATLAS_COVERAGE_PARTIAL,
    /* It was observed, and what was observed no longer describes the present.
     * Distinct from PARTIAL because the remedy differs: PARTIAL needs a wider
     * look, STALE needs a fresh one. */
    ATLAS_COVERAGE_STALE,
    /* This claim cannot depend on this dimension, so there is nothing to
     * cover. The only value besides COMPLETE that is *sufficient*, and it must
     * be asserted from a mechanical fact rather than assumed — an unconsidered
     * dimension is UNKNOWN, not NOT_APPLICABLE. */
    ATLAS_COVERAGE_NOT_APPLICABLE
} atlas_verify_coverage;

const char *atlas_verify_coverage_name(atlas_verify_coverage c);
bool atlas_verify_coverage_parse(const char *name, atlas_verify_coverage *out);
/* Whether this state permits a negative conclusion to rest on this dimension.
 * True for COMPLETE and NOT_APPLICABLE and for nothing else — in particular
 * **not** for UNKNOWN, which is the whole invariant in one line. */
bool atlas_verify_coverage_sufficient(atlas_verify_coverage c);

/* The dimensions a negative conclusion can turn on.
 *
 * Explicit and closed, because §4 requires a reader to be able to ask *which*
 * part of the looking was incomplete. A single opaque "coverage: partial" tells
 * somebody that something was missed without telling them what, which is not
 * enough to act on and not enough to audit. */
typedef enum atlas_verify_coverage_dim {
    /* A semantic generation is published, COMPLETE (no failed, partial or
     * unsupported translation units) and describes the current commit. */
    ATLAS_COVDIM_SEMANTIC_GENERATION = 0,
    /* The file index describes the working tree as it is now. Separate from
     * SOURCE_DRIFT, which compares the claim's commit against the scanned head;
     * this compares the scanned head against what is on disk. */
    ATLAS_COVDIM_REPOSITORY_SNAPSHOT,
    /* Every tracked source file in scope was read. */
    ATLAS_COVDIM_TRACKED_SOURCE,
    /* Build-generated sources were included. Generated code is where a symbol
     * hides from a reader who only grepped the tree. */
    ATLAS_COVDIM_GENERATED_SOURCE,
    /* Every compiler-proved direct call edge in scope is recorded. */
    ATLAS_COVDIM_DIRECT_CALLS,
    /* Calls through function pointers, callbacks, dispatch tables and dynamic
     * registration are accounted for. §9: direct-call completeness is **not**
     * sufficient for "no caller reaches X", and this dimension is why that is a
     * structural fact rather than a warning in a comment. */
    ATLAS_COVDIM_INDIRECT_CALLS,
    /* Callers outside the indexed repository, and reachability through dynamic
     * symbol lookup, are excluded. Decidable for an internal-linkage symbol and
     * not for an external-linkage one. */
    ATLAS_COVDIM_EXTERNAL_CALLERS,
    /* Test sources were included in scope. */
    ATLAS_COVDIM_TESTS,
    /* The document corpus a documentation claim is bounded by was enumerated.
     * §12: searching three Markdown files establishes nothing project-wide
     * unless those three files *are* the declared corpus. */
    ATLAS_COVDIM_DOCUMENT_CORPUS,
    /* The running system was observed. §11: repository absence is not
     * operational absence, and Atlas has no runtime probe, so this is UNKNOWN
     * for every claim that needs it — which makes such claims UNKNOWN, which is
     * the correct fail-closed answer. */
    ATLAS_COVDIM_RUNTIME_STATE,
    /* Deployed configuration was read. Unavailable for the same reason. */
    ATLAS_COVDIM_DEPLOYED_CONFIG,
    /* A9.2.4. Every compilation database relevant to this repository was found.
     *
     * The dimension underneath every other semantic one, and the one A9.2.3
     * could not express:
     *
     *   **COMPLETE PROCESSING OF CONFIGURED INPUTS DOES NOT PROVE COMPLETE
     *   DISCOVERY OF RELEVANT INPUTS.**
     *
     * `TRACKED_SOURCE` says every source in scope was read; the scope is
     * whatever the accepted compilation databases named, and this says whether
     * Atlas can account for having found them all. Two databases reporting
     * 200/200 and 216/216 establish nothing about whether a third exists — and
     * on the repository that produced this season, a third did.
     *
     * UNKNOWN whenever discovery has not run or was pinned by hand, PARTIAL
     * whenever the bounded walk stopped early or an operator excluded a subtree.
     * Both refuse an absence, which is the whole point of adding it. */
    ATLAS_COVDIM_BUILD_INPUT_DISCOVERY
} atlas_verify_coverage_dim;

#define ATLAS_VERIFY_COVERAGE_DIMS 12

const char *atlas_verify_coverage_dim_name(atlas_verify_coverage_dim d);
bool atlas_verify_coverage_dim_parse(const char *name, atlas_verify_coverage_dim *out);
const char *atlas_verify_coverage_dim_description(atlas_verify_coverage_dim d);

/* Why the truth axis says what it says.
 *
 * A closed vocabulary rather than prose, because §22 requires a model to be able
 * to ask "why is this UNKNOWN?" and receive something it can branch on. A
 * sentence is not an answer a program can act on, and an LLM-written
 * justification of a machine verdict is the one explanation that must not be
 * the explanation.
 *
 * Kept **separate from `atlas_verify_reason`** on purpose. That vocabulary
 * explains a *policy* verdict and each member carries the verdict it implies;
 * this one explains a *truth* value. Merging them would have the two axes this
 * season exists to separate sharing a field. */
typedef enum atlas_verify_truth_reason {
    ATLAS_TREASON_NONE = 0,
    /* The verifier evaluated its truth condition and coverage sufficed. */
    ATLAS_TREASON_ESTABLISHED,
    /* No deterministic verifier ran, so nothing was mechanically evaluated. */
    ATLAS_TREASON_NOT_EVALUATED,
    /* A required dimension is PARTIAL. */
    ATLAS_TREASON_COVERAGE_PARTIAL,
    /* A required dimension was never established at all. */
    ATLAS_TREASON_COVERAGE_UNKNOWN,
    /* The semantic index does not describe the current commit. */
    ATLAS_TREASON_SEMANTIC_INDEX_STALE,
    /* No semantic generation is published; Atlas could not look. */
    ATLAS_TREASON_SEMANTIC_INDEX_ABSENT,
    /* The generation has failed, partial or unsupported translation units, so a
     * symbol may live in a file that never parsed. */
    ATLAS_TREASON_SEMANTIC_INDEX_INCOMPLETE,
    /* The symbol's address is taken, so a call through a pointer, a dispatch
     * table or a dynamic registration cannot be ruled out. */
    ATLAS_TREASON_INDIRECT_CALLS_UNRESOLVED,
    /* External linkage: callers outside the indexed repository, and dynamic
     * symbol lookup, are not observable from here. */
    ATLAS_TREASON_EXTERNAL_CALLERS_POSSIBLE,
    /* §11. The running system was not observed. */
    ATLAS_TREASON_RUNTIME_NOT_OBSERVED,
    /* §11. Deployed configuration was not available. */
    ATLAS_TREASON_DEPLOYED_CONFIG_UNAVAILABLE,
    /* The file index does not describe the working tree. */
    ATLAS_TREASON_REPOSITORY_SNAPSHOT_STALE,
    /* A9.2.1 §5. The claim is bound to one commit and the index to another, so
     * the evaluation is about a tree the repository has left. */
    ATLAS_TREASON_SOURCE_DRIFT,
    /* The claim declares no bound, so there is no set over which an absence
     * could be complete. */
    ATLAS_TREASON_SCOPE_UNBOUNDED,
    /* Normative or judgment: not a factual question. */
    ATLAS_TREASON_NOT_FACTUAL,
    /* §15. An empirical basis never establishes PRESENT or ABSENT however high
     * the score. Five agents failing to find X is not absence; it is five
     * failures to find. */
    ATLAS_TREASON_EMPIRICAL_BASIS
} atlas_verify_truth_reason;

const char *atlas_verify_truth_reason_name(atlas_verify_truth_reason r);
bool atlas_verify_truth_reason_parse(const char *name, atlas_verify_truth_reason *out);
/* One fixed Atlas-owned sentence per reason. No repository byte and no model
 * byte reaches it, so it may be reported to a model unencoded. */
const char *atlas_verify_truth_reason_description(atlas_verify_truth_reason r);
size_t atlas_verify_truth_reason_count(void);
atlas_verify_truth_reason atlas_verify_truth_reason_at(size_t index);

/* What was looked at, per dimension, plus why the answer came out as it did.
 *
 * Derived server-side from index state and from nothing a caller supplied.
 * There is deliberately **no intake path that can set a dimension**: a model
 * may state a hypothesis and may not manufacture the coverage that would make
 * it an absence proof. §29's adversarial requirement is met by the absence of a
 * parameter rather than by a check on one. */
typedef struct atlas_verify_coverage_report {
    atlas_verify_coverage dims[ATLAS_VERIFY_COVERAGE_DIMS];
    atlas_verify_truth_reason reason;
} atlas_verify_coverage_report;

/* Zeroes every dimension to UNKNOWN, which is the safe default and the one a
 * `memset` would produce anyway. */
void atlas_verify_coverage_report_init(atlas_verify_coverage_report *r);

/* Whether every listed dimension is sufficient.
 *
 * `failed_out` receives the first insufficient dimension and `why_out` the
 * reason it implies, so a caller reports *which* part of the looking fell short
 * rather than that some part did. */
bool atlas_verify_coverage_satisfies(const atlas_verify_coverage_report *r,
                                     const atlas_verify_coverage_dim *dims, size_t count,
                                     atlas_verify_coverage_dim *failed_out,
                                     atlas_verify_truth_reason *why_out);

/* One value for display: the weakest state among the dimensions **this
 * verifier's conclusion actually depends on**.
 *
 * Folding over *every* dimension would be the obvious implementation and is
 * wrong in a way that shows up immediately in a UI. No verifier observes a
 * running system, so `runtime_state` and `deployed_config` are UNKNOWN for all
 * of them — and an all-dimensions fold is therefore UNKNOWN for every claim
 * Atlas can answer, including the ones it answered completely. A reader then
 * sees `truth: ABSENT` beside `coverage: UNKNOWN` and has to work out that the
 * two are not in conflict, which is exactly the confusion this season exists to
 * remove.
 *
 * So the summary answers the question a reader is actually asking: *was the
 * coverage this conclusion rested on sufficient?* The per-dimension map is
 * still reported in full beside it, so nothing is hidden — a dimension that was
 * never established is visible there whether or not this claim needed it.
 *
 * A summary only. Never an input to `atlas_verify_truth_of`, which asks the
 * dimensions directly: a claim must not be blocked by a dimension it does not
 * depend on, and must not be let through because an irrelevant one happened to
 * be complete. */
atlas_verify_coverage atlas_verify_coverage_summary(const atlas_verify_coverage_report *r,
                                                    atlas_verify_verifier v);

/* Renders `dim=STATE;dim=STATE` from the two closed vocabularies. Every byte is
 * Atlas-owned, so the result is safe to store, relay and print unencoded.
 * Returns the number of bytes written, excluding the terminator. */
size_t atlas_verify_coverage_render(const atlas_verify_coverage_report *r, char *out,
                                    size_t out_size);
/* Reads back what `atlas_verify_coverage_render` wrote. An unrecognised
 * dimension or state leaves that dimension UNKNOWN rather than being skipped,
 * so a row written by a newer Atlas is read conservatively rather than
 * optimistically. */
bool atlas_verify_coverage_parse_detail(const char *text, atlas_verify_coverage_report *out);

/* --- verifier polarity ----------------------------------------------------
 *
 * What PASS and FAIL mean on the truth axis, per verifier. A table rather than
 * a convention, because the mapping is genuinely per-verifier: PASS is PRESENT
 * for `atlas.symbol_present` and ABSENT for `atlas.symbol_absent`, and a
 * consumer that assumed either would be wrong about half the vocabulary. */
atlas_verify_truth atlas_verify_verifier_truth_of_check(atlas_verify_verifier v,
                                                        atlas_verify_check c);

/* The coverage dimensions this verifier requires before a **negative**
 * conclusion may be drawn. Writes a pointer to a static table and returns its
 * length; zero for a verifier whose negative rests on a positive reading.
 *
 * This is the per-verifier half of the absence-proof rule, and the reason it is
 * a function rather than a constant per call site is that a test can enumerate
 * it and a new verifier cannot quietly ship without deciding. */
size_t atlas_verify_verifier_absence_dims(atlas_verify_verifier v,
                                          const atlas_verify_coverage_dim **out);

/* --- THE ABSENCE-PROOF RULE ----------------------------------------------
 *
 * §6, and the single most important function in the season. **The only
 * producer of `ATLAS_TRUTH_ABSENT` in Atlas.** Nothing else may assign that
 * value, no caller may pass it in, no transport carries a field that could hold
 * it, and no intake verb accepts one. That is the same single-write-point shape
 * as `settle()`, `atlas_db_evidence_insert`, `atlas_decision_apply_in_tx` and
 * `atlas_orch_apply_in_tx`, applied to a value rather than to a table.
 *
 * The order is the argument, and each step refuses for a different reason:
 *
 *   1. Normative semantics or a JUDGMENT basis → NOT_VERIFIABLE. Asked first
 *      because no amount of evidence changes it, so nothing below should get a
 *      chance to matter.
 *   2. A basis other than DETERMINISTIC → UNKNOWN. §15: empirical evidence
 *      never establishes presence or absence however high the score, because
 *      "nobody found it" is a fact about the searchers.
 *   3. `check == UNAVAILABLE` → UNKNOWN. Atlas could not look, which is not a
 *      finding.
 *   4. The verifier's polarity decides which of PRESENT/ABSENT the check maps
 *      to.
 *   5. **If and only if that is ABSENT**, every dimension the verifier declares
 *      must be sufficient. Otherwise UNKNOWN, naming the dimension that fell
 *      short.
 *   6. PRESENT is returned without a coverage requirement. §7's asymmetry, and
 *      the one step that is deliberately permissive: an incomplete index cannot
 *      conjure a symbol that is not there, so finding one is finding one.
 *
 * `why_out` always receives a reason, including on the PRESENT and ABSENT
 * paths, so every surface can answer "why?" without a second call. */
atlas_verify_truth atlas_verify_truth_of(atlas_verify_verifier v, atlas_verify_basis basis,
                                         atlas_verify_claim_semantics semantics,
                                         atlas_verify_check check,
                                         const atlas_verify_coverage_report *coverage,
                                         atlas_verify_truth_reason *why_out);

/* Whether these two truth values, about the same subject and scope, are logical
 * opposites. §21, and deliberately no more than this: PRESENT versus ABSENT is
 * a contradiction, anything involving UNKNOWN or NOT_VERIFIABLE is not.
 *
 * Atlas does no natural-language negation detection and must not start. What is
 * recognised is the mechanical case — the same verifier subject reached by
 * `atlas.symbol_present` and `atlas.symbol_absent` — and nothing wider. */
bool atlas_verify_truth_contradicts(atlas_verify_truth a, atlas_verify_truth b);

/* --- what a change in the answer means ------------------------------------
 *
 * §16 and §20. Atlas concluded one thing and now concludes another. Whether
 * that is a *verification error* depends on two questions a bare before/after
 * pair cannot answer, and getting either wrong corrupts calibration in a
 * direction that is hard to notice.
 *
 * The three cases, and why they must stay apart:
 *
 *   - **ACQUISITION.** UNKNOWN became PRESENT or ABSENT. Atlas said it did not
 *     know and now it does. This is the system working, not failing. Counting
 *     it as a false negative would charge a verifier for having been honest
 *     about the limits of its coverage — which is exactly the behaviour A9.2.2
 *     exists to encourage, so penalising it would push every verifier back
 *     towards guessing, which is the failure this season set out to end.
 *   - **HISTORICAL.** An established answer changed, and the snapshots differ.
 *     The code changed. §20's SUPERSESSION: both were true, at different times,
 *     and the older stays as history. Charging a verifier for the passage of
 *     time would make every long-lived claim eventually look like a failure.
 *   - **ERROR.** An established answer was contradicted **at the same bound
 *     snapshot and scope**. Atlas asserted a thing over coverage it certified
 *     sufficient, and it was wrong. This is the one case that is a genuine
 *     verifier error and the one case eligible for calibration feedback.
 *
 * `same_snapshot` is what separates the last two, and it is decided from what
 * the earlier result was *bound to* — its evaluated commit and semantic
 * generation — never from how long ago it was written. */
typedef enum atlas_verify_truth_change {
    /* Nothing changed, or nothing established changed. */
    ATLAS_TRUTH_CHANGE_NONE = 0,
    /* UNKNOWN became established. Normal, and never an error. */
    ATLAS_TRUTH_CHANGE_ACQUISITION,
    /* An established answer changed across different snapshots. History. */
    ATLAS_TRUTH_CHANGE_HISTORICAL,
    /* An established answer was contradicted at the same snapshot. */
    ATLAS_TRUTH_CHANGE_ERROR
} atlas_verify_truth_change;

const char *atlas_verify_truth_change_name(atlas_verify_truth_change c);
atlas_verify_truth_change atlas_verify_truth_change_of(atlas_verify_truth prior,
                                                       atlas_verify_truth now, bool same_snapshot);

/* Runs one deterministic verifier over one claim's bounded input.
 *
 * `scope_out` receives Atlas' own sentence saying what was actually
 * established, which the caller compares against the claim's declared scope: a
 * claim whose text outruns its verifier is not verified, it is a claim with an
 * unverified remainder. `detail_out` receives a fixed Atlas-owned explanation.
 * Neither ever carries repository bytes.
 *
 * Returns ATLAS_OK with `UNAVAILABLE` when it could not look. That is not a
 * failure of the call and not evidence about the claim.
 *
 * A9.2.2: `coverage_out` receives what the verifier was able to observe, per
 * dimension. It is filled **before** the check is decided, and that ordering is
 * the fix for the two defects the season's audit found rather than a detail: a
 * *negative* raw result over insufficient coverage is reported as UNAVAILABLE
 * here, not as FAIL, so the verification-state axis and the truth axis cannot
 * disagree about the same evaluation. A negative verdict that reached the check
 * axis while the truth axis said UNKNOWN would be one row contradicting itself.
 *
 * A *positive* raw result stays PASS or FAIL whatever the coverage, which is
 * §7's asymmetry: finding the thing is finding it. */
atlas_status atlas_verify_run_verifier(atlas_db *db, atlas_verify_verifier v, int64_t repo_id,
                                       const char *input, atlas_verify_check *check_out,
                                       atlas_verify_coverage_report *coverage_out, char *scope_out,
                                       size_t scope_size, char *detail_out, size_t detail_size,
                                       atlas_err *err);

/* --- the machine lifecycle audit row, which is also the warrant -----------
 *
 * §40's requirement in a struct: everything needed to reconstruct why Atlas
 * finalized a record, including the policy hash, so the reconstruction does not
 * depend on the policy file still saying what it said.
 *
 * It is also the capability. A row whose verdict is AUTO and which has not been
 * consumed authorises exactly one transition of exactly one revision at exactly
 * one content hash — the shape `decision_challenges` has, because the machine
 * path must bind no more loosely than the operator path. What differs between
 * them is who may mint one, not how tightly it binds. */
typedef struct atlas_verify_audit {
    int64_t id;
    int64_t claim_id;
    int64_t result_id;
    int64_t document_id;
    int64_t revision_id;
    const char *content_hash;
    const char *kind;
    const char *from_status;
    const char *to_status;
    atlas_verify_basis basis;
    atlas_verify_policy_verdict verdict;
    const char *reasons;
    const char *policy_id;
    const char *policy_hash;
    const char *algorithm;
    int prior_version;
    int family_version;
    int confidence;
    atlas_verify_calibration calibration;
    int calibrated_probability; /* -1 for none */
    int independent_groups;
    const char *evidence_snapshot;
    const char *verifier;
    atlas_verify_check check_result;
    const char *binary_id;
} atlas_verify_audit;

/* --- the database layer ---------------------------------------------------
 *
 * `src/db/db_verify.c` is the single write point over the migration-14 tables,
 * the rule `settle()`, `atlas_db_evidence_insert`, `atlas_decision_apply_in_tx`
 * and `atlas_orch_apply_in_tx` all follow. Nothing else writes them. */

/* Finds an actor by uid or creates it. **Refuses a class that requires Atlas
 * attestation unless the identity says Atlas attested it** — the schema CHECK
 * says the same thing, and both are here because the C refusal produces the
 * better message and the CHECK is the guarantee. */
atlas_status atlas_db_verify_actor_upsert(atlas_db *db, atlas_verify_actor *a, const char *now,
                                          atlas_err *err);
atlas_status atlas_db_verify_actor_get(atlas_db *db, int64_t id, atlas_verify_actor *out,
                                       bool *found, atlas_err *err);

atlas_status atlas_db_verify_claim_insert(atlas_db *db, atlas_verify_claim *c, const char *now,
                                          atlas_err *err);
atlas_status atlas_db_verify_claim_get(atlas_db *db, int64_t id, atlas_verify_claim *out,
                                       bool *found, atlas_err *err);
atlas_status atlas_db_verify_claim_find(atlas_db *db, const char *uid, atlas_verify_claim *out,
                                        bool *found, atlas_err *err);

/* Every live claim attached to one revision, newest first, bounded by
 * ATLAS_VERIFY_MAX_CLAIMS. `truncated_out` reports the bound being reached, so
 * a partial answer never reads as a complete one. */
typedef atlas_status (*atlas_verify_claim_cb)(const atlas_verify_claim *c, void *ctx,
                                              atlas_err *err);
atlas_status atlas_db_verify_claims_for_revision(atlas_db *db, int64_t document_id,
                                                 int64_t revision_id, atlas_verify_claim_cb cb,
                                                 void *ctx, bool *truncated_out, atlas_err *err);

/* A9.2.1. Every live claim in one repository, optionally narrowed to one
 * knowledge record, newest first. `document_id` and `revision_id` of 0 mean "no
 * filter". Bounded by `ATLAS_VERIFY_MAX_CLAIMS`, and `truncated_out` reports
 * the bound being reached so a partial list never reads as a complete one. */
atlas_status atlas_db_verify_claims_for_repo(atlas_db *db, int64_t repo_id, int64_t document_id,
                                             int64_t revision_id, int64_t limit,
                                             atlas_verify_claim_cb cb, void *ctx,
                                             bool *truncated_out, atlas_err *err);

atlas_status atlas_db_verify_evidence_insert(atlas_db *db, atlas_verify_evidence *e,
                                             const char *now, atlas_err *err);
atlas_status atlas_db_verify_evidence_dep_add(atlas_db *db, int64_t evidence_id,
                                              int64_t derives_from_id, const char *now,
                                              atlas_err *err);

/* --- A9.2.1: identity and reference resolution ----------------------------
 *
 * The `_by_key` lookups implement §27: a repeated intake call carrying the same
 * immutable content resolves to the row that already exists rather than writing
 * a second one, so a retry is never counted as a corroboration. An empty key
 * never matches, which is what keeps the pre-A9.2.1 rows — written before keys
 * existed — from colliding with each other.
 *
 * The `_find` lookups implement §12's validation requirement: a reference to an
 * object that does not exist is refused at the write point rather than stored,
 * because a dangling edge would silently leave an interpretation looking like an
 * independent root. */
atlas_status atlas_db_verify_claim_by_key(atlas_db *db, const char *key, int64_t *id_out,
                                          atlas_buf *uid_out, bool *found, atlas_err *err);
atlas_status atlas_db_verify_evidence_by_key(atlas_db *db, const char *key, int64_t *id_out,
                                             atlas_buf *uid_out, bool *found, atlas_err *err);
atlas_status atlas_db_verify_attestation_by_key(atlas_db *db, const char *key, int64_t *id_out,
                                                atlas_buf *uid_out, bool *found, atlas_err *err);
atlas_status atlas_db_verify_evidence_find(atlas_db *db, const char *uid, int64_t *id_out,
                                           atlas_verify_evidence_class *class_out, bool *found,
                                           atlas_err *err);
atlas_status atlas_db_verify_attestation_find(atlas_db *db, const char *uid, int64_t *id_out,
                                              bool *found, atlas_err *err);
atlas_status atlas_db_verify_actor_find(atlas_db *db, const char *uid, int64_t *id_out, bool *found,
                                        atlas_err *err);
/* The semantic generation currently served for a repository, or 0. §30. */
atlas_status atlas_db_verify_sem_generation(atlas_db *db, int64_t repo_id, int64_t *gen_out,
                                            atlas_err *err);
/* Whether a commit has been ingested for this repository. §4: source binding is
 * validated against the index, never by asking git, because intake runs on the
 * writer thread where A1 forbids creating a process. */
atlas_status atlas_db_verify_commit_exists(atlas_db *db, int64_t repo_id, const char *oid,
                                           bool *found, atlas_err *err);

/* Writes one attestation and the evidence it rests on, together. They are one
 * statement: an attestation whose evidence links failed to land would be
 * counted as an undeclared interpretation for ever, which silently changes what
 * it is worth. */
atlas_status atlas_db_verify_attestation_insert(atlas_db *db, atlas_verify_attestation *a,
                                                const int64_t *evidence_ids, size_t evidence_count,
                                                const char *now, atlas_err *err);

/* Everything the aggregation needs about one claim, already reduced to counted
 * inputs so the algorithm stays a pure function that tests can drive without a
 * database. Owns its allocations; release with `atlas_verify_inputs_free`. */
typedef struct atlas_verify_inputs {
    atlas_verify_input *items;
    size_t count;
    /* The true number of attestations, even when more existed than were kept.
     * A3's rule about `candidate_count`: a bound that makes something look
     * smaller than it is is a bound that lies. */
    size_t total;
    int64_t *dep_from;
    int64_t *dep_to;
    size_t dep_count;
    bool limit_reached;
} atlas_verify_inputs;

void atlas_verify_inputs_free(atlas_verify_inputs *in);

/* --- A9.2.1 closeout: the detail every product surface reads ---------------
 *
 * `atlas_verify_input` is aggregation-shaped: it carries what the algorithm
 * needs to weigh an attestation and nothing a person could read. That is right
 * for the algorithm and useless for a reader, and a verification system whose
 * answer is a number with no visible evidence behind it is one nobody can check
 * — which defeats the point of recording evidence at all.
 *
 * These rows are the readable half. They are **display detail, never inputs**:
 * nothing here is fed back into a verdict, so a field being wrong misleads a
 * reader without changing a score. Every field a repository or a model chose is
 * UNTRUSTED_DATA and is safe-encoded at the surface that renders it.
 *
 * The two identity columns are the ones that matter and are deliberately
 * separate: `producer_class`/`actor_class` say *what sort of thing* spoke, and
 * `producer_identity`/`actor_identity` say *how well Atlas knows it* —
 * SELF_DECLARED, PEER_AUTHENTICATED or ATLAS_ATTESTED. A UI that prints the
 * first without the second is telling somebody a model is a compiler. */
typedef struct atlas_verify_evidence_detail {
    atlas_buf uid;
    atlas_buf producer_uid;
    atlas_buf producer_name; /* UNTRUSTED_DATA */
    atlas_buf commit_oid;
    atlas_buf path_text;
    atlas_buf symbol;
    atlas_buf target;    /* UNTRUSTED_DATA */
    atlas_buf observed;  /* UNTRUSTED_DATA */
    atlas_buf observed_at;
    atlas_buf tool;
    atlas_buf proof_class;
    atlas_verify_evidence_class cls;
    atlas_verify_evidence_family family;
    atlas_verify_actor_class producer_class;
    atlas_verify_actor_identity producer_identity;
    int64_t id;
    int64_t line_start;
    int64_t line_end;
    /* Older than the policy's evidence horizon. Marked rather than dropped:
     * §47 keeps the historical record and lets only its current weight fall. */
    bool stale;
} atlas_verify_evidence_detail;

typedef struct atlas_verify_attestation_detail {
    atlas_buf uid;
    atlas_buf actor_uid;
    atlas_buf actor_name;     /* UNTRUSTED_DATA */
    atlas_buf actor_provider; /* UNTRUSTED_DATA */
    atlas_buf actor_family;   /* UNTRUSTED_DATA */
    atlas_buf actor_version;  /* UNTRUSTED_DATA */
    atlas_buf actor_role;     /* UNTRUSTED_DATA */
    atlas_buf method;         /* UNTRUSTED_DATA */
    atlas_buf scope_note;     /* UNTRUSTED_DATA */
    atlas_buf basis_commit;
    atlas_verify_actor_class actor_class;
    atlas_verify_actor_identity actor_identity;
    atlas_verify_verdict verdict;
    int64_t id;
    /* The actor's own number, 0..100, or -1 when it did not give one. Data
     * about the source; never Atlas' confidence. */
    int self_confidence;
    /* This actor said something later that replaces this. Kept readable
     * because a reversal is a fact reliability must be able to see; it does
     * not vote. */
    bool superseded;
    /* Which independent evidence group this attestation landed in, or -1 when
     * it did not vote. Two attestations sharing a group corroborate each other
     * not at all, and this is what makes that visible rather than a number a
     * reader has to take on trust. */
    int group;
} atlas_verify_attestation_detail;

typedef struct atlas_verify_detail {
    atlas_verify_evidence_detail *evidence;
    size_t evidence_count;
    size_t evidence_total;
    atlas_verify_attestation_detail *attestations;
    size_t attestation_count;
    size_t attestation_total;
    /* A9.2's rule and A3's before it: a bound that makes a set look smaller
     * than it is is a bound that lies. */
    bool limit_reached;
} atlas_verify_detail;

void atlas_verify_detail_init(atlas_verify_detail *d);
void atlas_verify_detail_free(atlas_verify_detail *d);

/* Loads the readable evidence and attestations for one claim, superseded rows
 * included and marked. A read; it writes nothing. */
atlas_status atlas_db_verify_detail_load(atlas_db *db, int64_t claim_id, const char *stale_before,
                                         atlas_verify_detail *out, atlas_err *err);

/* Loads the inputs for one claim. `stale_before` is the ISO-8601 instant
 * against which an observation is judged to have lost current force; evidence
 * older than it is marked stale rather than dropped, because §47 requires the
 * historical record to survive and only its *current* weight to fall. */
atlas_status atlas_db_verify_inputs_load(atlas_db *db, int64_t claim_id, const char *stale_before,
                                         atlas_verify_inputs *out, atlas_err *err);

/* A9.2.1, §5. What a stored result is *of*: the repository state the claim was
 * bound to, the state Atlas had indexed when the aggregation ran, the semantic
 * generation it used, and whether the first two disagreed.
 *
 * Stored rather than derived on read, which is the one place A9.2's "recompute
 * everything" rule is deliberately reversed and the reversal is the point: the
 * repository will have moved again by the time anybody reads the row, so a
 * derived answer would be about a different pair of commits every time it ran.
 * A9.2 makes the same exception for the result row itself, for the same reason
 * — it is history, not state. */
typedef struct atlas_verify_source_binding {
    const char *claim_commit;     /* what the claim was bound to; NULL for none */
    const char *evaluated_commit; /* what Atlas had indexed; NULL for none */
    int64_t sem_generation;
    bool drift;
} atlas_verify_source_binding;

/* A9.2.2. What a result concluded on the truth axis, and what that conclusion
 * rested on. Grouped the way `atlas_verify_source_binding` groups the drift
 * columns: three facts that are only meaningful together, so a caller cannot
 * record the verdict and forget the coverage that justifies it.
 *
 * `coverage` may be NULL, which records every dimension as UNKNOWN — the honest
 * reading for a result that ran no verifier. */
typedef struct atlas_verify_truth_record {
    atlas_verify_truth truth;
    atlas_verify_truth_reason reason;
    const atlas_verify_coverage_report *coverage;
    /* Which verifier produced this, so the stored `coverage_summary` folds the
     * dimensions the conclusion actually rested on rather than all eleven. */
    atlas_verify_verifier verifier;
} atlas_verify_truth_record;

atlas_status atlas_db_verify_result_insert(atlas_db *db, int64_t claim_id,
                                           const atlas_verify_aggregate *agg, const char *verifier,
                                           atlas_verify_check check,
                                           const atlas_verify_source_binding *src,
                                           const atlas_verify_truth_record *truth, const char *now,
                                           int64_t *id_out, atlas_err *err);

atlas_status atlas_db_verify_audit_insert(atlas_db *db, const atlas_verify_audit *a,
                                          const char *now, int64_t *id_out, atlas_err *err);

/* Whether this audit row is a live warrant for exactly this transition.
 *
 * Called from the decision layer's single write point, inside the transaction
 * that spends it. Requires the row to exist, to carry verdict AUTO, to be
 * unconsumed, and to name this document, this revision, this target state and
 * this content hash. A mismatch on any of them is a refusal, never a warning:
 * a warrant bound to a hash the content no longer has binds to nothing. */
atlas_status atlas_db_verify_warrant_check(atlas_db *db, int64_t warrant_id, int64_t document_id,
                                           int64_t revision_id, const char *to_status,
                                           const char *content_hash, bool *ok_out,
                                           atlas_err *err);
/* Marks it spent. Reports `spent_out = false` when the row had already been
 * consumed, which is what makes a replayed warrant fail deterministically
 * rather than transition twice. */
atlas_status atlas_db_verify_warrant_consume(atlas_db *db, int64_t warrant_id, const char *now,
                                             bool *spent_out, atlas_err *err);

/* Measured reliability for one actor in one domain, in weight units.
 *
 * `reliability_out` is -1 when there is no estimate, which is the state of
 * every actor on a machine where this phase has just been installed. The caller
 * then falls back to `atlas_verify_prior_reliability`, which is documented,
 * versioned and conservative — never to an invented number. */
atlas_status atlas_db_verify_reliability_get(atlas_db *db, int64_t actor_id, const char *domain,
                                             int *reliability_out, int *samples_out,
                                             atlas_verify_calibration *calibration_out,
                                             atlas_err *err);

/* Records one resolved outcome and folds it into the actor's reliability.
 *
 * An ineligible source is stored and **not** counted, so the ineligible case is
 * auditable rather than absent. That is what breaks the circular-ground-truth
 * loop structurally: a machine transition can be seen in the outcome table and
 * can never move a reliability estimate. */
atlas_status atlas_db_verify_outcome_record(atlas_db *db, int64_t claim_id, int64_t actor_id,
                                            const char *domain, atlas_verify_verdict attested,
                                            bool truth, atlas_verify_outcome_source source,
                                            const char *now, atlas_err *err);

/* The three bounded reads the deterministic verifiers are built from.
 *
 * Every one is a read. No A9.2 verifier creates a process, runs a repository's
 * build, or executes a command from anywhere, so there is no input on any of
 * these paths that could become an instruction — which is what lets a
 * deterministic verdict be trusted without asking who supplied the claim. */
atlas_status atlas_db_verify_file_hash(atlas_db *db, int64_t repo_id, const char *path_text,
                                       atlas_buf *hash_out, bool *found_out, atlas_err *err);
/* `complete_out` is false for a generation with failed, partial or unsupported
 * translation units. An absence cannot be established over a partial index —
 * "I did not find it" is not "it is not there" — and reporting otherwise would
 * close obligations that are still outstanding. */
atlas_status atlas_db_verify_sem_symbol(atlas_db *db, int64_t repo_id, const char *name,
                                        int64_t *count_out, bool *complete_out, bool *indexed_out,
                                        atlas_err *err);
atlas_status atlas_db_verify_sem_proven_edge(atlas_db *db, int64_t repo_id, const char *src,
                                             const char *dst, bool *exists_out, bool *indexed_out,
                                             bool *complete_out, atlas_err *err);

/* A9.2.2. Everything `atlas.no_proven_caller` needs to decide whether "nothing
 * calls X" is establishable, in one bounded read.
 *
 * `caller_count` is compiler-proved direct callers. `address_taken` is the
 * number of PROVEN `ADDRESS_TAKEN` edges naming this symbol, and it is the
 * mechanical form of "could a function pointer reach it?" — a C function cannot
 * be called indirectly unless its address escapes somewhere, so zero
 * address-takes over a *complete* generation rules out every dispatch table,
 * callback and registration in the indexed tree at once.
 *
 * `internal_linkage` is true only when the compiler computed INTERNAL for every
 * definition of the name — never for EXTERNAL, NONE or UNKNOWN, so an
 * unestablished linkage is treated as the dangerous case rather than the
 * convenient one. It decides `ATLAS_COVDIM_EXTERNAL_CALLERS`: an internal
 * symbol cannot be named from outside its translation unit, so the indexed tree
 * is the whole world for it. An external one can be called from code Atlas
 * never indexed and reached through dynamic symbol lookup, neither of which any
 * amount of indexing would show — so that dimension is PARTIAL and the answer
 * is UNKNOWN. Bounding the claim mechanically beats a caveat nobody reads.
 *
 * It is reported as a boolean rather than as `atlas_sem_linkage` so this header
 * does not have to include `atlas/sem.h`, which would drag `atlas/db.h` behind
 * it — the dependency this file's opening comment declines to take.
 *
 * `defined` reports whether the symbol exists at all: "nothing calls a function
 * that does not exist" is a statement about the wrong thing, and answering it
 * as an absence would be true and useless. */
atlas_status atlas_db_verify_sem_callers(atlas_db *db, int64_t repo_id, const char *name,
                                         int64_t *caller_count_out, int64_t *address_taken_out,
                                         bool *internal_linkage_out, bool *defined_out,
                                         bool *complete_out, bool *indexed_out, atlas_err *err);

/* A9.2.2. The published semantic generation's state, for the coverage model.
 *
 * `indexed` — a generation is published at all. `complete` — it holds no failed,
 * partial or unsupported translation unit. `current` — it was built from the
 * commit the repository is now scanned at.
 *
 * Three separate answers because they call for three different actions and
 * carry three different truth reasons: nothing to look at, a look that missed
 * part of the tree, and a look at a tree that has since moved. Collapsing any
 * two would tell a reader that something was wrong without telling them what to
 * do about it. */
atlas_status atlas_db_verify_sem_current(atlas_db *db, int64_t repo_id, bool *indexed_out,
                                         bool *complete_out, bool *current_out, atlas_err *err);

/* A9.2.2. The most recently *recorded* verdict for one claim.
 *
 * For the claim list, where recomputing an assessment per row would run a
 * verifier for every claim in a repository. What a list shows is therefore the
 * last recorded answer rather than a fresh one — history, explicitly, which is
 * the same distinction A9.2 draws between `verify_results` and everything else
 * it recomputes on read.
 *
 * Both out-parameters keep their zero — UNVERIFIED and UNKNOWN — when no result
 * has ever been recorded, so a claim nobody has evaluated reads as unevaluated
 * rather than as anything about the world. */
atlas_status atlas_db_verify_last_result(atlas_db *db, int64_t claim_id,
                                         atlas_verify_state *state_out,
                                         atlas_verify_truth *truth_out, atlas_err *err);

/* A9.2.2, §24. What Atlas most recently concluded about a knowledge record, on
 * the truth axis, for the context builder.
 *
 * **Deliberately conservative, and the conservatism is the feature.** The
 * answer is an established value only when the record has at least one live
 * claim, **every** one of them has been evaluated, and they all agree. A record
 * with no claims, with any claim nobody has evaluated, or with claims that
 * disagree reports UNKNOWN.
 *
 * The "every one evaluated" half is the part that is easy to get wrong and was:
 * counting only the claims that *have* a result lets one settled claim speak
 * for a record whose others are still open.
 *
 * The reason is §24 exactly. A context package is read by a model, and a model
 * given a record whose claim is "no runtime override for X exists" must not be
 * able to come away with "runtime override X does not exist". Reporting the
 * strongest or the newest of several claims would let one settled claim speak
 * for a record whose other claims are open — so where there is any doubt the
 * answer is that Atlas does not know, which is the reading that cannot mislead.
 *
 * Recomputed on read, never cached: A6's rule about freshness. */
atlas_status atlas_db_verify_truth_for_document(atlas_db *db, int64_t document_id,
                                                atlas_verify_truth *truth_out, atlas_err *err);

/* A9.2.2. Whether the file index currently describes the working tree.
 *
 * `ATLAS_COVDIM_REPOSITORY_SNAPSHOT`, and it is a different question from
 * A9.2.1's SOURCE_DRIFT: drift compares the claim's commit against the scanned
 * head, and this compares the scanned head against what is on disk. A watcher
 * that is behind makes `atlas_db_verify_file_hash` report bytes the file no
 * longer has, so a content-hash mismatch would be "read a stale snapshot"
 * rather than "read the actual content" — which must not become an absence. */
atlas_status atlas_db_verify_index_current(atlas_db *db, int64_t repo_id, bool *current_out,
                                           atlas_err *err);

/* Clears the soft repository references A4's rule requires, inside
 * `atlas_db_repo_remove`'s transaction. `repositories.id` is a reused rowid and
 * a pointer left behind would eventually name a different repository. */
atlas_status atlas_db_verify_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err);

#endif /* ATLAS_VERIFY_H */

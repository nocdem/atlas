/* Atlas - A9.2: the verification vocabularies, the aggregation, and the
 * separations the phase exists to enforce.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These are enumerated tests over closed vocabularies, in the shape
 * `tests/test_gate_model.c` and `tests/test_decision_model.c` use: every member
 * is visited, and the expectations are written out rather than derived from the
 * same table the implementation reads. A test that agreed with a second copy of
 * the rules would pass while the rules were wrong.
 *
 * The aggregation tests need no database. That is a property of the design —
 * `atlas_verify_aggregate_compute` is a pure function of counted inputs — and
 * it is what makes the scoring auditable: every case below can be read and
 * checked by hand.
 */
#include <string.h>

#include "atlas/verify.h"
#include "atlas/verify_ops.h"
#include "atlas/verifypolicy.h"
#include "atlas_test.h"

/* --- zeros ---------------------------------------------------------------- */

static void test_every_zero_is_the_safe_reading(void) {
    /* A6 keeps UNKNOWN and BLOCKED at zero, A8 keeps DISABLED there, and A9.2
     * follows for the same reason: a `memset` must never produce a permissive
     * default. Checked as values rather than trusted, because a later edit that
     * reorders an enum would silently move them. */
    T_EQ_INT((int)ATLAS_VERIFY_UNVERIFIED, 0);
    T_EQ_INT((int)ATLAS_VERIFY_BASIS_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_ATTEST_INCONCLUSIVE, 0);
    T_EQ_INT((int)ATLAS_ACTOR_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_ACTOR_IDENTITY_SELF_DECLARED, 0);
    T_EQ_INT((int)ATLAS_EVIDENCE_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_EVIDENCE_FAMILY_UNKNOWN, 0);
    T_EQ_INT((int)ATLAS_CALIBRATION_INSUFFICIENT_DATA, 0);
    T_EQ_INT((int)ATLAS_CONFLICT_NONE, 0);
    T_EQ_INT((int)ATLAS_POLICY_NEEDS_REVIEW, 0);
    T_EQ_INT((int)ATLAS_VREASON_NONE, 0);
    T_EQ_INT((int)ATLAS_VERIFIER_NONE, 0);
    T_EQ_INT((int)ATLAS_CHECK_UNAVAILABLE, 0);
    T_EQ_INT((int)ATLAS_OUTCOME_UNKNOWN, 0);

    /* DESCRIPTIVE is zero, which is the safe default for a *different* reason
     * from the rest: it is not "unknown", it is the weaker of two assertions.
     * A claim that does not say which it makes asserts the one that describes
     * rather than the one that legislates. */
    T_EQ_INT((int)ATLAS_CLAIM_DESCRIPTIVE, 0);

    /* A zeroed aggregate must read as "nobody has looked", not as "fine". */
    atlas_verify_aggregate a;
    memset(&a, 0xff, sizeof a);
    atlas_verify_aggregate_init(&a);
    T_EQ_INT((int)a.state, (int)ATLAS_VERIFY_UNVERIFIED);
    T_EQ_INT((int)a.verdict, (int)ATLAS_POLICY_NEEDS_REVIEW);
    T_EQ_INT((int)a.calibration, (int)ATLAS_CALIBRATION_INSUFFICIENT_DATA);
    T_EQ_INT(a.confidence, 0);
    /* -1, never 0: there is no calibrated probability, and zero would *be* a
     * calibrated probability. */
    T_EQ_INT(a.calibrated_probability, -1);

    /* And a zeroed policy automates nothing. */
    atlas_verifypolicy p;
    memset(&p, 0, sizeof p);
    T_EQ_INT((int)p.state, (int)ATLAS_VERIFYPOLICY_DISABLED);
    T_CHECK(!p.deterministic_enforce);
    T_CHECK(!p.empirical_enforce);
    T_EQ_INT((int)p.rule_count, 0);
    T_CHECK(atlas_verifypolicy_find(&p, ATLAS_DECISION_KIND_INVARIANT, ATLAS_DECISION_PROPOSED,
                                    ATLAS_DECISION_APPROVED) == NULL);
}

/* --- vocabularies round-trip ---------------------------------------------- */

static void test_every_name_round_trips(void) {
    for (int i = 0; i <= (int)ATLAS_VERIFY_STALE; i++) {
        atlas_verify_state got = ATLAS_VERIFY_STALE;
        T_CHECK_MSG(atlas_verify_state_parse(atlas_verify_state_name((atlas_verify_state)i), &got),
                    "state %d does not parse back", i);
        T_EQ_INT((int)got, i);
    }
    for (int i = 0; i <= (int)ATLAS_VERIFY_BASIS_JUDGMENT; i++) {
        atlas_verify_basis got = ATLAS_VERIFY_BASIS_UNKNOWN;
        T_CHECK(atlas_verify_basis_parse(atlas_verify_basis_name((atlas_verify_basis)i), &got));
        T_EQ_INT((int)got, i);
    }
    for (int i = 0; i <= (int)ATLAS_ACTOR_ATLAS_VERIFIER; i++) {
        atlas_verify_actor_class got = ATLAS_ACTOR_UNKNOWN;
        T_CHECK_MSG(atlas_verify_actor_class_parse(
                        atlas_verify_actor_class_name((atlas_verify_actor_class)i), &got),
                    "actor class %d does not parse back", i);
        T_EQ_INT((int)got, i);
    }
    for (int i = 0; i <= (int)ATLAS_EVIDENCE_AI_ANALYSIS; i++) {
        atlas_verify_evidence_class got = ATLAS_EVIDENCE_UNKNOWN;
        T_CHECK_MSG(atlas_verify_evidence_class_parse(
                        atlas_verify_evidence_class_name((atlas_verify_evidence_class)i), &got),
                    "evidence class %d does not parse back", i);
        T_EQ_INT((int)got, i);
    }
    for (int i = 0; i <= (int)ATLAS_CALIBRATION_CALIBRATED; i++) {
        atlas_verify_calibration got = ATLAS_CALIBRATION_INSUFFICIENT_DATA;
        T_CHECK(atlas_verify_calibration_parse(
            atlas_verify_calibration_name((atlas_verify_calibration)i), &got));
        T_EQ_INT((int)got, i);
    }
    for (int i = 0; i <= (int)ATLAS_CONFLICT_COMPETING_NORMATIVE; i++) {
        atlas_verify_conflict got = ATLAS_CONFLICT_NONE;
        T_CHECK(atlas_verify_conflict_parse(atlas_verify_conflict_name((atlas_verify_conflict)i),
                                            &got));
        T_EQ_INT((int)got, i);
    }
}

static void test_every_reason_has_a_row_and_a_written_meaning(void) {
    /* A6's arrangement: the verdict follows from the reason rather than being
     * chosen beside it, so a reason with no row cannot exist. A member added to
     * the enum without a row falls through to the placeholder, and this is what
     * catches it. */
    for (size_t i = 0; i < atlas_verify_reason_count(); i++) {
        atlas_verify_reason r = atlas_verify_reason_at(i);
        const char *name = atlas_verify_reason_name(r);
        const char *desc = atlas_verify_reason_description(r);
        T_CHECK_MSG(strcmp(name, "UNNAMED_REASON") != 0, "reason %d has no name", (int)r);
        T_CHECK_MSG(strstr(desc, "which is a defect") == NULL, "reason %s has no meaning", name);
        T_CHECK_MSG(strlen(desc) > 20, "reason %s has a meaning too short to be one", name);
    }
    /* Every member of the enum is in the table, checked by walking the enum
     * rather than the table — otherwise a missing row would simply not be
     * visited. */
    for (int i = 0; i <= (int)ATLAS_VREASON_OK; i++) {
        T_CHECK_MSG(strcmp(atlas_verify_reason_name((atlas_verify_reason)i), "UNNAMED_REASON") != 0,
                    "reason value %d has no row in REASONS[]", i);
    }
}

static void test_an_unlisted_reason_is_not_permission(void) {
    /* A reason nobody wrote down must not read as approval. Cast a value past
     * the end of the vocabulary, which is what a future edit would introduce. */
    atlas_verify_reason bogus = (atlas_verify_reason)(ATLAS_VREASON_OK + 7);
    T_EQ_INT((int)atlas_verify_reason_verdict(bogus), (int)ATLAS_POLICY_BLOCKED);
}

/* --- the fold ------------------------------------------------------------- */

static void test_the_verdict_fold_takes_the_weaker(void) {
    /* FORBIDDEN absorbs everything, then BLOCKED, then NEEDS_REVIEW, then
     * SHADOW. AUTO is reachable only when nothing objected. All 25 pairs. */
    static const atlas_verify_policy_verdict V[] = {ATLAS_POLICY_AUTO, ATLAS_POLICY_SHADOW,
                                                    ATLAS_POLICY_NEEDS_REVIEW,
                                                    ATLAS_POLICY_BLOCKED, ATLAS_POLICY_FORBIDDEN};
    /* Rank written out, not derived from the implementation's own helper. */
    static const int RANK[] = {0, 1, 2, 3, 4};
    for (size_t i = 0; i < 5; i++) {
        for (size_t j = 0; j < 5; j++) {
            atlas_verify_policy_verdict got = atlas_verify_verdict_fold(V[i], V[j]);
            atlas_verify_policy_verdict want = RANK[i] >= RANK[j] ? V[i] : V[j];
            T_CHECK_MSG(got == want, "fold(%s,%s) = %s, wanted %s",
                        atlas_verify_policy_verdict_name(V[i]),
                        atlas_verify_policy_verdict_name(V[j]),
                        atlas_verify_policy_verdict_name(got),
                        atlas_verify_policy_verdict_name(want));
        }
    }
}

static void test_a_reason_that_does_not_fit_still_weakens_the_verdict(void) {
    /* A6's rule exactly: the fold happens before the list is appended to, so a
     * decision with more problems than fit cannot report a better verdict than
     * one with fewer. */
    atlas_verify_aggregate a;
    atlas_verify_aggregate_init(&a);
    for (size_t i = 0; i < ATLAS_VERIFY_MAX_REASONS + 5u; i++) {
        atlas_verify_aggregate_note(&a, ATLAS_VREASON_LOW_CONFIDENCE);
    }
    atlas_verify_aggregate_note(&a, ATLAS_VREASON_RISK_REQUIRES_AUTHORITY);
    T_EQ_INT((int)a.verdict, (int)ATLAS_POLICY_FORBIDDEN);
    T_CHECK_MSG(a.reason_count <= ATLAS_VERIFY_MAX_REASONS, "the list overflowed its bound");
    /* The true count is reported even though fewer were kept — A3's rule about
     * `candidate_count`: a bound that makes something look smaller than it is
     * is a bound that lies. */
    T_CHECK_MSG(a.reason_total > a.reason_count, "the true reason count was not preserved");
}

/* --- the five separations ------------------------------------------------- */

static void test_deterministic_verification_does_not_require_calibration(void) {
    /* **The phase's central rule.** A function rather than a comment precisely
     * so it can be asserted, and so no policy path can quietly reintroduce the
     * coupling.
     *
     * The asymmetry is the whole point: an empirical verdict is a claim about
     * how often sources of a kind have been right, which has no content without
     * history; a deterministic verdict is a mechanical evaluation of a bounded
     * truth condition, to which that history is not an input. */
    T_CHECK(!atlas_verify_basis_requires_calibration(ATLAS_VERIFY_BASIS_DETERMINISTIC));
    T_CHECK(atlas_verify_basis_requires_calibration(ATLAS_VERIFY_BASIS_EMPIRICAL));
    /* JUDGMENT returns false too, and for a different reason: calibration is
     * irrelevant because no automatic transition happens at all. A false here
     * must never be read as "may proceed". */
    T_CHECK(!atlas_verify_basis_requires_calibration(ATLAS_VERIFY_BASIS_JUDGMENT));
}

static void test_a_deterministic_verifier_cannot_establish_a_rule(void) {
    /* Separation 4, in the one cell that enforces it. A mechanical verifier
     * reads the world; a normative claim is not about the world; so
     * DETERMINISTIC + NORMATIVE is false. Allowing it would let an observation
     * of the current implementation become permanent policy with an audit trail
     * that looks impeccable. */
    T_CHECK(!atlas_verify_basis_may_verify_semantics(ATLAS_VERIFY_BASIS_DETERMINISTIC,
                                                     ATLAS_CLAIM_NORMATIVE));
    T_CHECK(!atlas_verify_basis_may_verify_semantics(ATLAS_VERIFY_BASIS_EMPIRICAL,
                                                     ATLAS_CLAIM_NORMATIVE));
    T_CHECK(atlas_verify_basis_may_verify_semantics(ATLAS_VERIFY_BASIS_DETERMINISTIC,
                                                    ATLAS_CLAIM_DESCRIPTIVE));
    T_CHECK(atlas_verify_basis_may_verify_semantics(ATLAS_VERIFY_BASIS_EMPIRICAL,
                                                    ATLAS_CLAIM_DESCRIPTIVE));
    /* A judgment is about what ought to be, so it cannot establish a
     * descriptive fact either — that is what evidence is for. */
    T_CHECK(!atlas_verify_basis_may_verify_semantics(ATLAS_VERIFY_BASIS_JUDGMENT,
                                                     ATLAS_CLAIM_DESCRIPTIVE));
}

static void test_a_submitted_actor_cannot_become_a_tool(void) {
    /* §44. An AI saying "clang proves this" is not Atlas running clang. The
     * four classes whose entire weight is "Atlas did this" may only exist with
     * an Atlas-attested identity, and the refusal is at the write point rather
     * than a discount — a discounted forgery still reads as tool output to
     * somebody skimming a UI. */
    T_CHECK(atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_TOOL));
    T_CHECK(atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_TEST));
    T_CHECK(atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_RUNTIME_OBSERVATION));
    T_CHECK(atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_ATLAS_VERIFIER));
    /* And the classes a submitter legitimately may name. */
    T_CHECK(!atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_AI_AGENT));
    T_CHECK(!atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_HUMAN));
    T_CHECK(!atlas_verify_actor_class_requires_atlas_identity(ATLAS_ACTOR_DOCUMENT));
}

static void test_no_prior_makes_a_self_declared_source_strong(void) {
    /* §20. Nothing is `human = 100` or `model X = 95`, and identity dominates
     * class: the difference between "Atlas ran the compiler" and "something
     * told Atlas the compiler said so" is the largest real difference here and
     * the one an attacker controls. */
    for (int c = 0; c <= (int)ATLAS_ACTOR_ATLAS_VERIFIER; c++) {
        int self = atlas_verify_prior_reliability((atlas_verify_actor_class)c,
                                                  ATLAS_ACTOR_IDENTITY_SELF_DECLARED);
        int attested = atlas_verify_prior_reliability((atlas_verify_actor_class)c,
                                                      ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED);
        T_CHECK_MSG(self <= 350, "a self-declared %s is weighted %d, above the cap",
                    atlas_verify_actor_class_name((atlas_verify_actor_class)c), self);
        T_CHECK_MSG(self <= attested, "self-declaration outweighed Atlas attestation for %s",
                    atlas_verify_actor_class_name((atlas_verify_actor_class)c));
        /* Nothing reaches full scale: no single attestation can carry a claim
         * to certainty by itself, whatever it is. */
        T_CHECK_MSG(attested < ATLAS_VERIFY_WEIGHT_SCALE,
                    "%s reaches full weight on its own",
                    atlas_verify_actor_class_name((atlas_verify_actor_class)c));
    }
    /* A human is not at the top, because these weights concern claims of fact
     * and human authority lives entirely elsewhere — in the operator channel,
     * which no weight here substitutes for. */
    T_CHECK(atlas_verify_prior_reliability(ATLAS_ACTOR_HUMAN, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED) <
            atlas_verify_prior_reliability(ATLAS_ACTOR_ATLAS_VERIFIER,
                                           ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED));
}

static void test_a_machine_transition_can_never_be_ground_truth(void) {
    /* §45 and §65, the circularity break. A model supports a claim, the
     * aggregate likes it, Atlas transitions, the transition is counted as
     * truth, the model's reliability rises. Every step looks reasonable and the
     * result is a system that taught itself to trust a source using that
     * source's own output. */
    T_CHECK(!atlas_verify_outcome_eligible(ATLAS_OUTCOME_MACHINE_TRANSITION));
    T_CHECK(!atlas_verify_outcome_eligible(ATLAS_OUTCOME_UNKNOWN));
    T_CHECK(atlas_verify_outcome_eligible(ATLAS_OUTCOME_DETERMINISTIC_VERIFIER));
    T_CHECK(atlas_verify_outcome_eligible(ATLAS_OUTCOME_OPERATOR_RESOLUTION));
    T_CHECK(atlas_verify_outcome_eligible(ATLAS_OUTCOME_RUNTIME_OBSERVATION));
}

static void test_an_interpretation_is_never_an_independent_root(void) {
    /* §11's mechanism. An interpretation is by definition *of* something, so
     * one that declares no source is an undeclared derivation rather than a
     * fresh observation of the world. */
    T_CHECK(!atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_AI_ANALYSIS));
    T_CHECK(!atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_HUMAN_STATEMENT));
    T_CHECK(!atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_ATLAS_KNOWLEDGE));
    T_CHECK(!atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_UNKNOWN));
    T_CHECK(atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_SOURCE_CODE));
    T_CHECK(atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_COMPILER));
    T_CHECK(atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_TEST));
    T_CHECK(atlas_verify_evidence_class_may_be_root(ATLAS_EVIDENCE_RUNTIME));

    /* And every class maps to a family, so a class added without a family
     * cannot silently become "unknown" and float free of the grouping. */
    for (int c = 1; c <= (int)ATLAS_EVIDENCE_AI_ANALYSIS; c++) {
        T_CHECK_MSG(atlas_verify_evidence_family_of((atlas_verify_evidence_class)c) !=
                        ATLAS_EVIDENCE_FAMILY_UNKNOWN,
                    "evidence class %s has no family",
                    atlas_verify_evidence_class_name((atlas_verify_evidence_class)c));
    }
}

/* --- the aggregation ------------------------------------------------------ */

static atlas_verify_input mk(atlas_verify_verdict v, atlas_verify_actor_class cls,
                             atlas_verify_actor_identity id, atlas_verify_evidence_family fam) {
    atlas_verify_input in;
    memset(&in, 0, sizeof in);
    in.verdict = v;
    in.actor_class = cls;
    in.actor_identity = id;
    in.family = fam;
    in.scope_match = true;
    in.reliability = -1;
    in.group = -1;
    return in;
}

static void test_three_models_reading_one_document_count_once(void) {
    /* **Fixture D.** The case the whole phase is shaped around.
     *
     * Three differently-named AI agents each support a claim, each resting on
     * an interpretation with no declared source. That is three attestations
     * over one evidence root, and it must not read as three independent
     * corroborations. */
    atlas_verify_input in[3];
    for (size_t i = 0; i < 3; i++) {
        in[i] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_AI_AGENT,
                   ATLAS_ACTOR_IDENTITY_SELF_DECLARED, ATLAS_EVIDENCE_FAMILY_INTERPRETATION);
    }
    int groups = atlas_verify_independent_groups(in, 3, NULL, NULL, 0);
    T_CHECK_MSG(groups == 1, "three models over one root produced %d independent groups", groups);

    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, in, 3, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_EQ_INT(a.support_count, 3);
    T_EQ_INT(a.independent_groups, 1);
    /* One group contributes what its *best* member is worth, never the sum:
     * repetition contributes nothing at all. */
    T_CHECK_MSG(a.support_mass <= 350,
                "three correlated sources summed to %lld", (long long)a.support_mass);
    /* And a single group can never reach VERIFIED on the empirical path,
     * however confident it is. */
    T_CHECK_MSG(a.state != ATLAS_VERIFY_VERIFIED,
                "one evidence group reached VERIFIED: %s", atlas_verify_state_name(a.state));
}

static void test_confidence_does_not_grow_by_repetition(void) {
    /* §46. One actor submitting the same attestation over and over, and an
     * orchestrator's fleet of subagents, are the same failure and get the same
     * answer: Atlas could not demonstrate independence, so it does not assume
     * it. Confidence must not scale roughly linearly with duplication. */
    atlas_verify_input one[1] = {mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_AI_AGENT,
                                    ATLAS_ACTOR_IDENTITY_SELF_DECLARED,
                                    ATLAS_EVIDENCE_FAMILY_INTERPRETATION)};
    (void)atlas_verify_independent_groups(one, 1, NULL, NULL, 0);
    atlas_verify_aggregate a1;
    atlas_verify_aggregate_compute(&a1, one, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);

    atlas_verify_input many[40];
    for (size_t i = 0; i < 40; i++) {
        many[i] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_AI_AGENT,
                     ATLAS_ACTOR_IDENTITY_SELF_DECLARED, ATLAS_EVIDENCE_FAMILY_INTERPRETATION);
    }
    (void)atlas_verify_independent_groups(many, 40, NULL, NULL, 0);
    atlas_verify_aggregate a40;
    atlas_verify_aggregate_compute(&a40, many, 40, ATLAS_VERIFY_BASIS_EMPIRICAL);

    T_CHECK_MSG(a40.confidence == a1.confidence,
                "40 duplicated attestations moved confidence from %d to %d", a1.confidence,
                a40.confidence);
    T_EQ_INT(a40.independent_groups, 1);
}

static void test_genuinely_independent_groups_raise_confidence(void) {
    /* **Fixture E.** One interpretation, corroborated separately by compiler
     * output, a test and a runtime observation. Their derivation chains
     * genuinely differ, so they form several groups — and *that* is what
     * confidence is allowed to grow on. */
    atlas_verify_input in[4];
    in[0] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_AI_AGENT, ATLAS_ACTOR_IDENTITY_SELF_DECLARED,
               ATLAS_EVIDENCE_FAMILY_INTERPRETATION);
    in[1] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TOOL, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
    in[2] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TEST, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION);
    in[3] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_RUNTIME_OBSERVATION,
               ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED, ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION);
    int groups = atlas_verify_independent_groups(in, 4, NULL, NULL, 0);
    T_CHECK_MSG(groups >= 3, "genuinely independent evidence produced only %d groups", groups);

    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, in, 4, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_CHECK_MSG(a.confidence >= 80, "independent corroboration scored only %d", a.confidence);
    T_EQ_INT((int)a.state, (int)ATLAS_VERIFY_VERIFIED);
    /* Two distinct families were present and both are counted. */
    T_CHECK(a.independent_families >= 2);
}

static void test_a_declared_derivation_edge_collapses_a_group(void) {
    /* The mechanical form of "actor is not evidence": what makes two sources
     * correlated is the material they read. Two otherwise-independent-looking
     * attestations joined by a declared edge are one group. */
    atlas_verify_input in[2];
    in[0] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TOOL, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
    in[1] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TEST, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION);
    T_EQ_INT(atlas_verify_independent_groups(in, 2, NULL, NULL, 0), 2);

    int64_t from[1] = {0};
    int64_t to[1] = {1};
    atlas_verify_input joined[2];
    joined[0] = in[0];
    joined[1] = in[1];
    joined[0].group = -1;
    joined[1].group = -1;
    T_EQ_INT(atlas_verify_independent_groups(joined, 2, from, to, 1), 1);
}

static void test_a_lone_source_never_reaches_certainty(void) {
    /* `ATLAS_VERIFY_PRIOR_MASS` is the weight of not knowing and never goes
     * away, so 100 is unreachable by accumulation and one perfect source lands
     * short of it. Certainty has to be assembled out of independent groups. */
    atlas_verify_input in[1] = {mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_ATLAS_VERIFIER,
                                   ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
                                   ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT)};
    (void)atlas_verify_independent_groups(in, 1, NULL, NULL, 0);
    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, in, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_CHECK_MSG(a.confidence < 100, "a single source reached certainty: %d", a.confidence);
}

static void test_a_proposer_supporting_itself_is_discounted(void) {
    /* §26. A proposal does not validate itself merely because its proposer
     * likes it. Not discarded — it is evidence about what the proposer
     * believes — but it cannot carry the claim. */
    atlas_verify_input plain[1] = {mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_AI_AGENT,
                                      ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED,
                                      ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT)};
    atlas_verify_input self[1];
    self[0] = plain[0];
    self[0].proposer = true;

    (void)atlas_verify_independent_groups(plain, 1, NULL, NULL, 0);
    (void)atlas_verify_independent_groups(self, 1, NULL, NULL, 0);
    atlas_verify_aggregate ap, as;
    atlas_verify_aggregate_compute(&ap, plain, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);
    atlas_verify_aggregate_compute(&as, self, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_CHECK_MSG(as.confidence < ap.confidence,
                "self-attestation scored %d, the same as an independent one", as.confidence);
}

static void test_a_deterministic_fail_outweighs_any_amount_of_agreement(void) {
    /* "Deterministic evidence is not reliability aggregation", as behaviour
     * rather than documentation: a mechanical FAIL contradicts the claim
     * however many sources like it. */
    atlas_verify_input in[6];
    for (size_t i = 0; i < 5; i++) {
        in[i] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_AI_AGENT,
                   ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED, ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
    }
    in[5] = mk(ATLAS_ATTEST_CONTRADICT, ATLAS_ACTOR_ATLAS_VERIFIER,
               ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED, ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
    (void)atlas_verify_independent_groups(in, 6, NULL, NULL, 0);
    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, in, 6, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_EQ_INT((int)a.state, (int)ATLAS_VERIFY_CONTRADICTED);
    T_CHECK(a.deterministic_fail);
}

static void test_meaningful_dissent_keeps_a_claim_short_of_verified(void) {
    /* §63: confidence must not erase disagreement. A claim with real
     * contradiction stays SUPPORTED however high the raw score, so a UI cannot
     * present it as settled and hide the objection. */
    atlas_verify_input in[4];
    in[0] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TOOL, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
    in[1] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TEST, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION);
    in[2] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_RUNTIME_OBSERVATION,
               ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED, ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION);
    in[3] = mk(ATLAS_ATTEST_CONTRADICT, ATLAS_ACTOR_TOOL, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
               ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
    (void)atlas_verify_independent_groups(in, 4, NULL, NULL, 0);
    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, in, 4, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_CHECK_MSG(a.contradict_count == 1, "the dissent was lost");
    T_CHECK_MSG(a.state != ATLAS_VERIFY_VERIFIED,
                "a contradicted claim reported as VERIFIED at confidence %d", a.confidence);
}

static void test_stale_evidence_loses_current_force_and_keeps_its_record(void) {
    /* §47. Old evidence is not deleted; it stops deciding the present. */
    atlas_verify_input fresh[1] = {mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_RUNTIME_OBSERVATION,
                                      ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
                                      ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION)};
    atlas_verify_input old[1];
    old[0] = fresh[0];
    old[0].stale = true;

    (void)atlas_verify_independent_groups(fresh, 1, NULL, NULL, 0);
    (void)atlas_verify_independent_groups(old, 1, NULL, NULL, 0);
    atlas_verify_aggregate af, ao;
    atlas_verify_aggregate_compute(&af, fresh, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);
    atlas_verify_aggregate_compute(&ao, old, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_CHECK_MSG(ao.confidence < af.confidence, "stale evidence kept its full weight");
    /* It is still counted and still visible: the attestation did not vanish. */
    T_EQ_INT(ao.support_count, 1);
    T_CHECK(ao.stale);
}

static void test_no_attestation_is_unverified_not_inconclusive(void) {
    /* "Nobody looked" and "we looked and could not tell" call for different
     * actions, so they must stay different answers. */
    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, NULL, 0, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_EQ_INT((int)a.state, (int)ATLAS_VERIFY_UNVERIFIED);

    atlas_verify_input in[1] = {mk(ATLAS_ATTEST_INCONCLUSIVE, ATLAS_ACTOR_AI_AGENT,
                                   ATLAS_ACTOR_IDENTITY_SELF_DECLARED,
                                   ATLAS_EVIDENCE_FAMILY_INTERPRETATION)};
    (void)atlas_verify_independent_groups(in, 1, NULL, NULL, 0);
    atlas_verify_aggregate b;
    atlas_verify_aggregate_compute(&b, in, 1, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_EQ_INT((int)b.state, (int)ATLAS_VERIFY_INCONCLUSIVE);
}

static void test_the_algorithm_is_reproducible(void) {
    /* Integer throughout and no floating point anywhere, so "the same inputs
     * produce the same result" is a property of the code rather than a hope
     * about rounding. A machine transition has to be reconstructable from its
     * audit row years later on a different machine. */
    atlas_verify_input a[3], b[3];
    for (size_t i = 0; i < 3; i++) {
        a[i] = mk(i == 1 ? ATLAS_ATTEST_CONTRADICT : ATLAS_ATTEST_SUPPORT,
                  ATLAS_ACTOR_TOOL, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
                  ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT);
        b[i] = a[i];
    }
    (void)atlas_verify_independent_groups(a, 3, NULL, NULL, 0);
    (void)atlas_verify_independent_groups(b, 3, NULL, NULL, 0);
    atlas_verify_aggregate ra, rb;
    atlas_verify_aggregate_compute(&ra, a, 3, ATLAS_VERIFY_BASIS_EMPIRICAL);
    atlas_verify_aggregate_compute(&rb, b, 3, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_EQ_INT(ra.confidence, rb.confidence);
    T_EQ_INT(ra.independent_groups, rb.independent_groups);
    T_CHECK(ra.support_mass == rb.support_mass);
    T_CHECK(ra.contradict_mass == rb.contradict_mass);
    /* Group numbering is a function of input order and nothing else. */
    for (size_t i = 0; i < 3; i++) {
        T_EQ_INT(a[i].group, b[i].group);
    }
    T_CHECK(strcmp(ra.algorithm, ATLAS_VERIFY_ALGORITHM) == 0);
}

static void test_no_score_is_ever_a_calibrated_probability_without_calibration(void) {
    /* Separation 3. The two are different fields with different printers, and
     * the probability stays absent until calibration supports it. On a machine
     * where this phase has just been installed that is always. */
    atlas_verify_input in[3];
    for (size_t i = 0; i < 3; i++) {
        in[i] = mk(ATLAS_ATTEST_SUPPORT, ATLAS_ACTOR_TOOL, ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED,
                   i == 0 ? ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT
                          : ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION);
    }
    (void)atlas_verify_independent_groups(in, 3, NULL, NULL, 0);
    atlas_verify_aggregate a;
    atlas_verify_aggregate_compute(&a, in, 3, ATLAS_VERIFY_BASIS_EMPIRICAL);
    T_CHECK_MSG(a.confidence > 0, "the fixture produced no score to test");
    T_EQ_INT((int)a.calibration, (int)ATLAS_CALIBRATION_INSUFFICIENT_DATA);
    T_EQ_INT(a.calibrated_probability, -1);
}

/* --- what no policy may authorise ----------------------------------------- */

static void test_no_policy_can_authorise_accepting_a_risk(void) {
    /* **Fixture J.** Atlas may verify that a risk is real and that it has been
     * mitigated. That the project is willing to live with it is a different
     * sentence with an owner, and no quantity of agreement supplies one.
     *
     * Checked before the policy file is read, so an operator's mistaken rule is
     * refused rather than obeyed: "root wrote it" establishes that the
     * instruction is authentic, not that it is one Atlas should carry out. */
    atlas_verify_reason why = ATLAS_VREASON_NONE;
    T_CHECK(atlas_verifypolicy_transition_forbidden(ATLAS_DECISION_KIND_ACCEPTED_RISK,
                                                    ATLAS_DECISION_PROPOSED,
                                                    ATLAS_DECISION_APPROVED, &why));
    T_EQ_INT((int)why, (int)ATLAS_VREASON_RISK_REQUIRES_AUTHORITY);
    T_EQ_INT((int)atlas_verify_reason_verdict(why), (int)ATLAS_POLICY_FORBIDDEN);

    /* And a policy naming it is malformed rather than inert: an inert rule
     * reads, to whoever wrote it, exactly like one that works. */
    static const char CONF[] = "enabled = yes\n"
                               "allow = ACCEPTED_RISK PROPOSED APPROVED atlas.symbol_absent\n";
    atlas_verifypolicy p;
    atlas_verifypolicy_parse_buffer(CONF, sizeof CONF - 1u, &p);
    T_EQ_INT((int)p.state, (int)ATLAS_VERIFYPOLICY_DISABLED);
    T_EQ_INT((int)p.reason, (int)ATLAS_VERIFYPOLICY_REASON_MALFORMED);
}

static void test_no_policy_can_authorise_a_rejection(void) {
    /* §34. Automatic rejection needs an explicit falsification condition, and
     * low confidence is not one. Rejecting a legitimate proposal is not the
     * mirror of failing to approve one: the first destroys work and is
     * terminal, the second costs a wait. */
    for (int k = 0; k < (int)ATLAS_DECISION_KIND_MAX; k++) {
        atlas_verify_reason why = ATLAS_VREASON_NONE;
        T_CHECK_MSG(atlas_verifypolicy_transition_forbidden((atlas_decision_kind)k,
                                                            ATLAS_DECISION_PROPOSED,
                                                            ATLAS_DECISION_REJECTED, &why),
                    "kind %s permitted an automatic rejection",
                    atlas_decision_kind_name((atlas_decision_kind)k));
    }
}

static void test_a_policy_may_not_name_a_transition_the_state_machine_refuses(void) {
    /* The state machine has the final say and it is kind-aware. A DECISION
     * cannot be resolved, so a rule saying it can is malformed. */
    atlas_verify_reason why = ATLAS_VREASON_NONE;
    T_CHECK(atlas_verifypolicy_transition_forbidden(ATLAS_DECISION_KIND_DECISION,
                                                    ATLAS_DECISION_APPROVED,
                                                    ATLAS_DECISION_RESOLVED, &why));
    T_EQ_INT((int)why, (int)ATLAS_VREASON_TRANSITION_ILLEGAL);
    /* An OBLIGATION can be, because its approved form makes a demand. */
    T_CHECK(!atlas_verifypolicy_transition_forbidden(ATLAS_DECISION_KIND_OBLIGATION,
                                                     ATLAS_DECISION_APPROVED,
                                                     ATLAS_DECISION_RESOLVED, &why));
}

/* --- A12.1: the channel vocabulary, every member visited ------------------- */

static void test_only_model_and_operator_are_transport_selectable(void) {
    /* The one definition of transport-selectability, walked over the whole
     * enum with the expectations written out rather than derived — the shape
     * every vocabulary test in this file uses. The predicate is what
     * `atlas_verify_channel_parse` matches names against, so this walk is also
     * the accept-list: ATLAS cannot make a request's evidence authentic,
     * DOCUMENT cannot mint one speaker per pasted file, and UNKNOWN cannot be
     * asserted into existence. */
    static const struct {
        atlas_verify_channel c;
        bool selectable;
    } WALK[] = {
        {ATLAS_VERIFY_CHANNEL_UNKNOWN, false}, {ATLAS_VERIFY_CHANNEL_MODEL, true},
        {ATLAS_VERIFY_CHANNEL_OPERATOR, true}, {ATLAS_VERIFY_CHANNEL_ATLAS, false},
        {ATLAS_VERIFY_CHANNEL_DOCUMENT, false},
    };
    for (size_t i = 0; i < sizeof WALK / sizeof WALK[0]; i++) {
        T_CHECK_MSG(atlas_verify_channel_is_transport_selectable(WALK[i].c) == WALK[i].selectable,
                    "%s must%s be transport-selectable", atlas_verify_channel_name(WALK[i].c),
                    WALK[i].selectable ? "" : " not");
        atlas_verify_channel got = ATLAS_VERIFY_CHANNEL_UNKNOWN;
        bool parsed = atlas_verify_channel_parse(atlas_verify_channel_name(WALK[i].c), &got);
        T_CHECK_MSG(parsed == WALK[i].selectable,
                    "the parse and the predicate disagree about %s",
                    atlas_verify_channel_name(WALK[i].c));
    }
    /* The rank is inert for DOCUMENT and must stay so: it sits below OPERATOR,
     * so the transport edge's strict weakening comparison would admit the name
     * for an operator peer the moment a parse accepted it. The parse refusal
     * is the whole guard on that path. */
    T_CHECK(atlas_verify_channel_authority(ATLAS_VERIFY_CHANNEL_DOCUMENT) <
            atlas_verify_channel_authority(ATLAS_VERIFY_CHANNEL_OPERATOR));
}

static const atlas_test TESTS[] = {
    {"every zero is the safe reading", test_every_zero_is_the_safe_reading},
    {"only MODEL and OPERATOR are transport-selectable",
     test_only_model_and_operator_are_transport_selectable},
    {"every name round-trips", test_every_name_round_trips},
    {"every reason has a row and a written meaning",
     test_every_reason_has_a_row_and_a_written_meaning},
    {"an unlisted reason is not permission", test_an_unlisted_reason_is_not_permission},
    {"the verdict fold takes the weaker", test_the_verdict_fold_takes_the_weaker},
    {"a reason that does not fit still weakens the verdict",
     test_a_reason_that_does_not_fit_still_weakens_the_verdict},
    {"deterministic verification does not require calibration",
     test_deterministic_verification_does_not_require_calibration},
    {"a deterministic verifier cannot establish a rule",
     test_a_deterministic_verifier_cannot_establish_a_rule},
    {"a submitted actor cannot become a tool", test_a_submitted_actor_cannot_become_a_tool},
    {"no prior makes a self-declared source strong",
     test_no_prior_makes_a_self_declared_source_strong},
    {"a machine transition can never be ground truth",
     test_a_machine_transition_can_never_be_ground_truth},
    {"an interpretation is never an independent root",
     test_an_interpretation_is_never_an_independent_root},
    {"three models reading one document count once",
     test_three_models_reading_one_document_count_once},
    {"confidence does not grow by repetition", test_confidence_does_not_grow_by_repetition},
    {"genuinely independent groups raise confidence",
     test_genuinely_independent_groups_raise_confidence},
    {"a declared derivation edge collapses a group",
     test_a_declared_derivation_edge_collapses_a_group},
    {"a lone source never reaches certainty", test_a_lone_source_never_reaches_certainty},
    {"a proposer supporting itself is discounted",
     test_a_proposer_supporting_itself_is_discounted},
    {"a deterministic fail outweighs any amount of agreement",
     test_a_deterministic_fail_outweighs_any_amount_of_agreement},
    {"meaningful dissent keeps a claim short of verified",
     test_meaningful_dissent_keeps_a_claim_short_of_verified},
    {"stale evidence loses current force and keeps its record",
     test_stale_evidence_loses_current_force_and_keeps_its_record},
    {"no attestation is unverified, not inconclusive",
     test_no_attestation_is_unverified_not_inconclusive},
    {"the algorithm is reproducible", test_the_algorithm_is_reproducible},
    {"no score is ever a calibrated probability without calibration",
     test_no_score_is_ever_a_calibrated_probability_without_calibration},
    {"no policy can authorise accepting a risk", test_no_policy_can_authorise_accepting_a_risk},
    {"no policy can authorise a rejection", test_no_policy_can_authorise_a_rejection},
    {"a policy may not name a transition the state machine refuses",
     test_a_policy_may_not_name_a_transition_the_state_machine_refuses},
};

ATLAS_TEST_MAIN("verify_model", TESTS)

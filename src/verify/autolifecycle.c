/* Atlas - A9.2: the automatic lifecycle engine.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The one place a verification result can become a lifecycle transition.
 *
 * ## Why this owns its transaction
 *
 * `atlas_decision_apply_in_tx` has had exactly two callers since A4:
 * `atlas_decision_apply`, which adds only begin/commit/rollback, and
 * `op_decision_locked` in `src/ai/ai.c`, which already owns a transaction
 * because its A2 row and its A4 document must commit together. The rule for
 * adding a third is that it must genuinely own a wider unit of work.
 *
 * This does. A machine transition and the audit row that justifies it are one
 * fact: an audit row with no transition describes something that did not
 * happen, and — far worse — a transition with no audit row is an automatic
 * change to project knowledge with no recoverable reason. §40 requires that a
 * future auditor can reconstruct why Atlas finalized a record, and the only way
 * to guarantee that is for the reason and the act to commit or fail together.
 *
 * The audit row is also the capability, so the ordering matters: written first,
 * checked and consumed by the decision layer's write point, and rolled back
 * with everything else if the transition fails. There is no window in which a
 * warrant exists for a transition that did not occur.
 *
 * ## The gate order is deliberate
 *
 * Refusals that can never be lifted come first — a judgment basis, a risk
 * approval, a normative claim reached deterministically — before the policy is
 * consulted at all. Two reasons. An operator who wrote a rule authorising one
 * of them has made a mistake Atlas should refuse rather than obey, so the file
 * must not get a chance to matter; and a FORBIDDEN answer should never depend
 * on configuration, because then it would read as something more evidence could
 * change.
 *
 * Only then does the policy decide, and only then do the thresholds.
 *
 * ## What "shadow" produces
 *
 * A full audit row with `verdict = SHADOW`, recording exactly what Atlas would
 * have done. `atlas_db_verify_warrant_check` requires `verdict = 'AUTO'`, so a
 * shadow row can never be spent — shadow mode is a complete result that is
 * structurally incapable of becoming an action.
 */
#include "atlas/verifypolicy.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/decision_ops.h"
#include "atlas/verify.h"

/* A compact, fixed-vocabulary description of the evidence a decision rested on,
 * stored in the audit row so a reconstruction does not depend on the evidence
 * tables still holding what they held. Counted facts only: no repository byte
 * and no model byte reaches it, which is why it can be reported unencoded. */
static void evidence_snapshot(const atlas_verify_aggregate *agg, char *out, size_t out_size) {
    (void)snprintf(out, out_size,
                   "support=%d contradict=%d inconclusive=%d groups=%d families=%d "
                   "support_mass=%lld contradict_mass=%lld stale=%d",
                   agg->support_count, agg->contradict_count, agg->inconclusive_count,
                   agg->independent_groups, agg->independent_families,
                   (long long)agg->support_mass, (long long)agg->contradict_mass,
                   agg->stale ? 1 : 0);
}

static void reasons_join(const atlas_verify_aggregate *agg, char *out, size_t out_size) {
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < agg->reason_count; i++) {
        const char *name = atlas_verify_reason_name(agg->reasons[i]);
        int wrote = snprintf(out + n, out_size > n ? out_size - n : 0, "%s%s", n > 0 ? "," : "",
                             name);
        if (wrote < 0 || (size_t)wrote >= (out_size > n ? out_size - n : 0)) {
            break;
        }
        n += (size_t)wrote;
    }
}

/* Which transition, if any, this record and this claim could be a candidate
 * for. Derived from the record's current state and kind, never from anything a
 * caller supplied. */
static bool candidate_transition(atlas_decision_kind kind, atlas_decision_state current,
                                 atlas_decision_state *from_out, atlas_decision_state *to_out,
                                 atlas_decision_op_kind *op_out) {
    if (current == ATLAS_DECISION_PROPOSED) {
        *from_out = ATLAS_DECISION_PROPOSED;
        *to_out = ATLAS_DECISION_APPROVED;
        *op_out = ATLAS_DECISION_OP_AUTO_APPROVE;
        return true;
    }
    if (current == ATLAS_DECISION_APPROVED && atlas_decision_kind_resolvable(kind)) {
        *from_out = ATLAS_DECISION_APPROVED;
        *to_out = ATLAS_DECISION_RESOLVED;
        *op_out = ATLAS_DECISION_OP_AUTO_RESOLVE;
        return true;
    }
    return false;
}

void atlas_verify_assessment_init(atlas_verify_assessment *a) {
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof *a);
    atlas_verify_aggregate_init(&a->aggregate);
    a->check = ATLAS_CHECK_UNAVAILABLE;
    a->verifier = ATLAS_VERIFIER_NONE;
    /* A9.2.2. Every zero here already means the safe thing — UNKNOWN truth,
     * UNKNOWN coverage on every dimension, NONE reason — which is why the
     * vocabularies were shaped that way. Stated rather than relied on. */
    a->truth = ATLAS_TRUTH_UNKNOWN;
    a->truth_reason = ATLAS_TREASON_NONE;
    atlas_verify_coverage_report_init(&a->coverage);
}

/* Assesses one claim: runs its deterministic verifier if it has one, folds the
 * attestations, applies the policy, and decides what — if anything — may
 * happen. **Writes nothing.** Every caller that wants an action calls
 * `atlas_verify_autolifecycle_run`, which performs this and then acts on it
 * inside one transaction. Keeping the assessment side-effect-free is what lets
 * the same code answer "what would you do?" for a shadow report and a GUI. */
atlas_status atlas_verify_assess(atlas_db *db, const atlas_verifypolicy *policy, int64_t claim_id,
                                 atlas_verify_assessment *out, atlas_err *err) {
    if (out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "nowhere to put the assessment");
    }
    atlas_verify_assessment_init(out);

    atlas_verify_claim claim;
    atlas_verify_claim_init(&claim);
    bool found = false;
    atlas_status st = atlas_db_verify_claim_get(db, claim_id, &claim, &found, err);
    if (st != ATLAS_OK || !found) {
        atlas_verify_claim_free(&claim);
        return st != ATLAS_OK ? st
                              : atlas_err_set(err, ATLAS_ERR_USAGE, "no claim has that id");
    }
    out->claim_id = claim.id;
    out->document_id = claim.document_id;
    out->revision_id = claim.revision_id;
    out->semantics = claim.semantics;

    /* --- A9.2.1, §5: SOURCE_DRIFT -----------------------------------------
     *
     * The claim says which repository state it is true of. Atlas asks its own
     * index which state it currently describes. If those disagree, everything
     * below — the verifier's answer, the evidence, the fold — is about a tree
     * the repository has since left.
     *
     * Detected here, once, because both the read path and the write path come
     * through this function: a check in `atlas_verify_autolifecycle_run` alone
     * would let `verify show` report a confident verdict that the recording path
     * would then refuse, and the two must not disagree.
     *
     * The enforcement is the existing fold. `SOURCE_DRIFT` carries BLOCKED in
     * `REASONS[]`, `atlas_verify_aggregate_note` folds before it records, and
     * `actionable` is computed from the folded verdict — so a drifting claim
     * cannot reach a transition and no code below has to remember that. The
     * result is still published, still bound to the snapshot it examined, and
     * still true of that snapshot; what it may not do is move a lifecycle
     * state.
     *
     * Both commits are recorded even when they agree, so a stored result always
     * says what it was of rather than only saying so when something went
     * wrong. */
    (void)snprintf(out->claim_commit, sizeof out->claim_commit, "%s",
                   atlas_buf_cstr(&claim.basis_commit));
    if (claim.repo_id != 0) {
        atlas_repo_info repo;
        atlas_repo_info_init(&repo);
        bool repo_found = false;
        st = atlas_db_repo_get_by_id(db, claim.repo_id, &repo, &repo_found, err);
        if (st != ATLAS_OK) {
            atlas_repo_info_free(&repo);
            atlas_verify_claim_free(&claim);
            return st;
        }
        if (repo_found) {
            (void)snprintf(out->evaluated_commit, sizeof out->evaluated_commit, "%s",
                           repo.scanned_head);
        }
        atlas_repo_info_free(&repo);
        (void)atlas_db_verify_sem_generation(db, claim.repo_id, &out->sem_generation, err);
    }
    /* Only when both are known. An unindexed repository has no head, and
     * "Atlas could not look" is not "the ground moved" — reporting the second
     * for the first would make an ordinary unindexed fixture look like
     * tampering. */
    if (out->claim_commit[0] != '\0' && out->evaluated_commit[0] != '\0' &&
        strcmp(out->claim_commit, out->evaluated_commit) != 0) {
        out->source_drift = true;
    }

    /* The basis follows from what is available, and the order is the phase's
     * central rule made operational: a claim with a named deterministic
     * verifier is verified deterministically, whatever anybody has said about
     * it, and a normative claim is a judgment however much evidence exists. */
    atlas_verify_verifier verifier = ATLAS_VERIFIER_NONE;
    (void)atlas_verify_verifier_parse(atlas_buf_cstr(&claim.verifier), &verifier);
    out->verifier = verifier;

    atlas_verify_basis basis;
    if (claim.semantics == ATLAS_CLAIM_NORMATIVE) {
        basis = ATLAS_VERIFY_BASIS_JUDGMENT;
    } else if (verifier != ATLAS_VERIFIER_NONE) {
        basis = ATLAS_VERIFY_BASIS_DETERMINISTIC;
    } else {
        basis = ATLAS_VERIFY_BASIS_EMPIRICAL;
    }
    out->basis = basis;

    /* Run the verifier. It is a read and creates no process; see detverify.c.
     *
     * A9.2.2: it also fills the coverage report, and it fills it *before* it
     * decides the check — so a negative conclusion whose coverage is
     * insufficient comes back UNAVAILABLE rather than FAIL, and the check axis
     * and the truth axis cannot end up disagreeing about one evaluation. */
    if (basis == ATLAS_VERIFY_BASIS_DETERMINISTIC) {
        st = atlas_verify_run_verifier(db, verifier, claim.repo_id,
                                       atlas_buf_cstr(&claim.verifier_input), &out->check,
                                       &out->coverage, out->verified_scope,
                                       sizeof out->verified_scope, out->detail, sizeof out->detail,
                                       err);
        if (st != ATLAS_OK) {
            atlas_verify_claim_free(&claim);
            return st;
        }
    }

    /* Fold the attestations. Even on the deterministic path: the score and the
     * dissent are still worth reporting, and §63 requires a contradiction to
     * stay visible rather than be erased by a confident verdict. */
    char stale_before[ATLAS_TS_MAX];
    long long age = policy != NULL && policy->max_evidence_age > 0
                        ? policy->max_evidence_age
                        : ATLAS_VERIFY_DEFAULT_MAX_EVIDENCE_AGE;
    atlas_iso8601_before_now(stale_before, sizeof stale_before, age * 1000);

    atlas_verify_inputs inputs;
    memset(&inputs, 0, sizeof inputs);
    st = atlas_db_verify_inputs_load(db, claim.id, stale_before, &inputs, err);
    if (st != ATLAS_OK) {
        atlas_verify_claim_free(&claim);
        return st;
    }
    (void)atlas_verify_independent_groups(inputs.items, inputs.count, inputs.dep_from,
                                          inputs.dep_to, inputs.dep_count);
    atlas_verify_aggregate_compute(&out->aggregate, inputs.items, inputs.count, basis);
    out->attestation_total = inputs.total;
    if (inputs.limit_reached) {
        /* A6's rule: a bound that is reached is reported, and a truncated fold
         * can never read as a complete one. Recorded as a conflict rather than
         * as "not verified", because what happened is that Atlas could not see
         * all the evidence — which is a different statement from the evidence
         * being insufficient, and blocks for a different reason. */
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_CONFLICT);
        out->truncated = true;
    }
    /* §5. Noted after the fold is computed, so the reason weakens the verdict
     * the way every other reason does rather than being a special case checked
     * somewhere else. `atlas_verify_aggregate_note` folds before it records, so
     * a drifting claim reports BLOCKED however good its evidence was about the
     * tree it examined. */
    if (out->source_drift) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_SOURCE_DRIFT);
    }
    atlas_verify_inputs_free(&inputs);

    /* The deterministic verdict overrides the fold entirely. This is where
     * "deterministic verification is not reliability aggregation" stops being a
     * documentation claim and becomes the code's behaviour: a mechanical PASS
     * establishes the claim however few sources like it, and a mechanical FAIL
     * contradicts it however many do. */
    if (basis == ATLAS_VERIFY_BASIS_DETERMINISTIC) {
        switch (out->check) {
        case ATLAS_CHECK_PASS:
            out->aggregate.state = ATLAS_VERIFY_VERIFIED;
            out->aggregate.deterministic_pass = true;
            /* A mechanically established claim is not scored against opinion.
             * 100 here is not a probability and is not presented as one; it is
             * the score field carrying "the truth condition was met". */
            out->aggregate.confidence = 100;
            break;
        case ATLAS_CHECK_FAIL:
            out->aggregate.state = ATLAS_VERIFY_CONTRADICTED;
            out->aggregate.deterministic_fail = true;
            out->aggregate.confidence = 0;
            break;
        case ATLAS_CHECK_UNAVAILABLE:
            /* Could not look. Not a fail, and not evidence of anything: the
             * state falls back to whatever the attestations said, which for a
             * fresh claim is UNVERIFIED. */
            break;
        }
    }

    /* --- A9.2.2: the truth axis ------------------------------------------
     *
     * Computed here, after the check has settled and the drift is known, and
     * computed by `atlas_verify_truth_of` — **the only producer of
     * `ATLAS_TRUTH_ABSENT` in Atlas**. Nothing in this file assigns that value,
     * no request carries it and no intake verb accepts one, which is the same
     * single-write-point shape `settle()` and `atlas_decision_apply_in_tx`
     * have, applied to a value rather than to a table.
     *
     * It is a *fourth* axis and is derived from none of the other three. An
     * OBLIGATION (kind) that is APPROVED (status) may be VERIFIED (verification
     * state) and ABSENT (truth) — that combination is exactly what discharges
     * it — and every one of the four is reported separately. */
    out->truth = atlas_verify_truth_of(verifier, basis, claim.semantics, out->check, &out->coverage,
                                       &out->truth_reason);

    /* §19. A result about the tree Atlas has indexed is not a result about the
     * tree the claim is bound to. Both directions are demoted, not just the
     * negative one: a symbol found in the index at commit Y may have been added
     * after the claim's commit X, so PRESENT is no safer than ABSENT across a
     * drift.
     *
     * NOT_VERIFIABLE survives, because a normative proposition does not become
     * a factual one by the repository moving. */
    if (out->source_drift && out->truth != ATLAS_TRUTH_NOT_VERIFIABLE) {
        out->truth = ATLAS_TRUTH_UNKNOWN;
        out->truth_reason = ATLAS_TREASON_SOURCE_DRIFT;
    }

    /* Calibration. On this machine, and on any machine where this phase has
     * just been installed, there is no resolved history, so this stays
     * INSUFFICIENT_DATA and `calibrated_probability` stays -1. That is reported
     * plainly rather than papered over — and, on the deterministic path, it is
     * not an input to anything. */
    out->aggregate.calibration = ATLAS_CALIBRATION_INSUFFICIENT_DATA;
    out->aggregate.calibrated_probability = -1;

    /* --- the gates, in order --------------------------------------------- */

    /* The record's own state and kind, read from the document. */
    atlas_decision_kind kind = ATLAS_DECISION_KIND_DECISION;
    atlas_decision_state current = ATLAS_DECISION_PROPOSED;
    if (claim.document_id > 0) {
        bool kfound = false;
        st = atlas_db_decision_kind_of(db, claim.document_id, &kind, &kfound, err);
        if (st == ATLAS_OK) {
            char status[24];
            st = atlas_db_decision_document_status(db, claim.document_id, status, sizeof status,
                                                   err);
            if (st == ATLAS_OK) {
                (void)atlas_decision_state_parse(status, &current);
            }
        }
        if (st != ATLAS_OK) {
            atlas_verify_claim_free(&claim);
            return st;
        }
    }
    out->kind = kind;
    out->from = current;

    atlas_decision_state from = current, to = current;
    atlas_decision_op_kind opk = ATLAS_DECISION_OP_AUTO_APPROVE;
    bool have_candidate = claim.document_id > 0 &&
                          candidate_transition(kind, current, &from, &to, &opk);
    out->to = to;
    out->op = opk;

    /* Gate 1 — refusals no policy may lift. Before the policy is read. */
    if (basis == ATLAS_VERIFY_BASIS_JUDGMENT) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_JUDGMENT_REQUIRES_AUTHORITY);
    }
    if (claim.semantics == ATLAS_CLAIM_NORMATIVE &&
        !atlas_verify_basis_may_verify_semantics(basis, claim.semantics)) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_NORMATIVE_CLAIM);
    }
    if (have_candidate) {
        atlas_verify_reason why = ATLAS_VREASON_NONE;
        if (atlas_verifypolicy_transition_forbidden(kind, from, to, &why)) {
            atlas_verify_aggregate_note(&out->aggregate, why);
        }
    } else {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_TRANSITION_ILLEGAL);
    }

    /* Gate 2 — the policy. */
    const atlas_verifypolicy_rule *rule = NULL;
    if (policy == NULL || policy->state != ATLAS_VERIFYPOLICY_ENABLED) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_NO_POLICY);
    } else if (have_candidate) {
        rule = atlas_verifypolicy_find(policy, kind, from, to);
        if (rule == NULL) {
            atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_NOT_ALLOWED);
        } else if (rule->verifier != verifier || verifier == ATLAS_VERIFIER_NONE) {
            /* The rule names the verifier that must have established the claim.
             * A claim verified by a different mechanism than the one the policy
             * authorised is not the thing the policy authorised. */
            atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_NOT_ALLOWED);
            rule = NULL;
        }
    }

    /* Gate 3 — the evidence itself. */
    if (out->aggregate.state != ATLAS_VERIFY_VERIFIED) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_NOT_VERIFIED);
    }
    if (out->aggregate.deterministic_fail) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_CONFLICT);
    }
    if (policy != NULL && policy->state == ATLAS_VERIFYPOLICY_ENABLED) {
        if (out->aggregate.confidence < policy->min_confidence) {
            atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_LOW_CONFIDENCE);
        }
        /* Independence is required of the empirical path and **not** of the
         * deterministic one, and the asymmetry is the point. Independent
         * corroboration is what substitutes for a proof; where there is a
         * proof, asking for corroboration of it is asking a mechanical check to
         * be confirmed by opinion. */
        if (basis == ATLAS_VERIFY_BASIS_EMPIRICAL &&
            out->aggregate.independent_groups < policy->min_evidence_groups) {
            atlas_verify_aggregate_note(&out->aggregate,
                                        ATLAS_VREASON_INSUFFICIENT_INDEPENDENCE);
        }
    }
    if (out->aggregate.stale && !out->aggregate.deterministic_pass) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_STALE_EVIDENCE);
    }

    /* §18. **UNKNOWN must never satisfy a policy condition that requires
     * ABSENT.** A machine transition that rests on a deterministic verifier is
     * a claim that Atlas established something about the world, so the truth
     * axis has to have established it.
     *
     * Largely belt-and-braces by construction — a negative conclusion with
     * insufficient coverage never reaches PASS, so it is never VERIFIED, so it
     * is already BLOCKED by `NOT_VERIFIED` above. It is checked anyway, for the
     * reason A6 gives about asserting a permissive verdict deliberately: a
     * guarantee that holds only because three other gates happen to catch it is
     * one a later edit to any of the three can delete silently. The distinct
     * reason also tells an auditor *which* of the two happened — thin evidence,
     * or a look that never covered the ground it would have had to. */
    if (basis == ATLAS_VERIFY_BASIS_DETERMINISTIC && have_candidate &&
        !atlas_verify_truth_is_established(out->truth)) {
        atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_COVERAGE_INSUFFICIENT);
    }

    /* Gate 4 — calibration, for the empirical path only.
     *
     * **This is the rule the whole phase turns on.** The check is guarded on
     * the basis, so a deterministic verdict is never blocked by an actor's
     * sample count. Blocking it would not be caution: how often some model has
     * been right in the past is not an input to a mechanical evaluation of a
     * bounded truth condition, and making it a precondition would be a category
     * error wearing conservatism's clothes. */
    if (atlas_verify_basis_requires_calibration(basis)) {
        if (out->aggregate.calibration != ATLAS_CALIBRATION_CALIBRATED) {
            atlas_verify_aggregate_note(&out->aggregate, ATLAS_VREASON_CALIBRATION_INSUFFICIENT);
        }
    }

    /* Gate 5 — enforcement, per path.
     *
     * Reached only when nothing above objected, and the permissive verdict is
     * **asserted here deliberately** rather than arrived at. That is A6's
     * arrangement exactly: `atlas_gate_report_init` starts at BLOCKED so the
     * engine has to commit to PASS at the one point it has checked everything,
     * and a report that started at its safe default can never be lifted out of
     * it by accident. The same holds here — `NEEDS_REVIEW` is zero, the fold
     * only ever weakens, so AUTO is unreachable except by this statement.
     *
     * `reason_total` rather than `reason_count`, because a reason that
     * overflowed the list still objected. */
    if (out->aggregate.reason_total == 0) {
        out->aggregate.verdict = ATLAS_POLICY_AUTO;
        bool enforce = basis == ATLAS_VERIFY_BASIS_DETERMINISTIC
                           ? (policy != NULL && policy->deterministic_enforce)
                           : (policy != NULL && policy->empirical_enforce);
        /* Both branches fold: OK keeps AUTO, SHADOW_MODE weakens it to SHADOW.
         * Neither can strengthen anything, which is what keeps this statement
         * the only way out of the safe default. */
        atlas_verify_aggregate_note(&out->aggregate,
                                    enforce ? ATLAS_VREASON_OK : ATLAS_VREASON_SHADOW_MODE);
    }

    out->actionable = out->aggregate.verdict == ATLAS_POLICY_AUTO && have_candidate && rule != NULL;
    atlas_verify_claim_free(&claim);
    return ATLAS_OK;
}

atlas_status atlas_verify_autolifecycle_run(atlas_db *db, const atlas_verifypolicy *policy,
                                            int64_t claim_id, const char *repo_name,
                                            atlas_verify_assessment *out, atlas_err *err) {
    atlas_verify_assessment local;
    atlas_verify_assessment *a = out != NULL ? out : &local;
    atlas_status st = atlas_verify_assess(db, policy, claim_id, a, err);
    if (st != ATLAS_OK) {
        return st;
    }

    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof now);

    st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The result row first: it is the justification the audit row points at,
     * and a warrant naming a result that does not exist is not a justification. */
    int64_t result_id = 0;
    /* §5. The result records the pair of commits the assessment compared and
     * whether they disagreed, so a reader years later can tell what tree this
     * verdict was about without the repository having to still be at it. */
    const atlas_verify_source_binding binding = {
        a->claim_commit[0] != '\0' ? a->claim_commit : NULL,
        a->evaluated_commit[0] != '\0' ? a->evaluated_commit : NULL,
        a->sem_generation,
        a->source_drift,
    };
    /* A9.2.2. The truth axis travels with the result, so a row read years later
     * says what Atlas concluded about the subject and what coverage that rested
     * on — not merely how strong the evidence was. */
    const atlas_verify_truth_record truth_record = {a->truth, a->truth_reason, &a->coverage};
    st = atlas_db_verify_result_insert(db, claim_id, &a->aggregate,
                                       atlas_verify_verifier_name(a->verifier), a->check, &binding,
                                       &truth_record, now, &result_id, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        return st;
    }
    a->result_id = result_id;

    /* Nothing to record a lifecycle intent about. A claim on no record, or one
     * whose record is in a state no transition leaves, still produced a
     * verification result — which is the useful output — and no audit row,
     * because no transition was contemplated. */
    if (a->document_id <= 0) {
        st = atlas_db_commit(db, err);
        if (st != ATLAS_OK) {
            atlas_db_rollback(db);
        }
        return st;
    }

    /* The revision the transition would touch, and its content hash. Read here
     * rather than in the decision layer so the audit row binds to exactly what
     * the write point will check. */
    int64_t revision_id = 0;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    content_hash[0] = '\0';
    {
        char state[16];
        int64_t rev_no = 0;
        if (a->op == ATLAS_DECISION_OP_AUTO_RESOLVE) {
            st = atlas_db_decision_approved_revision(db, a->document_id, &revision_id, err);
            if (st == ATLAS_OK && revision_id > 0) {
                atlas_decision_revision loaded;
                atlas_decision_revision_init(&loaded);
                bool lf = false;
                st = atlas_db_decision_revision_load(db, revision_id, &loaded, &lf, err);
                if (st == ATLAS_OK && lf) {
                    (void)snprintf(content_hash, sizeof content_hash, "%s", loaded.content_hash);
                }
                atlas_decision_revision_free(&loaded);
            }
        } else {
            st = atlas_db_decision_latest_revision(db, a->document_id, &revision_id, &rev_no,
                                                   content_hash, sizeof content_hash, state,
                                                   sizeof state, err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        return st;
    }
    a->revision_id = revision_id;

    char reasons[512];
    char snapshot[512];
    reasons_join(&a->aggregate, reasons, sizeof reasons);
    evidence_snapshot(&a->aggregate, snapshot, sizeof snapshot);

    atlas_verify_audit audit;
    memset(&audit, 0, sizeof audit);
    audit.claim_id = claim_id;
    audit.result_id = result_id;
    audit.document_id = a->document_id;
    audit.revision_id = revision_id;
    audit.content_hash = content_hash;
    audit.kind = atlas_decision_kind_name(a->kind);
    audit.from_status = atlas_decision_state_name(a->from);
    audit.to_status = atlas_decision_state_name(a->to);
    audit.basis = a->basis;
    audit.verdict = a->aggregate.verdict;
    audit.reasons = reasons;
    audit.policy_id = policy != NULL ? policy->policy_id : "";
    audit.policy_hash = policy != NULL ? policy->policy_hash : "";
    audit.algorithm = a->aggregate.algorithm;
    audit.prior_version = ATLAS_VERIFY_PRIOR_VERSION;
    audit.family_version = a->aggregate.family_version;
    audit.confidence = a->aggregate.confidence;
    audit.calibration = a->aggregate.calibration;
    audit.calibrated_probability = a->aggregate.calibrated_probability;
    audit.independent_groups = a->aggregate.independent_groups;
    audit.evidence_snapshot = snapshot;
    audit.verifier = atlas_verify_verifier_name(a->verifier);
    audit.check_result = a->check;
    audit.binary_id = ATLAS_VERSION_STRING;

    /* A shadow row is written with verdict SHADOW and is therefore permanently
     * unusable as a warrant — `atlas_db_verify_warrant_check` requires AUTO. It
     * is a complete record of what Atlas would have done, structurally
     * incapable of becoming what Atlas did. */
    int64_t warrant_id = 0;
    st = atlas_db_verify_audit_insert(db, &audit, now, &warrant_id, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        return st;
    }
    a->audit_id = warrant_id;

    if (!a->actionable || a->aggregate.verdict != ATLAS_POLICY_AUTO) {
        st = atlas_db_commit(db, err);
        if (st != ATLAS_OK) {
            atlas_db_rollback(db);
        }
        return st;
    }

    /* The transition, inside the same transaction as its justification. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, a->op);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    op.warrant_id = warrant_id;
    st = atlas_buf_set_str(&op.repo_name, repo_name != NULL ? repo_name : "", err);
    if (st == ATLAS_OK) {
        st = atlas_db_decision_uid_of(db, a->document_id, &op.uid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_decision_apply_in_tx(db, &op, &res, err);
    }
    if (st == ATLAS_OK) {
        a->transitioned = true;
    }
    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);

    if (st != ATLAS_OK) {
        /* Whole or nothing. A spent warrant with no transition, or a transition
         * with no audit row, is the one outcome this table must never hold. */
        atlas_db_rollback(db);
        return st;
    }
    st = atlas_db_commit(db, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
    }
    return st;
}

/* Atlas - A9.2: the root-owned verification policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What this decides
 *
 * Whether Atlas may change a knowledge record's lifecycle state **by itself**,
 * and if so: for exactly which knowledge kinds, exactly which transitions,
 * established by exactly which deterministic verifier, and above which
 * thresholds of confidence, independence, freshness and calibration.
 *
 * ## Why it is a root-owned file
 *
 * The principal it constrains is Atlas' own verification engine — and,
 * transitively, everything that can put evidence in front of that engine, which
 * includes every model with an MCP connection. A9 states the general form of
 * this argument: a component that could widen its own authority is not
 * constrained by anything. Here the authority in question is the power to make
 * a proposal into project policy without a person, which is the most valuable
 * thing in Atlas to be able to forge.
 *
 * So the policy is reached through `atlas_rootpath_open`, from `/`, with no
 * symlink traversed and every component owned by uid 0 and writable by nobody
 * else — the same walk A7's authority policy, A7.1's system policy, A8's
 * orchestration policy and A9's gateway policy use, for the same reason and
 * with the same failure behaviour.
 *
 * `ATLAS_VERIFYPOLICY_PATH` is a **compiled-in constant**. No environment
 * override, no flag, no data-directory-relative variant. A caller that can
 * choose the policy is not constrained by it, and adding a way to choose it
 * would delete the phase.
 *
 * ## Fail-closed at zero, in every direction
 *
 * `ATLAS_VERIFYPOLICY_DISABLED` is zero and a zeroed struct automates nothing:
 * no rule, no enforcement, thresholds at their most demanding. A policy that is
 * missing, unreadable, malformed, symlinked, or owned or writable by anyone but
 * root leaves every path manual, with a reason. There is no direction in which
 * a degraded policy automates more.
 *
 * An **unrecognised key is an error**, not something skipped. A7.1's rule, and
 * here the stakes are at their highest: the thing an author most plausibly
 * believes they configured, and Atlas most plausibly never read, is a
 * restriction.
 *
 * ## Enforcement is per path, not global
 *
 * This is the structural expression of the phase's central rule. There are
 * separate switches for the deterministic and empirical paths, so a machine
 * with no calibration data whatsoever can enforce mechanically-verified
 * transitions while the empirical path stays in shadow. Tying them to one flag
 * would make an unrelated statistic — how often some model has been right — a
 * precondition for acting on a proof, which is a category error rather than
 * caution.
 *
 * **There is no judgment switch, and its absence is the design.** A normative
 * choice is not a discoverable fact, so there is nothing a verifier could
 * establish and no threshold that would mean anything. A policy may still
 * *execute* on a judgment-adjacent record — an obligation opened because a
 * root-owned rule already says a condition is forbidden — but that is the
 * policy's pre-existing decision being applied, not a new one being made.
 *
 * ## What a rule can and cannot say
 *
 * A rule names a kind, a from-state, a to-state and a verifier. It cannot name
 * a document, an actor, a model or a repository, and that is deliberate: a
 * policy that could bless one specific record would be a way to launder a
 * single approval through a file, and a policy that could bless one specific
 * *source* would be reliability being written down as authority.
 *
 * Some pairs can never appear whatever a policy says, and the engine refuses
 * them rather than trusting the file:
 *
 *   - anything reaching `ACCEPTED_RISK → APPROVED`. Atlas can verify that a
 *     risk exists and that it has been mitigated; accepting it is a claim about
 *     what the project is willing to live with, which has an owner.
 *   - anything whose claim is NORMATIVE on the deterministic path.
 *   - `REJECTED` as a target. §34: auto-rejection needs an explicit
 *     falsification condition, and Atlas does not have one, so the transition
 *     is **absent** rather than refused — the house pattern, because an absent
 *     path cannot be weakened by a later edit and a refusing one can.
 */
#ifndef ATLAS_VERIFYPOLICY_H
#define ATLAS_VERIFYPOLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/limits.h"
#include "atlas/sha256.h"
#include "atlas/verify.h"

/* Compiled in. See above: this is not configurable and must never become so. */
#ifndef ATLAS_VERIFYPOLICY_PATH
#define ATLAS_VERIFYPOLICY_PATH "/etc/atlas/verification.conf"
#endif

typedef enum atlas_verifypolicy_state {
    /* Zero. Nothing is automatic. */
    ATLAS_VERIFYPOLICY_DISABLED = 0,
    ATLAS_VERIFYPOLICY_ENABLED
} atlas_verifypolicy_state;

const char *atlas_verifypolicy_state_name(atlas_verifypolicy_state s);

typedef enum atlas_verifypolicy_reason {
    ATLAS_VERIFYPOLICY_REASON_UNKNOWN = 0,
    ATLAS_VERIFYPOLICY_REASON_ABSENT,
    ATLAS_VERIFYPOLICY_REASON_PATH_UNSAFE,
    ATLAS_VERIFYPOLICY_REASON_WRITABLE,
    ATLAS_VERIFYPOLICY_REASON_MALFORMED,
    ATLAS_VERIFYPOLICY_REASON_DISABLED,
    ATLAS_VERIFYPOLICY_REASON_ACTIVE
} atlas_verifypolicy_reason;

const char *atlas_verifypolicy_reason_name(atlas_verifypolicy_reason r);
const char *atlas_verifypolicy_reason_detail(atlas_verifypolicy_reason r);

/* One transition this policy permits Atlas to make on its own.
 *
 * Four fields and no more. Everything that would let a rule pick out an
 * individual record, source or repository is deliberately not expressible. */
typedef struct atlas_verifypolicy_rule {
    atlas_decision_kind kind;
    atlas_decision_state from;
    atlas_decision_state to;
    /* The deterministic verifier that must have passed. `ATLAS_VERIFIER_NONE`
     * is not writable in a rule: a deterministic rule with no named verifier
     * would be a rule that any evidence at all satisfies. */
    atlas_verify_verifier verifier;
} atlas_verifypolicy_rule;

typedef struct atlas_verifypolicy {
    atlas_verifypolicy_state state;
    atlas_verifypolicy_reason reason;
    char detail[512];

    /* An operator-chosen label, reported in every audit row so an auditor can
     * tell which generation of policy authorised a transition even after the
     * file has changed. Defaults to a fixed string rather than to empty. */
    char policy_id[128];
    /* SHA-256 of the exact policy bytes. The audit row records it, so a
     * reconstruction does not depend on the file still saying what it said —
     * §40's requirement, and the reason the hash is over bytes rather than over
     * the parsed struct. */
    char policy_hash[ATLAS_SHA256_HEX_LEN + 1u];

    /* Per-path enforcement. Both default false: a policy that says `enabled`
     * and nothing else records only shadow verdicts, which is the correct
     * first deployment. */
    bool deterministic_enforce;
    bool empirical_enforce;

    /* Thresholds. Defaults are the most demanding values that still permit the
     * deterministic path to function, so an absent key never loosens anything. */
    int min_confidence;          /* 0..100 */
    int min_evidence_groups;     /* genuinely independent groups */
    long long max_evidence_age;  /* seconds */
    int min_calibration_samples; /* empirical path only */

    atlas_verifypolicy_rule rules[ATLAS_VERIFY_MAX_POLICY_RULES];
    size_t rule_count;
} atlas_verifypolicy;

/* Loads from the compiled-in path. Never fails: a policy that cannot be read
 * safely is a disabled policy with a reason, which is the only useful shape for
 * a fail-closed loader. */
void atlas_verifypolicy_load(atlas_verifypolicy *out);
/* The same, from an explicit path. For tests and for `atlas verify policy
 * --path`; the daemon and the engine always use the compiled-in constant. */
void atlas_verifypolicy_load_at(const char *path, atlas_verifypolicy *out);
/* Parses bytes already read. Exposed so the malformed matrix in
 * `tests/test_verify_policy.c` can drive every case without creating root-owned
 * files, which a test cannot do. */
void atlas_verifypolicy_parse_buffer(const char *buf, size_t len, atlas_verifypolicy *out);

/* Whether this policy permits this exact transition, and under which verifier.
 *
 * Returns NULL when no rule matches. The engine asks this **after** it has
 * independently refused the pairs no policy may authorise, so a matching rule
 * is necessary and never sufficient. */
const atlas_verifypolicy_rule *atlas_verifypolicy_find(const atlas_verifypolicy *p,
                                                       atlas_decision_kind kind,
                                                       atlas_decision_state from,
                                                       atlas_decision_state to);

/* Transitions no policy may ever authorise, asked before the policy is
 * consulted.
 *
 * A function rather than a comment because it is a security boundary: the file
 * is root-owned, but "root wrote it" is not the same as "Atlas should do it",
 * and an operator who writes `allow = ACCEPTED_RISK APPROVED ...` has made a
 * mistake Atlas should refuse rather than obey. Writes the reason so the
 * refusal explains itself. */
bool atlas_verifypolicy_transition_forbidden(atlas_decision_kind kind, atlas_decision_state from,
                                             atlas_decision_state to, atlas_verify_reason *why);

/* --- the assessment -------------------------------------------------------
 *
 * Everything the engine concluded about one claim, and enough to explain all of
 * it. §25: "why does Atlas say this is 96?" must have a structured answer, so
 * this is counted facts and closed vocabularies with no prose verdict anywhere.
 * The `detail` and `verified_scope` strings are Atlas' own sentences from
 * `src/verify/detverify.c`; no repository byte and no model byte reaches
 * either. */
typedef struct atlas_verify_assessment {
    int64_t claim_id;
    int64_t document_id;
    int64_t revision_id;
    int64_t result_id;
    int64_t audit_id;

    atlas_decision_kind kind;
    atlas_decision_state from;
    atlas_decision_state to;
    atlas_decision_op_kind op;

    atlas_verify_basis basis;
    atlas_verify_claim_semantics semantics;
    atlas_verify_verifier verifier;
    atlas_verify_check check;
    atlas_verify_aggregate aggregate;

    /* The true number of attestations, even when more existed than were folded,
     * and whether that bound was reached. A6's rule. */
    size_t attestation_total;
    bool truncated;

    /* What the verifier actually established, in Atlas' words, so a claim that
     * outruns its verifier can be seen to. */
    char verified_scope[512];
    char detail[512];

    /* --- A9.2.1, §4/§5: what this assessment is *of* ---------------------
     *
     * `claim_commit` is the repository state the claim was bound to when it was
     * written. `evaluated_commit` is the state Atlas had indexed when the
     * aggregation ran. They are normally equal and the interesting case is when
     * they are not.
     *
     * `source_drift` records that they disagreed. It is stored on the result
     * rather than derived later because the repository will have moved again by
     * the time anybody reads the row, so a derivation would answer a different
     * question every time it ran.
     *
     * A drifting assessment notes `ATLAS_VREASON_SOURCE_DRIFT`, which folds to
     * BLOCKED through `REASONS[]` — so `actionable` is false and no machine
     * transition is possible, without a second rule anywhere deciding that. */
    char claim_commit[ATLAS_OID_HEX_MAX + 1u];
    char evaluated_commit[ATLAS_OID_HEX_MAX + 1u];
    int64_t sem_generation;
    bool source_drift;

    /* Every gate passed and a rule authorised it. Distinct from
     * `aggregate.verdict == AUTO`, which is about the gates alone: this also
     * requires a candidate transition and a matching rule to exist. */
    bool actionable;
    /* Set only by `atlas_verify_autolifecycle_run`, and only when the
     * transition actually committed. */
    bool transitioned;
} atlas_verify_assessment;

void atlas_verify_assessment_init(atlas_verify_assessment *a);

/* Assesses one claim and **writes nothing**. Runs the deterministic verifier if
 * the claim names one, folds the attestations, applies the policy gates in the
 * order the implementation documents, and reports what would happen.
 *
 * Side-effect-free on purpose: the same code answers "what would you do?" for a
 * shadow report, a CLI read and the GUI, so those can never disagree with what
 * the engine would actually do. */
atlas_status atlas_verify_assess(atlas_db *db, const atlas_verifypolicy *policy, int64_t claim_id,
                                 atlas_verify_assessment *out, atlas_err *err);

/* Assesses, records the result and the audit row, and — only when every gate
 * passed and enforcement is on for that path — performs the transition, all in
 * one transaction.
 *
 * This is the **third and last** caller of `atlas_decision_apply_in_tx`, and it
 * qualifies under the rule for adding one: it genuinely owns a wider unit of
 * work, because a machine transition and the audit row justifying it are one
 * fact. An audit row with no transition describes something that did not
 * happen; a transition with no audit row is an automatic change to project
 * knowledge with no recoverable reason. */
atlas_status atlas_verify_autolifecycle_run(atlas_db *db, const atlas_verifypolicy *policy,
                                            int64_t claim_id, const char *repo_name,
                                            atlas_verify_assessment *out, atlas_err *err);

#endif /* ATLAS_VERIFYPOLICY_H */

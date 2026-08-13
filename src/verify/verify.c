/* Atlas - A9.2: the verification vocabularies and the aggregation algorithm.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every table in this file is the single authority on what it describes, asked
 * by the engine and by the tests rather than restated in either. That is A6's
 * discipline about `REASONS[]`: a test that agreed with a second copy of the
 * rules would pass while the rules were wrong.
 *
 * The arithmetic is integer throughout. There is no `double` in this file and
 * there must not be one — "the same inputs produce the same result" has to be a
 * property of the code, not a hope about how a compiler rounds, because a
 * machine lifecycle transition has to be reproducible from its audit row years
 * later on a different machine.
 */
#include "atlas/verify.h"

#include <string.h>

/* --- verification state --------------------------------------------------- */

static const char *const STATE_NAMES[] = {"UNVERIFIED",   "VERIFYING",     "SUPPORTED",
                                          "VERIFIED",     "CONTRADICTED",  "INCONCLUSIVE",
                                          "STALE"};

const char *atlas_verify_state_name(atlas_verify_state s) {
    if ((size_t)s < sizeof STATE_NAMES / sizeof STATE_NAMES[0]) {
        return STATE_NAMES[s];
    }
    return "UNVERIFIED";
}

bool atlas_verify_state_parse(const char *name, atlas_verify_state *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof STATE_NAMES / sizeof STATE_NAMES[0]; i++) {
        if (strcmp(name, STATE_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_state)i;
            }
            return true;
        }
    }
    return false;
}

/* --- basis ---------------------------------------------------------------- */

static const char *const BASIS_NAMES[] = {"UNKNOWN", "DETERMINISTIC", "EMPIRICAL", "JUDGMENT"};

const char *atlas_verify_basis_name(atlas_verify_basis b) {
    if ((size_t)b < sizeof BASIS_NAMES / sizeof BASIS_NAMES[0]) {
        return BASIS_NAMES[b];
    }
    return "UNKNOWN";
}

bool atlas_verify_basis_parse(const char *name, atlas_verify_basis *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof BASIS_NAMES / sizeof BASIS_NAMES[0]; i++) {
        if (strcmp(name, BASIS_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_basis)i;
            }
            return true;
        }
    }
    return false;
}

bool atlas_verify_basis_writable(atlas_verify_basis b) {
    return b == ATLAS_VERIFY_BASIS_DETERMINISTIC || b == ATLAS_VERIFY_BASIS_EMPIRICAL ||
           b == ATLAS_VERIFY_BASIS_JUDGMENT;
}

/* The phase's central rule, as one line of code so that it can be tested and
 * cannot be reintroduced by accident somewhere else. */
bool atlas_verify_basis_requires_calibration(atlas_verify_basis b) {
    return b == ATLAS_VERIFY_BASIS_EMPIRICAL;
}

/* --- descriptive versus normative ----------------------------------------- */

static const char *const SEMANTICS_NAMES[] = {"DESCRIPTIVE", "NORMATIVE"};

const char *atlas_verify_claim_semantics_name(atlas_verify_claim_semantics s) {
    if ((size_t)s < sizeof SEMANTICS_NAMES / sizeof SEMANTICS_NAMES[0]) {
        return SEMANTICS_NAMES[s];
    }
    return "DESCRIPTIVE";
}

bool atlas_verify_claim_semantics_parse(const char *name, atlas_verify_claim_semantics *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof SEMANTICS_NAMES / sizeof SEMANTICS_NAMES[0]; i++) {
        if (strcmp(name, SEMANTICS_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_claim_semantics)i;
            }
            return true;
        }
    }
    return false;
}

/* Separation 4, in one cell.
 *
 * A mechanical verifier reads the world. A normative claim is not about the
 * world. There is therefore nothing for a deterministic verifier to read, and
 * the pair is refused rather than discounted — a discounted forgery of this
 * shape would still print as "VERIFIED · DETERMINISTIC" beside a rule nobody
 * adopted. */
bool atlas_verify_basis_may_verify_semantics(atlas_verify_basis b,
                                             atlas_verify_claim_semantics s) {
    if (s == ATLAS_CLAIM_NORMATIVE) {
        return b == ATLAS_VERIFY_BASIS_JUDGMENT;
    }
    return b == ATLAS_VERIFY_BASIS_DETERMINISTIC || b == ATLAS_VERIFY_BASIS_EMPIRICAL;
}

/* --- verdicts ------------------------------------------------------------- */

static const char *const VERDICT_NAMES[] = {"INCONCLUSIVE", "SUPPORT", "CONTRADICT"};

const char *atlas_verify_verdict_name(atlas_verify_verdict v) {
    if ((size_t)v < sizeof VERDICT_NAMES / sizeof VERDICT_NAMES[0]) {
        return VERDICT_NAMES[v];
    }
    return "INCONCLUSIVE";
}

bool atlas_verify_verdict_parse(const char *name, atlas_verify_verdict *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof VERDICT_NAMES / sizeof VERDICT_NAMES[0]; i++) {
        if (strcmp(name, VERDICT_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_verdict)i;
            }
            return true;
        }
    }
    return false;
}

/* --- actors --------------------------------------------------------------- */

static const char *const ACTOR_CLASS_NAMES[] = {
    "UNKNOWN", "HUMAN",  "AI_AGENT", "TOOL", "TEST", "RUNTIME_OBSERVATION",
    "REPOSITORY_EVIDENCE", "DOCUMENT", "ATLAS_VERIFIER"};

const char *atlas_verify_actor_class_name(atlas_verify_actor_class c) {
    if ((size_t)c < sizeof ACTOR_CLASS_NAMES / sizeof ACTOR_CLASS_NAMES[0]) {
        return ACTOR_CLASS_NAMES[c];
    }
    return "UNKNOWN";
}

bool atlas_verify_actor_class_parse(const char *name, atlas_verify_actor_class *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof ACTOR_CLASS_NAMES / sizeof ACTOR_CLASS_NAMES[0]; i++) {
        if (strcmp(name, ACTOR_CLASS_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_actor_class)i;
            }
            return true;
        }
    }
    return false;
}

static const char *const ACTOR_IDENTITY_NAMES[] = {"SELF_DECLARED", "PEER_AUTHENTICATED",
                                                   "ATLAS_ATTESTED"};

const char *atlas_verify_actor_identity_name(atlas_verify_actor_identity i) {
    if ((size_t)i < sizeof ACTOR_IDENTITY_NAMES / sizeof ACTOR_IDENTITY_NAMES[0]) {
        return ACTOR_IDENTITY_NAMES[i];
    }
    return "SELF_DECLARED";
}

bool atlas_verify_actor_identity_parse(const char *name, atlas_verify_actor_identity *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof ACTOR_IDENTITY_NAMES / sizeof ACTOR_IDENTITY_NAMES[0]; i++) {
        if (strcmp(name, ACTOR_IDENTITY_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_actor_identity)i;
            }
            return true;
        }
    }
    return false;
}

/* The four classes whose entire evidentiary weight is "Atlas did this". */
bool atlas_verify_actor_class_requires_atlas_identity(atlas_verify_actor_class c) {
    switch (c) {
    case ATLAS_ACTOR_TOOL:
    case ATLAS_ACTOR_TEST:
    case ATLAS_ACTOR_RUNTIME_OBSERVATION:
    case ATLAS_ACTOR_ATLAS_VERIFIER:
        return true;
    case ATLAS_ACTOR_UNKNOWN:
    case ATLAS_ACTOR_HUMAN:
    case ATLAS_ACTOR_AI_AGENT:
    case ATLAS_ACTOR_REPOSITORY_EVIDENCE:
    case ATLAS_ACTOR_DOCUMENT:
        return false;
    }
    return false;
}

/* --- evidence ------------------------------------------------------------- */

static const char *const EVIDENCE_CLASS_NAMES[] = {
    "UNKNOWN",    "SOURCE_CODE",   "COMPILER", "TEST",           "RUNTIME",
    "DEPLOYED_CONFIG", "GIT_HISTORY", "SPECIFICATION", "DOCUMENT", "ATLAS_KNOWLEDGE",
    "HUMAN_STATEMENT", "AI_ANALYSIS"};

const char *atlas_verify_evidence_class_name(atlas_verify_evidence_class c) {
    if ((size_t)c < sizeof EVIDENCE_CLASS_NAMES / sizeof EVIDENCE_CLASS_NAMES[0]) {
        return EVIDENCE_CLASS_NAMES[c];
    }
    return "UNKNOWN";
}

bool atlas_verify_evidence_class_parse(const char *name, atlas_verify_evidence_class *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof EVIDENCE_CLASS_NAMES / sizeof EVIDENCE_CLASS_NAMES[0]; i++) {
        if (strcmp(name, EVIDENCE_CLASS_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_evidence_class)i;
            }
            return true;
        }
    }
    return false;
}

static const char *const FAMILY_NAMES[] = {"UNKNOWN", "STATIC_ARTIFACT", "DYNAMIC_OBSERVATION",
                                           "INTERPRETATION"};

const char *atlas_verify_evidence_family_name(atlas_verify_evidence_family f) {
    if ((size_t)f < sizeof FAMILY_NAMES / sizeof FAMILY_NAMES[0]) {
        return FAMILY_NAMES[f];
    }
    return "UNKNOWN";
}

/* The taxonomy. Bump ATLAS_VERIFY_FAMILY_VERSION when this map changes: a
 * stored result records the version it was computed under, so an old verdict is
 * never silently reinterpreted by a new grouping. */
atlas_verify_evidence_family atlas_verify_evidence_family_of(atlas_verify_evidence_class c) {
    switch (c) {
    case ATLAS_EVIDENCE_SOURCE_CODE:
    case ATLAS_EVIDENCE_COMPILER:
    case ATLAS_EVIDENCE_GIT_HISTORY:
    case ATLAS_EVIDENCE_SPECIFICATION:
    case ATLAS_EVIDENCE_DOCUMENT:
        return ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT;
    case ATLAS_EVIDENCE_TEST:
    case ATLAS_EVIDENCE_RUNTIME:
    case ATLAS_EVIDENCE_DEPLOYED_CONFIG:
        return ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION;
    case ATLAS_EVIDENCE_ATLAS_KNOWLEDGE:
    case ATLAS_EVIDENCE_HUMAN_STATEMENT:
    case ATLAS_EVIDENCE_AI_ANALYSIS:
        return ATLAS_EVIDENCE_FAMILY_INTERPRETATION;
    case ATLAS_EVIDENCE_UNKNOWN:
        return ATLAS_EVIDENCE_FAMILY_UNKNOWN;
    }
    return ATLAS_EVIDENCE_FAMILY_UNKNOWN;
}

bool atlas_verify_evidence_class_may_be_root(atlas_verify_evidence_class c) {
    atlas_verify_evidence_family f = atlas_verify_evidence_family_of(c);
    return f == ATLAS_EVIDENCE_FAMILY_STATIC_ARTIFACT ||
           f == ATLAS_EVIDENCE_FAMILY_DYNAMIC_OBSERVATION;
}

/* --- calibration ---------------------------------------------------------- */

static const char *const CALIBRATION_NAMES[] = {"INSUFFICIENT_DATA", "UNCALIBRATED", "CALIBRATING",
                                                "CALIBRATED"};

const char *atlas_verify_calibration_name(atlas_verify_calibration c) {
    if ((size_t)c < sizeof CALIBRATION_NAMES / sizeof CALIBRATION_NAMES[0]) {
        return CALIBRATION_NAMES[c];
    }
    return "INSUFFICIENT_DATA";
}

bool atlas_verify_calibration_parse(const char *name, atlas_verify_calibration *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof CALIBRATION_NAMES / sizeof CALIBRATION_NAMES[0]; i++) {
        if (strcmp(name, CALIBRATION_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_calibration)i;
            }
            return true;
        }
    }
    return false;
}

/* --- conflicts ------------------------------------------------------------ */

static const char *const CONFLICT_NAMES[] = {"NONE",           "CONTRADICTION",
                                             "SUPERSESSION",   "SCOPE_MISMATCH",
                                             "IMPLEMENTATION", "STALE_EVIDENCE",
                                             "COMPETING_NORMATIVE"};

const char *atlas_verify_conflict_name(atlas_verify_conflict c) {
    if ((size_t)c < sizeof CONFLICT_NAMES / sizeof CONFLICT_NAMES[0]) {
        return CONFLICT_NAMES[c];
    }
    return "NONE";
}

bool atlas_verify_conflict_parse(const char *name, atlas_verify_conflict *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof CONFLICT_NAMES / sizeof CONFLICT_NAMES[0]; i++) {
        if (strcmp(name, CONFLICT_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (atlas_verify_conflict)i;
            }
            return true;
        }
    }
    return false;
}

/* --- policy verdicts and reasons ------------------------------------------ */

static const char *const POLICY_VERDICT_NAMES[] = {"NEEDS_REVIEW", "AUTO", "SHADOW", "BLOCKED",
                                                   "FORBIDDEN"};

const char *atlas_verify_policy_verdict_name(atlas_verify_policy_verdict v) {
    if ((size_t)v < sizeof POLICY_VERDICT_NAMES / sizeof POLICY_VERDICT_NAMES[0]) {
        return POLICY_VERDICT_NAMES[v];
    }
    return "NEEDS_REVIEW";
}

/* How weak each verdict is. Higher absorbs lower in the fold. AUTO is the only
 * value reachable by nothing having gone wrong, so it is weakest in this
 * ordering — any reason at all outranks it. */
static int verdict_strength(atlas_verify_policy_verdict v) {
    switch (v) {
    case ATLAS_POLICY_AUTO:
        return 0;
    case ATLAS_POLICY_SHADOW:
        return 1;
    case ATLAS_POLICY_NEEDS_REVIEW:
        return 2;
    case ATLAS_POLICY_BLOCKED:
        return 3;
    case ATLAS_POLICY_FORBIDDEN:
        return 4;
    }
    return 4;
}

atlas_verify_policy_verdict atlas_verify_verdict_fold(atlas_verify_policy_verdict a,
                                                     atlas_verify_policy_verdict b) {
    return verdict_strength(a) >= verdict_strength(b) ? a : b;
}

/* One row per reason: its name, the verdict it implies **on its own**, and a
 * fixed Atlas-owned sentence saying what it means.
 *
 * The verdict lives here rather than beside each call site for A6's reason: the
 * answer follows from the reason instead of being chosen next to it, so a
 * reason cannot be added without deciding what it implies. */
typedef struct reason_entry {
    atlas_verify_reason reason;
    const char *name;
    atlas_verify_policy_verdict verdict;
    const char *description;
} reason_entry;

static const reason_entry REASONS[] = {
    {ATLAS_VREASON_NONE, "NONE", ATLAS_POLICY_NEEDS_REVIEW,
     "no reason was recorded, which is not a statement that everything passed"},
    {ATLAS_VREASON_NO_POLICY, "NO_POLICY", ATLAS_POLICY_NEEDS_REVIEW,
     "no root-owned verification policy is installed, or it is disabled; nothing is automatic"},
    {ATLAS_VREASON_NOT_ALLOWED, "NOT_ALLOWED", ATLAS_POLICY_BLOCKED,
     "the policy carries no rule allowing this knowledge kind and this transition"},
    {ATLAS_VREASON_SHADOW_MODE, "SHADOW_MODE", ATLAS_POLICY_SHADOW,
     "every gate passed and enforcement is off for this basis; Atlas recorded what it would have "
     "done and changed nothing"},
    {ATLAS_VREASON_NOT_VERIFIED, "NOT_VERIFIED", ATLAS_POLICY_BLOCKED,
     "the claim is not verified, so there is nothing for a transition to follow from"},
    {ATLAS_VREASON_LOW_CONFIDENCE, "LOW_CONFIDENCE", ATLAS_POLICY_BLOCKED,
     "the confidence score is below the threshold this policy sets"},
    {ATLAS_VREASON_INSUFFICIENT_INDEPENDENCE, "INSUFFICIENT_INDEPENDENCE", ATLAS_POLICY_BLOCKED,
     "fewer genuinely independent evidence groups than the policy requires; more attestations "
     "from correlated sources do not change this"},
    {ATLAS_VREASON_STALE_EVIDENCE, "STALE_EVIDENCE", ATLAS_POLICY_BLOCKED,
     "the evidence is older than this policy allows for a claim about the present"},
    {ATLAS_VREASON_CONFLICT, "CONFLICT", ATLAS_POLICY_BLOCKED,
     "a blocking conflict exists between this claim and other evidence"},
    {ATLAS_VREASON_CALIBRATION_INSUFFICIENT, "CALIBRATION_INSUFFICIENT", ATLAS_POLICY_SHADOW,
     "this verdict rests on source reliability and there is not enough resolved history to say "
     "how reliable those sources are; the empirical path stays in shadow"},
    {ATLAS_VREASON_TRANSITION_ILLEGAL, "TRANSITION_ILLEGAL", ATLAS_POLICY_BLOCKED,
     "the lifecycle state machine does not permit this transition for this knowledge kind"},
    {ATLAS_VREASON_JUDGMENT_REQUIRES_AUTHORITY, "JUDGMENT_REQUIRES_AUTHORITY",
     ATLAS_POLICY_FORBIDDEN,
     "this is a normative choice rather than a discoverable fact; no confidence score and no "
     "source's reliability substitutes for the authority to make it"},
    {ATLAS_VREASON_RISK_REQUIRES_AUTHORITY, "RISK_REQUIRES_AUTHORITY", ATLAS_POLICY_FORBIDDEN,
     "verifying that a risk exists, or that it has been mitigated, establishes nothing about "
     "whether the project accepts it; risk acceptance requires explicit authority"},
    {ATLAS_VREASON_NORMATIVE_CLAIM, "NORMATIVE_CLAIM", ATLAS_POLICY_FORBIDDEN,
     "a mechanical verifier cannot establish a rule about what ought to be; it can only observe "
     "what is"},
    {ATLAS_VREASON_SOURCE_DRIFT, "SOURCE_DRIFT", ATLAS_POLICY_BLOCKED,
     "the claim is bound to one repository state and the repository is at another, so this result "
     "describes a tree the repository has since left and cannot justify a transition about the "
     "current one"},
    {ATLAS_VREASON_OK, "OK", ATLAS_POLICY_AUTO, "every gate this policy sets was passed"},
};

/* A reason added to the enum without a row here would fall through to the
 * placeholder and `tests/test_verify_model.c` fails on it — A6's arrangement,
 * for A6's reason. */
static const reason_entry *reason_row(atlas_verify_reason r) {
    for (size_t i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++) {
        if (REASONS[i].reason == r) {
            return &REASONS[i];
        }
    }
    return NULL;
}

const char *atlas_verify_reason_name(atlas_verify_reason r) {
    const reason_entry *e = reason_row(r);
    return e != NULL ? e->name : "UNNAMED_REASON";
}

const char *atlas_verify_reason_description(atlas_verify_reason r) {
    const reason_entry *e = reason_row(r);
    return e != NULL ? e->description : "this reason has no written meaning, which is a defect";
}

atlas_verify_policy_verdict atlas_verify_reason_verdict(atlas_verify_reason r) {
    const reason_entry *e = reason_row(r);
    /* An unlisted reason is not permission. */
    return e != NULL ? e->verdict : ATLAS_POLICY_BLOCKED;
}

size_t atlas_verify_reason_count(void) { return sizeof REASONS / sizeof REASONS[0]; }

atlas_verify_reason atlas_verify_reason_at(size_t index) {
    if (index < sizeof REASONS / sizeof REASONS[0]) {
        return REASONS[index].reason;
    }
    return ATLAS_VREASON_NONE;
}

/* --- outcome eligibility -------------------------------------------------- */

static const char *const OUTCOME_NAMES[] = {"UNKNOWN", "DETERMINISTIC_VERIFIER",
                                            "OPERATOR_RESOLUTION", "RUNTIME_OBSERVATION",
                                            "MACHINE_TRANSITION"};

const char *atlas_verify_outcome_source_name(atlas_verify_outcome_source s) {
    if ((size_t)s < sizeof OUTCOME_NAMES / sizeof OUTCOME_NAMES[0]) {
        return OUTCOME_NAMES[s];
    }
    return "UNKNOWN";
}

/* The loop-breaker. A machine transition is never ground truth about the
 * sources that argued for it, because then a source would be teaching Atlas to
 * trust it using Atlas' trust in it. */
bool atlas_verify_outcome_eligible(atlas_verify_outcome_source s) {
    switch (s) {
    case ATLAS_OUTCOME_DETERMINISTIC_VERIFIER:
    case ATLAS_OUTCOME_OPERATOR_RESOLUTION:
    case ATLAS_OUTCOME_RUNTIME_OBSERVATION:
        return true;
    case ATLAS_OUTCOME_UNKNOWN:
    case ATLAS_OUTCOME_MACHINE_TRANSITION:
        return false;
    }
    return false;
}

/* --- deterministic verifiers ---------------------------------------------- */

typedef struct verifier_entry {
    atlas_verify_verifier v;
    const char *name;
    const char *description;
} verifier_entry;

static const verifier_entry VERIFIERS[] = {
    {ATLAS_VERIFIER_NONE, "NONE", "no deterministic verifier applies to this claim"},
    {ATLAS_VERIFIER_CONTENT_HASH, "atlas.content_hash",
     "the content of a repository path at a bound commit hashes to a stated value"},
    {ATLAS_VERIFIER_SYMBOL_PRESENT, "atlas.symbol_present",
     "a named symbol exists in a complete semantic generation"},
    {ATLAS_VERIFIER_SYMBOL_ABSENT, "atlas.symbol_absent",
     "no symbol of a given name exists in a complete semantic generation; a partial generation "
     "cannot establish an absence and reports UNAVAILABLE instead"},
    {ATLAS_VERIFIER_PROVEN_EDGE, "atlas.proven_edge",
     "the compiler proved a direct call edge between two named symbols"},
};

const char *atlas_verify_verifier_name(atlas_verify_verifier v) {
    for (size_t i = 0; i < sizeof VERIFIERS / sizeof VERIFIERS[0]; i++) {
        if (VERIFIERS[i].v == v) {
            return VERIFIERS[i].name;
        }
    }
    return "NONE";
}

const char *atlas_verify_verifier_description(atlas_verify_verifier v) {
    for (size_t i = 0; i < sizeof VERIFIERS / sizeof VERIFIERS[0]; i++) {
        if (VERIFIERS[i].v == v) {
            return VERIFIERS[i].description;
        }
    }
    return "unknown verifier";
}

bool atlas_verify_verifier_parse(const char *name, atlas_verify_verifier *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof VERIFIERS / sizeof VERIFIERS[0]; i++) {
        if (strcmp(name, VERIFIERS[i].name) == 0) {
            if (out != NULL) {
                *out = VERIFIERS[i].v;
            }
            return true;
        }
    }
    return false;
}

size_t atlas_verify_verifier_count(void) { return sizeof VERIFIERS / sizeof VERIFIERS[0]; }

atlas_verify_verifier atlas_verify_verifier_at(size_t index) {
    if (index < sizeof VERIFIERS / sizeof VERIFIERS[0]) {
        return VERIFIERS[index].v;
    }
    return ATLAS_VERIFIER_NONE;
}

static const char *const CHECK_NAMES[] = {"UNAVAILABLE", "PASS", "FAIL"};

const char *atlas_verify_check_name(atlas_verify_check c) {
    if ((size_t)c < sizeof CHECK_NAMES / sizeof CHECK_NAMES[0]) {
        return CHECK_NAMES[c];
    }
    return "UNAVAILABLE";
}

/* A9.2.1. The parsers the socket client needs, written the same way as every
 * other `_parse` in this file: a name is matched against the closed vocabulary
 * and an unrecognised one is **refused**, leaving the caller's value at its
 * zero.
 *
 * That zero is the safe answer in each of these vocabularies by construction —
 * UNAVAILABLE for a check, NEEDS_REVIEW for a verdict — so a newer daemon
 * naming something this binary has never heard of degrades to "Atlas cannot
 * tell" rather than to something permissive. A parser that guessed would let a
 * version skew manufacture a PASS. */
bool atlas_verify_check_parse(const char *name, atlas_verify_check *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof CHECK_NAMES / sizeof CHECK_NAMES[0]; i++) {
        if (strcmp(name, CHECK_NAMES[i]) == 0) {
            *out = (atlas_verify_check)i;
            return true;
        }
    }
    return false;
}

bool atlas_verify_policy_verdict_parse(const char *name, atlas_verify_policy_verdict *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof POLICY_VERDICT_NAMES / sizeof POLICY_VERDICT_NAMES[0]; i++) {
        if (strcmp(name, POLICY_VERDICT_NAMES[i]) == 0) {
            *out = (atlas_verify_policy_verdict)i;
            return true;
        }
    }
    return false;
}

bool atlas_verify_reason_parse(const char *name, atlas_verify_reason *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < atlas_verify_reason_count(); i++) {
        atlas_verify_reason r = (atlas_verify_reason)i;
        if (strcmp(name, atlas_verify_reason_name(r)) == 0) {
            *out = r;
            return true;
        }
    }
    return false;
}

/* --- conservative priors --------------------------------------------------
 *
 * §20 in a table. Read the numbers as "how much weight does Atlas give a source
 * of this sort, about which it knows nothing else?" and not as accuracy
 * estimates, which they are not and could not be on a fresh install.
 *
 * Three things about the shape of this table are deliberate and are the reasons
 * it looks unlike what one might expect:
 *
 * - **A human is not at the top.** These weights concern claims of *fact*, and
 *   on questions of fact a person's recollection of what a system does is
 *   ordinary evidence. Human authority is real and lives entirely elsewhere:
 *   in the operator channel, which no weight in this table can substitute for.
 *
 * - **Identity dominates class.** A self-declared actor is capped low whatever
 *   it says it is, because the difference between "Atlas ran the compiler" and
 *   "something told Atlas the compiler said so" is the largest real difference
 *   available here, and it is the one an attacker controls.
 *
 * - **Nothing reaches full scale.** The maximum is 900 of 1000, so no single
 *   attestation can carry a claim to certainty by itself. Certainty has to be
 *   assembled out of independent groups, which is the behaviour the phase
 *   exists to produce.
 *
 * Bump ATLAS_VERIFY_PRIOR_VERSION when these change. Once a source has resolved
 * outcomes, its measured reliability replaces the prior — that is what makes
 * these starting points rather than settings. */
int atlas_verify_prior_reliability(atlas_verify_actor_class cls,
                                   atlas_verify_actor_identity identity) {
    int base;
    switch (cls) {
    case ATLAS_ACTOR_ATLAS_VERIFIER:
        base = 900;
        break;
    case ATLAS_ACTOR_TOOL:
    case ATLAS_ACTOR_TEST:
        base = 700;
        break;
    case ATLAS_ACTOR_RUNTIME_OBSERVATION:
    case ATLAS_ACTOR_REPOSITORY_EVIDENCE:
        base = 650;
        break;
    case ATLAS_ACTOR_HUMAN:
        base = 500;
        break;
    case ATLAS_ACTOR_DOCUMENT:
        base = 400;
        break;
    case ATLAS_ACTOR_AI_AGENT:
        base = 350;
        break;
    case ATLAS_ACTOR_UNKNOWN:
        base = 100;
        break;
    default:
        base = 100;
        break;
    }

    /* The cap an unauthenticated identity imposes. A self-declared source is
     * held to a third of scale however impressive its description, because its
     * description is the part an adversary writes. */
    int cap;
    switch (identity) {
    case ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED:
        cap = 900;
        break;
    case ATLAS_ACTOR_IDENTITY_PEER_AUTHENTICATED:
        cap = 600;
        break;
    case ATLAS_ACTOR_IDENTITY_SELF_DECLARED:
    default:
        cap = 350;
        break;
    }
    return base < cap ? base : cap;
}

/* --- aggregate scaffolding ------------------------------------------------ */

void atlas_verify_aggregate_init(atlas_verify_aggregate *a) {
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof *a);
    /* Every zero here is the safe reading: UNVERIFIED, UNKNOWN basis,
     * INSUFFICIENT_DATA calibration, NEEDS_REVIEW verdict, NONE conflict. */
    a->algorithm = ATLAS_VERIFY_ALGORITHM;
    a->family_version = ATLAS_VERIFY_FAMILY_VERSION;
    a->confidence = 0;
    /* -1 rather than 0: there is no calibrated probability, and zero would be
     * a calibrated probability of nought. */
    a->calibrated_probability = -1;
}

void atlas_verify_aggregate_note(atlas_verify_aggregate *a, atlas_verify_reason r) {
    if (a == NULL) {
        return;
    }
    /* Fold first. A reason that does not fit in the list still weakens the
     * verdict, so a truncated explanation can never produce a better answer
     * than a complete one. */
    a->verdict = atlas_verify_verdict_fold(a->verdict, atlas_verify_reason_verdict(r));
    a->reason_total++;
    for (size_t i = 0; i < a->reason_count; i++) {
        if (a->reasons[i] == r) {
            return; /* recorded once; the fold above is idempotent */
        }
    }
    if (a->reason_count < ATLAS_VERIFY_MAX_REASONS) {
        a->reasons[a->reason_count++] = r;
    }
}

/* --- owned structures -----------------------------------------------------
 *
 * `_init`/`_free` pairs, in Atlas' ownership discipline: every `atlas_buf`
 * member is owned by the struct, released exactly once, and a partially built
 * value is destroyed by its own destructor rather than unwound by hand. */

void atlas_verify_claim_init(atlas_verify_claim *c) {
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof *c);
    atlas_buf_init(&c->uid);
    atlas_buf_init(&c->repo_identity_hash);
    atlas_buf_init(&c->domain);
    atlas_buf_init(&c->text);
    atlas_buf_init(&c->scope_note);
    atlas_buf_init(&c->verifier);
    atlas_buf_init(&c->verifier_input);
    atlas_buf_init(&c->basis_commit);
    atlas_buf_init(&c->environment);
    atlas_buf_init(&c->created_at);
    atlas_buf_init(&c->content_key);
}

void atlas_verify_claim_free(atlas_verify_claim *c) {
    if (c == NULL) {
        return;
    }
    atlas_buf_free(&c->uid);
    atlas_buf_free(&c->repo_identity_hash);
    atlas_buf_free(&c->domain);
    atlas_buf_free(&c->text);
    atlas_buf_free(&c->scope_note);
    atlas_buf_free(&c->verifier);
    atlas_buf_free(&c->verifier_input);
    atlas_buf_free(&c->basis_commit);
    atlas_buf_free(&c->environment);
    atlas_buf_free(&c->created_at);
    atlas_buf_free(&c->content_key);
    memset(c, 0, sizeof *c);
}

void atlas_verify_actor_init(atlas_verify_actor *a) {
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof *a);
    atlas_buf_init(&a->uid);
    atlas_buf_init(&a->name);
    atlas_buf_init(&a->provider);
    atlas_buf_init(&a->family);
    atlas_buf_init(&a->version);
    atlas_buf_init(&a->role);
    atlas_buf_init(&a->session_key);
    atlas_buf_init(&a->run_id);
    atlas_buf_init(&a->first_seen_at);
    atlas_buf_init(&a->last_seen_at);
}

void atlas_verify_actor_free(atlas_verify_actor *a) {
    if (a == NULL) {
        return;
    }
    atlas_buf_free(&a->uid);
    atlas_buf_free(&a->name);
    atlas_buf_free(&a->provider);
    atlas_buf_free(&a->family);
    atlas_buf_free(&a->version);
    atlas_buf_free(&a->role);
    atlas_buf_free(&a->session_key);
    atlas_buf_free(&a->run_id);
    atlas_buf_free(&a->first_seen_at);
    atlas_buf_free(&a->last_seen_at);
    memset(a, 0, sizeof *a);
}

void atlas_verify_evidence_init(atlas_verify_evidence *e) {
    if (e == NULL) {
        return;
    }
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->uid);
    atlas_buf_init(&e->commit_oid);
    atlas_buf_init(&e->path_raw);
    atlas_buf_init(&e->path_text);
    atlas_buf_init(&e->symbol);
    atlas_buf_init(&e->content_hash);
    atlas_buf_init(&e->suite);
    atlas_buf_init(&e->test_name);
    atlas_buf_init(&e->result);
    atlas_buf_init(&e->binary_id);
    atlas_buf_init(&e->environment);
    atlas_buf_init(&e->tool);
    atlas_buf_init(&e->tool_version);
    atlas_buf_init(&e->proof_class);
    atlas_buf_init(&e->target);
    atlas_buf_init(&e->probe);
    atlas_buf_init(&e->observed);
    atlas_buf_init(&e->deployed_revision);
    atlas_buf_init(&e->observed_at);
    atlas_buf_init(&e->recorded_at);
    atlas_buf_init(&e->content_key);
}

void atlas_verify_evidence_free(atlas_verify_evidence *e) {
    if (e == NULL) {
        return;
    }
    atlas_buf_free(&e->uid);
    atlas_buf_free(&e->commit_oid);
    atlas_buf_free(&e->path_raw);
    atlas_buf_free(&e->path_text);
    atlas_buf_free(&e->symbol);
    atlas_buf_free(&e->content_hash);
    atlas_buf_free(&e->suite);
    atlas_buf_free(&e->test_name);
    atlas_buf_free(&e->result);
    atlas_buf_free(&e->binary_id);
    atlas_buf_free(&e->environment);
    atlas_buf_free(&e->tool);
    atlas_buf_free(&e->tool_version);
    atlas_buf_free(&e->proof_class);
    atlas_buf_free(&e->target);
    atlas_buf_free(&e->probe);
    atlas_buf_free(&e->observed);
    atlas_buf_free(&e->deployed_revision);
    atlas_buf_free(&e->observed_at);
    atlas_buf_free(&e->recorded_at);
    atlas_buf_free(&e->content_key);
    memset(e, 0, sizeof *e);
}

void atlas_verify_attestation_init(atlas_verify_attestation *a) {
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof *a);
    atlas_buf_init(&a->uid);
    atlas_buf_init(&a->method);
    atlas_buf_init(&a->scope_note);
    atlas_buf_init(&a->created_at);
    atlas_buf_init(&a->basis_commit);
    atlas_buf_init(&a->environment);
    atlas_buf_init(&a->content_key);
    /* -1 rather than 0: an actor that stated no confidence is not an actor
     * that stated zero confidence. */
    a->self_confidence = -1;
}

void atlas_verify_attestation_free(atlas_verify_attestation *a) {
    if (a == NULL) {
        return;
    }
    atlas_buf_free(&a->uid);
    atlas_buf_free(&a->method);
    atlas_buf_free(&a->scope_note);
    atlas_buf_free(&a->created_at);
    atlas_buf_free(&a->basis_commit);
    atlas_buf_free(&a->environment);
    atlas_buf_free(&a->content_key);
    memset(a, 0, sizeof *a);
}

/* --- independence ---------------------------------------------------------
 *
 * Union-find, with the conservative closures that make it safe. See the header
 * for the argument; what follows is the mechanism.
 *
 * The critical index is `SHARED`: one extra set that every attestation with no
 * demonstrable independent root joins. Three models each reading the same
 * document and declaring nothing land there together, which is the §11 case,
 * and so does an orchestrator's fleet of subagents, which is the §46 case. The
 * two are the same failure and get the same answer for the same reason: Atlas
 * could not demonstrate independence, so it does not assume it. */
typedef struct uf {
    int *parent;
    size_t n;
} uf;

static int uf_find(uf *u, int x) {
    while (u->parent[x] != x) {
        u->parent[x] = u->parent[u->parent[x]]; /* path halving */
        x = u->parent[x];
    }
    return x;
}

static void uf_union(uf *u, int a, int b) {
    int ra = uf_find(u, a);
    int rb = uf_find(u, b);
    if (ra != rb) {
        /* Lower index wins, so grouping is deterministic rather than dependent
         * on tree shape — the same inputs must produce the same group numbers
         * for the aggregate to be reproducible. */
        if (ra < rb) {
            u->parent[rb] = ra;
        } else {
            u->parent[ra] = rb;
        }
    }
}

int atlas_verify_independent_groups(atlas_verify_input *inputs, size_t count,
                                    const int64_t *dep_from, const int64_t *dep_to,
                                    size_t dep_count) {
    if (inputs == NULL || count == 0) {
        return 0;
    }
    if (count > ATLAS_VERIFY_MAX_ATTESTATIONS) {
        count = ATLAS_VERIFY_MAX_ATTESTATIONS;
    }

    /* One slot per input plus the shared undeclared set at the end. Fixed size,
     * so this allocates nothing and cannot fail: the bound is the ceiling from
     * limits.h and the caller has already been told when it was reached. */
    static const size_t CAP = ATLAS_VERIFY_MAX_ATTESTATIONS + 1u;
    int parent[ATLAS_VERIFY_MAX_ATTESTATIONS + 1u];
    for (size_t i = 0; i < CAP; i++) {
        parent[i] = (int)i;
    }
    uf u = {parent, CAP};
    const int SHARED = (int)ATLAS_VERIFY_MAX_ATTESTATIONS;

    /* Declared derivation edges. `dep_from` and `dep_to` are attestation
     * indices already resolved by the db layer: an edge means the two rest on
     * evidence that shares a root, whether that is because one derives from
     * the other or because both name the same source. */
    for (size_t i = 0; i < dep_count && i < ATLAS_VERIFY_MAX_DEP_EDGES; i++) {
        int64_t a = dep_from[i];
        int64_t b = dep_to[i];
        if (a >= 0 && b >= 0 && (size_t)a < count && (size_t)b < count) {
            uf_union(&u, (int)a, (int)b);
        }
    }

    /* The conservative closure. An attestation whose evidence cannot stand as
     * an independent root — an interpretation, or nothing at all — joins the
     * shared set. This is where "actor ≠ evidence" is actually enforced: it
     * does not matter how many differently-named sources say it. */
    for (size_t i = 0; i < count; i++) {
        if (inputs[i].family == ATLAS_EVIDENCE_FAMILY_INTERPRETATION ||
            inputs[i].family == ATLAS_EVIDENCE_FAMILY_UNKNOWN) {
            uf_union(&u, (int)i, SHARED);
        }
    }

    /* Number the components in order of first appearance, so group ids are a
     * function of the input order and nothing else. */
    int next = 0;
    int label[ATLAS_VERIFY_MAX_ATTESTATIONS + 1u];
    for (size_t i = 0; i < CAP; i++) {
        label[i] = -1;
    }
    for (size_t i = 0; i < count; i++) {
        int root = uf_find(&u, (int)i);
        if (label[root] < 0) {
            label[root] = next++;
        }
        inputs[i].group = label[root];
    }
    return next;
}

/* --- the algorithm --------------------------------------------------------
 *
 * `atlas-reliability-v1`. Integer, bounded, and short enough that a reviewer
 * can hold all of it at once — which is the property that matters most, because
 * an unauditable scorer is one nobody can challenge.
 */

/* One attestation's weight, in ATLAS_VERIFY_WEIGHT_SCALE units. */
static int64_t input_weight(const atlas_verify_input *in) {
    /* Measured reliability where there is any; otherwise the documented prior.
     * Never an invented number, and never silence. */
    int64_t w = in->reliability >= 0
                    ? in->reliability
                    : atlas_verify_prior_reliability(in->actor_class, in->actor_identity);

    /* §26: a proposer supporting its own proposal is not verification. It is
     * not discarded — it is evidence about what the proposer believes — but it
     * cannot carry the claim. Halved, and it still cannot reach a threshold
     * alone because a single group is capped by the prior mass anyway. */
    if (in->proposer && in->verdict == ATLAS_ATTEST_SUPPORT) {
        w /= 2;
    }

    /* A source speaking outside the scope it examined is weaker evidence about
     * this claim, whatever its reliability elsewhere. */
    if (!in->scope_match) {
        w /= 2;
    }

    /* Stale evidence loses current force and keeps historical value. It is not
     * removed: §47 requires the old observation to remain, and a weight of a
     * quarter says "this was true once" without letting July decide August. */
    if (in->stale) {
        w /= 4;
    }
    return w;
}

void atlas_verify_aggregate_compute(atlas_verify_aggregate *out, atlas_verify_input *inputs,
                                    size_t count, atlas_verify_basis basis) {
    if (out == NULL) {
        return;
    }
    atlas_verify_aggregate_init(out);
    out->basis = basis;
    if (inputs == NULL || count == 0) {
        /* No attestation is UNVERIFIED, not INCONCLUSIVE: nobody looked.
         *
         * **No reason is recorded here**, and that is not an omission. This
         * function reports what the *evidence* says; whether that is grounds to
         * act is the policy engine's question, and the two must not be mixed.
         *
         * Mixing them was a real defect: noting NOT_VERIFIED at this point
         * folded the verdict to BLOCKED before the deterministic verifier had
         * run, and because the fold is deliberately monotonic — a reason can
         * only ever weaken an answer — a subsequent mechanical PASS could not
         * lift it. Every deterministic claim with no attestations was silently
         * unactionable. The fold's one-way property is correct and the mistake
         * was recording a policy conclusion from inside the evidence fold. */
        out->state = ATLAS_VERIFY_UNVERIFIED;
        return;
    }
    if (count > ATLAS_VERIFY_MAX_ATTESTATIONS) {
        count = ATLAS_VERIFY_MAX_ATTESTATIONS;
    }

    /* Per-group strongest weight, for support and contradiction separately.
     *
     * **This is the anti-inflation rule and the heart of the algorithm.**
     * Summing within a group would mean that saying the same thing twice from
     * the same root is twice the evidence, which is exactly the property §11
     * and §46 forbid. Taking the maximum means a group contributes what its
     * best member is worth and repetition contributes nothing at all. */
    int64_t sup[ATLAS_VERIFY_MAX_ATTESTATIONS + 1u];
    int64_t con[ATLAS_VERIFY_MAX_ATTESTATIONS + 1u];
    for (size_t i = 0; i <= ATLAS_VERIFY_MAX_ATTESTATIONS; i++) {
        sup[i] = 0;
        con[i] = 0;
    }

    bool family_seen[4] = {false, false, false, false};
    int max_group = -1;

    for (size_t i = 0; i < count; i++) {
        const atlas_verify_input *in = &inputs[i];
        int g = in->group;
        if (g < 0 || (size_t)g > ATLAS_VERIFY_MAX_ATTESTATIONS) {
            g = 0;
        }
        if (g > max_group) {
            max_group = g;
        }
        if ((size_t)in->family < 4) {
            family_seen[in->family] = true;
        }
        if (in->stale) {
            out->stale = true;
        }

        int64_t w = input_weight(in);
        switch (in->verdict) {
        case ATLAS_ATTEST_SUPPORT:
            out->support_count++;
            if (w > sup[g]) {
                sup[g] = w;
            }
            /* An Atlas-attested verifier's pass is not a vote. It settles the
             * claim's truth condition, and the score follows rather than
             * deciding. */
            if (in->actor_class == ATLAS_ACTOR_ATLAS_VERIFIER &&
                in->actor_identity == ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED && !in->stale) {
                out->deterministic_pass = true;
            }
            break;
        case ATLAS_ATTEST_CONTRADICT:
            out->contradict_count++;
            if (w > con[g]) {
                con[g] = w;
            }
            if (in->actor_class == ATLAS_ACTOR_ATLAS_VERIFIER &&
                in->actor_identity == ATLAS_ACTOR_IDENTITY_ATLAS_ATTESTED && !in->stale) {
                out->deterministic_fail = true;
            }
            break;
        case ATLAS_ATTEST_INCONCLUSIVE:
            out->inconclusive_count++;
            break;
        }
    }

    out->independent_groups = max_group + 1;
    for (size_t f = 1; f < 4; f++) {
        if (family_seen[f]) {
            out->independent_families++;
        }
    }

    for (int g = 0; g <= max_group; g++) {
        out->support_mass += sup[g];
        out->contradict_mass += con[g];
    }

    /* The score. `PRIOR_MASS` is the weight of not knowing: it never goes away,
     * so 100 is unreachable and a lone source lands near 80 rather than at
     * certainty. Integer division truncates, which biases every score
     * *downward* — the correct direction for a system that prefers abstention
     * to unjustified confidence. */
    int64_t denom = out->support_mass + out->contradict_mass + ATLAS_VERIFY_PRIOR_MASS;
    out->confidence = denom > 0 ? (int)((out->support_mass * 100) / denom) : 0;

    /* Calibration is a property of the sources and the basis, supplied by the
     * caller from stored outcomes. The algorithm never invents one, and on a
     * machine with no resolved history this stays INSUFFICIENT_DATA — which is
     * reported, not hidden. `calibrated_probability` therefore stays -1, and
     * nothing in Atlas prints a percent sign next to `confidence`. */

    /* The state. A deterministic result is not weighed against opinions: a
     * mechanical fail contradicts the claim however many sources like it, and a
     * mechanical pass establishes it. This is the ordering that makes
     * "deterministic evidence is not reliability aggregation" true of the code
     * rather than only of the documentation. */
    if (out->deterministic_fail) {
        out->state = ATLAS_VERIFY_CONTRADICTED;
    } else if (out->deterministic_pass) {
        out->state = ATLAS_VERIFY_VERIFIED;
    } else if (out->stale && out->support_count > 0) {
        out->state = ATLAS_VERIFY_STALE;
    } else if (out->contradict_mass > out->support_mass) {
        out->state = ATLAS_VERIFY_CONTRADICTED;
    } else if (out->support_count == 0 && out->contradict_count == 0) {
        out->state = ATLAS_VERIFY_INCONCLUSIVE;
    } else if (out->support_mass == 0) {
        out->state = ATLAS_VERIFY_INCONCLUSIVE;
    } else if (out->contradict_mass > 0 && out->contradict_mass * 4 >= out->support_mass) {
        /* Meaningful dissent keeps a claim short of established, however high
         * the raw score. §63: confidence must not erase disagreement. */
        out->state = ATLAS_VERIFY_SUPPORTED;
    } else if (out->independent_groups >= 2 && out->confidence >= 80) {
        /* The empirical bar for VERIFIED needs genuine independence. One group,
         * however strong, is SUPPORTED — which is what stops a single confident
         * source from ever reading as established fact. */
        out->state = ATLAS_VERIFY_VERIFIED;
    } else {
        out->state = ATLAS_VERIFY_SUPPORTED;
    }
}

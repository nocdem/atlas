/* Atlas - A9.2.3: deriving a repository's semantic state and the one decision
 * the daemon makes about it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The whole file is a read. It opens no transaction, takes no lock, creates no
 * process and writes no row — the property `atlas_gate_check` has and for the
 * same reason: asking what Atlas thinks must not change what Atlas thinks, and a
 * scheduler that mutated on inspection could not also be the thing a status
 * command calls. That it is one function serving both is what makes "the
 * scheduler and the status page agree" structural rather than a promise.
 *
 * The order of the checks is the order the answers matter in, and each one is a
 * refusal the ones after it never get to overrule:
 *
 *   1. no libclang — this Atlas cannot build an index at all;
 *   2. an operator explicitly refused maintenance for this repository, which no
 *      machine-wide default overrules in either direction;
 *   3. the root-owned default says no and nobody has said otherwise;
 *   4. build-input discovery accepted nothing — either because Atlas walked and
 *      found nothing, or because it has not walked yet, which are different
 *      statements and get different reasons;
 *   5. a build is already in flight;
 *   6. the last automatic attempt failed and nothing has changed since;
 *   7. freshness, which is A8-CI's axis;
 *   8. coverage, which is A9.2.3's and which A9.2.4 widened to include whether
 *      the *search for build inputs* was complete.
 *
 * A9.2.4 reversed A9.2.3's second check. It used to be "no build description, or
 * automatic rebuild not enabled", which kept A8-CI's rule that no model can cause
 * a compiler to run by refusing for every repository nobody had configured. That
 * rule survives, and now survives elsewhere: enabling is still operator-only —
 * there is no MCP tool, gateway route or ordinary RPC method that reaches it —
 * and what a repository nobody has spoken about does is a root-owned decision
 * rather than a compiled-in refusal. See `atlas_sem_auto_effective`.
 */
#include "atlas/sem_schedule.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_ops.h"
#include "atlas/syspolicy.h"

const char *atlas_sem_activity_name(atlas_sem_activity a) {
    switch (a) {
    case ATLAS_SEM_ACT_DISABLED:
        return "DISABLED";
    case ATLAS_SEM_ACT_EXPLICITLY_DISABLED:
        return "EXPLICITLY_DISABLED";
    case ATLAS_SEM_ACT_NO_INPUTS:
        return "NO_INPUTS";
    case ATLAS_SEM_ACT_UNAVAILABLE:
        return "UNAVAILABLE";
    case ATLAS_SEM_ACT_CURRENT:
        return "CURRENT";
    case ATLAS_SEM_ACT_INCOMPLETE:
        return "INCOMPLETE";
    case ATLAS_SEM_ACT_BUILDING:
        return "BUILDING";
    case ATLAS_SEM_ACT_DIRTY:
        return "DIRTY";
    case ATLAS_SEM_ACT_FAILED:
        return "FAILED";
    case ATLAS_SEM_ACT_UNKNOWN:
        break;
    }
    return "UNKNOWN";
}

/* Returns Atlas' own copy of a known hold reason, or NULL.
 *
 * A reason that came back over a socket is a *matching* string, not Atlas'
 * string, and storing the caller's bytes would put a value whose lifetime and
 * origin Atlas does not own in front of an operator and a model. The same shape
 * as `atlas_sem_why_intern` and `atlas_sem_stale_reason_intern`. */
const char *atlas_sem_hold_intern(const char *reason) {
    static const char *const HOLDS[] = {
        ATLAS_SEM_HOLD_DISABLED,          ATLAS_SEM_HOLD_NO_COMPDB,
        ATLAS_SEM_HOLD_BUILDING,          ATLAS_SEM_HOLD_FAILED_UNCHANGED,
        ATLAS_SEM_HOLD_NO_LIBCLANG,       ATLAS_SEM_HOLD_CURRENT,
        ATLAS_SEM_HOLD_COVERAGE_INCOMPLETE,
        ATLAS_SEM_HOLD_FILE_INDEX,        ATLAS_SEM_HOLD_EXPLICIT_DISABLE,
        ATLAS_SEM_HOLD_POLICY_DEFAULT_OFF, ATLAS_SEM_HOLD_NO_INPUTS,
        ATLAS_SEM_HOLD_NOT_DISCOVERED,
    };
    if (reason == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(HOLDS) / sizeof(HOLDS[0]); i++) {
        if (strcmp(reason, HOLDS[i]) == 0) {
            return HOLDS[i];
        }
    }
    return NULL;
}

bool atlas_sem_hold_reason_is_known(const char *reason) {
    return atlas_sem_hold_intern(reason) != NULL;
}

void atlas_sem_plan_init(atlas_sem_plan *p) {
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
    /* Everything a memset leaves is the safe reading: UNKNOWN activity, ABSENT
     * freshness, UNKNOWN scope discovery, coverage not complete, and — the one
     * that matters most — `should_build` false. A zeroed plan never causes a
     * compiler to run. */
}

/* Coverage, as one boolean, from three separately meaningful facts.
 *
 * They are folded here rather than at the caller because the fold is a
 * *judgement about what a negative conclusion may rest on*, and it must be the
 * same judgement everywhere. Each of the three is a different problem with a
 * different remedy — a unit that failed to parse, a source the compilation
 * database never named, and an enumeration Atlas cannot vouch for — and every
 * one of them means the generation cannot support "there is no X". */
/* A9.2.3's rule, now asked rather than restated.
 *
 * `atlas_sem_coverage_gap` is the one implementation, shared with
 * `atlas_sem_trust_settle`, so a repository the scheduler calls INCOMPLETE and a
 * query that answers UNKNOWN name the same dimension because they consulted the
 * same function. Three copies of the rule that decides whether Atlas may state
 * an absence is exactly what this codebase keeps removing. */
static const char *coverage_gap_of(const atlas_sem_generation *g) {
    return atlas_sem_coverage_gap(g->scope_discovery, g->discovery,
                                  g->tu_partial == 0 && g->tu_failed == 0 &&
                                      g->tu_unsupported == 0,
                                  g->scope_uncovered);
}

atlas_status atlas_sem_plan_for_with_default(atlas_db *db, atlas_repo_info *repo, bool building,
                                             bool policy_default, atlas_sem_plan *out,
                                             atlas_err *err) {
    return atlas_sem_plan_for_with_trust(db, repo, building, policy_default, NULL, out, err);
}

atlas_status atlas_sem_plan_for(atlas_db *db, atlas_repo_info *repo, bool building,
                                atlas_sem_plan *out, atlas_err *err) {
    /* The root-owned policy, read here so that every caller which does not care
     * gets the right answer without knowing the key exists. A sweep that will
     * ask about many repositories reads it once and calls the `_with_default`
     * form instead. */
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    return atlas_sem_plan_for_with_default(db, repo, building,
                                           atlas_syspolicy_semantic_auto_default(&pol), out, err);
}

atlas_status atlas_sem_plan_for_with_trust(atlas_db *db, atlas_repo_info *repo, bool building,
                                           bool policy_default,
                                           const atlas_sem_trust *have_trust,
                                           atlas_sem_plan *out, atlas_err *err) {
    atlas_sem_plan_init(out);
    if (db == NULL || repo == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic plan: bad request");
    }

    atlas_sem_generation gen;
    bool found = false;
    atlas_status st = atlas_db_sem_current(db, repo->id, &gen, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A generation still being built, seen from the durable record rather than
     * only from the caller's observation. Either is enough: the caller knows
     * about a job it queued that has not opened a generation yet, and the record
     * knows about one a previous process opened and never finished. */
    atlas_sem_generation latest;
    bool have_latest = false;
    st = atlas_db_sem_latest(db, repo->id, &latest, &have_latest, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool running = building || (have_latest && latest.status == ATLAS_SEM_GEN_RUNNING);

    /* A9.2.5. When the caller has already gathered the trust facts — which
     * `atlas_sem_status_on` has, because it must emit them — the freshness comes
     * from there rather than being computed a second time.
     *
     * Both `atlas_sem_freshness_now` and `atlas_sem_trust_now` reach
     * `live_facts`, which reads and SHA-256s every accepted compilation database
     * and every source hash. Calling both in one response is the defect A9.2.3's
     * closure measured and removed; it came back when the trust block was added
     * beside the plan instead of in place of it. */
    if (have_trust != NULL) {
        out->freshness = have_trust->freshness;
        out->stale_reason = have_trust->stale_reason;
    } else {
        out->freshness =
            atlas_sem_freshness_now(db, repo, &gen, found, running, &out->stale_reason);
    }
    if (found) {
        out->generation_id = gen.id;
        (void)snprintf(out->generation_identity, sizeof out->generation_identity, "%s",
                       gen.source_identity);
        out->generation_discovery = gen.discovery;
        out->scope_discovery = gen.scope_discovery;
        out->scope_candidates = gen.scope_candidates;
        out->scope_covered = gen.scope_covered;
        out->scope_uncovered = gen.scope_uncovered;
        out->coverage_gap = coverage_gap_of(&gen);
        out->coverage_complete = out->coverage_gap == NULL;
    }

    /* The tree as it is now, reported whether or not anything is built from it,
     * so a surface can show the divergence rather than only the verdict. */
    atlas_err ignored;
    atlas_err_init(&ignored);
    (void)atlas_sem_source_identity(db, repo, out->source_identity, &ignored);

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    st = atlas_db_sem_config_get(db, repo->id, &cfg, err);
    if (st != ATLAS_OK) {
        atlas_sem_config_free(&cfg);
        return st;
    }
    out->configured = cfg.present;
    /* A9.2.4. The intent decides, and an absent row is UNSET rather than a
     * refusal — which is the whole of the activation reversal. `cfg.present` no
     * longer gates it: requiring a row would put the "remember to configure it"
     * problem straight back, one field along. */
    out->auto_intent = cfg.auto_intent;
    out->auto_intent_by = cfg.auto_intent_by;
    out->policy_default = policy_default;
    out->discovery_mode = cfg.discovery_mode;
    out->auto_rebuild = atlas_sem_auto_effective(cfg.auto_intent, policy_default);
    out->fail_count = cfg.fail_count;
    (void)snprintf(out->fail_reason, sizeof out->fail_reason, "%s", cfg.fail_reason);
    (void)snprintf(out->fail_at, sizeof out->fail_at, "%s", cfg.fail_at);
    /* A9.2.5. One further attempt after an *interrupted* pass, and exactly one.
     *
     * The governor's rule is unchanged for every other failure: an attempt is
     * allowed again only once the source identity has moved, never after an
     * interval, because retrying a tree that does not compile spends a compiler
     * run over the whole repository to reach the same answer. That is what makes
     * a storm impossible and it stays.
     *
     * What it could not distinguish was a pass that failed *because of the
     * machine* — out of memory, a database error — from one that failed because
     * of the inputs. Both pinned `fail_identity`, so a transient interruption
     * held a repository on `HOLD_FAILED_UNCHANGED` until somebody happened to
     * edit a file. The exception is bounded by `fail_count`, which the attempt
     * recorder increments and which is durable: the second interruption reaches
     * `fail_count == 2` and holds. There is no timer, so nothing can wake up and
     * try a third time, and a daemon restart reads the same persisted count and
     * reaches the same answer. */
    const bool interrupted_once =
        cfg.fail_count == 1 && strcmp(cfg.fail_reason, ATLAS_SEM_WHY_PASS_INTERRUPTED) == 0;
    const bool identity_unmoved =
        cfg.fail_count > 0 && cfg.fail_identity[0] != '\0' && out->source_identity[0] != '\0' &&
        strcmp(cfg.fail_identity, out->source_identity) == 0 && !interrupted_once;
    /* The discovery verdict of the last walk, and when it ran. Both are stored
     * beside the operator's statements for the reason the retry governor's
     * fields are: they are facts about the repository, not about any candidate. */
    out->discovery = cfg.discovery_state;
    (void)snprintf(out->discovered_at, sizeof out->discovered_at, "%s", cfg.discovered_at);
    (void)snprintf(out->discovery_limit, sizeof out->discovery_limit, "%s", cfg.discovery_limit);
    atlas_sem_config_free(&cfg);

    /* A9.2.4. What discovery last found, read from the persisted candidate list
     * rather than by walking the tree.
     *
     * The walk is the one expensive thing in this layer and this function runs
     * on every status read, every verification and every sweep tick — so
     * membership is persisted and refreshed on its own slower interval, while
     * the *content* of each accepted database is digested live inside
     * `atlas_sem_source_identity`. An edited or deleted database therefore moves
     * the identity at once; a newly created one is noticed at the next discovery
     * pass. Convergence, not correctness. */
    atlas_sem_input inputs[ATLAS_SEM_DISCOVERY_MAX_CANDIDATES];
    size_t input_count = 0;
    if (atlas_db_sem_inputs_get(db, repo->id, inputs, ATLAS_SEM_DISCOVERY_MAX_CANDIDATES,
                                &input_count, &ignored) != ATLAS_OK) {
        input_count = 0;
    }
    for (size_t i = 0; i < input_count; i++) {
        if (inputs[i].accepted) {
            out->inputs_accepted++;
        } else {
            out->inputs_rejected++;
        }
    }
    const bool have_compdbs = out->inputs_accepted > 0;

    /* --- the activity, then the decision --- */

    if (!found) {
        out->activity = ATLAS_SEM_ACT_UNAVAILABLE;
    } else if (running) {
        out->activity = ATLAS_SEM_ACT_BUILDING;
    } else if (out->freshness != ATLAS_SEM_FRESH_CURRENT) {
        out->activity = ATLAS_SEM_ACT_DIRTY;
    } else if (!out->coverage_complete) {
        /* Source-current and coverage-incomplete. Rebuilding will not fix it —
         * the compilation database names what it names, and a unit that failed
         * to parse will fail again from identical bytes — so this is reported
         * rather than scheduled. The remedy is an operator's, and the state
         * exists so that it is visible instead of hidden behind CURRENT. */
        out->activity = ATLAS_SEM_ACT_INCOMPLETE;
    } else {
        out->activity = ATLAS_SEM_ACT_CURRENT;
    }

    /* Refusals first, weakest answer wins, and each one overrides the activity
     * where it describes the situation better. */
    if (!atlas_sem_available()) {
        out->hold_reason = ATLAS_SEM_HOLD_NO_LIBCLANG;
        return ATLAS_OK;
    }
    if (out->auto_intent == ATLAS_SEM_INTENT_DISABLED) {
        /* An operator's own refusal, and it is checked *before* the effective
         * boolean so that the reason names the person rather than the policy.
         * `atlas_sem_auto_effective` already returns false here; what this
         * ordering buys is that a status surface can say whose decision it was,
         * which §20 requires and which "disabled" alone never could. */
        out->activity = ATLAS_SEM_ACT_EXPLICITLY_DISABLED;
        out->hold_reason = ATLAS_SEM_HOLD_EXPLICIT_DISABLE;
        return ATLAS_OK;
    }
    if (!out->auto_rebuild) {
        /* The root-owned default says no and nobody has said otherwise about
         * this repository. Not a fault, and not the operator's doing — which is
         * exactly why it is a different state and a different reason from the
         * one above. */
        out->activity = ATLAS_SEM_ACT_DISABLED;
        out->hold_reason = ATLAS_SEM_HOLD_POLICY_DEFAULT_OFF;
        return ATLAS_OK;
    }
    if (!have_compdbs) {
        /* Maintenance is on and there is nothing to build from. Reported as its
         * own state rather than as DISABLED: the remedy is to generate a build
         * description, not to change a setting, and an operator told "disabled"
         * would go looking for the setting.
         *
         * Two reasons, not one. A repository Atlas has walked and found nothing
         * in is a statement about the repository; a repository Atlas has not
         * walked yet is a statement about Atlas, and it is temporary. Reporting
         * the second as the first is the conflation this whole season exists to
         * end, one layer down from where A9.2.2 refuses it. */
        out->activity = ATLAS_SEM_ACT_NO_INPUTS;
        out->hold_reason = out->discovery == ATLAS_SEM_DISC_UNKNOWN
                               ? ATLAS_SEM_HOLD_NOT_DISCOVERED
                               : ATLAS_SEM_HOLD_NO_INPUTS;
        return ATLAS_OK;
    }
    if (running) {
        out->hold_reason = ATLAS_SEM_HOLD_BUILDING;
        return ATLAS_OK;
    }
    if (identity_unmoved) {
        /* The retry governor, and it is conservative on purpose. Retrying after
         * an interval would spin on a repository that does not compile — every
         * attempt running a compiler over the whole tree, for ever, achieving
         * nothing. What legitimately makes another attempt worth making is that
         * the inputs have changed, and the source identity is exactly that
         * question. A rebuild an operator asks for explicitly is not governed by
         * this: `code index` is a different decision by a different principal. */
        out->activity = ATLAS_SEM_ACT_FAILED;
        out->hold_reason = ATLAS_SEM_HOLD_FAILED_UNCHANGED;
        return ATLAS_OK;
    }
    if (out->activity == ATLAS_SEM_ACT_INCOMPLETE) {
        /* A9.2.5. **Not HOLD_CURRENT.**
         *
         * It was, and the sentence — "the published generation describes the
         * current source" — is true and conceals the half that decides whether
         * any absence over this index means anything. A repository can sit here
         * for ever: on the repository that produced this season the cause was
         * the operator's own `--exclude`, which makes discovery PARTIAL, which
         * makes coverage incomplete, which no rebuild can change.
         *
         * Still a hold rather than a build. Rebuilding cannot widen a
         * compilation database, cannot un-exclude a subtree, and cannot make a
         * unit that failed on these bytes succeed on them, so scheduling one
         * would spin without converging — the rebuild storm this season is
         * required not to create. What changes is that the state is now *named*,
         * `coverage_gap` says which dimension, and `operator_action_required`
         * says that waiting will not fix it. */
        out->hold_reason = ATLAS_SEM_HOLD_COVERAGE_INCOMPLETE;
        /* **Only the two gaps an automatic pass genuinely cannot close.**
         *
         * The header defines this as "true exactly when the repository is held on
         * something no automatic pass can fix", and two of the four gaps do not
         * qualify: a generation with no coverage manifest gets one the next time
         * it publishes, and a transiently failed unit is precisely what §9 of the
         * season document argues is *not* permanent. Setting the flag for all
         * four made the human renderer print "an operator: no automatic rebuild
         * can widen this coverage" in two cases where a rebuild is exactly what
         * would. */
        out->operator_action_required =
            out->coverage_gap != NULL &&
            (strcmp(out->coverage_gap, ATLAS_SEM_UNK_COVERAGE) == 0 ||
             strcmp(out->coverage_gap, ATLAS_SEM_UNK_DISCOVERY) == 0);
        return ATLAS_OK;
    }
    if (out->activity == ATLAS_SEM_ACT_CURRENT) {
        out->hold_reason = ATLAS_SEM_HOLD_CURRENT;
        return ATLAS_OK;
    }
    if (out->stale_reason != NULL && strcmp(out->stale_reason, ATLAS_SEM_STALE_FILE_INDEX) == 0) {
        /* Wait for the reconciliation pass rather than racing it.
         *
         * The source identity is computed from the file index's content hashes,
         * so a generation built now would be built from hashes that do not
         * describe the tree — and would be stale again the moment the pass that
         * fixes them completes. Building anyway would not merely waste a
         * compiler run: it would publish a generation claiming a source identity
         * it never observed, and on a large repository the two would keep
         * chasing each other. The pass is already scheduled; this is ordering,
         * not delay. */
        out->hold_reason = ATLAS_SEM_HOLD_FILE_INDEX;
        return ATLAS_OK;
    }

    /* UNAVAILABLE and DIRTY both build. The first is the repository's first
     * index and the second is its next one; they differ in what is preserved
     * while it runs, which is `atlas_sem_index_run`'s business rather than this
     * function's. */
    out->should_build = true;
    return ATLAS_OK;
}

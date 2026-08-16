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
 *   2. no build description, or automatic rebuild not enabled — an operator has
 *      not authorised a compiler to run over this repository, and A8-CI's rule
 *      that no model can cause one to run survives A9.2.3 because of this line;
 *   3. a build is already in flight;
 *   4. the last automatic attempt failed and nothing has changed since;
 *   5. freshness, which is A8-CI's axis;
 *   6. coverage, which is A9.2.3's.
 */
#include "atlas/sem_schedule.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/sem_ops.h"

const char *atlas_sem_activity_name(atlas_sem_activity a) {
    switch (a) {
    case ATLAS_SEM_ACT_DISABLED:
        return "DISABLED";
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
        ATLAS_SEM_HOLD_DISABLED,         ATLAS_SEM_HOLD_NO_COMPDB,
        ATLAS_SEM_HOLD_BUILDING,         ATLAS_SEM_HOLD_FAILED_UNCHANGED,
        ATLAS_SEM_HOLD_NO_LIBCLANG,      ATLAS_SEM_HOLD_CURRENT,
        ATLAS_SEM_HOLD_FILE_INDEX,
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
static bool coverage_is_complete(const atlas_sem_generation *g) {
    if (g->scope_discovery != ATLAS_SEM_SCOPE_DECLARED) {
        /* Includes every generation built before A9.2.3, which recorded no
         * scope. Conservative by construction rather than by a rule that says
         * so: a generation with no manifest is not one with a complete one. */
        return false;
    }
    if (g->tu_partial != 0 || g->tu_failed != 0 || g->tu_unsupported != 0) {
        return false;
    }
    return g->scope_uncovered == 0;
}

atlas_status atlas_sem_plan_for(atlas_db *db, atlas_repo_info *repo, bool building,
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

    out->freshness =
        atlas_sem_freshness_now(db, repo, &gen, found, running, &out->stale_reason);
    if (found) {
        out->generation_id = gen.id;
        (void)snprintf(out->generation_identity, sizeof out->generation_identity, "%s",
                       gen.source_identity);
        out->scope_discovery = gen.scope_discovery;
        out->scope_candidates = gen.scope_candidates;
        out->scope_covered = gen.scope_covered;
        out->scope_uncovered = gen.scope_uncovered;
        out->coverage_complete = coverage_is_complete(&gen);
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
    out->auto_rebuild = cfg.present && cfg.auto_rebuild;
    out->fail_count = cfg.fail_count;
    (void)snprintf(out->fail_reason, sizeof out->fail_reason, "%s", cfg.fail_reason);
    (void)snprintf(out->fail_at, sizeof out->fail_at, "%s", cfg.fail_at);
    const bool identity_unmoved =
        cfg.fail_count > 0 && cfg.fail_identity[0] != '\0' && out->source_identity[0] != '\0' &&
        strcmp(cfg.fail_identity, out->source_identity) == 0;
    const bool have_compdbs = cfg.present && cfg.compdbs.len > 0;
    atlas_sem_config_free(&cfg);

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
    if (!out->auto_rebuild) {
        /* Not a fault. A repository nobody has configured is not one that is
         * failing, and reporting it as such would teach an operator to ignore
         * the state that means something. */
        out->activity = ATLAS_SEM_ACT_DISABLED;
        out->hold_reason = ATLAS_SEM_HOLD_DISABLED;
        return ATLAS_OK;
    }
    if (!have_compdbs) {
        out->activity = ATLAS_SEM_ACT_DISABLED;
        out->hold_reason = ATLAS_SEM_HOLD_NO_COMPDB;
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
    if (out->activity == ATLAS_SEM_ACT_CURRENT || out->activity == ATLAS_SEM_ACT_INCOMPLETE) {
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

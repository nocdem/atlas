/* Atlas - A9.2.3: what state a repository's semantic index is in, and whether
 * the daemon should do something about it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## The whole state is derived, and that is not an optimisation
 *
 * There is no dirty bit and there must not be one. Freshness is recomputed on
 * every read — A6's rule about gate freshness and A4's about link currency —
 * and a stored dirty flag would be a second answer to a question that already
 * has one, free to disagree with it. Every value here is a pure function of the
 * generation rows, the build description, the repository row and whether a build
 * is in flight, so the scheduler and every reporting surface see the same state
 * because they compute the same thing rather than because somebody keeps two
 * things in step.
 *
 * ## Coalescing falls out of that rather than being implemented
 *
 * The scheduler always builds *now*: it never queues a build of a state it
 * observed earlier, because there is no queue of states — there is one question,
 * asked again on the next tick. A developer saving six files during a build
 * produces one further build, not six, and no mutation is lost, because the
 * build that follows is a build of whatever the tree holds when it starts. If
 * that has moved again by the time it publishes, the next tick sees STALE and
 * builds again. Correctness never depends on timing; only how soon it converges
 * does.
 *
 * ## Two axes, kept apart
 *
 * `atlas_sem_freshness` is A8-CI's axis and is unchanged: does this generation
 * describe the current source? Coverage is A9.2.3's second axis: how much of the
 * repository did it read? `INCOMPLETE` is the state where the first says yes and
 * the second says no, and it exists precisely so that neither is derived from
 * the other. A generation can be perfectly current and describe half a tree.
 */
#ifndef ATLAS_SEM_SCHEDULE_H
#define ATLAS_SEM_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/sem.h"

/* The state of one repository's semantic index, folding freshness, coverage and
 * the build description into the one value an operator or a model actually acts
 * on. Reported *beside* the two axes, never instead of them.
 *
 * UNKNOWN is zero, for the reason every Atlas vocabulary keeps its zero there: a
 * zeroed struct must not describe a healthy index. */
typedef enum atlas_sem_activity {
    /* Nothing has been established — a read failed, or nobody asked. */
    ATLAS_SEM_ACT_UNKNOWN = 0,
    /* Automatic maintenance is off because the root-owned default says so and
     * no operator has said otherwise. A configured fact rather than a fault: a
     * repository nobody has spoken about is not one that is failing.
     *
     * A9.2.4 narrowed this. Before it, DISABLED also covered "no operator has
     * enabled this repository" — which after the default reversed is no longer a
     * state that exists — and "there is no compilation database", which is now
     * NO_INPUTS. Keeping all three under one word was what made
     * `activity = DISABLED` unreadable in the field: it named a policy, a
     * configuration gap and a missing file with the same string. */
    ATLAS_SEM_ACT_DISABLED,
    /* An operator explicitly refused automatic maintenance for this repository.
     * Distinct from DISABLED on purpose, and never lifted by a policy change:
     * the two are a machine-wide default and a per-repository decision, and
     * collapsing them would let a default quietly overrule a person. */
    ATLAS_SEM_ACT_EXPLICITLY_DISABLED,
    /* Maintenance would run, and build-input discovery accepted no compilation
     * database. Not a refusal and not a failure — most often a repository whose
     * build has not been configured yet — and it is deliberately not DISABLED,
     * because the remedy is to generate a build description rather than to
     * change a setting. */
    ATLAS_SEM_ACT_NO_INPUTS,
    /* No usable generation exists. Different from DISABLED, and different from
     * STALE: nobody has ever indexed this. */
    ATLAS_SEM_ACT_UNAVAILABLE,
    /* The generation describes the current source *and* its coverage is
     * complete. CURRENT never means only "a semantic index exists". */
    ATLAS_SEM_ACT_CURRENT,
    /* Source-current, coverage incomplete. The state that must not be hidden
     * behind a green badge. */
    ATLAS_SEM_ACT_INCOMPLETE,
    /* A generation is being built right now; the previous one is still served. */
    ATLAS_SEM_ACT_BUILDING,
    /* The source has moved past the published generation and a rebuild is due.
     * The last-known-good generation is still served and still says which source
     * it describes. */
    ATLAS_SEM_ACT_DIRTY,
    /* The last automatic attempt failed and the source identity has not moved
     * since, so retrying would fail the same way. The last-known-good generation
     * is preserved and the reason is recorded. */
    ATLAS_SEM_ACT_FAILED
} atlas_sem_activity;

const char *atlas_sem_activity_name(atlas_sem_activity a);

/* Why the scheduler is not building right now, when it is not.
 *
 * Fixed Atlas strings, never assembled from repository bytes or compiler output
 * — the discipline `ATLAS_SEM_STALE_*` and `ATLAS_SEM_WHY_*` follow. */
#define ATLAS_SEM_HOLD_DISABLED "no_operator_has_enabled_automatic_rebuild_for_this_repository"
#define ATLAS_SEM_HOLD_NO_COMPDB "the_build_description_names_no_compilation_database"
/* A9.2.4. An operator's own refusal, honoured whatever the machine-wide default
 * says. Kept apart from every other reason a repository is not being maintained
 * so that a status surface can say *whose* decision it was. */
#define ATLAS_SEM_HOLD_EXPLICIT_DISABLE                                                            \
    "an_operator_explicitly_disabled_automatic_semantic_maintenance_for_this_repository"
/* The root-owned `semantic_auto_default` key says no, and nobody has said
 * otherwise about this repository. */
#define ATLAS_SEM_HOLD_POLICY_DEFAULT_OFF                                                          \
    "the_root_owned_policy_disables_automatic_semantic_maintenance_by_default"
/* Discovery ran and accepted nothing. Different from NO_COMPDB, which is about a
 * build description that names none: this is about a repository where Atlas
 * looked and there was nothing to find, which is a statement about the
 * repository rather than about its configuration. */
#define ATLAS_SEM_HOLD_NO_INPUTS "build_input_discovery_accepted_no_compilation_database"
/* Atlas has not walked this repository yet, so it has no accepted inputs and no
 * grounds to say there are none. Kept apart from NO_INPUTS for the reason this
 * season exists: "I looked and there is nothing here" and "I have not looked"
 * are different statements, and reporting the second as the first is the exact
 * conflation A9.2.2's absence rule refuses one layer up. */
#define ATLAS_SEM_HOLD_NOT_DISCOVERED "build_input_discovery_has_not_run_for_this_repository"
#define ATLAS_SEM_HOLD_BUILDING "a_generation_is_already_being_built"
#define ATLAS_SEM_HOLD_FAILED_UNCHANGED "the_last_attempt_failed_and_the_source_has_not_changed_since"
#define ATLAS_SEM_HOLD_NO_LIBCLANG "this_atlas_was_built_without_libclang"
#define ATLAS_SEM_HOLD_CURRENT "the_published_generation_describes_the_current_source"
/* The file index is what the source identity is computed from, so a semantic
 * generation built while it is behind describes hashes nobody can vouch for and
 * would be stale the moment it published. The reconciliation pass that fixes
 * this is already scheduled; waiting for it is not a delay, it is the ordering. */
#define ATLAS_SEM_HOLD_FILE_INDEX "the_file_index_has_not_caught_up_with_the_working_tree"
bool atlas_sem_hold_reason_is_known(const char *reason);
/* Atlas' own copy of a known reason, or NULL. A value that arrived over a socket
 * is a matching string, not Atlas' string — `atlas_sem_why_intern`'s rule. */
const char *atlas_sem_hold_intern(const char *reason);

typedef struct atlas_sem_plan {
    atlas_sem_activity activity;

    /* A8-CI's axis, unchanged, with its own reason vocabulary. */
    atlas_sem_freshness freshness;
    const char *stale_reason; /* an ATLAS_SEM_STALE_* string, or NULL */

    /* A9.2.3's axis. `coverage_complete` is false whenever a unit did not
     * complete *or* the scope was not fully covered *or* the scope discovery is
     * UNKNOWN — three different problems that all mean the same thing about what
     * a negative conclusion may rest on. */
    bool coverage_complete;
    atlas_sem_scope_discovery scope_discovery;
    int64_t scope_candidates;
    int64_t scope_covered;
    int64_t scope_uncovered;

    /* What the scheduler decided, and why. `should_build` is the one output the
     * daemon acts on; `hold_reason` is what it says when it does not. */
    bool should_build;
    const char *hold_reason; /* an ATLAS_SEM_HOLD_* string, or NULL */

    bool configured;
    /* The effective answer — what the daemon acts on. Derived from the intent
     * and the root-owned default by `atlas_sem_auto_effective`. */
    bool auto_rebuild;

    /* --- A9.2.4: the third axis, and who decided it -------------------------
     *
     * Reported beside the effective answer rather than instead of it, for the
     * reason every season since A9.1 reports its axes separately: `auto_rebuild
     * = false` is one fact and three quite different situations, and a surface
     * that showed only the boolean would tell an operator their repository is
     * off without telling them whether that was their own doing, a machine-wide
     * policy, or a default nobody has ever revisited. */
    atlas_sem_auto_intent auto_intent;
    atlas_sem_intent_source auto_intent_by;
    bool policy_default;

    /* Build-input discovery, as it stands now — not as the published generation
     * recorded it. Both are reported: `discovery` says what Atlas can currently
     * account for, `generation_discovery` says what the index being served was
     * built under, and they differ exactly when a rebuild is due. */
    atlas_sem_discovery discovery;
    atlas_sem_discovery_mode discovery_mode;
    atlas_sem_discovery generation_discovery;
    int64_t inputs_accepted;
    int64_t inputs_rejected;
    char discovered_at[ATLAS_TS_MAX];
    /* Why the last walk fell short, if it did — a ceiling, a directory Atlas
     * could not enter, or a subtree an operator excluded. A fixed Atlas string,
     * or "". A PARTIAL verdict without it tells an operator that something was
     * missed without telling them what, which is not enough to act on. */
    char discovery_limit[128];

    /* The generation being served, and the source identity of the tree as it is
     * now. Both reported so a surface can show the divergence rather than only
     * the verdict. */
    int64_t generation_id;
    char source_identity[65];
    char generation_identity[65];

    /* The retry governor's state, so a FAILED repository can say what happened
     * and how many times without anybody reading the table. */
    int64_t fail_count;
    char fail_reason[96]; /* a fixed Atlas string, or "" */
    char fail_at[ATLAS_TS_MAX];
} atlas_sem_plan;

void atlas_sem_plan_init(atlas_sem_plan *p);

/* Computes the plan for one repository. Reads only, writes nothing, takes no
 * lock and creates no process — A6's property of `atlas_gate_check`, for A6's
 * reason: asking what Atlas thinks must not change what Atlas thinks, and a
 * scheduler that mutated on inspection could not be run from a status command.
 *
 * `building` is the caller's own observation, because a read of the published
 * generation cannot see a build the caller has in flight. */
atlas_status atlas_sem_plan_for(atlas_db *db, atlas_repo_info *repo, bool building,
                                atlas_sem_plan *out, atlas_err *err);

/* The same computation with the machine-wide default supplied rather than read.
 *
 * `atlas_sem_plan_for` reads the root-owned policy itself, which is what every
 * ordinary caller wants. This form exists so that a test can drive the policy
 * both ways without a root-owned file, and so that a caller which has already
 * loaded the policy does not load it again per repository in a sweep. It is not
 * a bypass: the parameter is a boolean the caller must have obtained from
 * somewhere, and in the shipped binary every caller obtains it from
 * `atlas_syspolicy_semantic_auto_default`. */
atlas_status atlas_sem_plan_for_with_default(atlas_db *db, atlas_repo_info *repo, bool building,
                                             bool policy_default, atlas_sem_plan *out,
                                             atlas_err *err);

#endif /* ATLAS_SEM_SCHEDULE_H */

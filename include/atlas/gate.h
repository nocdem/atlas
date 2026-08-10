/* Atlas - impact gates and stale-decision detection.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A4 made it possible to record that something was decided, and to bind that
 * record to the bytes it was decided about. A6 is the phase that asks the
 * question those bindings were recorded for: *is this decision still about the
 * code that is there now?*
 *
 * Four rules shape this header. Like A4's, each of them is a rule about honesty
 * rather than about storage.
 *
 * 1. **An assessment is an observation, never a judgement about the decision.**
 *    STALE does not mean the decision was wrong, has been revoked, or no longer
 *    applies. It means the code the decision was validated against has moved,
 *    so a human has to look again. Atlas has no way to know whether an
 *    architectural decision survives a change to the code it concerns — that is
 *    a semantic question about intent, and Atlas holds bytes and graph edges.
 *    The whole of what STALE claims is "the anchors moved"; the whole of what
 *    IMPACTED claims is "a bounded walk from the anchors reached something that
 *    moved". Both are review signals. Neither is a verdict.
 *
 * 2. **Nothing here is computed by a model, and nothing here is cached.**
 *    Freshness is derived from stored Atlas facts and stored Git facts by
 *    deterministic code, so the same database and the same arguments give the
 *    same answer on any machine and in any order. It is recomputed on every
 *    read for the reason A4 gives for link currency: a cached freshness is
 *    wrong between the change and the recomputation, and "is this still
 *    current?" is precisely the question that must not be answered from a stale
 *    cache. The only assessment Atlas stores is the one a revalidation record
 *    captures, and that one is stored *because* it is history rather than
 *    state.
 *
 * 3. **UNKNOWN fails closed, and it is not a defect.** Atlas reaches UNKNOWN
 *    whenever it cannot prove a safe answer: the index is behind the working
 *    tree, an anchor will not resolve, history does not reach the validation
 *    point, a bound was hit, or stored state disagrees with itself. Every one
 *    of those makes the gate BLOCKED. A gate that answered PASS on incomplete
 *    information would be worse than no gate, because the answer is the same
 *    shape as a real one.
 *
 * 4. **A6 grants no new authority to anything a model can reach.** Freshness
 *    and gate results are readable — by the CLI, over IPC, and through MCP —
 *    and are mutable by nothing. There is no operation anywhere that clears a
 *    stale result, and the one operation that establishes a *new* validation
 *    point goes through A4's operator channel unchanged: an interactive
 *    terminal, a short-lived single-use capability bound to this exact
 *    revision, and a confirmation typed against its content hash. The honesty
 *    limits A4 states about that channel apply here word for word — it
 *    identifies the channel, not a person.
 *
 * See docs/impact-gates.md.
 */
#ifndef ATLAS_GATE_H
#define ATLAS_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/decision.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/sha256.h"

/* --- freshness ------------------------------------------------------------
 *
 * What Atlas can say about one approved revision, relative to one exact
 * repository state.
 *
 * UNKNOWN is zero on purpose. A zeroed assessment is an assessment nobody
 * filled in, and the safe reading of "nobody filled this in" is not "fresh". */
typedef enum atlas_gate_freshness {
    /* Atlas could not prove a safe answer. Fails closed. */
    ATLAS_GATE_UNKNOWN = 0,
    /* Every anchor still resolves to what it was validated against, and no
     * change in the range since the validation point reached the decision by
     * any path Atlas walked. */
    ATLAS_GATE_FRESH,
    /* Something the decision is directly bound to moved: content changed, an
     * anchor disappeared, or a name that resolved to one thing now resolves to
     * several. Requires human revalidation; does not mean the decision is
     * wrong. */
    ATLAS_GATE_STALE,
    /* The direct anchors still hold, but a bounded walk from them reached a
     * file or symbol that changed in the range. A conservative review signal. */
    ATLAS_GATE_IMPACTED
} atlas_gate_freshness;

const char *atlas_gate_freshness_name(atlas_gate_freshness f);
bool atlas_gate_freshness_parse(const char *name, atlas_gate_freshness *out);

/* --- the gate -------------------------------------------------------------
 *
 * The aggregate over every decision a query covered. BLOCKED is zero for the
 * same reason UNKNOWN is. */
typedef enum atlas_gate_result {
    ATLAS_GATE_BLOCKED = 0,
    ATLAS_GATE_PASS,
    ATLAS_GATE_REVIEW_REQUIRED
} atlas_gate_result;

const char *atlas_gate_result_name(atlas_gate_result r);
/* Folds one assessment into a running result. The mapping is a function rather
 * than prose in a comment, for the reason `atlas_decision_transition_allowed`
 * is: the tests ask this, so they cannot pass by agreeing with a second copy of
 * the rules.
 *
 *   any UNKNOWN            -> BLOCKED
 *   else any STALE or IMPACTED -> REVIEW_REQUIRED
 *   else                   -> PASS
 *
 * BLOCKED is absorbing: once a query has seen an UNKNOWN, no later FRESH can
 * lift it. */
atlas_gate_result atlas_gate_fold(atlas_gate_result running, atlas_gate_freshness f);

/* --- reason codes ---------------------------------------------------------
 *
 * A closed, stable, machine-readable vocabulary. These are Atlas string
 * literals: nothing repository-derived and nothing model-derived ever becomes
 * one, which is what lets them be stored packed, reported over JSON and shown
 * to a model without an encoding step.
 *
 * Every reason implies exactly one freshness, and `atlas_gate_reason_freshness`
 * is the single authority on which. An assessment's freshness is therefore not
 * chosen separately from its reasons — it is the weakest freshness among them,
 * so a verdict can never disagree with its own explanation. */
typedef enum atlas_gate_reason {
    /* FRESH. */
    ATLAS_GATE_REASON_NO_RELEVANT_CHANGE = 0,

    /* STALE: something directly bound moved. */
    ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED,
    ATLAS_GATE_REASON_LINKED_PATH_MISSING,
    ATLAS_GATE_REASON_LINKED_SYMBOL_MISSING,
    ATLAS_GATE_REASON_LINKED_SYMBOL_AMBIGUOUS,
    ATLAS_GATE_REASON_LINKED_COMMIT_MISSING,

    /* IMPACTED: a bounded walk reached a change. */
    ATLAS_GATE_REASON_DEPENDENCY_CHANGED,

    /* UNKNOWN: Atlas cannot prove a safe answer. */
    /* The working tree or Git HEAD is ahead of what Atlas has indexed. */
    ATLAS_GATE_REASON_INDEX_LAG,
    /* The structural graph is not current with the file index. */
    ATLAS_GATE_REASON_STRUCTURAL_INDEX_STALE,
    /* The validation point commit is not in the index at all. */
    ATLAS_GATE_REASON_UNREACHABLE_BASE,
    /* The validation point is provably not an ancestor of the indexed head. */
    ATLAS_GATE_REASON_HISTORY_REWRITTEN,
    /* A walk or a collection hit its ceiling, so the result would be a subset
     * of an answer rather than an answer. */
    ATLAS_GATE_REASON_TRAVERSAL_LIMIT,
    /* An anchor resolved to nothing Atlas can compare — usually because the
     * repository has never been indexed. */
    ATLAS_GATE_REASON_EVIDENCE_UNRESOLVED,
    /* The revision records no basis and no revalidation, so there is no point
     * in history to measure a change range from. */
    ATLAS_GATE_REASON_MISSING_VALIDATION_POINT,
    /* The decision's durable identity does not match the repository it was
     * assessed against, or matches more than one. */
    ATLAS_GATE_REASON_REPOSITORY_AMBIGUOUS,
    /* The revision's stored content does not hash to its stored digest, so
     * every approval bound to that digest covers bytes that are not there. */
    ATLAS_GATE_REASON_CONTENT_HASH_MISMATCH,
    /* The decision claims a breadth Atlas cannot test at this state — a
     * repository-wide decision with no validation point to bound a range. */
    ATLAS_GATE_REASON_SCOPE_NOT_ASSESSABLE,

    ATLAS_GATE_REASON__COUNT
} atlas_gate_reason;

const char *atlas_gate_reason_name(atlas_gate_reason r);
bool atlas_gate_reason_parse(const char *name, atlas_gate_reason *out);
/* The single authority on what each reason means for a verdict. */
atlas_gate_freshness atlas_gate_reason_freshness(atlas_gate_reason r);

/* --- the evidence-set digest ----------------------------------------------
 *
 * A digest of what a decision's anchors resolve to *right now*, as opposed to
 * `atlas_decision_content_hash`, which digests what they were recorded as.
 *
 * The two exist for opposite reasons and must never be confused. The content
 * hash is immutable: it is what an approval bound, and it changes only when the
 * revision does, which is never. This one is expected to change, and the point
 * of computing it is to notice when it has. A revalidation binds one, so that a
 * capability issued against one view of the code cannot be spent against
 * another — the same protection the content hash gives an approval, applied to
 * the moving half.
 *
 * Domain-separated and length-prefixed for exactly the reasons A4 gives: a
 * delimiter is a byte a path can contain, and a bare SHA-256 says nothing about
 * what was hashed. Links contribute in the canonical order Atlas imposes on a
 * set, so a reordering is not a drift.
 *
 * It covers, per link: the kind, the selector bytes as stored, and what those
 * bytes resolve to in the current index — a file's content hash, a symbol's
 * defining file hash and line and candidate count, a commit's presence — plus
 * the resolved currency. It does **not** cover the indexed head, which is bound
 * separately and for a separate reason: a commit that changed nothing the
 * decision touches is drift in the head and not in the evidence. */
#define ATLAS_GATE_EVIDENCE_DOMAIN "atlas.gate.evidence.v1"

/* --- one assessment -------------------------------------------------------
 *
 * Everything a caller needs to act, and everything a reader needs to check the
 * verdict against its inputs. Every identity the assessment was bound to is
 * carried, because an assessment that does not say what it was about is not
 * reproducible and therefore not evidence of anything. */
typedef struct atlas_gate_assessment {
    /* --- what was assessed ------------------------------------------------ */
    int64_t repo_id;
    atlas_buf repo_name;  /* safe-encoded */
    atlas_buf root_text;  /* safe-encoded */
    atlas_buf repo_identity_hash;

    atlas_buf uid; /* the decision's public uid */
    int64_t document_id;
    int64_t revision_id;
    int64_t revision_no;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_decision_state state;
    atlas_decision_scope scope;
    /* The revision's title. Untrusted project prose, already safe-encoded by
     * the service layer, and labelled as such by both renderers. */
    atlas_buf title;

    /* --- against what state ----------------------------------------------- */
    /* The last point a human validated this revision against: the newest
     * revalidation record if there is one, otherwise the basis the revision was
     * proposed and approved at. Empty when neither is known. */
    char validated_at_commit[ATLAS_OID_HEX_MAX_INCL];
    /* True when `validated_at_commit` came from a revalidation record rather
     * than from the revision's own basis. */
    bool validated_by_revalidation;
    int64_t revalidation_count;
    /* The commit Atlas' index describes — the snapshot the whole assessment was
     * computed from. */
    char indexed_commit[ATLAS_OID_HEX_MAX_INCL];
    /* What the caller asked about. Equal to `indexed_commit` unless the caller
     * named one, in which case a difference is INDEX_LAG rather than an
     * extrapolation. */
    char requested_commit[ATLAS_OID_HEX_MAX_INCL];
    /* The digest of what the anchors resolve to at `indexed_commit`. */
    char evidence_digest[ATLAS_SHA256_HEX_LEN + 1u];

    /* --- the verdict ------------------------------------------------------ */
    atlas_gate_freshness freshness;
    atlas_gate_reason reasons[ATLAS_GATE_MAX_REASONS];
    size_t reason_count;

    /* --- the evidence for it ---------------------------------------------- */
    int64_t links_total;
    int64_t links_current;
    int64_t links_changed;
    int64_t links_missing;
    int64_t links_ambiguous;
    int64_t links_unknown;
    /* Commits between the validation point and the indexed head, and distinct
     * paths they touched. */
    int64_t range_commits;
    int64_t range_paths;
    /* Nodes the structural walk reached, and how many of them were in the
     * change set. */
    int64_t walk_visited;
    int64_t walk_matched;
    /* True when any bound in this assessment was reached. Always accompanied by
     * TRAVERSAL_LIMIT, and therefore by UNKNOWN: a limit is never absorbed. */
    bool limit_reached;
    /* Which one, as an Atlas literal. NULL when none was. */
    const char *limit_detail;
} atlas_gate_assessment;

void atlas_gate_assessment_init(atlas_gate_assessment *a);
void atlas_gate_assessment_free(atlas_gate_assessment *a);
/* Records a reason and folds its freshness into the verdict.
 *
 * This is the only way an assessment's freshness is ever set, which is what
 * makes "the verdict is the weakest of its reasons" a property of the code
 * rather than a discipline. Duplicates are absorbed; overflow past
 * ATLAS_GATE_MAX_REASONS keeps the verdict and drops the surplus reason, and
 * cannot lose information that would change the verdict because the verdict is
 * already at least as weak as anything dropped. */
void atlas_gate_assessment_note(atlas_gate_assessment *a, atlas_gate_reason r);
/* Packs an assessment's reasons into the stored form: reason names separated by
 * single spaces, in ascending enum order, bounded by ATLAS_GATE_MAX_REASON_TEXT.
 * Every byte is an Atlas literal. */
atlas_status atlas_gate_reasons_pack(const atlas_gate_assessment *a, atlas_buf *out,
                                     atlas_err *err);
/* The inverse, refusing any token that is not in the vocabulary rather than
 * reproducing it. A stored list that fails to parse is a corrupt record, not a
 * list with an extra element. */
atlas_status atlas_gate_reasons_unpack(const char *packed, atlas_gate_reason *out, size_t max,
                                       size_t *count_out, atlas_err *err);

/* --- a gate query ---------------------------------------------------------- */

typedef struct atlas_gate_query {
    /* Where. Exactly one is normally set; `root` wins, as everywhere else. */
    const char *repo_name;
    const char *root;
    /* The exact repository state to assess. NULL or empty means "whatever Atlas
     * has indexed"; a value that is not the indexed head is INDEX_LAG, never an
     * extrapolation to a state Atlas has not seen. */
    const char *at_commit;
    /* Optional target scope: repository-relative path prefixes, as raw bytes in
     * the `%XX` text form every Atlas path input accepts. A decision is in
     * scope when any anchor of it is under one of these, and a
     * repository-scoped decision is always in scope. */
    const char *paths[ATLAS_GATE_MAX_SCOPE_PATHS];
    size_t path_count;
    /* One decision, by public uid. When set, only that decision is assessed —
     * the same engine, over one document instead of all of them, so a
     * single-decision question costs what one assessment costs rather than what
     * the whole repository's does. */
    const char *only_uid;
    /* Structural traversal depth. 0 takes ATLAS_GATE_DEFAULT_IMPACT_DEPTH;
     * anything above ATLAS_GATE_MAX_IMPACT_DEPTH is a usage error rather than a
     * clamp, because a silently reduced depth is a silently smaller answer. */
    int64_t depth;
} atlas_gate_query;

void atlas_gate_query_init(atlas_gate_query *q);

typedef struct atlas_gate_report {
    int64_t repo_id;
    atlas_buf repo_name;
    atlas_buf root_text;
    atlas_buf repo_identity_hash;
    char indexed_commit[ATLAS_OID_HEX_MAX_INCL];
    char requested_commit[ATLAS_OID_HEX_MAX_INCL];
    int64_t depth;

    atlas_gate_result result;
    /* Owned. Ordered by decision uid, so two runs over one database emit the
     * same document byte for byte. */
    atlas_gate_assessment *items;
    size_t item_count;

    int64_t fresh;
    int64_t stale;
    int64_t impacted;
    int64_t unknown;
    /* Approved decisions the scope excluded, so a reader can tell an empty
     * result from a filtered one. */
    int64_t out_of_scope;
    bool limit_reached;
    const char *limit_detail;
} atlas_gate_report;

void atlas_gate_report_init(atlas_gate_report *r);
void atlas_gate_report_free(atlas_gate_report *r);

/* --- exit codes ------------------------------------------------------------
 *
 * The gate extends Atlas' stable exit-code contract rather than reusing it.
 * 0..7 keep their meanings exactly; a gate outcome is not an error and must not
 * be reported as one, but REVIEW_REQUIRED and BLOCKED must both be non-zero so
 * that `atlas gate check && deploy` cannot proceed on either.
 *
 * These are deliberately distinct from each other: an automation that treats
 * "a human should look at this" and "Atlas could not tell" identically is one
 * that will eventually be given the second and act as though it got the
 * first. */
#define ATLAS_EXIT_GATE_REVIEW_REQUIRED 8
#define ATLAS_EXIT_GATE_BLOCKED 9

int atlas_gate_exit_code(atlas_gate_result r);

/* --- running one --------------------------------------------------------
 *
 * Declared against a database handle rather than a context, because the IPC
 * server answers from a per-request read-only handle and there is exactly one
 * implementation of an assessment. `atlas_service_gate_check` is this with a
 * context unwrapped, and nothing else.
 *
 * Both are reads. Neither takes the writer lock and neither writes a row. */
struct atlas_db;
atlas_status atlas_gate_run(struct atlas_db *db, const atlas_gate_query *q,
                            atlas_gate_report *out, atlas_err *err);
atlas_status atlas_gate_run_one(struct atlas_db *db, const char *repo, const char *uid,
                                const char *at_commit, atlas_gate_report *out, atlas_err *err);

#endif /* ATLAS_GATE_H */

/* Atlas - `atlas maintenance plan|prune`.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * RETENTION[] below is the whole retention policy. Every table in the schema
 * has exactly one row in it, and adding a table without adding a row is a test
 * failure rather than an omission that nobody notices: `tests/test_backup.c`
 * compares this array against `sqlite_schema` and fails on either direction.
 *
 * The `reason` strings are the deliverable. A classification without one is a
 * label, and a label is what lets a later phase quietly reclassify a table
 * because deleting from it would have been convenient.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/limits.h"
#include "atlas/lock.h"
#include "atlas/maintenance.h"

const char *atlas_retention_class_name(atlas_retention_class c) {
    switch (c) {
    case ATLAS_RETAIN_CANONICAL:
        return "canonical";
    case ATLAS_RETAIN_MEMORY:
        return "memory";
    case ATLAS_RETAIN_DERIVED:
        return "derived";
    case ATLAS_RETAIN_OPERATIONAL:
        return "operational";
    }
    return "unknown";
}

typedef struct retention_entry {
    const char *table;
    atlas_retention_class cls;
    bool prunable;
    const char *reason;
} retention_entry;

/* How a prunable table counts and removes its own eligible rows.
 *
 * Until A9 there was one prunable table and the loop below called the
 * `repo_events` functions by name. That was correct exactly as long as the
 * count stayed one, and silently wrong the moment it did not: a second prunable
 * table would have been counted, and *deleted from*, using `repo_events`'
 * query — reporting the wrong number and removing the wrong rows, while every
 * existing test still passed.
 *
 * So the pair lives here, keyed by table name, and `pruner_for` returns NULL
 * for anything not listed. A table marked prunable in RETENTION[] with no row
 * here cannot be pruned at all rather than falling back to some default, and
 * `tests/test_maintenance.c` asserts the two lists agree — in both directions,
 * because a pruner for a table nobody marked prunable is the more dangerous
 * half. */
typedef atlas_status (*retention_eligible_fn)(atlas_db *db, const char *cutoff, int64_t retain,
                                              int64_t *out, atlas_err *err);
typedef atlas_status (*retention_prune_fn)(atlas_db *db, const char *cutoff, int64_t retain,
                                           int64_t batch, int64_t *removed_out, bool *more_out,
                                           atlas_err *err);

typedef struct retention_pruner {
    const char *table;
    retention_eligible_fn eligible;
    retention_prune_fn prune;
} retention_pruner;

static const retention_pruner PRUNERS[] = {
    {"repo_events", atlas_db_maintenance_events_eligible, atlas_db_maintenance_events_prune},
    {"gw_audit", atlas_db_maintenance_audit_eligible, atlas_db_maintenance_audit_prune},
};

static const retention_pruner *pruner_for(const char *table) {
    for (size_t i = 0; i < sizeof PRUNERS / sizeof PRUNERS[0]; i++) {
        if (strcmp(PRUNERS[i].table, table) == 0) {
            return &PRUNERS[i];
        }
    }
    return NULL;
}

/* One row per table. `prunable` is true exactly twice: `repo_events` since A5 and
 * `gw_audit` since A9, each with its argument written beside it. */
static const retention_entry RETENTION[] = {
    /* --- the schema's own record ----------------------------------------- */
    {"schema_migrations", ATLAS_RETAIN_CANONICAL, false,
     "the record of which migrations ran; losing a row makes the database's own version a guess"},

    /* --- registration ----------------------------------------------------- */
    {"repositories", ATLAS_RETAIN_CANONICAL, false,
     "registrations are removed by `atlas repo remove`, an explicit act aimed at one repository, "
     "never by age"},

    /* --- pass history ----------------------------------------------------- */
    {"scans", ATLAS_RETAIN_OPERATIONAL, false,
     "files.first_seen_scan_id, last_seen_scan_id and deleted_scan_id hold scans.id, which is a "
     "plain rowid SQLite reuses; deleting an old pass would hand its id to a future one and make "
     "those columns describe a different pass"},

    /* --- the file and history index --------------------------------------- */
    {"files", ATLAS_RETAIN_DERIVED, false,
     "the live index; a deleted=1 row is the record that a path went away, not dead weight"},
    {"commits", ATLAS_RETAIN_DERIVED, false,
     "rebuildable only by rewalking history from scratch; repo_commit_tips is the cursor that "
     "says it need not be, so removing commits by age would make that cursor a lie"},
    {"file_changes", ATLAS_RETAIN_DERIVED, false, "per-commit path changes; same cursor as commits"},
    {"compile_databases", ATLAS_RETAIN_DERIVED, false,
     "the digest and argument summary of an observed compile_commands.json; rebuilt whole by a "
     "structural pass"},
    {"repo_worktree_changes", ATLAS_RETAIN_DERIVED, false,
     "a live observation of the working tree, replaced whole on every pass"},

    /* --- provenance -------------------------------------------------------- */
    {"evidence", ATLAS_RETAIN_CANONICAL, false,
     "provenance is what makes a result citable; an index whose evidence has been aged out still "
     "answers, and can no longer say why"},

    /* --- daemon and reconciliation state ----------------------------------- */
    {"repo_index_state", ATLAS_RETAIN_OPERATIONAL, false,
     "one row per repository holding the generation, the event-gap flag and the sync cursor; it "
     "is state, not history"},
    {"repo_commit_tips", ATLAS_RETAIN_OPERATIONAL, false,
     "the per-ref ingest cursor; removing it silently converts the next pass into a full history "
     "replay"},
    {"daemon_state", ATLAS_RETAIN_OPERATIONAL, false,
     "a single liveness row bounded by its own CHECK"},

    /* --- the one prunable table -------------------------------------------- */
    {"repo_events", ATLAS_RETAIN_OPERATIONAL, true,
     "a bounded stream of observations that already carries a documented per-repository ceiling; "
     "consumers hold an id cursor and the durable SOURCE and GIT evidence is in `evidence`, which "
     "is never pruned with it"},

    /* --- A2: sessions, attribution, reasons, proposals ---------------------- */
    {"ai_clients", ATLAS_RETAIN_MEMORY, false,
     "the identity every session is attributed through"},
    {"ai_sessions", ATLAS_RETAIN_MEMORY, false,
     "a session is what a reason is attached to; removing one would orphan or, worse, re-point "
     "the record of who said something"},
    {"ai_session_repos", ATLAS_RETAIN_MEMORY, false, "which repositories a session touched"},
    {"ai_session_events", ATLAS_RETAIN_MEMORY, false, "the session's own lifecycle record"},
    {"ai_change_sets", ATLAS_RETAIN_MEMORY, false, "what changed within one turn"},
    {"ai_changed_paths", ATLAS_RETAIN_MEMORY, false,
     "including the explicit UNKNOWN rows written for paths nobody explained; those are the "
     "honest gaps, and deleting them would turn a recorded gap into an absence"},
    {"ai_reasons", ATLAS_RETAIN_MEMORY, false, "the engineering memory Atlas exists to hold"},
    {"ai_reason_paths", ATLAS_RETAIN_MEMORY, false, "what each reason was about"},
    {"ai_decisions", ATLAS_RETAIN_MEMORY, false,
     "model-proposed decisions; a promoted one is pointed at from decision_revisions"},
    {"ai_decision_paths", ATLAS_RETAIN_MEMORY, false, "what each proposal was about"},
    {"ai_evidence_links", ATLAS_RETAIN_MEMORY, false, "what a record cited"},
    {"ai_checkpoints", ATLAS_RETAIN_MEMORY, false, "session continuity across compaction"},

    /* --- A3: the structural graph ------------------------------------------ */
    {"code_analyzers", ATLAS_RETAIN_DERIVED, false,
     "the interned producer identity every graph row references"},
    {"code_index_state", ATLAS_RETAIN_OPERATIONAL, false,
     "per-repository structural cursor, including resolve_settled"},
    {"code_files", ATLAS_RETAIN_DERIVED, false,
     "rebuilt whole by a structural pass; an age-pruned graph is not a smaller graph, it is a "
     "wrong one, and nothing in it says rows are missing"},
    {"code_file_roles", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_files"},
    {"code_symbols", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_files"},
    {"code_occurrences", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_files"},
    {"code_relations", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_files"},
    {"code_candidates", ATLAS_RETAIN_DERIVED, false,
     "the recorded candidate set of an ambiguity; dropping it would make an ambiguity look "
     "smaller than it is"},
    {"code_units", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_files"},
    {"code_unit_includes", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_units"},
    {"code_unit_defines", ATLAS_RETAIN_DERIVED, false, "rebuilt with code_units"},
    {"code_index_errors", ATLAS_RETAIN_DERIVED, false,
     "what a pass could not parse; reported rather than silently absent, and replaced by the next "
     "pass over the same file"},

    /* --- A4: decisions ------------------------------------------------------ */
    {"decision_documents", ATLAS_RETAIN_CANONICAL, false,
     "canonical; nothing in Atlas deletes a decision record"},
    {"decision_revisions", ATLAS_RETAIN_CANONICAL, false,
     "immutable, and each approval is bound to a revision's content hash"},
    {"decision_alternatives", ATLAS_RETAIN_CANONICAL, false,
     "part of what a revision hashes, so removing one would invalidate an approval's digest"},
    {"decision_links", ATLAS_RETAIN_CANONICAL, false,
     "durable selector snapshots, also inside the canonical hash"},
    {"decision_events", ATLAS_RETAIN_CANONICAL, false,
     "the append-only ledger the status columns are only a cache of"},
    {"decision_edge_events", ATLAS_RETAIN_CANONICAL, false,
     "the only durable account of why one decision was related to another, and of why a relation "
     "was withdrawn; the edge itself lives in an immutable revision but the reason for it lives "
     "nowhere else, so an age-pruned history would leave live relations no reader could explain "
     "and would erase the removal evidence that distinguishes a withdrawn edge from one that was "
     "never drawn"},
    {"decision_challenges", ATLAS_RETAIN_CANONICAL, false,
     "a consumed challenge is part of an approval record and the event points at it; expired "
     "unconsumed ones are already removed at the point of use, which is the only DELETE the "
     "decision tables have"},
    /* --- A6: revalidation --------------------------------------------------- */
    {"decision_validations", ATLAS_RETAIN_CANONICAL, false,
     "the record that a human checked an approved decision against an exact repository state, "
     "together with the assessment and the reasons that prompted them to; it is the evidence "
     "that a concern was addressed rather than ignored, and an age-pruned validation history "
     "would silently move every surviving decision's validation point back to whatever record "
     "happened to remain — widening its change range and, for the ones whose every record went, "
     "removing the only proof that anybody ever looked"},

    {"decision_search", ATLAS_RETAIN_DERIVED, false,
     "the searchable projection of decision prose, rebuilt from the canonical rows"},

    /* --- A8: orchestration ---------------------------------------------------
     *
     * Every one of these is CANONICAL and none is prunable, and that is not
     * caution. Nothing rebuilds an orchestration row: the repository never held
     * what was asked for, who asked, what was granted, what ran or what came
     * back, so a job record is the only account of it that exists. An age-pruned
     * execution history would leave a control plane that can say a job succeeded
     * and cannot say what it did.
     *
     * A retention policy that mattered here would be about the *workspaces* on
     * disk — bytes the worker owns, bounded by the dispatcher — not about these
     * rows, which are small and are the audit trail. */
    {"orch_runs", ATLAS_RETAIN_CANONICAL, false,
     "the durable grouping one chain of tasks belongs to: its root, the repository it is bound "
     "to, and whether it has been settled; nothing rebuilds it, and pruning a run by age would "
     "orphan every task still pointing at it while leaving those tasks perfectly readable, which "
     "is a worse answer than not pruning at all"},
    {"orch_jobs", ATLAS_RETAIN_CANONICAL, false,
     "the specification, submitter and outcome of every job Atlas accepted; it is the only record "
     "of what was asked for, and its digest is what makes an idempotent resubmission provably the "
     "same request rather than a similar one"},
    {"orch_attempts", ATLAS_RETAIN_CANONICAL, false,
     "one row per execution attempt, holding the dispatcher that took it and how it ended; "
     "attempt numbers are monotonic per job and a missing row would make a later attempt look "
     "like an earlier one"},
    {"orch_leases", ATLAS_RETAIN_CANONICAL, false,
     "which attempt held the exclusive right to execute a job, and until when; the partial unique "
     "index over unreleased leases is what makes concurrent execution impossible, and a deleted "
     "row is a lease nothing can prove was ever exclusive"},
    {"orch_transitions", ATLAS_RETAIN_CANONICAL, false,
     "the append-only state ledger, and the ordering authority every job row points at through "
     "state_seq; its AUTOINCREMENT id must never be reissued, so nothing may delete from it"},
    {"orch_events", ATLAS_RETAIN_CANONICAL, false,
     "what the worker reported while it ran, recorded as the worker's claim; it is the only "
     "narrative of a failed job and the thing an operator reads when deciding whether to retry"},
    {"orch_artifacts", ATLAS_RETAIN_CANONICAL, false,
     "the manifest — name, size and digest — of what an attempt produced, and for small artifacts "
     "the bytes; the patch a job generated is evidence of what a model proposed and is never "
     "regenerable, because the model run that produced it is gone"},
    {"orch_idempotency", ATLAS_RETAIN_CANONICAL, false,
     "the key-to-job mapping that makes resubmission replay-safe; removing a row by age turns a "
     "replay into a second execution of work that already ran"},
    {"orch_snapshots", ATLAS_RETAIN_CANONICAL, false,
     "the identity of the exact source an attempt was given: the pinned commit, the tree, the "
     "entry count and the domain-separated digest; it is the only record of what a job actually "
     "ran against, and without it a produced patch describes a tree nobody can name"},
    {"orch_snapshot_entries", ATLAS_RETAIN_CANONICAL, false,
     "the ordered manifest the snapshot digest is computed over; the order is part of the "
     "identity, so removing or renumbering a row would silently change what the digest means "
     "rather than making the record smaller"},

    {"orch_observations", ATLAS_RETAIN_CANONICAL, false,
     "the worker's own account of the phase it believed it was in, written on a phase change "
     "rather than per heartbeat so the row count is bounded by the state machine; it is what "
     "distinguishes a worker that died silently from one that never started"},

    /* --- A8-CI: the compiler-derived semantic index --------------------------
     *
     * Every one of these is DERIVED, and that classification is the whole
     * architectural claim of the season: a semantic index is rebuildable from
     * the repository and its compilation database, and nothing authoritative
     * may depend on it. If all seven tables were dropped, Atlas would lose no
     * decision, no revision, no approval, no link rationale and no audit event
     * — it would lose an answer it can compute again.
     *
     * None is *prunable* even so, and the reason is A5's about `scans` rather
     * than a reluctance to delete. These tables are pruned as a unit or not at
     * all: a generation with some of its units removed by age is not a smaller
     * index, it is a wrong one, and nothing in it records that rows are
     * missing. Discarding a generation is `sem` index work — a replacement — not
     * retention work, and it happens when an operator asks for an index, not on
     * a timer. A8-CI adds no background deleter, exactly as A5 forbids. */
    {"sem_generations", ATLAS_RETAIN_DERIVED, false,
     "one row per attempt to build a semantic index, successful or not; rebuilt by indexing, and "
     "kept whole because a failed attempt is the operational record that indexing is failing — a "
     "table holding only successes could not state it"},
    {"sem_current", ATLAS_RETAIN_DERIVED, false,
     "the one-row-per-repository pointer at the published generation; it is what makes replacement "
     "atomic, and a pruned pointer would leave a complete index nobody can find"},
    {"sem_compdbs", ATLAS_RETAIN_DERIVED, false,
     "the compilation databases a generation read and their digests; rebuilt by indexing, and the "
     "basis on which that generation is later judged stale"},
    {"sem_units", ATLAS_RETAIN_DERIVED, false,
     "one translation unit compiled under one configuration, with its outcome and its input "
     "digest; rebuilt by indexing, and the record that says which units failed and why"},
    {"sem_symbols", ATLAS_RETAIN_DERIVED, false,
     "compiler-derived symbol occurrences; rebuilt by indexing from the repository and the "
     "compilation database, and referenced by nothing authoritative"},
    {"sem_edges", ATLAS_RETAIN_DERIVED, false,
     "compiler-derived relations between symbols, each carrying its evidence class; rebuilt by "
     "indexing, and referenced by nothing authoritative"},
    {"sem_includes", ATLAS_RETAIN_DERIVED, false,
     "the include graph as the preprocessor resolved it; rebuilt by indexing"},
    /* A9.2.3. CANONICAL rather than DERIVED, and the distinction is the whole
     * table: every other `sem_` table is rebuilt by indexing, and this one is
     * what says indexing may run at all. A repository does not record that an
     * operator authorised a compiler to be run over it, so nothing could rebuild
     * this row — and an aged-out row would silently stop a repository being
     * maintained, which looks exactly like a repository nobody ever configured. */
    {"sem_repo_config", ATLAS_RETAIN_CANONICAL, false,
     "an operator's semantic build description for one repository — which compilation databases to "
     "read, which roots are tests, and whether this daemon may rebuild automatically; nothing "
     "rebuilds it because the repository never held it, and its absence is what keeps a compiler "
     "from being run on a repository nobody authorised"},

    /* A9.2.4. The candidates one bounded walk found, accepted and rejected.
     *
     * DERIVED, because another walk reproduces it exactly — which is what makes
     * it safe to rewrite whole on every discovery pass. Not prunable by age for
     * A5's reason about derived tables: a half-aged candidate list is not a
     * smaller search, it is a *wrong* one, and nothing in the surviving rows
     * would record that some are missing. That matters more here than for most
     * derived data, because this table is what a negative conclusion's coverage
     * rests on: rows silently gone would make a repository look like one whose
     * build inputs were fewer than they are. */
    {"sem_build_inputs", ATLAS_RETAIN_DERIVED, false,
     "the compilation databases one bounded discovery walk found for a repository, accepted and "
     "rejected, with the reason for each; reproduced by walking again, and never aged out because "
     "a partly-deleted candidate list is a search nobody performed"},

    /* A9.2.5, and the same argument one step further: this table records where a
     * walk could *not* look. Ageing rows out of it would silently restore the
     * invisibility it exists to remove — a repository would look like one whose
     * search met fewer obstacles than it did, which is the direction that lets a
     * negative conclusion be believed when it should not be. */
    {"sem_discovery_obstacles", ATLAS_RETAIN_DERIVED, false,
     "the places one bounded discovery walk could not account for, each with its exact "
     "repository-relative path and a fixed reason; reproduced by walking again, and never aged "
     "out because a partly-deleted list of what was missed reads as a search that missed less"},

    /* --- full-text indexes -------------------------------------------------- */
    {"files_fts", ATLAS_RETAIN_DERIVED, false, "FTS5 index over files; rebuilt from files"},
    {"commits_fts", ATLAS_RETAIN_DERIVED, false, "FTS5 index over commits; rebuilt from commits"},
    {"decisions_fts", ATLAS_RETAIN_DERIVED, false,
     "FTS5 index over decision prose; rebuilt from decision_search"},

    /* --- A9: remote credentials and the gateway audit trail ------------------
     *
     * `api_keys` is CANONICAL for the reason `repositories` is: a credential
     * goes away when an operator revokes it, an explicit act aimed at one key,
     * and never by age. A key aged out of existence would not stop working
     * safely — it would stop working *silently*, and the operator holding it
     * would have no row to read to find out why.
     *
     * `gw_audit` is the **second prunable table in Atlas**, and A5 says
     * widening that needs an argument that survives being read back. Here it
     * is, in the shape A5 asks for.
     *
     *   *What holds a rowid into it?* Nothing. No column in any table
     *   references `gw_audit.id`, and the id is AUTOINCREMENT, so a deleted row
     *   can never hand its id to a later one — which is exactly why `scans` is
     *   not prunable and this is.
     *
     *   *What cursor points at it?* Only a reader's own paging cursor within a
     *   single listing. AUTOINCREMENT means a cursor that outlives a prune
     *   points past the deleted rows rather than at different ones, so the
     *   worst outcome is a page that is shorter than expected, never one that
     *   describes other events.
     *
     *   *What is lost that cannot be rebuilt?* The record of one request. That
     *   is a real loss and the reason this is `--older-than` rather than a
     *   ceiling: an operational access log is unbounded by construction — it
     *   grows with traffic rather than with the repository — and A9.6 requires
     *   it to be bounded or to have a stated retention strategy. An index that
     *   fills a disk with audit rows stops answering anything at all, which is
     *   a worse outcome for the record than aged-out access history. Nothing
     *   Atlas *decides* is stored here: an approval, a revision, a job or a
     *   reason is in a canonical table that is not prunable, and this holds who
     *   asked and when.
     *
     * The strategy is A5's unchanged: no background deleter, no prune on a
     * timer, at startup or on low disk. Rows go away when an operator runs
     * `atlas maintenance prune --apply`, and at no other moment. */
    {"api_keys", ATLAS_RETAIN_CANONICAL, false,
     "a remote credential is revoked by an explicit operator act aimed at one key, never by age; "
     "an aged-out key would stop authenticating with no row left to say that it had been removed, "
     "and the verifier it holds is not recoverable from anything else"},
    {"gw_audit", ATLAS_RETAIN_OPERATIONAL, true,
     "the bounded operational record of remote requests: who asked, on which interface, whether it "
     "was allowed and whether it worked; nothing holds its rowids, its id is AUTOINCREMENT so a "
     "deletion can never re-point a reader's cursor, and every fact Atlas decides — an approval, a "
     "revision, a job, a reason — lives in a canonical table that is not prunable"},

    /* --- A9.2: claims, attestations, evidence and verification -------------
     *
     * Every one of these is CANONICAL and not one is prunable, and the argument
     * is the same for all ten: **none of it is rebuildable**. The repository
     * remembers its own bytes, so a code index can be thrown away and rebuilt;
     * it does not remember that anybody spoke about them. An attestation is a
     * statement an actor made at a moment, and once deleted there is nothing
     * anywhere from which to recover it.
     *
     * There is a second reason that applies specifically to the age-based
     * pruning A5 makes available, and it is the more dangerous one. These
     * tables are the input to a *count*: how many independent evidence groups
     * support this claim, how many samples does this source have. A half-aged
     * evidence table is not a smaller evidence table — it is a wrong one, and
     * nothing in it records that rows are missing, so every confidence score
     * computed afterwards would be confidently wrong in the direction of less
     * support. That is the failure this phase exists to prevent, so the tables
     * it stores its own answers in must not be able to cause it.
     *
     * Staleness is handled by weighting, never by deletion. §47: old evidence
     * keeps its historical value and loses its current force. */
    {"verify_actors", ATLAS_RETAIN_CANONICAL, false,
     "who spoke; attestations and evidence reference these rows and an actor that vanished would "
     "make every statement it made unattributable, which is the one property a provenance system "
     "cannot lose"},
    {"verify_claims", ATLAS_RETAIN_CANONICAL, false,
     "the propositions Atlas checks knowledge against; nothing else holds them and a repository "
     "cannot regenerate a question somebody chose to ask"},
    {"verify_evidence", ATLAS_RETAIN_CANONICAL, false,
     "where each fact came from; an aged-out row would silently reduce the independent-group count "
     "for every claim resting on it, and a confidence score computed from a partially deleted "
     "evidence table is wrong with nothing recording that it is"},
    {"verify_evidence_deps", ATLAS_RETAIN_CANONICAL, false,
     "the declared derivation edges that make correlated evidence detectable; deleting one does "
     "not lose a fact, it *creates* a false independence — the exact inflation the aggregation "
     "exists to prevent"},
    {"verify_attestations", ATLAS_RETAIN_CANONICAL, false,
     "one actor's verdict at one moment, never overwritten and never removed; a source that "
     "reversed itself is a fact a reliability system must be able to see"},
    {"verify_attestation_evidence", ATLAS_RETAIN_CANONICAL, false,
     "which evidence each attestation rests on; without it an attestation's independence cannot be "
     "computed at all, and unknown independence is treated as none"},
    {"verify_results", ATLAS_RETAIN_CANONICAL, false,
     "the append-only record of what an aggregation concluded, under which algorithm and taxonomy "
     "version; a machine lifecycle transition must be reconstructable from it years later, which "
     "is the same argument A6 makes for decision_validations"},
    {"verify_outcomes", ATLAS_RETAIN_CANONICAL, false,
     "the resolved ground truth reliability is learned from; deleting outcomes would move every "
     "source's measured accuracy without any statement recording that the sample changed"},
    {"verify_reliability", ATLAS_RETAIN_CANONICAL, false,
     "per actor and domain accuracy, derived from verify_outcomes but not cheaply: it is the "
     "accumulated result of every resolution, and a rebuild would need the whole outcome history "
     "that is itself canonical"},
    {"verify_lifecycle_audit", ATLAS_RETAIN_CANONICAL, false,
     "why Atlas itself changed a lifecycle state, with the policy hash and evidence snapshot that "
     "justified it; this is the record that makes automatic transitions auditable at all, and an "
     "automatic transition whose justification had been pruned is indistinguishable from one that "
     "never had a justification"},
};

#define RETENTION_COUNT (sizeof RETENTION / sizeof RETENTION[0])

void atlas_maintenance_report_init(atlas_maintenance_report *r) {
    memset(r, 0, sizeof(*r));
}

void atlas_maintenance_report_free(atlas_maintenance_report *r) {
    free(r->tables);
    r->tables = NULL;
    r->table_count = 0;
}

static void cutoff_for(int64_t days, char *out, size_t out_size) {
    atlas_iso8601_before_now(out, out_size, days * 24 * 60 * 60 * 1000);
}

/* The maintenance core, over a handle the caller already owns.
 *
 * The public entry point below opens its own handle and takes the
 * data-directory lock, which is right for a local operator invocation. The
 * daemon cannot do that — it already holds the lock — so it calls this with the
 * writer thread's handle instead. One implementation, for the reason
 * `atlas_sem_index_on` is one: two would answer differently the first time
 * somebody fixed a bug in only one of them.
 *
 * Nothing here acquires a lock or opens a database. Whether it may write is
 * entirely `opts->apply` plus whether the handle it was given is writable. */
atlas_status atlas_maintenance_on(atlas_db *db, const atlas_maintenance_opts *opts,
                                  atlas_maintenance_report *out, atlas_err *err) {
    if (opts->older_than_days < 0 || opts->retain_per_repo < 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--older-than and --retain must not be negative");
    }
    int64_t days = opts->older_than_days > 0 ? opts->older_than_days
                                             : ATLAS_MAINTENANCE_DEFAULT_DAYS;
    int64_t retain = opts->retain_per_repo > 0 ? opts->retain_per_repo
                                               : ATLAS_MAINTENANCE_DEFAULT_RETAIN;
    if (days < ATLAS_MAINTENANCE_MIN_DAYS || days > ATLAS_MAINTENANCE_MAX_DAYS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--older-than must be between %d and %d days",
                             ATLAS_MAINTENANCE_MIN_DAYS, ATLAS_MAINTENANCE_MAX_DAYS);
    }
    if (retain < ATLAS_MAINTENANCE_MIN_RETAIN) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "--retain must be at least %d",
                             ATLAS_MAINTENANCE_MIN_RETAIN);
    }

    out->applied = opts->apply;
    out->older_than_days = days;
    out->retain_per_repo = retain;
    cutoff_for(days, out->cutoff, sizeof out->cutoff);

    out->tables = calloc(RETENTION_COUNT, sizeof out->tables[0]);
    if (out->tables == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory planning maintenance");
    }
    out->table_count = RETENTION_COUNT;

    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < RETENTION_COUNT; i++) {
        atlas_maintenance_row *row = &out->tables[i];
        row->table = RETENTION[i].table;
        row->cls = RETENTION[i].cls;
        row->prunable = RETENTION[i].prunable;
        row->reason = RETENTION[i].reason;
        if (RETENTION[i].prunable) {
            out->prunable_tables++;
        } else {
            out->protected_tables++;
        }

        bool exists = false;
        st = atlas_db_maintenance_table_exists(db, row->table, &exists, err);
        if (st != ATLAS_OK || !exists) {
            continue;
        }
        row->counted = true;
        st = atlas_db_maintenance_count(db, row->table, &row->rows_before, err);
        if (st != ATLAS_OK) {
            break;
        }
        row->rows_after = row->rows_before;
        out->total_rows += row->rows_before;
        if (!row->prunable) {
            continue;
        }
        const retention_pruner *p = pruner_for(row->table);
        if (p == NULL) {
            /* Marked prunable with nothing that knows how to prune it. Refused
             * rather than skipped: the plan would otherwise report zero
             * eligible rows for a table an operator was told is prunable, which
             * reads as "there is nothing to remove" rather than as a defect. */
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                               "\"%s\" is marked prunable but has no pruner", row->table);
            break;
        }
        st = p->eligible(db, out->cutoff, retain, &row->rows_eligible, err);
        if (st == ATLAS_OK) {
            out->total_eligible += row->rows_eligible;
        }
    }

    /* Nothing above wrote anything. This is the only block that can, and only
     * with --apply. */
    if (st == ATLAS_OK && opts->apply) {
        for (size_t i = 0; st == ATLAS_OK && i < RETENTION_COUNT; i++) {
            atlas_maintenance_row *row = &out->tables[i];
            if (!row->prunable || !row->counted) {
                continue;
            }
            const retention_pruner *p = pruner_for(row->table);
            if (p == NULL) {
                st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                   "\"%s\" is marked prunable but has no pruner", row->table);
                break;
            }
            bool more = true;
            while (st == ATLAS_OK && more) {
                int64_t removed = 0;
                st = p->prune(db, out->cutoff, retain, ATLAS_DB_BATCH_MAX, &removed, &more, err);
                if (st == ATLAS_OK) {
                    row->rows_removed += removed;
                    out->total_removed += removed;
                    if (removed == 0) {
                        break;
                    }
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_db_maintenance_count(db, row->table, &row->rows_after, err);
            }
        }
    }

    return st;
}

/* The cutoff, as the same ISO-8601 UTC text every timestamp column stores.
 * String comparison is correct for this format because it sorts
 * lexicographically, which is the same property `ai_sessions` idle expiry
 * already relies on. */

atlas_status atlas_service_maintenance(const char *data_dir_override,
                                       const atlas_maintenance_opts *opts,
                                       atlas_maintenance_report *out, atlas_err *err) {
    /* Zero means "not given" and takes the documented default. A negative is a
     * refusal, not a default: silently turning `--older-than -1` into 90 days
     * would delete far more than was asked for, and the caller would never see
     * that their number was discarded. */
    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_lock *lk = NULL;
    atlas_db *db = NULL;

    atlas_status st = atlas_datadir_resolve(data_dir_override, &data_dir, NULL, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(atlas_buf_cstr(&data_dir), &db_path, err);
    }

    /* An apply is a write, and Atlas has exactly one writer. Taking the lock is
     * what makes "the daemon must be stopped" a fact rather than an
     * instruction. A plan takes nothing and opens read-only. */
    if (st == ATLAS_OK && opts->apply) {
        st = atlas_lock_acquire(atlas_buf_cstr(&data_dir), ATLAS_LOCK_ROLE_ONESHOT, &lk, err);
    }
    if (st == ATLAS_OK) {
        st = opts->apply ? atlas_db_open(atlas_buf_cstr(&db_path), &db, err)
                         : atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, err);
    }

    if (st == ATLAS_OK) {
        st = atlas_maintenance_on(db, opts, out, err);
    }

    atlas_db_close(db);
    atlas_lock_release(lk);
    atlas_buf_free(&db_path);
    atlas_buf_free(&data_dir);
    return st;
}

/* Exposed for the test that compares this policy against the live schema. It
 * returns the compiled-in list rather than a copy: the strings are literals and
 * the array is const. */
bool atlas_maintenance_policy_lookup(const char *table, const char **table_out,
                                     atlas_retention_class *cls_out, bool *prunable_out,
                                     const char **reason_out) {
    for (size_t i = 0; i < RETENTION_COUNT; i++) {
        if (strcmp(RETENTION[i].table, table) == 0) {
            *table_out = RETENTION[i].table;
            *cls_out = RETENTION[i].cls;
            *prunable_out = RETENTION[i].prunable;
            *reason_out = RETENTION[i].reason;
            return true;
        }
    }
    return false;
}

size_t atlas_maintenance_policy(const char *const **names_out) {
    static const char *names[RETENTION_COUNT];
    for (size_t i = 0; i < RETENTION_COUNT; i++) {
        names[i] = RETENTION[i].table;
    }
    *names_out = names;
    return RETENTION_COUNT;
}

size_t atlas_maintenance_pruners(const char *const **names_out) {
    static const char *names[sizeof PRUNERS / sizeof PRUNERS[0]];
    for (size_t i = 0; i < sizeof PRUNERS / sizeof PRUNERS[0]; i++) {
        names[i] = PRUNERS[i].table;
    }
    *names_out = names;
    return sizeof PRUNERS / sizeof PRUNERS[0];
}

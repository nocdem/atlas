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

/* One row per table. `prunable` is true exactly once. */
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
    {"decision_challenges", ATLAS_RETAIN_CANONICAL, false,
     "a consumed challenge is part of an approval record and the event points at it; expired "
     "unconsumed ones are already removed at the point of use, which is the only DELETE the "
     "decision tables have"},
    {"decision_search", ATLAS_RETAIN_DERIVED, false,
     "the searchable projection of decision prose, rebuilt from the canonical rows"},

    /* --- full-text indexes -------------------------------------------------- */
    {"files_fts", ATLAS_RETAIN_DERIVED, false, "FTS5 index over files; rebuilt from files"},
    {"commits_fts", ATLAS_RETAIN_DERIVED, false, "FTS5 index over commits; rebuilt from commits"},
    {"decisions_fts", ATLAS_RETAIN_DERIVED, false,
     "FTS5 index over decision prose; rebuilt from decision_search"},
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

/* The cutoff, as the same ISO-8601 UTC text every timestamp column stores.
 * String comparison is correct for this format because it sorts
 * lexicographically, which is the same property `ai_sessions` idle expiry
 * already relies on. */
static void cutoff_for(int64_t days, char *out, size_t out_size) {
    atlas_iso8601_before_now(out, out_size, days * 24 * 60 * 60 * 1000);
}

atlas_status atlas_service_maintenance(const char *data_dir_override,
                                       const atlas_maintenance_opts *opts,
                                       atlas_maintenance_report *out, atlas_err *err) {
    /* Zero means "not given" and takes the documented default. A negative is a
     * refusal, not a default: silently turning `--older-than -1` into 90 days
     * would delete far more than was asked for, and the caller would never see
     * that their number was discarded. */
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
        st = atlas_db_maintenance_events_eligible(db, out->cutoff, retain, &row->rows_eligible,
                                                  err);
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
            bool more = true;
            while (st == ATLAS_OK && more) {
                int64_t removed = 0;
                st = atlas_db_maintenance_events_prune(db, out->cutoff, retain,
                                                       ATLAS_DB_BATCH_MAX, &removed, &more, err);
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

    atlas_db_close(db);
    atlas_lock_release(lk);
    atlas_buf_free(&db_path);
    atlas_buf_free(&data_dir);
    return st;
}

/* Exposed for the test that compares this policy against the live schema. It
 * returns the compiled-in list rather than a copy: the strings are literals and
 * the array is const. */
size_t atlas_maintenance_policy(const char *const **names_out) {
    static const char *names[RETENTION_COUNT];
    for (size_t i = 0; i < RETENTION_COUNT; i++) {
        names[i] = RETENTION[i].table;
    }
    *names_out = names;
    return RETENTION_COUNT;
}

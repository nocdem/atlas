/* Atlas - retention classification and local maintenance.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every table in the Atlas index is classified, and the classification is the
 * product here — not the deletion. Atlas is an engineering-memory system: the
 * default answer to "may this be removed" is no, and a table becomes prunable
 * only when there is an argument for it, written down, that survives being
 * read back.
 *
 * There is no background deleter. Nothing prunes on a timer, at startup, on
 * low disk, or as a side effect of any other command. Rows go away when an
 * operator runs `atlas maintenance prune --apply` at a terminal, and at no
 * other moment. (`repo_events` is separately capped by the daemon's own
 * long-standing per-repository ceiling; that is A1 behaviour this file
 * neither adds nor changes.)
 *
 * There is no RPC method, no MCP tool and no hook that can reach any of this.
 * A model cannot plan a prune, cannot apply one, and cannot ask the daemon to.
 *
 * As of A5 exactly one table is prunable: `repo_events`, which is a bounded
 * stream of observations that already had a documented retention ceiling. Every
 * other table is canonical, is durable engineering memory, is derived state
 * that a pass rebuilds as a whole rather than by age, or holds a cursor. The
 * reasons are in RETENTION[] in src/core/service_maintenance.c, one per table,
 * and `atlas maintenance plan` prints them.
 */
#ifndef ATLAS_MAINTENANCE_H
#define ATLAS_MAINTENANCE_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"

/* Bounds on what an operator may ask for. Both are checked, and a value outside
 * them is a usage error rather than something clamped: silently turning
 * `--older-than 0` into a default would delete far more than was asked for. */
#define ATLAS_MAINTENANCE_MIN_DAYS 1
#define ATLAS_MAINTENANCE_MAX_DAYS 36500 /* a century; past this, a typo */
#define ATLAS_MAINTENANCE_MIN_RETAIN 100
#define ATLAS_MAINTENANCE_DEFAULT_RETAIN 1000
#define ATLAS_MAINTENANCE_DEFAULT_DAYS 90

typedef enum atlas_retention_class {
    /* The record Atlas exists to keep. Never removed by any automatic rule. */
    ATLAS_RETAIN_CANONICAL = 0,
    /* Durable engineering memory: retained by default, and removable only by
     * an explicit act aimed at the thing itself, never by age. */
    ATLAS_RETAIN_MEMORY,
    /* Derived from the repository and rebuilt whole by a pass. Not pruned by
     * age, because a half-aged derived table is not a smaller index — it is a
     * wrong one, and nothing in it records that rows are missing. */
    ATLAS_RETAIN_DERIVED,
    /* Bounded operational history or a cursor. */
    ATLAS_RETAIN_OPERATIONAL
} atlas_retention_class;

const char *atlas_retention_class_name(atlas_retention_class c);

typedef struct atlas_maintenance_row {
    const char *table;
    atlas_retention_class cls;
    bool prunable;
    /* Fixed Atlas-owned text saying why. Never derived from anything in a
     * repository or from any model. */
    const char *reason;
    int64_t rows_before;
    int64_t rows_eligible;
    int64_t rows_removed;
    int64_t rows_after;
    bool counted; /* false when the table is absent from this schema */
} atlas_maintenance_row;

typedef struct atlas_maintenance_report {
    bool applied;
    int64_t older_than_days;
    int64_t retain_per_repo;
    char cutoff[32]; /* the ISO-8601 instant rows were compared against */
    atlas_maintenance_row *tables;
    size_t table_count;
    int64_t total_rows;
    int64_t total_eligible;
    int64_t total_removed;
    size_t prunable_tables;
    size_t protected_tables;
} atlas_maintenance_report;

void atlas_maintenance_report_init(atlas_maintenance_report *r);
void atlas_maintenance_report_free(atlas_maintenance_report *r);

typedef struct atlas_maintenance_opts {
    int64_t older_than_days;
    int64_t retain_per_repo;
    bool apply; /* --apply; without it nothing is written and nothing is locked */
} atlas_maintenance_opts;

/* Reports what a prune would remove, and removes it only when `opts->apply`.
 *
 * Without `apply` this opens the index read-only, takes no lock, writes no
 * byte, and can be run while the daemon is serving. With `apply` it acquires
 * the data-directory writer lock exclusively, which means the daemon must be
 * stopped — Atlas has exactly one writer, and a maintenance pass is a writer. */
/* The maintenance core, over a handle the caller already owns and with no lock
 * of its own. The local entry point below opens a handle and takes the
 * data-directory lock; the daemon calls this with the writer thread's handle,
 * because it already holds that lock. One implementation, so the two cannot
 * drift. */
atlas_status atlas_maintenance_on(atlas_db *db, const atlas_maintenance_opts *opts,
                                  atlas_maintenance_report *out, atlas_err *err);

atlas_status atlas_service_maintenance(const char *data_dir_override,
                                       const atlas_maintenance_opts *opts,
                                       atlas_maintenance_report *out, atlas_err *err);

/* Looks one table up in the compiled-in retention policy.
 *
 * Used by the socket client so that the classification, the prunable flag and
 * the written reason come from *this* binary rather than from the wire. They
 * are Atlas-owned constants and a report is not the place to start trusting a
 * peer for them; only the counts cross the socket. A table this binary does not
 * know is reported as unknown rather than invented. */
/* The daemon-served form. Same report, same renderers; only the transport
 * differs — which is what keeps a local answer and a socket answer from
 * drifting apart. Offered only to the operator uid the root-owned policy
 * names. */
atlas_status atlas_service_maintenance_remote(const atlas_maintenance_opts *opts,
                                              atlas_maintenance_report *out, atlas_err *err);

bool atlas_maintenance_policy_lookup(const char *table, const char **table_out,
                                     atlas_retention_class *cls_out, bool *prunable_out,
                                     const char **reason_out);

/* The compiled-in policy's table names, for the test that compares it against
 * the live schema in both directions. A table with no entry, or an entry with
 * no table, is a failure rather than something to notice later. */
size_t atlas_maintenance_policy(const char *const **names_out);

/* The tables Atlas actually knows how to prune, as opposed to the ones the
 * policy classifies as prunable.
 *
 * The two must agree, and the test that checks it runs in both directions: a
 * prunable table with no pruner reports zero eligible rows for ever, which
 * reads as "nothing to remove" rather than as a defect, and a pruner for a
 * protected table is a delete statement aimed at something the policy says is
 * never removed by age. */
size_t atlas_maintenance_pruners(const char *const **names_out);

/* --- internals shared with src/db ---------------------------------------- */

atlas_status atlas_db_maintenance_table_exists(atlas_db *db, const char *table, bool *out,
                                               atlas_err *err);
atlas_status atlas_db_maintenance_count(atlas_db *db, const char *table, int64_t *out,
                                        atlas_err *err);
/* Rows in `repo_events` that are both older than `cutoff` and outside the newest
 * `retain` for their repository. Both conditions, always: age alone would empty
 * a quiet repository's whole stream, and the retain floor alone would delete
 * fresh observations from a busy one. */
atlas_status atlas_db_maintenance_events_eligible(atlas_db *db, const char *cutoff, int64_t retain,
                                                  int64_t *out, atlas_err *err);
/* Deletes at most `batch` of those rows in one transaction, reporting how many
 * went and whether more remain. Bounded per call because a delete large enough
 * to matter is large enough to hold a write transaction open too long. */
atlas_status atlas_db_maintenance_events_prune(atlas_db *db, const char *cutoff, int64_t retain,
                                               int64_t batch, int64_t *removed_out, bool *more_out,
                                               atlas_err *err);

/* A9. The same pair for `gw_audit`, the second prunable table.
 *
 * The signature is identical on purpose: `RETENTION[]` now carries a pair of
 * function pointers per prunable table rather than the loop calling the
 * `repo_events` functions by name, so adding a third prunable table is a row
 * with two more functions and not an edit to the loop. A loop that knows which
 * table it is pruning is a loop that prunes the wrong one the first time a
 * second table appears — which is exactly what would have happened here.
 *
 * `retain` is a global floor rather than a per-repository one, because an audit
 * row belongs to a credential and an interface, not to a repository. The
 * meaning is the same in the way that matters: age alone would empty the trail
 * of a quiet installation, so the newest `retain` rows survive any cutoff. */
atlas_status atlas_db_maintenance_audit_eligible(atlas_db *db, const char *cutoff, int64_t retain,
                                                 int64_t *out, atlas_err *err);
atlas_status atlas_db_maintenance_audit_prune(atlas_db *db, const char *cutoff, int64_t retain,
                                              int64_t batch, int64_t *removed_out, bool *more_out,
                                              atlas_err *err);

#endif /* ATLAS_MAINTENANCE_H */

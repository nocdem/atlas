/* Atlas - typed operations over the migration-11 semantic tables.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `sqlite3` types never leave `src/db`, so this is the whole surface the
 * semantic indexer and the semantic queries have. It follows A8's shape: one
 * write point per thing that can change, compare-and-swap where two writers
 * could race, and reads that are bounded by their own arguments rather than by
 * the caller remembering to bound them.
 *
 * The rule this header exists to enforce: **publication is one statement.** A
 * generation's rows are written while the previous generation is still being
 * read, and `atlas_db_sem_publish` is the single UPDATE that makes the new one
 * current. There is no path that makes a partially written generation visible,
 * because becoming visible is a separate act from being written.
 */
#ifndef ATLAS_SEM_OPS_H
#define ATLAS_SEM_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/sem.h"

/* --- generations ------------------------------------------------------------ */

/* Opens a RUNNING generation and returns its id. Nothing points at it yet. */
atlas_status atlas_db_sem_generation_begin(atlas_db *db, const atlas_sem_generation *g,
                                           int64_t *id_out, atlas_err *err);

/* Marks a generation COMPLETE with its counts **and makes it current, in the
 * caller's transaction**. The two are one act: a generation that completed but
 * was not published is invisible, and one that was published but not completed
 * is a lie. */
/* Counts what one generation actually holds: distinct symbol rows, edge rows
 * and include rows.
 *
 * Needed because the pass' own running totals count something else. They are
 * accumulated per translation unit, so a symbol declared in a header is added
 * once for every unit that includes it — on a real repository that inflated the
 * reported symbol count by more than twenty times against the rows actually
 * stored, and it made a full pass and an incremental one disagree about
 * identical content. A number an operator compares between generations has to
 * mean the same thing in both.
 *
 * Read once at publication, over generation-scoped indexes. */
atlas_status atlas_db_sem_generation_counts(atlas_db *db, int64_t generation_id,
                                            int64_t *symbols_out, int64_t *edges_out,
                                            int64_t *includes_out, atlas_err *err);

atlas_status atlas_db_sem_publish(atlas_db *db, int64_t generation_id,
                                  const atlas_sem_generation *counts, atlas_err *err);

/* Marks a generation FAILED with a fixed Atlas reason. The previous current
 * generation is untouched — that is what "preserve the last valid generation"
 * means in practice. */
atlas_status atlas_db_sem_fail(atlas_db *db, int64_t generation_id, const char *why,
                               atlas_err *err);

/* The published generation for a repository, if any. `found` is false when the
 * repository has never been indexed, which is ABSENT and not STALE. */
atlas_status atlas_db_sem_current(atlas_db *db, int64_t repo_id, atlas_sem_generation *out,
                                  bool *found, atlas_err *err);

/* The most recent generation of any status, used to report a failed or
 * in-progress attempt beside the one being served. */
atlas_status atlas_db_sem_latest(atlas_db *db, int64_t repo_id, atlas_sem_generation *out,
                                 bool *found, atlas_err *err);

/* Deletes a generation and every row scoped to it.
 *
 * The only deletion in this file. It removes derived rows and nothing else —
 * A3's rule about `atlas_db_code_clear_repo`, and the reason it is safe here is
 * that no authoritative record references a semantic row. Refuses to delete the
 * currently published generation: replacing an index is publishing a new one,
 * never removing the old one first. */
atlas_status atlas_db_sem_generation_delete(atlas_db *db, int64_t generation_id, atlas_err *err);

/* Drops every generation for a repository except the newest `keep` complete
 * ones and the current one. Called at the end of a successful index so the
 * table does not grow without bound; it is not a retention policy and runs at
 * no other moment. */
atlas_status atlas_db_sem_prune_generations(atlas_db *db, int64_t repo_id, int64_t keep,
                                            atlas_err *err);

/* Clears the soft `repo_id` reference. Called inside `atlas_db_repo_remove`'s
 * transaction, because `repositories.id` is a reused rowid. */
atlas_status atlas_db_sem_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err);

/* --- A9.2.3: the durable build description ----------------------------------
 *
 * Reads leave `present` false when no operator has configured this repository,
 * which is the default and means this daemon never runs a compiler for it. */
atlas_status atlas_db_sem_config_get(atlas_db *db, int64_t repo_id, atlas_sem_config *out,
                                     atlas_err *err);

/* Writes the operator's build description. The retry-governor columns are not
 * touched here: configuring a repository is not a claim that a previous failure
 * is resolved, and a write that cleared the record would let an operator turn a
 * deterministic failure back into a spin by re-running one command. Changing the
 * description does move the source identity, which is what legitimately makes
 * the next attempt eligible. */
atlas_status atlas_db_sem_config_set(atlas_db *db, const atlas_sem_config *c, atlas_err *err);

/* Records the outcome of an *automatic* attempt against the source identity it
 * was made at. Success clears the record; a failure stores the identity, so a
 * further attempt is allowed only once the identity has moved. */
atlas_status atlas_db_sem_config_record_attempt(atlas_db *db, int64_t repo_id,
                                                const char *source_identity, bool ok,
                                                const char *why, atlas_err *err);

/* Every repository with a build description, for the daemon's scheduler.
 * Bounded by `max`; a truncated list is reported rather than silently shortened,
 * because a repository dropped from a scheduling sweep is one that never
 * rebuilds and nothing would say so. */
atlas_status atlas_db_sem_config_repos(atlas_db *db, int64_t *out, size_t max, size_t *count_out,
                                       bool *truncated_out, atlas_err *err);

/* Clears the soft `repo_id` reference for the same reason the generations do. */
atlas_status atlas_db_sem_config_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err);

/* --- A9.2.3: the coverage manifest ------------------------------------------
 *
 * Counts the candidate source files the *file index* holds for this repository
 * and how many of them this generation parsed as a translation unit. The
 * denominator is the file index's enumeration of the tree, never the compilation
 * database's own contents — which is the whole point: a compilation database
 * that names two of three sources reports `2/2` units and `2/3` scope, and only
 * the second number can refuse an absence.
 *
 * Read once, at publication, over generation-scoped indexes — the shape
 * `atlas_db_sem_generation_counts` uses and for its reason. */
atlas_status atlas_db_sem_scope_counts(atlas_db *db, int64_t repo_id, int64_t generation_id,
                                       int64_t *candidates_out, int64_t *covered_out,
                                       atlas_err *err);

/* Classifies this generation's units against the operator's declared test roots.
 * With no roots declared both counts are zero and `known_out` is false, which is
 * "Atlas does not know which sources are tests" and is a different statement
 * from "there are no test units". */
atlas_status atlas_db_sem_scope_test_split(atlas_db *db, int64_t generation_id,
                                           const char *packed_test_roots, int64_t *test_out,
                                           int64_t *production_out, bool *known_out,
                                           atlas_err *err);

/* A digest of every live C source and header this repository holds, by path and
 * content hash, in path order — domain-separated and length-prefixed.
 *
 * The working-tree half of `atlas_sem_source_identity`, and it lives here
 * because sqlite3 types do not leave `src/db`. A file whose content hash the
 * index does not hold contributes a fixed marker rather than being skipped:
 * skipping it would make a file Atlas could not read compare equal to one that
 * was never there, and those are different states. */
atlas_status atlas_db_sem_source_content_digest(atlas_db *db, int64_t repo_id, char out[65],
                                                atlas_err *err);

/* Records the source identity a generation was built from, inside the
 * publishing transaction with the rest of the manifest. */
atlas_status atlas_db_sem_source_identity_set(atlas_db *db, int64_t generation_id,
                                              const char *identity, atlas_err *err);

/* Writes the manifest into the generation row. Called inside the publishing
 * transaction, so the manifest and the generation become visible together: a
 * reader must never see a published generation whose coverage is still zero and
 * read it as "nothing was covered". */
atlas_status atlas_db_sem_scope_set(atlas_db *db, int64_t generation_id,
                                    const atlas_sem_generation *m, atlas_err *err);

/* --- writing a generation's contents ---------------------------------------- */

atlas_status atlas_db_sem_compdb_add(atlas_db *db, int64_t generation_id, const char *path_text,
                                     const char *digest, int64_t entries, int64_t dropped,
                                     int64_t *id_out, atlas_err *err);

typedef struct atlas_sem_unit_row {
    int64_t generation_id;
    const char *source_text;
    int64_t compdb_id;
    const char *config_digest;
    const char *input_digest;
    atlas_sem_tu_status status;
    const char *why; /* a fixed Atlas string, or NULL */
    int64_t diagnostics_errors;
    int64_t symbols;
    int64_t edges;
    int64_t duration_ms;
    bool reused;
} atlas_sem_unit_row;

atlas_status atlas_db_sem_unit_add(atlas_db *db, const atlas_sem_unit_row *row, int64_t *id_out,
                                   atlas_err *err);

/* Writes one fact. Idempotent by the table's unique key, so a unit replayed
 * after a crash produces the same rows rather than duplicates. */
atlas_status atlas_db_sem_symbol_add(atlas_db *db, int64_t generation_id,
                                     const atlas_sem_fact *fact, atlas_err *err);
atlas_status atlas_db_sem_edge_add(atlas_db *db, int64_t generation_id, int64_t unit_id,
                                   const atlas_sem_fact *fact, atlas_err *err);
atlas_status atlas_db_sem_include_add(atlas_db *db, int64_t generation_id,
                                      const atlas_sem_fact *fact, atlas_err *err);

/* Copies one translation unit's facts forward from another generation.
 *
 * This is what makes incremental indexing incremental: a unit whose input
 * digest is unchanged is not reparsed, its rows are carried over. Copying is
 * cheap relative to a compiler front end and, unlike sharing rows between
 * generations, keeps a generation a self-contained description of one state —
 * so dropping an old generation can never damage a newer one. */
atlas_status atlas_db_sem_copy_unit(atlas_db *db, int64_t from_generation, int64_t to_generation,
                                    const char *source_text, const char *config_digest,
                                    int64_t *symbols_out, int64_t *edges_out, atlas_err *err);

/* Attaches candidate targets to the indirect call sites of a generation.
 *
 * Runs once, after every unit is applied, because it is the only step that
 * needs the whole repository at once: a call site in one translation unit may
 * be answered by a function whose address was taken in another. A MAY_CALL edge
 * with prototype P gains one CANDIDATE edge per function that had its address
 * taken with prototype P.
 *
 * **The result is never PROVEN and cannot be.** Address-taken-with-a-matching-
 * prototype is compiler-derived evidence that a function *could* be the target;
 * it is not a proof that it is, and C offers no such proof without whole-program
 * analysis this season excludes. `candidate_total` records how many candidates
 * existed even when more existed than were recorded. */
atlas_status atlas_db_sem_attach_candidates(atlas_db *db, int64_t generation_id, int64_t max_per_site,
                                            int64_t *attached_out, atlas_err *err);

/* --- bounded reads ----------------------------------------------------------- */

/* Every read below is scoped to one generation, which is scoped to one
 * repository. There is no query in this file that can reach a row belonging to
 * another repository, because a generation id is the only way in. */

typedef struct atlas_sem_symbol_row {
    int64_t id;
    const char *usr;
    const char *name;
    const char *kind;
    const char *linkage;
    const char *type_text;
    const char *file_text;
    int64_t line;
    int64_t col;
    int64_t end_line;
    bool is_definition;
    bool external;
    const char *evidence;
} atlas_sem_symbol_row;

typedef atlas_status (*atlas_sem_symbol_cb)(const atlas_sem_symbol_row *row, void *ud,
                                            atlas_err *err);

/* Symbols matching an exact name, or — when `name` is NULL — every symbol with
 * the given USR. Bounded by `limit`; `truncated_out` says a bound was reached
 * rather than that nothing more existed. */
atlas_status atlas_db_sem_symbols_by_name(atlas_db *db, int64_t generation_id, const char *name,
                                          const char *usr, const char *kind, int64_t limit,
                                          atlas_sem_symbol_cb cb, void *ud, int64_t *total_out,
                                          bool *truncated_out, atlas_err *err);

/* Symbols declared or defined in one repository-relative file. */
atlas_status atlas_db_sem_symbols_in_file(atlas_db *db, int64_t generation_id,
                                          const char *file_text, int64_t limit,
                                          atlas_sem_symbol_cb cb, void *ud, int64_t *total_out,
                                          bool *truncated_out, atlas_err *err);

typedef struct atlas_sem_edge_row {
    int64_t id;
    const char *kind;
    const char *src_usr;
    const char *dst_usr;
    /* The peer's display name and location, resolved by the query so a caller
     * does not have to issue one lookup per edge. Empty when the peer is an
     * entity Atlas does not describe. */
    const char *peer_name;
    const char *peer_file;
    int64_t peer_line;
    const char *evidence;
    const char *file_text;
    int64_t line;
    int64_t col;
    const char *proto;
    int64_t candidate_total;
} atlas_sem_edge_row;

typedef atlas_status (*atlas_sem_edge_cb)(const atlas_sem_edge_row *row, void *ud, atlas_err *err);

/* Edges into (`inbound`) or out of (`!inbound`) a USR.
 *
 * Inbound answers "who reaches this"; outbound answers "what does this reach".
 * `kind` names one edge kind, or is NULL/"" for no kind restriction.
 * `calls_only` narrows to the call kinds — CALLS and MAY_CALL — which is what
 * `code callers` and `code callees` mean and what a call-graph traversal
 * follows. The two compose: `calls_only` with no `kind` is the call graph, and
 * a `kind` with `calls_only` false is any single relation.
 *
 * Filtering happens in SQL rather than by discarding rows in the caller,
 * because a bound applied after a filter and a bound applied before one report
 * different truncation — and a caller asking for ten callers must not be told
 * the result was truncated because eleven *type* edges were read first.
 *
 * MAY_CALL edges with no destination are returned by the outbound query and
 * never by the inbound one: an edge with no destination cannot be an edge
 * *into* anything, and returning one would let an unknown target appear in a
 * list of callers. */
atlas_status atlas_db_sem_edges_of(atlas_db *db, int64_t generation_id, const char *usr,
                                   bool inbound, const char *kind, bool calls_only, int64_t limit,
                                   atlas_sem_edge_cb cb, void *ud, int64_t *total_out,
                                   bool *truncated_out, atlas_err *err);

/* The files that include `file_text`, directly. */
atlas_status atlas_db_sem_includers_of(atlas_db *db, int64_t generation_id, const char *file_text,
                                       int64_t limit, atlas_sem_edge_cb cb, void *ud,
                                       int64_t *total_out, bool *truncated_out, atlas_err *err);

/* Units whose outcome was not COMPLETE, for `code status` and for diagnosing a
 * partial index. */
typedef struct atlas_sem_unit_report {
    const char *source_text;
    const char *status;
    const char *why;
    int64_t diagnostics_errors;
} atlas_sem_unit_report;

typedef atlas_status (*atlas_sem_unit_cb)(const atlas_sem_unit_report *row, void *ud,
                                          atlas_err *err);

atlas_status atlas_db_sem_failed_units(atlas_db *db, int64_t generation_id, int64_t limit,
                                       atlas_sem_unit_cb cb, void *ud, int64_t *total_out,
                                       bool *truncated_out, atlas_err *err);

/* The stored input digest of one unit in a generation, for the incremental
 * comparison. `found` is false when that generation never held the unit. */
atlas_status atlas_db_sem_unit_digest(atlas_db *db, int64_t generation_id, const char *source_text,
                                      const char *config_digest, atlas_buf *digest_out,
                                      bool *found, atlas_err *err);

/* Every unit in a generation, for the digest-sealing pass at the end of an
 * index. Borrowed pointers, valid for the call only. */
typedef struct atlas_sem_unit_key {
    const char *source_text;
    const char *config_digest;
} atlas_sem_unit_key;

typedef atlas_status (*atlas_sem_unit_key_cb)(const atlas_sem_unit_key *key, void *ud,
                                              atlas_err *err);

atlas_status atlas_db_sem_units_all(atlas_db *db, int64_t generation_id,
                                    atlas_sem_unit_key_cb cb, void *ud, atlas_err *err);

/* Records a unit's input digest.
 *
 * Separate from `atlas_db_sem_unit_add` because it happens at a different time
 * and for a different reason: the digest covers the *transitive include
 * closure*, and a unit's closure is not complete until every unit in the
 * generation has contributed its include rows. Computing it when the unit was
 * parsed measured whatever had been recorded so far — which depends on the
 * order units happened to be processed in, and made the same unchanged bytes
 * produce a different digest on the next run. Incremental indexing then never
 * converged: a fixed set of units was reparsed for ever. */
atlas_status atlas_db_sem_unit_set_digest(atlas_db *db, int64_t generation_id,
                                          const char *source_text, const char *config_digest,
                                          const char *digest, atlas_err *err);

/* Every repository-relative path a unit's facts touched in a generation, which
 * is what an input digest is computed over. */
atlas_status atlas_db_sem_unit_inputs(atlas_db *db, int64_t generation_id, const char *source_text,
                                      atlas_buf *paths_out, atlas_err *err);

#endif /* ATLAS_SEM_OPS_H */

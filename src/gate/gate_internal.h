/* Atlas - the A6 engine's own interface.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not installed and not public. `atlas/gate.h` is what a caller sees; this is
 * what the two files under src/gate say to each other, plus the one struct the
 * service layer fills in on their behalf.
 */
#ifndef ATLAS_GATE_INTERNAL_H
#define ATLAS_GATE_INTERNAL_H

#include "atlas/db.h"
#include "atlas/gate.h"
#include "atlas/safetext.h"

/* Everything about the repository that one gate query resolves once and every
 * assessment in it then shares.
 *
 * The split matters. The facts here are the ones that need something outside
 * the database snapshot to establish — the live Git head, whether the index is
 * current, whether the structural graph is — and gathering them once, before
 * the snapshot is taken, is what keeps the per-decision work purely a read of
 * that snapshot. An assessment therefore cannot be a mixture: the repository
 * state it was measured against is a single value it was handed, not something
 * it looked up while it worked. */
typedef struct atlas_gate_env {
    int64_t repo_id;
    atlas_buf repo_name;
    atlas_buf root_text;
    atlas_buf repo_identity_hash;
    /* Whether Atlas has anything to compare against at all. A link resolved
     * against an index that was never built reports UNKNOWN rather than
     * MISSING, which is the difference between "Atlas has not looked" and "it
     * is not there" — and only the second one is staleness. */
    bool file_index_known;
    bool code_index_known;
    /* The commit Atlas' index describes. Empty when nothing has been scanned. */
    char indexed_commit[ATLAS_OID_HEX_MAX_INCL];
    /* What the caller asked about; equal to the above unless one was named. */
    char requested_commit[ATLAS_OID_HEX_MAX_INCL];
    /* Reasons that apply to every decision in this repository — index lag, a
     * structural graph that is behind, a requested state Atlas has not seen.
     * Copied into each assessment so that one read in isolation still explains
     * itself. */
    atlas_gate_reason reasons[ATLAS_GATE_MAX_REASONS];
    size_t reason_count;
} atlas_gate_env;

void atlas_gate_env_init(atlas_gate_env *e);
void atlas_gate_env_free(atlas_gate_env *e);
void atlas_gate_env_note(atlas_gate_env *e, atlas_gate_reason r);

/* Assesses one document against `env`. Reads only, and only through `db`, so
 * the caller's read transaction is the whole snapshot. */
atlas_status atlas_gate_assess(atlas_db *db, const atlas_gate_env *env, int64_t document_id,
                               const atlas_decision_doc_row *doc, int64_t depth,
                               atlas_gate_assessment *a, atlas_err *err);

/* The digest of what a revision's links resolve to, as they stand in `rev`
 * after every link has been resolved. Exposed for the writer, which binds one
 * into a revalidation capability and compares it again when the capability is
 * spent, and for the tests. */
atlas_status atlas_gate_evidence_digest(const atlas_decision_revision *rev, char *hex_out,
                                        atlas_err *err);

/* Resolves every link of `rev` against `repo_id` and digests the result. The
 * one path both the assessment and the writer take, so a capability can never
 * be bound to a digest computed a slightly different way from the one it is
 * later compared against. */
atlas_status atlas_gate_evidence_digest_for(atlas_db *db, int64_t repo_id,
                                            atlas_decision_revision *rev, char *hex_out,
                                            atlas_err *err);

#endif /* ATLAS_GATE_INTERNAL_H */

/* Atlas - repository scanner.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A scan is read-only with respect to the target repository. It enumerates
 * tracked files from the git index, hashes their working-tree content without
 * ever following a symlink, ingests git history, and records SOURCE and GIT
 * evidence. Repeated scans of an unchanged repository are idempotent.
 */
#ifndef ATLAS_SCAN_H
#define ATLAS_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas/limits.h"

/* Working-tree files larger than this are recorded with size and git oid but
 * without a content hash, so a hostile repository cannot make a scan unbounded
 * in time. Overridable per scan. */
#define ATLAS_SCAN_DEFAULT_MAX_FILE_BYTES (256u * 1024u * 1024u)

typedef struct atlas_scan_opts {
    int64_t max_commits;    /* <= 0 means unlimited */
    bool skip_history;      /* index files only */
    uint64_t max_file_bytes;/* 0 means ATLAS_SCAN_DEFAULT_MAX_FILE_BYTES */
    int timeout_ms;         /* per git invocation; 0 means default */
} atlas_scan_opts;

void atlas_scan_opts_init(atlas_scan_opts *o);

typedef struct atlas_scan_summary {
    int64_t scan_id;
    int64_t files_total;
    int64_t files_added;
    int64_t files_modified;
    int64_t files_deleted;
    int64_t files_unchanged;
    int64_t files_unreadable;
    int64_t files_unsafe;      /* skipped: a path component was a symlink */
    int64_t commits_ingested;  /* newly inserted commits */
    int64_t commits_seen;
    int64_t changes_ingested;
    int64_t evidence_created;
    bool compile_db_found;
    bool compile_db_is_symlink;
    bool history_skipped;
    char head_oid[ATLAS_OID_HEX_MAX_INCL];
    char head_state[16];
    char branch[ATLAS_BRANCH_MAX];
    bool dirty;
} atlas_scan_summary;

/* Runs a full scan of `repo_id`, which must already be registered and whose
 * canonical root must be the root `g` is bound to. */
atlas_status atlas_scan_run(atlas_db *db, atlas_git *g, int64_t repo_id,
                            const atlas_scan_opts *opts, atlas_scan_summary *summary,
                            atlas_err *err);

/* Language detection from a path's extension or basename. Returns NULL when
 * undetected. The returned string is a static literal. */
const char *atlas_detect_language(const void *path_raw, size_t path_len);

#endif /* ATLAS_SCAN_H */

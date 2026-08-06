/* Atlas - application/service layer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every Atlas front end goes through this layer: the CLI's human renderer and
 * its JSON renderer consume identical service results, and a future MCP or
 * skill adapter will do the same. No SQL, git invocation, or output formatting
 * lives above it.
 */
#ifndef ATLAS_SERVICE_H
#define ATLAS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas/limits.h"
#include "atlas/scan.h"

typedef struct atlas_ctx atlas_ctx;

typedef struct atlas_ctx_opts {
    const char *data_dir_override; /* --data-dir; NULL to resolve from env */
    bool read_only;                /* reserved; A0 always opens read-write */
} atlas_ctx_opts;

atlas_status atlas_ctx_open(const atlas_ctx_opts *opts, atlas_ctx **out, atlas_err *err);
void atlas_ctx_close(atlas_ctx *ctx);
const char *atlas_ctx_data_dir(const atlas_ctx *ctx);
const char *atlas_ctx_db_path(const atlas_ctx *ctx);
atlas_datadir_source atlas_ctx_data_dir_source(const atlas_ctx *ctx);
atlas_db *atlas_ctx_db(atlas_ctx *ctx);

/* --- doctor ------------------------------------------------------------- */

typedef struct atlas_doctor_report {
    char atlas_version[32];
    char build_compiler[64];
    char sqlite_runtime[32];
    char sqlite_compiled[32];
    bool git_found;
    atlas_buf git_exe;
    atlas_buf git_version;
    atlas_buf data_dir;
    atlas_buf db_path;
    atlas_datadir_source data_dir_source;
    bool db_ok;
    int schema_version;
    int expected_schema_version;
    bool fts5;
    bool wal;
    bool foreign_keys;
    char journal_mode[16];
    atlas_buf integrity;      /* "ok" or the first problem reported */
    atlas_buf foreign_key_check;
    int64_t repo_count;
    atlas_search_mode search_mode;
    bool ok;                  /* no blocking problem found */
    atlas_buf problems;       /* newline-separated, empty when ok */
} atlas_doctor_report;

void atlas_doctor_report_init(atlas_doctor_report *r);
void atlas_doctor_report_free(atlas_doctor_report *r);
atlas_status atlas_service_doctor(atlas_ctx *ctx, atlas_doctor_report *out, atlas_err *err);

/* --- repositories ------------------------------------------------------- */

atlas_status atlas_service_repo_add(atlas_ctx *ctx, const char *path, const char *name,
                                    atlas_repo_info *out, atlas_err *err);
atlas_status atlas_service_repo_list(atlas_ctx *ctx, atlas_repo_cb cb, void *ud, int64_t *count_out,
                                     atlas_err *err);
/* Removes Atlas metadata only. The target repository is never touched. */
atlas_status atlas_service_repo_remove(atlas_ctx *ctx, const char *name, atlas_repo_info *removed,
                                       atlas_err *err);

atlas_status atlas_service_scan(atlas_ctx *ctx, const char *name, const atlas_scan_opts *opts,
                                atlas_scan_summary *summary, atlas_err *err);

/* --- status ------------------------------------------------------------ */

typedef struct atlas_status_report {
    atlas_repo_info repo;
    atlas_repo_counts counts;
    bool scanned;
    /* live git observation, taken fresh at query time */
    bool git_ok;
    atlas_buf git_error;
    atlas_git_head live_head;
    atlas_git_worktree_state live_state;
    bool head_drift;   /* live head differs from the scanned head */
    bool never_scanned;
    /* Other registrations sharing this repository's common Git directory, that
     * is, sibling worktrees. Their states are independent of this one's. */
    int64_t sibling_worktrees;
} atlas_status_report;

void atlas_status_report_init(atlas_status_report *r);
void atlas_status_report_free(atlas_status_report *r);
atlas_status atlas_service_status(atlas_ctx *ctx, const char *name, atlas_status_report *out,
                                  atlas_err *err);

/* --- search / file / history / diff ------------------------------------ */

atlas_status atlas_service_search(atlas_ctx *ctx, const char *name, const char *query,
                                  int64_t limit, atlas_search_mode *mode_out, atlas_search_cb cb,
                                  void *ud, int64_t *count_out, atlas_err *err);

/* A0 answers "why" with UNKNOWN: it never infers a historical reason. */
#define ATLAS_REASON_UNKNOWN "UNKNOWN"

typedef struct atlas_file_report {
    atlas_file_row row;         /* borrowed: valid until the next service call */
    const char *reason;         /* always ATLAS_REASON_UNKNOWN in A0 */
    const char *reason_evidence;/* always "UNKNOWN" in A0 */
    int64_t change_count;
    const char *last_commit_oid;   /* NULL when history holds nothing */
    const char *last_commit_subject;
    int64_t last_commit_time;
} atlas_file_report;

typedef atlas_status (*atlas_file_report_cb)(const atlas_file_report *rep, void *ud,
                                             atlas_err *err);

atlas_status atlas_service_file(atlas_ctx *ctx, const char *name, const char *path,
                                atlas_file_report_cb cb, void *ud, atlas_err *err);

atlas_status atlas_service_history(atlas_ctx *ctx, const char *name, const char *path,
                                   int64_t limit, atlas_history_cb cb, void *ud,
                                   int64_t *count_out, atlas_err *err);

/* --- diff ---------------------------------------------------------------- */

/* Default ceiling on reported diff entries. A repository with an enormous number
 * of changes must not produce an unbounded response, so the report is truncated
 * and says so rather than silently stopping. */
#define ATLAS_DIFF_DEFAULT_MAX_ENTRIES 2000
/* Untracked files above this size are recorded with their size but not hashed. */
#define ATLAS_DIFF_DEFAULT_MAX_HASH_BYTES (64u * 1024u * 1024u)

typedef struct atlas_diff_opts {
    int64_t max_entries;      /* 0 means the default */
    uint64_t max_hash_bytes;  /* 0 means the default */
    bool skip_untracked;
} atlas_diff_opts;

void atlas_diff_opts_init(atlas_diff_opts *o);

typedef struct atlas_diff_entry {
    atlas_change_scope scope;
    char status;              /* A C D M R T U ? */
    const char *change_type;  /* add|modify|delete|rename|copy|typechange|unmerged|untracked */
    int score;
    bool score_known;
    /* Safe text form; the raw bytes are also available for exactness. */
    const char *path_text;
    const void *path_raw;
    size_t path_raw_len;
    bool path_is_utf8;
    const char *old_path_text; /* NULL unless rename/copy */
    const char *head_oid;      /* "" when the path is not in HEAD */
    const char *index_oid;
    const char *mode_head;
    const char *mode_index;
    const char *mode_worktree;
    /* Line counts, when git could report them. */
    int64_t added;
    int64_t deleted;
    bool counts_known;
    bool binary;
    /* Untracked entries only: Atlas records identity, never contents. */
    bool is_directory;
    bool size_known;
    int64_t size_bytes;
    const char *content_hash;      /* NULL when not hashed */
    const char *content_hash_algo; /* "sha256" when hashed */
    const char *note;              /* why there is no hash, when there is none */
} atlas_diff_entry;

typedef struct atlas_diff_report {
    /* The base the staged comparison is against. */
    char base_head[ATLAS_OID_HEX_MAX_INCL]; /* "" when HEAD is unborn */
    char head_state[16];                    /* born | unborn | detached */
    char branch[ATLAS_BRANCH_MAX];           /* "" when detached */
    bool dirty;
    int64_t staged_count;
    int64_t unstaged_count;
    int64_t untracked_count;
    int64_t unmerged_count;
    int64_t binary_changes;
    int64_t total_entries;
    bool truncated;
    atlas_buf truncated_reason; /* empty unless truncated */
} atlas_diff_report;

void atlas_diff_report_init(atlas_diff_report *r);
void atlas_diff_report_free(atlas_diff_report *r);

typedef atlas_status (*atlas_diff_entry_cb)(const atlas_diff_entry *e, void *ud, atlas_err *err);

/* Reports the complete working-tree change state: staged against HEAD, unstaged
 * against the index, untracked paths, and unmerged paths. Entries are delivered
 * grouped by scope in the order staged, unstaged, unmerged, untracked. */
atlas_status atlas_service_diff(atlas_ctx *ctx, const char *name, const atlas_diff_opts *opts,
                                atlas_diff_entry_cb cb, void *ud, atlas_diff_report *report,
                                atlas_err *err);

#endif /* ATLAS_SERVICE_H */

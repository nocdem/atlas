/* Atlas - SQLite index.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The database is a rebuildable index, never the canonical record of history.
 * Every fact it holds is derived from git or from the working tree and carries
 * provenance back to a git object, a path, or a scan.
 *
 * This header exposes typed operations only; sqlite3 types never leak out of
 * src/db. Row callbacks receive borrowed pointers that are valid only for the
 * duration of the call.
 */
#ifndef ATLAS_DB_H
#define ATLAS_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/limits.h"

#define ATLAS_SCHEMA_VERSION 2

typedef struct atlas_db atlas_db;

typedef struct atlas_db_caps {
    bool fts5;                 /* FTS5 virtual tables can be created */
    bool wal;                  /* journal_mode=WAL took effect */
    bool foreign_keys;         /* PRAGMA foreign_keys is ON */
    char sqlite_version[32];
    char journal_mode[16];
} atlas_db_caps;

/* --- lifecycle ---------------------------------------------------------- */

atlas_status atlas_db_open(const char *path, atlas_db **out, atlas_err *err);
void atlas_db_close(atlas_db *db);
/* Applies all pending numbered migrations, each in its own transaction. */
atlas_status atlas_db_migrate(atlas_db *db, atlas_err *err);
int atlas_db_schema_version(atlas_db *db, atlas_err *err);
const atlas_db_caps *atlas_db_caps_of(const atlas_db *db);
/* Creates the FTS5 shadow tables when FTS5 is available. Idempotent. */
atlas_status atlas_db_ensure_fts(atlas_db *db, atlas_err *err);
atlas_status atlas_db_integrity_check(atlas_db *db, atlas_buf *out, atlas_err *err);
atlas_status atlas_db_foreign_key_check(atlas_db *db, atlas_buf *out, atlas_err *err);

/* --- records ------------------------------------------------------------ */

typedef struct atlas_repo_info {
    int64_t id;
    char name[ATLAS_NAME_MAX + 1];
    atlas_buf root_path;      /* canonical root, raw bytes */
    atlas_buf root_path_text; /* safe text form */
    atlas_buf git_common_dir; /* shared by every worktree of one repository */
    atlas_buf git_dir;        /* this worktree's own git dir; distinguishes worktrees */
    bool is_linked_worktree;  /* a linked worktree rather than the main one */
    char object_format[16];   /* "sha1" | "sha256" | "unknown" */
    char registered_at[ATLAS_TS_MAX];
    char last_scan_at[ATLAS_TS_MAX];          /* "" when never scanned */
    char scanned_head[ATLAS_OID_HEX_MAX + 1]; /* "" when never scanned */
    char current_branch[ATLAS_BRANCH_MAX];    /* "" when detached/unknown */
    char head_state[16];                      /* born | unborn | detached | unknown */
    int64_t last_scan_id;                     /* 0 when never scanned */
    bool dirty;
    int dirty_staged;
    int dirty_unstaged;
    int dirty_untracked;
    int dirty_unmerged;
} atlas_repo_info;

void atlas_repo_info_init(atlas_repo_info *ri);
void atlas_repo_info_free(atlas_repo_info *ri);

/* A tracked path as observed by one scan. */
typedef struct atlas_file_record {
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    bool path_is_utf8;
    const char *file_type;   /* regular | symlink | other | missing */
    const char *language;    /* NULL when undetected */
    const char *git_mode;    /* e.g. "100644"; NULL when unknown */
    const char *git_index_oid;
    const char *content_hash;      /* NULL when not hashed */
    const char *content_hash_algo; /* "sha256" when hashed */
    int64_t size_bytes;
    bool size_known;
    bool is_executable;
    bool is_symlink;
    bool unsafe_path; /* an intermediate path component was a symlink */
    const char *read_error;
} atlas_file_record;

typedef enum atlas_upsert_kind {
    ATLAS_UPSERT_UNCHANGED = 0,
    ATLAS_UPSERT_ADDED,
    ATLAS_UPSERT_MODIFIED
} atlas_upsert_kind;

typedef struct atlas_file_row {
    int64_t id;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    bool path_is_utf8;
    const char *file_type;
    const char *language;
    const char *git_mode;
    const char *git_index_oid;
    const char *content_hash;
    const char *content_hash_algo;
    int64_t size_bytes;
    bool size_known;
    bool is_executable;
    bool is_symlink;
    bool unsafe_path;
    bool deleted;
    const char *read_error;
    int64_t first_seen_scan_id;
    int64_t last_seen_scan_id;
    const char *first_seen_at;
    const char *last_seen_at;
    const char *deleted_at; /* NULL unless deleted */
} atlas_file_row;

typedef struct atlas_commit_record {
    const char *oid;
    const char *parents; /* space-separated oids, "" for a root commit */
    int parent_count;
    const char *author_name;
    const char *author_email;
    int64_t author_time;
    int64_t commit_time;
    const char *subject;
    const char *body;
    size_t body_len;
} atlas_commit_record;

typedef struct atlas_change_record {
    const char *change_type; /* add|modify|delete|rename|copy|typechange|unmerged|unknown */
    int score;
    bool score_known;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const void *old_path_raw; /* NULL unless rename/copy */
    size_t old_path_raw_len;
    const char *old_path_text;
    const char *raw_status;
} atlas_change_record;

typedef struct atlas_history_row {
    const char *commit_oid;
    const char *author_name;
    const char *author_email;
    int64_t author_time;
    int64_t commit_time;
    const char *subject;
    const char *change_type;
    const char *path_text;
    const char *old_path_text; /* NULL unless rename/copy */
    int score;
    bool score_known;
} atlas_history_row;

typedef struct atlas_search_hit {
    const char *kind; /* "file" | "commit" */
    /* file hits */
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    bool path_is_utf8;
    const char *git_index_oid;
    bool deleted;
    /* commit hits */
    const char *commit_oid;
    const char *subject;
    const char *author_name;
    int64_t author_time;
    /* provenance */
    const char *evidence; /* "SOURCE" | "GIT" */
} atlas_search_hit;

typedef struct atlas_repo_counts {
    int64_t files_live;
    int64_t files_deleted;
    int64_t commits;
    int64_t changes;
    int64_t scans;
    int64_t evidence;
    int64_t compile_databases;
} atlas_repo_counts;

/* --- callbacks ---------------------------------------------------------- */

typedef atlas_status (*atlas_repo_cb)(const atlas_repo_info *ri, void *ud, atlas_err *err);
typedef atlas_status (*atlas_file_row_cb)(const atlas_file_row *row, void *ud, atlas_err *err);
typedef atlas_status (*atlas_history_cb)(const atlas_history_row *row, void *ud, atlas_err *err);
typedef atlas_status (*atlas_search_cb)(const atlas_search_hit *hit, void *ud, atlas_err *err);

/* --- repositories ------------------------------------------------------- */

/* Validates `name`: 1..ATLAS_NAME_MAX bytes of [A-Za-z0-9._-], not starting
 * with '-' or '.'. */
atlas_status atlas_db_check_repo_name(const char *name, atlas_err *err);

/* Identity of one registered worktree. `git_dir` distinguishes two worktrees that
 * share `common_dir`; both are stored so the relationship is recoverable. */
typedef struct atlas_repo_identity {
    const void *root;
    size_t root_len;
    const void *common_dir;
    size_t common_dir_len;
    const void *git_dir;
    size_t git_dir_len;
    bool is_linked_worktree;
    const char *object_format;
} atlas_repo_identity;

atlas_status atlas_db_repo_add(atlas_db *db, const char *name, const atlas_repo_identity *id,
                               int64_t *id_out, atlas_err *err);
/* Lists every other registration sharing this repository's common Git directory,
 * that is, the sibling worktrees. */
atlas_status atlas_db_repo_siblings(atlas_db *db, int64_t repo_id, const void *common_dir,
                                    size_t common_dir_len, atlas_repo_cb cb, void *ud,
                                    int64_t *count_out, atlas_err *err);
atlas_status atlas_db_repo_get(atlas_db *db, const char *name, atlas_repo_info *out, bool *found,
                               atlas_err *err);
atlas_status atlas_db_repo_get_by_root(atlas_db *db, const void *root_raw, size_t root_len,
                                       atlas_repo_info *out, bool *found, atlas_err *err);
atlas_status atlas_db_repo_list(atlas_db *db, atlas_repo_cb cb, void *ud, atlas_err *err);
atlas_status atlas_db_repo_remove(atlas_db *db, const char *name, bool *removed, atlas_err *err);
atlas_status atlas_db_repo_counts(atlas_db *db, int64_t repo_id, atlas_repo_counts *out,
                                  atlas_err *err);

/* --- scans -------------------------------------------------------------- */

typedef struct atlas_scan_state {
    const char *head_oid;   /* "" when unborn */
    const char *head_state; /* born | unborn | detached */
    const char *branch;     /* "" when detached */
    const char *object_format;
    bool dirty;
    int dirty_staged;
    int dirty_unstaged;
    int dirty_untracked;
    int dirty_unmerged;
} atlas_scan_state;

atlas_status atlas_db_scan_begin(atlas_db *db, int64_t repo_id, const atlas_scan_state *st,
                                 int64_t *scan_id_out, atlas_err *err);
atlas_status atlas_db_scan_finish(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                  const char *status, const char *error_text,
                                  int64_t files_total, int64_t files_added, int64_t files_modified,
                                  int64_t files_deleted, int64_t files_unchanged,
                                  int64_t files_unreadable, int64_t commits_ingested,
                                  atlas_err *err);
/* Copies the scan's observed head/branch/dirty summary onto the repository. */
atlas_status atlas_db_repo_apply_scan(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                     const atlas_scan_state *st, atlas_err *err);

/* --- files, commits, changes, evidence ---------------------------------- */

atlas_status atlas_db_file_upsert(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                  const atlas_file_record *rec, atlas_upsert_kind *kind_out,
                                  atlas_err *err);
atlas_status atlas_db_files_mark_deleted(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                         int64_t *count_out, atlas_err *err);
atlas_status atlas_db_file_get(atlas_db *db, int64_t repo_id, const void *path_raw, size_t path_len,
                               atlas_file_row_cb cb, void *ud, bool *found, atlas_err *err);
atlas_status atlas_db_file_history(atlas_db *db, int64_t repo_id, const void *path_raw,
                                   size_t path_len, int64_t limit, atlas_history_cb cb, void *ud,
                                   int64_t *count_out, atlas_err *err);

atlas_status atlas_db_commit_upsert(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                    const atlas_commit_record *rec, int64_t *commit_id_out,
                                    bool *inserted_out, atlas_err *err);
atlas_status atlas_db_change_insert(atlas_db *db, int64_t repo_id, int64_t commit_id,
                                    const atlas_change_record *rec, atlas_err *err);
atlas_status atlas_db_changes_clear_for_commit(atlas_db *db, int64_t commit_id, atlas_err *err);

typedef struct atlas_compile_db_record {
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    bool is_regular_file;
    bool is_symlink;
    const char *content_hash;
    int64_t size_bytes;
    bool size_known;
} atlas_compile_db_record;

atlas_status atlas_db_compile_db_upsert(atlas_db *db, int64_t repo_id, int64_t scan_id,
                                       const atlas_compile_db_record *rec, atlas_err *err);

/* Evidence kinds. A0 may only create SOURCE and GIT; the enum is complete so
 * the schema and JSON contract are stable across phases. */
typedef enum atlas_evidence_kind {
    ATLAS_EV_SOURCE = 0,
    ATLAS_EV_GIT,
    ATLAS_EV_DECISION,
    ATLAS_EV_USER_STATEMENT,
    ATLAS_EV_INFERENCE,
    ATLAS_EV_UNKNOWN
} atlas_evidence_kind;

const char *atlas_evidence_kind_name(atlas_evidence_kind k);

atlas_status atlas_db_evidence_insert(atlas_db *db, int64_t repo_id, atlas_evidence_kind kind,
                                      int64_t scan_id, const char *git_oid, const void *path_raw,
                                      size_t path_len, const char *path_text,
                                      const char *commit_oid, const char *detail, atlas_err *err);

/* --- search ------------------------------------------------------------- */

typedef enum atlas_search_mode {
    ATLAS_SEARCH_FTS5 = 0,
    ATLAS_SEARCH_DEGRADED_LIKE
} atlas_search_mode;

const char *atlas_search_mode_name(atlas_search_mode m);

/* Searches file paths and commit messages. When FTS5 is unavailable the query
 * falls back to a substring match and reports ATLAS_SEARCH_DEGRADED_LIKE, which
 * callers must surface to the user. */
atlas_status atlas_db_search(atlas_db *db, int64_t repo_id, const char *query, int64_t limit,
                             atlas_search_mode *mode_out, atlas_search_cb cb, void *ud,
                             int64_t *count_out, atlas_err *err);
/* Rebuilds the FTS5 content after a scan. No-op without FTS5. */
atlas_status atlas_db_fts_rebuild(atlas_db *db, atlas_err *err);

/* --- transactions ------------------------------------------------------- */

atlas_status atlas_db_begin(atlas_db *db, atlas_err *err);
atlas_status atlas_db_commit(atlas_db *db, atlas_err *err);
void atlas_db_rollback(atlas_db *db);

#endif /* ATLAS_DB_H */

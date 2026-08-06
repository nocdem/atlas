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

#define ATLAS_SCHEMA_VERSION 4

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

/* --- filesystem identity -------------------------------------------------
 *
 * Declared here, above its first use, and used *by value* everywhere. There is
 * exactly one identity type in Atlas on purpose: an earlier revision had a
 * second, structurally identical one embedded in atlas_file_record, and copying
 * between them field by field is how ctime came to be dropped on the way to the
 * database while every other field arrived intact. One type means one
 * assignment, and a new field cannot be forgotten at a copy site.
 *
 * What Atlas last observed about a path on disk, used to decide whether the
 * content has to be read again. A pass that finds every field unchanged does no
 * I/O beyond the lstat that produced them.
 *
 * ctime is part of the identity, and it is the field that makes the identity
 * trustworthy rather than merely plausible. mtime is writable: `utimensat` lets
 * any process that can write a file restore the mtime it had before the write.
 * A same-length in-place edit followed by a restored mtime leaves device,
 * inode, size, mode and mtime all identical, so an identity built from those
 * five would report a cache hit, read nothing, and keep serving the old content
 * hash indefinitely.
 *
 * Nothing in userspace can set ctime. Every write to an inode's data or metadata
 * — including the `utimensat` call that restores mtime — updates it. Including
 * ctime therefore turns "the file looks unchanged" into "the inode has not been
 * touched". */

/* How many columns one identity occupies. Named so the bind loops and the
 * SELECT column list cannot drift apart silently when a field is added — which
 * is precisely how ctime came to be missing in the first place. */
#define ATLAS_FS_IDENTITY_COLUMNS 8

typedef struct atlas_fs_identity {
    bool known;
    int64_t dev;
    int64_t ino;
    int64_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    int64_t mode;
} atlas_fs_identity;

/* True when two observations describe the same unchanged file.
 *
 * Every recorded field must match: device, inode, size, mode (which carries the
 * file type), and both timestamps to the nanosecond. A missing prior
 * observation, or one with any field unrecorded, is never "the same" — unknown
 * means unknown, not unchanged, and an unknown identity is always rehashed. */
bool atlas_fs_identity_same(const atlas_fs_identity *a, const atlas_fs_identity *b);

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
    /* A1. `tracked` is false for a file discovered inside an untracked
     * directory; `ignored` records that git's own ignore rules cover it, so a
     * caller can tell "skipped because ignored" from "skipped because a ceiling
     * was reached". `truncated` says the content was not fully hashed and
     * `truncated_reason` says why; neither is ever left implicit. */
    bool tracked;
    bool ignored;
    bool truncated;
    const char *truncated_reason;
    int64_t generation;
    /* What lstat reported, so the next pass can skip reading this file. Stored
     * as a unit: `known == false` writes NULLs, and a partially recorded
     * identity is never written, because it would compare unequal forever.
     *
     * The shared type, assigned whole. Never copy this field by field. */
    atlas_fs_identity fs;
} atlas_file_record;

/* Initialises a record to the A0-equivalent defaults: tracked, not ignored, not
 * truncated, no filesystem identity. Callers must use this rather than memset so
 * `tracked` does not silently default to false. */
void atlas_file_record_init(atlas_file_record *rec);

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
    /* A1 */
    bool tracked;
    bool ignored;
    bool truncated;
    const char *truncated_reason;
    int64_t last_generation;
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
/* Finds the registered worktree that contains `path`, which may be the root
 * itself or any directory beneath it.
 *
 * A2 needs this because an adapter observes a working directory, not a
 * repository: a hook's cwd is normally a subdirectory and a tool's path always
 * is. The search is longest-match, walking up one component at a time, so a
 * linked worktree registered inside another repository's tree resolves to
 * itself rather than to its container. `/` is never a match.
 *
 * `*found_out` is false rather than an error when nothing matches: an
 * unregistered directory is an ordinary state, and the caller decides. */
atlas_status atlas_db_repo_get_containing(atlas_db *db, const void *path, size_t path_len,
                                          atlas_repo_info *out, bool *found_out, atlas_err *err);
/* Looks a repository up by its row id. The daemon queues work by id so that a
 * rename or removal between queueing and running resolves to nothing rather than
 * to a different repository. */
atlas_status atlas_db_repo_get_by_id(atlas_db *db, int64_t repo_id, atlas_repo_info *out,
                                     bool *found, atlas_err *err);
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
/* Rebuilds the FTS5 content from scratch. No-op without FTS5. This is O(index)
 * and is only correct to call after a full scan; an incremental pass must use
 * the per-row maintenance below instead, or a one-file edit would cost a whole
 * repository's reindex. */
atlas_status atlas_db_fts_rebuild(atlas_db *db, atlas_err *err);

/* Incremental FTS5 maintenance. `old_path_text` may be NULL when the row is new;
 * when it is given, the stale term is removed first, because an external-content
 * FTS5 table cannot derive the old value itself. All are no-ops without FTS5. */
atlas_status atlas_db_fts_file_upsert(atlas_db *db, int64_t file_id, const char *old_path_text,
                                      const char *new_path_text, atlas_err *err);
atlas_status atlas_db_fts_file_delete(atlas_db *db, int64_t file_id, const char *path_text,
                                      atlas_err *err);
atlas_status atlas_db_fts_commit_insert(atlas_db *db, int64_t commit_id, const char *subject,
                                        const char *body, size_t body_len, atlas_err *err);
/* True when the FTS5 shadow tables exist and are being maintained. */
bool atlas_db_fts_ready(const atlas_db *db);

/* --- A1: continuous index state ----------------------------------------- */

/* How current one repository's index is, and in what way it is not.
 *
 * `last_complete_generation` is the only generation a reader is ever shown. A
 * pass in flight increments `generation` first and only publishes on success,
 * so a crash mid-pass is invisible rather than half-visible.
 *
 * `event_gap` is set whenever Atlas cannot prove it observed every change: an
 * inotify queue overflow, a watch-limit failure, or a worker error. While it is
 * set, no caller may be told the index is current. */
typedef enum atlas_watch_state {
    ATLAS_WATCH_UNWATCHED = 0, /* no daemon is watching this repository */
    ATLAS_WATCH_WATCHING,      /* watches installed and complete */
    ATLAS_WATCH_DEGRADED,      /* watching, but with a known blind spot */
    ATLAS_WATCH_INCOMPLETE,    /* an event gap is unresolved; a full pass is due */
    ATLAS_WATCH_ERROR          /* the watcher failed and is not observing */
} atlas_watch_state;

const char *atlas_watch_state_name(atlas_watch_state s);
atlas_watch_state atlas_watch_state_parse(const char *name);

typedef struct atlas_index_state {
    int64_t repo_id;
    int64_t generation;
    int64_t last_complete_generation;
    char last_reconcile_at[ATLAS_TS_MAX];
    char last_complete_at[ATLAS_TS_MAX];
    atlas_watch_state watch_state;
    atlas_buf watch_detail;
    int64_t watched_dirs;
    bool event_gap;
    bool pending_full_reconcile;
    atlas_buf last_error;
    int64_t last_sync_seq;
    bool present; /* false when no row exists yet */
} atlas_index_state;

void atlas_index_state_init(atlas_index_state *s);
void atlas_index_state_free(atlas_index_state *s);

/* Reads the state, reporting `present=false` rather than failing when the
 * repository has never been watched or scanned under A1. */
atlas_status atlas_db_index_state_get(atlas_db *db, int64_t repo_id, atlas_index_state *out,
                                      atlas_err *err);
/* Creates the row if it is missing. Idempotent. */
atlas_status atlas_db_index_state_ensure(atlas_db *db, int64_t repo_id, atlas_err *err);
/* Claims the next generation number and returns it. The claim is visible
 * immediately; `last_complete_generation` is not advanced until the pass ends. */
atlas_status atlas_db_generation_begin(atlas_db *db, int64_t repo_id, int64_t *generation_out,
                                       atlas_err *err);
/* Publishes `generation` as the newest consistent state. `clear_gap` resolves an
 * outstanding event gap, which only a full reconciliation may do. */
atlas_status atlas_db_generation_complete(atlas_db *db, int64_t repo_id, int64_t generation,
                                          bool clear_gap, int64_t sync_seq, atlas_err *err);
atlas_status atlas_db_index_state_set_watch(atlas_db *db, int64_t repo_id, atlas_watch_state st,
                                            const char *detail, int64_t watched_dirs,
                                            atlas_err *err);
/* Records that Atlas may have missed changes. Sets `event_gap` and schedules a
 * full reconciliation; both survive a restart. */
atlas_status atlas_db_index_state_mark_gap(atlas_db *db, int64_t repo_id, const char *detail,
                                           atlas_err *err);
atlas_status atlas_db_index_state_set_error(atlas_db *db, int64_t repo_id, const char *detail,
                                            atlas_err *err);

/* --- A1: durable event journal ------------------------------------------ */

typedef struct atlas_event_record {
    const char *kind;
    int64_t generation;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *detail;
    /* When non-NULL, replaying the same observation is a no-op rather than a
     * duplicate row. */
    const char *dedup_key;
} atlas_event_record;

typedef struct atlas_event_row {
    int64_t id; /* the monotonic cursor */
    int64_t repo_id;
    int64_t generation;
    const char *kind;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *detail;
    const char *created_at;
} atlas_event_row;

typedef atlas_status (*atlas_event_cb)(const atlas_event_row *row, void *ud, atlas_err *err);

/* Appends an event. `*inserted_out` is false when a dedup key suppressed it. */
atlas_status atlas_db_event_append(atlas_db *db, int64_t repo_id, const atlas_event_record *rec,
                                   bool *inserted_out, atlas_err *err);
/* Streams events with id strictly greater than `since`, oldest first. `limit` is
 * clamped to ATLAS_EVENTS_PAGE_MAX. `next_cursor_out` receives the id of the last
 * delivered row, or `since` when none were. */
atlas_status atlas_db_events_since(atlas_db *db, int64_t repo_id, int64_t since, int64_t limit,
                                   atlas_event_cb cb, void *ud, int64_t *count_out,
                                   int64_t *next_cursor_out, bool *more_out, atlas_err *err);
/* The newest event id for a repository, 0 when there are none. */
atlas_status atlas_db_events_head(atlas_db *db, int64_t repo_id, int64_t *out, atlas_err *err);
/* Prunes the journal to `retain` newest rows. Durable evidence is untouched. */
atlas_status atlas_db_events_prune(atlas_db *db, int64_t repo_id, int64_t retain,
                                   int64_t *removed_out, atlas_err *err);

/* --- A1: incremental history -------------------------------------------- */

/* The commit each ref was at when its history was last ingested. */
atlas_status atlas_db_commit_tip_get(atlas_db *db, int64_t repo_id, const char *ref_name,
                                     char *oid_out, size_t oid_out_size, bool *found,
                                     atlas_err *err);
atlas_status atlas_db_commit_tip_set(atlas_db *db, int64_t repo_id, const char *ref_name,
                                     const char *oid, atlas_err *err);

/* --- A1: daemon liveness record ----------------------------------------- */

typedef struct atlas_daemon_record {
    int64_t pid;
    char started_at[ATLAS_TS_MAX];
    char last_heartbeat_at[ATLAS_TS_MAX];
    char stopped_at[ATLAS_TS_MAX];
    int protocol_version;
    char atlas_version[32];
    atlas_buf socket_path;
    bool present;
} atlas_daemon_record;

void atlas_daemon_record_init(atlas_daemon_record *r);
void atlas_daemon_record_free(atlas_daemon_record *r);

atlas_status atlas_db_daemon_started(atlas_db *db, int64_t pid, const char *socket_path,
                                     atlas_err *err);
atlas_status atlas_db_daemon_heartbeat(atlas_db *db, atlas_err *err);
atlas_status atlas_db_daemon_stopped(atlas_db *db, atlas_err *err);
atlas_status atlas_db_daemon_get(atlas_db *db, atlas_daemon_record *out, atlas_err *err);

/* --- A1: filesystem identity on file rows ------------------------------- */

/* Reads just the identity and hash of one path, without loading the whole row. */
atlas_status atlas_db_file_identity(atlas_db *db, int64_t repo_id, const void *path_raw,
                                    size_t path_len, atlas_fs_identity *out, int64_t *file_id_out,
                                    bool *found, atlas_err *err);

/* Marks a file as seen by this pass without touching any recorded fact about it.
 *
 * This is what an identity hit writes. It must not go through the full upsert:
 * that would need a complete record, and a pass that deliberately did not read
 * the file has no hash to put in one. Passing NULL for hash there would look
 * like "the file has no hash now", which is a different fact entirely. */
atlas_status atlas_db_file_touch(atlas_db *db, int64_t file_id, int64_t scan_id,
                                 int64_t generation, const atlas_fs_identity *fs, atlas_err *err);

/* --- A2: per-path working-tree change scope -----------------------------
 *
 * A1 recorded the dirty counts per repository. That answers "does this
 * repository have staged changes" and not "which paths are staged", and an MCP
 * adapter must answer the second from the index rather than by running git in
 * the daemon's serve loop.
 *
 * The reconciliation pass already runs one `git status --porcelain=v2` for the
 * counts, so it now records the entries it was already parsing. The table is a
 * snapshot replaced wholesale by each pass, not a journal: a path that is no
 * longer dirty is not a historical fact worth keeping, and history already
 * lives in `file_changes`. */
typedef struct atlas_worktree_change_record {
    const char *scope;       /* staged | unstaged | untracked | unmerged */
    char status;             /* git's own letter */
    const char *change_type; /* the Atlas change-type name */
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const void *old_path_raw; /* NULL unless rename/copy */
    size_t old_path_raw_len;
    const char *old_path_text;
    bool is_directory;
} atlas_worktree_change_record;

typedef struct atlas_worktree_change_row {
    int64_t id;
    int64_t generation;
    const char *scope;
    const char *status;
    const char *change_type;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *old_path_text; /* NULL unless rename/copy */
    bool is_directory;
    const char *observed_at;
} atlas_worktree_change_row;

typedef atlas_status (*atlas_worktree_change_cb)(const atlas_worktree_change_row *row, void *ud,
                                                 atlas_err *err);

/* Replaces the repository's snapshot. Called inside the pass's apply stage. */
atlas_status atlas_db_worktree_changes_clear(atlas_db *db, int64_t repo_id, atlas_err *err);
atlas_status atlas_db_worktree_change_insert(atlas_db *db, int64_t repo_id, int64_t generation,
                                             const atlas_worktree_change_record *rec,
                                             atlas_err *err);
/* Streams the snapshot. `scope` may be NULL for every scope. Requests one row
 * more than `limit` and never delivers it, so `more` is a fact rather than an
 * inference from a full page. `after_id` is the exclusive cursor. */
atlas_status atlas_db_worktree_changes_list(atlas_db *db, int64_t repo_id, const char *scope,
                                            int64_t after_id, int64_t limit,
                                            atlas_worktree_change_cb cb, void *ud,
                                            int64_t *count_out, int64_t *cursor_out,
                                            bool *more_out, atlas_err *err);
atlas_status atlas_db_worktree_changes_count(atlas_db *db, int64_t repo_id, int64_t *staged,
                                             int64_t *unstaged, int64_t *untracked,
                                             int64_t *unmerged, atlas_err *err);

/* --- A2: AI sessions, change sets, reasons and decisions ------------------
 *
 * Typed operations over the migration-4 tables. sqlite3 types stay in src/db as
 * everywhere else; the provider-neutral behaviour on top of these lives in
 * src/ai.
 *
 * The A0 evidence restriction is untouched by all of this: nothing here writes
 * to `evidence`, and `atlas_db_evidence_insert` still refuses everything but
 * SOURCE and GIT. */

/* One session as a reader sees it. Owned buffers, so `_init`/`_free` as usual. */
typedef struct atlas_ai_session_report {
    int64_t id;
    atlas_buf provider;
    atlas_buf client;
    atlas_buf session_key;
    atlas_buf agent_type; /* empty for a main session rather than a subagent */
    char state[16];       /* open | closed | expired */
    char started_at[ATLAS_TS_MAX];
    char last_seen_at[ATLAS_TS_MAX];
    int64_t turns;
    int64_t tool_calls;
    int64_t records;
    int64_t compactions;
    int64_t resumes;
    int64_t repos;
    bool present;
} atlas_ai_session_report;

void atlas_ai_session_report_init(atlas_ai_session_report *r);
void atlas_ai_session_report_free(atlas_ai_session_report *r);

/* Borrowed row pointers, valid only for the duration of the callback, exactly
 * like every other row callback in this header. */
typedef struct atlas_ai_reason_row {
    int64_t id;
    int64_t session_id;
    const char *created_at;
    const char *provenance;
    const char *state; /* proposed | unknown | superseded */
    const char *confidence;
    const char *summary;        /* model-authored, safe-encoded, bounded */
    const char *detail;         /* NULL when none */
    const char *unknown_reason; /* NULL unless the state is unknown */
    int64_t path_count;
    bool approved; /* always false in A2; the column CHECKs it */
} atlas_ai_reason_row;

typedef struct atlas_ai_decision_row {
    int64_t id;
    int64_t session_id;
    const char *created_at;
    const char *provenance;
    const char *state;
    const char *title;
    const char *statement;
    const char *rationale; /* NULL when none */
    int64_t path_count;
    bool approved; /* always false in A2 */
} atlas_ai_decision_row;

typedef struct atlas_ai_changed_row {
    const void *path_raw; /* the key; borrowed for the call only */
    size_t path_raw_len;
    const char *path_text; /* already in the safe encoding */
    const char *attribution;
    const char *direct_tool; /* NULL unless the attribution is direct_edit */
    const char *first_at;
    const char *last_at;
    int64_t occurrences;
    int64_t concurrent_sessions;
    bool has_reason;
} atlas_ai_changed_row;

typedef atlas_status (*atlas_ai_reason_cb)(const atlas_ai_reason_row *row, void *ud,
                                           atlas_err *err);
typedef atlas_status (*atlas_ai_decision_cb)(const atlas_ai_decision_row *row, void *ud,
                                             atlas_err *err);
typedef atlas_status (*atlas_ai_changed_cb)(const atlas_ai_changed_row *row, void *ud,
                                            atlas_err *err);

atlas_status atlas_db_ai_client_upsert(atlas_db *db, const char *provider, const char *name,
                                       int64_t *id_out, atlas_err *err);
/* The read-only half of the pair. `*id_out` is 0 when this provider/client has
 * never been seen. Exists because the read methods run on a read-only handle
 * and must not create a client row merely by asking a question. */
atlas_status atlas_db_ai_client_find(atlas_db *db, const char *provider, const char *name,
                                     int64_t *id_out, atlas_err *err);

/* Finds a session by its client-chosen key. `*id_out` is 0 when there is none,
 * which is not an error: a hook for a session Atlas never saw is normal.
 *
 * The lookup is `(client_id, session_key)` and nothing else. There is
 * deliberately no "find a session for this repository" query: a repository does
 * not identify a session, and choosing one by recency attributes a record to
 * whichever session happened to be touched last. */
atlas_status atlas_db_ai_session_find(atlas_db *db, int64_t client_id, const char *session_key,
                                      int64_t *id_out, atlas_err *err);
/* The same lookup, also reporting whether the session is open.
 *
 * The state is a separate output rather than a filter because the two facts are
 * different: "no session has this key" and "the session with this key has
 * ended" lead to different, and differently reported, outcomes. */
atlas_status atlas_db_ai_session_find_state(atlas_db *db, int64_t client_id,
                                            const char *session_key, int64_t *id_out,
                                            bool *open_out, atlas_err *err);
/* Opens or resumes. An existing row is resumed — its `resumes` counter is
 * incremented and its state returns to open — rather than replaced, so a resume
 * keeps the change set the session already has. */
atlas_status atlas_db_ai_session_open(atlas_db *db, int64_t client_id, const char *session_key,
                                      int64_t parent_id, const char *agent_id,
                                      const char *agent_type, const char *client_version,
                                      int64_t *id_out, bool *created_out, atlas_err *err);
/* Bumps `last_seen_at` and, when the counter name is given, one bounded counter
 * of `turns`, `tool_calls`, `records` or `compactions`. */
atlas_status atlas_db_ai_session_touch(atlas_db *db, int64_t session_id, const char *counter,
                                       atlas_err *err);
atlas_status atlas_db_ai_session_close(atlas_db *db, int64_t session_id, const char *reason,
                                       atlas_err *err);
/* Marks every open session untouched since `cutoff_iso` as expired. A client
 * that vanished must not hold a change set open forever. */
atlas_status atlas_db_ai_sessions_expire(atlas_db *db, const char *cutoff_iso, int64_t *count_out,
                                         atlas_err *err);
atlas_status atlas_db_ai_session_get(atlas_db *db, int64_t client_id, const char *session_key,
                                     atlas_ai_session_report *out, atlas_err *err);

atlas_status atlas_db_ai_session_attach_repo(atlas_db *db, int64_t session_id, int64_t repo_id,
                                             const char *source, const char *base_head,
                                             atlas_err *err);

atlas_status atlas_db_ai_event_append(atlas_db *db, int64_t session_id, int64_t repo_id,
                                      const char *kind, const char *tool_name,
                                      const char *tool_use_id, const void *path_raw,
                                      size_t path_len, const char *path_text,
                                      const char *dedup_key, bool *inserted_out, atlas_err *err);
atlas_status atlas_db_ai_events_prune(atlas_db *db, int64_t session_id, int64_t retain,
                                      int64_t *removed_out, atlas_err *err);
/* The most recent intent recorded for a path in this session, so a batch can
 * tell a path this session edited from one that merely changed. `*found_out` is
 * false when there is none. `tool_out` receives the tool name. */
atlas_status atlas_db_ai_event_intent_for_path(atlas_db *db, int64_t session_id,
                                               const void *path_raw, size_t path_len,
                                               atlas_buf *tool_out, bool *found_out,
                                               atlas_err *err);

atlas_status atlas_db_ai_change_set_ensure(atlas_db *db, int64_t session_id, int64_t repo_id,
                                           const char *base_head, int64_t base_generation,
                                           int64_t *id_out, atlas_err *err);
atlas_status atlas_db_ai_change_set_find(atlas_db *db, int64_t session_id, int64_t repo_id,
                                         int64_t *id_out, atlas_err *err);
/* Records or refreshes one observed changed path.
 *
 * Attribution never improves: a row already marked ambiguous stays ambiguous,
 * because a later unambiguous observation does not retroactively resolve an
 * earlier overlapping one. */
atlas_status atlas_db_ai_changed_path_record(atlas_db *db, int64_t change_set_id,
                                             const void *path_raw, size_t path_len,
                                             const char *path_text, const char *attribution,
                                             const char *direct_tool, int64_t concurrent_sessions,
                                             atlas_err *err);
atlas_status atlas_db_ai_changed_counts(atlas_db *db, int64_t change_set_id, int64_t *total,
                                        int64_t *direct, int64_t *ambiguous, int64_t *unresolved,
                                        atlas_err *err);
atlas_status atlas_db_ai_changed_list(atlas_db *db, int64_t change_set_id, int64_t limit,
                                      atlas_ai_changed_cb cb, void *ud, int64_t *count_out,
                                      bool *more_out, atlas_err *err);
/* How many *other* sessions currently have this repository open. */
atlas_status atlas_db_ai_concurrent_sessions(atlas_db *db, int64_t repo_id, int64_t except_session,
                                             int64_t *count_out, atlas_err *err);

/* A durable record. `dedup_key` may be NULL; when it is not, replaying the same
 * request collides on a partial unique index instead of creating a second row. */
typedef struct atlas_ai_record_input {
    int64_t session_id; /* 0 for none */
    int64_t repo_id;
    int64_t change_set_id; /* 0 for none */
    const char *provenance;
    const char *state;
    const char *confidence;
    const char *summary;
    const char *detail;
    const char *unknown_reason;
    const char *title;
    const char *statement;
    const char *rationale;
    const char *dedup_key;
} atlas_ai_record_input;

atlas_status atlas_db_ai_reason_insert(atlas_db *db, const atlas_ai_record_input *in,
                                       int64_t *id_out, bool *duplicate_out, atlas_err *err);
atlas_status atlas_db_ai_reason_path_add(atlas_db *db, int64_t reason_id, const void *path_raw,
                                         size_t path_len, const char *path_text, atlas_err *err);
atlas_status atlas_db_ai_decision_insert(atlas_db *db, const atlas_ai_record_input *in,
                                         int64_t *id_out, bool *duplicate_out, atlas_err *err);
atlas_status atlas_db_ai_decision_path_add(atlas_db *db, int64_t decision_id, const void *path_raw,
                                           size_t path_len, const char *path_text, atlas_err *err);
/* Links a record to the newest SOURCE or GIT evidence Atlas holds for a path,
 * so a model claim and the facts about the same path stay connected without
 * either becoming the other. A path with no evidence links nothing, silently:
 * that is the normal case for a file Atlas has not indexed yet. */
atlas_status atlas_db_ai_evidence_link(atlas_db *db, const char *subject_kind, int64_t subject_id,
                                       int64_t repo_id, const void *path_raw, size_t path_len,
                                       atlas_err *err);

atlas_status atlas_db_ai_checkpoint_insert(atlas_db *db, int64_t session_id, const char *phase,
                                           int64_t repos, int64_t changed_paths,
                                           int64_t unresolved_paths, int64_t reasons,
                                           int64_t decisions, const char *dedup_key,
                                           bool *inserted_out, atlas_err *err);

/* Reasons and decisions recorded for one repository, newest first. `path_raw`
 * may be NULL to list every record for the repository. */
atlas_status atlas_db_ai_reasons_list(atlas_db *db, int64_t repo_id, const void *path_raw,
                                      size_t path_len, int64_t limit, atlas_ai_reason_cb cb,
                                      void *ud, int64_t *count_out, bool *more_out,
                                      atlas_err *err);
atlas_status atlas_db_ai_decisions_list(atlas_db *db, int64_t repo_id, const void *path_raw,
                                        size_t path_len, int64_t limit, atlas_ai_decision_cb cb,
                                        void *ud, int64_t *count_out, bool *more_out,
                                        atlas_err *err);
/* A bounded substring search over recorded reasons and decisions. Deliberately
 * not FTS5: these tables are small, the query is a model's, and a bounded LIKE
 * has no query language a caller can be surprised by. */
atlas_status atlas_db_ai_reasons_search(atlas_db *db, int64_t repo_id, const char *query,
                                        int64_t limit, atlas_ai_reason_cb cb, void *ud,
                                        int64_t *count_out, bool *more_out, atlas_err *err);
atlas_status atlas_db_ai_decisions_search(atlas_db *db, int64_t repo_id, const char *query,
                                          int64_t limit, atlas_ai_decision_cb cb, void *ud,
                                          int64_t *count_out, bool *more_out, atlas_err *err);
/* Counts for the automatic envelope: proposed decisions, approved decisions
 * (always 0 in A2, present so it can stop being), and changed paths with no
 * reason of any kind recorded. */
atlas_status atlas_db_ai_repo_record_counts(atlas_db *db, int64_t repo_id, int64_t *proposed,
                                            int64_t *approved, int64_t *reasons, atlas_err *err);

/* --- transactions ------------------------------------------------------- */

atlas_status atlas_db_begin(atlas_db *db, atlas_err *err);
atlas_status atlas_db_commit(atlas_db *db, atlas_err *err);
void atlas_db_rollback(atlas_db *db);
/* Opens the database read-only. Used by daemon reader threads and by CLI read
 * commands while a daemon owns the writer, so a reader can never take the write
 * lock by accident. */
atlas_status atlas_db_open_readonly(const char *path, atlas_db **out, atlas_err *err);
bool atlas_db_is_readonly(const atlas_db *db);

#endif /* ATLAS_DB_H */

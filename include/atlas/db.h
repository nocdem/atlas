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
#include "atlas/decision.h"
#include "atlas/error.h"
#include "atlas/limits.h"

#define ATLAS_SCHEMA_VERSION 21

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

/* Whether Atlas can prove the file index describes the working tree.
 *
 * The single authority on that question, asked by the A2 serve loop
 * (`atlas_server_index_current` delegates to it) and by A9.2.2's coverage model
 * when it decides `ATLAS_COVDIM_REPOSITORY_SNAPSHOT`. `reason_out` receives one
 * of a fixed Atlas-owned vocabulary of sentences, or NULL when the index is
 * current; nothing a repository can influence reaches it.
 *
 * A pure function of the state, so it takes no handle and can be asked
 * anywhere. **Fail-closed**: every path that cannot establish currency returns
 * false, and A1's rule that an event gap makes the index non-current until a
 * full pass resolves it is enforced here rather than by each caller. */
bool atlas_index_state_is_current(const atlas_index_state *s, const char **reason_out);

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

/* --- A3: the structural code graph ---------------------------------------
 *
 * Typed operations over the migration-5 tables. sqlite3 types stay in src/db as
 * everywhere else; the extraction and resolution logic lives in src/code.
 *
 * The A0 evidence restriction is untouched by all of this: nothing here writes
 * to `evidence`, and `atlas_db_evidence_insert` still refuses everything but
 * SOURCE and GIT. Structural facts carry their own resolution and provenance
 * columns, for the same reason A2's model records do. */

/* How current one repository's structural index is.
 *
 * The generation pair mirrors `atlas_index_state` deliberately: it holds the
 * *reconciliation* pass generation whose structural work completed, so "does
 * the graph describe the file index?" is an integer comparison rather than an
 * inference from timestamps. */
typedef struct atlas_code_index_state {
    int64_t repo_id;
    int64_t generation;
    int64_t last_complete_generation;
    char last_indexed_at[ATLAS_TS_MAX];
    char last_complete_at[ATLAS_TS_MAX];
    bool degraded;
    atlas_buf degraded_reason; /* a fixed Atlas string */
    atlas_buf detail;
    atlas_buf last_error;
    int64_t files_indexed;
    int64_t files_parsed_last;
    int64_t symbols;
    int64_t relations;
    int64_t ambiguous;
    int64_t unresolved;
    bool compile_db_present;
    atlas_buf compile_db_hash;
    int64_t compile_units;
    int64_t compile_entries_dropped;
    /* The analyzer that produced the stored graph, read back from
     * `code_analyzers`. Empty and 0 when nothing has indexed yet. A mismatch
     * against ATLAS_CODE_ANALYZER_ID / ATLAS_CODE_ANALYZER_VERSION means the
     * graph was built by a different algorithm and is stale however well its
     * generations line up. */
    atlas_buf analyzer_name;
    int64_t analyzer_version;
    /* Every edge has been through resolution since the last thing that could
     * change the answer. A pass that parsed and removed nothing, and found the
     * compile database unchanged, skips resolution entirely when this is set.
     * Cleared by atlas_db_code_state_begin, set by atlas_db_code_state_complete,
     * so it is false after a crash rather than optimistically true. */
    bool resolve_settled;
    bool present; /* false when no row exists yet */
} atlas_code_index_state;

void atlas_code_index_state_init(atlas_code_index_state *s);
void atlas_code_index_state_free(atlas_code_index_state *s);

atlas_status atlas_db_code_state_get(atlas_db *db, int64_t repo_id, atlas_code_index_state *out,
                                     atlas_err *err);
atlas_status atlas_db_code_state_ensure(atlas_db *db, int64_t repo_id, atlas_err *err);
/* Interns one Atlas-owned producer identity and returns its row id, creating the
 * row the first time it is seen. `name` is always a string literal in the Atlas
 * binary; nothing repository-controlled or model-controlled may reach it. */
atlas_status atlas_db_code_analyzer_intern(atlas_db *db, const char *name, int64_t version,
                                           int64_t *id_out, atlas_err *err);
/* Records which analyzer built the repository's current graph. */
atlas_status atlas_db_code_state_set_analyzer(atlas_db *db, int64_t repo_id, int64_t analyzer_id,
                                              atlas_err *err);
/* Claims the structural generation for this pass. Mirrors
 * atlas_db_generation_begin: visible immediately, published only at the end. */
atlas_status atlas_db_code_state_begin(atlas_db *db, int64_t repo_id, int64_t generation,
                                       atlas_err *err);
/* Publishes `generation`, recomputes the counters from the tables, and records
 * the degraded state. Advanced with max() like every other published
 * generation, so a slow pass finishing after a newer one cannot move the
 * published state backwards.
 *
 * The counters are recomputed rather than incremented: an incremented counter
 * drifts the first time a path fails part way through, and five COUNT(*)
 * queries are cheaper than a number nobody can trust.
 *
 * `recount` says whether to run them. Pass false only for a pass that provably
 * wrote nothing — no file parsed, none removed, no compile-database change, and
 * resolution already settled — because two of the five counts scan the relation
 * table and confirming a number that cannot have moved is not free.
 *
 * Also sets `resolve_settled`, which `atlas_db_code_state_begin` cleared. */
atlas_status atlas_db_code_state_complete(atlas_db *db, int64_t repo_id, int64_t generation,
                                          int64_t files_parsed, bool degraded,
                                          const char *degraded_reason, const char *detail,
                                          bool recount, atlas_err *err);
atlas_status atlas_db_code_state_set_error(atlas_db *db, int64_t repo_id, const char *detail,
                                           atlas_err *err);
atlas_status atlas_db_code_state_set_compile_db(atlas_db *db, int64_t repo_id, bool present,
                                                const char *hash, int64_t units, int64_t dropped,
                                                atlas_err *err);

/* --- selection ----------------------------------------------------------- */

/* One file the structural pass has to parse, or one whose graph rows must go.
 * Borrowed pointers, valid for the callback only, exactly like every other row
 * callback in this header. */
typedef struct atlas_code_todo_row {
    int64_t file_id;      /* the `files` row; 0 for a removal */
    int64_t code_file_id; /* the `code_files` row; 0 when there is none yet */
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *content_hash; /* NULL for a removal */
    const char *language;     /* the atlas_code_language wire name */
} atlas_code_todo_row;

typedef atlas_status (*atlas_code_todo_cb)(const atlas_code_todo_row *row, void *ud,
                                           atlas_err *err);

/* Streams the files whose stored graph facts no longer match their content.
 *
 * This one query is what makes the incremental guarantees fall out rather than
 * being aimed at. It compares `files.content_hash` against the hash the graph
 * rows were built from, so an unchanged pass selects nothing **even when it was
 * a full content-verifying pass** — a full pass rehashes every byte and finds
 * the same hash — and a one-file edit selects one file.
 *
 * Ordered by raw path bytes, so the parse order and therefore every id
 * allocation is deterministic regardless of worker scheduling. */
atlas_status atlas_db_code_files_to_parse(atlas_db *db, int64_t repo_id, int64_t limit,
                                          atlas_code_todo_cb cb, void *ud, int64_t *count_out,
                                          bool *more_out, atlas_err *err);
/* Streams `code_files` rows whose path is gone from the index or tombstoned.
 *
 * Deletion is explicit writer-path work rather than a foreign key, because
 * `files` rows are tombstoned rather than removed: a cascade from `files` would
 * fire only on `repo remove`, which is the one case it is not needed for. */
atlas_status atlas_db_code_files_to_remove(atlas_db *db, int64_t repo_id, atlas_code_todo_cb cb,
                                           void *ud, int64_t *count_out, atlas_err *err);

/* --- per-file replacement ------------------------------------------------- */

typedef struct atlas_code_file_record {
    int64_t file_id;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *language;
    const char *content_hash;
    const char *parse_status;
    const char *parse_detail;
    bool truncated;
    const char *truncated_reason;
    bool include_guard;
    int64_t symbol_count;
    int64_t include_count;
    int64_t occurrence_count;
    int64_t bytes;
    int64_t lines;
    int64_t generation;
} atlas_code_file_record;

/* Creates or refreshes the `code_files` row and returns its id. */
atlas_status atlas_db_code_file_upsert(atlas_db *db, int64_t repo_id,
                                       const atlas_code_file_record *rec, int64_t *id_out,
                                       atlas_err *err);
/* Drops every symbol, occurrence, relation, candidate and role this file owns,
 * leaving the `code_files` row. Called immediately before reinserting them, so
 * a reparse replaces rather than accumulates. */
atlas_status atlas_db_code_file_clear(atlas_db *db, int64_t code_file_id, atlas_err *err);
/* Drops the `code_files` row and everything it owns. */
atlas_status atlas_db_code_file_delete(atlas_db *db, int64_t code_file_id, atlas_err *err);
/* Drops every structural row for a repository, so the next pass rebuilds. */
atlas_status atlas_db_code_clear_repo(atlas_db *db, int64_t repo_id, atlas_err *err);

atlas_status atlas_db_code_role_add(atlas_db *db, int64_t code_file_id, const char *role,
                                    const char *basis, const char *resolution, atlas_err *err);

typedef struct atlas_code_symbol_record {
    const void *name;
    size_t name_len;
    const char *name_text;
    const char *kind;
    const char *linkage;
    const char *resolution;
    bool is_definition;
    bool is_declaration;
    int64_t line;
    int64_t col;
    int64_t byte_offset;
    int64_t end_line;
    int64_t enclosing_id; /* 0 for file scope */
    int64_t generation;
} atlas_code_symbol_record;

atlas_status atlas_db_code_symbol_insert(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                         const atlas_code_symbol_record *rec, int64_t *id_out,
                                         atlas_err *err);

typedef struct atlas_code_occurrence_record {
    const void *name;
    size_t name_len;
    const char *name_text;
    const char *resolution;
    int64_t enclosing_id; /* 0 when outside any function */
    int64_t line;
    int64_t col;
    int64_t byte_offset;
    int64_t generation;
} atlas_code_occurrence_record;

atlas_status atlas_db_code_occurrence_insert(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                             const atlas_code_occurrence_record *rec,
                                             int64_t *id_out, atlas_err *err);

typedef struct atlas_code_relation_record {
    int64_t owner_file_id;
    const char *kind;
    const char *src_kind;
    int64_t src_id;
    const char *dst_kind;
    int64_t dst_id;
    const void *dst_name; /* the spelling; kept whether or not it resolved */
    size_t dst_name_len;
    const char *dst_name_text;
    /* "quote" or "angle" for an include, NULL otherwise. It changes what the
     * spelling means, so resolution needs it and it cannot be re-derived. */
    const char *spelling_form;
    const char *resolution;
    const char *provenance;
    int64_t candidate_count;
    const char *detail; /* one of the fixed ATLAS_CODE_WHY_* strings */
    int64_t line;
    int64_t col;
    int64_t generation;
} atlas_code_relation_record;

atlas_status atlas_db_code_relation_insert(atlas_db *db, int64_t repo_id,
                                           const atlas_code_relation_record *rec, int64_t *id_out,
                                           atlas_err *err);
atlas_status atlas_db_code_candidate_add(atlas_db *db, int64_t relation_id, const char *node_kind,
                                         int64_t node_id, int64_t rank, const char *detail,
                                         atlas_err *err);

/* --- resolution ----------------------------------------------------------- */

/* One edge awaiting resolution, as the resolver sees it. */
typedef struct atlas_code_pending_row {
    int64_t id;
    int64_t owner_file_id;
    const char *kind;
    int64_t src_id;
    const char *src_kind;
    const void *dst_name;
    size_t dst_name_len;
    const char *dst_name_text;
    const char *spelling_form; /* "quote" | "angle" for an include, NULL otherwise */
    const char *resolution;
    /* How many candidates the *previous* resolution of this edge found.
     *
     * Carried so the resolver can skip clearing a candidate set that cannot
     * exist. `settle` is the only writer of `code_candidates` and it always
     * records the count alongside, so zero here means there is nothing to
     * delete — and on a first pass that is every edge in the repository. */
    int64_t candidate_count;
    int64_t line;
    int64_t col;
    /* The owning file's path, so include resolution can work relative to it
     * without a second query per edge. */
    const void *owner_path_raw;
    size_t owner_path_len;
} atlas_code_pending_row;

typedef atlas_status (*atlas_code_pending_cb)(const atlas_code_pending_row *row, void *ud,
                                              atlas_err *err);

/* Which edges a resolution sweep looks at. */
typedef enum atlas_code_sweep {
    /* Everything of that kind. Used by a rebuild. */
    ATLAS_CODE_SWEEP_ALL = 0,
    /* UNRESOLVED and AMBIGUOUS only: this pass's own new edges, plus everything
     * a previous pass could not place. The cheap incremental case. */
    ATLAS_CODE_SWEEP_UNSETTLED,
    /* Everything *except* those: the edges a previous pass already settled.
     *
     * This is what the by-name sweep looks at, and the distinction is what keeps
     * it from being quadratic. The unsettled sweep has already dealt with the
     * new edges; re-resolving them again, once per changed symbol name, is the
     * same work repeated thousands of times on a first pass. */
    ATLAS_CODE_SWEEP_SETTLED
} atlas_code_sweep;

/* Streams every edge of `kind` that needs (re-)resolving.
 *
 * `owner_file_id` selects one file's edges; 0 selects the whole repository.
 * `name` restricts to edges mentioning one spelling, which is how a header that
 * gained a definition updates the call sites elsewhere that name it without
 * anything being reparsed — and which is a genuine index seek rather than a
 * filtered scan, because the two shapes use different statements.
 *
 * Paginated: rows with an id greater than `after_id` only, at most `limit` of
 * them, and `*cursor_out` receives the last id delivered. That is what lets the
 * resolver commit between chunks — a transaction may not be held across the
 * whole repository's worth of resolution, and a statement may not be left open
 * across a commit, so the sweep has to be resumable rather than streamed. */
atlas_status atlas_db_code_relations_pending(atlas_db *db, int64_t repo_id, const char *kind,
                                             int64_t owner_file_id, atlas_code_sweep sweep,
                                             const void *name, size_t name_len, int64_t after_id,
                                             int64_t limit, atlas_code_pending_cb cb, void *ud,
                                             int64_t *count_out, int64_t *cursor_out,
                                             atlas_err *err);
/* Unsettles every edge that resolved to something owned by one file: its
 * symbols, and — when `include_file_itself` — the file node too.
 *
 * The targeted form of the dangling sweep below, and the one the incremental
 * path uses. Reparsing a file deletes and recreates its symbol rows with new
 * ids, so the edges elsewhere that pointed at the old ones must go back to
 * unresolved; the rows to change are found through `idx_code_rel_dst` from the
 * ids about to disappear, which is a seek per symbol rather than a scan of the
 * repository. Call it *before* the rows are deleted — afterwards the ids are
 * gone and only the scan can find the damage.
 *
 * `include_file_itself` is for a removal: an include edge resolved to this file
 * dangles too, whereas a reparse leaves the `code_files` row in place. */
atlas_status atlas_db_code_relations_unsettle_for_file(atlas_db *db, int64_t repo_id,
                                                       int64_t code_file_id,
                                                       bool include_file_itself,
                                                       int64_t generation, int64_t *count_out,
                                                       atlas_err *err);
/* Streams edges whose resolved destination row no longer exists, across the
 * whole repository. Complete but repository-sized: a left join over every
 * relation. Kept for the rebuild path and for the case where the incremental
 * scope overflowed, where a scan is the honest answer. */
atlas_status atlas_db_code_relations_dangling(atlas_db *db, int64_t repo_id,
                                              atlas_code_pending_cb cb, void *ud,
                                              int64_t *count_out, atlas_err *err);
/* Rewrites one edge's resolution. Candidates are replaced wholesale. */
atlas_status atlas_db_code_relation_resolve(atlas_db *db, int64_t relation_id, const char *dst_kind,
                                            int64_t dst_id, const char *resolution,
                                            const char *provenance, int64_t candidate_count,
                                            const char *detail, int64_t generation,
                                            atlas_err *err);
atlas_status atlas_db_code_candidates_clear(atlas_db *db, int64_t relation_id, atlas_err *err);
/* Removes the derived `file_depends_on_file` edges a file owns, so they can be
 * rebuilt from the resolved edges without accumulating. */
atlas_status atlas_db_code_depends_clear(atlas_db *db, int64_t owner_file_id, atlas_err *err);

/* Candidate lookup used by the resolver. Both are ordered deterministically —
 * by raw path bytes, then id — so the same repository resolves the same way
 * whatever order the workers finished in. */
typedef struct atlas_code_match_row {
    int64_t id;         /* symbol id, or code_files id for a file match */
    int64_t code_file_id;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *kind;
    const char *linkage;
    bool is_definition;
} atlas_code_match_row;

typedef atlas_status (*atlas_code_match_cb)(const atlas_code_match_row *row, void *ud,
                                            atlas_err *err);

/* Repository files whose path equals, or ends with `/`, the given suffix. Used
 * for include resolution. */
atlas_status atlas_db_code_files_matching(atlas_db *db, int64_t repo_id, const void *suffix,
                                          size_t suffix_len, bool exact, int64_t limit,
                                          atlas_code_match_cb cb, void *ud, int64_t *count_out,
                                          atlas_err *err);
/* Definitions and declarations of one name. `same_file_id` non-zero additionally
 * returns internal-linkage matches from that file; internal-linkage matches from
 * any other file are never returned, which is what keeps two files' `static
 * helper` functions distinct. */
/* `ordered` asks for candidates in the reported order — path bytes, then byte
 * offset, then id. Neither supporting index can produce that order, so an
 * ordered lookup costs a temporary B-tree; pass false when the caller is going
 * to discover there are fewer than two candidates, and ask again with true only
 * if there turn out to be more. Zero or one candidate has no order to get
 * wrong, and that is the case a quarter of a million times per acceptance
 * pass. */
atlas_status atlas_db_code_symbols_named(atlas_db *db, int64_t repo_id, const void *name,
                                         size_t name_len, bool definitions_only,
                                         int64_t same_file_id, int64_t limit, bool ordered,
                                         atlas_code_match_cb cb, void *ud, int64_t *count_out,
                                         atlas_err *err);

/* --- compile units -------------------------------------------------------- */

typedef struct atlas_code_unit_record {
    const void *source_path_raw;
    size_t source_path_len;
    const char *source_path_text;
    const char *output_text;
    const char *directory_text;
    const char *language_standard;
    const char *explicit_language;
    int64_t arg_count;
    int64_t dropped_args;
    bool command_present;
    const char *command_hash;
    int64_t entry_index;
    int64_t generation;
} atlas_code_unit_record;

atlas_status atlas_db_code_units_clear(atlas_db *db, int64_t repo_id, atlas_err *err);
/* Rebuilds the unit-to-file edges from the recorded units and the resolved
 * include edges.
 *
 * Deleted and recreated wholesale rather than maintained incrementally, and it
 * has to run *after* the parse and the resolution rather than during compile
 * database ingestion: on a first pass there is no `code_files` row to link to
 * yet, and on a later one the includes are not resolved until the resolver has
 * run. Two indexed joins over a bounded number of units is cheap enough that
 * correctness by construction beats bookkeeping. */
/* `code_file_id` restricts the rebuild to the units that compile that one file,
 * which is what an incremental pass needs: `unit_uses_header` is derived from a
 * source file's own direct includes, so reparsing one file can only change the
 * edges of the units built from it. Pass 0 for the whole repository — required
 * when the compile database itself changed, since then every unit may differ. */
atlas_status atlas_db_code_link_units(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                      int64_t generation,
                                      int64_t *count_out, atlas_err *err);
atlas_status atlas_db_code_unit_insert(atlas_db *db, int64_t repo_id,
                                       const atlas_code_unit_record *rec, int64_t *id_out,
                                       atlas_err *err);
atlas_status atlas_db_code_unit_include_add(atlas_db *db, int64_t unit_id, const char *kind,
                                            const void *dir_raw, size_t dir_len,
                                            const char *dir_text, bool external, int64_t rank,
                                            atlas_err *err);
atlas_status atlas_db_code_unit_define_add(atlas_db *db, int64_t unit_id, const char *name,
                                           const char *value, bool undef, int64_t rank,
                                           atlas_err *err);
/* The include directories a build was configured with, ordered deterministically
 * and **excluding external ones** — those are metadata about where a build looks
 * and never authorise Atlas to read anything.
 *
 * `path_raw` selects the units that compile that exact source. Passing NULL
 * selects the repository-wide distinct set, which is what a header falls back
 * to: no translation unit compiles a header, and computing which units
 * transitively include it would cost more than the answer is worth. The fallback
 * is why an include resolved through it is `BUILD_METADATA` rather than
 * anything stronger. */
atlas_status atlas_db_code_unit_dirs_for_file(atlas_db *db, int64_t repo_id, const void *path_raw,
                                              size_t path_len, int64_t limit,
                                              atlas_code_match_cb cb, void *ud, atlas_err *err);

/* --- errors --------------------------------------------------------------- */

atlas_status atlas_db_code_error_add(atlas_db *db, int64_t repo_id, const char *path_text,
                                     const char *kind, const char *detail, int64_t generation,
                                     atlas_err *err);
atlas_status atlas_db_code_errors_prune(atlas_db *db, int64_t repo_id, int64_t retain,
                                        atlas_err *err);

/* --- queries -------------------------------------------------------------- */

/* One structural symbol as a reader sees it. Borrowed for the callback only. */
typedef struct atlas_code_symbol_row {
    int64_t id;
    int64_t code_file_id;
    const char *path_text;
    const char *name_text;
    const char *kind;
    const char *linkage;
    const char *resolution;
    bool is_definition;
    bool is_declaration;
    int64_t line;
    int64_t col;
} atlas_code_symbol_row;

typedef atlas_status (*atlas_code_symbol_cb)(const atlas_code_symbol_row *row, void *ud,
                                             atlas_err *err);

/* A bounded substring search over indexed symbol names.
 *
 * Deliberately a LIKE rather than FTS5: identifiers are not prose, an
 * identifier substring is what a caller actually wants, and a bounded LIKE has
 * no query language a caller can be surprised by. `kind` may be NULL. */
atlas_status atlas_db_code_symbol_search(atlas_db *db, int64_t repo_id, const char *query,
                                         const char *kind, int64_t limit,
                                         atlas_code_symbol_cb cb, void *ud, int64_t *count_out,
                                         bool *more_out, atlas_err *err);
/* Every recorded site for one exact name. */
atlas_status atlas_db_code_symbols_by_name(atlas_db *db, int64_t repo_id, const char *name_text,
                                           int64_t limit, atlas_code_symbol_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err);
atlas_status atlas_db_code_symbols_in_file(atlas_db *db, int64_t code_file_id, int64_t limit,
                                           atlas_code_symbol_cb cb, void *ud, int64_t *count_out,
                                           bool *more_out, atlas_err *err);

/* One edge as a reader sees it, with everything needed to explain why it is
 * being reported. */
typedef struct atlas_code_edge_row {
    int64_t id;
    const char *kind;
    const char *src_kind;
    int64_t src_id;
    const char *src_path_text; /* NULL when the source is not a file */
    const char *dst_kind;
    int64_t dst_id;
    const char *dst_path_text; /* NULL when unresolved or not a file */
    const char *dst_name_text; /* the spelling, always present for named edges */
    const char *resolution;
    const char *provenance;
    int64_t candidate_count;
    const char *detail;
    int64_t line;
    int64_t col;
} atlas_code_edge_row;

typedef atlas_status (*atlas_code_edge_cb)(const atlas_code_edge_row *row, void *ud,
                                           atlas_err *err);

/* Edges out of, or into, one node. `kind` may be NULL for every kind. */
atlas_status atlas_db_code_edges_from(atlas_db *db, int64_t repo_id, const char *src_kind,
                                      int64_t src_id, const char *kind, int64_t limit,
                                      atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                      bool *more_out, atlas_err *err);
atlas_status atlas_db_code_edges_to(atlas_db *db, int64_t repo_id, const char *dst_kind,
                                    int64_t dst_id, const char *kind, int64_t limit,
                                    atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                    bool *more_out, atlas_err *err);
atlas_status atlas_db_code_candidates_of(atlas_db *db, int64_t relation_id, int64_t limit,
                                         atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                         atlas_err *err);

/* One structurally indexed file. */
typedef struct atlas_code_file_row {
    int64_t id;
    int64_t file_id;
    const void *path_raw;
    size_t path_raw_len;
    const char *path_text;
    const char *language;
    const char *content_hash;
    const char *parse_status;
    const char *parse_detail;
    bool truncated;
    const char *truncated_reason;
    bool include_guard;
    int64_t symbol_count;
    int64_t include_count;
    int64_t occurrence_count;
    int64_t bytes;
    int64_t lines;
    int64_t generation;
} atlas_code_file_row;

typedef atlas_status (*atlas_code_file_cb)(const atlas_code_file_row *row, void *ud,
                                           atlas_err *err);
typedef struct atlas_code_role_row {
    const char *role;
    const char *basis;
    const char *resolution;
} atlas_code_role_row;
typedef atlas_status (*atlas_code_role_cb)(const atlas_code_role_row *row, void *ud,
                                           atlas_err *err);

atlas_status atlas_db_code_file_get(atlas_db *db, int64_t repo_id, const void *path_raw,
                                    size_t path_len, atlas_code_file_cb cb, void *ud, bool *found,
                                    int64_t *id_out, atlas_err *err);
atlas_status atlas_db_code_roles_of(atlas_db *db, int64_t code_file_id, atlas_code_role_cb cb,
                                    void *ud, int64_t *count_out, atlas_err *err);
/* The translation units that compile, or include, one file. */
atlas_status atlas_db_code_units_for_file(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                          int64_t limit, atlas_code_edge_cb cb, void *ud,
                                          int64_t *count_out, bool *more_out, atlas_err *err);
/* Per-file counts of unsettled edges, so a file context can report how much of
 * what it says is inferred without listing every edge. */
atlas_status atlas_db_code_file_unsettled(atlas_db *db, int64_t code_file_id, int64_t *ambiguous,
                                          int64_t *unresolved, atlas_err *err);

/* --- A4: decision documents, revisions, links and the lifecycle ledger -----
 *
 * Typed operations over the migration-6 tables. sqlite3 types stay in src/db as
 * everywhere else; the state machine and the operator channel live in
 * src/decision.
 *
 * Two things distinguish this group from every other one in this header.
 *
 * **Nothing here deletes a decision, a revision, an event or a consumed
 * challenge.** There is no `_clear` and no `_prune` for them, deliberately:
 * `atlas_db_decision_challenges_prune` removes *expired, unconsumed*
 * capabilities and nothing else, and it is the only DELETE in the group.
 *
 * **Nothing here cascades from `repositories`.** `repo_id` on a decision
 * document is a soft reference and `repo_root_hash` is the durable identity, so
 * `atlas repo remove` orphans decisions rather than destroying them and
 * `atlas_db_decision_relink_repo` reattaches them if the same root is
 * registered again. The A0 evidence restriction is untouched: nothing here
 * writes to `evidence`.
 *
 * `atlas_db_evidence_insert` still refuses everything except SOURCE and GIT,
 * and A4 writes no evidence at all. */

/* One document as a list sees it. Borrowed pointers, valid for the call only,
 * exactly like every other row callback in this header.
 *
 * `title` is model- or operator-authored prose. It is safe-encoded in the
 * database and is still UNTRUSTED_DATA: approval changes a status, not the
 * nature of the bytes. */
typedef struct atlas_decision_doc_row {
    int64_t id;
    const char *uid;
    int64_t repo_id;
    const char *created_at;
    const char *updated_at;
    const char *status; /* PROPOSED | APPROVED | REJECTED | SUPERSEDED | RESOLVED */
    /* A9.1. Which sort of knowledge record this is, from
     * `atlas_decision_kind_name`. Orthogonal to `status`, and never derived from
     * it: an APPROVED INVARIANT and an APPROVED ACCEPTED_RISK differ here and
     * nowhere else. Every document has one, and a record written before the
     * vocabulary existed reads as DECISION. */
    const char *kind;
    int64_t latest_revision_no;
    int64_t current_revision_id; /* 0 when no revision is approved */
    /* The revision a reader is shown: the approved one when there is one, the
     * newest otherwise. Both numbers are reported so "approved at revision 2,
     * revision 3 proposed" is legible without a second query. */
    int64_t head_revision_id;
    int64_t head_revision_no;
    const char *head_state;
    const char *title;        /* UNTRUSTED_DATA */
    const char *content_hash; /* of the head revision */
    const char *proposed_by;  /* the actor vocabulary */
    const char *superseded_by_uid; /* NULL unless this document was superseded */
    int64_t link_count;
} atlas_decision_doc_row;

/* One ledger event. The ledger is canonical; the status columns are a cache. */
typedef struct atlas_decision_event_row {
    int64_t id;
    int64_t revision_id;
    int64_t revision_no;
    const char *event; /* PROPOSED | APPROVED | REJECTED | SUPERSEDED | RESOLVED */
    const char *actor; /* the actor vocabulary; LOCAL_OPERATOR_CONFIRMED is a
                        * statement about a channel, never about a person */
    const char *content_hash;
    int64_t challenge_id; /* 0 when the transition consumed no capability */
    int64_t superseded_by_revision_id;
    const char *superseded_by_uid;
    const char *detail; /* a fixed Atlas vocabulary, never caller text */
    const char *created_at;
} atlas_decision_event_row;

/* One revision as a history listing sees it: identity and status, never prose
 * beyond the title. Full content is loaded on request, for one revision. */
typedef struct atlas_decision_rev_row {
    int64_t id;
    int64_t revision_no;
    const char *content_hash;
    const char *title; /* UNTRUSTED_DATA */
    const char *state;
    const char *proposed_by;
    const char *created_at;
    const char *basis_head;
    int64_t session_id;
    bool session_unbound;
    const char *unbound_reason;
    int64_t imported_from_ai_decision_id;
} atlas_decision_rev_row;

typedef atlas_status (*atlas_decision_doc_cb)(const atlas_decision_doc_row *row, void *ud,
                                              atlas_err *err);
typedef atlas_status (*atlas_decision_event_cb)(const atlas_decision_event_row *row, void *ud,
                                                atlas_err *err);
typedef atlas_status (*atlas_decision_rev_cb)(const atlas_decision_rev_row *row, void *ud,
                                              atlas_err *err);

/* --- writes (writer thread only) --- */

/* Creates a document and derives its public uid.
 *
 * The uid is derived from the row id, so it is assigned in a second statement
 * inside the caller's transaction. That is why this is not two functions: a
 * document without a uid is not a document, and leaving the window open for a
 * caller to forget is how one appears. */
/* A9.1: `kind` is supplied here and nowhere else. It is written by the INSERT
 * that creates the row and no UPDATE in this layer names the column, which is
 * what makes a document's classification as immutable as a revision's prose. */
atlas_status atlas_db_decision_document_create(atlas_db *db, int64_t repo_id,
                                               const char *root_hash, atlas_decision_kind kind,
                                               const char *created_at, int64_t *id_out,
                                               char *uid_out, size_t uid_size, atlas_err *err);
/* The document's kind, for the caller that has to decide whether an operation
 * is meaningful for it — a resolve, or a revise that asserts a different one. */
atlas_status atlas_db_decision_kind_of(atlas_db *db, int64_t document_id,
                                       atlas_decision_kind *kind_out, bool *found_out,
                                       atlas_err *err);
/* Inserts one immutable revision. `r` supplies content, `document_id`,
 * `revision_no` and `content_hash`; nothing about state is taken from it.
 *
 * When `dedup_key` is not NULL a replay collides on the partial unique index
 * and `*duplicate_out` is set, with `*id_out` carrying the existing row — the
 * same idempotency contract A2 uses. */
atlas_status atlas_db_decision_revision_insert(atlas_db *db, const atlas_decision_revision *r,
                                               const char *dedup_key, int64_t *id_out,
                                               bool *duplicate_out, atlas_err *err);
atlas_status atlas_db_decision_alternative_add(atlas_db *db, int64_t revision_id, int64_t ordinal,
                                               const char *text, size_t len, atlas_err *err);
atlas_status atlas_db_decision_link_add(atlas_db *db, int64_t revision_id,
                                        const atlas_decision_link *link, int64_t target_document_id,
                                        const char *created_at, atlas_err *err);
/* Writes the searchable projection for a revision. Derived data, and the only
 * decision row a rebuild may legitimately recreate. */
atlas_status atlas_db_decision_search_put(atlas_db *db, int64_t revision_id, int64_t document_id,
                                          int64_t repo_id, const char *haystack, size_t len,
                                          atlas_err *err);

/* Appends to the canonical ledger. Never updates, never deletes. */
atlas_status atlas_db_decision_event_append(atlas_db *db, int64_t document_id, int64_t revision_id,
                                            int64_t revision_no, const char *event,
                                            const char *actor, const char *content_hash,
                                            int64_t challenge_id,
                                            int64_t superseded_by_revision_id,
                                            int64_t superseded_by_document_id, const char *detail,
                                            const char *dedup_key, bool *inserted_out,
                                            atlas_err *err);

/* The conditional transition. `*changed_out` is false when the revision was not
 * in `from_state`, which is how a concurrent transition loses deterministically
 * instead of overwriting: the UPDATE names the expected state and the caller
 * requires that exactly one row changed. */
atlas_status atlas_db_decision_revision_set_state(atlas_db *db, int64_t revision_id,
                                                  const char *from_state, const char *to_state,
                                                  bool *changed_out, atlas_err *err);
/* Updates the cache on the document, in the same transaction as the event that
 * justifies it. `current_revision_id` of 0 stores NULL. */
atlas_status atlas_db_decision_document_set_state(atlas_db *db, int64_t document_id,
                                                  int64_t current_revision_id, const char *status,
                                                  const char *updated_at, atlas_err *err);
atlas_status atlas_db_decision_document_note_revision(atlas_db *db, int64_t document_id,
                                                      int64_t revision_no, const char *updated_at,
                                                      atlas_err *err);
atlas_status atlas_db_decision_document_set_superseded_by(atlas_db *db, int64_t document_id,
                                                          int64_t by_document_id, const char *at,
                                                          atlas_err *err);
/* The durable identity of a registered repository: a SHA-256 over the canonical
 * root path's raw bytes, the object format, **and the sorted set of root commit
 * object ids Atlas has ingested for it**.
 *
 * The lineage is the part that matters. A path is a location: delete a
 * repository, `git init` an unrelated one in the same directory, and a hash of
 * the path alone says they are the same project. The root commits say they are
 * not, and they are already in `commits` after any scan — so this needs no git
 * invocation and no new plumbing.
 *
 * `*out` is left **empty** when no root commit is recorded: an unborn HEAD, or
 * a repository whose history has not been ingested yet. An empty identity is
 * not an identity and never matches, which is the fail-closed direction. */
atlas_status atlas_db_repo_identity_hash(atlas_db *db, int64_t repo_id, atlas_buf *out,
                                         atlas_err *err);

/* Detaches every decision document currently carrying `repo_id`.
 *
 * Called unconditionally when a repository row is created, and it needs no git
 * and no history: a brand-new registration must start with no decisions
 * whatever else is true. `repositories.id` is a reused rowid, so without this a
 * new repository would inherit a removed one's approved decisions silently, in
 * every list and count.
 *
 * Detaching is not deleting. The rows keep everything except their attachment,
 * and `atlas_db_decision_orphans_list` finds them. */
atlas_status atlas_db_decision_detach_repo(atlas_db *db, int64_t repo_id, int64_t *count_out,
                                           atlas_err *err);

/* Clears the A2 origin pointer on every A4 revision that names an `ai_decisions`
 * row belonging to `repo_name`, and must be called **before** that repository is
 * deleted, while the rows it names still exist.
 *
 * `decision_revisions.imported_from_ai_decision_id` is an `ai_decisions` rowid.
 * `ai_decisions` cascades from `repositories`, so `repo remove` deletes those
 * rows — but A4 documents deliberately do not cascade, so their revisions
 * survive still holding the ids. **SQLite reuses rowids**, so the next A2 record
 * written anywhere takes an id an orphaned revision is already pointing at, and
 * two things follow, both bad: the unique index on the column rejects the
 * insert, so the next `atlas_record_decision` after a `repo remove` fails
 * outright; and if it did not, the orphan's pointer would silently resolve to an
 * unrelated repository's proposal, which is the false attribution A4 exists to
 * prevent.
 *
 * Clearing it loses nothing real. The row it named is being deleted either way,
 * and the revision already carries its own copy of the promoted content. A2's
 * rule decides the rest: an honest gap beats a plausible pointer to somebody
 * else's record. */
atlas_status atlas_db_decision_forget_legacy_origins(atlas_db *db, const char *repo_name,
                                                     int64_t *count_out, atlas_err *err);

/* Reattaches documents to a repository whose durable identity matches theirs
 * exactly.
 *
 * Matched on `repo_identity_hash` and on nothing else — never on the path
 * alone, never on a name, never on a remote URL. An empty identity on either
 * side matches nothing. Attaching is therefore fail-closed: an unchanged
 * repository re-registered and rescanned reclaims its decisions, and a
 * replaced, recloned or merely uncertain one does not.
 *
 * Called after history ingestion rather than at registration, because the
 * lineage is not knowable until Atlas has read some. */
atlas_status atlas_db_decision_relink_repo(atlas_db *db, int64_t repo_id,
                                           const char *identity_hash, int64_t *count_out,
                                           atlas_err *err);

/* Recomputes a repository's durable identity and reattaches any orphaned
 * decision documents whose recorded identity matches it exactly.
 *
 * Called after history ingestion — a scan or a reconciliation pass — because
 * the identity commits to the ingested root commits and is not knowable before
 * then. Safe to call when nothing matches, which is the normal case. */
atlas_status atlas_db_decision_relink_after_ingest(atlas_db *db, int64_t repo_id,
                                                   int64_t *count_out, atlas_err *err);

/* Decision documents attached to no live repository. Bounded, and ordered
 * newest first like every other listing.
 *
 * This exists because canonical records must not become silently invisible: a
 * detached decision is in no repository listing, and without a way to see it a
 * user who removed a repository would conclude Atlas had deleted their approval
 * history. */
atlas_status atlas_db_decision_orphans_list(atlas_db *db, int64_t limit,
                                            atlas_decision_doc_cb cb, void *ud,
                                            int64_t *count_out, bool *more_out, atlas_err *err);

/* --- the operator channel --- */

atlas_status atlas_db_decision_challenge_insert(atlas_db *db, const atlas_decision_challenge *c,
                                                int64_t *id_out, atlas_err *err);
atlas_status atlas_db_decision_challenge_find(atlas_db *db, const char *token,
                                              atlas_decision_challenge *out, bool *found_out,
                                              atlas_err *err);
/* Marks a challenge spent. `*changed_out` is false when it was already
 * consumed, which is the replay rejection: the UPDATE carries `AND consumed =
 * 0`, so two concurrent consumers cannot both see one. */
atlas_status atlas_db_decision_challenge_consume(atlas_db *db, int64_t challenge_id,
                                                 const char *at, bool *changed_out,
                                                 atlas_err *err);
/* Removes **expired, unconsumed** capabilities only. A consumed challenge is
 * part of the approval record and is referenced by the event; it is never
 * removed, and no other decision row is ever deleted by anything. */
atlas_status atlas_db_decision_challenges_prune(atlas_db *db, const char *now, int64_t retain,
                                                int64_t *removed_out, atlas_err *err);

/* --- reads --- */

/* Resolves a public uid. `*found_out` is false when nothing has it. */
atlas_status atlas_db_decision_find_uid(atlas_db *db, const char *uid, int64_t *id_out,
                                        int64_t *repo_id_out, bool *found_out, atlas_err *err);
atlas_status atlas_db_decision_uid_of(atlas_db *db, int64_t document_id, atlas_buf *out,
                                      atlas_err *err);
/* The approved, not-yet-superseded revision of a document, or 0.
 *
 * Read from the cached column rather than searched for, because the partial
 * unique index guarantees at most one and a search would imply there might be
 * several. */
atlas_status atlas_db_decision_current_revision(atlas_db *db, int64_t document_id,
                                                int64_t *revision_id_out, atlas_err *err);
/* The same fact, derived from `decision_revisions` instead of read from the
 * cache. 0 when no revision is approved.
 *
 * A9.1 needed it because a status *recomputation* that reads the cached column
 * cannot notice that the cache is now wrong — and resolving an approved revision
 * is the first operation that makes it wrong, since the revision leaves APPROVED
 * while the document still points at it. The partial unique index on
 * `state = 'APPROVED'` is what makes this a lookup rather than a choice. */
atlas_status atlas_db_decision_approved_revision(atlas_db *db, int64_t document_id,
                                                 int64_t *revision_id_out, atlas_err *err);
/* The facts a status recomputation needs besides the approved revision: which
 * document supersedes this one (0 for none), how many of its revisions are still
 * merely proposed, and how many have been resolved. */
atlas_status atlas_db_decision_document_shape(atlas_db *db, int64_t document_id,
                                              int64_t *superseded_by_out, int64_t *proposed_out,
                                              int64_t *resolved_out, atlas_err *err);
/* A9.2. The document's cached `current_status`, read rather than re-derived.
 *
 * Deliberately a read of the cache and not a second implementation of the
 * precedence: `recompute_status` in `lifecycle.c` is the single authority on
 * what a document's status is, `atlas_db_decision_verify` checks the cache
 * against the ledger replay, and a third opinion in a caller is exactly the
 * disagreement A4 warns about. A reader that wants to know the status asks for
 * the status. */
atlas_status atlas_db_decision_document_status(atlas_db *db, int64_t document_id, char *out,
                                               size_t out_size, atlas_err *err);
/* The newest revision of a document, whatever its state. */
atlas_status atlas_db_decision_latest_revision(atlas_db *db, int64_t document_id, int64_t *id_out,
                                               int64_t *no_out, char *hash_out, size_t hash_size,
                                               char *state_out, size_t state_size,
                                               atlas_err *err);
/* Loads one whole revision, with its alternatives and its links. */
atlas_status atlas_db_decision_revision_load(atlas_db *db, int64_t revision_id,
                                             atlas_decision_revision *out, bool *found_out,
                                             atlas_err *err);
atlas_status atlas_db_decision_revision_by_no(atlas_db *db, int64_t document_id, int64_t revision_no,
                                              int64_t *id_out, bool *found_out, atlas_err *err);

/* `status` and `kind` are both optional filters and both are NULL for "any".
 * They are separate parameters because they are separate dimensions: asking for
 * approved invariants is one query, and neither filter implies anything about
 * the other. */
atlas_status atlas_db_decision_documents_list(atlas_db *db, int64_t repo_id, const char *status,
                                              const char *kind, int64_t limit,
                                              atlas_decision_doc_cb cb, void *ud,
                                              int64_t *count_out, bool *more_out, atlas_err *err);
atlas_status atlas_db_decision_document_row(atlas_db *db, int64_t document_id,
                                            atlas_decision_doc_cb cb, void *ud, bool *found_out,
                                            atlas_err *err);
atlas_status atlas_db_decision_revisions_list(atlas_db *db, int64_t document_id, int64_t limit,
                                              atlas_decision_rev_cb cb, void *ud,
                                              int64_t *count_out, bool *more_out, atlas_err *err);
atlas_status atlas_db_decision_events_list(atlas_db *db, int64_t document_id, int64_t limit,
                                           atlas_decision_event_cb cb, void *ud, int64_t *count_out,
                                           bool *more_out, atlas_err *err);

/* --- the durable account of a decision-to-decision edge (migration 10) ------
 *
 * An edge lives in an immutable revision; the reason it was drawn lives here.
 * The table is append-only and keyed by the semantic edge — source document,
 * target document, kind — never by a `decision_links.id`, because a link row is
 * rewritten with a fresh id on every revision and an id-keyed reason would be
 * lost by the next revise.
 *
 * `ADDED` and `ANNOTATED` carry a rationale, `REMOVED` carries the reason an
 * edge was withdrawn. Nothing here decides whether an edge is live: the current
 * revision's links are canonical for that. Ordering is by `id`, never by a
 * timestamp. */
#define ATLAS_DECISION_EDGE_EVENT_ADDED "ADDED"
#define ATLAS_DECISION_EDGE_EVENT_ANNOTATED "ANNOTATED"
#define ATLAS_DECISION_EDGE_EVENT_REMOVED "REMOVED"

/* One row, borrowed for the duration of the call like every other row callback
 * in this header. */
typedef struct atlas_decision_edge_event_row {
    int64_t id;
    const char *source_uid;
    const char *target_uid;
    const char *kind;
    const char *event;
    const char *note; /* safe-encoded prose */
    const char *provenance;
    int64_t revision_id;
    const char *created_at;
} atlas_decision_edge_event_row;

typedef atlas_status (*atlas_decision_edge_event_cb)(const atlas_decision_edge_event_row *row,
                                                     void *ud, atlas_err *err);

atlas_status atlas_db_decision_edge_event_append(atlas_db *db, int64_t source_document_id,
                                                 int64_t target_document_id, const char *kind,
                                                 const char *event, const char *note,
                                                 const char *provenance, int64_t revision_id,
                                                 atlas_err *err);

/* The current rationale of one edge: the note on its highest-id ADDED or
 * ANNOTATED row. `found_out` is false when the edge has never been explained,
 * which is a reportable gap rather than an error. */
atlas_status atlas_db_decision_edge_rationale(atlas_db *db, int64_t source_document_id,
                                              int64_t target_document_id, const char *kind,
                                              atlas_buf *note_out, atlas_buf *provenance_out,
                                              bool *found_out, atlas_err *err);

/* Every event for one document's outgoing edges, oldest first. */
atlas_status atlas_db_decision_edge_events_list(atlas_db *db, int64_t source_document_id,
                                                int64_t limit, atlas_decision_edge_event_cb cb,
                                                void *ud, int64_t *count_out, bool *more_out,
                                                atlas_err *err);
/* Documents whose head revision links to a path, by raw bytes. `kind` is the
 * A9.1 filter and is NULL for any. */
atlas_status atlas_db_decision_for_path(atlas_db *db, int64_t repo_id, const void *path_raw,
                                        size_t path_len, const char *kind, int64_t limit,
                                        atlas_decision_doc_cb cb, void *ud, int64_t *count_out,
                                        bool *more_out, atlas_err *err);
/* Bounded search. Uses FTS5 over `decision_search` when the linked SQLite build
 * has it, and a repository-filtered scan of the same narrow table when it does
 * not. Both are bounded, and `atlas doctor` reports which is in use. */
atlas_status atlas_db_decision_search(atlas_db *db, int64_t repo_id, const char *query,
                                      const char *kind, int64_t limit, atlas_decision_doc_cb cb,
                                      void *ud, int64_t *count_out, bool *more_out, atlas_err *err);
/* Lifecycle counts for one repository, for the automatic context envelope and
 * for `decision list`. These are the real state, replacing A2's placeholder
 * zero for approvals. */
atlas_status atlas_db_decision_repo_counts(atlas_db *db, int64_t repo_id, int64_t *proposed,
                                           int64_t *approved, int64_t *rejected,
                                           int64_t *superseded, int64_t *resolved, atlas_err *err);
/* A9.1: the same repository's documents counted by knowledge kind, indexed by
 * the enum's own value so a caller cannot mismatch the order. `out` must have
 * `atlas_decision_kind_count()` elements.
 *
 * A separate function rather than a second dimension on the status counts,
 * because a cross-tabulation of five states by eight kinds is forty numbers
 * nobody asked for, and both callers want one axis at a time. */
atlas_status atlas_db_decision_kind_counts(atlas_db *db, int64_t repo_id, int64_t *out,
                                           size_t out_count, atlas_err *err);
/* Approved decisions in a repository with at least one path link whose file has
 * changed or gone since the link was recorded.
 *
 * A count, for the automatic context envelope. It is a *path* check only, and
 * deliberately: a path link's currency is one indexed lookup per link, while a
 * symbol link's needs the structural graph, and this runs on the hook path
 * where the budget is twenty milliseconds. A caller that wants the full picture
 * asks `decision.get`, which resolves every link kind. */
atlas_status atlas_db_decision_review_count(atlas_db *db, int64_t repo_id, int64_t *count_out,
                                            atlas_err *err);

/* How many approved documents link to a path, and how many of those links are
 * no longer current. Both integers, for the automatic file context. */
atlas_status atlas_db_decision_path_counts(atlas_db *db, int64_t repo_id, const void *path_raw,
                                           size_t path_len, int64_t *approved_out,
                                           int64_t *proposed_out, atlas_err *err);

/* Where a symbol name is defined, when it is defined in exactly one place.
 *
 * `*matches_out` reports the true number of definition sites; the path and hash
 * are filled in only when it is 1. That is what lets a decision's symbol
 * snapshot record a file without Atlas choosing between same-named definitions
 * — an ambiguous name gets a snapshot with no file, which resolves AMBIGUOUS
 * later, which is the honest answer. */
atlas_status atlas_db_code_symbol_definition_site(atlas_db *db, int64_t repo_id, const void *name,
                                                  size_t name_len, atlas_buf *path_raw_out,
                                                  atlas_buf *content_hash_out,
                                                  int64_t *matches_out, atlas_err *err);

/* Resolves one link's currency against the current index.
 *
 * Computed on read and never stored: a cached currency is wrong between the
 * change and the recomputation, and "is this decision still about this code?"
 * is exactly the question a stale cache must not answer. Atlas never re-points
 * a link — a rename yields MISSING and several matches yield AMBIGUOUS with the
 * count, because choosing would be inventing.
 *
 * `file_index_known` and `code_index_known` say whether the relevant index has
 * ever completed a pass for this repository. When it has not, the answer is
 * UNKNOWN rather than MISSING: "Atlas has not looked" and "it is not there" are
 * different facts. */
atlas_status atlas_db_decision_link_resolve(atlas_db *db, int64_t repo_id,
                                            atlas_decision_link *link, bool file_index_known,
                                            bool code_index_known, atlas_err *err);

/* Would making `from` superseded by `to` create a cycle?
 *
 * Walks the existing supersession chain from `to`, bounded by
 * ATLAS_DECISION_MAX_SUPERSEDE_DEPTH, and reports whether it reaches `from`. A
 * chain longer than the bound is reported as reaching, because an unbounded
 * walk over data a caller influences is not a check and the safe answer to
 * "cannot tell" is "refuse". */
atlas_status atlas_db_decision_supersede_reaches(atlas_db *db, int64_t from_document_id,
                                                 int64_t to_document_id, bool *reaches_out,
                                                 atlas_err *err);

/* Recomputes a document's status from the ledger and compares it with the
 * cached columns. Reports, never repairs: `atlas doctor` calls this and doctor
 * creates and changes nothing. `detail` receives a fixed Atlas-owned
 * description when they disagree. */
atlas_status atlas_db_decision_verify(atlas_db *db, int64_t document_id, bool *ok_out,
                                      atlas_buf *detail, atlas_err *err);
/* Every document in the database whose cache disagrees with its ledger, and
 * every revision whose stored content no longer hashes to its recorded
 * `content_hash`.
 *
 * The rehash is what makes the immutability claim checkable rather than merely
 * asserted. Every field that changes what was approved is in the canonical
 * encoding — the prose, the scope, the alternatives, the links with their whole
 * snapshot, the basis HEAD, the repository identity and the proposing actor —
 * so mutating any of them, by any route, leaves a revision that no longer
 * matches its own digest. Approval bound to that digest, so a mismatch means an
 * approval now covers bytes that are not there.
 *
 * Reported, never repaired: `atlas doctor` calls this, and a diagnostic that
 * silently fixed what it found could not tell you whether the fault recurs. */
atlas_status atlas_db_decision_verify_all(atlas_db *db, int64_t *checked_out,
                                          int64_t *mismatched_out, int64_t *rehashed_out,
                                          int64_t *corrupt_out, atlas_err *err);

/* --- A2 compatibility ---
 *
 * The A2 tables are read, never written and never altered. A legacy proposal is
 * *representable* — every read that lists decisions can report it — and
 * *promotable* into an A4 document, which creates a new revision carrying
 * `imported_from_ai_decision_id` and leaves the `ai_decisions` row exactly as
 * it was, `approved = 0` and all. Nothing rewrites history to look approved. */
typedef struct atlas_decision_legacy_row {
    int64_t id;
    int64_t session_id;
    const char *created_at;
    const char *provenance;
    const char *state;
    const char *title;
    const char *statement;
    const char *rationale;
    int64_t path_count;
    bool imported;         /* an A4 revision already carries this row's id */
    const char *imported_uid; /* which document, when it does */
} atlas_decision_legacy_row;

typedef atlas_status (*atlas_decision_legacy_cb)(const atlas_decision_legacy_row *row, void *ud,
                                                 atlas_err *err);

atlas_status atlas_db_decision_legacy_list(atlas_db *db, int64_t repo_id, bool unimported_only,
                                           int64_t limit, atlas_decision_legacy_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err);
atlas_status atlas_db_decision_legacy_get(atlas_db *db, int64_t repo_id, int64_t ai_decision_id,
                                          atlas_decision_revision *out, bool *found_out,
                                          atlas_err *err);

/* --- A6: ancestry, change ranges and the revalidation ledger -------------
 *
 * All three read through the caller's handle, so an assessment that opens one
 * read transaction and asks all of these gets one coherent snapshot rather than
 * four reads that happened to run close together. */

/* Whether Atlas' ingested history reaches a commit from another.
 *
 * The three non-answers are kept apart on purpose. LIMIT and UNKNOWN both mean
 * Atlas stopped before it could tell; NOT_ANCESTOR is the only value that
 * asserts something, and it is only ever produced when the walk exhausted every
 * reachable commit without meeting a parent it had never ingested. Collapsing
 * them would turn "we do not hold that much history" into "your history was
 * rewritten", which is a very different thing to tell somebody. */
typedef enum atlas_db_gate_ancestry_verdict {
    ATLAS_DB_GATE_ANCESTRY_UNKNOWN = 0,
    ATLAS_DB_GATE_ANCESTRY_REACHED,
    ATLAS_DB_GATE_ANCESTRY_NOT_ANCESTOR,
    ATLAS_DB_GATE_ANCESTRY_LIMIT
} atlas_db_gate_ancestry_verdict;

typedef struct atlas_db_gate_ancestry_result {
    atlas_db_gate_ancestry_verdict verdict;
    int64_t visited;
    /* True when the walk named a parent commit Atlas has not ingested. */
    bool missing_parent;
} atlas_db_gate_ancestry_result;

atlas_status atlas_db_gate_ancestry(atlas_db *db, int64_t repo_id, const char *head_oid,
                                    const char *target_oid, atlas_db_gate_ancestry_result *out,
                                    atlas_err *err);

/* A set of repository paths, by exact bytes. Sorted, so membership and
 * "anything under this prefix" are both binary searches rather than passes. */
typedef struct atlas_db_gate_path {
    unsigned char *bytes;
    size_t len;
} atlas_db_gate_path;

typedef struct atlas_db_gate_paths {
    atlas_db_gate_path *items;
    size_t count;
    size_t cap;
} atlas_db_gate_paths;

void atlas_db_gate_paths_init(atlas_db_gate_paths *p);
void atlas_db_gate_paths_free(atlas_db_gate_paths *p);
atlas_status atlas_db_gate_paths_add(atlas_db_gate_paths *p, const void *bytes, size_t len,
                                     atlas_err *err);
bool atlas_db_gate_paths_has(const atlas_db_gate_paths *p, const void *bytes, size_t len);
/* True when the set holds `prefix` itself or anything below it. A path
 * component boundary is required, so `src` does not match `srcfoo`. */
bool atlas_db_gate_paths_has_prefix(const atlas_db_gate_paths *p, const void *prefix,
                                    size_t prefix_len);

/* The paths touched between `stop_oid` (exclusive) and `head_oid` (inclusive).
 *
 * `limit_reached` is the field a caller must check before trusting a negative
 * membership test: a set that stopped being collected is a set every lookup
 * misses for two different reasons. */
typedef struct atlas_db_gate_range {
    atlas_db_gate_paths paths;
    int64_t commits;
    bool limit_reached;
    /* True when the walk reached a commit Atlas has not ingested, so the range
     * may be missing whole branches of itself. */
    bool missing_commit;
} atlas_db_gate_range;

atlas_status atlas_db_gate_range_paths(atlas_db *db, int64_t repo_id, const char *head_oid,
                                       const char *stop_oid, atlas_db_gate_range *out,
                                       atlas_err *err);
void atlas_db_gate_range_free(atlas_db_gate_range *r);

/* One revalidation record. Append-only: there is no update and no delete for
 * this table anywhere in Atlas, and there must not be one. */
typedef struct atlas_db_gate_validation {
    int64_t id;
    int64_t document_id;
    int64_t revision_id;
    int64_t revision_no;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    int64_t repo_id;
    char repo_identity_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char validated_at_commit[ATLAS_OID_HEX_MAX_INCL];
    char evidence_digest[ATLAS_SHA256_HEX_LEN + 1u];
    int64_t challenge_id;
    /* The assessment the operator was shown, preserved rather than recomputed.
     * Closed Atlas vocabularies; see atlas/gate.h. */
    char prior_freshness[16];
    char prior_reasons[ATLAS_GATE_MAX_REASON_TEXT];
    char created_at[ATLAS_TS_MAX];
} atlas_db_gate_validation;

typedef atlas_status (*atlas_db_gate_validation_cb)(const atlas_db_gate_validation *v, void *ud,
                                                    atlas_err *err);

atlas_status atlas_db_gate_validation_insert(atlas_db *db, const atlas_db_gate_validation *v,
                                             int64_t *id_out, atlas_err *err);
/* The newest revalidation of one revision *within one repository identity*. A
 * revalidation performed against one worktree does not establish a validation
 * point for another, because the two are at different commits by construction. */
atlas_status atlas_db_gate_validation_newest(atlas_db *db, int64_t revision_id,
                                             const char *repo_identity_hash,
                                             atlas_db_gate_validation *out, bool *found_out,
                                             atlas_err *err);
atlas_status atlas_db_gate_validation_count(atlas_db *db, int64_t revision_id,
                                            const char *repo_identity_hash, int64_t *count_out,
                                            atlas_err *err);
atlas_status atlas_db_gate_validations_for_document(atlas_db *db, int64_t document_id,
                                                    int64_t limit,
                                                    atlas_db_gate_validation_cb cb, void *ud,
                                                    atlas_err *err);
/* Structural checks over the ledger, for `atlas doctor`. Appends a description
 * of anything wrong to `out` and leaves it untouched when nothing is. Reports,
 * never repairs — and deliberately does not re-derive evidence digests against
 * the live index, because those are meant to drift. */
atlas_status atlas_db_gate_verify(atlas_db *db, atlas_buf *out, atlas_err *err);

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

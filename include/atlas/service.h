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
#include "atlas/reconcile.h"
#include "atlas/scan.h"

typedef struct atlas_ctx atlas_ctx;

/* How a context intends to use the index.
 *
 * Exactly one process may write at a time, enforced by the data-directory lock
 * (see atlas/lock.h). The mode is what decides whether this process is trying to
 * be that one. */
typedef enum atlas_ctx_mode {
    /* Take the writer lock if it is free; fall back to a read-only handle if
     * something else — normally the daemon — already holds it. This is what read
     * commands use: they work whether or not a daemon is running, and a first
     * run still gets to create and migrate the database. */
    ATLAS_CTX_AUTO = 0,
    /* Never take the lock. A read-only handle, and a schema mismatch is an error
     * rather than something to migrate away. */
    ATLAS_CTX_READ,
    /* Observe without creating anything.
     *
     * The data directory is not created, the database file is not created, no
     * lock is taken and no migration runs. A missing data directory or database
     * is reported through `atlas_ctx_index_present()` rather than being
     * conjured into existence.
     *
     * This exists because a diagnostic that initialises state is not a
     * diagnostic. `atlas doctor` used to create `~/.local/share/atlas` and an
     * empty index as a side effect of asking whether one was healthy, which
     * meant the answer was always yes and the question could not be asked at
     * all on a machine where Atlas had never run. */
    ATLAS_CTX_INSPECT,
    /* Take the writer lock or fail. Mutating CLI commands use this, so that a
     * command run by hand while the daemon owns the index is refused with an
     * explanation instead of racing it. */
    ATLAS_CTX_WRITE
} atlas_ctx_mode;

typedef struct atlas_ctx_opts {
    const char *data_dir_override; /* --data-dir; NULL to resolve from env */
    atlas_ctx_mode mode;
} atlas_ctx_opts;

atlas_status atlas_ctx_open(const atlas_ctx_opts *opts, atlas_ctx **out, atlas_err *err);
void atlas_ctx_close(atlas_ctx *ctx);
const char *atlas_ctx_data_dir(const atlas_ctx *ctx);
const char *atlas_ctx_db_path(const atlas_ctx *ctx);
atlas_datadir_source atlas_ctx_data_dir_source(const atlas_ctx *ctx);
atlas_db *atlas_ctx_db(atlas_ctx *ctx);
/* True when this context holds the writer lock. A context that does not hold it
 * cannot mutate the index, and says so rather than failing at the first write. */
bool atlas_ctx_is_writer(const atlas_ctx *ctx);
/* False when the context was opened in ATLAS_CTX_INSPECT mode and there was no
 * database to open. `atlas_ctx_db` returns NULL in that case, and a caller
 * reports the absence rather than dereferencing it. */
bool atlas_ctx_index_present(const atlas_ctx *ctx);
/* True when the data directory itself does not exist. Distinguished from a
 * missing database because "Atlas has never run here" and "the index was
 * deleted" are different things to tell somebody. */
bool atlas_ctx_data_dir_present(const atlas_ctx *ctx);

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
    /* Whether there was anything to inspect. Both false on a machine where
     * Atlas has never run, which is a finding rather than a fault: `doctor`
     * reports the absence instead of creating an index in order to have one. */
    bool data_dir_present;
    bool index_present;
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

/* --- A1: repository mutations at the database level ---------------------
 *
 * The daemon's writer thread owns a bare atlas_db, not an atlas_ctx: the context
 * owns the data-directory lock, and the daemon already holds that for its whole
 * lifetime. These are the same operations the ctx-level functions perform, with
 * the lock ownership left to the caller. The ctx-level functions are thin
 * wrappers over them, so there is one implementation of "register a repository"
 * rather than two that can drift. */
/* `exact_root` refuses to register when `path` is not itself the worktree root.
 *
 * A repository path normally resolves upward: `atlas repo add src/` registers
 * the whole worktree, which is what a person means. An MCP client granting a
 * root means something narrower — *this* directory — so registering its parent
 * would index files outside what was granted. The MCP adapter therefore asks for
 * the exact form; the CLI and the session-start hook do not. */
atlas_status atlas_service_repo_add_db(atlas_db *db, const char *path, const char *name,
                                       bool exact_root, atlas_repo_info *out, atlas_err *err);
atlas_status atlas_service_repo_remove_db(atlas_db *db, const char *name, atlas_repo_info *removed,
                                          atlas_err *err);

/* --- A1: daemon-facing reports ------------------------------------------ */

typedef struct atlas_daemon_status_report {
    bool running;             /* something holds the writer lock right now */
    bool reachable;           /* the socket answers */
    atlas_buf socket_path;
    atlas_buf lock_holder;    /* diagnostic text recorded by the lock holder */
    atlas_daemon_record record;
    int64_t repo_count;
    int64_t watched_repos;
    int64_t degraded_repos;   /* degraded, incomplete or error */
    int64_t repos_with_gap;   /* an unresolved event gap: NOT current */
    int protocol_version;
    atlas_buf atlas_version;
} atlas_daemon_status_report;

void atlas_daemon_status_report_init(atlas_daemon_status_report *r);
void atlas_daemon_status_report_free(atlas_daemon_status_report *r);

/* Assembles the report from the index and the lock, without contacting the
 * daemon. Works whether or not a daemon is running, which is what makes
 * `atlas daemon status` useful when it is not. */
atlas_status atlas_service_daemon_status(atlas_ctx *ctx, atlas_daemon_status_report *out,
                                         atlas_err *err);

/* One repository's continuous-index state, as `atlas events` and `atlas sync`
 * report it. */
typedef struct atlas_repo_state_report {
    atlas_repo_info repo;
    atlas_index_state state;
    int64_t event_cursor; /* newest event id */
    /* The honest summary: true only when a completed generation exists AND no
     * event gap is outstanding. */
    bool index_current;
    const char *not_current_reason; /* NULL when index_current */
} atlas_repo_state_report;

void atlas_repo_state_report_init(atlas_repo_state_report *r);
void atlas_repo_state_report_free(atlas_repo_state_report *r);

atlas_status atlas_service_repo_state(atlas_ctx *ctx, const char *name,
                                      atlas_repo_state_report *out, atlas_err *err);

/* Streams the durable event journal from `since`, exclusive. */
atlas_status atlas_service_events(atlas_ctx *ctx, const char *name, int64_t since, int64_t limit,
                                 atlas_event_cb cb, void *ud, int64_t *count_out,
                                 int64_t *next_cursor_out, bool *more_out, atlas_err *err);

/* --- A1: sync ------------------------------------------------------------ */

typedef struct atlas_sync_report {
    bool via_daemon;      /* the daemon performed it, rather than this process */
    bool waited;
    bool completed;       /* --wait: the requested pass finished */
    int64_t sync_seq;
    int64_t generation;
    atlas_reconcile_summary summary; /* only meaningful when !via_daemon */
} atlas_sync_report;

void atlas_sync_report_init(atlas_sync_report *r);
void atlas_sync_report_free(atlas_sync_report *r);

/* Requests reconciliation. When a daemon is running the request is routed to it;
 * otherwise this process performs the pass itself, holding the writer lock.
 * `wait` polls for completion, bounded by `timeout_ms`. */
atlas_status atlas_service_sync(atlas_ctx *ctx, const char *name, bool full, bool wait,
                                int timeout_ms, atlas_sync_report *out, atlas_err *err);

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

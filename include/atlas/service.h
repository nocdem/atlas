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
#include <stdio.h>

#include "atlas/authority.h"
#include "atlas/backup.h"
#include "atlas/code.h"
#include "atlas/orch_usage.h"
#include "atlas/plan.h"
#include "atlas/sem.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_schedule.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/gate.h"
#include "atlas/json.h"
#include "atlas/safetext.h"
#include "atlas/verify_ops.h"
#include "atlas/verifypolicy.h"
#include "atlas/git.h"
#include "atlas/ipc.h"
#include "atlas/limits.h"
#include "atlas/reconcile.h"
#include "atlas/review.h"
#include "atlas/scan.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"

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
    /* A7.1. True when a data directory or index exists that this process may
     * not read — the correct state of a separated deployment seen from the
     * operator's account, and a different fact from "there is no index". Not a
     * problem, and it does not affect `ok`; reported so the two are
     * distinguishable in the one command that is run to tell them apart. */
    bool index_unreadable;
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
    /* A7. Which operations this profile will let anyone perform, and why.
     *
     * Reported by `doctor` because a lock nobody can see is a lock nobody
     * trusts: the first thing an operator does when `decision approve` refuses
     * is ask what is wrong with Atlas, and this is the answer. Never a
     * *finding* — a locked profile is the correct and expected state of an
     * unseparated machine, not a fault — so it does not affect `ok`. */
    atlas_authority_state authority_state;
    atlas_authority_reason authority_reason;
    /* A7.1. Which deployment this is, and why. Reported beside the authority
     * profile because the two answer the same operator question — "what is this
     * Atlas allowed to do, and for whom?" — and because a system deployment
     * that silently fell back to per-user mode would otherwise look identical. */
    atlas_syspolicy_state deployment_state;
    atlas_syspolicy_reason deployment_reason;
    /* A13. How many registered repositories have no scanner uid. Each is named
     * in `problems`: a count nobody can act on is not a diagnosis. */
    int repos_without_scanner;
    bool ok;                  /* no blocking problem found */
    atlas_buf problems;       /* newline-separated, empty when ok */
} atlas_doctor_report;

void atlas_doctor_report_init(atlas_doctor_report *r);
void atlas_doctor_report_free(atlas_doctor_report *r);
atlas_status atlas_service_doctor(atlas_ctx *ctx, atlas_doctor_report *out, atlas_err *err);

/* --- repositories ------------------------------------------------------- */

/* Derives the scanner uid from the repository root's owner. Use
 * `atlas_service_repo_add_as` to name one instead. */
atlas_status atlas_service_repo_add(atlas_ctx *ctx, const char *path, const char *name,
                                    atlas_repo_info *out, atlas_err *err);
/* As above, but with the operator's own choice of scanner uid. */
atlas_status atlas_service_repo_add_as(atlas_ctx *ctx, const char *path, const char *name,
                                       bool scanner_uid_given, int64_t scanner_uid,
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
/* The same two reads over a bare handle, for the daemon's threads. One
 * implementation each, called by both the CLI and the method that answers it
 * over the socket. */
atlas_status atlas_service_file_db(atlas_db *db, int64_t repo_id, const char *name,
                                   const char *path, atlas_file_report_cb cb, void *ud,
                                   atlas_err *err);
atlas_status atlas_service_history_db(atlas_db *db, int64_t repo_id, const char *path,
                                      int64_t limit, atlas_history_cb cb, void *ud,
                                      int64_t *count_out, atlas_err *err);

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
/* A13. `scanner_uid_given` false derives the scanner uid from the repository
 * root's owner; true takes `scanner_uid` as the operator's own choice. The flag
 * is separate from the value because 0 already means "no scanner assigned" in
 * the column and cannot also mean "derive one". A uid that may never scan fails
 * the registration rather than being stored, so a refusal never reads as an
 * absence. `out->scanner_uid` carries what was stored. */
atlas_status atlas_service_repo_add_db(atlas_db *db, const char *path, const char *name,
                                       bool exact_root, bool scanner_uid_given,
                                       int64_t scanner_uid, atlas_repo_info *out, atlas_err *err);
atlas_status atlas_service_repo_remove_db(atlas_db *db, const char *name, atlas_repo_info *removed,
                                          atlas_err *err);

/* A13. Assigns `name`'s scanner uid without re-registering it, which is the
 * only path a repository registered before A13 has: migration 27 could not
 * `stat` a root, so it left every existing repository unassigned.
 *
 * Derives from the repository root's owner when `uid_given` is false. The same
 * refusals apply as at registration, and a refusal leaves the stored value
 * alone — a repository that had a working scanner must not lose one because a
 * later command named something impossible. `out` is filled by re-reading the
 * row, so the caller sees what is stored rather than what was asked for. */
atlas_status atlas_service_repo_set_scanner(atlas_ctx *ctx, const char *name, bool uid_given,
                                            int64_t uid, atlas_repo_info *out, atlas_err *err);
atlas_status atlas_service_repo_set_scanner_db(atlas_db *db, const char *name, bool uid_given,
                                               int64_t uid, atlas_repo_info *out, atlas_err *err);

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
    /* P0. Watches are still being installed. Neither watched nor degraded: not
     * the first because part of the tree is not yet observed, not the second
     * because nothing is wrong. Counting it as either would make one of the two
     * numbers a lie during every ordinary startup. */
    int64_t priming_repos;
    int64_t repos_with_gap;   /* an unresolved event gap: NOT current */
    /* P0. The watch budget and the arithmetic that produced it, so an operator
     * looking at a degraded repository can see whether Atlas chose the limit or
     * the kernel did. Zero means the daemon did not report them — an older
     * daemon, or a local read with no daemon running — never that they are zero. */
    int64_t watches;              /* physical inotify descriptors */
    int64_t watch_subscriptions;  /* (repository, descriptor) pairs; >= watches */
    int64_t watch_budget_total;
    int64_t watch_budget_repo;
    int64_t kernel_max_user_watches;
    bool watch_budget_from_policy;
    bool priming_complete;
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

/* A7.1: the same two reads, answered by the daemon over the socket.
 *
 * They take no `atlas_ctx` on purpose — under a system deployment the index is
 * 0700 `atlasd` and a client uid cannot open it, so there is nothing for a
 * context to hold. Results land in the same report structs the local reads fill,
 * so both renderers are unchanged. There is no fallback to a local read: a
 * client that cannot reach the daemon fails, rather than quietly answering from
 * the pre-cutover per-user database. */
atlas_status atlas_service_repo_list_remote(atlas_repo_cb cb, void *ud, int64_t *count_out,
                                            atlas_err *err);
atlas_status atlas_service_repo_state_remote(const char *name, atlas_repo_state_report *out,
                                             atlas_err *err);
atlas_status atlas_service_status_remote(const char *name, atlas_status_report *out,
                                         atlas_err *err);

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

/* --- A3: structural code intelligence ------------------------------------
 *
 * The CLI orchestrates these section by section rather than one call answering
 * everything, because a file context is several independent lists and a single
 * function returning all of them would have to buffer the lot. Each one is a
 * bounded query; the renderer streams what comes back.
 *
 * Every one of them refuses when the structural index is not current? No — they
 * answer, and they report that it is not. Refusing would leave a caller with
 * nothing when it has stale-but-labelled facts, which is worse. */

typedef struct atlas_code_status_report {
    atlas_repo_info repo;
    atlas_index_state file_state;
    atlas_code_index_state code_state;
    bool file_index_current;
    bool code_index_current;
    /* One of the fixed Atlas strings, or NULL when current. */
    const char *not_current_reason;
} atlas_code_status_report;

void atlas_code_status_report_init(atlas_code_status_report *r);
void atlas_code_status_report_free(atlas_code_status_report *r);

atlas_status atlas_service_code_status(atlas_ctx *ctx, const char *name,
                                       atlas_code_status_report *out, atlas_err *err);

/* One role, as reported. All three values come from fixed vocabularies. */
typedef struct atlas_code_role_entry {
    char role[24];
    char basis[24];
    char resolution[20];
} atlas_code_role_entry;

typedef struct atlas_code_file_report {
    bool indexed;
    atlas_buf path_text;
    char language[16];
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char parse_status[16];
    atlas_buf parse_detail;
    atlas_buf truncated_reason;
    bool truncated;
    bool include_guard;
    int64_t code_file_id;
    int64_t symbol_count;
    int64_t include_count;
    int64_t occurrence_count;
    int64_t bytes;
    int64_t lines;
    int64_t generation;
    /* How much of what this file says is inferred rather than established, so a
     * caller can weigh the answer without listing every edge. */
    int64_t ambiguous;
    int64_t unresolved;
    atlas_code_role_entry roles[ATLAS_CODE_MAX_ROLES_PER_FILE];
    size_t role_count;
    /* The repository's structural currency, carried here so a file context is
     * self-describing rather than needing a second call to be trustworthy. */
    bool code_index_current;
    const char *not_current_reason;
} atlas_code_file_report;

void atlas_code_file_report_init(atlas_code_file_report *r);
void atlas_code_file_report_free(atlas_code_file_report *r);

atlas_status atlas_service_code_file(atlas_ctx *ctx, const char *name, const char *path,
                                     atlas_code_file_report *out, atlas_err *err);
/* Symbols defined or declared in one file, in source order. */
atlas_status atlas_service_code_file_symbols(atlas_ctx *ctx, const char *name, const char *path,
                                             int64_t limit, atlas_code_symbol_cb cb, void *ud,
                                             int64_t *count_out, bool *more_out, atlas_err *err);
/* Edges of one kind leaving or entering a file. `inbound` selects the
 * destination index rather than the source one, which is the whole difference
 * between "what does this include" and "what includes this". */
atlas_status atlas_service_code_file_edges(atlas_ctx *ctx, const char *name, const char *path,
                                           const char *kind, bool inbound, int64_t limit,
                                           atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                           bool *more_out, atlas_err *err);
/* A bounded substring search over indexed symbol names. `kind` may be NULL. */
atlas_status atlas_service_code_symbol_search(atlas_ctx *ctx, const char *name, const char *query,
                                              const char *kind, int64_t limit,
                                              atlas_code_symbol_cb cb, void *ud,
                                              int64_t *count_out, bool *more_out, atlas_err *err);
/* Every recorded site for one exact symbol name. Several rows is the normal
 * answer, not an error: two files' identically named statics are two symbols. */
atlas_status atlas_service_code_symbol_sites(atlas_ctx *ctx, const char *name, const char *symbol,
                                             int64_t limit, atlas_code_symbol_cb cb, void *ud,
                                             int64_t *count_out, bool *more_out, atlas_err *err);
/* Edges into or out of every site of one symbol name — its callers and its
 * callees. */
atlas_status atlas_service_code_symbol_edges(atlas_ctx *ctx, const char *name, const char *symbol,
                                             bool inbound, int64_t limit, atlas_code_edge_cb cb,
                                             void *ud, int64_t *count_out, bool *more_out,
                                             atlas_err *err);

/* Bounded traversal from a path or a symbol.
 *
 * Exactly one of `path` and `symbol` is given. `inbound` answers "what may be
 * affected if this changes"; outbound answers "what does this depend on". The
 * result is a set of graph paths and says so; it is never a claim that anything
 * will break. */
atlas_status atlas_service_code_walk(atlas_ctx *ctx, const char *name, const char *path,
                                     const char *symbol, bool inbound, int64_t depth,
                                     int64_t limit, atlas_code_walk_cb cb, void *ud,
                                     atlas_code_walk_summary *sum, atlas_err *err);

/* Requests a structural reindex. Routed to the daemon when one is running, as
 * every other mutation is, so the single writer stays single. `rebuild` drops
 * every structural row first. */
atlas_status atlas_service_code_sync(atlas_ctx *ctx, const char *name, bool rebuild, bool wait,
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
/* The same observation given a repository row, so the daemon-served form runs
 * one implementation rather than a second copy. */
atlas_status atlas_service_diff_repo(const atlas_repo_info *info, const atlas_diff_opts *opts,
                                     atlas_diff_entry_cb cb, void *ud, atlas_diff_report *report,
                                     atlas_err *err);

/* --- A4: decision documents ------------------------------------------------
 *
 * All A4 command behaviour lives in src/core/service_decision.c. The CLI parses
 * arguments and picks a renderer; the renderers format what these produce.
 *
 * Reads go straight to the index on the calling thread, like every other read
 * command. **Every write is routed to the daemon's writer thread** when one is
 * answering, and taken on this thread only when this process holds the
 * data-directory lock — which is the same rule `atlas code sync --rebuild`
 * follows, and for the same reason: exactly one process writes the index. */

/* What a listing shows for one decision. Owned buffers.
 *
 * `title` is model- or operator-authored prose. It arrives already safe-encoded
 * from the daemon, or is encoded by the renderer when it came from the local
 * index; either way it stays UNTRUSTED_DATA, because approval changes a
 * record's status and not the nature of its bytes. */
typedef struct atlas_decision_summary {
    atlas_buf uid;
    atlas_buf status;
    /* A9.1. Which sort of knowledge record this is, from the closed
     * `atlas_decision_kind` vocabulary. Carried beside `status` and never folded
     * into it: a reader has to be able to tell an approved invariant from an
     * approved accepted risk, and one field cannot say both. */
    atlas_buf kind;
    atlas_buf revision_state;
    atlas_buf title;
    atlas_buf content_hash;
    atlas_buf proposed_by;
    atlas_buf superseded_by;
    atlas_buf created_at;
    atlas_buf updated_at;
    int64_t revision_no;
    int64_t latest_revision_no;
    int64_t link_count;
} atlas_decision_summary;

void atlas_decision_summary_init(atlas_decision_summary *s);
void atlas_decision_summary_free(atlas_decision_summary *s);

/* The counts a listing reports alongside its page.
 *
 * Two axes, counted separately rather than cross-tabulated: five states and
 * eight kinds is forty numbers, and every caller wants one axis at a time. */
typedef struct atlas_decision_counts {
    int64_t proposed;
    int64_t approved;
    int64_t rejected;
    int64_t superseded;
    int64_t resolved;
    /* Indexed by `atlas_decision_kind`, so `by_kind[ATLAS_DECISION_KIND_OBLIGATION]`
     * is the obligation count whatever order a query returned rows in. */
    int64_t by_kind[ATLAS_DECISION_KIND_MAX];
} atlas_decision_counts;

/* One resolved link, ready to render. */
typedef struct atlas_decision_link_view {
    atlas_buf kind;
    atlas_buf value;    /* the path, commit, symbol or target id, safe-encoded */
    atlas_buf detail;   /* the symbol's file or kind, when there is one */
    atlas_buf currency; /* CURRENT | CHANGED | MISSING | AMBIGUOUS | UNKNOWN */
    atlas_buf analyzer;
    /* Migration 10, and only ever set for a `relates_to` link: the durable
     * reason this relation exists, safe-encoded, and where that reason came
     * from. Empty when nobody has explained the edge, which is a reportable
     * gap rather than an error. It is read from `decision_edge_events` rather
     * than from the revision, because a reason recorded after an approval was
     * not part of what was approved and is not covered by its content hash. */
    atlas_buf rationale;
    atlas_buf rationale_provenance;
    int64_t analyzer_version;
    int64_t matches;
} atlas_decision_link_view;

void atlas_decision_link_view_init(atlas_decision_link_view *v);
void atlas_decision_link_view_free(atlas_decision_link_view *v);

/* A whole decision, for `decision show` and `decision export`. */
typedef struct atlas_decision_document {
    atlas_decision_summary summary;
    atlas_buf repo;
    atlas_buf context_text;
    atlas_buf decision_text;
    atlas_buf rationale_text;
    atlas_buf consequences_text;
    atlas_buf scope;
    atlas_buf basis_head;
    /* The repository identity this revision captured when it was written, or
     * empty when none was knowable then. Immutable, hashed, and deliberately
     * distinct from the document's current attachment identity. */
    atlas_buf basis_repo_identity;
    atlas_buf unbound_reason;
    atlas_buf alternatives[ATLAS_DECISION_MAX_ALTERNATIVES];
    size_t alternative_count;
    atlas_decision_link_view links[ATLAS_DECISION_MAX_LINKS];
    size_t link_count;
    int64_t links_needing_review;
    int64_t imported_from_a2_decision;
    bool session_unbound;
    /* Whether Atlas has looked. A link's currency is UNKNOWN rather than
     * MISSING when the relevant index has never completed a pass, and a reader
     * has to be able to tell those apart. */
    bool file_index_known;
    bool code_index_known;
    /* Whether the cached status agrees with the append-only ledger. Reported,
     * never repaired. */
    bool ledger_agrees;
} atlas_decision_document;

void atlas_decision_document_init(atlas_decision_document *d);
void atlas_decision_document_free(atlas_decision_document *d);

/* One entry in a document's timeline. Borrowed for the callback only. */
typedef struct atlas_decision_timeline_entry {
    const char *event;
    const char *actor;
    const char *content_hash;
    const char *superseded_by;
    const char *detail;
    const char *at;
    int64_t revision_no;
    bool operator_channel;
    /* A16, migration 31. NULL for every transition but a
     * REMOTE_OPERATOR_CONFIRMED one, mirroring `atlas_decision_event_row`'s
     * own field: the sixteen-hex id of the credential the gateway presented,
     * once the daemon has verified it -- an id, not a secret. */
    const char *key_id;
} atlas_decision_timeline_entry;

/* What a lifecycle write reports back. */
typedef struct atlas_decision_outcome {
    atlas_buf repo;
    atlas_buf uid;
    atlas_buf state;
    /* A9.1. The record's kind, echoed by every write so a caller that proposed
     * without naming one is told what it created rather than having to read the
     * record back. Beside `state`, never folded into it. */
    atlas_buf kind;
    atlas_buf replaced_by;
    atlas_buf unbound_reason;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    int64_t revision_no;
    int64_t superseded_revision_no;
    bool created;
    bool duplicate;
    bool session_unbound;
    bool via_daemon;
    /* True for a transition the operator channel authorised. It means the
     * channel was used; it does not identify a person. */
    bool operator_confirmed;
    /* Migration 10, and only meaningful for `decision link remove`. `is_removal`
     * says the answer is about a withdrawal at all, so the renderers emit
     * `removed` for that command and for no other — a `removed: false` on an
     * unrelated command would read as a claim that something was not deleted. */
    bool is_removal;
    bool removed;
} atlas_decision_outcome;

void atlas_decision_outcome_init(atlas_decision_outcome *o);
void atlas_decision_outcome_free(atlas_decision_outcome *o);

/* What a proposal carries in from the command line. Every field is validated
 * before it leaves the CLI layer. */
typedef struct atlas_decision_input {
    const char *title;
    const char *context_text;
    const char *decision_text;
    const char *rationale_text;
    const char *consequences_text;
    const char *scope;
    /* A9.1. The knowledge kind, or NULL for "the caller said nothing" — which
     * means DECISION on a propose and "do not check" on a revise. An
     * unrecognised name is a usage error rather than a silent default. */
    const char *kind;
    const char *const *alternatives;
    size_t alternative_count;
    const char *const *paths;
    size_t path_count;
    const char *const *commits;
    size_t commit_count;
    const char *const *symbols;
    size_t symbol_count;
    /* Decision uids this decision relates to. A general reference: the revision
     * holding them is the source and each uid is a target, and no lifecycle
     * rule reads them. */
    const char *const *decision_links;
    size_t decision_link_count;
    const char *dedup_key;
} atlas_decision_input;

typedef atlas_status (*atlas_decision_summary_cb)(const atlas_decision_summary *s, void *ud,
                                                  atlas_err *err);
typedef atlas_status (*atlas_decision_timeline_cb)(const atlas_decision_timeline_entry *e, void *ud,
                                                   atlas_err *err);

/* Which listing. One function rather than four, because they differ only in the
 * predicate and four would be four places for the projection to drift. */
typedef enum atlas_decision_list_mode {
    ATLAS_DECISION_LIST_ALL = 0,
    ATLAS_DECISION_LIST_STATUS,
    ATLAS_DECISION_LIST_SEARCH,
    ATLAS_DECISION_LIST_PATH
} atlas_decision_list_mode;

typedef struct atlas_decision_list_opts {
    atlas_decision_list_mode mode;
    const char *status; /* LIST_STATUS */
    const char *query;  /* LIST_SEARCH */
    const char *path;   /* LIST_PATH, in the safe text encoding */
    /* A9.1. An optional knowledge-kind filter, honoured in every mode, NULL for
     * any. It is not a mode of its own: filtering by kind is orthogonal to
     * filtering by status, by text and by path, so making it a mode would have
     * made "approved invariants" inexpressible. */
    const char *kind;
    int64_t limit;
} atlas_decision_list_opts;

atlas_status atlas_service_decision_list(atlas_ctx *ctx, const char *repo,
                                         const atlas_decision_list_opts *opts,
                                         atlas_decision_summary_cb cb, void *ud,
                                         atlas_decision_counts *counts, int64_t *count_out,
                                         bool *more_out, atlas_err *err);
/* One whole decision. `revision_no` of 0 means the effective revision — the
 * approved one when there is one, the newest otherwise. */
atlas_status atlas_service_decision_show(atlas_ctx *ctx, const char *repo, const char *uid,
                                         int64_t revision_no, atlas_decision_document *out,
                                         atlas_err *err);
atlas_status atlas_service_decision_history(atlas_ctx *ctx, const char *repo, const char *uid,
                                            atlas_decision_summary_cb rev_cb,
                                            atlas_decision_timeline_cb event_cb, void *ud,
                                            bool *ledger_agrees_out, atlas_err *err);

/* --- the account of one decision's relations (migration 10) ----------------
 *
 * Every event ever recorded about this document's outgoing edges, oldest
 * first: why each was drawn, any later correction, and why any was withdrawn.
 * It is a separate ledger from `decision_events` and is read separately,
 * because they record different kinds of act and merging them would make an
 * edge annotation look like a lifecycle transition.
 *
 * `active` says whether the current revision still asserts the edge. It is
 * computed on read from the revision's links, never stored — the revision is
 * canonical for what is live, and a cached flag would be a second answer to a
 * question that already has one. So a withdrawn edge appears here with its
 * whole history and `active` false, which is what makes "it existed and this
 * is why it no longer does" answerable at all. */
typedef struct atlas_decision_edge_entry {
    int64_t id; /* the append-only order; never a timestamp */
    const char *target;
    const char *kind;
    const char *event;      /* ADDED | ANNOTATED | REMOVED */
    const char *note;       /* safe-encoded prose */
    const char *provenance; /* a fixed Atlas vocabulary */
    const char *created_at;
    int64_t revision_id;
    bool active;
} atlas_decision_edge_entry;

typedef atlas_status (*atlas_decision_edge_cb)(const atlas_decision_edge_entry *e, void *ud,
                                               atlas_err *err);

atlas_status atlas_service_decision_links(atlas_ctx *ctx, const char *repo, const char *uid,
                                          atlas_decision_edge_cb cb, void *ud, int64_t *count_out,
                                          bool *more_out, atlas_err *err);
atlas_status atlas_service_decision_links_remote(const char *repo, const char *uid,
                                                 atlas_decision_edge_cb cb, void *ud,
                                                 int64_t *count_out, bool *more_out,
                                                 atlas_err *err);

atlas_status atlas_service_decision_propose(atlas_ctx *ctx, const char *repo,
                                            const atlas_decision_input *in,
                                            atlas_decision_outcome *out, atlas_err *err);
/* Relates one decision to another, on a document that already exists.
 *
 * A new revision, because a revision is immutable and its links are hashed;
 * idempotent, because a target already related is reported rather than added
 * again. Not an operator operation: it writes a proposal through the same path
 * `propose` and `revise` use. */
/* `note` is the durable reason the relation exists (migration 10), or NULL.
 * When the relation is already there and a note is given, the note is attached
 * to the existing edge and **no revision is written**: an explanation recorded
 * after an approval was not part of what was approved, and minting a revision
 * for it would move a content hash to cover something the approval never did.
 * `provenance` names where the note came from, or NULL for UNKNOWN. */
atlas_status atlas_service_decision_link_add(atlas_ctx *ctx, const char *repo, const char *uid,
                                             const char *target_uid, const char *note,
                                             const char *provenance, atlas_decision_outcome *out,
                                             atlas_err *err);

/* Withdraws a relation. Writes a new proposed revision asserting one relation
 * fewer; deletes nothing. The revision that carried the relation keeps it, with
 * its creation event and its rationale, so a withdrawn edge stays explicable.
 * `removed_out` is false when there was no such relation, which is reported
 * rather than treated as an error so that a repeated removal is a no-op. */
/* Records one event about an edge and touches no link at all.
 *
 * The only way to say what happened to a relation that is already gone: the
 * edge is in no current revision, so there is nothing to add or remove, and the
 * remaining honest act is to record it. Writes no revision, moves no status and
 * mints no capability. `event` may name ADDED, ANNOTATED or REMOVED; NULL means
 * ANNOTATED. */
atlas_status atlas_service_decision_link_note(atlas_ctx *ctx, const char *repo, const char *uid,
                                              const char *target_uid, const char *note,
                                              const char *provenance, const char *event,
                                              atlas_decision_outcome *out, atlas_err *err);
atlas_status atlas_service_decision_link_note_remote(const char *repo, const char *uid,
                                                     const char *target_uid, const char *note,
                                                     const char *provenance, const char *event,
                                                     atlas_decision_outcome *out, atlas_err *err);

atlas_status atlas_service_decision_link_remove(atlas_ctx *ctx, const char *repo, const char *uid,
                                                const char *target_uid, const char *reason,
                                                atlas_decision_outcome *out, bool *removed_out,
                                                atlas_err *err);

atlas_status atlas_service_decision_revise(atlas_ctx *ctx, const char *repo, const char *uid,
                                           const atlas_decision_input *in,
                                           atlas_decision_outcome *out, atlas_err *err);

/* Decision documents attached to no live repository.
 *
 * `repo remove` detaches decisions rather than deleting them — they are the one
 * canonical record in the index — and a detached document appears in no
 * repository listing. Without this a user who removed a repository would
 * conclude Atlas had destroyed their approval history, which is exactly the
 * wrong thing to conclude. */
atlas_status atlas_service_decision_orphans(atlas_ctx *ctx, int64_t limit,
                                            atlas_decision_summary_cb cb, void *ud,
                                            int64_t *count_out, bool *more_out, atlas_err *err);

/* Creates an A4 document from an A2 `ai_decisions` proposal.
 *
 * The A2 row is read and left exactly as it was: still `approved = 0`, still
 * present, still listed. The new document is PROPOSED and carries a pointer
 * back to it. Promotion is an explicit act rather than something a migration
 * did, because a migration that manufactured decision documents out of model
 * proposals would be inventing records nobody wrote. */
atlas_status atlas_service_decision_promote(atlas_ctx *ctx, const char *repo, int64_t legacy_id,
                                            atlas_decision_outcome *out, atlas_err *err);

/* The A2 proposals in one repository, with the A4 document each was promoted
 * into when it was. Read-only over the A2 tables. */
typedef struct atlas_decision_legacy_view {
    int64_t id;
    atlas_buf title;      /* UNTRUSTED_DATA */
    atlas_buf statement;  /* UNTRUSTED_DATA */
    atlas_buf provenance;
    atlas_buf created_at;
    atlas_buf imported_uid; /* empty when not promoted */
    int64_t path_count;
    bool imported;
} atlas_decision_legacy_view;

void atlas_decision_legacy_view_init(atlas_decision_legacy_view *v);
void atlas_decision_legacy_view_free(atlas_decision_legacy_view *v);

typedef atlas_status (*atlas_decision_legacy_view_cb)(const atlas_decision_legacy_view *v, void *ud,
                                                      atlas_err *err);

atlas_status atlas_service_decision_legacy(atlas_ctx *ctx, const char *repo, int64_t limit,
                                           atlas_decision_legacy_view_cb cb, void *ud,
                                           int64_t *count_out, bool *more_out, atlas_err *err);

/* --- the operator channel ---
 *
 * `atlas_service_decision_confirm` is the whole of it: it obtains a
 * capability, displays the exact values on the terminal, reads a confirmation
 * from `/dev/tty`, and spends the capability. There is no way to reach the
 * spending half without the displaying half, which is the point.
 *
 * `allow_yes` is not a parameter. `--yes` is refused by the CLI before this is
 * called and there is nothing here that could honour it. */
atlas_status atlas_service_decision_confirm(atlas_ctx *ctx, const char *repo, const char *uid,
                                            atlas_decision_intent intent,
                                            const char *replacement_uid, int64_t revision_no,
                                            atlas_decision_outcome *out, atlas_err *err);

/* A15 T4. The exact sentence `atlas_service_decision_confirm` returns when the
 * operator typed something other than the confirmation
 * (`src/core/service_decision.c`), pinned once here rather than left as two
 * literals that could drift: the review walker's ABANDONED mapping keys on
 * this text matching exactly, compared whole with `strcmp`. Declared `extern`
 * rather than `static const char[]` so that a translation unit which includes
 * this header without quoting the sentence carries no unused-variable warning
 * for it; defined once, in `src/core/service_decision.c`. */
extern const char ATLAS_DECISION_CONFIRM_MISTYPED_MSG[];

/* --- A15 T4: the review walker ----------------------------------------------
 *
 * A review sheet (`include/atlas/review.h`, T3) is an operator's own file on
 * this machine, transcribed by hand out of a Mission Control browser session.
 * It carries no authority: its fifth field is the public prefix the operator
 * will type, and no field it names is read in place of that typing -- every
 * entry is still confirmed by typing that hash prefix on `/dev/tty`, per
 * entry, through `atlas_service_decision_confirm` above and through nothing
 * else — this file mints no capability and spends none itself.
 *
 * The walker's whole job beyond looping `atlas_service_decision_confirm` is
 * the pre-check: reading the live record again, immediately before minting
 * anything, so that a record which moved between when it was read in a
 * browser and when the sheet reached a terminal is refused rather than
 * approved at the wrong revision. The pre-check costs no challenge row —
 * everything it does is a read — which is what lets `--check` run it exactly
 * once with nothing minted, as a dry run of what a real run would find. */

/* What one entry's walk produced. Owned buffers, `_init`/`_free` below;
 * `entry` is borrowed for the callback only, like every other row callback in
 * this layer. */
typedef struct atlas_review_outcome {
    const atlas_review_entry *entry; /* borrowed for the callback */
    atlas_review_verdict verdict;
    atlas_buf status;  /* the record's status as read, safe-encoded at every
                        * path -- a caller must NOT encode it again. Ordinarily
                        * it is a closed Atlas vocabulary value
                        * (PROPOSED/APPROVED/REJECTED/SUPERSEDED/RESOLVED), the
                        * same category service_decision.c's fill_summary
                        * marks untrusted = false for, and a value from that
                        * vocabulary encodes to itself byte-for-byte, so this
                        * field reads exactly as it would unencoded for every
                        * legitimate record. The encoding exists for the one
                        * case that category assumes away: a status column
                        * holding bytes that are not a vocabulary member at
                        * all, which only walk_entry's own
                        * atlas_decision_state_parse check can detect, and
                        * which is exactly the case the unencoded field would
                        * have been wrong for. Empty when nothing was read
                        * (MISSING). */
    atlas_buf detail;  /* a §Frozen detail line, or the refusal message; fixed
                        * Atlas text with checked values, or safe-encoded */
    int64_t current_revision_no; /* MOVED: what the newest is */
    char current_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
} atlas_review_outcome;

void atlas_review_outcome_init(atlas_review_outcome *o);
void atlas_review_outcome_free(atlas_review_outcome *o);

typedef atlas_status (*atlas_review_outcome_cb)(const atlas_review_outcome *o, void *ud,
                                                atlas_err *err);

/* The counts across a whole sheet. `ready` is meaningful only under
 * `check_only`; `applied`/`abandoned`/`refused` only outside it — a sheet's
 * entries settle into exactly one bucket each, and which buckets can be
 * nonzero depends on which mode walked it. */
typedef struct atlas_review_totals {
    int64_t ready, applied, abandoned, moved, disposed, missing, refused;
} atlas_review_totals;

/* Walks one sheet. Order of operations, and none may move:
 *   1. unless `check_only`, atlas_authority_require(ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE)
 *      — before a file is read or a terminal is touched, so a locked profile
 *      never gets that far. A dry run mints nothing and reveals nothing this
 *      uid could not already read with `atlas decision show`, so `--check`
 *      asks for neither authority nor a terminal — see the T4 report for the
 *      chain this turns on;
 *   2. unless `check_only`, atlas_terminal_open + close — the refusal is that
 *      function's own sentence, and it happens before the sheet is read;
 *   3. read `sheet_path` with O_NOFOLLOW, bounded by ATLAS_REVIEW_SHEET_MAX_BYTES,
 *      and atlas_review_sheet_parse; a parsed sheet with zero entries — a
 *      header with nothing after it, which the parser accepts as
 *      grammatically valid — is refused here with "review sheet: no entries;
 *      there is nothing to review", because under the frozen exit-code rule
 *      zero entries makes "every entry ended APPLIED" vacuously true and a
 *      truncated sheet would otherwise exit 0 having done nothing;
 *   4. per entry, in sheet order: atlas_service_decision_show with revision_no
 *      = the entry's revision (or its remote form, chosen as the rest of the
 *      service layer chooses) → MISSING when the repository or the decision
 *      is unknown; MOVED when summary.latest_revision_no != revision_no or
 *      that revision's content_hash does not begin with the entry's prefix;
 *      DISPOSED when summary.status is not the status the intent needs
 *      (PROPOSED for approve and reject, APPROVED for resolve) — all three
 *      without minting; else, under `check_only`, READY; else
 *      atlas_service_decision_confirm(ctx, repo, decision, intent, NULL,
 *      revision_no, …) → APPLIED, or ABANDONED when its refusal is exactly
 *      ATLAS_DECISION_CONFIRM_MISTYPED_MSG, or REFUSED carrying its message.
 * The callback is invoked once per entry, in order, with borrowed pointers. A
 * refused or abandoned entry does not stop the walk; a non-OK return from the
 * callback itself does. Nothing is retried. */
atlas_status atlas_service_review_apply(atlas_ctx *ctx, const char *sheet_path, bool check_only,
                                        atlas_review_outcome_cb cb, void *ud,
                                        atlas_review_totals *totals, atlas_err *err);

/* Markdown or JSON, to a stream. Never written into the target repository:
 * Atlas is read-only with respect to a registered worktree, and a decision
 * document is Atlas' record rather than the project's file. */
atlas_status atlas_service_decision_export_markdown(const atlas_decision_document *doc, FILE *out,
                                                    atlas_err *err);

/* --- A6: impact gates ------------------------------------------------------
 *
 * Both are reads. Neither takes the writer lock, neither writes a row, and
 * there is no service function anywhere that clears, overrides or caches a
 * freshness result — the only thing that changes what an assessment will say
 * next time is the code, or a revalidation recorded through the operator
 * channel.
 *
 * The whole gate is `atlas_service_gate_check`. `atlas_service_gate_show` runs
 * the same query and keeps one decision from it, rather than implementing a
 * second, narrower assessment that would disagree with the first the moment
 * either was fixed. */
atlas_status atlas_service_gate_check(atlas_ctx *ctx, const atlas_gate_query *q,
                                      atlas_gate_report *out, atlas_err *err);
atlas_status atlas_service_gate_show(atlas_ctx *ctx, const char *repo, const char *uid,
                                     const char *at_commit, atlas_gate_report *out,
                                     atlas_err *err);


/* --- A9.2: verification -----------------------------------------------------
 *
 * What a `verify` command reports. It carries the assessment, the policy state
 * that shaped it, and the descriptive halves a reader needs — the claim's text
 * and the record's identity — so a renderer never has to query anything.
 *
 * `claim_text` and `record_title` are **UNTRUSTED_DATA**. Verification changes
 * a status, never the nature of bytes: a VERIFIED claim's proposition is
 * exactly as untrusted as a proposed one's, and both renderers encode it. That
 * is A4's rule about approved prose, restated because the temptation is
 * stronger here — "Atlas verified this" reads like a warrant for the text, and
 * it is a statement about a truth condition, not about the sentence. */
typedef struct atlas_verify_report {
    atlas_verify_assessment assessment;

    atlas_buf claim_uid;
    atlas_buf claim_text;   /* UNTRUSTED_DATA */
    atlas_buf domain;
    atlas_buf record_uid;
    atlas_buf record_title; /* UNTRUSTED_DATA */

    /* A9.2.1 closeout: the evidence and attestations behind the numbers.
     *
     * A verification result whose evidence a reader cannot see is a number to
     * be taken on trust, which is the opposite of what recording evidence is
     * for. Every surface that shows a score shows this beside it, and every
     * text field in it is UNTRUSTED_DATA. */
    atlas_verify_detail detail;

    /* The root-owned policy as it was when the assessment ran, so a reader can
     * see *why* an answer was shadow rather than automatic without going to
     * look at a file they may not be able to read. */
    atlas_verifypolicy_state policy_state;
    atlas_verifypolicy_reason policy_reason;
    char policy_id[128];
    char policy_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char policy_path[256];
    char policy_detail[512];
    bool deterministic_enforce;
    bool empirical_enforce;
    size_t rule_count;
} atlas_verify_report;

void atlas_verify_report_init(atlas_verify_report *r);
void atlas_verify_report_free(atlas_verify_report *r);

/* Assesses one claim and writes nothing — no result row, no audit row, no
 * transition. Asking what Atlas thinks cannot change what Atlas thinks, which
 * is the property A6's gate has and for the same reason. */
atlas_status atlas_service_verify_show(atlas_ctx *ctx, int64_t claim_id, atlas_verify_report *out,
                                       atlas_err *err);
/* Assesses, records the result and the audit row, and performs the transition
 * when every gate passed and enforcement is on for that path.
 *
 * **An operator action**, served over IPC only in the operator-uid group. It
 * mints no capability and approves nothing: it asks Atlas to evaluate a policy
 * somebody else installed, which is why it needs no terminal, no challenge and
 * no confirmation. What it can produce is a `VERIFICATION_POLICY` ledger entry,
 * which is not `LOCAL_OPERATOR_CONFIRMED` and never becomes it.
 *
 * The claim may be named by its rowid or by the uid every surface reports; pass
 * one and leave the other empty. Both spellings are accepted for the reason
 * `verify show` accepts both, and the two cannot collide because a uid never
 * parses as a number. */
atlas_status atlas_service_verify_run(atlas_ctx *ctx, int64_t claim_id, const char *claim_uid,
                                      const char *repo_name, atlas_verify_report *out,
                                      atlas_err *err);
/* Reports the root-owned policy. Opens no index and binds nothing, so it is
 * safe to run anywhere — the shape `gateway status` has. */
atlas_status atlas_service_verify_policy(atlas_verify_report *out, atlas_err *err);

/* --- A9.2.1: the forms the daemon calls -------------------------------------
 *
 * These take a raw handle and a resolved repository, so the CLI and the daemon
 * call one implementation and parity between the surfaces is structural rather
 * than two functions somebody keeps in step — A8-CI's rule about
 * `atlas_sem_impact_on`.
 *
 * A claim may be named by rowid or by public uid; both resolve to the same
 * claim, because the uid is what every surface reports and the rowid is what
 * A9.2's CLI took. */
atlas_status atlas_service_verify_show_on(atlas_db *db, int64_t claim_id, const char *claim_uid,
                                          atlas_verify_report *out, atlas_err *err);
atlas_status atlas_service_verify_claims_on(atlas_db *db, atlas_json *j, atlas_safe_pool *safe,
                                            int64_t repo_id, const char *decision_uid,
                                            int64_t limit, atlas_err *err);

/* The one serialization of a verification answer, so the daemon's response and
 * the CLI's `--json` cannot describe the same assessment differently.
 *
 * The three axes are separate fields and a confidence score carries no percent
 * sign; `calibrated_probability` is **absent** rather than null when calibration
 * does not support one, because a null invites a client to render "0%". */
atlas_status atlas_service_verify_write_assessment(atlas_json *j,
                                                   const atlas_verify_assessment *a,
                                                   atlas_err *err);
atlas_status atlas_service_verify_write_policy(atlas_json *j, const atlas_verify_report *r,
                                               atlas_err *err);
/* The evidence and attestation lists. Exported because the CLI's JSON renderer
 * builds its own envelope but must not build its own *evidence*: a surface that
 * showed a confidence score with a different set of evidence behind it than the
 * daemon showed would be two answers to one question. */
atlas_status atlas_service_verify_write_detail(atlas_json *j, atlas_safe_pool *safe,
                                               const atlas_verify_detail *d, atlas_err *err);
atlas_status atlas_service_verify_write_report(atlas_json *j, atlas_safe_pool *safe,
                                               const atlas_verify_report *r, atlas_err *err);


/* --- A8: orchestration ------------------------------------------------------
 *
 * Every one of these speaks to the daemon over the socket. There is no offline
 * path: orchestration state lives in the index, `atlasd` is the only writer of
 * it, and a CLI that fell back to opening the database itself would be a second
 * writer — which is the one thing A1 forbids and A7.1 makes impossible anyway.
 */

/* One job, as either renderer presents it. Strings are borrowed and every
 * untrusted one — the task text — has already been safe-encoded by the daemon,
 * so a renderer prints it as-is. */
typedef struct atlas_job_render {
    const char *job;
    const char *state;
    const char *repo;
    const char *driver;
    const char *commit;
    const char *created_at;
    const char *terminal_at;
    const char *spec_digest;
    const char *task; /* already in the safe text encoding */
    int64_t attempts;
    int64_t max_attempts;
    int64_t seq;
    bool cancel_requested;
    bool duplicate;
    /* A11.0/A11.1. The run this job belongs to, and — for `job run` — what the
     * invocation did to it. All are NULL or zero when they do not apply; a
     * renderer prints what is present and infers nothing from what is not. */
    const char *run;
    const char *run_status;
    const char *follow_up;
    int64_t worker_starts;
    int64_t tasks;
    /* The run's active task was already held, so nothing was started. Neither
     * an acceptance nor a refusal: the run stays ACTIVE and resumable. */
    bool busy;
    /* A11.6. For `job run-status`: how many tasks in the run are non-terminal,
     * and how many it may hold. Both are zero when the daemon did not report
     * them — an older daemon against a newer CLI — and a renderer prints the
     * line only when the bound is present, because zero is not a claim that a
     * run allows nothing. */
    int64_t active_count;
    int64_t max_parallel;
    /* A10.0. What the run cost, for `job run-status`. `usage_status` is
     * `ATLAS_USAGE_UNKNOWN` when the daemon reported none — an older daemon
     * against a newer CLI reads exactly that, which is the conservative value
     * and never an error, following A9.2.5's rule for absent keys. */
    atlas_usage_run usage;
    bool usage_present;
    /* A10.1. What this run was shown of earlier runs, for `job run-status`.
     * Every one is absent — NULL, zero, or false — for a run created before
     * migration 23, which reads as "this run was never part of a memory arm"
     * and is deliberately not the same as a run that ran with memory OFF. */
    const char *memory_mode;
    const char *memory_package_status;
    const char *memory_package_digest;
    int64_t memory_source_count;
    int64_t memory_package_bytes;
    bool memory_candidates_truncated;
    bool memory_present;
    const char *memory_sources[3];
    size_t memory_source_listed;
    /* True for `job get`, which prints every field; false for a list row. */
    bool detail;
    /* A14. The bare 16-hex credential selector that queued this job, or NULL
     * when the job was submitted locally.  Human renderers prefix with
     * `ATLAS_APIKEY_ID_PREFIX` ("key_"); JSON renderers emit `key_id`.
     * Points into a live response frame — copy if it must outlive the call. */
    const char *key_id;
    /* True only when this row is being emitted inside a `jobs` array. The JSON
     * renderer needs to know: a member of an array is an anonymous object, and
     * a single result is a set of members on the document itself. Emitting an
     * unkeyed object at the top level is what the writer refuses, and it
     * refused during the A8 cutover. */
    bool in_list;
} atlas_job_render;

typedef struct atlas_job_submit_opts {
    const char *repo;
    const char *task;
    const char *mode;
    const char *driver;
    const char *idempotency_key;
    const char *correlation;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_attempts;
    /* A11.1. The verification gates, fixed here — at the root task's submission
     * — and inherited verbatim by every follow-up in the run. Each is a command
     * line split on ASCII spaces and **never by a shell**: there is no quoting,
     * no expansion and no way to express an argument containing a space, which
     * is deliberate. `argv[0]` must still be on the binary's own allowlist.
     *
     * A worker cannot add, remove or weaken one. It never sees this structure,
     * the daemon stores what arrives here on the job row, and a follow-up's list
     * is copied from its parent's rather than supplied. */
    const char *gates[8];
    size_t gate_count;
    /* A10.1. The cross-run memory mode this run is created in: "off" or
     * "bounded". NULL and empty both mean off. Chosen by the operator invoking
     * the CLI, sent on the submission, and frozen against the run it creates;
     * no model payload reaches this field, because no MCP tool and no gateway
     * route reaches `job.submit`. */
    const char *memory;
    /* A11.6. The task this submission follows, which is what puts it in that
     * task's run. NULL or empty means this is a root task and creates its own
     * run. The daemon resolves it against stored rows — it must exist, describe
     * the same repository, be pinned to the run's commit, and leave the run with
     * room — and refuses rather than repairs on every one of those. */
    const char *parent;
    /* A11.6. How many tasks the run this submission creates may hold active at
     * once. Zero means "not stated" and resolves to one, which is what every run
     * did before this existed; anything outside `1..ATLAS_ORCH_RUN_MAX_PARALLEL`
     * is refused rather than reduced, and naming it together with `parent` is
     * refused too — a run's bound is fixed at its root. */
    int64_t max_parallel;
} atlas_job_submit_opts;

/* A11.1. `atlas job run`: submit a root task and drive its run to a settled
 * answer, or resume one that already exists.
 *
 * Exactly one of `repo`+`task` (start) and `resume` (continue) is given. There
 * is no form that names a job: which task is next is the run's answer, not the
 * caller's. */
typedef struct atlas_job_run_opts {
    const char *repo;
    const char *task;
    const char *resume;
    const char *mode;
    const char *driver;
    const char *idempotency_key;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    const char *gates[8];
    size_t gate_count;
    /* A10.1. As `atlas_job_submit_opts::memory`, and it applies only to the
     * start form. Naming it while resuming is refused rather than ignored: the
     * package is frozen against the run, so honouring the flag would be a lie
     * and dropping it silently would be a worse one. */
    const char *memory;
    /* A11.6. As `atlas_job_submit_opts::max_parallel`, and it applies only to
     * the start form. Naming it while resuming is refused rather than ignored,
     * for the reason `memory` is: the bound is fixed against the run when the
     * run is created, so honouring the flag would be a lie and dropping it
     * silently would be a worse one. */
    int64_t max_parallel;
    /* Where the driver narrates what it is doing. NULL is silent. */
    FILE *log;
} atlas_job_run_opts;

/* Receives one job. A callback rather than a renderer, because the service
 * layer never formats output and must not know that renderers exist — the
 * layering rule this repository has kept since A0. The CLI passes a callback
 * that calls the renderer vtbl. */
typedef atlas_status (*atlas_job_sink)(const atlas_job_render *jr, void *ud, atlas_err *err);

atlas_status atlas_service_job_submit(atlas_ctx *ctx, const atlas_job_submit_opts *o,
                                      atlas_job_sink sink, void *ud, atlas_err *err);
atlas_status atlas_service_job_get(atlas_ctx *ctx, const char *job, atlas_job_sink sink,
                                   void *ud, atlas_err *err);
atlas_status atlas_service_job_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                    bool remote, atlas_job_sink sink, void *ud,
                                    int64_t *count_out, bool *more_out, atlas_err *err);
atlas_status atlas_service_job_cancel(atlas_ctx *ctx, const char *job, atlas_job_sink sink,
                                      void *ud, atlas_err *err);

/* A11.1. Drives one run in the foreground, to a settled answer or to the point
 * where there is nothing more this invocation may do.
 *
 * It starts nothing in the background, polls no queue and schedules nothing. It
 * decides neither ACCEPTED nor BLOCKED: it reports an exit classification Atlas
 * computed and a gate verdict Atlas ran, and the daemon derives the run's status
 * inside the transaction that records them. */
atlas_status atlas_service_job_run(atlas_ctx *ctx, const atlas_job_run_opts *o,
                                   atlas_job_sink sink, void *ud, atlas_err *err);
/* Reads one run: its status, its root, and the task it is waiting on. A read,
 * and the only run-shaped surface there is. */
atlas_status atlas_service_job_run_status(atlas_ctx *ctx, const char *run, atlas_job_sink sink,
                                          void *ud, atlas_err *err);

/* Runs the dispatcher loop. Reads the root-owned policies itself and refuses to
 * start when orchestration is disabled — a disabled policy is a refusal to
 * start, not a loop that idles. */
atlas_status atlas_service_dispatcher_run(bool once, FILE *log, atlas_err *err);

/* A13. Asks the daemon which repositories this uid may scan.
 *
 * Opens no index, takes no lock and holds no database handle: every answer
 * comes over the socket, which is why the CLI dispatches it before any
 * `atlas_ctx` exists. `once` asks a single time and returns; without it the
 * command refuses, because the polling loop is a later plan and a process that
 * idled instead of saying so would look healthy while doing nothing. Logs to
 * `log` so a systemd user unit captures them in the journal. */
atlas_status atlas_service_scanner_run(bool once, FILE *log, atlas_err *err);

/* --- A12.0: the plan domain's four calls ------------------------------------
 *
 * The wire half of the plan commands, in `src/core/service_plan.c`. Like every
 * orchestration command there is deliberately **no offline path**: plan state
 * lives in the index, `atlasd` is the only writer of it, and a CLI that opened
 * the database itself would be a second writer.
 *
 * These are the calls; the commands come later. `plan run` composes them into a
 * foreground loop and `plan status|show|list` render what they return, and both
 * of those are built on top of these rather than beside them, so there is one
 * spelling of each request in the binary.
 *
 * Two of them hand back the parsed response rather than a filled struct. A plan
 * is a wide document — a goal, a floor, a revision list, a task list with job
 * and run states and a usage rollup — and the shape a renderer wants is a
 * question for the renderer. `atlas_ipc_response_free` releases it, and `raw`
 * holds the bytes every returned string borrows from, so it must outlive the
 * response.
 */
typedef struct atlas_plan_create_opts {
    const char *repo;
    /* The operator's own words, sent verbatim: bounded and stored as they were
     * typed, and safe-encoded on every surface that reads them back. */
    const char *goal;
    /* The operator's gate floor, one command line per element, split on spaces
     * by `atlas_orch_gate_split` exactly as `--gate` is for a job. At least one
     * is required by the daemon: a plan with no operator gate could only ever be
     * accepted on a model's word. */
    const char *gates[8];
    size_t gate_count;
    /* 0 is "not stated", which the daemon resolves to its own default. A value
     * outside the range is refused with the bound named, never clamped. */
    int64_t max_parallel;
} atlas_plan_create_opts;

atlas_status atlas_plan_wire_create(atlas_ctx *ctx, const atlas_plan_create_opts *o,
                                    atlas_buf *plan_uid_out, atlas_err *err);

typedef struct atlas_plan_revision_opts {
    const char *plan;
    /* The planner job whose own stored artifact becomes the revision. The
     * document itself never travels this call: there is no member here that
     * could carry one. */
    const char *planner_job;
    /* `INITIAL` or `REPLAN`, from `atlas_plan_revision_reason_name`. */
    const char *reason;
    int rev_no;
} atlas_plan_revision_opts;

/* Ingests one planner job's document.
 *
 * A refusal that came from the *document* fills `refusal_out` and `line_out` —
 * Atlas' own sentence and the line it is about — which is what a plan driver
 * composes a retry prompt from. Every other refusal leaves them untouched, so a
 * non-empty `refusal_out` is the discriminator between a planner's mistake and
 * the caller's, exactly as it is at the write point. Both are optional. */
atlas_status atlas_plan_wire_revision_add(atlas_ctx *ctx, const atlas_plan_revision_opts *o,
                                          int *rev_no_out, atlas_buf *refusal_out,
                                          int *line_out, atlas_err *err);

/* Reads one plan. `rev_no` above zero also asks for that revision's document,
 * which comes back safe-encoded and labelled UNTRUSTED_DATA.
 *
 * `task_detail` additionally asks each task for the two members a driver submits
 * from — the executor prompt and the merged gate list. It is off for every
 * operator-facing surface: those two are the wide part of the document and
 * nothing an operator reads needs them. */
atlas_status atlas_plan_wire_get(atlas_ctx *ctx, const char *plan, int rev_no, bool task_detail,
                                 atlas_ipc_response **out, atlas_buf *raw, atlas_err *err);

/* One page of this principal's plans. */
atlas_status atlas_plan_wire_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                  atlas_ipc_response **out, atlas_buf *raw, atlas_err *err);

/* --- A12.0: the four plan commands ------------------------------------------
 *
 * `atlas plan run|status|show|list`. One service result and two renderers, as
 * every command in this binary has been since A0.
 *
 * **Every string in the render structs is borrowed** — from the parsed response
 * these were filled from, which the service layer keeps alive across the sink
 * call and frees afterwards, exactly as `atlas_job_render` borrows from
 * `job.get`'s. A renderer copies nothing and must not retain anything.
 *
 * Every untrusted value — the goal, the gate floor block, a task title, a
 * revision's document — arrived **already safe-encoded from the daemon**, so a
 * renderer prints it as-is and labels it. Encoding it a second time is how `%2F`
 * becomes `%252F`; both renderers say so at the top of the file. */
typedef struct atlas_plan_task_render {
    const char *key;
    const char *kind; /* "TREE" or "SIDE" — Atlas' own vocabulary */
    const char *title; /* already in the safe text encoding */
    /* NULL until the task has been submitted. An absent job is an absent line,
     * never an empty identifier: the two read differently and only one is
     * true. */
    const char *job;
    const char *job_state;
    /* Tree tasks only: the run the task's stage became, and how it settled. */
    const char *run;
    const char *run_status;
    /* A10.0's per-attempt measurement, rolled up over this task's job. Present
     * only when something was measured — an absent measurement is not a zero,
     * and printing one as 0 would turn "we do not know" into "it was free". */
    const char *usage_model; /* already in the safe text encoding */
    int64_t usage_cost_micro_usd;
    int64_t usage_turns;
    bool has_cost;
    bool has_turns;
    int64_t stage;
} atlas_plan_task_render;

typedef struct atlas_plan_render {
    const char *plan;
    const char *repo;
    /* The derived status, from `atlas_plan_status_name`. Derived by the daemon
     * on the read that produced it and never stored anywhere. */
    const char *status;
    const char *created_at;
    /* The operator's own words and the operator's own commands, both already in
     * the safe text encoding. The floor is one command per line, so its newlines
     * are escaped like every other C0 byte. */
    const char *goal;
    const char *gate_floor_text;
    /* The plan's latest planner job, when it has one. */
    const char *planner_job;
    const char *planner_job_state;
    /* `plan show P --rev N` only: that revision's document, a planner's bytes,
     * already safe-encoded and labelled UNTRUSTED_DATA. NULL otherwise. */
    const char *content;
    int64_t content_rev_no;
    int64_t max_parallel;
    int64_t gate_floor_count;
    int64_t rev_no;
    int64_t planner_jobs_seen;
    int64_t stages_accepted;
    int64_t revision_count;
    bool replan_wanted;
    /* `plan run` only: the daemon was busy and took nothing. Neither an
     * acceptance nor a refusal — the plan is untouched and resumable, and the
     * same invocation may simply be repeated. */
    bool busy;
    atlas_plan_task_render tasks[ATLAS_PLAN_MAX_TASKS];
    size_t task_count;
    /* True for `plan run|status|show`, which print every field; false for a list
     * row. One method, two depths — `job_item`'s shape, for its reason. */
    bool detail;
    /* True only inside a `plans` array, where a row is an anonymous object. A
     * single result is a set of members on the document itself, and the JSON
     * writer refuses an unkeyed object at the top level. */
    bool in_list;
} atlas_plan_render;

typedef atlas_status (*atlas_plan_sink)(const atlas_plan_render *pr, void *ud, atlas_err *err);

/* What `atlas plan run` was asked for. Mirrors `atlas_job_run_opts`: a goal and
 * a gate floor create a plan, or `resume` carries an existing one and nothing
 * else may be named beside it. */
typedef struct atlas_plan_run_opts {
    const char *repo;
    const char *goal;
    /* An existing plan to carry on. Naming a goal, a gate or a parallelism
     * beside it is refused rather than ignored — A10.1's `--memory --resume`
     * rule: a flag that was quietly dropped reads exactly like one that was
     * honoured. */
    const char *resume;
    const char *gates[8];
    size_t gate_count;
    /* 0 is "not stated", which the daemon resolves to its own default. Outside
     * `1..ATLAS_ORCH_RUN_MAX_PARALLEL` it is refused with the bound named,
     * never clamped. */
    int64_t max_parallel;
    /* Where the loop narrates. `stderr` under `--json`, so exactly one document
     * reaches stdout. */
    FILE *log;
} atlas_plan_run_opts;

/* Creates or resumes a plan and drives it in the foreground until it is
 * COMPLETED or BLOCKED, until nothing this process can do would move it, or
 * until the driver's own defect guard. Reads the root-owned orchestration policy
 * itself and refuses to start when orchestration is disabled.
 *
 * A plan that ended BLOCKED is an *answer*, reported through the sink exactly as
 * a BLOCKED run is, and not an error. */
atlas_status atlas_service_plan_run(atlas_ctx *ctx, const atlas_plan_run_opts *o,
                                    atlas_plan_sink sink, void *ud, atlas_err *err);

/* One plan, read. `rev_no` above zero also asks for that revision's document. */
atlas_status atlas_service_plan_status(atlas_ctx *ctx, const char *plan, atlas_plan_sink sink,
                                       void *ud, atlas_err *err);
atlas_status atlas_service_plan_show(atlas_ctx *ctx, const char *plan, int rev_no,
                                     atlas_plan_sink sink, void *ud, atlas_err *err);

/* One page of this principal's plans. */
atlas_status atlas_service_plan_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                     atlas_plan_sink sink, void *ud, int64_t *count_out,
                                     bool *more_out, atlas_err *err);

/* --- A7.1: the remaining read commands, answered by the daemon --------------
 *
 * Declared here, at the end, because each one names a report type defined
 * above. Every one fills the same struct its local twin fills and is called
 * from the same CLI call site, so the renderers and the JSON contract are
 * shared rather than reproduced. See `src/core/service_remote.c`. */
atlas_status atlas_service_search_remote(const char *name, const char *query, int64_t limit,
                                         atlas_search_mode *mode_out, atlas_search_cb cb, void *ud,
                                         int64_t *count_out, atlas_err *err);
/* Backup create and verify, performed by the daemon in its own backup
 * directory. `name` is one path component and never a path: the destination is
 * fixed and the caller chooses a name within it. `out->path` receives that
 * name, which is also what `verify` takes back. */
/* Relate one decision to another, daemon-side. The content is never sent, so
 * nothing can be re-encoded on the way back. */
atlas_status atlas_service_decision_link_add_remote(const char *repo, const char *uid,
                                                    const char *target_uid, const char *note,
                                                    const char *provenance,
                                                    atlas_decision_outcome *out, atlas_err *err);
atlas_status atlas_service_decision_link_remove_remote(const char *repo, const char *uid,
                                                       const char *target_uid, const char *note,
                                                       atlas_decision_outcome *out,
                                                       bool *removed_out, atlas_err *err);
/* One long-running operation's state, as a client sees it.
 *
 * `state` is a fixed vocabulary string from the daemon and `kind` names the
 * operation; both are Atlas-owned. `message` and `detail` are the daemon's own
 * summary of its own artefact and arrive already safe-encoded. */
typedef struct atlas_operation_report {
    int64_t id;
    atlas_buf kind;
    atlas_buf state;
    bool done;
    bool succeeded;
    int64_t duration_ms;
    atlas_buf message;
    atlas_buf detail;
} atlas_operation_report;

void atlas_operation_report_init(atlas_operation_report *r);
void atlas_operation_report_free(atlas_operation_report *r);

/* Asks the daemon about one accepted operation. A read, and idempotent: a
 * record that reached a terminal state never changes, so a client may ask as
 * often as it likes and a client that was killed mid-poll may simply ask
 * again. */
atlas_status atlas_service_operation_status_remote(int64_t op_id, atlas_operation_report *out,
                                                   atlas_err *err);

atlas_status atlas_service_backup_create_remote(const char *name, atlas_backup_report *out,
                                                atlas_backup_verify_report *verified,
                                                atlas_err *err);
atlas_status atlas_service_backup_verify_remote(const char *name, atlas_backup_verify_report *out,
                                                atlas_err *err);
atlas_status atlas_service_events_remote(const char *name, int64_t since, int64_t limit,
                                         atlas_event_cb cb, void *ud, int64_t *count_out,
                                         int64_t *next_cursor_out, bool *more_out, atlas_err *err);
atlas_status atlas_service_sync_remote(const char *name, bool full, bool wait, int timeout_ms,
                                       atlas_sync_report *out, atlas_err *err);
atlas_status atlas_service_sync_wait_remote(const char *name, atlas_sync_report *out,
                                            int timeout_ms, atlas_err *err);
atlas_status atlas_service_daemon_status_remote(atlas_daemon_status_report *out, atlas_err *err);
atlas_status atlas_service_code_status_remote(const char *name, atlas_code_status_report *out,
                                              atlas_err *err);
atlas_status atlas_service_decision_list_remote(const char *repo,
                                                const atlas_decision_list_opts *opts,
                                                atlas_decision_summary_cb cb, void *ud,
                                                int64_t *count_out, bool *more_out,
                                                atlas_decision_counts *counts_out, atlas_err *err);
atlas_status atlas_service_gate_check_remote(const atlas_gate_query *q, atlas_gate_report *out,
                                             atlas_err *err);
atlas_status atlas_service_file_remote(const char *name, const char *path,
                                       atlas_file_report_cb cb, void *ud, atlas_err *err);
atlas_status atlas_service_history_remote(const char *name, const char *path, int64_t limit,
                                          atlas_history_cb cb, void *ud, int64_t *count_out,
                                          atlas_err *err);
atlas_status atlas_service_diff_remote(const char *name, const atlas_diff_opts *opts,
                                       atlas_diff_entry_cb cb, void *ud, atlas_diff_report *rep,
                                       atlas_err *err);
atlas_status atlas_service_code_file_remote(const char *name, const char *path,
                                            atlas_code_file_report *out, atlas_err *err);
atlas_status atlas_service_code_file_symbols_remote(const char *name, const char *path,
                                                    int64_t limit, atlas_code_symbol_cb cb,
                                                    void *ud, int64_t *count_out, bool *more_out,
                                                    atlas_err *err);
atlas_status atlas_service_code_file_edges_remote(const char *name, const char *path,
                                                  const char *kind, bool inbound, int64_t limit,
                                                  atlas_code_edge_cb cb, void *ud,
                                                  int64_t *count_out, bool *more_out,
                                                  atlas_err *err);
atlas_status atlas_service_code_symbol_search_remote(const char *name, const char *query,
                                                     const char *kind, int64_t limit,
                                                     atlas_code_symbol_cb cb, void *ud,
                                                     int64_t *count_out, bool *more_out,
                                                     atlas_err *err);
atlas_status atlas_service_code_symbol_sites_remote(const char *name, const char *symbol,
                                                    int64_t limit, atlas_code_symbol_cb cb,
                                                    void *ud, int64_t *count_out, bool *more_out,
                                                    atlas_err *err);
atlas_status atlas_service_code_symbol_edges_remote(const char *name, const char *symbol,
                                                    bool inbound, int64_t limit,
                                                    atlas_code_edge_cb cb, void *ud,
                                                    int64_t *count_out, bool *more_out,
                                                    atlas_err *err);
atlas_status atlas_service_code_walk_remote(const char *name, const char *path, const char *symbol,
                                            bool inbound, int64_t depth, int64_t limit,
                                            atlas_code_walk_cb cb, void *ud,
                                            atlas_code_walk_summary *sum, atlas_err *err);
atlas_status atlas_service_decision_show_remote(const char *repo, const char *uid,
                                                int64_t revision_no, atlas_decision_document *out,
                                                atlas_err *err);
atlas_status atlas_service_decision_history_remote(const char *repo, const char *uid,
                                                   atlas_decision_summary_cb rev_cb,
                                                   atlas_decision_timeline_cb event_cb, void *ud,
                                                   bool *ledger_agrees_out, atlas_err *err);
atlas_status atlas_service_decision_orphans_remote(int64_t limit, atlas_decision_summary_cb cb,
                                                   void *ud, int64_t *count_out, bool *more_out,
                                                   atlas_err *err);
atlas_status atlas_service_decision_legacy_remote(const char *repo, int64_t limit,
                                                  atlas_decision_legacy_view_cb cb, void *ud,
                                                  int64_t *count_out, bool *more_out,
                                                  atlas_err *err);
atlas_status atlas_service_gate_show_remote(const char *repo, const char *uid,
                                            atlas_gate_report *out, atlas_err *err);
/* Narrows a single-decision assessment to the report `gate show` promises.
 * Shared by the local and daemon-served forms. */
atlas_status atlas_gate_narrow_to_one(atlas_gate_report *out, const char *uid, atlas_err *err);

/* --- A8-CI: the compiler-derived semantic index ----------------------------
 *
 * Every report here carries three things before it carries any result: which
 * repository answered, which generation answered, and how fresh that generation
 * is. A caller that reads only the rows would be unable to tell a current answer
 * from one describing code that has since changed, and those must never look
 * alike.
 *
 * The repository is always resolved through `atlas_service_require_repo`, which
 * reads the persistent registry and nothing else — the one resolver CLI, RPC and
 * MCP share. */

/* Failed or unsupported translation units listed by `code status`. Bounded, and
 * the true total is reported separately so a short list never reads as the
 * whole story. */
#define ATLAS_SEM_STATUS_MAX_UNITS 32

typedef struct atlas_sem_failed_unit {
    char source[512];
    char status[16];
    char why[96];
    int64_t diagnostics_errors;
} atlas_sem_failed_unit;

typedef struct atlas_sem_status_report {
    atlas_repo_info repo;
    /* False when this Atlas was built without libclang. Reported rather than
     * silently answering with an empty index. */
    bool libclang_available;
    char compiler_id[64];
    char compiler_version[96];

    bool have_generation;
    atlas_sem_generation generation;
    atlas_sem_freshness freshness;
    const char *stale_reason; /* a fixed Atlas string, or NULL */
    atlas_sem_trust trust;    /* A9.2.5 */

    /* The most recent attempt of any status, so a failed index is visible
     * beside the one still being served. */
    bool have_latest;
    atlas_sem_generation latest;

    atlas_sem_failed_unit failed[ATLAS_SEM_STATUS_MAX_UNITS];
    size_t failed_count;
    int64_t failed_total;
    bool failed_truncated;

    /* --- A9.2.3 -------------------------------------------------------------
     *
     * The derived state, the coverage manifest and the operator's build
     * description, so `code sem-status` answers "is semantic evidence from this
     * repository trustworthy, and if not what would fix it" without anybody
     * opening the database. §24 asks for exactly that.
     *
     * The plan carries freshness and coverage as *separate* fields and the
     * renderers print them as separate lines. A single badge combining them is
     * the presentation A9.1, A9.2 and A9.2.2 each exist to prevent, and it would
     * hide the one state this season adds: source-current and coverage
     * incomplete. */
    atlas_sem_plan plan;
    /* Newline-separated, repository-relative, as the operator wrote them.
     * Owned. Untrusted only in the weak sense that an operator chose them, but
     * still encoded at the point of output like every other path. */
    atlas_buf compdbs;
    atlas_buf test_roots;

    /* --- A9.2.4 -------------------------------------------------------------
     *
     * The rest of the build description, and what discovery actually found.
     *
     * The candidate list carries **rejected** candidates as well as accepted
     * ones, with the reason for each. That is the difference between a status
     * that says "two build inputs" and one that says "two accepted, one refused
     * because it is a symlink, one because it does not parse" — and the second
     * is what somebody debugging a coverage gap needs. A rejected candidate
     * nobody is shown is indistinguishable from a candidate that does not
     * exist. */
    atlas_buf excludes;
    atlas_buf vendor_roots;
    struct atlas_sem_input *inputs; /* owned; ATLAS_SEM_DISCOVERY_MAX_CANDIDATES */
    size_t input_count;

    /* --- A9.2.5 -------------------------------------------------------------
     *
     * Every place the last walk could not account for, each with the exact
     * repository-relative path it is about. A9.2.4 kept the *first* reason and
     * no path, so one declared `--exclude` masked every unreadable directory for
     * the rest of the walk. `plan.discovery_limit` is still the one-line summary;
     * this is what an operator acts on. */
    struct atlas_sem_obstacle *obstacles; /* owned; ATLAS_SEM_DISCOVERY_MAX_OBSTACLES */
    size_t obstacle_count;
    bool obstacles_truncated;
} atlas_sem_status_report;

void atlas_sem_status_report_init(atlas_sem_status_report *r);
void atlas_sem_status_report_free(atlas_sem_status_report *r);

atlas_status atlas_service_sem_status(atlas_ctx *ctx, const char *name,
                                      atlas_sem_status_report *out, atlas_err *err);

typedef struct atlas_sem_symbol_item {
    char usr[ATLAS_SEM_MAX_USR_BYTES];
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    char kind[32];
    char linkage[32];
    char type_text[ATLAS_SEM_MAX_TYPE_BYTES];
    char file_text[512];
    char evidence[16];
    int64_t line;
    int64_t col;
    int64_t end_line;
    bool is_definition;
    bool external;
} atlas_sem_symbol_item;

typedef struct atlas_sem_symbols_report {
    atlas_repo_info repo;
    atlas_sem_generation generation;
    atlas_sem_freshness freshness;
    const char *stale_reason;
    /* A9.2.5. What this answer is worth. `freshness` and `stale_reason` above
     * are kept because every existing reader uses them and nothing public is
     * removed; they are also inside `trust`, which is what the renderers and the
     * IPC server both emit from. */
    atlas_sem_trust trust;
    char query[ATLAS_SEM_MAX_NAME_BYTES];
    atlas_sem_symbol_item *items;
    size_t count;
    size_t cap;
    int64_t total;
    bool truncated;
} atlas_sem_symbols_report;

void atlas_sem_symbols_report_init(atlas_sem_symbols_report *r);
void atlas_sem_symbols_report_free(atlas_sem_symbols_report *r);

atlas_status atlas_service_sem_symbol(atlas_ctx *ctx, const char *name, const char *symbol,
                                      const char *kind, int64_t limit,
                                      atlas_sem_symbols_report *out, atlas_err *err);

typedef struct atlas_sem_graph_item {
    int64_t depth;
    char usr[ATLAS_SEM_MAX_USR_BYTES];
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    char file_text[512];
    char edge_kind[32];
    char via_name[ATLAS_SEM_MAX_NAME_BYTES];
    char evidence[16];
    char site_file[512];
    int64_t line;
    int64_t site_line;
    int64_t candidate_total;
} atlas_sem_graph_item;

typedef struct atlas_sem_graph_report {
    atlas_repo_info repo;
    atlas_sem_generation generation;
    atlas_sem_freshness freshness;
    const char *stale_reason;
    atlas_sem_trust trust; /* A9.2.5 */
    char query[ATLAS_SEM_MAX_NAME_BYTES * 2 + 8];
    bool inbound;
    atlas_sem_graph_item *items;
    size_t count;
    size_t cap;
    atlas_sem_walk_summary summary;
} atlas_sem_graph_report;

void atlas_sem_graph_report_init(atlas_sem_graph_report *r);
void atlas_sem_graph_report_free(atlas_sem_graph_report *r);

/* Callers (`inbound`) or callees. Depth 1 is the direct answer; deeper is the
 * bounded transitive one. */
atlas_status atlas_service_sem_graph(atlas_ctx *ctx, const char *name, const char *symbol,
                                     bool inbound, int64_t depth, int64_t limit, bool proven_only,
                                     atlas_sem_graph_report *out, atlas_err *err);

atlas_status atlas_service_sem_trace(atlas_ctx *ctx, const char *name, const char *from,
                                     const char *to, int64_t depth, atlas_sem_graph_report *out,
                                     atlas_err *err);

/* The one mutating semantic operation.
 *
 * Under A7.1 the index is 0700 `atlasd`, so an operator's CLI cannot perform
 * this locally: it routes over the socket and the daemon offers the method only
 * to the peer the root-owned policy names. Both paths call this function, so
 * the local and remote forms cannot drift. */
atlas_status atlas_service_sem_index(atlas_ctx *ctx, const char *name, const char *const *compdbs,
                                     size_t compdb_count, bool rebuild,
                                     atlas_sem_index_summary *out, atlas_err *err);

/* The indexing core, over a raw handle and an already-resolved repository.
 *
 * The CLI reaches it through `atlas_service_sem_index`, which resolves the
 * repository from a context it owns; the daemon's writer thread reaches it
 * directly, because it already holds the only writable handle and has resolved
 * the repository from the registry. One implementation, for the reason
 * `atlas_sem_impact_on` and `atlas_sem_context_on` are one each.
 *
 * It creates git and parser processes, so it must never be called with a write
 * transaction open.
 *
 * `yield`, when supplied, is offered to the pass at the points where nothing is
 * open — between translation units, and once either side of the unit loop — so
 * the thread running it can be lent to something short. It changes nothing the
 * pass produces: the generation is byte for byte what it would have been. NULL
 * is the ordinary case, and is what the CLI passes: a local `code index` has one
 * thread and nothing else waiting for it. */
/* A13: `data_dir` is where a mirror lives, or NULL for the tree itself -- see
 * `atlas_repo_open_git`. A semantic pass reads sources, so a repository the
 * daemon cannot open is one it can never index. */
/* A13. Points a repository row at the tree this process actually reads: the
 * mirror for a scanner-backed repository, the registered root otherwise.
 *
 * **One question, asked in every place that reads.** The semantic layer takes a
 * path from the row in three places -- the discovery walk, the `root_fd` behind
 * `live_facts`, and the index pass -- and each meant the registered root when it
 * was written. Correcting them one at a time is how this season spent four
 * days. */
atlas_status atlas_sem_repo_read_root(atlas_repo_info *repo, const char *data_dir,
                                      atlas_err *err);

/* A13.1. Build-input discovery on the tree this process reads: corrects `repo`'s
 * root with `atlas_sem_repo_read_root`, then runs the walk. Every caller outside
 * `src/sem` uses this rather than `atlas_sem_discovery_run`, because three call
 * sites reached the raw walk and only one of them remembered to correct the row
 * first -- see the comment at the definition. `repo` is mutated in place, the
 * way `atlas_sem_repo_read_root` mutates it. */
atlas_status atlas_sem_discovery_run_on(atlas_db *db, const char *data_dir, atlas_repo_info *repo,
                                        void (*yield)(void *ud), void *yield_ud,
                                        atlas_sem_discovery_result *out, atlas_err *err);

/* `cancel` is polled between translation units and, when it answers true, the
 * generation is abandoned rather than published — `atlas_sem_index_opts.cancel`,
 * carried through untouched. It is separate from `yield` because the two answer
 * opposite questions: `yield` lends the thread and cannot fail the pass, and
 * this one ends it.
 *
 * The daemon supplies it so that shutting down does not have to wait for a pass
 * over every unit the build describes. Without it `atlas_writer_stop` set
 * `stopping`, which is read at every submission point and in the idle wait and
 * nowhere inside a running job, and then joined a thread that had minutes of
 * work left. A CLI caller passes NULL: the process running the pass is the
 * process that asked for it, and it has no shutdown of its own to observe. */
atlas_status atlas_sem_index_on(atlas_db *db, const char *data_dir, const atlas_repo_info *repo,
                                const char *const *compdbs, size_t compdb_count, bool rebuild,
                                void (*yield)(void *ud), void *yield_ud,
                                bool (*cancel)(void *ud), void *cancel_ud,
                                atlas_sem_index_summary *out, atlas_err *err);

/* The daemon-served form: queue the index on the writer thread and poll until
 * it finishes. The service keeps running throughout, and no client ever has to
 * become the service account. */
atlas_status atlas_service_sem_index_remote(const char *name, const char *const *compdbs,
                                            size_t compdb_count, bool rebuild,
                                            atlas_sem_index_summary *out, atlas_err *err);

/* --- A9.2.3: writing the durable build description --------------------------
 *
 * The one operation that decides whether this daemon will run a compiler over a
 * repository when that repository changes. It is an operator action for exactly
 * that reason — A8-CI's rule that indexing runs a compiler and is therefore
 * authorised — and it has no MCP tool and no gateway route.
 *
 * `auto_rebuild` is a tri-state: negative leaves the stored value alone,
 * zero disables, positive enables. That distinction matters because an operator
 * adjusting a compilation-database list must not silently turn automatic
 * rebuilding on or off as a side effect of a command about something else.
 *
 * `compdbs` and `test_roots` are NULL to leave the stored lists unchanged, and a
 * zero count with a non-NULL array clears one. Paths are repository-relative;
 * they are validated inside the root by the indexer, never here, because this
 * function must not need a repository on disk to record an operator's
 * intention — a description can legitimately be written before the tree it
 * describes has been built. */
/* A9.2.4. One build-description write, in the caller's array form.
 *
 * A struct rather than a widening parameter list, because the season adds four
 * more optional lists and two more tri-states and a nine-argument function whose
 * arguments are all `const char *const *` and `size_t` is one somebody will
 * eventually mis-order silently.
 *
 * Every list follows the same rule: NULL leaves the stored value alone, and a
 * non-NULL pointer with a zero count clears it. An operator adjusting their test
 * roots must not silently drop their exclusions, and vice versa. Every tri-state
 * follows the same rule too: negative leaves it alone. */
typedef struct atlas_sem_config_request {
    const char *name;
    const char *const *compdbs;
    size_t compdb_count;
    const char *const *test_roots;
    size_t test_root_count;
    const char *const *excludes;
    size_t exclude_count;
    const char *const *vendor_roots;
    size_t vendor_root_count;
    /* Negative leaves the stored intent alone; zero records an operator's
     * explicit DISABLED; positive records an operator's explicit ENABLED. Both
     * non-negative values also record OPERATOR provenance, which is what makes
     * a deliberate refusal distinguishable from a migrated default for ever. */
    int auto_rebuild;
    /* Negative leaves it alone; zero is AUTOMATIC; positive is MANUAL. */
    int discovery_mode;
} atlas_sem_config_request;

atlas_status atlas_service_sem_config_set(atlas_ctx *ctx, const atlas_sem_config_request *req,
                                          atlas_sem_status_report *out, atlas_err *err);

/* One build-description write, in the storage form.
 *
 * Here rather than in the daemon's internal header because both the CLI and the
 * writer thread build one: it is a service request, not a daemon detail, and a
 * second copy of its shape is how the two paths would start disagreeing about
 * what "leave this list alone" means. */
typedef struct atlas_sem_config_job {
    const char *repo_name;
    /* NUL-separated lists, or NULL to leave the stored value alone. A non-NULL
     * pointer with a zero length clears one. */
    const char *compdbs;
    size_t compdbs_len;
    const char *test_roots;
    size_t test_roots_len;
    /* A9.2.4. The same rule, for the two lists that bound the search universe. */
    const char *excludes;
    size_t excludes_len;
    const char *vendor_roots;
    size_t vendor_roots_len;
    /* Negative leaves the stored value alone, zero disables, positive enables.
     * A9.2.4: a non-negative value records an *operator* intent with OPERATOR
     * provenance, which is what a machine-wide default can never overrule. */
    int auto_rebuild;
    /* Negative leaves it alone; zero is AUTOMATIC; positive is MANUAL. */
    int discovery_mode;
} atlas_sem_config_job;

/* The raw-handle cores, for the reason `atlas_sem_index_on` and
 * `atlas_sem_impact_on` exist: the daemon's writer thread has a handle and no
 * `atlas_ctx`, and one implementation is what makes the two surfaces agree by
 * construction rather than by being kept in step. */
atlas_status atlas_sem_config_on(atlas_db *db, const char *data_dir,
                                 const atlas_sem_config_job *job,
                                 atlas_sem_status_report *out, atlas_err *err);
atlas_status atlas_sem_status_on(atlas_db *db, const char *name, atlas_sem_status_report *out,
                                 atlas_err *err);

/* The daemon-served form. Routed on `atlas_ctx_is_writer` rather than on
 * `ctx != NULL`: with a daemon running, a context in AUTO mode still opens
 * read-only, so the weaker test fails with "attempt to write a readonly
 * database" — the A9.2.1 defect, and it is not repeated. */
atlas_status atlas_service_sem_config_set_remote(const atlas_sem_config_request *req,
                                                 atlas_sem_status_report *out, atlas_err *err);

/* The daemon-served forms. Same reports, same renderers; only the transport
 * differs, which is what keeps a local answer and a socket answer from drifting
 * apart. Under A7.1 these are the only forms that work on a deployed machine:
 * the index is 0700 `atlasd`, so an operator has no local handle at all. */
/* The impact core, over a raw read-only handle and an already-resolved
 * repository. The CLI and the daemon both call this, which is what makes their
 * answers identical by construction rather than by two functions kept in step. */
atlas_status atlas_sem_impact_on(atlas_db *db, const atlas_repo_info *repo, const char *subject,
                                 int64_t depth, int64_t limit, atlas_sem_impact_report *out,
                                 atlas_err *err);

/* Bounded change-impact for a symbol or a repository-relative path. Every item
 * carries its evidence class and the fixed reason that selected it. */
atlas_status atlas_service_sem_impact(atlas_ctx *ctx, const char *name, const char *subject,
                                      int64_t depth, int64_t limit,
                                      atlas_sem_impact_report *out, atlas_err *err);

/* The deterministic task-context package. Reads only; task text ranks evidence
 * and authorises nothing. */
atlas_status atlas_sem_context_on(atlas_db *db, const atlas_repo_info *repo,
                                  const atlas_sem_context_req *req,
                                  atlas_sem_context_report *out, atlas_err *err);

atlas_status atlas_service_sem_context(atlas_ctx *ctx, const atlas_sem_context_req *req,
                                       atlas_sem_context_report *out, atlas_err *err);

atlas_status atlas_service_sem_status_remote(const char *name, atlas_sem_status_report *out,
                                             atlas_err *err);
atlas_status atlas_service_sem_symbol_remote(const char *name, const char *symbol,
                                             const char *kind, int64_t limit,
                                             atlas_sem_symbols_report *out, atlas_err *err);
atlas_status atlas_service_sem_graph_remote(const char *name, const char *symbol, bool inbound,
                                            int64_t depth, int64_t limit, bool proven_only,
                                            atlas_sem_graph_report *out, atlas_err *err);
atlas_status atlas_service_sem_trace_remote(const char *name, const char *from, const char *to,
                                            int64_t depth, atlas_sem_graph_report *out,
                                            atlas_err *err);
atlas_status atlas_service_sem_impact_remote(const char *name, const char *subject, int64_t depth,
                                             int64_t limit, atlas_sem_impact_report *out,
                                             atlas_err *err);
atlas_status atlas_service_sem_context_remote(const atlas_sem_context_req *req,
                                              atlas_sem_context_report *out, atlas_err *err);

/* --- A12.1 T16: the `memory` command family ---------------------------------
 *
 * One render struct and one sink, `job_item`'s own philosophy carried across
 * seven sub-commands instead of two shapes of one: `form` says which of
 * `status`/`scan`/`reconcile`/`pack`/`diff`/`patch`/`trailer` produced this
 * row, so a missing renderer branch is a silent gap in one method rather than
 * a whole method nobody implemented. Every member is a borrowed pointer or a
 * plain scalar, valid only for the sink call, exactly as `atlas_job_render`
 * documents for the identical reason.
 *
 * `status`, `scan` and `reconcile` speak to the daemon's existing operator
 * RPC methods (`memory.status`, `memory.put`, `memory.reconcile`, T11) —
 * `scan`/`reconcile` unconditionally, because A9.2.7's writer-thread job queue
 * has no local equivalent to call into, the same fact that makes A11.1's run
 * driver have no offline path either. `pack`, `diff`, `patch` and `trailer`
 * read already-materialised rows through `atlas_ctx_db(ctx)` and have **no
 * remote form in this build** — `ctx == NULL` (an A7.1 deployment where the
 * index is 0700 `atlasd`) refuses with a stated reason rather than
 * dereferencing a NULL handle. This is a disclosed gap, not an oversight: see
 * the T16 report for the reasoning and the backlog entry it recommends. */

typedef struct atlas_memory_source_render {
    const char *uid;
    const char *cls;             /* atlas_memory_source_class_name() */
    const char *path;             /* path_text; already encoded, printed as-is */
    const char *registered_at;    /* raw; atlas_safe() at render */
    bool has_version;
    const char *version_uid;
    const char *content_sha256;
    int64_t content_bytes;
    const char *observed_at;      /* raw; atlas_safe() at render */
    const char *commit_oid;
} atlas_memory_source_render;

typedef struct atlas_memory_render {
    /* "status" | "scan" | "reconcile" | "pack" | "diff" | "patch" | "trailer" */
    const char *form;
    bool in_list;   /* this call is one row inside list_begin/list_end */
    bool detail;    /* job_item's own flag: a full view rather than a summary row */

    const char *repo;

    /* status: the root-owned system policy, loaded locally regardless of
       transport -- a file read, never a database read, so it is always
       present even when nothing else on this row could be determined.
       `gateway status`'s own precedent, carried across the transport split it
       does not have to make (see the T16 report's §5 finding). */
    const char *policy_state;         /* "SYSTEM" | "LEGACY" */
    const char *policy_reason;
    const char *policy_reason_detail;
    const char *policy_path;          /* the compiled-in constant */

    /* status */
    const char *plan_for;             /* atlas_memory_gen_cause_name() */
    int64_t generation;
    bool generation_found;
    const char *head_commit;
    atlas_memory_source_render sources[ATLAS_MEMORY_MAX_SOURCES];
    size_t source_count;
    bool sources_truncated;   /* true iff the repository has more registered
                                  sources than this row array holds -- rows are
                                  never deleted, so a policy edited over time
                                  can exceed ATLAS_MEMORY_MAX_SOURCES; a caller
                                  must be told rather than shown a silently
                                  short list. */

    /* scan: one row per source read, or per put outcome. `scan_no_sources` is
       the finding a silent zero-put success must never produce: nothing
       registered, or nothing materialised because no reconciliation pass has
       run yet. */
    const char *scan_source_uid;
    const char *scan_rel_path;        /* path_text; empty for a *_FILE source */
    const char *scan_outcome;         /* Atlas-authored token */
    bool scan_put;
    const char *scan_version_uid;
    const char *scan_content_sha256;
    int64_t scan_content_bytes;
    bool scan_created;
    bool scan_no_sources;

    /* reconcile */
    bool accepted;

    /* pack. `pack_preview` is true for a freshly built `--task` pack (no
       freshness verdict: nothing pinned it yet) and false for a frozen
       `--run` one (`pack_found` says whether that run has one at all).
       T13's reliance fields -- context §7's question, answered in the T16
       report: rendered here, all three, undistorted into one bit. */
    bool pack_found;
    bool pack_other_repo;             /* true iff --run named a pack that
                                          exists but belongs to a different
                                          repository -- kept apart from
                                          "no such run has a pack" (pack_found
                                          false, pack_other_repo false), which
                                          is a different fact and a caller
                                          must not be told the wrong one. */
    bool pack_preview;
    const char *pack_status;          /* CURRENT | STALE; empty for a preview */
    const char *pack_which_moved;
    int64_t pack_claim_count;
    int64_t pack_excluded_count;
    int64_t pack_unanchored_count;
    const char *pack_digest;
    const char *pack_body;            /* already atlas-safe-1 encoded */
    bool reliance_checked;
    bool reliance_complete;
    const char *reliance_claim_uids;  /* decoded, comma-joined; uids only */

    /* diff: one row per claim in the named generation */
    int64_t diff_generation;
    bool diff_generation_found;
    const char *diff_claim_uid;
    const char *diff_kind;            /* atlas_memory_diff_kind_name() */
    const char *diff_reason;

    /* patch */
    const char *patch_source_uid;
    const char *patch_diff;           /* already atlas-safe-1 encoded unified diff */
    const char *patch_findings;       /* already atlas-safe-1 encoded, one per line */

    /* trailer: composed (`--run`+`--reason`) or shown (`--commit`+`--repo`) */
    bool trailer_compose;
    const char *trailer_block;        /* composed text; uids and digests only */
    bool trailer_found;               /* show mode: whether a binding row exists */
    bool trailer_has_block;
    bool trailer_bound_hit;
    const char *trailer_run;
    int64_t trailer_generation;
    bool trailer_context_digest_ok;
    bool trailer_decision_set_ok;
    const char *trailer_change_reason_uid;
    const char *trailer_unknown_fields; /* decoded, comma-joined */
} atlas_memory_render;

typedef atlas_status (*atlas_memory_sink)(const atlas_memory_render *mr, void *ud, atlas_err *err);

atlas_status atlas_service_memory_status(atlas_ctx *ctx, const char *repo, atlas_memory_sink sink,
                                         void *ud, atlas_err *err);
atlas_status atlas_service_memory_status_remote(const char *repo, atlas_memory_sink sink, void *ud,
                                                atlas_err *err);

/* No `ctx` parameter: unconditionally daemon-served, `atlas_service_run_drive`'s
   own "no offline path" reason -- discovery goes through `memory.status` and
   the write through `memory.put`, and neither has a local implementation to
   fall back to. */
atlas_status atlas_service_memory_scan(const char *repo, atlas_memory_sink sink, void *ud,
                                       int64_t *count_out, atlas_err *err);
atlas_status atlas_service_memory_reconcile(const char *repo, atlas_memory_sink sink, void *ud,
                                            atlas_err *err);

/* Local-only (`ctx` required); see the section comment above. */
atlas_status atlas_service_memory_pack(atlas_ctx *ctx, const char *repo, const char *task,
                                       const char *run, atlas_memory_sink sink, void *ud,
                                       atlas_err *err);
atlas_status atlas_service_memory_diff(atlas_ctx *ctx, const char *repo, int64_t generation,
                                       atlas_memory_sink sink, void *ud, int64_t *count_out,
                                       atlas_err *err);
atlas_status atlas_service_memory_patch(atlas_ctx *ctx, const char *repo, const char *source_uid,
                                        atlas_memory_sink sink, void *ud, atlas_err *err);
atlas_status atlas_service_memory_trailer(atlas_ctx *ctx, const char *run, const char *reason,
                                          const char *commit, const char *repo,
                                          atlas_memory_sink sink, void *ud, atlas_err *err);

#endif /* ATLAS_SERVICE_H */

/* --- A9.2.1: the verification surface over the socket -----------------------
 *
 * The remote twins of the functions above. Under A7.1 the index is 0700
 * `atlasd`, so from the operator's account these are the *only* forms that can
 * answer — A9.2.1 shipped the nine RPC methods without them, and the result was
 * `atlas verify show` reporting "no index is available to read" about a claim
 * the daemon was holding. The CLI picks between the pair on whether it has a
 * context, which is the shape `atlas_service_gate_show` already uses. */
atlas_status atlas_service_verify_show_remote(int64_t claim_id, const char *claim_uid,
                                             atlas_verify_report *out, atlas_err *err);
/* Carries one typed intake operation to its method, and reads the answer back
 * into the same result struct the local write point fills — including the
 * assessment EVALUATE returns, so the two paths render identically. */
atlas_status atlas_service_verify_intake_remote(const atlas_verify_op *op,
                                               atlas_verify_intake_result *out, atlas_err *err);

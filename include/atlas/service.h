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
#include "atlas/code.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/gate.h"
#include "atlas/git.h"
#include "atlas/limits.h"
#include "atlas/reconcile.h"
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

/* The counts a listing reports alongside its page. */
typedef struct atlas_decision_counts {
    int64_t proposed;
    int64_t approved;
    int64_t rejected;
    int64_t superseded;
} atlas_decision_counts;

/* One resolved link, ready to render. */
typedef struct atlas_decision_link_view {
    atlas_buf kind;
    atlas_buf value;    /* the path, commit, symbol or target id, safe-encoded */
    atlas_buf detail;   /* the symbol's file or kind, when there is one */
    atlas_buf currency; /* CURRENT | CHANGED | MISSING | AMBIGUOUS | UNKNOWN */
    atlas_buf analyzer;
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
} atlas_decision_timeline_entry;

/* What a lifecycle write reports back. */
typedef struct atlas_decision_outcome {
    atlas_buf repo;
    atlas_buf uid;
    atlas_buf state;
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
    const char *const *alternatives;
    size_t alternative_count;
    const char *const *paths;
    size_t path_count;
    const char *const *commits;
    size_t commit_count;
    const char *const *symbols;
    size_t symbol_count;
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

atlas_status atlas_service_decision_propose(atlas_ctx *ctx, const char *repo,
                                            const atlas_decision_input *in,
                                            atlas_decision_outcome *out, atlas_err *err);
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

#endif /* ATLAS_SERVICE_H */

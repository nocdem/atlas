/* Atlas - daemon internals shared between the src/daemon translation units.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header.
 */
#ifndef ATLAS_DAEMON_INTERNAL_H
#define ATLAS_DAEMON_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/ai.h"
#include "atlas/buf.h"
#include "atlas/daemon.h"
#include "atlas/db.h"
#include "atlas/decision_ops.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/workers.h"

/* --- the writer job queue ------------------------------------------------
 *
 * Bounded, with real backpressure. A queue that grows without limit turns a
 * burst of filesystem events into unbounded memory, and a queue that drops
 * silently turns it into a lost change. Neither is acceptable, so a full queue
 * refuses the submission and the submitter is told.
 *
 * For reconciliation that refusal is harmless: a pending pass for the same
 * repository already covers the change, so a duplicate submission is folded
 * into it rather than queued twice. That is the coalescing that makes a burst of
 * writes cost one pass. */

typedef enum atlas_job_kind {
    ATLAS_JOB_RECONCILE = 0,
    ATLAS_JOB_REPO_ADD,
    ATLAS_JOB_REPO_REMOVE,
    /* The watcher observes the conditions that produce these but holds a
     * read-only handle, so it hands them to the writer rather than acquiring a
     * second write path into the index. */
    ATLAS_JOB_MARK_GAP,
    ATLAS_JOB_SET_WATCH,
    /* A2. Every AI-session mutation is one job kind carrying one typed
     * operation, rather than a job kind per verb: the validation happens in the
     * IPC layer before anything is queued, and the writer's switch stays a
     * switch rather than becoming a second dispatch table that can drift from
     * the first. */
    ATLAS_JOB_AI,
    /* A4. One job kind carrying one typed decision operation, for exactly the
     * reasons ATLAS_JOB_AI is one: validation happens in the IPC layer before
     * anything is queued, and the writer's switch stays a switch.
     *
     * Every lifecycle transition comes through here, including the ones the
     * operator channel authorises — a challenge is a write, so issuing one is a
     * writer job too. There is no path to `atlas_decision_apply` that does not
     * run on the writer thread. */
    ATLAS_JOB_DECISION
} atlas_job_kind;

typedef struct atlas_job atlas_job;

struct atlas_job {
    atlas_job_kind kind;
    int64_t repo_id;
    bool full;         /* reconcile: re-read every file */
    /* A3: drop every structural row and reindex. Distinct from `full`, which is
     * about re-reading file *content*: a full pass rehashes bytes and still
     * parses nothing when the hashes match, so rebuilding the graph needs its
     * own flag rather than being implied by one. */
    bool code_rebuild;
    int64_t sync_seq;  /* echoed into the published state */
    int watch_state;   /* set watch: an atlas_watch_state */
    int64_t watched_dirs;
    atlas_buf arg1;    /* repo add: path, repo remove: name, gap/watch: detail */
    atlas_buf arg2;    /* repo add: name (may be empty) */
    /* repo add: refuse unless `arg1` is itself the worktree root. Set by the
     * MCP path, where the caller granted one directory and registering its
     * parent would index what was not granted. */
    bool exact_root;
    /* Reconcile: repository-relative paths the watcher saw an event for, NUL
     * separated. Each is hashed regardless of its metadata. */
    atlas_buf dirty_paths;

    /* Completion handshake, used only by the jobs a caller waits on.
     *
     * The result is carried as typed fields rather than as a JSON fragment. A
     * fragment would have to be spliced into the response document verbatim,
     * and a "write these bytes as JSON" primitive is exactly the hole through
     * which an unescaped value eventually reaches a client. */
    bool wants_result;
    bool done;
    atlas_status result;
    atlas_err result_err;
    int64_t result_id;
    atlas_buf result_name;      /* validated [A-Za-z0-9._-] */
    atlas_buf result_root_text; /* already in the safe text encoding */

    /* A2. The job owns the operation and frees it. The result is typed for the
     * same reason the fields above are: a JSON fragment carried through here
     * would have to be spliced into a response verbatim. */
    atlas_ai_op *ai;
    atlas_ai_result ai_result;

    /* A4. Same ownership rule as `ai`: the job owns the operation and frees it,
     * and the result is typed rather than a JSON fragment. */
    atlas_decision_op *decision;
    atlas_decision_result decision_result;
};

/* What a completed mutation reports back. */
typedef struct atlas_writer_result {
    int64_t id;
    atlas_buf name;
    atlas_buf root_text;
} atlas_writer_result;

void atlas_writer_result_init(atlas_writer_result *r);
void atlas_writer_result_free(atlas_writer_result *r);

typedef struct atlas_writer atlas_writer;

/* Starts the writer thread. It opens its own writable database handle from
 * `db_path`; no handle is passed in, because a handle created on another thread
 * and used here is exactly the sharing the model forbids. */
atlas_status atlas_writer_start(const char *db_path, const char *socket_path,
                                atlas_workers *workers, FILE *log, atlas_writer **out,
                                atlas_err *err);
void atlas_writer_stop(atlas_writer *w);

/* Queues a reconciliation. Coalesces with an already-pending pass for the same
 * repository. `sync_seq_out` receives the sequence number the completed pass
 * will publish, so a caller can wait for it.
 *
 * `dirty_paths` names repository-relative paths the caller observed an event
 * for, NUL separated; each is hashed regardless of its metadata. Coalescing
 * merges the sets, because a pending pass has to cover both requests. */
atlas_status atlas_writer_submit_reconcile(atlas_writer *w, int64_t repo_id, bool full,
                                           bool code_rebuild,
                                           const char *dirty_paths, size_t dirty_len,
                                           int64_t *sync_seq_out, atlas_err *err);

/* Queues a mutation and waits for it, bounded by `timeout_ms`. */
atlas_status atlas_writer_call(atlas_writer *w, atlas_job_kind kind, const char *arg1,
                               const char *arg2, int timeout_ms, atlas_writer_result *result,
                               atlas_err *err);
/* The same, for a repository registration that must not resolve upward. See
 * `atlas_job.exact_root`. */
atlas_status atlas_writer_call_repo_add(atlas_writer *w, const char *path, const char *name,
                                        bool exact_root, int timeout_ms,
                                        atlas_writer_result *result, atlas_err *err);

/* Queues one AI-session operation and waits for it, bounded by `timeout_ms`.
 *
 * Takes ownership of `op` unconditionally — including on the failure paths —
 * so a caller has exactly one thing to do with it. A correlate operation may ask
 * for a reconciliation; the writer supplies that callback itself, which is what
 * keeps src/ai free of any knowledge of the job queue. */
atlas_status atlas_writer_ai(atlas_writer *w, atlas_ai_op *op, int timeout_ms,
                             atlas_ai_result *result, atlas_err *err);

/* Queues one decision-lifecycle operation and waits for it, bounded by
 * `timeout_ms`.
 *
 * Takes ownership of `op` unconditionally, including on the failure paths, so a
 * caller has exactly one thing to do with it — the same contract
 * `atlas_writer_ai` has, and for the same reason: an ownership rule that
 * depends on the outcome is one that leaks on the path nobody tests. */
atlas_status atlas_writer_decision(atlas_writer *w, atlas_decision_op *op, int timeout_ms,
                                   atlas_decision_result *result, atlas_err *err);

/* Records that changes may have been missed for a repository. Fire and forget:
 * the watcher has nothing useful to do with a failure here, and the periodic
 * full pass covers the case where the flag did not get written. */
atlas_status atlas_writer_submit_gap(atlas_writer *w, int64_t repo_id, const char *detail,
                                     atlas_err *err);
/* Records the watcher's own state for a repository. */
atlas_status atlas_writer_submit_watch_state(atlas_writer *w, int64_t repo_id, int watch_state,
                                             const char *detail, int64_t watched_dirs,
                                             atlas_err *err);

/* Depth of the queue right now, for `daemon status`. */
int64_t atlas_writer_queue_depth(atlas_writer *w);
/* Reconciliation passes completed since start, for `daemon status`. */
int64_t atlas_writer_passes(atlas_writer *w);

/* Set by the writer when a repository set change means the watcher must
 * re-derive its watches. The watcher clears it. */
bool atlas_writer_take_watch_dirty(atlas_writer *w);
void atlas_writer_set_watch_dirty(atlas_writer *w);

/* --- the watcher --------------------------------------------------------- */

typedef struct atlas_watcher atlas_watcher;

/* Starts the inotify watcher. It opens its own read-only database handle to
 * enumerate repositories, and submits reconciliations through `writer`. */
atlas_status atlas_watcher_start(const char *db_path, atlas_writer *writer, FILE *log,
                                 int reconcile_interval_ms, atlas_watcher **out, atlas_err *err);
void atlas_watcher_stop(atlas_watcher *w);
/* Total inotify watches currently installed, for `daemon status`. */
int64_t atlas_watcher_watch_count(atlas_watcher *w);
/* True once the initial reconciliation of every repository has been submitted. */
bool atlas_watcher_primed(atlas_watcher *w);

/* --- the IPC serve loop -------------------------------------------------- */

typedef struct atlas_server_ctx {
    const char *db_path;
    const char *data_dir;
    const char *socket_path;
    atlas_writer *writer;
    atlas_watcher *watcher;
    atlas_workers *workers;
    FILE *log;
    int64_t started_at_ms;
} atlas_server_ctx;

/* Serves until `stop` becomes true. `listen_fd` is owned by the caller. */
atlas_status atlas_server_serve(atlas_server_ctx *ctx, int listen_fd, int signal_fd,
                                atomic_bool *stop, atlas_err *err);

/* Handles one request payload and produces one response payload. Exposed so the
 * protocol can be tested without a socket. */
atlas_status atlas_server_dispatch(atlas_server_ctx *ctx, const void *payload, size_t len,
                                   atlas_buf *response, atlas_err *err);

/* --- logging ------------------------------------------------------------- */

/* One line, timestamped, with every untrusted value already safe-encoded by the
 * caller. Never NULL-checks away a message: a dropped log line is a dropped
 * explanation. */
void atlas_daemon_log(FILE *log, const char *level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* ATLAS_DAEMON_INTERNAL_H */

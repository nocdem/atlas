/* Atlas - the single writer thread and its bounded job queue.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every SQLite write in the daemon happens on this thread, on a handle this
 * thread created and never shares. That is the whole concurrency model: readers
 * get their own read-only handles and WAL lets them run alongside, and there is
 * exactly one writer, so SQLITE_BUSY on the write path is not something to be
 * papered over with a longer busy_timeout — it should not arise at all, and if
 * it does it means something outside the daemon took the write lock, which is
 * what the data-directory lock exists to prevent.
 */
#define _GNU_SOURCE 1

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/git.h"
#include "atlas/reconcile.h"
#include "atlas/safetext.h"
#include "atlas/maintenance.h"
#include "atlas/ops.h"
#include "atlas/sem_discover.h"
#include "atlas/sem.h"
#include "atlas/sem_ops.h"
#include "atlas/service.h"
#include "daemon/daemon_internal.h"

struct atlas_writer {
    pthread_t thread;
    bool thread_started;

    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t job_done;
    /* Signalled once the writer has opened the index and applied every pending
     * migration, so nothing else in the daemon runs against a schema that does
     * not exist yet. See the wait in atlas_writer_start. */
    pthread_cond_t ready_cv;

    /* --- guarded by lock --- */
    bool stopping;
    bool ready;        /* the index is open and migrated */
    bool ready_failed; /* it could not be, and `ready_err` says why */
    atlas_err ready_err;
    atlas_job *queue[ATLAS_WRITER_QUEUE_MAX];
    size_t head;
    size_t count;
    int64_t next_sync_seq;
    int64_t passes;
    bool watch_dirty;
    /* A9.2.3. The repository whose semantic index this thread is building right
     * now, or 0. Guarded by `lock` and set around the whole job rather than
     * around the generation, because the window that matters is the one before
     * a generation exists: a job dequeued but not yet started is invisible in
     * the durable record, and a scheduler that only looked there would queue a
     * second build of the same repository. */
    int64_t sem_index_running_repo;
    /* A9.2.6. What this thread is executing right now, and whether it is
     * executing anything at all.
     *
     * `running_kind` is meaningless unless `running` is set, so the two are
     * always read together: there is no idle value to reserve in the enum, and
     * inventing one would put a member in `atlas_job_kind` that names no job.
     *
     * Both are set in the same lock hold that dequeues the job and cleared in
     * the same one that completes it, so a waiter never observes the writer as
     * idle while its own job is still unfinished, nor as busy after the job it
     * was waiting for has been handed back. */
    bool running;
    atlas_job_kind running_kind;

    atlas_workers *workers;
    FILE *log;
    atlas_buf db_path;
    atlas_buf socket_path;
    atlas_db *db; /* owned by the writer thread only */
    /* Where a semantic-index job records its outcome. Not owned. */
    atlas_ops *ops;
};

/* --- logging ------------------------------------------------------------- */

void atlas_daemon_log(FILE *log, const char *level, const char *fmt, ...) {
    if (log == NULL) {
        return;
    }
    char ts[ATLAS_TS_MAX];
    atlas_now_iso8601(ts, sizeof(ts));
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(log, "%s %-5s ", ts, level);
    (void)vfprintf(log, fmt, ap);
    (void)fputc('\n', log);
    va_end(ap);
    (void)fflush(log);
}

/* --- job lifecycle ------------------------------------------------------- */

static atlas_job *job_new(atlas_job_kind kind) {
    atlas_job *j = calloc(1u, sizeof(*j));
    if (j == NULL) {
        return NULL;
    }
    j->kind = kind;
    atlas_buf_init(&j->arg1);
    atlas_buf_init(&j->arg2);
    atlas_buf_init(&j->dirty_paths);
    atlas_buf_init(&j->result_name);
    atlas_buf_init(&j->result_root_text);
    atlas_err_init(&j->result_err);
    atlas_ai_result_init(&j->ai_result);
    atlas_decision_result_init(&j->decision_result);
    atlas_orch_result_init(&j->orch_result);
    return j;
}

static void job_free(atlas_job *j) {
    if (j == NULL) {
        return;
    }
    atlas_buf_free(&j->arg1);
    atlas_buf_free(&j->arg2);
    atlas_buf_free(&j->dirty_paths);
    atlas_buf_free(&j->result_name);
    atlas_buf_free(&j->result_root_text);
    if (j->ai != NULL) {
        atlas_ai_op_free(j->ai);
        free(j->ai);
    }
    atlas_ai_result_free(&j->ai_result);
    if (j->decision != NULL) {
        atlas_decision_op_free(j->decision);
        free(j->decision);
    }
    atlas_decision_result_free(&j->decision_result);
    if (j->orch != NULL) {
        atlas_orch_op_free(j->orch);
        free(j->orch);
    }
    atlas_orch_result_free(&j->orch_result);
    if (j->verify != NULL) {
        atlas_verify_op_free(j->verify);
        free(j->verify);
    }
    if (j->verify_result != NULL) {
        atlas_verify_intake_result_free(j->verify_result);
        free(j->verify_result);
    }
    free(j->gw_audit);
    free(j);
}

void atlas_writer_result_init(atlas_writer_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->name);
    atlas_buf_init(&r->root_text);
}

void atlas_writer_result_free(atlas_writer_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->name);
    atlas_buf_free(&r->root_text);
}

/* Copies the identifying fields of a repository into a job result. */
static atlas_status job_set_result(atlas_job *j, const atlas_repo_info *ri, atlas_err *err) {
    j->result_id = ri->id;
    atlas_status st = atlas_buf_set_str(&j->result_name, ri->name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&j->result_root_text, ri->root_path_text.data, ri->root_path_text.len,
                           err);
    }
    return st;
}

/* Caller holds the lock. */
static bool queue_push(atlas_writer *w, atlas_job *j) {
    if (w->count >= ATLAS_WRITER_QUEUE_MAX) {
        return false;
    }
    w->queue[(w->head + w->count) % ATLAS_WRITER_QUEUE_MAX] = j;
    w->count++;
    return true;
}

/* Caller holds the lock. */
static atlas_job *queue_pop(atlas_writer *w) {
    if (w->count == 0) {
        return NULL;
    }
    atlas_job *j = w->queue[w->head];
    w->head = (w->head + 1u) % ATLAS_WRITER_QUEUE_MAX;
    w->count--;
    return j;
}

/* Caller holds the lock. Removes one job a submitter has given up on, and
 * returns whether it was still there to remove.
 *
 * The answer is the whole point of the function. A job that is still queued has
 * not been looked at: excising it means nothing ran, which is what makes the
 * refusal its submitter reports honest. A job that is *not* found has been
 * dequeued and is executing, and there is nothing to take back — the caller must
 * keep waiting rather than treat "not found" as "cancelled".
 *
 * Every other job keeps its position. Only the abandoned one leaves, and the
 * jobs behind it shift up by one, so the queue stays first-in-first-out. That
 * ordering is load-bearing well outside this file: the orchestration ledger and
 * the decision lifecycle both depend on writes being applied in the order they
 * were accepted, and a queue that let one job overtake another to save a caller
 * some latency would break them silently. */
static bool queue_remove(atlas_writer *w, const atlas_job *j) {
    for (size_t k = 0; k < w->count; k++) {
        if (w->queue[(w->head + k) % ATLAS_WRITER_QUEUE_MAX] != j) {
            continue;
        }
        for (size_t m = k + 1u; m < w->count; m++) {
            w->queue[(w->head + m - 1u) % ATLAS_WRITER_QUEUE_MAX] =
                w->queue[(w->head + m) % ATLAS_WRITER_QUEUE_MAX];
        }
        w->count--;
        return true;
    }
    return false;
}

/* A9.2.6. The one thing a caller is told when the writer is busy with work that
 * has no statable end.
 *
 * "Semantic maintenance" rather than "rebuilding an index", because two kinds
 * produce this and only one of them is a rebuild: a discovery walk is looking
 * for build descriptions, and an operator sent to look at a build that is not
 * running has been told something false in the one message they will read.
 *
 * The sentence after the token is the load-bearing half. The other way a
 * synchronous writer call can fail — the deadline expiring — leaves the job
 * queued and running, so the write *does* happen and the caller simply never
 * hears the outcome. This one guarantees the opposite: the job was taken back
 * out before anything looked at it. Only that difference makes retrying safe,
 * and a caller cannot infer it from a status code, so it is said. The two
 * messages are never merged. */
static const char WRITER_BUSY_MSG[] =
    "BUSY: the Atlas daemon is performing semantic maintenance and cannot take this write yet. "
    "Nothing was queued and nothing will run, so the request may be sent again.";

/* A9.2.6. Whether a job of this kind has a duration Atlas can state.
 *
 * **THE DEADLINE WAS NEVER THE BOUND; THE SHORT JOB WAS.** Every synchronous
 * writer call below waits with a timeout, and the comment explaining why has
 * always said that the timeout is what stops one slow mutation stalling every
 * other client. That was true only because every job on this queue was a handful
 * of statements: the timeout bounded a stall nothing was expected to reach.
 * A9.2.4 put an automatic, minutes-long semantic pass on the same thread and the
 * same queue, and the premise stopped holding without a line of the waiting code
 * changing. A hook's session write queued behind such a pass now sits out its
 * whole four seconds and *then* fails — and because the serve loop dispatches
 * one request at a time, `daemon.ping` and every other client sit behind it too.
 * Measured: ping goes from 26 ms to 3.9 s for each such write.
 *
 * So a waiter has to be able to ask what the writer is doing, and this is the
 * question. It is asked of the kind rather than of elapsed time because elapsed
 * time cannot distinguish a job that is nearly finished from one that has barely
 * started, and a waiter that guesses wrong in the second direction abandons a
 * write that was about to succeed.
 *
 * `true` costs the caller a refusal it must retry. `false` costs it a wait. The
 * kinds that answer `true` are the two that run a compiler or walk a whole tree
 * — the ones with no statable bound. **Reconciliation is deliberately `false`**
 * even though a first pass over a large repository can exceed the timeouts here:
 * an incremental pass is the common case, it finishes well inside them, and a
 * hook write refused during one would be dropped rather than delayed, because
 * hooks fail open. Refusing a write that would have succeeded is the worse
 * failure, and it would be the frequent one.
 *
 * There is no `default:`, so adding a job kind will not compile until somebody
 * decides which side it is on. See `docs/extending.md`. */
static bool job_kind_is_unbounded(atlas_job_kind kind) {
    switch (kind) {
    /* Runs a compiler over every translation unit the build describes. */
    case ATLAS_JOB_SEM_INDEX:
    /* Walks the repository looking for build descriptions. */
    case ATLAS_JOB_SEM_DISCOVER:
        return true;
    case ATLAS_JOB_RECONCILE:
    case ATLAS_JOB_REPO_ADD:
    case ATLAS_JOB_REPO_REMOVE:
    case ATLAS_JOB_MARK_GAP:
    case ATLAS_JOB_SET_WATCH:
    case ATLAS_JOB_AI:
    case ATLAS_JOB_DECISION:
    case ATLAS_JOB_ORCH:
    case ATLAS_JOB_SNAPSHOT:
    case ATLAS_JOB_MAINTENANCE:
    case ATLAS_JOB_GW_AUDIT:
    case ATLAS_JOB_APIKEY:
    case ATLAS_JOB_VERIFY:
    case ATLAS_JOB_SEM_CONFIG:
        return false;
    }
    return false;
}

/* A9.2.6. Wait for one queued job, and say whether waiting was abandoned.
 *
 * **THE DEADLINE WAS NEVER THE BOUND; THE SHORT JOB WAS.** Every synchronous
 * writer call in this file waits with a timeout, and the comment explaining why
 * has always said the timeout is what stops one slow mutation stalling every
 * other client. That held only because every job on this queue was a handful of
 * statements — the timeout bounded a stall nothing was expected to reach.
 * A9.2.4 put an automatic, minutes-long semantic pass on the same thread and the
 * same queue, and the premise stopped holding without a line of the waiting code
 * changing. A hook's session write queued behind such a pass sits out its whole
 * four seconds and *then* fails; and because the serve loop dispatches one
 * request at a time, `daemon.ping` and every other client sit behind it too.
 * Measured on this repository: ping 26 ms idle, 3.9 s per such write.
 *
 * Caller holds the lock and still holds it on return. Every synchronous caller
 * uses this rather than its own wait loop: nine copies of a waiting rule is nine
 * places to fix it, and a daemon responsive on eight of nine methods is
 * indistinguishable from an intermittent one.
 *
 * `true` means the job was removed from the queue and **never ran**, which is
 * the caller's licence to report a retryable refusal. `false` means the ordinary
 * two outcomes are still in play and the caller decides between them by reading
 * `j->done`, exactly as it did before.
 *
 * The wait is sliced rather than made in one call so the condition can be asked
 * again: a pass that starts *after* this job is queued blocks it exactly as much
 * as one already running, and a single `pthread_cond_timedwait` would sleep
 * straight through the difference. A slice expiring is not the timeout — only
 * the deadline is, which is why the slice is clamped to it rather than allowed
 * to overrun it. */
static bool writer_wait_locked(atlas_writer *w, atlas_job *j, const struct timespec *deadline) {
    while (!j->done) {
        struct timespec now;
        (void)clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
            return false; /* the caller's deadline, and the only timeout there is */
        }
        struct timespec slice = now;
        slice.tv_nsec += (long)ATLAS_WRITER_WAIT_SLICE_MS * 1000000L;
        if (slice.tv_nsec >= 1000000000L) {
            slice.tv_sec++;
            slice.tv_nsec -= 1000000000L;
        }
        if (slice.tv_sec > deadline->tv_sec ||
            (slice.tv_sec == deadline->tv_sec && slice.tv_nsec > deadline->tv_nsec)) {
            slice = *deadline;
        }
        (void)pthread_cond_timedwait(&w->job_done, &w->lock, &slice);

        /* One question, asked in the one lock hold the wait reacquired, because
         * either half alone gives the wrong answer. "The writer is busy with
         * something unbounded" is not a reason to give up unless this job is
         * also still queued: if `queue_remove` cannot find it, the writer has
         * dequeued it and is running *this* job — there is nothing to take back,
         * and abandoning it would report a refusal for a write already in
         * progress. Keep waiting instead, which is what the deadline is for. */
        if (!j->done && w->running && job_kind_is_unbounded(w->running_kind) &&
            queue_remove(w, j)) {
            return true;
        }
    }
    return false;
}

/* --- the work ------------------------------------------------------------ */

/* Opens the repository, runs a pass, and records the outcome.
 *
 * Retries are for one specific cause: the pass observed a repository whose HEAD
 * then moved before it could commit. That is not an error, it is a race with the
 * user, and the right response is to look again. The retry count is bounded so
 * a branch being flipped in a loop degrades honestly instead of spinning. */
static void run_reconcile(atlas_writer *w, atlas_job *j) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    if (atlas_db_repo_get_by_id(w->db, j->repo_id, &info, &found, &err) != ATLAS_OK || !found) {
        /* The repository was removed between queueing and running. Nothing to
         * do, and nothing has gone wrong. */
        atlas_repo_info_free(&info);
        atlas_safe_pool_free(&safe);
        return;
    }

    atlas_git *g = NULL;
    if (atlas_git_open(atlas_buf_cstr(&info.root_path), &g, &err) != ATLAS_OK) {
        atlas_daemon_log(w->log, "warn", "repository %s cannot be opened: %s",
                         atlas_safe(&safe, info.name), atlas_safe(&safe, atlas_err_msg(&err)));
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_db_index_state_set_error(w->db, j->repo_id, atlas_err_msg(&err), &ignore);
        atlas_repo_info_free(&info);
        atlas_safe_pool_free(&safe);
        return;
    }

    /* An outstanding event gap can only be cleared by a pass that actually read
     * everything, so a repository carrying one is upgraded to a full pass here
     * rather than being left permanently incomplete. */
    atlas_index_state state;
    atlas_index_state_init(&state);
    bool want_full = j->full;
    if (atlas_db_index_state_get(w->db, j->repo_id, &state, &err) == ATLAS_OK) {
        if (state.pending_full_reconcile || state.event_gap) {
            want_full = true;
        }
    }
    atlas_index_state_free(&state);

    bool published = false;
    for (int attempt = 0; attempt < ATLAS_RECONCILE_MAX_ATTEMPTS && !published; attempt++) {
        atlas_reconcile_opts opts;
        atlas_reconcile_opts_init(&opts);
        opts.full = want_full;
        opts.workers = w->workers;
        opts.sync_seq = j->sync_seq;
        /* Only meaningful for an incremental pass; a full one reads everything
         * anyway. Passed regardless so the reconciler owns the precedence rule
         * rather than having it split across two files. */
        opts.dirty_paths = j->dirty_paths.len > 0 ? j->dirty_paths.data : NULL;
        opts.dirty_paths_len = j->dirty_paths.len;
        /* A structural rebuild is a separate request from a full pass: `full`
         * re-reads file content, and a full pass that finds every hash unchanged
         * still parses nothing. Discarding the graph has to be asked for. */
        opts.code_rebuild = j->code_rebuild;

        atlas_reconcile_summary sum;
        atlas_reconcile_summary_init(&sum);
        atlas_err perr;
        atlas_err_init(&perr);
        atlas_status st = atlas_reconcile_run(w->db, g, j->repo_id, &opts, &sum, &perr);
        if (st != ATLAS_OK) {
            atlas_daemon_log(w->log, "error", "reconciling %s failed: %s",
                             atlas_safe(&safe, info.name), atlas_safe(&safe, atlas_err_msg(&perr)));
            atlas_err ignore;
            atlas_err_init(&ignore);
            (void)atlas_db_index_state_set_error(w->db, j->repo_id, atlas_err_msg(&perr), &ignore);
            atlas_reconcile_summary_free(&sum);
            break;
        }
        published = sum.published;
        if (published) {
            atlas_daemon_log(w->log, "info",
                             "reconciled %s generation %lld: %lld examined, %lld hashed, "
                             "%lld unchanged by identity, +%lld ~%lld -%lld, %lld commits, %lld ms",
                             atlas_safe(&safe, info.name), (long long)sum.generation,
                             (long long)sum.files_examined, (long long)sum.files_hashed,
                             (long long)sum.files_identity_hit, (long long)sum.files_added,
                             (long long)sum.files_modified, (long long)sum.files_deleted,
                             (long long)sum.commits_ingested, (long long)sum.duration_ms);
            (void)pthread_mutex_lock(&w->lock);
            w->passes++;
            (void)pthread_mutex_unlock(&w->lock);
        } else {
            atlas_daemon_log(w->log, "info",
                             "HEAD moved while reconciling %s; discarding the pass and looking "
                             "again (attempt %d of %d)",
                             atlas_safe(&safe, info.name), attempt + 1,
                             ATLAS_RECONCILE_MAX_ATTEMPTS);
        }
        atlas_reconcile_summary_free(&sum);
    }

    if (!published) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_db_index_state_mark_gap(
            w->db, j->repo_id,
            "HEAD kept moving while reconciling; the index may not reflect the current state",
            &ignore);
    }

    atlas_git_close(g);
    atlas_repo_info_free(&info);
    atlas_safe_pool_free(&safe);
}

/* Registers a repository. Runs here rather than on the IPC thread because it
 * writes, and every write in the daemon is this thread's. */
static void run_repo_add(atlas_writer *w, atlas_job *j) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    const char *name = j->arg2.len > 0 ? atlas_buf_cstr(&j->arg2) : NULL;
    atlas_status st = atlas_service_repo_add_db(w->db, atlas_buf_cstr(&j->arg1), name,
                                                j->exact_root, &info, &j->result_err);
    if (st == ATLAS_OK) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_db_index_state_ensure(w->db, info.id, &ignore);
        st = job_set_result(j, &info, &j->result_err);
    }
    j->result = st;
    if (st == ATLAS_OK) {
        /* The watcher re-derives its watch set rather than being handed one, so
         * a repository added while the daemon is running is watched without a
         * restart. Set through the accessor: the flag is read by the watcher
         * thread, so it is guarded by the queue lock like everything else. */
        atlas_writer_set_watch_dirty(w);
        /* An initial pass, so a freshly registered repository is usable
         * immediately rather than at the next filesystem event. */
        atlas_job *rj = job_new(ATLAS_JOB_RECONCILE);
        if (rj != NULL) {
            rj->repo_id = info.id;
            rj->full = true;
            (void)pthread_mutex_lock(&w->lock);
            rj->sync_seq = ++w->next_sync_seq;
            if (!queue_push(w, rj)) {
                job_free(rj);
            } else {
                (void)pthread_cond_signal(&w->not_empty);
            }
            (void)pthread_mutex_unlock(&w->lock);
        }
    }
    atlas_repo_info_free(&info);
}

static void run_repo_remove(atlas_writer *w, atlas_job *j) {
    atlas_repo_info removed;
    atlas_repo_info_init(&removed);
    atlas_status st =
        atlas_service_repo_remove_db(w->db, atlas_buf_cstr(&j->arg1), &removed, &j->result_err);
    if (st == ATLAS_OK) {
        st = job_set_result(j, &removed, &j->result_err);
        atlas_writer_set_watch_dirty(w);
    }
    j->result = st;
    atlas_repo_info_free(&removed);
}

/* Runs one AI-session operation.
 *
 * The reconciliation callback is supplied here rather than in src/ai, so that
 * the AI service knows nothing about the job queue and the queue's coalescing
 * rules apply unchanged: a correlate that asks for a pass is folded into any
 * pass already pending for the same repository, exactly like a watcher event. */
static atlas_status ai_request_sync(void *ud, int64_t repo_id, const char *dirty_paths,
                                    size_t dirty_len, int64_t *sync_seq_out, atlas_err *err) {
    atlas_writer *w = (atlas_writer *)ud;
    if (repo_id <= 0) {
        return ATLAS_OK;
    }
    return atlas_writer_submit_reconcile(w, repo_id, false, false, dirty_paths, dirty_len,
                                         sync_seq_out,
                                         err);
}

static void run_ai(atlas_writer *w, atlas_job *j) {
    if (j->ai == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "an AI job arrived with no operation attached");
        return;
    }
    j->result = atlas_ai_apply(w->db, j->ai, ai_request_sync, w, &j->ai_result, &j->result_err);
}

static void run_decision(atlas_writer *w, atlas_job *j) {
    if (j->decision == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "a decision job arrived with no operation attached");
        return;
    }
    /* `atlas_decision_apply` owns its own transaction, exactly like
     * `atlas_ai_apply`, and is called with none open — so an operation is
     * whole or nothing, and a spent challenge without the transition it
     * authorised cannot be committed. */
    j->result = atlas_decision_apply(w->db, j->decision, &j->decision_result, &j->result_err);
}

/* A9.2.1. `atlas_verify_intake_apply` owns its own transaction, exactly like
 * `atlas_ai_apply`, `atlas_decision_apply` and `atlas_orch_apply`, and is called
 * with none open — so an operation is whole or nothing.
 *
 * That matters most for the one operation here that can reach a lifecycle
 * state. An evaluation records a durable result, and if the root-owned policy's
 * gates are met it spends a warrant and transitions the record; the audit row
 * and the transition must commit together or neither, because an audit row with
 * no transition describes something that did not happen and a transition with
 * no audit row is an automatic change to project knowledge with no recoverable
 * reason. */
static void run_verify(atlas_writer *w, atlas_job *j) {
    if (j->verify == NULL || j->verify_result == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "a verification job arrived with no operation attached");
        return;
    }
    j->result = atlas_verify_intake_apply(w->db, j->verify, j->verify_result, &j->result_err);
}

static void run_orch(atlas_writer *w, atlas_job *j) {
    if (j->orch == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "an orchestration job arrived with no operation attached");
        return;
    }
    /* `atlas_orch_apply` owns its own transaction, exactly like
     * `atlas_ai_apply` and `atlas_decision_apply`, and is called with none
     * open — so an operation is whole or nothing, and a granted lease without
     * the attempt it belongs to cannot be committed. */
    j->result = atlas_orch_apply(w->db, j->orch, &j->orch_result, &j->result_err);
}

static void run_snapshot(atlas_writer *w, atlas_job *j) {
    if (j->snapshot_meta == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "a snapshot job arrived with nowhere to put its manifest");
        return;
    }
    /* `atlas_snapshot_open` owns its own transaction around the rows it writes,
     * and does its git reads outside it — A1's rule that no process is created
     * inside a write transaction. */
    j->result = atlas_snapshot_open(w->db, j->snapshot_attempt_id, j->snapshot_meta,
                                    &j->result_err);
}

/* Marks every registered repository as having an unresolved event gap. */
typedef struct gap_ctx {
    atlas_db *db;
    const char *detail;
} gap_ctx;

static atlas_status gap_one(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    gap_ctx *gc = (gap_ctx *)ud;
    (void)err;
    atlas_err ignore;
    atlas_err_init(&ignore);
    /* Best effort per repository: one that cannot be marked must not stop the
     * others from being marked, and the periodic full pass covers whatever this
     * misses. */
    (void)atlas_db_index_state_mark_gap(gc->db, ri->id, gc->detail, &ignore);
    return ATLAS_OK;
}

static void mark_all_repos_gapped(atlas_db *db, const char *detail) {
    gap_ctx gc = {db, detail};
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_db_repo_list(db, gap_one, &gc, &ignore);
}


/* A8-CI closeout: build a semantic index on the writer thread.
 *
 * This is the serialized writer path the closeout requires, and putting it here
 * rather than on the operations thread is the whole point: every SQLite write
 * in the daemon happens on this thread, on a handle it never shares. An index
 * built anywhere else would be a second writer.
 *
 * It reports through the operations table instead of the completion handshake,
 * because the client was answered when the work was *accepted* — a full index
 * of a large repository was measured at 141 s, and no client is going to hold a
 * socket open for that.
 *
 * The last valid generation is preserved by construction rather than by
 * anything here: `atlas_sem_index_run` writes a new generation while the
 * previous one is still being served and publishes it in one statement, so a
 * failure at any point leaves the old generation current and a RUNNING one that
 * nothing points at. That is also what makes a crash mid-index survivable. */
/* A9.2.4. One bounded walk of one repository, and the write that records it.
 *
 * Everything about how the walk is bounded is in `atlas_sem_discover`; what
 * belongs here is only that this runs on the writer thread and that a failure is
 * logged rather than propagated — nobody asked for this walk and nobody is
 * waiting for it. A repository whose root cannot be opened keeps the verdict it
 * had: "Atlas could not look this time" is not evidence that what it found last
 * time is wrong. */
static void run_sem_discover(atlas_writer *w, atlas_job *j) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    bool found = false;
    if (atlas_db_repo_get(w->db, atlas_buf_cstr(&j->arg1), &repo, &found, &err) != ATLAS_OK ||
        !found) {
        atlas_repo_info_free(&repo);
        return;
    }
    atlas_sem_discovery_result res;
    atlas_sem_discovery_result_init(&res);
    if (atlas_sem_discovery_run(w->db, &repo, &res, &err) != ATLAS_OK) {
        atlas_daemon_log(w->log, "warn",
                         "build-input discovery could not run for repository %lld: %s",
                         (long long)j->repo_id, atlas_err_msg(&err));
    } else if (res.limit_reached) {
        /* Every bound that is reached is reported — A8-CI's rule, and it matters
         * more here than usual: a walk that stopped early is why a repository
         * can never state an absence, and an operator who is not told will spend
         * their time looking at coverage instead. */
        atlas_daemon_log(w->log, "warn",
                         "build-input discovery for repository %lld is PARTIAL: %s",
                         (long long)j->repo_id, res.limit_detail);
    }
    atlas_repo_info_free(&repo);
}

static void run_sem_index(atlas_writer *w, atlas_job *j) {
    /* Claimed for the whole job, so `atlas_writer_sem_index_pending` covers the
     * window before a generation row exists as well as the one after. */
    (void)pthread_mutex_lock(&w->lock);
    w->sem_index_running_repo = j->repo_id;
    (void)pthread_mutex_unlock(&w->lock);

    atlas_err err;
    atlas_err_init(&err);
    atlas_sem_index_summary sum;
    atlas_sem_index_summary_init(&sum);

    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    bool found = false;
    atlas_status st = atlas_db_repo_get(w->db, atlas_buf_cstr(&j->arg1), &repo, &found, &err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(&err, ATLAS_ERR_REPO,
                           "NOT_REGISTERED: the repository was removed from the registry before "
                           "the index could start");
    }

    /* The compilation databases arrive as a NUL-separated list and are handed
     * on as one: they are repository-relative paths validated inside the root
     * by the indexer, and nothing here re-resolves them from a string. */
    const char *compdbs[ATLAS_SEM_MAX_COMPDBS];
    size_t n = 0;
    /* A9.2.4. An empty list is not "nothing to do": it means *use what discovery
     * accepted*, which is how every automatic build now arrives. A caller that
     * names databases explicitly — an operator running `code index --compdb` —
     * still gets exactly those, because naming them is a deliberate act about a
     * particular build and discovery is not entitled to overrule it. */
    atlas_buf accepted = ATLAS_BUF_INIT;
    if (st == ATLAS_OK && j->arg2.len == 0) {
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_sem_accepted_inputs(w->db, j->repo_id, &accepted, NULL, &ignored);
    }
    if (st == ATLAS_OK) {
        const char *p = j->arg2.len > 0 ? atlas_buf_cstr(&j->arg2) : (const char *)accepted.data;
        size_t len = j->arg2.len > 0 ? j->arg2.len : accepted.len;
        const char *end = p != NULL ? p + len : NULL;
        while (p != NULL && p < end && n < ATLAS_SEM_MAX_COMPDBS) {
            compdbs[n++] = p;
            p += strlen(p) + 1u;
        }
    }
    /* A9.2.3. The identity the attempt is made *at*, measured before the pass
     * rather than after it.
     *
     * The retry governor asks "have the inputs changed since the attempt that
     * failed?", so what it must record is the state the failing attempt saw. If
     * the tree moves during a failed build, recording the later identity would
     * block a retry that has every reason to succeed — the inputs did change,
     * just not in time for that attempt. Recording the earlier one means the
     * next sweep sees a moved identity and tries again, which is correct. */
    char attempt_identity[65];
    attempt_identity[0] = '\0';
    if (st == ATLAS_OK && j->op_id == 0) {
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_sem_source_identity(w->db, &repo, attempt_identity, &ignored);
    }

    if (st == ATLAS_OK) {
        st = atlas_sem_index_on(w->db, &repo, compdbs, n, j->sem_rebuild, &sum, &err);
    }

    /* Only an *automatic* attempt feeds the governor, and `op_id == 0` is how
     * this thread knows: an operation id exists when a client asked and is
     * polling, and a client asking is a different principal making a different
     * decision. An operator running `code index` against a repository that
     * cannot build should get the failure reported to them every time rather
     * than be told the governor is holding — and their attempt must not be able
     * to clear a governor record either, which is why success only clears it on
     * this path. */
    if (j->op_id == 0) {
        atlas_err rerr;
        atlas_err_init(&rerr);
        /* The reason stored is a fixed Atlas string, never the error text: an
         * error message can quote a path or a compiler diagnostic, and this
         * column is read back by an operator and by a model.
         *
         * Two reasons rather than one, chosen from the status, because they call
         * for different actions. A usage failure is the build description: the
         * named compilation databases could not be read, or describe nothing
         * Atlas can compile, and an operator fixes the file. Anything else is
         * the pass itself. Recording every failure as
         * `the_parser_process_did_not_report_a_result` — which is what the first
         * cut did — told an operator to look at a parser that had never run. */
        const char *why = ATLAS_SEM_WHY_PASS_FAILED;
        if (st == ATLAS_OK) {
            why = "";
        } else if (st == ATLAS_ERR_USAGE || st == ATLAS_ERR_CONFIG) {
            why = ATLAS_SEM_WHY_BUILD_DESCRIPTION;
        } else if (st == ATLAS_ERR_INTERNAL || st == ATLAS_ERR_DB) {
            /* A9.2.5. Out of memory, a database error, a write that could not
             * complete — a failure of the machine rather than of the inputs.
             * Recorded distinctly so the governor can allow exactly one further
             * attempt, which `ATLAS_SEM_WHY_PASS_FAILED` must not: the source
             * identity has not moved, so without this the repository holds on
             * `HOLD_FAILED_UNCHANGED` until somebody happens to edit a file. */
            why = ATLAS_SEM_WHY_PASS_INTERRUPTED;
        }
        (void)atlas_db_sem_config_record_attempt(w->db, repo.id, attempt_identity, st == ATLAS_OK,
                                                 why, &rerr);
    }

    atlas_buf detail = ATLAS_BUF_INIT;
    atlas_err derr;
    atlas_err_init(&derr);
    if (st == ATLAS_OK) {
        /* Atlas' own counts about its own pass. Every bound that was reached is
         * carried through, because a partial index must never be displayed the
         * way a complete one is. */
        (void)atlas_buf_appendf(&detail, &derr,
                                "generation=%lld units=%lld complete=%lld partial=%lld "
                                "failed=%lld unsupported=%lld symbols=%lld edges=%lld "
                                /* A9.2.4. How much work *this pass* did, which the
                                 * generation deliberately does not record — it reports
                                 * the rows it holds, which is a different question and
                                 * the one A8-CI's closure fixed. The remote form reads
                                 * its summary from the generation, so without these two
                                 * `code index` on a deployed machine reported
                                 * "parsed 0, reused 0" after parsing every unit in the
                                 * repository. Under A7.1 the socket is the operator's
                                 * only path, so that was the *only* thing they saw. */
                                "parsed=%lld reused=%lld retried=%lld",
                                (long long)sum.generation_id, (long long)sum.units_total,
                                (long long)sum.units_complete, (long long)sum.units_partial,
                                (long long)sum.units_failed, (long long)sum.units_unsupported,
                                (long long)sum.symbols, (long long)sum.edges,
                                (long long)sum.units_parsed, (long long)sum.units_reused,
                                (long long)sum.units_retried);
    }
    atlas_ops_finish(w->ops, j->op_id, st,
                     st == ATLAS_OK ? "semantic index published" : atlas_err_msg(&err),
                     atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);
    atlas_buf_free(&accepted);
    atlas_repo_info_free(&repo);

    /* Released on every path, including the failing ones. A claim left behind
     * would make the scheduler hold for ever on a repository whose build
     * failed — which is the same defect as a flag that never clears, one layer
     * down, and it must not be reintroduced here. */
    (void)pthread_mutex_lock(&w->lock);
    w->sem_index_running_repo = 0;
    (void)pthread_mutex_unlock(&w->lock);
}


static void *writer_main(void *arg) {
    atlas_writer *w = (atlas_writer *)arg;

    /* The writable handle is created here and never leaves this thread. */
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_db_open(atlas_buf_cstr(&w->db_path), &w->db, &err) != ATLAS_OK ||
        atlas_db_migrate(w->db, &err) != ATLAS_OK) {
        atlas_daemon_log(w->log, "error", "the writer cannot open the index: %s",
                         atlas_err_msg(&err));
        (void)pthread_mutex_lock(&w->lock);
        w->stopping = true;
        w->ready_failed = true;
        w->ready_err = err;
        (void)pthread_cond_broadcast(&w->ready_cv);
        (void)pthread_cond_broadcast(&w->job_done);
        (void)pthread_mutex_unlock(&w->lock);
        return NULL;
    }
    /* The index exists and is at the expected version. Publishing that here,
     * before anything else in the daemon runs, is what stops a reader — the
     * watcher building its watch set, or an IPC request arriving the instant the
     * socket opens — from querying a schema that has not been created yet.
     *
     * Without this the window is small and real: a hook that fires while systemd
     * is still starting the daemon gets "no such table: repositories" instead of
     * a registration, and a session start silently records nothing. */
    (void)pthread_mutex_lock(&w->lock);
    w->ready = true;
    (void)pthread_cond_broadcast(&w->ready_cv);
    (void)pthread_mutex_unlock(&w->lock);

    /* A9.2.3: reap semantic generations left RUNNING by a daemon that is gone.
     *
     * Here because this is the only moment a daemon can be certain nothing of
     * its own is building: the writer has just opened the index, no job has run,
     * and the data-directory lock this process holds means no other process can
     * be building it either. A row still RUNNING is therefore a pass that died.
     *
     * A8-CI said the next pass would report and reap one and nothing ever did.
     * That was harmless while nothing scheduled off the record. A9.2.3 holds the
     * scheduler while a generation is being built, and a RUNNING row from a dead
     * daemon is indistinguishable from a live build — so one crash left the
     * repository reporting BUILDING for ever and never rebuilding again. Found
     * by killing a daemon mid-build and restarting it. */
    {
        atlas_err rerr;
        atlas_err_init(&rerr);
        int64_t reaped = 0;
        if (atlas_db_sem_reap_running(w->db, &reaped, &rerr) != ATLAS_OK) {
            atlas_daemon_log(w->log, "warn",
                             "could not reap interrupted semantic generations: %s",
                             atlas_err_msg(&rerr));
        } else if (reaped > 0) {
            /* Reported rather than silent: "indexing died and nobody noticed"
             * is exactly the operational fact a log exists for. */
            atlas_daemon_log(w->log, "info",
                             "reaped %lld semantic generation(s) left running by a previous "
                             "daemon; the last published generation is unaffected",
                             (long long)reaped);
        }
    }

    /* Did the previous run shut down cleanly?
     *
     * A clean stop records `stopped_at`. A record with a start and no stop means
     * the last daemon was killed, crashed, or lost power — and therefore that
     * whatever happened to the repositories after its final pass was never
     * observed. The startup pass below verifies content and will resolve it, but
     * until that pass *completes* the index must not describe itself as current,
     * so the gap is recorded first.
     *
     * Recorded before atlas_db_daemon_started overwrites the row. */
    {
        atlas_err ignore;
        atlas_err_init(&ignore);
        atlas_daemon_record prev;
        atlas_daemon_record_init(&prev);
        if (atlas_db_daemon_get(w->db, &prev, &ignore) == ATLAS_OK && prev.present &&
            prev.started_at[0] != '\0' && prev.stopped_at[0] == '\0') {
            atlas_daemon_log(w->log, "warn",
                             "the previous Atlas daemon did not shut down cleanly; every "
                             "repository is marked incomplete until a full content verification "
                             "completes");
            mark_all_repos_gapped(w->db,
                                  "the previous daemon did not shut down cleanly, so changes made "
                                  "after its last pass were not observed");
        }
        atlas_daemon_record_free(&prev);
        (void)atlas_db_daemon_started(w->db, (int64_t)getpid(), atlas_buf_cstr(&w->socket_path),
                                      &ignore);
    }

    for (;;) {
        (void)pthread_mutex_lock(&w->lock);
        while (!w->stopping && w->count == 0) {
            (void)pthread_cond_wait(&w->not_empty, &w->lock);
        }
        if (w->stopping && w->count == 0) {
            (void)pthread_mutex_unlock(&w->lock);
            break;
        }
        atlas_job *j = queue_pop(w);
        /* Claimed in the hold that dequeued it, so there is no instant in which
         * this thread owns a job and reports itself idle. A waiter that saw that
         * instant would conclude its own job could not be the one running. */
        if (j != NULL) {
            w->running = true;
            w->running_kind = j->kind;
        }
        (void)pthread_mutex_unlock(&w->lock);
        if (j == NULL) {
            continue;
        }

        switch (j->kind) {
        case ATLAS_JOB_RECONCILE: run_reconcile(w, j); break;
        case ATLAS_JOB_REPO_ADD: run_repo_add(w, j); break;
        case ATLAS_JOB_REPO_REMOVE: run_repo_remove(w, j); break;
        case ATLAS_JOB_AI: run_ai(w, j); break;
        case ATLAS_JOB_DECISION: run_decision(w, j); break;
        case ATLAS_JOB_ORCH: run_orch(w, j); break;
        case ATLAS_JOB_VERIFY: run_verify(w, j); break;
        case ATLAS_JOB_SNAPSHOT: run_snapshot(w, j); break;
        case ATLAS_JOB_SEM_INDEX: run_sem_index(w, j); break;
        case ATLAS_JOB_SEM_DISCOVER: run_sem_discover(w, j); break;
        case ATLAS_JOB_MAINTENANCE: {
            /* Both pointers belong to a caller that may have stopped waiting;
             * cleared under the lock in that case, so a NULL here means the
             * result has nowhere to go. */
            if (j->maint != NULL && j->maint_out != NULL) {
                j->result = atlas_maintenance_on(w->db, j->maint, j->maint_out, &j->result_err);
            }
            break;
        }
        case ATLAS_JOB_SEM_CONFIG: {
            if (j->sem_config != NULL && j->sem_config_out != NULL) {
                j->result = atlas_sem_config_on(w->db, j->sem_config, j->sem_config_out,
                                                &j->result_err);
            }
            break;
        }
        case ATLAS_JOB_APIKEY: {
            /* Both pointers belong to a caller that may have stopped waiting;
             * cleared under the lock in that case, so a NULL here means the
             * result has nowhere to go — and in particular means a freshly
             * minted plaintext is never written into a struct nobody owns. */
            if (j->apikey != NULL && j->apikey_out != NULL) {
                if (j->apikey->kind == ATLAS_APIKEY_JOB_REVOKE) {
                    j->result = atlas_apikey_revoke_on(w->db, j->apikey->key_id,
                                                       &j->apikey_out->changed, &j->result_err);
                } else {
                    atlas_apikey_create_opts co;
                    memset(&co, 0, sizeof co);
                    co.label = j->apikey->label;
                    co.scopes = j->apikey->scopes;
                    co.rotate_from = j->apikey->rotate_from[0] != '\0' ? j->apikey->rotate_from
                                                                      : NULL;
                    j->result = atlas_apikey_create_on(w->db, &co, &j->apikey_out->created,
                                                       &j->result_err);
                }
            }
            break;
        }
        case ATLAS_JOB_GW_AUDIT: {
            /* Nobody is waiting, so a failure is logged and dropped rather than
             * reported: the request this describes has already been answered,
             * and failing it retroactively is not something Atlas can do.
             *
             * The credential touch rides along because the gateway audits every
             * request anyway, which makes `last_used_at` free rather than a
             * second round trip on the path of every authenticated read. It is
             * throttled in SQL, so two gateway processes cannot each decide it
             * is their turn. */
            if (j->gw_audit != NULL) {
                atlas_err aerr;
                atlas_err_init(&aerr);
                if (atlas_db_gw_audit_append(w->db, j->gw_audit, &aerr) != ATLAS_OK) {
                    /* Logged and dropped. The request this row describes has
                     * already been answered, and failing it retroactively is
                     * not something Atlas can do. */
                    atlas_safe_pool safe;
                    atlas_safe_pool_init(&safe);
                    atlas_daemon_log(w->log, "warn", "gateway audit row not written: %s",
                                     atlas_safe(&safe, atlas_err_msg(&aerr)));
                    atlas_safe_pool_free(&safe);
                }
                if (j->gw_audit->key_id[0] != '\0') {
                    atlas_err terr;
                    atlas_err_init(&terr);
                    (void)atlas_db_apikey_touch(w->db, j->gw_audit->key_id, &terr);
                }
            }
            break;
        }
        case ATLAS_JOB_MARK_GAP: {
            atlas_err ignore;
            atlas_err_init(&ignore);
            (void)atlas_db_index_state_mark_gap(w->db, j->repo_id, atlas_buf_cstr(&j->arg1),
                                                &ignore);
            break;
        }
        case ATLAS_JOB_SET_WATCH: {
            atlas_err ignore;
            atlas_err_init(&ignore);
            (void)atlas_db_index_state_set_watch(w->db, j->repo_id,
                                                 (atlas_watch_state)j->watch_state,
                                                 j->arg1.len > 0 ? atlas_buf_cstr(&j->arg1) : NULL,
                                                 j->watched_dirs, &ignore);
            break;
        }
        default: break;
        }

        /* One hold clears the claim, completes the job and wakes the waiters,
         * because a waiter that saw the writer idle while its own job was still
         * unfinished would take its job back out of a queue it is no longer in.
         *
         * `wants_result` is read here rather than before the lock, and that is
         * what decides who frees the job. A waiter that has given up clears it
         * under this same lock, so reading it outside could see the waiter still
         * present, hand the job to it, and leak — the waiter has already
         * returned. Whoever observes it last under the lock owns the answer. */
        (void)pthread_mutex_lock(&w->lock);
        w->running = false;
        bool waited_on = j->wants_result;
        if (waited_on) {
            j->done = true;
        }
        (void)pthread_cond_broadcast(&w->job_done);
        (void)pthread_mutex_unlock(&w->lock);
        if (!waited_on) {
            job_free(j);
        }
        /* Otherwise ownership stays with the waiter, which frees it. */
    }

    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_db_daemon_stopped(w->db, &ignore);
    atlas_db_close(w->db);
    w->db = NULL;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

atlas_status atlas_writer_start(const char *db_path, const char *socket_path,
                                atlas_workers *workers, FILE *log, atlas_writer **out,
                                atlas_err *err) {
    *out = NULL;
    atlas_writer *w = calloc(1u, sizeof(*w));
    if (w == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the writer");
    }
    atlas_buf_init(&w->db_path);
    atlas_buf_init(&w->socket_path);
    w->workers = workers;
    w->log = log;
    atlas_err_init(&w->ready_err);
    if (pthread_mutex_init(&w->lock, NULL) != 0 || pthread_cond_init(&w->not_empty, NULL) != 0 ||
        pthread_cond_init(&w->job_done, NULL) != 0 || pthread_cond_init(&w->ready_cv, NULL) != 0) {
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create writer synchronisation");
    }
    atlas_status st = atlas_buf_set_str(&w->db_path, db_path, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&w->socket_path, socket_path != NULL ? socket_path : "", err);
    }
    if (st != ATLAS_OK) {
        atlas_writer_stop(w);
        return st;
    }
    if (pthread_create(&w->thread, NULL, writer_main, w) != 0) {
        atlas_writer_stop(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create the writer thread");
    }
    w->thread_started = true;

    /* Wait for the index to be open and migrated before returning.
     *
     * Starting the watcher and the serve loop against a database the writer has
     * not created yet produces exactly the failures it should: the watcher
     * cannot enumerate repositories, and an IPC request that arrives in the
     * window gets a hard error rather than an answer. Both were observable
     * before this wait existed — the daemon logged "cannot build the watch set:
     * no such table: repositories" on a first run.
     *
     * The deadline is generous because it covers a first-run migration on slow
     * storage, and exceeding it is a startup failure rather than something to
     * proceed past: a daemon whose index never opened has nothing to serve. */
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 120;
    int rc = 0;
    (void)pthread_mutex_lock(&w->lock);
    while (!w->ready && !w->ready_failed && rc == 0) {
        rc = pthread_cond_timedwait(&w->ready_cv, &w->lock, &deadline);
    }
    bool ready = w->ready;
    bool failed = w->ready_failed;
    atlas_err ready_err = w->ready_err;
    (void)pthread_mutex_unlock(&w->lock);

    if (failed) {
        atlas_writer_stop(w);
        *err = ready_err;
        return err->status != ATLAS_OK ? err->status : ATLAS_ERR_DB;
    }
    if (!ready) {
        atlas_writer_stop(w);
        return atlas_err_set(err, ATLAS_ERR_DB,
                             "the Atlas index did not open within 120 seconds; refusing to serve "
                             "against a database that may not exist");
    }
    *out = w;
    return ATLAS_OK;
}

void atlas_writer_stop(atlas_writer *w) {
    if (w == NULL) {
        return;
    }
    if (w->thread_started) {
        (void)pthread_mutex_lock(&w->lock);
        w->stopping = true;
        (void)pthread_cond_broadcast(&w->not_empty);
        (void)pthread_mutex_unlock(&w->lock);
        (void)pthread_join(w->thread, NULL);
    }
    /* Anything still queued at shutdown was never run. Freeing it here rather
     * than leaking it keeps the sanitiser build meaningful. */
    atlas_job *j;
    while ((j = queue_pop(w)) != NULL) {
        job_free(j);
    }
    (void)pthread_cond_destroy(&w->not_empty);
    (void)pthread_cond_destroy(&w->job_done);
    (void)pthread_cond_destroy(&w->ready_cv);
    (void)pthread_mutex_destroy(&w->lock);
    atlas_buf_free(&w->db_path);
    atlas_buf_free(&w->socket_path);
    free(w);
}

/* --- submission ---------------------------------------------------------- */

atlas_status atlas_writer_submit_reconcile(atlas_writer *w, int64_t repo_id, bool full,
                                           bool code_rebuild,
                                           const char *dirty_paths, size_t dirty_len,
                                           int64_t *sync_seq_out, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_RECONCILE);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a reconciliation");
    }
    j->repo_id = repo_id;
    j->full = full;
    j->code_rebuild = code_rebuild;
    if (dirty_paths != NULL && dirty_len > 0) {
        atlas_status cst = atlas_buf_set(&j->dirty_paths, dirty_paths, dirty_len, err);
        if (cst != ATLAS_OK) {
            job_free(j);
            return cst;
        }
    }

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping) {
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the Atlas daemon is shutting down");
    }
    /* Coalesce. A pass that has not started yet will observe the filesystem as
     * it is when it runs, so it already covers this request; queueing a second
     * identical pass would double the work and change nothing. A request for a
     * *full* pass upgrades a pending incremental one rather than being dropped. */
    for (size_t k = 0; k < w->count; k++) {
        atlas_job *q = w->queue[(w->head + k) % ATLAS_WRITER_QUEUE_MAX];
        if (q->kind == ATLAS_JOB_RECONCILE && q->repo_id == repo_id && !q->wants_result) {
            if (full) {
                q->full = true;
            }
            /* A rebuild request upgrades a pending pass rather than being
             * dropped, exactly as a full request does. The two coalesce
             * independently because they mean different things: `full` re-reads
             * file content, `code_rebuild` discards the graph. */
            if (code_rebuild) {
                q->code_rebuild = true;
            }
            /* The pending pass must cover both requests, so the named paths are
             * merged rather than replaced. Dropping one set here would silently
             * lose the very override that makes an event outrank metadata. If
             * the merged set no longer fits, the pass is upgraded to a full
             * content verification: unable to enumerate what changed, it must
             * not pretend it can. */
            if (j->dirty_paths.len > 0) {
                atlas_err merge_err;
                atlas_err_init(&merge_err);
                if (q->dirty_paths.len + j->dirty_paths.len > ATLAS_WATCH_MAX_DIRTY_BYTES ||
                    atlas_buf_append(&q->dirty_paths, j->dirty_paths.data, j->dirty_paths.len,
                                     &merge_err) != ATLAS_OK) {
                    q->full = true;
                    atlas_buf_reset(&q->dirty_paths);
                }
            }
            q->sync_seq = ++w->next_sync_seq;
            if (sync_seq_out != NULL) {
                *sync_seq_out = q->sync_seq;
            }
            (void)pthread_mutex_unlock(&w->lock);
            job_free(j);
            return ATLAS_OK;
        }
    }
    j->sync_seq = ++w->next_sync_seq;
    if (!queue_push(w, j)) {
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        /* Backpressure, stated. The queue is bounded on purpose, and a caller
         * that is told "full" can retry; a caller that is silently dropped
         * cannot. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon's write queue is full (%u pending); the request was "
                             "refused rather than dropped",
                             (unsigned)ATLAS_WRITER_QUEUE_MAX);
    }
    if (sync_seq_out != NULL) {
        *sync_seq_out = j->sync_seq;
    }
    (void)pthread_cond_signal(&w->not_empty);
    (void)pthread_mutex_unlock(&w->lock);
    return ATLAS_OK;
}

/* Fire-and-forget notification jobs from the watcher.
 *
 * A full queue drops these rather than blocking the watcher: the flags they
 * carry are re-derived on the next watch rebuild or periodic pass, and a watcher
 * blocked on the writer would stop draining inotify, which is how a queue
 * overflow gets manufactured. */
static atlas_status submit_note(atlas_writer *w, atlas_job_kind kind, int64_t repo_id,
                                const char *detail, int watch_state, int64_t watched_dirs,
                                atlas_err *err) {
    atlas_job *j = job_new(kind);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a watcher note");
    }
    j->repo_id = repo_id;
    j->watch_state = watch_state;
    j->watched_dirs = watched_dirs;
    atlas_status st = atlas_buf_set_str(&j->arg1, detail != NULL ? detail : "", err);
    if (st != ATLAS_OK) {
        job_free(j);
        return st;
    }
    (void)pthread_mutex_lock(&w->lock);
    bool queued = !w->stopping && queue_push(w, j);
    if (queued) {
        (void)pthread_cond_signal(&w->not_empty);
    }
    (void)pthread_mutex_unlock(&w->lock);
    if (!queued) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the Atlas daemon's write queue is full");
    }
    return ATLAS_OK;
}

atlas_status atlas_writer_submit_gap(atlas_writer *w, int64_t repo_id, const char *detail,
                                     atlas_err *err) {
    return submit_note(w, ATLAS_JOB_MARK_GAP, repo_id, detail, 0, 0, err);
}

atlas_status atlas_writer_submit_watch_state(atlas_writer *w, int64_t repo_id, int watch_state,
                                             const char *detail, int64_t watched_dirs,
                                             atlas_err *err) {
    return submit_note(w, ATLAS_JOB_SET_WATCH, repo_id, detail, watch_state, watched_dirs, err);
}

static atlas_status writer_call_impl(atlas_writer *w, atlas_job_kind kind, const char *arg1,
                                     const char *arg2, bool exact_root, int timeout_ms,
                                     atlas_writer_result *result, atlas_err *err) {
    atlas_job *j = job_new(kind);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a request");
    }
    j->wants_result = true;
    j->exact_root = exact_root;
    atlas_status st = atlas_buf_set_str(&j->arg1, arg1 != NULL ? arg1 : "", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&j->arg2, arg2 != NULL ? arg2 : "", err);
    }
    if (st != ATLAS_OK) {
        job_free(j);
        return st;
    }

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    /* A bounded wait. The IPC loop serves one request at a time, so an unbounded
     * wait here would let one slow mutation stall every other client
     * indefinitely; a deadline turns that into a reported timeout. */
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    int ms = timeout_ms > 0 ? timeout_ms : 30000;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        /* The job is still owned by the writer, which will free it when it
         * finishes. Detaching rather than freeing here is what keeps that safe. */
        (void)pthread_mutex_lock(&w->lock);
        j->wants_result = false;
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the request within %d ms", ms);
    }

    st = j->result;
    if (st == ATLAS_OK) {
        result->id = j->result_id;
        st = atlas_buf_set(&result->name, j->result_name.data, j->result_name.len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&result->root_text, j->result_root_text.data,
                               j->result_root_text.len, err);
        }
    } else {
        *err = j->result_err;
    }
    job_free(j);
    return st;
}

atlas_status atlas_writer_ai(atlas_writer *w, atlas_ai_op *op, int timeout_ms,
                             atlas_ai_result *result, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_AI);
    if (j == NULL) {
        /* Ownership is taken unconditionally, including here: a caller that has
         * to free the operation on some paths and not others eventually frees it
         * on the wrong one. */
        atlas_ai_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing an AI request");
    }
    j->ai = op;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    int ms = timeout_ms > 0 ? timeout_ms : 5000;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        /* Detached rather than freed: the job is still the writer's, and the
         * writer frees it when it finishes. */
        (void)pthread_mutex_lock(&w->lock);
        j->wants_result = false;
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the request within %d ms", ms);
    }

    atlas_status st = j->result;
    if (st == ATLAS_OK) {
        result->session_id = j->ai_result.session_id;
        result->repo_id = j->ai_result.repo_id;
        result->change_set_id = j->ai_result.change_set_id;
        result->record_id = j->ai_result.record_id;
        result->session_created = j->ai_result.session_created;
        result->session_unbound = j->ai_result.session_unbound;
        /* A pointer to one of the ATLAS_AI_UNBOUND_* string literals, so copying
         * the pointer across the thread boundary is copying the value. Nothing
         * else may ever be put in this field. */
        result->unbound_reason = j->ai_result.unbound_reason;
        result->repo_registered = j->ai_result.repo_registered;
        result->duplicate = j->ai_result.duplicate;
        result->degraded = j->ai_result.degraded;
        result->changed_paths = j->ai_result.changed_paths;
        result->direct_paths = j->ai_result.direct_paths;
        result->ambiguous_paths = j->ai_result.ambiguous_paths;
        result->unresolved_paths = j->ai_result.unresolved_paths;
        result->concurrent_sessions = j->ai_result.concurrent_sessions;
        result->sync_seq = j->ai_result.sync_seq;
        st = atlas_buf_set(&result->repo_name, j->ai_result.repo_name.data,
                           j->ai_result.repo_name.len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&result->root_text, j->ai_result.root_text.data,
                               j->ai_result.root_text.len, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&result->degraded_reason, j->ai_result.degraded_reason.data,
                               j->ai_result.degraded_reason.len, err);
        }
        if (st == ATLAS_OK) {
            /* A4. The decision document `atlas_record_decision` materialised, so
             * the caller learns its id without a second query. */
            st = atlas_buf_set(&result->decision_uid, j->ai_result.decision_uid.data,
                               j->ai_result.decision_uid.len, err);
        }
    } else {
        *err = j->result_err;
    }
    job_free(j);
    return st;
}

atlas_status atlas_writer_call(atlas_writer *w, atlas_job_kind kind, const char *arg1,
                               const char *arg2, int timeout_ms, atlas_writer_result *result,
                               atlas_err *err) {
    return writer_call_impl(w, kind, arg1, arg2, false, timeout_ms, result, err);
}

atlas_status atlas_writer_call_repo_add(atlas_writer *w, const char *path, const char *name,
                                        bool exact_root, int timeout_ms,
                                        atlas_writer_result *result, atlas_err *err) {
    return writer_call_impl(w, ATLAS_JOB_REPO_ADD, path, name, exact_root, timeout_ms, result,
                            err);
}

int64_t atlas_writer_queue_depth(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    int64_t n = (int64_t)w->count;
    (void)pthread_mutex_unlock(&w->lock);
    return n;
}

int64_t atlas_writer_passes(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    int64_t n = w->passes;
    (void)pthread_mutex_unlock(&w->lock);
    return n;
}

bool atlas_writer_take_watch_dirty(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    bool v = w->watch_dirty;
    w->watch_dirty = false;
    (void)pthread_mutex_unlock(&w->lock);
    return v;
}

void atlas_writer_set_watch_dirty(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    w->watch_dirty = true;
    (void)pthread_mutex_unlock(&w->lock);
}

atlas_status atlas_writer_decision(atlas_writer *w, atlas_decision_op *op, int timeout_ms,
                                   atlas_decision_result *result, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_DECISION);
    if (j == NULL) {
        /* Ownership is taken unconditionally, here as everywhere: a caller that
         * has to free the operation on some paths and not others eventually
         * frees it on the wrong one. */
        atlas_decision_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a decision request");
    }
    j->decision = op;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    int ms = timeout_ms > 0 ? timeout_ms : 5000;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        /* Detached rather than freed: the job is still the writer's, and the
         * writer frees it when it finishes. */
        (void)pthread_mutex_lock(&w->lock);
        j->wants_result = false;
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the request within %d ms", ms);
    }

    atlas_status st = j->result;
    if (st == ATLAS_OK) {
        result->repo_id = j->decision_result.repo_id;
        result->document_id = j->decision_result.document_id;
        result->revision_id = j->decision_result.revision_id;
        result->revision_no = j->decision_result.revision_no;
        result->state = j->decision_result.state;
        /* A9.1. Copied like every other scalar: this block is a field-by-field
         * copy across a thread boundary, so a field added to
         * `atlas_decision_result` and not added here is silently zero on the
         * daemon path while the local path reports it — and zero for a kind is
         * DECISION, which is a confident wrong answer rather than an absent one.
         * The acceptance run caught exactly that. */
        result->knowledge_kind = j->decision_result.knowledge_kind;
        result->superseded_revision_no = j->decision_result.superseded_revision_no;
        result->document_created = j->decision_result.document_created;
        result->duplicate = j->decision_result.duplicate;
        result->session_unbound = j->decision_result.session_unbound;
        /* A pointer to one of the ATLAS_AI_UNBOUND_* string literals, so
         * copying the pointer across the thread boundary copies the value.
         * Nothing else may ever be put in this field. */
        result->unbound_reason = j->decision_result.unbound_reason;
        memcpy(result->content_hash, j->decision_result.content_hash,
               sizeof(result->content_hash));
        memcpy(result->confirm, j->decision_result.confirm, sizeof(result->confirm));
        memcpy(result->expires_at, j->decision_result.expires_at, sizeof(result->expires_at));
        struct {
            atlas_buf *to;
            const atlas_buf *from;
        } copies[] = {
            {&result->repo_name, &j->decision_result.repo_name},
            {&result->root_text, &j->decision_result.root_text},
            {&result->uid, &j->decision_result.uid},
            {&result->token, &j->decision_result.token},
            {&result->title, &j->decision_result.title},
            {&result->replaced_by_uid, &j->decision_result.replaced_by_uid},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof(copies) / sizeof(copies[0]); i++) {
            st = atlas_buf_set(copies[i].to, copies[i].from->data, copies[i].from->len, err);
        }
    } else {
        *err = j->result_err;
    }
    job_free(j);
    return st;
}

/* A9.2.1. The same shape as `atlas_writer_decision` and `atlas_writer_orch`, and
 * for the same reasons: the job owns the operation unconditionally, the wait is
 * bounded, and a request that times out is detached rather than freed — it is
 * still the writer's, and the writer frees it when it finishes.
 *
 * The result is moved rather than copied field by field. A9.1's postmortem is
 * the argument for doing it this way: a scalar added to the result struct and
 * not added to a hand-written copy block is silently zero on the daemon path
 * while the local path reports it, and zero is a legitimate value for most of
 * these fields — so the two paths disagree with no error anywhere. Handing over
 * the whole allocation cannot drift. */
atlas_status atlas_writer_verify(atlas_writer *w, atlas_verify_op *op, int timeout_ms,
                                 atlas_verify_intake_result *result, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_VERIFY);
    atlas_verify_intake_result *slot = calloc(1u, sizeof *slot);
    if (j == NULL || slot == NULL) {
        /* Ownership is taken unconditionally, here as everywhere: a caller that
         * has to free the operation on some paths and not others eventually
         * frees it on the wrong one. */
        atlas_verify_op_free(op);
        free(op);
        free(slot);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory queueing a verification request");
    }
    atlas_verify_intake_result_init(slot);
    j->verify = op;
    j->verify_result = slot;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    int ms = timeout_ms > 0 ? timeout_ms : 5000;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        /* Detached rather than freed: the job is still the writer's, and the
         * writer frees it when it finishes. */
        (void)pthread_mutex_lock(&w->lock);
        j->wants_result = false;
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the request within %d ms", ms);
    }

    atlas_status st = j->result;
    if (st == ATLAS_OK) {
        /* Move: the caller's struct takes the job's allocations whole, and the
         * job's slot is emptied so `job_free` releases nothing twice. */
        atlas_verify_intake_result_free(result);
        *result = *j->verify_result;
        memset(j->verify_result, 0, sizeof *j->verify_result);
    } else {
        *err = j->result_err;
    }
    job_free(j);
    return st;
}

/* A8. The same shape as `atlas_writer_decision`, and for the same reasons: the
 * job owns the operation unconditionally, the wait is bounded, and a request
 * that times out is detached rather than freed — it is still the writer's, and
 * the writer frees it when it finishes. */
atlas_status atlas_writer_orch(atlas_writer *w, atlas_orch_op *op, int timeout_ms,
                               atlas_orch_result *result, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_ORCH);
    if (j == NULL) {
        atlas_orch_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory queueing an orchestration request");
    }
    j->orch = op;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    int ms = timeout_ms > 0 ? timeout_ms : 5000;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        (void)pthread_mutex_lock(&w->lock);
        j->wants_result = false;
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the request within %d ms", ms);
    }

    atlas_status st = j->result;
    if (st == ATLAS_OK) {
        result->job_id = j->orch_result.job_id;
        result->state = j->orch_result.state;
        result->attempt_id = j->orch_result.attempt_id;
        result->attempt_no = j->orch_result.attempt_no;
        result->seq = j->orch_result.seq;
        result->duplicate = j->orch_result.duplicate;
        result->granted = j->orch_result.granted;
        result->cancel_requested = j->orch_result.cancel_requested;
        result->expires_ms = j->orch_result.expires_ms;
        result->wall_timeout_ms = j->orch_result.wall_timeout_ms;
        result->idle_timeout_ms = j->orch_result.idle_timeout_ms;
        result->max_output_bytes = j->orch_result.max_output_bytes;
        result->max_artifact_bytes = j->orch_result.max_artifact_bytes;
        result->max_artifact_count = j->orch_result.max_artifact_count;
        result->expired = j->orch_result.expired;
        result->retried = j->orch_result.retried;
        result->timed_out = j->orch_result.timed_out;
        result->recovered = j->orch_result.recovered;
        memcpy(result->spec_digest, j->orch_result.spec_digest, sizeof(result->spec_digest));
        struct {
            atlas_buf *to;
            const atlas_buf *from;
        } copies[] = {
            {&result->job_uid, &j->orch_result.job_uid},
            {&result->token, &j->orch_result.token},
            {&result->repo_name, &j->orch_result.repo_name},
            {&result->repo_root, &j->orch_result.repo_root},
            {&result->source_commit, &j->orch_result.source_commit},
            {&result->mode, &j->orch_result.mode},
            {&result->driver, &j->orch_result.driver},
            {&result->task_text, &j->orch_result.task_text},
            {&result->allowed_paths, &j->orch_result.allowed_paths},
            {&result->validations, &j->orch_result.validations},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof copies / sizeof copies[0]; i++) {
            st = atlas_buf_set(copies[i].to, copies[i].from->data, copies[i].from->len, err);
        }
    } else {
        *err = j->result_err;
    }
    /* The writer hands ownership to whoever was waiting, so freeing it here is
     * the contract rather than a courtesy — see the `wants_result` branch at the
     * end of `writer_main`. The timeout path above is the other half: it clears
     * `wants_result` and returns without freeing, because the job is still the
     * writer's. */
    job_free(j);
    return st;
}


/* A8. Snapshot enumeration on the writer thread. The manifest is written into
 * the caller's struct, which stays alive for the wait — the same handshake
 * every other writer call uses. */
atlas_status atlas_writer_snapshot(atlas_writer *w, int64_t attempt_id, int timeout_ms,
                                   struct atlas_snapshot_meta *out, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_SNAPSHOT);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a snapshot");
    }
    j->snapshot_attempt_id = attempt_id;
    j->snapshot_meta = out;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    int ms = timeout_ms > 0 ? timeout_ms : 120000;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }
    if (!done) {
        /* Detached, not freed: the job is still the writer's, and it must not
         * write into a struct the caller has stopped waiting on. */
        (void)pthread_mutex_lock(&w->lock);
        j->wants_result = false;
        j->snapshot_meta = NULL;
        (void)pthread_mutex_unlock(&w->lock);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not enumerate the snapshot within %d ms", ms);
    }
    atlas_status st = j->result;
    if (st != ATLAS_OK) {
        *err = j->result_err;
    }
    job_free(j);
    return st;
}

/* True when a semantic index for this repository is queued or running.
 *
 * The durable record cannot answer this on its own: a job that has been queued,
 * or dequeued and not yet reached the point of opening a generation, leaves no
 * RUNNING row — so a scheduler consulting only the index would queue a second
 * build of the same repository. A flag on the scheduler's own side cannot answer
 * it either, because nothing tells the scheduler when a job finished; the
 * daemon's first cut kept one, fed it back into the plan it used to clear it,
 * and so never cleared it at all: the repository reported DIRTY for ever and
 * rebuilt exactly once.
 *
 * The writer knows, because the writer owns the queue and runs the job. */
bool atlas_writer_sem_index_pending(atlas_writer *w, int64_t repo_id) {
    if (w == NULL || repo_id <= 0) {
        return false;
    }
    bool pending = false;
    (void)pthread_mutex_lock(&w->lock);
    if (w->sem_index_running_repo == repo_id) {
        pending = true;
    }
    for (size_t k = 0; !pending && k < w->count; k++) {
        const atlas_job *q = w->queue[(w->head + k) % ATLAS_WRITER_QUEUE_MAX];
        if (q->kind == ATLAS_JOB_SEM_INDEX && q->repo_id == repo_id) {
            pending = true;
        }
    }
    (void)pthread_mutex_unlock(&w->lock);
    return pending;
}

/* Queues a semantic index and returns immediately.
 *
 * `wants_result` is deliberately false: the caller has already been given an
 * operation id and will poll. Blocking here would put the old timeout back, one
 * layer down. */
atlas_status atlas_writer_submit_sem_index(atlas_writer *w, int64_t repo_id, const char *name,
                                           const char *compdbs, size_t compdbs_len, bool rebuild,
                                           int64_t op_id, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_SEM_INDEX);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a semantic index");
    }
    j->repo_id = repo_id;
    j->op_id = op_id;
    j->sem_rebuild = rebuild;
    atlas_status st = atlas_buf_set_str(&j->arg1, name, err);
    if (st == ATLAS_OK && compdbs_len > 0) {
        st = atlas_buf_append(&j->arg2, compdbs, compdbs_len, err);
    }
    if (st != ATLAS_OK) {
        job_free(j);
        return st;
    }
    (void)pthread_mutex_lock(&w->lock);
    bool queued = !w->stopping && queue_push(w, j);
    if (queued) {
        (void)pthread_cond_signal(&w->not_empty);
    }
    (void)pthread_mutex_unlock(&w->lock);
    if (!queued) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the Atlas daemon's write queue is full");
    }
    return ATLAS_OK;
}

/* A9.2.4. Queues one bounded walk of one repository.
 *
 * Fire-and-forget, like the automatic index: nobody is polling for it, and what
 * it produces is a durable candidate list an operator reads through
 * `code sem-status`. A failure means the repository keeps the verdict it had,
 * and the next interval tries again — the same backpressure the freshness sweep
 * uses, and nothing is lost by a walk that did not happen this time. */
atlas_status atlas_writer_submit_sem_discover(atlas_writer *w, int64_t repo_id, const char *name,
                                              atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_SEM_DISCOVER);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing build-input discovery");
    }
    j->repo_id = repo_id;
    atlas_status st = atlas_buf_set_str(&j->arg1, name, err);
    if (st != ATLAS_OK) {
        job_free(j);
        return st;
    }
    (void)pthread_mutex_lock(&w->lock);
    /* Coalesced against the queue for the reason the reconcile job is: a walk of
     * a repository already waiting to be walked adds nothing, and a slow writer
     * must not accumulate a backlog of identical tree walks. */
    bool duplicate = false;
    for (size_t k = 0; !duplicate && k < w->count; k++) {
        const atlas_job *q = w->queue[(w->head + k) % ATLAS_WRITER_QUEUE_MAX];
        if (q->kind == ATLAS_JOB_SEM_DISCOVER && q->repo_id == repo_id) {
            duplicate = true;
        }
    }
    bool queued = duplicate || (!w->stopping && queue_push(w, j));
    if (queued && !duplicate) {
        (void)pthread_cond_signal(&w->not_empty);
    }
    (void)pthread_mutex_unlock(&w->lock);
    if (duplicate || !queued) {
        job_free(j);
    }
    if (!queued) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the Atlas daemon's write queue is full");
    }
    return ATLAS_OK;
}

void atlas_writer_set_ops(atlas_writer *w, atlas_ops *ops) {
    /* Set once, after both exist and before the serve loop starts, so no job
     * that reports through the table can be queued while it is missing. */
    w->ops = ops;
}

/* A9.2.3. The synchronous shape a prune uses, for the reason a prune uses it:
 * this is a single-row upsert followed by a bounded read, so there is nothing
 * here that can outlast a client and making it poll would add a mechanism to an
 * operation that does not need one. It is on the writer thread because it
 * writes, and exactly one thread in this daemon writes. */
atlas_status atlas_writer_sem_config(atlas_writer *w, const atlas_sem_config_job *job,
                                     atlas_sem_status_report *out, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_SEM_CONFIG);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory queueing a semantic build description");
    }
    j->sem_config = job;
    j->sem_config_out = out;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    /* Generous rather than tight: the work itself is milliseconds, but the
     * writer may be behind a semantic index that takes minutes, and a client
     * that gave up would report a failure for a write that then succeeds. */
    deadline.tv_sec += 300;
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    atlas_status st = j->result;
    atlas_err jerr = j->result_err;
    if (!done) {
        /* Detached rather than freed: the job is still the writer's, and the
         * report it was given belongs to the caller, so the writer must not
         * touch it after this. */
        j->wants_result = false;
        j->sem_config = NULL;
        j->sem_config_out = NULL;
    }
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not record the semantic build description "
                             "within 300 s");
    }
    if (st != ATLAS_OK) {
        *err = jerr;
    }
    return st;
}

atlas_status atlas_writer_maintenance(atlas_writer *w, const atlas_maintenance_opts *opts,
                                      atlas_maintenance_report *out, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_MAINTENANCE);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a prune");
    }
    j->maint = opts;
    j->maint_out = out;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    /* The caller waits, unlike an index. A prune deletes in bounded batches and
     * a plan of the whole retention policy was measured at 78 ms, so there is
     * nothing here that outlasts a client — and making it poll would add a
     * mechanism to an operation that does not need one. The ceiling is generous
     * rather than tight because the writer may be behind other work. */
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 300;
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    atlas_status st = j->result;
    atlas_err jerr = j->result_err;
    if (!done) {
        /* Detached rather than freed: the job is still the writer's. The
         * report it was given belongs to the caller, so the writer must not
         * touch it after this — which is what clearing the pointers does. */
        j->wants_result = false;
        j->maint = NULL;
        j->maint_out = NULL;
    }
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the prune within 300 s");
    }
    if (st != ATLAS_OK) {
        *err = jerr;
    }
    return st;
}

/* --- A9: the gateway audit trail ------------------------------------------
 *
 * Queued and forgotten. The caller does not wait, does not learn the outcome,
 * and cannot fail because of it — A9.6's requirement that audit failure must
 * not break request handling, made structural rather than handled. The entry is
 * copied into the job, so nothing points at the caller's stack once this
 * returns.
 *
 * A full queue drops the row and says so in the log. That is the honest
 * behaviour: the alternative is blocking a request path on the writer, which
 * would turn a busy index into a stalled gateway. */
atlas_status atlas_writer_gw_audit(atlas_writer *w, const atlas_gw_audit_entry *entry,
                                   atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_GW_AUDIT);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing an audit row");
    }
    j->gw_audit = malloc(sizeof(*j->gw_audit));
    if (j->gw_audit == NULL) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing an audit row");
    }
    *j->gw_audit = *entry;
    j->wants_result = false;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);
    (void)pthread_mutex_unlock(&w->lock);
    return ATLAS_OK;
}


/* --- A9: credential operations on the writer thread ------------------------
 *
 * The caller waits. A credential operation is a handful of statements, and an
 * operator is standing at a terminal — polling would add a mechanism to
 * something that does not need one.
 *
 * This exists so revocation does not require stopping the daemon. The local
 * path takes the data-directory writer lock, which a running daemon holds, so
 * without this `atlas api-key revoke` would fail on exactly the machines where
 * revoking matters most. */
atlas_status atlas_writer_apikey(atlas_writer *w, const atlas_apikey_job *op,
                                 atlas_apikey_job_result *out, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_APIKEY);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a credential change");
    }
    j->apikey = op;
    j->apikey_out = out;
    j->wants_result = true;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping || !queue_push(w, j)) {
        bool stopping = w->stopping;
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             stopping ? "the Atlas daemon is shutting down"
                                      : "the Atlas daemon's write queue is full");
    }
    (void)pthread_cond_signal(&w->not_empty);

    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 60;
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    atlas_status st = j->result;
    atlas_err jerr = j->result_err;
    if (!done) {
        /* Detached rather than freed: the job is still the writer's. Clearing
         * the pointers is what stops it writing a plaintext into a result
         * struct whose owner has gone. */
        j->wants_result = false;
        j->apikey = NULL;
        j->apikey_out = NULL;
    }
    (void)pthread_mutex_unlock(&w->lock);

    /* Taken back out of the queue before anything looked at it, so this reports
     * a refusal rather than a failure: the write did not happen and asking again
     * is safe. Freed here because the writer never saw it. */
    if (backed_out) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", WRITER_BUSY_MSG);
    }

    if (!done) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon did not complete the credential change within 60 s");
    }
    if (st != ATLAS_OK) {
        *err = jerr;
    }
    return st;
}

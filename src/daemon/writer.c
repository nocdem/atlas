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

    atlas_workers *workers;
    FILE *log;
    atlas_buf db_path;
    atlas_buf socket_path;
    atlas_db *db; /* owned by the writer thread only */
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
        case ATLAS_JOB_SNAPSHOT: run_snapshot(w, j); break;
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

        if (j->wants_result) {
            (void)pthread_mutex_lock(&w->lock);
            j->done = true;
            (void)pthread_cond_broadcast(&w->job_done);
            (void)pthread_mutex_unlock(&w->lock);
            /* Ownership stays with the waiter, which frees it. */
        } else {
            job_free(j);
        }
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
    int rc = 0;
    while (!j->done && rc == 0) {
        rc = pthread_cond_timedwait(&w->job_done, &w->lock, &deadline);
    }
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

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
    int rc = 0;
    while (!j->done && rc == 0) {
        rc = pthread_cond_timedwait(&w->job_done, &w->lock, &deadline);
    }
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

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
    int rc = 0;
    while (!j->done && rc == 0) {
        rc = pthread_cond_timedwait(&w->job_done, &w->lock, &deadline);
    }
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

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
    int rc = 0;
    while (!j->done && rc == 0) {
        rc = pthread_cond_timedwait(&w->job_done, &w->lock, &deadline);
    }
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);

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
    int rc = 0;
    while (!j->done && rc == 0) {
        rc = pthread_cond_timedwait(&w->job_done, &w->lock, &deadline);
    }
    bool done = j->done;
    (void)pthread_mutex_unlock(&w->lock);
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

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
#include "atlas/memory.h"
#include "atlas/ops.h"
#include "atlas/ipc.h"
#include "atlas/sem_discover.h"
#include "atlas/sem.h"
#include "atlas/sem_ops.h"
#include "atlas/service.h"
#include "atlas/mirror.h"
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

    /* P0. The writer's test channel, guarded by `lock`.
     *
     * Two properties are otherwise unassertable. The first is that a watch
     * state Atlas has decided on but not yet persisted still reads fail-closed
     * — which only bites while the single writer thread is held by a long job,
     * and there is no way to hold it on demand from outside. The second is that
     * a `SET_WATCH` job which *reaches the database and fails there* leaves the
     * obligation outstanding; a refusal at the submit call proves only that a
     * full queue is handled, which is a different claim about a different code
     * path.
     *
     * It lives in this internal header and nowhere else: no CLI flag, no
     * environment variable, no RPC method, no MCP tool, no policy key. Nothing
     * parses a string into it and nothing outside `tests/` calls it, for the
     * reason the watcher's `inject_` fields carry — a way to stall the writer
     * or discard its writes, reachable by anyone who can start a daemon, would
     * be a denial of service with a nicer name. */
    bool test_stall_armed;
    atlas_job_kind test_stall_kind;
    pthread_cond_t test_release_cv;
    int64_t test_fail_set_watch;
    int64_t test_set_watch_failed;
    bool test_stall_active; /* a job of the stalled kind is being held right now */

    atlas_workers *workers;
    FILE *log;
    atlas_buf db_path;
    /* A13. Where the mirror lives, for a repository whose own tree this process
     * cannot open. The writer is handed it rather than deriving it from
     * `db_path`, because stripping a filename to recover a directory is a guess
     * about a path the caller already knows exactly. */
    atlas_buf data_dir;
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
    if (j->plan != NULL) {
        atlas_plan_op_free(j->plan);
        free(j->plan);
    }
    if (j->plan_result != NULL) {
        atlas_plan_result_free(j->plan_result);
        free(j->plan_result);
    }
    /* Plain data, so a bare `free` is its whole destructor -- see the field's
     * own comment in daemon_internal.h. */
    free(j->memory_pol);
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

void atlas_memory_put_op_init(atlas_memory_put_op *op) {
    if (op == NULL) {
        return;
    }
    memset(op, 0, sizeof(*op));
    atlas_buf_init(&op->source_uid);
    atlas_buf_init(&op->rel_path);
    atlas_buf_init(&op->content);
    atlas_buf_init(&op->observed_at);
}

void atlas_memory_put_op_free(atlas_memory_put_op *op) {
    if (op == NULL) {
        return;
    }
    atlas_buf_free(&op->source_uid);
    atlas_buf_free(&op->rel_path);
    atlas_buf_free(&op->content);
    atlas_buf_free(&op->observed_at);
}

void atlas_memory_put_result_init(atlas_memory_put_result *r) {
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->version_uid);
    atlas_buf_init(&r->content_sha256);
}

void atlas_memory_put_result_free(atlas_memory_put_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->version_uid);
    atlas_buf_free(&r->content_sha256);
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
    ATLAS_IPC_BUSY_TOKEN
    " the Atlas daemon is performing semantic maintenance and cannot take this write yet. "
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
 * decides which side it is on. See `docs/extending.md`.
 *
 * Not `static`, and it is declared in `daemon/daemon_internal.h` for one reason:
 * the agreement test walks the whole `atlas_job_kind` enum and asks both this
 * question and `job_kind_is_drainable` of every member. Two classifications of
 * one vocabulary drift, and a drift nothing compares is one nobody sees. */
bool job_kind_is_unbounded(atlas_job_kind kind) {
    switch (kind) {
    /* Runs a compiler over every translation unit the build describes. */
    case ATLAS_JOB_SEM_INDEX:
    /* Walks the repository looking for build descriptions. */
    case ATLAS_JOB_SEM_DISCOVER:
        return true;
    case ATLAS_JOB_RECONCILE:
    case ATLAS_JOB_REPO_ADD:
    case ATLAS_JOB_REPO_REMOVE:
    case ATLAS_JOB_REPO_SCANNER:
    case ATLAS_JOB_MIRROR_STATE:
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
    case ATLAS_JOB_PLAN:
    /* A handful of statements over bytes a caller already handed in, bounded
     * by ATLAS_MEMORY_MAX_SOURCE_BYTES before anything is queued. */
    case ATLAS_JOB_MEMORY:
        return false;
    /* A12.1. T8 measured this pass's worst case at the compiled ceiling -- 16
     * sources, 128 candidates each, 2048 claims, one transaction -- at
     * 2429.9 ms, which outlasts both ATLAS_HOOK_IPC_TIMEOUT_MS (2000 ms) and
     * ATLAS_HOOK_TEARDOWN_TIMEOUT_MS (700 ms): this pass can outlast a hook's
     * own deadline. `false` is still right, for the reason RECONCILE above
     * already carries: answering `true` lets a waiter back out with `BUSY`,
     * `BUSY` means nothing was queued, and a hook that fails open then loses
     * the write outright rather than merely delaying it. `false` costs a
     * caller its answer; `true` costs the write itself. */
    case ATLAS_JOB_MEMORY_RECONCILE:
        return false;
    }
    return false;
}

/* Whether a job of this kind may be run *inside* a yield of an unbounded one.
 *
 * **THE PASS HELD THE THREAD; NOBODY ELSE COULD REACH IT.** A9.2.6 let a waiter
 * stop waiting, which stopped one slow write taking every client with it, and
 * left the write itself refused. That was the honest answer and it was not the
 * useful one: for as long as a semantic pass ran, *nothing else was written at
 * all*, so a recovery sweep, a completion and a session record were all refused
 * for minutes at a stretch and had to be sent again. The pass now hands the
 * thread back between translation units, and this is the question asked of what
 * is waiting there.
 *
 * `true` for the latency-critical writes whose tables are disjoint from
 * everything a semantic pass or a discovery walk touches. Disjointness is why
 * the interleave is safe to reason about: a drained job cannot see, and cannot
 * be seen by, the half-built generation the pass is assembling — which is
 * invisible until `atlas_db_sem_publish` in any case.
 *
 * `false` is not "unimportant". It means *not while a pass is in the middle of
 * itself*, and each one has its own reason, written at the case rather than
 * inferred from the list.
 *
 * The order of everything not drained is untouched: the drain scans front to
 * back and removes only what it runs, so first-in-first-out holds among the
 * drainable kinds and within every kind. What it deliberately gives up is
 * first-in-first-out *between* a drained bounded job and a queued unbounded one:
 * a bounded write may pass a queued pass. Refusing that would reimpose the
 * starvation this exists to end, and the orderings that are load-bearing — the
 * orchestration ledger's and the decision lifecycle's — are per domain and every
 * drained domain keeps its own. See `docs/daemon-and-ipc.md`.
 *
 * There is no `default:` here either, for the reason there is none above: a new
 * job kind must not compile until somebody has decided both questions about it.
 * See `docs/extending.md`. */
bool job_kind_is_drainable(atlas_job_kind kind) {
    switch (kind) {
    /* A lease, an attempt, a completion, a recovery sweep. The writes a worker
     * and its driver are blocked on, and the ones whose refusal was measured.
     *
     * A12.1 fix round, I4. This kind stopped being disjoint from a semantic
     * pass at T13, and `true` no longer rests on that argument for it: a
     * root-task SUBMIT builds a memory pack (`run_orch_build_pack` calling
     * `atlas_memory_pack_build`, `src/memory/pack.c`), which reads `memory_*`
     * tables and, when the repository row says dirty, calls
     * `atlas_sem_source_identity` -- sem tables plus the accepted compilation
     * databases on disk; a pack-delivering LEASE runs
     * `run_orch_lease_freshness` calling `atlas_memory_pack_freshness`, which
     * does the same. `true` is still correct, on a narrower argument than
     * disjointness: a drain only ever runs where the unbounded job holds
     * nothing open (between translation units, either side of the unit loop,
     * every `ATLAS_SEM_DISCOVER_YIELD_EVERY` directory entries of a discovery
     * walk), a discovery pass commits `sem_build_inputs` in one transaction
     * only *after* its walk yields (`atlas_sem_discovery_run`,
     * `src/sem/index.c`), and a SEM_INDEX pass never writes
     * `sem_build_inputs` at all -- so a drained SUBMIT or LEASE always reads a
     * consistent, already-committed set, never a half-built generation. That
     * is a claim about transaction boundaries, not about which tables a job
     * touches, and it is the one to re-check the next time this kind's body
     * gains a file read. */
    case ATLAS_JOB_ORCH:
    /* A session record from a hook, which has four seconds and fails open. */
    case ATLAS_JOB_AI:
    /* A decision revision or a lifecycle transition; an operator is waiting. */
    case ATLAS_JOB_DECISION:
    /* A claim, its evidence, an attestation — records nothing rebuilds. */
    case ATLAS_JOB_VERIFY:
    /* A12.0. A plan's creation or one revision of it. Three small tables of its
     * own — `orch_plans`, `orch_plan_revisions`, `orch_plan_tasks` — disjoint
     * from everything a semantic pass or a discovery walk touches, and an
     * operator's foreground plan driver is blocked on it, which is the same
     * latency argument the orchestration writes above make. */
    case ATLAS_JOB_PLAN:
    /* One audit row for a request that has already been answered. */
    case ATLAS_JOB_GW_AUDIT:
    /* A credential change; revocation must not wait for a compiler. */
    case ATLAS_JOB_APIKEY:
    /* A12.1. `memory_sources` and `memory_source_versions`: disjoint from
     * everything a semantic pass or a discovery walk touches, and an
     * operator's CLI is waiting on the answer -- the same two conditions
     * ATLAS_JOB_VERIFY above satisfies.
     *
     * Both halves describe `memory.put`'s job body, which T11 writes: this kind
     * has no producer until then, so the claim is a constraint on that body
     * rather than an observation about one. If T11's body touches a table a
     * semantic pass or a discovery walk also touches, or answers nobody who is
     * waiting, this case is what has to change -- not the body. */
    case ATLAS_JOB_MEMORY:
        return true;
    /* An unbounded job must never run inside another one. */
    case ATLAS_JOB_SEM_INDEX:
    case ATLAS_JOB_SEM_DISCOVER:
    /* A first full pass can run for minutes. A yield is a pause, not a tunnel
     * through which another long pass reaches the thread. */
    case ATLAS_JOB_RECONCILE:
    /* Reads a pinned commit's whole tree, up to its 120 s budget. */
    case ATLAS_JOB_SNAPSHOT:
    /* Deletes rows. A prune interleaved with a pass over the same database is a
     * new argument nobody has written, and this is not the change to write it. */
    case ATLAS_JOB_MAINTENANCE:
    /* These mutate the ground the running pass is standing on. Today a removal
     * cannot interleave with a pass, and that impossibility is worth keeping. */
    case ATLAS_JOB_REPO_ADD:
    case ATLAS_JOB_REPO_REMOVE:
    /* One row, one statement, and it changes which principal may report about a
     * repository -- not what a running pass is reading. It is here rather than
     * among the drainable kinds because it shares `repositories` with them. */
    case ATLAS_JOB_REPO_SCANNER:
    /* One row, one statement, and what it changes is whether a mirror may be
     * read at all. */
    case ATLAS_JOB_MIRROR_STATE:
    /* Watcher bookkeeping, ordered against the reconcile submissions it
     * accompanies. Nothing latency-critical is waiting on either. */
    case ATLAS_JOB_MARK_GAP:
    case ATLAS_JOB_SET_WATCH:
    /* Reconfigures the very activity that is yielding. */
    case ATLAS_JOB_SEM_CONFIG:
    /* A12.1. The observe phase reads sources and forks git in its own right.
     * A yield must stay a pause, not a tunnel through which that file I/O
     * reaches the thread. */
    case ATLAS_JOB_MEMORY_RECONCILE:
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
 * to overrun it.
 *
 * What this season changes is *when* a waiter gives up, and nothing else. An
 * unbounded job now hands the thread back between translation units, so
 * observing one is no longer a reason to abandon a write — it is a reason to
 * wait one grace and see whether the drain arrives. Only if it does not does the
 * job come out of the queue. Both messages a caller can receive mean exactly
 * what they meant before: `BUSY:` still says nothing ran and nothing will,
 * because the job is still removed before the message is built, and the deadline
 * still says the write is on its way. */
static bool writer_wait_locked(atlas_writer *w, atlas_job *j, const struct timespec *deadline) {
    /* When this waiter first saw the writer inside an unbounded job.
     *
     * Local, so the grace is per waiter rather than per pass, and it is set once
     * and never cleared. Two consequences, both intended. A waiter that has
     * already spent its grace does not earn a second one because a *new* pass
     * started: what it is protecting against is its own latency, not the
     * identity of what is holding the thread. And a waiter that arrives during a
     * drain — when the running kind is momentarily the bounded job being served
     * — starts its grace at the next observation rather than never, because the
     * pass's kind is restored in the same lock hold that completes the drained
     * job. */
    struct timespec unbounded_seen_at;
    bool unbounded_seen = false;
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
         * progress. Keep waiting instead, which is what the deadline is for.
         *
         * The observation and the removal are two steps now, and the separation
         * is the correctness detail. `queue_remove` used to be the *test*: it
         * both asked "is this job still queued?" and answered by taking it out.
         * With a grace to serve, the first observation must leave the job exactly
         * where it is, because the whole point is that the drain may still reach
         * it. So the queue is only touched once the grace has actually run out. */
        if (!j->done && w->running && job_kind_is_unbounded(w->running_kind)) {
            struct timespec seen;
            (void)clock_gettime(CLOCK_REALTIME, &seen);
            if (!unbounded_seen) {
                unbounded_seen = true;
                unbounded_seen_at = seen;
            } else {
                int64_t waited_ms = (int64_t)(seen.tv_sec - unbounded_seen_at.tv_sec) * 1000 +
                                    (seen.tv_nsec - unbounded_seen_at.tv_nsec) / 1000000;
                if (waited_ms >= (int64_t)ATLAS_WRITER_YIELD_GRACE_MS && queue_remove(w, j)) {
                    return true;
                }
            }
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
    bool from_mirror = false;
    if (atlas_repo_open_git(&info, atlas_buf_cstr(&w->data_dir), &g, &from_mirror, &err) !=
        ATLAS_OK) {
        atlas_daemon_log(w->log, "warn", "repository %s cannot be opened: %s",
                         atlas_safe(&safe, info.name), atlas_safe(&safe, atlas_err_msg(&err)));
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_db_index_state_set_error(w->db, j->repo_id, atlas_err_msg(&err), &ignore);
        atlas_repo_info_free(&info);
        atlas_safe_pool_free(&safe);
        return;
    }
    if (from_mirror) {
        /* Info, not warn: a repository indexed from its mirror is working as
         * designed. The line exists because "which bytes did this pass read"
         * is a question an operator must be able to answer from the log. */
        atlas_daemon_log(w->log, "info", "repository %s indexed from its mirror",
                         atlas_safe(&safe, info.name));
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

/* A12.1 T11. `memory.put`'s one typed write: appends a version row for a
 * registered *external* source, from bytes `atlas_writer_memory_put` already
 * hex-decoded and bound-checked before this job was ever queued.
 *
 * `j->memory_put`/`j->memory_put_out` are borrowed, `ATLAS_JOB_APIKEY`'s own
 * shape: NULL means the caller gave up before this ran, and this is then
 * exactly a no-op, never a write into a stack or a struct that has since gone
 * away.
 *
 * **Only an `EXTERNAL_*` source is accepted here.** A `REPO_*` source is read
 * directly by the daemon or by a named scanner's mirror (A13's routing,
 * `atlas_memory_read_source`) -- a client handing over bytes for one would let
 * a caller assert content the daemon has never itself read, which is exactly
 * what `atlas_memory_observed_source.external_latest`'s own comment reserves
 * for a *different* principal reading an external path. `rel_path` is a
 * class-consistency check only, never stored -- `memory_source_versions` has
 * no column for it, and the struct's own comment in daemon_internal.h says
 * why: two byte-identical children are one version row, so a version's
 * identity is its content, never a child's name. For a `*_DIR` source it must
 * be exactly one path component ending in `ATLAS_MEMORY_DIR_SUFFIX`, T6's own
 * DIR contract (`src/memory/read.c`'s `readdir` loop admits nothing else) --
 * an operator naming a nested path or a non-`.md` name would be naming
 * something the reconciliation pass could never itself have produced. */
static void run_memory(atlas_writer *w, atlas_job *j) {
    if (j->memory_put == NULL || j->memory_put_out == NULL) {
        return;
    }
    const atlas_memory_put_op *op = j->memory_put;
    atlas_memory_put_result *out = j->memory_put_out;

    /* Read-only resolution and validation, deliberately outside any
     * transaction: A1 has no objection to a plain database read here, but
     * there is nothing yet worth wrapping one open for. */
    int64_t source_id = 0;
    atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_UNKNOWN;
    bool found = false;
    atlas_status st = atlas_db_memory_source_by_uid(w->db, atlas_buf_cstr(&op->source_uid),
                                                     &source_id, NULL, &cls, NULL, NULL, &found,
                                                     &j->result_err);
    if (st == ATLAS_OK && !found) {
        /* Names no path and no other source -- A13's rule that a refusal is
         * not an inventory handed to whoever asked. */
        st = atlas_err_set(&j->result_err, ATLAS_ERR_REPO,
                           "no registered memory source has that uid");
    }
    if (st == ATLAS_OK && atlas_memory_source_class_is_repo(cls)) {
        st = atlas_err_set(&j->result_err, ATLAS_ERR_USAGE,
                           "this source is read directly; memory.put is for an external one");
    }
    bool is_dir = (cls == ATLAS_MEMORY_SOURCE_EXTERNAL_DIR);
    if (st == ATLAS_OK) {
        size_t rlen = op->rel_path.len;
        const char *rel = rlen > 0 ? (const char *)op->rel_path.data : "";
        if (is_dir) {
            size_t suflen = strlen(ATLAS_MEMORY_DIR_SUFFIX);
            bool has_slash = memchr(rel, '/', rlen) != NULL;
            bool is_dotdot =
                (rlen == 1 && rel[0] == '.') || (rlen == 2 && rel[0] == '.' && rel[1] == '.');
            bool has_suffix = rlen > suflen &&
                              memcmp(rel + (rlen - suflen), ATLAS_MEMORY_DIR_SUFFIX, suflen) == 0;
            if (rlen == 0 || has_slash || is_dotdot || !has_suffix) {
                st = atlas_err_set(&j->result_err, ATLAS_ERR_USAGE,
                                   "a directory source needs rel_path naming one %s child",
                                   ATLAS_MEMORY_DIR_SUFFIX);
            }
        } else if (rlen > 0) {
            st = atlas_err_set(&j->result_err, ATLAS_ERR_USAGE, "a file source takes no rel_path");
        }
    }
    if (st != ATLAS_OK) {
        j->result = st;
        return;
    }

    char sha_hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(op->content.data != NULL ? op->content.data : "", op->content.len, sha_hex);
    int64_t content_bytes = (int64_t)op->content.len;

    /* The dedup read, also outside any transaction and for the same reason:
     * this thread is the only writer and runs one job at a time, so nothing
     * can insert a matching row between this read and the write below. */
    bool existing = false;
    atlas_buf existing_uid = ATLAS_BUF_INIT;
    st = atlas_db_memory_version_exists(w->db, source_id, sha_hex, &existing, NULL, &existing_uid,
                                        NULL, &j->result_err);
    if (st == ATLAS_OK && existing) {
        out->created = false;
        st = atlas_buf_set(&out->version_uid, existing_uid.data, existing_uid.len, &j->result_err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->content_sha256, sha_hex, &j->result_err);
        }
        out->content_bytes = content_bytes;
        atlas_buf_free(&existing_uid);
        j->result = st;
        return;
    }
    atlas_buf_free(&existing_uid);
    if (st != ATLAS_OK) {
        j->result = st;
        return;
    }

    char now[32];
    atlas_now_iso8601(now, sizeof now);

    st = atlas_db_begin(w->db, &j->result_err);
    if (st != ATLAS_OK) {
        j->result = st;
        return;
    }

    int64_t new_id = 0;
    atlas_buf new_uid = ATLAS_BUF_INIT;
    const char *obs = atlas_buf_cstr(&op->observed_at);
    st = atlas_db_memory_version_insert(w->db, source_id, "", "", sha_hex, content_bytes,
                                        op->content.data, op->content.len, obs, now, op->peer_uid,
                                        &new_id, &new_uid, &j->result_err);
    if (st == ATLAS_OK) {
        atlas_status cst = atlas_db_commit(w->db, &j->result_err);
        if (cst != ATLAS_OK) {
            atlas_db_rollback(w->db);
            st = cst;
        }
    } else {
        atlas_db_rollback(w->db);
    }
    if (st == ATLAS_OK) {
        out->created = true;
        st = atlas_buf_set(&out->version_uid, new_uid.data, new_uid.len, &j->result_err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->content_sha256, sha_hex, &j->result_err);
        }
        out->content_bytes = content_bytes;
    }
    atlas_buf_free(&new_uid);
    j->result = st;
}

/* A12.1. The memory reconciliation pass, submitted by `memory_sweep`
 * (`watch.c`) and, from A12.1 T11, by an operator's `memory.reconcile`.
 *
 * Two phases, in the order A1 requires: `atlas_memory_observe` reads every
 * registered source and forks git, with no transaction open, and only its
 * result -- an `atlas_memory_observation`, owning its own buffers and
 * referencing no live statement, git handle or open fd -- crosses into the
 * transaction that follows. The transaction wraps `atlas_memory_apply_in_tx`
 * alone, exactly the shape `atlas_decision_apply` gives
 * `atlas_decision_apply_in_tx` (`lifecycle.c:2073`): whole or nothing, so a
 * version row can never land without the generation it belongs to.
 *
 * The observation is heap-allocated on the memory module's own instruction:
 * it is bounded at `ATLAS_MEMORY_MAX_SOURCES` times `ATLAS_MEMORY_MAX_DIR_ENTRIES`
 * items, on the order of a megabyte at the compiled ceilings, and that size is
 * Decision 10's whole argument for why this job's duration is *statable*
 * rather than unbounded -- putting it on this thread's stack would trade a
 * documented bound for an undocumented one. */
static void run_memory_reconcile(atlas_writer *w, atlas_job *j) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);

    if (j->memory_pol == NULL) {
        /* Never happens outside a defect in the submit path -- every producer
         * of this kind attaches a policy before queueing. Refusing rather
         * than falling back to a fresh `atlas_syspolicy_load()` keeps this
         * job acting on exactly the evidence its submitter saw, never on a
         * second, independent read. */
        atlas_daemon_log(w->log, "warn",
                         "a memory reconciliation for repository %lld arrived with no policy "
                         "attached",
                         (long long)j->repo_id);
        atlas_safe_pool_free(&safe);
        return;
    }
    const atlas_syspolicy *pol = j->memory_pol;

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

    atlas_memory_observation *obs = malloc(sizeof *obs);
    if (obs == NULL) {
        atlas_daemon_log(w->log, "warn",
                         "memory reconciliation for %s could not allocate its observation",
                         atlas_safe(&safe, info.name));
        atlas_repo_info_free(&info);
        atlas_safe_pool_free(&safe);
        return;
    }
    atlas_memory_observation_init(obs);

    atlas_err oerr;
    atlas_err_init(&oerr);
    if (atlas_memory_observe(w->db, &info, atlas_buf_cstr(&w->data_dir), pol, obs, &oerr) !=
        ATLAS_OK) {
        atlas_daemon_log(w->log, "warn",
                         "memory reconciliation for %s could not observe its sources: %s",
                         atlas_safe(&safe, info.name), atlas_safe(&safe, atlas_err_msg(&oerr)));
        atlas_memory_observation_free(obs);
        free(obs);
        atlas_repo_info_free(&info);
        atlas_safe_pool_free(&safe);
        return;
    }

    char now[32];
    atlas_now_iso8601(now, sizeof now);

    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    atlas_err aerr;
    atlas_err_init(&aerr);
    atlas_status st = atlas_db_begin(w->db, &aerr);
    if (st == ATLAS_OK) {
        st = atlas_memory_apply_in_tx(w->db, &info, obs, pol, now, &result, &aerr);
        if (st != ATLAS_OK) {
            /* Whole or nothing: a version row appended with no generation to
             * carry it, or a generation with no diff row for a claim it
             * affected, is the one outcome this pass must never leave. */
            atlas_db_rollback(w->db);
        } else {
            st = atlas_db_commit(w->db, &aerr);
            if (st != ATLAS_OK) {
                atlas_db_rollback(w->db);
            }
        }
    }

    if (st != ATLAS_OK) {
        atlas_daemon_log(w->log, "warn", "memory reconciliation for %s failed: %s",
                         atlas_safe(&safe, info.name), atlas_safe(&safe, atlas_err_msg(&aerr)));
    } else {
        atlas_daemon_log(w->log, "info",
                         "memory reconciled %s generation %lld: %zu sources, +%zu versions, "
                         "+%zu claims, ~%zu resolved, %zu unanchored, %zu diff rows",
                         atlas_safe(&safe, info.name), (long long)result.generation,
                         result.sources_seen, result.versions_added, result.claims_created,
                         result.claims_resolved, result.unanchored, result.diff_rows);
    }

    atlas_memory_observation_free(obs);
    free(obs);
    atlas_repo_info_free(&info);
    atlas_safe_pool_free(&safe);
}

/* Registers a repository. Runs here rather than on the IPC thread because it
 * writes, and every write in the daemon is this thread's. */
/* A13. Names the uid whose scanner may report about one repository.
 *
 * The write goes through `atlas_service_repo_set_scanner_db`, which is the same
 * function the local path calls: there is one write point, and this is a second
 * *route* to it rather than a second implementation of it.
 *
 * `arg1` is the repository name and `arg2` carries the uid as text, empty when
 * the operator named none and the owner of the root is to be derived. The job
 * shape already provides two strings; adding an integer field for one caller
 * would be a wider change than encoding a number the way every other argument
 * on this path is already encoded. */
/* A13. Records what a mirroring run left behind.
 *
 * `repo_id` names the repository, `exact_root` carries "the run finished and
 * skipped nothing", and Atlas supplies the time — a scanner's clock must not
 * decide when a mirror was last whole. The job shape already provides both
 * fields; adding one for this caller would be a wider change than reusing what
 * is there. */
static void run_mirror_state(atlas_writer *w, atlas_job *j) {
    const char *id_text = atlas_buf_cstr(&j->arg1);
    char *end = NULL;
    long long id = strtoll(id_text, &end, 10);
    if (end == id_text || *end != '\0' || id <= 0) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_USAGE,
                                  "a mirror state needs a repository id");
        return;
    }
    char now[32];
    atlas_now_iso8601(now, sizeof(now));
    j->result = atlas_db_repo_set_mirror_state(w->db, (int64_t)id, j->exact_root, now,
                                               &j->result_err);
    if (j->result == ATLAS_OK && j->exact_root && j->mirror_published) {
        /* A repository that has just become readable is one the watcher's view
         * of was built when it was not.
         *
         * A13.1. `mirror_published` narrows this to the case it was written for.
         * A run that changed nothing discards its staged generation instead of
         * swapping, so `<id>`'s directories keep the inodes the watcher already
         * holds watches on and the view built while the mirror was incomplete is
         * still the right one. Rebuilding anyway cost every *other* repository
         * an event gap too -- the flag and the rebuild are daemon-wide -- and an
         * event gap makes the next pass full, which re-hashes a repository whose
         * files nothing had touched. */
        atlas_writer_set_watch_dirty(w);
    }
}

static void run_repo_scanner(atlas_writer *w, atlas_job *j) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    const char *uid_text = atlas_buf_cstr(&j->arg2);
    bool uid_given = uid_text[0] != '\0';
    int64_t uid = 0;
    if (uid_given) {
        char *end = NULL;
        long long v = strtoll(uid_text, &end, 10);
        if (end == uid_text || *end != '\0') {
            j->result = atlas_err_set(&j->result_err, ATLAS_ERR_USAGE,
                                      "a scanner uid must be a decimal number");
            atlas_repo_info_free(&info);
            return;
        }
        uid = (int64_t)v;
    }
    atlas_status st = atlas_service_repo_set_scanner_db(w->db, atlas_buf_cstr(&j->arg1), uid_given,
                                                        uid, &info, &j->result_err);
    if (st == ATLAS_OK) {
        st = job_set_result(j, &info, &j->result_err);
    }
    j->result = st;
    if (st == ATLAS_OK) {
        /* Which tree this repository is read from has just changed, so the watch
         * set is no longer derived from current facts. The watcher re-derives it
         * rather than being handed one. */
        atlas_writer_set_watch_dirty(w);
    }
    atlas_repo_info_free(&info);
}

static void run_repo_add(atlas_writer *w, atlas_job *j) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    const char *name = j->arg2.len > 0 ? atlas_buf_cstr(&j->arg2) : NULL;
    /* A13: derive the scanner uid from the root's owner. This path has no
     * operator to name one — it is the daemon acting on a queued job. */
    atlas_status st = atlas_service_repo_add_db(w->db, atlas_buf_cstr(&j->arg1), name,
                                                j->exact_root, false, 0, &info, &j->result_err);
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

/* A12.1 T13. Builds the Canonical Context Pack for a root-task SUBMIT, before
 * `atlas_orch_apply` ever opens a transaction. `atlas_memory_pack_build` may
 * open the tree (`atlas_sem_source_identity`, gated on the repository row
 * saying dirty) and A1 forbids that inside a write transaction -- the same
 * seam `atlas_memory_observe` already uses one layer over. See `include/atlas/
 * memory.h`'s T12 section comment for the full derivation; this is where it
 * said T13's task would land.
 *
 * A child submission never reaches here (`op_submit`'s own branch does not
 * call `submit_resolve_run`'s root path for one), and a repository with no
 * generation yet builds successfully but is not frozen -- `has_context_pack`
 * says so, and `submit_resolve_run` reads it.
 *
 * A build failure is logged and answered packless, never by refusing the
 * whole submission: the memory layer vetoing a run's *creation* would be the
 * same coupling this season already refuses for a run's *acceptance*
 * (Decision 8's reliance check settles nothing, for the identical reason). */
static void run_orch_build_pack(atlas_writer *w, atlas_job *j) {
    atlas_orch_op *op = j->orch;
    if (op->kind != ATLAS_ORCH_OP_SUBMIT || op->spec.parent_job_uid.len > 0 ||
       j->memory_pol == NULL) {
        return;
    }
    atlas_err berr;
    atlas_err_init(&berr);
    atlas_status bs = atlas_memory_pack_build(w->db, op->repo_id, j->memory_pol,
                                              atlas_buf_cstr(&op->spec.task_text),
                                              &op->context_pack, &berr);
    if (bs != ATLAS_OK) {
        atlas_daemon_log(w->log, "warn",
                         "a context pack could not be built for repository %lld: %s",
                         (long long)op->repo_id, atlas_err_msg(&berr));
        return;
    }
    /* Zero means no generation exists yet -- a repository with no registered
     * source ever having produced one, T13's own reading of Decision 8's "no
     * repository with no sources or no generation gets a row". Freezing an
     * all-but-empty pack here would freeze something to compare every future
     * staleness read against forever, for a repository that has said nothing
     * yet.
     *
     * M4, T13 fix round, disclosed here for the first time: this checks
     * `memory_generation` alone, not the "no sources" half of the sentence
     * above. A repository whose declared sources were all deregistered from
     * the root-owned policy while an old generation still exists gets a row
     * here, because a generation surviving is exactly what this condition
     * reads as "has said something". Left this way rather than adding a
     * sources count: a stale-but-frozen pack is not a wrong one, and
     * `atlas_memory_pack_freshness`'s `SOURCE_SET` comparison
     * (`src/memory/pack.c`) reports the mismatch at the next lease the same
     * way it reports any other moved input. */
    op->has_context_pack = op->context_pack.memory_generation > 0;
}

/* A12.1 T13. Computes a granted LEASE's pack delivery-time freshness, after
 * `atlas_orch_apply`'s transaction has committed. `atlas_memory_pack_freshness`
 * may open the tree (gated on a non-empty pinned `source_identity`) and must
 * therefore run with none open, `atlas_memory_pack_build`'s own rule one
 * transaction later.
 *
 * A worker is never told a moved pack is current: if freshness cannot be
 * established at all -- no policy to compute it with, the row vanished
 * between the grant and now, or the read itself failed -- `context_pack` is
 * cleared rather than delivered with an empty status, because an unlabelled
 * body reads as silently CURRENT to anything that composes it. */
static void run_orch_lease_freshness(atlas_writer *w, atlas_job *j) {
    atlas_orch_result *r = &j->orch_result;
    if (j->orch->kind != ATLAS_ORCH_OP_LEASE || !r->granted || r->context_pack.len == 0) {
        return;
    }
    bool ok = false;
    if (j->memory_pol != NULL && r->run_uid.len > 0) {
        atlas_err ferr;
        atlas_err_init(&ferr);
        atlas_memory_pack pack;
        atlas_memory_pack_init(&pack);
        bool found = false;
        atlas_status fs = atlas_db_memory_pack_get(w->db, atlas_buf_cstr(&r->run_uid), &pack,
                                                   &found, &ferr);
        if (fs == ATLAS_OK && found) {
            atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
            atlas_buf which_moved = ATLAS_BUF_INIT;
            fs = atlas_memory_pack_freshness(w->db, j->memory_pol, &pack, &status, &which_moved,
                                             &ferr);
            if (fs == ATLAS_OK) {
                const char *line = status == ATLAS_MEMORY_PACK_CURRENT
                                       ? atlas_memory_pack_status_name(status)
                                       : atlas_buf_cstr(&which_moved);
                ok = atlas_buf_set_str(&r->context_pack_status, line, &ferr) == ATLAS_OK;
            }
            atlas_buf_free(&which_moved);
        }
        if (!ok) {
            atlas_daemon_log(w->log, "warn",
                             "a delivered context pack's freshness could not be established for "
                             "run %s: %s",
                             atlas_buf_cstr(&r->run_uid), atlas_err_msg(&ferr));
        }
        atlas_memory_pack_free(&pack);
    }
    if (!ok) {
        atlas_buf_reset(&r->context_pack);
        atlas_buf_reset(&r->context_pack_status);
    }
}

static void run_orch(atlas_writer *w, atlas_job *j) {
    if (j->orch == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "an orchestration job arrived with no operation attached");
        return;
    }
    /* M6, T13 fix round. This runs before `atlas_orch_apply` below has had
     * any chance to refuse the submission -- A11.0's four checks, the
     * idempotency check, A11.6's bounds. A SUBMIT that any of those refuses
     * has already paid `run_orch_build_pack`'s tree read and file read on the
     * writer thread. Cost, not correctness: `run_orch_build_pack` is a no-op
     * for anything but a root-task SUBMIT, and a refused submission's pack is
     * simply never frozen, never delivered, and never read again. Reordering
     * to build the pack only after a successful `atlas_orch_apply` would move
     * the tree read *inside* that function's transaction, which A1 forbids —
     * this seam exists for the same reason `run_orch_lease_freshness` runs
     * after the transaction below rather than inside it. */
    run_orch_build_pack(w, j);
    /* `atlas_orch_apply` owns its own transaction, exactly like
     * `atlas_ai_apply` and `atlas_decision_apply`, and is called with none
     * open — so an operation is whole or nothing, and a granted lease without
     * the attempt it belongs to cannot be committed. */
    j->result = atlas_orch_apply(w->db, j->orch, &j->orch_result, &j->result_err);
    if (j->result == ATLAS_OK) {
        run_orch_lease_freshness(w, j);
    }
}

/* A12.0. `atlas_plan_apply` owns its own transaction, exactly like the four
 * above, and is called with none open — so an operation is whole or nothing, and
 * a revision row without the task rows it compiled to cannot be committed.
 *
 * A refusal here is not always a failure of the caller's request: a planner
 * document that does not parse fills the result's `refusal` and `refusal_line`
 * and returns non-OK, and the rollback deliberately leaves those alone. They are
 * carried back to the caller by `atlas_writer_plan` on that path as well as on
 * the successful one. */
static void run_plan(atlas_writer *w, atlas_job *j) {
    if (j->plan == NULL || j->plan_result == NULL) {
        j->result = atlas_err_set(&j->result_err, ATLAS_ERR_INTERNAL,
                                  "a plan job arrived with no operation attached");
        return;
    }
    j->result = atlas_plan_apply(w->db, j->plan, j->plan_result, &j->result_err);
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


/* What an unbounded job calls to hand the writer thread back for a moment.
 *
 * Declared here and defined below, because the drain runs the very jobs the
 * `run_*` functions below it are: the cycle is real and a forward declaration is
 * the honest way to write it. It crosses into `src/sem` as a bare function
 * pointer and a `void *`, exactly as `atlas_sem_index_opts.cancel` already does,
 * so nothing in `src/sem` or `src/core` names a daemon type. */
static void writer_yield_cb(void *ud);

/* What an unbounded job calls to learn that the daemon is stopping, and the
 * record of whether it ever answered yes. Declared here and defined beside the
 * drain, because it is the same shape and answers from the same flag; the
 * reasoning is at the definition. */
typedef struct writer_cancel_state {
    atlas_writer *w;
    bool fired;
} writer_cancel_state;

static bool writer_cancel_cb(void *ud);

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
    /* The walk is handed the drain, so a repository with a large tree does not
     * hold every other write for the length of it. The walk finishes before the
     * transaction that records it opens, which is what makes that safe. */
    /* A13.1. `_run_on` corrects the read root before walking: the mirror for a
     * scanner-backed repository, the registered root otherwise. This job used to
     * do that itself, which is why the two service-layer walks could be written
     * without it and nothing said so. One door now. */
    if (atlas_sem_discovery_run_on(w->db, atlas_buf_cstr(&w->data_dir), &repo, writer_yield_cb, w,
                                   &res, &err) != ATLAS_OK) {
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

    /* Whether this pass was stopped by us rather than by anything about the
     * repository. It has to be remembered rather than re-derived, because
     * `stopping` can become true in the moment after a pass finished normally,
     * and that is a different fact from "this pass was cut short". */
    writer_cancel_state cancel_state = {w, false};

    if (st == ATLAS_OK) {
        /* The pass is handed the drain. This is the job the season is about: it
         * runs a compiler over every unit the build describes, and until it
         * offered the thread back between them nothing else in this daemon was
         * written for as long as it took.
         *
         * It is handed the cancel beside it so that shutting down does not have
         * to wait for the whole of it; see `writer_cancel_cb`. */
        st = atlas_sem_index_on(w->db, atlas_buf_cstr(&w->data_dir), &repo, compdbs, n,
                                j->sem_rebuild, writer_yield_cb, w, writer_cancel_cb,
                                &cancel_state, &sum, &err);
    }

    /* Only an *automatic* attempt feeds the governor, and `op_id == 0` is how
     * this thread knows: an operation id exists when a client asked and is
     * polling, and a client asking is a different principal making a different
     * decision. An operator running `code index` against a repository that
     * cannot build should get the failure reported to them every time rather
     * than be told the governor is holding — and their attempt must not be able
     * to clear a governor record either, which is why success only clears it on
     * this path. */
    /* **A pass Atlas stopped is not evidence about the repository.**
     *
     * Cancellation surfaces as `ATLAS_ERR_USAGE`, which the classification below
     * reads as `ATLAS_SEM_WHY_BUILD_DESCRIPTION` — the build description is
     * broken, go and fix your compilation database. That would be false, and it
     * would not merely mislead: that reason holds the governor while the source
     * identity is unchanged, so a shutdown landing mid-pass would leave the
     * repository refusing to rebuild until somebody happened to edit a file.
     *
     * So the attempt is not recorded at all. Not recorded is the truthful state:
     * Atlas learned nothing about this repository, and the governor keeps
     * whatever it already held. The next daemon derives freshness from the
     * stored generation exactly as it would have, finds it stale, and schedules
     * the pass again. */
    if (cancel_state.fired && j->op_id == 0) {
        atlas_daemon_log(w->log, "info",
                         "semantic index for repository %lld was cancelled because the daemon is "
                         "stopping; nothing was published and no attempt was recorded",
                         (long long)j->repo_id);
    } else if (j->op_id == 0) {
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

    /* **A failed pass says so.** The reason was recorded and never logged, so a
     * daemon rebuilding nothing looked exactly like a daemon with nothing to
     * rebuild: "semantic index scheduled" every fifteen seconds, no generation
     * for ninety minutes, and no line anywhere saying why. Measured on this
     * machine, and it cost most of a day of looking in the wrong places.
     *
     * The message is Atlas' own fixed string plus the error text, which is
     * safe-encoded like every other untrusted value on this path -- a compiler
     * diagnostic or a path can appear in it. */
    if (st != ATLAS_OK && !cancel_state.fired) {
        atlas_safe_pool safe;
        atlas_safe_pool_init(&safe);
        atlas_daemon_log(w->log, "warn", "semantic index for %s failed: %s",
                         atlas_safe(&safe, atlas_buf_cstr(&j->arg1)),
                         atlas_safe(&safe, atlas_err_msg(&err)));
        atlas_safe_pool_free(&safe);
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
    /* **An automatic pass says what it did.** The summary went only to
     * `atlas_ops_finish`, and an automatic sweep has no operation -- `op_id` is
     * zero -- so the daemon rebuilt, or did not rebuild, and told nobody. What
     * that looked like from outside: "semantic index scheduled" every fifteen
     * seconds, the generation unchanged for ninety minutes, no error anywhere,
     * and no way to tell a pass that failed from one that decided there was
     * nothing to do. Reconcile has logged its own line since A1 for the same
     * reason. */
    if (j->op_id == 0 && st == ATLAS_OK) {
        atlas_safe_pool safe;
        atlas_safe_pool_init(&safe);
        atlas_daemon_log(w->log, "info",
                         "semantic index for %s generation %lld: %lld units, %lld complete, "
                         "%lld failed",
                         atlas_safe(&safe, atlas_buf_cstr(&j->arg1)),
                         (long long)sum.generation_id, (long long)sum.units_total,
                         (long long)sum.units_complete, (long long)sum.units_failed);
        atlas_safe_pool_free(&safe);
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


/* Run exactly one job to completion, and settle who owns it afterwards.
 *
 * **There is one implementation of the completion protocol and this is it.** The
 * main loop below calls it, and so does the yield drain, and a second copy of
 * these rules is the defect this file's history predicts: the ownership exit is
 * three lines that must all happen in one lock hold, and two copies of it would
 * agree until the day one of them was edited.
 *
 * Called with the lock **not** held, and it takes the lock twice: once to claim
 * the job before running it, once to complete it.
 *
 * The claim is saved and restored rather than set and cleared, which is what
 * lets the drain reuse this untouched. From the main loop the writer was idle,
 * so the restore puts it back to idle — byte for byte what the loop did before
 * this function existed. From a drain the writer was already inside an unbounded
 * job, so for the length of the drained job `running_kind` truthfully names the
 * *bounded* kind being served, and the restore puts the pass's kind back in the
 * same hold that completes the job. That is what keeps `writer_wait_locked`
 * correct with no special case anywhere: mid-drain the running kind is bounded,
 * so nothing backs out; the instant the drain ends it is unbounded again.
 *
 * One window falls out of that and is not a defect: between two drained jobs the
 * restored kind is briefly the pass's, so a waiter whose grace has *already*
 * expired can back out at the very moment the drain would have reached it. The
 * refusal is still exactly true — its job is removed and never ran, and sending
 * it again is safe — so what it costs is one retry, not a lost write.
 *
 * `wants_result` is read inside the completing hold rather than before it, and
 * that is what decides who frees the job. A waiter that has given up clears it
 * under this same lock, so reading it outside could see the waiter still there,
 * hand it the job, and leak — the waiter has already returned. Whoever observes
 * it last under the lock owns the answer. */
static void writer_run_job(atlas_writer *w, atlas_job *j) {
    (void)pthread_mutex_lock(&w->lock);
    bool was_running = w->running;
    atlas_job_kind was_kind = w->running_kind;
    /* Claimed before the lock is released, so there is no instant in which this
     * thread owns a job and reports itself idle. A waiter that saw that instant
     * would conclude its own job could not be the one running. */
    w->running = true;
    w->running_kind = j->kind;
    /* P0. The test channel's stall, taken while the claim is held, because that
     * is precisely the state being modelled: the writer owns a job and is inside
     * it. `stopping` releases it, so a stalled writer is still joinable. */
    while (w->test_stall_armed && w->test_stall_kind == j->kind && !w->stopping) {
        w->test_stall_active = true;
        (void)pthread_cond_wait(&w->test_release_cv, &w->lock);
    }
    w->test_stall_active = false;
    (void)pthread_mutex_unlock(&w->lock);

    switch (j->kind) {
    case ATLAS_JOB_RECONCILE: run_reconcile(w, j); break;
    case ATLAS_JOB_REPO_ADD: run_repo_add(w, j); break;
    case ATLAS_JOB_REPO_REMOVE: run_repo_remove(w, j); break;
    case ATLAS_JOB_REPO_SCANNER: run_repo_scanner(w, j); break;
    case ATLAS_JOB_MIRROR_STATE: run_mirror_state(w, j); break;
    case ATLAS_JOB_AI: run_ai(w, j); break;
    case ATLAS_JOB_DECISION: run_decision(w, j); break;
    case ATLAS_JOB_ORCH: run_orch(w, j); break;
    case ATLAS_JOB_VERIFY: run_verify(w, j); break;
    case ATLAS_JOB_PLAN: run_plan(w, j); break;
    case ATLAS_JOB_SNAPSHOT: run_snapshot(w, j); break;
    case ATLAS_JOB_SEM_INDEX: run_sem_index(w, j); break;
    case ATLAS_JOB_SEM_DISCOVER: run_sem_discover(w, j); break;
    case ATLAS_JOB_MEMORY_RECONCILE: run_memory_reconcile(w, j); break;
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
            j->result = atlas_sem_config_on(w->db, atlas_buf_cstr(&w->data_dir), j->sem_config,
                                            j->sem_config_out,
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
                co.no_scopes = j->apikey->no_scopes;
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
        /* P0. The test channel's write fault. Injected here — after the dequeue,
         * at the point the row would change — because that is the failure the
         * watcher has to survive: the job ran, the database refused it, and
         * nothing is coming back to say so. A `SET_WATCH` carries no result and
         * nobody waits for it, so the only correct response is for the watcher
         * to still believe it owes the publication. */
        bool test_fail = false;
        (void)pthread_mutex_lock(&w->lock);
        if (w->test_fail_set_watch > 0) {
            w->test_fail_set_watch--;
            w->test_set_watch_failed++;
            test_fail = true;
        }
        (void)pthread_mutex_unlock(&w->lock);
        if (test_fail) {
            break;
        }
        const char *detail = j->arg1.len > 0 ? atlas_buf_cstr(&j->arg1) : NULL;
        /* P0. Gap flags first, then the outcome, and the order is the point.
         *
         * `mark_gap` also sets `watch_state='incomplete'`, which is right for
         * its other callers — a queue overflow, a failed pass — and wrong here,
         * because the watcher knows something more specific and has a reason
         * code for it. Writing the outcome second lets the gap flags stand while
         * the state and the reason are the watcher's. Both statements run on
         * this thread inside one job, so nothing can interleave between them. */
        if (j->watch_mark_gap) {
            (void)atlas_db_index_state_mark_gap(w->db, j->repo_id, detail, &ignore);
        }
        atlas_watch_outcome outcome;
        atlas_watch_outcome_init(&outcome);
        outcome.state = (atlas_watch_state)j->watch_state;
        outcome.reason = (atlas_watch_reason)j->watch_reason;
        outcome.detail = detail;
        outcome.source_dirs = j->watched_source;
        outcome.meta_dirs = j->watched_meta;
        outcome.shared_dirs = j->watched_shared;
        (void)atlas_db_index_state_set_watch(w->db, j->repo_id, &outcome, &ignore);
        break;
    }
    /* A12.1 T11. This dispatch listing every enumerator (`-Wswitch-enum`, not
     * merely `-Wswitch`) is what made a job kind with no case here a build
     * failure rather than a silent `default:` no-op while `run_memory` did
     * not exist yet, so the classification switches above and this one could
     * not drift apart about which kinds exist. */
    case ATLAS_JOB_MEMORY: run_memory(w, j); break;
    default: break;
    }

    /* One hold restores the claim, completes the job and wakes the waiters,
     * because a waiter that saw the writer idle while its own job was still
     * unfinished would take its job back out of a queue it is no longer in. */
    (void)pthread_mutex_lock(&w->lock);
    w->running = was_running;
    w->running_kind = was_kind;
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

/* A pause in an unbounded job, long enough to serve what is waiting.
 *
 * **A RESOLVED QUEUE THAT NOBODY COULD REACH WAS STILL A BACKLOG, NOT WORK.**
 * A9.2.4 put a minutes-long pass on the single writer thread, and from that
 * moment every other write was refused for the duration — honestly, and
 * uselessly. The pass already chunks its own work and already commits per batch;
 * what it did not do was let anything else run between the chunks. This is that,
 * and it is scheduling rather than state: it records nothing durable, and a
 * daemon that never called it would produce the same rows, later.
 *
 * Called only from a point the pass has chosen — between translation units,
 * between chunks of a directory walk — and every one of those points is outside
 * any transaction. The guard below re-asks that anyway, and it is belt and
 * braces rather than a real branch: A1 forbids holding `BEGIN IMMEDIATE` across
 * unbounded work, so a yield point inside a transaction would already be a bug,
 * and this turns it into a no-op instead of a nested write. It is cheap and it
 * is checked at the one place where being wrong would corrupt something.
 *
 * Jobs enqueued while draining are picked up by this same loop or at the next
 * yield; either is correct, and neither is a starvation risk, because the loop
 * ends the moment nothing eligible is left. Everything ineligible keeps its
 * position untouched. */
static void writer_yield(atlas_writer *w) {
    if (atlas_db_in_transaction(w->db)) {
        return;
    }
    for (;;) {
        (void)pthread_mutex_lock(&w->lock);
        atlas_job *j = NULL;
        /* Front to back, so first-in-first-out holds among the kinds that are
         * eligible and within every one of them. The first eligible job wins;
         * anything ahead of it that is not eligible simply stays where it is. */
        for (size_t k = 0; j == NULL && k < w->count; k++) {
            atlas_job *q = w->queue[(w->head + k) % ATLAS_WRITER_QUEUE_MAX];
            if (job_kind_is_drainable(q->kind)) {
                j = q;
            }
        }
        if (j != NULL) {
            /* The same excision a waiter that gave up performs, and the same
             * shifting discipline: every other job keeps its position. */
            (void)queue_remove(w, j);
        }
        (void)pthread_mutex_unlock(&w->lock);
        if (j == NULL) {
            return;
        }
        writer_run_job(w, j);
    }
}

/* The `void *`-shaped face of the drain, which is how it crosses into `src/sem`.
 *
 * A bare function pointer and an opaque pointer, exactly as `cancel` already
 * travels, so the semantic layer calls this without ever naming a writer, a job
 * or a queue. */
static void writer_yield_cb(void *ud) {
    writer_yield((atlas_writer *)ud);
}

/* Whether an unbounded pass should abandon what it is building because the
 * daemon is stopping.
 *
 * `stopping` was read at every submission point and in the idle wait, and
 * nowhere inside a running job — so `atlas_writer_stop` set it, broadcast, and
 * then joined a thread that still had a compiler pass over every translation
 * unit ahead of it. Measured on this machine 2026-08-28: two consecutive
 * shutdowns exceeded `TimeoutStopSec=30`, both with semantic maintenance live,
 * and systemd killed the daemon with SIGKILL. The next start then logged that
 * the previous one had not shut down cleanly and marked every repository
 * incomplete, which is a full pass owed for a shutdown that was orderly right
 * up to the point nobody told the pass about it.
 *
 * The test channel already worked this way and is the evidence the shape was
 * understood: the stall in `writer_run_job` releases on `stopping`, "so a
 * stalled writer is still joinable". A real pass had no equivalent.
 *
 * Answering true fails the generation rather than publishing a partial one,
 * which is the contract `atlas_sem_index_opts.cancel` already states — and it is
 * the right answer here, because A9.2.5's rule is that an interrupted index
 * never replaces the last valid generation. A shutdown costs the work done so
 * far and nothing else: the next daemon finds the stored generation stale and
 * schedules the pass again.
 *
 * The lock is taken because `stopping` is written under it. This runs on the
 * writer thread between units, where no lock is held and no transaction is
 * open, so there is nothing here to deadlock against. */
static bool writer_cancel_cb(void *ud) {
    writer_cancel_state *cs = (writer_cancel_state *)ud;
    (void)pthread_mutex_lock(&cs->w->lock);
    const bool stopping = cs->w->stopping;
    (void)pthread_mutex_unlock(&cs->w->lock);
    if (stopping) {
        /* Written on the writer thread and read on the writer thread, after the
         * pass has returned. No other thread touches it. */
        cs->fired = true;
    }
    return stopping;
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
        (void)pthread_mutex_unlock(&w->lock);
        if (j == NULL) {
            continue;
        }
        /* Claiming the job, running it, completing it and settling who frees it
         * are one sequence with one implementation — the same one the yield
         * drain uses. See `writer_run_job`. */
        writer_run_job(w, j);
    }

    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_db_daemon_stopped(w->db, &ignore);
    atlas_db_close(w->db);
    w->db = NULL;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

atlas_status atlas_writer_start(const char *db_path, const char *data_dir,
                                const char *socket_path, atlas_workers *workers, FILE *log,
                                atlas_writer **out, atlas_err *err) {
    *out = NULL;
    atlas_writer *w = calloc(1u, sizeof(*w));
    if (w == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the writer");
    }
    atlas_buf_init(&w->db_path);
    atlas_buf_init(&w->data_dir);
    atlas_buf_init(&w->socket_path);
    w->workers = workers;
    w->log = log;
    atlas_err_init(&w->ready_err);
    if (pthread_mutex_init(&w->lock, NULL) != 0 || pthread_cond_init(&w->not_empty, NULL) != 0 ||
        pthread_cond_init(&w->job_done, NULL) != 0 || pthread_cond_init(&w->ready_cv, NULL) != 0 ||
        pthread_cond_init(&w->test_release_cv, NULL) != 0) {
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create writer synchronisation");
    }
    atlas_status st = atlas_buf_set_str(&w->db_path, db_path, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&w->data_dir, data_dir != NULL ? data_dir : "", err);
    }
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
        /* A thread held by the test channel's stall waits on its own condition,
         * so shutting down has to wake that one too or the join never returns. */
        (void)pthread_cond_broadcast(&w->test_release_cv);
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
    (void)pthread_cond_destroy(&w->test_release_cv);
    (void)pthread_mutex_destroy(&w->lock);
    atlas_buf_free(&w->db_path);
    atlas_buf_free(&w->data_dir);
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

/* A13. Queues a mirror state and does not wait for it.
 *
 * A8-CI's rule, one layer down: an operation that can outlast a client's
 * patience does not run in the serve loop, and the client is answered when the
 * work is *accepted*. Measured on a live daemon: a scanner's `scanner.state`
 * arrived while the writer was 19,864 ms into a full reconciliation of the same
 * repository — every one of 21,996 files re-hashed, because publishing a mirror
 * generation replaces every inode and no stored filesystem identity survives it
 * — and the call timed out reading its response frame. The write had been queued
 * and did land, so the warning the scanner printed was false.
 *
 * Nothing goes silent. `scanner.poll` answers `full` while no complete mirror
 * exists, so a job that fails is answered by the next poll asking for the
 * mirror again — the poll is the confirmation channel, exactly as it is the
 * heartbeat. Raising the scanner's deadline instead would have needed a bound
 * on how long a reconciliation may take, and Atlas has no such bound to derive
 * one from.
 *
 * Not coalesced, unlike a reconciliation: two states for one repository are two
 * different assertions about it, and the later one is not a repetition of the
 * earlier. */
atlas_status atlas_writer_submit_mirror_state(atlas_writer *w, int64_t repo_id, bool complete,
                                              bool published, atlas_err *err) {
    if (w == NULL || repo_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a mirror state needs a repository id");
    }
    atlas_job *j = job_new(ATLAS_JOB_MIRROR_STATE);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a mirror state");
    }
    char id[32];
    (void)snprintf(id, sizeof(id), "%lld", (long long)repo_id);
    atlas_status st = atlas_buf_set_str(&j->arg1, id, err);
    if (st != ATLAS_OK) {
        job_free(j);
        return st;
    }
    j->repo_id = repo_id;
    j->exact_root = complete;
    j->mirror_published = published;

    (void)pthread_mutex_lock(&w->lock);
    if (w->stopping) {
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the Atlas daemon is shutting down");
    }
    j->sync_seq = ++w->next_sync_seq;
    if (!queue_push(w, j)) {
        (void)pthread_mutex_unlock(&w->lock);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the Atlas daemon's write queue is full (%u pending); the request was "
                             "refused rather than dropped",
                             (unsigned)ATLAS_WRITER_QUEUE_MAX);
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
                                const char *detail, const atlas_watch_outcome *outcome,
                                bool mark_gap, atlas_err *err) {
    atlas_job *j = job_new(kind);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a watcher note");
    }
    j->repo_id = repo_id;
    if (outcome != NULL) {
        j->watch_state = (int)outcome->state;
        j->watch_reason = (int)outcome->reason;
        j->watched_source = outcome->source_dirs;
        j->watched_meta = outcome->meta_dirs;
        j->watched_shared = outcome->shared_dirs;
    }
    j->watch_mark_gap = mark_gap;
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
    return submit_note(w, ATLAS_JOB_MARK_GAP, repo_id, detail, NULL, false, err);
}

atlas_status atlas_writer_submit_watch_outcome(atlas_writer *w, int64_t repo_id,
                                               const atlas_watch_outcome *outcome, bool mark_gap,
                                               atlas_err *err) {
    return submit_note(w, ATLAS_JOB_SET_WATCH, repo_id, outcome->detail, outcome, mark_gap, err);
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

/* A13. Names a repository's scanner uid, while the daemon runs.
 *
 * `uid_text` is empty when the operator named none, which asks the write point
 * to derive it from the root's owner -- the same defaulting the local path
 * performs, because it is the same function. */
atlas_status atlas_writer_call_repo_scanner(atlas_writer *w, const char *name,
                                            const char *uid_text, int timeout_ms,
                                            atlas_writer_result *result, atlas_err *err) {
    return writer_call_impl(w, ATLAS_JOB_REPO_SCANNER, name, uid_text, false, timeout_ms, result,
                            err);
}

/* A13. Records a mirroring run's verdict, through the writer.
 *
 * The repository id travels as text in `arg1` and `complete` in `exact_root`,
 * because a job carries no field of its own for either; see `run_mirror_state`.
 * It goes through the writer because it is a write: `scanner.state` first did it
 * on the dispatch handle, which is read-only, and failed every time. */

void atlas_writer_test_stall(atlas_writer *w, atlas_job_kind kind) {
    (void)pthread_mutex_lock(&w->lock);
    w->test_stall_armed = true;
    w->test_stall_kind = kind;
    (void)pthread_mutex_unlock(&w->lock);
}

void atlas_writer_test_release(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    w->test_stall_armed = false;
    (void)pthread_cond_broadcast(&w->test_release_cv);
    (void)pthread_mutex_unlock(&w->lock);
}

void atlas_writer_test_fail_watch_writes(atlas_writer *w, int64_t n) {
    (void)pthread_mutex_lock(&w->lock);
    w->test_fail_set_watch = n;
    (void)pthread_mutex_unlock(&w->lock);
}

bool atlas_writer_test_stalled(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    bool held = w->test_stall_active;
    (void)pthread_mutex_unlock(&w->lock);
    return held;
}

int64_t atlas_writer_test_watch_writes_failed(atlas_writer *w) {
    (void)pthread_mutex_lock(&w->lock);
    int64_t n = w->test_set_watch_failed;
    (void)pthread_mutex_unlock(&w->lock);
    return n;
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

/* A12.0. The same shape as `atlas_writer_verify` — the job owns the operation
 * and the result slot, the wait is bounded, and a request that times out is
 * detached rather than freed — with one deliberate difference.
 *
 * **The result is moved on the failure path too.** Every other wrapper in this
 * file hands the caller a result only when the operation succeeded, because for
 * every other domain a failure is entirely described by `atlas_err`. A plan
 * operation has a second kind of failure: a planner's document that does not
 * parse. That is a *model's* mistake rather than a caller's, and the answer to it
 * is the refusal sentence and the line it happened on, which
 * `atlas_plan_apply` fills in and the rollback leaves alone. Moving the result
 * only on success would leave those on the writer thread to be freed, and the
 * driver with Atlas' prose to parse for a line number.
 *
 * The status still says what happened; the result carries what to do about it. */
atlas_status atlas_writer_plan(atlas_writer *w, atlas_plan_op *op, int timeout_ms,
                               atlas_plan_result *result, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_PLAN);
    atlas_plan_result *slot = calloc(1u, sizeof *slot);
    if (j == NULL || slot == NULL) {
        /* Ownership is taken unconditionally, here as everywhere. */
        atlas_plan_op_free(op);
        free(op);
        free(slot);
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a plan request");
    }
    atlas_plan_result_init(slot);
    j->plan = op;
    j->plan_result = slot;
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
    /* Move, on both paths. The job's slot is emptied so `job_free` releases
     * nothing twice. */
    atlas_plan_result_free(result);
    *result = *j->plan_result;
    memset(j->plan_result, 0, sizeof *j->plan_result);
    if (st != ATLAS_OK) {
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
                               const atlas_syspolicy *pol, atlas_orch_result *result,
                               atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_ORCH);
    if (j == NULL) {
        atlas_orch_op_free(op);
        free(op);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory queueing an orchestration request");
    }
    j->orch = op;
    j->wants_result = true;
    /* A12.1 T13. Copied rather than referenced, `atlas_writer_submit_memory_
     * reconcile`'s own reason: `pol` is very often the caller's own stack
     * variable, and this job can outlive that frame by as long as the queue is
     * deep. NULL for every op kind `run_orch` never consults it for. */
    if (pol != NULL) {
        j->memory_pol = malloc(sizeof *j->memory_pol);
        if (j->memory_pol == NULL) {
            job_free(j);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "out of memory queueing an orchestration request");
        }
        *j->memory_pol = *pol;
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
        result->run_status = j->orch_result.run_status;
        result->worker_starts = j->orch_result.worker_starts;
        /* A10.1. The run's frozen memory mode and the digest of what it was
         * shown. On this list for the reason the block below says: a field
         * missing from it reaches a socket client as an absent key however
         * carefully the write point filled it in, and this one did exactly that
         * until a fake-driver dry run noticed the worker was never handed its
         * package. */
        result->memory_mode = j->orch_result.memory_mode;
        memcpy(result->memory_digest, j->orch_result.memory_digest,
               sizeof(result->memory_digest));
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
            /* A11.0/A11.1. The run the operation settled, the task it created,
             * and what the run now is. Copied here for the reason every line
             * above it is: this is the boundary between the writer thread's
             * result and the caller's, and a field that is not on this list
             * reaches a socket client as an absent key however carefully the
             * write point filled it in. */
            {&result->run_uid, &j->orch_result.run_uid},
            {&result->follow_up_job_uid, &j->orch_result.follow_up_job_uid},
            /* A10.1's package. The bytes a worker is shown travel this way and
             * no other. */
            {&result->memory_package, &j->orch_result.memory_package},
            /* A12.1 T13. The pack body and its post-commit freshness line,
             * computed by `run_orch_lease_freshness` above before this copy
             * ever runs. A field missing from this list is a pack the daemon
             * built and the caller never sees -- `rundriver.c:936`'s own
             * lesson, one layer out. */
            {&result->context_pack, &j->orch_result.context_pack},
            {&result->context_pack_status, &j->orch_result.context_pack_status},
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

/* A12.1. Queues one bounded memory reconciliation pass and returns
 * immediately -- fire-and-forget like `atlas_writer_submit_sem_discover`
 * above, for the same reason: the sweep that calls this is a timer, not a
 * waiter, and `memory.status` (T11) is the confirmation channel.
 *
 * Coalesced against the *queue* only, not against a running pass the way
 * `atlas_writer_sem_index_pending` guards ATLAS_JOB_SEM_INDEX: a duplicate
 * reconciliation is idempotent rather than merely wasted -- a pass over a
 * source nothing moved reports generation 0 -- so the cheaper queue-only
 * check already costs nothing wrong. */
atlas_status atlas_writer_submit_memory_reconcile(atlas_writer *w, int64_t repo_id,
                                                  const atlas_syspolicy *pol, atlas_err *err) {
    atlas_job *j = job_new(ATLAS_JOB_MEMORY_RECONCILE);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory queueing a memory reconciliation");
    }
    j->repo_id = repo_id;
    /* Copied rather than referenced: `pol` is very often the caller's own
     * stack variable, and this job can outlive that frame by as long as the
     * queue is deep. `atlas_syspolicy` is plain data, so the copy is the
     * whole of taking ownership. */
    j->memory_pol = malloc(sizeof *j->memory_pol);
    if (j->memory_pol == NULL) {
        job_free(j);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory queueing a memory reconciliation");
    }
    *j->memory_pol = *pol;
    (void)pthread_mutex_lock(&w->lock);
    bool duplicate = false;
    for (size_t k = 0; !duplicate && k < w->count; k++) {
        const atlas_job *q = w->queue[(w->head + k) % ATLAS_WRITER_QUEUE_MAX];
        if (q->kind == ATLAS_JOB_MEMORY_RECONCILE && q->repo_id == repo_id) {
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

/* A12.1 T11. See this function's own declaration in daemon_internal.h for why
 * the length bound is checked here rather than inside `run_memory`, and why a
 * caller waits for it at all. `atlas_apikey_job`'s exact shape: both `op` and
 * `out` are borrowed, never copied, so a caller that gives up clears them to
 * NULL under the lock rather than transferring ownership to a job that might
 * run seconds later. */
atlas_status atlas_writer_memory_put(atlas_writer *w, const atlas_memory_put_op *op,
                                     atlas_memory_put_result *out, atlas_err *err) {
    if (w == NULL || op == NULL || out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no memory put to run");
    }
    if (op->content.len > ATLAS_MEMORY_MAX_SOURCE_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "content is %zu bytes; the bound is %u",
                             op->content.len, ATLAS_MEMORY_MAX_SOURCE_BYTES);
    }

    atlas_job *j = job_new(ATLAS_JOB_MEMORY);
    if (j == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory queueing a memory write");
    }
    j->memory_put = op;
    j->memory_put_out = out;
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
    deadline.tv_sec += ATLAS_IPC_WRITE_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (long)(ATLAS_IPC_WRITE_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    bool backed_out = writer_wait_locked(w, j, &deadline);
    bool done = j->done;
    atlas_status st = j->result;
    atlas_err jerr = j->result_err;
    if (!done) {
        /* Detached rather than freed: the job is still the writer's. Clearing
         * the pointers is what stops it writing into a result struct whose
         * owner has gone. */
        j->wants_result = false;
        j->memory_put = NULL;
        j->memory_put_out = NULL;
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
                             "the Atlas daemon did not complete a memory put within %d ms",
                             ATLAS_IPC_WRITE_TIMEOUT_MS);
    }
    if (st != ATLAS_OK) {
        *err = jerr;
    }
    job_free(j);
    return st;
}

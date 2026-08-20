/* Atlas - A11.1: the operator's foreground run driver.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/rundriver.h for what this is and, more importantly, for what it
 * deliberately is not.
 *
 * The order of operations in `drive_one` is the whole safety argument, so it is
 * written once, here, and every step says what it would cost to move it:
 *
 *   1. Read the run. A terminal run is touched not at all.
 *   2. Claim the run's active task *by name*, in one transaction. Two drivers
 *      racing here produce one grant, because the grant is a compare-and-swap
 *      against `state = 'QUEUED'` and the lease's partial unique index. The
 *      loser is told `busy` and writes nothing.
 *   3. Check the repository is still at the commit the task was pinned to,
 *      **before** starting anything. A tree that has moved is not the tree the
 *      work was authorised over, and continuing would produce changes against
 *      something else.
 *   4. Record RUNNING. This is durable before the worker exists, which is what
 *      makes the run's worker-start budget count real starts rather than
 *      completed ones — a crash spends budget exactly as a finish does.
 *   5. Start exactly one worker, in the repository's root.
 *   6. Check the pinned commit again. A worker that committed, reset or checked
 *      out has invalidated everything a gate could tell us, so the gates are not
 *      run at all and the run is refused rather than judged.
 *   7. Run the gates, in the repository's root, from the task's own stored list.
 *   8. Report. The daemon decides what the run is.
 */
#define _GNU_SOURCE 1

#include "atlas/rundriver.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/driver.h"
#include "atlas/validate.h"
#include "atlas/ipc.h"
#include "atlas/git.h"
#include "atlas/orch_memory.h"
#include "atlas/sha256.h"

void atlas_rundriver_report_init(atlas_rundriver_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->run_uid);
    atlas_buf_init(&r->last_job_uid);
}

void atlas_rundriver_report_free(atlas_rundriver_report *r) {
    if (r != NULL) {
        atlas_buf_free(&r->run_uid);
        atlas_buf_free(&r->last_job_uid);
    }
}

static void say(const atlas_rundriver_opts *o, const char *fmt, ...) {
    if (o->log == NULL) {
        return;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(o->log, "%s run ", at);
    (void)vfprintf(o->log, fmt, ap);
    (void)fputc('\n', o->log);
    va_end(ap);
    (void)fflush(o->log);
}

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* The repository's HEAD, right now.
 *
 * Opened read-only through the ordinary hardened adapter and closed again: this
 * is the one place the run driver touches git, it reads and nothing else, and
 * it creates no process outside `atlas_proc_run`. */
static atlas_status head_commit(const char *root, atlas_buf *out, atlas_err *err) {
    atlas_git *g = NULL;
    atlas_status st = atlas_git_open(root, &g, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    st = atlas_git_read_head(g, &h, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(out, h.oid, err);
    }
    atlas_git_close(g);
    return st;
}

/* --- one claimed task ------------------------------------------------------ */

typedef struct claimed {
    atlas_buf job_uid;
    atlas_buf token;
    atlas_buf repo_root;
    atlas_buf commit;
    atlas_buf mode;
    atlas_buf driver;
    atlas_buf task;
    atlas_buf validations;
    /* A10.1. The run's frozen memory package, exactly as the daemon granted it.
     * Read nowhere in this file except where it is appended to the task; no
     * branch depends on its contents, and there is no path by which anything
     * inside it reaches a gate, a status or a decision. */
    atlas_buf memory;
    int64_t attempt_no;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_output_bytes;
    /* The live half. `ATLAS_ORCH_LEASE_MS` is a minute and a real worker runs
     * for many; a lease is renewed while the child works or the daemon reclaims
     * the attempt underneath it, marks it timed out, requeues the task — and
     * the completion carrying the worker's whole result is then refused as an
     * unknown token. That is not a lost message, it is a second worker on the
     * same task, which is exactly what this milestone must not do. */
    int64_t last_heartbeat_ms;
    /* The phase the attempt is in, so a renewal names it and therefore renews
     * without transitioning. Reading it from anywhere else would mean a
     * heartbeat could move the attempt by accident. */
    atlas_orch_state phase_now;
    bool cancelled;
    const atlas_rundriver_opts *opts;
} claimed;

static void claimed_init(claimed *c) {
    memset(c, 0, sizeof(*c));
    atlas_buf_init(&c->job_uid);
    atlas_buf_init(&c->memory);
    atlas_buf_init(&c->token);
    atlas_buf_init(&c->repo_root);
    atlas_buf_init(&c->commit);
    atlas_buf_init(&c->mode);
    atlas_buf_init(&c->driver);
    atlas_buf_init(&c->task);
    atlas_buf_init(&c->validations);
}

static void claimed_free(claimed *c) {
    atlas_buf_free(&c->job_uid);
    /* The token is a bearer capability. Zeroed before release rather than left
     * in freed memory, which is what the dispatcher does with its own. */
    if (c->token.data != NULL && c->token.len > 0) {
        memset(c->token.data, 0, c->token.len);
    }
    atlas_buf_free(&c->token);
    atlas_buf_free(&c->repo_root);
    atlas_buf_free(&c->commit);
    atlas_buf_free(&c->mode);
    atlas_buf_free(&c->driver);
    atlas_buf_free(&c->task);
    atlas_buf_free(&c->validations);
    atlas_buf_free(&c->memory);
}

/* One operation, retried while the daemon says it took nothing.
 *
 * `BUSY:` is the daemon's promise that the write was taken back out of the
 * queue before anything looked at it — A9.2.6 wrote that sentence into the
 * message precisely so a caller could act on it. Every write this driver makes
 * is worth retrying under it, and the completion most of all: it carries a
 * worker's entire result, and giving up on it means the lease expires, the task
 * is requeued, and a second worker runs the same work.
 *
 * Bounded, because a retry loop with no end is a hang. When the budget is spent
 * the error is returned unchanged and the run stays ACTIVE and resumable — the
 * caller has lost this invocation, not the run. */
#define RUN_BUSY_TRIES 12
#define RUN_BUSY_PAUSE_MS 500

/* A12.0. The other way a call fails, and the other claim it makes.
 *
 * `BUSY:` is the daemon saying it took nothing. A transport failure — the
 * connect, the send, the read, a reply that was never a reply — says nothing at
 * all about whether the request was processed, and A11.1 treated it as fatal to
 * the invocation. Pilot A11.6-P lost a run to that twice: a congested serve loop
 * timed out one frame-header read on a phase call, the foreground driver exited,
 * the worker kept working, nobody renewed the lease, and the attempt was
 * reclaimed underneath a process that was still editing the tree.
 *
 * So a lost answer is asked for again, on its own budget, and is never confused
 * with a refusal — a refusal is an answer, and asking again gets the same one.
 * The classification is `atlas_err_is_transport`, stamped by the client layer
 * that held the file descriptor. It cannot travel the socket, so nothing a
 * daemon says, or quotes back from a repository, a task or a model, can produce
 * one; that is the whole difference between this and matching text.
 *
 * Bounded, for the reason the BUSY budget is: a loop with no end is a hang. When
 * it is spent the error is returned unchanged and the run stays ACTIVE and
 * resumable — the caller has lost this invocation, not the run. */
#define RUN_XPORT_TRIES 5
#define RUN_XPORT_PAUSE_MS 2000

static void nap(int64_t ms) {
    if (ms > 0) {
        struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
        (void)nanosleep(&ts, NULL);
    }
}

static int64_t xport_pause(const atlas_rundriver_opts *o) {
    return o->xport_pause_ms > 0 ? o->xport_pause_ms : RUN_XPORT_PAUSE_MS;
}

/* A11.5a-R. The completion gets its own, much longer budget, because losing one
 * is not the same kind of loss as losing a heartbeat.
 *
 * A refused heartbeat costs a renewal that the next one makes up, and an expired
 * lease is now deferred while Atlas is the thing refusing writes. A refused
 * completion costs the entire attempt: the exit classification, the gate
 * verdict and both logs, none of which can be recomputed, and it hands the work
 * to a second worker that will do it again. Measured on the run this was found
 * on: twelve tries spanned about seven seconds against a semantic pass that ran
 * for a hundred and seventy.
 *
 * So it retries for longer than a pass rather than longer than a stall, and it
 * is still bounded — a loop with no end is a hang, and the spooled result below
 * is what makes running out survivable rather than fatal. */
#define RUN_COMPLETE_BUSY_MS 300000
#define RUN_COMPLETE_PAUSE_MS 2000

/* A12.0. Is the daemon still waiting for this completion?
 *
 * Asked only once a delivery has failed in a way that does not say whether it
 * landed. A read that times out leaves the request queued and running — ipc.h
 * says so in as many words — so the write may well have happened with nobody
 * left to hear the outcome, and the completion is the one operation that cannot
 * simply be offered again: the token it carries is consumed by the delivery that
 * landed, and the redelivery is refused as an unknown one.
 *
 * The run is the place that says. A run driver's task is the run's active
 * repo-tree task, and `idx_orch_jobs_one_active_repo_tree` means there is at
 * most one; a run that no longer names this task holds a terminal record for it,
 * so nothing is owed and re-offering can only be refused. What it does *not*
 * establish is that this particular delivery is the one the daemon recorded —
 * recovery could have ended the attempt instead — which is exactly why the
 * caller keeps the spooled result rather than clearing it.
 *
 * Conservative in both directions it can be: a read that fails leaves the
 * completion owed, and so does a run that still names the task. One attempt, and
 * no retry of its own: the caller's loop is the budget. */
static bool completion_owed(const atlas_rundriver_opts *o, const char *job_uid,
                            atlas_orch_run_status *settled_out) {
    atlas_err ignore;
    atlas_err_init(&ignore);
    atlas_orch_run_view rv;
    memset(&rv, 0, sizeof(rv));
    bool found = false;
    bool owed = true;
    if (o->transport.run_get(o->transport.ud, o->run_uid, &rv, &found, &ignore) == ATLAS_OK &&
        found) {
        owed = strcmp(rv.active_job_uid, job_uid) == 0;
        if (!owed && settled_out != NULL) {
            *settled_out = rv.status;
        }
    }
    return owed;
}

/* The completion's own retry. Same shape, a budget measured in time rather than
 * in tries, and it says which one it is so an operator reading a long wait knows
 * Atlas is protecting a finished worker's result rather than hanging.
 *
 * `acked` is what the daemon itself answered, and it is not the same claim as
 * the status: a completion can be delivered without being acknowledged, which is
 * what the run check below settles and what keeps the spooled copy in place.
 *
 * The run check answers two failures, not one. A refusal is ordinarily final,
 * but a refusal that *follows* a lost answer is the daemon's account of the
 * delivery this driver already made — the token was consumed by it — and
 * mourning the result there would be the pilots' loss with one step in front. */
static atlas_status apply_completion(const atlas_rundriver_opts *o, const char *job_uid,
                                     const atlas_orch_op *op, atlas_orch_result *out, bool *acked,
                                     atlas_err *err) {
    int64_t budget = o->complete_busy_ms > 0 ? o->complete_busy_ms : RUN_COMPLETE_BUSY_MS;
    int64_t started = now_ms();
    int xport = 0;
    bool lost_one = false;
    atlas_status st = ATLAS_OK;
    *acked = false;
    for (;;) {
        st = o->transport.apply(o->transport.ud, op, out, err);
        if (st == ATLAS_OK) {
            *acked = true;
            break;
        }
        if (atlas_ipc_message_is_busy(err->msg)) {
            int64_t waited = now_ms() - started;
            if (waited >= budget) {
                break;
            }
            say(o, "the daemon is busy; the worker's result is spooled and will be offered again "
                   "(%lld s of %lld)", (long long)(waited / 1000), (long long)(budget / 1000));
            nap(RUN_COMPLETE_PAUSE_MS);
            atlas_err_init(err);
            continue;
        }
        bool lost = atlas_err_is_transport(err);
        if (!lost && !lost_one) {
            break; /* a refusal this driver never disturbed: an answer. */
        }
        atlas_orch_run_status settled = ATLAS_ORCH_RUN_UNKNOWN;
        if (!completion_owed(o, job_uid, &settled)) {
            say(o, "the answer to this completion did not arrive, and the run no longer holds "
                   "task %s open: the daemon has it. The spooled result is left in place, "
                   "because this delivery was never acknowledged.", job_uid);
            out->run_status = settled;
            atlas_err_init(err);
            st = ATLAS_OK;
            break;
        }
        if (!lost) {
            break; /* a refusal, and the task really is still waiting for one. */
        }
        lost_one = true;
        if (xport++ >= RUN_XPORT_TRIES) {
            break;
        }
        say(o, "the completion's answer did not arrive (%s); the worker's result is spooled and "
               "will be offered again (%d/%d)", atlas_err_msg(err), xport, RUN_XPORT_TRIES);
        nap(xport_pause(o));
        atlas_err_init(err);
    }
    return st;
}

/* One operation, asked again while the daemon says it took nothing and while the
 * answer does not arrive, and never once it has answered.
 *
 * Both retries are safe for every operation that reaches here, and for different
 * reasons. A lease is a compare-and-swap against `state = 'QUEUED'`, so a
 * re-request after a grant nobody heard is answered "not granted" rather than
 * granted twice — costing the invocation, not the run. A heartbeat that names
 * the phase the attempt is already in renews without transitioning, which is
 * what makes re-asking one uninteresting. The completion is the operation this
 * is not true of, and it has its own function above. */
static atlas_status apply_op(const atlas_rundriver_opts *o, const atlas_orch_op *op,
                             atlas_orch_result *out, atlas_err *err) {
    int busy = 0;
    int xport = 0;
    atlas_status st = ATLAS_OK;
    for (;;) {
        st = o->transport.apply(o->transport.ud, op, out, err);
        if (st == ATLAS_OK) {
            break;
        }
        int64_t pause = 0;
        if (atlas_ipc_message_is_busy(err->msg)) {
            if (busy++ >= RUN_BUSY_TRIES) {
                break;
            }
            say(o, "the daemon is busy and took nothing; retrying (%d/%d)", busy, RUN_BUSY_TRIES);
            pause = RUN_BUSY_PAUSE_MS;
        } else if (atlas_err_is_transport(err)) {
            if (xport++ >= RUN_XPORT_TRIES) {
                break;
            }
            say(o, "the answer did not arrive (%s); asking again (%d/%d)", atlas_err_msg(err),
                xport, RUN_XPORT_TRIES);
            pause = xport_pause(o);
        } else {
            break; /* an answer. Asking again would get the same one. */
        }
        nap(pause);
        atlas_err_init(err);
    }
    return st;
}

/* Moves the attempt one step along the pipeline. The transition table is the
 * authority on whether the step is legal; this only asks for it. */
static atlas_status phase(const atlas_rundriver_opts *o, claimed *c, atlas_orch_state to,
                          bool *cancel_out, atlas_err *err) {
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_HEARTBEAT);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    op->phase = to;
    atlas_status st = atlas_buf_set(&op->token, c->token.data, c->token.len, err);
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    if (st == ATLAS_OK) {
        st = apply_op(o, op, &r, err);
    }
    if (st == ATLAS_OK) {
        c->phase_now = to;
        c->last_heartbeat_ms = now_ms();
        if (cancel_out != NULL) {
            *cancel_out = r.cancel_requested;
        }
    }
    atlas_orch_result_free(&r);
    atlas_orch_op_free(op);
    free(op);
    return st;
}

/* Renews the lease if it is due, and answers whether the child must stop.
 *
 * Asked from inside `atlas_proc_run`'s wait loop, which is the only moment the
 * driver has while a child is running. This is dispatch.c's arrangement exactly:
 * a heartbeat that names the phase the attempt is already in renews the lease
 * without transitioning, so the same call does both jobs, and cancellation
 * arrives as the daemon's answer to it rather than as a signal — Atlas has no
 * path into the worker's process tree and must not grow one.
 *
 * A failed renewal does not stop the child. The attempt may already be lost, but
 * killing a worker mid-edit on the strength of one unanswered call would leave
 * the tree in a state nobody chose; the completion will find out and say so. */
static bool driver_should_stop(void *ud) {
    claimed *c = (claimed *)ud;
    if (c->cancelled) {
        return true;
    }
    int64_t at = now_ms();
    if (at - c->last_heartbeat_ms < ATLAS_ORCH_LEASE_MS / 4) {
        return false;
    }
    c->last_heartbeat_ms = at;
    atlas_err err;
    atlas_err_init(&err);
    bool cancel = false;
    if (phase(c->opts, c, c->phase_now, &cancel, &err) == ATLAS_OK && cancel) {
        c->cancelled = true;
    }
    return c->cancelled;
}

/* What the completion carries. Every field is either an Atlas classification or
 * a bounded excerpt of a program's output; none of it is a claim the worker
 * made about itself. */
typedef struct outcome {
    bool success;
    atlas_orch_exit_kind exit_kind;
    int64_t exit_code;
    atlas_orch_reason reason;
    int64_t failed_gate;
    atlas_buf detail;
    atlas_buf worker_log;
    atlas_buf driver_version;
    /* A10.0. Carried from the driver to the completion, and durable on disk
     * before the completion is offered. */
    atlas_usage usage;
} outcome;

static void outcome_init(outcome *x) {
    memset(x, 0, sizeof(*x));
    x->exit_kind = ATLAS_ORCH_EXIT_UNKNOWN;
    x->exit_code = -1;
    x->reason = ATLAS_ORCH_REASON_UNKNOWN;
    x->failed_gate = -1;
    atlas_buf_init(&x->detail);
    atlas_buf_init(&x->worker_log);
    atlas_buf_init(&x->driver_version);
}

static void outcome_free(outcome *x) {
    atlas_buf_free(&x->detail);
    atlas_buf_free(&x->worker_log);
    atlas_buf_free(&x->driver_version);
}

/* Reports the attempt and lets the daemon settle the run.
 *
 * The artifacts carried here are the worker's redacted log and, when a gate
 * failed, that gate's redacted output. They are evidence, stored so a restart
 * can read what happened; nothing reads them to decide anything. */
/* A11.5a-R. The finished worker's result, on disk, before anybody asks the
 * daemon to accept it.
 *
 * The problem this solves is narrow and was measured rather than imagined. A
 * repository-tree attempt has no workspace — that is the A11.1 reversal, the
 * work happens in the registered repository's own tree — so `store_log` has
 * nowhere to put the worker's streams and appends them to a buffer instead.
 * Everything the attempt produced therefore lived in one process's memory until
 * the completion was accepted, and the completion is an ordinary synchronous
 * write that A9.2.6 refuses for the whole of a semantic pass. When the driver
 * ran out of retries the run recorded a worker that had run for five minutes and
 * `orch_events` held nothing at all.
 *
 * The file is written whole under a temporary name and renamed into place, so a
 * reader never sees half of one. The identities are carried so a result can
 * never be read as belonging to a different attempt, and the digest covers the
 * bodies so a truncated file is detectable rather than plausible.
 *
 * **The lease token is not in it, and must never be.** A8's rule is that a token
 * is never stored, only a digest of it, and a bearer credential sitting in a
 * file is exactly what that rule exists to prevent. The consequence is
 * deliberate and is the honest limit of this change: a *different* process
 * cannot present this result, because it cannot prove it holds the attempt. What
 * the spool guarantees is that the result still exists to be looked at, not that
 * somebody else may deliver it. */
static atlas_status spool_path(const atlas_rundriver_opts *o, const claimed *c, atlas_buf *out,
                               atlas_err *err) {
    return atlas_buf_appendf(out, err, "%s/%s.%lld.result", o->spool_dir,
                             atlas_buf_cstr(&c->job_uid), (long long)c->attempt_no);
}

static atlas_status spool_write(const atlas_rundriver_opts *o, const claimed *c, const outcome *x,
                                atlas_err *err) {
    if (o->spool_dir == NULL || o->spool_dir[0] != '/') {
        return ATLAS_OK;
    }
    atlas_buf body = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_append(&body, x->worker_log.data, x->worker_log.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&body, x->detail.data, x->detail.len, err);
    }
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    if (st == ATLAS_OK) {
        atlas_sha256_hex(body.data != NULL ? body.data : "", body.len, hex);
    }

    atlas_buf doc = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&doc, err,
                               "atlas-orch-result-1\njob=%s\nrun=%s\nattempt=%lld\n"
                               "exit_kind=%s\nexit_code=%lld\nsuccess=%d\nreason=%s\n"
                               "failed_gate=%lld\ndriver_version=%s\n"
                               "worker_log_len=%zu\ngate_log_len=%zu\nsha256=%s\n--\n",
                               atlas_buf_cstr(&c->job_uid), o->run_uid != NULL ? o->run_uid : "",
                               (long long)c->attempt_no, atlas_orch_exit_kind_name(x->exit_kind),
                               (long long)x->exit_code, x->success ? 1 : 0,
                               atlas_orch_reason_name(x->reason), (long long)x->failed_gate,
                               atlas_buf_cstr(&x->driver_version), x->worker_log.len,
                               x->detail.len, hex);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&doc, body.data, body.len, err);
    }

    atlas_buf tmp = ATLAS_BUF_INIT, final = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = spool_path(o, c, &final, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&tmp, err, "%s.part", atlas_buf_cstr(&final));
    }
    if (st == ATLAS_OK) {
        FILE *f = fopen(atlas_buf_cstr(&tmp), "wxe");
        if (f == NULL && errno == EEXIST) {
            (void)unlink(atlas_buf_cstr(&tmp));
            f = fopen(atlas_buf_cstr(&tmp), "wxe");
        }
        if (f == NULL) {
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot open the result spool");
        } else {
            bool ok = doc.len == 0 || fwrite(doc.data, 1, doc.len, f) == doc.len;
            if (ok) {
                ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
            }
            if (fclose(f) != 0) {
                ok = false;
            }
            if (!ok) {
                (void)unlink(atlas_buf_cstr(&tmp));
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                         "cannot write the result spool");
            } else if (rename(atlas_buf_cstr(&tmp), atlas_buf_cstr(&final)) != 0) {
                (void)unlink(atlas_buf_cstr(&tmp));
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                         "cannot publish the result spool");
            }
        }
    }
    atlas_buf_free(&tmp);
    atlas_buf_free(&final);
    atlas_buf_free(&doc);
    atlas_buf_free(&body);
    return st;
}

/* A10.0. The cost summary, written beside the result and kept when the result
 * is not.
 *
 * The result spool is cleared once the daemon accepts a completion, and the
 * worker log that carries a second copy of these numbers is dropped whenever it
 * exceeds the inline artifact ceiling. Both of those are right, and together
 * they threw away every figure on exactly the path where a run *worked* — which
 * is the path an experiment compares. This file is small, bounded, and outlives
 * both.
 *
 * It holds numbers and checked names. Not the final record's `result` text, not
 * a session identifier, not a prompt, not a tool argument: a summary that
 * quietly retained model output would be a transcript with a shorter name, and
 * this one is meant to be kept. */
static atlas_status usage_write(const atlas_rundriver_opts *o, const claimed *c,
                                const outcome *x, atlas_err *err) {
    if (o->spool_dir == NULL || o->spool_dir[0] != '/') {
        return ATLAS_OK;
    }
    atlas_buf doc = ATLAS_BUF_INIT;
    atlas_status st = atlas_usage_encode(&x->usage, &doc, err);

    atlas_buf final = ATLAS_BUF_INIT, tmp = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&final, err, "%s/%s.%lld.usage", o->spool_dir,
                               atlas_buf_cstr(&c->job_uid), (long long)c->attempt_no);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&tmp, err, "%s.part", atlas_buf_cstr(&final));
    }
    if (st == ATLAS_OK) {
        FILE *f = fopen(atlas_buf_cstr(&tmp), "we");
        if (f == NULL) {
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot open the usage summary");
        } else {
            bool ok = doc.len == 0 || fwrite(doc.data, 1, doc.len, f) == doc.len;
            if (ok) {
                ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
            }
            if (fclose(f) != 0) {
                ok = false;
            }
            if (!ok || rename(atlas_buf_cstr(&tmp), atlas_buf_cstr(&final)) != 0) {
                (void)unlink(atlas_buf_cstr(&tmp));
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                         "cannot publish the usage summary");
            }
        }
    }
    atlas_buf_free(&tmp);
    atlas_buf_free(&final);
    atlas_buf_free(&doc);
    return st;
}

/* Dropped only once the daemon holds the result, because until then the file is
 * the only copy that survives this process. */
static void spool_clear(const atlas_rundriver_opts *o, const claimed *c) {
    if (o->spool_dir == NULL || o->spool_dir[0] != '/') {
        return;
    }
    atlas_err ignore;
    atlas_err_init(&ignore);
    atlas_buf path = ATLAS_BUF_INIT;
    if (spool_path(o, c, &path, &ignore) == ATLAS_OK) {
        (void)unlink(atlas_buf_cstr(&path));
    }
    atlas_buf_free(&path);
}

static atlas_status report(const atlas_rundriver_opts *o, const claimed *c, const outcome *x,
                           atlas_orch_result *res, atlas_err *err) {
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_COMPLETE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    op->success = x->success;
    op->exit_kind = x->exit_kind;
    op->exit_code = x->exit_code;
    op->failure_reason = x->reason;
    op->failed_gate = x->failed_gate;
    op->usage = x->usage;
    atlas_status st = atlas_buf_set(&op->token, c->token.data, c->token.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&op->driver_version, x->driver_version.data, x->driver_version.len,
                           err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&op->failure_detail, x->detail.data, x->detail.len, err);
    }

    atlas_orch_artifact arts[2];
    size_t na = 0;
    for (size_t i = 0; i < 2; i++) {
        atlas_orch_artifact_init(&arts[i]);
    }
    struct {
        const char *name;
        const atlas_buf *body;
    } want[2] = {{"worker.log", &x->worker_log}, {"gate.log", &x->detail}};
    for (size_t i = 0; st == ATLAS_OK && i < 2; i++) {
        if (want[i].body->len == 0 || want[i].body->len > ATLAS_ORCH_ARTIFACT_INLINE_MAX) {
            continue;
        }
        atlas_orch_artifact *a = &arts[na];
        st = atlas_buf_set_str(&a->name, want[i].name, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&a->kind, "log", err);
        }
        if (st == ATLAS_OK) {
            char hex[ATLAS_SHA256_HEX_LEN + 1u];
            atlas_sha256_hex(want[i].body->data, want[i].body->len, hex);
            st = atlas_buf_set_str(&a->sha256, hex, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&a->content, want[i].body->data, want[i].body->len, err);
        }
        if (st == ATLAS_OK) {
            a->size_bytes = (int64_t)want[i].body->len;
            a->content_stored = true;
            na++;
        }
    }
    if (st == ATLAS_OK && na > 0) {
        op->artifacts = arts;
        op->artifact_count = na;
    }
    /* Durable before it is offered. If every retry below is refused the file is
     * what is left, and it is the difference between "the daemon never took this
     * result" and "this result no longer exists anywhere". */
    if (st == ATLAS_OK) {
        atlas_status us = usage_write(o, c, x, err);
        if (us != ATLAS_OK) {
            say(o, "the attempt's usage summary could not be written: %s", atlas_err_msg(err));
            atlas_err_init(err);
        }
    }
    if (st == ATLAS_OK) {
        atlas_status ss = spool_write(o, c, x, err);
        if (ss != ATLAS_OK) {
            /* A spool that cannot be written is reported and does not stop the
             * completion: the in-memory result is still worth offering, and
             * refusing to try would turn a storage problem into a lost attempt. */
            say(o, "the worker's result could not be spooled: %s", atlas_err_msg(err));
            atlas_err_init(err);
        }
    }
    if (st == ATLAS_OK) {
        /* Cleared on the daemon's own acknowledgement and on nothing else. A
         * completion the run says was settled without this reply being heard is
         * delivered enough to stop offering and not enough to destroy the only
         * copy: whether the daemon recorded *this* result or recovery ended the
         * attempt some other way is precisely what a lost answer does not say. */
        bool acked = false;
        st = apply_completion(o, atlas_buf_cstr(&c->job_uid), op, res, &acked, err);
        if (st == ATLAS_OK && acked) {
            spool_clear(o, c);
        }
    }
    /* The artifacts are stack-owned here, so they are detached before the op is
     * freed rather than handed over. Freeing them twice is the alternative. */
    op->artifacts = NULL;
    op->artifact_count = 0;
    for (size_t i = 0; i < 2; i++) {
        atlas_orch_artifact_free(&arts[i]);
    }
    atlas_orch_op_free(op);
    free(op);
    return st;
}

/* Maps what the driver reported to the reason the ledger records. A closed
 * vocabulary in, a closed vocabulary out: there is no path by which a driver's
 * prose becomes a reason. */
static atlas_orch_reason reason_for(atlas_orch_exit_kind k) {
    switch (k) {
    case ATLAS_ORCH_EXIT_TIMEOUT: return ATLAS_ORCH_REASON_WALL_TIMEOUT;
    case ATLAS_ORCH_EXIT_CANCELLED: return ATLAS_ORCH_REASON_CANCEL_REQUESTED;
    case ATLAS_ORCH_EXIT_MALFORMED_RESULT: return ATLAS_ORCH_REASON_ENVELOPE_INVALID;
    case ATLAS_ORCH_EXIT_OK:
    case ATLAS_ORCH_EXIT_NONZERO:
    case ATLAS_ORCH_EXIT_SIGNALLED:
    case ATLAS_ORCH_EXIT_SPAWN_FAILED:
    case ATLAS_ORCH_EXIT_UNKNOWN: break;
    }
    return ATLAS_ORCH_REASON_WORKER_FAILURE;
}

/* Runs the task's gates in the repository root and folds the verdict into the
 * outcome. Only reached when the worker itself came back OK: a gate result over
 * a tree a crashed worker left half-edited establishes nothing. */
static atlas_status gate(const atlas_rundriver_opts *o, claimed *c, outcome *x,
                         atlas_err *err) {
    atlas_orch_argv cmds[ATLAS_ORCH_MAX_VALIDATIONS];
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_init(&cmds[i]);
    }
    size_t n = 0;
    atlas_status st = atlas_orch_validations_decode(atlas_buf_cstr(&c->validations), cmds,
                                                    ATLAS_ORCH_MAX_VALIDATIONS, &n, err);
    atlas_validation_result gr;
    atlas_validation_result_init(&gr);
    if (st == ATLAS_OK && n > 0) {
        atlas_validation_opts go;
        memset(&go, 0, sizeof(go));
        go.cwd = atlas_buf_cstr(&c->repo_root);
        go.wall_timeout_ms = c->wall_timeout_ms;
        go.idle_timeout_ms = c->idle_timeout_ms;
        go.max_output_bytes = c->max_output_bytes;
        /* Gates renew the lease too. `make test` is a gate an operator will
         * declare, and it outlives a lease as readily as the worker does. */
        go.cancel = driver_should_stop;
        go.cancel_ud = c;
        st = atlas_validations_run(cmds, n, &go, &gr, err);
        say(o, "gates: %zu declared, %zu ran, %s", n, gr.ran, gr.passed ? "passed" : "failed");
    }
    if (st == ATLAS_OK) {
        if (n == 0 || gr.passed) {
            x->success = true;
            x->reason = ATLAS_ORCH_REASON_WORKER_SUCCESS;
        } else {
            x->success = false;
            x->reason = ATLAS_ORCH_REASON_VALIDATION_FAILED;
            x->failed_gate = gr.failed_index;
            st = atlas_buf_set(&x->detail, gr.output.data, gr.output.len, err);
        }
    }
    atlas_validation_result_free(&gr);
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&cmds[i]);
    }
    return st;
}

/* Claims the run's active task, or reports that there was nothing to claim. */
static atlas_status claim(const atlas_rundriver_opts *o, const atlas_orch_run_view *rv,
                          claimed *c, bool *granted, atlas_err *err) {
    *granted = false;
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    if (op == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    atlas_status st = atlas_buf_set_str(&op->job_uid, rv->active_job_uid, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&op->dispatcher_id,
                               o->dispatcher_id != NULL ? o->dispatcher_id : "atlas-run", err);
    }
    /* Named explicitly, because a repo-tree driver is never granted to a lease
     * that did not ask for it. There is exactly one caller in Atlas that asks,
     * and this is it. */
    if (st == ATLAS_OK) {
        atlas_orch_argv want;
        atlas_orch_argv_init(&want);
        st = atlas_orch_argv_push(&want, "claude-repo", strlen("claude-repo"), err);
        if (st == ATLAS_OK) {
            st = atlas_orch_argv_push(&want, "fake-repo", strlen("fake-repo"), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_orch_validations_encode(&want, 1u, &op->lease_drivers, err);
        }
        atlas_orch_argv_free(&want);
    }

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    if (st == ATLAS_OK) {
        st = apply_op(o, op, &r, err);
    }
    if (st == ATLAS_OK && r.granted) {
        *granted = true;
        struct {
            atlas_buf *dst;
            const atlas_buf *src;
        } copy[] = {
            {&c->job_uid, &r.job_uid},   {&c->token, &r.token},
            {&c->repo_root, &r.repo_root}, {&c->commit, &r.source_commit},
            {&c->mode, &r.mode},         {&c->driver, &r.driver},
            {&c->task, &r.task_text},    {&c->validations, &r.validations},
            {&c->memory, &r.memory_package},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof copy / sizeof copy[0]; i++) {
            st = atlas_buf_set(copy[i].dst, copy[i].src->data, copy[i].src->len, err);
        }
        c->attempt_no = r.attempt_no;
        c->opts = o;
        c->phase_now = ATLAS_ORCH_STATE_LEASED;
        c->last_heartbeat_ms = now_ms();
        c->wall_timeout_ms = r.wall_timeout_ms;
        c->idle_timeout_ms = r.idle_timeout_ms;
        c->max_output_bytes = r.max_output_bytes;
    }
    atlas_orch_result_free(&r);
    atlas_orch_op_free(op);
    free(op);
    return st;
}

/* Refuses the claimed task without starting anything, and records why.
 *
 * `POLICY_REFUSED` is the reason, and the daemon reads it as non-retryable: a
 * repository that has moved off the pinned commit is not answered by trying
 * again or by a narrower task. */
static atlas_status refuse(const atlas_rundriver_opts *o, const claimed *c, const char *why,
                           atlas_orch_result *res, atlas_err *err) {
    outcome x;
    outcome_init(&x);
    x.success = false;
    x.exit_kind = ATLAS_ORCH_EXIT_SPAWN_FAILED;
    x.exit_code = -1;
    x.reason = ATLAS_ORCH_REASON_POLICY_REFUSED;
    atlas_status st = atlas_buf_set_str(&x.detail, why, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&x.driver_version, "atlas-run/1", err);
    }
    if (st == ATLAS_OK) {
        st = report(o, c, &x, res, err);
    }
    outcome_free(&x);
    return st;
}

/* The run, read with the same tolerance for an answer that did not arrive.
 *
 * A read changes nothing, so asking again is uninteresting except that not
 * asking again ends the invocation — which is the whole of A11.1's defect, and
 * this is the first call the driver makes. */
static atlas_status read_run(const atlas_rundriver_opts *o, atlas_orch_run_view *rv, bool *found,
                             atlas_err *err) {
    atlas_status st = ATLAS_OK;
    for (int i = 0;; i++) {
        st = o->transport.run_get(o->transport.ud, o->run_uid, rv, found, err);
        if (st == ATLAS_OK || !atlas_err_is_transport(err) || i >= RUN_XPORT_TRIES) {
            break;
        }
        say(o, "the run could not be read (%s); asking again (%d/%d)", atlas_err_msg(err), i + 1,
            RUN_XPORT_TRIES);
        nap(xport_pause(o));
        atlas_err_init(err);
    }
    return st;
}

/* One task, start to reported finish. */
static atlas_status drive_one(const atlas_rundriver_opts *o, const atlas_orch_run_view *rv,
                              atlas_rundriver_report *rep, bool *did_work, atlas_err *err) {
    *did_work = false;
    claimed c;
    claimed_init(&c);
    bool granted = false;
    atlas_status st = claim(o, rv, &c, &granted, err);
    if (st != ATLAS_OK || !granted) {
        if (st == ATLAS_OK) {
            /* Nothing was claimed. The run is untouched and resumable; this is
             * not a refusal of it and must never be reported as one. */
            rep->busy = true;
            say(o, "task %s is already held; nothing was started", rv->active_job_uid);
        }
        claimed_free(&c);
        return st;
    }
    st = atlas_buf_set(&rep->last_job_uid, c.job_uid.data, c.job_uid.len, err);
    if (st != ATLAS_OK) {
        claimed_free(&c);
        return st;
    }
    *did_work = true;
    rep->tasks++;

    atlas_orch_result res;
    atlas_orch_result_init(&res);
    atlas_buf live = ATLAS_BUF_INIT;
    /* A10.1. Declared with `live` rather than beside its use, so that every
     * `goto done` below releases it: the refusal paths jump from inside the
     * worker block, and a buffer declared there would be leaked by two of
     * them and indeterminate at the label for the rest. */
    atlas_buf composed = ATLAS_BUF_INIT;

    /* Step 3. Before anything is started. */
    st = phase(o, &c, ATLAS_ORCH_STATE_PREPARING, NULL, err);
    if (st == ATLAS_OK) {
        st = head_commit(atlas_buf_cstr(&c.repo_root), &live, err);
        if (st != ATLAS_OK) {
            atlas_err_init(err);
            st = refuse(o, &c, "the repository's HEAD could not be read", &res, err);
            goto done;
        }
        if (strcmp(atlas_buf_cstr(&live), atlas_buf_cstr(&c.commit)) != 0) {
            say(o, "HEAD has moved off the commit this task was pinned to; nothing was started");
            st = refuse(o, &c,
                        "the repository is no longer at the commit this run was pinned to",
                        &res, err);
            goto done;
        }
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    /* Step 4. Durable before the worker exists. */
    st = phase(o, &c, ATLAS_ORCH_STATE_RUNNING, NULL, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    /* A10.1. The one place a memory package is ever injected, and it happens
     * before the worker exists.
     *
     * The task comes first and stays first: the operator's words, the
     * repository's own instructions and every safety bound sit above the memory
     * section, and the section's own preamble says so. `atlas_orch_memory_compose`
     * appends the package once and appends nothing at all when it is empty,
     * which is what makes a memory-off arm differ from a memory-on one by
     * exactly the package's bytes and by nothing else.
     *
     * The composition is not stored. `orch_jobs.task_text` stays what was
     * submitted, so `spec_digest` still describes the request that was made,
     * and the package stays where a reader looks for it -- on the run. */
    st = atlas_orch_memory_compose(atlas_buf_cstr(&c.task), atlas_buf_cstr(&c.memory), &composed,
                                   err);
    if (st != ATLAS_OK) {
        goto done;
    }
    if (c.memory.len > 0) {
        say(o, "the run's frozen memory package was appended to the task: %zu bytes",
            c.memory.len);
    }

    outcome x;
    outcome_init(&x);
    {
        const atlas_driver *d = atlas_driver_find(atlas_buf_cstr(&c.driver));
        if (d == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "this job names a driver Atlas does not ship");
        } else {
            atlas_driver_req req;
            memset(&req, 0, sizeof(req));
            /* No workspace. The tree is the registered repository's own, which
             * is the reversal, and the log has nowhere to go but back to us. */
            req.ws = NULL;
            req.work_dir = atlas_buf_cstr(&c.repo_root);
            req.job_uid = atlas_buf_cstr(&c.job_uid);
            req.attempt_no = c.attempt_no;
            /* Same directory the finished result is spooled to, so one attempt's
             * evidence lives in one place and is named the same way. */
            req.progress_dir = o->spool_dir;
            req.task = atlas_buf_cstr(&composed);
            req.mode = atlas_buf_cstr(&c.mode);
            req.wall_timeout_ms = c.wall_timeout_ms;
            req.idle_timeout_ms = c.idle_timeout_ms;
            req.max_output_bytes = c.max_output_bytes;
            req.live_model = o->live_model;
            req.operator_session = o->operator_session;
            /* Polled while the child runs: it renews the lease and is how the
             * child learns of a cancellation. Without it a worker that takes
             * longer than one lease loses the attempt underneath itself. */
            req.cancel = driver_should_stop;
            req.cancel_ud = &c;

            atlas_driver_res dr;
            atlas_driver_res_init(&dr);
            say(o, "starting one worker for task %s attempt %lld", atlas_buf_cstr(&c.job_uid),
                (long long)c.attempt_no);
            atlas_status ds = d->run(&req, &dr, err);
            x.exit_kind = dr.exit_kind;
            x.exit_code = dr.exit_code;
            if (ds != ATLAS_OK && dr.exit_kind == ATLAS_ORCH_EXIT_UNKNOWN) {
                x.exit_kind = ATLAS_ORCH_EXIT_SPAWN_FAILED;
                atlas_err_init(err);
            }
            (void)atlas_buf_set(&x.worker_log, dr.log.data, dr.log.len, err);
            (void)atlas_buf_set(&x.driver_version, dr.version.data, dr.version.len, err);
            x.usage = dr.usage;
            atlas_driver_res_free(&dr);
        }
    }

    if (st == ATLAS_OK && x.exit_kind == ATLAS_ORCH_EXIT_OK) {
        /* Step 6. A worker that moved HEAD has invalidated every gate. */
        atlas_buf after = ATLAS_BUF_INIT;
        atlas_status hs = head_commit(atlas_buf_cstr(&c.repo_root), &after, err);
        bool same = hs == ATLAS_OK && strcmp(atlas_buf_cstr(&after), atlas_buf_cstr(&c.commit)) == 0;
        atlas_buf_free(&after);
        if (!same) {
            atlas_err_init(err);
            say(o, "the worker moved HEAD; the gates were not run");
            outcome_free(&x);
            st = refuse(o, &c, "the worker moved the repository off its pinned commit", &res, err);
            goto done;
        }
        st = phase(o, &c, ATLAS_ORCH_STATE_VALIDATING, NULL, err);
        if (st == ATLAS_OK) {
            st = gate(o, &c, &x, err);
        }
    } else if (st == ATLAS_OK) {
        x.success = false;
        x.reason = reason_for(x.exit_kind);
    }

    if (st == ATLAS_OK) {
        st = report(o, &c, &x, &res, err);
    }
    outcome_free(&x);

done:
    if (st == ATLAS_OK) {
        /* A12.0. A completion whose answer was lost fills neither, because the
         * reply that carries them never arrived; the run check recovers the
         * status and nothing else. Neither is overwritten with the value that
         * means "nobody said": the loop above read the status from the run
         * before this task started, and a count from an earlier task in the
         * chain is a fact where a zero here would be a claim. */
        if (res.run_status != ATLAS_ORCH_RUN_UNKNOWN) {
            rep->status = res.run_status;
        }
        if (res.worker_starts > 0) {
            rep->worker_starts = res.worker_starts;
        }
        if (res.follow_up_job_uid.len > 0) {
            say(o, "one follow-up task was created: %s", atlas_buf_cstr(&res.follow_up_job_uid));
        }
    }
    atlas_buf_free(&live);
    atlas_buf_free(&composed);
    atlas_orch_result_free(&res);
    claimed_free(&c);
    return st;
}

atlas_status atlas_rundriver_run(const atlas_rundriver_opts *o, atlas_rundriver_report *rep,
                                 atlas_err *err) {
    if (o->run_uid == NULL || o->run_uid[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no run was named");
    }
    if (o->transport.apply == NULL || o->transport.run_get == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the run driver has no transport");
    }
    atlas_status st = atlas_buf_set_str(&rep->run_uid, o->run_uid, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* An upper bound on iterations that cannot be reached in a healthy run: the
     * worker-start budget is the real limit and the daemon enforces it. This
     * exists so that a defect somewhere below cannot become an unbounded loop,
     * which is the one failure mode a driver must not have. */
    int64_t ceiling = o->max_tasks > 0 ? o->max_tasks : ATLAS_ORCH_RUN_MAX_WORKER_STARTS + 2;
    for (int64_t i = 0; i < ceiling; i++) {
        atlas_orch_run_view rv;
        memset(&rv, 0, sizeof(rv));
        bool found = false;
        st = read_run(o, &rv, &found, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (!found) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "no run named %s exists", o->run_uid);
        }
        rep->status = rv.status;
        if (atlas_orch_run_status_is_terminal(rv.status)) {
            /* Touched not at all. A terminal run takes no task, no attempt and
             * no worker, and saying so is the whole of what happens here. */
            say(o, "run %s already ended in %s", o->run_uid,
                atlas_orch_run_status_name(rv.status));
            return ATLAS_OK;
        }
        if (rv.active_job_uid[0] == '\0') {
            /* ACTIVE with nothing for *this* driver to do. Left exactly as it
             * is: it is resumable, and inventing a task for it here would be
             * this milestone deciding something it was not asked to decide.
             *
             * A11.6 makes that two different situations, and they are said
             * differently because an operator would act differently on them. A
             * run with nothing active is between tasks. A run whose repo-tree
             * chain is finished while workspace siblings are still going has
             * work in flight that this driver cannot claim — the run will settle
             * when they end, and running this command again now would still find
             * nothing. */
            if (rv.active_count > 0) {
                say(o, "run %s has no task in the repository's tree to claim; %lld other "
                       "task(s) are still running and the run settles when they end",
                    o->run_uid, (long long)rv.active_count);
            } else {
                say(o, "run %s is active with no task to claim", o->run_uid);
            }
            return ATLAS_OK;
        }
        bool did = false;
        st = drive_one(o, &rv, rep, &did, err);
        if (st != ATLAS_OK || !did) {
            return st;
        }
    }
    return ATLAS_OK;
}

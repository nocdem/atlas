/* Atlas - A11.1: the operator's foreground run driver.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## What this is
 *
 * One loop, started deliberately by an operator, that drives one run to a
 * settled answer: claim the run's active task, start one worker in the
 * registered repository's own tree, run the gates the task declared, report what
 * happened, and let the daemon decide what the run is. It is the trusted
 * component the season's authority question is answered with — not because it
 * says so, but because of what it does and does not carry.
 *
 * ## What it does not do
 *
 * It runs **no background loop**, polls **no queue**, and starts **nothing**
 * that the operator did not name. There is no scheduler here, no provider
 * router, and no second submit path: a follow-up task is created by the daemon
 * inside the completion that produced it, through `atlas_orch_apply_in_tx`, the
 * same write point every orchestration row goes through.
 *
 * It does not decide whether the run was accepted. It reports an exit
 * classification and a gate verdict, both of which **Atlas computed**, and the
 * daemon derives the run's status from those and from the ledger. There is no
 * field on the wire that says ACCEPTED, and a worker could not fill one in if
 * there were: the worker's stdout, its result document and its exit code are
 * evidence about a process and are read by nothing that decides.
 *
 * ## What it deliberately reverses
 *
 * A8's worker runs on a snapshot in an isolated workspace and Atlas applies
 * nothing it produces. A11.1's runs in the registered repository's own tree,
 * because a chain of tasks that build on each other's changes cannot be built
 * out of workers who cannot see them. The reversal is scoped to
 * `atlas_orch_driver_is_repo_tree`'s two drivers and to this command; Atlas'
 * own reads — scan, the index passes, every `src/git` invocation — are unchanged
 * and still read-only. `docs/engineering-rules.md` carries the argument.
 *
 * The consequence an operator must know is that **the working tree is expected
 * to be dirty afterwards**. That is the first worker's output and the second
 * worker's input. Nothing here cleans, resets, checks out, stashes or reverts
 * anything, ever, on any path including every failure path.
 */
#ifndef ATLAS_RUNDRIVER_H
#define ATLAS_RUNDRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"

/* How the driver reaches the one write point.
 *
 * In the shipped binary this is the socket, because orchestration state lives in
 * the index and `atlasd` is its only writer — a driver that opened the database
 * itself would be a second writer, which A1 forbids and A7.1 makes impossible.
 * A test hosts the same loop against a fixture database handle, which is what
 * lets the acceptance contracts be proved in milliseconds without a daemon, a
 * model or a network.
 *
 * It loosens no production check. The socket transport is the only one the
 * shipped binary constructs; what the IPC edge refuses — the peer uid, the
 * orchestration policy, the method group — it still refuses, and
 * `tests/test_orch_rpc.c` is what proves that. What this abstracts is the
 * *carriage* of an operation, not its validation. */
typedef struct atlas_rundriver_transport {
    atlas_status (*apply)(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                          atlas_err *err);
    atlas_status (*run_get)(void *ud, const char *run_uid, atlas_orch_run_view *out, bool *found,
                            atlas_err *err);
    void *ud;
} atlas_rundriver_transport;

typedef struct atlas_rundriver_opts {
    /* The chain to drive. Always a run: a caller that has just submitted a root
     * task got the run back from the submission, and a caller resuming was
     * given one. There is no form that takes a job, because "drive this task"
     * is not a thing an operator can safely mean — the run decides which task
     * is next. */
    const char *run_uid;
    /* Recorded on each attempt so a restart is visible in the history. Not an
     * authorisation; nothing is decided from it. */
    const char *dispatcher_id;
    /* Whether a driver that calls a live model may run, and whether it uses the
     * invoking account's own logged-in session. Both come from the root-owned
     * orchestration policy and neither is inferred from the process's uid. */
    bool live_model;
    bool operator_session;
    /* Stop after this many claimed tasks. Zero means "until the run settles",
     * which is what the command does; a positive value is how a test drives
     * exactly one task without racing anything. The run's own worker-start
     * budget applies regardless and is the bound that matters. */
    int64_t max_tasks;
    /* A11.5a-R. Where a finished worker's result is made durable *before* the
     * completion is offered to the daemon, or NULL to keep the old behaviour of
     * holding it only in memory.
     *
     * The completion is an ordinary synchronous write, so A9.2.6 refuses it for
     * the whole of an unbounded semantic pass — and until this existed, a driver
     * that ran out of retries lost the exit classification, the gate verdict and
     * both logs together, because a repository-tree attempt has no workspace to
     * have written them to. Measured: attempt 1 of the run this was found on
     * left `orch_events` empty for a worker that had run for five minutes.
     *
     * Absolute, chosen by Atlas from the root-owned policy's model worker root,
     * and never from the task text, the environment or anything the model
     * produced. A test passes its fixture's own directory. */
    const char *spool_dir;
    /* How long a refused completion keeps being offered, in milliseconds, or
     * zero for `RUN_COMPLETE_BUSY_MS`. Set by the caller that constructs this
     * struct — the service layer in production, a fixture in a test — and never
     * reachable from task text, a model payload or the environment, which is the
     * same rule `max_tasks` follows and for the same reason. */
    int64_t complete_busy_ms;
    FILE *log;
    atlas_rundriver_transport transport;
} atlas_rundriver_opts;

typedef struct atlas_rundriver_report {
    atlas_buf run_uid;
    atlas_orch_run_status status;
    /* Worker starts this run has spent, as the daemon counted them. */
    int64_t worker_starts;
    /* Tasks this invocation claimed and carried. Zero is an ordinary answer. */
    int64_t tasks;
    atlas_buf last_job_uid;
    /* Nothing was claimed because the run's active task is already held.
     *
     * This is neither an acceptance nor a refusal, and it is emphatically not
     * BLOCKED: the run stays ACTIVE and resumable, nothing was written, and the
     * same invocation may simply be repeated. Reporting it as a failure of the
     * run is the mistake this field exists to prevent. */
    bool busy;
} atlas_rundriver_report;

void atlas_rundriver_report_init(atlas_rundriver_report *r);
void atlas_rundriver_report_free(atlas_rundriver_report *r);

/* Drives the run until it settles, until `max_tasks` is reached, or until there
 * is nothing to claim. Returns non-OK only for a failure of Atlas itself; a run
 * that ended BLOCKED is an answer and is reported in `rep->status`.
 *
 * A terminal run is touched not at all: no lease is asked for, no task is
 * created, and the report says what the run already was. */
atlas_status atlas_rundriver_run(const atlas_rundriver_opts *o, atlas_rundriver_report *rep,
                                 atlas_err *err);

#endif /* ATLAS_RUNDRIVER_H */

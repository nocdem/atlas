/* Atlas - A8: the dispatcher process.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One loop, running as `atlas-worker`, that asks `atlasd` for work over the
 * A7.1 socket and carries it out inside an isolated workspace. It holds no
 * database handle, opens no repository for writing, and decides nothing about
 * the job beyond how to execute what it was handed.
 *
 * See `docs/orchestration.md` for the whole model and `src/orch/dispatch.c` for
 * what the loop refuses to do.
 */
#ifndef ATLAS_DISPATCH_H
#define ATLAS_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/error.h"

typedef struct atlas_dispatch_opts {
    /* Where the daemon listens. Taken from the root-owned system policy, never
     * from the environment, so a worker cannot be pointed at another socket. */
    const char *socket_path;
    /* Where this dispatcher owns its workspaces. From the root-owned
     * orchestration policy, and validated before the first connection. */
    const char *worker_root;
    /* A name for this dispatcher process, recorded on each attempt so a restart
     * is visible in the history. It is not an authorisation. */
    const char *dispatcher_id;

    /* How long to wait when the queue is empty. */
    int64_t poll_ms;
    /* The ceiling on reconnect backoff when the daemon is unreachable. */
    int64_t max_backoff_ms;
    /* How often a running attempt renews its lease. Must be comfortably below
     * the lease lifetime, or a healthy job loses its lease while working. */
    int64_t heartbeat_ms;

    /* Whether a driver that calls a live model may run. From the policy. */
    bool live_model;
    /* Keep a successful attempt's workspace instead of removing it. Failed
     * attempts are always kept as evidence. */
    bool keep_workspaces;

    /* Stop after this many loop iterations. Zero runs forever, which is what
     * the service does; a positive value is how a test drives exactly one
     * attempt without a background process. */
    int64_t max_iterations;

    FILE *log;
} atlas_dispatch_opts;

/* Runs until SIGTERM, SIGINT or the iteration bound. Returns ATLAS_OK on a
 * graceful stop; a failure to reach the daemon is not a failure of the loop. */
atlas_status atlas_dispatch_run(const atlas_dispatch_opts *o, atlas_err *err);

#endif /* ATLAS_DISPATCH_H */

/* Atlas - A8: the versioned driver interface, and what a driver may not decide.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A driver is the only part of A8 that runs somebody else's code. Everything
 * about this interface is shaped by that.
 *
 * ## What a driver is given
 *
 * A workspace, a task text, and bounds. That is all. It does not receive the
 * lease token, the job's database identity, the repository path, the pinned
 * commit, the retry limit or the policy — because it must not be able to change
 * any of them, and the simplest way to guarantee that is not to hand them over.
 *
 * ## What a driver may decide
 *
 * What to write inside `work/` and `artifacts/`, and what to say on its own
 * streams. Nothing else. In particular a driver does **not** decide:
 *
 *   * whether the job succeeded — Atlas classifies its exit and validates the
 *     result envelope, and a driver that exits zero while producing malformed
 *     metadata is `MALFORMED_RESULT`, not success;
 *   * which files changed — Atlas compares the pristine snapshot against the
 *     work tree, because a driver reporting its own changes is a driver
 *     describing itself;
 *   * when it may stop heartbeating — heartbeats are emitted by the dispatcher
 *     on a timer, independently of whether the driver has produced any output.
 *
 * **Natural-language output is never parsed as authority.** A driver's prose is
 * UNTRUSTED_DATA, is stored as a log, and no branch anywhere reads it.
 */
#ifndef ATLAS_DRIVER_H
#define ATLAS_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/orch.h"
#include "atlas/proc.h"
#include "atlas/workspace.h"

/* Bounds on what a driver may say about itself. */
#define ATLAS_DRIVER_VERSION_MAX 128u

typedef struct atlas_driver_req {
    const atlas_ws *ws;
    const char *job_uid;
    int64_t attempt_no;
    /* UNTRUSTED_DATA: what a submitter typed. Handed to the driver because the
     * driver is what it is for; never interpreted by Atlas. */
    const char *task;
    const char *mode;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_output_bytes;
    /* Polled while the driver runs. A cancellation arrives over the socket at a
     * heartbeat, so the worker is the only thing that can stop its own child. */
    atlas_proc_cancel_fn cancel;
    void *cancel_ud;
    /* Whether the orchestration policy permits a driver that calls a live
     * model. A driver that needs it and does not have it refuses to run rather
     * than degrading to something else. */
    bool live_model;
    /* A8.1. When set, the model driver runs under the dispatcher's own logged-in
     * session: it inherits that account's HOME and uses whatever credentials
     * already live there. Atlas never reads, copies, prints or stores any of
     * them — it sets HOME and executes, and the CLI authenticates itself.
     *
     * This is only ever true in the operator's own model dispatcher, which the
     * root-owned policy has to name explicitly. */
    bool operator_session;
} atlas_driver_req;

typedef struct atlas_driver_res {
    atlas_orch_exit_kind exit_kind;
    int64_t exit_code;
    atlas_buf version; /* "<name>/<version>", recorded on the attempt */
    int64_t stdout_bytes;
    int64_t stderr_bytes;
    /* How many credential-shaped runs were removed from the captured logs.
     * Reported so an operator can see redaction happened rather than assume it. */
    int64_t redactions;
    /* Model metadata when the driver can report it, and zero when it cannot.
     * Zero means "not reported", never "free". */
    int64_t input_tokens;
    int64_t output_tokens;
    atlas_buf cost;
} atlas_driver_res;

void atlas_driver_res_init(atlas_driver_res *r);
void atlas_driver_res_free(atlas_driver_res *r);

typedef struct atlas_driver {
    const char *name;
    /* Bumped whenever the driver would produce different work from identical
     * inputs. Recorded on every attempt, for the reason A3 records the analyzer
     * version: a result is only comparable to another when the thing that
     * produced it is named. */
    const char *version;
    /* True when this driver calls a live model, and so needs `live_model` in
     * the policy. */
    bool needs_live_model;
    atlas_status (*run)(const atlas_driver_req *req, atlas_driver_res *res, atlas_err *err);
} atlas_driver;

/* Resolves a driver by name. NULL for an unknown name — an unknown driver is a
 * refusal, never a default: silently substituting one would run something other
 * than what the job specified. */
const atlas_driver *atlas_driver_find(const char *name);

/* Every driver Atlas ships, for the policy check and for `atlas dispatcher
 * drivers`. */
const atlas_driver *const *atlas_drivers(size_t *count_out);

#endif /* ATLAS_DRIVER_H */

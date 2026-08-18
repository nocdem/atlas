/* Atlas - A8/A11.1: running a job's declared verification commands.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One implementation of "run the gates this job declared and say what happened".
 *
 * A8 ran them in the dispatcher, inside a workspace. A11.1 runs them in the run
 * driver, in the registered repository's own tree. The two differ in *where*
 * and in *what they do with the output*, and in nothing else — so the argv
 * construction, the program allowlist, the constructed environment, the bounds
 * and the pass/fail rule live here, once. Two copies would be two places for a
 * gate to become weaker, and a gate that is weaker in one caller than the other
 * is the failure the whole arrangement exists to prevent.
 *
 * **A gate is never a shell string.** `atlas_proc_run` execve's an argument
 * vector, and a validation command is a vector of counted arguments all the way
 * from the job specification. `argv[0]` is resolved against a fixed allowlist
 * compiled into the binary rather than against `PATH`, so a job cannot name an
 * arbitrary program and a `PATH` something planted cannot select one.
 *
 * **A gate is never chosen by a worker.** What runs comes from the job row,
 * which was written at submission and is inherited verbatim by every follow-up
 * task. Nothing a driver produces reaches this file.
 */
#ifndef ATLAS_VALIDATE_H
#define ATLAS_VALIDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"
#include "atlas/orch.h"
#include "atlas/proc.h"

/* True when `name` may be `argv[0]` of a validation command.
 *
 * Deliberately tiny, and deliberately not containing a shell. A deployment that
 * needs another program adds it here, in the binary, rather than in a job —
 * which is the difference between an operator's decision and a submitter's. */
bool atlas_validation_program_allowed(const char *name);

/* Receives one command's redacted output, whatever the outcome. A failed gate's
 * output is the only account of why it failed, so it is stored before the run
 * is abandoned rather than after. Returning non-OK abandons the run. */
typedef atlas_status (*atlas_validation_log_fn)(size_t index, const atlas_buf *redacted, void *ud,
                                          atlas_err *err);

typedef struct atlas_validation_opts {
    /* Absolute, and resolved by Atlas: a workspace's writable tree, or the
     * registry's canonical repository root. Never from a task text. */
    const char *cwd;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_output_bytes;
    atlas_proc_cancel_fn cancel;
    void *cancel_ud;
    atlas_validation_log_fn log;
    void *log_ud;
} atlas_validation_opts;

typedef struct atlas_validation_result {
    bool passed;
    /* Which command failed, zero-based, or -1 when none did. This index is what
     * a completion carries: the *name* of the gate is rendered later from the
     * job's own stored list, so a caller cannot name a gate the job never
     * declared. */
    int64_t failed_index;
    /* The failing command's redacted output, bounded. UNTRUSTED_DATA. */
    atlas_buf output;
    /* How many commands actually ran. Reported rather than assumed: a run that
     * stopped at the second of five gates has not established anything about
     * the other three. */
    size_t ran;
} atlas_validation_result;

void atlas_validation_result_init(atlas_validation_result *r);
void atlas_validation_result_free(atlas_validation_result *r);

/* Runs `n` commands in order, stopping at the first failure.
 *
 * A command fails when it could not be spawned, when its program is not on the
 * allowlist, when it exited non-zero, or when it hit either bound. A zero exit
 * from every command is the only thing that sets `passed`.
 *
 * Returns non-OK only for a failure of Atlas itself — an allocation, an encoding
 * that will not decode. A gate that simply failed is `passed == false` with
 * ATLAS_OK, because a failing gate is an answer and not an error. */
atlas_status atlas_validations_run(const atlas_orch_argv *cmds, size_t n, const atlas_validation_opts *o,
                             atlas_validation_result *out, atlas_err *err);

#endif /* ATLAS_VALIDATE_H */

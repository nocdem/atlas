/* Atlas - safe subprocess execution.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Hard rules enforced here (see docs/git-safety.md):
 *   - No shell. Ever. Programs are executed with execve() and an explicit
 *     argument vector; there is no system(), popen(), or "/bin/sh -c" path.
 *   - The environment handed to the child is constructed explicitly; the
 *     parent's environment is never inherited implicitly.
 *   - stdin is always /dev/null, so a child can never block on a prompt.
 *   - The child is placed in its own process group and the whole group is
 *     terminated on timeout, so grandchildren cannot outlive the call.
 *   - stdout is streamed to a sink with a hard byte ceiling; stderr is captured
 *     into a bounded buffer.
 */
#ifndef ATLAS_PROC_H
#define ATLAS_PROC_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Receives stdout in arbitrary-sized chunks. Returning non-OK aborts the run
 * and terminates the child. */
typedef atlas_status (*atlas_proc_sink)(const char *chunk, size_t n, void *ud, atlas_err *err);

/* Polled while a child runs. Returning true asks for the child to stop.
 *
 * A8 needs this because cancellation arrives over the socket at a heartbeat, not
 * as a signal: the daemon has no path into the worker's process tree, by design,
 * so the worker is the only thing that can stop its own child. */
typedef bool (*atlas_proc_cancel_fn)(void *ud);

typedef struct atlas_proc_opts {
    const char *const *argv; /* NULL-terminated; argv[0] is the program name */
    const char *const *env;  /* NULL-terminated "K=V" list; NULL means empty env */
    int timeout_ms;          /* <= 0 means ATLAS_PROC_DEFAULT_TIMEOUT_MS */
    size_t max_stdout;       /* hard ceiling on stdout bytes; 0 means default */
    size_t max_stderr;       /* hard ceiling on captured stderr; 0 means default */

    /* A8 additions. All optional: a zeroed opts behaves exactly as before, which
     * is what keeps every existing caller — every git invocation — unchanged.
     *
     * `idle_timeout_ms` is time since the child last produced *any* output on
     * either stream. It is a separate bound from the wall clock because the two
     * catch different failures: a wedged process that prints nothing, and a
     * chatty process that never finishes. Whichever fires first wins. */
    int idle_timeout_ms;
    /* Polled at least every ATLAS_PROC_CANCEL_POLL_MS while the child runs. */
    atlas_proc_cancel_fn cancel;
    void *cancel_ud;
    /* Milliseconds between SIGTERM and SIGKILL for a cancelled or timed-out
     * child. 0 takes ATLAS_PROC_KILL_GRACE_MS. Both signals go to the whole
     * process group, so grandchildren cannot outlive the call. */
    int grace_ms;
    /* Absolute directory the child starts in. NULL keeps Atlas' own working
     * directory, which is what every pre-A8 caller wants — Atlas never chdirs
     * itself and addresses repositories with `git -C`.
     *
     * A8 needs it because a job command runs *in* its workspace: the working
     * directory is part of the isolation, not a convenience. It is applied in
     * the child after the fork and before execve, and a failure is reported
     * through the same status pipe an exec failure uses, so a command whose
     * directory is missing never runs somewhere else instead. */
    const char *cwd;
    /* A8-CI addition. Bytes of address space the child may map, applied with
     * RLIMIT_AS after the fork and before the execve. 0 means no ceiling, which
     * is what every earlier caller passes and gets.
     *
     * This exists because the semantic indexer hands untrusted repository source
     * to a compiler front end, and a front end holds a whole translation unit's
     * AST in memory. The bound is enforced by the kernel — the child's
     * allocation fails and it dies — rather than by asking the child to behave,
     * which is the same reason A7.1 prefers a filesystem guarantee to a check in
     * C. */
    unsigned long long max_address_space;
} atlas_proc_opts;

#define ATLAS_PROC_DEFAULT_TIMEOUT_MS 60000
#define ATLAS_PROC_DEFAULT_MAX_STDOUT (256u * 1024u * 1024u)
#define ATLAS_PROC_DEFAULT_MAX_STDERR (64u * 1024u)
/* How often a cancel callback is consulted while a child runs. */
#define ATLAS_PROC_CANCEL_POLL_MS 100

typedef struct atlas_proc_result {
    int exit_code;          /* -1 when the child did not exit normally */
    int term_signal;        /* signal that killed the child, else 0 */
    bool timed_out;         /* the wall-clock bound fired */
    /* Kept apart from `timed_out` on purpose: "it ran too long" and "it went
     * quiet" are different failures and an operator acts differently on them. */
    bool idle_timed_out;
    /* The cancel callback asked for a stop. Distinct from a timeout because a
     * cancelled job is not a failed one. */
    bool cancelled;
    bool stdout_truncated;  /* max_stdout was reached; child was terminated */
    size_t stdout_bytes;
} atlas_proc_result;

/* Resolve `program` against PATH (or accept it as-is when it contains '/').
 * Writes an absolute executable path into `out`. */
atlas_status atlas_proc_which(const char *program, const char *path_env, atlas_buf *out,
                              atlas_err *err);

/* Run to completion. `sink` may be NULL to discard stdout. `stderr_out` may be
 * NULL. `res` may be NULL. A non-zero child exit status is *not* an error here;
 * it is reported through `res`, and callers decide. */
atlas_status atlas_proc_run(const atlas_proc_opts *opts, atlas_proc_sink sink, void *sink_ud,
                            atlas_buf *stderr_out, atlas_proc_result *res, atlas_err *err);

/* Convenience sink: appends into an atlas_buf passed as `ud`. */
atlas_status atlas_proc_sink_buf(const char *chunk, size_t n, void *ud, atlas_err *err);

#endif /* ATLAS_PROC_H */

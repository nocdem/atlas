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

typedef struct atlas_proc_opts {
    const char *const *argv; /* NULL-terminated; argv[0] is the program name */
    const char *const *env;  /* NULL-terminated "K=V" list; NULL means empty env */
    int timeout_ms;          /* <= 0 means ATLAS_PROC_DEFAULT_TIMEOUT_MS */
    size_t max_stdout;       /* hard ceiling on stdout bytes; 0 means default */
    size_t max_stderr;       /* hard ceiling on captured stderr; 0 means default */
} atlas_proc_opts;

#define ATLAS_PROC_DEFAULT_TIMEOUT_MS 60000
#define ATLAS_PROC_DEFAULT_MAX_STDOUT (256u * 1024u * 1024u)
#define ATLAS_PROC_DEFAULT_MAX_STDERR (64u * 1024u)

typedef struct atlas_proc_result {
    int exit_code;          /* -1 when the child did not exit normally */
    int term_signal;        /* signal that killed the child, else 0 */
    bool timed_out;
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

/* Atlas - a bounded worker pool for filesystem metadata and hashing.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Hashing is the only part of a reconciliation pass that is worth parallelising:
 * it is CPU- and I/O-bound per file and completely independent between files.
 * Everything else — deciding what to hash, and writing the result — is
 * deliberately serial, because that is what makes the single-writer model
 * provable rather than hopeful.
 *
 * The pool is therefore a parallel-for and nothing more:
 *
 *     atlas_workers_for_each(w, n, fn, ud, err)
 *
 * runs fn(i, ud) for every i in [0, n) across at most `count` threads and
 * returns only when every one of them has finished. There is no result queue, no
 * future, and no callback into the caller from a worker thread: the caller's
 * data structure is indexed by `i`, so each job writes to its own slot and no
 * lock is needed for the results themselves.
 *
 * Consequences that matter:
 *   - a job never touches SQLite. The write connection belongs to one thread,
 *     and a read connection is not shared across threads either.
 *   - a job never creates a process. Git runtime state is frozen before the
 *     pool starts (atlas_git_runtime_init), and git invocations happen on the
 *     serial part of the pass.
 *   - because the call is a barrier, there is no window in which a result from
 *     an abandoned pass can arrive later and be applied. Generation checking
 *     still happens before the write, for the case where the *world* changed
 *     underneath a pass that ran to completion.
 */
#ifndef ATLAS_WORKERS_H
#define ATLAS_WORKERS_H

#include <stddef.h>
#include <stdint.h>

#include "atlas/error.h"

typedef struct atlas_workers atlas_workers;

/* The job. `index` identifies the caller's slot; `ud` is the shared, read-mostly
 * context. A job reports failure by writing it into its own slot: it must not
 * report through a shared atlas_err, because several jobs can fail at once. */
typedef void (*atlas_worker_fn)(size_t index, void *ud);

/* Starts `count` threads, clamped to [1, ATLAS_WORKER_COUNT_MAX] and to the
 * number of online CPUs. `count == 0` selects a default. */
atlas_status atlas_workers_start(size_t count, atlas_workers **out, atlas_err *err);

/* Stops the pool and joins every thread. Safe on NULL. */
void atlas_workers_stop(atlas_workers *w);

/* Number of threads actually running. */
size_t atlas_workers_count(const atlas_workers *w);

/* Runs `fn(i, ud)` for i in [0, n) and returns when all have completed.
 * `w` may be NULL, in which case the jobs run serially on the calling thread —
 * which is what a one-shot CLI invocation wants, and what the tests use to get
 * a deterministic ordering. */
atlas_status atlas_workers_for_each(atlas_workers *w, size_t n, atlas_worker_fn fn, void *ud,
                                    atlas_err *err);

#endif /* ATLAS_WORKERS_H */

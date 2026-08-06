/* Atlas - the data-directory writer lock.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Exactly one process may write the Atlas index at a time. SQLite's own locking
 * would serialise the writes, but it would not stop two processes from both
 * believing they own the index: a daemon reconciling in the background and an
 * `atlas scan` started by hand would interleave generations and produce a
 * consistent database describing an inconsistent story.
 *
 * So writing is gated by an advisory lock on <data-dir>/atlas.lock, taken with
 * flock(LOCK_EX|LOCK_NB):
 *
 *   - the daemon holds it for its whole lifetime
 *   - an offline one-shot writer holds it only for the duration of its mutation
 *   - a second writer of either kind is refused with an actionable message,
 *     never queued and never allowed to proceed anyway
 *
 * flock is used rather than a pid file because the kernel releases it when the
 * holder dies, however it dies. A crashed daemon leaves no stale lock to clean
 * up, and there is no window where a pid has been recycled onto another process.
 * The lock file's own contents are advisory diagnostics only: ownership is
 * decided by the kernel, never by what the file says.
 */
#ifndef ATLAS_LOCK_H
#define ATLAS_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

#define ATLAS_LOCK_FILENAME "atlas.lock"

typedef struct atlas_lock atlas_lock;

typedef enum atlas_lock_role {
    ATLAS_LOCK_ROLE_DAEMON = 0, /* held for the process lifetime */
    ATLAS_LOCK_ROLE_ONESHOT     /* held for one mutation by a CLI invocation */
} atlas_lock_role;

/* Acquires the writer lock for `data_dir`. Returns ATLAS_ERR_INTEGRITY with a
 * message naming the current holder's recorded role and pid when it is already
 * held. Never blocks. */
atlas_status atlas_lock_acquire(const char *data_dir, atlas_lock_role role, atlas_lock **out,
                                atlas_err *err);

/* Releases and frees. Safe on NULL. */
void atlas_lock_release(atlas_lock *lk);

/* Reports whether the lock is currently held by anybody, without acquiring it.
 * This is a probe: the answer is only a fact about the instant it was taken, so
 * callers must still handle losing a subsequent acquire race. `holder_out` may
 * be NULL; when given it receives the diagnostic text recorded by the holder. */
atlas_status atlas_lock_probe(const char *data_dir, bool *held_out, atlas_buf *holder_out,
                              atlas_err *err);

/* The absolute lock path for a data directory, for diagnostics. */
atlas_status atlas_lock_path(const char *data_dir, atlas_buf *out, atlas_err *err);

#endif /* ATLAS_LOCK_H */

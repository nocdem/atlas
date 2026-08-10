/* Atlas - A8: the per-attempt worker workspace.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## The layout, and why it is constructed in one place
 *
 *   <worker_root>/jobs/<job_uid>/<attempt_no>/
 *       spec.json          the immutable canonical job specification
 *       source/            the trusted snapshot, never written after creation
 *       work/              the writable tree the driver may change
 *       driver/            driver start and result metadata
 *       logs/              stdout.log, stderr.log
 *       tests/             validation-command evidence
 *       artifacts/         collected artifacts, including changes.patch
 *       events.jsonl       structured worker events
 *       result.json        the completion envelope
 *
 * Every one of those paths is built by `atlas_ws_open` from components Atlas
 * chose — a validated worker root, an Atlas-generated job id, and an integer
 * attempt number. No component of a workspace path ever comes from a
 * repository, a request, a driver or a model. That is what makes "a job cannot
 * reach another job's workspace" a property of construction rather than a check
 * somebody might forget.
 *
 * ## What the snapshot deliberately is not
 *
 * It is a directory of ordinary files with **no git metadata whatsoever**. There
 * is no `.git` anywhere under an attempt, so:
 *
 *   * there is no repository configuration to be hostile;
 *   * there are no hooks, because there is no hook directory to put them in;
 *   * there are no alternates, so nothing can write objects back into the source;
 *   * there is no index and no lock file, so nothing under `/opt/...` is touched;
 *   * submodules and LFS are not "disabled", they are *absent* — a gitlink entry
 *     is refused at listing time and no machinery exists to act on one.
 *
 * Symlinks are refused rather than recreated. A snapshot that reproduced a
 * tracked symlink would hand the driver a path that escapes the workspace the
 * moment it is followed, and A8's whole isolation argument is that it cannot.
 *
 * ## Removal
 *
 * `atlas_ws_remove` descends with `openat` and `O_NOFOLLOW` from a validated
 * root and refuses anything that is not a directory or a regular file. It never
 * takes a path from a caller beyond the same three components `atlas_ws_open`
 * takes, and it is bounded in depth and in entries. Atlas has no
 * "remove this path recursively" primitive and must not grow one.
 */
#ifndef ATLAS_WORKSPACE_H
#define ATLAS_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Bounds. Each refuses rather than trims: a snapshot that silently stopped
 * copying would hand the driver a repository that is missing files, and every
 * result computed from it would be about something that never existed. */
#define ATLAS_WS_MAX_FILES 20000
#define ATLAS_WS_MAX_BYTES (512ll * 1024ll * 1024ll)
#define ATLAS_WS_MAX_FILE_BYTES (64ll * 1024ll * 1024ll)
#define ATLAS_WS_MAX_DEPTH 32
/* Free space required under the worker root before an attempt starts. A job
 * that fills the disk takes the machine down with it, and the dispatcher is the
 * only thing positioned to notice in time. */
#define ATLAS_WS_MIN_FREE_BYTES (256ll * 1024ll * 1024ll)

typedef struct atlas_ws {
    atlas_buf root;      /* .../jobs/<job_uid>/<attempt_no> */
    atlas_buf source;    /* pristine */
    atlas_buf work;      /* writable */
    atlas_buf logs;
    atlas_buf tests;
    atlas_buf artifacts;
    atlas_buf driver;
    /* An open descriptor on the attempt root, kept for the workspace's life so
     * that later operations descend from a directory that was validated once
     * rather than re-resolving a path that could have been swapped underneath. */
    int root_fd;
} atlas_ws;

void atlas_ws_init(atlas_ws *w);
void atlas_ws_free(atlas_ws *w);

/* Validates `worker_root` and creates the attempt tree.
 *
 * The root must be absolute, must exist, must be a directory reached without
 * traversing a symlink, must be owned by the calling uid, and must not be
 * writable by group or other. Root ownership is *not* required — the worker owns
 * its own area — but ownership by somebody else is refused: a workspace root
 * another account can write is one another account can pre-create entries in. */
atlas_status atlas_ws_open(const char *worker_root, const char *job_uid, int64_t attempt_no,
                           atlas_ws *out, atlas_err *err);

typedef struct atlas_ws_snapshot_stats {
    int64_t files;
    int64_t bytes;
    /* Entries refused by kind, reported rather than silently skipped: a caller
     * that does not know a symlink was dropped believes it has a faithful
     * snapshot. */
    int64_t skipped_symlinks;
    int64_t skipped_submodules;
    int64_t skipped_other;
} atlas_ws_snapshot_stats;

/* Materialises one received snapshot entry into both `source/` and `work/`.
 *
 * **The worker no longer reads a repository.** `atlas_ws_snapshot` — which
 * opened the registered repository with git — is gone, and with it the worker's
 * need for any path under `/opt`. Bytes arrive from `atlasd` over the socket and
 * are written here, and every path is re-checked on arrival: the daemon
 * validated it, and a receiver that trusts a sender's path validation has no
 * boundary of its own.
 *
 * Two copies are written rather than a link, because a hard link would let a
 * driver's edit rewrite the pristine side and make the patch look empty. */
atlas_status atlas_ws_materialise(const atlas_ws *w, const void *rel, size_t rel_len,
                                  const char *mode, const void *data, size_t len, bool first,
                                  atlas_err *err);

/* Produces `artifacts/changes.patch` by diffing the pristine snapshot against
 * the working tree. `changed_out` receives the number of changed files.
 *
 * The patch is an artifact and nothing else. There is no function in Atlas that
 * applies one, and adding one is explicitly deferred past A8. */
atlas_status atlas_ws_make_patch(const atlas_ws *w, int64_t max_bytes, int64_t *changed_out,
                                 bool *differed_out, atlas_err *err);

/* Enumerates the files a driver changed, relative to the work tree. Compares the
 * two trees by content rather than by asking the driver, because a driver
 * reporting its own changes is a driver describing itself. */
typedef atlas_status (*atlas_ws_changed_cb)(const char *rel, void *ud, atlas_err *err);
atlas_status atlas_ws_changed_files(const atlas_ws *w, atlas_ws_changed_cb cb, void *ud,
                                    int64_t *count_out, atlas_err *err);

/* True when every changed path lies under one of the job's declared prefixes.
 * An empty declaration means "anywhere in the workspace" and is not a wildcard
 * over the host — the workspace is the boundary either way. */
bool atlas_ws_paths_are_declared(const char *changed_rel, const atlas_buf *declared,
                                 size_t declared_count);

/* Collects artifacts from `artifacts/`, refusing anything that is not a regular
 * file reached without following a symlink. A driver that plants a symlink to
 * `/etc/shadow` and asks for artifact collection gets a refusal naming the
 * entry, never its target's bytes. */
typedef struct atlas_ws_artifact {
    atlas_buf name;
    atlas_buf sha256;
    int64_t size_bytes;
    atlas_buf content; /* populated only below `inline_max` */
    bool content_stored;
} atlas_ws_artifact;

atlas_status atlas_ws_collect(const atlas_ws *w, int64_t max_count, int64_t max_bytes,
                              size_t inline_max, atlas_ws_artifact **out, size_t *count_out,
                              int64_t *refused_out, atlas_err *err);
void atlas_ws_artifacts_free(atlas_ws_artifact *a, size_t count);

/* Bounded removal of one attempt directory. Refuses a symlinked component and
 * anything that is not a directory or regular file. */
atlas_status atlas_ws_remove(const char *worker_root, const char *job_uid, int64_t attempt_no,
                             atlas_err *err);

/* Free bytes under the worker root, for the disk-space threshold check. */
atlas_status atlas_ws_free_space(const char *worker_root, int64_t *bytes_out, atlas_err *err);

/* Writes one file inside the attempt root, creating parents. `rel` must be a
 * safe relative path; it is the only way anything writes into a workspace from
 * outside this file. */
atlas_status atlas_ws_write(const atlas_ws *w, const char *rel, const void *data, size_t len,
                            atlas_err *err);

/* Replaces anything that looks like a credential with a fixed marker.
 *
 * Applied to every captured log before it is stored or reported. It is a
 * mitigation, not a guarantee: a secret Atlas has never seen the shape of will
 * pass through, which is exactly why no credential is ever placed in a
 * workspace, an environment or a job specification in the first place. Saying
 * "logs are redacted" without that sentence would be the overclaim. */
atlas_status atlas_ws_redact(const char *in, size_t len, atlas_buf *out, int64_t *hits_out,
                             atlas_err *err);

#endif /* ATLAS_WORKSPACE_H */

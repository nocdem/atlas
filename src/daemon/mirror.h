/* Atlas - A13: the mirror.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The daemon runs as its own principal and cannot read a repository owned by
 * somebody else. A scanner running as that owner can, and hands the bytes over;
 * this is where they land — under the daemon's own data directory, owned by the
 * daemon, readable by it.
 *
 * The paths come from a scanner, which reads names an untrusted repository
 * chose. So every component is created and opened the way `src/orch/workspace.c`
 * materialises a snapshot, and for the same reason: **`openat` with
 * `O_NOFOLLOW` from a descriptor validated once, never a path string.** A
 * symlink anywhere along the way refuses rather than redirects, and a name that
 * is not a safe relative path is refused before anything is opened.
 */
#ifndef ATLAS_MIRROR_H
#define ATLAS_MIRROR_H

#include "atlas/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opens `<data_dir>/mirror/<repo_id>`, creating what is missing. The caller
 * closes `*fd_out`. Every directory is created 0700 and opened `O_NOFOLLOW`. */
atlas_status atlas_mirror_open_repo(const char *data_dir, int64_t repo_id, int *fd_out,
                                    atlas_err *err);

/* Writes `len` bytes at `rel` beneath `root_fd`.
 *
 * `first` starts the file: any previous entry is unlinked and the file is
 * created with `O_EXCL`, so a rescanned file replaces rather than accumulates.
 * Otherwise the bytes are appended, and a path that does not exist is refused —
 * with no `O_CREAT`, because a chunk arriving for a file that was never started
 * means the stream broke, and creating one would paper over that.
 *
 * `rel` must satisfy `atlas_snapshot_path_ok`. One that does not is refused
 * before any descriptor is opened. */
/* `exec` sets the one mode bit git tracks. A file mirrored without it reads as
 * modified against the mirrored index, because git compares 100755 with 100644
 * and calls the difference a change. */
atlas_status atlas_mirror_put(int root_fd, const void *rel, size_t rel_len, bool first, bool exec,
                              const void *data, size_t len, atlas_err *err);

/* A13. Recreates a symlink in the mirror, with the link text the tree holds.
 * The link text is the content Atlas hashes, and nothing follows it. See the
 * definition. */
atlas_status atlas_mirror_put_symlink(int root_fd, const void *rel, size_t rel_len,
                                      const void *target, size_t target_len, atlas_err *err);

/* A13. Opens the directory a pass writes into, rather than the one readers use.
 * A refresh must not make a finished mirror unreadable; see the definition. */
atlas_status atlas_mirror_open_staging(const char *data_dir, int64_t repo_id, int *fd_out,
                                       atlas_err *err);

/* A13. Makes the staged generation the one readers see, by rename. See the
 * definition. */
/* A13. Carries one path from the published generation into the staging one
 * without its bytes crossing the socket again.
 *
 * A mirroring pass used to re-read, hex-encode and send every file every time,
 * because `atlas_mirror_publish` renames the staging directory into place and
 * the next pass therefore starts from an empty one. Measured 2026-08-28: 28,450
 * files across two repositories, every five minutes, of which essentially none
 * had changed.
 *
 * `*kept_out` is false — not an error — when the published generation does not
 * hold that path, which is how a scanner whose memory of what it sent has
 * outlived the mirror corrects itself: it sends the bytes instead. Nothing here
 * trusts the caller's belief about the file. The link is made without following
 * symlinks, so a mirrored symlink is carried as a symlink. */
/* One repository-relative path, as bytes. Paths are bytes, so a length travels
 * with every one of them. */
typedef struct atlas_mirror_path {
    const void *rel;
    size_t rel_len;
} atlas_mirror_path;

/* Carries a batch, answering per path into `kept_out[i]`.
 *
 * **A batch, because the cost was the request and not the bytes.** Sending one
 * request per file replaced the file's content with its name and left the count
 * alone — measured 2026-08-29, the daemon still spent a full core for half of
 * every cycle, at roughly eight `openat` and nine `pread64` per request, each
 * request also opening its own read-only database handle. The batch opens both
 * generations once and authorises once; what the daemon decides per path does
 * not change. */
atlas_status atlas_mirror_keep_many(const char *data_dir, int64_t repo_id,
                                    const atlas_mirror_path *paths, size_t n, bool *kept_out,
                                    atlas_err *err);

atlas_status atlas_mirror_publish(const char *data_dir, int64_t repo_id, atlas_err *err);

#endif /* ATLAS_MIRROR_H */

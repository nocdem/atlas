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
atlas_status atlas_mirror_put(int root_fd, const void *rel, size_t rel_len, bool first,
                              const void *data, size_t len, atlas_err *err);

/* A13. Recreates a symlink in the mirror, with the link text the tree holds.
 * The link text is the content Atlas hashes, and nothing follows it. See the
 * definition. */
atlas_status atlas_mirror_put_symlink(int root_fd, const void *rel, size_t rel_len,
                                      const void *target, size_t target_len, atlas_err *err);

#endif /* ATLAS_MIRROR_H */

/* Atlas - opening a file that only root may have written.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A7 introduced this walk inside `src/core/authority.c`, for the operator
 * policy. A7.1 needs exactly the same question answered about a second file —
 * the system-deployment policy — and about the socket's runtime directory, so
 * the walk moved here rather than being written twice.
 *
 * One implementation, for the reason Atlas gives everywhere else: two copies of
 * a security check are two places for one of them to be weaker, and the weaker
 * one is the one an attacker uses.
 *
 * The question it answers is narrow and absolute:
 *
 *   Is this absolute path reachable from `/` without traversing a single
 *   symbolic link, with every component — the root, every directory on the way,
 *   and the file itself — owned by uid 0 and writable by nobody else?
 *
 * If yes, the returned descriptor refers to a file that no unprivileged uid on
 * this machine could have created, replaced or edited. If no, the caller is told
 * which component failed and why, and must fail closed.
 *
 * `realpath(3)` is deliberately absent and must stay absent: it resolves links,
 * which means it answers about whichever file the link currently points at, and
 * the whole point here is to refuse a path somebody else can re-aim.
 *
 * The sticky bit is not treated as an exception. A world-writable sticky
 * directory like `/tmp` stops one user deleting another's files and does nothing
 * to stop them creating their own, so a policy underneath one is a policy an
 * attacker can put a directory beside.
 */
#ifndef ATLAS_ROOTPATH_H
#define ATLAS_ROOTPATH_H

#include <stdbool.h>
#include <stddef.h>

typedef enum atlas_rootpath_result {
    /* Zero is a failure, deliberately: a zeroed result is one nobody filled
     * in, and the safe reading of that is never "this path is trusted". */
    ATLAS_ROOTPATH_UNKNOWN = 0,
    ATLAS_ROOTPATH_OK,
    /* The path, or a component of it, does not exist. */
    ATLAS_ROOTPATH_MISSING,
    /* A component is a symbolic link, so whoever can create links there
     * chooses the file. */
    ATLAS_ROOTPATH_SYMLINK,
    /* A component is owned by, or writable by, somebody other than root. */
    ATLAS_ROOTPATH_WRITABLE,
    /* Not an absolute path, or contains an empty, `.` or `..` component. */
    ATLAS_ROOTPATH_BAD_PATH,
    /* Reached, root-owned, and not a regular file. */
    ATLAS_ROOTPATH_NOT_REGULAR,
    /* Reached, root-owned, and not a directory when one was required. */
    ATLAS_ROOTPATH_NOT_DIRECTORY
} atlas_rootpath_result;

const char *atlas_rootpath_result_name(atlas_rootpath_result r);

/* Walks `path` from `/`, refusing symlinks and non-root-owned components.
 *
 * `want_dir` selects whether the final component must be a directory or a
 * regular file; anything else is refused rather than opened.
 *
 * On ATLAS_ROOTPATH_OK the caller owns the returned descriptor and must
 * `close()` it. On anything else the return is -1. `detail`, when not NULL,
 * receives the path prefix that failed — never a caller-supplied string beyond
 * what was passed in, and always NUL-terminated. */
int atlas_rootpath_open(const char *path, bool want_dir, atlas_rootpath_result *result_out,
                        char *detail, size_t detail_size);

#endif /* ATLAS_ROOTPATH_H */

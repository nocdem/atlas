/* Atlas - byte-safe path representation and safe path traversal.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Repository paths are arbitrary byte strings: they are not necessarily UTF-8
 * and may contain spaces, tabs and newlines. Atlas therefore keeps two
 * representations of every path:
 *
 *   raw   the exact bytes as reported by git, stored as a SQLite BLOB
 *   text  a lossless, printable, valid-UTF-8 encoding used for display, search
 *         and JSON. Bytes that are not valid UTF-8, plus control bytes, '%' and
 *         DEL, are percent-escaped as %XX (uppercase hex). Everything else is
 *         copied verbatim, so ASCII paths are identical in both forms.
 *
 * The encoding is reversible: atlas_path_text_decode() reproduces the raw bytes.
 */
#ifndef ATLAS_PATHREP_H
#define ATLAS_PATHREP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Encode raw path bytes into the safe text form (appends to `out`). */
atlas_status atlas_path_text_encode(const void *raw, size_t n, atlas_buf *out, atlas_err *err);
/* Decode the safe text form back to raw bytes (appends to `out`). */
atlas_status atlas_path_text_decode(const char *text, size_t n, atlas_buf *out, atlas_err *err);
/* True when the raw bytes need no escaping at all. */
bool atlas_path_is_plain(const void *raw, size_t n);

/* Reject paths Atlas refuses to index or resolve: empty, absolute, containing a
 * NUL, "." or ".." components, or an internal empty component. Repository paths
 * from git are already normalised and relative; this is defence in depth
 * against hostile or corrupt input. */
atlas_status atlas_path_check_relative(const void *raw, size_t n, atlas_err *err);

/* Result of a guarded relative-path resolution. */
typedef enum atlas_path_open_result {
    ATLAS_PATH_OPEN_OK = 0,       /* fd_out holds an O_RDONLY|O_NOFOLLOW fd */
    ATLAS_PATH_OPEN_SYMLINK,      /* final component is a symlink (not opened) */
    ATLAS_PATH_OPEN_UNSAFE,       /* an intermediate component is a symlink */
    ATLAS_PATH_OPEN_MISSING,      /* component does not exist */
    ATLAS_PATH_OPEN_NOT_REGULAR,  /* exists but is neither regular nor symlink */
    ATLAS_PATH_OPEN_DENIED        /* permission or other errno */
} atlas_path_open_result;

/* Walk `rel` component-by-component starting at `root_fd`, refusing to traverse
 * any symlink. Never opens the final component when it is a symlink. On
 * ATLAS_PATH_OPEN_OK the caller owns *fd_out and must close() it. `st_out` is
 * filled from an lstat of the final component whenever it exists. */
atlas_status atlas_path_open_nofollow(int root_fd, const char *rel, size_t rel_len,
                                      atlas_path_open_result *result_out, int *fd_out,
                                      struct stat *st_out, int *errno_out, atlas_err *err);

/* Read the target text of a tracked symlink without following it. */
atlas_status atlas_path_readlink_at(int root_fd, const char *rel, size_t rel_len,
                                    atlas_buf *target_out, atlas_path_open_result *result_out,
                                    atlas_err *err);

#endif /* ATLAS_PATHREP_H */

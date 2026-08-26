/* Atlas - A13: where a repository's bytes are read from.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#ifndef ATLAS_MIRROR_H
#define ATLAS_MIRROR_H

#include "atlas/buf.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/git.h"

/* `<data_dir>/mirror/<repo_id>`, as a string, for opening the mirror to read.
 *
 * The write side never forms this path. `atlas_mirror_open_repo` walks
 * `data_dir` -> `mirror` -> `<id>` with `openat` from a descriptor it validated,
 * which is the discipline `src/orch/workspace.c` set and the reason a component
 * a caller supplied can never escape the directory. Handing it a composed
 * string would weaken that, so this builder is the read side's alone and the
 * two are deliberately not unified. */
atlas_status atlas_mirror_repo_path(const char *data_dir, int64_t repo_id, atlas_buf *out,
                                    atlas_err *err);

/* Opens the repository a reader should use, preferring the tree itself.
 *
 * A13. A registered tree a process cannot read is one it can never index —
 * measured, with `atlasd` as its own principal, as "repository ... cannot be
 * opened" logged every ten seconds against a repository that was intact. The
 * scanner's mirror is the answer, and Plan 5 made it a real git repository
 * precisely so that this is a different root and nothing else.
 *
 * The real root is tried first and the mirror only on its failure. Reading the
 * thing itself is better evidence than reading a copy of it, so the fallback
 * must never quietly become the preference.
 *
 * **`data_dir == NULL` means the tree itself is the only acceptable source.**
 * The guarantee is the absent argument rather than a flag, so a caller that
 * supplies nothing gets the behaviour that shipped before A13 rather than a
 * surprise. Three readers pass NULL deliberately: registration, which has no
 * row and therefore no mirror to consult; the run driver's pinned-commit check
 * and the worker's workspace snapshot, which must see the tree a worker edits.
 *
 * A mirror is refused for a repository whose row names no scanner. `out` is
 * NULL on failure and the error is the *real* root's, because that is the one
 * an operator has to act on.
 */
atlas_status atlas_repo_open_git(const atlas_repo_info *info, const char *data_dir,
                                 atlas_git **out, bool *from_mirror, atlas_err *err);

#endif /* ATLAS_MIRROR_H */

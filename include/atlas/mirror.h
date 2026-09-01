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
 * **The row decides the source, not a failure.** A repository whose row names a
 * scanner is read from its mirror and from nothing else: no mirror is a refusal,
 * an incomplete mirror is a refusal, and the tree is never the way out of
 * either. A process running *as* the scanner uid reads the tree directly, which
 * is a capability question — can this process read those bytes — and grants
 * nothing.
 *
 * This paragraph once described the opposite, and the opposite was implemented,
 * tried and reverted. The first design read the tree and fell back to the mirror
 * on failure, on the reasoning that reading the thing itself beats reading a copy
 * of it. It answered neither real failure, because both were **partial**: a
 * repository whose hundred loose objects were mode 0400, so `atlas_git_open`
 * succeeded and `git log` failed three calls later; and one whose fifty private
 * directories could not be entered, so every pass completed while covering less
 * than the tree. A fallback keyed on "could not open" catches neither. Keyed on
 * the row, both stop being this process's problem, because it never touches the
 * tree at all.
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

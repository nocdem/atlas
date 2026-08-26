/* Atlas - A13: choosing which bytes a repository is read from.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See the contract in `include/atlas/mirror.h`. This file makes no decision
 * about *who* may write a mirror — that is one comparison in
 * `src/ipc/server_scanner.c`, against the uid the repository's row names.
 */
#include "atlas/mirror.h"

#include <stdio.h>
#include <unistd.h>

atlas_status atlas_mirror_repo_path(const char *data_dir, int64_t repo_id, atlas_buf *out,
                                    atlas_err *err) {
    if (data_dir == NULL || *data_dir == '\0') {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no data directory to build a mirror path");
    }
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, "%s/mirror/%lld", data_dir, (long long)repo_id);
}

atlas_status atlas_repo_open_git(const atlas_repo_info *info, const char *data_dir,
                                 atlas_git **out, bool *from_mirror, atlas_err *err) {
    if (out == NULL || info == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no repository handle to fill");
    }
    *out = NULL;
    if (from_mirror != NULL) {
        *from_mirror = false;
    }

    /* A13. **The row decides the source, not a failure.**
     *
     * A repository that names a scanner is read from its mirror and from
     * nothing else. This process does not open its tree, does not stat it and
     * does not fall back to it — not when the mirror is missing, not when it is
     * stale, not when the tree is right there and readable.
     *
     * The first design had this the other way round: try the tree, use the
     * mirror when the tree refuses. It was wrong, and the machine this season
     * was built on is what showed it. Both failures there were *partial*: a
     * repository whose 100 loose objects were mode 0400, so `atlas_git_open`
     * succeeded and `git log` failed three calls later; and one whose 50
     * private directories could not be entered, so every pass completed and
     * covered less than the tree. A fallback keyed on "could not open" answers
     * neither. Keyed on the row, both stop being this process's problem: it
     * never touches the tree at all.
     *
     * The cost is stated rather than hidden. A repository whose scanner has not
     * run has no mirror, and it is **refused rather than read from its tree** —
     * because reading the tree is exactly the thing the operator asked Atlas to
     * stop doing when they named a scanner. */
    /* The scanner uid names the principal that may read the tree. A process
     * running as it — the scanner itself, and the operator's own CLI — reads the
     * tree, because it can and because reading a copy of what you can read
     * directly is worse evidence. Every other principal, the daemon above all,
     * reads the mirror and never the tree.
     *
     * `geteuid` is a capability question here, not an authority one: it asks
     * "can this process read that tree", which is a fact about the filesystem,
     * and it grants nothing. A7's rule is about deciding *permission* from
     * process properties, and no permission is decided here. */
    if (info->scanner_uid != 0 && (int64_t)geteuid() != info->scanner_uid) {
        if (data_dir == NULL) {
            /* A caller that may not consult a mirror asked about a repository
             * that may only be read through one. `rundriver` and `snapshot`
             * pass NULL and must see the worker's real tree, so this is not
             * reachable from them for any repository they drive; it is a
             * programming error everywhere else. */
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "repository %lld names a scanner, so it has no readable tree "
                                 "for a caller that may not use the mirror",
                                 (long long)info->id);
        }
        /* An incomplete mirror is not a repository, and the daemon reads it as
         * one. Measured on the first live run: a mirror carrying 2007 of a
         * tree's 22012 files made the daemon record 20000 deletions, because
         * every file the mirror does not hold is a file that no longer exists.
         *
         * So a mirror is read only when the run that wrote it said it finished
         * and skipped nothing. Anything else is refused, exactly as a missing
         * mirror is: waiting is the correct answer, and the tree is never the
         * fallback. */
        if (!info->mirror_complete) {
            return atlas_err_set(err, ATLAS_ERR_REPO,
                                 "repository %lld has no complete mirror yet, so there is "
                                 "nothing for this process to read",
                                 (long long)info->id);
        }
        atlas_buf path = ATLAS_BUF_INIT;
        atlas_status st = atlas_mirror_repo_path(data_dir, info->id, &path, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&path);
            return st;
        }
        atlas_git *mg = NULL;
        st = atlas_git_open(atlas_buf_cstr(&path), &mg, err);
        atlas_buf_free(&path);
        if (st != ATLAS_OK) {
            return st;
        }
        *out = mg;
        if (from_mirror != NULL) {
            *from_mirror = true;
        }
        return ATLAS_OK;
    }

    /* No scanner named: this process reads the tree itself, as it always has.
     *
     * The status is kept, not only the message. `atlas_git_open` refuses a
     * partial (promisor) repository with an integrity status, and a caller that
     * saw that collapsed into a plain repository error would read "not found"
     * where Atlas meant "refused, and deliberately". Measured: flattening it
     * turned `test_git_hardening`'s rescan case from 7 into 4. */
    atlas_git *g = NULL;
    atlas_status st = atlas_git_open(atlas_buf_cstr(&info->root_path), &g, err);
    if (st == ATLAS_OK) {
        *out = g;
    }
    return st;
}

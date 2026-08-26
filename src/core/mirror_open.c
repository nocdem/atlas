/* Atlas - A13: choosing which bytes a repository is read from.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See the contract in `include/atlas/mirror.h`. This file makes no decision
 * about *who* may write a mirror — that is one comparison in
 * `src/ipc/server_scanner.c`, against the uid the repository's row names.
 */
#include "atlas/mirror.h"

#include <stdio.h>

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

    atlas_git *g = NULL;
    atlas_err direct;
    atlas_err_init(&direct);
    if (atlas_git_open(atlas_buf_cstr(&info->root_path), &g, &direct) == ATLAS_OK) {
        *out = g;
        return ATLAS_OK;
    }

    /* The tree did not answer. It may be gone, it may be broken, or it may
     * simply belong to a principal this process is not — the three are
     * indistinguishable from here, and the mirror answers the third without
     * needing to tell them apart. */
    if (data_dir == NULL) {
        *err = direct;
        return ATLAS_ERR_REPO;
    }

    /* A repository whose row names no scanner has no writer for its mirror, so
     * there is nothing there this process should trust.
     *
     * No reachable path produces such a mirror today: `atlas_scanner_uid_refusal`
     * refuses uid 0 at assignment, and `peer_owns` admits no peer without a
     * non-zero `scanner_uid`, so a mirror cannot be written for a row that
     * names none. The check is here so that "the row still names this writer"
     * — the warrant that replaces the canonical-root check in
     * `atlas_service_open_repo_git` — is true because this code asks, and not
     * because a refusal in another file makes the alternative unreachable. */
    if (info->scanner_uid == 0) {
        *err = direct;
        return ATLAS_ERR_REPO;
    }

    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_mirror_repo_path(data_dir, info->id, &path, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&path);
        return st;
    }

    atlas_err from_copy;
    atlas_err_init(&from_copy);
    bool opened = atlas_git_open(atlas_buf_cstr(&path), &g, &from_copy) == ATLAS_OK;
    atlas_buf_free(&path);
    if (opened) {
        *out = g;
        if (from_mirror != NULL) {
            *from_mirror = true;
        }
        return ATLAS_OK;
    }

    /* Neither answered. The error reported is the *real* root's: an operator
     * acts on the repository, and "the mirror is not a git repository" would
     * send them to a directory Atlas owns and they never created. */
    *err = direct;
    return ATLAS_ERR_REPO;
}

/* Atlas - A13: the mirror.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every directory here is created with `mkdirat` and opened with `O_NOFOLLOW`
 * from a descriptor that was validated once, never from a path string. That is
 * `src/orch/workspace.c`'s rule and it is repeated rather than shared because
 * the two roots differ; the discipline does not. A repository chooses the names
 * that arrive here, so a symlink anywhere along the way must refuse rather than
 * redirect the write somewhere the daemon can reach and the repository cannot.
 */
#include "daemon/mirror.h"

#include "daemon/daemon_internal.h"

#include "atlas/snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The longest single path component this will create. A name longer than this
 * is refused rather than truncated: a truncated name is a different file. */
#define MIRROR_COMP_MAX 255u

/* mkdirat + openat, tolerating an existing directory but never a symlink. */
static int make_dir(int parent, const char *name, atlas_err *err) {
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
        (void)atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                  "cannot create mirror directory \"%s\"", name);
        return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        (void)atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                  "cannot open mirror directory \"%s\" without following a link",
                                  name);
        return -1;
    }
    return fd;
}

atlas_status atlas_mirror_open_repo(const char *data_dir, int64_t repo_id, int *fd_out,
                                    atlas_err *err) {
    if (data_dir == NULL || data_dir[0] == '\0' || fd_out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a data directory is required");
    }
    if (repo_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a repository id is required");
    }
    *fd_out = -1;

    int base = open(data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (base < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                   "cannot open the data directory to reach the mirror");
    }
    int mirror = make_dir(base, "mirror", err);
    (void)close(base);
    if (mirror < 0) {
        return ATLAS_ERR_INTEGRITY;
    }

    char name[32];
    (void)snprintf(name, sizeof(name), "%lld", (long long)repo_id);
    int repo = make_dir(mirror, name, err);
    (void)close(mirror);
    if (repo < 0) {
        return ATLAS_ERR_INTEGRITY;
    }
    *fd_out = repo;
    return ATLAS_OK;
}

atlas_status atlas_mirror_put(int root_fd, const void *rel, size_t rel_len, bool first,
                              const void *data, size_t len, atlas_err *err) {
    if (root_fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a mirror root is required");
    }
    /* Refused before any descriptor is opened. The check is lexical and that is
     * all it can be: the daemon cannot canonicalise a path in a tree it never
     * reads, so what it enforces is the shape of the name. */
    if (!atlas_snapshot_path_ok(rel, rel_len)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a mirror path must be a safe relative path");
    }

    const char *p = (const char *)rel;
    /* Walk to the leaf's parent, creating directories on the way. */
    int parent = dup(root_fd);
    if (parent < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot hold the mirror root");
    }
    size_t start = 0;
    char comp[MIRROR_COMP_MAX + 1u];
    for (size_t i = 0; i < rel_len; i++) {
        if (p[i] != '/') {
            continue;
        }
        size_t n = i - start;
        if (n > MIRROR_COMP_MAX) {
            (void)close(parent);
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a mirror path component is too long");
        }
        memcpy(comp, p + start, n);
        comp[n] = '\0';
        int next = make_dir(parent, comp, err);
        (void)close(parent);
        if (next < 0) {
            return ATLAS_ERR_INTEGRITY;
        }
        parent = next;
        start = i + 1u;
    }

    size_t leaf_len = rel_len - start;
    if (leaf_len == 0 || leaf_len > MIRROR_COMP_MAX) {
        (void)close(parent);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a mirror path leaf is empty or too long");
    }
    memcpy(comp, p + start, leaf_len);
    comp[leaf_len] = '\0';

    int fd = -1;
    if (first) {
        /* A rescanned file replaces rather than accumulates, and O_EXCL after
         * an unlink is what makes the create refuse a symlink planted between
         * the two rather than write through it. */
        if (unlinkat(parent, comp, 0) != 0 && errno != ENOENT) {
            int saved = errno;
            (void)close(parent);
            return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                       "cannot replace \"%s\" in the mirror", comp);
        }
        fd = openat(parent, comp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    } else {
        /* No O_CREAT. A chunk for a file that was never started means the
         * stream broke, and creating one here would turn a detectable failure
         * into a silently truncated file. */
        fd = openat(parent, comp, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    }
    int saved = errno;
    (void)close(parent);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   first ? "cannot create \"%s\" in the mirror"
                                         : "cannot append to \"%s\" in the mirror: no such file "
                                           "was started",
                                   comp);
    }

    atlas_status st = ATLAS_OK;
    const char *b = (const char *)data;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, b + done, len - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                     "cannot write \"%s\" into the mirror", comp);
            break;
        }
        done += (size_t)w;
    }
    (void)close(fd);
    return st;
}

/* A13 Plan 6. See the contract in `daemon/daemon_internal.h`. */
atlas_status atlas_daemon_open_index_root(const char *data_dir, int64_t repo_id,
                                          const char *root_path, atlas_git **out,
                                          bool *from_mirror, atlas_err *err) {
    if (out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no repository handle to fill");
    }
    *out = NULL;
    if (from_mirror != NULL) {
        *from_mirror = false;
    }

    atlas_git *g = NULL;
    atlas_err direct;
    atlas_err_init(&direct);
    if (atlas_git_open(root_path, &g, &direct) == ATLAS_OK) {
        *out = g;
        return ATLAS_OK;
    }

    /* The tree did not answer. It may be gone, it may be broken, or it may
     * simply belong to a principal this process is not — the three are
     * indistinguishable from here, and the mirror is the answer to the third
     * without needing to tell them apart. */
    char path[ATLAS_SNAPSHOT_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/mirror/%lld", data_dir, (long long)repo_id);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the mirror path does not fit");
    }

    atlas_err from_copy;
    atlas_err_init(&from_copy);
    if (atlas_git_open(path, &g, &from_copy) == ATLAS_OK) {
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

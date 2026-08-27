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
#include <stdlib.h>
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

/* Walks `rel` to its leaf's parent, creating directories on the way, and copies
 * the leaf's name into `comp`.
 *
 * Shared by the two writers so there is one implementation of "where does this
 * path land in the mirror": a file and a symlink differ in what is created at
 * the leaf, in nothing before it. */
static atlas_status walk_to_parent(int root_fd, const void *rel, size_t rel_len, int *parent_out,
                                   char *comp, atlas_err *err) {
    *parent_out = -1;
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
    int parent = dup(root_fd);
    if (parent < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot hold the mirror root");
    }
    size_t start = 0;
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
    *parent_out = parent;
    return ATLAS_OK;
}

/* A13. Recreates a symlink in the mirror, with the link text the tree holds.
 *
 * **The link text is the content.** Atlas hashes a tracked symlink's text and
 * never opens its target — `reconcile.c`'s `ENTRY_SYMLINK`, "link text hashed;
 * the target was never opened". So a mirror that dropped symlinks was missing
 * files the index holds, and every one of them read as a deletion.
 *
 * Creating symlinks in the mirror is safe for the same reason the tree's are:
 * nothing follows them. Every descent in this file and in `reconcile.c` is
 * `O_NOFOLLOW`, so a link text pointing anywhere at all is a string that gets
 * hashed and never a path that gets opened. The target is not resolved, not
 * checked and not required to exist — it is data. */
atlas_status atlas_mirror_put_symlink(int root_fd, const void *rel, size_t rel_len,
                                      const void *target, size_t target_len, atlas_err *err) {
    if (target_len == 0 || memchr(target, '\0', target_len) != NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a symlink's text must be non-empty and hold no NUL");
    }
    char *text = malloc(target_len + 1u);
    if (text == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory mirroring a symlink");
    }
    memcpy(text, target, target_len);
    text[target_len] = '\0';

    int parent = -1;
    char comp[MIRROR_COMP_MAX + 1u];
    atlas_status st = walk_to_parent(root_fd, rel, rel_len, &parent, comp, err);
    if (st != ATLAS_OK) {
        free(text);
        return st;
    }
    /* Replace rather than accumulate, exactly as a rescanned file does. */
    if (unlinkat(parent, comp, 0) != 0 && errno != ENOENT) {
        int saved = errno;
        (void)close(parent);
        free(text);
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   "cannot replace \"%s\" in the mirror", comp);
    }
    int rc = symlinkat(text, parent, comp);
    int saved = errno;
    (void)close(parent);
    free(text);
    if (rc != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   "cannot create the symlink \"%s\" in the mirror", comp);
    }
    return ATLAS_OK;
}

atlas_status atlas_mirror_put(int root_fd, const void *rel, size_t rel_len, bool first, bool exec,
                              const void *data, size_t len, atlas_err *err) {
    int parent = -1;
    char comp[MIRROR_COMP_MAX + 1u];
    atlas_status walk = walk_to_parent(root_fd, rel, rel_len, &parent, comp, err);
    if (walk != ATLAS_OK) {
        return walk;
    }

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
        /* **Git tracks one mode bit and the mirror has to carry it.** A tree's
         * executable file mirrored 0600 compares 100644 against the mirrored
         * index's 100755, and git calls that a modification -- so a clean
         * repository read as dirty with 24 files changed, none of which
         * differed by a byte. Owner-only either way: the daemon's files describe
         * private repositories, and git only asks whether the bit is set. */
        fd = openat(parent, comp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    exec ? 0700 : 0600);
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

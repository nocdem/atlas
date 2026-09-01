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
#include <dirent.h>
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

/* A13. The directory a pass writes into, as opposed to the one readers use.
 *
 * **A refresh must not make a finished mirror unreadable.** The first design
 * cleared `mirror_complete` at the start of every pass, which is right for crash
 * safety and wrong for everything else: a pass over /opt/dna takes seven minutes
 * on a ten-minute cycle, so that repository was refused seventy per cent of the
 * time. "A pass is running" and "the mirror cannot be trusted" are different
 * claims, and I had conflated them.
 *
 * So a pass writes into `<id>.next` and the finished mirror stays at `<id>`,
 * readable and complete throughout. `atlas_mirror_publish` swaps them with
 * `rename`, which is atomic within a directory: a reader sees the old generation
 * or the new one and never a mixture, and a scanner killed mid-pass leaves
 * `<id>.next` half-written and `<id>` exactly as it was.
 *
 * `staging` selects which one. Readers never pass true. */
static atlas_status open_repo_dir(const char *data_dir, int64_t repo_id, bool staging, int *fd_out,
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

    char name[48];
    (void)snprintf(name, sizeof(name), "%lld%s", (long long)repo_id, staging ? ".next" : "");
    int repo = make_dir(mirror, name, err);
    (void)close(mirror);
    if (repo < 0) {
        return ATLAS_ERR_INTEGRITY;
    }
    *fd_out = repo;
    return ATLAS_OK;
}

atlas_status atlas_mirror_open_repo(const char *data_dir, int64_t repo_id, int *fd_out,
                                    atlas_err *err) {
    return open_repo_dir(data_dir, repo_id, false, fd_out, err);
}

atlas_status atlas_mirror_open_staging(const char *data_dir, int64_t repo_id, int *fd_out,
                                       atlas_err *err) {
    return open_repo_dir(data_dir, repo_id, true, fd_out, err);
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

/* Removes a directory tree beneath `parent`, by name.
 *
 * Only ever called on a mirror generation this process created, and every
 * descent is `openat` with `O_NOFOLLOW`, so a symlink planted inside a
 * generation cannot make this delete anything outside it. */
static void remove_tree(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        (void)unlinkat(parent, name, 0);
        return;
    }
    DIR *d = fdopendir(fd);
    if (d == NULL) {
        (void)close(fd);
        return;
    }
    struct dirent *e = NULL;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        struct stat sb;
        if (fstatat(dirfd(d), e->d_name, &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(sb.st_mode)) {
            remove_tree(dirfd(d), e->d_name);
        } else {
            (void)unlinkat(dirfd(d), e->d_name, 0);
        }
    }
    (void)closedir(d);
    (void)unlinkat(parent, name, AT_REMOVEDIR);
}

/* `walk_to_parent`'s read-only twin: descends an existing generation and creates
 * nothing. A missing component is an ordinary answer here rather than a failure
 * — it means the published generation does not hold this path — so it reports
 * through `*missing_out` and leaves `err` untouched. */
static atlas_status walk_to_parent_ro(int root_fd, const void *rel, size_t rel_len,
                                      int *parent_out, char *comp, bool *missing_out,
                                      atlas_err *err) {
    *parent_out = -1;
    *missing_out = false;
    if (root_fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a mirror root is required");
    }
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
        int next = openat(parent, comp, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        (void)close(parent);
        if (next < 0) {
            *missing_out = true;
            return ATLAS_OK;
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

/* One path, against roots the caller has already opened. */
static atlas_status keep_one(int cur, int stage, const void *rel, size_t rel_len, bool *kept_out,
                             atlas_err *err) {
    *kept_out = false;

    char src_comp[MIRROR_COMP_MAX + 1];
    int src_parent = -1;
    bool missing = false;
    atlas_status st = walk_to_parent_ro(cur, rel, rel_len, &src_parent, src_comp, &missing, err);
    if (st != ATLAS_OK || missing) {
        return st;
    }

    char dst_comp[MIRROR_COMP_MAX + 1];
    int dst_parent = -1;
    st = walk_to_parent(stage, rel, rel_len, &dst_parent, dst_comp, err);
    if (st != ATLAS_OK) {
        (void)close(src_parent);
        return st;
    }

    /* Flags zero, so a symlink is linked as itself rather than followed. A
     * mirrored symlink is a file the index holds like any other. */
    if (linkat(src_parent, src_comp, dst_parent, dst_comp, 0) == 0) {
        *kept_out = true;
    } else if (errno == EEXIST) {
        /* Already staged — this pass sent it, or a previous one did and died
         * after. Either way the staged generation holds the path. */
        *kept_out = true;
    } else if (errno != ENOENT) {
        int saved = errno;
        (void)close(src_parent);
        (void)close(dst_parent);
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   "cannot carry a mirrored file into the staging generation");
    }
    (void)close(src_parent);
    (void)close(dst_parent);
    return ATLAS_OK;
}

atlas_status atlas_mirror_keep_many(const char *data_dir, int64_t repo_id,
                                    const atlas_mirror_path *paths, size_t n, bool *kept_out,
                                    atlas_err *err) {
    if (kept_out == NULL || (paths == NULL && n > 0)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a keep needs paths and somewhere to answer");
    }
    for (size_t i = 0; i < n; i++) {
        kept_out[i] = false;
    }
    if (n == 0) {
        return ATLAS_OK;
    }

    /* The published generation may simply not be there — a daemon that has never
     * mirrored this repository, or one whose mirror was removed. Not an error:
     * every path answers false and the caller sends the bytes. */
    int cur = -1;
    atlas_err ignored;
    atlas_err_init(&ignored);
    if (atlas_mirror_open_repo(data_dir, repo_id, &cur, &ignored) != ATLAS_OK) {
        return ATLAS_OK;
    }
    int stage = -1;
    atlas_status st = atlas_mirror_open_staging(data_dir, repo_id, &stage, err);
    if (st != ATLAS_OK) {
        (void)close(cur);
        return st;
    }

    /* Both roots opened once for the whole batch, which is the point of the
     * batch: measured 2026-08-29, serving one request per file cost the daemon a
     * full core for half of every cycle, at about eight `openat` and nine
     * `pread64` per request — and the per-request read-only database handle,
     * which `src/ipc/server.c` opens for a reason that does not stop being true
     * just because a caller is chatty. Fewer requests, not a weaker snapshot. */
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        st = keep_one(cur, stage, paths[i].rel, paths[i].rel_len, &kept_out[i], err);
    }
    (void)close(stage);
    (void)close(cur);
    return st;
}

/* A13.1. Counts the files in one generation, and -- when `twin` is given --
 * proves every one of them is the same inode as the file at the same path
 * there. Returns false the moment either fails, so an ordinary changed pass
 * stops early rather than paying for a whole comparison it cannot pass.
 *
 * Inode equality is the whole test, and it is exact rather than heuristic: the
 * only thing that creates an entry in a staging generation without writing
 * bytes is `keep_one`'s `linkat`, which produces a second name for the *same*
 * inode. `atlas_mirror_put` and `atlas_mirror_put_symlink` always create a new
 * one. So "every staged file shares its inode with the published file at that
 * path" is precisely "this pass wrote nothing", with no flag to keep in sync
 * and nothing for a caller to get wrong.
 *
 * Symlinks are compared as themselves: `keep_one` links a symlink with flags
 * zero and `fstatat` is asked with `AT_SYMLINK_NOFOLLOW`, so a mirrored link
 * answers for its own inode and never its target's. */
static bool count_and_match(int dir_fd, int twin_fd, size_t *count_out) {
    DIR *d = fdopendir(dir_fd);
    if (d == NULL) {
        (void)close(dir_fd);
        return false;
    }
    bool ok = true;
    struct dirent *e = NULL;
    while (ok && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        struct stat sb;
        if (fstatat(dirfd(d), e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR(sb.st_mode)) {
            int sub = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (sub < 0) {
                ok = false;
                break;
            }
            int twin_sub = -1;
            if (twin_fd >= 0) {
                twin_sub = openat(twin_fd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                                          O_NOFOLLOW);
                if (twin_sub < 0) {
                    (void)close(sub);
                    ok = false;
                    break;
                }
            }
            ok = count_and_match(sub, twin_sub, count_out);
            continue;
        }
        if (twin_fd >= 0) {
            struct stat tb;
            if (fstatat(twin_fd, e->d_name, &tb, AT_SYMLINK_NOFOLLOW) != 0 ||
                tb.st_ino != sb.st_ino || tb.st_dev != sb.st_dev) {
                ok = false;
                break;
            }
        }
        (*count_out)++;
    }
    (void)closedir(d);
    if (twin_fd >= 0) {
        (void)close(twin_fd);
    }
    return ok;
}

/* True when publishing `next` would put back exactly what `cur` already holds.
 *
 * Two questions, and both are needed. Inode equality proves the staged
 * generation wrote nothing *new*; equal file counts prove it dropped nothing.
 * Without the second a pass that deleted a file would compare identical and the
 * deletion would never reach the index. */
static bool staged_is_the_published_one(int mirror, const char *cur, const char *next) {
    int next_fd = openat(mirror, next, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next_fd < 0) {
        return false;
    }
    int cur_fd = openat(mirror, cur, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (cur_fd < 0) {
        (void)close(next_fd);
        return false;
    }
    size_t staged = 0;
    if (!count_and_match(next_fd, cur_fd, &staged)) {
        return false;
    }
    int cur_again = openat(mirror, cur, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (cur_again < 0) {
        return false;
    }
    size_t published = 0;
    if (!count_and_match(cur_again, -1, &published)) {
        return false;
    }
    return staged == published;
}

atlas_status atlas_mirror_publish(const char *data_dir, int64_t repo_id, bool *published_out,
                                  atlas_err *err) {
    if (published_out != NULL) {
        *published_out = false;
    }
    if (data_dir == NULL || data_dir[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a data directory is required");
    }
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

    char cur[48], next[48], old[48];
    (void)snprintf(cur, sizeof(cur), "%lld", (long long)repo_id);
    (void)snprintf(next, sizeof(next), "%lld.next", (long long)repo_id);
    (void)snprintf(old, sizeof(old), "%lld.old", (long long)repo_id);

    /* A leftover from a publish that died between the two renames. */
    remove_tree(mirror, old);

    /* Nothing staged means nothing to publish, which is not an error: a pass
     * that mirrored no files at all still reports its verdict. */
    struct stat sb;
    if (fstatat(mirror, next, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        (void)close(mirror);
        return ATLAS_OK;
    }

    bool had_current = fstatat(mirror, cur, &sb, AT_SYMLINK_NOFOLLOW) == 0;

    /* A13.1. A pass that changed nothing publishes nothing.
     *
     * The swap is not free the way it looks. `remove_tree` unlinks the outgoing
     * generation, so every directory the watcher holds an inotify watch on is
     * gone -- a watch is on an inode, not on a path -- and the watcher must drop
     * and rebuild every watch. P0 says a rebuild owes every repository an event
     * gap, an event gap makes the next pass `full`, and `reconcile.c` sets
     * `need_hash` for a full pass before it looks at any stored identity. So an
     * unchanged repository was re-hashed end to end every few minutes, and the
     * cache could not prevent it because it was never consulted.
     *
     * Discarding the staged twin instead costs the same syscalls -- the same
     * number of unlinks, on the other tree -- and keeps the published inodes
     * alive, which is the entire difference. Nothing about generation atomicity
     * moves: `<id>` is never partially written either way, and there is still no
     * delete sweep anywhere.
     *
     * The identity is proved, not assumed. See `staged_is_the_published_one`. */
    if (had_current && staged_is_the_published_one(mirror, cur, next)) {
        remove_tree(mirror, next);
        (void)close(mirror);
        return ATLAS_OK;
    }

    /* Move the current generation aside, then the staged one into place. A
     * reader between the two finds no mirror and refuses, which is correct and
     * two syscalls wide. */
    if (had_current && renameat(mirror, cur, mirror, old) != 0) {
        int saved = errno;
        (void)close(mirror);
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   "cannot move the current mirror aside");
    }
    if (renameat(mirror, next, mirror, cur) != 0) {
        int saved = errno;
        /* Put the old one back rather than leaving the repository with none. */
        if (had_current) {
            (void)renameat(mirror, old, mirror, cur);
        }
        (void)close(mirror);
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   "cannot publish the staged mirror");
    }
    remove_tree(mirror, old);
    (void)close(mirror);
    if (published_out != NULL) {
        *published_out = true;
    }
    return ATLAS_OK;
}

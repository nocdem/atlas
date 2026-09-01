/* Atlas - A12.1 T6: reading a registered memory source, by the principal that
 * can actually read it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A registered source (`memory_sources`, migration 29) names bytes; it does
 * not hand them over. This file is the one place that turns a source into the
 * bytes it names.
 *
 * For a `REPO_*` source that is A13's own question, already answered:
 * `atlas_repo_open_git` decides whether this process reads a repository's
 * tree directly or its scanner's mirror, and this file asks rather than
 * deciding again -- `atlas_memory_read_source` never opens the tree itself,
 * never stats it to choose, and never falls back to it when a mirror is
 * missing or incomplete. That design was tried and reverted for a measured
 * reason (see CLAUDE.md's A13 section): a rule keyed on "could not open"
 * answers neither of the two ways a read can fail *partially*, and the fix was
 * to stop asking the tree the question at all.
 *
 * For an `EXTERNAL_*` source the daemon has no filesystem path to reach at
 * all -- it names something outside every registered repository -- so
 * `atlas_memory_read_source` reads nothing for one and reports
 * `ATLAS_MEMORY_READ_NOT_OURS`. `atlas_memory_read_external` is the other
 * entry point: the one a caller who *is* the right principal (the operator's
 * own CLI) uses to read such a path directly.
 *
 * Every path this file opens, tracked or not, is opened with
 * `atlas_path_open_nofollow` / `atlas_path_opendir_nofollow`: a symlink
 * anywhere in a registered path -- the source's own path, an intermediate
 * directory, or a directory source's child entry -- is refused and never
 * followed. A git-tracked file is the one exception to "never opens the tree
 * itself": its content is read through `git cat-file` against the same root
 * `atlas_repo_open_git` returned, which reads a git object rather than
 * touching the filesystem a second time -- there is no symlink to follow in
 * a blob's bytes, only in the path used to find it, and that path was already
 * validated on the filesystem before git was ever asked about it.
 */
#include "atlas/memory.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/limits.h"
#include "atlas/mirror.h"
#include "atlas/pathrep.h"

void atlas_memory_read_item_init(atlas_memory_read_item *it) {
    memset(it, 0, sizeof(*it));
    atlas_buf_init(&it->rel_path);
    atlas_buf_init(&it->bytes);
    atlas_buf_init(&it->blob_oid);
    atlas_buf_init(&it->commit_oid);
}

void atlas_memory_read_item_free(atlas_memory_read_item *it) {
    if (it == NULL) {
        return;
    }
    atlas_buf_free(&it->rel_path);
    atlas_buf_free(&it->bytes);
    atlas_buf_free(&it->blob_oid);
    atlas_buf_free(&it->commit_oid);
}

/* --- one open, validated file -------------------------------------------- */

/* Reads `fd` to EOF into `out`, which is reset first. `fd` is a regular file
 * already opened O_NOFOLLOW by the caller; nothing here re-checks its type. */
static atlas_status read_fd_into_buf(int fd, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    unsigned char chunk[8192];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                       "read failed while reading a memory source");
        }
        if (n == 0) {
            break;
        }
        atlas_status st = atlas_buf_append(out, chunk, (size_t)n, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* Opens `path` under `root_fd`, refusing every symlink and enforcing the size
 * bound -- the two checks every source, tracked or not, must pass before its
 * content is worth reading at all.
 *
 * On `ATLAS_MEMORY_READ_OK` the caller owns `*fd_out` (a validated, in-bound
 * regular file) and must close() it. Every other outcome has already closed
 * anything it opened and left `*fd_out == -1`; there is nothing left for the
 * caller to do but record the outcome. */
static atlas_status open_fs_file(int root_fd, const void *path, size_t path_len,
                                 atlas_memory_read_outcome *outcome_out, int *fd_out,
                                 atlas_err *err) {
    *fd_out = -1;
    *outcome_out = ATLAS_MEMORY_READ_UNKNOWN;

    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    int open_errno = 0;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    atlas_status st = atlas_path_open_nofollow(root_fd, (const char *)path, path_len, &res, &fd,
                                               &sb, &open_errno, err);
    if (st != ATLAS_OK) {
        return st;
    }
    switch (res) {
    case ATLAS_PATH_OPEN_SYMLINK:
    case ATLAS_PATH_OPEN_UNSAFE:
        /* The final component is a symlink, or a component before it is: both
         * are "a symlink stood in this path", refused rather than followed
         * either way. */
        *outcome_out = ATLAS_MEMORY_READ_SYMLINK;
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_MISSING:
    case ATLAS_PATH_OPEN_NOT_REGULAR:
        /* Not there, or there but not a file this layer reads (a directory
         * masquerading as a FILE source, a device, a fifo): both report as
         * "nothing to read", not as an error. */
        *outcome_out = ATLAS_MEMORY_READ_ABSENT;
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_DENIED:
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, open_errno,
                                   "a memory source could not be opened");
    case ATLAS_PATH_OPEN_OK:
        break;
    }

    /* A bound that is reached is refused, never trimmed -- this season's own
     * rule, ATLAS_MEMORY_MAX_SOURCE_BYTES' own comment. Checked from the same
     * lstat atlas_path_open_nofollow already took, so this costs no extra
     * syscall in the ordinary case. */
    uint64_t limit = ATLAS_MEMORY_MAX_SOURCE_BYTES;
    if (sb.st_size < 0 || (uint64_t)sb.st_size > limit) {
        (void)close(fd);
        *outcome_out = ATLAS_MEMORY_READ_TOO_LARGE;
        return ATLAS_OK;
    }

    *fd_out = fd;
    *outcome_out = ATLAS_MEMORY_READ_OK;
    return ATLAS_OK;
}

/* Reads one path under `root_fd` as plain filesystem bytes: no git, blob_oid
 * and commit_oid left empty. Used for EXTERNAL_* sources and for every entry
 * of a REPO_DIR/EXTERNAL_DIR listing -- a directory source is a filesystem
 * convenience, not a git operation, so its children are never checked against
 * a tree. */
static atlas_status read_fs_file(int root_fd, const void *path, size_t path_len,
                                 atlas_memory_read_item *item, atlas_err *err) {
    int fd = -1;
    atlas_memory_read_outcome outcome = ATLAS_MEMORY_READ_UNKNOWN;
    atlas_status st = open_fs_file(root_fd, path, path_len, &outcome, &fd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    item->outcome = outcome;
    if (outcome != ATLAS_MEMORY_READ_OK) {
        return ATLAS_OK;
    }
    st = read_fd_into_buf(fd, &item->bytes, err);
    (void)close(fd);
    if (st != ATLAS_OK) {
        item->outcome = ATLAS_MEMORY_READ_UNKNOWN;
        return st;
    }
    return ATLAS_OK;
}

/* Reads a REPO_FILE source's current bytes. The filesystem check (symlink,
 * size) runs first and unconditionally, against whichever root
 * atlas_repo_open_git returned -- tree or mirror, this file cannot tell and
 * does not need to. Only when that passes does tracked-ness matter: git is
 * canonical for "is this the same bytes HEAD has", and a tracked path is read
 * through `cat-file` against the resolved blob rather than through the
 * filesystem handle already open on it, so blob_oid and commit_oid describe
 * exactly the bytes returned. An untracked path -- including one that exists
 * only because nobody has committed it yet -- falls back to that same open
 * handle, with both oids left empty. */
static atlas_status read_repo_file(atlas_git *g, const void *path_raw, size_t path_len,
                                   atlas_memory_read_item *item, atlas_err *err) {
    int fd = -1;
    atlas_memory_read_outcome outcome = ATLAS_MEMORY_READ_UNKNOWN;
    atlas_status st = open_fs_file(atlas_git_root_fd(g), path_raw, path_len, &outcome, &fd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    item->outcome = outcome;
    if (outcome != ATLAS_MEMORY_READ_OK) {
        return ATLAS_OK;
    }

    atlas_git_head head;
    memset(&head, 0, sizeof(head));
    st = atlas_git_read_head(g, &head, err);
    if (st != ATLAS_OK) {
        (void)close(fd);
        item->outcome = ATLAS_MEMORY_READ_UNKNOWN;
        return st;
    }

    bool tracked = false;
    atlas_buf oid = ATLAS_BUF_INIT;
    if (head.oid[0] != '\0') {
        /* An unborn repository has no HEAD to resolve against, so every path
         * in it is untracked by definition -- there is no tree yet. */
        st = atlas_git_blob_oid_at(g, head.oid, path_raw, path_len, &oid, &tracked, err);
        if (st != ATLAS_OK) {
            (void)close(fd);
            atlas_buf_free(&oid);
            item->outcome = ATLAS_MEMORY_READ_UNKNOWN;
            return st;
        }
    }

    if (tracked) {
        /* Content comes from the git object now, not the checkout: the
         * filesystem handle already did its job (proving the path is not a
         * symlink and is in bound) and is no longer needed. */
        (void)close(fd);
        st = atlas_git_cat_blob(g, atlas_buf_cstr(&oid), atlas_proc_sink_buf, &item->bytes,
                                (size_t)ATLAS_MEMORY_MAX_SOURCE_BYTES, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&item->blob_oid, atlas_buf_cstr(&oid), oid.len, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&item->commit_oid, head.oid, strlen(head.oid), err);
        }
        atlas_buf_free(&oid);
        if (st != ATLAS_OK) {
            item->outcome = ATLAS_MEMORY_READ_UNKNOWN;
            return st;
        }
    } else {
        atlas_buf_free(&oid);
        st = read_fd_into_buf(fd, &item->bytes, err);
        (void)close(fd);
        if (st != ATLAS_OK) {
            item->outcome = ATLAS_MEMORY_READ_UNKNOWN;
            return st;
        }
    }
    item->outcome = ATLAS_MEMORY_READ_OK;
    return ATLAS_OK;
}

/* --- directory sources: one level, bounded, sorted ------------------------ */

/* One name row. Sized for an ordinary filename; a name that would not fit is
 * excluded from the candidate list rather than truncated -- the same "refused,
 * never trimmed" rule the byte bound follows, applied to a name nothing
 * realistic produces anyway. */
#define MEMDIR_NAME_MAX 256u

static int cmp_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Lists a directory source's children: one level, no descent, `.md` names
 * only, sorted, bounded by `cap`. If the source's own path does not open as a
 * directory at all (absent, a symlink, something else entirely), exactly one
 * item is produced describing that -- the same contract a FILE source has,
 * rather than a silently empty listing a caller cannot tell apart from a
 * directory that genuinely holds nothing.
 *
 * A directory holding more than `cap` matching files is refused outright
 * (ATLAS_MEMORY_MAX_DIR_ENTRIES' own "refused, never trimmed" rule): keeping
 * the alphabetically-first `cap` of them would make which files count depend
 * on how many other files happen to sit beside them, silently, on every
 * pass. */
static atlas_status read_dir_entries(int root_fd, const void *path_raw, size_t path_len,
                                     atlas_memory_read_item *items, size_t cap,
                                     size_t *count_out, atlas_err *err) {
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int dir_fd = -1;
    int open_errno = 0;
    atlas_status st = atlas_path_opendir_nofollow(root_fd, (const char *)path_raw, path_len, &res,
                                                  &dir_fd, NULL, &open_errno, err);
    if (st != ATLAS_OK) {
        return st;
    }
    switch (res) {
    case ATLAS_PATH_OPEN_SYMLINK:
    case ATLAS_PATH_OPEN_UNSAFE:
        atlas_memory_read_item_init(&items[0]);
        items[0].outcome = ATLAS_MEMORY_READ_SYMLINK;
        *count_out = 1;
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_MISSING:
    case ATLAS_PATH_OPEN_NOT_REGULAR:
        atlas_memory_read_item_init(&items[0]);
        items[0].outcome = ATLAS_MEMORY_READ_ABSENT;
        *count_out = 1;
        return ATLAS_OK;
    case ATLAS_PATH_OPEN_DENIED:
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, open_errno,
                                   "a memory directory could not be opened");
    case ATLAS_PATH_OPEN_OK:
        break;
    }

    DIR *d = fdopendir(dir_fd);
    if (d == NULL) {
        int e = errno;
        (void)close(dir_fd);
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, e, "cannot read a memory directory");
    }

    /* Heap rather than a stack array of unknown size: `cap` is a caller
     * parameter, not a compile-time constant, and a VLA is refused outright by
     * this project's own warning policy (-Wvla). */
    char(*names)[MEMDIR_NAME_MAX] = malloc((cap == 0 ? 1u : cap) * sizeof(*names));
    if (names == NULL) {
        (void)closedir(d);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory reading a memory directory");
    }

    size_t suflen = strlen(ATLAS_MEMORY_DIR_SUFFIX);
    size_t count = 0;
    bool overflow = false;
    bool read_error = false;
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(d);
        if (ent == NULL) {
            if (errno != 0) {
                read_error = true;
            }
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen < suflen || memcmp(ent->d_name + (nlen - suflen), ATLAS_MEMORY_DIR_SUFFIX,
                                    suflen) != 0) {
            continue; /* not a `.md` name: not a memory file, by this class's own rule */
        }
        struct stat cst;
        if (fstatat(dirfd(d), ent->d_name, &cst, AT_SYMLINK_NOFOLLOW) != 0) {
            continue; /* raced away, or unreadable metadata: a best-effort listing skips it */
        }
        if (S_ISDIR(cst.st_mode)) {
            continue; /* a directory is never a memory file, whatever it is named */
        }
        if (nlen + 1u > MEMDIR_NAME_MAX) {
            continue; /* cannot be represented in one row; not a realistic name */
        }
        if (count >= cap) {
            overflow = true;
            break;
        }
        memcpy(names[count], ent->d_name, nlen + 1u);
        count++;
    }

    if (read_error) {
        int e = errno;
        (void)closedir(d);
        free(names);
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, e,
                                   "a memory directory could not be read to the end");
    }

    if (overflow) {
        (void)closedir(d);
        free(names);
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "a memory directory holds more than %zu matching files; refused "
                             "rather than trimmed",
                             cap);
    }

    /* Sorted before any content is read, so the *kept* set (there is no
     * trimming here, but a later, smaller cap could still apply) and its
     * order never depend on readdir()'s arbitrary return order. */
    qsort(names, count, sizeof(*names), cmp_name);

    size_t produced = 0;
    atlas_status rst = ATLAS_OK;
    for (size_t i = 0; i < count; i++) {
        atlas_memory_read_item *it = &items[i];
        atlas_memory_read_item_init(it);
        size_t nlen = strlen(names[i]);
        rst = atlas_buf_set(&it->rel_path, names[i], nlen, err);
        if (rst == ATLAS_OK) {
            rst = read_fs_file(dirfd(d), names[i], nlen, it, err);
        }
        if (rst != ATLAS_OK) {
            break;
        }
        produced++;
    }
    (void)closedir(d); /* also closes dir_fd */
    free(names);

    if (rst != ATLAS_OK) {
        for (size_t i = 0; i < produced; i++) {
            atlas_memory_read_item_free(&items[i]);
        }
        if (produced < count) {
            atlas_memory_read_item_free(&items[produced]);
        }
        *count_out = 0;
        return rst;
    }
    *count_out = produced;
    return ATLAS_OK;
}

/* --- entry points ----------------------------------------------------------- */

atlas_status atlas_memory_read_source(const atlas_repo_info *repo, const char *data_dir,
                                      atlas_memory_source_class cls, const void *path_raw,
                                      size_t path_len, atlas_memory_read_item *items,
                                      size_t cap, size_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (repo == NULL || path_raw == NULL || path_len == 0 || items == NULL || cap == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a memory source read needs a repository, a path and room for a "
                             "result");
    }
    if (cls == ATLAS_MEMORY_SOURCE_UNKNOWN) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "a memory source class is required");
    }
    if (!atlas_memory_source_class_is_repo(cls)) {
        /* EXTERNAL_*: a different principal reads it, through
         * atlas_memory_read_external -- never this one, and never the tree
         * this process happens to be able to see. */
        atlas_memory_read_item_init(&items[0]);
        items[0].outcome = ATLAS_MEMORY_READ_NOT_OURS;
        if (count_out != NULL) {
            *count_out = 1;
        }
        return ATLAS_OK;
    }

    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_status st = atlas_repo_open_git(repo, data_dir, &g, &from_mirror, err);
    if (st != ATLAS_OK) {
        /* A13's own refusal, read from the same row fields
         * atlas_repo_open_git already consulted, and reported through this
         * vocabulary's own outcome instead of a status failure: a scanner is
         * named, this process is not it, and no complete mirror exists yet.
         * atlas_repo_open_git remains the one place that decides *where* a
         * repository is read from -- this only names why it just refused, so
         * a caller sees "wait for the scanner" rather than a bare error. */
        if (repo->scanner_uid != 0 && (int64_t)geteuid() != repo->scanner_uid &&
            !repo->mirror_complete) {
            atlas_err_init(err);
            atlas_memory_read_item_init(&items[0]);
            items[0].outcome = ATLAS_MEMORY_READ_NO_MIRROR;
            if (count_out != NULL) {
                *count_out = 1;
            }
            return ATLAS_OK;
        }
        return st;
    }

    if (cls == ATLAS_MEMORY_SOURCE_REPO_FILE) {
        atlas_memory_read_item_init(&items[0]);
        st = read_repo_file(g, path_raw, path_len, &items[0], err);
        if (st == ATLAS_OK) {
            if (count_out != NULL) {
                *count_out = 1;
            }
        } else {
            atlas_memory_read_item_free(&items[0]);
        }
    } else {
        size_t produced = 0;
        st = read_dir_entries(atlas_git_root_fd(g), path_raw, path_len, items, cap, &produced,
                              err);
        if (st == ATLAS_OK && count_out != NULL) {
            *count_out = produced;
        }
    }
    atlas_git_close(g);
    return st;
}

atlas_status atlas_memory_read_external(const void *path_raw, size_t path_len, bool is_dir,
                                        atlas_memory_read_item *items, size_t cap,
                                        size_t *count_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (path_raw == NULL || path_len < 2u || ((const char *)path_raw)[0] != '/' ||
        items == NULL || cap == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "an external memory source needs an absolute path and room for a "
                             "result");
    }

    int root_fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot open /");
    }

    /* Everything past the leading '/' is a root-relative path, so the same
     * symlink-refusing walk atlas_path_open_nofollow/atlas_path_opendir_nofollow
     * already implement for a repository root applies unchanged here, rooted
     * at "/" instead. */
    const char *rel = (const char *)path_raw + 1;
    size_t rel_len = path_len - 1u;

    atlas_status st;
    if (is_dir) {
        size_t produced = 0;
        st = read_dir_entries(root_fd, rel, rel_len, items, cap, &produced, err);
        if (st == ATLAS_OK && count_out != NULL) {
            *count_out = produced;
        }
    } else {
        atlas_memory_read_item_init(&items[0]);
        st = read_fs_file(root_fd, rel, rel_len, &items[0], err);
        if (st == ATLAS_OK) {
            if (count_out != NULL) {
                *count_out = 1;
            }
        } else {
            atlas_memory_read_item_free(&items[0]);
        }
    }
    (void)close(root_fd);
    return st;
}

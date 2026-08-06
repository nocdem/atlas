/* Atlas - byte-safe path representation and symlink-refusing traversal.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Path text encoding is the safe-text encoding: one rule covers every untrusted
 * string Atlas prints, so a filename cannot carry a terminal escape while a
 * commit subject is escaped, or the reverse. See atlas/safetext.h.
 */
#include "atlas/pathrep.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "atlas/safetext.h"

#define ATLAS_COMPONENT_MAX 255u

atlas_status atlas_path_text_encode(const void *raw, size_t n, atlas_buf *out, atlas_err *err) {
    return atlas_text_encode_safe(raw, n, out, err);
}

atlas_status atlas_path_text_decode(const char *text, size_t n, atlas_buf *out, atlas_err *err) {
    return atlas_text_decode_safe(text, n, out, err);
}

bool atlas_path_is_plain(const void *raw, size_t n) {
    return atlas_text_is_safe(raw, n);
}

atlas_status atlas_path_check_relative(const void *raw, size_t n, atlas_err *err) {
    const unsigned char *p = (const unsigned char *)raw;
    if (n == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "empty path");
    }
    if (p[0] == '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "absolute paths are not repository paths");
    }
    if (p[n - 1u] == '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "path has a trailing slash");
    }
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && p[i] == '\0') {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "path contains a NUL byte");
        }
        if (i == n || p[i] == '/') {
            size_t clen = i - start;
            if (clen == 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "path has an empty component");
            }
            if (clen > ATLAS_COMPONENT_MAX) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "path component exceeds %u bytes",
                                     ATLAS_COMPONENT_MAX);
            }
            if (clen == 1u && p[start] == '.') {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "path contains a '.' component");
            }
            if (clen == 2u && p[start] == '.' && p[start + 1u] == '.') {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "path contains a '..' component");
            }
            start = i + 1u;
        }
    }
    return ATLAS_OK;
}

/* Open the directory holding the final component, refusing to traverse any
 * symlink. On success the caller owns the returned directory fd, and the last
 * component is reported through the `last` and `last_len` outputs. */
static atlas_status walk_to_parent(int root_fd, const char *rel, size_t rel_len, int *dirfd_out,
                                   const char **last, size_t *last_len,
                                   atlas_path_open_result *result_out, int *errno_out,
                                   atlas_err *err) {
    *dirfd_out = -1;
    *result_out = ATLAS_PATH_OPEN_OK;
    if (errno_out != NULL) {
        *errno_out = 0;
    }
    atlas_status st = atlas_path_check_relative(rel, rel_len, err);
    if (st != ATLAS_OK) {
        return st;
    }

    int dirfd = dup(root_fd);
    if (dirfd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot duplicate root fd");
    }

    size_t pos = 0;
    for (;;) {
        const char *slash = memchr(rel + pos, '/', rel_len - pos);
        if (slash == NULL) {
            *last = rel + pos;
            *last_len = rel_len - pos;
            *dirfd_out = dirfd;
            return ATLAS_OK;
        }
        size_t clen = (size_t)(slash - (rel + pos));
        char comp[ATLAS_COMPONENT_MAX + 1u];
        memcpy(comp, rel + pos, clen);
        comp[clen] = '\0';

        struct stat cst;
        if (fstatat(dirfd, comp, &cst, AT_SYMLINK_NOFOLLOW) != 0) {
            int e = errno;
            (void)close(dirfd);
            if (errno_out != NULL) {
                *errno_out = e;
            }
            *result_out = (e == ENOENT || e == ENOTDIR) ? ATLAS_PATH_OPEN_MISSING
                                                        : ATLAS_PATH_OPEN_DENIED;
            return ATLAS_OK;
        }
        if (S_ISLNK(cst.st_mode)) {
            (void)close(dirfd);
            *result_out = ATLAS_PATH_OPEN_UNSAFE;
            return ATLAS_OK;
        }
        if (!S_ISDIR(cst.st_mode)) {
            (void)close(dirfd);
            *result_out = ATLAS_PATH_OPEN_NOT_REGULAR;
            return ATLAS_OK;
        }
        int next = openat(dirfd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int e = errno;
        (void)close(dirfd);
        if (next < 0) {
            if (errno_out != NULL) {
                *errno_out = e;
            }
            *result_out = (e == ELOOP) ? ATLAS_PATH_OPEN_UNSAFE : ATLAS_PATH_OPEN_DENIED;
            return ATLAS_OK;
        }
        dirfd = next;
        pos += clen + 1u;
    }
}

atlas_status atlas_path_open_nofollow(int root_fd, const char *rel, size_t rel_len,
                                      atlas_path_open_result *result_out, int *fd_out,
                                      struct stat *st_out, int *errno_out, atlas_err *err) {
    if (fd_out != NULL) {
        *fd_out = -1;
    }
    int dirfd = -1;
    const char *last = NULL;
    size_t last_len = 0;
    atlas_status st =
        walk_to_parent(root_fd, rel, rel_len, &dirfd, &last, &last_len, result_out, errno_out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (*result_out != ATLAS_PATH_OPEN_OK) {
        return ATLAS_OK;
    }

    char comp[ATLAS_COMPONENT_MAX + 1u];
    memcpy(comp, last, last_len);
    comp[last_len] = '\0';

    struct stat lst;
    if (fstatat(dirfd, comp, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = errno;
        (void)close(dirfd);
        if (errno_out != NULL) {
            *errno_out = e;
        }
        *result_out =
            (e == ENOENT || e == ENOTDIR) ? ATLAS_PATH_OPEN_MISSING : ATLAS_PATH_OPEN_DENIED;
        return ATLAS_OK;
    }
    if (st_out != NULL) {
        *st_out = lst;
    }
    if (S_ISLNK(lst.st_mode)) {
        (void)close(dirfd);
        *result_out = ATLAS_PATH_OPEN_SYMLINK;
        return ATLAS_OK;
    }
    if (!S_ISREG(lst.st_mode)) {
        (void)close(dirfd);
        *result_out = ATLAS_PATH_OPEN_NOT_REGULAR;
        return ATLAS_OK;
    }
    int fd = openat(dirfd, comp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int e = errno;
    (void)close(dirfd);
    if (fd < 0) {
        if (errno_out != NULL) {
            *errno_out = e;
        }
        *result_out = (e == ELOOP) ? ATLAS_PATH_OPEN_UNSAFE : ATLAS_PATH_OPEN_DENIED;
        return ATLAS_OK;
    }
    if (fd_out != NULL) {
        *fd_out = fd;
    } else {
        (void)close(fd);
    }
    *result_out = ATLAS_PATH_OPEN_OK;
    return ATLAS_OK;
}

atlas_status atlas_path_readlink_at(int root_fd, const char *rel, size_t rel_len,
                                    atlas_buf *target_out, atlas_path_open_result *result_out,
                                    atlas_err *err) {
    int dirfd = -1;
    const char *last = NULL;
    size_t last_len = 0;
    atlas_status st =
        walk_to_parent(root_fd, rel, rel_len, &dirfd, &last, &last_len, result_out, NULL, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (*result_out != ATLAS_PATH_OPEN_OK) {
        return ATLAS_OK;
    }

    char comp[ATLAS_COMPONENT_MAX + 1u];
    memcpy(comp, last, last_len);
    comp[last_len] = '\0';

    struct stat lst;
    if (fstatat(dirfd, comp, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = errno;
        (void)close(dirfd);
        *result_out =
            (e == ENOENT || e == ENOTDIR) ? ATLAS_PATH_OPEN_MISSING : ATLAS_PATH_OPEN_DENIED;
        return ATLAS_OK;
    }
    if (!S_ISLNK(lst.st_mode)) {
        (void)close(dirfd);
        *result_out = ATLAS_PATH_OPEN_NOT_REGULAR;
        return ATLAS_OK;
    }

    /* st_size is advisory for symlinks; grow until readlinkat stops filling. */
    size_t cap = (lst.st_size > 0) ? (size_t)lst.st_size + 1u : 256u;
    for (;;) {
        atlas_buf_reset(target_out);
        st = atlas_buf_reserve(target_out, cap, err);
        if (st != ATLAS_OK) {
            (void)close(dirfd);
            return st;
        }
        ssize_t n = readlinkat(dirfd, comp, target_out->data, cap);
        if (n < 0) {
            int e = errno;
            (void)close(dirfd);
            return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, e, "cannot read symlink target");
        }
        if ((size_t)n < cap) {
            target_out->len = (size_t)n;
            target_out->data[target_out->len] = '\0';
            (void)close(dirfd);
            *result_out = ATLAS_PATH_OPEN_SYMLINK;
            return ATLAS_OK;
        }
        if (cap > (1u << 20)) {
            (void)close(dirfd);
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "symlink target exceeds 1 MiB");
        }
        cap *= 2u;
    }
}

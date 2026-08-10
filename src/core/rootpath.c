/* Atlas - opening a file that only root may have written.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/rootpath.h. Moved here from `src/core/authority.c` in A7.1, byte
 * for byte in behaviour, so that the operator policy and the system-deployment
 * policy ask the same code the same question.
 */
#define _GNU_SOURCE 1

#include "atlas/rootpath.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const char *atlas_rootpath_result_name(atlas_rootpath_result r) {
    switch (r) {
    case ATLAS_ROOTPATH_UNKNOWN: return "UNKNOWN";
    case ATLAS_ROOTPATH_OK: return "OK";
    case ATLAS_ROOTPATH_MISSING: return "MISSING";
    case ATLAS_ROOTPATH_SYMLINK: return "SYMLINK";
    case ATLAS_ROOTPATH_WRITABLE: return "WRITABLE";
    case ATLAS_ROOTPATH_BAD_PATH: return "BAD_PATH";
    case ATLAS_ROOTPATH_NOT_REGULAR: return "NOT_REGULAR";
    case ATLAS_ROOTPATH_NOT_DIRECTORY: return "NOT_DIRECTORY";
    }
    return "UNKNOWN";
}

static void set_detail(char *detail, size_t size, const char *text) {
    if (detail != NULL && size > 0) {
        (void)snprintf(detail, size, "%s", text != NULL ? text : "");
    }
}

/* Root-owned and no non-root writer. */
static bool root_owned_and_unwritable(const struct stat *sb) {
    return sb->st_uid == 0u && (sb->st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

int atlas_rootpath_open(const char *path, bool want_dir, atlas_rootpath_result *result_out,
                        char *detail, size_t detail_size) {
    atlas_rootpath_result ignored = ATLAS_ROOTPATH_UNKNOWN;
    if (result_out == NULL) {
        result_out = &ignored;
    }
    *result_out = ATLAS_ROOTPATH_UNKNOWN;
    set_detail(detail, detail_size, path);

    if (path == NULL || path[0] != '/') {
        *result_out = ATLAS_ROOTPATH_BAD_PATH;
        return -1;
    }

    int dir = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        *result_out = ATLAS_ROOTPATH_MISSING;
        set_detail(detail, detail_size, "/");
        return -1;
    }
    struct stat sb;
    if (fstat(dir, &sb) != 0 || !root_owned_and_unwritable(&sb)) {
        /* A machine whose `/` is not root-owned is not one Atlas can reason
         * about at all. */
        *result_out = ATLAS_ROOTPATH_WRITABLE;
        set_detail(detail, detail_size, "/");
        (void)close(dir);
        return -1;
    }

    const char *p = path + 1;
    char component[256];
    char sofar[PATH_MAX];
    size_t sofar_len = 0;
    sofar[0] = '\0';

    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
        /* An empty component (`//` or a trailing slash), `.` and `..` are all
         * refused rather than normalised: Atlas is not in the business of
         * guessing what a malformed absolute path meant, and `..` in
         * particular would let a caller re-enter a directory this walk has
         * already accepted. */
        if (n == 0 || n >= sizeof(component)) {
            *result_out = ATLAS_ROOTPATH_BAD_PATH;
            set_detail(detail, detail_size, path);
            (void)close(dir);
            return -1;
        }
        memcpy(component, p, n);
        component[n] = '\0';
        if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
            *result_out = ATLAS_ROOTPATH_BAD_PATH;
            set_detail(detail, detail_size, path);
            (void)close(dir);
            return -1;
        }
        if (sofar_len + 1u + n < sizeof(sofar)) {
            sofar[sofar_len++] = '/';
            memcpy(sofar + sofar_len, component, n);
            sofar_len += n;
            sofar[sofar_len] = '\0';
        }

        bool last = (slash == NULL) || (slash[1] == '\0');
        /* O_NOFOLLOW turns a symlink component into ELOOP rather than a
         * traversal, which is the whole reason this loop exists. */
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
        if (!last || want_dir) {
            flags |= O_DIRECTORY;
        }
        int next = openat(dir, component, flags);
        int saved = errno;
        (void)close(dir);
        if (next < 0) {
            if (saved == ELOOP) {
                *result_out = ATLAS_ROOTPATH_SYMLINK;
            } else if (saved == ENOENT) {
                *result_out = ATLAS_ROOTPATH_MISSING;
            } else if (saved == ENOTDIR) {
                *result_out = last && want_dir ? ATLAS_ROOTPATH_NOT_DIRECTORY
                                               : ATLAS_ROOTPATH_BAD_PATH;
            } else {
                /* EACCES and everything else. Unreadable is not permission. */
                *result_out = ATLAS_ROOTPATH_WRITABLE;
            }
            set_detail(detail, detail_size, sofar);
            return -1;
        }
        if (fstat(next, &sb) != 0 || !root_owned_and_unwritable(&sb)) {
            *result_out = ATLAS_ROOTPATH_WRITABLE;
            set_detail(detail, detail_size, sofar);
            (void)close(next);
            return -1;
        }
        if (last) {
            if (want_dir ? !S_ISDIR(sb.st_mode) : !S_ISREG(sb.st_mode)) {
                *result_out = want_dir ? ATLAS_ROOTPATH_NOT_DIRECTORY : ATLAS_ROOTPATH_NOT_REGULAR;
                set_detail(detail, detail_size, sofar);
                (void)close(next);
                return -1;
            }
            *result_out = ATLAS_ROOTPATH_OK;
            set_detail(detail, detail_size, sofar);
            return next;
        }
        dir = next;
        p = slash + 1;
    }
    *result_out = ATLAS_ROOTPATH_BAD_PATH;
    set_detail(detail, detail_size, path);
    (void)close(dir);
    return -1;
}

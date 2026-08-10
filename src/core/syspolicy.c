/* Atlas - the system-deployment policy: who may speak to a shared daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/syspolicy.h for what a policy establishes and what it does not.
 *
 * The whole file fails closed. There is one `out->state = ATLAS_SYSPOLICY_SYSTEM`
 * assignment, it is the last statement of the loader, and every path that does
 * not reach it leaves the struct as `memset` left it — which is legacy
 * per-user mode, because legacy is zero.
 */
#define _GNU_SOURCE 1

#include "atlas/syspolicy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/rootpath.h"

/* A policy is a handful of `key = value` lines. The bound is small because the
 * file is small; a file Atlas parses is a file whose size is somebody else's
 * choice, root's here, and the habit of bounding it costs nothing. */
#define SYSPOLICY_MAX_BYTES 8192u

const char *atlas_syspolicy_reason_name(atlas_syspolicy_reason r) {
    switch (r) {
    case ATLAS_SYSPOLICY_REASON_UNKNOWN: return "UNKNOWN";
    case ATLAS_SYSPOLICY_REASON_ABSENT: return "ABSENT";
    case ATLAS_SYSPOLICY_REASON_PATH_UNSAFE: return "PATH_UNSAFE";
    case ATLAS_SYSPOLICY_REASON_WRITABLE: return "WRITABLE";
    case ATLAS_SYSPOLICY_REASON_MALFORMED: return "MALFORMED";
    case ATLAS_SYSPOLICY_REASON_ACTIVE: return "ACTIVE";
    }
    return "UNKNOWN";
}

const char *atlas_syspolicy_reason_explain(atlas_syspolicy_reason r) {
    switch (r) {
    case ATLAS_SYSPOLICY_REASON_UNKNOWN:
        return "the policy was never loaded, which Atlas reads as per-user mode";
    case ATLAS_SYSPOLICY_REASON_ABSENT:
        return "no system-deployment policy is installed, so Atlas serves only its own uid";
    case ATLAS_SYSPOLICY_REASON_PATH_UNSAFE:
        return "a component of the policy path is a symbolic link or is malformed, so whoever "
               "can create links there would choose the policy";
    case ATLAS_SYSPOLICY_REASON_WRITABLE:
        return "the policy, or a directory leading to it, can be modified by somebody other "
               "than root, so it constrains nobody";
    case ATLAS_SYSPOLICY_REASON_MALFORMED:
        return "the policy exists but does not describe a complete system deployment";
    case ATLAS_SYSPOLICY_REASON_ACTIVE:
        return "a root-anchored policy defines the shared socket, index and client list";
    }
    return "Atlas is in per-user mode";
}

bool atlas_syspolicy_permits(const atlas_syspolicy *p, long long uid) {
    if (p == NULL || p->state != ATLAS_SYSPOLICY_SYSTEM) {
        return false;
    }
    for (size_t i = 0; i < p->client_count; i++) {
        if (p->client_uids[i] == uid) {
            return true;
        }
    }
    return false;
}

/* --- parsing ---------------------------------------------------------------
 *
 * Deliberately dull. One `key = value` per line, `#` comments, no quoting, no
 * escapes, no includes and no continuation lines. Every one of those would be a
 * parser feature whose bugs are reachable from a security-relevant file, and
 * none of them buys the operator anything a second line would not. */

static bool take_value(const char *line, size_t len, const char *key, const char **val_out,
                       size_t *val_len_out) {
    size_t klen = strlen(key);
    if (len <= klen || strncmp(line, key, klen) != 0) {
        return false;
    }
    size_t i = klen;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i] != '=') {
        return false;
    }
    i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    size_t end = len;
    while (end > i && (line[end - 1u] == ' ' || line[end - 1u] == '\t' || line[end - 1u] == '\r')) {
        end--;
    }
    if (end <= i) {
        return false;
    }
    *val_out = line + i;
    *val_len_out = end - i;
    return true;
}

static bool copy_value(char *dst, size_t dst_size, const char *val, size_t len) {
    if (len + 1u > dst_size) {
        return false;
    }
    memcpy(dst, val, len);
    dst[len] = '\0';
    return true;
}

/* Absolute, no `..`, and printable ASCII. The socket path additionally has to
 * fit a `sockaddr_un`, which the caller checks; this is the shape check. */
static bool plausible_abs_path(const char *s) {
    if (s[0] != '/') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x20u || *p >= 0x7fu) {
            return false;
        }
    }
    return strstr(s, "/../") == NULL && strstr(s, "//") == NULL;
}

static bool parse_uid(const char *val, size_t len, long long *out) {
    long long v = 0;
    if (len == 0 || len > 10u) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (val[i] < '0' || val[i] > '9') {
            return false;
        }
        v = v * 10 + (val[i] - '0');
    }
    /* uid 0 is never listed. Root does not need the allowlist and putting it
     * there would suggest the socket is a route to privilege. */
    if (v <= 0) {
        return false;
    }
    *out = v;
    return true;
}

void atlas_syspolicy_load_at(const char *path, atlas_syspolicy *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_SYSPOLICY_LEGACY;
    out->reason = ATLAS_SYSPOLICY_REASON_ABSENT;

    atlas_rootpath_result rr = ATLAS_ROOTPATH_UNKNOWN;
    int fd = atlas_rootpath_open(path, false, &rr, out->detail, sizeof(out->detail));
    if (fd < 0) {
        switch (rr) {
        case ATLAS_ROOTPATH_MISSING: out->reason = ATLAS_SYSPOLICY_REASON_ABSENT; break;
        case ATLAS_ROOTPATH_SYMLINK:
        case ATLAS_ROOTPATH_BAD_PATH: out->reason = ATLAS_SYSPOLICY_REASON_PATH_UNSAFE; break;
        case ATLAS_ROOTPATH_NOT_REGULAR:
        case ATLAS_ROOTPATH_NOT_DIRECTORY: out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED; break;
        case ATLAS_ROOTPATH_UNKNOWN:
        case ATLAS_ROOTPATH_OK:
        case ATLAS_ROOTPATH_WRITABLE: out->reason = ATLAS_SYSPOLICY_REASON_WRITABLE; break;
        }
        return;
    }

    char buf[SYSPOLICY_MAX_BYTES];
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t got = read(fd, buf + total, sizeof(buf) - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
            return;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    (void)close(fd);
    if (total == sizeof(buf)) {
        /* Larger than a policy can be. Refused rather than truncated: a
         * truncated policy is a different policy, and the part that would be
         * lost is whichever line happened to be last. */
        out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
        return;
    }

    bool have_socket = false, have_data = false;
    size_t i = 0;
    while (i < total) {
        size_t start = i;
        while (i < total && buf[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < total) {
            i++;
        }
        while (start < end && (buf[start] == ' ' || buf[start] == '\t')) {
            start++;
        }
        if (start >= end || buf[start] == '#') {
            continue;
        }
        const char *line = buf + start;
        size_t len = end - start;
        const char *val = NULL;
        size_t vlen = 0;

        if (take_value(line, len, "socket_path", &val, &vlen)) {
            if (!copy_value(out->socket_path, sizeof(out->socket_path), val, vlen) ||
                !plausible_abs_path(out->socket_path)) {
                out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
                return;
            }
            have_socket = true;
        } else if (take_value(line, len, "data_dir", &val, &vlen)) {
            if (!copy_value(out->data_dir, sizeof(out->data_dir), val, vlen) ||
                !plausible_abs_path(out->data_dir)) {
                out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
                return;
            }
            have_data = true;
        } else if (take_value(line, len, "client_group", &val, &vlen)) {
            if (!copy_value(out->client_group, sizeof(out->client_group), val, vlen)) {
                out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
                return;
            }
        } else if (take_value(line, len, "client_uid", &val, &vlen)) {
            long long uid = 0;
            if (!parse_uid(val, vlen, &uid)) {
                out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
                return;
            }
            if (out->client_count >= ATLAS_SYSPOLICY_MAX_CLIENTS) {
                /* Refused, not truncated. A silently shortened allowlist is a
                 * policy whose author and reader disagree about who is on it —
                 * and the disagreement is invisible from both ends. */
                out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
                return;
            }
            out->client_uids[out->client_count++] = uid;
        } else {
            /* An unrecognised key is malformed rather than ignored. A policy
             * Atlas half-understands is one whose author believes they
             * configured something Atlas never read — including, one day, a
             * restriction. */
            out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
            return;
        }
    }

    if (!have_socket || !have_data) {
        out->reason = ATLAS_SYSPOLICY_REASON_MALFORMED;
        return;
    }

    out->reason = ATLAS_SYSPOLICY_REASON_ACTIVE;
    out->state = ATLAS_SYSPOLICY_SYSTEM;
}

void atlas_syspolicy_load(atlas_syspolicy *out) {
    atlas_syspolicy_load_at(ATLAS_SYSPOLICY_PATH, out);
}

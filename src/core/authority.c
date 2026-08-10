/* Atlas - operator authority: the probe, and the refusal.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/authority.h for what a grant requires and, more importantly, for
 * what it does not establish.
 *
 * Every path here is walked component by component from `/` with `O_NOFOLLOW`,
 * and every component is checked for root ownership and for group and other
 * write bits. `realpath(3)` is the wrong tool and does not appear, for the
 * reason A5 gives about backup paths: it resolves links, which means it answers
 * about a file somebody else may have chosen.
 *
 * The whole file fails closed. There is one `out->state = ATLAS_AUTHORITY_GRANTED`
 * assignment, it is the last statement of the probe, and every path that does
 * not reach it leaves the struct as `memset` left it — which is LOCKED, because
 * LOCKED is zero.
 */
#define _GNU_SOURCE 1

#include "atlas/authority.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "atlas/atlas.h"

/* A policy is a handful of `key = value` lines. This bounds the read, because a
 * file Atlas parses is a file whose size is somebody else's choice — root's,
 * here, but the bound costs nothing and the habit is the point. */
#define AUTHORITY_POLICY_MAX_BYTES 4096u

const char *atlas_authority_op_name(atlas_authority_op op) {
    switch (op) {
    case ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE:
        return "approving, rejecting, superseding or revalidating a decision";
    }
    return "unknown operation";
}

const char *atlas_authority_reason_name(atlas_authority_reason r) {
    switch (r) {
    case ATLAS_AUTHORITY_REASON_UNKNOWN: return "UNKNOWN";
    case ATLAS_AUTHORITY_REASON_NO_POLICY: return "NO_POLICY";
    case ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE: return "POLICY_PATH_UNSAFE";
    case ATLAS_AUTHORITY_REASON_POLICY_WRITABLE: return "POLICY_WRITABLE";
    case ATLAS_AUTHORITY_REASON_POLICY_MALFORMED: return "POLICY_MALFORMED";
    case ATLAS_AUTHORITY_REASON_NOT_THE_OPERATOR: return "NOT_THE_OPERATOR";
    case ATLAS_AUTHORITY_REASON_BINARY_WRITABLE: return "BINARY_WRITABLE";
    case ATLAS_AUTHORITY_REASON_GRANTED: return "GRANTED";
    }
    return "UNKNOWN";
}

const char *atlas_authority_reason_explain(atlas_authority_reason r) {
    switch (r) {
    case ATLAS_AUTHORITY_REASON_UNKNOWN:
        return "the profile was never probed, which Atlas reads as locked";
    case ATLAS_AUTHORITY_REASON_NO_POLICY:
        return "no operator-authority policy is installed, so no OS principal is separated "
               "from the one this process runs as";
    case ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE:
        return "a component of the policy path is a symbolic link, so whoever can create links "
               "there would choose the policy";
    case ATLAS_AUTHORITY_REASON_POLICY_WRITABLE:
        return "the policy, or a directory leading to it, can be modified by somebody other "
               "than root, so it constrains nobody";
    case ATLAS_AUTHORITY_REASON_POLICY_MALFORMED:
        return "the policy exists but does not name an operator uid Atlas can read";
    case ATLAS_AUTHORITY_REASON_NOT_THE_OPERATOR:
        return "the policy names a different uid than the one running this command";
    case ATLAS_AUTHORITY_REASON_BINARY_WRITABLE:
        return "the atlas executable can be replaced by a non-root uid, so any check it "
               "performs could be replaced with one that always agrees";
    case ATLAS_AUTHORITY_REASON_GRANTED:
        return "a root-anchored policy names this uid as the operator";
    }
    return "the profile is locked";
}

/* --- the guarded walk ----------------------------------------------------- */

static void set_reason(atlas_authority *out, atlas_authority_reason r, const char *detail) {
    out->reason = r;
    if (detail != NULL) {
        (void)snprintf(out->detail, sizeof(out->detail), "%s", detail);
    }
}

/* True when this inode is root-owned and no non-root uid may write it.
 *
 * The sticky bit is not an exception. A directory like `/tmp` is world-writable
 * and sticky, which stops one user deleting another's files and does nothing
 * whatever to stop them creating their own — so a policy underneath one is a
 * policy an attacker can put a directory beside. */
static bool root_owned_and_unwritable(const struct stat *sb) {
    return sb->st_uid == 0u && (sb->st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

/* Opens `path` by walking it from `/`, refusing every symlink and requiring
 * every component to be root-owned and not writable by anyone else.
 *
 * Returns a file descriptor for the final component on success. `*out` carries
 * the reason on failure and is otherwise untouched. */
static int open_root_anchored(const char *path, bool want_dir_only_parents,
                              atlas_authority *out) {
    (void)want_dir_only_parents;
    if (path == NULL || path[0] != '/') {
        set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, path);
        return -1;
    }
    int dir = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) {
        set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, "/");
        return -1;
    }
    struct stat sb;
    if (fstat(dir, &sb) != 0 || !root_owned_and_unwritable(&sb)) {
        /* A machine whose `/` is not root-owned is not one Atlas can reason
         * about at all. */
        set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_WRITABLE, "/");
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
        if (n == 0) {
            /* An empty component: `//` or a trailing slash. Refused rather than
             * skipped — Atlas is not in the business of guessing what a
             * malformed absolute path meant. */
            set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, path);
            (void)close(dir);
            return -1;
        }
        if (n >= sizeof(component)) {
            set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, path);
            (void)close(dir);
            return -1;
        }
        memcpy(component, p, n);
        component[n] = '\0';
        if (strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
            set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, path);
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
        if (!last) {
            flags |= O_DIRECTORY;
        }
        int next = openat(dir, component, flags);
        int saved = errno;
        (void)close(dir);
        if (next < 0) {
            if (saved == ELOOP) {
                set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, sofar);
            } else if (saved == ENOENT) {
                set_reason(out, ATLAS_AUTHORITY_REASON_NO_POLICY, sofar);
            } else {
                /* EACCES, ENOTDIR and everything else. Unreadable is not
                 * permission. */
                set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_WRITABLE, sofar);
            }
            return -1;
        }
        if (fstat(next, &sb) != 0 || !root_owned_and_unwritable(&sb)) {
            set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_WRITABLE, sofar);
            (void)close(next);
            return -1;
        }
        if (last) {
            if (!S_ISREG(sb.st_mode)) {
                set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_MALFORMED, sofar);
                (void)close(next);
                return -1;
            }
            return next;
        }
        dir = next;
        p = slash + 1;
    }
    set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_PATH_UNSAFE, path);
    (void)close(dir);
    return -1;
}

/* --- the policy ----------------------------------------------------------- */

/* Reads `operator_uid = N`. Anything it does not understand is malformed, and
 * malformed is locked: a policy Atlas half-understands is one whose author and
 * whose reader disagree about what was authorised. */
static bool parse_operator_uid(const char *text, size_t len, long long *uid_out) {
    bool found = false;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t')) {
            start++;
        }
        if (start >= end || text[start] == '#') {
            continue;
        }
        static const char KEY[] = "operator_uid";
        size_t klen = sizeof(KEY) - 1u;
        if ((size_t)(end - start) <= klen || strncmp(text + start, KEY, klen) != 0) {
            continue;
        }
        size_t j = start + klen;
        while (j < end && (text[j] == ' ' || text[j] == '\t')) {
            j++;
        }
        if (j >= end || text[j] != '=') {
            continue;
        }
        j++;
        while (j < end && (text[j] == ' ' || text[j] == '\t')) {
            j++;
        }
        if (j >= end) {
            return false;
        }
        long long v = 0;
        size_t digits = 0;
        while (j < end && text[j] >= '0' && text[j] <= '9') {
            if (v > (long long)1 << 40) {
                return false;
            }
            v = v * 10 + (text[j] - '0');
            j++;
            digits++;
        }
        while (j < end && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r')) {
            j++;
        }
        if (digits == 0 || j != end) {
            return false;
        }
        if (found) {
            /* Two answers is no answer. */
            return false;
        }
        *uid_out = v;
        found = true;
    }
    return found;
}

/* --- the probe ------------------------------------------------------------ */

void atlas_authority_probe_at(const char *policy_path, const char *exe_path,
                              atlas_authority *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_AUTHORITY_LOCKED;
    out->reason = ATLAS_AUTHORITY_REASON_NO_POLICY;
    out->operator_uid = -1;
    out->caller_uid = (long long)getuid();

    int fd = open_root_anchored(policy_path, false, out);
    if (fd < 0) {
        return;
    }
    char buf[AUTHORITY_POLICY_MAX_BYTES];
    ssize_t got = 0;
    size_t total = 0;
    while (total < sizeof(buf)) {
        got = read(fd, buf + total, sizeof(buf) - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_MALFORMED, policy_path);
            (void)close(fd);
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
         * truncated policy is a different policy. */
        set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_MALFORMED, policy_path);
        return;
    }

    long long uid = -1;
    if (!parse_operator_uid(buf, total, &uid)) {
        set_reason(out, ATLAS_AUTHORITY_REASON_POLICY_MALFORMED, policy_path);
        return;
    }
    out->operator_uid = uid;
    if (uid != out->caller_uid) {
        set_reason(out, ATLAS_AUTHORITY_REASON_NOT_THE_OPERATOR, policy_path);
        return;
    }

    /* The binary last, because its failure is the least obvious and the most
     * worth reporting on its own. A grant from a replaceable binary is a grant
     * from whatever was last compiled by whoever could replace it. */
    atlas_authority exe_probe;
    memset(&exe_probe, 0, sizeof(exe_probe));
    int exe_fd = open_root_anchored(exe_path, false, &exe_probe);
    if (exe_fd < 0) {
        set_reason(out, ATLAS_AUTHORITY_REASON_BINARY_WRITABLE, exe_probe.detail);
        return;
    }
    (void)close(exe_fd);

    out->state = ATLAS_AUTHORITY_GRANTED;
    set_reason(out, ATLAS_AUTHORITY_REASON_GRANTED, policy_path);
}

/* The running executable, asked of the kernel rather than reconstructed from
 * `argv[0]` — which the caller chooses. */
static void self_exe(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1u);
    if (n <= 0) {
        /* No `/proc` is not a reason to skip the check. An empty path fails the
         * walk, which locks. */
        buf[0] = '\0';
        return;
    }
    buf[n] = '\0';
    /* A deleted-and-replaced binary reads as "<path> (deleted)". That is not a
     * path, and Atlas does not try to repair it into one. */
}

void atlas_authority_probe(atlas_authority *out) {
    char exe[PATH_MAX];
    self_exe(exe, sizeof(exe));
    atlas_authority_probe_at(ATLAS_AUTHORITY_POLICY_PATH, exe, out);
}

atlas_status atlas_authority_require(atlas_authority_op op, atlas_err *err) {
    atlas_authority a;
    atlas_authority_probe(&a);
    if (a.state == ATLAS_AUTHORITY_GRANTED) {
        return ATLAS_OK;
    }
    /* The message is long because a refusal nobody understands gets worked
     * around. It says what was refused, why, and precisely what would change
     * it — including the part Atlas cannot do for the operator. */
    return atlas_err_set(
        err, ATLAS_ERR_CONFIG,
        "%s is locked in this Atlas profile.\n"
        "\n"
        "  reason : %s\n"
        "  detail : %s\n"
        "  caller : uid %lld\n"
        "\n"
        "Atlas cannot tell a person from a program running as the same user. A terminal, a "
        "pseudo-terminal, a typed confirmation and an environment variable are all producible "
        "by any process with this uid, so none of them is treated as evidence that an operator "
        "acted.\n"
        "\n"
        "To enable this operation, a separate OS principal has to exist and Atlas has to be "
        "told about it:\n"
        "  1. run the Atlas daemon and its data directory as a uid the model does not have;\n"
        "  2. install %s, owned by root, mode 0644, on a root-owned path, containing\n"
        "     operator_uid = <the human's uid>;\n"
        "  3. install the atlas executable root-owned and not writable by any other uid.\n"
        "Both steps need root and neither is something Atlas will do to a machine by itself. "
        "See docs/security/A7_SECURITY_REVIEW.md.",
        atlas_authority_op_name(op), atlas_authority_reason_name(a.reason),
        atlas_authority_reason_explain(a.reason), a.caller_uid, ATLAS_AUTHORITY_POLICY_PATH);
}

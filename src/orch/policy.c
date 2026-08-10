/* Atlas - A8: loading the root-owned orchestration policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/orchpolicy.h for what a policy decides and what it cannot.
 *
 * The whole file fails closed. There is one `out->state = ATLAS_ORCHPOLICY_ENABLED`
 * assignment, it is the last statement of the loader, and every path that does
 * not reach it leaves the struct as `memset` left it — which is disabled,
 * because disabled is zero. That is A7.1's shape and it is deliberate.
 */
#define _GNU_SOURCE 1

#include "atlas/orchpolicy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/rootpath.h"

#define ORCHPOLICY_MAX_BYTES 8192u

const char *atlas_orchpolicy_reason_name(atlas_orchpolicy_reason r) {
    switch (r) {
    case ATLAS_ORCHPOLICY_REASON_UNKNOWN: return "UNKNOWN";
    case ATLAS_ORCHPOLICY_REASON_ABSENT: return "ABSENT";
    case ATLAS_ORCHPOLICY_REASON_PATH_UNSAFE: return "PATH_UNSAFE";
    case ATLAS_ORCHPOLICY_REASON_WRITABLE: return "WRITABLE";
    case ATLAS_ORCHPOLICY_REASON_MALFORMED: return "MALFORMED";
    case ATLAS_ORCHPOLICY_REASON_ACTIVE: return "ACTIVE";
    }
    return "UNKNOWN";
}

const char *atlas_orchpolicy_reason_explain(atlas_orchpolicy_reason r) {
    switch (r) {
    case ATLAS_ORCHPOLICY_REASON_UNKNOWN:
        return "the policy was never loaded, which Atlas reads as orchestration disabled";
    case ATLAS_ORCHPOLICY_REASON_ABSENT:
        return "no orchestration policy is installed, so Atlas accepts no jobs";
    case ATLAS_ORCHPOLICY_REASON_PATH_UNSAFE:
        return "a component of the policy path is a symbolic link or is malformed, so whoever "
               "can create links there would choose the policy";
    case ATLAS_ORCHPOLICY_REASON_WRITABLE:
        return "the policy, or a directory leading to it, can be modified by somebody other "
               "than root, so it constrains nobody";
    case ATLAS_ORCHPOLICY_REASON_MALFORMED:
        return "the policy exists but does not describe a complete orchestration deployment";
    case ATLAS_ORCHPOLICY_REASON_ACTIVE:
        return "a root-anchored policy defines the repositories, modes, drivers and ceilings "
               "orchestration runs under";
    }
    return "orchestration is disabled";
}

/* --- parsing ---------------------------------------------------------------
 *
 * Deliberately the same dull parser A7.1 uses: one `key = value` per line, `#`
 * comments, no quoting, no escapes, no includes, no continuations. Every one of
 * those would be a parser feature whose bugs are reachable from a
 * security-relevant file. */

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
    /* uid 0 is never listed, for A7.1's reason: root does not need an
     * allowlist, and putting it on one would suggest orchestration is a route
     * to privilege. */
    if (v <= 0) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_i64(const char *val, size_t len, long long *out) {
    long long v = 0;
    if (len == 0 || len > 18u) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (val[i] < '0' || val[i] > '9') {
            return false;
        }
        v = v * 10 + (val[i] - '0');
    }
    if (v <= 0) {
        return false; /* a zero or negative ceiling is not a ceiling */
    }
    *out = v;
    return true;
}

/* Names are the same shape the specification validator enforces, checked here
 * so a policy can never introduce a vocabulary entry a specification could not
 * legally carry. */
static bool is_name(const char *s, size_t len) {
    if (len == 0 || len > ATLAS_ORCH_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool push_name(char table[][ATLAS_ORCH_NAME_MAX + 1u], size_t cap, size_t *count,
                      const char *val, size_t len) {
    if (!is_name(val, len) || *count >= cap) {
        /* Refused, not truncated. A silently shortened list is one whose author
         * and reader disagree about what is on it, invisibly from both ends. */
        return false;
    }
    memcpy(table[*count], val, len);
    table[*count][len] = '\0';
    (*count)++;
    return true;
}

void atlas_orchpolicy_load_at(const char *path, atlas_orchpolicy *out) {
    memset(out, 0, sizeof(*out));
    out->state = ATLAS_ORCHPOLICY_DISABLED;
    out->reason = ATLAS_ORCHPOLICY_REASON_ABSENT;

    atlas_rootpath_result rr = ATLAS_ROOTPATH_UNKNOWN;
    int fd = atlas_rootpath_open(path, false, &rr, out->detail, sizeof(out->detail));
    if (fd < 0) {
        switch (rr) {
        case ATLAS_ROOTPATH_MISSING: out->reason = ATLAS_ORCHPOLICY_REASON_ABSENT; break;
        case ATLAS_ROOTPATH_SYMLINK:
        case ATLAS_ROOTPATH_BAD_PATH: out->reason = ATLAS_ORCHPOLICY_REASON_PATH_UNSAFE; break;
        case ATLAS_ROOTPATH_NOT_REGULAR:
        case ATLAS_ROOTPATH_NOT_DIRECTORY:
            out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
            break;
        case ATLAS_ROOTPATH_UNKNOWN:
        case ATLAS_ROOTPATH_OK:
        case ATLAS_ROOTPATH_WRITABLE: out->reason = ATLAS_ORCHPOLICY_REASON_WRITABLE; break;
        }
        return;
    }

    char buf[ORCHPOLICY_MAX_BYTES];
    size_t total = 0;
    while (total < sizeof(buf)) {
        ssize_t got = read(fd, buf + total, sizeof(buf) - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
            return;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    (void)close(fd);
    if (total == sizeof(buf)) {
        out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
        return;
    }

    bool have_dispatcher = false;
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

#define BAD()                                              \
    do {                                                   \
        out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;   \
        return;                                            \
    } while (0)

        if (take_value(line, len, "dispatcher_uid", &val, &vlen)) {
            if (have_dispatcher || !parse_uid(val, vlen, &out->dispatcher_uid)) {
                /* One dispatcher. Two would mean two principals can claim the
                 * same lease, and which of them a grant went to would depend on
                 * which connected first. */
                BAD();
            }
            have_dispatcher = true;
        } else if (take_value(line, len, "submitter_uid", &val, &vlen)) {
            long long uid = 0;
            if (!parse_uid(val, vlen, &uid) ||
                out->submitter_count >= ATLAS_ORCHPOLICY_MAX_SUBMITTERS) {
                BAD();
            }
            out->submitter_uids[out->submitter_count++] = uid;
        } else if (take_value(line, len, "repo", &val, &vlen)) {
            if (!push_name(out->repos, ATLAS_ORCHPOLICY_MAX_REPOS, &out->repo_count, val, vlen)) {
                BAD();
            }
        } else if (take_value(line, len, "mode", &val, &vlen)) {
            if (!push_name(out->modes, ATLAS_ORCHPOLICY_MAX_MODES, &out->mode_count, val, vlen)) {
                BAD();
            }
        } else if (take_value(line, len, "driver", &val, &vlen)) {
            if (!push_name(out->drivers, ATLAS_ORCHPOLICY_MAX_DRIVERS, &out->driver_count, val,
                           vlen)) {
                BAD();
            }
        } else if (take_value(line, len, "max_wall_timeout_ms", &val, &vlen)) {
            if (!parse_i64(val, vlen, &out->max_wall_timeout_ms)) {
                BAD();
            }
        } else if (take_value(line, len, "max_idle_timeout_ms", &val, &vlen)) {
            if (!parse_i64(val, vlen, &out->max_idle_timeout_ms)) {
                BAD();
            }
        } else if (take_value(line, len, "max_attempts", &val, &vlen)) {
            if (!parse_i64(val, vlen, &out->max_attempts)) {
                BAD();
            }
        } else if (take_value(line, len, "max_output_bytes", &val, &vlen)) {
            if (!parse_i64(val, vlen, &out->max_output_bytes)) {
                BAD();
            }
        } else if (take_value(line, len, "max_artifact_bytes", &val, &vlen)) {
            if (!parse_i64(val, vlen, &out->max_artifact_bytes)) {
                BAD();
            }
        } else if (take_value(line, len, "max_artifact_count", &val, &vlen)) {
            if (!parse_i64(val, vlen, &out->max_artifact_count)) {
                BAD();
            }
        } else if (take_value(line, len, "worker_root", &val, &vlen)) {
            if (!copy_value(out->worker_root, sizeof(out->worker_root), val, vlen) ||
                !plausible_abs_path(out->worker_root)) {
                BAD();
            }
        } else if (take_value(line, len, "live_model", &val, &vlen)) {
            if (vlen == 2u && strncmp(val, "on", 2u) == 0) {
                out->live_model = true;
            } else if (vlen == 3u && strncmp(val, "off", 3u) == 0) {
                out->live_model = false;
            } else {
                /* Not a boolean-ish parser. "yes", "1" and "true" are refused
                 * because a policy whose author wrote one of them and got
                 * `false` would have configured the opposite of what they
                 * believe, silently, on the key that decides whether a model
                 * runs at all. */
                BAD();
            }
        } else {
            /* An unrecognised key is malformed rather than ignored — A7.1's
             * rule. A policy Atlas half-understands is one whose author
             * believes they configured something Atlas never read. */
            BAD();
        }
#undef BAD
    }

    /* A policy has to be complete to be a policy. Each of these missing would
     * leave a decision to a default nobody wrote down. */
    if (!have_dispatcher || out->submitter_count == 0 || out->repo_count == 0 ||
        out->mode_count == 0 || out->driver_count == 0 || out->worker_root[0] == '\0') {
        out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
        return;
    }
    /* The dispatcher may not also be a submitter. Keeping the two disjoint is
     * what makes "an ordinary client cannot forge a dispatcher message, and a
     * dispatcher cannot create its own work" a property of the configuration
     * rather than a hope about it. */
    for (size_t k = 0; k < out->submitter_count; k++) {
        if (out->submitter_uids[k] == out->dispatcher_uid) {
            out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
            return;
        }
    }
    /* Ceilings default to the compiled-in absolutes when unset, and may only
     * lower them. A policy asking for more than Atlas will ever do is refused
     * rather than silently reduced: its author would otherwise believe a job
     * may run for longer than it can. */
    struct {
        long long *field;
        long long absolute;
    } caps[] = {
        {&out->max_wall_timeout_ms, ATLAS_ORCH_MAX_WALL_TIMEOUT_MS},
        {&out->max_idle_timeout_ms, ATLAS_ORCH_MAX_IDLE_TIMEOUT_MS},
        {&out->max_attempts, ATLAS_ORCH_MAX_ATTEMPTS},
        {&out->max_output_bytes, (long long)ATLAS_ORCH_MAX_OUTPUT_BYTES},
        {&out->max_artifact_bytes, (long long)ATLAS_ORCH_MAX_ARTIFACT_BYTES},
        {&out->max_artifact_count, ATLAS_ORCH_MAX_ARTIFACT_COUNT},
    };
    for (size_t k = 0; k < sizeof caps / sizeof caps[0]; k++) {
        if (*caps[k].field == 0) {
            *caps[k].field = caps[k].absolute;
        } else if (*caps[k].field > caps[k].absolute) {
            out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
            return;
        }
    }
    if (out->max_idle_timeout_ms > out->max_wall_timeout_ms) {
        out->reason = ATLAS_ORCHPOLICY_REASON_MALFORMED;
        return;
    }

    out->reason = ATLAS_ORCHPOLICY_REASON_ACTIVE;
    out->state = ATLAS_ORCHPOLICY_ENABLED;
}

void atlas_orchpolicy_load(atlas_orchpolicy *out) {
    atlas_orchpolicy_load_at(ATLAS_ORCHPOLICY_PATH, out);
}

/* --- membership ----------------------------------------------------------- */

static bool has_name(const char table[][ATLAS_ORCH_NAME_MAX + 1u], size_t count,
                     const char *name) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(table[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool atlas_orchpolicy_permits_submitter(const atlas_orchpolicy *p, long long uid) {
    if (p == NULL || p->state != ATLAS_ORCHPOLICY_ENABLED) {
        return false;
    }
    for (size_t i = 0; i < p->submitter_count; i++) {
        if (p->submitter_uids[i] == uid) {
            return true;
        }
    }
    return false;
}

bool atlas_orchpolicy_is_dispatcher(const atlas_orchpolicy *p, long long uid) {
    /* No `uid == getuid()` fallback, unlike the A7.1 socket check. The daemon's
     * own uid is not a dispatcher, and giving it that role would mean a bug in
     * the daemon could grant itself a lease. */
    return p != NULL && p->state == ATLAS_ORCHPOLICY_ENABLED && p->dispatcher_uid > 0 &&
           p->dispatcher_uid == uid;
}

bool atlas_orchpolicy_permits_repo(const atlas_orchpolicy *p, const char *name) {
    return p != NULL && p->state == ATLAS_ORCHPOLICY_ENABLED &&
           has_name(p->repos, p->repo_count, name);
}

bool atlas_orchpolicy_permits_mode(const atlas_orchpolicy *p, const char *name) {
    return p != NULL && p->state == ATLAS_ORCHPOLICY_ENABLED &&
           has_name(p->modes, p->mode_count, name);
}

bool atlas_orchpolicy_permits_driver(const atlas_orchpolicy *p, const char *name) {
    return p != NULL && p->state == ATLAS_ORCHPOLICY_ENABLED &&
           has_name(p->drivers, p->driver_count, name);
}

atlas_status atlas_orchpolicy_apply_limits(const atlas_orchpolicy *p, atlas_orch_spec *s,
                                           atlas_err *err) {
    if (p == NULL || p->state != ATLAS_ORCHPOLICY_ENABLED) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "orchestration is disabled on this machine (%s)",
                             p != NULL ? atlas_orchpolicy_reason_name(p->reason) : "UNKNOWN");
    }
    struct {
        int64_t *field;
        long long ceiling;
        const char *what;
    } limits[] = {
        {&s->wall_timeout_ms, p->max_wall_timeout_ms, "wall_timeout_ms"},
        {&s->idle_timeout_ms, p->max_idle_timeout_ms, "idle_timeout_ms"},
        {&s->max_attempts, p->max_attempts, "max_attempts"},
        {&s->max_output_bytes, p->max_output_bytes, "max_output_bytes"},
        {&s->max_artifact_bytes, p->max_artifact_bytes, "max_artifact_bytes"},
        {&s->max_artifact_count, p->max_artifact_count, "max_artifact_count"},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        if (*limits[i].field < 0) {
            /* A negative bound is a usage error, not a silent default — A5's
             * rule about `--older-than`, for the same reason: a discarded
             * number nobody is told about produces a job unlike the one asked
             * for. */
            return atlas_err_set(err, ATLAS_ERR_USAGE, "%s may not be negative", limits[i].what);
        }
        if (*limits[i].field == 0) {
            *limits[i].field = (int64_t)limits[i].ceiling;
        } else if (*limits[i].field > limits[i].ceiling) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "%s of %lld exceeds the orchestration policy ceiling of %lld",
                                 limits[i].what, (long long)*limits[i].field, limits[i].ceiling);
        }
    }
    return ATLAS_OK;
}

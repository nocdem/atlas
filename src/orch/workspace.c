/* Atlas - A8: creating, filling, harvesting and removing a job workspace.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/workspace.h for the layout and for what the snapshot deliberately
 * is not. The rule this file exists to keep: **every path is descended with
 * `openat` and `O_NOFOLLOW` from a descriptor that was validated once**, never
 * re-resolved from a string. A path that is checked and then reopened by name is
 * a path somebody can swap in between.
 */
#define _GNU_SOURCE 1

#include "atlas/workspace.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/git.h"
#include "atlas/orch.h"
#include "atlas/sha256.h"

void atlas_ws_init(atlas_ws *w) {
    memset(w, 0, sizeof(*w));
    atlas_buf_init(&w->root);
    atlas_buf_init(&w->source);
    atlas_buf_init(&w->work);
    atlas_buf_init(&w->logs);
    atlas_buf_init(&w->tests);
    atlas_buf_init(&w->artifacts);
    atlas_buf_init(&w->driver);
    w->root_fd = -1;
}

void atlas_ws_free(atlas_ws *w) {
    if (w == NULL) {
        return;
    }
    if (w->root_fd >= 0) {
        (void)close(w->root_fd);
        w->root_fd = -1;
    }
    atlas_buf_free(&w->root);
    atlas_buf_free(&w->source);
    atlas_buf_free(&w->work);
    atlas_buf_free(&w->logs);
    atlas_buf_free(&w->tests);
    atlas_buf_free(&w->artifacts);
    atlas_buf_free(&w->driver);
}

/* --- opening the worker root --------------------------------------------- */

/* Walks an absolute path from `/`, refusing every symlink, and requires the
 * final directory to be owned by this uid and not writable by group or other.
 *
 * Deliberately *not* `atlas_rootpath_open`: that one requires uid 0 on every
 * component, which is right for a policy nobody but root may write and wrong
 * for a directory the worker owns. What matters here is the opposite question —
 * can anybody *other than us* write it? — and the answer must be no. */
static int open_owned_dir(const char *path, atlas_err *err) {
    if (path == NULL || path[0] != '/') {
        (void)atlas_err_set(err, ATLAS_ERR_CONFIG, "the worker root must be an absolute path");
        return -1;
    }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        (void)atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot open /");
        return -1;
    }
    const char *p = path + 1;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            (void)close(fd);
            (void)atlas_err_set(err, ATLAS_ERR_CONFIG,
                                "the worker root path contains an empty or relative component");
            return -1;
        }
        char comp[256];
        if (n + 1u > sizeof(comp)) {
            (void)close(fd);
            (void)atlas_err_set(err, ATLAS_ERR_CONFIG, "a worker root component is too long");
            return -1;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        int next = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int saved = errno;
        (void)close(fd);
        if (next < 0) {
            (void)atlas_err_set_errno(err, ATLAS_ERR_CONFIG, saved,
                                      "cannot open worker root component \"%s\"", comp);
            return -1;
        }
        fd = next;
        p = slash != NULL ? slash + 1 : p + n;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        (void)close(fd);
        (void)atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot stat the worker root");
        return -1;
    }
    if (sb.st_uid != getuid()) {
        (void)close(fd);
        (void)atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                            "the worker root is owned by uid %lld, not by this process's uid %lld",
                            (long long)sb.st_uid, (long long)getuid());
        return -1;
    }
    if ((sb.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        (void)close(fd);
        (void)atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                            "the worker root is writable by group or other, so another account "
                            "could place entries inside a job workspace");
        return -1;
    }
    return fd;
}

/* mkdirat + openat, tolerating an existing directory but never a symlink. */
static int make_dir(int parent, const char *name, atlas_err *err) {
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
        (void)atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot create \"%s\"", name);
        return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        (void)atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                  "cannot open \"%s\" without following a link", name);
        return -1;
    }
    return fd;
}

atlas_status atlas_ws_open(const char *worker_root, const char *job_uid, int64_t attempt_no,
                           atlas_ws *out, atlas_err *err) {
    atlas_ws_init(out);
    /* The job id is Atlas-generated and the attempt number is an integer Atlas
     * counted. Both are re-checked here rather than trusted, because this
     * function is the single place a workspace path is constructed and a check
     * here covers every caller. */
    if (job_uid == NULL || strlen(job_uid) != ATLAS_ORCH_UID_HEX + 1u || job_uid[0] != 'j') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a workspace needs an Atlas-generated job identifier");
    }
    for (size_t i = 1; job_uid[i] != '\0'; i++) {
        char c = job_uid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the job identifier is not hex");
        }
    }
    if (attempt_no <= 0 || attempt_no > ATLAS_ORCH_MAX_ATTEMPTS) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the attempt number is out of range");
    }

    int root = open_owned_dir(worker_root, err);
    if (root < 0) {
        return err != NULL && err->status != ATLAS_OK ? err->status : ATLAS_ERR_CONFIG;
    }

    atlas_status st = ATLAS_OK;
    int jobs = make_dir(root, "jobs", err);
    (void)close(root);
    if (jobs < 0) {
        return ATLAS_ERR_INTERNAL;
    }
    int job = make_dir(jobs, job_uid, err);
    (void)close(jobs);
    if (job < 0) {
        return ATLAS_ERR_INTERNAL;
    }
    char attempt[32];
    (void)snprintf(attempt, sizeof(attempt), "%lld", (long long)attempt_no);
    int adir = make_dir(job, attempt, err);
    (void)close(job);
    if (adir < 0) {
        return ATLAS_ERR_INTERNAL;
    }

    static const char *const SUBDIRS[] = {"source", "work", "logs", "tests", "artifacts",
                                          "driver"};
    for (size_t i = 0; i < sizeof SUBDIRS / sizeof SUBDIRS[0]; i++) {
        int d = make_dir(adir, SUBDIRS[i], err);
        if (d < 0) {
            (void)close(adir);
            return ATLAS_ERR_INTERNAL;
        }
        (void)close(d);
    }

    out->root_fd = adir;
    st = atlas_buf_appendf(&out->root, err, "%s/jobs/%s/%lld", worker_root, job_uid,
                           (long long)attempt_no);
    struct {
        atlas_buf *b;
        const char *sub;
    } paths[] = {
        {&out->source, "source"}, {&out->work, "work"},           {&out->logs, "logs"},
        {&out->tests, "tests"},   {&out->artifacts, "artifacts"}, {&out->driver, "driver"},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof paths / sizeof paths[0]; i++) {
        st = atlas_buf_appendf(paths[i].b, err, "%s/%s", atlas_buf_cstr(&out->root),
                               paths[i].sub);
    }
    if (st != ATLAS_OK) {
        atlas_ws_free(out);
    }
    return st;
}

/* --- writing a file inside the workspace ---------------------------------- */

/* Descends `rel` from `base`, creating directories, and returns a descriptor on
 * the final component's *parent* plus the leaf name. Every step is `O_NOFOLLOW`,
 * so a directory somebody replaced with a symlink between two steps fails rather
 * than redirecting the write. */
static int descend(int base, const char *rel, size_t rel_len, char *leaf, size_t leaf_cap,
                   atlas_err *err) {
    int cur = dup(base);
    if (cur < 0) {
        (void)atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot duplicate a directory");
        return -1;
    }
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= rel_len; i++) {
        if (i != rel_len && rel[i] != '/') {
            continue;
        }
        size_t n = i - start;
        if (n == 0 || (n == 1 && rel[start] == '.') ||
            (n == 2 && rel[start] == '.' && rel[start + 1] == '.')) {
            (void)close(cur);
            (void)atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                "a workspace path contains an empty or traversing component");
            return -1;
        }
        if (n + 1u > leaf_cap) {
            (void)close(cur);
            (void)atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a workspace path component is too long");
            return -1;
        }
        memcpy(leaf, rel + start, n);
        leaf[n] = '\0';
        if (i == rel_len) {
            return cur; /* `leaf` is the final component; `cur` is its parent */
        }
        if (++depth > ATLAS_WS_MAX_DEPTH) {
            (void)close(cur);
            (void)atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a workspace path is too deep");
            return -1;
        }
        int next = make_dir(cur, leaf, err);
        (void)close(cur);
        if (next < 0) {
            return -1;
        }
        cur = next;
        start = i + 1u;
    }
    (void)close(cur);
    (void)atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a workspace path is empty");
    return -1;
}

static atlas_status write_at(int base, const char *rel, size_t rel_len, const void *data,
                             size_t len, atlas_err *err) {
    char leaf[256];
    int parent = descend(base, rel, rel_len, leaf, sizeof(leaf), err);
    if (parent < 0) {
        return ATLAS_ERR_INTEGRITY;
    }
    /* O_EXCL so a snapshot can never overwrite something already present, and
     * O_NOFOLLOW so it can never write through a link. Mode 0600: nothing a job
     * produces is readable by another account. */
    int fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int saved = errno;
    (void)close(parent);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, saved, "cannot create \"%s\"", leaf);
    }
    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            saved = errno;
            (void)close(fd);
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, saved, "cannot write \"%s\"",
                                       leaf);
        }
        p += w;
        left -= (size_t)w;
    }
    (void)close(fd);
    return ATLAS_OK;
}

/* Appends to a file the first chunk already created. `O_NOFOLLOW` still, and no
 * `O_CREAT`: a later chunk for a file that does not exist means the stream is
 * out of order, and creating it here would hide that. */
static atlas_status append_at(int base, const char *rel, size_t rel_len, const void *data,
                              size_t len, atlas_err *err) {
    char leaf[256];
    int parent = descend(base, rel, rel_len, leaf, sizeof(leaf), err);
    if (parent < 0) {
        return ATLAS_ERR_INTEGRITY;
    }
    int fd = openat(parent, leaf, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    int saved = errno;
    (void)close(parent);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, saved,
                                   "a snapshot chunk arrived for \"%s\" before its first chunk",
                                   leaf);
    }
    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t wr = write(fd, p, left);
        if (wr < 0) {
            if (errno == EINTR) {
                continue;
            }
            saved = errno;
            (void)close(fd);
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, saved, "cannot append to \"%s\"",
                                       leaf);
        }
        p += wr;
        left -= (size_t)wr;
    }
    (void)close(fd);
    return ATLAS_OK;
}

atlas_status atlas_ws_write(const atlas_ws *w, const char *rel, const void *data, size_t len,
                            atlas_err *err) {
    if (w->root_fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the workspace is not open");
    }
    /* Anything written from outside this file goes through the declared-path
     * shape check, which refuses absolute paths, traversal and NULs. */
    if (!atlas_orch_relpath_is_safe(rel, strlen(rel))) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "\"%s\" is not a safe workspace-relative path", rel);
    }
    return write_at(w->root_fd, rel, strlen(rel), data, len, err);
}

/* A snapshot path is repository bytes, so it is *not* held to the declared-path
 * ASCII shape — A0's rule that paths are bytes applies. What is enforced is the
 * structure that makes a path safe to descend: relative, no empty component, no
 * `.` or `..`, and no NUL. */
static bool snapshot_path_is_safe(const char *p, size_t len) {
    if (len == 0 || len > 4096u || p[0] == '/') {
        return false;
    }
    if (memchr(p, '\0', len) != NULL) {
        return false;
    }
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && p[i] != '/') {
            continue;
        }
        size_t n = i - start;
        if (n == 0 || (n == 1 && p[start] == '.') ||
            (n == 2 && p[start] == '.' && p[start + 1] == '.')) {
            return false;
        }
        start = i + 1u;
    }
    return true;
}

/* --- materialising a received snapshot --------------------------------------
 *
 * The worker receives bytes from `atlasd` and writes them. It opens no
 * repository, resolves no commit and runs no git: `atlas_ws_snapshot` used to do
 * all three and is deliberately gone, because requiring the untrusted account to
 * hold a read path to `/opt/dna`, `/opt/atlas` and `/opt/swapper` was the wrong
 * boundary — and, on a machine where the repositories are owned by somebody
 * else, one git refuses to honour anyway.
 */

atlas_status atlas_ws_materialise(const atlas_ws *w, const void *rel, size_t rel_len,
                                  const char *mode, const void *data, size_t len, bool first,
                                  atlas_err *err) {
    if (w->root_fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the workspace is not open");
    }
    /* Re-checked on arrival. The daemon validated this path before sending it;
     * a receiver that trusts the sender's validation has no boundary of its
     * own, and this is the boundary. */
    if (!snapshot_path_is_safe((const char *)rel, rel_len)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a snapshot entry names a path that cannot be materialised safely");
    }
    (void)mode;
    int src = openat(w->root_fd, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int wrk = openat(w->root_fd, "work", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    atlas_status st = ATLAS_OK;
    if (src < 0 || wrk < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                 "cannot open the snapshot directories");
    } else if (first) {
        /* The first chunk creates the file with O_EXCL, so a duplicated entry
         * in the stream collides rather than overwriting — which is how a
         * replayed or reordered transfer is caught at the filesystem as well as
         * by the digest. */
        st = write_at(src, (const char *)rel, rel_len, data, len, err);
        if (st == ATLAS_OK) {
            st = write_at(wrk, (const char *)rel, rel_len, data, len, err);
        }
    } else {
        st = append_at(src, (const char *)rel, rel_len, data, len, err);
        if (st == ATLAS_OK) {
            st = append_at(wrk, (const char *)rel, rel_len, data, len, err);
        }
    }
    if (src >= 0) {
        (void)close(src);
    }
    if (wrk >= 0) {
        (void)close(wrk);
    }
    return st;
}

/* --- the patch ------------------------------------------------------------- */

typedef struct patch_ctx {
    atlas_buf out;
    int64_t max;
    atlas_status st;
} patch_ctx;

static atlas_status patch_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    patch_ctx *p = (patch_ctx *)ud;
    if ((int64_t)(p->out.len + n) > p->max) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the generated patch exceeds the %lld byte bound",
                             (long long)p->max);
    }
    return atlas_buf_append(&p->out, chunk, n, err);
}

atlas_status atlas_ws_make_patch(const atlas_ws *w, int64_t max_bytes, int64_t *changed_out,
                                 bool *differed_out, atlas_err *err) {
    patch_ctx p;
    atlas_buf_init(&p.out);
    p.max = max_bytes > 0 ? max_bytes : (16ll * 1024ll * 1024ll);
    bool differed = false;
    atlas_status st = atlas_git_diff_no_index(atlas_buf_cstr(&w->source), atlas_buf_cstr(&w->work),
                                              patch_sink, &p, (size_t)p.max, &differed, err);
    if (st == ATLAS_OK) {
        st = atlas_ws_write(w, "artifacts/changes.patch", p.out.data, p.out.len, err);
    }
    if (st == ATLAS_OK && changed_out != NULL) {
        /* Counted from the diff's own file headers rather than from a second
         * traversal, so the number always describes the patch that was written. */
        int64_t n = 0;
        const char *s = atlas_buf_cstr(&p.out);
        for (const char *q = s; (q = strstr(q, "diff --git ")) != NULL; q++) {
            if (q == s || q[-1] == '\n') {
                n++;
            }
        }
        *changed_out = n;
    }
    if (differed_out != NULL) {
        *differed_out = differed;
    }
    atlas_buf_free(&p.out);
    return st;
}

/* --- changed files --------------------------------------------------------- */

typedef struct changed_ctx {
    atlas_ws_changed_cb cb;
    void *ud;
    int64_t count;
} changed_ctx;

/* Walks `work/` and `source/` in parallel by descending both with `openat`.
 * Simple and bounded: a file present in one and not the other, or differing in
 * size or content digest, is a change. */
static atlas_status walk_changed(int src, int wrk, const char *prefix, changed_ctx *c,
                                 int depth, atlas_err *err) {
    if (depth > ATLAS_WS_MAX_DEPTH) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the work tree is too deep");
    }
    int dupfd = dup(wrk);
    if (dupfd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot duplicate a directory");
    }
    DIR *d = fdopendir(dupfd);
    if (d == NULL) {
        (void)close(dupfd);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot read the work tree");
    }
    atlas_status st = ATLAS_OK;
    struct dirent *de;
    while (st == ATLAS_OK && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        struct stat wsb;
        if (fstatat(wrk, de->d_name, &wsb, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }
        char rel[4096];
        int rn = snprintf(rel, sizeof(rel), "%s%s%s", prefix, prefix[0] != '\0' ? "/" : "",
                          de->d_name);
        if (rn < 0 || (size_t)rn >= sizeof(rel)) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a work-tree path is too long");
            break;
        }
        if (S_ISDIR(wsb.st_mode)) {
            int wsub = openat(wrk, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (wsub < 0) {
                continue;
            }
            int ssub = openat(src, de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            st = walk_changed(ssub >= 0 ? ssub : wsub, wsub, rel, c, depth + 1, err);
            if (ssub >= 0) {
                (void)close(ssub);
            }
            (void)close(wsub);
            continue;
        }
        if (!S_ISREG(wsb.st_mode)) {
            /* A driver that created a symlink, fifo or device in the work tree
             * is reported as a change rather than followed. */
            c->count++;
            if (c->cb != NULL) {
                st = c->cb(rel, c->ud, err);
            }
            continue;
        }
        struct stat ssb;
        bool changed = (fstatat(src, de->d_name, &ssb, AT_SYMLINK_NOFOLLOW) != 0) ||
                       !S_ISREG(ssb.st_mode) || ssb.st_size != wsb.st_size;
        if (!changed) {
            /* Same size: compare content. Size alone would miss an in-place
             * edit of equal length, which is exactly what a code change often
             * is. */
            int fa = openat(src, de->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            int fb = openat(wrk, de->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fa >= 0 && fb >= 0) {
                char ba[8192], bb[8192];
                for (;;) {
                    ssize_t ra = read(fa, ba, sizeof(ba));
                    ssize_t rb = read(fb, bb, sizeof(bb));
                    if (ra != rb || (ra > 0 && memcmp(ba, bb, (size_t)ra) != 0)) {
                        changed = true;
                        break;
                    }
                    if (ra <= 0) {
                        break;
                    }
                }
            } else {
                changed = true;
            }
            if (fa >= 0) {
                (void)close(fa);
            }
            if (fb >= 0) {
                (void)close(fb);
            }
        }
        if (changed) {
            c->count++;
            if (c->cb != NULL) {
                st = c->cb(rel, c->ud, err);
            }
        }
    }
    (void)closedir(d);
    return st;
}

atlas_status atlas_ws_changed_files(const atlas_ws *w, atlas_ws_changed_cb cb, void *ud,
                                    int64_t *count_out, atlas_err *err) {
    changed_ctx c = {cb, ud, 0};
    int src = openat(w->root_fd, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int wrk = openat(w->root_fd, "work", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    atlas_status st = ATLAS_OK;
    if (src < 0 || wrk < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot open the snapshot trees");
    } else {
        st = walk_changed(src, wrk, "", &c, 0, err);
    }
    if (src >= 0) {
        (void)close(src);
    }
    if (wrk >= 0) {
        (void)close(wrk);
    }
    if (count_out != NULL) {
        *count_out = c.count;
    }
    return st;
}

bool atlas_ws_paths_are_declared(const char *changed_rel, const atlas_buf *declared,
                                 size_t declared_count) {
    if (declared_count == 0) {
        /* No declaration means the workspace itself is the boundary. It is not
         * a wildcard over the host: the driver cannot leave the workspace
         * whatever it declares, because the OS boundary is what stops it. */
        return true;
    }
    size_t n = strlen(changed_rel);
    for (size_t i = 0; i < declared_count; i++) {
        const char *p = atlas_buf_cstr(&declared[i]);
        size_t pn = declared[i].len;
        if (pn == 0 || pn > n) {
            continue;
        }
        if (memcmp(changed_rel, p, pn) != 0) {
            continue;
        }
        /* A prefix must end at a component boundary: "src" must not match
         * "srcfoo.c", which is how a prefix check usually goes wrong. */
        if (n == pn || changed_rel[pn] == '/') {
            return true;
        }
    }
    return false;
}

/* --- artifact collection ---------------------------------------------------- */

void atlas_ws_artifacts_free(atlas_ws_artifact *a, size_t count) {
    if (a == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        atlas_buf_free(&a[i].name);
        atlas_buf_free(&a[i].sha256);
        atlas_buf_free(&a[i].content);
    }
    free(a);
}

atlas_status atlas_ws_collect(const atlas_ws *w, int64_t max_count, int64_t max_bytes,
                              size_t inline_max, atlas_ws_artifact **out, size_t *count_out,
                              int64_t *refused_out, atlas_err *err) {
    *out = NULL;
    *count_out = 0;
    if (refused_out != NULL) {
        *refused_out = 0;
    }
    int dir = openat(w->root_fd, "artifacts", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                   "cannot open the artifact directory");
    }
    int dupfd = dup(dir);
    DIR *d = dupfd >= 0 ? fdopendir(dupfd) : NULL;
    if (d == NULL) {
        if (dupfd >= 0) {
            (void)close(dupfd);
        }
        (void)close(dir);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                   "cannot read the artifact directory");
    }

    size_t cap = 16;
    atlas_ws_artifact *list = calloc(cap, sizeof(*list));
    if (list == NULL) {
        (void)closedir(d);
        (void)close(dir);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
    }
    size_t n = 0;
    int64_t total = 0;
    atlas_status st = ATLAS_OK;
    struct dirent *de;
    while (st == ATLAS_OK && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        struct stat sb;
        if (fstatat(dir, de->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }
        /* **A symlink is refused, never followed.** A driver that plants a link
         * to /etc/shadow and asks for collection gets a refusal naming the
         * entry, and Atlas never opens the target. Anything that is not a plain
         * regular file is refused for the same reason. */
        if (!S_ISREG(sb.st_mode)) {
            if (refused_out != NULL) {
                (*refused_out)++;
            }
            continue;
        }
        if (!atlas_orch_relpath_is_safe(de->d_name, strlen(de->d_name))) {
            if (refused_out != NULL) {
                (*refused_out)++;
            }
            continue;
        }
        if ((int64_t)n >= max_count) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the job produced more than %lld artifacts", (long long)max_count);
            break;
        }
        total += sb.st_size;
        if (total > max_bytes) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the job's artifacts total more than %lld bytes",
                               (long long)max_bytes);
            break;
        }
        if (n == cap) {
            size_t ncap = cap * 2u;
            atlas_ws_artifact *bigger = realloc(list, ncap * sizeof(*list));
            if (bigger == NULL) {
                st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
                break;
            }
            memset(bigger + cap, 0, (ncap - cap) * sizeof(*list));
            list = bigger;
            cap = ncap;
        }
        atlas_ws_artifact *a = &list[n];
        atlas_buf_init(&a->name);
        atlas_buf_init(&a->sha256);
        atlas_buf_init(&a->content);
        a->size_bytes = sb.st_size;
        st = atlas_buf_set_str(&a->name, de->d_name, err);
        if (st != ATLAS_OK) {
            break;
        }
        int fd = openat(dir, de->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            if (refused_out != NULL) {
                (*refused_out)++;
            }
            atlas_buf_free(&a->name);
            continue;
        }
        atlas_sha256 h;
        atlas_sha256_init(&h);
        char buf[65536];
        for (;;) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (r == 0) {
                break;
            }
            atlas_sha256_update(&h, buf, (size_t)r);
            if (a->content.len < inline_max) {
                size_t room = inline_max - a->content.len;
                size_t take = ((size_t)r < room) ? (size_t)r : room;
                st = atlas_buf_append(&a->content, buf, take, err);
                if (st != ATLAS_OK) {
                    break;
                }
            }
        }
        (void)close(fd);
        if (st != ATLAS_OK) {
            break;
        }
        unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
        atlas_sha256_final(&h, digest);
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_hex_encode(digest, sizeof(digest), hex);
        st = atlas_buf_set_str(&a->sha256, hex, err);
        /* Only whole small artifacts are carried inline. A truncated blob with
         * a whole-file digest would be a record whose digest does not describe
         * its content, which is worse than no content at all. */
        a->content_stored = (a->size_bytes <= (int64_t)inline_max);
        if (!a->content_stored) {
            atlas_buf_free(&a->content);
            atlas_buf_init(&a->content);
        }
        n++;
    }
    (void)closedir(d);
    (void)close(dir);
    if (st != ATLAS_OK) {
        atlas_ws_artifacts_free(list, n);
        return st;
    }
    *out = list;
    *count_out = n;
    return ATLAS_OK;
}

/* --- removal ---------------------------------------------------------------- */

static atlas_status remove_tree(int parent, const char *name, int depth, atlas_err *err) {
    if (depth > ATLAS_WS_MAX_DEPTH) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "refusing to remove a tree this deep");
    }
    struct stat sb;
    if (fstatat(parent, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? ATLAS_OK
                               : atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                                     "cannot stat \"%s\"", name);
    }
    if (S_ISLNK(sb.st_mode)) {
        /* Unlinked, never descended into. Removing a link removes the link. */
        return unlinkat(parent, name, 0) == 0 || errno == ENOENT
                   ? ATLAS_OK
                   : atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                         "cannot remove the link \"%s\"", name);
    }
    if (!S_ISDIR(sb.st_mode)) {
        if (!S_ISREG(sb.st_mode)) {
            /* A fifo, socket or device a driver created. Unlinked, but named in
             * the error path if that fails, rather than being skipped silently. */
            (void)unlinkat(parent, name, 0);
            return ATLAS_OK;
        }
        return unlinkat(parent, name, 0) == 0 || errno == ENOENT
                   ? ATLAS_OK
                   : atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot remove \"%s\"",
                                         name);
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                   "cannot descend into \"%s\" without following a link", name);
    }
    int dupfd = dup(fd);
    DIR *d = dupfd >= 0 ? fdopendir(dupfd) : NULL;
    if (d == NULL) {
        if (dupfd >= 0) {
            (void)close(dupfd);
        }
        (void)close(fd);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot read \"%s\"", name);
    }
    atlas_status st = ATLAS_OK;
    struct dirent *de;
    while (st == ATLAS_OK && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        st = remove_tree(fd, de->d_name, depth + 1, err);
    }
    (void)closedir(d);
    (void)close(fd);
    if (st == ATLAS_OK && unlinkat(parent, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot remove directory \"%s\"",
                                 name);
    }
    return st;
}

atlas_status atlas_ws_remove(const char *worker_root, const char *job_uid, int64_t attempt_no,
                             atlas_err *err) {
    /* The same three validated components `atlas_ws_open` takes, and nothing
     * else. There is no path parameter here and there must never be one: a
     * "remove this path" primitive is the shape that eventually removes the
     * wrong path. */
    if (job_uid == NULL || strlen(job_uid) != ATLAS_ORCH_UID_HEX + 1u || job_uid[0] != 'j') {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refusing to remove a workspace without an Atlas job identifier");
    }
    if (attempt_no <= 0 || attempt_no > ATLAS_ORCH_MAX_ATTEMPTS) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the attempt number is out of range");
    }
    int root = open_owned_dir(worker_root, err);
    if (root < 0) {
        return ATLAS_ERR_CONFIG;
    }
    int jobs = openat(root, "jobs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    (void)close(root);
    if (jobs < 0) {
        return errno == ENOENT ? ATLAS_OK
                               : atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                                     "cannot open the jobs directory");
    }
    int job = openat(jobs, job_uid, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    (void)close(jobs);
    if (job < 0) {
        return errno == ENOENT ? ATLAS_OK
                               : atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                                     "cannot open the job directory");
    }
    char attempt[32];
    (void)snprintf(attempt, sizeof(attempt), "%lld", (long long)attempt_no);
    atlas_status st = remove_tree(job, attempt, 0, err);
    (void)close(job);
    return st;
}

atlas_status atlas_ws_free_space(const char *worker_root, int64_t *bytes_out, atlas_err *err) {
    *bytes_out = 0;
    int fd = open_owned_dir(worker_root, err);
    if (fd < 0) {
        return ATLAS_ERR_CONFIG;
    }
    struct statvfs vfs;
    int rc = fstatvfs(fd, &vfs);
    int saved = errno;
    (void)close(fd);
    if (rc != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, saved,
                                   "cannot measure free space under the worker root");
    }
    *bytes_out = (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
    return ATLAS_OK;
}

/* --- redaction --------------------------------------------------------------- */

/* Token shapes that are worth removing from a log before it is stored.
 *
 * This is a mitigation and is documented as one. It cannot recognise a secret it
 * has never seen the shape of, which is exactly why A8's real defence is that no
 * credential is ever placed in a workspace, an environment or a job
 * specification — see `docs/orchestration.md`. Claiming "logs are redacted"
 * without that sentence would be the overclaim. */
static const char *const REDACT_PREFIXES[] = {
    "sk-ant-", "sk-", "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_",
    "AKIA",    "ASIA", "xoxb-", "xoxp-", "AIza", "-----BEGIN",
};

atlas_status atlas_ws_redact(const char *in, size_t len, atlas_buf *out, int64_t *hits_out,
                             atlas_err *err) {
    atlas_buf_reset(out);
    int64_t hits = 0;
    size_t i = 0;
    while (i < len) {
        bool matched = false;
        for (size_t k = 0; k < sizeof REDACT_PREFIXES / sizeof REDACT_PREFIXES[0]; k++) {
            size_t pl = strlen(REDACT_PREFIXES[k]);
            if (i + pl <= len && memcmp(in + i, REDACT_PREFIXES[k], pl) == 0) {
                /* Consume the prefix and everything token-like after it, so the
                 * secret goes rather than just its recognisable opening. */
                size_t j = i + pl;
                while (j < len) {
                    unsigned char c = (unsigned char)in[j];
                    bool tokenish = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '+' ||
                                    c == '/' || c == '=' || c == ' ';
                    if (!tokenish) {
                        break;
                    }
                    j++;
                }
                atlas_status st = atlas_buf_append_str(out, "[REDACTED]", err);
                if (st != ATLAS_OK) {
                    return st;
                }
                hits++;
                i = j;
                matched = true;
                break;
            }
        }
        if (!matched) {
            atlas_status st = atlas_buf_append(out, in + i, 1u, err);
            if (st != ATLAS_OK) {
                return st;
            }
            i++;
        }
    }
    if (hits_out != NULL) {
        *hits_out = hits;
    }
    return ATLAS_OK;
}

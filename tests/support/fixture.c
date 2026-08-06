/* Atlas - integration test fixtures.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "support/fixture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/proc.h"
#include "atlas/sha256.h"

#ifndef ATLAS_BIN
#define ATLAS_BIN "atlas"
#endif

/* Fixed base timestamp so commit ordering in tests is deterministic. */
#define FX_BASE_TIME 1700000000L

/* Temporary trees that have been created but not yet closed. A test that aborts
 * mid-body never reaches its fx_close(), so the harness sweeps these. */
#define FX_MAX_LIVE 16
static char *g_live[FX_MAX_LIVE];

static void live_add(const char *path) {
    for (size_t i = 0; i < FX_MAX_LIVE; i++) {
        if (g_live[i] == NULL) {
            g_live[i] = strdup(path);
            return;
        }
    }
}

static void live_remove(const char *path) {
    for (size_t i = 0; i < FX_MAX_LIVE; i++) {
        if (g_live[i] != NULL && strcmp(g_live[i], path) == 0) {
            free(g_live[i]);
            g_live[i] = NULL;
            return;
        }
    }
}

static const char *fx_tmp_base(void) {
    const char *t = getenv("TMPDIR");
    if (t != NULL && t[0] == '/') {
        return t;
    }
    return "/tmp";
}

atlas_status fx_open(fixture *fx, atlas_err *err) {
    memset(fx, 0, sizeof(*fx));
    atlas_buf_init(&fx->root);
    atlas_buf_init(&fx->repo);
    atlas_buf_init(&fx->data_dir);

    atlas_status st = atlas_buf_appendf(&fx->root, err, "%s/atlas-test-XXXXXX", fx_tmp_base());
    if (st != ATLAS_OK) {
        return st;
    }
    if (mkdtemp(fx->root.data) == NULL) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                   "cannot create a temporary directory");
    }
    st = atlas_buf_appendf(&fx->repo, err, "%s/repo", atlas_buf_cstr(&fx->root));
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&fx->data_dir, err, "%s/data", atlas_buf_cstr(&fx->root));
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (mkdir(atlas_buf_cstr(&fx->repo), S_IRWXU) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot create the repo dir");
    }
    if (mkdir(atlas_buf_cstr(&fx->data_dir), S_IRWXU) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot create the data dir");
    }
    live_add(atlas_buf_cstr(&fx->root));
    return ATLAS_OK;
}

/* Recursive delete without a shell. */
static void rm_rf_at(int parent_fd, const char *name) {
    struct stat sb;
    if (fstatat(parent_fd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        return;
    }
    if (S_ISDIR(sb.st_mode)) {
        int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            DIR *d = fdopendir(fd);
            if (d != NULL) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                        continue;
                    }
                    rm_rf_at(dirfd(d), e->d_name);
                }
                (void)closedir(d); /* closes fd */
            } else {
                (void)close(fd);
            }
        }
        (void)unlinkat(parent_fd, name, AT_REMOVEDIR);
        return;
    }
    (void)unlinkat(parent_fd, name, 0);
}

/* Removes the tree at `path` (an absolute mkdtemp result). */
static void remove_tree(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return;
    }
    atlas_buf parent = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_buf_append(&parent, path, (size_t)(slash - path), &err) == ATLAS_OK) {
        int pfd = open(atlas_buf_cstr(&parent), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (pfd >= 0) {
            rm_rf_at(pfd, slash + 1);
            (void)close(pfd);
        }
    }
    atlas_buf_free(&parent);
}

void fx_cleanup_leaked(void) {
    for (size_t i = 0; i < FX_MAX_LIVE; i++) {
        if (g_live[i] != NULL) {
            remove_tree(g_live[i]);
            free(g_live[i]);
            g_live[i] = NULL;
        }
    }
}

void fx_close(fixture *fx) {
    if (fx->root.len > 0) {
        const char *path = atlas_buf_cstr(&fx->root);
        remove_tree(path);
        live_remove(path);
    }
    atlas_buf_free(&fx->root);
    atlas_buf_free(&fx->repo);
    atlas_buf_free(&fx->data_dir);
}

const char *fx_repo(const fixture *fx) {
    return atlas_buf_cstr(&fx->repo);
}

const char *fx_data_dir(const fixture *fx) {
    return atlas_buf_cstr(&fx->data_dir);
}

/* --- running git --------------------------------------------------------- */

atlas_status fx_git(const fixture *fx, const char *dir, const char *const *args, size_t nargs,
                    int *exit_code, atlas_buf *stdout_out, atlas_err *err) {
    atlas_buf exe = ATLAS_BUF_INIT;
    atlas_status st = atlas_proc_which("git", getenv("PATH"), &exe, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&exe);
        return st;
    }

    const char *argv[32];
    size_t n = 0;
    argv[n++] = atlas_buf_cstr(&exe);
    argv[n++] = "-C";
    argv[n++] = dir;
    for (size_t i = 0; i < nargs; i++) {
        if (n + 2u >= sizeof(argv) / sizeof(argv[0])) {
            atlas_buf_free(&exe);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "fixture git argv is too long");
        }
        argv[n++] = args[i];
    }
    argv[n] = NULL;

    char adate[64];
    char cdate[64];
    long stamp = FX_BASE_TIME + (long)fx->commit_seq * 60L;
    (void)snprintf(adate, sizeof(adate), "GIT_AUTHOR_DATE=%ld +0000", stamp);
    (void)snprintf(cdate, sizeof(cdate), "GIT_COMMITTER_DATE=%ld +0000", stamp);

    atlas_buf path_env = ATLAS_BUF_INIT;
    const char *path = getenv("PATH");
    st = atlas_buf_appendf(&path_env, err, "PATH=%s",
                           (path != NULL && path[0] != '\0') ? path : "/usr/bin:/bin");
    if (st != ATLAS_OK) {
        atlas_buf_free(&exe);
        atlas_buf_free(&path_env);
        return st;
    }

    const char *env[] = {
        atlas_buf_cstr(&path_env),
        "HOME=/nonexistent-atlas-test-home",
        "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_CONFIG_SYSTEM=/dev/null",
        "GIT_AUTHOR_NAME=Atlas Test",
        "GIT_AUTHOR_EMAIL=test@atlas.invalid",
        "GIT_COMMITTER_NAME=Atlas Test",
        "GIT_COMMITTER_EMAIL=test@atlas.invalid",
        adate,
        cdate,
        "GIT_TERMINAL_PROMPT=0",
        "LC_ALL=C",
        "TZ=UTC",
        NULL,
    };

    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.env = env;
    opts.timeout_ms = 30000;
    opts.max_stdout = 8u * 1024u * 1024u;

    atlas_buf errbuf = ATLAS_BUF_INIT;
    atlas_proc_result res;
    st = atlas_proc_run(&opts, stdout_out != NULL ? atlas_proc_sink_buf : NULL, stdout_out, &errbuf,
                        &res, err);
    if (st == ATLAS_OK && exit_code != NULL) {
        *exit_code = res.exit_code;
    }
    if (st == ATLAS_OK && res.exit_code != 0 && exit_code == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "fixture git exited %d: %s", res.exit_code,
                           atlas_buf_cstr(&errbuf));
    }
    atlas_buf_free(&errbuf);
    atlas_buf_free(&exe);
    atlas_buf_free(&path_env);
    return st;
}

atlas_status fx_git_ok(const fixture *fx, const char *dir, const char *const *args, size_t nargs,
                       atlas_err *err) {
    return fx_git(fx, dir, args, nargs, NULL, NULL, err);
}

atlas_status fx_init_repo(fixture *fx, const char *dir, const char *object_format, atlas_err *err) {
    char fmt[48];
    const char *args[6];
    size_t n = 0;
    args[n++] = "init";
    args[n++] = "-q";
    args[n++] = "-b";
    args[n++] = "main";
    if (object_format != NULL) {
        (void)snprintf(fmt, sizeof(fmt), "--object-format=%s", object_format);
        args[n++] = fmt;
    }
    args[n++] = ".";

    int code = 0;
    atlas_status st = fx_git(fx, dir, args, n, &code, NULL, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (code != 0) {
        /* Most likely this git cannot create the requested object format. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "git init exited %d (object format %s)", code,
                             object_format != NULL ? object_format : "default");
    }
    return ATLAS_OK;
}

/* --- working tree helpers ------------------------------------------------ */

static atlas_status open_dir(const char *dir, int *fd_out, atlas_err *err) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot open %s", dir);
    }
    *fd_out = fd;
    return ATLAS_OK;
}

atlas_status fx_write_bytes(const char *dir, const void *rel, size_t rel_len, const void *data,
                            size_t n, mode_t mode, atlas_err *err) {
    atlas_buf name = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_append(&name, rel, rel_len, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&name);
        return st;
    }
    int dfd = -1;
    st = open_dir(dir, &dfd, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&name);
        return st;
    }
    int fd = openat(dfd, atlas_buf_cstr(&name), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot create %s/%s", dir,
                                 atlas_buf_cstr(&name));
        (void)close(dfd);
        atlas_buf_free(&name);
        return st;
    }
    const char *p = (const char *)data;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "write failed");
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (st == ATLAS_OK && fchmod(fd, mode) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "fchmod failed");
    }
    (void)close(fd);
    (void)close(dfd);
    atlas_buf_free(&name);
    return st;
}

atlas_status fx_write(const char *dir, const char *rel, const char *contents, atlas_err *err) {
    return fx_write_bytes(dir, rel, strlen(rel), contents, strlen(contents), 0644, err);
}

atlas_status fx_write_exec(const char *dir, const char *rel, const char *contents, atlas_err *err) {
    return fx_write_bytes(dir, rel, strlen(rel), contents, strlen(contents), 0755, err);
}

atlas_status fx_mkdir(const char *dir, const char *rel, atlas_err *err) {
    int dfd = -1;
    atlas_status st = open_dir(dir, &dfd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (mkdirat(dfd, rel, S_IRWXU) != 0 && errno != EEXIST) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "mkdir %s failed", rel);
    }
    (void)close(dfd);
    return st;
}

atlas_status fx_symlink(const char *dir, const char *target, const char *linkname,
                        atlas_err *err) {
    int dfd = -1;
    atlas_status st = open_dir(dir, &dfd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (symlinkat(target, dfd, linkname) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "symlink %s failed", linkname);
    }
    (void)close(dfd);
    return st;
}

atlas_status fx_remove(const char *dir, const char *rel, atlas_err *err) {
    int dfd = -1;
    atlas_status st = open_dir(dir, &dfd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (unlinkat(dfd, rel, 0) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "unlink %s failed", rel);
    }
    (void)close(dfd);
    return st;
}

atlas_status fx_chmod(const char *dir, const char *rel, mode_t mode, atlas_err *err) {
    int dfd = -1;
    atlas_status st = open_dir(dir, &dfd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (fchmodat(dfd, rel, mode, 0) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "chmod %s failed", rel);
    }
    (void)close(dfd);
    return st;
}

bool fx_can_create_name(const char *dir, const void *rel, size_t rel_len) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = fx_write_bytes(dir, rel, rel_len, "probe\n", 6u, 0644, &err);
    if (st != ATLAS_OK) {
        return false;
    }
    atlas_buf name = ATLAS_BUF_INIT;
    if (atlas_buf_append(&name, rel, rel_len, &err) == ATLAS_OK) {
        (void)fx_remove(dir, atlas_buf_cstr(&name), &err);
    }
    atlas_buf_free(&name);
    return true;
}

atlas_status fx_add_all(const fixture *fx, const char *dir, atlas_err *err) {
    const char *args[] = {"add", "-A", "--", "."};
    return fx_git_ok(fx, dir, args, 4u, err);
}

atlas_status fx_commit_body(fixture *fx, const char *dir, const char *subject, const char *body,
                            atlas_err *err) {
    fx->commit_seq++;
    const char *args[8];
    size_t n = 0;
    args[n++] = "commit";
    args[n++] = "-q";
    args[n++] = "--allow-empty";
    args[n++] = "-m";
    args[n++] = subject;
    if (body != NULL) {
        args[n++] = "-m";
        args[n++] = body;
    }
    return fx_git_ok(fx, dir, args, n, err);
}

atlas_status fx_commit(fixture *fx, const char *dir, const char *message, atlas_err *err) {
    return fx_commit_body(fx, dir, message, NULL, err);
}

/* --- tree digest --------------------------------------------------------- */

typedef struct name_list {
    char **names;
    size_t count;
    size_t cap;
} name_list;

static int name_cmp(const void *a, const void *b) {
    const char *const *x = (const char *const *)a;
    const char *const *y = (const char *const *)b;
    return strcmp(*x, *y);
}

static atlas_status name_list_push(name_list *l, const char *name, atlas_err *err) {
    if (l->count == l->cap) {
        size_t cap = l->cap != 0 ? l->cap * 2u : 32u;
        char **p = realloc(l->names, cap * sizeof(*p));
        if (p == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory listing a directory");
        }
        l->names = p;
        l->cap = cap;
    }
    char *copy = strdup(name);
    if (copy == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory copying a name");
    }
    l->names[l->count++] = copy;
    return ATLAS_OK;
}

static void name_list_free(name_list *l) {
    for (size_t i = 0; i < l->count; i++) {
        free(l->names[i]);
    }
    free(l->names);
    l->names = NULL;
    l->count = 0;
    l->cap = 0;
}

static atlas_status digest_dir(int dfd, atlas_buf *prefix, atlas_sha256 *ctx, atlas_err *err) {
    int dup_fd = dup(dfd);
    if (dup_fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "dup failed");
    }
    DIR *d = fdopendir(dup_fd);
    if (d == NULL) {
        (void)close(dup_fd);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "fdopendir failed");
    }

    name_list names;
    memset(&names, 0, sizeof(names));
    atlas_status st = ATLAS_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        st = name_list_push(&names, e->d_name, err);
        if (st != ATLAS_OK) {
            break;
        }
    }
    (void)closedir(d);
    if (st != ATLAS_OK) {
        name_list_free(&names);
        return st;
    }
    /* An empty directory leaves names.names NULL, and qsort's first argument is
     * declared non-null. */
    if (names.count > 0) {
        qsort(names.names, names.count, sizeof(names.names[0]), name_cmp);
    }

    for (size_t i = 0; st == ATLAS_OK && i < names.count; i++) {
        const char *name = names.names[i];
        size_t prefix_len = prefix->len;
        st = atlas_buf_append_str(prefix, name, err);
        if (st != ATLAS_OK) {
            break;
        }

        struct stat sb;
        if (fstatat(dfd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            prefix->len = prefix_len;
            continue;
        }
        /* Path, type and permission bits always contribute. */
        atlas_sha256_update(ctx, prefix->data, prefix->len);
        char meta[64];
        int mn = snprintf(meta, sizeof(meta), "|%o|%o|", (unsigned)(sb.st_mode & S_IFMT),
                          (unsigned)(sb.st_mode & 07777));
        atlas_sha256_update(ctx, meta, (size_t)(mn > 0 ? mn : 0));

        if (S_ISDIR(sb.st_mode)) {
            int child = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0) {
                st = atlas_buf_append_ch(prefix, '/', err);
                if (st == ATLAS_OK) {
                    st = digest_dir(child, prefix, ctx, err);
                }
                (void)close(child);
            }
        } else if (S_ISLNK(sb.st_mode)) {
            char target[4096];
            ssize_t tn = readlinkat(dfd, name, target, sizeof(target));
            if (tn > 0) {
                atlas_sha256_update(ctx, target, (size_t)tn);
            }
        } else if (S_ISREG(sb.st_mode)) {
            int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                unsigned char buf[16384];
                for (;;) {
                    ssize_t rn = read(fd, buf, sizeof(buf));
                    if (rn <= 0) {
                        break;
                    }
                    atlas_sha256_update(ctx, buf, (size_t)rn);
                }
                (void)close(fd);
            }
        }
        prefix->len = prefix_len;
        if (prefix->data != NULL) {
            prefix->data[prefix->len] = '\0';
        }
    }
    name_list_free(&names);
    return st;
}

atlas_status fx_tree_digest(const char *dir, char *hex_out, atlas_err *err) {
    int dfd = -1;
    atlas_status st = open_dir(dir, &dfd, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    atlas_buf prefix = ATLAS_BUF_INIT;
    st = digest_dir(dfd, &prefix, &ctx, err);
    atlas_buf_free(&prefix);
    (void)close(dfd);
    if (st != ATLAS_OK) {
        return st;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), hex_out);
    return ATLAS_OK;
}

/* --- running the atlas binary -------------------------------------------- */

static atlas_status fx_atlas_impl(const char *const *args, size_t nargs,
                                 const char *const *extra_env, atlas_buf *stdout_out,
                                 atlas_buf *stderr_out, int *exit_code, atlas_err *err) {
    const char *argv[32];
    size_t n = 0;
    argv[n++] = ATLAS_BIN;
    for (size_t i = 0; i < nargs; i++) {
        if (n + 2u >= sizeof(argv) / sizeof(argv[0])) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "atlas argv is too long");
        }
        argv[n++] = args[i];
    }
    argv[n] = NULL;

    atlas_buf path_env = ATLAS_BUF_INIT;
    const char *path = getenv("PATH");
    atlas_status st = atlas_buf_appendf(&path_env, err, "PATH=%s",
                                        (path != NULL && path[0] != '\0') ? path : "/usr/bin:/bin");
    if (st != ATLAS_OK) {
        atlas_buf_free(&path_env);
        return st;
    }
    /* HOME is deliberately absent: every CLI test must pass --data-dir, and a
     * test that forgets must fail rather than touch a real user directory. The
     * extra entries let a test plant hostile GIT_* variables in Atlas' own
     * environment, which Atlas must not forward to git. */
    const char *env[24];
    size_t envn = 0;
    env[envn++] = atlas_buf_cstr(&path_env);
    env[envn++] = "LC_ALL=C";
    env[envn++] = "TZ=UTC";
    for (size_t i = 0; extra_env != NULL && extra_env[i] != NULL; i++) {
        if (envn + 1u >= sizeof(env) / sizeof(env[0])) {
            atlas_buf_free(&path_env);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "too many extra environment entries");
        }
        env[envn++] = extra_env[i];
    }
    env[envn] = NULL;

    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.env = env;
    opts.timeout_ms = 60000;
    opts.max_stdout = 16u * 1024u * 1024u;

    atlas_proc_result res;
    st = atlas_proc_run(&opts, stdout_out != NULL ? atlas_proc_sink_buf : NULL, stdout_out,
                        stderr_out, &res, err);
    if (st == ATLAS_OK && exit_code != NULL) {
        *exit_code = res.exit_code;
    }
    atlas_buf_free(&path_env);
    return st;
}

atlas_status fx_atlas(const char *const *args, size_t nargs, atlas_buf *stdout_out,
                      atlas_buf *stderr_out, int *exit_code, atlas_err *err) {
    return fx_atlas_impl(args, nargs, NULL, stdout_out, stderr_out, exit_code, err);
}

atlas_status fx_atlas_env(const char *const *args, size_t nargs, const char *const *extra_env,
                          atlas_buf *stdout_out, atlas_buf *stderr_out, int *exit_code,
                          atlas_err *err) {
    return fx_atlas_impl(args, nargs, extra_env, stdout_out, stderr_out, exit_code, err);
}

/* --- adversarial helpers ------------------------------------------------- */

#ifndef ATLAS_MARKER_BIN
#define ATLAS_MARKER_BIN "atlas-marker"
#endif

atlas_status fx_install_marker(const char *dir, const char *name, atlas_buf *helper_out,
                               atlas_buf *marker_out, atlas_err *err) {
    atlas_buf_reset(helper_out);
    atlas_buf_reset(marker_out);
    atlas_status st = atlas_buf_appendf(helper_out, err, "%s/%s", dir, name);
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(marker_out, err, "%s/%s.fired", dir, name);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    /* Copy the built helper so the config value is a bare absolute path with no
     * arguments: git then execs it directly, with no shell involved. */
    int src = open(ATLAS_MARKER_BIN, O_RDONLY | O_CLOEXEC);
    if (src < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot open %s",
                                   ATLAS_MARKER_BIN);
    }
    int dst = open(atlas_buf_cstr(helper_out), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (dst < 0) {
        int e = errno;
        (void)close(src);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, e, "cannot create %s",
                                   atlas_buf_cstr(helper_out));
    }
    char buf[65536];
    for (;;) {
        ssize_t n = read(src, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "read failed");
            break;
        }
        if (n == 0) {
            break;
        }
        const char *p = buf;
        size_t left = (size_t)n;
        while (left > 0) {
            ssize_t w = write(dst, p, left);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "write failed");
                left = 0;
                break;
            }
            p += w;
            left -= (size_t)w;
        }
        if (st != ATLAS_OK) {
            break;
        }
    }
    if (st == ATLAS_OK && fchmod(dst, 0755) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "chmod failed");
    }
    (void)close(src);
    (void)close(dst);
    return st;
}

bool fx_marker_fired(const char *marker_path) {
    struct stat sb;
    return stat(marker_path, &sb) == 0;
}

void fx_marker_clear(const char *marker_path) {
    (void)unlink(marker_path);
}

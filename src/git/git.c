/* Atlas - read-only git adapter.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Safety model (see docs/git-safety.md). Four layers, in order of how much they
 * cover:
 *
 *   1. The child environment is CONSTRUCTED, never inherited, so no GIT_* variable
 *      from Atlas' caller can redirect where git reads from or make it execute
 *      something. Atlas' own environment is never modified.
 *   2. A `-c` prefix closes every configuration-driven execution vector the
 *      *repository* controls: fsmonitor, hooks, external diff, pager, askpass,
 *      signature verification, automatic gc. `-c` outranks every config file.
 *   3. Diff-producing commands additionally refuse external diff drivers,
 *      textconv filters and submodule descent.
 *   4. The subcommand is checked against a read-only allowlist before the fork.
 *
 * The allowlist is the weakest of the four and is deliberately last: it stops
 * Atlas from asking git to write, but it does nothing about git being configured
 * to execute a helper. Layers 1 to 3 are what close that.
 *
 * Every invocation is built by src/git/git_harden.c so the policy lives in one
 * auditable place, and the git executable is resolved once per process.
 */
#include "atlas/git.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/pathrep.h"
#include "atlas/proc.h"
#include "git/git_harden.h"
#include "git/git_parse.h"

#define ATLAS_GIT_SMALL_OUTPUT (4u * 1024u * 1024u)

struct atlas_git {
    atlas_buf exe;
    atlas_buf root;
    atlas_buf common_dir;
    atlas_buf git_dir;
    char object_format[16];
    int root_fd;
    int timeout_ms;
    size_t max_output;

    atlas_buf env_slots[ATLAS_GIT_ENV_MAX];
    const char *env[ATLAS_GIT_ENV_MAX + 1u];
    size_t env_count;

    /* Set when the repository cannot be read without possible network access. */
    bool partial_clone;
    atlas_buf partial_reason;
    /* Objects may live outside this repository; recorded, not refused. */
    bool has_alternates;
};

/* Subcommands Atlas is allowed to run. Everything else is refused before the
 * process is created, including by future callers. */
static const char *const READONLY_SUBCOMMANDS[] = {
    "rev-parse", "ls-files", "log", "status", "diff", "symbolic-ref", "cat-file",
};

static bool is_config_flag(const char *s) {
    return strcmp(s, "-c") == 0 || strcmp(s, "-C") == 0 || strcmp(s, "--git-dir") == 0 ||
           strcmp(s, "--work-tree") == 0;
}

bool atlas_git_argv_is_readonly(const char *const *argv, const char **reason_out) {
    const char *reason = NULL;
    if (argv == NULL || argv[0] == NULL) {
        reason = "empty argv";
        goto deny;
    }
    for (size_t i = 1; argv[i] != NULL; i++) {
        const char *a = argv[i];
        if (is_config_flag(a)) {
            if (argv[i + 1u] == NULL) {
                reason = "option is missing its value";
                goto deny;
            }
            i++; /* skip the value, which is not a subcommand */
            continue;
        }
        if (a[0] == '-') {
            continue;
        }
        for (size_t k = 0; k < sizeof(READONLY_SUBCOMMANDS) / sizeof(READONLY_SUBCOMMANDS[0]);
             k++) {
            if (strcmp(a, READONLY_SUBCOMMANDS[k]) == 0) {
                return true;
            }
        }
        reason = "subcommand is not on the read-only allowlist";
        goto deny;
    }
    /* No subcommand at all (for example `git --version`) cannot modify a
     * repository. */
    return true;

deny:
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    return false;
}

/* --- invocation --------------------------------------------------------- */

/* Recognises the ownership refusal so it can be reported as something the user
 * can act on rather than as an opaque git failure. Atlas cannot use
 * `-c safe.directory=...` for this: git deliberately ignores safe.directory
 * unless it comes from a system or global config file, and Atlas reads neither. */
static bool stderr_is_dubious_ownership(const char *text) {
    return strstr(text, "dubious ownership") != NULL || strstr(text, "safe.directory") != NULL;
}

static atlas_status git_run(atlas_git *g, atlas_git_cmd_kind kind, const char *const *sub,
                            size_t nsub, atlas_proc_sink sink, void *sink_ud,
                            atlas_buf *stderr_out, size_t max_output, atlas_proc_result *res,
                            atlas_err *err) {
    const char *argv[ATLAS_GIT_ARGV_MAX];
    size_t n = 0;
    atlas_status st = atlas_git_build_argv(argv, ATLAS_GIT_ARGV_MAX, &n, atlas_buf_cstr(&g->exe),
                                           atlas_buf_cstr(&g->root), err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (nsub == 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no git subcommand given");
    }

#define PUSH_ARG(v)                                                                  \
    do {                                                                             \
        if (n + 2u >= ATLAS_GIT_ARGV_MAX) {                                           \
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "git argv is too long");     \
        }                                                                            \
        argv[n++] = (v);                                                              \
    } while (0)

    /* sub[0] is the subcommand. The per-kind flags are subcommand options, so they
     * go immediately after it and before the caller's own arguments. */
    PUSH_ARG(sub[0]);
    const char *const *flags = atlas_git_cmd_flags(kind);
    for (size_t i = 0; flags[i] != NULL; i++) {
        PUSH_ARG(flags[i]);
    }
    for (size_t i = 1; i < nsub; i++) {
        PUSH_ARG(sub[i]);
    }
    argv[n] = NULL;
#undef PUSH_ARG

    const char *reason = NULL;
    if (!atlas_git_argv_is_readonly(argv, &reason)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "refusing to run git: %s",
                             reason != NULL ? reason : "not read-only");
    }
    /* The environment was built and asserted at open time; check it again here so
     * that the guarantee holds per invocation. */
    const char *offender = NULL;
    if (!atlas_git_env_is_sanitized(g->env, &offender)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refusing to run git: environment carries %s", offender);
    }

    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.env = g->env;
    opts.timeout_ms = g->timeout_ms;
    opts.max_stdout = max_output != 0 ? max_output : g->max_output;
    opts.max_stderr = 64u * 1024u;
    return atlas_proc_run(&opts, sink, sink_ud, stderr_out, res, err);
}

/* Runs git and requires a zero exit status, reporting stderr on failure. */
static atlas_status git_run_checked(atlas_git *g, atlas_git_cmd_kind kind, const char *const *sub,
                                    size_t nsub, atlas_proc_sink sink, void *sink_ud,
                                    size_t max_output, const char *what, atlas_err *err) {
    atlas_buf errbuf = ATLAS_BUF_INIT;
    atlas_proc_result res;
    atlas_status st =
        git_run(g, kind, sub, nsub, sink, sink_ud, &errbuf, max_output, &res, err);
    if (st == ATLAS_OK && res.exit_code != 0) {
        /* Trim trailing newlines so the message reads as one line. */
        while (errbuf.len > 0 && (errbuf.data[errbuf.len - 1u] == '\n' ||
                                  errbuf.data[errbuf.len - 1u] == '\r')) {
            errbuf.data[--errbuf.len] = '\0';
        }
        if (stderr_is_dubious_ownership(atlas_buf_cstr(&errbuf))) {
            st = atlas_err_set(err, ATLAS_ERR_REPO,
                               "git refused to read %s because it is owned by another user. "
                               "Atlas reads no global or system git configuration, so a "
                               "safe.directory entry there does not apply. Register a "
                               "repository you own, or run Atlas as the owning user.",
                               atlas_buf_cstr(&g->root));
        } else {
            st = atlas_err_set(err, ATLAS_ERR_GIT, "%s failed (git exit %d)%s%s", what,
                               res.exit_code, errbuf.len > 0 ? ": " : "",
                               atlas_buf_cstr(&errbuf));
        }
    }
    atlas_buf_free(&errbuf);
    return st;
}

/* Captures stdout of a small query, tolerating a non-zero exit. */
static atlas_status git_capture(atlas_git *g, const char *const *sub, size_t nsub, atlas_buf *out,
                                int *exit_code_out, atlas_err *err) {
    atlas_buf_reset(out);
    atlas_proc_result res;
    atlas_buf errbuf = ATLAS_BUF_INIT;
    atlas_status st = git_run(g, ATLAS_GIT_CMD_PLAIN, sub, nsub, atlas_proc_sink_buf, out, &errbuf,
                              ATLAS_GIT_SMALL_OUTPUT, &res, err);
    if (st == ATLAS_OK && res.exit_code != 0 &&
        stderr_is_dubious_ownership(atlas_buf_cstr(&errbuf))) {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "git refused to read %s because it is owned by another user. Atlas "
                           "reads no global or system git configuration, so a safe.directory "
                           "entry there does not apply.",
                           atlas_buf_cstr(&g->root));
    }
    atlas_buf_free(&errbuf);
    if (st != ATLAS_OK) {
        return st;
    }
    if (exit_code_out != NULL) {
        *exit_code_out = res.exit_code;
    }
    /* Strip exactly one trailing newline: a path may legitimately contain
     * newlines, so nothing more is removed. */
    if (out->len > 0 && out->data[out->len - 1u] == '\n') {
        out->data[--out->len] = '\0';
    }
    return ATLAS_OK;
}

/* --- probe -------------------------------------------------------------- */

atlas_status atlas_git_probe(atlas_buf *exe_out, atlas_buf *version_out, atlas_err *err) {
    atlas_buf exe = ATLAS_BUF_INIT;
    /* Resolved once per process and cached; PATH is not searched again. */
    atlas_status st = atlas_git_executable(&exe, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&exe);
        return st;
    }
    if (exe_out != NULL) {
        st = atlas_buf_set(exe_out, exe.data, exe.len, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&exe);
            return st;
        }
    }
    if (version_out == NULL) {
        atlas_buf_free(&exe);
        return ATLAS_OK;
    }

    /* Built through the same policy as every other invocation, with no repository
     * context. */
    const char *argv[ATLAS_GIT_ARGV_MAX];
    size_t n = 0;
    st = atlas_git_build_argv(argv, ATLAS_GIT_ARGV_MAX, &n, atlas_buf_cstr(&exe), NULL, err);
    if (st == ATLAS_OK) {
        argv[n++] = "--version";
        argv[n] = NULL;
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&exe);
        return st;
    }

    const char *reason = NULL;
    if (!atlas_git_argv_is_readonly(argv, &reason)) {
        atlas_buf_free(&exe);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "refusing to run git: %s", reason);
    }

    atlas_buf env_slots[ATLAS_GIT_ENV_MAX];
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_init(&env_slots[i]);
    }
    const char *env[ATLAS_GIT_ENV_MAX + 1u];
    size_t env_count = 0;
    st = atlas_git_build_env(env_slots, ATLAS_GIT_ENV_MAX, env, ATLAS_GIT_ENV_MAX + 1u, &env_count,
                             err);
    if (st != ATLAS_OK) {
        goto probe_done;
    }

    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.env = env;
    opts.timeout_ms = 10000;
    opts.max_stdout = 64u * 1024u;
    atlas_buf_reset(version_out);
    atlas_proc_result res;
    st = atlas_proc_run(&opts, atlas_proc_sink_buf, version_out, NULL, &res, err);
    if (st == ATLAS_OK && res.exit_code != 0) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "git --version exited %d", res.exit_code);
    }
    if (st == ATLAS_OK) {
        while (version_out->len > 0 && (version_out->data[version_out->len - 1u] == '\n' ||
                                        version_out->data[version_out->len - 1u] == '\r')) {
            version_out->data[--version_out->len] = '\0';
        }
    }

probe_done:
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_free(&env_slots[i]);
    }
    atlas_buf_free(&exe);
    return st;
}

/* --- partial clone detection -------------------------------------------- */

/* A promisor (partial) repository can be missing objects that git would fetch on
 * demand. Atlas must never touch the network, and git 2.39 has no
 * --no-lazy-fetch, so such a repository is detected and refused before any object
 * is read. Detection is filesystem-only: it needs no extra git subcommand, so it
 * cannot itself trigger a fetch.
 *
 * Two independent markers are checked, and either one is enough:
 *   - a *.promisor file beside a pack in <common-dir>/objects/pack
 *   - the strings "promisor" or "partialclone" in <common-dir>/config
 * The config scan is a deliberately blunt substring match: over-refusing a
 * repository is the safe direction. */
static atlas_status detect_partial_clone(atlas_git *g, atlas_err *err) {
    g->partial_clone = false;
    if (g->common_dir.len == 0) {
        return ATLAS_OK;
    }
    int common_fd = open(atlas_buf_cstr(&g->common_dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (common_fd < 0) {
        /* Not being able to look is not proof of safety, but it is also not proof
         * of a partial clone; the operation itself will fail later if the git dir
         * is unreadable. */
        return ATLAS_OK;
    }

    /* Marker 1: a promisor pack. */
    int pack_fd = openat(common_fd, "objects/pack", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pack_fd >= 0) {
        DIR *d = fdopendir(pack_fd);
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                size_t nlen = strlen(e->d_name);
                if (nlen > 9u && strcmp(e->d_name + nlen - 9u, ".promisor") == 0) {
                    g->partial_clone = true;
                    (void)atlas_buf_set_str(&g->partial_reason,
                                            "a promisor pack file is present in objects/pack", err);
                    break;
                }
            }
            (void)closedir(d);
        } else {
            (void)close(pack_fd);
        }
    }

    /* Marker 2: promisor or partial-clone configuration. */
    if (!g->partial_clone) {
        int cfg = openat(common_fd, "config", O_RDONLY | O_CLOEXEC);
        if (cfg >= 0) {
            /* Bounded read: a config larger than this is not something Atlas will
             * scan, and is refused rather than trusted. */
            char buf[64u * 1024u];
            ssize_t got = read(cfg, buf, sizeof(buf) - 1u);
            (void)close(cfg);
            if (got > 0) {
                buf[got] = '\0';
                for (ssize_t i = 0; i < got; i++) {
                    buf[i] = (char)tolower((unsigned char)buf[i]);
                }
                const char *marker = NULL;
                if (strstr(buf, "promisor") != NULL) {
                    marker = "the repository config mentions a promisor remote";
                } else if (strstr(buf, "partialclone") != NULL) {
                    marker = "the repository config mentions a partial clone filter";
                }
                if (marker != NULL) {
                    g->partial_clone = true;
                    (void)atlas_buf_set_str(&g->partial_reason, marker, err);
                }
            }
        }
    }

    /* Not a refusal, but worth recording: objects may come from elsewhere. */
    struct stat sb;
    g->has_alternates = (fstatat(common_fd, "objects/info/alternates", &sb, 0) == 0);

    (void)close(common_fd);
    return ATLAS_OK;
}

bool atlas_git_is_partial_clone(const atlas_git *g) {
    return g->partial_clone;
}

const char *atlas_git_partial_reason(const atlas_git *g) {
    return atlas_buf_cstr(&g->partial_reason);
}

bool atlas_git_has_alternates(const atlas_git *g) {
    return g->has_alternates;
}

/* --- lifecycle ---------------------------------------------------------- */

atlas_status atlas_git_open(const char *path, atlas_git **out, atlas_err *err) {
    *out = NULL;
    if (path == NULL || path[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no repository path given");
    }

    atlas_git *g = calloc(1u, sizeof(*g));
    if (g == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory opening git adapter");
    }
    atlas_buf_init(&g->exe);
    atlas_buf_init(&g->root);
    atlas_buf_init(&g->common_dir);
    atlas_buf_init(&g->git_dir);
    atlas_buf_init(&g->partial_reason);
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_init(&g->env_slots[i]);
    }
    g->root_fd = -1;
    g->timeout_ms = ATLAS_GIT_DEFAULT_TIMEOUT_MS;
    g->max_output = ATLAS_PROC_DEFAULT_MAX_STDOUT;
    (void)snprintf(g->object_format, sizeof(g->object_format), "unknown");

    atlas_status st = atlas_git_executable(&g->exe, err);
    if (st != ATLAS_OK) {
        goto fail;
    }
    st = atlas_git_build_env(g->env_slots, ATLAS_GIT_ENV_MAX, g->env, ATLAS_GIT_ENV_MAX + 1u,
                             &g->env_count, err);
    if (st != ATLAS_OK) {
        goto fail;
    }

    /* A real directory must exist before git is asked about it, so a typo does
     * not turn into a confusing git error. */
    struct stat pst;
    if (stat(path, &pst) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_REPO, errno, "cannot access %s", path);
        goto fail;
    }
    if (!S_ISDIR(pst.st_mode)) {
        st = atlas_err_set(err, ATLAS_ERR_REPO, "%s is not a directory", path);
        goto fail;
    }

    st = atlas_buf_set_str(&g->root, path, err);
    if (st != ATLAS_OK) {
        goto fail;
    }

    /* Canonical working-tree root. */
    atlas_buf tmp = ATLAS_BUF_INIT;
    {
        const char *sub[] = {"rev-parse", "--show-toplevel"};
        int code = 0;
        st = git_capture(g, sub, 2u, &tmp, &code, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&tmp);
            goto fail;
        }
        if (code != 0 || tmp.len == 0) {
            atlas_buf_free(&tmp);
            st = atlas_err_set(err, ATLAS_ERR_REPO,
                               "%s is not inside a git working tree (a bare repository has no "
                               "working tree to scan)",
                               path);
            goto fail;
        }
        st = atlas_buf_set(&g->root, tmp.data, tmp.len, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&tmp);
            goto fail;
        }
    }

    /* Git common directory, absolute when this git supports --path-format. */
    {
        const char *sub[] = {"rev-parse", "--path-format=absolute", "--git-common-dir"};
        int code = 0;
        st = git_capture(g, sub, 3u, &tmp, &code, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&tmp);
            goto fail;
        }
        if (code != 0 || tmp.len == 0) {
            const char *sub2[] = {"rev-parse", "--git-common-dir"};
            st = git_capture(g, sub2, 2u, &tmp, &code, err);
            if (st != ATLAS_OK) {
                atlas_buf_free(&tmp);
                goto fail;
            }
        }
        if (code == 0 && tmp.len > 0) {
            if (tmp.data[0] == '/') {
                st = atlas_buf_set(&g->common_dir, tmp.data, tmp.len, err);
            } else {
                atlas_buf_reset(&g->common_dir);
                st = atlas_buf_append(&g->common_dir, g->root.data, g->root.len, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_append_ch(&g->common_dir, '/', err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_buf_append(&g->common_dir, tmp.data, tmp.len, err);
                }
            }
            if (st != ATLAS_OK) {
                atlas_buf_free(&tmp);
                goto fail;
            }
        }
    }

    /* This worktree's own git directory. For a linked worktree this differs from
     * the common dir, which is how Atlas tells two worktrees of one repository
     * apart. */
    {
        const char *sub[] = {"rev-parse", "--path-format=absolute", "--git-dir"};
        int code = 0;
        st = git_capture(g, sub, 3u, &tmp, &code, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&tmp);
            goto fail;
        }
        if (code != 0 || tmp.len == 0) {
            const char *sub2[] = {"rev-parse", "--git-dir"};
            st = git_capture(g, sub2, 2u, &tmp, &code, err);
            if (st != ATLAS_OK) {
                atlas_buf_free(&tmp);
                goto fail;
            }
        }
        if (code == 0 && tmp.len > 0) {
            if (tmp.data[0] == '/') {
                st = atlas_buf_set(&g->git_dir, tmp.data, tmp.len, err);
            } else {
                atlas_buf_reset(&g->git_dir);
                st = atlas_buf_append(&g->git_dir, g->root.data, g->root.len, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_append_ch(&g->git_dir, '/', err);
                }
                if (st == ATLAS_OK) {
                    st = atlas_buf_append(&g->git_dir, tmp.data, tmp.len, err);
                }
            }
            if (st != ATLAS_OK) {
                atlas_buf_free(&tmp);
                goto fail;
            }
        }
    }

    /* Object format; older git releases do not know the option at all. */
    {
        const char *sub[] = {"rev-parse", "--show-object-format"};
        int code = 0;
        st = git_capture(g, sub, 2u, &tmp, &code, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&tmp);
            goto fail;
        }
        if (code == 0 && tmp.len > 0 && tmp.len < sizeof(g->object_format)) {
            (void)snprintf(g->object_format, sizeof(g->object_format), "%s",
                           atlas_buf_cstr(&tmp));
        }
    }
    atlas_buf_free(&tmp);

    /* Fail closed on a repository whose objects may not all be local. This runs
     * before any object-reading command. */
    st = detect_partial_clone(g, err);
    if (st != ATLAS_OK) {
        goto fail;
    }
    if (g->partial_clone) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "%s is a partial (promisor) repository: %s. Atlas performs no network "
                           "access and this git cannot be told to refuse a lazy fetch, so "
                           "read-only offline operation cannot be guaranteed. Complete the clone "
                           "(git fetch --refetch, or reclone without a filter) and try again.",
                           atlas_buf_cstr(&g->root), atlas_buf_cstr(&g->partial_reason));
        goto fail;
    }

    g->root_fd = open(atlas_buf_cstr(&g->root), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (g->root_fd < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_REPO, errno, "cannot open repository root %s",
                                 atlas_buf_cstr(&g->root));
        goto fail;
    }

    *out = g;
    return ATLAS_OK;

fail:
    atlas_git_close(g);
    return st;
}

void atlas_git_close(atlas_git *g) {
    if (g == NULL) {
        return;
    }
    if (g->root_fd >= 0) {
        (void)close(g->root_fd);
    }
    atlas_buf_free(&g->exe);
    atlas_buf_free(&g->root);
    atlas_buf_free(&g->common_dir);
    atlas_buf_free(&g->git_dir);
    atlas_buf_free(&g->partial_reason);
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_free(&g->env_slots[i]);
    }
    free(g);
}

const char *atlas_git_root(const atlas_git *g) {
    return atlas_buf_cstr(&g->root);
}

const char *atlas_git_common_dir(const atlas_git *g) {
    return atlas_buf_cstr(&g->common_dir);
}

const char *atlas_git_dir(const atlas_git *g) {
    return atlas_buf_cstr(&g->git_dir);
}

bool atlas_git_is_linked_worktree(const atlas_git *g) {
    /* A linked worktree has its own git dir under the common dir's worktrees/
     * subdirectory; the main worktree's git dir *is* the common dir. */
    return g->git_dir.len > 0 && g->common_dir.len > 0 &&
           (g->git_dir.len != g->common_dir.len ||
            memcmp(g->git_dir.data, g->common_dir.data, g->git_dir.len) != 0);
}

const char *atlas_git_object_format(const atlas_git *g) {
    return g->object_format;
}

const char *atlas_git_exe(const atlas_git *g) {
    return atlas_buf_cstr(&g->exe);
}

int atlas_git_root_fd(const atlas_git *g) {
    return g->root_fd;
}

void atlas_git_set_timeout_ms(atlas_git *g, int ms) {
    g->timeout_ms = ms > 0 ? ms : ATLAS_GIT_DEFAULT_TIMEOUT_MS;
}

void atlas_git_set_max_output(atlas_git *g, size_t bytes) {
    g->max_output = bytes != 0 ? bytes : ATLAS_PROC_DEFAULT_MAX_STDOUT;
}

/* --- HEAD and worktree state -------------------------------------------- */

atlas_status atlas_git_read_head(atlas_git *g, atlas_git_head *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    atlas_buf tmp = ATLAS_BUF_INIT;

    int code = 0;
    const char *rev[] = {"rev-parse", "--verify", "--quiet", "HEAD"};
    atlas_status st = git_capture(g, rev, 4u, &tmp, &code, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&tmp);
        return st;
    }
    bool born = (code == 0 && atlas_git_is_hex_oid(atlas_buf_cstr(&tmp), tmp.len));
    if (born) {
        memcpy(out->oid, tmp.data, tmp.len);
        out->oid[tmp.len] = '\0';
    }

    const char *sym[] = {"symbolic-ref", "--quiet", "--short", "HEAD"};
    st = git_capture(g, sym, 4u, &tmp, &code, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&tmp);
        return st;
    }
    bool on_branch = (code == 0 && tmp.len > 0);
    if (on_branch) {
        if (tmp.len >= sizeof(out->branch)) {
            atlas_buf_free(&tmp);
            return atlas_err_set(err, ATLAS_ERR_GIT, "branch name exceeds %zu bytes",
                                 sizeof(out->branch) - 1u);
        }
        memcpy(out->branch, tmp.data, tmp.len);
        out->branch[tmp.len] = '\0';
    }
    atlas_buf_free(&tmp);

    const char *state = born ? (on_branch ? "born" : "detached") : "unborn";
    (void)snprintf(out->state, sizeof(out->state), "%s", state);
    return ATLAS_OK;
}

typedef struct status_stream {
    atlas_nulsplit split;
} status_stream;

static atlas_status status_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    status_stream *s = (status_stream *)ud;
    return atlas_nulsplit_feed(&s->split, chunk, n, err);
}

const char *atlas_change_scope_name(atlas_change_scope s) {
    switch (s) {
    case ATLAS_SCOPE_STAGED: return "staged";
    case ATLAS_SCOPE_UNSTAGED: return "unstaged";
    case ATLAS_SCOPE_UNTRACKED: return "untracked";
    case ATLAS_SCOPE_UNMERGED: return "unmerged";
    }
    return "unknown";
}

atlas_status atlas_git_read_status(atlas_git *g, atlas_git_worktree_state *out,
                                   atlas_git_status_cb cb, void *ud, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    atlas_status_parser sp;
    atlas_status_parser_init(&sp, out, cb, ud);

    status_stream stream;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, atlas_status_token, &sp);

    /* One invocation yields the branch header, staged and unstaged states per
     * path, renames with their origin, unmerged entries and untracked paths.
     * --untracked-files=normal keeps a wholly untracked directory collapsed to one
     * entry, so an enormous ignored-but-untracked tree cannot blow the output up. */
    const char *sub[] = {"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=normal"};
    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_STATUS, sub, 5u, status_sink, &stream, 0, "git status", err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_status_parser_finish(&sp, err);
    }
    atlas_nulsplit_free(&stream.split);
    atlas_status_parser_free(&sp);
    return st;
}

atlas_status atlas_git_read_worktree_state(atlas_git *g, atlas_git_worktree_state *out,
                                           atlas_err *err) {
    return atlas_git_read_status(g, out, NULL, NULL, err);
}

/* --- tracked files ------------------------------------------------------ */

typedef struct lsfiles_stream {
    atlas_nulsplit split;
} lsfiles_stream;

static atlas_status lsfiles_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    lsfiles_stream *s = (lsfiles_stream *)ud;
    return atlas_nulsplit_feed(&s->split, chunk, n, err);
}

atlas_status atlas_git_ls_files(atlas_git *g, atlas_git_index_cb cb, void *ud, atlas_err *err) {
    atlas_lsfiles_parser lp;
    memset(&lp, 0, sizeof(lp));
    lp.cb = cb;
    lp.ud = ud;

    lsfiles_stream stream;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, atlas_lsfiles_token, &lp);

    const char *sub[] = {"ls-files", "-z", "--stage", "--cached"};
    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_PLAIN, sub, 4u, lsfiles_sink, &stream, 0, "git ls-files", err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    atlas_nulsplit_free(&stream.split);
    return st;
}

/* --- history ------------------------------------------------------------ */

typedef struct log_stream {
    atlas_nulsplit split;
} log_stream;

static atlas_status log_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    log_stream *s = (log_stream *)ud;
    return atlas_nulsplit_feed(&s->split, chunk, n, err);
}

atlas_status atlas_git_log(atlas_git *g, const void *limit_path, size_t limit_path_len,
                           int64_t max_commits, atlas_git_commit_cb commit_cb,
                           atlas_git_change_cb change_cb, void *ud, atlas_err *err) {
    atlas_log_parser lp;
    atlas_log_parser_init(&lp, commit_cb, change_cb, ud);

    log_stream stream;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, atlas_log_token, &lp);

    /* The sentinel byte marks a commit header so a name-status record can never
     * be mistaken for one, and the raw message is last so a message containing
     * the field separator cannot shift the parse. */
    static const char format[] = "--format=\x01%H\x1f%P\x1f%an\x1f%ae\x1f%at\x1f%ct\x1f%B";

    const char *sub[10];
    size_t nsub = 0;
    sub[nsub++] = "log";
    sub[nsub++] = "-z";
    sub[nsub++] = "--name-status";
    sub[nsub++] = "-M";
    sub[nsub++] = "-C";
    sub[nsub++] = "--no-color";
    sub[nsub++] = format;

    char maxbuf[32];
    if (max_commits > 0) {
        (void)snprintf(maxbuf, sizeof(maxbuf), "--max-count=%lld", (long long)max_commits);
        sub[nsub++] = maxbuf;
    }

    atlas_buf pathbuf = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (limit_path != NULL && limit_path_len > 0) {
        if (memchr(limit_path, '\0', limit_path_len) != NULL) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "path contains a NUL byte");
            goto out;
        }
        st = atlas_buf_set(&pathbuf, limit_path, limit_path_len, err);
        if (st != ATLAS_OK) {
            goto out;
        }
        sub[nsub++] = "--";
        sub[nsub++] = atlas_buf_cstr(&pathbuf);
    }

    st = git_run_checked(g, ATLAS_GIT_CMD_DIFF, sub, nsub, log_sink, &stream, 0, "git log", err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_log_parser_finish(&lp, err);
    }

out:
    atlas_buf_free(&pathbuf);
    atlas_nulsplit_free(&stream.split);
    atlas_log_parser_free(&lp);
    return st;
}

/* --- working tree diff -------------------------------------------------- */

typedef struct numstat_stream {
    atlas_nulsplit split;
} numstat_stream;

static atlas_status numstat_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    numstat_stream *s = (numstat_stream *)ud;
    return atlas_nulsplit_feed(&s->split, chunk, n, err);
}

static atlas_status run_numstat(atlas_git *g, const char *const *sub, size_t nsub, const char *what,
                                atlas_git_diff_cb cb, void *ud, atlas_err *err) {
    atlas_numstat_parser np;
    atlas_numstat_parser_init(&np, cb, ud);

    numstat_stream stream;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, atlas_numstat_token, &np);

    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_DIFF, sub, nsub, numstat_sink, &stream, 0, what, err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_numstat_parser_finish(&np, err);
    }
    atlas_nulsplit_free(&stream.split);
    atlas_numstat_parser_free(&np);
    return st;
}

atlas_status atlas_git_diff_worktree(atlas_git *g, atlas_git_diff_cb cb, void *ud, atlas_err *err) {
    const char *sub[] = {"diff", "--numstat", "-z", "--no-color", "-M"};
    return run_numstat(g, sub, 5u, "git diff", cb, ud, err);
}

atlas_status atlas_git_diff_staged(atlas_git *g, atlas_git_diff_cb cb, void *ud, atlas_err *err) {
    const char *sub[] = {"diff", "--cached", "--numstat", "-z", "--no-color", "-M"};
    return run_numstat(g, sub, 6u, "git diff --cached", cb, ud, err);
}

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

/* The `git log` pretty format, shared by the full and incremental walks so the
 * two cannot drift into parsing differently. The sentinel byte marks a commit
 * header so a name-status record can never be mistaken for one, and the raw
 * message is last so a message containing the field separator cannot shift the
 * parse. */
#define ATLAS_LOG_FORMAT "--format=\x01%H\x1f%P\x1f%an\x1f%ae\x1f%at\x1f%ct\x1f%B"

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
 * process is created, including by future callers.
 *
 * `config` is on this list but is NOT covered by it: the subcommand name alone
 * says nothing about whether an invocation reads or writes, since `git config
 * a.b c` writes. Every `config` invocation is instead matched against a
 * positive allowlist of complete argument vectors below. */
static const char *const READONLY_SUBCOMMANDS[] = {
    "rev-parse", "ls-files", "log", "status", "diff", "symbolic-ref", "cat-file", "config",
    /* A8. `ls-tree` reads a commit's tree and writes stdout; it is in the same
     * class as `cat-file`, which was already here. It opens no new vector: it
     * runs no helper, consults no working tree and touches no index — the
     * fsmonitor, hook, external-diff and textconv routes are closed by the `-c`
     * prefix and the constructed environment for every subcommand alike, and
     * `tests/test_git_hardening.c` exercises it against a hostile repository. */
    "ls-tree",
};

/* The only `git config` invocations Atlas may ever make.
 *
 * A positive allowlist of whole argument tails, not a denylist of writing
 * options. A denylist would have to enumerate --add, --replace-all, --unset,
 * --unset-all, --edit, --rename-section, --remove-section, plus whatever a
 * future git adds — and the first one missed is a write to a repository Atlas
 * promised never to modify. Matching the entire vector means a new query has to
 * be added here deliberately, and cannot be smuggled in by a new call site.
 *
 * `--includes` is what makes the answer exact rather than approximate: it makes
 * git honour include.path and includeIf exactly as it would when using the
 * repository itself, so a promisor marker hidden in an included file is found.
 * `--get-regexp` matches against git's own canonicalised key names, which are
 * lowercased for section and variable, so key case variants and arbitrary
 * remote names are covered by the pattern rather than by string matching. */
static const char *const CONFIG_QUERY_PROMISOR[] = {
    "config", "--includes", "--null", "--get-regexp", "^remote\\..*\\.promisor$", NULL,
};
static const char *const CONFIG_QUERY_PARTIAL_FILTER[] = {
    "config", "--includes", "--null", "--get-regexp", "^remote\\..*\\.partialclonefilter$", NULL,
};
static const char *const CONFIG_QUERY_EXTENSIONS[] = {
    "config", "--includes", "--null", "--get-regexp", "^extensions\\.partialclone$", NULL,
};

static const char *const *const CONFIG_ALLOWED_QUERIES[] = {
    CONFIG_QUERY_PROMISOR,
    CONFIG_QUERY_PARTIAL_FILTER,
    CONFIG_QUERY_EXTENSIONS,
};

/* True when argv's tail, starting at the `config` token, is exactly one of the
 * permitted vectors. */
static bool config_tail_is_allowed(const char *const *argv, size_t start) {
    for (size_t q = 0; q < sizeof(CONFIG_ALLOWED_QUERIES) / sizeof(CONFIG_ALLOWED_QUERIES[0]);
         q++) {
        const char *const *want = CONFIG_ALLOWED_QUERIES[q];
        size_t i = 0;
        for (;; i++) {
            if (want[i] == NULL) {
                return argv[start + i] == NULL; /* both ended together */
            }
            if (argv[start + i] == NULL || strcmp(argv[start + i], want[i]) != 0) {
                break;
            }
        }
    }
    return false;
}

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
        if (strcmp(a, "config") == 0) {
            if (config_tail_is_allowed(argv, i)) {
                return true;
            }
            reason = "git config invocation is not one of the permitted read-only queries";
            goto deny;
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
 * can act on rather than as an opaque git failure.
 *
 * **Corrected in A8.** This comment used to say Atlas could not use
 * `-c safe.directory=...` because git ignores it outside system or global
 * config. Measured on git 2.39.5 on this host, that is false: the bare
 * invocation refuses, the `-c` invocation succeeds, and a `-c` naming a
 * different directory still refuses. Every repository invocation now carries a
 * declaration for the exact canonical root Atlas resolved — see the push in
 * `atlas_git_build_argv` — so this refusal should now only be reachable when a
 * path outside the registry is opened. It is kept because that case is real and
 * deserves a message a person can act on. */
static bool stderr_is_dubious_ownership(const char *text) {
    return strstr(text, "dubious ownership") != NULL || strstr(text, "safe.directory") != NULL;
}

static atlas_status git_run(atlas_git *g, atlas_git_cmd_kind kind, const char *const *sub,
                            size_t nsub, atlas_proc_sink sink, void *sink_ud,
                            atlas_buf *stderr_out, size_t max_output, atlas_proc_result *res,
                            atlas_err *err) {
    const char *argv[ATLAS_GIT_ARGV_MAX];
    size_t n = 0;
    /* Lives for the whole call, because argv points into it. */
    char safedir[ATLAS_GIT_SAFEDIR_MAX];
    atlas_status st = atlas_git_build_argv(argv, ATLAS_GIT_ARGV_MAX, &n, atlas_buf_cstr(&g->exe),
                                           atlas_buf_cstr(&g->root), safedir, sizeof(safedir),
                                           err);
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
    st = atlas_git_build_argv(argv, ATLAS_GIT_ARGV_MAX, &n, atlas_buf_cstr(&exe), NULL, NULL,
                              0u, err);
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
 * is read.
 *
 * A0 detected this by reading the first 64 KiB of <common-dir>/config and looking
 * for the substrings "promisor" and "partialclone". That was wrong in five
 * separate ways, each of which is a bypass rather than a false negative in the
 * safe direction:
 *
 *   - a config larger than 64 KiB hid the marker behind padding, and a config can
 *     be padded to any size with comments
 *   - a marker straddling the 64 KiB boundary was split and matched neither half
 *   - $GIT_DIR/config.worktree, which git reads when extensions.worktreeConfig is
 *     set, was never opened at all
 *   - include.path and includeIf files, which git honours as if their contents
 *     were inline, were never opened either
 *   - it was a substring match, so it also *over*-refused any repository with a
 *     branch or remote whose name happened to contain "promisor"
 *
 * It is replaced by asking git itself, which is the only component that knows
 * exactly which files make up this repository's configuration and how they
 * compose. `--includes` makes git resolve include.path and includeIf; the
 * hardened environment already pins GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM to
 * /dev/null, so the answer describes this repository and nothing else.
 *
 * Fail-closed throughout. `git config --get-regexp` exits 0 when it matched and
 * 1 when it did not; every other outcome — a signal, a timeout, a truncated
 * stdout, an exit code git does not document — is treated as "cannot prove this
 * repository is complete" and refuses. The safe direction is to refuse. */

#define ATLAS_PROMISOR_MAX_PACK_ENTRIES 100000u

/* Runs one allowlisted config query. `*matched` is set only when git said so. */
static atlas_status config_query(atlas_git *g, const char *const *sub, size_t nsub,
                                 const char *what, bool *matched, atlas_err *err) {
    *matched = false;
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errbuf = ATLAS_BUF_INIT;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));

    /* A configuration query's output is a handful of lines. A ceiling this small
     * means a config crafted to produce megabytes of matches trips the truncation
     * check below instead of being buffered. */
    atlas_status st = git_run(g, ATLAS_GIT_CMD_PLAIN, sub, nsub, atlas_proc_sink_buf, &out, &errbuf,
                              256u * 1024u, &res, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&out);
        atlas_buf_free(&errbuf);
        return st;
    }
    if (res.timed_out || res.stdout_truncated || res.term_signal != 0) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "cannot determine whether %s is a partial repository: the %s query %s",
                           atlas_buf_cstr(&g->root), what,
                           res.timed_out          ? "timed out"
                           : res.stdout_truncated ? "produced more output than Atlas will read"
                                                  : "was killed by a signal");
    } else if (res.exit_code == 0) {
        *matched = (out.len > 0);
        if (!*matched) {
            /* git reported success with no output. That is not a documented
             * outcome for --get-regexp, so it is ambiguity, and ambiguity
             * refuses. */
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "cannot determine whether %s is a partial repository: the %s query "
                               "succeeded but produced no output",
                               atlas_buf_cstr(&g->root), what);
        }
    } else if (res.exit_code != 1) {
        /* 1 is "no key matched". Anything else is a failure to answer. */
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "cannot determine whether %s is a partial repository: the %s query "
                           "exited %d%s%s",
                           atlas_buf_cstr(&g->root), what, res.exit_code,
                           errbuf.len > 0 ? ": " : "", atlas_buf_cstr(&errbuf));
    }
    atlas_buf_free(&out);
    atlas_buf_free(&errbuf);
    return st;
}

/* Bounded scan for a *.promisor file beside a pack.
 *
 * A promisor pack is proof independent of configuration: it is what git writes
 * when it has fetched with a filter, and it survives the configuration being
 * edited away afterwards. The directory read is bounded so a repository with an
 * absurd number of pack files cannot make this unbounded, and hitting the bound
 * refuses rather than returning "nothing found". */
static atlas_status detect_promisor_pack(atlas_git *g, int common_fd, bool *found, atlas_err *err) {
    *found = false;
    int pack_fd = openat(common_fd, "objects/pack", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pack_fd < 0) {
        if (errno == ENOENT) {
            return ATLAS_OK; /* no pack directory: nothing to find */
        }
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                   "cannot read objects/pack in %s to check for promisor packs",
                                   atlas_buf_cstr(&g->common_dir));
    }
    DIR *d = fdopendir(pack_fd);
    if (d == NULL) {
        atlas_status st = atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                              "cannot enumerate objects/pack in %s",
                                              atlas_buf_cstr(&g->common_dir));
        (void)close(pack_fd);
        return st;
    }
    atlas_status st = ATLAS_OK;
    unsigned seen = 0;
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        if (++seen > ATLAS_PROMISOR_MAX_PACK_ENTRIES) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "objects/pack in %s holds more than %u entries; Atlas cannot prove "
                               "there is no promisor pack and refuses to guess",
                               atlas_buf_cstr(&g->common_dir), ATLAS_PROMISOR_MAX_PACK_ENTRIES);
            break;
        }
        size_t nlen = strlen(e->d_name);
        if (nlen > 9u && strcmp(e->d_name + nlen - 9u, ".promisor") == 0) {
            *found = true;
            break;
        }
    }
    if (st == ATLAS_OK && e == NULL && errno != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                 "cannot finish enumerating objects/pack in %s",
                                 atlas_buf_cstr(&g->common_dir));
    }
    (void)closedir(d);
    return st;
}

static atlas_status detect_partial_clone(atlas_git *g, atlas_err *err) {
    g->partial_clone = false;
    if (g->common_dir.len == 0) {
        /* Without a common dir there is no object store to reason about, and no
         * way to prove the repository is complete. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "cannot locate the git common directory for %s, so Atlas cannot "
                             "verify that its object store is complete",
                             atlas_buf_cstr(&g->root));
    }
    int common_fd = open(atlas_buf_cstr(&g->common_dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (common_fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                   "cannot open the git common directory %s to verify that its "
                                   "object store is complete",
                                   atlas_buf_cstr(&g->common_dir));
    }

    /* Marker 1: a promisor pack on disk. */
    bool found = false;
    atlas_status st = detect_promisor_pack(g, common_fd, &found, err);
    if (st == ATLAS_OK && found) {
        g->partial_clone = true;
        st = atlas_buf_set_str(&g->partial_reason,
                               "a promisor pack file is present in objects/pack", err);
    }

    /* Markers 2 to 4: configuration, resolved by git itself including every file
     * git would include, across the repository config and config.worktree. */
    if (st == ATLAS_OK && !g->partial_clone) {
        static const struct {
            const char *const *argv;
            size_t n;
            const char *what;
            const char *reason;
        } QUERIES[] = {
            {CONFIG_QUERY_PROMISOR, 5u, "promisor remote",
             "a remote is configured as a promisor remote"},
            {CONFIG_QUERY_PARTIAL_FILTER, 5u, "partial clone filter",
             "a remote is configured with a partial clone filter"},
            {CONFIG_QUERY_EXTENSIONS, 5u, "partialclone extension",
             "extensions.partialclone is set"},
        };
        for (size_t i = 0; i < sizeof(QUERIES) / sizeof(QUERIES[0]) && st == ATLAS_OK; i++) {
            bool matched = false;
            st = config_query(g, QUERIES[i].argv, QUERIES[i].n, QUERIES[i].what, &matched, err);
            if (st == ATLAS_OK && matched) {
                g->partial_clone = true;
                st = atlas_buf_set_str(&g->partial_reason, QUERIES[i].reason, err);
                break;
            }
        }
    }

    /* Not a refusal, but worth recording: objects may come from elsewhere. */
    struct stat sb;
    g->has_alternates = (fstatat(common_fd, "objects/info/alternates", &sb, 0) == 0);

    (void)close(common_fd);
    return st;
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

/* --- untracked discovery (A1) ------------------------------------------- */

typedef struct path_stream {
    atlas_nulsplit split;
    atlas_git_path_cb cb;
    void *ud;
    int64_t seen;
} path_stream;

static atlas_status path_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    path_stream *s = (path_stream *)ud;
    if (len == 0) {
        /* git never emits an empty -z record here. An empty one means the output
         * is not what Atlas thinks it is, and guessing would mean indexing the
         * repository root. */
        return atlas_err_set(err, ATLAS_ERR_GIT, "git ls-files produced an empty path record");
    }
    s->seen++;
    if (s->cb == NULL) {
        return ATLAS_OK;
    }
    return s->cb(tok, len, s->ud, err);
}

static atlas_status path_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    path_stream *s = (path_stream *)ud;
    return atlas_nulsplit_feed(&s->split, chunk, n, err);
}

static atlas_status run_path_listing(atlas_git *g, const char *const *sub, size_t nsub,
                                     const char *what, atlas_git_path_cb cb, void *ud,
                                     atlas_err *err) {
    path_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.cb = cb;
    stream.ud = ud;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, path_token, &stream);

    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_PLAIN, sub, nsub, path_sink, &stream, 0, what,
                                      err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    atlas_nulsplit_free(&stream.split);
    return st;
}

atlas_status atlas_git_ls_untracked(atlas_git *g, atlas_git_path_cb cb, void *ud, atlas_err *err) {
    /* --others: not in the index. --exclude-standard: apply .gitignore,
     * .git/info/exclude and the core.excludesFile git would apply. No
     * --directory, so a wholly untracked directory is expanded into its files,
     * which is the whole point of this call. */
    const char *sub[] = {"ls-files", "-z", "--others", "--exclude-standard"};
    return run_path_listing(g, sub, 4u, "git ls-files --others", cb, ud, err);
}

atlas_status atlas_git_ls_ignored(atlas_git *g, atlas_git_path_cb cb, void *ud, atlas_err *err) {
    /* --directory here, deliberately: an ignored node_modules or build tree is
     * reported as one path. Enumerating it file by file is exactly the unbounded
     * work ignoring it was meant to avoid. */
    const char *sub[] = {"ls-files", "-z", "--others", "--ignored", "--exclude-standard",
                         "--directory"};
    return run_path_listing(g, sub, 6u, "git ls-files --ignored", cb, ud, err);
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

    static const char format[] = ATLAS_LOG_FORMAT;

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

/* --- incremental history (A1) ------------------------------------------- */

atlas_status atlas_git_log_since(atlas_git *g, const char *exclude_oid, int64_t max_commits,
                                 atlas_git_commit_cb commit_cb, atlas_git_change_cb change_cb,
                                 void *ud, atlas_err *err) {
    if (exclude_oid == NULL || exclude_oid[0] == '\0') {
        return atlas_git_log(g, NULL, 0, max_commits, commit_cb, change_cb, ud, err);
    }
    /* Validated before it becomes an argument: an object id is the only thing
     * that may appear here, so a revision expression, an option-looking string
     * or a pathspec cannot be smuggled through a stored tip. */
    if (!atlas_git_is_hex_oid(exclude_oid, strlen(exclude_oid))) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refusing to use a stored commit tip that is not a hex object id");
    }

    atlas_log_parser lp;
    atlas_log_parser_init(&lp, commit_cb, change_cb, ud);
    log_stream stream;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, atlas_log_token, &lp);

    char maxbuf[32];
    const char *sub[12];
    size_t nsub = 0;
    sub[nsub++] = "log";
    sub[nsub++] = "-z";
    sub[nsub++] = "--name-status";
    sub[nsub++] = "-M";
    sub[nsub++] = "-C";
    sub[nsub++] = "--no-color";
    sub[nsub++] = ATLAS_LOG_FORMAT;
    if (max_commits > 0) {
        (void)snprintf(maxbuf, sizeof(maxbuf), "--max-count=%lld", (long long)max_commits);
        sub[nsub++] = maxbuf;
    }
    sub[nsub++] = "HEAD";
    sub[nsub++] = "--not";
    sub[nsub++] = exclude_oid;

    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_DIFF, sub, nsub, log_sink, &stream, 0,
                                      "git log (incremental)", err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_log_parser_finish(&lp, err);
    }
    atlas_nulsplit_free(&stream.split);
    atlas_log_parser_free(&lp);
    return st;
}

/* Counts nothing and parses nothing: the question is only whether git produced a
 * commit, so the sink just records that a byte arrived. */
static atlas_status any_output_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    (void)chunk;
    (void)err;
    if (n > 0) {
        *(bool *)ud = true;
    }
    return ATLAS_OK;
}

atlas_status atlas_git_tip_is_stale(atlas_git *g, const char *oid, bool *stale_out,
                                    bool *unknown_out, atlas_err *err) {
    *stale_out = false;
    *unknown_out = false;
    if (oid == NULL || !atlas_git_is_hex_oid(oid, strlen(oid))) {
        *unknown_out = true;
        return ATLAS_OK;
    }

    /* Does the object still exist? A rebase that garbage-collected the old tip
     * leaves it absent, and `log <missing>` would then fail for a reason that
     * has nothing to do with reachability. */
    {
        char spec[ATLAS_OID_HEX_MAX_INCL + 8u];
        (void)snprintf(spec, sizeof(spec), "%s^{commit}", oid);
        const char *sub[] = {"cat-file", "-e", spec};
        atlas_buf sink = ATLAS_BUF_INIT;
        atlas_proc_result res;
        memset(&res, 0, sizeof(res));
        atlas_status st = git_run(g, ATLAS_GIT_CMD_PLAIN, sub, 3u, atlas_proc_sink_buf, &sink, NULL,
                                  ATLAS_GIT_SMALL_OUTPUT, &res, err);
        atlas_buf_free(&sink);
        if (st != ATLAS_OK) {
            return st;
        }
        if (res.exit_code != 0) {
            *unknown_out = true;
            return ATLAS_OK;
        }
    }

    /* `log <tip> --not HEAD` lists what the old tip can reach and HEAD cannot.
     * For an ordinary fast-forward that set is empty. Any output means history
     * moved sideways or backwards: a force-push, a rebase, a reset, a branch
     * switch. That is the signal to stop trusting the stored tip. */
    bool any = false;
    const char *sub[] = {"log", "--max-count=1", "--format=%H", oid, "--not", "HEAD"};
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    atlas_status st = git_run(g, ATLAS_GIT_CMD_DIFF, sub, 6u, any_output_sink, &any, NULL,
                              ATLAS_GIT_SMALL_OUTPUT, &res, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (res.exit_code != 0) {
        /* Cannot answer: treat the tip as unusable rather than assume it is
         * still an ancestor, which would silently skip real commits. */
        *unknown_out = true;
        return ATLAS_OK;
    }
    *stale_out = any;
    return ATLAS_OK;
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


/* --- A8: trusted source snapshotting -------------------------------------
 *
 * These three reads let the dispatcher materialise an exact commit into a
 * worker-owned directory that contains **no git metadata at all**. That absence
 * is the security property: with no `.git` in the workspace there is no
 * repository configuration to be hostile, no hook to run, no alternate pointing
 * at the source's object store, no index to lock and no submodule or LFS
 * machinery to invoke. The worker gets ordinary files.
 */

typedef struct lstree_parser {
    atlas_git_tree_cb cb;
    void *ud;
    int64_t entries;
} lstree_parser;

/* One record of `git ls-tree -r -z -l`: "<mode> <type> <oid> <size>\t<path>".
 *
 * `-l` is what makes the size available before the object is read. Git pads the
 * size field to a fixed width, so the run of spaces before it is variable, and a
 * gitlink carries "-" rather than a number — parsed as unknown, never as zero.
 *
 * Parsed by position rather than by splitting on whitespace, because a path may
 * contain spaces, tabs and newlines — A0's rule about paths being bytes applies
 * here exactly as it does everywhere else. The tab is the only delimiter, and it
 * is the *first* one. */
static atlas_status lstree_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    lstree_parser *lp = (lstree_parser *)ud;
    const char *tab = memchr(tok, '\t', len);
    if (tab == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record: no path separator");
    }
    size_t head_len = (size_t)(tab - tok);
    /* "<mode> <type> <oid>" with single spaces. Mode is 6 octal digits, type is
     * a short word, oid is hex. Anything else is refused rather than guessed. */
    char head[128];
    if (head_len == 0 || head_len >= sizeof(head)) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record header");
    }
    memcpy(head, tok, head_len);
    head[head_len] = '\0';
    char *sp1 = strchr(head, ' ');
    if (sp1 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record header");
    }
    *sp1 = '\0';
    char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record header");
    }
    *sp2 = '\0';
    char *sp3 = strchr(sp2 + 1, ' ');
    if (sp3 == NULL) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record header");
    }
    *sp3 = '\0';
    const char *sz = sp3 + 1;
    while (*sz == ' ') {
        sz++;
    }
    int64_t size = -1;
    if (*sz >= '0' && *sz <= '9') {
        size = 0;
        for (const char *d = sz; *d >= '0' && *d <= '9'; d++) {
            if (size > (INT64_MAX - (*d - '0')) / 10) {
                return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record header");
            }
            size = size * 10 + (*d - '0');
        }
    } else if (*sz != '-') {
        return atlas_err_set(err, ATLAS_ERR_GIT, "malformed ls-tree record header");
    }

    atlas_git_tree_entry e;
    e.mode = head;
    e.type = sp1 + 1;
    e.oid = sp2 + 1;
    e.size = size;
    e.path = tab + 1;
    e.path_len = len - head_len - 1u;
    lp->entries++;
    return lp->cb != NULL ? lp->cb(&e, lp->ud, err) : ATLAS_OK;
}

typedef struct lstree_stream {
    atlas_nulsplit split;
} lstree_stream;

static atlas_status lstree_sink(const char *chunk, size_t n, void *ud, atlas_err *err) {
    lstree_stream *st = (lstree_stream *)ud;
    return atlas_nulsplit_feed(&st->split, chunk, n, err);
}

/* An exact object id and nothing else. A branch name would make the snapshot
 * depend on when it happened to run, which is precisely what pinning a commit
 * into the job specification exists to prevent. */
static bool is_exact_oid(const char *s) {
    if (s == NULL) {
        return false;
    }
    size_t n = strlen(s);
    if (n != 40u && n != 64u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

atlas_status atlas_git_ls_tree(atlas_git *g, const char *commit, atlas_git_tree_cb cb, void *ud,
                               atlas_err *err) {
    if (!is_exact_oid(commit)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a snapshot needs an exact object id, not a reference");
    }
    lstree_parser lp;
    memset(&lp, 0, sizeof(lp));
    lp.cb = cb;
    lp.ud = ud;

    lstree_stream stream;
    atlas_nulsplit_init(&stream.split, ATLAS_GIT_MAX_TOKEN, lstree_token, &lp);

    /* `--full-tree` so the listing is rooted at the commit's tree rather than at
     * whatever directory git thinks it is in; Atlas never changes its own
     * working directory, but saying so removes the question. */
    const char *sub[] = {"ls-tree", "-r", "-z", "-l", "--full-tree", commit};
    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_PLAIN, sub, 6u, lstree_sink, &stream, 0,
                                      "git ls-tree", err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&stream.split, err);
    }
    atlas_nulsplit_free(&stream.split);
    return st;
}

atlas_status atlas_git_commit_tree(atlas_git *g, const char *commit, atlas_buf *out,
                                   atlas_err *err) {
    if (!is_exact_oid(commit)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a snapshot needs an exact commit id, not a reference");
    }
    atlas_buf_reset(out);
    /* `<commit>^{tree}` fails when the object is not a commit in this
     * repository, which is the check as much as the answer. */
    char spec[80];
    (void)snprintf(spec, sizeof(spec), "%s^{tree}", commit);
    const char *sub[] = {"rev-parse", "--verify", "--end-of-options", spec};
    atlas_status st = git_run_checked(g, ATLAS_GIT_CMD_PLAIN, sub, 4u, atlas_proc_sink_buf, out,
                                      4096u, "git rev-parse", err);
    if (st != ATLAS_OK) {
        return st;
    }
    while (out->len > 0 && (out->data[out->len - 1u] == '\n' || out->data[out->len - 1u] == '\r')) {
        out->len--;
        out->data[out->len] = '\0';
    }
    if (!is_exact_oid(atlas_buf_cstr(out))) {
        return atlas_err_set(err, ATLAS_ERR_GIT, "the commit did not resolve to a tree");
    }
    return ATLAS_OK;
}

atlas_status atlas_git_cat_blob(atlas_git *g, const char *oid, atlas_proc_sink sink,
                                void *sink_ud, size_t max, atlas_err *err) {
    if (!is_exact_oid(oid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a blob needs an exact object id");
    }
    /* `blob` rather than a bare oid: naming the expected type means a tree or a
     * commit id cannot be materialised as though it were file content. */
    const char *sub[] = {"cat-file", "blob", oid};
    return git_run_checked(g, ATLAS_GIT_CMD_PLAIN, sub, 3u, sink, sink_ud, max, "git cat-file",
                           err);
}

atlas_status atlas_git_blob_oid_at(atlas_git *g, const char *commit, const void *path,
                                   size_t path_len, atlas_buf *oid_out, bool *found_out,
                                   atlas_err *err) {
    if (oid_out == NULL || found_out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "no destination for a blob lookup");
    }
    atlas_buf_reset(oid_out);
    *found_out = false;
    if (!is_exact_oid(commit)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a blob lookup needs an exact commit id, not a reference");
    }
    if (path == NULL || path_len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a blob lookup needs a path");
    }

    /* "<commit>:<path>", built by hand rather than with a formatter: `path` is
     * raw bytes that may hold any byte but NUL, and a formatter that treats it
     * as a C string would stop at the first embedded byte that looked like
     * one. `commit` is exact hex, so it can never itself contain the colon
     * that separates the two halves. */
    atlas_buf spec = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_append(&spec, commit, strlen(commit), err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&spec, ':', err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&spec, path, path_len, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&spec);
        return st;
    }

    /* `--quiet` so a path this commit does not resolve is a clean "not found"
     * rather than stderr noise -- the same shape as `atlas_git_read_head`'s
     * probe of `HEAD` on an unborn branch. `--end-of-options` so a path that
     * happens to start with '-' is never read as a flag, `atlas_git_commit_tree`'s
     * precedent for the same spec shape. */
    int code = 0;
    const char *sub[] = {"rev-parse", "--verify", "--quiet", "--end-of-options",
                         atlas_buf_cstr(&spec)};
    st = git_capture(g, sub, 5u, oid_out, &code, err);
    atlas_buf_free(&spec);
    if (st != ATLAS_OK) {
        return st;
    }
    if (code == 0 && is_exact_oid(atlas_buf_cstr(oid_out))) {
        *found_out = true;
    } else {
        atlas_buf_reset(oid_out);
    }
    return ATLAS_OK;
}

atlas_status atlas_git_diff_no_index(const char *a, const char *b, atlas_proc_sink sink,
                                     void *sink_ud, size_t max, bool *differed_out,
                                     atlas_err *err) {
    if (differed_out != NULL) {
        *differed_out = false;
    }
    if (a == NULL || b == NULL || a[0] != '/' || b[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "diff --no-index needs two absolute paths");
    }
    atlas_buf exe = ATLAS_BUF_INIT;
    atlas_status st = atlas_git_executable(&exe, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&exe);
        return st;
    }
    /* No repository context at all — the whole point. The hardening prefix is
     * still applied, so hooks, fsmonitor, external diff, textconv, pagers and
     * transports are off exactly as they are for every other invocation. */
    const char *argv[ATLAS_GIT_ARGV_MAX];
    size_t n = 0;
    st = atlas_git_build_argv(argv, ATLAS_GIT_ARGV_MAX, &n, atlas_buf_cstr(&exe), NULL, NULL,
                              0u, err);
    if (st == ATLAS_OK) {
        argv[n++] = "diff";
        const char *const *flags = atlas_git_cmd_flags(ATLAS_GIT_CMD_DIFF);
        for (size_t i = 0; flags[i] != NULL; i++) {
            argv[n++] = flags[i];
        }
        argv[n++] = "--no-index";
        argv[n++] = "--binary";
        argv[n++] = "--no-color";
        argv[n++] = "--";
        argv[n++] = a;
        argv[n++] = b;
        argv[n] = NULL;
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&exe);
        return st;
    }
    const char *reason = NULL;
    if (!atlas_git_argv_is_readonly(argv, &reason)) {
        atlas_buf_free(&exe);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "refusing to run git: %s",
                             reason != NULL ? reason : "not read-only");
    }

    atlas_buf env_slots[ATLAS_GIT_ENV_MAX];
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_init(&env_slots[i]);
    }
    const char *env[ATLAS_GIT_ENV_MAX + 1u];
    size_t env_count = 0;
    st = atlas_git_build_env(env_slots, ATLAS_GIT_ENV_MAX, env, ATLAS_GIT_ENV_MAX + 1u,
                             &env_count, err);
    if (st == ATLAS_OK) {
        atlas_proc_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.argv = argv;
        opts.env = env;
        opts.timeout_ms = 120000;
        opts.max_stdout = max != 0 ? max : (16u * 1024u * 1024u);
        opts.max_stderr = 64u * 1024u;
        atlas_proc_result res;
        atlas_buf errbuf = ATLAS_BUF_INIT;
        st = atlas_proc_run(&opts, sink, sink_ud, &errbuf, &res, err);
        if (st == ATLAS_OK) {
            /* Exit 1 means "there were differences", which is the ordinary
             * case for a job that changed something. Only 0 and 1 are
             * meaningful; anything else is a real failure. */
            if (res.exit_code == 1) {
                if (differed_out != NULL) {
                    *differed_out = true;
                }
            } else if (res.exit_code != 0) {
                st = atlas_err_set(err, ATLAS_ERR_GIT, "git diff --no-index exited %d: %s",
                                   res.exit_code, atlas_buf_cstr(&errbuf));
            }
        }
        atlas_buf_free(&errbuf);
    }
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_free(&env_slots[i]);
    }
    atlas_buf_free(&exe);
    return st;
}

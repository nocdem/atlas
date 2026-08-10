/* Atlas - the hardened git invocation policy.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "git/git_harden.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/proc.h"

/* --- environment --------------------------------------------------------- */

/* Every variable here can either point git at a different repository, change
 * where it reads configuration from, or make it execute a program. Atlas builds
 * the child environment from scratch, so none of these is ever forwarded.
 *
 * Entries ending in '*' are prefix matches (GIT_TRACE, GIT_CONFIG_KEY_0, ...). */
const char *const ATLAS_GIT_FORBIDDEN_ENV[] = {
    /* repository and object-store selectors */
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_COMMON_DIR",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_NAMESPACE",
    "GIT_CEILING_DIRECTORIES",
    "GIT_DISCOVERY_ACROSS_FILESYSTEM",
    /* configuration injection */
    "GIT_CONFIG",
    "GIT_CONFIG_KEY_*",
    "GIT_CONFIG_VALUE_*",
    /* programs git would execute */
    "GIT_EXTERNAL_DIFF",
    "GIT_DIFF_OPTS",
    "GIT_ASKPASS",
    "SSH_ASKPASS",
    "GIT_SSH",
    "GIT_SSH_COMMAND",
    "GIT_EXEC_PATH",
    "GIT_TEMPLATE_DIR",
    "GIT_EDITOR",
    "GIT_SEQUENCE_EDITOR",
    "GIT_ATTR_SOURCE",
    /* tracing writes files and can be pointed anywhere */
    "GIT_TRACE*",
    "GIT_DEBUG*",
    /* the inherited HOME would reintroduce the user's global configuration */
    "HOME",
    "XDG_CONFIG_HOME",
};

size_t atlas_git_forbidden_env_count(void) {
    return sizeof(ATLAS_GIT_FORBIDDEN_ENV) / sizeof(ATLAS_GIT_FORBIDDEN_ENV[0]);
}

/* Matches "NAME" or, for a pattern ending in '*', "PREFIX...". */
static bool env_name_matches(const char *entry, const char *pattern) {
    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1u] == '*') {
        return strncmp(entry, pattern, plen - 1u) == 0;
    }
    /* Compare up to '=' so "GIT_DIR=x" matches the name "GIT_DIR". */
    size_t n = strcspn(entry, "=");
    return n == plen && strncmp(entry, pattern, plen) == 0;
}

bool atlas_git_env_is_sanitized(const char *const *env, const char **offender_out) {
    if (env == NULL) {
        return true; /* an empty environment cannot carry anything */
    }
    size_t count = atlas_git_forbidden_env_count();
    for (size_t i = 0; env[i] != NULL; i++) {
        for (size_t k = 0; k < count; k++) {
            if (env_name_matches(env[i], ATLAS_GIT_FORBIDDEN_ENV[k])) {
                if (offender_out != NULL) {
                    *offender_out = env[i];
                }
                return false;
            }
        }
    }
    return true;
}

/* The complete child environment. Deterministic: it does not depend on Atlas'
 * own environment at all, so a hostile GIT_* variable in the parent cannot reach
 * git, and Atlas' own environment is never mutated. */
static const char *const ATLAS_GIT_ENV[] = {
    /* Fixed minimal PATH. git finds its own subcommands through its compiled
     * exec-path, not PATH; this exists only so that anything git does legitimately
     * need resolves the same way on every run. */
    "PATH=/usr/bin:/bin",
    /* Deterministic, locale-independent, timezone-independent output. */
    "LC_ALL=C",
    "LANG=C",
    "LC_MESSAGES=C",
    "TZ=UTC",
    /* No configuration file is read: not the user's, not the system's. */
    "GIT_CONFIG_GLOBAL=/dev/null",
    "GIT_CONFIG_SYSTEM=/dev/null",
    "GIT_CONFIG_NOSYSTEM=1",
    /* Refuse inherited config-via-environment. Zero pairs means git reads none,
     * and any inherited GIT_CONFIG_KEY_n or GIT_CONFIG_VALUE_n is absent anyway. */
    "GIT_CONFIG_COUNT=0",
    "GIT_ATTR_NOSYSTEM=1",
    /* Never write to the repository while reading it. */
    "GIT_OPTIONAL_LOCKS=0",
    /* Never block on input, and never run an askpass helper. */
    "GIT_TERMINAL_PROMPT=0",
    /* No pager process, belt and braces with --no-pager. */
    "GIT_PAGER=cat",
    "PAGER=cat",
    /* Report true history rather than replacement objects. */
    "GIT_NO_REPLACE_OBJECTS=1",
    /* No network, by any transport. GIT_NO_LAZY_FETCH is honoured by git 2.41 and
     * later and ignored by older versions, so partial clones are additionally
     * detected and refused before any object read. */
    "GIT_ALLOW_PROTOCOL=none",
    "GIT_NO_LAZY_FETCH=1",
    "GIT_FLUSH=1",
};

atlas_status atlas_git_build_env(atlas_buf *slots, size_t slot_count, const char **env_out,
                                 size_t env_out_cap, size_t *env_count_out, atlas_err *err) {
    size_t fixed = sizeof(ATLAS_GIT_ENV) / sizeof(ATLAS_GIT_ENV[0]);
    if (fixed + 1u > env_out_cap || fixed > slot_count) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "git environment needs %zu slots, only %zu available", fixed + 1u,
                             env_out_cap);
    }
    for (size_t i = 0; i < fixed; i++) {
        atlas_buf_reset(&slots[i]);
        atlas_status st = atlas_buf_append_str(&slots[i], ATLAS_GIT_ENV[i], err);
        if (st != ATLAS_OK) {
            return st;
        }
        env_out[i] = atlas_buf_cstr(&slots[i]);
    }
    env_out[fixed] = NULL;
    *env_count_out = fixed;

    /* Assert the policy on the way out, so a future edit that adds a forwarded
     * variable fails here rather than silently widening the attack surface. */
    const char *offender = NULL;
    if (!atlas_git_env_is_sanitized(env_out, &offender)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refusing to run git: environment carries %s",
                             offender != NULL ? offender : "a forbidden variable");
    }
    return ATLAS_OK;
}

/* --- argv --------------------------------------------------------------- */

/* Subcommand options, which must follow the subcommand rather than precede it.
 *
 *   --no-ext-diff            never run an external diff driver
 *   --no-textconv            never run a textconv filter
 *   --ignore-submodules=all  never descend into a submodule, which would bring its
 *                            own configuration and its own helpers into play
 */
static const char *const DIFF_FLAGS[] = {"--no-ext-diff", "--no-textconv",
                                         "--ignore-submodules=all", NULL};
/* git status takes the submodule flag but not the diff-driver flags. */
static const char *const STATUS_FLAGS[] = {"--ignore-submodules=all", NULL};
static const char *const NO_FLAGS[] = {NULL};

const char *const *atlas_git_cmd_flags(atlas_git_cmd_kind kind) {
    switch (kind) {
    case ATLAS_GIT_CMD_DIFF: return DIFF_FLAGS;
    case ATLAS_GIT_CMD_STATUS: return STATUS_FLAGS;
    case ATLAS_GIT_CMD_PLAIN: return NO_FLAGS;
    }
    return NO_FLAGS;
}

atlas_status atlas_git_build_argv(const char **argv, size_t argv_cap, size_t *n, const char *exe,
                                  const char *root, char *safedir, size_t safedir_cap,
                                  atlas_err *err) {
#define PUSH(v)                                                                       \
    do {                                                                              \
        if (*n + 2u >= argv_cap) {                                                     \
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "git argv is too long");     \
        }                                                                             \
        argv[(*n)++] = (v);                                                            \
    } while (0)

    PUSH(exe);
    if (root != NULL) {
        /* Address the repository explicitly; Atlas never changes its own working
         * directory. */
        PUSH("-C");
        PUSH(root);

        /* The trusted safe-directory declaration, and the one thing that lets a
         * service account read a repository owned by somebody else.
         *
         * Git refuses a repository whose owner is not the calling euid unless
         * `safe.directory` names it. A7.1 split the principals — the daemon runs
         * as `atlasd`, the repositories are owned by the operator — and that
         * refusal silently stopped all indexing: 1 030 warnings, and every
         * registered repository serving a scan taken before the cutover.
         *
         * The declaration here is safe because of *where the value comes from*.
         * It is the canonical root Atlas resolved from its own registry — never
         * request text, never a worker message, never an environment variable,
         * never a config file. It names exactly one directory, and git checks it
         * exactly: a declaration for one path does not admit another, which
         * `tests/test_git_hardening.c` asserts.
         *
         * Note for anyone who reads the older comment in `git.c`: it claimed git
         * ignores `safe.directory` from `-c`. That is **not true of git 2.39.5**,
         * measured directly on this host — the bare invocation refuses, the `-c`
         * invocation succeeds, and a `-c` naming a different directory still
         * refuses. The claim has been corrected rather than worked around.
         *
         * Global and system configuration remain unread: GIT_CONFIG_GLOBAL and
         * GIT_CONFIG_SYSTEM stay /dev/null and GIT_CONFIG_NOSYSTEM stays 1, so
         * an operator's or a repository's own `safe.directory` still cannot
         * influence anything. This is the only declaration, and Atlas writes
         * it. */
        if (safedir == NULL || safedir_cap == 0) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "a repository invocation needs a safe.directory buffer");
        }
        int wrote = snprintf(safedir, safedir_cap, "safe.directory=%s", root);
        if (wrote < 0 || (size_t)wrote >= safedir_cap) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "the repository path is too long to declare safely");
        }
        PUSH("-c");
        PUSH(safedir);
    }

    /* Flags, where the git version supports them. All three are present in every
     * git Atlas supports and are checked by the test suite. */
    PUSH("--no-pager");            /* no pager process at all */
    PUSH("--no-optional-locks");   /* never write the index while reading */
    PUSH("--no-replace-objects");  /* report true objects */

    /* Configuration overrides. `-c` outranks every configuration file, including
     * the repository's own, which is the untrusted one. */
    PUSH("-c");
    PUSH("core.fsmonitor=false");  /* never run a filesystem-monitor helper */
    PUSH("-c");
    PUSH("core.hooksPath=/dev/null"); /* never run a repository hook */
    PUSH("-c");
    PUSH("color.ui=false");        /* no colour escapes in output */
    PUSH("-c");
    PUSH("core.pager=cat");
    PUSH("-c");
    PUSH("diff.external=");        /* no external diff program */
    PUSH("-c");
    PUSH("gc.auto=0");             /* no background repacking, which would write */
    PUSH("-c");
    PUSH("maintenance.auto=false");
    PUSH("-c");
    PUSH("log.showSignature=false"); /* no signature-verification helper */
    PUSH("-c");
    PUSH("core.quotePath=false");  /* raw bytes with -z, never quoted */
    PUSH("-c");
    PUSH("advice.detachedHead=false");
    PUSH("-c");
    PUSH("protocol.allow=never");  /* no transport, with GIT_ALLOW_PROTOCOL=none */
    PUSH("-c");
    PUSH("uploadpack.allowFilter=false");
    PUSH("-c");
    PUSH("core.askPass=");         /* no askpass helper */
    return ATLAS_OK;
#undef PUSH
}

/* --- executable resolution ----------------------------------------------
 *
 * A0 cached the resolved git path in two plain globals and read them without
 * synchronisation. That was correct exactly as long as Atlas stayed
 * single-threaded, and A1 makes it not single-threaded: the daemon runs a
 * writer thread, a watcher thread and a worker pool, any of which may open a
 * repository. Two threads racing on `g_git_exe_resolved` is a data race whether
 * or not it ever produces a wrong answer on a given machine, and it would
 * additionally leak the buffer whose ownership the loser transferred.
 *
 * The cache is now immutable after publication and every access is serialised.
 * The mutex is held only for the pointer copy, so the fast path is a few
 * instructions; the PATH search itself happens once.
 *
 * A mutex rather than pthread_once because the resolution can *fail* (git not
 * installed), and a pthread_once initialiser that fails has no way to be
 * retried or to report why. atlas_git_runtime_init() exists so the daemon can
 * force the one-time resolution to happen deterministically, before it creates
 * any thread — after which every reader observes a value that never changes. */
static pthread_mutex_t g_git_exe_lock = PTHREAD_MUTEX_INITIALIZER;
static atlas_buf g_git_exe = ATLAS_BUF_INIT; /* guarded by g_git_exe_lock */
static bool g_git_exe_resolved = false;      /* guarded by g_git_exe_lock */

atlas_status atlas_git_executable(atlas_buf *out, atlas_err *err) {
    atlas_status st = ATLAS_OK;
    if (pthread_mutex_lock(&g_git_exe_lock) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot lock the git runtime cache");
    }
    if (!g_git_exe_resolved) {
        atlas_buf resolved = ATLAS_BUF_INIT;
        /* PATH from Atlas' own environment is used here, and only here, to find
         * the real git. The child never receives it. */
        st = atlas_proc_which("git", getenv("PATH"), &resolved, err);
        if (st == ATLAS_OK) {
            atlas_buf_free(&g_git_exe);
            g_git_exe = resolved; /* ownership moves to the cache */
            g_git_exe_resolved = true;
        } else {
            atlas_buf_free(&resolved);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(out, g_git_exe.data, g_git_exe.len, err);
    }
    (void)pthread_mutex_unlock(&g_git_exe_lock);
    return st;
}

atlas_status atlas_git_runtime_init(atlas_err *err) {
    atlas_buf tmp = ATLAS_BUF_INIT;
    atlas_status st = atlas_git_executable(&tmp, err);
    atlas_buf_free(&tmp);
    return st;
}

void atlas_git_executable_reset(void) {
    /* Test-only. Resetting immutable-after-publication state is by definition not
     * safe while other threads may read it; the suite calls this between cases,
     * with no daemon threads alive. The lock is still taken so the reset itself
     * cannot tear the buffer. */
    (void)pthread_mutex_lock(&g_git_exe_lock);
    atlas_buf_free(&g_git_exe);
    g_git_exe_resolved = false;
    (void)pthread_mutex_unlock(&g_git_exe_lock);
}

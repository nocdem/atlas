/* Atlas - the hardened git invocation policy, in one place.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header. Every production git invocation is built here, so the
 * policy is auditable in one file and cannot be bypassed by a new call site.
 *
 * The argv allowlist alone is not sufficient protection, because git itself can
 * be configured to execute helpers: fsmonitor hooks, external diff drivers,
 * textconv filters, pagers, askpass programs, hooks, and submodule commands that
 * each carry their own configuration. Those vectors are closed by the environment
 * and the `-c` prefix below, not by the allowlist.
 */
#ifndef ATLAS_GIT_HARDEN_H
#define ATLAS_GIT_HARDEN_H

#include <stdbool.h>
#include <stddef.h>

#include "atlas/buf.h"
#include "atlas/error.h"

/* Maximum entries in the constructed child environment. */
#define ATLAS_GIT_ENV_MAX 32u
/* Maximum tokens in a built argv, including the hardening prefix. */
#define ATLAS_GIT_ARGV_MAX 80u
/* Room for "safe.directory=" plus a canonical repository path. */
#define ATLAS_GIT_SAFEDIR_MAX 4200u

/* Which family a subcommand belongs to, which decides the extra flags it needs. */
typedef enum atlas_git_cmd_kind {
    /* Reads refs, the index or config only: rev-parse, ls-files, symbolic-ref. */
    ATLAS_GIT_CMD_PLAIN = 0,
    /* Produces a diff, directly or as part of another command: diff, log, show.
     * These must never run an external diff, a textconv filter, or descend into a
     * submodule with its own configuration. */
    ATLAS_GIT_CMD_DIFF,
    /* git status: compares the working tree, and takes its own submodule flag. */
    ATLAS_GIT_CMD_STATUS
} atlas_git_cmd_kind;

/* The environment variables Atlas refuses to pass to git, because each one can
 * redirect where git reads from or cause git to execute something. The child
 * environment is built from scratch, so absence is the default; this list exists
 * so it can be asserted rather than assumed. */
extern const char *const ATLAS_GIT_FORBIDDEN_ENV[];
size_t atlas_git_forbidden_env_count(void);

/* True when `env` contains no forbidden variable. On failure `*offender_out`
 * names the entry. Exposed for tests. */
bool atlas_git_env_is_sanitized(const char *const *env, const char **offender_out);

/* Builds the deterministic child environment. Nothing is inherited from Atlas'
 * own environment, and Atlas' environment is never modified. */
atlas_status atlas_git_build_env(atlas_buf *slots, size_t slot_count, const char **env_out,
                                 size_t env_out_cap, size_t *env_count_out, atlas_err *err);

/* Appends the global hardening prefix to `argv`: the executable, the repository
 * selector, the global flags and the `-c` overrides. Everything here is valid
 * *before* the subcommand. `root` may be NULL for an invocation with no
 * repository context (such as --version). */
/* `safedir` receives the `safe.directory=<root>` token when `root` is non-NULL,
 * and must outlive `argv`. See the comment at the `-c safe.directory` push in
 * git_harden.c for why this is the trusted declaration and not a bypass. */
atlas_status atlas_git_build_argv(const char **argv, size_t argv_cap, size_t *n,
                                  const char *exe, const char *root, char *safedir,
                                  size_t safedir_cap, atlas_err *err);

/* The flags a command of this kind needs *after* its subcommand, because they are
 * subcommand options rather than global ones. Returns a NULL-terminated array,
 * empty for ATLAS_GIT_CMD_PLAIN. */
const char *const *atlas_git_cmd_flags(atlas_git_cmd_kind kind);

/* Resolves the git executable once per process and caches it. Subsequent calls
 * copy the cached path without searching PATH again. Safe to call from any
 * thread: the cache is immutable once published and every access is serialised. */
atlas_status atlas_git_executable(atlas_buf *out, atlas_err *err);

/* Forces the one-time resolution to happen now.
 *
 * The daemon calls this before it creates any thread, so that the git runtime
 * state is frozen before concurrency begins and a missing git is reported once,
 * at startup, rather than by whichever worker happened to need it first. */
atlas_status atlas_git_runtime_init(atlas_err *err);

/* Releases the cached path. Test-only, and only valid with no other thread
 * running: it un-publishes state that is otherwise immutable. */
void atlas_git_executable_reset(void);

#endif /* ATLAS_GIT_HARDEN_H */

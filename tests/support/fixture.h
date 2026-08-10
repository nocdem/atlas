/* Atlas - integration test fixtures.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Fixtures create real git repositories in temporary directories. They drive git
 * through atlas_proc with explicit argument arrays, so there is no shell here
 * either. Fixture repositories are written to by the fixture itself; the Atlas
 * git adapter under test remains restricted to read-only commands.
 *
 * Every test must resolve its data directory to a temporary path, so the real
 * user database is never opened by the suite.
 */
#ifndef ATLAS_TEST_FIXTURE_H
#define ATLAS_TEST_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "atlas/buf.h"
#include "atlas/error.h"

typedef struct fixture {
    atlas_buf root;     /* temporary directory owned by this fixture */
    atlas_buf repo;     /* <root>/repo */
    atlas_buf data_dir; /* <root>/data */
    int commit_seq;     /* drives deterministic commit timestamps */
} fixture;

/* Creates the temporary tree. Does not create a git repository. */
atlas_status fx_open(fixture *fx, atlas_err *err);
/* Removes the whole temporary tree. */
void fx_close(fixture *fx);

/* Removes any temporary tree whose fx_close() never ran. A test that fails an
 * assertion abandons the rest of its body, so the harness calls this after every
 * test to keep a failing suite from accumulating directories under TMPDIR. */
void fx_cleanup_leaked(void);

/* Disowns everything this process inherited from its parent across fork().
 *
 * A forked child shares the fixture's bookkeeping with its parent: the live
 * temporary trees, the tracked daemon pids and the process-private runtime
 * directory. The exit handler that releases those belongs to whichever process
 * created them, so a child that means to create its own must first stop being a
 * second claimant on its parent's. Nothing on disk is touched — this only
 * forgets.
 *
 * A child that immediately execs does not need this: exec discards the exit
 * handler along with the rest of the image. Only a child that keeps running
 * fixture code does. */
void fx_reset_after_fork(void);

const char *fx_repo(const fixture *fx);
const char *fx_data_dir(const fixture *fx);

/* Runs git in `dir` with the given arguments (no "-C" needed). Returns the
 * child's exit code through `exit_code`; a non-zero exit is not an error here. */
atlas_status fx_git(const fixture *fx, const char *dir, const char *const *args, size_t nargs,
                    int *exit_code, atlas_buf *stdout_out, atlas_err *err);
/* Same, but fails when git exits non-zero. */
atlas_status fx_git_ok(const fixture *fx, const char *dir, const char *const *args, size_t nargs,
                       atlas_err *err);

/* `git init` with an explicit initial branch. `object_format` may be NULL for
 * the default, or "sha1"/"sha256". Returns ATLAS_ERR_CONFIG when this git cannot
 * create the requested object format, so a test can skip instead of failing. */
atlas_status fx_init_repo(fixture *fx, const char *dir, const char *object_format, atlas_err *err);

/* Working-tree helpers. `rel` is used as raw bytes, so it may contain spaces,
 * tabs, newlines or non-UTF-8. */
atlas_status fx_write_bytes(const char *dir, const void *rel, size_t rel_len, const void *data,
                           size_t n, mode_t mode, atlas_err *err);
atlas_status fx_write(const char *dir, const char *rel, const char *contents, atlas_err *err);
atlas_status fx_write_exec(const char *dir, const char *rel, const char *contents, atlas_err *err);
atlas_status fx_mkdir(const char *dir, const char *rel, atlas_err *err);
atlas_status fx_symlink(const char *dir, const char *target, const char *linkname, atlas_err *err);
atlas_status fx_remove(const char *dir, const char *rel, atlas_err *err);
atlas_status fx_chmod(const char *dir, const char *rel, mode_t mode, atlas_err *err);
/* True when the filesystem accepted a filename with the given raw bytes. */
bool fx_can_create_name(const char *dir, const void *rel, size_t rel_len);

atlas_status fx_add_all(const fixture *fx, const char *dir, atlas_err *err);
/* Commits with deterministic, strictly increasing author and committer dates. */
atlas_status fx_commit(fixture *fx, const char *dir, const char *message, atlas_err *err);
atlas_status fx_commit_body(fixture *fx, const char *dir, const char *subject, const char *body,
                           atlas_err *err);

/* Deterministic digest of a directory tree: relative paths, entry types,
 * permission bits, symlink targets and file contents, in sorted order. Used to
 * prove that read commands do not modify a repository. */
atlas_status fx_tree_digest(const char *dir, char *hex_out, atlas_err *err);

/* Runs the atlas binary with the given arguments, capturing stdout and stderr.
 * ATLAS_BIN is defined by the build. --data-dir is NOT added automatically. */
atlas_status fx_atlas(const char *const *args, size_t nargs, atlas_buf *stdout_out,
                      atlas_buf *stderr_out, int *exit_code, atlas_err *err);

/* --- A2: running an adapter that reads stdin ------------------------------
 *
 * `atlas hook` takes its whole input on stdin, and `atlas mcp` takes a stream of
 * messages there. Neither can go through fx_atlas: atlas_proc_run points a
 * child's stdin at /dev/null by design, which is right for git and wrong for an
 * adapter.
 *
 * So this forks directly, keeping the same discipline as everything else in the
 * suite — an explicit argv, an explicitly constructed environment, no shell —
 * and adds a pipe. stdout and stderr are captured separately, which is what lets
 * a test assert that a diagnostic went to stderr *and* that stdout carried only
 * protocol. `extra_env` is a NULL-terminated "K=V" list. */
atlas_status fx_atlas_stdin(const char *const *args, size_t nargs, const char *const *extra_env,
                            const void *payload, size_t payload_len, atlas_buf *stdout_out,
                            atlas_buf *stderr_out, int *exit_code, atlas_err *err);


/* --- A1: a live daemon under test ---------------------------------------
 *
 * The suite never installs, enables or starts a real systemd service. It forks
 * the built binary directly with an isolated data directory and an isolated
 * XDG_RUNTIME_DIR, so the socket, the lock and the index all live inside the
 * fixture's temporary tree and nothing touches the developer's account. */

typedef struct fx_daemon {
    pid_t pid;
    atlas_buf runtime_dir; /* the fixture's private XDG_RUNTIME_DIR */
    atlas_buf socket;      /* <runtime_dir>/atlas/atlas.sock */
    atlas_buf log_path;
} fx_daemon;

void fx_daemon_init(fx_daemon *d);
/* Forks `atlas daemon run --data-dir <fixture data>` with XDG_RUNTIME_DIR set
 * inside the fixture. Does not wait for it to be ready. */
atlas_status fx_daemon_start(fixture *fx, fx_daemon *d, atlas_err *err);
/* Polls the socket until the daemon answers a ping, or the deadline passes. */
atlas_status fx_daemon_wait_ready(fx_daemon *d, int timeout_ms, atlas_err *err);
/* SIGTERM, then wait. `hard` sends SIGKILL instead, to exercise crash recovery. */
void fx_daemon_stop(fx_daemon *d, bool hard);
void fx_daemon_free(fx_daemon *d);
/* Reads the daemon's captured log, for assertions about what it reported. */
atlas_status fx_daemon_log(const fx_daemon *d, atlas_buf *out, atlas_err *err);
/* True when the daemon process has exited. */
bool fx_daemon_exited(fx_daemon *d);

/* Runs the atlas binary with the fixture's data directory and the daemon's
 * runtime directory, so a CLI invocation reaches this daemon and no other. */
atlas_status fx_atlas_with_runtime(const fixture *fx, const fx_daemon *d, const char *const *args,
                                   size_t nargs, atlas_buf *stdout_out, atlas_buf *stderr_out,
                                   int *exit_code, atlas_err *err);

/* One raw request/response round trip, so the framing can be driven directly
 * with bytes the client library would never send. */
atlas_status fx_ipc_raw(const char *socket_path, const void *frame, size_t len,
                        atlas_buf *response_out, bool *closed_out, atlas_err *err);

/* Waits until `predicate` holds over the JSON of `atlas events NAME --json`, or
 * the deadline passes. Used so watcher tests wait for an observable outcome
 * rather than sleeping a guessed interval. */
atlas_status fx_wait_for_substring(const fixture *fx, const fx_daemon *d, const char *const *args,
                                   size_t nargs, const char *needle, int timeout_ms, bool *found,
                                   atlas_err *err);

/* --- adversarial helpers ------------------------------------------------- */

/* Installs the marker helper (see tests/tools/atlas_marker.c) into `dir` under
 * `name`, so a repository config can point at it as an absolute path with no
 * arguments. `helper_out` receives that path and `marker_out` the path the helper
 * would create if it ever ran. */
atlas_status fx_install_marker(const char *dir, const char *name, atlas_buf *helper_out,
                               atlas_buf *marker_out, atlas_err *err);

/* True when the marker helper has run. */
bool fx_marker_fired(const char *marker_path);
/* Removes the marker so a later assertion starts clean. */
void fx_marker_clear(const char *marker_path);

/* Runs the atlas binary with additional variables in its inherited environment,
 * to prove that Atlas does not forward them to git. `extra_env` is a
 * NULL-terminated list of "K=V" strings. */
atlas_status fx_atlas_env(const char *const *args, size_t nargs, const char *const *extra_env,
                          atlas_buf *stdout_out, atlas_buf *stderr_out, int *exit_code,
                          atlas_err *err);
#endif /* ATLAS_TEST_FIXTURE_H */

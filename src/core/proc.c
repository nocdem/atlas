/* Atlas - safe subprocess execution.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * There is deliberately no code path in this file that can reach a shell:
 * execve() is called with the caller's argv array and an explicitly built
 * environment, and argv[0] must already be an absolute path.
 */
#include "atlas/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ATLAS_PROC_READ_CHUNK (64u * 1024u)
#define ATLAS_PROC_KILL_GRACE_MS 200

static const char *const ATLAS_DEFAULT_PATH = "/usr/local/bin:/usr/bin:/bin";

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static bool is_executable_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    return access(path, X_OK) == 0;
}

atlas_status atlas_proc_which(const char *program, const char *path_env, atlas_buf *out,
                              atlas_err *err) {
    if (program == NULL || program[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "empty program name");
    }
    atlas_buf_reset(out);
    if (strchr(program, '/') != NULL) {
        if (!is_executable_regular_file(program)) {
            return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s is not an executable file", program);
        }
        return atlas_buf_append_str(out, program, err);
    }

    const char *search = (path_env != NULL && path_env[0] != '\0') ? path_env : ATLAS_DEFAULT_PATH;
    const char *p = search;
    atlas_buf cand = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t seg_len = (colon != NULL) ? (size_t)(colon - p) : strlen(p);
        /* An empty PATH element means the current directory. Atlas refuses to
         * search it: a repository must never be able to shadow git. */
        if (seg_len > 0 && p[0] == '/') {
            atlas_buf_reset(&cand);
            st = atlas_buf_append(&cand, p, seg_len, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_append_ch(&cand, '/', err);
            }
            if (st == ATLAS_OK) {
                st = atlas_buf_append_str(&cand, program, err);
            }
            if (st != ATLAS_OK) {
                atlas_buf_free(&cand);
                return st;
            }
            if (is_executable_regular_file(atlas_buf_cstr(&cand))) {
                st = atlas_buf_append(out, cand.data, cand.len, err);
                atlas_buf_free(&cand);
                return st;
            }
        }
        if (colon == NULL) {
            break;
        }
        p = colon + 1;
    }
    atlas_buf_free(&cand);
    return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s not found in PATH", program);
}

atlas_status atlas_proc_sink_buf(const char *chunk, size_t n, void *ud, atlas_err *err) {
    return atlas_buf_append((atlas_buf *)ud, chunk, n, err);
}

static void set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

/* execve() predates const-correctness: it takes char *const argv[] but does not
 * modify the strings. This launders the type without a const-discarding cast. */
static char *const *drop_const(const char *const *p) {
    union {
        const char *const *in;
        char *const *out;
    } u;
    u.in = p;
    return u.out;
}

/* Everything between fork() and execve() must stay simple and allocation-free. */
static void child_exec(const atlas_proc_opts *opts, int out_w, int err_w, int status_w) {
    (void)setpgid(0, 0);

    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) {
        int e = errno;
        (void)!write(status_w, &e, sizeof(e));
        _exit(127);
    }
    if (dup2(devnull, STDIN_FILENO) < 0 || dup2(out_w, STDOUT_FILENO) < 0 ||
        dup2(err_w, STDERR_FILENO) < 0) {
        int e = errno;
        (void)!write(status_w, &e, sizeof(e));
        _exit(127);
    }
    (void)close(devnull);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    (void)sigaction(SIGPIPE, &sa, NULL);
    sigset_t empty;
    (void)sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);

    static char *const empty_env[] = {NULL};
    char *const *envp = (opts->env != NULL) ? drop_const(opts->env) : empty_env;
    (void)execve(opts->argv[0], drop_const(opts->argv), envp);

    int e = errno;
    (void)!write(status_w, &e, sizeof(e));
    _exit(127);
}

static void kill_group(pid_t pid, int sig) {
    /* The child called setpgid(0,0), so its pgid equals its pid. Signal the
     * whole group so grandchildren cannot survive. */
    if (kill(-pid, sig) != 0 && errno == ESRCH) {
        (void)kill(pid, sig);
    }
}

static void reap(pid_t pid, atlas_proc_result *res) {
    int wstatus = 0;
    for (;;) {
        pid_t r = waitpid(pid, &wstatus, 0);
        if (r == pid) {
            break;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
    if (WIFEXITED(wstatus)) {
        res->exit_code = WEXITSTATUS(wstatus);
        res->term_signal = 0;
    } else if (WIFSIGNALED(wstatus)) {
        res->exit_code = -1;
        res->term_signal = WTERMSIG(wstatus);
    }
}

atlas_status atlas_proc_run(const atlas_proc_opts *opts, atlas_proc_sink sink, void *sink_ud,
                            atlas_buf *stderr_out, atlas_proc_result *res, atlas_err *err) {
    atlas_proc_result local_res;
    memset(&local_res, 0, sizeof(local_res));
    local_res.exit_code = -1;
    if (res != NULL) {
        *res = local_res;
    }

    if (opts == NULL || opts->argv == NULL || opts->argv[0] == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no program to execute");
    }
    if (opts->argv[0][0] != '/') {
        /* Callers resolve the executable with atlas_proc_which() first; this
         * keeps PATH resolution in one auditable place. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "argv[0] must be an absolute path, got \"%s\"", opts->argv[0]);
    }

    int timeout_ms = opts->timeout_ms > 0 ? opts->timeout_ms : ATLAS_PROC_DEFAULT_TIMEOUT_MS;
    size_t max_stdout = opts->max_stdout != 0 ? opts->max_stdout : ATLAS_PROC_DEFAULT_MAX_STDOUT;
    size_t max_stderr = opts->max_stderr != 0 ? opts->max_stderr : ATLAS_PROC_DEFAULT_MAX_STDERR;

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int status_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "pipe failed");
    }
    if (pipe(err_pipe) != 0) {
        int e = errno;
        (void)close(out_pipe[0]);
        (void)close(out_pipe[1]);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, e, "pipe failed");
    }
    if (pipe(status_pipe) != 0) {
        int e = errno;
        (void)close(out_pipe[0]);
        (void)close(out_pipe[1]);
        (void)close(err_pipe[0]);
        (void)close(err_pipe[1]);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, e, "pipe failed");
    }
    set_cloexec(out_pipe[0]);
    set_cloexec(err_pipe[0]);
    set_cloexec(status_pipe[0]);
    set_cloexec(status_pipe[1]); /* closed by a successful execve */

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        (void)close(out_pipe[0]);
        (void)close(out_pipe[1]);
        (void)close(err_pipe[0]);
        (void)close(err_pipe[1]);
        (void)close(status_pipe[0]);
        (void)close(status_pipe[1]);
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, e, "fork failed");
    }
    if (pid == 0) {
        (void)close(out_pipe[0]);
        (void)close(err_pipe[0]);
        (void)close(status_pipe[0]);
        child_exec(opts, out_pipe[1], err_pipe[1], status_pipe[1]);
        _exit(127); /* not reached */
    }

    (void)close(out_pipe[1]);
    (void)close(err_pipe[1]);
    (void)close(status_pipe[1]);

    /* Read the exec status first: it is either closed on success or carries a
     * single errno on failure. */
    int child_errno = 0;
    {
        ssize_t n = read(status_pipe[0], &child_errno, sizeof(child_errno));
        (void)close(status_pipe[0]);
        if (n == (ssize_t)sizeof(child_errno)) {
            (void)close(out_pipe[0]);
            (void)close(err_pipe[0]);
            kill_group(pid, SIGKILL);
            reap(pid, &local_res);
            if (res != NULL) {
                *res = local_res;
            }
            return atlas_err_set_errno(err, ATLAS_ERR_GIT, child_errno, "cannot execute %s",
                                       opts->argv[0]);
        }
    }

    atlas_status st = ATLAS_OK;
    int64_t deadline = now_ms() + (int64_t)timeout_ms;
    char *chunk = malloc(ATLAS_PROC_READ_CHUNK);
    if (chunk == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory allocating read buffer");
    }

    bool out_open = (chunk != NULL);
    bool err_open = (chunk != NULL);
    bool kill_requested = (chunk == NULL);
    size_t stderr_seen = 0;

    while (out_open || err_open) {
        struct pollfd fds[2];
        int nfds = 0;
        int out_idx = -1;
        int err_idx = -1;
        if (out_open) {
            fds[nfds].fd = out_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            out_idx = nfds++;
        }
        if (err_open) {
            fds[nfds].fd = err_pipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            err_idx = nfds++;
        }

        int64_t remaining = deadline - now_ms();
        if (remaining < 0) {
            remaining = 0;
        }
        int poll_timeout = kill_requested ? 50 : (int)(remaining > 3600000 ? 3600000 : remaining);
        int pr = poll(fds, (nfds_t)nfds, poll_timeout);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "poll failed");
            break;
        }
        if (pr == 0) {
            if (kill_requested) {
                /* Waiting only for the pipes to drain after a kill. */
                continue;
            }
            local_res.timed_out = true;
            kill_requested = true;
            kill_group(pid, SIGTERM);
            deadline = now_ms() + ATLAS_PROC_KILL_GRACE_MS;
            continue;
        }

        if (out_idx >= 0 && fds[out_idx].revents != 0) {
            ssize_t n = read(out_pipe[0], chunk, ATLAS_PROC_READ_CHUNK);
            if (n > 0) {
                local_res.stdout_bytes += (size_t)n;
                if (local_res.stdout_bytes > max_stdout) {
                    local_res.stdout_truncated = true;
                    if (!kill_requested) {
                        kill_requested = true;
                        kill_group(pid, SIGKILL);
                    }
                    st = atlas_err_set(err, ATLAS_ERR_GIT,
                                       "output of %s exceeded the %zu byte limit", opts->argv[0],
                                       max_stdout);
                    break;
                }
                if (sink != NULL) {
                    st = sink(chunk, (size_t)n, sink_ud, err);
                    if (st != ATLAS_OK) {
                        if (!kill_requested) {
                            kill_requested = true;
                            kill_group(pid, SIGKILL);
                        }
                        break;
                    }
                }
            } else if (n == 0) {
                out_open = false;
            } else if (errno != EINTR && errno != EAGAIN) {
                st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "read failed");
                break;
            }
        }

        if (err_idx >= 0 && fds[err_idx].revents != 0) {
            ssize_t n = read(err_pipe[0], chunk, ATLAS_PROC_READ_CHUNK);
            if (n > 0) {
                if (stderr_out != NULL && stderr_seen < max_stderr) {
                    size_t room = max_stderr - stderr_seen;
                    size_t take = ((size_t)n < room) ? (size_t)n : room;
                    atlas_status ast = atlas_buf_append(stderr_out, chunk, take, err);
                    if (ast != ATLAS_OK) {
                        st = ast;
                        if (!kill_requested) {
                            kill_requested = true;
                            kill_group(pid, SIGKILL);
                        }
                        break;
                    }
                }
                stderr_seen += (size_t)n;
            } else if (n == 0) {
                err_open = false;
            } else if (errno != EINTR && errno != EAGAIN) {
                err_open = false;
            }
        }

        if (kill_requested && now_ms() >= deadline) {
            kill_group(pid, SIGKILL);
            deadline = now_ms() + 3600000; /* only drain from here on */
        }
    }

    free(chunk);
    (void)close(out_pipe[0]);
    (void)close(err_pipe[0]);

    if (kill_requested) {
        kill_group(pid, SIGKILL);
    }
    reap(pid, &local_res);

    if (local_res.timed_out && st == ATLAS_OK) {
        st = atlas_err_set(err, ATLAS_ERR_GIT, "%s timed out after %d ms", opts->argv[0],
                           timeout_ms);
    }
    if (res != NULL) {
        *res = local_res;
    }
    if (err != NULL) {
        err->exit_code = local_res.exit_code;
    }
    return st;
}

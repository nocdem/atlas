/* Atlas - a real pseudo-terminal for the operator-channel test suites.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See pty.h. Moved out of tests/test_decision_operator.c (A15 T6), unchanged
 * in behaviour.
 */
#define _GNU_SOURCE 1

#include "support/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"

atlas_status pty_spawn(const char *data_dir, const char *bin_path, const char *const *args,
                       size_t nargs, pty *out, atlas_err *err) {
    out->master = -1;
    out->child = -1;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "posix_openpt: %s", strerror(errno));
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "grantpt/unlockpt: %s", strerror(errno));
    }
    char slave_name[128];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "ptsname_r: %s", strerror(errno));
    }

    /* Built here, before the fork: allocating in a forked child is exactly the
     * thing not to do, and the argv has to outlive the fork anyway. */
    const char *argv[24];
    size_t k = 0;
    argv[k++] = bin_path;
    argv[k++] = "--data-dir";
    argv[k++] = data_dir;
    for (size_t i = 0; i < nargs; i++) {
        argv[k++] = args[i];
    }
    argv[k] = NULL;
    /* An explicitly constructed environment, like everywhere else in the
     * suite: nothing inherited, so no ambient variable can influence the
     * child. HOME points inside the fixture so nothing the child does can
     * reach the developer's account.
     *
     * `XDG_RUNTIME_DIR` is a private, empty subdirectory of `data_dir` --
     * matching `tests/test_review_apply.c`'s own `run_installed`, which needs
     * this so its non-interactive invocations of the *installed* binary can
     * never reach the socket the real `atlasd` happens to be serving. Harmless
     * here too (an explicit `--data-dir` already keeps every command local),
     * and one spawner leaving it unset while the other set it is exactly the
     * kind of unexplained difference that gets the wrong one copied later. */
    char home[1024];
    (void)snprintf(home, sizeof(home), "HOME=%s", data_dir);
    char xdgrt_path[1024];
    (void)snprintf(xdgrt_path, sizeof(xdgrt_path), "%s/xdgrt", data_dir);
    (void)mkdir(xdgrt_path, S_IRWXU); /* EEXIST is fine; anything else just leaves it unset */
    char xdgrt_env[1200];
    (void)snprintf(xdgrt_env, sizeof(xdgrt_env), "XDG_RUNTIME_DIR=%s", xdgrt_path);
    const char *envp[] = {"PATH=/usr/bin:/bin", home, xdgrt_env, "LC_ALL=C", "TZ=UTC", NULL};

    pid_t pid = fork();
    if (pid < 0) {
        (void)close(master);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "fork: %s", strerror(errno));
    }
    if (pid == 0) {
        /* Child. Only async-signal-safe work from here. */
        (void)close(master);
        if (setsid() < 0) {
            _exit(120);
        }
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) {
            _exit(121);
        }
#ifdef TIOCSCTTY
        if (ioctl(slave, TIOCSCTTY, 0) < 0) {
            _exit(122);
        }
#endif
        if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            _exit(123);
        }
        if (slave > STDERR_FILENO) {
            (void)close(slave);
        }
        execve(bin_path, (char *const *)(uintptr_t)argv, (char *const *)(uintptr_t)envp);
        _exit(124);
    }
    out->master = master;
    out->child = pid;
    return ATLAS_OK;
}

bool pty_expect(pty *p, const char *needle, atlas_buf *transcript) {
    atlas_err err;
    atlas_err_init(&err);
    /* Bounded by an absolute deadline rather than by a read count: a hung child
     * must fail the test rather than block CTest until its own timeout. */
    for (int waited = 0; waited < 200; waited++) {
        if (transcript->len > 0 && strstr(atlas_buf_cstr(transcript), needle) != NULL) {
            return true;
        }
        struct pollfd pfd = {p->master, POLLIN, 0};
        int rc = poll(&pfd, 1u, 50);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            continue;
        }
        char buf[1024];
        ssize_t n = read(p->master, buf, sizeof(buf));
        if (n <= 0) {
            /* EIO on the master is how a pty reports that the last slave was
             * closed, which is the child exiting. */
            break;
        }
        if (atlas_buf_append(transcript, buf, (size_t)n, &err) != ATLAS_OK) {
            return false;
        }
    }
    return transcript->len > 0 && strstr(atlas_buf_cstr(transcript), needle) != NULL;
}

void pty_type(pty *p, const char *line) {
    (void)write(p->master, line, strlen(line));
    (void)write(p->master, "\n", 1u);
}

int pty_wait(pty *p, atlas_buf *transcript) {
    atlas_err err;
    atlas_err_init(&err);
    /* Drain whatever is left, then reap. */
    for (int i = 0; i < 100; i++) {
        struct pollfd pfd = {p->master, POLLIN, 0};
        if (poll(&pfd, 1u, 50) <= 0) {
            int status = 0;
            pid_t r = waitpid(p->child, &status, WNOHANG);
            if (r == p->child) {
                (void)close(p->master);
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            continue;
        }
        char buf[1024];
        ssize_t n = read(p->master, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        (void)atlas_buf_append(transcript, buf, (size_t)n, &err);
    }
    /* Bounded, not `waitpid(p->child, &status, 0)`: a walker that wrongly
     * prompts (or hangs for any other reason) leaves the child blocked
     * reading `/dev/tty`, and an unbounded wait here would hang this whole
     * test process to CTest's own timeout instead of failing with a message
     * that says what happened. Up to 10 more seconds of polling, then
     * SIGKILL and reap. */
    for (int i = 0; i < 200; i++) {
        int status = 0;
        pid_t r = waitpid(p->child, &status, WNOHANG);
        if (r == p->child) {
            (void)close(p->master);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        struct timespec ts = {0, 50L * 1000L * 1000L};
        (void)nanosleep(&ts, NULL);
    }
    (void)kill(p->child, SIGKILL);
    int status = 0;
    (void)waitpid(p->child, &status, 0);
    (void)close(p->master);
    return PTY_WAIT_TIMED_OUT;
}

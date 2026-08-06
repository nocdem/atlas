/* Atlas - Unix-domain socket setup, peer verification and connection.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#define _GNU_SOURCE 1

#include "atlas/ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define ATLAS_SOCKET_BASENAME "atlas.sock"
#define ATLAS_RUNTIME_SUBDIR "atlas"

/* --- paths --------------------------------------------------------------- */

atlas_status atlas_ipc_runtime_dir(atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg == NULL || xdg[0] == '\0') {
        /* No /tmp fallback. $XDG_RUNTIME_DIR is a per-user directory the system
         * guarantees is 0700 and cleaned up at logout; /tmp is neither, and an
         * endpoint that can mutate the index does not belong in a directory
         * every local user can write to. Say what to do instead of degrading. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "XDG_RUNTIME_DIR is not set, so there is no private per-user runtime "
                             "directory for the Atlas socket. Atlas does not fall back to /tmp. "
                             "On a systemd machine this is normally /run/user/%lld: log in through "
                             "a session that creates it, or export XDG_RUNTIME_DIR to a directory "
                             "you own with mode 0700.",
                             (long long)getuid());
    }
    if (xdg[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "XDG_RUNTIME_DIR must be an absolute path, got \"%s\"", xdg);
    }
    size_t n = strlen(xdg);
    while (n > 1u && xdg[n - 1u] == '/') {
        n--;
    }
    atlas_status st = atlas_buf_append(out, xdg, n, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, "/" ATLAS_RUNTIME_SUBDIR, err);
    }
    return st;
}

atlas_status atlas_ipc_socket_path(atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_ipc_runtime_dir(out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_buf_append_str(out, "/" ATLAS_SOCKET_BASENAME, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* sun_path is a fixed 108-byte field. A path that would not fit must fail
     * here with an explanation rather than be silently truncated into a
     * different, possibly attacker-chosen, path. */
    struct sockaddr_un probe;
    if (out->len + 1u > sizeof(probe.sun_path)) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the Atlas socket path is %zu bytes, above the %zu byte limit a "
                             "Unix-domain socket address allows. Set XDG_RUNTIME_DIR to a shorter "
                             "path.",
                             out->len, sizeof(probe.sun_path) - 1u);
    }
    return ATLAS_OK;
}

/* --- runtime directory --------------------------------------------------- */

atlas_status atlas_ipc_ensure_runtime_dir(const char *dir, atlas_err *err) {
    if (mkdir(dir, S_IRWXU) != 0 && errno != EEXIST) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                   "cannot create the Atlas runtime directory %s", dir);
    }
    /* lstat, not stat: a symlink here would let anything that can write the
     * parent redirect the socket somewhere else entirely. */
    struct stat sb;
    if (lstat(dir, &sb) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot stat %s", dir);
    }
    if (S_ISLNK(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is a symbolic link; refusing to place the Atlas socket there",
                             dir);
    }
    if (!S_ISDIR(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s exists and is not a directory", dir);
    }
    if (sb.st_uid != getuid()) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is owned by uid %lld, not by uid %lld; refusing to use it", dir,
                             (long long)sb.st_uid, (long long)getuid());
    }
    if ((sb.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        /* Tighten rather than refuse: this is our own subdirectory, and the
         * likely cause is an unusual umask rather than an attack. It is then
         * re-checked, so a directory that cannot be tightened is still refused. */
        if (chmod(dir, S_IRWXU) != 0) {
            return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                       "%s is accessible to other users and cannot be tightened",
                                       dir);
        }
        if (lstat(dir, &sb) != 0 || (sb.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "%s remains accessible to other users; refusing to use it", dir);
        }
    }
    return ATLAS_OK;
}

/* --- listening ----------------------------------------------------------- */

static void fill_addr(struct sockaddr_un *addr, const char *path) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    /* Length was validated in atlas_ipc_socket_path; copy with the NUL. */
    (void)snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);
}

/* Decides whether an existing path may be removed to make room for our socket.
 *
 * The rule is narrow on purpose. Only a socket, only one we own, and only one
 * that nothing answers on. Anything else — a symlink, a regular file, a
 * directory, a live daemon's socket — is refused, because "clean up whatever is
 * in the way" is how an unrelated file gets deleted by a service start. */
static atlas_status clear_stale_socket(const char *path, atlas_err *err) {
    struct stat sb;
    if (lstat(path, &sb) != 0) {
        if (errno == ENOENT) {
            return ATLAS_OK;
        }
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot inspect %s", path);
    }
    if (S_ISLNK(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is a symbolic link; refusing to remove or bind over it", path);
    }
    if (!S_ISSOCK(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s exists and is not a socket; refusing to remove it. Move it out of "
                             "the way if the Atlas socket belongs there.",
                             path);
    }
    if (sb.st_uid != getuid()) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is a socket owned by uid %lld, not by uid %lld; refusing to "
                             "remove it",
                             path, (long long)sb.st_uid, (long long)getuid());
    }

    /* Liveness is decided by trying to connect, not by a pid file: a daemon that
     * is answering must never have its socket unlinked out from under it. */
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (probe >= 0) {
        struct sockaddr_un addr;
        fill_addr(&addr, path);
        int rc = connect(probe, (const struct sockaddr *)&addr, sizeof(addr));
        bool live = (rc == 0) || (rc < 0 && errno == EINPROGRESS);
        (void)close(probe);
        if (live) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "another Atlas daemon is already listening on %s", path);
        }
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                   "cannot remove the stale Atlas socket %s", path);
    }
    return ATLAS_OK;
}

atlas_status atlas_ipc_listen(const char *socket_path, int *fd_out, atlas_err *err) {
    *fd_out = -1;
    atlas_status st = clear_stale_socket(socket_path, err);
    if (st != ATLAS_OK) {
        return st;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot create a Unix socket");
    }

    /* bind() applies the umask to the socket's mode, so the mode is set
     * explicitly afterwards and then verified. A umask that would have left the
     * socket group-readable is a configuration accident, not a reason to serve
     * requests from a socket other users can open. */
    mode_t prev = umask(0077);
    struct sockaddr_un addr;
    fill_addr(&addr, socket_path);
    int rc = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    int saved = errno;
    (void)umask(prev);
    if (rc != 0) {
        (void)close(fd);
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, saved, "cannot bind %s", socket_path);
    }

    if (chmod(socket_path, S_IRUSR | S_IWUSR) != 0) {
        atlas_status s = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                             "cannot restrict permissions on %s", socket_path);
        (void)close(fd);
        (void)unlink(socket_path);
        return s;
    }
    struct stat sb;
    if (lstat(socket_path, &sb) != 0 || (sb.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        (void)close(fd);
        (void)unlink(socket_path);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is accessible to other users after binding; refusing to serve",
                             socket_path);
    }

    if (listen(fd, (int)ATLAS_IPC_MAX_CLIENTS) != 0) {
        atlas_status s =
            atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot listen on %s", socket_path);
        (void)close(fd);
        (void)unlink(socket_path);
        return s;
    }
    *fd_out = fd;
    return ATLAS_OK;
}

/* --- accepting ----------------------------------------------------------- */

atlas_status atlas_ipc_accept(int listen_fd, int *fd_out, int64_t *peer_pid_out, atlas_err *err) {
    *fd_out = -1;
    if (peer_pid_out != NULL) {
        *peer_pid_out = 0;
    }
    int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return ATLAS_OK; /* nothing pending */
        }
        if (errno == ECONNABORTED) {
            return ATLAS_OK; /* the peer went away between connect and accept */
        }
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "accept failed");
    }

    /* Filesystem permissions already make this socket unreachable by other
     * users. SO_PEERCRED is checked anyway, because the two controls fail in
     * different ways: a permission mistake, a bind-mount, or a path handed over
     * by a more privileged process would all defeat the first one alone. The
     * kernel fills this in at connect time, so it cannot be spoofed by the peer. */
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || len != sizeof(cred)) {
        atlas_status s = atlas_err_set_errno(err, ATLAS_ERR_INTEGRITY, errno,
                                             "cannot read peer credentials; refusing the client");
        (void)close(fd);
        return s;
    }
    if (cred.uid != getuid()) {
        (void)close(fd);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refused a client running as uid %lld; the Atlas daemon serves only "
                             "uid %lld",
                             (long long)cred.uid, (long long)getuid());
    }
    if (peer_pid_out != NULL) {
        *peer_pid_out = (int64_t)cred.pid;
    }
    *fd_out = fd;
    return ATLAS_OK;
}

/* --- connecting ---------------------------------------------------------- */

atlas_status atlas_ipc_connect(const char *socket_path, int timeout_ms, int *fd_out,
                               atlas_err *err) {
    *fd_out = -1;
    (void)timeout_ms;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot create a Unix socket");
    }
    struct sockaddr_un addr;
    fill_addr(&addr, socket_path);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int saved = errno;
        (void)close(fd);
        if (saved == ENOENT || saved == ECONNREFUSED) {
            /* Not a failure: this is exactly how a CLI invocation learns that it
             * should take the offline path. */
            return atlas_err_set(err, ATLAS_ERR_CONFIG, "no Atlas daemon is listening on %s",
                                 socket_path);
        }
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, saved, "cannot connect to %s",
                                   socket_path);
    }
    *fd_out = fd;
    return ATLAS_OK;
}

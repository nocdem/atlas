/* Atlas - Unix-domain socket setup, peer verification and connection.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#define _GNU_SOURCE 1

#include "atlas/ipc.h"

#include "atlas/datadir.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
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

/* The per-user runtime directory systemd creates, used only when
 * $XDG_RUNTIME_DIR is absent — and only on proof.
 *
 * This is not a relaxation of the "no /tmp fallback" rule; it is the same rule
 * applied to the one directory that rule already names. `/run/user/<uid>` is
 * exactly what a login session would have put in $XDG_RUNTIME_DIR, and the
 * variable's absence is an environment accident rather than evidence that the
 * directory is unsafe: a non-login SSH invocation, a cron-style launch and a
 * hook spawned by an editor all reach a machine where it exists and the
 * variable does not.
 *
 * Every property the rule actually relies on is therefore *checked* rather than
 * assumed: it must exist, not be a symbolic link, be a directory, be owned by
 * this uid, and grant nothing to group or other. A directory failing any of
 * those is not used, and the caller gets the same refusal as before.
 *
 * `lstat`, not `stat`: following a link here would let whoever could create one
 * choose the directory, which is the whole failure mode being avoided. */
static bool systemd_runtime_dir(char *out, size_t out_size) {
    uid_t uid = getuid();
    int n = snprintf(out, out_size, "/run/user/%lld", (long long)uid);
    if (n < 0 || (size_t)n >= out_size) {
        return false;
    }
    struct stat sb;
    if (lstat(out, &sb) != 0) {
        return false;
    }
    if (!S_ISDIR(sb.st_mode) || S_ISLNK(sb.st_mode)) {
        return false;
    }
    if (sb.st_uid != uid) {
        return false;
    }
    if ((sb.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return false;
    }
    return true;
}

/* The index this process is addressing, set once by the CLI. See
 * `atlas_ipc_socket_scope_set`. Empty means "whatever the policy says", which
 * is the daemon's case and every adapter's case. */
static char g_socket_scope[512];

void atlas_ipc_socket_scope_set(const char *data_dir) {
    if (data_dir == NULL) {
        g_socket_scope[0] = '\0';
        return;
    }
    size_t n = strlen(data_dir);
    while (n > 1u && data_dir[n - 1u] == '/') {
        n--;
    }
    if (n + 1u > sizeof(g_socket_scope)) {
        /* Too long to record is treated as "not the system index", which is the
         * conservative reading: it sends this process to a per-user socket
         * rather than to the shared one. */
        (void)snprintf(g_socket_scope, sizeof(g_socket_scope), "%s", "/");
        return;
    }
    memcpy(g_socket_scope, data_dir, n);
    g_socket_scope[n] = '\0';
}

/* True when this process is addressing the index the policy describes.
 *
 * With no scope recorded — an adapter, or a command with no `--data-dir` — the
 * question is asked of the resolver instead, so that `ATLAS_DATA_DIR` moves the
 * socket along with the index it selects. A process reading one index over the
 * socket of another is the confusion this exists to prevent, and it does not
 * matter which of the two ways of naming an index produced the mismatch. */
static bool scope_is_system(const atlas_syspolicy *p) {
    if (p->state != ATLAS_SYSPOLICY_SYSTEM) {
        return false;
    }
    if (g_socket_scope[0] != '\0') {
        return strcmp(g_socket_scope, p->data_dir) == 0;
    }
    atlas_buf resolved = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    bool same = false;
    if (atlas_datadir_resolve(NULL, &resolved, NULL, &err) == ATLAS_OK) {
        same = strcmp(atlas_buf_cstr(&resolved), p->data_dir) == 0;
    }
    atlas_buf_free(&resolved);
    return same;
}

/* The directory part of the system socket path.
 *
 * Derived from the policy rather than configured separately, so the two can
 * never disagree about where the socket lives. */
static bool system_runtime_dir(const atlas_syspolicy *p, char *out, size_t out_size) {
    if (p == NULL || p->state != ATLAS_SYSPOLICY_SYSTEM) {
        return false;
    }
    const char *slash = strrchr(p->socket_path, '/');
    if (slash == NULL || slash == p->socket_path) {
        return false;
    }
    size_t n = (size_t)(slash - p->socket_path);
    if (n + 1u > out_size) {
        return false;
    }
    memcpy(out, p->socket_path, n);
    out[n] = '\0';
    return true;
}

atlas_status atlas_ipc_runtime_dir(atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    /* A7.1: a root-anchored system policy decides this, and `$XDG_RUNTIME_DIR`
     * is not consulted at all when one is active. Both halves matter — a shared
     * daemon has to be reachable from an SSH session that has no runtime
     * directory, and a client must not be able to pick a different endpoint by
     * exporting a variable. */
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    char sysdir[sizeof(sp.socket_path)];
    if (scope_is_system(&sp) && system_runtime_dir(&sp, sysdir, sizeof(sysdir))) {
        return atlas_buf_append_str(out, sysdir, err);
    }

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char discovered[64];
    if ((xdg == NULL || xdg[0] == '\0') && systemd_runtime_dir(discovered, sizeof discovered)) {
        xdg = discovered;
    }
    if (xdg == NULL || xdg[0] == '\0') {
        /* Still no /tmp fallback. $XDG_RUNTIME_DIR is a per-user directory the
         * system guarantees is 0700 and cleaned up at logout; /tmp is neither,
         * and an endpoint that can mutate the index does not belong in a
         * directory every local user can write to. Say what to do instead of
         * degrading. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "XDG_RUNTIME_DIR is not set and /run/user/%lld is not a private "
                             "directory owned by this user, so there is no runtime directory for "
                             "the Atlas socket. Atlas does not fall back to /tmp. Log in through "
                             "a session that creates one, or export XDG_RUNTIME_DIR to a directory "
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
    atlas_buf_reset(out);
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    atlas_status st;
    if (scope_is_system(&sp)) {
        /* The whole path, from the policy, rather than a directory plus a
         * basename Atlas chose: the operator wrote down an endpoint and that is
         * the endpoint. */
        st = atlas_buf_append_str(out, sp.socket_path, err);
        if (st != ATLAS_OK) {
            return st;
        }
    } else {
        st = atlas_ipc_runtime_dir(out, err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = atlas_buf_append_str(out, "/" ATLAS_SOCKET_BASENAME, err);
        if (st != ATLAS_OK) {
            return st;
        }
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

/* Resolves the policy's client group to a gid.
 *
 * By name, once, at the point it is needed. A gid in the policy file would be
 * one more machine-specific number for an operator to get wrong, and the name
 * is what `groupadd` gave them. */
static bool client_gid(const atlas_syspolicy *p, gid_t *out) {
    if (p == NULL || p->state != ATLAS_SYSPOLICY_SYSTEM || p->client_group[0] == '\0') {
        return false;
    }
    struct group *g = getgrnam(p->client_group);
    if (g == NULL) {
        return false;
    }
    *out = g->gr_gid;
    return true;
}

/* System mode: the runtime directory is 0750, owned by this uid, group the
 * client group — traversable by members and by nobody else. Set explicitly and
 * then verified, because a directory that merely *should* have been tightened
 * is one whose mode nobody checked. */
static atlas_status system_runtime_dir_shape(const char *dir, const atlas_syspolicy *policy,
                                             atlas_err *err) {
    gid_t gid = 0;
    if (!client_gid(policy, &gid)) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the system policy names client group \"%s\", which does not exist "
                             "on this machine; refusing to serve",
                             policy->client_group);
    }
    /* `chown` only when the group is not already right.
     *
     * The unit gives the daemon `atlas-clients` as its *primary* group, so
     * systemd's RuntimeDirectory and the socket are created with that group
     * already and this call is normally unnecessary. Skipping it matters:
     * `chown` is in systemd's `@privileged` syscall set, which the sandbox
     * filters, so calling it unconditionally killed the daemon with SIGSYS
     * before it ever bound a socket. Attempting it only when it would change
     * something keeps the fallback for a hand-run daemon whose primary group is
     * different, without requiring the sandbox to allow a privileged syscall in
     * the normal path.
     *
     * The verification below is unconditional either way, so a group that is
     * wrong and cannot be corrected still refuses to serve. */
    struct stat pre;
    if (lstat(dir, &pre) == 0 && pre.st_gid != gid) {
        if (chown(dir, (uid_t)-1, gid) != 0) {
            return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                       "cannot give %s to group %s", dir, policy->client_group);
        }
    }
    if (chmod(dir, S_IRWXU | S_IRGRP | S_IXGRP) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot set the mode of %s", dir);
    }
    struct stat sb;
    if (lstat(dir, &sb) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot stat %s", dir);
    }
    if (sb.st_uid != getuid() || sb.st_gid != gid ||
        (sb.st_mode & 07777u) != (S_IRWXU | S_IRGRP | S_IXGRP)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is not owned by uid %lld group %lld with mode 0750 after being "
                             "set; refusing to serve",
                             dir, (long long)getuid(), (long long)gid);
    }
    return ATLAS_OK;
}

atlas_status atlas_ipc_ensure_runtime_dir(const char *dir, const atlas_syspolicy *policy,
                                          atlas_err *err) {
    if (policy != NULL && policy->state == ATLAS_SYSPOLICY_SYSTEM) {
        if (mkdir(dir, S_IRWXU) != 0 && errno != EEXIST) {
            return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                       "cannot create the Atlas runtime directory %s", dir);
        }
        /* lstat first, for the reason the per-user path gives: a symlink here
         * would let whatever can write the parent redirect the socket. */
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
                                 "%s is owned by uid %lld, not by uid %lld; refusing to use it",
                                 dir, (long long)sb.st_uid, (long long)getuid());
        }
        return system_runtime_dir_shape(dir, policy, err);
    }
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

/* System mode: the socket is 0660, owned by this uid, group the client group.
 *
 * Established explicitly and then read back, and a failure at any step unlinks
 * the socket before returning. A daemon that started with a socket more open
 * than intended would be serving strangers while reporting success, which is
 * strictly worse than not starting. */
static atlas_status system_socket_shape(const char *path, const atlas_syspolicy *policy,
                                        atlas_err *err) {
    gid_t gid = 0;
    if (!client_gid(policy, &gid)) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the system policy names client group \"%s\", which does not exist "
                             "on this machine; refusing to serve",
                             policy->client_group);
    }
    /* Only when it would change something — see the runtime-directory helper
     * above for why an unconditional `chown` is fatal under the sandbox. */
    struct stat pre;
    if (lstat(path, &pre) == 0 && pre.st_gid != gid) {
        if (chown(path, (uid_t)-1, gid) != 0) {
            return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot give %s to group %s",
                                       path, policy->client_group);
        }
    }
    if (chmod(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                   "cannot set the mode of %s", path);
    }
    struct stat sb;
    if (lstat(path, &sb) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot stat %s", path);
    }
    if (sb.st_uid != getuid() || sb.st_gid != gid ||
        (sb.st_mode & 07777u) != (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is not owned by uid %lld group %lld with mode 0660 after being "
                             "set; refusing to serve",
                             path, (long long)getuid(), (long long)gid);
    }
    return ATLAS_OK;
}

atlas_status atlas_ipc_listen(const char *socket_path, const atlas_syspolicy *policy, int *fd_out,
                              atlas_err *err) {
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

    if (policy != NULL && policy->state == ATLAS_SYSPOLICY_SYSTEM) {
        atlas_status s = system_socket_shape(socket_path, policy, err);
        if (s != ATLAS_OK) {
            (void)close(fd);
            (void)unlink(socket_path);
            return s;
        }
    } else {
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

atlas_status atlas_ipc_accept(int listen_fd, const atlas_syspolicy *policy, int *fd_out,
                              int64_t *peer_pid_out, int64_t *peer_uid_out, atlas_err *err) {
    *fd_out = -1;
    if (peer_pid_out != NULL) {
        *peer_pid_out = 0;
    }
    /* Zero is not a uid any A8 method group is selected on, so a caller that
     * forgets to look at the return value gets the refusing default. */
    if (peer_uid_out != NULL) {
        *peer_uid_out = 0;
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
    /* The daemon's own uid always, and in system mode the uids a root-owned
     * policy lists — nothing else, and decided here, before a single byte of
     * the request has been read.
     *
     * `cred.uid` is the kernel's answer about the peer, recorded at connect
     * time. There is deliberately no path by which a uid, gid, pid or role from
     * the request body, the peer's environment or `/proc` reaches this
     * comparison: a client describing itself is not evidence about itself. */
    bool permitted = (cred.uid == getuid()) || atlas_syspolicy_permits(policy, (long long)cred.uid);
    if (!permitted) {
        (void)close(fd);
        if (policy != NULL && policy->state == ATLAS_SYSPOLICY_SYSTEM) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "refused a client running as uid %lld; this Atlas daemon serves "
                                 "uid %lld and the uids listed in %s",
                                 (long long)cred.uid, (long long)getuid(), ATLAS_SYSPOLICY_PATH);
        }
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refused a client running as uid %lld; the Atlas daemon serves only "
                             "uid %lld",
                             (long long)cred.uid, (long long)getuid());
    }
    if (peer_pid_out != NULL) {
        *peer_pid_out = (int64_t)cred.pid;
    }
    if (peer_uid_out != NULL) {
        *peer_uid_out = (int64_t)cred.uid;
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

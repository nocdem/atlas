/* Atlas - the data-directory writer lock.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#define _GNU_SOURCE 1

#include "atlas/lock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"

struct atlas_lock {
    int fd;
    atlas_buf path;
};

atlas_status atlas_lock_path(const char *data_dir, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    if (data_dir == NULL || data_dir[0] != '/') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "data directory must be an absolute path");
    }
    atlas_status st = atlas_buf_append_str(out, data_dir, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '/', err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, ATLAS_LOCK_FILENAME, err);
    }
    return st;
}

static const char *role_name(atlas_lock_role role) {
    return role == ATLAS_LOCK_ROLE_DAEMON ? "daemon" : "one-shot";
}

/* Opens the lock file without following a symlink into it.
 *
 * O_NOFOLLOW only refuses a symlink as the final component, which is the case
 * that matters here: the data directory itself is created by Atlas with mode
 * 0700, so an attacker who can plant intermediate symlinks inside it already has
 * write access to the index and the lock is not the interesting target. */
static atlas_status open_lock_file(const char *path, int flags, int *fd_out, atlas_err *err) {
    *fd_out = -1;
    int fd = open(path, flags | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (errno == ELOOP) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "%s is a symbolic link; refusing to use it as a lock file", path);
        }
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot open lock file %s", path);
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        atlas_status st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot stat %s", path);
        (void)close(fd);
        return st;
    }
    if (!S_ISREG(sb.st_mode)) {
        (void)close(fd);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "%s is not a regular file; refusing to use it as a lock file", path);
    }
    *fd_out = fd;
    return ATLAS_OK;
}

/* Reads the holder's self-description. Purely diagnostic: the kernel decides who
 * holds the lock, and this text may be stale, empty or from a crashed process. */
static void read_holder(int fd, atlas_buf *out) {
    char buf[256];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1u, 0);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    /* One line only, and never any control byte: this text reaches a terminal. */
    for (ssize_t i = 0; i < n; i++) {
        if ((unsigned char)buf[i] < 0x20u || (unsigned char)buf[i] == 0x7fu) {
            buf[i] = '\0';
            break;
        }
    }
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_buf_set_str(out, buf, &ignore);
}

atlas_status atlas_lock_acquire(const char *data_dir, atlas_lock_role role, atlas_lock **out,
                                atlas_err *err) {
    *out = NULL;
    atlas_lock *lk = calloc(1u, sizeof(*lk));
    if (lk == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory acquiring the writer lock");
    }
    lk->fd = -1;
    atlas_buf_init(&lk->path);

    atlas_status st = atlas_lock_path(data_dir, &lk->path, err);
    if (st != ATLAS_OK) {
        atlas_lock_release(lk);
        return st;
    }
    st = open_lock_file(atlas_buf_cstr(&lk->path), O_RDWR | O_CREAT, &lk->fd, err);
    if (st != ATLAS_OK) {
        atlas_lock_release(lk);
        return st;
    }

    if (flock(lk->fd, LOCK_EX | LOCK_NB) != 0) {
        int saved = errno;
        atlas_buf holder = ATLAS_BUF_INIT;
        read_holder(lk->fd, &holder);
        if (saved == EWOULDBLOCK) {
            st = atlas_err_set(
                err, ATLAS_ERR_INTEGRITY,
                "another Atlas writer already owns %s%s%s. Exactly one process may write the "
                "index: stop the daemon (systemctl --user stop atlas) or wait for the other "
                "command to finish.",
                atlas_buf_cstr(&lk->path), holder.len > 0 ? " — held by " : "",
                holder.len > 0 ? atlas_buf_cstr(&holder) : "");
        } else {
            st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, saved, "cannot lock %s",
                                     atlas_buf_cstr(&lk->path));
        }
        atlas_buf_free(&holder);
        atlas_lock_release(lk);
        return st;
    }

    /* Record who we are, for the next process's diagnostics. Truncate first so a
     * longer previous line cannot leave a tail behind. Failures here are ignored
     * on purpose: the lock is already held, and losing the annotation must not
     * turn a successful acquire into an error. */
    char line[160];
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    int n = snprintf(line, sizeof(line), "%s pid %lld since %s\n", role_name(role),
                     (long long)getpid(), now);
    if (n > 0) {
        (void)ftruncate(lk->fd, 0);
        ssize_t w = pwrite(lk->fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1u, 0);
        (void)w;
    }

    *out = lk;
    return ATLAS_OK;
}

void atlas_lock_release(atlas_lock *lk) {
    if (lk == NULL) {
        return;
    }
    if (lk->fd >= 0) {
        /* Closing releases the flock. Doing it explicitly first keeps the
         * ordering obvious and makes the release point greppable. */
        (void)flock(lk->fd, LOCK_UN);
        (void)close(lk->fd);
    }
    atlas_buf_free(&lk->path);
    free(lk);
}

atlas_status atlas_lock_probe(const char *data_dir, bool *held_out, atlas_buf *holder_out,
                              atlas_err *err) {
    *held_out = false;
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_lock_path(data_dir, &path, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&path);
        return st;
    }
    int fd = -1;
    /* No O_CREAT: probing must not create the file, so that "no lock file" and
     * "lock file exists but is free" stay distinguishable. */
    fd = open(atlas_buf_cstr(&path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        atlas_buf_free(&path);
        if (errno == ENOENT) {
            return ATLAS_OK; /* never locked */
        }
        if (errno == ELOOP) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the Atlas lock file is a symbolic link; refusing to trust it");
        }
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot probe the Atlas lock");
    }
    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        (void)flock(fd, LOCK_UN);
    } else if (errno == EWOULDBLOCK) {
        *held_out = true;
        if (holder_out != NULL) {
            read_holder(fd, holder_out);
        }
    } else {
        atlas_status s = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                             "cannot probe the Atlas lock");
        (void)close(fd);
        atlas_buf_free(&path);
        return s;
    }
    (void)close(fd);
    atlas_buf_free(&path);
    return ATLAS_OK;
}

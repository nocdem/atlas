/* Atlas - `atlas backup create|verify|restore`.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The filesystem half. Nothing here includes sqlite3.h; the copy itself and
 * every record check live in src/db/db_backup.c.
 *
 * Three properties are what this file exists to hold:
 *
 *   nothing partial is ever published   every write goes to a temporary file in
 *                                       the destination directory, is verified
 *                                       in full, is fsynced, and only then is
 *                                       renamed into place. A crash or a
 *                                       failure leaves the temporary file and
 *                                       nothing else.
 *
 *   no symlink is ever traversed        every path is resolved component by
 *                                       component with O_NOFOLLOW from the
 *                                       root. A symlinked component refuses the
 *                                       operation; it is never followed "just
 *                                       to see where it goes".
 *
 *   a failed restore changes nothing    the original database is byte-identical
 *                                       through every failure. See restore()
 *                                       for the one instruction that is not
 *                                       reversible and what stands behind it.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/datadir.h"
#include "atlas/lock.h"

/* Copy chunk. Large enough that a 100 MiB database is a few thousand reads,
 * small enough that the peak resident set is a property of this constant
 * rather than of the database. */
#define BACKUP_IO_CHUNK (256u * 1024u)

const char *atlas_backup_verdict_name(atlas_backup_verdict v) {
    switch (v) {
    case ATLAS_BACKUP_OK:
        return "ok";
    case ATLAS_BACKUP_UNREADABLE:
        return "unreadable";
    case ATLAS_BACKUP_NOT_SQLITE:
        return "not_sqlite";
    case ATLAS_BACKUP_NOT_ATLAS:
        return "not_atlas";
    case ATLAS_BACKUP_SCHEMA_FUTURE:
        return "schema_future";
    case ATLAS_BACKUP_CORRUPT:
        return "corrupt";
    case ATLAS_BACKUP_INCONSISTENT:
        return "inconsistent";
    }
    return "unknown";
}

void atlas_backup_report_init(atlas_backup_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->path);
    atlas_buf_init(&r->source_db_path);
    snprintf(r->atlas_version, sizeof r->atlas_version, "%s", ATLAS_VERSION_STRING);
}

void atlas_backup_report_free(atlas_backup_report *r) {
    atlas_buf_free(&r->path);
    atlas_buf_free(&r->source_db_path);
}

void atlas_backup_verify_report_init(atlas_backup_verify_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->path);
    atlas_buf_init(&r->integrity);
    atlas_buf_init(&r->foreign_key_check);
    atlas_buf_init(&r->missing_tables);
    atlas_buf_init(&r->problems);
    r->schema_version = -1;
    r->expected_schema_version = ATLAS_SCHEMA_VERSION;
    r->verdict = ATLAS_BACKUP_OK;
}

void atlas_backup_verify_report_free(atlas_backup_verify_report *r) {
    atlas_buf_free(&r->path);
    atlas_buf_free(&r->integrity);
    atlas_buf_free(&r->foreign_key_check);
    atlas_buf_free(&r->missing_tables);
    atlas_buf_free(&r->problems);
}

void atlas_backup_restore_report_init(atlas_backup_restore_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->data_dir);
    atlas_buf_init(&r->db_path);
    atlas_buf_init(&r->recovery_path);
    atlas_backup_verify_report_init(&r->source);
    atlas_backup_verify_report_init(&r->installed);
    r->schema_before = -1;
    r->schema_after = -1;
}

void atlas_backup_restore_report_free(atlas_backup_restore_report *r) {
    atlas_buf_free(&r->data_dir);
    atlas_buf_free(&r->db_path);
    atlas_buf_free(&r->recovery_path);
    atlas_backup_verify_report_free(&r->source);
    atlas_backup_verify_report_free(&r->installed);
}

/* --- fault injection -----------------------------------------------------
 *
 * `ATLAS_BACKUP_FAULT` names one point at which this file pretends the
 * operating system failed. It exists because the guarantees above are entirely
 * about failure paths, and a guarantee whose failure path is never executed is
 * a comment.
 *
 * It is compiled into every build on purpose. An #ifdef would mean the binary
 * that ships is not the binary the failure tests ran against, which is the one
 * thing worth avoiding here. It can only ever cause an operation to *abort*:
 * there is no fault point that skips a check, weakens a guarantee or publishes
 * something. The worst an operator who sets it can do is fail to take a
 * backup, loudly. */
static bool fault(const char *point) {
    const char *want = getenv("ATLAS_BACKUP_FAULT");
    return want != NULL && strcmp(want, point) == 0;
}

/* --- path resolution -----------------------------------------------------
 *
 * A resolved destination: an fd on the containing directory, opened without
 * ever traversing a symlink, plus the final component's name and the whole
 * lexically normalised path. Every metadata check, every create, every fsync
 * and the final rename go through the fd; the string is only for SQLite, which
 * takes a path, and for reporting.
 */
typedef struct dest_path {
    int dirfd;         /* the containing directory */
    atlas_buf dir;     /* its absolute path */
    atlas_buf base;    /* the final component */
    atlas_buf full;    /* dir + "/" + base */
} dest_path;

static void dest_path_init(dest_path *d) {
    d->dirfd = -1;
    atlas_buf_init(&d->dir);
    atlas_buf_init(&d->base);
    atlas_buf_init(&d->full);
}

static void dest_path_free(dest_path *d) {
    if (d->dirfd >= 0) {
        close(d->dirfd);
        d->dirfd = -1;
    }
    atlas_buf_free(&d->dir);
    atlas_buf_free(&d->base);
    atlas_buf_free(&d->full);
}

/* Make `in` absolute and collapse ".", ".." and repeated separators.
 *
 * Lexical rather than realpath(3), which resolves symlinks: resolving them is
 * precisely what must not happen, because the answer would name a directory the
 * operator did not write down. The collapse is sound here only because the
 * caller then walks every remaining component with O_NOFOLLOW and refuses on
 * the first symlink — so any path this accepts has no symlink in it, and for
 * such a path the lexical and physical resolutions are the same. */
static atlas_status normalise_abs(const char *in, atlas_buf *out, atlas_err *err) {
    if (in == NULL || in[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "empty path");
    }
    if (strchr(in, '\n') != NULL || strchr(in, '\r') != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "path contains a line break");
    }
    atlas_buf work = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (in[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd) == NULL) {
            atlas_buf_free(&work);
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "cannot resolve \"%s\": the current directory is unavailable",
                                 in);
        }
        st = atlas_buf_append_str(&work, cwd, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(&work, '/', err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&work, in, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&work);
        return st;
    }

    /* Rebuild from components, dropping "." and popping on "..". */
    atlas_buf built = ATLAS_BUF_INIT;
    const char *p = atlas_buf_cstr(&work);
    while (*p != '\0' && st == ATLAS_OK) {
        while (*p == '/') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t n = (size_t)(p - start);
        if (n == 0) {
            continue;
        }
        if (n == 1 && start[0] == '.') {
            continue;
        }
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            char *b = built.data;
            size_t len = built.len;
            while (len > 0 && b[len - 1] != '/') {
                len--;
            }
            built.len = len > 0 ? len - 1 : 0;
            if (built.data != NULL) {
                built.data[built.len] = '\0';
            }
            continue;
        }
        st = atlas_buf_append_ch(&built, '/', err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append(&built, start, n, err);
        }
    }
    atlas_buf_free(&work);
    if (st == ATLAS_OK && built.len == 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"%s\" resolves to the filesystem root", in);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(out, built.data, built.len, err);
    }
    atlas_buf_free(&built);
    return st;
}

/* Open every directory component of `abs` from "/" downwards with O_NOFOLLOW,
 * refusing the operation on the first symlink rather than following it. */
static atlas_status resolve_dest(const char *path, dest_path *out, atlas_err *err) {
    atlas_buf abs = ATLAS_BUF_INIT;
    atlas_status st = normalise_abs(path, &abs, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&abs);
        return st;
    }
    st = atlas_buf_set(&out->full, abs.data, abs.len, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&abs);
        return st;
    }

    const char *s = atlas_buf_cstr(&abs);
    const char *slash = strrchr(s, '/');
    st = atlas_buf_set_str(&out->base, slash + 1, err);
    if (st == ATLAS_OK) {
        st = slash == s ? atlas_buf_set_str(&out->dir, "/", err)
                        : atlas_buf_set(&out->dir, s, (size_t)(slash - s), err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&abs);
        return st;
    }

    int fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        atlas_buf_free(&abs);
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot open the filesystem root");
    }
    const char *p = s;
    const char *stop = slash;
    while (p < stop && st == ATLAS_OK) {
        while (p < stop && *p == '/') {
            p++;
        }
        const char *cs = p;
        while (p < stop && *p != '/') {
            p++;
        }
        size_t n = (size_t)(p - cs);
        if (n == 0) {
            continue;
        }
        char comp[NAME_MAX + 1];
        if (n > NAME_MAX) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "path component is too long");
            break;
        }
        memcpy(comp, cs, n);
        comp[n] = '\0';
        int next = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            /* Why it failed decides what to say, and the errno alone does not
             * decide it: O_NOFOLLOW with O_DIRECTORY reports ELOOP for a
             * symlink on some kernels and ENOTDIR on others, and ENOTDIR is
             * also what a plain file gives. So the component is lstat'd and the
             * answer comes from what it actually is. */
            int saved = errno;
            struct stat lb;
            bool is_link = fstatat(fd, comp, &lb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lb.st_mode);
            st = is_link
                     ? atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                     "refusing \"%s\": the component \"%s\" is a symbolic link",
                                     atlas_buf_cstr(&out->full), comp)
                     : atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot open directory \"%s\": %s",
                                     comp, strerror(saved));
            break;
        }
        close(fd);
        fd = next;
    }
    atlas_buf_free(&abs);
    if (st != ATLAS_OK) {
        close(fd);
        return st;
    }
    out->dirfd = fd;
    return ATLAS_OK;
}

/* --- small filesystem helpers ------------------------------------------- */

static atlas_status hash_fd(int fd, char *hex_out, int64_t *size_out, atlas_err *err) {
    unsigned char *buf = malloc(BACKUP_IO_CHUNK);
    if (buf == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory hashing a backup");
    }
    atlas_sha256 sh;
    atlas_sha256_init(&sh);
    int64_t total = 0;
    atlas_status st = ATLAS_OK;
    for (;;) {
        ssize_t n = read(fd, buf, BACKUP_IO_CHUNK);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot read backup: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            break;
        }
        atlas_sha256_update(&sh, buf, (size_t)n);
        total += n;
    }
    free(buf);
    if (st != ATLAS_OK) {
        return st;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&sh, digest);
    atlas_hex_encode(digest, sizeof digest, hex_out);
    if (size_out != NULL) {
        *size_out = total;
    }
    return ATLAS_OK;
}

static atlas_status hash_at(int dirfd, const char *name, char *hex_out, int64_t *size_out,
                            atlas_err *err) {
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot open \"%s\": %s", name,
                             strerror(errno));
    }
    atlas_status st = hash_fd(fd, hex_out, size_out, err);
    close(fd);
    return st;
}

static atlas_status fsync_at(int dirfd, const char *name, atlas_err *err) {
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot reopen \"%s\" to flush it: %s", name,
                             strerror(errno));
    }
    int rc = fault("fsync") ? -1 : fsync(fd);
    close(fd);
    if (rc != 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot flush \"%s\" to disk", name);
    }
    return ATLAS_OK;
}

static atlas_status fsync_dir(int dirfd, atlas_err *err) {
    if (fault("fsync_dir") || fsync(dirfd) != 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "cannot flush the destination directory to disk");
    }
    return ATLAS_OK;
}

/* A temporary name in the destination directory. Dot-prefixed so a directory
 * listing does not advertise it, and stamped with the pid so two concurrent
 * backups into one directory cannot collide. */
static atlas_status temp_name(atlas_buf *out, const char *tag, atlas_err *err) {
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, ".atlas-%s.%ld.tmp", tag, (long)getpid());
}

/* Create `name` in `dirfd` with mode 0600, failing if it already exists.
 *
 * O_EXCL is what makes this safe against a planted file, and the explicit mode
 * is what keeps a permissive umask out of the answer: SQLite would create the
 * file 0644 or worse. */
static atlas_status create_private(int dirfd, const char *name, int *fd_out, atlas_err *err) {
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot create \"%s\": %s", name,
                             strerror(errno));
    }
    /* Belt for the umask: openat's mode is masked, fchmod is not. */
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        (void)unlinkat(dirfd, name, 0);
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot restrict \"%s\" to owner-only", name);
    }
    if (fd_out != NULL) {
        *fd_out = fd;
    } else {
        close(fd);
    }
    return ATLAS_OK;
}

/* Confirm the file SQLite has just written through a path is the same inode
 * this process created through the directory fd. Without this, everything the
 * O_NOFOLLOW walk established could be undone between the create and the write
 * by anybody who can rename inside the destination directory. */
static atlas_status same_inode(int dirfd, const char *name, dev_t dev, ino_t ino, atlas_err *err) {
    struct stat sb;
    if (fstatat(dirfd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the staged file \"%s\" disappeared", name);
    }
    if (!S_ISREG(sb.st_mode) || sb.st_dev != dev || sb.st_ino != ino) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the staged file \"%s\" was replaced while it was being written",
                             name);
    }
    if ((sb.st_mode & 07777) != 0600) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the staged file \"%s\" is not owner-only", name);
    }
    return ATLAS_OK;
}

/* --- create -------------------------------------------------------------- */

atlas_status atlas_service_backup_create(const char *data_dir_override,
                                         const atlas_backup_create_opts *opts,
                                         atlas_backup_report *out, atlas_err *err) {
    if (opts == NULL || opts->output == NULL || opts->output[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas backup create OUTPUT");
    }

    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_buf tmp = ATLAS_BUF_INIT;
    dest_path dest;
    dest_path_init(&dest);
    atlas_db *src = NULL;
    bool staged = false;

    atlas_status st = atlas_datadir_resolve(data_dir_override, &data_dir, NULL, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(atlas_buf_cstr(&data_dir), &db_path, err);
    }
    if (st == ATLAS_OK) {
        struct stat sb;
        if (stat(atlas_buf_cstr(&db_path), &sb) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                               "there is no Atlas index at \"%s\" to back up",
                               atlas_buf_cstr(&db_path));
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->source_db_path, db_path.data, db_path.len, err);
    }

    /* Whether a daemon owns the index right now. This changes nothing about how
     * the snapshot is taken — it is a read-only connection either way — but an
     * operator reading the report should not have to guess which situation
     * produced it. */
    if (st == ATLAS_OK) {
        bool held = false;
        atlas_err probe_err;
        atlas_err_init(&probe_err);
        if (atlas_lock_probe(atlas_buf_cstr(&data_dir), &held, NULL, &probe_err) == ATLAS_OK) {
            out->source_online = held;
        }
    }

    if (st == ATLAS_OK) {
        st = resolve_dest(opts->output, &dest, err);
    }

    /* Destination policy, all of it before anything is created. */
    if (st == ATLAS_OK) {
        struct stat sb;
        if (fstatat(dest.dirfd, atlas_buf_cstr(&dest.base), &sb, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISLNK(sb.st_mode)) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "refusing to write \"%s\": it is a symbolic link",
                                   atlas_buf_cstr(&dest.full));
            } else if (S_ISDIR(sb.st_mode)) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "refusing to write \"%s\": it is a directory",
                                   atlas_buf_cstr(&dest.full));
            } else if (!S_ISREG(sb.st_mode)) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "refusing to write \"%s\": it is not a regular file",
                                   atlas_buf_cstr(&dest.full));
            } else if (!opts->force) {
                st = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "\"%s\" already exists; pass --force to replace it",
                                   atlas_buf_cstr(&dest.full));
            }
        } else if (errno != ENOENT) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot inspect \"%s\": %s",
                               atlas_buf_cstr(&dest.full), strerror(errno));
        }
    }

    if (st == ATLAS_OK) {
        st = temp_name(&tmp, "backup", err);
    }
    /* A leftover from a killed run in the same process slot would fail O_EXCL
     * forever, so it is cleared first. It is ours by construction: the name
     * carries this pid. */
    if (st == ATLAS_OK) {
        (void)unlinkat(dest.dirfd, atlas_buf_cstr(&tmp), 0);
        st = create_private(dest.dirfd, atlas_buf_cstr(&tmp), NULL, err);
    }
    dev_t tdev = 0;
    ino_t tino = 0;
    if (st == ATLAS_OK) {
        staged = true;
        struct stat sb;
        if (fstatat(dest.dirfd, atlas_buf_cstr(&tmp), &sb, AT_SYMLINK_NOFOLLOW) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "the staged backup vanished immediately");
        } else {
            tdev = sb.st_dev;
            tino = sb.st_ino;
        }
    }

    /* The absolute path of the staged file, for SQLite. Safe to use because
     * every directory above it has been opened O_NOFOLLOW and the inode is
     * re-confirmed through the directory fd once the copy is done. */
    atlas_buf tmp_full = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&tmp_full, dest.dir.data, dest.dir.len, err);
        if (st == ATLAS_OK && strcmp(atlas_buf_cstr(&dest.dir), "/") != 0) {
            st = atlas_buf_append_ch(&tmp_full, '/', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&tmp_full, atlas_buf_cstr(&tmp), err);
        }
    }

    /* Read-only, so the writer lock stays with whoever has it and the daemon
     * keeps working while this runs. */
    if (st == ATLAS_OK) {
        st = atlas_db_open_readonly(atlas_buf_cstr(&db_path), &src, err);
    }
    if (st == ATLAS_OK) {
        st = fault("copy") ? atlas_err_set(err, ATLAS_ERR_DB, "injected copy failure")
                           : atlas_db_backup_copy(src, atlas_buf_cstr(&tmp_full), &out->page_count,
                                                  &out->page_size, err);
    }
    if (src != NULL) {
        atlas_db_close(src);
        src = NULL;
    }
    if (st == ATLAS_OK) {
        st = same_inode(dest.dirfd, atlas_buf_cstr(&tmp), tdev, tino, err);
    }

    /* Verify what was written, not what was intended. A backup nobody can
     * restore is worse than a failure, because it is filed and forgotten. */
    atlas_backup_verify_report check;
    atlas_backup_verify_report_init(&check);
    if (st == ATLAS_OK) {
        st = atlas_service_backup_verify(atlas_buf_cstr(&tmp_full), &check, err);
    }
    if (st == ATLAS_OK && !check.ok) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "the backup Atlas just wrote does not verify (%s); nothing published",
                           atlas_backup_verdict_name(check.verdict));
    }
    if (st == ATLAS_OK) {
        out->schema_version = check.schema_version;
        out->size_bytes = check.size_bytes;
        memcpy(out->sha256, check.sha256, sizeof out->sha256);
    }
    atlas_backup_verify_report_free(&check);

    if (st == ATLAS_OK) {
        st = fsync_at(dest.dirfd, atlas_buf_cstr(&tmp), err);
    }
    if (st == ATLAS_OK) {
        st = fault("rename")
                 ? atlas_err_set(err, ATLAS_ERR_CONFIG, "injected rename failure")
                 : (renameat(dest.dirfd, atlas_buf_cstr(&tmp), dest.dirfd,
                             atlas_buf_cstr(&dest.base)) == 0
                        ? ATLAS_OK
                        : atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot publish \"%s\": %s",
                                        atlas_buf_cstr(&dest.full), strerror(errno)));
    }
    if (st == ATLAS_OK) {
        staged = false;
        st = fsync_dir(dest.dirfd, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->path, dest.full.data, dest.full.len, err);
    }

    if (staged) {
        (void)unlinkat(dest.dirfd, atlas_buf_cstr(&tmp), 0);
    }
    atlas_buf_free(&tmp_full);
    atlas_buf_free(&tmp);
    atlas_buf_free(&db_path);
    atlas_buf_free(&data_dir);
    dest_path_free(&dest);
    return st;
}

/* --- verify -------------------------------------------------------------- */

atlas_status atlas_service_backup_verify(const char *path, atlas_backup_verify_report *out,
                                         atlas_err *err) {
    if (path == NULL || path[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas backup verify BACKUP");
    }
    dest_path at;
    dest_path_init(&at);
    atlas_status st = resolve_dest(path, &at, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->path, at.full.data, at.full.len, err);
    }
    if (st != ATLAS_OK) {
        dest_path_free(&at);
        return st;
    }

    struct stat sb;
    if (fstatat(at.dirfd, atlas_buf_cstr(&at.base), &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        out->verdict = ATLAS_BACKUP_UNREADABLE;
        st = atlas_buf_set_str(&out->problems, "no such file", err);
    } else if (S_ISLNK(sb.st_mode)) {
        out->verdict = ATLAS_BACKUP_UNREADABLE;
        st = atlas_buf_set_str(&out->problems,
                               "refusing to read a symbolic link as a backup", err);
    } else if (!S_ISREG(sb.st_mode)) {
        out->verdict = ATLAS_BACKUP_UNREADABLE;
        st = atlas_buf_set_str(&out->problems, "not a regular file", err);
    } else if (sb.st_size == 0) {
        out->verdict = ATLAS_BACKUP_UNREADABLE;
        st = atlas_buf_set_str(&out->problems, "the file is empty", err);
    }
    if (st != ATLAS_OK || out->verdict != ATLAS_BACKUP_OK) {
        dest_path_free(&at);
        return st;
    }

    st = hash_at(at.dirfd, atlas_buf_cstr(&at.base), out->sha256, &out->size_bytes, err);

    /* The SQLite header, checked directly. A truncated or overwritten file
     * would also be caught by sqlite3_open, but only after it had been opened
     * and only with a message about the wrong thing. */
    if (st == ATLAS_OK) {
        int fd = openat(at.dirfd, atlas_buf_cstr(&at.base), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot read \"%s\": %s",
                               atlas_buf_cstr(&out->path), strerror(errno));
        } else {
            static const char MAGIC[16] = "SQLite format 3";
            char head[16];
            ssize_t n = read(fd, head, sizeof head);
            close(fd);
            if (n != (ssize_t)sizeof head || memcmp(head, MAGIC, sizeof MAGIC) != 0) {
                out->verdict = ATLAS_BACKUP_NOT_SQLITE;
                st = atlas_buf_set_str(&out->problems,
                                       "no SQLite header: the file is not a database, or is "
                                       "truncated at the front",
                                       err);
            }
        }
    }
    dest_path_free(&at);
    if (st != ATLAS_OK || out->verdict != ATLAS_BACKUP_OK) {
        return st;
    }
    return atlas_db_backup_inspect(atlas_buf_cstr(&out->path), out, err);
}

/* --- restore ------------------------------------------------------------- */

/* Byte-for-byte copy of a verified static backup into a staged file. The
 * backup is not live, so a plain copy is correct here; taking it through the
 * SQLite backup API instead would rewrite pages and produce a file whose
 * SHA-256 no longer matches what was verified. */
static atlas_status copy_file(const char *src_path, int dirfd, const char *dst_name,
                              atlas_err *err) {
    int in = open(src_path, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot read the backup: %s", strerror(errno));
    }
    int outfd = -1;
    atlas_status st = create_private(dirfd, dst_name, &outfd, err);
    if (st != ATLAS_OK) {
        close(in);
        return st;
    }
    unsigned char *buf = malloc(BACKUP_IO_CHUNK);
    if (buf == NULL) {
        close(in);
        close(outfd);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory staging a restore");
    }
    for (;;) {
        ssize_t n = read(in, buf, BACKUP_IO_CHUNK);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot read the backup: %s",
                               strerror(errno));
            break;
        }
        if (n == 0) {
            break;
        }
        if (fault("write")) {
            st = atlas_err_set(err, ATLAS_ERR_CONFIG, "injected write failure");
            break;
        }
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(outfd, buf + off, (size_t)n - off);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot stage the restore: %s",
                                   strerror(errno));
                break;
            }
            off += (size_t)w;
        }
        if (st != ATLAS_OK) {
            break;
        }
    }
    free(buf);
    close(in);
    if (close(outfd) != 0 && st == ATLAS_OK) {
        st = atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot flush the staged restore");
    }
    if (st != ATLAS_OK) {
        (void)unlinkat(dirfd, dst_name, 0);
    }
    return st;
}

/* Refuse a data-directory entry that is a symbolic link.
 *
 * A restore writes through these names. If `atlas.db` is a link, the rename
 * replaces the link and the target is left behind holding the real index; if
 * `atlas.db-wal` is a link, removing it removes somebody else's file. Neither
 * is a situation Atlas resolves by guessing. */
static atlas_status refuse_symlink(int dirfd, const char *name, atlas_err *err) {
    struct stat sb;
    if (fstatat(dirfd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? ATLAS_OK
                               : atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot inspect \"%s\": %s",
                                               name, strerror(errno));
    }
    if (S_ISLNK(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refusing to restore: \"%s\" in the data directory is a symbolic link",
                             name);
    }
    if (!S_ISREG(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "refusing to restore: \"%s\" in the data directory is not a regular "
                             "file",
                             name);
    }
    return ATLAS_OK;
}

atlas_status atlas_service_backup_restore(const char *data_dir_override,
                                          const atlas_backup_restore_opts *opts,
                                          atlas_backup_restore_report *out, atlas_err *err) {
    if (opts == NULL || opts->input == NULL || opts->input[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas backup restore BACKUP --yes");
    }
    if (!opts->confirmed) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "refusing to restore without --yes: this replaces the Atlas index");
    }

    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_buf staged = ATLAS_BUF_INIT;
    atlas_buf wal_aside = ATLAS_BUF_INIT;
    dest_path dd; /* the data directory, addressed through <data-dir>/atlas.db */
    dest_path_init(&dd);
    atlas_lock *lk = NULL;
    bool have_staged = false;
    bool wal_moved = false;

    atlas_status st = atlas_datadir_resolve(data_dir_override, &data_dir, NULL, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(atlas_buf_cstr(&data_dir), &db_path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->data_dir, data_dir.data, data_dir.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->db_path, db_path.data, db_path.len, err);
    }

    /* Verify the backup first, and completely. Nothing below this point is
     * reached by a file that would not restore. */
    if (st == ATLAS_OK) {
        st = atlas_service_backup_verify(opts->input, &out->source, err);
    }
    if (st == ATLAS_OK && !out->source.ok) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "refusing to restore \"%s\": %s",
                           atlas_buf_cstr(&out->source.path),
                           atlas_backup_verdict_name(out->source.verdict));
    }

    /* The writer lock, exclusively, for the whole operation. This is the same
     * lock the daemon holds for its lifetime, so acquiring it *is* the proof
     * that no daemon is running. */
    if (st == ATLAS_OK) {
        st = atlas_lock_acquire(atlas_buf_cstr(&data_dir), ATLAS_LOCK_ROLE_ONESHOT, &lk, err);
    }

    if (st == ATLAS_OK) {
        st = resolve_dest(atlas_buf_cstr(&db_path), &dd, err);
    }
    if (st == ATLAS_OK) {
        st = refuse_symlink(dd.dirfd, atlas_buf_cstr(&dd.base), err);
    }
    if (st == ATLAS_OK) {
        st = refuse_symlink(dd.dirfd, ATLAS_DB_FILENAME "-wal", err);
    }
    if (st == ATLAS_OK) {
        st = refuse_symlink(dd.dirfd, ATLAS_DB_FILENAME "-shm", err);
    }
    if (st == ATLAS_OK) {
        st = refuse_symlink(dd.dirfd, ATLAS_LOCK_FILENAME, err);
    }

    bool had_db = false;
    if (st == ATLAS_OK) {
        struct stat sb;
        had_db = fstatat(dd.dirfd, atlas_buf_cstr(&dd.base), &sb, AT_SYMLINK_NOFOLLOW) == 0;
    }

    /* A consistent snapshot of what is about to be displaced, taken the same
     * way a backup is: through a read-only connection and the online backup
     * API, so it includes anything still in the write-ahead log. A plain file
     * copy here would preserve the wrong thing. It is kept, not cleaned up. */
    if (st == ATLAS_OK && had_db) {
        char ts[ATLAS_TS_MAX];
        atlas_now_iso8601(ts, sizeof ts);
        atlas_buf name = ATLAS_BUF_INIT;
        st = atlas_buf_appendf(&name, err, ATLAS_DB_FILENAME ".replaced-%s", ts);
        for (size_t i = 0; st == ATLAS_OK && i < name.len; i++) {
            if (name.data[i] == ':') {
                name.data[i] = '-';
            }
        }
        if (st == ATLAS_OK) {
            (void)unlinkat(dd.dirfd, atlas_buf_cstr(&name), 0);
            st = create_private(dd.dirfd, atlas_buf_cstr(&name), NULL, err);
        }
        atlas_buf full = ATLAS_BUF_INIT;
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(&full, err, "%s/%s", atlas_buf_cstr(&data_dir),
                                   atlas_buf_cstr(&name));
        }
        atlas_db *cur = NULL;
        if (st == ATLAS_OK) {
            st = atlas_db_open_readonly(atlas_buf_cstr(&db_path), &cur, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_backup_copy(cur, atlas_buf_cstr(&full), NULL, NULL, err);
            atlas_db_close(cur);
        }
        if (st == ATLAS_OK) {
            st = fsync_at(dd.dirfd, atlas_buf_cstr(&name), err);
        }
        if (st == ATLAS_OK) {
            out->recovery_made = true;
            st = atlas_buf_set(&out->recovery_path, full.data, full.len, err);
        } else {
            (void)unlinkat(dd.dirfd, atlas_buf_cstr(&name), 0);
        }
        atlas_buf_free(&full);
        atlas_buf_free(&name);
    }

    if (st == ATLAS_OK) {
        st = temp_name(&staged, "restore", err);
    }
    if (st == ATLAS_OK) {
        (void)unlinkat(dd.dirfd, atlas_buf_cstr(&staged), 0);
        st = copy_file(atlas_buf_cstr(&out->source.path), dd.dirfd, atlas_buf_cstr(&staged), err);
        have_staged = st == ATLAS_OK;
    }
    /* The staged file must be the backup, byte for byte, before it is allowed
     * to become the index.
     *
     * This is also what closes the gap between verifying the backup and reading
     * it: the copy above opens it by path a second time, and anybody who could
     * swap the file in between would have to produce one with the same SHA-256
     * and length as the file that was verified. */
    if (st == ATLAS_OK) {
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        int64_t size = 0;
        st = hash_at(dd.dirfd, atlas_buf_cstr(&staged), hex, &size, err);
        if (st == ATLAS_OK &&
            (size != out->source.size_bytes || strcmp(hex, out->source.sha256) != 0)) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the staged copy does not match the backup it came from");
        }
    }
    if (st == ATLAS_OK) {
        st = fsync_at(dd.dirfd, atlas_buf_cstr(&staged), err);
    }

    /* --- the commit ------------------------------------------------------
     *
     * Everything above fails with the original database byte-identical. What
     * follows is the only part that is not a no-op on failure, so it is also
     * the only part that is reversible by hand:
     *
     *   1. the previous `-shm` is removed. It is a pure cache and SQLite
     *      recreates it, so this is not a change to any database's content.
     *   2. the previous `-wal` is *renamed aside*, not deleted. It must not
     *      survive the rename below, because SQLite would apply it to the
     *      restored file and corrupt it. Renaming rather than unlinking is what
     *      makes step 3's failure recoverable: the name goes back.
     *   3. the staged copy is renamed over `atlas.db`. If it fails, the `-wal`
     *      is put back and the original database is again exactly what it was.
     *
     * A crash between 2 and 3 leaves the previous database without its
     * write-ahead log. That window is two renames wide with no I/O between
     * them, and the consistent snapshot taken above — which already contains
     * the log's content — is what recovers from it. */
    if (st == ATLAS_OK) {
        struct stat sb;
        if (fstatat(dd.dirfd, ATLAS_DB_FILENAME "-shm", &sb, AT_SYMLINK_NOFOLLOW) == 0) {
            out->removed_shm = unlinkat(dd.dirfd, ATLAS_DB_FILENAME "-shm", 0) == 0;
        }
        if (fstatat(dd.dirfd, ATLAS_DB_FILENAME "-wal", &sb, AT_SYMLINK_NOFOLLOW) == 0) {
            st = atlas_buf_appendf(&wal_aside, err, ".atlas-wal.%ld.aside", (long)getpid());
            if (st == ATLAS_OK) {
                (void)unlinkat(dd.dirfd, atlas_buf_cstr(&wal_aside), 0);
                if (renameat(dd.dirfd, ATLAS_DB_FILENAME "-wal", dd.dirfd,
                             atlas_buf_cstr(&wal_aside)) != 0) {
                    st = atlas_err_set(err, ATLAS_ERR_CONFIG,
                                       "cannot set the previous write-ahead log aside: %s",
                                       strerror(errno));
                } else {
                    wal_moved = true;
                    out->removed_wal = true;
                }
            }
        }
    }
    if (st == ATLAS_OK) {
        st = fault("rename")
                 ? atlas_err_set(err, ATLAS_ERR_CONFIG, "injected rename failure")
                 : (renameat(dd.dirfd, atlas_buf_cstr(&staged), dd.dirfd,
                             atlas_buf_cstr(&dd.base)) == 0
                        ? ATLAS_OK
                        : atlas_err_set(err, ATLAS_ERR_CONFIG, "cannot install the restore: %s",
                                        strerror(errno)));
        if (st == ATLAS_OK) {
            have_staged = false;
            out->published = true;
        }
    }
    if (st == ATLAS_OK) {
        st = fsync_dir(dd.dirfd, err);
    }

    if (out->published) {
        /* The set-aside log belonged to the database that is now the recovery
         * copy, and that copy already contains its content. */
        if (wal_moved) {
            (void)unlinkat(dd.dirfd, atlas_buf_cstr(&wal_aside), 0);
            wal_moved = false;
        }
    } else if (wal_moved) {
        /* Put it back. The original database is byte-identical and complete. */
        (void)renameat(dd.dirfd, atlas_buf_cstr(&wal_aside), dd.dirfd, ATLAS_DB_FILENAME "-wal");
        wal_moved = false;
        out->removed_wal = false;
    }
    if (have_staged) {
        (void)unlinkat(dd.dirfd, atlas_buf_cstr(&staged), 0);
    }

    /* Reopen and recheck. Migration runs only because verification already
     * established the schema is one this build supports; a future schema was
     * refused before the lock was even taken. */
    if (st == ATLAS_OK) {
        out->schema_before = out->source.schema_version;
        atlas_db *db = NULL;
        st = atlas_db_open(atlas_buf_cstr(&db_path), &db, err);
        if (st == ATLAS_OK) {
            out->schema_after = atlas_db_schema_version(db, err);
            out->migrated = out->schema_after > out->schema_before;
            atlas_db_close(db);
        }
    }
    if (st == ATLAS_OK) {
        st = fault("post_verify")
                 ? atlas_err_set(err, ATLAS_ERR_INTEGRITY, "injected post-restore verify failure")
                 : atlas_service_backup_verify(atlas_buf_cstr(&db_path), &out->installed, err);
    }
    if (st == ATLAS_OK && !out->installed.ok) {
        st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                           "the restored index does not verify (%s); the database it replaced is "
                           "at \"%s\"",
                           atlas_backup_verdict_name(out->installed.verdict),
                           out->recovery_made ? atlas_buf_cstr(&out->recovery_path) : "(none)");
    }

    atlas_lock_release(lk);
    atlas_buf_free(&wal_aside);
    atlas_buf_free(&staged);
    atlas_buf_free(&db_path);
    atlas_buf_free(&data_dir);
    dest_path_free(&dd);
    return st;
}

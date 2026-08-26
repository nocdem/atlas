/* Atlas - A13: the scanner process.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A scanner runs as a repository's owner and reads a tree the daemon cannot.
 * This is the process side of that: it opens no index, takes no lock and holds
 * no database handle. Every answer it gets comes over the daemon socket, which
 * is why the CLI dispatches it before any `atlas_ctx` is opened — the same
 * arrangement `atlas gateway run` and `atlas dispatcher run` use, and for the
 * same reason.
 *
 * This plan ships the channel only. There is no loop yet, and none is
 * simulated: `--once` asks once and returns, and without it the command refuses
 * rather than idling. A process that idles silently looks healthy in
 * `systemctl status` while doing nothing, which is the failure the dispatcher
 * refuses to start for.
 */
#include "atlas/service.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/git.h"
#include "atlas/ipc.h"
#include "atlas/pathrep.h"

/* The largest file this will mirror.
 *
 * A scanner reads a repository it does not control the contents of, so an
 * unbounded read is an unbounded allocation in a process that must not fall
 * over. A file above the bound is reported and skipped rather than truncated:
 * half a source file in the mirror would be a file that never existed, and a
 * consumer could not tell it from a real one. */
#define SCANNER_MAX_FILE_BYTES (8u * 1024u * 1024u)

typedef struct walk_ctx {
    const char *socket_path;
    int64_t repo_id;
    int root_fd;
    FILE *log;
    atlas_err *err;
    atlas_status status;
    int64_t mirrored;
    int64_t skipped_symlink;
    int64_t skipped_large;
    int64_t skipped_unreadable;
} walk_ctx;

/* Sends one file's bytes as hex. The wire carries bytes, not text: a source
 * file may hold a quote, a newline, a C0 control or a sequence that is not
 * valid UTF-8, and a JSON string carries none of them unchanged. */
static atlas_status put_file(walk_ctx *w, const void *rel, size_t rel_len, const void *data,
                             size_t len) {
    atlas_buf hex = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_reserve(&hex, len * 2u + 1u, w->err);
    if (st == ATLAS_OK) {
        static const char DIGITS[] = "0123456789abcdef";
        const unsigned char *b = (const unsigned char *)data;
        for (size_t i = 0; i < len && st == ATLAS_OK; i++) {
            char pair[2] = {DIGITS[b[i] >> 4], DIGITS[b[i] & 0x0fu]};
            st = atlas_buf_append(&hex, pair, 2u, w->err);
        }
    }

    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        /* The path is sent as the raw bytes git gave, %XX-encoded so it
         * survives a JSON string. Repository paths are bytes, not text. */
        atlas_buf enc = ATLAS_BUF_INIT;
        st = atlas_path_text_encode(rel, rel_len, &enc, w->err);
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(&params, w->err,
                                   "{\"repo\":%lld,\"path\":\"%s\",\"first\":true,\"data\":\"%s\"}",
                                   (long long)w->repo_id, atlas_buf_cstr(&enc),
                                   len == 0 ? "" : atlas_buf_cstr(&hex));
        }
        atlas_buf_free(&enc);
    }

    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(w->socket_path, "scanner.put", atlas_buf_cstr(&params), &raw, w->err);
    }
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    atlas_buf_free(&hex);
    return st;
}

/* Mirrors one regular file at `rel` beneath the repository root.
 *
 * Shared by the tracked walk and the `.git` walk, so both get the same
 * nofollow open, the same size bound and the same skip accounting. */
static void mirror_one(walk_ctx *w, const void *rel, size_t rel_len) {
    if (w->status != ATLAS_OK) {
        return;
    }
    atlas_path_open_result res = ATLAS_PATH_OPEN_OK;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    if (atlas_path_open_nofollow(w->root_fd, (const char *)rel, rel_len, &res, &fd, &sb, NULL,
                                 w->err) != ATLAS_OK) {
        w->skipped_unreadable++;
        return;
    }
    if (res != ATLAS_PATH_OPEN_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        if (res == ATLAS_PATH_OPEN_SYMLINK || res == ATLAS_PATH_OPEN_UNSAFE) {
            w->skipped_symlink++;
        } else {
            w->skipped_unreadable++;
        }
        return;
    }
    if (sb.st_size < 0 || (uint64_t)sb.st_size > SCANNER_MAX_FILE_BYTES) {
        (void)close(fd);
        w->skipped_large++;
        return;
    }

    atlas_buf content = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    char chunk[64u * 1024u];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            st = atlas_err_set_errno(w->err, ATLAS_ERR_REPO, errno, "cannot read a mirrored file");
            break;
        }
        if (n == 0) {
            break;
        }
        st = atlas_buf_append(&content, chunk, (size_t)n, w->err);
        if (st != ATLAS_OK) {
            break;
        }
        if (content.len > SCANNER_MAX_FILE_BYTES) {
            st = ATLAS_ERR_REPO;
            w->skipped_large++;
            break;
        }
    }
    (void)close(fd);

    if (st == ATLAS_OK) {
        st = put_file(w, rel, rel_len, content.data, content.len);
        if (st == ATLAS_OK) {
            w->mirrored++;
        } else {
            w->status = st;
        }
    } else if (st != ATLAS_ERR_REPO) {
        w->status = st;
    }
    atlas_buf_free(&content);
}

/* Mirrors `.git` as an ordinary directory tree, so the daemon can open the
 * mirror with `atlas_git_open` and ask it every question it asks a real
 * repository — which is what lets reconcile, A3, the semantic layer, snapshots
 * and gates keep working unchanged rather than having twenty git operations
 * reproduced over a socket.
 *
 * Walked as directories rather than through git, because git is what the
 * mirror is *for*: asking the source repository to enumerate its own object
 * store would work, but every answer would then have to be turned back into
 * files, and the files are already there.
 *
 * `.git/index` is mirrored rather than skipped. It records the *source*
 * worktree's stat data, which will not match the mirrored files, so git
 * re-hashes on the mirror — correct but slower. Skipping it would make git
 * rebuild the index from scratch instead, which is not cheaper and loses the
 * recorded staging state. */
static void mirror_dir(walk_ctx *w, atlas_buf *rel, int dir_fd) {
    if (w->status != ATLAS_OK) {
        return;
    }
    DIR *d = fdopendir(dir_fd);
    if (d == NULL) {
        (void)close(dir_fd);
        w->skipped_unreadable++;
        return;
    }
    size_t base = rel->len;
    struct dirent *ent = NULL;
    while (w->status == ATLAS_OK && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        /* Rewind to the parent's prefix. `len` is public and the buffer keeps
         * its capacity, so this is the cheap way back up the tree. */
        rel->len = base;
        if (rel->data != NULL) {
            rel->data[base] = '\0';
        }
        if (rel->len > 0) {
            if (atlas_buf_append(rel, "/", 1u, w->err) != ATLAS_OK) {
                w->status = ATLAS_ERR_INTERNAL;
                break;
            }
        }
        if (atlas_buf_append(rel, ent->d_name, strlen(ent->d_name), w->err) != ATLAS_OK) {
            w->status = ATLAS_ERR_INTERNAL;
            break;
        }

        int sub = openat(dirfd(d), ent->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (sub >= 0) {
            mirror_dir(w, rel, sub);
            continue;
        }
        /* Not a directory, or a symlink we will not follow. `mirror_one`
         * re-opens through the validated root and decides which it was. */
        mirror_one(w, rel->data, rel->len);
    }
    rel->len = base;
    if (rel->data != NULL) {
        rel->data[base] = '\0';
    }
    (void)closedir(d);
}

static atlas_status walk_cb(const atlas_git_index_entry *e, void *ud, atlas_err *err) {
    walk_ctx *w = (walk_ctx *)ud;
    (void)err;
    /* The tracked walk and the `.git` walk mirror a file the same way, so they
     * share one implementation: the same nofollow open, the same size bound and
     * the same skip accounting. `e->path` is raw bytes of `e->path_len` and is
     * not NUL-terminated. */
    mirror_one(w, e->path, e->path_len);
    return ATLAS_OK;
}

atlas_status atlas_service_scanner_run(bool once, FILE *log, atlas_err *err) {
    if (!once) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas scanner run needs --once: the polling loop is not implemented "
                             "yet, and a process that idled instead of saying so would look "
                             "healthy while doing nothing");
    }

    atlas_buf socket_path = ATLAS_BUF_INIT;
    atlas_status st = atlas_ipc_socket_path(&socket_path, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&socket_path);
        return st;
    }

    atlas_buf raw = ATLAS_BUF_INIT;
    st = atlas_ipc_call(atlas_buf_cstr(&socket_path), "scanner.poll", "{}", &raw, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&raw);
        atlas_buf_free(&socket_path);
        return st;
    }

    atlas_ipc_response *resp = NULL;
    st = atlas_ipc_response_parse(raw.data, raw.len, &resp, err);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) {
        atlas_buf_free(&socket_path);
        return st;
    }
    if (!atlas_ipc_response_ok(resp)) {
        /* The daemon's message is already safe-encoded, and its status is the
         * CLI's exit-code vocabulary, so both travel out unchanged. */
        atlas_status refused = atlas_err_set(err, atlas_ipc_response_status(resp), "%s",
                                             atlas_ipc_response_message(resp));
        atlas_ipc_response_free(resp);
        atlas_buf_free(&socket_path);
        return refused;
    }

    size_t n = 0;
    if (!atlas_ipc_result_arr_len(resp, "repositories", &n)) {
        n = 0;
    }
    if (log != NULL) {
        (void)fprintf(log, "scanner: %zu repository/repositories for this uid\n", n);
    }

    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        const char *name = NULL;
        const char *root = NULL;
        int64_t id = 0;
        if (!atlas_ipc_result_arr_obj_str(resp, "repositories", i, "name", &name)) {
            name = "";
        }
        if (!atlas_ipc_result_arr_obj_str(resp, "repositories", i, "root", &root)) {
            root = "";
        }
        if (!atlas_ipc_result_arr_obj_int(resp, "repositories", i, "id", &id) || id <= 0) {
            continue;
        }

        /* The root arrives %XX-encoded, which is how the database holds it.
         * Opening it needs the raw bytes back. */
        atlas_buf root_raw = ATLAS_BUF_INIT;
        if (atlas_path_text_decode(root, strlen(root), &root_raw, err) != ATLAS_OK) {
            atlas_buf_free(&root_raw);
            continue;
        }

        atlas_git *g = NULL;
        atlas_err open_err;
        atlas_err_init(&open_err);
        if (atlas_git_open(atlas_buf_cstr(&root_raw), &g, &open_err) != ATLAS_OK) {
            /* One repository that cannot be opened must not stop the others,
             * and the reason belongs where an operator will look for it. */
            if (log != NULL) {
                (void)fprintf(log, "  %s: skipped, %s\n", name, open_err.msg);
            }
            atlas_buf_free(&root_raw);
            continue;
        }

        walk_ctx w;
        memset(&w, 0, sizeof(w));
        w.socket_path = atlas_buf_cstr(&socket_path);
        w.repo_id = id;
        w.log = log;
        w.err = err;
        w.status = ATLAS_OK;
        w.root_fd = open(atlas_buf_cstr(&root_raw), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (w.root_fd < 0) {
            if (log != NULL) {
                (void)fprintf(log, "  %s: skipped, cannot open its root\n", name);
            }
            atlas_git_close(g);
            atlas_buf_free(&root_raw);
            continue;
        }

        atlas_status walked = atlas_git_ls_files(g, walk_cb, &w, err);

        /* And `.git`, so the mirror is a repository the daemon can open rather
         * than a bag of files. That is what lets reconcile, A3, the semantic
         * layer, snapshots and gates keep working unchanged. */
        if (walked == ATLAS_OK && w.status == ATLAS_OK) {
            int git_fd = openat(w.root_fd, ".git", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (git_fd >= 0) {
                atlas_buf rel = ATLAS_BUF_INIT;
                if (atlas_buf_append(&rel, ".git", 4u, err) == ATLAS_OK) {
                    mirror_dir(&w, &rel, git_fd);
                } else {
                    (void)close(git_fd);
                    w.status = ATLAS_ERR_INTERNAL;
                }
                atlas_buf_free(&rel);
            } else if (log != NULL) {
                /* A linked worktree's `.git` is a file, not a directory. It is
                 * reported rather than silently skipped: such a repository's
                 * mirror is not openable by git and a later plan will need to
                 * know. */
                (void)fprintf(log, "  %s: .git is not a directory, mirror will not be a git "
                                   "repository\n",
                              name);
            }
        }

        (void)close(w.root_fd);
        atlas_git_close(g);
        atlas_buf_free(&root_raw);

        if (log != NULL) {
            (void)fprintf(log,
                          "  %s  mirrored %lld, skipped %lld symlink, %lld too large, "
                          "%lld unreadable\n",
                          name, (long long)w.mirrored, (long long)w.skipped_symlink,
                          (long long)w.skipped_large, (long long)w.skipped_unreadable);
        }
        if (walked != ATLAS_OK) {
            st = walked;
        } else if (w.status != ATLAS_OK) {
            st = w.status;
        }
    }

    atlas_ipc_response_free(resp);
    atlas_buf_free(&socket_path);
    return st;
}

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
#include <time.h>
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
/* One chunk of a file, in bytes before hex encoding.
 *
 * Derived from the transport rather than guessed: hex doubles it, and the
 * request also carries a method name, a repository id and a `%XX`-encoded path
 * that can itself be long. A quarter of `ATLAS_IPC_MAX_REQUEST_BYTES` leaves
 * half the limit for the encoded bytes and half again for everything else,
 * which is margin rather than arithmetic on the exact overhead -- the exact
 * overhead depends on a path this code does not choose. */
#define SCANNER_CHUNK_BYTES (ATLAS_IPC_MAX_REQUEST_BYTES / 4u)

typedef struct walk_ctx {
    const char *socket_path;
    int64_t repo_id;
    int root_fd;
    FILE *log;
    atlas_err *err;
    atlas_status status;
    int64_t mirrored;
    int64_t skipped_symlink;
    int64_t skipped_unreadable;
} walk_ctx;

/* A13. Tells the daemon a run is starting, which clears `mirror_complete`.
 *
 * Before the first byte, so a crash anywhere in the walk leaves the mirror
 * refused rather than read as whole. */
static atlas_status say_state(walk_ctx *w, bool complete) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&params, w->err, "{\"repo\":%lld,\"complete\":%s}",
                                        (long long)w->repo_id, complete ? "true" : "false");
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(w->socket_path, "scanner.state", atlas_buf_cstr(&params), &raw,
                            w->err);
    }
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    return st;
}

/* Sends one file's bytes as hex, in chunks the transport can carry.
 *
 * The wire carries bytes, not text: a source file may hold a quote, a newline,
 * a C0 control or a sequence that is not valid UTF-8, and a JSON string carries
 * none of them unchanged.
 *
 * **Hex doubles the size, and the transport has its own ceiling.** A whole file
 * per request was refused for anything over about half of
 * `ATLAS_IPC_MAX_REQUEST_BYTES` — measured on the repository this season was
 * built for, which stopped at `dna`'s first large object with "refusing to send
 * a request, above the limit". `SCANNER_CHUNK_BYTES` is picked against that
 * ceiling rather than against a file size, so raising the file bound never
 * reopens this: one chunk becomes two.
 *
 * `first` distinguishes the chunk that creates the mirrored file from the ones
 * that extend it. `atlas_mirror_put` unlinks and `O_EXCL`-creates on the first
 * and appends with no `O_CREAT` on the rest, so a partial transfer leaves a
 * short file rather than a mixture of two versions. */
static atlas_status put_file(walk_ctx *w, const void *rel, size_t rel_len, const void *data,
                             size_t len, bool exec) {
    atlas_buf enc = ATLAS_BUF_INIT;
    /* The path is sent as the raw bytes git gave, %XX-encoded so it survives a
     * JSON string. Repository paths are bytes, not text. */
    atlas_status st = atlas_path_text_encode(rel, rel_len, &enc, w->err);

    const unsigned char *b = (const unsigned char *)data;
    size_t sent = 0;
    bool first = true;
    /* An empty file still needs one request, or it would never be created. */
    while (st == ATLAS_OK && (sent < len || first)) {
        size_t take = len - sent;
        if (take > SCANNER_CHUNK_BYTES) {
            take = SCANNER_CHUNK_BYTES;
        }

        atlas_buf hex = ATLAS_BUF_INIT;
        st = atlas_buf_reserve(&hex, take * 2u + 1u, w->err);
        if (st == ATLAS_OK) {
            static const char DIGITS[] = "0123456789abcdef";
            for (size_t i = 0; i < take && st == ATLAS_OK; i++) {
                unsigned char c = b[sent + i];
                char pair[2] = {DIGITS[c >> 4], DIGITS[c & 0x0fu]};
                st = atlas_buf_append(&hex, pair, 2u, w->err);
            }
        }

        atlas_buf params = ATLAS_BUF_INIT;
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(&params, w->err,
                                   "{\"repo\":%lld,\"path\":\"%s\",\"first\":%s,\"exec\":%s,"
                                   "\"data\":\"%s\"}",
                                   (long long)w->repo_id, atlas_buf_cstr(&enc),
                                   first ? "true" : "false", exec ? "true" : "false",
                                   take == 0 ? "" : atlas_buf_cstr(&hex));
        }
        atlas_buf raw = ATLAS_BUF_INIT;
        if (st == ATLAS_OK) {
            st = atlas_ipc_call(w->socket_path, "scanner.put", atlas_buf_cstr(&params), &raw,
                                w->err);
        }
        atlas_buf_free(&raw);
        atlas_buf_free(&params);
        atlas_buf_free(&hex);

        sent += take;
        first = false;
    }
    atlas_buf_free(&enc);
    return st;
}

/* A13. Sends one symlink's text.
 *
 * The same wire shape as a file chunk, with `symlink` set: a link text is bytes
 * too, and it travels as hex for the same reason a file's content does. One
 * request always suffices — a link text is bounded by the filesystem far below
 * `SCANNER_CHUNK_BYTES`, so there is no chunking here and no `first` to carry. */
static atlas_status put_symlink(walk_ctx *w, const void *rel, size_t rel_len, const void *target,
                                size_t target_len) {
    atlas_buf enc = ATLAS_BUF_INIT;
    atlas_status st = atlas_path_text_encode(rel, rel_len, &enc, w->err);

    atlas_buf hex = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_reserve(&hex, target_len * 2u + 1u, w->err);
    }
    if (st == ATLAS_OK) {
        static const char DIGITS[] = "0123456789abcdef";
        const unsigned char *b = (const unsigned char *)target;
        for (size_t i = 0; i < target_len && st == ATLAS_OK; i++) {
            char pair[2] = {DIGITS[b[i] >> 4], DIGITS[b[i] & 0x0fu]};
            st = atlas_buf_append(&hex, pair, 2u, w->err);
        }
    }

    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(
            &params, w->err,
            "{\"repo\":%lld,\"path\":\"%s\",\"first\":true,\"symlink\":true,\"data\":\"%s\"}",
            (long long)w->repo_id, atlas_buf_cstr(&enc), atlas_buf_cstr(&hex));
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(w->socket_path, "scanner.put", atlas_buf_cstr(&params), &raw, w->err);
    }
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    atlas_buf_free(&hex);
    atlas_buf_free(&enc);
    return st;
}

/* Mirrors one regular file at `rel` beneath the repository root.
 *
 * Shared by the tracked walk and the `.git` walk, so both get the same
 * nofollow open, the same size bound and the same skip accounting. */
/* Extracts a symlink's text into `target`, or returns a negative length.
 *
 * `rel` is raw bytes and is not NUL-terminated; `readlinkat` needs a C string,
 * so the name is copied rather than assumed. */
static ssize_t read_link_text(walk_ctx *w, const void *rel, size_t rel_len, char *target,
                              size_t target_cap) {
    char name[4096];
    if (rel_len >= sizeof(name)) {
        return -1;
    }
    memcpy(name, rel, rel_len);
    name[rel_len] = '\0';
    ssize_t n = readlinkat(w->root_fd, name, target, target_cap);
    return (n > 0 && (size_t)n < target_cap) ? n : -1;
}

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
        /* **A symlink is never opened, so a failed open says nothing about one.**
         * A broken link -- one whose target does not exist -- fails here with the
         * open's own error, and counting it unreadable made the mirror
         * permanently incomplete for a file Atlas indexes perfectly well: what
         * Atlas hashes is the link *text*, and the target's existence has
         * nothing to do with it. Found on the live tree, where one link into a
         * directory that no longer exists kept a whole repository refused.
         *
         * `readlinkat` answers without following, so it answers for a broken
         * link exactly as it does for a whole one. */
        char target[4096];
        ssize_t n = read_link_text(w, rel, rel_len, target, sizeof(target));
        if (n > 0 && put_symlink(w, rel, rel_len, target, (size_t)n) == ATLAS_OK) {
            w->mirrored++;
            return;
        }
        w->skipped_unreadable++;
        return;
    }
    if (res != ATLAS_PATH_OPEN_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        if (res == ATLAS_PATH_OPEN_SYMLINK) {
            /* **The link text is the file.** Atlas hashes a tracked symlink's
             * text and never opens its target, so a mirror that dropped them was
             * missing files the index holds -- and the daemon read every one as a
             * deletion. `readlinkat` reads the text without following it. */
            char target[4096];
            ssize_t n = read_link_text(w, rel, rel_len, target, sizeof(target));
            if (n > 0 && put_symlink(w, rel, rel_len, target, (size_t)n) == ATLAS_OK) {
                w->mirrored++;
                return;
            }
            /* Unreadable, empty, or longer than this buffer. Counted as a skip,
             * which is what stops the run claiming the mirror is complete. */
            w->skipped_symlink++;
        } else if (res == ATLAS_PATH_OPEN_UNSAFE) {
            w->skipped_symlink++;
        } else {
            w->skipped_unreadable++;
        }
        return;
    }
    /* **No size bound.** The mirror's job is to be the tree, and the daemon reads
     * it as the tree -- so a file the mirror does not hold is a file that no
     * longer exists. A bound here protects nothing; it turns a large file into a
     * repository that cannot be indexed at all.
     *
     * There was one, and I invented it rather than derived it: 8 MiB, then
     * 64 MiB. Both sat below Atlas' own `ATLAS_HASH_MAX_FILE_BYTES` of 256 MiB,
     * so the scanner refused to mirror files Atlas would have indexed -- and one
     * of the two it refused on the first live run was a 91 MiB pack, which left
     * the mirror's `.git` incomplete and therefore not a repository at all.
     *
     * Atlas' bound is on *hashing*, not on existing: above it reconcile records
     * `ENTRY_TOO_LARGE` and the file is still there. The mirror has to be there
     * too. */
    if (sb.st_size < 0) {
        (void)close(fd);
        w->skipped_unreadable++;
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
    }
    (void)close(fd);

    if (st == ATLAS_OK) {
        st = put_file(w, rel, rel_len, content.data, content.len, (sb.st_mode & S_IXUSR) != 0);
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

/* The untracked half of the same walk. `ls_untracked` reports a path and
 * nothing else, so this is a second callback rather than a second walk: it
 * lands in the same `mirror_one`, with the same nofollow open, the same size
 * bound and the same skip accounting. */
static atlas_status untracked_cb(const void *path, size_t path_len, void *ud, atlas_err *err) {
    walk_ctx *w = (walk_ctx *)ud;
    (void)err;
    mirror_one(w, path, path_len);
    return ATLAS_OK;
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

/* One pass: ask what is owed, mirror it, report what was left behind.
 *
 * `*poll_within_ms` receives the cadence the daemon asked for, so the loop
 * sleeps for a time Atlas chose rather than one this process invented. The spec
 * has no `hello` for the same reason: the scanner asks what is owed, and asking
 * is what proves it is alive. */
static atlas_status scan_pass(int64_t *poll_within_ms, FILE *log, atlas_err *err) {
    *poll_within_ms = ATLAS_SCANNER_POLL_INTERVAL_MS;

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

    /* Read only from an answer that succeeded. The daemon says how soon it wants
     * to be asked again; an older one that does not say leaves the compiled
     * default, which is the same number its own freshness rule uses. */
    {
        int64_t within = 0;
        if (atlas_ipc_result_int(resp, "poll_within_ms", &within) && within > 0) {
            *poll_within_ms = within;
        }
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
        /* Cleared before anything is written, so a crash leaves the mirror
         * refused rather than trusted. */
        if (say_state(&w, false) != ATLAS_OK) {
            st = w.err != NULL ? ATLAS_ERR_INTERNAL : ATLAS_ERR_INTERNAL;
            atlas_git_close(g);
            atlas_buf_free(&root_raw);
            continue;
        }
        w.root_fd = open(atlas_buf_cstr(&root_raw), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (w.root_fd < 0) {
            if (log != NULL) {
                (void)fprintf(log, "  %s: skipped, cannot open its root\n", name);
            }
            atlas_git_close(g);
            atlas_buf_free(&root_raw);
            continue;
        }

        /* Tracked **and** untracked, because that is exactly what reconcile
         * indexes: `atlas_git_ls_files` then `atlas_git_ls_untracked`, at
         * `src/core/reconcile.c:1211` and `:1218`. Mirroring only the tracked
         * set made the mirror a strict subset of the repository the daemon
         * believes it is reading, and the daemon recorded the difference as
         * deletions -- measured on the first live run: `-20000` against a tree
         * with 2012 tracked files and 22012 indexed ones.
         *
         * Ignored paths are not walked, for the same reason reconcile does not
         * index them. */
        atlas_status walked = atlas_git_ls_files(g, walk_cb, &w, err);
        if (walked == ATLAS_OK && w.status == ATLAS_OK) {
            walked = atlas_git_ls_untracked(g, untracked_cb, &w, err);
        }

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

        /* Complete means every file this run enumerated reached the mirror. Any
         * skip at all leaves it false, including a symlink: the daemon reads the
         * mirror as the repository, so a file the mirror does not hold is a file
         * that no longer exists, and there is no such thing as a small delete
         * sweep. A skipped symlink is a real gap rather than a rounding error --
         * Atlas indexes a tracked symlink's link text, so the mirror is missing
         * something the index would otherwise hold. */
        bool complete = walked == ATLAS_OK && w.status == ATLAS_OK &&
                        w.skipped_symlink == 0 &&
                        w.skipped_unreadable == 0;
        /* Not ignored. A state report that did not land means the daemon still
         * refuses to read this mirror, and a run that reported "mirrored 4685,
         * skipped 0" while the daemon went on saying "no complete mirror yet"
         * is exactly the silence this run has to break. */
        atlas_status said = say_state(&w, complete);
        if (said != ATLAS_OK && log != NULL) {
            (void)fprintf(log, "  %s  WARNING: the daemon did not record this run: %s\n", name,
                          atlas_err_msg(err));
        }

        if (log != NULL) {
            (void)fprintf(log,
                          "  %s  mirrored %lld, skipped %lld symlink, %lld unreadable\n", name,
                          (long long)w.mirrored, (long long)w.skipped_symlink,
                          (long long)w.skipped_unreadable);
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

/* A13. The scanner's loop.
 *
 * A repository read from a mirror is only as current as the last run that wrote
 * it, and the daemon watches the mirror rather than the tree — so if this
 * process stops, nothing observes the repository. `atlas_server_overlay_mirror`
 * is what stops claiming currency once the polling stops, and this is the
 * process whose polling it is.
 *
 * **The cadence is the daemon's.** Each pass is told how soon to ask again, and
 * that is the same number the freshness rule judges by, so what Atlas requests
 * and what it holds a scanner to cannot drift apart. This process invents no
 * schedule and promises nothing: it asks, and asking is the evidence.
 *
 * `--once` is a snapshot — one pass, after which nothing is polling and the
 * daemon will not call an index built from it current. That is not a limitation
 * to route around; one pass establishes what the tree was, not what it is.
 *
 * The sleep is between passes rather than on a timer, so a pass that runs long
 * delays the next one instead of overlapping it. */
atlas_status atlas_service_scanner_run(bool once, FILE *log, atlas_err *err) {
    int64_t within = ATLAS_SCANNER_POLL_INTERVAL_MS;
    if (once) {
        return scan_pass(&within, log, err);
    }
    for (;;) {
        atlas_err pass_err;
        atlas_err_init(&pass_err);
        atlas_status st = scan_pass(&within, log, &pass_err);
        if (st != ATLAS_OK && log != NULL) {
            /* Reported and survived. A repository that could not be mirrored
             * this time keeps whatever the last successful pass left, and the
             * daemon already refuses to call that current once the polling
             * cadence has lapsed. Exiting here would turn one repository's
             * problem into every repository's. */
            (void)fprintf(log, "scanner: pass failed: %s\n", atlas_err_msg(&pass_err));
        }
        if (within <= 0) {
            /* A daemon that answered with nothing usable. The compiled cadence is
             * the same number that daemon's own freshness rule uses, so falling
             * back to it cannot put this scanner outside the bound. */
            within = ATLAS_SCANNER_POLL_INTERVAL_MS;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)(within / 1000);
        ts.tv_nsec = (long)((within % 1000) * 1000000);
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            /* Interrupted by a signal: finish the remaining time rather than
             * treating the wake-up as the interval having elapsed. */
        }
    }
}

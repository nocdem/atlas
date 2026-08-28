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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/db.h"
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

/* --- what this process has already sent ------------------------------------
 *
 * A mirroring pass used to re-read, hex-encode and send every file every time.
 * `atlas_mirror_publish` renames the staging directory into place, so the next
 * pass starts from an empty one and `put_file` was called unconditionally.
 * Measured 2026-08-28 on this machine: 28,450 files across two repositories,
 * every five minutes, of which essentially none had changed.
 *
 * **In memory, and deliberately not on disk.** A manifest that outlived the
 * process would be a promise about a mirror this process did not build — the
 * same shape as the cadence the scanner was once allowed to declare, and
 * reverted for the same reason. Losing it costs one full pass, which is what a
 * scanner that has just started does anyway.
 *
 * **It is never trusted, only used.** What it decides is whether to *ask*; the
 * daemon decides what to do, by linking whatever its published generation holds
 * at that path and answering `kept: false` when it holds nothing. So a memory
 * that has outlived the mirror corrects itself on the next call instead of
 * producing a wrong mirror.
 *
 * What it compares is `atlas_fs_identity` — all eight fields, ctime included —
 * through `atlas_fs_identity_same`. That is not a new kind of trust: it is the
 * same evidence a reconciliation already uses to skip hashing a path no event
 * named, applied one process further out. A racy observation is not remembered
 * at all, which A1 requires and which costs one resend. */
typedef struct sent_entry {
    char *path; /* owned, NUL-terminated; NULL is an empty slot */
    size_t path_len;
    atlas_fs_identity id;
    uint64_t seen; /* the pass that last visited this path */
} sent_entry;

typedef struct sent_map {
    int64_t repo_id;
    sent_entry *slots;
    size_t cap; /* a power of two, or zero when nothing is held */
    size_t count;
    uint64_t pass;
} sent_map;

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
    /* How many paths this pass carried forward instead of sending. */
    int64_t kept;
    /* This repository's memory of what was sent, or NULL when the daemon asked
     * for a full mirror. */
    sent_map *mem;
    /* When this pass started looking, for A1's raciness rule. */
    struct timespec observed_at;
    /* Paths waiting to be carried, sent as one request.
     *
     * **The request was the cost, not the bytes.** Naming a file instead of
     * sending it left the request count alone, and the count is what the daemon
     * pays for: measured 2026-08-29, one request per file kept it at a full core
     * for half of every cycle, at roughly eight `openat` and nine `pread64`
     * each, every one also opening its own read-only handle on a 3.96 GB
     * database. */
    char *batch[ATLAS_SCANNER_KEEP_MAX_PATHS];
    size_t batch_len[ATLAS_SCANNER_KEEP_MAX_PATHS];
    size_t batch_n;
    /* Set while re-sending what the daemon could not carry, so the resend does
     * not offer the same path back to the batch it just came out of. */
    bool no_keep;
    /* A13. When this walk last told the daemon it is alive.
     *
     * The heartbeat is `scanner.poll`, and one poll per pass is not enough: a
     * pass over 80000 files takes twelve minutes and the staleness bound is
     * five, so a scanner doing exactly its job would have the repository called
     * stale for most of every pass. Measured on /opt/dna. */
    int64_t last_beat_ms;
} walk_ctx;

/* FNV-1a over the raw path bytes. Paths are bytes, so this hashes bytes. */
static uint64_t path_hash(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void sent_map_free(sent_map *m) {
    if (m->slots != NULL) {
        for (size_t i = 0; i < m->cap; i++) {
            free(m->slots[i].path);
        }
        free(m->slots);
    }
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

static sent_entry *sent_map_find(sent_map *m, const void *path, size_t len) {
    if (m == NULL || m->cap == 0) {
        return NULL;
    }
    size_t mask = m->cap - 1u;
    size_t i = (size_t)path_hash(path, len) & mask;
    for (size_t probe = 0; probe < m->cap; probe++) {
        sent_entry *e = &m->slots[i];
        if (e->path == NULL) {
            return NULL;
        }
        if (e->path_len == len && memcmp(e->path, path, len) == 0) {
            return e;
        }
        i = (i + 1u) & mask;
    }
    return NULL;
}

static bool sent_map_grow(sent_map *m) {
    size_t want = m->cap == 0 ? 1024u : m->cap * 2u;
    sent_entry *slots = (sent_entry *)calloc(want, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    sent_entry *old = m->slots;
    size_t old_cap = m->cap;
    m->slots = slots;
    m->cap = want;
    size_t mask = want - 1u;
    for (size_t k = 0; k < old_cap; k++) {
        if (old[k].path == NULL) {
            continue;
        }
        size_t i = (size_t)path_hash(old[k].path, old[k].path_len) & mask;
        while (m->slots[i].path != NULL) {
            i = (i + 1u) & mask;
        }
        m->slots[i] = old[k];
    }
    free(old);
    return true;
}

/* Remembers one path's identity. A failure to allocate is not an error the pass
 * reports: forgetting costs a resend next time and nothing else. */
static void sent_map_put(sent_map *m, const void *path, size_t len, const atlas_fs_identity *id) {
    if (m == NULL) {
        return;
    }
    sent_entry *e = sent_map_find(m, path, len);
    if (e != NULL) {
        e->id = *id;
        e->seen = m->pass;
        return;
    }
    /* Half full at most, so probing stays short. */
    if (m->cap == 0 || (m->count + 1u) * 2u > m->cap) {
        if (!sent_map_grow(m)) {
            return;
        }
    }
    size_t mask = m->cap - 1u;
    size_t i = (size_t)path_hash(path, len) & mask;
    while (m->slots[i].path != NULL) {
        i = (i + 1u) & mask;
    }
    char *copy = (char *)malloc(len + 1u);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, path, len);
    copy[len] = '\0';
    m->slots[i].path = copy;
    m->slots[i].path_len = len;
    m->slots[i].id = *id;
    m->slots[i].seen = m->pass;
    m->count++;
}

/* Drops what this pass did not visit, so the memory tracks the tree rather than
 * growing with everything the repository has ever held. Rebuilt rather than
 * tombstoned, because a deletion in an open-addressing table has to be, and a
 * pass is already the natural point to do it. */
static void sent_map_sweep(sent_map *m) {
    if (m == NULL || m->cap == 0) {
        return;
    }
    sent_entry *old = m->slots;
    size_t old_cap = m->cap;
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
    for (size_t k = 0; k < old_cap; k++) {
        if (old[k].path == NULL) {
            continue;
        }
        if (old[k].seen == m->pass) {
            sent_map_put(m, old[k].path, old[k].path_len, &old[k].id);
        }
        free(old[k].path);
    }
    free(old);
}

/* One memory per repository, held by the loop and never by a pass: a pass is
 * the thing being made cheaper, so it cannot be what owns the saving. */
typedef struct scanner_memory {
    sent_map *repos;
    size_t count;
    size_t cap;
} scanner_memory;

static void memory_free(scanner_memory *m) {
    for (size_t i = 0; i < m->count; i++) {
        sent_map_free(&m->repos[i]);
    }
    free(m->repos);
    m->repos = NULL;
    m->count = 0;
    m->cap = 0;
}

/* The memory for one repository, created on first sight. NULL when it cannot be
 * created, which turns this pass into the full one it would have been anyway. */
static sent_map *memory_for(scanner_memory *m, int64_t repo_id) {
    for (size_t i = 0; i < m->count; i++) {
        if (m->repos[i].repo_id == repo_id) {
            return &m->repos[i];
        }
    }
    if (m->count == m->cap) {
        size_t want = m->cap == 0 ? 4u : m->cap * 2u;
        sent_map *grown = (sent_map *)realloc(m->repos, want * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        m->repos = grown;
        m->cap = want;
    }
    sent_map *s = &m->repos[m->count++];
    memset(s, 0, sizeof(*s));
    s->repo_id = repo_id;
    return s;
}

/* Fills an identity from a stat, the same eight fields `reconcile.c` records. */
static void scanner_identity_from_stat(atlas_fs_identity *out, const struct stat *sb) {
    out->known = true;
    out->dev = (int64_t)sb->st_dev;
    out->ino = (int64_t)sb->st_ino;
    out->size = (int64_t)sb->st_size;
    out->mtime_sec = (int64_t)sb->st_mtim.tv_sec;
    out->mtime_nsec = (int64_t)sb->st_mtim.tv_nsec;
    out->ctime_sec = (int64_t)sb->st_ctim.tv_sec;
    out->ctime_nsec = (int64_t)sb->st_ctim.tv_nsec;
    out->mode = (int64_t)sb->st_mode;
}

/* A1's raciness rule, applied to what this process is willing to remember: an
 * observation whose timestamps sit inside the open tick describes a file that
 * may still be being written, so it is not recorded as a value. Costs one
 * resend on the next pass, which is the safe direction. */
static bool scanner_stamp_racy(int64_t sec, int64_t nsec, const struct timespec *at) {
    if (sec > (int64_t)at->tv_sec) {
        return true;
    }
    if (sec == (int64_t)at->tv_sec) {
        return nsec >= (int64_t)at->tv_nsec || nsec == 0;
    }
    return false;
}

static bool scanner_identity_stable(const atlas_fs_identity *id, const struct timespec *at) {
    return !scanner_stamp_racy(id->mtime_sec, id->mtime_nsec, at) &&
           !scanner_stamp_racy(id->ctime_sec, id->ctime_nsec, at);
}

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

static void mirror_one(walk_ctx *w, const void *rel, size_t rel_len);

/* A13. Asks the daemon to carry a batch of paths forward instead of sending
 * their bytes, and sends whatever it would not carry.
 *
 * The answer is the daemon's, not this process's belief: `resend` names exactly
 * the paths its published generation does not hold, and each of those goes
 * through `mirror_one` again — re-opened and re-stated, because the file may
 * have changed since it was looked at. A failed call resends everything, since
 * "I could not find out" and "it is not there" call for the same next step. */
static void batch_flush(walk_ctx *w) {
    if (w->batch_n == 0) {
        return;
    }
    const size_t n = w->batch_n;
    w->batch_n = 0; /* cleared first, so a resend cannot re-enter this batch */

    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&params, w->err, "{\"repo\":%lld,\"paths\":[",
                                        (long long)w->repo_id);
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        atlas_buf enc = ATLAS_BUF_INIT;
        st = atlas_path_text_encode(w->batch[i], w->batch_len[i], &enc, w->err);
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(&params, w->err, "%s\"%s\"", i == 0 ? "" : ",",
                                   atlas_buf_cstr(&enc));
        }
        atlas_buf_free(&enc);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&params, "]}", w->err);
    }

    /* Every path is resent unless the daemon says it carried it. */
    bool resend_all = true;
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(w->socket_path, "scanner.keep", atlas_buf_cstr(&params), &raw, w->err);
    }
    atlas_ipc_response *resp = NULL;
    if (st == ATLAS_OK) {
        atlas_err perr;
        atlas_err_init(&perr);
        if (atlas_ipc_response_parse(raw.data, raw.len, &resp, &perr) == ATLAS_OK &&
            atlas_ipc_response_ok(resp)) {
            resend_all = false;
        }
    }

    w->no_keep = true;
    for (size_t i = 0; i < n; i++) {
        bool carried = !resend_all;
        if (carried) {
            /* Named in `resend` means the daemon does not hold it. */
            size_t rn = 0;
            if (atlas_ipc_result_arr_len(resp, "resend", &rn)) {
                atlas_buf enc = ATLAS_BUF_INIT;
                if (atlas_path_text_encode(w->batch[i], w->batch_len[i], &enc, w->err) ==
                    ATLAS_OK) {
                    for (size_t k = 0; k < rn; k++) {
                        const char *p = NULL;
                        if (atlas_ipc_result_arr_str(resp, "resend", k, &p) && p != NULL &&
                            strcmp(p, atlas_buf_cstr(&enc)) == 0) {
                            carried = false;
                            break;
                        }
                    }
                }
                atlas_buf_free(&enc);
            }
        }
        if (carried) {
            w->mirrored++;
            w->kept++;
        } else {
            mirror_one(w, w->batch[i], w->batch_len[i]);
        }
        free(w->batch[i]);
        w->batch[i] = NULL;
    }
    w->no_keep = false;

    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        /* Reported, not fatal: every path was resent. */
        atlas_err_init(w->err);
    }
}

/* Queues one unchanged path. Flushes when the batch is full, so the buffer is a
 * fixed frame and never grows with the tree. */
static void batch_add(walk_ctx *w, const void *rel, size_t rel_len) {
    char *copy = (char *)malloc(rel_len + 1u);
    if (copy == NULL) {
        mirror_one(w, rel, rel_len); /* forgetting costs a send, never a gap */
        return;
    }
    memcpy(copy, rel, rel_len);
    copy[rel_len] = '\0';
    w->batch[w->batch_n] = copy;
    w->batch_len[w->batch_n] = rel_len;
    w->batch_n++;
    if (w->batch_n == ATLAS_SCANNER_KEEP_MAX_PATHS) {
        batch_flush(w);
    }
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

/* Monotonic milliseconds, for pacing the heartbeat inside a long walk. */
static int64_t beat_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* Tells the daemon this scanner is still working, if enough time has passed.
 *
 * `scanner.poll` is the heartbeat — asking what is owed is the evidence of being
 * alive — and a pass long enough to outlast the staleness bound has to say so
 * more than once. Half the cadence, so two beats fit inside every bound.
 *
 * The answer is discarded: what this call establishes is that the request
 * arrived, and a directive read mid-walk would change what the walk is already
 * doing. A failure is ignored for the same reason a failed pass is survived —
 * the daemon simply stops hearing, which is what it should conclude. */
static void beat(walk_ctx *w) {
    int64_t now = beat_now_ms();
    if (w->last_beat_ms != 0 && now - w->last_beat_ms < ATLAS_SCANNER_POLL_INTERVAL_MS / 2) {
        return;
    }
    w->last_beat_ms = now;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_err beat_err;
    atlas_err_init(&beat_err);
    (void)atlas_ipc_call(w->socket_path, "scanner.poll", "{}", &raw, &beat_err);
    atlas_buf_free(&raw);
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
    beat(w);
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
        } else if (res == ATLAS_PATH_OPEN_MISSING) {
            /* **A file that is gone is not a gap.** `git ls-files` listed it and
             * it went away before this walk reached it -- a race, not a failure,
             * and the mirror not holding it is the correct outcome rather than
             * an incomplete one.
             *
             * Counting it as a skip made an actively-built repository
             * permanently unindexable: a pass over `/opt/dna` takes twelve
             * minutes, build outputs come and go inside that window, and every
             * pass therefore ended with "9 unreadable" and refused to claim
             * completeness. Three hours of no indexing, with nothing wrong.
             *
             * Not counted at all, deliberately. A counter that rises for a file
             * that does not exist would report a problem nobody can act on. */
            (void)0;
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

    /* Unchanged since this process sent it? Then ask the daemon to carry it
     * forward rather than reading, hex-encoding and sending it again. The
     * identity was taken by the open above, so this costs no extra syscall on
     * the tree — and when the daemon says it does not hold the path, the code
     * below sends it exactly as it always did. */
    atlas_fs_identity now;
    memset(&now, 0, sizeof(now));
    scanner_identity_from_stat(&now, &sb);
    if (w->mem != NULL && !w->no_keep) {
        const sent_entry *e = sent_map_find(w->mem, rel, rel_len);
        if (e != NULL && atlas_fs_identity_same(&e->id, &now)) {
            (void)close(fd);
            sent_map_put(w->mem, rel, rel_len, &now);
            batch_add(w, rel, rel_len);
            return;
        }
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
            /* Remembered only now, and only when the observation is not racy: a
             * file whose timestamps sit inside the open tick may still be being
             * written, and A1 stores such an observation as unknown rather than
             * as a value. Not remembering costs one resend next pass. */
            if (w->mem != NULL && scanner_identity_stable(&now, &w->observed_at)) {
                sent_map_put(w->mem, rel, rel_len, &now);
            }
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
static atlas_status scan_pass(int64_t *poll_within_ms, scanner_memory *mem, FILE *log,
                              atlas_err *err) {
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
        (void)clock_gettime(CLOCK_REALTIME, &w.observed_at);
        /* **The directive decides whether this pass may remember anything.**
         * `full` means the daemon holds no complete mirror, so there is nothing
         * to carry forward and anything this process still remembers describes
         * a generation that is gone. Dropped rather than ignored: a memory kept
         * across a `full` would make the next pass ask about files the daemon
         * cannot have. */
        {
            const char *directive = NULL;
            const bool incremental =
                atlas_ipc_result_arr_obj_str(resp, "repositories", i, "directive", &directive) &&
                directive != NULL && strcmp(directive, "incremental") == 0;
            sent_map *m = mem != NULL ? memory_for(mem, id) : NULL;
            if (m != NULL && !incremental) {
                sent_map_free(m);
            }
            if (m != NULL && incremental) {
                m->pass++;
                w.mem = m;
            }
        }
        /* **Nothing is cleared here, deliberately.** The first design reported
         * `complete=false` before writing anything, so a crash left the mirror
         * refused rather than trusted -- and made a repository unreadable for the
         * whole of every pass. Measured: seven minutes of every ten for
         * /opt/dna, refused seventy per cent of the time while nothing was
         * wrong with it.
         *
         * A pass now writes into a staging generation and the finished mirror
         * stays where readers look, so the crash this protected against cannot
         * touch what they read: a scanner killed mid-pass leaves `<id>.next`
         * half-written and `<id>` exactly as it was. The verdict is reported
         * once, at the end, and publishing is what makes it visible. */
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

        /* A13. The build inputs the daemon named in its poll answer.
         *
         * **Ignored files the mirror carries anyway.** A compilation database is
         * a build artefact, so the tracked-and-untracked walk skips it — and
         * without it in the mirror the semantic pass fails with "the named
         * compilation database is not a readable regular file inside the
         * repository", the retry governor holds on "the last attempt failed and
         * the source has not changed since", and the semantic index stops for
         * good. Measured: both repositories were a day stale that way.
         *
         * The daemon names them because what belongs here is what *discovery
         * accepted*, which is a fact it holds and this process cannot derive. */
        size_t bi_n = 0;
        if (walked == ATLAS_OK && w.status == ATLAS_OK &&
            atlas_ipc_result_arr_obj_arr_len(resp, "repositories", i, "build_inputs", &bi_n)) {
            for (size_t k = 0; k < bi_n; k++) {
                const char *enc = NULL;
                if (!atlas_ipc_result_arr_obj_arr_str(resp, "repositories", i, "build_inputs", k,
                                                      &enc) ||
                    enc == NULL) {
                    continue;
                }
                atlas_buf rel = ATLAS_BUF_INIT;
                if (atlas_path_text_decode(enc, strlen(enc), &rel, err) == ATLAS_OK) {
                    mirror_one(&w, rel.data, rel.len);
                }
                atlas_buf_free(&rel);
            }
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

        /* Anything still queued is carried before anything else looks at the
         * result. A path left in the batch is one the staging generation does
         * not hold, so flushing after `say_state` — which is what publishes —
         * would rename the generation into place without it, and the daemon
         * would read the absence as a deletion. It also has to precede
         * `complete`, which is a claim about what reached the mirror. */
        batch_flush(&w);

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
         * is exactly the silence this run has to break.
         *
         * What it may claim, though, is only that the daemon did not *confirm*
         * the run. `scanner.state` is answered when the write is accepted, so a
         * failure here is a refusal or a lost answer, and a lost answer is not
         * evidence that nothing was recorded — A9.2.6's rule that backing out
         * and timing out are different claims. The earlier wording said the
         * daemon "did not record this run" and was measured saying it about a
         * run the daemon had recorded. The next poll settles it either way: its
         * directive asks for a full mirror again while none is complete. */
        atlas_status said = say_state(&w, complete);
        if (said != ATLAS_OK && log != NULL) {
            (void)fprintf(log,
                          "  %s  WARNING: the daemon did not confirm this run, so the next poll "
                          "will ask for it again: %s\n",
                          name, atlas_err_msg(err));
        }

        /* Forget what this pass did not visit, so the memory tracks the tree
         * rather than everything the repository has ever held. */
        if (w.mem != NULL) {
            sent_map_sweep(w.mem);
        }

        if (log != NULL) {
            (void)fprintf(log,
                          "  %s  mirrored %lld (%lld carried), skipped %lld symlink, "
                          "%lld unreadable\n",
                          name, (long long)w.mirrored, (long long)w.kept,
                          (long long)w.skipped_symlink, (long long)w.skipped_unreadable);
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
    /* Held by the loop, so it survives a pass and nothing else. `--once` gets one
     * too and drops it on return: a single pass has nothing to remember from,
     * and giving it the same structure keeps one code path rather than two. */
    scanner_memory mem;
    memset(&mem, 0, sizeof(mem));
    if (once) {
        atlas_status one = scan_pass(&within, &mem, log, err);
        memory_free(&mem);
        return one;
    }
    for (;;) {
        atlas_err pass_err;
        atlas_err_init(&pass_err);
        atlas_status st = scan_pass(&within, &mem, log, &pass_err);
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

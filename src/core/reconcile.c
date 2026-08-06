/* Atlas - incremental reconciliation of one repository.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#define _GNU_SOURCE 1

#include "atlas/reconcile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "atlas/scan.h"
#include "atlas/sha256.h"

#define HASH_CHUNK (64u * 1024u)
#define COMPILE_DB_NAME "compile_commands.json"

void atlas_reconcile_opts_init(atlas_reconcile_opts *o) {
    memset(o, 0, sizeof(*o));
    o->max_file_bytes = ATLAS_HASH_MAX_FILE_BYTES;
    o->max_untracked = ATLAS_WATCH_MAX_DISCOVER_FILES;
}

void atlas_reconcile_summary_init(atlas_reconcile_summary *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->truncated_reason);
}

void atlas_reconcile_summary_free(atlas_reconcile_summary *s) {
    if (s == NULL) {
        return;
    }
    atlas_buf_free(&s->truncated_reason);
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- racy timestamps -----------------------------------------------------
 *
 * A recorded identity is only useful if a later write is guaranteed to change
 * it. That guarantee fails for a write that lands in the same timestamp tick
 * the observation was taken in: the file changes, the timestamp does not, and
 * the identity keeps matching forever.
 *
 * The window is real. A pass starts at T, stats a file at T+delta and reads
 * mtime = M. If M is at or after T, then a write arriving after the stat can
 * still produce mtime = M, because it falls in the same tick. Everything the
 * pass then records about that file is a fact about a moment that is still
 * being written to.
 *
 * So an observation is *racy* when either timestamp is not strictly in the past
 * relative to the moment the pass began looking:
 *
 *   racy  iff  ts >= observed_at                        (nanosecond compare)
 *         or   ts.nsec == 0 && ts.sec >= observed_at.sec (coarse filesystem)
 *
 * The second clause handles filesystems that truncate to whole seconds. There
 * `ts` is always numerically below a sub-second `observed_at`, so the first
 * clause alone would call a same-second write non-racy. A zero nanosecond field
 * is the signal that the timestamp may have been truncated, and the whole second
 * is then treated as still open. A genuine zero-nanosecond timestamp on a
 * nanosecond filesystem costs one extra hash, which is the right direction to
 * be wrong in.
 *
 * `observed_at` is the pass's start, which is earlier than any stat it performs.
 * Using it rather than the individual stat time is deliberately conservative.
 *
 * A racy observation is stored as an *unknown* identity — all NULLs — rather
 * than as a value. The next pass therefore rehashes the file exactly once, and
 * by then the timestamp is safely in the past and a real identity is recorded.
 * The condition self-heals; it does not accumulate. */
static bool timestamp_is_racy(int64_t sec, int64_t nsec, const struct timespec *observed_at) {
    if (sec > (int64_t)observed_at->tv_sec) {
        return true;
    }
    if (sec == (int64_t)observed_at->tv_sec) {
        return nsec >= (int64_t)observed_at->tv_nsec || nsec == 0;
    }
    /* Strictly earlier second: only a filesystem that truncated to the second
     * could still be inside the open tick, and that cannot be the case here
     * because the second itself has passed. */
    return false;
}

/* True when neither recorded timestamp can still change without changing. */
static bool identity_is_stable(const atlas_fs_identity *id, const struct timespec *observed_at) {
    return !timestamp_is_racy(id->mtime_sec, id->mtime_nsec, observed_at) &&
           !timestamp_is_racy(id->ctime_sec, id->ctime_nsec, observed_at);
}

/* Fills an identity from a stat. Kept in one place so no call site can record a
 * partial one, which would compare unequal forever. */
static void identity_from_stat(atlas_fs_identity *out, const struct stat *sb) {
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

/* --- the dirty-path set --------------------------------------------------
 *
 * Membership is checked once per candidate, so a linear scan over the watcher's
 * list would be O(candidates x dirty). A small open-addressing set of path
 * hashes keeps it O(1); a hash collision costs one unnecessary file read, never
 * a missed one, which is the only direction this may be wrong in. */

typedef struct dirty_set {
    uint64_t *slots; /* 0 means empty */
    size_t cap;
    size_t count;
} dirty_set;

static uint64_t path_hash(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ULL; /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    /* Never 0: that value marks an empty slot. */
    return h != 0 ? h : 1;
}

static void dirty_set_free(dirty_set *s) {
    free(s->slots);
    memset(s, 0, sizeof(*s));
}

static atlas_status dirty_set_build(dirty_set *s, const char *paths, size_t len, atlas_err *err) {
    memset(s, 0, sizeof(*s));
    if (paths == NULL || len == 0) {
        return ATLAS_OK;
    }
    size_t entries = 0;
    for (size_t off = 0; off < len; off++) {
        if (paths[off] == '\0') {
            entries++;
        }
    }
    if (entries == 0) {
        return ATLAS_OK;
    }
    size_t cap = 16;
    while (cap < entries * 4u) {
        cap *= 2u;
    }
    s->slots = calloc(cap, sizeof(*s->slots));
    if (s->slots == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building the dirty-path set");
    }
    s->cap = cap;

    size_t start = 0;
    for (size_t off = 0; off < len; off++) {
        if (paths[off] != '\0') {
            continue;
        }
        size_t n = off - start;
        if (n > 0) {
            uint64_t h = path_hash(paths + start, n);
            size_t i = (size_t)(h % cap);
            for (size_t probe = 0; probe < cap; probe++) {
                uint64_t *slot = &s->slots[(i + probe) % cap];
                if (*slot == 0) {
                    *slot = h;
                    s->count++;
                    break;
                }
                if (*slot == h) {
                    break;
                }
            }
        }
        start = off + 1u;
    }
    return ATLAS_OK;
}

static bool dirty_set_has(const dirty_set *s, const void *p, size_t n) {
    if (s->cap == 0) {
        return false;
    }
    uint64_t h = path_hash(p, n);
    size_t i = (size_t)(h % s->cap);
    for (size_t probe = 0; probe < s->cap; probe++) {
        uint64_t slot = s->slots[(i + probe) % s->cap];
        if (slot == 0) {
            return false;
        }
        if (slot == h) {
            return true;
        }
    }
    return false;
}

/* --- the candidate table ------------------------------------------------
 *
 * One entry per path the pass will consider. Path and object-id bytes live in a
 * single arena so that the table itself stays a flat array of fixed-size
 * records: it is indexed by the worker pool, and an array of pointers into
 * separately allocated strings would be both larger and worse to iterate. */

typedef enum entry_outcome {
    ENTRY_PENDING = 0,
    ENTRY_OK,          /* hashed, or identity-matched */
    ENTRY_SYMLINK,     /* link text hashed; the target was never opened */
    ENTRY_MISSING,
    ENTRY_UNSAFE,      /* an intermediate path component was a symlink */
    ENTRY_NOT_REGULAR,
    ENTRY_DENIED,
    ENTRY_TOO_LARGE,   /* recorded with size and identity, content not hashed */
    ENTRY_READ_FAILED,
    ENTRY_GITLINK      /* a submodule: its contents belong to another repository */
} entry_outcome;

typedef struct recon_entry {
    size_t path_off;
    uint32_t path_len;
    uint32_t oid_off; /* into the arena; 0 means none */
    char mode[8];
    bool tracked;
    bool need_hash;
    bool dirty_forced; /* the watcher named this path; metadata cannot excuse it */
    bool racy;         /* observed inside an open timestamp tick; identity not stored */
    entry_outcome outcome;
    atlas_fs_identity fs;
    int64_t size_bytes;
    bool size_known;
    bool is_executable;
    char hash[ATLAS_SHA256_HEX_LEN + 1u];
    bool have_hash;
} recon_entry;

typedef struct recon_table {
    atlas_buf arena;
    recon_entry *items;
    size_t count;
    size_t cap;
    bool truncated;
} recon_table;

static void table_init(recon_table *t) {
    memset(t, 0, sizeof(*t));
    atlas_buf_init(&t->arena);
}

static void table_free(recon_table *t) {
    atlas_buf_free(&t->arena);
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

static atlas_status table_add(recon_table *t, const void *path, size_t path_len, const char *mode,
                              const char *oid, bool tracked, atlas_err *err) {
    if (t->count >= (size_t)ATLAS_RECONCILE_MAX_FILES) {
        t->truncated = true;
        return ATLAS_OK; /* the caller reports it; nothing is silently dropped */
    }
    if (path_len > 0xffffffffu) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "git reported an implausibly long path");
    }
    if (t->count == t->cap) {
        size_t next = t->cap == 0 ? 1024u : t->cap * 2u;
        if (next > (size_t)ATLAS_RECONCILE_MAX_FILES) {
            next = (size_t)ATLAS_RECONCILE_MAX_FILES;
        }
        recon_entry *grown = realloc(t->items, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building the scan table");
        }
        t->items = grown;
        t->cap = next;
    }

    recon_entry *e = &t->items[t->count];
    memset(e, 0, sizeof(*e));
    e->path_off = t->arena.len;
    e->path_len = (uint32_t)path_len;
    atlas_status st = atlas_buf_append(&t->arena, path, path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&t->arena, '\0', err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (oid != NULL && oid[0] != '\0') {
        if (t->arena.len > 0xffffffffu) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the scan arena grew beyond 4 GiB");
        }
        e->oid_off = (uint32_t)t->arena.len;
        st = atlas_buf_append_str(&t->arena, oid, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(&t->arena, '\0', err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (mode != NULL) {
        (void)snprintf(e->mode, sizeof(e->mode), "%s", mode);
    }
    e->tracked = tracked;
    t->count++;
    return ATLAS_OK;
}

/* Arena pointers are only valid while the arena is not being appended to. Every
 * caller of these runs after collection has finished. */
static const char *entry_path(const recon_table *t, const recon_entry *e) {
    return t->arena.data + e->path_off;
}

static const char *entry_oid(const recon_table *t, const recon_entry *e) {
    return e->oid_off != 0 ? t->arena.data + e->oid_off : NULL;
}

/* --- stage 1: observe ---------------------------------------------------- */

typedef struct collect_ctx {
    recon_table *table;
    atlas_err *err;
    atlas_status st;
    int64_t limit;
    int64_t count;
} collect_ctx;

static atlas_status on_tracked(const atlas_git_index_entry *e, void *ud, atlas_err *err) {
    collect_ctx *c = (collect_ctx *)ud;
    /* git should never hand back an absolute or dotted path. Refusing here means
     * a corrupt or hostile index cannot steer a later openat outside the tree. */
    atlas_status st = atlas_path_check_relative(e->path, e->path_len, err);
    if (st != ATLAS_OK) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "git reported an unusable tracked path: %s", atlas_err_msg(err));
    }
    /* Stage > 0 entries are the conflicting sides of an unmerged path. The path
     * appears once per stage; recording it once is enough, and stage 1 (the
     * merge base) is not what is in the working tree. */
    if (e->stage > 1) {
        return ATLAS_OK;
    }
    return table_add(c->table, e->path, e->path_len, e->mode, e->oid, true, err);
}

static atlas_status on_untracked(const void *path, size_t path_len, void *ud, atlas_err *err) {
    collect_ctx *c = (collect_ctx *)ud;
    if (c->limit > 0 && c->count >= c->limit) {
        c->table->truncated = true;
        return ATLAS_OK;
    }
    atlas_status st = atlas_path_check_relative(path, path_len, err);
    if (st != ATLAS_OK) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "git reported an unusable untracked path: %s", atlas_err_msg(err));
    }
    c->count++;
    return table_add(c->table, path, path_len, NULL, NULL, false, err);
}

static atlas_status count_ignored(const void *path, size_t path_len, void *ud, atlas_err *err) {
    (void)path;
    (void)path_len;
    (void)err;
    (*(int64_t *)ud)++;
    return ATLAS_OK;
}

/* --- stage 2 and 3: select and hash -------------------------------------- */

typedef struct hash_ctx {
    recon_table *table;
    int root_fd;
    uint64_t max_file_bytes;
    /* Read-only for the duration of the batch; workers compare against it and
     * never write it. */
    struct timespec observed_at;
} hash_ctx;

/* Streams a file through SHA-256. The content is never held in memory, and the
 * byte ceiling is enforced during the read rather than trusted from the earlier
 * lstat: a file can grow between the two. */
static bool hash_fd_bounded(int fd, uint64_t limit, char *hex_out, uint64_t *size_out,
                            bool *truncated_out) {
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    unsigned char buf[HASH_CHUNK];
    uint64_t total = 0;
    *truncated_out = false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            break;
        }
        if (limit != 0 && total + (uint64_t)n > limit) {
            *truncated_out = true;
            break;
        }
        atlas_sha256_update(&ctx, buf, (size_t)n);
        total += (uint64_t)n;
    }
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), hex_out);
    *size_out = total;
    return true;
}

/* One job. Runs on a worker thread and touches only entry `i`, the read-only
 * shared context, and the filesystem. It never touches SQLite and never creates
 * a process. */
static void hash_job(size_t i, void *ud) {
    hash_ctx *hc = (hash_ctx *)ud;
    recon_table *t = hc->table;
    recon_entry *e = &t->items[i];
    if (!e->need_hash) {
        return;
    }
    const char *rel = entry_path(t, e);

    atlas_err err;
    atlas_err_init(&err);
    atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
    int fd = -1;
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    /* Never follows a symlink, at any component. A repository that plants a
     * symlink cannot make Atlas read a file outside it. */
    if (atlas_path_open_nofollow(hc->root_fd, rel, e->path_len, &res, &fd, &sb, NULL, &err) !=
        ATLAS_OK) {
        e->outcome = ENTRY_READ_FAILED;
        return;
    }

    switch (res) {
    case ATLAS_PATH_OPEN_OK: {
        e->size_bytes = (int64_t)sb.st_size;
        e->size_known = true;
        e->is_executable = (sb.st_mode & S_IXUSR) != 0;
        identity_from_stat(&e->fs, &sb);
        /* A racy observation is recorded as *unknown*, not as a value. Storing
         * it would let a write that lands in the same still-open tick keep
         * matching it forever. Unknown costs exactly one extra read on the next
         * pass, by which time the tick has closed. */
        if (!identity_is_stable(&e->fs, &hc->observed_at)) {
            e->fs.known = false;
            e->racy = true;
        }

        if (hc->max_file_bytes != 0 && (uint64_t)sb.st_size > hc->max_file_bytes) {
            (void)close(fd);
            e->outcome = ENTRY_TOO_LARGE;
            break;
        }
        uint64_t hashed = 0;
        bool truncated = false;
        bool ok = hash_fd_bounded(fd, hc->max_file_bytes, e->hash, &hashed, &truncated);
        (void)close(fd);
        if (!ok) {
            e->outcome = ENTRY_READ_FAILED;
            break;
        }
        e->have_hash = true;
        e->size_bytes = (int64_t)hashed;
        e->outcome = truncated ? ENTRY_TOO_LARGE : ENTRY_OK;
        break;
    }
    case ATLAS_PATH_OPEN_SYMLINK: {
        /* For a symlink the content *is* the link text. Hashing the text and
         * never opening the target is what keeps a link pointing outside the
         * repository unreadable. */
        atlas_buf target = ATLAS_BUF_INIT;
        atlas_path_open_result lres = ATLAS_PATH_OPEN_MISSING;
        if (atlas_path_readlink_at(hc->root_fd, rel, e->path_len, &target, &lres, &err) ==
            ATLAS_OK) {
            atlas_sha256_hex(target.data, target.len, e->hash);
            e->have_hash = true;
            e->size_bytes = (int64_t)target.len;
            e->size_known = true;
            e->outcome = ENTRY_SYMLINK;
            identity_from_stat(&e->fs, &sb);
            if (!identity_is_stable(&e->fs, &hc->observed_at)) {
                e->fs.known = false;
                e->racy = true;
            }
        } else {
            e->outcome = ENTRY_READ_FAILED;
        }
        atlas_buf_free(&target);
        break;
    }
    case ATLAS_PATH_OPEN_UNSAFE: e->outcome = ENTRY_UNSAFE; break;
    case ATLAS_PATH_OPEN_MISSING: e->outcome = ENTRY_MISSING; break;
    case ATLAS_PATH_OPEN_NOT_REGULAR: e->outcome = ENTRY_NOT_REGULAR; break;
    case ATLAS_PATH_OPEN_DENIED:
    default: e->outcome = ENTRY_DENIED; break;
    }
}

/* Decides, without reading any content, which entries have to be read.
 *
 * This is the whole reason a pass is cheap: a file whose device, inode, size,
 * mtime (to the nanosecond) and mode all match what was recorded cannot have
 * different content, so it is not opened. An entry with no recorded identity —
 * a new file, or a row written by A0 — is always read, exactly once, after which
 * it has one. */
/* Decides, for each candidate, whether its content has to be read.
 *
 * The cache-hit rule, stated once, in order of precedence:
 *
 *   1. a gitlink is never read (its content belongs to another repository)
 *   2. a content-verifying pass reads everything else, unconditionally
 *   3. a path the watcher named is read, whatever its metadata says
 *   4. a path whose stat failed is read, so the hash stage can classify it
 *   5. otherwise, read unless the complete recorded identity — device, inode,
 *      size, mode, mtime and ctime, all present, all equal — matches, *and* the
 *      new observation is not racy
 *
 * Rules 2 and 3 exist because rule 5 is a statement about metadata, and metadata
 * can be made to lie. Rule 2 covers "we may have missed something"; rule 3
 * covers "we know something happened to this file". */
static atlas_status select_candidates(atlas_db *db, int64_t repo_id, recon_table *t, int root_fd,
                                      bool full, const dirty_set *dirty,
                                      const struct timespec *observed_at,
                                      atlas_reconcile_summary *sum, atlas_err *err) {
    for (size_t i = 0; i < t->count; i++) {
        recon_entry *e = &t->items[i];
        sum->files_examined++;

        if (strcmp(e->mode, "160000") == 0) {
            /* A submodule. Its contents belong to another repository, which has
             * to be registered separately to be indexed. */
            e->outcome = ENTRY_GITLINK;
            e->need_hash = false;
            continue;
        }
        if (full) {
            /* Content verification. No stat is consulted and no stored identity
             * is looked at, because the point of this pass is to establish the
             * truth independently of both. */
            e->need_hash = true;
            continue;
        }

        const char *rel = entry_path(t, e);
        if (dirty_set_has(dirty, rel, e->path_len)) {
            /* The watcher saw an event for this path. That is positive evidence
             * that the file was touched, and it outranks any metadata argument
             * that it was not — including a metadata tuple that has been made to
             * look unchanged. */
            e->need_hash = true;
            e->dirty_forced = true;
            sum->files_dirty_forced++;
            continue;
        }

        struct stat sb;
        atlas_path_open_result res = ATLAS_PATH_OPEN_MISSING;
        int fd = -1;
        memset(&sb, 0, sizeof(sb));
        /* The lstat is done through the same no-follow walker the hash uses, so
         * the identity that is compared is the identity of the file Atlas would
         * actually read — not of whatever a symlink points at. The fd is closed
         * immediately: this stage decides, it does not read. */
        atlas_status st =
            atlas_path_open_nofollow(root_fd, rel, e->path_len, &res, &fd, &sb, NULL, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (fd >= 0) {
            (void)close(fd);
        }
        if (res != ATLAS_PATH_OPEN_OK && res != ATLAS_PATH_OPEN_SYMLINK) {
            e->need_hash = true; /* let the hash stage classify the failure */
            continue;
        }

        atlas_fs_identity now;
        memset(&now, 0, sizeof(now));
        identity_from_stat(&now, &sb);

        atlas_fs_identity stored;
        bool found = false;
        st = atlas_db_file_identity(db, repo_id, rel, e->path_len, &stored, NULL, &found, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (found && atlas_fs_identity_same(&stored, &now) &&
            identity_is_stable(&now, observed_at)) {
            e->need_hash = false;
            e->outcome = ENTRY_OK;
            e->fs = now;
            e->size_known = true;
            e->size_bytes = now.size;
            e->is_executable = (sb.st_mode & S_IXUSR) != 0;
            sum->files_identity_hit++;
        } else {
            /* Either something differs, or the observation lands inside a
             * timestamp tick that is still open. Both mean: read the file. */
            if (found && !identity_is_stable(&now, observed_at)) {
                sum->files_racy++;
            }
            e->need_hash = true;
        }
    }
    return ATLAS_OK;
}

/* --- stage 4: apply ------------------------------------------------------ */

/* --- the working-tree change snapshot -----------------------------------
 *
 * A1 recorded the dirty *counts* the status invocation produced and threw the
 * entries away. A2 needs the entries: an MCP adapter has to answer "which paths
 * are staged" from the index, because running git inside the daemon's serve loop
 * would let one such question stall every other client for the git timeout.
 *
 * This costs no extra git invocation. `atlas_git_read_worktree_state` is already
 * `atlas_git_read_status` with a NULL callback, so passing one collects records
 * the parser was producing and discarding.
 *
 * Collected in stage 1, alongside everything else observed there, and written in
 * stage 4 — never inside a transaction that a git process is running under. */

typedef struct wt_change {
    char scope[16];
    char status;
    const char *change_type; /* a static string from atlas_git_change_type_name */
    char *path;              /* owned */
    size_t path_len;
    char *old_path; /* owned; NULL unless rename/copy */
    size_t old_path_len;
    bool is_directory;
} wt_change;

typedef struct wt_snapshot {
    wt_change *items;
    size_t count;
    size_t cap;
    bool truncated;
} wt_snapshot;

static void wt_snapshot_free(wt_snapshot *s) {
    for (size_t i = 0; i < s->count; i++) {
        free(s->items[i].path);
        free(s->items[i].old_path);
    }
    free(s->items);
    memset(s, 0, sizeof(*s));
}

/* Copies raw path bytes. Paths are bytes, so this is memcpy plus a NUL for the
 * benefit of callers that want a C string, never strdup. */
static char *dup_bytes(const void *src, size_t n) {
    char *p = malloc(n + 1u);
    if (p == NULL) {
        return NULL;
    }
    if (n > 0) {
        memcpy(p, src, n);
    }
    p[n] = '\0';
    return p;
}

static atlas_status on_status_change(const atlas_git_status_entry *e, void *ud, atlas_err *err) {
    wt_snapshot *s = (wt_snapshot *)ud;
    /* Bounded, and it says so. A repository with a hundred thousand dirty paths
     * gets a truncated snapshot with `truncated` set, never a silent prefix. */
    if (s->count >= (size_t)ATLAS_AI_MAX_CHANGED_PATHS) {
        s->truncated = true;
        return ATLAS_OK;
    }
    if (s->count == s->cap) {
        size_t cap = s->cap == 0 ? 64u : s->cap * 2u;
        wt_change *items = realloc(s->items, cap * sizeof(*items));
        if (items == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "out of memory recording the working-tree change snapshot");
        }
        s->items = items;
        s->cap = cap;
    }
    wt_change *w = &s->items[s->count];
    memset(w, 0, sizeof(*w));
    (void)snprintf(w->scope, sizeof(w->scope), "%s", atlas_change_scope_name(e->scope));
    w->status = e->status;
    w->change_type = atlas_git_change_type_name(e->status);
    w->is_directory = e->is_directory;
    w->path = dup_bytes(e->path, e->path_len);
    if (w->path == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory copying a changed path");
    }
    w->path_len = e->path_len;
    if (e->old_path != NULL && e->old_path_len > 0) {
        w->old_path = dup_bytes(e->old_path, e->old_path_len);
        if (w->old_path == NULL) {
            free(w->path);
            w->path = NULL;
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory copying a renamed path");
        }
        w->old_path_len = e->old_path_len;
    }
    s->count++;
    return ATLAS_OK;
}

/* Replaces the repository's snapshot in one bounded transaction. */
static atlas_status apply_wt_snapshot(atlas_db *db, int64_t repo_id, int64_t generation,
                                      const wt_snapshot *s, atlas_err *err) {
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_worktree_changes_clear(db, repo_id, err);
    atlas_buf path_text = ATLAS_BUF_INIT;
    atlas_buf old_text = ATLAS_BUF_INIT;
    for (size_t i = 0; st == ATLAS_OK && i < s->count; i++) {
        const wt_change *w = &s->items[i];
        atlas_buf_reset(&path_text);
        atlas_buf_reset(&old_text);
        st = atlas_path_text_encode(w->path, w->path_len, &path_text, err);
        if (st == ATLAS_OK && w->old_path != NULL) {
            st = atlas_path_text_encode(w->old_path, w->old_path_len, &old_text, err);
        }
        if (st != ATLAS_OK) {
            break;
        }
        atlas_worktree_change_record rec;
        memset(&rec, 0, sizeof(rec));
        rec.scope = w->scope;
        rec.status = w->status;
        rec.change_type = w->change_type;
        rec.path_raw = w->path;
        rec.path_raw_len = w->path_len;
        rec.path_text = atlas_buf_cstr(&path_text);
        rec.old_path_raw = w->old_path;
        rec.old_path_raw_len = w->old_path_len;
        rec.old_path_text = w->old_path != NULL ? atlas_buf_cstr(&old_text) : NULL;
        rec.is_directory = w->is_directory;
        st = atlas_db_worktree_change_insert(db, repo_id, generation, &rec, err);
    }
    atlas_buf_free(&path_text);
    atlas_buf_free(&old_text);
    if (st == ATLAS_OK) {
        st = atlas_db_commit(db, err);
    } else {
        atlas_db_rollback(db);
    }
    return st;
}

typedef struct apply_ctx {
    atlas_db *db;
    int64_t repo_id;
    int64_t scan_id;
    int64_t generation;
    atlas_reconcile_summary *sum;
    atlas_buf path_text;
    int64_t in_batch;
} apply_ctx;

/* Opens a transaction if none is open, and commits once the batch is full.
 *
 * Bounded batches are not an optimisation: an unbounded transaction would hold
 * the write lock for the length of the whole apply stage, which on a large
 * repository is long enough to make every reader see stale data and every other
 * writer time out. */
static atlas_status batch_begin(apply_ctx *ac, atlas_err *err) {
    if (ac->in_batch == 0) {
        atlas_status st = atlas_db_begin(ac->db, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    ac->in_batch++;
    return ATLAS_OK;
}

static atlas_status batch_maybe_commit(apply_ctx *ac, bool force, atlas_err *err) {
    if (ac->in_batch == 0) {
        return ATLAS_OK;
    }
    if (!force && ac->in_batch < ATLAS_DB_BATCH_MAX) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_commit(ac->db, err);
    ac->in_batch = 0;
    if (st == ATLAS_OK) {
        ac->sum->batches_written++;
    }
    return st;
}

static const char *outcome_read_error(entry_outcome o) {
    switch (o) {
    case ENTRY_MISSING: return "tracked but not present in the working tree";
    case ENTRY_UNSAFE: return "refused: a path component is a symlink";
    case ENTRY_NOT_REGULAR: return "not a regular file or symlink in the working tree";
    case ENTRY_DENIED: return "cannot be opened";
    case ENTRY_READ_FAILED: return "content could not be read";
    case ENTRY_GITLINK: return "submodule (gitlink); contents belong to another repository";
    case ENTRY_OK:
    case ENTRY_SYMLINK:
    case ENTRY_TOO_LARGE:
    case ENTRY_PENDING:
    default: return NULL;
    }
}

static const char *outcome_file_type(entry_outcome o) {
    switch (o) {
    case ENTRY_OK:
    case ENTRY_TOO_LARGE: return "regular";
    case ENTRY_SYMLINK: return "symlink";
    case ENTRY_MISSING: return "missing";
    case ENTRY_UNSAFE:
    case ENTRY_NOT_REGULAR:
    case ENTRY_DENIED:
    case ENTRY_READ_FAILED:
    case ENTRY_GITLINK:
    case ENTRY_PENDING:
    default: return "other";
    }
}

static atlas_status append_event(apply_ctx *ac, const char *kind, const recon_entry *e,
                                 const char *path_text, const char *detail, atlas_err *err) {
    atlas_event_record ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.generation = ac->generation;
    ev.path_text = path_text;
    ev.detail = detail;
    /* One event per (generation, kind, path). Replaying the same observation —
     * after a restart, or because two inotify events described one change —
     * collides on the partial unique index instead of appending a duplicate. */
    char key[512];
    if (path_text != NULL) {
        (void)snprintf(key, sizeof(key), "%s:%.400s", kind, path_text);
        ev.dedup_key = key;
    }
    if (e != NULL) {
        ev.path_raw = NULL; /* the safe text form is the durable identity here */
    }
    bool inserted = false;
    atlas_status st = atlas_db_event_append(ac->db, ac->repo_id, &ev, &inserted, err);
    if (st == ATLAS_OK && inserted) {
        ac->sum->events_appended++;
    }
    return st;
}

static atlas_status apply_entry(apply_ctx *ac, const recon_table *t, const recon_entry *e,
                                atlas_err *err) {
    const char *rel = entry_path(t, e);
    atlas_buf_reset(&ac->path_text);
    atlas_status st = atlas_path_text_encode(rel, e->path_len, &ac->path_text, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_file_record rec;
    atlas_file_record_init(&rec);
    rec.path_raw = rel;
    rec.path_raw_len = e->path_len;
    rec.path_text = atlas_buf_cstr(&ac->path_text);
    rec.path_is_utf8 = atlas_utf8_valid(rel, e->path_len);
    rec.file_type = outcome_file_type(e->outcome);
    rec.language = atlas_detect_language(rel, e->path_len);
    rec.git_mode = e->mode[0] != '\0' ? e->mode : NULL;
    rec.git_index_oid = entry_oid(t, e);
    rec.read_error = outcome_read_error(e->outcome);
    rec.is_symlink = (e->outcome == ENTRY_SYMLINK);
    rec.is_executable = e->is_executable;
    rec.unsafe_path = (e->outcome == ENTRY_UNSAFE);
    rec.size_bytes = e->size_bytes;
    rec.size_known = e->size_known;
    rec.tracked = e->tracked;
    rec.generation = ac->generation;
    /* Assigned whole. Copying the identity field by field is exactly how ctime
     * was lost between the stat and the database while every other field arrived
     * intact — the fields matched, so the file compared as unchanged forever. */
    rec.fs = e->fs;
    if (e->have_hash) {
        rec.content_hash = e->hash;
        rec.content_hash_algo = "sha256";
    }
    if (e->outcome == ENTRY_TOO_LARGE) {
        rec.truncated = true;
        rec.truncated_reason = "file exceeds the reconciliation hash ceiling; content not hashed";
        ac->sum->files_truncated++;
    }
    if (e->outcome == ENTRY_UNSAFE) {
        ac->sum->files_unsafe++;
    } else if (e->outcome == ENTRY_READ_FAILED || e->outcome == ENTRY_DENIED ||
               e->outcome == ENTRY_NOT_REGULAR) {
        ac->sum->files_unreadable++;
    }

    /* An identity hit means nothing was read, so the stored hash must be kept
     * rather than replaced with nothing. Refreshing only the liveness fields is
     * exactly what atlas_db_file_upsert's "unchanged" path does, and it gets
     * there by being handed a record that matches the stored one — which is why
     * the hash is carried forward here rather than left NULL. */
    if (!e->need_hash && !e->have_hash) {
        atlas_fs_identity ignore;
        int64_t file_id = 0;
        bool found = false;
        st = atlas_db_file_identity(ac->db, ac->repo_id, rel, e->path_len, &ignore, &file_id,
                                    &found, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (found) {
            /* The row exists and its identity matched. Touch liveness only. */
            atlas_upsert_kind kind = ATLAS_UPSERT_UNCHANGED;
            st = atlas_db_file_touch(ac->db, file_id, ac->scan_id, ac->generation, &e->fs, err);
            if (st == ATLAS_OK) {
                ac->sum->files_unchanged++;
            }
            (void)kind;
            return st;
        }
    }

    atlas_upsert_kind kind = ATLAS_UPSERT_UNCHANGED;
    st = atlas_db_file_upsert(ac->db, ac->repo_id, ac->scan_id, &rec, &kind, err);
    if (st != ATLAS_OK) {
        return st;
    }
    switch (kind) {
    case ATLAS_UPSERT_ADDED:
        ac->sum->files_added++;
        st = atlas_db_evidence_insert(ac->db, ac->repo_id, ATLAS_EV_SOURCE, ac->scan_id,
                                      rec.git_index_oid, rel, e->path_len, rec.path_text, NULL,
                                      e->tracked ? "tracked file first seen by this pass"
                                                 : "untracked file discovered by this pass",
                                      err);
        if (st == ATLAS_OK) {
            st = append_event(ac, "file_added", e, rec.path_text, NULL, err);
        }
        break;
    case ATLAS_UPSERT_MODIFIED:
        ac->sum->files_modified++;
        st = atlas_db_evidence_insert(ac->db, ac->repo_id, ATLAS_EV_SOURCE, ac->scan_id,
                                      rec.git_index_oid, rel, e->path_len, rec.path_text, NULL,
                                      "file changed since the last pass", err);
        if (st == ATLAS_OK) {
            st = append_event(ac, "file_modified", e, rec.path_text, NULL, err);
        }
        break;
    case ATLAS_UPSERT_UNCHANGED:
    default:
        ac->sum->files_unchanged++;
        break;
    }
    return st;
}

/* --- history ------------------------------------------------------------- */

typedef struct history_ctx {
    atlas_db *db;
    int64_t repo_id;
    int64_t scan_id;
    int64_t commit_id;
    bool commit_is_new;
    atlas_reconcile_summary *sum;
    atlas_buf path_text;
    atlas_buf old_path_text;
} history_ctx;

static atlas_status on_commit(const atlas_git_commit *c, void *ud, atlas_err *err) {
    history_ctx *hc = (history_ctx *)ud;
    atlas_commit_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.oid = c->oid;
    rec.parents = c->parents;
    rec.parent_count = c->parent_count;
    rec.author_name = c->author_name;
    rec.author_email = c->author_email;
    rec.author_time = c->author_time;
    rec.commit_time = c->commit_time;
    rec.subject = c->subject;
    rec.body = c->body;
    rec.body_len = c->body_len;

    bool inserted = false;
    atlas_status st =
        atlas_db_commit_upsert(hc->db, hc->repo_id, hc->scan_id, &rec, &hc->commit_id, &inserted,
                               err);
    if (st != ATLAS_OK) {
        return st;
    }
    hc->commit_is_new = inserted;
    hc->sum->commits_seen++;
    if (inserted) {
        hc->sum->commits_ingested++;
        st = atlas_db_evidence_insert(hc->db, hc->repo_id, ATLAS_EV_GIT, hc->scan_id, c->oid, NULL,
                                      0, NULL, c->oid, "commit metadata read from git log", err);
    }
    return st;
}

static atlas_status on_change(const atlas_git_commit *c, const atlas_git_change *ch, void *ud,
                              atlas_err *err) {
    history_ctx *hc = (history_ctx *)ud;
    (void)c;
    /* A commit Atlas already holds already has its changes recorded. Inserting
     * them again would duplicate rows on every pass, which is precisely the
     * unbounded growth an incremental indexer must not have. */
    if (!hc->commit_is_new || hc->commit_id == 0) {
        return ATLAS_OK;
    }
    atlas_buf_reset(&hc->path_text);
    atlas_status st = atlas_path_text_encode(ch->path, ch->path_len, &hc->path_text, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool have_old = (ch->old_path != NULL && ch->old_path_len > 0);
    if (have_old) {
        atlas_buf_reset(&hc->old_path_text);
        st = atlas_path_text_encode(ch->old_path, ch->old_path_len, &hc->old_path_text, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    atlas_change_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.change_type = atlas_git_change_type_name(ch->kind);
    rec.score = ch->score;
    rec.score_known = ch->score_known;
    rec.path_raw = ch->path;
    rec.path_raw_len = ch->path_len;
    rec.path_text = atlas_buf_cstr(&hc->path_text);
    rec.old_path_raw = have_old ? ch->old_path : NULL;
    rec.old_path_raw_len = have_old ? ch->old_path_len : 0u;
    rec.old_path_text = have_old ? atlas_buf_cstr(&hc->old_path_text) : NULL;
    rec.raw_status = ch->raw_status;

    st = atlas_db_change_insert(hc->db, hc->repo_id, hc->commit_id, &rec, err);
    if (st == ATLAS_OK) {
        hc->sum->changes_ingested++;
    }
    return st;
}

/* Ingests only what is new.
 *
 * The stored tip is what HEAD was when history was last ingested. `git log HEAD
 * --not <tip>` is then exactly the set of commits Atlas has not seen. Two things
 * invalidate that shortcut, and both are detected rather than assumed away:
 * the tip may no longer exist (garbage-collected after a rebase), and the tip
 * may still exist but no longer be an ancestor (force-push, reset, or simply a
 * different branch). Either one falls back to a full walk and says so. */
static atlas_status ingest_history(atlas_db *db, atlas_git *g, int64_t repo_id, int64_t scan_id,
                                   const atlas_git_head *head, int64_t max_commits,
                                   atlas_reconcile_summary *sum, atlas_err *err) {
    if (strcmp(head->state, "unborn") == 0) {
        return ATLAS_OK; /* nothing to walk */
    }

    /* One tip per branch, plus a distinct key for a detached HEAD so that
     * checking out a commit does not corrupt the branch's recorded position. */
    const char *ref_name = head->branch[0] != '\0' ? head->branch : "HEAD@detached";

    char tip[ATLAS_OID_HEX_MAX_INCL];
    bool have_tip = false;
    atlas_status st = atlas_db_commit_tip_get(db, repo_id, ref_name, tip, sizeof(tip), &have_tip,
                                              err);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *exclude = NULL;
    if (have_tip && tip[0] != '\0') {
        bool stale = false;
        bool unknown = false;
        st = atlas_git_tip_is_stale(g, tip, &stale, &unknown, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (unknown) {
            sum->history_full_replay = true;
        } else if (stale) {
            sum->branch_rewrite = true;
            sum->history_full_replay = true;
        } else {
            exclude = tip;
        }
    } else {
        sum->history_full_replay = true;
    }

    history_ctx hc;
    memset(&hc, 0, sizeof(hc));
    hc.db = db;
    hc.repo_id = repo_id;
    hc.scan_id = scan_id;
    hc.sum = sum;
    atlas_buf_init(&hc.path_text);
    atlas_buf_init(&hc.old_path_text);

    st = atlas_git_log_since(g, exclude, max_commits, on_commit, on_change, &hc, err);
    atlas_buf_free(&hc.path_text);
    atlas_buf_free(&hc.old_path_text);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Only recorded after the walk succeeded, so a failure part way through does
     * not convince the next pass that everything up to the new HEAD was read. */
    if (head->oid[0] != '\0') {
        st = atlas_db_commit_tip_set(db, repo_id, ref_name, head->oid, err);
    }
    return st;
}

/* --- the pass ------------------------------------------------------------ */

static atlas_status note_truncated(atlas_reconcile_summary *sum, const char *reason,
                                   atlas_err *err) {
    sum->truncated = true;
    return atlas_buf_set_str(&sum->truncated_reason, reason, err);
}

atlas_status atlas_reconcile_run(atlas_db *db, atlas_git *g, int64_t repo_id,
                                 const atlas_reconcile_opts *opts,
                                 atlas_reconcile_summary *summary, atlas_err *err) {
    atlas_reconcile_opts defaults;
    atlas_reconcile_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }
    int64_t started = monotonic_ms();

    /* The instant the pass began looking. Every timestamp at or after this is
     * treated as still-open and cannot be recorded as an identity. Captured
     * before the first observation, so it is earlier than every stat this pass
     * performs — which is the conservative direction. */
    struct timespec observed_at;
    if (clock_gettime(CLOCK_REALTIME, &observed_at) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                   "cannot read the clock; refusing to record timestamps that "
                                   "cannot be checked for raciness");
    }

    if (opts->timeout_ms > 0) {
        atlas_git_set_timeout_ms(g, opts->timeout_ms);
    } else {
        atlas_git_set_timeout_ms(g, ATLAS_DAEMON_GIT_TIMEOUT_MS);
    }
    atlas_git_set_max_output(g, ATLAS_DAEMON_GIT_MAX_OUTPUT);

    /* ---- stage 1: observe. No transaction is open. ---- */
    atlas_git_head head;
    atlas_status st = atlas_git_read_head(g, &head, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_git_worktree_state wt;
    wt_snapshot wts;
    memset(&wts, 0, sizeof(wts));
    /* The same single `git status --porcelain=v2` A1 ran, with a callback
     * attached so the per-path scope is kept rather than discarded. */
    st = atlas_git_read_status(g, &wt, on_status_change, &wts, err);
    if (st != ATLAS_OK) {
        wt_snapshot_free(&wts);
        return st;
    }
    (void)snprintf(summary->head_oid, sizeof(summary->head_oid), "%s", head.oid);
    (void)snprintf(summary->head_state, sizeof(summary->head_state), "%s", head.state);
    (void)snprintf(summary->branch, sizeof(summary->branch), "%s", head.branch);
    summary->dirty = wt.dirty;

    recon_table table;
    table_init(&table);
    dirty_set dirty;
    memset(&dirty, 0, sizeof(dirty));
    collect_ctx cc;
    memset(&cc, 0, sizeof(cc));
    cc.table = &table;

    st = atlas_git_ls_files(g, on_tracked, &cc, err);
    if (st != ATLAS_OK) {
        goto done;
    }
    if (!opts->skip_untracked) {
        cc.limit = opts->max_untracked > 0 ? opts->max_untracked : ATLAS_WATCH_MAX_DISCOVER_FILES;
        cc.count = 0;
        st = atlas_git_ls_untracked(g, on_untracked, &cc, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        summary->untracked_discovered = cc.count;
        /* What git's ignore rules covered, counted separately so "skipped
         * because ignored" is distinguishable from "skipped because a ceiling
         * was reached". Directories are collapsed, so this is a count of ignored
         * roots rather than of ignored files. */
        st = atlas_git_ls_ignored(g, count_ignored, &summary->ignored_paths, err);
        if (st != ATLAS_OK) {
            goto done;
        }
    }
    if (table.truncated) {
        st = note_truncated(summary,
                            "the repository holds more candidate paths than one reconciliation "
                            "pass will consider; the index is incomplete",
                            err);
        if (st != ATLAS_OK) {
            goto done;
        }
    }

    /* ---- stage 2: select. Reads the database, opens no transaction. ---- */
    st = dirty_set_build(&dirty, opts->dirty_paths, opts->dirty_paths_len, err);
    if (st != ATLAS_OK) {
        goto done;
    }
    st = select_candidates(db, repo_id, &table, atlas_git_root_fd(g), opts->full, &dirty,
                           &observed_at, summary, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    /* ---- stage 3: hash. Parallel, and still no transaction. ---- */
    {
        hash_ctx hc;
        memset(&hc, 0, sizeof(hc));
        hc.table = &table;
        hc.root_fd = atlas_git_root_fd(g);
        hc.max_file_bytes =
            opts->max_file_bytes != 0 ? opts->max_file_bytes : ATLAS_HASH_MAX_FILE_BYTES;
        hc.observed_at = observed_at;
        st = atlas_workers_for_each(opts->workers, table.count, hash_job, &hc, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        for (size_t i = 0; i < table.count; i++) {
            if (table.items[i].need_hash) {
                summary->files_hashed++;
            }
            if (table.items[i].racy) {
                summary->files_racy++;
            }
        }
        /* What this pass can honestly claim. A pass "verified content" only if it
         * read every eligible file: no identity was trusted, and nothing was
         * dropped by a ceiling. Derived from what happened, not from the request,
         * because that is what an event gap needs before it may be cleared. */
        summary->content_verified =
            opts->full && !summary->truncated && summary->files_identity_hit == 0;
    }

    /* ---- the staleness check ----
     *
     * Everything above describes the repository as it was when stage 1 ran. If
     * HEAD has moved since — a branch switch, a commit, a reset — then the
     * tracked file list, the hashes and the history tip all describe a state
     * that no longer exists. Committing them would leave the index describing a
     * mixture of two branches, which is worse than being briefly out of date.
     * Abandon instead, and let the caller run another pass. */
    {
        atlas_git_head now;
        st = atlas_git_read_head(g, &now, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        if (strcmp(now.oid, head.oid) != 0 || strcmp(now.branch, head.branch) != 0 ||
            strcmp(now.state, head.state) != 0) {
            summary->published = false;
            summary->duration_ms = monotonic_ms() - started;
            st = ATLAS_OK;
            goto done;
        }
    }

    /* ---- stage 4: apply, in bounded transactions ---- */
    {
        int64_t generation = 0;
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            st = atlas_db_generation_begin(db, repo_id, &generation, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_commit(db, err);
        } else {
            atlas_db_rollback(db);
        }
        if (st != ATLAS_OK) {
            goto done;
        }
        summary->generation = generation;

        atlas_scan_state state;
        memset(&state, 0, sizeof(state));
        state.head_oid = head.oid;
        state.head_state = head.state;
        state.branch = head.branch;
        state.object_format = atlas_git_object_format(g);
        state.dirty = wt.dirty;
        state.dirty_staged = wt.staged;
        state.dirty_unstaged = wt.unstaged;
        state.dirty_untracked = wt.untracked;
        state.dirty_unmerged = wt.unmerged;

        apply_ctx ac;
        memset(&ac, 0, sizeof(ac));
        ac.db = db;
        ac.repo_id = repo_id;
        ac.generation = generation;
        ac.sum = summary;
        atlas_buf_init(&ac.path_text);

        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            st = atlas_db_scan_begin(db, repo_id, &state, &ac.scan_id, err);
            if (st == ATLAS_OK) {
                st = atlas_db_commit(db, err);
            } else {
                atlas_db_rollback(db);
            }
        }
        if (st != ATLAS_OK) {
            atlas_buf_free(&ac.path_text);
            goto done;
        }

        for (size_t i = 0; i < table.count && st == ATLAS_OK; i++) {
            st = batch_begin(&ac, err);
            if (st != ATLAS_OK) {
                break;
            }
            st = apply_entry(&ac, &table, &table.items[i], err);
            if (st != ATLAS_OK) {
                break;
            }
            st = batch_maybe_commit(&ac, false, err);
        }
        if (st == ATLAS_OK) {
            st = batch_maybe_commit(&ac, true, err);
        }
        if (st != ATLAS_OK) {
            atlas_db_rollback(db);
            atlas_buf_free(&ac.path_text);
            goto done;
        }

        /* Deletions, history and the published state, each in its own bounded
         * transaction. Git runs between them, never inside one. */
        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            st = atlas_db_files_mark_deleted(db, repo_id, ac.scan_id, &summary->files_deleted, err);
            if (st == ATLAS_OK) {
                st = atlas_db_commit(db, err);
            } else {
                atlas_db_rollback(db);
            }
        }
        atlas_buf_free(&ac.path_text);
        if (st != ATLAS_OK) {
            goto done;
        }

        /* The working-tree change snapshot, in its own bounded transaction. It
         * is derived from the same observation as the dirty counts written
         * above, so the two always describe the same instant. */
        st = apply_wt_snapshot(db, repo_id, generation, &wts, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        if (wts.truncated && !summary->truncated) {
            summary->truncated = true;
            st = atlas_buf_set_str(&summary->truncated_reason,
                                   "more than the per-repository ceiling of working-tree changes "
                                   "were observed; the change snapshot is partial",
                                   err);
            if (st != ATLAS_OK) {
                goto done;
            }
        }

        if (!opts->skip_history) {
            /* History ingestion issues git commands, so it manages its own
             * bounded transaction around the writes rather than running inside
             * one opened here. */
            st = atlas_db_begin(db, err);
            if (st == ATLAS_OK) {
                st = ingest_history(db, g, repo_id, ac.scan_id, &head, opts->max_commits, summary,
                                    err);
                if (st == ATLAS_OK) {
                    st = atlas_db_commit(db, err);
                } else {
                    atlas_db_rollback(db);
                }
            }
            if (st != ATLAS_OK) {
                goto done;
            }
        }

        st = atlas_db_begin(db, err);
        if (st == ATLAS_OK) {
            st = atlas_db_scan_finish(db, repo_id, ac.scan_id, "ok", NULL, summary->files_examined,
                                      summary->files_added, summary->files_modified,
                                      summary->files_deleted, summary->files_unchanged,
                                      summary->files_unreadable, summary->commits_ingested, err);
            if (st == ATLAS_OK) {
                st = atlas_db_repo_apply_scan(db, repo_id, ac.scan_id, &state, err);
            }
            if (st == ATLAS_OK && summary->branch_rewrite) {
                atlas_event_record ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = "branch_rewrite";
                ev.generation = generation;
                ev.detail = "the previously ingested tip is no longer reachable from HEAD; "
                            "history was replayed in full";
                st = atlas_db_event_append(db, repo_id, &ev, NULL, err);
            }
            if (st == ATLAS_OK) {
                atlas_event_record ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = "reconciled";
                ev.generation = generation;
                ev.detail = summary->truncated ? atlas_buf_cstr(&summary->truncated_reason) : NULL;
                st = atlas_db_event_append(db, repo_id, &ev, NULL, err);
            }
            if (st == ATLAS_OK) {
                /* Only a pass that actually read the bytes may clear a gap. That
                 * is `content_verified`, computed above from what the pass did —
                 * not from `opts->full`, which is only what was asked for. An
                 * incremental pass, or a full one that hit a ceiling, leaves the
                 * gap exactly where it found it. */
                st = atlas_db_generation_complete(db, repo_id, generation,
                                                  summary->content_verified, opts->sync_seq, err);
            }
            if (st == ATLAS_OK) {
                int64_t removed = 0;
                st = atlas_db_events_prune(db, repo_id, ATLAS_EVENTS_RETAIN_PER_REPO, &removed, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_db_commit(db, err);
                if (st == ATLAS_OK) {
                    summary->batches_written++;
                }
            } else {
                atlas_db_rollback(db);
            }
        }
        if (st != ATLAS_OK) {
            goto done;
        }
        summary->published = true;
    }

    summary->duration_ms = monotonic_ms() - started;

done:
    wt_snapshot_free(&wts);
    dirty_set_free(&dirty);
    table_free(&table);
    return st;
}

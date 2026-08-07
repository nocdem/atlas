/* Atlas - the inotify watcher.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * What this thread is for, and what it deliberately is not:
 *
 * It decides *when* a repository should be reconciled, and it keeps its own
 * watch set accurate as directories appear and disappear. It does not decide
 * what the index should contain. Every fact still comes from git and from the
 * filesystem, read by the reconciliation pass; an inotify event is a hint that
 * something may have changed, never evidence of what changed.
 *
 * That split is what makes the watcher's failure modes survivable. inotify can
 * drop events (IN_Q_OVERFLOW), can run out of watches (ENOSPC), and cannot see
 * changes made while the daemon was not running. None of those can corrupt the
 * index, because the index is never derived from events. What they *can* do is
 * make Atlas believe it is up to date when it is not — so each one sets an event
 * gap, and while a gap is set nothing may describe the index as current until a
 * full pass has actually looked at everything.
 *
 * Watches are installed on:
 *   - the worktree, recursively, excluding .git and excluding directories git's
 *     own ignore rules cover (an ignored build tree would otherwise consume the
 *     entire watch budget for changes Atlas will not index anyway)
 *   - the worktree's own git directory, for HEAD and the index
 *   - the common git directory and refs/, so a branch update in one worktree is
 *     seen by every worktree that shares the object store
 *
 * .git itself is watched for metadata but never indexed as source.
 */
#define _GNU_SOURCE 1

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/git.h"
#include "atlas/safetext.h"
#include "daemon/daemon_internal.h"

/* The events Atlas cares about.
 *
 * IN_DONT_FOLLOW: never place a watch by traversing a symlink. IN_EXCL_UNLINK:
 * stop reporting on a file once it has been unlinked, so a deleted-but-open file
 * does not generate events forever. IN_ONLYDIR on directory watches, so a race
 * that replaces a directory with a file cannot make Atlas watch the file. */
#define ATLAS_INOTIFY_MASK                                                                     \
    (IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |      \
     IN_MOVED_TO | IN_MOVE_SELF | IN_ATTRIB | IN_DONT_FOLLOW | IN_EXCL_UNLINK | IN_ONLYDIR)

/* --- watch descriptor map ------------------------------------------------
 *
 * A linear scan would be O(watches) per event, and a burst delivers thousands of
 * events against thousands of watches. Open addressing on the wd keeps it O(1);
 * wds are small dense integers, so the hash is trivially good. */

typedef struct wd_slot {
    int wd; /* 0 = empty, -1 = tombstone */
    int64_t repo_id;
    atlas_buf path; /* absolute path of the watched directory */
    bool is_meta;   /* a git metadata directory, not indexable source */
} wd_slot;

typedef struct wd_map {
    wd_slot *slots;
    size_t cap;
    size_t count;
} wd_map;

static atlas_status wd_map_init(wd_map *m, size_t cap, atlas_err *err) {
    memset(m, 0, sizeof(*m));
    m->slots = calloc(cap, sizeof(*m->slots));
    if (m->slots == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory building the watch map");
    }
    m->cap = cap;
    for (size_t i = 0; i < cap; i++) {
        atlas_buf_init(&m->slots[i].path);
    }
    return ATLAS_OK;
}

static void wd_map_free(wd_map *m) {
    if (m->slots == NULL) {
        return;
    }
    for (size_t i = 0; i < m->cap; i++) {
        atlas_buf_free(&m->slots[i].path);
    }
    free(m->slots);
    memset(m, 0, sizeof(*m));
}

static size_t wd_hash(int wd, size_t cap) {
    return (size_t)((uint32_t)wd * 2654435761u) % cap;
}

static wd_slot *wd_map_find(wd_map *m, int wd) {
    if (m->cap == 0) {
        return NULL;
    }
    size_t i = wd_hash(wd, m->cap);
    for (size_t probe = 0; probe < m->cap; probe++) {
        wd_slot *s = &m->slots[(i + probe) % m->cap];
        if (s->wd == 0) {
            return NULL; /* an empty slot ends the probe chain */
        }
        if (s->wd == wd) {
            return s;
        }
    }
    return NULL;
}

static atlas_status wd_map_put(wd_map *m, int wd, int64_t repo_id, const char *path, bool is_meta,
                               atlas_err *err) {
    if (m->count + 1u > m->cap / 2u) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the watch map is full");
    }
    size_t i = wd_hash(wd, m->cap);
    for (size_t probe = 0; probe < m->cap; probe++) {
        wd_slot *s = &m->slots[(i + probe) % m->cap];
        if (s->wd == wd) {
            s->repo_id = repo_id;
            s->is_meta = is_meta;
            return atlas_buf_set_str(&s->path, path, err);
        }
        if (s->wd == 0 || s->wd == -1) {
            s->wd = wd;
            s->repo_id = repo_id;
            s->is_meta = is_meta;
            m->count++;
            return atlas_buf_set_str(&s->path, path, err);
        }
    }
    return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the watch map is full");
}

static void wd_map_remove(wd_map *m, int wd) {
    wd_slot *s = wd_map_find(m, wd);
    if (s == NULL) {
        return;
    }
    /* Tombstone rather than clear: clearing would break the probe chain of any
     * other key that hashed to this slot. */
    s->wd = -1;
    s->repo_id = 0;
    s->is_meta = false;
    atlas_buf_reset(&s->path);
    m->count--;
}

/* --- per-repository watch state ------------------------------------------ */

typedef struct repo_watch {
    int64_t repo_id;
    atlas_buf name;
    atlas_buf root;
    atlas_buf git_dir;
    atlas_buf common_dir;
    int64_t dirs;
    bool degraded;
    atlas_buf degraded_detail;

    /* Debounce. `first_dirty_ms` bounds how long continued activity can defer a
     * pass; without it, a process writing continuously would defer indexing for
     * as long as it kept writing. */
    bool dirty;
    int64_t first_dirty_ms;
    int64_t last_event_ms;
    int64_t last_submit_ms;

    /* Repository-relative paths seen since the last submission, NUL separated.
     *
     * These are handed to the reconciliation pass, which hashes each of them
     * whatever its metadata says. That override matters because the metadata can
     * be made to look unchanged — a same-length write with the mtime restored —
     * and the event is the one piece of evidence a writer cannot forge away.
     *
     * Bounded. Past the ceiling the watcher stops naming paths and asks for a
     * full content verification instead: it can no longer enumerate what
     * changed, and saying so is better than naming a subset. */
    atlas_buf dirty_paths;
    size_t dirty_count;
    bool dirty_overflow;
} repo_watch;

/* --- pending renames ----------------------------------------------------- */

typedef struct pending_move {
    uint32_t cookie;
    int64_t repo_id;
    int64_t at_ms;
    atlas_buf path;
    bool is_dir;
    bool used;
} pending_move;

struct atlas_watcher {
    pthread_t thread;
    bool thread_started;
    atomic_bool stop;

    int inotify_fd;
    int wake_fd[2]; /* a self-pipe, so stopping does not wait for a timeout */

    wd_map map;
    repo_watch *repos;
    size_t repo_count;
    size_t repo_cap;

    pending_move moves[ATLAS_WATCH_MAX_PENDING_MOVES];
    size_t move_count;

    atlas_writer *writer;
    FILE *log;
    atlas_buf db_path;
    atlas_db *db; /* read-only, owned by this thread */
    int reconcile_interval_ms;

    pthread_mutex_t stat_lock;
    int64_t watch_count; /* guarded by stat_lock */
    bool primed;         /* guarded by stat_lock */
};

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static repo_watch *find_repo(atlas_watcher *w, int64_t repo_id) {
    for (size_t i = 0; i < w->repo_count; i++) {
        if (w->repos[i].repo_id == repo_id) {
            return &w->repos[i];
        }
    }
    return NULL;
}

static void repo_watch_free(repo_watch *r) {
    atlas_buf_free(&r->name);
    atlas_buf_free(&r->root);
    atlas_buf_free(&r->git_dir);
    atlas_buf_free(&r->common_dir);
    atlas_buf_free(&r->degraded_detail);
    atlas_buf_free(&r->dirty_paths);
}

/* Records a repository-relative path the watcher saw an event for.
 *
 * `abs_path` must lie under the repository root; anything else — a git metadata
 * path, or a path from another repository — is ignored here, because only paths
 * the index can hold are useful to name. */
static void note_dirty_path(repo_watch *rw, const char *abs_path, size_t abs_len) {
    if (rw->dirty_overflow) {
        return; /* already asking for a full pass */
    }
    if (abs_len <= rw->root.len + 1u || memcmp(abs_path, rw->root.data, rw->root.len) != 0 ||
        abs_path[rw->root.len] != '/') {
        return;
    }
    const char *rel = abs_path + rw->root.len + 1u;
    size_t rel_len = abs_len - rw->root.len - 1u;

    if (rw->dirty_count >= ATLAS_WATCH_MAX_DIRTY_PATHS ||
        rw->dirty_paths.len + rel_len + 1u > ATLAS_WATCH_MAX_DIRTY_BYTES) {
        rw->dirty_overflow = true;
        atlas_buf_reset(&rw->dirty_paths);
        rw->dirty_count = 0;
        return;
    }
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_buf_append(&rw->dirty_paths, rel, rel_len, &err) != ATLAS_OK ||
        atlas_buf_append_ch(&rw->dirty_paths, '\0', &err) != ATLAS_OK) {
        /* Out of memory naming paths. Degrade to a full pass rather than to a
         * partial list, which would be indistinguishable from "these are all the
         * paths that changed". */
        rw->dirty_overflow = true;
        atlas_buf_reset(&rw->dirty_paths);
        rw->dirty_count = 0;
        return;
    }
    rw->dirty_count++;
}

static void clear_dirty_paths(repo_watch *rw) {
    atlas_buf_reset(&rw->dirty_paths);
    rw->dirty_count = 0;
    rw->dirty_overflow = false;
}

static void clear_repos(atlas_watcher *w) {
    for (size_t i = 0; i < w->repo_count; i++) {
        repo_watch_free(&w->repos[i]);
    }
    w->repo_count = 0;
}

/* --- ignored-directory set ----------------------------------------------
 *
 * Watching an ignored build tree would consume the watch budget for changes
 * Atlas will not index. git already knows which directories those are, and using
 * its answer means there is no second implementation of ignore semantics to
 * drift from the first. */

typedef struct ignore_set {
    atlas_buf bytes; /* NUL-separated relative directory paths, each with a trailing '/' */
    size_t count;
} ignore_set;

static atlas_status collect_ignored(const void *path, size_t path_len, void *ud, atlas_err *err) {
    ignore_set *set = (ignore_set *)ud;
    if (path_len == 0 || ((const char *)path)[path_len - 1u] != '/') {
        return ATLAS_OK; /* an ignored file, not a directory: watches are per-directory */
    }
    if (set->count >= ATLAS_WATCH_MAX_DISCOVER_DIRS) {
        return ATLAS_OK; /* bounded; the surplus is simply watched, not skipped */
    }
    atlas_status st = atlas_buf_append(&set->bytes, path, path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&set->bytes, '\0', err);
    }
    if (st == ATLAS_OK) {
        set->count++;
    }
    return st;
}

static bool is_ignored_dir(const ignore_set *set, const char *rel, size_t rel_len) {
    size_t off = 0;
    while (off < set->bytes.len) {
        const char *entry = set->bytes.data + off;
        size_t elen = strlen(entry);
        /* An entry is "dir/"; a path matches when it is that directory or lives
         * under it. */
        if (elen > 0 && rel_len + 1u >= elen && memcmp(rel, entry, elen - 1u) == 0 &&
            (rel_len == elen - 1u || rel[elen - 1u] == '/')) {
            return true;
        }
        off += elen + 1u;
    }
    return false;
}

/* --- installing watches --------------------------------------------------- */

typedef struct add_ctx {
    atlas_watcher *w;
    repo_watch *rw;
    const ignore_set *ignored;
    size_t root_len; /* prefix length to strip when forming a relative path */
    bool is_meta;
    bool limit_hit;
    bool budget_hit;
} add_ctx;

static atlas_status add_watch(add_ctx *ac, const char *abs_path, atlas_err *err) {
    atlas_watcher *w = ac->w;
    if (w->map.count + 1u >= ATLAS_WATCH_MAX_DIRS) {
        ac->budget_hit = true;
        return ATLAS_OK;
    }
    int wd = inotify_add_watch(w->inotify_fd, abs_path, ATLAS_INOTIFY_MASK);
    if (wd < 0) {
        if (errno == ENOSPC) {
            /* The kernel's per-user watch limit. This is the single most common
             * way a watcher silently stops seeing changes, so it is a reported
             * degraded state rather than a warning nobody reads. */
            ac->limit_hit = true;
            return ATLAS_OK;
        }
        if (errno == ENOENT || errno == ENOTDIR || errno == EACCES || errno == ELOOP) {
            return ATLAS_OK; /* raced away, or not ours to watch */
        }
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot watch a directory");
    }
    atlas_status st = wd_map_put(&w->map, wd, ac->rw->repo_id, abs_path, ac->is_meta, err);
    if (st == ATLAS_OK) {
        ac->rw->dirs++;
    }
    return st;
}

/* Recursively installs watches under `abs_path`.
 *
 * Iterative, with an explicit bounded stack: recursing on a directory tree whose
 * depth is controlled by the repository is a stack-exhaustion primitive. Never
 * follows a symlink, so a link pointing outside the repository cannot pull the
 * watcher out of it. */
static atlas_status add_watch_tree(add_ctx *ac, const char *abs_root, atlas_err *err) {
    atlas_buf stack = ATLAS_BUF_INIT; /* NUL-separated absolute paths still to visit */
    /* The directory currently being read is copied out of the stack before it is
     * used. It must be: appending a child re-allocates the stack, and a pointer
     * into it taken beforehand would dangle. Reading through that pointer walked
     * a truncated tree — most of a repository silently unwatched — which is the
     * kind of failure that looks like "the watcher is a bit slow" rather than
     * like a bug. */
    atlas_buf current = ATLAS_BUF_INIT;
    size_t pending = 0;
    size_t visited = 0;

    atlas_status st = atlas_buf_append_str(&stack, abs_root, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&stack, '\0', err);
        pending = 1;
    }
    size_t cursor = 0;
    while (st == ATLAS_OK && pending > 0) {
        size_t dlen = strlen(stack.data + cursor);
        st = atlas_buf_set(&current, stack.data + cursor, dlen, err);
        if (st != ATLAS_OK) {
            break;
        }
        const char *dir = atlas_buf_cstr(&current);
        cursor += dlen + 1u;
        pending--;

        if (++visited > ATLAS_WATCH_MAX_DISCOVER_DIRS) {
            ac->budget_hit = true;
            break;
        }
        st = add_watch(ac, dir, err);
        if (st != ATLAS_OK || ac->limit_hit || ac->budget_hit) {
            break;
        }

        DIR *d = opendir(dir);
        if (d == NULL) {
            continue; /* raced away or unreadable; the pass will notice */
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            /* .git holds metadata, watched separately and never indexed as
             * source. Descending into it here would both waste watches and blur
             * that distinction. */
            if (!ac->is_meta && strcmp(e->d_name, ".git") == 0) {
                continue;
            }
            atlas_buf child = ATLAS_BUF_INIT;
            atlas_status cst = atlas_buf_set_str(&child, dir, err);
            if (cst == ATLAS_OK) {
                cst = atlas_buf_append_ch(&child, '/', err);
            }
            if (cst == ATLAS_OK) {
                cst = atlas_buf_append_str(&child, e->d_name, err);
            }
            if (cst != ATLAS_OK) {
                atlas_buf_free(&child);
                st = cst;
                break;
            }
            /* lstat, so a symlink to a directory is not descended into. */
            struct stat sb;
            if (lstat(atlas_buf_cstr(&child), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
                atlas_buf_free(&child);
                continue;
            }
            if (ac->ignored != NULL && child.len > ac->root_len + 1u) {
                const char *rel = atlas_buf_cstr(&child) + ac->root_len + 1u;
                if (is_ignored_dir(ac->ignored, rel, child.len - ac->root_len - 1u)) {
                    atlas_buf_free(&child);
                    continue;
                }
            }
            st = atlas_buf_append(&stack, child.data, child.len, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_append_ch(&stack, '\0', err);
                pending++;
            }
            atlas_buf_free(&child);
            if (st != ATLAS_OK) {
                break;
            }
        }
        (void)closedir(d);
    }
    atlas_buf_free(&stack);
    atlas_buf_free(&current);
    return st;
}

/* Removes every watch whose path is `prefix` or lives under it. */
static void remove_watch_tree(atlas_watcher *w, const char *prefix) {
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < w->map.cap; i++) {
        wd_slot *s = &w->map.slots[i];
        if (s->wd <= 0) {
            continue;
        }
        const char *p = atlas_buf_cstr(&s->path);
        size_t len = s->path.len;
        if (len < plen || memcmp(p, prefix, plen) != 0) {
            continue;
        }
        if (len != plen && p[plen] != '/') {
            continue;
        }
        (void)inotify_rm_watch(w->inotify_fd, s->wd);
        repo_watch *rw = find_repo(w, s->repo_id);
        if (rw != NULL && rw->dirs > 0) {
            rw->dirs--;
        }
        wd_map_remove(&w->map, s->wd);
    }
}

/* --- building the watch set ---------------------------------------------- */

typedef struct build_ctx {
    atlas_watcher *w;
    atlas_status st;
    atlas_err *err;
} build_ctx;

static atlas_status add_repo(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    build_ctx *bc = (build_ctx *)ud;
    atlas_watcher *w = bc->w;

    if (w->repo_count == w->repo_cap) {
        size_t next = w->repo_cap == 0 ? 8u : w->repo_cap * 2u;
        repo_watch *grown = realloc(w->repos, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory tracking repositories");
        }
        w->repos = grown;
        w->repo_cap = next;
    }
    repo_watch *rw = &w->repos[w->repo_count];
    memset(rw, 0, sizeof(*rw));
    atlas_buf_init(&rw->name);
    atlas_buf_init(&rw->root);
    atlas_buf_init(&rw->git_dir);
    atlas_buf_init(&rw->common_dir);
    atlas_buf_init(&rw->degraded_detail);
    atlas_buf_init(&rw->dirty_paths);
    rw->repo_id = ri->id;
    atlas_status st = atlas_buf_set_str(&rw->name, ri->name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&rw->root, ri->root_path.data, ri->root_path.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&rw->git_dir, ri->git_dir.data, ri->git_dir.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&rw->common_dir, ri->git_common_dir.data, ri->git_common_dir.len, err);
    }
    if (st != ATLAS_OK) {
        repo_watch_free(rw);
        return st;
    }
    w->repo_count++;
    return ATLAS_OK;
}

/* Installs every watch for one repository and records the resulting state. */
static void watch_repository(atlas_watcher *w, repo_watch *rw) {
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);
    atlas_err err;
    atlas_err_init(&err);

    ignore_set ignored;
    memset(&ignored, 0, sizeof(ignored));
    atlas_buf_init(&ignored.bytes);

    /* Ask git which directories to skip. A failure here is not fatal: watching
     * an ignored tree wastes watches but indexes nothing wrong, so the watcher
     * carries on with an empty skip set. */
    atlas_git *g = NULL;
    if (atlas_git_open(atlas_buf_cstr(&rw->root), &g, &err) == ATLAS_OK) {
        atlas_err ignore_err;
        atlas_err_init(&ignore_err);
        (void)atlas_git_ls_ignored(g, collect_ignored, &ignored, &ignore_err);
        atlas_git_close(g);
    }

    add_ctx ac;
    memset(&ac, 0, sizeof(ac));
    ac.w = w;
    ac.rw = rw;
    ac.ignored = &ignored;
    ac.root_len = rw->root.len;

    atlas_err add_err;
    atlas_err_init(&add_err);
    atlas_status st = add_watch_tree(&ac, atlas_buf_cstr(&rw->root), &add_err);

    /* Git metadata: this worktree's git dir and the shared common dir. HEAD, the
     * index and refs all live in one of the two, and a linked worktree's HEAD is
     * in its own git dir while the branch it points at is in the common one. */
    if (st == ATLAS_OK) {
        ac.is_meta = true;
        ac.ignored = NULL;
        if (rw->git_dir.len > 0) {
            st = add_watch(&ac, atlas_buf_cstr(&rw->git_dir), &add_err);
        }
        if (st == ATLAS_OK && rw->common_dir.len > 0 &&
            (rw->common_dir.len != rw->git_dir.len ||
             memcmp(rw->common_dir.data, rw->git_dir.data, rw->common_dir.len) != 0)) {
            st = add_watch(&ac, atlas_buf_cstr(&rw->common_dir), &add_err);
        }
        if (st == ATLAS_OK && rw->common_dir.len > 0) {
            atlas_buf refs = ATLAS_BUF_INIT;
            if (atlas_buf_set(&refs, rw->common_dir.data, rw->common_dir.len, &add_err) ==
                    ATLAS_OK &&
                atlas_buf_append_str(&refs, "/refs", &add_err) == ATLAS_OK) {
                st = add_watch_tree(&ac, atlas_buf_cstr(&refs), &add_err);
            }
            atlas_buf_free(&refs);
        }
    }

    rw->degraded = false;
    atlas_buf_reset(&rw->degraded_detail);
    if (st != ATLAS_OK) {
        rw->degraded = true;
        (void)atlas_buf_set_str(&rw->degraded_detail, atlas_err_msg(&add_err), &err);
    } else if (ac.limit_hit) {
        rw->degraded = true;
        (void)atlas_buf_set_str(
            &rw->degraded_detail,
            "the kernel's inotify watch limit was reached, so some directories are not being "
            "observed. Raise fs.inotify.max_user_watches, or expect to rely on periodic "
            "reconciliation for the unwatched parts.",
            &err);
    } else if (ac.budget_hit) {
        rw->degraded = true;
        (void)atlas_buf_set_str(&rw->degraded_detail,
                                "this repository has more directories than Atlas will watch, so "
                                "some are not being observed and are covered only by periodic "
                                "reconciliation",
                                &err);
    }

    if (rw->degraded) {
        atlas_daemon_log(w->log, "warn", "watcher degraded for %s: %s",
                         atlas_safe(&safe, atlas_buf_cstr(&rw->name)),
                         atlas_safe(&safe, atlas_buf_cstr(&rw->degraded_detail)));
        /* A degraded watcher may miss changes, which is exactly an event gap:
         * the index must not be described as current until a full pass runs. */
        (void)atlas_writer_submit_gap(w->writer, rw->repo_id,
                                      atlas_buf_cstr(&rw->degraded_detail), &err);
    } else {
        atlas_daemon_log(w->log, "info", "watching %s (%lld directories)",
                         atlas_safe(&safe, atlas_buf_cstr(&rw->name)), (long long)rw->dirs);
        (void)atlas_writer_submit_watch_state(w->writer, rw->repo_id, ATLAS_WATCH_WATCHING, NULL,
                                              rw->dirs, &err);
    }

    atlas_buf_free(&ignored.bytes);
    atlas_safe_pool_free(&safe);
}

/* Rebuilds the whole watch set. Called at startup and whenever the repository
 * set changes, so `repo add` takes effect without restarting the daemon. */
static atlas_status rebuild_watches(atlas_watcher *w, atlas_err *err) {
    /* Drop every existing watch first. Rebuilding from scratch is O(watches) and
     * happens only on a repository-set change; getting incremental watch
     * bookkeeping subtly wrong would be far more expensive than that. */
    for (size_t i = 0; i < w->map.cap; i++) {
        if (w->map.slots[i].wd > 0) {
            (void)inotify_rm_watch(w->inotify_fd, w->map.slots[i].wd);
            wd_map_remove(&w->map, w->map.slots[i].wd);
        }
    }
    clear_repos(w);

    build_ctx bc = {w, ATLAS_OK, err};
    atlas_status st = atlas_db_repo_list(w->db, add_repo, &bc, err);
    if (st != ATLAS_OK) {
        return st;
    }
    for (size_t i = 0; i < w->repo_count; i++) {
        watch_repository(w, &w->repos[i]);
    }
    (void)pthread_mutex_lock(&w->stat_lock);
    w->watch_count = (int64_t)w->map.count;
    (void)pthread_mutex_unlock(&w->stat_lock);
    return ATLAS_OK;
}

/* --- event handling ------------------------------------------------------ */

static void mark_dirty(atlas_watcher *w, int64_t repo_id) {
    repo_watch *rw = find_repo(w, repo_id);
    if (rw == NULL) {
        return;
    }
    int64_t t = now_ms();
    if (!rw->dirty) {
        rw->dirty = true;
        rw->first_dirty_ms = t;
    }
    rw->last_event_ms = t;
}

/* Marks every repository as having a gap. An IN_Q_OVERFLOW is global to the
 * inotify instance: the kernel does not say which events were lost, so nothing
 * can be excluded. */
static void handle_overflow(atlas_watcher *w) {
    atlas_daemon_log(w->log, "warn",
                     "the inotify queue overflowed; filesystem events were lost. Every watched "
                     "repository is marked incomplete and will be fully reconciled.");
    atlas_err err;
    atlas_err_init(&err);
    for (size_t i = 0; i < w->repo_count; i++) {
        (void)atlas_writer_submit_gap(
            w->writer, w->repos[i].repo_id,
            "the inotify queue overflowed and events were lost; a full reconciliation is required "
            "before the index can be described as current",
            &err);
        mark_dirty(w, w->repos[i].repo_id);
    }
}

static void expire_moves(atlas_watcher *w, int64_t t) {
    size_t out = 0;
    for (size_t i = 0; i < w->move_count; i++) {
        if (!w->moves[i].used && t - w->moves[i].at_ms < ATLAS_WATCH_MOVE_PAIR_MS) {
            if (out != i) {
                atlas_buf_free(&w->moves[out].path);
                w->moves[out] = w->moves[i];
                atlas_buf_init(&w->moves[i].path);
            }
            out++;
        } else {
            /* Either paired, or old enough that the matching IN_MOVED_TO is
             * never coming — the file was moved out of every watched directory,
             * which is a delete from Atlas' point of view. The watches were
             * already removed when the IN_MOVED_FROM arrived, and the repository
             * was already marked dirty, so nothing further is needed. */
            atlas_buf_free(&w->moves[i].path);
        }
    }
    w->move_count = out;
}

static void remember_move(atlas_watcher *w, uint32_t cookie, int64_t repo_id, const char *path,
                          bool is_dir) {
    if (w->move_count >= ATLAS_WATCH_MAX_PENDING_MOVES) {
        /* Bounded. Losing the pairing costs a rename being reported as a delete
         * plus an add, which the reconciliation pass resolves anyway. */
        return;
    }
    pending_move *m = &w->moves[w->move_count++];
    memset(m, 0, sizeof(*m));
    atlas_buf_init(&m->path);
    m->cookie = cookie;
    m->repo_id = repo_id;
    m->at_ms = now_ms();
    m->is_dir = is_dir;
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_buf_set_str(&m->path, path, &ignore);
}

static pending_move *take_move(atlas_watcher *w, uint32_t cookie) {
    for (size_t i = 0; i < w->move_count; i++) {
        if (!w->moves[i].used && w->moves[i].cookie == cookie) {
            w->moves[i].used = true;
            return &w->moves[i];
        }
    }
    return NULL;
}

static void handle_event(atlas_watcher *w, const struct inotify_event *ev) {
    if ((ev->mask & IN_Q_OVERFLOW) != 0) {
        handle_overflow(w);
        return;
    }
    wd_slot *s = wd_map_find(&w->map, ev->wd);
    if (s == NULL) {
        return; /* a watch we have already dropped */
    }
    int64_t repo_id = s->repo_id;

    /* IN_IGNORED means the kernel dropped the watch, normally because the
     * directory was deleted. Forget it so the map does not fill with dead wds. */
    if ((ev->mask & IN_IGNORED) != 0) {
        repo_watch *rw = find_repo(w, repo_id);
        if (rw != NULL && rw->dirs > 0) {
            rw->dirs--;
        }
        wd_map_remove(&w->map, ev->wd);
        mark_dirty(w, repo_id);
        return;
    }

    bool is_dir = (ev->mask & IN_ISDIR) != 0;
    atlas_buf full = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (ev->len > 0) {
        if (atlas_buf_set(&full, s->path.data, s->path.len, &ignore) == ATLAS_OK &&
            atlas_buf_append_ch(&full, '/', &ignore) == ATLAS_OK) {
            (void)atlas_buf_append_str(&full, ev->name, &ignore);
        }
    } else {
        (void)atlas_buf_set(&full, s->path.data, s->path.len, &ignore);
    }

    /* Keep the watch set accurate. A directory created or moved in is watched
     * recursively — otherwise the files inside a directory that appears while
     * the daemon runs would be invisible until the next periodic pass, which is
     * exactly the "new work appears in a new directory" case. */
    if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0 && full.len > 0) {
        repo_watch *rw = find_repo(w, repo_id);
        if (rw != NULL) {
            add_ctx ac;
            memset(&ac, 0, sizeof(ac));
            ac.w = w;
            ac.rw = rw;
            ac.root_len = rw->root.len;
            ac.is_meta = s->is_meta;
            atlas_err aerr;
            atlas_err_init(&aerr);
            (void)add_watch_tree(&ac, atlas_buf_cstr(&full), &aerr);
            if (ac.limit_hit || ac.budget_hit) {
                (void)atlas_writer_submit_gap(
                    w->writer, repo_id,
                    "a new directory could not be fully watched because a watch limit was reached",
                    &aerr);
            }
        }
    }
    if (is_dir && (ev->mask & (IN_DELETE | IN_MOVED_FROM)) != 0 && full.len > 0) {
        remove_watch_tree(w, atlas_buf_cstr(&full));
    }

    /* Cookie pairing. Both halves already mark the repository dirty, so this is
     * about watch bookkeeping and about being able to say, in the log, that a
     * rename happened rather than a delete plus an unrelated create. */
    if ((ev->mask & IN_MOVED_FROM) != 0 && full.len > 0) {
        remember_move(w, ev->cookie, repo_id, atlas_buf_cstr(&full), is_dir);
    } else if ((ev->mask & IN_MOVED_TO) != 0 && ev->cookie != 0) {
        pending_move *m = take_move(w, ev->cookie);
        if (m != NULL && m->repo_id != repo_id) {
            /* A rename that crossed from one watched repository into another:
             * both need a pass, not just the destination. */
            mark_dirty(w, m->repo_id);
        }
    }

    /* A move of the watched directory itself invalidates every path below it. */
    if ((ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF)) != 0) {
        remove_watch_tree(w, atlas_buf_cstr(&s->path));
    }

    /* Name the path, so the pass hashes it whatever its metadata says. Only for
     * working-tree watches: a change under .git is a reason to reconcile, but it
     * is not itself an indexable path. A directory event names no file — the
     * files inside it are found by the pass. */
    if (!s->is_meta && !is_dir && full.len > 0) {
        repo_watch *rw = find_repo(w, repo_id);
        if (rw != NULL) {
            note_dirty_path(rw, atlas_buf_cstr(&full), full.len);
        }
    }

    mark_dirty(w, repo_id);
    atlas_buf_free(&full);
}

/* --- the loop ------------------------------------------------------------ */

static void submit_due(atlas_watcher *w) {
    int64_t t = now_ms();
    expire_moves(w, t);
    for (size_t i = 0; i < w->repo_count; i++) {
        repo_watch *rw = &w->repos[i];
        bool due = false;
        bool full = false;

        if (rw->dirty) {
            /* Quiet for the debounce window, or dirty for longer than the cap.
             * The cap is what stops a continuously writing process from
             * deferring indexing indefinitely. */
            if (t - rw->last_event_ms >= ATLAS_WATCH_DEBOUNCE_MS ||
                t - rw->first_dirty_ms >= ATLAS_WATCH_MAX_DEBOUNCE_MS) {
                due = true;
            }
        }
        /* Periodic reconciliation, whether or not anything was observed. This is
         * what covers the parts inotify cannot: an unwatched subtree, a change
         * made while the daemon was stopped, a missed event. */
        if (!due && t - rw->last_submit_ms >= w->reconcile_interval_ms) {
            due = true;
            full = true;
        }
        if (!due) {
            continue;
        }

        atlas_err err;
        atlas_err_init(&err);
        /* An overflowed path list means the watcher cannot say what changed, so
         * it asks for content verification instead of naming a subset. */
        if (rw->dirty_overflow) {
            full = true;
        }
        if (atlas_writer_submit_reconcile(w->writer, rw->repo_id, full, false,
                                          rw->dirty_paths.len > 0 ? rw->dirty_paths.data : NULL,
                                          rw->dirty_paths.len, NULL, &err) != ATLAS_OK) {
            /* Backpressure. The repository stays dirty and keeps its named
             * paths, so the next tick tries again; nothing is dropped. */
            continue;
        }
        rw->dirty = false;
        rw->last_submit_ms = t;
        /* Handed over successfully, so the names are no longer owed. */
        clear_dirty_paths(rw);
    }
}

static void *watcher_main(void *arg) {
    atlas_watcher *w = (atlas_watcher *)arg;
    atlas_err err;
    atlas_err_init(&err);

    /* A read-only handle, created on this thread and never shared. The watcher
     * has no business writing, and the handle makes that structural. */
    if (atlas_db_open_readonly(atlas_buf_cstr(&w->db_path), &w->db, &err) != ATLAS_OK) {
        atlas_daemon_log(w->log, "error", "the watcher cannot open the index: %s",
                         atlas_err_msg(&err));
        return NULL;
    }

    if (rebuild_watches(w, &err) != ATLAS_OK) {
        atlas_daemon_log(w->log, "error", "cannot build the watch set: %s", atlas_err_msg(&err));
    }

    /* Initial reconciliation. Whatever happened while the daemon was not running
     * was not observed, so every repository starts with a full pass rather than
     * with an assumption that the stored index is still accurate. */
    for (size_t i = 0; i < w->repo_count; i++) {
        atlas_err serr;
        atlas_err_init(&serr);
        (void)atlas_writer_submit_reconcile(w->writer, w->repos[i].repo_id, true, false, NULL, 0u,
                                            NULL,
                                            &serr);
        w->repos[i].last_submit_ms = now_ms();
    }
    (void)pthread_mutex_lock(&w->stat_lock);
    w->primed = true;
    (void)pthread_mutex_unlock(&w->stat_lock);

    /* The read buffer must hold at least one maximum-length event. */
    static const size_t BUF = 64u * 1024u;
    char *buf = malloc(BUF);
    if (buf == NULL) {
        atlas_daemon_log(w->log, "error", "the watcher cannot allocate its event buffer");
        atlas_db_close(w->db);
        w->db = NULL;
        return NULL;
    }

    while (!atomic_load(&w->stop)) {
        struct pollfd pfd[2];
        pfd[0].fd = w->inotify_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = w->wake_fd[0];
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        /* The poll timeout is the debounce granularity: it is what makes a
         * quiet period turn into a submitted pass. */
        int rc = poll(pfd, 2u, ATLAS_WATCH_DEBOUNCE_MS / 2);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            atlas_daemon_log(w->log, "error", "the watcher's poll failed; stopping");
            break;
        }
        if ((pfd[1].revents & POLLIN) != 0) {
            char drain[64];
            while (read(w->wake_fd[0], drain, sizeof(drain)) > 0) {
                /* drained */
            }
        }
        if ((pfd[0].revents & POLLIN) != 0) {
            for (;;) {
                ssize_t n = read(w->inotify_fd, buf, BUF);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break; /* EAGAIN: drained */
                }
                if (n == 0) {
                    break;
                }
                for (ssize_t off = 0; off + (ssize_t)sizeof(struct inotify_event) <= n;) {
                    const struct inotify_event *ev = (const struct inotify_event *)(buf + off);
                    size_t rec = sizeof(struct inotify_event) + ev->len;
                    if (off + (ssize_t)rec > n) {
                        break; /* a truncated record cannot happen, but never trust the length */
                    }
                    handle_event(w, ev);
                    off += (ssize_t)rec;
                }
            }
        }

        if (atlas_writer_take_watch_dirty(w->writer)) {
            atlas_err rerr;
            atlas_err_init(&rerr);
            if (rebuild_watches(w, &rerr) != ATLAS_OK) {
                atlas_daemon_log(w->log, "error", "cannot rebuild the watch set: %s",
                                 atlas_err_msg(&rerr));
            }
        }
        submit_due(w);
        (void)pthread_mutex_lock(&w->stat_lock);
        w->watch_count = (int64_t)w->map.count;
        (void)pthread_mutex_unlock(&w->stat_lock);
    }

    free(buf);
    atlas_db_close(w->db);
    w->db = NULL;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

atlas_status atlas_watcher_start(const char *db_path, atlas_writer *writer, FILE *log,
                                 int reconcile_interval_ms, atlas_watcher **out, atlas_err *err) {
    *out = NULL;
    atlas_watcher *w = calloc(1u, sizeof(*w));
    if (w == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the watcher");
    }
    atlas_buf_init(&w->db_path);
    w->inotify_fd = -1;
    w->wake_fd[0] = -1;
    w->wake_fd[1] = -1;
    w->writer = writer;
    w->log = log;
    w->reconcile_interval_ms =
        reconcile_interval_ms > 0 ? reconcile_interval_ms : ATLAS_WATCH_RECONCILE_INTERVAL_MS;
    atomic_init(&w->stop, false);
    if (pthread_mutex_init(&w->stat_lock, NULL) != 0) {
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create the watcher mutex");
    }

    atlas_status st = atlas_buf_set_str(&w->db_path, db_path, err);
    if (st == ATLAS_OK) {
        st = wd_map_init(&w->map, (size_t)ATLAS_WATCH_MAX_DIRS * 4u, err);
    }
    if (st == ATLAS_OK) {
        w->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (w->inotify_fd < 0) {
            st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno,
                                     "cannot create an inotify instance");
        }
    }
    if (st == ATLAS_OK && pipe(w->wake_fd) != 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot create the wake pipe");
    }
    if (st == ATLAS_OK) {
        (void)fcntl(w->wake_fd[0], F_SETFL, O_NONBLOCK);
        (void)fcntl(w->wake_fd[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(w->wake_fd[1], F_SETFD, FD_CLOEXEC);
        if (pthread_create(&w->thread, NULL, watcher_main, w) != 0) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create the watcher thread");
        } else {
            w->thread_started = true;
        }
    }
    if (st != ATLAS_OK) {
        atlas_watcher_stop(w);
        return st;
    }
    *out = w;
    return ATLAS_OK;
}

void atlas_watcher_stop(atlas_watcher *w) {
    if (w == NULL) {
        return;
    }
    atomic_store(&w->stop, true);
    if (w->wake_fd[1] >= 0) {
        (void)!write(w->wake_fd[1], "x", 1u);
    }
    if (w->thread_started) {
        (void)pthread_join(w->thread, NULL);
    }
    if (w->inotify_fd >= 0) {
        (void)close(w->inotify_fd);
    }
    if (w->wake_fd[0] >= 0) {
        (void)close(w->wake_fd[0]);
    }
    if (w->wake_fd[1] >= 0) {
        (void)close(w->wake_fd[1]);
    }
    for (size_t i = 0; i < w->move_count; i++) {
        atlas_buf_free(&w->moves[i].path);
    }
    clear_repos(w);
    free(w->repos);
    wd_map_free(&w->map);
    atlas_buf_free(&w->db_path);
    (void)pthread_mutex_destroy(&w->stat_lock);
    free(w);
}

int64_t atlas_watcher_watch_count(atlas_watcher *w) {
    if (w == NULL) {
        return 0;
    }
    (void)pthread_mutex_lock(&w->stat_lock);
    int64_t n = w->watch_count;
    (void)pthread_mutex_unlock(&w->stat_lock);
    return n;
}

bool atlas_watcher_primed(atlas_watcher *w) {
    if (w == NULL) {
        return false;
    }
    (void)pthread_mutex_lock(&w->stat_lock);
    bool p = w->primed;
    (void)pthread_mutex_unlock(&w->stat_lock);
    return p;
}

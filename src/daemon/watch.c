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
#include "atlas/ipc.h"
#include "atlas/memory.h"
#include "atlas/safetext.h"
#include "atlas/sem.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_ops.h"
#include "atlas/sem_schedule.h"
#include "atlas/syspolicy.h"
#include "atlas/mirror.h"
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

/* P0. A slot is one *physical* watch descriptor and the set of repositories
 * subscribed to it, which are two different things and used to be one.
 *
 * `inotify_add_watch` on a path already watched by this instance returns the
 * *same* descriptor — verified, not assumed. The old map stored a single
 * `repo_id` per slot and overwrote it on a repeated add, so when two linked
 * worktrees of one repository were both registered, the second one to be
 * installed silently took over every watch on the shared git common directory
 * and on `refs/`. From then on a branch update reached exactly one of the two,
 * and the other's `watched_dirs` counted watches that no longer pointed at it.
 * Removing either repository then called `inotify_rm_watch` on a descriptor the
 * survivor was still relying on.
 *
 * So: the descriptor is shared, the subscription is per repository, an event
 * fans out to every subscriber, and the descriptor is released when the last
 * subscriber leaves. The single-subscriber case — every source directory in
 * every ordinary repository — stays inline and allocates nothing. */
typedef struct wd_slot {
    int wd; /* 0 = empty, -1 = tombstone */
    atlas_buf path; /* absolute path of the watched directory */
    bool is_meta;   /* a git metadata directory, not indexable source */
    int64_t sub_inline; /* the first subscriber; meaningful when sub_count > 0 */
    int64_t *subs;      /* subscribers beyond the first; NULL unless shared */
    uint16_t sub_count;
    uint16_t sub_cap;
} wd_slot;

typedef struct wd_map {
    wd_slot *slots;
    size_t cap;
    size_t count; /* physical descriptors held, never subscriptions */
} wd_map;

/* True when `repo_id` is already subscribed to this descriptor. Linear over
 * `sub_count`, which is bounded by ATLAS_WATCH_MAX_REPOS and is 1 for every
 * unshared watch — which is almost all of them. */
static bool slot_has_sub(const wd_slot *s, int64_t repo_id) {
    if (s->sub_count == 0) {
        return false;
    }
    if (s->sub_inline == repo_id) {
        return true;
    }
    for (uint16_t i = 0; i + 1u < s->sub_count; i++) {
        if (s->subs[i] == repo_id) {
            return true;
        }
    }
    return false;
}

/* Adds a subscription. `*added` says whether this call created one, which is
 * what the caller charges a budget against: subscribing twice must not cost
 * twice, and that double count is exactly what P0 found in the old code. */
static atlas_status slot_add_sub(wd_slot *s, int64_t repo_id, bool *added, atlas_err *err) {
    *added = false;
    if (slot_has_sub(s, repo_id)) {
        return ATLAS_OK;
    }
    if (s->sub_count == 0) {
        s->sub_inline = repo_id;
        s->sub_count = 1;
        *added = true;
        return ATLAS_OK;
    }
    if (s->sub_count >= ATLAS_WATCH_MAX_REPOS) {
        /* Cannot happen while the watcher refuses to observe more repositories
         * than this, and it is checked anyway: the bound that makes it
         * impossible lives in another function, and a bound enforced somewhere
         * else is a bound this code cannot see. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "more repositories subscribed to one watch than Atlas allows");
    }
    uint16_t need = (uint16_t)(s->sub_count); /* subs[] holds sub_count-1, so we need one more */
    if (need > s->sub_cap) {
        uint16_t next = s->sub_cap == 0 ? 2u : (uint16_t)(s->sub_cap * 2u);
        if (next > ATLAS_WATCH_MAX_REPOS) {
            next = (uint16_t)ATLAS_WATCH_MAX_REPOS;
        }
        int64_t *grown = realloc(s->subs, (size_t)next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory sharing a watch");
        }
        s->subs = grown;
        s->sub_cap = next;
    }
    s->subs[s->sub_count - 1u] = repo_id;
    s->sub_count++;
    *added = true;
    return ATLAS_OK;
}

/* Drops one subscription and reports how many remain. The caller releases the
 * kernel descriptor only at zero — which is what keeps a surviving worktree
 * watching the common directory after its sibling is removed. */
static uint16_t slot_remove_sub(wd_slot *s, int64_t repo_id) {
    if (s->sub_count == 0) {
        return 0;
    }
    if (s->sub_inline == repo_id) {
        if (s->sub_count > 1u) {
            s->sub_inline = s->subs[0];
            for (uint16_t i = 0; i + 2u < s->sub_count; i++) {
                s->subs[i] = s->subs[i + 1u];
            }
        }
        s->sub_count--;
        return s->sub_count;
    }
    for (uint16_t i = 0; i + 1u < s->sub_count; i++) {
        if (s->subs[i] != repo_id) {
            continue;
        }
        for (uint16_t k = i; k + 2u < s->sub_count; k++) {
            s->subs[k] = s->subs[k + 1u];
        }
        s->sub_count--;
        return s->sub_count;
    }
    return s->sub_count;
}

static void slot_free_subs(wd_slot *s) {
    free(s->subs);
    s->subs = NULL;
    s->sub_cap = 0;
    s->sub_count = 0;
    s->sub_inline = 0;
}

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
        slot_free_subs(&m->slots[i]);
    }
    free(m->slots);
    memset(m, 0, sizeof(*m));
}

/* Empties the map without giving back its allocation.
 *
 * A rebuild drops every watch, and doing that by tombstoning each slot left the
 * table full of tombstones: they do not terminate a probe chain, so chains grew
 * with every rebuild and never shrank. Clearing outright is both cheaper and the
 * only way `wd_map_put`'s tombstone reuse stays a rare path rather than the
 * normal one. */
static void wd_map_clear(wd_map *m) {
    for (size_t i = 0; i < m->cap; i++) {
        m->slots[i].wd = 0;
        m->slots[i].is_meta = false;
        atlas_buf_reset(&m->slots[i].path);
        slot_free_subs(&m->slots[i]);
    }
    m->count = 0;
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

/* Finds the slot for `wd`, creating it if this descriptor is new to the map.
 *
 * `*created` distinguishes "this is a new physical watch" from "this descriptor
 * is already held and another repository is about to subscribe", which is the
 * distinction the budget is spent on.
 *
 * The probe loop searches the **whole** chain for an existing `wd` before it
 * falls back to the first tombstone it passed. Inserting at the tombstone
 * immediately — which is what the old code did — could put a second slot for a
 * descriptor that already lived further down the chain, and then `wd_map_find`
 * would return whichever came first and the other would be unreachable and
 * unreleasable. */
/* Grows the table and rehashes, keeping every slot's owned members by moving the
 * structs whole.
 *
 * The map used to be sized once, in `atlas_watcher_start`, from the budget
 * resolved at that moment — while `rebuild_watches` re-resolves the budget on
 * every repository-set change. An operator who followed Atlas' own remedy for
 * `kernel_limit` ("raise fs.inotify.max_user_watches") and then ran `repo add`
 * got a budget an order of magnitude larger against a table that had not moved,
 * and the watcher failed with the internal error "the watch map is full" —
 * after `inotify_add_watch` had already succeeded, so the descriptor was held by
 * the kernel, absent from the map, and never released.
 *
 * Growing on demand also lets the table start small. Sizing it for the whole
 * budget up front cost 33 MiB of resident memory on a machine with a large
 * sysctl and *no repositories registered*, and made `remove_watch_tree`'s full
 * scan proportional to the budget rather than to the watches actually held. */
static atlas_status wd_map_grow(wd_map *m, atlas_err *err) {
    size_t next = m->cap * 2u;
    if (next <= m->cap) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the watch map cannot grow further");
    }
    wd_slot *slots = calloc(next, sizeof(*slots));
    if (slots == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory growing the watch map");
    }
    for (size_t i = 0; i < next; i++) {
        atlas_buf_init(&slots[i].path);
    }
    for (size_t i = 0; i < m->cap; i++) {
        wd_slot *old = &m->slots[i];
        if (old->wd <= 0) {
            atlas_buf_free(&old->path);
            slot_free_subs(old);
            continue;
        }
        size_t h = wd_hash(old->wd, next);
        for (size_t probe = 0; probe < next; probe++) {
            wd_slot *dst = &slots[(h + probe) % next];
            if (dst->wd == 0) {
                atlas_buf_free(&dst->path);
                *dst = *old; /* moves the path buffer and the subscriber array */
                break;
            }
        }
    }
    free(m->slots);
    m->slots = slots;
    m->cap = next;
    return ATLAS_OK;
}

static atlas_status wd_map_put(wd_map *m, int wd, const char *path, bool is_meta,
                               wd_slot **slot_out, bool *created, atlas_err *err) {
    *slot_out = NULL;
    *created = false;
    size_t i = wd_hash(wd, m->cap);
    wd_slot *reuse = NULL;
    for (size_t probe = 0; probe < m->cap; probe++) {
        wd_slot *s = &m->slots[(i + probe) % m->cap];
        if (s->wd == wd) {
            s->is_meta = is_meta;
            *slot_out = s;
            return atlas_buf_set_str(&s->path, path, err);
        }
        if (s->wd == -1 && reuse == NULL) {
            reuse = s; /* remembered, not taken: the chain may still hold `wd` */
        }
        if (s->wd == 0) {
            break; /* an empty slot ends the chain, so `wd` is definitely absent */
        }
    }
    if (m->count + 1u > m->cap / 2u) {
        atlas_status gst = wd_map_grow(m, err);
        if (gst != ATLAS_OK) {
            return gst;
        }
        /* The table moved, so the remembered tombstone is gone with it and the
         * chain has to be walked again in the new table. */
        reuse = NULL;
        i = wd_hash(wd, m->cap);
    }
    wd_slot *dst = reuse;
    if (dst == NULL) {
        for (size_t probe = 0; probe < m->cap; probe++) {
            wd_slot *s = &m->slots[(i + probe) % m->cap];
            if (s->wd == 0 || s->wd == -1) {
                dst = s;
                break;
            }
        }
    }
    if (dst == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the watch map is full");
    }
    dst->wd = wd;
    dst->is_meta = is_meta;
    slot_free_subs(dst);
    m->count++;
    *created = true;
    *slot_out = dst;
    return atlas_buf_set_str(&dst->path, path, err);
}

static void wd_map_remove(wd_map *m, int wd) {
    wd_slot *s = wd_map_find(m, wd);
    if (s == NULL) {
        return;
    }
    /* Tombstone rather than clear: clearing would break the probe chain of any
     * other key that hashed to this slot. */
    s->wd = -1;
    s->is_meta = false;
    atlas_buf_reset(&s->path);
    slot_free_subs(s);
    m->count--;
}

/* --- per-repository watch state ------------------------------------------ */

/* P0. The ignored-directory inventory, owned by the repository it describes.
 *
 * It used to be built on the stack inside the priming function and freed when
 * that function returned, which meant it did not exist at the moment the
 * watcher most needed it: when a directory appeared while the daemon was
 * running. That path built its context with `memset` and left the ignore set
 * NULL, so **a directory created after priming was watched recursively whatever
 * git thought of it** — every build tree, every `node_modules`, every
 * `.dart_tool`, for as long as the daemon ran.
 *
 * What it is, stated precisely because the distinction is the whole of P0's
 * first review blocker: it is an **inventory of ignored paths that existed when
 * `git ls-files` was last run**. It is never the authority on a path that did
 * not exist then — `git ls-files` enumerates the filesystem, so a `build/` rule
 * with no `build/` directory yet produces no entry at all. A path Atlas has not
 * seen before goes to `pending_ignore` and waits for a fresh inventory instead
 * of being judged against a stale one.
 *
 * Entries are NUL-separated relative directory paths, each with a trailing '/',
 * and `offsets` indexes them in sorted order so membership is a binary search
 * rather than a scan of the whole set. */
typedef struct ignore_set {
    atlas_buf bytes;
    size_t *offsets; /* into `bytes`, sorted by the entry they point at */
    size_t count;
    size_t cap;
    bool overflow; /* the bound was reached; the inventory is not complete */
} ignore_set;

static void ignore_set_init(ignore_set *set) {
    memset(set, 0, sizeof(*set));
    atlas_buf_init(&set->bytes);
}

static void ignore_set_free(ignore_set *set) {
    atlas_buf_free(&set->bytes);
    free(set->offsets);
    set->offsets = NULL;
    set->count = 0;
    set->cap = 0;
    set->overflow = false;
}

typedef struct repo_watch {
    int64_t repo_id;
    atlas_buf name;
    atlas_buf root;
    atlas_buf git_dir;
    atlas_buf common_dir;
    /* P0. Subscriptions this repository holds, split because they answer
     * different questions and because their sum is the only number that used to
     * exist. `shared` counts those whose descriptor another repository is also
     * subscribed to, which is what makes the per-repo totals legitimately
     * exceed the daemon's physical count instead of looking like an error. */
    int64_t source_dirs;
    int64_t meta_dirs;
    int64_t shared_dirs;
    bool degraded;
    atlas_watch_reason reason;
    atlas_buf degraded_detail;

    /* P0. The ignored-path inventory, and whether it needs rebuilding.
     *
     * `ignore_stale` is set by an event that could have changed what git
     * ignores: any `.gitignore` at any depth, `info/exclude`, or a HEAD move
     * that swaps one branch's ignore rules for another's. */
    ignore_set ignored;
    bool ignore_stale;
    /* P0. A bounded backoff after a failed ignore refresh.
     *
     * Degrading the repository was not enough on its own: `resolve_pending_ignores`
     * reaches a repository through its *pending queue* as well as through
     * `ignore_stale`, and the queue is not emptied by a failure — so a repository
     * whose git invocation keeps failing was asked again on every watcher tick,
     * which is a git process every 200 ms for as long as the condition lasts.
     * The attempt is frozen instead: retried when the backoff expires, or
     * immediately when a *new* ignore-rule event arrives, which is a fresh
     * reason to believe the answer changed. */
    int64_t ignore_retry_at_ms;
    int64_t ignore_backoff_ms;

    /* P0. The depth-first priming frontier: absolute paths still to visit, NUL
     * separated, consumed from the end.
     *
     * Depth-first and popped by truncation, so the buffer shrinks as the walk
     * proceeds. The old walk was a queue with a cursor that only ever advanced,
     * so it held every path it had already visited until the traversal ended.
     * Non-empty means this repository is still priming. */
    atlas_buf frontier;
    size_t frontier_count;
    int64_t primed_this_round; /* watches installed since the round's share was set */
    int64_t visits;            /* directories entered by this priming pass */
    bool prime_started;
    /* P0. Set when a source walk begins and cleared when the sweep below has
     * run. The walk judges children against an inventory captured once at its
     * start; on a large tree that walk takes tens of seconds, and a directory
     * created inside that window is absent from the inventory and therefore
     * watched. One re-check at the end closes it. */
    bool prime_sweep_owed;

    /* P0. Directories seen for the first time, waiting for a fresh inventory to
     * say whether git ignores them. Absolute paths, NUL separated.
     *
     * Nothing under them is watched while they wait, so their contents are
     * producing events nobody receives — which is why a non-empty queue keeps
     * the repository out of `watching` and its index out of `current`. */
    atlas_buf pending_ignore;
    size_t pending_ignore_count;
    bool pending_ignore_overflow;

    /* P0. A subtree that turned out to be visible was watched late, so events
     * inside it between its creation and its watch were missed. That is an
     * event gap, and it is owed a content-verifying pass before this repository
     * can be described as current again. */
    bool owes_gap;
    /* A13. Whether this watch is over the scanner's mirror rather than the
     * repository's own tree. Recorded because the two make different claims:
     * a mirror reports a change when the *scanner writes it*, not when the
     * developer saves the file, so the index is current as of the scanner's
     * last pass and no sooner. */
    bool from_mirror;
    /* Whether the publication carrying this obligation has actually been
     * accepted by the writer. Until it has, a gap already in the database says
     * nothing about *this* obligation — it may predate the subtree that created
     * it, and a pass that ran before the subtree was watched cannot have
     * verified it. Discharging on such a gap would be unsound, and it also made
     * the failure path untestable. */
    bool owes_gap_submitted;
    /* The published generation at the moment this obligation was created.
     *
     * What discharges it is a *completed pass newer than that* — a full-reconcile
     * acknowledgement — not the presence of an `event_gap` bit. A bit already set
     * when the obligation was created belongs to some earlier cause and may have
     * been satisfied by a pass that ran before the subtree this obligation is
     * about even existed; discharging on it would retire a newer obligation on
     * older evidence. And because `submit_due` forces the pass full while
     * anything is owed, a newer completed generation *is* the content
     * verification the obligation asked for, whether or not the bit ever
     * reached the row. */
    int64_t owes_gap_at_gen;
    atlas_buf owes_gap_detail;

    /* What was last written for this repository, so the watcher can publish on
     * change rather than on every tick. Priming advances continuously and would
     * otherwise queue a write per chunk, which is a lot of writes to say the
     * same thing. */
    atlas_watch_state published_state;
    atlas_watch_reason published_reason;
    int64_t published_source;
    int64_t published_meta;
    bool published_valid;

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
    /* A13. Where a mirror lives, for a repository whose own tree this process
     * cannot open. Set once at start and never mutated, so the watcher thread
     * reads it without a lock. */
    atlas_buf data_dir;
    atlas_db *db; /* read-only, owned by this thread */
    int reconcile_interval_ms;
    /* A8: whether this daemon sweeps expired leases. */
    bool orch_enabled;
    int64_t last_recover_ms;
    /* A9.2.3. When the semantic freshness sweep last ran. */
    int64_t last_sem_sweep_ms;
    /* A9.2.4. The bounded walk runs on its own, much slower, timer. */
    int64_t last_discovery_sweep_ms;
    /* A12.1. When the memory reconciliation sweep last ran. */
    int64_t last_memory_sweep_ms;

    /* P0. The resolved watch budget, and the arithmetic behind it.
     *
     * `budget_total` is what this daemon may hold across every repository, and
     * `budget_repo` is the ceiling any one repository can reach — which is the
     * whole pool, because a single large repository should be able to use it.
     * `round_share` is this allocation round's per-repository allowance, and it
     * is recomputed each round over the repositories that still want more, so a
     * repository that finishes under its share hands the remainder back and no
     * repository's completeness depends on where its name sorts.
     *
     * `kernel_max` and `budget_from_policy` are carried so status can show the
     * arithmetic rather than a bare number: an operator looking at a degraded
     * repository needs to know whether Atlas chose the limit or the kernel did. */
    int64_t budget_total;
    int64_t budget_repo;
    int64_t round_share;
    int64_t kernel_max;
    bool budget_from_policy;
    /* P0. The test channel, and deliberately not a CLI flag or an environment
     * variable: a boundary test needs a small budget, and a *public* way to set
     * one would be a second, undocumented way to configure a resource the
     * root-owned policy is supposed to own. Non-zero replaces the resolved total
     * and nothing else, so an injected limit runs the identical comparison,
     * allocation and accounting code as production. */
    int64_t budget_injected;
    /* Test channel: refuse this many watch-state publications, so the "an
     * obligation survives a failed publication" path is reachable from a
     * fixture. Zero on every production path. */
    int64_t inject_publish_failures;
    /* Whether this daemon serves the system index, which is what decides its
     * share of the kernel's per-uid inotify budget. Supplied by `daemon.c` from
     * the same guard it uses for the orchestration and gateway policies. */
    bool system_deployment;
    /* P0. The resolved bounds. Each is the compiled constant unless a test
     * injected one; nothing else reads the constants, so the comparison under
     * test is the comparison production runs. */
    size_t max_repos;
    size_t max_pending_ignore;
    size_t max_pending_ignore_bytes;

    pthread_mutex_t stat_lock;
    int64_t watch_count; /* physical descriptors; guarded by stat_lock */
    int64_t sub_count;   /* logical subscriptions; guarded by stat_lock */
    /* A copy of the resolved budget, published under `stat_lock` so the serve
     * loop can read it without racing the watcher thread's rebuild. The
     * originals above are the watcher thread's own working values. */
    int64_t stat_budget_total;
    int64_t stat_budget_repo;
    int64_t stat_kernel_max;
    bool stat_budget_from_policy;
    /* P0. How many times `git ls-files --ignored` has been run for any
     * repository. Reported so a test can assert that a persistently failing
     * repository is not being asked on every tick — a bound of this kind is
     * only real if something counts it. */
    int64_t ignore_refresh_attempts;
    int64_t stat_ignore_refresh_attempts;
    /* P0. Repositories that owe an event gap Atlas has not yet seen land in the
     * database. Reported so a test can assert the obligation is *retained*
     * across a failed publication rather than inferring it from an end state a
     * reconciliation may already have cleared. */
    int64_t stat_owed_gaps;
    /* Per-repository live view, guarded by `stat_lock`. Small and fixed: the
     * watcher never observes more repositories than the compiled ceiling. */
    struct {
        int64_t repo_id;
        bool priming;
        bool degraded;
        bool owes_gap;
    } live[ATLAS_WATCH_MAX_REPOS];
    size_t live_count;
    bool primed;         /* guarded by stat_lock */
    bool priming_complete; /* every repository's watch set is fully installed */
};

/* P0. Reads this uid's inotify ceiling.
 *
 * Unreadable is not fatal and not zero: a machine that will not say gets the
 * documented minimum, the watcher installs what it can, and the kernel refuses
 * the rest with ENOSPC — which is reported as itself. Guessing high here would
 * turn a missing file into a repository nobody can explain. */
static int64_t read_kernel_watch_max(void) {
    FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "re");
    if (f == NULL) {
        return (int64_t)ATLAS_WATCH_TOTAL_MIN;
    }
    long long v = 0;
    int n = fscanf(f, "%lld", &v);
    (void)fclose(f);
    if (n != 1 || v <= 0) {
        return (int64_t)ATLAS_WATCH_TOTAL_MIN;
    }
    return (int64_t)v;
}

/* P0. Resolves the budget: policy where it states one, otherwise a share of the
 * kernel's own limit.
 *
 * Derived rather than compiled because a watch budget written into a header is a
 * guess about a machine the author never saw — and the guess Atlas shipped with
 * was 8192, on a machine whose kernel offered 122910.
 *
 * `injected` is the test channel: a non-zero value replaces the resolved total
 * and nothing else changes, so a boundary test runs the same comparison,
 * allocation and accounting code as production. */
static void resolve_budget(atlas_watcher *w, int64_t injected) {
    w->kernel_max = read_kernel_watch_max();

    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    long long stated = 0;
    unsigned share = ATLAS_WATCH_KERNEL_SHARE_PCT_USER;
    /* The larger share belongs to a daemon that *is* the system deployment, not
     * to any daemon on a machine where a system policy happens to exist.
     *
     * `atlas_syspolicy_load` answers "is there a valid root-owned policy here?",
     * which on a deployed machine is true for every daemon any user starts —
     * including a fixture one. Claiming half the uid's inotify budget on that
     * basis would contradict the reason the two shares exist: a dedicated
     * `atlasd` has no other consumer of its budget, and a per-user daemon shares
     * the uid with every editor the operator runs. So the answer comes from
     * whether *this* daemon serves the system index, which is the same guard
     * `daemon.c` already applies to the orchestration and gateway policies. */
    if (w->system_deployment) {
        stated = atlas_syspolicy_watch_max_dirs_total_checked(&pol);
        share = atlas_syspolicy_watch_kernel_share_pct(&pol);
    }

    int64_t derived = w->kernel_max / 100 * (int64_t)share;
    if (derived < (int64_t)ATLAS_WATCH_TOTAL_MIN) {
        derived = (int64_t)ATLAS_WATCH_TOTAL_MIN;
    }
    if (derived > (int64_t)ATLAS_WATCH_TOTAL_SOFT_MAX) {
        derived = (int64_t)ATLAS_WATCH_TOTAL_SOFT_MAX;
    }

    if (injected > 0) {
        w->budget_total = injected;
        w->budget_from_policy = false;
    } else if (stated > 0) {
        w->budget_total = (int64_t)stated;
        w->budget_from_policy = true;
    } else {
        w->budget_total = derived;
        w->budget_from_policy = false;
    }
    if (w->budget_total > (int64_t)ATLAS_WATCH_DIRS_HARD_CEILING) {
        w->budget_total = (int64_t)ATLAS_WATCH_DIRS_HARD_CEILING;
    }
    /* One repository may use the whole pool. There is deliberately no smaller
     * per-repository cap: the review that produced this rejected one, because a
     * fixed per-repo ceiling stops a single large repository from using a budget
     * nobody else wants. Sharing between repositories is done by the round,
     * below, not by a constant. */
    w->budget_repo = w->budget_total;
    w->round_share = w->budget_total;
}

/* Defined below, beside the loop stages that own them; declared here because the
 * priming and ignore-resolution stages need them and sit earlier in the file. */
static void mark_dirty(atlas_watcher *w, int64_t repo_id);
static bool prime_round(atlas_watcher *w);
static void owe_gap(atlas_watcher *w, repo_watch *rw, const char *why);

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A11.5a-R2. The wall clock, for the one value that is compared against a
 * *stored* one.
 *
 * `now_ms` above is monotonic and every other timer in this file wants that: an
 * interval must not move when somebody sets the clock. The contention stamp is
 * the exception, because `op_recover` and `require_lease` compare it against
 * `orch_leases.expires_ms`, which is written from `CLOCK_REALTIME` in
 * `src/db/db_orch.c`. Stamping it monotonically made the subtraction a
 * difference between uptime and the epoch — a number in the decades — so the
 * grace window could never be entered and the sweep that shipped in `8c41245`
 * deferred exactly nothing in production. Same clock as the value it is
 * compared with, or the comparison is not a comparison. */
static int64_t wall_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
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
    ignore_set_free(&r->ignored);
    atlas_buf_free(&r->frontier);
    atlas_buf_free(&r->pending_ignore);
    atlas_buf_free(&r->owes_gap_detail);
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

static atlas_status collect_ignored(const void *path, size_t path_len, void *ud, atlas_err *err) {
    ignore_set *set = (ignore_set *)ud;
    if (path_len == 0 || ((const char *)path)[path_len - 1u] != '/') {
        return ATLAS_OK; /* an ignored file, not a directory: watches are per-directory */
    }
    if (set->count >= ATLAS_WATCH_MAX_IGNORED_DIRS ||
        set->bytes.len + path_len + 1u > ATLAS_WATCH_MAX_IGNORED_BYTES) {
        /* P0. Fails closed, where it used to fail open.
         *
         * The old comment read "the surplus is simply watched, not skipped",
         * which inverts the purpose of the set on exactly the repositories that
         * need it: past the bound, watches were spent on the trees the
         * inventory exists to skip. An inventory Atlas knows to be incomplete
         * cannot distinguish "not ignored" from "ignored but not recorded", so
         * it says so and the repository is degraded with a reason. */
        set->overflow = true;
        return ATLAS_OK;
    }
    if (set->count + 1u > set->cap) {
        size_t next = set->cap == 0 ? 64u : set->cap * 2u;
        size_t *grown = realloc(set->offsets, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory recording ignored paths");
        }
        set->offsets = grown;
        set->cap = next;
    }
    size_t at = set->bytes.len;
    atlas_status st = atlas_buf_append(&set->bytes, path, path_len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&set->bytes, '\0', err);
    }
    if (st == ATLAS_OK) {
        set->offsets[set->count++] = at;
    }
    return st;
}

static int ignore_cmp(const void *a, const void *b, void *ud) {
    const ignore_set *set = (const ignore_set *)ud;
    return strcmp(set->bytes.data + *(const size_t *)a, set->bytes.data + *(const size_t *)b);
}

/* Sorts the offsets so membership can binary-search. Insertion sort would be
 * quadratic on a repository with tens of thousands of ignored trees, and
 * `qsort_r`'s comparator needs the buffer, so the set is passed through. */
static void ignore_set_sort(ignore_set *set) {
    if (set->count > 1u) {
        qsort_r(set->offsets, set->count, sizeof(*set->offsets), ignore_cmp, set);
    }
}

/* Exact membership: is `rel` itself an entry?
 *
 * This is the hot path, and it is complete **only under the descent invariant**
 * that every ancestor of `rel` has already been tested and cleared — which the
 * priming walk guarantees, because it filters a child before pushing it and so
 * never reaches a directory beneath an ignored one. Where that invariant does
 * not hold, callers use `ignore_set_covers` below instead.
 *
 * The tempting shortcut of "binary search for the greatest entry <= rel, then
 * test whether it is a prefix" is **wrong**, and the counterexample is small
 * enough to keep here: with entries {"a/", "a/b/"} and candidate "a/z/", the
 * greatest entry not exceeding "a/z/" is "a/b/", which is not a prefix — so the
 * test answers "not ignored" while "a/" plainly covers it. */
static bool ignore_set_has_exact(const ignore_set *set, const char *rel, size_t rel_len) {
    if (set->count == 0) {
        return false;
    }
    size_t lo = 0;
    size_t hi = set->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        const char *entry = set->bytes.data + set->offsets[mid];
        size_t elen = strlen(entry);
        size_t n = rel_len < elen ? rel_len : elen;
        int c = memcmp(rel, entry, n);
        if (c == 0) {
            c = rel_len < elen ? -1 : (rel_len > elen ? 1 : 0);
        }
        if (c == 0) {
            return true;
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return false;
}

/* Is `rel` an entry, or beneath one?
 *
 * Correct without any descent invariant, and bounded: it tests each of `rel`'s
 * own ancestor prefixes for exact membership, so it is O(components x log n)
 * rather than O(n), and the component count is bounded by the path length the
 * walk already accepts. Used where a walk *starts* — the repository root, and
 * every directory that appears while the daemon runs — because those are exactly
 * the places where nothing has cleared the ancestors. */
static bool ignore_set_covers(const ignore_set *set, const char *rel, size_t rel_len) {
    if (set->count == 0) {
        return false;
    }
    for (size_t i = 0; i < rel_len; i++) {
        if (rel[i] == '/' && ignore_set_has_exact(set, rel, i + 1u)) {
            return true;
        }
    }
    /* And the path itself, which the loop above only reaches when it already
     * ends in a separator. Entries always carry a trailing '/', so the candidate
     * is given one to compare against. */
    if (rel_len > 0 && rel[rel_len - 1u] != '/') {
        char stack_buf[512];
        if (rel_len + 1u <= sizeof(stack_buf)) {
            memcpy(stack_buf, rel, rel_len);
            stack_buf[rel_len] = '/';
            return ignore_set_has_exact(set, stack_buf, rel_len + 1u);
        }
        /* A path longer than the small buffer is rare enough to pay for. */
        char *heap = malloc(rel_len + 1u);
        if (heap == NULL) {
            return false; /* fail open: watching it costs a watch, skipping it costs a file */
        }
        memcpy(heap, rel, rel_len);
        heap[rel_len] = '/';
        bool hit = ignore_set_has_exact(set, heap, rel_len + 1u);
        free(heap);
        return hit;
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
    /* P0. How much metadata this pass may install for this repository.
     *
     * The metadata phase runs twice: once with the reserve, so **every**
     * repository secures the watches a branch switch depends on, and once with
     * the full ceiling for whatever is left. Without the first pass a single
     * repository with more refs than the reserve could take the whole metadata
     * allowance and leave another repository unable to watch its own HEAD --
     * which is the same starvation the source rounds were restructured to
     * prevent, one resource over. */
    int64_t meta_cap;
    /* P0. Which bound stopped this installer, as a reason rather than a
     * boolean. Before P0 the kernel's refusal and two different Atlas bounds set
     * one of two flags between them and produced one of two sentences, so a
     * caller could not tell "raise the sysctl" from "this daemon's budget is
     * spent" — and neither could Atlas. */
    atlas_watch_reason stop;
} add_ctx;

static bool ac_stopped(const add_ctx *ac) { return ac->stop != ATLAS_WATCH_REASON_NONE; }

/* Installs one watch and subscribes this repository to it.
 *
 * Three budgets are checked, in the order that makes the message useful: the
 * repository's share of this allocation round, then the daemon's physical total,
 * then the kernel's own limit — which is not checked at all but reported when
 * `inotify_add_watch` refuses.
 *
 * The comparison is `>=`, not the old `+ 1 >=`. With a budget of N the old form
 * refused the Nth watch, so a documented ceiling of 8192 was in fact 8191 and
 * the daemon reported exactly that number for weeks. */
static atlas_status add_watch(add_ctx *ac, const char *abs_path, atlas_err *err) {
    atlas_watcher *w = ac->w;
    repo_watch *rw = ac->rw;

    if (ac->is_meta) {
        int64_t cap = ac->meta_cap > 0 ? ac->meta_cap : (int64_t)ATLAS_WATCH_META_MAX_PER_REPO;
        if (rw->meta_dirs >= cap) {
            /* Reaching the *reserve* cap is the first pass finishing its job, not
             * a refusal; only the full ceiling is a degradation. */
            ac->stop = cap >= (int64_t)ATLAS_WATCH_META_MAX_PER_REPO
                           ? ATLAS_WATCH_REASON_META_BUDGET
                           : ATLAS_WATCH_REASON_REPO_BUDGET;
            return ATLAS_OK;
        }
    } else if (rw->primed_this_round >= w->round_share) {
        /* This repository has taken its share for this round. Another round
         * gives it more if the pool still holds any, so this is not a verdict —
         * it just ends this repository's turn. */
        ac->stop = ATLAS_WATCH_REASON_REPO_BUDGET;
        return ATLAS_OK;
    }

    /* The total budget is checked **after** the descriptor is known, not before.
     *
     * `inotify_add_watch` on a path this instance already watches returns the
     * descriptor it already holds and costs the kernel nothing. Refusing that
     * because `map.count == budget_total` would deny a second worktree a
     * subscription to a descriptor its sibling is already paying for — the
     * budget counts physical watches, and this would not be one. So: ask the
     * kernel, and only refuse if the answer is a descriptor Atlas does not
     * already hold.
     *
     * The cost of that ordering is one add/remove syscall pair in the rare case
     * where the budget really is full and the path really is new. That is
     * cheaper than the alternative it replaces, which was wrong. */
    int wd = inotify_add_watch(w->inotify_fd, abs_path, ATLAS_INOTIFY_MASK);
    if (wd < 0) {
        if (errno == ENOSPC) {
            /* The kernel's per-user watch limit. This is the single most common
             * way a watcher silently stops seeing changes, so it is a reported
             * degraded state rather than a warning nobody reads — and it is kept
             * distinct from Atlas' own budgets because the remedy is a sysctl,
             * not an Atlas setting. */
            ac->stop = ATLAS_WATCH_REASON_KERNEL_LIMIT;
            return ATLAS_OK;
        }
        if (errno == ENOENT || errno == ENOTDIR || errno == EACCES || errno == ELOOP) {
            return ATLAS_OK; /* raced away, or not ours to watch */
        }
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot watch a directory");
    }

    if (wd_map_find(&w->map, wd) == NULL && (int64_t)w->map.count >= w->budget_total) {
        /* A genuinely new descriptor, and there is no room for one. Hand it back
         * rather than exceeding the budget. */
        (void)inotify_rm_watch(w->inotify_fd, wd);
        ac->stop = ATLAS_WATCH_REASON_TOTAL_BUDGET;
        return ATLAS_OK;
    }

    wd_slot *slot = NULL;
    bool created = false;
    atlas_status st = wd_map_put(&w->map, wd, abs_path, ac->is_meta, &slot, &created, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool added = false;
    st = slot_add_sub(slot, rw->repo_id, &added, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!added) {
        /* Already subscribed. The descriptor is held and this repository already
         * counts it; charging again is the double count P0 removed. */
        return ATLAS_OK;
    }
    if (ac->is_meta) {
        rw->meta_dirs++;
    } else {
        rw->source_dirs++;
        rw->primed_this_round++;
    }
    if (slot->sub_count == 2u) {
        /* The descriptor has just become shared, so *both* parties start
         * counting it: this repository, and the one that was already here.
         *
         * Incrementing only the joiner was asymmetric against the decrement in
         * `remove_watch_tree`, which fires for whichever subscriber leaves a
         * shared descriptor — so the first subscriber could be decremented for a
         * descriptor it had never counted. The figure is display-only, but a
         * displayed number that drifts is worse than none. */
        atlas_err serr;
        atlas_err_init(&serr);
        (void)serr;
        rw->shared_dirs++;
        repo_watch *first = find_repo(w, slot->sub_inline);
        if (first != NULL && first != rw) {
            first->shared_dirs++;
        }
    } else if (slot->sub_count > 2u) {
        rw->shared_dirs++;
    }
    return ATLAS_OK;
}

/* --- the resumable priming frontier ---------------------------------------
 *
 * The walk used to run to completion inside one call, and three separate things
 * were wrong with that.
 *
 * It did not poll inotify while it ran, so on a large tree the kernel's event
 * queue could overflow — and `IN_Q_OVERFLOW` is global to the instance, so one
 * repository's priming could gap every repository at once. It could not stop at
 * a budget share and resume later, which is what fair allocation between
 * repositories needs. And its pending list was a queue with a cursor that only
 * advanced, so it held every path it had already visited for the length of the
 * traversal instead of only the ones still owed.
 *
 * The frontier is therefore depth-first and popped by truncation: memory tracks
 * the frontier rather than the tree, and a chunk of it can be walked per
 * watcher tick with the event drain in between. */

/* A frontier is a buffer and a count, and it is passed explicitly rather than
 * reached through the repository.
 *
 * That matters: source priming is resumable and holds its frontier across ticks
 * on `repo_watch`, while the metadata walk and the walk of a subtree that
 * appeared while the daemon ran both run to completion inside one call. If those
 * shared the repository's frontier they would clobber a suspended source walk —
 * which is not hypothetical, it is what the first version of this did, and the
 * symptom was a repository reporting zero watched directories in the middle of
 * priming. */
static void frontier_clear(atlas_buf *buf, size_t *count) {
    atlas_buf_reset(buf);
    *count = 0;
}

static atlas_status frontier_push(atlas_buf *buf, size_t *count, const char *abs_path, size_t len,
                                  atlas_err *err) {
    if (buf->len + len + 1u > ATLAS_WATCH_FRONTIER_MAX_BYTES) {
        /* A directory wide enough to overrun the frontier. Reported rather than
         * truncated: the alternative is a silently unwatched subtree, which is
         * exactly the failure class P0 exists to end. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the watch frontier is full");
    }
    atlas_status st = atlas_buf_append(buf, abs_path, len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(buf, '\0', err);
    }
    if (st == ATLAS_OK) {
        (*count)++;
    }
    return st;
}

/* Pops the last path. Depth-first, so the buffer truncates and the memory comes
 * back without a compaction step. */
static bool frontier_pop(atlas_buf *buf, size_t *count, atlas_buf *out, atlas_err *err) {
    if (*count == 0 || buf->len == 0) {
        return false;
    }
    size_t end = buf->len - 1u; /* the NUL of the last entry */
    size_t start = end;
    while (start > 0 && buf->data[start - 1u] != '\0') {
        start--;
    }
    if (atlas_buf_set(out, buf->data + start, end - start, err) != ATLAS_OK) {
        return false;
    }
    buf->len = start;
    (*count)--;
    return true;
}

/* Advances one repository's priming by at most `max_dirs` directories.
 *
 * Returns with the frontier non-empty when it stopped early, which is what the
 * caller reads as "still priming". Never follows a symlink, so a link pointing
 * outside the repository cannot pull the watcher out of it, and never descends
 * into `.git`, which is watched separately and never indexed as source. */
static atlas_status prime_chunk(add_ctx *ac, atlas_buf *fbuf, size_t *fcount, size_t max_dirs,
                                atlas_err *err) {
    repo_watch *rw = ac->rw;
    atlas_buf current = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    size_t done = 0;

    while (st == ATLAS_OK && done < max_dirs && !ac_stopped(ac)) {
        if (!frontier_pop(fbuf, fcount, &current, err)) {
            break;
        }
        done++;
        const char *dir = atlas_buf_cstr(&current);

        if (++rw->visits > (int64_t)(ac->w->budget_repo * ATLAS_WATCH_DISCOVER_FACTOR)) {
            /* A bound on work, kept separate from the bound on watches. Before
             * P0 both set one flag and produced one sentence, so a repository
             * that had walked too far and one that had run out of budget were
             * told the same thing. */
            ac->stop = ATLAS_WATCH_REASON_DISCOVERY_BOUND;
            break;
        }

        st = add_watch(ac, dir, err);
        if (st != ATLAS_OK || ac_stopped(ac)) {
            if (ac_stopped(ac)) {
                /* Put it back: the budget may return next round, and a directory
                 * dropped here would never be walked at all. */
                atlas_err perr;
                atlas_err_init(&perr);
                (void)frontier_push(fbuf, fcount, current.data, current.len, &perr);
            }
            break;
        }

        /* Opened with O_NOFOLLOW, not plain `opendir`.
         *
         * The child was validated with `lstat` in its parent's `readdir` loop,
         * but it is only *entered* here — and with a resumable frontier the gap
         * between those two moments is no longer a few instructions, it is up to
         * the whole remaining priming pass. Repository contents are untrusted
         * input (invariant 6), so anything with write access to the tree could
         * replace the validated directory with a symlink to `/` in that window
         * and have the walk enumerate the filesystem. `IN_DONT_FOLLOW` does not
         * help: by the time `inotify_add_watch` sees the path, the traversal has
         * already happened.
         *
         * O_NOFOLLOW refuses the swapped final component with ELOOP, which lands
         * in the same "raced away" path as any other disappearance. */
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) {
            continue; /* raced away, replaced by a symlink, or unreadable */
        }
        DIR *d = fdopendir(dfd);
        if (d == NULL) {
            (void)close(dfd);
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
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
            /* Exact membership is complete here: this walk filters a child
             * before pushing it, so every directory it reaches has had all of
             * its ancestors tested and cleared. `ignore_set_covers` is for the
             * places where that is not true — see its comment. */
            if (ac->ignored != NULL && child.len > ac->root_len + 1u) {
                const char *rel = atlas_buf_cstr(&child) + ac->root_len + 1u;
                size_t rel_len = child.len - ac->root_len - 1u;
                char sep[1024];
                bool ignored = false;
                if (rel_len + 2u <= sizeof(sep)) {
                    memcpy(sep, rel, rel_len);
                    sep[rel_len] = '/';
                    ignored = ignore_set_has_exact(ac->ignored, sep, rel_len + 1u);
                } else {
                    ignored = ignore_set_covers(ac->ignored, rel, rel_len);
                }
                if (ignored) {
                    atlas_buf_free(&child);
                    continue;
                }
            }
            st = frontier_push(fbuf, fcount, child.data, child.len, err);
            atlas_buf_free(&child);
            if (st != ATLAS_OK) {
                ac->stop = ATLAS_WATCH_REASON_FRONTIER_OVERFLOW;
                st = ATLAS_OK; /* reported as a degraded state, not as a failure */
                break;
            }
        }
        (void)closedir(d);
    }
    atlas_buf_free(&current);
    return st;
}

/* Seeds a walk at `abs_root` and runs it to completion within this call.
 *
 * Used for the metadata phase, which is small and bounded by
 * ATLAS_WATCH_META_MAX_PER_REPO, and for a subtree that appeared while the
 * daemon was running. Source priming does not use it: that goes through the
 * frontier so it can be interleaved with the event drain. */
static atlas_status add_watch_tree(add_ctx *ac, const char *abs_root, atlas_err *err) {
    /* Its own frontier, not the repository's. A source walk may be suspended
     * mid-tree with its frontier held on `repo_watch`, and borrowing it here
     * would discard the rest of that walk — silently, and with the repository
     * still reporting itself as watched. */
    atlas_buf fbuf = ATLAS_BUF_INIT;
    size_t fcount = 0;
    atlas_status st = frontier_push(&fbuf, &fcount, abs_root, strlen(abs_root), err);
    if (st != ATLAS_OK) {
        ac->stop = ATLAS_WATCH_REASON_FRONTIER_OVERFLOW;
        atlas_buf_free(&fbuf);
        return ATLAS_OK;
    }
    while (st == ATLAS_OK && fcount > 0 && !ac_stopped(ac)) {
        st = prime_chunk(ac, &fbuf, &fcount, ATLAS_WATCH_PRIME_CHUNK_DIRS, err);
    }
    atlas_buf_free(&fbuf);
    return st;
}

/* Drops `repo_id`'s subscription to every watch at `prefix` or beneath it, and
 * releases the kernel descriptor only when the last subscriber has gone.
 *
 * That last clause is the fix for a real defect: two registered worktrees of one
 * repository subscribe to the same descriptor on the shared git directory, and
 * the old code called `inotify_rm_watch` on behalf of whichever repository asked
 * first — leaving the survivor believing it was watching a descriptor the kernel
 * had already released, and silently missing every branch update from then on. */
static void remove_watch_tree(atlas_watcher *w, int64_t repo_id, const char *prefix) {
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
        if (!slot_has_sub(s, repo_id)) {
            continue;
        }
        bool was_shared = s->sub_count > 1u;
        uint16_t left = slot_remove_sub(s, repo_id);
        repo_watch *rw = find_repo(w, repo_id);
        if (rw != NULL) {
            if (s->is_meta) {
                if (rw->meta_dirs > 0) {
                    rw->meta_dirs--;
                }
            } else if (rw->source_dirs > 0) {
                rw->source_dirs--;
            }
            if (was_shared && rw->shared_dirs > 0) {
                rw->shared_dirs--;
            }
        }
        if (left == 0) {
            (void)inotify_rm_watch(w->inotify_fd, s->wd);
            wd_map_remove(&w->map, s->wd);
        }
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
    ignore_set_init(&rw->ignored);
    atlas_buf_init(&rw->frontier);
    atlas_buf_init(&rw->pending_ignore);
    atlas_buf_init(&rw->owes_gap_detail);
    rw->reason = ATLAS_WATCH_REASON_NONE;
    rw->repo_id = ri->id;
    /* A13. Which tree this watch is over.
     *
     * The three paths below used to come straight from the row. They still do
     * for a repository this process can open — but a tree belonging to another
     * principal cannot be watched by opening it, and P0's metadata watches are
     * built from `git_dir` and `common_dir`, so all three must describe the
     * same repository or branch correctness rests on one tree while the source
     * walk covers another.
     *
     * So when the mirror answers they are taken from the adapter's own report
     * about what it opened, rather than from this code repeating the choice.
     * A repository neither path can open keeps the row's values and is watched
     * exactly as before. */
    atlas_git *g = NULL;
    bool from_mirror = false;
    atlas_err open_err;
    atlas_err_init(&open_err);
    const char *dd = w->data_dir.len > 0 ? atlas_buf_cstr(&w->data_dir) : NULL;
    atlas_buf mirror_root = ATLAS_BUF_INIT;
    atlas_buf mirror_gitdir = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (atlas_repo_open_git(ri, dd, &g, &from_mirror, &open_err) == ATLAS_OK) {
        if (from_mirror) {
            st = atlas_buf_set_str(&mirror_root, atlas_git_root(g), err);
            if (st == ATLAS_OK) {
                st = atlas_buf_set_str(&mirror_gitdir, atlas_git_dir(g), err);
            }
        }
        atlas_git_close(g);
    }
    rw->from_mirror = from_mirror;
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&rw->name, ri->name, err);
    }
    if (st == ATLAS_OK) {
        st = from_mirror ? atlas_buf_set(&rw->root, mirror_root.data, mirror_root.len, err)
                         : atlas_buf_set(&rw->root, ri->root_path.data, ri->root_path.len, err);
    }
    if (st == ATLAS_OK) {
        st = from_mirror ? atlas_buf_set(&rw->git_dir, mirror_gitdir.data, mirror_gitdir.len, err)
                         : atlas_buf_set(&rw->git_dir, ri->git_dir.data, ri->git_dir.len, err);
    }
    if (st == ATLAS_OK) {
        /* A mirror is a repository of its own: no linked worktrees, nothing
         * shared, so its common dir is its git dir. */
        st = from_mirror
                 ? atlas_buf_set(&rw->common_dir, mirror_gitdir.data, mirror_gitdir.len, err)
                 : atlas_buf_set(&rw->common_dir, ri->git_common_dir.data,
                                 ri->git_common_dir.len, err);
    }
    atlas_buf_free(&mirror_root);
    atlas_buf_free(&mirror_gitdir);
    if (st != ATLAS_OK) {
        repo_watch_free(rw);
        return st;
    }
    w->repo_count++;
    if (from_mirror) {
        /* Waiting is an event gap -- P0's rule, and a mirror is the same kind of
         * window. Whatever the scanner has not yet written is a change this
         * watch cannot have seen, so the repository owes a content-verifying
         * pass before it may be described as current. Owed on every build, as
         * P0 owes one for a rebuilt watch set, rather than only on a change of
         * source: the watch set is rebuilt from scratch and has no memory of
         * which tree answered last time. */
        owe_gap(w, rw, "watching a mirror");
    }
    return ATLAS_OK;
}

/* Refreshes one repository's ignored-path inventory from git.
 *
 * A failure is not fatal and does not clear what is already held: an inventory
 * Atlas cannot refresh is stale, and a stale inventory still describes trees
 * that were ignored a moment ago far better than an empty one does. What it must
 * not do is silently pass for complete, which is what `overflow` records. */
static bool refresh_ignored(atlas_watcher *w, repo_watch *rw) {
    (void)w;
    /* The staleness signal is consumed **here**, on every path, before anything
     * can fail.
     *
     * It used to be cleared only on the success path, which made a persistent
     * git failure a livelock rather than an error: an unlinked `.gitignore` in a
     * repository whose working tree had gone set `ignore_stale`, every failing
     * refresh left it set, and `resolve_pending_ignores` therefore re-primed the
     * repository on *every* watcher tick — a git process every 200 ms, a writer
     * job every tick, a full reconciliation every debounce ceiling, and a
     * repository permanently in `priming` with zero source watches. A promisor
     * repository, which `atlas_git_open` refuses by design, reproduces it
     * without deleting anything.
     *
     * One event, one attempt. A failure is reported to the caller, which
     * degrades the repository rather than asking again immediately. */
    rw->ignore_stale = false;
    w->ignore_refresh_attempts++;
    atlas_err err;
    atlas_err_init(&err);
    atlas_git *g = NULL;
    if (atlas_git_open(atlas_buf_cstr(&rw->root), &g, &err) != ATLAS_OK) {
        return false;
    }
    ignore_set fresh;
    ignore_set_init(&fresh);
    atlas_err ignore_err;
    atlas_err_init(&ignore_err);
    atlas_status st = atlas_git_ls_ignored(g, collect_ignored, &fresh, &ignore_err);
    atlas_git_close(g);
    if (st != ATLAS_OK) {
        ignore_set_free(&fresh);
        return false;
    }
    ignore_set_sort(&fresh);
    ignore_set_free(&rw->ignored);
    rw->ignored = fresh;
    rw->ignore_backoff_ms = 0;
    rw->ignore_retry_at_ms = 0;
    /* Recovery. A repository degraded *because* the ignore inventory could not be
     * read has no reason to stay degraded once it can: the condition that
     * produced the verdict is gone. Priming restarts from the root, because the
     * watch set it holds was built against whatever inventory it had before the
     * failure — or was torn down by the re-prime that accompanied it.
     *
     * Only that reason is lifted. A repository degraded for a budget or a kernel
     * refusal is not made well by a working `git ls-files`. */
    if (rw->degraded && rw->reason == ATLAS_WATCH_REASON_ERROR) {
        rw->degraded = false;
        rw->reason = ATLAS_WATCH_REASON_NONE;
        atlas_buf_reset(&rw->degraded_detail);
        frontier_clear(&rw->frontier, &rw->frontier_count);
        rw->prime_started = false;
        rw->visits = 0;
        rw->prime_sweep_owed = false;
        owe_gap(w, rw,
                "this repository's ignore rules could not be read for a time, so its watch set "
                "was incomplete and events may have been missed");
        mark_dirty(w, rw->repo_id);
    }
    return true;
}

/* Degrades a repository whose ignore inventory could not be read.
 *
 * Not a retry: `prime_round` and `continue_priming` both skip a degraded
 * repository, so this is what stops the loop. The periodic reconciliation still
 * covers the repository, and the next ignore-rule event will try again. */
static void degrade_on_ignore_failure(repo_watch *rw) {
    atlas_err err;
    atlas_err_init(&err);
    /* Exponential, from one second to a minute. Bounded at both ends: it never
     * hammers and it never gives up. */
    if (rw->ignore_backoff_ms <= 0) {
        rw->ignore_backoff_ms = ATLAS_WATCH_IGNORE_RETRY_MIN_MS;
    } else if (rw->ignore_backoff_ms < ATLAS_WATCH_IGNORE_RETRY_MAX_MS) {
        rw->ignore_backoff_ms *= 2;
        if (rw->ignore_backoff_ms > ATLAS_WATCH_IGNORE_RETRY_MAX_MS) {
            rw->ignore_backoff_ms = ATLAS_WATCH_IGNORE_RETRY_MAX_MS;
        }
    }
    rw->ignore_retry_at_ms = now_ms() + rw->ignore_backoff_ms;
    rw->degraded = true;
    rw->reason = ATLAS_WATCH_REASON_ERROR;
    (void)atlas_buf_set_str(&rw->degraded_detail,
                            "git could not be asked which directories this repository ignores, "
                            "so the watch set cannot be established; the periodic pass still "
                            "covers it",
                            &err);
}

/* P0. Phase one: the watches a branch switch depends on.
 *
 * Installed for every repository before any source tree is walked, and drawing
 * from a reserve held back for exactly this. Before P0 these went in *after* the
 * recursive source walk, so a repository large enough to exhaust the budget
 * stopped watching its own HEAD — branch correctness was contingent on the
 * source tree fitting, which is the wrong thing to make it contingent on.
 *
 * `info/` is subscribed explicitly because an inotify watch is not recursive: a
 * watch on the git directory reports `config` and `HEAD`, which are its direct
 * children, and reports **nothing at all** for `info/exclude`. That was verified
 * rather than assumed. `info/exclude` resolves to the *common* directory even
 * from a linked worktree, so one descriptor serves every worktree that shares
 * it — which the subscriber set now makes safe. */
static void watch_repository_meta(atlas_watcher *w, repo_watch *rw, int64_t meta_cap) {
    atlas_err err;
    atlas_err_init(&err);

    add_ctx ac;
    memset(&ac, 0, sizeof(ac));
    ac.w = w;
    ac.rw = rw;
    ac.root_len = rw->root.len;
    ac.is_meta = true;
    ac.meta_cap = meta_cap;
    ac.stop = ATLAS_WATCH_REASON_NONE;

    atlas_status st = ATLAS_OK;
    if (rw->git_dir.len > 0) {
        st = add_watch(&ac, atlas_buf_cstr(&rw->git_dir), &err);
    }
    bool distinct_common = rw->common_dir.len > 0 &&
                           (rw->common_dir.len != rw->git_dir.len ||
                            memcmp(rw->common_dir.data, rw->git_dir.data, rw->common_dir.len) != 0);
    if (st == ATLAS_OK && distinct_common) {
        st = add_watch(&ac, atlas_buf_cstr(&rw->common_dir), &err);
    }
    /* `info/` under both, where they differ. Absent is not an error: the
     * directory is a direct child of a watched one, so its later creation
     * arrives as an ordinary event and subscribes it then. */
    for (int which = 0; st == ATLAS_OK && which < 2; which++) {
        const atlas_buf *base = which == 0 ? &rw->common_dir : &rw->git_dir;
        if (base->len == 0 || (which == 1 && !distinct_common)) {
            continue;
        }
        atlas_buf info = ATLAS_BUF_INIT;
        if (atlas_buf_set(&info, base->data, base->len, &err) == ATLAS_OK &&
            atlas_buf_append_str(&info, "/info", &err) == ATLAS_OK) {
            st = add_watch(&ac, atlas_buf_cstr(&info), &err);
        }
        atlas_buf_free(&info);
    }
    if (st == ATLAS_OK && rw->common_dir.len > 0) {
        atlas_buf refs = ATLAS_BUF_INIT;
        if (atlas_buf_set(&refs, rw->common_dir.data, rw->common_dir.len, &err) == ATLAS_OK &&
            atlas_buf_append_str(&refs, "/refs", &err) == ATLAS_OK) {
            st = add_watch_tree(&ac, atlas_buf_cstr(&refs), &err);
        }
        atlas_buf_free(&refs);
    }
    if (st != ATLAS_OK) {
        rw->degraded = true;
        rw->reason = ATLAS_WATCH_REASON_ERROR;
        (void)atlas_buf_set_str(&rw->degraded_detail, atlas_err_msg(&err), &err);
    } else if (ac_stopped(&ac) && ac.stop != ATLAS_WATCH_REASON_REPO_BUDGET) {
        /* REPO_BUDGET here means "the reserve pass filled its allowance", which
         * is the first pass doing exactly what it is for. Only a stop at the
         * real ceiling, or a kernel or total refusal, is a degradation. */
        rw->degraded = true;
        rw->reason = ac.stop;
        (void)atlas_buf_set_str(&rw->degraded_detail, atlas_watch_reason_explain(ac.stop), &err);
    }
}

/* P0. Phase two, one round's worth: walk this repository's source tree until its
 * share of the round is spent, the frontier empties, or a bound is reached.
 *
 * Returns with a non-empty frontier when there is more to do. */
static void prime_repository_source(atlas_watcher *w, repo_watch *rw, size_t max_dirs) {
    atlas_err err;
    atlas_err_init(&err);

    if (!rw->prime_started) {
        if ((rw->ignore_stale || rw->ignored.count == 0) && !refresh_ignored(w, rw)) {
            degrade_on_ignore_failure(rw);
            return;
        }
        rw->prime_started = true;
        rw->prime_sweep_owed = true;
        rw->visits = 0;
        if (frontier_push(&rw->frontier, &rw->frontier_count, rw->root.data, rw->root.len,
                          &err) != ATLAS_OK) {
            rw->degraded = true;
            rw->reason = ATLAS_WATCH_REASON_FRONTIER_OVERFLOW;
            (void)atlas_buf_set_str(&rw->degraded_detail,
                                    atlas_watch_reason_explain(rw->reason), &err);
            return;
        }
    }
    if (rw->ignored.overflow && !rw->degraded) {
        rw->degraded = true;
        rw->reason = ATLAS_WATCH_REASON_IGNORE_OVERFLOW;
        (void)atlas_buf_set_str(&rw->degraded_detail, atlas_watch_reason_explain(rw->reason),
                                &err);
    }

    add_ctx ac;
    memset(&ac, 0, sizeof(ac));
    ac.w = w;
    ac.rw = rw;
    ac.ignored = &rw->ignored;
    ac.root_len = rw->root.len;
    ac.is_meta = false;
    ac.stop = ATLAS_WATCH_REASON_NONE;

    atlas_status st = prime_chunk(&ac, &rw->frontier, &rw->frontier_count, max_dirs, &err);
    if (st != ATLAS_OK) {
        rw->degraded = true;
        rw->reason = ATLAS_WATCH_REASON_ERROR;
        (void)atlas_buf_set_str(&rw->degraded_detail, atlas_err_msg(&err), &err);
        /* The walk is abandoned but not restarted: `prime_started` stays set, so
         * the next round does not begin it again from the root. */
        frontier_clear(&rw->frontier, &rw->frontier_count);
        return;
    }
    /* REPO_BUDGET means "this round is spent", which is not a degradation: the
     * next round may hand out more. Every other stop is one. */
    if (ac_stopped(&ac) && ac.stop != ATLAS_WATCH_REASON_REPO_BUDGET) {
        rw->degraded = true;
        rw->reason = ac.stop;
        (void)atlas_buf_set_str(&rw->degraded_detail, atlas_watch_reason_explain(ac.stop), &err);
        /* Nothing more will fit, so stop asking. The unwatched remainder is
         * covered by periodic reconciliation, which is what the reason says. */
        frontier_clear(&rw->frontier, &rw->frontier_count);
    }
}

/* True while anything about this repository's observation is still incomplete.
 *
 * All three conditions mean the same thing to a reader — there is a part of this
 * tree Atlas is not receiving events for — so all three keep it out of
 * `watching` and out of `index_current`. */
static bool repo_is_priming(const repo_watch *rw) {
    /* `!prime_started` is the third condition and it is not redundant.
     *
     * A repository whose source walk has not begun has an *empty* frontier, so
     * the first two conditions are both false and it would be published as
     * `watching` with whatever count it happened to hold — zero, immediately
     * after a re-prime dropped its source watches. That is precisely the claim
     * P0 exists to stop Atlas making: watched, complete, current, with nothing
     * actually installed. Caught by `test_watch_ignore`, which asserted the
     * count and found it reading 0. */
    return !rw->prime_started || rw->frontier_count > 0 || rw->pending_ignore_count > 0;
}

/* Publishes what this repository's watch build established. */
static void publish_repo_state(atlas_watcher *w, repo_watch *rw) {
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);
    atlas_err err;
    atlas_err_init(&err);

    atlas_watch_outcome o;
    atlas_watch_outcome_init(&o);
    o.source_dirs = rw->source_dirs;
    o.meta_dirs = rw->meta_dirs;
    o.shared_dirs = rw->shared_dirs;

    bool mark_gap = false;
    if (rw->degraded) {
        o.state = ATLAS_WATCH_DEGRADED;
        o.reason = rw->reason;
        o.detail = atlas_buf_cstr(&rw->degraded_detail);
        /* A degraded watcher may miss changes, which is exactly an event gap:
         * the index must not be described as current until a full pass runs. */
        mark_gap = true;
        atlas_daemon_log(w->log, "warn", "watcher degraded for %s: %s",
                         atlas_safe(&safe, atlas_buf_cstr(&rw->name)),
                         atlas_safe(&safe, atlas_buf_cstr(&rw->degraded_detail)));
    } else if (repo_is_priming(rw)) {
        o.state = ATLAS_WATCH_PRIMING;
        o.reason = ATLAS_WATCH_REASON_NONE;
        o.detail = "the watch set for this repository is still being installed, so parts of it "
                   "are not yet observed";
        /* Priming marks the gap, and not only because this Atlas would refuse to
         * call it current anyway.
         *
         * `atlas_watch_state_parse` maps any unrecognised state to UNWATCHED,
         * and `derive_index_current` falls through UNWATCHED to *current* — so a
         * pre-P0 client reading `"priming"` over the socket re-derives
         * `index_current: true` for a tree whose watches are still going in.
         * The gap is a field every such client already understands, and setting
         * it makes the honest answer the one they compute. It is exactly what
         * priming means: a part of this tree is producing events nobody is
         * receiving. A content-verifying pass clears it, as it does every
         * other gap. */
        mark_gap = true;
    } else {
        o.state = ATLAS_WATCH_WATCHING;
        o.reason = ATLAS_WATCH_REASON_NONE;
        o.detail = NULL;
        atlas_daemon_log(w->log, "info",
                         "watching %s (%lld source, %lld metadata, %lld shared)",
                         atlas_safe(&safe, atlas_buf_cstr(&rw->name)),
                         (long long)rw->source_dirs, (long long)rw->meta_dirs,
                         (long long)rw->shared_dirs);
    }
    /* An owed gap is independent of the state: a subtree that was watched late
     * missed events whether or not anything else went wrong, and the obligation
     * has to survive into the row. */
    if (rw->owes_gap) {
        mark_gap = true;
        if (o.detail == NULL) {
            o.detail = atlas_buf_cstr(&rw->owes_gap_detail);
        }
    }
    /* The obligation is discharged **only if the writer accepted it**.
     *
     * The comment here used to claim exactly that and the code did the opposite:
     * it cast the result away and cleared `owes_gap` unconditionally. The writer
     * queue is bounded at ATLAS_WRITER_QUEUE_MAX and a submission is refused
     * when it is full or when the writer is stopping — and A9.2.6 documents that
     * an unbounded semantic pass can occupy the writer for minutes while jobs
     * accumulate behind it. So the refusal is not hypothetical, and dropping a
     * gap on it reinstates the precise false claim this season exists to end: a
     * repository reported `watching` and `index_current` over a subtree whose
     * events were provably missed. A fixture never fills the queue, so no test
     * would have caught it.
     *
     * On failure nothing is recorded as published either, so
     * `maybe_publish_repo_state` sees a stale cache and tries again next tick. */
    if (w->inject_publish_failures > 0) {
        /* Test channel only, and zero on every production path. A dropped
         * submission is otherwise unreachable from a fixture — the writer queue
         * is never full there — and "the obligation survives a failure" is
         * exactly the property that needs proving. */
        w->inject_publish_failures--;
        atlas_daemon_log(w->log, "warn", "watch state publication refused by injection for %s",
                         atlas_safe(&safe, atlas_buf_cstr(&rw->name)));
        atlas_safe_pool_free(&safe);
        return;
    }
    if (atlas_writer_submit_watch_outcome(w->writer, rw->repo_id, &o, mark_gap, &err) !=
        ATLAS_OK) {
        atlas_daemon_log(w->log, "warn",
                         "could not record the watch state for %s; it stays owed and will be "
                         "retried: %s",
                         atlas_safe(&safe, atlas_buf_cstr(&rw->name)), atlas_err_msg(&err));
        atlas_safe_pool_free(&safe);
        return;
    }
    /* `owes_gap` is deliberately **not** cleared here.
     *
     * A successful submission means the job reached the queue, not that the row
     * was written: the writer runs later, on another thread, and its own
     * database call can fail. Treating "queued" as "persisted" is the same
     * mistake as treating "the comment says so" as "the code does so", and its
     * consequence is the one this season exists to prevent — a repository whose
     * gap was never recorded, reported current over a subtree it missed.
     *
     * The obligation is discharged in `settle_owed_gaps`, which reads the flag
     * back out of the database with the watcher's own read-only handle. Until
     * then the repository keeps asking, and `submit_due` keeps its next pass
     * full. */
    if (rw->owes_gap) {
        rw->owes_gap_submitted = true;
    }
    rw->published_state = o.state;
    rw->published_reason = o.reason;
    rw->published_source = rw->source_dirs;
    rw->published_meta = rw->meta_dirs;
    rw->published_valid = true;
    atlas_safe_pool_free(&safe);
}

/* Records that this repository owes a content-verifying pass, stamping the
 * published generation the obligation is measured against. */
static void owe_gap(atlas_watcher *w, repo_watch *rw, const char *why) {
    atlas_err err;
    atlas_err_init(&err);
    rw->owes_gap = true;
    rw->owes_gap_submitted = false;
    rw->owes_gap_at_gen = 0;
    if (w->db != NULL) {
        atlas_index_state st;
        atlas_index_state_init(&st);
        if (atlas_db_index_state_get(w->db, rw->repo_id, &st, &err) == ATLAS_OK && st.present) {
            rw->owes_gap_at_gen = st.last_complete_generation;
        }
        atlas_index_state_free(&st);
    }
    (void)atlas_buf_set_str(&rw->owes_gap_detail, why, &err);
}

/* P0. Discharges an owed gap only once it is visible in the database.
 *
 * The watcher holds a read-only handle of its own, created on its own thread, so
 * asking is cheap and needs no writer round trip. A repository that still owes a
 * gap is one whose next pass must be full, whatever else happens — see
 * `submit_due` — so an obligation that never lands costs repeated full passes
 * rather than a silent incremental one. That is the right direction to fail in.
 */
static void settle_owed_gaps(atlas_watcher *w) {
    if (w->db == NULL) {
        return;
    }
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        repo_watch *rw = &w->repos[i];
        if (!rw->owes_gap) {
            continue;
        }
        atlas_index_state st;
        atlas_index_state_init(&st);
        atlas_err err;
        atlas_err_init(&err);
        if (atlas_db_index_state_get(w->db, rw->repo_id, &st, &err) == ATLAS_OK && st.present &&
            st.last_complete_generation > rw->owes_gap_at_gen && !st.event_gap &&
            !st.pending_full_reconcile) {
            /* A pass completed after this obligation was created and left
             * nothing outstanding. That pass was full — `submit_due` forces it
             * while anything is owed — so the content verification this
             * obligation demanded has happened. */
            rw->owes_gap = false;
            rw->owes_gap_submitted = false;
        }
        atlas_index_state_free(&st);
    }
}

/* Publishes only when something a reader would notice has changed.
 *
 * Priming advances every tick, so publishing unconditionally would put a write
 * on the queue for each chunk to say the same thing — and the writer queue is
 * shared with everything else the daemon does. An owed gap always publishes,
 * because it is an obligation rather than a description. */
static void maybe_publish_repo_state(atlas_watcher *w, repo_watch *rw) {
    atlas_watch_state now = rw->degraded ? ATLAS_WATCH_DEGRADED
                            : repo_is_priming(rw) ? ATLAS_WATCH_PRIMING
                                                  : ATLAS_WATCH_WATCHING;
    atlas_watch_reason reason = rw->degraded ? rw->reason : ATLAS_WATCH_REASON_NONE;
    /* An outstanding obligation is a reason to publish **once**, not on every
     * tick. Gating on `owes_gap` alone re-enqueued a SET_WATCH job five times a
     * second for as long as the obligation lasted, which on a busy writer is a
     * queue full of identical jobs ahead of everything else. `owes_gap_submitted`
     * is the difference between "still owed" and "not yet told". */
    bool owed_and_untold = rw->owes_gap && !rw->owes_gap_submitted;
    if (rw->published_valid && !owed_and_untold && rw->published_state == now &&
        rw->published_reason == reason && rw->published_source == rw->source_dirs &&
        rw->published_meta == rw->meta_dirs) {
        return;
    }
    publish_repo_state(w, rw);
}

/* Drops this repository's *source* subscriptions, keeping its metadata ones.
 *
 * `remove_watch_tree` on the repository root would take the metadata watches
 * too — `.git` lives under the root — and metadata is what a branch switch
 * depends on. A re-prime that dropped it would make the repository blind to the
 * very event that most often triggers the re-prime. */
static void remove_repo_source_watches(atlas_watcher *w, repo_watch *rw) {
    for (size_t i = 0; i < w->map.cap; i++) {
        wd_slot *s = &w->map.slots[i];
        if (s->wd <= 0 || s->is_meta || !slot_has_sub(s, rw->repo_id)) {
            continue;
        }
        bool was_shared = s->sub_count > 1u;
        uint16_t left = slot_remove_sub(s, rw->repo_id);
        if (rw->source_dirs > 0) {
            rw->source_dirs--;
        }
        if (was_shared && rw->shared_dirs > 0) {
            rw->shared_dirs--;
        }
        if (left == 0) {
            (void)inotify_rm_watch(w->inotify_fd, s->wd);
            wd_map_remove(&w->map, s->wd);
        }
    }
}

/* P0. One re-check of the finished watch set against a freshly read inventory.
 *
 * The priming walk asks `git ls-files` once, at the start, and then judges every
 * directory it discovers against that answer. That is correct for a tree that
 * does not change under it — and a walk of a large repository takes tens of
 * seconds, during which a build can create an ignored tree the inventory has
 * never heard of. Those directories are not in the inventory, so the walk
 * watches them: exactly the outcome the inventory exists to prevent, arrived at
 * from the other direction.
 *
 * So when the frontier empties, the inventory is read once more and any watch
 * this repository holds beneath a now-ignored path is released. Bounded: one git
 * invocation and one pass over the map, once per priming run — not per round and
 * not per directory.
 *
 * It releases watches and never installs any, so it cannot miss an event that
 * was not already going to be missed; a subtree that stopped being ignored is
 * the `ignore_stale` path's business, not this one. */
static void sweep_newly_ignored(atlas_watcher *w, repo_watch *rw) {
    rw->prime_sweep_owed = false;
    if (!refresh_ignored(w, rw)) {
        degrade_on_ignore_failure(rw);
        return;
    }
    if (rw->ignored.count == 0) {
        return;
    }
    size_t released = 0;
    for (size_t i = 0; i < w->map.cap; i++) {
        wd_slot *sl = &w->map.slots[i];
        if (sl->wd <= 0 || sl->is_meta || !slot_has_sub(sl, rw->repo_id)) {
            continue;
        }
        if (sl->path.len <= rw->root.len + 1u ||
            memcmp(sl->path.data, rw->root.data, rw->root.len) != 0) {
            continue;
        }
        const char *rel = atlas_buf_cstr(&sl->path) + rw->root.len + 1u;
        size_t rel_len = sl->path.len - rw->root.len - 1u;
        if (!ignore_set_covers(&rw->ignored, rel, rel_len)) {
            continue;
        }
        bool was_shared = sl->sub_count > 1u;
        uint16_t left = slot_remove_sub(sl, rw->repo_id);
        if (rw->source_dirs > 0) {
            rw->source_dirs--;
        }
        if (was_shared && rw->shared_dirs > 0) {
            rw->shared_dirs--;
        }
        if (left == 0) {
            (void)inotify_rm_watch(w->inotify_fd, sl->wd);
            wd_map_remove(&w->map, sl->wd);
        }
        released++;
    }
    if (released > 0) {
        atlas_safe_pool safe;
        atlas_safe_pool_init(&safe);
        atlas_daemon_log(w->log, "info",
                         "released %zu watch(es) on %s that became ignored while it was being "
                         "primed",
                         released, atlas_safe(&safe, atlas_buf_cstr(&rw->name)));
        atlas_safe_pool_free(&safe);
    }
}

/* P0. Re-primes one repository against a freshly read ignore inventory.
 *
 * Repository-scoped, so an unrelated repository is not disturbed, and honest
 * about its own cost: dropping and reinstalling this repository's source watches
 * is a window in which its events are not observed, which is an event gap and is
 * recorded as one. */
static void reprime_repository(atlas_watcher *w, repo_watch *rw, const char *why) {
    atlas_err err;
    atlas_err_init(&err);
    remove_repo_source_watches(w, rw);
    atlas_buf_reset(&rw->pending_ignore);
    rw->pending_ignore_count = 0;
    rw->pending_ignore_overflow = false;
    /* A deliberate restart, so `prime_started` is cleared too: the next round
     * walks this repository from its root against the fresh inventory. */
    frontier_clear(&rw->frontier, &rw->frontier_count);
    rw->prime_started = false;
    rw->visits = 0;
    rw->degraded = false;
    rw->reason = ATLAS_WATCH_REASON_NONE;
    atlas_buf_reset(&rw->degraded_detail);
    if (!refresh_ignored(w, rw)) {
        degrade_on_ignore_failure(rw);
    }
    owe_gap(w, rw, why);
    mark_dirty(w, rw->repo_id);
}

/* P0. Answers, for every repository that has queued directories or whose ignore
 * rules moved, with **one** `git ls-files` invocation per repository per tick.
 *
 * That bound is the whole reason this is a queue rather than a question asked
 * per directory: an unpacked archive or a build that creates a thousand
 * directories in a burst would otherwise mean a thousand git processes. The
 * inventory is re-read once and the entire queue is resolved against it.
 *
 * Re-reading is what makes the answer correct for a path that did not exist
 * before. `git ls-files --others --ignored --directory` enumerates the
 * filesystem, so a directory created a moment ago is now reported — including an
 * empty one, and including a whole freshly created subtree collapsed to its
 * topmost ignored directory. */
static void resolve_pending_ignores(atlas_watcher *w) {
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        repo_watch *rw = &w->repos[i];
        /* A pending retry is a reason to come back on its own.
         *
         * The queue is not the only way a repository gets here, and after a
         * failure it may well be empty — `reprime_repository` clears it. Gating
         * entry on the queue alone meant a repository whose git had recovered sat
         * degraded forever with nothing left to bring it back. The timer is that
         * something. */
        bool retry_due = rw->ignore_retry_at_ms > 0 && now_ms() >= rw->ignore_retry_at_ms;
        if (rw->pending_ignore_count == 0 && !rw->ignore_stale && !rw->pending_ignore_overflow &&
            !retry_due) {
            continue;
        }
        /* Frozen after a failure. `ignore_stale` is the defined new trigger: an
         * actual ignore-rule event is a fresh reason to ask, and it clears the
         * wait. Everything else waits out the backoff. */
        if (!rw->ignore_stale && !retry_due && rw->ignore_retry_at_ms > 0) {
            continue;
        }
        if (rw->pending_ignore_overflow) {
            /* More appeared at once than Atlas will hold. It cannot say which of
             * them git ignores, so it says so and takes a full pass rather than
             * guessing in either direction. */
            atlas_err err;
            atlas_err_init(&err);
            rw->degraded = true;
            rw->reason = ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW;
            (void)atlas_buf_set_str(&rw->degraded_detail,
                                    atlas_watch_reason_explain(rw->reason), &err);
            reprime_repository(w, rw, atlas_watch_reason_explain(rw->reason));
            rw->degraded = true;
            rw->reason = ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW;
            (void)atlas_buf_set_str(&rw->degraded_detail,
                                    atlas_watch_reason_explain(rw->reason), &err);
            maybe_publish_repo_state(w, rw);
            continue;
        }
        if (rw->ignore_stale) {
            /* The rules themselves moved — a `.gitignore` edit, an
             * `info/exclude` write, or a branch switch that swapped one
             * branch's rules for another's. Which directories are affected is
             * not derivable from the event, so the repository is re-primed
             * against the fresh inventory: newly ignored subtrees lose their
             * watches, newly visible ones gain them, by construction. */
            reprime_repository(w, rw,
                               "this repository's git ignore rules changed, so its watch set was "
                               "rebuilt and events during the rebuild were not observed");
            maybe_publish_repo_state(w, rw);
            continue;
        }

        if (!refresh_ignored(w, rw)) {
            /* Without a fresh inventory these directories cannot be judged, and
             * judging them against a stale one is the defect this queue exists
             * to prevent. They stay unwatched and the repository says why. */
            degrade_on_ignore_failure(rw);
            maybe_publish_repo_state(w, rw);
            continue;
        }
        if (rw->ignored.overflow) {
            /* The inventory is known incomplete, so "not ignored" is not an
             * answer Atlas can give about these directories. `collect_ignored`
             * fails closed and so does this: the refresh path used to skip the
             * check that `prime_repository_source` makes. */
            atlas_err oerr;
            atlas_err_init(&oerr);
            rw->degraded = true;
            rw->reason = ATLAS_WATCH_REASON_IGNORE_OVERFLOW;
            (void)atlas_buf_set_str(&rw->degraded_detail,
                                    atlas_watch_reason_explain(rw->reason), &oerr);
            maybe_publish_repo_state(w, rw);
            continue;
        }

        atlas_buf queued = ATLAS_BUF_INIT;
        atlas_err err;
        atlas_err_init(&err);
        if (atlas_buf_set(&queued, rw->pending_ignore.data, rw->pending_ignore.len, &err) !=
            ATLAS_OK) {
            continue;
        }
        atlas_buf_reset(&rw->pending_ignore);
        rw->pending_ignore_count = 0;

        size_t off = 0;
        bool watched_any = false;
        while (off < queued.len) {
            const char *abs = queued.data + off;
            size_t alen = strlen(abs);
            off += alen + 1u;
            if (alen <= rw->root.len + 1u) {
                continue;
            }
            const char *rel = abs + rw->root.len + 1u;
            size_t rel_len = alen - rw->root.len - 1u;
            /* `covers`, not `has_exact`: this directory is a walk *entry point*,
             * so nothing has cleared its ancestors and only the bounded ancestor
             * test is complete here. */
            if (ignore_set_covers(&rw->ignored, rel, rel_len)) {
                continue; /* ignored: no watch, nothing owed, nothing to report */
            }
            /* Pushed onto the repository's own priming frontier, not walked
             * here.
             *
             * `add_watch_tree` runs to completion in one call, so a directory
             * that turns out to be a 65 000-entry visible tree — an unpacked
             * archive, a restored dependency directory — would be walked whole
             * inside a single watcher tick, with no inotify drain for its
             * duration. That is precisely the stall chunked priming was
             * introduced to remove, arrived at through the other door. The
             * frontier already has the machinery: push the root and let
             * `continue_priming` advance it a chunk per tick. */
            atlas_err aerr;
            atlas_err_init(&aerr);
            /* A late subtree begins a *new* pass over new ground, so the pass
             * accounting starts again and the post-walk sweep is owed once more.
             *
             * Without the reset, `visits` accumulated across every late batch a
             * repository ever received and eventually crossed the discovery
             * bound — reporting `discovery_bound` on a repository that had
             * walked a few directories at a time for hours. And without
             * `prime_sweep_owed`, a directory that became ignored while the late
             * walk was running would keep the watches the walk gave it. */
            rw->visits = 0;
            rw->prime_sweep_owed = true;
            if (frontier_push(&rw->frontier, &rw->frontier_count, abs, alen, &aerr) != ATLAS_OK) {
                rw->degraded = true;
                rw->reason = ATLAS_WATCH_REASON_FRONTIER_OVERFLOW;
                (void)atlas_buf_set_str(&rw->degraded_detail,
                                        atlas_watch_reason_explain(rw->reason), &aerr);
            }
            watched_any = true;
        }
        atlas_buf_free(&queued);

        if (watched_any) {
            /* The decision took a debounce interval, and this subtree was not
             * watched for the whole of it — anything created inside it in the
             * meantime produced no event. That is an event gap, and the
             * repository owes a content-verifying pass before it may be
             * described as current again. */
            owe_gap(w, rw,
                    "a directory appeared and was not watched until git had been asked whether "
                    "it is ignored, so events inside it during that interval were not observed");
            mark_dirty(w, rw->repo_id);
        }
        maybe_publish_repo_state(w, rw);
    }
}

/* P0. Advances source priming by one round, then publishes what changed. */
static void continue_priming(atlas_watcher *w) {
    bool any = false;
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        const repo_watch *rw = &w->repos[i];
        /* A non-empty frontier means a walk is suspended mid-tree; `!prime_started`
         * means one has not begun. Both need a round, and testing only the first
         * left a repository that had just been re-primed — frontier deliberately
         * emptied, `prime_started` deliberately cleared — sitting in `priming`
         * with zero source watches forever, because nothing ever asked it to
         * start. A repository that is degraded has already given up and is not
         * asked again. */
        if (!rw->degraded && (rw->frontier_count > 0 || !rw->prime_started)) {
            any = true;
            break;
        }
    }
    if (any) {
        (void)prime_round(w);
    }
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        maybe_publish_repo_state(w, &w->repos[i]);
    }
}

/* P0. One allocation round over the repositories that still want watches.
 *
 * The share is recomputed every round over the repositories whose frontier is
 * still non-empty, so a repository that finishes under its share returns the
 * remainder to the pool automatically and a single large repository alone gets
 * the whole budget. Nothing here reads a repository's position in the list, so
 * the outcome does not depend on `ORDER BY name` — which is what used to decide,
 * silently, which repository was left permanently degraded.
 *
 * Returns true while any repository is still priming. */
static bool prime_round(atlas_watcher *w) {
    size_t wanting = 0;
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        /* A degraded repository has already been told why it stopped and is not
         * asked again this build; including it would divide the pool with a
         * claimant that cannot use it. */
        if (w->repos[i].degraded) {
            continue;
        }
        if (w->repos[i].frontier_count > 0 || !w->repos[i].prime_started) {
            wanting++;
        }
    }
    if (wanting == 0) {
        return false;
    }
    /* P0. Hold back what every repository still owes its metadata reserve.
     *
     * Meta-first ordering covers the initial build and nothing else: without
     * this, a source round spends the whole pool, and metadata that appears
     * *later* — `.git/info` created live, a new `refs/` subdirectory after a
     * branch is pushed — has nothing left and reports `meta_budget`. The reserve
     * is what makes "branch correctness does not depend on the source tree
     * fitting" true after the first minute as well as during it.
     *
     * A repository that has already installed its reserve owes nothing, so a
     * daemon watching ordinary repositories holds back almost nothing. */
    int64_t reserved = 0;
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        if (w->repos[i].degraded) {
            continue;
        }
        int64_t owed = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO - w->repos[i].meta_dirs;
        if (owed > 0) {
            reserved += owed;
        }
    }
    int64_t pool = w->budget_total - (int64_t)w->map.count - reserved;
    if (pool <= 0) {
        /* Nothing left for source once metadata's reserve is set aside.
         *
         * The repositories that still want watches are told `total_budget` and
         * stop asking. A floor of one watch per round was the first attempt and
         * was wrong in a way worth recording: it kept the loop making progress,
         * and the progress it made was source watches eating the reserve one per
         * tick — which is precisely what the reserve exists to prevent. A bound
         * that yields under pressure is not a bound. */
        atlas_err err;
        atlas_err_init(&err);
        for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
            repo_watch *rw = &w->repos[i];
            if (rw->degraded || (rw->frontier_count == 0 && rw->prime_started)) {
                continue;
            }
            rw->degraded = true;
            rw->reason = ATLAS_WATCH_REASON_TOTAL_BUDGET;
            (void)atlas_buf_set_str(&rw->degraded_detail,
                                    atlas_watch_reason_explain(rw->reason), &err);
            frontier_clear(&rw->frontier, &rw->frontier_count);
            rw->prime_started = true;
        }
        return false;
    }
    int64_t share = pool / (int64_t)wanting;
    if (share < 1) {
        share = 1; /* the pool is positive, so somebody can still make progress */
    }
    w->round_share = share;

    bool more = false;
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        repo_watch *rw = &w->repos[i];
        if (rw->degraded || (rw->frontier_count == 0 && rw->prime_started)) {
            continue;
        }
        rw->primed_this_round = 0;
        prime_repository_source(w, rw, ATLAS_WATCH_PRIME_CHUNK_DIRS);
        if (rw->frontier_count == 0 && rw->prime_sweep_owed && !rw->degraded) {
            sweep_newly_ignored(w, rw);
        }
        if (rw->frontier_count > 0) {
            more = true;
        }
    }
    return more;
}

static void refresh_stats(atlas_watcher *w) {
    int64_t subs = 0;
    bool complete = true;
    for (size_t i = 0; i < w->repo_count; i++) {
        subs += w->repos[i].source_dirs + w->repos[i].meta_dirs;
        /* A degraded repository has stopped: it is not priming and never will
         * without another rebuild, so counting it as incomplete would pin
         * `priming_complete` false for the life of the daemon. Registering more
         * repositories than the watcher observes did exactly that. */
        if (!w->repos[i].degraded && repo_is_priming(&w->repos[i])) {
            complete = false;
        }
    }
    (void)pthread_mutex_lock(&w->stat_lock);
    w->watch_count = (int64_t)w->map.count;
    w->sub_count = subs;
    w->priming_complete = complete;
    w->stat_budget_total = w->budget_total;
    w->stat_budget_repo = w->budget_repo;
    w->stat_kernel_max = w->kernel_max;
    w->stat_budget_from_policy = w->budget_from_policy;
    w->stat_ignore_refresh_attempts = w->ignore_refresh_attempts;
    {
        int64_t owed = 0;
        for (size_t k = 0; k < w->repo_count && k < w->max_repos; k++) {
            if (w->repos[k].owes_gap) {
                owed++;
            }
        }
        w->stat_owed_gaps = owed;
    }
    /* The live view, refreshed with everything else and read by the serve loop
     * without touching the writer. */
    w->live_count = 0;
    for (size_t k = 0; k < w->repo_count && k < w->max_repos && k < ATLAS_WATCH_MAX_REPOS; k++) {
        w->live[w->live_count].repo_id = w->repos[k].repo_id;
        w->live[w->live_count].priming = repo_is_priming(&w->repos[k]) && !w->repos[k].degraded;
        w->live[w->live_count].degraded = w->repos[k].degraded;
        w->live[w->live_count].owes_gap = w->repos[k].owes_gap;
        w->live_count++;
    }
    (void)pthread_mutex_unlock(&w->stat_lock);
}

/* Rebuilds the whole watch set. Called at startup and whenever the repository
 * set changes, so `repo add` takes effect without restarting the daemon.
 *
 * P0 splits it into two phases across *all* repositories: every repository's
 * git metadata first, then source trees by fair-share rounds. The ordering is
 * the correctness argument — a repository must observe its own HEAD whether or
 * not the machine has budget left for its source tree.
 *
 * Source priming is only *started* here. It continues on the watcher loop in
 * chunks, so a large tree does not stop the loop draining inotify — an overflow
 * there is global to the instance and would gap every repository at once. */
static atlas_status rebuild_watches(atlas_watcher *w, atlas_err *err) {
    /* Drop every existing watch first. Rebuilding from scratch is O(watches) and
     * happens only on a repository-set change; getting incremental watch
     * bookkeeping subtly wrong would be far more expensive than that.
     *
     * Cleared rather than tombstoned one by one: tombstones do not terminate a
     * probe chain, so rebuilding by removal made every chain longer and never
     * shorter. */
    for (size_t i = 0; i < w->map.cap; i++) {
        if (w->map.slots[i].wd > 0) {
            (void)inotify_rm_watch(w->inotify_fd, w->map.slots[i].wd);
        }
    }
    wd_map_clear(&w->map);
    clear_repos(w);

    resolve_budget(w, w->budget_injected);

    build_ctx bc = {w, ATLAS_OK, err};
    atlas_status st = atlas_db_repo_list(w->db, add_repo, &bc, err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* P0. Checked before anything is installed: can the metadata reserve for
     * every repository even fit inside the budget?
     *
     * `repo_count` is bounded by ATLAS_WATCH_MAX_REPOS and the reserve is a
     * small constant, so the product cannot overflow — asserted by
     * `tests/test_watch_budget.c` against the hard ceiling rather than left to
     * be re-derived here. What can happen is a budget so small that the reserve
     * exhausts it, and the honest answer to that is to say so with both numbers
     * rather than to prime a repository whose metadata will not fit. */
    {
        size_t counted = w->repo_count < w->max_repos ? w->repo_count : w->max_repos;
        int64_t need = (int64_t)counted * (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO;
        if (counted > 0 && need >= w->budget_total) {
            /* Said once, with both numbers, and then the build proceeds.
             *
             * An earlier version degraded every repository here without
             * installing anything, which was wrong: metadata is what a branch
             * switch depends on, and a budget too small for the *reserve* is
             * still large enough to watch some of it. The reserve is a target
             * for what source may not take, not a precondition for starting —
             * so the honest response is to warn and then install what fits,
             * reporting `total_budget` to whatever does not. */
            atlas_daemon_log(w->log, "warn",
                             "the watch budget is %lld, below the metadata reserve of %lld for "
                             "%zu repositories; source directories will not be watched. Raise "
                             "fs.inotify.max_user_watches or watch_max_dirs_total.",
                             (long long)w->budget_total, (long long)need, counted);
        }
    }

    for (size_t i = 0; i < w->repo_count; i++) {
        repo_watch *rw = &w->repos[i];
        if (i >= w->max_repos) {
            /* Observed by nobody, and said so. Registration is unchanged: losing
             * observation is a watcher fact, and refusing `repo add` would be a
             * change to a different contract. */
            rw->degraded = true;
            rw->reason = ATLAS_WATCH_REASON_REPO_LIMIT;
            (void)atlas_buf_set_str(&rw->degraded_detail,
                                    atlas_watch_reason_explain(rw->reason), err);
            continue;
        }
        watch_repository_meta(w, rw, (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO);
    }
    /* Second pass, only after every repository holds its reserve: whatever any
     * one of them needs beyond it, up to the real ceiling. A repository with
     * thousands of ref prefixes can have them — but not before its neighbour has
     * the handful of watches its branch switches depend on. */
    for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
        repo_watch *rw = &w->repos[i];
        if (rw->degraded) {
            continue;
        }
        watch_repository_meta(w, rw, (int64_t)ATLAS_WATCH_META_MAX_PER_REPO);
    }
    (void)prime_round(w);
    for (size_t i = 0; i < w->repo_count; i++) {
        publish_repo_state(w, &w->repos[i]);
    }
    refresh_stats(w);
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

/* P0. Queues a directory Atlas has not judged yet.
 *
 * Deliberately installs no watch and descends nowhere. The ignored-path
 * inventory lists paths that *existed* when `git ls-files` last ran, and this
 * one did not, so the inventory has nothing to say about it — a `build/` rule
 * with no `build/` on disk produces no entry at all. Judging it against the
 * stale inventory is what made every freshly created build tree get watched in
 * full, whatever `.gitignore` said.
 *
 * The cost of waiting is stated rather than hidden: nothing under this directory
 * is watched until the decision arrives, so events inside it are being missed.
 * `repo_is_priming` therefore keeps the repository out of `watching` and out of
 * `index_current` for as long as the queue is non-empty. */
static void queue_pending_ignore(atlas_watcher *w, repo_watch *rw, const char *abs_path,
                                 size_t len) {
    if (rw->pending_ignore_overflow) {
        return;
    }
    if (rw->pending_ignore_count >= w->max_pending_ignore ||
        rw->pending_ignore.len + len + 1u > w->max_pending_ignore_bytes) {
        rw->pending_ignore_overflow = true;
        return;
    }
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (atlas_buf_append(&rw->pending_ignore, abs_path, len, &ignore) != ATLAS_OK ||
        atlas_buf_append_ch(&rw->pending_ignore, '\0', &ignore) != ATLAS_OK) {
        rw->pending_ignore_overflow = true;
    } else {
        rw->pending_ignore_count++;
    }
    /* Deliberately does **not** set `ignore_stale`. A new directory means the
     * inventory is *incomplete* for that path, which is answered by re-reading
     * it and resolving the queue. `ignore_stale` means the *rules* changed,
     * which is answered by re-priming the whole repository — a far heavier
     * thing, and setting it here made every `mkdir` trigger one. */
}

/* Does this event name something that could change what git ignores?
 *
 * Any `.gitignore` at any depth, `info/exclude` under a git directory, and a
 * HEAD move — because switching branches swaps one branch's ignore rules for
 * another's without touching a file in the working tree. Each sets
 * `ignore_stale`, and the next tick re-primes the repository against a fresh
 * inventory.
 *
 * `core.excludesFile` is a stated gap: it normally lives outside the repository
 * root, and Atlas never watches outside a repository root. A change to it is
 * picked up by the periodic pass, not immediately. */
static bool names_ignore_rules(const char *name, bool is_meta) {
    if (name == NULL) {
        return false;
    }
    if (!is_meta) {
        return strcmp(name, ".gitignore") == 0;
    }
    return strcmp(name, "exclude") == 0 || strcmp(name, "HEAD") == 0 ||
           strcmp(name, "info") == 0;
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

    /* P0. Every subscriber, not one owner.
     *
     * A descriptor on a shared git common directory belongs to every worktree
     * registered against it. The old code stored one `repo_id` per descriptor
     * and the last installer overwrote it, so a branch update reached exactly
     * one of two worktrees and the other silently stopped seeing its own refs.
     * The subscriber list is bounded by the repository count, so this loop is
     * short and its length is a bound Atlas enforces elsewhere. */
    int64_t subs[ATLAS_WATCH_MAX_REPOS];
    uint16_t nsubs = 0;
    if (s->sub_count > 0) {
        subs[nsubs++] = s->sub_inline;
        for (uint16_t i = 0; i + 1u < s->sub_count && nsubs < ATLAS_WATCH_MAX_REPOS; i++) {
            subs[nsubs++] = s->subs[i];
        }
    }
    if (nsubs == 0) {
        return;
    }
    int64_t repo_id = subs[0];
    /* Everything else this function needs from the slot, taken now.
     *
     * `s` must not be read again after this point. Two things can invalidate it:
     * `remove_watch_tree` can release the descriptor and reset the slot when
     * this repository is its last subscriber, and `add_watch_tree` can grow the
     * map and move every slot to a new allocation. `is_meta` in particular was
     * read at the end of the function after both had had a chance to run — on a
     * released slot it reads back false, which would have made a metadata path
     * be recorded as an indexable one. */
    const bool slot_is_meta = s->is_meta;
    const bool slot_shared = s->sub_count > 1u;
    atlas_buf slot_path = ATLAS_BUF_INIT;
    {
        atlas_err perr;
        atlas_err_init(&perr);
        if (atlas_buf_set(&slot_path, s->path.data, s->path.len, &perr) != ATLAS_OK) {
            return;
        }
    }
    s = NULL;

    /* IN_IGNORED means the kernel dropped the watch, normally because the
     * directory was deleted. Forget it so the map does not fill with dead wds. */
    if ((ev->mask & IN_IGNORED) != 0) {
        const bool shared = slot_shared;
        for (uint16_t i = 0; i < nsubs; i++) {
            repo_watch *rw = find_repo(w, subs[i]);
            if (rw != NULL) {
                if (slot_is_meta) {
                    if (rw->meta_dirs > 0) {
                        rw->meta_dirs--;
                    }
                } else if (rw->source_dirs > 0) {
                    rw->source_dirs--;
                }
                if (shared && rw->shared_dirs > 0) {
                    rw->shared_dirs--;
                }
            }
            mark_dirty(w, subs[i]);
        }
        wd_map_remove(&w->map, ev->wd);
        atlas_buf_free(&slot_path);
        return;
    }

    bool is_dir = (ev->mask & IN_ISDIR) != 0;
    atlas_buf full = ATLAS_BUF_INIT;
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (ev->len > 0) {
        if (atlas_buf_set(&full, slot_path.data, slot_path.len, &ignore) == ATLAS_OK &&
            atlas_buf_append_ch(&full, '/', &ignore) == ATLAS_OK) {
            (void)atlas_buf_append_str(&full, ev->name, &ignore);
        }
    } else {
        (void)atlas_buf_set(&full, slot_path.data, slot_path.len, &ignore);
    }

    /* A directory that appears while the daemon runs is *queued*, not watched.
     * See `queue_pending_ignore`: the inventory cannot answer for a path that
     * did not exist when it was built, and watching first would spend the budget
     * on exactly the trees the inventory exists to skip. Metadata directories —
     * `info/` appearing under a git dir — are watched straight away, because git
     * ignore rules have nothing to say about them. */
    if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0 && full.len > 0) {
        for (uint16_t i = 0; i < nsubs; i++) {
            repo_watch *rw = find_repo(w, subs[i]);
            if (rw == NULL) {
                continue;
            }
            if (slot_is_meta) {
                add_ctx ac;
                memset(&ac, 0, sizeof(ac));
                ac.w = w;
                ac.rw = rw;
                ac.root_len = rw->root.len;
                ac.is_meta = true;
                ac.stop = ATLAS_WATCH_REASON_NONE;
                atlas_err aerr;
                atlas_err_init(&aerr);
                (void)add_watch_tree(&ac, atlas_buf_cstr(&full), &aerr);
                if (ac_stopped(&ac)) {
                    rw->degraded = true;
                    rw->reason = ac.stop;
                    (void)atlas_buf_set_str(&rw->degraded_detail,
                                            atlas_watch_reason_explain(ac.stop), &aerr);
                }
            } else {
                queue_pending_ignore(w, rw, atlas_buf_cstr(&full), full.len);
            }
        }
    }
    /* A directory moved *out* of the tree keeps its watches — the kernel sends no
     * IN_IGNORED, because the descriptors are still valid; they now point
     * somewhere Atlas does not index. So a move needs the prefix scan.
     *
     * A delete does not. The kernel drops every watch under a deleted tree and
     * reports each one with IN_IGNORED, which the branch above handles in O(1)
     * through `wd_map_find`. Scanning as well made `rm -rf` of a 5000-directory
     * subtree cost 5000 full passes over the slot table while the watcher was
     * not reading from the inotify fd — which is how a queue overflow gets
     * manufactured, and an overflow gaps *every* repository at once. */
    if (is_dir && (ev->mask & IN_MOVED_FROM) != 0 && full.len > 0) {
        for (uint16_t i = 0; i < nsubs; i++) {
            remove_watch_tree(w, subs[i], atlas_buf_cstr(&full));
        }
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

    /* A move of the watched directory itself invalidates every path below it.
     *
     * The prefix is copied out of the slot before the loop, and it has to be:
     * the first subscriber to leave may be the last one, in which case
     * `remove_watch_tree` releases the descriptor and resets `s->path` — so the
     * next iteration would pass an **empty** prefix, which matches every watch
     * in the map and would unsubscribe that repository from all of them. */
    if ((ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF)) != 0) {
        atlas_buf self = ATLAS_BUF_INIT;
        if (atlas_buf_set(&self, slot_path.data, slot_path.len, &ignore) == ATLAS_OK &&
            self.len > 0) {
            for (uint16_t i = 0; i < nsubs; i++) {
                remove_watch_tree(w, subs[i], atlas_buf_cstr(&self));
            }
        }
        atlas_buf_free(&self);
    }

    /* P0. Did this event change what git ignores? If so the inventory is stale
     * and the repository is re-primed against a fresh one on the next tick. */
    if (ev->len > 0 && names_ignore_rules(ev->name, slot_is_meta)) {
        for (uint16_t i = 0; i < nsubs; i++) {
            repo_watch *rw = find_repo(w, subs[i]);
            if (rw != NULL) {
                rw->ignore_stale = true;
            }
        }
    }

    /* Name the path, so the pass hashes it whatever its metadata says. Only for
     * working-tree watches: a change under .git is a reason to reconcile, but it
     * is not itself an indexable path. A directory event names no file — the
     * files inside it are found by the pass. */
    if (!slot_is_meta && !is_dir && full.len > 0) {
        for (uint16_t i = 0; i < nsubs; i++) {
            repo_watch *rw = find_repo(w, subs[i]);
            if (rw != NULL) {
                note_dirty_path(rw, atlas_buf_cstr(&full), full.len);
            }
        }
    }

    for (uint16_t i = 0; i < nsubs; i++) {
        mark_dirty(w, subs[i]);
    }
    atlas_buf_free(&full);
    atlas_buf_free(&slot_path);
}

/* --- the loop ------------------------------------------------------------ */

/* A8: the orchestration recovery sweep.
 *
 * `op_recover` releases expired leases, requeues attempts that remain, ends
 * jobs past their wall deadline, and marks an exhausted job RECOVERY_REQUIRED
 * rather than FAILED. It is the whole of A8's crash recovery, and nothing
 * outside Atlas can ask for it — which is what stops one worker expiring
 * another's job, and also means it runs if and only if something here calls it.
 *
 * This is the watcher's timer because the watcher is the daemon's timer. The
 * call waits on the writer, briefly and with a bound: the sweep is a bounded
 * query over leases, and blocking this thread for a moment costs at most a
 * debounce tick of watch latency. A failure is logged and retried on the next
 * tick rather than escalated — an unswept expiry is recovered late, and there
 * is no state in which sweeping less often loses a record. */
static void recover_due(atlas_watcher *w) {
    if (!w->orch_enabled) {
        return;
    }
    int64_t t = now_ms();
    if (w->last_recover_ms != 0 && t - w->last_recover_ms < ATLAS_ORCH_RECOVER_INTERVAL_MS) {
        return;
    }
    w->last_recover_ms = t;

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_RECOVER);
    if (op == NULL) {
        return;
    }
    op->actor = ATLAS_ORCH_ACTOR_ATLAS;
    /* A11.5a-R. What this sweep knows about Atlas' own recent unavailability.
     * A refused sweep is the cheapest evidence there is that a heartbeat would
     * have been refused too: both are ordinary synchronous writes, and nothing
     * else in the daemon has to be instrumented to learn it. */
    op->contended_until_ms = atlas_orch_contention_seen();
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    atlas_err err;
    atlas_err_init(&err);
    /* `atlas_writer_orch` takes ownership of `op` on every path. */
    if (atlas_writer_orch(w->writer, op, 5000, &r, &err) != ATLAS_OK) {
        if (atlas_ipc_message_is_busy(atlas_err_msg(&err))) {
            atlas_orch_contention_note(wall_now_ms());
        }
        atlas_daemon_log(w->log, "warn", "orchestration recovery sweep failed: %s",
                         atlas_err_msg(&err));
    } else {
        if (r.recovered > 0) {
            atlas_daemon_log(w->log, "info", "orchestration recovery reclaimed %lld attempts",
                             (long long)r.recovered);
        }
        if (r.deferred > 0) {
            atlas_daemon_log(w->log, "info",
                             "orchestration recovery deferred %lld expired lease(s): the daemon "
                             "was refusing writes and the holder may not have been able to renew",
                             (long long)r.deferred);
        }
    }
    atlas_orch_result_free(&r);
}

/* A9.2.3: the daemon's semantic freshness sweep.
 *
 * This is the whole of "the daemon owns semantic freshness", and it is
 * deliberately small, because everything that could have been state here is
 * derived instead. There is no dirty bit, no queue of pending source states and
 * no debounce of its own: on each tick it asks `atlas_sem_plan_for` what the
 * situation is, and queues a build if the answer is that one is due.
 *
 * **Coalescing falls out of that rather than being implemented.** A build is
 * always a build of the tree as it is when the build starts, so six saves during
 * one build produce one further build and not six, and no save is lost — if the
 * tree has moved again by the time the build publishes, the next tick sees STALE
 * and builds again. The system converges on the newest state without ever
 * building an intermediate one, which is what §34 asks for, and it does so
 * because it has no memory of intermediate states to build from.
 *
 * **Backpressure is the writer queue's, unchanged.** The job goes through
 * `atlas_writer_submit_sem_index` — the same entry point `code.index` uses — so
 * a semantic rebuild is serialized against every other write exactly as a manual
 * one is, and a full queue means the repository stays dirty and the next tick
 * tries again. One repository failing to build never blocks another from being
 * considered, because the sweep asks about each independently and the answer for
 * a failing one is a hold rather than a retry.
 *
 * This runs on the watcher's thread because the watcher is the daemon's timer,
 * which is the argument A8's recovery sweep already makes. It holds a read-only
 * handle, so it cannot write and does not need to. */
/* A9.2.4. The bounded walk, on its own much slower timer.
 *
 * Separate from the freshness sweep, and the asymmetry is the design rather than
 * a compromise. The freshness sweep asks a question answered from the index and
 * a handful of file digests, so it can run every fifteen seconds. Discovery
 * walks a directory tree, which is the one expensive thing in this layer, so it
 * runs on `ATLAS_SEM_DISCOVERY_INTERVAL_MS` — and everything stays correct in
 * between, because the *content* of an already-accepted database is digested on
 * every freshness read. An edited or deleted database therefore moves the source
 * identity at once; only a *newly created* one waits for the next walk.
 *
 * It considers every registered repository, including ones an operator has
 * explicitly disabled. Discovery is what a status surface reports, and a
 * repository nobody is building still deserves an honest answer about what Atlas
 * can see in it — refusing to look would make "explicitly disabled" also mean
 * "and Atlas will not tell you what is there", which is not what an operator
 * asked for. */
static void discovery_sweep(atlas_watcher *w) {
    int64_t t = now_ms();
    if (w->last_discovery_sweep_ms != 0 &&
        t - w->last_discovery_sweep_ms < ATLAS_SEM_DISCOVERY_INTERVAL_MS) {
        return;
    }
    w->last_discovery_sweep_ms = t;
    if (w->db == NULL || w->writer == NULL || !atlas_sem_available()) {
        return;
    }
    for (size_t i = 0; i < w->repo_count; i++) {
        repo_watch *rw = &w->repos[i];
        atlas_err err;
        atlas_err_init(&err);
        (void)atlas_writer_submit_sem_discover(w->writer, rw->repo_id, atlas_buf_cstr(&rw->name),
                                               &err);
        /* A failure means the queue is full and this repository keeps the
         * verdict it had. The next interval tries again — the same backpressure
         * every sweep here uses, and nothing is lost by a walk that did not
         * happen this time. */
    }
}

static void sem_sweep(atlas_watcher *w) {
    int64_t t = now_ms();
    if (w->last_sem_sweep_ms != 0 && t - w->last_sem_sweep_ms < ATLAS_SEM_SWEEP_INTERVAL_MS) {
        return;
    }
    w->last_sem_sweep_ms = t;
    if (w->db == NULL || !atlas_sem_available()) {
        return;
    }

    /* A9.2.4. The root-owned answer to "may this daemon run a compiler over a
     * repository nobody has spoken about?", read once for the whole sweep. */
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    const bool policy_default = atlas_syspolicy_semantic_auto_default(&pol);

    int64_t repos[ATLAS_SEM_SWEEP_MAX_REPOS];
    size_t n = 0;
    bool truncated = false;
    atlas_err err;
    atlas_err_init(&err);
    if (atlas_db_sem_config_repos(w->db, repos, ATLAS_SEM_SWEEP_MAX_REPOS, &n, &truncated, &err) !=
        ATLAS_OK) {
        atlas_daemon_log(w->log, "warn", "the semantic freshness sweep could not read the "
                                         "registered repositories: %s",
                         atlas_err_msg(&err));
        return;
    }
    if (truncated) {
        /* Reported rather than silently applied. A repository dropped from a
         * sweep is one that never rebuilds, and a bound that trims a result
         * without saying so is the one thing this layer must not have. */
        atlas_daemon_log(w->log, "warn",
                         "more than %d repositories are registered; this semantic sweep "
                         "considered %zu of them",
                         ATLAS_SEM_SWEEP_MAX_REPOS, n);
    }

    for (size_t i = 0; i < n; i++) {
        repo_watch *rw = find_repo(w, repos[i]);
        if (rw == NULL) {
            /* Configured but not watched: the registry and the build description
             * disagree, which happens while a repository is being removed. There
             * is nothing to rebuild and nothing to report. */
            continue;
        }
        atlas_repo_info info;
        atlas_repo_info_init(&info);
        bool found = false;
        atlas_err rerr;
        atlas_err_init(&rerr);
        if (atlas_db_repo_get(w->db, atlas_buf_cstr(&rw->name), &info, &found, &rerr) != ATLAS_OK ||
            !found) {
            atlas_repo_info_free(&info);
            continue;
        }

        /* Asked of the writer, which owns the queue and runs the job.
         *
         * The daemon's first cut kept a flag here instead and fed it into the
         * plan it then used to decide whether to clear it — so the plan always
         * said BUILDING, the flag never cleared, and the repository reported
         * DIRTY for ever having rebuilt exactly once. A scheduler must not
         * derive its own liveness from a value it supplied. */
        const bool in_flight = atlas_writer_sem_index_pending(w->writer, rw->repo_id);

        atlas_sem_plan plan;
        atlas_sem_plan_init(&plan);
        atlas_err perr;
        atlas_err_init(&perr);
        /* The root-owned default, read once for the whole sweep rather than
         * once per repository: it is a file on disk, the answer is the same for
         * every repository in this pass, and a sweep that re-read it per
         * repository could see it change halfway and treat two repositories
         * under two policies. */
        atlas_status pst = atlas_sem_plan_for_with_default(w->db, &info, in_flight, policy_default,
                                                           &plan, &perr);
        atlas_repo_info_free(&info);
        if (pst != ATLAS_OK) {
            atlas_daemon_log(w->log, "warn", "cannot plan a semantic rebuild: %s",
                             atlas_err_msg(&perr));
            continue;
        }
        if (!plan.should_build) {
            continue;
        }

        /* A9.2.4: what discovery accepted, not what an operator pinned. A
         * repository whose compilation databases were found rather than named
         * has an empty pinned list and a full accepted one, and reading the
         * pinned list here would mean the daemon never builds exactly the
         * repositories this season exists to maintain. */
        atlas_buf list = ATLAS_BUF_INIT;
        atlas_err cerr;
        atlas_err_init(&cerr);
        atlas_status cst = atlas_sem_accepted_inputs(w->db, rw->repo_id, &list, NULL, &cerr);
        if (cst != ATLAS_OK || list.len == 0) {
            atlas_buf_free(&list);
            continue;
        }

        atlas_err serr;
        atlas_err_init(&serr);
        /* `op_id` is zero: nobody is polling for this. The operations table
         * exists so a *client* that asked for an index can find out how it went,
         * and no client asked for this one. What it produced is the generation
         * record, which is durable and is what an operator reads. */
        if (atlas_writer_submit_sem_index(w->writer, rw->repo_id, atlas_buf_cstr(&rw->name),
                                          (const char *)list.data, list.len, false, 0,
                                          &serr) == ATLAS_OK) {
            atlas_daemon_log(w->log, "info",
                             "semantic index scheduled for repository %lld: %s",
                             (long long)rw->repo_id,
                             plan.stale_reason != NULL ? plan.stale_reason
                                                       : atlas_sem_activity_name(plan.activity));
        }
        /* On failure the repository simply stays dirty and the next sweep tries
         * again — the same backpressure `submit_due` uses, and nothing is lost. */
        atlas_buf_free(&list);
    }
}

typedef struct memory_sweep_ctx {
    atlas_db *db;
    atlas_writer *writer;
    const atlas_syspolicy *pol;
} memory_sweep_ctx;

static atlas_status memory_sweep_one(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    (void)err;
    memory_sweep_ctx *ctx = (memory_sweep_ctx *)ud;
    atlas_memory_gen_cause cause = ATLAS_MEMORY_CAUSE_UNKNOWN;
    atlas_err perr;
    atlas_err_init(&perr);
    if (atlas_memory_plan_for(ctx->db, ri, ctx->pol, &cause, &perr) == ATLAS_OK &&
        cause != ATLAS_MEMORY_CAUSE_UNKNOWN) {
        atlas_err serr;
        atlas_err_init(&serr);
        /* Fire-and-forget, the same backpressure every sweep in this file
         * uses: a full queue means this repository keeps the verdict it had
         * and the next interval tries again. */
        (void)atlas_writer_submit_memory_reconcile(ctx->writer, ri->id, ctx->pol, &serr);
    }
    /* One repository's own obstacle -- `atlas_memory_plan_for` could not read
     * the index for it -- must not stop the walk over the rest. */
    return ATLAS_OK;
}

/* A12.1. Decision 10's sweep, given an already-loaded policy.
 *
 * Deliberately free of `atlas_watcher`: `atlas_db_repo_list` reads the
 * registry directly rather than the watcher's own tracked subset, and
 * `atlas_writer_submit_memory_reconcile` is the cross-thread-safe surface
 * every other sweep in this file already submits through. Nothing here
 * touches a field only the watcher's own thread may touch, which is what
 * lets `memory_sweep` below be the *only* thing that has to run there --
 * this function is what a test drives directly, with a hand-built policy,
 * exactly as `atlas_memory_plan_for` itself is already tested. */
void atlas_memory_sweep_for(atlas_db *db, atlas_writer *writer, const atlas_syspolicy *pol) {
    if (db == NULL || writer == NULL || pol == NULL) {
        return;
    }
    /* A12.1 T10 §5: the `_checked` accessor, not the one that reads the field
     * alone. A policy that parsed only part way -- a well-formed
     * `memory_reconcile = ENABLED` followed by a line that did not parse --
     * leaves the field set on a struct whose `state` is LEGACY, and honouring
     * that half would start a pass that reads documents and writes stored
     * claims from a policy nobody can read back. */
    if (!atlas_syspolicy_memory_reconcile_effective_checked(pol)) {
        return;
    }
    memory_sweep_ctx ctx;
    ctx.db = db;
    ctx.writer = writer;
    ctx.pol = pol;
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_db_repo_list(db, memory_sweep_one, &ctx, &err);
}

/* A12.1. Decision 10's sweep, on the tick beside `sem_sweep`.
 *
 * Throttled by `ATLAS_MEMORY_SWEEP_INTERVAL_MS`, `last_sem_sweep_ms`'s own
 * shape. The policy is loaded here, once per sweep, for the same reason
 * `sem_sweep` loads its own once per sweep rather than per repository: it is
 * a file on disk, the answer is the same for every repository this pass
 * considers, and re-reading it per repository could see it change halfway
 * and treat two repositories under two policies. */
static void memory_sweep(atlas_watcher *w) {
    int64_t t = now_ms();
    if (w->last_memory_sweep_ms != 0 &&
        t - w->last_memory_sweep_ms < ATLAS_MEMORY_SWEEP_INTERVAL_MS) {
        return;
    }
    w->last_memory_sweep_ms = t;
    if (w->db == NULL || w->writer == NULL) {
        return;
    }
    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    atlas_memory_sweep_for(w->db, w->writer, &pol);
}

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
        /* An owed gap may not yet be in the database — the publication is
         * asynchronous and can fail. Until the watcher has seen it land, the
         * pass it asks for is full on its own account, so a lost publication
         * cannot quietly downgrade the content verification the gap requires. */
        if (rw->owes_gap) {
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
            /* P0. A rebuild drops every watch and reinstalls it, and events that
             * land in that window are gone: the kernel had no descriptor to
             * report them on. Nothing used to record that, so a repository came
             * out of a rebuild reported as `watching` with a hole in its
             * history that no later pass had any reason to look for.
             *
             * Every repository is therefore owed a content-verifying pass after
             * a rebuild. It is charged before `rebuild_watches` runs, so the
             * obligation exists even if the rebuild itself fails. */
            for (size_t i = 0; i < w->repo_count; i++) {
                atlas_err gerr;
                atlas_err_init(&gerr);
                (void)atlas_writer_submit_gap(
                    w->writer, w->repos[i].repo_id,
                    "the watch set was rebuilt after a repository-set change, so events during "
                    "the rebuild were not observed and a full reconciliation is required",
                    &gerr);
            }
            if (rebuild_watches(w, &rerr) != ATLAS_OK) {
                atlas_daemon_log(w->log, "error", "cannot rebuild the watch set: %s",
                                 atlas_err_msg(&rerr));
            }
            for (size_t i = 0; i < w->repo_count; i++) {
                atlas_err serr;
                atlas_err_init(&serr);
                (void)atlas_writer_submit_reconcile(w->writer, w->repos[i].repo_id, true, false,
                                                    NULL, 0u, NULL, &serr);
                w->repos[i].last_submit_ms = now_ms();
            }
        }
        /* P0. Say the repository is priming **before** anything is decided or
         * walked, not after.
         *
         * A directory that has just appeared is already an unobserved subtree:
         * nothing under it is watched, and it stays that way until git has been
         * asked. If the published state only caught up once resolution finished,
         * a reader arriving in between would be told `watching` and
         * `index_current` over exactly that subtree — the claim this season
         * exists to prevent, narrowed to a window rather than removed. Resolution
         * and the chunked walk can both span many ticks, so the window is not
         * theoretical.
         *
         * Publishing first costs one writer job per repository per transition,
         * because `maybe_publish_repo_state` still only writes on change. */
        settle_owed_gaps(w);
        for (size_t i = 0; i < w->repo_count && i < w->max_repos; i++) {
            maybe_publish_repo_state(w, &w->repos[i]);
        }
        resolve_pending_ignores(w);
        continue_priming(w);
        submit_due(w);
        recover_due(w);
        /* After `submit_due`, deliberately: the semantic sweep holds while the
         * file index is behind, so asking it before the reconciliation pass has
         * even been queued would only ever produce that hold. */
        discovery_sweep(w);
        sem_sweep(w);
        /* A12.1. The *observe* phase reads a source through git directly
         * (`atlas_memory_read_source`), never through Atlas' own file index --
         * but the *owed* decision this sweep makes first
         * (`atlas_memory_plan_for`) compares a REPO_FILE source's already-
         * materialised hash against `atlas_db_verify_file_hash`, which reads
         * that same file index. So this sweep holds while the file index is
         * behind for exactly `sem_sweep`'s reason: an edit that has not yet
         * reached `files` is invisible to the comparison, and the sweep would
         * see the old hash and decide nothing is owed. After `submit_due` is
         * therefore where this belongs, not merely where it is convenient --
         * convergence at the next tick once reconciliation has caught up, not
         * immediacy. */
        memory_sweep(w);
        refresh_stats(w);
    }

    free(buf);
    atlas_db_close(w->db);
    w->db = NULL;
    return NULL;
}

/* --- lifecycle ----------------------------------------------------------- */

void atlas_watcher_opts_init(atlas_watcher_opts *o) { memset(o, 0, sizeof(*o)); }

atlas_status atlas_watcher_start(const char *db_path, const char *data_dir,
                                 atlas_writer *writer, FILE *log,
                                 const atlas_watcher_opts *opts, atlas_watcher **out,
                                 atlas_err *err) {
    atlas_watcher_opts defaults;
    atlas_watcher_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }
    *out = NULL;
    atlas_watcher *w = calloc(1u, sizeof(*w));
    if (w == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory starting the watcher");
    }
    atlas_buf_init(&w->db_path);
    atlas_buf_init(&w->data_dir);
    w->inotify_fd = -1;
    w->wake_fd[0] = -1;
    w->wake_fd[1] = -1;
    w->writer = writer;
    w->log = log;
    w->orch_enabled = opts->orch_enabled;
    w->system_deployment = opts->system_deployment;
    w->reconcile_interval_ms = opts->reconcile_interval_ms > 0
                                   ? opts->reconcile_interval_ms
                                   : ATLAS_WATCH_RECONCILE_INTERVAL_MS;
    /* Zero means the compiled bound, for every one of these. */
    w->max_repos =
        opts->inject_max_repos > 0 ? opts->inject_max_repos : (size_t)ATLAS_WATCH_MAX_REPOS;
    if (w->max_repos > (size_t)ATLAS_WATCH_MAX_REPOS) {
        /* Clamped, because `handle_event` snapshots subscribers into a stack
         * array sized by the *compiled* ceiling. Injection exists to make a
         * bound smaller and reachable in a test; letting it make one larger
         * would turn a test channel into a buffer overflow. */
        w->max_repos = (size_t)ATLAS_WATCH_MAX_REPOS;
    }
    w->max_pending_ignore = opts->inject_max_pending_ignore > 0
                                ? opts->inject_max_pending_ignore
                                : (size_t)ATLAS_WATCH_MAX_PENDING_IGNORE;
    w->max_pending_ignore_bytes = opts->inject_max_pending_ignore_bytes > 0
                                      ? opts->inject_max_pending_ignore_bytes
                                      : (size_t)ATLAS_WATCH_MAX_PENDING_IGNORE_BYTES;
    atomic_init(&w->stop, false);
    if (pthread_mutex_init(&w->stat_lock, NULL) != 0) {
        free(w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot create the watcher mutex");
    }

    /* P0. The budget is resolved here and re-resolved on every rebuild, because
     * `fs.inotify.max_user_watches` and the root-owned policy can both change
     * under a running daemon. The map does not depend on it: it grows on demand,
     * so a raised budget can never turn into "the watch map is full". */
    resolve_budget(w, opts->inject_budget_total);
    w->budget_injected = opts->inject_budget_total;
    w->inject_publish_failures = opts->inject_publish_failures;
    /* Published before the thread exists, so a status call that arrives before
     * the first rebuild reports the real budget rather than zero. */
    w->stat_budget_total = w->budget_total;
    w->stat_budget_repo = w->budget_repo;
    w->stat_kernel_max = w->kernel_max;
    w->stat_budget_from_policy = w->budget_from_policy;

    atlas_status st = atlas_buf_set_str(&w->db_path, db_path, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&w->data_dir, data_dir != NULL ? data_dir : "", err);
    }
    if (st == ATLAS_OK) {
        /* Small, and grown on demand by `wd_map_grow`. Sizing it for the whole
         * resolved budget up front made a daemon with no repositories resident
         * for tens of megabytes on any machine with a large sysctl, and made
         * every `remove_watch_tree` scan proportional to the budget rather than
         * to the watches actually held. */
        st = wd_map_init(&w->map, 4096u, err);
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
    atlas_buf_free(&w->data_dir);
    (void)pthread_mutex_destroy(&w->stat_lock);
    free(w);
}

/* P0. The watcher's own live view of one repository, for a reader that must not
 * be told something the database has not caught up with yet.
 *
 * Publishing a watch state is asynchronous: the job goes on the single writer's
 * queue, and A9.2.6 documents that an unbounded semantic pass can hold that
 * thread for minutes. During such a stretch the stored row still says whatever
 * was true before — so a repository that started priming, or that owes a
 * content-verifying pass, would be reported `watching` and `index_current`
 * for as long as the writer was busy. Enqueueing is not persistence, and a
 * claim that is true only once a queue drains is not a claim Atlas may make.
 *
 * This is the overlay that closes it: the serve loop asks the watcher directly,
 * under the same `stat_lock` every other statistic uses, and downgrades the
 * answer when the live view is worse than the stored one. It never upgrades —
 * a stored gap stays a gap whatever the watcher thinks — so it can only ever
 * make Atlas less confident, which is the only safe direction.
 *
 * Non-blocking by construction: one mutex the watcher holds for a few
 * assignments, no writer involvement, no I/O. */
void atlas_watcher_repo_live(atlas_watcher *w, int64_t repo_id, atlas_watch_live *out) {
    memset(out, 0, sizeof(*out));
    if (w == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&w->stat_lock);
    for (size_t i = 0; i < w->live_count && i < ATLAS_WATCH_MAX_REPOS; i++) {
        if (w->live[i].repo_id != repo_id) {
            continue;
        }
        out->known = true;
        out->priming = w->live[i].priming;
        out->degraded = w->live[i].degraded;
        out->owes_gap = w->live[i].owes_gap;
        break;
    }
    (void)pthread_mutex_unlock(&w->stat_lock);
}

void atlas_watcher_stats(atlas_watcher *w, atlas_watch_stats *out) {
    memset(out, 0, sizeof(*out));
    if (w == NULL) {
        return;
    }
    /* Every field here is written on the watcher thread and read from the serve
     * loop, so all of them are taken under the one lock — including the budget,
     * which a rebuild recomputes. */
    (void)pthread_mutex_lock(&w->stat_lock);
    out->watches = w->watch_count;
    out->subscriptions = w->sub_count;
    out->priming_complete = w->priming_complete;
    out->budget_total = w->stat_budget_total;
    out->budget_repo = w->stat_budget_repo;
    out->kernel_max = w->stat_kernel_max;
    out->budget_from_policy = w->stat_budget_from_policy;
    out->ignore_refresh_attempts = w->stat_ignore_refresh_attempts;
    out->owed_gaps = w->stat_owed_gaps;
    (void)pthread_mutex_unlock(&w->stat_lock);
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

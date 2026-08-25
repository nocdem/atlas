/* Atlas - incremental reconciliation of one repository.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A0 had one operation: scan everything. A1 needs an operation that costs
 * roughly what changed, because a daemon that reacts to a single keystroke by
 * rehashing a repository is not a daemon anybody can leave running.
 *
 * The pass has four stages, in this order, and the order is the design:
 *
 *   1. observe   — git tells us HEAD, the worktree state, the tracked paths and
 *                  the untracked-but-not-ignored paths. No database write is in
 *                  progress; no transaction is open.
 *   2. select    — for every candidate, lstat it and compare against the
 *                  identity recorded last time. Matching identity means the
 *                  content cannot have changed, so it is not read at all. This
 *                  is the stage that makes the pass incremental.
 *   3. hash      — read and hash only the selected files, across the worker
 *                  pool. Still no transaction.
 *   4. apply     — write the results in bounded batches. Each batch is its own
 *                  transaction, and no git process or file read happens inside
 *                  one.
 *
 * Between stage 3 and stage 4 the HEAD is read again. If it moved, the results
 * describe a repository that no longer exists and the pass is abandoned rather
 * than committed: a branch switch during a pass must never leave the index
 * describing a mixture of two branches.
 *
 * Everything about the pass is bounded, and every bound reports itself.
 */
#ifndef ATLAS_RECONCILE_H
#define ATLAS_RECONCILE_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/code.h"
#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/git.h"
#include "atlas/limits.h"
#include "atlas/workers.h"

/* Candidate paths held in memory during one pass. At roughly 48 bytes of index
 * plus the path and object-id bytes, a full-size pass costs tens of megabytes,
 * which is the documented memory ceiling for reconciliation. Exceeding it
 * truncates *and says so*; it never quietly indexes a prefix. */
#define ATLAS_RECONCILE_MAX_FILES 250000

/* How many times a pass will be restarted because HEAD moved underneath it
 * before Atlas stops trying and records a degraded state. Without a bound, a
 * repository whose branch is being flipped in a loop would spin forever. */
#define ATLAS_RECONCILE_MAX_ATTEMPTS 3

typedef struct atlas_reconcile_opts {
    /* Verify content: ignore every recorded filesystem identity and re-read
     * every eligible file. This is what resolves an event gap, and it is the
     * only thing that may — only a pass that actually read the bytes can say
     * the index matches them.
     *
     * "Full" therefore means content verification, not a thorough stat. A pass
     * that stat'ed every path and read none of them proves nothing about
     * content, because the whole reason a gap exists is that the recorded
     * metadata may no longer describe reality. */
    bool full;

    /* Paths the watcher observed an event for, as NUL-separated repository-
     * relative byte strings. Every one of them is hashed regardless of what its
     * metadata says.
     *
     * An explicit event is evidence that something happened to that file.
     * Metadata equality must never suppress it: the metadata tuple can be made
     * to look unchanged (a same-length write with the mtime restored), and the
     * event is the one signal that cannot be forged away by the writer. */
    const char *dirty_paths;
    size_t dirty_paths_len;
    bool skip_history;
    bool skip_untracked;   /* index tracked files only */
    int64_t max_commits;   /* <= 0 means unlimited */
    uint64_t max_file_bytes; /* 0 means ATLAS_HASH_MAX_FILE_BYTES */
    int64_t max_untracked;   /* 0 means ATLAS_WATCH_MAX_DISCOVER_FILES */
    int timeout_ms;          /* per git invocation; 0 means the daemon default */
    int64_t sync_seq;        /* echoed into the published state for `sync --wait` */
    atlas_workers *workers;  /* NULL hashes serially on the calling thread */

    /* A3. The structural pass is a stage of this one rather than a second
     * pipeline, so it shares the pass's generation, its worker pool and its
     * transaction discipline. */
    bool skip_code;    /* index files and history only */
    bool code_rebuild; /* drop every structural row first and reindex */

    /* P0. A test barrier, called once on the pass's own thread **after hashing
     * and before the staleness check re-reads HEAD**.
     *
     * That window is the one the abandon rule exists for, and it is otherwise
     * unreachable from a test: a branch switch has to land inside it, and racing
     * a real daemon into a few microseconds is not a test, it is a coin toss.
     *
     * NULL in production, and set by nothing a user can reach — no CLI flag, no
     * environment variable, no IPC field, no policy key. Its only caller is
     * `tests/test_branch_switch.c`. With it NULL the pass is byte-for-byte the
     * shipped one; the hook adds no branch that changes what a pass decides,
     * only a point at which a test may act. */
    void (*before_head_recheck)(void *ud);
    void *before_head_recheck_ud;
} atlas_reconcile_opts;

void atlas_reconcile_opts_init(atlas_reconcile_opts *o);

typedef struct atlas_reconcile_summary {
    int64_t generation;
    bool published; /* false when the pass was abandoned as stale */

    /* True when this pass read the content of every eligible file, so its result
     * can be trusted without reference to any earlier observation. Only a pass
     * with this set may clear an event gap. It is derived from what the pass
     * actually did, not from what was asked for. */
    bool content_verified;

    int64_t files_examined;
    int64_t files_hashed;         /* content actually read */
    int64_t files_identity_hit;   /* skipped: filesystem identity unchanged */
    int64_t files_dirty_forced;   /* hashed because the watcher named them */
    int64_t files_racy;           /* observed too recently to record an identity */
    int64_t files_added;
    int64_t files_modified;
    int64_t files_unchanged;
    int64_t files_deleted;
    int64_t files_unreadable;
    int64_t files_unsafe;         /* a path component was a symlink */
    int64_t files_truncated;      /* recorded, but content not fully hashed */

    int64_t untracked_discovered;
    int64_t ignored_paths;        /* reported by git as covered by ignore rules */

    int64_t commits_ingested;
    int64_t commits_seen;
    int64_t changes_ingested;
    bool history_full_replay;
    bool branch_rewrite;

    int64_t events_appended;
    int64_t batches_written;

    /* A3. What the structural stage did, so `sync --json` reports it next to
     * everything else the pass established rather than in a separate command. */
    bool code_ran;
    atlas_code_pass_summary code;

    bool truncated;
    atlas_buf truncated_reason;

    char head_oid[ATLAS_OID_HEX_MAX_INCL];
    char head_state[16];
    char branch[ATLAS_BRANCH_MAX];
    bool dirty;
    int64_t duration_ms;
} atlas_reconcile_summary;

void atlas_reconcile_summary_init(atlas_reconcile_summary *s);
void atlas_reconcile_summary_free(atlas_reconcile_summary *s);

/* Runs one pass. `db` must be a writable handle owned by the calling thread.
 *
 * A pass abandoned because HEAD moved returns ATLAS_OK with
 * `summary->published == false`; that is a normal outcome, not an error, and the
 * caller schedules another pass. */
atlas_status atlas_reconcile_run(atlas_db *db, atlas_git *g, int64_t repo_id,
                                 const atlas_reconcile_opts *opts,
                                 atlas_reconcile_summary *summary, atlas_err *err);

#endif /* ATLAS_RECONCILE_H */

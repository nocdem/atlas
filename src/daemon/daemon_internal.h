/* Atlas - daemon internals shared between the src/daemon translation units.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header.
 */
#ifndef ATLAS_DAEMON_INTERNAL_H
#define ATLAS_DAEMON_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/ai.h"
#include "atlas/buf.h"
#include "atlas/daemon.h"
#include "atlas/db.h"
#include "atlas/decision_ops.h"
#include "atlas/git.h"
#include "atlas/maintenance.h"
#include "atlas/ops.h"
#include "atlas/orch_ops.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/orchpolicy.h"
#include "atlas/plan.h"
#include "atlas/snapshot.h"
#include "atlas/syspolicy.h"
#include "atlas/verify_ops.h"
#include "atlas/error.h"
#include "atlas/limits.h"
#include "atlas/workers.h"

/* --- the writer job queue ------------------------------------------------
 *
 * Bounded, with real backpressure. A queue that grows without limit turns a
 * burst of filesystem events into unbounded memory, and a queue that drops
 * silently turns it into a lost change. Neither is acceptable, so a full queue
 * refuses the submission and the submitter is told.
 *
 * For reconciliation that refusal is harmless: a pending pass for the same
 * repository already covers the change, so a duplicate submission is folded
 * into it rather than queued twice. That is the coalescing that makes a burst of
 * writes cost one pass. */

typedef enum atlas_job_kind {
    ATLAS_JOB_RECONCILE = 0,
    ATLAS_JOB_REPO_ADD,
    ATLAS_JOB_REPO_REMOVE,
    /* A13. Assigning a repository's scanner uid while the daemon runs.
     *
     * A7 made the registry local-only because "the socket carries no authority:
     * every peer on it is the same uid as the daemon". A7.1 ended that premise
     * by splitting the principals, and the operator group this method sits in
     * is selected by SO_PEERCRED against the root-owned policy — the same door
     * `code.index` and `backup.create` already use. So the refusal that made an
     * operator stop the daemon to name a scanner is answered by the mechanism
     * A7.1 built, not by weakening what A7 established. */
    ATLAS_JOB_REPO_SCANNER,
    /* The watcher observes the conditions that produce these but holds a
     * read-only handle, so it hands them to the writer rather than acquiring a
     * second write path into the index. */
    ATLAS_JOB_MARK_GAP,
    ATLAS_JOB_SET_WATCH,
    /* A2. Every AI-session mutation is one job kind carrying one typed
     * operation, rather than a job kind per verb: the validation happens in the
     * IPC layer before anything is queued, and the writer's switch stays a
     * switch rather than becoming a second dispatch table that can drift from
     * the first. */
    ATLAS_JOB_AI,
    /* A4. One job kind carrying one typed decision operation, for exactly the
     * reasons ATLAS_JOB_AI is one: validation happens in the IPC layer before
     * anything is queued, and the writer's switch stays a switch.
     *
     * Every lifecycle transition comes through here, including the ones the
     * operator channel authorises — a challenge is a write, so issuing one is a
     * writer job too. There is no path to `atlas_decision_apply` that does not
     * run on the writer thread. */
    ATLAS_JOB_DECISION,
    /* A8. One job kind carrying one typed orchestration operation, for exactly
     * the reasons ATLAS_JOB_AI and ATLAS_JOB_DECISION are one each: validation
     * and policy happen at the IPC edge before anything is queued, and the
     * writer's switch stays a switch rather than becoming a second dispatch
     * table that can drift from the first.
     *
     * Every orchestration write comes through here, including the recovery
     * sweep the daemon's own timer drives — a sweep is a write, so it is a
     * writer job too. There is no path to `atlas_orch_apply` inside the daemon
     * that does not run on the writer thread. The sweep is pure database work,
     * which is what makes it legal there: A1 forbids creating a process or
     * reading a file inside a write transaction, and recovery does neither. */
    ATLAS_JOB_ORCH,
    /* A8. Snapshot enumeration writes the manifest, so it runs on the writer
     * thread like every other write. It creates a git process, which A1 forbids
     * *inside a transaction* — so `atlas_snapshot_open` opens its transaction
     * only around the rows, after the reads it needs. */
    ATLAS_JOB_SNAPSHOT,
    /* A8-CI closeout. Building a semantic index writes the index, so it happens
     * here and nowhere else — the daemon's one serialized writer path. It is
     * the only job that reports its outcome through the operations table rather
     * than through the completion handshake below, because the caller is told
     * "accepted" long before the work is done and polls for the rest. */
    ATLAS_JOB_SEM_INDEX,
    /* The one bounded delete. It writes, so it happens on this thread; the
     * caller waits, because a prune of an events table is fast and bounded per
     * batch — unlike an index, which is why that one polls instead. */
    ATLAS_JOB_MAINTENANCE,
    /* A9. One gateway audit row, and the credential touch that goes with it.
     *
     * Fire-and-forget: the caller queues it and returns without waiting. A9.6
     * requires that a failure to write the audit trail must not break request
     * handling, and the way that is guaranteed here is that the request path
     * never learns the outcome. Atlas prefers answering with a gap in the trail
     * to refusing a request because it could not write one; the trade is stated
     * in `docs/remote-access.md` rather than left to be discovered.
     *
     * The job owns the entry and frees it, so nothing points at a caller's
     * stack after the queue accepts it. */
    ATLAS_JOB_GW_AUDIT,
    /* A9. One credential operation: create, rotate or revoke.
     *
     * It writes, so it happens on the writer thread like every other write, and
     * the caller waits — a credential operation is a handful of statements and
     * an operator is standing there.
     *
     * It exists at all because revocation must not require stopping the daemon.
     * The local path takes the data-directory writer lock, which the daemon
     * holds while it is running, so on any machine with a live daemon `atlas
     * api-key revoke` could not have worked — and "stop the service to revoke a
     * leaked credential" is not an answer. */
    ATLAS_JOB_APIKEY,
    /* A9.2.1. One job kind carrying one typed verification-intake operation,
     * for exactly the reasons ATLAS_JOB_AI, ATLAS_JOB_DECISION and
     * ATLAS_JOB_ORCH are one each: validation happens at the IPC edge before
     * anything is queued, and the writer's switch stays a switch rather than
     * becoming a second dispatch table that can drift from the first.
     *
     * Every verification write comes through here, including an evaluation —
     * which reads far more than it writes but records a durable result and may
     * spend a warrant, so it is a write and belongs on the one writer thread.
     *
     * It is pure database work, which is what makes it legal here: A1 forbids
     * creating a process or reading a file inside a write transaction, and
     * intake does neither. Every reference it validates is checked against the
     * index rather than by asking git, for exactly that reason. */
    ATLAS_JOB_VERIFY,
    /* A9.2.3: writing a repository's durable semantic build description.
     *
     * A single-row upsert, so it takes the synchronous shape `ATLAS_JOB_MAINTENANCE`
     * uses rather than the operations-table shape an index needs: the caller
     * waits, because there is nothing here that can outlast a client. It is on
     * the writer thread for the ordinary reason — it writes, and exactly one
     * thread in this daemon writes. */
    ATLAS_JOB_SEM_CONFIG,
    /* A9.2.4. One bounded walk of one repository, looking for compilation
     * databases, and the write that records what it found.
     *
     * It is a *separate* job from the index rather than a step inside one,
     * because it has to happen for repositories that are never going to be
     * built: an explicitly disabled repository still reports what Atlas can see,
     * and — more importantly — a repository can only become DIRTY because a new
     * build description appeared if something noticed it appearing. Folding
     * discovery into the index would make the trigger depend on the thing it
     * triggers.
     *
     * On the writer thread because it writes, and because exactly one thread in
     * this daemon writes. The walk itself runs before the transaction opens,
     * which is A1's rule about file reads. */
    ATLAS_JOB_SEM_DISCOVER,
    /* A12.0. One job kind carrying one typed plan operation, for exactly the
     * reasons ATLAS_JOB_AI, ATLAS_JOB_DECISION, ATLAS_JOB_ORCH and
     * ATLAS_JOB_VERIFY are one each: validation and policy happen at the IPC
     * edge before anything is queued, and the writer's switch stays a switch
     * rather than becoming a second dispatch table that can drift from the
     * first.
     *
     * It is a kind of its own rather than a member of ATLAS_JOB_ORCH because
     * `atlas_plan_apply_in_tx` is the plan domain's single write point and has
     * exactly one caller. Reaching it through the orchestration operation would
     * either put plan kinds in `atlas_orch_op_kind` — a second entry into that
     * write point — or add a discriminator to the ORCH case, which is the
     * second dispatch table the comments above exist to prevent.
     *
     * Pure database work, which is what makes it legal here: A1 forbids
     * creating a process or reading a file inside a write transaction, and a
     * plan operation does neither. The planner's bytes come from the job's own
     * stored artifact row, so nothing here opens anything. */
    ATLAS_JOB_PLAN
} atlas_job_kind;


/* One credential operation, as the writer thread receives it. */
typedef enum atlas_apikey_job_kind {
    ATLAS_APIKEY_JOB_CREATE = 0,
    ATLAS_APIKEY_JOB_REVOKE
} atlas_apikey_job_kind;

typedef struct atlas_apikey_job {
    atlas_apikey_job_kind kind;
    /* create/rotate */
    char label[ATLAS_APIKEY_LABEL_MAX + 1];
    atlas_scope_mask scopes;
    char rotate_from[ATLAS_APIKEY_SELECTOR_HEX + 1];
    /* revoke */
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
} atlas_apikey_job;

typedef struct atlas_apikey_job_result {
    /* Holds the one copy of a freshly minted plaintext. The caller wipes it. */
    atlas_apikey_created created;
    bool changed;
} atlas_apikey_job_result;

typedef struct atlas_job atlas_job;

struct atlas_job {
    atlas_job_kind kind;
    int64_t repo_id;
    bool full;         /* reconcile: re-read every file */
    /* A3: drop every structural row and reindex. Distinct from `full`, which is
     * about re-reading file *content*: a full pass rehashes bytes and still
     * parses nothing when the hashes match, so rebuilding the graph needs its
     * own flag rather than being implied by one. */
    bool code_rebuild;
    int64_t sync_seq;  /* echoed into the published state */
    /* P0. The whole outcome of one watch build, carried as one job.
     *
     * It used to be two jobs — a gap note and a state note — and the watcher
     * submitted whichever applied. That was already fragile and A9.2.7 made it
     * unsafe to reason about: FIFO holds within a kind and among drainable
     * kinds, not between two arbitrary kinds, so "the state note lands after the
     * gap note" was an assumption rather than a guarantee. One job writes both,
     * in order, on the writer thread, and the question does not arise. */
    int watch_state;   /* set watch: an atlas_watch_state */
    int watch_reason;  /* set watch: an atlas_watch_reason */
    int64_t watched_source;
    int64_t watched_meta;
    int64_t watched_shared;
    /* Mark an event gap in the same step. The watcher's own state and reason are
     * written *after* the gap flags, so a degraded repository reports why it is
     * degraded rather than the generic 'incomplete' the gap writer would leave. */
    bool watch_mark_gap;
    atlas_buf arg1;    /* repo add: path, repo remove: name, gap/watch: detail */
    atlas_buf arg2;    /* repo add: name (may be empty) */
    /* repo add: refuse unless `arg1` is itself the worktree root. Set by the
     * MCP path, where the caller granted one directory and registering its
     * parent would index what was not granted. */
    bool exact_root;
    /* Reconcile: repository-relative paths the watcher saw an event for, NUL
     * separated. Each is hashed regardless of its metadata. */
    atlas_buf dirty_paths;

    /* Completion handshake, used only by the jobs a caller waits on.
     *
     * The result is carried as typed fields rather than as a JSON fragment. A
     * fragment would have to be spliced into the response document verbatim,
     * and a "write these bytes as JSON" primitive is exactly the hole through
     * which an unescaped value eventually reaches a client. */
    bool wants_result;
    bool done;
    atlas_status result;
    atlas_err result_err;
    int64_t result_id;
    atlas_buf result_name;      /* validated [A-Za-z0-9._-] */
    atlas_buf result_root_text; /* already in the safe text encoding */

    /* A8-CI closeout: the operations-table record this job reports into, and
     * the semantic-index request itself. `arg1` carries the repository name and
     * `arg2` the NUL-separated compilation-database list — bytes, never split
     * on whitespace. */
    int64_t op_id;
    bool sem_rebuild;
    /* ATLAS_JOB_MAINTENANCE: the request, and where the writer puts the result.
     * Both are owned by the caller, which waits for `done`. */
    const atlas_maintenance_opts *maint;
    atlas_maintenance_report *maint_out;

    /* A9. Owned by the job and freed with it — the caller does not wait, so a
     * pointer to its stack would dangle the moment it returned. */
    atlas_gw_audit_entry *gw_audit;

    /* A9. The credential operation, and where the writer puts the result. Both
     * belong to a caller that waits, like the maintenance pair above. */
    const atlas_apikey_job *apikey;

    /* A9.2.3. The build-description write and where the writer puts the state
     * it produced. Both belong to a caller that waits. */
    const atlas_sem_config_job *sem_config;
    atlas_sem_status_report *sem_config_out;
    atlas_apikey_job_result *apikey_out;

    /* A2. The job owns the operation and frees it. The result is typed for the
     * same reason the fields above are: a JSON fragment carried through here
     * would have to be spliced into a response verbatim. */
    atlas_ai_op *ai;
    atlas_ai_result ai_result;

    /* A4. Same ownership rule as `ai`: the job owns the operation and frees it,
     * and the result is typed rather than a JSON fragment. */
    atlas_decision_op *decision;
    atlas_decision_result decision_result;

    /* A8. Same ownership rule as `ai` and `decision`: the job owns the
     * operation and frees it, and the result is typed rather than a JSON
     * fragment. */
    atlas_orch_op *orch;
    atlas_orch_result orch_result;

    /* A9.2.1. Same ownership rule as `ai`, `decision` and `orch`: the job owns
     * the operation and frees it, and the result is typed rather than a JSON
     * fragment. */
    atlas_verify_op *verify;
    atlas_verify_intake_result *verify_result;

    /* A12.0. Same ownership rule again: the job owns the operation and the
     * result slot and frees both, and the result is typed rather than a JSON
     * fragment. The result is handed back on the **failure** path too, which is
     * this pair's one difference from the four above it: a refused planner
     * document is a typed answer, and the sentence and line that describe it are
     * exactly what the caller came for. */
    atlas_plan_op *plan;
    atlas_plan_result *plan_result;

    /* A8 snapshot enumeration. */
    int64_t snapshot_attempt_id;
    struct atlas_snapshot_meta *snapshot_meta;
};

/* What a completed mutation reports back. */
typedef struct atlas_writer_result {
    int64_t id;
    atlas_buf name;
    atlas_buf root_text;
} atlas_writer_result;

void atlas_writer_result_init(atlas_writer_result *r);
void atlas_writer_result_free(atlas_writer_result *r);
/* A13. When each repository's scanner was last heard from.
 *
 * **In memory, not in the index.** A heartbeat is liveness, not a durable fact:
 * a daemon that has just started has heard from nobody, which is exactly the
 * conservative answer, and persisting one would let a restarted daemon trust a
 * scanner that died before it. It also keeps `scanner.poll` a read — the method
 * runs on a read-only handle, and writing from there is what the suite caught.
 *
 * One mutex, no writer and no I/O, exactly like the watcher's live view.
 * Bounded: repositories a daemon serves are few, and an unknown one reads as
 * "never heard from", which only ever subtracts. */
typedef struct atlas_scanner_seen atlas_scanner_seen;

atlas_scanner_seen *atlas_scanner_seen_new(void);
void atlas_scanner_seen_free(atlas_scanner_seen *s);
/* Records that `repo_id`'s scanner asked what is owed, now. */
void atlas_scanner_seen_touch(atlas_scanner_seen *s, int64_t repo_id);
/* Milliseconds since it last did, or a negative value when it never has. */
int64_t atlas_scanner_seen_age_ms(atlas_scanner_seen *s, int64_t repo_id);

typedef struct atlas_writer atlas_writer;

/* Starts the writer thread. It opens its own writable database handle from
 * `db_path`; no handle is passed in, because a handle created on another thread
 * and used here is exactly the sharing the model forbids. */
atlas_status atlas_writer_start(const char *db_path, const char *data_dir,
                                const char *socket_path, atlas_workers *workers, FILE *log,
                                atlas_writer **out, atlas_err *err);
void atlas_writer_stop(atlas_writer *w);

/* Queues a reconciliation. Coalesces with an already-pending pass for the same
 * repository. `sync_seq_out` receives the sequence number the completed pass
 * will publish, so a caller can wait for it.
 *
 * `dirty_paths` names repository-relative paths the caller observed an event
 * for, NUL separated; each is hashed regardless of its metadata. Coalescing
 * merges the sets, because a pending pass has to cover both requests. */
atlas_status atlas_writer_submit_reconcile(atlas_writer *w, int64_t repo_id, bool full,
                                           bool code_rebuild,
                                           const char *dirty_paths, size_t dirty_len,
                                           int64_t *sync_seq_out, atlas_err *err);

/* Queues a mutation and waits for it, bounded by `timeout_ms`. */
/* A8-CI closeout: queue a semantic index on the writer thread and return at
 * once. The outcome is recorded in the operations table under `op_id`; nothing
 * blocks on it here, which is what keeps the caller's answer immediate. */
/* Runs one bounded prune on the writer thread and waits for it. */
/* A9. Queues one gateway audit row and returns immediately.
 *
 * The caller never waits and never learns the outcome, which is what makes
 * "audit failure does not break request handling" structural rather than
 * something every call site has to remember. The entry is copied. */
atlas_status atlas_writer_gw_audit(atlas_writer *w, const atlas_gw_audit_entry *entry,
                                   atlas_err *err);

/* A9. One credential operation, on the writer thread, with the caller waiting. */
atlas_status atlas_writer_apikey(atlas_writer *w, const atlas_apikey_job *op,
                                 atlas_apikey_job_result *out, atlas_err *err);

/* A9.2.3. Queues a build-description write and waits for it, like a prune. */
atlas_status atlas_writer_sem_config(atlas_writer *w, const atlas_sem_config_job *job,
                                     atlas_sem_status_report *out, atlas_err *err);

atlas_status atlas_writer_maintenance(atlas_writer *w, const atlas_maintenance_opts *opts,
                                      atlas_maintenance_report *out, atlas_err *err);

atlas_status atlas_writer_submit_sem_index(atlas_writer *w, int64_t repo_id, const char *name,
                                           const char *compdbs, size_t compdbs_len, bool rebuild,
                                           int64_t op_id, atlas_err *err);

/* A9.2.4. Queues one bounded walk for compilation databases.
 *
 * Fire-and-forget: nobody polls for it, and what it produces is a durable
 * candidate list. Coalesced against the queue, because a walk of a repository
 * already waiting to be walked adds nothing and a slow writer must not
 * accumulate a backlog of identical tree walks. */
atlas_status atlas_writer_submit_sem_discover(atlas_writer *w, int64_t repo_id, const char *name,
                                              atlas_err *err);

/* A9.2.3. Whether a semantic index for this repository is queued or running.
 *
 * The scheduler's one guard against queueing a second build. The durable record
 * cannot answer it — a job dequeued but not yet at the point of opening a
 * generation leaves no RUNNING row — and a flag on the scheduler's side cannot
 * either, because nothing tells the scheduler when a job finished. The writer
 * owns the queue and runs the job, so the writer is the only place that knows. */
bool atlas_writer_sem_index_pending(atlas_writer *w, int64_t repo_id);

/* Hands the writer the operations table. Called once at startup, after both
 * exist and before the serve loop accepts anything. */
void atlas_writer_set_ops(atlas_writer *w, atlas_ops *ops);

atlas_status atlas_writer_call(atlas_writer *w, atlas_job_kind kind, const char *arg1,
                               const char *arg2, int timeout_ms, atlas_writer_result *result,
                               atlas_err *err);
/* The same, for a repository registration that must not resolve upward. See
 * `atlas_job.exact_root`. */
atlas_status atlas_writer_call_repo_add(atlas_writer *w, const char *path, const char *name,
                                        bool exact_root, int timeout_ms,
                                        atlas_writer_result *result, atlas_err *err);
/* A13. Names a repository's scanner uid while the daemon runs. `uid_text` empty
 * derives it from the root's owner. */
atlas_status atlas_writer_call_repo_scanner(atlas_writer *w, const char *name,
                                            const char *uid_text, int timeout_ms,
                                            atlas_writer_result *result, atlas_err *err);

/* Queues one AI-session operation and waits for it, bounded by `timeout_ms`.
 *
 * Takes ownership of `op` unconditionally — including on the failure paths —
 * so a caller has exactly one thing to do with it. A correlate operation may ask
 * for a reconciliation; the writer supplies that callback itself, which is what
 * keeps src/ai free of any knowledge of the job queue. */
atlas_status atlas_writer_ai(atlas_writer *w, atlas_ai_op *op, int timeout_ms,
                             atlas_ai_result *result, atlas_err *err);

/* Queues one decision-lifecycle operation and waits for it, bounded by
 * `timeout_ms`.
 *
 * Takes ownership of `op` unconditionally, including on the failure paths, so a
 * caller has exactly one thing to do with it — the same contract
 * `atlas_writer_ai` has, and for the same reason: an ownership rule that
 * depends on the outcome is one that leaks on the path nobody tests. */
/* A8. Ownership of `op` is taken unconditionally, as everywhere else: a caller
 * that has to free the operation on some paths and not others eventually frees
 * it on the wrong one. */
atlas_status atlas_writer_orch(atlas_writer *w, atlas_orch_op *op, int timeout_ms,
                               atlas_orch_result *result, atlas_err *err);

/* A9.2.1. Ownership of `op` is taken unconditionally, as everywhere else. The
 * result is moved rather than copied field by field, so a field added to
 * `atlas_verify_intake_result` cannot silently stay zero on the daemon path
 * while the local path reports it — which is the A9.1 defect this shape avoids
 * rather than remembers to avoid. */
atlas_status atlas_writer_verify(atlas_writer *w, atlas_verify_op *op, int timeout_ms,
                                 atlas_verify_intake_result *result, atlas_err *err);

/* A12.0. Ownership of `op` is taken unconditionally, as everywhere else, and the
 * result is moved rather than copied field by field for `atlas_writer_verify`'s
 * reason.
 *
 * **The result is handed back on the failure path too**, which is what makes it
 * different from every other wrapper here. A refused planner document returns a
 * non-OK status *and* fills `refusal` and `refusal_line`, and those are the
 * whole answer: the driver composes a retry prompt out of Atlas' own refusal
 * sentence and the line it happened on. A wrapper that moved the result only on
 * success would drop them on the writer thread and leave the caller with prose
 * to parse. */
atlas_status atlas_writer_plan(atlas_writer *w, atlas_plan_op *op, int timeout_ms,
                               atlas_plan_result *result, atlas_err *err);

/* A8. Enumerates and persists one attempt's snapshot manifest on the writer
 * thread. Idempotent per attempt. */
atlas_status atlas_writer_snapshot(atlas_writer *w, int64_t attempt_id, int timeout_ms,
                                   struct atlas_snapshot_meta *out, atlas_err *err);

atlas_status atlas_writer_decision(atlas_writer *w, atlas_decision_op *op, int timeout_ms,
                                   atlas_decision_result *result, atlas_err *err);

/* Records that changes may have been missed for a repository. Fire and forget:
 * the watcher has nothing useful to do with a failure here, and the periodic
 * full pass covers the case where the flag did not get written. */
atlas_status atlas_writer_submit_gap(atlas_writer *w, int64_t repo_id, const char *detail,
                                     atlas_err *err);
/* P0. Records everything one watch build established for a repository, and
 * optionally marks an event gap in the same step.
 *
 * `mark_gap` is not a convenience: a watch set that could not be installed
 * completely is a tree whose events are being missed, which is the definition of
 * a gap, and writing the two facts separately is how they used to disagree. */
atlas_status atlas_writer_submit_watch_outcome(atlas_writer *w, int64_t repo_id,
                                               const atlas_watch_outcome *outcome, bool mark_gap,
                                               atlas_err *err);

/* --- how a job kind is classified ----------------------------------------
 *
 * Two questions, asked of the kind and never of elapsed time, and every member
 * of `atlas_job_kind` has to answer both. Neither switch has a `default:`, so a
 * new kind does not compile until somebody has decided.
 *
 * They are declared here rather than left file-static in `writer.c` because the
 * agreement test walks the whole enum and asks both of every member: the two
 * classifications must not overlap where they cannot — an unbounded job is never
 * drainable — and the drainable set must be the one the documentation names. Two
 * spellings of one rule drift, and a drift nothing compares is one nobody sees.
 * The reasoning for each answer is written at its case in `writer.c`, and the
 * checklist for adding a kind is in `docs/extending.md`. */

/* Whether a job of this kind has a duration Atlas can state. `true` for the two
 * that run a compiler or walk a whole tree; a waiter that observes one gives it
 * `ATLAS_WRITER_YIELD_GRACE_MS` to yield and then reports a refusal. */
bool job_kind_is_unbounded(atlas_job_kind kind);

/* Whether a job of this kind may be run inside a yield of an unbounded one.
 * `true` for the latency-critical writes whose tables are disjoint from what a
 * semantic pass or a discovery walk touches. */
bool job_kind_is_drainable(atlas_job_kind kind);

/* Depth of the queue right now, for `daemon status`. */
/* P0. The writer's test channel. Declared here and nowhere else: no CLI flag,
 * no environment variable, no RPC method, no MCP tool, no policy key, and no
 * caller outside `tests/`. See the block comment on the fields in `writer.c`
 * for why the two properties it exists for cannot be asserted without it.
 *
 * `atlas_writer_test_stall` holds the writer thread *inside* a job of the named
 * kind — the claim taken, the queue behind it untouched — until
 * `atlas_writer_test_release`, which is what "the writer is busy with a
 * long-running job" looks like from every other thread's point of view.
 * `atlas_writer_test_fail_watch_writes` makes the next `n` `SET_WATCH` jobs
 * reach the database write and not perform it. */
void atlas_writer_test_stall(atlas_writer *w, atlas_job_kind kind);
void atlas_writer_test_release(atlas_writer *w);
/* True while the writer thread is actually being held. A test that only armed
 * the stall has not yet blocked anything: the job it stalls has to be dequeued
 * first, and everything a test wants to keep out of the database has to happen
 * after that, not after the arming. */
bool atlas_writer_test_stalled(atlas_writer *w);
void atlas_writer_test_fail_watch_writes(atlas_writer *w, int64_t n);
int64_t atlas_writer_test_watch_writes_failed(atlas_writer *w);

int64_t atlas_writer_queue_depth(atlas_writer *w);
/* Reconciliation passes completed since start, for `daemon status`. */
int64_t atlas_writer_passes(atlas_writer *w);

/* Set by the writer when a repository set change means the watcher must
 * re-derive its watches. The watcher clears it. */
bool atlas_writer_take_watch_dirty(atlas_writer *w);
void atlas_writer_set_watch_dirty(atlas_writer *w);

/* --- the watcher --------------------------------------------------------- */

typedef struct atlas_watcher atlas_watcher;

/* Starts the inotify watcher. It opens its own read-only database handle to
 * enumerate repositories, and submits reconciliations through `writer`. */
/* P0. Everything the watcher is started with, as a struct rather than six
 * positional arguments.
 *
 * The four `inject_` fields are the **test channel**, and they are deliberately
 * not reachable from anywhere a user is: no CLI flag, no environment variable,
 * no RPC method, no policy key. A public way to lower a watch budget or a
 * repository ceiling would be a second answer to questions
 * `/etc/atlas/system.conf` and `include/atlas/limits.h` already own, reachable
 * by anyone who can start a daemon — which is not the same set of people who
 * can edit a root-owned file.
 *
 * Zero means "use the production value" for every one of them, so a caller that
 * memsets the struct gets production behaviour exactly. A non-zero value
 * replaces the resolved bound **and nothing else**: the comparison, the
 * allocation rounds, the accounting and the reason codes are the shipped ones.
 * That is what makes a boundary test evidence about production rather than
 * about a test-only path. */
typedef struct atlas_watcher_opts {
    /* Arms the orchestration recovery sweep on the watcher's timer. False on
     * every daemon that is not serving the system index under an active policy,
     * so a fixture or ad-hoc daemon never sweeps. */
    bool orch_enabled;
    /* Whether this daemon serves the system index, which decides its share of
     * the kernel's per-uid inotify budget. */
    bool system_deployment;
    int reconcile_interval_ms; /* 0 selects ATLAS_WATCH_RECONCILE_INTERVAL_MS */

    int64_t inject_budget_total;         /* 0 -> resolved from policy and kernel */
    size_t inject_max_repos;             /* 0 -> ATLAS_WATCH_MAX_REPOS */
    size_t inject_max_pending_ignore;    /* 0 -> ATLAS_WATCH_MAX_PENDING_IGNORE */
    size_t inject_max_pending_ignore_bytes; /* 0 -> ..._BYTES */
    int64_t inject_publish_failures;        /* refuse this many watch-state publications */
} atlas_watcher_opts;

void atlas_watcher_opts_init(atlas_watcher_opts *o);

atlas_status atlas_watcher_start(const char *db_path, const char *data_dir,
                                 atlas_writer *writer, FILE *log,
                                 const atlas_watcher_opts *opts, atlas_watcher **out,
                                 atlas_err *err);
void atlas_watcher_stop(atlas_watcher *w);
/* P0. What `daemon status` reports about the watch set.
 *
 * `watches` counts **physical** inotify descriptors and is what the kernel
 * holds. `subscriptions` counts (repository, descriptor) pairs and is what the
 * per-repository figures sum to. They are equal only when nothing is shared, and
 * two registered worktrees of one repository share every descriptor on their
 * common git directory — so `subscriptions >= watches` is the relation, and any
 * surface that claims equality is wrong for exactly the case worktrees create. */
typedef struct atlas_watch_stats {
    int64_t watches;       /* physical descriptors */
    int64_t subscriptions; /* logical (repo, descriptor) pairs */
    int64_t budget_total;
    int64_t budget_repo;
    int64_t kernel_max;
    bool budget_from_policy;
    bool priming_complete;
    /* P0. Cumulative `git ls-files --ignored` invocations across all
     * repositories. A backoff is only real if something counts the attempts it
     * is meant to prevent. */
    int64_t ignore_refresh_attempts;
    /* Repositories owing an event gap not yet observed in the database. */
    int64_t owed_gaps;
} atlas_watch_stats;

/* P0. What the watcher believes about one repository *now*, as opposed to what
 * the database was last told.
 *
 * `known` is false for a repository this watcher does not observe, in which case
 * the caller has nothing to overlay and uses the stored row unchanged. */
typedef struct atlas_watch_live {
    bool known;
    bool priming;
    bool degraded;
    bool owes_gap;
} atlas_watch_live;

void atlas_watcher_repo_live(atlas_watcher *w, int64_t repo_id, atlas_watch_live *out);

void atlas_watcher_stats(atlas_watcher *w, atlas_watch_stats *out);
/* True once the initial reconciliation of every repository has been submitted. */
bool atlas_watcher_primed(atlas_watcher *w);

/* --- the IPC serve loop -------------------------------------------------- */

typedef struct atlas_server_ctx {
    const char *db_path;
    const char *data_dir;
    const char *socket_path;
    atlas_writer *writer;
    atlas_watcher *watcher;
    /* A13. Scanner liveness, in memory. See `atlas_scanner_seen`. */
    atlas_scanner_seen *scanner_seen;
    atlas_workers *workers;
    /* Long-running operations that must not run inside the serve loop. See
     * atlas/ops.h: a backup or a semantic index takes longer than a client will
     * hold a socket open, and running one here stalled every other client. */
    atlas_ops *ops;
    FILE *log;
    int64_t started_at_ms;
    /* A7.1. Loaded once at startup and never reloaded, so the set of uids the
     * daemon will accept cannot change under a running serve loop — a policy
     * edit takes effect on restart, which is a fact an operator can reason
     * about. Legacy per-user mode leaves this zeroed, which permits nobody
     * beyond the daemon's own uid. */
    atlas_syspolicy syspolicy;
    /* A8. Loaded once at startup, for the reason the system policy is: the set
     * of repositories, drivers, modes and principals orchestration runs under
     * cannot change under a running serve loop, so a policy edit takes effect on
     * restart and an operator can reason about when. A disabled policy — the
     * zeroed default — leaves every orchestration method refusing. */
    atlas_orchpolicy orchpolicy;
    /* A9. Loaded once at startup, for the reason the other two are: the uid the
     * daemon will recognise as the gateway cannot change under a running serve
     * loop, so a policy edit takes effect on restart and an operator can reason
     * about when. A disabled policy — the zeroed default — leaves `gateway_uid`
     * at zero, and zero matches no peer, so the `gateway.` group is offered to
     * nobody. */
    atlas_gwpolicy gwpolicy;
} atlas_server_ctx;

/* Serves until `stop` becomes true. `listen_fd` is owned by the caller. */
atlas_status atlas_server_serve(atlas_server_ctx *ctx, int listen_fd, int signal_fd,
                                atomic_bool *stop, atlas_err *err);

/* Handles one request payload and produces one response payload. Exposed so the
 * protocol can be tested without a socket. */
atlas_status atlas_server_dispatch(atlas_server_ctx *ctx, const void *payload, size_t len,
                                   int64_t peer_uid, int64_t peer_pid,
                                   atlas_buf *response, atlas_err *err);

/* --- logging ------------------------------------------------------------- */

/* One line, timestamped, with every untrusted value already safe-encoded by the
 * caller. Never NULL-checks away a message: a dropped log line is a dropped
 * explanation. */
void atlas_daemon_log(FILE *log, const char *level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* ATLAS_DAEMON_INTERNAL_H */

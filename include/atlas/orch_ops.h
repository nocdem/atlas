/* Atlas - A8: the one typed orchestration operation, and its one write point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This mirrors `atlas/decision_ops.h` on purpose, and for the same reason.
 * Every orchestration mutation is one operation, validated at the IPC edge
 * before anything is queued, carried to the writer thread as a typed value, and
 * applied by exactly one function. There is no second path into the tables.
 *
 * `atlas_orch_apply_in_tx` is that function. The transition check, the lease
 * check, the attempt-number allocation, the ledger append and the status-cache
 * update all live behind it, and every one would be bypassable if a second
 * implementation reached the tables. `settle()` in A3, `atlas_db_evidence_insert`
 * in A0 and `atlas_decision_apply_in_tx` in A4 follow the same rule.
 *
 * Nothing here can approve a decision, change the registry, read a backup or
 * touch any table outside the eight `orch_*` ones. That is not a promise about
 * the code; it is the whole content of the file, and `tests/test_orch_trust.c`
 * asserts it against a real database after a full job lifecycle.
 */
#ifndef ATLAS_ORCH_OPS_H
#define ATLAS_ORCH_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "atlas/orch.h"

typedef struct atlas_db atlas_db;

typedef enum atlas_orch_op_kind {
    /* Zero is not an operation. A zeroed op applies nothing. */
    ATLAS_ORCH_OP_NONE = 0,
    /* A client creates a job. The specification has already been resolved
     * against the registry and the policy by the time it arrives here. */
    ATLAS_ORCH_OP_SUBMIT,
    /* A client asks for cancellation. Race-safe: immediate for a queued job,
     * a request for an active one, and refused for a terminal one. */
    ATLAS_ORCH_OP_CANCEL,
    /* The dispatcher asks for work. Grants at most one lease, creating the
     * attempt in the same transaction. */
    ATLAS_ORCH_OP_LEASE,
    /* The dispatcher renews its lease and reports its phase. This is also how
     * it learns that cancellation was requested. */
    ATLAS_ORCH_OP_HEARTBEAT,
    /* The dispatcher appends one structured event. */
    ATLAS_ORCH_OP_EVENT,
    /* The dispatcher reports a terminal outcome for the attempt it holds. */
    ATLAS_ORCH_OP_COMPLETE,
    /* Atlas itself: expire leases, enforce deadlines, decide retry or recovery.
     * There is no request that produces this — it is driven by the daemon's own
     * timer and by startup reconciliation, so nothing outside Atlas can ask for
     * a job to be expired. */
    ATLAS_ORCH_OP_RECOVER
} atlas_orch_op_kind;

/* One artifact as the worker described it. `content` is present only when the
 * worker sent the bytes inline, which it does only below
 * ATLAS_ORCH_ARTIFACT_INLINE_MAX; a larger artifact is recorded by name, size
 * and digest and its bytes stay in the workspace. There is no path field: an
 * artifact is addressed by its server-assigned id and never by a path a client
 * or a worker chose. */
typedef struct atlas_orch_artifact {
    atlas_buf name;
    atlas_buf kind;
    atlas_buf sha256;
    int64_t size_bytes;
    atlas_buf content;
    bool content_stored;
} atlas_orch_artifact;

void atlas_orch_artifact_init(atlas_orch_artifact *a);
void atlas_orch_artifact_free(atlas_orch_artifact *a);

typedef struct atlas_orch_op {
    atlas_orch_op_kind kind;

    /* Trusted connection facts, filled in at the IPC edge from SO_PEERCRED and
     * from nowhere else. A request that contains a `uid` member is describing
     * itself to a field Atlas does not have. */
    long long peer_uid;
    atlas_orch_actor actor;

    /* SUBMIT. The op owns the specification. `repo_id` and `repo_root` were
     * resolved from the registry by the caller: the worker never names a
     * repository path and a client never supplies one. */
    atlas_orch_spec spec;
    int64_t repo_id;
    atlas_buf repo_root;

    /* CANCEL, and every read that names a job. */
    atlas_buf job_uid;

    /* LEASE. A stable identifier for the dispatcher process, recorded so a
     * restart is visible in the history. It is not an authorisation. */
    atlas_buf dispatcher_id;
    /* LEASE. Which drivers this dispatcher will run, as a netstring-encoded
     * list. Empty means "any".
     *
     * A8.1 runs two dispatchers on one queue: the untrusted `atlas-worker` one
     * takes everything that needs no model, and the operator's takes only what
     * does. The filter is what keeps them from stealing each other's work, and
     * the daemon applies it to the job's *stored* driver — never to anything the
     * worker said about the job. */
    atlas_buf lease_drivers;

    /* HEARTBEAT, EVENT, COMPLETE: the bearer capability. Compared by digest and
     * never stored, logged or reported. */
    atlas_buf token;

    /* HEARTBEAT: the phase the worker says it has reached. Validated against the
     * transition table like any other transition — a worker cannot skip
     * PREPARING, and cannot go backwards. */
    atlas_orch_state phase;
    int64_t claimed_pid;

    /* EVENT. `seq` is the worker's own counter; the unique index refuses a
     * repeat, which is what makes a duplicated delivery harmless. */
    int64_t event_seq;
    atlas_buf event_kind;
    atlas_buf event_payload;

    /* COMPLETE. */
    bool success;
    atlas_orch_exit_kind exit_kind;
    int64_t exit_code;
    atlas_orch_reason failure_reason;
    atlas_buf driver_version;

    /* COMPLETE, A11.1. Which of the job's **stored** validation commands failed,
     * as a zero-based index into them, or -1 when none did. An index rather
     * than a name: the follow-up task is told which gate failed by rendering
     * the argv Atlas has on the job row, so a caller cannot name a gate the job
     * never declared. Out-of-range is refused, not clamped.
     *
     * `failure_detail` is the bounded excerpt of what that gate actually
     * printed. It is UNTRUSTED_DATA — a compiler's or a test runner's output —
     * and it is used for exactly one thing: quoting into the follow-up task's
     * text, where it is already labelled as such. No branch reads it. */
    int64_t failed_gate;
    atlas_buf failure_detail;
    atlas_orch_artifact *artifacts;
    size_t artifact_count;

    /* RECOVER. The current wall-clock instant, supplied by the caller rather
     * than read here, so a test can drive expiry deterministically instead of
     * sleeping. Zero means "now". */
    int64_t now_ms;

    /* A11.5a-R. RECOVER. The instant this caller was last refused a write
     * because the daemon was busy with something unbounded, or zero if it has
     * not been refused recently.
     *
     * A lease is a liveness proxy, and a heartbeat is an ordinary synchronous
     * write. A9.2.6 refuses those for the whole of a semantic pass, so a worker
     * that is alive and healthy can be unable to say so for minutes at a time —
     * and then be judged for not having said so. This field is how the sweep
     * learns that the silence was Atlas' own, not the worker's.
     *
     * Zero is the old behaviour exactly, which is why every existing caller and
     * every existing test is unchanged by this field's existence. */
    int64_t contended_until_ms;
} atlas_orch_op;

atlas_orch_op *atlas_orch_op_new(atlas_orch_op_kind kind);
void atlas_orch_op_free(atlas_orch_op *op);

/* What an applied operation reports back. Typed rather than a JSON fragment,
 * for the reason A2 gives: a fragment would have to be spliced into a response
 * verbatim, and a "write these bytes as JSON" primitive is exactly the hole
 * through which an unescaped value eventually reaches a client. */
typedef struct atlas_orch_result {
    int64_t job_id;
    atlas_buf job_uid;
    /* A11.0. The run this job belongs to. On a duplicate it is the run the
     * original submission settled, because a duplicate creates nothing. */
    atlas_buf run_uid;
    atlas_orch_state state;
    int64_t attempt_id;
    int64_t attempt_no;
    /* The ledger id of the transition this operation recorded, and the ordering
     * authority for everything that follows it. Zero when nothing changed. */
    int64_t seq;

    /* SUBMIT: this key had already produced a job with the same digest, so no
     * second job was created. */
    bool duplicate;
    /* LEASE: whether any work was available. False is the ordinary answer on an
     * idle queue and is not an error. */
    bool granted;
    /* HEARTBEAT: an operator has asked for cancellation and the worker must
     * stop. The worker learns it here rather than being signalled. */
    bool cancel_requested;

    /* COMPLETE, A11.1. What this completion did to the run the job belongs to.
     * `run_status` is the run's status *after* the operation — ACTIVE when it
     * is still open, and one of the two terminal answers when this completion
     * settled it. UNKNOWN when the job belongs to no run at all, which is every
     * job submitted before migration 21.
     *
     * `follow_up_job_uid` names the task this completion created, and is empty
     * when it created none — because the run was settled, because the budget
     * was spent, or because a resumed completion found the follow-up already
     * there. A caller reads it; nothing decides from it. */
    atlas_orch_run_status run_status;
    atlas_buf follow_up_job_uid;
    /* How many worker starts this run has spent, after this operation. */
    int64_t worker_starts;

    /* LEASE only, and once: the bearer token, returned at grant and never
     * retrievable afterwards. */
    atlas_buf token;
    int64_t expires_ms;

    /* The job payload a granted worker needs. Every field comes from trusted
     * Atlas state; none of it is echoed back from anything the worker sent. */
    atlas_buf repo_name;
    atlas_buf repo_root;
    atlas_buf source_commit;
    atlas_buf mode;
    atlas_buf driver;
    atlas_buf task_text;
    atlas_buf allowed_paths;
    atlas_buf validations;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_output_bytes;
    int64_t max_artifact_bytes;
    int64_t max_artifact_count;
    char spec_digest[65];

    /* RECOVER: how many jobs each outcome applied to, for the daemon log and
     * for `atlas job recover`. */
    int64_t expired;
    int64_t retried;
    int64_t timed_out;
    int64_t recovered;
    /* A11.5a-R. Expired leases this sweep declined to judge, because the daemon
     * had recently refused writes and the holder may have been unable to
     * heartbeat. Reported rather than silent: a sweep that keeps deferring is a
     * machine under sustained contention, which an operator should be able to
     * see without reading the lease table. */
    int64_t deferred;
} atlas_orch_result;

void atlas_orch_result_init(atlas_orch_result *r);
void atlas_orch_result_free(atlas_orch_result *r);

/* The public entry point: begin, apply, commit, with rollback on failure. Call
 * with no transaction open. */
atlas_status atlas_orch_apply(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                              atlas_err *err);

/* The single write point. Assumes the caller owns a transaction.
 *
 * It has exactly one caller — `atlas_orch_apply` — and adding a second means
 * arguing that it genuinely owns a wider unit of work, which is the argument A4
 * requires for `atlas_decision_apply_in_tx`. Adding a second *implementation* is
 * what the rule forbids. */
atlas_status atlas_orch_apply_in_tx(atlas_db *db, const atlas_orch_op *op,
                                    atlas_orch_result *out, atlas_err *err);

/* --- reads ------------------------------------------------------------------
 *
 * Every read is bounded and every list says whether more rows exist. A page that
 * silently ends is indistinguishable from the end of the list, which is the same
 * complaint A1 makes about `events.since`. */

#define ATLAS_ORCH_TS_MAX 32u

typedef struct atlas_orch_job_view {
    char job_uid[ATLAS_ORCH_UID_MAX];
    /* A11.0. The run this task belongs to, and the task it followed. Both are
     * empty for a job submitted before migration 21; `run_uid` empty means the
     * job belongs to no run, never that it is the root of one. */
    char run_uid[ATLAS_ORCH_RUN_UID_MAX];
    char parent_job_uid[ATLAS_ORCH_UID_MAX];
    atlas_orch_state state;
    char repo_name[ATLAS_ORCH_NAME_MAX + 1u];
    char source_commit[41];
    char mode[ATLAS_ORCH_NAME_MAX + 1u];
    char driver[ATLAS_ORCH_NAME_MAX + 1u];
    char spec_digest[65];
    char correlation[ATLAS_ORCH_NAME_MAX + 1u];
    long long submitter_uid;
    int64_t attempts_started;
    int64_t max_attempts;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t state_seq;
    char created_at[ATLAS_ORCH_TS_MAX];
    char terminal_at[ATLAS_ORCH_TS_MAX];
    bool cancel_requested;
    /* UNTRUSTED_DATA. Safe-encoded by the renderer, labelled wherever it
     * reaches a model, and never placed in automatic context. */
    atlas_buf task_text;
} atlas_orch_job_view;

void atlas_orch_job_view_init(atlas_orch_job_view *v);
void atlas_orch_job_view_free(atlas_orch_job_view *v);

atlas_status atlas_db_orch_job_get(atlas_db *db, const char *uid, atlas_orch_job_view *out,
                                   bool *found, atlas_err *err);

/* --- the run (A11.0) --------------------------------------------------------
 *
 * A run is read whole: its identity, its root, the repository it is bound to,
 * its status, and the one task in it that is still active. `active_job_uid` is
 * part of the view rather than a second query because "which task is this run
 * waiting on?" is the question a caller resuming after a restart actually has,
 * and answering it in two reads would let the two disagree.
 */
typedef struct atlas_orch_run_view {
    char run_uid[ATLAS_ORCH_RUN_UID_MAX];
    char root_job_uid[ATLAS_ORCH_UID_MAX];
    char repo_identity_hash[65];
    atlas_orch_run_status status;
    /* The run's single non-terminal task, or empty when it has none. Empty is
     * an ordinary answer — a run between tasks — and never an error. */
    char active_job_uid[ATLAS_ORCH_UID_MAX];
    atlas_orch_state active_state;
    char created_at[ATLAS_ORCH_TS_MAX];
} atlas_orch_run_view;

atlas_status atlas_db_orch_run_get(atlas_db *db, const char *run_uid, atlas_orch_run_view *out,
                                   bool *found, atlas_err *err);

/* Settles a run, as a compare-and-swap. `observed` is the status the caller
 * believes the run holds and the update applies only if it still does, so two
 * callers racing to settle one run cannot both succeed — A8's rule for every
 * orchestration state change, and A4's before it.
 *
 * Only ACTIVE -> ACCEPTED and ACTIVE -> BLOCKED are permitted. A terminal run is
 * final: nothing reopens it, and there is no edge between the two terminal
 * answers.
 *
 * **A11.0 calls this from no automatic path.** Nothing in this milestone decides
 * that a run is accepted or blocked — not a succeeding task, not a failing one.
 * It exists so that a caller which has decided can record the decision, and the
 * question of who may decide is deliberately left to A11.1. There is no RPC
 * method, no MCP tool and no gateway route that reaches it, which is what makes
 * "a model payload cannot accept a run" true by absence rather than by a check. */
atlas_status atlas_db_orch_run_set_status(atlas_db *db, const char *run_uid,
                                          atlas_orch_run_status observed,
                                          atlas_orch_run_status want, atlas_err *err);

typedef struct atlas_orch_list_row {
    int64_t id;
    char job_uid[ATLAS_ORCH_UID_MAX];
    atlas_orch_state state;
    char repo_name[ATLAS_ORCH_NAME_MAX + 1u];
    char driver[ATLAS_ORCH_NAME_MAX + 1u];
    char created_at[ATLAS_ORCH_TS_MAX];
    int64_t attempts_started;
} atlas_orch_list_row;

/* Row callbacks receive borrowed pointers valid only for the call, as
 * everywhere else in Atlas. Copy anything that must outlive it. */
typedef atlas_status (*atlas_orch_list_cb)(const atlas_orch_list_row *row, void *ud,
                                           atlas_err *err);

/* Scoped to one submitter. A client sees its own jobs and nothing else; there is
 * no query that returns another principal's work. */
atlas_status atlas_db_orch_job_list(atlas_db *db, long long submitter_uid, int64_t after_id,
                                    int64_t limit, atlas_orch_list_cb cb, void *ud,
                                    int64_t *count_out, int64_t *cursor_out, bool *more_out,
                                    atlas_err *err);

/* One artifact row, as a borrowed view valid only for the callback — the rule
 * every row callback in Atlas follows, because these point into a live
 * statement. Copy anything that must outlive the call.
 *
 * There is deliberately no path member. An artifact is addressed by its
 * server-assigned `id`; the bytes, when Atlas has them, are `content`. */
typedef struct atlas_orch_artifact_row {
    int64_t id;
    int64_t attempt_no;
    const char *name;
    const char *kind;
    int64_t size_bytes;
    const char *sha256;
    bool content_stored;
    const void *content; /* NULL unless content was requested and stored */
    size_t content_len;
} atlas_orch_artifact_row;

typedef atlas_status (*atlas_orch_artifact_cb)(const atlas_orch_artifact_row *row, void *ud,
                                               atlas_err *err);

/* Artifacts of one job, by its public uid. `artifact_id` of 0 means all of them;
 * a non-zero value selects one. Content is only ever materialised when
 * `want_content` is set *and* the worker sent it inline. */
atlas_status atlas_db_orch_artifacts(atlas_db *db, const char *job_uid, int64_t artifact_id,
                                     bool want_content, atlas_orch_artifact_cb cb, void *ud,
                                     int64_t *count_out, atlas_err *err);

/* --- A8: the source snapshot manifest ---------------------------------------
 *
 * Persisted so a dispatcher that restarts mid-transfer resumes against the same
 * snapshot identity. Content is never stored — only the manifest — because
 * SQLite is Atlas' rebuildable index and repository content in it would make it
 * something else. */
typedef struct atlas_orch_snapshot_source {
    int64_t job_id;
    int64_t repo_id;
    atlas_buf repo_root; /* the canonical path from the registry */
    atlas_buf commit;    /* the exact pinned commit from the job */
    atlas_buf identity;  /* the durable repository identity the job was created against */
} atlas_orch_snapshot_source;

typedef struct atlas_orch_snapshot_entry {
    atlas_buf path;
    char mode[8];
    char oid[65];
    char sha256[65];
    int64_t size_bytes;
} atlas_orch_snapshot_entry;

/* Resolves a bearer token to the attempt it authorises, refusing an expired,
 * released or unknown one. The single place a worker message becomes an attempt
 * identity. */
atlas_status atlas_db_orch_attempt_for_token(atlas_db *db, const char *token,
                                             int64_t *attempt_id_out, atlas_err *err);

/* Resolves the trusted source for one attempt: the registry's canonical path and
 * the job's pinned commit. Refuses when the repository no longer has the durable
 * identity the job was created against — the bytes it would hand over are then
 * not the bytes the job was authorised over. */
atlas_status atlas_db_orch_snapshot_source(atlas_db *db, int64_t attempt_id,
                                           atlas_orch_snapshot_source *out, atlas_err *err);

struct atlas_snapshot_meta;
atlas_status atlas_db_orch_snapshot_get(atlas_db *db, int64_t attempt_id,
                                        struct atlas_snapshot_meta *out, bool *found,
                                        atlas_err *err);
atlas_status atlas_db_orch_snapshot_create(atlas_db *db, int64_t attempt_id, const char *commit,
                                           const char *tree, int64_t *id_out, atlas_err *err);
atlas_status atlas_db_orch_snapshot_add_entry(atlas_db *db, int64_t snapshot_id, int64_t index,
                                              const void *path, size_t path_len, const char *mode,
                                              const char *oid, int64_t size, const char *sha256,
                                              atlas_err *err);
atlas_status atlas_db_orch_snapshot_finish(atlas_db *db, int64_t snapshot_id,
                                           const struct atlas_snapshot_meta *meta,
                                           atlas_err *err);
/* One manifest entry plus the repository it came from, for serving a chunk. */
atlas_status atlas_db_orch_snapshot_entry(atlas_db *db, int64_t attempt_id, int64_t index,
                                          atlas_orch_snapshot_entry *out, atlas_buf *repo_root,
                                          atlas_err *err);

/* Clears the soft repository pointer when a repository is removed. Called from
 * inside `atlas_db_repo_remove`'s transaction. */
atlas_status atlas_db_orch_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err);

#endif /* ATLAS_ORCH_OPS_H */

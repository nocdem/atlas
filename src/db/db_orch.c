/* Atlas - A8: the one write point for orchestration state, and the reads.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See atlas/orch_ops.h for the rule this file exists to keep: every mutation of
 * every `orch_*` table happens inside `atlas_orch_apply_in_tx`, and there is no
 * second implementation. The transition check, the lease check, the attempt
 * allocation, the ledger append and the status-cache update all live behind it.
 *
 * Two properties are worth stating before the code, because they are what the
 * shape of it is for:
 *
 *   **Every state change is a compare-and-swap.** The UPDATE names the state it
 *   observed — `... WHERE id = ? AND state = ?` — and the caller requires that
 *   exactly one row changed. That is what makes a concurrent transition lose
 *   deterministically rather than last-write-wins, and it is A4's rule applied
 *   here. Never replace it with a read followed by an unconditional write.
 *
 *   **Ordering is the ledger's AUTOINCREMENT id, never a timestamp.** Two events
 *   in the same millisecond are ordered; a clock that steps backwards cannot
 *   reorder history. Timestamps are recorded as evidence and are used for
 *   exactly one decision — whether a lease has expired — which is genuinely a
 *   question about time.
 */
#define _GNU_SOURCE 1

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/limits.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "atlas/snapshot.h"
#include "db_internal.h"

/* --- small helpers --------------------------------------------------------- */

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

void atlas_orch_artifact_init(atlas_orch_artifact *a) {
    memset(a, 0, sizeof(*a));
    atlas_buf_init(&a->name);
    atlas_buf_init(&a->kind);
    atlas_buf_init(&a->sha256);
    atlas_buf_init(&a->content);
}

void atlas_orch_artifact_free(atlas_orch_artifact *a) {
    if (a == NULL) {
        return;
    }
    atlas_buf_free(&a->name);
    atlas_buf_free(&a->kind);
    atlas_buf_free(&a->sha256);
    atlas_buf_free(&a->content);
}

atlas_orch_op *atlas_orch_op_new(atlas_orch_op_kind kind) {
    atlas_orch_op *op = calloc(1, sizeof(*op));
    if (op == NULL) {
        return NULL;
    }
    op->kind = kind;
    atlas_orch_spec_init(&op->spec);
    atlas_buf_init(&op->repo_root);
    atlas_buf_init(&op->job_uid);
    atlas_buf_init(&op->dispatcher_id);
    atlas_buf_init(&op->lease_drivers);
    atlas_buf_init(&op->token);
    atlas_buf_init(&op->event_kind);
    atlas_buf_init(&op->event_payload);
    atlas_buf_init(&op->driver_version);
    return op;
}

void atlas_orch_op_free(atlas_orch_op *op) {
    if (op == NULL) {
        return;
    }
    atlas_orch_spec_free(&op->spec);
    atlas_buf_free(&op->repo_root);
    atlas_buf_free(&op->job_uid);
    atlas_buf_free(&op->dispatcher_id);
    atlas_buf_free(&op->lease_drivers);
    atlas_buf_free(&op->token);
    atlas_buf_free(&op->event_kind);
    atlas_buf_free(&op->event_payload);
    atlas_buf_free(&op->driver_version);
    for (size_t i = 0; i < op->artifact_count; i++) {
        atlas_orch_artifact_free(&op->artifacts[i]);
    }
    free(op->artifacts);
    op->artifacts = NULL;
    op->artifact_count = 0;
}

void atlas_orch_result_init(atlas_orch_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->job_uid);
    atlas_buf_init(&r->run_uid);
    atlas_buf_init(&r->token);
    atlas_buf_init(&r->repo_name);
    atlas_buf_init(&r->repo_root);
    atlas_buf_init(&r->source_commit);
    atlas_buf_init(&r->mode);
    atlas_buf_init(&r->driver);
    atlas_buf_init(&r->task_text);
    atlas_buf_init(&r->allowed_paths);
    atlas_buf_init(&r->validations);
}

void atlas_orch_result_free(atlas_orch_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->job_uid);
    atlas_buf_free(&r->run_uid);
    atlas_buf_free(&r->token);
    atlas_buf_free(&r->repo_name);
    atlas_buf_free(&r->repo_root);
    atlas_buf_free(&r->source_commit);
    atlas_buf_free(&r->mode);
    atlas_buf_free(&r->driver);
    atlas_buf_free(&r->task_text);
    atlas_buf_free(&r->allowed_paths);
    atlas_buf_free(&r->validations);
}

/* --- the job row ----------------------------------------------------------- */

typedef struct job_row {
    int64_t id;
    char uid[ATLAS_ORCH_UID_MAX];
    atlas_orch_state state;
    int64_t repo_id;
    char repo_name[ATLAS_ORCH_NAME_MAX + 1u];
    char repo_identity_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char source_commit[41];
    char mode[ATLAS_ORCH_NAME_MAX + 1u];
    char driver[ATLAS_ORCH_NAME_MAX + 1u];
    char spec_digest[ATLAS_SHA256_HEX_LEN + 1u];
    int64_t submitter_uid;
    int64_t attempts_started;
    int64_t max_attempts;
    int64_t wall_timeout_ms;
    int64_t idle_timeout_ms;
    int64_t max_output_bytes;
    int64_t max_artifact_bytes;
    int64_t max_artifact_count;
    int64_t deadline_ms;
    bool cancel_requested;
    /* A11.0. Empty for every job submitted before migration 21, which reads as
     * "this job belongs to no run" and never as "this job is its own root". */
    char run_uid[ATLAS_ORCH_RUN_UID_MAX];
    char parent_job_uid[ATLAS_ORCH_UID_MAX];
} job_row;

/* The two job lookups below select the same columns in the same order and share
 * `job_fill`. They are written out separately rather than composed from a
 * fragment because `atlas_db_prepare` caches on the SQL *pointer* and requires
 * static storage duration: a constructed string would miss the cache on every
 * call, and — worse — a reused buffer at a stable address is exactly the shape
 * that once handed a caller the previous statement. A string literal per query
 * is the documented way to use that cache. */
static atlas_status job_fill(atlas_db *db, sqlite3_stmt *st, job_row *j, atlas_err *err) {
    memset(j, 0, sizeof(*j));
    j->id = sqlite3_column_int64(st, 0);
    atlas_status s = atlas_db_col_copy(st, 1, j->uid, sizeof(j->uid), "job_uid", err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!atlas_orch_state_parse(atlas_db_col_text(st, 2), &j->state)) {
        return atlas_err_set(err, ATLAS_ERR_DB, "job %s holds a state Atlas does not recognise",
                             j->uid);
    }
    j->repo_id = sqlite3_column_int64(st, 3);
    s = atlas_db_col_copy(st, 4, j->repo_name, sizeof(j->repo_name), "repo_name", err);
    if (s == ATLAS_OK) {
        s = atlas_db_col_copy(st, 5, j->repo_identity_hash, sizeof(j->repo_identity_hash),
                              "repo_identity_hash", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_col_copy(st, 6, j->source_commit, sizeof(j->source_commit), "source_commit",
                              err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_col_copy(st, 7, j->mode, sizeof(j->mode), "mode", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_col_copy(st, 8, j->driver, sizeof(j->driver), "driver", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_col_copy(st, 9, j->spec_digest, sizeof(j->spec_digest), "spec_digest", err);
    }
    if (s != ATLAS_OK) {
        return s;
    }
    j->submitter_uid = sqlite3_column_int64(st, 10);
    j->attempts_started = sqlite3_column_int64(st, 11);
    j->max_attempts = sqlite3_column_int64(st, 12);
    j->wall_timeout_ms = sqlite3_column_int64(st, 13);
    j->idle_timeout_ms = sqlite3_column_int64(st, 14);
    j->max_output_bytes = sqlite3_column_int64(st, 15);
    j->max_artifact_bytes = sqlite3_column_int64(st, 16);
    j->max_artifact_count = sqlite3_column_int64(st, 17);
    j->deadline_ms = sqlite3_column_int64(st, 18);
    j->cancel_requested = sqlite3_column_int64(st, 19) != 0;
    s = atlas_db_col_copy(st, 20, j->run_uid, sizeof(j->run_uid), "run_uid", err);
    if (s == ATLAS_OK) {
        s = atlas_db_col_copy(st, 21, j->parent_job_uid, sizeof(j->parent_job_uid),
                              "parent_job_uid", err);
    }
    if (s != ATLAS_OK) {
        return s;
    }
    (void)db;
    return ATLAS_OK;
}

static atlas_status job_by_uid(atlas_db *db, const char *uid, job_row *j, bool *found,
                               atlas_err *err) {
    static const char SQL[] =
        "SELECT id, job_uid, state, repo_id, repo_name, repo_identity_hash, source_commit, mode,"
        "       driver, spec_digest, submitter_uid, attempts_started, max_attempts,"
        "       wall_timeout_ms, idle_timeout_ms, max_output_bytes, max_artifact_bytes,"
        "       max_artifact_count, deadline_ms, cancel_requested, run_uid, parent_job_uid"
        "  FROM orch_jobs WHERE job_uid = ?1;";
    *found = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        s = job_fill(db, st, j, err);
        *found = (s == ATLAS_OK);
    } else if (rc != SQLITE_DONE) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a job");
    }
    atlas_db_finish(db, st);
    return s;
}

static atlas_status job_by_id(atlas_db *db, int64_t id, job_row *j, bool *found, atlas_err *err) {
    static const char SQL[] =
        "SELECT id, job_uid, state, repo_id, repo_name, repo_identity_hash, source_commit, mode,"
        "       driver, spec_digest, submitter_uid, attempts_started, max_attempts,"
        "       wall_timeout_ms, idle_timeout_ms, max_output_bytes, max_artifact_bytes,"
        "       max_artifact_count, deadline_ms, cancel_requested, run_uid, parent_job_uid"
        "  FROM orch_jobs WHERE id = ?1;";
    *found = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        s = job_fill(db, st, j, err);
        *found = (s == ATLAS_OK);
    } else if (rc != SQLITE_DONE) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a job");
    }
    atlas_db_finish(db, st);
    return s;
}

/* --- the ledger and the compare-and-swap ------------------------------------
 *
 * `record_transition` is the only function that appends to `orch_transitions`,
 * and `set_state` is the only one that changes `orch_jobs.state`. They are
 * always called together and in this order: the ledger row is the canonical
 * event and the status column is a cache of it, which is A4's relationship
 * between `decision_events` and `current_status`.
 */
static atlas_status record_transition(atlas_db *db, int64_t job_id, int64_t attempt_id,
                                      atlas_orch_state from, atlas_orch_state to,
                                      atlas_orch_reason reason, atlas_orch_actor actor,
                                      long long actor_uid, const char *detail, int64_t *seq_out,
                                      atlas_err *err) {
    static const char SQL[] =
        "INSERT INTO orch_transitions(job_id, attempt_id, from_state, to_state, reason, actor,"
        "                             actor_uid, detail, at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    (void)sqlite3_bind_int64(st, 1, job_id);
    if (attempt_id > 0) {
        (void)sqlite3_bind_int64(st, 2, attempt_id);
    } else {
        (void)sqlite3_bind_null(st, 2);
    }
    s = atlas_db_bind_text_opt(db, st, 3, atlas_orch_state_name(from), err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 4, atlas_orch_state_name(to), err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 5, atlas_orch_reason_name(reason), err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 6, atlas_orch_actor_name(actor), err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 7, actor_uid);
        s = atlas_db_bind_text_opt(db, st, 8, detail != NULL ? detail : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 9, at, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    s = atlas_db_step_done(db, st, err);
    if (s == ATLAS_OK && seq_out != NULL) {
        *seq_out = sqlite3_last_insert_rowid(db->h);
    }
    return s;
}

/* The compare-and-swap. `from` is the state the caller observed; exactly one
 * row must change or the transition lost a race and is refused. */
static atlas_status set_state(atlas_db *db, int64_t job_id, atlas_orch_state from,
                              atlas_orch_state to, int64_t seq, bool terminal, atlas_err *err) {
    static const char SQL[] =
        "UPDATE orch_jobs SET state = ?1, state_seq = ?2,"
        "       terminal_at = CASE WHEN ?3 = 1 THEN ?4 ELSE terminal_at END"
        " WHERE id = ?5 AND state = ?6;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, atlas_orch_state_name(to), err);
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 2, seq);
        (void)sqlite3_bind_int(st, 3, terminal ? 1 : 0);
        s = atlas_db_bind_text_opt(db, st, 4, at, err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 5, job_id);
        s = atlas_db_bind_text_opt(db, st, 6, atlas_orch_state_name(from), err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    s = atlas_db_step_done(db, st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (sqlite3_changes(db->h) != 1) {
        /* Somebody else moved the job between the read and the write. Losing is
         * the correct outcome and it is deterministic: the loser is whoever's
         * observed state no longer matches. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the job moved out of %s before this change could be applied",
                             atlas_orch_state_name(from));
    }
    return ATLAS_OK;
}

/* Applies one transition: checks it centrally, appends the ledger row, then
 * swaps the cache. Every state change in A8 goes through here. */
static atlas_status transition(atlas_db *db, const job_row *j, int64_t attempt_id,
                               atlas_orch_state to, atlas_orch_reason reason,
                               atlas_orch_actor actor, long long actor_uid, const char *detail,
                               int64_t *seq_out, atlas_err *err) {
    if (!atlas_orch_transition_allowed(j->state, to)) {
        /* Fail closed, and say which edge was refused: an invalid transition is
         * a bug in a caller or a hostile request, and both are worth naming. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "a job may not move from %s to %s",
                             atlas_orch_state_name(j->state), atlas_orch_state_name(to));
    }
    int64_t seq = 0;
    atlas_status s = record_transition(db, j->id, attempt_id, j->state, to, reason, actor,
                                       actor_uid, detail, &seq, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = set_state(db, j->id, j->state, to, seq, atlas_orch_state_is_terminal(to), err);
    if (s == ATLAS_OK && seq_out != NULL) {
        *seq_out = seq;
    }
    return s;
}

/* Attempts carry their own state so a stale message can be told from a current
 * one without reading the job. Updated in the same transaction as the job. */
static atlas_status set_attempt_state(atlas_db *db, int64_t attempt_id, atlas_orch_state to,
                                      bool ended, atlas_err *err) {
    static const char SQL[] =
        "UPDATE orch_attempts SET state = ?1,"
        "       ended_at = CASE WHEN ?2 = 1 THEN ?3 ELSE ended_at END"
        " WHERE id = ?4;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, atlas_orch_state_name(to), err);
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int(st, 2, ended ? 1 : 0);
        s = atlas_db_bind_text_opt(db, st, 3, at, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    (void)sqlite3_bind_int64(st, 4, attempt_id);
    return atlas_db_step_done(db, st, err);
}

static atlas_status release_lease(atlas_db *db, int64_t attempt_id, const char *reason,
                                  atlas_err *err) {
    static const char SQL[] =
        "UPDATE orch_leases SET released_at = ?1, release_reason = ?2"
        " WHERE attempt_id = ?3 AND released_at IS NULL;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, at, err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, reason, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    (void)sqlite3_bind_int64(st, 3, attempt_id);
    return atlas_db_step_done(db, st, err);
}

/* --- authenticating a worker message ---------------------------------------
 *
 * A worker presents a bearer token. It identifies the *attempt*, and nothing
 * else about the worker is consulted: not its claimed pid, not its claimed uid,
 * not the job id it says it is working on. The token is hashed and compared
 * against the stored digest, so the database never holds a value that could be
 * replayed if it leaked.
 *
 * The lease must also be unexpired and unreleased. An expired lease cannot
 * complete successfully — that is the rule that stops a worker which was
 * declared dead, and whose job was retried, from overwriting the newer
 * attempt's result. */
typedef struct lease_row {
    int64_t lease_id;
    int64_t attempt_id;
    int64_t job_id;
    int64_t attempt_no;
    int64_t expires_ms;
    int64_t renewals;
    int64_t max_renewals;
    int64_t last_heartbeat_ms;
    atlas_orch_state attempt_state;
    bool released;
} lease_row;

static atlas_status lease_by_token(atlas_db *db, const char *token, lease_row *lr, bool *found,
                                   atlas_err *err) {
    static const char SQL[] =
        "SELECT l.id, l.attempt_id, l.job_id, a.attempt_no, l.expires_ms, l.renewals,"
        "       l.max_renewals, l.last_heartbeat_ms, a.state, l.released_at IS NOT NULL"
        "  FROM orch_leases l JOIN orch_attempts a ON a.id = l.attempt_id"
        " WHERE l.token_digest = ?1;";
    *found = false;
    char digest[65];
    atlas_status s = atlas_orch_token_digest(token, digest, err);
    if (s != ATLAS_OK) {
        return s;
    }
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, digest, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        memset(lr, 0, sizeof(*lr));
        lr->lease_id = sqlite3_column_int64(st, 0);
        lr->attempt_id = sqlite3_column_int64(st, 1);
        lr->job_id = sqlite3_column_int64(st, 2);
        lr->attempt_no = sqlite3_column_int64(st, 3);
        lr->expires_ms = sqlite3_column_int64(st, 4);
        lr->renewals = sqlite3_column_int64(st, 5);
        lr->max_renewals = sqlite3_column_int64(st, 6);
        lr->last_heartbeat_ms = sqlite3_column_int64(st, 7);
        if (!atlas_orch_state_parse(atlas_db_col_text(st, 8), &lr->attempt_state)) {
            lr->attempt_state = ATLAS_ORCH_STATE_UNKNOWN;
        }
        lr->released = sqlite3_column_int64(st, 9) != 0;
        *found = true;
    } else if (rc != SQLITE_DONE) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a lease");
    }
    atlas_db_finish(db, st);
    return s;
}

/* Resolves a worker message to the attempt it may act on, or refuses.
 *
 * The error text is deliberately the same shape for "no such token", "released"
 * and "expired": a worker learning *why* its token was refused learns something
 * about leases it does not hold. */
static atlas_status require_lease(atlas_db *db, const atlas_orch_op *op, int64_t at_ms,
                                  lease_row *lr, job_row *j, atlas_err *err) {
    bool found = false;
    atlas_status s = lease_by_token(db, atlas_buf_cstr(&op->token), lr, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found || lr->released) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the presented lease is not valid for any active attempt");
    }
    if (lr->expires_ms <= at_ms) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the presented lease is not valid for any active attempt");
    }
    bool jfound = false;
    s = job_by_id(db, lr->job_id, j, &jfound, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!jfound) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the leased job no longer exists");
    }
    /* The job may have moved on without the worker noticing — a retry after an
     * expiry, or a cancellation that completed. A message about an attempt that
     * is no longer the job's current one changes nothing. */
    if (atlas_orch_state_is_terminal(j->state)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "job %s already ended in %s; this message is from a previous attempt",
                             j->uid, atlas_orch_state_name(j->state));
    }
    return ATLAS_OK;
}

/* --- the run (A11.0) --------------------------------------------------------
 *
 * A8 stored `parent_job_uid` and resolved it nowhere. What follows is the
 * resolution, and it happens inside the submit transaction rather than beside
 * it, for the reason the idempotency check is in there: a check that a run is
 * still ACTIVE is worthless if a second submission can land between the check
 * and the insert.
 *
 * Everything here refuses. Nothing in this function repairs a chain, invents a
 * run for a parent that has none, or downgrades a mismatch to a warning.
 */

/* The complement of `atlas_orch_state_is_terminal`, as SQLite must spell it.
 * `tests/test_orch_run.c` asserts this predicate and the C function agree over
 * the whole vocabulary, so the duplication cannot drift silently. It is a string
 * literal so `atlas_db_prepare`'s pointer cache still works — concatenation of
 * literals happens at translation time, not at run time. */
#define ORCH_SQL_ACTIVE_STATE \
    "state NOT IN ('SUCCEEDED','FAILED','CANCELLED','TIMED_OUT','RECOVERY_REQUIRED')"

/* A bounded copy between two fixed fields. Every use below copies between
 * arrays of equal declared size, so the refusal is unreachable today; it is
 * written anyway because "unreachable" is a property of the current sizes and
 * a silent truncation of an identifier is the failure it would become. */
static atlas_status copy_fixed(char *dst, size_t cap, const char *src, const char *what,
                               atlas_err *err) {
    size_t n = strlen(src);
    if (n + 1u > cap) {
        return atlas_err_set(err, ATLAS_ERR_DB, "a stored %s does not fit", what);
    }
    memcpy(dst, src, n + 1u);
    return ATLAS_OK;
}

typedef struct run_row {
    int64_t id;
    char run_uid[ATLAS_ORCH_RUN_UID_MAX];
    char root_job_uid[ATLAS_ORCH_UID_MAX];
    char repo_identity_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_orch_run_status status;
} run_row;

static atlas_status run_by_uid(atlas_db *db, const char *uid, run_row *r, bool *found,
                               atlas_err *err) {
    static const char SQL[] =
        "SELECT id, run_uid, root_job_uid, repo_identity_hash, status"
        "  FROM orch_runs WHERE run_uid = ?1;";
    *found = false;
    memset(r, 0, sizeof(*r));
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        r->id = sqlite3_column_int64(st, 0);
        s = atlas_db_col_copy(st, 1, r->run_uid, sizeof(r->run_uid), "run_uid", err);
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 2, r->root_job_uid, sizeof(r->root_job_uid), "root_job_uid",
                                  err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 3, r->repo_identity_hash, sizeof(r->repo_identity_hash),
                                  "repo_identity_hash", err);
        }
        if (s == ATLAS_OK && !atlas_orch_run_status_parse(atlas_db_col_text(st, 4), &r->status)) {
            /* Includes the literal 'UNKNOWN', which the parser refuses on
             * purpose: it is the vocabulary's zero and no stored run may hold
             * it, so a row presenting it is corruption rather than a state. */
            s = atlas_err_set(err, ATLAS_ERR_DB,
                              "run %s holds a status Atlas does not recognise", r->run_uid);
        }
        *found = (s == ATLAS_OK);
    } else if (rc != SQLITE_DONE) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a run");
    }
    atlas_db_finish(db, st);
    return s;
}

/* Names the run's active task, if it has one. The answer is a uid rather than a
 * count because the caller's refusal has to say *which* task is in the way — a
 * bare "there is already an active task" sends an operator looking for it. */
static atlas_status run_active_job(atlas_db *db, const char *run_uid, char out[ATLAS_ORCH_UID_MAX],
                                   bool *found, atlas_err *err) {
    static const char SQL[] = "SELECT job_uid FROM orch_jobs"
                              "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE " ORDER BY id"
                              "  LIMIT 1;";
    *found = false;
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        s = atlas_db_col_copy(st, 0, out, ATLAS_ORCH_UID_MAX, "job_uid", err);
        *found = (s == ATLAS_OK);
    } else if (rc != SQLITE_DONE) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a run's active task");
    }
    atlas_db_finish(db, st);
    return s;
}

/* Decides which run the job being submitted belongs to, creating one when the
 * job is a root. On success `run_uid_out` holds the run this job joins.
 *
 * A root task — one with no parent — always gets a fresh run. A child task
 * joins its parent's, and every one of the four conditions below is a refusal
 * rather than a repair. */
static atlas_status submit_resolve_run(atlas_db *db, const atlas_orch_spec *s, const char *job_uid,
                                       int64_t created_ms, atlas_buf *run_uid_out,
                                       atlas_err *err) {
    if (s->parent_job_uid.len == 0) {
        atlas_status st = atlas_orch_new_run_uid(run_uid_out, err);
        if (st != ATLAS_OK) {
            return st;
        }
        static const char INS[] =
            "INSERT INTO orch_runs(run_uid, root_job_uid, repo_identity_hash, status,"
            "  created_at, created_ms) VALUES(?1, ?2, ?3, 'ACTIVE', ?4, ?5);";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, INS, &q, err);
        if (st != ATLAS_OK) {
            return st;
        }
        char at[ATLAS_TS_MAX];
        atlas_now_iso8601(at, sizeof(at));
        const char *texts[] = {atlas_buf_cstr(run_uid_out), job_uid,
                               atlas_buf_cstr(&s->repo_identity_hash), at};
        for (size_t i = 0; st == ATLAS_OK && i < sizeof texts / sizeof texts[0]; i++) {
            st = atlas_db_bind_text_opt(db, q, (int)i + 1, texts[i], err);
        }
        if (st == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 5, created_ms);
            st = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        return st;
    }

    /* A child. The parent is resolved, not merely well formed — which is the
     * whole difference between A8's column and A11.0's chain. */
    const char *parent = atlas_buf_cstr(&s->parent_job_uid);
    job_row p;
    bool found = false;
    atlas_status st = job_by_uid(db, parent, &p, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no job named %s exists to be a parent",
                             parent);
    }
    if (p.run_uid[0] == '\0') {
        /* A job from before migration 21. It belongs to no run, and inventing
         * one for it now would be the backfill the migration refused to do. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "job %s belongs to no run and cannot be a parent", parent);
    }
    if (strcmp(p.repo_identity_hash, atlas_buf_cstr(&s->repo_identity_hash)) != 0) {
        /* A chain that changes repository midway is two chains. Joining them
         * would let a child inherit a run whose source identity it does not
         * share, and every later reader of the run would be wrong about one of
         * them. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "job %s describes a different repository from this submission",
                             parent);
    }

    run_row r;
    bool run_found = false;
    st = run_by_uid(db, p.run_uid, &r, &run_found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!run_found) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "job %s names a run that does not exist",
                             parent);
    }
    if (atlas_orch_run_status_is_terminal(r.status)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "run %s already ended in %s and takes no further task",
                             r.run_uid, atlas_orch_run_status_name(r.status));
    }

    char active[ATLAS_ORCH_UID_MAX];
    bool busy = false;
    st = run_active_job(db, p.run_uid, active, &busy, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (busy) {
        /* The schema's partial unique index would refuse this too. The check is
         * here so the caller gets a sentence naming the task in the way, rather
         * than a constraint violation it cannot act on. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "run %s already has an active task, %s; it takes no second one",
                             r.run_uid, active);
    }
    return atlas_buf_set_str(run_uid_out, r.run_uid, err);
}

/* --- SUBMIT ---------------------------------------------------------------- */

static atlas_status op_submit(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                              atlas_err *err) {
    const atlas_orch_spec *s = &op->spec;
    char digest[65];
    atlas_status st = atlas_orch_spec_digest(s, digest, err);
    if (st != ATLAS_OK) {
        return st;
    }
    memcpy(out->spec_digest, digest, sizeof(out->spec_digest));

    /* Idempotency first, and inside the same transaction as the insert, so a
     * replay cannot slip between the check and the create.
     *
     * Same key and same digest: the earlier job is returned and nothing new is
     * made. Same key and a *different* digest: refused. Silently returning the
     * older job would run something other than what was asked for, and the
     * caller would have no way to notice — which is worse than an error. */
    if (s->idempotency_key.len > 0) {
        static const char SEL[] =
            "SELECT job_id, spec_digest FROM orch_idempotency"
            " WHERE submitter_uid = ?1 AND key = ?2;";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, SEL, &q, err);
        if (st != ATLAS_OK) {
            return st;
        }
        (void)sqlite3_bind_int64(q, 1, s->submitter_uid);
        st = atlas_db_bind_text_opt(db, q, 2, atlas_buf_cstr(&s->idempotency_key), err);
        if (st != ATLAS_OK) {
            atlas_db_finish(db, q);
            return st;
        }
        int rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) {
            int64_t existing = sqlite3_column_int64(q, 0);
            bool same = strcmp(atlas_db_col_text(q, 1), digest) == 0;
            atlas_db_finish(db, q);
            if (!same) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "idempotency key \"%s\" was already used for a different job "
                                     "specification",
                                     atlas_buf_cstr(&s->idempotency_key));
            }
            job_row j;
            bool found = false;
            st = job_by_id(db, existing, &j, &found, err);
            if (st != ATLAS_OK) {
                return st;
            }
            if (!found) {
                return atlas_err_set(err, ATLAS_ERR_DB,
                                     "an idempotency record points at a job that is gone");
            }
            out->duplicate = true;
            out->job_id = j.id;
            out->state = j.state;
            /* A duplicate joins no run and creates none: the run it reports is
             * the one the original submission already settled. */
            atlas_status rs = atlas_buf_set_str(&out->run_uid, j.run_uid, err);
            if (rs != ATLAS_OK) {
                return rs;
            }
            return atlas_buf_set_str(&out->job_uid, j.uid, err);
        }
        atlas_db_finish(db, q);
        if (rc != SQLITE_DONE) {
            return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read an idempotency record");
        }
    }

    atlas_buf uid = ATLAS_BUF_INIT;
    atlas_buf paths = ATLAS_BUF_INIT;
    atlas_buf vals = ATLAS_BUF_INIT;
    atlas_buf run_uid = ATLAS_BUF_INIT;
    st = atlas_orch_new_uid(&uid, err);
    if (st == ATLAS_OK) {
        st = atlas_orch_paths_encode(s->allowed_paths, s->allowed_path_count, &paths, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_orch_validations_encode(s->validations, s->validation_count, &vals, err);
    }
    if (st == ATLAS_OK) {
        /* Which run this job joins, decided before the job row exists and inside
         * the same transaction as its insert. Every refusal in here refuses the
         * whole submission: a job whose run could not be settled is not stored
         * without one. */
        st = submit_resolve_run(db, s, atlas_buf_cstr(&uid), op->now_ms > 0 ? op->now_ms : now_ms(),
                                &run_uid, err);
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    {
        static const char INS[] =
            "INSERT INTO orch_jobs(job_uid, spec_version, spec_digest, submitter_uid, repo_id,"
            "  repo_name, repo_identity_hash, source_commit, mode, driver, task_text,"
            "  allowed_paths, validations, wall_timeout_ms, idle_timeout_ms, max_attempts,"
            "  max_output_bytes, max_artifact_bytes, max_artifact_count, correlation,"
            "  parent_job_uid, idempotency_key, state, created_at, created_ms, deadline_ms, run_uid)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,"
            "        ?21,?22,'QUEUED',?23,?24,?25,?26);";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, INS, &q, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        char at[ATLAS_TS_MAX];
        atlas_now_iso8601(at, sizeof(at));
        int64_t created = op->now_ms > 0 ? op->now_ms : now_ms();
        struct {
            int idx;
            const char *text;
        } texts[] = {
            {1, atlas_buf_cstr(&uid)},
            {3, digest},
            {6, atlas_buf_cstr(&s->repo_name)},
            {7, atlas_buf_cstr(&s->repo_identity_hash)},
            {8, atlas_buf_cstr(&s->source_commit)},
            {9, atlas_buf_cstr(&s->mode)},
            {10, atlas_buf_cstr(&s->driver)},
            {12, atlas_buf_cstr(&paths)},
            {13, atlas_buf_cstr(&vals)},
            {20, atlas_buf_cstr(&s->correlation)},
            {21, atlas_buf_cstr(&s->parent_job_uid)},
            {22, atlas_buf_cstr(&s->idempotency_key)},
            {26, atlas_buf_cstr(&run_uid)},
            {23, at},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof texts / sizeof texts[0]; i++) {
            st = atlas_db_bind_text_opt(db, q, texts[i].idx, texts[i].text, err);
        }
        /* Task text is bound with an explicit length rather than as a C string:
         * it is UNTRUSTED_DATA of a length the submitter chose, and binding it
         * by length is what makes the stored value the hashed one. */
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_n(db, q, 11, s->task_text.data, s->task_text.len, err);
        }
        if (st == ATLAS_OK) {
            (void)sqlite3_bind_int(q, 2, s->spec_version);
            (void)sqlite3_bind_int64(q, 4, s->submitter_uid);
            (void)sqlite3_bind_int64(q, 5, op->repo_id);
            (void)sqlite3_bind_int64(q, 14, s->wall_timeout_ms);
            (void)sqlite3_bind_int64(q, 15, s->idle_timeout_ms);
            (void)sqlite3_bind_int64(q, 16, s->max_attempts);
            (void)sqlite3_bind_int64(q, 17, s->max_output_bytes);
            (void)sqlite3_bind_int64(q, 18, s->max_artifact_bytes);
            (void)sqlite3_bind_int64(q, 19, s->max_artifact_count);
            (void)sqlite3_bind_int64(q, 24, created);
            (void)sqlite3_bind_int64(q, 25, created + s->wall_timeout_ms);
            st = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        if (st != ATLAS_OK) {
            goto done;
        }
        out->job_id = sqlite3_last_insert_rowid(db->h);
    }

    if (s->idempotency_key.len > 0) {
        static const char INS2[] =
            "INSERT INTO orch_idempotency(submitter_uid, key, job_id, spec_digest, created_at)"
            " VALUES(?1, ?2, ?3, ?4, ?5);";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, INS2, &q, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        char at[ATLAS_TS_MAX];
        atlas_now_iso8601(at, sizeof(at));
        (void)sqlite3_bind_int64(q, 1, s->submitter_uid);
        st = atlas_db_bind_text_opt(db, q, 2, atlas_buf_cstr(&s->idempotency_key), err);
        if (st == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 3, out->job_id);
            st = atlas_db_bind_text_opt(db, q, 4, digest, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_bind_text_opt(db, q, 5, at, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        if (st != ATLAS_OK) {
            goto done;
        }
    }

    /* The ledger's first row. `from` is UNKNOWN because there was no previous
     * state — the job did not exist — and that is the one place UNKNOWN appears
     * legitimately: as the absence a creation moved out of. */
    {
        int64_t seq = 0;
        st = record_transition(db, out->job_id, 0, ATLAS_ORCH_STATE_UNKNOWN,
                               ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_REASON_SUBMITTED,
                               ATLAS_ORCH_ACTOR_CLIENT, op->peer_uid, digest, &seq, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        static const char UPD[] = "UPDATE orch_jobs SET state_seq = ?1 WHERE id = ?2;";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, UPD, &q, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        (void)sqlite3_bind_int64(q, 1, seq);
        (void)sqlite3_bind_int64(q, 2, out->job_id);
        st = atlas_db_step_done(db, q, err);
        if (st != ATLAS_OK) {
            goto done;
        }
        out->seq = seq;
    }

    out->state = ATLAS_ORCH_STATE_QUEUED;
    st = atlas_buf_set(&out->job_uid, uid.data, uid.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->run_uid, run_uid.data, run_uid.len, err);
    }

done:
    atlas_buf_free(&uid);
    atlas_buf_free(&run_uid);
    atlas_buf_free(&paths);
    atlas_buf_free(&vals);
    return st;
}

/* --- CANCEL -----------------------------------------------------------------
 *
 * Race-safe by construction rather than by locking. A queued job is cancelled
 * outright, because nothing is running and there is nothing to ask to stop. An
 * active job enters CANCEL_REQUESTED, from which the transition table offers no
 * edge to SUCCEEDED — so a completion that arrives afterwards loses,
 * deterministically, and is told why. A terminal job is refused. */
static atlas_status op_cancel(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                              atlas_err *err) {
    job_row j;
    bool found = false;
    atlas_status s = job_by_uid(db, atlas_buf_cstr(&op->job_uid), &j, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    /* A submitter may cancel its own jobs. Another client's job is reported as
     * absent rather than as forbidden: whether a job exists is itself
     * information, and a caller who may not act on it need not learn it. */
    if (j.submitter_uid != op->peer_uid) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    out->job_id = j.id;
    s = atlas_buf_set_str(&out->job_uid, j.uid, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (atlas_orch_state_is_terminal(j.state)) {
        out->state = j.state;
        return atlas_err_set(err, ATLAS_ERR_USAGE, "job %s already ended in %s", j.uid,
                             atlas_orch_state_name(j.state));
    }

    /* The flag is set in the same transaction as the transition. It is what a
     * heartbeat reads, so a worker learns of the cancellation at its next
     * check-in rather than being signalled — there is no path from the daemon
     * to a worker process, by design. */
    {
        static const char UPD[] = "UPDATE orch_jobs SET cancel_requested = 1 WHERE id = ?1;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, UPD, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(q, 1, j.id);
        s = atlas_db_step_done(db, q, err);
        if (s != ATLAS_OK) {
            return s;
        }
    }

    if (j.state == ATLAS_ORCH_STATE_QUEUED) {
        s = transition(db, &j, 0, ATLAS_ORCH_STATE_CANCELLED, ATLAS_ORCH_REASON_CANCEL_CONFIRMED,
                       ATLAS_ORCH_ACTOR_CLIENT, op->peer_uid, "", &out->seq, err);
        out->state = ATLAS_ORCH_STATE_CANCELLED;
        return s;
    }
    if (j.state == ATLAS_ORCH_STATE_CANCEL_REQUESTED) {
        /* Already asked for. Idempotent rather than an error: a client retrying
         * a cancellation is doing the right thing. */
        out->state = j.state;
        return ATLAS_OK;
    }
    s = transition(db, &j, 0, ATLAS_ORCH_STATE_CANCEL_REQUESTED,
                   ATLAS_ORCH_REASON_CANCEL_REQUESTED, ATLAS_ORCH_ACTOR_CLIENT, op->peer_uid, "",
                   &out->seq, err);
    out->state = ATLAS_ORCH_STATE_CANCEL_REQUESTED;
    return s;
}

/* --- LEASE ------------------------------------------------------------------
 *
 * The attempt and the lease are created in the same transaction as the job's
 * transition out of QUEUED, and the job's state is swapped with a
 * compare-and-swap. Two dispatchers asking at once cannot both win: the second
 * finds the job is no longer QUEUED and its CAS changes no row.
 *
 * The partial unique index on `orch_leases(job_id) WHERE released_at IS NULL` is
 * the backstop underneath that. If the logic above were ever wrong, the second
 * insert fails the index rather than producing two workers on one job. */
static atlas_status op_lease(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                             atlas_err *err) {
    int64_t at_ms = op->now_ms > 0 ? op->now_ms : now_ms();

    /* Oldest queued job first, and only ones nobody has asked to cancel. `id`
     * rather than `created_at`: the ordering authority is the sequence, and two
     * jobs submitted in the same second must still have an order. */
    /* Oldest queued job whose driver this dispatcher will run.
     *
     * The filter is matched against `orch_jobs.driver` — the driver stored when
     * the job was created and validated against the policy — never against
     * anything the requesting worker said about it. */
    atlas_orch_argv want[1];
    atlas_orch_argv_init(&want[0]);
    size_t want_n = 0;
    if (op->lease_drivers.len > 0) {
        atlas_status ds = atlas_orch_validations_decode(atlas_buf_cstr(&op->lease_drivers), want,
                                                        1u, &want_n, err);
        if (ds != ATLAS_OK) {
            atlas_orch_argv_free(&want[0]);
            return ds;
        }
    }
    static const char PICK[] =
        "SELECT id FROM orch_jobs WHERE state = 'QUEUED' AND cancel_requested = 0"
        " ORDER BY id;";
    int64_t job_id = 0;
    {
        sqlite3_stmt *q = NULL;
        atlas_status s = atlas_db_prepare(db, PICK, &q, err);
        if (s != ATLAS_OK) {
            atlas_orch_argv_free(&want[0]);
            return s;
        }
        static const char DRV[] = "SELECT driver FROM orch_jobs WHERE id = ?1;";
        while (sqlite3_step(q) == SQLITE_ROW) {
            int64_t candidate = sqlite3_column_int64(q, 0);
            if (want_n == 0 || want[0].count == 0) {
                job_id = candidate;
                break;
            }
            sqlite3_stmt *dq = NULL;
            if (atlas_db_prepare(db, DRV, &dq, err) != ATLAS_OK) {
                break;
            }
            (void)sqlite3_bind_int64(dq, 1, candidate);
            bool match = false;
            if (sqlite3_step(dq) == SQLITE_ROW) {
                const char *drv = atlas_db_col_text(dq, 0);
                for (size_t i = 0; i < want[0].count; i++) {
                    if (strcmp(drv, atlas_buf_cstr(&want[0].args[i])) == 0) {
                        match = true;
                        break;
                    }
                }
            }
            atlas_db_finish(db, dq);
            if (match) {
                job_id = candidate;
                break;
            }
        }
        atlas_db_finish(db, q);
        atlas_orch_argv_free(&want[0]);
        if (s != ATLAS_OK) {
            return s;
        }
    }
    if (job_id == 0) {
        out->granted = false;
        return ATLAS_OK; /* an idle queue is not an error */
    }

    job_row j;
    bool found = false;
    atlas_status s = job_by_id(db, job_id, &j, &found, err);
    if (s != ATLAS_OK || !found) {
        return s != ATLAS_OK ? s : atlas_err_set(err, ATLAS_ERR_DB, "the picked job vanished");
    }

    /* The policy may have changed between submission and now, and the
     * repository may have been removed or re-registered. Both are checked here,
     * at the moment work would actually start, rather than trusted from
     * submission time. A job whose repository no longer has the identity it was
     * created against cannot be run: the bytes it would be given are not the
     * bytes it was authorised over. */
    atlas_buf identity = ATLAS_BUF_INIT;
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool repo_ok = false;
    if (atlas_db_repo_get(db, j.repo_name, &ri, &repo_ok, err) == ATLAS_OK && repo_ok) {
        if (atlas_db_repo_identity_hash(db, ri.id, &identity, err) == ATLAS_OK) {
            repo_ok = strcmp(atlas_buf_cstr(&identity), j.repo_identity_hash) == 0;
        } else {
            repo_ok = false;
        }
    }
    if (!repo_ok) {
        atlas_err_init(err);
        s = transition(db, &j, 0, ATLAS_ORCH_STATE_FAILED, ATLAS_ORCH_REASON_POLICY_REFUSED,
                       ATLAS_ORCH_ACTOR_ATLAS, 0,
                       "the registered repository no longer has the identity this job was "
                       "created against",
                       &out->seq, err);
        atlas_buf_free(&identity);
        atlas_repo_info_free(&ri);
        if (s != ATLAS_OK) {
            return s;
        }
        out->granted = false;
        return ATLAS_OK;
    }

    /* Attempts are monotonic per job. Derived from the job's own counter rather
     * than from `count(*)`, so a future phase that ever removed an attempt row
     * could not hand its number to a new one. */
    int64_t attempt_no = j.attempts_started + 1;
    if (attempt_no > j.max_attempts) {
        s = transition(db, &j, 0, ATLAS_ORCH_STATE_FAILED,
                       ATLAS_ORCH_REASON_ATTEMPTS_EXHAUSTED, ATLAS_ORCH_ACTOR_ATLAS, 0, "",
                       &out->seq, err);
        atlas_buf_free(&identity);
        atlas_repo_info_free(&ri);
        if (s != ATLAS_OK) {
            return s;
        }
        out->granted = false;
        return ATLAS_OK;
    }

    int64_t attempt_id = 0;
    {
        static const char INS[] =
            "INSERT INTO orch_attempts(job_id, attempt_no, dispatcher_uid, dispatcher_id,"
            "  state, driver, started_at) VALUES(?1, ?2, ?3, ?4, 'LEASED', ?5, ?6);";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, INS, &q, err);
        if (s != ATLAS_OK) {
            goto fail;
        }
        char at[ATLAS_TS_MAX];
        atlas_now_iso8601(at, sizeof(at));
        (void)sqlite3_bind_int64(q, 1, j.id);
        (void)sqlite3_bind_int64(q, 2, attempt_no);
        (void)sqlite3_bind_int64(q, 3, op->peer_uid);
        s = atlas_db_bind_text_opt(db, q, 4, atlas_buf_cstr(&op->dispatcher_id), err);
        if (s == ATLAS_OK) {
            s = atlas_db_bind_text_opt(db, q, 5, j.driver, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_bind_text_opt(db, q, 6, at, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        if (s != ATLAS_OK) {
            goto fail;
        }
        attempt_id = sqlite3_last_insert_rowid(db->h);
    }

    {
        atlas_buf token = ATLAS_BUF_INIT;
        s = atlas_orch_new_token(&token, err);
        char digest[65];
        if (s == ATLAS_OK) {
            s = atlas_orch_token_digest(atlas_buf_cstr(&token), digest, err);
        }
        if (s == ATLAS_OK) {
            static const char INS[] =
                "INSERT INTO orch_leases(job_id, attempt_id, token_digest, granted_at,"
                "  expires_ms, max_renewals, last_heartbeat_ms)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);";
            sqlite3_stmt *q = NULL;
            s = atlas_db_prepare(db, INS, &q, err);
            if (s == ATLAS_OK) {
                char at[ATLAS_TS_MAX];
                atlas_now_iso8601(at, sizeof(at));
                (void)sqlite3_bind_int64(q, 1, j.id);
                (void)sqlite3_bind_int64(q, 2, attempt_id);
                s = atlas_db_bind_text_opt(db, q, 3, digest, err);
                if (s == ATLAS_OK) {
                    s = atlas_db_bind_text_opt(db, q, 4, at, err);
                }
                if (s == ATLAS_OK) {
                    (void)sqlite3_bind_int64(q, 5, at_ms + ATLAS_ORCH_LEASE_MS);
                    (void)sqlite3_bind_int64(q, 6, ATLAS_ORCH_LEASE_MAX_RENEWALS);
                    (void)sqlite3_bind_int64(q, 7, at_ms);
                    s = atlas_db_step_done(db, q, err);
                } else {
                    atlas_db_finish(db, q);
                }
            }
        }
        /* The token leaves here once, in the result, and is never stored,
         * logged or reported again. */
        if (s == ATLAS_OK) {
            s = atlas_buf_set(&out->token, token.data, token.len, err);
        }
        atlas_buf_free(&token);
        if (s != ATLAS_OK) {
            goto fail;
        }
        out->expires_ms = at_ms + ATLAS_ORCH_LEASE_MS;
    }

    {
        static const char UPD[] =
            "UPDATE orch_jobs SET attempts_started = ?1 WHERE id = ?2;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, UPD, &q, err);
        if (s != ATLAS_OK) {
            goto fail;
        }
        (void)sqlite3_bind_int64(q, 1, attempt_no);
        (void)sqlite3_bind_int64(q, 2, j.id);
        s = atlas_db_step_done(db, q, err);
        if (s != ATLAS_OK) {
            goto fail;
        }
    }

    s = transition(db, &j, attempt_id, ATLAS_ORCH_STATE_LEASED,
                   ATLAS_ORCH_REASON_LEASE_GRANTED, ATLAS_ORCH_ACTOR_DISPATCHER, op->peer_uid,
                   atlas_buf_cstr(&op->dispatcher_id), &out->seq, err);
    if (s != ATLAS_OK) {
        goto fail;
    }

    /* Everything the worker is given comes from trusted Atlas state. Nothing
     * here is echoed back from the request: the repository path is the
     * registry's, the commit is the pinned one, and the bounds are the job's. */
    out->granted = true;
    out->job_id = j.id;
    out->attempt_id = attempt_id;
    out->attempt_no = attempt_no;
    out->state = ATLAS_ORCH_STATE_LEASED;
    out->wall_timeout_ms = j.wall_timeout_ms;
    out->idle_timeout_ms = j.idle_timeout_ms;
    out->max_output_bytes = j.max_output_bytes;
    out->max_artifact_bytes = j.max_artifact_bytes;
    out->max_artifact_count = j.max_artifact_count;
    memcpy(out->spec_digest, j.spec_digest, sizeof(out->spec_digest));
    struct {
        atlas_buf *to;
        const char *from;
    } copies[] = {
        {&out->job_uid, j.uid},
        {&out->repo_name, j.repo_name},
        {&out->repo_root, atlas_buf_cstr(&ri.root_path_text)},
        {&out->source_commit, j.source_commit},
        {&out->mode, j.mode},
        {&out->driver, j.driver},
    };
    for (size_t i = 0; s == ATLAS_OK && i < sizeof copies / sizeof copies[0]; i++) {
        s = atlas_buf_set_str(copies[i].to, copies[i].from, err);
    }
    if (s == ATLAS_OK) {
        static const char SEL[] =
            "SELECT task_text, allowed_paths, validations FROM orch_jobs WHERE id = ?1;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, SEL, &q, err);
        if (s == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 1, j.id);
            if (sqlite3_step(q) == SQLITE_ROW) {
                const void *tt = sqlite3_column_blob(q, 0);
                int tn = sqlite3_column_bytes(q, 0);
                s = atlas_buf_set(&out->task_text, tt, tn > 0 ? (size_t)tn : 0u, err);
                if (s == ATLAS_OK) {
                    s = atlas_buf_set_str(&out->allowed_paths, atlas_db_col_text(q, 1), err);
                }
                if (s == ATLAS_OK) {
                    s = atlas_buf_set_str(&out->validations, atlas_db_col_text(q, 2), err);
                }
            } else {
                s = atlas_err_set(err, ATLAS_ERR_DB, "cannot read the leased job payload");
            }
            atlas_db_finish(db, q);
        }
    }

fail:
    atlas_buf_free(&identity);
    atlas_repo_info_free(&ri);
    return s;
}

/* --- HEARTBEAT ---------------------------------------------------------------
 *
 * Renews the lease, advances the phase if the worker says it has moved, and
 * tells the worker whether cancellation has been requested.
 *
 * Renewals are bounded. A wedged worker cannot hold a job forever by
 * heartbeating; the wall-clock deadline is the separate bound that finally stops
 * it, and both are enforced here. */
static atlas_status op_heartbeat(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                                 atlas_err *err) {
    int64_t at_ms = op->now_ms > 0 ? op->now_ms : now_ms();
    lease_row lr;
    job_row j;
    atlas_status s = require_lease(db, op, at_ms, &lr, &j, err);
    if (s != ATLAS_OK) {
        return s;
    }
    out->job_id = j.id;
    out->attempt_id = lr.attempt_id;
    out->attempt_no = lr.attempt_no;
    s = atlas_buf_set_str(&out->job_uid, j.uid, err);
    if (s != ATLAS_OK) {
        return s;
    }

    if (lr.renewals >= lr.max_renewals) {
        s = transition(db, &j, lr.attempt_id, ATLAS_ORCH_STATE_TIMED_OUT,
                       ATLAS_ORCH_REASON_IDLE_TIMEOUT, ATLAS_ORCH_ACTOR_ATLAS, 0,
                       "the lease reached its renewal bound", &out->seq, err);
        if (s == ATLAS_OK) {
            s = set_attempt_state(db, lr.attempt_id, ATLAS_ORCH_STATE_TIMED_OUT, true, err);
        }
        if (s == ATLAS_OK) {
            s = release_lease(db, lr.attempt_id, "RENEWAL_BOUND", err);
        }
        out->state = ATLAS_ORCH_STATE_TIMED_OUT;
        return s;
    }
    if (j.deadline_ms > 0 && at_ms >= j.deadline_ms) {
        s = transition(db, &j, lr.attempt_id, ATLAS_ORCH_STATE_TIMED_OUT,
                       ATLAS_ORCH_REASON_WALL_TIMEOUT, ATLAS_ORCH_ACTOR_ATLAS, 0, "", &out->seq,
                       err);
        if (s == ATLAS_OK) {
            s = set_attempt_state(db, lr.attempt_id, ATLAS_ORCH_STATE_TIMED_OUT, true, err);
        }
        if (s == ATLAS_OK) {
            s = release_lease(db, lr.attempt_id, "WALL_TIMEOUT", err);
        }
        out->state = ATLAS_ORCH_STATE_TIMED_OUT;
        return s;
    }

    {
        static const char UPD[] =
            "UPDATE orch_leases SET expires_ms = ?1, renewals = renewals + 1,"
            "  last_heartbeat_ms = ?2 WHERE id = ?3 AND released_at IS NULL;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, UPD, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(q, 1, at_ms + ATLAS_ORCH_LEASE_MS);
        (void)sqlite3_bind_int64(q, 2, at_ms);
        (void)sqlite3_bind_int64(q, 3, lr.lease_id);
        s = atlas_db_step_done(db, q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        out->expires_ms = at_ms + ATLAS_ORCH_LEASE_MS;
    }

    /* A phase the worker reports is a transition like any other and is checked
     * by the same table: it cannot skip PREPARING, cannot go backwards, and
     * cannot reach a terminal state — a worker ends an attempt by completing
     * it, not by declaring itself finished in a heartbeat. */
    if (op->phase != ATLAS_ORCH_STATE_UNKNOWN && op->phase != j.state) {
        if (atlas_orch_state_is_terminal(op->phase)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a heartbeat may not put a job into the terminal state %s",
                                 atlas_orch_state_name(op->phase));
        }
        s = transition(db, &j, lr.attempt_id, op->phase, ATLAS_ORCH_REASON_WORKER_PROGRESS,
                       ATLAS_ORCH_ACTOR_DISPATCHER, op->peer_uid, "", &out->seq, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = set_attempt_state(db, lr.attempt_id, op->phase, false, err);
        if (s != ATLAS_OK) {
            return s;
        }
        /* One observation row per phase change, so the count is bounded by the
         * state machine rather than by how long the job ran. */
        static const char OBS[] =
            "INSERT INTO orch_observations(job_id, attempt_id, at, at_ms, claimed_pid, phase)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6);";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, OBS, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        char at[ATLAS_TS_MAX];
        atlas_now_iso8601(at, sizeof(at));
        (void)sqlite3_bind_int64(q, 1, j.id);
        (void)sqlite3_bind_int64(q, 2, lr.attempt_id);
        s = atlas_db_bind_text_opt(db, q, 3, at, err);
        if (s == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 4, at_ms);
            /* The worker's claim about its own pid, stored as a claim and used
             * for nothing that matters. */
            (void)sqlite3_bind_int64(q, 5, op->claimed_pid);
            s = atlas_db_bind_text_opt(db, q, 6, atlas_orch_state_name(op->phase), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        if (s != ATLAS_OK) {
            return s;
        }
        out->state = op->phase;
    } else {
        out->state = j.state;
    }

    /* How the worker learns it must stop. There is no signal from the daemon to
     * a worker process and there must not be one: the daemon has no path to the
     * worker's process tree, which is exactly the isolation A8 is for. */
    out->cancel_requested = j.cancel_requested;
    return ATLAS_OK;
}

/* --- EVENT ------------------------------------------------------------------ */

static atlas_status op_event(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                             atlas_err *err) {
    int64_t at_ms = op->now_ms > 0 ? op->now_ms : now_ms();
    lease_row lr;
    job_row j;
    atlas_status s = require_lease(db, op, at_ms, &lr, &j, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (op->event_payload.len > ATLAS_ORCH_EVENT_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "an event payload may be at most %u bytes",
                             (unsigned)ATLAS_ORCH_EVENT_MAX);
    }
    /* Bounds refuse rather than trim. An event stream that silently stopped
     * recording would look like a job that went quiet, which is the one thing a
     * narrative of a failed job must not do. */
    int64_t count = 0;
    {
        static const char SEL[] = "SELECT event_count FROM orch_attempts WHERE id = ?1;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, SEL, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(q, 1, lr.attempt_id);
        if (sqlite3_step(q) == SQLITE_ROW) {
            count = sqlite3_column_int64(q, 0);
        }
        atlas_db_finish(db, q);
    }
    if (count >= ATLAS_ORCH_MAX_EVENTS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "attempt %lld has reached its event bound of %d",
                             (long long)lr.attempt_no, ATLAS_ORCH_MAX_EVENTS);
    }

    static const char INS[] =
        "INSERT INTO orch_events(job_id, attempt_id, seq, kind, payload, at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6);";
    sqlite3_stmt *q = NULL;
    s = atlas_db_prepare(db, INS, &q, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    (void)sqlite3_bind_int64(q, 1, j.id);
    (void)sqlite3_bind_int64(q, 2, lr.attempt_id);
    (void)sqlite3_bind_int64(q, 3, op->event_seq);
    s = atlas_db_bind_text_opt(db, q, 4, atlas_buf_cstr(&op->event_kind), err);
    if (s == ATLAS_OK) {
        /* UNTRUSTED_DATA, bound by length. It is safe-encoded on the way out to
         * a terminal or a JSON document and is never treated as an instruction. */
        s = atlas_db_bind_text_n(db, q, 5, op->event_payload.data, op->event_payload.len, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, q, 6, at, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_step_done(db, q, err);
    } else {
        atlas_db_finish(db, q);
    }
    if (s != ATLAS_OK) {
        /* The unique index over (attempt_id, seq) is what makes a duplicated
         * delivery a refusal rather than a second row, so a dispatcher retrying
         * a send cannot inflate its own history. */
        return s;
    }

    static const char UPD[] =
        "UPDATE orch_attempts SET event_count = event_count + 1, event_bytes = event_bytes + ?1"
        " WHERE id = ?2;";
    sqlite3_stmt *u = NULL;
    s = atlas_db_prepare(db, UPD, &u, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(u, 1, (int64_t)op->event_payload.len);
    (void)sqlite3_bind_int64(u, 2, lr.attempt_id);
    s = atlas_db_step_done(db, u, err);
    out->job_id = j.id;
    out->attempt_id = lr.attempt_id;
    out->state = j.state;
    out->cancel_requested = j.cancel_requested;
    return s;
}

/* --- COMPLETE ---------------------------------------------------------------
 *
 * The worker reports a terminal outcome for the attempt whose lease it holds.
 *
 * Three things make this safe against the failure modes A8 must survive:
 *
 *   A stale worker cannot overwrite a newer result, because `require_lease`
 *   refuses an expired or released lease and a retried job's old lease is
 *   always released.
 *
 *   A cancelled job cannot be completed successfully, because the transition
 *   table offers no edge from CANCEL_REQUESTED to SUCCEEDED.
 *
 *   A duplicated completion changes nothing the second time, because the first
 *   released the lease.
 */
static atlas_status op_complete(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                                atlas_err *err) {
    int64_t at_ms = op->now_ms > 0 ? op->now_ms : now_ms();
    lease_row lr;
    job_row j;
    atlas_status s = require_lease(db, op, at_ms, &lr, &j, err);
    if (s != ATLAS_OK) {
        return s;
    }
    out->job_id = j.id;
    out->attempt_id = lr.attempt_id;
    out->attempt_no = lr.attempt_no;
    s = atlas_buf_set_str(&out->job_uid, j.uid, err);
    if (s != ATLAS_OK) {
        return s;
    }

    if (op->artifact_count > (size_t)j.max_artifact_count) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the completion declares %zu artifacts; this job permits %lld",
                             op->artifact_count, (long long)j.max_artifact_count);
    }
    int64_t total = 0;
    for (size_t i = 0; i < op->artifact_count; i++) {
        const atlas_orch_artifact *a = &op->artifacts[i];
        /* An artifact name is a safe relative path and is checked here as well
         * as in the worker. The worker's check protects the worker's own
         * filesystem; this one protects the manifest, because a name that
         * escaped would be a record pointing outside the workspace. */
        if (!atlas_orch_relpath_is_safe(atlas_buf_cstr(&a->name), a->name.len)) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "artifact %zu has a name that is not a safe relative path", i);
        }
        if (a->size_bytes < 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "artifact %zu has a negative size", i);
        }
        total += a->size_bytes;
        if (total > j.max_artifact_bytes) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "the completion's artifacts total more than this job's bound of "
                                 "%lld bytes",
                                 (long long)j.max_artifact_bytes);
        }
        if (a->content_stored && a->content.len > ATLAS_ORCH_ARTIFACT_INLINE_MAX) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "artifact %zu carries more inline content than %u bytes", i,
                                 (unsigned)ATLAS_ORCH_ARTIFACT_INLINE_MAX);
        }
    }

    for (size_t i = 0; i < op->artifact_count; i++) {
        const atlas_orch_artifact *a = &op->artifacts[i];
        static const char INS[] =
            "INSERT INTO orch_artifacts(job_id, attempt_id, name, kind, size_bytes, sha256,"
            "  content_stored, content, at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, INS, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        char at[ATLAS_TS_MAX];
        atlas_now_iso8601(at, sizeof(at));
        (void)sqlite3_bind_int64(q, 1, j.id);
        (void)sqlite3_bind_int64(q, 2, lr.attempt_id);
        s = atlas_db_bind_text_n(db, q, 3, a->name.data, a->name.len, err);
        if (s == ATLAS_OK) {
            s = atlas_db_bind_text_opt(db, q, 4, atlas_buf_cstr(&a->kind), err);
        }
        if (s == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 5, a->size_bytes);
            s = atlas_db_bind_text_opt(db, q, 6, atlas_buf_cstr(&a->sha256), err);
        }
        if (s == ATLAS_OK) {
            (void)sqlite3_bind_int(q, 7, a->content_stored ? 1 : 0);
            if (a->content_stored) {
                s = atlas_db_bind_blob(db, q, 8, a->content.data, a->content.len, err);
            } else {
                (void)sqlite3_bind_null(q, 8);
            }
        }
        if (s == ATLAS_OK) {
            s = atlas_db_bind_text_opt(db, q, 9, at, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        if (s != ATLAS_OK) {
            return s;
        }
    }

    {
        static const char UPD[] =
            "UPDATE orch_attempts SET driver_version = ?1, exit_kind = ?2, exit_code = ?3,"
            "  failure_reason = ?4, claimed_pid = ?5, artifact_count = ?6, artifact_bytes = ?7"
            " WHERE id = ?8;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, UPD, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_db_bind_text_opt(db, q, 1, atlas_buf_cstr(&op->driver_version), err);
        if (s == ATLAS_OK) {
            s = atlas_db_bind_text_opt(db, q, 2, atlas_orch_exit_kind_name(op->exit_kind), err);
        }
        if (s == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 3, op->exit_code);
            s = atlas_db_bind_text_opt(db, q, 4, atlas_orch_reason_name(op->failure_reason), err);
        }
        if (s == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 5, op->claimed_pid);
            (void)sqlite3_bind_int64(q, 6, (int64_t)op->artifact_count);
            (void)sqlite3_bind_int64(q, 7, total);
            (void)sqlite3_bind_int64(q, 8, lr.attempt_id);
            s = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        if (s != ATLAS_OK) {
            return s;
        }
    }

    /* Cancellation wins. A completion that arrives after an operator asked to
     * cancel is refused as a success and the job settles as CANCELLED — the
     * transition table has no CANCEL_REQUESTED to SUCCEEDED edge, so this is
     * the machine's answer rather than a special case bolted on beside it. */
    atlas_orch_state to;
    atlas_orch_reason reason;
    if (j.state == ATLAS_ORCH_STATE_CANCEL_REQUESTED || j.cancel_requested) {
        to = ATLAS_ORCH_STATE_CANCELLED;
        reason = ATLAS_ORCH_REASON_CANCEL_CONFIRMED;
    } else if (op->success) {
        to = ATLAS_ORCH_STATE_SUCCEEDED;
        reason = ATLAS_ORCH_REASON_WORKER_SUCCESS;
    } else if (lr.attempt_no < j.max_attempts) {
        /* Bounded retry with a recorded reason, and never an infinite loop: the
         * attempt counter is the bound and it only goes up. */
        to = ATLAS_ORCH_STATE_QUEUED;
        reason = ATLAS_ORCH_REASON_RETRY;
    } else {
        to = ATLAS_ORCH_STATE_FAILED;
        reason = ATLAS_ORCH_REASON_ATTEMPTS_EXHAUSTED;
    }

    /* The job may be in CANCEL_REQUESTED while the attempt is mid-pipeline, and
     * a job in RUNNING may complete directly when it declared no validations.
     * Both are edges the table already allows; nothing special is needed here. */
    s = transition(db, &j, lr.attempt_id, to, reason, ATLAS_ORCH_ACTOR_DISPATCHER, op->peer_uid,
                   atlas_orch_exit_kind_name(op->exit_kind), &out->seq, err);
    if (s != ATLAS_OK) {
        return s;
    }
    atlas_orch_state attempt_end = to == ATLAS_ORCH_STATE_QUEUED ? ATLAS_ORCH_STATE_FAILED : to;
    s = set_attempt_state(db, lr.attempt_id, attempt_end, true, err);
    if (s == ATLAS_OK) {
        /* Releasing the lease is what makes a duplicated completion harmless
         * and what lets a retry take a new one: the partial unique index
         * permits exactly one unreleased lease per job. */
        s = release_lease(db, lr.attempt_id, atlas_orch_reason_name(reason), err);
    }
    out->state = to;
    return s;
}

/* --- RECOVER -----------------------------------------------------------------
 *
 * Driven by the daemon's own timer and by startup reconciliation. Nothing
 * outside Atlas can ask for it, which is why a worker cannot cause another
 * worker's job to be expired.
 *
 * What it does, and the reasoning:
 *
 *   A lease that has expired means the worker stopped reporting. What happened
 *   to the work is genuinely unknown — the process may have died before
 *   starting, may be mid-run and merely unable to reach the daemon, or may have
 *   finished and crashed before committing. Atlas does not guess. It releases
 *   the lease and, if attempts remain, queues a new one; the previous attempt is
 *   recorded as having ended in TIMED_OUT with its reason, so a duplicate
 *   execution is visible in the history rather than invisible.
 *
 *   A job past its wall deadline ends, whatever state it was in.
 *
 *   A job whose attempts are exhausted and whose lease expired mid-flight is
 *   RECOVERY_REQUIRED rather than FAILED: "we do not know whether this ran" and
 *   "this ran and failed" are different answers, and collapsing them is the
 *   mistake A6 refuses to make about ancestry.
 */
static atlas_status op_recover(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                               atlas_err *err) {
    int64_t at_ms = op->now_ms > 0 ? op->now_ms : now_ms();

    /* Expired leases first. Collected before anything is changed, because the
     * loop below writes to the same tables it would otherwise be iterating. */
    int64_t ids[256];
    size_t n = 0;
    {
        static const char SEL[] =
            "SELECT l.attempt_id FROM orch_leases l WHERE l.released_at IS NULL"
            "   AND l.expires_ms <= ?1 ORDER BY l.id LIMIT 256;";
        sqlite3_stmt *q = NULL;
        atlas_status s = atlas_db_prepare(db, SEL, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(q, 1, at_ms);
        while (n < sizeof ids / sizeof ids[0] && sqlite3_step(q) == SQLITE_ROW) {
            ids[n++] = sqlite3_column_int64(q, 0);
        }
        atlas_db_finish(db, q);
    }

    for (size_t i = 0; i < n; i++) {
        int64_t attempt_id = ids[i];
        int64_t job_id = 0, attempt_no = 0;
        {
            static const char SEL[] =
                "SELECT job_id, attempt_no FROM orch_attempts WHERE id = ?1;";
            sqlite3_stmt *q = NULL;
            atlas_status s = atlas_db_prepare(db, SEL, &q, err);
            if (s != ATLAS_OK) {
                return s;
            }
            (void)sqlite3_bind_int64(q, 1, attempt_id);
            if (sqlite3_step(q) == SQLITE_ROW) {
                job_id = sqlite3_column_int64(q, 0);
                attempt_no = sqlite3_column_int64(q, 1);
            }
            atlas_db_finish(db, q);
        }
        if (job_id == 0) {
            continue;
        }
        job_row j;
        bool found = false;
        atlas_status s = job_by_id(db, job_id, &j, &found, err);
        if (s != ATLAS_OK) {
            return s;
        }
        if (!found || atlas_orch_state_is_terminal(j.state)) {
            /* A completed job whose lease was never released. Release it and
             * leave the outcome alone: a persisted completed job stays
             * completed, which is the first thing recovery must not break. */
            s = release_lease(db, attempt_id, "STALE_AFTER_TERMINAL", err);
            if (s != ATLAS_OK) {
                return s;
            }
            continue;
        }

        s = release_lease(db, attempt_id, "LEASE_EXPIRED", err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = set_attempt_state(db, attempt_id, ATLAS_ORCH_STATE_TIMED_OUT, true, err);
        if (s != ATLAS_OK) {
            return s;
        }

        atlas_orch_state to;
        atlas_orch_reason reason;
        if (j.cancel_requested) {
            to = ATLAS_ORCH_STATE_CANCELLED;
            reason = ATLAS_ORCH_REASON_CANCEL_CONFIRMED;
        } else if (at_ms >= j.deadline_ms) {
            to = ATLAS_ORCH_STATE_TIMED_OUT;
            reason = ATLAS_ORCH_REASON_WALL_TIMEOUT;
        } else if (attempt_no < j.max_attempts) {
            to = ATLAS_ORCH_STATE_QUEUED;
            reason = ATLAS_ORCH_REASON_LEASE_EXPIRED;
        } else {
            /* Attempts are gone and the last one's fate is unknown. Recorded as
             * exactly that. */
            to = ATLAS_ORCH_STATE_RECOVERY_REQUIRED;
            reason = ATLAS_ORCH_REASON_RECOVERY_AMBIGUOUS;
        }
        s = transition(db, &j, attempt_id, to, reason, ATLAS_ORCH_ACTOR_ATLAS, 0,
                       "the lease expired without a completion", NULL, err);
        if (s != ATLAS_OK) {
            return s;
        }
        out->expired++;
        if (to == ATLAS_ORCH_STATE_QUEUED) {
            out->retried++;
        } else if (to == ATLAS_ORCH_STATE_TIMED_OUT) {
            out->timed_out++;
        } else if (to == ATLAS_ORCH_STATE_RECOVERY_REQUIRED) {
            out->recovered++;
        }
    }

    /* Queued jobs past their wall deadline. A job that never got leased still
     * has to end somewhere, and a queue that silently held one forever would be
     * a job whose submitter is never told anything. */
    {
        static const char SEL[] =
            "SELECT id FROM orch_jobs WHERE state = 'QUEUED' AND deadline_ms <= ?1"
            " ORDER BY id LIMIT 256;";
        sqlite3_stmt *q = NULL;
        atlas_status s = atlas_db_prepare(db, SEL, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(q, 1, at_ms);
        n = 0;
        while (n < sizeof ids / sizeof ids[0] && sqlite3_step(q) == SQLITE_ROW) {
            ids[n++] = sqlite3_column_int64(q, 0);
        }
        atlas_db_finish(db, q);
        for (size_t i = 0; i < n; i++) {
            job_row j;
            bool found = false;
            s = job_by_id(db, ids[i], &j, &found, err);
            if (s != ATLAS_OK) {
                return s;
            }
            if (!found || j.state != ATLAS_ORCH_STATE_QUEUED) {
                continue;
            }
            s = transition(db, &j, 0, ATLAS_ORCH_STATE_TIMED_OUT, ATLAS_ORCH_REASON_WALL_TIMEOUT,
                           ATLAS_ORCH_ACTOR_ATLAS, 0, "the job was never leased", NULL, err);
            if (s != ATLAS_OK) {
                return s;
            }
            out->timed_out++;
        }
    }
    return ATLAS_OK;
}

/* --- the one write point ---------------------------------------------------- */

atlas_status atlas_orch_apply_in_tx(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                                    atlas_err *err) {
    if (db->read_only) {
        return atlas_err_set(err, ATLAS_ERR_DB,
                             "orchestration state cannot be written through a read-only handle");
    }
    switch (op->kind) {
    case ATLAS_ORCH_OP_SUBMIT: return op_submit(db, op, out, err);
    case ATLAS_ORCH_OP_CANCEL: return op_cancel(db, op, out, err);
    case ATLAS_ORCH_OP_LEASE: return op_lease(db, op, out, err);
    case ATLAS_ORCH_OP_HEARTBEAT: return op_heartbeat(db, op, out, err);
    case ATLAS_ORCH_OP_EVENT: return op_event(db, op, out, err);
    case ATLAS_ORCH_OP_COMPLETE: return op_complete(db, op, out, err);
    case ATLAS_ORCH_OP_RECOVER: return op_recover(db, op, out, err);
    case ATLAS_ORCH_OP_NONE: break;
    }
    return atlas_err_set(err, ATLAS_ERR_INTERNAL, "an empty orchestration operation was applied");
}

atlas_status atlas_orch_apply(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                              atlas_err *err) {
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_orch_apply_in_tx(db, op, out, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
        return st;
    }
    st = atlas_db_commit(db, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
    }
    return st;
}

/* --- reads ------------------------------------------------------------------ */

atlas_status atlas_db_orch_job_get(atlas_db *db, const char *uid, atlas_orch_job_view *out,
                                   bool *found, atlas_err *err) {
    static const char SQL[] =
        "SELECT job_uid, state, repo_name, source_commit, mode, driver, spec_digest,"
        "       submitter_uid, attempts_started, max_attempts, created_at, terminal_at,"
        "       cancel_requested, state_seq, task_text, correlation, wall_timeout_ms,"
        "       idle_timeout_ms, run_uid, parent_job_uid"
        "  FROM orch_jobs WHERE job_uid = ?1;";
    *found = false;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        s = atlas_db_col_copy(st, 0, out->job_uid, sizeof(out->job_uid), "job_uid", err);
        if (s == ATLAS_OK) {
            (void)atlas_orch_state_parse(atlas_db_col_text(st, 1), &out->state);
            s = atlas_db_col_copy(st, 2, out->repo_name, sizeof(out->repo_name), "repo_name", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 3, out->source_commit, sizeof(out->source_commit),
                                  "source_commit", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 4, out->mode, sizeof(out->mode), "mode", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 5, out->driver, sizeof(out->driver), "driver", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 6, out->spec_digest, sizeof(out->spec_digest), "spec_digest",
                                  err);
        }
        if (s == ATLAS_OK) {
            out->submitter_uid = sqlite3_column_int64(st, 7);
            out->attempts_started = sqlite3_column_int64(st, 8);
            out->max_attempts = sqlite3_column_int64(st, 9);
            s = atlas_db_col_copy(st, 10, out->created_at, sizeof(out->created_at), "created_at",
                                  err);
        }
        if (s == ATLAS_OK) {
            const char *t = atlas_db_col_text_opt(st, 11);
            s = atlas_db_col_copy(st, 11, out->terminal_at, sizeof(out->terminal_at),
                                  "terminal_at", t == NULL ? NULL : err);
            if (t == NULL) {
                out->terminal_at[0] = '\0';
                s = ATLAS_OK;
            }
        }
        if (s == ATLAS_OK) {
            out->cancel_requested = sqlite3_column_int64(st, 12) != 0;
            out->state_seq = sqlite3_column_int64(st, 13);
            s = atlas_buf_set_str(&out->task_text, atlas_db_col_text(st, 14), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 15, out->correlation, sizeof(out->correlation),
                                  "correlation", err);
        }
        if (s == ATLAS_OK) {
            out->wall_timeout_ms = sqlite3_column_int64(st, 16);
            out->idle_timeout_ms = sqlite3_column_int64(st, 17);
            s = atlas_db_col_copy(st, 18, out->run_uid, sizeof(out->run_uid), "run_uid", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 19, out->parent_job_uid, sizeof(out->parent_job_uid),
                                  "parent_job_uid", err);
        }
        if (s == ATLAS_OK) {
            *found = true;
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* --- the run, read and settled (A11.0) ------------------------------------ */

atlas_status atlas_db_orch_run_get(atlas_db *db, const char *run_uid, atlas_orch_run_view *out,
                                   bool *found, atlas_err *err) {
    *found = false;
    memset(out, 0, sizeof(*out));
    run_row r;
    bool got = false;
    atlas_status s = run_by_uid(db, run_uid, &r, &got, err);
    if (s != ATLAS_OK || !got) {
        return s;
    }
    s = copy_fixed(out->run_uid, sizeof(out->run_uid), r.run_uid, "run_uid", err);
    if (s == ATLAS_OK) {
        s = copy_fixed(out->root_job_uid, sizeof(out->root_job_uid), r.root_job_uid,
                                  "root_job_uid", err);
    }
    if (s == ATLAS_OK) {
        s = copy_fixed(out->repo_identity_hash, sizeof(out->repo_identity_hash),
                                  r.repo_identity_hash, "repo_identity_hash", err);
    }
    if (s != ATLAS_OK) {
        return s;
    }
    out->status = r.status;

    {
        static const char SQL[] = "SELECT created_at FROM orch_runs WHERE run_uid = ?1;";
        sqlite3_stmt *st = NULL;
        s = atlas_db_prepare(db, SQL, &st, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
        if (s == ATLAS_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                s = atlas_db_col_copy(st, 0, out->created_at, sizeof(out->created_at), "created_at",
                                      err);
            }
        }
        atlas_db_finish(db, st);
        if (s != ATLAS_OK) {
            return s;
        }
    }

    /* The active task, if there is one. Its state comes from the same row, so a
     * caller resuming after a restart cannot read a uid from one moment and a
     * state from another. */
    {
        static const char SQL[] = "SELECT job_uid, state FROM orch_jobs"
                                  "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE
                                  "  ORDER BY id LIMIT 1;";
        sqlite3_stmt *st = NULL;
        s = atlas_db_prepare(db, SQL, &st, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
        if (s == ATLAS_OK) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                s = atlas_db_col_copy(st, 0, out->active_job_uid, sizeof(out->active_job_uid),
                                      "job_uid", err);
                if (s == ATLAS_OK && !atlas_orch_state_parse(atlas_db_col_text(st, 1),
                                                             &out->active_state)) {
                    s = atlas_err_set(err, ATLAS_ERR_DB,
                                      "job %s holds a state Atlas does not recognise",
                                      out->active_job_uid);
                }
            } else if (rc != SQLITE_DONE) {
                s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a run's active task");
            }
        }
        atlas_db_finish(db, st);
    }
    *found = (s == ATLAS_OK);
    return s;
}

atlas_status atlas_db_orch_run_set_status(atlas_db *db, const char *run_uid,
                                          atlas_orch_run_status observed,
                                          atlas_orch_run_status want, atlas_err *err) {
    if (observed != ATLAS_ORCH_RUN_ACTIVE) {
        /* Includes UNKNOWN, deliberately. A caller that did not read the run
         * cannot settle it, and a terminal run is final. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a run can only be settled from ACTIVE, not from %s",
                             atlas_orch_run_status_name(observed));
    }
    if (want != ATLAS_ORCH_RUN_ACCEPTED && want != ATLAS_ORCH_RUN_BLOCKED) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a run cannot be moved to %s",
                             atlas_orch_run_status_name(want));
    }
    static const char UPD[] = "UPDATE orch_runs SET status = ?1, terminal_at = ?2"
                              "  WHERE run_uid = ?3 AND status = ?4;";
    sqlite3_stmt *q = NULL;
    atlas_status s = atlas_db_prepare(db, UPD, &q, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    const char *texts[] = {atlas_orch_run_status_name(want), at, run_uid,
                           atlas_orch_run_status_name(observed)};
    for (size_t i = 0; s == ATLAS_OK && i < sizeof texts / sizeof texts[0]; i++) {
        s = atlas_db_bind_text_opt(db, q, (int)i + 1, texts[i], err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_step_done(db, q, err);
    } else {
        atlas_db_finish(db, q);
        return s;
    }
    if (s != ATLAS_OK) {
        return s;
    }
    /* Exactly one changed row, the way every A8 compare-and-swap ends. Zero
     * means the run moved under the caller or does not exist, and the two are
     * reported as one because both mean "the state you named is not the state
     * that is there". */
    if (sqlite3_changes(db->h) != 1) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "run %s was not in %s, so it was not settled", run_uid,
                             atlas_orch_run_status_name(observed));
    }
    return ATLAS_OK;
}

void atlas_orch_job_view_init(atlas_orch_job_view *v) {
    memset(v, 0, sizeof(*v));
    atlas_buf_init(&v->task_text);
}

void atlas_orch_job_view_free(atlas_orch_job_view *v) {
    if (v != NULL) {
        atlas_buf_free(&v->task_text);
    }
}

atlas_status atlas_db_orch_job_list(atlas_db *db, long long submitter_uid, int64_t after_id,
                                    int64_t limit, atlas_orch_list_cb cb, void *ud,
                                    int64_t *count_out, int64_t *cursor_out, bool *more_out,
                                    atlas_err *err) {
    /* Bounded pagination, and the caller is always told whether more exist —
     * a page that silently ends is indistinguishable from the end of the list. */
    if (limit <= 0 || limit > ATLAS_ORCH_LIST_MAX) {
        limit = ATLAS_ORCH_LIST_MAX;
    }
    static const char SQL[] =
        "SELECT id, job_uid, state, repo_name, driver, created_at, attempts_started"
        "  FROM orch_jobs WHERE submitter_uid = ?1 AND id > ?2 ORDER BY id LIMIT ?3;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, submitter_uid);
    (void)sqlite3_bind_int64(st, 2, after_id);
    (void)sqlite3_bind_int64(st, 3, limit + 1);
    int64_t n = 0;
    int64_t cursor = after_id;
    bool more = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == limit) {
            more = true;
            break;
        }
        atlas_orch_list_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(st, 0);
        s = atlas_db_col_copy(st, 1, row.job_uid, sizeof(row.job_uid), "job_uid", err);
        if (s == ATLAS_OK) {
            (void)atlas_orch_state_parse(atlas_db_col_text(st, 2), &row.state);
            s = atlas_db_col_copy(st, 3, row.repo_name, sizeof(row.repo_name), "repo_name", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 4, row.driver, sizeof(row.driver), "driver", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 5, row.created_at, sizeof(row.created_at), "created_at",
                                  err);
        }
        if (s != ATLAS_OK) {
            break;
        }
        row.attempts_started = sqlite3_column_int64(st, 6);
        cursor = row.id;
        n++;
        if (cb != NULL) {
            s = cb(&row, ud, err);
            if (s != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_db_finish(db, st);
    if (count_out != NULL) {
        *count_out = n;
    }
    if (cursor_out != NULL) {
        *cursor_out = cursor;
    }
    if (more_out != NULL) {
        *more_out = more;
    }
    return s;
}

atlas_status atlas_db_orch_artifacts(atlas_db *db, const char *job_uid, int64_t artifact_id,
                                     bool want_content, atlas_orch_artifact_cb cb, void *ud,
                                     int64_t *count_out, atlas_err *err) {
    /* Joined to `orch_jobs` on the public uid rather than taking a job row id,
     * so a caller cannot reach an artifact by guessing an internal number: the
     * uid is unguessable and the caller's right to see the job is checked before
     * this is ever called. */
    static const char SQL[] =
        "SELECT a.id, at.attempt_no, a.name, a.kind, a.size_bytes, a.sha256, a.content_stored,"
        "       a.content"
        "  FROM orch_artifacts a"
        "  JOIN orch_attempts at ON at.id = a.attempt_id"
        "  JOIN orch_jobs j ON j.id = a.job_id"
        " WHERE j.job_uid = ?1 AND (?2 = 0 OR a.id = ?2)"
        " ORDER BY a.id LIMIT ?3;";
    if (count_out != NULL) {
        *count_out = 0;
    }
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, job_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    (void)sqlite3_bind_int64(st, 2, artifact_id);
    (void)sqlite3_bind_int64(st, 3, ATLAS_ORCH_MAX_ARTIFACT_COUNT);
    int64_t n = 0;
    while (s == ATLAS_OK && sqlite3_step(st) == SQLITE_ROW) {
        atlas_orch_artifact_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(st, 0);
        row.attempt_no = sqlite3_column_int64(st, 1);
        row.name = atlas_db_col_text(st, 2);
        row.kind = atlas_db_col_text(st, 3);
        row.size_bytes = sqlite3_column_int64(st, 4);
        row.sha256 = atlas_db_col_text(st, 5);
        row.content_stored = sqlite3_column_int64(st, 6) != 0;
        if (want_content && row.content_stored) {
            const void *blob = sqlite3_column_blob(st, 7);
            int len = sqlite3_column_bytes(st, 7);
            row.content = blob;
            row.content_len = len > 0 ? (size_t)len : 0u;
        }
        n++;
        if (cb != NULL) {
            s = cb(&row, ud, err);
        }
    }
    atlas_db_finish(db, st);
    if (s == ATLAS_OK && count_out != NULL) {
        *count_out = n;
    }
    return s;
}

/* --- A8: the snapshot manifest ------------------------------------------------
 *
 * Everything an attempt is entitled to receive, resolved from persisted state.
 * A worker names none of it. */

atlas_status atlas_db_orch_attempt_for_token(atlas_db *db, const char *token,
                                             int64_t *attempt_id_out, atlas_err *err) {
    *attempt_id_out = 0;
    lease_row lr;
    bool found = false;
    atlas_status s = lease_by_token(db, token, &lr, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    /* The same refusal text for every reason, as everywhere else a token is
     * checked: a worker learning *why* its token was refused learns something
     * about leases it does not hold. */
    if (!found || lr.released || lr.expires_ms <= now_ms()) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the presented lease is not valid for any active attempt");
    }
    *attempt_id_out = lr.attempt_id;
    return ATLAS_OK;
}

atlas_status atlas_db_orch_snapshot_source(atlas_db *db, int64_t attempt_id,
                                           atlas_orch_snapshot_source *out, atlas_err *err) {
    static const char SQL[] =
        "SELECT j.id, j.repo_name, j.source_commit, j.repo_identity_hash"
        "  FROM orch_attempts a JOIN orch_jobs j ON j.id = a.job_id"
        " WHERE a.id = ?1;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, attempt_id);
    char repo_name[ATLAS_ORCH_NAME_MAX + 1u];
    repo_name[0] = '\0';
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->job_id = sqlite3_column_int64(st, 0);
        s = atlas_db_col_copy(st, 1, repo_name, sizeof(repo_name), "repo_name", err);
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->commit, atlas_db_col_text(st, 2), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->identity, atlas_db_col_text(st, 3), err);
        }
    } else {
        s = atlas_err_set(err, ATLAS_ERR_USAGE, "no such attempt");
    }
    atlas_db_finish(db, st);
    if (s != ATLAS_OK) {
        return s;
    }

    /* The canonical path comes from the registry, never from the job and never
     * from a worker. And the repository must still have the durable identity the
     * job was created against: if it does not, the bytes it would hand over are
     * not the bytes the job was authorised over. */
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    s = atlas_db_repo_get(db, repo_name, &ri, &found, err);
    if (s == ATLAS_OK && !found) {
        s = atlas_err_set(err, ATLAS_ERR_REPO,
                          "the repository this job names is no longer registered");
    }
    if (s == ATLAS_OK) {
        atlas_buf ident = ATLAS_BUF_INIT;
        s = atlas_db_repo_identity_hash(db, ri.id, &ident, err);
        if (s == ATLAS_OK && strcmp(atlas_buf_cstr(&ident), atlas_buf_cstr(&out->identity)) != 0) {
            s = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                              "the registered repository no longer has the identity this job was "
                              "created against");
        }
        atlas_buf_free(&ident);
    }
    if (s == ATLAS_OK) {
        out->repo_id = ri.id;
        s = atlas_buf_set(&out->repo_root, ri.root_path.data, ri.root_path.len, err);
    }
    atlas_repo_info_free(&ri);
    return s;
}

atlas_status atlas_db_orch_snapshot_get(atlas_db *db, int64_t attempt_id,
                                        struct atlas_snapshot_meta *out, bool *found,
                                        atlas_err *err) {
    static const char SQL[] =
        "SELECT id, protocol, source_commit, tree_oid, entry_count, total_bytes, digest,"
        "       refused_symlinks, refused_gitlinks, refused_other, completed_at IS NOT NULL"
        "  FROM orch_snapshots WHERE attempt_id = ?1;";
    *found = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, attempt_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        /* Only a *completed* manifest counts. A half-enumerated one is not a
         * snapshot identity and must be enumerated again rather than served. */
        if (sqlite3_column_int64(st, 10) != 0) {
            out->snapshot_id = sqlite3_column_int64(st, 0);
            out->protocol = (int)sqlite3_column_int64(st, 1);
            s = atlas_db_col_copy(st, 2, out->commit, sizeof(out->commit), "commit", err);
            if (s == ATLAS_OK) {
                s = atlas_db_col_copy(st, 3, out->tree, sizeof(out->tree), "tree", err);
            }
            if (s == ATLAS_OK) {
                out->entries = sqlite3_column_int64(st, 4);
                out->total_bytes = sqlite3_column_int64(st, 5);
                s = atlas_db_col_copy(st, 6, out->digest, sizeof(out->digest), "digest", err);
            }
            if (s == ATLAS_OK) {
                out->refused_symlinks = sqlite3_column_int64(st, 7);
                out->refused_gitlinks = sqlite3_column_int64(st, 8);
                out->refused_other = sqlite3_column_int64(st, 9);
                *found = true;
            }
        }
    }
    atlas_db_finish(db, st);
    return s;
}

atlas_status atlas_db_orch_snapshot_create(atlas_db *db, int64_t attempt_id, const char *commit,
                                           const char *tree, int64_t *id_out, atlas_err *err) {
    /* An incomplete manifest from an earlier attempt at enumeration is replaced
     * whole; `completed_at` is what makes a row servable, so a leftover is inert
     * until then. */
    {
        static const char DEL[] =
            "DELETE FROM orch_snapshots WHERE attempt_id = ?1 AND completed_at IS NULL;";
        sqlite3_stmt *d = NULL;
        atlas_status ds = atlas_db_prepare(db, DEL, &d, err);
        if (ds != ATLAS_OK) {
            return ds;
        }
        (void)sqlite3_bind_int64(d, 1, attempt_id);
        ds = atlas_db_step_done(db, d, err);
        if (ds != ATLAS_OK) {
            return ds;
        }
    }
    static const char SQL[] =
        "INSERT INTO orch_snapshots(attempt_id, job_id, protocol, source_commit, tree_oid,"
        "  entry_count, total_bytes, digest, created_at)"
        " SELECT ?1, a.job_id, ?2, ?3, ?4, 0, 0, '', ?5 FROM orch_attempts a WHERE a.id = ?1;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    (void)sqlite3_bind_int64(st, 1, attempt_id);
    (void)sqlite3_bind_int(st, 2, ATLAS_SNAPSHOT_PROTOCOL);
    s = atlas_db_bind_text_opt(db, st, 3, commit, err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 4, tree, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 5, at, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    s = atlas_db_step_done(db, st, err);
    if (s == ATLAS_OK) {
        *id_out = sqlite3_last_insert_rowid(db->h);
        if (*id_out == 0) {
            s = atlas_err_set(err, ATLAS_ERR_USAGE, "no such attempt");
        }
    }
    return s;
}

atlas_status atlas_db_orch_snapshot_add_entry(atlas_db *db, int64_t snapshot_id, int64_t index,
                                              const void *path, size_t path_len, const char *mode,
                                              const char *oid, int64_t size, const char *sha256,
                                              atlas_err *err) {
    static const char SQL[] =
        "INSERT INTO orch_snapshot_entries(snapshot_id, idx, path, mode, oid, size_bytes, sha256)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, snapshot_id);
    (void)sqlite3_bind_int64(st, 2, index);
    /* A path is bytes, so it is bound as a blob. */
    s = atlas_db_bind_blob(db, st, 3, path, path_len, err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 4, mode, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 5, oid, err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 6, size);
        s = atlas_db_bind_text_opt(db, st, 7, sha256, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    return atlas_db_step_done(db, st, err);
}

atlas_status atlas_db_orch_snapshot_finish(atlas_db *db, int64_t snapshot_id,
                                           const struct atlas_snapshot_meta *meta,
                                           atlas_err *err) {
    static const char SQL[] =
        "UPDATE orch_snapshots SET entry_count = ?1, total_bytes = ?2, digest = ?3,"
        "  refused_symlinks = ?4, refused_gitlinks = ?5, refused_other = ?6, completed_at = ?7"
        " WHERE id = ?8;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    (void)sqlite3_bind_int64(st, 1, meta->entries);
    (void)sqlite3_bind_int64(st, 2, meta->total_bytes);
    s = atlas_db_bind_text_opt(db, st, 3, meta->digest, err);
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 4, meta->refused_symlinks);
        (void)sqlite3_bind_int64(st, 5, meta->refused_gitlinks);
        (void)sqlite3_bind_int64(st, 6, meta->refused_other);
        s = atlas_db_bind_text_opt(db, st, 7, at, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    (void)sqlite3_bind_int64(st, 8, snapshot_id);
    return atlas_db_step_done(db, st, err);
}

atlas_status atlas_db_orch_snapshot_entry(atlas_db *db, int64_t attempt_id, int64_t index,
                                          atlas_orch_snapshot_entry *out, atlas_buf *repo_root,
                                          atlas_err *err) {
    static const char SQL[] =
        "SELECT e.path, e.mode, e.oid, e.size_bytes, e.sha256, j.repo_name"
        "  FROM orch_snapshot_entries e"
        "  JOIN orch_snapshots s ON s.id = e.snapshot_id"
        "  JOIN orch_jobs j ON j.id = s.job_id"
        " WHERE s.attempt_id = ?1 AND e.idx = ?2 AND s.completed_at IS NOT NULL;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, attempt_id);
    (void)sqlite3_bind_int64(st, 2, index);
    char repo_name[ATLAS_ORCH_NAME_MAX + 1u];
    repo_name[0] = '\0';
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *p = sqlite3_column_blob(st, 0);
        int pn = sqlite3_column_bytes(st, 0);
        s = atlas_buf_set(&out->path, p, pn > 0 ? (size_t)pn : 0u, err);
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 1, out->mode, sizeof(out->mode), "mode", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 2, out->oid, sizeof(out->oid), "oid", err);
        }
        if (s == ATLAS_OK) {
            out->size_bytes = sqlite3_column_int64(st, 3);
            s = atlas_db_col_copy(st, 4, out->sha256, sizeof(out->sha256), "sha256", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 5, repo_name, sizeof(repo_name), "repo_name", err);
        }
    } else {
        s = atlas_err_set(err, ATLAS_ERR_USAGE, "no such snapshot entry");
    }
    atlas_db_finish(db, st);
    if (s != ATLAS_OK) {
        return s;
    }
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    s = atlas_db_repo_get(db, repo_name, &ri, &found, err);
    if (s == ATLAS_OK && !found) {
        s = atlas_err_set(err, ATLAS_ERR_REPO, "the repository is no longer registered");
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_set(repo_root, ri.root_path.data, ri.root_path.len, err);
    }
    atlas_repo_info_free(&ri);
    return s;
}

/* Clears the soft repository pointer when a repository is removed.
 *
 * A8 records do not cascade from `repositories`, for A4's reason: an FK would
 * make `repo remove --yes` destroy execution history. But `repositories.id` is a
 * reused rowid, so a pointer left behind would eventually name a different
 * repository. This runs in the same transaction as the delete.
 * `repo_identity_hash` is the durable identity and is deliberately untouched:
 * it is what the history is *about*, and clearing it would erase the only record
 * of which repository a job ran against. */
atlas_status atlas_db_orch_forget_repo(atlas_db *db, int64_t repo_id, atlas_err *err) {
    static const char SQL[] = "UPDATE orch_jobs SET repo_id = NULL WHERE repo_id = ?1;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, repo_id);
    return atlas_db_step_done(db, st, err);
}

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
#include "atlas/memory.h"
#include "atlas/orch_ops.h"
#include "atlas/orch_remote.h"
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
    atlas_buf_init(&op->failure_detail);
    op->failed_gate = -1;
    /* A12.1 T13. Always initialised whether or not this op ever becomes a
     * root-task SUBMIT: `atlas_orch_op_free` has one thing to do regardless of
     * `has_context_pack`. */
    atlas_memory_pack_init(&op->context_pack);
    atlas_buf_init(&op->touched_paths);
    /* A12.1 fix round, I1. `false` -- not `true` -- is the conservative
     * default the task directive asks for: "send the key always, and make
     * the absent reading the conservative one." A completion that never sets
     * this field explicitly is one that never made an observation, and
     * `false` (not complete) is the honest reading of that, never `true`
     * (complete). This is a new field in A12.1, so no older client's meaning
     * moves by changing its default. `reliance_check` (`src/db/db_orch.c`)
     * does not read this value at all while `op->touched_paths` is empty --
     * see I2 there -- so the default is inert for every case reached today;
     * it matters for the wire contract regardless. */
    op->touched_complete = false;
    /* A14, T3. The bearer token the gateway forwards; wiped by `atlas_orch_op_free`. */
    atlas_buf_init(&op->remote_token);
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
    atlas_buf_free(&op->failure_detail);
    atlas_memory_pack_free(&op->context_pack);
    atlas_buf_free(&op->touched_paths);
    /* A14, T3. Wipe the bearer token bytes before freeing, so the secret does
     * not survive in a dangling allocation.  `atlas_buf_free` frees but does
     * not zero; we wipe the full capacity (not just the used length) explicitly
     * first.  On `atlas_decision_op_free`'s precedent in
     * `src/decision/lifecycle.c`, which cites gateway.c's wipe of the login
     * key.  `test_i_op_free_wipes_token` in `tests/test_orch_remote.c` proves
     * the path runs (by inspection of this code rather than by reading freed
     * memory). */
    if (op->remote_token.data != NULL) {
        volatile unsigned char *z = (volatile unsigned char *)op->remote_token.data;
        for (size_t wi = 0; wi < op->remote_token.cap; wi++) {
            z[wi] = 0;
        }
    }
    atlas_buf_free(&op->remote_token);
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
    atlas_buf_init(&r->follow_up_job_uid);
    atlas_buf_init(&r->memory_package);
    atlas_buf_init(&r->context_pack);
    atlas_buf_init(&r->context_pack_status);
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
    atlas_buf_free(&r->follow_up_job_uid);
    atlas_buf_free(&r->memory_package);
    atlas_buf_free(&r->context_pack);
    atlas_buf_free(&r->context_pack_status);
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
    /* A14, T3. Empty for every job submitted before migration 32 (local jobs)
     * or when no remote credential was verified. */
    char submit_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
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
    if (s == ATLAS_OK) {
        /* A14, T3. Column 22: submit_key_id.  Empty for pre-migration-32 rows
         * and for local submissions; `atlas_db_col_copy` treats a NULL column
         * as an empty string, which is the right answer for both. */
        s = atlas_db_col_copy(st, 22, j->submit_key_id, sizeof(j->submit_key_id),
                              "submit_key_id", err);
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
        "       max_artifact_count, deadline_ms, cancel_requested, run_uid, parent_job_uid,"
        "       submit_key_id"
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
        "       max_artifact_count, deadline_ms, cancel_requested, run_uid, parent_job_uid,"
        "       submit_key_id"
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
                                      long long actor_uid, const char *detail, const char *key_id,
                                      int64_t *seq_out, atlas_err *err) {
    static const char SQL[] =
        "INSERT INTO orch_transitions(job_id, attempt_id, from_state, to_state, reason, actor,"
        "                             actor_uid, detail, at, key_id)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";
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
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 10, key_id != NULL ? key_id : "", err);
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
                                       actor_uid, detail, "", &seq, err);
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
    bool jfound = false;
    s = job_by_id(db, lr->job_id, j, &jfound, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!jfound) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the leased job no longer exists");
    }
    /* A11.5a-R2. The expiry is checked *after* the job is read, because whether
     * an expired lease is still usable depends on the job's wall deadline and on
     * whether Atlas was refusing writes.
     *
     * A released lease is refused above and unconditionally: release is what
     * happens when the sweep reclaims an attempt, so a released lease may have
     * been superseded by a newer one and a late completion under it could
     * overwrite another worker's result. Grace applies only to a lease that has
     * run out of time while nobody took it away — the case where the holder is
     * still the rightful one and simply could not be heard.
     *
     * Without this, `op_recover`'s grace was worth nothing: the sweep would
     * spare a worker and this function would reject the very heartbeat that
     * proved it alive. Measured in pilot 3 — the completion of a five-minute
     * worker was refused here as "not valid for any active attempt" after fifty
     * seconds of correct retrying. */
    if (lr->expires_ms <= at_ms &&
        !atlas_orch_lease_in_grace(j->deadline_ms, at_ms, op->contended_until_ms)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the presented lease is not valid for any active attempt");
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

/* A11.6. The drivers that work in the registered repository's own tree, as
 * SQLite must spell them.
 *
 * The same duplication `ORCH_SQL_ACTIVE_STATE` is, with the same defence:
 * `tests/test_orch_parallel.c` compares this list against
 * `atlas_orch_driver_is_repo_tree` over the whole of `atlas_drivers()`, in both
 * directions, so neither can gain a member the other lacks without a test
 * saying so. Adding a repo-tree driver therefore costs a migration as well —
 * `idx_orch_jobs_one_active_repo_tree` carries the same list and an index
 * predicate cannot be changed in place.
 *
 * It is used in exactly three places: the exclusivity check at submission, the
 * worker-start count that bounds the repo-tree chain, and the run view's claim
 * target. A fourth use is a decision, not a convenience. */
#define ORCH_SQL_REPO_TREE_DRIVER "driver IN ('claude-repo','fake-repo')"

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
    /* A11.6. How many tasks this run may hold active at once, fixed when the run
     * was created. At least 1 for every stored run — the schema's CHECK says so
     * — so a zero here would be a row nobody wrote correctly. */
    int64_t max_parallel;
} run_row;

static atlas_status run_by_uid(atlas_db *db, const char *uid, run_row *r, bool *found,
                               atlas_err *err) {
    static const char SQL[] =
        "SELECT id, run_uid, root_job_uid, repo_identity_hash, status, max_parallel"
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
        if (s == ATLAS_OK) {
            r->max_parallel = sqlite3_column_int64(st, 5);
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

/* A11.6. Names the run's active task that works in the repository's own tree, if
 * it has one. There is at most one by construction —
 * `idx_orch_jobs_one_active_repo_tree` — so this returns a uid rather than a
 * count for the reason `run_active_job` does: a refusal has to say *which* task
 * is in the way, and it is also the one task a run driver can claim. */
static atlas_status run_active_repo_tree_job(atlas_db *db, const char *run_uid,
                                             char out[ATLAS_ORCH_UID_MAX], bool *found,
                                             atlas_err *err) {
    static const char SQL[] = "SELECT job_uid FROM orch_jobs"
                              "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE
                              "    AND " ORCH_SQL_REPO_TREE_DRIVER " ORDER BY id LIMIT 1;";
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
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a run's active repository task");
    }
    atlas_db_finish(db, st);
    return s;
}

/* A11.6. How many tasks in the run are non-terminal.
 *
 * The number `max_parallel` bounds, and the number the settlement defers on: a
 * run is never accepted or blocked while this is above zero, because a task
 * that has not finished has not said what it did. Counted rather than inferred
 * from `run_active_job` for the obvious reason — "there is one" and "there are
 * two" are the same answer to a question that returns a uid. */
static atlas_status run_active_count(atlas_db *db, const char *run_uid, int64_t *out,
                                     atlas_err *err) {
    static const char SQL[] = "SELECT COUNT(*) FROM orch_jobs"
                              "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE ";";
    *out = 0;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            *out = sqlite3_column_int64(st, 0);
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count a run's active tasks");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* A11.6. The lowest slot in `[0, max_parallel)` no active task of the run holds.
 *
 * Inside the submit transaction, like every other check here, and for the same
 * reason: a slot that was free when it was read and taken before the insert is
 * not a free slot. The unique index is what makes that true rather than likely
 * — this function only decides *which* number to try, and a race loses on the
 * constraint rather than on the arithmetic.
 *
 * Lowest rather than next-highest so the numbering is a set of occupied slots
 * and not a counter: a run that has cycled through twenty tasks two at a time
 * has still only ever used slots 0 and 1, which is what keeps the schema's
 * ceiling meaningful. Failing to find one is not an error here — the caller has
 * already refused on capacity, so this is unreachable and says so. */
static atlas_status run_pick_slot(atlas_db *db, const char *run_uid, int64_t max_parallel,
                                  int64_t *out, atlas_err *err) {
    static const char SQL[] = "SELECT run_slot FROM orch_jobs"
                              "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE ";";
    *out = -1;
    unsigned taken = 0u;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
    if (s == ATLAS_OK) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            int64_t slot = sqlite3_column_int64(st, 0);
            if (slot >= 0 && slot < ATLAS_ORCH_RUN_MAX_PARALLEL) {
                taken |= 1u << (unsigned)slot;
            }
        }
        if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a run's occupied slots");
        }
    }
    atlas_db_finish(db, st);
    if (s != ATLAS_OK) {
        return s;
    }
    for (int64_t i = 0; i < max_parallel && i < ATLAS_ORCH_RUN_MAX_PARALLEL; i++) {
        if ((taken & (1u << (unsigned)i)) == 0u) {
            *out = i;
            return ATLAS_OK;
        }
    }
    return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                         "run %s has no free task slot below its bound of %lld, which the "
                         "capacity check should already have refused",
                         run_uid, (long long)max_parallel);
}

/* Decides which run the job being submitted belongs to, creating one when the
 * job is a root. On success `run_uid_out` holds the run this job joins and
 * `slot_out` the slot it occupies while it is active.
 *
 * A root task — one with no parent — always gets a fresh run and slot 0. A child
 * task joins its parent's, and every one of the seven conditions below is a
 * refusal rather than a repair. */
static atlas_status submit_resolve_run(atlas_db *db, const atlas_orch_spec *s, const char *job_uid,
                                       int64_t created_ms, atlas_orch_memory_mode mode,
                                       int64_t repo_id, int64_t max_parallel,
                                       bool has_context_pack, const atlas_memory_pack *context_pack,
                                       atlas_buf *run_uid_out, int64_t *slot_out, atlas_err *err) {
    *slot_out = 0;
    if (s->parent_job_uid.len == 0) {
        atlas_status st = atlas_orch_new_run_uid(run_uid_out, err);
        if (st != ATLAS_OK) {
            return st;
        }
        static const char INS[] =
            "INSERT INTO orch_runs(run_uid, root_job_uid, repo_identity_hash, status,"
            "  created_at, created_ms, max_parallel) VALUES(?1, ?2, ?3, 'ACTIVE', ?4, ?5, ?6);";
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
            /* A11.6. Resolved by the caller, never taken raw from the operation:
             * zero means "not stated" and is 1 here, which is what every run was
             * before the column existed. The schema's CHECK refuses anything
             * outside 1..8 whatever this function believes. */
            (void)sqlite3_bind_int64(q, 6, max_parallel >= 1 ? max_parallel : 1);
            st = atlas_db_step_done(db, q, err);
        } else {
            atlas_db_finish(db, q);
        }
        /* A10.1. The memory manifest is frozen here, in the transaction that
         * creates the run and before any task in it can be leased. Two things
         * follow, and both are the point:
         *
         *   - A submission that lands later cannot change what an already
         *     created run will be shown, because the package is already bytes.
         *   - A run that is still ACTIVE is not a candidate for any other run's
         *     package, so two arms of a comparison created before either runs
         *     cannot see each other however they are ordered afterwards.
         *
         * A follow-up task takes the branch below instead and freezes nothing:
         * it inherits its parent's run, and therefore its parent's package. */
        if (st == ATLAS_OK) {
            atlas_orch_memory_package pkg;
            atlas_orch_memory_package_init(&pkg);
            st = atlas_db_orch_memory_freeze(db, atlas_buf_cstr(run_uid_out), mode, repo_id,
                                             atlas_buf_cstr(&s->task_text),
                                             atlas_buf_cstr(&s->source_commit), &pkg, err);
            atlas_orch_memory_package_free(&pkg);
        }
        /* A12.1 T13, Decision 8. The Canonical Context Pack is frozen the same
         * way and for the same two reasons, in its own table under
         * `UNIQUE(run_uid)`: `atlas_memory_pack_freeze_in_tx` reads nothing and
         * writes one row, so it belongs here beside the freeze above rather
         * than after this transaction commits. `has_context_pack` is decided
         * before this point by `run_orch_build_pack`
         * (`src/daemon/writer.c`), which sets it from
         * `op->context_pack.memory_generation > 0` alone: true once a
         * generation has ever been produced for the repository, false when
         * none has. That is not quite "no registered source or no generation
         * yet" -- a repository whose declared sources were all deregistered
         * from the root-owned policy while an old generation still exists
         * still has one, so it still gets a row here (M4, T13 fix round;
         * `run_orch_build_pack`'s own comment carries the reasoning for
         * leaving it this way rather than adding a sources count). Either
         * way, a run with `has_context_pack` false gets no row, and delivery
         * later appends nothing at all -- not a shorter section, not a
         * sentence saying there is none. */
        if (st == ATLAS_OK && has_context_pack) {
            st = atlas_memory_pack_freeze_in_tx(db, atlas_buf_cstr(run_uid_out), context_pack, err);
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

    /* A11.6, and the fifth refusal. **A run holds one pin.**
     *
     * Every task in a run is authorised over the same tree, and the run's root
     * is where that tree was named. A sibling pinned to a different commit would
     * make the run's own answer ambiguous: ACCEPTED would mean "the gates passed
     * over this tree" for one task and over a different one for another, and no
     * reader could tell which. Compared against the *root*, not the immediate
     * parent, because the root is the run's pin and a chain cannot drift a
     * commit at a time. */
    {
        job_row root;
        bool root_found = false;
        st = job_by_uid(db, r.root_job_uid, &root, &root_found, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (!root_found) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "run %s names a root task that is gone",
                                 r.run_uid);
        }
        if (strcmp(root.source_commit, atlas_buf_cstr(&s->source_commit)) != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "run %s is pinned to commit %s and this submission names %s; a "
                                 "run holds one pin",
                                 r.run_uid, root.source_commit, atlas_buf_cstr(&s->source_commit));
        }
    }

    /* Capacity. The schema's `idx_orch_jobs_active_slot` would refuse an
     * over-subscription too, once every slot below the bound is taken; the check
     * is here so the caller gets a sentence naming a task in the way rather than
     * a constraint violation it cannot act on. That is M21's arrangement, kept. */
    char active[ATLAS_ORCH_UID_MAX];
    bool busy = false;
    st = run_active_job(db, p.run_uid, active, &busy, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t active_n = 0;
    st = run_active_count(db, p.run_uid, &active_n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t bound = r.max_parallel >= 1 ? r.max_parallel : 1;
    if (active_n >= bound) {
        if (bound == 1) {
            /* The default, and the sentence a run has given since A11.0. It is
             * spelled out separately rather than folded into the general form
             * because it is the one an operator who never asked for parallelism
             * will ever see, and "1 of 1" is a worse way to say "one". */
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "run %s already has an active task, %s; it takes no second one",
                                 r.run_uid, active);
        }
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "run %s already has %lld active task(s) including %s, which is its "
                             "bound of %lld; it takes no further task until one of them ends",
                             r.run_uid, (long long)active_n, active, (long long)bound);
    }

    /* Tree exclusivity, whatever the bound says. The registered repository has
     * one working tree, so two workers editing it at once is not a slower run,
     * it is an incoherent one. `idx_orch_jobs_one_active_repo_tree` is what
     * makes this a schema fact; this check is what makes it a sentence. */
    if (atlas_orch_driver_is_repo_tree(atlas_buf_cstr(&s->driver))) {
        char tree_active[ATLAS_ORCH_UID_MAX];
        bool tree_busy = false;
        st = run_active_repo_tree_job(db, p.run_uid, tree_active, &tree_busy, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (tree_busy) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "run %s already has an active task in the repository's own tree, "
                                 "%s; the tree is exclusive however many tasks a run allows",
                                 r.run_uid, tree_active);
        }
    }

    st = run_pick_slot(db, p.run_uid, bound, slot_out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_buf_set_str(run_uid_out, r.run_uid, err);
}

/* --- SUBMIT ---------------------------------------------------------------- */

static atlas_status op_submit(atlas_db *db, const atlas_orch_op *op, atlas_orch_result *out,
                              atlas_err *err) {
    const atlas_orch_spec *s = &op->spec;

    /* A14, T3. Remote path: verify the bearer credential inside this
     * transaction, before any other check.
     *
     * Verification is first — before shape checks, before the digest, before
     * the idempotency lookup — because the namespaced idempotency key cannot be
     * computed until the credential is verified (the namespace includes the
     * key_id), and because a check that follows verification but runs before the
     * insert is still inside the same transaction.
     *
     * On the local path (`remote_allowed_count == 0`) none of this block runs
     * and the op takes exactly the path it took before A14, except that
     * `spawn_follow_up` seeds `op->remote_key_id` with the parent's
     * `submit_key_id` so a follow-up inherits it — we copy it into the local
     * variable here so the INSERT at `?28` sees the inherited value. */
    char remote_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    /* Seed from the op field (set by `spawn_follow_up` for follow-ups, zero for
     * ordinary local submissions) rather than starting blind. */
    (void)snprintf(remote_key_id, sizeof(remote_key_id), "%s", op->remote_key_id);
    atlas_buf ns_key = ATLAS_BUF_INIT; /* namespaced idempotency key, if built */
    atlas_status rmt_st = ATLAS_OK;

    if (op->remote_allowed_count > 0) {
        rmt_st = atlas_orch_remote_verify(db, &op->remote_token,
                                          (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])
                                              op->remote_allowed_ids,
                                          op->remote_allowed_count, remote_key_id, err);
        if (rmt_st != ATLAS_OK) {
            return rmt_st;
        }

        /* Build the namespaced key when the caller supplied a client fragment.
         * The write point namespaces rather than using the raw client key, so
         * two credentials with the same client key are two independent
         * idempotency namespaces. */
        if (op->remote_client_key[0] != '\0') {
            rmt_st = atlas_orch_remote_idempotency_key(remote_key_id, op->remote_client_key,
                                                       &ns_key, err);
            if (rmt_st != ATLAS_OK) {
                atlas_buf_free(&ns_key);
                return rmt_st;
            }
        }
    }

    /* For the remote path, replace the spec's idempotency key pointer with the
     * namespaced one for the remainder of op_submit.  `s` is a const pointer so
     * we shadow it through a local copy that shares the spec's other fields but
     * carries the namespaced key.  The shadow is never freed — only `ns_key` is,
     * at the `done:` label — and the spec's own allocation is untouched. */
    atlas_orch_spec shadow_spec;
    if (ns_key.len > 0) {
        shadow_spec = *s; /* shallow copy: all buf members aliased, not owned */
        shadow_spec.idempotency_key = ns_key; /* override with namespaced key */
        s = &shadow_spec;
    }

    /* A11.1. A task that works in the registered repository's own tree declares
     * at least one verification gate, or it is not created.
     *
     * The reason is what "accepted" would otherwise mean. A11.1 settles a run
     * ACCEPTED when its task succeeded, and a task with no gates succeeds on the
     * worker's process outcome alone — which is the one thing this repository
     * has said since A8 is not a success claim. Without this check a caller
     * could reach that by submitting a gateless repo-tree job directly, so the
     * check is here, at the write point, and not only on the command that
     * usually creates one. It is the same reasoning `atlas_verify_intake` uses
     * for placing its refusals where the row is written rather than where the
     * request arrives.
     *
     * A follow-up inherits its parent's list, so this is checked once per chain
     * and satisfied by every task in it by construction. */
    if (atlas_orch_driver_is_repo_tree(atlas_buf_cstr(&s->driver)) && s->validation_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a task that runs in the repository's own tree must declare at "
                             "least one verification command; a run with none could only be "
                             "accepted on a worker's exit code, which is not a success claim");
    }

    /* A11.6. How many tasks the run being created may hold active at once.
     *
     * Refused rather than clamped, which is this file's rule for every bound: a
     * number quietly reduced is a run that behaves differently from the one that
     * was asked for and nobody is told. Refused rather than ignored on a child,
     * for A10.1's reason about `--memory` on a resume — a flag that is dropped
     * silently reads, afterwards, exactly like one that was honoured.
     *
     * Both checks are before the idempotency lookup, beside the gateless check
     * above and for the same reason: a submission that is malformed is malformed
     * whether or not a key resolves it to something that already exists. */
    if (op->run_max_parallel < 0 || op->run_max_parallel > ATLAS_ORCH_RUN_MAX_PARALLEL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a run may hold between 1 and %d tasks active at once; %lld is "
                             "outside that and is refused rather than reduced",
                             ATLAS_ORCH_RUN_MAX_PARALLEL, (long long)op->run_max_parallel);
    }
    if (op->run_max_parallel > 0 && s->parent_job_uid.len > 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "how many tasks a run may hold active is fixed when the run is "
                             "created, so it cannot be named on a task that joins one");
    }

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

    /* A14, T3. Budget checks — only for a remote op and only when a new row
     * will be created.  Duplicates do not spend budget: the idempotency lookup
     * above returns early when the key resolves, so if we are still here the
     * key is either absent or this is a local op.
     *
     * Follow-ups (parent_job_uid non-empty) count against the active budget but
     * NOT against the daily budget, because they are continuations of work that
     * was already counted on submission. */
    if (op->remote_allowed_count > 0 && remote_key_id[0] != '\0') {
        /* Active budget: count non-terminal rows for this key. */
        int64_t active_count = 0;
        st = atlas_db_orch_remote_active_count(db, remote_key_id, &active_count, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&ns_key);
            return st;
        }
        if (active_count >= op->remote_max_active) {
            atlas_status bs = atlas_err_set(
                err, ATLAS_ERR_USAGE,
                "credential %s already has %lld active remote job(s), which is its bound of %lld;"
                " it takes no further one until one of them ends",
                remote_key_id, (long long)active_count, (long long)op->remote_max_active);
            atlas_buf_free(&ns_key);
            return bs;
        }

        /* Daily budget: count root submissions today for this key (UTC). */
        if (s->parent_job_uid.len == 0) {
            /* Derive the UTC midnight string from the current wall clock.
             * `atlas_now_iso8601` emits the full ISO-8601 timestamp; the first
             * 10 chars are the YYYY-MM-DD date, and appending "T00:00:00Z"
             * gives the day's lexicographic floor for a >= comparison.
             *
             * If `atlas_now_iso8601` emits fractional seconds (it does not —
             * it emits "YYYY-MM-DDTHH:MM:SSZ"), this comparison would miss the
             * first second of every day.  This is a stated cost; the
             * implementation is correct for the format produced. */
            char now_buf[ATLAS_TS_MAX];
            atlas_now_iso8601(now_buf, sizeof(now_buf));
            char day_start[32];
            /* Copy the date part (10 chars) and append midnight UTC. */
            memset(day_start, 0, sizeof(day_start));
            memcpy(day_start, now_buf, 10u);
            memcpy(day_start + 10u, "T00:00:00Z", 10u);

            int64_t today_count = 0;
            st = atlas_db_orch_remote_today_count(db, remote_key_id, day_start, &today_count,
                                                  err);
            if (st != ATLAS_OK) {
                atlas_buf_free(&ns_key);
                return st;
            }
            if (today_count >= op->remote_max_per_day) {
                atlas_status bs = atlas_err_set(
                    err, ATLAS_ERR_USAGE,
                    "credential %s has submitted %lld job(s) today (UTC), which is its bound of"
                    " %lld; it takes no further one until tomorrow",
                    remote_key_id, (long long)today_count, (long long)op->remote_max_per_day);
                atlas_buf_free(&ns_key);
                return bs;
            }
        }
    }

    atlas_buf uid = ATLAS_BUF_INIT;
    atlas_buf paths = ATLAS_BUF_INIT;
    atlas_buf vals = ATLAS_BUF_INIT;
    atlas_buf run_uid = ATLAS_BUF_INIT;
    int64_t run_slot = 0;
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
                                /* UNKNOWN is the zero and is read as OFF. A default that
                                 * quietly enabled memory is the one mistake this milestone
                                 * cannot make. */
                                op->memory_mode == ATLAS_ORCH_MEMORY_MODE_BOUNDED
                                    ? ATLAS_ORCH_MEMORY_MODE_BOUNDED
                                    : ATLAS_ORCH_MEMORY_MODE_OFF,
                                /* A11.6. Zero is "not stated" and resolves to one,
                                 * which is what every run was before the column
                                 * existed. Out-of-range was refused above. */
                                op->repo_id, op->run_max_parallel > 0 ? op->run_max_parallel : 1,
                                /* A12.1 T13. Built by `run_orch` before this op ever
                                 * reached the transaction; a child submission always
                                 * carries `has_context_pack == false` because
                                 * `run_orch` only builds one for a root task. */
                                op->has_context_pack, &op->context_pack, &run_uid, &run_slot, err);
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
            "  parent_job_uid, idempotency_key, state, created_at, created_ms, deadline_ms,"
            "  run_uid, run_slot, submit_key_id)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,"
            "        ?21,?22,'QUEUED',?23,?24,?25,?26,?27,?28);";
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
            /* A11.6. The slot this task holds while it is active. Assigned in
             * this transaction by `submit_resolve_run`, and the unique index on
             * `(run_uid, run_slot)` is what refuses a second task that somehow
             * reached here believing the same number was free. */
            (void)sqlite3_bind_int64(q, 27, run_slot);
            /* A14, T3. The verified key id, or empty for a local submission.
             * For a gateway submission, `remote_key_id` was populated by
             * `atlas_orch_remote_verify` above.  For a follow-up,
             * `remote_key_id` was seeded from `op->remote_key_id`, which
             * `spawn_follow_up` copied from the parent's `submit_key_id`.
             * For any other local submission, `op->remote_key_id` is zero
             * (set by calloc via `atlas_orch_op_new`), so `remote_key_id`
             * is zero and the binding stores the empty string. */
            st = atlas_db_bind_text_opt(db, q, 28,
                                        remote_key_id[0] != '\0' ? remote_key_id : "", err);
            if (st == ATLAS_OK) {
                st = atlas_db_step_done(db, q, err);
            } else {
                atlas_db_finish(db, q);
            }
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
        /* A11.1. The actor the operation carried, and CLIENT when it carried
         * none — which every request from the IPC edge does. A follow-up task
         * is created by Atlas rather than by a client and its first ledger row
         * says ATLAS, because reading it later as a submission somebody made is
         * exactly the confusion the ledger exists to prevent.
         *
         * A14, T3. For a remote root submission, the ledger detail is the
         * frozen sentence that records the channel and the credential, not the
         * person behind it. For a local submission or a follow-up, the detail
         * is the spec digest as before ("" key_id, digest detail). */
        char rmt_detail[256];
        const char *detail_str = digest;
        const char *key_id_str = "";
        if (remote_key_id[0] != '\0' && s->parent_job_uid.len == 0) {
            /* Root remote submission: frozen detail sentence. */
            (void)snprintf(rmt_detail, sizeof(rmt_detail),
                           "submitted through the Atlas gateway with credential %s; this records"
                           " the channel and the credential, not which person or program"
                           " presented it",
                           remote_key_id);
            detail_str = rmt_detail;
            key_id_str = remote_key_id;
        } else if (remote_key_id[0] != '\0') {
            /* Follow-up of a remote submission: record the key but keep the
             * digest as detail — "submitted through the gateway" would be false
             * for Atlas-generated follow-up tasks. */
            key_id_str = remote_key_id;
        }
        st = record_transition(db, out->job_id, 0, ATLAS_ORCH_STATE_UNKNOWN,
                               ATLAS_ORCH_STATE_QUEUED, ATLAS_ORCH_REASON_SUBMITTED,
                               op->actor != ATLAS_ORCH_ACTOR_UNKNOWN ? op->actor
                                                                     : ATLAS_ORCH_ACTOR_CLIENT,
                               op->peer_uid, detail_str, key_id_str, &seq, err);
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

    /* A14, T3. Fill the result's key_id and the two post-insert budget counts.
     * These are reads inside the same transaction as the insert, so they include
     * the row just created.  On a local op or after a budget-refused early
     * return, `remote_key_id[0]` is '\0' and we skip this. */
    if (st == ATLAS_OK && remote_key_id[0] != '\0') {
        (void)snprintf(out->key_id, sizeof(out->key_id), "%s", remote_key_id);
        int64_t active_after = 0;
        st = atlas_db_orch_remote_active_count(db, remote_key_id, &active_after, err);
        if (st == ATLAS_OK) {
            out->remote_active = active_after;
            /* Daily count only makes sense for root submissions. */
            if (s->parent_job_uid.len == 0) {
                char now_buf2[ATLAS_TS_MAX];
                atlas_now_iso8601(now_buf2, sizeof(now_buf2));
                char day_start2[32];
                memset(day_start2, 0, sizeof(day_start2));
                memcpy(day_start2, now_buf2, 10u);
                memcpy(day_start2 + 10u, "T00:00:00Z", 10u);
                int64_t today_after = 0;
                st = atlas_db_orch_remote_today_count(db, remote_key_id, day_start2, &today_after,
                                                      err);
                if (st == ATLAS_OK) {
                    out->remote_today = today_after;
                }
            }
        }
    }

done:
    atlas_buf_free(&uid);
    atlas_buf_free(&run_uid);
    atlas_buf_free(&paths);
    atlas_buf_free(&vals);
    atlas_buf_free(&ns_key);
    return st;
}

/* A11.6. Settlement for a terminal state nobody completed, declared here and
 * defined below.
 *
 * It belongs beside the settlement machinery it wraps — the quiescence scan, the
 * follow-up and the run's status are one argument and are written as one — and
 * five of its callers are the operations below this line: a heartbeat that runs
 * out of renewals or of wall clock, a cancellation of a queued task, and a lease
 * that refuses to grant. A forward declaration is the honest way to write that;
 * moving the definition up here would put the smallest part of the argument
 * first and the reasoning nine hundred lines away from it. */
static atlas_status run_settle_without_op(atlas_db *db, const job_row *j, atlas_orch_state to,
                                          atlas_err *err);

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
    /* A14, T3. Ownership check for cancel, in this order:
     *
     * 1. A remote cancel (op->remote_allowed_count > 0): verify the credential
     *    inside this transaction and refuse "no such job" unless the verified
     *    key_id matches the job's submit_key_id.  The "did not authenticate"
     *    refusal sentence is frozen; we use it here even though the context is
     *    cancellation rather than submission, because there is no
     *    cancel-flavoured frozen sentence (plan gap, reported).
     *
     * 2. An operator cancel (op->peer_is_operator && job has a key): allow.
     *    The operator's uid is already trusted by SO_PEERCRED.  This flag is
     *    set by the IPC method from atlas_server_peer_is_operator(peer_uid),
     *    which is a SO_PEERCRED-derived predicate — never from a request
     *    parameter, so the gateway cannot set it.
     *
     * 3. Otherwise: the existing submitter_uid check. */
    if (op->remote_allowed_count > 0) {
        /* Remote cancel: verify the credential. */
        char cancel_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
        cancel_key_id[0] = '\0';
        s = atlas_orch_remote_verify(db, &op->remote_token,
                                     (const char (*)[ATLAS_APIKEY_SELECTOR_HEX + 1u])
                                         op->remote_allowed_ids,
                                     op->remote_allowed_count, cancel_key_id, err);
        if (s != ATLAS_OK) {
            /* Verification failure is "no such job": we do not reveal that the
             * job exists to a caller whose credential did not check out. */
            (void)s;
            return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
        }
        if (j.submit_key_id[0] == '\0' || strcmp(cancel_key_id, j.submit_key_id) != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
        }
    } else if (op->peer_is_operator && j.submit_key_id[0] != '\0') {
        /* Operator cancelling a remote job: allowed unconditionally. */
        (void)0;
    } else if (j.submit_key_id[0] != '\0') {
        /* A14, T3. The job was submitted remotely (it has a submit_key_id) but
         * this cancel carries no credential and is not from an operator.  The
         * dispatch says "the gateway holds no authority of its own — it carries
         * a bearer credential it received; the daemon verifies that credential
         * itself."  Without this check, a process running as the gateway uid
         * could cancel any keyed job purely on uid equality (submitter_uid is
         * the gateway's SO_PEERCRED uid for every remote job), which is exactly
         * the authority the gateway is not supposed to hold.
         * Refuse as "no such job" to avoid disclosing that the job exists. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    } else {
        /* Local cancel: the existing submitter_uid check.
         * A submitter may cancel its own jobs. Another client's job is reported
         * as absent rather than as forbidden: whether a job exists is itself
         * information, and a caller who may not act on it need not learn it. */
        if (j.submitter_uid != op->peer_uid) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
        }
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
        if (s == ATLAS_OK) {
            /* A queued task cancelled here may have been the last one its run
             * was waiting for, and a run whose last task ends on this path must
             * not stay ACTIVE forever. Quiescence is A11.6's contract and every
             * terminal producer owes it a check; a cancelled task is one nobody
             * answered, so the scan blocks the run. */
            s = run_settle_without_op(db, &j, ATLAS_ORCH_STATE_CANCELLED, err);
        }
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
    /* A11.1. A lease may name the job it wants.
     *
     * A8's dispatcher takes whatever is next, which is right for a queue it
     * owns. A run driver is resuming one particular chain and must claim *that*
     * run's active task or nothing — taking someone else's would start work the
     * operator did not ask this invocation for. The two forms share the
     * transaction, the attempt allocation and the lease, so there is still one
     * grant path; what differs is which row it picks.
     *
     * Naming a job that is not QUEUED grants nothing and is not an error: it is
     * the ordinary answer when another driver already holds it, and it is what
     * makes "two concurrent drivers start one worker" fall out of the machine
     * rather than out of a check. */
    static const char PICK[] =
        "SELECT id FROM orch_jobs WHERE state = 'QUEUED' AND cancel_requested = 0"
        " ORDER BY id;";
    static const char PICK_ONE[] =
        "SELECT id FROM orch_jobs WHERE job_uid = ?1 AND state = 'QUEUED'"
        "  AND cancel_requested = 0;";
    bool targeted = op->job_uid.len > 0;
    int64_t job_id = 0;
    {
        sqlite3_stmt *q = NULL;
        atlas_status s = atlas_db_prepare(db, targeted ? PICK_ONE : PICK, &q, err);
        if (s != ATLAS_OK) {
            atlas_orch_argv_free(&want[0]);
            return s;
        }
        if (targeted) {
            s = atlas_db_bind_text_opt(db, q, 1, atlas_buf_cstr(&op->job_uid), err);
            if (s != ATLAS_OK) {
                atlas_db_finish(db, q);
                atlas_orch_argv_free(&want[0]);
                return s;
            }
        }
        static const char DRV[] = "SELECT driver FROM orch_jobs WHERE id = ?1;";
        while (sqlite3_step(q) == SQLITE_ROW) {
            int64_t candidate = sqlite3_column_int64(q, 0);
            sqlite3_stmt *dq = NULL;
            if (atlas_db_prepare(db, DRV, &dq, err) != ATLAS_OK) {
                break;
            }
            (void)sqlite3_bind_int64(dq, 1, candidate);
            bool match = false;
            if (sqlite3_step(dq) == SQLITE_ROW) {
                const char *drv = atlas_db_col_text(dq, 0);
                if (want_n == 0 || want[0].count == 0) {
                    /* An unfiltered lease means "any", and A11.1 narrows what
                     * "any" covers: never a driver that works in the registered
                     * repository's own tree. The background dispatcher polls
                     * exactly this way, and without the exclusion it would take
                     * an A11 task, provision a workspace the driver does not
                     * use, run it somewhere it was not meant to run, and
                     * complete it — settling a run with no gate having run
                     * where the changes are. Refusing at the grant covers every
                     * dispatcher that exists and every one that might. */
                    match = !atlas_orch_driver_is_repo_tree(drv);
                } else {
                    for (size_t i = 0; i < want[0].count; i++) {
                        if (strcmp(drv, atlas_buf_cstr(&want[0].args[i])) == 0) {
                            match = true;
                            break;
                        }
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
        if (s == ATLAS_OK) {
            /* A grant that refused ends the task, and a run whose last task ends
             * on this path must not stay ACTIVE forever: quiescence is A11.6's
             * contract and every terminal producer owes it a check. Nothing
             * narrower answers a tree that is not the one the work was
             * authorised over, so this failure is unanswered and the run
             * blocks. */
            s = run_settle_without_op(db, &j, ATLAS_ORCH_STATE_FAILED, err);
        }
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
        if (s == ATLAS_OK) {
            /* The same obligation: a task whose attempts ran out at the grant
             * has ended, and a run whose last task ends here must not stay
             * ACTIVE forever. No completion arrived, so there is nothing to
             * build a follow-up from and the exhausted task is one nobody
             * answered — the scan blocks the run. */
            s = run_settle_without_op(db, &j, ATLAS_ORCH_STATE_FAILED, err);
        }
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
        /* A12.1 T13. Not read anywhere else in this function, but `run_orch`
         * (`src/daemon/writer.c`) needs it after this transaction commits, to
         * re-read the pack row and compute its delivery-time freshness with no
         * transaction open. Empty for a job that belongs to no run, same as
         * every field below that is conditioned on one. */
        {&out->run_uid, j.run_uid},
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
    /* A10.1. The run's frozen memory package, read in the same transaction as
     * the grant so that the bytes and the attempt they belong to are settled
     * together. A job that belongs to no run, or a run with no manifest, leaves
     * this empty — which is what every job submitted before migration 23 does,
     * and it is the conservative answer rather than an error. */
    if (s == ATLAS_OK && j.run_uid[0] != '\0') {
        atlas_orch_memory_package pkg;
        atlas_orch_memory_package_init(&pkg);
        bool have = false;
        atlas_orch_memory_mode mm = ATLAS_ORCH_MEMORY_MODE_UNKNOWN;
        s = atlas_db_orch_memory_get(db, j.run_uid, &pkg, &have, &mm, err);
        if (s == ATLAS_OK && have) {
            out->memory_mode = mm;
            memcpy(out->memory_digest, pkg.digest, sizeof(out->memory_digest));
            if (mm == ATLAS_ORCH_MEMORY_MODE_BOUNDED) {
                s = atlas_buf_set(&out->memory_package, pkg.package.data, pkg.package.len, err);
            }
        }
        atlas_orch_memory_package_free(&pkg);
    }
    /* A12.1 T13, Decision 8. The run's frozen Canonical Context Pack row,
     * read in the same transaction as the grant for the same reason the A10.1
     * package is: the bytes a worker may be shown and the attempt they belong
     * to must be settled together, not across two reads that could disagree
     * on a resume.
     *
     * Only the stored row -- `atlas_db_memory_pack_get` reads `memory_context_
     * packs` and nothing else, no process, no file. `context_pack` is `p->
     * rendered` verbatim, already `atlas_safe()`-encoded. `context_pack_status`
     * is deliberately **not** set here: freshness may open the tree
     * (`atlas_memory_pack_freshness`, gated on a non-empty pinned
     * `source_identity`) and A1 forbids that inside this transaction, so
     * `run_orch` computes it after this call returns and this commits. An
     * empty `context_pack` here (no row, or a job with no run) is what tells
     * `run_orch` there is nothing to compute freshness for at all. */
    if (s == ATLAS_OK && j.run_uid[0] != '\0') {
        atlas_memory_pack pack;
        atlas_memory_pack_init(&pack);
        bool pack_found = false;
        s = atlas_db_memory_pack_get(db, j.run_uid, &pack, &pack_found, err);
        if (s == ATLAS_OK && pack_found) {
            s = atlas_buf_set(&out->context_pack, pack.rendered.data, pack.rendered.len, err);
        }
        atlas_memory_pack_free(&pack);
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
        if (s == ATLAS_OK) {
            /* The task ended here, on a check-in, and no completion will ever
             * arrive for it. A run whose last task ends this way must not stay
             * ACTIVE forever — quiescence is A11.6's contract and every terminal
             * producer owes it a check. */
            s = run_settle_without_op(db, &j, ATLAS_ORCH_STATE_TIMED_OUT, err);
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
        if (s == ATLAS_OK) {
            /* The wall clock is what finally stops a job, and stopping it is
             * also the end of a task nobody will complete. Pilot A11.6-P is why
             * this line is here: a sibling hit its wall on a heartbeat, the run
             * had nothing else active, and it was still ACTIVE hours later. */
            s = run_settle_without_op(db, &j, ATLAS_ORCH_STATE_TIMED_OUT, err);
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
/* --- A11.1: the run's own axis, settled here and nowhere else ----------------
 *
 * A11.0 left one question open on purpose: a run's status is its own axis, no
 * task transition wrote it, and nothing in production produced ACCEPTED or
 * BLOCKED. *Who may decide* was named as A11.1's question. This is the answer,
 * and its shape is the answer's content.
 *
 * **Every settlement travels on a COMPLETE.** There is no `run.settle` RPC
 * method, no MCP tool and no gateway route, and `atlas_db_orch_run_set_status`
 * still has no standalone caller outside this file — so "a model payload cannot
 * accept a run" stays true by absence rather than by a check. A run is settled
 * in the same transaction as the task completion that justifies it, from facts
 * this function reads: the state the task machine put the job in, the ledger's
 * count of worker starts, and the repository identity checked again here.
 *
 * **A model's output reaches none of it.** The completion carries an exit
 * classification Atlas computed and a gate verdict Atlas ran; the model's
 * stdout, its result document, its prose and its exit code are evidence about a
 * process, never a claim this branch reads. `op->failure_detail` is the single
 * piece of untrusted text that survives, and it survives into exactly one
 * place: quoted, labelled, into a follow-up task's text.
 *
 * **It is scoped to the drivers A11.1 dispatches.** A run whose task ran under
 * an A8 workspace driver is not settled at all — nothing decided anything about
 * it, and A11.0's statement still holds for it unchanged.
 */

/* How many worker starts this run's repo-tree chain has spent.
 *
 * Derived from the ledger and stored nowhere. RUNNING is the state the run
 * driver records immediately *before* it execs, so the count is durable before
 * the worker exists: a crash spends budget exactly as a finished run does, and
 * a refusal that never reached a lease spends none — which is what makes
 * retrying a `BUSY` safe rather than merely likely to be safe.
 *
 * A stored counter would be a second place for this to be true, and the two
 * would disagree the first time a process died between the exec and the
 * increment.
 *
 * A11.6 restricts it to the run's repo-tree jobs, which is the subject
 * `ATLAS_ORCH_RUN_MAX_WORKER_STARTS` has always described: three starts of the
 * chain that edits the repository, with a follow-up between them. A parallel
 * workspace sibling runs somewhere else entirely and is bounded by its own
 * `max_attempts`; counting its starts here would let one run out of budget
 * because another was busy, and the failure that produced would be a chain
 * denied the follow-up its gate failure had earned. **This changes no existing
 * count**: before parallelism every job in a repo-tree run was a repo-tree job,
 * because a follow-up inherits its parent's driver. */
static atlas_status run_worker_starts(atlas_db *db, const char *run_uid, int64_t *out,
                                      atlas_err *err) {
    static const char SQL[] =
        "SELECT COUNT(*) FROM orch_transitions t JOIN orch_jobs j ON j.id = t.job_id"
        "  WHERE j.run_uid = ?1 AND t.to_state = 'RUNNING' AND j." ORCH_SQL_REPO_TREE_DRIVER ";";
    *out = 0;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            *out = sqlite3_column_int64(st, 0);
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot count a run's worker starts");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* The columns a follow-up inherits verbatim, which `job_row` deliberately does
 * not carry: they are large, they are UNTRUSTED_DATA, and every other reader of
 * a job row needs none of them. */
typedef struct job_spec_row {
    int spec_version;
    atlas_buf task_text;
    atlas_buf allowed_paths;
    atlas_buf validations;
    atlas_buf correlation;
} job_spec_row;

static void job_spec_row_init(job_spec_row *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->task_text);
    atlas_buf_init(&r->allowed_paths);
    atlas_buf_init(&r->validations);
    atlas_buf_init(&r->correlation);
}

static void job_spec_row_free(job_spec_row *r) {
    atlas_buf_free(&r->task_text);
    atlas_buf_free(&r->allowed_paths);
    atlas_buf_free(&r->validations);
    atlas_buf_free(&r->correlation);
}

static atlas_status job_spec_by_uid(atlas_db *db, const char *uid, job_spec_row *out, bool *found,
                                    atlas_err *err) {
    static const char SQL[] =
        "SELECT spec_version, task_text, allowed_paths, validations, correlation"
        "  FROM orch_jobs WHERE job_uid = ?1;";
    *found = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            out->spec_version = sqlite3_column_int(st, 0);
            const void *blob = sqlite3_column_blob(st, 1);
            int n = sqlite3_column_bytes(st, 1);
            s = atlas_buf_set(&out->task_text, blob != NULL ? blob : "", (size_t)(n > 0 ? n : 0),
                              err);
            if (s == ATLAS_OK) {
                s = atlas_buf_set_str(&out->allowed_paths, atlas_db_col_text(st, 2), err);
            }
            if (s == ATLAS_OK) {
                s = atlas_buf_set_str(&out->validations, atlas_db_col_text(st, 3), err);
            }
            if (s == ATLAS_OK) {
                s = atlas_buf_set_str(&out->correlation, atlas_db_col_text(st, 4), err);
            }
            *found = (s == ATLAS_OK);
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a job specification");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* Appends at most `cap` bytes of `text`, and says so when it stops.
 *
 * Truncation is never silent here: a follow-up task whose evidence stopped
 * mid-sentence, with nothing saying it did, is a task told a shorter story than
 * happened — the same complaint A5 makes about a clamped bound. */
static atlas_status append_bounded(atlas_buf *out, const char *text, size_t len, size_t cap,
                                   atlas_err *err) {
    if (text == NULL) {
        return ATLAS_OK;
    }
    size_t n = len < cap ? len : cap;
    atlas_status s = atlas_buf_append(out, text, n, err);
    if (s == ATLAS_OK && n < len) {
        s = atlas_buf_appendf(out, err, "\n[... %zu further bytes not shown ...]\n", len - n);
    }
    return s;
}

/* Renders one stored validation command as the argv it is, for naming the gate
 * that failed. The name comes from the job row and never from the caller: a
 * completion supplies an *index*, so it cannot tell a follow-up that a gate the
 * job never declared is the one to fix. */
static atlas_status gate_name(const char *encoded, int64_t which, atlas_buf *out, atlas_err *err) {
    atlas_orch_argv cmds[ATLAS_ORCH_MAX_VALIDATIONS];
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_init(&cmds[i]);
    }
    size_t n = 0;
    atlas_status s = atlas_orch_validations_decode(encoded, cmds, ATLAS_ORCH_MAX_VALIDATIONS, &n,
                                                   err);
    if (s == ATLAS_OK) {
        if (which < 0 || (size_t)which >= n) {
            s = atlas_err_set(err, ATLAS_ERR_USAGE,
                              "the completion names validation command %lld; this job declared %zu",
                              (long long)which, n);
        } else {
            for (size_t a = 0; s == ATLAS_OK && a < cmds[which].count; a++) {
                s = atlas_buf_appendf(out, err, "%s%s", a > 0 ? " " : "",
                                      atlas_buf_cstr(&cmds[which].args[a]));
            }
        }
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&cmds[i]);
    }
    return s;
}

/* The follow-up task's text.
 *
 * Deterministic, and assembled entirely from stored rows plus the one bounded
 * excerpt the completion carried. It quotes the **root** task's goal rather than
 * the immediate parent's, so a third-generation task states the same objective
 * as the first rather than a summary of a summary — and so the text is a
 * function of (root, parent, attempt) and nothing else, which is what lets the
 * idempotency key be one too.
 *
 * The constraints are restated because they are the boundary of what the worker
 * is being asked to do, and a task that carries the goal without them is a task
 * that has been told to do anything. They are also enforced elsewhere and by
 * other means — the gate is run by Atlas, the run is settled by Atlas, and a
 * worker cannot reach either — so this is instruction, never enforcement. */
static atlas_status follow_up_text(atlas_db *db, const job_row *parent, const job_spec_row *pspec,
                                   const atlas_orch_op *op, const char *root_job_uid,
                                   atlas_buf *out, atlas_err *err) {
    job_spec_row root;
    job_spec_row_init(&root);
    bool found = false;
    atlas_status s = job_spec_by_uid(db, root_job_uid, &root, &found, err);
    if (s == ATLAS_OK && !found) {
        s = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "run root task %s is gone", root_job_uid);
    }
    /* The root of a chain is the run's root task; if this job *is* it, its own
     * text is the goal. */
    const atlas_buf *goal = found ? &root.task_text : &pspec->task_text;

    if (s == ATLAS_OK) {
        s = atlas_buf_set_str(out,
                              "atlas-follow-up: the previous task in this run did not pass its "
                              "verification gates.\n\n", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_appendf(out, err, "original-goal:\n");
    }
    if (s == ATLAS_OK) {
        s = append_bounded(out, goal->data, goal->len, ATLAS_ORCH_TASK_MAX / 4u, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_appendf(out, err, "\n\nprevious-task: %s\nprevious-outcome: %s\n",
                              parent->uid, atlas_orch_exit_kind_name(op->exit_kind));
    }
    if (s == ATLAS_OK && op->failed_gate >= 0) {
        atlas_buf name = ATLAS_BUF_INIT;
        s = gate_name(atlas_buf_cstr(&pspec->validations), op->failed_gate, &name, err);
        if (s == ATLAS_OK) {
            s = atlas_buf_appendf(out, err, "failed-gate: %s\n", atlas_buf_cstr(&name));
        }
        atlas_buf_free(&name);
    }
    if (s == ATLAS_OK && op->failure_detail.len > 0) {
        s = atlas_buf_appendf(out, err, "gate-output (bounded excerpt, untrusted):\n");
        if (s == ATLAS_OK) {
            s = append_bounded(out, op->failure_detail.data, op->failure_detail.len,
                               ATLAS_ORCH_GATE_EXCERPT_MAX, err);
        }
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_appendf(
            out, err,
            "\nFix this failure and preserve the existing work in the tree.\n"
            "Constraints:\n"
            "- Work only inside the registered repository's own root.\n"
            "- Do not commit, push, deploy, restart a daemon, or run any destructive git\n"
            "  operation. Leave the working tree as you found it plus your changes.\n"
            "- Do not change the acceptance rules or the verification gates.\n"
            "- Report honestly. Your report is not an acceptance decision: Atlas runs the\n"
            "  gates itself and settles this run itself.\n");
    }
    job_spec_row_free(&root);
    return s;
}

/* Creates the run's next task.
 *
 * Through `op_submit` — the same write point, the same digest, the same
 * idempotency table, the same refusals — because a second insert path would
 * bypass exactly the checks a forger would want somewhere else. The parent is
 * already terminal when this runs, which is what frees its slot and lets
 * `idx_orch_jobs_active_slot` and `idx_orch_jobs_one_active_repo_tree` accept
 * the row.
 *
 * A11.6. The child op carries no `run_max_parallel`: a follow-up joins a run
 * rather than creating one, and stating a run property on it would be refused by
 * the check `op_submit` applies to any child. Every other refusal it passes by
 * construction — it inherits the parent's repository, driver, gates and pinned
 * commit, and the run has just been left with a free slot by the very completion
 * that is creating it.
 *
 * The idempotency key is derived from the failure it answers,
 * `a11.<parent>.<attempt>`, so a resumed or replayed completion of the same
 * attempt resolves to the task that already exists rather than making a second
 * one. That is belt and braces over the transaction and the partial unique
 * index; all three would have to fail together for a run to sprout two
 * follow-ups for one failure. */
static atlas_status spawn_follow_up(atlas_db *db, const atlas_orch_op *op, const job_row *parent,
                                    int64_t attempt_no, const run_row *run,
                                    atlas_orch_result *out, atlas_err *err) {
    job_spec_row pspec;
    job_spec_row_init(&pspec);
    bool found = false;
    atlas_status s = job_spec_by_uid(db, parent->uid, &pspec, &found, err);
    if (s == ATLAS_OK && !found) {
        s = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the completing job is gone");
    }

    atlas_orch_op *child = NULL;
    if (s == ATLAS_OK) {
        child = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
        if (child == NULL) {
            s = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        }
    }
    if (s == ATLAS_OK) {
        atlas_orch_spec *cs = &child->spec;
        /* Atlas is the actor. A follow-up is not a client submission and the
         * ledger says so; the row's `submitter_uid` stays the parent's, because
         * the task belongs to the same principal's work and the idempotency key
         * is scoped to that principal. */
        child->actor = ATLAS_ORCH_ACTOR_ATLAS;
        child->peer_uid = parent->submitter_uid;
        child->repo_id = parent->repo_id;
        child->now_ms = op->now_ms;
        /* A14, T3. A follow-up inherits the parent's `submit_key_id`.  The
         * child op is local (`remote_allowed_count == 0`), so no credential
         * re-verification happens.  `op_submit` seeds its local `remote_key_id`
         * variable from `op->remote_key_id` before the verify block runs, so
         * what we write here is what ends up in column 28 (`submit_key_id`)
         * of the child row.  No IPC method ever sets `remote_key_id` on a
         * local op — `calloc` zeroes it — so it is safe to carry the parent's
         * key_id here without risking an unintended override. */
        (void)snprintf(child->remote_key_id, sizeof(child->remote_key_id), "%s",
                       parent->submit_key_id);
        cs->spec_version = pspec.spec_version;
        cs->submitter_uid = parent->submitter_uid;
        s = atlas_buf_set_str(&cs->repo_name, parent->repo_name, err);
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&cs->repo_identity_hash, parent->repo_identity_hash, err);
        }
        /* The pinned commit is inherited, never re-resolved. A follow-up that
         * looked HEAD up again would be authorised over whatever the tree had
         * become, which is the one thing the pin exists to prevent. */
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&cs->source_commit, parent->source_commit, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&cs->mode, parent->mode, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&cs->driver, parent->driver, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&cs->correlation, atlas_buf_cstr(&pspec.correlation), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&cs->parent_job_uid, parent->uid, err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_appendf(&cs->idempotency_key, err, "a11.%s.%lld", parent->uid,
                                  (long long)attempt_no);
        }
        /* The gates are inherited **verbatim**. A follow-up cannot be given a
         * shorter list than the task it follows, because it is not given a list
         * at all — it is given the parent's, decoded and re-encoded by the same
         * canonical functions. */
        if (s == ATLAS_OK) {
            s = atlas_orch_paths_decode(atlas_buf_cstr(&pspec.allowed_paths), cs->allowed_paths,
                                        ATLAS_ORCH_MAX_ALLOWED_PATHS, &cs->allowed_path_count,
                                        err);
        }
        if (s == ATLAS_OK) {
            s = atlas_orch_validations_decode(atlas_buf_cstr(&pspec.validations), cs->validations,
                                              ATLAS_ORCH_MAX_VALIDATIONS, &cs->validation_count,
                                              err);
        }
        if (s == ATLAS_OK) {
            cs->wall_timeout_ms = parent->wall_timeout_ms;
            cs->idle_timeout_ms = parent->idle_timeout_ms;
            cs->max_attempts = parent->max_attempts;
            cs->max_output_bytes = parent->max_output_bytes;
            cs->max_artifact_bytes = parent->max_artifact_bytes;
            cs->max_artifact_count = parent->max_artifact_count;
            s = follow_up_text(db, parent, &pspec, op, run->root_job_uid, &cs->task_text, err);
        }
        /* Validated exactly as a client's specification is. A follow-up that
         * could not be submitted through the front door is one Atlas will not
         * create through the back one. */
        if (s == ATLAS_OK) {
            s = atlas_orch_spec_validate(cs, err);
        }
    }

    atlas_orch_result cres;
    atlas_orch_result_init(&cres);
    if (s == ATLAS_OK) {
        s = op_submit(db, child, &cres, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_buf_set(&out->follow_up_job_uid, cres.job_uid.data, cres.job_uid.len, err);
    }
    atlas_orch_result_free(&cres);
    atlas_orch_op_free(child);
    free(child);
    job_spec_row_free(&pspec);
    return s;
}



/* A11.6. Whether every task in the run ended well.
 *
 * "Well" is one of two things, and the second is why this is a scan rather than
 * a count of failures: a task that SUCCEEDED, or a task that FAILED **and has a
 * child in the same run** — a failure that was answered. A run whose gate failed
 * twice and whose third task passed did not fail; a run whose last task failed
 * with nothing following it did, and so did a run holding a task that was
 * cancelled, timed out, or ended in RECOVERY_REQUIRED.
 *
 * The asymmetry is deliberate and is stated in `docs/orchestration.md`: a
 * gateless workspace sibling can **veto** acceptance and can never grant it.
 * ACCEPTED still flows only from the gated repo-tree chain succeeding; a sibling
 * adds the requirement that it, too, ended well, and adds nothing else. */
static atlas_status run_every_task_ended_well(atlas_db *db, const char *run_uid, bool *out,
                                              atlas_err *err) {
    static const char SQL[] =
        "SELECT COUNT(*) FROM orch_jobs j"
        "  WHERE j.run_uid = ?1"
        "    AND NOT (j.state = 'SUCCEEDED'"
        "             OR (j.state = 'FAILED'"
        "                 AND EXISTS (SELECT 1 FROM orch_jobs c"
        "                              WHERE c.run_uid = j.run_uid"
        "                                AND c.parent_job_uid = j.job_uid)));";
    *out = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            *out = sqlite3_column_int64(st, 0) == 0;
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot scan a run's tasks");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* What a terminal task did to its run, and the whole of A11.1's authority.
 *
 * The answers, in this order, and the order is the argument:
 *
 *   1. **The run is already terminal.** Nothing happens. A settled run is
 *      final in both directions and takes no further task; a completion that
 *      arrives afterwards is recorded against its job and changes nothing else.
 *   2. **The task did not end.** A task that went back to the queue for another
 *      attempt settles nothing: the run is still ACTIVE with an active task in
 *      it, and there is nothing to decide.
 *   3. **The run is not one this milestone drives.** Settle-eligibility is the
 *      **root** task's driver, asked in C and never in SQL: a run whose root
 *      works in an A8 snapshot workspace has no A11.1 driver behind it, nothing
 *      has decided anything about it, and A11.0's answer — that a run's status
 *      is its own axis and nothing derives it — still stands for it unchanged.
 *      It is the root's driver rather than the completing task's because a
 *      parallel sibling is a workspace task and its completion must still be
 *      able to settle the repo-tree run it belongs to.
 *   4. **The repo-tree chain failed and can try again.** Exactly one narrower
 *      task, in this transaction, and the run stays ACTIVE. A crash and an
 *      exhausted attempt are answered this way as much as a failed gate is; a
 *      policy refusal never is, because the tree the work was authorised over is
 *      not the tree that is there and no task answers that.
 *   5. **Some task in the run is still active.** The run stays ACTIVE. This is
 *      A11.6's deferral and the whole of what parallelism changed here: a run is
 *      never ACCEPTED or BLOCKED while anything in it is unfinished, because a
 *      task that has not ended has not said what it did.
 *   6. **Nothing is active.** The run settles by scanning its tasks. ACCEPTED
 *      when every one of them ended well *and* the repository still has the
 *      identity the run was created against — checked again, here, because it
 *      was checked at lease time and that is not the same claim: a repository
 *      re-registered or replaced between the grant and now is not the one the
 *      work was authorised over. Otherwise BLOCKED.
 *
 * **Success is the task machine's word, not the worker's.** `to` is what
 * `atlas_orch_transition_allowed` and the completion branch produced from an
 * exit classification Atlas computed and a gate verdict Atlas ran. Nothing the
 * model wrote is read here, and there is no field on the operation through which
 * it could be.
 *
 * `op` and `out` are both NULL exactly when the terminal state was produced by
 * something that carries no completion — recovery, an expired heartbeat, a
 * cancelled queued task, a lease that refused to grant — and that is the single
 * discriminator: such a caller spawns nothing (step 4 is skipped), reports
 * nothing on a result it does not own, and treats a missing run row as nothing
 * to do rather than as corruption — a sweep that failed hard on one inconsistent
 * row would stop reclaiming every other job in the pass. */

/* A12.1 T13, Decision 8. The reliance check: intersects a completing job's
 * run's frozen pack against the driver's own touched-paths observation, and
 * records the result on the pack row.
 *
 * Silent -- and no query at all beyond the one pack read -- for a job that
 * belongs to no run, a run with no frozen pack, a pack with no flagged
 * anchor, and (fix round, I2) a completion that carries no observation at
 * all: "when the run's pack has flagged anchors" is the plan's own gate, and
 * none of those four cases has anything for this check to say.
 *
 * I2. The fourth case is not the same as "the driver observed zero touched
 * paths": `atlas_orch_paths_encode` with `count = 0` produces `"0:"`, length
 * 2, so a genuine "nothing changed" observation always leaves
 * `op->touched_paths.len > 0`. Truly empty (`len == 0`) only happens when the
 * driver never reached the gather step (a refusal before the worker ran, a
 * moved HEAD) or reached it and the gather itself failed
 * (`src/orch/rundriver.c`'s `gather_touched_paths` failure path) -- both are
 * "no evidence", never "evidence of nothing", A9.2.2's shape at this season's
 * newest surface. Gating on it here, before `op->touched_complete` is ever
 * read, is what makes `touched_complete`'s wire value irrelevant to this one
 * case regardless of I1's fix: a claim about completeness said nothing about
 * an observation that was never made, complete or otherwise, and
 * `reliance_checked` stays at its schema default (`0`, migration 28) exactly
 * as it does for the other three silent cases -- never a positive claim
 * manufactured from an absence.
 *
 * **This function writes to `memory_context_packs` and to nothing
 * `settle_run_at_quiescence` or `run_every_task_ended_well` reads.** It is
 * called from `op_complete` beside, never inside, the settlement call below,
 * and either order of the two calls would produce the same run status: a
 * reliance finding never gates, blocks or otherwise moves a verdict. Anchors,
 * never prose: `atlas_memory_pack_reliance_match` compares two lists of
 * `path_text` values and nothing it touches is read by a branch. */
static atlas_status reliance_check(atlas_db *db, const atlas_orch_op *op, const char *run_uid,
                                   atlas_err *err) {
    if (run_uid == NULL || run_uid[0] == '\0') {
        return ATLAS_OK;
    }
    atlas_memory_pack pack;
    atlas_memory_pack_init(&pack);
    bool found = false;
    atlas_status st = atlas_db_memory_pack_get(db, run_uid, &pack, &found, err);
    if (st != ATLAS_OK || !found || pack.flagged_anchors.len == 0) {
        atlas_memory_pack_free(&pack);
        return st;
    }
    if (op->touched_paths.len == 0) {
        /* I2. Nothing was ever observed for this completion -- see the
         * function comment. Silent, like the three cases just above. */
        atlas_memory_pack_free(&pack);
        return ATLAS_OK;
    }

    atlas_buf touched[ATLAS_MEMORY_MAX_TOUCHED_PATHS];
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_TOUCHED_PATHS; i++) {
        atlas_buf_init(&touched[i]);
    }
    size_t touched_n = 0;
    st = atlas_orch_paths_decode(atlas_buf_cstr(&op->touched_paths), touched,
                                 ATLAS_MEMORY_MAX_TOUCHED_PATHS, &touched_n, err);
    atlas_buf matched = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_memory_pack_reliance_match(&pack, touched, touched_n, &matched, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_memory_pack_reliance_set(db, run_uid, op->touched_complete,
                                               atlas_buf_cstr(&matched), err);
    }
    atlas_buf_free(&matched);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_TOUCHED_PATHS; i++) {
        atlas_buf_free(&touched[i]);
    }
    atlas_memory_pack_free(&pack);
    return st;
}

static atlas_status settle_run_at_quiescence(atlas_db *db, const atlas_orch_op *op,
                                             const job_row *j, atlas_orch_state to,
                                             int64_t attempt_no, int64_t starts,
                                             atlas_orch_result *out, atlas_err *err) {
    if (j->run_uid[0] == '\0') {
        return ATLAS_OK;
    }
    run_row r;
    bool found = false;
    atlas_status s = run_by_uid(db, j->run_uid, &r, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        if (op == NULL) {
            return ATLAS_OK;
        }
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "job %s names a run that does not exist",
                             j->uid);
    }
    if (out != NULL) {
        out->run_status = r.status;
    }
    if (atlas_orch_run_status_is_terminal(r.status) || !atlas_orch_state_is_terminal(to)) {
        return ATLAS_OK;
    }

    /* Settle-eligibility: the run's *root* driver, in C. */
    job_row root;
    bool root_found = false;
    s = job_by_uid(db, r.root_job_uid, &root, &root_found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!root_found || !atlas_orch_driver_is_repo_tree(root.driver)) {
        return ATLAS_OK;
    }

    /* The repo-tree chain's answer to its own failure: one narrower task.
     *
     * Only for a repo-tree task, because only the chain has a budget and only
     * the chain's failure is one a follow-up addresses; a workspace sibling that
     * failed is bounded by its own `max_attempts` and gets no second task from
     * here. POLICY_REFUSED is excluded for the reason the state exists — the
     * tree is not where the task was pinned, and nothing narrower fixes that.
     * CANCELLED and RECOVERY_REQUIRED are different states, so they never reach
     * this branch at all. */
    if (op != NULL && atlas_orch_driver_is_repo_tree(j->driver) &&
        to == ATLAS_ORCH_STATE_FAILED &&
        op->failure_reason != ATLAS_ORCH_REASON_POLICY_REFUSED &&
        starts < ATLAS_ORCH_RUN_MAX_WORKER_STARTS) {
        s = spawn_follow_up(db, op, j, attempt_no, &r, out, err);
        if (s == ATLAS_OK && out != NULL) {
            out->run_status = ATLAS_ORCH_RUN_ACTIVE;
        }
        return s;
    }

    /* A11.6. Quiescence. Anything still running has not said what it did, and a
     * run settled over it would be a verdict on work nobody has seen the end of. */
    int64_t active_n = 0;
    s = run_active_count(db, r.run_uid, &active_n, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (active_n > 0) {
        if (out != NULL) {
            out->run_status = ATLAS_ORCH_RUN_ACTIVE;
        }
        return ATLAS_OK;
    }

    bool all_well = false;
    s = run_every_task_ended_well(db, r.run_uid, &all_well, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (all_well) {
        /* The repository identity, re-checked at the moment of settlement, from
         * the **root** task: the run's pin and the run's repository are the
         * root's, and at quiescence the task that completed last may be a
         * sibling with nothing to say about either. When it does not match, the
         * run is BLOCKED rather than accepted — the honest answer, because Atlas
         * cannot tell what the worker changed and where. */
        atlas_buf identity = ATLAS_BUF_INIT;
        atlas_repo_info ri;
        atlas_repo_info_init(&ri);
        bool repo_ok = false;
        if (atlas_db_repo_get(db, root.repo_name, &ri, &repo_ok, err) == ATLAS_OK && repo_ok) {
            if (atlas_db_repo_identity_hash(db, ri.id, &identity, err) == ATLAS_OK) {
                repo_ok = strcmp(atlas_buf_cstr(&identity), root.repo_identity_hash) == 0;
            } else {
                repo_ok = false;
            }
        }
        atlas_buf_free(&identity);
        atlas_repo_info_free(&ri);
        atlas_err_init(err);
        all_well = repo_ok;
    }

    atlas_orch_run_status want = all_well ? ATLAS_ORCH_RUN_ACCEPTED : ATLAS_ORCH_RUN_BLOCKED;
    s = atlas_db_orch_run_set_status(db, r.run_uid, ATLAS_ORCH_RUN_ACTIVE, want, err);
    if (s == ATLAS_OK && out != NULL) {
        out->run_status = want;
    }
    return s;
}

/* A11.1. Settlement for every terminal producer that has no completion op.
 *
 * Recovery was the first of them and named the helper for a season. It is Atlas
 * saying, in the state's own words, that it does not know what ran: a task that
 * ends that way did not end well, so the run it belongs to cannot be accepted —
 * starting a fresh worker on top of a tree an unknown process may have been
 * half-way through editing is the opposite of what RECOVERY_REQUIRED means, and
 * leaving the run ACTIVE with no task in it would be a chain that can never be
 * resumed and never says why.
 *
 * It is not the only one. A heartbeat that reaches its renewal bound or its wall
 * deadline, a cancellation of a queued task, and a lease that refuses to grant
 * all end a task without a worker having reported anything, and a run whose last
 * task ended on one of those paths stays ACTIVE forever unless it settles here.
 * None of them carries an operation a follow-up could be built from, which is
 * exactly why they share this entry point and not `op_complete`'s: `op` and
 * `out` are NULL, so nothing is spawned and nothing is reported on a result this
 * caller does not own.
 *
 * A11.6 routes all of it through the same settlement helper rather than blocking
 * outright, which changes nothing at a bound of one and is what makes the run
 * wait for its other tasks when there are any. A recovery that merely re-queued
 * the task settles nothing either way — the run still has an active task and the
 * ordinary path will reach it. */
static atlas_status run_settle_without_op(atlas_db *db, const job_row *j, atlas_orch_state to,
                                          atlas_err *err) {
    return settle_run_at_quiescence(db, NULL, j, to, 0, 0, NULL, err);
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
/* A10.0. Records what an attempt cost, once.
 *
 * `INSERT OR IGNORE` against `UNIQUE(attempt_id)` is the whole idempotency
 * story, and it belongs to the schema rather than to this function: a completion
 * delivered twice — retried through a `BUSY` window, or offered again after a
 * driver lost its answer — leaves one row and does not double a total. It runs
 * inside the completion's transaction, so a usage row exists exactly when the
 * completion it describes does.
 *
 * Every count binds NULL when it was not observed. That is the point of the
 * table: an attempt whose usage never arrived did not cost zero, and a reader
 * adding a column of zeroes would produce a total that is confidently wrong. */
static atlas_status write_usage(atlas_db *db, const atlas_orch_op *op, const job_row *j,
                                int64_t attempt_id, int64_t attempt_no, atlas_err *err) {
    static const char INS[] =
        "INSERT OR IGNORE INTO orch_usage(attempt_id, job_id, run_uid, attempt_no, status,"
        "  provider, model, input_tokens, output_tokens, cache_creation_tokens,"
        "  cache_read_tokens, cost_micro_usd, duration_ms, api_duration_ms, turns,"
        "  exit_kind, source, created_at, digest)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19);";
    sqlite3_stmt *q = NULL;
    atlas_status s = atlas_db_prepare(db, INS, &q, err);
    if (s != ATLAS_OK) {
        return s;
    }
    const atlas_usage *u = &op->usage;
    (void)sqlite3_bind_int64(q, 1, attempt_id);
    (void)sqlite3_bind_int64(q, 2, j->id);
    s = atlas_db_bind_text_opt(db, q, 3, j->run_uid, err);
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(q, 4, attempt_no);
        s = atlas_db_bind_text_opt(db, q, 5, atlas_usage_status_name(u->status), err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, q, 6, u->provider, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, q, 7, u->model, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, q);
        return s;
    }
    const struct {
        int idx;
        bool has;
        int64_t v;
    } N[] = {
        {8, u->has_input, u->input_tokens},
        {9, u->has_output, u->output_tokens},
        {10, u->has_cache_creation, u->cache_creation_tokens},
        {11, u->has_cache_read, u->cache_read_tokens},
        {12, u->has_cost, u->cost_micro_usd},
        {13, u->has_duration, u->duration_ms},
        {14, u->has_api_duration, u->api_duration_ms},
        {15, u->has_turns, u->turns},
    };
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) {
        if (N[i].has) {
            (void)sqlite3_bind_int64(q, N[i].idx, N[i].v);
        } else {
            (void)sqlite3_bind_null(q, N[i].idx);
        }
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    atlas_buf doc = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u] = {0};
    if (atlas_usage_encode(u, &doc, err) == ATLAS_OK) {
        atlas_sha256_hex(doc.data != NULL ? doc.data : "", doc.len, digest);
    } else {
        atlas_err_init(err);
    }
    atlas_buf_free(&doc);
    s = atlas_db_bind_text_opt(db, q, 16, atlas_orch_exit_kind_name(op->exit_kind), err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, q, 17, "claude-stream-result", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, q, 18, at, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, q, 19, digest, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_step_done(db, q, err);
    } else {
        atlas_db_finish(db, q);
    }
    return s;
}

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
    s = write_usage(db, op, &j, lr.attempt_id, lr.attempt_no, err);
    if (s != ATLAS_OK) {
        return s;
    }
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

    /* A11.1. Whether this task belongs to a chain this milestone drives, and
     * how much of that chain's budget is already spent. Both are read *before*
     * the transition is decided, because the budget is one of the things that
     * decides it: a run with nothing left must not be handed a task that goes
     * back to the queue for an attempt nobody may start. */
    bool in_run = j.run_uid[0] != '\0' && atlas_orch_driver_is_repo_tree(j.driver);
    int64_t starts = 0;
    if (in_run) {
        s = run_worker_starts(db, j.run_uid, &starts, err);
        if (s != ATLAS_OK) {
            return s;
        }
        out->worker_starts = starts;
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
    } else if (in_run && op->failure_reason == ATLAS_ORCH_REASON_VALIDATION_FAILED) {
        /* A11.1. A gate failure is deterministic: the same task over the same
         * tree fails the same gate again, so another attempt at it is not a
         * retry, it is the same thing twice. The run's answer to a failed gate
         * is a *different, narrower task*, which is created below.
         *
         * This is why the retry branch and the follow-up branch are not one
         * mechanism. A crashed worker says nothing about the task; a failed
         * gate says something specific about it. */
        to = ATLAS_ORCH_STATE_FAILED;
        reason = ATLAS_ORCH_REASON_VALIDATION_FAILED;
    } else if (in_run && op->failure_reason == ATLAS_ORCH_REASON_POLICY_REFUSED) {
        /* A11.1. The run driver refused to start, or refused to judge what it
         * started, because the repository is not where the task was pinned. No
         * retry answers that and no narrower task answers it either: the tree
         * the work was authorised over is not the tree that is there. It ends
         * the task, and below it ends the run. */
        to = ATLAS_ORCH_STATE_FAILED;
        reason = ATLAS_ORCH_REASON_POLICY_REFUSED;
    } else if (lr.attempt_no < j.max_attempts &&
               (!in_run || starts < ATLAS_ORCH_RUN_MAX_WORKER_STARTS)) {
        /* Bounded retry with a recorded reason, and never an infinite loop: the
         * attempt counter is the bound and it only goes up. A11.1 adds the
         * run's own bound beside it, and the tighter of the two wins. */
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
    if (s == ATLAS_OK) {
        /* A12.1 T13. Beside, never inside, settlement -- see `reliance_check`'s
         * own comment for why the order of these two calls cannot change what
         * either produces.
         *
         * A12.1 fix round, I3. `reliance_check`'s own failures are this
         * function's -- a malformed stored `flagged_anchors`, a caller-supplied
         * `touched_paths` outside `ATLAS_MEMORY_MAX_TOUCHED_PATHS`
         * (`atlas_orch_paths_decode`, refused before this point is ever
         * reached for anything a peer controls directly), a decode failure in
         * `reliance_set`'s own read of the stored column -- never anything the
         * worker did. Propagating it would have failed this whole completion:
         * the transition and lease release above are inside the same
         * transaction as everything below, so a non-OK return here rolls all
         * of it back, the lease is never released, the attempt eventually
         * expires, and the run can reach BLOCKED from a memory-layer error --
         * a run status decided by the memory layer, exactly the coupling
         * Decision 8 forbids one layer up. So the failure is recorded as not
         * performed and the completion proceeds, never touching `s`: the same
         * shape the driver already uses for its own `gather_touched_paths`
         * failure (`src/orch/rundriver.c`, logged and never propagated) and
         * this file's own precedent a few hundred lines up, where a failed
         * `atlas_usage_encode` (`:2883`) resets `err` and substitutes a safe
         * default rather than failing the write it rides with. The candidate
         * this round rejected was failing loudly through the transaction: a
         * dispatcher-uid completion can already reach this only by feeding
         * `atlas_orch_paths_decode` a malformed or over-cap `touched_paths`
         * (self-harm, the review's own reason this is Important and not
         * Critical), and every other source is Atlas' own storage, where a
         * "loud" failure has no channel of its own to travel on that would not
         * itself become a silent BLOCKED -- the run driver's transport retries
         * only transport errors and treats anything else as fatal to the
         * whole invocation, leaving the lease to expire regardless. */
        if (reliance_check(db, op, j.run_uid, err) != ATLAS_OK) {
            atlas_err_init(err);
        }
    }
    if (s == ATLAS_OK) {
        /* A11.6. Every job that belongs to a run, not only a repo-tree one. The
         * helper decides settle-eligibility from the run's *root*, so a workspace
         * sibling's completion can bring its run to quiescence while a plain A8
         * workspace run still settles nothing. `starts` is the repo-tree chain's
         * budget and is zero for a sibling — which is safe, because the only
         * branch that reads it also requires a repo-tree completing job. */
        s = settle_run_at_quiescence(db, op, &j, to, lr.attempt_no, starts, out, err);
    }
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
/* The unreleased lease on an attempt, or zero. Used only by the contention
 * grace below, which has an attempt id and needs the lease row it belongs to. */
static int64_t lease_id_of(atlas_db *db, int64_t attempt_id) {
    static const char SEL[] =
        "SELECT id FROM orch_leases WHERE attempt_id = ?1 AND released_at IS NULL;";
    sqlite3_stmt *q = NULL;
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (atlas_db_prepare(db, SEL, &q, &ignore) != ATLAS_OK) {
        return 0;
    }
    (void)sqlite3_bind_int64(q, 1, attempt_id);
    int64_t id = 0;
    if (sqlite3_step(q) == SQLITE_ROW) {
        id = sqlite3_column_int64(q, 0);
    }
    atlas_db_finish(db, q);
    return id;
}

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

        /* A11.5a-R. The lease ran out, but did the holder fail to renew it, or
         * was Atlas refusing to listen?
         *
         * A heartbeat is an ordinary synchronous write and A9.2.6 refuses those
         * for the whole of an unbounded semantic pass. On the machine this was
         * found on, passes over a second registered repository ran 167-176 s
         * with 14-20 s between them, so writes were refused about ninety per
         * cent of the time and a sixty-second lease could not survive. A worker
         * that was alive, correct and ten minutes inside its wall deadline had
         * its attempt requeued as LEASE_EXPIRED and its result thrown away.
         *
         * So an expired lease is not judged while Atlas' own refusals are
         * recent. This sweep is itself a synchronous write, which is what makes
         * the test cheap rather than delicate: it can only run in the gaps
         * between passes, and a gap wide enough for it is wide enough for the
         * heartbeat that follows.
         *
         * Deferring is not extending. The lease is left exactly as it is and the
         * next sweep asks again, so a genuinely dead driver on a quiet machine
         * is still reclaimed on the following tick. What contention buys is
         * another question, never an answer. And it is checked *after* the wall
         * deadline below is not: `j.deadline_ms` is the submitter's bound and no
         * amount of contention moves it, which keeps "the wall clock is what
         * finally stops a job" literally true. The honest cost is that under
         * unbroken contention a dead driver is not distinguished from a live one
         * until that deadline. */
        if (!j.cancel_requested &&
            atlas_orch_lease_in_grace(j.deadline_ms, at_ms, op->contended_until_ms)) {
            /* Deferring has to move the lease, not merely decline to reclaim it.
             * `require_lease` refuses an expired lease independently of this
             * sweep — that is what stops a zombie overwriting a newer attempt's
             * result — so a lease left expired would keep the attempt alive here
             * and still refuse the very heartbeat that would prove it healthy.
             * The worker would be spared and silenced in the same breath.
             *
             * So Atlas grants the time it cost the holder. The grant is decided
             * here, from the daemon's own record of refusing writes, and never
             * from anything the holder claims about itself — a lease that could
             * be extended by asking is not a lease. It is bounded by the same
             * wall deadline checked above, which is why repeating it under
             * unbroken contention cannot run forever, and `renewals` is left
             * alone because this is not one: the worker has not been heard from,
             * and `ATLAS_ORCH_LEASE_MAX_RENEWALS` still counts only the times it
             * has. */
            static const char EXT[] =
                "UPDATE orch_leases SET expires_ms = ?1 WHERE id = ?2 AND released_at IS NULL;";
            sqlite3_stmt *q = NULL;
            s = atlas_db_prepare(db, EXT, &q, err);
            if (s != ATLAS_OK) {
                return s;
            }
            (void)sqlite3_bind_int64(q, 1, at_ms + ATLAS_ORCH_CONTENTION_GRACE_MS);
            (void)sqlite3_bind_int64(q, 2, lease_id_of(db, attempt_id));
            s = atlas_db_step_done(db, q, err);
            if (s != ATLAS_OK) {
                return s;
            }
            out->deferred++;
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
        s = run_settle_without_op(db, &j, to, err);
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
            s = run_settle_without_op(db, &j, ATLAS_ORCH_STATE_TIMED_OUT, err);
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
        "       idle_timeout_ms, run_uid, parent_job_uid, submit_key_id"
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
            s = atlas_db_col_copy(st, 20, out->submit_key_id, sizeof(out->submit_key_id),
                                  "submit_key_id", err);
        }
        if (s == ATLAS_OK) {
            *found = true;
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* --- the run, read and settled (A11.0) ------------------------------------ */

/* A10.0. See atlas/orch_ops.h. Every started attempt of every job in the run is
 * accounted for: the ones with a usage row are folded, and the ones without are
 * counted as missing rather than skipped, which is what keeps a run whose worker
 * never spawned from reporting a confident zero. */
atlas_status atlas_db_orch_run_usage(atlas_db *db, const char *run_uid, atlas_usage_run *out,
                                     atlas_err *err) {
    atlas_usage_run_init(out);
    if (run_uid == NULL || run_uid[0] == '\0') {
        return ATLAS_OK;
    }
    {
        static const char SEL[] =
            "SELECT u.status, u.input_tokens, u.output_tokens, u.cache_creation_tokens,"
            "       u.cache_read_tokens, u.cost_micro_usd, u.duration_ms, u.turns"
            "  FROM orch_usage u WHERE u.run_uid = ?1 ORDER BY u.id;";
        sqlite3_stmt *q = NULL;
        atlas_status s = atlas_db_prepare(db, SEL, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_db_bind_text_opt(db, q, 1, run_uid, err);
        if (s != ATLAS_OK) {
            atlas_db_finish(db, q);
            return s;
        }
        while (sqlite3_step(q) == SQLITE_ROW) {
            atlas_usage u;
            atlas_usage_init(&u);
            const char *st = atlas_db_col_text(q, 0);
            if (!atlas_usage_status_parse(st != NULL ? st : "", &u.status)) {
                u.status = ATLAS_USAGE_UNKNOWN;
            }
            const struct {
                int col;
                bool *has;
                int64_t *v;
            } N[] = {
                {1, &u.has_input, &u.input_tokens},
                {2, &u.has_output, &u.output_tokens},
                {3, &u.has_cache_creation, &u.cache_creation_tokens},
                {4, &u.has_cache_read, &u.cache_read_tokens},
                {5, &u.has_cost, &u.cost_micro_usd},
                {6, &u.has_duration, &u.duration_ms},
                {7, &u.has_turns, &u.turns},
            };
            for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) {
                if (sqlite3_column_type(q, N[i].col) != SQLITE_NULL) {
                    *N[i].has = true;
                    *N[i].v = sqlite3_column_int64(q, N[i].col);
                }
            }
            atlas_usage_run_fold(out, &u);
        }
        atlas_db_finish(db, q);
    }

    int64_t started = 0;
    {
        /* From the ledger, not from the usage rows: the denominator has to come
         * from what Atlas recorded starting, or a run with no usage at all would
         * divide by nothing and look complete. */
        static const char CNT[] =
            "SELECT count(*) FROM orch_attempts a JOIN orch_jobs j ON j.id = a.job_id"
            " WHERE j.run_uid = ?1;";
        sqlite3_stmt *q = NULL;
        atlas_status s = atlas_db_prepare(db, CNT, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_db_bind_text_opt(db, q, 1, run_uid, err);
        if (s != ATLAS_OK) {
            atlas_db_finish(db, q);
            return s;
        }
        if (sqlite3_step(q) == SQLITE_ROW) {
            started = sqlite3_column_int64(q, 0);
        }
        atlas_db_finish(db, q);
    }
    atlas_usage_run_settle(out, started, started);
    return ATLAS_OK;
}

/* A12.0. One job's cost, for a plan task's rollup. See `atlas/orch_ops.h`.
 *
 * Every attempt's row is folded, newest last, so `model` ends up being the one
 * the most recent attempt that named a model named. Cost and turns are summed
 * over the attempts that reported them and left absent when none did — the
 * distinction A10.0's nullable columns exist for, carried out to the reader
 * rather than collapsed into a zero here. */
atlas_status atlas_db_orch_job_usage(atlas_db *db, const char *job_uid,
                                     atlas_orch_job_usage *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    if (job_uid == NULL || job_uid[0] == '\0') {
        return ATLAS_OK;
    }
    static const char SQL[] = "SELECT u.model, u.cost_micro_usd, u.turns FROM orch_usage u"
                              "  JOIN orch_jobs j ON j.id = u.job_id"
                              " WHERE j.job_uid = ?1 ORDER BY u.attempt_no, u.id;";
    sqlite3_stmt *q = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &q, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, q, 1, job_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, q);
        return s;
    }
    while (s == ATLAS_OK && sqlite3_step(q) == SQLITE_ROW) {
        out->present = true;
        const char *model = atlas_db_col_text(q, 0);
        if (model != NULL && model[0] != '\0') {
            s = copy_fixed(out->model, sizeof(out->model), model, "model", err);
        }
        if (s == ATLAS_OK && sqlite3_column_type(q, 1) != SQLITE_NULL) {
            int64_t sum = 0;
            if (atlas_usage_add(out->cost_micro_usd, sqlite3_column_int64(q, 1), &sum)) {
                out->cost_micro_usd = sum;
                out->has_cost = true;
            }
        }
        if (s == ATLAS_OK && sqlite3_column_type(q, 2) != SQLITE_NULL) {
            int64_t sum = 0;
            if (atlas_usage_add(out->turns, sqlite3_column_int64(q, 2), &sum)) {
                out->turns = sum;
                out->has_turns = true;
            }
        }
    }
    atlas_db_finish(db, q);
    return s;
}

/* A14. The `reason` field of `job.remote_get`: the newest transition's reason
 * name for the given job.  Returns ATLAS_OK with `out[0] == '\0'` when the job
 * has no transitions (should not happen in practice, but is not an error).
 * Used only by the remote group — `job.get` does not yet report a reason. */
atlas_status atlas_db_orch_job_newest_reason(atlas_db *db, const char *job_uid,
                                             char out[64], atlas_err *err) {
    out[0] = '\0';
    if (job_uid == NULL || job_uid[0] == '\0') {
        return ATLAS_OK;
    }
    static const char SQL[] =
        "SELECT t.reason FROM orch_transitions t"
        " JOIN orch_jobs j ON j.id = t.job_id"
        " WHERE j.job_uid = ?1"
        " ORDER BY t.id DESC LIMIT 1;";
    sqlite3_stmt *q = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &q, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, q, 1, job_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, q);
        return s;
    }
    if (sqlite3_step(q) == SQLITE_ROW) {
        const char *r = atlas_db_col_text(q, 0);
        if (r != NULL) {
            s = copy_fixed(out, 64u, r, "reason", err);
        }
    }
    atlas_db_finish(db, q);
    return s;
}

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

    out->max_parallel = r.max_parallel >= 1 ? r.max_parallel : 1;

    /* A11.6. Every non-terminal task, which is the number `max_parallel` bounds.
     * Read before the claim target below, because a caller resuming a run wants
     * both and two reads of "is this run busy?" that could disagree is exactly
     * what putting the active task in the view avoided in the first place. */
    s = run_active_count(db, run_uid, &out->active_count, err);
    if (s != ATLAS_OK) {
        return s;
    }

    /* A11.6. Whether this run is one a run driver drives, from its **root**
     * task's driver and in C. It decides which of the two selects below names
     * the active task, and the difference is the whole of what the field means:
     *
     *   - For a run whose root works in the repository's own tree, the active
     *     task is the active **repo-tree** task. That is the one — and by
     *     `idx_orch_jobs_one_active_repo_tree` the only one — a run driver can
     *     claim, and it is empty when the chain is done while a workspace
     *     sibling is still going. That is an ordinary mid-run answer now, not an
     *     ending: `active_count` is what says whether anything is left.
     *   - For every other run it is the run's first active task, exactly as it
     *     has been since A11.0. Narrowing it there would empty the field for
     *     every plain A8 workspace run, which nothing asked for. */
    bool driver_run = false;
    {
        job_row root;
        bool root_found = false;
        s = job_by_uid(db, r.root_job_uid, &root, &root_found, err);
        if (s != ATLAS_OK) {
            return s;
        }
        driver_run = root_found && atlas_orch_driver_is_repo_tree(root.driver);
    }

    /* The active task, if there is one. Its state comes from the same row, so a
     * caller resuming after a restart cannot read a uid from one moment and a
     * state from another. */
    {
        static const char SQL[] = "SELECT job_uid, state FROM orch_jobs"
                                  "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE
                                  "  ORDER BY id LIMIT 1;";
        static const char SQL_TREE[] = "SELECT job_uid, state FROM orch_jobs"
                                       "  WHERE run_uid = ?1 AND " ORCH_SQL_ACTIVE_STATE
                                       "    AND " ORCH_SQL_REPO_TREE_DRIVER
                                       "  ORDER BY id LIMIT 1;";
        sqlite3_stmt *st = NULL;
        s = atlas_db_prepare(db, driver_run ? SQL_TREE : SQL, &st, err);
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
        "SELECT id, job_uid, state, repo_name, driver, created_at, attempts_started,"
        "       submit_key_id"
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
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 7, row.submit_key_id, sizeof(row.submit_key_id),
                                  "submit_key_id", err);
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

/* A14. Shared body for the two remote-listing functions. The only differences
 * are the WHERE clause and the number of bound parameters, so both callers
 * build the statement with this helper rather than duplicating the pagination
 * loop. */
static atlas_status orch_job_list_impl(atlas_db *db, sqlite3_stmt *st, int64_t after_id,
                                       int64_t limit, atlas_orch_list_cb cb, void *ud,
                                       int64_t *count_out, int64_t *cursor_out, bool *more_out,
                                       atlas_err *err) {
    int64_t n = 0;
    int64_t cursor = after_id;
    bool more = false;
    atlas_status s = ATLAS_OK;
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
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 7, row.submit_key_id, sizeof(row.submit_key_id),
                                  "submit_key_id", err);
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

atlas_status atlas_db_orch_job_list_by_key(atlas_db *db, const char *key_id, int64_t after_id,
                                           int64_t limit, atlas_orch_list_cb cb, void *ud,
                                           int64_t *count_out, int64_t *cursor_out,
                                           bool *more_out, atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_ORCH_LIST_MAX) {
        limit = ATLAS_ORCH_LIST_MAX;
    }
    static const char SQL[] =
        "SELECT id, job_uid, state, repo_name, driver, created_at, attempts_started,"
        "       submit_key_id"
        "  FROM orch_jobs WHERE submit_key_id = ?1 AND id > ?2 ORDER BY id LIMIT ?3;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, key_id != NULL ? key_id : "", err);
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 2, after_id);
        (void)sqlite3_bind_int64(st, 3, limit + 1);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    return orch_job_list_impl(db, st, after_id, limit, cb, ud, count_out, cursor_out, more_out,
                              err);
}

atlas_status atlas_db_orch_job_list_remote(atlas_db *db, int64_t after_id, int64_t limit,
                                           atlas_orch_list_cb cb, void *ud, int64_t *count_out,
                                           int64_t *cursor_out, bool *more_out, atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_ORCH_LIST_MAX) {
        limit = ATLAS_ORCH_LIST_MAX;
    }
    static const char SQL[] =
        "SELECT id, job_uid, state, repo_name, driver, created_at, attempts_started,"
        "       submit_key_id"
        "  FROM orch_jobs WHERE submit_key_id <> '' AND id > ?1 ORDER BY id LIMIT ?2;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, after_id);
    (void)sqlite3_bind_int64(st, 2, limit + 1);
    return orch_job_list_impl(db, st, after_id, limit, cb, ud, count_out, cursor_out, more_out,
                              err);
}

atlas_status atlas_db_orch_remote_active_count(atlas_db *db, const char *key_id, int64_t *out,
                                               atlas_err *err) {
    /* The NOT IN predicate must stay in sync with `atlas_orch_state_is_terminal`
     * and with `idx_orch_jobs_state`'s WHERE clause. The spelling here is the
     * third copy of the terminal set (after the C function and the index); all
     * three are compared against each other in `test_orch_run.c`. */
    static const char SQL[] =
        "SELECT COUNT(*) FROM orch_jobs"
        " WHERE submit_key_id = ?1"
        "   AND state NOT IN ('SUCCEEDED','FAILED','CANCELLED','TIMED_OUT','RECOVERY_REQUIRED');";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, key_id != NULL ? key_id : "", err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    int64_t v = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
    }
    atlas_db_finish(db, st);
    if (out != NULL) {
        *out = v;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_orch_remote_today_count(atlas_db *db, const char *key_id,
                                              const char *utc_day_start, int64_t *out,
                                              atlas_err *err) {
    /* Counts root submissions only: `parent_job_uid = ''` excludes follow-up
     * tasks. `created_at >= utc_day_start` partitions by UTC calendar day,
     * where `utc_day_start` is an ISO-8601 UTC midnight string supplied by the
     * caller (TEXT comparison is correct for the ISO-8601 lexicographic
     * ordering SQLite uses). */
    static const char SQL[] =
        "SELECT COUNT(*) FROM orch_jobs"
        " WHERE submit_key_id = ?1 AND parent_job_uid = '' AND created_at >= ?2;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, key_id != NULL ? key_id : "", err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, utc_day_start != NULL ? utc_day_start : "", err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    int64_t v = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        v = sqlite3_column_int64(st, 0);
    }
    atlas_db_finish(db, st);
    if (out != NULL) {
        *out = v;
    }
    return ATLAS_OK;
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

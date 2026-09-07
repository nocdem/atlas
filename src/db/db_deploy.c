/* Atlas - A17 T1: the deploy vocabulary and the deploy write point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `atlas/deploy.h` for the state machine this file implements. Everything
 * that writes `deploys` or `deploy_transitions` lives here, and there is no
 * second implementation -- A8's `atlas_orch_apply_in_tx` rule, kept for a
 * second table rather than folded into the first one.
 *
 * Every `_in_tx` function assumes an open transaction: it neither begins nor
 * commits one. `atlas_db_deploy_confirmed_uid` must be askable from inside
 * A11.0's own submit transaction (a later task's wiring); the four writers
 * must be askable from inside the transaction a daemon RPC method opens once
 * per call, so that a credential check and the write it gates are one atomic
 * decision rather than two racing ones.
 */
#define _GNU_SOURCE 1

#include "atlas/deploy.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/apikey.h"
#include "atlas/hmac.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "db_internal.h"

/* --- the state vocabulary --------------------------------------------------- */

const char *atlas_deploy_state_name(atlas_deploy_state s) {
    /* No `default:`: adding a member to the vocabulary is a build failure
     * here rather than a row silently reporting UNKNOWN. */
    switch (s) {
    case ATLAS_DEPLOY_UNKNOWN: return "UNKNOWN";
    case ATLAS_DEPLOY_PROPOSED: return "PROPOSED";
    case ATLAS_DEPLOY_CONFIRMED: return "CONFIRMED";
    case ATLAS_DEPLOY_SUCCEEDED: return "SUCCEEDED";
    case ATLAS_DEPLOY_FAILED: return "FAILED";
    case ATLAS_DEPLOY_CANCELLED: return "CANCELLED";
    }
    return "UNKNOWN";
}

bool atlas_deploy_state_parse(const char *s, atlas_deploy_state *out) {
    static const atlas_deploy_state ALL[] = {ATLAS_DEPLOY_PROPOSED, ATLAS_DEPLOY_CONFIRMED,
                                             ATLAS_DEPLOY_SUCCEEDED, ATLAS_DEPLOY_FAILED,
                                             ATLAS_DEPLOY_CANCELLED};
    if (s == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        if (strcmp(s, atlas_deploy_state_name(ALL[i])) == 0) {
            *out = ALL[i];
            return true;
        }
    }
    /* "UNKNOWN" is deliberately not parseable, on `atlas_orch_state_parse`'s
     * precedent: nothing may ask a stored row to hold the value that means
     * "nobody wrote this row correctly". */
    return false;
}

bool atlas_deploy_state_is_terminal(atlas_deploy_state s) {
    switch (s) {
    case ATLAS_DEPLOY_SUCCEEDED:
    case ATLAS_DEPLOY_FAILED:
    case ATLAS_DEPLOY_CANCELLED: return true;
    case ATLAS_DEPLOY_UNKNOWN:
    case ATLAS_DEPLOY_PROPOSED:
    case ATLAS_DEPLOY_CONFIRMED: return false;
    }
    return false;
}

const char *atlas_deploy_actor_name(atlas_deploy_actor a) {
    switch (a) {
    case ATLAS_DEPLOY_ACTOR_UNKNOWN: return "UNKNOWN";
    case ATLAS_DEPLOY_ACTOR_REMOTE_CREDENTIAL: return "REMOTE_CREDENTIAL";
    case ATLAS_DEPLOY_ACTOR_REMOTE_OPERATOR_CONFIRMED: return "REMOTE_OPERATOR_CONFIRMED";
    case ATLAS_DEPLOY_ACTOR_DEPLOY_AGENT: return "DEPLOY_AGENT";
    }
    return "UNKNOWN";
}

bool atlas_deploy_actor_parse(const char *s, atlas_deploy_actor *out) {
    static const atlas_deploy_actor ALL[] = {ATLAS_DEPLOY_ACTOR_REMOTE_CREDENTIAL,
                                             ATLAS_DEPLOY_ACTOR_REMOTE_OPERATOR_CONFIRMED,
                                             ATLAS_DEPLOY_ACTOR_DEPLOY_AGENT};
    if (s == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        if (strcmp(s, atlas_deploy_actor_name(ALL[i])) == 0) {
            *out = ALL[i];
            return true;
        }
    }
    return false;
}

/* --- row ownership ----------------------------------------------------------- */

void atlas_deploy_row_init(atlas_deploy_row *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->deploy_uid);
    atlas_buf_init(&r->job_uid);
    atlas_buf_init(&r->base_commit);
    atlas_buf_init(&r->patch_digest);
    atlas_buf_init(&r->proposed_key_id);
    atlas_buf_init(&r->confirmed_key_id);
    atlas_buf_init(&r->created_at);
    atlas_buf_init(&r->confirmed_at);
    atlas_buf_init(&r->spooled_at);
    atlas_buf_init(&r->terminal_at);
    atlas_buf_init(&r->result_stage);
    atlas_buf_init(&r->result_rollback);
    atlas_buf_init(&r->result_head_before);
    atlas_buf_init(&r->result_head_after);
    atlas_buf_init(&r->result_version);
    atlas_buf_init(&r->result_text);
}

void atlas_deploy_row_free(atlas_deploy_row *r) {
    atlas_buf_free(&r->deploy_uid);
    atlas_buf_free(&r->job_uid);
    atlas_buf_free(&r->base_commit);
    atlas_buf_free(&r->patch_digest);
    atlas_buf_free(&r->proposed_key_id);
    atlas_buf_free(&r->confirmed_key_id);
    atlas_buf_free(&r->created_at);
    atlas_buf_free(&r->confirmed_at);
    atlas_buf_free(&r->spooled_at);
    atlas_buf_free(&r->terminal_at);
    atlas_buf_free(&r->result_stage);
    atlas_buf_free(&r->result_rollback);
    atlas_buf_free(&r->result_head_before);
    atlas_buf_free(&r->result_head_after);
    atlas_buf_free(&r->result_version);
    atlas_buf_free(&r->result_text);
}

/* --- small helpers ------------------------------------------------------------ */

static int64_t deploy_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* A fresh deploy identifier from the kernel's random source: "d" plus 32
 * lowercase hex, on `atlas_orch_new_uid`'s exact construction (random bytes,
 * hex-encoded, prefixed) -- reusing `atlas_random_bytes` rather than copying
 * `src/orch/orch.c`'s file-private `random_hex`, since that one is not
 * exported and this is the same "read from the kernel, fail rather than
 * substitute anything" contract `atlas/hmac.h` already documents. */
static atlas_status new_deploy_uid(atlas_buf *out, atlas_err *err) {
    unsigned char raw[ATLAS_DEPLOY_UID_HEX / 2u];
    atlas_status st = atlas_random_bytes(raw, sizeof raw, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char hex[ATLAS_DEPLOY_UID_HEX + 1u];
    atlas_hex_encode(raw, sizeof raw, hex);
    st = atlas_buf_set_str(out, "d", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append(out, hex, ATLAS_DEPLOY_UID_HEX, err);
    }
    return st;
}

/* The complement of `atlas_orch_state_is_terminal` (`src/orch/orch.c`), as
 * SQLite must spell it -- `src/db/db_orch.c`'s `ORCH_SQL_ACTIVE_STATE` and
 * `ORCH_NOT_TERMINAL_SQL` macros are file-private there, so this is a second
 * copy rather than a shared one, and `tests/test_db_deploy.c` asserts this
 * predicate and `atlas_orch_state_is_terminal` agree over the whole
 * vocabulary, exactly as `tests/test_orch_run.c` does for the first copy: two
 * spellings of one rule drift, and a deploy restarts the whole daemon, so
 * every job everywhere -- not only this deploy's own repository -- must have
 * ended before a confirm may proceed. */
#define DEPLOY_SQL_JOB_NOT_TERMINAL \
    "state NOT IN ('SUCCEEDED','FAILED','CANCELLED','TIMED_OUT','RECOVERY_REQUIRED')"

static atlas_status deploy_record_transition(atlas_db *db, int64_t deploy_id,
                                             atlas_deploy_state from, atlas_deploy_state to,
                                             atlas_deploy_actor actor, const char *key_id,
                                             const char *reason, atlas_err *err) {
    static const char SQL[] =
        "INSERT INTO deploy_transitions(deploy_id, from_state, to_state, actor, key_id, reason, at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    (void)sqlite3_bind_int64(st, 1, deploy_id);
    s = atlas_db_bind_text_opt(db, st, 2, atlas_deploy_state_name(from), err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 3, atlas_deploy_state_name(to), err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 4, atlas_deploy_actor_name(actor), err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 5, key_id != NULL ? key_id : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 6, reason != NULL ? reason : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 7, at, err);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    return atlas_db_step_done(db, st, err);
}

/* The subset of a `deploys` row every write function needs before it decides
 * anything: which row, what it may do next, who may cancel it, and what a
 * confirmation must match. */
typedef struct deploy_min {
    int64_t id;
    atlas_deploy_state state;
    char proposed_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char patch_digest[ATLAS_SHA256_HEX_LEN + 1u];
} deploy_min;

static atlas_status deploy_load_min(atlas_db *db, const char *deploy_uid, deploy_min *out,
                                    bool *found, atlas_err *err) {
    *found = false;
    memset(out, 0, sizeof(*out));
    static const char SQL[] =
        "SELECT id, state, proposed_key_id, patch_digest FROM deploys WHERE deploy_uid = ?1;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, deploy_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->id = sqlite3_column_int64(st, 0);
        (void)atlas_deploy_state_parse(atlas_db_col_text(st, 1), &out->state);
        s = atlas_db_col_copy(st, 2, out->proposed_key_id, sizeof(out->proposed_key_id),
                              "proposed_key_id", err);
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 3, out->patch_digest, sizeof(out->patch_digest),
                                  "patch_digest", err);
        }
        if (s == ATLAS_OK) {
            *found = true;
        }
    }
    atlas_db_finish(db, st);
    return s;
}

static atlas_status deploy_cas_lost(atlas_err *err, const char *deploy_uid,
                                    atlas_deploy_state from) {
    return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                         "deploy %s moved out of %s before this change could be applied",
                         deploy_uid, atlas_deploy_state_name(from));
}

/* --- propose ------------------------------------------------------------------ */

/* The one `changes.patch` artifact a SUCCEEDED job's newest attempt produced,
 * found by asking `atlas_db_orch_artifacts` for every artifact of the job and
 * keeping the last matching row -- `src/ipc/server_orch_remote.c`'s
 * `take_result_artifact` precedent for "newest attempt wins": the rows arrive
 * in id order, so a later attempt's patch is not a smaller answer, it is a
 * different one. `want_content` is false: T1 never needs the patch bytes
 * themselves, only the digest and size the artifact row already carries, and
 * the bytes stay in `orch_artifacts` for a later task's spool write to read
 * fresh. */
typedef struct patch_find {
    bool found;
    bool content_stored;
    int64_t size_bytes;
    char sha256[ATLAS_SHA256_HEX_LEN + 1u];
} patch_find;

static atlas_status patch_take(const atlas_orch_artifact_row *row, void *ud, atlas_err *err) {
    (void)err;
    if (row->name == NULL || strcmp(row->name, ATLAS_ORCH_RESULT_PATCH_NAME) != 0) {
        return ATLAS_OK;
    }
    patch_find *pf = (patch_find *)ud;
    pf->found = true;
    pf->content_stored = row->content_stored;
    pf->size_bytes = row->size_bytes;
    (void)snprintf(pf->sha256, sizeof(pf->sha256), "%s", row->sha256 != NULL ? row->sha256 : "");
    return ATLAS_OK;
}

atlas_status atlas_db_deploy_propose_in_tx(atlas_db *db, const char *job_uid, const char *key_id,
                                           atlas_buf *deploy_uid_out, atlas_err *err) {
    if (job_uid == NULL || job_uid[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }

    static const char SQL_JOB[] =
        "SELECT state, mode, submit_key_id, repo_id, source_commit"
        "  FROM orch_jobs WHERE job_uid = ?1;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL_JOB, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, job_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    bool found = false;
    atlas_orch_state job_state = ATLAS_ORCH_STATE_UNKNOWN;
    char mode[ATLAS_ORCH_NAME_MAX + 1u];
    mode[0] = '\0';
    char submit_key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    submit_key_id[0] = '\0';
    bool have_repo_id = false;
    int64_t repo_id = 0;
    char source_commit[41];
    source_commit[0] = '\0';
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = true;
        (void)atlas_orch_state_parse(atlas_db_col_text(st, 0), &job_state);
        s = atlas_db_col_copy(st, 1, mode, sizeof(mode), "mode", err);
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 2, submit_key_id, sizeof(submit_key_id), "submit_key_id",
                                  err);
        }
        if (s == ATLAS_OK) {
            have_repo_id = sqlite3_column_type(st, 3) != SQLITE_NULL;
            repo_id = sqlite3_column_int64(st, 3);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 4, source_commit, sizeof(source_commit), "source_commit",
                                  err);
        }
    }
    atlas_db_finish(db, st);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such job");
    }
    /* `repo_id` is nullable on `orch_jobs` (a job whose repository never
     * resolved), and a deploy has nowhere to record that -- `deploys.repo_id`
     * is NOT NULL. Not one of the season's seven enumerated refusals because
     * a SUCCEEDED job always has one in practice; still checked, because a
     * malformed row must never reach `sqlite3_bind_int64` with a value that
     * was never validated. */
    if (!have_repo_id) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "job %s has no resolved repository", job_uid);
    }
    if (job_state != ATLAS_ORCH_STATE_SUCCEEDED) {
        return atlas_err_set(
            err, ATLAS_ERR_USAGE,
            "job %s has not SUCCEEDED (it is %s); only a SUCCEEDED job may be deployed", job_uid,
            atlas_orch_state_name(job_state));
    }
    if (strcmp(mode, "patch") != 0) {
        return atlas_err_set(
            err, ATLAS_ERR_USAGE,
            "job %s does not use patch mode (it is \"%s\"); only a patch-mode job may be deployed",
            job_uid, mode);
    }

    patch_find pf;
    memset(&pf, 0, sizeof(pf));
    int64_t artifact_count = 0;
    s = atlas_db_orch_artifacts(db, job_uid, 0, false, patch_take, &pf, &artifact_count, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!pf.found || !pf.content_stored || pf.size_bytes <= 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "job %s has no stored \"%s\" artifact to deploy", job_uid,
                             ATLAS_ORCH_RESULT_PATCH_NAME);
    }

    if (strcmp(submit_key_id, key_id != NULL ? key_id : "") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "job %s was queued by a different credential", job_uid);
    }

    /* One deploy in flight per repository: `idx_deploys_one_active` is the
     * schema guarantee, this is the sentence that names it. */
    {
        static const char SQL[] = "SELECT deploy_uid, state FROM deploys"
                                   " WHERE repo_id = ?1 AND state IN ('PROPOSED','CONFIRMED')"
                                   " LIMIT 1;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, SQL, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(q, 1, repo_id);
        if (sqlite3_step(q) == SQLITE_ROW) {
            char existing_uid[ATLAS_DEPLOY_UID_MAX];
            (void)snprintf(existing_uid, sizeof(existing_uid), "%s", atlas_db_col_text(q, 0));
            char existing_state[16];
            (void)snprintf(existing_state, sizeof(existing_state), "%s", atlas_db_col_text(q, 1));
            atlas_db_finish(db, q);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a deploy is already in flight for this repository: %s is %s",
                                 existing_uid, existing_state);
        }
        atlas_db_finish(db, q);
    }

    /* This job's patch is deployed at most once successfully:
     * `idx_deploys_one_per_job` is that guarantee's own schema fact. A FAILED
     * or CANCELLED deploy does not count, so the same job may be proposed
     * again after either. */
    {
        static const char SQL[] =
            "SELECT deploy_uid, state FROM deploys"
            " WHERE job_uid = ?1 AND state IN ('PROPOSED','CONFIRMED','SUCCEEDED') LIMIT 1;";
        sqlite3_stmt *q = NULL;
        s = atlas_db_prepare(db, SQL, &q, err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_db_bind_text_opt(db, q, 1, job_uid, err);
        if (s != ATLAS_OK) {
            atlas_db_finish(db, q);
            return s;
        }
        if (sqlite3_step(q) == SQLITE_ROW) {
            char existing_uid[ATLAS_DEPLOY_UID_MAX];
            (void)snprintf(existing_uid, sizeof(existing_uid), "%s", atlas_db_col_text(q, 0));
            char existing_state[16];
            (void)snprintf(existing_state, sizeof(existing_state), "%s", atlas_db_col_text(q, 1));
            atlas_db_finish(db, q);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "job %s already has a %s deploy, %s",
                                 job_uid, existing_state, existing_uid);
        }
        atlas_db_finish(db, q);
    }

    atlas_buf uid = ATLAS_BUF_INIT;
    s = new_deploy_uid(&uid, err);
    if (s != ATLAS_OK) {
        atlas_buf_free(&uid);
        return s;
    }

    static const char INS[] =
        "INSERT INTO deploys(deploy_uid, job_uid, repo_id, base_commit, patch_digest,"
        "  patch_bytes, state, proposed_key_id, created_at, created_ms)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'PROPOSED', ?7, ?8, ?9);";
    sqlite3_stmt *ins = NULL;
    s = atlas_db_prepare(db, INS, &ins, err);
    if (s != ATLAS_OK) {
        atlas_buf_free(&uid);
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, ins, 1, atlas_buf_cstr(&uid), err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, ins, 2, job_uid, err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(ins, 3, repo_id);
        s = atlas_db_bind_text_opt(db, ins, 4, source_commit, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, ins, 5, pf.sha256, err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(ins, 6, pf.size_bytes);
        s = atlas_db_bind_text_opt(db, ins, 7, key_id != NULL ? key_id : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, ins, 8, at, err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(ins, 9, deploy_now_ms());
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, ins);
        atlas_buf_free(&uid);
        return s;
    }
    s = atlas_db_step_done(db, ins, err);
    if (s != ATLAS_OK) {
        atlas_buf_free(&uid);
        return s;
    }
    int64_t deploy_id = sqlite3_last_insert_rowid(db->h);

    s = deploy_record_transition(db, deploy_id, ATLAS_DEPLOY_UNKNOWN, ATLAS_DEPLOY_PROPOSED,
                                 ATLAS_DEPLOY_ACTOR_REMOTE_CREDENTIAL, key_id, "", err);
    if (s == ATLAS_OK && deploy_uid_out != NULL) {
        s = atlas_buf_set(deploy_uid_out, uid.data, uid.len, err);
    }
    atlas_buf_free(&uid);
    return s;
}

/* --- confirmed_uid ------------------------------------------------------------ */

atlas_status atlas_db_deploy_confirmed_uid(atlas_db *db, int64_t repo_id, atlas_buf *uid_or_empty,
                                           atlas_err *err) {
    atlas_status s = atlas_buf_set_str(uid_or_empty, "", err);
    if (s != ATLAS_OK) {
        return s;
    }
    static const char SQL[] =
        "SELECT deploy_uid FROM deploys WHERE repo_id = ?1 AND state = 'CONFIRMED' LIMIT 1;";
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, repo_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        s = atlas_buf_set_str(uid_or_empty, atlas_db_col_text(st, 0), err);
    }
    atlas_db_finish(db, st);
    return s;
}

/* --- confirm -------------------------------------------------------------- */

atlas_status atlas_db_deploy_confirm_in_tx(atlas_db *db, const char *deploy_uid,
                                           const char *key_id, const char *confirmation,
                                           atlas_err *err) {
    deploy_min d;
    bool found = false;
    atlas_status s = deploy_load_min(db, deploy_uid, &d, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }
    if (d.state != ATLAS_DEPLOY_PROPOSED) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "deploy %s is %s, not PROPOSED", deploy_uid,
                             atlas_deploy_state_name(d.state));
    }

    /* The confirmation the operator typed, compared against the phrase
     * derived from the *stored* patch digest -- A16's "first eight hex of the
     * content hash" rule, applied here to a patch digest instead of a
     * decision revision's content hash. A length mismatch is a mismatch, not
     * a prefix match against a shorter string. */
    size_t want = ATLAS_DEPLOY_CONFIRMATION_HEX;
    size_t have = confirmation != NULL ? strlen(confirmation) : 0u;
    if (confirmation == NULL || have != want ||
        strncmp(confirmation, d.patch_digest, want) != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the confirmation does not match this deploy's patch; nothing was "
                             "changed");
    }

    /* Every job everywhere must be terminal: confirming restarts the whole
     * daemon, which ends every job regardless of which repository it
     * targets. */
    int64_t not_terminal = 0;
    s = atlas_db_query_int64(
        db, "SELECT COUNT(*) FROM orch_jobs WHERE " DEPLOY_SQL_JOB_NOT_TERMINAL ";",
        &not_terminal, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (not_terminal > 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "%lld job(s) are not terminal; confirming this deploy restarts the "
                             "daemon and would end them mid-flight",
                             (long long)not_terminal);
    }

    static const char UPD[] = "UPDATE deploys SET state = 'CONFIRMED', confirmed_key_id = ?1,"
                              "  confirmed_at = ?2"
                              " WHERE deploy_uid = ?3 AND state = 'PROPOSED';";
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, UPD, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, key_id != NULL ? key_id : "", err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, at, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 3, deploy_uid, err);
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
        return deploy_cas_lost(err, deploy_uid, ATLAS_DEPLOY_PROPOSED);
    }
    return deploy_record_transition(db, d.id, ATLAS_DEPLOY_PROPOSED, ATLAS_DEPLOY_CONFIRMED,
                                    ATLAS_DEPLOY_ACTOR_REMOTE_OPERATOR_CONFIRMED, key_id, "", err);
}

/* --- cancel --------------------------------------------------------------- */

atlas_status atlas_db_deploy_cancel_in_tx(atlas_db *db, const char *deploy_uid,
                                          const char *key_id, atlas_err *err) {
    deploy_min d;
    bool found = false;
    atlas_status s = deploy_load_min(db, deploy_uid, &d, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }
    /* A mismatched credential is told the same thing an absent deploy is --
     * A14 T3's `op_cancel` precedent (`src/db/db_orch.c`): a caller must not
     * be able to use this to learn that some other credential's deploy
     * exists. */
    if (strcmp(d.proposed_key_id, key_id != NULL ? key_id : "") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }
    if (d.state != ATLAS_DEPLOY_PROPOSED) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "deploy %s is %s; only a PROPOSED deploy may be cancelled",
                             deploy_uid, atlas_deploy_state_name(d.state));
    }

    static const char UPD[] = "UPDATE deploys SET state = 'CANCELLED', terminal_at = ?1"
                              " WHERE deploy_uid = ?2 AND state = 'PROPOSED';";
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, UPD, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, at, err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, deploy_uid, err);
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
        return deploy_cas_lost(err, deploy_uid, ATLAS_DEPLOY_PROPOSED);
    }
    return deploy_record_transition(db, d.id, ATLAS_DEPLOY_PROPOSED, ATLAS_DEPLOY_CANCELLED,
                                    ATLAS_DEPLOY_ACTOR_REMOTE_CREDENTIAL, key_id, "", err);
}

/* --- mark_spooled ----------------------------------------------------------- */

atlas_status atlas_db_deploy_mark_spooled_in_tx(atlas_db *db, const char *deploy_uid,
                                                atlas_err *err) {
    deploy_min d;
    bool found = false;
    atlas_status s = deploy_load_min(db, deploy_uid, &d, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }
    if (d.state != ATLAS_DEPLOY_CONFIRMED) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "deploy %s is %s, not CONFIRMED", deploy_uid,
                             atlas_deploy_state_name(d.state));
    }

    /* No `deploy_transitions` row: `state` does not change here, and that
     * ledger records state changes -- a from-CONFIRMED-to-CONFIRMED entry
     * would misdescribe an event that was not a transition. `spooled_at` is
     * the durable record of this event on its own. The CAS predicate still
     * requires exactly one `deploys` row changed: `spooled_at IS NULL` makes
     * a second call refused rather than silently repeated. */
    static const char UPD[] = "UPDATE deploys SET spooled_at = ?1"
                              " WHERE deploy_uid = ?2 AND state = 'CONFIRMED'"
                              "  AND spooled_at IS NULL;";
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, UPD, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, at, err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, deploy_uid, err);
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
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "deploy %s was not CONFIRMED-and-unspooled when this was recorded",
                             deploy_uid);
    }
    return ATLAS_OK;
}

/* --- finish ----------------------------------------------------------------- */

atlas_status atlas_db_deploy_finish_in_tx(atlas_db *db, const char *deploy_uid,
                                          const atlas_deploy_result *r, atlas_err *err) {
    if (r == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no result given");
    }
    size_t text_len = r->text != NULL ? strlen(r->text) : 0u;
    if (text_len > ATLAS_DEPLOY_RESULT_TEXT_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "the result text is %zu bytes, exceeding the %u byte limit",
                             text_len, ATLAS_DEPLOY_RESULT_TEXT_MAX);
    }

    deploy_min d;
    bool found = false;
    atlas_status s = deploy_load_min(db, deploy_uid, &d, &found, err);
    if (s != ATLAS_OK) {
        return s;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "no such deploy");
    }
    if (d.state != ATLAS_DEPLOY_CONFIRMED) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "deploy %s is %s, not CONFIRMED", deploy_uid,
                             atlas_deploy_state_name(d.state));
    }

    atlas_deploy_state to = r->success ? ATLAS_DEPLOY_SUCCEEDED : ATLAS_DEPLOY_FAILED;
    static const char UPD[] =
        "UPDATE deploys SET state = ?1, terminal_at = ?2, result_dry_run = ?3, result_stage = ?4,"
        "  result_rollback = ?5, result_head_before = ?6, result_head_after = ?7,"
        "  result_version = ?8, result_text = ?9"
        " WHERE deploy_uid = ?10 AND state = 'CONFIRMED';";
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, UPD, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    s = atlas_db_bind_text_opt(db, st, 1, atlas_deploy_state_name(to), err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, at, err);
    }
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 3, r->dry_run ? 1 : 0);
        s = atlas_db_bind_text_opt(db, st, 4, r->stage != NULL ? r->stage : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 5, r->rollback != NULL ? r->rollback : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 6, r->head_before != NULL ? r->head_before : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 7, r->head_after != NULL ? r->head_after : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 8, r->version != NULL ? r->version : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 9, r->text != NULL ? r->text : "", err);
    }
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 10, deploy_uid, err);
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
        return deploy_cas_lost(err, deploy_uid, ATLAS_DEPLOY_CONFIRMED);
    }
    return deploy_record_transition(db, d.id, ATLAS_DEPLOY_CONFIRMED, to,
                                    ATLAS_DEPLOY_ACTOR_DEPLOY_AGENT, "", "", err);
}

/* --- reads -------------------------------------------------------------------- */

atlas_status atlas_db_deploy_get(atlas_db *db, const char *deploy_uid, atlas_deploy_row *out,
                                 bool *have, atlas_err *err) {
    if (have != NULL) {
        *have = false;
    }
    static const char SQL[] =
        "SELECT id, deploy_uid, job_uid, repo_id, base_commit, patch_digest, patch_bytes, state,"
        "  proposed_key_id, confirmed_key_id, created_at, created_ms, confirmed_at, spooled_at,"
        "  terminal_at, result_dry_run, result_stage, result_rollback, result_head_before,"
        "  result_head_after, result_version, result_text"
        " FROM deploys WHERE deploy_uid = ?1;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, deploy_uid, err);
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->id = sqlite3_column_int64(st, 0);
        s = atlas_buf_set_str(&out->deploy_uid, atlas_db_col_text(st, 1), err);
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->job_uid, atlas_db_col_text(st, 2), err);
        }
        if (s == ATLAS_OK) {
            out->repo_id = sqlite3_column_int64(st, 3);
            s = atlas_buf_set_str(&out->base_commit, atlas_db_col_text(st, 4), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->patch_digest, atlas_db_col_text(st, 5), err);
        }
        if (s == ATLAS_OK) {
            out->patch_bytes = sqlite3_column_int64(st, 6);
            (void)atlas_deploy_state_parse(atlas_db_col_text(st, 7), &out->state);
            s = atlas_buf_set_str(&out->proposed_key_id, atlas_db_col_text(st, 8), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->confirmed_key_id, atlas_db_col_text(st, 9), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->created_at, atlas_db_col_text(st, 10), err);
        }
        if (s == ATLAS_OK) {
            out->created_ms = sqlite3_column_int64(st, 11);
            s = atlas_buf_set_str(&out->confirmed_at, atlas_db_col_text(st, 12), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->spooled_at, atlas_db_col_text(st, 13), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->terminal_at, atlas_db_col_text(st, 14), err);
        }
        if (s == ATLAS_OK) {
            out->result_dry_run = sqlite3_column_int64(st, 15) != 0;
            s = atlas_buf_set_str(&out->result_stage, atlas_db_col_text(st, 16), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->result_rollback, atlas_db_col_text(st, 17), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->result_head_before, atlas_db_col_text(st, 18), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->result_head_after, atlas_db_col_text(st, 19), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->result_version, atlas_db_col_text(st, 20), err);
        }
        if (s == ATLAS_OK) {
            s = atlas_buf_set_str(&out->result_text, atlas_db_col_text(st, 21), err);
        }
        if (s == ATLAS_OK && have != NULL) {
            *have = true;
        }
    }
    atlas_db_finish(db, st);
    return s;
}

static atlas_status deploy_list_impl(atlas_db *db, sqlite3_stmt *st, int64_t after_id,
                                     int64_t limit, atlas_deploy_list_cb cb, void *ud,
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
        atlas_deploy_list_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(st, 0);
        s = atlas_db_col_copy(st, 1, row.deploy_uid, sizeof(row.deploy_uid), "deploy_uid", err);
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 2, row.job_uid, sizeof(row.job_uid), "job_uid", err);
        }
        if (s == ATLAS_OK) {
            row.repo_id = sqlite3_column_int64(st, 3);
            (void)atlas_deploy_state_parse(atlas_db_col_text(st, 4), &row.state);
            s = atlas_db_col_copy(st, 5, row.proposed_key_id, sizeof(row.proposed_key_id),
                                  "proposed_key_id", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 6, row.confirmed_key_id, sizeof(row.confirmed_key_id),
                                  "confirmed_key_id", err);
        }
        if (s == ATLAS_OK) {
            s = atlas_db_col_copy(st, 7, row.created_at, sizeof(row.created_at), "created_at",
                                  err);
        }
        if (s != ATLAS_OK) {
            break;
        }
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

atlas_status atlas_db_deploy_list(atlas_db *db, const char *key_id_or_null, int64_t after_id,
                                  int64_t limit, atlas_deploy_list_cb cb, void *ud,
                                  int64_t *count_out, int64_t *cursor_out, bool *more_out,
                                  atlas_err *err) {
    if (limit <= 0 || limit > ATLAS_DEPLOY_LIST_MAX) {
        limit = ATLAS_DEPLOY_LIST_MAX;
    }
    if (key_id_or_null == NULL || key_id_or_null[0] == '\0') {
        static const char SQL[] =
            "SELECT id, deploy_uid, job_uid, repo_id, state, proposed_key_id, confirmed_key_id,"
            "  created_at"
            "  FROM deploys WHERE id > ?1 ORDER BY id LIMIT ?2;";
        sqlite3_stmt *st = NULL;
        atlas_status s = atlas_db_prepare(db, SQL, &st, err);
        if (s != ATLAS_OK) {
            return s;
        }
        (void)sqlite3_bind_int64(st, 1, after_id);
        (void)sqlite3_bind_int64(st, 2, limit + 1);
        return deploy_list_impl(db, st, after_id, limit, cb, ud, count_out, cursor_out, more_out,
                                err);
    }
    static const char SQL2[] =
        "SELECT id, deploy_uid, job_uid, repo_id, state, proposed_key_id, confirmed_key_id,"
        "  created_at"
        "  FROM deploys WHERE (proposed_key_id = ?1 OR confirmed_key_id = ?1) AND id > ?2"
        " ORDER BY id LIMIT ?3;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL2, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, key_id_or_null, err);
    if (s == ATLAS_OK) {
        (void)sqlite3_bind_int64(st, 2, after_id);
        (void)sqlite3_bind_int64(st, 3, limit + 1);
    }
    if (s != ATLAS_OK) {
        atlas_db_finish(db, st);
        return s;
    }
    return deploy_list_impl(db, st, after_id, limit, cb, ud, count_out, cursor_out, more_out, err);
}

atlas_status atlas_db_deploy_confirmed_unspooled(atlas_db *db, atlas_buf *uids_out,
                                                 atlas_err *err) {
    atlas_status s = atlas_buf_set_str(uids_out, "", err);
    if (s != ATLAS_OK) {
        return s;
    }
    static const char SQL[] =
        "SELECT deploy_uid FROM deploys WHERE state = 'CONFIRMED' AND spooled_at IS NULL"
        " ORDER BY id;";
    sqlite3_stmt *st = NULL;
    s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    while (s == ATLAS_OK && sqlite3_step(st) == SQLITE_ROW) {
        s = atlas_buf_append_str(uids_out, atlas_db_col_text(st, 0), err);
    }
    atlas_db_finish(db, st);
    return s;
}

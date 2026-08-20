/* Atlas - A12.0: the one write point for a plan, and the status nothing writes.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `atlas/plan.h` for the rule this file exists to keep: every mutation of
 * `orch_plans`, `orch_plan_revisions` and `orch_plan_tasks` happens inside
 * `atlas_plan_apply_in_tx`, and there is no second implementation. The binding
 * checks a forger would want somewhere else — the correlation, the driver's
 * role, the job's state, the artifact's name and its bounds, and the document
 * format itself — all live behind that one call.
 *
 * Three properties are worth stating before the code, because they are what the
 * shape of it is for.
 *
 *   **A plan has no status, so nothing here writes one.** `atlas_plan_op_kind`
 *   has two members and neither settles anything. What a plan is doing is
 *   derived on every read by `atlas_db_plan_state_derive` from rows this
 *   vocabulary cannot reach: the revisions that compiled, the jobs their
 *   correlations name, and the runs those jobs settled. There is no
 *   `plan.settle`, no column, no compare-and-swap to win. A11.0 left `ACCEPTED`
 *   and `BLOCKED` with no producer so that "who may decide" could be answered
 *   later; here the question does not arise, because the verb does not exist.
 *
 *   **The operator brings the goal and the gate floor; the planner may only
 *   add.** The floor is stored once on the plan row and is prepended verbatim
 *   and first to every tree task's validations when a revision compiles. A
 *   planner's `gate:` lines are *additions*, and the merged list is bounded
 *   again here — which is a refusal the planner was told about in the format
 *   specification it was given, so it is enforcement of a stated rule rather
 *   than a surprise.
 *
 *   **A refused document is a typed answer, not only an error.** A planner that
 *   writes an unparseable plan has made a model's mistake, and the driver
 *   answers it by composing a retry prompt out of Atlas' own refusal sentence
 *   and the line it happened on. Both travel in `atlas_plan_result`, apart, so
 *   the driver never has to read Atlas' prose to recover the line number.
 *
 * The parser this file calls, `atlas_plan_parse`, is pure — no database handle,
 * no process, no file, no clock — and calling it from here is what keeps one
 * implementation of the format. The bytes it is given come from the planner
 * job's own stored `orch_artifacts` row and from nowhere else: the model's
 * output never travels a second path, and this file opens nothing.
 */
#define _GNU_SOURCE 1

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/driver.h"
#include "atlas/hmac.h"
#include "atlas/orch_ops.h"
#include "atlas/plan.h"
#include "atlas/sha256.h"
#include "db_internal.h"

/* --- small helpers --------------------------------------------------------- */

static int64_t plan_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

const char *atlas_plan_revision_reason_name(atlas_plan_revision_reason r) {
    /* No `default:`, so adding a member to the vocabulary is a build failure
     * here rather than a row that silently stores "UNKNOWN" — which the CHECK
     * would then refuse at a point nobody can act on. */
    switch (r) {
    case ATLAS_PLAN_REVISION_INITIAL: return "INITIAL";
    case ATLAS_PLAN_REVISION_REPLAN: return "REPLAN";
    case ATLAS_PLAN_REVISION_UNKNOWN: break;
    }
    return "UNKNOWN";
}

void atlas_plan_op_init(atlas_plan_op *op, atlas_plan_op_kind kind) {
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    atlas_buf_init(&op->repo_name);
    atlas_buf_init(&op->repo_identity_hash);
    atlas_buf_init(&op->goal_text);
    atlas_buf_init(&op->gate_floor);
    atlas_buf_init(&op->plan_uid);
    atlas_buf_init(&op->planner_job_uid);
}

void atlas_plan_op_free(atlas_plan_op *op) {
    if (op == NULL) {
        return;
    }
    atlas_buf_free(&op->repo_name);
    atlas_buf_free(&op->repo_identity_hash);
    atlas_buf_free(&op->goal_text);
    atlas_buf_free(&op->gate_floor);
    atlas_buf_free(&op->plan_uid);
    atlas_buf_free(&op->planner_job_uid);
    memset(op, 0, sizeof(*op));
}

void atlas_plan_result_init(atlas_plan_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->plan_uid);
    atlas_buf_init(&r->refusal);
}

void atlas_plan_result_free(atlas_plan_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->plan_uid);
    atlas_buf_free(&r->refusal);
    memset(r, 0, sizeof(*r));
}

/* --- the correlation, which is the whole plan-to-job mapping ----------------
 *
 * **The string has to fit inside what a job specification may carry**, and that
 * is a harder constraint than it looks. `atlas_orch_spec_validate` holds both
 * `correlation` and `idempotency_key` to `is_name`: `[a-z0-9._-]`, at most
 * `ATLAS_ORCH_NAME_MAX` (64) bytes. A colon is not in that set at all, and a
 * 33-byte plan uid beside a 32-byte task key cannot fit in 64 with any
 * separators whatever — the arithmetic is 5 + 33 + 4 + 32 = 74.
 *
 * So the plan uid is *shortened where it is spelled into a name*, and only
 * there: `ATLAS_PLAN_CORR_UID_LEN` characters — the `'p'` and the first 20 hex
 * digits, 80 bits. The stored `orch_plans.plan_uid` is still the full 33, and
 * every other surface still uses it whole.
 *
 *   planner  `plan.<uid21>.planner.<k>`      5 + 21 + 9 + 1        = 36
 *   task     `plan.<uid21>.r<R>.<key>`       5 + 21 + 3 + 1 + 32   = 62
 *
 * 62 is the worst case and it is two under the bound, which
 * `tests/test_plan_db.c` pins by building that exact string and running it
 * through `atlas_orch_spec_validate`. The bound is what a *stored* correlation
 * must satisfy rather than merely what a client may send, because
 * `spawn_follow_up` validates the correlation a follow-up inherits from its
 * parent: a repo-tree plan task whose gate fails would otherwise be unable to
 * create the one follow-up it earned.
 *
 * 80 bits is unguessable for the thing this identifier has to be unguessable
 * for. The correlation is not a capability — presenting one authorises nothing,
 * and `plan.revision_add` still requires the named job to *hold* it — but a
 * plan whose correlations could be predicted would be a plan whose jobs another
 * local process could name before they existed.
 */
#define ATLAS_PLAN_CORR_UID_LEN 21u

/* The plan identifier as it is spelled into a name. Checked rather than
 * assumed, because this is the one place the shortening happens and a uid that
 * is not `'p'` plus hex would put something else entirely into a job's stored
 * correlation. */
static bool plan_uid_prefix(const char *plan_uid, char out[ATLAS_PLAN_CORR_UID_LEN + 1u]) {
    if (plan_uid == NULL || strlen(plan_uid) < ATLAS_PLAN_CORR_UID_LEN || plan_uid[0] != 'p') {
        return false;
    }
    for (size_t i = 1; i < ATLAS_PLAN_CORR_UID_LEN; i++) {
        char c = plan_uid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    memcpy(out, plan_uid, ATLAS_PLAN_CORR_UID_LEN);
    out[ATLAS_PLAN_CORR_UID_LEN] = '\0';
    return true;
}

/* Nothing about a task key is trusted here even though the parser has already
 * checked it. The key travels into this string as its last field, and the
 * separator is now a dot — so a key holding a dot would be a key that could
 * spell a different plan's revision, and one holding anything outside
 * `[a-z0-9-]` would be a key that could make the whole correlation fail
 * `is_name` after it had been stored. The one place the string is built refuses
 * both, rather than relying on having been called in the right order. */
static bool key_is_safe(const char *key, size_t len) {
    if (key == NULL || len == 0 || len > 32u) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

atlas_status atlas_plan_correlation_planner(const char *plan_uid, int k, atlas_buf *out,
                                            atlas_err *err) {
    char uid[ATLAS_PLAN_CORR_UID_LEN + 1u];
    if (!plan_uid_prefix(plan_uid, uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan correlation needs a plan identifier: 'p' and at least %u "
                             "lowercase hex characters",
                             (unsigned)(ATLAS_PLAN_CORR_UID_LEN - 1u));
    }
    if (k < 1 || k > ATLAS_PLAN_MAX_PLANNER_JOBS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan has at most %d planner jobs; %d was asked for",
                             ATLAS_PLAN_MAX_PLANNER_JOBS, k);
    }
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, "plan.%s.planner.%d", uid, k);
}

atlas_status atlas_plan_correlation_task(const char *plan_uid, int rev_no, const char *task_key,
                                         atlas_buf *out, atlas_err *err) {
    char uid[ATLAS_PLAN_CORR_UID_LEN + 1u];
    if (!plan_uid_prefix(plan_uid, uid)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan correlation needs a plan identifier: 'p' and at least %u "
                             "lowercase hex characters",
                             (unsigned)(ATLAS_PLAN_CORR_UID_LEN - 1u));
    }
    if (rev_no < 1 || rev_no > ATLAS_PLAN_MAX_REVISIONS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan has at most %d revisions; %d was asked for",
                             ATLAS_PLAN_MAX_REVISIONS, rev_no);
    }
    if (task_key == NULL || !key_is_safe(task_key, strlen(task_key))) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a task key is 1 to 32 characters of [a-z0-9-]");
    }
    atlas_buf_reset(out);
    return atlas_buf_appendf(out, err, "plan.%s.r%d.%s", uid, rev_no, task_key);
}

/* --- the plan identifier ---------------------------------------------------
 *
 * 'p' plus 128 bits of kernel randomness in lowercase hex — the run uid's shape
 * with a prefix of its own, so a plan can never be mistaken for a run, a job, a
 * commit or a content hash on sight or by a parser.
 *
 * Unguessable is load-bearing rather than decorative here, because the
 * correlation that binds a job to a plan is built out of this value: a plan
 * identifier somebody could predict would be a plan whose jobs somebody could
 * name before it existed. Failure to read the CSPRNG refuses to create the plan
 * rather than falling back to anything weaker, which is `db_verify.c`'s rule for
 * a uid and `atlas_orch_new_uid`'s before it.
 *
 * This is the file's only use of anything outside the database, and it is not a
 * hole in "the planner's bytes come from the artifact row": it produces an
 * identifier, reads nothing and is reached before any document is. */
#define PLAN_UID_BYTES 16u

static atlas_status plan_new_uid(atlas_buf *out, atlas_err *err) {
    unsigned char raw[PLAN_UID_BYTES];
    atlas_status st = atlas_random_bytes(raw, sizeof raw, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char hex[PLAN_UID_BYTES * 2u + 1u];
    atlas_hex_encode(raw, sizeof raw, hex);
    st = atlas_buf_set_str(out, "p", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, hex, err);
    }
    return st;
}

/* --- the plan row ----------------------------------------------------------- */

typedef struct plan_row {
    int64_t id;
    char plan_uid[ATLAS_ORCH_RUN_UID_MAX];
    char repo_name[ATLAS_ORCH_NAME_MAX + 1u];
    char repo_identity_hash[ATLAS_SHA256_HEX_LEN + 1u];
    int64_t max_parallel;
    /* The operator's floor, in the stored netstring encoding. Owned. */
    atlas_buf gate_floor;
} plan_row;

static void plan_row_init(plan_row *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->gate_floor);
}

static void plan_row_free(plan_row *r) {
    atlas_buf_free(&r->gate_floor);
    memset(r, 0, sizeof(*r));
}

static atlas_status plan_by_uid(atlas_db *db, const char *uid, plan_row *r, bool *found,
                                atlas_err *err) {
    static const char SQL[] = "SELECT id, plan_uid, repo_name, repo_identity_hash, max_parallel,"
                              "       gate_floor FROM orch_plans WHERE plan_uid = ?1;";
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
            r->id = sqlite3_column_int64(st, 0);
            s = atlas_db_col_copy(st, 1, r->plan_uid, sizeof(r->plan_uid), "plan_uid", err);
            if (s == ATLAS_OK) {
                s = atlas_db_col_copy(st, 2, r->repo_name, sizeof(r->repo_name), "repo_name", err);
            }
            if (s == ATLAS_OK) {
                s = atlas_db_col_copy(st, 3, r->repo_identity_hash, sizeof(r->repo_identity_hash),
                                      "repo_identity_hash", err);
            }
            if (s == ATLAS_OK) {
                r->max_parallel = sqlite3_column_int64(st, 4);
                s = atlas_buf_set_str(&r->gate_floor, atlas_db_col_text(st, 5), err);
            }
            *found = (s == ATLAS_OK);
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a plan");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* The highest revision number this plan holds, or 0 when it holds none. */
static atlas_status plan_max_rev(atlas_db *db, int64_t plan_id, int *out, atlas_err *err) {
    static const char SQL[] =
        "SELECT COALESCE(MAX(rev_no), 0) FROM orch_plan_revisions WHERE plan_id = ?1;";
    *out = 0;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, plan_id);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *out = (int)sqlite3_column_int64(st, 0);
    } else if (rc != SQLITE_DONE) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a plan's revisions");
    }
    atlas_db_finish(db, st);
    return s;
}

/* --- refusals a planner earned ---------------------------------------------
 *
 * Two shapes, one destination. `carry_refusal` propagates what
 * `atlas_plan_parse` already decided; `refuse_document` states a refusal this
 * layer applies on top of the parse. Both fill the typed members and both return
 * a non-OK status, so the transaction is rolled back and no row is written —
 * which is what makes "a refused parse leaves no revision" a fact rather than a
 * convention, and is why the refused state has to be *derived* from a planner
 * job no revision names.
 *
 * The scratch error is deliberate: recording the refusal must not overwrite the
 * refusal. If the copy itself fails there is nothing better to say than what
 * `err` already says. */
static void carry_refusal(atlas_plan_result *out, int line, const atlas_err *err) {
    atlas_err scratch;
    atlas_err_init(&scratch);
    out->refusal_line = line;
    (void)atlas_buf_set_str(&out->refusal, atlas_err_msg(err), &scratch);
}

static atlas_status refuse_document(atlas_plan_result *out, int line, atlas_err *err,
                                    const char *msg) {
    atlas_err scratch;
    atlas_err_init(&scratch);
    out->refusal_line = line;
    (void)atlas_buf_set_str(&out->refusal, msg, &scratch);
    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s", msg);
}

/* --- CREATE ----------------------------------------------------------------- */

static atlas_status op_create(atlas_db *db, const atlas_plan_op *op, atlas_plan_result *out,
                              atlas_err *err) {
    if (op->repo_name.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan names the repository it is for");
    }
    if (op->repo_name.len > ATLAS_ORCH_NAME_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a repository name is at most %u bytes",
                             (unsigned)ATLAS_ORCH_NAME_MAX);
    }
    if (op->repo_identity_hash.len == 0 ||
        op->repo_identity_hash.len > ATLAS_SHA256_HEX_LEN) {
        /* Resolved from the registry by the caller. A plan whose repository
         * identity is missing or misshapen is one no task could be authorised
         * against, so it is refused here rather than stored and discovered
         * later. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan carries the repository's durable identity");
    }
    if (op->goal_text.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan needs a goal");
    }
    if (op->goal_text.len > (size_t)ATLAS_PLAN_GOAL_MAX) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan's goal is at most %d bytes; this one is %zu",
                             ATLAS_PLAN_GOAL_MAX, op->goal_text.len);
    }
    if (memchr(op->goal_text.data, '\0', op->goal_text.len) != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a plan's goal holds a NUL byte");
    }

    /* The floor. At least one command, because a plan with no operator gate
     * could only ever be accepted on a model's word — which is the whole
     * authority argument of the season, stated as a refusal. */
    if (op->gate_floor.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan needs at least one gate; the operator brings the gate floor "
                             "and the planner may only add to it");
    }
    {
        atlas_orch_argv floor[ATLAS_ORCH_MAX_VALIDATIONS];
        memset(floor, 0, sizeof floor);
        size_t count = 0;
        atlas_status st = atlas_orch_validations_decode(atlas_buf_cstr(&op->gate_floor), floor,
                                                       ATLAS_ORCH_MAX_VALIDATIONS, &count, err);
        for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
            atlas_orch_argv_free(&floor[i]);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        if (count == 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a plan needs at least one gate; the operator brings the gate "
                                 "floor and the planner may only add to it");
        }
    }

    /* Refused rather than clamped, for the reason every bound in A8 is: a
     * discarded number nobody is told about is a plan that runs differently
     * from the one that was asked for. */
    int parallel = op->max_parallel;
    if (parallel == 0) {
        parallel = ATLAS_PLAN_DEFAULT_PARALLEL;
    }
    if (parallel < 1 || parallel > ATLAS_ORCH_RUN_MAX_PARALLEL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a plan runs between 1 and %d tasks at once; %d was asked for",
                             ATLAS_ORCH_RUN_MAX_PARALLEL, op->max_parallel);
    }

    atlas_status st = plan_new_uid(&out->plan_uid, err);
    if (st != ATLAS_OK) {
        return st;
    }

    static const char INS[] =
        "INSERT INTO orch_plans(plan_uid, repo_name, repo_identity_hash, goal_text, gate_floor,"
        "  max_parallel, submitter_uid, created_at, created_ms)"
        "  VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);";
    sqlite3_stmt *q = NULL;
    st = atlas_db_prepare(db, INS, &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char at[ATLAS_TS_MAX];
    atlas_now_iso8601(at, sizeof(at));
    const char *texts[] = {atlas_buf_cstr(&out->plan_uid), atlas_buf_cstr(&op->repo_name),
                           atlas_buf_cstr(&op->repo_identity_hash),
                           atlas_buf_cstr(&op->goal_text), atlas_buf_cstr(&op->gate_floor)};
    for (size_t i = 0; st == ATLAS_OK && i < sizeof texts / sizeof texts[0]; i++) {
        st = atlas_db_bind_text_opt(db, q, (int)i + 1, texts[i], err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, q);
        return st;
    }
    (void)sqlite3_bind_int64(q, 6, parallel);
    (void)sqlite3_bind_int64(q, 7, op->submitter_uid);
    st = atlas_db_bind_text_opt(db, q, 8, at, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, q);
        return st;
    }
    (void)sqlite3_bind_int64(q, 9, plan_now_ms());
    st = atlas_db_step_done(db, q, err);
    if (st == ATLAS_OK) {
        out->plan_id = sqlite3_last_insert_rowid(db->h);
    }
    return st;
}

/* --- REVISION_ADD ----------------------------------------------------------- */

/* What the named job is, as far as this operation is concerned. */
typedef struct planner_job {
    int64_t id;
    char correlation[ATLAS_ORCH_NAME_MAX + 1u];
    char driver[ATLAS_ORCH_NAME_MAX + 1u];
    atlas_orch_state state;
} planner_job;

static atlas_status job_for_revision(atlas_db *db, const char *uid, planner_job *j, bool *found,
                                     atlas_err *err) {
    static const char SQL[] =
        "SELECT id, correlation, driver, state FROM orch_jobs WHERE job_uid = ?1;";
    *found = false;
    memset(j, 0, sizeof(*j));
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            j->id = sqlite3_column_int64(st, 0);
            s = atlas_db_col_copy(st, 1, j->correlation, sizeof(j->correlation), "correlation",
                                  err);
            if (s == ATLAS_OK) {
                s = atlas_db_col_copy(st, 2, j->driver, sizeof(j->driver), "driver", err);
            }
            if (s == ATLAS_OK && !atlas_orch_state_parse(atlas_db_col_text(st, 3), &j->state)) {
                s = atlas_err_set(err, ATLAS_ERR_DB, "job %s holds a state Atlas does not recognise",
                                  uid);
            }
            *found = (s == ATLAS_OK);
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a job");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* The `plan.atlas-plan` artifact of the job's successful attempt.
 *
 * Joined through `orch_attempts` on the attempt's own state rather than on the
 * job's, because an artifact belongs to one attempt: a job that failed twice and
 * then succeeded has three attempts, and the document Atlas compiles has to be
 * the one the successful attempt produced. `ORDER BY attempt_no DESC` picks the
 * last of them when a job somehow holds more than one successful attempt, which
 * the state machine does not produce and which this does not depend on. */
static atlas_status planner_artifact(atlas_db *db, const char *job_uid, atlas_buf *out,
                                     int64_t *size_out, bool *found, bool *stored_out,
                                     atlas_err *err) {
    static const char SQL[] = "SELECT a.content_stored, a.size_bytes, a.content"
                              "  FROM orch_artifacts a"
                              "  JOIN orch_attempts t ON t.id = a.attempt_id"
                              "  JOIN orch_jobs j ON j.id = t.job_id"
                              " WHERE j.job_uid = ?1 AND t.state = 'SUCCEEDED' AND a.name = ?2"
                              " ORDER BY t.attempt_no DESC LIMIT 1;";
    *found = false;
    *stored_out = false;
    *size_out = 0;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, job_uid, err);
    if (s == ATLAS_OK) {
        s = atlas_db_bind_text_opt(db, st, 2, ATLAS_PLAN_ARTIFACT_NAME, err);
    }
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            *found = true;
            *stored_out = sqlite3_column_int64(st, 0) != 0;
            *size_out = sqlite3_column_int64(st, 1);
            const void *blob = sqlite3_column_blob(st, 2);
            int n = sqlite3_column_bytes(st, 2);
            if (blob != NULL && n > 0) {
                /* Copied out before the statement is released: the pointer is
                 * borrowed for the length of the step, which is every row
                 * callback's rule in this repository and is no different here. */
                s = atlas_buf_set(out, blob, (size_t)n, err);
            }
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a planner artifact");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* Appends one argv vector to another, argument by argument. `atlas_orch_argv`
 * owns its buffers, so a struct copy would alias them and the second free would
 * be a double free. */
static atlas_status argv_append_all(atlas_orch_argv *dst, const atlas_orch_argv *src,
                                    atlas_err *err) {
    for (size_t i = 0; i < src->count; i++) {
        atlas_status st = atlas_orch_argv_push(dst, atlas_buf_cstr(&src->args[i]),
                                               src->args[i].len, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* One tree task's stored validations: the operator's floor, verbatim and first,
 * then the planner's additions in the order the document listed them.
 *
 * The merged list is bounded again here even though each half was bounded on its
 * own, because eight plus eight is sixteen and the encoding a job stores holds
 * eight. The refusal is *document-shaped* — it goes in `atlas_plan_result` with
 * a line of 0 and no row is written — because the planner was told this rule in
 * the format specification it was given, so a plan that breaks it is a plan the
 * planner can be asked to write again. */
static atlas_status merged_validations(const atlas_orch_argv *floor, size_t floor_count,
                                       const atlas_plan_doc_task *t, atlas_buf *out,
                                       atlas_plan_result *res, atlas_err *err) {
    if (floor_count + t->gate_count > ATLAS_ORCH_MAX_VALIDATIONS) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
                       "task %s would run %zu gates: the operator's floor of %zu, which is "
                       "prepended verbatim and cannot be replaced, plus %zu the plan adds. A task "
                       "runs at most %u gates.",
                       t->key, floor_count + t->gate_count, floor_count, t->gate_count,
                       (unsigned)ATLAS_ORCH_MAX_VALIDATIONS);
        return refuse_document(res, 0, err, msg);
    }
    atlas_orch_argv merged[ATLAS_ORCH_MAX_VALIDATIONS];
    memset(merged, 0, sizeof merged);
    atlas_status st = ATLAS_OK;
    size_t n = 0;
    for (size_t i = 0; st == ATLAS_OK && i < floor_count; i++) {
        st = argv_append_all(&merged[n++], &floor[i], err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < t->gate_count; i++) {
        st = argv_append_all(&merged[n++], &t->gates[i], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_orch_validations_encode(merged, n, out, err);
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&merged[i]);
    }
    return st;
}

static atlas_status insert_task(atlas_db *db, int64_t revision_id, int64_t plan_id,
                                const atlas_plan_doc_task *t, const char *validations,
                                atlas_err *err) {
    static const char INS[] =
        "INSERT INTO orch_plan_tasks(revision_id, plan_id, stage_no, task_key, kind, title,"
        "  prompt, validations) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";
    sqlite3_stmt *q = NULL;
    atlas_status st = atlas_db_prepare(db, INS, &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)sqlite3_bind_int64(q, 1, revision_id);
    (void)sqlite3_bind_int64(q, 2, plan_id);
    (void)sqlite3_bind_int64(q, 3, t->stage_no);
    st = atlas_db_bind_text_opt(db, q, 4, t->key, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, q, 5, t->is_tree ? "TREE" : "SIDE", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, q, 6, t->title, err);
    }
    if (st == ATLAS_OK) {
        /* The planner's prompt, byte for byte. It is UNTRUSTED_DATA, it is
         * bounded by the parser, and nothing here reads it. */
        st = atlas_db_bind_text_n(db, q, 7, atlas_buf_cstr(&t->prompt), t->prompt.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, q, 8, validations, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, q);
        return st;
    }
    return atlas_db_step_done(db, q, err);
}

static atlas_status op_revision_add(atlas_db *db, const atlas_plan_op *op, atlas_plan_result *out,
                                    atlas_err *err) {
    /* Cheapest first, and every one of them inside the transaction: a check that
     * a plan holds two revisions is worthless if a third can land between the
     * check and the insert. That is A11.0's rule for a run's submit path, and it
     * is this file's for the same reason. */
    const char *uid = atlas_buf_cstr(&op->plan_uid);
    plan_row plan;
    plan_row_init(&plan);
    bool found = false;
    atlas_status st = plan_by_uid(db, uid, &plan, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no plan named %s exists", uid);
    }
    if (st != ATLAS_OK) {
        plan_row_free(&plan);
        return st;
    }
    st = atlas_buf_set_str(&out->plan_uid, plan.plan_uid, err);

    /* (2) The revision number. Named by the caller and checked against what is
     * stored, so a resumed driver that re-issues an already-applied ingest is
     * refused with a sentence rather than writing a second revision holding the
     * same document. */
    int max_rev = 0;
    if (st == ATLAS_OK) {
        st = plan_max_rev(db, plan.id, &max_rev, err);
    }
    if (st == ATLAS_OK && op->rev_no != max_rev + 1) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "plan %s holds %d revision(s), so the next one is %d; %d was offered",
                           plan.plan_uid, max_rev, max_rev + 1, op->rev_no);
    }
    if (st == ATLAS_OK && op->rev_no > ATLAS_PLAN_MAX_REVISIONS) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "plan %s already holds its %d revisions and takes no further one",
                           plan.plan_uid, ATLAS_PLAN_MAX_REVISIONS);
    }
    if (st == ATLAS_OK && op->reason == ATLAS_PLAN_REVISION_UNKNOWN) {
        /* UNKNOWN is the vocabulary's zero and means "nobody filled this in".
         * The schema's CHECK would refuse it too; this is the sentence. */
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "a revision records why it exists");
    }
    if (st != ATLAS_OK) {
        plan_row_free(&plan);
        return st;
    }

    /* (3) The binding. A job becomes this plan's planner job k by carrying
     * exactly the correlation this plan's identifier produces — and by nothing
     * else. There is no bind RPC and no column to update, so the mapping cannot
     * be moved after the fact; the plan identifier is unguessable, which is what
     * makes the correlation a binding rather than a label. */
    const char *job_uid = atlas_buf_cstr(&op->planner_job_uid);
    planner_job job;
    bool job_found = false;
    st = job_for_revision(db, job_uid, &job, &job_found, err);
    if (st == ATLAS_OK && !job_found) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no job named %s exists", job_uid);
    }
    if (st == ATLAS_OK) {
        bool bound = false;
        atlas_buf want = ATLAS_BUF_INIT;
        for (int k = 1; st == ATLAS_OK && !bound && k <= ATLAS_PLAN_MAX_PLANNER_JOBS; k++) {
            st = atlas_plan_correlation_planner(plan.plan_uid, k, &want, err);
            if (st == ATLAS_OK && strcmp(atlas_buf_cstr(&want), job.correlation) == 0) {
                bound = true;
            }
        }
        atlas_buf_free(&want);
        if (st == ATLAS_OK && !bound) {
            /* The expected shape, spelled with this plan's own shortened
             * identifier, so an operator can compare it against the job's stored
             * correlation instead of deriving it. The job's actual correlation is
             * deliberately not echoed: it is a value a client chose, and this
             * sentence reaches a terminal. */
            char short_uid[ATLAS_PLAN_CORR_UID_LEN + 1u];
            if (!plan_uid_prefix(plan.plan_uid, short_uid)) {
                short_uid[0] = '\0';
            }
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "job %s is not a planner job of plan %s; a planner job of this plan "
                               "carries the correlation plan.%s.planner.<k> for k in 1..%d",
                               job_uid, plan.plan_uid, short_uid, ATLAS_PLAN_MAX_PLANNER_JOBS);
        }
    }

    /* (4) The role, asked of the driver name the job **stored**. Never of
     * anything a worker said about itself, and never of a name in the request:
     * an executor job's artifact can then never become a plan, whatever it
     * contains and whoever offers it. */
    if (st == ATLAS_OK) {
        const atlas_driver *d = atlas_driver_find(job.driver);
        if (d == NULL || d->role != ATLAS_DRIVER_ROLE_PLANNER) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "job %s ran under the driver %s, which is not a planner; only a "
                               "planner-role job can produce a plan revision",
                               job_uid, job.driver);
        }
    }

    /* (5) The outcome. A zero exit is not a success claim anywhere in Atlas, and
     * this is the state Atlas itself recorded after classifying the attempt. */
    if (st == ATLAS_OK && job.state != ATLAS_ORCH_STATE_SUCCEEDED) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "job %s ended %s; only a job that SUCCEEDED can produce a plan revision",
                           job_uid, atlas_orch_state_name(job.state));
    }

    /* (6) The artifact, read from the job's own stored row. The planner's bytes
     * never travel a second path: there is no parameter on this operation that
     * carries a document, so a caller cannot offer one the job did not produce. */
    atlas_buf bytes = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        bool art_found = false, stored = false;
        int64_t size = 0;
        st = planner_artifact(db, job_uid, &bytes, &size, &art_found, &stored, err);
        if (st == ATLAS_OK && !art_found) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "job %s produced no artifact named %s on its successful attempt",
                               job_uid, ATLAS_PLAN_ARTIFACT_NAME);
        }
        if (st == ATLAS_OK && !stored) {
            /* Described but not kept, which is what happens above the inline
             * ceiling. Atlas has the name, the size and the digest and not the
             * bytes, and it will not compile a plan out of a description. */
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "job %s recorded %s without storing its bytes, so there is no "
                               "document to compile",
                               job_uid, ATLAS_PLAN_ARTIFACT_NAME);
        }
        if (st == ATLAS_OK && size > (int64_t)ATLAS_PLAN_MAX_BYTES) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "job %s produced a %lld-byte %s; a plan document is at most %d bytes",
                               job_uid, (long long)size, ATLAS_PLAN_ARTIFACT_NAME,
                               ATLAS_PLAN_MAX_BYTES);
        }
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&bytes);
        plan_row_free(&plan);
        return st;
    }

    /* (7) The document. Everything above was about *whose* bytes these are;
     * this is the first thing that reads them, and it is a pure function of the
     * bytes and the plan's own parallelism bound. A refusal here is the
     * planner's and travels back typed. */
    atlas_plan_doc doc;
    memset(&doc, 0, sizeof doc);
    int line = 0;
    st = atlas_plan_parse(bytes.data, bytes.len, (int)plan.max_parallel, &doc, &line, err);
    if (st != ATLAS_OK) {
        carry_refusal(out, line, err);
        atlas_buf_free(&bytes);
        plan_row_free(&plan);
        return st;
    }

    /* (8) The merged gate list, per tree task. Decoded once, before the loop. */
    atlas_orch_argv floor[ATLAS_ORCH_MAX_VALIDATIONS];
    memset(floor, 0, sizeof floor);
    size_t floor_count = 0;
    st = atlas_orch_validations_decode(atlas_buf_cstr(&plan.gate_floor), floor,
                                       ATLAS_ORCH_MAX_VALIDATIONS, &floor_count, err);

    /* (9) The rows. The revision first, then its tasks: the tasks reference it,
     * and the whole of it is one transaction, so a reader never sees a revision
     * without the tasks it compiled to. */
    int64_t revision_id = 0;
    if (st == ATLAS_OK) {
        static const char INS[] =
            "INSERT INTO orch_plan_revisions(plan_id, rev_no, planner_job_uid, reason, content,"
            "  content_sha256, created_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, INS, &q, err);
        if (st == ATLAS_OK) {
            char hex[ATLAS_SHA256_HEX_LEN + 1u];
            atlas_sha256_hex(bytes.data, bytes.len, hex);
            char at[ATLAS_TS_MAX];
            atlas_now_iso8601(at, sizeof(at));
            (void)sqlite3_bind_int64(q, 1, plan.id);
            (void)sqlite3_bind_int64(q, 2, op->rev_no);
            st = atlas_db_bind_text_opt(db, q, 3, job_uid, err);
            if (st == ATLAS_OK) {
                st = atlas_db_bind_text_opt(db, q, 4,
                                            atlas_plan_revision_reason_name(op->reason), err);
            }
            if (st == ATLAS_OK) {
                /* Verbatim. The compiled task rows are a *reading* of these
                 * bytes, and a reading nobody can re-check against the original
                 * is a reading nobody can dispute. */
                st = atlas_db_bind_blob(db, q, 5, bytes.data, bytes.len, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_db_bind_text_opt(db, q, 6, hex, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_db_bind_text_opt(db, q, 7, at, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_db_step_done(db, q, err);
            } else {
                atlas_db_finish(db, q);
            }
        }
        if (st == ATLAS_OK) {
            revision_id = sqlite3_last_insert_rowid(db->h);
        }
    }
    for (size_t i = 0; st == ATLAS_OK && i < doc.task_count; i++) {
        const atlas_plan_doc_task *t = &doc.tasks[i];
        atlas_buf enc = ATLAS_BUF_INIT;
        if (t->is_tree) {
            st = merged_validations(floor, floor_count, t, &enc, out, err);
        } else {
            /* A side task declares no gate, and one is not invented for it: it
             * is a workspace job under A8's isolation, it cannot reach the
             * repository's tree, and a gate run over a workspace would be a gate
             * that proved nothing about the repository. It can veto the run's
             * acceptance by failing and can never grant it. */
            st = atlas_buf_set_str(&enc, "", err);
        }
        if (st == ATLAS_OK) {
            st = insert_task(db, revision_id, plan.id, t, atlas_buf_cstr(&enc), err);
        }
        atlas_buf_free(&enc);
    }

    if (st == ATLAS_OK) {
        out->rev_no = op->rev_no;
        out->task_count = (int)doc.task_count;
    }
    for (size_t i = 0; i < ATLAS_ORCH_MAX_VALIDATIONS; i++) {
        atlas_orch_argv_free(&floor[i]);
    }
    atlas_plan_doc_free(&doc);
    atlas_buf_free(&bytes);
    plan_row_free(&plan);
    return st;
}

/* --- the write point --------------------------------------------------------- */

atlas_status atlas_plan_apply_in_tx(atlas_db *db, const atlas_plan_op *op, atlas_plan_result *out,
                                    atlas_err *err) {
    if (db->read_only) {
        return atlas_err_set(err, ATLAS_ERR_DB,
                             "a plan cannot be written through a read-only handle");
    }
    switch (op->kind) {
    case ATLAS_PLAN_OP_CREATE: return op_create(db, op, out, err);
    case ATLAS_PLAN_OP_REVISION_ADD: return op_revision_add(db, op, out, err);
    case ATLAS_PLAN_OP_NONE: break;
    }
    return atlas_err_set(err, ATLAS_ERR_INTERNAL, "an empty plan operation was applied");
}

atlas_status atlas_plan_apply(atlas_db *db, const atlas_plan_op *op, atlas_plan_result *out,
                              atlas_err *err) {
    atlas_status st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_plan_apply_in_tx(db, op, out, err);
    if (st != ATLAS_OK) {
        /* The rollback leaves no row behind. It deliberately does not touch
         * `out`: a refused document's sentence and line are what the caller came
         * for, and they describe bytes that are still stored on the planner
         * job's artifact row whatever this transaction did. */
        atlas_db_rollback(db);
        return st;
    }
    st = atlas_db_commit(db, err);
    if (st != ATLAS_OK) {
        atlas_db_rollback(db);
    }
    return st;
}

/* --- the derived state ------------------------------------------------------
 *
 * Every rule below names the rows it reads. Nothing is cached, nothing is
 * stored, and the same rows give the same answer on every call — which is what
 * lets the plan driver's loop and `plan.get` be one function instead of two that
 * can disagree about whether a plan is finished.
 */

typedef struct planner_facts {
    int seen;     /* how many of the five correlations resolve to a job */
    int latest_k; /* the highest k that does, or 0 */
    /* True when some revision of this plan names the latest planner job as the
     * one it was compiled from. */
    bool consumed;
} planner_facts;

/* One job by its exact correlation, through `idx_orch_jobs_correlation`.
 *
 * `ORDER BY id` rather than assuming uniqueness: a correlation is not a unique
 * column and never has been. The idempotency key is what stops a resumed driver
 * creating a second job for one slot, and if two ever existed the first is the
 * one every later read would also pick. */
static atlas_status job_by_correlation(atlas_db *db, const char *correlation, char uid_out[],
                                       size_t uid_cap, atlas_orch_state *state_out,
                                       char run_out[], size_t run_cap, bool *found,
                                       atlas_err *err) {
    static const char SQL[] = "SELECT job_uid, state, run_uid FROM orch_jobs"
                              "  WHERE correlation = ?1 ORDER BY id LIMIT 1;";
    *found = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, correlation, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            s = atlas_db_col_copy(st, 0, uid_out, uid_cap, "job_uid", err);
            if (s == ATLAS_OK && !atlas_orch_state_parse(atlas_db_col_text(st, 1), state_out)) {
                s = atlas_err_set(err, ATLAS_ERR_DB, "a job holds a state Atlas does not recognise");
            }
            if (s == ATLAS_OK && run_out != NULL) {
                s = atlas_db_col_copy(st, 2, run_out, run_cap, "run_uid", err);
            }
            *found = (s == ATLAS_OK);
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a plan's jobs");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

static atlas_status revision_names_job(atlas_db *db, int64_t plan_id, const char *job_uid,
                                       bool *out, atlas_err *err) {
    static const char SQL[] = "SELECT 1 FROM orch_plan_revisions"
                              "  WHERE plan_id = ?1 AND planner_job_uid = ?2 LIMIT 1;";
    *out = false;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, plan_id);
    s = atlas_db_bind_text_opt(db, st, 2, job_uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            *out = true;
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a plan's revisions");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* Reads: `orch_jobs` by the five planner correlations, and
 * `orch_plan_revisions.planner_job_uid` for whether the newest of them has been
 * turned into a revision.
 *
 * The uid and state land in `state` directly rather than in a local that is
 * copied afterwards, because the destination's field is the one whose width has
 * to hold them: a copy through an intermediate of a different size is exactly
 * how a field-by-field transfer loses something, which is A1's ctime lesson in
 * miniature. `job_by_correlation` writes nothing when it finds nothing, so the
 * highest k that resolves is the one left standing. */
static atlas_status read_planner_facts(atlas_db *db, const plan_row *plan, atlas_plan_state *state,
                                       planner_facts *out, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    atlas_buf corr = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    for (int k = 1; st == ATLAS_OK && k <= ATLAS_PLAN_MAX_PLANNER_JOBS; k++) {
        st = atlas_plan_correlation_planner(plan->plan_uid, k, &corr, err);
        if (st != ATLAS_OK) {
            break;
        }
        bool found = false;
        st = job_by_correlation(db, atlas_buf_cstr(&corr), state->planner_job_uid,
                                sizeof(state->planner_job_uid), &state->planner_job_state, NULL,
                                0u, &found, err);
        if (st == ATLAS_OK && found) {
            out->seen++;
            out->latest_k = k;
        }
    }
    atlas_buf_free(&corr);
    if (st == ATLAS_OK && out->latest_k > 0) {
        st = revision_names_job(db, plan->id, state->planner_job_uid, &out->consumed, err);
    }
    return st;
}

static atlas_status run_status_of(atlas_db *db, const char *run_uid, atlas_orch_run_status *out,
                                  atlas_err *err) {
    static const char SQL[] = "SELECT status FROM orch_runs WHERE run_uid = ?1;";
    *out = ATLAS_ORCH_RUN_UNKNOWN;
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    s = atlas_db_bind_text_opt(db, st, 1, run_uid, err);
    if (s == ATLAS_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            if (!atlas_orch_run_status_parse(atlas_db_col_text(st, 0), out)) {
                s = atlas_err_set(err, ATLAS_ERR_DB, "run %s holds a status Atlas does not recognise",
                                  run_uid);
            }
        } else if (rc != SQLITE_DONE) {
            s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a run");
        }
    }
    atlas_db_finish(db, st);
    return s;
}

/* Reads: `orch_plan_tasks` of the latest revision, then for each of them
 * `orch_jobs` by the task's correlation, and for a tree task `orch_runs` by that
 * job's `run_uid`.
 *
 * Ordered by `(stage_no, id)`, which is the order the document listed them in
 * within a stage. The replan composer renders completed work in array order, so
 * a read that came back in a different order on a different day would compose a
 * different prompt from the same rows. */
static atlas_status read_tasks(atlas_db *db, const plan_row *plan, int rev_no, int64_t revision_id,
                               atlas_plan_state *out, atlas_err *err) {
    static const char SQL[] = "SELECT task_key, stage_no, kind, title FROM orch_plan_tasks"
                              "  WHERE revision_id = ?1 ORDER BY stage_no, id;";
    sqlite3_stmt *st = NULL;
    atlas_status s = atlas_db_prepare(db, SQL, &st, err);
    if (s != ATLAS_OK) {
        return s;
    }
    (void)sqlite3_bind_int64(st, 1, revision_id);
    int rc = SQLITE_DONE;
    while (s == ATLAS_OK && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (out->task_count >= ATLAS_PLAN_MAX_TASKS) {
            s = atlas_err_set(err, ATLAS_ERR_DB,
                              "plan %s revision %d holds more than %d tasks", plan->plan_uid,
                              rev_no, ATLAS_PLAN_MAX_TASKS);
            break;
        }
        atlas_plan_task_view *v = &out->tasks[out->task_count];
        memset(v, 0, sizeof(*v));
        s = atlas_db_col_copy(st, 0, v->task_key, sizeof(v->task_key), "task_key", err);
        if (s == ATLAS_OK) {
            v->stage_no = (int)sqlite3_column_int64(st, 1);
            v->is_tree = strcmp(atlas_db_col_text(st, 2), "TREE") == 0;
            s = atlas_db_col_copy(st, 3, v->title, sizeof(v->title), "title", err);
        }
        if (s == ATLAS_OK) {
            out->task_count++;
        }
    }
    if (s == ATLAS_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        s = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a revision's tasks");
    }
    atlas_db_finish(db, st);
    if (s != ATLAS_OK) {
        return s;
    }

    atlas_buf corr = ATLAS_BUF_INIT;
    for (int i = 0; s == ATLAS_OK && i < out->task_count; i++) {
        atlas_plan_task_view *v = &out->tasks[i];
        s = atlas_plan_correlation_task(plan->plan_uid, rev_no, v->task_key, &corr, err);
        if (s != ATLAS_OK) {
            break;
        }
        bool found = false;
        s = job_by_correlation(db, atlas_buf_cstr(&corr), v->job_uid, sizeof(v->job_uid),
                               &v->job_state, v->run_uid, sizeof(v->run_uid), &found, err);
        if (s != ATLAS_OK) {
            break;
        }
        if (!found) {
            /* Not submitted yet. Empty uid and UNKNOWN state, which is what the
             * struct documents and is never read as "finished". */
            v->job_uid[0] = '\0';
            v->job_state = ATLAS_ORCH_STATE_UNKNOWN;
            v->run_uid[0] = '\0';
            continue;
        }
        if (!v->is_tree) {
            /* A side task's run is its stage's — it is submitted as a sibling of
             * the stage's tree task — but a side task's own outcome is its job's
             * state, and the run belongs to the tree task's row. Clearing it here
             * keeps the struct's contract exact: `run_status` on a side task
             * would be the *stage's* answer wearing a task's name. */
            v->run_uid[0] = '\0';
            continue;
        }
        if (v->run_uid[0] != '\0') {
            s = run_status_of(db, v->run_uid, &v->run_status, err);
        }
    }
    atlas_buf_free(&corr);
    return s;
}

atlas_status atlas_db_plan_state_derive(atlas_db *db, const char *plan_uid, atlas_plan_state *out,
                                        atlas_err *err) {
    memset(out, 0, sizeof(*out));

    plan_row plan;
    plan_row_init(&plan);
    bool found = false;
    atlas_status st = plan_by_uid(db, plan_uid, &plan, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no plan named %s exists", plan_uid);
    }
    if (st != ATLAS_OK) {
        plan_row_free(&plan);
        return st;
    }

    planner_facts pf;
    memset(&pf, 0, sizeof pf);
    st = read_planner_facts(db, &plan, out, &pf, err);
    out->planner_jobs_seen = pf.seen;

    /* The latest revision, which is the only one whose work is executed: a
     * replan produces a complete plan for the remaining work, so an earlier
     * revision's tasks are history rather than outstanding. */
    int64_t revision_id = 0;
    if (st == ATLAS_OK) {
        static const char SQL[] = "SELECT id, rev_no FROM orch_plan_revisions"
                                  "  WHERE plan_id = ?1 ORDER BY rev_no DESC LIMIT 1;";
        sqlite3_stmt *q = NULL;
        st = atlas_db_prepare(db, SQL, &q, err);
        if (st == ATLAS_OK) {
            (void)sqlite3_bind_int64(q, 1, plan.id);
            int rc = sqlite3_step(q);
            if (rc == SQLITE_ROW) {
                revision_id = sqlite3_column_int64(q, 0);
                out->rev_no = (int)sqlite3_column_int64(q, 1);
            } else if (rc != SQLITE_DONE) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read a plan's revisions");
            }
            atlas_db_finish(db, q);
        }
    }
    if (st == ATLAS_OK && out->rev_no > 0) {
        st = read_tasks(db, &plan, out->rev_no, revision_id, out, err);
    }
    if (st != ATLAS_OK) {
        plan_row_free(&plan);
        return st;
    }

    /* --- the derivation -----------------------------------------------------
     *
     * Four counts over the latest revision's tasks, and four facts about the
     * planner. Nothing else, and nothing stored.
     *
     * A **tree** task's outcome is its stage-run's status, not its job's: a
     * failed repo-tree job whose run is still ACTIVE has a follow-up coming, and
     * reading the job would call that stage finished before Atlas had settled it.
     * A **side** task's outcome is its job's state, because a workspace sibling
     * settles nothing on its own — it can veto its run's acceptance by failing
     * and can never grant it. */
    int good = 0, bad = 0, active = 0, unsubmitted = 0;
    for (int i = 0; i < out->task_count; i++) {
        const atlas_plan_task_view *v = &out->tasks[i];
        if (v->job_uid[0] == '\0') {
            unsubmitted++;
            continue;
        }
        if (v->is_tree) {
            if (v->run_status == ATLAS_ORCH_RUN_ACCEPTED) {
                good++;
                out->stages_accepted++;
            } else if (v->run_status == ATLAS_ORCH_RUN_BLOCKED) {
                bad++;
            } else {
                active++;
            }
            continue;
        }
        if (v->job_state == ATLAS_ORCH_STATE_SUCCEEDED) {
            good++;
        } else if (atlas_orch_state_is_terminal(v->job_state)) {
            bad++;
        } else {
            active++;
        }
    }

    const bool rev_budget = out->rev_no < ATLAS_PLAN_MAX_REVISIONS;
    const bool planner_budget = pf.seen < ATLAS_PLAN_MAX_PLANNER_JOBS;
    /* The newest planner job, when no revision was compiled from it. Three
     * dispositions, and the rows cannot tell the middle one from a driver that
     * died between the job finishing and the ingest — which is why the ingest is
     * deterministic and re-runnable: asked again with the same bytes it produces
     * the same revision or the same refusal. */
    const bool planner_unconsumed = pf.latest_k > 0 && !pf.consumed;
    const bool planner_pending =
        planner_unconsumed && !atlas_orch_state_is_terminal(out->planner_job_state);
    const bool planner_ready =
        planner_unconsumed && out->planner_job_state == ATLAS_ORCH_STATE_SUCCEEDED;

    if (out->rev_no >= 1 && bad == 0 && active == 0 && unsubmitted == 0 && out->task_count > 0) {
        /* COMPLETED. Every tree task's run ACCEPTED and every side task's job
         * SUCCEEDED, over the latest revision. */
        out->status = ATLAS_PLAN_STATUS_COMPLETED;
    } else if (out->rev_no >= 1 && bad == 0) {
        /* EXECUTING. Work outstanding and nothing wrong with it: a task not yet
         * submitted, a job still going, or a stage-run Atlas has not settled. */
        out->status = ATLAS_PLAN_STATUS_EXECUTING;
    } else if (out->rev_no >= 1 && active > 0) {
        /* EXECUTING, and doomed. A11.6's rule: one task's failure must not break
         * another task's execution, so the plan does not stop mid-revision. The
         * verdict waits for quiescence, exactly as a run's does. */
        out->status = ATLAS_PLAN_STATUS_EXECUTING;
    } else if (planner_pending) {
        /* PLANNING. A planner job is in flight — either the first, or the replan
         * this revision's failure already earned. Asked, not yet answered. */
        out->status = ATLAS_PLAN_STATUS_PLANNING;
    } else if (planner_ready && rev_budget && planner_budget) {
        /* PLANNING, and `replan_wanted`. The newest planner job SUCCEEDED and no
         * revision names it: either its document was refused, or the driver has
         * not offered it yet. The rows cannot distinguish those and do not have
         * to — the driver re-runs `plan.revision_add`, which either compiles the
         * document or reproduces the same refusal from the same stored bytes. */
        out->status = ATLAS_PLAN_STATUS_PLANNING;
        out->replan_wanted = true;
    } else if (rev_budget && planner_budget) {
        /* Budgets remain, and what would move this plan is a planner document.
         * With a revision already compiled that is a *replan*, which is the
         * driver's cue; with none it is still the initial plan. */
        if (out->rev_no >= 1) {
            out->status = ATLAS_PLAN_STATUS_NEEDS_REPLAN;
            out->replan_wanted = true;
        } else {
            out->status = ATLAS_PLAN_STATUS_PLANNING;
        }
    } else {
        /* BLOCKED. A replan is what this plan needs and no budget remains to
         * produce one — either the five planner jobs are spent, or the three
         * revisions are. Derived rather than stored, so it is not a trap: if the
         * outstanding planner document is later ingested successfully, the very
         * next read says EXECUTING. */
        out->status = ATLAS_PLAN_STATUS_BLOCKED;
    }

    plan_row_free(&plan);
    return ATLAS_OK;
}

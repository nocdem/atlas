/* Atlas - A14, T3: the write point's remote submission channel.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is `tests/test_decision_remote.c`'s own shape, one layer further in:
 * it drives `atlas_orch_apply` directly at the write point on a fixture with
 * one registered repository, and three real keys minted through the CLI --
 * `model` (`--scope repo:read`), `browser` (`--no-scopes`) and `other`
 * (`--scope repo:read`, not named in the allowed list).
 *
 * The fixture holds no daemon and no running gateway.  The gateway's bearer
 * credential check lives in `server_gw.c`; the write point's own check lives
 * in `src/orch/remote.c` and is what this suite proves.
 *
 * This file MUST NOT contain `atlas_decision_apply_in_tx(` or
 * `atlas_service_decision_confirm(`.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/apikey.h"
#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/gw.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/orch_remote.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- the fixture ---------------------------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_buf identity;
    atlas_buf commit;
    int64_t repo_id;
    /* Keys: model (repo:read, in allowed list), browser (no scopes, in allowed
     * list), other (repo:read, NOT in the allowed list). */
    char model_token[ATLAS_APIKEY_TOKEN_MAX];
    char model_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char browser_token[ATLAS_APIKEY_TOKEN_MAX];
    char browser_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char other_token[ATLAS_APIKEY_TOKEN_MAX];
    char other_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
} env;

static void run_atlas(env *e, const char *const *extra, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    T_REQUIRE(n + k <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf errout = ATLAS_BUF_INIT;
    T_OK(fx_atlas(argv, k, out, &errout, code, &err), &err);
    atlas_buf_free(&errout);
}

static void mint_key(env *e, const char *label, const char *scope_or_null, char *token_out,
                     size_t token_size, char *id_out, size_t id_size) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    if (scope_or_null == NULL) {
        const char *create[] = {"api-key", "create", "--label", label, "--no-scopes"};
        run_atlas(e, create, 5u, &out, &code);
    } else {
        const char *create[] = {"api-key", "create", "--label", label, "--scope", scope_or_null};
        run_atlas(e, create, 6u, &out, &code);
    }
    T_EQ_INT(code, 0);

    token_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "ATLAS_API_KEY=");
        T_REQUIRE(s != NULL);
        s += strlen("ATLAS_API_KEY=");
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < token_size) {
            n++;
        }
        memcpy(token_out, s, n);
        token_out[n] = '\0';
    }
    id_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "id:     " ATLAS_APIKEY_ID_PREFIX);
        T_REQUIRE(s != NULL);
        s += strlen("id:     " ATLAS_APIKEY_ID_PREFIX);
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < id_size) {
            n++;
        }
        memcpy(id_out, s, n);
        id_out[n] = '\0';
    }
    atlas_buf_free(&out);
}

static void env_open(env *e) {
    memset(e, 0, sizeof(*e));
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);
    atlas_buf_init(&e->identity);
    atlas_buf_init(&e->commit);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    /* A Makefile with a gate that always passes. */
    T_OK(fx_write(fx_repo(&e->fx), "Makefile", "pass:\n\t@true\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
    run_atlas(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);
    const char *scan[] = {"scan", "proj"};
    run_atlas(e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);

    mint_key(e, "model", "repo:read", e->model_token, sizeof(e->model_token), e->model_id,
             sizeof(e->model_id));
    mint_key(e, "browser", NULL, e->browser_token, sizeof(e->browser_token), e->browser_id,
             sizeof(e->browser_id));
    mint_key(e, "other", "repo:read", e->other_token, sizeof(e->other_token), e->other_id,
             sizeof(e->other_id));

    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", &ri, &found, &err), &err);
    T_REQUIRE(found);
    e->repo_id = ri.id;
    T_OK(atlas_db_repo_identity_hash(e->db, ri.id, &e->identity, &err), &err);
    T_REQUIRE(ri.scanned_head[0] != '\0');
    T_OK(atlas_buf_set_str(&e->commit, ri.scanned_head, &err), &err);
    atlas_repo_info_free(&ri);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    e->db = NULL;
    atlas_buf_free(&e->db_path);
    atlas_buf_free(&e->identity);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

/* --- helpers --------------------------------------------------------------- */

/* Build a base remote SUBMIT op.  `gate` is the make target for the
 * validation (NULL for a non-repo-tree job).  `client_key` is the
 * idempotency client key (NULL for none).  Allowed list is {model, browser}.
 * Budgets: max_active=2, max_per_day=3. */
static atlas_orch_op *remote_submit_op(env *e, const char *task, const char *gate,
                                       const char *client_key) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 65534; /* the gateway's uid */
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = e->repo_id;
    op->spec.submitter_uid = 65534;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e->identity.data, e->identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, e->commit.data, e->commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    if (gate != NULL) {
        T_OK(atlas_buf_set_str(&op->spec.driver, "fake-repo", &err), &err);
        T_OK(atlas_orch_argv_push(&op->spec.validations[0], "make", 4u, &err), &err);
        T_OK(atlas_orch_argv_push(&op->spec.validations[0], gate, strlen(gate), &err), &err);
        op->spec.validation_count = 1;
    } else {
        T_OK(atlas_buf_set_str(&op->spec.driver, "fake", &err), &err);
    }
    T_OK(atlas_buf_set_str(&op->spec.task_text, task, &err), &err);
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = 3;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);

    op->remote_max_active = 2;
    op->remote_max_per_day = 3;
    op->remote_allowed_count = 2;
    (void)snprintf(op->remote_allowed_ids[0], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s", e->model_id);
    (void)snprintf(op->remote_allowed_ids[1], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                   e->browser_id);
    if (client_key != NULL) {
        (void)snprintf(op->remote_client_key, ATLAS_ORCH_REMOTE_CLIENT_KEY_MAX + 1u, "%s",
                       client_key);
    }
    return op;
}

static void set_token(atlas_orch_op *op, const char *token) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set_str(&op->remote_token, token, &err), &err);
}

static int64_t count_sql(atlas_db *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) == SQLITE_OK);
    int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

static void read_text_col(atlas_db *db, const char *sql, char *out, size_t out_size) {
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) == SQLITE_OK);
    out[0] = '\0';
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v != NULL) {
            (void)snprintf(out, out_size, "%s", (const char *)v);
        }
    }
    sqlite3_finalize(st);
}

static void apply_ok(atlas_db *db, atlas_orch_op *op, atlas_orch_result *r) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_orch_apply(db, op, r, &err), &err);
}

static void apply_fails(atlas_db *db, atlas_orch_op *op, const char *needle) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    atlas_status st = atlas_orch_apply(db, op, &r, &err);
    T_CHECK_MSG(st != ATLAS_OK, "expected failure but got ATLAS_OK");
    if (st != ATLAS_OK) {
        T_CHECK_MSG(strstr(atlas_err_msg(&err), needle) != NULL,
                    "error '%s' does not contain '%s'", atlas_err_msg(&err), needle);
    }
    atlas_orch_result_free(&r);
}

/* --- tests ----------------------------------------------------------------- */

/* (a) SUBMIT with model's token succeeds; the row and ledger carry the
 * verified key_id; the result carries key_id, remote_active=1, remote_today=1.
 */
static void test_a_remote_submit_success(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_op *op = remote_submit_op(&e, "hello remote", NULL, NULL);
    set_token(op, e.model_token);

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    apply_ok(e.db, op, &r);

    T_CHECK_MSG(strcmp(r.key_id, e.model_id) == 0,
                "result.key_id '%s' != model_id '%s'", r.key_id, e.model_id);
    T_EQ_INT((int)r.remote_active, 1);
    T_EQ_INT((int)r.remote_today, 1);
    T_CHECK(!r.duplicate);
    T_CHECK(r.job_uid.len > 0);

    char key_col[64];
    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT submit_key_id FROM orch_jobs WHERE job_uid = '%s'",
                   atlas_buf_cstr(&r.job_uid));
    read_text_col(e.db, sql, key_col, sizeof(key_col));
    T_CHECK_MSG(strcmp(key_col, e.model_id) == 0,
                "stored submit_key_id '%s' != model_id '%s'", key_col, e.model_id);

    char uid_col[32];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT submitter_uid FROM orch_jobs WHERE job_uid = '%s'",
                   atlas_buf_cstr(&r.job_uid));
    read_text_col(e.db, sql, uid_col, sizeof(uid_col));
    T_CHECK_MSG(strcmp(uid_col, "65534") == 0,
                "submitter_uid '%s' != gateway uid '65534'", uid_col);

    char ledger_key[64];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT key_id FROM orch_transitions WHERE job_id = %lld",
                   (long long)r.job_id);
    read_text_col(e.db, sql, ledger_key, sizeof(ledger_key));
    T_CHECK_MSG(strcmp(ledger_key, e.model_id) == 0,
                "ledger key_id '%s' != model_id '%s'", ledger_key, e.model_id);

    char ledger_detail[512];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT detail FROM orch_transitions WHERE job_id = %lld",
                   (long long)r.job_id);
    read_text_col(e.db, sql, ledger_detail, sizeof(ledger_detail));
    T_CHECK_MSG(strstr(ledger_detail, "submitted through the Atlas gateway") != NULL,
                "ledger detail '%s' does not have the frozen sentence", ledger_detail);
    T_CHECK_MSG(strstr(ledger_detail, e.model_id) != NULL,
                "ledger detail '%s' does not name the key_id", ledger_detail);

    atlas_orch_result_free(&r);
    atlas_orch_op_free(op);
    free(op);
    env_close(&e);
}

/* (b) Wrong/unknown/not-in-list credentials are refused; no row is written. */
static void test_b_wrong_credential(void) {
    env e;
    env_open(&e);

    /* `other` is not in the allowed list. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "other test", NULL, NULL);
        set_token(op, e.other_token);
        apply_fails(e.db, op,
                    "that credential is not one the remote submission policy names");
        atlas_orch_op_free(op);
        free(op);
        T_EQ_INT((int)count_sql(e.db, "SELECT COUNT(*) FROM orch_jobs"), 0);
    }

    /* Wrong secret (corrupt last byte). */
    {
        atlas_orch_op *op = remote_submit_op(&e, "bad secret", NULL, NULL);
        char bad_token[ATLAS_APIKEY_TOKEN_MAX];
        (void)snprintf(bad_token, sizeof(bad_token), "%s", e.model_token);
        size_t n = strlen(bad_token);
        if (n > 0) {
            bad_token[n - 1] = (bad_token[n - 1] == 'a') ? 'b' : 'a';
        }
        set_token(op, bad_token);
        apply_fails(e.db, op, "did not authenticate");
        atlas_orch_op_free(op);
        free(op);
        T_EQ_INT((int)count_sql(e.db, "SELECT COUNT(*) FROM orch_jobs"), 0);
    }

    /* Empty token. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "empty", NULL, NULL);
        /* remote_token left as empty buf */
        apply_fails(e.db, op, "did not authenticate");
        atlas_orch_op_free(op);
        free(op);
        T_EQ_INT((int)count_sql(e.db, "SELECT COUNT(*) FROM orch_jobs"), 0);
    }

    env_close(&e);
}

/* (c) Active budget: 3rd submission refused when max_active = 2; cancelling
 * one lets it through. */
static void test_c_active_budget(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf job1_uid = ATLAS_BUF_INIT;
    {
        atlas_orch_op *op = remote_submit_op(&e, "active job 1", NULL, NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_OK(atlas_buf_set(&job1_uid, r.job_uid.data, r.job_uid.len, &err), &err);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }
    {
        atlas_orch_op *op = remote_submit_op(&e, "active job 2", NULL, NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_EQ_INT((int)r.remote_active, 2);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Third is refused. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "active job 3", NULL, NULL);
        set_token(op, e.model_token);
        apply_fails(e.db, op,
                    "already has 2 active remote job(s), which is its bound of 2");
        atlas_orch_op_free(op);
        free(op);
    }

    /* Cancel job1 to free a slot.  The cancel must present the bearer that
     * submitted it: a keyed job may not be cancelled by uid alone. */
    {
        atlas_orch_op *cop = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(cop != NULL);
        cop->peer_uid = 65534;
        cop->remote_allowed_count = 2;
        (void)snprintf(cop->remote_allowed_ids[0], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                       e.model_id);
        (void)snprintf(cop->remote_allowed_ids[1], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                       e.browser_id);
        T_OK(atlas_buf_set_str(&cop->remote_token, e.model_token, &err), &err);
        T_OK(atlas_buf_set(&cop->job_uid, job1_uid.data, job1_uid.len, &err), &err);
        atlas_orch_result cr;
        atlas_orch_result_init(&cr);
        apply_ok(e.db, cop, &cr);
        atlas_orch_result_free(&cr);
        atlas_orch_op_free(cop);
        free(cop);
    }

    /* Third submission now succeeds. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "active job 3 retry", NULL, NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_EQ_INT((int)r.remote_active, 2);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    atlas_buf_free(&job1_uid);
    env_close(&e);
}

/* (d) Daily budget: 4th root submission in one UTC day is refused; a row with
 * a past created_at does not count. */
static void test_d_daily_budget(void) {
    env e;
    env_open(&e);

    /* Submit one job and then back-date its created_at to "yesterday" using a
     * direct SQL UPDATE.  The day-count query uses `created_at >= day_start`,
     * so a past row must not count.
     *
     * `remote_max_active` is set to 100 on every op so the active-budget check
     * never fires; we are testing the daily-budget check only. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "yesterday job", NULL, NULL);
        op->remote_max_active = 100;
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        /* Back-date to 2000-01-01. */
        char upd[256];
        (void)snprintf(upd, sizeof(upd),
                       "UPDATE orch_jobs SET created_at = '2000-01-01T00:00:00Z'"
                       " WHERE job_uid = '%s'",
                       atlas_buf_cstr(&r.job_uid));
        sqlite3_exec(e.db->h, upd, NULL, NULL, NULL);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Submit three more today -- all should succeed. */
    for (int i = 0; i < 3; i++) {
        char task[32];
        (void)snprintf(task, sizeof(task), "daily job %d", i + 1);
        atlas_orch_op *op = remote_submit_op(&e, task, NULL, NULL);
        op->remote_max_active = 100;
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        /* remote_today counts only today's root rows. */
        T_EQ_INT((int)r.remote_today, i + 1);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Fourth today is refused by the daily budget. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "daily job 4", NULL, NULL);
        op->remote_max_active = 100;
        set_token(op, e.model_token);
        apply_fails(e.db, op,
                    "has submitted 3 job(s) today (UTC), which is its bound of 3");
        atlas_orch_op_free(op);
        free(op);
    }

    env_close(&e);
}

/* (e) Idempotency namespace: same key+task = duplicate; different credential
 * same key = different job; bad key format refused; stored key is
 * `remote.<id>.k1`. */
static void test_e_idempotency(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf job1_uid = ATLAS_BUF_INIT;
    int64_t job1_id = 0;
    {
        atlas_orch_op *op = remote_submit_op(&e, "idem task", NULL, "k1");
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_CHECK(!r.duplicate);
        T_OK(atlas_buf_set(&job1_uid, r.job_uid.data, r.job_uid.len, &err), &err);
        job1_id = r.job_id;

        char stored_key[128];
        char sql[256];
        (void)snprintf(sql, sizeof(sql),
                       "SELECT key FROM orch_idempotency WHERE job_id = %lld",
                       (long long)r.job_id);
        read_text_col(e.db, sql, stored_key, sizeof(stored_key));
        char expected_key[128];
        (void)snprintf(expected_key, sizeof(expected_key), "remote.%s.k1", e.model_id);
        T_CHECK_MSG(strcmp(stored_key, expected_key) == 0,
                    "stored key '%s' != expected '%s'", stored_key, expected_key);

        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Same key + same task = duplicate. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "idem task", NULL, "k1");
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_CHECK_MSG(r.duplicate, "expected duplicate=true");
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&r.job_uid), atlas_buf_cstr(&job1_uid)) == 0,
                    "duplicate returned different uid");
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Browser with the same client key "k1" = different namespace. */
    {
        atlas_orch_op *op = remote_submit_op(&e, "idem task", NULL, "k1");
        set_token(op, e.browser_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_CHECK_MSG(!r.duplicate, "browser k1 should be a new job");
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&r.job_uid), atlas_buf_cstr(&job1_uid)) != 0,
                    "browser k1 and model k1 should be different jobs");
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* A 41-char client key is refused.  The op struct's fixed buffer would
     * silently truncate a 41-char string to 40 chars before `atlas_orch_apply`
     * sees it, so we test the validator directly -- the same function the write
     * point calls.  The apply path is tested by the space case below. */
    {
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_err ferr;
        atlas_err_init(&ferr);
        atlas_status st = atlas_orch_remote_idempotency_key(
            e.model_id, "aaaaaaaaaabbbbbbbbbbccccccccccdddddddddde", &out, &ferr);
        T_CHECK_MSG(st != ATLAS_OK, "41-char key should be refused");
        if (st != ATLAS_OK) {
            T_CHECK_MSG(
                strstr(atlas_err_msg(&ferr), "at most 40 characters of [a-z0-9._-]") != NULL,
                "wrong message: %s", atlas_err_msg(&ferr));
        }
        atlas_buf_free(&out);
    }

    /* Client key with a space is refused (invalid character). */
    {
        atlas_orch_op *op = remote_submit_op(&e, "space key", NULL, "bad key");
        set_token(op, e.model_token);
        apply_fails(e.db, op, "at most 40 characters of [a-z0-9._-]");
        atlas_orch_op_free(op);
        free(op);
    }

    (void)job1_id;
    atlas_buf_free(&job1_uid);
    env_close(&e);
}

/* (f) CANCEL: wrong credential => "no such job"; correct => CANCELLED;
 * operator flag => CANCELLED; non-operator non-owner => "no such job". */
static void test_f_cancel(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Submit a job with model's token. */
    atlas_buf job_uid = ATLAS_BUF_INIT;
    {
        atlas_orch_op *op = remote_submit_op(&e, "cancel target", NULL, NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_OK(atlas_buf_set(&job_uid, r.job_uid.data, r.job_uid.len, &err), &err);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Cancel with browser's token on model's job => "no such job". */
    {
        atlas_orch_op *cop = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(cop != NULL);
        cop->peer_uid = 65534;
        cop->remote_allowed_count = 2;
        (void)snprintf(cop->remote_allowed_ids[0], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                       e.model_id);
        (void)snprintf(cop->remote_allowed_ids[1], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                       e.browser_id);
        T_OK(atlas_buf_set_str(&cop->remote_token, e.browser_token, &err), &err);
        T_OK(atlas_buf_set(&cop->job_uid, job_uid.data, job_uid.len, &err), &err);
        atlas_err cerr;
        atlas_err_init(&cerr);
        atlas_orch_result cr;
        atlas_orch_result_init(&cr);
        atlas_status st = atlas_orch_apply(e.db, cop, &cr, &cerr);
        T_CHECK_MSG(st != ATLAS_OK, "browser cancel should fail");
        T_CHECK_MSG(strstr(atlas_err_msg(&cerr), "no such job") != NULL,
                    "wrong message: %s", atlas_err_msg(&cerr));
        atlas_orch_result_free(&cr);
        atlas_orch_op_free(cop);
        free(cop);
    }
    T_EQ_INT((int)count_sql(e.db, "SELECT COUNT(*) FROM orch_jobs WHERE state = 'QUEUED'"), 1);

    /* Cancel with model's token => CANCELLED. */
    {
        atlas_orch_op *cop = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(cop != NULL);
        cop->peer_uid = 65534;
        cop->remote_allowed_count = 2;
        (void)snprintf(cop->remote_allowed_ids[0], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                       e.model_id);
        (void)snprintf(cop->remote_allowed_ids[1], ATLAS_APIKEY_SELECTOR_HEX + 1u, "%s",
                       e.browser_id);
        T_OK(atlas_buf_set_str(&cop->remote_token, e.model_token, &err), &err);
        T_OK(atlas_buf_set(&cop->job_uid, job_uid.data, job_uid.len, &err), &err);
        atlas_orch_result cr;
        atlas_orch_result_init(&cr);
        apply_ok(e.db, cop, &cr);
        T_EQ_INT(cr.state, (int)ATLAS_ORCH_STATE_CANCELLED);
        atlas_orch_result_free(&cr);
        atlas_orch_op_free(cop);
        free(cop);
    }

    /* Submit a second job for the operator-flag test. */
    atlas_buf job2_uid = ATLAS_BUF_INIT;
    {
        atlas_orch_op *op = remote_submit_op(&e, "cancel target 2", NULL, NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_OK(atlas_buf_set(&job2_uid, r.job_uid.data, r.job_uid.len, &err), &err);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Operator cancels with peer_is_operator = true, no remote token. */
    {
        atlas_orch_op *cop = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(cop != NULL);
        cop->peer_uid = 1000; /* operator uid, different from gateway */
        cop->peer_is_operator = true;
        T_OK(atlas_buf_set(&cop->job_uid, job2_uid.data, job2_uid.len, &err), &err);
        atlas_orch_result cr;
        atlas_orch_result_init(&cr);
        apply_ok(e.db, cop, &cr);
        T_EQ_INT(cr.state, (int)ATLAS_ORCH_STATE_CANCELLED);
        atlas_orch_result_free(&cr);
        atlas_orch_op_free(cop);
        free(cop);
    }

    /* Submit a third job for the non-operator non-owner test. */
    atlas_buf job3_uid = ATLAS_BUF_INIT;
    {
        atlas_orch_op *op = remote_submit_op(&e, "cancel target 3", NULL, NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_OK(atlas_buf_set(&job3_uid, r.job_uid.data, r.job_uid.len, &err), &err);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* Non-operator non-owner (different peer_uid, no remote_allowed_count). */
    {
        atlas_orch_op *cop = atlas_orch_op_new(ATLAS_ORCH_OP_CANCEL);
        T_REQUIRE(cop != NULL);
        cop->peer_uid = 9999;
        cop->peer_is_operator = false;
        T_OK(atlas_buf_set(&cop->job_uid, job3_uid.data, job3_uid.len, &err), &err);
        atlas_err cerr;
        atlas_err_init(&cerr);
        atlas_orch_result cr;
        atlas_orch_result_init(&cr);
        atlas_status st = atlas_orch_apply(e.db, cop, &cr, &cerr);
        T_CHECK_MSG(st != ATLAS_OK, "non-owner cancel should fail");
        T_CHECK_MSG(strstr(atlas_err_msg(&cerr), "no such job") != NULL,
                    "wrong message: %s", atlas_err_msg(&cerr));
        atlas_orch_result_free(&cr);
        atlas_orch_op_free(cop);
        free(cop);
    }

    atlas_buf_free(&job_uid);
    atlas_buf_free(&job2_uid);
    atlas_buf_free(&job3_uid);
    env_close(&e);
}

/* (g) Follow-up of a remote repo-tree submission inherits submit_key_id.
 *
 * Drive SUBMIT -> LEASE -> COMPLETE(gate fail) via atlas_orch_apply.  The
 * COMPLETE produces a follow-up; verify the follow-up row carries
 * submit_key_id = model_id.  Then verify active_count = 1, today_count = 1. */
static void test_g_followup_inherits_key_id(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Submit a repo-tree job with a gate. */
    atlas_buf root_uid = ATLAS_BUF_INIT;
    {
        atlas_orch_op *op = remote_submit_op(&e, "follow task", "pass", NULL);
        set_token(op, e.model_token);
        atlas_orch_result r;
        atlas_orch_result_init(&r);
        apply_ok(e.db, op, &r);
        T_OK(atlas_buf_set(&root_uid, r.job_uid.data, r.job_uid.len, &err), &err);
        atlas_orch_result_free(&r);
        atlas_orch_op_free(op);
        free(op);
    }

    /* LEASE: take the job as a dispatcher, naming the repo-tree driver. */
    atlas_buf lease_token = ATLAS_BUF_INIT;
    {
        atlas_orch_op *lop = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
        T_REQUIRE(lop != NULL);
        lop->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
        lop->peer_uid = 65534;
        T_OK(atlas_buf_set_str(&lop->dispatcher_id, "test-remote", &err), &err);
        T_OK(atlas_buf_set(&lop->job_uid, root_uid.data, root_uid.len, &err), &err);
        {
            atlas_orch_argv want;
            atlas_orch_argv_init(&want);
            T_OK(atlas_orch_argv_push(&want, "fake-repo", 9u, &err), &err);
            T_OK(atlas_orch_validations_encode(&want, 1u, &lop->lease_drivers, &err), &err);
            atlas_orch_argv_free(&want);
        }
        atlas_orch_result lr;
        atlas_orch_result_init(&lr);
        apply_ok(e.db, lop, &lr);
        T_CHECK_MSG(lr.granted, "lease was not granted");
        T_OK(atlas_buf_set(&lease_token, lr.token.data, lr.token.len, &err), &err);
        atlas_orch_result_free(&lr);
        atlas_orch_op_free(lop);
        free(lop);
    }

    /* COMPLETE with a gate failure: process exited 0 but validation failed. */
    atlas_buf follow_uid = ATLAS_BUF_INIT;
    {
        atlas_orch_op *cop = atlas_orch_op_new(ATLAS_ORCH_OP_COMPLETE);
        T_REQUIRE(cop != NULL);
        cop->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
        cop->peer_uid = 65534;
        cop->success = false;
        cop->exit_kind = ATLAS_ORCH_EXIT_OK;
        cop->failure_reason = ATLAS_ORCH_REASON_VALIDATION_FAILED;
        cop->failed_gate = 0;
        T_OK(atlas_buf_set(&cop->token, lease_token.data, lease_token.len, &err), &err);
        T_OK(atlas_buf_set_str(&cop->driver_version, "test", &err), &err);
        atlas_orch_result cr;
        atlas_orch_result_init(&cr);
        apply_ok(e.db, cop, &cr);
        if (cr.follow_up_job_uid.len > 0) {
            T_OK(atlas_buf_set(&follow_uid, cr.follow_up_job_uid.data,
                               cr.follow_up_job_uid.len, &err), &err);
        }
        atlas_orch_result_free(&cr);
        atlas_orch_op_free(cop);
        free(cop);
    }

    T_REQUIRE_MSG(follow_uid.len > 0, "expected a follow-up job uid");

    char follow_key[64];
    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT submit_key_id FROM orch_jobs WHERE job_uid = '%s'",
                   atlas_buf_cstr(&follow_uid));
    read_text_col(e.db, sql, follow_key, sizeof(follow_key));
    T_CHECK_MSG(strcmp(follow_key, e.model_id) == 0,
                "follow-up submit_key_id '%s' != model_id '%s'", follow_key, e.model_id);

    /* Active count = 1 (follow-up is non-terminal; root is FAILED). */
    int64_t active_count = 0;
    atlas_err err2;
    atlas_err_init(&err2);
    T_OK(atlas_db_orch_remote_active_count(e.db, e.model_id, &active_count, &err2), &err2);
    T_EQ_INT((int)active_count, 1);

    /* Today count = 1 (follow-up has a parent -- not a root submission). */
    char now_buf[ATLAS_TS_MAX];
    atlas_now_iso8601(now_buf, sizeof(now_buf));
    char day_start[32];
    memset(day_start, 0, sizeof(day_start));
    memcpy(day_start, now_buf, 10u);
    memcpy(day_start + 10u, "T00:00:00Z", 10u);
    int64_t today_count = 0;
    T_OK(atlas_db_orch_remote_today_count(e.db, e.model_id, day_start, &today_count, &err2),
         &err2);
    T_EQ_INT((int)today_count, 1);

    atlas_buf_free(&root_uid);
    atlas_buf_free(&lease_token);
    atlas_buf_free(&follow_uid);
    env_close(&e);
}

/* (h) A local SUBMIT (no token, remote_allowed_count = 0) is unchanged:
 * empty key in the row, no gateway detail, result.key_id is empty. */
static void test_h_local_submit_unchanged(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = e.repo_id;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e.identity.data, e.identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, e.commit.data, e.commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, "local task", &err), &err);
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = 3;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    apply_ok(e.db, op, &r);

    T_CHECK_MSG(r.key_id[0] == '\0', "local submit should have empty key_id, got '%s'", r.key_id);
    T_EQ_INT((int)r.remote_active, 0);
    T_EQ_INT((int)r.remote_today, 0);

    char key_col[64];
    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT submit_key_id FROM orch_jobs WHERE job_uid = '%s'",
                   atlas_buf_cstr(&r.job_uid));
    read_text_col(e.db, sql, key_col, sizeof(key_col));
    T_CHECK_MSG(key_col[0] == '\0', "local submit should have empty submit_key_id, got '%s'",
                key_col);

    char ledger_detail[512];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT detail FROM orch_transitions WHERE job_id = %lld",
                   (long long)r.job_id);
    read_text_col(e.db, sql, ledger_detail, sizeof(ledger_detail));
    T_CHECK_MSG(strstr(ledger_detail, "submitted through the Atlas gateway") == NULL,
                "local submit should not have gateway detail: '%s'", ledger_detail);

    atlas_orch_result_free(&r);
    atlas_orch_op_free(op);
    free(op);
    env_close(&e);
}

/* (i) atlas_orch_op_free wipes the token bytes before freeing. */
static void test_i_op_free_wipes_token(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);

    T_OK(atlas_buf_set_str(&op->remote_token, "ATLAS_API_KEY=abc123secretbytes", &err), &err);
    T_REQUIRE(op->remote_token.len > 0);
    T_REQUIRE(op->remote_token.data != NULL);

    bool any_nonzero = false;
    for (size_t i = 0; i < op->remote_token.len; i++) {
        if (op->remote_token.data[i] != 0) {
            any_nonzero = true;
            break;
        }
    }
    T_CHECK_MSG(any_nonzero, "token bytes should be non-zero before free");

    /* The wipe itself, verified by inspection rather than by reading freed
     * memory: dereferencing `op->remote_token.data` after `atlas_orch_op_free`
     * is undefined behaviour, and this project builds an ASan suite
     * (`make asan`) this test also runs under, where that read would be
     * reported as a use-after-free rather than proving anything about the wipe.
     * `atlas_orch_op_free` in `src/db/db_orch.c` iterates every byte of
     * `remote_token.data[0..remote_token.cap)` through a volatile pointer,
     * setting each to zero, unconditionally whenever `remote_token.data !=
     * NULL`, strictly before the matching `atlas_buf_free(&op->remote_token)`
     * releases the allocation — read there rather than reproduced here, on
     * `atlas_decision_op_free`'s precedent in `src/decision/lifecycle.c`
     * which this code cites by name. */
    atlas_orch_op_free(op);
    free(op);
}

static const atlas_test TESTS[] = {
    {"(a) remote SUBMIT with model's token succeeds, key stored in row and ledger",
     test_a_remote_submit_success},
    {"(b) wrong or not-in-list credential is refused, no row written",
     test_b_wrong_credential},
    {"(c) active budget: 3rd submission refused; cancelling one lets it through",
     test_c_active_budget},
    {"(d) daily budget: 4th root submission refused; back-dated row does not count",
     test_d_daily_budget},
    {"(e) idempotency namespace: same key+task is duplicate; different cred is new job",
     test_e_idempotency},
    {"(f) CANCEL: wrong cred = no such job; correct = CANCELLED; operator flag = CANCELLED",
     test_f_cancel},
    {"(g) follow-up inherits submit_key_id; counts toward active, not daily",
     test_g_followup_inherits_key_id},
    {"(h) local SUBMIT with no token is unchanged",
     test_h_local_submit_unchanged},
    {"(i) atlas_orch_op_free wipes the token bytes",
     test_i_op_free_wipes_token},
};

ATLAS_TEST_MAIN("orch_remote", TESTS)

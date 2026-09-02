/* Atlas - A12.1 T12: the Canonical Context Pack.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_memory_pack_build`/`_freeze_in_tx`/`_freshness`/`_render`/`_compose`,
 * against a real git repository and a real database. The plan's six cases:
 *
 *   (a) determinism -- two builds over an unchanged database agree byte for
 *       byte on `rendered` and on `pack_digest`.
 *   (b) freshness -- each of the six pinned inputs moved in turn, asserted
 *       separately, `which_moved` naming exactly the one that moved.
 *   (c) a relevant claim carrying a stored CONTRADICTION/IMPLEMENTATION
 *       conflict renders CONTEXT_CONFLICT; an irrelevant stale claim is
 *       excluded and counted, and the pack's status is untouched by it.
 *   (d) relevance is never recency: an older, overlapping claim is selected
 *       over a newer, non-overlapping one.
 *   (e) freeze: a second freeze for one run_uid is refused by the constraint.
 *   (f) compose(task, NULL, NULL, NULL) returns task byte for byte.
 *
 * Plus (g): the claims-count bound is refused, never trimmed.
 *
 * Fixture helpers here are a fresh, small copy of `test_memory_reconcile.c`'s
 * shape (`t8env`/`t8_bind_head`/`t9_propose`/`t9_approve`) rather than a
 * shared header -- this codebase's own practice for per-file test scaffolding.
 * Claims and verification results are seeded directly through typed
 * operations and raw SQL (`atlas_db_verify_claim_insert`, `atlas_db_exec_sql`)
 * rather than through the full extraction/reconciliation pipeline, the way
 * `t8_seed_file`/`t9_update_file_hash` seed `files` rows directly in that
 * file -- this file is about the pack's own five functions, not about T7/T8's
 * extraction.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/memory.h"
#include "atlas/sha256.h"
#include "atlas/syspolicy.h"
#include "atlas/verify.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct pkenv {
    fixture fx;
    atlas_db *db;
    int64_t repo_id;
    atlas_buf db_path;
} pkenv;

static void pk_env_open(pkenv *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_REQUIRE(fx_open(&e->fx, err) == ATLAS_OK);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
    /* An initial commit: `fx_init_repo` only `git init`s, and `pk_bind_head`
     * needs a born HEAD to bind to -- `test_memory_reconcile.c`'s own
     * fixtures always commit something before their first `t8_bind_head`. */
    T_OK(fx_write(fx_repo(&e->fx), "README.md", "seed\n", err), err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), err), err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "initial commit", err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_buf common = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&common, err, "%s/.git", fx_repo(&e->fx)), err);
    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = fx_repo(&e->fx);
    id.root_len = strlen(id.root);
    id.common_dir = atlas_buf_cstr(&common);
    id.common_dir_len = common.len;
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
    atlas_buf_free(&common);
}

static void pk_env_close(pkenv *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

static void pk_head(pkenv *e, atlas_buf *out, atlas_err *err) {
    const char *args[] = {"rev-parse", "HEAD"};
    int exit_code = -1;
    atlas_buf stdout_buf = ATLAS_BUF_INIT;
    T_OK(fx_git(&e->fx, fx_repo(&e->fx), args, 2, &exit_code, &stdout_buf, err), err);
    T_REQUIRE_MSG(exit_code == 0, "git rev-parse HEAD failed with exit code %d", exit_code);
    /* Trim the trailing newline `git rev-parse` always emits. */
    while (stdout_buf.len > 0 &&
          (stdout_buf.data[stdout_buf.len - 1] == '\n' || stdout_buf.data[stdout_buf.len - 1] == '\r')) {
        stdout_buf.len--;
    }
    T_OK(atlas_buf_set(out, stdout_buf.data, stdout_buf.len, err), err);
    atlas_buf_free(&stdout_buf);
}

/* Binds `repositories.scanned_head` to the real repository's current HEAD and
 * records that oid as a root commit -- `test_memory_reconcile.c`'s
 * `t8_bind_head` shape. */
static void pk_bind_head(pkenv *e, atlas_err *err) {
    atlas_buf head = ATLAS_BUF_INIT;
    pk_head(e, &head, err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO commits(repo_id, oid, parent_count) VALUES(%lld, '%s', 0);"
                           "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
                           (long long)e->repo_id, atlas_buf_cstr(&head), atlas_buf_cstr(&head),
                           (long long)e->repo_id),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
    atlas_buf_free(&head);
}

/* Moves `repositories.scanned_head` to the real repository's current HEAD
 * *without* recording a new root commit -- unlike `pk_bind_head`, which
 * always inserts with `parent_count = 0` (right for the very first commit,
 * wrong for any commit after it, which has a parent). A test isolating the
 * `scanned_head` pin alone needs `atlas_db_repo_identity_hash`'s root-commit
 * set to stay exactly what it was, which is why this exists as its own
 * helper rather than a second call to `pk_bind_head`. */
static void pk_advance_scanned_head(pkenv *e, atlas_err *err) {
    atlas_buf head = ATLAS_BUF_INIT;
    pk_head(e, &head, err);
    char sql[256];
    (void)snprintf(sql, sizeof sql, "UPDATE repositories SET scanned_head = '%s' WHERE id = %lld;",
                  atlas_buf_cstr(&head), (long long)e->repo_id);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
    atlas_buf_free(&head);
}

/* A second root commit -- moves `atlas_db_repo_identity_hash`'s sorted set of
 * ingested root commits without touching `scanned_head`. The context file's
 * "hand-editing the stored repository row" fixture pattern for a moved
 * lineage, applied here through the `commits` table that hash is built from. */
static void pk_add_root_commit(pkenv *e, const char *oid, atlas_err *err) {
    char sql[256];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO commits(repo_id, oid, parent_count) VALUES(%lld, '%s', 0);",
                  (long long)e->repo_id, oid);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

static void pk_set_dirty(pkenv *e, bool dirty, atlas_err *err) {
    char sql[128];
    (void)snprintf(sql, sizeof sql, "UPDATE repositories SET dirty = %d WHERE id = %lld;",
                  dirty ? 1 : 0, (long long)e->repo_id);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

/* A tracked `.c` file's content hash -- what `atlas_sem_source_identity`
 * folds into the live source identity (`language IN ('c','c-header')`,
 * `src/db/db_sem.c`). */
static void pk_seed_c_file(pkenv *e, const char *path, const char *hash, atlas_err *err) {
    char sql[768];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO scans(repo_id, started_at, status)"
                  "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                  "INSERT INTO files(repo_id, path_raw, path_text, file_type, language,"
                  "  content_hash, size_bytes, deleted, first_seen_scan_id, last_seen_scan_id,"
                  "  first_seen_at, last_seen_at)"
                  "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular', 'c', '%s', 64, 0,"
                  "         last_insert_rowid(), last_insert_rowid(),"
                  "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');",
                  (long long)e->repo_id, (long long)e->repo_id, path, path, hash);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

static void pk_update_file_hash(pkenv *e, const char *path_text, const char *new_hash,
                                atlas_err *err) {
    char sql[512];
    (void)snprintf(sql, sizeof sql,
                  "UPDATE files SET content_hash = '%s' WHERE repo_id = %lld AND path_text = '%s';",
                  new_hash, (long long)e->repo_id, path_text);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

static void pk_policy(atlas_syspolicy *pol, atlas_memory_source_class cls, const char *const *paths,
                      size_t n) {
    memset(pol, 0, sizeof *pol);
    pol->state = ATLAS_SYSPOLICY_SYSTEM;
    pol->memory_source_count = n;
    for (size_t i = 0; i < n; i++) {
        pol->memory_sources[i].cls = cls;
        pol->memory_sources[i].repo_name[0] = '\0';
        (void)snprintf(pol->memory_sources[i].path, sizeof pol->memory_sources[i].path, "%s",
                       paths[i]);
    }
}

static void pk_op_repo(atlas_decision_op *op, atlas_err *err) {
    T_OK(atlas_buf_set_str(&op->repo_name, "proj", err), err);
}

static void pk_propose(pkenv *e, const char *title, atlas_buf *uid_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    pk_op_repo(&op, err);
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a body for the T12 fixture", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

static void pk_revise(pkenv *e, const char *uid, const char *text, int64_t *revision_no_out,
                      atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
    pk_op_repo(&op, err);
    T_OK(atlas_buf_set_str(&op.uid, uid, err), err);
    T_OK(atlas_buf_set_str(&op.revision.title, "T12 fixture", err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, text, err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    *revision_no_out = res.revision_no;
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

static void pk_approve(pkenv *e, const char *uid, int64_t revision_no, atlas_err *err) {
    atlas_decision_op cop;
    atlas_decision_op_init(&cop, ATLAS_DECISION_OP_CHALLENGE);
    pk_op_repo(&cop, err);
    T_OK(atlas_buf_set_str(&cop.uid, uid, err), err);
    cop.expect_revision_no = revision_no;
    cop.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cres;
    atlas_decision_result_init(&cres);
    T_OK(atlas_decision_apply(e->db, &cop, &cres, err), err);

    atlas_decision_op aop;
    atlas_decision_op_init(&aop, ATLAS_DECISION_OP_APPROVE);
    pk_op_repo(&aop, err);
    T_OK(atlas_buf_set_str(&aop.uid, uid, err), err);
    T_OK(atlas_buf_set(&aop.token, cres.token.data, cres.token.len, err), err);
    T_OK(atlas_buf_set_str(&aop.confirmation, cres.confirm, err), err);
    atlas_decision_result ares;
    atlas_decision_result_init(&ares);
    T_OK(atlas_decision_apply(e->db, &aop, &ares, err), err);
    T_CHECK_MSG(ares.state == ATLAS_DECISION_APPROVED, "the approval did not land");
    atlas_decision_result_free(&ares);
    atlas_decision_op_free(&aop);
    atlas_decision_result_free(&cres);
    atlas_decision_op_free(&cop);
}

/* A claim, inserted directly rather than through
 * `atlas_verify_intake_apply_in_tx` -- this file is testing the pack's own
 * five functions, not T8's extraction pipeline, and a test may reach into the
 * database the way `t8_seed_file` already does in `test_memory_reconcile.c`. */
static void pk_claim(pkenv *e, const char *text, atlas_buf *uid_out, int64_t *id_out,
                     atlas_err *err) {
    atlas_verify_claim c;
    atlas_verify_claim_init(&c);
    c.repo_id = e->repo_id;
    T_OK(atlas_buf_set_str(&c.text, text, err), err);
    T_OK(atlas_buf_set_str(&c.domain, "test", err), err);
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    T_OK(atlas_db_verify_claim_insert(e->db, &c, now, err), err);
    *id_out = c.id;
    T_OK(atlas_buf_set(uid_out, c.uid.data, c.uid.len, err), err);
    atlas_verify_claim_free(&c);
}

static void pk_anchor(pkenv *e, const char *claim_uid, atlas_memory_anchor_kind kind,
                      const char *value, atlas_err *err) {
    T_OK(atlas_db_memory_anchor_add(e->db, e->repo_id, claim_uid, kind, value, err), err);
}

/* A `verify_results` row, by raw SQL -- deliberately not through
 * `atlas_verify_assess`/`atlas_verify_autolifecycle_run`, both of which are
 * live (a root-owned policy, the clock) and would make this fixture
 * non-reproducible. `state` and `conflict` are the two columns Decision 8's
 * pack flags a claim on. */
static void pk_result(pkenv *e, int64_t claim_id, const char *state, const char *conflict,
                      atlas_err *err) {
    char sql[512];
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO verify_results(claim_id, state, basis, confidence_score,"
                  "  calibration, algorithm, family_version, conflict, stale, created_at)"
                  " VALUES(%lld, '%s', 'DETERMINISTIC', 0, 'INSUFFICIENT_DATA', 'test', 1, '%s',"
                  "  %d, '%s');",
                  (long long)claim_id, state, conflict, strcmp(state, "STALE") == 0 ? 1 : 0, now);
    T_OK(atlas_db_exec_sql(e->db, sql, err), err);
}

static void pk_build(pkenv *e, const atlas_syspolicy *pol, const char *task, atlas_memory_pack *out,
                     atlas_err *err) {
    T_OK(atlas_memory_pack_build(e->db, e->repo_id, pol, task, out, err), err);
}

/* --- (a) determinism -------------------------------------------------------- */

static void test_build_is_deterministic(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_buf u1 = ATLAS_BUF_INIT, u2 = ATLAS_BUF_INIT, u3 = ATLAS_BUF_INIT;
    int64_t i1 = 0, i2 = 0, i3 = 0;
    pk_claim(&e, "the widget subsystem validates checksums before writing", &u1, &i1, &err);
    pk_claim(&e, "unrelated prose about nothing in the task", &u2, &i2, &err);
    pk_claim(&e, "the widget subsystem also logs every checksum failure", &u3, &i3, &err);
    (void)i2;

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p1, p2;
    atlas_memory_pack_init(&p1);
    atlas_memory_pack_init(&p2);
    pk_build(&e, &pol, "fix the widget subsystem checksum bug", &p1, &err);
    pk_build(&e, &pol, "fix the widget subsystem checksum bug", &p2, &err);

    T_CHECK_MSG(p1.rendered.len == p2.rendered.len && p1.rendered.len > 0,
               "two builds over an unchanged database produced different-length rendered bodies");
    T_CHECK_MSG(memcmp(p1.rendered.data, p2.rendered.data, p1.rendered.len) == 0,
               "two builds over an unchanged database produced different rendered bytes");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&p1.pack_digest), atlas_buf_cstr(&p2.pack_digest)) == 0,
               "two builds over an unchanged database produced different digests");
    T_CHECK_MSG(p1.pack_digest.len == ATLAS_SHA256_HEX_LEN, "the digest is not a sha256 hex string");
    T_CHECK_MSG(p1.claim_count == 2, "expected exactly the two overlapping claims selected, got %lld",
               (long long)p1.claim_count);

    atlas_memory_pack_free(&p1);
    atlas_memory_pack_free(&p2);
    atlas_buf_free(&u1);
    atlas_buf_free(&u2);
    atlas_buf_free(&u3);
    pk_env_close(&e);
}

/* --- (b) freshness: six pins, six separate assertions ----------------------- */

static void expect_stale(atlas_memory_pack_status status, const atlas_buf *which, const char *name) {
    T_CHECK_MSG(status == ATLAS_MEMORY_PACK_STALE, "expected STALE, got %d", (int)status);
    char want[64];
    (void)snprintf(want, sizeof want, "STALE:%s", name);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(which), want) == 0, "expected which_moved=%s, got %s", want,
               atlas_buf_cstr(which));
}

static void test_freshness_commit_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);

    T_OK(fx_write(fx_repo(&e.fx), "again.txt", "more\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "second commit", &err), &err);
    pk_advance_scanned_head(&e, &err);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    expect_stale(status, &which, "COMMIT");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

static void test_freshness_source_identity_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);
    pk_seed_c_file(&e, "src/thing.c",
                   "1111111111111111111111111111111111111111111111111111111111111111", &err);
    pk_set_dirty(&e, true, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);
    T_CHECK_MSG(p.source_identity.len > 0, "a dirty repository must pin a non-empty source identity");

    pk_update_file_hash(&e, "src/thing.c",
                        "2222222222222222222222222222222222222222222222222222222222222222", &err);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    expect_stale(status, &which, "SOURCE_IDENTITY");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

static void test_freshness_decision_set_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_buf doc_uid = ATLAS_BUF_INIT;
    pk_propose(&e, "a decision the fixture cites", &doc_uid, &err);
    pk_approve(&e, atlas_buf_cstr(&doc_uid), 1, &err);

    atlas_buf claim_uid = ATLAS_BUF_INIT;
    int64_t claim_id = 0;
    pk_claim(&e, "this cites the approved decision", &claim_uid, &claim_id, &err);
    pk_anchor(&e, atlas_buf_cstr(&claim_uid), ATLAS_MEMORY_ANCHOR_DECISION, atlas_buf_cstr(&doc_uid),
             &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);

    int64_t rev2 = 0;
    pk_revise(&e, atlas_buf_cstr(&doc_uid), "a revised body", &rev2, &err);
    pk_approve(&e, atlas_buf_cstr(&doc_uid), rev2, &err);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    expect_stale(status, &which, "DECISION_SET");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    atlas_buf_free(&claim_uid);
    atlas_buf_free(&doc_uid);
    pk_env_close(&e);
}

static void test_freshness_generation_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);

    /* A second generation row, sharing the *exact* live decision-set and
     * source-set digests build() itself would compute right now -- so this
     * moves memory_generation alone, the way a COMMIT-caused pass that
     * changed neither a source nor a decision's approved revision would. */
    char dd[ATLAS_SHA256_HEX_LEN + 1], sd[ATLAS_SHA256_HEX_LEN + 1];
    T_OK(atlas_memory_decision_set_digest(e.db, e.repo_id, dd, &err), &err);
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    bool found = false;
    T_OK(atlas_db_repo_get_by_id(e.db, e.repo_id, &repo, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_memory_source_set_digest(e.db, &repo, &pol, sd, &err), &err);
    int64_t next_gen = 0;
    T_OK(atlas_db_memory_generation_next(e.db, e.repo_id, &next_gen, &err), &err);
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    int64_t gen_id = 0;
    T_OK(atlas_db_memory_generation_insert(e.db, e.repo_id, next_gen, ATLAS_MEMORY_CAUSE_COMMIT,
                                          atlas_buf_cstr(&p.repo_identity_hash), repo.scanned_head,
                                          dd, sd, 0, now, &gen_id, &err),
        &err);
    atlas_repo_info_free(&repo);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    expect_stale(status, &which, "GENERATION");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

static void test_freshness_source_set_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_syspolicy pol_before;
    memset(&pol_before, 0, sizeof pol_before);
    pol_before.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol_before, "a task", &p, &err);

    /* "A source registration": the operator's policy now names one more
     * memory source than it did when the pack was built. */
    const char *paths[] = {"NOTES.md"};
    atlas_syspolicy pol_after;
    pk_policy(&pol_after, ATLAS_MEMORY_SOURCE_REPO_FILE, paths, 1);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol_after, &p, &status, &which, &err), &err);
    expect_stale(status, &which, "SOURCE_SET");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

static void test_freshness_repo_identity_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);
    T_CHECK_MSG(p.repo_identity_hash.len > 0, "a repository with a bound root commit must have a "
                                             "non-empty identity hash");

    pk_add_root_commit(&e, "fedcba9876543210fedcba9876543210fedcba9", &err);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    expect_stale(status, &which, "REPO_IDENTITY");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

static void test_freshness_current_when_nothing_moved(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    T_CHECK_MSG(status == ATLAS_MEMORY_PACK_CURRENT, "expected CURRENT, got %d", (int)status);
    T_CHECK_MSG(which.len == 0, "which_moved must be empty when nothing moved");

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

/* --- (c) conflict flagging and irrelevant-stale exclusion ------------------- */

static void test_conflict_flags_and_irrelevant_stale_excluded(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_buf relevant_uid = ATLAS_BUF_INIT, irrelevant_uid = ATLAS_BUF_INIT,
             irrelevant_healthy_uid = ATLAS_BUF_INIT;
    int64_t relevant_id = 0, irrelevant_id = 0, irrelevant_healthy_id = 0;
    pk_claim(&e, "the payment gateway retries on timeout", &relevant_uid, &relevant_id, &err);
    pk_result(&e, relevant_id, "VERIFIED", "IMPLEMENTATION", &err);
    pk_claim(&e, "completely unrelated prose naming nothing in the task", &irrelevant_uid,
            &irrelevant_id, &err);
    pk_result(&e, irrelevant_id, "STALE", "NONE", &err);
    /* A second irrelevant claim, but a *healthy* one (VERIFIED, no conflict)
     * -- without it, `excluded_count == 1` cannot tell a correct
     * troubled-only count from a broken read that treats every irrelevant
     * claim as troubled (`atlas_db_verify_result_latest` returning `found =
     * false` unconditionally would still pass a check that only ever sees
     * one irrelevant claim). This one must never be counted. */
    pk_claim(&e, "orthogonal notes about something else altogether outside scope",
            &irrelevant_healthy_uid, &irrelevant_healthy_id, &err);
    pk_result(&e, irrelevant_healthy_id, "VERIFIED", "NONE", &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "fix the payment gateway retry timeout handling", &p, &err);

    T_CHECK_MSG(p.claim_count == 1, "expected exactly one relevant claim, got %lld",
               (long long)p.claim_count);
    T_CHECK_MSG(p.excluded_count == 1, "expected the irrelevant stale claim counted as excluded, "
                                      "got %lld",
               (long long)p.excluded_count);
    /* The fixed preamble's own disclaimer prose names "CONTEXT_CONFLICT" as a
     * word, so a bare `strstr` for it would pass whether or not any claim was
     * actually tagged -- caught by this test's own self-check (disclosed in
     * the T12 report). The tag `atlas_memory_pack_build` actually appends is
     * `" [CONTEXT_CONFLICT]"`, right after the claim's own text, which the
     * preamble cannot contain. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&p.rendered), "payment gateway retries on timeout "
                                                   "[CONTEXT_CONFLICT]") != NULL,
               "expected the IMPLEMENTATION-conflicted relevant claim to render tagged "
               "[CONTEXT_CONFLICT]:\n%s",
               atlas_buf_cstr(&p.rendered));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&p.rendered), "unrelated prose") == NULL,
               "the irrelevant stale claim must not be rendered at all");

    atlas_memory_pack_status status = ATLAS_MEMORY_PACK_UNKNOWN;
    atlas_buf which = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_freshness(e.db, &pol, &p, &status, &which, &err), &err);
    T_CHECK_MSG(status == ATLAS_MEMORY_PACK_CURRENT,
               "the excluded irrelevant claim must not itself make the pack's own status STALE, "
               "got %d",
               (int)status);

    atlas_buf_free(&which);
    atlas_memory_pack_free(&p);
    atlas_buf_free(&relevant_uid);
    atlas_buf_free(&irrelevant_uid);
    atlas_buf_free(&irrelevant_healthy_uid);
    pk_env_close(&e);
}

/* --- (d) relevance is never recency ------------------------------------------ */

static void test_relevance_never_recency(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    /* Older (lower claim id, inserted first) and overlapping. */
    atlas_buf older_uid = ATLAS_BUF_INIT;
    int64_t older_id = 0;
    pk_claim(&e, "the cache eviction policy uses LRU ordering", &older_uid, &older_id, &err);

    /* Newer (higher claim id, inserted second) and sharing no token with the
     * task at all. */
    atlas_buf newer_uid = ATLAS_BUF_INIT;
    int64_t newer_id = 0;
    pk_claim(&e, "totally different subject matter about something else entirely", &newer_uid,
            &newer_id, &err);
    T_CHECK_MSG(newer_id > older_id, "the fixture's own ordering assumption failed: %lld vs %lld",
               (long long)newer_id, (long long)older_id);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "investigate the cache eviction LRU ordering bug", &p, &err);

    T_CHECK_MSG(p.claim_count == 1, "expected exactly the older overlapping claim selected, got %lld",
               (long long)p.claim_count);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&p.rendered), "cache eviction") != NULL,
               "the older, overlapping claim must be included:\n%s", atlas_buf_cstr(&p.rendered));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&p.rendered), "totally different") == NULL,
               "the newer, non-overlapping claim must not be included:\n%s",
               atlas_buf_cstr(&p.rendered));

    atlas_memory_pack_free(&p);
    atlas_buf_free(&older_uid);
    atlas_buf_free(&newer_uid);
    pk_env_close(&e);
}

/* --- (e) freeze: a second freeze for one run_uid is refused ----------------- */

static void test_freeze_unique_run_uid(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    pk_build(&e, &pol, "a task", &p, &err);

    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_memory_pack_freeze_in_tx(e.db, "jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &p, &err), &err);
    T_OK(atlas_db_commit(e.db, &err), &err);

    T_OK(atlas_db_begin(e.db, &err), &err);
    atlas_status second = atlas_memory_pack_freeze_in_tx(e.db, "jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &p,
                                                        &err);
    T_CHECK_MSG(second != ATLAS_OK, "a second freeze for the same run_uid must be refused");
    atlas_db_rollback(e.db);

    bool found = false;
    atlas_memory_pack readback;
    atlas_memory_pack_init(&readback);
    T_OK(atlas_db_memory_pack_get(e.db, "jaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &readback, &found, &err),
        &err);
    T_CHECK_MSG(found, "the first freeze must be readable back");
    T_CHECK_MSG(readback.pack_digest.len == p.pack_digest.len &&
                   memcmp(readback.pack_digest.data, p.pack_digest.data, p.pack_digest.len) == 0,
               "the read-back pack's digest does not match what was frozen");
    T_CHECK_MSG(readback.rendered.len == p.rendered.len &&
                   memcmp(readback.rendered.data, p.rendered.data, p.rendered.len) == 0,
               "the read-back pack's rendered body does not match what was frozen");

    atlas_memory_pack_free(&readback);
    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

/* --- (f) compose appends nothing for an absent piece ------------------------ */

static void test_compose_with_nothing_is_byte_identical(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_compose("do the thing", NULL, NULL, NULL, &out, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&out), "do the thing") == 0,
               "compose with every optional piece absent must return the task byte for byte, got: %s",
               atlas_buf_cstr(&out));
    atlas_buf_free(&out);
}

static void test_compose_appends_both_pieces_independently(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out_pkg_only = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_compose("task", "PKG", NULL, NULL, &out_pkg_only, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out_pkg_only), "PKG") != NULL, "the memory package must appear");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out_pkg_only), "status") == NULL,
               "no pack section may appear when pack_body is absent");
    atlas_buf_free(&out_pkg_only);

    atlas_buf out_body_only = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_compose("task", NULL, "CURRENT", "BODY", &out_body_only, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out_body_only), "BODY") != NULL, "the pack body must appear");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out_body_only), "CURRENT") != NULL,
               "the status line must appear alongside a present pack body");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out_body_only), "PKG") == NULL,
               "no memory package section may appear when memory_package is absent");
    atlas_buf_free(&out_body_only);
}

/* --- (g) the claims bound is refused, never trimmed ------------------------- */

static void test_claims_over_bound_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    /* One more claim than the pack may hold, every one sharing the single
     * token "widget" with the task -- cheap: short text, no anchors. */
    for (unsigned i = 0; i < ATLAS_MEMORY_PACK_MAX_CLAIMS + 1u; i++) {
        char text[64];
        (void)snprintf(text, sizeof text, "widget claim number %u", i);
        atlas_buf uid = ATLAS_BUF_INIT;
        int64_t id = 0;
        pk_claim(&e, text, &uid, &id, &err);
        atlas_buf_free(&uid);
    }

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    atlas_status st = atlas_memory_pack_build(e.db, e.repo_id, &pol, "the widget task", &p, &err);
    T_CHECK_MSG(st != ATLAS_OK,
               "more relevant claims than ATLAS_MEMORY_PACK_MAX_CLAIMS must be refused, not "
               "silently trimmed");
    T_CHECK_MSG(p.claim_count == 0 && p.rendered.len == 0,
               "a refused build must leave no partial pack behind");

    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

/* (g), the second half: within `ATLAS_MEMORY_PACK_MAX_CLAIMS` but over
 * `ATLAS_MEMORY_PACK_MAX_BYTES` once rendered -- the same discipline, the
 * other bound. `advisor`-flagged residual, closed: the plan's own six cases
 * do not name a byte-bound test, and an earlier draft of this report left it
 * disclosed as untested rather than adding this. */
static void test_pack_over_byte_bound_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    pkenv e;
    pk_env_open(&e, &err);
    pk_bind_head(&e, &err);

    /* `ATLAS_MEMORY_PACK_MAX_CLAIMS` claims, each long enough that the
     * rendered total clears `ATLAS_MEMORY_PACK_MAX_BYTES` (64 KiB) -- roughly
     * 1200 bytes apiece times 64 is comfortably over 65536, and each claim
     * stays well under `ATLAS_VERIFY_CLAIM_TEXT_MAX` (4096). Every claim
     * shares the single token "widget" with the task, so every one lands in
     * the relevant set and none is dropped by the claims-count bound above. */
    for (unsigned i = 0; i < ATLAS_MEMORY_PACK_MAX_CLAIMS; i++) {
        char text[1300];
        int n = snprintf(text, sizeof text, "widget claim number %u ", i);
        for (int j = n; j + 1 < (int)sizeof text; j++) {
            text[j] = 'x';
        }
        text[sizeof text - 1] = '\0';
        atlas_buf uid = ATLAS_BUF_INIT;
        int64_t id = 0;
        pk_claim(&e, text, &uid, &id, &err);
        atlas_buf_free(&uid);
    }

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    atlas_status st = atlas_memory_pack_build(e.db, e.repo_id, &pol, "the widget task", &p, &err);
    T_CHECK_MSG(st != ATLAS_OK,
               "a rendered pack over ATLAS_MEMORY_PACK_MAX_BYTES must be refused, not silently "
               "trimmed");
    T_CHECK_MSG(p.claim_count == 0 && p.rendered.len == 0,
               "a refused build must leave no partial pack behind");

    atlas_memory_pack_free(&p);
    pk_env_close(&e);
}

static const atlas_test TESTS[] = {
    {"(a) two builds over an unchanged database agree byte for byte",
     test_build_is_deterministic},
    {"(b1) freshness: the indexed commit moved", test_freshness_commit_moved},
    {"(b2) freshness: the live source identity moved", test_freshness_source_identity_moved},
    {"(b3) freshness: a decision approval moved the decision-set digest",
     test_freshness_decision_set_moved},
    {"(b4) freshness: the memory generation moved alone", test_freshness_generation_moved},
    {"(b5) freshness: a source registration moved the source-set digest",
     test_freshness_source_set_moved},
    {"(b6) freshness: the repository identity moved", test_freshness_repo_identity_moved},
    {"(b) freshness: CURRENT and which_moved empty when nothing moved",
     test_freshness_current_when_nothing_moved},
    {"(c) a stored CONTRADICTION/IMPLEMENTATION conflict renders CONTEXT_CONFLICT; an "
     "irrelevant stale claim is excluded, counted, and does not move the pack's own status",
     test_conflict_flags_and_irrelevant_stale_excluded},
    {"(d) relevance is never recency", test_relevance_never_recency},
    {"(e) a second freeze for one run_uid is refused by the constraint",
     test_freeze_unique_run_uid},
    {"(f) compose(task, NULL, NULL, NULL) returns task byte-identical",
     test_compose_with_nothing_is_byte_identical},
    {"(f) compose appends the memory package and the pack body independently",
     test_compose_appends_both_pieces_independently},
    {"(g) more relevant claims than ATLAS_MEMORY_PACK_MAX_CLAIMS is refused, not trimmed",
     test_claims_over_bound_is_refused},
    {"(g) a rendered pack over ATLAS_MEMORY_PACK_MAX_BYTES is refused, not trimmed",
     test_pack_over_byte_bound_is_refused},
};

ATLAS_TEST_MAIN("memory_pack", TESTS)

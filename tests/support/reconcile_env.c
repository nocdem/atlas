/* Atlas - the T8/T11 reconciliation-pass fixture, shared between
 * test_memory_reconcile.c and test_memory_reconcile_live.c.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See support/reconcile_env.h for why this is hoisted rather than copied.
 * Every function here is moved verbatim out of test_memory_reconcile.c (T8's
 * and T11's own comments travel with them); nothing about their behaviour
 * changed, only their reachability.
 */
#define _GNU_SOURCE 1

#include "support/reconcile_env.h"

#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "atlas/datadir.h"
#include "atlas/memory.h"
#include "atlas_test.h"
#include "db/db_internal.h"

/* Used only by t8_bind_head, immediately below -- not exposed, since nothing
 * else in either test binary calls it directly. */
static void t8_head(t8env *e, atlas_buf *out, atlas_err *err) {
    const char *rev[] = {"rev-parse", "HEAD"};
    atlas_buf raw = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_git(&e->fx, fx_repo(&e->fx), rev, 2u, &code, &raw, err), err);
    T_REQUIRE(code == 0);
    size_t n = raw.len;
    while (n > 0 && (raw.data[n - 1u] == '\n' || raw.data[n - 1u] == '\r')) {
        n--;
    }
    T_OK(atlas_buf_set(out, raw.data, n, err), err);
    atlas_buf_free(&raw);
    T_REQUIRE(out->len > 0);
}

/* `test_verify_intake.c`'s own seed_file, reused: a row in `files` is what
 * lets a backtick token resolve as a PATH anchor and what lets a memory
 * source's own path pass EVIDENCE_ADD's index lookup. The hash is never
 * checked against real content anywhere T8's own pass runs (T8 never asks
 * `atlas.content_hash` to verify), so an arbitrary hex string is honest.
 *
 * T9 fix-round-3 (C3, door 4): `size_bytes` is now set to a small,
 * comfortably-in-bound value rather than left unset (NULL). Round 3 added
 * `size_bytes IS NOT NULL AND size_bytes <= ?4` to
 * `atlas_db_memory_dir_hash_mismatch`'s own predicate list -- a row this
 * function seeds is meant to be a `read.c`-ingestible `.md` child, and a
 * NULL `size_bytes` no longer reads as one. Every existing caller of this
 * helper that exercises `atlas_memory_plan_for`'s SOURCE_REVISION path
 * depends on the row it seeds being seen as a real, ingestible file. */
void t8_seed_file(t8env *e, const char *path, const char *hash, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO scans(repo_id, started_at, status)"
                           "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, size_bytes, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular', '%s', 128,"
                           "         last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');",
                           (long long)e->repo_id, (long long)e->repo_id, path, path, hash),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

void t8_env_open(t8env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_repo_info_init(&e->repo);
    atlas_buf_init(&e->db_path);
    T_REQUIRE(fx_open(&e->fx, err) == ATLAS_OK);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
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

/* Binds the repository's own `scanned_head` (and a matching `commits` row) to
 * the real repository's current HEAD, then refetches `e->repo` so the struct
 * the pass reads matches what was just bound -- CLAIM_CREATE's `bind_commit`
 * defaults an empty `basis_commit` to `scanned_head` with no existence check,
 * but EVIDENCE_ADD re-derives its own commit from the claim's (now non-empty)
 * `basis_commit`, which *is* checked against `commits`. Called again after
 * every commit that should move what a fresh pass binds to. */
void t8_bind_head(t8env *e, atlas_err *err) {
    atlas_buf head = ATLAS_BUF_INIT;
    t8_head(e, &head, err);
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

    bool found = false;
    atlas_repo_info_free(&e->repo);
    atlas_repo_info_init(&e->repo);
    T_OK(atlas_db_repo_get(e->db, "proj", &e->repo, &found, err), err);
    T_REQUIRE(found);
}

void t8_env_close(t8env *e) {
    atlas_repo_info_free(&e->repo);
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* Read a scalar off an arbitrary handle, `t8_scalar`'s own shape but not tied
 * to a `t8env` -- needed here because several cases deliberately hold a
 * *readonly* handle (a fresh one, or one opened after the writer closed
 * its own) rather than `t8env`'s writable `e->db`. */
int64_t t11_scalar(atlas_db *db, const char *sql, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(db, sql, &stmt, err), err);
    int step = sqlite3_step(stmt);
    T_REQUIRE_MSG(step == SQLITE_ROW, "scalar query did not yield a row: %s", sql);
    int64_t v = sqlite3_column_int64(stmt, 0);
    atlas_db_finish(db, stmt);
    return v;
}

/* Opens a fresh writer against `e`'s database, logging to `/dev/null`. Every
 * T11 case that drives `atlas_writer_memory_put` needs exactly this, and nine
 * copies of six lines is nine chances for one of them to leak a log fd. */
void t11_writer_open(t8env *e, FILE **log_out, atlas_writer **w_out, atlas_err *err) {
    FILE *log = fopen("/dev/null", "we");
    T_REQUIRE_MSG(log != NULL, "cannot open a log sink");
    atlas_writer *w = NULL;
    T_OK(atlas_writer_start(atlas_buf_cstr(&e->db_path), fx_data_dir(&e->fx), "", NULL, log, &w,
                            err),
         err);
    *log_out = log;
    *w_out = w;
}

void t11_writer_close(FILE *log, atlas_writer *w) {
    atlas_writer_stop(w);
    (void)fclose(log);
}

/* Waits for one memory reconciliation to land a generation, the way a caller
 * would poll memory.status rather than a guessed sleep -- fx_wait_for_
 * substring's own discipline, one layer down at the database instead of the
 * socket. */
bool t11_wait_for_generation(t8env *e, int64_t *gen_out, atlas_err *err) {
    for (int i = 0; i < 250; i++) {
        atlas_db *rdb = NULL;
        if (atlas_db_open_readonly(atlas_buf_cstr(&e->db_path), &rdb, err) == ATLAS_OK) {
            int64_t gen = 0;
            atlas_buf head = ATLAS_BUF_INIT;
            atlas_buf dd = ATLAS_BUF_INIT;
            atlas_buf sd = ATLAS_BUF_INIT;
            bool found = false;
            atlas_status st =
                atlas_db_memory_generation_latest(rdb, e->repo_id, &gen, &head, &dd, &sd, &found,
                                                  err);
            atlas_buf_free(&head);
            atlas_buf_free(&dd);
            atlas_buf_free(&sd);
            atlas_db_close(rdb);
            if (st == ATLAS_OK && found) {
                *gen_out = gen;
                return true;
            }
        }
        usleep(20000);
    }
    return false;
}

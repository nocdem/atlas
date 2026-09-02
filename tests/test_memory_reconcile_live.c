/* Atlas - A12.1 T11: the two memory.* cases that need a real forked daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * T11 originally added both of these to test_memory_reconcile.c and labelled
 * that whole file "integration;daemon" with `RUN_SERIAL TRUE` on their
 * account -- which cost the other fifty-odd, wholly in-process cases in that
 * file both `ctest -LE daemon` and parallelism in every `ctest -j`, for as
 * long as they stayed. Fix round, item 6: split out, so the fast cases pay
 * neither cost and these two pay only what a real daemon fork actually costs.
 *
 *   `test_memory_methods_refuse_a_non_operator_peer` -- a real socket, so the
 *   check exercises `atlas_server_dispatch`'s actual routing rather than
 *   assuming it.
 *
 *   `test_memory_survives_a_daemon_restart` -- a real restart of the daemon
 *   process against the same data directory, which is the one thing an
 *   in-process writer can never stand in for.
 *
 * `t8env` and the T8/T11 fixture helpers (`t8_env_open`, `t8_bind_head`,
 * `t8_seed_file`, `t11_writer_open`, `t11_writer_close`, `t11_scalar`,
 * `t11_wait_for_generation`) are hoisted into `tests/support/reconcile_env.h`
 * rather than copied here, since `test_memory_reconcile.c`'s T8/T9 pass tests
 * need the identical implementation.
 */
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/ipc.h"
#include "atlas/memory.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "db/db_internal.h"
#include "support/fixture.h"
#include "support/reconcile_env.h"

/* Acceptance item: from a peer the root-owned authority policy does not grant
 * -- which every test process in this build tree structurally is, see
 * `test_memory_reconcile.c`'s own T11 section header for why -- all three
 * answer `unknown method`, driven over a real socket against a real forked
 * daemon so the check exercises `atlas_server_dispatch`'s actual routing
 * rather than assuming it. */
static void test_memory_methods_refuse_a_non_operator_peer(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
    T_OK(fx_commit(&fx, fx_repo(&fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&fx), "repo", "add", fx_repo(&fx),
                             "--name", "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_EQ_INT(code, 0);
    }

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    static const char *const METHODS[] = {"memory.put", "memory.status", "memory.reconcile"};
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), METHODS[i], "{\"repo\":\"proj\"}", &resp,
                             &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                    "%s answered something other than unknown for a non-operator peer: %s",
                    METHODS[i], atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* Acceptance item 4, second half. Two versions of one external source are put
 * through the writer, one reconciliation pass turns the latest of them into a
 * claim anchored to a real (seeded) file, and a *fresh daemon process* --
 * `fx_daemon_start` against the same data directory, after this test's own
 * writer has stopped -- reads all of it back: the source, both versions in
 * order and the same generation still present through a fresh database
 * handle, and the claim itself through `verify.claims` / `verify.show` over
 * the real socket, which is the client group and needs no operator standing
 * to reach.
 *
 * `memory.status` itself is not called here -- see this section's own header
 * comment for why an RPC-layer positive call is not available to any test in
 * this tree -- so "memory.status shows..." is satisfied by reading exactly
 * the rows that method would read, through the same typed functions it uses
 * (`atlas_db_memory_source_by_uid`, the raw version rows, `atlas_db_memory_
 * generation_latest`), against a connection this test opened itself rather
 * than the one the writer held.
 *
 * T11 fix round, item 5: the generation and claim checks below ask "does the
 * specific row this test itself created still exist", never "is it still the
 * *only* one" or "is it still the *latest* one". The freshly-forked daemon's
 * own `memory_sweep` reaches `atlas_syspolicy_load` on its first tick, which
 * reads this host's `/etc/atlas/system.conf` -- a compiled-in path A7.1 gives
 * no override for, so this test cannot arrange a controlled one. On a host
 * where `memory_reconcile` is enabled with an unscoped `memory_source` line
 * that matches this fixture repository, that sweep could legitimately append
 * a further generation, or a further claim, for a source this test never
 * registered, between the restart and these reads. Comparing raw counts or
 * "the latest generation number" against a before-restart snapshot would then
 * fail on such a host for a reason that is not a regression. Comparing
 * existence of the specific generation number and the specific claim uid this
 * test recorded before the restart is not vulnerable to that: nothing in
 * Atlas ever deletes or renumbers a `memory_generations` row or a
 * `verify_claims` row (`docs/operations.md`'s retention policy prunes neither
 * table), so a sweep with nothing to do with this test's own rows can only
 * ever add alongside them, never remove or renumber what already survived. */
static void test_memory_survives_a_daemon_restart(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/a.c", "int x;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "src/a.c",
                "1111111111111111111111111111111111111111111111111111111111111111", &err);

    int64_t source_id = 0;
    atlas_buf source_uid = ATLAS_BUF_INIT;
    static const char PATH[] = "/home/u/notes.md";
    T_OK(atlas_db_memory_source_upsert(e.db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, PATH,
                                       strlen(PATH), PATH, "t0", &source_id, &source_uid, &err),
         &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_buf v1 = ATLAS_BUF_INIT;
    atlas_buf v2 = ATLAS_BUF_INIT;
    {
        atlas_memory_put_op op;
        atlas_memory_put_op_init(&op);
        T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
        T_OK(atlas_buf_set_str(&op.content, "the daemon reads `src/a.c`", &err), &err);
        T_OK(atlas_buf_set_str(&op.observed_at, "2026-01-01T00:00:00Z", &err), &err);
        op.peer_uid = 1000;
        atlas_memory_put_result res;
        atlas_memory_put_result_init(&res);
        T_OK(atlas_writer_memory_put(w, &op, &res, &err), &err);
        T_OK(atlas_buf_set(&v1, res.version_uid.data, res.version_uid.len, &err), &err);
        atlas_memory_put_op_free(&op);
        atlas_memory_put_result_free(&res);
    }
    {
        atlas_memory_put_op op;
        atlas_memory_put_op_init(&op);
        T_OK(atlas_buf_set(&op.source_uid, source_uid.data, source_uid.len, &err), &err);
        T_OK(atlas_buf_set_str(&op.content, "the daemon reads `src/a.c` -- updated", &err), &err);
        T_OK(atlas_buf_set_str(&op.observed_at, "2026-01-02T00:00:00Z", &err), &err);
        op.peer_uid = 1000;
        atlas_memory_put_result res;
        atlas_memory_put_result_init(&res);
        T_OK(atlas_writer_memory_put(w, &op, &res, &err), &err);
        T_OK(atlas_buf_set(&v2, res.version_uid.data, res.version_uid.len, &err), &err);
        atlas_memory_put_op_free(&op);
        atlas_memory_put_result_free(&res);
    }
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&v1), atlas_buf_cstr(&v2)) != 0,
                "two puts of different content produced the same version uid");

    /* The caller's own loaded policy -- atlas_writer_submit_memory_reconcile's
     * own contract -- naming this repository's one external source. */
    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;
    pol.memory_source_count = 1;
    pol.memory_sources[0].cls = ATLAS_MEMORY_SOURCE_EXTERNAL_FILE;
    pol.memory_sources[0].repo_name[0] = '\0';
    (void)snprintf(pol.memory_sources[0].path, sizeof pol.memory_sources[0].path, "%s", PATH);

    T_OK(atlas_writer_submit_memory_reconcile(w, e.repo_id, &pol, &err), &err);

    int64_t generation_before = 0;
    T_REQUIRE_MSG(t11_wait_for_generation(&e, &generation_before, &err),
                 "the reconciliation pass never landed a generation");

    /* The *specific* claim this pass created, not a repository-wide count --
     * see this function's own header comment. */
    atlas_buf claim_uid_before = ATLAS_BUF_INIT;
    {
        atlas_db *rdb = NULL;
        T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);
        sqlite3_stmt *stmt = NULL;
        T_OK(atlas_db_prepare(rdb,
                              "SELECT uid FROM verify_claims WHERE repo_id = ?1"
                              " ORDER BY id ASC LIMIT 1;",
                              &stmt, &err),
             &err);
        T_REQUIRE(sqlite3_bind_int64(stmt, 1, e.repo_id) == SQLITE_OK);
        T_REQUIRE_MSG(sqlite3_step(stmt) == SQLITE_ROW,
                     "expected the reconciliation pass to anchor at least one claim to "
                     "`src/a.c`");
        const unsigned char *u = sqlite3_column_text(stmt, 0);
        T_OK(atlas_buf_set_str(&claim_uid_before, u != NULL ? (const char *)u : "", &err), &err);
        atlas_db_finish(rdb, stmt);
        atlas_db_close(rdb);
    }
    T_CHECK_MSG(claim_uid_before.len > 0, "the pre-restart claim uid was empty");

    /* Stopped before the fresh daemon starts, so this is a genuine restart
     * against one open writer at a time rather than two writable handles
     * open on the same file. */
    t11_writer_close(log, w);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&e.fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    /* A connection this test opened itself, reading exactly what a restarted
     * process finds on disk -- not the handle the writer held, and not the
     * daemon's own. */
    atlas_db *rdb = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&e.db_path), &rdb, &err), &err);

    int64_t found_id = 0;
    atlas_memory_source_class found_cls = ATLAS_MEMORY_SOURCE_UNKNOWN;
    bool src_found = false;
    T_OK(atlas_db_memory_source_by_uid(rdb, atlas_buf_cstr(&source_uid), &found_id, NULL,
                                       &found_cls, NULL, NULL, &src_found, &err),
         &err);
    T_CHECK_MSG(src_found, "the registered source did not survive a restart");
    T_CHECK_MSG(found_cls == ATLAS_MEMORY_SOURCE_EXTERNAL_FILE,
                "the source's class changed across a restart");
    T_CHECK_MSG(found_id == source_id, "the source's own id changed across a restart");

    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(rdb,
                          "SELECT version_uid FROM memory_source_versions"
                          " WHERE source_id = ?1 ORDER BY id ASC;",
                          &stmt, &err),
         &err);
    T_REQUIRE(sqlite3_bind_int64(stmt, 1, found_id) == SQLITE_OK);
    char got1[64];
    char got2[64];
    memset(got1, 0, sizeof got1);
    memset(got2, 0, sizeof got2);
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *u = sqlite3_column_text(stmt, 0);
        if (count == 0) {
            (void)snprintf(got1, sizeof got1, "%s", u != NULL ? (const char *)u : "");
        } else if (count == 1) {
            (void)snprintf(got2, sizeof got2, "%s", u != NULL ? (const char *)u : "");
        }
        count++;
    }
    atlas_db_finish(rdb, stmt);
    T_CHECK_MSG(count == 2, "expected two version rows to survive the restart, found %d", count);
    T_CHECK_MSG(strcmp(got1, atlas_buf_cstr(&v1)) == 0,
                "the first version's uid changed across a restart: %s vs %s", got1,
                atlas_buf_cstr(&v1));
    T_CHECK_MSG(strcmp(got2, atlas_buf_cstr(&v2)) == 0,
                "the second version's uid changed across a restart: %s vs %s", got2,
                atlas_buf_cstr(&v2));

    /* Whether *this* generation survived, rather than whether it is still the
     * latest one -- see this function's own header comment. Nothing deletes
     * or renumbers a memory_generations row, so this is a plain existence
     * check over the exact (repo_id, generation) pair `t11_wait_for_
     * generation` observed before the restart. */
    char gsql[256];
    (void)snprintf(gsql, sizeof gsql,
                  "SELECT COUNT(*) FROM memory_generations"
                  " WHERE repo_id = %lld AND generation = %lld;",
                  (long long)e.repo_id, (long long)generation_before);
    T_CHECK_MSG(t11_scalar(rdb, gsql, &err) == 1,
                "generation %lld did not survive the restart", (long long)generation_before);

    atlas_db_close(rdb);

    /* The claim itself, read by a process that did not create it -- O10's
     * "accepted must mean committed and rediscoverable", one layer over, and
     * through the client group's own verify.claims / verify.show rather than
     * the operator-gated memory.status this test cannot reach. verify.claims
     * is asked only to confirm the endpoint still answers after the restart
     * (host-independent by construction); the actual survival claim is
     * verify.show against the exact uid recorded before the restart. */
    atlas_buf resp = ATLAS_BUF_INIT;
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "verify.claims", "{\"repo\":\"proj\"}", &resp,
                        &err),
         &err);
    atlas_ipc_response *cr = NULL;
    T_OK(atlas_ipc_response_parse(resp.data, resp.len, &cr, &err), &err);
    T_REQUIRE(cr != NULL);
    T_CHECK_MSG(atlas_ipc_response_ok(cr), "verify.claims failed after a restart: %s",
                atlas_buf_cstr(&resp));
    atlas_ipc_response_free(cr);
    atlas_buf_free(&resp);

    char params[256];
    (void)snprintf(params, sizeof params, "{\"claim\":\"%s\"}", atlas_buf_cstr(&claim_uid_before));
    atlas_buf resp2 = ATLAS_BUF_INIT;
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "verify.show", params, &resp2, &err), &err);
    atlas_ipc_response *sr = NULL;
    T_OK(atlas_ipc_response_parse(resp2.data, resp2.len, &sr, &err), &err);
    T_REQUIRE(sr != NULL);
    T_CHECK_MSG(atlas_ipc_response_ok(sr), "verify.show could not show the surviving claim: %s",
                atlas_buf_cstr(&resp2));
    const char *echoed = NULL;
    (void)atlas_ipc_result_str(sr, "claim", &echoed);
    T_CHECK_MSG(echoed != NULL && strcmp(echoed, atlas_buf_cstr(&claim_uid_before)) == 0,
                "verify.show echoed a different claim than the one asked for");
    atlas_ipc_response_free(sr);
    atlas_buf_free(&resp2);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);

    atlas_buf_free(&v1);
    atlas_buf_free(&v2);
    atlas_buf_free(&claim_uid_before);
    atlas_buf_free(&source_uid);
    t8_env_close(&e);
}

static const atlas_test TESTS[] = {
    {"memory.put/status/reconcile refuse a non-operator peer over a real socket",
     test_memory_methods_refuse_a_non_operator_peer},
    {"acceptance item 4: reconciled memory survives a daemon restart",
     test_memory_survives_a_daemon_restart},
};

ATLAS_TEST_MAIN("memory_reconcile_live", TESTS)

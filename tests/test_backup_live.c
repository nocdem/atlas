/* Atlas - backup, restore and maintenance against a live daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The cases here need a running daemon, which is the whole point of three of
 * them:
 *
 *   an online snapshot is consistent   taken while the daemon is serving reads
 *                                      and its writer thread is committing, and
 *                                      then verified in full.
 *   a restore cannot race a daemon     refused because the daemon holds the
 *                                      writer lock, not because of a check that
 *                                      could be forgotten.
 *   a prune cannot race a daemon       refused for the same reason, and a plan
 *                                      still works while it runs.
 *
 * And one that is about what is *not* there: the daemon does not answer to any
 * backup, restore or maintenance method, asserted by asking it.
 */
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/ipc.h"
#include "atlas/maintenance.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

static void build_repo(fixture *fx, atlas_err *err) {
    T_OK(fx_init_repo(fx, fx_repo(fx), NULL, err), err);
    T_OK(fx_write(fx_repo(fx), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(fx, fx_repo(fx), err), err);
    T_OK(fx_commit(fx, fx_repo(fx), "first", err), err);
    const char *dd = fx_data_dir(fx);
    const char *add[] = {"repo", "add", fx_repo(fx), "--name", "proj", "--data-dir", dd};
    atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(add, 7, &so, &se, &code, err), err);
    T_CHECK_MSG(code == 0, "repo add exited %d: %s", code, atlas_buf_cstr(&se));
    atlas_buf_free(&so);
    atlas_buf_free(&se);
}

/* A snapshot taken while the daemon owns the index. The daemon keeps the writer
 * lock the whole time, so this also proves the backup does not take it. */
static void test_a_snapshot_of_a_live_daemon_verifies(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    /* Give the daemon work: each of these is a real reconciliation, so the
     * writer thread is committing while the copy below runs. */
    for (int i = 0; i < 4; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "f%d.c", i);
        T_OK(fx_write(fx_repo(&fx), name, "int x;\n", &err), &err);
        const char *sync[] = {"sync", "proj", "--wait"};
        atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&fx, &d, sync, 3, &so, &se, &code, &err), &err);
        atlas_buf_free(&so);
        atlas_buf_free(&se);
    }

    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&out, &err, "%s/live.db", atlas_buf_cstr(&fx.root)), &err);

    atlas_backup_create_opts o;
    memset(&o, 0, sizeof o);
    o.output = atlas_buf_cstr(&out);
    atlas_backup_report rep;
    atlas_backup_report_init(&rep);
    T_OK(atlas_service_backup_create(fx_data_dir(&fx), &o, &rep, &err), &err);
    T_CHECK_MSG(rep.source_online,
                "the daemon was running but the backup did not report an online source");
    atlas_backup_report_free(&rep);

    /* The daemon is still alive and still answering: the snapshot did not take
     * its lock away from it. */
    T_CHECK(!fx_daemon_exited(&d));
    {
        const char *ping[] = {"daemon", "ping"};
        atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&fx, &d, ping, 2, &so, &se, &code, &err), &err);
        T_EQ_INT(code, 0);
        atlas_buf_free(&so);
        atlas_buf_free(&se);
    }

    atlas_backup_verify_report vr;
    atlas_backup_verify_report_init(&vr);
    T_OK(atlas_service_backup_verify(atlas_buf_cstr(&out), &vr, &err), &err);
    T_CHECK_MSG(vr.ok, "an online snapshot did not verify: %s",
                atlas_buf_cstr(&vr.problems));
    T_CHECK(vr.repo_count == 1);
    atlas_backup_verify_report_free(&vr);

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* Restore and prune are writers, and Atlas has exactly one writer. Both are
 * refused while a daemon holds the lock — by the kernel, not by a check. */
static void test_a_running_daemon_refuses_a_restore_and_a_prune(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&out, &err, "%s/before.db", atlas_buf_cstr(&fx.root)), &err);
    atlas_backup_create_opts o;
    memset(&o, 0, sizeof o);
    o.output = atlas_buf_cstr(&out);
    atlas_backup_report brep;
    atlas_backup_report_init(&brep);
    T_OK(atlas_service_backup_create(fx_data_dir(&fx), &o, &brep, &err), &err);
    atlas_backup_report_free(&brep);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    atlas_backup_restore_opts ro;
    memset(&ro, 0, sizeof ro);
    ro.input = atlas_buf_cstr(&out);
    ro.confirmed = true;
    atlas_backup_restore_report rrep;
    atlas_backup_restore_report_init(&rrep);
    atlas_err e2;
    atlas_err_init(&e2);
    atlas_status st = atlas_service_backup_restore(fx_data_dir(&fx), &ro, &rrep, &e2);
    T_CHECK_MSG(st != ATLAS_OK, "a restore succeeded while a daemon held the writer lock");
    T_CHECK(!rrep.published);
    atlas_backup_restore_report_free(&rrep);

    atlas_maintenance_opts mo;
    memset(&mo, 0, sizeof mo);
    mo.older_than_days = 30;
    mo.apply = true;
    atlas_maintenance_report mrep;
    atlas_maintenance_report_init(&mrep);
    atlas_err e3;
    atlas_err_init(&e3);
    st = atlas_service_maintenance(fx_data_dir(&fx), &mo, &mrep, &e3);
    T_CHECK_MSG(st != ATLAS_OK, "a prune succeeded while a daemon held the writer lock");
    T_EQ_INT(mrep.total_removed, 0);
    atlas_maintenance_report_free(&mrep);

    /* A plan takes no lock, so it still works while the daemon runs. */
    memset(&mo, 0, sizeof mo);
    mo.older_than_days = 30;
    atlas_maintenance_report plan;
    atlas_maintenance_report_init(&plan);
    T_OK(atlas_service_maintenance(fx_data_dir(&fx), &mo, &plan, &err), &err);
    T_CHECK(!plan.applied);
    T_CHECK(plan.table_count > 0);
    atlas_maintenance_report_free(&plan);

    /* And a backup still works while the daemon runs, which is the point. */
    atlas_buf second = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&second, &err, "%s/during.db", atlas_buf_cstr(&fx.root)), &err);
    o.output = atlas_buf_cstr(&second);
    atlas_backup_report_init(&brep);
    T_OK(atlas_service_backup_create(fx_data_dir(&fx), &o, &brep, &err), &err);
    atlas_backup_report_free(&brep);

    T_CHECK(!fx_daemon_exited(&d));
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&second);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* The structural claim, asked of the process that would have to answer. A
 * method that does not exist is a better guarantee than one that refuses. */
static void test_the_daemon_answers_to_no_backup_or_maintenance_method(void) {
    static const char *METHODS[] = {
        "backup.create",       "backup.verify",     "backup.restore", "maintenance.plan",
        "maintenance.prune",   "db.backup",         "db.restore",     "index.prune",
    };
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_repo(&fx, &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), METHODS[i], "{}", &resp, &e2);
        /* Either the call fails outright or the daemon reports an error; what
         * must never happen is a success document. */
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL,
                    "the daemon accepted \"%s\"", METHODS[i]);
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* A mutation is applied to the data directory it named, not to whichever one a
 * reachable daemon happens to own.
 *
 * This is an A1 routing property and it lives here because A5 is where it was
 * found: `atlas --data-dir X repo add ...` with a daemon running on directory Y
 * wrote the registration into Y and reported success, with `--data-dir X`
 * mentioned by neither process. There is one socket per user runtime directory
 * but a data directory is chosen per invocation, so reachability was never the
 * right question.
 *
 * It surfaced while preparing a real pilot, where the consequence would have
 * been a test command writing into somebody's live index. */
static void test_a_mutation_goes_to_the_data_directory_it_named(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    /* A repository, and a second data directory the daemon knows nothing of. */
    T_OK(fx_init_repo(&fx, fx_repo(&fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
    T_OK(fx_commit(&fx, fx_repo(&fx), "first", &err), &err);

    atlas_buf other = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&other, &err, "%s/elsewhere", atlas_buf_cstr(&fx.root)), &err);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&other), &err), &err);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    /* The daemon owns the fixture's data directory. This names the other one,
     * over the same runtime directory, so the daemon is reachable throughout. */
    const char *add[] = {"repo", "add", fx_repo(&fx), "--name",
                         "elsewhere", "--data-dir", atlas_buf_cstr(&other)};
    atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas_with_runtime(&fx, &d, add, 7, &so, &se, &code, &err), &err);
    T_CHECK_MSG(code == 0, "repo add exited %d: %s", code, atlas_buf_cstr(&se));
    atlas_buf_free(&so);
    atlas_buf_free(&se);

    /* It landed where it was told, and nowhere else. */
    atlas_db *db = NULL;
    atlas_buf p = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(atlas_buf_cstr(&other), &p, &err), &err);
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&p), &db, &err), &err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repositories;", &n, &err), &err);
    T_CHECK_MSG(n == 1, "the named data directory holds %lld repositories, expected 1",
                (long long)n);
    atlas_db_close(db);

    atlas_buf_reset(&p);
    T_OK(atlas_datadir_db_path(fx_data_dir(&fx), &p, &err), &err);
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&p), &db, &err), &err);
    n = -1;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repositories;", &n, &err), &err);
    T_CHECK_MSG(n == 0,
                "the daemon's own index gained %lld repositories from a command that named a "
                "different data directory",
                (long long)n);
    atlas_db_close(db);

    /* Registration was only the first of three places that decided this. `sync`
     * and `code sync` each had their own reachability test, and each would have
     * asked a daemon on another index to reconcile a repository it has never
     * heard of. They are checked here rather than trusted, because the first
     * fix looked complete and was not: the second and third only surfaced when
     * the suite was run on a machine that had a real daemon of its own. */
    const char *sync[] = {"sync", "elsewhere", "--wait", "--data-dir", atlas_buf_cstr(&other)};
    atlas_buf so3 = ATLAS_BUF_INIT, se3 = ATLAS_BUF_INIT;
    T_OK(fx_atlas_with_runtime(&fx, &d, sync, 5, &so3, &se3, &code, &err), &err);
    T_CHECK_MSG(code == 0, "`sync` against a data directory the daemon does not own exited %d: %s",
                code, atlas_buf_cstr(&se3));
    atlas_buf_free(&so3);
    atlas_buf_free(&se3);

    const char *csync[] = {"code", "sync", "elsewhere", "--wait", "--data-dir",
                           atlas_buf_cstr(&other)};
    atlas_buf so4 = ATLAS_BUF_INIT, se4 = ATLAS_BUF_INIT;
    T_OK(fx_atlas_with_runtime(&fx, &d, csync, 6, &so4, &se4, &code, &err), &err);
    T_CHECK_MSG(code == 0,
                "`code sync` against a data directory the daemon does not own exited %d: %s", code,
                atlas_buf_cstr(&se4));
    atlas_buf_free(&so4);
    atlas_buf_free(&se4);

    /* And a registration that *does* name the daemon's own directory is now
     * refused rather than routed.
     *
     * A7 inverted this assertion deliberately. It used to prove that routing
     * still worked; what routing meant was that `repo.add` was an RPC method,
     * and therefore that anything able to open the socket could decide which
     * directories Atlas treats as repositories. The refusal is the guarantee,
     * and it has to name the remedy. */
    const char *add2[] = {"repo", "add", fx_repo(&fx), "--name", "proper"};
    atlas_buf so2 = ATLAS_BUF_INIT, se2 = ATLAS_BUF_INIT;
    T_OK(fx_atlas_with_runtime(&fx, &d, add2, 5, &so2, &se2, &code, &err), &err);
    T_CHECK_MSG(code != 0, "repo add against a running daemon must be refused, exited %d", code);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&se2), "Stop it first") != NULL,
                "the refusal must tell the operator what to do: %s", atlas_buf_cstr(&se2));
    atlas_buf_free(&so2);
    atlas_buf_free(&se2);
    /* And nothing reached the daemon's index. */
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&p), &db, &err), &err);
    n = -1;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repositories;", &n, &err), &err);
    T_CHECK_MSG(n == 0, "a refused registration still reached the daemon's index (%lld rows)",
                (long long)n);
    atlas_db_close(db);

    T_CHECK(!fx_daemon_exited(&d));
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&p);
    atlas_buf_free(&other);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a snapshot of a live daemon verifies", test_a_snapshot_of_a_live_daemon_verifies},
    {"a running daemon refuses a restore and a prune",
     test_a_running_daemon_refuses_a_restore_and_a_prune},
    {"the daemon answers to no backup or maintenance method",
     test_the_daemon_answers_to_no_backup_or_maintenance_method},
    {"a mutation goes to the data directory it named",
     test_a_mutation_goes_to_the_data_directory_it_named},
};

ATLAS_TEST_MAIN("backup_live", TESTS)

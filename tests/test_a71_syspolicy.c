/* Atlas - A7.1: who may open the socket, and what that buys them.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A7 removed every authority-bearing method from the protocol, which is what
 * made it safe to say the socket carries no authority. A7.1 lets a *second* uid
 * onto that socket — `atlas-worker`, the account every persistent model process
 * runs as — so the sentence has to be re-earned rather than inherited.
 *
 * Two halves, and the split matters:
 *
 *   **Here**, portably and without root: the decision procedure. Given a policy
 *   file of some shape, does Atlas enter system mode? Given a policy, which
 *   uids does it permit? Does a client that *describes* itself in the request
 *   body change any of that? Every one of these is a pure function of a file and
 *   a number, and every unprivileged shape must fail closed.
 *
 *   **Live**, as root, during deployment: whether uid N can actually connect to
 *   the real daemon over the real socket, and what it can do once there. That
 *   cannot be established here and this suite does not pretend to — a fixture
 *   file owned by the test user is not a root-owned policy, and asserting
 *   against one would be asserting about the fixture.
 *
 * What this suite therefore proves is the half that is provable without
 * privilege: **no shape an unprivileged uid can construct anywhere on the
 * filesystem turns system mode on.** That is the property that makes the live
 * half meaningful, because it means the only policy that can be in force is one
 * root wrote.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/ipc.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- the decision procedure ----------------------------------------------- */

static void write_policy(const fixture *fx, const char *name, const char *body, atlas_err *err) {
    T_OK(fx_write(fx_data_dir(fx), name, body, err), err);
}

static void load(const fixture *fx, const char *name, atlas_syspolicy *out) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", fx_data_dir(fx), name);
    atlas_syspolicy_load_at(path, out);
}

static void test_no_unprivileged_shape_enables_system_mode(void) {
    /* Every way an unprivileged uid might try to manufacture a system policy.
     * None can, because all of them fail the root-ownership check that this uid
     * cannot satisfy for any path on the filesystem — the fixture lives under a
     * directory it owns, so the walk refuses at the first non-root component and
     * never even reads the file. */
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);

    atlas_syspolicy p;

    /* Absent. */
    load(&fx, "no-such.conf", &p);
    T_EQ_INT((int)p.state, (int)ATLAS_SYSPOLICY_LEGACY);

    /* Present, perfectly well formed, and written by this uid — the file an
     * attacker would write. */
    write_policy(&fx, "good.conf",
                 "socket_path = /run/atlas/atlas.sock\n"
                 "data_dir = /var/lib/atlas\n"
                 "client_group = atlas-clients\n"
                 "client_uid = 1000\n",
                 &err);
    load(&fx, "good.conf", &p);
    T_CHECK_MSG(p.state == ATLAS_SYSPOLICY_LEGACY,
                "a policy written by an unprivileged uid enabled system mode");

    /* A symlink pointing at something root-owned gains nothing: the walk
     * refuses to traverse it rather than resolving it. */
    T_OK(fx_symlink(fx_data_dir(&fx), "/etc/hostname", "link.conf", &err), &err);
    load(&fx, "link.conf", &p);
    T_CHECK_MSG(p.state == ATLAS_SYSPOLICY_LEGACY, "a symlinked policy enabled system mode");

    /* A directory where the policy should be. */
    T_OK(fx_mkdir(fx_data_dir(&fx), "dir.conf", &err), &err);
    load(&fx, "dir.conf", &p);
    T_CHECK_MSG(p.state == ATLAS_SYSPOLICY_LEGACY, "a directory enabled system mode");

    /* Malformed absolute paths, checked against the loader directly. */
    static const char *const BAD[] = {"etc/atlas/system.conf", "", "/etc/../etc/atlas/system.conf",
                                      "/etc//atlas/system.conf", "/"};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_syspolicy q;
        atlas_syspolicy_load_at(BAD[i], &q);
        T_CHECK_MSG(q.state == ATLAS_SYSPOLICY_LEGACY, "\"%s\" enabled system mode", BAD[i]);
    }

    /* And the machine's real compiled-in path, whatever it is here. Recorded
     * rather than asserted either way: on an undeployed machine it is absent,
     * and on a deployed one it is root-owned and active. What must never happen
     * is that it is active *and* this uid could have written it, which is what
     * every case above rules out. */
    atlas_syspolicy live;
    atlas_syspolicy_load(&live);
    atlas_test_note(live.state == ATLAS_SYSPOLICY_SYSTEM
                        ? "this machine has an active root-owned system policy"
                        : "this machine is in per-user mode");

    fx_close(&fx);
}

/* --- the allowlist -------------------------------------------------------- */

static void test_the_allowlist_permits_exactly_what_it_lists(void) {
    /* `atlas_syspolicy_permits` is what stands between a stranger's connection
     * and the serve loop, so its answer is enumerated rather than sampled. */
    atlas_syspolicy p;
    memset(&p, 0, sizeof(p));

    /* Legacy mode permits nobody at all — the daemon's own uid is checked by
     * the caller, separately, so this function can never be the only thing
     * standing between a stranger and the socket. */
    T_CHECK(!atlas_syspolicy_permits(&p, 0));
    T_CHECK(!atlas_syspolicy_permits(&p, 1000));
    T_CHECK(!atlas_syspolicy_permits(NULL, 1000));

    p.state = ATLAS_SYSPOLICY_SYSTEM;
    p.client_uids[0] = 991;
    p.client_uids[1] = 1000;
    p.client_count = 2;

    T_CHECK(atlas_syspolicy_permits(&p, 991));
    T_CHECK(atlas_syspolicy_permits(&p, 1000));
    /* Everything else, including root and near-misses. */
    static const long long DENIED[] = {0, 1, 990, 992, 999, 1001, 65534, 4294967294LL};
    for (size_t i = 0; i < sizeof DENIED / sizeof DENIED[0]; i++) {
        T_CHECK_MSG(!atlas_syspolicy_permits(&p, DENIED[i]), "uid %lld was permitted", DENIED[i]);
    }
}

/* --- a client cannot describe itself -------------------------------------- */

static void test_a_client_cannot_supply_its_own_identity(void) {
    /* The identity that decides acceptance is `SO_PEERCRED`, which the kernel
     * fills in at connect time. This drives a live daemon with requests that
     * assert a different identity in every place a careless implementation
     * might have looked for one, and requires the answers to be identical to
     * the same requests without them.
     *
     * If any of these were consulted, a model would be one JSON member away
     * from being root. */
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

    /* `daemon.ping` rather than `repo.list`, because the comparison has to be
     * byte-for-byte and `repo.list` carries live indexing state that moves on
     * its own while the daemon works. A test that compared a moving answer
     * would fail for a reason unrelated to identity — which is exactly what the
     * first version of it did. Ping reports the version, the protocol, the pid
     * and the data directory this daemon owns: all constant for its lifetime,
     * and all things an identity claim would be trying to influence. */
    atlas_buf plain = ATLAS_BUF_INIT;
    T_OK(atlas_ipc_call(atlas_buf_cstr(&d.socket), "daemon.ping", "{}", &plain, &err), &err);

    static const char *const CLAIMS[] = {
        "{\"uid\":0}",
        "{\"gid\":0}",
        "{\"pid\":1}",
        "{\"user\":\"root\"}",
        "{\"role\":\"operator\"}",
        "{\"peer_uid\":0,\"peer_gid\":0}",
        "{\"client_uid\":0}",
        "{\"authority\":\"GRANTED\"}",
        "{\"operator_uid\":0}",
        "{\"trusted\":true,\"system\":true}",
    };
    for (size_t i = 0; i < sizeof CLAIMS / sizeof CLAIMS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), "daemon.ping", CLAIMS[i], &resp, &e2);
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&resp), atlas_buf_cstr(&plain)) == 0,
                    "claiming %s changed the answer:\n  with: %s\n  without: %s", CLAIMS[i],
                    atlas_buf_cstr(&resp), atlas_buf_cstr(&plain));
        atlas_buf_free(&resp);
    }

    /* And the same claims do not unlock anything that does not exist. */
    for (size_t i = 0; i < sizeof CLAIMS / sizeof CLAIMS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(atlas_buf_cstr(&d.socket), "decision.approve", CLAIMS[i], &resp, &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                    "decision.approve answered something other than unknown for %s: %s",
                    CLAIMS[i], atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    atlas_buf_free(&plain);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- the index a client resolves ------------------------------------------ */

static void test_an_explicit_data_dir_still_wins(void) {
    /* `--data-dir` is how the operator runs an offline lifecycle command and how
     * this suite isolates itself, so it must keep overriding everything —
     * including an active system policy. The complementary half, that
     * `ATLAS_DATA_DIR` and `$HOME` stop selecting an index once a system policy
     * is active, is a property of a machine that has one and is verified live by
     * `scripts/a71-verify.sh`. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_datadir_source src = ATLAS_DATADIR_HOME;
    T_OK(atlas_datadir_resolve("/tmp/atlas-explicit", &out, &src, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "/tmp/atlas-explicit");
    T_EQ_INT((int)src, (int)ATLAS_DATADIR_OVERRIDE);
    atlas_buf_free(&out);
}

static const atlas_test TESTS[] = {
    {"no unprivileged shape enables system mode",
     test_no_unprivileged_shape_enables_system_mode},
    {"the allowlist permits exactly what it lists",
     test_the_allowlist_permits_exactly_what_it_lists},
    {"a client cannot supply its own identity", test_a_client_cannot_supply_its_own_identity},
    {"an explicit data dir still wins", test_an_explicit_data_dir_still_wins},
};

ATLAS_TEST_MAIN("a71_syspolicy", TESTS)

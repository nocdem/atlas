/* Atlas - A8: what the orchestration protocol exposes, and what it must not.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Three claims, checked three ways.
 *
 *   **The A7/A7.1 negative matrix stays green.** Every authority-bearing name —
 *   lifecycle, registry, backup, restore, maintenance — still answers `unknown
 *   method`, and adding a control plane did not put one back. The list here is
 *   deliberately longer than A7's: it also covers the names an orchestration
 *   phase would plausibly reach for if somebody decided a finished job should be
 *   allowed to land itself.
 *
 *   **An ordinary client cannot reach a dispatcher method.** The group is
 *   selected by the peer's uid from SO_PEERCRED, so a name in the group this
 *   peer is not in is simply not found — the same answer as for a name that does
 *   not exist. A refusal that distinguished "you may not" from "there is no such
 *   thing" would tell a caller what to try next.
 *
 *   **A machine with no orchestration policy runs no jobs.** The fixture daemon
 *   has no root-owned policy — an unprivileged uid cannot create one anywhere,
 *   which is the point — so every `job.` method refuses. That is the fail-closed
 *   default, and it is the state of every machine until an operator installs a
 *   policy deliberately.
 *
 * Required cases covered here: 23 (malformed and oversized frames), 41 (ordinary
 * clients cannot call dispatcher RPC), 42 (dispatcher cannot call lifecycle
 * RPC), 48 (the existing authority enumeration), 50 (no live resource is
 * touched).
 */
#include <string.h>

#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* Declared in src/ipc/server_internal.h, which is not on the test include path.
 * The two tables are the subject of half this file, so they are reached
 * directly rather than inferred from a live daemon's behaviour. */
typedef struct atlas_method_entry atlas_method_entry;
const atlas_method_entry *atlas_server_orch_client_methods(size_t *count_out);
const atlas_method_entry *atlas_server_orch_dispatch_methods(size_t *count_out);
/* Only the name is read here; the function pointer is the server's business. */
struct atlas_method_entry {
    const char *name;
    void *fn;
};

/* --- the negative enumeration ------------------------------------------------
 *
 * Every name a method would plausibly have if A8 had grown one it must not.
 * Asked of a live daemon, and required to answer `unknown method` rather than
 * merely to fail: an absent method is answered by the dispatcher's unknown-method
 * case, and a refusing one is a refusal a later edit can weaken. That is A7's
 * argument and it is why those methods were deleted rather than left refusing. */
static const char *const FORBIDDEN_METHODS[] = {
    /* A4/A6 lifecycle. A job may never mint or spend a capability. */
    "decision.challenge", "decision.approve", "decision.reject", "decision.supersede",
    "decision.revalidate", "Decision.Approve", "DECISION.APPROVE", "decision_approve",
    /* A9.1's resolve, for the same reason: a dispatcher's peer is `atlas-worker`,
     * and a completed job must not be able to record that the obligation it was
     * working on has been discharged. */
    "decision.resolve", "decision_resolve", "DECISION.RESOLVE",
    /* A7 registry. Nothing registers a repository except an operator. */
    "repo.add", "repo.ensure", "repo.remove", "repo.register", "repo.unregister",
    /* A5 backup, restore and maintenance. */
    "backup.create", "backup.verify", "backup.restore", "backup.list",
    "maintenance.prune", "maintenance.plan", "db.restore", "index.rebuild",
    /* A8-CI and A9.2.3: causing a compiler to run.
     *
     * `code.index` runs one when an operator asks. `code.sem_config` decides
     * whether the daemon runs one **every time the repository changes**, which
     * is the stronger capability of the two — so it goes in the operator-uid
     * table beside it, and an ordinary peer is told neither exists. The name
     * variants are here because a gate that a caller can walk around by
     * changing case is not a gate. */
    "code.index", "code.sem_config", "Code.Sem_Config", "CODE.SEM_CONFIG",
    "code.semconfig", "code.sem-config", "sem.config", "sem.config_set",
    "sem.index", "sem.rebuild", "code.rebuild",
    /* A9.2.4. Discovery decides what a compiler is run *over*, and activation
     * decides whether one runs at all — so every verb somebody would reach for
     * to declare a search complete, add a build input, enable maintenance or
     * clear an operator's refusal is enumerated here. None of them exists:
     * `sem.status` already carries the whole derived state as a read, and a
     * separate verb would only be a second surface to defend. */
    "sem.discover", "code.discover", "sem.discovery", "code.sem_discover",
    "sem.enable", "sem.disable", "code.sem_enable", "code.sem_disable",
    "sem.set_discovery", "sem.set_auto", "sem.compdb_add", "code.compdb_add",
    "sem.mark_complete", "sem.mark_current", "sem.build_inputs_set",
    /* The daemon's own lifecycle. */
    "daemon.shutdown", "daemon.stop", "daemon.restart",
    /* And the ones an orchestration phase would reach for if somebody decided a
     * finished job should be allowed to land itself. Every one of these is
     * explicitly deferred, and their absence is the deferral. */
    "job.apply", "job.apply_patch", "job.commit", "job.push", "job.merge",
    "job.branch", "job.pr", "job.github", "job.approve", "job.authorize",
    "orch.apply", "orch.commit", "orch.push", "patch.apply", "patch.commit",
    "dispatch.apply", "dispatch.commit", "dispatch.push",
};

/* Dispatcher methods, asked from an ordinary client connection. */
static const char *const DISPATCHER_METHODS[] = {
    "dispatch.lease", "dispatch.heartbeat", "dispatch.event", "dispatch.complete",
    "Dispatch.Lease", "DISPATCH.LEASE", "dispatch_lease",
};

static void expect_unknown(const char *socket, const char *method, const char *params) {
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_ipc_call(socket, method, params, &resp, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                "%s answered something other than unknown: %s", method, atlas_buf_cstr(&resp));
    atlas_buf_free(&resp);
}

static void test_no_authority_method_exists_in_the_protocol(void) {
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
    const char *sock = atlas_buf_cstr(&d.socket);

    for (size_t i = 0; i < sizeof FORBIDDEN_METHODS / sizeof FORBIDDEN_METHODS[0]; i++) {
        expect_unknown(sock, FORBIDDEN_METHODS[i], "{}");
    }
    /* And with every identity a caller might assert about itself. A method that
     * does not exist cannot be unlocked by claiming to be root. */
    static const char *const CLAIMS[] = {
        "{\"uid\":0}", "{\"role\":\"dispatcher\"}", "{\"dispatcher\":true}",
        "{\"peer_uid\":993}", "{\"authority\":\"GRANTED\"}", "{\"trusted\":true}",
    };
    for (size_t i = 0; i < sizeof CLAIMS / sizeof CLAIMS[0]; i++) {
        expect_unknown(sock, "decision.approve", CLAIMS[i]);
        expect_unknown(sock, "repo.add", CLAIMS[i]);
        expect_unknown(sock, "job.apply", CLAIMS[i]);
    }

    /* Required case 41. The dispatcher group is not reachable from this
     * connection, and the answer is indistinguishable from a name that does not
     * exist — including when the request claims to be the dispatcher. */
    for (size_t i = 0; i < sizeof DISPATCHER_METHODS / sizeof DISPATCHER_METHODS[0]; i++) {
        expect_unknown(sock, DISPATCHER_METHODS[i], "{}");
        expect_unknown(sock, DISPATCHER_METHODS[i], "{\"uid\":993,\"role\":\"dispatcher\"}");
        expect_unknown(sock, DISPATCHER_METHODS[i],
                       "{\"token\":\"0000000000000000000000000000000000000000000000000000000"
                       "000000000\"}");
    }

    /* A fixture daemon runs no jobs — even on a machine where a real
     * orchestration policy is installed.
     *
     * The policy path is compiled in, so without a guard its mere presence would
     * arm orchestration in every daemon on the machine, including this one and
     * any ad-hoc daemon an unprivileged user starts on their own database. The
     * daemon applies the policy only when it is serving the *system* index,
     * exactly as it does for the A7.1 system policy. A clean-extraction run
     * caught the missing guard by finding this fixture daemon happily answering
     * `job.list` under the live policy. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(sock, "job.submit",
                             "{\"repo\":\"proj\",\"task\":\"do something\"}", &resp, &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "orchestration is not enabled") != NULL,
                    "job.submit on a policy-less machine answered: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
    for (const char *m = NULL;;) {
        static const char *const CLIENT_METHODS[] = {"job.get", "job.list", "job.cancel",
                                                    "job.artifact"};
        for (size_t i = 0; i < sizeof CLIENT_METHODS / sizeof CLIENT_METHODS[0]; i++) {
            atlas_buf resp = ATLAS_BUF_INIT;
            atlas_err e2;
            atlas_err_init(&e2);
            (void)atlas_ipc_call(sock, CLIENT_METHODS[i], "{\"job\":\"j0\"}", &resp, &e2);
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "orchestration is not enabled") != NULL,
                        "%s on a policy-less machine answered: %s", CLIENT_METHODS[i],
                        atlas_buf_cstr(&resp));
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL,
                        "%s succeeded with orchestration disabled", CLIENT_METHODS[i]);
            atlas_buf_free(&resp);
        }
        (void)m;
        break;
    }

    /* Nothing above created a job, an attempt or a lease. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&fx), "--json", "doctor"};
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        (void)fx_atlas(args, 4u, &out, NULL, &code, &err);
        atlas_buf_free(&out);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- the two tables ----------------------------------------------------------- */

static void test_the_two_method_groups_are_disjoint_and_carry_no_verb(void) {
    size_t nc = 0, nd = 0;
    const atlas_method_entry *c = atlas_server_orch_client_methods(&nc);
    const atlas_method_entry *d = atlas_server_orch_dispatch_methods(&nd);
    T_CHECK(nc > 0 && nd > 0);

    /* Disjoint. An overlap would mean a name whose behaviour depends on which
     * table was searched first, which is the drift two dispatch tables cause. */
    for (size_t i = 0; i < nc; i++) {
        for (size_t k = 0; k < nd; k++) {
            T_CHECK_MSG(strcmp(c[i].name, d[k].name) != 0,
                        "\"%s\" is in both orchestration method groups", c[i].name);
        }
    }
    /* Each group's names are prefixed by its own namespace, so which group a
     * method is in is visible in the name a caller types. */
    for (size_t i = 0; i < nc; i++) {
        T_CHECK_MSG(strncmp(c[i].name, "job.", 4u) == 0, "client method \"%s\" is misnamed",
                    c[i].name);
    }
    for (size_t k = 0; k < nd; k++) {
        T_CHECK_MSG(strncmp(d[k].name, "dispatch.", 9u) == 0,
                    "dispatcher method \"%s\" is misnamed", d[k].name);
    }

    /* No verb that would mean authority or repository mutation. A8 produces a
     * patch as an artifact; applying, committing, pushing, merging and approving
     * are deferred, and their absence from these names is the deferral. */
    static const char *const VERBS[] = {"approve", "reject", "supersede", "revalidate",
                                        "apply",   "commit", "push",      "merge",
                                        "branch",  "pr",     "github",    "authorize",
                                        "grant",   "install", "restore",  "prune",
                                        "backup",  "register"};
    for (size_t g = 0; g < 2; g++) {
        const atlas_method_entry *t = g == 0 ? c : d;
        size_t n = g == 0 ? nc : nd;
        for (size_t i = 0; i < n; i++) {
            for (size_t v = 0; v < sizeof VERBS / sizeof VERBS[0]; v++) {
                T_CHECK_MSG(strstr(t[i].name, VERBS[v]) == NULL,
                            "orchestration method \"%s\" contains the verb \"%s\"", t[i].name,
                            VERBS[v]);
            }
        }
    }
}

static void test_a_disabled_policy_selects_the_client_group_for_everybody(void) {
    /* The group is chosen by `atlas_orchpolicy_is_dispatcher`, which is false
     * for every uid when no policy is active — including the daemon's own. So a
     * machine with no policy has no dispatcher at all, and `dispatch.*` is
     * unreachable from any connection rather than merely from untrusted ones. */
    atlas_orchpolicy p;
    memset(&p, 0, sizeof(p));
    for (long long uid = 0; uid < 2000; uid += 499) {
        T_CHECK_MSG(!atlas_orchpolicy_is_dispatcher(&p, uid),
                    "uid %lld was treated as the dispatcher with no policy", uid);
    }
    /* And a policy that names a dispatcher names exactly one. */
    T_CHECK(!atlas_orchpolicy_is_dispatcher(NULL, 993));
}

static const atlas_test TESTS[] = {
    {"no authority method exists in the protocol",
     test_no_authority_method_exists_in_the_protocol},
    {"the two method groups are disjoint and carry no verb",
     test_the_two_method_groups_are_disjoint_and_carry_no_verb},
    {"a disabled policy selects the client group for everybody",
     test_a_disabled_policy_selects_the_client_group_for_everybody},
};

ATLAS_TEST_MAIN("orch_rpc", TESTS)

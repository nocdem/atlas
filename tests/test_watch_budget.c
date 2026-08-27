/* Atlas - P0: the watch budget, its accounting, and the bound each refusal names.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * What this suite is for.
 *
 * `ATLAS_WATCH_MAX_DIRS` was documented as a per-repository ceiling of 8192 and
 * enforced against the daemon-global watch count with a `+ 1 >=` comparison, so
 * the real ceiling was 8191 across every repository at once. On a machine whose
 * kernel offered 122910 watches, the second of two registered repositories —
 * chosen by `ORDER BY name`, not by need — was permanently degraded, its index
 * permanently not current, and the message it was given said it had more
 * directories than Atlas would watch. Every part of that sentence was
 * misleading: the limit was not per repository, it was not the number in the
 * header, and the repository was not too big.
 *
 * So the properties asserted here are about *which* bound stopped a watch and
 * *what* was counted, not merely that something stopped:
 *
 *   1. With a budget of N, exactly N watches install and the N+1th is refused —
 *      no off-by-one, at any of the four bounds.
 *   2. Two repositories whose sum exceeds any single one's share are both
 *      watched completely, and neither consumes the other's share.
 *   3. A repository is told which bound it reached, from a closed vocabulary,
 *      and the daemon's own budget is never reported as the repository's size.
 *   4. Physical descriptors and logical subscriptions are different numbers, and
 *      the relation between them is `>=` rather than `==`.
 *   5. The bounds are consistent with the types that have to hold them.
 *
 * The watcher is driven in process, not through a forked daemon, because the
 * budget has to be injectable and there is deliberately no public way to set it:
 * a flag or an environment variable for the watch budget would be a second
 * answer to a question the root-owned policy owns, reachable by anyone who can
 * start a daemon. `atlas_daemon_opts.watch_budget_total` and this suite are the
 * only callers, and an injected value replaces the resolved total and nothing
 * else — so every comparison, allocation round and counter below is the one
 * production runs.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/limits.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "ipc/server_internal.h"
#include "support/fixture.h"

#define WAIT_MS 20000

/* A live writer plus watcher over a fixture's data directory, with the watch
 * budget injected. */
typedef struct rig {
    fixture fx;
    atlas_db *db;
    atlas_writer *writer;
    atlas_watcher *watcher;
    atlas_buf db_path;
    FILE *log;
} rig;

static void rig_open(rig *r, atlas_err *err) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->db_path);
    T_OK(fx_open(&r->fx, err), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&r->fx), err), err);
    T_OK(atlas_datadir_db_path(fx_data_dir(&r->fx), &r->db_path, err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&r->db_path), &r->db, err), err);
    T_OK(atlas_db_migrate(r->db, err), err);
    /* The writer opens its own handle, so this one is closed before it starts:
     * exactly one process writes the index, and two open write handles in one
     * process is the shape that rule exists to prevent. */
    atlas_db_close(r->db);
    r->db = NULL;
    r->log = fopen("/dev/null", "we");
    T_REQUIRE(r->log != NULL);
}

/* Starts the writer and the watcher with whatever bounds the caller injected.
 *
 * `system_deployment` is false: a fixture daemon is never the system deployment,
 * whatever policy happens to exist on the machine running the tests. Every
 * `inject_` field left at zero is the production bound, so a test that sets one
 * differs from production in exactly that number. */
static void rig_start_writer(rig *r, atlas_err *err) {
    T_OK(atlas_writer_start(atlas_buf_cstr(&r->db_path), fx_data_dir(&r->fx), "", NULL, r->log,
                           &r->writer, err), err);
}

static void rig_start_watcher(rig *r, const atlas_watcher_opts *o, atlas_err *err) {
    T_OK(atlas_watcher_start(atlas_buf_cstr(&r->db_path), fx_data_dir(&r->fx), r->writer, r->log, o,
                              &r->watcher, err),
         err);
}

/* Split in two so a test can arm the writer's test channel in the gap between
 * them. Arming afterwards races the watcher's first publication, which is
 * exactly the publication some of these tests need to see fail. */
static void rig_start_opts(rig *r, const atlas_watcher_opts *o, atlas_err *err) {
    rig_start_writer(r, err);
    rig_start_watcher(r, o, err);
}

static void rig_start(rig *r, int64_t budget, atlas_err *err) {
    atlas_watcher_opts o;
    atlas_watcher_opts_init(&o);
    o.reconcile_interval_ms = 3600000;
    o.inject_budget_total = budget;
    rig_start_opts(r, &o, err);
}

static void rig_start_opts_default(rig *r, atlas_err *err) { rig_start(r, 0, err); }

static void rig_start_opts_budget(rig *r, int64_t budget, atlas_err *err) {
    rig_start(r, budget, err);
}

/* Stops the watcher and writer but keeps the fixture, so a test can re-start
 * against the same repositories with different bounds. */
static void rig_stop(rig *r) {
    if (r->watcher != NULL) {
        atlas_watcher_stop(r->watcher);
        r->watcher = NULL;
    }
    if (r->writer != NULL) {
        atlas_writer_stop(r->writer);
        r->writer = NULL;
    }
}

static void rig_close(rig *r) {
    if (r->watcher != NULL) {
        atlas_watcher_stop(r->watcher);
    }
    if (r->writer != NULL) {
        atlas_writer_stop(r->writer);
    }
    if (r->log != NULL) {
        (void)fclose(r->log);
    }
    atlas_buf_free(&r->db_path);
    fx_close(&r->fx);
}

/* Registers a repository through the CLI, which is the only thing that may. */
/* Holds the writer thread inside a reconciliation and waits until it is really
 * held. Arming alone blocks nothing — the stalled kind still has to be dequeued
 * — so a test that armed and then acted would be racing the very window it wants
 * to keep empty. One reconciliation is submitted explicitly rather than waiting
 * for the watcher to want one, because a quiescent repository may not ask for a
 * pass at all. */
static bool engage_writer_stall(rig *r) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_writer_test_stall(r->writer, ATLAS_JOB_RECONCILE);
    (void)atlas_writer_submit_reconcile(r->writer, 1, false, false, NULL, 0, NULL, &err);
    for (int i = 0; i < WAIT_MS / 20; i++) {
        if (atlas_writer_test_stalled(r->writer)) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

static void register_repo(rig *r, const char *path, const char *name, atlas_err *err) {
    const char *argv[] = {"--data-dir", fx_data_dir(&r->fx), "repo", "add", path, "--name", name};
    int code = -1;
    T_OK(fx_atlas(argv, 7u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);
}

/* Builds a repository with `dirs` watchable directories beneath its root, each
 * holding one committed file so git reports the tree rather than an empty one. */
static void build_tree(rig *r, const char *root, int dirs, atlas_err *err) {
    T_OK(fx_init_repo(&r->fx, root, NULL, err), err);
    for (int i = 0; i < dirs; i++) {
        char d[64];
        char f[96];
        (void)snprintf(d, sizeof(d), "d%04d", i);
        T_OK(fx_mkdir(root, d, err), err);
        (void)snprintf(f, sizeof(f), "%s/f.c", d);
        T_OK(fx_write(root, f, "1\n", err), err);
    }
    T_OK(fx_add_all(&r->fx, root, err), err);
    T_OK(fx_commit(&r->fx, root, "tree", err), err);
}

/* Waits until the watcher reports priming complete, or the deadline passes.
 * Never a fixed sleep: priming advances on the watcher's own tick and how many
 * ticks it takes is a property of the machine, not of Atlas. */
static bool wait_primed(rig *r, int budget_hint) {
    (void)budget_hint;
    for (int i = 0; i < WAIT_MS / 20; i++) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r->watcher, &ws);
        if (atlas_watcher_primed(r->watcher) && ws.priming_complete) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

static void read_state(rig *r, int64_t repo_id, atlas_index_state *out, atlas_err *err) {
    atlas_db *db = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&r->db_path), &db, err), err);
    T_OK(atlas_db_index_state_get(db, repo_id, out, err), err);
    atlas_db_close(db);
}

/* Waits for a repository's stored watch state to settle on something other than
 * `priming`, since the state is published through the writer queue. */
static bool wait_repo_settled(rig *r, int64_t repo_id, atlas_index_state *out) {
    for (int i = 0; i < WAIT_MS / 20; i++) {
        atlas_err err;
        atlas_err_init(&err);
        atlas_index_state_free(out);
        atlas_index_state_init(out);
        read_state(r, repo_id, out, &err);
        if (out->present && out->watch_state != ATLAS_WATCH_UNWATCHED &&
            out->watch_state != ATLAS_WATCH_PRIMING) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

/* Waits until the *stored* state carries `reason`.
 *
 * `wait_repo_settled` answers "has this repository reached some settled watch
 * state", which is the right question after a startup and the wrong one after a
 * transition: a repository that was already `watching` satisfies it on the first
 * read, before the degradation it is being watched for has reached the
 * database. The watcher publishes through the writer, so there is always a
 * window — and a test that read the row inside it got the state from before the
 * event and asserted against it.
 *
 * That is why `a temporary ignore failure recovers ...` failed about half the
 * time under ASan and passed under load-free release runs: it was a race the
 * test created, not one the watcher has. Measured before this existed: 3 of 6
 * standalone runs, and 3 of 3 while a full gate matrix competed for the machine.
 *
 * Returns false on timeout, so a reason that never arrives still fails the test
 * rather than passing it quietly. */
static bool wait_repo_reason(rig *r, int64_t repo_id, atlas_watch_reason reason,
                             atlas_index_state *out) {
    for (int i = 0; i < WAIT_MS / 20; i++) {
        atlas_err err;
        atlas_err_init(&err);
        atlas_index_state_free(out);
        atlas_index_state_init(out);
        read_state(r, repo_id, out, &err);
        if (out->present && out->watch_reason == reason) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

/* --- the vocabulary and the bounds ---------------------------------------- */

/* Every reason round-trips through its stored spelling, and no two share one.
 *
 * The names here are what migration 26's CHECK lists, so a member added to the
 * enum and not to the migration is refused by the database at the first write —
 * but only if somebody runs that path. This asserts the pairing directly. */
static void test_reason_vocabulary_round_trips(void) {
    static const atlas_watch_reason ALL[] = {
        ATLAS_WATCH_REASON_UNKNOWN,          ATLAS_WATCH_REASON_NONE,
        ATLAS_WATCH_REASON_KERNEL_LIMIT,     ATLAS_WATCH_REASON_REPO_BUDGET,
        ATLAS_WATCH_REASON_TOTAL_BUDGET,     ATLAS_WATCH_REASON_META_BUDGET,
        ATLAS_WATCH_REASON_DISCOVERY_BOUND,  ATLAS_WATCH_REASON_IGNORE_OVERFLOW,
        ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW, ATLAS_WATCH_REASON_FRONTIER_OVERFLOW,
        ATLAS_WATCH_REASON_REPO_LIMIT,       ATLAS_WATCH_REASON_ERROR,
    };
    size_t n = sizeof ALL / sizeof ALL[0];
    for (size_t i = 0; i < n; i++) {
        const char *name = atlas_watch_reason_name(ALL[i]);
        T_CHECK_MSG(atlas_watch_reason_parse(name) == ALL[i], "reason %s does not round-trip",
                    name);
        T_CHECK_MSG(atlas_watch_reason_explain(ALL[i]) != NULL, "reason %s has no sentence", name);
        for (size_t k = 0; k < i; k++) {
            T_CHECK_MSG(strcmp(name, atlas_watch_reason_name(ALL[k])) != 0,
                        "two reasons share the spelling %s", name);
        }
    }
    /* Zero is UNKNOWN, and an unrecognised spelling reads as UNKNOWN rather than
     * as the nearest match: a newer daemon's reason arriving at an older client
     * must claim less, not something else. */
    T_EQ_INT((int)ATLAS_WATCH_REASON_UNKNOWN, 0);
    T_CHECK(atlas_watch_reason_parse("total_budget_v2") == ATLAS_WATCH_REASON_UNKNOWN);
    T_CHECK(atlas_watch_reason_parse(NULL) == ATLAS_WATCH_REASON_UNKNOWN);
    /* `priming` is a stored watch state and must round-trip too, or a migrated
     * row would read back as `unwatched`. */
    T_CHECK(atlas_watch_state_parse("priming") == ATLAS_WATCH_PRIMING);
    T_CHECK(strcmp(atlas_watch_state_name(ATLAS_WATCH_PRIMING), "priming") == 0);
}

/* The bounds have to be consistent with each other and with the types that hold
 * them. Each of these was a real hazard in review rather than a hypothetical:
 * `sub_count` is a uint16_t, and the metadata reserve is multiplied by the
 * repository count. */
static void test_bounds_are_mutually_consistent(void) {
    /* Every subscriber of one descriptor is a distinct repository, and the count
     * is held in a uint16_t. */
    T_CHECK_MSG(ATLAS_WATCH_MAX_REPOS <= 65535u,
                "the repository ceiling must fit the type that counts subscribers");
    /* The metadata reserve is held back from the source pool for every
     * repository at once. If the product could reach the ceiling the source pool
     * would be zero and no repository would ever be watched. */
    T_CHECK_MSG((uint64_t)ATLAS_WATCH_MAX_REPOS * ATLAS_WATCH_META_RESERVE_PER_REPO <
                    (uint64_t)ATLAS_WATCH_DIRS_HARD_CEILING,
                "the metadata reserve for every repository must leave a source pool");
    /* The reserve is a floor and the maximum is a ceiling; swapping them would
     * silently cap metadata at the reserve, which is the defect this pair
     * replaced. */
    T_CHECK(ATLAS_WATCH_META_RESERVE_PER_REPO <= ATLAS_WATCH_META_MAX_PER_REPO);
    /* A derived default may never exceed what a policy may ask for. */
    T_CHECK(ATLAS_WATCH_TOTAL_SOFT_MAX <= ATLAS_WATCH_DIRS_HARD_CEILING);
    T_CHECK(ATLAS_WATCH_TOTAL_MIN < ATLAS_WATCH_TOTAL_SOFT_MAX);
    /* The proven envelope is a claim about what was measured. It must be
     * reachable within the ceiling, and it must not be confused with it: the
     * ceiling is a refusal point, the envelope is a support statement. */
    T_CHECK(ATLAS_WATCH_PROVEN_ENVELOPE_DIRS <= ATLAS_WATCH_DIRS_HARD_CEILING);
    /* The walk's visit bound is derived from the budget rather than sharing a
     * constant with it, so "walked too far" and "ran out of watches" cannot be
     * the same answer. */
    T_CHECK(ATLAS_WATCH_DISCOVER_FACTOR >= 1u);
}

/* --- the root-owned policy ------------------------------------------------ */

static void write_policy(const fixture *fx, const char *body, atlas_buf *path_out,
                         atlas_err *err) {
    T_OK(atlas_buf_set_str(path_out, fx_data_dir(fx), err), err);
    T_OK(atlas_buf_append_str(path_out, "/system.conf", err), err);
    FILE *f = fopen(atlas_buf_cstr(path_out), "we");
    T_REQUIRE(f != NULL);
    (void)fputs(body, f);
    (void)fclose(f);
}

/* Absent, valid, malformed, below the minimum and above the ceiling.
 *
 * Split into two halves for a reason worth stating, because it is a limit on
 * what this suite can prove rather than a shortcut.
 *
 * The *value* half is asked of `atlas_syspolicy_watch_budget_in_range`, the pure
 * predicate the parser asks. It has to be, because no unprivileged uid can
 * produce a root-owned policy at any path on the filesystem — that is exactly
 * what `test_a71_syspolicy` proves — so a test cannot get far enough into the
 * parser to see a value be refused. The predicate grants nothing and is not a
 * bypass: it answers a question about a number.
 *
 * The *authority* half is asked of the real loader against a fixture-written
 * file, and asserts the property that actually matters: a policy this uid wrote
 * sets no watch budget, because it is not honoured at all.
 *
 * Out of range is refused rather than clamped, for `client_uid`'s reason: an
 * operator whose number Atlas silently replaced cannot read back what they
 * configured. And a refusal is not local to the key — it drops the whole policy
 * to legacy mode, which on a system deployment removes every client uid from the
 * socket. That is why this change deploys binary first and policy second. */
static void test_policy_watch_budget_is_range_checked(void) {
    /* Values, against the predicate the parser uses. */
    struct {
        long long v;
        bool ok;
        const char *what;
    } VALUES[] = {
        {(long long)ATLAS_WATCH_TOTAL_MIN, true, "exactly the minimum is allowed"},
        {(long long)ATLAS_WATCH_TOTAL_MIN - 1, false, "one below the minimum is refused"},
        {(long long)ATLAS_WATCH_DIRS_HARD_CEILING, true, "exactly the ceiling is allowed"},
        {(long long)ATLAS_WATCH_DIRS_HARD_CEILING + 1, false, "one above the ceiling is refused"},
        {40000, true, "an ordinary value is allowed"},
        {0, false, "zero is not a budget; absent is expressed by omitting the key"},
        {-1, false, "a negative budget is refused"},
    };
    for (size_t i = 0; i < sizeof VALUES / sizeof VALUES[0]; i++) {
        T_CHECK_MSG(atlas_syspolicy_watch_budget_in_range(VALUES[i].v) == VALUES[i].ok, "%s",
                    VALUES[i].what);
    }

    /* Authority, against the real loader. */
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    atlas_buf path = ATLAS_BUF_INIT;

    static const char *const BODIES[] = {
        /* Absent from an otherwise well-formed policy. */
        "socket_path = /run/atlas/atlas.sock\ndata_dir = /var/lib/atlas\n",
        /* A perfectly well-formed budget — the file an unprivileged attacker
         * would write to give Atlas a budget of their choosing. */
        "socket_path = /run/atlas/atlas.sock\ndata_dir = /var/lib/atlas\n"
        "watch_max_dirs_total = 40000\n",
        /* Not a number. */
        "socket_path = /run/atlas/atlas.sock\ndata_dir = /var/lib/atlas\n"
        "watch_max_dirs_total = lots\n",
        /* Below the minimum, and above the ceiling. */
        "socket_path = /run/atlas/atlas.sock\ndata_dir = /var/lib/atlas\n"
        "watch_max_dirs_total = 4\n",
        "socket_path = /run/atlas/atlas.sock\ndata_dir = /var/lib/atlas\n"
        "watch_max_dirs_total = 999999999\n",
    };
    for (size_t i = 0; i < sizeof BODIES / sizeof BODIES[0]; i++) {
        write_policy(&fx, BODIES[i], &path, &err);
        atlas_syspolicy pol;
        atlas_syspolicy_load_at(atlas_buf_cstr(&path), &pol);
        T_CHECK_MSG(pol.state != ATLAS_SYSPOLICY_SYSTEM,
                    "a policy written by this uid must never reach system mode (case %zu)", i);
        T_CHECK_MSG(atlas_syspolicy_watch_max_dirs_total(&pol) == 0,
                    "a policy written by this uid must set no watch budget (case %zu)", i);
    }

    /* The share is a property of the deployment shape, not a new setting: a
     * dedicated `atlasd` has no other consumer of its inotify budget, while a
     * per-user daemon shares the uid with the operator's editors and language
     * servers. A zeroed policy is the per-user case. */
    atlas_syspolicy none;
    memset(&none, 0, sizeof(none));
    atlas_syspolicy sys;
    memset(&sys, 0, sizeof(sys));
    sys.state = ATLAS_SYSPOLICY_SYSTEM;
    T_CHECK(atlas_syspolicy_watch_kernel_share_pct(&sys) == ATLAS_WATCH_KERNEL_SHARE_PCT_SYSTEM);
    T_CHECK(atlas_syspolicy_watch_kernel_share_pct(&none) == ATLAS_WATCH_KERNEL_SHARE_PCT_USER);
    T_CHECK(atlas_syspolicy_watch_kernel_share_pct(NULL) == ATLAS_WATCH_KERNEL_SHARE_PCT_USER);
    T_CHECK_MSG(atlas_syspolicy_watch_kernel_share_pct(&none) <
                    atlas_syspolicy_watch_kernel_share_pct(&sys),
                "a shared uid must claim less of the kernel's watches than a dedicated one");
    /* And an unset budget is "no statement", so the caller derives one. */
    T_CHECK(atlas_syspolicy_watch_max_dirs_total(&none) == 0);
    T_CHECK(atlas_syspolicy_watch_max_dirs_total(NULL) == 0);

    atlas_buf_free(&path);
    fx_close(&fx);
}

/* --- the exact boundary --------------------------------------------------- */

/* With a budget of N, exactly N physical descriptors are held and no more.
 *
 * The old comparison was `count + 1 >= LIMIT`, so a documented 8192 was in fact
 * 8191 — and the daemon reported exactly 8191 in production for weeks without
 * anybody reading it as a bug. This asserts the equality, not an inequality:
 * `<= N` would have passed against the defect.
 */
static void test_exact_boundary_at_the_daemon_total(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    /* Above the metadata reserve, because a budget below it is a broken
     * deployment rather than a boundary worth pinning: metadata would take
     * what there is and source would get nothing, which is reported honestly
     * but tests nothing about the source bound. Small enough to reach quickly
     * all the same. */
    const int64_t BUDGET = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO + 64;
    rig_open(&r, &err);
    /* Far more directories than the budget, so the bound is certain to bind. */
    build_tree(&r, fx_repo(&r.fx), 200, &err);
    register_repo(&r, fx_repo(&r.fx), "wide", &err);
    rig_start(&r, BUDGET, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK_MSG(wait_repo_settled(&r, 1, &s), "the repository never reached a settled watch state");

    atlas_watch_stats ws;
    atlas_watcher_stats(r.watcher, &ws);
    T_CHECK_MSG(ws.budget_total == BUDGET, "the injected budget must be the resolved one");
    /* The exact boundary is on the *source* bound, not on the daemon total, and
     * the difference is the metadata reserve — which source may not spend.
     *
     * With one repository the arithmetic is exact: the pool is the budget less
     * what metadata is owed, so source installs precisely `BUDGET - RESERVE`
     * watches and stops. Asserting equality rather than a bound is the point:
     * `<=` would have passed against the `+ 1 >=` comparison that made a
     * documented ceiling of 8192 mean 8191. */
    const int64_t expect_source = BUDGET - (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO;
    T_CHECK_MSG(s.watched_source == expect_source,
                "expected exactly %lld source watches at a budget of %lld with a reserve of %u, "
                "got %lld",
                (long long)expect_source, (long long)BUDGET,
                ATLAS_WATCH_META_RESERVE_PER_REPO, (long long)s.watched_source);
    /* And nothing was installed past the budget itself. */
    T_CHECK_MSG(ws.watches <= BUDGET, "the daemon must never hold more than its budget: %lld > %lld",
                (long long)ws.watches, (long long)BUDGET);

    /* And it is told which bound stopped it. `total_budget`, not `repo_budget`:
     * the daemon's budget is spent, and reporting that as the repository's own
     * share is how a repository late in `ORDER BY name` used to be told it was
     * too big. */
    T_CHECK_MSG(s.watch_state == ATLAS_WATCH_DEGRADED, "expected a degraded repository, got %s",
                atlas_watch_state_name(s.watch_state));
    T_CHECK_MSG(s.watch_reason == ATLAS_WATCH_REASON_TOTAL_BUDGET,
                "expected total_budget, got %s", atlas_watch_reason_name(s.watch_reason));
    /* A degraded watcher may have missed changes, so the index is not current. */
    T_CHECK(s.event_gap);
    T_CHECK(!atlas_index_state_is_current(&s, NULL));
    /* The count is written on the degraded path too. Before P0 it was not, so a
     * degraded repository reported whatever it had held the last time it was
     * healthy. */
    T_CHECK_MSG(s.watched_dirs == s.watched_source + s.watched_meta,
                "watched_dirs must be the sum of its parts");
    T_CHECK_MSG(s.watched_dirs > 0, "a degraded repository still reports what it holds");

    atlas_index_state_free(&s);
    rig_close(&r);
}

/* Metadata is installed before any source directory, and out of its own
 * reserve.
 *
 * Before P0 the metadata watches went in *after* the recursive source walk, so a
 * repository large enough to exhaust the budget stopped watching its own HEAD
 * and `refs/` — branch correctness was contingent on the source tree fitting.
 * With a budget far smaller than the tree, the metadata watches must still all
 * be there. */
static void test_metadata_is_installed_before_source(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    const int64_t BUDGET = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO + 8;
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 200, &err);
    register_repo(&r, fx_repo(&r.fx), "tiny-budget", &err);
    rig_start(&r, BUDGET, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));

    T_CHECK_MSG(s.watched_meta > 0,
                "a repository whose source tree does not fit must still watch its git metadata");
    /* The git directory, the refs tree and info/ are all metadata; the exact
     * count depends on how git laid the repository out, so the assertion is a
     * floor. What matters is that metadata was not starved by source. */
    T_CHECK_MSG(s.watched_meta >= 2,
                "expected at least the git directory and one refs directory, got %lld",
                (long long)s.watched_meta);

    atlas_index_state_free(&s);
    rig_close(&r);
}

/* --- two repositories ----------------------------------------------------- */

/* Two repositories, each below any single one's share, whose sum exceeds what
 * either could take alone: both are watched completely, and neither is degraded.
 *
 * This is the production failure in miniature. With a global bound enforced as
 * though it were per-repository, the repository that sorted later took what was
 * left and was told it was too big. The assertion that matters is that *neither*
 * is degraded and that the one that sorts last is complete. */
static void test_two_repositories_do_not_consume_each_others_budget(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    const int64_t BUDGET = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO * 2 + 200;
    rig_open(&r, &err);

    atlas_buf second = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&second, &err, "%s/second", atlas_buf_cstr(&r.fx.root)), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&r.fx.root), "second", &err), &err);

    /* Named so the alphabetical order the old code depended on is exercised:
     * "aaa" sorts first, "zzz" last. Under the defect, "zzz" was the one that
     * lost. */
    build_tree(&r, fx_repo(&r.fx), 30, &err);
    build_tree(&r, atlas_buf_cstr(&second), 30, &err);
    register_repo(&r, fx_repo(&r.fx), "aaa", &err);
    register_repo(&r, atlas_buf_cstr(&second), "zzz", &err);
    rig_start(&r, BUDGET, &err);

    T_CHECK_MSG(wait_primed(&r, (int)BUDGET), "priming never completed");

    for (int64_t id = 1; id <= 2; id++) {
        atlas_index_state s;
        atlas_index_state_init(&s);
        T_CHECK(wait_repo_settled(&r, id, &s));
        T_CHECK_MSG(s.watch_state == ATLAS_WATCH_WATCHING,
                    "repository %lld expected watching, got %s (%s)", (long long)id,
                    atlas_watch_state_name(s.watch_state),
                    atlas_watch_reason_name(s.watch_reason));
        T_CHECK_MSG(s.watch_reason == ATLAS_WATCH_REASON_NONE,
                    "a complete watch set must carry no degradation reason");
        T_CHECK_MSG(s.watched_source >= 31,
                    "repository %lld expected its whole tree watched, got %lld source watches",
                    (long long)id, (long long)s.watched_source);
        atlas_index_state_free(&s);
    }

    /* A single large repository is not capped below the pool: the per-repository
     * ceiling is the whole budget, and sharing is done by the round. */
    atlas_watch_stats ws;
    atlas_watcher_stats(r.watcher, &ws);
    T_CHECK_MSG(ws.budget_repo == ws.budget_total,
                "one repository must be able to use the whole budget when nothing else wants it");

    atlas_buf_free(&second);
    rig_close(&r);
}

/* --- physical descriptors versus logical subscriptions --------------------- */

/* Two registered worktrees of one repository share every descriptor on their
 * common git directory. The per-repository counts therefore sum to *more* than
 * the daemon's physical count, and that is correct rather than an error.
 *
 * The claim withdrawn during review was "the per-repository counts sum to the
 * daemon total". It is false for exactly this case, which is why the relation
 * asserted here is `>=`, together with the field that explains the difference. */
static void test_shared_descriptors_are_counted_once_by_the_kernel(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);

    build_tree(&r, fx_repo(&r.fx), 4, &err);
    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/wt2", atlas_buf_cstr(&r.fx.root)), &err);
    const char *addwt[] = {"worktree", "add", "-b", "second", atlas_buf_cstr(&wt)};
    atlas_err werr;
    atlas_err_init(&werr);
    if (fx_git_ok(&r.fx, fx_repo(&r.fx), addwt, 5u, &werr) != ATLAS_OK) {
        atlas_test_note("this git cannot create a linked worktree; skipping");
        atlas_buf_free(&wt);
        rig_close(&r);
        return;
    }
    register_repo(&r, fx_repo(&r.fx), "main-wt", &err);
    register_repo(&r, atlas_buf_cstr(&wt), "second-wt", &err);
    rig_start(&r, 0, &err);
    T_CHECK(wait_primed(&r, 0));

    atlas_index_state a;
    atlas_index_state b;
    atlas_index_state_init(&a);
    atlas_index_state_init(&b);
    T_CHECK(wait_repo_settled(&r, 1, &a));
    T_CHECK(wait_repo_settled(&r, 2, &b));

    atlas_watch_stats ws;
    atlas_watcher_stats(r.watcher, &ws);

    /* Both worktrees watch the shared common directory and its refs tree. */
    T_CHECK_MSG(a.watched_meta > 0 && b.watched_meta > 0,
                "both worktrees must hold metadata subscriptions");
    T_CHECK_MSG(a.watched_shared > 0 || b.watched_shared > 0,
                "two worktrees of one repository must share at least one descriptor");

    /* The relation, in both directions. */
    int64_t summed = a.watched_dirs + b.watched_dirs;
    T_CHECK_MSG(summed == ws.subscriptions,
                "the per-repository counts must sum to the subscription total: %lld vs %lld",
                (long long)summed, (long long)ws.subscriptions);
    T_CHECK_MSG(ws.subscriptions >= ws.watches,
                "subscriptions must never be fewer than physical descriptors");
    T_CHECK_MSG(ws.subscriptions > ws.watches,
                "with a shared git directory the two must actually differ, or this test is not "
                "exercising sharing at all");

    atlas_index_state_free(&a);
    atlas_index_state_free(&b);
    atlas_buf_free(&wt);
    rig_close(&r);
}

/* Removing one worktree must not take the other's shared descriptors with it.
 *
 * The old code released the kernel descriptor on behalf of whichever repository
 * asked first, so the survivor believed it was watching something the kernel had
 * already dropped — and silently stopped seeing branch updates. */
static void test_removing_one_worktree_leaves_the_others_watches(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);

    build_tree(&r, fx_repo(&r.fx), 3, &err);
    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/wt2", atlas_buf_cstr(&r.fx.root)), &err);
    const char *addwt[] = {"worktree", "add", "-b", "second", atlas_buf_cstr(&wt)};
    atlas_err werr;
    atlas_err_init(&werr);
    if (fx_git_ok(&r.fx, fx_repo(&r.fx), addwt, 5u, &werr) != ATLAS_OK) {
        atlas_test_note("this git cannot create a linked worktree; skipping");
        atlas_buf_free(&wt);
        rig_close(&r);
        return;
    }
    register_repo(&r, fx_repo(&r.fx), "keeper", &err);
    register_repo(&r, atlas_buf_cstr(&wt), "goer", &err);
    rig_start(&r, 0, &err);
    T_CHECK(wait_primed(&r, 0));

    atlas_index_state before;
    atlas_index_state_init(&before);
    T_CHECK(wait_repo_settled(&r, 1, &before));
    int64_t meta_before = before.watched_meta;
    T_CHECK(meta_before > 0);

    /* Remove the second registration. The watcher rebuilds on the repository-set
     * change; the survivor must come back with its metadata intact. */
    const char *rm[] = {"--data-dir", fx_data_dir(&r.fx), "repo", "remove", "goer", "--yes"};
    int code = -1;
    atlas_buf out = ATLAS_BUF_INIT;
    (void)fx_atlas(rm, 6u, &out, NULL, &code, &err);
    atlas_buf_free(&out);

    bool ok = false;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        atlas_index_state s;
        atlas_index_state_init(&s);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r, 1, &s, &rerr);
        if (s.present && s.watched_meta >= meta_before) {
            ok = true;
        }
        atlas_index_state_free(&s);
        if (ok) {
            break;
        }
        usleep(50000);
    }
    T_CHECK_MSG(ok,
                "the surviving worktree must keep its shared metadata watches after its sibling "
                "was removed");

    atlas_index_state_free(&before);
    atlas_buf_free(&wt);
    rig_close(&r);
}

/* --- the metadata reserve is a floor, not a cap ---------------------------- */

/* A repository with more metadata directories than the reserve installs all of
 * them, drawing the remainder from the pool.
 *
 * The review that produced this design rejected an earlier one in which 256 was
 * a hard cap, on the grounds that it recreated the 8192 mistake one layer down:
 * a number chosen from today's repositories, applied to a resource whose demand
 * is not bounded by them. `refs/` is hierarchical, so a repository with many
 * branches under many prefixes has as many metadata directories as it has
 * prefixes. */
static void test_metadata_beyond_the_reserve_completes(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);

    /* A hierarchical refs tree well past the reserve. Written directly rather
     * than through `git branch`, because what is under test is the watcher's
     * accounting over a directory shape, not git's ref storage. */
    const int REFS = (int)ATLAS_WATCH_META_RESERVE_PER_REPO + 64;
    for (int i = 0; i < REFS; i++) {
        char d[128];
        (void)snprintf(d, sizeof(d), ".git/refs/heads/p%04d", i);
        T_OK(fx_mkdir(fx_repo(&r.fx), d, &err), &err);
    }
    register_repo(&r, fx_repo(&r.fx), "manyrefs", &err);
    rig_start(&r, 0, &err);
    T_CHECK(wait_primed(&r, 0));

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));

    T_CHECK_MSG(s.watch_state == ATLAS_WATCH_WATCHING,
                "a repository with more refs directories than the reserve must still complete, "
                "got %s (%s)",
                atlas_watch_state_name(s.watch_state),
                atlas_watch_reason_name(s.watch_reason));
    T_CHECK_MSG(s.watched_meta > (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO,
                "expected more than the reserve of %u metadata watches, got %lld — the reserve is "
                "a floor, not a cap",
                ATLAS_WATCH_META_RESERVE_PER_REPO, (long long)s.watched_meta);

    atlas_index_state_free(&s);
    rig_close(&r);
}

/* And the reserve is genuinely held back: a source tree far larger than the
 * budget cannot spend what metadata is owed.
 *
 * Meta-first ordering alone does not give this. It covers the initial build and
 * nothing after it — a `.git/info` created live, a new refs prefix pushed an
 * hour later. Without a reserve those hit `meta_budget` on a daemon whose source
 * rounds have taken everything, and branch correctness would again depend on the
 * source tree fitting, just later. */
static void test_the_metadata_reserve_survives_a_hungry_source_tree(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    /* Enough for the metadata reserve plus a little, and far less than the tree
     * wants. */
    const int64_t BUDGET = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO + 32;
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 400, &err);
    register_repo(&r, fx_repo(&r.fx), "hungry", &err);
    rig_start(&r, BUDGET, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));

    atlas_watch_stats ws;
    atlas_watcher_stats(r.watcher, &ws);
    /* The source walk stopped, as it must. What matters is where it stopped:
     * with the reserve still unspent, so metadata appearing later can be
     * watched. */
    int64_t headroom = ws.budget_total - ws.watches;
    int64_t reserve_left = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO - s.watched_meta;
    if (reserve_left < 0) {
        reserve_left = 0;
    }
    T_CHECK_MSG(headroom >= reserve_left,
                "the source walk spent the metadata reserve: budget %lld, held %lld, metadata "
                "owed %lld",
                (long long)ws.budget_total, (long long)ws.watches, (long long)reserve_left);

    atlas_index_state_free(&s);
    rig_close(&r);
}

/* The exact boundary at `ATLAS_WATCH_META_MAX_PER_REPO`.
 *
 * The reserve has a floor test above; this is its ceiling. A repository with
 * more metadata directories than Atlas will install gets exactly the maximum and
 * is told `meta_budget` — not `total_budget`, because the daemon has budget
 * left, and not silence, because a repository watching only part of its refs is
 * one whose branch updates are partly unobserved.
 *
 * Asserted as equality for the reason every boundary here is: `<=` would pass
 * against an off-by-one, which is the defect that started this season. */
static void test_exact_boundary_at_the_metadata_maximum(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);

    /* One more refs directory than the ceiling allows. Written directly rather
     * than through `git branch`: what is under test is the watcher's accounting
     * over a directory shape, not git's ref storage. */
    const int REFS = (int)ATLAS_WATCH_META_MAX_PER_REPO + 16;
    for (int i = 0; i < REFS; i++) {
        char d[128];
        (void)snprintf(d, sizeof(d), ".git/refs/heads/p%05d", i);
        T_OK(fx_mkdir(fx_repo(&r.fx), d, &err), &err);
    }
    register_repo(&r, fx_repo(&r.fx), "toomanyrefs", &err);
    /* A budget with room to spare, so the only bound that can bind is the
     * metadata ceiling. */
    rig_start(&r, (int64_t)ATLAS_WATCH_META_MAX_PER_REPO * 4, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));

    T_CHECK_MSG(s.watched_meta == (int64_t)ATLAS_WATCH_META_MAX_PER_REPO,
                "expected exactly %u metadata watches at the ceiling, got %lld",
                ATLAS_WATCH_META_MAX_PER_REPO, (long long)s.watched_meta);
    T_CHECK_MSG(s.watch_reason == ATLAS_WATCH_REASON_META_BUDGET,
                "expected meta_budget, got %s — the daemon has budget left, so this is not a "
                "total_budget refusal",
                atlas_watch_reason_name(s.watch_reason));
    T_CHECK_MSG(s.watch_state == ATLAS_WATCH_DEGRADED,
                "a repository whose refs are only partly watched is degraded");
    T_CHECK_MSG(s.event_gap, "and cannot claim to have observed every change");

    atlas_index_state_free(&s);
    rig_close(&r);
}

/* --- the kernel's own limit, reported as itself ---------------------------- */

/* `ENOSPC` from `inotify_add_watch` is `kernel_limit`, not a budget.
 *
 * The distinction is the operator's remedy: one is `fs.inotify.max_user_watches`
 * and the other is an Atlas setting, and before P0 both produced one of two
 * sentences with no way to tell which had happened.
 *
 * The uid's budget is exhausted from inside the test rather than by lowering the
 * sysctl, which needs root. Watches are counted per uid across every inotify
 * instance, so the same handful of directories added to many instances consumes
 * the budget without creating a hundred thousand directories. Everything is
 * released at the end, and the suite is `RUN_SERIAL` because a test that
 * deliberately exhausts a machine-wide resource must not run beside one that
 * needs it. */
static void test_kernel_enospc_is_reported_as_the_kernels(void) {
    long kmax = 0;
    {
        FILE *f = fopen("/proc/sys/fs/inotify/max_user_watches", "re");
        if (f != NULL) {
            if (fscanf(f, "%ld", &kmax) != 1) {
                kmax = 0;
            }
            (void)fclose(f);
        }
    }
    if (kmax <= 0 || kmax > 200000) {
        atlas_test_note("the kernel watch limit is absent or impractically large; skipping");
        return;
    }

    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 8, &err);
    register_repo(&r, fx_repo(&r.fx), "starved", &err);

    /* Directories to soak up the budget with. */
    const int SOAK_DIRS = 1400;
    for (int i = 0; i < SOAK_DIRS; i++) {
        char d[64];
        (void)snprintf(d, sizeof(d), "soak%04d", i);
        T_OK(fx_mkdir(atlas_buf_cstr(&r.fx.root), d, &err), &err);
    }

    int fds[120];
    size_t nfds = 0;
    long consumed = 0;
    for (size_t k = 0; k < sizeof fds / sizeof fds[0] && consumed < kmax; k++) {
        int fd = inotify_init1(IN_CLOEXEC);
        if (fd < 0) {
            break; /* out of instances; whatever we hold is what we hold */
        }
        fds[nfds++] = fd;
        for (int i = 0; i < SOAK_DIRS && consumed < kmax; i++) {
            char p[512];
            (void)snprintf(p, sizeof(p), "%s/soak%04d", atlas_buf_cstr(&r.fx.root), i);
            if (inotify_add_watch(fd, p, IN_ATTRIB) < 0) {
                consumed = kmax; /* the kernel is already refusing: that is the point */
                break;
            }
            consumed++;
        }
    }

    bool starved = consumed >= kmax;
    if (!starved) {
        atlas_test_note("could not exhaust this uid's inotify budget; skipping the ENOSPC case");
    } else {
        /* A budget far larger than the kernel will now grant, so the refusal can
         * only come from the kernel. */
        rig_start(&r, kmax * 2, &err);
        atlas_index_state s;
        atlas_index_state_init(&s);
        T_CHECK(wait_repo_settled(&r, 1, &s));
        T_CHECK_MSG(s.watch_reason == ATLAS_WATCH_REASON_KERNEL_LIMIT,
                    "expected kernel_limit when the kernel refuses, got %s",
                    atlas_watch_reason_name(s.watch_reason));
        T_CHECK_MSG(s.watch_state == ATLAS_WATCH_DEGRADED, "a starved watcher is degraded");
        T_CHECK_MSG(s.event_gap, "and cannot claim to have observed everything");
        /* The sentence must send the operator to the sysctl, not to an Atlas
         * setting: that is the whole reason this reason exists separately. */
        const char *why = atlas_watch_reason_explain(ATLAS_WATCH_REASON_KERNEL_LIMIT);
        T_CHECK_MSG(strstr(why, "max_user_watches") != NULL,
                    "the kernel_limit sentence must name the sysctl, not an Atlas setting");
        atlas_index_state_free(&s);
    }

    for (size_t k = 0; k < nfds; k++) {
        (void)close(fds[k]);
    }
    rig_close(&r);
}

/* --- the rebuild window is an event gap ------------------------------------ */

/* A repository-set change drops every watch and reinstalls it. Events in that
 * window are gone — the kernel had no descriptor to report them on — and before
 * P0 nothing recorded it: the repository came out of the rebuild reported as
 * `watching`, with a hole no later pass had any reason to look for.
 *
 * So every repository is owed a content-verifying pass afterwards, including the
 * ones that were not the reason for the rebuild. */
static void test_a_repo_set_change_gaps_every_repository(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);

    atlas_buf second = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&second, &err, "%s/second", atlas_buf_cstr(&r.fx.root)), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&r.fx.root), "second", &err), &err);
    build_tree(&r, fx_repo(&r.fx), 3, &err);
    build_tree(&r, atlas_buf_cstr(&second), 3, &err);

    register_repo(&r, fx_repo(&r.fx), "first", &err);
    rig_start(&r, 0, &err);
    T_CHECK(wait_primed(&r, 0));

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));

    /* Register the second repository, and then raise the flag the writer raises
     * on a repository-set change.
     *
     * Raised directly rather than by routing `repo add` through a daemon,
     * because this rig has no socket: it drives the writer and the watcher in
     * process, so a CLI `repo add` writes the row locally and nothing tells the
     * watcher. `atlas_writer_set_watch_dirty` is the *same* call the writer's
     * own repo-add job makes, and what is under test is what the watcher does
     * when it sees it — not how the flag got there. */
    register_repo(&r, atlas_buf_cstr(&second), "arrival", &err);
    atlas_writer_set_watch_dirty(r.writer);

    bool gapped = false;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        atlas_index_state g;
        atlas_index_state_init(&g);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r, 1, &g, &rerr);
        if (g.present && (g.event_gap || g.pending_full_reconcile)) {
            gapped = true;
        }
        atlas_index_state_free(&g);
        if (gapped) {
            break;
        }
        usleep(50000);
    }
    T_CHECK_MSG(gapped,
                "a repository that was already watched must be gapped when the watch set is "
                "rebuilt around it: events during the rebuild were not observed");

    atlas_index_state_free(&s);
    atlas_buf_free(&second);
    rig_close(&r);
}

/* --- the repository ceiling ------------------------------------------------ */

/* Exactly `N` repositories are observed and the `N+1`th is told `REPO_LIMIT`.
 *
 * Driven with an injected ceiling rather than 257 real git repositories: the
 * comparison, the accounting and the reason code are production's, and only the
 * number differs. The compiled 256 is asserted separately, by range, in
 * `test_bounds_are_mutually_consistent` — a small injected bound proves the
 * behaviour, and the static assertion proves the shipped value.
 *
 * The `N+1`th is **reported**, never silently dropped. That is the whole point:
 * a repository nobody is watching and nobody was told about is exactly the state
 * this season exists to make impossible. */
static void test_exact_boundary_at_the_repository_ceiling(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);

    const size_t LIMIT = 3;
    const size_t TOTAL = LIMIT + 1;
    /* Named so registration order (`ORDER BY name`) is r0..r3 and the last one
     * is the one past the ceiling. */
    for (size_t i = 0; i < TOTAL; i++) {
        char sub[32];
        char name[32];
        (void)snprintf(sub, sizeof(sub), "r%zu", i);
        (void)snprintf(name, sizeof(name), "r%zu", i);
        atlas_buf path = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&path, &err, "%s/%s", atlas_buf_cstr(&r.fx.root), sub), &err);
        T_OK(fx_mkdir(atlas_buf_cstr(&r.fx.root), sub, &err), &err);
        build_tree(&r, atlas_buf_cstr(&path), 2, &err);
        register_repo(&r, atlas_buf_cstr(&path), name, &err);
        atlas_buf_free(&path);
    }

    atlas_watcher_opts o;
    atlas_watcher_opts_init(&o);
    o.reconcile_interval_ms = 3600000;
    o.inject_max_repos = LIMIT;
    rig_start_opts(&r, &o, &err);

    size_t watched = 0;
    size_t refused = 0;
    for (int64_t id = 1; id <= (int64_t)TOTAL; id++) {
        atlas_index_state st;
        atlas_index_state_init(&st);
        T_CHECK_MSG(wait_repo_settled(&r, id, &st), "repository %lld never settled", (long long)id);
        if (st.watch_reason == ATLAS_WATCH_REASON_REPO_LIMIT) {
            refused++;
            T_CHECK_MSG(st.watch_state == ATLAS_WATCH_DEGRADED,
                        "a repository past the ceiling is degraded, not silently unwatched");
            T_CHECK_MSG(st.event_gap,
                        "and carries an event gap: nothing is observing it, so nothing may call "
                        "its index current");
            T_CHECK_MSG(!atlas_index_state_is_current(&st, NULL),
                        "an unobserved repository is never current");
            T_CHECK_MSG(st.watched_source == 0 && st.watched_meta == 0,
                        "a repository past the ceiling holds no watches at all, got %lld/%lld",
                        (long long)st.watched_source, (long long)st.watched_meta);
        } else {
            watched++;
            T_CHECK_MSG(st.watch_state == ATLAS_WATCH_WATCHING,
                        "repository %lld expected watching, got %s (%s)", (long long)id,
                        atlas_watch_state_name(st.watch_state),
                        atlas_watch_reason_name(st.watch_reason));
        }
        atlas_index_state_free(&st);
    }
    T_CHECK_MSG(watched == LIMIT, "expected exactly %zu observed repositories, got %zu", LIMIT,
                watched);
    T_CHECK_MSG(refused == TOTAL - LIMIT, "expected exactly %zu refused, got %zu", TOTAL - LIMIT,
                refused);

    rig_close(&r);
}

/* --- the pending-ignore queue ---------------------------------------------- */

/* The pending-ignore queue's bound, from both sides.
 *
 * Injected rather than produced by creating 4096 directories inside one debounce
 * window. The queue code under test is production's — the same
 * `queue_pending_ignore`, the same counters, the same reason code.
 *
 * Why this is two phases rather than "N then N+1": the watcher does not wait out
 * its poll timeout before acting. It wakes on the *first* inotify event, drains
 * what is readable, and resolves the queue in the same loop iteration — so five
 * `mkdir`s issued back to back are usually handled in five separate iterations
 * and never sit in the queue together. Asserting "the N+1th overflows" against
 * that is asserting a race, and a test that passes because the scheduler
 * cooperated is not evidence.
 *
 * So the bound is pinned from both directions, each deterministically:
 *
 *   at or below N  - can never overflow, whatever the batching, and every
 *                    directory is resolved and watched;
 *   far above N    - must overflow, because no batching can keep a queue of N
 *                    from seeing more than N of several thousand arrivals.
 *
 * The load-bearing assertion is the last one: while a decision is unresolved, no
 * source watch is installed beneath it. An implementation that watched first and
 * asked git afterwards would pass every other check here, and would be the exact
 * defect P0 exists to fix. */
static void test_the_pending_ignore_queue_is_bounded_from_both_sides(void) {
    /* --- at the bound: never overflows, and everything is watched --- */
    {
        rig r;
        atlas_err err;
        atlas_err_init(&err);
        rig_open(&r, &err);
        build_tree(&r, fx_repo(&r.fx), 2, &err);
        register_repo(&r, fx_repo(&r.fx), "atbound", &err);

        const size_t LIMIT = 64;
        atlas_watcher_opts o;
        atlas_watcher_opts_init(&o);
        o.reconcile_interval_ms = 3600000;
        o.inject_max_pending_ignore = LIMIT;
        rig_start_opts(&r, &o, &err);

        atlas_index_state s;
        atlas_index_state_init(&s);
        T_CHECK(wait_repo_settled(&r, 1, &s));
        int64_t before = s.watched_source;

        for (size_t i = 0; i < LIMIT; i++) {
            char d[64];
            (void)snprintf(d, sizeof(d), "ok%03zu", i);
            T_OK(fx_mkdir(fx_repo(&r.fx), d, &err), &err);
        }

        bool settled = false;
        for (int i = 0; i < WAIT_MS / 50; i++) {
            atlas_index_state g;
            atlas_index_state_init(&g);
            atlas_err rerr;
            atlas_err_init(&rerr);
            read_state(&r, 1, &g, &rerr);
            T_CHECK_MSG(g.watch_reason != ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW,
                        "a queue of %zu must never overflow on %zu arrivals", LIMIT, LIMIT);
            if (g.present && g.watch_state == ATLAS_WATCH_WATCHING &&
                g.watched_source >= before + (int64_t)LIMIT) {
                settled = true;
            }
            atlas_index_state_free(&g);
            if (settled) {
                break;
            }
            usleep(50000);
        }
        T_CHECK_MSG(settled,
                    "every directory at or below the bound is resolved and watched: expected "
                    "%lld source watches",
                    (long long)(before + (int64_t)LIMIT));
        atlas_index_state_free(&s);
        rig_close(&r);
    }

    /* --- far above the bound: must overflow, and must say so --- */
    {
        rig r;
        atlas_err err;
        atlas_err_init(&err);
        rig_open(&r, &err);
        build_tree(&r, fx_repo(&r.fx), 2, &err);
        register_repo(&r, fx_repo(&r.fx), "overflow", &err);

        const size_t LIMIT = 4;
        const size_t FLOOD = 3000;
        atlas_watcher_opts o;
        atlas_watcher_opts_init(&o);
        o.reconcile_interval_ms = 3600000;
        o.inject_max_pending_ignore = LIMIT;
        rig_start_opts(&r, &o, &err);

        atlas_index_state s;
        atlas_index_state_init(&s);
        T_CHECK(wait_repo_settled(&r, 1, &s));

        for (size_t i = 0; i < FLOOD; i++) {
            char d[64];
            (void)snprintf(d, sizeof(d), "flood%04zu", i);
            T_OK(fx_mkdir(fx_repo(&r.fx), d, &err), &err);
        }

        bool overflowed = false;
        for (int i = 0; i < WAIT_MS / 20; i++) {
            atlas_index_state g;
            atlas_index_state_init(&g);
            atlas_err rerr;
            atlas_err_init(&rerr);
            read_state(&r, 1, &g, &rerr);
            if (g.present && g.watch_reason == ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW) {
                overflowed = true;
                T_CHECK_MSG(g.watch_state == ATLAS_WATCH_DEGRADED,
                            "an overflowed pending-ignore queue degrades the repository");
                T_CHECK_MSG(g.event_gap,
                            "and gaps it: Atlas cannot say which of those directories git "
                            "ignores, so it cannot claim to have observed everything");
                T_CHECK_MSG(!atlas_index_state_is_current(&g, NULL),
                            "and its index is not current");
            }
            atlas_index_state_free(&g);
            if (overflowed) {
                break;
            }
            usleep(20000);
        }
        T_CHECK_MSG(overflowed,
                    "%zu arrivals against a queue of %zu must overflow it and say so, not be "
                    "silently forgotten",
                    FLOOD, LIMIT);
        atlas_index_state_free(&s);
        rig_close(&r);
    }
}

/* --- review round 3: the five properties the last review required ---------- */

/* Every repository gets its essential metadata before any repository gets more
 * than its reserve.
 *
 * Adversarial by construction: the repository that sorts first has far more refs
 * than the reserve, so under a single-pass allocation it would take the whole
 * metadata allowance and the second repository would be unable to watch its own
 * HEAD — which is the starvation the source rounds were restructured to prevent,
 * one resource over. The budget is injected just large enough that a greedy
 * first repository would exhaust it. */
static void test_metadata_is_allocated_fairly_across_repositories(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);

    atlas_buf second = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&second, &err, "%s/second", atlas_buf_cstr(&r.fx.root)), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&r.fx.root), "second", &err), &err);

    /* **Both** repositories want far more metadata than the reserve, which is
     * what makes the reserve bind at all. With only one greedy repository the
     * other's whole need fits in the remainder and nothing is proved: the first
     * may legitimately take everything left over once its neighbour is
     * satisfied. "aaa" sorts first, so under a single-pass allocation it would
     * take the entire budget and "zzz" would hold nothing. */
    const int REFS = (int)ATLAS_WATCH_META_RESERVE_PER_REPO * 3;
    build_tree(&r, fx_repo(&r.fx), 2, &err);
    for (int i = 0; i < REFS; i++) {
        char d[128];
        (void)snprintf(d, sizeof(d), ".git/refs/heads/p%05d", i);
        T_OK(fx_mkdir(fx_repo(&r.fx), d, &err), &err);
    }
    build_tree(&r, atlas_buf_cstr(&second), 2, &err);
    for (int i = 0; i < REFS; i++) {
        char d[128];
        (void)snprintf(d, sizeof(d), ".git/refs/heads/q%05d", i);
        T_OK(fx_mkdir(atlas_buf_cstr(&second), d, &err), &err);
    }
    register_repo(&r, fx_repo(&r.fx), "aaa-greedy", &err);
    register_repo(&r, atlas_buf_cstr(&second), "zzz-starved", &err);

    /* Deliberately tight: barely more than one reserve. Under a single-pass
     * allocation the greedy repository would take all of it and the second would
     * hold nothing. */
    atlas_watcher_opts o;
    atlas_watcher_opts_init(&o);
    o.reconcile_interval_ms = 3600000;
    /* Room for both reserves and almost nothing else. */
    o.inject_budget_total = (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO * 2 + 16;
    rig_start_opts(&r, &o, &err);

    atlas_index_state a;
    atlas_index_state b;
    atlas_index_state_init(&a);
    atlas_index_state_init(&b);
    T_CHECK(wait_repo_settled(&r, 1, &a));
    T_CHECK(wait_repo_settled(&r, 2, &b));

    /* The starved repository must hold its essential metadata: its git dir, the
     * common dir's `info/`, and at least one `refs/` directory. A repository
     * that cannot watch those cannot see its own branch switches. */
    T_CHECK_MSG(b.watched_meta >= (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO,
                "the second repository got only %lld metadata watches of its reserve of %u, "
                "while the first holds %lld — the reserve pass must complete for every "
                "repository before any of them takes more",
                (long long)b.watched_meta, ATLAS_WATCH_META_RESERVE_PER_REPO,
                (long long)a.watched_meta);
    /* The greedy repository is capped at its reserve while the budget is this
     * tight — it may not spend into the second repository's essential capacity.
     *
     * It is *not* an error for it to exceed the reserve once every other
     * repository is satisfied: the reserve is a floor on what each repository is
     * guaranteed, not a ceiling on what any one may hold. That is why the
     * assertion is conditioned on the budget being tight rather than stated
     * unconditionally. */
    T_CHECK_MSG(a.watched_meta <= (int64_t)ATLAS_WATCH_META_RESERVE_PER_REPO + 16,
                "the greedy repository took %lld metadata watches; with both reserves owed it "
                "may take its own plus only the remainder",
                (long long)a.watched_meta);

    atlas_index_state_free(&a);
    atlas_index_state_free(&b);
    atlas_buf_free(&second);
    rig_close(&r);
}

/* P0. A shared descriptor is granted at a budget that is *exactly* full, and
 * granting it does not increase the physical count.
 *
 * Two linked worktrees share every descriptor on their common git directory. The
 * budget check used to run *before* `inotify_add_watch`, so at exactly the
 * budget the second worktree was refused a subscription that would have cost the
 * kernel nothing — the budget counts physical watches, and this is not one.
 *
 * An earlier version of this test set the budget to what the *pair* needs, which
 * left room for the second worktree and so never reached the boundary it claimed
 * to test. This one measures what the **first** worktree alone occupies, sets the
 * budget to exactly that, and then asks the second to install its metadata into a
 * completely full map. Both sides of the boundary are then asserted at the same
 * instant:
 *
 *   - the shared descriptors are granted, and `watches` does not move — an
 *     already-held descriptor costs nothing and is not refused;
 *   - the descriptors that are genuinely new *are* refused, and the second
 *     worktree is told `total_budget`.
 *
 * The metadata is what makes this measurable: it is installed before any source
 * walk, so the map is at its budget from the first worktree's metadata alone and
 * the second worktree meets a full map on its very first `add_watch`. */
static void test_a_shared_descriptor_is_granted_at_a_full_budget(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);

    build_tree(&r, fx_repo(&r.fx), 3, &err);
    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/wt2", atlas_buf_cstr(&r.fx.root)), &err);
    const char *addwt[] = {"worktree", "add", "-b", "second", atlas_buf_cstr(&wt)};
    atlas_err werr;
    atlas_err_init(&werr);
    if (fx_git_ok(&r.fx, fx_repo(&r.fx), addwt, 5u, &werr) != ATLAS_OK) {
        atlas_test_note("this git cannot create a linked worktree; skipping");
        atlas_buf_free(&wt);
        rig_close(&r);
        return;
    }

    /* First pass, first worktree only: how many physical descriptors does its
     * metadata actually occupy? With one repository registered nothing is
     * shared, so its subscription count and the kernel's descriptor count are
     * the same number — which is what makes it usable as a budget. */
    register_repo(&r, fx_repo(&r.fx), "wt-a", &err);
    rig_start_opts_default(&r, &err);
    T_CHECK(wait_primed(&r, 0));
    atlas_index_state solo;
    atlas_index_state_init(&solo);
    T_CHECK(wait_repo_settled(&r, 1, &solo));
    int64_t meta_only = solo.watched_meta;
    T_CHECK_MSG(solo.watched_shared == 0,
                "with one repository registered nothing can be shared, got %lld",
                (long long)solo.watched_shared);
    atlas_index_state_free(&solo);
    T_REQUIRE(meta_only > 0);
    rig_stop(&r);

    /* Second pass: both worktrees, and a budget of exactly the first one's
     * metadata. The first worktree fills the map to the last descriptor; the
     * second meets a map with no room in it at all. */
    register_repo(&r, atlas_buf_cstr(&wt), "wt-b", &err);
    rig_start_opts_budget(&r, meta_only, &err);
    T_CHECK(wait_primed(&r, 0));

    atlas_index_state a;
    atlas_index_state b;
    atlas_index_state_init(&a);
    atlas_index_state_init(&b);
    T_CHECK(wait_repo_settled(&r, 1, &a));
    T_CHECK(wait_repo_settled(&r, 2, &b));

    atlas_watch_stats ws;
    atlas_watcher_stats(r.watcher, &ws);
    /* Exactly full, not merely within the budget: if this were `<` the rest of
     * the test would be asserting the easy case. */
    T_CHECK_MSG(ws.watches == meta_only,
                "the map must be exactly full at the boundary: watches=%lld budget=%lld",
                (long long)ws.watches, (long long)meta_only);
    /* The grant. The second worktree holds metadata subscriptions it could only
     * have obtained by sharing, because there was no room for a new one. */
    T_CHECK_MSG(b.watched_meta > 0,
                "the second worktree must hold metadata subscriptions at a full budget");
    T_CHECK_MSG(b.watched_shared > 0,
                "those subscriptions must be on descriptors already held: shared=%lld",
                (long long)b.watched_shared);
    T_CHECK_MSG(ws.subscriptions > ws.watches,
                "sharing must be visible as more subscriptions than descriptors: %lld <= %lld",
                (long long)ws.subscriptions, (long long)ws.watches);
    /* The refusal, at the same instant and from the same walk: the second
     * worktree's own git directory is not shared with anything, so it is a new
     * descriptor meeting a full map, and it is named as such. */
    T_CHECK_MSG(b.watch_reason == ATLAS_WATCH_REASON_TOTAL_BUDGET,
                "a new descriptor at a full budget must be refused with total_budget, got %s",
                atlas_watch_reason_name(b.watch_reason));
    /* And the first worktree kept everything it had: the second one's arrival
     * cost it nothing. */
    T_CHECK_MSG(a.watched_meta == meta_only,
                "the first worktree must keep its metadata: %lld of %lld",
                (long long)a.watched_meta, (long long)meta_only);

    atlas_index_state_free(&a);
    atlas_index_state_free(&b);
    atlas_buf_free(&wt);
    rig_close(&r);
}

/* P0. A blocked writer never lets Atlas claim the index is current.
 *
 * Publishing a watch state is asynchronous, and A9.2.6 documents that an
 * unbounded job can hold the single writer thread for minutes. During such a
 * stretch the stored row is frozen at whatever was last written — `watching`,
 * no gap, `index_current: true` — while the watcher has already established that
 * a subtree appeared late and a content-verifying pass is owed.
 *
 * Enqueueing is not persistence. The test therefore holds the writer inside a
 * reconciliation on purpose, so that *nothing* the watcher decides afterwards
 * can reach the database, and then asserts the two halves separately:
 *
 *   - the stored row, read on its own, still says the index is current — which
 *     is the premise, and a test that could not show it would prove nothing;
 *   - the same row overlaid with the watcher's live view says it is not, and
 *     names the owed pass.
 *
 * `atlas_server_overlay_live` is the production path: `atlas_server_write_repo_state`
 * calls it on every `status` and every `ai.context`. */
static void test_a_blocked_writer_never_reports_the_index_current(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);
    register_repo(&r, fx_repo(&r.fx), "blocked", &err);
    rig_start_opts_default(&r, &err);

    /* Start from a state the row itself calls current, or the assertion below is
     * vacuous. */
    atlas_index_state before;
    atlas_index_state_init(&before);
    bool clean = false;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r.watcher, &ws);
        atlas_index_state_free(&before);
        atlas_index_state_init(&before);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r, 1, &before, &rerr);
        clean = ws.priming_complete && atlas_watcher_primed(r.watcher) &&
                atlas_index_state_is_current(&before, NULL);
        if (clean) {
            break;
        }
        usleep(50000);
    }
    T_REQUIRE(clean);
    atlas_index_state_free(&before);

    /* From here the writer is held inside a reconciliation and never leaves it,
     * so every later job — the watch publication and its gap flags among them —
     * sits in the queue behind it. Engaged *before* the directory appears: a
     * stall armed but not yet reached would let the first publication through,
     * and that publication is exactly what must not reach the database. */
    T_REQUIRE(engage_writer_stall(&r));

    T_OK(fx_mkdir(fx_repo(&r.fx), "late", &err), &err);
    T_OK(fx_write(fx_repo(&r.fx), "late/f.c", "int f;\n", &err), &err);

    /* Wait for the watcher to establish the obligation. It cannot be discharged
     * while the writer is held: `settle_owed_gaps` discharges only on a
     * *completed* pass newer than the obligation, and no pass can complete. */
    bool owed = false;
    for (int i = 0; i < WAIT_MS / 20; i++) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r.watcher, &ws);
        if (ws.owed_gaps > 0) {
            owed = true;
            break;
        }
        usleep(20000);
    }
    T_CHECK_MSG(owed, "the watcher never recorded the obligation the late subtree creates");

    atlas_index_state s;
    atlas_index_state_init(&s);
    atlas_err rerr;
    atlas_err_init(&rerr);
    read_state(&r, 1, &s, &rerr);

    /* The premise: nothing reached the database, so the row still says current. */
    const char *stored_reason = NULL;
    T_CHECK_MSG(atlas_index_state_is_current(&s, &stored_reason),
                "the stored row should still read current while the writer is blocked, but says "
                "%s",
                stored_reason == NULL ? "(no reason)" : stored_reason);

    /* The property: the answer a client is given is not the row. */
    atlas_watch_live live;
    atlas_watcher_repo_live(r.watcher, 1, &live);
    T_CHECK_MSG(live.known, "the watcher must have a live view of a repository it observes");
    T_CHECK_MSG(live.owes_gap, "the live view must carry the obligation the row does not");

    atlas_server_overlay_live(&s, &live);
    const char *reason = NULL;
    T_CHECK_MSG(!atlas_index_state_is_current(&s, &reason),
                "a repository owing a content-verifying pass must never be reported current, "
                "however busy the writer is");
    T_CHECK_MSG(reason != NULL && strstr(reason, "full content verification") != NULL,
                "the reason must name the owed pass, got %s",
                reason == NULL ? "(none)" : reason);
    T_CHECK(s.pending_full_reconcile);
    atlas_index_state_free(&s);

    /* Released before teardown so the writer drains rather than being joined out
     * of a stall — the shutdown path handles that too, but a test that never
     * exercised the ordinary release would not notice it breaking. */
    atlas_writer_test_release(r.writer);
    rig_close(&r);
}

/* P0. A watch publication that fails **inside the database write** does not
 * leave Atlas reporting a blind-spotted repository as current.
 *
 * `inject_publish_failures` refuses the submission, which proves that a full
 * writer queue is handled. It proves nothing about the other failure, and the
 * other failure is the one with no feedback path: `SET_WATCH` carries no result
 * and nobody waits for it, so a job that is dequeued, reaches
 * `atlas_db_index_state_set_watch` and fails there is invisible to its
 * submitter. The row keeps its previous contents; in a fresh database that is
 * `watch_state = 'unwatched'`, which the currency rule does not treat as a
 * blind spot — so once a reconciliation completes, the row on its own reads
 * `index_current: true` for a repository the watcher has already given up on.
 *
 * The fault is injected in the writer, after the dequeue, at the statement — and
 * the counter proves it fired there rather than at the submission. The budget is
 * squeezed so the repository degrades, because degradation is terminal for the
 * run and makes the assertion a property rather than a race. */
static void test_a_watch_write_that_fails_in_the_database_is_not_a_current_index(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 40, &err);
    register_repo(&r, fx_repo(&r.fx), "discarded", &err);

    /* Armed between the writer starting and the watcher starting, so not one
     * publication in this test's life reaches the database. */
    rig_start_writer(&r, &err);
    atlas_writer_test_fail_watch_writes(r.writer, 1000000);
    atlas_watcher_opts o;
    atlas_watcher_opts_init(&o);
    o.reconcile_interval_ms = 3600000;
    /* Small enough that the source tree cannot be watched, which is the
     * degradation this test wants to be unreportable. */
    o.inject_budget_total = 6;
    rig_start_watcher(&r, &o, &err);

    /* Wait until three things hold at once: a publication was discarded inside
     * the writer, the watcher has settled on degraded, and a pass has completed
     * leaving the row with nothing outstanding. */
    atlas_index_state s;
    atlas_index_state_init(&s);
    atlas_watch_live live;
    memset(&live, 0, sizeof live);
    bool ready = false;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        atlas_watcher_repo_live(r.watcher, 1, &live);
        atlas_index_state_free(&s);
        atlas_index_state_init(&s);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r, 1, &s, &rerr);
        if (atlas_writer_test_watch_writes_failed(r.writer) > 0 && live.known && live.degraded &&
            atlas_index_state_is_current(&s, NULL)) {
            ready = true;
            break;
        }
        usleep(50000);
    }
    if (!ready) {
        T_CHECK_MSG(false,
                    "never reached the state under test (discarded=%lld known=%d degraded=%d "
                    "state=%s gap=%d pending=%d gen=%lld)",
                    (long long)atlas_writer_test_watch_writes_failed(r.writer), (int)live.known,
                    (int)live.degraded, atlas_watch_state_name(s.watch_state), (int)s.event_gap,
                    (int)s.pending_full_reconcile, (long long)s.last_complete_generation);
    }

    /* The write really was discarded: the row never learned what the watcher
     * published. */
    T_CHECK_MSG(s.watch_state != ATLAS_WATCH_DEGRADED,
                "the premise fails if the degraded state reached the row anyway");
    T_CHECK_MSG(atlas_writer_test_watch_writes_failed(r.writer) > 0,
                "the fault must fire inside the writer, not at the submission");

    /* The property. */
    atlas_server_overlay_live(&s, &live);
    const char *reason = NULL;
    T_CHECK_MSG(!atlas_index_state_is_current(&s, &reason),
                "a degraded repository must never be reported current because its publication "
                "failed in the database");
    T_CHECK_MSG(s.watch_state == ATLAS_WATCH_DEGRADED,
                "the overlaid state must say degraded, got %s",
                atlas_watch_state_name(s.watch_state));
    T_CHECK_MSG(reason != NULL && strstr(reason, "blind spot") != NULL,
                "the reason must name the blind spot, got %s", reason == NULL ? "(none)" : reason);
    atlas_index_state_free(&s);
    rig_close(&r);
}

/* The overlay never makes Atlas *more* confident than the row it was given.
 *
 * That is the only direction in which it could turn a durable fact into a
 * transient opinion: a watcher that believes everything is fine must not be able
 * to clear a recorded gap, retire an owed pass, or promote a recorded failure.
 * Asserted against the function directly, over every stored state, because the
 * shapes that matter are cheap to enumerate and expensive to reach through a
 * daemon. */
static void test_the_live_overlay_only_ever_subtracts(void) {
    static const atlas_watch_state STATES[] = {
        ATLAS_WATCH_UNWATCHED,  ATLAS_WATCH_WATCHING, ATLAS_WATCH_DEGRADED,
        ATLAS_WATCH_INCOMPLETE, ATLAS_WATCH_ERROR,    ATLAS_WATCH_PRIMING};
    atlas_watch_live healthy;
    memset(&healthy, 0, sizeof healthy);
    healthy.known = true;

    for (size_t i = 0; i < sizeof STATES / sizeof STATES[0]; i++) {
        for (int gap = 0; gap < 2; gap++) {
            for (int pending = 0; pending < 2; pending++) {
                atlas_index_state s;
                atlas_index_state_init(&s);
                s.present = true;
                s.last_complete_generation = 7;
                s.watch_state = STATES[i];
                s.event_gap = gap != 0;
                s.pending_full_reconcile = pending != 0;
                bool was_current = atlas_index_state_is_current(&s, NULL);

                atlas_server_overlay_live(&s, &healthy);
                T_CHECK_MSG(s.event_gap == (gap != 0), "the overlay cleared an event gap");
                T_CHECK_MSG(s.pending_full_reconcile == (pending != 0),
                            "the overlay cleared an owed pass");
                T_CHECK_MSG(s.watch_state == STATES[i],
                            "a healthy live view changed the stored state from %s to %s",
                            atlas_watch_state_name(STATES[i]),
                            atlas_watch_state_name(s.watch_state));
                T_CHECK_MSG(atlas_index_state_is_current(&s, NULL) == was_current,
                            "a healthy live view changed the currency of %s",
                            atlas_watch_state_name(STATES[i]));

                /* And a live view that is worse never promotes a worse row. */
                atlas_watch_live worse;
                memset(&worse, 0, sizeof worse);
                worse.known = true;
                worse.priming = true;
                worse.degraded = true;
                worse.owes_gap = true;
                atlas_server_overlay_live(&s, &worse);
                T_CHECK_MSG(!atlas_index_state_is_current(&s, NULL),
                            "a priming, degraded, gap-owing watcher must never read current");
                if (STATES[i] == ATLAS_WATCH_ERROR || STATES[i] == ATLAS_WATCH_INCOMPLETE) {
                    T_CHECK_MSG(s.watch_state == STATES[i],
                                "the overlay downgraded %s to %s, which is a promotion in the "
                                "other direction",
                                atlas_watch_state_name(STATES[i]),
                                atlas_watch_state_name(s.watch_state));
                }
                atlas_index_state_free(&s);
            }
        }
    }

    /* An unknown repository is left entirely alone: the watcher has nothing to
     * say about it, which is not the same as saying it is fine. */
    atlas_index_state s;
    atlas_index_state_init(&s);
    s.present = true;
    s.last_complete_generation = 3;
    s.watch_state = ATLAS_WATCH_WATCHING;
    atlas_watch_live unknown;
    memset(&unknown, 0, sizeof unknown);
    unknown.priming = true;
    unknown.degraded = true;
    unknown.owes_gap = true;
    atlas_server_overlay_live(&s, &unknown);
    T_CHECK(s.watch_state == ATLAS_WATCH_WATCHING);
    T_CHECK(!s.pending_full_reconcile);
    T_CHECK(atlas_index_state_is_current(&s, NULL));
    atlas_index_state_free(&s);
}

/* A persistently failing ignore refresh is asked a bounded number of times.
 *
 * The repository's working tree is removed under the daemon, so every
 * `atlas_git_open` fails. Without a backoff the pending queue and the staleness
 * flag would each bring the repository back to `refresh_ignored` on every
 * watcher tick — one git process every 200 ms for as long as the condition
 * lasts. The counter is what makes the bound checkable: a bound nothing counts
 * is a claim. */
static void test_a_failing_ignore_refresh_is_not_retried_every_tick(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);
    register_repo(&r, fx_repo(&r.fx), "vanishing", &err);
    rig_start(&r, 0, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));

    /* Break git for this repository, then give it a reason to ask. */
    atlas_buf gitdir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&gitdir, &err, "%s/.git", fx_repo(&r.fx)), &err);
    atlas_buf moved = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&moved, &err, "%s/.git-moved", fx_repo(&r.fx)), &err);
    T_REQUIRE(rename(atlas_buf_cstr(&gitdir), atlas_buf_cstr(&moved)) == 0);

    atlas_watch_stats base;
    atlas_watcher_stats(r.watcher, &base);

    /* Several seconds of ticks. At ~200 ms per tick that is tens of
     * opportunities to run git; the backoff starts at one second and doubles. */
    for (int i = 0; i < 60; i++) {
        char d[64];
        (void)snprintf(d, sizeof(d), "poke%02d", i);
        (void)fx_mkdir(fx_repo(&r.fx), d, &err);
        usleep(50000);
    }
    atlas_watch_stats after;
    atlas_watcher_stats(r.watcher, &after);
    int64_t attempts = after.ignore_refresh_attempts - base.ignore_refresh_attempts;

    /* Three seconds of backoff starting at one second and doubling admits at
     * most a handful. The ceiling is deliberately loose — what it must exclude
     * is one-per-tick, which over this window would be dozens. */
    T_CHECK_MSG(attempts <= 8,
                "a persistently failing ignore refresh ran git %lld times in ~3 s; the backoff "
                "must freeze the attempt rather than retry every tick",
                (long long)attempts);

    /* Put it back so the fixture can be torn down cleanly. */
    (void)rename(atlas_buf_cstr(&moved), atlas_buf_cstr(&gitdir));
    atlas_buf_free(&gitdir);
    atlas_buf_free(&moved);
    atlas_index_state_free(&s);
    rig_close(&r);
}

/* An owed gap survives a failed publication, and the pass it forces stays full.
 *
 * A successful submission means the job reached the writer queue, not that the
 * row was written. Clearing the obligation on enqueue is the same class of
 * mistake as clearing it on a dropped submission: in both cases a repository can
 * end up reported current over a subtree whose events were missed. The
 * injection makes the failure deterministic, because a fixture's writer queue is
 * never full. */
static void test_an_owed_gap_survives_a_failed_publication(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);
    register_repo(&r, fx_repo(&r.fx), "faulty", &err);

    atlas_watcher_opts o;
    atlas_watcher_opts_init(&o);
    o.reconcile_interval_ms = 3600000;
    /* Refused for the whole test. Six was not enough: the startup transitions
     * consume them before the late directory even appears, so publication had
     * recovered by the time an obligation existed and the window in which one
     * was outstanding was too short to observe. The property under test is that
     * the obligation is *retained* while publication fails, so publication must
     * fail throughout. */
    o.inject_publish_failures = 1000000;
    rig_start_opts(&r, &o, &err);

    /* Wait for a genuinely clean recorded state first.
     *
     * `settle_owed_gaps` discharges an obligation when a gap is already visible
     * in the database — correctly, because the obligation is "a gap is recorded"
     * and one already is. Starting the test with a gap left over from startup
     * would therefore discharge the new obligation for a legitimate reason and
     * prove nothing about the failure path. */
    bool clean = false;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r.watcher, &ws);
        atlas_index_state g;
        atlas_index_state_init(&g);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r, 1, &g, &rerr);
        /* Priming must be *finished* as well as the record clean. A directory
         * created while the walk is still running is discovered by `readdir`,
         * not by an inotify event, so it never enters the pending-ignore queue
         * and never creates the obligation this test is about — it would be
         * watched perfectly correctly and prove nothing. */
        clean = ws.priming_complete && atlas_watcher_primed(r.watcher) && g.present &&
                !g.event_gap && !g.pending_full_reconcile;
        atlas_index_state_free(&g);
        if (clean) {
            break;
        }
        usleep(50000);
    }
    T_CHECK_MSG(clean, "the repository never reached a primed, clean state to start from");

    /* The writer is held from here on. Without it this test asserted something
     * stronger than the design: `settle_owed_gaps` discharges on a *completed
     * pass newer than the obligation*, which is a legitimate discharge — the
     * content verification really did happen — and a fixture's passes complete in
     * milliseconds, so "it stays owed" was true only for as long as the machine
     * was slow. Holding the writer removes the legitimate discharge and leaves
     * exactly the illegitimate one under test: a submission that succeeded while
     * its write never landed. */
    T_REQUIRE(engage_writer_stall(&r));

    /* A directory appears: it is queued, resolved, watched late, and therefore
     * owes a content-verifying pass. Every publication is refused. */
    T_OK(fx_mkdir(fx_repo(&r.fx), "late", &err), &err);
    T_OK(fx_write(fx_repo(&r.fx), "late/f.c", "int f;\n", &err), &err);

    /* The obligation is asserted directly rather than through the database's
     * `event_gap` bit, because a full pass can clear that bit between two polls
     * — which would make this test pass or fail on timing rather than on the
     * property. `owed_gaps` is what the watcher still believes it owes. */
    bool retained = false;
    for (int i = 0; i < WAIT_MS / 20; i++) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r.watcher, &ws);
        if (ws.owed_gaps > 0) {
            retained = true;
            break;
        }
        usleep(20000);
    }
    if (!retained) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r.watcher, &ws);
        T_CHECK_MSG(false,
                    "an obligation whose publication was refused must be retained, not "
                    "discharged because a job was queued (watches=%lld subs=%lld primed=%d "
                    "owed=%lld)",
                    (long long)ws.watches, (long long)ws.subscriptions,
                    (int)ws.priming_complete, (long long)ws.owed_gaps);
    }

    /* It stays owed: nothing discharges it while publication keeps failing. */
    for (int i = 0; i < 20; i++) {
        atlas_watch_stats ws;
        atlas_watcher_stats(r.watcher, &ws);
        if (ws.owed_gaps <= 0) {
            atlas_index_state g;
            atlas_index_state_init(&g);
            atlas_err rerr;
            atlas_err_init(&rerr);
            read_state(&r, 1, &g, &rerr);
            T_CHECK_MSG(false,
                        "the obligation was discharged while publication was still failing "
                        "(db: present=%d gap=%d pending=%d state=%s reason=%s)",
                        (int)g.present, (int)g.event_gap, (int)g.pending_full_reconcile,
                        atlas_watch_state_name(g.watch_state),
                        atlas_watch_reason_name(g.watch_reason));
            atlas_index_state_free(&g);
            break;
        }
        usleep(20000);
    }
    atlas_writer_test_release(r.writer);
    rig_close(&r);

    /* The other half: with publication working, the same scenario records the
     * gap in the database. */
    rig r2;
    atlas_err e2;
    atlas_err_init(&e2);
    rig_open(&r2, &e2);
    build_tree(&r2, fx_repo(&r2.fx), 2, &e2);
    register_repo(&r2, fx_repo(&r2.fx), "healthy", &e2);
    rig_start(&r2, 0, &e2);
    atlas_index_state s2;
    atlas_index_state_init(&s2);
    T_CHECK(wait_repo_settled(&r2, 1, &s2));
    T_OK(fx_mkdir(fx_repo(&r2.fx), "late", &e2), &e2);
    T_OK(fx_write(fx_repo(&r2.fx), "late/f.c", "int f;\n", &e2), &e2);
    bool landed = false;
    for (int i = 0; i < WAIT_MS / 50; i++) {
        atlas_index_state g;
        atlas_index_state_init(&g);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r2, 1, &g, &rerr);
        if (g.present && (g.event_gap || g.pending_full_reconcile)) {
            landed = true;
        }
        atlas_index_state_free(&g);
        if (landed) {
            break;
        }
        usleep(50000);
    }
    T_CHECK_MSG(landed, "with publication working the gap must reach the database");
    atlas_index_state_free(&s2);
    rig_close(&r2);
}


/* P0. A temporary ignore failure is recovered from, not survived.
 *
 * The backoff above stops a broken repository from running one git process per
 * tick. A backoff that never expires is a different failure with the same
 * symptom: the repository sits degraded for the life of the daemon while the
 * condition that degraded it is long gone, and nothing brings it back because
 * `reprime_repository` cleared the queue that used to be the only way in.
 *
 * So the whole cycle is asserted here, and the recovery half is the point: git
 * is broken, the repository degrades with `error`, git is restored, and the
 * repository must return to `watching` and to a current index on its own —
 * without a repository-set change, a restart, or anything else asking it to. */
static void test_a_temporary_ignore_failure_recovers(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);
    register_repo(&r, fx_repo(&r.fx), "recovering", &err);
    rig_start(&r, 0, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));
    atlas_index_state_free(&s);

    /* Git is broken by making its directory unreadable, not by moving it aside.
     *
     * Both break `atlas_git_open`; only one of them can be *undone* without the
     * watcher noticing. Moving `.git` back produces an `IN_MOVED_TO` for a
     * directory in the watched root, which the watcher queues as a
     * pending-ignore decision — and a non-empty queue is itself a way back into
     * `resolve_pending_ignores`. The repository would then recover whether or
     * not the retry timer existed, which is a test that cannot fail. A mode
     * change produces `IN_ATTRIB` on a name that is neither a new directory nor
     * an ignore-rule file, so nothing is queued and nothing is marked stale.
     *
     * The mode is restored in every exit path below, so the fixture can still be
     * torn down. */
    T_OK(fx_chmod(fx_repo(&r.fx), ".git", 0000, &err), &err);

    /* The failure is provoked by an ignore-rule change rather than by a new
     * directory, and that choice is the second half of the isolation.
     *
     * A rule change re-primes the repository, and re-priming **empties the
     * pending-ignore queue**. So when the refresh then fails, the repository is
     * left degraded with nothing queued — which is the state in which the retry
     * timer is the only thing left that can bring it back.
     *
     * The file is written directly: git is broken, so nothing is committed. */
    T_OK(fx_write(fx_repo(&r.fx), ".gitignore", "build/\n", &err), &err);

    /* Wait for the verdict. `error` specifically: a repository that degraded for
     * a budget reason would not be recovered by a working git, and asserting the
     * wrong reason here would let that confusion through. */
    bool degraded = false;
    for (int i = 0; i < WAIT_MS / 50 && !degraded; i++) {
        atlas_watch_live live;
        atlas_watcher_repo_live(r.watcher, 1, &live);
        degraded = live.known && live.degraded;
        if (!degraded) {
            usleep(50000);
        }
    }
    T_CHECK_MSG(degraded, "a repository whose git cannot be opened must degrade");

    atlas_index_state broken;
    atlas_index_state_init(&broken);
    /* The *reason*, not merely a settled state: this repository was already
     * `watching`, so waiting for "settled" is satisfied by the row from before
     * the failure and the assertion below then reads it. */
    T_CHECK_MSG(wait_repo_reason(&r, 1, ATLAS_WATCH_REASON_ERROR, &broken),
                "the failure never reached the stored state");
    T_CHECK_MSG(broken.watch_reason == ATLAS_WATCH_REASON_ERROR,
                "the reason must name the failure, got %s",
                atlas_watch_reason_name(broken.watch_reason));
    T_CHECK_MSG(!atlas_index_state_is_current(&broken, NULL),
                "a repository whose ignore rules could not be read is not a current index");
    atlas_index_state_free(&broken);

    /* Restore git and then ask for nothing at all: no directory is created, no
     * rule is edited, no repository is added or removed. The retry timer is the
     * only thing left that can bring it back. */
    T_OK(fx_chmod(fx_repo(&r.fx), ".git", 0700, &err), &err);

    bool recovered = false;
    atlas_index_state good;
    atlas_index_state_init(&good);
    for (int i = 0; i < WAIT_MS / 100; i++) {
        atlas_index_state_free(&good);
        atlas_index_state_init(&good);
        atlas_err rerr;
        atlas_err_init(&rerr);
        read_state(&r, 1, &good, &rerr);
        atlas_watch_live live;
        atlas_watcher_repo_live(r.watcher, 1, &live);
        atlas_server_overlay_live(&good, &live);
        if (good.present && good.watch_state == ATLAS_WATCH_WATCHING &&
            atlas_index_state_is_current(&good, NULL)) {
            recovered = true;
            break;
        }
        usleep(100000);
    }
    if (!recovered) {
        T_CHECK_MSG(false,
                    "a repository whose git recovered must return to watching and current on its "
                    "own (state=%s reason=%s gap=%d pending=%d)",
                    atlas_watch_state_name(good.watch_state),
                    atlas_watch_reason_name(good.watch_reason), (int)good.event_gap,
                    (int)good.pending_full_reconcile);
    }
    /* And it did not come back on a stale watch set: recovery re-primes from the
     * root, so the source watches are the ones a fresh walk installed. */
    T_CHECK_MSG(good.watched_source > 0,
                "a recovered repository must hold source watches again, got %lld",
                (long long)good.watched_source);
    atlas_index_state_free(&good);
    rig_close(&r);
}

/* P0. Late batches, one after another, never accumulate into a false
 * `discovery_bound`.
 *
 * `visits` bounds the *work* one priming pass may do, separately from the bound
 * on how many watches it may hold — a repository that has walked too far and one
 * that has run out of budget are different findings. The counter was reset when
 * a priming pass began and nowhere else, so every directory a running daemon
 * ever queued added to the same total: a repository receiving a few new
 * directories at a time would, after enough hours, be told it had exceeded a
 * discovery bound it had never come near.
 *
 * A late subtree begins a new pass over new ground, so it resets the counter.
 * The test walks past the bound in batches, and the directories are removed
 * between batches so the *watch* budget is never the thing that stops it —
 * otherwise a `total_budget` refusal would mask the reason under test.
 *
 * The budget is injected small so the bound (`budget_repo * 2`) is reachable in
 * a test rather than after a day of production. Nothing else differs from
 * production: the same counter, the same comparison, the same reset. */
static void test_repeated_late_batches_never_report_a_false_discovery_bound(void) {
    rig r;
    atlas_err err;
    atlas_err_init(&err);
    rig_open(&r, &err);
    build_tree(&r, fx_repo(&r.fx), 2, &err);
    register_repo(&r, fx_repo(&r.fx), "batched", &err);

    /* 500 leaves room for the metadata reserve and roughly 240 source watches,
     * and puts the discovery bound at 1000 visits. */
    const int64_t budget = 500;
    rig_start_opts_budget(&r, budget, &err);
    T_CHECK(wait_primed(&r, 0));
    atlas_index_state s;
    atlas_index_state_init(&s);
    T_CHECK(wait_repo_settled(&r, 1, &s));
    int64_t base = s.watched_source;
    atlas_index_state_free(&s);

    const int ROUNDS = 5;
    const int PER_ROUND = 220; /* 5 x 220 = 1100 visits, past a bound of 1000 */
    for (int round = 0; round < ROUNDS; round++) {
        for (int i = 0; i < PER_ROUND; i++) {
            char d[64];
            (void)snprintf(d, sizeof(d), "b%d_%03d", round, i);
            T_OK(fx_mkdir(fx_repo(&r.fx), d, &err), &err);
        }
        /* Wait for the batch to be walked, sampling the reason throughout: the
         * defect shows up *during* a walk, not only at the end of one. */
        bool walked = false;
        for (int t = 0; t < WAIT_MS / 50 && !walked; t++) {
            atlas_index_state g;
            atlas_index_state_init(&g);
            atlas_err rerr;
            atlas_err_init(&rerr);
            read_state(&r, 1, &g, &rerr);
            T_CHECK_MSG(g.watch_reason != ATLAS_WATCH_REASON_DISCOVERY_BOUND,
                        "round %d: a repository walking %d directories at a time reported a "
                        "discovery bound it never reached", round, PER_ROUND);
            atlas_watch_stats ws;
            atlas_watcher_stats(r.watcher, &ws);
            walked = ws.priming_complete && g.watched_source >= base + PER_ROUND;
            atlas_index_state_free(&g);
            if (!walked) {
                usleep(50000);
            }
        }
        T_CHECK_MSG(walked, "round %d: the batch was never fully watched", round);

        /* Removed again, so the next round's visits are not paid for out of the
         * watch budget as well. */
        for (int i = 0; i < PER_ROUND; i++) {
            char p[PATH_MAX];
            (void)snprintf(p, sizeof(p), "%s/b%d_%03d", fx_repo(&r.fx), round, i);
            (void)rmdir(p);
        }
        for (int t = 0; t < WAIT_MS / 50; t++) {
            atlas_index_state g;
            atlas_index_state_init(&g);
            atlas_err rerr;
            atlas_err_init(&rerr);
            read_state(&r, 1, &g, &rerr);
            bool dropped = g.watched_source <= base + 8;
            atlas_index_state_free(&g);
            if (dropped) {
                break;
            }
            usleep(50000);
        }
    }

    /* Total visits across the run are 1100 against a bound of 1000. The final
     * verdict must still be a repository that walked what it was asked to. */
    atlas_index_state end;
    atlas_index_state_init(&end);
    atlas_err rerr;
    atlas_err_init(&rerr);
    read_state(&r, 1, &end, &rerr);
    T_CHECK_MSG(end.watch_reason != ATLAS_WATCH_REASON_DISCOVERY_BOUND,
                "%d rounds of %d directories must not accumulate into a discovery bound of %lld",
                ROUNDS, PER_ROUND, (long long)(budget * ATLAS_WATCH_DISCOVER_FACTOR));
    atlas_index_state_free(&end);
    rig_close(&r);
}

static const atlas_test TESTS[] = {
    {"the watch reason vocabulary round-trips and is distinct",
     test_reason_vocabulary_round_trips},
    {"the watch bounds are consistent with each other and their types",
     test_bounds_are_mutually_consistent},
    {"the policy's watch budget is range-checked and never clamped",
     test_policy_watch_budget_is_range_checked},
    {"exactly N watches install at a budget of N, and the bound is named",
     test_exact_boundary_at_the_daemon_total},
    {"git metadata is watched even when the source tree does not fit",
     test_metadata_is_installed_before_source},
    {"two repositories do not consume each other's watch budget",
     test_two_repositories_do_not_consume_each_others_budget},
    {"a shared descriptor is one watch and several subscriptions",
     test_shared_descriptors_are_counted_once_by_the_kernel},
    {"removing one worktree leaves the other's shared watches installed",
     test_removing_one_worktree_leaves_the_others_watches},
    {"metadata beyond the reserve still completes: the reserve is a floor",
     test_metadata_beyond_the_reserve_completes},
    {"a hungry source tree cannot spend the metadata reserve",
     test_the_metadata_reserve_survives_a_hungry_source_tree},
    {"exactly the metadata ceiling installs, and the bound is meta_budget",
     test_exact_boundary_at_the_metadata_maximum},
    {"the kernel's ENOSPC is reported as the kernel's, not as a budget",
     test_kernel_enospc_is_reported_as_the_kernels},
    {"exactly N repositories are observed and the N+1th is told repo_limit",
     test_exact_boundary_at_the_repository_ceiling},
    {"the pending-ignore queue holds N, overflows beyond it, and watches neither",
     test_the_pending_ignore_queue_is_bounded_from_both_sides},
    {"metadata is allocated fairly: every repository's reserve before any excess",
     test_metadata_is_allocated_fairly_across_repositories},
    {"a shared descriptor is granted at an exactly full budget, and a new one is not",
     test_a_shared_descriptor_is_granted_at_a_full_budget},
    {"a blocked writer never lets Atlas report the index current",
     test_a_blocked_writer_never_reports_the_index_current},
    {"a watch write that fails in the database is not a current index",
     test_a_watch_write_that_fails_in_the_database_is_not_a_current_index},
    {"the live overlay only ever subtracts confidence",
     test_the_live_overlay_only_ever_subtracts},
    {"a failing ignore refresh is frozen, not retried every tick",
     test_a_failing_ignore_refresh_is_not_retried_every_tick},
    {"a temporary ignore failure recovers to watching and current on its own",
     test_a_temporary_ignore_failure_recovers},
    {"repeated late batches never accumulate into a false discovery bound",
     test_repeated_late_batches_never_report_a_false_discovery_bound},
    {"an owed gap survives a failed publication and still forces a full pass",
     test_an_owed_gap_survives_a_failed_publication},
    {"a repository-set change gaps every repository, not only the new one",
     test_a_repo_set_change_gaps_every_repository},
};

ATLAS_TEST_MAIN("watch_budget", TESTS)

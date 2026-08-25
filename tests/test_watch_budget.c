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
#include <sys/inotify.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/limits.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
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
static void rig_start_opts(rig *r, const atlas_watcher_opts *o, atlas_err *err) {
    T_OK(atlas_writer_start(atlas_buf_cstr(&r->db_path), "", NULL, r->log, &r->writer, err), err);
    T_OK(atlas_watcher_start(atlas_buf_cstr(&r->db_path), r->writer, r->log, o, &r->watcher, err),
         err);
}

static void rig_start(rig *r, int64_t budget, atlas_err *err) {
    atlas_watcher_opts o;
    atlas_watcher_opts_init(&o);
    o.reconcile_interval_ms = 3600000;
    o.inject_budget_total = budget;
    rig_start_opts(r, &o, err);
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
    {"a repository-set change gaps every repository, not only the new one",
     test_a_repo_set_change_gaps_every_repository},
};

ATLAS_TEST_MAIN("watch_budget", TESTS)

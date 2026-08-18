/* Atlas - A9.2.4: build-input discovery, and the completeness of the search.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The sentence this suite exists to hold:
 *
 *   **COMPLETE PROCESSING OF CONFIGURED INPUTS DOES NOT PROVE COMPLETE
 *   DISCOVERY OF RELEVANT INPUTS.**
 *
 * A9.2.3 gave a generation a denominator over the sources the file index
 * enumerates, which turned `416/416 units complete` into the honest `369 of 761
 * sources covered`. It could not ask the question underneath: were those the
 * right compilation databases, and were there only those? Nothing could ask it,
 * because compilation databases were *named, never discovered* — so the answer
 * was whatever an operator had typed, and on the repository that produced this
 * season an operator had typed two of three.
 *
 * The claims, and what goes wrong without each:
 *
 *   1. **Discovery finds every compilation database in the bounded universe**,
 *      for any number of them. Without it, a repository's second and third build
 *      trees are invisible and their sources are simply absent from the index
 *      with nothing saying so.
 *   2. **A bound that is reached makes the search PARTIAL**, and PARTIAL never
 *      supports an absence. Without it, a walk that stopped early is
 *      indistinguishable from a repository that has nothing more to find.
 *   3. **One file is one input however many paths reach it.** Without canonical
 *      identity, a symlinked compilation database is indexed twice and every
 *      count derived from it is wrong.
 *   4. **A candidate that cannot be used is *shown*, with a reason.** Without
 *      that, a rejected candidate is indistinguishable from one that does not
 *      exist — which is exactly the indistinguishability this season is about.
 *   5. **A change to the input set moves the source identity**, which is what
 *      makes the existing A9.2.3 scheduler rebuild. Without it, a new build
 *      directory appears and the generation stays CURRENT for ever.
 *   6. **A pinned list is not a completeness claim.** MANUAL discovery reads
 *      UNKNOWN even though the operator named an exact set, because an operator
 *      naming two databases is not evidence that there are two.
 *
 * Everything here is synthetic: a fixture repository, fixture compilation
 * databases and an isolated data directory. Nothing reaches a live daemon, a
 * live socket, a real database or a registered repository.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/git.h"
#include "atlas/reconcile.h"
#include "atlas/sem.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_ops.h"
#include "atlas/sem_schedule.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    atlas_db *db;
    atlas_git *g;
    int64_t repo_id;
} env;

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, err), err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_datadir_db_path(fx_data_dir(&e->fx), &db_path, err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);
    atlas_buf_free(&db_path);

    T_OK(atlas_git_open(fx_repo(&e->fx), &e->g, err), err);
    const char *root = atlas_git_root(e->g);
    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = atlas_git_common_dir(e->g);
    id.common_dir_len = strlen((const char *)id.common_dir);
    id.git_dir = atlas_git_dir(e->g);
    id.git_dir_len = strlen((const char *)id.git_dir);
    id.object_format = atlas_git_object_format(e->g);
    T_OK(atlas_db_repo_add(e->db, "fx", &id, &e->repo_id, err), err);
}

static void env_close(env *e) {
    atlas_git_close(e->g);
    atlas_db_close(e->db);
    fx_close(&e->fx);
}

static void run_file_pass(env *e, atlas_err *err) {
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    opts.full = true;
    atlas_reconcile_summary sum;
    atlas_reconcile_summary_init(&sum);
    T_OK(atlas_reconcile_run(e->db, e->g, e->repo_id, &opts, &sum, err), err);
}

/* Writes one compilation database at `rel`, naming one source.
 *
 * The document is written under a directory the fixture creates, so a test can
 * plant several at different depths and check that the walk reaches all of
 * them. */
static void mkdirs(env *e, const char *rel, atlas_err *err) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s", rel);
    for (char *p = path; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        /* Already-there is not an error: the fixture plants several databases
         * under one build directory. */
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)fx_mkdir(fx_repo(&e->fx), path, &ignored);
        *p = '/';
    }
    atlas_err ignored;
    atlas_err_init(&ignored);
    (void)fx_mkdir(fx_repo(&e->fx), path, &ignored);
    (void)err;
}

static void write_compdb_at(env *e, const char *rel, const char *source, atlas_err *err) {
    char dir[512];
    (void)snprintf(dir, sizeof dir, "%s", rel);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        mkdirs(e, dir, err);
    }
    atlas_buf doc = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&doc, err,
                           "[{\"directory\":\"%s\","
                           "\"arguments\":[\"cc\",\"-std=gnu11\",\"-c\",\"%s\"],"
                           "\"file\":\"%s\"}]",
                           fx_repo(&e->fx), source, source),
         err);
    T_OK(fx_write(fx_repo(&e->fx), rel, atlas_buf_cstr(&doc), err), err);
    atlas_buf_free(&doc);
}

static void repo_of(env *e, atlas_repo_info *out, atlas_err *err) {
    bool found = false;
    atlas_repo_info_init(out);
    T_OK(atlas_db_repo_get(e->db, "fx", out, &found, err), err);
    T_REQUIRE_MSG(found, "the fixture repository is not registered");
}

/* One bounded walk, against the repository's stored configuration. */
static void discover(env *e, atlas_sem_discovery_result *out, atlas_err *err) {
    atlas_repo_info info;
    repo_of(e, &info, err);
    atlas_sem_discovery_result_init(out);
    T_OK(atlas_sem_discovery_run(e->db, &info, out, err), err);
    atlas_repo_info_free(&info);
}

static void plan_of(env *e, bool policy_default, atlas_sem_plan *out, atlas_err *err) {
    atlas_repo_info info;
    repo_of(e, &info, err);
    T_OK(atlas_sem_plan_for_with_default(e->db, &info, false, policy_default, out, err), err);
    atlas_repo_info_free(&info);
}

static void identity_of(env *e, char out[65], atlas_err *err) {
    atlas_repo_info info;
    repo_of(e, &info, err);
    T_OK(atlas_sem_source_identity(e->db, &info, out, err), err);
    atlas_repo_info_free(&info);
}

/* Writes the repository's configuration, leaving the discovery verdict alone —
 * which is what `atlas_db_sem_config_set` does, because the verdict is derived
 * and only a walk writes it. */
static void configure(env *e, const char *const *excludes, size_t n_excl,
                      atlas_sem_discovery_mode mode, atlas_err *err) {
    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    T_OK(atlas_db_sem_config_get(e->db, e->repo_id, &cfg, err), err);
    cfg.repo_id = e->repo_id;
    cfg.discovery_mode = mode;
    if (excludes != NULL) {
        T_OK(atlas_sem_config_pack(excludes, n_excl, &cfg.excludes, err), err);
    }
    T_OK(atlas_db_sem_config_set(e->db, &cfg, err), err);
    atlas_sem_config_free(&cfg);
}

static size_t accepted_of(const atlas_sem_discovery_result *r) {
    size_t n = 0;
    for (size_t i = 0; i < r->count; i++) {
        if (r->inputs[i].accepted) {
            n++;
        }
    }
    return n;
}

static const atlas_sem_input *find_input(const atlas_sem_discovery_result *r, const char *path) {
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->inputs[i].path, path) == 0) {
            return &r->inputs[i];
        }
    }
    return NULL;
}

/* --- §23: any number of compilation databases -------------------------------- */

static void test_discovery_scales_from_zero_to_many(void) {
    /* §9's regression matrix as one test, because the interesting property is
     * that the answer is the *count that is there* at every step rather than a
     * particular number. A9.2.3 shipped a defect where the second database
     * silently vanished; the shape of that defect is invisible at n = 1, so the
     * only useful assertion is over a range. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);

    /* Zero. The walk covered the universe and found nothing, which is COMPLETE
     * — "I looked and there is nothing here" is a different statement from "I
     * have not looked", and only the first can ever support an absence. */
    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    T_EQ_INT((int)r.state, (int)ATLAS_SEM_DISC_COMPLETE);
    T_EQ_INT((int)accepted_of(&r), 0);

    static const int STEPS[] = {1, 2, 3, 10};
    int made = 0;
    for (size_t s = 0; s < sizeof STEPS / sizeof STEPS[0]; s++) {
        while (made < STEPS[s]) {
            char rel[128];
            (void)snprintf(rel, sizeof rel, "build/b%d/compile_commands.json", made);
            write_compdb_at(&e, rel, "a.c", &err);
            made++;
        }
        discover(&e, &r, &err);
        T_CHECK_MSG((int)accepted_of(&r) == STEPS[s],
                    "with %d compilation databases on disk the walk accepted %zu", STEPS[s],
                    accepted_of(&r));
        T_EQ_INT((int)r.state, (int)ATLAS_SEM_DISC_COMPLETE);
    }

    /* And the order is deterministic — by the reported path, never by whatever
     * `readdir` happened to return, so two runs produce one identity. */
    atlas_sem_discovery_result again;
    discover(&e, &again, &err);
    for (size_t i = 0; i < r.count && i < again.count; i++) {
        T_CHECK(strcmp(r.inputs[i].path, again.inputs[i].path) == 0);
    }

    env_close(&e);
}

/* --- §28: one file is one input, however many paths reach it ------------------ */

static void test_a_symlinked_database_is_not_a_second_input(void) {
    /* The case is on disk in the repository this season was developed in: a
     * top-level `compile_commands.json` symlinked into `build/`. It must not
     * become two semantic inputs, and — the part that matters more — the link
     * must be *shown* as a refused candidate rather than silently skipped. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);
    write_compdb_at(&e, "build/compile_commands.json", "a.c", &err);

    T_OK(fx_symlink(fx_repo(&e.fx), "build/compile_commands.json", "compile_commands.json", &err),
         &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 1);
    T_EQ_INT((int)r.count, 2);

    const atlas_sem_input *real = find_input(&r, "build/compile_commands.json");
    const atlas_sem_input *linked = find_input(&r, "compile_commands.json");
    T_REQUIRE(real != NULL && linked != NULL);
    T_CHECK(real->accepted);
    T_CHECK_MSG(!linked->accepted, "a symlinked compilation database was followed");
    /* Refused for being a link rather than for being a duplicate: Atlas never
     * opened it, so it does not know what it points at. */
    T_CHECK(strcmp(linked->reject_reason, ATLAS_SEM_REJECT_SYMLINK) == 0);
    T_CHECK(atlas_sem_reject_reason_is_known(linked->reject_reason));

    env_close(&e);
}

/* --- §27: a candidate that cannot be used is shown, with a reason ------------- */

static void test_a_malformed_database_is_visible_and_recovers(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);
    write_compdb_at(&e, "build/good/compile_commands.json", "a.c", &err);
    mkdirs(&e, "build/bad", &err);
    T_OK(fx_write(fx_repo(&e.fx), "build/bad/compile_commands.json", "{ this is not json",
                  &err),
         &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    /* The good one is still accepted: one unreadable file must never erase a
     * repository's whole build description. */
    T_EQ_INT((int)accepted_of(&r), 1);
    const atlas_sem_input *bad = find_input(&r, "build/bad/compile_commands.json");
    T_REQUIRE(bad != NULL);
    T_CHECK(!bad->accepted);
    T_CHECK(strcmp(bad->reject_reason, ATLAS_SEM_REJECT_MALFORMED) == 0);
    /* The *walk* is still COMPLETE: Atlas looked everywhere it said it would.
     * What the rejected candidate costs is coverage of that candidate's units,
     * which the generation's own counts carry — two different problems, kept
     * apart on purpose. */
    T_EQ_INT((int)r.state, (int)ATLAS_SEM_DISC_COMPLETE);

    /* Fix it. Recovery is automatic in the sense that matters here: nothing has
     * to be told the file was repaired. */
    write_compdb_at(&e, "build/bad/compile_commands.json", "a.c", &err);
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 2);
    bad = find_input(&r, "build/bad/compile_commands.json");
    T_REQUIRE(bad != NULL);
    T_CHECK(bad->accepted);
    T_CHECK(bad->reject_reason[0] == '\0');

    env_close(&e);
}

/* --- A9.2.5 / GAP-3: every obstacle, with its exact path ---------------------
 *
 * A9.2.4 kept the *first* reason a walk fell short and no path at all, so one
 * declared `--exclude` consumed the only slot and masked every unreadable
 * directory for the rest of the walk. On `/opt/atlas` itself that is exactly
 * what happened: `discovery_limit` read "an operator excluded a subtree from the
 * search" and nothing could say what else had been missed. */

static const atlas_sem_obstacle *find_obstacle(const atlas_sem_discovery_result *r,
                                               const char *path) {
    for (size_t i = 0; i < r->obstacle_count; i++) {
        if (strcmp(r->obstacles[i].path, path) == 0) {
            return &r->obstacles[i];
        }
    }
    return NULL;
}

static void test_an_unreadable_directory_is_recorded_with_its_path(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "secret", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "secret/compile_commands.json", "[]", &err), &err);
    write_compdb_at(&e, "compile_commands.json", "src/a.c", &err);
    /* Unreadable to this process. If the suite is running as root the mode is
     * ignored, so the test skips rather than asserting something untrue. */
    T_OK(fx_chmod(fx_repo(&e.fx), "secret", 0000, &err), &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);

    if (geteuid() == 0) {
        T_CHECK_MSG(true, "running as root: a 0000 directory is still readable, so skipped");
    } else {
        T_CHECK_MSG(r.state == ATLAS_SEM_DISC_PARTIAL,
                    "a directory Atlas could not enter must make the search PARTIAL");
        const atlas_sem_obstacle *ob = find_obstacle(&r, "secret");
        T_CHECK_MSG(ob != NULL,
                    "the unreadable directory must be recorded by its exact path, not merely "
                    "counted (%zu obstacles recorded)",
                    r.obstacle_count);
        if (ob != NULL) {
            T_CHECK_MSG(strcmp(ob->reason, ATLAS_SEM_OBSTACLE_UNREADABLE_DIR) == 0,
                        "expected the unreadable-directory reason, got %s", ob->reason);
            T_CHECK(atlas_sem_obstacle_reason_is_known(ob->reason));
        }
        /* The walk carries on: the readable database beside it is still found. */
        T_CHECK_MSG(find_input(&r, "compile_commands.json") != NULL,
                    "an unreadable directory must not abandon the rest of the walk");
    }
    /* Restore, so the fixture can be removed. */
    (void)fx_chmod(fx_repo(&e.fx), "secret", 0755, &err);
    env_close(&e);
}

static void test_an_exclusion_does_not_mask_a_later_obstacle(void) {
    /* The defect this whole table exists for. Both obstacles must be present and
     * neither may hide the other, whichever the walk met first. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "vendor", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "vendor/compile_commands.json", "[]", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "zlocked", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "zlocked/compile_commands.json", "[]", &err), &err);
    write_compdb_at(&e, "compile_commands.json", "src/a.c", &err);
    T_OK(fx_chmod(fx_repo(&e.fx), "zlocked", 0000, &err), &err);

    const char *excl[] = {"vendor"};
    configure(&e, excl, 1, ATLAS_SEM_DISCMODE_AUTOMATIC, &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);

    const atlas_sem_obstacle *ex = find_obstacle(&r, "vendor");
    T_CHECK_MSG(ex != NULL, "the excluded subtree must be recorded by path");
    if (ex != NULL) {
        T_CHECK(strcmp(ex->reason, ATLAS_SEM_OBSTACLE_EXCLUDED) == 0);
    }
    if (geteuid() != 0) {
        const atlas_sem_obstacle *lk = find_obstacle(&r, "zlocked");
        T_CHECK_MSG(lk != NULL,
                    "an exclusion must not consume the only slot and hide an unreadable "
                    "directory — the A9.2.4 defect (%zu obstacles recorded)",
                    r.obstacle_count);
        if (lk != NULL) {
            T_CHECK(strcmp(lk->reason, ATLAS_SEM_OBSTACLE_UNREADABLE_DIR) == 0);
        }
    }
    /* The one-line summary is still produced, because existing readers use it. */
    T_CHECK(r.limit_reached && r.limit_detail[0] != '\0');

    (void)fx_chmod(fx_repo(&e.fx), "zlocked", 0755, &err);
    env_close(&e);
}

static void test_obstacles_are_persisted_and_deterministic(void) {
    /* Persisted by the same transaction that records the candidates, read back
     * in the walk's own order, and stable across two walks over an unchanged
     * tree whatever order `readdir` returned entries in. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "aaa", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "mmm", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "zzz", &err), &err);
    write_compdb_at(&e, "compile_commands.json", "src/a.c", &err);
    const char *excl[] = {"aaa", "mmm", "zzz"};
    configure(&e, excl, 3, ATLAS_SEM_DISCMODE_AUTOMATIC, &err);

    atlas_sem_discovery_result r1;
    discover(&e, &r1, &err);
    T_CHECK_MSG(r1.obstacle_count == 3, "expected three excluded subtrees, got %zu",
                r1.obstacle_count);
    /* Sorted by path, so the order does not depend on the filesystem. */
    T_CHECK(strcmp(r1.obstacles[0].path, "aaa") == 0);
    T_CHECK(strcmp(r1.obstacles[1].path, "mmm") == 0);
    T_CHECK(strcmp(r1.obstacles[2].path, "zzz") == 0);

    atlas_sem_obstacle stored[ATLAS_SEM_DISCOVERY_MAX_OBSTACLES];
    size_t n = 0;
    bool trunc = true;
    T_OK(atlas_db_sem_obstacles_get(e.db, e.repo_id, stored,
                                    ATLAS_SEM_DISCOVERY_MAX_OBSTACLES, &n, &trunc, &err),
         &err);
    T_CHECK_MSG(n == 3, "the obstacle list must be persisted, got %zu rows", n);
    T_CHECK(!trunc);
    for (size_t i = 0; i < n && i < 3; i++) {
        T_CHECK(strcmp(stored[i].path, r1.obstacles[i].path) == 0);
        T_CHECK(atlas_sem_obstacle_reason_is_known(stored[i].reason));
    }

    /* A second walk over the same tree produces the same list — and replaces
     * rather than appends, which is what lets the list shrink to nothing when a
     * directory's permissions are repaired. */
    atlas_sem_discovery_result r2;
    discover(&e, &r2, &err);
    T_CHECK(r2.obstacle_count == r1.obstacle_count);
    size_t n2 = 0;
    T_OK(atlas_db_sem_obstacles_get(e.db, e.repo_id, stored,
                                    ATLAS_SEM_DISCOVERY_MAX_OBSTACLES, &n2, NULL, &err),
         &err);
    T_CHECK_MSG(n2 == 3, "a second walk must replace the list, not append to it (got %zu)", n2);

    /* Withdrawing the exclusions empties the list: "Atlas met no obstacle" is a
     * statement it must be able to make, and the only honest record of a
     * repaired directory is the row's absence. */
    configure(&e, (const char *const *)NULL, 0, ATLAS_SEM_DISCMODE_AUTOMATIC, &err);
    {
        atlas_sem_config cfg;
        atlas_sem_config_init(&cfg);
        T_OK(atlas_db_sem_config_get(e.db, e.repo_id, &cfg, &err), &err);
        cfg.repo_id = e.repo_id;
        atlas_buf_reset(&cfg.excludes);
        T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
        atlas_sem_config_free(&cfg);
    }
    atlas_sem_discovery_result r3;
    discover(&e, &r3, &err);
    size_t n3 = 1;
    T_OK(atlas_db_sem_obstacles_get(e.db, e.repo_id, stored,
                                    ATLAS_SEM_DISCOVERY_MAX_OBSTACLES, &n3, NULL, &err),
         &err);
    T_CHECK_MSG(n3 == 0, "withdrawing every exclusion must empty the obstacle list (got %zu)",
                n3);
    T_CHECK_MSG(r3.state == ATLAS_SEM_DISC_COMPLETE,
                "a walk that met no obstacle must be COMPLETE");

    env_close(&e);
}

/* --- §7: an excluded subtree is a hole in the universe, and it is shown ------- */

static void test_an_exclusion_makes_the_search_partial(void) {
    /* An operator saying "do not look there" is not the statement "there is
     * nothing there", and conflating them is how DID NOT DISCOVER becomes
     * PROVEN NOT TO EXIST. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);
    write_compdb_at(&e, "build/compile_commands.json", "a.c", &err);
    write_compdb_at(&e, "vendor/build/compile_commands.json", "a.c", &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 2);
    T_EQ_INT((int)r.state, (int)ATLAS_SEM_DISC_COMPLETE);

    const char *excl[] = {"vendor"};
    configure(&e, excl, 1, ATLAS_SEM_DISCMODE_AUTOMATIC, &err);
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 1);
    T_CHECK_MSG(r.state == ATLAS_SEM_DISC_PARTIAL,
                "a subtree Atlas was told not to enter still reported a complete search");
    T_CHECK(r.excluded_subtrees > 0);
    /* And the reason is *recorded*, because a PARTIAL verdict with nothing
     * saying why tells an operator something was missed without telling them
     * what — the same rule A8-CI applies to a bound, widened to every reason a
     * search fell short. */
    T_CHECK_MSG(r.limit_reached && r.limit_detail[0] != '\0',
                "an excluded subtree made the search PARTIAL with no reason recorded");

    /* The exclusion must not abandon the rest of the walk. An earlier cut used
     * one flag for "record why this is partial" and "stop walking", so an
     * ordinary `--exclude` silently skipped every sibling directory not yet
     * visited: the search returned PARTIAL, which was true, and found one
     * compilation database instead of the two that were there. */
    write_compdb_at(&e, "zzz/compile_commands.json", "a.c", &err);
    discover(&e, &r, &err);
    T_CHECK_MSG(find_input(&r, "zzz/compile_commands.json") != NULL,
                "an exclusion abandoned the rest of the walk");
    T_EQ_INT((int)accepted_of(&r), 2);

    /* And the component-boundary rule: `vendor` must not exclude `vendorish`. */
    write_compdb_at(&e, "vendorish/compile_commands.json", "a.c", &err);
    discover(&e, &r, &err);
    T_CHECK_MSG(find_input(&r, "vendorish/compile_commands.json") != NULL,
                "an exclusion matched on a substring rather than a path component");

    env_close(&e);
}

/* --- §6: a pinned list is not a completeness claim ---------------------------- */

static void test_manual_mode_never_claims_a_complete_search(void) {
    /* The lesson this season had to buy. The repository that exposed the problem
     * had a hand-written list of two databases, the list was wrong, and no flag
     * the operator could have ticked would have made it right — because the
     * operator did not know either. An assertion of completeness by somebody who
     * has not looked is not evidence of completeness. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);
    write_compdb_at(&e, "build/a/compile_commands.json", "a.c", &err);
    write_compdb_at(&e, "build/b/compile_commands.json", "a.c", &err);

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    cfg.repo_id = e.repo_id;
    cfg.discovery_mode = ATLAS_SEM_DISCMODE_MANUAL;
    const char *pinned[] = {"build/a/compile_commands.json"};
    T_OK(atlas_sem_config_pack(pinned, 1, &cfg.compdbs, &err), &err);
    T_OK(atlas_db_sem_config_set(e.db, &cfg, &err), &err);
    atlas_sem_config_free(&cfg);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 1);
    T_CHECK_MSG(r.state == ATLAS_SEM_DISC_UNKNOWN,
                "a pinned list was treated as a complete search");
    const atlas_sem_input *pin = find_input(&r, "build/a/compile_commands.json");
    T_REQUIRE(pin != NULL);
    T_EQ_INT((int)pin->origin, (int)ATLAS_SEM_INPUT_PINNED);
    /* The second database is on disk and Atlas never looked, so it is not in the
     * result at all — which is the honest outcome and exactly why the verdict is
     * UNKNOWN rather than COMPLETE. */
    T_CHECK(find_input(&r, "build/b/compile_commands.json") == NULL);

    /* And a pinned path in AUTOMATIC mode is *both*: named by an operator and
     * found by the walk. Two different facts about one path, and an operator
     * debugging a build description needs to tell them apart. */
    configure(&e, NULL, 0, ATLAS_SEM_DISCMODE_AUTOMATIC, &err);
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 2);
    pin = find_input(&r, "build/a/compile_commands.json");
    T_REQUIRE(pin != NULL);
    T_EQ_INT((int)pin->origin, (int)ATLAS_SEM_INPUT_BOTH);

    env_close(&e);
}

/* --- §17 and §24: a change to the input set is a rebuild trigger -------------- */

static void test_a_new_database_moves_the_source_identity(void) {
    /* The whole of §17, and the reason there is no second scheduler: discovery
     * feeds the source identity, and the identity was already what A9.2.3
     * derived every rebuild decision from. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);
    write_compdb_at(&e, "build/a/compile_commands.json", "a.c", &err);
    write_compdb_at(&e, "build/b/compile_commands.json", "a.c", &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 2);
    char before[65];
    identity_of(&e, before, &err);
    T_CHECK(before[0] != '\0');

    /* A third appears. Nothing else about the repository changes. */
    write_compdb_at(&e, "build/c/compile_commands.json", "a.c", &err);
    char unwalked[65];
    identity_of(&e, unwalked, &err);
    T_CHECK_MSG(strcmp(before, unwalked) == 0,
                "the identity moved before a walk had noticed the new database");

    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 3);
    char after[65];
    identity_of(&e, after, &err);
    T_CHECK_MSG(strcmp(before, after) != 0,
                "a new compilation database left the source identity unmoved");

    /* And one going away moves it too, in the other direction. */
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/build/b/compile_commands.json", fx_repo(&e.fx));
    T_REQUIRE(unlink(path) == 0);
    discover(&e, &r, &err);
    T_EQ_INT((int)accepted_of(&r), 2);
    char removed[65];
    identity_of(&e, removed, &err);
    T_CHECK_MSG(strcmp(after, removed) != 0,
                "a removed compilation database left the source identity unmoved");

    env_close(&e);
}

/* --- §18: what the scheduler does about all of that --------------------------- */

static void test_the_scheduler_builds_when_inputs_appear(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);

    /* No inputs: maintenance is on and there is nothing to build from, which is
     * its own state and not a refusal. */
    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    atlas_sem_plan p;
    plan_of(&e, true, &p, &err);
    T_CHECK(!p.should_build);
    T_EQ_INT((int)p.activity, (int)ATLAS_SEM_ACT_NO_INPUTS);

    /* An input appears and discovery notices. Nothing had to be enabled: the
     * repository was never configured, and that is the point. */
    write_compdb_at(&e, "build/compile_commands.json", "a.c", &err);
    discover(&e, &r, &err);
    plan_of(&e, true, &p, &err);
    T_CHECK_MSG(p.should_build,
                "a repository with a discoverable build input did not schedule a build");
    T_EQ_INT((int)p.activity, (int)ATLAS_SEM_ACT_UNAVAILABLE);
    T_EQ_INT((int)p.discovery, (int)ATLAS_SEM_DISC_COMPLETE);
    T_EQ_INT((int)p.inputs_accepted, 1);

    env_close(&e);
}

/* --- §32: the walk never leaves the repository, and never enters .git --------- */

static void test_the_walk_stays_inside_the_repository(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);

    /* A compilation database inside `.git`, which the walk must never enter, and
     * one reachable only through a symlinked *directory*, which it must never
     * traverse. */
    write_compdb_at(&e, ".git/compile_commands.json", "a.c", &err);
    write_compdb_at(&e, "real/compile_commands.json", "a.c", &err);
    T_OK(fx_symlink(fx_repo(&e.fx), "real", "aliased", &err), &err);

    atlas_sem_discovery_result r;
    discover(&e, &r, &err);
    T_CHECK_MSG(find_input(&r, ".git/compile_commands.json") == NULL,
                "the walk entered the git metadata directory");
    T_CHECK_MSG(find_input(&r, "aliased/compile_commands.json") == NULL,
                "the walk traversed a symlinked directory");
    T_EQ_INT((int)accepted_of(&r), 1);
    T_CHECK(find_input(&r, "real/compile_commands.json") != NULL);

    env_close(&e);
}

/* --- §22: incomplete discovery prevents negative proof ------------------------ */

static void test_incomplete_discovery_refuses_coverage(void) {
    /* The fold that carries this season into A9.2.2's model. Discovery is a
     * fourth way for coverage to be incomplete, and the one that was previously
     * inexpressible: every configured input processed, every source in scope
     * read, and no idea whether those were all the inputs. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int f(void){return 0;}\n", &err), &err);
    run_file_pass(&e, &err);

    /* A generation that is perfect on every A9.2.3 axis, with a discovery
     * verdict Atlas could not establish. */
    T_OK(atlas_db_exec_sql(
             e.db,
             "INSERT INTO sem_generations(repo_id, commit_id, status, started_at, completed_at,"
             "  tu_total, tu_complete, analyzer_id, analyzer_version,"
             "  scope_discovery, scope_candidates, scope_covered, scope_uncovered, discovery)"
             " VALUES(1, '', 'COMPLETE', '2026-01-01T00:00:00Z', '2026-01-01T00:00:01Z',"
             "        1, 1, '" ATLAS_SEM_ANALYZER_ID "', 1, 'DECLARED', 1, 1, 0, 'UNKNOWN');"
             "INSERT INTO sem_current(repo_id, generation_id) VALUES(1, last_insert_rowid());",
             &err),
         &err);

    atlas_sem_plan p;
    plan_of(&e, true, &p, &err);
    T_CHECK_MSG(!p.coverage_complete,
                "a generation whose build-input search Atlas could not vouch for reported "
                "complete coverage");

    /* Establish the search and the same generation becomes usable, which is what
     * keeps the gate from being uselessly cautious rather than correctly so. */
    T_OK(atlas_db_exec_sql(e.db, "UPDATE sem_generations SET discovery = 'COMPLETE';", &err),
         &err);
    plan_of(&e, true, &p, &err);
    T_CHECK_MSG(p.coverage_complete,
                "a generation with a complete search and complete scope still refused coverage");

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"discovery scales from zero to many databases", test_discovery_scales_from_zero_to_many},
    {"a symlinked database is not a second input",
     test_a_symlinked_database_is_not_a_second_input},
    {"a malformed database is visible and recovers",
     test_a_malformed_database_is_visible_and_recovers},
    {"an unreadable directory is recorded with its exact path",
     test_an_unreadable_directory_is_recorded_with_its_path},
    {"an exclusion does not mask a later obstacle",
     test_an_exclusion_does_not_mask_a_later_obstacle},
    {"obstacles are persisted, ordered and replaced whole",
     test_obstacles_are_persisted_and_deterministic},
    {"an exclusion makes the search partial", test_an_exclusion_makes_the_search_partial},
    {"manual mode never claims a complete search",
     test_manual_mode_never_claims_a_complete_search},
    {"a new database moves the source identity", test_a_new_database_moves_the_source_identity},
    {"the scheduler builds when inputs appear", test_the_scheduler_builds_when_inputs_appear},
    {"the walk stays inside the repository", test_the_walk_stays_inside_the_repository},
    {"incomplete discovery refuses coverage", test_incomplete_discovery_refuses_coverage},
};

ATLAS_TEST_MAIN("sem_discovery", TESTS)

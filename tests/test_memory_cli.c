/* Atlas - A12.1 T16: the `memory` command family's service layer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One test per form (the plan's own requirement), against the service
 * functions directly -- `src/core/service_memory.c` -- rather than through
 * the built binary, which `scripts/smoke.sh` already exercises for the
 * wiring (COMMANDS[], the argument parser, exit codes). `scan` and
 * `reconcile` are argument-validation only here: both are unconditionally
 * daemon-served (T11's `memory.put`/`memory.reconcile`), and driving them
 * against a live daemon is `tests/test_memory_reconcile_live.c`'s job, not
 * this file's.
 *
 * Two db_memory.c reads are new this task (`atlas_db_memory_generation_
 * diffs_list`, `atlas_db_memory_pack_reliance_get`) and are asserted
 * directly, against rows seeded through the existing typed writers rather
 * than through a full reconciliation pass -- `test_memory_pack.c`'s own
 * practice for exercising one layer's functions without re-running the
 * layer below it.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/memory.h"
#include "atlas/service.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define REPO_NAME "fixture"

typedef struct cli_env {
    fixture fx;
    atlas_ctx *ctx;
    int64_t repo_id;
} cli_env;

static void env_open(cli_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_REQUIRE(fx_open(&e->fx, &err) == ATLAS_OK);
    T_REQUIRE(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err) == ATLAS_OK);
    T_REQUIRE(fx_write(fx_repo(&e->fx), "README.md", "seed\n", &err) == ATLAS_OK);
    T_REQUIRE(fx_add_all(&e->fx, fx_repo(&e->fx), &err) == ATLAS_OK);
    T_REQUIRE(fx_commit(&e->fx, fx_repo(&e->fx), "initial commit", &err) == ATLAS_OK);

    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = fx_data_dir(&e->fx);
    T_REQUIRE(atlas_ctx_open(&opts, &e->ctx, &err) == ATLAS_OK);

    atlas_repo_info ri;
    memset(&ri, 0, sizeof ri);
    T_REQUIRE(atlas_service_repo_add(e->ctx, fx_repo(&e->fx), REPO_NAME, &ri, &err) == ATLAS_OK);
    e->repo_id = ri.id;
    atlas_repo_info_free(&ri);
}

static void env_close(cli_env *e) {
    atlas_ctx_close(e->ctx);
    e->ctx = NULL;
    fx_close(&e->fx);
}

/* A sink that never runs -- used where the call must be refused before any
 * row is produced. */
static atlas_status sink_never(const atlas_memory_render *mr, void *ud, atlas_err *err) {
    (void)mr;
    (void)ud;
    (void)err;
    T_CHECK_MSG(false, "the sink must not have been called");
    return ATLAS_OK;
}

/* --- status ----------------------------------------------------------------- */

typedef struct status_capture {
    bool called;
    char form[16];
    char repo[64];
    bool generation_found;
    size_t source_count;
    bool sources_truncated;
    char policy_state[16];
} status_capture;

static atlas_status status_sink(const atlas_memory_render *mr, void *ud, atlas_err *err) {
    (void)err;
    status_capture *c = (status_capture *)ud;
    c->called = true;
    (void)snprintf(c->form, sizeof c->form, "%s", mr->form);
    (void)snprintf(c->repo, sizeof c->repo, "%s", mr->repo != NULL ? mr->repo : "");
    c->generation_found = mr->generation_found;
    c->source_count = mr->source_count;
    c->sources_truncated = mr->sources_truncated;
    (void)snprintf(c->policy_state, sizeof c->policy_state,
                   "%s", mr->policy_state != NULL ? mr->policy_state : "");
    return ATLAS_OK;
}

static void test_status_needs_a_repo(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_service_memory_status(NULL, NULL, sink_never, NULL, &err), ATLAS_ERR_USAGE,
                &err);
    T_FAILS_WITH(atlas_service_memory_status_remote(NULL, sink_never, NULL, &err), ATLAS_ERR_USAGE,
                &err);
}

static void test_status_refuses_a_null_ctx_locally(void) {
    /* The local form has no offline path of its own kind: without a `ctx` it
     * states the A7.1 gap rather than dereferencing one -- the CLI's own
     * `run_memory` picks the remote form instead when `ctx` is NULL, and this
     * asserts the local function's own half of that contract. */
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_service_memory_status(NULL, REPO_NAME, sink_never, NULL, &err),
                ATLAS_ERR_CONFIG, &err);
}

static void test_status_reports_policy_and_repo_with_no_sources(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    status_capture c;
    memset(&c, 0, sizeof c);
    T_OK(atlas_service_memory_status(e.ctx, REPO_NAME, status_sink, &c, &err), &err);
    T_CHECK(c.called);
    T_EQ_STR(c.form, "status");
    T_EQ_STR(c.repo, REPO_NAME);
    T_CHECK(!c.generation_found);
    T_EQ_INT((int)c.source_count, 0);
    T_CHECK(!c.sources_truncated);
    /* Context §5: the policy state is always present, whatever it resolves
     * to on this machine -- never an empty string, and never a field the
     * caller has to guess is missing. */
    T_CHECK(c.policy_state[0] != '\0');
    env_close(&e);
}

/* Advisor finding #1 on this task: `ATLAS_MEMORY_MAX_SOURCES` bounds a
 * policy's declared source list, not a repository's `memory_sources` rows --
 * those are never deleted, so a policy edited over the life of a repository
 * can leave more rows than either renderer has room to show. Silently
 * dropping the extras would be exactly the "nothing is silently truncated"
 * rule this project fights for; this proves the row array is instead capped
 * *and* the caller is told so. */
static void test_status_reports_truncation_past_the_source_ceiling(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = atlas_ctx_db(e.ctx);

    for (unsigned i = 0; i < ATLAS_MEMORY_MAX_SOURCES + 1u; i++) {
        char path[64];
        (void)snprintf(path, sizeof path, "/ext/source-%u.md", i);
        int64_t id = 0;
        atlas_buf uid = ATLAS_BUF_INIT;
        T_OK(atlas_db_memory_source_upsert(db, e.repo_id, ATLAS_MEMORY_SOURCE_EXTERNAL_FILE, path,
                                           strlen(path), path, "2026-01-01T00:00:00Z", &id, &uid,
                                           &err),
            &err);
        atlas_buf_free(&uid);
    }

    status_capture c;
    memset(&c, 0, sizeof c);
    T_OK(atlas_service_memory_status(e.ctx, REPO_NAME, status_sink, &c, &err), &err);
    T_CHECK(c.called);
    T_EQ_INT((int)c.source_count, (int)ATLAS_MEMORY_MAX_SOURCES);
    T_CHECK(c.sources_truncated);
    env_close(&e);
}

/* --- scan / reconcile: argument validation only ----------------------------- */

static void test_scan_needs_a_repo(void) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t count = -1;
    T_FAILS_WITH(atlas_service_memory_scan(NULL, sink_never, NULL, &count, &err), ATLAS_ERR_USAGE,
                &err);
    T_EQ_INT((int)count, 0);
}

static void test_reconcile_needs_a_repo(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_service_memory_reconcile(NULL, sink_never, NULL, &err), ATLAS_ERR_USAGE,
                &err);
}

/* --- pack -------------------------------------------------------------------- */

typedef struct pack_capture {
    bool called;
    bool pack_found;
    bool pack_preview;
    bool pack_other_repo;
    int64_t claim_count;
} pack_capture;

static atlas_status pack_sink(const atlas_memory_render *mr, void *ud, atlas_err *err) {
    (void)err;
    pack_capture *c = (pack_capture *)ud;
    c->called = true;
    c->pack_found = mr->pack_found;
    c->pack_preview = mr->pack_preview;
    c->pack_other_repo = mr->pack_other_repo;
    c->claim_count = mr->pack_claim_count;
    return ATLAS_OK;
}

static void test_pack_needs_exactly_one_of_task_or_run(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(
        atlas_service_memory_pack(NULL, REPO_NAME, NULL, NULL, sink_never, NULL, &err),
        ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(
        atlas_service_memory_pack(NULL, REPO_NAME, "a task", "runX", sink_never, NULL, &err),
        ATLAS_ERR_USAGE, &err);
}

static void test_pack_refuses_a_null_ctx(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(
        atlas_service_memory_pack(NULL, REPO_NAME, "a task", NULL, sink_never, NULL, &err),
        ATLAS_ERR_CONFIG, &err);
}

static void test_pack_preview_over_no_sources_is_empty_but_ok(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    pack_capture c;
    memset(&c, 0, sizeof c);
    T_OK(atlas_service_memory_pack(e.ctx, REPO_NAME, "does the tree do X", NULL, pack_sink, &c,
                                   &err),
        &err);
    T_CHECK(c.called);
    T_CHECK(c.pack_preview);
    T_CHECK(c.pack_found);
    T_EQ_INT((int)c.claim_count, 0);
    env_close(&e);
}

static void test_pack_run_not_found(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    pack_capture c;
    memset(&c, 0, sizeof c);
    T_OK(atlas_service_memory_pack(e.ctx, REPO_NAME, NULL, "rnosuchrun", pack_sink, &c, &err),
        &err);
    T_CHECK(c.called);
    T_CHECK(!c.pack_found);
    T_CHECK(!c.pack_other_repo);
    env_close(&e);
}

/* A run whose frozen pack names a *different* repository is a distinct fact
 * from "no such run has a pack" -- advisor finding #2 on this task: the two
 * were collapsed into one `pack_found: false` and a caller was told a run did
 * not exist when it did, just not for the repository asked about. */
static void test_pack_run_belongs_to_another_repo(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = atlas_ctx_db(e.ctx);

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    p.repo_id = e.repo_id + 1; /* deliberately not this fixture's own repo id */
    T_OK(atlas_buf_set_str(&p.repo_identity_hash, "otheridentity", &err), &err);
    T_OK(atlas_buf_set_str(&p.decision_set_digest, "otherdecdigest", &err), &err);
    T_OK(atlas_buf_set_str(&p.source_set_digest, "othersrcdigest", &err), &err);
    T_OK(atlas_buf_set_str(&p.pack_digest, "otherpackdigest", &err), &err);
    T_OK(atlas_buf_set_str(&p.rendered, "other repo's body", &err), &err);
    T_OK(atlas_db_memory_pack_insert(db, "rotherrepo", &p, "2026-01-01T00:00:00Z", &err), &err);
    atlas_memory_pack_free(&p);

    pack_capture c;
    memset(&c, 0, sizeof c);
    T_OK(atlas_service_memory_pack(e.ctx, REPO_NAME, NULL, "rotherrepo", pack_sink, &c, &err),
        &err);
    T_CHECK(c.called);
    T_CHECK(!c.pack_found);
    T_CHECK(c.pack_other_repo);
    env_close(&e);
}

/* --- diff -------------------------------------------------------------------- */

typedef struct diff_capture {
    int rows;
    bool generation_found;
} diff_capture;

static atlas_status diff_sink(const atlas_memory_render *mr, void *ud, atlas_err *err) {
    (void)err;
    diff_capture *c = (diff_capture *)ud;
    c->rows++;
    c->generation_found = mr->diff_generation_found;
    return ATLAS_OK;
}

static void test_diff_needs_a_positive_generation(void) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t count = 0;
    T_FAILS_WITH(
        atlas_service_memory_diff(NULL, REPO_NAME, 0, sink_never, NULL, &count, &err),
        ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(
        atlas_service_memory_diff(NULL, REPO_NAME, -1, sink_never, NULL, &count, &err),
        ATLAS_ERR_USAGE, &err);
}

static void test_diff_refuses_a_null_ctx(void) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t count = 0;
    T_FAILS_WITH(atlas_service_memory_diff(NULL, REPO_NAME, 1, sink_never, NULL, &count, &err),
                ATLAS_ERR_CONFIG, &err);
}

static void test_diff_reports_a_generation_that_does_not_exist(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    diff_capture c;
    memset(&c, 0, sizeof c);
    int64_t count = -1;
    T_OK(atlas_service_memory_diff(e.ctx, REPO_NAME, 1, diff_sink, &c, &count, &err), &err);
    T_EQ_INT(c.rows, 1);
    T_CHECK(!c.generation_found);
    T_EQ_INT((int)count, 0);
    env_close(&e);
}

static void test_diff_lists_a_seeded_generation_in_order(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = atlas_ctx_db(e.ctx);

    int64_t gen_id = 0;
    T_OK(atlas_db_memory_generation_insert(db, e.repo_id, 1, ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
                                           "identityhash", "deadbeef", "decdigest", "srcdigest", 0,
                                           "2026-01-01T00:00:00Z", &gen_id, &err),
        &err);
    T_OK(atlas_db_memory_claim_diff_add(db, gen_id, "cclaimaaaa", ATLAS_MEMORY_DIFF_ADDED, "",
                                        &err),
        &err);
    T_OK(atlas_db_memory_claim_diff_add(db, gen_id, "cclaimbbbb", ATLAS_MEMORY_DIFF_SUPPORTED,
                                        "reason-token", &err),
        &err);

    diff_capture c;
    memset(&c, 0, sizeof c);
    int64_t count = -1;
    T_OK(atlas_service_memory_diff(e.ctx, REPO_NAME, 1, diff_sink, &c, &count, &err), &err);
    T_EQ_INT(c.rows, 2);
    T_CHECK(c.generation_found);
    T_EQ_INT((int)count, 2);
    env_close(&e);
}

/* --- patch ------------------------------------------------------------------- */

static void test_patch_needs_a_source(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_service_memory_patch(NULL, REPO_NAME, NULL, sink_never, NULL, &err),
                ATLAS_ERR_USAGE, &err);
}

static void test_patch_refuses_a_null_ctx(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(atlas_service_memory_patch(NULL, REPO_NAME, "msomeuid", sink_never, NULL, &err),
                ATLAS_ERR_CONFIG, &err);
}

static void test_patch_refuses_an_unregistered_source(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_status st =
        atlas_service_memory_patch(e.ctx, REPO_NAME, "mnosuchsource", sink_never, NULL, &err);
    T_CHECK(st != ATLAS_OK);
    env_close(&e);
}

/* --- trailer ------------------------------------------------------------------ */

typedef struct trailer_capture {
    bool called;
    bool compose;
    bool found;
} trailer_capture;

static atlas_status trailer_sink(const atlas_memory_render *mr, void *ud, atlas_err *err) {
    (void)err;
    trailer_capture *c = (trailer_capture *)ud;
    c->called = true;
    c->compose = mr->trailer_compose;
    c->found = mr->trailer_found;
    return ATLAS_OK;
}

static void test_trailer_needs_a_compose_or_a_show_form(void) {
    atlas_err err;
    atlas_err_init(&err);
    /* Neither given. */
    T_FAILS_WITH(
        atlas_service_memory_trailer(NULL, NULL, NULL, NULL, NULL, sink_never, NULL, &err),
        ATLAS_ERR_USAGE, &err);
    /* Both given -- the XOR refuses this too, deliberately: one call cannot
     * be composing a fresh block and showing a stored one at once. */
    T_FAILS_WITH(atlas_service_memory_trailer(NULL, "runX", "reasonX", "deadbeef", REPO_NAME,
                                              sink_never, NULL, &err),
                ATLAS_ERR_USAGE, &err);
}

static void test_trailer_compose_needs_a_reason(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(
        atlas_service_memory_trailer(NULL, "runX", NULL, NULL, NULL, sink_never, NULL, &err),
        ATLAS_ERR_USAGE, &err);
}

static void test_trailer_show_needs_a_repo(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_FAILS_WITH(
        atlas_service_memory_trailer(NULL, NULL, NULL, "deadbeef", NULL, sink_never, NULL, &err),
        ATLAS_ERR_USAGE, &err);
}

static void test_trailer_compose_refuses_a_run_with_no_frozen_pack(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = atlas_service_memory_trailer(e.ctx, "rnosuchrun", "reasonX", NULL, NULL,
                                                   sink_never, NULL, &err);
    /* `atlas_memory_trailer_compose`'s own refusal for a run with no frozen
     * pack -- ATLAS_ERR_INTEGRITY, exit 7, observed directly against the
     * built binary during this task's own Step 3. */
    T_CHECK(st == ATLAS_ERR_INTEGRITY);
    env_close(&e);
}

static void test_trailer_show_reports_no_binding_recorded(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    trailer_capture c;
    memset(&c, 0, sizeof c);
    T_OK(atlas_service_memory_trailer(e.ctx, NULL, NULL,
                                      "0000000000000000000000000000000000000000", REPO_NAME,
                                      trailer_sink, &c, &err),
        &err);
    T_CHECK(c.called);
    T_CHECK(!c.compose);
    T_CHECK(!c.found);
    env_close(&e);
}

/* --- db_memory.c: the two new reads ------------------------------------------ */

static atlas_status count_row_cb(const char *claim_uid, atlas_memory_diff_kind kind,
                                 const char *reason, void *ud, atlas_err *err) {
    (void)claim_uid;
    (void)kind;
    (void)reason;
    (void)err;
    int *n = (int *)ud;
    (*n)++;
    return ATLAS_OK;
}

static void test_generation_diffs_list_distinguishes_absent_from_empty(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    bool found = true;
    int n = 0;
    T_OK(atlas_db_memory_generation_diffs_list(atlas_ctx_db(e.ctx), e.repo_id, 1, count_row_cb, &n,
                                               &found, &err),
        &err);
    T_CHECK(!found);
    T_EQ_INT(n, 0);
    env_close(&e);
}

static void test_pack_reliance_get_never_gathered_when_no_row(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    bool checked = true, complete = true, found = true;
    atlas_buf uids = ATLAS_BUF_INIT;
    T_OK(atlas_db_memory_pack_reliance_get(atlas_ctx_db(e.ctx), "rnosuchrun", &checked, &complete,
                                           &uids, &found, &err),
        &err);
    T_CHECK(!found);
    T_CHECK(!checked);
    T_CHECK(!complete);
    T_EQ_INT((int)uids.len, 0);
    atlas_buf_free(&uids);
    env_close(&e);
}

static void test_pack_reliance_get_reads_back_what_reliance_set_wrote(void) {
    cli_env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = atlas_ctx_db(e.ctx);

    atlas_memory_pack p;
    atlas_memory_pack_init(&p);
    p.repo_id = e.repo_id;
    T_OK(atlas_buf_set_str(&p.repo_identity_hash, "identityhash", &err), &err);
    T_OK(atlas_buf_set_str(&p.decision_set_digest, "decdigest", &err), &err);
    T_OK(atlas_buf_set_str(&p.source_set_digest, "srcdigest", &err), &err);
    T_OK(atlas_buf_set_str(&p.pack_digest, "packdigest", &err), &err);
    T_OK(atlas_buf_set_str(&p.rendered, "body", &err), &err);
    T_OK(atlas_db_memory_pack_insert(db, "rreliancetest", &p, "2026-01-01T00:00:00Z", &err), &err);
    atlas_memory_pack_free(&p);

    /* Before any reliance check: never gathered, not "gathered zero". */
    bool checked = true, complete = false, found = false;
    atlas_buf uids = ATLAS_BUF_INIT;
    T_OK(atlas_db_memory_pack_reliance_get(db, "rreliancetest", &checked, &complete, &uids, &found,
                                           &err),
        &err);
    T_CHECK(found);
    T_CHECK(!checked);
    atlas_buf_free(&uids);

    /* T13's own write, merging in one matched claim. */
    T_OK(atlas_db_memory_pack_reliance_set(db, "rreliancetest", true, "1:10:cclaimaaaa,", &err),
        &err);
    checked = false;
    complete = false;
    found = false;
    T_OK(atlas_db_memory_pack_reliance_get(db, "rreliancetest", &checked, &complete, &uids, &found,
                                           &err),
        &err);
    T_CHECK(found);
    T_CHECK(checked);
    T_CHECK(complete);
    T_CHECK(uids.len > 0);
    atlas_buf_free(&uids);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"memory status needs a repository", test_status_needs_a_repo},
    {"memory status refuses a NULL local ctx", test_status_refuses_a_null_ctx_locally},
    {"memory status reports policy and repo over an empty repository",
     test_status_reports_policy_and_repo_with_no_sources},
    {"memory status reports truncation past the source ceiling",
     test_status_reports_truncation_past_the_source_ceiling},
    {"memory scan needs a repository", test_scan_needs_a_repo},
    {"memory reconcile needs a repository", test_reconcile_needs_a_repo},
    {"memory pack needs exactly one of --task or --run",
     test_pack_needs_exactly_one_of_task_or_run},
    {"memory pack refuses a NULL ctx", test_pack_refuses_a_null_ctx},
    {"memory pack preview over no sources is empty but ok",
     test_pack_preview_over_no_sources_is_empty_but_ok},
    {"memory pack --run reports not-found for an unknown run", test_pack_run_not_found},
    {"memory pack --run distinguishes another repository's pack from not-found",
     test_pack_run_belongs_to_another_repo},
    {"memory diff needs a positive generation", test_diff_needs_a_positive_generation},
    {"memory diff refuses a NULL ctx", test_diff_refuses_a_null_ctx},
    {"memory diff reports a generation that does not exist",
     test_diff_reports_a_generation_that_does_not_exist},
    {"memory diff lists a seeded generation in order",
     test_diff_lists_a_seeded_generation_in_order},
    {"memory patch needs a source", test_patch_needs_a_source},
    {"memory patch refuses a NULL ctx", test_patch_refuses_a_null_ctx},
    {"memory patch refuses an unregistered source", test_patch_refuses_an_unregistered_source},
    {"memory trailer needs a compose or a show form",
     test_trailer_needs_a_compose_or_a_show_form},
    {"memory trailer compose needs a reason", test_trailer_compose_needs_a_reason},
    {"memory trailer show needs a repo", test_trailer_show_needs_a_repo},
    {"memory trailer compose refuses a run with no frozen pack",
     test_trailer_compose_refuses_a_run_with_no_frozen_pack},
    {"memory trailer show reports no binding recorded",
     test_trailer_show_reports_no_binding_recorded},
    {"generation diffs list distinguishes absent from empty",
     test_generation_diffs_list_distinguishes_absent_from_empty},
    {"pack reliance get reports never-gathered when no row exists",
     test_pack_reliance_get_never_gathered_when_no_row},
    {"pack reliance get reads back what reliance set wrote",
     test_pack_reliance_get_reads_back_what_reliance_set_wrote},
};

ATLAS_TEST_MAIN("memory_cli", TESTS)

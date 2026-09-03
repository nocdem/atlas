/* Atlas - A12.1 T13: delivery, injection and the reliance check, against a
 * real writer thread.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `run_orch` (`src/daemon/writer.c`) is the seam T13 adds: a root-task SUBMIT
 * builds the Canonical Context Pack *before* `atlas_orch_apply` opens its
 * transaction, and a pack-delivering LEASE computes freshness *after* it
 * commits. Neither half is reachable through `atlas_orch_apply` called
 * directly against a fixture handle -- the shape every other orchestration
 * test in this tree uses (`tests/test_orch_run.c`'s own header says so) --
 * because that shape has no writer thread and therefore no `run_orch` at all.
 * This file is what a real `atlas_writer_start` thread is for, T11's own
 * precedent (`t11_writer_open`, `tests/support/reconcile_env.h`) one layer
 * over: every case here drives `ATLAS_ORCH_OP_SUBMIT`, `_LEASE` and
 * `_COMPLETE` through `atlas_writer_orch` and waits for the real background
 * thread to answer, exactly as the shipped `job.submit` / `dispatch.lease` /
 * `dispatch.complete` RPC handlers do one layer further out.
 *
 * Labelled `daemon` per the task brief, though it forks no separate process:
 * `atlas_workers_start` and `atlas_writer_start` are real daemon internals (a
 * thread pool and the single writer thread), which is what "daemon" names
 * throughout this suite's existing `daemon`-labelled-without-`RUN_SERIAL`
 * cases (`test_registry`, `test_ops`). No inotify watcher is started here, so
 * there is nothing to serialise against a sibling daemon test over.
 *
 * `rundriver.c`'s own composition and touched-paths gathering are not driven
 * end to end here: `fake-repo` does not echo the task text it receives
 * anywhere retrievable, and teaching it to was outside this task's file list.
 * Instead, each case reads the daemon's real delivery fields
 * (`context_pack`, `context_pack_status`, `memory_package`) off a real LEASE
 * grant and feeds them to `atlas_memory_pack_compose` itself -- the exact call
 * `rundriver.c`'s `drive_one` makes with the exact same struct fields -- which
 * proves the same claim about the same bytes one function call short of a live
 * driver invocation. `test_orch_driver.c` and `test_a11_run.c` already run
 * `fake-repo` through `drive_one` for other reasons and both still pass, which
 * is this file's evidence that the compose call sites this task added did not
 * break the ordinary path.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/memory.h"
#include "atlas/orch_ops.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "db/db_internal.h"
#include "support/fixture.h"
#include "support/reconcile_env.h"

/* --- policy ------------------------------------------------------------- */

static void live_policy(atlas_syspolicy *pol, const char *path) {
    memset(pol, 0, sizeof *pol);
    pol->state = ATLAS_SYSPOLICY_SYSTEM;
    if (path != NULL) {
        pol->memory_source_count = 1;
        pol->memory_sources[0].cls = ATLAS_MEMORY_SOURCE_REPO_FILE;
        pol->memory_sources[0].repo_name[0] = '\0';
        (void)snprintf(pol->memory_sources[0].path, sizeof pol->memory_sources[0].path, "%s",
                       path);
    }
}

/* --- a submitted, leased job ---------------------------------------------- */

/* `identity` and `commit` are read while `e->db` is still open and cached by
 * the caller: every case here closes `e->db` before starting the writer
 * (`t11_writer_open`'s own precondition, every T11 daemon case in this tree
 * follows it), so this function must not reach into `e->db` or `e->repo`
 * itself. */
static atlas_orch_op *build_submit(int64_t repo_id, const char *identity_hash, const char *commit,
                                   const char *task) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = repo_id;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.repo_identity_hash, identity_hash, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.source_commit, commit, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake-repo", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, task, &err), &err);
    T_OK(atlas_orch_argv_push(&op->spec.validations[0], "make", 4u, &err), &err);
    T_OK(atlas_orch_argv_push(&op->spec.validations[0], "pass", 4u, &err), &err);
    op->spec.validation_count = 1;
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = 3;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);
    return op;
}

static void submit(atlas_writer *w, const atlas_syspolicy *pol, int64_t repo_id,
                   const char *identity_hash, const char *commit, const char *task,
                   atlas_orch_result *r) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = build_submit(repo_id, identity_hash, commit, task);
    atlas_orch_result_init(r);
    T_OK(atlas_writer_orch(w, op, 5000, pol, r, &err), &err);
    T_REQUIRE(r->run_uid.len > 0);
    T_REQUIRE(r->job_uid.len > 0);
}

static void lease(atlas_writer *w, const atlas_syspolicy *pol, const char *job_uid,
                  atlas_orch_result *r) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    T_REQUIRE(op != NULL);
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->job_uid, job_uid, &err), &err);
    T_OK(atlas_buf_set_str(&op->dispatcher_id, "t13-live", &err), &err);
    /* An unfiltered lease never grants a repo-tree driver (A11.1) -- named
     * explicitly, `rundriver.c`'s own `claim()` shape, since every task here
     * runs `fake-repo`. */
    {
        atlas_orch_argv want;
        atlas_orch_argv_init(&want);
        T_OK(atlas_orch_argv_push(&want, "fake-repo", strlen("fake-repo"), &err), &err);
        T_OK(atlas_orch_validations_encode(&want, 1u, &op->lease_drivers, &err), &err);
        atlas_orch_argv_free(&want);
    }
    atlas_orch_result_init(r);
    T_OK(atlas_writer_orch(w, op, 5000, pol, r, &err), &err);
    T_REQUIRE_MSG(r->granted, "the targeted lease was not granted");
}

/* `touched_paths` is NULL to send none at all -- the same "absent means
 * nothing gathered" contract `report()` (`src/orch/rundriver.c`) uses. */

/* Advances the leased attempt's phase, LEASED -> PREPARING -> RUNNING --
 * `atlas_orch_transition_allowed`'s own forward edge, needed because COMPLETE
 * with `success = true` is only a valid transition from RUNNING or VALIDATING,
 * never from LEASED directly. */
static void heartbeat(atlas_writer *w, const atlas_buf *token, atlas_orch_state phase) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_HEARTBEAT);
    T_REQUIRE(op != NULL);
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set(&op->token, token->data, token->len, &err), &err);
    op->phase = phase;
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    T_OK(atlas_writer_orch(w, op, 5000, NULL, &r, &err), &err);
    atlas_orch_result_free(&r);
}

/* `touched_paths` is sent raw and unencoded by this helper -- a caller that
 * wants `atlas_orch_paths_encode`'s shape builds it itself (`one_path_encoded`
 * below, or by hand for more than one path); NULL leaves `op->touched_paths`
 * empty, the "never gathered" wire shape. `touched_complete` is a parameter
 * (T13 fix round, I1/I2/I3) rather than hardcoded `true`, so a case can drive
 * the daemon's `op_complete` / `reliance_check` (`src/db/db_orch.c`) with the
 * exact struct fields a real completion carries -- the client-side JSON
 * wire encoding (`build_run_complete`, `src/core/service_orch.c`, I1's own
 * fix) is not exercised by this path, `atlas_writer_orch` taking the struct
 * directly, matching every other daemon-thread test in this tree. */
static void complete_job(atlas_writer *w, const atlas_buf *token, const char *touched_paths,
                         bool touched_complete, atlas_orch_result *r) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_COMPLETE);
    T_REQUIRE(op != NULL);
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set(&op->token, token->data, token->len, &err), &err);
    op->success = true;
    op->exit_kind = ATLAS_ORCH_EXIT_OK;
    op->exit_code = 0;
    if (touched_paths != NULL) {
        T_OK(atlas_buf_set_str(&op->touched_paths, touched_paths, &err), &err);
    }
    op->touched_complete = touched_complete;
    atlas_orch_result_init(r);
    T_OK(atlas_writer_orch(w, op, 10000, NULL, r, &err), &err);
}

/* One netstring-encoded single-path list, `atlas_orch_paths_encode`'s own
 * shape -- the format `op->touched_paths` and a PATH anchor's stored value
 * both use, so this is what a driver's completion would have sent for one
 * changed file. */
static void one_path_encoded(const char *path_text, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf p = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&p, path_text, &err), &err);
    T_OK(atlas_orch_paths_encode(&p, 1u, out, &err), &err);
    atlas_buf_free(&p);
}

/* A fresh handle against the same file the writer holds open -- never `e->db`,
 * which every T11 daemon case in this tree closes before starting the writer
 * (`t11_writer_open`'s own precondition). Used for setup that must happen
 * before the writer starts and for read/mutate access afterward. */
static atlas_db *reopen(t8env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &db, &err), &err);
    return db;
}

static void pack_row(atlas_db *db, const char *run_uid, atlas_memory_pack *out, bool *found) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_memory_pack_init(out);
    T_OK(atlas_db_memory_pack_get(db, run_uid, out, found, &err), &err);
}

/* --- (a): a run in a repository with a frozen pack ------------------------ */

/* Required case (a): the pack is frozen at SUBMIT, delivered at LEASE, and the
 * composer -- fed exactly the fields a real LEASE grant carries, the same
 * fields `rundriver.c`'s `drive_one` reads off `claimed` -- places the pack
 * block after a non-empty A10.1 section exactly once. */
static void test_pack_is_frozen_and_delivered(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "notes.md", "the daemon reads `db_orch.c` for orchestration state",
                  &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "notes.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    atlas_syspolicy pol;
    live_policy(&pol, "notes.md");
    atlas_memory_pass_result pr;
    memset(&pr, 0, sizeof pr);
    {
        char now[64];
        atlas_now_iso8601(now, sizeof now);
        atlas_memory_observation *obs = malloc(sizeof *obs);
        T_REQUIRE(obs != NULL);
        T_OK(atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, obs, &err), &err);
        T_OK(atlas_db_begin(e.db, &err), &err);
        T_OK(atlas_memory_apply_in_tx(e.db, &e.repo, obs, &pol, now, &pr, &err), &err);
        T_OK(atlas_db_commit(e.db, &err), &err);
        atlas_memory_observation_free(obs);
        free(obs);
    }
    atlas_buf identity = ATLAS_BUF_INIT;
    T_OK(atlas_db_repo_identity_hash(e.db, e.repo_id, &identity, &err), &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_orch_result sub;
    submit(w, &pol, e.repo_id, atlas_buf_cstr(&identity), e.repo.scanned_head,
          "orchestration reads db_orch.c to decide what to do", &sub);
    atlas_buf_free(&identity);
    atlas_orch_result lr;
    lease(w, &pol, atlas_buf_cstr(&sub.job_uid), &lr);

    /* T_REQUIRE, not T_CHECK: every assertion below reads `lr.context_pack`
     * and divides by its length, so continuing past a missing pack would not
     * fail loudly, it would loop forever over an empty needle. */
    T_REQUIRE_MSG(lr.context_pack.len > 0, "a run over a repository with a generation got no pack");
    T_CHECK(lr.context_pack_status.len > 0);

    /* The A10.1 section is synthetic here on purpose: its own ordering
     * contract is T12/A10.1's unit-tested property (`atlas_memory_pack_
     * compose`'s own tests), not something this task re-derives. What this
     * asserts is T13's own claim -- the delivered pack lands after it, once,
     * and is labelled by the composer's own "Context Pack status:" line. */
    atlas_buf composed = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_compose("do the work", "-- memory package --",
                                   atlas_buf_cstr(&lr.context_pack_status),
                                   atlas_buf_cstr(&lr.context_pack), &composed, &err),
        &err);
    const char *c = atlas_buf_cstr(&composed);
    const char *mem_at = strstr(c, "-- memory package --");
    const char *pack_at = strstr(c, atlas_buf_cstr(&lr.context_pack));
    T_REQUIRE(mem_at != NULL);
    T_REQUIRE(pack_at != NULL);
    T_CHECK_MSG(pack_at > mem_at, "the pack section did not follow the A10.1 section");
    /* Exactly once: the pack's own preamble is a fixed string that could in
     * principle recur, so the check is on the whole body's occurrence count. */
    size_t plen = lr.context_pack.len;
    int occurrences = 0;
    for (const char *p = c; (p = strstr(p, atlas_buf_cstr(&lr.context_pack))) != NULL; p += plen) {
        occurrences++;
    }
    T_CHECK_MSG(occurrences == 1, "the pack body appeared %d times, not once", occurrences);
    atlas_buf_free(&composed);

    atlas_orch_result_free(&sub);
    atlas_orch_result_free(&lr);
    t11_writer_close(log, w);
    t8_env_close(&e);
}

/* --- (b): a repository with no sources appends nothing at all ------------- */

static void test_no_sources_composes_the_bare_task(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    t8_bind_head(&e, &err);
    atlas_buf identity = ATLAS_BUF_INIT;
    T_OK(atlas_db_repo_identity_hash(e.db, e.repo_id, &identity, &err), &err);
    atlas_db_close(e.db);
    e.db = NULL;

    atlas_syspolicy pol;
    live_policy(&pol, NULL); /* zero registered sources */

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    const char *task = "a bare task with nothing appended";
    atlas_orch_result sub;
    submit(w, &pol, e.repo_id, atlas_buf_cstr(&identity), e.repo.scanned_head, task, &sub);
    atlas_buf_free(&identity);
    atlas_orch_result lr;
    lease(w, &pol, atlas_buf_cstr(&sub.job_uid), &lr);

    T_CHECK_MSG(lr.context_pack.len == 0,
               "a repository with no registered memory source got a pack anyway");
    T_CHECK(lr.context_pack_status.len == 0);

    atlas_buf composed = ATLAS_BUF_INIT;
    T_OK(atlas_memory_pack_compose(task, atlas_buf_cstr(&lr.memory_package),
                                   atlas_buf_cstr(&lr.context_pack_status),
                                   atlas_buf_cstr(&lr.context_pack), &composed, &err),
        &err);
    T_CHECK_MSG(composed.len == strlen(task) && memcmp(composed.data, task, composed.len) == 0,
               "OFF appended something: got %zu bytes for a %zu-byte bare task", composed.len,
               strlen(task));
    atlas_buf_free(&composed);

    atlas_orch_result_free(&sub);
    atlas_orch_result_free(&lr);
    t11_writer_close(log, w);
    t8_env_close(&e);
}

/* --- (c): the reliance check, and the terminal status it must not move ---- */

/* Runs one full SUBMIT/LEASE/COMPLETE cycle over a repository with exactly one
 * flagged, PATH-anchored claim, sending `touched_paths_raw` (or nothing, when
 * NULL -- the "never gathered" wire shape) and `touched_complete` exactly as
 * the completion's `op` struct, the same fields `op_complete` /
 * `reliance_check` (`src/db/db_orch.c`) actually read. `touched_paths_raw` is
 * sent unencoded: a caller wanting `atlas_orch_paths_encode`'s netstring
 * shape builds it first (`one_path_encoded` below, or by hand for more than
 * one entry, or a deliberately malformed string to drive I3's internal-error
 * path). Returns the run's terminal status, whether the pack row's
 * `reliance_claim_uids` names the seeded claim, and the raw
 * `reliance_checked` / `reliance_complete` columns -- T13 fix round I1/I2/I3's
 * own evidence, beside case (c)'s original two outputs. */
static void run_reliance_scenario_full(const char *touched_paths_raw, bool touched_complete,
                                       atlas_orch_run_status *status, bool *claim_named,
                                       int64_t *reliance_checked, int64_t *reliance_complete) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "ATLAS_FAKE_DRIVER.txt", "seed\n", &err), &err);
    T_OK(fx_write(repo, "notes.md",
                  "the fake driver appends to `ATLAS_FAKE_DRIVER.txt` on every run", &err),
        &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "ATLAS_FAKE_DRIVER.txt",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &err);
    t8_seed_file(&e, "notes.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    atlas_syspolicy pol;
    live_policy(&pol, "notes.md");
    atlas_memory_pass_result pr;
    memset(&pr, 0, sizeof pr);
    {
        char now[64];
        atlas_now_iso8601(now, sizeof now);
        atlas_memory_observation *obs = malloc(sizeof *obs);
        T_REQUIRE(obs != NULL);
        T_OK(atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, obs, &err), &err);
        T_OK(atlas_db_begin(e.db, &err), &err);
        T_OK(atlas_memory_apply_in_tx(e.db, &e.repo, obs, &pol, now, &pr, &err), &err);
        T_OK(atlas_db_commit(e.db, &err), &err);
        atlas_memory_observation_free(obs);
        free(obs);
    }
    atlas_buf identity = ATLAS_BUF_INIT;
    T_OK(atlas_db_repo_identity_hash(e.db, e.repo_id, &identity, &err), &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_orch_result sub;
    submit(w, &pol, e.repo_id, atlas_buf_cstr(&identity), e.repo.scanned_head,
          "the fake driver appends to ATLAS_FAKE_DRIVER.txt on every worker start", &sub);
    atlas_buf_free(&identity);
    atlas_orch_result lr;
    lease(w, &pol, atlas_buf_cstr(&sub.job_uid), &lr);
    T_REQUIRE_MSG(lr.context_pack.len > 0, "the reliance scenario needs a delivered pack");
    heartbeat(w, &lr.token, ATLAS_ORCH_STATE_PREPARING);
    heartbeat(w, &lr.token, ATLAS_ORCH_STATE_RUNNING);

    atlas_orch_result comp;
    complete_job(w, &lr.token, touched_paths_raw, touched_complete, &comp);
    *status = comp.run_status;

    atlas_db *rdb = reopen(&e);
    atlas_memory_pack pack;
    bool found = false;
    pack_row(rdb, atlas_buf_cstr(&sub.run_uid), &pack, &found);
    T_REQUIRE(found);
    T_CHECK_MSG(pack.claim_count > 0, "the seeded claim was not part of the pack's relevant set");
    /* Read the three reliance columns directly: `atlas_memory_pack` (T12's own
     * struct) deliberately carries none of them -- see its own field comment.
     * `reliance_claim_uids` is compared against the frozen `claims_manifest`'s
     * own first (and only) claim uid, extracted the same way `atlas_memory_
     * pack_reliance_match`'s own test would. */
    {
        sqlite3_stmt *st = NULL;
        T_OK(atlas_db_prepare(rdb,
                              "SELECT reliance_checked, reliance_claim_uids, reliance_complete "
                              "FROM memory_context_packs WHERE run_uid = ?1;",
                              &st, &err),
            &err);
        T_OK(atlas_db_bind_text_opt(rdb, st, 1, atlas_buf_cstr(&sub.run_uid), &err), &err);
        T_REQUIRE(sqlite3_step(st) == SQLITE_ROW);
        *reliance_checked = sqlite3_column_int64(st, 0);
        const char *uids = (const char *)sqlite3_column_text(st, 1);
        /* The claim's own uid is embedded in `claims_manifest` as a netstring
         * element; a match in `reliance_claim_uids` is a substring match on
         * the same bytes, which is sufficient for one claim in the pack. */
        *claim_named = uids != NULL && uids[0] != '\0' && strcmp(uids, "0:") != 0;
        *reliance_complete = sqlite3_column_int64(st, 2);
        atlas_db_finish(rdb, st);
    }
    atlas_memory_pack_free(&pack);
    atlas_db_close(rdb);

    atlas_orch_result_free(&sub);
    atlas_orch_result_free(&lr);
    atlas_orch_result_free(&comp);
    t11_writer_close(log, w);
    t8_env_close(&e);
}

/* Case (c)'s original shape: one path, `touched_complete = true`, over
 * `run_reliance_scenario_full`. */
static void run_reliance_scenario(const char *touched_path_or_null, atlas_orch_run_status *status,
                                  bool *claim_named, int64_t *reliance_checked) {
    atlas_buf touched = ATLAS_BUF_INIT;
    if (touched_path_or_null != NULL) {
        one_path_encoded(touched_path_or_null, &touched);
    }
    int64_t reliance_complete = 0;
    run_reliance_scenario_full(touched_path_or_null != NULL ? atlas_buf_cstr(&touched) : NULL,
                               true, status, claim_named, reliance_checked, &reliance_complete);
    atlas_buf_free(&touched);
}

/* Required case (c). A fake worker's completion names the flagged claim's own
 * PATH anchor among its touched paths: `reliance_checked = 1` and the claim's
 * uid land on the pack row. Run a second time with an unrelated touched path
 * and require the **same** terminal status either way -- Decision 8's second
 * half, and the one that matters: settlement must not move. */
static void test_reliance_check_finds_the_overlap_and_settles_the_same_way(void) {
    atlas_orch_run_status with_overlap = ATLAS_ORCH_RUN_UNKNOWN;
    bool named_with = false;
    int64_t checked_with = 0;
    run_reliance_scenario("ATLAS_FAKE_DRIVER.txt", &with_overlap, &named_with, &checked_with);
    T_CHECK_MSG(checked_with == 1, "the flagged claim was never checked");
    T_CHECK_MSG(named_with, "the touched anchor's claim was not recorded on the pack row");

    atlas_orch_run_status without_overlap = ATLAS_ORCH_RUN_UNKNOWN;
    bool named_without = false;
    int64_t checked_without = 0;
    run_reliance_scenario("some/unrelated/path.txt", &without_overlap, &named_without,
                         &checked_without);
    T_CHECK_MSG(!named_without, "an unrelated touched path was recorded as a match");

    T_CHECK_MSG(with_overlap == without_overlap,
               "a reliance finding changed the run's terminal status (%s vs %s)",
               atlas_orch_run_status_name(with_overlap), atlas_orch_run_status_name(without_overlap));
    T_CHECK_MSG(with_overlap == ATLAS_ORCH_RUN_ACCEPTED, "the scenario itself did not settle ACCEPTED");
}

/* --- T13 fix round: I1/I2/I3, the reliance check's three states ----------- */

/* Builds a touched-paths netstring list of exactly `n` entries: entry 0 is
 * `first_path_or_null` when given, and every other entry (and entry 0 when
 * `first_path_or_null` is NULL) is a distinct path nothing in the pack ever
 * flags. Used to build a `truncated` observation
 * (`n == ATLAS_MEMORY_MAX_TOUCHED_PATHS`) that still proves the reliance
 * match runs over what *was* observed before the bound. */
static void many_paths_encoded(size_t n, const char *first_path_or_null, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(n <= ATLAS_MEMORY_MAX_TOUCHED_PATHS);
    atlas_buf *paths = calloc(n, sizeof *paths);
    T_REQUIRE(paths != NULL);
    for (size_t i = 0; i < n; i++) {
        atlas_buf_init(&paths[i]);
        if (i == 0 && first_path_or_null != NULL) {
            T_OK(atlas_buf_set_str(&paths[i], first_path_or_null, &err), &err);
        } else {
            char name[64];
            (void)snprintf(name, sizeof name, "filler/%zu.txt", i);
            T_OK(atlas_buf_set_str(&paths[i], name, &err), &err);
        }
    }
    T_OK(atlas_orch_paths_encode(paths, n, out, &err), &err);
    for (size_t i = 0; i < n; i++) {
        atlas_buf_free(&paths[i]);
    }
    free(paths);
}

/* T13 fix round, I1/I2. Three states over the same seeded, flagged pack:
 * "complete" (one observed path, the flagged anchor itself, `touched_complete
 * = true`), "truncated" (`ATLAS_MEMORY_MAX_TOUCHED_PATHS` observed paths, the
 * flagged anchor among them, `touched_complete = false` -- exactly what the
 * driver sends when `gather_touched_paths` hits its own bound,
 * `src/orch/rundriver.c`'s `touched_add`), and "never-gathered" (no
 * `touched_paths` at all -- what a refusal before the worker ran, a moved
 * HEAD, or I1's own failed-gather case all leave). Each must land a
 * different stored `(reliance_checked, reliance_complete, is a claim
 * named)` tuple: I2's fix is exactly that the third state stops looking like
 * the first. The never-gathered case deliberately sends `touched_complete =
 * true` -- the *wrong* value for what actually happened, matching
 * `outcome_init`'s own default -- because I2's fix means this field is never
 * read when there is nothing to observe: the stored row must not depend on
 * it.
 *
 * A fourth row, "gathered-zero", drives the exact byte boundary I2's own
 * argument rests on: `atlas_orch_paths_encode(..., 0, ...)` produces `"0:"`,
 * length 2, never length 0 -- so a real "the worker changed nothing"
 * observation must never be gated out the way "never-gathered" (length 0) is.
 * It is not a fourth *code path* (it lands on the same branch as "complete",
 * with a matched-claim count of zero rather than one) but it is the input
 * that distinguishes the gate from a length check that got the boundary
 * wrong. */
static void test_reliance_three_states_produce_three_stored_results(void) {
    atlas_orch_run_status status = ATLAS_ORCH_RUN_UNKNOWN;
    bool claim_named = false;
    int64_t checked = 0, complete_col = 0;

    /* complete */
    {
        atlas_buf touched = ATLAS_BUF_INIT;
        one_path_encoded("ATLAS_FAKE_DRIVER.txt", &touched);
        run_reliance_scenario_full(atlas_buf_cstr(&touched), true, &status, &claim_named, &checked,
                                   &complete_col);
        atlas_buf_free(&touched);
        T_CHECK_MSG(checked == 1, "complete: reliance_checked was not set");
        T_CHECK_MSG(complete_col == 1, "complete: reliance_complete was not set");
        T_CHECK_MSG(claim_named, "complete: the flagged anchor's claim was not recorded");
        T_CHECK_MSG(status == ATLAS_ORCH_RUN_ACCEPTED, "complete: the run did not settle ACCEPTED");
    }

    /* gathered-zero: the worker's own `git status` came back clean. The
     * driver would never send anything but `atlas_orch_paths_encode`'s own
     * `"0:"` for this; sent here literally to pin the exact boundary. */
    {
        run_reliance_scenario_full("0:", true, &status, &claim_named, &checked, &complete_col);
        T_CHECK_MSG(checked == 1,
                    "gathered-zero: reliance_checked was not set even though \"0:\" is a real "
                    "observation, not an absent one");
        T_CHECK_MSG(complete_col == 1, "gathered-zero: reliance_complete was not set");
        T_CHECK_MSG(!claim_named,
                    "gathered-zero: a claim was recorded when nothing was observed to touch it");
        T_CHECK_MSG(status == ATLAS_ORCH_RUN_ACCEPTED,
                    "gathered-zero: the reliance finding moved the run's terminal status");
    }

    /* truncated */
    {
        atlas_buf touched = ATLAS_BUF_INIT;
        many_paths_encoded(ATLAS_MEMORY_MAX_TOUCHED_PATHS, "ATLAS_FAKE_DRIVER.txt", &touched);
        run_reliance_scenario_full(atlas_buf_cstr(&touched), false, &status, &claim_named, &checked,
                                   &complete_col);
        atlas_buf_free(&touched);
        T_CHECK_MSG(checked == 1, "truncated: reliance_checked was not set");
        T_CHECK_MSG(complete_col == 0,
                    "truncated: reliance_complete was set from a truncated observation");
        T_CHECK_MSG(claim_named,
                    "truncated: the flagged anchor's claim was not recorded even though it was "
                    "among the paths observed before the bound");
        T_CHECK_MSG(status == ATLAS_ORCH_RUN_ACCEPTED,
                    "truncated: the reliance finding moved the run's terminal status");
    }

    /* never-gathered */
    {
        run_reliance_scenario_full(NULL, true, &status, &claim_named, &checked, &complete_col);
        T_CHECK_MSG(checked == 0,
                    "never-gathered: reliance_checked was set from a completion with no "
                    "observation at all");
        T_CHECK_MSG(!claim_named, "never-gathered: a claim was recorded from nothing observed");
        T_CHECK_MSG(status == ATLAS_ORCH_RUN_ACCEPTED,
                    "never-gathered: the absent observation moved the run's terminal status");
    }
}

/* T13 fix round, I3. A malformed `touched_paths` -- a count past
 * `ATLAS_MEMORY_MAX_TOUCHED_PATHS`, the shape a dispatcher-uid peer could send
 * (never the run driver, which never sends more than the bound) -- makes
 * `atlas_orch_paths_decode` refuse deep inside `reliance_check`
 * (`src/db/db_orch.c`). Before this round the failure propagated out of
 * `op_complete` and rolled back the whole completion transaction: the lease
 * was never released and the run could only reach BLOCKED, later, by expiry
 * -- a run status decided by the memory layer, the coupling Decision 8
 * forbids. `complete_job`'s own `T_OK` on `atlas_writer_orch` is the first
 * assertion this test makes and the one that would have failed before this
 * round: the completion must land at all. */
static void test_a_reliance_check_internal_error_never_blocks_the_run(void) {
    atlas_orch_run_status status = ATLAS_ORCH_RUN_UNKNOWN;
    bool claim_named = false;
    int64_t checked = 0, complete_col = 0;
    /* Six digits (the decoder's own ceiling before it refuses outright) and a
     * count past the cap: `atlas_orch_paths_decode` refuses with "a stored
     * path list holds 999999 entries, cap is 256" before it reads a single
     * path. */
    run_reliance_scenario_full("999999:", true, &status, &claim_named, &checked, &complete_col);
    T_CHECK_MSG(status == ATLAS_ORCH_RUN_ACCEPTED,
               "an internal reliance-check error changed the run's terminal status instead of "
               "being recorded as not performed");
    T_CHECK_MSG(checked == 0, "an internal reliance-check error was recorded as a completed check");
    T_CHECK_MSG(!claim_named, "an internal reliance-check error somehow recorded a claim");
}

/* --- (d): a stale pack is delivered as such, never as current ------------- */

/* Required case (d), acceptance item 6 at the delivery surface. The frozen
 * pack's own pinned `memory_generation` is corrupted directly -- the same
 * "hand-edit the stored row" technique T12's own freshness tests use for a
 * moved `repo_identity_hash` -- rather than moving the *live* repository
 * identity, which would trip `op_lease`'s own older, unrelated identity-
 * refusal check (A11.1) before the pack is ever read. Any of the six pins
 * demonstrates the same STALE-computation and delivery-clearing logic; this
 * is the one that can be corrupted without colliding with a different
 * refusal. */
static void test_a_stale_pack_is_delivered_as_stale(void) {
    atlas_err err;
    atlas_err_init(&err);
    t8env e;
    t8_env_open(&e, &err);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_write(repo, "notes.md", "the daemon reads `db_orch.c` for orchestration state",
                  &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "seed", &err), &err);
    t8_bind_head(&e, &err);
    t8_seed_file(&e, "db_orch.c", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                &err);
    t8_seed_file(&e, "notes.md", "1111111111111111111111111111111111111111111111111111111111111111",
                &err);

    atlas_syspolicy pol;
    live_policy(&pol, "notes.md");
    atlas_memory_pass_result pr;
    memset(&pr, 0, sizeof pr);
    {
        char now[64];
        atlas_now_iso8601(now, sizeof now);
        atlas_memory_observation *obs = malloc(sizeof *obs);
        T_REQUIRE(obs != NULL);
        T_OK(atlas_memory_observe(e.db, &e.repo, fx_data_dir(&e.fx), &pol, obs, &err), &err);
        T_OK(atlas_db_begin(e.db, &err), &err);
        T_OK(atlas_memory_apply_in_tx(e.db, &e.repo, obs, &pol, now, &pr, &err), &err);
        T_OK(atlas_db_commit(e.db, &err), &err);
        atlas_memory_observation_free(obs);
        free(obs);
    }
    atlas_buf identity = ATLAS_BUF_INIT;
    T_OK(atlas_db_repo_identity_hash(e.db, e.repo_id, &identity, &err), &err);
    atlas_db_close(e.db);
    e.db = NULL;

    FILE *log = NULL;
    atlas_writer *w = NULL;
    t11_writer_open(&e, &log, &w, &err);

    atlas_orch_result sub;
    submit(w, &pol, e.repo_id, atlas_buf_cstr(&identity), e.repo.scanned_head,
          "orchestration reads db_orch.c to decide what to do", &sub);
    atlas_buf_free(&identity);

    /* Move the pin, not the live repository. */
    {
        atlas_db *rdb = reopen(&e);
        T_OK(atlas_db_exec_sql(rdb,
                               "UPDATE memory_context_packs SET memory_generation = 999999 "
                               "WHERE run_uid = (SELECT run_uid FROM orch_jobs LIMIT 1);",
                               &err),
            &err);
        atlas_db_close(rdb);
    }

    atlas_orch_result lr;
    lease(w, &pol, atlas_buf_cstr(&sub.job_uid), &lr);
    T_REQUIRE_MSG(lr.context_pack.len > 0, "the stale-pack scenario needs a delivered pack");
    T_CHECK_MSG(strncmp(atlas_buf_cstr(&lr.context_pack_status), "STALE:", 6) == 0,
               "a moved pin was delivered as \"%s\", not STALE:<WHICH>",
               atlas_buf_cstr(&lr.context_pack_status));

    atlas_orch_result_free(&sub);
    atlas_orch_result_free(&lr);
    t11_writer_close(log, w);
    t8_env_close(&e);
}

/* --- the strstr grep's own witness ----------------------------------------
 *
 * Step 3's requirement, checked here rather than only by eye: no branch on any
 * path this task touches may call `strstr` over the pack, the worker log or an
 * artifact. `grep -rn strstr` over every file this task's diff modified
 * (`include/atlas/orch_ops.h`, `include/atlas/memory.h`, `src/db/db_orch.c`,
 * `src/db/db_memory.c`, `src/memory/pack.c`, `src/orch/rundriver.c`,
 * `src/orch/dispatch.c`, `src/ipc/server_orch.c`, `src/core/service_orch.c`,
 * `src/daemon/writer.c`, `src/daemon/daemon_internal.h`, `src/daemon/watch.c`)
 * finds exactly one hit, in `memory.h`, and it is prose in a pre-existing T1/T2
 * comment about the policy grammar's `..` rule ("a parser reaching for
 * `strstr` there is the obvious bug") -- not code, and not touched by this
 * task. This file's own `strstr` calls, below, are test assertions locating a
 * known-good byte string in a composed result; the rule they would violate is
 * a production branch deciding something from parsed prose, which none of
 * these are. */
static const atlas_test TESTS[] = {
    {"a pack is frozen at submit and delivered at lease, after the memory section",
     test_pack_is_frozen_and_delivered},
    {"a repository with no sources composes the bare task",
     test_no_sources_composes_the_bare_task},
    {"the reliance check finds the overlap and settles the run the same way either way",
     test_reliance_check_finds_the_overlap_and_settles_the_same_way},
    {"complete, truncated and never-gathered each land a different stored reliance result",
     test_reliance_three_states_produce_three_stored_results},
    {"an internal reliance-check error is recorded as not performed, never a blocked run",
     test_a_reliance_check_internal_error_never_blocks_the_run},
    {"a stale pack is delivered as stale, never as current", test_a_stale_pack_is_delivered_as_stale},
};

ATLAS_TEST_MAIN("memory_pack_live", TESTS)

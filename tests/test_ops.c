/* Atlas - long operations: accepted, polled, and never mistaken for failures.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This suite exists because of one defect shape that appeared twice, and the
 * shape is worth stating before the tests.
 *
 * `atlas backup create` against a 437 MiB index failed at 10.022 s with
 * "timed out while reading a frame header" and exit 1 — while the daemon went
 * on to write and verify a complete, correct backup. A *success reported as a
 * failure* is worse than a failure, because the next thing anybody does about
 * it is re-run the operation or work around it, and both are wrong. The same
 * shape then appeared one layer down: a semantic index makes the daemon write
 * hard for minutes, ordinary reads slow under that load, and the client's own
 * *poll* hit the same timeout — so a poll failing was again read as the
 * operation failing.
 *
 * Underneath both was something the rules already forbade: `atlas_server_dispatch`
 * runs inline in the non-blocking serve loop, so a thirty-second backup stalled
 * every other client for thirty seconds. The serve loop is written the way it is
 * precisely so one slow client cannot do that; the backup path simply was not
 * covered by the rule.
 *
 * What is pinned here is not a duration. Durations are what the defect was made
 * of, and a test that waited out a real one would add that wait to every run of
 * the suite for ever, to re-derive what two constants already state. What is
 * pinned is the shape that makes durations stop mattering: a terminal record
 * never changes, a second request is refused deterministically, the client's
 * patience is bounded far above the transport's, and an unknown id says so.
 *
 * **On what is tested where.** The operator-gated methods — `backup.create`,
 * `operation.get`, `code.index` — require an authority grant, which requires a
 * root-owned policy naming the caller *and* a root-owned executable. A fixture
 * daemon runs the build tree's own binary, which by construction is writable by
 * the account running the tests, so the grant is correctly refused and those
 * methods correctly do not exist over a fixture socket. That refusal is
 * asserted here as a property in its own right. The mechanism behind the
 * methods is exercised directly, and the end-to-end socket path is exercised
 * against the deployed system, where the policy and the binary are root-owned —
 * which is the only place it can honestly be exercised at all.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/limits.h"
#include "atlas/maintenance.h"
#include "atlas/ops.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- 1. the client's patience is bounded above the transport's ---------------
 *
 * This is the constant that makes the whole design work, and it is asserted
 * rather than assumed because the failure it prevents is silent: if a client
 * ever stopped waiting at the transport's timeout again, every long operation
 * would go back to reporting success as failure, and nothing else in this
 * suite would notice. */
static void test_the_client_waits_far_longer_than_the_transport(void) {
    T_CHECK_MSG((int64_t)ATLAS_OPS_CLIENT_WAIT_MS > (int64_t)ATLAS_IPC_READ_TIMEOUT_MS,
                "a client would stop waiting at or before the transport's own timeout "
                "(%d ms vs %d ms), which is exactly how a completed operation gets reported "
                "as a failure",
                ATLAS_OPS_CLIENT_WAIT_MS, ATLAS_IPC_READ_TIMEOUT_MS);
    /* Not merely greater: greater by enough to cover an operation that is slow
     * *because the daemon is busy doing it*. A margin of one poll interval
     * would satisfy the check above and none of its purpose. A full semantic
     * index of a real repository was measured at 144 s. */
    T_CHECK_MSG((int64_t)ATLAS_OPS_CLIENT_WAIT_MS >= (int64_t)ATLAS_IPC_READ_TIMEOUT_MS * 60,
                "the client's ceiling (%d ms) is not far enough above the transport's "
                "(%d ms) to cover a long operation",
                ATLAS_OPS_CLIENT_WAIT_MS, ATLAS_IPC_READ_TIMEOUT_MS);
    /* Short enough that an answer is not stale by the time it arrives, without
     * being a busy loop. */
    T_CHECK(ATLAS_OPS_POLL_INTERVAL_MS > 0 &&
            ATLAS_OPS_POLL_INTERVAL_MS < ATLAS_IPC_READ_TIMEOUT_MS);
}

/* --- 2. the state model ------------------------------------------------------
 *
 * UNKNOWN is zero and terminal states are terminal. Both are asked of the
 * functions rather than restated here, so a test cannot pass by agreeing with a
 * second copy of the rules. */
static void test_an_unfinished_operation_never_reads_as_a_finished_one(void) {
    T_CHECK_MSG(ATLAS_OP_UNKNOWN == 0,
                "UNKNOWN is not zero, so a zeroed record would read as a real state");
    T_CHECK_MSG(ATLAS_OP_KIND_UNKNOWN == 0, "an operation kind's zero is not UNKNOWN");
    T_CHECK(!atlas_op_state_is_terminal(ATLAS_OP_UNKNOWN));
    T_CHECK(!atlas_op_state_is_terminal(ATLAS_OP_RUNNING));
    T_CHECK(atlas_op_state_is_terminal(ATLAS_OP_SUCCEEDED));
    T_CHECK(atlas_op_state_is_terminal(ATLAS_OP_FAILED));
    /* Every state and kind has a name; a missing one prints as UNKNOWN and
     * quietly turns a real state into "no idea". */
    T_CHECK(strcmp(atlas_op_state_name(ATLAS_OP_RUNNING), "RUNNING") == 0);
    T_CHECK(strcmp(atlas_op_state_name(ATLAS_OP_SUCCEEDED), "SUCCEEDED") == 0);
    T_CHECK(strcmp(atlas_op_state_name(ATLAS_OP_FAILED), "FAILED") == 0);
    T_CHECK(strcmp(atlas_op_kind_name(ATLAS_OP_KIND_BACKUP_CREATE), "backup.create") == 0);
    T_CHECK(strcmp(atlas_op_kind_name(ATLAS_OP_KIND_BACKUP_VERIFY), "backup.verify") == 0);
    T_CHECK(strcmp(atlas_op_kind_name(ATLAS_OP_KIND_SEM_INDEX), "code.index") == 0);
}

/* --- 3. a record is answerable the moment it exists, and terminal for ever ---
 *
 * The table is exercised directly. `begin_sem_index` mints a record without
 * running anything, which is exactly the state a client is answered in: the
 * whole point of the split is that "accepted" is observable long before "done".
 */
static void test_a_record_is_answerable_before_it_is_finished(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ops *ops = NULL;
    T_REQUIRE(atlas_ops_start(NULL, NULL, &ops, &err) == ATLAS_OK);

    int64_t id = 0;
    T_OK(atlas_ops_begin_sem_index(ops, 7, &id, &err), &err);
    T_CHECK_MSG(id > 0, "an accepted operation got no id");

    atlas_op_record rec;
    atlas_op_record_init(&rec);
    T_OK(atlas_ops_get(ops, id, &rec, &err), &err);
    T_CHECK_MSG(rec.state == ATLAS_OP_RUNNING, "a fresh record is not RUNNING but %s",
                atlas_op_state_name(rec.state));
    T_CHECK(rec.kind == ATLAS_OP_KIND_SEM_INDEX);
    T_CHECK(rec.repo_id == 7);
    T_CHECK_MSG(!atlas_op_state_is_terminal(rec.state), "an unstarted operation is terminal");
    T_CHECK(atlas_ops_is_running(ops, id));

    /* Finishing moves it once. */
    atlas_ops_finish(ops, id, ATLAS_OK, "published", "generation=3");
    atlas_op_record done;
    atlas_op_record_init(&done);
    T_OK(atlas_ops_get(ops, id, &done, &err), &err);
    T_CHECK(done.state == ATLAS_OP_SUCCEEDED);
    T_CHECK(strcmp(atlas_buf_cstr(&done.message), "published") == 0);
    T_CHECK(strcmp(atlas_buf_cstr(&done.detail), "generation=3") == 0);
    T_CHECK(!atlas_ops_is_running(ops, id));

    /* And never again. This is reachable from an error path that can itself run
     * twice, so a second transition must not rewrite the first answer — that is
     * the whole idempotency claim, and a client polling depends on it. */
    atlas_ops_finish(ops, id, ATLAS_ERR_INTEGRITY, "it actually failed", "nonsense");
    atlas_op_record again;
    atlas_op_record_init(&again);
    T_OK(atlas_ops_get(ops, id, &again, &err), &err);
    T_CHECK_MSG(again.state == ATLAS_OP_SUCCEEDED,
                "a terminal record changed state to %s", atlas_op_state_name(again.state));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&again.message), "published") == 0,
                "a terminal record's message was rewritten to \"%s\"",
                atlas_buf_cstr(&again.message));

    atlas_op_record_free(&again);
    atlas_op_record_free(&done);
    atlas_op_record_free(&rec);
    atlas_ops_stop(ops);
}

/* --- 4. a second concurrent request is refused, naming the first ------------- */
static void test_concurrent_requests_are_refused_deterministically(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ops *ops = NULL;
    T_REQUIRE(atlas_ops_start(NULL, NULL, &ops, &err) == ATLAS_OK);

    int64_t first = 0;
    T_OK(atlas_ops_begin_sem_index(ops, 1, &first, &err), &err);

    /* Same repository: refused, and the refusal names the operation holding the
     * slot so the caller's next move is to poll rather than to guess. Two
     * indexes of one repository differ only in which generation somebody ends
     * up looking at. */
    int64_t second = 0;
    atlas_err berr;
    atlas_err_init(&berr);
    T_CHECK_MSG(atlas_ops_begin_sem_index(ops, 1, &second, &berr) != ATLAS_OK,
                "a second index of the same repository was accepted");
    T_CHECK_MSG(strstr(atlas_err_msg(&berr), "already running") != NULL,
                "the refusal does not say one is already running: %s", atlas_err_msg(&berr));
    T_CHECK_MSG(strstr(atlas_err_msg(&berr), "operation status") != NULL,
                "the refusal does not say how to poll the one that is running: %s",
                atlas_err_msg(&berr));

    /* A *different* repository is not blocked by it: the constraint is per
     * repository, because that is where the ambiguity is. */
    int64_t other = 0;
    T_OK(atlas_ops_begin_sem_index(ops, 2, &other, &err), &err);
    T_CHECK(other != first);

    /* Once the first finishes the slot is free again. */
    atlas_ops_finish(ops, first, ATLAS_OK, "done", "");
    int64_t third = 0;
    T_OK(atlas_ops_begin_sem_index(ops, 1, &third, &err), &err);
    T_CHECK_MSG(third != first, "a finished operation's id was reused");

    atlas_ops_stop(ops);
}

/* --- 5. an unknown id says so, and says why ---------------------------------
 *
 * Unknown covers three situations deliberately — never issued, evicted, or the
 * daemon restarted — and distinguishing them would mean inventing a history the
 * table does not keep. What it must not do is guess: answering SUCCEEDED would
 * be a claim nothing supports, and FAILED would be false. */
static void test_an_unknown_operation_is_unknown_and_explains_itself(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ops *ops = NULL;
    T_REQUIRE(atlas_ops_start(NULL, NULL, &ops, &err) == ATLAS_OK);

    atlas_op_record rec;
    atlas_op_record_init(&rec);
    atlas_err gerr;
    atlas_err_init(&gerr);
    T_CHECK_MSG(atlas_ops_get(ops, 4242, &rec, &gerr) != ATLAS_OK,
                "an id nobody issued was answered");
    T_CHECK_MSG(strstr(atlas_err_msg(&gerr), "no operation 4242 is known") != NULL,
                "the unknown-id message does not name the id: %s", atlas_err_msg(&gerr));
    T_CHECK_MSG(strstr(atlas_err_msg(&gerr), "do not survive a restart") != NULL,
                "the unknown-id message does not explain that records are not durable: %s",
                atlas_err_msg(&gerr));
    /* It points at the artefact, which is what actually survives. */
    T_CHECK_MSG(strstr(atlas_err_msg(&gerr), "backup verify") != NULL &&
                    strstr(atlas_err_msg(&gerr), "sem-status") != NULL,
                "the unknown-id message does not point at the durable artefacts: %s",
                atlas_err_msg(&gerr));

    /* Zero and negative ids are unknown too rather than indexing anything. */
    T_CHECK(atlas_ops_get(ops, 0, &rec, &gerr) != ATLAS_OK);
    T_CHECK(atlas_ops_get(ops, -1, &rec, &gerr) != ATLAS_OK);
    T_CHECK(!atlas_ops_is_running(ops, 0));

    atlas_op_record_free(&rec);
    atlas_ops_stop(ops);
}

/* --- 6. the table is bounded, and the bound behaves like a restart ----------- */
static void test_the_table_is_bounded_and_eviction_reads_as_unknown(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ops *ops = NULL;
    T_REQUIRE(atlas_ops_start(NULL, NULL, &ops, &err) == ATLAS_OK);

    int64_t first = 0;
    T_OK(atlas_ops_begin_sem_index(ops, 1, &first, &err), &err);
    atlas_ops_finish(ops, first, ATLAS_OK, "done", "");

    /* Fill past the ring. Each is finished so the next may start. */
    for (size_t i = 0; i < ATLAS_OPS_MAX_RECORDS + 2u; i++) {
        int64_t id = 0;
        atlas_err lerr;
        atlas_err_init(&lerr);
        if (atlas_ops_begin_sem_index(ops, 1, &id, &lerr) != ATLAS_OK) {
            T_CHECK_MSG(false, "filling the ring was refused at %zu: %s", i, atlas_err_msg(&lerr));
            break;
        }
        atlas_ops_finish(ops, id, ATLAS_OK, "done", "");
    }

    /* The oldest is gone, and gone is unknown — the same answer a restart gives
     * for every id, so a client already has to handle it. Nothing is silently
     * reinterpreted as some other operation. */
    atlas_op_record rec;
    atlas_op_record_init(&rec);
    atlas_err gerr;
    atlas_err_init(&gerr);
    T_CHECK_MSG(atlas_ops_get(ops, first, &rec, &gerr) != ATLAS_OK,
                "an evicted record was still answered, so the ring is not bounded as documented");
    atlas_op_record_free(&rec);
    atlas_ops_stop(ops);
}

/* --- 7. the operator group does not exist for an ungranted peer --------------
 *
 * Over a fixture socket the authority probe cannot grant: the policy is not
 * root-owned here and neither is the binary under test. So the *socket* forms
 * of the operations this closeout added must be unreachable, and when the
 * daemon is the one to answer it must say `unknown method` — the same answer a
 * name that does not exist gets, because a refusal distinguishing "you may not"
 * from "there is no such thing" tells a caller what to try next.
 *
 * `backup create` and `backup verify` are deliberately *not* here. In a
 * per-user deployment they are local operations and succeed, which is A5's
 * design: the account that owns the data directory can copy the file with `cp`
 * anyway, so refusing it would be a check an adversary walks around. They
 * acquire an authority gate only when the index belongs to another account,
 * which is a condition a fixture cannot create and the deployed system is
 * where it is exercised. Asserting they fail here would have pinned the
 * opposite of the contract. */
static void test_the_new_operations_are_invisible_without_authority(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(fx_daemon_start(&fx, &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    static const char *const CASES[][6] = {
        /* No local form exists and never will: the operations table lives in
         * the daemon's memory. */
        {"operation", "status", "1", NULL, NULL, NULL},
        /* Writes the index. The fixture's daemon holds the writer lock, so the
         * local path must refuse rather than fail part-way through, and the
         * socket path must not exist for an ungranted peer. */
        {"code", "index", "alpha", "--compdb", "compile_commands.json", NULL},
    };
    /* `maintenance` is checked separately below: in a per-user fixture it is a
     * local operation and correctly succeeds, exactly as backup does. Its
     * socket form is what is gated, and that is only reachable where the index
     * belongs to another account. */
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        size_t n = 0;
        while (n < 6 && CASES[i][n] != NULL) {
            n++;
        }
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errbuf = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&fx, &d, CASES[i], n, &out, &errbuf, &code, &err), &err);
        T_OK(atlas_buf_append(&out, errbuf.data, errbuf.len, &err), &err);
        atlas_buf_free(&errbuf);
        T_CHECK_MSG(code != 0, "`atlas %s %s` succeeded without an authority grant", CASES[i][0],
                    CASES[i][1]);
        /* The daemon may not be the one to refuse — the CLI's own local path
         * can decline first — but under no circumstances may the operation
         * happen, and when the daemon does answer it must be `unknown method`
         * rather than a refusal that admits the name exists. */
        const char *text = atlas_buf_cstr(&out);
        if (strstr(text, "method") != NULL) {
            T_CHECK_MSG(strstr(text, "unknown method") != NULL,
                        "`atlas %s %s` was refused in a way that admits the method exists: %s",
                        CASES[i][0], CASES[i][1], text);
        }
        atlas_buf_free(&out);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- 8. every operation that can outlast a client is a long operation --------
 *
 * Converting `backup.create` and leaving `backup.verify` inline simply moved
 * the timeout to the next command, and that is exactly what happened: an
 * 815 MiB backup verified fine on the daemon while the operator was told
 * "timed out while reading a frame header". Verification reads every page —
 * `PRAGMA integrity_check` walks the b-trees and every decision revision is
 * rehashed — so it belongs in the same mechanism.
 *
 * The kinds are enumerated here so that adding a fourth long operation without
 * a name, or removing one, is a test failure rather than a surprise on a large
 * index somewhere. */
static void test_every_long_operation_has_a_kind(void) {
    /* Each kind names itself as the method or command it stands for, because
     * the name is reported to a terminal and "operation 4 succeeded" is not an
     * answer to "did my backup work". */
    static const struct {
        atlas_op_kind kind;
        const char *name;
    } KINDS[] = {
        {ATLAS_OP_KIND_BACKUP_CREATE, "backup.create"},
        {ATLAS_OP_KIND_BACKUP_VERIFY, "backup.verify"},
        {ATLAS_OP_KIND_SEM_INDEX, "code.index"},
    };
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        T_CHECK_MSG(strcmp(atlas_op_kind_name(KINDS[i].kind), KINDS[i].name) == 0,
                    "operation kind %d does not name itself \"%s\" but \"%s\"",
                    (int)KINDS[i].kind, KINDS[i].name, atlas_op_kind_name(KINDS[i].kind));
        T_CHECK_MSG(KINDS[i].kind != ATLAS_OP_KIND_UNKNOWN,
                    "a real operation kind collides with UNKNOWN");
    }
    /* And nothing beyond them: a kind with no name prints as UNKNOWN, which
     * turns a real operation into "no idea". */
    T_CHECK_MSG(strcmp(atlas_op_kind_name((atlas_op_kind)(ATLAS_OP_KIND_SEM_INDEX + 1)),
                       "UNKNOWN") == 0,
                "an unnamed kind does not fall back to UNKNOWN");
}

/* --- 9. an operation Atlas does not serve says so, not SQLite ---------------
 *
 * `maintenance` opens the index directly and is dispatched before any context
 * exists, so it never reached the refusal every other command gets on a system
 * deployment: from the operator's account it failed with SQLite's own "unable
 * to open database file", which describes a permission the caller was never
 * going to have and points at a file they cannot see.
 *
 * It must stay without an RPC method — A5 gives maintenance no socket surface
 * deliberately, because nothing reachable from a model may prune the index —
 * so the fix is the message, and this pins that the message is Atlas' rather
 * than the database's. In a fixture the index is this account's own, so the
 * command works; what is asserted is that whatever it says, it never leaks a
 * raw driver error. */
static void test_maintenance_never_reports_a_raw_database_error(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    fx_daemon d;
    fx_daemon_init(&d);

    static const char *const CASES[][3] = {
        {"maintenance", "plan", NULL},
        {"maintenance", "prune", NULL},
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errbuf = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&fx, &d, CASES[i], 2u, &out, &errbuf, &code, &err), &err);
        T_OK(atlas_buf_append(&out, errbuf.data, errbuf.len, &err), &err);
        atlas_buf_free(&errbuf);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "unable to open database file") == NULL,
                    "`atlas %s %s` leaked a raw SQLite error to the user: %s", CASES[i][0],
                    CASES[i][1], atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- 10. an id is never reused by a later daemon ----------------------------
 *
 * The table is forgotten on restart, which is documented and correct. What is
 * *not* acceptable is reusing the ids: an operation id issued before a restart
 * must not name a different operation afterwards, or a client polling it is
 * handed another operation's verdict — a confident wrong answer about whether
 * their backup succeeded, which is precisely the failure this layer exists to
 * prevent.
 *
 * Two tables started in succession stand in for two daemons. The second must
 * issue ids strictly above the first's, so every id the first minted is below
 * the second's base and is answered "unknown", which is true and actionable. */
static void test_ids_are_not_reused_by_a_later_daemon(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_ops *first = NULL;
    T_REQUIRE(atlas_ops_start(NULL, NULL, &first, &err) == ATLAS_OK);
    int64_t a = 0;
    T_OK(atlas_ops_begin_sem_index(first, 1, &a, &err), &err);
    int64_t b = 0;
    atlas_ops_finish(first, a, ATLAS_OK, "done", "");
    T_OK(atlas_ops_begin_sem_index(first, 1, &b, &err), &err);
    atlas_ops_stop(first);

    atlas_ops *second = NULL;
    T_REQUIRE(atlas_ops_start(NULL, NULL, &second, &err) == ATLAS_OK);
    int64_t c = 0;
    T_OK(atlas_ops_begin_sem_index(second, 1, &c, &err), &err);

    T_CHECK_MSG(c > b, "a restarted table reissued id %lld, which the previous one had already "
                       "used (%lld); a client polling an old id would be handed a different "
                       "operation's answer",
                (long long)c, (long long)b);

    /* And the previous daemon's ids are unknown to this one, rather than
     * resolving to whatever now occupies that slot. */
    atlas_op_record rec;
    atlas_op_record_init(&rec);
    atlas_err gerr;
    atlas_err_init(&gerr);
    T_CHECK_MSG(atlas_ops_get(second, a, &rec, &gerr) != ATLAS_OK,
                "an id from a previous daemon resolved to an operation in this one");
    T_CHECK_MSG(atlas_ops_get(second, b, &rec, &gerr) != ATLAS_OK,
                "an id from a previous daemon resolved to an operation in this one");
    atlas_op_record_free(&rec);
    atlas_ops_stop(second);
}

/* --- 11. the retention policy is this binary's, never the wire's ------------
 *
 * A maintenance report crossing the socket carries counts. The classification,
 * the prunable flag and the written reason are compiled-in constants and are
 * looked up locally by table name — a report is not the place to start trusting
 * a peer for the policy it is reporting on, and the reason in particular is the
 * deliverable: a classification without one is a label, and a label is what
 * lets a later phase quietly reclassify a table.
 *
 * A table this binary does not know is reported as unknown rather than
 * invented, which is what a version skew looks like. */
static void test_the_retention_policy_is_local(void) {
    const char *table = NULL;
    const char *reason = NULL;
    atlas_retention_class cls = ATLAS_RETAIN_CANONICAL;
    bool prunable = true;

    T_CHECK_MSG(atlas_maintenance_policy_lookup("repo_events", &table, &cls, &prunable, &reason),
                "the one prunable table is not in the compiled-in policy");
    T_CHECK(strcmp(table, "repo_events") == 0);
    T_CHECK_MSG(prunable, "repo_events is not marked prunable");
    T_CHECK_MSG(reason != NULL && reason[0] != '\0',
                "a policy entry carries no written reason, which makes it a label");

    /* Something canonical must come back not prunable, with its own reason. */
    T_CHECK(atlas_maintenance_policy_lookup("decision_revisions", &table, &cls, &prunable,
                                            &reason));
    T_CHECK_MSG(!prunable, "decision_revisions is marked prunable, which would delete history");
    T_CHECK_MSG(reason != NULL && reason[0] != '\0', "a canonical table carries no reason");

    /* And a name nobody knows is refused rather than answered. */
    T_CHECK_MSG(!atlas_maintenance_policy_lookup("not_a_table", &table, &cls, &prunable, &reason),
                "an unknown table was given a classification");
}

static const atlas_test TESTS[] = {
    {"the client waits far longer than the transport",
     test_the_client_waits_far_longer_than_the_transport},
    {"an unfinished operation never reads as a finished one",
     test_an_unfinished_operation_never_reads_as_a_finished_one},
    {"a record is answerable before it is finished",
     test_a_record_is_answerable_before_it_is_finished},
    {"concurrent requests are refused deterministically",
     test_concurrent_requests_are_refused_deterministically},
    {"an unknown operation is unknown and explains itself",
     test_an_unknown_operation_is_unknown_and_explains_itself},
    {"the table is bounded and eviction reads as unknown",
     test_the_table_is_bounded_and_eviction_reads_as_unknown},
    {"the new operations are invisible without authority",
     test_the_new_operations_are_invisible_without_authority},
    {"every long operation has a kind", test_every_long_operation_has_a_kind},
    {"maintenance never reports a raw database error",
     test_maintenance_never_reports_a_raw_database_error},
    {"ids are not reused by a later daemon", test_ids_are_not_reused_by_a_later_daemon},
    {"the retention policy is local", test_the_retention_policy_is_local},
};

ATLAS_TEST_MAIN("ops", TESTS)

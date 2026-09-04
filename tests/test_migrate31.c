/* Atlas - A16 T2: migration 31 -- the remote operator channel: which channel
 * and credential minted a challenge, and which credential the ledger records.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two leaf-referenced tables rebuilt, on migration 15's own precedent (the
 * only prior migration that widened `decision_events.actor`): the events
 * CHECK admits `REMOTE_OPERATOR_CONFIRMED` and gains `key_id`, and
 * `decision_challenges` gains `channel` and `key_id`. This suite proves:
 *
 *   - a fresh database reaches schema 31 with both new columns and every
 *     index this migration recreates;
 *   - a database stopped at 30 with three pre-existing events (one per actor
 *     value the old CHECK admitted, including VERIFICATION_POLICY) and two
 *     pre-existing challenges reaches 31 with every column of every
 *     pre-existing row byte-identical, every challenge reading
 *     channel = 'LOCAL' and key_id IS NULL, and every event reading
 *     key_id IS NULL;
 *   - the widened events CHECK accepts REMOTE_OPERATOR_CONFIRMED and refuses
 *     REMOTE_OPERATOR and UNKNOWN; the new channel CHECK accepts LOCAL and
 *     REMOTE and refuses UNKNOWN and the empty string;
 *   - pragma_foreign_key_check is empty throughout;
 *   - `atlas_db_decision_verify`'s replay is unaffected by this migration: it
 *     agrees before migration 31 runs and still agrees after a
 *     REMOTE_OPERATOR_CONFIRMED APPROVED event is inserted by hand -- the
 *     replay reads `event` and `revision_id` only, never `actor`;
 *   - a rebuild that loses a ledger row is refused by the named constraint
 *     (`no_decision_event_may_be_lost_in_migration_31`) and rolled back in
 *     full, on `test_migrate8.c`'s own failure-injection shape.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

static int schema_of(atlas_db *db) {
    atlas_err err;
    atlas_err_init(&err);
    return atlas_db_schema_version(db, &err);
}

static int64_t count_sql(atlas_db *db, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t v = -1;
    T_OK(atlas_db_query_int64(db, sql, &v, &err), &err);
    return v;
}

static bool column_exists(atlas_db *db, const char *table, const char *column) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2;", &q, &err) !=
        ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, table, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, column, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static bool object_exists(atlas_db *db, const char *type, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM sqlite_schema WHERE type = ?1 AND name = ?2;", &q,
                         &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, type, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static atlas_status open_fresh(fixture *fx, atlas_db **out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_datadir_ensure(fx_data_dir(fx), err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(fx_data_dir(fx), &path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_open(atlas_buf_cstr(&path), out, err);
    }
    atlas_buf_free(&path);
    return st;
}

/* The pre-31 column set for each table, in the exact order migration 15 (for
 * `decision_events`) and migration 13 (for `decision_challenges`) declared
 * them. Named explicitly rather than `SELECT *`, on `test_migrate8.c`'s
 * `preserved_cols` pattern: migration 31 appends columns with either a
 * default or NULL, which changes nothing about a pre-existing row, and a
 * `SELECT *` digest taken before and after would report that addition as a
 * difference it is not. */
#define EVENTS_V30_COLS                                                                          \
    "id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"        \
    " superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key"
#define CHALLENGES_V30_COLS                                                                       \
    "id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"            \
    " supersede_document_id, indexed_commit, evidence_digest, prior_freshness, prior_reasons,"     \
    " created_at, expires_at, consumed, consumed_at"

/* A digest over an explicit column list, in rowid order, including column
 * names -- so a reordered, renamed or retyped column is a difference too.
 * `test_migrate29.c`'s `table_digest`, parameterised on the column list. */
static void table_digest_cols(atlas_db *db, const char *table, const char *cols, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[512];
    (void)snprintf(sql, sizeof sql, "SELECT %s FROM %s ORDER BY rowid;", cols, table);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db, sql, &s, &err), &err);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    while (sqlite3_step(s) == SQLITE_ROW) {
        for (int c = 0; c < sqlite3_column_count(s); c++) {
            const char *name = sqlite3_column_name(s, c);
            atlas_sha256_update(&ctx, name != NULL ? name : "", name != NULL ? strlen(name) : 0u);
            if (sqlite3_column_type(s, c) == SQLITE_NULL) {
                atlas_sha256_update(&ctx, "\x00NULL", 5u);
            } else {
                const unsigned char *t = sqlite3_column_text(s, c);
                int n = sqlite3_column_bytes(s, c);
                atlas_sha256_update(&ctx, t != NULL ? (const char *)t : "", n > 0 ? (size_t)n : 0u);
            }
        }
        atlas_sha256_update(&ctx, "\x01", 1u);
    }
    atlas_db_finish(db, s);
    unsigned char d[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, d);
    atlas_hex_encode(d, sizeof d, out);
}

/* Opens a fresh database and stops it at schema 30, one migration short of
 * the one under test -- `test_migrate29.c`'s shape for `atlas_db_migrate_list`. */
static void open_at_schema_30(fixture *fx, atlas_db **db_out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(open_fresh(fx, db_out, &err), &err);
    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 30u);
    T_OK(atlas_db_migrate_list(*db_out, all, 30u, &err), &err);
    T_EQ_INT(schema_of(*db_out), 30);
}

/* One document and one revision, written with plain SQL -- shared by
 * `seed_v30_rows` below (against a schema-30 database) and by the
 * vocabulary-pinning test (against a fully migrated one; nothing this
 * function inserts names a column migration 31 touches, so it is valid
 * against either schema). A fresh `uid` per call, so multiple tests in this
 * suite can each seed their own document without colliding on the
 * `decision_documents.uid` UNIQUE index. */
static void seed_doc_and_revision(atlas_db *db, const char *uid_suffix, int64_t *doc_id_out,
                                  int64_t *rev_id_out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[1024];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_documents"
                  "  (uid, repo_id, repo_root_hash, created_at, updated_at)"
                  "  VALUES ('atlas-dec-m31%s', 1, 'roothash',"
                  "          '2026-09-01T00:00:00Z', '2026-09-01T00:00:00Z');",
                  uid_suffix);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    *doc_id_out = count_sql(db, "SELECT id FROM decision_documents ORDER BY id DESC LIMIT 1;");
    T_REQUIRE(*doc_id_out > 0);

    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_revisions"
                  "  (document_id, revision_no, content_hash, title, proposed_by, created_at)"
                  "  VALUES (%lld, 1, 'c0ffee', 'a title', 'MODEL_PROPOSAL',"
                  "          '2026-09-01T00:00:00Z');",
                  (long long)*doc_id_out);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    *rev_id_out = count_sql(db, "SELECT id FROM decision_revisions ORDER BY id DESC LIMIT 1;");
    T_REQUIRE(*rev_id_out > 0);
}

/* One document, one revision (via `seed_doc_and_revision` above), three
 * events (one per actor value the old CHECK admitted, including
 * VERIFICATION_POLICY) and two challenges, written with plain SQL against
 * the schema-30 shape -- `atlas_db_decision_event_append` and
 * `atlas_db_decision_challenge_insert` already name `key_id`/`channel` and
 * would fail to prepare against a database that does not have them yet. */
static void seed_v30_rows(atlas_db *db, int64_t *doc_id_out, int64_t *rev_id_out) {
    atlas_err err;
    atlas_err_init(&err);
    seed_doc_and_revision(db, "000000000000000000000001", doc_id_out, rev_id_out);

    char sql[1024];
    /* Three events, one per actor value the old CHECK admitted, including
     * VERIFICATION_POLICY -- the brief's own fixture. */
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events"
                  "  (document_id, revision_id, revision_no, event, actor, content_hash,"
                  "   created_at)"
                  "  VALUES (%lld, %lld, 1, 'PROPOSED', 'MODEL_PROPOSAL', 'c0ffee',"
                  "          '2026-09-01T00:00:01Z');",
                  (long long)*doc_id_out, (long long)*rev_id_out);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events (document_id, revision_no, event, actor, created_at)"
                  "  VALUES (%lld, 0, 'PROPOSED', 'LOCAL_OPERATOR_CONFIRMED',"
                  "          '2026-09-01T00:00:02Z');",
                  (long long)*doc_id_out);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events (document_id, revision_no, event, actor, created_at)"
                  "  VALUES (%lld, 0, 'APPROVED', 'VERIFICATION_POLICY',"
                  "          '2026-09-01T00:00:03Z');",
                  (long long)*doc_id_out);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_events;"), 3);

    /* Two challenges: one live, one already consumed -- so `consumed_at`'s
     * preservation is exercised too. */
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_challenges"
                  "  (token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
                  "   created_at, expires_at, consumed)"
                  "  VALUES ('m31challengetokenaaaaaaaaaaaaaaa', 1, %lld, %lld, 1, 'c0ffee',"
                  "          'approve', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', 0);",
                  (long long)*doc_id_out, (long long)*rev_id_out);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_challenges"
                  "  (token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
                  "   created_at, expires_at, consumed, consumed_at)"
                  "  VALUES ('m31challengetokenbbbbbbbbbbbbbbb', 1, %lld, %lld, 1, 'c0ffee',"
                  "          'reject', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', 1,"
                  "          '2026-09-01T00:05:00Z');",
                  (long long)*doc_id_out, (long long)*rev_id_out);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_challenges;"), 2);
}

/* --- 1: a fresh database reaches 31 --------------------------------------- */

static void test_fresh_database_reaches_31(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 31);
    T_EQ_INT(schema_of(db), 31);

    T_CHECK(column_exists(db, "decision_events", "key_id"));
    T_CHECK(column_exists(db, "decision_challenges", "channel"));
    T_CHECK(column_exists(db, "decision_challenges", "key_id"));

    T_CHECK(object_exists(db, "index", "idx_decision_events_doc"));
    T_CHECK(object_exists(db, "index", "idx_decision_events_rev"));
    T_CHECK(object_exists(db, "index", "idx_decision_events_dedup"));
    T_CHECK(object_exists(db, "index", "idx_decision_challenges_repo"));

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 2: a database stopped at 30 reaches 31 losslessly, and the widened --- */
/* --- CHECKs behave exactly as the brief states ---------------------------- */

static void test_stopped_at_30_reaches_31_losslessly(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    open_at_schema_30(&fx, &db);

    int64_t doc_id = 0, rev_id = 0;
    seed_v30_rows(db, &doc_id, &rev_id);

    char events_before[ATLAS_SHA256_HEX_LEN + 1u];
    char challenges_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "decision_events", EVENTS_V30_COLS, events_before);
    table_digest_cols(db, "decision_challenges", CHALLENGES_V30_COLS, challenges_before);

    /* Before the migration under test: the replay agrees with the cache, on
     * three PROPOSED-shaped events for a document that is still PROPOSED. */
    bool ok = false;
    atlas_buf detail = ATLAS_BUF_INIT;
    T_OK(atlas_db_decision_verify(db, doc_id, &ok, &detail, &err), &err);
    T_CHECK_MSG(ok, "the replay disagreed before migration 31 ran: %s", atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);

    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 31u);
    T_OK(atlas_db_migrate_list(db, all, 31u, &err), &err);
    T_EQ_INT(schema_of(db), 31);

    /* Every column of every pre-existing row, byte-identical. */
    char events_after[ATLAS_SHA256_HEX_LEN + 1u];
    char challenges_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "decision_events", EVENTS_V30_COLS, events_after);
    table_digest_cols(db, "decision_challenges", CHALLENGES_V30_COLS, challenges_after);
    T_CHECK_MSG(strcmp(events_before, events_after) == 0,
                "migration 31 rewrote a decision_events row:\nbefore: %s\nafter:  %s",
                events_before, events_after);
    T_CHECK_MSG(strcmp(challenges_before, challenges_after) == 0,
                "migration 31 rewrote a decision_challenges row:\nbefore: %s\nafter:  %s",
                challenges_before, challenges_after);

    /* Every migrated challenge reads channel = 'LOCAL' and key_id IS NULL;
     * every migrated event reads key_id IS NULL. */
    T_EQ_INT((int)count_sql(db,
                            "SELECT COUNT(*) FROM decision_challenges"
                            " WHERE channel = 'LOCAL' AND key_id IS NULL;"),
             2);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_events WHERE key_id IS NULL;"), 3);

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);
    T_CHECK(object_exists(db, "index", "idx_decision_events_doc"));
    T_CHECK(object_exists(db, "index", "idx_decision_events_rev"));
    T_CHECK(object_exists(db, "index", "idx_decision_events_dedup"));
    T_CHECK(object_exists(db, "index", "idx_decision_challenges_repo"));

    /* The widened events CHECK: REMOTE_OPERATOR_CONFIRMED is admitted;
     * REMOTE_OPERATOR (a plausible near-miss) and UNKNOWN are refused. */
    char sql[512];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events (document_id, revision_no, event, actor, created_at)"
                  "  VALUES (%lld, 0, 'APPROVED', 'REMOTE_OPERATOR_CONFIRMED',"
                  "          '2026-09-01T00:00:04Z');",
                  (long long)doc_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    T_EQ_INT((int)count_sql(db,
                            "SELECT COUNT(*) FROM decision_events"
                            " WHERE actor = 'REMOTE_OPERATOR_CONFIRMED';"),
             1);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events (document_id, revision_no, event, actor, created_at)"
                  "  VALUES (%lld, 0, 'APPROVED', 'REMOTE_OPERATOR', '2026-09-01T00:00:05Z');",
                  (long long)doc_id);
    T_CHECK_MSG(atlas_db_exec_sql(db, sql, &err) != ATLAS_OK,
                "the events CHECK accepted the near-miss 'REMOTE_OPERATOR'");
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events (document_id, revision_no, event, actor, created_at)"
                  "  VALUES (%lld, 0, 'APPROVED', 'UNKNOWN', '2026-09-01T00:00:06Z');",
                  (long long)doc_id);
    T_CHECK_MSG(atlas_db_exec_sql(db, sql, &err) != ATLAS_OK,
                "the events CHECK accepted the zero actor name 'UNKNOWN'");

    /* The new channel CHECK: LOCAL and REMOTE are admitted; UNKNOWN and the
     * empty string are refused. */
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_challenges"
                  "  (token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
                  "   created_at, expires_at, channel, key_id)"
                  "  VALUES ('m31challengetokenremote00000001', 1, %lld, %lld, 1, 'c0ffee',"
                  "          'approve', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', 'REMOTE',"
                  "          '0123456789abcdef');",
                  (long long)doc_id, (long long)rev_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_challenges WHERE channel = 'REMOTE';"),
             1);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_challenges"
                  "  (token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
                  "   created_at, expires_at, channel)"
                  "  VALUES ('m31challengetokenunknown0000001', 1, %lld, %lld, 1, 'c0ffee',"
                  "          'approve', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', 'UNKNOWN');",
                  (long long)doc_id, (long long)rev_id);
    T_CHECK_MSG(atlas_db_exec_sql(db, sql, &err) != ATLAS_OK,
                "the channel CHECK accepted the zero channel name 'UNKNOWN'");
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_challenges"
                  "  (token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
                  "   created_at, expires_at, channel)"
                  "  VALUES ('m31challengetokenempty000000001', 1, %lld, %lld, 1, 'c0ffee',"
                  "          'approve', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', '');",
                  (long long)doc_id, (long long)rev_id);
    T_CHECK_MSG(atlas_db_exec_sql(db, sql, &err) != ATLAS_OK,
                "the channel CHECK accepted the empty string");

    /* The ledger replay needs no change, and this is the proof: insert a
     * REMOTE_OPERATOR_CONFIRMED APPROVED event by hand, make it the
     * revision's and the document's cache agree with what the replay must
     * now derive, and watch `atlas_db_decision_verify` still agree. */
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_events"
                  "  (document_id, revision_id, revision_no, event, actor, content_hash, key_id,"
                  "   created_at)"
                  "  VALUES (%lld, %lld, 1, 'APPROVED', 'REMOTE_OPERATOR_CONFIRMED', 'c0ffee',"
                  "          '0123456789abcdef', '2026-09-01T00:00:07Z');",
                  (long long)doc_id, (long long)rev_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    (void)snprintf(sql, sizeof sql,
                  "UPDATE decision_revisions SET state = 'APPROVED' WHERE id = %lld;",
                  (long long)rev_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    (void)snprintf(sql, sizeof sql,
                  "UPDATE decision_documents SET current_status = 'APPROVED',"
                  "  current_revision_id = %lld WHERE id = %lld;",
                  (long long)rev_id, (long long)doc_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);

    ok = false;
    atlas_buf_init(&detail);
    T_OK(atlas_db_decision_verify(db, doc_id, &ok, &detail, &err), &err);
    T_CHECK_MSG(ok,
                "the replay disagreed after a REMOTE_OPERATOR_CONFIRMED APPROVED event: %s",
                atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 3: the widened CHECKs are pinned to the C vocabulary itself --------- */

/* Fix round 1: a reviewer mutated migration 31 to remove `MODEL_INFERENCE`
 * from the widened events CHECK and `ctest -L unit` still passed 40/40,
 * because `test_stopped_at_30_reaches_31_losslessly` above only ever
 * exercised three of the six actor values. A CHECK vocabulary re-typed by
 * hand into SQL drifts silently from its C enum unless something inserts
 * every member and watches -- `test_migrate29.c`'s `check_vocab_matches_schema`
 * is the precedent: an INSERT against the real schema proves the CHECK's
 * actual behaviour, which a DDL substring match would not, because it would
 * still pass with a member's name sitting unused elsewhere in the file.
 *
 * This loops over `atlas_decision_actor_name` for every member the enum
 * declares and asserts the widened events CHECK accepts each one as a real
 * row, so a member missing from the CHECK -- added to the enum and never
 * added here, or quietly dropped as the reviewer's mutation did -- fails
 * this test rather than waiting for a production write to discover it. The
 * near-miss and zero-name refusals in test 2 above are the negative half of
 * this same CHECK and are not repeated here. */
static void check_actor_vocab_accepted(atlas_db *db, int64_t doc_id) {
    atlas_err err;
    atlas_err_init(&err);
    static const atlas_decision_actor MEMBERS[] = {
        ATLAS_DECISION_ACTOR_MODEL_PROPOSAL,
        ATLAS_DECISION_ACTOR_MODEL_INFERENCE,
        ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED,
        ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC,
        ATLAS_DECISION_ACTOR_VERIFICATION_POLICY,
        ATLAS_DECISION_ACTOR_REMOTE_OPERATOR_CONFIRMED,
    };
    for (size_t i = 0; i < sizeof MEMBERS / sizeof MEMBERS[0]; i++) {
        const char *name = atlas_decision_actor_name(MEMBERS[i]);
        char sql[512];
        (void)snprintf(sql, sizeof sql,
                      "INSERT INTO decision_events (document_id, revision_no, event, actor,"
                      "  created_at)"
                      "  VALUES (%lld, 0, 'PROPOSED', '%s', '2026-09-01T00:00:20Z');",
                      (long long)doc_id, name);
        atlas_status st = atlas_db_exec_sql(db, sql, &err);
        T_CHECK_MSG(st == ATLAS_OK, "the widened events CHECK refused actor '%s' (%s)", name,
                    atlas_err_msg(&err));
    }
}

/* The channel vocabulary is re-typed by hand into SQL exactly the same way
 * (`M31_CHALLENGES`'s `CHECK(channel IN ('LOCAL','REMOTE'))`), so the same
 * argument applies and gets the same loop -- over both non-zero members,
 * plus the zero member's own name refused, on `check_vocab_matches_schema`'s
 * full shape rather than only the positive half. */
static void check_channel_vocab_matches_schema(atlas_db *db, int64_t doc_id, int64_t rev_id) {
    atlas_err err;
    atlas_err_init(&err);
    static const atlas_decision_channel MEMBERS[] = {
        ATLAS_DECISION_CHANNEL_LOCAL,
        ATLAS_DECISION_CHANNEL_REMOTE,
    };
    for (size_t i = 0; i < sizeof MEMBERS / sizeof MEMBERS[0]; i++) {
        const char *name = atlas_decision_channel_name(MEMBERS[i]);
        char sql[768];
        (void)snprintf(sql, sizeof sql,
                      "INSERT INTO decision_challenges"
                      "  (token, repo_id, document_id, revision_id, revision_no, content_hash,"
                      "   intent, created_at, expires_at, channel)"
                      "  VALUES ('m31vocabchanneltoken%zu00000000000', 1, %lld, %lld, 1, 'c0ffee',"
                      "          'approve', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', '%s');",
                      i, (long long)doc_id, (long long)rev_id, name);
        atlas_status st = atlas_db_exec_sql(db, sql, &err);
        T_CHECK_MSG(st == ATLAS_OK, "the channel CHECK refused channel '%s' (%s)", name,
                    atlas_err_msg(&err));
    }
    /* And the zero member's own name -- what `atlas_decision_channel_name`
     * produces for UNKNOWN, which `atlas_decision_channel_parse` already
     * refuses to parse back -- is refused by the schema too. */
    char sql[768];
    const char *zero_name = atlas_decision_channel_name(ATLAS_DECISION_CHANNEL_UNKNOWN);
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO decision_challenges"
                  "  (token, repo_id, document_id, revision_id, revision_no, content_hash,"
                  "   intent, created_at, expires_at, channel)"
                  "  VALUES ('m31vocabchannelzero0000000000000000', 1, %lld, %lld, 1, 'c0ffee',"
                  "          'approve', '2026-09-01T00:00:00Z', '2026-09-01T00:10:00Z', '%s');",
                  (long long)doc_id, (long long)rev_id, zero_name);
    T_CHECK_MSG(atlas_db_exec_sql(db, sql, &err) != ATLAS_OK,
                "the channel CHECK accepted the zero member's own name '%s'", zero_name);
}

static void test_widened_check_vocabularies_are_pinned_to_the_c_enum(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), 31);

    int64_t doc_id = 0, rev_id = 0;
    seed_doc_and_revision(db, "0000000000000000000vocab1", &doc_id, &rev_id);

    check_actor_vocab_accepted(db, doc_id);
    check_channel_vocab_matches_schema(db, doc_id, rev_id);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 4: a rebuild that loses a ledger row is refused by name and rolled --- */
/* --- back completely, exactly as the real migration verifies itself ------ */

static const char BROKEN_M31_VERIFY[] =
    "CREATE TEMP TABLE m31_before AS"
    "  SELECT (SELECT COUNT(*) FROM decision_events) AS events_n,"
    "         (SELECT COUNT(*) FROM decision_challenges) AS challenges_n;";

/* Identical to the real `M31_EVENTS` in src/db/migrate.c, except the
 * `INSERT ... SELECT` drops the ledger row with the smallest id -- standing
 * in for a rebuild that silently lost a row, which `BROKEN_M31_CONFIRM`
 * below must catch. */
static const char BROKEN_M31_EVENTS[] =
    "CREATE TABLE decision_events_new ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL DEFAULT 0,"
    "  event TEXT NOT NULL CHECK(event IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED','RESOLVED')),"
    "  actor TEXT NOT NULL CHECK(actor IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC',"
    "     'VERIFICATION_POLICY','REMOTE_OPERATOR_CONFIRMED')),"
    "  content_hash TEXT,"
    "  challenge_id INTEGER,"
    "  superseded_by_revision_id INTEGER,"
    "  superseded_by_document_id INTEGER,"
    "  detail TEXT,"
    "  created_at TEXT NOT NULL,"
    "  dedup_key TEXT,"
    "  key_id TEXT"
    ");"
    "INSERT INTO decision_events_new"
    "  (id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
    "   superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key)"
    "  SELECT id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
    "         superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key"
    "  FROM decision_events"
    /* The injected loss. */
    "  WHERE id > (SELECT MIN(id) FROM decision_events);"
    "DROP TABLE decision_events;"
    "ALTER TABLE decision_events_new RENAME TO decision_events;"
    "CREATE INDEX idx_decision_events_doc ON decision_events(document_id, id);"
    "CREATE INDEX idx_decision_events_rev ON decision_events(revision_id, id);"
    "CREATE UNIQUE INDEX idx_decision_events_dedup ON decision_events(document_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* Identical to the real `M31_CHALLENGES`. */
static const char BROKEN_M31_CHALLENGES[] =
    "CREATE TABLE decision_challenges_new ("
    "  id INTEGER PRIMARY KEY,"
    "  token TEXT NOT NULL UNIQUE,"
    "  repo_id INTEGER NOT NULL,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede','revalidate',"
    "    'resolve')),"
    "  supersede_document_id INTEGER,"
    "  indexed_commit TEXT,"
    "  evidence_digest TEXT,"
    "  prior_freshness TEXT CHECK(prior_freshness IS NULL OR prior_freshness IN"
    "    ('FRESH','STALE','IMPACTED','UNKNOWN')),"
    "  prior_reasons TEXT,"
    "  created_at TEXT NOT NULL,"
    "  expires_at TEXT NOT NULL,"
    "  consumed INTEGER NOT NULL DEFAULT 0,"
    "  consumed_at TEXT,"
    "  channel TEXT NOT NULL DEFAULT 'LOCAL' CHECK(channel IN ('LOCAL','REMOTE')),"
    "  key_id TEXT"
    ");"
    "INSERT INTO decision_challenges_new"
    "  (id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "   supersede_document_id, indexed_commit, evidence_digest, prior_freshness, prior_reasons,"
    "   created_at, expires_at, consumed, consumed_at)"
    "  SELECT id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "         supersede_document_id, indexed_commit, evidence_digest, prior_freshness,"
    "         prior_reasons, created_at, expires_at, consumed, consumed_at"
    "  FROM decision_challenges;"
    "DROP TABLE decision_challenges;"
    "ALTER TABLE decision_challenges_new RENAME TO decision_challenges;"
    "CREATE INDEX idx_decision_challenges_repo ON decision_challenges"
    "  (repo_id, consumed, expires_at);";

/* Identical to the real `M31_CONFIRM`, including the per-table FK scoping
 * (Minor 2 of the fix round): each named CHECK names only its own table's
 * foreign keys, so this test's failure below is attributable to the same
 * constraint name production code would produce, not to an unscoped check
 * this test file forgot to update. */
static const char BROKEN_M31_CONFIRM[] =
    "CREATE TEMP TABLE m31_check("
    "  events_ok INTEGER NOT NULL"
    "    CONSTRAINT no_decision_event_may_be_lost_in_migration_31 CHECK(events_ok = 1),"
    "  challenges_ok INTEGER NOT NULL"
    "    CONSTRAINT no_decision_challenge_may_be_lost_in_migration_31 CHECK(challenges_ok = 1));"
    "INSERT INTO m31_check(events_ok, challenges_ok) SELECT"
    "  CASE WHEN (SELECT events_n FROM m31_before) = (SELECT COUNT(*) FROM decision_events)"
    "        AND (SELECT COUNT(*) FROM pragma_foreign_key_check('decision_events')) = 0"
    "       THEN 1 ELSE 0 END,"
    "  CASE WHEN (SELECT challenges_n FROM m31_before) = (SELECT COUNT(*) FROM decision_challenges)"
    "        AND (SELECT COUNT(*) FROM pragma_foreign_key_check('decision_challenges')) = 0"
    "       THEN 1 ELSE 0 END;"
    "DROP TABLE m31_check;"
    "DROP TABLE m31_before;";

static const char *const BROKEN_M31_STATEMENTS[] = {BROKEN_M31_VERIFY, BROKEN_M31_EVENTS,
                                                     BROKEN_M31_CHALLENGES, BROKEN_M31_CONFIRM,
                                                     NULL};

/* Minor 1 of the fix round: the events-loss list above never exercises
 * `no_decision_challenge_may_be_lost_in_migration_31`, because it only ever
 * injects loss into `decision_events`. This second list pairs the real,
 * unmodified events rebuild with a challenges rebuild that drops the
 * earliest challenge row, so the *other* named constraint is the one that
 * fires. */
static const char GOOD_M31_EVENTS[] =
    "CREATE TABLE decision_events_new ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL DEFAULT 0,"
    "  event TEXT NOT NULL CHECK(event IN"
    "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED','RESOLVED')),"
    "  actor TEXT NOT NULL CHECK(actor IN"
    "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC',"
    "     'VERIFICATION_POLICY','REMOTE_OPERATOR_CONFIRMED')),"
    "  content_hash TEXT,"
    "  challenge_id INTEGER,"
    "  superseded_by_revision_id INTEGER,"
    "  superseded_by_document_id INTEGER,"
    "  detail TEXT,"
    "  created_at TEXT NOT NULL,"
    "  dedup_key TEXT,"
    "  key_id TEXT"
    ");"
    "INSERT INTO decision_events_new"
    "  (id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
    "   superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key)"
    "  SELECT id, document_id, revision_id, revision_no, event, actor, content_hash, challenge_id,"
    "         superseded_by_revision_id, superseded_by_document_id, detail, created_at, dedup_key"
    "  FROM decision_events;"
    "DROP TABLE decision_events;"
    "ALTER TABLE decision_events_new RENAME TO decision_events;"
    "CREATE INDEX idx_decision_events_doc ON decision_events(document_id, id);"
    "CREATE INDEX idx_decision_events_rev ON decision_events(revision_id, id);"
    "CREATE UNIQUE INDEX idx_decision_events_dedup ON decision_events(document_id, dedup_key)"
    "  WHERE dedup_key IS NOT NULL;";

/* Identical to the real `M31_CHALLENGES`, except the `INSERT ... SELECT`
 * drops the challenge row with the smallest id. */
static const char LOSSY_M31_CHALLENGES[] =
    "CREATE TABLE decision_challenges_new ("
    "  id INTEGER PRIMARY KEY,"
    "  token TEXT NOT NULL UNIQUE,"
    "  repo_id INTEGER NOT NULL,"
    "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
    "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
    "  revision_no INTEGER NOT NULL,"
    "  content_hash TEXT NOT NULL,"
    "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede','revalidate',"
    "    'resolve')),"
    "  supersede_document_id INTEGER,"
    "  indexed_commit TEXT,"
    "  evidence_digest TEXT,"
    "  prior_freshness TEXT CHECK(prior_freshness IS NULL OR prior_freshness IN"
    "    ('FRESH','STALE','IMPACTED','UNKNOWN')),"
    "  prior_reasons TEXT,"
    "  created_at TEXT NOT NULL,"
    "  expires_at TEXT NOT NULL,"
    "  consumed INTEGER NOT NULL DEFAULT 0,"
    "  consumed_at TEXT,"
    "  channel TEXT NOT NULL DEFAULT 'LOCAL' CHECK(channel IN ('LOCAL','REMOTE')),"
    "  key_id TEXT"
    ");"
    "INSERT INTO decision_challenges_new"
    "  (id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "   supersede_document_id, indexed_commit, evidence_digest, prior_freshness, prior_reasons,"
    "   created_at, expires_at, consumed, consumed_at)"
    "  SELECT id, token, repo_id, document_id, revision_id, revision_no, content_hash, intent,"
    "         supersede_document_id, indexed_commit, evidence_digest, prior_freshness,"
    "         prior_reasons, created_at, expires_at, consumed, consumed_at"
    "  FROM decision_challenges"
    /* The injected loss. */
    "  WHERE id > (SELECT MIN(id) FROM decision_challenges);"
    "DROP TABLE decision_challenges;"
    "ALTER TABLE decision_challenges_new RENAME TO decision_challenges;"
    "CREATE INDEX idx_decision_challenges_repo ON decision_challenges"
    "  (repo_id, consumed, expires_at);";

static const char *const LOSSY_CHALLENGES_M31_STATEMENTS[] = {
    BROKEN_M31_VERIFY, GOOD_M31_EVENTS, LOSSY_M31_CHALLENGES, BROKEN_M31_CONFIRM, NULL};

static void test_a_lossy_migration_31_is_refused_and_rolled_back(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    open_at_schema_30(&fx, &db);

    int64_t doc_id = 0, rev_id = 0;
    seed_v30_rows(db, &doc_id, &rev_id);
    (void)rev_id;

    char events_before[ATLAS_SHA256_HEX_LEN + 1u];
    char challenges_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "decision_events", EVENTS_V30_COLS, events_before);
    table_digest_cols(db, "decision_challenges", CHALLENGES_V30_COLS, challenges_before);

    size_t base_count = 0;
    const atlas_migration *base = atlas_migrations(&base_count);
    T_REQUIRE(base_count >= 31u);
    /* Sized from the schema version rather than pinned at a literal --
     * `test_migrate8.c`'s own reasoning for the same array. */
    atlas_migration list[ATLAS_SCHEMA_VERSION];
    T_REQUIRE(base_count <= sizeof list / sizeof list[0]);
    memcpy(list, base, base_count * sizeof list[0]);
    /* Replace the real migration 31 (index 30: migrations are 1-indexed by
     * version, 0-indexed in this array) with the lossy one. */
    T_REQUIRE(list[30].version == 31);
    list[30].statements = BROKEN_M31_STATEMENTS;

    T_FAILS_WITH(atlas_db_migrate_list(db, list, base_count, &err), ATLAS_ERR_DB, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "no_decision_event_may_be_lost_in_migration_31") !=
                    NULL,
                "the error should name the constraint the lossy rebuild violated, got: %s",
                atlas_err_msg(&err));

    /* Nothing half-applied: the schema did not advance and every row from
     * before the attempt is exactly as it was. */
    T_EQ_INT(schema_of(db), 30);
    char events_after[ATLAS_SHA256_HEX_LEN + 1u];
    char challenges_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "decision_events", EVENTS_V30_COLS, events_after);
    table_digest_cols(db, "decision_challenges", CHALLENGES_V30_COLS, challenges_after);
    T_CHECK_MSG(strcmp(events_before, events_after) == 0,
                "a rolled-back migration 31 still changed decision_events");
    T_CHECK_MSG(strcmp(challenges_before, challenges_after) == 0,
                "a rolled-back migration 31 still changed decision_challenges");
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_events;"), 3);

    /* And the real migration still applies cleanly afterwards -- the failure
     * left a recoverable state, not a wedged one. */
    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), 31);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_events;"), 3);

    atlas_db_close(db);
    fx_close(&fx);
}

/* Minor 1's own test: the challenges-loss twin of the test above, over
 * `LOSSY_CHALLENGES_M31_STATEMENTS` rather than `BROKEN_M31_STATEMENTS`, so
 * `no_decision_challenge_may_be_lost_in_migration_31` is actually exercised
 * and not merely declared. */
static void test_a_lossy_challenges_rebuild_is_refused_and_rolled_back(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    open_at_schema_30(&fx, &db);

    int64_t doc_id = 0, rev_id = 0;
    seed_v30_rows(db, &doc_id, &rev_id);
    (void)rev_id;

    char events_before[ATLAS_SHA256_HEX_LEN + 1u];
    char challenges_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "decision_events", EVENTS_V30_COLS, events_before);
    table_digest_cols(db, "decision_challenges", CHALLENGES_V30_COLS, challenges_before);

    size_t base_count = 0;
    const atlas_migration *base = atlas_migrations(&base_count);
    T_REQUIRE(base_count >= 31u);
    atlas_migration list[ATLAS_SCHEMA_VERSION];
    T_REQUIRE(base_count <= sizeof list / sizeof list[0]);
    memcpy(list, base, base_count * sizeof list[0]);
    T_REQUIRE(list[30].version == 31);
    list[30].statements = LOSSY_CHALLENGES_M31_STATEMENTS;

    T_FAILS_WITH(atlas_db_migrate_list(db, list, base_count, &err), ATLAS_ERR_DB, &err);
    T_CHECK_MSG(
        strstr(atlas_err_msg(&err), "no_decision_challenge_may_be_lost_in_migration_31") != NULL,
        "the error should name the constraint the lossy challenges rebuild violated, got: %s",
        atlas_err_msg(&err));
    /* And not the events constraint -- a dangling row in the wrong table's
     * name is exactly Minor 2's failure mode, so this failure had better be
     * attributed correctly. */
    T_CHECK_MSG(
        strstr(atlas_err_msg(&err), "no_decision_event_may_be_lost_in_migration_31") == NULL,
        "the challenges-only loss was misattributed to the events constraint: %s",
        atlas_err_msg(&err));

    T_EQ_INT(schema_of(db), 30);
    char events_after[ATLAS_SHA256_HEX_LEN + 1u];
    char challenges_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest_cols(db, "decision_events", EVENTS_V30_COLS, events_after);
    table_digest_cols(db, "decision_challenges", CHALLENGES_V30_COLS, challenges_after);
    T_CHECK_MSG(strcmp(events_before, events_after) == 0,
                "a rolled-back lossy-challenges migration 31 still changed decision_events");
    T_CHECK_MSG(strcmp(challenges_before, challenges_after) == 0,
                "a rolled-back lossy-challenges migration 31 still changed decision_challenges");
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_challenges;"), 2);

    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), 31);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM decision_challenges;"), 2);

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a fresh database reaches 31 with the new columns and every index",
     test_fresh_database_reaches_31},
    {"a database stopped at 30 reaches 31 losslessly, and the widened CHECKs behave",
     test_stopped_at_30_reaches_31_losslessly},
    {"the widened events and new channel CHECKs are pinned to the C vocabulary itself",
     test_widened_check_vocabularies_are_pinned_to_the_c_enum},
    {"a rebuild that loses a ledger row is refused by name and rolled back completely",
     test_a_lossy_migration_31_is_refused_and_rolled_back},
    {"a rebuild that loses a challenge is refused by its own name and rolled back completely",
     test_a_lossy_challenges_rebuild_is_refused_and_rolled_back},
};

ATLAS_TEST_MAIN("migrate31", TESTS)

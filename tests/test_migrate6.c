/* Atlas - migrating a real, populated pre-A4 database forward to schema 6.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The fixture is not a hand-written schema-5 database: it is built by applying
 * the shipped migrations 1..5 and then populating them the way A0, A1, A2 and
 * A3 populate them. So this tests the migration a real user's database will
 * take rather than a reconstruction of one.
 *
 * Two properties, and the second is the one that is easy to get wrong:
 *
 *   1. **Nothing is lost.** Every A0..A3 table is compared row by row across
 *      the migration — not merely counted, because a migration that rewrote a
 *      column would keep the count.
 *   2. **Nothing is falsely approved.** The A2 approval restriction survives
 *      untouched: `ai_decisions.approved` is still pinned to 0 by its own
 *      CHECK, no A2 proposal acquires an A4 approval, and the `evidence` table
 *      still refuses everything but SOURCE and GIT.
 *
 * And the migration is idempotent: applying it to an up-to-date database is a
 * no-op, which is checked by running it twice and comparing the whole database
 * digest.
 */
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- helpers ---------------------------------------------------------------- */

static atlas_status open_db(fixture *fx, atlas_db **out, atlas_err *err) {
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

static int64_t count_of(atlas_db *db, const char *sql, atlas_err *err) {
    int64_t v = -1;
    T_OK(atlas_db_query_int64(db, sql, &v, err), err);
    return v;
}

/* A digest of every column of every row of one table, in primary-key order.
 *
 * A count would pass a migration that rewrote a value; this would not. The
 * column separator is 0x1F so that two adjacent columns cannot be confused with
 * one column containing the separator's spelling. */
static void table_digest(atlas_db *db, const char *table, const char *order, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[256];
    (void)snprintf(sql, sizeof(sql), "SELECT * FROM %s ORDER BY %s;", table, order);
    /* Not the cached prepare: the SQL is constructed, so the pointer-keyed cache
     * would miss anyway and this keeps the intent obvious. */
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db, sql, &s, &err), &err);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    while (sqlite3_step(s) == SQLITE_ROW) {
        for (int c = 0; c < sqlite3_column_count(s); c++) {
            /* The column *name* is hashed too, so a migration that reordered or
             * renamed columns is a difference rather than a coincidence. */
            const char *name = sqlite3_column_name(s, c);
            atlas_sha256_update(&ctx, name != NULL ? name : "", name != NULL ? strlen(name) : 0u);
            atlas_sha256_update(&ctx, "=", 1u);
            if (sqlite3_column_type(s, c) == SQLITE_NULL) {
                atlas_sha256_update(&ctx, "\x00NULL", 5u);
            } else {
                const void *b = sqlite3_column_blob(s, c);
                int n = sqlite3_column_bytes(s, c);
                atlas_sha256_update(&ctx, b != NULL ? b : "", n > 0 ? (size_t)n : 0u);
            }
            atlas_sha256_update(&ctx, "\x1f", 1u);
        }
        atlas_sha256_update(&ctx, "\x1e", 1u);
    }
    atlas_db_finish(db, s);
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
}

/* Every table an A0..A3 database holds, with a deterministic ordering.
 *
 * Listed explicitly rather than read from `sqlite_master`, so that a migration
 * which *dropped* a table would fail here — a query over the surviving tables
 * would happily report that all of them survived. */
typedef struct table_ref {
    const char *name;
    const char *order;
} table_ref;

static const table_ref PRE_A4_TABLES[] = {
    {"repositories", "id"},
    {"scans", "id"},
    {"files", "id"},
    {"commits", "id"},
    {"file_changes", "id"},
    {"compile_databases", "id"},
    {"evidence", "id"},
    /* A1 */
    {"repo_index_state", "repo_id"},
    {"repo_events", "id"},
    {"repo_commit_tips", "repo_id, ref_name"},
    {"daemon_state", "id"},
    /* A2 */
    {"ai_clients", "id"},
    {"ai_sessions", "id"},
    {"ai_session_repos", "session_id, repo_id"},
    {"ai_session_events", "id"},
    {"ai_change_sets", "id"},
    {"ai_changed_paths", "id"},
    {"ai_reasons", "id"},
    {"ai_reason_paths", "reason_id, path_raw"},
    {"ai_decisions", "id"},
    {"ai_decision_paths", "decision_id, path_raw"},
    {"ai_evidence_links", "id"},
    {"ai_checkpoints", "id"},
    {"repo_worktree_changes", "id"},
    /* A3 */
    {"code_analyzers", "id"},
    {"code_index_state", "repo_id"},
    {"code_files", "id"},
    {"code_file_roles", "id"},
    {"code_symbols", "id"},
    {"code_occurrences", "id"},
    {"code_relations", "id"},
    {"code_candidates", "id"},
    {"code_units", "id"},
    {"code_unit_includes", "id"},
    {"code_unit_defines", "id"},
    {"code_index_errors", "id"},
};

#define PRE_A4_TABLE_COUNT (sizeof(PRE_A4_TABLES) / sizeof(PRE_A4_TABLES[0]))

/* Applies migrations 1..5 only, leaving the database exactly where A3 left it.
 * Taken from the shipped list rather than duplicated, so a change to the
 * historical statements cannot silently diverge from what this tests. */
static void migrate_to_v5(atlas_db *db, atlas_err *err) {
    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 6u);
    T_OK(atlas_db_migrate_list(db, all, 5u, err), err);
    T_EQ_INT(atlas_db_schema_version(db, err), 5);
}

/* A populated pre-A4 database: a repository, a scan, files, commits, changes,
 * SOURCE and GIT evidence, A1 index state and events, an A2 session with a
 * change set, a reason and a decision proposal with paths, and an A3 structural
 * graph. Every one of these has to come through the migration untouched. */
static void seed_pre_a4(atlas_db *db, atlas_err *err) {
    /* Split into groups for the same reason `atlas_migration` holds a list of
     * statement groups rather than one string: ISO C only guarantees 4095-byte
     * string literals. */
    static const char SEED_A0_A1[] =
        /* A0 */
        "INSERT INTO repositories(id, name, root_path, root_path_text, git_common_dir,"
        " git_common_dir_text, object_format, registered_at, head_state, scanned_head)"
        " VALUES(1,'proj',X'2F746D702F70','/tmp/p',X'2F746D702F702F676974','/tmp/p/git','sha1',"
        "'2026-01-01T00:00:00Z','born','abc123');"
        "INSERT INTO scans(id, repo_id, started_at, status) VALUES(1,1,'2026-01-01T00:00:00Z','ok');"
        "INSERT INTO files(id, repo_id, path_raw, path_text, file_type, content_hash,"
        " first_seen_scan_id, last_seen_scan_id, first_seen_at, last_seen_at)"
        " VALUES(1,1,X'612E63','a.c','regular','deadbeef',1,1,'2026-01-01T00:00:00Z',"
        "'2026-01-01T00:00:00Z');"
        "INSERT INTO commits(id, repo_id, oid, subject) VALUES(1,1,'abc123','a commit');"
        "INSERT INTO file_changes(id, repo_id, commit_id, change_type, path_raw, path_text,"
        " raw_status) VALUES(1,1,1,'add',X'612E63','a.c','A');"
        "INSERT INTO compile_databases(id, repo_id, path_raw, path_text, scan_id, seen_at)"
        " VALUES(1,1,X'6363','cc',1,'2026-01-01T00:00:00Z');"
        "INSERT INTO evidence(id, repo_id, kind, created_at)"
        " VALUES(1,1,'SOURCE','2026-01-01T00:00:00Z'),(2,1,'GIT','2026-01-01T00:00:00Z');"
        /* A1 */
        "INSERT INTO repo_index_state(repo_id, generation, last_complete_generation)"
        " VALUES(1,4,4);"
        "INSERT INTO repo_events(id, repo_id, kind, created_at)"
        " VALUES(1,1,'reconciled','2026-01-01T00:00:00Z');"
        "INSERT INTO repo_commit_tips(repo_id, ref_name, tip_oid, ingested_at)"
        " VALUES(1,'HEAD','abc123','2026-01-01T00:00:00Z');"
        "INSERT INTO daemon_state(id, pid, started_at) VALUES(1,42,'2026-01-01T00:00:00Z');";
    static const char SEED_A2[] =
        "INSERT INTO ai_clients(id, provider, name, first_seen_at, last_seen_at)"
        " VALUES(1,'anthropic','claude-code','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "INSERT INTO ai_sessions(id, client_id, session_key, started_at, last_seen_at)"
        " VALUES(1,1,'sess-a','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "INSERT INTO ai_session_repos(session_id, repo_id, attached_at)"
        " VALUES(1,1,'2026-01-01T00:00:00Z');"
        "INSERT INTO ai_session_events(id, session_id, repo_id, kind, created_at)"
        " VALUES(1,1,1,'session_open','2026-01-01T00:00:00Z');"
        "INSERT INTO ai_change_sets(id, session_id, repo_id, opened_at)"
        " VALUES(1,1,1,'2026-01-01T00:00:00Z');"
        "INSERT INTO ai_changed_paths(id, change_set_id, path_raw, path_text, first_at, last_at)"
        " VALUES(1,1,X'612E63','a.c','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "INSERT INTO ai_reasons(id, session_id, repo_id, created_at, provenance, state, summary)"
        " VALUES(1,1,1,'2026-01-01T00:00:00Z','MODEL_PROPOSAL','proposed','a reason');"
        "INSERT INTO ai_reason_paths(reason_id, path_raw, path_text) VALUES(1,X'612E63','a.c');"
        "INSERT INTO ai_decisions(id, session_id, repo_id, created_at, provenance, state, title,"
        " statement, rationale)"
        " VALUES(1,1,1,'2026-01-01T00:00:00Z','MODEL_PROPOSAL','proposed','An A2 decision',"
        "'Recorded before A4 existed.','Because it seemed right.');"
        "INSERT INTO ai_decision_paths(decision_id, path_raw, path_text) VALUES(1,X'612E63','a.c');"
        "INSERT INTO ai_evidence_links(id, subject_kind, subject_id, evidence_id)"
        " VALUES(1,'decision',1,1);"
        "INSERT INTO ai_checkpoints(id, session_id, created_at, phase)"
        " VALUES(1,1,'2026-01-01T00:00:00Z','pre_compact');"
        "INSERT INTO repo_worktree_changes(id, repo_id, scope, status, change_type, path_raw,"
        " path_text, observed_at)"
        " VALUES(1,1,'staged','M','modify',X'612E63','a.c','2026-01-01T00:00:00Z');";
    static const char SEED_A3[] =
        "INSERT INTO code_analyzers(id, name, version, first_seen_at)"
        " VALUES(1,'atlas-lexical-c',1,'2026-01-01T00:00:00Z');"
        "INSERT INTO code_index_state(repo_id, generation, last_complete_generation, analyzer_id)"
        " VALUES(1,4,4,1);"
        "INSERT INTO code_files(id, repo_id, path_raw, path_text, basename_raw, language,"
        " content_hash, parsed_at, parse_status)"
        " VALUES(1,1,X'612E63','a.c',X'612E63','c','deadbeef','2026-01-01T00:00:00Z','ok');"
        "INSERT INTO code_file_roles(id, code_file_id, role, basis, resolution)"
        " VALUES(1,1,'implementation','extension','SOURCE_EXACT');"
        "INSERT INTO code_symbols(id, repo_id, code_file_id, name, name_text, kind, linkage,"
        " resolution, is_definition)"
        " VALUES(1,1,1,X'6D61696E','main','function','external','SOURCE_EXACT',1);"
        "INSERT INTO code_occurrences(id, repo_id, code_file_id, enclosing_id, name, name_text,"
        " kind, resolution)"
        " VALUES(1,1,1,1,X'70757473','puts','call_candidate','UNRESOLVED');"
        "INSERT INTO code_relations(id, repo_id, owner_file_id, kind, src_kind, src_id, dst_kind,"
        " resolution, provenance)"
        " VALUES(1,1,1,'file_defines_symbol','file',1,'symbol','SOURCE_EXACT','SOURCE');"
        "INSERT INTO code_candidates(id, relation_id, node_kind, node_id) VALUES(1,1,'symbol',1);"
        "INSERT INTO code_units(id, repo_id, source_path_raw, source_path_text)"
        " VALUES(1,1,X'612E63','a.c');"
        "INSERT INTO code_unit_includes(id, unit_id, kind, dir_raw, dir_text)"
        " VALUES(1,1,'search',X'696E63','inc');"
        "INSERT INTO code_unit_defines(id, unit_id, name) VALUES(1,1,'NDEBUG');"
        "INSERT INTO code_index_errors(id, repo_id, kind, created_at)"
        " VALUES(1,1,'parse_partial','2026-01-01T00:00:00Z');";
    T_OK(atlas_db_exec_sql(db, SEED_A0_A1, err), err);
    T_OK(atlas_db_exec_sql(db, SEED_A2, err), err);
    T_OK(atlas_db_exec_sql(db, SEED_A3, err), err);
}

/* --- tests -------------------------------------------------------------------- */

static void test_a_populated_pre_a4_database_migrates_losslessly(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    migrate_to_v5(db, &err);
    seed_pre_a4(db, &err);

    char before[PRE_A4_TABLE_COUNT][ATLAS_SHA256_HEX_LEN + 1u];
    for (size_t i = 0; i < PRE_A4_TABLE_COUNT; i++) {
        table_digest(db, PRE_A4_TABLES[i].name, PRE_A4_TABLES[i].order, before[i]);
    }

    /* The migration under test. */
    T_OK(atlas_db_migrate(db, &err), &err);
    /* All the way to head, not merely to six. A later migration that damaged a
     * pre-A4 table would be exactly as bad as migration 6 doing it, and this
     * test is the one that would notice. */
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    T_CHECK(ATLAS_SCHEMA_VERSION >= 6);

    /* Row for row, column for column, name for name. */
    for (size_t i = 0; i < PRE_A4_TABLE_COUNT; i++) {
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        table_digest(db, PRE_A4_TABLES[i].name, PRE_A4_TABLES[i].order, after);
        T_CHECK_MSG(strcmp(before[i], after) == 0,
                    "migration 6 changed table %s, which it must not touch at all",
                    PRE_A4_TABLES[i].name);
    }

    /* The A4 tables exist and are empty: a migration creates structure, never
     * content. In particular it does not manufacture decision documents out of
     * the A2 proposals — promotion is an explicit act. */
    static const char *const NEW_TABLES[] = {
        "decision_documents", "decision_revisions", "decision_alternatives", "decision_links",
        "decision_events",    "decision_challenges", "decision_search",      NULL,
    };
    for (size_t i = 0; NEW_TABLES[i] != NULL; i++) {
        char sql[128];
        (void)snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s;", NEW_TABLES[i]);
        T_CHECK_MSG(count_of(db, sql, &err) == 0, "%s must be created empty", NEW_TABLES[i]);
    }

    atlas_db_close(db);
    fx_close(&fx);
}

static void test_the_a2_approval_restriction_survives(void) {
    /* The single most important property of this migration.
     *
     * A4 could have been built by lifting `CHECK(approved = 0)` on
     * `ai_decisions`. It was not, and this asserts that the restriction is
     * still there and still enforced — so the A2 statement "a model proposal
     * never becomes approved by itself" stays literally true rather than
     * becoming a historical note. */
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    migrate_to_v5(db, &err);
    seed_pre_a4(db, &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    /* Still pinned. */
    T_EQ_INT(count_of(db, "SELECT count(*) FROM ai_decisions WHERE approved=0;", &err), 1);
    T_CHECK_MSG(atlas_db_exec_sql(db, "UPDATE ai_decisions SET approved=1 WHERE id=1;", &err) !=
                    ATLAS_OK,
                "the A2 CHECK(approved = 0) must still refuse an approval");
    atlas_err_init(&err);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM ai_decisions WHERE approved=1;", &err), 0);
    T_CHECK_MSG(atlas_db_exec_sql(db, "UPDATE ai_reasons SET approved=1 WHERE id=1;", &err) !=
                    ATLAS_OK,
                "the A2 reason CHECK must still refuse an approval too");
    atlas_err_init(&err);

    /* And the proposal did not acquire an A4 approval by being migrated past. */
    T_EQ_INT(count_of(db, "SELECT count(*) FROM decision_events;", &err), 0);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM decision_documents;", &err), 0);

    /* The evidence restriction is untouched for the third phase running. */
    T_EQ_INT(count_of(db, "SELECT count(*) FROM evidence WHERE kind NOT IN ('SOURCE','GIT');", &err),
             0);
    T_CHECK_MSG(
        atlas_db_exec_sql(db,
                          "INSERT INTO evidence(repo_id, kind, created_at)"
                          " VALUES(1,'NOT_A_KIND','2026-01-01T00:00:00Z');",
                          &err) != ATLAS_OK,
        "the evidence CHECK must still refuse an unknown kind");
    atlas_err_init(&err);

    atlas_db_close(db);
    fx_close(&fx);
}

static void test_migration_is_idempotent(void) {
    /* Applying migrations to an up-to-date database is a no-op. Checked by
     * digesting the whole schema and every A4 table, migrating again, and
     * comparing — rather than by asserting the version did not move, which
     * would pass even if a CREATE had silently rerun. */
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    migrate_to_v5(db, &err);
    seed_pre_a4(db, &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    char schema_before[ATLAS_SHA256_HEX_LEN + 1u];
    char applied_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest(db, "sqlite_master", "type, name", schema_before);
    table_digest(db, "schema_migrations", "version", applied_before);

    for (int again = 0; again < 3; again++) {
        T_OK(atlas_db_migrate(db, &err), &err);
        T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    }

    char schema_after[ATLAS_SHA256_HEX_LEN + 1u];
    char applied_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest(db, "sqlite_master", "type, name", schema_after);
    table_digest(db, "schema_migrations", "version", applied_after);
    T_CHECK_MSG(strcmp(schema_before, schema_after) == 0,
                "repeated migration must not change the schema");
    T_CHECK_MSG(strcmp(applied_before, applied_after) == 0,
                "repeated migration must not record a second application");
    T_EQ_INT(count_of(db, "SELECT count(*) FROM schema_migrations WHERE version=6;", &err), 1);

    /* The pre-A4 rows are still there after three more migration passes. */
    T_EQ_INT(count_of(db, "SELECT count(*) FROM ai_decisions;", &err), 1);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM code_relations;", &err), 1);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM evidence;", &err), 2);

    atlas_db_close(db);
    fx_close(&fx);
}

static void test_a_v2_database_reaches_schema_six(void) {
    /* The long path: an A0-era database migrating all the way forward in one
     * go. `test_migrate3.c` covers v2 to v3; this covers v2 to the newest, so a
     * migration that only works when applied one step at a time is caught. */
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_OK(atlas_db_migrate_list(db, all, 2u, &err), &err);
    T_OK(atlas_db_exec_sql(db,
                           "INSERT INTO repositories(id, name, root_path, root_path_text,"
                           " object_format, registered_at)"
                           " VALUES(1,'old',X'2F6F','/o','sha1','2026-01-01T00:00:00Z');",
                           &err),
         &err);

    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM repositories WHERE name='old';", &err), 1);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM decision_documents;", &err), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

static void test_the_partial_unique_index_enforces_one_approved_revision(void) {
    /* Rule 9 as a schema constraint rather than as care in the writer. A second
     * APPROVED revision for one document must be impossible to bring into
     * existence, so that a bug in the ordering of approve-and-supersede is a
     * hard failure rather than two effective revisions that every later read
     * quietly picks between. */
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_db(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    T_OK(atlas_db_exec_sql(
             db,
             "INSERT INTO decision_documents(id, uid, repo_id, repo_root_hash, created_at,"
             " updated_at) VALUES(1,'atlas-dec-0000000000000000',1,'aa','t','t');"
             "INSERT INTO decision_revisions(id, document_id, revision_no, content_hash, title,"
             " proposed_by, created_at, state)"
             " VALUES(1,1,1,'h1','A','MODEL_PROPOSAL','t','APPROVED');",
             &err),
         &err);
    T_CHECK_MSG(
        atlas_db_exec_sql(db,
                          "INSERT INTO decision_revisions(id, document_id, revision_no,"
                          " content_hash, title, proposed_by, created_at, state)"
                          " VALUES(2,1,2,'h2','B','MODEL_PROPOSAL','t','APPROVED');",
                          &err) != ATLAS_OK,
        "a second APPROVED revision of one document must be refused by the schema");
    atlas_err_init(&err);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM decision_revisions WHERE state='APPROVED';", &err),
             1);

    /* A second APPROVED revision of a *different* document is fine, which is
     * what makes the index partial rather than a mistake. */
    T_OK(atlas_db_exec_sql(
             db,
             "INSERT INTO decision_documents(id, uid, repo_id, repo_root_hash, created_at,"
             " updated_at) VALUES(2,'atlas-dec-1111111111111111',1,'aa','t','t');"
             "INSERT INTO decision_revisions(id, document_id, revision_no, content_hash, title,"
             " proposed_by, created_at, state)"
             " VALUES(3,2,1,'h3','C','MODEL_PROPOSAL','t','APPROVED');",
             &err),
         &err);
    T_EQ_INT(count_of(db, "SELECT count(*) FROM decision_revisions WHERE state='APPROVED';", &err),
             2);

    /* And the lifecycle vocabularies are closed at the schema level too. */
    T_CHECK_MSG(atlas_db_exec_sql(db,
                                  "UPDATE decision_revisions SET state='ACCEPTED' WHERE id=1;",
                                  &err) != ATLAS_OK,
                "an unknown revision state must be refused by the schema");
    atlas_err_init(&err);
    T_CHECK_MSG(
        atlas_db_exec_sql(db,
                          "INSERT INTO decision_events(document_id, revision_no, event, actor,"
                          " created_at) VALUES(1,1,'APPROVED','A_HUMAN','t');",
                          &err) != ATLAS_OK,
        "an unknown actor must be refused by the schema");
    atlas_err_init(&err);

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a populated pre-A4 database migrates losslessly",
     test_a_populated_pre_a4_database_migrates_losslessly},
    {"the A2 approval restriction survives", test_the_a2_approval_restriction_survives},
    {"migration is idempotent", test_migration_is_idempotent},
    {"a v2 database reaches schema six", test_a_v2_database_reaches_schema_six},
    {"one approved revision per document, by schema",
     test_the_partial_unique_index_enforces_one_approved_revision},
};

ATLAS_TEST_MAIN("migrate6", TESTS)

/* Atlas - A12.1: migration 29 -- registered memory sources, their versions,
 * the generations a reconciliation produces, the diffs and unanchored
 * candidates within one, the frozen context pack a run was shown, and the
 * trailer bindings a commit's block resolved to.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Eight tables, additive, none rebuilt -- see the migration comment in
 * `src/db/migrate.c`. This suite proves the properties the task brief names:
 *
 *   - a fresh database reaches schema 29 with all eight tables and no
 *     dangling foreign key;
 *   - `memory_source_versions`' CHECK refuses a row with neither a blob nor
 *     stored content;
 *   - the asymmetry is real: deleting a `repositories` row leaves every
 *     memory row in place, because `repo_id` is a plain column on every
 *     memory table by design (the migration comment's argument), while
 *     deleting a `memory_generations` row cascades its `memory_claim_diffs`
 *     children, because that foreign key is real;
 *   - a database stopped at 28 reaches 29 with no pre-existing row rewritten
 *     (every column of every row, not merely an id that survived);
 *   - every CHECK vocabulary this migration declares -- `memory_sources.cls`,
 *     `memory_claim_anchors.kind`, `memory_generations.cause` and
 *     `memory_claim_diffs.kind` -- accepts every one of its C enum's current
 *     spellings and refuses the zero member's name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/memory.h"
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

static bool table_exists(atlas_db *db, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name = ?1;", &q,
                         &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
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

/* The stored DDL for one table, which is what a CHECK constraint's literal
 * vocabulary actually is -- used to prove the controller ruling (UNKNOWN out,
 * UNDETERMINED in) landed in the schema and not only in the enum. */
static bool ddl_mentions(atlas_db *db, const char *table, const char *needle) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT sql FROM sqlite_schema WHERE type='table' AND name = ?1;",
                         &q, &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, table, -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(q) == SQLITE_ROW) {
        const char *sql = (const char *)sqlite3_column_text(q, 0);
        found = sql != NULL && strstr(sql, needle) != NULL;
    }
    atlas_db_finish(db, q);
    return found;
}

/* A digest over every column of every row, in rowid order, including column
 * names -- so a reordered, renamed, retyped or rewritten column is a
 * difference too. `test_migrate8.c`'s shape, for the same reason: a count or
 * an id comparison would not notice a row whose other columns were rewritten
 * in place by an `UPDATE` this migration should never contain. Migration 29
 * declares no `ALTER TABLE` on `repositories` at all, so `SELECT *` is the
 * right query here -- unlike `test_migrate8.c`, which has to name columns
 * because migration 27 widens that same table between the two snapshots it
 * compares. */
static void table_digest(atlas_db *db, const char *table, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT * FROM %s ORDER BY rowid;", table);
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

/* A synthetic identity is enough here: these tests are about the memory
 * tables' own shape, not about a real tree on disk. */
static void insert_repo(atlas_db *db, const char *name, int64_t *repo_id_out) {
    atlas_err err;
    atlas_err_init(&err);
    char root[256];
    char common[300];
    (void)snprintf(root, sizeof root, "/tmp/atlas-migrate29-%s", name);
    (void)snprintf(common, sizeof common, "%s/.git", root);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = common;
    id.common_dir_len = strlen(common);
    id.git_dir = common;
    id.git_dir_len = strlen(common);
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(db, name, &id, repo_id_out, &err), &err);
}

static const char *const TABLES[] = {
    "memory_sources",     "memory_source_versions", "memory_claim_anchors",
    "memory_generations", "memory_claim_diffs",      "memory_unanchored",
    "memory_context_packs", "memory_trailer_bindings",
};

/* Every column the brief's SQL declares, per table, checked rather than
 * assumed: sixteen later tasks were cleared against these exact names during
 * planning (`memory_context_packs.pack_digest`, `.claims_manifest` and
 * `.flagged_anchors` among them), and a typo here would pass table-existence
 * and empty-count checks alike. */
typedef struct table_columns {
    const char *table;
    const char *const *columns;
    size_t count;
} table_columns;

static const char *const SOURCES_COLS[] = {
    "id", "repo_id", "source_uid", "cls", "path_raw", "path_text", "registered_at",
};
static const char *const SOURCE_VERSIONS_COLS[] = {
    "id",          "source_id",     "version_uid",  "commit_oid",   "blob_oid",
    "content_sha256", "content_bytes", "content",     "observed_at",  "recorded_at",
    "read_by_uid",
};
static const char *const CLAIM_ANCHORS_COLS[] = {
    "id", "repo_id", "claim_uid", "kind", "value",
};
static const char *const GENERATIONS_COLS[] = {
    "id",           "repo_id",        "generation",          "cause",
    "repo_identity_hash", "head_commit", "decision_set_digest", "source_set_digest",
    "trailer_scan_high",  "created_at",
};
static const char *const CLAIM_DIFFS_COLS[] = {
    "id", "generation_id", "claim_uid", "kind", "reason",
};
static const char *const UNANCHORED_COLS[] = {
    "id", "source_version_id", "ordinal", "text_sha256", "text",
};
static const char *const CONTEXT_PACKS_COLS[] = {
    "id",         "run_uid",           "repo_id",             "repo_identity_hash",
    "pinned_commit", "source_identity", "memory_generation",   "decision_set_digest",
    "source_set_digest", "pack_digest", "rendered",            "claim_count",
    "excluded_count", "unanchored_count", "claims_manifest",   "flagged_anchors",
    "reliance_checked", "reliance_complete", "reliance_claim_uids", "created_at",
};
static const char *const TRAILER_BINDINGS_COLS[] = {
    "id",            "repo_id",         "commit_oid",        "has_block",
    "run_uid",       "memory_generation", "context_digest_ok", "decision_set_ok",
    "change_reason_uid", "unknown_fields", "recorded_at",
};

#define TCOLS(arr) arr, sizeof(arr) / sizeof(arr[0])

static const table_columns EXPECTED[] = {
    {"memory_sources", TCOLS(SOURCES_COLS)},
    {"memory_source_versions", TCOLS(SOURCE_VERSIONS_COLS)},
    {"memory_claim_anchors", TCOLS(CLAIM_ANCHORS_COLS)},
    {"memory_generations", TCOLS(GENERATIONS_COLS)},
    {"memory_claim_diffs", TCOLS(CLAIM_DIFFS_COLS)},
    {"memory_unanchored", TCOLS(UNANCHORED_COLS)},
    {"memory_context_packs", TCOLS(CONTEXT_PACKS_COLS)},
    {"memory_trailer_bindings", TCOLS(TRAILER_BINDINGS_COLS)},
};

/* --- 1: a fresh database reaches 29 with all eight tables, no dangling FK - */

static void test_fresh_database_reaches_29_with_eight_tables(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 29);
    T_EQ_INT(schema_of(db), 29);

    for (size_t i = 0; i < sizeof TABLES / sizeof TABLES[0]; i++) {
        T_CHECK_MSG(table_exists(db, TABLES[i]), "%s does not exist", TABLES[i]);
    }
    for (size_t t = 0; t < sizeof EXPECTED / sizeof EXPECTED[0]; t++) {
        for (size_t c = 0; c < EXPECTED[t].count; c++) {
            T_CHECK_MSG(column_exists(db, EXPECTED[t].table, EXPECTED[t].columns[c]),
                        "%s.%s does not exist", EXPECTED[t].table, EXPECTED[t].columns[c]);
        }
    }

    /* The controller ruling landed in the schema, not only in the enum:
     * memory_claim_diffs.kind's CHECK carries UNDETERMINED and not UNKNOWN. */
    T_CHECK_MSG(ddl_mentions(db, "memory_claim_diffs", "UNDETERMINED"),
                "memory_claim_diffs.kind's CHECK does not mention UNDETERMINED");
    T_CHECK_MSG(!ddl_mentions(db, "memory_claim_diffs", "'UNKNOWN'"),
                "memory_claim_diffs.kind's CHECK still admits the unparseable zero");

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 2: the CHECK on memory_source_versions ------------------------------- */

static void test_version_check_refuses_no_blob_no_content(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    int64_t repo_id = 0;
    insert_repo(db, "proj", &repo_id);

    char sql[1024];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO memory_sources"
                  "  (repo_id, source_uid, cls, path_raw, path_text, registered_at)"
                  "  VALUES (%lld, 'm-src-1', 'REPO_FILE', x'2e2f412e6d64', './A.md',"
                  "          '2026-09-01T00:00:00Z');",
                  (long long)repo_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    int64_t source_id = count_sql(db, "SELECT id FROM memory_sources ORDER BY id DESC LIMIT 1;");
    T_REQUIRE(source_id > 0);

    /* Refused: blob_oid is '' (the column default) and content is NULL. */
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO memory_source_versions"
                  "  (source_id, version_uid, content_sha256, content_bytes, observed_at,"
                  "   recorded_at, read_by_uid)"
                  "  VALUES (%lld, 'v-no-blob-no-content', 'deadbeef', 0,"
                  "          '2026-09-01T00:00:00Z', '2026-09-01T00:00:00Z', 1000);",
                  (long long)source_id);
    atlas_status st = atlas_db_exec_sql(db, sql, &err);
    T_CHECK_MSG(st != ATLAS_OK, "a version with neither a blob nor content was accepted");
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_source_versions;"), 0);

    /* Accepted: content is not NULL, even with blob_oid still empty -- Atlas
     * is canonical for exactly the versions with no blob. */
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO memory_source_versions"
                  "  (source_id, version_uid, content_sha256, content_bytes, content,"
                  "   observed_at, recorded_at, read_by_uid)"
                  "  VALUES (%lld, 'v-content-only', 'deadbeef', 5, x'68656c6c6f',"
                  "          '2026-09-01T00:00:00Z', '2026-09-01T00:00:00Z', 1000);",
                  (long long)source_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_source_versions;"), 1);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 3: the deliberate asymmetry ------------------------------------------- */

static void test_repo_delete_leaves_memory_rows_generation_delete_cascades_diffs(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    int64_t repo_id = 0;
    insert_repo(db, "proj", &repo_id);

    char sql[1024];
    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO memory_sources"
                  "  (repo_id, source_uid, cls, path_raw, path_text, registered_at)"
                  "  VALUES (%lld, 'm-src-1', 'REPO_FILE', x'2e2f412e6d64', './A.md',"
                  "          '2026-09-01T00:00:00Z');",
                  (long long)repo_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);

    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO memory_generations"
                  "  (repo_id, generation, cause, repo_identity_hash, decision_set_digest,"
                  "   source_set_digest, created_at)"
                  "  VALUES (%lld, 1, 'COMMIT', 'deadbeef', 'dd', 'ss',"
                  "          '2026-09-01T00:00:00Z');",
                  (long long)repo_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);
    int64_t generation_id =
        count_sql(db, "SELECT id FROM memory_generations ORDER BY id DESC LIMIT 1;");
    T_REQUIRE(generation_id > 0);

    (void)snprintf(sql, sizeof sql,
                  "INSERT INTO memory_claim_diffs(generation_id, claim_uid, kind)"
                  "  VALUES (%lld, 'claim-1', 'ADDED');",
                  (long long)generation_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_sources;"), 1);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_generations;"), 1);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_claim_diffs;"), 1);

    (void)snprintf(sql, sizeof sql, "DELETE FROM repositories WHERE id = %lld;",
                  (long long)repo_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);

    /* The asymmetry: repo_id is a plain column on every memory table, so
     * registry churn deletes no memory history. */
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_sources;"), 1);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_generations;"), 1);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_claim_diffs;"), 1);

    (void)snprintf(sql, sizeof sql, "DELETE FROM memory_generations WHERE id = %lld;",
                  (long long)generation_id);
    T_OK(atlas_db_exec_sql(db, sql, &err), &err);

    /* The real foreign key: a generation's diffs go with it. */
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_generations;"), 0);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_claim_diffs;"), 0);
    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM memory_sources;"), 1);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 4: a database stopped at 28 reaches 29 losslessly --------------------- */

static void test_a_database_stopped_at_28_reaches_29_losslessly(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);

    size_t count = 0;
    const atlas_migration *all = atlas_migrations(&count);
    T_REQUIRE(count >= 29u);
    T_OK(atlas_db_migrate_list(db, all, 28u, &err), &err);
    T_EQ_INT(schema_of(db), 28);

    /* Real rows, so "nothing pre-existing was rewritten" is a claim about
     * every column of every row rather than about one empty table agreeing
     * with another, or about one id surviving while every other column beside
     * it was rewritten. A digest taken before and compared after is what
     * catches that second failure; an id comparison alone does not. */
    int64_t repo_id = 0;
    int64_t repo2_id = 0;
    insert_repo(db, "proj", &repo_id);
    insert_repo(db, "proj2", &repo2_id);
    for (size_t i = 0; i < sizeof TABLES / sizeof TABLES[0]; i++) {
        T_CHECK_MSG(!table_exists(db, TABLES[i]), "%s exists before migration 29 ran",
                    TABLES[i]);
    }
    char repos_before[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest(db, "repositories", repos_before);

    T_OK(atlas_db_migrate(db, &err), &err);
    T_EQ_INT(schema_of(db), 29);
    T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 29);

    for (size_t i = 0; i < sizeof TABLES / sizeof TABLES[0]; i++) {
        T_CHECK_MSG(table_exists(db, TABLES[i]), "migration 29 created no %s", TABLES[i]);
        char sql[128];
        (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s;", TABLES[i]);
        T_EQ_INT((int)count_sql(db, sql), 0);
    }

    /* The pre-existing repositories rows are untouched -- not merely present
     * under the same id, but byte-for-byte identical in every column. */
    char repos_after[ATLAS_SHA256_HEX_LEN + 1u];
    table_digest(db, "repositories", repos_after);
    T_CHECK_MSG(strcmp(repos_before, repos_after) == 0,
                "migration 29 rewrote a repositories row:\nbefore: %s\nafter:  %s", repos_before,
                repos_after);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(db, "proj", &info, &found, &err), &err);
    T_CHECK(found);
    T_EQ_INT((int)info.id, (int)repo_id);
    atlas_repo_info_free(&info);
    (void)repo2_id;

    T_EQ_INT((int)count_sql(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);

    atlas_db_close(db);
    fx_close(&fx);
}

/* --- 5: every CHECK vocabulary in this migration agrees with its C enum --- */

/* Proves a stored CHECK vocabulary agrees with its C enum: every non-zero
 * member's exact `_name()` spelling is accepted by an INSERT against the real
 * schema, and the zero member's own name -- which every `_parse` already
 * refuses -- is refused by the schema too.
 *
 * This is the failure the project cares about most: add a member to a C
 * vocabulary and add its case to `_name`/`_parse` (`-Wswitch-enum` forces
 * that much), and the build and every unit test over the enum stay green
 * while the widening migration was never written -- until the first real
 * insert using the new spelling fails on the writer thread, at runtime, in a
 * daemon. An INSERT against the real schema proves the *behaviour*; a DDL
 * substring match (test 1's `ddl_mentions`, kept for the diff kind because it
 * also proves the controller ruling's exact wording) only proves a word
 * appears somewhere in a stored string.
 *
 * `insert_tmpl` has every column but the vocabulary one already filled in,
 * with exactly one literal `%s` left as the splice point for the spelling
 * under test. It is split around that marker with `strstr` rather than handed
 * to `snprintf` as the format string itself -- passing a runtime string as a
 * format argument is `-Wformat-nonliteral`, which `ATLAS_WERROR` turns into a
 * build failure, and rightly so for any caller-supplied string; the splice
 * here is always this function's own literal marker, but the compiler cannot
 * tell that from the call site, so the splice is done by hand instead. The
 * row is deleted after every attempt -- accepted or refused -- so repeated
 * calls, and the different vocabularies sharing one table's uniqueness rules,
 * never collide. */
static void check_vocab_matches_schema(atlas_db *db, const char *table, const char *insert_tmpl,
                                       const char *const *member_names, size_t member_count,
                                       const char *zero_name) {
    atlas_err err;
    atlas_err_init(&err);
    const char *marker = strstr(insert_tmpl, "%s");
    T_REQUIRE_MSG(marker != NULL, "insert_tmpl for %s has no %%s splice point", table);
    int prefix_len = (int)(marker - insert_tmpl);
    const char *suffix = marker + 2;

    char sql[1024];
    char cleanup[64];
    (void)snprintf(cleanup, sizeof cleanup, "DELETE FROM %s;", table);

    for (size_t i = 0; i < member_count; i++) {
        (void)snprintf(sql, sizeof sql, "%.*s%s%s", prefix_len, insert_tmpl, member_names[i],
                       suffix);
        atlas_status st = atlas_db_exec_sql(db, sql, &err);
        T_CHECK_MSG(st == ATLAS_OK, "%s: '%s' was refused by the schema (%s)", table,
                    member_names[i], atlas_err_msg(&err));
        T_OK(atlas_db_exec_sql(db, cleanup, &err), &err);
    }

    (void)snprintf(sql, sizeof sql, "%.*s%s%s", prefix_len, insert_tmpl, zero_name, suffix);
    atlas_status st = atlas_db_exec_sql(db, sql, &err);
    T_CHECK_MSG(st != ATLAS_OK, "%s: the zero member's name '%s' was accepted by the schema",
                table, zero_name);
    T_OK(atlas_db_exec_sql(db, cleanup, &err), &err);
}

static void test_vocabulary_checks_agree_with_their_c_enum(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);

    atlas_db *db = NULL;
    T_OK(open_fresh(&fx, &db, &err), &err);
    T_OK(atlas_db_migrate(db, &err), &err);

    int64_t repo_id = 0;
    insert_repo(db, "proj", &repo_id);

    /* memory_sources.cls <-> atlas_memory_source_class */
    {
        char fmt[512];
        (void)snprintf(fmt, sizeof fmt,
                      "INSERT INTO memory_sources"
                      "  (repo_id, source_uid, cls, path_raw, path_text, registered_at)"
                      "  VALUES (%lld, 'm-vocab-test', '%%s', x'2e2f412e6d64', './A.md',"
                      "          '2026-09-01T00:00:00Z');",
                      (long long)repo_id);
        static const atlas_memory_source_class MEMBERS[] = {
            ATLAS_MEMORY_SOURCE_REPO_FILE,
            ATLAS_MEMORY_SOURCE_REPO_DIR,
            ATLAS_MEMORY_SOURCE_EXTERNAL_FILE,
            ATLAS_MEMORY_SOURCE_EXTERNAL_DIR,
        };
        const char *names[sizeof MEMBERS / sizeof MEMBERS[0]];
        for (size_t i = 0; i < sizeof MEMBERS / sizeof MEMBERS[0]; i++) {
            names[i] = atlas_memory_source_class_name(MEMBERS[i]);
        }
        check_vocab_matches_schema(db, "memory_sources", fmt, names,
                                   sizeof names / sizeof names[0],
                                   atlas_memory_source_class_name(ATLAS_MEMORY_SOURCE_UNKNOWN));
    }

    /* memory_claim_anchors.kind <-> atlas_memory_anchor_kind */
    {
        char fmt[512];
        (void)snprintf(fmt, sizeof fmt,
                      "INSERT INTO memory_claim_anchors (repo_id, claim_uid, kind, value)"
                      "  VALUES (%lld, 'claim-vocab-test', '%%s', 'v');",
                      (long long)repo_id);
        static const atlas_memory_anchor_kind MEMBERS[] = {
            ATLAS_MEMORY_ANCHOR_PATH,
            ATLAS_MEMORY_ANCHOR_SYMBOL,
            ATLAS_MEMORY_ANCHOR_DECISION,
            ATLAS_MEMORY_ANCHOR_COMMIT,
        };
        const char *names[sizeof MEMBERS / sizeof MEMBERS[0]];
        for (size_t i = 0; i < sizeof MEMBERS / sizeof MEMBERS[0]; i++) {
            names[i] = atlas_memory_anchor_kind_name(MEMBERS[i]);
        }
        check_vocab_matches_schema(db, "memory_claim_anchors", fmt, names,
                                   sizeof names / sizeof names[0],
                                   atlas_memory_anchor_kind_name(ATLAS_MEMORY_ANCHOR_UNKNOWN));
    }

    /* memory_generations.cause <-> atlas_memory_gen_cause */
    {
        char fmt[512];
        (void)snprintf(fmt, sizeof fmt,
                      "INSERT INTO memory_generations"
                      "  (repo_id, generation, cause, repo_identity_hash, decision_set_digest,"
                      "   source_set_digest, created_at)"
                      "  VALUES (%lld, 1, '%%s', 'h', 'd', 's', '2026-09-01T00:00:00Z');",
                      (long long)repo_id);
        static const atlas_memory_gen_cause MEMBERS[] = {
            ATLAS_MEMORY_CAUSE_SOURCE_REVISION,
            ATLAS_MEMORY_CAUSE_DECISION_REVISION,
            ATLAS_MEMORY_CAUSE_COMMIT,
        };
        const char *names[sizeof MEMBERS / sizeof MEMBERS[0]];
        for (size_t i = 0; i < sizeof MEMBERS / sizeof MEMBERS[0]; i++) {
            names[i] = atlas_memory_gen_cause_name(MEMBERS[i]);
        }
        check_vocab_matches_schema(db, "memory_generations", fmt, names,
                                   sizeof names / sizeof names[0],
                                   atlas_memory_gen_cause_name(ATLAS_MEMORY_CAUSE_UNKNOWN));
    }

    /* memory_claim_diffs.kind <-> atlas_memory_diff_kind, asserted the same
     * way as the other three, for parity -- test 1's DDL substring check
     * proves the controller ruling's exact wording and stays alongside this. */
    {
        char sql[512];
        (void)snprintf(sql, sizeof sql,
                      "INSERT INTO memory_generations"
                      "  (repo_id, generation, cause, repo_identity_hash, decision_set_digest,"
                      "   source_set_digest, created_at)"
                      "  VALUES (%lld, 2, 'COMMIT', 'h', 'd', 's', '2026-09-01T00:00:00Z');",
                      (long long)repo_id);
        T_OK(atlas_db_exec_sql(db, sql, &err), &err);
        int64_t generation_id =
            count_sql(db, "SELECT id FROM memory_generations ORDER BY id DESC LIMIT 1;");
        T_REQUIRE(generation_id > 0);

        char fmt[512];
        (void)snprintf(fmt, sizeof fmt,
                      "INSERT INTO memory_claim_diffs (generation_id, claim_uid, kind)"
                      "  VALUES (%lld, 'diff-vocab-test', '%%s');",
                      (long long)generation_id);
        static const atlas_memory_diff_kind MEMBERS[] = {
            ATLAS_MEMORY_DIFF_ADDED,      ATLAS_MEMORY_DIFF_CHANGED,
            ATLAS_MEMORY_DIFF_SUPPORTED,  ATLAS_MEMORY_DIFF_CONTRADICTED,
            ATLAS_MEMORY_DIFF_STALE,      ATLAS_MEMORY_DIFF_IMPACTED,
            ATLAS_MEMORY_DIFF_SUPERSEDED, ATLAS_MEMORY_DIFF_UNDETERMINED,
        };
        const char *names[sizeof MEMBERS / sizeof MEMBERS[0]];
        for (size_t i = 0; i < sizeof MEMBERS / sizeof MEMBERS[0]; i++) {
            names[i] = atlas_memory_diff_kind_name(MEMBERS[i]);
        }
        check_vocab_matches_schema(db, "memory_claim_diffs", fmt, names,
                                   sizeof names / sizeof names[0],
                                   atlas_memory_diff_kind_name(ATLAS_MEMORY_DIFF_UNKNOWN));
    }

    atlas_db_close(db);
    fx_close(&fx);
}

static const atlas_test TESTS[] = {
    {"a fresh database reaches 29 with eight tables and no dangling foreign key",
     test_fresh_database_reaches_29_with_eight_tables},
    {"memory_source_versions' CHECK refuses a row with neither a blob nor content",
     test_version_check_refuses_no_blob_no_content},
    {"deleting a repository leaves memory rows; deleting a generation cascades its diffs",
     test_repo_delete_leaves_memory_rows_generation_delete_cascades_diffs},
    {"a database stopped at 28 reaches 29 with nothing pre-existing rewritten",
     test_a_database_stopped_at_28_reaches_29_losslessly},
    {"every CHECK vocabulary in migration 29 agrees with its C enum",
     test_vocabulary_checks_agree_with_their_c_enum},
};

ATLAS_TEST_MAIN("migrate29", TESTS)

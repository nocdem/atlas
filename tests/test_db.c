/* Atlas - database tests: migrations, rollback, records and evidence rules.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Covers required tests 1 (migration idempotency) and 2 (rollback on a failed
 * migration).
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct db_env {
    fixture fx;
    atlas_buf path;
    atlas_db *db;
} db_env;

static void db_env_open(db_env *e, bool migrate) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->path);
    T_OK(atlas_buf_appendf(&e->path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->path), &e->db, &err), &err);
    if (migrate) {
        T_OK(atlas_db_migrate(e->db, &err), &err);
    }
}

static void db_env_close(db_env *e) {
    atlas_db_close(e->db);
    e->db = NULL;
    atlas_buf_free(&e->path);
    fx_close(&e->fx);
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
    atlas_buf sql = ATLAS_BUF_INIT;
    /* The name is a test literal, never user input. */
    T_OK(atlas_buf_appendf(&sql, &err,
                           "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='%s';",
                           name),
         &err);
    int64_t v = 0;
    T_OK(atlas_db_query_int64(db, atlas_buf_cstr(&sql), &v, &err), &err);
    atlas_buf_free(&sql);
    return v > 0;
}

/* Builds a minimal identity for a synthetic repository. */
static atlas_repo_identity make_ident(const char *root, const char *cdir, const char *gdir,
                                     bool linked, const char *fmt) {
    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = cdir;
    id.common_dir_len = cdir != NULL ? strlen(cdir) : 0u;
    id.git_dir = gdir;
    id.git_dir_len = gdir != NULL ? strlen(gdir) : 0u;
    id.is_linked_worktree = linked;
    id.object_format = fmt;
    return id;
}

/* Registers a synthetic repository through the identity struct. */
static atlas_status add_repo(atlas_db *db, const char *name, const char *root, const char *cdir,
                            const char *gdir, bool linked, const char *fmt, int64_t *id,
                            atlas_err *err) {
    atlas_repo_identity ident = make_ident(root, cdir, gdir, linked, fmt);
    return atlas_db_repo_add(db, name, &ident, id, err);
}

/* --- required test 1: migration idempotency ------------------------------ */

static void test_migration_idempotency(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, false);
    atlas_err err;
    atlas_err_init(&err);

    T_EQ_INT(atlas_db_schema_version(e.db, &err), 0);
    T_OK(atlas_db_migrate(e.db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);

    /* Every table the A0 schema promises must exist. */
    const char *tables[] = {"schema_migrations", "repositories", "scans",   "files",
                            "commits",           "file_changes", "compile_databases",
                            "evidence"};
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        T_CHECK_MSG(table_exists(e.db, tables[i]), "table %s is missing", tables[i]);
    }

    int64_t applied = count_sql(e.db, "SELECT count(*) FROM schema_migrations;");

    /* Migrating again must change nothing at all. */
    for (int round = 0; round < 3; round++) {
        T_OK(atlas_db_migrate(e.db, &err), &err);
        T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);
        T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM schema_migrations;"), applied);
    }

    /* And it must survive being reopened. */
    atlas_db_close(e.db);
    e.db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&e.path), &e.db, &err), &err);
    T_OK(atlas_db_migrate(e.db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM schema_migrations;"), applied);

    db_env_close(&e);
}

/* --- required test 2: rollback on a failed migration --------------------- */

static void test_migration_rollback(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    /* A migration whose second statement is invalid: the first statement must not
     * survive, and the version must not advance. */
    static const char *const bad_statements[] = {
        "CREATE TABLE atlas_rollback_probe(id INTEGER PRIMARY KEY);",
        "CREATE TABLE this is not valid sql;",
        NULL,
    };
    size_t base_count = 0;
    const atlas_migration *base = atlas_migrations(&base_count);
    T_REQUIRE(base_count >= 1u);

    /* Zeroed before anything is assigned. `atlas_migration` gained
     * `foreign_keys_off` in A9.1 and both construction sites in this file kept
     * assigning three fields out of four, so the flag was whatever the stack
     * happened to hold — which UBSan reported as a load of 192 into a `_Bool`
     * once A9.2's schema bump changed the frame layout. A struct built field by
     * field silently stops being complete the moment somebody adds a field, so
     * these sites zero first and assign after. */
    atlas_migration list[64];
    T_REQUIRE(base_count + 1u <= sizeof(list) / sizeof(list[0]));
    memset(list, 0, sizeof(list));
    memcpy(list, base, base_count * sizeof(list[0]));
    list[base_count].version = (int)base_count + 1;
    list[base_count].name = "deliberately broken";
    list[base_count].statements = bad_statements;
    list[base_count].foreign_keys_off = false;

    T_FAILS_WITH(atlas_db_migrate_list(e.db, list, base_count + 1u, &err), ATLAS_ERR_DB, &err);
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "rolled back") != NULL,
                "the error should say the migration was rolled back, got: %s",
                atlas_err_msg(&err));

    /* Nothing from the failed migration is left behind. */
    T_CHECK_MSG(!table_exists(e.db, "atlas_rollback_probe"),
                "the first statement of the failed migration was not rolled back");
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM schema_migrations;"), (long long)base_count);

    /* The database is still usable afterwards. */
    int64_t id = 0;
    T_OK(add_repo(e.db, "after-failure", "/tmp/x", "/tmp/x/.git", "/tmp/x/.git", false,
                  "sha1", &id, &err),
         &err);
    T_CHECK(id > 0);

    db_env_close(&e);
}

static void test_migration_out_of_sequence(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    /* A gap in the numbering is refused rather than applied out of order. */
    static const char *const stmts[] = {"CREATE TABLE gap_probe(id INTEGER);", NULL};
    atlas_migration list[1];
    memset(list, 0, sizeof(list));
    list[0].version = ATLAS_SCHEMA_VERSION + 5;
    list[0].name = "far future";
    list[0].statements = stmts;
    list[0].foreign_keys_off = false;

    T_FAILS_WITH(atlas_db_migrate_list(e.db, list, 1u, &err), ATLAS_ERR_DB, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "out of sequence") != NULL);
    T_CHECK(!table_exists(e.db, "gap_probe"));
    db_env_close(&e);
}

/* --- transaction observation ---------------------------------------------- */

static void test_in_transaction(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, false);
    atlas_err err;
    atlas_err_init(&err);

    /* A NULL handle answers no rather than crashing. */
    T_CHECK(!atlas_db_in_transaction(NULL));

    /* A freshly opened handle is not in a transaction. */
    T_CHECK(!atlas_db_in_transaction(e.db));

    /* Inside atlas_db_begin the answer is yes. */
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_CHECK(atlas_db_in_transaction(e.db));

    /* And no again once the commit balances it. */
    T_OK(atlas_db_commit(e.db, &err), &err);
    T_CHECK(!atlas_db_in_transaction(e.db));

    /* A rollback ends the transaction the same way. */
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_CHECK(atlas_db_in_transaction(e.db));
    atlas_db_rollback(e.db);
    T_CHECK(!atlas_db_in_transaction(e.db));

    /* A BEGIN issued behind atlas_db_begin's back leaves tx_depth at zero, so
     * this is the sqlite3_get_autocommit half of the check answering. A failed
     * raw statement would make every assertion after it noise, hence REQUIRE. */
    T_REQUIRE(sqlite3_exec(e.db->h, "BEGIN;", NULL, NULL, NULL) == SQLITE_OK);
    T_CHECK(atlas_db_in_transaction(e.db));
    T_REQUIRE(sqlite3_exec(e.db->h, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK);
    T_CHECK(!atlas_db_in_transaction(e.db));

    /* Nested begins stay open until the outermost commit balances them. */
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_db_begin(e.db, &err), &err);
    T_OK(atlas_db_commit(e.db, &err), &err);
    T_CHECK(atlas_db_in_transaction(e.db));
    T_OK(atlas_db_commit(e.db, &err), &err);
    T_CHECK(!atlas_db_in_transaction(e.db));

    db_env_close(&e);
}

/* --- capabilities and checks -------------------------------------------- */

static void test_caps_and_checks(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    const atlas_db_caps *caps = atlas_db_caps_of(e.db);
    T_CHECK_MSG(caps->foreign_keys, "foreign keys must be enforced");
    T_CHECK(caps->sqlite_version[0] != '\0');
    T_CHECK(caps->journal_mode[0] != '\0');

    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(atlas_db_integrity_check(e.db, &out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "ok");
    T_OK(atlas_db_foreign_key_check(e.db, &out, &err), &err);
    T_EQ_STR(atlas_buf_cstr(&out), "ok");
    atlas_buf_free(&out);

    /* Foreign keys really do cascade: removing a repository removes its rows. */
    int64_t id = 0;
    T_OK(add_repo(e.db, "cascade", "/tmp/c", "/tmp/c/.git", "/tmp/c/.git", false, "sha1", &id,
                  &err),
         &err);
    atlas_scan_state stt;
    memset(&stt, 0, sizeof(stt));
    stt.head_oid = "";
    stt.head_state = "unborn";
    stt.branch = "main";
    stt.object_format = "sha1";
    int64_t scan_id = 0;
    T_OK(atlas_db_scan_begin(e.db, id, &stt, &scan_id, &err), &err);

    atlas_file_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.path_raw = "a.txt";
    rec.path_raw_len = 5u;
    rec.path_text = "a.txt";
    rec.path_is_utf8 = true;
    rec.file_type = "regular";
    rec.content_hash = "deadbeef";
    rec.content_hash_algo = "sha256";
    T_OK(atlas_db_file_upsert(e.db, id, scan_id, &rec, NULL, &err), &err);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM files;"), 1);

    bool removed = false;
    T_OK(atlas_db_repo_remove(e.db, "cascade", &removed, &err), &err);
    T_CHECK(removed);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM files;"), 0);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM scans;"), 0);

    db_env_close(&e);
}

/* --- repository records -------------------------------------------------- */

static void test_repo_name_validation(void) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_db_check_repo_name("dna", &err), &err);
    T_OK(atlas_db_check_repo_name("my-repo_2.0", &err), &err);

    T_FAILS_WITH(atlas_db_check_repo_name("", &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_db_check_repo_name("has space", &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_db_check_repo_name("slash/name", &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_db_check_repo_name("-leading-dash", &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_db_check_repo_name(".leading-dot", &err), ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(atlas_db_check_repo_name("semi;colon", &err), ATLAS_ERR_USAGE, &err);

    char toolong[ATLAS_NAME_MAX + 8u];
    memset(toolong, 'a', sizeof(toolong) - 1u);
    toolong[sizeof(toolong) - 1u] = '\0';
    T_FAILS_WITH(atlas_db_check_repo_name(toolong, &err), ATLAS_ERR_USAGE, &err);
}

typedef struct repo_names {
    atlas_buf joined;
    int count;
} repo_names;

static atlas_status collect_repo(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    repo_names *rn = (repo_names *)ud;
    rn->count++;
    atlas_status st = atlas_buf_append_str(&rn->joined, ri->name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&rn->joined, ';', err);
    }
    return st;
}

static void test_repo_crud(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    int64_t id_b = 0;
    T_OK(add_repo(e.db, "beta", "/srv/beta", "/srv/beta/.git", "/srv/beta/.git", false,
                  "sha1", &id_b, &err),
         &err);
    int64_t id_a = 0;
    T_OK(add_repo(e.db, "alpha", "/srv/alpha", "/srv/alpha/.git", "/srv/alpha/.git", false,
                  "sha256", &id_a, &err),
         &err);
    T_CHECK(id_a != id_b);

    /* A duplicate name and a duplicate root are both refused, with different
     * messages so the user knows which. */
    int64_t dup = 0;
    T_FAILS_WITH(add_repo(e.db, "alpha", "/srv/other", "", "", false, "sha1", &dup, &err),
                 ATLAS_ERR_REPO, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "already in use") != NULL);
    T_FAILS_WITH(add_repo(e.db, "gamma", "/srv/alpha", "", "", false, "sha1", &dup, &err),
                 ATLAS_ERR_REPO, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "already registered") != NULL);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(e.db, "alpha", &info, &found, &err), &err);
    T_CHECK(found);
    T_EQ_STR(info.name, "alpha");
    T_EQ_STR(atlas_buf_cstr(&info.root_path), "/srv/alpha");
    T_EQ_STR(info.object_format, "sha256");
    T_EQ_STR(info.head_state, "unknown");
    T_EQ_INT(info.last_scan_id, 0);
    T_CHECK(info.registered_at[0] == '2'); /* ISO-8601 */
    atlas_repo_info_free(&info);

    /* Lookup by canonical root is how `scan` detects a stale registration. */
    atlas_repo_info by_root;
    atlas_repo_info_init(&by_root);
    found = false;
    T_OK(atlas_db_repo_get_by_root(e.db, "/srv/beta", 9u, &by_root, &found, &err), &err);
    T_CHECK(found);
    T_EQ_STR(by_root.name, "beta");
    atlas_repo_info_free(&by_root);

    atlas_repo_info missing;
    atlas_repo_info_init(&missing);
    found = true;
    T_OK(atlas_db_repo_get(e.db, "nope", &missing, &found, &err), &err);
    T_CHECK(!found);
    atlas_repo_info_free(&missing);

    /* Listing is ordered by name so output is stable. */
    repo_names rn;
    memset(&rn, 0, sizeof(rn));
    atlas_buf_init(&rn.joined);
    T_OK(atlas_db_repo_list(e.db, collect_repo, &rn, &err), &err);
    T_EQ_INT(rn.count, 2);
    T_EQ_STR(atlas_buf_cstr(&rn.joined), "alpha;beta;");
    atlas_buf_free(&rn.joined);

    bool removed = false;
    T_OK(atlas_db_repo_remove(e.db, "beta", &removed, &err), &err);
    T_CHECK(removed);
    removed = true;
    T_OK(atlas_db_repo_remove(e.db, "beta", &removed, &err), &err);
    T_CHECK(!removed);

    db_env_close(&e);
}

/* --- file upsert transitions -------------------------------------------- */

static void test_file_upsert_transitions(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    int64_t repo = 0;
    T_OK(add_repo(e.db, "r", "/srv/r", "", "", false, "sha1", &repo, &err), &err);
    atlas_scan_state stt;
    memset(&stt, 0, sizeof(stt));
    stt.head_oid = "";
    stt.head_state = "born";
    stt.branch = "main";
    stt.object_format = "sha1";

    int64_t scan1 = 0;
    T_OK(atlas_db_scan_begin(e.db, repo, &stt, &scan1, &err), &err);

    atlas_file_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.path_raw = "a.txt";
    rec.path_raw_len = 5u;
    rec.path_text = "a.txt";
    rec.path_is_utf8 = true;
    rec.file_type = "regular";
    rec.git_mode = "100644";
    rec.git_index_oid = "aaaa";
    rec.content_hash = "hash1";
    rec.content_hash_algo = "sha256";
    rec.size_bytes = 10;
    rec.size_known = true;

    atlas_upsert_kind kind = ATLAS_UPSERT_UNCHANGED;
    T_OK(atlas_db_file_upsert(e.db, repo, scan1, &rec, &kind, &err), &err);
    T_EQ_INT(kind, ATLAS_UPSERT_ADDED);

    /* Same content in a later scan is unchanged, even though the scan id and
     * timestamps differ. */
    int64_t scan2 = 0;
    T_OK(atlas_db_scan_begin(e.db, repo, &stt, &scan2, &err), &err);
    T_OK(atlas_db_file_upsert(e.db, repo, scan2, &rec, &kind, &err), &err);
    T_EQ_INT(kind, ATLAS_UPSERT_UNCHANGED);

    /* A different content hash is a modification. */
    rec.content_hash = "hash2";
    T_OK(atlas_db_file_upsert(e.db, repo, scan2, &rec, &kind, &err), &err);
    T_EQ_INT(kind, ATLAS_UPSERT_MODIFIED);

    /* So is a mode change with identical content. */
    rec.git_mode = "100755";
    rec.is_executable = true;
    T_OK(atlas_db_file_upsert(e.db, repo, scan2, &rec, &kind, &err), &err);
    T_EQ_INT(kind, ATLAS_UPSERT_MODIFIED);

    /* A path absent from the newest scan is marked deleted, not removed. */
    int64_t scan3 = 0;
    T_OK(atlas_db_scan_begin(e.db, repo, &stt, &scan3, &err), &err);
    int64_t deleted = 0;
    T_OK(atlas_db_files_mark_deleted(e.db, repo, scan3, &deleted, &err), &err);
    T_EQ_INT(deleted, 1);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM files WHERE deleted=1;"), 1);

    /* Marking twice does not double-count. */
    int64_t again = 0;
    T_OK(atlas_db_files_mark_deleted(e.db, repo, scan3, &again, &err), &err);
    T_EQ_INT(again, 0);

    /* A deleted path that comes back counts as an addition and is undeleted. */
    int64_t scan4 = 0;
    T_OK(atlas_db_scan_begin(e.db, repo, &stt, &scan4, &err), &err);
    T_OK(atlas_db_file_upsert(e.db, repo, scan4, &rec, &kind, &err), &err);
    T_EQ_INT(kind, ATLAS_UPSERT_ADDED);
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM files WHERE deleted=1;"), 0);
    /* Still one row: history is preserved rather than duplicated. */
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM files;"), 1);

    db_env_close(&e);
}

/* --- evidence ----------------------------------------------------------- */

static void test_evidence_kinds(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    int64_t repo = 0;
    T_OK(add_repo(e.db, "r", "/srv/r", "", "", false, "sha1", &repo, &err), &err);

    T_OK(atlas_db_evidence_insert(e.db, repo, ATLAS_EV_SOURCE, 1, "abc", "a.txt", 5u, "a.txt", NULL,
                                  "tracked file", &err),
         &err);
    T_OK(atlas_db_evidence_insert(e.db, repo, ATLAS_EV_GIT, 1, "def", NULL, 0, NULL, "def",
                                  "commit", &err),
         &err);

    /* A0 must not be able to record an inferred or claimed reason, even by
     * accident: the restriction is enforced in code, not only by convention. */
    const atlas_evidence_kind forbidden[] = {ATLAS_EV_DECISION, ATLAS_EV_USER_STATEMENT,
                                             ATLAS_EV_INFERENCE, ATLAS_EV_UNKNOWN};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        T_FAILS_WITH(atlas_db_evidence_insert(e.db, repo, forbidden[i], 1, NULL, NULL, 0, NULL,
                                              NULL, "should not be stored", &err),
                     ATLAS_ERR_INTEGRITY, &err);
    }
    T_EQ_INT(count_sql(e.db, "SELECT count(*) FROM evidence;"), 2);
    T_EQ_INT(count_sql(e.db,
                       "SELECT count(*) FROM evidence WHERE kind NOT IN ('SOURCE','GIT');"),
             0);

    T_EQ_STR(atlas_evidence_kind_name(ATLAS_EV_SOURCE), "SOURCE");
    T_EQ_STR(atlas_evidence_kind_name(ATLAS_EV_GIT), "GIT");
    T_EQ_STR(atlas_evidence_kind_name(ATLAS_EV_DECISION), "DECISION");
    T_EQ_STR(atlas_evidence_kind_name(ATLAS_EV_USER_STATEMENT), "USER_STATEMENT");
    T_EQ_STR(atlas_evidence_kind_name(ATLAS_EV_INFERENCE), "INFERENCE");
    T_EQ_STR(atlas_evidence_kind_name(ATLAS_EV_UNKNOWN), "UNKNOWN");

    db_env_close(&e);
}

static void test_binary_paths_round_trip(void) {
    db_env e;
    memset(&e, 0, sizeof(e));
    db_env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);

    int64_t repo = 0;
    T_OK(add_repo(e.db, "r", "/srv/r", "", "", false, "sha1", &repo, &err), &err);
    atlas_scan_state stt;
    memset(&stt, 0, sizeof(stt));
    stt.head_oid = "";
    stt.head_state = "born";
    stt.branch = "main";
    stt.object_format = "sha1";
    int64_t scan = 0;
    T_OK(atlas_db_scan_begin(e.db, repo, &stt, &scan, &err), &err);

    /* Paths are stored as BLOBs, so bytes that are not valid UTF-8 and bytes that
     * look like NUL-adjacent noise survive a round trip exactly. */
    static const char weird[] = {'b', 'a', 'd', (char)0xff, '\t', '\n', 'x'};
    atlas_buf text = ATLAS_BUF_INIT;
    T_OK(atlas_path_text_encode(weird, sizeof(weird), &text, &err), &err);

    atlas_file_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.path_raw = weird;
    rec.path_raw_len = sizeof(weird);
    rec.path_text = atlas_buf_cstr(&text);
    rec.path_is_utf8 = false;
    rec.file_type = "regular";
    T_OK(atlas_db_file_upsert(e.db, repo, scan, &rec, NULL, &err), &err);

    bool found = false;
    T_OK(atlas_db_file_get(e.db, repo, weird, sizeof(weird), NULL, NULL, &found, &err), &err);
    T_CHECK_MSG(found, "a path with non-UTF-8 bytes could not be found by its exact bytes");

    /* A near-miss must not match. */
    static const char other[] = {'b', 'a', 'd', (char)0xfe, '\t', '\n', 'x'};
    found = true;
    T_OK(atlas_db_file_get(e.db, repo, other, sizeof(other), NULL, NULL, &found, &err), &err);
    T_CHECK(!found);

    atlas_buf_free(&text);
    db_env_close(&e);
}

static const atlas_test TESTS[] = {
    {"migrations are idempotent", test_migration_idempotency},
    {"a failed migration is rolled back whole", test_migration_rollback},
    {"out-of-sequence migrations are refused", test_migration_out_of_sequence},
    {"transaction state tracks begin, commit, rollback and a raw BEGIN", test_in_transaction},
    {"capabilities, checks and cascade", test_caps_and_checks},
    {"repository name validation", test_repo_name_validation},
    {"repository create, read, list, remove", test_repo_crud},
    {"file upsert transitions", test_file_upsert_transitions},
    {"only SOURCE and GIT evidence can be written", test_evidence_kinds},
    {"binary paths round-trip through the index", test_binary_paths_round_trip},
};

ATLAS_TEST_MAIN("db", TESTS)

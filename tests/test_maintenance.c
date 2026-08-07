/* Atlas - `atlas maintenance plan|prune`.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The classification is the product, so the first test is the one that keeps it
 * honest: every table in the live schema has a policy row and every policy row
 * names a live table, checked in both directions. A table added without a
 * classification would otherwise default into "nobody looked", which is exactly
 * the state this file exists to prevent.
 *
 * The rest is the conservatism: a plan writes nothing, a prune without --apply
 * is refused rather than quietly treated as a plan, protected tables come
 * through an applied prune byte for byte, and a plan's count is compared with
 * what an apply actually removes rather than trusting two SQL texts to agree.
 */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "atlas/atlas.h"
#include "atlas/maintenance.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- helpers ------------------------------------------------------------- */

static atlas_status open_at(const char *data_dir, atlas_db **out, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_status st = atlas_datadir_ensure(data_dir, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(data_dir, &path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_open(atlas_buf_cstr(&path), out, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_migrate(*out, err);
    }
    atlas_buf_free(&path);
    return st;
}

static void table_digest(atlas_db *db, const char *table, const char *order, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    char sql[256];
    (void)snprintf(sql, sizeof sql, "SELECT * FROM %s ORDER BY %s;", table, order);
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
                const void *b = sqlite3_column_blob(s, c);
                int n = sqlite3_column_bytes(s, c);
                atlas_sha256_update(&ctx, b != NULL ? b : "", n > 0 ? (size_t)n : 0u);
            }
            atlas_sha256_update(&ctx, "\x1f", 1u);
        }
        atlas_sha256_update(&ctx, "\x1e", 1u);
    }
    atlas_db_finish(db, s);
    unsigned char d[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, d);
    atlas_hex_encode(d, sizeof d, out);
}

/* One repository, and `n` events whose timestamps straddle the cutoff: the
 * first `old` are dated well in the past, the rest today. */
static void seed_events(atlas_db *db, int64_t old, int64_t fresh, atlas_err *err) {
    static const char REPO[] =
        "INSERT INTO repositories(id, name, root_path, root_path_text, git_common_dir,"
        " git_common_dir_text, object_format, registered_at, head_state)"
        " VALUES(1,'proj',X'2F746D702F70','/tmp/p',X'2F702F67','/p/g','sha1',"
        "'2026-01-01T00:00:00Z','born');"
        "INSERT INTO repo_index_state(repo_id, generation, last_complete_generation, last_sync_seq)"
        " VALUES(1,4,4,17);"
        "INSERT INTO ai_clients(id, provider, name, first_seen_at, last_seen_at)"
        " VALUES(1,'anthropic','claude-code','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "INSERT INTO ai_sessions(id, client_id, session_key, started_at, last_seen_at)"
        " VALUES(1,1,'sess-a','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "INSERT INTO ai_reasons(id, session_id, repo_id, created_at, provenance, state, summary)"
        " VALUES(1,1,1,'2020-01-01T00:00:00Z','MODEL_PROPOSAL','proposed','an old reason');"
        "INSERT INTO evidence(id, repo_id, kind, created_at)"
        " VALUES(1,1,'SOURCE','2020-01-01T00:00:00Z');"
        "INSERT INTO commits(id, repo_id, oid, subject, commit_time)"
        " VALUES(1,1,'abc123','an old commit',1577836800);";
    T_OK(atlas_db_exec_sql(db, REPO, err), err);

    T_OK(atlas_db_begin(db, err), err);
    for (int64_t i = 0; i < old + fresh; i++) {
        char sql[256];
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO repo_events(repo_id, kind, created_at)"
                       " VALUES(1,'reconciled','%s');",
                       i < old ? "2020-01-01T00:00:00Z" : "2099-01-01T00:00:00Z");
        T_OK(atlas_db_exec_sql(db, sql, err), err);
    }
    T_OK(atlas_db_commit(db, err), err);
}

static void plan_or_prune(const char *data_dir, int64_t days, int64_t retain, bool apply,
                          atlas_maintenance_report *out, atlas_err *err, atlas_status expect) {
    atlas_maintenance_opts o;
    memset(&o, 0, sizeof o);
    o.older_than_days = days;
    o.retain_per_repo = retain;
    o.apply = apply;
    atlas_status st = atlas_service_maintenance(data_dir, &o, out, err);
    T_CHECK_MSG(st == expect, "maintenance returned %d, expected %d (%s)", (int)st, (int)expect,
                atlas_err_msg(err));
}

static const atlas_maintenance_row *row_for(const atlas_maintenance_report *r, const char *table) {
    for (size_t i = 0; i < r->table_count; i++) {
        if (strcmp(r->tables[i].table, table) == 0) {
            return &r->tables[i];
        }
    }
    return NULL;
}

/* --- tests --------------------------------------------------------------- */

/* Both directions. A new table with no classification is the failure this
 * catches; a classification for a table that no longer exists is the other. */
static void test_every_table_has_a_classification_and_every_classification_a_table(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);

    const char *const *policy = NULL;
    size_t policy_count = atlas_maintenance_policy(&policy);
    T_REQUIRE(policy_count > 0);

    /* Every classified table exists. */
    for (size_t i = 0; i < policy_count; i++) {
        bool exists = false;
        T_OK(atlas_db_maintenance_table_exists(db, policy[i], &exists, &err), &err);
        T_CHECK_MSG(exists, "the retention policy classifies \"%s\", which is not in the schema",
                    policy[i]);
    }

    /* Every table in the schema is classified. FTS5's own shadow tables are
     * excluded by name: they are storage belonging to a virtual table that is
     * itself classified, not tables anybody could have a retention opinion
     * about. `sqlite_*` is SQLite's. */
    static const char *SHADOW_SUFFIX[] = {"_data", "_idx", "_docsize", "_config", "_content"};
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db, "SELECT name FROM sqlite_schema WHERE type='table' ORDER BY name;",
                          &s, &err),
         &err);
    int64_t seen = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(s, 0);
        if (name == NULL || strncmp(name, "sqlite_", 7) == 0) {
            continue;
        }
        bool shadow = false;
        for (size_t k = 0; k < sizeof SHADOW_SUFFIX / sizeof SHADOW_SUFFIX[0]; k++) {
            size_t nl = strlen(name), sl = strlen(SHADOW_SUFFIX[k]);
            if (nl > sl && strcmp(name + nl - sl, SHADOW_SUFFIX[k]) == 0 &&
                strstr(name, "_fts") != NULL) {
                shadow = true;
            }
        }
        if (shadow) {
            continue;
        }
        bool classified = false;
        for (size_t i = 0; i < policy_count; i++) {
            if (strcmp(policy[i], name) == 0) {
                classified = true;
            }
        }
        T_CHECK_MSG(classified,
                    "the table \"%s\" has no entry in RETENTION[]; classify it before it is "
                    "shipped, even if the answer is \"never pruned\"",
                    name);
        seen++;
    }
    atlas_db_finish(db, s);
    T_CHECK(seen > 40);
    atlas_db_close(db);
    fx_close(&fx);
}

/* Exactly one table is prunable in A5, and it is the one with a documented
 * ceiling already. If a later phase makes a second table prunable, this fails
 * and somebody has to say so out loud. */
static void test_exactly_one_table_is_prunable(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    atlas_db_close(db);

    atlas_maintenance_report rep;
    atlas_maintenance_report_init(&rep);
    plan_or_prune(fx_data_dir(&fx), 30, 0, false, &rep, &err, ATLAS_OK);
    T_EQ_INT((long long)rep.prunable_tables, 1);
    const atlas_maintenance_row *ev = row_for(&rep, "repo_events");
    T_REQUIRE(ev != NULL);
    T_CHECK(ev->prunable);
    for (size_t i = 0; i < rep.table_count; i++) {
        if (strcmp(rep.tables[i].table, "repo_events") != 0) {
            T_CHECK_MSG(!rep.tables[i].prunable, "\"%s\" became prunable without a note",
                        rep.tables[i].table);
        }
        T_CHECK_MSG(rep.tables[i].reason != NULL && rep.tables[i].reason[0] != '\0',
                    "\"%s\" is classified without a reason", rep.tables[i].table);
    }
    atlas_maintenance_report_free(&rep);
    fx_close(&fx);
}

/* A plan does not write a byte. Compared against the whole file, not against
 * the rows: a plan that opened the database read-write would checkpoint on
 * close and change it without changing a row. */
static void test_a_plan_writes_nothing(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 5000, 100, &err);
    atlas_db_close(db);

    atlas_buf p = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(fx_data_dir(&fx), &p, &err), &err);
    struct stat before, after;
    T_REQUIRE(stat(atlas_buf_cstr(&p), &before) == 0);

    atlas_maintenance_report rep;
    atlas_maintenance_report_init(&rep);
    plan_or_prune(fx_data_dir(&fx), 30, 1000, false, &rep, &err, ATLAS_OK);
    T_CHECK_MSG(rep.total_eligible > 0, "the fixture produced nothing eligible");
    T_EQ_INT(rep.total_removed, 0);
    T_CHECK(!rep.applied);
    atlas_maintenance_report_free(&rep);

    T_REQUIRE(stat(atlas_buf_cstr(&p), &after) == 0);
    T_CHECK_MSG(before.st_size == after.st_size && before.st_mtime == after.st_mtime,
                "a maintenance plan modified the database file");

    /* And the rows are still there. */
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    int64_t n = 0;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repo_events;", &n, &err), &err);
    T_EQ_INT(n, 5100);
    atlas_db_close(db);
    atlas_buf_free(&p);
    fx_close(&fx);
}

/* What a plan says is eligible is exactly what an apply removes. Two SQL texts
 * describe that set, and this is what keeps them the same set. */
static void test_a_plan_predicts_what_an_apply_removes(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 5000, 100, &err);
    atlas_db_close(db);

    atlas_maintenance_report planned;
    atlas_maintenance_report_init(&planned);
    plan_or_prune(fx_data_dir(&fx), 30, 1000, false, &planned, &err, ATLAS_OK);
    int64_t expected = planned.total_eligible;
    T_CHECK_MSG(expected == 4100, "expected 4100 eligible (5100 rows, 1000 kept), got %lld",
                (long long)expected);
    atlas_maintenance_report_free(&planned);

    atlas_maintenance_report applied;
    atlas_maintenance_report_init(&applied);
    plan_or_prune(fx_data_dir(&fx), 30, 1000, true, &applied, &err, ATLAS_OK);
    T_EQ_INT(applied.total_removed, expected);
    const atlas_maintenance_row *ev = row_for(&applied, "repo_events");
    T_REQUIRE(ev != NULL);
    T_EQ_INT(ev->rows_after, 1000);
    atlas_maintenance_report_free(&applied);

    /* Idempotent: a second run finds nothing left to do. */
    atlas_maintenance_report again;
    atlas_maintenance_report_init(&again);
    plan_or_prune(fx_data_dir(&fx), 30, 1000, true, &again, &err, ATLAS_OK);
    T_EQ_INT(again.total_removed, 0);
    atlas_maintenance_report_free(&again);
    fx_close(&fx);
}

/* Everything the policy protects is byte-identical across an applied prune, and
 * the cursors an operator would lose sleep over still read the same. */
static void test_protected_tables_and_cursors_survive_an_applied_prune(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 5000, 100, &err);

    const char *const *policy = NULL;
    size_t policy_count = atlas_maintenance_policy(&policy);
    char *before = calloc(policy_count, ATLAS_SHA256_HEX_LEN + 1u);
    T_REQUIRE(before != NULL);
    for (size_t i = 0; i < policy_count; i++) {
        if (strcmp(policy[i], "repo_events") == 0) {
            continue;
        }
        table_digest(db, policy[i], "rowid", before + i * (ATLAS_SHA256_HEX_LEN + 1u));
    }
    int64_t cursor_before = 0, session_before = 0;
    T_OK(atlas_db_query_int64(db, "SELECT last_sync_seq FROM repo_index_state WHERE repo_id=1;",
                              &cursor_before, &err),
         &err);
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM ai_sessions;", &session_before, &err),
         &err);
    atlas_db_close(db);

    atlas_maintenance_report rep;
    atlas_maintenance_report_init(&rep);
    plan_or_prune(fx_data_dir(&fx), 30, 1000, true, &rep, &err, ATLAS_OK);
    T_CHECK(rep.total_removed > 0);
    atlas_maintenance_report_free(&rep);

    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    for (size_t i = 0; i < policy_count; i++) {
        if (strcmp(policy[i], "repo_events") == 0) {
            continue;
        }
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        table_digest(db, policy[i], "rowid", after);
        T_CHECK_MSG(strcmp(before + i * (ATLAS_SHA256_HEX_LEN + 1u), after) == 0,
                    "the protected table \"%s\" changed during a prune", policy[i]);
    }
    int64_t cursor_after = 0, session_after = 0;
    T_OK(atlas_db_query_int64(db, "SELECT last_sync_seq FROM repo_index_state WHERE repo_id=1;",
                              &cursor_after, &err),
         &err);
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM ai_sessions;", &session_after, &err), &err);
    T_EQ_INT(cursor_after, cursor_before);
    T_EQ_INT(session_after, session_before);

    /* The surviving events are the newest ones, so a consumer holding a recent
     * cursor still resumes. */
    int64_t max_id = 0, min_id = 0;
    T_OK(atlas_db_query_int64(db, "SELECT max(id) FROM repo_events;", &max_id, &err), &err);
    T_OK(atlas_db_query_int64(db, "SELECT min(id) FROM repo_events;", &min_id, &err), &err);
    T_EQ_INT(max_id - min_id, 999);
    T_OK(atlas_db_query_int64(db,
                              "SELECT count(*) FROM repo_events WHERE created_at"
                              " < '2099-01-01T00:00:00Z';",
                              &max_id, &err),
         &err);
    T_EQ_INT(max_id, 900); /* 1000 kept, of which 100 are the fresh ones */
    atlas_db_close(db);
    free(before);
    fx_close(&fx);
}

/* A fresh repository is never emptied by age alone: the retain floor is a
 * floor, and a repository with fewer than `retain` events contributes nothing
 * however old they are. */
static void test_the_retain_floor_protects_a_quiet_repository(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 50, 0, &err); /* all ancient, all below the floor */
    atlas_db_close(db);

    atlas_maintenance_report rep;
    atlas_maintenance_report_init(&rep);
    plan_or_prune(fx_data_dir(&fx), 1, 1000, true, &rep, &err, ATLAS_OK);
    T_EQ_INT(rep.total_eligible, 0);
    T_EQ_INT(rep.total_removed, 0);
    atlas_maintenance_report_free(&rep);

    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    int64_t n = 0;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repo_events;", &n, &err), &err);
    T_EQ_INT(n, 50);
    atlas_db_close(db);
    fx_close(&fx);
}

static void test_invalid_bounds_are_refused_rather_than_clamped(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 5000, 0, &err);
    atlas_db_close(db);

    /* Negative, and past a century. Zero means "unset" and takes the default,
     * which is stated in the report rather than inferred. */
    const int64_t BAD_DAYS[] = {-1, ATLAS_MAINTENANCE_MAX_DAYS + 1, 1000000};
    for (size_t i = 0; i < sizeof BAD_DAYS / sizeof BAD_DAYS[0]; i++) {
        atlas_maintenance_report rep;
        atlas_maintenance_report_init(&rep);
        atlas_err e2;
        atlas_err_init(&e2);
        plan_or_prune(fx_data_dir(&fx), BAD_DAYS[i], 0, false, &rep, &e2, ATLAS_ERR_USAGE);
        atlas_maintenance_report_free(&rep);
    }
    {
        atlas_maintenance_report rep;
        atlas_maintenance_report_init(&rep);
        atlas_err e2;
        atlas_err_init(&e2);
        plan_or_prune(fx_data_dir(&fx), 30, 1, false, &rep, &e2, ATLAS_ERR_USAGE);
        atlas_maintenance_report_free(&rep);
    }

    /* Nothing was removed by any of the refusals. */
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    int64_t n = 0;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repo_events;", &n, &err), &err);
    T_EQ_INT(n, 5000);
    atlas_db_close(db);
    fx_close(&fx);
}

/* `maintenance prune` without `--apply` is a usage error, not a silent plan.
 * Driven through the binary, because this is a CLI guarantee. */
static void test_prune_without_apply_is_refused_at_the_command_line(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 5000, 0, &err);
    atlas_db_close(db);

    const char *dd = fx_data_dir(&fx);
    const char *args[] = {"maintenance", "prune", "--older-than", "30", "--data-dir", dd};
    atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(args, 6, &so, &se, &code, &err), &err);
    T_EQ_INT(code, ATLAS_ERR_USAGE);
    T_CHECK(strstr(atlas_buf_cstr(&se), "--apply") != NULL);

    /* And `plan --apply` is refused too: the two words must not become
     * interchangeable. */
    const char *args2[] = { "maintenance", "plan", "--apply",
                           "--data-dir", dd};
    atlas_buf so2 = ATLAS_BUF_INIT, se2 = ATLAS_BUF_INIT;
    T_OK(fx_atlas(args2, 5, &so2, &se2, &code, &err), &err);
    T_EQ_INT(code, ATLAS_ERR_USAGE);

    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    int64_t n = 0;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repo_events;", &n, &err), &err);
    T_EQ_INT(n, 5000);
    atlas_db_close(db);

    atlas_buf_free(&so);
    atlas_buf_free(&se);
    atlas_buf_free(&so2);
    atlas_buf_free(&se2);
    fx_close(&fx);
}

/* Human and JSON describe the same run. Both renderers are driven from the same
 * report, and this is what proves the second one exists. */
static void test_both_renderers_describe_the_same_plan(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed_events(db, 5000, 100, &err);
    atlas_db_close(db);

    const char *dd = fx_data_dir(&fx);
    const char *human[] = {"maintenance", "plan", "--older-than", "30",
                           "--retain", "1000", "--data-dir", dd};
    atlas_buf ho = ATLAS_BUF_INIT, he = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(human, 8, &ho, &he, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&ho), "repo_events") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&ho), "nothing was written") != NULL);

    const char *json[] = {"maintenance", "plan", "--older-than", "30",
                          "--retain", "1000", "--data-dir", dd, "--json"};
    atlas_buf jo = ATLAS_BUF_INIT, je = ATLAS_BUF_INIT;
    T_OK(fx_atlas(json, 9, &jo, &je, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&jo), "\"total_eligible\":4100") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&jo), "\"applied\":false") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&jo), "\"retention_class\":\"canonical\"") != NULL);

    atlas_buf_free(&ho);
    atlas_buf_free(&he);
    atlas_buf_free(&jo);
    atlas_buf_free(&je);
    fx_close(&fx);
}

static void test_maintenance_is_absent_from_the_ai_surface(void) {
    static const char *FORBIDDEN[] = {"maintenance", "prune", "retention", "delete"};
    const char *const *names = atlas_mcp_tool_names();
    T_REQUIRE(names != NULL);
    for (size_t i = 0; names[i] != NULL; i++) {
        for (size_t f = 0; f < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; f++) {
            T_CHECK_MSG(strstr(names[i], FORBIDDEN[f]) == NULL,
                        "MCP tool \"%s\" contains \"%s\"", names[i], FORBIDDEN[f]);
        }
    }
}

static const atlas_test TESTS[] = {
    {"every table has a classification and every classification a table",
     test_every_table_has_a_classification_and_every_classification_a_table},
    {"exactly one table is prunable", test_exactly_one_table_is_prunable},
    {"a plan writes nothing", test_a_plan_writes_nothing},
    {"a plan predicts what an apply removes", test_a_plan_predicts_what_an_apply_removes},
    {"protected tables and cursors survive an applied prune",
     test_protected_tables_and_cursors_survive_an_applied_prune},
    {"the retain floor protects a quiet repository",
     test_the_retain_floor_protects_a_quiet_repository},
    {"invalid bounds are refused rather than clamped",
     test_invalid_bounds_are_refused_rather_than_clamped},
    {"prune without --apply is refused at the command line",
     test_prune_without_apply_is_refused_at_the_command_line},
    {"both renderers describe the same plan", test_both_renderers_describe_the_same_plan},
    {"maintenance is absent from the AI surface",
     test_maintenance_is_absent_from_the_ai_surface},
};

ATLAS_TEST_MAIN("maintenance", TESTS)

/* Atlas - `atlas backup create|verify|restore`.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Backup is one of the few subsystems whose whole value is in what happens when
 * something goes wrong, so most of this file is failure paths. The properties
 * being held are:
 *
 *   a restored index is the same index      compared table by table with the
 *                                           column-name-and-value digest
 *                                           test_migrate6.c uses, not by row
 *                                           counts, which survive a rewrite.
 *
 *   a damaged backup is refused             a flipped byte, a truncation at any
 *                                           boundary, a database Atlas did not
 *                                           write, and a schema from a newer
 *                                           Atlas each get their own verdict.
 *
 *   a failed restore changes nothing        the original database is compared
 *                                           byte for byte after every injected
 *                                           failure, at every fault point.
 *
 *   verification is genuinely read-only     run against a fresh HOME, and the
 *                                           whole HOME is digested before and
 *                                           after.
 *
 *   the AI surface cannot reach any of it   asserted against the tool inventory
 *                                           the process actually reports.
 */
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/decision.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- helpers ------------------------------------------------------------- */

static void path_in(const fixture *fx, const char *rel, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(out);
    T_OK(atlas_buf_appendf(out, &err, "%s/%s", atlas_buf_cstr(&fx->root), rel), &err);
}

static void db_path_of(const char *data_dir, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(out);
    T_OK(atlas_buf_appendf(out, &err, "%s/" ATLAS_DB_FILENAME, data_dir), &err);
}

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

/* The digest of one table, columns and values, in a deterministic order. The
 * same shape test_migrate6.c uses and for the same reason: a count survives a
 * migration or a copy that rewrote a value, and this does not. */
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
    atlas_hex_encode(digest, sizeof digest, out);
}

typedef struct table_ref {
    const char *name;
    const char *order;
} table_ref;

/* Every table a schema-6 database holds, listed rather than read from
 * sqlite_schema: a query over the surviving tables would report that all of
 * them survived. */
static const table_ref ALL_TABLES[] = {
    {"schema_migrations", "version"},
    {"repositories", "id"},
    {"scans", "id"},
    {"files", "id"},
    {"commits", "id"},
    {"file_changes", "id"},
    {"compile_databases", "id"},
    {"evidence", "id"},
    {"repo_index_state", "repo_id"},
    {"repo_events", "id"},
    {"repo_commit_tips", "repo_id, ref_name"},
    {"daemon_state", "id"},
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
    {"decision_documents", "id"},
    {"decision_revisions", "id"},
    {"decision_alternatives", "id"},
    {"decision_links", "id"},
    {"decision_events", "id"},
    {"decision_challenges", "id"},
    {"decision_search", "rowid"},
    /* A9.2.4/A9.2.5. Derived, and in the round trip because their *absence*
     * reads as a positive statement: a restored index with no obstacle rows
     * looks like a repository whose search met no obstacle. */
    {"sem_build_inputs", "id"},
    {"sem_discovery_obstacles", "id"},
};

#define ALL_TABLE_COUNT (sizeof ALL_TABLES / sizeof ALL_TABLES[0])

/* A populated A0-A4 database, written as SQL for the same reason
 * test_migrate6.c does: this is about the copy, not about the writers, and a
 * fixture built through the whole stack would take a git repository and a
 * structural pass to say the same thing.
 *
 * The A4 rows are the interesting ones. `decision_revisions.content_hash` is a
 * real canonical hash of the row it describes, so `backup verify`'s rehash has
 * something true to confirm — a fabricated digest would make every verification
 * in this file pass for the wrong reason. */
static void seed(atlas_db *db, atlas_err *err) {
    static const char SEED_A0_A1[] =
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
        "INSERT INTO repo_index_state(repo_id, generation, last_complete_generation)"
        " VALUES(1,4,4);"
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
        "INSERT INTO code_index_state(repo_id, generation, last_complete_generation, analyzer_id,"
        " resolve_settled) VALUES(1,4,4,1,1);"
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

    /* A9.2.4/A9.2.5. The two derived tables that record what Atlas found and
     * what it could not look at.
     *
     * They are in the round trip because losing them is not visibly a loss: a
     * restored index with no obstacle rows reads as a repository whose search
     * met no obstacle, which is the direction that lets a negative conclusion be
     * believed when it should not be. Migration 20 must therefore survive a
     * backup and a restore like every other table, and the comparison below is
     * what says so. */
    static const char SEED_SEM[] =
        "INSERT INTO sem_build_inputs(id, repo_id, path_text, origin, accepted,"
        "  reject_reason, digest, unit_count, discovered_at)"
        " VALUES(1,1,'compile_commands.json','DISCOVERED',1,'','abc',3,"
        "        '2026-01-01T00:00:00Z');"
        "INSERT INTO sem_build_inputs(id, repo_id, path_text, origin, accepted,"
        "  reject_reason, digest, unit_count, discovered_at)"
        " VALUES(2,1,'vendor/compile_commands.json','DISCOVERED',0,"
        "        'a_symlinked_path_is_never_followed','',0,'2026-01-01T00:00:00Z');"
        "INSERT INTO sem_discovery_obstacles(id, repo_id, seq, path_text, reason,"
        "  discovered_at)"
        " VALUES(1,1,0,'locked','this_directory_could_not_be_entered',"
        "        '2026-01-01T00:00:00Z');"
        "INSERT INTO sem_discovery_obstacles(id, repo_id, seq, path_text, reason,"
        "  discovered_at)"
        " VALUES(2,1,1,'vendor',"
        "        'an_operator_excluded_this_subtree_from_the_search',"
        "        '2026-01-01T00:00:00Z');";

    T_OK(atlas_db_exec_sql(db, SEED_A0_A1, err), err);
    T_OK(atlas_db_exec_sql(db, SEED_A2, err), err);
    T_OK(atlas_db_exec_sql(db, SEED_A3, err), err);
    T_OK(atlas_db_exec_sql(db, SEED_SEM, err), err);
}

static void run_atlas(const char *const *argv, size_t n, atlas_err *err) {
    atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(argv, n, &so, &se, &code, err), err);
    T_CHECK_MSG(code == 0, "`atlas %s %s` exited %d: %s", argv[0], n > 1 ? argv[1] : "", code,
                atlas_buf_cstr(&se));
    atlas_buf_free(&so);
    atlas_buf_free(&se);
}

/* A real index: a real git repository, a real scan, a real structural pass and
 * a real decision document, all through the shipped binary. The A4 rows must be
 * genuine — `backup verify` rehashes every revision from its stored content, so
 * a hand-written `content_hash` would make every verification in this file pass
 * for the wrong reason.
 *
 * The A2 tables have no CLI writer that does not need a live Claude session, so
 * those rows are seeded as SQL against the repository the scan registered.
 */
static void build_populated(fixture *fx, atlas_err *err) {
    T_OK(fx_init_repo(fx, fx_repo(fx), NULL, err), err);
    T_OK(fx_write(fx_repo(fx), "a.c", "#include \"a.h\"\nint main(void){return helper();}\n", err),
         err);
    T_OK(fx_write(fx_repo(fx), "a.h", "int helper(void);\n", err), err);
    T_OK(fx_add_all(fx, fx_repo(fx), err), err);
    T_OK(fx_commit(fx, fx_repo(fx), "first", err), err);

    const char *dd = fx_data_dir(fx);
    const char *add[] = {"repo", "add", fx_repo(fx), "--name", "proj", "--data-dir", dd};
    run_atlas(add, 7, err);
    const char *scan[] = {"scan", "proj", "--data-dir", dd};
    run_atlas(scan, 4, err);
    const char *csync[] = {"code", "sync", "proj", "--data-dir", dd};
    run_atlas(csync, 5, err);
    const char *propose[] = {
                             "decision",
                             "propose",
                             "proj",
                             "--title",
                             "Keep the ledger canonical",
                             "--decision",
                             "The status columns are a cache; the events are the record.",
                             "--context",
                             "Two places could disagree about one document.",
                             "--rationale",
                             "A cache can be rebuilt and a ledger cannot.",
                             "--alternative",
                             "Trust the cache and repair the ledger from it.",
                             "--path",
                             "a.c",
                             "--data-dir",
                             dd};
    run_atlas(propose, 17, err);

    /* A2 rows, against the repository the scan registered. */
    atlas_db *db = NULL;
    T_OK(open_at(dd, &db, err), err);
    int64_t repo_id = 0;
    T_OK(atlas_db_query_int64(db, "SELECT id FROM repositories WHERE name='proj';", &repo_id, err),
         err);
    T_REQUIRE(repo_id > 0);
    char sql[2048];
    (void)snprintf(sql, sizeof sql,
                   "INSERT INTO ai_clients(id, provider, name, first_seen_at, last_seen_at)"
                   " VALUES(1,'anthropic','claude-code','2026-01-01T00:00:00Z',"
                   "'2026-01-01T00:00:00Z');"
                   "INSERT INTO ai_sessions(id, client_id, session_key, started_at, last_seen_at)"
                   " VALUES(1,1,'sess-a','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
                   "INSERT INTO ai_session_repos(session_id, repo_id, attached_at)"
                   " VALUES(1,%lld,'2026-01-01T00:00:00Z');"
                   "INSERT INTO ai_change_sets(id, session_id, repo_id, opened_at)"
                   " VALUES(1,1,%lld,'2026-01-01T00:00:00Z');"
                   "INSERT INTO ai_changed_paths(id, change_set_id, path_raw, path_text,"
                   " first_at, last_at) VALUES(1,1,X'612E63','a.c','2026-01-01T00:00:00Z',"
                   "'2026-01-01T00:00:00Z');"
                   "INSERT INTO ai_reasons(id, session_id, repo_id, created_at, provenance,"
                   " state, summary) VALUES(1,1,%lld,'2026-01-01T00:00:00Z','MODEL_PROPOSAL',"
                   "'proposed','a reason');"
                   "INSERT INTO ai_reason_paths(reason_id, path_raw, path_text)"
                   " VALUES(1,X'612E63','a.c');"
                   "INSERT INTO ai_decisions(id, session_id, repo_id, created_at, provenance,"
                   " state, title, statement, rationale) VALUES(1,1,%lld,"
                   "'2026-01-01T00:00:00Z','MODEL_PROPOSAL','proposed','An A2 decision',"
                   "'Recorded before A4 existed.','Because it seemed right.');"
                   "INSERT INTO ai_decision_paths(decision_id, path_raw, path_text)"
                   " VALUES(1,X'612E63','a.c');"
                   "INSERT INTO ai_checkpoints(id, session_id, created_at, phase)"
                   " VALUES(1,1,'2026-01-01T00:00:00Z','pre_compact');",
                   (long long)repo_id, (long long)repo_id, (long long)repo_id,
                   (long long)repo_id);
    T_OK(atlas_db_exec_sql(db, sql, err), err);
    atlas_db_close(db);
}

static void backup_to(const char *data_dir, const char *out_path, bool force, atlas_err *err) {
    atlas_backup_create_opts o;
    memset(&o, 0, sizeof o);
    o.output = out_path;
    o.force = force;
    atlas_backup_report rep;
    atlas_backup_report_init(&rep);
    T_OK(atlas_service_backup_create(data_dir, &o, &rep, err), err);
    T_CHECK(rep.size_bytes > 0);
    T_CHECK(rep.schema_version == ATLAS_SCHEMA_VERSION);
    atlas_backup_report_free(&rep);
}

static atlas_backup_verdict verdict_of(const char *path, bool *ok_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_backup_verify_report rep;
    atlas_backup_verify_report_init(&rep);
    T_OK(atlas_service_backup_verify(path, &rep, &err), &err);
    atlas_backup_verdict v = rep.verdict;
    if (ok_out != NULL) {
        *ok_out = rep.ok;
    }
    atlas_backup_verify_report_free(&rep);
    return v;
}

static void file_digest(const char *path, char *hex_out, int64_t *size_out) {
    int fd = open(path, O_RDONLY);
    T_REQUIRE(fd >= 0);
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    unsigned char buf[8192];
    int64_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        atlas_sha256_update(&ctx, buf, (size_t)n);
        total += n;
    }
    close(fd);
    unsigned char d[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, d);
    atlas_hex_encode(d, sizeof d, hex_out);
    if (size_out != NULL) {
        *size_out = total;
    }
}

static void restore_from(const char *data_dir, const char *backup, atlas_err *err,
                         atlas_status expect) {
    atlas_backup_restore_opts o;
    memset(&o, 0, sizeof o);
    o.input = backup;
    o.confirmed = true;
    atlas_backup_restore_report rep;
    atlas_backup_restore_report_init(&rep);
    atlas_status st = atlas_service_backup_restore(data_dir, &o, &rep, err);
    T_CHECK_MSG(st == expect, "restore returned %d, expected %d (%s)", (int)st, (int)expect,
                atlas_err_msg(err));
    atlas_backup_restore_report_free(&rep);
}

/* --- tests --------------------------------------------------------------- */

static void test_an_empty_index_round_trips(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    atlas_db_close(db);

    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "empty.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);
    bool ok = false;
    T_EQ_INT(verdict_of(atlas_buf_cstr(&out), &ok), ATLAS_BACKUP_OK);
    T_CHECK(ok);

    atlas_buf dest = ATLAS_BUF_INIT;
    path_in(&fx, "restored", &dest);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&dest), &err), &err);
    restore_from(atlas_buf_cstr(&dest), atlas_buf_cstr(&out), &err, ATLAS_OK);

    atlas_buf_free(&dest);
    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_a_populated_index_round_trips_table_for_table(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    build_populated(&fx, &err);

    atlas_db *db = NULL;
    char before[ALL_TABLE_COUNT][ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    for (size_t i = 0; i < ALL_TABLE_COUNT; i++) {
        table_digest(db, ALL_TABLES[i].name, ALL_TABLES[i].order, before[i]);
    }
    atlas_db_close(db);

    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "full.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);

    /* The verification is not a formality here: it rehashes every revision from
     * its stored content, so a copy that changed a single byte of decision
     * prose would fail before anything was compared. */
    atlas_backup_verify_report vr;
    atlas_backup_verify_report_init(&vr);
    T_OK(atlas_service_backup_verify(atlas_buf_cstr(&out), &vr, &err), &err);
    T_CHECK(vr.ok);
    T_CHECK_MSG(vr.revisions_rehashed >= 1, "expected at least one revision to be rehashed");
    T_EQ_INT(vr.revisions_corrupt, 0);
    T_EQ_INT(vr.ledger_mismatched, 0);
    atlas_backup_verify_report_free(&vr);

    atlas_buf dest = ATLAS_BUF_INIT;
    path_in(&fx, "restored", &dest);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&dest), &err), &err);
    restore_from(atlas_buf_cstr(&dest), atlas_buf_cstr(&out), &err, ATLAS_OK);

    T_OK(open_at(atlas_buf_cstr(&dest), &db, &err), &err);
    for (size_t i = 0; i < ALL_TABLE_COUNT; i++) {
        char after[ATLAS_SHA256_HEX_LEN + 1u];
        table_digest(db, ALL_TABLES[i].name, ALL_TABLES[i].order, after);
        T_CHECK_MSG(strcmp(before[i], after) == 0, "table \"%s\" differs after a restore",
                    ALL_TABLES[i].name);
    }
    atlas_db_close(db);

    atlas_buf_free(&dest);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* Rows committed but not yet checkpointed live only in the write-ahead log. A
 * file copy of `atlas.db` would miss them entirely and still open. */
static void test_rows_still_in_the_write_ahead_log_are_in_the_backup(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);

    atlas_buf db_file = ATLAS_BUF_INIT;
    db_path_of(fx_data_dir(&fx), &db_file);
    char main_before[ATLAS_SHA256_HEX_LEN + 1u];
    file_digest(atlas_buf_cstr(&db_file), main_before, NULL);

    /* Commit more, and deliberately do not checkpoint. The connection stays
     * open so SQLite cannot checkpoint on close either. */
    T_OK(atlas_db_exec_sql(db,
                           "INSERT INTO repo_events(id, repo_id, kind, created_at)"
                           " VALUES(9001,1,'reconciled','2026-02-02T00:00:00Z');",
                           &err),
         &err);

    char main_after[ATLAS_SHA256_HEX_LEN + 1u];
    file_digest(atlas_buf_cstr(&db_file), main_after, NULL);
    T_CHECK_MSG(strcmp(main_before, main_after) == 0,
                "the fixture did not produce an uncheckpointed write; the rest of this test "
                "would prove nothing");

    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "wal.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);
    atlas_db_close(db);

    atlas_db *bk = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&out), &bk, &err), &err);
    int64_t n = 0;
    T_OK(atlas_db_query_int64(bk, "SELECT count(*) FROM repo_events WHERE id=9001;", &n, &err),
         &err);
    T_CHECK_MSG(n == 1, "the uncheckpointed row is missing from the backup");
    atlas_db_close(bk);

    atlas_buf_free(&db_file);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* A backup must be one file with no sidecars, or `backup verify` could not be
 * read-only: a WAL-mode database cannot be opened without creating a `-shm`. */
static void test_a_backup_is_one_owner_only_file_with_no_sidecars(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);

    mode_t old = umask(0); /* the most permissive umask there is */
    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "modes.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);
    (void)umask(old);

    struct stat sb;
    T_REQUIRE(stat(atlas_buf_cstr(&out), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & 07777) == 0600, "backup mode is %04o, expected 0600",
                (unsigned)(sb.st_mode & 07777));

    atlas_buf side = ATLAS_BUF_INIT;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&side, &err, "%s-wal", atlas_buf_cstr(&out)), &err);
    T_CHECK_MSG(stat(atlas_buf_cstr(&side), &sb) != 0, "the backup left a -wal file beside it");
    atlas_buf_reset(&side);
    T_OK(atlas_buf_appendf(&side, &err, "%s-shm", atlas_buf_cstr(&out)), &err);
    T_CHECK_MSG(stat(atlas_buf_cstr(&side), &sb) != 0, "the backup left a -shm file beside it");

    atlas_buf_free(&side);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* Where a byte sequence sits in a file, or -1. */
static off_t offset_of(const char *path, const char *needle, size_t needle_len) {
    int fd = open(path, O_RDONLY);
    T_REQUIRE(fd >= 0);
    struct stat sb;
    T_REQUIRE(fstat(fd, &sb) == 0);
    unsigned char *whole = malloc((size_t)sb.st_size);
    T_REQUIRE(whole != NULL);
    T_REQUIRE(pread(fd, whole, (size_t)sb.st_size, 0) == (ssize_t)sb.st_size);
    close(fd);
    off_t at = -1;
    for (off_t i = 0; i + (off_t)needle_len <= sb.st_size; i++) {
        if (memcmp(whole + i, needle, needle_len) == 0) {
            at = i;
            break;
        }
    }
    free(whole);
    return at;
}

static void poke(const char *path, off_t at, const void *bytes, size_t n) {
    int fd = open(path, O_RDWR);
    T_REQUIRE(fd >= 0);
    T_REQUIRE(pwrite(fd, bytes, n, at) == (ssize_t)n);
    close(fd);
}

/* Corruption, in three kinds, because they have three different answers and
 * pretending otherwise would overstate what verification can do.
 *
 * SQLite has no per-page checksum. `PRAGMA integrity_check` validates b-tree
 * structure, not cell content, so a byte flipped inside an ordinary string
 * value leaves a structurally perfect database holding a different value.
 * Nothing Atlas can run will find that, and a test that claimed otherwise
 * would be a test of a guarantee that does not exist.
 *
 * What Atlas does have is a rehash of every decision revision, which is where
 * silent alteration would actually matter, and a whole-file SHA-256 for the
 * rest. Each of the three is asserted here. */
static void test_corruption_is_detected_where_atlas_can_detect_it(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    build_populated(&fx, &err);

    /* --- 1. inside a decision revision: caught by the rehash --------------- */
    atlas_buf a = ATLAS_BUF_INIT;
    path_in(&fx, "decision-corrupt.db", &a);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&a), false, &err);
    static const char PROSE[] = "the events are the record";
    off_t at = offset_of(atlas_buf_cstr(&a), PROSE, sizeof PROSE - 1);
    T_REQUIRE_MSG(at >= 0, "the decision prose was not found in the backup");
    unsigned char flipped = (unsigned char)((unsigned char)PROSE[0] ^ 0x20u); /* 't' -> 'T' */
    poke(atlas_buf_cstr(&a), at, &flipped, 1);

    atlas_backup_verify_report vr;
    atlas_backup_verify_report_init(&vr);
    T_OK(atlas_service_backup_verify(atlas_buf_cstr(&a), &vr, &err), &err);
    T_CHECK_MSG(!vr.ok, "altered decision prose verified as usable");
    T_EQ_INT(vr.verdict, ATLAS_BACKUP_INCONSISTENT);
    T_CHECK_MSG(vr.revisions_corrupt >= 1,
                "the rehash did not report the altered revision");
    atlas_backup_verify_report_free(&vr);

    atlas_buf dest = ATLAS_BUF_INIT;
    path_in(&fx, "restored", &dest);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&dest), &err), &err);
    restore_from(atlas_buf_cstr(&dest), atlas_buf_cstr(&a), &err, ATLAS_ERR_INTEGRITY);
    atlas_buf db_file = ATLAS_BUF_INIT;
    struct stat sb;
    db_path_of(atlas_buf_cstr(&dest), &db_file);
    T_CHECK_MSG(stat(atlas_buf_cstr(&db_file), &sb) != 0,
                "a refused restore created a database anyway");

    /* --- 2. a whole page destroyed: caught by integrity_check -------------- */
    atlas_buf b = ATLAS_BUF_INIT;
    path_in(&fx, "page-corrupt.db", &b);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&b), false, &err);
    at = offset_of(atlas_buf_cstr(&b), PROSE, sizeof PROSE - 1);
    T_REQUIRE(at >= 0);
    unsigned char junk[4096];
    memset(junk, 0xA5, sizeof junk);
    poke(atlas_buf_cstr(&b), at - (at % 4096), junk, sizeof junk);
    bool ok = true;
    atlas_backup_verdict v = verdict_of(atlas_buf_cstr(&b), &ok);
    T_CHECK_MSG(!ok, "a destroyed page verified as usable");
    T_CHECK_MSG(v == ATLAS_BACKUP_CORRUPT || v == ATLAS_BACKUP_NOT_ATLAS ||
                    v == ATLAS_BACKUP_INCONSISTENT,
                "unexpected verdict %s for a destroyed page", atlas_backup_verdict_name(v));

    /* --- 3. inside an ordinary value: NOT detectable, and said so ---------- */
    atlas_buf c = ATLAS_BUF_INIT;
    path_in(&fx, "value-corrupt.db", &c);
    atlas_backup_create_opts o;
    memset(&o, 0, sizeof o);
    o.output = atlas_buf_cstr(&c);
    atlas_backup_report crep;
    atlas_backup_report_init(&crep);
    T_OK(atlas_service_backup_create(fx_data_dir(&fx), &o, &crep, &err), &err);
    char recorded[ATLAS_SHA256_HEX_LEN + 1u];
    memcpy(recorded, crep.sha256, sizeof recorded);
    atlas_backup_report_free(&crep);

    static const char VALUE[] = "MODEL_PROPOSAL";
    at = offset_of(atlas_buf_cstr(&c), VALUE, sizeof VALUE - 1);
    T_REQUIRE(at >= 0);
    unsigned char lower = 'm';
    poke(atlas_buf_cstr(&c), at, &lower, 1);

    atlas_backup_verify_report vr3;
    atlas_backup_verify_report_init(&vr3);
    T_OK(atlas_service_backup_verify(atlas_buf_cstr(&c), &vr3, &err), &err);
    T_CHECK_MSG(vr3.ok,
                "this assertion is the honest one: SQLite has no page checksum, so a byte "
                "flipped inside an ordinary value leaves a structurally valid database. If "
                "this now fails, something detects it and the documented limitation should be "
                "narrowed rather than this test relaxed");
    /* What does change is the digest, which is why one is reported at all. */
    T_CHECK_MSG(strcmp(vr3.sha256, recorded) != 0,
                "the recorded SHA-256 did not change when the file did");
    atlas_backup_verify_report_free(&vr3);

    atlas_buf_free(&c);
    atlas_buf_free(&b);
    atlas_buf_free(&a);

    atlas_buf_free(&db_file);
    atlas_buf_free(&dest);
    fx_close(&fx);
}

static void test_truncation_is_detected_at_every_boundary(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);

    atlas_buf src = ATLAS_BUF_INIT;
    path_in(&fx, "whole.db", &src);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&src), false, &err);
    struct stat sb;
    T_REQUIRE(stat(atlas_buf_cstr(&src), &sb) == 0);

    /* Empty, mid-header, just past the header, mid-file, and one byte short. */
    const off_t cuts[] = {0, 8, 15, 100, 4096, sb.st_size / 2, sb.st_size - 1};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        atlas_buf cut = ATLAS_BUF_INIT;
        atlas_err_init(&err);
        T_OK(atlas_buf_appendf(&cut, &err, "%s/cut-%zu.db", atlas_buf_cstr(&fx.root), i), &err);
        backup_to(fx_data_dir(&fx), atlas_buf_cstr(&cut), false, &err);
        T_REQUIRE(truncate(atlas_buf_cstr(&cut), cuts[i]) == 0);
        bool ok = true;
        atlas_backup_verdict v = verdict_of(atlas_buf_cstr(&cut), &ok);
        T_CHECK_MSG(!ok, "a backup truncated to %lld bytes verified as usable",
                    (long long)cuts[i]);
        T_CHECK_MSG(v != ATLAS_BACKUP_OK, "truncation at %lld got verdict ok",
                    (long long)cuts[i]);
        atlas_buf_free(&cut);
    }
    atlas_buf_free(&src);
    fx_close(&fx);
}

static void test_a_database_atlas_did_not_write_is_refused(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_buf other = ATLAS_BUF_INIT;
    path_in(&fx, "someone-elses.db", &other);
    sqlite3 *h = NULL;
    T_REQUIRE(sqlite3_open(atlas_buf_cstr(&other), &h) == SQLITE_OK);
    T_REQUIRE(sqlite3_exec(h, "CREATE TABLE notes(x); INSERT INTO notes VALUES('hello');", NULL,
                           NULL, NULL) == SQLITE_OK);
    T_REQUIRE(sqlite3_close(h) == SQLITE_OK);

    bool ok = true;
    T_EQ_INT(verdict_of(atlas_buf_cstr(&other), &ok), ATLAS_BACKUP_NOT_ATLAS);
    T_CHECK(!ok);

    atlas_buf dest = ATLAS_BUF_INIT;
    path_in(&fx, "restored", &dest);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&dest), &err), &err);
    restore_from(atlas_buf_cstr(&dest), atlas_buf_cstr(&other), &err, ATLAS_ERR_INTEGRITY);

    atlas_buf_free(&dest);
    atlas_buf_free(&other);
    fx_close(&fx);
}

/* A backup from a newer Atlas is refused rather than migrated. Migrations only
 * go forward, so this build cannot make a schema-7 database into a schema-6
 * one; opening it anyway would silently drop whatever it could not see. */
static void test_a_future_schema_is_refused_and_never_restored(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);

    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "future.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);

    sqlite3 *h = NULL;
    T_REQUIRE(sqlite3_open(atlas_buf_cstr(&out), &h) == SQLITE_OK);
    char sql[160];
    (void)snprintf(sql, sizeof sql,
                   "INSERT INTO schema_migrations(version, name, applied_at)"
                   " VALUES(%d,'from-the-future','2027-01-01T00:00:00Z');",
                   ATLAS_SCHEMA_VERSION + 1);
    T_REQUIRE(sqlite3_exec(h, sql, NULL, NULL, NULL) == SQLITE_OK);
    T_REQUIRE(sqlite3_close(h) == SQLITE_OK);

    bool ok = true;
    T_EQ_INT(verdict_of(atlas_buf_cstr(&out), &ok), ATLAS_BACKUP_SCHEMA_FUTURE);
    T_CHECK(!ok);

    atlas_buf dest = ATLAS_BUF_INIT;
    path_in(&fx, "restored", &dest);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&dest), &err), &err);
    restore_from(atlas_buf_cstr(&dest), atlas_buf_cstr(&out), &err, ATLAS_ERR_INTEGRITY);
    struct stat sb;
    atlas_buf db_file = ATLAS_BUF_INIT;
    db_path_of(atlas_buf_cstr(&dest), &db_file);
    T_CHECK_MSG(stat(atlas_buf_cstr(&db_file), &sb) != 0,
                "a future-schema backup created a database anyway");

    atlas_buf_free(&db_file);
    atlas_buf_free(&dest);
    atlas_buf_free(&out);
    fx_close(&fx);
}

static void test_symlinks_are_refused_on_both_sides(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);

    atlas_buf real = ATLAS_BUF_INIT;
    path_in(&fx, "real.db", &real);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&real), false, &err);

    /* A symlinked destination: writing through it would put the backup
     * somewhere the operator did not name. */
    T_OK(fx_symlink(atlas_buf_cstr(&fx.root), "real.db", "link-dest.db", &err), &err);
    atlas_buf link_dest = ATLAS_BUF_INIT;
    path_in(&fx, "link-dest.db", &link_dest);
    {
        atlas_backup_create_opts o;
        memset(&o, 0, sizeof o);
        o.output = atlas_buf_cstr(&link_dest);
        o.force = true; /* even with --force */
        atlas_backup_report rep;
        atlas_backup_report_init(&rep);
        atlas_err e2;
        atlas_err_init(&e2);
        T_EQ_INT(atlas_service_backup_create(fx_data_dir(&fx), &o, &rep, &e2),
                 ATLAS_ERR_INTEGRITY);
        atlas_backup_report_free(&rep);
    }

    /* A symlinked directory component of the destination. */
    T_OK(fx_mkdir(atlas_buf_cstr(&fx.root), "elsewhere", &err), &err);
    T_OK(fx_symlink(atlas_buf_cstr(&fx.root), "elsewhere", "via-link", &err), &err);
    atlas_buf through = ATLAS_BUF_INIT;
    path_in(&fx, "via-link/x.db", &through);
    {
        atlas_backup_create_opts o;
        memset(&o, 0, sizeof o);
        o.output = atlas_buf_cstr(&through);
        atlas_backup_report rep;
        atlas_backup_report_init(&rep);
        atlas_err e2;
        atlas_err_init(&e2);
        T_EQ_INT(atlas_service_backup_create(fx_data_dir(&fx), &o, &rep, &e2),
                 ATLAS_ERR_INTEGRITY);
        atlas_backup_report_free(&rep);
    }

    /* A symlinked source is not read as a backup either. */
    bool ok = true;
    T_EQ_INT(verdict_of(atlas_buf_cstr(&link_dest), &ok), ATLAS_BACKUP_UNREADABLE);
    T_CHECK(!ok);

    /* And a symlinked database inside the destination data directory refuses a
     * restore rather than replacing the link and orphaning its target. */
    atlas_buf dest = ATLAS_BUF_INIT;
    path_in(&fx, "linkdata", &dest);
    T_OK(atlas_datadir_ensure(atlas_buf_cstr(&dest), &err), &err);
    T_OK(fx_symlink(atlas_buf_cstr(&dest), "../real.db", ATLAS_DB_FILENAME, &err), &err);
    restore_from(atlas_buf_cstr(&dest), atlas_buf_cstr(&real), &err, ATLAS_ERR_INTEGRITY);
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    char before[ATLAS_SHA256_HEX_LEN + 1u];
    file_digest(atlas_buf_cstr(&real), before, NULL);
    file_digest(atlas_buf_cstr(&real), after, NULL);
    T_EQ_STR(after, before);

    atlas_buf_free(&dest);
    atlas_buf_free(&through);
    atlas_buf_free(&link_dest);
    atlas_buf_free(&real);
    fx_close(&fx);
}

static void test_overwrite_is_refused_unless_forced(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);
    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);

    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "once.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);
    char first[ATLAS_SHA256_HEX_LEN + 1u];
    file_digest(atlas_buf_cstr(&out), first, NULL);

    atlas_backup_create_opts o;
    memset(&o, 0, sizeof o);
    o.output = atlas_buf_cstr(&out);
    atlas_backup_report rep;
    atlas_backup_report_init(&rep);
    atlas_err e2;
    atlas_err_init(&e2);
    T_EQ_INT(atlas_service_backup_create(fx_data_dir(&fx), &o, &rep, &e2), ATLAS_ERR_USAGE);
    atlas_backup_report_free(&rep);
    char after_refusal[ATLAS_SHA256_HEX_LEN + 1u];
    file_digest(atlas_buf_cstr(&out), after_refusal, NULL);
    T_CHECK_MSG(strcmp(first, after_refusal) == 0, "a refused overwrite still changed the file");

    /* A directory is refused whatever --force says: --force replaces a file, it
     * does not decide what kind of thing may be replaced. */
    T_OK(fx_mkdir(atlas_buf_cstr(&fx.root), "adir", &err), &err);
    atlas_buf dirpath = ATLAS_BUF_INIT;
    path_in(&fx, "adir", &dirpath);
    o.output = atlas_buf_cstr(&dirpath);
    o.force = true;
    atlas_backup_report_init(&rep);
    atlas_err_init(&e2);
    T_EQ_INT(atlas_service_backup_create(fx_data_dir(&fx), &o, &rep, &e2), ATLAS_ERR_USAGE);
    atlas_backup_report_free(&rep);

    /* With --force, a regular file is replaced. */
    o.output = atlas_buf_cstr(&out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), true, &err);

    atlas_buf_free(&dirpath);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* Every fault point, and after each one the original database must be exactly
 * what it was. This is the whole restore contract in one loop. */
static void test_every_injected_failure_leaves_the_original_untouched(void) {
    static const char *POINTS[] = {"write", "fsync", "fsync_dir", "rename", "post_verify"};

    for (size_t p = 0; p < sizeof POINTS / sizeof POINTS[0]; p++) {
        fixture fx;
        atlas_err err;
        atlas_err_init(&err);
        T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

        atlas_db *db = NULL;
        T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
        seed(db, &err);
        atlas_db_close(db);

        atlas_buf out = ATLAS_BUF_INIT;
        path_in(&fx, "source.db", &out);
        backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);

        /* A second, different index to restore over, so a silent no-op would
         * not look like success. */
        atlas_buf dest = ATLAS_BUF_INIT;
        path_in(&fx, "target", &dest);
        T_OK(open_at(atlas_buf_cstr(&dest), &db, &err), &err);
        T_OK(atlas_db_exec_sql(db,
                               "INSERT INTO repositories(id, name, root_path, root_path_text,"
                               " git_common_dir, git_common_dir_text, object_format, registered_at,"
                               " head_state) VALUES(7,'other',X'2F71','/q',X'2F712F67','/q/g',"
                               "'sha1','2026-03-03T00:00:00Z','born');",
                               &err),
             &err);
        atlas_db_close(db);

        atlas_buf target_db = ATLAS_BUF_INIT;
        db_path_of(atlas_buf_cstr(&dest), &target_db);
        char before[ATLAS_SHA256_HEX_LEN + 1u];
        int64_t size_before = 0;
        file_digest(atlas_buf_cstr(&target_db), before, &size_before);

        T_REQUIRE(setenv("ATLAS_BACKUP_FAULT", POINTS[p], 1) == 0);
        atlas_backup_restore_opts ro;
        memset(&ro, 0, sizeof ro);
        ro.input = atlas_buf_cstr(&out);
        ro.confirmed = true;
        atlas_backup_restore_report rep;
        atlas_backup_restore_report_init(&rep);
        atlas_err e2;
        atlas_err_init(&e2);
        atlas_status st = atlas_service_backup_restore(atlas_buf_cstr(&dest), &ro, &rep, &e2);
        bool published = rep.published;
        bool recovery = rep.recovery_made;
        atlas_backup_restore_report_free(&rep);
        T_REQUIRE(unsetenv("ATLAS_BACKUP_FAULT") == 0);

        T_CHECK_MSG(st != ATLAS_OK, "fault point \"%s\" did not fail the restore", POINTS[p]);

        char after[ATLAS_SHA256_HEX_LEN + 1u];
        int64_t size_after = 0;
        file_digest(atlas_buf_cstr(&target_db), after, &size_after);

        if (!published) {
            /* Failed before the commit: byte-identical, no exception. */
            T_CHECK_MSG(strcmp(before, after) == 0,
                        "fault point \"%s\" changed the original database", POINTS[p]);
            T_EQ_INT(size_after, size_before);
        } else {
            /* `post_verify` fails after the index was legitimately replaced.
             * The contract there is different and narrower: the database it
             * displaced is kept, and the report says where. */
            T_CHECK_MSG(recovery, "fault point \"%s\" replaced the index without keeping a copy",
                        POINTS[p]);
        }

        atlas_buf_free(&target_db);
        atlas_buf_free(&dest);
        atlas_buf_free(&out);
        fx_close(&fx);
    }
}

/* Restoring over a live index keeps what it displaced, and what it kept is a
 * usable Atlas database rather than a raw file copy that lost its log. */
static void test_a_restore_keeps_what_it_replaced(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);
    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "snapshot.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);

    /* Move the index on, without checkpointing, so the displaced database's
     * newest row exists only in its write-ahead log. */
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    T_OK(atlas_db_exec_sql(db,
                           "INSERT INTO repo_events(id, repo_id, kind, created_at)"
                           " VALUES(4242,1,'reconciled','2026-04-04T00:00:00Z');",
                           &err),
         &err);
    atlas_db_close(db);

    atlas_backup_restore_opts ro;
    memset(&ro, 0, sizeof ro);
    ro.input = atlas_buf_cstr(&out);
    ro.confirmed = true;
    atlas_backup_restore_report rep;
    atlas_backup_restore_report_init(&rep);
    T_OK(atlas_service_backup_restore(fx_data_dir(&fx), &ro, &rep, &err), &err);
    T_CHECK(rep.published);
    T_REQUIRE(rep.recovery_made);

    /* The restored index is the snapshot: the later row is gone. */
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(db, "SELECT count(*) FROM repo_events WHERE id=4242;", &n, &err),
         &err);
    T_EQ_INT(n, 0);
    atlas_db_close(db);

    /* The copy it kept still has it, which a plain file copy would not. */
    atlas_backup_verify_report vr;
    atlas_backup_verify_report_init(&vr);
    T_OK(atlas_service_backup_verify(atlas_buf_cstr(&rep.recovery_path), &vr, &err), &err);
    T_CHECK_MSG(vr.ok, "the copy of the replaced database does not verify");
    atlas_backup_verify_report_free(&vr);

    atlas_db *kept = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&rep.recovery_path), &kept, &err), &err);
    n = -1;
    T_OK(atlas_db_query_int64(kept, "SELECT count(*) FROM repo_events WHERE id=4242;", &n, &err),
         &err);
    T_CHECK_MSG(n == 1, "the replaced database's uncheckpointed row was not preserved");
    atlas_db_close(kept);

    /* No stale sidecar survived to be applied to the restored file. */
    atlas_buf side = ATLAS_BUF_INIT;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&side, &err, "%s/" ATLAS_DB_FILENAME "-wal", fx_data_dir(&fx)), &err);
    struct stat sb;
    if (stat(atlas_buf_cstr(&side), &sb) == 0) {
        T_CHECK_MSG(sb.st_size == 0,
                    "a non-empty write-ahead log from the replaced database survived");
    }

    atlas_buf_free(&side);
    atlas_backup_restore_report_free(&rep);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* Verification must be usable on a machine where Atlas has never stored
 * anything, and must leave it that way. Run through the real binary so the
 * whole process is under test, not just the service function. */
static void test_verification_creates_nothing_in_a_fresh_home(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_db *db = NULL;
    T_OK(open_at(fx_data_dir(&fx), &db, &err), &err);
    seed(db, &err);
    atlas_db_close(db);
    atlas_buf out = ATLAS_BUF_INIT;
    path_in(&fx, "fresh.db", &out);
    backup_to(fx_data_dir(&fx), atlas_buf_cstr(&out), false, &err);

    T_OK(fx_mkdir(atlas_buf_cstr(&fx.root), "fresh-home", &err), &err);
    atlas_buf home = ATLAS_BUF_INIT;
    path_in(&fx, "fresh-home", &home);
    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(atlas_buf_cstr(&home), before, &err), &err);

    atlas_buf home_env = ATLAS_BUF_INIT;
    atlas_buf xdg_env = ATLAS_BUF_INIT;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&home_env, &err, "HOME=%s", atlas_buf_cstr(&home)), &err);
    T_OK(atlas_buf_appendf(&xdg_env, &err, "XDG_DATA_HOME=%s/xdg", atlas_buf_cstr(&home)), &err);
    const char *env[] = {atlas_buf_cstr(&home_env), atlas_buf_cstr(&xdg_env), NULL};
    const char *args[] = {"backup", "verify", atlas_buf_cstr(&out), "--json"};
    atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas_env(args, 4, env, &so, &se, &code, &err), &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&so), "\"verdict\":\"ok\"") != NULL);

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_err_init(&err);
    T_OK(fx_tree_digest(atlas_buf_cstr(&home), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
                "`backup verify` created or changed something under a fresh HOME");

    /* And the backup itself is untouched by having been read. */
    atlas_buf_free(&so);
    atlas_buf_free(&se);
    atlas_buf_free(&xdg_env);
    atlas_buf_free(&home_env);
    atlas_buf_free(&home);
    atlas_buf_free(&out);
    fx_close(&fx);
}

/* An unusable backup is an answer, not a failure to answer: `backup verify`
 * writes a complete document and then exits non-zero, and in --json mode it
 * writes exactly one document. */
static void test_verify_reports_a_bad_backup_and_still_exits_non_zero(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    atlas_buf junk = ATLAS_BUF_INIT;
    path_in(&fx, "junk.db", &junk);
    T_OK(fx_write(atlas_buf_cstr(&fx.root), "junk.db", "not a database at all", &err), &err);

    const char *args[] = {"backup", "verify", atlas_buf_cstr(&junk), "--json"};
    atlas_buf so = ATLAS_BUF_INIT, se = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_atlas(args, 4, &so, &se, &code, &err), &err);
    T_EQ_INT(code, ATLAS_ERR_INTEGRITY);
    T_CHECK(strstr(atlas_buf_cstr(&so), "\"verdict\":\"not_sqlite\"") != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&so), "\"error\"") == NULL,
                "--json emitted a second document on stdout");

    atlas_buf_free(&so);
    atlas_buf_free(&se);
    atlas_buf_free(&junk);
    fx_close(&fx);
}

/* The structural claim: none of this is reachable from anything a model can
 * call. Asserted against the inventory the process reports, not against a list
 * kept somewhere in parallel with it. */
static void test_no_ai_facing_surface_can_back_up_restore_or_prune(void) {
    static const char *FORBIDDEN[] = {"backup", "restore", "prune", "maintenance", "vacuum"};
    const char *const *names = atlas_mcp_tool_names();
    T_REQUIRE(names != NULL);
    for (size_t i = 0; names[i] != NULL; i++) {
        for (size_t f = 0; f < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; f++) {
            T_CHECK_MSG(strstr(names[i], FORBIDDEN[f]) == NULL,
                        "MCP tool \"%s\" contains \"%s\"; A5 adds no such capability", names[i],
                        FORBIDDEN[f]);
        }
    }
}

/* --- the claim tripwire ---------------------------------------------------
 *
 * Operational documentation drifts in one direction: towards sounding stronger.
 * "verified" becomes "guaranteed", "fsynced" becomes "durable", "0600" becomes
 * "secure". This is the same device `tests/test_decision_mcp.c` uses for the
 * approval contract, aimed at A5's claims, and it exists because the overclaims
 * it forbids are all ones a reasonable person would write by accident.
 *
 * The required-wording half matters as much: a limitation deleted from the
 * documentation while it remains true of the code is the same failure as an
 * overclaim, and it is quieter. */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long n = ftell(f);
    rewind(f);
    if (n < 0) {
        (void)fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1u);
    if (buf == NULL) {
        (void)fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1u, (size_t)n, f);
    (void)fclose(f);
    buf[got] = '\0';
    /* Lowercased in place: a claim is no weaker for being capitalised. */
    for (size_t i = 0; i < got; i++) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') {
            buf[i] = (char)(buf[i] - 'A' + 'a');
        }
    }
    return buf;
}

static void test_no_operational_claim_is_stronger_than_the_implementation(void) {
    static const char *const FORBIDDEN[] = {
        /* Backups are a plain SQLite file with a mode. */
        "encrypted backup",
        "backups are encrypted",
        "backup is encrypted",
        "signed backup",
        "backups are signed",
        "tamper-proof",
        "tamper proof",
        /* fsync is what an application can do. It is not a durability promise. */
        "zero data loss",
        "no data can be lost",
        "cannot lose data",
        "guarantees durability",
        "survives disk failure",
        "survives a disk failure",
        "crash-proof",
        /* Verification is bounded, and one of its bounds is documented. */
        "detects all corruption",
        "detects any corruption",
        "verifies every byte",
        "guarantees the backup is correct",
        "bit-for-bit verification",
        /* Nothing here is incremental, and there is no log shipping. */
        "incremental backup",
        "point-in-time recovery",
        "continuous backup",
        /* Restore is same-machine SQLite. */
        "portable to any",
        "works on any platform",
        NULL,
    };
    static const char *const FILES[] = {
        ATLAS_SRC_DIR "/docs/operations.md",
        ATLAS_SRC_DIR "/README.md",
        ATLAS_SRC_DIR "/CLAUDE.md",
        ATLAS_SRC_DIR "/SECURITY.md",
        ATLAS_SRC_DIR "/include/atlas/backup.h",
        ATLAS_SRC_DIR "/include/atlas/maintenance.h",
        ATLAS_SRC_DIR "/src/core/service_backup.c",
        ATLAS_SRC_DIR "/src/core/service_maintenance.c",
        ATLAS_SRC_DIR "/src/db/db_backup.c",
        ATLAS_SRC_DIR "/src/cli/render_human.c",
        NULL,
    };
    for (size_t f = 0; FILES[f] != NULL; f++) {
        char *text = slurp(FILES[f]);
        T_REQUIRE_MSG(text != NULL, "cannot read %s", FILES[f]);
        for (size_t p = 0; FORBIDDEN[p] != NULL; p++) {
            T_CHECK_MSG(strstr(text, FORBIDDEN[p]) == NULL,
                        "%s claims \"%s\"; Atlas does not do that", FILES[f], FORBIDDEN[p]);
        }
        free(text);
    }

    /* And the limitations that are true stay written down. */
    static const struct {
        const char *file;
        const char *needle;
    } REQUIRED[] = {
        {ATLAS_SRC_DIR "/docs/operations.md", "not encrypted and not signed"},
        {ATLAS_SRC_DIR "/docs/operations.md", "sqlite has no per-page checksum"},
        {ATLAS_SRC_DIR "/docs/operations.md", "no claim of durability"},
        {ATLAS_SRC_DIR "/docs/operations.md", "the daemon must be stopped"},
        {ATLAS_SRC_DIR "/docs/operations.md", "there is no background deleter"},
        {ATLAS_SRC_DIR "/include/atlas/backup.h", "not encrypted and not signed"},
        {ATLAS_SRC_DIR "/include/atlas/maintenance.h", "no background deleter"},
        {NULL, NULL},
    };
    for (size_t i = 0; REQUIRED[i].file != NULL; i++) {
        char *text = slurp(REQUIRED[i].file);
        T_REQUIRE_MSG(text != NULL, "cannot read %s", REQUIRED[i].file);
        T_CHECK_MSG(strstr(text, REQUIRED[i].needle) != NULL,
                    "%s no longer says \"%s\"; the limitation is still true of the code",
                    REQUIRED[i].file, REQUIRED[i].needle);
        free(text);
    }
}

static const atlas_test TESTS[] = {
    {"an empty index round trips", test_an_empty_index_round_trips},
    {"a populated index round trips table for table",
     test_a_populated_index_round_trips_table_for_table},
    {"rows still in the write-ahead log are in the backup",
     test_rows_still_in_the_write_ahead_log_are_in_the_backup},
    {"a backup is one owner-only file with no sidecars",
     test_a_backup_is_one_owner_only_file_with_no_sidecars},
    {"corruption is detected where Atlas can detect it",
     test_corruption_is_detected_where_atlas_can_detect_it},
    {"truncation is detected at every boundary", test_truncation_is_detected_at_every_boundary},
    {"a database Atlas did not write is refused", test_a_database_atlas_did_not_write_is_refused},
    {"a future schema is refused and never restored",
     test_a_future_schema_is_refused_and_never_restored},
    {"symlinks are refused on both sides", test_symlinks_are_refused_on_both_sides},
    {"overwrite is refused unless forced", test_overwrite_is_refused_unless_forced},
    {"every injected failure leaves the original untouched",
     test_every_injected_failure_leaves_the_original_untouched},
    {"a restore keeps what it replaced", test_a_restore_keeps_what_it_replaced},
    {"verification creates nothing in a fresh HOME",
     test_verification_creates_nothing_in_a_fresh_home},
    {"verify reports a bad backup and still exits non-zero",
     test_verify_reports_a_bad_backup_and_still_exits_non_zero},
    {"no AI-facing surface can back up, restore or prune",
     test_no_ai_facing_surface_can_back_up_restore_or_prune},
    {"no operational claim is stronger than the implementation",
     test_no_operational_claim_is_stronger_than_the_implementation},
};

ATLAS_TEST_MAIN("backup", TESTS)

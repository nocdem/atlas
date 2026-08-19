/* Atlas - A9.1: knowledge kinds, and the lifecycle state that closes a demand.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These drive `atlas_decision_apply` and migration 13 against real storage,
 * without a daemon, because what is being checked is the model, the schema and
 * the migration rather than a transport.
 *
 * The two properties the whole season rests on are asserted here and they pull in
 * opposite directions:
 *
 *   1. **Backward compatibility is exact, not approximate.** A record written
 *      before this vocabulary existed reads as a DECISION, its content hash does
 *      not move, and every row of every table beside it survives. `atlas doctor`
 *      rehashes every revision, so a migration that moved one digest would report
 *      fifty-six approved decisions as tampered with — indistinguishable from the
 *      corruption that check exists to detect.
 *
 *   2. **Kind and status are orthogonal and stay orthogonal.** An APPROVED
 *      INVARIANT, an APPROVED ACCEPTED_RISK and an APPROVED DECISION are one
 *      status and three kinds, no code path derives either from the other, and the
 *      only thing the kind decides about the lifecycle is whether a record can be
 *      resolved.
 */
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision_ops.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- environment ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

#define ROOT_COMMIT "cccccccccccccccccccccccccccccccccccccccc"

static void seed_root_commit(env *e, int64_t repo_id, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO commits(repo_id, oid, parent_count, subject)"
                           " VALUES(%lld, '%s', 0, 'root');",
                           (long long)repo_id, ROOT_COMMIT),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

static void register_repo(env *e, atlas_err *err) {
    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-kind-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-kind-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
    seed_root_commit(e, e->repo_id, err);
}

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);
    register_repo(e, err);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

static int64_t count_of(env *e, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(e->db, sql, &n, &err), &err);
    return n;
}

static bool index_exists(atlas_db *db, const char *name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT 1 FROM sqlite_schema WHERE type='index' AND name = ?1;", &q,
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
    if (atlas_db_prepare(db,
                         "SELECT 1 FROM pragma_table_info(?1) WHERE name = ?2;", &q,
                         &err) != ATLAS_OK) {
        return false;
    }
    (void)sqlite3_bind_text(q, 1, table, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, column, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(q) == SQLITE_ROW;
    atlas_db_finish(db, q);
    return found;
}

static bool ddl_mentions(atlas_db *db, const char *table, const char *needle) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *q = NULL;
    if (atlas_db_prepare(db, "SELECT sql FROM sqlite_schema WHERE type='table' AND name = ?1;", &q,
                         &err) != ATLAS_OK) {
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

/* Rebuilds the four A9.1 tables back into their schema-12 shape, so that
 * migration 13 can be run forward against a database that genuinely predates it.
 *
 * The records are written first, through the real API at schema 13, so their
 * content hashes are the ones Atlas actually computes — which is what lets the
 * migration test assert that not one digest moved. A test that hand-wrote hashes
 * would be asserting that migration 13 preserves numbers the test invented.
 *
 * Foreign keys are turned off around it for exactly the reason migration 13 turns
 * them off: `decision_links.revision_id` declares `ON DELETE CASCADE`, so
 * dropping `decision_revisions` with them enabled would silently delete every
 * link — and the assertions below prove the wind-back did not, so the forward
 * migration is measured against a complete database. */
static void wind_back_to_schema_12(env *e, atlas_err *err) {
    int64_t links_before = count_of(e, "SELECT COUNT(*) FROM decision_links;");
    int64_t revs_before = count_of(e, "SELECT COUNT(*) FROM decision_revisions;");
    T_REQUIRE(links_before > 0 && revs_before > 0);

    T_OK(atlas_db_exec_sql(e->db, "PRAGMA foreign_keys=OFF;", err), err);
    static const char BACK_DOCUMENTS[] =
        /* documents: no `kind`, and the four-state status vocabulary. */
        "CREATE TABLE d12_documents ("
        "  id INTEGER PRIMARY KEY,"
        "  uid TEXT NOT NULL UNIQUE,"
        "  repo_id INTEGER NOT NULL,"
        "  repo_root_hash TEXT NOT NULL,"
        "  repo_identity_hash TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL,"
        "  latest_revision_no INTEGER NOT NULL DEFAULT 0,"
        "  current_revision_id INTEGER,"
        "  current_status TEXT NOT NULL DEFAULT 'PROPOSED' CHECK(current_status IN"
        "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED')),"
        "  superseded_by_document_id INTEGER,"
        "  superseded_at TEXT"
        ");"
        "INSERT INTO d12_documents SELECT id, uid, repo_id, repo_root_hash, repo_identity_hash,"
        "  created_at, updated_at, latest_revision_no, current_revision_id, current_status,"
        "  superseded_by_document_id, superseded_at FROM decision_documents;"
        "DROP TABLE decision_documents;"
        "ALTER TABLE d12_documents RENAME TO decision_documents;"
        "CREATE INDEX idx_decision_docs_repo ON decision_documents(repo_id, id DESC);"
        "CREATE INDEX idx_decision_docs_status ON decision_documents"
        "  (repo_id, current_status, id DESC);"
        "CREATE INDEX idx_decision_docs_root ON decision_documents(repo_root_hash);"
        "CREATE INDEX idx_decision_docs_identity ON decision_documents(repo_identity_hash)"
        "  WHERE repo_identity_hash <> '';";
    static const char BACK_REVISIONS[] =
        /* revisions: the four-state vocabulary. */
        "CREATE TABLE d12_revisions ("
        "  id INTEGER PRIMARY KEY,"
        "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
        "  revision_no INTEGER NOT NULL,"
        "  content_hash TEXT NOT NULL,"
        "  title TEXT NOT NULL,"
        "  context_text TEXT NOT NULL DEFAULT '',"
        "  decision_text TEXT NOT NULL DEFAULT '',"
        "  rationale_text TEXT NOT NULL DEFAULT '',"
        "  consequences_text TEXT NOT NULL DEFAULT '',"
        "  scope TEXT NOT NULL DEFAULT 'UNKNOWN' CHECK(scope IN"
        "    ('UNKNOWN','REPOSITORY','SUBSYSTEM','PATHS')),"
        "  proposed_by TEXT NOT NULL CHECK(proposed_by IN"
        "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC')),"
        "  session_id INTEGER,"
        "  session_unbound INTEGER NOT NULL DEFAULT 0,"
        "  unbound_reason TEXT,"
        "  basis_head TEXT,"
        "  basis_repo_identity_hash TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'PROPOSED' CHECK(state IN"
        "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED')),"
        "  imported_from_ai_decision_id INTEGER,"
        "  dedup_key TEXT,"
        "  UNIQUE(document_id, revision_no)"
        ");"
        "INSERT INTO d12_revisions SELECT id, document_id, revision_no, content_hash, title,"
        "  context_text, decision_text, rationale_text, consequences_text, scope, proposed_by,"
        "  session_id, session_unbound, unbound_reason, basis_head, basis_repo_identity_hash,"
        "  created_at, state, imported_from_ai_decision_id, dedup_key FROM decision_revisions;"
        "DROP TABLE decision_revisions;"
        "ALTER TABLE d12_revisions RENAME TO decision_revisions;"
        "CREATE INDEX idx_decision_rev_doc ON decision_revisions(document_id, revision_no DESC);"
        "CREATE UNIQUE INDEX idx_decision_rev_current ON decision_revisions(document_id)"
        "  WHERE state = 'APPROVED';"
        "CREATE UNIQUE INDEX idx_decision_rev_import ON decision_revisions"
        "  (imported_from_ai_decision_id) WHERE imported_from_ai_decision_id IS NOT NULL;"
        "CREATE UNIQUE INDEX idx_decision_rev_dedup ON decision_revisions(document_id, dedup_key)"
        "  WHERE dedup_key IS NOT NULL;";
    static const char BACK_EVENTS[] =
        /* events: the four-event vocabulary, still AUTOINCREMENT. */
        "CREATE TABLE d12_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
        "  revision_id INTEGER REFERENCES decision_revisions(id),"
        "  revision_no INTEGER NOT NULL DEFAULT 0,"
        "  event TEXT NOT NULL CHECK(event IN"
        "    ('PROPOSED','APPROVED','REJECTED','SUPERSEDED')),"
        "  actor TEXT NOT NULL CHECK(actor IN"
        "    ('MODEL_PROPOSAL','MODEL_INFERENCE','LOCAL_OPERATOR_CONFIRMED','ATLAS_AUTOMATIC')),"
        "  content_hash TEXT,"
        "  challenge_id INTEGER,"
        "  superseded_by_revision_id INTEGER,"
        "  superseded_by_document_id INTEGER,"
        "  detail TEXT,"
        "  created_at TEXT NOT NULL,"
        "  dedup_key TEXT"
        ");"
        "INSERT INTO d12_events SELECT id, document_id, revision_id, revision_no, event, actor,"
        "  content_hash, challenge_id, superseded_by_revision_id, superseded_by_document_id,"
        "  detail, created_at, dedup_key FROM decision_events;"
        "DROP TABLE decision_events;"
        "ALTER TABLE d12_events RENAME TO decision_events;"
        "CREATE INDEX idx_decision_events_doc ON decision_events(document_id, id);"
        "CREATE INDEX idx_decision_events_rev ON decision_events(revision_id, id);"
        "CREATE UNIQUE INDEX idx_decision_events_dedup ON decision_events(document_id, dedup_key)"
        "  WHERE dedup_key IS NOT NULL;";
    static const char BACK_CHALLENGES[] =
        /* challenges: the four-intent vocabulary. */
        "CREATE TABLE d12_challenges ("
        "  id INTEGER PRIMARY KEY,"
        "  token TEXT NOT NULL UNIQUE,"
        "  repo_id INTEGER NOT NULL,"
        "  document_id INTEGER NOT NULL REFERENCES decision_documents(id),"
        "  revision_id INTEGER NOT NULL REFERENCES decision_revisions(id),"
        "  revision_no INTEGER NOT NULL,"
        "  content_hash TEXT NOT NULL,"
        "  intent TEXT NOT NULL CHECK(intent IN ('approve','reject','supersede','revalidate')),"
        "  supersede_document_id INTEGER,"
        "  indexed_commit TEXT,"
        "  evidence_digest TEXT,"
        "  prior_freshness TEXT CHECK(prior_freshness IS NULL OR prior_freshness IN"
        "    ('FRESH','STALE','IMPACTED','UNKNOWN')),"
        "  prior_reasons TEXT,"
        "  created_at TEXT NOT NULL,"
        "  expires_at TEXT NOT NULL,"
        "  consumed INTEGER NOT NULL DEFAULT 0,"
        "  consumed_at TEXT"
        ");"
        "INSERT INTO d12_challenges SELECT id, token, repo_id, document_id, revision_id,"
        "  revision_no, content_hash, intent, supersede_document_id, indexed_commit,"
        "  evidence_digest, prior_freshness, prior_reasons, created_at, expires_at, consumed,"
        "  consumed_at FROM decision_challenges;"
        "DROP TABLE decision_challenges;"
        "ALTER TABLE d12_challenges RENAME TO decision_challenges;"
        "CREATE INDEX idx_decision_challenges_repo ON decision_challenges"
        "  (repo_id, consumed, expires_at);";
    /* A9.2's tables were added by migrations 14 and 15, so a database rewound
     * to twelve must not still be carrying them: the forward migration would
     * find them already present and fail. A new table means a line here, which
     * is the same "nothing is globbed" discipline the source list follows. */
    static const char BACK_VERSION[] =
        "DROP TABLE verify_lifecycle_audit;"
        "DROP TABLE verify_reliability;"
        "DROP TABLE verify_outcomes;"
        "DROP TABLE verify_results;"
        "DROP TABLE verify_attestation_evidence;"
        "DROP TABLE verify_attestations;"
        "DROP TABLE verify_evidence_deps;"
        "DROP TABLE verify_evidence;"
        "DROP TABLE verify_claims;"
        "DROP TABLE verify_actors;"
        /* A9.2.3's table, added by migration 18, for the same reason. Its seven
         * columns on `sem_generations` go too: that table belongs to migration
         * 11 and correctly survives a rewind to twelve, but the columns a later
         * migration added to it do not, and leaving them would present migration
         * 18 with columns it is about to add. */
        "DROP TABLE sem_repo_config;"
        "ALTER TABLE sem_generations DROP COLUMN scope_discovery;"
        "ALTER TABLE sem_generations DROP COLUMN scope_candidates;"
        "ALTER TABLE sem_generations DROP COLUMN scope_covered;"
        "ALTER TABLE sem_generations DROP COLUMN scope_uncovered;"
        "ALTER TABLE sem_generations DROP COLUMN tu_test;"
        "ALTER TABLE sem_generations DROP COLUMN tu_production;"
        "ALTER TABLE sem_generations DROP COLUMN test_scope_known;"
        "ALTER TABLE sem_generations DROP COLUMN source_identity;"
        /* A9.2.4's columns and its candidate table go for the same reason:
         * migration 19 adds them, so a database wound back past 19 must not
         * still hold them or the re-run fails on a duplicate column. */
        /* A9.2.5's table, dropped first: a rewind that leaves a later
              * migration's table behind is not a database at the version it
              * claims, and migration 20 would then fail to create it. */
             "DROP TABLE sem_discovery_obstacles;"
             "DROP TABLE sem_build_inputs;"
        "ALTER TABLE sem_generations DROP COLUMN discovery;"
        "ALTER TABLE sem_generations DROP COLUMN input_count;"
        "ALTER TABLE sem_generations DROP COLUMN scope_excluded;"
        /* A11.0's run, added by migration 21, for exactly the reason every line
         * above it is here: a rewind that leaves a later migration's table
         * behind is not a database at the version it claims, and migration 21
         * then fails on `table orch_runs already exists`. `orch_jobs` itself
         * belongs to migration 8 and correctly survives a rewind to twelve, but
         * the column and the index migration 21 added to it do not.
         *
         * This line was missing from A11.0 and the failure it caused was real:
         * the case below could not reach migration 13's assertions at all. It is
         * the cost of the discipline the comment above states — a new table
         * means a line here — being obeyed by hand. */
        /* A10.0's per-attempt cost, added by migration 22, for the same reason
         * and added at the same time as the migration rather than after a
         * full-suite run found it missing — which is exactly how the line above
         * came to be written. */
        /* A10.1's table goes with A10.0's, and for the reason every drop in this
         * list exists: a rewind that leaves a later migration's table behind is
         * not a database at the version it claims, and migration 23 would then
         * fail to create it. */
        /* A11.6's two indexes and its column on `orch_jobs`, for the same
         * reason: migration 24 replaced migration 21's single index with two,
         * so undoing 21 means undoing 24 first, and the indexes go before the
         * columns because SQLite refuses to drop an indexed one. */
        "DROP TABLE orch_run_memory;"
        "DROP TABLE orch_usage;"
        "DROP TABLE orch_runs;"
        "DROP INDEX idx_orch_jobs_one_active_repo_tree;"
        "DROP INDEX idx_orch_jobs_active_slot;"
        "DROP INDEX idx_orch_jobs_run;"
        "ALTER TABLE orch_jobs DROP COLUMN run_slot;"
        "ALTER TABLE orch_jobs DROP COLUMN run_uid;"
        "DELETE FROM schema_migrations WHERE version >= 13;";

    T_OK(atlas_db_exec_sql(e->db, BACK_DOCUMENTS, err), err);
    T_OK(atlas_db_exec_sql(e->db, BACK_REVISIONS, err), err);
    T_OK(atlas_db_exec_sql(e->db, BACK_EVENTS, err), err);
    T_OK(atlas_db_exec_sql(e->db, BACK_CHALLENGES, err), err);
    T_OK(atlas_db_exec_sql(e->db, BACK_VERSION, err), err);
    T_OK(atlas_db_exec_sql(e->db, "PRAGMA foreign_keys=ON;", err), err);

    /* The wind-back really produced a schema-12 database, and really preserved
     * everything. Without these the forward migration could be measured against
     * a database that already had the new shape, or one the wind-back had
     * quietly emptied. */
    /* Twelve as a literal, not `ATLAS_SCHEMA_VERSION - 1`. This test is about
     * migration *thirteen* specifically, so the wind-back target is a fixed
     * point in history; expressing it relative to the current version made it
     * silently mean something else the moment A9.2 added migrations 14 and 15. */
    T_EQ_INT(atlas_db_schema_version(e->db, err), 12);
    T_CHECK_MSG(!column_exists(e->db, "decision_documents", "kind"),
                "the wind-back left the kind column in place");
    T_CHECK_MSG(!ddl_mentions(e->db, "decision_revisions", "RESOLVED"),
                "the wind-back left RESOLVED in the revision vocabulary");
    T_CHECK_MSG(!ddl_mentions(e->db, "decision_events", "RESOLVED"),
                "the wind-back left RESOLVED in the event vocabulary");
    T_CHECK_MSG(!ddl_mentions(e->db, "decision_challenges", "resolve"),
                "the wind-back left the resolve intent in place");
    T_CHECK_MSG(!index_exists(e->db, "idx_decision_docs_kind"),
                "the wind-back left the kind index in place");
    T_EQ_INT((int)count_of(e, "SELECT COUNT(*) FROM decision_links;"), (int)links_before);
    T_EQ_INT((int)count_of(e, "SELECT COUNT(*) FROM decision_revisions;"), (int)revs_before);
}

/* --- operations ------------------------------------------------------------- */

static void propose_kind(env *e, atlas_decision_kind kind, const char *title, atlas_buf *uid_out,
                         atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "the recorded body", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    op.knowledge_kind = kind;
    op.knowledge_kind_given = true;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_CHECK(res.document_created);
    T_CHECK_MSG(res.knowledge_kind == kind, "propose echoed %s for a %s",
                atlas_decision_kind_name(res.knowledge_kind), atlas_decision_kind_name(kind));
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

/* A proposal that names no kind at all, which is what every client written
 * before A9.1 sends. */
static void propose_without_a_kind(env *e, const char *title, atlas_buf *uid_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "the recorded body", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    if (uid_out != NULL) {
        T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    }
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

static void challenge_for(env *e, const char *uid, atlas_decision_intent intent,
                          atlas_buf *token_out, char *confirm_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.uid, uid, err), err);
    op.intent = intent;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_OK(atlas_buf_set(token_out, res.token.data, res.token.len, err), err);
    (void)snprintf(confirm_out, ATLAS_DECISION_CONFIRM_MAX, "%s", res.confirm);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

/* The same, returning the status so a refusal at issue time can be asserted. */
static atlas_status try_challenge(env *e, const char *uid, atlas_decision_intent intent,
                                 atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_CHALLENGE);
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_buf_set_str(&op.repo_name, "proj", &ignore);
    (void)atlas_buf_set_str(&op.uid, uid, &ignore);
    op.intent = intent;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    atlas_status st = atlas_decision_apply(e->db, &op, &res, err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
    return st;
}

static atlas_status spend(env *e, atlas_decision_op_kind kind, const char *uid, const char *token,
                          const char *confirm, atlas_decision_result *res, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, kind);
    atlas_err ignore;
    atlas_err_init(&ignore);
    (void)atlas_buf_set_str(&op.repo_name, "proj", &ignore);
    (void)atlas_buf_set_str(&op.uid, uid, &ignore);
    (void)atlas_buf_set_str(&op.token, token, &ignore);
    (void)atlas_buf_set_str(&op.confirmation, confirm, &ignore);
    atlas_status st = atlas_decision_apply(e->db, &op, res, err);
    atlas_decision_op_free(&op);
    return st;
}

static void approve(env *e, const char *uid, atlas_err *err) {
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge_for(e, uid, ATLAS_DECISION_INTENT_APPROVE, &token, confirm, err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(e, ATLAS_DECISION_OP_APPROVE, uid, atlas_buf_cstr(&token), confirm, &res, err), err);
    T_CHECK(res.state == ATLAS_DECISION_APPROVED);
    atlas_decision_result_free(&res);
    atlas_buf_free(&token);
}

static void resolve_it(env *e, const char *uid, atlas_err *err) {
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    challenge_for(e, uid, ATLAS_DECISION_INTENT_RESOLVE, &token, confirm, err);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(spend(e, ATLAS_DECISION_OP_RESOLVE, uid, atlas_buf_cstr(&token), confirm, &res, err), err);
    T_CHECK_MSG(res.state == ATLAS_DECISION_RESOLVED, "resolve left the revision %s",
                atlas_decision_state_name(res.state));
    atlas_decision_result_free(&res);
    atlas_buf_free(&token);
}

/* --- reading back ----------------------------------------------------------- */

typedef struct probe {
    char status[16];
    char kind[32];
    bool seen;
    int64_t matches;
} probe;

static atlas_status probe_cb(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    (void)err;
    probe *p = (probe *)ud;
    (void)snprintf(p->status, sizeof p->status, "%s", row->status != NULL ? row->status : "");
    (void)snprintf(p->kind, sizeof p->kind, "%s", row->kind != NULL ? row->kind : "");
    p->seen = true;
    p->matches++;
    return ATLAS_OK;
}

static void read_doc(env *e, const char *uid, probe *p) {
    atlas_err err;
    atlas_err_init(&err);
    memset(p, 0, sizeof *p);
    int64_t doc = 0, repo = 0;
    bool found = false;
    T_OK(atlas_db_decision_find_uid(e->db, uid, &doc, &repo, &found, &err), &err);
    T_REQUIRE(found);
    bool seen = false;
    T_OK(atlas_db_decision_document_row(e->db, doc, probe_cb, p, &seen, &err), &err);
    T_CHECK(seen);
}

/* --- 1. the vocabulary ------------------------------------------------------ */

static void test_the_kind_vocabulary_is_closed_and_decision_is_zero(void) {
    /* DECISION is zero, and that is load-bearing rather than tidy: a zeroed
     * `atlas_decision_op`, an omitted argument and a column default all have to
     * mean DECISION for backward compatibility to be exact. */
    T_EQ_INT((int)ATLAS_DECISION_KIND_DECISION, 0);

    atlas_decision_kind k = ATLAS_DECISION_KIND_OBLIGATION;
    /* No default and no case folding. An unrecognised value must not become a
     * known one — a garbled classification that silently reads as DECISION is a
     * record labelled something its author did not ask for. */
    T_CHECK(!atlas_decision_kind_parse("decision", &k));
    T_CHECK(!atlas_decision_kind_parse("", &k));
    T_CHECK(!atlas_decision_kind_parse("INVARIENT", &k));
    T_CHECK(!atlas_decision_kind_parse("ANY", &k));
    T_CHECK(!atlas_decision_kind_parse(NULL, &k));

    /* Every member round-trips through its name, and the table covers the enum. */
    T_EQ_INT((int)atlas_decision_kind_count(), (int)ATLAS_DECISION_KIND_MAX);
    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        atlas_decision_kind want = atlas_decision_kind_at(i);
        atlas_decision_kind got = ATLAS_DECISION_KIND_DECISION;
        const char *name = atlas_decision_kind_name(want);
        T_CHECK_MSG(atlas_decision_kind_parse(name, &got) && got == want,
                    "%s did not round-trip", name);
        /* Every kind has a written meaning, and it is Atlas' own text. */
        const char *why = atlas_decision_kind_description(want);
        T_CHECK_MSG(why != NULL && why[0] != '\0', "%s has no description", name);
    }
    /* The enum's own members, in order, so a member added to the enum without a
     * row in KINDS[] is caught here rather than by falling back to DECISION. */
    T_CHECK(atlas_decision_kind_at(0) == ATLAS_DECISION_KIND_DECISION);
    T_CHECK(atlas_decision_kind_at(7) == ATLAS_DECISION_KIND_REJECTED_ALTERNATIVE);

    /* The usage string every refusal quotes names every kind exactly once. */
    const char *list = atlas_decision_kind_list();
    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        const char *name = atlas_decision_kind_name(atlas_decision_kind_at(i));
        const char *first = strstr(list, name);
        T_CHECK_MSG(first != NULL, "the kind list omits %s", name);
        if (first != NULL) {
            T_CHECK_MSG(strstr(first + 1, name) == NULL, "the kind list names %s twice", name);
        }
    }
    /* RESOLVED joined the lifecycle and the same closure rule applies to it. */
    atlas_decision_state s = ATLAS_DECISION_PROPOSED;
    T_CHECK(atlas_decision_state_parse("RESOLVED", &s) && s == ATLAS_DECISION_RESOLVED);
    T_CHECK(!atlas_decision_state_parse("resolved", &s));
    T_CHECK(strcmp(atlas_decision_state_name(ATLAS_DECISION_RESOLVED), "RESOLVED") == 0);
    /* And the resolve intent, which is what the operator channel binds. */
    atlas_decision_intent in = ATLAS_DECISION_INTENT_APPROVE;
    T_CHECK(atlas_decision_intent_parse("resolve", &in) && in == ATLAS_DECISION_INTENT_RESOLVE);
    T_CHECK(strcmp(atlas_decision_intent_name(ATLAS_DECISION_INTENT_RESOLVE), "resolve") == 0);
}

/* --- 2. creating every kind ------------------------------------------------- */

static void test_every_kind_can_be_created_and_reads_back(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        atlas_decision_kind kind = atlas_decision_kind_at(i);
        atlas_buf uid = ATLAS_BUF_INIT;
        char title[128];
        (void)snprintf(title, sizeof title, "a record of kind %s",
                       atlas_decision_kind_name(kind));
        propose_kind(&e, kind, title, &uid, &err);
        probe p;
        read_doc(&e, atlas_buf_cstr(&uid), &p);
        T_CHECK_MSG(strcmp(p.kind, atlas_decision_kind_name(kind)) == 0,
                    "a %s read back as %s", atlas_decision_kind_name(kind), p.kind);
        /* Every kind starts PROPOSED. The dimensions are independent, so the
         * kind must not move the starting status. */
        T_CHECK_MSG(strcmp(p.status, "PROPOSED") == 0, "a new %s was %s",
                    atlas_decision_kind_name(kind), p.status);
        atlas_buf_free(&uid);
    }

    /* And a proposal that names no kind is a DECISION — the property every
     * pre-A9.1 client depends on. */
    atlas_buf plain = ATLAS_BUF_INIT;
    propose_without_a_kind(&e, "a record from a client that has never heard of kinds", &plain,
                           &err);
    probe p;
    read_doc(&e, atlas_buf_cstr(&plain), &p);
    T_CHECK_MSG(strcmp(p.kind, "DECISION") == 0, "an unclassified proposal became %s", p.kind);
    atlas_buf_free(&plain);

    env_close(&e);
}

/* The schema is the backstop, not the parser: a kind outside the vocabulary
 * cannot be written even by something that bypasses every Atlas function. */
static void test_the_schema_refuses_a_kind_outside_the_vocabulary(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_POLICY, "a policy", &uid, &err);

    atlas_err berr;
    atlas_err_init(&berr);
    atlas_status st = atlas_db_exec_sql(
        e.db, "UPDATE decision_documents SET kind = 'ARCHITECTURE_ISH';", &berr);
    T_CHECK_MSG(st != ATLAS_OK, "the schema accepted a kind outside the vocabulary");
    /* And the legitimate value is still there: a refusal that also corrupts is
     * not a refusal. */
    probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK(strcmp(p.kind, "POLICY") == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- 3. the lifecycle ------------------------------------------------------- */

static void test_an_obligation_can_be_resolved_and_stays_resolved(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_OBLIGATION,
                 "replace the debug signing key before any public release", &uid, &err);
    approve(&e, atlas_buf_cstr(&uid), &err);

    probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK(strcmp(p.status, "APPROVED") == 0);
    T_CHECK(strcmp(p.kind, "OBLIGATION") == 0);

    resolve_it(&e, atlas_buf_cstr(&uid), &err);

    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK_MSG(strcmp(p.status, "RESOLVED") == 0, "a resolved obligation reads as %s", p.status);
    /* The kind is untouched by the transition: what changed is where the
     * workflow left the record, not what sort of record it is. */
    T_CHECK(strcmp(p.kind, "OBLIGATION") == 0);
    /* Nothing was deleted. The revision, its ledger and its prose are all still
     * there, which is what "resolved without rewriting history" means. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), 1);
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event = 'RESOLVED';") == 1,
                "the resolution was not appended to the ledger");
    T_CHECK_MSG(count_of(&e, "SELECT COUNT(*) FROM decision_events WHERE event = 'APPROVED';") == 1,
                "resolving removed the approval event");
    /* The document no longer points at an effective revision, so the gate and
     * every "what is current" read drop it. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_documents"
                               " WHERE current_revision_id IS NOT NULL;"),
             0);

    /* The cached status agrees with a replay of the ledger. This is the check
     * `atlas doctor` runs, and the one that would fail if `recompute_status` and
     * the replay disagreed about where RESOLVED sits in the precedence. */
    int64_t doc = 0, repo = 0;
    bool found = false;
    T_OK(atlas_db_decision_find_uid(e.db, atlas_buf_cstr(&uid), &doc, &repo, &found, &err), &err);
    bool ok = false;
    atlas_buf detail = ATLAS_BUF_INIT;
    T_OK(atlas_db_decision_verify(e.db, doc, &ok, &detail, &err), &err);
    T_CHECK_MSG(ok, "the ledger disagrees with the cached status: %s", atlas_buf_cstr(&detail));
    atlas_buf_free(&detail);

    /* Terminal: a second resolution has nothing to act on, and the capability
     * cannot be issued at all because the revision is no longer approved. */
    atlas_err serr;
    atlas_err_init(&serr);
    T_CHECK_MSG(try_challenge(&e, atlas_buf_cstr(&uid), ATLAS_DECISION_INTENT_RESOLVE, &serr) !=
                    ATLAS_OK,
                "a resolved revision could be resolved again");
    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK(strcmp(p.status, "RESOLVED") == 0);

    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_kind_that_makes_no_demand_cannot_be_resolved(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* Every non-resolvable kind, approved, then refused a resolution — refused
     * at *issue*, so an operator learns before typing a confirmation. */
    for (size_t i = 0; i < atlas_decision_kind_count(); i++) {
        atlas_decision_kind kind = atlas_decision_kind_at(i);
        if (atlas_decision_kind_resolvable(kind)) {
            continue;
        }
        atlas_buf uid = ATLAS_BUF_INIT;
        char title[128];
        (void)snprintf(title, sizeof title, "an approved %s", atlas_decision_kind_name(kind));
        propose_kind(&e, kind, title, &uid, &err);
        approve(&e, atlas_buf_cstr(&uid), &err);

        atlas_err rerr;
        atlas_err_init(&rerr);
        atlas_status st = try_challenge(&e, atlas_buf_cstr(&uid), ATLAS_DECISION_INTENT_RESOLVE,
                                       &rerr);
        T_CHECK_MSG(st != ATLAS_OK, "a %s could be resolved", atlas_decision_kind_name(kind));
        T_CHECK_MSG(strstr(atlas_err_msg(&rerr), atlas_decision_kind_name(kind)) != NULL,
                    "the refusal does not name the kind: %s", atlas_err_msg(&rerr));
        /* Still approved, and still that kind. */
        probe p;
        read_doc(&e, atlas_buf_cstr(&uid), &p);
        T_CHECK(strcmp(p.status, "APPROVED") == 0);
        T_CHECK(strcmp(p.kind, atlas_decision_kind_name(kind)) == 0);
        atlas_buf_free(&uid);
    }
    env_close(&e);
}

/* A resolution is an operator action and needs a capability, exactly as an
 * approval does. There is no path from a request body to this state. */
static void test_resolving_without_a_capability_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_OBLIGATION, "resolve the licensing question", &uid, &err);
    approve(&e, atlas_buf_cstr(&uid), &err);

    atlas_decision_result res;
    atlas_decision_result_init(&res);
    atlas_err nerr;
    atlas_err_init(&nerr);
    atlas_status st = spend(&e, ATLAS_DECISION_OP_RESOLVE, atlas_buf_cstr(&uid), "", "", &res,
                            &nerr);
    T_CHECK_MSG(st != ATLAS_OK, "a resolution happened with no capability");
    T_CHECK_MSG(strstr(atlas_err_msg(&nerr), "challenge") != NULL,
                "the refusal does not say what was missing: %s", atlas_err_msg(&nerr));
    atlas_decision_result_free(&res);

    /* An approval capability cannot be spent as a resolution either: the intent
     * is part of the bound tuple. */
    atlas_buf token = ATLAS_BUF_INIT;
    char confirm[ATLAS_DECISION_CONFIRM_MAX];
    atlas_buf second = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_OBLIGATION, "a second obligation", &second, &err);
    challenge_for(&e, atlas_buf_cstr(&second), ATLAS_DECISION_INTENT_APPROVE, &token, confirm,
                  &err);
    atlas_decision_result r2;
    atlas_decision_result_init(&r2);
    atlas_err ierr;
    atlas_err_init(&ierr);
    T_CHECK_MSG(spend(&e, ATLAS_DECISION_OP_RESOLVE, atlas_buf_cstr(&second),
                      atlas_buf_cstr(&token), confirm, &r2, &ierr) != ATLAS_OK,
                "an approval capability was spent as a resolution");
    atlas_decision_result_free(&r2);
    atlas_buf_free(&token);
    atlas_buf_free(&second);

    probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK(strcmp(p.status, "APPROVED") == 0);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- 4. a revision cannot reclassify -------------------------------------- */

static void test_a_revision_cannot_change_the_kind(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_POLICY, "no public release without fail-closed signing",
                 &uid, &err);

    /* A revise that asserts a different kind is refused, and the message points
     * at the honest remedy. */
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_REVISE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&uid), &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.title, "the same rule, reworded", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a different body", &err), &err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    op.knowledge_kind = ATLAS_DECISION_KIND_INVARIANT;
    op.knowledge_kind_given = true;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    atlas_err rerr;
    atlas_err_init(&rerr);
    atlas_status st = atlas_decision_apply(e.db, &op, &res, &rerr);
    T_CHECK_MSG(st != ATLAS_OK, "a revision reclassified a document");
    T_CHECK_MSG(strstr(atlas_err_msg(&rerr), "supersede") != NULL,
                "the refusal does not name the remedy: %s", atlas_err_msg(&rerr));
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);

    /* Nothing was written: still one revision, still a POLICY. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_revisions;"), 1);
    probe p;
    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK(strcmp(p.kind, "POLICY") == 0);

    /* A revise that says nothing about the kind is *not* an assertion, and must
     * still work — otherwise every client written before A9.1 would lose the
     * ability to revise anything that is not a decision. */
    atlas_decision_op ok_op;
    atlas_decision_op_init(&ok_op, ATLAS_DECISION_OP_REVISE);
    T_OK(atlas_buf_set_str(&ok_op.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ok_op.uid, atlas_buf_cstr(&uid), &err), &err);
    T_OK(atlas_buf_set_str(&ok_op.revision.title, "the same rule, reworded", &err), &err);
    T_OK(atlas_buf_set_str(&ok_op.revision.decision_text, "a different body", &err), &err);
    ok_op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result ok_res;
    atlas_decision_result_init(&ok_res);
    T_OK(atlas_decision_apply(e.db, &ok_op, &ok_res, &err), &err);
    T_EQ_INT((int)ok_res.revision_no, 2);
    T_CHECK_MSG(ok_res.knowledge_kind == ATLAS_DECISION_KIND_POLICY,
                "the revise result reported %s", atlas_decision_kind_name(ok_res.knowledge_kind));
    atlas_decision_result_free(&ok_res);
    atlas_decision_op_free(&ok_op);

    read_doc(&e, atlas_buf_cstr(&uid), &p);
    T_CHECK(strcmp(p.kind, "POLICY") == 0);

    /* And a revise that asserts the *same* kind is fine: it is agreement, not a
     * change. */
    atlas_decision_op same;
    atlas_decision_op_init(&same, ATLAS_DECISION_OP_REVISE);
    T_OK(atlas_buf_set_str(&same.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&same.uid, atlas_buf_cstr(&uid), &err), &err);
    T_OK(atlas_buf_set_str(&same.revision.title, "the rule, reworded again", &err), &err);
    T_OK(atlas_buf_set_str(&same.revision.decision_text, "a third body", &err), &err);
    same.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    same.knowledge_kind = ATLAS_DECISION_KIND_POLICY;
    same.knowledge_kind_given = true;
    atlas_decision_result r3;
    atlas_decision_result_init(&r3);
    T_OK(atlas_decision_apply(e.db, &same, &r3, &err), &err);
    T_EQ_INT((int)r3.revision_no, 3);
    atlas_decision_result_free(&r3);
    atlas_decision_op_free(&same);

    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- 5. filtering ---------------------------------------------------------- */

static void test_the_two_filters_are_independent(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf inv = ATLAS_BUF_INIT, risk = ATLAS_BUF_INIT, dec = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_INVARIANT, "the timestamp is inert and encoded as zero",
                 &inv, &err);
    propose_kind(&e, ATLAS_DECISION_KIND_ACCEPTED_RISK, "debug signing is accepted until release",
                 &risk, &err);
    propose_kind(&e, ATLAS_DECISION_KIND_DECISION, "the proof system is a batched STARK", &dec,
                 &err);
    approve(&e, atlas_buf_cstr(&inv), &err);
    approve(&e, atlas_buf_cstr(&dec), &err);

    struct {
        const char *status;
        const char *kind;
        int64_t want;
    } cases[] = {
        {NULL, NULL, 3},
        {NULL, "INVARIANT", 1},
        {NULL, "ACCEPTED_RISK", 1},
        {NULL, "OBLIGATION", 0},
        {"APPROVED", NULL, 2},
        {"PROPOSED", NULL, 1},
        /* The combination the dimension exists for. */
        {"APPROVED", "INVARIANT", 1},
        /* An approved accepted risk is a different thing from an approved
         * invariant, and this one has not been approved. */
        {"APPROVED", "ACCEPTED_RISK", 0},
        {"PROPOSED", "ACCEPTED_RISK", 1},
        {"RESOLVED", NULL, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        probe p;
        memset(&p, 0, sizeof p);
        int64_t n = 0;
        bool more = false;
        T_OK(atlas_db_decision_documents_list(e.db, e.repo_id, cases[i].status, cases[i].kind, 50,
                                              probe_cb, &p, &n, &more, &err),
             &err);
        T_CHECK_MSG(n == cases[i].want, "status=%s kind=%s returned %lld, expected %lld",
                    cases[i].status != NULL ? cases[i].status : "any",
                    cases[i].kind != NULL ? cases[i].kind : "any", (long long)n,
                    (long long)cases[i].want);
    }

    /* The repository totals are per axis and are not narrowed by a filter: they
     * are the denominator a filtered page is read against. */
    int64_t proposed = 0, approved_n = 0, rejected = 0, superseded = 0, resolved = 0;
    T_OK(atlas_db_decision_repo_counts(e.db, e.repo_id, &proposed, &approved_n, &rejected,
                                       &superseded, &resolved, &err),
         &err);
    T_EQ_INT((int)proposed, 1);
    T_EQ_INT((int)approved_n, 2);
    T_EQ_INT((int)resolved, 0);

    int64_t by_kind[ATLAS_DECISION_KIND_MAX];
    T_OK(atlas_db_decision_kind_counts(e.db, e.repo_id, by_kind,
                                       sizeof by_kind / sizeof by_kind[0], &err),
         &err);
    T_EQ_INT((int)by_kind[ATLAS_DECISION_KIND_INVARIANT], 1);
    T_EQ_INT((int)by_kind[ATLAS_DECISION_KIND_ACCEPTED_RISK], 1);
    T_EQ_INT((int)by_kind[ATLAS_DECISION_KIND_DECISION], 1);
    T_EQ_INT((int)by_kind[ATLAS_DECISION_KIND_OBLIGATION], 0);

    /* Search narrows by kind too, and by the same rule. */
    {
        probe p;
        memset(&p, 0, sizeof p);
        int64_t n = 0;
        bool more = false;
        T_OK(atlas_db_decision_search(e.db, e.repo_id, "timestamp", NULL, 50, probe_cb, &p, &n,
                                      &more, &err),
             &err);
        T_EQ_INT((int)n, 1);
        memset(&p, 0, sizeof p);
        n = 0;
        T_OK(atlas_db_decision_search(e.db, e.repo_id, "timestamp", "OBLIGATION", 50, probe_cb, &p,
                                      &n, &more, &err),
             &err);
        T_CHECK_MSG(n == 0, "a search filtered to the wrong kind returned %lld", (long long)n);
    }

    atlas_buf_free(&inv);
    atlas_buf_free(&risk);
    atlas_buf_free(&dec);
    env_close(&e);
}

/* --- 6. migration 13 ------------------------------------------------------- */

/* Every stored content hash, in row order. The one thing migration 13 must not
 * touch: `atlas doctor` rehashes every revision, and a digest that moved is
 * reported as tampering. */
static atlas_status hashes(atlas_db *db, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    sqlite3_stmt *q = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT id || '|' || content_hash || '|' || state FROM decision_revisions ORDER BY id;",
        &q, err);
    if (st != ATLAS_OK) {
        return st;
    }
    while (st == ATLAS_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(q, 0);
        st = atlas_buf_appendf(out, err, "%s\n", v != NULL ? v : "");
    }
    atlas_db_finish(db, q);
    return st;
}

static void test_a_schema_twelve_database_migrates_without_losing_a_row(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* A populated schema-12 database with all four pre-A9.1 states present, a
     * link, an edge account, alternatives and a consumed challenge — so the
     * preservation assertions are about rows rather than about empty tables
     * agreeing with each other. */
    atlas_buf approved_uid = ATLAS_BUF_INIT, rejected_uid = ATLAS_BUF_INIT;
    atlas_buf superseded_uid = ATLAS_BUF_INIT, proposed_uid = ATLAS_BUF_INIT;
    atlas_buf replacement_uid = ATLAS_BUF_INIT;

    /* An approved document with a path link and two alternatives. */
    {
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
        T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "an approved decision", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "with links and alternatives", &err),
             &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        T_OK(atlas_decision_revision_add_alternative(&op.revision, "the first alternative",
                                                     strlen("the first alternative"), &err),
             &err);
        T_OK(atlas_decision_revision_add_alternative(&op.revision, "the second alternative",
                                                     strlen("the second alternative"), &err),
             &err);
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        T_OK(atlas_buf_set_str(&l.path_raw, "src/core/service.c", &err), &err);
        T_OK(atlas_buf_set_str(&l.path_text, "src/core/service.c", &err), &err);
        T_OK(atlas_decision_revision_add_link(&op.revision, &l, &err), &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_OK(atlas_buf_set(&approved_uid, res.uid.data, res.uid.len, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
        approve(&e, atlas_buf_cstr(&approved_uid), &err);
    }
    propose_without_a_kind(&e, "a proposal nobody has acted on", &proposed_uid, &err);
    /* A rejected one. */
    {
        propose_without_a_kind(&e, "a refused proposal", &rejected_uid, &err);
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        challenge_for(&e, atlas_buf_cstr(&rejected_uid), ATLAS_DECISION_INTENT_REJECT, &token,
                      confirm, &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(spend(&e, ATLAS_DECISION_OP_REJECT, atlas_buf_cstr(&rejected_uid),
                   atlas_buf_cstr(&token), confirm, &res, &err),
             &err);
        atlas_decision_result_free(&res);
        atlas_buf_free(&token);
    }
    /* And a superseded one, with an edge account recorded about the relation. */
    {
        propose_without_a_kind(&e, "the record that was replaced", &superseded_uid, &err);
        approve(&e, atlas_buf_cstr(&superseded_uid), &err);
        propose_without_a_kind(&e, "the record that replaced it", &replacement_uid, &err);
        approve(&e, atlas_buf_cstr(&replacement_uid), &err);

        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_CHALLENGE);
        T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
        T_OK(atlas_buf_set_str(&op.uid, atlas_buf_cstr(&superseded_uid), &err), &err);
        T_OK(atlas_buf_set_str(&op.replacement_uid, atlas_buf_cstr(&replacement_uid), &err), &err);
        op.intent = ATLAS_DECISION_INTENT_SUPERSEDE;
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        T_OK(atlas_buf_set(&token, res.token.data, res.token.len, &err), &err);
        (void)snprintf(confirm, sizeof confirm, "%s", res.confirm);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);

        atlas_decision_result sres;
        atlas_decision_result_init(&sres);
        T_OK(spend(&e, ATLAS_DECISION_OP_SUPERSEDE, atlas_buf_cstr(&superseded_uid),
                   atlas_buf_cstr(&token), confirm, &sres, &err),
             &err);
        atlas_decision_result_free(&sres);
        atlas_buf_free(&token);

        /* An edge account. `decision_links.revision_id` declares ON DELETE
         * CASCADE and `decision_edge_events.revision_id` does too, so these are
         * the rows a rebuild of `decision_revisions` with foreign keys enabled
         * would have deleted silently. */
        atlas_decision_op note;
        atlas_decision_op_init(&note, ATLAS_DECISION_OP_EDGE_NOTE);
        T_OK(atlas_buf_set_str(&note.repo_name, "proj", &err), &err);
        T_OK(atlas_buf_set_str(&note.uid, atlas_buf_cstr(&approved_uid), &err), &err);
        T_OK(atlas_buf_set_str(&note.edge_target_uid, atlas_buf_cstr(&proposed_uid), &err), &err);
        T_OK(atlas_buf_set_str(&note.edge_event, "ANNOTATED", &err), &err);
        T_OK(atlas_buf_set_str(&note.edge_note, "why these two are related", &err), &err);
        T_OK(atlas_buf_set_str(&note.edge_provenance, "OPERATOR", &err), &err);
        atlas_decision_result nres;
        atlas_decision_result_init(&nres);
        T_OK(atlas_decision_apply(e.db, &note, &nres, &err), &err);
        atlas_decision_result_free(&nres);
        atlas_decision_op_free(&note);
    }

    /* Now make it a schema-12 database. The records above were written by the
     * real API, so their content hashes are Atlas' own. */
    wind_back_to_schema_12(&e, &err);

    /* The snapshot. Every table the rebuild touches or references. */
    static const char *const COUNTED[] = {
        "decision_documents",  "decision_revisions", "decision_events",
        "decision_challenges", "decision_links",     "decision_alternatives",
        "decision_search",     "decision_edge_events",
    };
    int64_t before_counts[sizeof COUNTED / sizeof COUNTED[0]];
    for (size_t i = 0; i < sizeof COUNTED / sizeof COUNTED[0]; i++) {
        char sql[128];
        (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s;", COUNTED[i]);
        before_counts[i] = count_of(&e, sql);
        T_CHECK_MSG(before_counts[i] > 0, "the fixture left %s empty", COUNTED[i]);
    }
    atlas_buf before = ATLAS_BUF_INIT;
    T_OK(hashes(e.db, &before, &err), &err);
    int64_t before_events_max = count_of(&e, "SELECT COALESCE(MAX(id), 0) FROM decision_events;");

    /* Migrate. */
    T_OK(atlas_db_migrate(e.db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);

    /* Nothing was lost. */
    for (size_t i = 0; i < sizeof COUNTED / sizeof COUNTED[0]; i++) {
        char sql[128];
        (void)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s;", COUNTED[i]);
        int64_t after = count_of(&e, sql);
        T_CHECK_MSG(after == before_counts[i], "migration 13 changed %s from %lld to %lld rows",
                    COUNTED[i], (long long)before_counts[i], (long long)after);
    }
    /* No row id and no content hash moved. */
    atlas_buf after = ATLAS_BUF_INIT;
    T_OK(hashes(e.db, &after, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&before), atlas_buf_cstr(&after)) == 0,
                "migration 13 moved a revision id, hash or state:\nbefore:\n%s\nafter:\n%s",
                atlas_buf_cstr(&before), atlas_buf_cstr(&after));

    /* Every migrated record is a DECISION. This is what backward compatibility
     * means here, rather than what approximates it. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_documents WHERE kind <> 'DECISION';"),
             0);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_documents WHERE kind IS NULL;"), 0);

    /* The ledger's AUTOINCREMENT sequence still covers every id it issued, so a
     * later event cannot take an id an existing one already has. */
    T_CHECK_MSG(count_of(&e, "SELECT COALESCE((SELECT seq FROM sqlite_sequence"
                             " WHERE name = 'decision_events'), 0);") >= before_events_max,
                "the events sequence was reset below the highest id it issued");

    /* Every index is back, by name. `idx_decision_rev_current` in particular: it
     * is the sole enforcement of at most one approved revision per document, and
     * losing it would break nothing until two revisions were effective at once. */
    static const char *const INDEXES[] = {
        "idx_decision_docs_repo",      "idx_decision_docs_status",
        "idx_decision_docs_root",      "idx_decision_docs_identity",
        "idx_decision_docs_kind",      "idx_decision_rev_doc",
        "idx_decision_rev_current",    "idx_decision_rev_import",
        "idx_decision_rev_dedup",      "idx_decision_events_doc",
        "idx_decision_events_rev",     "idx_decision_events_dedup",
        "idx_decision_challenges_repo",
    };
    for (size_t i = 0; i < sizeof INDEXES / sizeof INDEXES[0]; i++) {
        T_CHECK_MSG(index_exists(e.db, INDEXES[i]), "migration 13 did not recreate %s",
                    INDEXES[i]);
    }
    /* And the helper tables it used are gone. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM sqlite_schema WHERE name LIKE 'm13_%';"), 0);

    /* Foreign keys are enforced again, and nothing dangles. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM pragma_foreign_key_check;"), 0);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM pragma_foreign_keys;"), 1);

    /* Every document still reads exactly as it did, and every ledger replay still
     * agrees with its cached status. */
    int64_t checked = 0, mismatched = 0, rehashed = 0, corrupt = 0;
    T_OK(atlas_db_decision_verify_all(e.db, &checked, &mismatched, &rehashed, &corrupt, &err),
         &err);
    T_CHECK_MSG(checked == before_counts[0], "verify saw %lld of %lld documents",
                (long long)checked, (long long)before_counts[0]);
    T_CHECK_MSG(mismatched == 0, "%lld documents disagree with their ledger after migrating",
                (long long)mismatched);
    T_CHECK_MSG(corrupt == 0, "%lld revisions no longer hash to their stored digest",
                (long long)corrupt);

    /* The new vocabularies work on the migrated database, which is the point of
     * having widened the CHECK constraints at all. */
    atlas_buf ob = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_OBLIGATION, "work the migrated database now admits", &ob,
                 &err);
    approve(&e, atlas_buf_cstr(&ob), &err);
    resolve_it(&e, atlas_buf_cstr(&ob), &err);
    probe p;
    read_doc(&e, atlas_buf_cstr(&ob), &p);
    T_CHECK(strcmp(p.status, "RESOLVED") == 0);
    T_CHECK(strcmp(p.kind, "OBLIGATION") == 0);

    /* Idempotent as a set: migrating an up-to-date database is a no-op. */
    T_OK(atlas_db_migrate(e.db, &err), &err);
    T_EQ_INT(atlas_db_schema_version(e.db, &err), ATLAS_SCHEMA_VERSION);

    atlas_buf_free(&ob);
    atlas_buf_free(&before);
    atlas_buf_free(&after);
    atlas_buf_free(&approved_uid);
    atlas_buf_free(&rejected_uid);
    atlas_buf_free(&superseded_uid);
    atlas_buf_free(&proposed_uid);
    atlas_buf_free(&replacement_uid);
    env_close(&e);
}

/* A failed migration 13 leaves schema 12 exactly as it was — which for a
 * migration that rebuilds four tables is the assertion that matters most. The
 * failure is induced the way a real one would arrive: the object it is about to
 * create is already there. */
static void test_a_failed_migration_thirteen_leaves_twelve_untouched(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf uid = ATLAS_BUF_INIT;
    {
        /* A link, so the cascade that the fk-off flag exists for has something to
         * destroy if the rollback is not whole. */
        atlas_decision_op op;
        atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
        T_OK(atlas_buf_set_str(&op.repo_name, "proj", &err), &err);
        T_OK(atlas_buf_set_str(&op.revision.title, "a record that must survive a failure", &err),
             &err);
        T_OK(atlas_buf_set_str(&op.revision.decision_text, "with a link", &err), &err);
        op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
        atlas_decision_link l;
        atlas_decision_link_init(&l, ATLAS_DECISION_LINK_PATH);
        T_OK(atlas_buf_set_str(&l.path_raw, "src/db/migrate.c", &err), &err);
        T_OK(atlas_buf_set_str(&l.path_text, "src/db/migrate.c", &err), &err);
        T_OK(atlas_decision_revision_add_link(&op.revision, &l, &err), &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
        T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, &err), &err);
        atlas_decision_result_free(&res);
        atlas_decision_op_free(&op);
    }
    approve(&e, atlas_buf_cstr(&uid), &err);
    wind_back_to_schema_12(&e, &err);
    atlas_buf before = ATLAS_BUF_INIT;
    T_OK(hashes(e.db, &before, &err), &err);
    int64_t docs = count_of(&e, "SELECT COUNT(*) FROM decision_documents;");
    int64_t links = count_of(&e, "SELECT COUNT(*) FROM decision_links;");

    /* A table migration 13 creates, planted in advance. */
    T_OK(atlas_db_exec_sql(e.db, "CREATE TABLE m13_counts(x INTEGER);", &err), &err);

    atlas_err merr;
    atlas_err_init(&merr);
    T_CHECK_MSG(atlas_db_migrate(e.db, &merr) != ATLAS_OK,
                "migration 13 succeeded against an existing helper table");
    T_CHECK_MSG(strstr(atlas_err_msg(&merr), "rolled back") != NULL,
                "the failure did not report a rollback: %s", atlas_err_msg(&merr));

    /* Rolled back whole. */
    T_EQ_INT(atlas_db_schema_version(e.db, &err), 12);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_documents;"), (int)docs);
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM decision_links;"), (int)links);
    atlas_buf after = ATLAS_BUF_INIT;
    T_OK(hashes(e.db, &after, &err), &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&before), atlas_buf_cstr(&after)) == 0,
                "a failed migration 13 changed a revision");
    /* And foreign key enforcement was restored even though the migration failed:
     * a connection left with foreign keys off is one whose next write is
     * unchecked, and this connection goes on serving the process. */
    T_EQ_INT((int)count_of(&e, "SELECT COUNT(*) FROM pragma_foreign_keys;"), 1);

    atlas_buf_free(&before);
    atlas_buf_free(&after);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- 7. backup and restore ------------------------------------------------- */

/* The kind and the resolved state are ordinary columns, so they survive a backup
 * because a backup is a copy of the database — but "obviously" is not a test, and
 * `backup verify` re-runs the A4 record checks, which is where a schema the
 * verifier did not understand would surface. */
static void test_kind_and_resolution_survive_a_backup(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf ob = ATLAS_BUF_INIT, inv = ATLAS_BUF_INIT;
    propose_kind(&e, ATLAS_DECISION_KIND_OBLIGATION, "an obligation that was met", &ob, &err);
    approve(&e, atlas_buf_cstr(&ob), &err);
    resolve_it(&e, atlas_buf_cstr(&ob), &err);
    propose_kind(&e, ATLAS_DECISION_KIND_INVARIANT, "a property implementations must preserve",
                 &inv, &err);
    approve(&e, atlas_buf_cstr(&inv), &err);

    /* The backup is taken from the data directory rather than from the open
     * handle, and the handle is closed first: `atlas_service_backup_create` opens
     * the source read-only precisely so a running daemon can keep writing, but
     * this fixture's own writes must be committed before they can be copied. */
    atlas_buf dest = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&dest, &err, "%s/copy.db", fx_data_dir(&e.fx)), &err);
    atlas_backup_create_opts o;
    memset(&o, 0, sizeof o);
    o.output = atlas_buf_cstr(&dest);
    atlas_backup_report rep;
    atlas_backup_report_init(&rep);
    T_OK(atlas_service_backup_create(fx_data_dir(&e.fx), &o, &rep, &err), &err);
    T_EQ_INT(rep.schema_version, ATLAS_SCHEMA_VERSION);
    atlas_backup_report_free(&rep);

    atlas_backup_verify_report ver;
    atlas_backup_verify_report_init(&ver);
    T_OK(atlas_service_backup_verify(atlas_buf_cstr(&dest), &ver, &err), &err);
    T_CHECK_MSG(ver.ok, "the backup of a schema-13 database did not verify: %s",
                atlas_buf_cstr(&ver.problems));
    T_EQ_INT(ver.schema_version, ATLAS_SCHEMA_VERSION);
    /* The A4 record checks ran over the copy and found nothing wrong, which is
     * what says the verifier understands schema 13 rather than skipping it. */
    T_CHECK_MSG(ver.revisions_checked > 0, "the verifier checked no revisions");
    T_EQ_INT((int)ver.revisions_corrupt, 0);
    T_EQ_INT((int)ver.ledger_mismatched, 0);
    atlas_backup_verify_report_free(&ver);

    /* And the copy holds both dimensions. Opened read-only, which is also the
     * path that refuses a schema mismatch — so this asserts the verifier and the
     * reader agree about version 13. */
    atlas_db *copy = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&dest), &copy, &err), &err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(copy,
                              "SELECT COUNT(*) FROM decision_documents"
                              " WHERE kind = 'OBLIGATION' AND current_status = 'RESOLVED';",
                              &n, &err),
         &err);
    T_CHECK_MSG(n == 1, "the resolved obligation did not survive the backup");
    T_OK(atlas_db_query_int64(copy,
                              "SELECT COUNT(*) FROM decision_documents"
                              " WHERE kind = 'INVARIANT' AND current_status = 'APPROVED';",
                              &n, &err),
         &err);
    T_CHECK_MSG(n == 1, "the approved invariant did not survive the backup");
    T_OK(atlas_db_query_int64(copy, "SELECT COUNT(*) FROM decision_events WHERE event='RESOLVED';",
                             &n, &err),
         &err);
    T_CHECK_MSG(n == 1, "the resolution event did not survive the backup");
    atlas_db_close(copy);

    atlas_buf_free(&dest);
    atlas_buf_free(&ob);
    atlas_buf_free(&inv);
    env_close(&e);
}

/* Every lifecycle transition reports the kind of the record it changed.
 *
 * `propose` has always echoed it — `propose_kind` above asserts so. The five
 * operations that spend a capability did not: `atlas_decision_result` is zeroed
 * by its initialiser, `DECISION` is zero, and no transition path filled the
 * field in. So `atlas decision approve` printed `kind: DECISION` over an
 * APPROVED `INVARIANT` and over an APPROVED `OBLIGATION`, while the document,
 * `decision show`, `decision list` and the JSON surface all carried the right
 * one. `--json` is refused for the interactive commands, so the operator's only
 * view of what they had just approved was the wrong one.
 *
 * This is the hazard the zero-value rule exists for: a field nobody assigns is
 * not blank, it is `DECISION` — a confident wrong answer rather than a missing
 * one. A9.1's rule is that kind and status are orthogonal and every surface
 * reports both, and a surface reporting the *wrong* kind is worse than one
 * reporting neither.
 *
 * Asserted for every kind, so a kind added later without a transition answer is
 * a failure here rather than a wrong line on somebody's terminal. */
static void test_every_transition_reports_the_records_kind(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* `ATLAS_DECISION_KIND_MAX` is the number of kinds, not the last member. */
    for (unsigned k = 0; k < (unsigned)ATLAS_DECISION_KIND_MAX; k++) {
        atlas_decision_kind kind = (atlas_decision_kind)k;
        atlas_buf uid = ATLAS_BUF_INIT;
        char title[64];
        (void)snprintf(title, sizeof title, "transition kind %s",
                       atlas_decision_kind_name(kind));
        propose_kind(&e, kind, title, &uid, &err);

        /* Approve. */
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        challenge_for(&e, atlas_buf_cstr(&uid), ATLAS_DECISION_INTENT_APPROVE, &token, confirm,
                      &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(spend(&e, ATLAS_DECISION_OP_APPROVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                   confirm, &res, &err),
             &err);
        T_CHECK_MSG(res.knowledge_kind == kind, "approving a %s reported %s",
                    atlas_decision_kind_name(kind),
                    atlas_decision_kind_name(res.knowledge_kind));
        atlas_decision_result_free(&res);
        atlas_buf_free(&token);

        /* Resolve, for the kinds whose approved form makes a demand. */
        if (atlas_decision_kind_resolvable(kind)) {
            atlas_buf t2 = ATLAS_BUF_INIT;
            char c2[ATLAS_DECISION_CONFIRM_MAX];
            challenge_for(&e, atlas_buf_cstr(&uid), ATLAS_DECISION_INTENT_RESOLVE, &t2, c2, &err);
            atlas_decision_result r2;
            atlas_decision_result_init(&r2);
            T_OK(spend(&e, ATLAS_DECISION_OP_RESOLVE, atlas_buf_cstr(&uid), atlas_buf_cstr(&t2),
                       c2, &r2, &err),
                 &err);
            T_CHECK_MSG(r2.knowledge_kind == kind, "resolving a %s reported %s",
                        atlas_decision_kind_name(kind),
                        atlas_decision_kind_name(r2.knowledge_kind));
            atlas_decision_result_free(&r2);
            atlas_buf_free(&t2);
        }
        atlas_buf_free(&uid);
    }

    /* Rejection takes the same path and must answer the same way. */
    {
        atlas_buf uid = ATLAS_BUF_INIT;
        propose_kind(&e, ATLAS_DECISION_KIND_ACCEPTED_RISK, "a risk to refuse", &uid, &err);
        atlas_buf token = ATLAS_BUF_INIT;
        char confirm[ATLAS_DECISION_CONFIRM_MAX];
        challenge_for(&e, atlas_buf_cstr(&uid), ATLAS_DECISION_INTENT_REJECT, &token, confirm,
                      &err);
        atlas_decision_result res;
        atlas_decision_result_init(&res);
        T_OK(spend(&e, ATLAS_DECISION_OP_REJECT, atlas_buf_cstr(&uid), atlas_buf_cstr(&token),
                   confirm, &res, &err),
             &err);
        T_CHECK_MSG(res.knowledge_kind == ATLAS_DECISION_KIND_ACCEPTED_RISK,
                    "rejecting an ACCEPTED_RISK reported %s",
                    atlas_decision_kind_name(res.knowledge_kind));
        atlas_decision_result_free(&res);
        atlas_buf_free(&token);
        atlas_buf_free(&uid);
    }

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"the kind vocabulary is closed and DECISION is zero",
     test_the_kind_vocabulary_is_closed_and_decision_is_zero},
    {"every transition reports the record's kind",
     test_every_transition_reports_the_records_kind},
    {"every kind can be created and reads back",
     test_every_kind_can_be_created_and_reads_back},
    {"the schema refuses a kind outside the vocabulary",
     test_the_schema_refuses_a_kind_outside_the_vocabulary},
    {"an obligation can be resolved and stays resolved",
     test_an_obligation_can_be_resolved_and_stays_resolved},
    {"a kind that makes no demand cannot be resolved",
     test_a_kind_that_makes_no_demand_cannot_be_resolved},
    {"resolving without a capability is refused",
     test_resolving_without_a_capability_is_refused},
    {"a revision cannot change the kind", test_a_revision_cannot_change_the_kind},
    {"the two filters are independent", test_the_two_filters_are_independent},
    {"a schema-twelve database migrates without losing a row",
     test_a_schema_twelve_database_migrates_without_losing_a_row},
    {"a failed migration thirteen leaves twelve untouched",
     test_a_failed_migration_thirteen_leaves_twelve_untouched},
    {"kind and resolution survive a backup", test_kind_and_resolution_survive_a_backup},
};

ATLAS_TEST_MAIN("decision_kind", TESTS)

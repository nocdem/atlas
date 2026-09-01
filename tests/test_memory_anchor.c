/* Atlas - A12.1 T7: anchor resolution, the extractor's impure half.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_memory_anchor_resolve` reads the index -- `files`, the compiler-
 * derived semantic index, the decision store and `commits` -- so unlike
 * `tests/test_memory_extract.c` this suite needs a real database with real
 * rows in it, in exactly the shape `tests/test_verify_engine.c` and
 * `tests/test_verify_absence.c` build theirs: a fixture database with a
 * registered repository and no git repository behind it at all, because
 * every read under test is a plain indexed lookup and none of them touches
 * the tree.
 */
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/memory.h"
#include "atlas/verify.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

#define EXEC(e, err, ...)                                                                          \
    do {                                                                                           \
        char sql_[4096];                                                                           \
        int wrote_ = snprintf(sql_, sizeof sql_, __VA_ARGS__);                                      \
        T_REQUIRE(wrote_ > 0 && (size_t)wrote_ < sizeof sql_);                                      \
        T_OK(atlas_db_exec_sql((e)->db, sql_, (err)), (err));                                       \
    } while (0)

static int64_t last_id(env *e, const char *table, atlas_err *err) {
    char sql[128];
    (void)snprintf(sql, sizeof sql, "SELECT COALESCE(MAX(id), 0) FROM %s;", table);
    sqlite3_stmt *stmt = NULL;
    int64_t id = 0;
    T_OK(atlas_db_prepare(e->db, sql, &stmt, err), err);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    atlas_db_finish(e->db, stmt);
    return id;
}

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-memory-anchor-repo";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-memory-anchor-repo/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    T_OK(atlas_db_repo_add(e->db, "proj", &id, &e->repo_id, err), err);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* A file in the index, so a `path_text` backtick token has something to
 * resolve against -- `tests/test_verify_engine.c`'s seed_file, reused rather
 * than restated. */
static void seed_file(env *e, const char *path, const char *hash, atlas_err *err) {
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, err,
                           "INSERT INTO scans(repo_id, started_at, status)"
                           "  VALUES(%lld, '2026-01-01T00:00:00Z', 'ok');"
                           "INSERT INTO files(repo_id, path_raw, path_text, file_type,"
                           "  content_hash, first_seen_scan_id, last_seen_scan_id,"
                           "  first_seen_at, last_seen_at)"
                           "  VALUES(%lld, CAST('%s' AS BLOB), '%s', 'regular', '%s',"
                           "         last_insert_rowid(), last_insert_rowid(),"
                           "         '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');",
                           (long long)e->repo_id, (long long)e->repo_id, path, path, hash),
         err);
    T_OK(atlas_db_exec_sql(e->db, atlas_buf_cstr(&sql), err), err);
    atlas_buf_free(&sql);
}

/* A minimal COMPLETE semantic generation carrying one symbol -- everything
 * `atlas_db_verify_sem_symbol` needs and nothing `atlas.symbol_present` itself
 * would need beyond that, since resolve() only asks whether the count is
 * positive. */
static int64_t seed_symbol(env *e, const char *name, atlas_err *err) {
    EXEC(e, err,
        "INSERT INTO sem_generations(repo_id, status, started_at) VALUES(%lld, 'COMPLETE',"
        " '2026-01-01T00:00:00Z');",
        (long long)e->repo_id);
    int64_t gen = last_id(e, "sem_generations", err);
    EXEC(e, err, "INSERT INTO sem_current(repo_id, generation_id) VALUES(%lld, %lld);",
        (long long)e->repo_id, (long long)gen);
    EXEC(e, err,
        "INSERT INTO sem_symbols(generation_id, usr, name, external)"
        " VALUES(%lld, 'usr-%s', '%s', 0);",
        (long long)gen, name, name);
    return gen;
}

static void seed_commit(env *e, const char *oid, atlas_err *err) {
    EXEC(e, err, "INSERT INTO commits(repo_id, oid, parent_count) VALUES(%lld, '%s', 0);",
        (long long)e->repo_id, oid);
}

/* A proposed decision document, left PROPOSED -- resolve() only asks whether
 * the uid exists and belongs to this repository, not what its lifecycle
 * status is. */
static void propose_decision(env *e, atlas_buf *uid_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    op.knowledge_kind = ATLAS_DECISION_KIND_DECISION;
    op.knowledge_kind_given = true;
    T_OK(atlas_buf_set_str(&op.revision.title, "a fixture decision", err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a body for the fixture", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);
}

static void make_prop(atlas_memory_proposition *p, const char *text, atlas_err *err) {
    atlas_memory_proposition_init(p);
    T_OK(atlas_buf_set_str(&p->text, text, err), err);
}

/* --- PATH -------------------------------------------------------------- */

static void test_a_backtick_path_resolves_to_path(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_file(&e, "src/db/db_orch.c", "aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11",
              &err);

    atlas_memory_proposition p;
    make_prop(&p, "the daemon reads `src/db/db_orch.c` before it decides anything", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_PATH);
    T_EQ_STR(atlas_buf_cstr(&p.anchors[0].value), "src/db/db_orch.c");
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_CONTENT_HASH);
    T_CHECK(strstr(atlas_buf_cstr(&p.verifier_input), "path=src/db/db_orch.c;sha256=") != NULL);
    T_CHECK(p.decision_uid.len == 0);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

static void test_a_nonexistent_path_resolves_no_anchor(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    atlas_memory_proposition p;
    make_prop(&p, "see `no/such/file.c` for the rest", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, 0);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

static void test_pure_prose_resolves_no_anchor(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    atlas_memory_proposition p;
    make_prop(&p, "the daemon writes nothing to a registered tree, ever", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, 0);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);
    T_CHECK(p.decision_uid.len == 0);
    T_CHECK(p.verifier_input.len == 0);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

/* --- SYMBOL -------------------------------------------------------------- */

static void test_a_known_symbol_resolves_to_symbol_present(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_symbol(&e, "atlas_verify_run_verifier", &err);

    atlas_memory_proposition p;
    make_prop(&p, "the check runs through `atlas_verify_run_verifier` and nothing else", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_SYMBOL);
    T_EQ_STR(atlas_buf_cstr(&p.anchors[0].value), "atlas_verify_run_verifier");
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_SYMBOL_PRESENT);
    T_EQ_STR(atlas_buf_cstr(&p.verifier_input), "symbol=atlas_verify_run_verifier");

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

static void test_a_token_that_is_both_path_and_symbol_gets_both_anchors(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_file(&e, "helper", "bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11bb11", &err);
    seed_symbol(&e, "helper", &err);

    atlas_memory_proposition p;
    make_prop(&p, "call `helper` before anything else", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 2);
    bool saw_path = false, saw_symbol = false;
    for (size_t i = 0; i < p.anchor_count; i++) {
        if (p.anchors[i].kind == ATLAS_MEMORY_ANCHOR_PATH) saw_path = true;
        if (p.anchors[i].kind == ATLAS_MEMORY_ANCHOR_SYMBOL) saw_symbol = true;
    }
    T_CHECK(saw_path);
    T_CHECK(saw_symbol);
    /* Decision 4: SYMBOL present wins the claim's own semantics/verifier. */
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_SYMBOL_PRESENT);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

/* --- DECISION -------------------------------------------------------------- */

static void test_a_decision_uid_alone_is_normative(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose_decision(&e, &uid, &err);

    char text[256];
    (void)snprintf(text, sizeof text, "this rule is recorded in %s and nowhere else",
                   atlas_buf_cstr(&uid));
    atlas_memory_proposition p;
    make_prop(&p, text, &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_DECISION);
    T_EQ_STR(atlas_buf_cstr(&p.anchors[0].value), atlas_buf_cstr(&uid));
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_NORMATIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);
    T_EQ_STR(atlas_buf_cstr(&p.decision_uid), atlas_buf_cstr(&uid));

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void test_a_decision_plus_a_path_is_descriptive_with_decision_uid_set(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_file(&e, "src/verify/intake.c",
              "cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11cc11", &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    propose_decision(&e, &uid, &err);

    char text[256];
    (void)snprintf(text, sizeof text, "%s is enforced by `src/verify/intake.c`",
                   atlas_buf_cstr(&uid));
    atlas_memory_proposition p;
    make_prop(&p, text, &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 2);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_CONTENT_HASH);
    T_EQ_STR(atlas_buf_cstr(&p.decision_uid), atlas_buf_cstr(&uid));

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- COMMIT -------------------------------------------------------------- */

static void test_a_bare_commit_oid_resolves_to_commit(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    static const char OID[] = "dddddddddddddddddddddddddddddddddddddddd";
    seed_commit(&e, OID, &err);

    char text[128];
    (void)snprintf(text, sizeof text, "first indexed at %s", OID);
    atlas_memory_proposition p;
    make_prop(&p, text, &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_COMMIT);
    T_EQ_STR(atlas_buf_cstr(&p.anchors[0].value), OID);
    /* COMMIT alone carries no dedicated verifier -- Decision 4 does not name
     * one, so it falls to the same DESCRIPTIVE/NONE default an unanchored
     * proposition would carry, the one difference being anchor_count > 0. */
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

static void test_an_unindexed_commit_oid_resolves_no_anchor(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    atlas_memory_proposition p;
    make_prop(&p, "not yet indexed: eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, 0);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

/* --- the bound ------------------------------------------------------------- */

static void test_a_ninth_anchor_is_dropped_with_the_count_capped(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    atlas_buf text = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&text, "anchors: ", &err), &err);
    for (int i = 0; i < 7; i++) {
        char path[32], hash[65];
        (void)snprintf(path, sizeof path, "src/f%d.c", i);
        for (int k = 0; k < 64; k++) {
            hash[k] = (char)('0' + (k + i) % 10);
        }
        hash[64] = '\0';
        seed_file(&e, path, hash, &err);
        char tok[48];
        (void)snprintf(tok, sizeof tok, "`%s` ", path);
        T_OK(atlas_buf_append_str(&text, tok, &err), &err);
    }
    atlas_buf uid = ATLAS_BUF_INIT;
    propose_decision(&e, &uid, &err);
    T_OK(atlas_buf_append_str(&text, atlas_buf_cstr(&uid), &err), &err);
    T_OK(atlas_buf_append_str(&text, " ", &err), &err);
    static const char OID[] = "1234567890123456789012345678901234567890";
    seed_commit(&e, OID, &err);
    T_OK(atlas_buf_append_str(&text, OID, &err), &err);

    /* 7 distinct resolving PATH tokens + 1 resolving DECISION token + 1
     * resolving COMMIT token = 9 candidates that would each resolve, in that
     * left-to-right order. The ceiling is 8. */
    atlas_memory_proposition p;
    atlas_memory_proposition_init(&p);
    T_OK(atlas_buf_set(&p.text, text.data, text.len, &err), &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, (int)ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION);
    bool saw_commit = false;
    for (size_t i = 0; i < p.anchor_count; i++) {
        if (p.anchors[i].kind == ATLAS_MEMORY_ANCHOR_COMMIT) {
            saw_commit = true;
        }
    }
    T_CHECK_MSG(!saw_commit, "the 9th candidate anchor was recorded instead of dropped");

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&text);
    atlas_buf_free(&uid);
    env_close(&e);
}

static void hex_hash(char out[65], int salt) {
    for (int k = 0; k < 64; k++) {
        out[k] = (char)('0' + (k + salt) % 10);
    }
    out[64] = '\0';
}

/* The out-of-bounds write the ordinary "ninth anchor" test above cannot
 * reach: that test's ninth candidate arrives in its own fresh iteration of
 * the outer scan loop, which already refuses to enter it once anchor_count
 * hits the cap. The loop's own guard is checked once per *token*, not once
 * per anchor, so a single backtick token that resolves as both a path and a
 * symbol makes two add_anchor() calls inside one iteration -- and at
 * anchor_count == cap - 1, the first call fills the last slot and the
 * second must be refused by add_anchor's own check, not the loop's. */
static void test_a_dual_resolving_token_at_the_cap_is_not_written_out_of_bounds(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    atlas_buf text = ATLAS_BUF_INIT;
    for (int i = 0; i < 5; i++) {
        char path[32], hash[65];
        (void)snprintf(path, sizeof path, "src/g%d.c", i);
        hex_hash(hash, i);
        seed_file(&e, path, hash, &err);
        char tok[48];
        (void)snprintf(tok, sizeof tok, "`%s` ", path);
        T_OK(atlas_buf_append_str(&text, tok, &err), &err);
    }
    atlas_buf uid = ATLAS_BUF_INIT;
    propose_decision(&e, &uid, &err);
    T_OK(atlas_buf_append_str(&text, atlas_buf_cstr(&uid), &err), &err);
    T_OK(atlas_buf_append_str(&text, " ", &err), &err);
    static const char OID[] = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";
    seed_commit(&e, OID, &err);
    T_OK(atlas_buf_append_str(&text, OID, &err), &err);
    T_OK(atlas_buf_append_str(&text, " ", &err), &err);

    /* anchor_count == 7 at this point: 5 PATH + 1 DECISION + 1 COMMIT. The
     * 8th candidate resolves as both PATH and SYMBOL from one token. */
    char dual_hash[65];
    hex_hash(dual_hash, 9);
    seed_file(&e, "dual", dual_hash, &err);
    seed_symbol(&e, "dual", &err);
    T_OK(atlas_buf_append_str(&text, "`dual`", &err), &err);

    atlas_memory_proposition p;
    atlas_memory_proposition_init(&p);
    T_OK(atlas_buf_set(&p.text, text.data, text.len, &err), &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, (int)ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION);
    /* The PATH half of the dual token took the 8th slot; the SYMBOL half
     * could not fit, and must not be counted as though it had -- that is
     * what add_anchor's return value, not just its bounds check, protects:
     * a phantom SYMBOL_PRESENT verdict for an anchor that was never
     * actually written would be its own version of C1's mismatch. */
    bool saw_symbol = false;
    for (size_t i = 0; i < p.anchor_count; i++) {
        if (p.anchors[i].kind == ATLAS_MEMORY_ANCHOR_SYMBOL) {
            saw_symbol = true;
        }
    }
    T_CHECK_MSG(!saw_symbol, "a symbol anchor that could not fit was recorded anyway");
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_CONTENT_HASH);

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&text);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* --- adversarial: a filename cannot forge a verifier's own verdict --------- */

static void test_a_semicolon_in_a_path_cannot_forge_a_verifier_input(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    /* An ordinary, legal POSIX filename -- untrusted input by CLAUDE.md's
     * own rule, chosen by whoever can commit -- built to look like a second
     * `sha256=` field inside the verifier's own `path=`/`sha256=` grammar.
     * Its real content hash does not matter to the attack. */
    static const char EVIL[] =
        "a;sha256=0000000000000000000000000000000000000000000000000000000000000000";
    seed_file(&e, EVIL, "cafefeedcafefeedcafefeedcafefeedcafefeedcafefeedcafefeedcafefeed", &err);

    char text[256];
    (void)snprintf(text, sizeof text, "reads `%s` before it decides anything", EVIL);
    atlas_memory_proposition p;
    make_prop(&p, text, &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    /* The fact is still honestly recorded. */
    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_PATH);
    T_EQ_STR(atlas_buf_cstr(&p.anchors[0].value), EVIL);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);

    /* But no verifier is built from it: `input_field` (src/verify/
     * detverify.c) takes the *first* `sha256=` match in a `;`-split
     * grammar with no escaping, so a naively built input would compare the
     * real file against the sixty-four zeros embedded in its own name
     * rather than against the real hash appended after it -- a filename
     * forging Atlas' own deterministic verdict. Refusing to build the
     * verifier at all is what stops that, while still keeping the claim
     * DESCRIPTIVE and anchored rather than vanished. */
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);
    T_CHECK(p.verifier_input.len == 0);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

/* The same attack, but the malicious `files` row comes from the real
 * scanner rather than a hand-written INSERT: a real git repository, added
 * and scanned through the built `atlas` binary exactly as an operator
 * would. `files.content_hash` and `files.path_text` are Atlas' own, not a
 * fixture's guess at their shape. */
static void test_an_adversarial_filename_scanned_for_real_cannot_forge_a_verdict(void) {
    fixture fx;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&fx, &err), &err);
    const char *repo_dir = fx_repo(&fx);
    T_OK(fx_init_repo(&fx, repo_dir, NULL, &err), &err);

    static const char EVIL[] =
        "a;sha256=0000000000000000000000000000000000000000000000000000000000000000";
    T_OK(fx_write(repo_dir, "a", "the real file\n", &err), &err);
    if (!fx_can_create_name(repo_dir, EVIL, strlen(EVIL))) {
        atlas_test_note("this filesystem refused a ';' filename; skipping the real-scan case");
        fx_close(&fx);
        return;
    }
    T_OK(fx_write(repo_dir, EVIL, "irrelevant\n", &err), &err);
    T_OK(fx_add_all(&fx, repo_dir, &err), &err);
    T_OK(fx_commit(&fx, repo_dir, "an adversarial filename", &err), &err);

    const char *add[] = {"--data-dir", fx_data_dir(&fx), "repo", "add",
                         repo_dir,     "--name",         "evilproj"};
    int code = -1;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
    T_REQUIRE(code == 0);

    const char *scan[] = {"--data-dir", fx_data_dir(&fx), "scan", "evilproj"};
    code = -1;
    T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
    T_REQUIRE(code == 0);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(db, "evilproj", &info, &found, &err), &err);
    T_REQUIRE(found);

    char text[256];
    (void)snprintf(text, sizeof text, "reads `%s` before it decides anything", EVIL);
    atlas_memory_proposition p;
    make_prop(&p, text, &err);
    T_OK(atlas_memory_anchor_resolve(db, info.id, &p, &err), &err);

    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_PATH);
    T_EQ_STR(atlas_buf_cstr(&p.anchors[0].value), EVIL);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    /* No verifier, and therefore no verdict a filename could have steered:
     * a real scan of a real repository holding this name still cannot
     * produce a CONTENT_HASH check against the wrong sixty-four zeros. */
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);
    T_CHECK(p.verifier_input.len == 0);

    atlas_memory_proposition_free(&p);
    atlas_repo_info_free(&info);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
    fx_close(&fx);
}

static void test_an_embedded_nul_token_resolves_no_anchor(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_file(&e, "src/a.c",
              "1111111111111111111111111111111111111111111111111111111111111111", &err);

    /* `` `src/a.c<NUL>zzz` `` -- the NUL-terminated C-string lookup would
     * silently truncate at the NUL and match the real "src/a.c", while the
     * anchor's own `value` (a byte range, not a C string) would otherwise
     * carry the full span including "zzz". The two representations must
     * never be allowed to disagree, so a token carrying a raw NUL is
     * refused outright rather than resolved against its truncated prefix. */
    atlas_buf text = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&text, "reads `", &err), &err);
    T_OK(atlas_buf_append_str(&text, "src/a.c", &err), &err);
    T_OK(atlas_buf_append_ch(&text, '\0', &err), &err);
    T_OK(atlas_buf_append_str(&text, "zzz` here", &err), &err);

    atlas_memory_proposition p;
    atlas_memory_proposition_init(&p);
    T_OK(atlas_buf_set(&p.text, text.data, text.len, &err), &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, 0);

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&text);
    env_close(&e);
}

static void test_an_oversized_verifier_input_falls_back_to_no_verifier(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    /* An ordinary, safe path -- no `;`, nothing adversarial -- just long
     * enough that "path=<value>;sha256=<64 hex>" alone exceeds
     * ATLAS_VERIFY_VERIFIER_INPUT_MAX, so the only thing under test is the
     * length refusal. */
    atlas_buf long_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set_str(&long_path, "src/", &err), &err);
    while (long_path.len < ATLAS_VERIFY_VERIFIER_INPUT_MAX) {
        T_OK(atlas_buf_append_str(&long_path, "aaaaaaaaaa", &err), &err);
    }
    T_OK(atlas_buf_append_str(&long_path, ".c", &err), &err);
    seed_file(&e, atlas_buf_cstr(&long_path),
              "2222222222222222222222222222222222222222222222222222222222222222", &err);

    atlas_buf text = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&text, "reads `", &err), &err);
    T_OK(atlas_buf_append(&text, long_path.data, long_path.len, &err), &err);
    T_OK(atlas_buf_append_str(&text, "` here", &err), &err);

    atlas_memory_proposition p;
    atlas_memory_proposition_init(&p);
    T_OK(atlas_buf_set(&p.text, text.data, text.len, &err), &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    /* Still anchored -- the anchor's own value has no length bound tied to
     * the verifier's grammar -- but not verified: a claim intake would
     * refuse this input anyway, and leaving the verifier set would make the
     * proposition become a claim that could never be submitted and was
     * never recorded as unanchored either (A9.2.2's failure mode). */
    T_REQUIRE(p.anchor_count == 1);
    T_EQ_INT((int)p.anchors[0].kind, (int)ATLAS_MEMORY_ANCHOR_PATH);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);
    T_CHECK(p.verifier_input.len == 0);

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&text);
    atlas_buf_free(&long_path);
    env_close(&e);
}

static void test_a_truncated_proposition_skips_resolution(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_file(&e, "src/z.c",
              "3333333333333333333333333333333333333333333333333333333333333333", &err);

    atlas_memory_proposition p;
    make_prop(&p, "reads `src/z.c` here", &err);
    /* Stands in for what atlas_memory_extract sets on a candidate over
     * ATLAS_MEMORY_MAX_PROPOSITION_BYTES, without needing a 2 KiB+ fixture
     * text: resolve() must refuse to scan it at all, however resolvable the
     * text would otherwise be. */
    p.truncated = true;

    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, 0);
    T_EQ_INT((int)p.semantics, (int)ATLAS_CLAIM_DESCRIPTIVE);
    T_EQ_INT((int)p.verifier, (int)ATLAS_VERIFIER_NONE);

    atlas_memory_proposition_free(&p);
    env_close(&e);
}

/* --- idempotence and cross-repository scoping ------------------------------- */

static void test_resolve_is_idempotent(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    seed_file(&e, "src/db/db_orch.c", "aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11aa11",
              &err);

    atlas_memory_proposition p;
    make_prop(&p, "reads `src/db/db_orch.c`", &err);
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);
    T_REQUIRE(p.anchor_count == 1);
    atlas_buf first_input = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&first_input, p.verifier_input.data, p.verifier_input.len, &err), &err);

    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);
    T_REQUIRE(p.anchor_count == 1);
    T_EQ_MEM(p.verifier_input.data, p.verifier_input.len, first_input.data, first_input.len);

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&first_input);
    env_close(&e);
}

static void test_a_decision_from_another_repository_does_not_resolve(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);

    atlas_repo_identity id;
    memset(&id, 0, sizeof id);
    id.root = "/tmp/atlas-memory-anchor-other";
    id.root_len = strlen(id.root);
    id.common_dir = "/tmp/atlas-memory-anchor-other/.git";
    id.common_dir_len = strlen(id.common_dir);
    id.git_dir = id.common_dir;
    id.git_dir_len = id.common_dir_len;
    id.object_format = "sha1";
    int64_t other_repo_id = 0;
    T_OK(atlas_db_repo_add(e.db, "other", &id, &other_repo_id, &err), &err);

    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_buf_set_str(&op.repo_name, "other", &err), &err);
    op.knowledge_kind = ATLAS_DECISION_KIND_DECISION;
    op.knowledge_kind_given = true;
    T_OK(atlas_buf_set_str(&op.revision.title, "belongs to the other repository", &err), &err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "body", &err), &err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    T_OK(atlas_decision_apply(e.db, &op, &res, &err), &err);
    atlas_buf uid = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&uid, res.uid.data, res.uid.len, &err), &err);
    atlas_decision_op_free(&op);
    atlas_decision_result_free(&res);

    char text[256];
    (void)snprintf(text, sizeof text, "see %s", atlas_buf_cstr(&uid));
    atlas_memory_proposition p;
    make_prop(&p, text, &err);
    /* Resolved against e.repo_id ("proj"), not the repository the decision
     * actually belongs to. */
    T_OK(atlas_memory_anchor_resolve(e.db, e.repo_id, &p, &err), &err);

    T_EQ_INT((int)p.anchor_count, 0);
    T_CHECK(p.decision_uid.len == 0);

    atlas_memory_proposition_free(&p);
    atlas_buf_free(&uid);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a backtick path resolves to PATH", test_a_backtick_path_resolves_to_path},
    {"a nonexistent path resolves no anchor", test_a_nonexistent_path_resolves_no_anchor},
    {"pure prose resolves no anchor", test_pure_prose_resolves_no_anchor},
    {"a known symbol resolves to SYMBOL_PRESENT", test_a_known_symbol_resolves_to_symbol_present},
    {"a token that is both a path and a symbol gets both anchors",
     test_a_token_that_is_both_path_and_symbol_gets_both_anchors},
    {"a decision uid alone is NORMATIVE", test_a_decision_uid_alone_is_normative},
    {"a decision plus a path is DESCRIPTIVE with decision_uid set",
     test_a_decision_plus_a_path_is_descriptive_with_decision_uid_set},
    {"a bare commit oid resolves to COMMIT", test_a_bare_commit_oid_resolves_to_commit},
    {"an unindexed commit oid resolves no anchor", test_an_unindexed_commit_oid_resolves_no_anchor},
    {"a ninth anchor is dropped with the count capped",
     test_a_ninth_anchor_is_dropped_with_the_count_capped},
    {"a dual-resolving token at the cap is not written out of bounds",
     test_a_dual_resolving_token_at_the_cap_is_not_written_out_of_bounds},
    {"a semicolon in a path cannot forge a verifier input",
     test_a_semicolon_in_a_path_cannot_forge_a_verifier_input},
    {"an adversarial filename scanned for real cannot forge a verdict",
     test_an_adversarial_filename_scanned_for_real_cannot_forge_a_verdict},
    {"an embedded NUL token resolves no anchor", test_an_embedded_nul_token_resolves_no_anchor},
    {"an oversized verifier input falls back to no verifier",
     test_an_oversized_verifier_input_falls_back_to_no_verifier},
    {"a truncated proposition skips resolution", test_a_truncated_proposition_skips_resolution},
    {"resolve is idempotent", test_resolve_is_idempotent},
    {"a decision from another repository does not resolve",
     test_a_decision_from_another_repository_does_not_resolve},
};

ATLAS_TEST_MAIN("memory_anchor", TESTS)

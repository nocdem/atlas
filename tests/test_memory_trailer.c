/* Atlas - A12.1 T14: commit trailers -- composed for a person, ingested from
 * the index.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every case here builds a real git repository through the fixture, registers
 * and scans it through the built binary (never `atlas_db_repo_add` by hand --
 * a trailer lives in `commits.body`, which only a real scan populates), and
 * drives `atlas_orch_apply` directly for a root SUBMIT the way
 * `test_orch_run.c` does -- the writer-thread seam that freezes a pack as
 * part of a live SUBMIT (`run_orch`, T13) is daemon-only and out of reach for
 * an "integration"-labelled suite, so a pack is built and frozen directly
 * with `atlas_memory_pack_build`/`_freeze_in_tx`, `test_memory_pack.c`'s own
 * shape one layer over a real orchestration run rather than a synthetic
 * run_uid string.
 *
 * (a) is the round trip: compose, paste into a real commit through the
 * fixture's own git, reconcile, and read the stored binding back. (b) is
 * four independent tamperings, each landing exactly the field(s) that
 * structurally depend on the tampered value in `unknown_fields` -- not
 * always only one: an unknown run uid also takes the two digests and the
 * generation with it, because none of those three can be checked without
 * first finding the pack the (nonexistent) run would have frozen, and that
 * containment is asserted through `change_reason`, the one field with no
 * such dependency. (c) is a commit with no block at all, asserted against
 * (b)'s shape: `has_block` false and `unknown_fields` genuinely empty, never
 * a list of five names. (d) is the season's central claim for this task: a
 * tampered block moves no decision status and no verification-table row
 * count, captured before and after. (e) is the composer's own refusal.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/memory.h"
#include "atlas/orch_ops.h"
#include "atlas/syspolicy.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- environment ------------------------------------------------------------
 *
 * `test_orch_run.c`'s own shape: registered and scanned through the built
 * binary, never `atlas_db_repo_add` by hand, because this task's whole point
 * is a real `commits.body`. */
typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    int64_t repo_id;
} env;

static void env_rescan(env *e, atlas_err *err) {
    {
        const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "proj"};
        int code = -1;
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, err), err);
        T_REQUIRE(code == 0);
    }
    if (e->db != NULL) {
        atlas_db_close(e->db);
        e->db = NULL;
    }
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", &ri, &found, err), err);
    T_REQUIRE(found);
    e->repo_id = ri.id;
    atlas_repo_info_free(&ri);
}

static void env_open(env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->db_path);
    T_OK(fx_open(&e->fx, err), err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), err), err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", err), err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx),  "--name",         "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, err), err);
        T_REQUIRE(code == 0);
    }
    T_OK(atlas_buf_appendf(&e->db_path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    env_rescan(e, err);
}

static void env_close(env *e) {
    if (e->db != NULL) {
        atlas_db_close(e->db);
        e->db = NULL;
    }
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

static void env_repo_info(env *e, atlas_repo_info *out, atlas_err *err) {
    atlas_repo_info_init(out);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", out, &found, err), err);
    T_REQUIRE(found);
}

static void head_oid(env *e, atlas_buf *out, atlas_err *err) {
    const char *rev[] = {"rev-parse", "HEAD"};
    atlas_buf raw = ATLAS_BUF_INIT;
    int code = -1;
    T_OK(fx_git(&e->fx, fx_repo(&e->fx), rev, 2u, &code, &raw, err), err);
    T_REQUIRE(code == 0);
    size_t n = raw.len;
    while (n > 0 && (raw.data[n - 1u] == '\n' || raw.data[n - 1u] == '\r')) {
        n--;
    }
    T_OK(atlas_buf_set(out, raw.data, n, err), err);
    atlas_buf_free(&raw);
}

/* --- a reconciliation pass, no daemon, Decision 9's own shape --------------- */

static void run_pass(env *e, const atlas_syspolicy *pol, atlas_memory_pass_result *result,
                     atlas_err *err) {
    atlas_repo_info ri;
    env_repo_info(e, &ri, err);
    char now[64];
    atlas_now_iso8601(now, sizeof now);
    atlas_memory_observation *obs = malloc(sizeof *obs);
    T_REQUIRE(obs != NULL);
    T_OK(atlas_memory_observe(e->db, &ri, fx_data_dir(&e->fx), pol, obs, err), err);
    T_OK(atlas_db_begin(e->db, err), err);
    T_OK(atlas_memory_apply_in_tx(e->db, &ri, obs, pol, now, result, err), err);
    T_OK(atlas_db_commit(e->db, err), err);
    atlas_memory_observation_free(obs);
    free(obs);
    atlas_repo_info_free(&ri);
}

/* --- a real orchestration run, atlas_orch_apply directly, no writer thread -- */

static void submit_root(env *e, const char *task, atlas_buf *run_uid_out, atlas_err *err) {
    atlas_repo_info ri;
    env_repo_info(e, &ri, err);
    atlas_buf identity = ATLAS_BUF_INIT;
    T_OK(atlas_db_repo_identity_hash(e->db, e->repo_id, &identity, err), err);

    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = e->repo_id;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", err), err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, identity.data, identity.len, err), err);
    T_OK(atlas_buf_set_str(&op->spec.source_commit, ri.scanned_head, err), err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", err), err);
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake", err), err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, task, err), err);
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = 3;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, err), err);
    T_OK(atlas_orch_spec_validate(&op->spec, err), err);

    atlas_orch_result r;
    atlas_orch_result_init(&r);
    T_OK(atlas_orch_apply(e->db, op, &r, err), err);
    T_REQUIRE(r.run_uid.len > 0);
    T_OK(atlas_buf_set(run_uid_out, r.run_uid.data, r.run_uid.len, err), err);
    atlas_orch_result_free(&r);
    atlas_orch_op_free(op);
    free(op);
    atlas_buf_free(&identity);
    atlas_repo_info_free(&ri);
}

/* `atlas_memory_pack_build`/`_freeze_in_tx` directly -- `test_memory_pack.c`'s
 * own shape, unreachable through `run_orch` outside a real daemon writer
 * thread (`test_memory_pack_live.c`'s own header explains why). */
static void freeze_pack(env *e, const atlas_syspolicy *pol, const char *task, const char *run_uid,
                        atlas_memory_pack *out, atlas_err *err) {
    atlas_memory_pack_init(out);
    T_OK(atlas_memory_pack_build(e->db, e->repo_id, pol, task, out, err), err);
    T_OK(atlas_db_begin(e->db, err), err);
    T_OK(atlas_memory_pack_freeze_in_tx(e->db, run_uid, out, err), err);
    T_OK(atlas_db_commit(e->db, err), err);
}

static int64_t insert_reason(env *e, atlas_err *err) {
    atlas_ai_record_input in;
    memset(&in, 0, sizeof in);
    in.repo_id = e->repo_id;
    in.provenance = "MODEL_PROPOSAL";
    in.state = "proposed";
    in.summary = "a T14 test reason";
    int64_t id = 0;
    bool dup = false;
    T_OK(atlas_db_ai_reason_insert(e->db, &in, &id, &dup, err), err);
    T_REQUIRE(id > 0);
    return id;
}

static void propose_decision(env *e, const char *title, atlas_buf *uid_out, atlas_err *err) {
    atlas_decision_op op;
    atlas_decision_op_init(&op, ATLAS_DECISION_OP_PROPOSE);
    T_OK(atlas_buf_set_str(&op.repo_name, "proj", err), err);
    T_OK(atlas_buf_set_str(&op.revision.title, title, err), err);
    T_OK(atlas_buf_set_str(&op.revision.decision_text, "a body for the T14 fixture", err), err);
    op.revision.proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    atlas_decision_result res;
    atlas_decision_result_init(&res);
    T_OK(atlas_decision_apply(e->db, &op, &res, err), err);
    T_OK(atlas_buf_set(uid_out, res.uid.data, res.uid.len, err), err);
    atlas_decision_result_free(&res);
    atlas_decision_op_free(&op);
}

/* --- small SQL helpers, literal text at every call site (never a formatted
 * buffer) -- `atlas_db_prepare`'s pointer-plus-text cache is safe against a
 * reused buffer, `db.c`'s own comment says so, but a literal is what every
 * other call site in this tree passes and there is no reason for this file to
 * be the first exception. */

static int64_t count_rows(atlas_db *db, const char *sql, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(db, sql, &stmt, err), err);
    int rc = sqlite3_step(stmt);
    T_REQUIRE_MSG(rc == SQLITE_ROW, "count query did not yield a row: %s", sql);
    int64_t v = sqlite3_column_int64(stmt, 0);
    atlas_db_finish(db, stmt);
    return v;
}

static void read_decision_status(atlas_db *db, const char *uid, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    sqlite3_stmt *stmt = NULL;
    T_OK(atlas_db_prepare(db, "SELECT current_status FROM decision_documents WHERE uid = ?1;",
                          &stmt, err),
        err);
    T_OK(atlas_db_bind_text_opt(db, stmt, 1, uid, err), err);
    int rc = sqlite3_step(stmt);
    T_REQUIRE_MSG(rc == SQLITE_ROW, "no decision_documents row for uid %s", uid);
    const char *s = (const char *)sqlite3_column_text(stmt, 0);
    T_OK(atlas_buf_set_str(out, s != NULL ? s : "", err), err);
    atlas_db_finish(db, stmt);
}

/* Fix round M3: an exact per-element match against `unknown_fields`'
 * netstring encoding (`trailer.c`'s `ns_put`: "<count>:" then that many
 * "<len>:<bytes>," elements), replacing a `strstr` over the raw bytes.
 * `strstr` cannot currently collide -- the five compiled-in field names are
 * pairwise non-substrings of one another -- but it would silently pass on
 * the wrong evidence the day a sixth field name contains one of the first
 * five, which is exactly the looseness a review flagged. */
static bool unknown_fields_has(const atlas_buf *nf, const char *field) {
    const char *p = atlas_buf_cstr(nf);
    const char *end = p + nf->len;
    char *after = NULL;
    long count = strtol(p, &after, 10);
    if (after == NULL || after >= end || *after != ':' || count < 0) {
        return false;
    }
    p = after + 1;
    size_t field_len = strlen(field);
    for (long i = 0; i < count && p < end; i++) {
        char *lenend = NULL;
        long len = strtol(p, &lenend, 10);
        if (lenend == NULL || lenend >= end || *lenend != ':' || len < 0) {
            return false;
        }
        const char *data = lenend + 1;
        if (data + len >= end || data[len] != ',') {
            return false;
        }
        if ((size_t)len == field_len && memcmp(data, field, field_len) == 0) {
            return true;
        }
        p = data + len + 1;
    }
    return false;
}

/* Replaces the single line beginning with `prefix` in `src` with
 * `replacement` (a whole line, no trailing newline), byte for byte
 * elsewhere -- how every (b) tampering and (d)'s own tampering is built,
 * from one genuinely composed block. */
static void tamper_line(const char *src, const char *prefix, const char *replacement,
                        atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    const char *p = strstr(src, prefix);
    T_REQUIRE_MSG(p != NULL, "fixture assumption failed: \"%s\" not found in the composed block",
                 prefix);
    const char *nl = strchr(p, '\n');
    T_REQUIRE(nl != NULL);
    T_OK(atlas_buf_append(out, src, (size_t)(p - src), err), err);
    T_OK(atlas_buf_append_str(out, replacement, err), err);
    T_OK(atlas_buf_append(out, nl, strlen(nl), err), err);
}

/* --- (a) round trip ---------------------------------------------------------
 *
 * Acceptance item 9's first half: compose a block from a real frozen pack and
 * a real recorded reason, paste it into a commit through the fixture's own
 * git, reconcile, and read the same run uid, generation and both digests back
 * out of the stored binding. */
static void test_round_trip(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "trailer round trip", &run_uid, &err);

    atlas_memory_pack pack;
    freeze_pack(&e, &pol, "trailer round trip", atlas_buf_cstr(&run_uid), &pack, &err);

    int64_t reason_id = insert_reason(&e, &err);
    char reason_text[32];
    (void)snprintf(reason_text, sizeof reason_text, "%lld", (long long)reason_id);

    atlas_buf trailer = ATLAS_BUF_INIT;
    T_OK(atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), reason_text, &trailer, &err),
        &err);
    T_CHECK(strstr(atlas_buf_cstr(&trailer), "Atlas-Provenance: v1\n") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&trailer), "Atlas-Run: ") != NULL);

    T_OK(fx_write(fx_repo(&e.fx), "notes.md", "irrelevant content\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit_body(&e.fx, fx_repo(&e.fx), "trailer round trip test",
                        atlas_buf_cstr(&trailer), &err),
        &err);

    atlas_buf head = ATLAS_BUF_INIT;
    head_oid(&e, &head, &err);

    env_rescan(&e, &err);

    /* Isolates git-side from parse-side, as suggested in review: the raw
     * stored body really carries the marker before the parser is asked
     * anything about it. */
    {
        atlas_buf body = ATLAS_BUF_INIT;
        bool found = false;
        T_OK(atlas_db_commit_body_get(e.db, e.repo_id, atlas_buf_cstr(&head), &body, &found, &err),
            &err);
        T_REQUIRE(found);
        T_CHECK(strstr(atlas_buf_cstr(&body), "Atlas-Provenance: v1") != NULL);
        atlas_buf_free(&body);
    }

    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.trailer_bindings_written == 1,
               "expected exactly one binding written, got %zu", result.trailer_bindings_written);
    T_CHECK(result.trailer_scanned >= 1);
    T_CHECK(!result.trailer_scan_bound_hit);
    /* A real trailer find with zero registered memory sources must still
     * force a generation -- otherwise trailer_scan_high never advances
     * anywhere, and the same commit window is re-walked forever. */
    T_CHECK_MSG(result.generation != 0,
               "a real trailer binding must force a generation even with no source change");

    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    bool bfound = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&head), &b, &bfound,
                                             &err),
        &err);
    T_REQUIRE(bfound);
    T_CHECK(b.has_block);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.run_uid), atlas_buf_cstr(&run_uid)) == 0,
               "expected run_uid %s, got %s", atlas_buf_cstr(&run_uid), atlas_buf_cstr(&b.run_uid));
    T_CHECK_MSG(b.memory_generation == pack.memory_generation,
               "expected generation %lld, got %lld", (long long)pack.memory_generation,
               (long long)b.memory_generation);
    T_CHECK(b.context_digest_ok);
    T_CHECK(b.decision_set_ok);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.change_reason_uid), reason_text) == 0,
               "expected change reason %s, got %s", reason_text,
               atlas_buf_cstr(&b.change_reason_uid));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.unknown_fields), "0:") == 0,
               "expected no unknown fields, got \"%s\"", atlas_buf_cstr(&b.unknown_fields));

    atlas_memory_trailer_binding_free(&b);
    atlas_buf_free(&head);
    atlas_buf_free(&trailer);
    atlas_memory_pack_free(&pack);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* --- (b) four tamperings, each contained to the field(s) it structurally
 * touches ------------------------------------------------------------------- */
static void test_tamperings(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "tamper test", &run_uid, &err);
    atlas_memory_pack pack;
    freeze_pack(&e, &pol, "tamper test", atlas_buf_cstr(&run_uid), &pack, &err);
    int64_t reason_id = insert_reason(&e, &err);
    char reason_text[32];
    (void)snprintf(reason_text, sizeof reason_text, "%lld", (long long)reason_id);

    atlas_buf genuine = ATLAS_BUF_INIT;
    T_OK(atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), reason_text, &genuine, &err),
        &err);

    /* (i) an unknown run uid: well-formed shape, no such row anywhere. */
    atlas_buf t1 = ATLAS_BUF_INIT;
    tamper_line(atlas_buf_cstr(&genuine), "Atlas-Run: ",
               "Atlas-Run: rffffffffffffffffffffffffffffffff", &t1, &err);

    /* (ii) the context digest, one hex digit off the real value. */
    atlas_buf t2 = ATLAS_BUF_INIT;
    {
        char bad[ATLAS_SHA256_HEX_LEN + 1];
        (void)snprintf(bad, sizeof bad, "%s", atlas_buf_cstr(&pack.pack_digest));
        size_t last = strlen(bad) - 1u;
        bad[last] = (bad[last] == '0') ? '1' : '0';
        char line[128];
        (void)snprintf(line, sizeof line, "Atlas-Context-Digest: sha256:%s", bad);
        tamper_line(atlas_buf_cstr(&genuine), "Atlas-Context-Digest: ", line, &t2, &err);
    }

    /* (iii) the decision-set digest, truncated. */
    atlas_buf t3 = ATLAS_BUF_INIT;
    tamper_line(atlas_buf_cstr(&genuine), "Atlas-Decision-Set-Digest: ",
               "Atlas-Decision-Set-Digest: sha256:abcd", &t3, &err);

    /* (iv) a malformed field line: the generation line loses its "": "". */
    atlas_buf t4 = ATLAS_BUF_INIT;
    tamper_line(atlas_buf_cstr(&genuine), "Atlas-Memory-Generation: ",
               "Atlas-Memory-Generation not-a-field-line-at-all", &t4, &err);

    const atlas_buf *bodies[4] = {&t1, &t2, &t3, &t4};
    const char *subjects[4] = {"tamper run", "tamper context digest", "tamper decision digest",
                              "tamper generation line"};
    atlas_buf oids[4];
    for (int i = 0; i < 4; i++) {
        atlas_buf_init(&oids[i]);
        T_OK(fx_write(fx_repo(&e.fx), "notes.md", subjects[i], &err), &err);
        T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
        T_OK(fx_commit_body(&e.fx, fx_repo(&e.fx), subjects[i], atlas_buf_cstr(bodies[i]), &err),
            &err);
        head_oid(&e, &oids[i], &err);
    }

    env_rescan(&e, &err);

    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.trailer_bindings_written == 4, "expected 4 bindings written, got %zu",
               result.trailer_bindings_written);

    atlas_memory_trailer_binding b;

    /* (i) */
    atlas_memory_trailer_binding_init(&b);
    bool found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&oids[0]), &b, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(b.has_block);
    T_CHECK_MSG(b.run_uid.len == 0, "an unresolvable run uid must not be stored");
    T_CHECK(b.memory_generation == 0);
    T_CHECK(!b.context_digest_ok);
    T_CHECK(!b.decision_set_ok);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.change_reason_uid), reason_text) == 0,
               "an unknown run uid must not also take change_reason down: got \"%s\"",
               atlas_buf_cstr(&b.change_reason_uid));
    T_CHECK(unknown_fields_has(&b.unknown_fields, "run"));
    T_CHECK(unknown_fields_has(&b.unknown_fields, "generation"));
    T_CHECK(unknown_fields_has(&b.unknown_fields, "context_digest"));
    T_CHECK(unknown_fields_has(&b.unknown_fields, "decision_set_digest"));
    T_CHECK(!unknown_fields_has(&b.unknown_fields, "change_reason"));
    atlas_memory_trailer_binding_free(&b);

    /* (ii) */
    atlas_memory_trailer_binding_init(&b);
    found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&oids[1]), &b, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(!b.context_digest_ok);
    T_CHECK(unknown_fields_has(&b.unknown_fields, "context_digest"));
    T_CHECK(!unknown_fields_has(&b.unknown_fields, "decision_set_digest"));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.run_uid), atlas_buf_cstr(&run_uid)) == 0,
               "a bad context digest must not also take run down");
    T_CHECK(b.memory_generation == pack.memory_generation);
    T_CHECK(b.decision_set_ok);
    T_CHECK(strcmp(atlas_buf_cstr(&b.change_reason_uid), reason_text) == 0);
    atlas_memory_trailer_binding_free(&b);

    /* (iii) */
    atlas_memory_trailer_binding_init(&b);
    found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&oids[2]), &b, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(!b.decision_set_ok);
    T_CHECK(unknown_fields_has(&b.unknown_fields, "decision_set_digest"));
    T_CHECK(!unknown_fields_has(&b.unknown_fields, "context_digest"));
    T_CHECK(b.context_digest_ok);
    T_CHECK(b.memory_generation == pack.memory_generation);
    atlas_memory_trailer_binding_free(&b);

    /* (iv) */
    atlas_memory_trailer_binding_init(&b);
    found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&oids[3]), &b, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(b.memory_generation == 0);
    T_CHECK(unknown_fields_has(&b.unknown_fields, "generation"));
    T_CHECK(b.context_digest_ok);
    T_CHECK(b.decision_set_ok);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.run_uid), atlas_buf_cstr(&run_uid)) == 0,
               "a malformed generation line must not also take run down");
    atlas_memory_trailer_binding_free(&b);

    for (int i = 0; i < 4; i++) {
        atlas_buf_free(&oids[i]);
    }
    atlas_buf_free(&t1);
    atlas_buf_free(&t2);
    atlas_buf_free(&t3);
    atlas_buf_free(&t4);
    atlas_buf_free(&genuine);
    atlas_memory_pack_free(&pack);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* --- (c) a commit with no block at all --------------------------------------
 *
 * `has_block = 0`, no field rows -- a different fact from carrying six bad
 * ones, asserted directly against (b)'s shape: `unknown_fields` here is
 * genuinely empty (len 0), never the written-but-empty "0:" (a)'s own clean
 * case leaves behind. */
static void test_no_block(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_repo_info ri;
    env_repo_info(&e, &ri, &err);

    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    T_OK(atlas_memory_trailer_ingest(e.db, e.repo_id, ri.scanned_head, &b, &err), &err);
    T_CHECK(!b.has_block);
    /* Fix round I1/I2: the commit's whole (short) message was examined
     * within both bounds, so this is a genuine absence, not an unexamined
     * remainder -- `bound_hit` must be false here, distinct from the fixture
     * below that hits it. */
    T_CHECK(!b.bound_hit);
    T_CHECK(b.run_uid.len == 0);
    T_CHECK(b.memory_generation == 0);
    T_CHECK(!b.context_digest_ok);
    T_CHECK(!b.decision_set_ok);
    T_CHECK(b.change_reason_uid.len == 0);
    T_CHECK_MSG(b.unknown_fields.len == 0,
               "a commit with no block must carry no field rows, got \"%s\"",
               atlas_buf_cstr(&b.unknown_fields));
    atlas_memory_trailer_binding_free(&b);

    /* M4: nothing previously asserted what the *pass* -- as opposed to a
     * direct `atlas_memory_trailer_ingest` call -- leaves behind for a
     * blockless commit. It must store no row at all (distinct from a row
     * with `has_block = 0`, which the fix round now reserves for a
     * `bound_hit` commit), so a reader can tell "no block" apart from "not
     * yet scanned" by whether a row exists at all. */
    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;
    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.trailer_bindings_written == 0,
               "a blockless, fully-examined commit must not be recorded, got %zu",
               result.trailer_bindings_written);

    atlas_memory_trailer_binding stored;
    atlas_memory_trailer_binding_init(&stored);
    bool found = true; /* wrong on purpose: a bug that skips the read must not leave this unset */
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, ri.scanned_head, &stored, &found, &err),
        &err);
    T_CHECK_MSG(!found,
               "a fully-examined blockless commit must have no memory_trailer_bindings row");
    atlas_memory_trailer_binding_free(&stored);

    atlas_repo_info_free(&ri);
    env_close(&e);
}

/* --- Fix round C1: the cursor advances even when a pass finds nothing ------
 *
 * A synthetic history built with direct SQL against `commits` --
 * `test_commits_since_bound`'s own precedent below, used here instead of 601
 * real git commits -- with exactly one trailer marker placed past the first
 * `ATLAS_MEMORY_TRAILER_PASS_MAX` (512) commit-row window and nothing else
 * for a pass to change (no registered memory source, no decision, no HEAD
 * movement beyond the synthetic rows themselves).
 *
 * Before the fix, pass 1 would scan commits 1..512, find nothing, change
 * nothing else, and -- because `memory_generations.trailer_scan_high` was
 * the cursor's only storage and only a fresh generation row ever wrote it --
 * persist no progress at all. Pass 2 would re-read cursor 0 and re-scan the
 * identical 1..512 window, never reaching commit 560. This test would fail
 * against that code: `result2.trailer_bindings_written` would be 0 and the
 * lookup below would find nothing, forever, however many more passes ran.
 *
 * After the fix, pass 1 still writes no binding and mints no generation
 * (asserted below: `ctx.any_change` must not have been forced merely by
 * scan progress, which is the *other* candidate fix this task named and
 * rejected), but it persists the cursor at 512 regardless. Pass 2 resumes
 * there, scans only the 89 remaining rows, and finds the marker. */
static void test_cursor_advances_without_a_find(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err); /* one real commit already indexed, commits.id 1 */

    const int64_t marker_id = 560; /* comfortably past the first 512-row window */
    for (int64_t i = 2; i <= 601; i++) {
        char sql[512];
        if (i == marker_id) {
            (void)snprintf(sql, sizeof sql,
                          "INSERT INTO commits(repo_id, oid, parent_count, body)"
                          " VALUES(%lld, '%040lld', 0, 'Atlas-Provenance: v1\n');",
                          (long long)e.repo_id, (long long)i);
        } else {
            (void)snprintf(sql, sizeof sql,
                          "INSERT INTO commits(repo_id, oid, parent_count) VALUES(%lld, '%040lld', 0);",
                          (long long)e.repo_id, (long long)i);
        }
        T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
    }
    char marker_oid[41];
    (void)snprintf(marker_oid, sizeof marker_oid, "%040lld", (long long)marker_id);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_memory_pass_result result1;
    memset(&result1, 0, sizeof result1);
    run_pass(&e, &pol, &result1, &err);
    T_CHECK_MSG(result1.trailer_scanned == ATLAS_MEMORY_TRAILER_PASS_MAX,
               "expected the first pass to scan exactly the per-pass bound, got %zu",
               result1.trailer_scanned);
    T_CHECK_MSG(result1.trailer_bindings_written == 0,
               "the marker sits above the first window; expected 0 bindings, got %zu",
               result1.trailer_bindings_written);
    T_CHECK_MSG(result1.trailer_scan_bound_hit, "expected the per-pass bound to be reported");
    T_CHECK_MSG(result1.generation == 0,
               "a pass that found nothing and changed nothing else must mint no generation -- "
               "forcing one on scan progress alone is the candidate fix this task rejected");

    atlas_memory_pass_result result2;
    memset(&result2, 0, sizeof result2);
    run_pass(&e, &pol, &result2, &err);
    T_CHECK_MSG(result2.trailer_scanned == 601u - ATLAS_MEMORY_TRAILER_PASS_MAX,
               "expected the second pass to resume where the first left off and scan the "
               "remaining %llu rows, got %zu",
               (unsigned long long)(601u - ATLAS_MEMORY_TRAILER_PASS_MAX), result2.trailer_scanned);
    T_CHECK_MSG(!result2.trailer_scan_bound_hit, "expected no remainder after the second pass");
    T_CHECK_MSG(result2.trailer_bindings_written == 1,
               "expected the second pass to reach and record the marker at commit %lld, got %zu "
               "bindings written",
               (long long)marker_id, result2.trailer_bindings_written);
    T_CHECK_MSG(result2.generation != 0, "a real find must still force a generation");

    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    bool found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, marker_oid, &b, &found, &err), &err);
    T_CHECK_MSG(found, "expected a stored binding for the marker commit");
    T_CHECK(b.has_block);
    atlas_memory_trailer_binding_free(&b);

    env_close(&e);
}

/* --- Fix round I1: the block search now runs from the end of the tail
 * backward -------------------------------------------------------------- */

/* A message whose last 6 lines are a genuine, composed trailer block, preceded
 * by 520 filler lines -- more than `ATLAS_MEMORY_TRAILER_SCAN_MAX` (512), so
 * the pre-fix forward-from-the-front walk would have examined only the first
 * 512 (filler) lines and reported `has_block = false`. The fix walks from the
 * end of the tail backward, so the trailer -- which is where one lives "by
 * construction" (`limits.h`) -- is what it actually finds. */
static void test_block_past_the_forward_line_window(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "past the line window", &run_uid, &err);
    atlas_memory_pack pack;
    freeze_pack(&e, &pol, "past the line window", atlas_buf_cstr(&run_uid), &pack, &err);
    int64_t reason_id = insert_reason(&e, &err);
    char reason_text[32];
    (void)snprintf(reason_text, sizeof reason_text, "%lld", (long long)reason_id);

    atlas_buf trailer = ATLAS_BUF_INIT;
    T_OK(atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), reason_text, &trailer, &err),
        &err);

    atlas_buf body = ATLAS_BUF_INIT;
    for (int i = 0; i < 520; i++) {
        char line[32];
        (void)snprintf(line, sizeof line, "filler line %d\n", i);
        T_OK(atlas_buf_append_str(&body, line, &err), &err);
    }
    T_OK(atlas_buf_append(&body, trailer.data, trailer.len, &err), &err);

    T_OK(fx_write(fx_repo(&e.fx), "notes.md", "past the 512-line window\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit_body(&e.fx, fx_repo(&e.fx), "past the line window", atlas_buf_cstr(&body), &err),
        &err);
    atlas_buf head = ATLAS_BUF_INIT;
    head_oid(&e, &head, &err);

    env_rescan(&e, &err);

    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.trailer_bindings_written == 1,
               "expected the trailing block to be found despite 520 lines ahead of it, got %zu",
               result.trailer_bindings_written);

    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    bool found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&head), &b, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(b.has_block);
    T_CHECK(!b.bound_hit);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.run_uid), atlas_buf_cstr(&run_uid)) == 0,
               "expected the genuine run uid to resolve");
    T_CHECK(b.context_digest_ok);
    T_CHECK(b.decision_set_ok);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.unknown_fields), "0:") == 0,
               "expected every field to verify, got unknown_fields \"%s\"",
               atlas_buf_cstr(&b.unknown_fields));

    atlas_memory_trailer_binding_free(&b);
    atlas_buf_free(&head);
    atlas_buf_free(&body);
    atlas_buf_free(&trailer);
    atlas_memory_pack_free(&pack);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* A `git merge --squash`-shaped message: an older, tampered block (a run uid
 * that resolves to nothing) followed by a genuine one. The fix returns the
 * *last* marker found walking backward -- the one nearest the end of the
 * message -- so the genuine trailing block is what binds, never the quoted
 * one above it. */
static void test_last_block_wins_over_an_earlier_one(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "squash test", &run_uid, &err);
    atlas_memory_pack pack;
    freeze_pack(&e, &pol, "squash test", atlas_buf_cstr(&run_uid), &pack, &err);
    int64_t reason_id = insert_reason(&e, &err);
    char reason_text[32];
    (void)snprintf(reason_text, sizeof reason_text, "%lld", (long long)reason_id);

    atlas_buf genuine = ATLAS_BUF_INIT;
    T_OK(atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), reason_text, &genuine, &err),
        &err);
    atlas_buf stale = ATLAS_BUF_INIT;
    tamper_line(atlas_buf_cstr(&genuine), "Atlas-Run: ",
               "Atlas-Run: rffffffffffffffffffffffffffffffff", &stale, &err);

    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append(&body, stale.data, stale.len, &err), &err);
    T_OK(atlas_buf_append_str(&body, "\n", &err), &err);
    T_OK(atlas_buf_append(&body, genuine.data, genuine.len, &err), &err);

    T_OK(fx_write(fx_repo(&e.fx), "notes.md", "squash\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit_body(&e.fx, fx_repo(&e.fx), "squash test", atlas_buf_cstr(&body), &err), &err);
    atlas_buf head = ATLAS_BUF_INIT;
    head_oid(&e, &head, &err);

    env_rescan(&e, &err);

    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK(result.trailer_bindings_written == 1);

    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    bool found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&head), &b, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(b.has_block);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&b.run_uid), atlas_buf_cstr(&run_uid)) == 0,
               "expected the trailing genuine block to bind, not the stale one above it");
    T_CHECK(b.context_digest_ok);
    T_CHECK(b.decision_set_ok);

    atlas_memory_trailer_binding_free(&b);
    atlas_buf_free(&head);
    atlas_buf_free(&body);
    atlas_buf_free(&stale);
    atlas_buf_free(&genuine);
    atlas_memory_pack_free(&pack);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* --- Fix round I1/I2: a message too long to fully examine reports its own
 * bound rather than reading as "no block", and gets a row of its own ------- */
static void test_unexaminable_message_reports_bound_hit(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    /* No newline anywhere: a single line spanning the whole body, which is
     * exactly the case `ATLAS_MEMORY_TRAILER_TAIL_BYTES_MAX` exists for
     * (`limits.h`) -- and, at 70000 bytes, longer than that backstop, so the
     * search cannot reach the start of the message and must say so rather
     * than report a plain absence. */
    atlas_buf body = ATLAS_BUF_INIT;
    for (int i = 0; i < 70000; i++) {
        T_OK(atlas_buf_append_ch(&body, 'x', &err), &err);
    }

    T_OK(fx_write(fx_repo(&e.fx), "notes.md", "unexaminable\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit_body(&e.fx, fx_repo(&e.fx), "unexaminable message", atlas_buf_cstr(&body), &err),
        &err);
    atlas_buf head = ATLAS_BUF_INIT;
    head_oid(&e, &head, &err);

    env_rescan(&e, &err);

    atlas_repo_info ri;
    env_repo_info(&e, &ri, &err);
    atlas_memory_trailer_binding b;
    atlas_memory_trailer_binding_init(&b);
    T_OK(atlas_memory_trailer_ingest(e.db, e.repo_id, atlas_buf_cstr(&head), &b, &err), &err);
    T_CHECK(!b.has_block);
    T_CHECK_MSG(b.bound_hit,
               "a message longer than the byte backstop must report bound_hit, not a plain "
               "absence");
    atlas_memory_trailer_binding_free(&b);
    atlas_repo_info_free(&ri);

    /* I2: `has_block || bound_hit` is what the pass stores on, so this commit
     * -- genuinely unexaminable, not genuinely blockless -- gets a row of its
     * own rather than reading identically to `test_no_block`'s case. */
    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;
    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK_MSG(result.trailer_bindings_written == 1,
               "expected a bound_hit commit to still land a row, got %zu",
               result.trailer_bindings_written);

    atlas_memory_trailer_binding stored;
    atlas_memory_trailer_binding_init(&stored);
    bool found = false;
    T_OK(atlas_db_memory_trailer_binding_get(e.db, e.repo_id, atlas_buf_cstr(&head), &stored, &found,
                                             &err),
        &err);
    T_REQUIRE(found);
    T_CHECK(!stored.has_block);
    T_CHECK(stored.bound_hit);
    atlas_memory_trailer_binding_free(&stored);

    atlas_buf_free(&head);
    atlas_buf_free(&body);
    env_close(&e);
}

/* --- (d) a tampered block changes no decision status and no verification
 * state ------------------------------------------------------------------ */
static void test_no_authority(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    /* A real decision, PROPOSED and never approved -- the status a forged
     * trailer would have every reason to want moved, if a trailer bound
     * anything at all. */
    atlas_buf dec_uid = ATLAS_BUF_INIT;
    propose_decision(&e, "t14 no authority", &dec_uid, &err);

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "no authority test", &run_uid, &err);
    atlas_memory_pack pack;
    freeze_pack(&e, &pol, "no authority test", atlas_buf_cstr(&run_uid), &pack, &err);
    int64_t reason_id = insert_reason(&e, &err);
    char reason_text[32];
    (void)snprintf(reason_text, sizeof reason_text, "%lld", (long long)reason_id);

    atlas_buf genuine = ATLAS_BUF_INIT;
    T_OK(atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), reason_text, &genuine, &err),
        &err);
    atlas_buf tampered = ATLAS_BUF_INIT;
    tamper_line(atlas_buf_cstr(&genuine), "Atlas-Run: ",
               "Atlas-Run: rffffffffffffffffffffffffffffffff", &tampered, &err);

    int64_t decs_before = count_rows(e.db, "SELECT COUNT(*) FROM decision_revisions;", &err);
    int64_t docs_before = count_rows(e.db, "SELECT COUNT(*) FROM decision_documents;", &err);
    int64_t claims_before = count_rows(e.db, "SELECT COUNT(*) FROM verify_claims;", &err);
    int64_t evidence_before = count_rows(e.db, "SELECT COUNT(*) FROM verify_evidence;", &err);
    int64_t attest_before = count_rows(e.db, "SELECT COUNT(*) FROM verify_attestations;", &err);
    atlas_buf status_before = ATLAS_BUF_INIT;
    read_decision_status(e.db, atlas_buf_cstr(&dec_uid), &status_before, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&status_before), "PROPOSED") == 0,
               "fixture assumption failed: expected a fresh proposal PROPOSED, got %s",
               atlas_buf_cstr(&status_before));

    T_OK(fx_write(fx_repo(&e.fx), "notes.md", "tampered\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit_body(&e.fx, fx_repo(&e.fx), "no authority tamper", atlas_buf_cstr(&tampered),
                        &err),
        &err);

    env_rescan(&e, &err);

    atlas_memory_pass_result result;
    memset(&result, 0, sizeof result);
    run_pass(&e, &pol, &result, &err);
    T_CHECK(result.trailer_bindings_written == 1);

    int64_t decs_after = count_rows(e.db, "SELECT COUNT(*) FROM decision_revisions;", &err);
    int64_t docs_after = count_rows(e.db, "SELECT COUNT(*) FROM decision_documents;", &err);
    int64_t claims_after = count_rows(e.db, "SELECT COUNT(*) FROM verify_claims;", &err);
    int64_t evidence_after = count_rows(e.db, "SELECT COUNT(*) FROM verify_evidence;", &err);
    int64_t attest_after = count_rows(e.db, "SELECT COUNT(*) FROM verify_attestations;", &err);
    atlas_buf status_after = ATLAS_BUF_INIT;
    read_decision_status(e.db, atlas_buf_cstr(&dec_uid), &status_after, &err);

    T_CHECK_MSG(decs_before == decs_after, "decision_revisions row count moved: %lld -> %lld",
               (long long)decs_before, (long long)decs_after);
    T_CHECK_MSG(docs_before == docs_after, "decision_documents row count moved: %lld -> %lld",
               (long long)docs_before, (long long)docs_after);
    T_CHECK_MSG(claims_before == claims_after, "verify_claims row count moved: %lld -> %lld",
               (long long)claims_before, (long long)claims_after);
    T_CHECK_MSG(evidence_before == evidence_after, "verify_evidence row count moved: %lld -> %lld",
               (long long)evidence_before, (long long)evidence_after);
    T_CHECK_MSG(attest_before == attest_after, "verify_attestations row count moved: %lld -> %lld",
               (long long)attest_before, (long long)attest_after);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&status_before), atlas_buf_cstr(&status_after)) == 0,
               "a tampered trailer moved a decision's status: %s -> %s",
               atlas_buf_cstr(&status_before), atlas_buf_cstr(&status_after));

    atlas_buf_free(&status_before);
    atlas_buf_free(&status_after);
    atlas_buf_free(&dec_uid);
    atlas_buf_free(&genuine);
    atlas_buf_free(&tampered);
    atlas_memory_pack_free(&pack);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* --- (e) the composer refuses a run with no frozen pack --------------------- */
static void test_compose_refuses_no_pack(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "no pack yet", &run_uid, &err);
    /* deliberately: no freeze_pack call for this run */

    int64_t reason_id = insert_reason(&e, &err);
    char reason_text[32];
    (void)snprintf(reason_text, sizeof reason_text, "%lld", (long long)reason_id);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_status st =
        atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), reason_text, &out, &err);
    T_CHECK_MSG(st != ATLAS_OK, "compose must refuse a run with no frozen pack");
    T_CHECK_MSG(out.len == 0, "a refusal must not leave a partial block behind, got \"%s\"",
               atlas_buf_cstr(&out));
    atlas_err_init(&err);

    atlas_buf_free(&out);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* Disclosed as an addition beyond the plan's five listed cases: the composer
 * also refuses a `change_reason_uid` that names no `ai_reasons` row for the
 * run's own repository -- the same "never emit an unverifiable value"
 * argument as (e), over the other half of compose()'s two preconditions. */
static void test_compose_refuses_bad_reason(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err);

    atlas_syspolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.state = ATLAS_SYSPOLICY_SYSTEM;

    atlas_buf run_uid = ATLAS_BUF_INIT;
    submit_root(&e, "bad reason test", &run_uid, &err);
    atlas_memory_pack pack;
    freeze_pack(&e, &pol, "bad reason test", atlas_buf_cstr(&run_uid), &pack, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_status st =
        atlas_memory_trailer_compose(e.db, atlas_buf_cstr(&run_uid), "999999", &out, &err);
    T_CHECK_MSG(st != ATLAS_OK, "compose must refuse a change reason no row backs");
    T_CHECK(out.len == 0);
    atlas_err_init(&err);

    atlas_buf_free(&out);
    atlas_memory_pack_free(&pack);
    atlas_buf_free(&run_uid);
    env_close(&e);
}

/* --- the per-pass commit scan bound, driven directly ------------------------
 *
 * `ATLAS_MEMORY_TRAILER_PASS_MAX` is 512 in production; proving it caps a
 * real pass at that scale would mean 513 real git commits, which is a slow
 * way to exercise a bound this file can check directly against
 * `atlas_db_commits_since` -- the same mechanism the pass uses, with a small
 * `max_rows` standing in for the compiled constant. */
typedef struct scan_ctx {
    int64_t last_id;
    size_t calls;
} scan_ctx;

static atlas_status scan_cb(int64_t commit_id, const char *oid, void *ud, atlas_err *err) {
    (void)oid;
    (void)err;
    scan_ctx *c = (scan_ctx *)ud;
    c->calls++;
    c->last_id = commit_id;
    return ATLAS_OK;
}

static void test_commits_since_bound(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e, &err); /* one real commit already indexed */

    for (int i = 0; i < 3; i++) {
        char sql[256];
        (void)snprintf(sql, sizeof sql,
                       "INSERT INTO commits(repo_id, oid, parent_count) VALUES(%lld, '%040d', 0);",
                       (long long)e.repo_id, i + 1);
        T_OK(atlas_db_exec_sql(e.db, sql, &err), &err);
    }

    scan_ctx c1;
    c1.last_id = 0;
    c1.calls = 0;
    int64_t max_id1 = 0;
    bool more1 = false;
    T_OK(atlas_db_commits_since(e.db, e.repo_id, 0, 2u, scan_cb, &c1, &max_id1, &more1, &err),
        &err);
    T_CHECK_MSG(c1.calls == 2u, "expected exactly 2 callback invocations, got %zu", c1.calls);
    T_CHECK_MSG(more1, "expected the bound to be reported as reached");
    T_CHECK(max_id1 == c1.last_id);

    scan_ctx c2;
    c2.last_id = 0;
    c2.calls = 0;
    int64_t max_id2 = 0;
    bool more2 = true;
    T_OK(atlas_db_commits_since(e.db, e.repo_id, max_id1, 100u, scan_cb, &c2, &max_id2, &more2,
                                &err),
        &err);
    T_CHECK_MSG(!more2, "expected no further commits beyond the remainder");
    T_CHECK(c2.calls == 2u);

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"(a) round trip: compose, paste through a real commit, reconcile, read back",
     test_round_trip},
    {"(b) four tamperings, each contained to the field(s) it structurally touches",
     test_tamperings},
    {"(c) a commit with no block carries no field rows, and a pass stores no row for it",
     test_no_block},
    {"(d) a tampered block moves no decision status and no verification row count",
     test_no_authority},
    {"(e) compose refuses a run with no frozen pack", test_compose_refuses_no_pack},
    {"compose refuses a change reason no row backs", test_compose_refuses_bad_reason},
    {"the per-pass commit scan bound is reported", test_commits_since_bound},
    {"fix round C1: the cursor advances even when a pass finds nothing",
     test_cursor_advances_without_a_find},
    {"fix round I1: a trailer past the forward 512-line window is found",
     test_block_past_the_forward_line_window},
    {"fix round I1: the last of two blocks wins over an earlier, stale one",
     test_last_block_wins_over_an_earlier_one},
    {"fix round I1/I2: a message too long to examine reports bound_hit and gets a row",
     test_unexaminable_message_reports_bound_hit},
};

ATLAS_TEST_MAIN("memory_trailer", TESTS)

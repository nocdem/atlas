/* Atlas - A9.2.1: the verification workflow through its product surfaces.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `tests/test_verify_intake.c` drives the write point directly and proves what
 * it refuses. This file proves the same things **through the boundaries a real
 * caller actually reaches** — a live daemon over its socket, and the MCP stdio
 * adapter — because that is where A9.2.1's gap was: the engine and its refusals
 * were correct and complete, and nothing outside the tests could feed them.
 *
 * A refusal that only exists below the transport is a refusal an attacker never
 * meets. Every security case here therefore goes in as JSON on stdin and comes
 * back as JSON on stdout, exactly as a model's would.
 *
 * What is proved here:
 *
 *   §10  the channel is derived from the transport, and an MCP call speaks as a
 *        model even when the uid carrying it is the operator's
 *   §11  every A9.1 knowledge kind survives the real MCP schema, and every one
 *        lands PROPOSED
 *   §16  a model may reference evidence and may not have produced it
 *   §18  actors are not evidence: three readers of one document are one group
 *   §21  the public surface refuses every forgery, and the lifecycle verbs are
 *        absent rather than refused
 *   §22  reliability is not authority: agreement cannot accept a risk
 */
#include <string.h>

#include <sqlite3.h>

#include "atlas/atlas.h"
#include "atlas/buf.h"
#include "atlas/jsonread.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_buf runtime_env;
    atlas_buf project_env;
    atlas_buf repo;
} env;

static void env_start(env *e, atlas_err *err) {
    memset(e, 0, sizeof *e);
    atlas_buf_init(&e->runtime_env);
    atlas_buf_init(&e->project_env);
    atlas_buf_init(&e->repo);
    T_OK(fx_open(&e->fx, err), err);

    /* A repository an operator registered. Nothing model-facing can do this,
     * and there is no RPC method that could. */
    T_OK(atlas_buf_appendf(&e->repo, err, "%s/subject", e->fx.root.data), err);
    T_OK(fx_mkdir(e->fx.root.data, "subject", err), err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(&e->repo), NULL, err), err);
    T_OK(fx_write(atlas_buf_cstr(&e->repo), "a.c", "int subject(void){return 0;}\n", err), err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(&e->repo), err), err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(&e->repo), "initial", err), err);

    fx_daemon_init(&e->d);
    {
        const char *args[] = {"repo", "add", atlas_buf_cstr(&e->repo), "--name", "subject"};
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, 5u, &out, NULL, &code, err), err);
        T_CHECK_MSG(code == 0, "repo add exited %d: %s", code, atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    /* Index it before the daemon starts.
     *
     * A claim binds to the repository state it is about, so intake refuses one
     * against a repository Atlas has not indexed — correctly: it cannot say what
     * state the claim would be true of. Scanning here rather than waiting for
     * the watcher makes that a fact the fixture establishes rather than a race
     * it hopes to win. */
    {
        const char *args[] = {"scan", "subject"};
        atlas_buf out = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, 2u, &out, NULL, &code, err), err);
        T_CHECK_MSG(code == 0, "scan exited %d: %s", code, atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    T_OK(fx_daemon_start(&e->fx, &e->d, err), err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, err), err);
    T_OK(atlas_buf_appendf(&e->runtime_env, err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&e->d.runtime_dir)),
         err);
    T_OK(atlas_buf_appendf(&e->project_env, err, "CLAUDE_PROJECT_DIR=%s",
                           atlas_buf_cstr(&e->repo)),
         err);

}

static void env_stop(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    atlas_buf_free(&e->runtime_env);
    atlas_buf_free(&e->project_env);
    atlas_buf_free(&e->repo);
    fx_close(&e->fx);
}

#define MCP_INIT                                                                  \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"          \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"                  \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* Drives the MCP adapter exactly as a client does: JSON on stdin, JSON on
 * stdout. Every security assertion in this file goes through here. */
static void run_mcp(env *e, const char *script, atlas_buf *out, atlas_err *err) {
    const char *env_list[] = {atlas_buf_cstr(&e->runtime_env), atlas_buf_cstr(&e->project_env),
                              NULL};
    const char *args[] = {"mcp"};
    int code = 0;
    atlas_buf_reset(out);
    T_OK(fx_atlas_stdin(args, 1u, env_list, script, strlen(script), out, NULL, &code, err), err);
    T_EQ_INT(code, 0);
}

/* True when the reply carrying `id` reports the tool ran and succeeded.
 *
 * MCP reports a tool failure inside a successful JSON-RPC reply, so "the call
 * was refused" is `ok:false` in the body rather than a transport error. Reading
 * the envelope instead would score every refusal as a success. */
static bool tool_ok(const atlas_buf *out, const char *needle) {
    const char *p = strstr(atlas_buf_cstr(out), needle);
    if (p == NULL) {
        return false;
    }
    const char *line_end = strchr(p, '\n');
    size_t len = line_end != NULL ? (size_t)(line_end - p) : strlen(p);
    /* The structured body repeats the envelope; `\"ok\":true` appears in it
     * exactly when the tool succeeded. */
    static const char OK[] = "\\\"ok\\\":true";
    const size_t oklen = sizeof OK - 1u;
    for (size_t i = 0; i + oklen <= len; i++) {
        if (memcmp(p + i, OK, oklen) == 0) {
            return true;
        }
    }
    return false;
}

static void expect_tool_refused(const atlas_buf *out, const char *marker, const char *what) {
    T_CHECK_MSG(!tool_ok(out, marker), "%s was accepted through MCP and must not be", what);
}

/* --- §10: the transport decides what an actor is -------------------------- */

/* The defect this closes was Atlas' own, not an attacker's.
 *
 * `channel_for()` derived the actor class from `SO_PEERCRED` alone. A7.1
 * explicitly permits a person to run a model from their own account, and on an
 * unseparated machine there is no other account to run it from — so a local MCP
 * session speaks from the operator uid, and every attestation a model made was
 * stored as a HUMAN actor with PEER_AUTHENTICATED identity. Atlas was minting
 * the forged-human rows this season exists to refuse.
 *
 * The fix is asymmetric on purpose: a request may name its channel and the name
 * is honoured **only when it asserts less**. This test runs both surfaces as the
 * same uid and requires them to disagree in the one direction that is honest. */
static void test_an_mcp_call_speaks_as_a_model_whatever_uid_carries_it(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e,
            MCP_INIT "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                     "{\"name\":\"atlas_verify_claim_create\",\"arguments\":"
                     "{\"repo\":\"subject\",\"text\":\"subject returns zero\","
                     "\"actor\":\"a-model\"}}}\n",
            &out, &err);
    T_CHECK_MSG(tool_ok(&out, "\"id\":2"), "the claim was not created: %s", atlas_buf_cstr(&out));

    /* The same workflow through the CLI, which is the operator channel. */
    {
        const char *args[] = {"verify", "claim", "--repo", "subject",
                              "--text", "stated by the operator"};
        atlas_buf o = ATLAS_BUF_INIT;
        int code = -1;
        T_OK(fx_atlas_with_runtime(&e.fx, &e.d, args, 6u, &o, NULL, &code, &err), &err);
        T_CHECK_MSG(code == 0, "verify claim exited %d: %s", code, atlas_buf_cstr(&o));
        atlas_buf_free(&o);
    }

    /* Read the actors back. Same uid, two transports, two honest answers. */
    fx_daemon_stop(&e.d, false);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    sqlite3 *db = NULL;
    T_REQUIRE(sqlite3_open_v2(atlas_buf_cstr(&db_path), &db, SQLITE_OPEN_READONLY, NULL) ==
              SQLITE_OK);
    int ai = 0, human = 0, forged = 0;
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db, "SELECT class, identity FROM verify_actors;", -1, &st,
                                 NULL) == SQLITE_OK);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *cls = (const char *)sqlite3_column_text(st, 0);
        const char *ident = (const char *)sqlite3_column_text(st, 1);
        if (cls == NULL || ident == NULL) {
            continue;
        }
        if (strcmp(cls, "AI_AGENT") == 0 && strcmp(ident, "SELF_DECLARED") == 0) {
            ai++;
        } else if (strcmp(cls, "HUMAN") == 0 && strcmp(ident, "PEER_AUTHENTICATED") == 0) {
            human++;
        } else if (strcmp(ident, "ATLAS_ATTESTED") == 0 || strcmp(cls, "TOOL") == 0 ||
                   strcmp(cls, "TEST") == 0 || strcmp(cls, "RUNTIME_OBSERVATION") == 0 ||
                   strcmp(cls, "ATLAS_VERIFIER") == 0) {
            forged++;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    atlas_buf_free(&db_path);

    /* The property that must hold everywhere: an MCP call produced an AI_AGENT
     * whose identity is SELF_DECLARED, and nothing on either surface produced a
     * TOOL, a TEST, a RUNTIME_OBSERVATION or an ATLAS_VERIFIER.
     *
     * `human` is deliberately *not* asserted. Whether the CLI speaks as the
     * operator depends on the root-owned authority policy, and in a fixture the
     * profile is correctly LOCKED — the binary under test is one the running uid
     * can replace, which A7 says grants nothing. Requiring a HUMAN row here
     * would be requiring Atlas to over-claim in exactly the situation it is
     * designed to refuse to. What is asserted is that a locked profile costs a
     * *label*, never the ability to record. */
    T_CHECK_MSG(ai >= 1, "the MCP call recorded no AI_AGENT/SELF_DECLARED actor; a model's "
                         "attestation was stored as something else");
    T_CHECK_MSG(forged == 0,
                "%d actor rows carry a class or identity no transport may produce: TOOL, TEST, "
                "RUNTIME_OBSERVATION and ATLAS_VERIFIER exist only where Atlas performed the act",
                forged);
    (void)human;

    atlas_buf_free(&out);
    env_stop(&e);
}

/* --- §21: the public surface refuses every forgery ------------------------ */

/* Refused, not discounted. A discounted forgery still appears in the evidence
 * list, still reads as tool output to somebody skimming a UI, and still has to
 * be argued away by whoever finds it. */
static void test_a_model_cannot_produce_evidence_only_atlas_could_have(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e,
            MCP_INIT "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                     "{\"name\":\"atlas_verify_claim_create\",\"arguments\":"
                     "{\"repo\":\"subject\",\"text\":\"c\"}}}\n",
            &out, &err);
    /* The uid Atlas minted, read out of the reply the way a client would. */
    const char *p = strstr(atlas_buf_cstr(&out), "atlas-claim-");
    T_REQUIRE(p != NULL);
    char claim[96];
    size_t n = 0;
    while (n + 1u < sizeof claim && p[n] != '\0' && p[n] != '\\' && p[n] != '"') {
        claim[n] = p[n];
        n++;
    }
    claim[n] = '\0';

    /* Every class whose entire evidentiary weight comes from Atlas having
     * performed the act. A model naming one is making a claim about what it is,
     * not about what it read. */
    static const char *const FORGED[] = {"COMPILER", "TEST", "RUNTIME", "DEPLOYED_CONFIG"};
    for (size_t i = 0; i < sizeof FORGED / sizeof FORGED[0]; i++) {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"atlas_verify_evidence\",\"arguments\":"
                               "{\"repo\":\"subject\",\"claim\":\"%s\",\"class\":\"%s\","
                               "\"observed\":\"it passed\"}}}\n",
                               claim, FORGED[i]),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        expect_tool_refused(&out, "\"id\":7", FORGED[i]);
        atlas_buf_free(&script);
    }

    /* And the honest route still works: a model may say what it read. */
    {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"atlas_verify_evidence\",\"arguments\":"
                               "{\"repo\":\"subject\",\"claim\":\"%s\",\"class\":\"AI_ANALYSIS\","
                               "\"path\":\"a.c\"}}}\n",
                               claim),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        T_CHECK_MSG(tool_ok(&out, "\"id\":8"),
                    "AI_ANALYSIS was refused; the honest route must stay open: %s",
                    atlas_buf_cstr(&out));
        atlas_buf_free(&script);
    }

    atlas_buf_free(&out);
    env_stop(&e);
}

/* A9.2.2, §29. A model may state a hypothesis; it may not manufacture an
 * authenticated absence proof.
 *
 * Driven **through the real MCP adapter** rather than against the C API,
 * because a refusal that exists only below the transport is one an attacker
 * never meets — the rule A9.2.1 states and `tests/test_verify_absence.c` can
 * only half-satisfy in process.
 *
 * The guarantee being tested is an *absence of a parameter*: no tool names
 * `truth`, `coverage` or any coverage dimension, so nothing reads one. Every
 * schema advertises `additionalProperties: false` and the adapter ignores any
 * member a tool does not name — so a forged argument is not refused, it is
 * simply never read, which is the shape A9 gives the gateway's route table.
 *
 * The assertion is therefore about the *answer* rather than about a rejection:
 * whatever a caller supplies, the reported truth and coverage must remain the
 * ones Atlas derived from index state. That is a stronger check than a schema
 * refusal, because it fails if a future edit ever threads a caller's value
 * through — however politely the request was phrased. */
static void test_no_mcp_call_can_assert_truth_or_coverage(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    run_mcp(&e,
            MCP_INIT "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                     "{\"name\":\"atlas_verify_claim_create\",\"arguments\":"
                     "{\"repo\":\"subject\",\"text\":\"nothing calls the helper\"}}}\n",
            &out, &err);
    const char *p = strstr(atlas_buf_cstr(&out), "atlas-claim-");
    T_REQUIRE(p != NULL);
    char claim[96];
    size_t n = 0;
    while (n + 1u < sizeof claim && p[n] != '\0' && p[n] != '\\' && p[n] != '"') {
        claim[n] = p[n];
        n++;
    }
    claim[n] = '\0';

    /* Each of these would, if accepted, let a caller supply the very thing the
     * absence-proof rule exists to derive. */
    static const char *const FORGERIES[] = {
        "\"truth\":\"ABSENT\"",
        "\"coverage\":\"COMPLETE\"",
        "\"coverage_detail\":\"indirect_calls=COMPLETE\"",
        "\"semantic_generation\":\"COMPLETE\"",
        "\"indirect_calls\":\"COMPLETE\"",
        "\"truth_reason\":\"ESTABLISHED\"",
    };
    for (size_t i = 0; i < sizeof FORGERIES / sizeof FORGERIES[0]; i++) {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"atlas_verify_evaluate\",\"arguments\":"
                               "{\"repo\":\"subject\",\"claim\":\"%s\",%s}}}\n",
                               claim, FORGERIES[i]),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        /* The guarantee is that the supplied value **reaches nothing**, not
         * that the request is rejected.
         *
         * Every tool schema advertises `additionalProperties: false`, and the
         * adapter additionally *ignores* any member a tool does not name —
         * `run_verify_evaluate` reads `repo` and `claim` and nothing else. So a
         * forged argument is not refused; it is not read. That is the same
         * shape A9 gives the gateway's route table, where anything else in a
         * query string is ignored rather than forwarded.
         *
         * Which means the assertion that matters is about the *answer*: the
         * reply must still carry the truth and coverage Atlas derived from
         * index state. This fixture has no semantic generation, so the only
         * honest answer is UNKNOWN — and if a caller-supplied ABSENT or
         * COMPLETE could ever reach the aggregation, it is here that it would
         * show. */
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"truth\":\"UNKNOWN\"") != NULL,
                    "a model supplied %s and the reported truth was not the derived UNKNOWN; "
                    "truth must come from index state, never from a caller: %s",
                    FORGERIES[i], atlas_buf_cstr(&out));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"truth\":\"ABSENT\"") == NULL,
                    "a model supplied %s and Atlas reported ABSENT: %s", FORGERIES[i],
                    atlas_buf_cstr(&out));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"coverage\":\"COMPLETE\"") == NULL,
                    "a model supplied %s and Atlas reported COMPLETE coverage: %s", FORGERIES[i],
                    atlas_buf_cstr(&out));
        atlas_buf_free(&script);
    }

    /* The honest call still works, so this is testing a closed door rather than
     * a broken one. */
    {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"atlas_verify_evaluate\",\"arguments\":"
                               "{\"repo\":\"subject\",\"claim\":\"%s\"}}}\n",
                               claim),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        T_CHECK_MSG(tool_ok(&out, "\"id\":10"),
                    "the ordinary evaluate call was refused: %s", atlas_buf_cstr(&out));
        /* And what comes back carries the truth axis, so a model reading over
         * the transport sees UNKNOWN rather than inferring a negative from a
         * verifier verdict it would have to invert by hand. */
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "truth") != NULL,
                    "the MCP reply carried no truth axis: %s", atlas_buf_cstr(&out));
        atlas_buf_free(&script);
    }

    atlas_buf_free(&out);
    env_stop(&e);
}

/* Absent rather than refused, which is A7's pattern: an absent name is answered
 * by the dispatcher's unknown-tool case, and a refusing one is a refusal a
 * later edit can weaken. */
static void test_no_tool_approves_rejects_supersedes_resolves_or_revalidates(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    static const char *const NAMES[] = {
        "atlas_verify_approve",        "atlas_verify_accept",
        "atlas_approve_decision",      "atlas_reject_decision",
        "atlas_supersede_decision",    "atlas_resolve_decision",
        "atlas_revalidate_decision",   "atlas_decision_approve",
        "atlas_verify_policy_set",     "atlas_verify_set_threshold",
        "atlas_verify_transition",     "atlas_verify_warrant",
        "atlas_verify_override",       "atlas_verify_verdict",
    };
    atlas_buf out = ATLAS_BUF_INIT;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"%s\",\"arguments\":{}}}\n",
                               NAMES[i]),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "unknown tool") != NULL,
                    "\"%s\" did not answer \"unknown tool\"; a lifecycle verb must be absent "
                    "from MCP, not present and refusing", NAMES[i]);
        atlas_buf_free(&script);
    }
    atlas_buf_free(&out);
    env_stop(&e);
}

/* --- §11: every knowledge kind survives the real MCP schema --------------- */

/* This is the DNA migration defect made impossible to repeat.
 *
 * Every proposal there landed as DECISION because the kind never reached the
 * write point, so eight distinct sorts of knowledge were flattened into one and
 * nothing in the record said they had been. The check that matters is not that
 * the tool accepted the argument but that the *stored row* carries the kind, and
 * that it is PROPOSED whichever kind it is — because choosing a kind must never
 * be a way to choose a status. */
static void test_every_knowledge_kind_reaches_the_record_through_mcp(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    static const char *const KINDS[] = {"DECISION",     "POLICY",  "INVARIANT",
                                        "OPERATIONAL_FACT", "ACCEPTED_RISK", "OBLIGATION",
                                        "PARKED",       "REJECTED_ALTERNATIVE"};
    const size_t kind_count = sizeof KINDS / sizeof KINDS[0];
    atlas_buf out = ATLAS_BUF_INIT;
    for (size_t i = 0; i < kind_count; i++) {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"atlas_propose_decision\",\"arguments\":"
                               "{\"repo\":\"subject\",\"kind\":\"%s\","
                               "\"title\":\"kind %s\",\"decision\":\"body\"}}}\n",
                               KINDS[i], KINDS[i]),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        T_CHECK_MSG(tool_ok(&out, "\"id\":3"), "proposing a %s was refused: %s", KINDS[i],
                    atlas_buf_cstr(&out));
        atlas_buf_free(&script);
    }

    fx_daemon_stop(&e.d, false);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    sqlite3 *db = NULL;
    T_REQUIRE(sqlite3_open_v2(atlas_buf_cstr(&db_path), &db, SQLITE_OPEN_READONLY, NULL) ==
              SQLITE_OK);
    for (size_t i = 0; i < kind_count; i++) {
        sqlite3_stmt *st = NULL;
        T_REQUIRE(sqlite3_prepare_v2(db,
                                     "SELECT COUNT(*) FROM decision_documents "
                                     " WHERE kind = ?1 AND current_status = 'PROPOSED';",
                                     -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_text(st, 1, KINDS[i], -1, SQLITE_STATIC);
        int64_t n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
        sqlite3_finalize(st);
        T_CHECK_MSG(n == 1, "%s: expected one PROPOSED record, found %lld — a kind that does not "
                            "reach the record flattens every proposal into DECISION",
                    KINDS[i], (long long)n);
    }
    /* And nothing landed already approved. Choosing a kind is not a way to
     * choose a status. */
    {
        sqlite3_stmt *st = NULL;
        T_REQUIRE(sqlite3_prepare_v2(db,
                                     "SELECT COUNT(*) FROM decision_documents "
                                     " WHERE current_status <> 'PROPOSED';",
                                     -1, &st, NULL) == SQLITE_OK);
        int64_t n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
        sqlite3_finalize(st);
        T_CHECK_MSG(n == 0, "%lld model-written records are not PROPOSED", (long long)n);
    }
    sqlite3_close(db);
    atlas_buf_free(&db_path);
    atlas_buf_free(&out);
    env_stop(&e);
}

/* --- §18 and §22: actors are not evidence, and agreement is not authority -- */

/* Five AI actors agreeing that a privacy risk is acceptable.
 *
 * Two things must both hold, and they are different claims. Atlas may weigh
 * evidence that the risk *exists* — that is a factual question. It must not
 * accept the risk, because accepting one is a normative choice and no quantity
 * of agreement is authority to make it. The fixture is deliberately generous to
 * the wrong answer: five actors, all supporting, all confident. */
static void test_agreement_cannot_accept_a_risk(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf out = ATLAS_BUF_INIT;
    /* A NORMATIVE claim: it says what ought to be, not what is. */
    run_mcp(&e,
            MCP_INIT "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                     "{\"name\":\"atlas_verify_claim_create\",\"arguments\":"
                     "{\"repo\":\"subject\",\"text\":\"this privacy risk is acceptable\","
                     "\"semantics\":\"NORMATIVE\"}}}\n",
            &out, &err);
    const char *p = strstr(atlas_buf_cstr(&out), "atlas-claim-");
    T_REQUIRE(p != NULL);
    char claim[96];
    size_t n = 0;
    while (n + 1u < sizeof claim && p[n] != '\0' && p[n] != '\\' && p[n] != '"') {
        claim[n] = p[n];
        n++;
    }
    claim[n] = '\0';

    static const char *const ACTORS[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    for (size_t i = 0; i < sizeof ACTORS / sizeof ACTORS[0]; i++) {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               MCP_INIT
                               "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                               "\"params\":{\"name\":\"atlas_verify_attest\",\"arguments\":"
                               "{\"claim\":\"%s\",\"verdict\":\"SUPPORT\",\"actor\":\"%s\","
                               "\"self_confidence\":100}}}\n",
                               claim, ACTORS[i]),
             &err);
        run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
        T_CHECK_MSG(tool_ok(&out, "\"id\":4"), "%s could not attest: %s", ACTORS[i],
                    atlas_buf_cstr(&out));
        atlas_buf_free(&script);
    }

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_verify_evaluate\",\"arguments\":"
                           "{\"repo\":\"subject\",\"claim\":\"%s\"}}}\n",
                           claim),
         &err);
    run_mcp(&e, atlas_buf_cstr(&script), &out, &err);
    atlas_buf_free(&script);

    const char *body = atlas_buf_cstr(&out);
    /* A normative claim is JUDGMENT however many sources agree, and the policy
     * must not have acted. */
    T_CHECK_MSG(strstr(body, "JUDGMENT") != NULL,
                "a NORMATIVE claim was not assessed as JUDGMENT: %s", body);
    T_CHECK_MSG(strstr(body, "\\\"transitioned\\\":true") == NULL,
                "five agreeing models moved a lifecycle state; reliability is not authority: %s",
                body);
    T_CHECK_MSG(strstr(body, "\\\"actionable\\\":true") == NULL,
                "a normative claim was reported actionable: %s", body);

    /* Five actors, and the honest independence count is one: none of them
     * declared any evidence, so none of them demonstrated independence. */
    fx_daemon_stop(&e.d, false);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    sqlite3 *db = NULL;
    T_REQUIRE(sqlite3_open_v2(atlas_buf_cstr(&db_path), &db, SQLITE_OPEN_READONLY, NULL) ==
              SQLITE_OK);
    sqlite3_stmt *st = NULL;
    T_REQUIRE(sqlite3_prepare_v2(db,
                                 "SELECT independent_groups FROM verify_results "
                                 " ORDER BY id DESC LIMIT 1;",
                                 -1, &st, NULL) == SQLITE_OK);
    int64_t groups = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    /* And no automatic transition was recorded anywhere. */
    T_REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM verify_lifecycle_audit;", -1, &st,
                                 NULL) == SQLITE_OK);
    int64_t audits = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(db);
    atlas_buf_free(&db_path);

    T_CHECK_MSG(groups == 1,
                "five actors declaring no evidence produced %lld independent groups; an actor "
                "count is not an evidence count", (long long)groups);
    T_CHECK_MSG(audits == 0, "%lld lifecycle audit rows were written for a normative claim",
                (long long)audits);

    atlas_buf_free(&out);
    env_stop(&e);
}

static const atlas_test TESTS[] = {
    {"an MCP call speaks as a model whatever uid carries it",
     test_an_mcp_call_speaks_as_a_model_whatever_uid_carries_it},
    {"a model cannot produce evidence only Atlas could have",
     test_a_model_cannot_produce_evidence_only_atlas_could_have},
    {"no MCP call can assert truth or coverage",
     test_no_mcp_call_can_assert_truth_or_coverage},
    {"no tool approves, rejects, supersedes, resolves or revalidates",
     test_no_tool_approves_rejects_supersedes_resolves_or_revalidates},
    {"every knowledge kind reaches the record through MCP",
     test_every_knowledge_kind_reaches_the_record_through_mcp},
    {"agreement cannot accept a risk", test_agreement_cannot_accept_a_risk},
};

ATLAS_TEST_MAIN("verify_product", TESTS)

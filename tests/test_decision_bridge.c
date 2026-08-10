/* Atlas - A4: the `atlas_record_decision` bridge into the A4 decision model.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_record_decision` is A2's tool. Clients installed before A4 still call
 * it, so its schema and its response stay compatible. What must **not** stay
 * compatible is its *outcome*: a record that exists only in the legacy tables
 * and that somebody has to promote by hand later. An official client that kept
 * producing those would make the A4 decision model something a user opted into
 * rather than something they had.
 *
 * So this suite proves the bridge end to end, through a live daemon and the
 * real MCP process: an old call produces a real A4 document, attributed to the
 * exact session that made it, retrievable through the A4 tools without any CLI
 * step, idempotent under retry, sessionless for a generic client — and
 * approving nothing.
 *
 * It forks a daemon and drives adapters over a pipe, so it carries the daemon
 * label and its serialisation, like every other suite that does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/decision.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* Spelled out rather than included from either adapter, for the reason
 * test_ai_attribution.c gives: the point of the constant is that the hook
 * process and the MCP process independently agree on it. */
#define CLIENT_IDENTITY "\"provider\":\"anthropic\",\"client\":\"claude-code\""

#define MCP_INIT                                                                                   \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                          \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"roots\":{\"listChanged\":true}}}}\n"   \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* --- environment ---------------------------------------------------------- */

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_buf runtime_env;
    atlas_buf repo;  /* registered as "proj" */
    atlas_buf repo2; /* registered as "proj2", for the cross-repository tests */
} env;

static void env_start(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->runtime_env);
    atlas_buf_init(&e->repo);
    atlas_buf_init(&e->repo2);
    T_OK(fx_open(&e->fx, err), err);
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, err), err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, err), err);
    T_OK(atlas_buf_appendf(&e->runtime_env, err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&e->d.runtime_dir)),
         err);

    T_OK(fx_mkdir(e->fx.root.data, "proj", err), err);
    T_OK(atlas_buf_appendf(&e->repo, err, "%s/proj", e->fx.root.data), err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(&e->repo), NULL, err), err);
    T_OK(fx_write(atlas_buf_cstr(&e->repo), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(&e->repo), err), err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(&e->repo), "initial", err), err);
}

static void env_stop(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    atlas_buf_free(&e->runtime_env);
    atlas_buf_free(&e->repo);
    atlas_buf_free(&e->repo2);
    fx_close(&e->fx);
}

/* Runs the CLI against the fixture's data directory and the daemon's runtime. */
static void run_cli(env *e, const char *const *args, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf errout = ATLAS_BUF_INIT;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, n, out, &errout, code, &err), &err);
    /* A failing command explains itself on stderr, so a test that only captured
     * stdout would report "failed:" and nothing else. */
    if (*code != 0 && errout.len > 0) {
        atlas_err ignore;
        atlas_err_init(&ignore);
        (void)atlas_buf_append(out, errout.data, errout.len, &ignore);
    }
    atlas_buf_free(&errout);
}

/* One MCP process over a script. `session_id` becomes CLAUDE_CODE_SESSION_ID,
 * exactly as Claude Code supplies it; NULL leaves it unset, which is a generic
 * MCP client. */
static void run_mcp(env *e, const char *session_id, const char *script, atlas_buf *out,
                    atlas_err *err) {
    atlas_buf session_env = ATLAS_BUF_INIT;
    const char *env_list[3];
    size_t n = 0;
    env_list[n++] = atlas_buf_cstr(&e->runtime_env);
    if (session_id != NULL) {
        T_OK(atlas_buf_appendf(&session_env, err, "CLAUDE_CODE_SESSION_ID=%s", session_id), err);
        env_list[n++] = atlas_buf_cstr(&session_env);
    }
    env_list[n] = NULL;
    const char *args[] = {"mcp"};
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_stdin(args, 1u, env_list, script, strlen(script), out, &errout, &code, err), err);
    T_EQ_INT(code, 0);
    atlas_buf_free(&errout);
    atlas_buf_free(&session_env);
}

/* Opens a session by its exact key, the way the SessionStart hook does. */
static void open_session(env *e, const char *session_id, atlas_err *err) {
    atlas_buf payload = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&payload, err,
                           "{\"session_id\":\"%s\",\"hook_event_name\":\"SessionStart\","
                           "\"cwd\":\"%s\"}",
                           session_id, atlas_buf_cstr(&e->repo)),
         err);
    const char *env_list[] = {atlas_buf_cstr(&e->runtime_env), NULL};
    const char *args[] = {"hook", "SessionStart"};
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_stdin(args, 2u, env_list, atlas_buf_cstr(&payload), payload.len, &out, &errout,
                        &code, err),
         err);
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&payload);
}

/* The legacy call against a named repository, granting both roots so one client
 * can address either — which is exactly the shape that makes repository scope
 * matter. */
static void record_decision_in(env *e, const char *session_id, const char *repo,
                               const char *title, const char *statement, const char *rationale,
                               const char *path, atlas_buf *out, atlas_err *err) {
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"},{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_record_decision\",\"arguments\":"
                           "{\"repo\":\"%s\",\"title\":\"%s\",\"statement\":\"%s\","
                           "\"rationale\":\"%s\",\"paths\":[\"%s\"]}}}\n",
                           atlas_buf_cstr(&e->repo), atlas_buf_cstr(&e->repo2), repo, title,
                           statement, rationale, path),
         err);
    run_mcp(e, session_id, atlas_buf_cstr(&script), out, err);
    atlas_buf_free(&script);
}

/* The same call granting exactly one root.
 *
 * `atlas_record_decision` registers a granted root that is not yet known — that
 * is deliberate and is what makes the tool work on first use — so a test that
 * grants a tree it has not finished setting up will find Atlas has registered
 * it, under a name derived from the directory. One test below depends on a tree
 * staying unregistered until it can claim a freed name, so it grants one root
 * at a time. */
static void record_decision_one_root(env *e, const char *session_id, const char *root,
                                     const char *repo, const char *title, const char *statement,
                                     atlas_buf *out, atlas_err *err) {
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_record_decision\",\"arguments\":"
                           "{\"repo\":\"%s\",\"title\":\"%s\",\"statement\":\"%s\","
                           "\"rationale\":\"because\",\"paths\":[\"a.c\"]}}}\n",
                           root, repo, title, statement),
         err);
    run_mcp(e, session_id, atlas_buf_cstr(&script), out, err);
    atlas_buf_free(&script);
}

/* The legacy call, exactly as an A2-era client makes it. */
static void record_decision(env *e, const char *session_id, const char *title,
                            const char *statement, atlas_buf *out, atlas_err *err) {
    atlas_buf script = ATLAS_BUF_INIT;
    /* The granted root, answered the way a client answers the server's
     * `roots/list` request. MCP is not a filesystem reader: a repository
     * argument must name one a granted root resolved to, so without this the
     * call is refused before it reaches the daemon. */
    T_OK(atlas_buf_appendf(&script, err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_record_decision\",\"arguments\":"
                           "{\"repo\":\"%s\",\"title\":\"%s\",\"statement\":\"%s\","
                           "\"rationale\":\"because\",\"paths\":[\"a.c\"]}}}\n",
                           atlas_buf_cstr(&e->repo), "proj", title, statement),
         err);
    run_mcp(e, session_id, atlas_buf_cstr(&script), out, err);
    atlas_buf_free(&script);
}

/* Registers and scans, so the repository has a name and a lineage.
 *
 * A7: registration is local and the daemon holds the write lock while it runs,
 * so it is stopped around the `repo add` and restarted for the scan. */
static void register_and_scan(env *e) {
    atlas_err rerr;
    atlas_err_init(&rerr);
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    fx_daemon_stop(&e->d, false);
    const char *add[] = {"repo", "add", atlas_buf_cstr(&e->repo), "--name", "proj"};
    run_cli(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    T_OK(fx_daemon_start(&e->fx, &e->d, &rerr), &rerr);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &rerr), &rerr);
    atlas_buf_reset(&out);
    const char *scan[] = {"scan", "proj"};
    run_cli(e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);
}

/* A second, genuinely distinct repository — its own path and its own history —
 * so "one session, two repositories" is the real shape rather than two names
 * for one tree. Creating the tree and registering it are separate, because one
 * test below needs the tree to exist and stay unregistered until it can claim a
 * freed name. */
static void make_second_tree(env *e, atlas_err *err) {
    T_OK(fx_mkdir(e->fx.root.data, "proj2", err), err);
    T_OK(atlas_buf_appendf(&e->repo2, err, "%s/proj2", e->fx.root.data), err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(&e->repo2), NULL, err), err);
    T_OK(fx_write(atlas_buf_cstr(&e->repo2), "b.c", "int other(void){return 1;}\n", err), err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(&e->repo2), err), err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(&e->repo2), "second project", err), err);
}

static void register_second_repo(env *e, const char *name) {
    atlas_err rerr;
    atlas_err_init(&rerr);
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    fx_daemon_stop(&e->d, false);
    const char *add[] = {"repo", "add", atlas_buf_cstr(&e->repo2), "--name", name};
    run_cli(e, add, 5u, &out, &code);
    T_CHECK_MSG(code == 0, "repo add %s failed: %s", name, atlas_buf_cstr(&out));
    T_OK(fx_daemon_start(&e->fx, &e->d, &rerr), &rerr);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &rerr), &rerr);
    atlas_buf_reset(&out);
    const char *scan[] = {"scan", name};
    run_cli(e, scan, 2u, &out, &code);
    T_CHECK_MSG(code == 0, "scan %s failed: %s", name, atlas_buf_cstr(&out));
    atlas_buf_free(&out);
}

static void make_second_repo(env *e, atlas_err *err) {
    make_second_tree(e, err);
    register_second_repo(e, "proj2");
}

/* Counts rows by session, with the id bound rather than formatted in. */
static int64_t count_by_session(atlas_db *db, const char *sql, int64_t session_id) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db, sql, &s, &err), &err);
    T_REQUIRE(sqlite3_bind_int64(s, 1, session_id) == SQLITE_OK);
    int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) {
        n = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return n;
}

static int64_t count_revisions_for_session(atlas_db *db, int64_t session_id) {
    return count_by_session(db, "SELECT COUNT(*) FROM decision_revisions WHERE session_id = ?1;",
                            session_id);
}

static int64_t count_legacy_for_session(atlas_db *db, int64_t session_id) {
    return count_by_session(db, "SELECT COUNT(*) FROM ai_decisions WHERE session_id = ?1;",
                            session_id);
}

/* Counts occurrences of a needle. */
static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

/* --- the bridge ------------------------------------------------------------ */

static void test_the_legacy_tool_produces_an_a4_document(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);

    /* Two sessions open at once, so "attributed to A" is a real claim rather
     * than "attributed to the only one there was". B is the wrong neighbour —
     * the record must never land on it. */
    open_session(&e, "session-A", &err);
    open_session(&e, "session-B", &err);

    atlas_buf out = ATLAS_BUF_INIT;
    record_decision(&e, "session-A", "Use WAL journalling", "Enable WAL on the index.", &out,
                    &err);
    const char *doc = atlas_buf_cstr(&out);

    /* Compatibility first: the old response shape is intact. */
    T_CHECK_MSG(strstr(doc, "\"approved\"") != NULL,
                "the A2 response shape must be preserved: %s", doc);
    T_CHECK_MSG(strstr(doc, "\"error\"") == NULL, "the call must succeed: %s", doc);

    /* And the new outcome: a real A4 decision id came back, so a client is not
     * left needing a CLI promote step. */
    const char *uid_at = strstr(doc, ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE_MSG(uid_at != NULL, "no A4 decision id in the response: %s", doc);
    char uid[ATLAS_DECISION_UID_MAX];
    (void)snprintf(uid, sizeof(uid), "%.*s",
                   (int)(strlen(ATLAS_DECISION_UID_PREFIX) + ATLAS_DECISION_UID_HEX), uid_at);
    T_CHECK_MSG(atlas_decision_uid_is_valid(uid), "the returned id must be well formed: %s", uid);
    atlas_buf_free(&out);

    /* Exactly one document and one revision. */
    int code = 0;
    atlas_buf list = ATLAS_BUF_INIT;
    const char *args[] = {"--json", "decision", "list", "proj"};
    run_cli(&e, args, 4u, &list, &code);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(count_occurrences(atlas_buf_cstr(&list), ATLAS_DECISION_UID_PREFIX) == 1,
                "exactly one decision document must exist: %s", atlas_buf_cstr(&list));
    T_CHECK(strstr(atlas_buf_cstr(&list), "\"total_proposed\":1") != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&list), "\"total_approved\":0") != NULL,
                "the legacy path must not approve anything");
    atlas_buf_free(&list);

    /* The origin points at the A2 row, and the A2 row is untouched and still
     * pinned to approved = 0. */
    atlas_buf show = ATLAS_BUF_INIT;
    const char *sh[] = {"--json", "decision", "show", "proj", uid};
    run_cli(&e, sh, 5u, &show, &code);
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&show), "\"imported_from_a2_decision\"") != NULL,
                "the A4 revision must point back at the A2 row it came from: %s",
                atlas_buf_cstr(&show));
    T_CHECK(strstr(atlas_buf_cstr(&show), "Use WAL journalling") != NULL);
    /* The paths came across, so the document is about the same files. */
    T_CHECK(strstr(atlas_buf_cstr(&show), "a.c") != NULL);
    atlas_buf_free(&show);

    /* And it is retrievable through the A4 MCP tools, with no CLI step. */
    atlas_buf found = ATLAS_BUF_INIT;
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_decisions\",\"arguments\":"
                           "{\"repo\":\"proj\",\"query\":\"wal\"}}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_decision\",\"arguments\":"
                           "{\"repo\":\"proj\",\"decision\":\"%s\"}}}\n",
                           atlas_buf_cstr(&e.repo), uid),
         &err);
    run_mcp(&e, "session-A", atlas_buf_cstr(&script), &found, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&found), uid) != NULL,
                "the decision must be findable through atlas_decisions: %s",
                atlas_buf_cstr(&found));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&found), "Enable WAL on the index.") != NULL,
                "the body must be retrievable through atlas_decision: %s",
                atlas_buf_cstr(&found));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&found), "UNTRUSTED_DATA") != NULL,
                "and it must still be labelled untrusted");
    atlas_buf_free(&script);
    atlas_buf_free(&found);

    env_stop(&e);
}

static void test_the_bridge_attributes_to_the_exact_session(void) {
    /* A2's rule, unchanged and now load-bearing for A4 too: a session is found
     * by its key and by nothing else, and a record that cannot be attached
     * exactly is stored sessionless rather than attached to a neighbour.
     *
     * Two sessions are open. The record must land on the one that made the
     * call, and the *other* must be untouched — which is the half that a
     * "newest open session for this repository" implementation would fail. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);

    open_session(&e, "session-A", &err);
    open_session(&e, "session-B", &err);

    atlas_buf out = ATLAS_BUF_INIT;
    record_decision(&e, "session-A", "From A", "A made this.", &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"session_unbound\":true") == NULL,
                "a call from an open session must be attributed: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    /* Read the attribution out of the database directly: the CLI does not
     * expose a session id, and what is under test is which row it landed on. */
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &err), &err);

    int64_t a_id = 0, b_id = 0, client_id = 0;
    T_OK(atlas_db_ai_client_find(db, "anthropic", "claude-code", &client_id, &err), &err);
    T_REQUIRE(client_id > 0);
    T_OK(atlas_db_ai_session_find(db, client_id, "session-A", &a_id, &err), &err);
    T_OK(atlas_db_ai_session_find(db, client_id, "session-B", &b_id, &err), &err);
    T_REQUIRE(a_id > 0 && b_id > 0 && a_id != b_id);

    /* Counted with a bound parameter against one static statement, rather than
     * by formatting the id into the SQL. The statement cache is keyed on the
     * SQL *pointer*, and a reused buffer presents the same pointer with
     * different text — which is how the first version of this test asked about
     * session B and was answered about session A. The cache now confirms the
     * contents too, and this asks the question the correct way regardless. */
    int64_t on_a = count_revisions_for_session(db, a_id);
    int64_t on_b = count_revisions_for_session(db, b_id);
    int64_t unattributed = -1;
    T_OK(atlas_db_query_int64(
             db, "SELECT COUNT(*) FROM decision_revisions WHERE session_id IS NULL;",
             &unattributed, &err),
         &err);

    T_CHECK_MSG(on_a == 1, "the A4 revision must be attributed to session A, and %lld were",
                (long long)on_a);
    T_CHECK_MSG(on_b == 0, "session B must have gained nothing, and gained %lld",
                (long long)on_b);
    T_CHECK_MSG(unattributed == 0, "the record was attributable, so it must not be unattributed");

    /* The A2 row keeps its own attribution too — that is the origin provenance
     * the A4 revision points at. */
    T_CHECK(count_legacy_for_session(db, a_id) == 1);

    atlas_buf_free(&db_path);
    atlas_db_close(db);
    env_stop(&e);
}

static void test_a_retried_legacy_call_creates_nothing_new(void) {
    /* Idempotency across the pair. The A2 dedup key absorbs the retry, and the
     * early return means the A4 materialisation never runs a second time — so
     * neither a duplicate legacy row nor a duplicate document appears. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);
    open_session(&e, "session-A", &err);

    /* The MCP tool derives its own dedup key from the call, so replaying the
     * identical script is exactly what a redelivered tool call looks like. */
    for (int attempt = 0; attempt < 3; attempt++) {
        atlas_buf out = ATLAS_BUF_INIT;
        record_decision(&e, "session-A", "Repeated", "The same content every time.", &out, &err);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"error\"") == NULL,
                    "attempt %d failed: %s", attempt, atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &err), &err);

    int64_t legacy = -1, docs = -1, revs = -1;
    T_OK(atlas_db_query_int64(db, "SELECT COUNT(*) FROM ai_decisions;", &legacy, &err), &err);
    T_OK(atlas_db_query_int64(db, "SELECT COUNT(*) FROM decision_documents;", &docs, &err), &err);
    T_OK(atlas_db_query_int64(db, "SELECT COUNT(*) FROM decision_revisions;", &revs, &err), &err);

    T_CHECK_MSG(legacy == 1, "three identical calls must leave one A2 row, and left %lld",
                (long long)legacy);
    T_CHECK_MSG(docs == 1, "three identical calls must leave one A4 document, and left %lld",
                (long long)docs);
    T_CHECK_MSG(revs == 1, "and one revision, not %lld", (long long)revs);

    /* Every A2 row that exists has been materialised: none is left needing a
     * manual promote, which is the whole point of the bridge. */
    int64_t unimported = -1;
    T_OK(atlas_db_query_int64(db,
                              "SELECT COUNT(*) FROM ai_decisions a WHERE NOT EXISTS"
                              " (SELECT 1 FROM decision_revisions r"
                              "   WHERE r.imported_from_ai_decision_id = a.id);",
                              &unimported, &err),
         &err);
    T_CHECK_MSG(unimported == 0,
                "no A2 row written through the bridge may be left unmaterialised, and %lld was",
                (long long)unimported);

    atlas_buf_free(&db_path);
    atlas_db_close(db);
    env_stop(&e);
}

static void test_a_generic_client_stays_sessionless(void) {
    /* A generic MCP client — one that is not Claude Code and supplies no
     * session id — may still propose, and its record must be stored
     * sessionless rather than attached to whichever Claude session happens to
     * be open. That is A2's rule and it has to survive the bridge. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);

    /* A Claude session *is* open, so "sessionless" is a real refusal to guess
     * rather than an absence of candidates. */
    open_session(&e, "session-A", &err);

    atlas_buf out = ATLAS_BUF_INIT;
    record_decision(&e, NULL, "From nobody", "A generic client made this.", &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"session_unbound\":true") != NULL,
                "a generic client's record must be sessionless: %s", atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "no_session_id") != NULL,
                "and must say why: %s", atlas_buf_cstr(&out));
    /* It still produced a document. Sessionless is a gap in attribution, not a
     * refusal to record. */
    T_CHECK(strstr(atlas_buf_cstr(&out), ATLAS_DECISION_UID_PREFIX) != NULL);
    atlas_buf_free(&out);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &err), &err);
    int64_t attached = -1;
    T_OK(atlas_db_query_int64(
             db, "SELECT COUNT(*) FROM decision_revisions WHERE session_id IS NOT NULL;",
             &attached, &err),
         &err);
    T_CHECK_MSG(attached == 0,
                "a sessionless record must never borrow the open session: %lld did",
                (long long)attached);
    atlas_buf_free(&db_path);
    atlas_db_close(db);
    env_stop(&e);
}

static void test_neither_path_can_approve(void) {
    /* The bridge creates decision documents, so it is worth asserting directly
     * that it creates PROPOSED ones and that nothing reachable from MCP moves
     * them further. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);
    open_session(&e, "session-A", &err);

    atlas_buf out = ATLAS_BUF_INIT;
    record_decision(&e, "session-A", "Never approved", "Nothing approves this.", &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "APPROVED") == NULL,
                "the legacy path must not produce an approval: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    /* Every plausible follow-up a model could attempt over MCP. */
    static const char *const ATTEMPTS[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":"
        "\"atlas_approve_decision\",\"arguments\":{\"repo\":\"proj\"}}}\n",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":"
        "\"atlas_record_decision\",\"arguments\":{\"repo\":\"proj\",\"title\":\"t\","
        "\"statement\":\"s\",\"approved\":true}}}\n",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":"
        "\"atlas_propose_decision\",\"arguments\":{\"repo\":\"proj\",\"title\":\"t\","
        "\"decision\":\"d\",\"actor\":\"LOCAL_OPERATOR_CONFIRMED\"}}}\n",
        NULL,
    };
    for (size_t i = 0; ATTEMPTS[i] != NULL; i++) {
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_set_str(&script, MCP_INIT, &err), &err);
        T_OK(atlas_buf_appendf(&script, &err,
                               "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                               "[{\"uri\":\"file://%s\"}]}}\n",
                               atlas_buf_cstr(&e.repo)),
             &err);
        T_OK(atlas_buf_append_str(&script, ATTEMPTS[i], &err), &err);
        atlas_buf res = ATLAS_BUF_INIT;
        run_mcp(&e, "session-A", atlas_buf_cstr(&script), &res, &err);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&res), "\"state\":\"APPROVED\"") == NULL,
                    "attempt %zu produced an approved state: %s", i, atlas_buf_cstr(&res));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&res), "LOCAL_OPERATOR_CONFIRMED") == NULL,
                    "attempt %zu produced an operator-confirmed record: %s", i,
                    atlas_buf_cstr(&res));
        atlas_buf_free(&res);
        atlas_buf_free(&script);
    }

    /* And the ledger holds no approval at all. */
    atlas_buf list = ATLAS_BUF_INIT;
    int code = 0;
    const char *args[] = {"--json", "decision", "list", "proj"};
    run_cli(&e, args, 4u, &list, &code);
    T_CHECK(strstr(atlas_buf_cstr(&list), "\"total_approved\":0") != NULL);
    atlas_buf_free(&list);

    env_stop(&e);
}

/* Counts A4 documents attached to a repository, by name. */
static int64_t docs_in(atlas_db *db, const char *repo_name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db,
                          "SELECT COUNT(*) FROM decision_documents d"
                          "  JOIN repositories r ON r.id = d.repo_id"
                          " WHERE r.name = ?1;",
                          &s, &err),
         &err);
    T_REQUIRE(sqlite3_bind_text(s, 1, repo_name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) {
        n = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return n;
}

static int64_t legacy_in(atlas_db *db, const char *repo_name) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db,
                          "SELECT COUNT(*) FROM ai_decisions a"
                          "  JOIN repositories r ON r.id = a.repo_id"
                          " WHERE r.name = ?1;",
                          &s, &err),
         &err);
    T_REQUIRE(sqlite3_bind_text(s, 1, repo_name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW) {
        n = sqlite3_column_int64(s, 0);
    }
    atlas_db_finish(db, s);
    return n;
}

/* A count with no repository filter. */
static int64_t count_all(atlas_db *db, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(db, sql, &n, &err), &err);
    return n;
}

static void open_db(env *e, atlas_db **db, atlas_buf *path, atlas_err *err) {
    T_OK(atlas_buf_appendf(path, err, "%s/atlas.db", fx_data_dir(&e->fx)), err);
    T_OK(atlas_db_open_readonly(atlas_buf_cstr(path), db, err), err);
}

/* The A4 revision's stored attribution, read straight out of the row.
 *
 * Counting revisions per session proves a total; it does not prove *which*
 * document got which session, and a bug that swapped two attributions would
 * keep the totals right. So this reads the columns for one named document. */
typedef struct rev_attribution {
    int64_t session_id;
    int64_t session_unbound;
    atlas_buf unbound_reason;
    int64_t imported_from;
    int64_t legacy_session_id; /* the A2 origin row's own session */
    bool found;
} rev_attribution;

static void rev_attribution_init(rev_attribution *a) {
    a->session_id = -1;
    a->session_unbound = -1;
    atlas_buf_init(&a->unbound_reason);
    a->imported_from = -1;
    a->legacy_session_id = -1;
    a->found = false;
}

static void rev_attribution_free(rev_attribution *a) { atlas_buf_free(&a->unbound_reason); }

/* Reads the single revision of the document Atlas created for `repo_name`.
 *
 * `session_id` is a nullable column and NULL is the whole point of the
 * sessionless case, so it is read as 0-for-NULL deliberately rather than
 * through a helper that would flatten the distinction. */
static void read_attribution(atlas_db *db, const char *repo_name, rev_attribution *out) {
    atlas_err err;
    atlas_err_init(&err);
    sqlite3_stmt *s = NULL;
    T_OK(atlas_db_prepare(db,
                          "SELECT r.session_id, r.session_unbound, r.unbound_reason,"
                          "       r.imported_from_ai_decision_id, a.session_id"
                          "  FROM decision_revisions r"
                          "  JOIN decision_documents d ON d.id = r.document_id"
                          "  JOIN repositories p ON p.id = d.repo_id"
                          "  LEFT JOIN ai_decisions a ON a.id = r.imported_from_ai_decision_id"
                          " WHERE p.name = ?1;",
                          &s, &err),
         &err);
    T_REQUIRE(sqlite3_bind_text(s, 1, repo_name, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    if (sqlite3_step(s) == SQLITE_ROW) {
        out->found = true;
        out->session_id =
            sqlite3_column_type(s, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, 0);
        out->session_unbound = sqlite3_column_int64(s, 1);
        const char *reason = atlas_db_col_text(s, 2);
        T_OK(atlas_buf_set_str(&out->unbound_reason, reason != NULL ? reason : "", &err), &err);
        out->imported_from =
            sqlite3_column_type(s, 3) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, 3);
        out->legacy_session_id =
            sqlite3_column_type(s, 4) == SQLITE_NULL ? 0 : sqlite3_column_int64(s, 4);
    }
    atlas_db_finish(db, s);
}

static void test_identical_text_in_two_repositories_stays_two_decisions(void) {
    /* **One session is routinely attached to several repositories.**
     *
     * A dedup key scoped to the session and the content alone would let a
     * proposal about the second repository be swallowed by an identical one
     * already recorded against the first — the record would vanish, and the
     * response would say `duplicate`. That is not a hypothesis: remove the
     * repository from both places that carry it and this test reports
     * `"duplicate":true` for R2 and zero documents in it.
     *
     * There are two independent places, and **either one alone is sufficient**,
     * which is why removing just one does not fail this test:
     *   1. the key itself — `put_decision_dedup` in `src/mcp/mcp_tools.c`
     *      hashes the repository name into it;
     *   2. the store — `idx_ai_decisions_dedup` is UNIQUE over
     *      `(repo_id, dedup_key)`, and the duplicate lookup in
     *      `atlas_db_ai_decision_insert` binds `repo_id`.
     * Layer 2 is what holds for any client; layer 1 is what would still hold if
     * a future store were not repository-scoped. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);
    make_second_repo(&e, &err);

    open_session(&e, "session-A", &err);
    open_session(&e, "session-B", &err); /* the wrong neighbour, open throughout */

    /* Byte-identical payloads, to two repositories. */
    atlas_buf out = ATLAS_BUF_INIT;
    record_decision_in(&e, "session-A", "proj", "Shared title", "Identical body.", "because",
                       "a.c", &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"error\"") == NULL, "R1: %s",
                atlas_buf_cstr(&out));
    atlas_buf_reset(&out);
    record_decision_in(&e, "session-A", "proj2", "Shared title", "Identical body.", "because",
                       "a.c", &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"error\"") == NULL, "R2: %s",
                atlas_buf_cstr(&out));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"duplicate\":true") == NULL,
                "the second repository's proposal was swallowed as a duplicate of the first: %s",
                atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_db *db = NULL;
    open_db(&e, &db, &db_path, &err);

    T_CHECK_MSG(docs_in(db, "proj") == 1, "R1 must hold one document, holds %lld",
                (long long)docs_in(db, "proj"));
    T_CHECK_MSG(docs_in(db, "proj2") == 1, "R2 must hold one document, holds %lld",
                (long long)docs_in(db, "proj2"));
    T_CHECK(legacy_in(db, "proj") == 1);
    T_CHECK(legacy_in(db, "proj2") == 1);

    /* Both attributed to A, and B has nothing. */
    int64_t client_id = 0, a_id = 0, b_id = 0;
    T_OK(atlas_db_ai_client_find(db, "anthropic", "claude-code", &client_id, &err), &err);
    T_OK(atlas_db_ai_session_find(db, client_id, "session-A", &a_id, &err), &err);
    T_OK(atlas_db_ai_session_find(db, client_id, "session-B", &b_id, &err), &err);
    T_REQUIRE(a_id > 0 && b_id > 0 && a_id != b_id);
    T_CHECK_MSG(count_revisions_for_session(db, a_id) == 2,
                "both revisions must be attributed to A, and %lld were",
                (long long)count_revisions_for_session(db, a_id));
    T_CHECK_MSG(count_revisions_for_session(db, b_id) == 0,
                "session B must have gained nothing, and gained %lld",
                (long long)count_revisions_for_session(db, b_id));

    /* The totals above would still be right if the two attributions had been
     * swapped, so each document's own stored `session_id` is checked. */
    const char *const REPOS[] = {"proj", "proj2"};
    for (size_t i = 0; i < 2u; i++) {
        rev_attribution at;
        rev_attribution_init(&at);
        read_attribution(db, REPOS[i], &at);
        T_CHECK_MSG(at.found, "%s has no A4 revision", REPOS[i]);
        T_CHECK_MSG(at.session_id == a_id,
                    "%s: decision_revisions.session_id must be A (%lld), is %lld", REPOS[i],
                    (long long)a_id, (long long)at.session_id);
        T_CHECK_MSG(at.session_id != b_id, "%s was attributed to the neighbour B", REPOS[i]);
        T_CHECK_MSG(at.session_unbound == 0, "%s: session_unbound must be 0, is %lld", REPOS[i],
                    (long long)at.session_unbound);
        T_CHECK_MSG(at.unbound_reason.len == 0,
                    "%s: a bound revision must carry no unbound reason, carries \"%s\"",
                    REPOS[i], atlas_buf_cstr(&at.unbound_reason));
        /* The A2 origin is preserved and agrees. */
        T_CHECK_MSG(at.imported_from > 0, "%s: imported_from_ai_decision_id must point at the A2 row",
                    REPOS[i]);
        T_CHECK_MSG(at.legacy_session_id == a_id,
                    "%s: the A2 origin row must also record A (%lld), records %lld", REPOS[i],
                    (long long)a_id, (long long)at.legacy_session_id);
        rev_attribution_free(&at);
    }
    atlas_db_close(db);
    atlas_buf_free(&db_path);

    /* Retry each. Neither repository grows. */
    for (int attempt = 0; attempt < 2; attempt++) {
        atlas_buf r = ATLAS_BUF_INIT;
        record_decision_in(&e, "session-A", "proj", "Shared title", "Identical body.", "because",
                           "a.c", &r, &err);
        atlas_buf_reset(&r);
        record_decision_in(&e, "session-A", "proj2", "Shared title", "Identical body.", "because",
                           "a.c", &r, &err);
        atlas_buf_free(&r);
    }
    atlas_buf p2 = ATLAS_BUF_INIT;
    open_db(&e, &db, &p2, &err);
    T_CHECK_MSG(docs_in(db, "proj") == 1, "R1 grew on retry to %lld",
                (long long)docs_in(db, "proj"));
    T_CHECK_MSG(docs_in(db, "proj2") == 1, "R2 grew on retry to %lld",
                (long long)docs_in(db, "proj2"));
    T_CHECK(legacy_in(db, "proj") == 1 && legacy_in(db, "proj2") == 1);
    T_CHECK_MSG(count_revisions_for_session(db, b_id) == 0, "B still gained nothing");

    /* Changing any payload-bearing field is a different proposal, not a
     * duplicate — otherwise the key would be deduplicating too aggressively. */
    atlas_db_close(db);
    atlas_buf_free(&p2);
    struct {
        const char *title;
        const char *body;
        const char *why;
        const char *path;
    } variants[] = {
        {"Shared title", "Identical body.", "because", "b.c"},    /* the path list changed */
        {"Shared title", "A different body.", "because", "a.c"},  /* the statement changed */
        {"Another title", "Identical body.", "because", "a.c"},   /* the title changed */
        {"Shared title", "Identical body.", "a different reason", /* the rationale changed */
         "a.c"},
    };
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        atlas_buf r = ATLAS_BUF_INIT;
        record_decision_in(&e, "session-A", "proj", variants[i].title, variants[i].body,
                           variants[i].why, variants[i].path, &r, &err);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&r), "\"duplicate\":true") == NULL,
                    "variant %zu was wrongly deduplicated: %s", i, atlas_buf_cstr(&r));
        atlas_buf_free(&r);
    }
    atlas_buf p3 = ATLAS_BUF_INIT;
    open_db(&e, &db, &p3, &err);
    T_CHECK_MSG(docs_in(db, "proj") == 5,
                "four distinct variants plus the original must be five documents, not %lld",
                (long long)docs_in(db, "proj"));
    atlas_db_close(db);
    atlas_buf_free(&p3);

    env_stop(&e);
}

static void test_a_sessionless_client_also_gets_repository_scope(void) {
    /* The same cross-repository property for a generic MCP client, which has no
     * session id at all. Its scope is the typed `sessionless` marker plus the
     * repository and the payload — so two repositories still stay two
     * decisions.
     *
     * **The limitation, stated rather than papered over:** with no stable
     * client request identity, two *different* generic clients sending
     * byte-identical payloads to one repository are indistinguishable from one
     * client retrying, and Atlas treats them as one record. Content
     * deduplication cannot separate an intentional identical proposal from a
     * transport retry, and nothing here claims it can. A client that needs them
     * kept apart should send distinguishable content or identify its session. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);
    make_second_repo(&e, &err);
    open_session(&e, "session-B", &err); /* open, and must stay empty */

    atlas_buf out = ATLAS_BUF_INIT;
    record_decision_in(&e, NULL, "proj", "Generic", "From a generic client.", "because", "a.c",
                       &out, &err);
    T_CHECK(strstr(atlas_buf_cstr(&out), "\"session_unbound\":true") != NULL);
    atlas_buf_reset(&out);
    record_decision_in(&e, NULL, "proj2", "Generic", "From a generic client.", "because", "a.c",
                       &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"duplicate\":true") == NULL,
                "a sessionless proposal about R2 was swallowed by R1's: %s",
                atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_db *db = NULL;
    open_db(&e, &db, &db_path, &err);
    T_CHECK_MSG(docs_in(db, "proj") == 1 && docs_in(db, "proj2") == 1,
                "sessionless: R1 %lld, R2 %lld", (long long)docs_in(db, "proj"),
                (long long)docs_in(db, "proj2"));
    int64_t attached = -1;
    T_OK(atlas_db_query_int64(
             db, "SELECT COUNT(*) FROM decision_revisions WHERE session_id IS NOT NULL;",
             &attached, &err),
         &err);
    T_CHECK_MSG(attached == 0, "a sessionless record must not borrow the open session");
    atlas_db_close(db);
    atlas_buf_free(&db_path);

    /* And a retry still collapses within each repository. */
    atlas_buf r = ATLAS_BUF_INIT;
    record_decision_in(&e, NULL, "proj", "Generic", "From a generic client.", "because", "a.c",
                       &r, &err);
    atlas_buf_free(&r);
    atlas_buf p2 = ATLAS_BUF_INIT;
    open_db(&e, &db, &p2, &err);
    T_CHECK_MSG(docs_in(db, "proj") == 1, "sessionless retry grew R1 to %lld",
                (long long)docs_in(db, "proj"));
    atlas_db_close(db);
    atlas_buf_free(&p2);

    env_stop(&e);
}

static void test_a_reused_repository_row_id_cannot_absorb_a_new_decision(void) {
    /* **What the dedup key names, and what row-id reuse really threatens.**
     *
     * The idempotency key hashes the repository *name* the caller passed
     * (`record_args.repo`, a validated name — never a row id, never a path).
     * The store adds a second, independent scope: `idx_ai_decisions_dedup` is
     * UNIQUE over `(repo_id, dedup_key)`, and `repo_id` **is** a reused rowid.
     *
     * For the dedup key itself the reuse is harmless, and structurally so:
     * `ai_decisions.repo_id REFERENCES repositories(id) ON DELETE CASCADE`, with
     * `PRAGMA foreign_keys=ON` on the writable handle, so removing a repository
     * deletes its A2 rows and their keys. A reused row id has no retained
     * record to collide with. This test proves that by making the collision as
     * easy as possible: `repositories.name` is UNIQUE, so the second repository
     * takes the *freed* name, which makes the key byte-identical, and it takes
     * the freed row id too.
     *
     * Writing that test found a real fault a row away. A4 documents deliberately
     * do **not** cascade, so a promoted revision outlives the A2 row it was
     * promoted from while still holding that row's id in
     * `imported_from_ai_decision_id` — and `ai_decisions` rowids are reused as
     * well. The next A2 record anywhere took an id an orphaned revision already
     * pointed at, the unique index on that column rejected the insert, and the
     * whole `atlas_record_decision` call failed with
     * `UNIQUE constraint failed: decision_revisions.imported_from_ai_decision_id`
     * — recording became impossible after any `repo remove`. Worse than the hard
     * failure was what the index was preventing: without it the orphan's pointer
     * would have resolved silently to an unrelated repository's proposal.
     *
     * `atlas_db_repo_remove` now clears those pointers first, in the same
     * transaction as the delete. The revision keeps its own copy of the promoted
     * content, and A2's rule settles the rest: an honest gap beats a pointer to
     * somebody else's record. */
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);
    register_and_scan(&e);
    /* The tree exists so the MCP client can grant it as a root, but it stays
     * unregistered until it can take the freed name. */
    make_second_tree(&e, &err);

    open_session(&e, "session-A", &err);

    atlas_buf out = ATLAS_BUF_INIT;
    record_decision_one_root(&e, "session-A", atlas_buf_cstr(&e.repo), "proj", "Reuse",
                             "About the first project.", &out, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"error\"") == NULL, "%s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    /* The row id and the dedup key that must not be inherited. */
    atlas_buf p1 = ATLAS_BUF_INIT;
    atlas_db *db = NULL;
    open_db(&e, &db, &p1, &err);
    int64_t first_repo_id = -1;
    T_OK(atlas_db_query_int64(db, "SELECT id FROM repositories WHERE name='proj';", &first_repo_id,
                              &err),
         &err);
    T_REQUIRE(first_repo_id > 0);
    atlas_buf first_key = ATLAS_BUF_INIT;
    {
        sqlite3_stmt *st = NULL;
        T_OK(atlas_db_prepare(db, "SELECT dedup_key FROM ai_decisions WHERE repo_id = ?1;", &st,
                              &err),
             &err);
        T_REQUIRE(sqlite3_bind_int64(st, 1, first_repo_id) == SQLITE_OK);
        T_REQUIRE(sqlite3_step(st) == SQLITE_ROW);
        const char *k = atlas_db_col_text(st, 0);
        T_REQUIRE(k != NULL && k[0] != '\0');
        T_OK(atlas_buf_set_str(&first_key, k, &err), &err);
        atlas_db_finish(db, st);
    }
    T_CHECK(legacy_in(db, "proj") == 1 && docs_in(db, "proj") == 1);
    atlas_db_close(db);
    atlas_buf_free(&p1);

    /* Remove it. Registering the *second* tree under the freed name is what
     * makes the next key byte-identical to the first. */
    atlas_buf o = ATLAS_BUF_INIT;
    int code = 0;
    /* A7: local operation, so the daemon steps aside for it. */
    fx_daemon_stop(&e.d, false);
    const char *rm[] = {"repo", "remove", "proj", "--yes"};
    run_cli(&e, rm, 4u, &o, &code);
    T_CHECK_MSG(code == 0, "repo remove failed: %s", atlas_buf_cstr(&o));
    atlas_buf_free(&o);
    T_OK(fx_daemon_start(&e.fx, &e.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e.d, 15000, &err), &err);
    register_second_repo(&e, "proj");

    atlas_buf p2 = ATLAS_BUF_INIT;
    open_db(&e, &db, &p2, &err);
    int64_t reused = -1;
    T_OK(atlas_db_query_int64(db, "SELECT id FROM repositories WHERE name='proj';", &reused, &err),
         &err);
    /* The premise. If SQLite stopped reusing the row id the test would still be
     * correct but would no longer be testing anything, so it says so. */
    T_CHECK_MSG(reused == first_repo_id,
                "this test needs the row id to be reused; it was %lld, now %lld",
                (long long)first_repo_id, (long long)reused);
    /* The cascade fired: no A2 row survived to be collided with. */
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM ai_decisions;") == 0,
                "ON DELETE CASCADE must have removed the A2 rows with the repository");
    /* The A4 document is *not* deleted — it is detached, and visible as an
     * orphan. Decision history outliving a repository row is the A4 rule. */
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM decision_documents;") == 1,
                "a decision document must survive `repo remove`");
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM decision_documents WHERE repo_id = 0;") == 1,
                "and must be detached rather than left pointing at a reused row id");
    /* And its origin pointer is gone, because the row it named is gone and that
     * id is about to belong to somebody else. */
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM decision_revisions"
                              " WHERE imported_from_ai_decision_id IS NOT NULL;") == 0,
                "an orphaned revision must not keep pointing at a deleted A2 row id");
    atlas_db_close(db);
    atlas_buf_free(&p2);

    /* The same payload, the same repository name, the same row id. It must be a
     * new record, not an absorbed retry. */
    atlas_buf out2 = ATLAS_BUF_INIT;
    record_decision_one_root(&e, "session-A", atlas_buf_cstr(&e.repo2), "proj", "Reuse",
                             "About the first project.", &out2, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out2), "\"duplicate\":true") == NULL,
                "a reused row id absorbed a new repository's decision: %s",
                atlas_buf_cstr(&out2));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out2), "\"error\"") == NULL, "second record failed: %s",
                atlas_buf_cstr(&out2));
    atlas_buf_free(&out2);

    atlas_buf p3 = ATLAS_BUF_INIT;
    open_db(&e, &db, &p3, &err);
    T_CHECK_MSG(legacy_in(db, "proj") == 1,
                "the new repository must hold its own A2 row, holds %lld",
                (long long)legacy_in(db, "proj"));
    T_CHECK_MSG(docs_in(db, "proj") == 1,
                "and its own A4 document, holds %lld", (long long)docs_in(db, "proj"));
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM decision_documents;") == 2,
                "the orphan and the new document, two in total");
    /* Exactly one revision claims the reused A2 id, and it is the new one. The
     * orphan keeps its content and its gap. */
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM decision_revisions r"
                              "  JOIN ai_decisions a ON a.id = r.imported_from_ai_decision_id"
                              "  JOIN repositories p ON p.id = a.repo_id"
                              " WHERE p.name = 'proj';") == 1,
                "the new revision must own the reused A2 id");
    T_CHECK_MSG(count_all(db, "SELECT COUNT(*) FROM decision_revisions"
                              " WHERE imported_from_ai_decision_id IS NULL;") == 1,
                "and the orphan must still be the one without an origin");
    /* And the key really was identical, so the separation came from the
     * cascade rather than from the inputs happening to differ. */
    {
        sqlite3_stmt *st = NULL;
        T_OK(atlas_db_prepare(db, "SELECT dedup_key FROM ai_decisions;", &st, &err), &err);
        T_REQUIRE(sqlite3_step(st) == SQLITE_ROW);
        const char *k = atlas_db_col_text(st, 0);
        T_CHECK_MSG(k != NULL && strcmp(k, atlas_buf_cstr(&first_key)) == 0,
                    "the second key must be byte-identical for this test to mean anything");
        atlas_db_finish(db, st);
    }
    atlas_db_close(db);
    atlas_buf_free(&p3);
    atlas_buf_free(&first_key);

    env_stop(&e);
}

static const atlas_test TESTS[] = {
    {"a reused repository row id cannot absorb a new decision",
     test_a_reused_repository_row_id_cannot_absorb_a_new_decision},
    {"identical text in two repositories stays two decisions",
     test_identical_text_in_two_repositories_stays_two_decisions},
    {"a sessionless client also gets repository scope",
     test_a_sessionless_client_also_gets_repository_scope},
    {"the legacy tool produces an A4 document", test_the_legacy_tool_produces_an_a4_document},
    {"the bridge attributes to the exact session",
     test_the_bridge_attributes_to_the_exact_session},
    {"a retried legacy call creates nothing new", test_a_retried_legacy_call_creates_nothing_new},
    {"a generic client stays sessionless", test_a_generic_client_stays_sessionless},
    {"neither path can approve", test_neither_path_can_approve},
};

ATLAS_TEST_MAIN("decision_bridge", TESTS)

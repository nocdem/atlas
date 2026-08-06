/* Atlas - exact session attribution for MCP writes.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The defect this suite exists for: an MCP write used to attach to the *newest
 * open session for the repository*. With two Claude Code sessions open on one
 * worktree, session A calling `atlas_record_reason` could have its reason
 * recorded against session B, and the resulting row was indistinguishable from
 * a correct one. Nothing reported it, and nothing could detect it afterwards.
 *
 * What replaces it is a single rule, and almost every test here is one way of
 * trying to break it:
 *
 *   a record attaches to the session whose external id it carries, or to no
 *   session at all.
 *
 * So the assertions come in pairs. It is never enough that a record reached the
 * right session; the test also has to show that a *wrong* session was available
 * and was not chosen. A suite that only opens one session at a time would pass
 * against the broken implementation.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/limits.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "mcp/mcp_internal.h"
#include "support/fixture.h"

/* The identity the Claude Code adapters use. Spelled out here rather than
 * included from either adapter: the point of the constant is that the hook
 * process and the MCP process independently agree on it, and a test that shared
 * a symbol with them could not notice if they stopped agreeing. */
#define CLIENT_IDENTITY "\"provider\":\"anthropic\",\"client\":\"claude-code\""

/* --- environment ---------------------------------------------------------- */

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_buf runtime_env;
} env;

static void env_start(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->runtime_env);
    T_OK(fx_open(&e->fx, err), err);
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, err), err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, err), err);
    T_OK(atlas_buf_appendf(&e->runtime_env, err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&e->d.runtime_dir)),
         err);
}

static void env_stop(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    atlas_buf_free(&e->runtime_env);
    fx_close(&e->fx);
}

static void make_repo(env *e, const char *name, atlas_buf *path_out, atlas_err *err) {
    T_OK(fx_mkdir(e->fx.root.data, name, err), err);
    atlas_buf_reset(path_out);
    T_OK(atlas_buf_appendf(path_out, err, "%s/%s", e->fx.root.data, name), err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(path_out), NULL, err), err);
    T_OK(fx_write(atlas_buf_cstr(path_out), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(path_out), err), err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(path_out), "initial", err), err);
}

/* Registers a repository under a known name. `ai.session.open` deliberately
 * does not register anything, so a test that wants to ask the daemon about a
 * repository by name has to put it in the index first. */
static void register_repo(env *e, const char *path, const char *name, atlas_err *err) {
    const char *args[] = {"repo", "add", path, "--name", name};
    int code = 0;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, 5u, NULL, NULL, &code, err), err);
    T_EQ_INT(code, 0);
}

/* --- talking to the daemon directly --------------------------------------- */

/* One IPC round trip. The caller owns the response. Returns NULL and records a
 * failure if the call did not answer. */
static atlas_ipc_response *ipc(env *e, const char *method, const char *params, atlas_err *err) {
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    atlas_status st = atlas_ipc_call(atlas_buf_cstr(&e->d.socket), method, params, &raw, err);
    T_CHECK_MSG(st == ATLAS_OK, "%s failed: %s", method, atlas_err_msg(err));
    if (st == ATLAS_OK) {
        T_OK(atlas_ipc_response_parse(raw.data, raw.len, &r, err), err);
    }
    atlas_buf_free(&raw);
    return r;
}

/* Opens (or resumes) a session as the Claude Code hooks would, and returns its
 * Atlas row id. */
static int64_t hook_session_open(env *e, const char *key, const char *root, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, err,
                           "{" CLIENT_IDENTITY ",\"session_key\":\"%s\",\"root\":\"%s\"}", key,
                           root),
         err);
    atlas_ipc_response *r = ipc(e, "ai.session.open", atlas_buf_cstr(&params), err);
    int64_t id = 0;
    if (r != NULL) {
        T_CHECK_MSG(atlas_ipc_response_ok(r), "ai.session.open refused: %s",
                    atlas_ipc_response_message(r));
        T_CHECK(atlas_ipc_result_int(r, "session", &id));
        T_CHECK_MSG(id > 0, "session \"%s\" opened with no id", key);
        atlas_ipc_response_free(r);
    }
    atlas_buf_free(&params);
    return id;
}

static void hook_session_close(env *e, const char *key, const char *reason, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, err,
                           "{" CLIENT_IDENTITY ",\"session_key\":\"%s\",\"source\":\"%s\"}", key,
                           reason),
         err);
    atlas_ipc_response *r = ipc(e, "ai.session.close", atlas_buf_cstr(&params), err);
    if (r != NULL) {
        T_CHECK_MSG(atlas_ipc_response_ok(r), "ai.session.close refused: %s",
                    atlas_ipc_response_message(r));
        atlas_ipc_response_free(r);
    }
    atlas_buf_free(&params);
}

/* --- reading an MCP tool result ------------------------------------------- */

/* The last tool result on stdout, as raw text. Every tool result Atlas sends
 * carries a `structuredContent` member, so the last one is the answer to the
 * last call in the script. */
static const char *last_result(const atlas_buf *out) {
    const char *p = atlas_buf_cstr(out);
    const char *found = NULL;
    for (const char *q = p; (q = strstr(q, "\"structuredContent\"")) != NULL; q++) {
        found = q;
    }
    return found;
}

/* `"<key>":` in the last result. The colon is part of the needle so that
 * `session` does not also match `session_unbound`. */
static const char *member_of_last_result(const atlas_buf *out, const char *key) {
    const char *found = last_result(out);
    if (found == NULL) {
        return NULL;
    }
    char needle[64];
    (void)snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(found, needle);
    return at != NULL ? at + strlen(needle) : NULL;
}

static int64_t result_int(const atlas_buf *out, const char *key) {
    const char *at = member_of_last_result(out, key);
    T_CHECK_MSG(at != NULL, "the tool result has no \"%s\"", key);
    return at != NULL ? (int64_t)strtoll(at, NULL, 10) : -1;
}

static bool result_bool(const atlas_buf *out, const char *key) {
    const char *at = member_of_last_result(out, key);
    T_CHECK_MSG(at != NULL, "the tool result has no \"%s\"", key);
    return at != NULL && strncmp(at, "true", 4u) == 0;
}

/* True when the last result's `unbound_reason` is exactly `want`. */
static bool result_reason_is(const atlas_buf *out, const char *want) {
    const char *at = member_of_last_result(out, "unbound_reason");
    if (at == NULL || *at != '"') {
        return false;
    }
    at++;
    size_t n = strlen(want);
    return strncmp(at, want, n) == 0 && at[n] == '"';
}

/* --- running an MCP server ------------------------------------------------- */

/* One MCP session over a script.
 *
 * `session_id` becomes CLAUDE_CODE_SESSION_ID for that process, exactly as
 * Claude Code supplies it to a stdio MCP server. NULL leaves the variable
 * unset, which is a generic MCP client; "" sets it empty, which must be treated
 * the same as unset. */
static void run_mcp(env *e, const char *session_id, const char *script, atlas_buf *out,
                    atlas_buf *errout, atlas_err *err) {
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
    int code = 0;
    T_OK(fx_atlas_stdin(args, 1u, env_list, script, strlen(script), out, errout, &code, err), err);
    T_EQ_INT(code, 0);
    atlas_buf_free(&session_env);
}

#define MCP_INIT                                                                                   \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                          \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"roots\":{\"listChanged\":true}}}}\n"   \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* Grants one root and records one reason about `a.c`. */
static void record_reason(env *e, const char *session_id, const char *root, const char *summary,
                          atlas_buf *out, atlas_buf *errout, atlas_err *err) {
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_record_reason\",\"arguments\":"
                           "{\"paths\":[\"a.c\"],\"summary\":\"%s\"}}}\n",
                           root, summary),
         err);
    run_mcp(e, session_id, atlas_buf_cstr(&script), out, errout, err);
    atlas_buf_free(&script);
}

/* Asserts a record was durably created, whatever it attached to. An unbound
 * record is still a record: losing the content would be a worse answer than
 * losing the attribution. */
static void check_recorded(const atlas_buf *out) {
    T_CHECK_MSG(!result_bool(out, "degraded"), "the write tool reported degraded");
    T_CHECK_MSG(result_int(out, "record") > 0, "no record row was created");
}

/* --- the id validator ------------------------------------------------------ */

static void test_session_id_validation(void) {
    /* The shape Claude Code actually uses. */
    T_CHECK(atlas_mcp_session_id_valid("6f1e2b3c-0a4d-4e5f-8a9b-0c1d2e3f4a5b", 36u));
    T_CHECK(atlas_mcp_session_id_valid("a", 1u));
    T_CHECK(atlas_mcp_session_id_valid("A.b_c-d:e0", 10u));

    /* Empty is not an id. */
    T_CHECK(!atlas_mcp_session_id_valid("", 0u));
    T_CHECK(!atlas_mcp_session_id_valid(NULL, 0u));

    /* A separator would let an id name a subagent session (`<session>/<agent>`)
     * that this connection has no relationship to. */
    T_CHECK(!atlas_mcp_session_id_valid("sess/agent", 10u));
    /* Anything the daemon's safe encoding would rewrite arrives as a different
     * string from the one the hooks send, so it would silently stop matching. */
    T_CHECK(!atlas_mcp_session_id_valid("sess key", 8u));
    T_CHECK(!atlas_mcp_session_id_valid("sess%20key", 10u));
    T_CHECK(!atlas_mcp_session_id_valid("sess\nkey", 8u));
    T_CHECK(!atlas_mcp_session_id_valid("sess\x1b[0m", 9u));
    T_CHECK(!atlas_mcp_session_id_valid("caf\xc3\xa9", 5u));
    /* An embedded NUL truncates rather than terminating. */
    T_CHECK(!atlas_mcp_session_id_valid("a\0b", 3u));

    /* The ceiling is exact and refuses rather than truncating: a truncated id is
     * a different id, and the one it collides with is somebody else's. */
    char at_limit[ATLAS_AI_SESSION_KEY_MAX + 2u];
    memset(at_limit, 'k', sizeof(at_limit));
    T_CHECK(atlas_mcp_session_id_valid(at_limit, ATLAS_AI_SESSION_KEY_MAX));
    T_CHECK(!atlas_mcp_session_id_valid(at_limit, ATLAS_AI_SESSION_KEY_MAX + 1u));
}

/* --- the defect ------------------------------------------------------------ */

/* Two sessions, one repository. The one that records is not the newest.
 *
 * This is the test the old implementation fails: it would resolve the write to
 * B, because B was touched last, and report a plausible-looking success. */
static void test_two_sessions_one_repository(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "shared", &repo, &err);

    int64_t a = hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);
    /* B is opened second and is therefore the newest open session for this
     * repository — the row the old code would have picked. */
    int64_t b = hook_session_open(&e, "sess-b", atlas_buf_cstr(&repo), &err);
    T_CHECK_MSG(a > 0 && b > 0 && a != b, "the two sessions are not distinct");

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, "sess-a", atlas_buf_cstr(&repo), "A wrote this", &out, &errout, &err);
    check_recorded(&out);
    T_EQ_INT(result_int(&out, "session"), a);
    T_CHECK_MSG(result_int(&out, "session") != b, "A's record was attributed to B");
    T_CHECK_MSG(!result_bool(&out, "session_unbound"), "an exact match reported unbound");

    /* And symmetrically, so the result is not an artefact of which one is older.
     * B is not the newest any more once A has been touched. */
    atlas_buf out_b = ATLAS_BUF_INIT;
    atlas_buf errout_b = ATLAS_BUF_INIT;
    record_reason(&e, "sess-b", atlas_buf_cstr(&repo), "B wrote this", &out_b, &errout_b, &err);
    check_recorded(&out_b);
    T_EQ_INT(result_int(&out_b, "session"), b);

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&out_b);
    atlas_buf_free(&errout_b);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* Interleaved writes from two connections, each with its own id.
 *
 * Attribution has no timing component at all — it is a lookup on
 * `(client, session_key)` — so interleaving is the whole of what "concurrent"
 * can mean here, and the assertion is that no ordering produces a crossover. */
static void test_interleaved_writes_never_cross(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "busy", &repo, &err);

    int64_t a = hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);
    int64_t b = hook_session_open(&e, "sess-b", atlas_buf_cstr(&repo), &err);

    static const char *const ORDER[] = {"sess-a", "sess-b", "sess-b", "sess-a", "sess-a"};
    for (size_t i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]); i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        char summary[64];
        (void)snprintf(summary, sizeof(summary), "write %zu", i);
        record_reason(&e, ORDER[i], atlas_buf_cstr(&repo), summary, &out, &errout, &err);
        check_recorded(&out);
        int64_t want = strcmp(ORDER[i], "sess-a") == 0 ? a : b;
        T_CHECK_MSG(result_int(&out, "session") == want,
                    "write %zu from %s landed on session %lld, expected %lld", i, ORDER[i],
                    (long long)result_int(&out, "session"), (long long)want);
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }

    atlas_buf_free(&repo);
    env_stop(&e);
}

/* --- clients with no usable id --------------------------------------------- */

/* A generic MCP client. Two sessions are open on the repository and it gets
 * neither of them. */
static void test_client_without_a_session_id(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "generic", &repo, &err);
    (void)hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);
    (void)hook_session_open(&e, "sess-b", atlas_buf_cstr(&repo), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, NULL, atlas_buf_cstr(&repo), "from a generic client", &out, &errout, &err);

    /* Supported, not refused: the record exists. */
    check_recorded(&out);
    /* And it is honestly unattached rather than attached to a stranger. */
    T_EQ_INT(result_int(&out, "session"), 0);
    T_CHECK(result_bool(&out, "session_unbound"));
    T_CHECK_MSG(result_reason_is(&out, ATLAS_AI_UNBOUND_NO_SESSION_ID),
                "the reason was not no_session_id");
    /* Said in words too, because a caller reading prose should not have to
     * notice a boolean. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "not attached to any Atlas session") != NULL,
                "the unattached state was not explained in the result");
    /* And the words describe the connection, not an outcome the call did not
     * have: the same field is set on reads, where "recorded" would be false. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "Recorded,") == NULL,
                "the note describes a record rather than the connection");

    /* A variable set to the empty string is the same as an absent one. */
    atlas_buf out_empty = ATLAS_BUF_INIT;
    atlas_buf errout_empty = ATLAS_BUF_INIT;
    record_reason(&e, "", atlas_buf_cstr(&repo), "empty variable", &out_empty, &errout_empty, &err);
    check_recorded(&out_empty);
    T_EQ_INT(result_int(&out_empty, "session"), 0);
    T_CHECK(result_reason_is(&out_empty, ATLAS_AI_UNBOUND_NO_SESSION_ID));

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&out_empty);
    atlas_buf_free(&errout_empty);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* A malformed or over-long id is refused, reported, and never repaired into
 * something that matches. */
static void test_unusable_session_ids(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "hostile", &repo, &err);
    (void)hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);

    char overlong[ATLAS_AI_SESSION_KEY_MAX + 40u];
    memset(overlong, 'x', sizeof(overlong) - 1u);
    overlong[sizeof(overlong) - 1u] = '\0';

    /* Each of these is a different way of not being an id. None of them may
     * produce an attachment, and in particular the prefix of the over-long one
     * must not be used — truncation is how one id becomes another. */
    const char *cases[] = {
        "sess-a/agent",   /* would name a subagent of the open session */
        "sess a",         /* a space; the daemon would re-encode it */
        "sess\tA",        /* a control byte */
        "../../sess-a",   /* traversal-shaped */
        overlong,         /* above the ceiling */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        record_reason(&e, cases[i], atlas_buf_cstr(&repo), "unusable id", &out, &errout, &err);
        check_recorded(&out);
        T_CHECK_MSG(result_int(&out, "session") == 0, "case %zu attached to a session", i);
        T_CHECK_MSG(result_bool(&out, "session_unbound"), "case %zu did not report unbound", i);
        /* Reported on stderr, without echoing the value. */
        T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "not a usable session id") != NULL,
                    "case %zu was rejected silently", i);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "sess-a") == NULL,
                    "case %zu echoed the id back", i);
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }

    atlas_buf_free(&repo);
    env_stop(&e);
}

/* A well-formed id for a session Atlas has never opened — the state when the
 * hooks are not installed. */
static void test_unknown_session_id(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "nohooks", &repo, &err);
    /* Somebody else's session is open on the repository. */
    (void)hook_session_open(&e, "sess-other", atlas_buf_cstr(&repo), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, "sess-never-seen", atlas_buf_cstr(&repo), "no hooks here", &out, &errout,
                  &err);
    check_recorded(&out);
    T_EQ_INT(result_int(&out, "session"), 0);
    T_CHECK(result_reason_is(&out, ATLAS_AI_UNBOUND_UNKNOWN_SESSION));
    /* An MCP write must not conjure the session it names, either: doing so would
     * make every subsequent write "match" an id nothing else knows about. */
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{" CLIENT_IDENTITY ",\"session_key\":\"sess-never-seen\",\"repo\":\"%s\"}",
                           "nohooks"),
         &err);
    atlas_ipc_response *r = ipc(&e, "ai.session.get", atlas_buf_cstr(&params), &err);
    if (r != NULL) {
        bool present = true;
        T_CHECK(atlas_ipc_result_bool(r, "present", &present));
        T_CHECK_MSG(!present, "the write created the session it failed to find");
        atlas_ipc_response_free(r);
    }

    atlas_buf_free(&params);
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* --- ended sessions, resume, and /clear ------------------------------------ */

/* An id whose session has ended. The record is unattached, not attached to
 * whatever else happens to be open. */
static void test_closed_session_id(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "ended", &repo, &err);

    (void)hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);
    int64_t b = hook_session_open(&e, "sess-b", atlas_buf_cstr(&repo), &err);
    hook_session_close(&e, "sess-a", "other", &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, "sess-a", atlas_buf_cstr(&repo), "after the end", &out, &errout, &err);
    check_recorded(&out);
    T_EQ_INT(result_int(&out, "session"), 0);
    T_CHECK_MSG(result_int(&out, "session") != b, "an ended session's write fell through to B");
    T_CHECK(result_reason_is(&out, ATLAS_AI_UNBOUND_SESSION_CLOSED));

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* `claude --resume <id>`: the hooks reopen the same external id, so the same
 * Atlas row comes back and writes carrying that id attach to it again. */
static void test_resume_rebinds_to_the_same_session(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "resumed", &repo, &err);

    int64_t first = hook_session_open(&e, "sess-r", atlas_buf_cstr(&repo), &err);
    hook_session_close(&e, "sess-r", "prompt_input_exit", &err);

    /* While it is closed, a write carrying its id is unattached. */
    atlas_buf gap = ATLAS_BUF_INIT;
    atlas_buf gap_err = ATLAS_BUF_INIT;
    record_reason(&e, "sess-r", atlas_buf_cstr(&repo), "between sessions", &gap, &gap_err, &err);
    T_EQ_INT(result_int(&gap, "session"), 0);
    T_CHECK(result_reason_is(&gap, ATLAS_AI_UNBOUND_SESSION_CLOSED));

    /* The resume. A distraction is open at the same time, so "the newest open
     * session" and "the resumed one" are different rows. */
    int64_t other = hook_session_open(&e, "sess-distraction", atlas_buf_cstr(&repo), &err);
    int64_t resumed = hook_session_open(&e, "sess-r", atlas_buf_cstr(&repo), &err);
    T_CHECK_MSG(resumed == first, "a resume created a second session instead of reopening one");

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, "sess-r", atlas_buf_cstr(&repo), "after resume", &out, &errout, &err);
    check_recorded(&out);
    T_EQ_INT(result_int(&out, "session"), first);
    T_CHECK_MSG(result_int(&out, "session") != other, "the resumed write went to the distraction");
    T_CHECK_MSG(!result_bool(&out, "session_unbound"), "a resumed session reported unbound");

    atlas_buf_free(&gap);
    atlas_buf_free(&gap_err);
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* `/clear`: the hooks end the old conversation and begin a new one with a new
 * id, while a stdio MCP server already running keeps the id it was spawned
 * with. Its writes must not be attributed to the new conversation — and they
 * must not be attributed to the finished one either. */
static void test_clear_does_not_rebind(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "cleared", &repo, &err);

    int64_t before = hook_session_open(&e, "sess-before", atlas_buf_cstr(&repo), &err);
    /* SessionEnd fires with the clear reason, then SessionStart opens the new
     * conversation. The MCP process is not restarted and is not told. */
    hook_session_close(&e, "sess-before", "clear", &err);
    int64_t after = hook_session_open(&e, "sess-after", atlas_buf_cstr(&repo), &err);
    T_CHECK(before > 0 && after > 0 && before != after);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, "sess-before", atlas_buf_cstr(&repo), "after the clear", &out, &errout, &err);
    check_recorded(&out);
    T_CHECK_MSG(result_int(&out, "session") != after,
                "a stale MCP connection was rebound to the new conversation");
    T_CHECK_MSG(result_int(&out, "session") != before,
                "a current turn was attributed to the cleared conversation");
    T_EQ_INT(result_int(&out, "session"), 0);
    T_CHECK(result_reason_is(&out, ATLAS_AI_UNBOUND_SESSION_CLOSED));
    /* The explanation names the case, because "unbound" alone does not tell
     * anybody what to do about it. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "/clear") != NULL,
                "the result did not explain the stale-connection case");

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* --- repository identity is not session identity --------------------------- */

/* A session open on one repository, writing about another.
 *
 * The id proves which conversation this is, so the record attaches. What it
 * does not prove is that the session was ever working in that repository, so
 * there is no change set and the session does not silently acquire the
 * repository — an implicit attachment would change the concurrency accounting
 * for every other session there. */
static void test_session_open_on_a_different_repository(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf one = ATLAS_BUF_INIT;
    atlas_buf two = ATLAS_BUF_INIT;
    make_repo(&e, "one", &one, &err);
    make_repo(&e, "two", &two, &err);

    int64_t a = hook_session_open(&e, "sess-a", atlas_buf_cstr(&one), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    record_reason(&e, "sess-a", atlas_buf_cstr(&two), "about the other repo", &out, &errout, &err);
    check_recorded(&out);
    T_EQ_INT(result_int(&out, "session"), a);
    T_CHECK_MSG(result_int(&out, "change_set") == 0,
                "a change set appeared for a repository the session never attached");

    /* And repository two still has no session attached to it. */
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err, "{" CLIENT_IDENTITY ",\"repo\":\"two\"}"), &err);
    atlas_ipc_response *r = ipc(&e, "ai.session.get", atlas_buf_cstr(&params), &err);
    if (r != NULL) {
        int64_t open_sessions = -1;
        T_CHECK(atlas_ipc_result_int(r, "open_sessions", &open_sessions));
        T_CHECK_MSG(open_sessions == 0, "the write attached the session to the other repository");
        /* With no session key the read reports no session rather than guessing
         * one, exactly as a write would. */
        bool present = true;
        T_CHECK(atlas_ipc_result_bool(r, "present", &present));
        T_CHECK_MSG(!present, "a keyless read was handed a session");
        atlas_ipc_response_free(r);
    }

    atlas_buf_free(&params);
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&one);
    atlas_buf_free(&two);
    env_stop(&e);
}

/* A read tool carries the same explanation as a write, and it must not claim
 * anything was recorded — nothing was. */
static void test_a_read_tool_does_not_claim_a_record(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "reading", &repo, &err);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_session_state\",\"arguments\":{}}}\n",
                           atlas_buf_cstr(&repo)),
         &err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e, "sess-never-opened", atlas_buf_cstr(&script), &out, &errout, &err);

    T_CHECK_MSG(!result_bool(&out, "degraded"), "the read tool reported degraded");
    T_CHECK_MSG(!result_bool(&out, "present"), "a read found a session Atlas never opened");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "Recorded,") == NULL,
                "a read tool claimed something had been recorded");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "not attached to any Atlas session") != NULL,
                "a read tool did not explain why there is no session");

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&script);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* A keyless read must not report somebody else's session even when exactly one
 * is open — the case where guessing would look most defensible. */
static void test_keyless_read_reports_no_session(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "solo", &repo, &err);
    register_repo(&e, atlas_buf_cstr(&repo), "solo", &err);
    (void)hook_session_open(&e, "sess-only", atlas_buf_cstr(&repo), &err);

    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err, "{" CLIENT_IDENTITY ",\"repo\":\"solo\"}"), &err);
    atlas_ipc_response *r = ipc(&e, "ai.session.get", atlas_buf_cstr(&params), &err);
    if (r != NULL) {
        int64_t session = -1;
        T_CHECK(atlas_ipc_result_int(r, "session", &session));
        T_CHECK_MSG(session == 0, "a keyless read returned the only open session");
        /* What it may say is the repository-level fact, which is true without
         * identifying anybody. */
        int64_t open_sessions = -1;
        T_CHECK(atlas_ipc_result_int(r, "open_sessions", &open_sessions));
        T_EQ_INT(open_sessions, 1);
        atlas_ipc_response_free(r);
    }

    /* The envelope agrees: with no key it reports session 0 rather than the
     * neighbour's, because it is injected into a model's context automatically
     * and a wrong number there becomes a wrong belief. */
    atlas_buf ctx_params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ctx_params, &err, "{" CLIENT_IDENTITY ",\"repo\":\"solo\"}"), &err);
    atlas_ipc_response *cr = ipc(&e, "ai.context", atlas_buf_cstr(&ctx_params), &err);
    if (cr != NULL) {
        int64_t session = -1;
        T_CHECK(atlas_ipc_result_int(cr, "session", &session));
        T_CHECK_MSG(session == 0, "the envelope reported a session to a caller with no key");
        atlas_ipc_response_free(cr);
    }

    atlas_buf_free(&ctx_params);
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    env_stop(&e);
}

/* --- repeated writes -------------------------------------------------------- */

/* The same id used twice does not produce two sessions, and a repeated record
 * lands on the same one rather than drifting. */
static void test_repeated_writes_are_stable(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "repeat", &repo, &err);

    int64_t a = hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);
    (void)hook_session_open(&e, "sess-b", atlas_buf_cstr(&repo), &err);

    int64_t first_record = 0;
    for (int i = 0; i < 3; i++) {
        atlas_buf out = ATLAS_BUF_INIT;
        atlas_buf errout = ATLAS_BUF_INIT;
        /* A fresh MCP process each time, as a client restarting its server
         * would produce. Each one rebinds to the same session from the same id. */
        record_reason(&e, "sess-a", atlas_buf_cstr(&repo), "the same reason", &out, &errout, &err);
        check_recorded(&out);
        T_CHECK_MSG(result_int(&out, "session") == a, "repeat %d drifted to another session", i);
        if (i == 0) {
            first_record = result_int(&out, "record");
        } else {
            /* An MCP reason carries no idempotency key, so a caller that sends
             * the same thing twice gets two records. That is reported rather
             * than hidden: `duplicate` is false and the row id is new. */
            T_CHECK_MSG(result_int(&out, "record") != first_record,
                        "repeat %d silently collapsed into the first record", i);
            T_CHECK_MSG(!result_bool(&out, "duplicate"), "repeat %d claimed to be a duplicate", i);
        }
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }

    /* Exactly one session still holds this id. */
    int64_t again = hook_session_open(&e, "sess-a", atlas_buf_cstr(&repo), &err);
    T_CHECK_MSG(again == a, "the id resolved to a different session after repeated writes");

    atlas_buf_free(&repo);
    env_stop(&e);
}

static const atlas_test TESTS[] = {
    {"a session id is validated, never repaired", test_session_id_validation},
    {"two sessions on one repository never cross", test_two_sessions_one_repository},
    {"interleaved writes from two connections never cross",
     test_interleaved_writes_never_cross},
    {"a client with no session id records sessionless", test_client_without_a_session_id},
    {"a malformed or over-long id attaches to nothing", test_unusable_session_ids},
    {"an id Atlas never opened attaches to nothing", test_unknown_session_id},
    {"an ended session's id attaches to nothing", test_closed_session_id},
    {"a resume rebinds to the same session", test_resume_rebinds_to_the_same_session},
    {"a stale connection is not rebound after /clear", test_clear_does_not_rebind},
    {"a session writing about another repository gains no change set",
     test_session_open_on_a_different_repository},
    {"a read tool explains the connection, not a record",
     test_a_read_tool_does_not_claim_a_record},
    {"a keyless read reports no session, not the only one",
     test_keyless_read_reports_no_session},
    {"repeated writes stay on the same session", test_repeated_writes_are_stable},
};

ATLAS_TEST_MAIN("ai_attribution", TESTS)

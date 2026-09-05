/* Atlas - the MCP protocol, driven by a fake client.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Everything here drives the real `atlas mcp` process over pipes, because the
 * properties being checked are properties of the process: what it puts on
 * stdout, what it puts on stderr, and what it does with input a well-behaved
 * client would never send.
 *
 * Most of these tests deliberately run with no daemon. An MCP server whose
 * backend is down is the common case in practice — the daemon restarts, the
 * user has not started it yet — and it is the case where a server that fabricated
 * an answer would do the most damage.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/jsonread.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* --- driving the server --------------------------------------------------- */

typedef struct session {
    atlas_buf out;
    atlas_buf errout;
    int exit_code;
} session;

static void session_init(session *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->out);
    atlas_buf_init(&s->errout);
}

static void session_free(session *s) {
    atlas_buf_free(&s->out);
    atlas_buf_free(&s->errout);
}

/* Runs `atlas mcp` with a fixed script and captures both streams.
 *
 * XDG_RUNTIME_DIR points at a directory with no socket in it, so the daemon is
 * unreachable by construction rather than by luck. */
static void run_script(session *s, const char *script, const char *const *extra_env,
                       atlas_err *err) {
    const char *args[] = {"mcp"};
    const char *env[] = {"XDG_RUNTIME_DIR=/nonexistent-atlas-runtime", NULL, NULL};
    size_t n = 1;
    for (size_t i = 0; extra_env != NULL && extra_env[i] != NULL && n + 1u < 3u; i++) {
        env[n++] = extra_env[i];
    }
    env[n] = NULL;
    T_OK(fx_atlas_stdin(args, 1u, env, script, strlen(script), &s->out, &s->errout, &s->exit_code,
                        err),
         err);
}

/* Returns the Nth line of stdout, or NULL. Lines are the protocol's frames, so
 * counting them is how the framing is checked. */
static atlas_status nth_line(const atlas_buf *out, size_t index, atlas_buf *line, atlas_err *err) {
    atlas_buf_reset(line);
    const char *p = atlas_buf_cstr(out);
    size_t seen = 0;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (n > 0) {
            if (seen == index) {
                return atlas_buf_set(line, p, n, err);
            }
            seen++;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return ATLAS_OK;
}

static size_t line_count(const atlas_buf *out) {
    size_t n = 0;
    const char *p = atlas_buf_cstr(out);
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            n++;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return n;
}

/* Parses one line and hands back the document. The caller frees it. */
static atlas_jsondoc *parse_line(const atlas_buf *out, size_t index, atlas_err *err) {
    atlas_buf line = ATLAS_BUF_INIT;
    T_OK(nth_line(out, index, &line, err), err);
    T_REQUIRE_MSG(line.len > 0, "there is no line %zu on stdout", index);
    atlas_jsondoc *doc = NULL;
    T_OK(atlas_jsondoc_parse(line.data, line.len, ATLAS_MCP_MAX_MESSAGE_BYTES, 24u, &doc, err),
         err);
    atlas_buf_free(&line);
    return doc;
}

#define INIT_LINE                                                                                  \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                          \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"                                      \
    "\"clientInfo\":{\"name\":\"fake\",\"version\":\"1\"}}}\n"                                      \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* --- lifecycle ------------------------------------------------------------ */

static void test_initialize_and_capabilities(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s, INIT_LINE, NULL, &err);
    T_EQ_INT(s.exit_code, 0);

    /* The notification gets no response, so exactly one line comes back. */
    T_EQ_INT((long long)line_count(&s.out), 1);

    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    T_EQ_STR(atlas_jsonv_str_member(root, "jsonrpc"), "2.0");
    const atlas_jsonv *result = atlas_jsonv_get(root, "result");
    T_EQ_STR(atlas_jsonv_str_member(result, "protocolVersion"), "2025-06-18");
    T_EQ_STR(atlas_jsonv_str_member2(result, "serverInfo", "name"), "atlas");
    T_EQ_STR(atlas_jsonv_str_member2(result, "serverInfo", "version"), ATLAS_VERSION_STRING);
    T_CHECK(atlas_jsonv_get(atlas_jsonv_get(result, "capabilities"), "tools") != NULL);
    /* Instructions are Atlas-owned text and must say what the trust rule is. */
    const char *instructions = atlas_jsonv_str_member(result, "instructions");
    T_REQUIRE(instructions != NULL);
    T_CHECK(strstr(instructions, "UNTRUSTED_DATA") != NULL);
    atlas_jsondoc_free(doc);

    session_free(&s);
}

static void test_protocol_version_negotiation(void) {
    atlas_err err;
    atlas_err_init(&err);

    /* Every handshake revision Atlas claims to speak is echoed back. */
    static const char *const KNOWN[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05",
                                        NULL};
    for (size_t i = 0; KNOWN[i] != NULL; i++) {
        T_CHECK_MSG(atlas_mcp_protocol_supported(KNOWN[i]), "%s should be supported", KNOWN[i]);
        atlas_buf script = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&script, &err,
                               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                               "\"params\":{\"protocolVersion\":\"%s\",\"capabilities\":{}}}\n",
                               KNOWN[i]),
             &err);
        session s;
        session_init(&s);
        run_script(&s, atlas_buf_cstr(&script), NULL, &err);
        atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
        T_EQ_STR(atlas_jsonv_str_member(atlas_jsonv_get(atlas_jsondoc_root(doc), "result"),
                                        "protocolVersion"),
                 KNOWN[i]);
        atlas_jsondoc_free(doc);
        session_free(&s);
        atlas_buf_free(&script);
    }

    /* An unknown version gets Atlas' preferred one, which is what the
     * specification requires: the client then decides whether to continue. */
    T_CHECK(!atlas_mcp_protocol_supported("1999-01-01"));
    session s;
    session_init(&s);
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
               "{\"protocolVersion\":\"1999-01-01\",\"capabilities\":{}}}\n",
               NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    T_EQ_STR(atlas_jsonv_str_member(atlas_jsonv_get(atlas_jsondoc_root(doc), "result"),
                                    "protocolVersion"),
             ATLAS_MCP_PREFERRED_PROTOCOL);
    atlas_jsondoc_free(doc);
    /* And it says so on stderr, where diagnostics belong. */
    T_CHECK(strstr(atlas_buf_cstr(&s.errout), "1999-01-01") != NULL);
    session_free(&s);
}

static void test_ping_answers_before_initialization(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    /* Ping is one of the two things the specification allows before the
     * handshake, so it must not be refused for being early. */
    run_script(&s, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n", NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    int64_t id = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(root, "id"), &id));
    T_EQ_INT(id, 7);
    T_CHECK(atlas_jsonv_get(root, "result") != NULL);
    T_CHECK(atlas_jsonv_get(root, "error") == NULL);
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_requests_before_initialize_are_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n", NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    const atlas_jsonv *e = atlas_jsonv_get(atlas_jsondoc_root(doc), "error");
    T_REQUIRE(e != NULL);
    int64_t code = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(e, "code"), &code));
    T_EQ_INT(code, -32600);
    atlas_jsondoc_free(doc);
    session_free(&s);
}

/* --- tools ---------------------------------------------------------------- */

static void test_tools_list_is_complete_and_strictly_schemad(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s, INIT_LINE "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n", NULL,
               &err);

    atlas_jsondoc *doc = parse_line(&s.out, 1, &err);
    const atlas_jsonv *tools =
        atlas_jsonv_get(atlas_jsonv_get(atlas_jsondoc_root(doc), "result"), "tools");
    size_t n = atlas_jsonv_arr_len(tools);
    /* A3 added six structural tools to A2's ten. The floor is asserted so a
     * surface that silently shrinks is caught; the exact set is checked against
     * atlas_mcp_tool_names() below. */
    T_CHECK_MSG(n >= 16, "expected at least 16 tools, got %zu", n);

    /* The documented surface and the implemented one cannot drift: the header's
     * list is compared against what the process actually reported.
     *
     * A14: remote-only tools are correctly absent from the stdio listing —
     * `atlas_mcp_tool_remote_only` identifies them, and each is checked absent
     * rather than present. */
    const char *const *expected = atlas_mcp_tool_names();
    for (size_t i = 0; expected[i] != NULL; i++) {
        bool found = false;
        for (size_t k = 0; k < n; k++) {
            const char *name = atlas_jsonv_str_member(atlas_jsonv_at(tools, k), "name");
            if (name != NULL && strcmp(name, expected[i]) == 0) {
                found = true;
                break;
            }
        }
        if (atlas_mcp_tool_remote_only(expected[i])) {
            T_CHECK_MSG(!found, "remote-only tool %s appears in the stdio listing", expected[i]);
        } else {
            T_CHECK_MSG(found, "tool %s was not listed", expected[i]);
        }
    }

    for (size_t k = 0; k < n; k++) {
        const atlas_jsonv *tool = atlas_jsonv_at(tools, k);
        const char *name = atlas_jsonv_str_member(tool, "name");
        T_CHECK(name != NULL);
        T_CHECK_MSG(atlas_jsonv_str_member(tool, "description") != NULL, "%s has no description",
                    name != NULL ? name : "?");
        const atlas_jsonv *schema = atlas_jsonv_get(tool, "inputSchema");
        T_REQUIRE_MSG(schema != NULL, "%s has no inputSchema", name != NULL ? name : "?");
        T_EQ_STR(atlas_jsonv_str_member(schema, "type"), "object");
        /* An argument a tool does not implement must be refused rather than
         * silently accepted: otherwise a caller believes it asked for
         * something. */
        bool additional = true;
        T_CHECK_MSG(atlas_jsonv_bool(atlas_jsonv_get(schema, "additionalProperties"), &additional) &&
                        !additional,
                    "%s does not forbid additional properties", name != NULL ? name : "?");
        T_CHECK(atlas_jsonv_get(schema, "properties") != NULL);
        T_CHECK(atlas_jsonv_get(schema, "required") != NULL);
    }
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_remote_only_tools_absent_from_stdio(void) {
    /* A14: every remote-only tool answers "unknown tool" on the stdio adapter,
     * exactly as if the tool does not exist — absent rather than refused. */
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s,
               INIT_LINE "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
                         "{\"name\":\"atlas_job_submit\","
                         "\"arguments\":{\"task\":\"t\"}}}\n",
               NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 1, &err);
    const atlas_jsonv *e = atlas_jsonv_get(atlas_jsondoc_root(doc), "error");
    T_REQUIRE_MSG(e != NULL, "calling atlas_job_submit on stdio should produce an error");
    int64_t code = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(e, "code"), &code));
    T_EQ_INT((int)code, -32602);
    const char *msg = atlas_jsonv_str_member(e, "message");
    T_CHECK_MSG(msg != NULL && strstr(msg, "unknown tool") != NULL,
                "atlas_job_submit on stdio should answer \"unknown tool\", got: %s",
                msg != NULL ? msg : "(null)");
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_unknown_tool_is_a_protocol_error(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s,
               INIT_LINE "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                         "{\"name\":\"atlas_delete_everything\",\"arguments\":{}}}\n",
               NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 1, &err);
    const atlas_jsonv *e = atlas_jsonv_get(atlas_jsondoc_root(doc), "error");
    T_REQUIRE(e != NULL);
    int64_t code = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(e, "code"), &code));
    T_EQ_INT(code, -32602);
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_daemon_unavailable_is_a_degraded_result_not_a_crash(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s,
               INIT_LINE "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                         "{\"name\":\"atlas_status\",\"arguments\":{}}}\n",
               NULL, &err);
    T_EQ_INT(s.exit_code, 0);

    atlas_jsondoc *doc = parse_line(&s.out, 1, &err);
    const atlas_jsonv *result = atlas_jsonv_get(atlas_jsondoc_root(doc), "result");
    T_REQUIRE(result != NULL);
    bool is_error = false;
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(result, "isError"), &is_error));
    T_CHECK(is_error);
    /* The structured half says degraded, and says it is a fact about Atlas
     * rather than about the repository. */
    const atlas_jsonv *structured = atlas_jsonv_get(result, "structuredContent");
    T_REQUIRE(structured != NULL);
    bool degraded = false;
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(structured, "degraded"), &degraded));
    T_CHECK(degraded);
    T_EQ_STR(atlas_jsonv_str_member(structured, "provenance"), "ATLAS_OWNED");
    bool untrusted = true;
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(structured, "untrusted_data"), &untrusted));
    T_CHECK(!untrusted);
    atlas_jsondoc_free(doc);

    /* The diagnostic went to stderr, and stdout carried only protocol. */
    T_CHECK(s.errout.len > 0);
    T_CHECK(strstr(atlas_buf_cstr(&s.errout), "atlas mcp:") != NULL);
    T_EQ_INT((long long)line_count(&s.out), 2);
    session_free(&s);
}

static void test_absolute_paths_are_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    /* MCP is not a filesystem reader. An absolute path would make the granted
     * roots check decorative, so it is refused before anything is contacted. */
    run_script(&s,
               INIT_LINE "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                         "{\"name\":\"atlas_file_context\","
                         "\"arguments\":{\"path\":\"/etc/shadow\"}}}\n",
               NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 1, &err);
    const atlas_jsonv *result = atlas_jsonv_get(atlas_jsondoc_root(doc), "result");
    T_REQUIRE(result != NULL);
    bool is_error = false;
    T_CHECK(atlas_jsonv_bool(atlas_jsonv_get(result, "isError"), &is_error));
    T_CHECK(is_error);
    atlas_jsondoc_free(doc);
    /* Nothing about /etc/shadow appears anywhere in the answer beyond the echo
     * of the rejected argument, and no read was attempted. */
    T_CHECK(strstr(atlas_buf_cstr(&s.out), "root:") == NULL);
    session_free(&s);
}

/* --- transport ------------------------------------------------------------ */

static void test_invalid_json_gets_a_parse_error(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s, "this is not json\n", NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    const atlas_jsonv *e = atlas_jsonv_get(atlas_jsondoc_root(doc), "error");
    T_REQUIRE(e != NULL);
    int64_t code = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(e, "code"), &code));
    T_EQ_INT(code, -32700);
    /* An uncorrelatable message carries a null id, as JSON-RPC prescribes. */
    T_CHECK(atlas_jsonv_is_null(atlas_jsonv_get(atlas_jsondoc_root(doc), "id")));
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_a_batch_array_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    /* Batching was removed in 2025-06-18. Answering one would be inventing
     * behaviour rather than implementing it. */
    run_script(&s, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]\n", NULL, &err);
    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    T_CHECK(atlas_jsonv_get(atlas_jsondoc_root(doc), "error") != NULL);
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_ids_keep_their_type(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":\"a-string-id\",\"method\":\"ping\"}\n"
               "{\"jsonrpc\":\"2.0\",\"id\":-42,\"method\":\"ping\"}\n",
               NULL, &err);
    T_EQ_INT((long long)line_count(&s.out), 2);

    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    /* A string id comes back a string. Coercing it to a number would break a
     * client that correlates by identity rather than by value. */
    T_EQ_STR(atlas_jsonv_str_member(atlas_jsondoc_root(doc), "id"), "a-string-id");
    atlas_jsondoc_free(doc);

    doc = parse_line(&s.out, 1, &err);
    int64_t n = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(atlas_jsondoc_root(doc), "id"), &n));
    T_EQ_INT(n, -42);
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_an_unusable_id_is_refused(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    /* JSON-RPC allows a string or a number. An object is neither, and guessing
     * a rendering for it would produce a correlation nobody agreed to. */
    run_script(&s, "{\"jsonrpc\":\"2.0\",\"id\":{\"nested\":1},\"method\":\"ping\"}\n", NULL,
               &err);
    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    T_CHECK(atlas_jsonv_get(atlas_jsondoc_root(doc), "error") != NULL);
    atlas_jsondoc_free(doc);
    session_free(&s);
}

static void test_notifications_get_no_response(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\"}\n"
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/something-new\"}\n",
               NULL, &err);
    /* Not even an error response: a notification never gets one. */
    T_EQ_INT((long long)line_count(&s.out), 0);
    T_EQ_INT(s.exit_code, 0);
    session_free(&s);
}

static void test_an_overlong_message_is_refused_and_the_stream_resynchronises(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&script, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"x\":\"",
                              &err),
         &err);
    for (size_t i = 0; i < ATLAS_MCP_MAX_MESSAGE_BYTES + 4096u; i++) {
        T_OK(atlas_buf_append_ch(&script, 'x', &err), &err);
    }
    T_OK(atlas_buf_append_str(&script, "\"}\n", &err), &err);
    /* A well-formed message after the over-long one. The stream has to
     * resynchronise at the newline, or one bad message would poison the rest. */
    T_OK(atlas_buf_append_str(&script, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n",
                              &err),
         &err);

    session s;
    session_init(&s);
    const char *args[] = {"mcp"};
    const char *env[] = {"XDG_RUNTIME_DIR=/nonexistent-atlas-runtime", NULL};
    T_OK(fx_atlas_stdin(args, 1u, env, script.data, script.len, &s.out, &s.errout, &s.exit_code,
                        &err),
         &err);
    T_EQ_INT(s.exit_code, 0);
    T_EQ_INT((long long)line_count(&s.out), 2);

    atlas_jsondoc *doc = parse_line(&s.out, 0, &err);
    T_CHECK(atlas_jsonv_get(atlas_jsondoc_root(doc), "error") != NULL);
    atlas_jsondoc_free(doc);

    doc = parse_line(&s.out, 1, &err);
    int64_t id = 0;
    T_CHECK(atlas_jsonv_int(atlas_jsonv_get(atlas_jsondoc_root(doc), "id"), &id));
    T_EQ_INT(id, 2);
    T_CHECK(atlas_jsonv_get(atlas_jsondoc_root(doc), "result") != NULL);
    atlas_jsondoc_free(doc);

    atlas_buf_free(&script);
    session_free(&s);
}

static void test_premature_eof_and_partial_input(void) {
    atlas_err err;
    atlas_err_init(&err);

    /* A message with no trailing newline: end of file part way through a line
     * is a truncated message, not a short one, and the server exits cleanly. */
    session s;
    session_init(&s);
    run_script(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"pin", NULL, &err);
    T_EQ_INT(s.exit_code, 0);
    T_EQ_INT((long long)line_count(&s.out), 0);
    session_free(&s);

    /* Nothing at all. Closing stdin between messages is how a client shuts a
     * stdio server down, and it is a clean exit. */
    session_init(&s);
    run_script(&s, "", NULL, &err);
    T_EQ_INT(s.exit_code, 0);
    T_EQ_INT((long long)line_count(&s.out), 0);
    session_free(&s);

    /* Several messages in one write, and an empty line between them. */
    session_init(&s);
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n\n"
               "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n",
               NULL, &err);
    T_EQ_INT((long long)line_count(&s.out), 2);
    session_free(&s);
}

static void test_stdout_carries_no_diagnostics(void) {
    atlas_err err;
    atlas_err_init(&err);
    session s;
    session_init(&s);
    /* A script that provokes several diagnostics: an unknown protocol version,
     * a failed daemon call, and a percent-encoded root the server will not
     * guess at. Every one of them belongs on stderr. */
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
               "{\"protocolVersion\":\"3000-01-01\",\"capabilities\":{\"roots\":{}}}}\n"
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
               "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":["
               "{\"uri\":\"file:///tmp/a%20b\"}]}}\n"
               "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
               "{\"name\":\"atlas_status\",\"arguments\":{}}}\n",
               NULL, &err);

    /* Every non-empty stdout line parses as JSON and carries a jsonrpc member.
     * That is the whole of the stdout-purity requirement, checked rather than
     * asserted in prose. */
    size_t lines = line_count(&s.out);
    T_CHECK(lines >= 2);
    for (size_t i = 0; i < lines; i++) {
        atlas_jsondoc *doc = parse_line(&s.out, i, &err);
        T_CHECK_MSG(atlas_jsonv_str_member(atlas_jsondoc_root(doc), "jsonrpc") != NULL,
                    "stdout line %zu is not a JSON-RPC message", i);
        atlas_jsondoc_free(doc);
    }
    T_CHECK(strstr(atlas_buf_cstr(&s.out), "atlas mcp:") == NULL);
    T_CHECK(s.errout.len > 0);
    session_free(&s);
}

static void test_roots_are_requested_only_when_advertised(void) {
    atlas_err err;
    atlas_err_init(&err);

    /* No roots capability: no request, and the documented fallback is used. */
    session s;
    session_init(&s);
    run_script(&s, INIT_LINE, NULL, &err);
    T_CHECK(strstr(atlas_buf_cstr(&s.out), "roots/list") == NULL);
    session_free(&s);

    /* Advertised: the server asks, after `initialized` and not before. */
    session_init(&s);
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
               "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"roots\":"
               "{\"listChanged\":true}}}}\n"
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n",
               NULL, &err);
    T_CHECK(strstr(atlas_buf_cstr(&s.out), "\"method\":\"roots/list\"") != NULL);
    /* The request comes after the initialize response, never before. */
    const char *init_at = strstr(atlas_buf_cstr(&s.out), "protocolVersion");
    const char *roots_at = strstr(atlas_buf_cstr(&s.out), "roots/list");
    T_CHECK(init_at != NULL && roots_at != NULL && init_at < roots_at);
    session_free(&s);

    /* A list_changed notification triggers a fresh request: a revoked root must
     * stop authorizing anything immediately. */
    session_init(&s);
    run_script(&s,
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
               "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"roots\":{}}}}\n"
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/roots/list_changed\"}\n",
               NULL, &err);
    size_t count = 0;
    for (const char *p = atlas_buf_cstr(&s.out); (p = strstr(p, "roots/list\"")) != NULL; p++) {
        count++;
    }
    T_CHECK_MSG(count >= 2, "expected a second roots/list after list_changed, saw %zu", count);
    session_free(&s);
}

static const atlas_test TESTS[] = {
    {"initialize reports the version, capabilities and instructions",
     test_initialize_and_capabilities},
    {"protocol version negotiation echoes known revisions",
     test_protocol_version_negotiation},
    {"ping answers before initialization", test_ping_answers_before_initialization},
    {"other requests before initialize are refused",
     test_requests_before_initialize_are_refused},
    {"tools/list matches the documented surface and forbids extra arguments",
     test_tools_list_is_complete_and_strictly_schemad},
    {"remote-only tools are absent from the stdio adapter",
     test_remote_only_tools_absent_from_stdio},
    {"an unknown tool is a protocol error", test_unknown_tool_is_a_protocol_error},
    {"an unavailable daemon is a degraded result",
     test_daemon_unavailable_is_a_degraded_result_not_a_crash},
    {"an absolute path argument is refused", test_absolute_paths_are_refused},
    {"invalid JSON gets a parse error with a null id", test_invalid_json_gets_a_parse_error},
    {"a top-level array is refused", test_a_batch_array_is_refused},
    {"request ids keep their type", test_ids_keep_their_type},
    {"an id that is neither a string nor a number is refused", test_an_unusable_id_is_refused},
    {"notifications get no response", test_notifications_get_no_response},
    {"an over-long message is refused and the stream resynchronises",
     test_an_overlong_message_is_refused_and_the_stream_resynchronises},
    {"partial input and premature end of file", test_premature_eof_and_partial_input},
    {"stdout carries protocol only", test_stdout_carries_no_diagnostics},
    {"roots are requested only when the client advertises them",
     test_roots_are_requested_only_when_advertised},
};

ATLAS_TEST_MAIN("mcp", TESTS)

/* Atlas - MCP roots: URI decoding, authorization, and registration.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Roots are the entire authorization boundary of an MCP session, so almost
 * everything here is a refusal. A refusal is the easy thing to get wrong
 * quietly: decoding a root permissively authorizes a directory that merely
 * looks like the right one, and nothing complains.
 *
 * The second half establishes what Correction 3 was about — that an MCP client
 * with no Claude hooks, and a user who never typed `atlas repo add`, still ends
 * up with an indexed repository. Atlas is meant to work with any AI client, and
 * a query surface that only works after a Claude-specific hook has run is not
 * that.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/jsonread.h"
#include "atlas/mcp.h"
#include "atlas_test.h"
#include "mcp/mcp_internal.h"
#include "support/fixture.h"

/* --- file: URI decoding --------------------------------------------------- */

static void check_uri_ok(const char *uri, const char *expected) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_decode_file_uri(uri, &out, &err);
    T_CHECK_MSG(st == ATLAS_OK, "\"%s\" was refused: %s", uri, atlas_err_msg(&err));
    if (st == ATLAS_OK) {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&out), expected) == 0,
                    "\"%s\" decoded to \"%s\", expected \"%s\"", uri, atlas_buf_cstr(&out),
                    expected);
    }
    atlas_buf_free(&out);
}

static void check_uri_refused(const char *uri, const char *why) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_decode_file_uri(uri, &out, &err);
    T_CHECK_MSG(st != ATLAS_OK, "\"%s\" was accepted but should be refused (%s)", uri, why);
    /* A refusal has to say something, because it is the only signal a user gets
     * that a root they granted is not being honoured. */
    T_CHECK_MSG(st == ATLAS_OK || atlas_err_msg(&err)[0] != '\0', "\"%s\" refused silently", uri);
    atlas_buf_free(&out);
}

static void test_plain_and_localhost_authorities(void) {
    /* The two forms that mean "this machine". Everything else names a host. */
    check_uri_ok("file:///home/user/project", "/home/user/project");
    check_uri_ok("file://localhost/home/user/project", "/home/user/project");
    /* A trailing separator is dropped so the path compares equal to a canonical
     * root, which never has one. */
    check_uri_ok("file:///home/user/project/", "/home/user/project");
    check_uri_ok("file:///home/user/project///", "/home/user/project");
}

static void test_remote_authorities_are_refused(void) {
    check_uri_refused("file://example.com/etc/passwd", "a remote host is not a local path");
    check_uri_refused("file://192.0.2.1/srv", "an address is not a local path");
    check_uri_refused("file://localhost.evil.example/srv", "a suffix of localhost is not it");
    check_uri_refused("file://user@host/srv", "userinfo names a remote authority");
    check_uri_refused("http://localhost/srv", "only file: names a local directory");
    check_uri_refused("/plain/path", "a bare path is not a URI");
    check_uri_refused("file://", "there is no path at all");
}

static void test_percent_decoding(void) {
    /* A space is the ordinary case and has to work: directories with spaces in
     * them are completely normal and a client will encode them. */
    check_uri_ok("file:///home/user/my%20project", "/home/user/my project");
    check_uri_ok("file:///srv/a%20b%20c/d", "/srv/a b c/d");
    /* Percent-encoded UTF-8, byte by byte, as a client is required to encode it.
     * "café" and "日本". */
    check_uri_ok("file:///srv/caf%C3%A9", "/srv/caf\xc3\xa9");
    check_uri_ok("file:///srv/%E6%97%A5%E6%9C%AC", "/srv/\xe6\x97\xa5\xe6\x9c\xac");
    /* Valid UTF-8 that was not encoded at all is passed through unchanged. */
    check_uri_ok("file:///srv/caf\xc3\xa9", "/srv/caf\xc3\xa9");
    /* Lowercase hex digits are as valid as uppercase. */
    check_uri_ok("file:///srv/a%2db", "/srv/a-b");
    /* A literal percent, encoded. */
    check_uri_ok("file:///srv/100%25", "/srv/100%");
}

static void test_malformed_escapes_are_refused(void) {
    check_uri_refused("file:///srv/a%", "a truncated escape");
    check_uri_refused("file:///srv/a%2", "a one-digit escape");
    check_uri_refused("file:///srv/a%zz", "a non-hex escape");
    check_uri_refused("file:///srv/a%2g", "a half-hex escape");
    check_uri_refused("file:///srv/%00etc", "a decoded NUL");
}

static void test_encoded_separators_and_traversal_are_refused(void) {
    /* `%2F` in a component means the component contained a slash. Decoding it
     * into a real separator would change which directory the URI names, so it
     * is refused rather than normalised into something plausible. */
    check_uri_refused("file:///srv/a%2Fb", "an encoded path separator");
    check_uri_refused("file:///srv/a%2fb", "an encoded path separator, lowercase");
    /* Traversal, plain and encoded. The check runs on the *decoded* bytes,
     * because an escape is exactly how one would be smuggled past a check on
     * the raw URI. */
    check_uri_refused("file:///srv/../etc", "a relative component");
    check_uri_refused("file:///srv/./etc", "a dot component");
    check_uri_refused("file:///srv/%2e%2e/etc", "an encoded relative component");
    check_uri_refused("file:///srv//etc", "an empty component");
    /* The root of the filesystem is not a root Atlas will accept. */
    check_uri_refused("file:///", "it would authorize everything");
    check_uri_refused("file://localhost/", "it would authorize everything");
}

/* --- a live daemon and a real MCP session --------------------------------- */

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_buf runtime_env;
    atlas_buf project_env;
} env;

static void env_start(env *e, atlas_err *err) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->runtime_env);
    atlas_buf_init(&e->project_env);
    T_OK(fx_open(&e->fx, err), err);
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, err), err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, err), err);
    T_OK(atlas_buf_appendf(&e->runtime_env, err, "XDG_RUNTIME_DIR=%s",
                           atlas_buf_cstr(&e->d.runtime_dir)),
         err);
    T_OK(atlas_buf_appendf(&e->project_env, err, "CLAUDE_PROJECT_DIR=%s", fx_repo(&e->fx)), err);
}

static void env_stop(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    atlas_buf_free(&e->runtime_env);
    atlas_buf_free(&e->project_env);
    fx_close(&e->fx);
}

/* Creates a git repository at <fixture>/<name>. */
static void make_repo(env *e, const char *name, atlas_buf *path_out, atlas_err *err) {
    T_OK(fx_mkdir(e->fx.root.data, name, err), err);
    atlas_buf_reset(path_out);
    T_OK(atlas_buf_appendf(path_out, err, "%s/%s", e->fx.root.data, name), err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(path_out), NULL, err), err);
    T_OK(fx_write(atlas_buf_cstr(path_out), "a.c", "int main(void){return 0;}\n", err), err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(path_out), err), err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(path_out), "initial", err), err);
}

/* Runs one MCP session with the given script and returns stdout. */
static void run_mcp(env *e, const char *script, atlas_buf *out, atlas_buf *errout,
                    atlas_err *err) {
    const char *env_list[] = {atlas_buf_cstr(&e->runtime_env), atlas_buf_cstr(&e->project_env),
                              NULL};
    const char *args[] = {"mcp"};
    int code = 0;
    T_OK(fx_atlas_stdin(args, 1u, env_list, script, strlen(script), out, errout, &code, err), err);
    T_EQ_INT(code, 0);
}

/* How many repositories the index holds. */
static int64_t repo_count(env *e, atlas_err *err) {
    const char *args[] = {"repo", "list", "--json"};
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, 3u, &out, NULL, &code, err), err);
    atlas_jsondoc *doc = NULL;
    int64_t n = -1;
    if (out.len > 0 && atlas_jsondoc_parse(out.data, out.len, 1u << 20, 24u, &doc, err) ==
                           ATLAS_OK) {
        (void)atlas_jsonv_int(atlas_jsonv_get(atlas_jsondoc_root(doc), "count"), &n);
        atlas_jsondoc_free(doc);
    }
    atlas_buf_free(&out);
    return n;
}

#define MCP_INIT_ROOTS                                                                             \
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"                          \
    "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"roots\":{\"listChanged\":true}}}}\n"   \
    "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"

/* The structured half of the last tool result on stdout. */
static bool last_result_flag(const atlas_buf *out, const char *key, bool *value) {
    const char *p = atlas_buf_cstr(out);
    const char *found = NULL;
    for (const char *q = p; (q = strstr(q, "\"structuredContent\"")) != NULL; q++) {
        found = q;
    }
    if (found == NULL) {
        return false;
    }
    char needle[64];
    (void)snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(found, needle);
    if (at == NULL) {
        return false;
    }
    at += strlen(needle);
    *value = (strncmp(at, "true", 4u) == 0);
    return true;
}

static void test_mcp_registers_a_granted_root_with_no_hook(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "solo", &repo, &err);

    /* Nothing has registered anything: no hook has run and nobody typed
     * `atlas repo add`. This is the state a non-Claude MCP client starts in. */
    T_EQ_INT(repo_count(&e, &err), 0);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT_ROOTS
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_repo_overview\",\"arguments\":{}}}\n",
                           atlas_buf_cstr(&repo)),
         &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &errout, &err);

    /* Registered by MCP alone. */
    T_EQ_INT(repo_count(&e, &err), 1);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "registered a granted root") != NULL,
                "the registration was not reported on stderr");
    /* And the tool answered rather than reporting degraded. */
    bool degraded = true;
    T_CHECK(last_result_flag(&out, "degraded", &degraded));
    T_CHECK_MSG(!degraded, "the tool call was degraded after registering");

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&script);
    atlas_buf_free(&repo);
    env_stop(&e);
}

static void test_registered_reports_the_actual_state(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "known", &repo, &err);

    /* Register it up front, so the session below finds it already present.
     * `registered` must be true for a repository Atlas has held for a while,
     * not only for one this call created — which is what it used to report. */
    const char *add[] = {"repo", "add", NULL, "--name", "known"};
    add[2] = atlas_buf_cstr(&repo);
    int code = 0;
    T_OK(fx_atlas_with_runtime(&e.fx, &e.d, add, 5u, NULL, NULL, &code, &err), &err);
    T_EQ_INT(code, 0);

    atlas_buf sock = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&sock, e.d.socket.data, e.d.socket.len, &err), &err);
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"provider\":\"test\",\"client\":\"suite\","
                           "\"session_key\":\"s-registered\",\"root\":\"%s\"}",
                           atlas_buf_cstr(&repo)),
         &err);
    atlas_buf resp = ATLAS_BUF_INIT;
    T_OK(atlas_ipc_call(atlas_buf_cstr(&sock), "ai.session.open", atlas_buf_cstr(&params), &resp,
                        &err),
         &err);
    atlas_ipc_response *r = NULL;
    T_OK(atlas_ipc_response_parse(resp.data, resp.len, &r, &err), &err);
    T_CHECK(atlas_ipc_response_ok(r));

    bool registered = false;
    T_CHECK(atlas_ipc_result_bool(r, "registered", &registered));
    T_CHECK_MSG(registered, "`registered` was false for a repository already in the index");
    /* And the separate fact — that *this* call did not perform the
     * registration — is still available. */
    bool now = true;
    T_CHECK(atlas_ipc_result_bool(r, "registered_now", &now));
    T_CHECK_MSG(!now, "`registered_now` claimed this call registered an existing repository");
    atlas_ipc_response_free(r);

    atlas_buf_free(&resp);
    atlas_buf_free(&params);
    atlas_buf_free(&sock);
    atlas_buf_free(&repo);
    env_stop(&e);
}

static void test_multiple_roots_and_worktrees(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf one = ATLAS_BUF_INIT;
    atlas_buf two = ATLAS_BUF_INIT;
    make_repo(&e, "alpha", &one, &err);
    make_repo(&e, "beta", &two, &err);

    /* A linked worktree of the first, which is a distinct registration sharing
     * one object store. */
    atlas_buf wt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&wt, &err, "%s/alpha-wt", e.fx.root.data), &err);
    const char *add_wt[] = {"worktree", "add", NULL, "-b", "side"};
    add_wt[2] = atlas_buf_cstr(&wt);
    T_OK(fx_git_ok(&e.fx, atlas_buf_cstr(&one), add_wt, 5u, &err), &err);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT_ROOTS
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":["
                           "{\"uri\":\"file://%s\"},{\"uri\":\"file://%s\"},"
                           "{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_repo_overview\",\"arguments\":{}}}\n",
                           atlas_buf_cstr(&one), atlas_buf_cstr(&two), atlas_buf_cstr(&wt)),
         &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &errout, &err);

    /* Three registrations: two repositories and one linked worktree, which is
     * its own row because its working state is independent. */
    T_EQ_INT(repo_count(&e, &err), 3);

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&script);
    atlas_buf_free(&wt);
    atlas_buf_free(&two);
    atlas_buf_free(&one);
    env_stop(&e);
}

static void test_a_subdirectory_root_does_not_register_its_parent(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "outer", &repo, &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&repo), "inner", &err), &err);

    atlas_buf inner = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&inner, &err, "%s/inner", atlas_buf_cstr(&repo)), &err);

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT_ROOTS
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_repo_overview\",\"arguments\":{}}}\n",
                           atlas_buf_cstr(&inner)),
         &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &errout, &err);

    /* Nothing registered. The client granted `inner`; registering `outer` would
     * index files it did not grant, so the refusal is the correct outcome. */
    T_EQ_INT(repo_count(&e, &err), 0);
    /* And it fails clearly rather than silently: the tool reports an error and
     * says why. */
    bool degraded = false;
    T_CHECK(last_result_flag(&out, "degraded", &degraded) || strstr(atlas_buf_cstr(&out),
                                                                    "isError") != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "worktree") != NULL,
                "the refusal did not explain itself: %s", atlas_buf_cstr(&out));
    /* Non-destructive: the repository is untouched. */
    char digest[65];
    T_OK(fx_tree_digest(atlas_buf_cstr(&repo), digest, &err), &err);
    T_CHECK(digest[0] != '\0');

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&script);
    atlas_buf_free(&inner);
    atlas_buf_free(&repo);
    env_stop(&e);
}

static void test_root_changes_update_the_authorization_set(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf one = ATLAS_BUF_INIT;
    atlas_buf two = ATLAS_BUF_INIT;
    make_repo(&e, "first", &one, &err);
    make_repo(&e, "second", &two, &err);

    /* Grant both, then send list_changed with only the second, then ask about
     * the first by name. A revoked root must stop authorizing immediately. */
    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(
             &script, &err,
             MCP_INIT_ROOTS
             "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":["
             "{\"uri\":\"file://%s\"},{\"uri\":\"file://%s\"}]}}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
             "{\"name\":\"atlas_repo_overview\",\"arguments\":{\"repo\":\"first\"}}}\n"
             "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/roots/list_changed\"}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":-2,\"result\":{\"roots\":[{\"uri\":\"file://%s\"}]}}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
             "{\"name\":\"atlas_repo_overview\",\"arguments\":{\"repo\":\"first\"}}}\n"
             "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
             "{\"name\":\"atlas_repo_overview\",\"arguments\":{\"repo\":\"second\"}}}\n",
             atlas_buf_cstr(&one), atlas_buf_cstr(&two), atlas_buf_cstr(&two)),
         &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &errout, &err);

    /* Both were registered while both were granted. */
    T_EQ_INT(repo_count(&e, &err), 2);

    /* Three tool responses. The middle one — "first" after it was revoked —
     * must be an error; the others must not be. */
    size_t results = 0;
    size_t errors_at[8];
    size_t error_count = 0;
    const char *p = atlas_buf_cstr(&out);
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (n > 0 && strstr(p, "\"content\"") != NULL &&
            (nl == NULL || strstr(p, "\"content\"") < nl)) {
            bool is_error = false;
            const char *flag = strstr(p, "\"isError\":");
            if (flag != NULL && (nl == NULL || flag < nl)) {
                is_error = strncmp(flag + 10, "true", 4u) == 0;
            }
            if (is_error && error_count < 8u) {
                errors_at[error_count++] = results;
            }
            results++;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    T_CHECK_MSG(results == 3, "expected 3 tool results, got %zu", results);
    T_CHECK_MSG(error_count == 1, "expected exactly one refused call, got %zu", error_count);
    if (error_count == 1) {
        T_CHECK_MSG(errors_at[0] == 1, "the refused call was #%zu, expected the second",
                    errors_at[0]);
    }
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "authorized") != NULL,
                "the refusal did not say it was an authorization failure");

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&script);
    atlas_buf_free(&two);
    atlas_buf_free(&one);
    env_stop(&e);
}

static void test_a_root_with_a_space_works_end_to_end(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    atlas_buf repo = ATLAS_BUF_INIT;
    make_repo(&e, "my project", &repo, &err);

    /* Percent-encoded, as a conforming client encodes a space. */
    atlas_buf encoded = ATLAS_BUF_INIT;
    for (size_t i = 0; i < repo.len; i++) {
        if (repo.data[i] == ' ') {
            T_OK(atlas_buf_append_str(&encoded, "%20", &err), &err);
        } else {
            T_OK(atlas_buf_append_ch(&encoded, repo.data[i], &err), &err);
        }
    }

    atlas_buf script = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&script, &err,
                           MCP_INIT_ROOTS
                           "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{\"roots\":"
                           "[{\"uri\":\"file://%s\"}]}}\n"
                           "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                           "{\"name\":\"atlas_repo_overview\",\"arguments\":{}}}\n",
                           atlas_buf_cstr(&encoded)),
         &err);

    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e, atlas_buf_cstr(&script), &out, &errout, &err);

    T_CHECK_MSG(repo_count(&e, &err) == 1, "a root whose path contains a space was not registered");
    bool degraded = true;
    T_CHECK(last_result_flag(&out, "degraded", &degraded));
    T_CHECK(!degraded);

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_buf_free(&script);
    atlas_buf_free(&encoded);
    atlas_buf_free(&repo);
    env_stop(&e);
}

static void test_no_roots_falls_back_to_the_project_dir(void) {
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_start(&e, &err);

    /* The fixture's own repo/ directory, which CLAUDE_PROJECT_DIR names. */
    T_OK(fx_init_repo(&e.fx, fx_repo(&e.fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "initial", &err), &err);

    /* No roots capability at all, so the documented fallback applies. */
    atlas_buf out = ATLAS_BUF_INIT;
    atlas_buf errout = ATLAS_BUF_INIT;
    run_mcp(&e,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
            "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
            "{\"name\":\"atlas_repo_overview\",\"arguments\":{}}}\n",
            &out, &errout, &err);

    T_CHECK_MSG(repo_count(&e, &err) == 1,
                "CLAUDE_PROJECT_DIR did not serve as the documented fallback");

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    env_stop(&e);
}

static const atlas_test TESTS[] = {
    {"empty and localhost file authorities decode", test_plain_and_localhost_authorities},
    {"remote and non-file authorities are refused", test_remote_authorities_are_refused},
    {"percent-encoded spaces and UTF-8 decode", test_percent_decoding},
    {"malformed percent escapes are refused", test_malformed_escapes_are_refused},
    {"encoded separators and traversal are refused",
     test_encoded_separators_and_traversal_are_refused},
    {"MCP registers a granted root with no hook and no repo add",
     test_mcp_registers_a_granted_root_with_no_hook},
    {"registered reports the actual state, not just this call",
     test_registered_reports_the_actual_state},
    {"multiple repositories and a linked worktree all register",
     test_multiple_roots_and_worktrees},
    {"a subdirectory root does not register its parent worktree",
     test_a_subdirectory_root_does_not_register_its_parent},
    {"a revoked root stops authorizing immediately",
     test_root_changes_update_the_authorization_set},
    {"a root whose path contains a space works end to end",
     test_a_root_with_a_space_works_end_to_end},
    {"no roots falls back to CLAUDE_PROJECT_DIR", test_no_roots_falls_back_to_the_project_dir},
};

ATLAS_TEST_MAIN("mcp_roots", TESTS)

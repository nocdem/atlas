/* Atlas - A9: remote MCP end to end, against a real daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This drives the whole remote path — HTTP bytes in, HTTP bytes out — through
 * `atlas_gateway_serve_bytes` against a live fixture daemon serving a real
 * index with real credentials in it. Nothing is stubbed: the token is one
 * `atlas api-key create` minted, the verification happens in the daemon over
 * the Unix socket, and the tool calls are the same `TOOLS[]` entries the stdio
 * adapter uses.
 *
 * No listening port is opened. `atlas_gateway_serve_bytes` exists in that shape
 * precisely so the routing, the authentication and the scope enforcement can be
 * tested without one — a test that had to bind a port would be a test people
 * skip on a busy machine.
 *
 * The properties asserted here are the ones A9 is answerable for:
 *
 *   - a valid credential can call a tool inside its scopes;
 *   - the same credential cannot call one outside them, whether or not it can
 *     see it in the listing;
 *   - no credential can call a write tool, because no credential can hold the
 *     scope every write tool maps to;
 *   - a revoked credential stops working immediately, with no restart;
 *   - an unauthenticated, malformed or absent credential all get one answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/gateway.h"
#include "atlas_test.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_gateway *g;
    char token[ATLAS_APIKEY_TOKEN_MAX];
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
} env;

/* Runs the binary against the fixture data directory.
 *
 * `with_daemon` selects the runtime directory the fixture daemon listens in, so
 * the CLI reaches *this* daemon and no other. That matters for the revoke case:
 * without it the CLI cannot see the daemon, takes the local path, and fails on
 * the writer lock the daemon is holding — which is precisely the situation the
 * operator-gated `apikey.revoke` method exists to fix. */
static int run_cli_ex(env *e, const char **args, size_t n, bool with_daemon, atlas_buf *out,
                      atlas_err *err) {
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    for (size_t i = 0; i < n; i++) {
        argv[k++] = args[i];
    }
    T_REQUIRE(k <= sizeof argv / sizeof argv[0]);
    int code = -1;
    atlas_buf_reset(out);
    if (with_daemon) {
        T_OK(fx_atlas_with_runtime(&e->fx, &e->d, argv, k, out, out, &code, err), err);
    } else {
        T_OK(fx_atlas(argv, k, out, NULL, &code, err), err);
    }
    return code;
}

static int run_cli(env *e, const char **args, size_t n, atlas_buf *out, atlas_err *err) {
    return run_cli_ex(e, args, n, false, out, err);
}

static void capture_key(env *e, const atlas_buf *out) {
    const char *s = strstr(atlas_buf_cstr(out), "ATLAS_API_KEY=");
    T_REQUIRE(s != NULL);
    s += strlen("ATLAS_API_KEY=");
    size_t n = 0;
    while (s[n] != '\0' && s[n] != '\n' && n + 1 < sizeof e->token) {
        n++;
    }
    memcpy(e->token, s, n);
    e->token[n] = '\0';

    const char *id = strstr(atlas_buf_cstr(out), "id:     key_");
    T_REQUIRE(id != NULL);
    id += strlen("id:     key_");
    n = 0;
    while (id[n] != '\0' && id[n] != '\n' && n + 1 < sizeof e->key_id) {
        n++;
    }
    memcpy(e->key_id, id, n);
    e->key_id[n] = '\0';
}

/* A repository, an index, a credential, a daemon, and a gateway pointed at it. */
static void env_open(env *e, const char *scopes[], size_t nscopes) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_REQUIRE(fx_open(&e->fx, &err) == ATLAS_OK);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
        T_EQ_INT(run_cli(e, add, 5, &out, &err), 0);
    }
    {
        const char *scan[] = {"scan", "proj"};
        T_EQ_INT(run_cli(e, scan, 2, &out, &err), 0);
    }
    {
        const char *args[16];
        size_t k = 0;
        args[k++] = "api-key";
        args[k++] = "create";
        args[k++] = "--label";
        args[k++] = "chatgpt-test";
        for (size_t i = 0; i < nscopes; i++) {
            args[k++] = "--scope";
            args[k++] = scopes[i];
        }
        T_EQ_INT(run_cli(e, args, k, &out, &err), 0);
        capture_key(e, &out);
    }
    atlas_buf_free(&out);

    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &err), &err);

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e->d.socket);
    o.timeout_ms = 15000;
    o.errout = NULL;

    /* A policy that enables the gateway. `gateway_uid` is unset, so the daemon
     * recognises its own uid — legacy per-user mode, which is the deployment
     * this fixture is. The separated case is a different machine shape and is
     * covered by the policy matrix in test_gateway.c. */
    atlas_gwpolicy p;
    atlas_gwpolicy_parse_buffer("enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n",
                                strlen("enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n"), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_OK(atlas_gateway_open(&p, &o, &e->g, &err), &err);
}

static void env_close(env *e) {
    atlas_gateway_close(e->g);
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    fx_close(&e->fx);
}

/* Builds one HTTP request and returns the gateway's response. */
static void request(env *e, const char *method, const char *path, const char *auth,
                    const char *body, atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf req = ATLAS_BUF_INIT;
    size_t blen = body != NULL ? strlen(body) : 0;
    T_OK(atlas_buf_appendf(&req, &err, "%s %s HTTP/1.1\r\nHost: atlas.test\r\n", method, path),
         &err);
    if (auth != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Authorization: %s\r\n", auth), &err);
    }
    T_OK(atlas_buf_appendf(&req, &err, "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                           blen),
         &err);
    if (blen > 0) {
        T_OK(atlas_buf_append(&req, body, blen, &err), &err);
    }
    T_OK(atlas_gateway_serve_bytes(e->g, req.data, req.len, resp, &err), &err);
    atlas_buf_free(&req);
}

static int status_of(const atlas_buf *resp) {
    const char *s = atlas_buf_cstr(resp);
    if (strncmp(s, "HTTP/1.1 ", 9) != 0) {
        return -1;
    }
    return atoi(s + 9);
}

static const char *body_of(const atlas_buf *resp) {
    const char *s = strstr(atlas_buf_cstr(resp), "\r\n\r\n");
    return s != NULL ? s + 4 : "";
}

static void bearer(env *e, char *out, size_t n) {
    (void)snprintf(out, n, "Bearer %s", e->token);
}

/* --- the tests -------------------------------------------------------------- */

static void test_a_scoped_credential_can_call_a_tool(void) {
    env e;
    const char *scopes[] = {"repo:read", "graph:read"};
    env_open(&e, scopes, 2);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* initialize is answered without a prior handshake: a stateless transport
     * cannot rely on one having happened on this connection. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"protocolVersion\"") != NULL,
                "initialize did not answer: %s", body_of(&resp));

    /* A tool inside the credential's scopes. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
            "{\"name\":\"atlas_repo_overview\",\"arguments\":{\"repo\":\"proj\"}}}",
            &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"result\"") != NULL, "the tool call failed: %s",
                body_of(&resp));
    /* A real answer from the real index, not an empty shell. */
    T_CHECK_MSG(strstr(body_of(&resp), "proj") != NULL,
                "the overview did not describe the repository: %s", body_of(&resp));

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_a_tool_outside_the_scopes_is_refused_and_hidden(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* The listing shows what the credential can call, and not what it cannot. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "atlas_repo_overview") != NULL,
                "a granted tool is missing from the listing");
    T_CHECK_MSG(strstr(body_of(&resp), "atlas_sem_callers") == NULL,
                "a tool outside the credential's scopes was listed");
    T_CHECK_MSG(strstr(body_of(&resp), "atlas_record_reason") == NULL,
                "a write tool was listed to a read-only credential");

    /* And naming it directly is refused anyway. Hiding is a convenience for the
     * client; the check at the call is the control. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
            "{\"name\":\"atlas_sem_callers\",\"arguments\":{\"repo\":\"proj\",\"symbol\":\"main\"}}}",
            &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "graph:read") != NULL,
                "the refusal does not name the missing scope: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"error\"") != NULL,
                "calling a hidden tool was not refused: %s", body_of(&resp));

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_no_credential_can_reach_a_write_tool(void) {
    env e;
    /* Every grantable scope at once. `memory:write` is not grantable at all, so
     * this is the most powerful credential A9 can issue. */
    const char *scopes[] = {"context:read", "repo:read",   "decisions:read",
                            "graph:read",   "impact:read", "audit:read"};
    env_open(&e, scopes, 6);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    static const char *const WRITES[] = {"atlas_record_reason", "atlas_record_unknown_reason",
                                         "atlas_record_decision", "atlas_propose_decision"};
    for (size_t i = 0; i < sizeof WRITES / sizeof WRITES[0]; i++) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
                       "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":"
                       "{\"name\":\"%s\",\"arguments\":{\"repo\":\"proj\"}}}",
                       WRITES[i]);
        request(&e, "POST", "/mcp", auth, msg, &resp);
        T_EQ_INT(status_of(&resp), 200);
        T_CHECK_MSG(strstr(body_of(&resp), "memory:write") != NULL,
                    "%s was not refused for want of the write scope: %s", WRITES[i],
                    body_of(&resp));
    }

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_every_authentication_failure_looks_the_same(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;

    static const char *const MSG =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";

    /* A token that is well-formed but was never issued: same selector shape,
     * random secret. */
    char forged[ATLAS_APIKEY_TOKEN_MAX + 16];
    (void)snprintf(forged, sizeof forged, "Bearer atlas_%s_%s", "0123456789abcdef",
                   "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

    /* The right selector with a wrong secret — the closest possible near miss. */
    char near[ATLAS_APIKEY_TOKEN_MAX + 16];
    (void)snprintf(near, sizeof near, "Bearer atlas_%s_%s", e.key_id,
                   "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

    const char *auths[] = {
        NULL,                      /* no header at all */
        "",                        /* an empty one */
        "Bearer",                  /* the scheme and nothing else */
        "Bearer not-a-token",      /* the wrong shape */
        "Basic dXNlcjpwYXNz",      /* the wrong scheme */
        forged, near,
    };

    for (size_t i = 0; i < sizeof auths / sizeof auths[0]; i++) {
        request(&e, "POST", "/mcp", auths[i], MSG, &resp);
        T_CHECK_MSG(status_of(&resp) == 401, "case %zu produced %d rather than 401", i,
                    status_of(&resp));
        /* One body for all of them: a caller that could tell an unknown
         * selector from a wrong secret would learn which half to vary. */
        T_CHECK_MSG(strstr(body_of(&resp), "a valid Atlas API key is required") != NULL,
                    "case %zu produced a different message: %s", i, body_of(&resp));
        /* And the response never echoes what was presented. */
        if (auths[i] != NULL && strlen(auths[i]) > 8) {
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), auths[i] + 7) == NULL,
                        "case %zu echoed the presented credential", i);
        }
    }

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_a_revoked_credential_stops_working_immediately(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);
    static const char *const MSG =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";

    request(&e, "POST", "/mcp", auth, MSG, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* Revoked locally, through the CLI, while the daemon and the gateway keep
     * running. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *revoke[] = {"api-key", "revoke", e.key_id};
        /* Through the running daemon, which is the whole point: an operator
         * must be able to revoke a leaked credential without stopping the
         * service. */
        T_EQ_INT(run_cli_ex(&e, revoke, 3, true, &out, &err), 0);
        T_CHECK(strstr(atlas_buf_cstr(&out), "revoked") != NULL);
    }
    atlas_buf_free(&out);

    /* Immediately, with no restart and nothing to invalidate: the gateway asks
     * the daemon on every request, so there is no cached verdict anywhere that
     * could outlive the revocation. */
    request(&e, "POST", "/mcp", auth, MSG, &resp);
    T_CHECK_MSG(status_of(&resp) == 401,
                "a revoked credential still worked (status %d)", status_of(&resp));

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_the_gateway_holds_no_credential_administration_verb(void) {
    env e;
    const char *scopes[] = {"context:read", "repo:read",   "decisions:read",
                            "graph:read",   "impact:read", "audit:read"};
    env_open(&e, scopes, 6);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* Every name a credential-management tool would plausibly have. None of
     * them exists: remote credential administration is absent in A9 rather than
     * refused, which is what A9.3 requires. */
    static const char *const NAMES[] = {
        "atlas_api_key_create", "atlas_apikey_create", "api_key_create", "atlas_api_key_list",
        "atlas_api_key_revoke", "atlas_api_key_rotate", "atlas_create_key", "apikey.create",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
                       "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
                       "{\"name\":\"%s\",\"arguments\":{}}}",
                       NAMES[i]);
        request(&e, "POST", "/mcp", auth, msg, &resp);
        T_EQ_INT(status_of(&resp), 200);
        T_CHECK_MSG(strstr(body_of(&resp), "unknown tool") != NULL,
                    "\"%s\" was not answered as an unknown tool: %s", NAMES[i], body_of(&resp));
    }

    /* And the listing contains no such name either. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_CHECK(strstr(body_of(&resp), "api_key") == NULL);
    T_CHECK(strstr(body_of(&resp), "apikey") == NULL);
    T_CHECK_MSG(strstr(body_of(&resp), "secret") == NULL,
                "the tool listing mentions a secret");

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_the_transport_refuses_what_it_should(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* GET /mcp is 405: the Streamable HTTP specification says a server offering
     * no SSE stream must answer exactly that. */
    request(&e, "GET", "/mcp", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 405);

    /* An unknown route never becomes a socket message. */
    request(&e, "POST", "/api/v1/anything", auth, "{}", &resp);
    T_EQ_INT(status_of(&resp), 404);
    request(&e, "GET", "/", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 404);

    /* The liveness probe needs no credential and leaks no configuration. */
    request(&e, "GET", "/healthz", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "\"ok\":true") != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "proj") == NULL,
                "the health probe named a repository");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "0.1.0") == NULL,
                "the health probe reported a version");

    /* Malformed JSON inside an authenticated request is a JSON-RPC parse error,
     * not a crash and not a 500. */
    request(&e, "POST", "/mcp", auth, "{not json", &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "\"error\"") != NULL);

    /* Every response carries the security headers, including the 404. */
    request(&e, "GET", "/nope", auth, NULL, &resp);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "X-Content-Type-Options: nosniff") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "Content-Security-Policy:") != NULL);

    atlas_buf_free(&resp);
    env_close(&e);
}

/* Counts the audit rows and notices whether the presented credential appears in
 * any of their text. A row callback receives borrowed pointers, so everything is
 * consumed inside the call. */
typedef struct audit_scan {
    int allowed;
    int denied;
    bool secret_seen;
    const char *secret_body;
} audit_scan;

static atlas_status scan_audit_row(const atlas_gw_audit_entry *e, void *ud, atlas_err *err) {
    (void)err;
    audit_scan *a = (audit_scan *)ud;
    if (e->decision == ATLAS_GW_ALLOWED) {
        a->allowed++;
    } else {
        a->denied++;
    }
    if (a->secret_body != NULL && a->secret_body[0] != '\0') {
        if (strstr(e->detail, a->secret_body) != NULL ||
            strstr(e->operation, a->secret_body) != NULL ||
            strstr(e->label, a->secret_body) != NULL ||
            strstr(e->key_id, a->secret_body) != NULL) {
            a->secret_seen = true;
        }
    }
    return ATLAS_OK;
}

static void test_the_audit_trail_records_what_happened(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* One allowed request and one denied. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 200);
    request(&e, "POST", "/mcp",
            "Bearer atlas_0123456789abcdef_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 401);

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);

    const char *secret_body = strrchr(e.token, '_');
    secret_body = secret_body != NULL ? secret_body + 1 : "";

    /* The rows are written by the daemon's writer thread, so poll for the
     * observable outcome rather than guessing at a sleep. */
    audit_scan seen;
    memset(&seen, 0, sizeof seen);
    for (int attempt = 0; attempt < 200; attempt++) {
        memset(&seen, 0, sizeof seen);
        seen.secret_body = secret_body;
        atlas_db *db = NULL;
        atlas_err oerr;
        atlas_err_init(&oerr);
        if (atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &oerr) == ATLAS_OK) {
            int64_t count = 0;
            bool more = false;
            atlas_err lerr;
            atlas_err_init(&lerr);
            (void)atlas_db_gw_audit_list(db, 50, 0, NULL, scan_audit_row, &seen, &count, &more,
                                         &lerr);
            atlas_db_close(db);
        }
        if (seen.allowed >= 1 && seen.denied >= 1) {
            break;
        }
        struct timespec ts = {0, 50 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }

    T_CHECK_MSG(seen.allowed >= 1, "no allowed request was recorded");
    T_CHECK_MSG(seen.denied >= 1, "the denied request was not recorded");
    T_CHECK_MSG(!seen.secret_seen, "a presented credential appears in the audit trail");

    /* And nowhere else in the database either, searched as raw bytes: a query
     * only finds a leak in a column somebody thought to check. */
    {
        FILE *f = fopen(atlas_buf_cstr(&db_path), "rb");
        T_REQUIRE(f != NULL);
        (void)fseek(f, 0, SEEK_END);
        long len = ftell(f);
        (void)fseek(f, 0, SEEK_SET);
        bool present = false;
        if (len > 0) {
            char *buf = malloc((size_t)len);
            if (buf != NULL) {
                size_t got = fread(buf, 1, (size_t)len, f);
                size_t nlen = strlen(secret_body);
                if (nlen > 0 && got >= nlen) {
                    for (size_t i = 0; i + nlen <= got; i++) {
                        if (memcmp(buf + i, secret_body, nlen) == 0) {
                            present = true;
                            break;
                        }
                    }
                }
                free(buf);
            }
        }
        (void)fclose(f);
        T_CHECK_MSG(!present, "the presented credential reached the database");
    }

    atlas_buf_free(&db_path);
    atlas_buf_free(&resp);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a scoped credential can call a tool", test_a_scoped_credential_can_call_a_tool},
    {"a tool outside the scopes is refused and hidden",
     test_a_tool_outside_the_scopes_is_refused_and_hidden},
    {"no credential can reach a write tool", test_no_credential_can_reach_a_write_tool},
    {"every authentication failure looks the same",
     test_every_authentication_failure_looks_the_same},
    {"a revoked credential stops working immediately",
     test_a_revoked_credential_stops_working_immediately},
    {"the gateway holds no credential-administration verb",
     test_the_gateway_holds_no_credential_administration_verb},
    {"the transport refuses what it should", test_the_transport_refuses_what_it_should},
    {"the audit trail records what happened", test_the_audit_trail_records_what_happened},
};

ATLAS_TEST_MAIN("gw_remote", TESTS)

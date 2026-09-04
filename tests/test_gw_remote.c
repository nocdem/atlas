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
#include <stdatomic.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "atlas/datadir.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/gateway.h"
#include "atlas/ipc.h"
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
                                         "atlas_record_decision", "atlas_propose_decision",
                                         /* A9.1's one new tool. It writes a
                                          * PROPOSED revision, so it is a write,
                                          * so no A9 credential reaches it. */
                                         "atlas_revise_decision"};
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

/* A16, T1: `atlas api-key create --label L --no-scopes` is the frozen minting
 * command for a remote-disposal credential, and it must work wherever a
 * daemon owns the index — not only offline. `atlas_apikey_create_opts`
 * gained a `no_scopes` field with no wire carrier at first: the CLI's local
 * path set it, but the RPC path
 * (`atlas_service_apikey_create_remote` -> `apikey.create` ->
 * `atlas_writer_apikey`) built its `atlas_apikey_job` and its
 * `atlas_apikey_create_opts` with no memory of the flag, so the same command
 * against a running daemon fell straight into "at least one --scope is
 * required" — the exact refusal `--no-scopes` exists to bypass. This drives
 * that path for real, through the running fixture daemon, with `--data-dir`
 * pointed at an index the daemon already owns. */
static void test_no_scopes_mints_a_disposal_credential_through_the_daemon(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;

    const char *create[] = {"api-key", "create", "--label", "disposal", "--no-scopes"};
    T_EQ_INT(run_cli_ex(&e, create, 5, true, &out, &err), 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "scopes: (none)") != NULL,
                "the daemon-routed --no-scopes create did not print the frozen block: %s",
                atlas_buf_cstr(&out));
    char disposal_id[ATLAS_APIKEY_SELECTOR_HEX + 1];
    {
        const char *s = strstr(atlas_buf_cstr(&out), "id:     " ATLAS_APIKEY_ID_PREFIX);
        T_REQUIRE(s != NULL);
        s += strlen("id:     " ATLAS_APIKEY_ID_PREFIX);
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < sizeof disposal_id) {
            n++;
        }
        memcpy(disposal_id, s, n);
        disposal_id[n] = '\0';
    }

    /* Rotation through the daemon too, with the policy-line reminder. */
    const char *rotate[] = {"api-key", "rotate", disposal_id, "--label", "disposal",
                            "--no-scopes"};
    T_EQ_INT(run_cli_ex(&e, rotate, 6, true, &out, &err), 0);
    T_CHECK_MSG(
        strstr(atlas_buf_cstr(&out), "the policy line remote_dispose_key must now name") != NULL,
        "the daemon-routed --no-scopes rotation did not print the reminder: %s",
        atlas_buf_cstr(&out));

    /* And the refusals still travel correctly against the daemon: an ordinary
     * caller cannot bypass "at least one --scope" by omission. */
    const char *bare[] = {"api-key", "create", "--label", "x"};
    int code = run_cli_ex(&e, bare, 4, true, &out, &err);
    T_CHECK_MSG(code != 0, "a scopeless create with no --no-scopes succeeded against the daemon");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "--no-scopes") != NULL,
                "the daemon-routed refusal did not mention --no-scopes: %s",
                atlas_buf_cstr(&out));

    atlas_buf_free(&out);
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

/* --- the web API ------------------------------------------------------------ */

static void test_the_web_api_reads_and_refuses(void) {
    env e;
    const char *scopes[] = {"repo:read", "decisions:read"};
    env_open(&e, scopes, 2);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* A route inside the credential's scopes, answering from the real index. */
    request(&e, "GET", "/api/v1/repos", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "proj") != NULL, "the listing did not name the repository: %s",
                body_of(&resp));

    request(&e, "GET", "/api/v1/status", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* A parameter the route declares, percent-encoded. */
    request(&e, "GET", "/api/v1/repo?repo=proj", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "proj") != NULL);

    /* A route outside the credential's scopes: 403, naming the scope. */
    request(&e, "GET", "/api/v1/sem/status?repo=proj", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 403);
    T_CHECK_MSG(strstr(body_of(&resp), "graph:read") != NULL,
                "the refusal does not name the missing scope: %s", body_of(&resp));

    request(&e, "GET", "/api/v1/audit", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 403);

    /* Unauthenticated. */
    request(&e, "GET", "/api/v1/repos", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);
    T_CHECK(strstr(atlas_buf_cstr(&resp), "WWW-Authenticate: Bearer") != NULL);

    /* Every route is a read: a POST to one is not a different operation. */
    request(&e, "POST", "/api/v1/repos", auth, "{}", &resp);
    T_EQ_INT(status_of(&resp), 405);

    /* A path that names no route never becomes a socket message. */
    request(&e, "GET", "/api/v1/nope", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 404);
    request(&e, "GET", "/api/v1/../mcp", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 404);

    /* A missing repository is 404 rather than 500: Atlas' status vocabulary
     * mapped onto HTTP, so a caller need not read the body to know what
     * happened. */
    request(&e, "GET", "/api/v1/repo?repo=nosuchrepo", auth, NULL, &resp);
    T_CHECK_MSG(status_of(&resp) == 404, "an unknown repository produced %d", status_of(&resp));

    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_the_api_forwards_only_what_a_route_declares(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* A parameter the route does not declare is ignored, not forwarded. The
     * request still succeeds, which is what proves it was dropped rather than
     * passed through and rejected downstream. */
    request(&e, "GET", "/api/v1/repo?repo=proj&full=true&method=decision.approve", auth, NULL,
            &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "proj") != NULL);

    /* A numeric parameter that is not a number is refused at the edge rather
     * than reaching the daemon as a string. */
    request(&e, "GET", "/api/v1/events?repo=proj&limit=notanumber", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 400);

    /* A malformed percent escape is a refusal, never a guess. */
    request(&e, "GET", "/api/v1/repo?repo=pro%zz", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 400);

    /* A decoded NUL is refused: a value that ends early is a different value
     * from the one that was sent. */
    request(&e, "GET", "/api/v1/repo?repo=proj%00extra", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 400);

    atlas_buf_free(&resp);
    env_close(&e);
}

/* --- the browser surface ---------------------------------------------------- */

/* Opens a gateway with the web GUI on, over the same fixture daemon. */
static void gui_env(env *e, atlas_gateway **g) {
    atlas_err err;
    atlas_err_init(&err);
    static const char *const TEXT =
        "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\nweb_gui = yes\n";
    atlas_gwpolicy p;
    atlas_gwpolicy_parse_buffer(TEXT, strlen(TEXT), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e->d.socket);
    o.timeout_ms = 15000;
    T_OK(atlas_gateway_open(&p, &o, g, &err), &err);
}

/* Pulls the session cookie value out of a response's Set-Cookie header. */
static void cookie_of(const atlas_buf *resp, char *out, size_t n) {
    out[0] = '\0';
    const char *s = strstr(atlas_buf_cstr(resp), "Set-Cookie: atlas_session=");
    if (s == NULL) {
        return;
    }
    s += strlen("Set-Cookie: atlas_session=");
    size_t k = 0;
    while (s[k] != '\0' && s[k] != ';' && s[k] != '\r' && k + 1 < n) {
        k++;
    }
    memcpy(out, s, k);
    out[k] = '\0';
}

static void gui_request(atlas_gateway *g, const char *method, const char *path,
                        const char *cookie, const char *body, atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf req = ATLAS_BUF_INIT;
    size_t blen = body != NULL ? strlen(body) : 0;
    T_OK(atlas_buf_appendf(&req, &err, "%s %s HTTP/1.1\r\nHost: t\r\n", method, path), &err);
    if (cookie != NULL && cookie[0] != '\0') {
        T_OK(atlas_buf_appendf(&req, &err, "Cookie: atlas_session=%s\r\n", cookie), &err);
    }
    T_OK(atlas_buf_appendf(&req, &err,
                           "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                           blen),
         &err);
    if (blen > 0) {
        T_OK(atlas_buf_append(&req, body, blen, &err), &err);
    }
    T_OK(atlas_gateway_serve_bytes(g, req.data, req.len, resp, &err), &err);
    atlas_buf_free(&req);
}

/* `gui_env` / `gui_env_anon` never set `listen_addr` or `listen_port`, so the
 * policy's documented defaults apply — this is what a request must send as
 * `Host` to pass `host_matches_listener`. */
#define GW_TEST_HOST "127.0.0.1:8787"

/* Like `gui_request`, but able to carry a bearer credential alongside — or
 * instead of — a session cookie, and able to name (or omit) a `Host` header.
 * Neither `auth` nor `cookie` is ever both non-NULL in a real client, but the
 * anonymous-floor tests need to construct exactly that shape on purpose: a
 * presented, wrong credential and a request with none at all are different
 * things, and only a helper that can build both proves it. `host == NULL`
 * omits the `Host` header entirely, for the HTTP/1.0-shaped case; any other
 * value, including "", is sent verbatim. */
static void full_request(atlas_gateway *g, const char *method, const char *path, const char *host,
                         const char *auth, const char *cookie, const char *body, atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf req = ATLAS_BUF_INIT;
    size_t blen = body != NULL ? strlen(body) : 0;
    T_OK(atlas_buf_appendf(&req, &err, "%s %s HTTP/1.1\r\n", method, path), &err);
    if (host != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Host: %s\r\n", host), &err);
    }
    if (auth != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Authorization: %s\r\n", auth), &err);
    }
    if (cookie != NULL && cookie[0] != '\0') {
        T_OK(atlas_buf_appendf(&req, &err, "Cookie: atlas_session=%s\r\n", cookie), &err);
    }
    T_OK(atlas_buf_appendf(&req, &err,
                           "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                           blen),
         &err);
    if (blen > 0) {
        T_OK(atlas_buf_append(&req, body, blen, &err), &err);
    }
    T_OK(atlas_gateway_serve_bytes(g, req.data, req.len, resp, &err), &err);
    atlas_buf_free(&req);
}

/* Opens a gateway with the web GUI on and a named anonymous floor, over the
 * same fixture daemon. */
static void gui_env_anon(env *e, const char *scopes, atlas_gateway **g) {
    atlas_err err;
    atlas_err_init(&err);
    char text[512];
    (void)snprintf(text, sizeof text,
                   "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\nweb_gui = yes\n"
                   "web_gui_anonymous_scopes = %s\n",
                   scopes);
    atlas_gwpolicy p;
    atlas_gwpolicy_parse_buffer(text, strlen(text), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e->d.socket);
    o.timeout_ms = 15000;
    T_OK(atlas_gateway_open(&p, &o, g, &err), &err);
}

static void test_no_anonymous_scopes_named_means_no_change(void) {
    /* `web_gui = yes` with no `web_gui_anonymous_scopes` key must behave
     * exactly as before this change: a credential-less request to `/api/` or
     * `/auth/me` is still 401. Absent is absent. */
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_gateway *g = NULL;
    gui_env(&e, &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);
    full_request(g, "GET", "/auth/me", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);
    /* A stale or forged session cookie is exactly the input the anonymous-
     * floor decision changed the handling of; with no floor configured it
     * must still be 401, unchanged. */
    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST, NULL,
                "0000000000000000000000000000000000000000000000000000000000000000", NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    atlas_gateway_close(g);
    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_the_anonymous_floor_grants_exactly_the_named_scopes(void) {
    env e;
    const char *scopes[] = {"repo:read", "decisions:read"};
    env_open(&e, scopes, 2);
    atlas_gateway *g = NULL;
    gui_env_anon(&e, "repo:read", &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    /* The served page actually reads the field the server now sends: a
     * consumer that ignored `me.anonymous` would hide the login form on any
     * 200 from /auth/me, forever trapping an anonymous viewer at the floor. */
    gui_request(g, "GET", "/", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "me.anonymous") != NULL,
                "the page does not read the anonymous field /auth/me now sends");

    /* No credential at all: the named scope works... */
    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "proj") != NULL);

    /* ...and nothing beyond it: audit:read is never a default, whatever else
     * is named, and decisions:read was not named even though the real key
     * holds it. */
    full_request(g, "GET", "/api/v1/audit", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 403);
    full_request(g, "GET", "/api/v1/decisions?repo=proj", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 403);
    T_CHECK(strstr(body_of(&resp), "decisions:read") != NULL);

    /* /auth/me tells the truth: it is a real, named identity, not a pretence
     * that nobody is there. */
    full_request(g, "GET", "/auth/me", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"anonymous\":true") != NULL,
                "/auth/me did not report the anonymous floor: %s", body_of(&resp));
    T_CHECK(strstr(body_of(&resp), " (anonymous)") != NULL);
    T_CHECK_MSG(strstr(body_of(&resp), "\"scopes\":\"repo:read\"") != NULL,
                "/auth/me did not report exactly the named scope: %s", body_of(&resp));

    /* A wrong bearer token is not "no credential": it tried and failed, and it
     * stays failed rather than sliding to the anonymous floor. */
    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST,
                "Bearer atlas_0123456789abcdef_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* A stale or forged session cookie authenticates to nothing and carries no
     * identity to lose — it lands on the same floor as no cookie at all, which
     * is what lets a browser holding a cookie from before a gateway restart
     * keep working rather than being stuck at a hard 401 until it logs out by
     * hand. */
    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST, NULL,
                "0000000000000000000000000000000000000000000000000000000000000000", NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    full_request(g, "GET", "/auth/me", GW_TEST_HOST, NULL,
                "0000000000000000000000000000000000000000000000000000000000000000", NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "\"anonymous\":true") != NULL);

    /* Naming an anonymous floor never removes the ability to authenticate for
     * more, and a real session is never masked down to it. */
    char body[512];
    (void)snprintf(body, sizeof body, "{\"key\":\"%s\"}", e.token);
    full_request(g, "POST", "/auth/login", GW_TEST_HOST, NULL, NULL, body, &resp);
    T_EQ_INT(status_of(&resp), 200);
    char cookie[128];
    cookie_of(&resp, cookie, sizeof cookie);
    T_REQUIRE_MSG(cookie[0] != '\0', "login set no session cookie");

    full_request(g, "GET", "/auth/me", GW_TEST_HOST, NULL, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"anonymous\":false") != NULL,
                "a real session was reported as anonymous: %s", body_of(&resp));
    T_CHECK(strstr(body_of(&resp), "chatgpt-test") != NULL);
    /* The real key's own scopes, wider than the anonymous floor — the floor
     * never overrides a session that exists. */
    T_CHECK(strstr(body_of(&resp), "decisions:read") != NULL);
    full_request(g, "GET", "/api/v1/decisions?repo=proj", GW_TEST_HOST, NULL, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* `/mcp` is untouched: no bearer is still refused, with or without the
     * anonymous floor configured, and a cookie never reaches it at all. */
    full_request(g, "POST", "/mcp", GW_TEST_HOST, NULL, NULL,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 401);
    full_request(g, "POST", "/mcp", GW_TEST_HOST, NULL, cookie,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 401);
    char auth[256];
    bearer(&e, auth, sizeof auth);
    full_request(g, "POST", "/mcp", GW_TEST_HOST, auth, NULL,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_EQ_INT(status_of(&resp), 200);

    atlas_gateway_close(g);
    atlas_buf_free(&resp);
    env_close(&e);
}

static atlas_status scan_anon_audit_row(const atlas_gw_audit_entry *e, void *ud, atlas_err *err) {
    (void)err;
    int *count = (int *)ud;
    if (e->decision == ATLAS_GW_ALLOWED && strcmp(e->key_id, "anonymous") == 0 &&
        strcmp(e->label, " (anonymous)") == 0) {
        (*count)++;
    }
    return ATLAS_OK;
}

static void test_the_audit_trail_names_an_anonymous_request_plainly(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_gateway *g = NULL;
    gui_env_anon(&e, "repo:read", &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e.fx)), &err);

    int seen = 0;
    for (int attempt = 0; attempt < 200 && seen < 1; attempt++) {
        seen = 0;
        atlas_db *db = NULL;
        atlas_err oerr;
        atlas_err_init(&oerr);
        if (atlas_db_open_readonly(atlas_buf_cstr(&db_path), &db, &oerr) == ATLAS_OK) {
            int64_t count = 0;
            bool more = false;
            atlas_err lerr;
            atlas_err_init(&lerr);
            (void)atlas_db_gw_audit_list(db, 50, 0, "anonymous", scan_anon_audit_row, &seen, &count,
                                         &more, &lerr);
            atlas_db_close(db);
        }
        if (seen < 1) {
            struct timespec ts = {0, 50 * 1000 * 1000};
            (void)nanosleep(&ts, NULL);
        }
    }
    T_CHECK_MSG(seen >= 1,
                "no gw_audit row named the anonymous principal plainly (key_id=\"anonymous\")");

    atlas_buf_free(&db_path);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    env_close(&e);
}

static void test_the_host_check_blocks_dns_rebinding(void) {
    /* The rebinding scenario an adversarial review found: a page served under
     * an attacker-controlled name with a short-TTL DNS record can be pointed
     * at this gateway's real address after the browser loads it. The browser
     * then treats a fetch from that page as same-origin with itself — no
     * `Origin` header, no CORS check, and no `atlas_session` cookie, since
     * none was ever set for the attacker's name — so the request presents
     * nothing, which is exactly what the anonymous floor accepted before this
     * check existed. `host_matches_listener` is what closes it. */
    env e;
    const char *scopes[] = {"repo:read", "decisions:read"};
    env_open(&e, scopes, 2);
    atlas_gateway *g = NULL;
    gui_env_anon(&e, "repo:read", &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    /* A Host that is not this listener: refused. This is the attack itself.
     * Fails (wrongly reports 200) if `host_matches_listener` is removed from
     * `anonymous_ok`, or if the comparison is ever loosened to prefix or
     * suffix matching -- e.g. accepting "127.0.0.1:8787.attacker.example" or
     * "evil-127.0.0.1:8787" the way a suffix/prefix bug would. */
    full_request(g, "GET", "/api/v1/repos", "attacker.example:8799", NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* The suffix shape by name, since it is the attack the comparison rule
     * exists to name: a hostname that merely *ends with* the listener's own
     * value. Fails if the comparison is ever done with a suffix check (e.g.
     * `strstr` from the end, or comparing only the last N bytes) instead of
     * whole-string equality. */
    full_request(g, "GET", "/api/v1/repos", "127.0.0.1:8787.attacker.example", NULL, NULL, NULL,
                &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* A prefix of the real port. Fails if the port comparison is ever done
     * with something like `strncmp` against the numeric prefix instead of
     * the whole "addr:port" string, which "87" being a prefix of "8787"
     * would slip past. */
    full_request(g, "GET", "/api/v1/repos", "127.0.0.1:87", NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* No Host at all, on an HTTP/1.1 request: refused, not guessed. Fails if
     * an absent Host is ever treated as a match -- e.g. a `host == NULL` or
     * empty-string short circuit that returns true instead of false. */
    full_request(g, "GET", "/api/v1/repos", NULL, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* The literal case the reviewer named: an HTTP/1.0 request, which sends
     * no Host header at all -- not merely a client choosing to omit it, but
     * the one real-world shape that has never carried one. Built by hand
     * because `full_request` always writes "HTTP/1.1". Fails the same way
     * the case above would: an empty parsed Host must never read as a match. */
    {
        atlas_err herr;
        atlas_err_init(&herr);
        atlas_buf req10 = ATLAS_BUF_INIT;
        static const char REQ10[] =
            "GET /api/v1/repos HTTP/1.0\r\nContent-Length: 0\r\n\r\n";
        T_OK(atlas_buf_append(&req10, REQ10, sizeof REQ10 - 1u, &herr), &herr);
        T_OK(atlas_gateway_serve_bytes(g, req10.data, req10.len, &resp, &herr), &herr);
        T_EQ_INT(status_of(&resp), 401);
        atlas_buf_free(&req10);
    }

    /* The listener's own address and port: served. Fails if the comparison is
     * ever stricter than the policy's own listen_addr/listen_port -- e.g.
     * requiring a scheme prefix, or comparing case-sensitively against a
     * differently-cased but equal hostname. */
    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST, NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* A trailing space in the header value: the HTTP parser trims it before
     * `host_matches_listener` ever sees the value, so this documents that
     * the trimming is the parser's job, not the comparison's. Fails if a
     * future parser change stops trimming header-value whitespace and this
     * silently starts failing closed instead of continuing to match. */
    full_request(g, "GET", "/api/v1/repos", GW_TEST_HOST " ", NULL, NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* A live session is unaffected by Host, on every shape above -- checked,
     * not assumed: a session cookie could only ever have been minted by this
     * gateway's own /auth/login, so it is judged on its own merits regardless
     * of what Host a request claims. Fails if a Host check is ever added
     * ahead of `session_get` instead of only inside `anonymous_ok`. */
    char body[512];
    (void)snprintf(body, sizeof body, "{\"key\":\"%s\"}", e.token);
    full_request(g, "POST", "/auth/login", GW_TEST_HOST, NULL, NULL, body, &resp);
    T_EQ_INT(status_of(&resp), 200);
    char cookie[128];
    cookie_of(&resp, cookie, sizeof cookie);
    T_REQUIRE_MSG(cookie[0] != '\0', "login set no session cookie");

    full_request(g, "GET", "/api/v1/repos", "attacker.example:8799", NULL, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    full_request(g, "GET", "/api/v1/repos", NULL, NULL, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* The scheme-default-port omission clause has its own branch in
     * `host_matches_listener` that none of the cases above reach, because
     * every fixture policy binds the non-default port 8787. Exercised here
     * against a policy bound to port 80. Fails if the literal port compared
     * is wrong, or if the omitted-port form is honoured on a non-80 port. */
    {
        atlas_gwpolicy p80;
        static const char *const TEXT80 =
            "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\nweb_gui = yes\n"
            "listen_port = 80\nweb_gui_anonymous_scopes = repo:read\n";
        atlas_gwpolicy_parse_buffer(TEXT80, strlen(TEXT80), &p80);
        T_REQUIRE(p80.state == ATLAS_GWPOLICY_ENABLED);
        atlas_gateway_opts o80;
        memset(&o80, 0, sizeof o80);
        o80.socket_path = atlas_buf_cstr(&e.d.socket);
        o80.timeout_ms = 15000;
        atlas_gateway *g80 = NULL;
        atlas_err err80;
        atlas_err_init(&err80);
        T_OK(atlas_gateway_open(&p80, &o80, &g80, &err80), &err80);

        full_request(g80, "GET", "/api/v1/repos", "127.0.0.1", NULL, NULL, NULL, &resp);
        T_EQ_INT(status_of(&resp), 200);
        full_request(g80, "GET", "/api/v1/repos", "127.0.0.1:80", NULL, NULL, NULL, &resp);
        T_EQ_INT(status_of(&resp), 200);
        full_request(g80, "GET", "/api/v1/repos", "127.0.0.1:8080", NULL, NULL, NULL, &resp);
        T_EQ_INT(status_of(&resp), 401);

        atlas_gateway_close(g80);
    }

    atlas_gateway_close(g);
    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_the_browser_exchanges_a_key_for_a_session(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_gateway *g = NULL;
    gui_env(&e, &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    /* The page is served from the binary. */
    gui_request(g, "GET", "/", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "Atlas Mission Control") != NULL);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "Content-Security-Policy:") != NULL,
                "the page was served without a content security policy");

    /* No session yet. */
    gui_request(g, "GET", "/auth/me", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* A wrong key is refused and sets no cookie. */
    gui_request(g, "POST", "/auth/login", NULL,
                "{\"key\":\"atlas_0123456789abcdef_"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}",
                &resp);
    T_EQ_INT(status_of(&resp), 401);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "Set-Cookie") == NULL,
                "a refused login set a session cookie");

    /* The real key is exchanged for a session. */
    char body[512];
    (void)snprintf(body, sizeof body, "{\"key\":\"%s\"}", e.token);
    gui_request(g, "POST", "/auth/login", NULL, body, &resp);
    T_EQ_INT(status_of(&resp), 200);

    char cookie[128];
    cookie_of(&resp, cookie, sizeof cookie);
    T_CHECK_MSG(cookie[0] != '\0', "login set no session cookie");
    T_CHECK_MSG(strlen(cookie) == 64u, "the session token is %zu characters", strlen(cookie));
    /* The cookie must be unreadable by script and not sent cross-site. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HttpOnly") != NULL, "the session cookie is readable by script");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "SameSite=Strict") != NULL,
                "the session cookie is sent cross-site");
    /* And the API key must not come back in the response. */
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), e.token) == NULL, "login echoed the API key");

    /* The session is the same principal a bearer token would be. */
    gui_request(g, "GET", "/auth/me", cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "repo:read") != NULL);
    T_CHECK(strstr(body_of(&resp), "chatgpt-test") != NULL);

    /* It authorises the API exactly as the bearer token does — and no more. */
    gui_request(g, "GET", "/api/v1/repos", cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "proj") != NULL);
    gui_request(g, "GET", "/api/v1/audit", cookie, NULL, &resp);
    T_CHECK_MSG(status_of(&resp) == 403, "a session granted a scope its key did not hold");

    /* A forged session token is refused. */
    gui_request(g, "GET", "/auth/me",
                "0000000000000000000000000000000000000000000000000000000000000000", NULL, &resp);
    T_EQ_INT(status_of(&resp), 401);

    /* Logout ends it immediately. */
    gui_request(g, "POST", "/auth/logout", cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    gui_request(g, "GET", "/auth/me", cookie, NULL, &resp);
    T_CHECK_MSG(status_of(&resp) == 401, "a session survived logout");

    atlas_gateway_close(g);
    atlas_buf_free(&resp);
    env_close(&e);
}

static void test_the_browser_surface_is_absent_when_disabled(void) {
    /* `web_gui = no` is the default, and the routes do not exist rather than
     * refusing: a gateway serving only remote MCP has no browser surface at
     * all. */
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* e.g is the remote-MCP-only gateway from env_open. */
    request(&e, "GET", "/", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 404);
    request(&e, "POST", "/auth/login", NULL, "{\"key\":\"x\"}", &resp);
    T_EQ_INT(status_of(&resp), 404);
    request(&e, "GET", "/auth/me", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 404);

    atlas_buf_free(&resp);
    env_close(&e);
}

/* --- the listener ----------------------------------------------------------
 *
 * Everything above drives `atlas_gateway_serve_bytes` directly, which is where
 * all the behaviour is. This one proves the socket loop around it actually
 * binds, accepts, reads a request off the wire and writes a response back — the
 * part that cannot be tested by handing it bytes.
 */

typedef struct listener {
    atlas_gateway *g;
    atomic_bool stop;
    pthread_t thread;
} listener;

static void *listener_main(void *ud) {
    listener *l = (listener *)ud;
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_gateway_serve(l->g, &l->stop, &err);
    return NULL;
}

/* Sends one raw request to a listening gateway and returns the raw response. */
static bool wire_request(int port, const char *request, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(out);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        (void)close(fd);
        return false;
    }
    size_t len = strlen(request);
    if (send(fd, request, len, 0) != (ssize_t)len) {
        (void)close(fd);
        return false;
    }
    for (;;) {
        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) {
            break;
        }
        if (atlas_buf_append(out, chunk, (size_t)n, &err) != ATLAS_OK) {
            break;
        }
    }
    (void)close(fd);
    return out->len > 0;
}

static void test_the_listener_binds_and_serves(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);

    /* A port high enough to be unprivileged and unlikely to collide. A
     * collision makes the bind fail, which is reported rather than retried:
     * silently moving to another port would make the test pass while measuring
     * something else. */
    const int port = 38787;
    atlas_gwpolicy p;
    char text[256];
    (void)snprintf(text, sizeof text,
                   "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n"
                   "listen_addr = 127.0.0.1\nlisten_port = %d\n",
                   port);
    atlas_gwpolicy_parse_buffer(text, strlen(text), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e.d.socket);
    o.timeout_ms = 15000;

    atlas_err err;
    atlas_err_init(&err);
    listener l;
    memset(&l, 0, sizeof l);
    atomic_store(&l.stop, false);
    T_OK(atlas_gateway_open(&p, &o, &l.g, &err), &err);
    T_REQUIRE(pthread_create(&l.thread, NULL, listener_main, &l) == 0);

    /* Wait for the listener rather than sleeping a guessed interval. */
    atlas_buf resp = ATLAS_BUF_INIT;
    bool up = false;
    for (int i = 0; i < 200 && !up; i++) {
        up = wire_request(port, "GET /healthz HTTP/1.1\r\nHost: t\r\n\r\n", &resp);
        if (!up) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            (void)nanosleep(&ts, NULL);
        }
    }
    T_CHECK_MSG(up, "the gateway never accepted a connection on port %d", port);
    if (up) {
        T_CHECK(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 200") != NULL);
        T_CHECK(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL);

        /* An authenticated tool call, over a real socket. */
        char req[1024];
        static const char BODY[] =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
        (void)snprintf(req, sizeof req,
                       "POST /mcp HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\n"
                       "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                       e.token, sizeof BODY - 1u, BODY);
        T_CHECK(wire_request(port, req, &resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 200") != NULL,
                    "the wire request was refused: %s", atlas_buf_cstr(&resp));
        T_CHECK(strstr(atlas_buf_cstr(&resp), "atlas_repo_overview") != NULL);

        /* And an unauthenticated one over the same socket. */
        T_CHECK(wire_request(port,
                             "POST /mcp HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\n{}",
                             &resp));
        T_CHECK(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 401") != NULL);
        T_CHECK(strstr(atlas_buf_cstr(&resp), "WWW-Authenticate: Bearer") != NULL);
    }

    atomic_store(&l.stop, true);
    (void)pthread_join(l.thread, NULL);
    atlas_gateway_close(l.g);
    atlas_buf_free(&resp);
    env_close(&e);
}

/* Many connections at once, over a real socket.
 *
 * Every test above drives one request at a time, which is structurally unable
 * to find a data race — and there were two: an unsynchronised read-modify-write
 * on the rate-limit window, and one `atlas_safe_pool` shared between connection
 * threads. A pool is a ring of scratch buffers with a mutable cursor, so
 * sharing it does not merely race, it hands two threads the same slot.
 *
 * This is the test that exercises the loop the way a client fleet would. It is
 * worth little without ThreadSanitizer and everything with it. */
typedef struct hammer {
    int port;
    const char *token;
    int ok;
    int refused;
} hammer;

static void *hammer_main(void *ud) {
    hammer *h = (hammer *)ud;
    atlas_buf resp = ATLAS_BUF_INIT;
    for (int i = 0; i < 12; i++) {
        char req[1024];
        static const char BODY[] =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
        (void)snprintf(req, sizeof req,
                       "POST /mcp HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\n"
                       "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                       h->token, sizeof BODY - 1u, BODY);
        if (wire_request(h->port, req, &resp)) {
            if (strstr(atlas_buf_cstr(&resp), "HTTP/1.1 200") != NULL) {
                h->ok++;
            } else {
                h->refused++;
            }
        }
        /* An unauthenticated one too, so the denied path — which logs and
         * audits — runs concurrently with the allowed one. That is where the
         * shared pool was reached. */
        (void)wire_request(h->port,
                           "POST /mcp HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer bad\r\n"
                           "Content-Length: 2\r\n\r\n{}",
                           &resp);
    }
    atlas_buf_free(&resp);
    return NULL;
}

static void test_concurrent_connections_are_safe(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);

    const int port = 38791;
    char text[256];
    (void)snprintf(text, sizeof text,
                   "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n"
                   "listen_addr = 127.0.0.1\nlisten_port = %d\n",
                   port);
    atlas_gwpolicy p;
    atlas_gwpolicy_parse_buffer(text, strlen(text), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e.d.socket);
    o.timeout_ms = 15000;

    atlas_err err;
    atlas_err_init(&err);
    listener l;
    memset(&l, 0, sizeof l);
    T_OK(atlas_gateway_open(&p, &o, &l.g, &err), &err);
    T_REQUIRE(pthread_create(&l.thread, NULL, listener_main, &l) == 0);

    atlas_buf probe = ATLAS_BUF_INIT;
    bool up = false;
    for (int i = 0; i < 200 && !up; i++) {
        up = wire_request(port, "GET /healthz HTTP/1.1\r\nHost: t\r\n\r\n", &probe);
        if (!up) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            (void)nanosleep(&ts, NULL);
        }
    }
    atlas_buf_free(&probe);
    T_REQUIRE(up);

    enum { CLIENTS = 6 };
    pthread_t threads[CLIENTS];
    hammer hs[CLIENTS];
    for (int i = 0; i < CLIENTS; i++) {
        memset(&hs[i], 0, sizeof hs[i]);
        hs[i].port = port;
        hs[i].token = e.token;
        T_REQUIRE(pthread_create(&threads[i], NULL, hammer_main, &hs[i]) == 0);
    }
    int ok = 0;
    for (int i = 0; i < CLIENTS; i++) {
        (void)pthread_join(threads[i], NULL);
        ok += hs[i].ok;
    }

    /* Every authenticated request must have been answered. The rate ceiling is
     * far above what this sends, so a refusal here would mean the counter was
     * corrupted rather than that the limit was reached. */
    T_CHECK_MSG(ok == CLIENTS * 12, "%d of %d concurrent authenticated requests succeeded", ok,
                CLIENTS * 12);

    atomic_store(&l.stop, true);
    (void)pthread_join(l.thread, NULL);
    atlas_gateway_close(l.g);
    env_close(&e);
}

/* --- A9.1: the knowledge dimension over the remote surfaces --------------- */

/* Both filters over the web API, and the one new tool refused for want of a scope
 * no credential can hold.
 *
 * The point of the route half is that a query parameter reaches a daemon call only
 * because a row in `API_ROUTES[]` names it, so a filter that works is evidence the
 * row is right — and a *misspelt* kind must be a refusal rather than an empty
 * result, because an empty result reads as an answer. */
static void test_the_api_exposes_the_knowledge_dimension(void) {
    env e;
    const char *scopes[] = {"decisions:read"};
    env_open(&e, scopes, 1);
    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[256];
    bearer(&e, auth, sizeof auth);

    /* Two records of different kinds. Written over the fixture daemon's own
     * socket rather than through the CLI: the daemon owns the index by the time
     * `env_open` returns, and the credential under test cannot propose — that is
     * the boundary, not a limitation of the test. */
    atlas_err err;
    atlas_err_init(&err);
    static const char *const SEED[] = {
        "{\"repo\":\"proj\",\"kind\":\"OBLIGATION\",\"actor\":\"MODEL_PROPOSAL\","
        "\"title\":\"licensing must be settled\","
        "\"decision_body\":\"before any public release\"}",
        "{\"repo\":\"proj\",\"kind\":\"INVARIANT\",\"actor\":\"MODEL_PROPOSAL\","
        "\"title\":\"the timestamp is inert\","
        "\"decision_body\":\"and is encoded as zero\"}",
    };
    for (size_t i = 0; i < sizeof SEED / sizeof SEED[0]; i++) {
        atlas_buf seeded = ATLAS_BUF_INIT;
        atlas_err serr;
        atlas_err_init(&serr);
        T_OK(atlas_ipc_call(atlas_buf_cstr(&e.d.socket), "decision.propose", SEED[i], &seeded,
                            &serr),
             &serr);
        T_REQUIRE_MSG(strstr(atlas_buf_cstr(&seeded), "\"ok\":true") != NULL,
                      "seeding a record failed: %s", atlas_buf_cstr(&seeded));
        atlas_buf_free(&seeded);
    }

    /* Unfiltered: both, each carrying its own kind. */
    request(&e, "GET", "/api/v1/decisions?repo=proj", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"kind\":\"OBLIGATION\"") != NULL,
                "the API omitted the kind: %s", body_of(&resp));
    T_CHECK(strstr(body_of(&resp), "\"kind\":\"INVARIANT\"") != NULL);
    /* And the per-kind totals, which are the second axis of the same answer. */
    T_CHECK_MSG(strstr(body_of(&resp), "total_by_kind") != NULL,
                "the API omitted the kind totals: %s", body_of(&resp));

    /* Filtered by kind: one, and not the other. */
    request(&e, "GET", "/api/v1/decisions?repo=proj&kind=OBLIGATION", auth, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"kind\":\"OBLIGATION\"") != NULL,
                "the kind filter dropped the matching record: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"kind\":\"INVARIANT\"") == NULL,
                "the kind filter kept a record of another kind: %s", body_of(&resp));

    /* Both filters at once — the combination the dimension exists for. */
    request(&e, "GET", "/api/v1/decisions?repo=proj&kind=INVARIANT&status=PROPOSED", auth, NULL,
            &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK(strstr(body_of(&resp), "\"kind\":\"INVARIANT\"") != NULL);
    T_CHECK(strstr(body_of(&resp), "\"kind\":\"OBLIGATION\"") == NULL);

    /* A kind outside the vocabulary is refused, not silently empty. */
    request(&e, "GET", "/api/v1/decisions?repo=proj&kind=INVARIENT", auth, NULL, &resp);
    T_CHECK_MSG(status_of(&resp) >= 400,
                "a misspelt kind returned %d rather than a refusal: %s", status_of(&resp),
                body_of(&resp));

    /* The listing offers the revise tool to nobody: its scope is not grantable. */
    request(&e, "POST", "/mcp", auth,
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\",\"params\":{}}", &resp);
    T_CHECK_MSG(strstr(body_of(&resp), "atlas_revise_decision") == NULL,
                "a read-only credential was offered the revise tool: %s", body_of(&resp));

    /* And there is no remote resolve, under any spelling. */
    static const char *const NAMES[] = {"atlas_decision_resolve", "atlas_resolve_decision",
                                        "atlas_decision_approve", "atlas_decision_set_kind"};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        char msg[512];
        (void)snprintf(msg, sizeof msg,
                       "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":"
                       "{\"name\":\"%s\",\"arguments\":{\"repo\":\"proj\"}}}",
                       NAMES[i]);
        request(&e, "POST", "/mcp", auth, msg, &resp);
        T_EQ_INT(status_of(&resp), 200);
        T_CHECK_MSG(strstr(body_of(&resp), "unknown tool") != NULL,
                    "\"%s\" was not answered as an unknown tool: %s", NAMES[i], body_of(&resp));
    }

    atlas_buf_free(&resp);
    env_close(&e);
}

/* A9.2.1 — the three verification routes, over a real listening socket.
 *
 * A9.2.1 shipped `/api/v1/verify/claims`, `/api/v1/verify/claim` and
 * `/api/v1/verify/policy` with no test between them, which meant the remote
 * half of the verification surface was argued for in prose and never executed.
 * This drives all three the way a client does — bytes onto a loopback socket,
 * a bearer credential, the gateway's own routing and scope check, the daemon
 * over the Unix socket, and the answer back.
 *
 * The listener binds `127.0.0.1` and nothing else. The gateway policy is built
 * with `atlas_gwpolicy_parse_buffer`, so no file is read and the root-owned
 * `/etc/atlas/gateway.conf` is neither consulted nor touched.
 *
 * Two properties, and the second is the one that matters:
 *
 *   - a credential holding `decisions:read` reads a claim, its evidence and the
 *     policy, and the reply carries the four A9.2.1 source-binding fields;
 *   - **there is no intake route.** Every verification path answers 405 to a
 *     POST, and every plausible spelling of an intake endpoint is 404. A leaked
 *     bearer token cannot state a claim, cite evidence, attest or ask for an
 *     evaluation, because `verify.evaluate` can move a lifecycle state. That
 *     absence is the argument A9's rule demands, so it is asserted rather than
 *     described.
 */
static void test_the_verification_routes_read_and_offer_no_intake(void) {
    env e;
    const char *scopes[] = {"repo:read", "decisions:read"};
    env_open(&e, scopes, 2);

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;

    /* One claim, stated through the CLI against the running daemon. The gateway
     * has no way to do this, which is the point of the test below. */
    {
        const char *args[] = {"verify", "claim",   "--repo", "proj",
                              "--text", "the gateway offers no intake route", "--json"};
        T_EQ_INT(run_cli_ex(&e, args, 7, true, &out, &err), 0);
    }
    char claim_uid[128];
    claim_uid[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "atlas-claim-");
        T_REQUIRE_MSG(s != NULL, "no claim uid in: %s", atlas_buf_cstr(&out));
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '"' && n + 1 < sizeof claim_uid) {
            n++;
        }
        memcpy(claim_uid, s, n);
        claim_uid[n] = '\0';
    }
    atlas_buf_free(&out);

    /* A real listener, on loopback. */
    const int port = 38791;
    atlas_gwpolicy p;
    char text[256];
    (void)snprintf(text, sizeof text,
                   "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n"
                   "listen_addr = 127.0.0.1\nlisten_port = %d\n",
                   port);
    atlas_gwpolicy_parse_buffer(text, strlen(text), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e.d.socket);
    o.timeout_ms = 15000;

    listener l;
    memset(&l, 0, sizeof l);
    atomic_store(&l.stop, false);
    T_OK(atlas_gateway_open(&p, &o, &l.g, &err), &err);
    T_REQUIRE(pthread_create(&l.thread, NULL, listener_main, &l) == 0);

    atlas_buf resp = ATLAS_BUF_INIT;
    bool up = false;
    for (int i = 0; i < 200 && !up; i++) {
        up = wire_request(port, "GET /healthz HTTP/1.1\r\nHost: t\r\n\r\n", &resp);
        if (!up) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            (void)nanosleep(&ts, NULL);
        }
    }
    T_CHECK_MSG(up, "the gateway never accepted a connection on port %d", port);

    if (up) {
        char req[1024];

        /* The claim list. */
        (void)snprintf(req, sizeof req,
                       "GET /api/v1/verify/claims?repo=proj HTTP/1.1\r\nHost: t\r\n"
                       "Authorization: Bearer %s\r\n\r\n",
                       e.token);
        T_CHECK(wire_request(port, req, &resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 200") != NULL,
                    "verify/claims was refused: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), claim_uid) != NULL,
                    "the claim is missing from the list: %s", atlas_buf_cstr(&resp));

        /* One claim, with the fields migration 16 added. A reply that omitted
         * them would leave a remote reader unable to tell whether the result
         * describes the tree the repository is actually on. */
        (void)snprintf(req, sizeof req,
                       "GET /api/v1/verify/claim?claim=%s HTTP/1.1\r\nHost: t\r\n"
                       "Authorization: Bearer %s\r\n\r\n",
                       claim_uid, e.token);
        T_CHECK(wire_request(port, req, &resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 200") != NULL,
                    "verify/claim was refused: %s", atlas_buf_cstr(&resp));
        static const char *const FIELDS[] = {"claim_commit", "evaluated_commit", "sem_generation",
                                             "source_drift", "state",            "basis",
                                             "confidence_score", "calibration", "kind", "status"};
        for (size_t i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), FIELDS[i]) != NULL,
                        "the reply carries no \"%s\": %s", FIELDS[i], atlas_buf_cstr(&resp));
        }
        /* A9.2's rule, at the transport: a score is never a probability, and an
         * uncalibrated one carries no probability field at all. */
        T_CHECK(strstr(atlas_buf_cstr(&resp), "calibrated_probability") == NULL);

        /* The policy, which opens no index. */
        (void)snprintf(req, sizeof req,
                       "GET /api/v1/verify/policy HTTP/1.1\r\nHost: t\r\n"
                       "Authorization: Bearer %s\r\n\r\n",
                       e.token);
        T_CHECK(wire_request(port, req, &resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 200") != NULL,
                    "verify/policy was refused: %s", atlas_buf_cstr(&resp));

        /* Intake is absent. A POST to a route that exists reads only; a route
         * that would perform intake does not exist at all. */
        static const char *const READ_ONLY[] = {"/api/v1/verify/claims", "/api/v1/verify/claim",
                                                "/api/v1/verify/policy"};
        for (size_t i = 0; i < sizeof READ_ONLY / sizeof READ_ONLY[0]; i++) {
            (void)snprintf(req, sizeof req,
                           "POST %s HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\n"
                           "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
                           READ_ONLY[i], e.token);
            T_CHECK(wire_request(port, req, &resp));
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 405") != NULL,
                        "POST %s did not answer 405: %s", READ_ONLY[i], atlas_buf_cstr(&resp));
        }
        static const char *const ABSENT[] = {
            "/api/v1/verify/claim_create", "/api/v1/verify/create", "/api/v1/verify/evidence",
            "/api/v1/verify/produce",      "/api/v1/verify/attest", "/api/v1/verify/attestation",
            "/api/v1/verify/depend",       "/api/v1/verify/evaluate", "/api/v1/verify/run",
            "/api/v1/verify/resolve",      "/api/v1/verify/approve",
        };
        for (size_t i = 0; i < sizeof ABSENT / sizeof ABSENT[0]; i++) {
            for (int m = 0; m < 2; m++) {
                (void)snprintf(req, sizeof req,
                               "%s %s HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer %s\r\n"
                               "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
                               m == 0 ? "GET" : "POST", ABSENT[i], e.token);
                T_CHECK(wire_request(port, req, &resp));
                T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "HTTP/1.1 404") != NULL,
                            "%s %s is not absent: %s", m == 0 ? "GET" : "POST", ABSENT[i],
                            atlas_buf_cstr(&resp));
            }
        }
    }

    atomic_store(&l.stop, true);
    (void)pthread_join(l.thread, NULL);
    atlas_gateway_close(l.g);
    atlas_buf_free(&resp);
    env_close(&e);
}

/* The same routes, to a credential that was never granted `decisions:read`.
 *
 * Hiding is not authorisation: naming the route directly meets the same check.
 */
static void test_a_credential_without_decisions_read_cannot_read_a_claim(void) {
    env e;
    const char *scopes[] = {"repo:read"};
    env_open(&e, scopes, 1);

    atlas_buf resp = ATLAS_BUF_INIT;
    char auth[ATLAS_APIKEY_TOKEN_MAX + 16];
    (void)snprintf(auth, sizeof auth, "Bearer %s", e.token);

    static const char *const PATHS[] = {"/api/v1/verify/claims?repo=proj",
                                        "/api/v1/verify/claim?claim=atlas-claim-0",
                                        "/api/v1/verify/policy"};
    for (size_t i = 0; i < sizeof PATHS / sizeof PATHS[0]; i++) {
        request(&e, "GET", PATHS[i], auth, NULL, &resp);
        T_CHECK_MSG(status_of(&resp) == 403, "%s answered %d rather than 403: %s", PATHS[i],
                    status_of(&resp), body_of(&resp));
    }

    atlas_buf_free(&resp);
    env_close(&e);
}

/* Mission Control's verification view, over the browser's own authentication.
 *
 * The page is not a second implementation: it renders exactly what
 * `/api/v1/verify/claim` returns. What this establishes is the other half —
 * that the browser principal reaches those routes at all, and that the page
 * shipped in the binary carries the bindings for the fields the route sends.
 * A page whose bindings had drifted from the JSON would render blanks while
 * every API test still passed.
 */
static void test_mission_control_reaches_the_verification_routes(void) {
    env e;
    const char *scopes[] = {"repo:read", "decisions:read"};
    env_open(&e, scopes, 2);

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *args[] = {"verify", "claim",   "--repo", "proj",
                              "--text", "mission control reads this", "--json"};
        T_EQ_INT(run_cli_ex(&e, args, 7, true, &out, &err), 0);
    }
    char claim_uid[128];
    claim_uid[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "atlas-claim-");
        T_REQUIRE_MSG(s != NULL, "no claim uid in: %s", atlas_buf_cstr(&out));
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '"' && n + 1 < sizeof claim_uid) {
            n++;
        }
        memcpy(claim_uid, s, n);
        claim_uid[n] = '\0';
    }
    atlas_buf_free(&out);

    atlas_gateway *g = NULL;
    gui_env(&e, &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    /* The page, and the bindings the verification view depends on. */
    gui_request(g, "GET", "/", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    static const char *const BOUND[] = {"verify/claim",  "verify/claims", "verify/policy",
                                        "source_drift",  "claim_commit",  "evaluated_commit",
                                        "sem_generation", "calibration",  "confidence_score",
                                        "independent_groups"};
    for (size_t i = 0; i < sizeof BOUND / sizeof BOUND[0]; i++) {
        T_CHECK_MSG(strstr(body_of(&resp), BOUND[i]) != NULL,
                    "the page carries no binding for \"%s\"", BOUND[i]);
    }
    /* The page says in words that a score is not a percentage. */
    T_CHECK_MSG(strstr(body_of(&resp), "% probability") != NULL,
                "the page does not say the score is not a probability");

    /* A browser session, then the three routes through it. */
    char body[512];
    (void)snprintf(body, sizeof body, "{\"key\":\"%s\"}", e.token);
    gui_request(g, "POST", "/auth/login", NULL, body, &resp);
    T_EQ_INT(status_of(&resp), 200);
    char cookie[128];
    cookie_of(&resp, cookie, sizeof cookie);
    T_REQUIRE_MSG(cookie[0] != '\0', "login set no session cookie");

    char path[256];
    (void)snprintf(path, sizeof path, "/api/v1/verify/claims?repo=proj");
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), claim_uid) != NULL, "the claim is missing: %s",
                body_of(&resp));

    (void)snprintf(path, sizeof path, "/api/v1/verify/claim?claim=%s", claim_uid);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    static const char *const FIELDS[] = {"claim_commit", "evaluated_commit", "sem_generation",
                                         "source_drift", "state",           "basis",
                                         "confidence_score", "calibration", "kind", "status"};
    for (size_t i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        T_CHECK_MSG(strstr(body_of(&resp), FIELDS[i]) != NULL, "the reply carries no \"%s\": %s",
                    FIELDS[i], body_of(&resp));
    }

    gui_request(g, "GET", "/api/v1/verify/policy", cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    /* And the browser has no intake either. */
    gui_request(g, "POST", "/api/v1/verify/evaluate", cookie, "{}", &resp);
    T_CHECK_MSG(status_of(&resp) == 404, "the browser reached an intake route: %d",
                status_of(&resp));

    atlas_gateway_close(g);
    atlas_buf_free(&resp);
    env_close(&e);
}

/* --- A15 T2: three routes forward one more parameter each -------------------- */

/* Pulls a decision uid out of `decision propose`/`revise` human output, the way
 * `tests/test_decision_operator.c` does: a fixed-length copy of the prefix plus
 * its hex body, validated rather than merely found. */
static void capture_decision_uid(const atlas_buf *out, char *buf, size_t n) {
    const char *p = strstr(atlas_buf_cstr(out), ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE_MSG(p != NULL, "no decision uid in: %s", atlas_buf_cstr(out));
    size_t len = strlen(ATLAS_DECISION_UID_PREFIX) + ATLAS_DECISION_UID_HEX;
    T_REQUIRE(len + 1u <= n);
    memcpy(buf, p, len);
    buf[len] = '\0';
    T_REQUIRE(atlas_decision_uid_is_valid(buf));
}

/* Counts non-overlapping occurrences of an exact substring. Used to prove
 * "exactly one row", which a single `strstr` cannot. */
static size_t count_occurrences(const char *haystack, const char *needle) {
    size_t n = 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

/* Approves `uid` in repository "proj" through the write point, without the
 * CLI's interactive channel and without the daemon's operator-uid IPC group.
 *
 * Neither of those two is blocked by "no real terminal" — a terminal is not
 * authority (`CLAUDE.md`, A7): a same-uid process driving a pseudo-terminal
 * reaches `LOCAL_OPERATOR_CONFIRMED` exactly as a person does, and
 * `tests/test_decision_operator.c:180-200` proves it by doing exactly that
 * with `posix_openpt`/`TIOCSCTTY`. What actually blocks both channels here is
 * Atlas' default *locked* authority profile: the CLI's interactive path is
 * refused under it (`tests/test_decision_operator.c:127-131`), and the
 * daemon's `decision.approve` sits behind the same fact from the other side —
 * `atlas_server_peer_is_operator` (`src/ipc/server_decision.c:2738`) answers
 * true only when `atlas_authority_probe_peer` finds a root-owned policy at a
 * root-owned path naming this uid as operator, and installing one is exactly
 * what an unprivileged test cannot do. `atlas_decision_apply` is the write
 * point both channels reach, and it performs no authority check of its own —
 * that check lives at the CLI entry point, in
 * `atlas_service_decision_confirm`. This is
 * `tests/test_decision_operator.c:145`'s `approve_through_the_write_point`,
 * verbatim, against the fixture's data directory.
 *
 * **Callers must run this before any daemon is started against `fx`'s data
 * directory.** It opens a plain read-write `atlas_db` handle that takes no
 * `flock` of its own, and invariant 11 (`CLAUDE.md`) — "exactly one process
 * writes the index at a time, enforced by an advisory lock ... not by
 * convention" — means a second writer beside a live daemon's writer thread is
 * exactly the situation that lock exists to prevent. Called only before
 * `fx_daemon_start` below, so at every moment there is exactly one writer. */
static void approve_decision_at_write_point(fixture *fx, const char *uid) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(fx_data_dir(fx), &db_path, &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);

    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, uid, &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);

    atlas_decision_op ap;
    atlas_decision_op_init(&ap, ATLAS_DECISION_OP_APPROVE);
    T_OK(atlas_buf_set_str(&ap.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ap.uid, uid, &err), &err);
    T_OK(atlas_buf_set(&ap.token, cr.token.data, cr.token.len, &err), &err);
    T_OK(atlas_buf_set_str(&ap.confirmation, cr.confirm, &err), &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    T_OK(atlas_decision_apply(db, &ap, &ar, &err), &err);
    T_CHECK(ar.state == ATLAS_DECISION_APPROVED);

    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
}

/* T2: three existing route rows each forward one more parameter name the
 * daemon already reads — `/api/v1/decision`'s `revision`, `/api/v1/gate`'s
 * `decision`, and `/api/v1/code/impact`'s `symbol`. No row is added, no
 * scope, no method, no migration. See
 * `docs/plans/2026-09-03-review-surface.md`, "The three route rows, after
 * T2", for the exact final shape.
 *
 * This does not use the shared `env_open` above: `env_open` starts the
 * fixture daemon before any test body runs, and approving a decision needs
 * `approve_decision_at_write_point`, which must run before that daemon
 * exists (see its own comment). So this test reproduces `env_open`'s setup
 * by hand — repository, scan, one API key — with every decision proposed and
 * approved in between, and only then starts the daemon and opens the
 * (non-GUI) gateway `env_close` expects to find on `e.g`.
 *
 * Every route below is then read through a browser session, as
 * `test_mission_control_reaches_the_verification_routes` above does:
 * `gui_env` opens a second, web-GUI-enabled gateway over the same fixture
 * daemon, `/auth/login` exchanges the API key for a session cookie, and every
 * route is read through that cookie rather than a bearer token — because
 * Mission Control, the reader this parameter reaches for, is a browser
 * client. */
static void test_the_review_parameters_reach_the_daemon(void) {
    env e;
    memset(&e, 0, sizeof e);
    atlas_err err;
    atlas_err_init(&err);

    T_REQUIRE(fx_open(&e.fx, &err) == ATLAS_OK);
    T_OK(fx_init_repo(&e.fx, fx_repo(&e.fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", "proj"};
        T_EQ_INT(run_cli(&e, add, 5, &out, &err), 0);
    }
    {
        const char *scan[] = {"scan", "proj"};
        T_EQ_INT(run_cli(&e, scan, 2, &out, &err), 0);
    }
    {
        /* (c)'s `code/impact?symbol=main` reads the structural (A3) index,
         * which `scan` does not build itself -- it only schedules the
         * daemon's background pass. Run `code sync` explicitly rather than
         * trust that pass to have finished before the later query lands. */
        const char *sync[] = {"code", "sync", "proj"};
        T_EQ_INT(run_cli(&e, sync, 3, &out, &err), 0);
    }
    {
        const char *scopes[] = {"repo:read", "decisions:read", "impact:read"};
        const char *args[16];
        size_t k = 0;
        args[k++] = "api-key";
        args[k++] = "create";
        args[k++] = "--label";
        args[k++] = "chatgpt-test";
        for (size_t i = 0; i < sizeof scopes / sizeof scopes[0]; i++) {
            args[k++] = "--scope";
            args[k++] = scopes[i];
        }
        T_EQ_INT(run_cli(&e, args, k, &out, &err), 0);
        capture_key(&e, &out);
    }

    /* Decision A: proposed, then revised, so revision 2 exists. Never
     * approved — it stays PROPOSED for the whole test, which is what makes it
     * usable below as one half of (b)'s probe. */
    char uid_a[64];
    {
        const char *propose[] = {"decision",   "propose", "proj",
                                 "--title",    "Use WAL journalling",
                                 "--decision", "Enable WAL on the index database.",
                                 "--path",     "a.c"};
        T_EQ_INT(run_cli(&e, propose, sizeof propose / sizeof propose[0], &out, &err), 0);
    }
    capture_decision_uid(&out, uid_a, sizeof uid_a);
    {
        const char *revise[] = {"decision",   "revise",  "proj",
                                 uid_a,        "--title", "Use WAL journalling, revised",
                                 "--decision", "Enable WAL on the index database, now revised."};
        T_EQ_INT(run_cli(&e, revise, sizeof revise / sizeof revise[0], &out, &err), 0);
    }

    /* Decision B: proposed, then approved through the write point. */
    char uid_b[64];
    {
        const char *propose[] = {"decision",   "propose", "proj",
                                 "--title",    "Gate check target",
                                 "--decision", "Something for the gate to assess."};
        T_EQ_INT(run_cli(&e, propose, sizeof propose / sizeof propose[0], &out, &err), 0);
    }
    capture_decision_uid(&out, uid_b, sizeof uid_b);
    approve_decision_at_write_point(&e.fx, uid_b);

    /* Decision C: also proposed and approved before the daemon exists. Its
     * only purpose is to be a *second* approved decision, so that asking
     * about B with `decision=B` can be told apart from asking with no
     * `decision` at all — with only B approved, both answers would be
     * identical and the assertion below would pass whether or not the row
     * forwards the parameter. */
    char uid_c[64];
    {
        const char *propose[] = {
            "decision",   "propose", "proj",
            "--title",    "A second approved record",
            "--decision", "Exists only to make (b)'s narrowing assertion discriminating."};
        T_EQ_INT(run_cli(&e, propose, sizeof propose / sizeof propose[0], &out, &err), 0);
    }
    capture_decision_uid(&out, uid_c, sizeof uid_c);
    approve_decision_at_write_point(&e.fx, uid_c);
    atlas_buf_free(&out);

    /* Only now does a daemon exist against this data directory. */
    fx_daemon_init(&e.d);
    T_OK(fx_daemon_start(&e.fx, &e.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e.d, 15000, &err), &err);

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e.d.socket);
    o.timeout_ms = 15000;
    o.errout = NULL;
    atlas_gwpolicy p;
    static const char *const POLICY_TEXT =
        "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n";
    atlas_gwpolicy_parse_buffer(POLICY_TEXT, strlen(POLICY_TEXT), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_OK(atlas_gateway_open(&p, &o, &e.g, &err), &err);

    atlas_gateway *g = NULL;
    gui_env(&e, &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    char body[512];
    (void)snprintf(body, sizeof body, "{\"key\":\"%s\"}", e.token);
    gui_request(g, "POST", "/auth/login", NULL, body, &resp);
    T_EQ_INT(status_of(&resp), 200);
    char cookie[128];
    cookie_of(&resp, cookie, sizeof cookie);
    T_REQUIRE_MSG(cookie[0] != '\0', "login set no session cookie");

    /* (a) `revision` reaches `decision.get`. */
    char path[256];
    (void)snprintf(path, sizeof path, "/api/v1/decision?repo=proj&decision=%s&revision=1", uid_a);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"number\":1") != NULL,
                "revision=1 did not return revision 1: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"title\":\"Use WAL journalling\"") != NULL,
                "revision=1 did not carry revision 1's title: %s", body_of(&resp));

    (void)snprintf(path, sizeof path, "/api/v1/decision?repo=proj&decision=%s&revision=2", uid_a);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"number\":2") != NULL,
                "revision=2 did not return revision 2: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"title\":\"Use WAL journalling, revised\"") != NULL,
                "revision=2 did not carry revision 2's title: %s", body_of(&resp));

    (void)snprintf(path, sizeof path, "/api/v1/decision?repo=proj&decision=%s&revision=abc",
                   uid_a);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 400);

    /* (b) `decision` reaches `gate.check`, narrowing the assessment to one
     * record: exactly one row naming <uid> when the record is APPROVED, and
     * nothing naming any other decision — APPROVED or not.
     *
     * B alone would not discriminate this: with only one approved decision
     * in the repository, "every approved decision" (the pre-fix fallback,
     * since a dropped `decision` parameter is silently ignored) and "just
     * B" are the same answer. C exists so this assertion can actually fail
     * against the unfixed row — asking about B must never also name C. */
    (void)snprintf(path, sizeof path, "/api/v1/gate?repo=proj&decision=%s", uid_b);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    char decision_field[96];
    (void)snprintf(decision_field, sizeof decision_field, "\"decision\":\"%s\"", uid_b);
    T_CHECK_MSG(count_occurrences(body_of(&resp), decision_field) == 1u,
                "the approved decision did not appear exactly once: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), uid_a) == NULL,
                "asking about the approved decision also named the unrelated one: %s",
                body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), uid_c) == NULL,
                "asking about B also named the other approved decision C: %s", body_of(&resp));

    /* What `gate.check` returns for a decision that is not APPROVED — this
     * plan does not claim it, so it is established here rather than assumed.
     * Asking about A (still PROPOSED) must at least never answer with B's or
     * C's uid — the pre-fix defect is exactly that "decision" is dropped, so
     * this request silently falls back to "every approved decision", which
     * is {B, C}. */
    (void)snprintf(path, sizeof path, "/api/v1/gate?repo=proj&decision=%s", uid_a);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_CHECK_MSG(strstr(body_of(&resp), uid_b) == NULL && strstr(body_of(&resp), uid_c) == NULL,
                "asking about the non-approved decision answered about an approved one instead: %s",
                body_of(&resp));
    /* Established by running this: a record that is not APPROVED (PROPOSED
     * here, and by the same read the same is true of REJECTED, SUPERSEDED
     * and RESOLVED) is never a candidate for `gate.check` at all
     * (`src/core/service_gate.c:365` lists only documents with status
     * "APPROVED"), so naming one narrows the assessment to zero items, and
     * `atlas_gate_narrow_to_one` (`src/core/service_gate.c:456`) treats zero
     * items as a refusal rather than an empty list: HTTP 404, with the
     * message "no approved decision "<uid>" is attached to this repository".
     * A caller cannot distinguish "this decision does not exist" from "this
     * decision exists but was never approved" from this response alone. */
    T_EQ_INT(status_of(&resp), 404);
    T_CHECK_MSG(strstr(body_of(&resp), "no approved decision") != NULL,
                "a non-APPROVED record's gate check did not read as a refusal: %s",
                body_of(&resp));

    /* (c) `symbol` reaches `code.impact`. Today this is refused — a "path" or
     * a "symbol" is required, and the route drops `symbol` — which is exactly
     * what Mission Control's Impact view sends. Fixed here. */
    gui_request(g, "GET", "/api/v1/code/impact?repo=proj&symbol=main", cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    T_CHECK_MSG(strstr(body_of(&resp), "\"symbol\":\"main\"") != NULL,
                "code/impact did not name the symbol: %s", body_of(&resp));

    atlas_gateway_close(g);
    atlas_buf_free(&resp);
    env_close(&e);
}

/* --- A15 T7: Mission Control's Review view ----------------------------------- */

/* Mission Control's Review view: the bindings the page carries for the five
 * detail panels and the review sheet, and -- through a real session cookie --
 * the five routes the view names, each answering 200.
 *
 * **This executes none of the page's JavaScript.** Nothing in this suite has a
 * browser, and nothing here claims the queue buttons, the sheet's text
 * building or the drift label actually render correctly for a person looking
 * at a screen. What is established is narrower and is exactly what
 * `test_mission_control_reaches_the_verification_routes` above establishes for
 * the Verification view: the served bytes carry the identifiers and sentences
 * each part of the Review view depends on (a page whose bindings drifted from
 * the JSON would render blanks while every route test still passed), and the
 * browser principal that would drive the page reaches every route the view
 * names, with a 200.
 *
 * The `innerHTML` check greps for the bare word, with no leading dot: the
 * page's own comment above its `<script>` used to spell that word out while
 * explaining the `textContent` rule (`mission-control.html:195` before this
 * season), which would have tripped a bare-word check on the page's own
 * documentation rather than on a real use. This season rewords that one
 * comment instead, so the check can be the plain, strict one -- catching an
 * assignment, a read, or a future comment that reintroduces the word -- and
 * still pass against a page that has no code and no prose to distinguish it
 * from.
 *
 * Fix round 1 additions: three more markup-parsing sinks alongside
 * `innerHTML` (the task names all four); the `verify/claims` needle replaced
 * by a `count_occurrences` check requiring two call sites, since a bare
 * `strstr` for that string passed against the unmodified parent page and
 * proved nothing about this view.
 */
static void test_mission_control_carries_the_review_view(void) {
    env e;
    memset(&e, 0, sizeof e);
    atlas_err err;
    atlas_err_init(&err);

    T_REQUIRE(fx_open(&e.fx, &err) == ATLAS_OK);
    T_OK(fx_init_repo(&e.fx, fx_repo(&e.fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", "proj"};
        T_EQ_INT(run_cli(&e, add, 5, &out, &err), 0);
    }
    {
        const char *scan[] = {"scan", "proj"};
        T_EQ_INT(run_cli(&e, scan, 2, &out, &err), 0);
    }
    {
        /* The structural (A3) index is what `code/impact?symbol=` reads.
         * `scan` alone does not build it -- `test_gate.c`'s `reindex` and
         * every other fixture that later queries a symbol run `code sync`
         * explicitly, rather than relying on the daemon's own background
         * pass to have finished by the time a later request reaches it. */
        const char *sync[] = {"code", "sync", "proj"};
        T_EQ_INT(run_cli(&e, sync, 3, &out, &err), 0);
    }
    {
        const char *scopes[] = {"repo:read", "decisions:read", "impact:read"};
        const char *args[16];
        size_t k = 0;
        args[k++] = "api-key";
        args[k++] = "create";
        args[k++] = "--label";
        args[k++] = "review-view-test";
        for (size_t i = 0; i < sizeof scopes / sizeof scopes[0]; i++) {
            args[k++] = "--scope";
            args[k++] = scopes[i];
        }
        T_EQ_INT(run_cli(&e, args, k, &out, &err), 0);
        capture_key(&e, &out);
    }

    /* One decision, approved before the daemon exists (see
     * `approve_decision_at_write_point`'s own comment for why that order is
     * required), so `gate?decision=` has an approved record to answer 200
     * about. */
    char uid[64];
    {
        const char *propose[] = {"decision",   "propose", "proj",
                                 "--title",    "Review view probe",
                                 "--decision", "Something the Review view can read in full."};
        T_EQ_INT(run_cli(&e, propose, sizeof propose / sizeof propose[0], &out, &err), 0);
    }
    capture_decision_uid(&out, uid, sizeof uid);
    approve_decision_at_write_point(&e.fx, uid);
    atlas_buf_free(&out);

    fx_daemon_init(&e.d);
    T_OK(fx_daemon_start(&e.fx, &e.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e.d, 15000, &err), &err);

    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&e.d.socket);
    o.timeout_ms = 15000;
    atlas_gwpolicy p;
    static const char *const POLICY_TEXT = "enabled = yes\ngateway_uid = 1\nremote_mcp = yes\n";
    atlas_gwpolicy_parse_buffer(POLICY_TEXT, strlen(POLICY_TEXT), &p);
    T_REQUIRE(p.state == ATLAS_GWPOLICY_ENABLED);
    T_OK(atlas_gateway_open(&p, &o, &e.g, &err), &err);

    atlas_gateway *g = NULL;
    gui_env(&e, &g);
    atlas_buf resp = ATLAS_BUF_INIT;

    /* The page, and the bindings the Review view depends on. */
    gui_request(g, "GET", "/", NULL, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);
    static const char *const BOUND[] = {
        "v-review",
        "atlas-review-sheet/1",
        "atlas.review.sheet.v1",
        "decision/history",
        "ledger_agrees",
        "operator_channel",
        /* Not "verify/claims" here: that string was already on the parent
         * page, in `viewVerification`, before this season -- a bare
         * `strstr` for it would pass against the unmodified page and prove
         * nothing about the Review view. The discriminating form is below:
         * it requires a *second* call site, which only the Review view's
         * own claims panel can supply. */
        "reviewIntentsAllowed",
        "IMPLEMENTATION conflict",
        "review apply",
        "names the channel, not a person",
    };
    for (size_t i = 0; i < sizeof BOUND / sizeof BOUND[0]; i++) {
        T_CHECK_MSG(strstr(body_of(&resp), BOUND[i]) != NULL,
                    "the page carries no binding for \"%s\"", BOUND[i]);
    }
    T_CHECK_MSG(count_occurrences(body_of(&resp), "verify/claims") >= 2u,
                "the Review view's own verify/claims call site is missing: %s",
                body_of(&resp));
    /* The four DOM APIs that would parse a string as markup rather than
     * insert it as text -- the whole of what "everything is textContent;
     * nothing is innerHTML" (CLAUDE.md) requires absent. `innerHTML` alone
     * would miss a page that used one of the other three instead. */
    static const char *const MARKUP_SINKS[] = {
        "innerHTML",
        "outerHTML",
        "insertAdjacentHTML",
        "document.write",
    };
    for (size_t i = 0; i < sizeof MARKUP_SINKS / sizeof MARKUP_SINKS[0]; i++) {
        T_CHECK_MSG(strstr(body_of(&resp), MARKUP_SINKS[i]) == NULL,
                    "the page uses the markup-parsing sink \"%s\"", MARKUP_SINKS[i]);
    }
    T_CHECK_MSG(strstr(body_of(&resp), "implementation drift") == NULL,
                "the page claims a drift detector broader than A12.1's reconciler");

    /* A browser session, then the five routes the Review view names. */
    char body[512];
    (void)snprintf(body, sizeof body, "{\"key\":\"%s\"}", e.token);
    gui_request(g, "POST", "/auth/login", NULL, body, &resp);
    T_EQ_INT(status_of(&resp), 200);
    char cookie[128];
    cookie_of(&resp, cookie, sizeof cookie);
    T_REQUIRE_MSG(cookie[0] != '\0', "login set no session cookie");

    char path[256];
    (void)snprintf(path, sizeof path, "/api/v1/decision/history?repo=proj&decision=%s", uid);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    (void)snprintf(path, sizeof path, "/api/v1/decision?repo=proj&decision=%s&revision=1", uid);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    (void)snprintf(path, sizeof path, "/api/v1/verify/claims?repo=proj&decision=%s", uid);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    (void)snprintf(path, sizeof path, "/api/v1/gate?repo=proj&decision=%s", uid);
    gui_request(g, "GET", path, cookie, NULL, &resp);
    T_EQ_INT(status_of(&resp), 200);

    gui_request(g, "GET", "/api/v1/code/impact?repo=proj&symbol=main", cookie, NULL, &resp);
    T_CHECK_MSG(status_of(&resp) == 200, "code/impact answered %d: %s", status_of(&resp),
                body_of(&resp));

    atlas_gateway_close(g);
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
    {"--no-scopes mints a disposal credential through the daemon",
     test_no_scopes_mints_a_disposal_credential_through_the_daemon},
    {"the gateway holds no credential-administration verb",
     test_the_gateway_holds_no_credential_administration_verb},
    {"the transport refuses what it should", test_the_transport_refuses_what_it_should},
    {"the audit trail records what happened", test_the_audit_trail_records_what_happened},
    {"the web API reads and refuses", test_the_web_api_reads_and_refuses},
    {"the API exposes the knowledge dimension", test_the_api_exposes_the_knowledge_dimension},
    {"the API forwards only what a route declares",
     test_the_api_forwards_only_what_a_route_declares},
    {"the browser exchanges a key for a session",
     test_the_browser_exchanges_a_key_for_a_session},
    {"no anonymous scopes named means no change",
     test_no_anonymous_scopes_named_means_no_change},
    {"the anonymous floor grants exactly the named scopes",
     test_the_anonymous_floor_grants_exactly_the_named_scopes},
    {"the audit trail names an anonymous request plainly",
     test_the_audit_trail_names_an_anonymous_request_plainly},
    {"the host check blocks DNS rebinding", test_the_host_check_blocks_dns_rebinding},
    {"the browser surface is absent when disabled",
     test_the_browser_surface_is_absent_when_disabled},
    {"the listener binds and serves", test_the_listener_binds_and_serves},
    {"the verification routes read and offer no intake",
     test_the_verification_routes_read_and_offer_no_intake},
    {"a credential without decisions:read cannot read a claim",
     test_a_credential_without_decisions_read_cannot_read_a_claim},
    {"mission control reaches the verification routes",
     test_mission_control_reaches_the_verification_routes},
    {"the review parameters reach the daemon", test_the_review_parameters_reach_the_daemon},
    {"mission control carries the review view", test_mission_control_carries_the_review_view},
    {"concurrent connections are safe", test_concurrent_connections_are_safe},
};

ATLAS_TEST_MAIN("gw_remote", TESTS)

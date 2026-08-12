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
    {"the web API reads and refuses", test_the_web_api_reads_and_refuses},
    {"the API forwards only what a route declares",
     test_the_api_forwards_only_what_a_route_declares},
    {"the browser exchanges a key for a session",
     test_the_browser_exchanges_a_key_for_a_session},
    {"the browser surface is absent when disabled",
     test_the_browser_surface_is_absent_when_disabled},
    {"the listener binds and serves", test_the_listener_binds_and_serves},
    {"concurrent connections are safe", test_concurrent_connections_are_safe},
};

ATLAS_TEST_MAIN("gw_remote", TESTS)

/* Atlas — A14 T6: the four remote-submission HTTP routes, against a real daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This suite proves the gateway side of remote submission:
 *   - the four POST routes (/api/v1/job/{submit,get,list,cancel}) route, body-bound
 *     and authenticate correctly against atlas-gw-daemon started with both a
 *     gateway policy (naming two submit keys) and an orchestration policy;
 *   - the bearer-only credential check: a session cookie and the anonymous floor
 *     do not reach the daemon;
 *   - /auth/me reports the three new submission fields correctly;
 *   - the audit trail records the key selector, not the plaintext token.
 *
 * T7 adds the /mcp tool half (tools/call on the four names) in
 * tests/test_gw_submit.c's same file; both halves share this daemon fixture.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "atlas/apikey.h"
#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/gateway.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"
#include "atlas/limits.h"
#include "atlas/orch.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "ipc/server_internal.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

#ifndef ATLAS_GW_DAEMON_BIN
#define ATLAS_GW_DAEMON_BIN "atlas-gw-daemon"
#endif

/* --- types ----------------------------------------------------------------- */

typedef struct env {
    fixture fx;
    /* Two submit keys (listed in the policy), one unlisted key, one browser key
     * used only for the session-cookie refusal case. */
    char submit_token[ATLAS_APIKEY_TOKEN_MAX];
    char submit_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* "key_<16hex>\0" */
    char second_token[ATLAS_APIKEY_TOKEN_MAX];
    char second_id[ATLAS_APIKEY_SELECTOR_HEX + 6u];
    char other_token[ATLAS_APIKEY_TOKEN_MAX];
    char other_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* not listed in the policy */
    char browser_token[ATLAS_APIKEY_TOKEN_MAX];
    char browser_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* for session-cookie login */
} env;

/* --- helpers: fixture run -------------------------------------------------- */

static void run_atlas(env *e, const char *const *extra, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[32];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    T_REQUIRE(n + k <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf errout = ATLAS_BUF_INIT;
    T_OK(fx_atlas(argv, k, out, &errout, code, &err), &err);
    atlas_buf_free(&errout);
}

static void mint_key(env *e, const char *label, char *token_out, size_t token_size,
                     char *id_out, size_t id_size) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *create[] = {"api-key", "create", "--label", label, "--no-scopes"};
    run_atlas(e, create, 5u, &out, &code);
    T_EQ_INT(code, 0);
    token_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "ATLAS_API_KEY=");
        T_REQUIRE_MSG(s != NULL, "no ATLAS_API_KEY= in output: %s", atlas_buf_cstr(&out));
        s += strlen("ATLAS_API_KEY=");
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < token_size) {
            n++;
        }
        memcpy(token_out, s, n);
        token_out[n] = '\0';
    }
    id_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "id:     " ATLAS_APIKEY_ID_PREFIX);
        T_REQUIRE_MSG(s != NULL, "no key id in output: %s", atlas_buf_cstr(&out));
        s += strlen("id:     ");
        size_t n = 0;
        while (s[n] != '\0' && s[n] != '\n' && n + 1 < id_size) {
            n++;
        }
        memcpy(id_out, s, n);
        id_out[n] = '\0';
    }
    atlas_buf_free(&out);
}

static void env_open(env *e) {
    memset(e, 0, sizeof(*e));
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "init", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
    run_atlas(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);
    const char *scan_args[] = {"scan", "proj"};
    run_atlas(e, scan_args, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);

    mint_key(e, "submit-key", e->submit_token, sizeof e->submit_token, e->submit_id,
             sizeof e->submit_id);
    mint_key(e, "second-key", e->second_token, sizeof e->second_token, e->second_id,
             sizeof e->second_id);
    mint_key(e, "other-key", e->other_token, sizeof e->other_token, e->other_id,
             sizeof e->other_id);
    mint_key(e, "browser-key", e->browser_token, sizeof e->browser_token, e->browser_id,
             sizeof e->browser_id);
}

static void env_close(env *e) {
    fx_close(&e->fx);
}

/* --- gwd_start: accepts both a gateway policy file and an orchestration
 *     policy file (orch_policy_path may be NULL for no-orch policy tests). --- */

static atlas_status gwd_start(env *e, const char *gw_policy_path,
                              const char *orch_policy_path, fx_daemon *d, atlas_err *err) {
    atlas_status st = atlas_buf_set_str(&d->data_dir, fx_data_dir(&e->fx), err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_buf_set(&d->runtime_dir, e->fx.root.data, e->fx.root.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&d->runtime_dir, "/run", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&d->log_path, e->fx.root.data, e->fx.root.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&d->log_path, "/gwdaemon.log", err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (mkdir(atlas_buf_cstr(&d->runtime_dir), S_IRWXU) != 0 && errno != EEXIST) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot create %s",
                                   atlas_buf_cstr(&d->runtime_dir));
    }
    st = atlas_buf_set(&d->socket, d->runtime_dir.data, d->runtime_dir.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(&d->socket, "/atlas/atlas.sock", err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_buf xdg = ATLAS_BUF_INIT;
    atlas_buf path_env = ATLAS_BUF_INIT;
    const char *path = getenv("PATH");
    st = atlas_buf_appendf(&xdg, err, "XDG_RUNTIME_DIR=%s", atlas_buf_cstr(&d->runtime_dir));
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&path_env, err, "PATH=%s",
                               (path != NULL && path[0] != '\0') ? path : "/usr/bin:/bin");
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&xdg);
        atlas_buf_free(&path_env);
        return st;
    }

    const char *argv[6];
    size_t ac = 0;
    argv[ac++] = ATLAS_GW_DAEMON_BIN;
    argv[ac++] = atlas_buf_cstr(&d->data_dir);
    argv[ac++] = gw_policy_path;
    if (orch_policy_path != NULL) {
        argv[ac++] = orch_policy_path;
    }
    argv[ac] = NULL;
    const char *envp[] = {atlas_buf_cstr(&path_env), "LC_ALL=C", "TZ=UTC", atlas_buf_cstr(&xdg),
                          NULL};

    pid_t pid = fork();
    if (pid < 0) {
        st = atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot fork a daemon");
        atlas_buf_free(&xdg);
        atlas_buf_free(&path_env);
        return st;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        int logfd = open(atlas_buf_cstr(&d->log_path), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
        }
        if (logfd >= 0) {
            (void)dup2(logfd, STDOUT_FILENO);
            (void)dup2(logfd, STDERR_FILENO);
        }
        (void)setpgid(0, 0);
        union {
            const char *const *in;
            char *const *out;
        } a = {argv}, ev = {envp};
        (void)execve(ATLAS_GW_DAEMON_BIN, a.out, ev.out);
        _exit(127);
    }
    d->pid = pid;
    atlas_buf_free(&xdg);
    atlas_buf_free(&path_env);
    return ATLAS_OK;
}

/* Two alternating static buffers so a caller can write two policy files before
 * passing both paths to gwd_start — a single buffer would overwrite the first
 * the moment the second is written. */
static const char *write_policy_file(env *e, const char *name, const char *text) {
    atlas_buf path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(&path, e->fx.root.data, e->fx.root.len, &err), &err);
    T_OK(atlas_buf_append_str(&path, "/", &err), &err);
    T_OK(atlas_buf_append_str(&path, name, &err), &err);
    int fd = open(atlas_buf_cstr(&path), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    T_REQUIRE_MSG(fd >= 0, "cannot create %s: %s", atlas_buf_cstr(&path), strerror(errno));
    size_t len = strlen(text);
    ssize_t wr = write(fd, text, len);
    (void)close(fd);
    T_REQUIRE_MSG((size_t)wr == len, "short write to %s", atlas_buf_cstr(&path));
    static char path_storage[2][4096];
    static unsigned path_slot;
    unsigned slot = (path_slot++) & 1u;
    (void)snprintf(path_storage[slot], sizeof(path_storage[slot]), "%s",
                   atlas_buf_cstr(&path));
    atlas_buf_free(&path);
    return path_storage[slot];
}

/* --- IPC helper for audit queries ------------------------------------------ */

static void call(const fx_daemon *d, const char *method, const char *params, atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(resp);
    (void)atlas_ipc_call(atlas_buf_cstr(&d->socket), method, params, resp, &err);
}

/* --- HTTP helpers (mirror of test_gw_dispose.c's own shapes) --------------- */

static void open_http_gateway(const char *policy_text, const fx_daemon *d,
                              atlas_gateway **g_out) {
    atlas_gwpolicy p;
    atlas_gwpolicy_parse_buffer(policy_text, strlen(policy_text), &p);
    T_REQUIRE_MSG(p.state == ATLAS_GWPOLICY_ENABLED,
                 "the policy text given to the HTTP gateway does not parse as ENABLED");
    atlas_gateway_opts o;
    memset(&o, 0, sizeof o);
    o.socket_path = atlas_buf_cstr(&d->socket);
    o.timeout_ms = 15000;
    o.errout = NULL;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_gateway_open(&p, &o, g_out, &err), &err);
}

static void http_request2(atlas_gateway *g, const char *method, const char *path,
                          const char *auth, const char *cookie, const char *origin,
                          const char *content_type, const char *body, size_t body_len_or_neg1,
                          atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf req = ATLAS_BUF_INIT;
    size_t blen = body != NULL ? (body_len_or_neg1 != (size_t)-1 ? body_len_or_neg1 : strlen(body))
                               : 0;
    T_OK(atlas_buf_appendf(&req, &err, "%s %s HTTP/1.1\r\nHost: 127.0.0.1:8787\r\n", method, path),
        &err);
    if (auth != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Authorization: %s\r\n", auth), &err);
    }
    if (cookie != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Cookie: atlas_session=%s\r\n", cookie), &err);
    }
    if (origin != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Origin: %s\r\n", origin), &err);
    }
    if (content_type != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Content-Type: %s\r\n", content_type), &err);
    } else if (body != NULL) {
        T_OK(atlas_buf_appendf(&req, &err, "Content-Type: application/x-www-form-urlencoded\r\n"),
            &err);
    }
    T_OK(atlas_buf_appendf(&req, &err, "Content-Length: %zu\r\n\r\n", blen), &err);
    if (blen > 0) {
        T_OK(atlas_buf_append(&req, body, blen, &err), &err);
    }
    T_OK(atlas_gateway_serve_bytes(g, req.data, req.len, resp, &err), &err);
    atlas_buf_free(&req);
}

/* POST with form body and optional bearer; no cookie, origin or explicit CT. */
static void post_form(atlas_gateway *g, const char *path, const char *auth, const char *body,
                      atlas_buf *resp) {
    http_request2(g, "POST", path, auth, NULL, NULL, NULL, body, (size_t)-1, resp);
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

static bool get_field(const atlas_buf *resp, const char *key, atlas_buf *out) {
    const char *b = body_of(resp);
    return tjson_get_string(b, strlen(b), key, out);
}

static bool extract_session_cookie(const atlas_buf *resp, char *out, size_t out_size) {
    static const char NEEDLE[] = "Set-Cookie: atlas_session=";
    const char *p = strstr(atlas_buf_cstr(resp), NEEDLE);
    if (p == NULL) {
        return false;
    }
    p += sizeof(NEEDLE) - 1u;
    size_t n = 0;
    while (p[n] != '\0' && p[n] != ';' && p[n] != '\r' && n + 1 < out_size) {
        n++;
    }
    if (n == 0) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Logs in with `token` and returns the session cookie. */
static void login_and_get_cookie(atlas_gateway *g, const char *token, char *cookie_out,
                                 size_t cookie_size) {
    atlas_buf lresp = ATLAS_BUF_INIT;
    char login_body[256];
    (void)snprintf(login_body, sizeof(login_body), "{\"key\":\"%s\"}", token);
    http_request2(g, "POST", "/auth/login", NULL, NULL, NULL, "application/json", login_body,
                 (size_t)-1, &lresp);
    T_REQUIRE_MSG(status_of(&lresp) == 200, "the login did not succeed: %d %s",
                 status_of(&lresp), body_of(&lresp));
    T_REQUIRE_MSG(extract_session_cookie(&lresp, cookie_out, cookie_size),
                 "the login response carried no session cookie: %s", atlas_buf_cstr(&lresp));
    atlas_buf_free(&lresp);
}

/* Build the bearer "Bearer <token>" string. */
static void bearer_of(const char *token, char *out, size_t out_size) {
    (void)snprintf(out, out_size, "Bearer %s", token);
}

/* --- (a): happy path ------------------------------------------------------- */

static void test_a_happy_path_and_audit(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    char gw_policy[2048];
    (void)snprintf(gw_policy, sizeof(gw_policy),
                   "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                   "web_gui = yes\nlisten_addr = 127.0.0.1\ntls_mode = REVERSE_PROXY\n"
                   "remote_submit_key = %s\nremote_submit_key = %s\n"
                   "remote_submit_driver = fake\nremote_submit_mode = patch\n"
                   "remote_submit_max_attempts = 1\nremote_submit_max_active = 100\n"
                   "remote_submit_max_per_day = 100\nremote_submit_gate = true\n",
                   (long)getuid(), e.submit_id, e.second_id);

    char orch_policy[1024];
    (void)snprintf(orch_policy, sizeof(orch_policy),
                   "dispatcher_uid = %ld\nsubmitter_uid = %ld\n"
                   "repo = proj\ndriver = fake\nmode = patch\nworker_root = /tmp\n",
                   (long)getuid(), (long)getuid());

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(gw_policy, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    /* (a1) Basic submit: 200 with job, key_id, budget. */
    atlas_buf resp = ATLAS_BUF_INIT;
    post_form(g, "/api/v1/job/submit", submit_bearer,
              "repo=proj&task=do+something&key=key1", &resp);
    T_CHECK_MSG(status_of(&resp) == 200, "submit did not answer 200: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"job\":") != NULL,
               "no job field in submit response: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"key_id\":") != NULL,
               "no key_id field in submit response: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"budget\":") != NULL,
               "no budget field in submit response: %s", body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"state\":\"QUEUED\"") != NULL,
               "submit state is not QUEUED: %s", body_of(&resp));

    /* The key_id in the response is the bare 16-hex selector (no "key_" prefix).
     * submit_id = "key_<16hex>"; response key_id = "<16hex>" = submit_id + 4. */
    {
        /* submit_id = "key_<16hex>", selector = bare 16-hex chars.
         * ATLAS_APIKEY_ID_PREFIX is "key_" (4 bytes), so the selector starts
         * at offset 4; use the literal to avoid format-truncation analysis
         * over a runtime-computed pointer. */
        char selector[ATLAS_APIKEY_SELECTOR_HEX + 1u];
        memcpy(selector, e.submit_id + 4u, ATLAS_APIKEY_SELECTOR_HEX);
        selector[ATLAS_APIKEY_SELECTOR_HEX] = '\0';
        char needle[64];
        (void)snprintf(needle, sizeof needle, "\"key_id\":\"%s\"", selector);
        T_CHECK_MSG(strstr(body_of(&resp), needle) != NULL,
                   "response key_id does not match the submit key selector: %s / want %s",
                   body_of(&resp), needle);
    }

    /* Capture the first job uid for the /get and /list tests. */
    atlas_buf job_uid = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_field(&resp, "job", &job_uid),
                 "no job field extractable from submit response: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* (a2) 70 000-byte body: the task value (23328 decoded bytes) fits the submit
     * bound (ATLAS_ORCH_TASK_MAX = 65536) and the body fits ATLAS_GW_SUBMIT_BODY_MAX_BYTES.
     * Body = "repo=proj&task=" (14) + 23325*"%01" (69975) + "AAA" (3) + "&key=ab" (8) = 70000. */
    {
        atlas_err berr;
        atlas_err_init(&berr);
        atlas_buf large_body = ATLAS_BUF_INIT;
        T_OK(atlas_buf_append_str(&large_body, "repo=proj&task=", &berr), &berr);
        for (size_t i = 0; i < 23325u; i++) {
            T_OK(atlas_buf_append_str(&large_body, "%41", &berr), &berr);
        }
        T_OK(atlas_buf_append_str(&large_body, "AAA&key=ab", &berr), &berr);
        T_REQUIRE_MSG(large_body.len == 70000u,
                     "large body construction has wrong length: %zu (want 70000)",
                     large_body.len);
        http_request2(g, "POST", "/api/v1/job/submit", submit_bearer, NULL, NULL, NULL,
                      large_body.data, large_body.len, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "70000-byte body did not answer 200: %d %s (body: %s)",
                   status_of(&resp), body_of(&resp), large_body.data);
        atlas_buf_free(&large_body);
        atlas_buf_reset(&resp);
    }

    /* (a3) 262145-byte body → 413. */
    {
        atlas_err berr;
        atlas_err_init(&berr);
        atlas_buf over_body = ATLAS_BUF_INIT;
        T_OK(atlas_buf_append_str(&over_body, "repo=proj&task=", &berr), &berr);
        /* Fill to exactly 262145 bytes total.
         * "repo=proj&task=" = 14 bytes; "&key=a" = 7 bytes; 262145 - 21 = 262124 bytes of 'A'. */
        for (size_t i = 0; i < 262124u; i++) {
            char ch = 'A';
            T_OK(atlas_buf_append(&over_body, &ch, 1u, &berr), &berr);
        }
        T_OK(atlas_buf_append_str(&over_body, "&key=a", &berr), &berr);
        T_REQUIRE_MSG(over_body.len == 262145u,
                     "over-limit body has wrong length: %zu (want 262145)", over_body.len);
        http_request2(g, "POST", "/api/v1/job/submit", submit_bearer, NULL, NULL, NULL,
                      over_body.data, over_body.len, &resp);
        T_CHECK_MSG(status_of(&resp) == 413,
                   "262145-byte body did not answer 413: %d %s", status_of(&resp), body_of(&resp));
        atlas_buf_free(&over_body);
        atlas_buf_reset(&resp);
    }

    /* (a4) /get → the job. */
    {
        char get_body[256];
        (void)snprintf(get_body, sizeof get_body, "job=%s", atlas_buf_cstr(&job_uid));
        post_form(g, "/api/v1/job/get", submit_bearer, get_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/get did not answer 200: %d %s", status_of(&resp), body_of(&resp));
        {
            char job_needle[128];
            (void)snprintf(job_needle, sizeof job_needle, "\"job\":\"%s\"",
                           atlas_buf_cstr(&job_uid));
            T_CHECK_MSG(strstr(body_of(&resp), job_needle) != NULL,
                       "/get response does not echo the job uid: %s", body_of(&resp));
        }
        atlas_buf_reset(&resp);
    }

    /* (a5) /list → at least one row. */
    {
        post_form(g, "/api/v1/job/list", submit_bearer, "", &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/list did not answer 200: %d %s", status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"jobs\":") != NULL,
                   "/list response has no jobs field: %s", body_of(&resp));
        atlas_buf_reset(&resp);
    }

    /* (a6) Submit a second job with the second key, then cancel it → CANCELLED. */
    {
        char second_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
        bearer_of(e.second_token, second_bearer, sizeof second_bearer);
        post_form(g, "/api/v1/job/submit", second_bearer,
                  "repo=proj&task=second+job&key=key2", &resp);
        T_REQUIRE_MSG(status_of(&resp) == 200, "second submit did not answer 200: %d %s",
                     status_of(&resp), body_of(&resp));
        atlas_buf second_job = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(get_field(&resp, "job", &second_job),
                     "no job field in second submit response: %s", body_of(&resp));
        atlas_buf_reset(&resp);

        char cancel_body[256];
        (void)snprintf(cancel_body, sizeof cancel_body, "job=%s",
                       atlas_buf_cstr(&second_job));
        post_form(g, "/api/v1/job/cancel", second_bearer, cancel_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/cancel did not answer 200: %d %s", status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"state\":\"CANCELLED\"") != NULL,
                   "cancel did not produce CANCELLED state: %s", body_of(&resp));
        atlas_buf_reset(&resp);
        atlas_buf_free(&second_job);
    }

    /* (a7) Audit trail: WEB_API rows with key_id = submit_id, no token bytes.
     * audit() is fire-and-forget: poll until the rows appear or time out. */
    {
        atlas_buf aresp = ATLAS_BUF_INIT;
        bool have_submit = false;
        for (int attempt = 0; attempt < 200; attempt++) {
            atlas_buf_reset(&aresp);
            call(&d, "gateway.audit_list", "{}", &aresp);
            have_submit =
                strstr(atlas_buf_cstr(&aresp), "\"/api/v1/job/submit\"") != NULL ||
                strstr(atlas_buf_cstr(&aresp), "/api/v1/job/submit") != NULL;
            if (have_submit) {
                break;
            }
            struct timespec ts = {0, 50 * 1000 * 1000};
            (void)nanosleep(&ts, NULL);
        }
        T_CHECK_MSG(have_submit, "no audit row for the submit route: %s",
                   atlas_buf_cstr(&aresp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), "\"interface\":\"WEB_API\"") != NULL,
                   "no WEB_API audit row: %s", atlas_buf_cstr(&aresp));
        /* The key_id in the audit row is the full "key_<16hex>" stored form. */
        {
            char needle[64];
            (void)snprintf(needle, sizeof needle, "\"key_id\":\"%s\"", e.submit_id);
            T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), needle) != NULL,
                       "audit trail does not name the submit key: %s / want %s",
                       atlas_buf_cstr(&aresp), needle);
        }
        /* No plaintext token bytes and no decoded task text in the audit table.
         * The first submit used "do+something" which decodes to "do something". */
        T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), e.submit_token) == NULL,
                   "the audit trail carries the bearer token plaintext");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), e.second_token) == NULL,
                   "the audit trail carries the second bearer token plaintext");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), "do something") == NULL,
                   "the audit trail carries the decoded task text");
        atlas_buf_free(&aresp);
    }

    atlas_buf_free(&job_uid);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (b): refusals --------------------------------------------------------- */

static void test_b_method_not_allowed(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    char gw_policy[2048];
    (void)snprintf(gw_policy, sizeof(gw_policy),
                   "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                   "web_gui = yes\ntls_mode = REVERSE_PROXY\n"
                   "remote_submit_key = %s\nremote_submit_driver = fake\n"
                   "remote_submit_mode = patch\nremote_submit_max_attempts = 1\n"
                   "remote_submit_max_active = 10\nremote_submit_max_per_day = 100\n",
                   (long)getuid(), e.submit_id);
    char orch_policy[512];
    (void)snprintf(orch_policy, sizeof(orch_policy),
                   "dispatcher_uid = %ld\nsubmitter_uid = %ld\n"
                   "repo = proj\ndriver = fake\nmode = patch\nworker_root = /tmp\n",
                   (long)getuid(), (long)getuid());

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy);
    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(gw_policy, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;

    /* GET → 405. */
    http_request2(g, "GET", "/api/v1/job/submit", submit_bearer, NULL, NULL, NULL, NULL,
                  (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 405, "GET did not answer 405: %d %s",
               status_of(&resp), body_of(&resp));
    atlas_buf_reset(&resp);

    /* JSON content type → 415 with the frozen disposal sentence. */
    http_request2(g, "POST", "/api/v1/job/submit", submit_bearer, NULL, NULL,
                  "application/json", "{\"repo\":\"proj\"}", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 415, "JSON CT did not answer 415: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp),
                       "a disposal request is application/x-www-form-urlencoded") != NULL,
               "wrong 415 sentence: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* No Authorization → 401 with the submit-needs-bearer sentence. */
    http_request2(g, "POST", "/api/v1/job/submit", NULL, NULL, NULL, NULL,
                  "repo=proj&task=hi", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 401, "no-auth did not answer 401: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "bearer token") != NULL,
               "401 sentence does not mention bearer token: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* Valid session cookie but no Authorization header → still 401.
     * The write route's api_handle_write calls neither session_get nor
     * anonymous_ok — the bearer is the only credential it accepts. */
    char cookie[256];
    login_and_get_cookie(g, e.browser_token, cookie, sizeof cookie);
    http_request2(g, "POST", "/api/v1/job/submit", NULL, cookie, NULL, NULL,
                  "repo=proj&task=hi", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 401,
               "cookie-only did not answer 401 (cookie must not help): %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "bearer token") != NULL,
               "cookie-path 401 sentence does not mention bearer token: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* Anonymous floor + matching Host (127.0.0.1:8787 = listen_addr) and no
     * Authorization → 401.  api_handle_write ignores the anonymous floor. */
    http_request2(g, "POST", "/api/v1/job/submit", NULL, NULL, NULL, NULL,
                  "repo=proj&task=hi", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 401,
               "anon-floor did not answer 401: %d %s", status_of(&resp), body_of(&resp));
    atlas_buf_reset(&resp);

    /* Unlisted key as bearer → 403 with the scope sentence. */
    char other_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.other_token, other_bearer, sizeof other_bearer);
    http_request2(g, "POST", "/api/v1/job/submit", other_bearer, NULL, NULL, NULL,
                  "repo=proj&task=hi", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 403,
               "unlisted key did not answer 403: %d %s", status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "jobs:submit") != NULL,
               "403 sentence does not mention jobs:submit scope: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* Malformed percent-escape → 400. */
    http_request2(g, "POST", "/api/v1/job/submit", submit_bearer, NULL, NULL, NULL,
                  "repo=proj&task=%GG&key=k1", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 400,
               "malformed %%GG did not answer 400: %d %s", status_of(&resp), body_of(&resp));
    atlas_buf_reset(&resp);

    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

static void test_b2_driver_dropped_and_no_key_policy(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Policy WITH submit keys — for the driver-dropped and cross-key tests. */
    char gw_policy[2048];
    (void)snprintf(gw_policy, sizeof(gw_policy),
                   "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                   "web_gui = yes\ntls_mode = REVERSE_PROXY\n"
                   "remote_submit_key = %s\nremote_submit_key = %s\n"
                   "remote_submit_driver = fake\nremote_submit_mode = patch\n"
                   "remote_submit_max_attempts = 1\nremote_submit_max_active = 100\n"
                   "remote_submit_max_per_day = 100\n",
                   (long)getuid(), e.submit_id, e.second_id);
    char orch_policy[512];
    (void)snprintf(orch_policy, sizeof(orch_policy),
                   "dispatcher_uid = %ld\nsubmitter_uid = %ld\n"
                   "repo = proj\ndriver = fake\nmode = patch\nworker_root = /tmp\n",
                   (long)getuid(), (long)getuid());

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy);
    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(gw_policy, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;

    /* driver= in body is silently dropped (not in the route's declared params).
     * The daemon accepts and uses the policy's driver ("fake"). */
    post_form(g, "/api/v1/job/submit", submit_bearer,
              "repo=proj&task=hello&key=k1&driver=attacker_driver", &resp);
    T_REQUIRE_MSG(status_of(&resp) == 200,
                 "submit with driver= in body did not answer 200: %d %s",
                 status_of(&resp), body_of(&resp));
    atlas_buf job_uid = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_field(&resp, "job", &job_uid),
                 "no job field: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* /get on that job verifies the stored driver is the policy's ("fake"),
     * not anything the body claimed. */
    {
        char get_body[256];
        (void)snprintf(get_body, sizeof get_body, "job=%s", atlas_buf_cstr(&job_uid));
        post_form(g, "/api/v1/job/get", submit_bearer, get_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/get after driver-dropped submit did not answer 200: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"driver\":\"fake\"") != NULL,
                   "stored driver is not the policy's fake: %s", body_of(&resp));
        atlas_buf_reset(&resp);
    }

    /* Submit a job with the second key, then try to GET it with the submit key.
     * The submit key's selector ≠ the second job's submit_key_id → USAGE → 400. */
    {
        char second_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
        bearer_of(e.second_token, second_bearer, sizeof second_bearer);
        post_form(g, "/api/v1/job/submit", second_bearer,
                  "repo=proj&task=second&key=k2", &resp);
        T_REQUIRE_MSG(status_of(&resp) == 200,
                     "second submit did not answer 200: %d %s",
                     status_of(&resp), body_of(&resp));
        atlas_buf second_job = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(get_field(&resp, "job", &second_job),
                     "no job field in second submit: %s", body_of(&resp));
        atlas_buf_reset(&resp);

        char get_body[256];
        (void)snprintf(get_body, sizeof get_body, "job=%s", atlas_buf_cstr(&second_job));
        /* Use submit key to GET second key's job → should be "no such job" → 400. */
        post_form(g, "/api/v1/job/get", submit_bearer, get_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 400,
                   "GET other key's job did not answer 400: %d %s",
                   status_of(&resp), body_of(&resp));
        atlas_buf_free(&second_job);
        atlas_buf_reset(&resp);
    }

    atlas_buf_free(&job_uid);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

static void test_b3_no_submit_key_policy(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Policy WITHOUT any remote_submit_key — route_offered returns false → 404. */
    char gw_policy[1024];
    (void)snprintf(gw_policy, sizeof(gw_policy),
                   "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                   "web_gui = yes\ntls_mode = REVERSE_PROXY\n",
                   (long)getuid());

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);
    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, NULL, &d, &err) == ATLAS_OK);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(gw_policy, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;
    http_request2(g, "POST", "/api/v1/job/submit", submit_bearer, NULL, NULL, NULL,
                  "repo=proj&task=hi", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 404,
               "no-submit-key policy did not answer 404: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "submission") != NULL,
               "404 sentence does not mention submission: %s", body_of(&resp));

    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (c): /auth/me fields -------------------------------------------------- */

static void test_c_auth_me_fields(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Instance 1: REVERSE_PROXY with two submit keys, no cleartext acceptance.
     * remote_submission=true, remote_submission_driver="fake", cleartext_submission=false. */
    {
        char ptext[2048];
        (void)snprintf(ptext, sizeof ptext,
                       "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                       "web_gui = yes\ntls_mode = REVERSE_PROXY\n"
                       "remote_submit_key = %s\nremote_submit_key = %s\n"
                       "remote_submit_driver = fake\nremote_submit_mode = patch\n"
                       "remote_submit_max_attempts = 1\nremote_submit_max_active = 10\n"
                       "remote_submit_max_per_day = 100\n",
                       (long)getuid(), e.submit_id, e.second_id);
        const char *gw_path = write_policy_file(&e, "c1.conf", ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_REQUIRE(gwd_start(&e, gw_path, NULL, &d, &err) == ATLAS_OK);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);
        atlas_gateway *g = NULL;
        open_http_gateway(ptext, &d, &g);

        char cookie[256];
        login_and_get_cookie(g, e.browser_token, cookie, sizeof cookie);

        atlas_buf resp = ATLAS_BUF_INIT;
        http_request2(g, "GET", "/auth/me", NULL, cookie, NULL, NULL, NULL, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/auth/me did not answer 200: %d %s", status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"remote_submission\":true") != NULL,
                   "remote_submission not true with submit keys: %s", body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"remote_submission_driver\":\"fake\"") != NULL,
                   "remote_submission_driver not fake: %s", body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"cleartext_submission\":false") != NULL,
                   "cleartext_submission not false under REVERSE_PROXY: %s", body_of(&resp));

        atlas_gateway_close(g);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&resp);
    }

    /* Instance 2: no submit keys at all (gui_env) → remote_submission=false. */
    {
        char ptext[512];
        (void)snprintf(ptext, sizeof ptext,
                       "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                       "web_gui = yes\ntls_mode = REVERSE_PROXY\n",
                       (long)getuid());
        const char *gw_path = write_policy_file(&e, "c2.conf", ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_REQUIRE(gwd_start(&e, gw_path, NULL, &d, &err) == ATLAS_OK);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);
        atlas_gateway *g = NULL;
        open_http_gateway(ptext, &d, &g);

        char cookie[256];
        login_and_get_cookie(g, e.browser_token, cookie, sizeof cookie);

        atlas_buf resp = ATLAS_BUF_INIT;
        http_request2(g, "GET", "/auth/me", NULL, cookie, NULL, NULL, NULL, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/auth/me did not answer 200: %d %s", status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"remote_submission\":false") != NULL,
                   "remote_submission not false under keyless policy: %s", body_of(&resp));

        atlas_gateway_close(g);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&resp);
    }

    /* Instance 3: cleartext acceptance → cleartext_submission=true. */
    {
        char ptext[2048];
        (void)snprintf(ptext, sizeof ptext,
                       "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                       "web_gui = yes\ntls_mode = NONE\n"
                       "remote_submit_key = %s\nremote_submit_driver = fake\n"
                       "remote_submit_mode = patch\nremote_submit_max_attempts = 1\n"
                       "remote_submit_max_active = 10\nremote_submit_max_per_day = 100\n"
                       "operator_accepts_cleartext_submission = yes\n",
                       (long)getuid(), e.submit_id);
        const char *gw_path = write_policy_file(&e, "c3.conf", ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_REQUIRE(gwd_start(&e, gw_path, NULL, &d, &err) == ATLAS_OK);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);
        atlas_gateway *g = NULL;
        open_http_gateway(ptext, &d, &g);

        char cookie[256];
        login_and_get_cookie(g, e.browser_token, cookie, sizeof cookie);

        atlas_buf resp = ATLAS_BUF_INIT;
        http_request2(g, "GET", "/auth/me", NULL, cookie, NULL, NULL, NULL, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "/auth/me did not answer 200: %d %s", status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"cleartext_submission\":true") != NULL,
                   "cleartext_submission not true under cleartext-accepted policy: %s",
                   body_of(&resp));

        atlas_gateway_close(g);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&resp);
    }

    env_close(&e);
}

/* --- test registry --------------------------------------------------------- */

static const atlas_test TESTS[] = {
    {"happy path: submit, large body, /get, /list, /cancel, audit trail",
     test_a_happy_path_and_audit},
    {"refusals: GET/405, JSON CT/415, no auth/401, cookie/401, anon/401, unlisted/403, malformed/400",
     test_b_method_not_allowed},
    {"refusals: driver= dropped, cross-key get/400",
     test_b2_driver_dropped_and_no_key_policy},
    {"refusals: policy with no submit key → 404 with submission sentence",
     test_b3_no_submit_key_policy},
    {"/auth/me: remote_submission, remote_submission_driver, cleartext_submission",
     test_c_auth_me_fields},
};

ATLAS_TEST_MAIN("gw_submit", TESTS)

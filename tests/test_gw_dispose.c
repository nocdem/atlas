/* Atlas - A16 T5: the daemon's remote disposal group, against a real daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is `test_decision_remote.c`'s own fixture shape (a repository, real
 * API-key credentials minted through the CLI, decisions proposed through the
 * CLI), one layer further out: everything here is driven over a Unix socket
 * against a real `atlas-gw-daemon` process, so what is proved is that the
 * *dispatcher* offers `decision.remote_challenge` and `decision.remote_dispose`
 * to the right peer under the right policy -- not merely that the write point
 * underneath them is correct, which `test_decision_remote.c` already
 * establishes in process.
 *
 * `atlas-gw-daemon` (`tests/tools/atlas_gw_daemon.c`) is the real
 * `atlas_daemon_run` with a gateway policy read from a plain, fixture-written
 * file rather than `/etc/atlas/gateway.conf` -- the only way to give this
 * suite a daemon whose `ctx->gwpolicy` actually has a `tls_mode`, a disposal
 * key and a `cleartext_disposal_accepted` value, since
 * `atlas_gwpolicy_load`'s root-ownership walk can never pass for a fixture.
 *
 * `test_gw_remote.c`'s legacy-mode trick -- an `atlas_gwpolicy` built only for
 * the in-process HTTP gateway object, with the daemon itself served with no
 * gwpolicy at all -- cannot reach this surface: legacy mode makes
 * `atlas_server_peer_is_gateway` true for the daemon's own uid regardless of
 * what policy anybody else holds, but `atlas_server_remote_disposal_offered`
 * additionally needs the *daemon's own* `ctx->gwpolicy` to name a real
 * disposal key, a real `tls_mode` and a real acceptance flag -- exactly the
 * fields a zeroed per-user policy never has. Hence a second daemon binary
 * instead of a bigger fixture.
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
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/gateway.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"
#include "atlas/service.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "ipc/server_internal.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

#ifndef ATLAS_GW_DAEMON_BIN
#define ATLAS_GW_DAEMON_BIN "atlas-gw-daemon"
#endif

/* --- the fixture ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
    char dispose_token[ATLAS_APIKEY_TOKEN_MAX];
    char dispose_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    char reader_token[ATLAS_APIKEY_TOKEN_MAX];
    char reader_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
} env;

static void run_atlas(env *e, const char *const *extra, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[24];
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

/* Proposes one document of `kind_name`, over the fixture's committed
 * `main.c`, and returns its public uid. Mirrors `test_decision_remote.c`'s
 * own helper. */
static void propose_kind(env *e, const char *kind_name, const char *title, atlas_buf *uid_out) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *propose[] = {
        "decision", "propose", "proj", "--kind", kind_name, "--title", title, "--decision", title,
        "--path",   "main.c",
    };
    run_atlas(e, propose, 11u, &out, &code);
    T_EQ_INT(code, 0);
    const char *p = strstr(atlas_buf_cstr(&out), ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE_MSG(p != NULL, "propose did not print a decision id: %s", atlas_buf_cstr(&out));
    size_t len = strlen(ATLAS_DECISION_UID_PREFIX) + ATLAS_DECISION_UID_HEX;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_set(uid_out, p, len, &err), &err);
    atlas_buf_free(&out);
}

/* Adds a second, PROPOSED revision to an existing document, so it has a
 * revision that is not the newest. */
static void revise_uid(env *e, const char *uid, const char *title) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *revise[] = {
        "decision", "revise", "proj", uid, "--title", title, "--decision", title,
    };
    run_atlas(e, revise, 8u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);
}

static void mint_key(env *e, const char *label, const char *scope_or_null, char *token_out,
                     size_t token_size, char *id_out, size_t id_size) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    if (scope_or_null == NULL) {
        const char *create[] = {"api-key", "create", "--label", label, "--no-scopes"};
        run_atlas(e, create, 5u, &out, &code);
    } else {
        const char *create[] = {"api-key", "create", "--label", label, "--scope", scope_or_null};
        run_atlas(e, create, 6u, &out, &code);
    }
    T_EQ_INT(code, 0);

    token_out[0] = '\0';
    {
        const char *s = strstr(atlas_buf_cstr(&out), "ATLAS_API_KEY=");
        T_REQUIRE(s != NULL);
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
        T_REQUIRE(s != NULL);
        s += strlen("id:     " ATLAS_APIKEY_ID_PREFIX);
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
    const char *scan[] = {"scan", "proj"};
    run_atlas(e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);

    mint_key(e, "dispose", NULL, e->dispose_token, sizeof(e->dispose_token), e->dispose_id,
            sizeof(e->dispose_id));
    mint_key(e, "reader", "decisions:read", e->reader_token, sizeof(e->reader_token),
            e->reader_id, sizeof(e->reader_id));
}

static void env_close(env *e) {
    fx_close(&e->fx);
}

/* --- writing a policy file --------------------------------------------------- */

static void write_policy(env *e, const char *name, const char *text, atlas_buf *path_out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_write(atlas_buf_cstr(&e->fx.root), name, text, &err), &err);
    atlas_buf_reset(path_out);
    T_OK(atlas_buf_appendf(path_out, &err, "%s/%s", atlas_buf_cstr(&e->fx.root), name), &err);
}

/* --- starting the real remote-disposal daemon -------------------------------
 *
 * `atlas-watch-daemon`'s own shape (`fx_daemon_start`, restated for a
 * different binary and a different pair of arguments): fork, redirect stdio,
 * exec. `fx_daemon_wait_ready` / `fx_daemon_stop` / `fx_daemon_free` are
 * reused unchanged -- they are generic over the `fx_daemon` struct's fields
 * and never reference the binary that filled them in. */
static atlas_status gwd_start(env *e, const char *policy_path, fx_daemon *d, atlas_err *err) {
    atlas_status dst = atlas_buf_set_str(&d->data_dir, fx_data_dir(&e->fx), err);
    if (dst != ATLAS_OK) {
        return dst;
    }
    atlas_status st = atlas_buf_set(&d->runtime_dir, e->fx.root.data, e->fx.root.len, err);
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

    const char *argv[] = {ATLAS_GW_DAEMON_BIN, atlas_buf_cstr(&d->data_dir), policy_path, NULL};
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

/* Runs `atlas-gw-daemon` to completion and reports its exit code plus
 * whatever it wrote to stderr -- for the one case where the daemon is
 * expected never to start serving at all: a policy the loader refuses. */
static int gwd_run_expect_exit(const char *data_dir, const char *policy_path,
                               atlas_buf *stderr_out) {
    int pfd[2];
    T_REQUIRE(pipe(pfd) == 0);
    pid_t pid = fork();
    T_REQUIRE(pid >= 0);
    if (pid == 0) {
        (void)close(pfd[0]);
        (void)dup2(pfd[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
        }
        (void)close(pfd[1]);
        const char *argv[] = {ATLAS_GW_DAEMON_BIN, data_dir, policy_path, NULL};
        const char *envp[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
        union {
            const char *const *in;
            char *const *out;
        } a = {argv}, e = {envp};
        (void)execve(ATLAS_GW_DAEMON_BIN, a.out, e.out);
        _exit(127);
    }
    (void)close(pfd[1]);
    if (stderr_out != NULL) {
        char buf[4096];
        atlas_err ignored;
        atlas_err_init(&ignored);
        for (;;) {
            ssize_t n = read(pfd[0], buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            (void)atlas_buf_append(stderr_out, buf, (size_t)n, &ignored);
        }
    }
    (void)close(pfd[0]);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* --- small IPC helpers ------------------------------------------------------- */

static void call(const fx_daemon *d, const char *method, const char *params, atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(resp);
    (void)atlas_ipc_call(atlas_buf_cstr(&d->socket), method, params, resp, &err);
}

static bool get_str(const atlas_buf *body, const char *key, atlas_buf *out) {
    return tjson_get_string(body->data, body->len, key, out);
}

/* --- the HTTP half: an in-process gateway pointed at the real daemon --------
 *
 * T6's own surface. `atlas_gateway_serve_bytes` needs no listening port, so
 * the write table's whole HTTP path -- routing, the bearer-only principal,
 * the scope check, the frozen refusals and their status codes -- is
 * exercised here against the *same* real `atlas-gw-daemon` process the
 * IPC-level tests above already use, over the identical policy text it was
 * started with. */

static void open_http_gateway(const char *policy_text, const fx_daemon *d, atlas_gateway **g_out) {
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

/* Builds one complete HTTP/1.1 request and returns the gateway's response.
 * Every optional argument is NULL to omit that header. `content_type` NULL
 * with a non-NULL `body` defaults to the write routes' own frozen shape,
 * since every test below that sends a body is sending a disposal. `origin`
 * is separate from the rest because the preflight case needs it without an
 * Authorization header at all. */
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

/* The common case: a POST with a form body and an optional bearer, nothing
 * else. */
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

/* `get_str` reads a field out of an `atlas_buf` holding a whole JSON
 * document; an HTTP response's body is a slice of a larger buffer, so this
 * reads directly off `body_of`'s pointer instead of copying it into one. */
static bool get_field(const atlas_buf *resp, const char *key, atlas_buf *out) {
    const char *b = body_of(resp);
    return tjson_get_string(b, strlen(b), key, out);
}

/* Extracts the `atlas_session` cookie value from a `Set-Cookie` response
 * header, for the "a valid session cookie must not help" refusal case. */
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

/* Logs in with `token` and returns the session cookie `/auth/me` (a
 * session-cookie-only route -- it never reads a bearer header) needs. */
static void login_and_get_cookie(atlas_gateway *g, const char *token, char *cookie_out,
                                 size_t cookie_size) {
    atlas_buf lresp = ATLAS_BUF_INIT;
    char login_body[256];
    (void)snprintf(login_body, sizeof(login_body), "{\"key\":\"%s\"}", token);
    http_request2(g, "POST", "/auth/login", NULL, NULL, NULL, "application/json", login_body,
                 (size_t)-1, &lresp);
    T_REQUIRE_MSG(status_of(&lresp) == 200, "the login did not succeed: %d %s", status_of(&lresp),
                 body_of(&lresp));
    T_REQUIRE_MSG(extract_session_cookie(&lresp, cookie_out, cookie_size),
                 "the login response carried no session cookie: %s", atlas_buf_cstr(&lresp));
    atlas_buf_free(&lresp);
}

/* --- (a): gateway.auth's derived scope --------------------------------------- */

static void test_a_gateway_auth_scope_derivation(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    char params[512];

    /* Instance 1: the full policy, TLS in front. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\nlisten_addr = 127.0.0.1\ntls_mode = "
                               "REVERSE_PROXY\nremote_dispose_key = key_%s\n"
                               "remote_dispose_kinds = OPERATIONAL_FACT\n",
                               (long long)getuid(), e.dispose_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "p1.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

        atlas_buf resp = ATLAS_BUF_INIT;
        (void)snprintf(params, sizeof(params), "{\"token\":\"%s\"}", e.dispose_token);
        call(&d, "gateway.auth", params, &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"authenticated\":true") != NULL,
                    "the disposal key did not authenticate: %s", atlas_buf_cstr(&resp));
        atlas_buf scopes = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(get_str(&resp, "scopes", &scopes), "no scopes field: %s",
                     atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&scopes), "decisions:dispose") == 0,
                    "wrong derived scope: %s", atlas_buf_cstr(&scopes));
        atlas_buf_free(&scopes);
        atlas_buf_reset(&resp);

        (void)snprintf(params, sizeof(params), "{\"token\":\"%s\"}", e.reader_token);
        call(&d, "gateway.auth", params, &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"authenticated\":true") != NULL,
                    "the reader key did not authenticate: %s", atlas_buf_cstr(&resp));
        T_REQUIRE_MSG(get_str(&resp, "scopes", &scopes), "no scopes field: %s",
                     atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&scopes), "decisions:read") == 0,
                    "the reader's own scopes were altered: %s", atlas_buf_cstr(&scopes));
        atlas_buf_free(&scopes);
        atlas_buf_free(&resp);
        atlas_buf_free(&ppath);

        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
    }

    /* Instance 2: no disposal keys at all. The dispose credential still
     * authenticates -- it is a real, active credential -- but its stored
     * scope list is empty and nothing here names it, so the reported scopes
     * are empty. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\nlisten_addr = 127.0.0.1\ntls_mode = NONE\n",
                               (long long)getuid()),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "p2.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

        atlas_buf resp = ATLAS_BUF_INIT;
        (void)snprintf(params, sizeof(params), "{\"token\":\"%s\"}", e.dispose_token);
        call(&d, "gateway.auth", params, &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"authenticated\":true") != NULL,
                    "the disposal key did not authenticate under a keyless policy: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf scopes = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(get_str(&resp, "scopes", &scopes), "no scopes field: %s",
                     atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&scopes), "") == 0,
                    "a policy naming no disposal key still derived one: %s",
                    atlas_buf_cstr(&scopes));
        atlas_buf_free(&scopes);
        atlas_buf_free(&resp);
        atlas_buf_free(&ppath);

        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
    }

    /* Instance 3: the policy names the *reader* credential as the disposal
     * key. `atlas_gwpolicy_parse_buffer` checks only the shape of
     * `remote_dispose_key` -- it opens no database and cannot know a
     * credential's stored scopes -- so this loads exactly as cleanly as
     * naming the scopeless one does. This is the case `rec.mask == 0u` in
     * `method_gateway_auth` exists for: with the reader key's own id equal
     * to the policy's `remote_dispose_key`, the `strcmp` alone would match
     * and derive `decisions:dispose` for a credential that already holds
     * `decisions:read` -- widening a real, scoped credential rather than
     * activating an inert one. Every other instance in this test names the
     * `--no-scopes` dispose credential, where `strcmp` failing for the
     * reader key already refuses it and `rec.mask == 0u` is never the
     * reason; this is the one case that isolates it. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\nlisten_addr = 127.0.0.1\ntls_mode = "
                               "REVERSE_PROXY\nremote_dispose_key = key_%s\n"
                               "remote_dispose_kinds = OPERATIONAL_FACT\n",
                               (long long)getuid(), e.reader_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "p3.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

        atlas_buf resp = ATLAS_BUF_INIT;
        (void)snprintf(params, sizeof(params), "{\"token\":\"%s\"}", e.reader_token);
        call(&d, "gateway.auth", params, &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"authenticated\":true") != NULL,
                    "the reader key did not authenticate: %s", atlas_buf_cstr(&resp));
        atlas_buf scopes = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(get_str(&resp, "scopes", &scopes), "no scopes field: %s",
                     atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&scopes), "decisions:read") == 0,
                    "naming an already-scoped credential as the disposal key widened it: %s",
                    atlas_buf_cstr(&scopes));
        atlas_buf_free(&scopes);
        atlas_buf_free(&resp);
        atlas_buf_free(&ppath);

        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
    }

    env_close(&e);
}

/* --- (b): the full remote-disposal happy path -------------------------------- */

/* Orphan #1 (review round 1): a collector for `atlas_service_decision_history_
 * remote`, called in-process against a real daemon exactly as
 * `test_remote_equivalence.c` calls the `_remote` twins -- the only way to
 * exercise `service_remote.c:1684`'s own `key_id` parse, since the CLI's
 * remote branch is reached only for a *foreign* index (`index_is_foreign`,
 * A7.1) and this fixture's own data directory is never that. */
typedef struct remote_key_capture {
    char key_id[ATLAS_APIKEY_SELECTOR_HEX + 1u];
    bool found;
} remote_key_capture;

static atlas_status capture_remote_key_id(const atlas_decision_timeline_entry *e, void *ud,
                                          atlas_err *err) {
    (void)err;
    remote_key_capture *kc = (remote_key_capture *)ud;
    if (e->key_id != NULL && e->event != NULL && strcmp(e->event, "REJECTED") == 0) {
        (void)snprintf(kc->key_id, sizeof(kc->key_id), "%s", e->key_id);
        kc->found = true;
    }
    return ATLAS_OK;
}

static void test_b_remote_dispose_happy_path(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf fact_uid = ATLAS_BUF_INIT;
    propose_kind(&e, "OPERATIONAL_FACT", "a live endpoint", &fact_uid);

    char before_digest[65];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before_digest, &err), &err);

    atlas_buf ptext = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ptext, &err,
                           "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                           "web_gui = yes\nlisten_addr = 127.0.0.1\ntls_mode = "
                           "REVERSE_PROXY\nremote_dispose_key = key_%s\n"
                           "remote_dispose_kinds = OPERATIONAL_FACT\n",
                           (long long)getuid(), e.dispose_id),
         &err);
    atlas_buf ppath = ATLAS_BUF_INIT;
    write_policy(&e, "p.conf", atlas_buf_cstr(&ptext), &ppath);
    atlas_buf_free(&ptext);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    char params[512];
    atlas_buf resp = ATLAS_BUF_INIT;
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,\"intent\":\"reject\","
                   "\"token\":\"%s\"}",
                   atlas_buf_cstr(&fact_uid), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL, "the mint failed: %s",
                atlas_buf_cstr(&resp));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"confirm\"") == NULL,
                "the remote mint response carried a \"confirm\" key: %s", atlas_buf_cstr(&resp));

    atlas_buf token = ATLAS_BUF_INIT, hash = ATLAS_BUF_INIT, expires = ATLAS_BUF_INIT,
             key_id = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_str(&resp, "token", &token), "no token: %s", atlas_buf_cstr(&resp));
    T_REQUIRE_MSG(get_str(&resp, "content_hash", &hash), "no content_hash: %s",
                 atlas_buf_cstr(&resp));
    T_REQUIRE_MSG(get_str(&resp, "expires_at", &expires), "no expires_at: %s",
                 atlas_buf_cstr(&resp));
    T_REQUIRE_MSG(get_str(&resp, "key_id", &key_id), "no key_id: %s", atlas_buf_cstr(&resp));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&key_id), e.dispose_id) == 0, "wrong key_id: %s vs %s",
                atlas_buf_cstr(&key_id), e.dispose_id);

    char confirm[16];
    (void)snprintf(confirm, sizeof(confirm), "%.8s", atlas_buf_cstr(&hash));

    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"intent\":\"reject\","
                   "\"token\":\"%s\",\"challenge\":\"%s\",\"confirmation\":\"%s\"}",
                   atlas_buf_cstr(&fact_uid), e.dispose_token, atlas_buf_cstr(&token), confirm);
    call(&d, "decision.remote_dispose", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL, "the dispose failed: %s",
                atlas_buf_cstr(&resp));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"state\":\"REJECTED\"") != NULL,
                "wrong state: %s", atlas_buf_cstr(&resp));
    atlas_buf actor = ATLAS_BUF_INIT, means = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_str(&resp, "actor", &actor), "no actor: %s", atlas_buf_cstr(&resp));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&actor), "REMOTE_OPERATOR_CONFIRMED") == 0, "wrong actor: %s",
                atlas_buf_cstr(&actor));
    T_REQUIRE_MSG(get_str(&resp, "actor_means", &means), "no actor_means: %s",
                 atlas_buf_cstr(&resp));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&means), "weaker than the local channel") != NULL,
                "actor_means missing the honesty limit: %s", atlas_buf_cstr(&means));
    atlas_buf out_key_id = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_str(&resp, "key_id", &out_key_id), "no key_id on the dispose response: %s",
                 atlas_buf_cstr(&resp));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&out_key_id), e.dispose_id) == 0, "wrong key_id: %s",
                atlas_buf_cstr(&out_key_id));

    /* decision.history shows the event with key_id -- the raw daemon
     * response, proving the wire format this season's write point actually
     * produces (`on_event` in server_decision.c). */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params), "{\"repo\":\"proj\",\"decision\":\"%s\"}",
                   atlas_buf_cstr(&fact_uid));
    call(&d, "decision.history", params, &resp);
    char needle[80];
    (void)snprintf(needle, sizeof(needle), "\"key_id\":\"%s\"", e.dispose_id);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), needle) != NULL,
                "decision.history does not carry the credential id: %s", atlas_buf_cstr(&resp));

    /* Orphan #1 (review round 1): the CLI's own `--json` rendering, exercised
     * end to end through the daemon this test already has running --
     * `atlas_decision_timeline_entry` gained a `key_id` member,
     * `service_decision.c`'s local `on_event` and `service_remote.c`'s
     * daemon-response parse both fill it, and `render_json.c`/`render_human.c`
     * both emit it. T10's brief verifies the human `credential: key_…` line
     * during live acceptance; this is the first place either rendering is
     * actually exercised. */
    {
        atlas_buf jout = ATLAS_BUF_INIT;
        int jcode = -1;
        const char *hist_json[] = {"decision", "history", "proj", atlas_buf_cstr(&fact_uid),
                                   "--json"};
        run_atlas(&e, hist_json, 5u, &jout, &jcode);
        T_EQ_INT(jcode, 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&jout), needle) != NULL,
                    "the CLI's --json rendering does not carry the credential id: %s",
                    atlas_buf_cstr(&jout));
        atlas_buf_free(&jout);

        atlas_buf hout = ATLAS_BUF_INIT;
        int hcode = -1;
        const char *hist_human[] = {"decision", "history", "proj", atlas_buf_cstr(&fact_uid)};
        run_atlas(&e, hist_human, 4u, &hout, &hcode);
        T_EQ_INT(hcode, 0);
        char credential_needle[80];
        (void)snprintf(credential_needle, sizeof(credential_needle), "credential: %s",
                       e.dispose_id);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&hout), credential_needle) != NULL,
                    "the human rendering does not carry \"credential: %s\": %s", e.dispose_id,
                    atlas_buf_cstr(&hout));
        atlas_buf_free(&hout);
    }

    /* Orphan #1, the fifth file: `service_remote.c`'s own parse of `key_id`,
     * proved by calling `atlas_service_decision_history_remote` directly, in
     * this process, against this same daemon -- `test_remote_equivalence.c`'s
     * own precedent for how a `_remote` function is pointed at a fixture
     * daemon rather than a real one. The two environment variables are
     * restored immediately after, since this test binary runs every test
     * function in one process and nothing later in this file expects either
     * set. */
    {
        const char *old_xdg = getenv("XDG_RUNTIME_DIR");
        const char *old_dd = getenv("ATLAS_DATA_DIR");
        char *saved_xdg = old_xdg != NULL ? strdup(old_xdg) : NULL;
        char *saved_dd = old_dd != NULL ? strdup(old_dd) : NULL;
        T_REQUIRE(setenv("XDG_RUNTIME_DIR", atlas_buf_cstr(&d.runtime_dir), 1) == 0);
        T_REQUIRE(setenv("ATLAS_DATA_DIR", fx_data_dir(&e.fx), 1) == 0);

        remote_key_capture kc;
        memset(&kc, 0, sizeof(kc));
        bool ragrees = true;
        atlas_status rst = atlas_service_decision_history_remote(
            "proj", atlas_buf_cstr(&fact_uid), NULL, capture_remote_key_id, &kc, &ragrees, &err);
        T_OK(rst, &err);
        T_CHECK_MSG(kc.found,
                    "service_remote.c's parse never saw a REJECTED event carrying a key_id");
        T_CHECK_MSG(strcmp(kc.key_id, e.dispose_id) == 0,
                    "service_remote.c parsed the wrong key_id: %s vs %s", kc.key_id,
                    e.dispose_id);

        if (saved_xdg != NULL) {
            (void)setenv("XDG_RUNTIME_DIR", saved_xdg, 1);
            free(saved_xdg);
        } else {
            (void)unsetenv("XDG_RUNTIME_DIR");
        }
        if (saved_dd != NULL) {
            (void)setenv("ATLAS_DATA_DIR", saved_dd, 1);
            free(saved_dd);
        } else {
            (void)unsetenv("ATLAS_DATA_DIR");
        }
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);

    atlas_buf doctor = ATLAS_BUF_INIT;
    int code = -1;
    const char *doc[] = {"doctor"};
    run_atlas(&e, doc, 1u, &doctor, &code);
    /* M1 (review round 1): the exit code was captured and never checked, and
     * the only assertion below is an absence -- both together mean an empty
     * `doctor` output with a non-zero exit would have passed. `doctor` exits
     * `ATLAS_ERR_CONFIG` (3) when `atlas_doctor_report.ok` is false
     * (`cli.c`'s "doctor reports a problem through its exit code"), so 0 here
     * means the report actually found nothing wrong, and "status: ok" is the
     * positive anchor proving the output is the real report and not empty. */
    T_EQ_INT(code, 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&doctor), "status: ok") != NULL,
                "doctor did not report a clean status: %s", atlas_buf_cstr(&doctor));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&doctor), "cached status disagrees") == NULL,
                "doctor reports the ledger disagreeing with the cache: %s",
                atlas_buf_cstr(&doctor));

    char after_digest[65];
    T_OK(fx_tree_digest(fx_repo(&e.fx), after_digest, &err), &err);
    T_CHECK_MSG(strcmp(before_digest, after_digest) == 0,
                "the repository tree changed during a remote disposal");

    atlas_buf_free(&token);
    atlas_buf_free(&hash);
    atlas_buf_free(&expires);
    atlas_buf_free(&key_id);
    atlas_buf_free(&actor);
    atlas_buf_free(&means);
    atlas_buf_free(&out_key_id);
    atlas_buf_free(&resp);
    atlas_buf_free(&doctor);
    atlas_buf_free(&ppath);
    atlas_buf_free(&fact_uid);
    env_close(&e);
}

/* --- (c): a plain daemon offers neither name --------------------------------- */

static void test_c_hidden_from_a_plain_daemon(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_REQUIRE(fx_open(&fx, &err) == ATLAS_OK);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);

    static const char *const NAMES[] = {"decision.remote_challenge", "decision.remote_dispose"};
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        call(&d, NAMES[i], "{}", &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                    "\"%s\" is reachable on a daemon with no gateway policy: %s", NAMES[i],
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- (d): the policy gate ----------------------------------------------------- */

static void test_d_policy_gate(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* d1: keys present, no tls_mode, no acceptance -- refused by the loader.
     * The tool never binds a socket, so the property under test is the exit
     * code and the message, not a live daemon's answer. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\nremote_dispose_key = key_%s\n"
                               "remote_dispose_kinds = OPERATIONAL_FACT\n",
                               (long long)getuid(), e.dispose_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "d1.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        atlas_buf stderr_out = ATLAS_BUF_INIT;
        int rc = gwd_run_expect_exit(fx_data_dir(&e.fx), atlas_buf_cstr(&ppath), &stderr_out);
        T_CHECK_MSG(rc != 0, "the tool did not refuse a policy naming keys with no TLS stance");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&stderr_out), "MALFORMED") != NULL,
                    "the refusal did not name MALFORMED: %s", atlas_buf_cstr(&stderr_out));
        atlas_buf_free(&stderr_out);
        atlas_buf_free(&ppath);
    }

    /* d2: keys absent, tls_mode = REVERSE_PROXY -- both names unknown. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = REVERSE_PROXY\n",
                               (long long)getuid()),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "d2.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

        static const char *const NAMES[] = {"decision.remote_challenge",
                                            "decision.remote_dispose"};
        for (size_t i = 0; i < 2; i++) {
            atlas_buf resp = ATLAS_BUF_INIT;
            call(&d, NAMES[i], "{}", &resp);
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                        "\"%s\" is reachable with no disposal key named: %s", NAMES[i],
                        atlas_buf_cstr(&resp));
            atlas_buf_free(&resp);
        }
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&ppath);
    }

    /* d3: keys present, tls_mode = NONE, acceptance = yes -- offered, and the
     * happy path completes exactly as under REVERSE_PROXY. */
    {
        atlas_buf fact_uid = ATLAS_BUF_INIT;
        propose_kind(&e, "OPERATIONAL_FACT", "a second endpoint", &fact_uid);

        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = NONE\nremote_dispose_key = key_%s\n"
                               "remote_dispose_kinds = OPERATIONAL_FACT\n"
                               "operator_accepts_cleartext_disposal = yes\n",
                               (long long)getuid(), e.dispose_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "d3.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

        char params[512];
        atlas_buf resp = ATLAS_BUF_INIT;
        (void)snprintf(params, sizeof(params), "{\"token\":\"%s\"}", e.dispose_token);
        call(&d, "gateway.auth", params, &resp);
        atlas_buf scopes = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(get_str(&resp, "scopes", &scopes), "no scopes: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&scopes), "decisions:dispose") == 0,
                    "the cleartext-accepted policy derived a different scope: %s",
                    atlas_buf_cstr(&scopes));
        atlas_buf_free(&scopes);
        atlas_buf_reset(&resp);

        (void)snprintf(params, sizeof(params),
                       "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,"
                       "\"intent\":\"reject\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&fact_uid), e.dispose_token);
        call(&d, "decision.remote_challenge", params, &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL,
                    "the mint failed under the accepted-cleartext policy: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf token = ATLAS_BUF_INIT, hash = ATLAS_BUF_INIT;
        T_REQUIRE(get_str(&resp, "token", &token));
        T_REQUIRE(get_str(&resp, "content_hash", &hash));
        char confirm[16];
        (void)snprintf(confirm, sizeof(confirm), "%.8s", atlas_buf_cstr(&hash));
        atlas_buf_reset(&resp);
        (void)snprintf(params, sizeof(params),
                       "{\"repo\":\"proj\",\"decision\":\"%s\",\"intent\":\"reject\","
                       "\"token\":\"%s\",\"challenge\":\"%s\",\"confirmation\":\"%s\"}",
                       atlas_buf_cstr(&fact_uid), e.dispose_token, atlas_buf_cstr(&token),
                       confirm);
        call(&d, "decision.remote_dispose", params, &resp);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL,
                    "the dispose failed under the accepted-cleartext policy: %s",
                    atlas_buf_cstr(&resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"state\":\"REJECTED\"") != NULL,
                    "wrong state: %s", atlas_buf_cstr(&resp));

        atlas_buf_free(&token);
        atlas_buf_free(&hash);
        atlas_buf_free(&resp);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&ppath);
        atlas_buf_free(&fact_uid);
    }

    /* d4: every policy condition passes -- state ENABLED, a named disposal
     * key, its kinds, TLS in front -- and only the peer half of the gate can
     * refuse. `gateway_uid` names a uid one away from the test process's own,
     * so `atlas_server_peer_is_gateway` compares the policy's uid against the
     * real `SO_PEERCRED` peer and the two never match: this daemon has a
     * fully valid disposal policy for a *different* gateway. This is the
     * regression I1 asked for: every other test in this file either fails
     * the policy half (d1, d2) or connects as the uid the policy actually
     * names (d3, test_a, test_b), so nothing before this exercised the peer
     * test at all -- confirmed by deleting it (see the report: the whole
     * suite still passed). Asserts `unknown method` for both disposal
     * methods and for `gateway.auth`, which this uid is equally not the
     * gateway for. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                               "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n",
                               (long long)getuid() + 1, e.dispose_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "d4.conf", atlas_buf_cstr(&ptext), &ppath);
        atlas_buf_free(&ptext);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

        static const char *const NAMES[] = {"decision.remote_challenge",
                                            "decision.remote_dispose", "gateway.auth"};
        for (size_t i = 0; i < 3; i++) {
            atlas_buf resp = ATLAS_BUF_INIT;
            char params[256];
            if (strcmp(NAMES[i], "gateway.auth") == 0) {
                (void)snprintf(params, sizeof(params), "{\"token\":\"%s\"}", e.dispose_token);
            } else {
                (void)snprintf(params, sizeof(params), "{}");
            }
            call(&d, NAMES[i], params, &resp);
            T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                        "\"%s\" is reachable for a peer the policy does not name as its "
                        "gateway, under an otherwise fully satisfied policy: %s",
                        NAMES[i], atlas_buf_cstr(&resp));
            atlas_buf_free(&resp);
        }
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&ppath);
    }

    env_close(&e);
}

/* --- (e): the write point's frozen refusals, reached through the daemon ----- */

static void test_e_refusals(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf d_rev0 = ATLAS_BUF_INIT, d_stale = ATLAS_BUF_INIT, d_policy = ATLAS_BUF_INIT,
             d_supersede = ATLAS_BUF_INIT, d_revalidate = ATLAS_BUF_INIT,
             d_confirm = ATLAS_BUF_INIT;
    propose_kind(&e, "OPERATIONAL_FACT", "revision-zero target", &d_rev0);
    propose_kind(&e, "OPERATIONAL_FACT", "a stale target", &d_stale);
    revise_uid(&e, atlas_buf_cstr(&d_stale), "a stale target, revised");
    propose_kind(&e, "POLICY", "a process rule", &d_policy);
    propose_kind(&e, "OPERATIONAL_FACT", "a supersede target", &d_supersede);
    propose_kind(&e, "OPERATIONAL_FACT", "a revalidate target", &d_revalidate);
    propose_kind(&e, "OPERATIONAL_FACT", "a confirmation target", &d_confirm);

    atlas_buf ptext = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ptext, &err,
                           "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                           "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                           "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n",
                           (long long)getuid(), e.dispose_id),
         &err);
    atlas_buf ppath = ATLAS_BUF_INIT;
    write_policy(&e, "e.conf", atlas_buf_cstr(&ptext), &ppath);
    atlas_buf_free(&ptext);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    char params[512];
    atlas_buf resp = ATLAS_BUF_INIT;

    /* revision = 0 */
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":0,\"intent\":\"reject\","
                   "\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_rev0), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "0 is not a revision") != NULL,
                "revision=0 was not refused with its frozen sentence: %s",
                atlas_buf_cstr(&resp));

    /* a non-newest revision. Review round 1: this sentence moved from
     * ATLAS_ERR_USAGE (400/exit 2) to ATLAS_ERR_INTEGRITY (409/exit 7) --
     * it is a refusal about the document's state, not about this
     * well-formed request, matching its spend-time twin ("this decision
     * gained revision ... after the challenge was minted") a few lines
     * below, which was INTEGRITY already. `"status"` on the wire is
     * `(int64_t)err->status` verbatim (`server.c`'s error-response writer),
     * so 7 here is the direct, positive proof of the class, not an
     * inference from the sentence text. */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,\"intent\":\"reject\","
                   "\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_stale), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "is minted only for the newest revision") != NULL,
                "a non-newest revision was not refused with its frozen sentence: %s",
                atlas_buf_cstr(&resp));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"status\":7") != NULL,
                "a non-newest revision was not refused as ATLAS_ERR_INTEGRITY (7): %s",
                atlas_buf_cstr(&resp));

    /* a kind outside the policy's list */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,\"intent\":\"reject\","
                   "\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_policy), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(
        strstr(atlas_buf_cstr(&resp), "is not one the remote disposal policy names") != NULL,
        "a POLICY-kind record was not refused with its frozen sentence: %s",
        atlas_buf_cstr(&resp));

    /* a supersede intent.
     *
     * `decision.remote_challenge` never reads a "replacement" parameter --
     * the Frozen formats' own params list for it has none, on purpose: a
     * browser offers no supersede action. Review finding I4 moved the
     * REMOTE-and-(SUPERSEDE|REVALIDATE) check in `op_challenge`
     * (`src/decision/lifecycle.c`) ahead of the replacement/supersede
     * handling that used to run first, so a REMOTE SUPERSEDE request no
     * longer falls into the ordinary "no replacement named" refusal (USAGE,
     * 400 -- actionable advice this endpoint has no way to act on, since it
     * never reads a `replacement` parameter) before reaching this
     * REMOTE-specific one (INTEGRITY, 409). Both SUPERSEDE and REVALIDATE
     * now reach the same frozen sentence through this real endpoint; before
     * the fix, only REVALIDATE did (see the report for the prior, wrong,
     * claim that neither did). */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,"
                   "\"intent\":\"supersede\",\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_supersede), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "are not offered from the browser") != NULL,
                "a supersede intent was not refused with the REMOTE-channel frozen sentence: %s",
                atlas_buf_cstr(&resp));

    /* a revalidate intent reaches the same REMOTE-channel refusal. Before
     * I4's fix this was already true (REVALIDATE never took the
     * replacement/supersede branch above it); after the fix it is true for
     * the same reason SUPERSEDE now is, so this sub-case doubles as I4's own
     * regression test. */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,"
                   "\"intent\":\"revalidate\",\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_revalidate), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "are not offered from the browser") != NULL,
                "a revalidate intent was not refused with the REMOTE-channel frozen sentence: %s",
                atlas_buf_cstr(&resp));

    /* a wrong confirmation, then the right one, then a replay -- proving the
     * consumed count moves only on the one success. */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,\"intent\":\"reject\","
                   "\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_confirm), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL, "the mint failed: %s",
                atlas_buf_cstr(&resp));
    atlas_buf token = ATLAS_BUF_INIT, hash = ATLAS_BUF_INIT;
    T_REQUIRE(get_str(&resp, "token", &token));
    T_REQUIRE(get_str(&resp, "content_hash", &hash));

    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"intent\":\"reject\","
                   "\"token\":\"%s\",\"challenge\":\"%s\",\"confirmation\":\"00000000\"}",
                   atlas_buf_cstr(&d_confirm), e.dispose_token, atlas_buf_cstr(&token));
    call(&d, "decision.remote_dispose", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "the confirmation does not match") != NULL,
                "a wrong confirmation was not refused with its frozen sentence: %s",
                atlas_buf_cstr(&resp));

    char confirm[16];
    (void)snprintf(confirm, sizeof(confirm), "%.8s", atlas_buf_cstr(&hash));
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"intent\":\"reject\","
                   "\"token\":\"%s\",\"challenge\":\"%s\",\"confirmation\":\"%s\"}",
                   atlas_buf_cstr(&d_confirm), e.dispose_token, atlas_buf_cstr(&token), confirm);
    call(&d, "decision.remote_dispose", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") != NULL,
                "the correct confirmation, after a wrong one, still failed: %s",
                atlas_buf_cstr(&resp));

    atlas_buf_reset(&resp);
    call(&d, "decision.remote_dispose", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "already been used") != NULL,
                "a replayed challenge was not refused: %s", atlas_buf_cstr(&resp));

    atlas_buf_free(&token);
    atlas_buf_free(&hash);
    atlas_buf_free(&resp);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&ppath);
    atlas_buf_free(&d_rev0);
    atlas_buf_free(&d_stale);
    atlas_buf_free(&d_policy);
    atlas_buf_free(&d_supersede);
    atlas_buf_free(&d_revalidate);
    atlas_buf_free(&d_confirm);
    env_close(&e);
}

/* --- the fourth condition: a MALFORMED policy never offers the group -------- */

static void test_f_disposal_offered_requires_state_enabled(void) {
    /* A policy MALFORMED for a reason that has nothing to do with the three
     * disposal fields still parses every one of them before it reaches the
     * unrelated refusal -- `atlas_gwpolicy_parse_buffer`'s one convention,
     * applied to every key it recognises. Input that would make this test
     * fail if `atlas_server_remote_disposal_offered`'s fourth condition
     * (`gw->state == ATLAS_GWPOLICY_ENABLED`) were removed: this exact
     * policy text, which satisfies every other condition -- the gateway
     * peer, a named disposal key, and an accepted cleartext channel -- while
     * never reaching `state = ENABLED` because of one unrelated unrecognised
     * key. */
    long long uid = (long long)getuid();
    char text[1024];
    (void)snprintf(text, sizeof(text),
                   "enabled = yes\n"
                   "gateway_uid = %lld\n"
                   "remote_mcp = yes\n"
                   "web_gui = yes\n"
                   "tls_mode = NONE\n"
                   "remote_dispose_key = key_0123456789abcdef\n"
                   "remote_dispose_kinds = OPERATIONAL_FACT\n"
                   "operator_accepts_cleartext_disposal = yes\n"
                   "this_key_does_not_exist = 1\n",
                   uid);

    atlas_gwpolicy p;
    atlas_gwpolicy_parse_buffer(text, strlen(text), &p);
    T_REQUIRE_MSG(p.state != ATLAS_GWPOLICY_ENABLED,
                 "the fixture policy was supposed to be malformed for an unrelated reason");
    T_CHECK_MSG(p.remote_dispose_key[0] != '\0',
                "the loader's own convention did not hold: the disposal key was cleared on "
                "refusal, so this test proves nothing");
    T_CHECK_MSG(p.cleartext_disposal_accepted,
                "the loader's own convention did not hold: the acceptance flag was cleared on "
                "refusal, so this test proves nothing");

    atlas_server_ctx sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.gwpolicy = p;

    T_CHECK_MSG(!atlas_server_remote_disposal_offered(&sctx, uid),
                "a policy the loader refused for an unrelated reason still offered the remote "
                "disposal group");
    T_CHECK_MSG(!atlas_server_remote_disposal_policy_ready(&sctx.gwpolicy),
                "the policy-only half of the predicate agreed to offer a refused policy");

    /* Control: the identical fields, actually accepted, do offer the group --
     * so the negative result above is the state check doing its job, not a
     * mistake elsewhere in the fixture policy text. */
    char text2[1024];
    (void)snprintf(text2, sizeof(text2),
                   "enabled = yes\n"
                   "gateway_uid = %lld\n"
                   "remote_mcp = yes\n"
                   "web_gui = yes\n"
                   "tls_mode = NONE\n"
                   "remote_dispose_key = key_0123456789abcdef\n"
                   "remote_dispose_kinds = OPERATIONAL_FACT\n"
                   "operator_accepts_cleartext_disposal = yes\n",
                   uid);
    atlas_gwpolicy p2;
    atlas_gwpolicy_parse_buffer(text2, strlen(text2), &p2);
    T_REQUIRE_MSG(p2.state == ATLAS_GWPOLICY_ENABLED, "the control policy should have parsed");
    atlas_server_ctx sctx2;
    memset(&sctx2, 0, sizeof(sctx2));
    sctx2.gwpolicy = p2;
    T_CHECK_MSG(atlas_server_remote_disposal_offered(&sctx2, uid),
                "the control policy did not offer the group");
}

/* --- (g): the HTTP happy path, and what it leaves in the audit trail -------- */

static void test_g_http_happy_path_and_audit(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf fact_uid = ATLAS_BUF_INIT;
    propose_kind(&e, "OPERATIONAL_FACT", "an http-disposed endpoint", &fact_uid);

    char before_digest[65];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before_digest, &err), &err);

    atlas_buf ptext = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ptext, &err,
                           "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                           "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                           "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n",
                           (long long)getuid(), e.dispose_id),
         &err);
    atlas_buf ppath = ATLAS_BUF_INIT;
    write_policy(&e, "g.conf", atlas_buf_cstr(&ptext), &ppath);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);
    atlas_buf_free(&ptext);

    char bearer[128];
    (void)snprintf(bearer, sizeof(bearer), "Bearer %s", e.dispose_token);

    char body[512];
    (void)snprintf(body, sizeof(body), "repo=proj&decision=%s&revision=1&intent=reject",
                   atlas_buf_cstr(&fact_uid));
    atlas_buf resp = ATLAS_BUF_INIT;
    post_form(g, "/api/v1/decision/challenge", bearer, body, &resp);
    T_CHECK_MSG(status_of(&resp) == 200, "the HTTP challenge did not answer 200: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"confirm\"") == NULL,
               "the HTTP mint response carried a \"confirm\" key: %s", body_of(&resp));

    atlas_buf token = ATLAS_BUF_INIT, hash = ATLAS_BUF_INIT, key_id = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_field(&resp, "token", &token), "no token: %s", body_of(&resp));
    T_REQUIRE_MSG(get_field(&resp, "content_hash", &hash), "no content_hash: %s", body_of(&resp));
    T_REQUIRE_MSG(get_field(&resp, "key_id", &key_id), "no key_id: %s", body_of(&resp));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&key_id), e.dispose_id) == 0, "wrong key_id: %s",
               atlas_buf_cstr(&key_id));

    char confirm[16];
    (void)snprintf(confirm, sizeof(confirm), "%.8s", atlas_buf_cstr(&hash));

    char body2[512];
    (void)snprintf(body2, sizeof(body2),
                   "repo=proj&decision=%s&intent=reject&challenge=%s&confirmation=%s",
                   atlas_buf_cstr(&fact_uid), atlas_buf_cstr(&token), confirm);
    atlas_buf_reset(&resp);
    post_form(g, "/api/v1/decision/dispose", bearer, body2, &resp);
    T_CHECK_MSG(status_of(&resp) == 200, "the HTTP dispose did not answer 200: %d %s",
               status_of(&resp), body_of(&resp));

    atlas_buf actor = ATLAS_BUF_INIT, out_key = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_field(&resp, "actor", &actor), "no actor: %s", body_of(&resp));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&actor), "REMOTE_OPERATOR_CONFIRMED") == 0, "wrong actor: %s",
               atlas_buf_cstr(&actor));
    T_REQUIRE_MSG(get_field(&resp, "key_id", &out_key), "no key_id on the dispose response: %s",
                 body_of(&resp));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&out_key), e.dispose_id) == 0, "wrong key_id: %s",
               atlas_buf_cstr(&out_key));

    /* Two `gw_audit` rows, one per path, naming the credential and the
     * `WEB_API` interface -- and nowhere in the trail does the bearer token
     * or the typed confirmation appear.
     *
     * `audit()` (`gateway.c`) is fire-and-forget by its own contract: the
     * daemon's writer thread queues the row and `gateway.audit` answers once
     * it is queued, not once it is committed. So this polls for the
     * observable outcome rather than guessing at a sleep -- the same
     * discipline `test_the_audit_trail_records_what_happened`
     * (`test_gw_remote.c`) uses for the identical race. */
    atlas_buf aresp = ATLAS_BUF_INIT;
    bool have_challenge = false, have_dispose = false;
    for (int attempt = 0; attempt < 200; attempt++) {
        atlas_buf_reset(&aresp);
        call(&d, "gateway.audit_list", "{}", &aresp);
        have_challenge =
            strstr(atlas_buf_cstr(&aresp), "\"operation\":\"/api/v1/decision/challenge\"") != NULL;
        have_dispose =
            strstr(atlas_buf_cstr(&aresp), "\"operation\":\"/api/v1/decision/dispose\"") != NULL;
        if (have_challenge && have_dispose) {
            break;
        }
        struct timespec ts = {0, 50 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    T_CHECK_MSG(have_challenge, "no audit row for the challenge route: %s",
               atlas_buf_cstr(&aresp));
    T_CHECK_MSG(have_dispose, "no audit row for the dispose route: %s", atlas_buf_cstr(&aresp));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), "\"interface\":\"WEB_API\"") != NULL,
               "no WEB_API audit row: %s", atlas_buf_cstr(&aresp));
    {
        char needle[64];
        (void)snprintf(needle, sizeof(needle), "\"key_id\":\"%s\"", e.dispose_id);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), needle) != NULL,
                   "no audit row names the dispose credential: %s", atlas_buf_cstr(&aresp));
    }
    T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), e.dispose_token) == NULL,
               "the audit trail carries the bearer token");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), atlas_buf_cstr(&token)) == NULL,
               "the audit trail carries the challenge capability");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&aresp), confirm) == NULL,
               "the audit trail carries the typed confirmation");

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_gateway_close(g);

    char after_digest[65];
    T_OK(fx_tree_digest(fx_repo(&e.fx), after_digest, &err), &err);
    T_CHECK_MSG(strcmp(before_digest, after_digest) == 0,
               "the repository tree changed during an HTTP remote disposal");

    atlas_buf_free(&aresp);
    atlas_buf_free(&out_key);
    atlas_buf_free(&actor);
    atlas_buf_free(&token);
    atlas_buf_free(&hash);
    atlas_buf_free(&key_id);
    atlas_buf_free(&resp);
    atlas_buf_free(&ppath);
    atlas_buf_free(&fact_uid);
    env_close(&e);
}

/* --- (h): every refusal, with its frozen sentence and status --------------- */

static void test_h_http_refusals(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf refusal_uid = ATLAS_BUF_INIT, drop_uid = ATLAS_BUF_INIT;
    propose_kind(&e, "OPERATIONAL_FACT", "a refusal target", &refusal_uid);
    propose_kind(&e, "OPERATIONAL_FACT", "a dropped-parameter target", &drop_uid);

    /* `web_gui_anonymous_scopes = repo:read` -- a real, valid anonymous grant
     * that has nothing to do with disposal -- so the "no header" case below
     * is also, at the same time, the "anonymous floor plus a matching Host"
     * case: this policy's floor and this request's Host
     * (`http_request2`'s default, `127.0.0.1:8787`, is this policy's own
     * default `listen_addr`/`listen_port`) both apply to it, and
     * `api_handle_write` still answers 401 because it never asks
     * `anonymous_ok` at all. */
    atlas_buf ptext = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ptext, &err,
                           "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                           "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                           "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n"
                           "web_gui_anonymous_scopes = repo:read\n",
                           (long long)getuid(), e.dispose_id),
         &err);
    atlas_buf ppath = ATLAS_BUF_INIT;
    write_policy(&e, "h.conf", atlas_buf_cstr(&ptext), &ppath);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);

    char dispose_bearer[128], reader_bearer[128];
    (void)snprintf(dispose_bearer, sizeof(dispose_bearer), "Bearer %s", e.dispose_token);
    (void)snprintf(reader_bearer, sizeof(reader_bearer), "Bearer %s", e.reader_token);

    char valid_body[512];
    (void)snprintf(valid_body, sizeof(valid_body), "repo=proj&decision=%s&revision=1&intent=reject",
                   atlas_buf_cstr(&refusal_uid));

    atlas_buf resp = ATLAS_BUF_INIT;

    /* GET -> 405. */
    http_request2(g, "GET", "/api/v1/decision/challenge", dispose_bearer, NULL, NULL, NULL, NULL,
                 (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 405, "GET did not answer 405: %d %s", status_of(&resp),
               body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "this endpoint takes a POST") != NULL,
               "405 did not carry its frozen sentence: %s", body_of(&resp));

    /* Content-Type: application/json -> 415. */
    atlas_buf_reset(&resp);
    http_request2(g, "POST", "/api/v1/decision/challenge", dispose_bearer, NULL, NULL,
                 "application/json", "{}", (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 415, "a JSON body did not answer 415: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(
        strstr(body_of(&resp), "a disposal request is application/x-www-form-urlencoded") != NULL,
        "415 did not carry its frozen sentence: %s", body_of(&resp));

    /* A 4097-byte body -> 413. */
    {
        char *big = malloc(4098);
        T_REQUIRE(big != NULL);
        memset(big, 'a', 4097);
        big[4097] = '\0';
        atlas_buf_reset(&resp);
        post_form(g, "/api/v1/decision/challenge", dispose_bearer, big, &resp);
        free(big);
    }
    T_CHECK_MSG(status_of(&resp) == 413, "a 4097-byte body did not answer 413: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "the request body exceeds the gateway limit") != NULL,
               "413 did not carry its frozen sentence: %s", body_of(&resp));

    /* A gateway whose policy has no keys -> 404. */
    {
        atlas_buf ptext2 = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext2, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = NONE\n",
                               (long long)getuid()),
             &err);
        atlas_buf ppath2 = ATLAS_BUF_INIT;
        write_policy(&e, "h-keyless.conf", atlas_buf_cstr(&ptext2), &ppath2);

        fx_daemon d2;
        fx_daemon_init(&d2);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath2), &d2, &err), &err);
        T_OK(fx_daemon_wait_ready(&d2, 15000, &err), &err);
        atlas_gateway *g2 = NULL;
        open_http_gateway(atlas_buf_cstr(&ptext2), &d2, &g2);

        atlas_buf_reset(&resp);
        post_form(g2, "/api/v1/decision/challenge", dispose_bearer, valid_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 404, "a keyless policy did not answer 404: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "this gateway does not serve remote disposal") != NULL,
                   "404 did not carry its frozen sentence: %s", body_of(&resp));

        atlas_gateway_close(g2);
        fx_daemon_stop(&d2, false);
        fx_daemon_free(&d2);
        atlas_buf_free(&ptext2);
        atlas_buf_free(&ppath2);
    }

    /* No Authorization header at all -- also the anonymous-floor-plus-
     * matching-Host case, per this test's own policy comment above. */
    atlas_buf_reset(&resp);
    post_form(g, "/api/v1/decision/challenge", NULL, valid_body, &resp);
    T_CHECK_MSG(status_of(&resp) == 401, "no Authorization header did not answer 401: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp),
                       "a session cookie or the anonymous floor cannot dispose") != NULL,
               "401 did not carry its frozen sentence: %s", body_of(&resp));

    /* No Authorization header, but a *valid* session cookie from a real login
     * with the reader key -- the cookie must not help. */
    {
        atlas_buf lresp = ATLAS_BUF_INIT;
        char login_body[256];
        (void)snprintf(login_body, sizeof(login_body), "{\"key\":\"%s\"}", e.reader_token);
        http_request2(g, "POST", "/auth/login", NULL, NULL, NULL, "application/json", login_body,
                     (size_t)-1, &lresp);
        T_REQUIRE_MSG(status_of(&lresp) == 200, "the reader login did not succeed: %d %s",
                     status_of(&lresp), body_of(&lresp));
        char cookie[128];
        T_REQUIRE_MSG(extract_session_cookie(&lresp, cookie, sizeof(cookie)),
                     "the login response carried no session cookie: %s", atlas_buf_cstr(&lresp));
        atlas_buf_free(&lresp);

        atlas_buf_reset(&resp);
        http_request2(g, "POST", "/api/v1/decision/challenge", NULL, cookie, NULL, NULL,
                     valid_body, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 401,
                   "a valid session cookie with no bearer still answered %d: %s",
                   status_of(&resp), body_of(&resp));
    }

    /* The reader key as bearer -> 403 with the scope sentence. */
    atlas_buf_reset(&resp);
    post_form(g, "/api/v1/decision/challenge", reader_bearer, valid_body, &resp);
    T_CHECK_MSG(status_of(&resp) == 403, "the reader key did not answer 403: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "decisions:dispose") != NULL,
               "403 did not name the missing scope: %s", body_of(&resp));

    /* A body naming an undeclared parameter (`token`, `key_id`) -- dropped,
     * never forwarded: the request still succeeds exactly as it would
     * without them, because the gateway's own bearer is what is actually
     * forwarded as `token`. */
    {
        char drop_body[512];
        (void)snprintf(drop_body, sizeof(drop_body),
                       "repo=proj&decision=%s&revision=1&intent=reject&token=forged&"
                       "key_id=forged",
                       atlas_buf_cstr(&drop_uid));
        atlas_buf_reset(&resp);
        post_form(g, "/api/v1/decision/challenge", dispose_bearer, drop_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 200,
                   "a body naming undeclared parameters did not still succeed: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "forged") == NULL,
                   "a forged body parameter reached the response: %s", body_of(&resp));
    }

    /* A malformed percent-escape -> 400. */
    atlas_buf_reset(&resp);
    post_form(g, "/api/v1/decision/challenge", dispose_bearer,
             "repo=proj&decision=%zz&revision=1&intent=reject", &resp);
    T_CHECK_MSG(status_of(&resp) == 400, "a malformed percent-escape did not answer 400: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "a body parameter is malformed") != NULL,
               "400 did not carry its frozen sentence: %s", body_of(&resp));

    /* A raw NUL byte inside the body -> 400. Without the explicit check,
     * `build_api_params`'s `strlen`-based read would see only
     * `repo=proj&decision=x` -- still 400, but for a *different* reason
     * (`"decision" is not a decision id`, the daemon's own refusal of a
     * malformed uid): the discriminator is the sentence, not the status. */
    {
        char nulbody[64];
        size_t n = 0;
        memcpy(nulbody + n, "repo=proj&decision=x", 20);
        n += 20;
        nulbody[n++] = '\0';
        memcpy(nulbody + n, "&revision=1&intent=reject", 25);
        n += 25;
        atlas_buf_reset(&resp);
        http_request2(g, "POST", "/api/v1/decision/challenge", dispose_bearer, NULL, NULL, NULL,
                     nulbody, n, &resp);
        T_CHECK_MSG(status_of(&resp) == 400, "a body with an embedded NUL did not answer 400: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "a body parameter is malformed") != NULL,
                   "400 did not carry its frozen sentence: %s", body_of(&resp));
    }

    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&resp);
    atlas_buf_free(&ptext);
    atlas_buf_free(&ppath);
    atlas_buf_free(&refusal_uid);
    atlas_buf_free(&drop_uid);
    env_close(&e);
}

/* --- (c): the write path's own CORS treatment ------------------------------- */

static void test_i_cors_preflight_on_the_write_path(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf ptext = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ptext, &err,
                           "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                           "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                           "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n"
                           "allowed_origin = https://good.example\n",
                           (long long)getuid(), e.dispose_id),
         &err);
    atlas_buf ppath = ATLAS_BUF_INIT;
    write_policy(&e, "i.conf", atlas_buf_cstr(&ptext), &ppath);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);
    atlas_buf_free(&ptext);

    /* An unlisted origin's preflight -> 403, `gateway.c:1351-1366`'s existing,
     * unchanged code -- this is the shared OPTIONS handler asked once for a
     * write path, not a second implementation. */
    atlas_buf resp = ATLAS_BUF_INIT;
    http_request2(g, "OPTIONS", "/api/v1/decision/dispose", NULL, NULL,
                 "https://evil.example", NULL, NULL, (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 403, "an unlisted origin's preflight did not answer 403: %d %s",
               status_of(&resp), body_of(&resp));

    /* A listed origin's preflight succeeds. */
    atlas_buf_reset(&resp);
    http_request2(g, "OPTIONS", "/api/v1/decision/dispose", NULL, NULL,
                 "https://good.example", NULL, NULL, (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 204, "a listed origin's preflight did not answer 204: %d %s",
               status_of(&resp), body_of(&resp));

    /* And the real request, from that same listed origin, still needs the
     * bearer -- no `Origin` check was added to the write handler, on
     * purpose: the CSRF defence is the bearer header, not `Origin`. */
    atlas_buf_reset(&resp);
    http_request2(g, "POST", "/api/v1/decision/dispose", NULL, NULL, "https://good.example",
                 "application/x-www-form-urlencoded", "repo=proj&decision=x&intent=reject",
                 (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 401,
               "a listed origin with no bearer still answered %d: %s", status_of(&resp),
               body_of(&resp));

    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&resp);
    atlas_buf_free(&ppath);
    env_close(&e);
}

/* --- (d): /auth/me's remote_disposal and cleartext_disposal ----------------- */

static void test_j_auth_me_fields(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Instance 1: a keyed policy, TLS in front, no cleartext acceptance --
     * remote_disposal true, cleartext_disposal false. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                               "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n",
                               (long long)getuid(), e.dispose_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "j1.conf", atlas_buf_cstr(&ptext), &ppath);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);
        atlas_gateway *g = NULL;
        open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);

        char cookie[128];
        login_and_get_cookie(g, e.reader_token, cookie, sizeof(cookie));

        atlas_buf resp = ATLAS_BUF_INIT;
        http_request2(g, "GET", "/auth/me", NULL, cookie, NULL, NULL, NULL, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 200, "/auth/me did not answer 200: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"remote_disposal\":true") != NULL,
                   "remote_disposal was not true under a keyed policy: %s", body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"cleartext_disposal\":false") != NULL,
                   "cleartext_disposal was not false under a REVERSE_PROXY policy: %s",
                   body_of(&resp));

        atlas_gateway_close(g);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&resp);
        atlas_buf_free(&ptext);
        atlas_buf_free(&ppath);
    }

    /* Instance 2: no disposal keys at all -- "gui_env" -- remote_disposal
     * false. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = REVERSE_PROXY\n",
                               (long long)getuid()),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "j2.conf", atlas_buf_cstr(&ptext), &ppath);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);
        atlas_gateway *g = NULL;
        open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);

        char cookie[128];
        login_and_get_cookie(g, e.reader_token, cookie, sizeof(cookie));

        atlas_buf resp = ATLAS_BUF_INIT;
        http_request2(g, "GET", "/auth/me", NULL, cookie, NULL, NULL, NULL, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 200, "/auth/me did not answer 200: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"remote_disposal\":false") != NULL,
                   "remote_disposal was not false under a keyless policy: %s", body_of(&resp));

        atlas_gateway_close(g);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&resp);
        atlas_buf_free(&ptext);
        atlas_buf_free(&ppath);
    }

    /* Instance 3: a keyed policy carrying the cleartext acceptance --
     * cleartext_disposal true. */
    {
        atlas_buf ptext = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&ptext, &err,
                               "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                               "web_gui = yes\ntls_mode = NONE\nremote_dispose_key = key_%s\n"
                               "remote_dispose_kinds = OPERATIONAL_FACT\n"
                               "operator_accepts_cleartext_disposal = yes\n",
                               (long long)getuid(), e.dispose_id),
             &err);
        atlas_buf ppath = ATLAS_BUF_INIT;
        write_policy(&e, "j3.conf", atlas_buf_cstr(&ptext), &ppath);

        fx_daemon d;
        fx_daemon_init(&d);
        T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
        T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);
        atlas_gateway *g = NULL;
        open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);

        char cookie[128];
        login_and_get_cookie(g, e.reader_token, cookie, sizeof(cookie));

        atlas_buf resp = ATLAS_BUF_INIT;
        http_request2(g, "GET", "/auth/me", NULL, cookie, NULL, NULL, NULL, (size_t)-1, &resp);
        T_CHECK_MSG(status_of(&resp) == 200, "/auth/me did not answer 200: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "\"cleartext_disposal\":true") != NULL,
                   "cleartext_disposal was not true under an accepted-cleartext policy: %s",
                   body_of(&resp));

        atlas_gateway_close(g);
        fx_daemon_stop(&d, false);
        fx_daemon_free(&d);
        atlas_buf_free(&resp);
        atlas_buf_free(&ptext);
        atlas_buf_free(&ppath);
    }

    env_close(&e);
}

/* --- (e): the disposal key over /mcp reaches no write tool ------------------ */

static void test_k_disposal_key_over_mcp_reaches_no_write_tool(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf ptext = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&ptext, &err,
                           "enabled = yes\ngateway_uid = %lld\nremote_mcp = yes\n"
                           "web_gui = yes\ntls_mode = REVERSE_PROXY\nremote_dispose_key = "
                           "key_%s\nremote_dispose_kinds = OPERATIONAL_FACT\n",
                           (long long)getuid(), e.dispose_id),
         &err);
    atlas_buf ppath = ATLAS_BUF_INIT;
    write_policy(&e, "k.conf", atlas_buf_cstr(&ptext), &ppath);

    fx_daemon d;
    fx_daemon_init(&d);
    T_OK(gwd_start(&e, atlas_buf_cstr(&ppath), &d, &err), &err);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(atlas_buf_cstr(&ptext), &d, &g);
    atlas_buf_free(&ptext);

    char bearer[128];
    (void)snprintf(bearer, sizeof(bearer), "Bearer %s", e.dispose_token);

    atlas_buf resp = ATLAS_BUF_INIT;
    http_request2(g, "POST", "/mcp", bearer, NULL, NULL, "application/json",
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}",
                 (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 200, "tools/list did not answer 200: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"tools\":[]") != NULL,
               "the disposal key's tool listing is not empty: %s", body_of(&resp));

    /* `test_the_gateway_holds_no_credential_administration_verb`'s shape:
     * every name a decision-lifecycle tool would plausibly have, none of
     * which exists for this credential either -- `decisions:dispose` maps to
     * no tool at all. */
    static const char *const NAMES[] = {
        "atlas_decision_approve", "atlas_approve_decision", "atlas_decision_dispose",
        "atlas_dispose",         "atlas_review_apply",     "atlas_decision_reject",
        "atlas_decision_resolve", "atlas_remote_dispose",   "decision.remote_dispose",
        "decision.remote_challenge",
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        char msg[512];
        (void)snprintf(msg, sizeof(msg),
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
                       "{\"name\":\"%s\",\"arguments\":{}}}",
                       NAMES[i]);
        atlas_buf_reset(&resp);
        http_request2(g, "POST", "/mcp", bearer, NULL, NULL, "application/json", msg, (size_t)-1,
                     &resp);
        T_CHECK_MSG(status_of(&resp) == 200, "%s call did not answer 200: %d %s", NAMES[i],
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "unknown tool") != NULL,
                   "\"%s\" was not answered as an unknown tool: %s", NAMES[i], body_of(&resp));
    }

    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&resp);
    atlas_buf_free(&ppath);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"gateway.auth derives decisions:dispose for exactly the named credential",
     test_a_gateway_auth_scope_derivation},
    {"the full remote disposal happy path, against a real daemon",
     test_b_remote_dispose_happy_path},
    {"a plain daemon with no gateway policy offers neither name",
     test_c_hidden_from_a_plain_daemon},
    {"the policy gate: malformed, keyless, and accepted-cleartext", test_d_policy_gate},
    {"the write point's frozen refusals, reached through the daemon", test_e_refusals},
    {"a policy MALFORMED for an unrelated reason never offers the group",
     test_f_disposal_offered_requires_state_enabled},
    {"the HTTP happy path, and what it leaves in the audit trail",
     test_g_http_happy_path_and_audit},
    {"every HTTP refusal, with its frozen sentence and status", test_h_http_refusals},
    {"the write path's own CORS treatment", test_i_cors_preflight_on_the_write_path},
    {"/auth/me's remote_disposal and cleartext_disposal", test_j_auth_me_fields},
    {"the disposal key over /mcp reaches no write tool",
     test_k_disposal_key_over_mcp_reaches_no_write_tool},
};

ATLAS_TEST_MAIN("gw_dispose", TESTS)

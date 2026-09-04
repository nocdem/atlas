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
#include <unistd.h>

#include "atlas/apikey.h"
#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"
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

    env_close(&e);
}

/* --- (b): the full remote-disposal happy path -------------------------------- */

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
     * response, not the CLI's `--json` rendering: `render_json.c` has not
     * been taught this field yet (a later task's job; the wire format this
     * season's write point actually produces is what this test is
     * responsible for), and `on_event` in server_decision.c is. */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params), "{\"repo\":\"proj\",\"decision\":\"%s\"}",
                   atlas_buf_cstr(&fact_uid));
    call(&d, "decision.history", params, &resp);
    char needle[80];
    (void)snprintf(needle, sizeof(needle), "\"key_id\":\"%s\"", e.dispose_id);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), needle) != NULL,
                "decision.history does not carry the credential id: %s", atlas_buf_cstr(&resp));

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);

    atlas_buf doctor = ATLAS_BUF_INIT;
    int code = 0;
    const char *doc[] = {"doctor"};
    run_atlas(&e, doc, 1u, &doctor, &code);
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

    /* a non-newest revision */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,\"intent\":\"reject\","
                   "\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_stale), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "is minted only for the newest revision") != NULL,
                "a non-newest revision was not refused with its frozen sentence: %s",
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
     * browser offers no supersede action. `op_challenge` checks
     * `replacement_uid.len > 0` first and, failing that, an
     * intent-is-SUPERSEDE-with-no-replacement case second -- both ahead of
     * the REMOTE-and-(SUPERSEDE|REVALIDATE) check below. So for SUPERSEDE
     * specifically, the earlier "no replacement named" refusal (the
     * ordinary one every SUPERSEDE intent gets, local or remote) always
     * fires first, and the REMOTE-specific sentence for this one intent is
     * unreachable through this endpoint by construction. It is still
     * exercised directly (bypassing both endpoints) by
     * `test_decision_remote.c`'s `test_g_supersede_and_revalidate_refused_
     * remotely`. The property this sub-case is for still holds: a browser
     * cannot mint a working supersede capability, one way or another. */
    atlas_buf_reset(&resp);
    (void)snprintf(params, sizeof(params),
                   "{\"repo\":\"proj\",\"decision\":\"%s\",\"revision\":1,"
                   "\"intent\":\"supersede\",\"token\":\"%s\"}",
                   atlas_buf_cstr(&d_supersede), e.dispose_token);
    call(&d, "decision.remote_challenge", params, &resp);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "needs the decision that replaces this one") !=
                   NULL,
                "a supersede intent with no replacement was not refused as expected: %s",
                atlas_buf_cstr(&resp));

    /* a revalidate intent, unlike supersede, has no earlier "no replacement
     * named" gate ahead of it in `op_challenge` -- `replacement_uid.len > 0`
     * is false and `c.intent == SUPERSEDE` is false, so REVALIDATE falls
     * through both and reaches the REMOTE-and-(SUPERSEDE|REVALIDATE) check
     * directly. This is the one real path by which the browser-facing
     * endpoint reaches that frozen sentence. */
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
};

ATLAS_TEST_MAIN("gw_dispose", TESTS)

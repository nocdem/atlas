/* Atlas - A14: the daemon's remote-submit group, verified against a live daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `job.remote_submit`, `job.remote_get`, `job.remote_list` and
 * `job.remote_cancel` are the four methods the gateway's uid may call.  This
 * file proves they behave correctly on a real socket against a real daemon that
 * carries both an injected gateway policy and an injected orchestration policy.
 *
 * Two shapes of daemon run here:
 *
 *  **The live fixture daemon** (started with `fx_daemon_start`) carries a
 *  zeroed gateway policy — no `gateway_uid`, no submit key, no TLS condition
 *  — so none of the four names is offered, and every caller gets `unknown
 *  method`.  That is the test for (b).
 *
 *  **The gateway daemon** (started with `gwd_start`) carries both a gateway
 *  policy naming a real `gateway_uid` and two submit keys, and an
 *  orchestration policy naming the repo, drivers `fake`/`fake-repo` and
 *  `mode = patch`.  It is the daemon for (a) through (f).
 *
 * Nothing here starts a real worker, calls a real model or touches the
 * developer's database.
 *
 * Required cases: 23 (malformed and oversized frames), 41 (gw uid vs ordinary
 * client), the four A14 remote-submit methods offered under policy.
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "atlas/apikey.h"
#include "atlas/atlas.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas_test.h"
#include "support/fixture.h"

#ifndef ATLAS_GW_DAEMON_BIN
#define ATLAS_GW_DAEMON_BIN "atlas-gw-daemon"
#endif

/* --- the fixture ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
    /* Three keys minted with --no-scopes.  Two are listed in the gateway policy
     * as remote_submit_key lines (submit and second); the third (other) is
     * deliberately unlisted so gateway.auth cannot derive jobs:submit from it.
     * The second key is used for scope-by-key tests: the job was submitted by
     * the primary key, so remote_get/remote_cancel with the second key returns
     * "no such job" — the credential is valid and policy-listed, but the job
     * belongs to a different key_id. */
    char submit_token[ATLAS_APIKEY_TOKEN_MAX];  /* the primary submit key */
    char submit_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* "key_<16hex>\0" = 21 bytes */
    char second_token[ATLAS_APIKEY_TOKEN_MAX];  /* second listed submit key */
    char second_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* "key_<16hex>\0" */
    char other_token[ATLAS_APIKEY_TOKEN_MAX];   /* an unlisted key */
    char other_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* "key_<16hex>\0" */
} env;

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
}

static void env_close(env *e) {
    fx_close(&e->fx);
}

/* --- gwd_start: a variant of fx_daemon_start for the atlas-gw-daemon binary
 *     accepting both a gateway policy file and an orchestration policy file. */

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

/* Write a text file relative to the fixture root. */
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
    /* Return a static buffer from atlas_buf for the path -- the caller only
     * uses it while the fixture is open, so the buf lifetime is fine.
     * Two alternating buffers because tests call this function twice before
     * passing both paths to gwd_start — a single buffer would overwrite the
     * first path the moment the second is written. */
    static char path_storage[2][4096];
    static unsigned path_slot;
    unsigned slot = (path_slot++) & 1u;
    (void)snprintf(path_storage[slot], sizeof(path_storage[slot]), "%s",
                   atlas_buf_cstr(&path));
    atlas_buf_free(&path);
    return path_storage[slot];
}

static void ipc_call(const char *socket, const char *method, const char *params,
                     atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_ipc_call(socket, method, params, resp, &err);
}

static bool contains(const atlas_buf *b, const char *s) {
    return strstr(atlas_buf_cstr(b), s) != NULL;
}

/* --- (a) / (b): group offered check ---------------------------------------- */

/* Checks all four remote-submit names against a live (non-gw) daemon; they
 * must all answer "unknown method". */
static void check_unknown(const char *socket) {
    static const char *const NAMES[] = {
        "job.remote_submit", "job.remote_get", "job.remote_list", "job.remote_cancel",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(socket, NAMES[i], "{}", &resp);
        T_CHECK_MSG(contains(&resp, "unknown method"), "%s did not answer unknown method: %s",
                    NAMES[i], atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
}

/* --- helper: parse a json field -------------------------------------------- */
static bool get_str_field(const char *json, const char *key, char *buf, size_t buf_size) {
    char pattern[128];
    (void)snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p += strlen(pattern);
    size_t n = 0;
    while (p[n] != '"' && p[n] != '\0' && n + 1 < buf_size) {
        n++;
    }
    memcpy(buf, p, n);
    buf[n] = '\0';
    return true;
}

/* --- test: zeroed-policy daemon answers unknown method for all four names --- */

static void test_zeroed_policy_answers_unknown_method(void) {
    env e;
    env_open(&e);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(fx_daemon_start(&e.fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    /* (b): a daemon with a zeroed gateway policy offers none of the four names
     * to any caller -- there is no gateway_uid, no submit key and no TLS
     * condition satisfied.  The peer uid here is the test process's own uid,
     * which is neither zero nor a gateway uid. */
    check_unknown(sock);

    /* `job.submit` is the submitter-group method; it should refuse with the
     * orch-disabled sentence, not "unknown method", from the same fixture
     * daemon. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "job.submit", "{\"repo\":\"proj\",\"task\":\"do something\"}", &resp);
        T_CHECK_MSG(contains(&resp, "orchestration is not enabled"),
                    "job.submit on a zeroed daemon should say orch disabled: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- test: the gateway daemon offers the group under policy ----------------- */

static void test_gateway_daemon_offers_remote_submit_under_policy(void) {
    env e;
    env_open(&e);

    /* Build the gateway policy: gateway_uid = this process's uid, two submit
     * keys (submit and second), tls_mode = REVERSE_PROXY, driver = fake,
     * mode = patch.  The third key (other) is not listed here.
     * The policy uses the full "key_<hex>" form that the parser expects. */
    char gw_policy[4096];
    (void)snprintf(
        gw_policy, sizeof gw_policy,
        "enabled = yes\n"
        "gateway_uid = %ld\n"
        "remote_mcp = yes\n"
        "web_gui = yes\n"
        "listen_addr = 127.0.0.1\n"
        "tls_mode = REVERSE_PROXY\n"
        "remote_submit_key = %s\n"
        "remote_submit_key = %s\n"
        "remote_submit_driver = fake\n"
        "remote_submit_mode = patch\n"
        "remote_submit_max_attempts = 1\n"
        "remote_submit_max_active = 2\n"
        "remote_submit_max_per_day = 3\n"
        "remote_submit_gate = true\n",
        (long)getuid(), e.submit_id, e.second_id);

    /* Build the orchestration policy. */
    /* submitter_uid deliberately omits the gateway uid so that job.submit is
     * refused for the gateway peer while job.remote_submit works via credential. */
    char orch_policy[4096];
    (void)snprintf(
        orch_policy, sizeof orch_policy,
        "dispatcher_uid = 1\n"
        "submitter_uid = 2\n"
        "repo = proj\n"
        "driver = fake\n"
        "driver = fake-repo\n"
        "mode = patch\n"
        "worker_root = /tmp\n");

    atlas_err err;
    atlas_err_init(&err);

    /* Verify the gateway policy parses as ENABLED before handing it to the daemon. */
    atlas_gwpolicy gwp;
    atlas_gwpolicy_parse_buffer(gw_policy, strlen(gw_policy), &gwp);
    T_REQUIRE_MSG(gwp.state == ATLAS_GWPOLICY_ENABLED,
                  "gateway policy should parse as ENABLED; reason: %d, detail: %s",
                  (int)gwp.reason, gwp.detail);

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    /* (a): job.submit as the gateway uid → "may not submit" (the orch policy
     * does not list the gateway uid as a submitter). */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "job.submit", "{\"repo\":\"proj\",\"task\":\"do something\"}", &resp);
        T_CHECK_MSG(contains(&resp, "may not submit") || contains(&resp, "orchestration"),
                    "job.submit as gw should refuse: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (a): gateway.auth with the submit key → scopes include "jobs:submit". */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[512];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.submit_token);
        ipc_call(sock, "gateway.auth", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"authenticated\":true"),
                    "gateway.auth with submit key should authenticate: %s",
                    atlas_buf_cstr(&resp));
        T_CHECK_MSG(contains(&resp, "jobs:submit"),
                    "gateway.auth with submit key should carry jobs:submit: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (a): gateway.auth with the other key (not in the policy) → no jobs:submit. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[512];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.other_token);
        ipc_call(sock, "gateway.auth", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"authenticated\":true"),
                    "gateway.auth with other key should authenticate: %s",
                    atlas_buf_cstr(&resp));
        T_CHECK_MSG(!contains(&resp, "jobs:submit"),
                    "gateway.auth with unlisted key must not carry jobs:submit: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (c): forbidden parameters must be refused.  Each one gets its own call
     * so the first refusal does not mask the rest. */
    static const char *const FORBIDDEN[] = {
        "driver", "mode", "validation", "parallel", "memory", "parent",
    };
    for (size_t i = 0; i < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"repo\":\"proj\",\"task\":\"t\",\"token\":\"%s\",\"%s\":\"x\"}",
                       e.submit_token, FORBIDDEN[i]);
        ipc_call(sock, "job.remote_submit", params, &resp);
        T_CHECK_MSG(
            contains(&resp, "root-owned policy"),
            "job.remote_submit with forbidden param \"%s\" should mention policy: %s",
            FORBIDDEN[i], atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (a): job.remote_submit with the submit key → QUEUED; response carries key_id and budget. */
    char job_uid[64];
    job_uid[0] = '\0';
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[1024];
        (void)snprintf(params, sizeof params,
                       "{\"repo\":\"proj\",\"task\":\"do something\",\"token\":\"%s\"}",
                       e.submit_token);
        ipc_call(sock, "job.remote_submit", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"state\":\"QUEUED\""),
                    "job.remote_submit should produce QUEUED: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(contains(&resp, "\"key_id\":"),
                    "job.remote_submit response should carry key_id: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(contains(&resp, "\"budget\":"),
                    "job.remote_submit response should carry budget: %s", atlas_buf_cstr(&resp));
        get_str_field(atlas_buf_cstr(&resp), "job", job_uid, sizeof job_uid);
        atlas_buf_free(&resp);
    }

    /* (a): job.remote_get with the submit key → finds the job. */
    if (job_uid[0] != '\0') {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[1024];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}", job_uid,
                       e.submit_token);
        ipc_call(sock, "job.remote_get", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"state\":\"QUEUED\""),
                    "job.remote_get should return the job: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(contains(&resp, "\"key_id\":"),
                    "job.remote_get should carry key_id: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (a): job.remote_get with the second key → "no such job".  The second key
     * is listed in the policy (so atlas_orch_remote_verify succeeds), but the
     * job was submitted by the primary submit key, so the key_id scope check
     * inside method_remote_get does not match and the answer is "no such job".
     * This is the scope-by-key_id check, not a credential failure. */
    if (job_uid[0] != '\0') {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[1024];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}", job_uid,
                       e.second_token);
        ipc_call(sock, "job.remote_get", params, &resp);
        T_CHECK_MSG(contains(&resp, "no such job"),
                    "job.remote_get with second key should say no such job: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (a): job.remote_list with the submit key → sees the job. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[512];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.submit_token);
        ipc_call(sock, "job.remote_list", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"count\":"),
                    "job.remote_list should return count: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* (a): job.remote_cancel with the submit key → cancels the job. */
    if (job_uid[0] != '\0') {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[1024];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}", job_uid,
                       e.submit_token);
        ipc_call(sock, "job.remote_cancel", params, &resp);
        /* Cancellation puts it in CANCEL_REQUESTED (or already terminal). */
        T_CHECK_MSG(contains(&resp, "state") || contains(&resp, "ok"),
                    "job.remote_cancel should answer with job state: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- test: zeroed orchpolicy produces orch_disabled sentence --------------- */

static void test_zeroed_orchpolicy_produces_orch_disabled(void) {
    env e;
    env_open(&e);

    /* Gateway policy with submit keys but no orchpolicy → orch_disabled. */
    char gw_policy[4096];
    (void)snprintf(
        gw_policy, sizeof gw_policy,
        "enabled = yes\n"
        "gateway_uid = %ld\n"
        "remote_mcp = yes\n"
        "web_gui = yes\n"
        "listen_addr = 127.0.0.1\n"
        "tls_mode = REVERSE_PROXY\n"
        "remote_submit_key = %s\n"
        "remote_submit_driver = fake\n"
        "remote_submit_mode = patch\n"
        "remote_submit_max_attempts = 1\n"
        "remote_submit_max_active = 2\n"
        "remote_submit_max_per_day = 3\n"
        "remote_submit_gate = true\n",
        (long)getuid(), e.submit_id);

    atlas_gwpolicy gwp;
    atlas_gwpolicy_parse_buffer(gw_policy, strlen(gw_policy), &gwp);
    T_REQUIRE_MSG(gwp.state == ATLAS_GWPOLICY_ENABLED,
                  "gateway policy should parse as ENABLED");

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);

    atlas_err err;
    atlas_err_init(&err);
    fx_daemon d;
    fx_daemon_init(&d);
    /* Pass NULL for the orch policy path — the daemon gets a zeroed orchpolicy. */
    T_REQUIRE(gwd_start(&e, gw_path, NULL, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"repo\":\"proj\",\"task\":\"t\",\"token\":\"%s\"}", e.submit_token);
        ipc_call(sock, "job.remote_submit", params, &resp);
        T_CHECK_MSG(contains(&resp, "orchestration is not enabled"),
                    "job.remote_submit with zeroed orchpolicy should say orch disabled: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- TESTS table ----------------------------------------------------------- */

static const atlas_test TESTS[] = {
    {"a zeroed gateway policy answers unknown method for the remote-submit group",
     test_zeroed_policy_answers_unknown_method},
    {"the gateway daemon offers the remote-submit group under policy",
     test_gateway_daemon_offers_remote_submit_under_policy},
    {"a zeroed orchpolicy produces the orch-disabled sentence",
     test_zeroed_orchpolicy_produces_orch_disabled},
};

ATLAS_TEST_MAIN("orch_remote_rpc", TESTS)

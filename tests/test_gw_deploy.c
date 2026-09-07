/* Atlas — A17 T3: the six /api/v1/deploy/ routes, against a real
 * daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `tests/test_gw_submit.c`'s own HTTP-gateway shape (`env`'s runtime bits,
 * `gwd_start`, `write_policy_file`, `open_http_gateway`, `http_request2`,
 * `post_form`, `status_of`, `body_of`) and `tests/test_deploy_rpc.c`'s own
 * seeding shape (`submit_op`, `apply_ok`, `worker_op`, `lease_once`,
 * `advance_to_running`, `set_submit_key_id`, `seed_succeeded_job`) are both
 * copied here rather than shared, on both files' own precedent stated in
 * `test_deploy_rpc.c`'s header: two different callers with two different
 * frozen response shapes sharing one helper is one more surface where a
 * future change to either quietly becomes a change to both.
 *
 * What this suite proves that neither of those two already does:
 *   - `deploy.remote_propose` and `deploy.remote_confirm` reach the socket
 *     through `/api/v1/deploy/propose` and `/api/v1/deploy/confirm`;
 *   - the submit bearer is refused (403) at `/api/v1/deploy/confirm`, and the
 *     deploy bearer with the correct eight-hex prefix is accepted (2xx);
 *   - a JSON Content-Type body is refused (415) before `build_api_params`
 *     runs at all, mirroring `tests/test_gw_submit.c`'s own "JSON content
 *     type -> 415" case -- and, separately, a *well-formed* form-encoded
 *     body naming a field outside the route's declared params ("patch",
 *     "dry_run") is dropped rather than forwarded: `tests/test_gw_submit.c`'s
 *     own "driver= in body is silently dropped" pattern, proved positively
 *     by a 2xx the daemon could not have answered had either field arrived;
 *   - `/api/v1/deploy/get` and `/api/v1/deploy/list` are reachable by either
 *     the submit bearer or the deploy bearer -- the one route property this
 *     season adds beside every other route's single scope
 *     (`route_scope_granted`, `src/gw/gateway.c`);
 *   - `cursor` and `limit` arrive at the daemon as integers, not strings: a
 *     second page differs from the first.
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

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
#include "atlas/deploy.h"
#include "atlas/gateway.h"
#include "atlas/gw.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/orchpolicy.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

#ifndef ATLAS_GW_DAEMON_BIN
#define ATLAS_GW_DAEMON_BIN "atlas-gw-daemon"
#endif

/* --- the fixture ------------------------------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_buf identity;
    atlas_buf commit;
    char submit_token[ATLAS_APIKEY_TOKEN_MAX];
    char submit_id[ATLAS_APIKEY_SELECTOR_HEX + 6u]; /* "key_<16hex>\0" */
    char deploy_token[ATLAS_APIKEY_TOKEN_MAX];
    char deploy_id[ATLAS_APIKEY_SELECTOR_HEX + 6u];
    /* Named in neither `remote_submit_key` nor `remote_deploy_key` -- the
       negative half of the dual-scope property: `gateway.auth` derives
       neither `jobs:submit` nor `deploys:confirm` for this key, so
       `route_scope_granted`'s alt-scope table must refuse it on
       `/api/v1/deploy/get` and `/api/v1/deploy/list` exactly as it would any
       other unlisted credential. */
    char other_token[ATLAS_APIKEY_TOKEN_MAX];
    char other_id[ATLAS_APIKEY_SELECTOR_HEX + 6u];
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

static void mint_key(env *e, const char *label, char *token_out, size_t token_size, char *id_out,
                     size_t id_size) {
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
    atlas_buf_init(&e->identity);
    atlas_buf_init(&e->commit);
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
    mint_key(e, "deploy-key", e->deploy_token, sizeof e->deploy_token, e->deploy_id,
             sizeof e->deploy_id);
    mint_key(e, "other-key", e->other_token, sizeof e->other_token, e->other_id,
             sizeof e->other_id);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(db, "proj", &ri, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_db_repo_identity_hash(db, ri.id, &e->identity, &err), &err);
    T_OK(atlas_buf_set_str(&e->commit,
                           ri.scanned_head[0] != '\0'
                               ? ri.scanned_head
                               : "0123456789abcdef0123456789abcdef01234567",
                           &err),
         &err);
    atlas_repo_info_free(&ri);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
}

static void env_close(env *e) {
    atlas_buf_free(&e->identity);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

/* --- seeding a job directly against the fixture's own database file, before
 * any daemon opens it -- test_db_deploy.c's / test_deploy_rpc.c's own shape. */

static atlas_orch_op *submit_op(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = 1;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e->identity.data, e->identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, e->commit.data, e->commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, "add a comment", &err), &err);
    op->spec.wall_timeout_ms = 60000;
    op->spec.idle_timeout_ms = 30000;
    op->spec.max_attempts = 1;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);
    return op;
}

static void apply_ok(atlas_db *db, atlas_orch_op *op, atlas_orch_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result_init(out);
    T_OK(atlas_orch_apply(db, op, out, &err), &err);
    atlas_orch_op_free(op);
    free(op);
}

static atlas_orch_op *worker_op(atlas_orch_op_kind kind, const char *token) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(kind);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->token, token, &err), &err);
    return op;
}

static int64_t lease_once(atlas_db *db, atlas_buf *token_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->dispatcher_id, "d1", &err), &err);
    atlas_orch_result g;
    apply_ok(db, op, &g);
    T_REQUIRE(g.granted);
    if (token_out != NULL) {
        T_OK(atlas_buf_set(token_out, g.token.data, g.token.len, &err), &err);
    }
    int64_t n = g.attempt_no;
    atlas_orch_result_free(&g);
    return n;
}

static void advance_to_running(atlas_db *db, const char *token) {
    static const atlas_orch_state FORWARD[] = {ATLAS_ORCH_STATE_PREPARING,
                                               ATLAS_ORCH_STATE_RUNNING};
    for (size_t i = 0; i < sizeof(FORWARD) / sizeof(FORWARD[0]); i++) {
        atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_HEARTBEAT, token);
        op->phase = FORWARD[i];
        atlas_orch_result r;
        apply_ok(db, op, &r);
        atlas_orch_result_free(&r);
    }
}

static void set_submit_key_id(atlas_db *db, const char *job_uid, const char *bare_key_id) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "UPDATE orch_jobs SET submit_key_id = '%s' WHERE job_uid = '%s';",
                           bare_key_id, job_uid),
         &err);
    T_OK(atlas_db_exec_sql(db, atlas_buf_cstr(&sql), &err), &err);
    atlas_buf_free(&sql);
}

/* Runs one queued job to SUCCEEDED with a stored `changes.patch` artifact,
 * and records `submit_key_id = bare_key_id` on it. */
static void seed_succeeded_job(env *e, const char *bare_key_id, const char *patch_content,
                               atlas_buf *job_uid_out, char digest_out[ATLAS_SHA256_HEX_LEN + 1u]) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);

    atlas_orch_result s;
    apply_ok(db, submit_op(e), &s);
    T_OK(atlas_buf_set(job_uid_out, s.job_uid.data, s.job_uid.len, &err), &err);
    atlas_orch_result_free(&s);

    atlas_buf tok = ATLAS_BUF_INIT;
    (void)lease_once(db, &tok);
    advance_to_running(db, atlas_buf_cstr(&tok));

    atlas_orch_op *op = worker_op(ATLAS_ORCH_OP_COMPLETE, atlas_buf_cstr(&tok));
    op->success = true;
    op->exit_kind = ATLAS_ORCH_EXIT_OK;
    T_OK(atlas_buf_set_str(&op->driver_version, "fake/1", &err), &err);
    op->artifacts = (atlas_orch_artifact *)calloc(1u, sizeof(atlas_orch_artifact));
    T_REQUIRE(op->artifacts != NULL);
    atlas_orch_artifact_init(&op->artifacts[0]);
    op->artifact_count = 1u;
    T_OK(atlas_buf_set_str(&op->artifacts[0].name, ATLAS_ORCH_RESULT_PATCH_NAME, &err), &err);
    T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "patch", &err), &err);
    size_t n = strlen(patch_content);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(patch_content, n, hex);
    T_OK(atlas_buf_set_str(&op->artifacts[0].sha256, hex, &err), &err);
    op->artifacts[0].size_bytes = (int64_t)n;
    op->artifacts[0].content_stored = true;
    T_OK(atlas_buf_set(&op->artifacts[0].content, patch_content, n, &err), &err);

    atlas_orch_result r;
    apply_ok(db, op, &r);
    T_CHECK(r.state == ATLAS_ORCH_STATE_SUCCEEDED);
    atlas_orch_result_free(&r);
    atlas_buf_free(&tok);

    if (digest_out != NULL) {
        (void)snprintf(digest_out, ATLAS_SHA256_HEX_LEN + 1u, "%s", hex);
    }
    if (bare_key_id != NULL) {
        set_submit_key_id(db, atlas_buf_cstr(job_uid_out), bare_key_id);
    }

    atlas_db_close(db);
    atlas_buf_free(&db_path);
}

/* Bare (no "key_" prefix) form of a policy-display-form id, as
 * `orch_jobs.submit_key_id` and `deploys.proposed_key_id`/`confirmed_key_id`
 * store it. */
static const char *bare_id(const char *display_id) {
    const char *p = strstr(display_id, ATLAS_APIKEY_ID_PREFIX);
    return p != NULL ? p + strlen(ATLAS_APIKEY_ID_PREFIX) : display_id;
}

/* --- gwd_start: test_gw_submit.c's / test_deploy_rpc.c's own variant of
 * fx_daemon_start for the atlas-gw-daemon binary, accepting both a gateway
 * policy file and an orchestration policy file. */

static atlas_status gwd_start(env *e, const char *gw_policy_path, const char *orch_policy_path,
                              fx_daemon *d, atlas_err *err) {
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

/* Two alternating static buffers so a caller can write two policy files
 * before passing both paths to gwd_start. */
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

/* --- HTTP helpers (test_gw_submit.c's own shapes) --------------------------- */

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

static void bearer_of(const char *token, char *out, size_t out_size) {
    (void)snprintf(out, out_size, "Bearer %s", token);
}

/* `tests/test_deploy_rpc.c`'s own shape, copied rather than shared on this
 * file's own header precedent -- reads the queue directory's `.patch` bytes
 * so this suite can hash them itself, rather than trust the daemon's own
 * report of its digest. */
static atlas_status read_whole_file(const char *path, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot open %s", path);
    }
    char buf[8192];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            atlas_status st = atlas_err_set_errno(err, ATLAS_ERR_CONFIG, errno, "cannot read %s",
                                                  path);
            (void)close(fd);
            return st;
        }
        if (r == 0) {
            break;
        }
        atlas_status st = atlas_buf_append(out, buf, (size_t)r, err);
        if (st != ATLAS_OK) {
            (void)close(fd);
            return st;
        }
    }
    (void)close(fd);
    return ATLAS_OK;
}

/* Builds the standard gateway + orchestration policy pair: `tls_mode =
 * REVERSE_PROXY`, one submit key, one deploy key -- both required together
 * for the propose/confirm chain this suite drives end to end. No cleartext
 * acceptance line is needed under REVERSE_PROXY (T2's grammar refuses one
 * present there as MALFORMED), on `test_gw_submit.c`'s own precedent. */
static void start_daemon(env *e, fx_daemon *d, atlas_gateway **g_out) {
    atlas_err err;
    atlas_err_init(&err);

    char gw_policy[2048];
    (void)snprintf(gw_policy, sizeof(gw_policy),
                   "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                   "web_gui = yes\ntls_mode = REVERSE_PROXY\n"
                   "remote_submit_key = %s\nremote_submit_driver = fake\n"
                   "remote_submit_mode = patch\nremote_submit_max_attempts = 1\n"
                   "remote_submit_max_active = 8\nremote_submit_max_per_day = 64\n"
                   "remote_submit_gate = true\n"
                   "remote_deploy_key = %s\n",
                   (long)getuid(), e->submit_id, e->deploy_id);
    char orch_policy[512];
    (void)snprintf(orch_policy, sizeof(orch_policy),
                   "dispatcher_uid = 1\nsubmitter_uid = 2\n"
                   "repo = proj\ndriver = fake\nmode = patch\nworker_root = /tmp\n");

    const char *gw_path = write_policy_file(e, "gw.conf", gw_policy);
    const char *orch_path = write_policy_file(e, "orch.conf", orch_policy);
    fx_daemon_init(d);
    T_REQUIRE(gwd_start(e, gw_path, orch_path, d, &err) == ATLAS_OK);
    T_OK(fx_daemon_wait_ready(d, 15000, &err), &err);

    open_http_gateway(gw_policy, d, g_out);
}

/* --- (a) propose (submit) -> 2xx; confirm (submit) -> 403; challenge+confirm
 * (deploy, correct prefix) -> 2xx; the queue files exist. -------------------- */

static void test_a_propose_confirm_scope_and_prefix(void) {
    env e;
    env_open(&e);
    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);

    fx_daemon d;
    atlas_gateway *g = NULL;
    start_daemon(&e, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);
    char deploy_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.deploy_token, deploy_bearer, sizeof deploy_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;

    /* propose, with the submit bearer -> 2xx. */
    char propose_body[256];
    (void)snprintf(propose_body, sizeof propose_body, "job=%s", atlas_buf_cstr(&job_uid));
    post_form(g, "/api/v1/deploy/propose", submit_bearer, propose_body, &resp);
    T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                 "propose did not answer 2xx: %d %s", status_of(&resp), body_of(&resp));
    atlas_buf deploy_uid = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_field(&resp, "deploy", &deploy_uid), "no deploy field: %s",
                 body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"state\":\"PROPOSED\"") != NULL,
               "propose did not report PROPOSED: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* confirm with the submit bearer (wrong scope) -> 403. */
    {
        char confirm_body[512];
        (void)snprintf(confirm_body, sizeof confirm_body,
                       "deploy=%s&challenge=00000000000000000000000000000000&confirmation=%.8s",
                       atlas_buf_cstr(&deploy_uid), digest);
        post_form(g, "/api/v1/deploy/confirm", submit_bearer, confirm_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 403,
                   "confirm with the submit bearer did not answer 403: %d %s",
                   status_of(&resp), body_of(&resp));
        T_CHECK_MSG(strstr(body_of(&resp), "deploys:confirm") != NULL,
                   "403 sentence does not mention deploys:confirm scope: %s", body_of(&resp));
        atlas_buf_reset(&resp);
    }

    /* propose is refused for the deploy bearer too (wrong scope) -- the
       inverse of the case above, at the other end of the pair. */
    {
        post_form(g, "/api/v1/deploy/propose", deploy_bearer, propose_body, &resp);
        T_CHECK_MSG(status_of(&resp) == 403,
                   "propose with the deploy bearer did not answer 403: %d %s",
                   status_of(&resp), body_of(&resp));
        atlas_buf_reset(&resp);
    }

    /* challenge with the deploy bearer -> 2xx, carries a challenge token and
       the full patch digest. */
    char challenge_body[256];
    (void)snprintf(challenge_body, sizeof challenge_body, "deploy=%s",
                   atlas_buf_cstr(&deploy_uid));
    post_form(g, "/api/v1/deploy/challenge", deploy_bearer, challenge_body, &resp);
    T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                 "challenge did not answer 2xx: %d %s", status_of(&resp), body_of(&resp));
    atlas_buf challenge = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(get_field(&resp, "challenge", &challenge), "no challenge field: %s",
                 body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), digest) != NULL,
               "challenge did not carry the full patch digest: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* confirm with the deploy bearer and the correct eight-hex prefix -> 2xx. */
    char confirmation[9];
    memcpy(confirmation, digest, 8u);
    confirmation[8] = '\0';
    char confirm_body[512];
    (void)snprintf(confirm_body, sizeof confirm_body, "deploy=%s&challenge=%s&confirmation=%s",
                   atlas_buf_cstr(&deploy_uid), atlas_buf_cstr(&challenge), confirmation);
    post_form(g, "/api/v1/deploy/confirm", deploy_bearer, confirm_body, &resp);
    T_CHECK_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
               "confirm with the deploy bearer and the correct prefix did not answer 2xx: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"state\":\"CONFIRMED\"") != NULL,
               "confirm did not report CONFIRMED: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* The queue files: a real .req and a real .patch, the patch's own sha256
       matching the digest T2's response named. */
    {
        atlas_buf req_path = ATLAS_BUF_INIT;
        atlas_err perr;
        atlas_err_init(&perr);
        T_OK(atlas_buf_appendf(&req_path, &perr, "%s/deploy/requests/%s.req", fx_data_dir(&e.fx),
                               atlas_buf_cstr(&deploy_uid)),
             &perr);
        struct stat st;
        T_CHECK_MSG(stat(atlas_buf_cstr(&req_path), &st) == 0, "no .req file at %s",
                   atlas_buf_cstr(&req_path));
        atlas_buf_free(&req_path);

        atlas_buf patch_path = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&patch_path, &perr, "%s/deploy/requests/%s.patch",
                               fx_data_dir(&e.fx), atlas_buf_cstr(&deploy_uid)),
             &perr);
        T_CHECK_MSG(stat(atlas_buf_cstr(&patch_path), &st) == 0, "no .patch file at %s",
                   atlas_buf_cstr(&patch_path));

        atlas_buf patch_bytes = ATLAS_BUF_INIT;
        T_OK(read_whole_file(atlas_buf_cstr(&patch_path), &patch_bytes, &perr), &perr);
        char patch_hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(patch_bytes.data, patch_bytes.len, patch_hex);
        T_CHECK_MSG(strcmp(patch_hex, digest) == 0,
                   ".patch file's own sha256 (%s) does not match the digest T2's response "
                   "named (%s)",
                   patch_hex, digest);
        atlas_buf_free(&patch_bytes);
        atlas_buf_free(&patch_path);
    }

    atlas_buf_free(&challenge);
    atlas_buf_free(&deploy_uid);
    atlas_buf_free(&job_uid);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (b1) a JSON Content-Type is refused before the socket ----------------- */

/* This proves the Content-Type gate, not the route table's field filter: it
 * would still pass with `route_wants`/`build_api_params` deleted entirely,
 * because the 415 fires before either runs. Kept for its own sake -- the gate
 * is real and worth asserting -- but (b2) below is the one that proves a
 * field outside a route's declared params is dropped rather than forwarded. */
static void test_b1_json_content_type_refused_before_the_socket(void) {
    env e;
    env_open(&e);
    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/y b/y\n+bye\n", &job_uid, digest);

    fx_daemon d;
    atlas_gateway *g = NULL;
    start_daemon(&e, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;

    /* A JSON body naming "job", "patch" and "dry_run" -- none of these route
       rows accept a body of this Content-Type at all, so it is refused before
       `build_api_params` ever runs. */
    char json_body[256];
    (void)snprintf(json_body, sizeof json_body,
                   "{\"job\":\"%s\",\"patch\":\"rm -rf /\",\"dry_run\":\"yes\"}",
                   atlas_buf_cstr(&job_uid));
    http_request2(g, "POST", "/api/v1/deploy/propose", submit_bearer, NULL, NULL,
                 "application/json", json_body, (size_t)-1, &resp);
    T_CHECK_MSG(status_of(&resp) == 415, "JSON CT did not answer 415: %d %s", status_of(&resp),
               body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "application/x-www-form-urlencoded") != NULL,
               "wrong 415 sentence: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* No deploy was ever created: the daemon never saw the request at all.
       Listing with the submit bearer must come back empty. */
    post_form(g, "/api/v1/deploy/list", submit_bearer, "", &resp);
    T_CHECK_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
               "list did not answer 2xx: %d %s", status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"count\":0") != NULL,
               "a deploy was created by a request the Content-Type check should have refused: %s",
               body_of(&resp));
    atlas_buf_reset(&resp);

    atlas_buf_free(&job_uid);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (b2) an undeclared field is dropped, not forwarded --------------------
 *
 * `tests/test_gw_submit.c:771-794`'s own pattern: a *well-formed*
 * (form-encoded) request naming a field the route does not declare must
 * still succeed, and succeeding is the positive proof the field never
 * reached the daemon. `/api/v1/deploy/propose`'s row declares only "job"
 * (`API_WRITE_ROUTES[]`, `src/gw/gateway.c`); `patch` and `dry_run` are not
 * in it. The daemon's own `deploy.remote_propose` explicitly forbids a
 * forwarded "dry_run" (`refuse_if_present`, `src/ipc/server_deploy_remote.c`)
 * and would answer USAGE/400 if it ever saw one -- so a 2xx here, with the
 * seeded job's own PROPOSED state, is proof the gateway dropped both fields
 * before the call, not merely a guess that it might have. */
static void test_b2_an_undeclared_field_is_dropped_not_forwarded(void) {
    env e;
    env_open(&e);
    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/w b/w\n+word\n", &job_uid, digest);

    fx_daemon d;
    atlas_gateway *g = NULL;
    start_daemon(&e, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;
    char body[256];
    (void)snprintf(body, sizeof body, "job=%s&patch=x&dry_run=yes", atlas_buf_cstr(&job_uid));
    post_form(g, "/api/v1/deploy/propose", submit_bearer, body, &resp);
    T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                 "propose with undeclared patch=/dry_run= fields did not answer 2xx: %d %s "
                 "(a non-2xx here means one of them reached the daemon)",
                 status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"state\":\"PROPOSED\"") != NULL,
               "propose did not report PROPOSED: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    atlas_buf_free(&job_uid);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (c) /api/v1/deploy/get and /api/v1/deploy/list accept either bearer --- */

static void test_c_get_and_list_accept_either_credential(void) {
    env e;
    env_open(&e);
    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/z b/z\n+again\n", &job_uid, digest);

    fx_daemon d;
    atlas_gateway *g = NULL;
    start_daemon(&e, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);
    char deploy_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.deploy_token, deploy_bearer, sizeof deploy_bearer);
    char other_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.other_token, other_bearer, sizeof other_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;
    char propose_body[256];
    (void)snprintf(propose_body, sizeof propose_body, "job=%s", atlas_buf_cstr(&job_uid));
    post_form(g, "/api/v1/deploy/propose", submit_bearer, propose_body, &resp);
    T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                 "propose did not answer 2xx: %d %s", status_of(&resp), body_of(&resp));
    atlas_buf deploy_uid = ATLAS_BUF_INIT;
    T_REQUIRE(get_field(&resp, "deploy", &deploy_uid));
    atlas_buf_reset(&resp);

    /* /get with the submit bearer (the proposing credential) -> 2xx. */
    char get_body[256];
    (void)snprintf(get_body, sizeof get_body, "deploy=%s", atlas_buf_cstr(&deploy_uid));
    post_form(g, "/api/v1/deploy/get", submit_bearer, get_body, &resp);
    T_CHECK_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
               "/get with the submit bearer did not answer 2xx: %d %s", status_of(&resp),
               body_of(&resp));
    atlas_buf_reset(&resp);

    /* /get with the deploy bearer (this season's alternate scope) -> 2xx --
       the property this whole route pair exists to prove: the deploy
       credential is this channel's operator proxy and must see a PROPOSED
       deploy before it has confirmed one. */
    post_form(g, "/api/v1/deploy/get", deploy_bearer, get_body, &resp);
    T_CHECK_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
               "/get with the deploy bearer did not answer 2xx: %d %s", status_of(&resp),
               body_of(&resp));
    atlas_buf_reset(&resp);

    /* /list with each bearer -> 2xx. */
    post_form(g, "/api/v1/deploy/list", submit_bearer, "", &resp);
    T_CHECK_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
               "/list with the submit bearer did not answer 2xx: %d %s", status_of(&resp),
               body_of(&resp));
    atlas_buf_reset(&resp);
    post_form(g, "/api/v1/deploy/list", deploy_bearer, "", &resp);
    T_CHECK_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
               "/list with the deploy bearer did not answer 2xx: %d %s", status_of(&resp),
               body_of(&resp));
    atlas_buf_reset(&resp);

    /* The negative half of the same property: a credential named in neither
       `remote_submit_key` nor `remote_deploy_key` holds neither `jobs:submit`
       nor `deploys:confirm` (`gateway.auth` derives nothing for it), so
       `route_scope_granted`'s alt-scope table must refuse it on both routes
       exactly as it would refuse the primary-only check on any other route --
       the side table widens who is admitted, it does not open the gate to
       everyone. */
    post_form(g, "/api/v1/deploy/get", other_bearer, get_body, &resp);
    T_CHECK_MSG(status_of(&resp) == 403,
               "/get with an unlisted bearer did not answer 403: %d %s", status_of(&resp),
               body_of(&resp));
    atlas_buf_reset(&resp);
    post_form(g, "/api/v1/deploy/list", other_bearer, "", &resp);
    T_CHECK_MSG(status_of(&resp) == 403,
               "/list with an unlisted bearer did not answer 403: %d %s", status_of(&resp),
               body_of(&resp));
    atlas_buf_reset(&resp);

    atlas_buf_free(&deploy_uid);
    atlas_buf_free(&job_uid);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (d) cursor and limit arrive as integers: a second page differs -------- */

static void test_d_cursor_and_limit_arrive_as_integers(void) {
    env e;
    env_open(&e);
    atlas_buf job1 = ATLAS_BUF_INIT, job2 = ATLAS_BUF_INIT;
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/p b/p\n+one\n", &job1, NULL);
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/q b/q\n+two\n", &job2, NULL);

    fx_daemon d;
    atlas_gateway *g = NULL;
    start_daemon(&e, &d, &g);

    char submit_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.submit_token, submit_bearer, sizeof submit_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_buf deploy1 = ATLAS_BUF_INIT, deploy2 = ATLAS_BUF_INIT;
    {
        char body[256];
        (void)snprintf(body, sizeof body, "job=%s", atlas_buf_cstr(&job1));
        post_form(g, "/api/v1/deploy/propose", submit_bearer, body, &resp);
        T_REQUIRE(status_of(&resp) >= 200 && status_of(&resp) < 300);
        T_REQUIRE(get_field(&resp, "deploy", &deploy1));
        atlas_buf_reset(&resp);
    }
    /* `idx_deploys_one_active` (D.3) allows only one active (non-terminal)
       deploy per repository at a time: the first must be cancelled -- a
       terminal state a submit credential can reach on its own, no root agent
       needed -- before a second may be proposed. */
    {
        char cancel_body[256];
        (void)snprintf(cancel_body, sizeof cancel_body, "deploy=%s", atlas_buf_cstr(&deploy1));
        post_form(g, "/api/v1/deploy/cancel", submit_bearer, cancel_body, &resp);
        T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                     "cancel of the first deploy did not answer 2xx: %d %s", status_of(&resp),
                     body_of(&resp));
        atlas_buf_reset(&resp);
    }
    {
        char body[256];
        (void)snprintf(body, sizeof body, "job=%s", atlas_buf_cstr(&job2));
        post_form(g, "/api/v1/deploy/propose", submit_bearer, body, &resp);
        T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                     "the second propose (after cancelling the first) did not answer 2xx: %d %s",
                     status_of(&resp), body_of(&resp));
        T_REQUIRE(get_field(&resp, "deploy", &deploy2));
        atlas_buf_reset(&resp);
    }

    /* Page one: limit=1, cursor unset (0). */
    post_form(g, "/api/v1/deploy/list", submit_bearer, "limit=1", &resp);
    T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                 "page one did not answer 2xx: %d %s", status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "\"count\":1") != NULL, "page one count was not 1: %s",
               body_of(&resp));
    atlas_buf page1_deploy = ATLAS_BUF_INIT;
    T_REQUIRE(get_field(&resp, "deploy", &page1_deploy));
    atlas_buf next_cursor = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(tjson_get_raw(body_of(&resp), strlen(body_of(&resp)), "cursor", &next_cursor),
                 "no cursor field: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    /* Page two: the same limit, `cursor` set to page one's own cursor. If
       `cursor` arrived at the daemon as a JSON string instead of an integer
       (the defect this test exists to catch), `atlas_ipc_param_int` fails
       silently, the daemon reads 0, and page two is identical to page one. */
    char page2_body[128];
    (void)snprintf(page2_body, sizeof page2_body, "limit=1&cursor=%s",
                   atlas_buf_cstr(&next_cursor));
    post_form(g, "/api/v1/deploy/list", submit_bearer, page2_body, &resp);
    T_REQUIRE_MSG(status_of(&resp) >= 200 && status_of(&resp) < 300,
                 "page two did not answer 2xx: %d %s", status_of(&resp), body_of(&resp));
    atlas_buf page2_deploy = ATLAS_BUF_INIT;
    T_REQUIRE(get_field(&resp, "deploy", &page2_deploy));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&page1_deploy), atlas_buf_cstr(&page2_deploy)) != 0,
               "page two returned the same deploy as page one (%s): cursor did not arrive as an "
               "integer",
               atlas_buf_cstr(&page1_deploy));
    atlas_buf_reset(&resp);

    atlas_buf_free(&page2_deploy);
    atlas_buf_free(&next_cursor);
    atlas_buf_free(&page1_deploy);
    atlas_buf_free(&deploy2);
    atlas_buf_free(&deploy1);
    atlas_buf_free(&job2);
    atlas_buf_free(&job1);
    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (e) with no deploy key configured, challenge/confirm are not offered -- */

static void test_e_challenge_and_confirm_are_404_with_no_deploy_key(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* A policy with a submit key and no deploy key at all. */
    char gw_policy[2048];
    (void)snprintf(gw_policy, sizeof(gw_policy),
                   "enabled = yes\ngateway_uid = %ld\nremote_mcp = yes\n"
                   "web_gui = yes\ntls_mode = REVERSE_PROXY\n"
                   "remote_submit_key = %s\nremote_submit_driver = fake\n"
                   "remote_submit_mode = patch\nremote_submit_max_attempts = 1\n"
                   "remote_submit_max_active = 8\nremote_submit_max_per_day = 64\n"
                   "remote_submit_gate = true\n",
                   (long)getuid(), e.submit_id);
    char orch_policy[512];
    (void)snprintf(orch_policy, sizeof(orch_policy),
                   "dispatcher_uid = 1\nsubmitter_uid = 2\n"
                   "repo = proj\ndriver = fake\nmode = patch\nworker_root = /tmp\n");

    const char *gw_path = write_policy_file(&e, "gw.conf", gw_policy);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy);
    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_OK(fx_daemon_wait_ready(&d, 15000, &err), &err);

    atlas_gateway *g = NULL;
    open_http_gateway(gw_policy, &d, &g);

    char deploy_bearer[ATLAS_APIKEY_TOKEN_MAX + 8u];
    bearer_of(e.deploy_token, deploy_bearer, sizeof deploy_bearer);

    atlas_buf resp = ATLAS_BUF_INIT;
    post_form(g, "/api/v1/deploy/challenge", deploy_bearer, "deploy=dnonexistent", &resp);
    T_CHECK_MSG(status_of(&resp) == 404, "challenge with no deploy key did not answer 404: %d %s",
               status_of(&resp), body_of(&resp));
    T_CHECK_MSG(strstr(body_of(&resp), "remote deploy") != NULL,
               "404 sentence does not mention remote deploy: %s", body_of(&resp));
    atlas_buf_reset(&resp);

    post_form(g, "/api/v1/deploy/confirm", deploy_bearer,
             "deploy=dnonexistent&challenge=00000000000000000000000000000000&confirmation=abcd1234",
             &resp);
    T_CHECK_MSG(status_of(&resp) == 404, "confirm with no deploy key did not answer 404: %d %s",
               status_of(&resp), body_of(&resp));
    atlas_buf_reset(&resp);

    atlas_buf_free(&resp);
    atlas_gateway_close(g);
    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"propose (submit) 2xx, confirm (submit) 403, challenge+confirm (deploy, correct prefix) 2xx",
     test_a_propose_confirm_scope_and_prefix},
    {"a JSON content type is refused before the socket, no deploy is created",
     test_b1_json_content_type_refused_before_the_socket},
    {"an undeclared field (patch=, dry_run=) is dropped, not forwarded to the daemon",
     test_b2_an_undeclared_field_is_dropped_not_forwarded},
    {"/api/v1/deploy/get and /api/v1/deploy/list accept either credential and refuse a third",
     test_c_get_and_list_accept_either_credential},
    {"cursor and limit arrive at the daemon as integers", test_d_cursor_and_limit_arrive_as_integers},
    {"challenge and confirm are 404 with no deploy key configured",
     test_e_challenge_and_confirm_are_404_with_no_deploy_key},
};

ATLAS_TEST_MAIN("gw_deploy", TESTS)

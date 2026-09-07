/* Atlas - A17 T2: the daemon's `deploy.remote_*` group, verified
 * against a live daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `deploy.remote_propose`, `deploy.remote_get`, `deploy.remote_list`,
 * `deploy.remote_cancel`, `deploy.remote_challenge` and `deploy.remote_confirm`
 * -- six methods, tested against a real socket and a real gateway daemon
 * carrying an injected gateway policy naming both a submit key and a deploy
 * key, and an injected orchestration policy. `tests/test_orch_remote_rpc.c`'s
 * own shape (`env`, `gwd_start`, `write_policy_file`, `ipc_call`,
 * `get_str_field`) and `tests/test_db_deploy.c`'s own shape (seeding a
 * SUCCEEDED patch-mode job with a stored `changes.patch` artifact by driving
 * `atlas_orch_apply` directly against the fixture's own database file, before
 * any daemon opens it) are both copied here rather than shared, on both
 * files' own precedent: two different callers with two different frozen
 * response shapes sharing one helper is one more surface where a future
 * change to either quietly becomes a change to both.
 *
 * Required cases (task-2-brief.md, step 1): (a) propose from a seeded
 * SUCCEEDED job, and a proposal naming "dry_run" refused; (b) challenge then
 * confirm with the deploy credential and the correct prefix moves the row to
 * CONFIRMED, and the two queue files exist with the patch file's own sha256
 * matching the stored digest; (c) confirm with the submit credential (wrong
 * scope) and confirm with the wrong prefix are both refused; (d) the daemon
 * restarted after a `.res` file was written directly ingests it at startup;
 * (e) a malformed `.res` ends the deploy FAILED/INGEST, and a hand-written
 * `outcome FAILED`/`stage ABANDONED` result lets the next proposal through;
 * (f) the watcher tick ingests a `.res` written while the daemon is already
 * running; (g) confirm is refused while any job anywhere is non-terminal;
 * (h) both submission write points refuse a root job, naming the deploy uid,
 * while a deploy is CONFIRMED, and accept the same submission once a result
 * has arrived; (i) the four forbidden names answer "unknown method".
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "atlas/apikey.h"
#include "atlas/atlas.h"
#include "atlas/deploy.h"
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

#ifndef ATLAS_GW_DAEMON_BIN
#define ATLAS_GW_DAEMON_BIN "atlas-gw-daemon"
#endif

/* --- the fixture ------------------------------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_buf identity;
    atlas_buf commit;
    /* Minted with --no-scopes, mirroring test_orch_remote_rpc.c's env exactly.
     * Ids are in the policy's display form ("key_" + 16 hex). */
    char submit_token[ATLAS_APIKEY_TOKEN_MAX];
    char submit_id[ATLAS_APIKEY_SELECTOR_HEX + 6u];
    char deploy_token[ATLAS_APIKEY_TOKEN_MAX];
    char deploy_id[ATLAS_APIKEY_SELECTOR_HEX + 6u];
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

/* The general form: `tail`/`tail_n` are appended verbatim after "api-key
 * create --label <label>", so a caller passes {"--no-scopes"} (every
 * ordinary key in this file) or {"--scope", "repo:read"} (fix round 1, item
 * 5's scoped-deploy-key case) without this function needing to know the
 * difference -- `--scope` and its value are two separate argv entries, the
 * grammar `tests/test_apikey.c` itself uses. */
static void mint_key_tail(env *e, const char *label, const char *const *tail, size_t tail_n,
                          char *token_out, size_t token_size, char *id_out, size_t id_size) {
    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *create[8];
    size_t k = 0;
    create[k++] = "api-key";
    create[k++] = "create";
    create[k++] = "--label";
    create[k++] = label;
    T_REQUIRE(k + tail_n <= sizeof(create) / sizeof(create[0]));
    for (size_t i = 0; i < tail_n; i++) {
        create[k++] = tail[i];
    }
    run_atlas(e, create, k, &out, &code);
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

static void mint_key(env *e, const char *label, char *token_out, size_t token_size, char *id_out,
                     size_t id_size) {
    static const char *const NO_SCOPES[] = {"--no-scopes"};
    mint_key_tail(e, label, NO_SCOPES, 1u, token_out, token_size, id_out, id_size);
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

    /* Read the repository identity and pinned commit directly, the way
     * test_db_deploy.c does, so `submit_op` below can build a valid spec. */
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
 * any daemon opens it -- test_db_deploy.c's own shape. */

/* The general form, parameterised on which repository -- used directly by the
 * two-repository SIGSEGV-regression case below (fix round 1, item 1), and by
 * `submit_op` immediately after as the "proj"-only case every other test in
 * this file already calls. */
static atlas_orch_op *submit_op_repo(int64_t repo_id, const char *repo_name,
                                     const atlas_buf *identity, const atlas_buf *commit) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = repo_id;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, repo_name, &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, identity->data, identity->len, &err), &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, commit->data, commit->len, &err), &err);
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

static atlas_orch_op *submit_op(env *e) {
    return submit_op_repo(1, "proj", &e->identity, &e->commit);
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

/* The general form, parameterised on which repository -- used directly by the
 * two-repository SIGSEGV-regression case below (fix round 1, item 1). Runs
 * one queued job to SUCCEEDED with a stored `changes.patch` artifact, and
 * records `submit_key_id = bare_key_id` on it -- test_db_deploy.c's
 * `run_patch_job_to_success`, plus the ownership stamp `deploy.remote_propose`
 * itself needs. */
static void seed_succeeded_job_repo(env *e, int64_t repo_id, const char *repo_name,
                                    const atlas_buf *identity, const atlas_buf *commit,
                                    const char *bare_key_id, const char *patch_content,
                                    atlas_buf *job_uid_out,
                                    char digest_out[ATLAS_SHA256_HEX_LEN + 1u]) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);

    atlas_orch_result s;
    apply_ok(db, submit_op_repo(repo_id, repo_name, identity, commit), &s);
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

static void seed_succeeded_job(env *e, const char *bare_key_id, const char *patch_content,
                               atlas_buf *job_uid_out, char digest_out[ATLAS_SHA256_HEX_LEN + 1u]) {
    seed_succeeded_job_repo(e, 1, "proj", &e->identity, &e->commit, bare_key_id, patch_content,
                            job_uid_out, digest_out);
}

/* Submits a job and leaves it QUEUED (non-terminal), for the "confirm refuses
 * while any job is not terminal" case (g). Global, exactly as
 * `atlas_db_deploy_confirm_in_tx` counts it: not scoped to any repository. */
static void seed_queued_job(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    atlas_orch_result s;
    apply_ok(db, submit_op(e), &s);
    atlas_orch_result_free(&s);
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

/* --- gwd_start: test_orch_remote_rpc.c's own variant of fx_daemon_start for
 * the atlas-gw-daemon binary, accepting both a gateway policy file and an
 * orchestration policy file. */

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
    (void)snprintf(path_storage[slot], sizeof(path_storage[slot]), "%s", atlas_buf_cstr(&path));
    atlas_buf_free(&path);
    return path_storage[slot];
}

static void build_policies(env *e, char *gw_policy, size_t gw_size, char *orch_policy,
                           size_t orch_size) {
    (void)snprintf(
        gw_policy, gw_size,
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
        "remote_submit_max_active = 4\n"
        "remote_submit_max_per_day = 20\n"
        "remote_submit_gate = true\n"
        "remote_deploy_key = %s\n",
        (long)getuid(), e->submit_id, e->deploy_id);
    /* `submitter_uid` names this test process's own uid, unlike
     * test_orch_remote_rpc.c's own policy (which deliberately omits it) --
     * case (h) needs `job.submit` to reach the deploy-confirmed refusal
     * rather than being refused earlier as "not a submitter". */
    (void)snprintf(orch_policy, orch_size,
                   "dispatcher_uid = 1\n"
                   "submitter_uid = %ld\n"
                   "repo = proj\n"
                   "driver = fake\n"
                   "driver = fake-repo\n"
                   "mode = patch\n"
                   "worker_root = /tmp\n",
                   (long)getuid());
}

static void ipc_call(const char *socket, const char *method, const char *params, atlas_buf *resp) {
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_ipc_call(socket, method, params, resp, &err);
}

static bool contains(const atlas_buf *b, const char *s) {
    return strstr(atlas_buf_cstr(b), s) != NULL;
}

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

/* --- filesystem checks on the daemon's own queue directories ---------------- */

static bool file_exists(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0;
}

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

/* Builds a well-formed `outcome SUCCEEDED`/`stage DONE` `.res` body with
 * `text` as its trailer, computing `text_bytes` from `strlen(text)` rather
 * than a hand-counted literal -- a hand-counted `text_bytes` is exactly the
 * off-by-one this file's own parser exists to catch. */
static int build_success_res_body(const char *deploy_uid, const char *text, char *out,
                                  size_t out_size) {
    return snprintf(out, out_size,
                    "atlas-deploy-result 1\n"
                    "deploy %s\n"
                    "outcome SUCCEEDED\n"
                    "stage DONE\n"
                    "dry_run no\n"
                    "head_before 1111111111111111111111111111111111111111\n"
                    "head_after 2222222222222222222222222222222222222222\n"
                    "tree_dirty_before no\n"
                    "installed_version atlas 9.9.9\n"
                    "units atlas.service=active\n"
                    "rollback none\n"
                    "text_bytes %zu\n"
                    "--\n"
                    "%s",
                    deploy_uid, strlen(text), text);
}

static void write_whole_file(const char *path, const char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    T_REQUIRE_MSG(fd >= 0, "cannot create %s: %s", path, strerror(errno));
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        T_REQUIRE_MSG(w > 0, "short write to %s", path);
        off += (size_t)w;
    }
    (void)close(fd);
}

/* One bounded poll loop, driving `deploy.remote_get` until its response
 * contains `needle` or the deadline passes. Mirrors `fx_wait_for_substring`'s
 * own shape (an observable outcome, never a guessed sleep) over a method
 * that is not `atlas events`. */
static bool poll_deploy_get_contains(const char *socket, const char *token, const char *deploy_uid,
                                     const char *needle, int timeout_ms) {
    char params[512];
    (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                   token);
    int waited = 0;
    while (waited <= timeout_ms) {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(socket, "deploy.remote_get", params, &resp);
        bool found = contains(&resp, needle);
        atlas_buf_free(&resp);
        if (found) {
            return true;
        }
        struct timespec ts = {0, 100L * 1000000L};
        (void)nanosleep(&ts, NULL);
        waited += 100;
    }
    return false;
}

/* --- fix round 1, item 2: gateway.auth derives the right scope for each key */

/* CRITICAL fix round 1, item 2. `gateway.auth`'s scope derivation
 * (`src/ipc/server_gw.c`) is an `else if` chain, and the middle arm's own
 * condition used to be only "is the remote submit group offered at all" --
 * true for every credential once one submit key is configured, including the
 * deploy credential, which is not one of the submit keys. An `else if` chain
 * commits to the first arm whose *condition* holds, whether or not that arm's
 * body actually does anything for this particular key, so with one submit key
 * and one deploy key configured (this file's own `build_policies`, the
 * season's own deployment) the deploy credential never reached the third arm
 * at all and `gateway.auth` reported no `deploys:confirm` for it -- T3's
 * confirm route would have 403'd. This proves each of the two keys
 * `build_policies` names derives exactly its own scope and never the other
 * one. */
static void test_gateway_auth_derives_the_right_scope_for_each_key(void) {
    env e;
    env_open(&e);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "gateway.auth", params, &resp);
        char scopes[256];
        T_REQUIRE_MSG(get_str_field(atlas_buf_cstr(&resp), "scopes", scopes, sizeof scopes),
                     "no \"scopes\" field: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(scopes, "deploys:confirm") == 0,
                    "the deploy key should derive exactly deploys:confirm, got \"%s\": %s", scopes,
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "gateway.auth", params, &resp);
        char scopes[256];
        T_REQUIRE_MSG(get_str_field(atlas_buf_cstr(&resp), "scopes", scopes, sizeof scopes),
                     "no \"scopes\" field: %s", atlas_buf_cstr(&resp));
        T_CHECK_MSG(strcmp(scopes, "jobs:submit") == 0,
                    "the submit key should derive exactly jobs:submit, got \"%s\": %s", scopes,
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- (a): propose from a seeded SUCCEEDED job; "dry_run" refused ------------ */

static void test_propose_from_seeded_job_and_dry_run_refused(void) {
    env e;
    env_open(&e);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    /* A proposal naming "dry_run" is refused before anything is queued. */
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"job\":\"%s\",\"token\":\"%s\",\"dry_run\":true}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_CHECK_MSG(contains(&resp, "dry_run"),
                    "a proposal naming dry_run was not refused: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_CHECK_MSG(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid),
                    "no \"deploy\" field in propose response: %s", atlas_buf_cstr(&resp));
        char got_digest[ATLAS_SHA256_HEX_LEN + 1u];
        T_CHECK(get_str_field(atlas_buf_cstr(&resp), "patch_sha256", got_digest,
                              sizeof got_digest));
        T_CHECK_MSG(strcmp(got_digest, digest) == 0, "patch_sha256 mismatch: got %s want %s",
                    got_digest, digest);
        T_CHECK(contains(&resp, "\"patch_bytes\":"));
        atlas_buf_free(&resp);
    }
    T_CHECK(deploy_uid[0] == 'd');

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    env_close(&e);
}

/* --- IMPORTANT fix round 1, item 5: the deploy credential holds no other power */

/* The deploy credential follows A16's dispose rule, not A14's submit rule: it
 * must hold NO stored scope at all (`verify_deploy_credential`,
 * `src/ipc/server_deploy_remote.c`). A deploy key minted with an ordinary
 * grantable read scope (`repo:read`) is refused at `deploy.remote_challenge`,
 * naming the rule, exactly as `gateway.auth`'s own `rec.mask == 0u` condition
 * (`src/ipc/server_gw.c`) already refuses to derive `deploys:confirm` for the
 * same key. */
static void test_deploy_credential_with_a_stored_scope_is_refused_at_challenge(void) {
    env e;
    env_open(&e);

    char scoped_token[ATLAS_APIKEY_TOKEN_MAX];
    char scoped_id[ATLAS_APIKEY_SELECTOR_HEX + 6u];
    static const char *const SCOPE_TAIL[] = {"--scope", "repo:read"};
    mint_key_tail(&e, "deploy-key-scoped", SCOPE_TAIL, 2u, scoped_token, sizeof scoped_token,
                 scoped_id, sizeof scoped_id);

    char gw_policy_text[4096];
    (void)snprintf(gw_policy_text, sizeof gw_policy_text,
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
                   "remote_submit_max_active = 4\n"
                   "remote_submit_max_per_day = 20\n"
                   "remote_submit_gate = true\n"
                   "remote_deploy_key = %s\n",
                   (long)getuid(), e.submit_id, scoped_id);
    char orch_policy_text[4096];
    (void)snprintf(orch_policy_text, sizeof orch_policy_text,
                   "dispatcher_uid = 1\n"
                   "submitter_uid = %ld\n"
                   "repo = proj\n"
                   "driver = fake\n"
                   "driver = fake-repo\n"
                   "mode = patch\n"
                   "worker_root = /tmp\n",
                   (long)getuid());
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       scoped_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_CHECK_MSG(!contains(&resp, "\"challenge\":"),
                    "a challenge was minted for a deploy key holding a stored scope: %s",
                    atlas_buf_cstr(&resp));
        T_CHECK_MSG(contains(&resp, "no other power"),
                    "the refusal did not name the rule: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    env_close(&e);
}

/* --- (b)/(c): challenge, confirm, the two queue files, scope and prefix ----- */

static void test_challenge_confirm_and_queue_files(void) {
    env e;
    env_open(&e);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    const char *patch = "diff --git a/x b/x\n+hi\n";
    seed_succeeded_job(&e, bare_id(e.submit_id), patch, &job_uid, digest);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid));
        atlas_buf_free(&resp);
    }

    /* (c), first half: confirm with the submit credential (wrong pool) is
     * refused before a challenge is even minted -- the deploy group's own
     * credential check, `verify_deploy_credential`. */
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_CHECK_MSG(contains(&resp, "did not authenticate") ||
                        contains(&resp, "not one the remote submission policy names"),
                    "a challenge minted with the submit credential was not refused: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    char challenge[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_CHECK_MSG(get_str_field(atlas_buf_cstr(&resp), "challenge", challenge, sizeof challenge),
                    "no \"challenge\" field: %s", atlas_buf_cstr(&resp));
        T_CHECK(contains(&resp, "\"patch_sha256\":\""));
        atlas_buf_free(&resp);
    }

    char prefix[9];
    memcpy(prefix, digest, 8u);
    prefix[8] = '\0';

    /* (c), second half: the wrong prefix is refused, and the challenge stays
     * spendable (it was not consumed by a refused spend). */
    {
        char wrong[9] = "deadbeef";
        if (strcmp(wrong, prefix) == 0) {
            wrong[0] = 'c'; /* astronomically unlikely; keep it wrong regardless */
        }
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, wrong);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_CHECK_MSG(!contains(&resp, "\"state\":\"CONFIRMED\""),
                    "confirm with the wrong prefix was not refused: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* The real confirmation. */
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, prefix);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"state\":\"CONFIRMED\""), "confirm did not succeed: %s",
                    atlas_buf_cstr(&resp));
        T_CHECK(contains(&resp, "\"spooled\":true"));
        atlas_buf_free(&resp);
    }

    /* deploy.remote_get on a CONFIRMED deploy with no result yet reports
     * `age_seconds`, derived from `confirmed_at` -- the one field this
     * method computes rather than reads back verbatim. */
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_get", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"age_seconds\":"), "no age_seconds on a CONFIRMED deploy: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* deploy.remote_list, driven with the deploy credential, sees the one
     * deploy this test proposed and confirmed -- the one method the brief
     * enumerates that otherwise has no coverage in this suite. */
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_list", params, &resp);
        T_CHECK_MSG(contains(&resp, deploy_uid), "deploy.remote_list did not list %s: %s",
                    deploy_uid, atlas_buf_cstr(&resp));
        T_CHECK_MSG(contains(&resp, "\"count\":1"), "expected count:1: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* A spent challenge cannot be reused. */
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, prefix);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_CHECK_MSG(!contains(&resp, "\"spooled\""),
                    "a spent challenge was accepted a second time: %s", atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* The two queue files exist, and the patch file's own sha256 equals the
     * stored digest. */
    char req_path[4096], patch_path[4096];
    (void)snprintf(req_path, sizeof req_path, "%s/deploy/requests/%s.req", fx_data_dir(&e.fx),
                   deploy_uid);
    (void)snprintf(patch_path, sizeof patch_path, "%s/deploy/requests/%s.patch",
                   fx_data_dir(&e.fx), deploy_uid);
    T_CHECK_MSG(file_exists(req_path), "no request file at %s", req_path);
    T_CHECK_MSG(file_exists(patch_path), "no patch file at %s", patch_path);
    {
        atlas_buf pbytes = ATLAS_BUF_INIT;
        T_OK(read_whole_file(patch_path, &pbytes, &err), &err);
        char got[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(pbytes.data, pbytes.len, got);
        T_CHECK_MSG(strcmp(got, digest) == 0, "queued patch sha256 mismatch: got %s want %s", got,
                    digest);
        atlas_buf_free(&pbytes);
    }
    {
        atlas_buf rbytes = ATLAS_BUF_INIT;
        T_OK(read_whole_file(req_path, &rbytes, &err), &err);
        T_CHECK(contains(&rbytes, "atlas-deploy-request 1\n"));
        T_CHECK(contains(&rbytes, deploy_uid));
        T_CHECK(contains(&rbytes, atlas_buf_cstr(&job_uid)));
        T_CHECK(contains(&rbytes, digest));
        atlas_buf_free(&rbytes);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    env_close(&e);
}

/* --- (d)/(e): startup ingest, malformed results, the ABANDONED escape ------- */

static void test_startup_ingest_malformed_and_abandoned(void) {
    env e;
    env_open(&e);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix[9];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid));
        atlas_buf_free(&resp);
    }
    memcpy(prefix, digest, 8u);
    prefix[8] = '\0';

    char challenge[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "challenge", challenge, sizeof challenge));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, prefix);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_REQUIRE_MSG(contains(&resp, "\"state\":\"CONFIRMED\""), "confirm did not succeed: %s",
                     atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);

    /* (d): the fake agent writes the result while the daemon is down. */
    char res_path[4096];
    (void)snprintf(res_path, sizeof res_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy_uid);
    {
        char body[512];
        int blen = build_success_res_body(deploy_uid, "all is well", body, sizeof body);
        T_REQUIRE(blen > 0 && (size_t)blen < sizeof body);
        write_whole_file(res_path, body, (size_t)blen);
    }

    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock2 = atlas_buf_cstr(&d.socket);

    T_CHECK_MSG(
        poll_deploy_get_contains(sock2, e.deploy_token, deploy_uid, "\"state\":\"SUCCEEDED\"",
                                 10000),
        "the daemon did not ingest the .res file written while it was stopped");
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_get", params, &resp);
        T_CHECK(contains(&resp, "\"stage\":\"DONE\""));
        T_CHECK(contains(&resp, "all is well"));
        atlas_buf_free(&resp);
    }
    T_CHECK_MSG(!file_exists(res_path), "the .res file was not removed after ingest");

    /* (e), first half: a second deploy whose .res is malformed ends
     * FAILED/INGEST. */
    atlas_buf job2_uid = ATLAS_BUF_INIT;
    char digest2[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/y b/y\n+bye\n", &job2_uid, digest2);
    char deploy2_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix2[9];
    memcpy(prefix2, digest2, 8u);
    prefix2[8] = '\0';
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job2_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_propose", params, &resp);
        T_REQUIRE(
            get_str_field(atlas_buf_cstr(&resp), "deploy", deploy2_uid, sizeof deploy2_uid));
        atlas_buf_free(&resp);
    }
    char challenge2[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy2_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_challenge", params, &resp);
        T_REQUIRE(
            get_str_field(atlas_buf_cstr(&resp), "challenge", challenge2, sizeof challenge2));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy2_uid, e.deploy_token, challenge2, prefix2);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_confirm", params, &resp);
        T_REQUIRE_MSG(contains(&resp, "\"state\":\"CONFIRMED\""), "confirm did not succeed: %s",
                     atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
    char res2_path[4096];
    (void)snprintf(res2_path, sizeof res2_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy2_uid);
    write_whole_file(res2_path, "not even close to the grammar", 30u);

    T_CHECK_MSG(
        poll_deploy_get_contains(sock2, e.deploy_token, deploy2_uid, "\"state\":\"FAILED\"", 10000),
        "a malformed .res did not end the deploy FAILED");
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy2_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_get", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"stage\":\"INGEST\""), "malformed result stage: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
    T_CHECK_MSG(!file_exists(res2_path), "the malformed .res file was not removed");

    /* (e), second half: root's own hand-written escape -- outcome FAILED,
     * stage ABANDONED, a short text -- and the next proposal for the SAME
     * repository is then accepted (idx_deploys_one_active no longer blocks
     * it, because this deploy is terminal). */
    atlas_buf job3_uid = ATLAS_BUF_INIT;
    char digest3[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/z b/z\n+again\n", &job3_uid,
                      digest3);
    char deploy3_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix3[9];
    memcpy(prefix3, digest3, 8u);
    prefix3[8] = '\0';
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job3_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_propose", params, &resp);
        T_REQUIRE(
            get_str_field(atlas_buf_cstr(&resp), "deploy", deploy3_uid, sizeof deploy3_uid));
        atlas_buf_free(&resp);
    }
    char challenge3[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy3_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_challenge", params, &resp);
        T_REQUIRE(
            get_str_field(atlas_buf_cstr(&resp), "challenge", challenge3, sizeof challenge3));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy3_uid, e.deploy_token, challenge3, prefix3);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_confirm", params, &resp);
        T_REQUIRE_MSG(contains(&resp, "\"state\":\"CONFIRMED\""), "confirm did not succeed: %s",
                     atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
    char res3_path[4096];
    (void)snprintf(res3_path, sizeof res3_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy3_uid);
    {
        /* D.3's escape hatch: root types the deploy uid it is rescuing (it
         * has to, to name the file `<uid>.res` in the first place) plus
         * `outcome FAILED`, `stage ABANDONED` and a short text -- the four
         * required keys and nothing else. */
        const char *text = "abandoned by root, by hand";
        char abandoned[512];
        int alen = snprintf(abandoned, sizeof abandoned,
                            "atlas-deploy-result 1\n"
                            "deploy %s\n"
                            "outcome FAILED\n"
                            "stage ABANDONED\n"
                            "text_bytes %zu\n"
                            "--\n"
                            "%s",
                            deploy3_uid, strlen(text), text);
        T_REQUIRE(alen > 0 && (size_t)alen < sizeof abandoned);
        write_whole_file(res3_path, abandoned, (size_t)alen);
    }
    T_CHECK_MSG(
        poll_deploy_get_contains(sock2, e.deploy_token, deploy3_uid, "\"state\":\"FAILED\"",
                                 10000),
        "the hand-written ABANDONED result did not end the deploy FAILED");
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy3_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_get", params, &resp);
        T_CHECK(contains(&resp, "\"stage\":\"ABANDONED\""));
        atlas_buf_free(&resp);
    }

    /* The next proposal for this repository is accepted: idx_deploys_one_active
     * no longer blocks, because every prior deploy for "proj" is terminal. */
    atlas_buf job4_uid = ATLAS_BUF_INIT;
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/w b/w\n+final\n", &job4_uid, NULL);
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job4_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock2, "deploy.remote_propose", params, &resp);
        T_CHECK_MSG(contains(&resp, "\"deploy\":\"d"),
                    "a proposal after every prior deploy terminated was refused: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    atlas_buf_free(&job2_uid);
    atlas_buf_free(&job3_uid);
    atlas_buf_free(&job4_uid);
    env_close(&e);
}

/* --- (f): tick ingest while the daemon is already running ------------------- */

static void test_tick_ingest_while_running(void) {
    env e;
    env_open(&e);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix[9];
    memcpy(prefix, digest, 8u);
    prefix[8] = '\0';
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid));
        atlas_buf_free(&resp);
    }
    char challenge[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "challenge", challenge, sizeof challenge));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, prefix);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_REQUIRE_MSG(contains(&resp, "\"state\":\"CONFIRMED\""), "confirm did not succeed: %s",
                     atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* Written while the daemon is up and serving requests -- the watcher
     * tick's own "is results/ non-empty?" derivation must notice it. */
    char res_path[4096];
    (void)snprintf(res_path, sizeof res_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy_uid);
    {
        char body[512];
        int blen = build_success_res_body(deploy_uid, "tick ok!", body, sizeof body);
        T_REQUIRE(blen > 0 && (size_t)blen < sizeof body);
        write_whole_file(res_path, body, (size_t)blen);
    }

    T_CHECK_MSG(
        poll_deploy_get_contains(sock, e.deploy_token, deploy_uid, "\"state\":\"SUCCEEDED\"",
                                 15000),
        "the running daemon's tick did not ingest a .res written while it ran");

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    env_close(&e);
}

/* --- fix round 1, item 1: two pending .res files in one ingest pass --------- */

/* Registers a second git repository ("proj2") in the same fixture,
 * test_decision_bridge.c's own shape for a two-repository case. Needed here
 * because `idx_deploys_one_active` allows only one PROPOSED-or-CONFIRMED
 * deploy per repository at a time: the only way to have two deploys both
 * reach CONFIRMED (and so both leave a `.res` file for one ingest pass to
 * find) is two different repositories. */
static void register_second_repo(env *e, atlas_buf *identity_out, atlas_buf *commit_out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_mkdir(e->fx.root.data, "proj2", &err), &err);
    atlas_buf repo2 = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&repo2, &err, "%s/proj2", e->fx.root.data), &err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(&repo2), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&repo2), "b.c", "int other(void){return 1;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(&repo2), &err), &err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(&repo2), "second project", &err), &err);

    atlas_buf out = ATLAS_BUF_INIT;
    int code = 0;
    const char *add[] = {"repo", "add", atlas_buf_cstr(&repo2), "--name", "proj2"};
    run_atlas(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);
    const char *scan_args[] = {"scan", "proj2"};
    run_atlas(e, scan_args, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);
    atlas_buf_free(&repo2);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(db, "proj2", &ri, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_db_repo_identity_hash(db, ri.id, identity_out, &err), &err);
    T_OK(atlas_buf_set_str(commit_out,
                           ri.scanned_head[0] != '\0'
                               ? ri.scanned_head
                               : "0123456789abcdef0123456789abcdef01234567",
                           &err),
         &err);
    atlas_repo_info_free(&ri);
    atlas_db_close(db);
    atlas_buf_free(&db_path);
}

/* CRITICAL fix round 1, item 1. `name_cmp` (`src/daemon/deploy_spool.c`) used
 * to dereference filename bytes as a pointer -- a deterministic SIGSEGV on
 * the writer thread whenever `qsort` had two or more elements to actually
 * compare. Every other case in this file only ever has one `.res` file
 * present at ingest time (each is written, polled and ingested before the
 * next is created), so `qsort` was always called with n<=1 and the buggy
 * comparator was simply never invoked -- which is exactly why the original
 * suite did not catch it. This test writes two real, valid `.res` files (for
 * two different repositories' CONFIRMED deploys, so both can be CONFIRMED at
 * once) while the daemon is stopped, so the restart's startup ingest sees
 * both in one `readdir` pass and must actually sort a two-element array. */
static void test_ingest_pass_sorts_two_pending_res_files_without_crashing(void) {
    env e;
    env_open(&e);

    atlas_buf identity2 = ATLAS_BUF_INIT, commit2 = ATLAS_BUF_INIT;
    register_second_repo(&e, &identity2, &commit2);

    atlas_buf job1_uid = ATLAS_BUF_INIT;
    char digest1[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+one\n", &job1_uid, digest1);

    atlas_buf db2_path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf job2_uid = ATLAS_BUF_INIT;
    char digest2[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job_repo(&e, 2, "proj2", &identity2, &commit2, bare_id(e.submit_id),
                            "diff --git a/y b/y\n+two\n", &job2_uid, digest2);
    atlas_buf_free(&db2_path);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    /* `build_policies` only names "proj"; append "proj2" as a second allowed
     * repository, the grammar's own repeated-key form (already used for
     * `driver` above). */
    size_t used = strlen(orch_policy_text);
    (void)snprintf(orch_policy_text + used, sizeof orch_policy_text - used, "repo = proj2\n");
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy1_uid[ATLAS_DEPLOY_UID_MAX], deploy2_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix1[9], prefix2[9];
    memcpy(prefix1, digest1, 8u);
    prefix1[8] = '\0';
    memcpy(prefix2, digest2, 8u);
    prefix2[8] = '\0';

    const struct {
        const atlas_buf *job_uid;
        char *deploy_uid_out;
        const char *prefix;
    } plan[2] = {
        {&job1_uid, deploy1_uid, prefix1},
        {&job2_uid, deploy2_uid, prefix2},
    };
    for (size_t i = 0; i < 2u; i++) {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(plan[i].job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE_MSG(
            get_str_field(atlas_buf_cstr(&resp), "deploy", plan[i].deploy_uid_out,
                         ATLAS_DEPLOY_UID_MAX),
            "propose %zu failed: %s", i, atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);

        char challenge[80];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}",
                       plan[i].deploy_uid_out, e.deploy_token);
        atlas_buf resp2 = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp2);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp2), "challenge", challenge, sizeof challenge));
        atlas_buf_free(&resp2);

        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       plan[i].deploy_uid_out, e.deploy_token, challenge, plan[i].prefix);
        atlas_buf resp3 = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp3);
        T_REQUIRE_MSG(contains(&resp3, "\"state\":\"CONFIRMED\""), "confirm %zu did not succeed: %s",
                     i, atlas_buf_cstr(&resp3));
        atlas_buf_free(&resp3);
    }

    fx_daemon_stop(&d, false);

    /* Both `.res` files land while the daemon is down, so the restart's
     * startup ingest is the one pass that must see both at once. */
    char res1_path[4096], res2_path[4096];
    (void)snprintf(res1_path, sizeof res1_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy1_uid);
    (void)snprintf(res2_path, sizeof res2_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy2_uid);
    {
        char body[512];
        int blen = build_success_res_body(deploy1_uid, "first result", body, sizeof body);
        T_REQUIRE(blen > 0 && (size_t)blen < sizeof body);
        write_whole_file(res1_path, body, (size_t)blen);
    }
    {
        char body[512];
        int blen = build_success_res_body(deploy2_uid, "second result", body, sizeof body);
        T_REQUIRE(blen > 0 && (size_t)blen < sizeof body);
        write_whole_file(res2_path, body, (size_t)blen);
    }

    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock2 = atlas_buf_cstr(&d.socket);

    T_CHECK_MSG(
        poll_deploy_get_contains(sock2, e.deploy_token, deploy1_uid, "\"state\":\"SUCCEEDED\"",
                                 10000),
        "the first deploy did not reach SUCCEEDED -- the daemon may have crashed sorting two "
        "pending .res files");
    T_CHECK_MSG(
        poll_deploy_get_contains(sock2, e.deploy_token, deploy2_uid, "\"state\":\"SUCCEEDED\"",
                                 10000),
        "the second deploy did not reach SUCCEEDED -- the daemon may have crashed sorting two "
        "pending .res files");
    /* Explicit liveness check, beyond the two polls above: a crashed writer
     * thread takes the whole process down (SIGSEGV's default disposition),
     * so this is a direct assertion that the daemon is still the same live
     * process, not merely that two polls happened to time out gracefully. */
    T_CHECK_MSG(kill(d.pid, 0) == 0, "the daemon process is gone after the ingest pass (pid %ld)",
                (long)d.pid);
    T_CHECK_MSG(!file_exists(res1_path), "the first .res file was not removed after ingest");
    T_CHECK_MSG(!file_exists(res2_path), "the second .res file was not removed after ingest");

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job1_uid);
    atlas_buf_free(&job2_uid);
    atlas_buf_free(&identity2);
    atlas_buf_free(&commit2);
    env_close(&e);
}

/* --- (g): confirm refused while any job anywhere is non-terminal ------------ */

static void test_confirm_refused_while_a_job_is_not_terminal(void) {
    env e;
    env_open(&e);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);
    /* A second, unrelated job left QUEUED -- global, not scoped to "proj". */
    seed_queued_job(&e);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix[9];
    memcpy(prefix, digest, 8u);
    prefix[8] = '\0';
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid));
        atlas_buf_free(&resp);
    }
    char challenge[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "challenge", challenge, sizeof challenge));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, prefix);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_CHECK_MSG(!contains(&resp, "\"state\":\"CONFIRMED\""),
                    "confirm succeeded while a job elsewhere was QUEUED: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    env_close(&e);
}

/* --- (h): both submission write points refuse a root job while CONFIRMED --- */

static void test_root_submission_refused_while_deploy_confirmed(void) {
    env e;
    env_open(&e);

    atlas_buf job_uid = ATLAS_BUF_INIT;
    char digest[ATLAS_SHA256_HEX_LEN + 1u];
    seed_succeeded_job(&e, bare_id(e.submit_id), "diff --git a/x b/x\n+hi\n", &job_uid, digest);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    char deploy_uid[ATLAS_DEPLOY_UID_MAX];
    char prefix[9];
    memcpy(prefix, digest, 8u);
    prefix[8] = '\0';
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"job\":\"%s\",\"token\":\"%s\"}",
                       atlas_buf_cstr(&job_uid), e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_propose", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "deploy", deploy_uid, sizeof deploy_uid));
        atlas_buf_free(&resp);
    }
    char challenge[80];
    {
        char params[512];
        (void)snprintf(params, sizeof params, "{\"deploy\":\"%s\",\"token\":\"%s\"}", deploy_uid,
                       e.deploy_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_challenge", params, &resp);
        T_REQUIRE(get_str_field(atlas_buf_cstr(&resp), "challenge", challenge, sizeof challenge));
        atlas_buf_free(&resp);
    }
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"deploy\":\"%s\",\"token\":\"%s\",\"challenge\":\"%s\","
                       "\"confirmation\":\"%s\"}",
                       deploy_uid, e.deploy_token, challenge, prefix);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "deploy.remote_confirm", params, &resp);
        T_REQUIRE_MSG(contains(&resp, "\"state\":\"CONFIRMED\""), "confirm did not succeed: %s",
                     atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* job.remote_submit, while CONFIRMED, is refused and names the deploy uid. */
    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"repo\":\"proj\",\"task\":\"do something else\",\"token\":\"%s\"}",
                       e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "job.remote_submit", params, &resp);
        T_CHECK_MSG(contains(&resp, deploy_uid) && contains(&resp, "confirmed"),
                    "job.remote_submit was not refused with the deploy uid while CONFIRMED: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }
    /* job.submit (the local operator/submitter path), same repository, same
     * refusal -- one write point, both methods reach it. */
    {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "job.submit", "{\"repo\":\"proj\",\"task\":\"do something else\"}", &resp);
        T_CHECK_MSG(contains(&resp, deploy_uid) && contains(&resp, "confirmed"),
                    "job.submit was not refused with the deploy uid while CONFIRMED: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* Once a result has arrived, the same submission is accepted. */
    char res_path[4096];
    (void)snprintf(res_path, sizeof res_path, "%s/deploy/results/%s.res", fx_data_dir(&e.fx),
                   deploy_uid);
    {
        char body[512];
        int blen = build_success_res_body(deploy_uid, "done", body, sizeof body);
        T_REQUIRE(blen > 0 && (size_t)blen < sizeof body);
        write_whole_file(res_path, body, (size_t)blen);
    }
    T_CHECK_MSG(
        poll_deploy_get_contains(sock, e.deploy_token, deploy_uid, "\"state\":\"SUCCEEDED\"",
                                 15000),
        "the result never arrived");

    {
        char params[512];
        (void)snprintf(params, sizeof params,
                       "{\"repo\":\"proj\",\"task\":\"do something else\",\"token\":\"%s\"}",
                       e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, "job.remote_submit", params, &resp);
        T_CHECK_MSG(!contains(&resp, "confirmed for this repository"),
                    "job.remote_submit was still refused after the deploy terminated: %s",
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    atlas_buf_free(&job_uid);
    env_close(&e);
}

/* --- (i): the four forbidden names -------------------------------------------- */

static void test_forbidden_names_answer_unknown_method(void) {
    env e;
    env_open(&e);

    char gw_policy_text[4096], orch_policy_text[4096];
    build_policies(&e, gw_policy_text, sizeof gw_policy_text, orch_policy_text,
                   sizeof orch_policy_text);
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    static const char *const FORBIDDEN[] = {
        "deploy.remote_apply", "deploy.remote_install", "deploy.remote_restart",
        "deploy.remote_run",
    };
    for (size_t i = 0; i < sizeof FORBIDDEN / sizeof FORBIDDEN[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        char params[128];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.deploy_token);
        ipc_call(sock, FORBIDDEN[i], params, &resp);
        T_CHECK_MSG(contains(&resp, "unknown method"), "%s did not answer unknown method: %s",
                    FORBIDDEN[i], atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- a zeroed policy offers none of the six names --------------------------- */

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

    static const char *const NAMES[] = {
        "deploy.remote_propose", "deploy.remote_get",       "deploy.remote_list",
        "deploy.remote_cancel",  "deploy.remote_challenge", "deploy.remote_confirm",
    };
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, NAMES[i], "{}", &resp);
        T_CHECK_MSG(contains(&resp, "unknown method"), "%s did not answer unknown method: %s",
                    NAMES[i], atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- IMPORTANT fix round 1, item 4: per-method-group offering ---------------- */

/* A policy naming a submit key but no deploy key -- unlike every other test
 * in this file, which uses `build_policies`'s submit-key-and-deploy-key
 * shape. `atlas_server_remote_deploy_policy_ready` used to OR the two pools,
 * so this policy made all six names reachable, and `deploy.remote_challenge`/
 * `_confirm` then fell through to this file's own "this connection may not
 * manage deploys" usage refusal instead of `unknown method` -- the wrong
 * refusal for a capability that, per `gwpolicy.h`'s own comment at
 * `remote_deploy_key`, is supposed to be "offered to nobody" when the key is
 * absent. `deploy.remote_propose`/`_get`/`_list`/`_cancel` must still be
 * reachable (offered on the submit pool alone); `_challenge`/`_confirm` must
 * answer `unknown method`. */
static void test_submit_only_policy_offers_four_and_hides_challenge_and_confirm(void) {
    env e;
    env_open(&e);

    char gw_policy_text[4096];
    (void)snprintf(gw_policy_text, sizeof gw_policy_text,
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
                   "remote_submit_max_active = 4\n"
                   "remote_submit_max_per_day = 20\n"
                   "remote_submit_gate = true\n",
                   (long)getuid(), e.submit_id);
    char orch_policy_text[4096];
    (void)snprintf(orch_policy_text, sizeof orch_policy_text,
                   "dispatcher_uid = 1\n"
                   "submitter_uid = %ld\n"
                   "repo = proj\n"
                   "driver = fake\n"
                   "driver = fake-repo\n"
                   "mode = patch\n"
                   "worker_root = /tmp\n",
                   (long)getuid());
    const char *gw_path = write_policy_file(&e, "gateway.conf", gw_policy_text);
    const char *orch_path = write_policy_file(&e, "orch.conf", orch_policy_text);

    fx_daemon d;
    fx_daemon_init(&d);
    atlas_err err;
    atlas_err_init(&err);
    T_REQUIRE(gwd_start(&e, gw_path, orch_path, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    /* Offered: not "unknown method", whatever else they say (each is missing
     * a required field or, for `_list`, needs none and simply succeeds). */
    static const char *const OFFERED[] = {
        "deploy.remote_propose", "deploy.remote_get", "deploy.remote_list", "deploy.remote_cancel",
    };
    for (size_t i = 0; i < sizeof OFFERED / sizeof OFFERED[0]; i++) {
        char params[128];
        (void)snprintf(params, sizeof params, "{\"token\":\"%s\"}", e.submit_token);
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, OFFERED[i], params, &resp);
        T_CHECK_MSG(!contains(&resp, "unknown method"),
                    "%s should be offered on a submit-only policy: %s", OFFERED[i],
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    /* Hidden: exactly "unknown method", the deploy pool is empty. */
    static const char *const HIDDEN[] = {"deploy.remote_challenge", "deploy.remote_confirm"};
    for (size_t i = 0; i < sizeof HIDDEN / sizeof HIDDEN[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        ipc_call(sock, HIDDEN[i], "{}", &resp);
        T_CHECK_MSG(contains(&resp, "unknown method"),
                    "%s should answer unknown method on a submit-only policy: %s", HIDDEN[i],
                    atlas_buf_cstr(&resp));
        atlas_buf_free(&resp);
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    env_close(&e);
}

/* --- TESTS table ---------------------------------------------------------- */

static const atlas_test TESTS[] = {
    {"gateway.auth derives the right scope for each key (fix round 1, item 2)",
     test_gateway_auth_derives_the_right_scope_for_each_key},
    {"propose from a seeded SUCCEEDED job, and dry_run refused",
     test_propose_from_seeded_job_and_dry_run_refused},
    {"a deploy credential with a stored scope is refused at challenge (fix round 1, item 5)",
     test_deploy_credential_with_a_stored_scope_is_refused_at_challenge},
    {"challenge, confirm, the two queue files, scope and prefix refusals",
     test_challenge_confirm_and_queue_files},
    {"startup ingest, a malformed result, and root's hand-written ABANDONED escape",
     test_startup_ingest_malformed_and_abandoned},
    {"the watcher tick ingests a result written while the daemon runs",
     test_tick_ingest_while_running},
    {"two pending .res files in one ingest pass are both ingested without crashing "
     "(fix round 1, item 1)",
     test_ingest_pass_sorts_two_pending_res_files_without_crashing},
    {"confirm is refused while any job anywhere is not terminal",
     test_confirm_refused_while_a_job_is_not_terminal},
    {"both submission write points refuse a root job while a deploy is CONFIRMED",
     test_root_submission_refused_while_deploy_confirmed},
    {"the four forbidden deploy.remote_* names answer unknown method",
     test_forbidden_names_answer_unknown_method},
    {"a zeroed gateway policy answers unknown method for all six names",
     test_zeroed_policy_answers_unknown_method},
    {"a submit-only policy offers four names and hides challenge/confirm "
     "(fix round 1, item 4)",
     test_submit_only_policy_offers_four_and_hides_challenge_and_confirm},
};

ATLAS_TEST_MAIN("deploy_rpc", TESTS)

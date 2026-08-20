/* Atlas - A12.0: the four plan methods on the one client surface.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `plan.create`, `plan.revision_add`, `plan.get` and `plan.list` join the
 * existing orchestration *client* group. There is no new group, no operator-uid
 * entry, no MCP tool and no gateway route, and the dispatcher group is
 * untouched. Four claims are under test, and they are separable on purpose.
 *
 *   **The plan travels the client surface and nothing else.** The four names are
 *   in `ORCH_CLIENT_METHODS[]`, they are gated by `require_submitter` like
 *   `job.submit`, and a machine with no orchestration policy refuses all four
 *   with the same honest sentence. The `dispatch.` group answers `unknown
 *   method` from an ordinary connection exactly as it did.
 *
 *   **A plan has no status anybody can write.** Every verb somebody would reach
 *   for to settle, accept, block or complete a plan answers `unknown method`,
 *   from a live daemon. That is the season's authority-by-absence claim, and
 *   this is where it is asserted rather than described.
 *
 *   **Only a planner-role job's own stored artifact becomes a revision.** The
 *   binding refusals are driven through the real edge: a malformed planner job
 *   identifier is refused *at the edge* before it reaches a sentence that would
 *   echo it, an executor-driver job is refused, a job with no artifact is
 *   refused, and a job carrying somebody else's correlation is refused.
 *
 *   **A refused document survives the writer thread.** A parse refusal is a
 *   typed answer — the sentence and the line travel apart, in the error
 *   document's `detail` object — and it has to cross the writer's completion
 *   handshake to get there. A wrapper that handed the result back only on
 *   success would lose it, which is the one bug this shape invites.
 *
 * ## What is real here and what is not
 *
 * Two layers, for the reason `tests/test_a11_run.c` states about its own
 * substitution.
 *
 * The **live fixture daemon** cases speak over a real socket to a real daemon.
 * That daemon has no root-owned orchestration policy — an unprivileged uid
 * cannot create one anywhere, which is the point — so what it proves is
 * registration, the honest refusal and the negative enumeration.
 *
 * The **edge** cases call `atlas_server_dispatch`, which `daemon_internal.h`
 * exposes so the protocol can be tested without a socket. Everything above the
 * socket is real: the method table, the peer uid, `require_submitter`, the
 * policy, the writer thread, the write point, the parser and the database. What
 * is substituted is the carriage of bytes and the *provenance* of the policy —
 * the parse seam reads one this process wrote, and sets `state` itself, which no
 * production path can do. `tests/test_orch_model.c` is what proves the loader
 * refuses a policy this process could have written.
 *
 * Nothing here starts a worker process, calls a model, opens a credential or
 * touches the real index, socket or registry.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/driver.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/orchpolicy.h"
#include "atlas/plan.h"
#include "atlas/safetext.h"
#include "atlas/sha256.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "db/db_internal.h"
#include "ipc/server_internal.h"
#include "orch/policy_internal.h"
#include "support/fixture.h"

/* --- the live daemon: what the protocol offers and what it must not ---------- */

static void expect_unknown(const char *socket, const char *method, const char *params) {
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    (void)atlas_ipc_call(socket, method, params, &resp, &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "unknown method") != NULL,
                "%s answered something other than unknown: %s", method, atlas_buf_cstr(&resp));
    atlas_buf_free(&resp);
}

/* Every name a plan method would have if A12.0 had grown one that decides
 * something. A plan's status is derived on every read from stored rows; there is
 * no column, no compare-and-swap and no verb, and the absence is the guarantee.
 * Asked of a live daemon, and required to answer `unknown method` rather than
 * merely to fail — A7's argument, which is why those methods were deleted rather
 * than left refusing. */
static const char *const FORBIDDEN_PLAN_METHODS[] = {
    "plan.settle",   "plan.accept",     "plan.block",    "plan.complete",
    "plan.status",   "plan.status_set", "plan.approve",  "plan.authorize",
    "plan.apply",    "plan.commit",     "plan.push",     "plan.cancel_all",
    "plan.compile",  "plan.execute",    "plan.run",      "plan.revision_set",
    "Plan.Settle",   "PLAN.SETTLE",     "plan_settle",   "plan.delete",
    "plan.remove",   "plan.task_add",   "plan.bind",     "plan.job_bind",
};

/* The four that do exist, with a parameter set that would be complete if the
 * policy permitted anything. */
static const struct {
    const char *method;
    const char *params;
} PLAN_METHODS[] = {
    {"plan.create", "{\"repo\":\"proj\",\"goal\":\"make it work\"}"},
    {"plan.revision_add",
     "{\"plan\":\"p0123456789abcdef0123456789abcdef0\",\"planner_job\":"
     "\"j0123456789abcdef0123456789abcdef\",\"reason\":\"INITIAL\",\"rev_no\":1}"},
    {"plan.get", "{\"plan\":\"p0123456789abcdef0123456789abcdef0\"}"},
    {"plan.list", "{}"},
};

static void test_the_plan_methods_are_registered_and_refuse_honestly(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture fx;
    T_OK(fx_open(&fx, &err), &err);
    T_OK(fx_init_repo(&fx, fx_repo(&fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&fx, fx_repo(&fx), &err), &err);
    T_OK(fx_commit(&fx, fx_repo(&fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&fx), "repo", "add", fx_repo(&fx),
                             "--name", "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    fx_daemon d;
    fx_daemon_init(&d);
    T_REQUIRE(fx_daemon_start(&fx, &d, &err) == ATLAS_OK);
    T_REQUIRE(fx_daemon_wait_ready(&d, 15000, &err) == ATLAS_OK);
    const char *sock = atlas_buf_cstr(&d.socket);

    /* The four exist: the client group is dispatchable by name, and a daemon
     * with no policy says so rather than pretending the method is missing. A
     * caller can then tell a disabled deployment from a binary too old to have
     * the method, which is the distinction the client group is honest about. */
    for (size_t i = 0; i < sizeof PLAN_METHODS / sizeof PLAN_METHODS[0]; i++) {
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_err e2;
        atlas_err_init(&e2);
        (void)atlas_ipc_call(sock, PLAN_METHODS[i].method, PLAN_METHODS[i].params, &resp, &e2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "orchestration is not enabled") != NULL,
                    "%s on a policy-less machine answered: %s", PLAN_METHODS[i].method,
                    atlas_buf_cstr(&resp));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL,
                    "%s succeeded with orchestration disabled", PLAN_METHODS[i].method);
        atlas_buf_free(&resp);
    }

    /* And no verb that would settle one exists at all, including when the
     * request claims an identity for itself. */
    for (size_t i = 0; i < sizeof FORBIDDEN_PLAN_METHODS / sizeof FORBIDDEN_PLAN_METHODS[0]; i++) {
        expect_unknown(sock, FORBIDDEN_PLAN_METHODS[i], "{}");
        expect_unknown(sock, FORBIDDEN_PLAN_METHODS[i],
                       "{\"uid\":0,\"role\":\"dispatcher\",\"authority\":\"GRANTED\"}");
    }

    /* The dispatcher group is still not reachable from an ordinary connection,
     * and adding a client-group namespace did not put anything in it. */
    static const char *const DISPATCHER_METHODS[] = {
        "dispatch.lease", "dispatch.heartbeat", "dispatch.event", "dispatch.complete",
        "dispatch.plan", "dispatch.plan_revision",
    };
    for (size_t i = 0; i < sizeof DISPATCHER_METHODS / sizeof DISPATCHER_METHODS[0]; i++) {
        expect_unknown(sock, DISPATCHER_METHODS[i], "{}");
    }

    fx_daemon_stop(&d, false);
    fx_daemon_free(&d);
    fx_close(&fx);
}

/* --- the table ------------------------------------------------------------- */

static bool table_has(const atlas_method_entry *t, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(t[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_the_four_names_are_in_the_client_group_and_nowhere_else(void) {
    size_t nc = 0, nd = 0;
    const atlas_method_entry *c = atlas_server_orch_client_methods(&nc);
    const atlas_method_entry *d = atlas_server_orch_dispatch_methods(&nd);

    for (size_t i = 0; i < sizeof PLAN_METHODS / sizeof PLAN_METHODS[0]; i++) {
        T_CHECK_MSG(table_has(c, nc, PLAN_METHODS[i].method),
                    "%s is not in the orchestration client group", PLAN_METHODS[i].method);
        T_CHECK_MSG(!table_has(d, nd, PLAN_METHODS[i].method),
                    "%s is in the dispatcher group", PLAN_METHODS[i].method);
    }
    /* The two namespaces stay disjoint: `plan.` is a client name and nothing in
     * the dispatcher group carries it. */
    for (size_t k = 0; k < nd; k++) {
        T_CHECK_MSG(strncmp(d[k].name, "plan.", 5u) != 0,
                    "dispatcher method \"%s\" is in the plan namespace", d[k].name);
    }
    /* And no member of either group carries a verb that would mean authority.
     * The plan names are read, create and add — none of them decides. */
    static const char *const VERBS[] = {"settle", "accept", "block",     "approve",
                                        "apply",  "commit", "push",      "authorize",
                                        "grant",  "resolve"};
    for (size_t g = 0; g < 2; g++) {
        const atlas_method_entry *t = g == 0 ? c : d;
        size_t n = g == 0 ? nc : nd;
        for (size_t i = 0; i < n; i++) {
            for (size_t v = 0; v < sizeof VERBS / sizeof VERBS[0]; v++) {
                T_CHECK_MSG(strstr(t[i].name, VERBS[v]) == NULL,
                            "orchestration method \"%s\" contains the verb \"%s\"", t[i].name,
                            VERBS[v]);
            }
        }
    }
}

/* --- the edge, without a socket ---------------------------------------------
 *
 * `atlas_server_dispatch` takes a request payload, a peer uid and a peer pid and
 * produces a response payload. Everything the socket would have carried is
 * carried here instead, and everything above it — the method table, the policy,
 * `require_submitter`, the writer thread, the write point, the parser and the
 * database — is the shipped code.
 */

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_buf identity;
    atlas_buf commit;
} env;

typedef struct edge {
    env e;
    atlas_workers *workers;
    atlas_writer *writer;
    atlas_buf socket_path;
    atlas_server_ctx ctx;
    long long owner;      /* a submitter, and the plans' creator */
    long long other;      /* a submitter, and nobody's creator */
    long long outsider;   /* not a submitter at all */
} edge;

/* Registered through the CLI, because that is the only way a repository is ever
 * registered, and scanned, because the durable identity's lineage half comes
 * from ingested root commits. `tests/test_plan_db.c` opens the same way. */
static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);
    atlas_buf_init(&e->identity);
    atlas_buf_init(&e->commit);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx),  "--name",         "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    {
        const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "proj"};
        int code = -1;
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", &ri, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_db_repo_identity_hash(e->db, ri.id, &e->identity, &err), &err);
    T_REQUIRE(ri.scanned_head[0] != '\0');
    T_OK(atlas_buf_set_str(&e->commit, ri.scanned_head, &err), &err);
    atlas_repo_info_free(&ri);
}

static void env_close(env *e) {
    atlas_db_close(e->db);
    e->db = NULL;
    atlas_buf_free(&e->db_path);
    atlas_buf_free(&e->identity);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

/* The policy this deployment would have.
 *
 * Read through the parse seam, which is internal, sets no `state` and enables
 * nothing: the one `ENABLED` assignment in Atlas is the loader's last statement,
 * after a root-owned path walk this process cannot satisfy. The test sets the
 * state itself, and `tests/test_orch_model.c` is what proves the loader refuses
 * a policy an ordinary uid could have written. */
static void edge_policy(edge *g) {
    char text[512];
    (void)snprintf(text, sizeof text,
                   "dispatcher_uid = 993\n"
                   "submitter_uid = %lld\n"
                   "submitter_uid = %lld\n"
                   "repo = proj\n"
                   "mode = patch\n"
                   "driver = fake\n"
                   "driver = fake-plan\n"
                   "worker_root = /var/lib/atlas-worker\n",
                   g->owner, g->other);
    memset(&g->ctx.orchpolicy, 0, sizeof(g->ctx.orchpolicy));
    atlas_orchpolicy_reason why =
        atlas_orchpolicy_parse_bytes(text, strlen(text), &g->ctx.orchpolicy);
    T_REQUIRE(why == ATLAS_ORCHPOLICY_REASON_ACTIVE);
    g->ctx.orchpolicy.reason = why;
    g->ctx.orchpolicy.state = ATLAS_ORCHPOLICY_ENABLED;
}

static void edge_open(edge *g) {
    atlas_err err;
    atlas_err_init(&err);
    memset(g, 0, sizeof(*g));
    env_open(&g->e);
    atlas_buf_init(&g->socket_path);

    /* Three principals: the plan's creator, a second submitter who created
     * nothing, and a uid the policy does not name at all. */
    g->owner = (long long)getuid();
    g->other = g->owner + 1;
    g->outsider = g->owner + 2;

    T_OK(atlas_buf_appendf(&g->socket_path, &err, "%s/atlas.sock", fx_data_dir(&g->e.fx)), &err);
    T_OK(atlas_workers_start(2u, &g->workers, &err), &err);
    /* A real writer thread with its own writable handle, which is the only thing
     * that writes through these methods. Nothing listens on the socket path; the
     * writer records it in the daemon's own liveness row and never opens it. */
    T_OK(atlas_writer_start(atlas_buf_cstr(&g->e.db_path), atlas_buf_cstr(&g->socket_path),
                            g->workers, NULL, &g->writer, &err),
         &err);

    g->ctx.db_path = atlas_buf_cstr(&g->e.db_path);
    g->ctx.data_dir = fx_data_dir(&g->e.fx);
    g->ctx.socket_path = atlas_buf_cstr(&g->socket_path);
    g->ctx.writer = g->writer;
    edge_policy(g);
}

static void edge_close(edge *g) {
    atlas_writer_stop(g->writer);
    g->writer = NULL;
    atlas_workers_stop(g->workers);
    g->workers = NULL;
    atlas_buf_free(&g->socket_path);
    env_close(&g->e);
}

/* One request in, one response out. The response is parsed with the client's own
 * parser, so what the test reads is what a client would read. */
static atlas_ipc_response *call(edge *g, long long uid, const char *method, const char *params,
                                atlas_buf *raw) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf payload = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&payload, &err, "{\"id\":\"t\",\"method\":\"%s\",\"params\":%s}", method,
                           params),
         &err);
    atlas_buf_reset(raw);
    T_OK(atlas_server_dispatch(&g->ctx, payload.data, payload.len, uid, (int64_t)getpid(), raw,
                               &err),
         &err);
    atlas_buf_free(&payload);
    atlas_ipc_response *resp = NULL;
    T_OK(atlas_ipc_response_parse(raw->data, raw->len, &resp, &err), &err);
    T_REQUIRE(resp != NULL);
    return resp;
}

/* A successful call, with the response left for the caller to read. */
static atlas_ipc_response *call_ok(edge *g, long long uid, const char *method, const char *params,
                                   atlas_buf *raw) {
    atlas_ipc_response *r = call(g, uid, method, params, raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "%s failed: %s", method, atlas_buf_cstr(raw));
    return r;
}

/* A refused call, checked for the sentence it was refused with. */
static void call_refused(edge *g, long long uid, const char *method, const char *params,
                         const char *needle) {
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(g, uid, method, params, &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "%s succeeded and should not have: %s", method,
                atlas_buf_cstr(&raw));
    T_CHECK_MSG(strstr(atlas_ipc_response_message(r), needle) != NULL,
                "%s was refused with \"%s\", which does not carry \"%s\"", method,
                atlas_ipc_response_message(r), needle);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
}

/* --- the rows a plan's jobs are made of -------------------------------------
 *
 * Built through the real orchestration write point against the fixture's own
 * handle, exactly as `tests/test_plan_db.c` builds them: a submission, a lease, a
 * heartbeat to each forward phase, and a completion carrying the artifact. The
 * writer thread is idle throughout — every call below returns before the next
 * begins — so the two writable handles never overlap.
 */

static void apply_ok(edge *g, atlas_orch_op *op, atlas_orch_result *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_result_init(out);
    T_OK(atlas_orch_apply(g->e.db, op, out, &err), &err);
    atlas_orch_op_free(op);
    free(op);
}

static void submit_planner_job(edge *g, const char *driver, const char *correlation,
                               atlas_buf *job_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = g->owner;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = 1;
    op->spec.submitter_uid = g->owner;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, g->e.identity.data, g->e.identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, g->e.commit.data, g->e.commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, driver, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, "plan the work", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.correlation, correlation, &err), &err);
    op->spec.wall_timeout_ms = 3600000;
    op->spec.idle_timeout_ms = 900000;
    op->spec.max_attempts = 1;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    /* The check a real submission meets at the IPC edge, asserted on every
     * specification this file builds. */
    T_OK(atlas_orch_spec_validate(&op->spec, &err), &err);
    atlas_orch_result r;
    apply_ok(g, op, &r);
    T_OK(atlas_buf_set(job_out, r.job_uid.data, r.job_uid.len, &err), &err);
    atlas_orch_result_free(&r);
}

static void lease_to_running(edge *g, const char *job_uid, const char *driver,
                             atlas_buf *token_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->dispatcher_id, "d1", &err), &err);
    T_OK(atlas_buf_set_str(&op->job_uid, job_uid, &err), &err);
    {
        atlas_orch_argv want;
        atlas_orch_argv_init(&want);
        T_OK(atlas_orch_argv_push(&want, driver, strlen(driver), &err), &err);
        T_OK(atlas_orch_validations_encode(&want, 1u, &op->lease_drivers, &err), &err);
        atlas_orch_argv_free(&want);
    }
    atlas_orch_result grant;
    apply_ok(g, op, &grant);
    T_REQUIRE(grant.granted);
    T_OK(atlas_buf_set(token_out, grant.token.data, grant.token.len, &err), &err);
    atlas_orch_result_free(&grant);

    /* A job may not succeed straight out of LEASED — the transition table has no
     * such edge — so the attempt walks the phases a real dispatcher walks. */
    static const atlas_orch_state FORWARD[] = {ATLAS_ORCH_STATE_PREPARING,
                                               ATLAS_ORCH_STATE_RUNNING};
    for (size_t i = 0; i < sizeof FORWARD / sizeof FORWARD[0]; i++) {
        atlas_orch_op *hb = atlas_orch_op_new(ATLAS_ORCH_OP_HEARTBEAT);
        T_REQUIRE(hb != NULL);
        hb->peer_uid = 993;
        hb->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
        T_OK(atlas_buf_set_str(&hb->token, atlas_buf_cstr(token_out), &err), &err);
        hb->phase = FORWARD[i];
        atlas_orch_result r;
        apply_ok(g, hb, &r);
        atlas_orch_result_free(&r);
    }
}

/* What an attempt cost, as a dispatcher reports it. Every field is one Atlas
 * classified from a worker's final record; nothing here is a claim the worker
 * makes about itself. */
typedef struct spend {
    const char *model;
    int64_t cost_micro_usd;
    int64_t turns;
} spend;

static void finish_full(edge *g, const char *token, bool success, const char *name,
                        const char *bytes, const spend *s) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_COMPLETE);
    T_REQUIRE(op != NULL);
    op->peer_uid = 993;
    op->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&op->token, token, &err), &err);
    op->success = success;
    op->failure_reason = success ? ATLAS_ORCH_REASON_UNKNOWN : ATLAS_ORCH_REASON_WORKER_FAILURE;
    op->exit_kind = success ? ATLAS_ORCH_EXIT_OK : ATLAS_ORCH_EXIT_NONZERO;
    if (name != NULL) {
        op->artifacts = (atlas_orch_artifact *)calloc(1u, sizeof(atlas_orch_artifact));
        T_REQUIRE(op->artifacts != NULL);
        atlas_orch_artifact_init(&op->artifacts[0]);
        op->artifact_count = 1u;
        T_OK(atlas_buf_set_str(&op->artifacts[0].name, name, &err), &err);
        T_OK(atlas_buf_set_str(&op->artifacts[0].kind, "plan", &err), &err);
        size_t n = strlen(bytes);
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(bytes, n, hex);
        T_OK(atlas_buf_set_str(&op->artifacts[0].sha256, hex, &err), &err);
        op->artifacts[0].size_bytes = (int64_t)n;
        op->artifacts[0].content_stored = true;
        T_OK(atlas_buf_set(&op->artifacts[0].content, bytes, n, &err), &err);
    }
    if (s != NULL) {
        atlas_usage_init(&op->usage);
        op->usage.status = ATLAS_USAGE_AVAILABLE;
        (void)snprintf(op->usage.model, sizeof(op->usage.model), "%s", s->model);
        op->usage.has_cost = true;
        op->usage.cost_micro_usd = s->cost_micro_usd;
        op->usage.has_turns = true;
        op->usage.turns = s->turns;
    }
    atlas_orch_result r;
    apply_ok(g, op, &r);
    atlas_orch_result_free(&r);
}

static void finish_with_artifact(edge *g, const char *token, bool success, const char *name,
                                 const char *bytes) {
    finish_full(g, token, success, name, bytes, NULL);
}

/* A whole planner job, from submission to a stored artifact, bound to a plan by
 * the correlation the plan's own builder produces. Never spelled out here: the
 * format has one implementation and moving it must move no test. */
static void planner_job(edge *g, const char *plan_uid, int k, const char *driver,
                        const char *artifact_name, const char *bytes, bool success,
                        atlas_buf *job_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf corr = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_planner(plan_uid, k, &corr, &err), &err);
    submit_planner_job(g, driver, atlas_buf_cstr(&corr), job_out);
    atlas_buf tok = ATLAS_BUF_INIT;
    lease_to_running(g, atlas_buf_cstr(job_out), driver, &tok);
    finish_with_artifact(g, atlas_buf_cstr(&tok), success, artifact_name, bytes);
    atlas_buf_free(&tok);
    atlas_buf_free(&corr);
}

/* One of a revision's tasks, submitted as the plan driver would submit it: the
 * correlation the plan's own builder produces, and nothing else binding it. */
static void task_job(edge *g, const char *plan_uid, int rev_no, const char *key,
                     const spend *s, atlas_buf *job_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf corr = ATLAS_BUF_INIT;
    T_OK(atlas_plan_correlation_task(plan_uid, rev_no, key, &corr, &err), &err);
    submit_planner_job(g, "fake", atlas_buf_cstr(&corr), job_out);
    atlas_buf tok = ATLAS_BUF_INIT;
    lease_to_running(g, atlas_buf_cstr(job_out), "fake", &tok);
    finish_full(g, atlas_buf_cstr(&tok), true, NULL, "", s);
    atlas_buf_free(&tok);
    atlas_buf_free(&corr);
}

/* One stage, one tree task and one workspace sibling: with `max_parallel = 2`
 * the side bound is `min(3, max_parallel - 1)`, which is one. */
static const char PLAN_ONE_STAGE[] = "atlas-plan-1\n"
                                     "stage: 1\n"
                                     "task: build\n"
                                     "kind: tree\n"
                                     "title: Build the thing\n"
                                     "gate: make test\n"
                                     "prompt<<\n"
                                     "do the work\n"
                                     ">>\n"
                                     "task: notes\n"
                                     "kind: side\n"
                                     "title: Take notes\n"
                                     "prompt<<\n"
                                     "write it down\n"
                                     ">>\n";

/* The gate floor as `job.submit`'s `--gate` wire form: one length-prefixed argv
 * encoding per command. `make pass` is two arguments. */
#define GATE_FLOOR_MAKE_PASS "[\"1:2:4:make,4:pass,\"]"

#define CREATE_PARAMS                                                                    \
    "{\"repo\":\"proj\",\"goal\":\"make the thing work\",\"parallel\":2,\"gate_floor\":" \
    GATE_FLOOR_MAKE_PASS "}"

static void create_plan(edge *g, atlas_buf *uid_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call_ok(g, g->owner, "plan.create", CREATE_PARAMS, &raw);
    const char *uid = NULL;
    T_REQUIRE(atlas_ipc_result_str(r, "plan", &uid) && uid != NULL);
    T_OK(atlas_buf_set_str(uid_out, uid, &err), &err);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
}

/* --- the cases -------------------------------------------------------------- */

static void test_a_connection_the_policy_does_not_name_reaches_nothing(void) {
    edge g;
    edge_open(&g);
    /* The name is found — the client group is dispatchable by name — and the
     * method refuses for itself. Reaching a name is not being allowed to use
     * it. */
    for (size_t i = 0; i < sizeof PLAN_METHODS / sizeof PLAN_METHODS[0]; i++) {
        call_refused(&g, g.outsider, PLAN_METHODS[i].method, PLAN_METHODS[i].params,
                     "this connection may not submit or read jobs");
    }
    /* And with orchestration disabled, every one of them says so instead — the
     * honest answer, which is what lets an operator tell a disabled deployment
     * from a binary too old to have the method. */
    g.ctx.orchpolicy.state = ATLAS_ORCHPOLICY_DISABLED;
    for (size_t i = 0; i < sizeof PLAN_METHODS / sizeof PLAN_METHODS[0]; i++) {
        call_refused(&g, g.owner, PLAN_METHODS[i].method, PLAN_METHODS[i].params,
                     "orchestration is not enabled");
    }
    edge_close(&g);
}

static void test_a_plan_is_created_read_back_and_listed(void) {
    edge g;
    edge_open(&g);
    atlas_buf uid = ATLAS_BUF_INIT;
    create_plan(&g, &uid);
    T_CHECK_MSG(uid.len == 33 && atlas_buf_cstr(&uid)[0] == 'p', "a plan identifier reads \"%s\"",
                atlas_buf_cstr(&uid));

    atlas_buf params = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&params, &err, "{\"plan\":\"%s\"}", atlas_buf_cstr(&uid)), &err);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call_ok(&g, g.owner, "plan.get", atlas_buf_cstr(&params), &raw);
    const char *v = NULL;
    T_CHECK(atlas_ipc_result_str(r, "plan", &v) && strcmp(v, atlas_buf_cstr(&uid)) == 0);
    T_CHECK(atlas_ipc_result_str(r, "repo", &v) && strcmp(v, "proj") == 0);
    /* A plan with no planner job yet is PLANNING, and its status is derived on
     * this read from stored rows rather than read out of a column. */
    T_CHECK(atlas_ipc_result_str(r, "status", &v) &&
            strcmp(v, atlas_plan_status_name(ATLAS_PLAN_STATUS_PLANNING)) == 0);
    /* The operator's own words, labelled with how they were encoded. */
    T_CHECK(atlas_ipc_result_str(r, "goal_encoding", &v) && strcmp(v, "atlas-safe-1") == 0);
    T_CHECK(atlas_ipc_result_str(r, "goal", &v) && strcmp(v, "make the thing work") == 0);
    /* The floor, one command per line — and the line separator is escaped by the
     * safe encoding like every other C0 byte, which is what `atlas-safe-1` beside
     * it means and what a renderer prints as-is. Asserted through the decoder
     * rather than by respelling the escape: the claim is that the block round
     * trips, not that a newline happens to be spelled `%0A`. */
    T_CHECK(atlas_ipc_result_str(r, "gate_floor_text", &v));
    {
        atlas_buf decoded = ATLAS_BUF_INIT;
        T_OK(atlas_text_decode_safe(v, strlen(v), &decoded, &err), &err);
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&decoded), "make pass\n") == 0,
                    "the gate floor decoded to \"%s\"", atlas_buf_cstr(&decoded));
        T_CHECK_MSG(strchr(v, '\n') == NULL, "an encoded block carried a raw newline");
        atlas_buf_free(&decoded);
    }
    T_CHECK(atlas_ipc_result_str(r, "gate_floor_encoding", &v) && strcmp(v, "atlas-safe-1") == 0);
    int64_t n = -1;
    T_CHECK(atlas_ipc_result_int(r, "gate_floor_count", &n) && n == 1);
    T_CHECK(atlas_ipc_result_int(r, "max_parallel", &n) && n == 2);
    T_CHECK(atlas_ipc_result_int(r, "rev_no", &n) && n == 0);
    T_CHECK(atlas_ipc_result_int(r, "planner_jobs_seen", &n) && n == 0);
    size_t len = 99;
    T_CHECK(atlas_ipc_result_arr_len(r, "revisions", &len) && len == 0);
    T_CHECK(atlas_ipc_result_arr_len(r, "tasks", &len) && len == 0);
    atlas_ipc_response_free(r);

    /* Listed, with the same derived status and nothing a second derivation could
     * disagree with. */
    r = call_ok(&g, g.owner, "plan.list", "{}", &raw);
    T_CHECK(atlas_ipc_result_int(r, "count", &n) && n == 1);
    bool more = true;
    T_CHECK(atlas_ipc_result_bool(r, "more", &more) && !more);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "plans", 0, "plan", &v) &&
            strcmp(v, atlas_buf_cstr(&uid)) == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "plans", 0, "repo", &v) && strcmp(v, "proj") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "plans", 0, "status", &v) &&
            strcmp(v, atlas_plan_status_name(ATLAS_PLAN_STATUS_PLANNING)) == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "plans", 0, "created_at", &v) && v[0] != '\0');
    atlas_ipc_response_free(r);

    /* A second submitter created nothing, so it has nothing to list and cannot
     * read the first one's plan. Whether it exists is itself information, so the
     * answer is "no such plan" rather than "forbidden". */
    r = call_ok(&g, g.other, "plan.list", "{}", &raw);
    T_CHECK(atlas_ipc_result_int(r, "count", &n) && n == 0);
    atlas_ipc_response_free(r);
    call_refused(&g, g.other, "plan.get", atlas_buf_cstr(&params), "no such plan");

    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    atlas_buf_free(&uid);
    edge_close(&g);
}

static void test_a_plan_creation_is_refused_rather_than_adjusted(void) {
    edge g;
    edge_open(&g);
    /* A repository the policy does not list, refused before the registry is
     * consulted. */
    call_refused(&g, g.owner, "plan.create",
                 "{\"repo\":\"elsewhere\",\"goal\":\"g\",\"gate_floor\":" GATE_FLOOR_MAKE_PASS "}",
                 "does not permit jobs against that repository");
    /* A repository the policy lists and the registry does not. */
    {
        edge h;
        edge_open(&h);
        (void)snprintf(h.ctx.orchpolicy.repos[0], sizeof(h.ctx.orchpolicy.repos[0]), "%s",
                       "ghost");
        call_refused(&h, h.owner, "plan.create",
                     "{\"repo\":\"ghost\",\"goal\":\"g\",\"gate_floor\":" GATE_FLOOR_MAKE_PASS "}",
                     "no repository named that is registered");
        edge_close(&h);
    }
    call_refused(&g, g.owner, "plan.create", "{\"goal\":\"g\"}",
                 "a plan names the repository it is for");
    call_refused(&g, g.owner, "plan.create", "{\"repo\":\"proj\"}", "a plan needs a goal");
    /* No gate floor at all. The operator brings the floor; a plan with none could
     * only ever be accepted on a model's word, so it is refused rather than
     * defaulted. */
    call_refused(&g, g.owner, "plan.create", "{\"repo\":\"proj\",\"goal\":\"g\"}",
                 "a plan needs at least one gate");
    /* And a parallelism outside the range is refused with the bound named, never
     * clamped. */
    call_refused(&g, g.owner, "plan.create",
                 "{\"repo\":\"proj\",\"goal\":\"g\",\"parallel\":99,\"gate_floor\":"
                 GATE_FLOOR_MAKE_PASS "}",
                 "tasks at once");
    edge_close(&g);
}

static void test_a_planner_jobs_artifact_becomes_a_revision(void) {
    edge g;
    edge_open(&g);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf uid = ATLAS_BUF_INIT;
    create_plan(&g, &uid);

    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&g, atlas_buf_cstr(&uid), 1, "fake-plan", ATLAS_PLAN_ARTIFACT_NAME,
                PLAN_ONE_STAGE, true, &job);

    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                           "\"rev_no\":1}",
                           atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
         &err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call_ok(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params),
                                    &raw);
    int64_t n = -1;
    T_CHECK(atlas_ipc_result_int(r, "rev_no", &n) && n == 1);
    T_CHECK(atlas_ipc_result_int(r, "tasks", &n) && n == 2);
    atlas_ipc_response_free(r);

    /* Offered a second time, the same ingest is refused rather than writing a
     * second revision holding the same document. */
    call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params), "was offered");

    /* And the plan now reads as one compiled revision with its two tasks. */
    atlas_buf get = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&get, &err, "{\"plan\":\"%s\"}", atlas_buf_cstr(&uid)), &err);
    r = call_ok(&g, g.owner, "plan.get", atlas_buf_cstr(&get), &raw);
    const char *v = NULL;
    T_CHECK(atlas_ipc_result_int(r, "rev_no", &n) && n == 1);
    T_CHECK(atlas_ipc_result_int(r, "planner_jobs_seen", &n) && n == 1);
    /* EXECUTING: a revision compiled and none of its tasks has been submitted. */
    T_CHECK(atlas_ipc_result_str(r, "status", &v) &&
            strcmp(v, atlas_plan_status_name(ATLAS_PLAN_STATUS_EXECUTING)) == 0);
    T_CHECK(atlas_ipc_result_str(r, "planner_job", &v) &&
            strcmp(v, atlas_buf_cstr(&job)) == 0);

    size_t len = 0;
    T_CHECK(atlas_ipc_result_arr_len(r, "revisions", &len) && len == 1);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "revisions", 0, "reason", &v) &&
            strcmp(v, "INITIAL") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "revisions", 0, "planner_job", &v) &&
            strcmp(v, atlas_buf_cstr(&job)) == 0);
    {
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(PLAN_ONE_STAGE, strlen(PLAN_ONE_STAGE), hex);
        T_CHECK(atlas_ipc_result_arr_obj_str(r, "revisions", 0, "sha256", &v) &&
                strcmp(v, hex) == 0);
    }
    /* The metadata carries no content: a list of revisions is not the place to
     * carry five plan documents. */
    T_CHECK(!atlas_ipc_result_arr_obj_str(r, "revisions", 0, "content", &v));

    T_CHECK(atlas_ipc_result_arr_len(r, "tasks", &len) && len == 2);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 0, "key", &v) && strcmp(v, "build") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 0, "kind", &v) && strcmp(v, "TREE") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 0, "title", &v) &&
            strcmp(v, "Build the thing") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 1, "key", &v) && strcmp(v, "notes") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 1, "kind", &v) && strcmp(v, "SIDE") == 0);
    /* Neither has been submitted, so neither names a job. An absent key reads as
     * what it is; an empty identifier would read as an identifier. */
    T_CHECK(!atlas_ipc_result_arr_obj_str(r, "tasks", 0, "job", &v));
    /* A model wrote both titles, and the array says so once. */
    T_CHECK(atlas_ipc_result_str(r, "task_title_encoding", &v) && strcmp(v, "atlas-safe-1") == 0);
    T_CHECK(atlas_ipc_result_str(r, "task_title_provenance", &v) &&
            strcmp(v, "UNTRUSTED_DATA") == 0);
    atlas_ipc_response_free(r);

    /* Asked for one revision by number, the document comes back — labelled and
     * safe-encoded exactly as `job.artifact` labels the bytes it came from. */
    atlas_buf show = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&show, &err, "{\"plan\":\"%s\",\"rev_no\":1}", atlas_buf_cstr(&uid)),
         &err);
    r = call_ok(&g, g.owner, "plan.get", atlas_buf_cstr(&show), &raw);
    T_CHECK(atlas_ipc_result_int(r, "content_rev_no", &n) && n == 1);
    T_CHECK(atlas_ipc_result_str(r, "content_encoding", &v) && strcmp(v, "atlas-safe-1") == 0);
    T_CHECK(atlas_ipc_result_str(r, "content_provenance", &v) &&
            strcmp(v, "UNTRUSTED_DATA") == 0);
    /* The planner's bytes, exactly — through the decoder, because the encoding is
     * what the label says it is and the claim is that nothing was lost. */
    T_CHECK(atlas_ipc_result_str(r, "content", &v));
    {
        atlas_buf decoded = ATLAS_BUF_INIT;
        T_OK(atlas_text_decode_safe(v, strlen(v), &decoded, &err), &err);
        T_CHECK_MSG(decoded.len == strlen(PLAN_ONE_STAGE) &&
                        memcmp(decoded.data, PLAN_ONE_STAGE, decoded.len) == 0,
                    "the revision's content came back as \"%s\"", atlas_buf_cstr(&decoded));
        atlas_buf_free(&decoded);
    }
    atlas_ipc_response_free(r);

    /* A revision that does not exist is a sentence, not an empty answer. */
    {
        atlas_buf gone = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&gone, &err, "{\"plan\":\"%s\",\"rev_no\":3}",
                               atlas_buf_cstr(&uid)),
             &err);
        call_refused(&g, g.owner, "plan.get", atlas_buf_cstr(&gone), "holds no revision 3");
        atlas_buf_free(&gone);
    }

    atlas_buf_free(&show);
    atlas_buf_free(&get);
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    atlas_buf_free(&job);
    atlas_buf_free(&uid);
    edge_close(&g);
}

/* Every binding refusal, driven through the edge. */
static void test_only_a_planner_jobs_own_artifact_can_become_a_revision(void) {
    edge g;
    edge_open(&g);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf uid = ATLAS_BUF_INIT;
    create_plan(&g, &uid);
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_buf job = ATLAS_BUF_INIT;

    /* A planner job identifier that is not one. Refused at the edge, before it
     * reaches a sentence that would quote it back — the bytes a caller sent must
     * not travel into a terminal on a refusal path. */
    static const char *const MALFORMED[] = {
        "notajob",
        "j0123456789abcdef0123456789abcde",   /* one hex short */
        "j0123456789ABCDEF0123456789ABCDEF",  /* uppercase */
        "r0123456789abcdef0123456789abcdef",  /* a run identifier */
        "",
    };
    for (size_t i = 0; i < sizeof MALFORMED / sizeof MALFORMED[0]; i++) {
        atlas_buf p = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&p, &err,
                               "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                               "\"rev_no\":1}",
                               atlas_buf_cstr(&uid), MALFORMED[i]),
             &err);
        atlas_buf raw = ATLAS_BUF_INIT;
        atlas_ipc_response *r = call(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&p), &raw);
        T_CHECK_MSG(!atlas_ipc_response_ok(r), "\"%s\" was accepted as a job identifier",
                    MALFORMED[i]);
        if (MALFORMED[i][0] != '\0') {
            T_CHECK_MSG(strstr(atlas_ipc_response_message(r), MALFORMED[i]) == NULL,
                        "the refusal echoed the identifier it refused: %s",
                        atlas_ipc_response_message(r));
        }
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        atlas_buf_free(&p);
    }

    /* A reason outside the closed vocabulary, and a revision number outside the
     * bound. Neither is left at a zero for the write point to discover. */
    {
        atlas_buf p = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&p, &err,
                               "{\"plan\":\"%s\",\"planner_job\":"
                               "\"j0123456789abcdef0123456789abcdef\",\"reason\":\"BECAUSE\","
                               "\"rev_no\":1}",
                               atlas_buf_cstr(&uid)),
             &err);
        call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&p), "for no other reason");
        atlas_buf_reset(&p);
        T_OK(atlas_buf_appendf(&p, &err,
                               "{\"plan\":\"%s\",\"planner_job\":"
                               "\"j0123456789abcdef0123456789abcdef\",\"reason\":\"INITIAL\","
                               "\"rev_no\":9}",
                               atlas_buf_cstr(&uid)),
             &err);
        call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&p), "a revision is numbered");
        atlas_buf_free(&p);
    }

    /* An **executor** job's artifact can never become a plan, whatever it
     * contains: the role is asked of the driver the job stored. */
    planner_job(&g, atlas_buf_cstr(&uid), 1, "fake", ATLAS_PLAN_ARTIFACT_NAME, PLAN_ONE_STAGE,
                true, &job);
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                           "\"rev_no\":1}",
                           atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
         &err);
    call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params), "is not a planner");

    /* A planner job that produced no artifact at all. */
    atlas_buf_reset(&job);
    planner_job(&g, atlas_buf_cstr(&uid), 2, "fake-plan", NULL, "", true, &job);
    atlas_buf_reset(&params);
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                           "\"rev_no\":1}",
                           atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
         &err);
    call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params), "produced no artifact");

    /* A planner job of a *different* plan, offered to this one. The correlation
     * is the whole of the binding, and it is not a capability: presenting one
     * authorises nothing. */
    {
        atlas_buf other_plan = ATLAS_BUF_INIT;
        create_plan(&g, &other_plan);
        atlas_buf_reset(&job);
        planner_job(&g, atlas_buf_cstr(&other_plan), 1, "fake-plan", ATLAS_PLAN_ARTIFACT_NAME,
                    PLAN_ONE_STAGE, true, &job);
        atlas_buf_reset(&params);
        T_OK(atlas_buf_appendf(&params, &err,
                               "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                               "\"rev_no\":1}",
                               atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
             &err);
        call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params),
                     "is not a planner job of plan");
        /* And the second plan belongs to the same principal, so this is a
         * binding refusal rather than a scoping one — the two are separable. */
        call_refused(&g, g.other, "plan.revision_add", atlas_buf_cstr(&params), "no such plan");
        atlas_buf_free(&other_plan);
    }

    /* A planner job that failed. Atlas' own classification of the attempt, not a
     * claim the worker made about itself. */
    atlas_buf_reset(&job);
    planner_job(&g, atlas_buf_cstr(&uid), 3, "fake-plan", ATLAS_PLAN_ARTIFACT_NAME,
                PLAN_ONE_STAGE, false, &job);
    atlas_buf_reset(&params);
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                           "\"rev_no\":1}",
                           atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
         &err);
    call_refused(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params), "only a job that");

    atlas_buf_free(&params);
    atlas_buf_free(&job);
    atlas_buf_free(&uid);
    edge_close(&g);
}

/* **A gate floor is held to exactly the strictness the submit path holds.**
 *
 * The wire form is `job.submit`'s, and its decoder is a reader of *stored* text:
 * it checks nothing about an argument, because what it reads was checked when it
 * was written. So a floor that arrived over the wire has to be pushed back
 * through `atlas_orch_argv_push` and held to the two command-level rules
 * `atlas_orch_spec_validate` applies, or a plan could carry a floor that every
 * one of its stage-run submissions would later refuse — with nobody watching.
 * Each case below is a floor a job submission would refuse. */
static void test_a_gate_floor_is_held_to_the_submit_paths_strictness(void) {
    edge g;
    edge_open(&g);
    atlas_err err;
    atlas_err_init(&err);

    static const struct {
        const char *floor; /* the `gate_floor` array, as JSON */
        const char *needle;
    } BAD[] = {
        /* A control byte inside an argument. `pass` is five bytes. */
        {"[\"1:2:4:make,5:pa\\u0001ss,\"]", "must be printable ASCII"},
        /* An empty argument. */
        {"[\"1:2:4:make,0:,\"]", "may not be empty"},
        /* A command with no argv at all. */
        {"[\"1:0:\"]", "has no argv"},
        /* A program named by path. The allowlist is the executor's; this is the
         * shape rule, and it is the one a job submission applies. */
        {"[\"1:1:7:/bin/sh,\"]", "names a program by path"},
        /* And a floor of more commands than a task may hold. */
        {"[\"1:1:4:make,\",\"1:1:4:make,\",\"1:1:4:make,\",\"1:1:4:make,\",\"1:1:4:make,\","
         "\"1:1:4:make,\",\"1:1:4:make,\",\"1:1:4:make,\",\"1:1:4:make,\"]",
         "at most 8 commands"},
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_buf p = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&p, &err,
                               "{\"repo\":\"proj\",\"goal\":\"g\",\"gate_floor\":%s}",
                               BAD[i].floor),
             &err);
        call_refused(&g, g.owner, "plan.create", atlas_buf_cstr(&p), BAD[i].needle);
        atlas_buf_free(&p);
    }

    /* An argument one byte past the bound. Built rather than written out, so the
     * case moves with `ATLAS_ORCH_ARG_MAX` instead of pinning today's number. */
    {
        size_t over = (size_t)ATLAS_ORCH_ARG_MAX + 1u;
        char *arg = (char *)malloc(over + 1u);
        T_REQUIRE(arg != NULL);
        memset(arg, 'x', over);
        arg[over] = '\0';
        atlas_buf p = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&p, &err,
                               "{\"repo\":\"proj\",\"goal\":\"g\",\"gate_floor\":"
                               "[\"1:2:4:make,%zu:%s,\"]}",
                               over, arg),
             &err);
        call_refused(&g, g.owner, "plan.create", atlas_buf_cstr(&p), "may be at most");
        atlas_buf_free(&p);
        free(arg);
    }

    /* Nothing above created a plan: a refused floor is refused before anything is
     * queued, so the writer never saw one of them. */
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call_ok(&g, g.owner, "plan.list", "{}", &raw);
    int64_t n = -1;
    T_CHECK_MSG(atlas_ipc_result_int(r, "count", &n) && n == 0,
                "a refused gate floor left %lld plan(s) behind", (long long)n);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);

    /* And the floor that is accepted is the one a job submission would accept,
     * which is the point: the two paths hold one wire form to one strictness. */
    {
        atlas_buf uid = ATLAS_BUF_INIT;
        create_plan(&g, &uid);
        T_CHECK(uid.len == 33);
        atlas_buf_free(&uid);
    }
    edge_close(&g);
}

/* A task that has become a job carries what it cost — and a task that has not
 * carries nothing at all, because an absent measurement is not a zero. */
static void test_a_task_that_became_a_job_reports_what_it_cost(void) {
    edge g;
    edge_open(&g);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf uid = ATLAS_BUF_INIT;
    create_plan(&g, &uid);

    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&g, atlas_buf_cstr(&uid), 1, "fake-plan", ATLAS_PLAN_ARTIFACT_NAME,
                PLAN_ONE_STAGE, true, &job);
    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                           "\"rev_no\":1}",
                           atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
         &err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call_ok(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params),
                                    &raw);
    atlas_ipc_response_free(r);

    /* The revision's *second* task becomes a job that reports a measurement; the
     * first stays unsubmitted. */
    atlas_buf side = ATLAS_BUF_INIT;
    const spend s = {"a-model", 1234, 7};
    task_job(&g, atlas_buf_cstr(&uid), 1, "notes", &s, &side);

    atlas_buf get = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&get, &err, "{\"plan\":\"%s\"}", atlas_buf_cstr(&uid)), &err);
    r = call_ok(&g, g.owner, "plan.get", atlas_buf_cstr(&get), &raw);
    const char *v = NULL;
    int64_t n = -1;
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 1, "job", &v) &&
            strcmp(v, atlas_buf_cstr(&side)) == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 1, "job_state", &v) &&
            strcmp(v, atlas_orch_state_name(ATLAS_ORCH_STATE_SUCCEEDED)) == 0);
    T_CHECK(atlas_ipc_result_arr_obj_str(r, "tasks", 1, "usage_model", &v) &&
            strcmp(v, "a-model") == 0);
    T_CHECK(atlas_ipc_result_arr_obj_int(r, "tasks", 1, "usage_cost_micro_usd", &n) && n == 1234);
    T_CHECK(atlas_ipc_result_arr_obj_int(r, "tasks", 1, "usage_turns", &n) && n == 7);
    /* A side task settles nothing on its own, so it names no run even though the
     * job it became belongs to one. */
    T_CHECK(!atlas_ipc_result_arr_obj_str(r, "tasks", 1, "run", &v));
    /* The task nobody submitted reports no job and no cost. Absent is not zero:
     * a task that never ran did not run for free. */
    T_CHECK(!atlas_ipc_result_arr_obj_str(r, "tasks", 0, "job", &v));
    T_CHECK(!atlas_ipc_result_arr_obj_str(r, "tasks", 0, "usage_model", &v));
    T_CHECK(!atlas_ipc_result_arr_obj_int(r, "tasks", 0, "usage_cost_micro_usd", &n));
    atlas_ipc_response_free(r);

    atlas_buf_free(&get);
    atlas_buf_free(&side);
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    atlas_buf_free(&job);
    atlas_buf_free(&uid);
    edge_close(&g);
}

/* **A refused document is a typed answer, and it has to survive the writer.**
 *
 * The sentence and the line travel apart, in the error document's `detail`
 * object, because the plan driver composes a retry prompt out of both and must
 * never have to parse Atlas' prose to recover a number. They are filled on the
 * write point's *failure* path, so a writer wrapper that handed the result back
 * only on success would drop them — which is the one bug this shape invites. */
static void test_a_refused_document_travels_as_a_sentence_and_a_line(void) {
    edge g;
    edge_open(&g);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf uid = ATLAS_BUF_INIT;
    create_plan(&g, &uid);

    /* A document whose third line names something that is not a task field. The
     * refusal is `atlas_plan_parse`'s, and it is about a line. */
    static const char BAD[] = "atlas-plan-1\n"
                              "stage: 1\n"
                              "nonsense: here\n";
    atlas_buf job = ATLAS_BUF_INIT;
    planner_job(&g, atlas_buf_cstr(&uid), 1, "fake-plan", ATLAS_PLAN_ARTIFACT_NAME, BAD, true,
                &job);

    atlas_buf params = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&params, &err,
                           "{\"plan\":\"%s\",\"planner_job\":\"%s\",\"reason\":\"INITIAL\","
                           "\"rev_no\":1}",
                           atlas_buf_cstr(&uid), atlas_buf_cstr(&job)),
         &err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params), &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "an unparseable plan was accepted: %s",
                atlas_buf_cstr(&raw));

    const char *refusal = NULL;
    int64_t line = -1;
    T_CHECK_MSG(atlas_ipc_error_detail_str(r, "refusal", &refusal),
                "the refusal carried no typed sentence: %s", atlas_buf_cstr(&raw));
    T_CHECK_MSG(atlas_ipc_error_detail_int(r, "line", &line),
                "the refusal carried no line: %s", atlas_buf_cstr(&raw));
    T_CHECK_MSG(line == 3, "the refusal named line %lld", (long long)line);
    T_CHECK_MSG(refusal != NULL && refusal[0] != '\0', "the refusal sentence was empty");
    /* The line is a field, not something to be recovered from the prose. */
    T_CHECK_MSG(strstr(atlas_ipc_response_message(r), refusal) != NULL,
                "the message and the typed sentence disagree: \"%s\" against \"%s\"",
                atlas_ipc_response_message(r), refusal);
    atlas_ipc_response_free(r);

    /* No revision was written, and the plan says so: the planner job is seen, no
     * revision names it, and the derived state asks for a replan. Asked again,
     * the same stored bytes produce the same refusal. */
    atlas_buf get = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&get, &err, "{\"plan\":\"%s\"}", atlas_buf_cstr(&uid)), &err);
    r = call_ok(&g, g.owner, "plan.get", atlas_buf_cstr(&get), &raw);
    int64_t n = -1;
    T_CHECK(atlas_ipc_result_int(r, "rev_no", &n) && n == 0);
    T_CHECK(atlas_ipc_result_int(r, "planner_jobs_seen", &n) && n == 1);
    bool replan = false;
    T_CHECK(atlas_ipc_result_bool(r, "replan_wanted", &replan) && replan);
    atlas_ipc_response_free(r);

    r = call(&g, g.owner, "plan.revision_add", atlas_buf_cstr(&params), &raw);
    T_CHECK(!atlas_ipc_response_ok(r));
    line = -1;
    T_CHECK(atlas_ipc_error_detail_int(r, "line", &line) && line == 3);
    atlas_ipc_response_free(r);

    /* And an ordinary refusal — one whose sentence is the whole answer — carries
     * no detail at all, so a caller can tell a planner's mistake from its own. */
    r = call(&g, g.owner, "plan.get", "{}", &raw);
    T_CHECK(!atlas_ipc_response_ok(r));
    T_CHECK_MSG(!atlas_ipc_error_detail_str(r, "refusal", &refusal),
                "a caller's own mistake carried a document refusal");
    atlas_ipc_response_free(r);

    atlas_buf_free(&get);
    atlas_buf_free(&raw);
    atlas_buf_free(&params);
    atlas_buf_free(&job);
    atlas_buf_free(&uid);
    edge_close(&g);
}

static const atlas_test TESTS[] = {
    {"the plan methods are registered and refuse honestly",
     test_the_plan_methods_are_registered_and_refuse_honestly},
    {"the four names are in the client group and nowhere else",
     test_the_four_names_are_in_the_client_group_and_nowhere_else},
    {"a connection the policy does not name reaches nothing",
     test_a_connection_the_policy_does_not_name_reaches_nothing},
    {"a plan is created, read back and listed", test_a_plan_is_created_read_back_and_listed},
    {"a plan creation is refused rather than adjusted",
     test_a_plan_creation_is_refused_rather_than_adjusted},
    {"a planner job's artifact becomes a revision",
     test_a_planner_jobs_artifact_becomes_a_revision},
    {"only a planner job's own artifact can become a revision",
     test_only_a_planner_jobs_own_artifact_can_become_a_revision},
    {"a gate floor is held to the submit path's strictness",
     test_a_gate_floor_is_held_to_the_submit_paths_strictness},
    {"a task that became a job reports what it cost",
     test_a_task_that_became_a_job_reports_what_it_cost},
    {"a refused document travels as a sentence and a line",
     test_a_refused_document_travels_as_a_sentence_and_a_line},
};

ATLAS_TEST_MAIN("plan_rpc", TESTS)

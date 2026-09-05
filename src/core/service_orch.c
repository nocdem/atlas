/* Atlas - A8: the `job` and `dispatcher` command behaviour.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every command here speaks to the daemon over the socket, and there is
 * deliberately **no offline path**. Orchestration state lives in the index,
 * `atlasd` is the only writer of it, and a CLI that fell back to opening the
 * database itself would be a second writer — the one thing A1 forbids, and
 * something A7.1 makes impossible anyway because no other account can open the
 * file. A client that cannot reach the daemon is told so and stops.
 *
 * The service layer formats nothing. Results reach the CLI through
 * `atlas_job_sink`, and the renderers decide how they look — the layering this
 * repository has kept since A0.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/dispatch.h"
#include "atlas/driver.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas/rundriver.h"
#include "atlas/service.h"
#include "atlas/syspolicy.h"
#include "service_internal.h"

/* --- talking to the daemon --------------------------------------------------
 *
 * A12.0 moved this out of `static` and into `service_internal.h`, unchanged, so
 * `src/core/service_plan.c` speaks the protocol through the same function rather
 * than through a second copy of it. The declaration carries the contract; this
 * is still its one implementation. */

atlas_status atlas_service_orch_call(atlas_ctx *ctx, const char *method,
                                     atlas_service_build_fn build, void *ud,
                                     atlas_ipc_response **out, atlas_buf *raw, atlas_err *err) {
    *out = NULL;
    (void)ctx;
    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_status st = atlas_ipc_socket_path(&sock, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&sock);
        return st;
    }
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&sock);
        return st;
    }
    if (build != NULL) {
        st = build(j, ud, err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&sock), method, atlas_buf_cstr(&params), raw, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&sock);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_ipc_response_parse(raw->data, raw->len, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!atlas_ipc_response_ok(*out)) {
        return atlas_err_set(err, atlas_ipc_response_status(*out), "%s",
                             atlas_ipc_response_message(*out));
    }
    return ATLAS_OK;
}

/* Fills a render struct from one result object. Every string is borrowed from
 * the parsed response and is valid only until it is freed — the same rule every
 * row callback in Atlas follows. */
static void fill_render(const atlas_ipc_response *r, atlas_job_render *jr, bool detail) {
    memset(jr, 0, sizeof(*jr));
    jr->detail = detail;
    struct {
        const char **field;
        const char *key;
    } strs[] = {
        {&jr->job, "job"},        {&jr->state, "state"},
        {&jr->repo, "repo"},      {&jr->driver, "driver"},
        {&jr->commit, "commit"},  {&jr->created_at, "created_at"},
        {&jr->terminal_at, "terminal_at"}, {&jr->spec_digest, "spec_digest"},
        {&jr->task, "task"},      {&jr->run, "run"},
        {&jr->run_status, "run_status"}, {&jr->follow_up, "follow_up"},
        /* A14. The credential's bare 16-hex selector; absent for local jobs. */
        {&jr->key_id, "key_id"},
    };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        const char *v = NULL;
        if (atlas_ipc_result_str(r, strs[i].key, &v)) {
            *strs[i].field = v;
        }
    }
    (void)atlas_ipc_result_int(r, "attempts", &jr->attempts);
    (void)atlas_ipc_result_int(r, "max_attempts", &jr->max_attempts);
    (void)atlas_ipc_result_int(r, "seq", &jr->seq);
    (void)atlas_ipc_result_bool(r, "cancel_requested", &jr->cancel_requested);
    (void)atlas_ipc_result_bool(r, "duplicate", &jr->duplicate);
    (void)atlas_ipc_result_int(r, "worker_starts", &jr->worker_starts);
}

/* --- job submit -------------------------------------------------------------- */

/* A12.0 moved the gate split to `atlas_orch_gate_split` in `src/orch/orch.c`,
 * unchanged, so the `atlas-plan-1` parser splits a planner's `gate:` line the
 * way this splits an operator's `--gate`. The comment explaining why it is not
 * shell-like moved with it. */

static atlas_status build_submit(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_job_submit_opts *o = (const atlas_job_submit_opts *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", o->repo, err);
    if (st == ATLAS_OK) {
        /* The task is a submitter's own words and is sent verbatim. It is never
         * concatenated into anything, and the daemon stores it as
         * UNTRUSTED_DATA. */
        st = atlas_json_key_str(j, "task", o->task, err);
    }
    struct {
        const char *k;
        const char *v;
    } opt[] = {
        {"mode", o->mode},
        {"driver", o->driver},
        {"idempotency_key", o->idempotency_key},
        {"correlation", o->correlation},
        /* A10.1. Absent means off at the daemon, so an empty value is simply
         * not sent rather than sent as "off" — one spelling of the default. */
        {"memory", o->memory},
        /* A11.6. The task this one follows, which is what puts it in that task's
         * run. Absent means "this is a root task", so an empty value is not
         * sent: an empty identifier reads like an identifier. */
        {"parent", o->parent},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof opt / sizeof opt[0]; i++) {
        if (opt[i].v != NULL && opt[i].v[0] != '\0') {
            st = atlas_json_key_str(j, opt[i].k, opt[i].v, err);
        }
    }
    struct {
        const char *k;
        int64_t v;
    } nums[] = {
        {"wall_timeout_ms", o->wall_timeout_ms},
        {"idle_timeout_ms", o->idle_timeout_ms},
        {"max_attempts", o->max_attempts},
        /* A11.6. Absent means "not stated", which the daemon resolves to one.
         * Zero is therefore not sent — it is the same request as not asking. */
        {"parallel", o->max_parallel},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof nums / sizeof nums[0]; i++) {
        if (nums[i].v > 0) {
            st = atlas_json_key_int(j, nums[i].k, nums[i].v, err);
        }
    }
    /* A11.1. The gates, each sent as a length-prefixed argv encoding rather than
     * as a command line, so no element of one can be confused with a separator
     * whatever it contains. The split happened here, on ASCII spaces, with no
     * shell involved at any point. */
    if (st == ATLAS_OK && o->gate_count > 0) {
        st = atlas_json_key(j, "validation", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < o->gate_count; i++) {
            atlas_orch_argv one;
            atlas_orch_argv_init(&one);
            st = atlas_orch_gate_split(o->gates[i], &one, err);
            atlas_buf enc = ATLAS_BUF_INIT;
            if (st == ATLAS_OK) {
                st = atlas_orch_validations_encode(&one, 1u, &enc, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_json_str(j, atlas_buf_cstr(&enc), err);
            }
            atlas_buf_free(&enc);
            atlas_orch_argv_free(&one);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

atlas_status atlas_service_job_submit(atlas_ctx *ctx, const atlas_job_submit_opts *o,
                                      atlas_job_sink sink, void *ud, atlas_err *err) {
    if (o->repo == NULL || o->repo[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which repository?");
    }
    if (o->task == NULL || o->task[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a job needs task text");
    }
    if (o->max_parallel != 0 && o->parent != NULL && o->parent[0] != '\0') {
        /* A11.6. The same refusal the daemon gives, given here too so an
         * operator who typed both learns it without a round trip. It is stated
         * in the same words deliberately: two spellings of one rule read as two
         * rules. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "how many tasks a run may hold active is fixed when the run is "
                             "created, so it cannot be named on a task that joins one");
    }
    /* Copied rather than const-cast: a cast that discards a qualifier is right
     * today and wrong after one edit. */
    atlas_job_submit_opts local = *o;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(ctx, "job.submit", build_submit, &local, &resp, &raw, err);
    if (st == ATLAS_OK && sink != NULL) {
        atlas_job_render jr;
        fill_render(resp, &jr, false);
        st = sink(&jr, ud, err);
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* --- job get and cancel -------------------------------------------------------- */

static atlas_status build_job(atlas_json *j, void *ud, atlas_err *err) {
    return atlas_json_key_str(j, "job", *(const char *const *)ud, err);
}

static atlas_status one_job(atlas_ctx *ctx, const char *method, const char *job, bool detail,
                            atlas_job_sink sink, void *ud, atlas_err *err) {
    if (job == NULL || job[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which job?");
    }
    const char *job_copy = job;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(ctx, method, build_job, &job_copy, &resp, &raw, err);
    if (st == ATLAS_OK && sink != NULL) {
        atlas_job_render jr;
        fill_render(resp, &jr, detail);
        st = sink(&jr, ud, err);
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_job_get(atlas_ctx *ctx, const char *job, atlas_job_sink sink,
                                   void *ud, atlas_err *err) {
    return one_job(ctx, "job.get", job, true, sink, ud, err);
}

atlas_status atlas_service_job_cancel(atlas_ctx *ctx, const char *job, atlas_job_sink sink,
                                      void *ud, atlas_err *err) {
    return one_job(ctx, "job.cancel", job, false, sink, ud, err);
}

/* --- job list ------------------------------------------------------------------ */

typedef struct list_args {
    int64_t after;
    int64_t limit;
    bool remote;
} list_args;

static atlas_status build_list(atlas_json *j, void *ud, atlas_err *err) {
    list_args *a = (list_args *)ud;
    atlas_status st = ATLAS_OK;
    if (a->after > 0) {
        st = atlas_json_key_int(j, "after", a->after, err);
    }
    if (st == ATLAS_OK && a->limit > 0) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    if (st == ATLAS_OK && a->remote) {
        st = atlas_json_key_bool(j, "remote", true, err);
    }
    return st;
}

typedef struct list_sink_ctx {
    atlas_job_sink sink;
    void *ud;
} list_sink_ctx;

static atlas_status forward_row(const atlas_ipc_response *r, size_t index, void *ud,
                                atlas_err *err);

atlas_status atlas_service_job_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                    bool remote, atlas_job_sink sink, void *ud,
                                    int64_t *count_out, bool *more_out, atlas_err *err) {
    list_args a = {after, limit, remote};
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(ctx, "job.list", build_list, &a, &resp, &raw, err);
    if (st == ATLAS_OK) {
        int64_t n = 0;
        (void)atlas_ipc_result_int(resp, "count", &n);
        if (count_out != NULL) {
            *count_out = n;
        }
        bool more = false;
        (void)atlas_ipc_result_bool(resp, "more", &more);
        if (more_out != NULL) {
            *more_out = more;
        }
        list_sink_ctx lc = {sink, ud};
        for (int64_t i = 0; st == ATLAS_OK && i < n; i++) {
            st = forward_row(resp, (size_t)i, &lc, err);
        }
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* The response's `jobs` array is read element by element through the typed
 * accessors. There is no path that copies bytes out of the response document
 * into another one. */
static atlas_status forward_row(const atlas_ipc_response *r, size_t index, void *ud,
                                atlas_err *err) {
    list_sink_ctx *lc = (list_sink_ctx *)ud;
    atlas_job_render jr;
    memset(&jr, 0, sizeof(jr));
    jr.in_list = true;
    if (!atlas_ipc_result_arr_obj_str(r, "jobs", index, "job", &jr.job)) {
        return ATLAS_OK;
    }
    (void)atlas_ipc_result_arr_obj_str(r, "jobs", index, "state", &jr.state);
    (void)atlas_ipc_result_arr_obj_str(r, "jobs", index, "repo", &jr.repo);
    (void)atlas_ipc_result_arr_obj_str(r, "jobs", index, "driver", &jr.driver);
    (void)atlas_ipc_result_arr_obj_str(r, "jobs", index, "created_at", &jr.created_at);
    (void)atlas_ipc_result_arr_obj_int(r, "jobs", index, "attempts", &jr.attempts);
    /* A14. key_id is present on remote jobs; absent on local ones. Borrowed. */
    (void)atlas_ipc_result_arr_obj_str(r, "jobs", index, "key_id", &jr.key_id);
    return lc->sink != NULL ? lc->sink(&jr, lc->ud, err) : ATLAS_OK;
}

/* --- the dispatcher -------------------------------------------------------------- */

/* The driver filter is derived from the policy's own driver list rather than
 * from a flag, so the two dispatchers partition the queue by construction:
 * whatever needs a model goes to the operator's, everything else to the
 * worker's. A driver added to the policy lands on exactly one of them.
 *
 * A12.0 shared it. The plan driver carries a planner job and a workspace sibling
 * in its own foreground process, which means asking for the *same* partition
 * this uid's background dispatcher would ask for — and a second derivation of
 * "which drivers are mine" is a second answer to which process may run what. */
atlas_status atlas_service_orch_driver_filter(const atlas_orchpolicy *pol, bool model_partition,
                                              char *out, size_t out_size, atlas_err *err) {
    size_t flen = 0;
    out[0] = '\0';
    for (size_t i = 0; i < pol->driver_count; i++) {
        const atlas_driver *d = atlas_driver_find(pol->drivers[i]);
        bool wants_model = (d != NULL && d->needs_live_model);
        if (wants_model != model_partition) {
            continue;
        }
        /* A11.1. A driver that works in the registered repository's own tree is
         * never on a *dispatched* filter, however the policy lists it. A
         * dispatcher provisions a workspace and would run the driver somewhere
         * it was not meant to run, then complete the task — settling a run with
         * no gate having run where the changes are.
         *
         * The daemon refuses such a grant to an unfiltered lease as well, and
         * both checks are needed: that one catches "give me anything", this one
         * catches a filter that names the driver because the policy did. Only
         * the operator's foreground run driver asks for these by name — which is
         * still true under A12.0, because the plan driver reaches a repo-tree
         * task through `atlas_rundriver_run` and never through this filter. */
        if (atlas_orch_driver_is_repo_tree(pol->drivers[i])) {
            continue;
        }
        int n = snprintf(out + flen, out_size - flen, "%s%s", flen > 0 ? "," : "",
                         pol->drivers[i]);
        if (n > 0 && (size_t)n < out_size - flen) {
            flen += (size_t)n;
        }
    }
    if (out[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the orchestration policy configures no %s driver for this "
                             "dispatcher to run",
                             model_partition ? "model" : "non-model");
    }
    return ATLAS_OK;
}

atlas_status atlas_service_dispatcher_run(bool once, FILE *log, atlas_err *err) {
    /* Both policies are read here, from their compiled-in root-owned paths, and
     * neither can be overridden by an environment variable or a flag. A
     * dispatcher that could choose its own policy would not be constrained by
     * one. */
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    if (sp.state != ATLAS_SYSPOLICY_SYSTEM) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no system deployment policy is active (%s), so there is no shared "
                             "daemon for a dispatcher to serve",
                             atlas_syspolicy_reason_name(sp.reason));
    }
    atlas_orchpolicy op;
    atlas_orchpolicy_load(&op);
    if (op.state != ATLAS_ORCHPOLICY_ENABLED) {
        /* A refusal to start, not a loop that idles: an idling dispatcher looks
         * healthy in `systemctl status` while doing nothing, and an operator who
         * mistyped a policy would never learn it. */
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "orchestration is not enabled (%s: %s)",
                             atlas_orchpolicy_reason_name(op.reason),
                             atlas_orchpolicy_reason_explain(op.reason));
    }
    /* The dispatcher must be a uid the policy names. Checked here so a
     * misconfigured unit fails at startup with a clear message rather than
     * being refused on every lease attempt for the lifetime of the service.
     *
     * Which of the two it is decides three things — the workspace root, the
     * driver filter and whose credentials a model driver uses — and all three
     * come from the root-owned policy rather than from the process deciding for
     * itself. */
    const bool is_worker = atlas_orchpolicy_is_dispatcher(&op, (long long)getuid());
    const bool is_model = atlas_orchpolicy_is_model_dispatcher(&op, (long long)getuid());
    if (!is_worker && !is_model) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this process runs as uid %lld, which the orchestration policy does "
                             "not name as a dispatcher",
                             (long long)getuid());
    }

    char filter[256];
    atlas_status fs = atlas_service_orch_driver_filter(&op, is_model, filter, sizeof filter, err);
    if (fs != ATLAS_OK) {
        return fs;
    }

    char id[128];
    (void)snprintf(id, sizeof id, "atlas-dispatcher/%lld", (long long)getpid());

    atlas_dispatch_opts o;
    memset(&o, 0, sizeof(o));
    o.socket_path = sp.socket_path;
    o.worker_root = is_model ? op.model_worker_root : op.worker_root;
    o.drivers = filter;
    o.operator_session = is_model && op.model_uses_operator_session;
    o.dispatcher_id = id;
    o.poll_ms = 1000;
    o.max_backoff_ms = 30000;
    /* Comfortably below the lease lifetime, so a healthy job never loses its
     * lease while working. */
    o.heartbeat_ms = ATLAS_ORCH_LEASE_MS / 4;
    o.live_model = op.live_model;
    /* A12.0. Which model each role runs under. Read from the root-owned policy
     * here and carried unchanged; the driver's role picks one of the two at the
     * attempt. Empty is unset and passes no flag at all. */
    o.models.planner = op.planner_model;
    o.models.executor = op.executor_model;
    o.max_iterations = once ? 1 : 0;
    o.log = log;
    return atlas_dispatch_run(&o, err);
}

/* --- A11.1: `atlas job run` -------------------------------------------------
 *
 * The socket transport for the run driver, and the command that uses it.
 *
 * The driver speaks in `atlas_orch_op`s and reads `atlas_orch_result`s; this
 * carries them over the same socket, the same method groups and the same
 * `SO_PEERCRED` checks as every other orchestration message. Nothing here
 * relaxes anything: it translates, and what the IPC edge refuses it still
 * refuses.
 */

typedef struct run_xport {
    atlas_buf sock;
} run_xport;

/* The lease request. */
static atlas_status build_run_lease(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_orch_op *op = (const atlas_orch_op *)ud;
    atlas_status st = atlas_json_key_str(j, "dispatcher", atlas_buf_cstr(&op->dispatcher_id), err);
    if (st == ATLAS_OK && op->job_uid.len > 0) {
        st = atlas_json_key_str(j, "job", atlas_buf_cstr(&op->job_uid), err);
    }
    if (st == ATLAS_OK && op->lease_drivers.len > 0) {
        atlas_orch_argv one[1];
        atlas_orch_argv_init(&one[0]);
        size_t n = 0;
        st = atlas_orch_validations_decode(atlas_buf_cstr(&op->lease_drivers), one, 1u, &n, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key(j, "driver", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && n > 0 && i < one[0].count; i++) {
            st = atlas_json_str(j, atlas_buf_cstr(&one[0].args[i]), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
        atlas_orch_argv_free(&one[0]);
    }
    return st;
}

static atlas_status build_run_heartbeat(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_orch_op *op = (const atlas_orch_op *)ud;
    atlas_status st = atlas_json_key_str(j, "token", atlas_buf_cstr(&op->token), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "phase", atlas_orch_state_name(op->phase), err);
    }
    return st;
}

/* The completion. Every field is either an Atlas classification or a bounded
 * excerpt; there is no field here a model could fill in even if it could reach
 * the socket, because none of them is copied from anything it produced. */
static atlas_status build_run_complete(atlas_json *j, void *ud, atlas_err *err) {
    const atlas_orch_op *op = (const atlas_orch_op *)ud;
    atlas_status st = atlas_json_key_str(j, "token", atlas_buf_cstr(&op->token), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "success", op->success, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "exit_kind", atlas_orch_exit_kind_name(op->exit_kind), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "exit_code", op->exit_code, err);
    }
    if (st == ATLAS_OK && !op->success) {
        st = atlas_json_key_str(j, "reason", atlas_orch_reason_name(op->failure_reason), err);
    }
    if (st == ATLAS_OK && op->failed_gate >= 0) {
        st = atlas_json_key_int(j, "failed_gate", op->failed_gate, err);
    }
    if (st == ATLAS_OK && op->failure_detail.len > 0) {
        st = atlas_json_key_str(j, "detail", atlas_buf_cstr(&op->failure_detail), err);
    }
    if (st == ATLAS_OK && op->driver_version.len > 0) {
        st = atlas_json_key_str(j, "driver_version", atlas_buf_cstr(&op->driver_version), err);
    }
    /* A10.0. What the attempt cost, as one encoded summary rather than a dozen
     * loose numbers, so the wire form and the durable file are the same bytes
     * and cannot disagree about what a field is called.
     *
     * `dispatch.complete` is in the dispatcher-only method group, selected by
     * `SO_PEERCRED` and disjoint from the client group, so this travels on a
     * message a model payload cannot send at all — which is what keeps "a
     * worker cannot write its own cost" true by the transport rather than by a
     * check. Omitted entirely when nothing was measured: an absent key reads as
     * UNKNOWN on the far side, which is the same answer and one fewer thing to
     * get wrong. */
    if (st == ATLAS_OK && op->usage.status != ATLAS_USAGE_UNKNOWN) {
        atlas_buf enc = ATLAS_BUF_INIT;
        st = atlas_usage_encode(&op->usage, &enc, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "usage", atlas_buf_cstr(&enc), err);
        }
        atlas_buf_free(&enc);
    }
    if (st == ATLAS_OK && op->artifact_count > 0) {
        st = atlas_json_key(j, "artifact", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; st == ATLAS_OK && i < op->artifact_count; i++) {
            const atlas_orch_artifact *a = &op->artifacts[i];
            atlas_buf ent = ATLAS_BUF_INIT;
            st = atlas_buf_appendf(&ent, err, "%s\x1f%s\x1f%s\x1f%lld",
                                   atlas_buf_cstr(&a->name), atlas_buf_cstr(&a->kind),
                                   atlas_buf_cstr(&a->sha256), (long long)a->size_bytes);
            if (st == ATLAS_OK && a->content_stored) {
                st = atlas_buf_append(&ent, "\x1f", 1u, err);
                static const char D[] = "0123456789abcdef";
                for (size_t k = 0; st == ATLAS_OK && k < a->content.len; k++) {
                    unsigned char c = (unsigned char)a->content.data[k];
                    char pair[2] = {D[c >> 4], D[c & 0x0fu]};
                    st = atlas_buf_append(&ent, pair, 2u, err);
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_json_str(j, atlas_buf_cstr(&ent), err);
            }
            atlas_buf_free(&ent);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    /* A12.1 T13. The run driver's own touched-paths observation. `touched_paths`
     * is omitted entirely when it gathered nothing -- a refusal before the
     * worker ran, a moved HEAD, or a gather that ran and failed all leave the
     * buffer empty, and there is nothing to send.
     *
     * A12.1 fix round, I1. `touched_complete` used to be nested inside the
     * `touched_paths.len > 0` guard above and sent only when `false`, which
     * meant a driver that gathered nothing and explicitly set
     * `touched_complete = false` (a failed gather, `src/orch/rundriver.c`'s
     * `gather_touched_paths` failure path) sent neither key, and the far side
     * read the absent `touched_complete` as its old default, `true` -- not
     * "conservative": `true` is the completeness claim, and the driver's own
     * `false` never left this process.
     *
     * The task directive for this fix is stronger than "send it when false":
     * send the key **always**, and make the absent reading the conservative
     * one. Both halves are now true. This key is new in A12.1 (no older
     * client's meaning moves by strengthening it), `atlas_orch_op_new`
     * (`src/db/db_orch.c`) and `outcome_init` (`src/orch/rundriver.c`) both
     * now default `touched_complete` to `false`, and it is emitted
     * unconditionally here, so the driver's classification always reaches the
     * wire -- explicitly, never by relying on a default anywhere in the
     * chain. (`reliance_check`, `src/db/db_orch.c`, does not read this value
     * when `touched_paths` is empty either way -- see I2 there -- but the
     * wire contract must still say what actually happened, for any future
     * reader, not only today's one.) */
    if (st == ATLAS_OK && op->touched_paths.len > 0) {
        st = atlas_json_key_str(j, "touched_paths", atlas_buf_cstr(&op->touched_paths), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(j, "touched_complete", op->touched_complete, err);
    }
    return st;
}

static atlas_status xport_apply(void *ud, const atlas_orch_op *op, atlas_orch_result *out,
                                atlas_err *err) {
    run_xport *x = (run_xport *)ud;
    const char *method = NULL;
    atlas_service_build_fn build = NULL;
    switch (op->kind) {
    case ATLAS_ORCH_OP_LEASE:
        method = "dispatch.lease";
        build = build_run_lease;
        break;
    case ATLAS_ORCH_OP_HEARTBEAT:
        method = "dispatch.heartbeat";
        build = build_run_heartbeat;
        break;
    case ATLAS_ORCH_OP_COMPLETE:
        method = "dispatch.complete";
        build = build_run_complete;
        break;
    /* The run driver issues three operations and no others. The rest are
     * written out rather than left to a `default:`, so adding an operation to
     * the vocabulary makes this stop compiling instead of silently widening
     * what the run driver may ask the daemon for. */
    case ATLAS_ORCH_OP_NONE:
    case ATLAS_ORCH_OP_SUBMIT:
    case ATLAS_ORCH_OP_CANCEL:
    case ATLAS_ORCH_OP_EVENT:
    case ATLAS_ORCH_OP_RECOVER: break;
    }
    if (method == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the run driver does not carry that operation");
    }

    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = build(j, (void *)(uintptr_t)(const void *)op, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&x->sock), method, atlas_buf_cstr(&params), &raw, err);
    }
    atlas_buf_free(&params);
    atlas_ipc_response *resp = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_response_parse(raw.data, raw.len, &resp, err);
        if (st != ATLAS_OK) {
            /* A12.0. Bytes that are not a response document are not an answer,
             * whatever else they are: the request may have been applied and this
             * caller cannot tell. Same claim as a read that timed out, so the
             * same mark — and never the mark on the line below, which is the
             * daemon answering. */
            atlas_err_mark_transport(err);
        }
    }
    if (st == ATLAS_OK && !atlas_ipc_response_ok(resp)) {
        st = atlas_err_set(err, atlas_ipc_response_status(resp), "%s: %s", method,
                           atlas_ipc_response_message(resp));
    }
    if (st == ATLAS_OK) {
        const char *v = NULL;
        (void)atlas_ipc_result_bool(resp, "granted", &out->granted);
        (void)atlas_ipc_result_bool(resp, "cancel_requested", &out->cancel_requested);
        (void)atlas_ipc_result_int(resp, "attempt", &out->attempt_no);
        (void)atlas_ipc_result_int(resp, "wall_timeout_ms", &out->wall_timeout_ms);
        (void)atlas_ipc_result_int(resp, "idle_timeout_ms", &out->idle_timeout_ms);
        (void)atlas_ipc_result_int(resp, "max_output_bytes", &out->max_output_bytes);
        (void)atlas_ipc_result_int(resp, "worker_starts", &out->worker_starts);
        struct {
            atlas_buf *dst;
            const char *key;
        } strs[] = {
            {&out->job_uid, "job"},        {&out->token, "token"},
            {&out->repo_root, "repo_root"}, {&out->source_commit, "commit"},
            {&out->mode, "mode"},          {&out->driver, "driver"},
            {&out->task_text, "task"},     {&out->validations, "validations"},
            {&out->run_uid, "run"},        {&out->follow_up_job_uid, "follow_up"},
            {&out->memory_package, "memory"},
            /* A12.1 T13. */
            {&out->context_pack, "context_pack"},
            {&out->context_pack_status, "context_pack_status"},
        };
        for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
            if (atlas_ipc_result_str(resp, strs[i].key, &v) && v != NULL) {
                st = atlas_buf_set_str(strs[i].dst, v, err);
            }
        }
        /* A10.1. An unrecognised or absent mode leaves UNKNOWN, and an absent
         * package leaves empty. An older daemon answering a newer CLI therefore
         * produces a run with no memory rather than an error — A9.2.5's rule for
         * an absent key, and the conservative value in this direction is "the
         * worker was shown nothing". */
        if (st == ATLAS_OK && atlas_ipc_result_str(resp, "memory_mode", &v) && v != NULL) {
            (void)atlas_orch_memory_mode_parse(v, &out->memory_mode);
        }
        if (st == ATLAS_OK && atlas_ipc_result_str(resp, "memory_digest", &v) && v != NULL) {
            (void)snprintf(out->memory_digest, sizeof out->memory_digest, "%s", v);
        }
        if (st == ATLAS_OK && atlas_ipc_result_str(resp, "state", &v) && v != NULL) {
            (void)atlas_orch_state_parse(v, &out->state);
        }
        /* An unrecognised run status leaves UNKNOWN, which is not terminal and
         * settles nothing. A newer daemon answering an older CLI must never
         * make this one read as an ending. */
        if (st == ATLAS_OK && atlas_ipc_result_str(resp, "run_status", &v) && v != NULL) {
            (void)atlas_orch_run_status_parse(v, &out->run_status);
        }
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

typedef struct run_get_req {
    const char *run;
} run_get_req;

static atlas_status build_run_get(atlas_json *j, void *ud, atlas_err *err) {
    return atlas_json_key_str(j, "run", ((const run_get_req *)ud)->run, err);
}

static atlas_status xport_run_get(void *ud, const char *run_uid, atlas_orch_run_view *out,
                                  bool *found, atlas_err *err) {
    run_xport *x = (run_xport *)ud;
    *found = false;
    memset(out, 0, sizeof(*out));
    run_get_req rq = {run_uid};
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = build_run_get(j, &rq, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&x->sock), "job.run_status", atlas_buf_cstr(&params), &raw,
                            err);
    }
    atlas_buf_free(&params);
    atlas_ipc_response *resp = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_response_parse(raw.data, raw.len, &resp, err);
        if (st != ATLAS_OK) {
            atlas_err_mark_transport(err);
        }
    }
    if (st == ATLAS_OK && !atlas_ipc_response_ok(resp)) {
        st = atlas_err_set(err, atlas_ipc_response_status(resp), "%s",
                           atlas_ipc_response_message(resp));
    }
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (atlas_ipc_result_str(resp, "run", &v) && v != NULL) {
            (void)snprintf(out->run_uid, sizeof(out->run_uid), "%s", v);
            *found = true;
        }
        if (atlas_ipc_result_str(resp, "root", &v) && v != NULL) {
            (void)snprintf(out->root_job_uid, sizeof(out->root_job_uid), "%s", v);
        }
        if (atlas_ipc_result_str(resp, "status", &v) && v != NULL) {
            (void)atlas_orch_run_status_parse(v, &out->status);
        }
        if (atlas_ipc_result_str(resp, "created_at", &v) && v != NULL) {
            (void)snprintf(out->created_at, sizeof(out->created_at), "%s", v);
        }
        if (atlas_ipc_result_str(resp, "active", &v) && v != NULL) {
            (void)snprintf(out->active_job_uid, sizeof(out->active_job_uid), "%s", v);
        }
        if (atlas_ipc_result_str(resp, "active_state", &v) && v != NULL) {
            (void)atlas_orch_state_parse(v, &out->active_state);
        }
        /* A11.6. Both keys are left at zero when they are absent, which is what
         * a newer CLI reads from a daemon that predates them. Zero is the
         * conservative value in both directions — it is never a claim that a run
         * has no active task, and never a claim that its bound is nothing,
         * because a stored run's bound is at least one. A9.2.5's rule for an
         * absent key, and never an error. */
        int64_t n = 0;
        if (atlas_ipc_result_int(resp, "active_count", &n)) {
            out->active_count = n;
        }
        if (atlas_ipc_result_int(resp, "max_parallel", &n)) {
            out->max_parallel = n;
        }
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

typedef struct job_get_req {
    const char *job;
} job_get_req;

static atlas_status build_job_get(atlas_json *j, void *ud, atlas_err *err) {
    return atlas_json_key_str(j, "job", ((const job_get_req *)ud)->job, err);
}

/* A12.0. One task, read by uid, over the method that already exists for it.
 *
 * `job.get` is in the client group beside `job.run_status`, which this transport
 * has always used, and it carries the same guard — the connection may read a
 * task it submitted. A run driver's tasks are its operator's own, and a
 * follow-up keeps its parent's `submitter_uid`, so both resolve.
 *
 * Only the members the run driver reads are filled. A caller that needs more
 * adds it here, deliberately, rather than finding a half-populated view and
 * assuming the rest. An unrecognised or absent state stays UNKNOWN, which is not
 * terminal and settles nothing — A9.2.5's rule for an absent key, and the
 * conservative value on this path. */
static atlas_status xport_job_get(void *ud, const char *job_uid, atlas_orch_job_view *out,
                                  bool *found, atlas_err *err) {
    run_xport *x = (run_xport *)ud;
    *found = false;
    job_get_req rq = {job_uid};
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = build_job_get(j, &rq, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&x->sock), "job.get", atlas_buf_cstr(&params), &raw,
                            err);
    }
    atlas_buf_free(&params);
    atlas_ipc_response *resp = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_response_parse(raw.data, raw.len, &resp, err);
        if (st != ATLAS_OK) {
            atlas_err_mark_transport(err);
        }
    }
    if (st == ATLAS_OK && !atlas_ipc_response_ok(resp)) {
        st = atlas_err_set(err, atlas_ipc_response_status(resp), "%s",
                           atlas_ipc_response_message(resp));
    }
    if (st == ATLAS_OK) {
        const char *v = NULL;
        if (atlas_ipc_result_str(resp, "job", &v) && v != NULL) {
            (void)snprintf(out->job_uid, sizeof(out->job_uid), "%s", v);
            *found = true;
        }
        if (atlas_ipc_result_str(resp, "state", &v) && v != NULL) {
            (void)atlas_orch_state_parse(v, &out->state);
        }
        int64_t n = 0;
        if (atlas_ipc_result_int(resp, "attempts", &n)) {
            out->attempts_started = n;
        }
        (void)atlas_ipc_result_bool(resp, "cancel_requested", &out->cancel_requested);
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_job_run_status(atlas_ctx *ctx, const char *run, atlas_job_sink sink,
                                          void *ud, atlas_err *err) {
    if (run == NULL || run[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "which run?");
    }
    run_get_req rq = {run};
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = atlas_service_orch_call(ctx, "job.run_status", build_run_get, &rq, &resp, &raw, err);
    if (st == ATLAS_OK && sink != NULL) {
        atlas_job_render jr;
        memset(&jr, 0, sizeof(jr));
        jr.detail = true;
        (void)atlas_ipc_result_str(resp, "run", &jr.run);
        (void)atlas_ipc_result_str(resp, "status", &jr.run_status);
        (void)atlas_ipc_result_str(resp, "root", &jr.job);
        (void)atlas_ipc_result_str(resp, "created_at", &jr.created_at);
        (void)atlas_ipc_result_str(resp, "active_state", &jr.state);
        (void)atlas_ipc_result_str(resp, "active", &jr.follow_up);
        /* A11.6. How many tasks are running and how many the run allows. Both
         * stay zero when the daemon does not report them, and the renderers
         * print the line only when the bound is present — an absent key must not
         * render as a claim that a run allows nothing. */
        {
            int64_t n = 0;
            if (atlas_ipc_result_int(resp, "active_count", &n)) {
                jr.active_count = n;
            }
            if (atlas_ipc_result_int(resp, "max_parallel", &n)) {
                jr.max_parallel = n;
            }
        }
        /* A10.0. Every absent key leaves the conservative value: a daemon that
         * does not report usage is read as UNKNOWN, never as an error and never
         * as a zero total. */
        atlas_usage_run_init(&jr.usage);
        const char *us = NULL;
        if (atlas_ipc_result_str(resp, "usage_status", &us) && us != NULL &&
            atlas_usage_status_parse(us, &jr.usage.status)) {
            jr.usage_present = true;
        }
        const struct {
            const char *k;
            int64_t *v;
        } U[] = {
            {"usage_attempts_started", &jr.usage.attempts_started},
            {"usage_attempts_measured", &jr.usage.attempts_with_usage},
            {"usage_attempts_missing", &jr.usage.attempts_missing_usage},
            {"usage_worker_starts", &jr.usage.worker_starts},
            {"usage_input_tokens", &jr.usage.input_tokens},
            {"usage_output_tokens", &jr.usage.output_tokens},
            {"usage_cache_creation_tokens", &jr.usage.cache_creation_tokens},
            {"usage_cache_read_tokens", &jr.usage.cache_read_tokens},
            {"usage_worker_duration_ms", &jr.usage.worker_duration_ms},
            {"usage_turns", &jr.usage.turns},
            {"usage_cost_known_micro_usd", &jr.usage.cost_known_micro_usd},
        };
        for (size_t i = 0; i < sizeof U / sizeof U[0]; i++) {
            int64_t v = 0;
            if (atlas_ipc_result_int(resp, U[i].k, &v)) {
                *U[i].v = v;
            }
        }
        jr.usage.has_any_cost = jr.usage.cost_known_micro_usd > 0;
        bool b = false;
        if (atlas_ipc_result_bool(resp, "usage_tokens_complete", &b)) {
            jr.usage.tokens_complete = b;
        }
        if (atlas_ipc_result_bool(resp, "usage_cost_complete", &b)) {
            jr.usage.cost_complete = b;
        }
        /* A10.1. The memory block, present only for a run that has a frozen
         * manifest. `memory_present` is what a renderer branches on, so a run
         * from before migration 23 prints nothing rather than printing OFF —
         * "this run was never part of a memory arm" and "this run ran with
         * memory off" are different facts and must not share a line. */
        if (atlas_ipc_result_str(resp, "memory_mode", &jr.memory_mode) &&
            jr.memory_mode != NULL) {
            jr.memory_present = true;
            (void)atlas_ipc_result_str(resp, "memory_package_status",
                                       &jr.memory_package_status);
            (void)atlas_ipc_result_str(resp, "memory_package_digest",
                                       &jr.memory_package_digest);
            (void)atlas_ipc_result_int(resp, "memory_source_count", &jr.memory_source_count);
            (void)atlas_ipc_result_int(resp, "memory_package_bytes", &jr.memory_package_bytes);
            if (atlas_ipc_result_bool(resp, "memory_candidates_truncated", &b)) {
                jr.memory_candidates_truncated = b;
            }
            size_t n = 0;
            if (atlas_ipc_result_arr_len(resp, "memory_sources", &n)) {
                for (size_t i = 0; i < n && i < 3u; i++) {
                    const char *sv = NULL;
                    if (atlas_ipc_result_arr_str(resp, "memory_sources", i, &sv) && sv != NULL) {
                        jr.memory_sources[jr.memory_source_listed++] = sv;
                    }
                }
            }
        }
        st = sink(&jr, ud, err);
    }
    atlas_ipc_response_free(resp);
    atlas_buf_free(&raw);
    return st;
}

/* A11.1's run driver on one run, over this process's socket transport.
 *
 * A12.0 factored it out of `atlas_service_job_run` unchanged. `atlas plan run`
 * drives each stage as an ordinary run, and it must do so through exactly this
 * wiring — the same socket, the same policy fields, the same spool — because a
 * second assembly of the run driver's options would be a second answer to what a
 * stage of a plan runs under, and the difference would show up as a run that
 * behaved differently depending on which command started it.
 *
 * `pol` is the caller's, already loaded from its compiled-in root-owned path and
 * already checked ENABLED; it must outlive this synchronous call. A run that
 * ended BLOCKED is an *answer* and returns ATLAS_OK with the status in `rep`. */
atlas_status atlas_service_run_drive(const atlas_orchpolicy *pol, const char *run_uid, FILE *log,
                                     atlas_rundriver_report *rep, atlas_err *err) {
    run_xport x;
    atlas_buf_init(&x.sock);
    atlas_status st = atlas_ipc_socket_path(&x.sock, err);
    if (st == ATLAS_OK) {
        char id[128];
        (void)snprintf(id, sizeof id, "atlas-run/%lld", (long long)getpid());
        atlas_rundriver_opts ro;
        memset(&ro, 0, sizeof(ro));
        ro.run_uid = run_uid;
        ro.dispatcher_id = id;
        ro.live_model = pol->live_model;
        ro.operator_session = atlas_orchpolicy_is_model_dispatcher(pol, (long long)getuid()) &&
                              pol->model_uses_operator_session;
        /* A12.0. The same two names the background dispatcher is given, from the
         * same root-owned policy. */
        ro.models.planner = pol->planner_model;
        ro.models.executor = pol->executor_model;
        /* A11.5a-R. Where a finished worker's result is made durable before the
         * daemon is asked to accept it. The path comes from the root-owned
         * policy — the same field that already says where this dispatcher owns
         * its state — so it is a value the operator's machine configured and
         * never one a task, an environment or a model chose. Empty in the policy
         * means no spool, which is the behaviour that shipped before this. */
        ro.spool_dir = pol->model_worker_root[0] != '\0' ? pol->model_worker_root : NULL;
        ro.log = log;
        ro.transport.apply = xport_apply;
        ro.transport.run_get = xport_run_get;
        ro.transport.job_get = xport_job_get;
        ro.transport.ud = &x;
        st = atlas_rundriver_run(&ro, rep, err);
    }
    atlas_buf_free(&x.sock);
    return st;
}

atlas_status atlas_service_job_run(atlas_ctx *ctx, const atlas_job_run_opts *o,
                                   atlas_job_sink sink, void *ud, atlas_err *err) {
    const bool resuming = o->resume != NULL && o->resume[0] != '\0';
    if (!resuming && (o->repo == NULL || o->repo[0] == '\0' || o->task == NULL ||
                      o->task[0] == '\0')) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas job run needs --repo and --task, or --resume RUN");
    }
    if (resuming && o->memory != NULL && o->memory[0] != '\0') {
        /* A10.1. Refused rather than ignored. The package is frozen against the
         * run at the moment it was created, so a resume cannot change what the
         * worker is shown — and a flag that is silently dropped reads, in a
         * transcript of an experiment, exactly like one that was honoured. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--memory applies when a run is created; the package a run was "
                             "frozen with cannot be changed by resuming it");
    }
    if (resuming && o->max_parallel != 0) {
        /* A11.6. Refused rather than ignored, for A10.1's reason exactly: how
         * many tasks a run may hold active is fixed when the run is created, so
         * a resume cannot change it — and a flag that is silently dropped reads,
         * afterwards, exactly like one that was honoured. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--parallel applies when a run is created; the bound a run was "
                             "created with cannot be changed by resuming it");
    }
    if (resuming && (o->repo != NULL || o->task != NULL)) {
        /* Refused rather than silently preferring one. A caller that named both
         * a new task and a run to resume has asked for two different things and
         * Atlas does not pick. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--resume names an existing run, so --repo and --task do not apply");
    }
    if (!resuming && o->gate_count == 0) {
        /* The gates are fixed at the root task and inherited unchanged by every
         * follow-up, so a run created without one could never be accepted on
         * anything but a process exit code. Refused at the only moment it can
         * still be fixed. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a run needs at least one --gate; a run with no gate can only be "
                             "accepted on a worker's exit code, which is not a success claim");
    }

    /* The policy decides whether a live model may run and whose session it uses.
     * Read here from its compiled-in root-owned path, exactly as the background
     * dispatcher reads it, and overridable by nothing. */
    atlas_orchpolicy pol;
    atlas_orchpolicy_load(&pol);
    if (pol.state != ATLAS_ORCHPOLICY_ENABLED) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG, "orchestration is not enabled (%s: %s)",
                             atlas_orchpolicy_reason_name(pol.reason),
                             atlas_orchpolicy_reason_explain(pol.reason));
    }

    atlas_buf run_uid = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (resuming) {
        st = atlas_buf_set_str(&run_uid, o->resume, err);
    } else {
        /* The root task goes through the ordinary submit path — the same RPC, the
         * same policy checks, the same write point. There is no second submit
         * surface and this milestone does not add one. */
        atlas_job_submit_opts so;
        memset(&so, 0, sizeof(so));
        so.repo = o->repo;
        so.task = o->task;
        so.mode = o->mode;
        so.driver = o->driver != NULL ? o->driver : "claude-repo";
        so.idempotency_key = o->idempotency_key;
        so.wall_timeout_ms = o->wall_timeout_ms;
        so.idle_timeout_ms = o->idle_timeout_ms;
        so.max_attempts = ATLAS_ORCH_RUN_MAX_WORKER_STARTS;
        so.memory = o->memory;
        so.max_parallel = o->max_parallel;
        for (size_t i = 0; i < o->gate_count && i < 8u; i++) {
            so.gates[so.gate_count++] = o->gates[i];
        }
        atlas_ipc_response *resp = NULL;
        atlas_buf raw = ATLAS_BUF_INIT;
        st = atlas_service_orch_call(ctx, "job.submit", build_submit, &so, &resp, &raw, err);
        if (st == ATLAS_OK) {
            const char *v = NULL;
            if (!atlas_ipc_result_str(resp, "run", &v) || v == NULL) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "the daemon accepted a task without a run");
            } else {
                st = atlas_buf_set_str(&run_uid, v, err);
            }
        }
        atlas_ipc_response_free(resp);
        atlas_buf_free(&raw);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&run_uid);
        return st;
    }

    atlas_rundriver_report rep;
    atlas_rundriver_report_init(&rep);
    st = atlas_service_run_drive(&pol, atlas_buf_cstr(&run_uid), o->log, &rep, err);
    if (st == ATLAS_OK && sink != NULL) {
        atlas_job_render jr;
        memset(&jr, 0, sizeof(jr));
        jr.detail = true;
        jr.run = atlas_buf_cstr(&rep.run_uid);
        jr.run_status = atlas_orch_run_status_name(rep.status);
        jr.job = rep.last_job_uid.len > 0 ? atlas_buf_cstr(&rep.last_job_uid) : NULL;
        jr.worker_starts = rep.worker_starts;
        jr.tasks = rep.tasks;
        jr.busy = rep.busy;
        st = sink(&jr, ud, err);
    }
    atlas_rundriver_report_free(&rep);
    atlas_buf_free(&run_uid);
    return st;
}

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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/dispatch.h"
#include "atlas/ipc.h"
#include "atlas/orch.h"
#include "atlas/orchpolicy.h"
#include "atlas/service.h"
#include "atlas/syspolicy.h"
#include "service_internal.h"

/* --- talking to the daemon -------------------------------------------------- */

typedef atlas_status (*orch_build_fn)(atlas_json *j, void *ud, atlas_err *err);

static atlas_status orch_call(atlas_ctx *ctx, const char *method, orch_build_fn build, void *ud,
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
        {&jr->task, "task"},
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
}

/* --- job submit -------------------------------------------------------------- */

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
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof nums / sizeof nums[0]; i++) {
        if (nums[i].v > 0) {
            st = atlas_json_key_int(j, nums[i].k, nums[i].v, err);
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
    /* Copied rather than const-cast: a cast that discards a qualifier is right
     * today and wrong after one edit. */
    atlas_job_submit_opts local = *o;
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = orch_call(ctx, "job.submit", build_submit, &local, &resp, &raw, err);
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
    atlas_status st = orch_call(ctx, method, build_job, &job_copy, &resp, &raw, err);
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
    return st;
}

typedef struct list_sink_ctx {
    atlas_job_sink sink;
    void *ud;
} list_sink_ctx;

static atlas_status forward_row(const atlas_ipc_response *r, size_t index, void *ud,
                                atlas_err *err);

atlas_status atlas_service_job_list(atlas_ctx *ctx, int64_t after, int64_t limit,
                                    atlas_job_sink sink, void *ud, int64_t *count_out,
                                    bool *more_out, atlas_err *err) {
    list_args a = {after, limit};
    atlas_ipc_response *resp = NULL;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_status st = orch_call(ctx, "job.list", build_list, &a, &resp, &raw, err);
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
    return lc->sink != NULL ? lc->sink(&jr, lc->ud, err) : ATLAS_OK;
}

/* --- the dispatcher -------------------------------------------------------------- */

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
    /* The dispatcher must be the uid the policy names. Checked here so a
     * misconfigured unit fails at startup with a clear message rather than
     * being refused on every lease attempt for the lifetime of the service. */
    if (!atlas_orchpolicy_is_dispatcher(&op, (long long)getuid())) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "this process runs as uid %lld, which the orchestration policy does "
                             "not name as the dispatcher",
                             (long long)getuid());
    }

    char id[128];
    (void)snprintf(id, sizeof id, "atlas-dispatcher/%lld", (long long)getpid());

    atlas_dispatch_opts o;
    memset(&o, 0, sizeof(o));
    o.socket_path = sp.socket_path;
    o.worker_root = op.worker_root;
    o.dispatcher_id = id;
    o.poll_ms = 1000;
    o.max_backoff_ms = 30000;
    /* Comfortably below the lease lifetime, so a healthy job never loses its
     * lease while working. */
    o.heartbeat_ms = ATLAS_ORCH_LEASE_MS / 4;
    o.live_model = op.live_model;
    o.max_iterations = once ? 1 : 0;
    o.log = log;
    return atlas_dispatch_run(&o, err);
}

/* Atlas - deploy MCP tools.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Shared transport and trust contracts: mcp_tools.c and mcp_tools_internal.h.
 */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "mcp/mcp_internal.h"


#include "mcp/mcp_tools_internal.h"

/* --- T4 (remote deploy): four remote-only deploy tools --------------------
 *
 * Forward, one for one, to the daemon's `deploy.remote_propose`,
 * `deploy.remote_get`, `deploy.remote_list` and `deploy.remote_cancel`,
 * exactly the way the four A14 job tools above forward to `job.remote_*`.
 * `remote_only = true` on all four: the methods behind them are offered only
 * to the gateway uid, so a stdio session would reach `unknown method` on
 * every call, and publishing a tool that always fails is worse than not
 * publishing it.
 *
 * There is no fifth tool here for `deploy.remote_challenge` or
 * `deploy.remote_confirm`, and there must not be. Confirming a deploy is the
 * operator's own decision, taken through a distinct credential
 * (`remote_deploy_key`, never `remote_submit_key`) and a dedicated route —
 * A16's shape, not an MCP call a model can make. `deploy.remote_propose` and
 * `deploy.remote_cancel` are authorised by the *proposing* submit credential
 * (the design's "remote_submit_key, derived jobs:submit" and "the proposing
 * submit identity"), so they carry `ATLAS_SCOPE_JOBS_SUBMIT`, the same scope
 * the job tools carry and the only one a submit credential can ever hold.
 * `deploy.remote_get` and `deploy.remote_list` accept either the submit or
 * the deploy identity at the daemon, but `tool_def` has one scope field for
 * visibility, and `ATLAS_SCOPE_DEPLOYS_CONFIRM` does not exist in this
 * header -- it is a different task's credential, never derived for an MCP
 * session. `ATLAS_SCOPE_JOBS_SUBMIT` is therefore the only scope available
 * here, and it is the correct one: a submit credential is guaranteed to
 * reach both.
 *
 * None of the four carries a `repo` argument. A deploy is not scoped to a
 * caller-chosen repository the way a job is -- `deploy.remote_propose` takes
 * only the job identifier, and the daemon resolves the repository, the base
 * commit and the patch from that job's own stored row (the design's "the
 * request carries only `job`"). */

typedef struct deploy_args {
    const char *job;    /* atlas_deploy_propose */
    const char *deploy; /* atlas_deploy_status, atlas_deploy_cancel */
    int64_t cursor;     /* atlas_deploy_list */
    const char *token;  /* the request's bearer, forwarded to the daemon */
} deploy_args;

static atlas_status put_deploy_propose_args(atlas_json *j, void *ud, atlas_err *err) {
    deploy_args *a = (deploy_args *)ud;
    atlas_status st = atlas_json_key_str(j, "job", a->job, err);
    if (st == ATLAS_OK && a->token != NULL) {
        st = atlas_json_key_str(j, "token", a->token, err);
    }
    return st;
}

static atlas_status put_deploy_id_args(atlas_json *j, void *ud, atlas_err *err) {
    deploy_args *a = (deploy_args *)ud;
    atlas_status st = atlas_json_key_str(j, "deploy", a->deploy, err);
    if (st == ATLAS_OK && a->token != NULL) {
        st = atlas_json_key_str(j, "token", a->token, err);
    }
    return st;
}

static atlas_status put_deploy_list_args(atlas_json *j, void *ud, atlas_err *err) {
    deploy_args *a = (deploy_args *)ud;
    atlas_status st = ATLAS_OK;
    if (a->cursor > 0) {
        st = atlas_json_key_int(j, "cursor", a->cursor, err);
    }
    if (st == ATLAS_OK && a->token != NULL) {
        st = atlas_json_key_str(j, "token", a->token, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_deploy_propose(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"job", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "job",
                      "the identifier of a SUCCEEDED job, returned by atlas_job_submit, whose "
                      "patch is proposed as a deploy. Atlas resolves the repository, base commit "
                      "and patch from the job's own stored record; nothing here names a "
                      "repository or file. Disposing of the proposal -- confirming it so the "
                      "deploy agent runs it, or cancelling it -- is a decision reachable only "
                      "through a different credential and a dedicated channel, never this tool.",
                      0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_deploy_propose(atlas_mcp_server *s, const atlas_jsonv *args,
                                       atlas_buf *body, bool *degraded, atlas_err *err) {
    static const char *const ALLOWED[] = {"job", NULL};
    atlas_status st = atlas_jsonv_check_only_keys(args, ALLOWED, err);
    if (st != ATLAS_OK) {
        return st;
    }
    deploy_args a;
    memset(&a, 0, sizeof(a));
    st = atlas_mcp_tools_arg_str(args, "job", ATLAS_NAME_MAX, &a.job, err);
    if (st == ATLAS_OK && a.job == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"job\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_deploy_propose_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* The response is a deploy identifier, a patch digest and byte count
         * -- Atlas' own record of a job it already holds, never the patch's
         * bytes or a worker's prose. */
        st = atlas_mcp_tools_forward(s, "deploy.remote_propose", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

atlas_status atlas_mcp_tools_schema_deploy_status(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"deploy", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "deploy", "the deploy identifier returned by atlas_deploy_propose", 0,
                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_deploy_status(atlas_mcp_server *s, const atlas_jsonv *args,
                                      atlas_buf *body, bool *degraded, atlas_err *err) {
    static const char *const ALLOWED[] = {"deploy", NULL};
    atlas_status st = atlas_jsonv_check_only_keys(args, ALLOWED, err);
    if (st != ATLAS_OK) {
        return st;
    }
    deploy_args a;
    memset(&a, 0, sizeof(a));
    st = atlas_mcp_tools_arg_str(args, "deploy", ATLAS_NAME_MAX, &a.deploy, err);
    if (st == ATLAS_OK && a.deploy == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"deploy\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_deploy_id_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* Marked untrusted for the same reason atlas_verify_show and
         * atlas_verify_evaluate are: the envelope is Atlas' own row, but a
         * terminal deploy's result carries `result_text` -- the deploy
         * agent's own stage lines and the tail of its last command's output
         * -- which is arbitrary captured text, not a value Atlas computed. */
        st = atlas_mcp_tools_forward(s, "deploy.remote_get", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

atlas_status atlas_mcp_tools_schema_deploy_list(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "cursor",
                      "return deploys after this cursor (0 or omitted = start from the "
                      "beginning)",
                      0, INT64_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_deploy_list(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    static const char *const ALLOWED[] = {"cursor", NULL};
    atlas_status st = atlas_jsonv_check_only_keys(args, ALLOWED, err);
    if (st != ATLAS_OK) {
        return st;
    }
    deploy_args a;
    memset(&a, 0, sizeof(a));
    a.cursor = atlas_mcp_tools_arg_int(args, "cursor", 0);
    if (s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_deploy_list_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "deploy.remote_list", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

atlas_status atlas_mcp_tools_schema_deploy_cancel(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"deploy", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "deploy", "the deploy identifier returned by atlas_deploy_propose", 0,
                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_deploy_cancel(atlas_mcp_server *s, const atlas_jsonv *args,
                                      atlas_buf *body, bool *degraded, atlas_err *err) {
    static const char *const ALLOWED[] = {"deploy", NULL};
    atlas_status st = atlas_jsonv_check_only_keys(args, ALLOWED, err);
    if (st != ATLAS_OK) {
        return st;
    }
    deploy_args a;
    memset(&a, 0, sizeof(a));
    st = atlas_mcp_tools_arg_str(args, "deploy", ATLAS_NAME_MAX, &a.deploy, err);
    if (st == ATLAS_OK && a.deploy == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"deploy\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_deploy_id_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "deploy.remote_cancel", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

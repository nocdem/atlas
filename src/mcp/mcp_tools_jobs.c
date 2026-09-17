/* Atlas - jobs MCP tools.
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

/* --- A14: four remote-only job tools -------------------------------------- */

typedef struct job_args {
    const char *repo;
    const char *task;  /* atlas_job_submit */
    const char *key;   /* atlas_job_submit: idempotency key */
    const char *job;   /* atlas_job_status, atlas_job_cancel */
    int64_t after;     /* atlas_job_list */
    int64_t limit;     /* atlas_job_list */
    const char *token; /* the request's bearer, forwarded to the daemon */
} job_args;

static atlas_status put_job_submit_args(atlas_json *j, void *ud, atlas_err *err) {
    job_args *a = (job_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->task != NULL) {
        st = atlas_json_key_str(j, "task", a->task, err);
    }
    if (st == ATLAS_OK && a->key != NULL) {
        st = atlas_json_key_str(j, "key", a->key, err);
    }
    if (st == ATLAS_OK && a->token != NULL) {
        st = atlas_json_key_str(j, "token", a->token, err);
    }
    return st;
}

static atlas_status put_job_id_args(atlas_json *j, void *ud, atlas_err *err) {
    job_args *a = (job_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->job != NULL) {
        st = atlas_json_key_str(j, "job", a->job, err);
    }
    if (st == ATLAS_OK && a->token != NULL) {
        st = atlas_json_key_str(j, "token", a->token, err);
    }
    return st;
}

static atlas_status put_job_list_args(atlas_json *j, void *ud, atlas_err *err) {
    job_args *a = (job_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->after > 0) {
        st = atlas_json_key_int(j, "after", a->after, err);
    }
    if (st == ATLAS_OK && a->limit > 0) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    if (st == ATLAS_OK && a->token != NULL) {
        st = atlas_json_key_str(j, "token", a->token, err);
    }
    return st;
}

/* Resolves the repository from the optional `repo` argument. */
static atlas_status begin_job_call(atlas_mcp_server *s, const atlas_jsonv *args, job_args *a,
                                   atlas_buf *repo, atlas_err *err) {
    memset(a, 0, sizeof(*a));
    const char *requested = NULL;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_resolve_repo(s, requested, repo, err);
    }
    if (st == ATLAS_OK) {
        a->repo = atlas_buf_cstr(repo);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_job_submit(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"task", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "task",
                      "the task text, stored as UNTRUSTED_DATA on the Atlas machine and used "
                      "as the worker's prompt. It is read on the Atlas machine, never here. "
                      "Maximum 65536 bytes.",
                      65536, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "key",
                      "optional idempotency key (maximum 40 bytes). Pass the same key on a "
                      "retry to resolve to the job you already submitted.",
                      40, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_job_submit(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    /* Enforce additionalProperties: false at runtime. The schema JSON publishes
     * this constraint but atlas_mcp_call_tool does not validate it before
     * calling run(). Check the positive allowlist before reading any arg. */
    static const char *const ALLOWED[] = {"repo", "task", "key", NULL};
    atlas_status st = atlas_jsonv_check_only_keys(args, ALLOWED, err);
    if (st != ATLAS_OK) {
        return st;
    }
    job_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    st = begin_job_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "task", 65536u, &a.task, err);
    }
    if (st == ATLAS_OK && a.task == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"task\" is required");
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "key", 40u, &a.key, err);
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_job_submit_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "job.remote_submit", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_job_status(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"job", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "job", "the job identifier returned by atlas_job_submit", 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_job_status(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    job_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_job_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "job", ATLAS_NAME_MAX, &a.job, err);
    }
    if (st == ATLAS_OK && a.job == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"job\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_job_id_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "job.remote_get", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* A14R. The result of one terminal job.
 *
 * Same shape and the same two arguments as `atlas_job_status`, deliberately: it
 * is the second half of one question a steward asks — "is it done" and "what
 * did it do" — and a caller that can drive one can drive the other. The
 * authorisation is not restated here; it is `job.remote_result`'s, in the write
 * transaction, against the credential this adapter forwards. */
atlas_status atlas_mcp_tools_schema_job_result(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"job", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "job", "the job identifier returned by atlas_job_submit", 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_job_result(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    job_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_job_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "job", ATLAS_NAME_MAX, &a.job, err);
    }
    if (st == ATLAS_OK && a.job == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"job\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_job_id_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* The result carries a patch and a worker's own words, so the envelope
         * says MODEL_PROPOSAL and the untrusted notice is attached — unlike the
         * three job tools beside it, whose fields are all Atlas' own and which
         * are ATLAS_OWNED. A proposal is exactly what this is: a change nothing
         * has applied, accepted or approved, offered for a person to read. */
        st = atlas_mcp_tools_forward(s, "job.remote_result", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* A14R-F. Why one job this credential submitted did not succeed.
 *
 * The same two arguments as `atlas_job_result`, and the same authorisation,
 * made in the daemon against the forwarded credential. It exists for the case
 * the result tool cannot serve: a worker that never started, or one that
 * stopped before it wrote a final answer. What comes back is the ledger, the
 * dispatcher's own failure record when one was carried, and a bounded tail of
 * the redacted worker log when one was carried -- each absence stated with its
 * reason rather than left as an empty field. */
atlas_status atlas_mcp_tools_schema_job_failure(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"job", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "job", "the job identifier returned by atlas_job_submit", 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_job_failure(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    job_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_job_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "job", ATLAS_NAME_MAX, &a.job, err);
    }
    if (st == ATLAS_OK && a.job == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"job\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_job_id_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* The envelope takes the weakest class of anything inside it -- A6's
         * rule for a verdict, applied to provenance. The ledger fields are
         * Atlas' own, but the log tail and the event payloads are a worker's
         * bytes, so this is labelled as `atlas_job_result` is and never
         * ATLAS_OWNED, which is the one class allowed into automatic context. */
        st = atlas_mcp_tools_forward(s, "job.remote_failure", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_job_list(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "after",
                      "return jobs submitted after this cursor (0 = start from the beginning)",
                      0, INT64_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum jobs to return", 1, 200, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_job_list(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                 bool *degraded, atlas_err *err) {
    job_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_job_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        a.after = atlas_mcp_tools_arg_int(args, "after", 0);
        a.limit = atlas_mcp_tools_arg_int(args, "limit", 0);
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_job_list_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "job.remote_list", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_job_cancel(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"job", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "job", "the job identifier returned by atlas_job_submit", 0, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_job_cancel(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    job_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_job_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "job", ATLAS_NAME_MAX, &a.job, err);
    }
    if (st == ATLAS_OK && a.job == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"job\" is required");
    }
    if (st == ATLAS_OK && s->remote_token.len > 0) {
        a.token = atlas_buf_cstr(&s->remote_token);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_job_id_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "job.remote_cancel", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

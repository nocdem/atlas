/* Atlas - read MCP tools.
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

/* --- read tools ------------------------------------------------------------ */

atlas_status atlas_mcp_tools_schema_none(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_status(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                               bool *degraded, atlas_err *err) {
    (void)args;
    return atlas_mcp_tools_forward(s, "daemon.status", "{}", atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false,
                   body, degraded, err);
}

atlas_status atlas_mcp_tools_schema_repo_only(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_overview(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                 bool *degraded, atlas_err *err) {
    repo_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_begin_repo_call(s, args, &a, &repo, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(atlas_mcp_tools_put_repo_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* The overview carries a root path and a repository name, both of which
         * are user-chosen rather than Atlas-chosen, so it is marked untrusted
         * even though everything else in it is an integer or a fixed string. */
        st = atlas_mcp_tools_forward(s, "repo.state", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_changed(atlas_json *j, atlas_err *err) {
    static const char *const SCOPES[] = {"all", "staged", "unstaged", "untracked", "unmerged",
                                         NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "scope", "which git comparison to report. Defaults to all.", SCOPES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum entries to return", 1, ATLAS_MCP_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "cursor", "resume after this cursor, from a previous call", 0, INT64_MAX,
                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_changed(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                bool *degraded, atlas_err *err) {
    repo_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_begin_repo_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "scope", 16u, &a.scope, err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(atlas_mcp_tools_put_repo_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "ai.changed", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_file(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"path", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "path", "a repository-relative path, never absolute", 4096, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum history and record entries", 1, ATLAS_MCP_MAX_ROWS,
                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_file(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                             bool *degraded, atlas_err *err) {
    repo_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_begin_repo_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_rel_path(args, "path", &a.path, err);
    }
    if (st == ATLAS_OK && a.path == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"path\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(atlas_mcp_tools_put_repo_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* This is the tool that returns commit subjects, which are repository
         * prose in the fullest sense. Bounded, encoded, labelled GIT, and
         * returned only because a caller asked about this specific path. */
        st = atlas_mcp_tools_forward(s, "ai.file.context", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_GIT), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_search(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"query", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "query", "text to look for in indexed paths and commit messages", 256,
                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum results per kind", 1, ATLAS_MCP_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_search(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                               bool *degraded, atlas_err *err) {
    repo_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_begin_repo_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "query", 256u, &a.query, err);
    }
    if (st == ATLAS_OK && a.query == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"query\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(atlas_mcp_tools_put_repo_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "repo.search", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_GIT), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_memory(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                               bool *degraded, atlas_err *err) {
    repo_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_begin_repo_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "query", 256u, &a.query, err);
    }
    if (st == ATLAS_OK && a.query == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"query\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(atlas_mcp_tools_put_repo_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* What comes back is what a model previously wrote down. It is a
         * proposal, it is untrusted, and it is labelled as both. */
        st = atlas_mcp_tools_forward(s, "ai.memory.search", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_session(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                bool *degraded, atlas_err *err) {
    repo_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_begin_repo_call(s, args, &a, &repo, err);
    session_args sa;
    sa.server = s;
    sa.repo = a.repo;
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(atlas_mcp_tools_put_session_args, &sa, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "ai.session.get", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

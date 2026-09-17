/* Atlas - code MCP tools.
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

/* --- structural tools (A3) ---------------------------------------------------
 *
 * Six tools over the structural graph, each a thin translation into one
 * `code.*` IPC method. No query logic here, for the same reason as everywhere
 * else in this file: a second implementation of "what depends on this" would
 * eventually answer differently from the first.
 *
 * Everything they return is repository text — symbol names, include spellings,
 * paths — so every one is `untrusted: true` and carries the same notice the
 * other repository-derived tools do. And every result carries the structural
 * index's currency and generation, because a structural answer without those is
 * a claim about a repository as it may no longer be. */

typedef struct code_args {
    const char *repo;
    const char *path;
    const char *symbol;
    const char *query;
    const char *kind;
    int64_t limit;
    int64_t depth;
} code_args;

static atlas_status put_code_args(atlas_json *j, void *ud, atlas_err *err) {
    code_args *a = (code_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->path != NULL) {
        st = atlas_json_key_str(j, "path", a->path, err);
    }
    if (st == ATLAS_OK && a->symbol != NULL) {
        st = atlas_json_key_str(j, "symbol", a->symbol, err);
    }
    if (st == ATLAS_OK && a->query != NULL) {
        st = atlas_json_key_str(j, "query", a->query, err);
    }
    if (st == ATLAS_OK && a->kind != NULL) {
        st = atlas_json_key_str(j, "kind", a->kind, err);
    }
    if (st == ATLAS_OK && a->limit > 0) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    if (st == ATLAS_OK && a->depth > 0) {
        st = atlas_json_key_int(j, "depth", a->depth, err);
    }
    return st;
}

/* Resolves the repository from the granted roots and fills the shared block. */
static atlas_status begin_code_call(atlas_mcp_server *s, const atlas_jsonv *args, code_args *a,
                                    atlas_buf *repo, atlas_err *err) {
    memset(a, 0, sizeof(*a));
    const char *requested = NULL;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_mcp_resolve_repo(s, requested, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    a->repo = atlas_buf_cstr(repo);
    a->limit = atlas_mcp_tools_arg_int(args, "limit", 0);
    a->depth = atlas_mcp_tools_arg_int(args, "depth", 0);
    return ATLAS_OK;
}

atlas_status atlas_mcp_tools_run_code_status(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    code_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_code_call(s, args, &a, &repo, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_code_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* Counts, states and a generation. The repository name is in it, which
         * is user-chosen, so it is marked untrusted like the overview is. */
        st = atlas_mcp_tools_forward(s, "code.status", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_code_search(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"query", NULL};
    static const char *const KINDS[] = {"function", "macro",         "macro_function", "typedef",
                                        "struct",   "union",         "enum",           "enum_constant",
                                        "variable", "unknown",       NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "query", "substring to look for in indexed symbol names", 256, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "kind", "restrict to one symbol kind", KINDS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum symbols to return", 1, ATLAS_CODE_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_code_search(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    code_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_code_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "query", 256u, &a.query, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "kind", 24u, &a.kind, err);
    }
    if (st == ATLAS_OK && a.query == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"query\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_code_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "code.symbol.search", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_code_symbol(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"symbol", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "symbol", "the exact symbol name", ATLAS_CODE_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum sites and edges to return", 1, ATLAS_CODE_MAX_ROWS,
                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_code_symbol(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    code_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_code_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "symbol", ATLAS_CODE_MAX_NAME_BYTES, &a.symbol, err);
    }
    if (st == ATLAS_OK && a.symbol == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"symbol\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_code_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "code.symbol", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_code_path(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"path", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "path", "a repository-relative path, never absolute", 4096, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum entries to return", 1, ATLAS_CODE_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_code_walk(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "path", "a repository-relative path. Give this or \"symbol\".", 4096, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "symbol", "an exact symbol name. Give this or \"path\".",
                      ATLAS_CODE_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "depth", "how far to traverse", 1, ATLAS_CODE_MAX_TRAVERSAL_DEPTH, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum candidates to return", 1, ATLAS_CODE_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

/* One helper for the three tools that take a path or a symbol. */
static atlas_status run_code_method(atlas_mcp_server *s, const atlas_jsonv *args,
                                    const char *method, bool need_path, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    code_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_code_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_rel_path(args, "path", &a.path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "symbol", ATLAS_CODE_MAX_NAME_BYTES, &a.symbol, err);
    }
    if (st == ATLAS_OK && need_path && a.path == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"path\" is required");
    }
    if (st == ATLAS_OK && !need_path && a.path == NULL && a.symbol == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "a \"path\" or a \"symbol\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_code_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, method, atlas_buf_cstr(&params), atlas_provenance_name(ATLAS_PROV_SOURCE),
                     true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_code_file(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                  bool *degraded, atlas_err *err) {
    return run_code_method(s, args, "code.file", true, body, degraded, err);
}

atlas_status atlas_mcp_tools_run_code_deps(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                  bool *degraded, atlas_err *err) {
    return run_code_method(s, args, "code.deps", false, body, degraded, err);
}

atlas_status atlas_mcp_tools_run_code_impact(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    return run_code_method(s, args, "code.impact", false, body, degraded, err);
}

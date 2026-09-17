/* Atlas - sem MCP tools.
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

/* --- A8-CI: the compiler-derived semantic index -----------------------------
 *
 * Five reads, and nothing else. There is no tool here that builds an index,
 * invalidates one or changes any state, and that absence is the guarantee: a
 * model holding every Atlas tool still cannot cause a compiler to run.
 *
 * Every result carries the index's freshness and generation, because a semantic
 * answer without them is a claim about a repository as it may no longer be —
 * the same reason the A3 tools report currency. And every edge carries its
 * evidence class, so a model can tell a call the compiler proved from a
 * candidate target of a function pointer. Atlas never claims to know every
 * target of a function pointer; a traversal that crosses one says so.
 *
 * The repository comes from the persistent registry, not from the client's
 * roots — see `atlas_mcp_resolve_repo`. No tool here accepts an absolute path.
 * A `usr` is an opaque identifier Atlas itself produced and handed back through
 * `atlas_sem_symbol`; accepting one opens nothing a name would not. */

typedef struct sem_args {
    const char *repo;
    const char *symbol;
    const char *usr;
    const char *kind;
    const char *from;
    const char *to;
    int64_t limit;
    int64_t depth;
    bool inbound;
    bool proven_only;
} sem_args;

static atlas_status put_sem_args(atlas_json *j, void *ud, atlas_err *err) {
    sem_args *a = (sem_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->symbol != NULL) {
        st = atlas_json_key_str(j, "symbol", a->symbol, err);
    }
    if (st == ATLAS_OK && a->usr != NULL) {
        st = atlas_json_key_str(j, "usr", a->usr, err);
    }
    if (st == ATLAS_OK && a->kind != NULL) {
        st = atlas_json_key_str(j, "kind", a->kind, err);
    }
    if (st == ATLAS_OK && a->from != NULL) {
        st = atlas_json_key_str(j, "from", a->from, err);
    }
    if (st == ATLAS_OK && a->to != NULL) {
        st = atlas_json_key_str(j, "to", a->to, err);
    }
    if (st == ATLAS_OK && a->limit > 0) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    if (st == ATLAS_OK && a->depth > 0) {
        st = atlas_json_key_int(j, "depth", a->depth, err);
    }
    if (st == ATLAS_OK && a->inbound) {
        st = atlas_json_key_bool(j, "inbound", true, err);
    }
    if (st == ATLAS_OK && a->proven_only) {
        st = atlas_json_key_bool(j, "proven_only", true, err);
    }
    return st;
}

static atlas_status begin_sem_call(atlas_mcp_server *s, const atlas_jsonv *args, sem_args *a,
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

atlas_status atlas_mcp_tools_schema_sem_status(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_sem_status(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    sem_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_sem_call(s, args, &a, &repo, err);
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_sem_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "sem.status", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_sem_symbol(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"symbol", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "symbol", "the exact symbol name", ATLAS_SEM_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "kind",
                      "restrict to one kind: FUNCTION, STRUCT, UNION, ENUM, ENUM_CONSTANT, "
                      "TYPEDEF, FIELD, VARIABLE or MACRO",
                      32, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum symbols to return", 1, ATLAS_SEM_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_sem_symbol(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    sem_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_sem_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "symbol", ATLAS_SEM_MAX_NAME_BYTES, &a.symbol, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "kind", 32, &a.kind, err);
    }
    if (st == ATLAS_OK && a.symbol == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"symbol\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_sem_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "sem.symbol", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* Callers and callees are one walk in two directions, so they share a schema
 * and a runner. Two tools rather than one with a direction flag, because
 * "who calls this" and "what does this call" are the two questions a reader
 * actually asks, and a flag would make every call site spell out which. */
atlas_status atlas_mcp_tools_schema_sem_graph(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "symbol", "the exact symbol name", ATLAS_SEM_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "usr",
                      "an exact symbol identifier returned by atlas_sem_symbol; use this when a "
                      "name is ambiguous",
                      ATLAS_SEM_MAX_USR_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "depth", "how many call edges to follow; 1 is the direct answer", 1,
                      ATLAS_SEM_MAX_DEPTH, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum nodes to return", 1, ATLAS_SEM_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_bool(j, "proven_only",
                       "follow only compiler-proven calls, excluding candidate targets of "
                       "function pointers",
                       err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

static atlas_status run_sem_graph(atlas_mcp_server *s, const atlas_jsonv *args, bool inbound,
                                  atlas_buf *body, bool *degraded, atlas_err *err) {
    sem_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_sem_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "symbol", ATLAS_SEM_MAX_NAME_BYTES, &a.symbol, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "usr", ATLAS_SEM_MAX_USR_BYTES, &a.usr, err);
    }
    if (st == ATLAS_OK && a.symbol == NULL && a.usr == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "one of \"symbol\" or \"usr\" is required");
    }
    a.inbound = inbound;
    a.proven_only = atlas_mcp_tools_arg_int(args, "proven_only", 0) != 0;
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_sem_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "sem.graph", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_sem_callers(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    return run_sem_graph(s, args, true, body, degraded, err);
}

atlas_status atlas_mcp_tools_run_sem_callees(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    return run_sem_graph(s, args, false, body, degraded, err);
}

atlas_status atlas_mcp_tools_schema_sem_trace(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"from", "to", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "from", "the calling symbol's exact name", ATLAS_SEM_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "to", "the called symbol's exact name", ATLAS_SEM_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "depth", "how many call edges the path may cross", 1,
                      ATLAS_SEM_MAX_DEPTH, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_sem_trace(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                  bool *degraded, atlas_err *err) {
    sem_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_sem_call(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "from", ATLAS_SEM_MAX_NAME_BYTES, &a.from, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "to", ATLAS_SEM_MAX_NAME_BYTES, &a.to, err);
    }
    if (st == ATLAS_OK && (a.from == NULL || a.to == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"from\" and \"to\" are both required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_sem_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "sem.trace", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}


atlas_status atlas_mcp_tools_schema_sem_impact(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"subject", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "subject", "an exact symbol name or a repository-relative path",
                      ATLAS_SEM_MAX_NAME_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "depth", "how many call edges to follow when finding callers", 1,
                      ATLAS_SEM_MAX_DEPTH, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "maximum items to return", 1, ATLAS_SEM_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_sem_impact(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                   bool *degraded, atlas_err *err) {
    sem_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_sem_call(s, args, &a, &repo, err);
    const char *subject = NULL;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "subject", ATLAS_SEM_MAX_NAME_BYTES, &subject, err);
    }
    if (st == ATLAS_OK && subject == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"subject\" is required");
    }
    a.symbol = subject; /* carried under "subject" by the params writer below */
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "repo", a.repo, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "subject", subject, err);
            }
            if (st == ATLAS_OK && a.depth > 0) {
                st = atlas_json_key_int(j, "depth", a.depth, err);
            }
            if (st == ATLAS_OK && a.limit > 0) {
                st = atlas_json_key_int(j, "limit", a.limit, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_ipc_params_finish(p, &params, err);
            } else {
                atlas_ipc_params_abort(p);
            }
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "sem.impact", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* The task-context package.
 *
 * The `task` argument is free text a model writes, and it is used for one thing:
 * ranking evidence Atlas already holds. It selects no repository — `repo` is a
 * separate argument resolved from the persistent registry — and it authorises
 * nothing, because every method this reaches is a read. An imperative in it
 * ("delete the index", "approve the decision") is ranked as words and acted on
 * by nothing. */
/* Validate arrays before forwarding; a schema alone is not a boundary. */
static atlas_status put_context_seeds(atlas_json *j, const atlas_jsonv *args,
                                      const char *key, bool paths, atlas_err *err) {
    const atlas_jsonv *arr = atlas_jsonv_get(args, key);
    if (arr == NULL) return ATLAS_OK;
    if (!atlas_jsonv_is_arr(arr) || atlas_jsonv_arr_len(arr) > ATLAS_SEM_CONTEXT_MAX_SEEDS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "context seeds must be bounded arrays of strings");
    }
    atlas_status st = atlas_json_key(j, key, err);
    if (st == ATLAS_OK) st = atlas_json_arr_begin(j, err);
    for (size_t i = 0; st == ATLAS_OK && i < atlas_jsonv_arr_len(arr); i++) {
        const char *value = NULL;
        size_t len = 0;
        if (!atlas_jsonv_str(atlas_jsonv_at(arr, i), &value, &len) || len == 0 ||
            len >= (paths ? 512u : ATLAS_SEM_MAX_NAME_BYTES)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "context seed is not a bounded nonempty string");
        }
        if (paths) {
            atlas_buf raw = ATLAS_BUF_INIT;
            st = atlas_path_text_decode(value, len, &raw, err);
            if (st == ATLAS_OK) st = atlas_path_check_relative(raw.data, raw.len, err);
            atlas_buf_free(&raw);
        }
        if (st == ATLAS_OK) st = atlas_json_str(j, value, err);
    }
    if (st == ATLAS_OK) st = atlas_json_arr_end(j, err);
    return st;
}

atlas_status atlas_mcp_tools_schema_context_build(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"task", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "task", "what you are about to do, in your own words",
                      ATLAS_SEM_CONTEXT_MAX_TASK_BYTES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "depth", "how far to expand from each starting point", 1,
                      ATLAS_SEM_MAX_DEPTH, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "max_tokens", "approximate text budget; default 2048 tokens (8 KiB), excluding JSON overhead", 1,
                      ATLAS_SEM_CONTEXT_MAX_BYTES / ATLAS_SEM_BYTES_PER_TOKEN, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "max_items", "maximum items to include; default 24, increase only when needed", 1,
                      ATLAS_SEM_CONTEXT_MAX_ITEMS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str_array(j, "paths", "explicit repository-relative path_text seeds",
                                            ATLAS_SEM_CONTEXT_MAX_SEEDS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str_array(j, "symbols", "explicit case-sensitive symbol names",
                                            ATLAS_SEM_CONTEXT_MAX_SEEDS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_context_build(atlas_mcp_server *s, const atlas_jsonv *args,
                                      atlas_buf *body, bool *degraded, atlas_err *err) {
    sem_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = begin_sem_call(s, args, &a, &repo, err);
    const char *task = NULL;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "task", ATLAS_SEM_CONTEXT_MAX_TASK_BYTES, &task, err);
    }
    if (st == ATLAS_OK && task == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"task\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "repo", a.repo, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_str(j, "task", task, err);
            }
            if (st == ATLAS_OK) st = put_context_seeds(j, args, "paths", true, err);
            if (st == ATLAS_OK) st = put_context_seeds(j, args, "symbols", false, err);
            if (st == ATLAS_OK && a.depth > 0) {
                st = atlas_json_key_int(j, "depth", a.depth, err);
            }
            if (st == ATLAS_OK) {
                int64_t mt = atlas_mcp_tools_arg_int(args, "max_tokens", 0);
                if (mt > 0) {
                    st = atlas_json_key_int(j, "max_tokens", mt, err);
                }
            }
            if (st == ATLAS_OK) {
                int64_t mi = atlas_mcp_tools_arg_int(args, "max_items", 0);
                if (mi > 0) {
                    st = atlas_json_key_int(j, "max_items", mi, err);
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_ipc_params_finish(p, &params, err);
            } else {
                atlas_ipc_params_abort(p);
            }
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "sem.context", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

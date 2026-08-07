/* Atlas - the A3 IPC method group: the structural code graph.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These are the methods the MCP adapter and the CLI reach for structural
 * answers. They are looked up through the *same* dispatch as every other group,
 * assembled in server.c: two dispatchers is how a method comes to behave
 * differently depending on which one found it.
 *
 * Every method here is a read, on the per-request read-only handle, bounded by
 * its query rather than by a deadline — with one exception, `code.sync`, which
 * is a mutation and is therefore queued to the writer like every other one.
 *
 * Two rules shape the output:
 *
 *   - **Every result says whether the structural index is current**, and the
 *     generation it describes. A structural answer without that is a claim about
 *     a repository as it may no longer be.
 *   - **Resolution travels with every relation.** There is no field anywhere in
 *     here that reports a dependency or a call without also reporting how sure
 *     Atlas is of it.
 *
 * Symbol names, include spellings and paths are repository text. They are
 * bounded and safe-encoded on the way in and emitted through the streaming
 * writer, and the MCP tools that return them label the result UNTRUSTED_DATA.
 * None of them ever reaches automatic context.
 */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/code.h"
#include "atlas/pathrep.h"
#include "ipc/server_internal.h"


/* --- shared preamble --------------------------------------------------------
 *
 * Written by every method before its own fields, so no structural answer can be
 * read without its currency. Computed through `atlas_code_index_current`, which
 * is the single place that decides it. */
static atlas_status write_code_state(dispatch_state *ds, int64_t repo_id, atlas_err *err) {
    atlas_index_state fs;
    atlas_index_state_init(&fs);
    atlas_code_index_state cs;
    atlas_code_index_state_init(&cs);
    atlas_status st = atlas_db_index_state_get(ds->db, repo_id, &fs, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_get(ds->db, repo_id, &cs, err);
    }
    if (st != ATLAS_OK) {
        atlas_index_state_free(&fs);
        atlas_code_index_state_free(&cs);
        return st;
    }
    const char *file_reason = NULL;
    bool file_current = atlas_server_index_current(&fs, &file_reason);
    const char *reason = NULL;
    bool current = atlas_code_index_current(&fs, &cs, file_current, &reason);

    st = atlas_json_key_bool(ds->j, "index_current", file_current, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "code_index_current", current, err);
    }
    if (st == ATLAS_OK) {
        /* A fixed Atlas string, checked against its own vocabulary rather than
         * trusted to be one of them. */
        st = atlas_json_key_str_opt(
            ds->j, "code_not_current_reason",
            reason == NULL ? NULL
                           : (atlas_code_not_current_reason_is_known(reason) ? reason : "other"),
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "code_generation", cs.last_complete_generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation", fs.last_complete_generation, err);
    }
    atlas_index_state_free(&fs);
    atlas_code_index_state_free(&cs);
    return st;
}

/* Bounded row limit for a structural response. */
static int64_t code_limit(const atlas_ipc_request *req) {
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0) {
        return ATLAS_CODE_DEFAULT_ROWS;
    }
    return limit > ATLAS_CODE_MAX_ROWS ? ATLAS_CODE_MAX_ROWS : limit;
}

/* --- code.status -------------------------------------------------------------- */

static atlas_status method_code_status(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    atlas_code_index_state cs;
    atlas_code_index_state_init(&cs);
    st = atlas_db_code_state_get(ds->db, info.id, &cs, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = write_code_state(ds, info.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "files_indexed", cs.files_indexed, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "files_parsed_last", cs.files_parsed_last, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "symbols", cs.symbols, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "relations", cs.relations, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "ambiguous", cs.ambiguous, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved", cs.unresolved, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "degraded", cs.degraded, err);
    }
    if (st == ATLAS_OK) {
        /* Atlas-owned text, but it came out of a column, so it is encoded on the
         * way out like everything else that did. */
        st = atlas_json_key_str_opt(
            ds->j, "degraded_reason",
            cs.degraded ? atlas_safe(&ds->safe, atlas_buf_cstr(&cs.degraded_reason)) : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "compile_db_present", cs.compile_db_present, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "compile_units", cs.compile_units, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "compile_entries_dropped", cs.compile_entries_dropped, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "last_complete_at", cs.last_complete_at, err);
    }
    if (st == ATLAS_OK) {
        /* Not encoded, and it is worth saying why next to a `degraded_reason`
         * three lines up that is: this came out of a column too, but the only
         * writer of that column is `atlas_db_code_analyzer_intern`, whose
         * argument is a string literal in the Atlas binary. It is a fixed
         * vocabulary, which is the same standard the trust boundary applies to
         * every other value it lets through unescaped. */
        st = atlas_json_key_str(ds->j, "analyzer", atlas_buf_cstr(&cs.analyzer_name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "analyzer_version", cs.analyzer_version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "analyzer_expected", ATLAS_CODE_ANALYZER_ID, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "analyzer_version_expected",
                                (int64_t)ATLAS_CODE_ANALYZER_VERSION, err);
    }
    atlas_code_index_state_free(&cs);
    atlas_repo_info_free(&info);
    return st;
}

/* --- code.sync ---------------------------------------------------------------- */

static atlas_status method_code_sync(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    bool rebuild = false;
    (void)atlas_ipc_param_bool(req, "rebuild", &rebuild);

    /* Queued, not performed. A structural pass over a large repository takes
     * seconds; performing it here would stall every other client for that long.
     * The sequence number is what a caller waits on instead. */
    int64_t seq = 0;
    st = atlas_writer_submit_reconcile(ds->ctx->writer, info.id, false, rebuild, NULL, 0u, &seq,
                                       err);
    atlas_repo_info_free(&info);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key_bool(ds->j, "queued", true, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "sync_seq", seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "rebuild", rebuild, err);
    }
    return st;
}

/* --- emitters ------------------------------------------------------------------ */

static atlas_status emit_symbol(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", row->id, err);
    }
    if (st == ATLAS_OK) {
        /* Stored in the safe encoding; re-encoding would stop it decoding back
         * to the original bytes. */
        st = atlas_json_key_str(ds->j, "name", row->name_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", row->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "linkage", row->linkage, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "path", row->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "line", row->line, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "col", row->col, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "definition", row->is_definition, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "declaration", row->is_declaration, err);
    }
    if (st == ATLAS_OK) {
        /* CONDITIONAL here means the symbol was found under an `#if` Atlas did
         * not evaluate. It is a real fact about the bytes and a weaker one about
         * the program, and the field is how a reader can tell. */
        st = atlas_json_key_str(ds->j, "resolution", row->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "untrusted_data", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status emit_edge(const atlas_code_edge_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", row->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "from_kind", row->src_kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "from_path", row->src_path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "to_kind", row->dst_kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "to_path", row->dst_path_text, err);
    }
    if (st == ATLAS_OK) {
        /* The spelling, kept whether or not anything resolved it. An include
         * Atlas cannot place is still a fact worth reporting. */
        st = atlas_json_key_str_opt(ds->j, "spelling", row->dst_name_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "resolution", row->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "provenance", row->provenance, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "candidates", row->candidate_count, err);
    }
    if (st == ATLAS_OK) {
        /* One of the fixed ATLAS_CODE_WHY_* strings, checked rather than
         * trusted: it reaches a model's context through an MCP result. */
        const char *why = row->detail;
        st = atlas_json_key_str_opt(
            ds->j, "reason", why == NULL ? NULL : (atlas_code_why_is_known(why) ? why : "other"),
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "line", row->line, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "untrusted_data", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status emit_walk(const atlas_code_walk_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "depth", row->depth, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "node_kind", row->node_kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "node", row->label, err);
    }
    if (st == ATLAS_OK) {
        /* Why this candidate is here: the edge that reached it and the node it
         * was reached from. Every impact result carries its path, because an
         * impact result without one is an assertion. */
        st = atlas_json_key_str(ds->j, "via", row->via_label, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "edge", row->edge_kind, err);
    }
    if (st == ATLAS_OK) {
        /* The weakest resolution on the whole path. A candidate reached through
         * one ambiguous edge is an ambiguous candidate however exact the rest of
         * the chain was. */
        st = atlas_json_key_str(ds->j, "resolution", row->resolution, err);
    }
    if (st == ATLAS_OK) {
        const char *why = row->detail;
        st = atlas_json_key_str_opt(
            ds->j, "reason", why == NULL ? NULL : (atlas_code_why_is_known(why) ? why : "other"),
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "untrusted_data", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* --- code.file ------------------------------------------------------------------ */

typedef struct file_ctx {
    dispatch_state *ds;
    int64_t code_file_id;
    atlas_status st;
} file_ctx;

static atlas_status emit_role(const atlas_code_role_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "role", row->role, err);
    }
    if (st == ATLAS_OK) {
        /* How the role was arrived at. A file under `tests/` is *named* like a
         * test; that is a fact about the path and not proof about the file, and
         * this field is what keeps the difference visible. */
        st = atlas_json_key_str(ds->j, "basis", row->basis, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "resolution", row->resolution, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status emit_file_row(const atlas_code_file_row *row, void *ud, atlas_err *err) {
    file_ctx *fc = (file_ctx *)ud;
    dispatch_state *ds = fc->ds;
    fc->code_file_id = row->id;
    atlas_status st = atlas_json_key_bool(ds->j, "indexed", true, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "language", row->language, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "content_hash", row->content_hash, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "parse_status", row->parse_status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "parse_detail",
            row->parse_detail != NULL ? atlas_safe(&ds->safe, row->parse_detail) : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "truncated", row->truncated, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "truncated_reason",
            row->truncated_reason != NULL ? atlas_safe(&ds->safe, row->truncated_reason) : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "include_guard", row->include_guard, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "symbols", row->symbol_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "includes", row->include_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "call_candidates", row->occurrence_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "bytes", row->bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "lines", row->lines, err);
    }
    return st;
}

/* Streams one named list of edges into the response. */
static atlas_status write_edge_list(dispatch_state *ds, int64_t repo_id, const char *key,
                                    const char *node_kind, int64_t node_id, const char *kind,
                                    bool inbound, int64_t limit, atlas_err *err) {
    atlas_status st = atlas_json_key(ds->j, key, err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t n = 0;
    bool more = false;
    if (st == ATLAS_OK) {
        if (inbound) {
            st = atlas_db_code_edges_to(ds->db, repo_id, node_kind, node_id, kind, limit, emit_edge,
                                        ds, &n, &more, err);
        } else {
            st = atlas_db_code_edges_from(ds->db, repo_id, node_kind, node_id, kind, limit,
                                          emit_edge, ds, &n, &more, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        char more_key[64];
        (void)snprintf(more_key, sizeof(more_key), "%s_more", key);
        st = atlas_json_key_bool(ds->j, more_key, more, err);
    }
    return st;
}

static atlas_status method_code_file(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    const char *path = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "path", &path)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "code.file needs a \"path\" parameter");
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_path_text_decode(path, strlen(path), &raw, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_path_check_relative(raw.data, raw.len, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&raw);
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = code_limit(req);

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "path", path, err);
    }
    if (st == ATLAS_OK) {
        st = write_code_state(ds, info.id, err);
    }

    file_ctx fc;
    memset(&fc, 0, sizeof(fc));
    fc.ds = ds;
    bool found = false;
    if (st == ATLAS_OK) {
        st = atlas_db_code_file_get(ds->db, info.id, raw.data, raw.len, emit_file_row, &fc, &found,
                                    NULL, err);
    }
    if (st == ATLAS_OK && !found) {
        /* Not indexed is a fact, not an error. A `.md` file is never
         * structurally indexed, and reporting that beats an empty answer that
         * reads like a missing file. */
        st = atlas_json_key_bool(ds->j, "indexed", false, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "reason",
                                    "Atlas extracts structure from C sources, headers and "
                                    "included fragments only",
                                    err);
        }
        atlas_buf_free(&raw);
        atlas_repo_info_free(&info);
        return st;
    }

    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "roles", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t roles = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_code_roles_of(ds->db, fc.code_file_id, emit_role, ds, &roles, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }

    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "symbols_defined", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t nsym = 0;
    bool smore = false;
    if (st == ATLAS_OK) {
        st = atlas_db_code_symbols_in_file(ds->db, fc.code_file_id, limit, emit_symbol, ds, &nsym,
                                           &smore, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "symbols_more", smore, err);
    }

    if (st == ATLAS_OK) {
        st = write_edge_list(ds, info.id, "includes", "file", fc.code_file_id,
                             "file_includes_file", false, limit, err);
    }
    if (st == ATLAS_OK) {
        st = write_edge_list(ds, info.id, "depends_on", "file", fc.code_file_id,
                             "file_depends_on_file", false, limit, err);
    }
    if (st == ATLAS_OK) {
        st = write_edge_list(ds, info.id, "depended_on_by", "file", fc.code_file_id,
                             "file_depends_on_file", true, limit, err);
    }
    if (st == ATLAS_OK) {
        st = write_edge_list(ds, info.id, "translation_units", "file", fc.code_file_id, NULL, true,
                             limit, err);
    }

    int64_t ambiguous = 0;
    int64_t unresolved = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_code_file_unsettled(ds->db, fc.code_file_id, &ambiguous, &unresolved, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "ambiguous", ambiguous, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved", unresolved, err);
    }
    if (st == ATLAS_OK) {
        /* A3 says nothing about *why* a file exists. That is still UNKNOWN
         * unless somebody recorded a reason through the A2 tools, and the
         * structural facts here do not weaken it. */
        st = atlas_json_key_str(ds->j, "reason", "UNKNOWN", err);
    }
    atlas_buf_free(&raw);
    atlas_repo_info_free(&info);
    return st;
}

/* --- code.symbol.search --------------------------------------------------------- */

static atlas_status method_code_symbol_search(dispatch_state *ds, const atlas_ipc_request *req,
                                              atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    const char *query = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "query", &query)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "code.symbol.search needs a \"query\" parameter");
    }
    const char *kind = NULL;
    if (st == ATLAS_OK && atlas_ipc_param_str(req, "kind", &kind)) {
        atlas_code_symbol_kind parsed;
        if (!atlas_code_symbol_kind_parse(kind, &parsed)) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "\"kind\" is not a symbol kind Atlas records");
        }
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    if (strlen(query) > 256u) {
        atlas_repo_info_free(&info);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"query\" is longer than 256 bytes");
    }

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = write_code_state(ds, info.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "symbols", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t n = 0;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_code_symbol_search(ds->db, info.id, query, kind, code_limit(req), emit_symbol,
                                         ds, &n, &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", n, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- code.symbol ----------------------------------------------------------------- */

typedef struct site_list {
    int64_t ids[ATLAS_CODE_MAX_CANDIDATES];
    size_t count;
    dispatch_state *ds;
    atlas_status st;
} site_list;

static atlas_status emit_site(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    site_list *sl = (site_list *)ud;
    if (sl->count < ATLAS_CODE_MAX_CANDIDATES) {
        sl->ids[sl->count++] = row->id;
    }
    return emit_symbol(row, sl->ds, err);
}

/* The same collection without the emission: a traversal's start node is not a
 * result, so it is not written into the response. */
static atlas_status collect_site(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    site_list *sl = (site_list *)ud;
    (void)err;
    if (sl->count < ATLAS_CODE_MAX_CANDIDATES) {
        sl->ids[sl->count++] = row->id;
    }
    return ATLAS_OK;
}

static atlas_status method_code_symbol(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    const char *symbol = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "symbol", &symbol)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "code.symbol needs a \"symbol\" parameter");
    }
    if (st == ATLAS_OK && strlen(symbol) > ATLAS_CODE_MAX_NAME_BYTES) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"symbol\" is longer than %u bytes",
                           (unsigned)ATLAS_CODE_MAX_NAME_BYTES);
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t limit = code_limit(req);

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "symbol", symbol, err);
    }
    if (st == ATLAS_OK) {
        st = write_code_state(ds, info.id, err);
    }

    /* Every recorded site, not one. Two files' identically named statics are two
     * symbols, and answering with one of them would be choosing arbitrarily
     * between things Atlas has deliberately kept distinct. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "sites", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    site_list sl;
    memset(&sl, 0, sizeof(sl));
    sl.ds = ds;
    int64_t n = 0;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_code_symbols_by_name(ds->db, info.id, symbol, limit, emit_site, &sl, &n,
                                           &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "site_count", n, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "sites_more", more, err);
    }

    /* Callers and callees, gathered across every site. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "callers", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < sl.count; i++) {
        int64_t got = 0;
        bool m = false;
        st = atlas_db_code_edges_to(ds->db, info.id, "symbol", sl.ids[i], "symbol_calls_symbol",
                                    limit, emit_edge, ds, &got, &m, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "calls", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < sl.count; i++) {
        int64_t got = 0;
        bool m = false;
        st = atlas_db_code_edges_from(ds->db, info.id, "symbol", sl.ids[i], "symbol_calls_symbol",
                                      limit, emit_edge, ds, &got, &m, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- code.deps and code.impact ---------------------------------------------------- */

/* Both are the same bounded walk, and the only difference is the direction.
 * `code.deps` asks what a thing depends on; `code.impact` asks what may be
 * affected if it changes. Sharing the implementation is deliberate: two would
 * eventually give different answers to the same graph. */
static atlas_status run_walk(dispatch_state *ds, const atlas_ipc_request *req, bool inbound,
                             atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    const char *path = NULL;
    const char *symbol = NULL;
    (void)atlas_ipc_param_str(req, "path", &path);
    (void)atlas_ipc_param_str(req, "symbol", &symbol);
    if (path == NULL && symbol == NULL) {
        atlas_repo_info_free(&info);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a \"path\" or a \"symbol\" is required");
    }
    int64_t depth = 0;
    (void)atlas_ipc_param_int(req, "depth", &depth);

    atlas_code_walk_opts opts;
    atlas_code_walk_opts_init(&opts);
    opts.inbound = inbound;
    opts.depth = depth;
    opts.max_nodes = code_limit(req);

    if (path != NULL) {
        atlas_buf raw = ATLAS_BUF_INIT;
        st = atlas_path_text_decode(path, strlen(path), &raw, err);
        if (st == ATLAS_OK) {
            st = atlas_path_check_relative(raw.data, raw.len, err);
        }
        bool found = false;
        int64_t id = 0;
        if (st == ATLAS_OK) {
            st = atlas_db_code_file_get(ds->db, info.id, raw.data, raw.len, NULL, NULL, &found, &id,
                                        err);
        }
        if (st == ATLAS_OK && !found) {
            st = atlas_err_set(err, ATLAS_ERR_REPO, "\"%s\" is not structurally indexed",
                               atlas_safe(&ds->safe, path));
        }
        atlas_buf_free(&raw);
        opts.start_kind = ATLAS_CODE_NODE_FILE;
        opts.start_id = id;
        opts.follow_symbols = false;
    } else {
        site_list sl;
        memset(&sl, 0, sizeof(sl));
        sl.ds = ds;
        int64_t n = 0;
        bool more = false;
        /* Collected without emitting: this is the traversal's start, not a
         * result. */
        st = atlas_db_code_symbols_by_name(ds->db, info.id, symbol, 2, collect_site, &sl, &n, &more,
                                           err);
        if (st == ATLAS_OK && sl.count == 0) {
            st = atlas_err_set(err, ATLAS_ERR_REPO, "no symbol named \"%s\" is recorded",
                               atlas_safe(&ds->safe, symbol));
        }
        if (st == ATLAS_OK) {
            opts.start_kind = ATLAS_CODE_NODE_SYMBOL;
            opts.start_id = sl.ids[0];
            opts.follow_files = false;
        }
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "path", path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "symbol", symbol, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "direction", inbound ? "inbound" : "outbound", err);
    }
    if (st == ATLAS_OK) {
        st = write_code_state(ds, info.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "candidates", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    atlas_code_walk_summary sum;
    memset(&sum, 0, sizeof(sum));
    if (st == ATLAS_OK) {
        st = atlas_code_walk(ds->db, info.id, &opts, emit_walk, ds, &sum, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", sum.emitted, err);
    }
    /* Split by how sure Atlas is, because merging them would be exactly the
     * conflation A3 exists to prevent. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "exact", sum.exact, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unique_lexical", sum.unique_lexical, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "ambiguous", sum.ambiguous, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved", sum.unresolved, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "truncated", sum.truncated, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "truncated_reason", sum.truncated_reason, err);
    }
    if (st == ATLAS_OK) {
        /* Said in the result rather than left to a tool description, because
         * this is the sentence that stops an impact list being read as a
         * prediction. */
        st = atlas_json_key_str(ds->j, "notice",
                                "These are graph paths, not predictions. Atlas is not a compiler: "
                                "a candidate here shares a recorded structural relation with what "
                                "you named, and may or may not be affected by changing it.",
                                err);
    }
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status method_code_deps(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    return run_walk(ds, req, false, err);
}

static atlas_status method_code_impact(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    return run_walk(ds, req, true, err);
}

/* --- the group -------------------------------------------------------------------- */

static const atlas_method_entry CODE_METHODS[] = {
    {"code.status", method_code_status},
    {"code.sync", method_code_sync},
    {"code.file", method_code_file},
    {"code.symbol.search", method_code_symbol_search},
    {"code.symbol", method_code_symbol},
    {"code.deps", method_code_deps},
    {"code.impact", method_code_impact},
};

const atlas_method_entry *atlas_server_code_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(CODE_METHODS) / sizeof(CODE_METHODS[0]);
    }
    return CODE_METHODS;
}

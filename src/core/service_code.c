/* Atlas - the service layer for structural code intelligence.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * All A3 command behaviour lives here. The CLI parses arguments and picks a
 * renderer; the renderers format what these produce. Neither reaches past this
 * layer, which is what keeps human and JSON output structurally incapable of
 * disagreeing.
 *
 * These answer even when the structural index is not current, and say so. The
 * alternative — refusing — leaves a caller with nothing when it could have had
 * stale-but-labelled facts, and a labelled stale answer is strictly more useful
 * than a silence.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/pathrep.h"
#include "core/service_internal.h"

void atlas_code_status_report_init(atlas_code_status_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_index_state_init(&r->file_state);
    atlas_code_index_state_init(&r->code_state);
}

void atlas_code_status_report_free(atlas_code_status_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    atlas_index_state_free(&r->file_state);
    atlas_code_index_state_free(&r->code_state);
}

void atlas_code_file_report_init(atlas_code_file_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->path_text);
    atlas_buf_init(&r->parse_detail);
    atlas_buf_init(&r->truncated_reason);
}

void atlas_code_file_report_free(atlas_code_file_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->path_text);
    atlas_buf_free(&r->parse_detail);
    atlas_buf_free(&r->truncated_reason);
}

/* The file index's own currency, computed the same way the daemon computes it.
 *
 * Duplicated logic would be two answers to one question, so this mirrors
 * `atlas_server_index_current` exactly — including the strings, which are a
 * fixed Atlas vocabulary because they reach a model's context through
 * `ai.context`. */
static bool file_index_current(const atlas_index_state *s, const char **reason_out) {
    if (!s->present || s->last_complete_generation <= 0) {
        *reason_out = "no reconciliation pass has completed for this repository yet";
        return false;
    }
    if (s->event_gap) {
        *reason_out = "an unresolved event gap means Atlas cannot prove it observed every change";
        return false;
    }
    if (s->pending_full_reconcile) {
        *reason_out = "a full content verification is owed and has not completed";
        return false;
    }
    if (s->watch_state == ATLAS_WATCH_ERROR) {
        *reason_out = "the filesystem watcher failed and is not observing this repository";
        return false;
    }
    if (s->watch_state == ATLAS_WATCH_DEGRADED) {
        *reason_out = "the filesystem watcher is running with a known blind spot";
        return false;
    }
    *reason_out = NULL;
    return true;
}

atlas_status atlas_service_code_status(atlas_ctx *ctx, const char *name,
                                       atlas_code_status_report *out, atlas_err *err) {
    atlas_status st = atlas_service_require_repo(ctx, name, &out->repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_index_state_get(atlas_ctx_db(ctx), out->repo.id, &out->file_state, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_get(atlas_ctx_db(ctx), out->repo.id, &out->code_state, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    const char *file_reason = NULL;
    out->file_index_current = file_index_current(&out->file_state, &file_reason);
    out->code_index_current = atlas_code_index_current(&out->file_state, &out->code_state,
                                                       out->file_index_current,
                                                       &out->not_current_reason);
    return ATLAS_OK;
}

/* --- one file ------------------------------------------------------------- */

typedef struct role_sink {
    atlas_code_file_report *rep;
} role_sink;

static atlas_status take_role(const atlas_code_role_row *row, void *ud, atlas_err *err) {
    role_sink *s = (role_sink *)ud;
    (void)err;
    if (s->rep->role_count >= ATLAS_CODE_MAX_ROLES_PER_FILE) {
        return ATLAS_OK;
    }
    atlas_code_role_entry *e = &s->rep->roles[s->rep->role_count];
    (void)snprintf(e->role, sizeof(e->role), "%s", row->role);
    (void)snprintf(e->basis, sizeof(e->basis), "%s", row->basis);
    (void)snprintf(e->resolution, sizeof(e->resolution), "%s", row->resolution);
    s->rep->role_count++;
    return ATLAS_OK;
}

typedef struct file_sink {
    atlas_code_file_report *rep;
    atlas_err *err;
    atlas_status st;
} file_sink;

static atlas_status take_file(const atlas_code_file_row *row, void *ud, atlas_err *err) {
    file_sink *s = (file_sink *)ud;
    atlas_code_file_report *r = s->rep;
    r->indexed = true;
    r->code_file_id = row->id;
    atlas_status st = atlas_buf_set_str(&r->path_text, row->path_text, err);
    if (st == ATLAS_OK && row->parse_detail != NULL) {
        st = atlas_buf_set_str(&r->parse_detail, row->parse_detail, err);
    }
    if (st == ATLAS_OK && row->truncated_reason != NULL) {
        st = atlas_buf_set_str(&r->truncated_reason, row->truncated_reason, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(r->language, sizeof(r->language), "%s", row->language);
    (void)snprintf(r->parse_status, sizeof(r->parse_status), "%s", row->parse_status);
    (void)snprintf(r->content_hash, sizeof(r->content_hash), "%s",
                   row->content_hash != NULL ? row->content_hash : "");
    r->truncated = row->truncated;
    r->include_guard = row->include_guard;
    r->symbol_count = row->symbol_count;
    r->include_count = row->include_count;
    r->occurrence_count = row->occurrence_count;
    r->bytes = row->bytes;
    r->lines = row->lines;
    r->generation = row->generation;
    return ATLAS_OK;
}

/* Decodes a path argument. Accepted in either the safe text form Atlas prints
 * or the plain form, because a caller pasting back what Atlas printed has to
 * work. Refused when it is not repository-relative. */
static atlas_status decode_path(const char *path, atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_path_text_decode(path, strlen(path), out, err);
    if (st == ATLAS_OK) {
        st = atlas_path_check_relative(out->data, out->len, err);
    }
    return st;
}

atlas_status atlas_service_code_file(atlas_ctx *ctx, const char *name, const char *path,
                                     atlas_code_file_report *out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = decode_path(path, &raw, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&raw);
        atlas_repo_info_free(&info);
        return st;
    }

    /* The repository's structural currency travels with the file context, so a
     * caller reading one answer is not left having to make a second call before
     * it knows whether to trust it. */
    atlas_index_state fs;
    atlas_index_state_init(&fs);
    atlas_code_index_state cs;
    atlas_code_index_state_init(&cs);
    st = atlas_db_index_state_get(atlas_ctx_db(ctx), info.id, &fs, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_get(atlas_ctx_db(ctx), info.id, &cs, err);
    }
    if (st == ATLAS_OK) {
        const char *ignore = NULL;
        bool file_ok = file_index_current(&fs, &ignore);
        out->code_index_current =
            atlas_code_index_current(&fs, &cs, file_ok, &out->not_current_reason);
    }

    file_sink fsk = {out, err, ATLAS_OK};
    bool found = false;
    if (st == ATLAS_OK) {
        st = atlas_db_code_file_get(atlas_ctx_db(ctx), info.id, raw.data, raw.len, take_file, &fsk,
                                    &found, NULL, err);
    }
    if (st == ATLAS_OK && found) {
        role_sink rs = {out};
        int64_t n = 0;
        st = atlas_db_code_roles_of(atlas_ctx_db(ctx), out->code_file_id, take_role, &rs, &n, err);
    }
    if (st == ATLAS_OK && found) {
        st = atlas_db_code_file_unsettled(atlas_ctx_db(ctx), out->code_file_id, &out->ambiguous,
                                          &out->unresolved, err);
    }
    if (st == ATLAS_OK && !found) {
        /* Not indexed is a fact, not an error: a `.md` file is never
         * structurally indexed and saying so is the right answer. */
        st = atlas_buf_set_str(&out->path_text, path, err);
    }
    atlas_code_index_state_free(&cs);
    atlas_index_state_free(&fs);
    atlas_buf_free(&raw);
    atlas_repo_info_free(&info);
    return st;
}

/* Resolves a repository and a path to a `code_files` id. */
static atlas_status resolve_code_file(atlas_ctx *ctx, const char *name, const char *path,
                                      int64_t *repo_id_out, int64_t *id_out, atlas_err *err) {
    *repo_id_out = 0;
    *id_out = 0;
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = decode_path(path, &raw, err);
    }
    bool found = false;
    if (st == ATLAS_OK) {
        *repo_id_out = info.id;
        st = atlas_db_code_file_get(atlas_ctx_db(ctx), info.id, raw.data, raw.len, NULL, NULL,
                                    &found, id_out, err);
    }
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "\"%s\" is not structurally indexed in \"%s\". Atlas extracts structure "
                           "from C sources and headers only; run `atlas code status %s` to see "
                           "whether the structural index is current.",
                           path, name, name);
    }
    atlas_buf_free(&raw);
    atlas_repo_info_free(&info);
    return st;
}

atlas_status atlas_service_code_file_symbols(atlas_ctx *ctx, const char *name, const char *path,
                                             int64_t limit, atlas_code_symbol_cb cb, void *ud,
                                             int64_t *count_out, bool *more_out, atlas_err *err) {
    int64_t repo_id = 0;
    int64_t id = 0;
    atlas_status st = resolve_code_file(ctx, name, path, &repo_id, &id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_db_code_symbols_in_file(atlas_ctx_db(ctx), id, limit, cb, ud, count_out, more_out,
                                         err);
}

atlas_status atlas_service_code_file_edges(atlas_ctx *ctx, const char *name, const char *path,
                                           const char *kind, bool inbound, int64_t limit,
                                           atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                                           bool *more_out, atlas_err *err) {
    int64_t repo_id = 0;
    int64_t id = 0;
    atlas_status st = resolve_code_file(ctx, name, path, &repo_id, &id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (inbound) {
        return atlas_db_code_edges_to(atlas_ctx_db(ctx), repo_id, "file", id, kind, limit, cb, ud,
                                      count_out, more_out, err);
    }
    return atlas_db_code_edges_from(atlas_ctx_db(ctx), repo_id, "file", id, kind, limit, cb, ud,
                                    count_out, more_out, err);
}

atlas_status atlas_service_code_symbol_search(atlas_ctx *ctx, const char *name, const char *query,
                                              const char *kind, int64_t limit,
                                              atlas_code_symbol_cb cb, void *ud,
                                              int64_t *count_out, bool *more_out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_symbol_search(atlas_ctx_db(ctx), info.id, query, kind, limit, cb, ud,
                                         count_out, more_out, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

atlas_status atlas_service_code_symbol_sites(atlas_ctx *ctx, const char *name, const char *symbol,
                                             int64_t limit, atlas_code_symbol_cb cb, void *ud,
                                             int64_t *count_out, bool *more_out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_symbols_by_name(atlas_ctx_db(ctx), info.id, symbol, limit, cb, ud,
                                           count_out, more_out, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* Collects every recorded site of one symbol name, so callers and callees can be
 * gathered across all of them.
 *
 * Several sites is the normal answer, not an error: two files' identically named
 * statics are two symbols, and a caller asking "who calls helper" wants both
 * sets rather than an arbitrary one. */
typedef struct site_ids {
    int64_t ids[ATLAS_CODE_MAX_CANDIDATES];
    size_t count;
    bool truncated;
} site_ids;

static atlas_status take_site(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    site_ids *s = (site_ids *)ud;
    (void)err;
    if (s->count >= ATLAS_CODE_MAX_CANDIDATES) {
        s->truncated = true;
        return ATLAS_OK;
    }
    s->ids[s->count++] = row->id;
    return ATLAS_OK;
}

atlas_status atlas_service_code_symbol_edges(atlas_ctx *ctx, const char *name, const char *symbol,
                                             bool inbound, int64_t limit, atlas_code_edge_cb cb,
                                             void *ud, int64_t *count_out, bool *more_out,
                                             atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    site_ids sites;
    memset(&sites, 0, sizeof(sites));
    int64_t n = 0;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_code_symbols_by_name(atlas_ctx_db(ctx), info.id, symbol,
                                           ATLAS_CODE_MAX_CANDIDATES, take_site, &sites, &n, &more,
                                           err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < sites.count; i++) {
        int64_t got = 0;
        bool m = false;
        if (inbound) {
            st = atlas_db_code_edges_to(atlas_ctx_db(ctx), info.id, "symbol", sites.ids[i],
                                        "symbol_calls_symbol", limit, cb, ud, &got, &m, err);
        } else {
            st = atlas_db_code_edges_from(atlas_ctx_db(ctx), info.id, "symbol", sites.ids[i],
                                          "symbol_calls_symbol", limit, cb, ud, &got, &m, err);
        }
        *count_out += got;
        if (m) {
            *more_out = true;
        }
    }
    if (sites.truncated || more) {
        *more_out = true;
    }
    atlas_repo_info_free(&info);
    return st;
}

atlas_status atlas_service_code_walk(atlas_ctx *ctx, const char *name, const char *path,
                                     const char *symbol, bool inbound, int64_t depth,
                                     int64_t limit, atlas_code_walk_cb cb, void *ud,
                                     atlas_code_walk_summary *sum, atlas_err *err) {
    memset(sum, 0, sizeof(*sum));
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_code_walk_opts opts;
    atlas_code_walk_opts_init(&opts);
    opts.inbound = inbound;
    opts.depth = depth;
    opts.max_nodes = limit;

    if (path != NULL) {
        int64_t repo_id = 0;
        int64_t id = 0;
        st = resolve_code_file(ctx, name, path, &repo_id, &id, err);
        opts.start_kind = ATLAS_CODE_NODE_FILE;
        opts.start_id = id;
        /* A file walk follows file dependencies. Symbol edges from a file node
         * would jump between two different kinds of "depends on" in one result,
         * and a caller asking about a file means files. */
        opts.follow_symbols = false;
    } else if (symbol != NULL) {
        site_ids sites;
        memset(&sites, 0, sizeof(sites));
        int64_t n = 0;
        bool more = false;
        st = atlas_db_code_symbols_by_name(atlas_ctx_db(ctx), info.id, symbol, 2, take_site, &sites,
                                           &n, &more, err);
        if (st == ATLAS_OK && sites.count == 0) {
            st = atlas_err_set(err, ATLAS_ERR_REPO,
                               "no symbol named \"%s\" is recorded in \"%s\"", symbol, name);
        }
        if (st == ATLAS_OK) {
            /* Starting from the first recorded site, which is the definition
             * when there is one — the query orders definitions first. When a
             * name has several sites the caller is told so by
             * `atlas code symbol`, which lists them all; a traversal has to
             * start somewhere and says where. */
            opts.start_kind = ATLAS_CODE_NODE_SYMBOL;
            opts.start_id = sites.ids[0];
            opts.follow_files = false;
        }
    } else {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "a path or a symbol is required");
    }
    if (st == ATLAS_OK) {
        st = atlas_code_walk(atlas_ctx_db(ctx), info.id, &opts, cb, ud, sum, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- structural sync ------------------------------------------------------- */

atlas_status atlas_service_code_sync(atlas_ctx *ctx, const char *name, bool rebuild, bool wait,
                                     int timeout_ms, atlas_sync_report *out, atlas_err *err) {
    if (!rebuild) {
        /* An ordinary structural sync is an ordinary reconciliation: the
         * structural stage is part of the pass, and the hash comparison already
         * finds everything that changed. There is deliberately no second code
         * path for it, because a second one would eventually behave
         * differently. */
        return atlas_service_sync(ctx, name, false, wait, timeout_ms, out, err);
    }

    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_err sock_err;
    atlas_err_init(&sock_err);
    bool have_socket = (atlas_ipc_socket_path(&sock, &sock_err) == ATLAS_OK);
    /* Routed to the daemon like every other mutation, so the single writer
     * stays single — but only to the daemon that owns *this* context's data
     * directory. One socket serves a user; a data directory is chosen per
     * invocation. */
    if (have_socket && atlas_ipc_daemon_owns(atlas_ctx_data_dir(ctx))) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        atlas_buf params = ATLAS_BUF_INIT;
        atlas_buf resp = ATLAS_BUF_INIT;
        atlas_status st = atlas_db_check_repo_name(name, err);
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_begin(&p, &j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "repo", name, err);
            if (st == ATLAS_OK) {
                st = atlas_json_key_bool(j, "rebuild", true, err);
            }
            if (st == ATLAS_OK) {
                st = atlas_ipc_params_finish(p, &params, err);
            } else {
                atlas_ipc_params_abort(p);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_call(atlas_buf_cstr(&sock), "code.sync", atlas_buf_cstr(&params), &resp,
                                err);
        }
        atlas_ipc_response *r = NULL;
        if (st == ATLAS_OK) {
            st = atlas_ipc_response_parse(resp.data, resp.len, &r, err);
        }
        if (st == ATLAS_OK && !atlas_ipc_response_ok(r)) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", atlas_ipc_response_message(r));
        }
        if (st == ATLAS_OK) {
            out->via_daemon = true;
            (void)atlas_ipc_result_int(r, "sync_seq", &out->sync_seq);
        }
        atlas_ipc_response_free(r);
        atlas_buf_free(&params);
        atlas_buf_free(&resp);
        atlas_buf_free(&sock);
        if (st != ATLAS_OK || !wait) {
            return st;
        }
        /* Waiting reuses the ordinary sync poll rather than a second one. */
        atlas_sync_report poll;
        atlas_sync_report_init(&poll);
        st = atlas_service_sync(ctx, name, false, true, timeout_ms, &poll, err);
        if (st == ATLAS_OK) {
            out->waited = true;
            out->completed = poll.completed;
            out->generation = poll.generation;
        }
        atlas_sync_report_free(&poll);
        return st;
    }
    atlas_buf_free(&sock);

    /* Offline. The same pass the daemon would run, on this thread, holding the
     * writer lock the context already took. */
    if (!atlas_ctx_is_writer(ctx)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "another Atlas writer owns the index and no daemon is answering on "
                             "the IPC socket. Start the daemon (systemctl --user start atlas) or "
                             "wait for the other command to finish.");
    }
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    atlas_git *g = NULL;
    if (st == ATLAS_OK) {
        st = atlas_service_open_repo_git(&info, &g, err);
    }
    if (st == ATLAS_OK) {
        atlas_reconcile_opts opts;
        atlas_reconcile_opts_init(&opts);
        opts.code_rebuild = true;
        opts.workers = NULL;
        st = atlas_reconcile_run(atlas_ctx_db(ctx), g, info.id, &opts, &out->summary, err);
    }
    atlas_git_close(g);
    if (st == ATLAS_OK) {
        out->via_daemon = false;
        out->completed = out->summary.published;
        out->generation = out->summary.generation;
    }
    atlas_repo_info_free(&info);
    return st;
}

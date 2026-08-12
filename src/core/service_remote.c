/* Atlas - the read commands, answered by the daemon instead of by this process.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * ## Why this file exists
 *
 * A7.1 puts the index behind a separate OS principal: `/var/lib/atlas` is 0700
 * `atlasd`, and it has to stay that way because `atlas-worker` is a member of
 * the client group and A7.1's guarantee is that it cannot read the index. So a
 * client uid can never open that database, whatever Atlas does. The socket is
 * the only read path.
 *
 * ## What this file is not
 *
 * It is **not a second implementation of any command**. Every function here
 * fills the same report struct its local twin fills, and the CLI calls it from
 * the same call site behind `ctx == NULL`, so both renderers, the human output
 * and the JSON contract are shared rather than reproduced. Anything a reader
 * would have to keep in step in two places would eventually stop being in step.
 *
 * ## What it must never do
 *
 * Fall back to a local read. A7.1's rule is that a client which cannot reach
 * the daemon fails, rather than quietly answering from the pre-cutover per-user
 * database that the cutover deliberately leaves in place as a rollback target.
 * Every function here is socket-or-error.
 *
 * ## Encoding
 *
 * Values arrive already safe-encoded — the daemon encoded them at the point of
 * output. They are copied into the struct fields both renderers treat as
 * *already encoded* and are never encoded again; double-encoding is silent and
 * makes a path stop decoding back to its original bytes. Where the local path
 * stores raw bytes instead — a repository root, which git is addressed with —
 * the `%XX` form is decoded back rather than used as text.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/backup.h"
#include "atlas/code.h"
#include "atlas/gate.h"
#include "atlas/ipc.h"
#include "atlas/pathrep.h"
#include "atlas/maintenance.h"
#include "atlas/service.h"
#include "core/service_internal.h"

static int64_t monotonic_ms(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One request/response round trip. The only place a socket is opened. */
static atlas_status atlas_remote_call(const char *method, const char *params, atlas_buf *raw,
                               atlas_ipc_response **out, atlas_err *err) {
    *out = NULL;
    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_status st = atlas_ipc_socket_path(&sock, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&sock), method, params, raw, err);
    }
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

/* `{"repo":"…","path":"…"}` and optionally a limit. The path travels in the
 * reversible `%XX` form every Atlas path input accepts — which is what the CLI
 * was given — so nothing has to agree about raw bytes across the socket. */
static atlas_status repo_path_params(const char *name, const char *path, int64_t limit,
                                     atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_db_check_repo_name(name, err);
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key_str(j, "repo", name, err);
    if (st == ATLAS_OK && path != NULL) {
        st = atlas_json_key_str(j, "path", path, err);
    }
    if (st == ATLAS_OK && limit > 0) {
        st = atlas_json_key_int(j, "limit", limit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, out, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    return st;
}

static void copy_str(char *dst, size_t n, const char *src) {
    if (src != NULL) {
        (void)snprintf(dst, n, "%s", src);
    }
}

/* --- search ---------------------------------------------------------------- */

atlas_status atlas_service_search_remote(const char *name, const char *query, int64_t limit,
                                         atlas_search_mode *mode_out, atlas_search_cb cb, void *ud,
                                         int64_t *count_out, atlas_err *err) {
    *count_out = 0;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "query", query, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("repo.search", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    /* The mode is the daemon's answer about its own database's capabilities.
     * The local path reads it from the handle it opened; there is no handle
     * here, and guessing would report FTS5 on an index that has none. */
    const char *m = NULL;
    bool degraded = false;
    if (atlas_ipc_result_bool(r, "degraded", &degraded)) {
        *mode_out = degraded ? ATLAS_SEARCH_DEGRADED_LIKE : ATLAS_SEARCH_FTS5;
    } else if (atlas_ipc_result_str(r, "search_mode", &m) && m != NULL) {
        *mode_out = strcmp(m, "fts5") == 0 ? ATLAS_SEARCH_FTS5 : ATLAS_SEARCH_DEGRADED_LIKE;
    }

    /* `cb == NULL` is the mode probe the CLI performs before it prints the
     * header: the search mode belongs above the results in both output forms,
     * and there is no local database to read it from. The results of that first
     * call are discarded rather than held, so the rendered page is the one the
     * second call returned and no page is ever half from each. */
    size_t n = 0;
    if (cb == NULL) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return ATLAS_OK;
    }
    (void)atlas_ipc_result_arr_len(r, "results", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_search_hit h;
        memset(&h, 0, sizeof h);
        const char *v = NULL;
        int64_t t = 0;
        bool b = false;
        h.kind = atlas_ipc_result_arr_obj_str(r, "results", i, "kind", &v) ? v : "file";
        if (atlas_ipc_result_arr_obj_str(r, "results", i, "path", &v)) {
            h.path_text = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "results", i, "commit", &v)) {
            h.commit_oid = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "results", i, "subject", &v)) {
            h.subject = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "results", i, "author", &v)) {
            h.author_name = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "results", i, "git_index_oid", &v)) {
            h.git_index_oid = v;
        }
        h.evidence = atlas_ipc_result_arr_obj_str(r, "results", i, "evidence", &v) ? v : "";
        if (atlas_ipc_result_arr_obj_int(r, "results", i, "time", &t)) {
            h.author_time = t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "results", i, "deleted", &b)) {
            h.deleted = b;
        }
        /* Borrowed for the duration of the call, exactly as a row callback's
         * pointers are borrowed from a live statement. */
        st = cb(&h, ud, err);
        if (st == ATLAS_OK) {
            (*count_out)++;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- events ---------------------------------------------------------------- */

atlas_status atlas_service_events_remote(const char *name, int64_t since, int64_t limit,
                                         atlas_event_cb cb, void *ud, int64_t *count_out,
                                         int64_t *next_cursor_out, bool *more_out,
                                         atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "since", since, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("events.since", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "events", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_event_row row;
        memset(&row, 0, sizeof row);
        const char *v = NULL;
        int64_t t = 0;
        if (atlas_ipc_result_arr_obj_int(r, "events", i, "cursor", &t)) {
            row.id = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "events", i, "generation", &t)) {
            row.generation = t;
        }
        row.kind = atlas_ipc_result_arr_obj_str(r, "events", i, "kind", &v) ? v : "";
        if (atlas_ipc_result_arr_obj_str(r, "events", i, "path", &v)) {
            row.path_text = v; /* already the safe path encoding */
        }
        if (atlas_ipc_result_arr_obj_str(r, "events", i, "detail", &v)) {
            row.detail = v;
        }
        row.created_at = atlas_ipc_result_arr_obj_str(r, "events", i, "at", &v) ? v : "";
        st = cb(&row, ud, err);
        if (st == ATLAS_OK) {
            (*count_out)++;
        }
    }
    int64_t cursor = since;
    if (atlas_ipc_result_int(r, "cursor", &cursor)) {
        *next_cursor_out = cursor;
    }
    bool more = false;
    if (atlas_ipc_result_bool(r, "more", &more)) {
        *more_out = more;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- decision list --------------------------------------------------------- */

atlas_status atlas_service_decision_list_remote(const char *repo,
                                                const atlas_decision_list_opts *opts,
                                                atlas_decision_summary_cb cb, void *ud,
                                                int64_t *count_out, bool *more_out,
                                                atlas_decision_counts *counts_out,
                                                atlas_err *err) {
    *count_out = 0;
    *more_out = false;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(repo, err);
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", repo, err);
        if (st == ATLAS_OK && opts != NULL && opts->status != NULL && opts->status[0] != '\0') {
            st = atlas_json_key_str(j, "status", opts->status, err);
        }
        if (st == ATLAS_OK && opts != NULL && opts->query != NULL && opts->query[0] != '\0') {
            st = atlas_json_key_str(j, "query", opts->query, err);
        }
        if (st == ATLAS_OK && opts != NULL && opts->path != NULL && opts->path[0] != '\0') {
            st = atlas_json_key_str(j, "path", opts->path, err);
        }
        if (st == ATLAS_OK && opts != NULL && opts->limit > 0) {
            st = atlas_json_key_int(j, "limit", opts->limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("decision.list", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "decisions", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_decision_summary sum;
        atlas_decision_summary_init(&sum);
        const char *v = NULL;
        int64_t t = 0;
        const struct {
            const char *k;
            atlas_buf *dst;
        } strs[] = {
            {"decision", &sum.uid},
            {"status", &sum.status},
            {"revision_state", &sum.revision_state},
            {"title", &sum.title},
            {"content_hash", &sum.content_hash},
            {"proposed_by", &sum.proposed_by},
            {"superseded_by", &sum.superseded_by},
            {"created_at", &sum.created_at},
            {"updated_at", &sum.updated_at},
        };
        for (size_t k = 0; st == ATLAS_OK && k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "decisions", i, strs[k].k, &v)) {
                st = atlas_buf_set_str(strs[k].dst, v, err);
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "revision", &t)) {
            sum.revision_no = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "latest_revision", &t)) {
            sum.latest_revision_no = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "links", &t)) {
            sum.link_count = t;
        }
        if (st == ATLAS_OK) {
            st = cb(&sum, ud, err);
        }
        atlas_decision_summary_free(&sum);
        if (st == ATLAS_OK) {
            (*count_out)++;
        }
    }
    bool more = false;
    if (atlas_ipc_result_bool(r, "more", &more)) {
        *more_out = more;
    }
    if (counts_out != NULL) {
        int64_t t = 0;
        if (atlas_ipc_result_int(r, "total_proposed", &t)) {
            counts_out->proposed = t;
        }
        if (atlas_ipc_result_int(r, "total_approved", &t)) {
            counts_out->approved = t;
        }
        if (atlas_ipc_result_int(r, "total_rejected", &t)) {
            counts_out->rejected = t;
        }
        if (atlas_ipc_result_int(r, "total_superseded", &t)) {
            counts_out->superseded = t;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- gate check ------------------------------------------------------------ */

atlas_status atlas_service_gate_check_remote(const atlas_gate_query *q, atlas_gate_report *out,
                                             atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK) {
        if (q->repo_name != NULL) {
            st = atlas_json_key_str(j, "repo", q->repo_name, err);
        }
        if (st == ATLAS_OK && q->at_commit != NULL && q->at_commit[0] != '\0') {
            st = atlas_json_key_str(j, "at", q->at_commit, err);
        }
        if (st == ATLAS_OK && q->depth > 0) {
            st = atlas_json_key_int(j, "depth", q->depth, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("gate.check", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        st = atlas_buf_set_str(&out->repo_name, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "root", &v)) {
        st = atlas_buf_set_str(&out->root_text, v, err);
    }
    if (atlas_ipc_result_str(r, "result", &v)) {
        (void)atlas_gate_result_parse(v, &out->result);
    }
    copy_str(out->indexed_commit, sizeof out->indexed_commit,
             atlas_ipc_result_str(r, "indexed_commit", &v) ? v : NULL);
    copy_str(out->requested_commit, sizeof out->requested_commit,
             atlas_ipc_result_str(r, "requested_commit", &v) ? v : NULL);
    const struct {
        const char *k;
        int64_t *dst;
    } tops[] = {
        {"fresh", &out->fresh},       {"stale", &out->stale},
        {"impacted", &out->impacted}, {"unknown", &out->unknown},
        {"out_of_scope", &out->out_of_scope}, {"depth", &out->depth},
    };
    for (size_t i = 0; i < sizeof tops / sizeof tops[0]; i++) {
        if (atlas_ipc_result_int(r, tops[i].k, &t)) {
            *tops[i].dst = t;
        }
    }
    if (atlas_ipc_result_bool(r, "limit_reached", &b)) {
        out->limit_reached = b;
    }
    /* `limit_detail` is `const char *` pointing at an Atlas literal, so it is
     * resolved back to one from the closed vocabulary rather than aliased into
     * a response that is about to be freed. An unrecognised value is dropped
     * rather than reproduced. */
    if (atlas_ipc_result_str(r, "limit_detail", &v)) {
        out->limit_detail = atlas_gate_limit_detail_intern(v);
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "decisions", &n);
    if (st == ATLAS_OK && n > 0) {
        out->items = calloc(n, sizeof(*out->items));
        if (out->items == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading a gate report");
        }
    }
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_gate_assessment *a = &out->items[i];
        atlas_gate_assessment_init(a);
        out->item_count = i + 1u;
        if (atlas_ipc_result_arr_obj_str(r, "decisions", i, "decision", &v)) {
            st = atlas_buf_set_str(&a->uid, v, err);
        }
        if (st == ATLAS_OK && atlas_ipc_result_arr_obj_str(r, "decisions", i, "title", &v)) {
            st = atlas_buf_set_str(&a->title, v, err);
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "revision", &t)) {
            a->revision_no = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "revalidations", &t)) {
            a->revalidation_count = t;
        }
        copy_str(a->content_hash, sizeof a->content_hash,
                 atlas_ipc_result_arr_obj_str(r, "decisions", i, "content_hash", &v) ? v : NULL);
        copy_str(a->evidence_digest, sizeof a->evidence_digest,
                 atlas_ipc_result_arr_obj_str(r, "decisions", i, "evidence_digest", &v) ? v : NULL);
        copy_str(a->validated_at_commit, sizeof a->validated_at_commit,
                 atlas_ipc_result_arr_obj_str(r, "decisions", i, "validated_at_commit", &v) ? v
                                                                                           : NULL);
        copy_str(a->indexed_commit, sizeof a->indexed_commit, out->indexed_commit);
        copy_str(a->requested_commit, sizeof a->requested_commit, out->requested_commit);
        if (atlas_ipc_result_arr_obj_bool(r, "decisions", i, "validated_by_revalidation", &b)) {
            a->validated_by_revalidation = b;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "decisions", i, "limit_reached", &b)) {
            a->limit_reached = b;
        }
        if (atlas_ipc_result_arr_obj_str(r, "decisions", i, "limit_detail", &v)) {
            a->limit_detail = atlas_gate_limit_detail_intern(v);
        }
        const struct {
            const char *k;
            int64_t *dst;
        } cs[] = {
            {"links_total", &a->links_total},         {"links_current", &a->links_current},
            {"links_changed", &a->links_changed},     {"links_missing", &a->links_missing},
            {"links_ambiguous", &a->links_ambiguous}, {"links_unknown", &a->links_unknown},
            {"range_commits", &a->range_commits},     {"range_paths", &a->range_paths},
            {"walk_visited", &a->walk_visited},       {"walk_matched", &a->walk_matched},
        };
        for (size_t k = 0; k < sizeof cs / sizeof cs[0]; k++) {
            if (atlas_ipc_result_arr_obj_int(r, "decisions", i, cs[k].k, &t)) {
                *cs[k].dst = t;
            }
        }
        /* Reasons are replayed through `atlas_gate_assessment_note`, which is
         * the only way a freshness is ever set. Reading `freshness` off the
         * wire and assigning it would make the verdict something the daemon
         * asserted rather than something the reasons imply — and the two could
         * then disagree. A token outside the vocabulary is refused, not
         * reproduced; the response is Atlas' own output, so one means the two
         * ends disagree about what a reason is. */
        size_t rn = 0;
        (void)atlas_ipc_result_arr_obj_arr_len(r, "decisions", i, "reasons", &rn);
        for (size_t k = 0; st == ATLAS_OK && k < rn; k++) {
            if (!atlas_ipc_result_arr_obj_arr_str(r, "decisions", i, "reasons", k, &v)) {
                continue;
            }
            atlas_gate_reason reason;
            if (!atlas_gate_reason_parse(v, &reason)) {
                st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                   "the daemon reported a gate reason this binary does not know");
                break;
            }
            atlas_gate_assessment_note(a, reason);
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- code status ----------------------------------------------------------- */

atlas_status atlas_service_code_status_remote(const char *name, atlas_code_status_report *out,
                                              atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\"}", name);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.status", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        copy_str(out->repo.name, sizeof out->repo.name, v);
    }
    if (atlas_ipc_result_bool(r, "index_current", &b)) {
        out->file_index_current = b;
    }
    if (atlas_ipc_result_bool(r, "code_index_current", &b)) {
        out->code_index_current = b;
    }
    if (atlas_ipc_result_str(r, "code_not_current_reason", &v)) {
        out->not_current_reason = atlas_code_not_current_reason_intern(v);
    }
    if (atlas_ipc_result_int(r, "code_generation", &t)) {
        out->code_state.last_complete_generation = t;
    }
    if (atlas_ipc_result_int(r, "generation", &t)) {
        out->file_state.last_complete_generation = t;
    }
    const struct {
        const char *k;
        int64_t *dst;
    } cs[] = {
        {"files_indexed", &out->code_state.files_indexed},
        {"files_parsed_last", &out->code_state.files_parsed_last},
        {"symbols", &out->code_state.symbols},
        {"relations", &out->code_state.relations},
        {"ambiguous", &out->code_state.ambiguous},
        {"unresolved", &out->code_state.unresolved},
        {"compile_units", &out->code_state.compile_units},
        {"compile_entries_dropped", &out->code_state.compile_entries_dropped},
        {"analyzer_version", &out->code_state.analyzer_version},
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        if (atlas_ipc_result_int(r, cs[i].k, &t)) {
            *cs[i].dst = t;
        }
    }
    if (atlas_ipc_result_bool(r, "degraded", &b)) {
        out->code_state.degraded = b;
    }
    if (atlas_ipc_result_bool(r, "compile_db_present", &b)) {
        out->code_state.compile_db_present = b;
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "degraded_reason", &v)) {
        st = atlas_buf_set_str(&out->code_state.degraded_reason, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "analyzer", &v)) {
        st = atlas_buf_set_str(&out->code_state.analyzer_name, v, err);
    }
    if (atlas_ipc_result_str(r, "last_complete_at", &v)) {
        copy_str(out->code_state.last_complete_at, sizeof out->code_state.last_complete_at, v);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- sync ------------------------------------------------------------------ */

atlas_status atlas_service_sync_remote(const char *name, bool full, bool wait, int timeout_ms,
                                       atlas_sync_report *out, atlas_err *err) {
    (void)wait;
    (void)timeout_ms;
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\",\"full\":%s}", name,
                               full ? "true" : "false");
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("repo.sync", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    /* Always via the daemon: there is no offline branch here, because an index
     * this process cannot open is not one it could reconcile. */
    out->via_daemon = true;
    int64_t seq = 0;
    if (atlas_ipc_result_int(r, "sync_seq", &seq)) {
        out->sync_seq = seq;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK || !wait) {
        return st;
    }
    /* Waiting polls the published state rather than holding the connection
     * open, exactly as the local path does — the sequence number exists so a
     * client can wait without a daemon's serve loop stalling behind it. */
    return atlas_service_sync_wait_remote(name, out, timeout_ms, err);
}

atlas_status atlas_service_sync_wait_remote(const char *name, atlas_sync_report *out,
                                            int timeout_ms, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\"}", name);
    int64_t deadline = monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 120000);
    out->waited = true;
    while (st == ATLAS_OK && monotonic_ms() < deadline) {
        struct timespec nap = {0, 100L * 1000000L};
        (void)nanosleep(&nap, NULL);
        atlas_buf raw = ATLAS_BUF_INIT;
        atlas_ipc_response *r = NULL;
        atlas_err perr;
        atlas_err_init(&perr);
        if (atlas_remote_call("repo.state", atlas_buf_cstr(&params), &raw, &r, &perr) != ATLAS_OK) {
            atlas_ipc_response_free(r);
            atlas_buf_free(&raw);
            continue;
        }
        int64_t seq = 0, gen = 0;
        bool done = atlas_ipc_result_int(r, "last_sync_seq", &seq) && seq >= out->sync_seq;
        (void)atlas_ipc_result_int(r, "last_complete_generation", &gen);
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        if (done) {
            out->completed = true;
            out->generation = gen;
            break;
        }
    }
    atlas_buf_free(&params);
    return st;
}

/* --- daemon status --------------------------------------------------------- */

atlas_status atlas_service_daemon_status_remote(atlas_daemon_status_report *out, atlas_err *err) {
    out->protocol_version = (int)ATLAS_IPC_PROTOCOL_VERSION;
    atlas_status st = atlas_buf_set_str(&out->atlas_version, ATLAS_VERSION_STRING, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_socket_path(&out->socket_path, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    st = atlas_remote_call("daemon.status", "{}", &raw, &r, err);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    /* **Reachability is the only liveness this process can measure.**
     *
     * The local path reports two independent answers — the writer lock says
     * whether something owns the index, the socket says whether it is answering
     * — and reporting both is what makes a wedged daemon visible. The lock lives
     * in a directory this process cannot open, so there is no second answer to
     * give. A daemon that answered this call holds the writer, so `running` is
     * sound; what is *not* sound is claiming `running = false` about a lock
     * nothing observed, which is why an unreachable daemon fails above rather
     * than producing a report here. */
    out->reachable = true;
    out->running = true;
    const char *v = NULL;
    int64_t t = 0;
    if (atlas_ipc_result_int(r, "pid", &t)) {
        out->record.pid = t;
    }
    if (atlas_ipc_result_str(r, "atlas", &v)) {
        st = atlas_buf_set_str(&out->atlas_version, v, err);
    }
    if (atlas_ipc_result_int(r, "protocol", &t)) {
        out->protocol_version = (int)t;
    }
    const struct {
        const char *k;
        int64_t *dst;
    } cs[] = {
        {"repositories", &out->repo_count},
        {"watching", &out->watched_repos},
        {"degraded", &out->degraded_repos},
        {"repositories_with_event_gap", &out->repos_with_gap},
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        if (atlas_ipc_result_int(r, cs[i].k, &t)) {
            *cs[i].dst = t;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- file, history and diff ------------------------------------------------- */

atlas_status atlas_service_file_remote(const char *name, const char *path,
                                       atlas_file_report_cb cb, void *ud, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = repo_path_params(name, path, 0, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("repo.file", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    atlas_file_report rep;
    memset(&rep, 0, sizeof rep);
    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    const struct {
        const char *k;
        const char **dst;
    } strs[] = {
        {"path", &rep.row.path_text},
        {"file_type", &rep.row.file_type},
        {"language", &rep.row.language},
        {"git_mode", &rep.row.git_mode},
        {"git_index_oid", &rep.row.git_index_oid},
        {"content_hash", &rep.row.content_hash},
        {"content_hash_algo", &rep.row.content_hash_algo},
        {"read_error", &rep.row.read_error},
        {"truncated_reason", &rep.row.truncated_reason},
        {"reason", &rep.reason},
        {"reason_evidence", &rep.reason_evidence},
        {"last_commit", &rep.last_commit_oid},
        {"last_commit_subject", &rep.last_commit_subject},
    };
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        if (atlas_ipc_result_str(r, strs[i].k, &v)) {
            *strs[i].dst = v;
        }
    }
    const struct {
        const char *k;
        bool *dst;
    } bools[] = {
        {"path_is_utf8", &rep.row.path_is_utf8},   {"size_known", &rep.row.size_known},
        {"is_executable", &rep.row.is_executable}, {"is_symlink", &rep.row.is_symlink},
        {"unsafe_path", &rep.row.unsafe_path},     {"deleted", &rep.row.deleted},
        {"tracked", &rep.row.tracked},             {"ignored", &rep.row.ignored},
        {"truncated", &rep.row.truncated},
    };
    for (size_t i = 0; i < sizeof bools / sizeof bools[0]; i++) {
        if (atlas_ipc_result_bool(r, bools[i].k, &b)) {
            *bools[i].dst = b;
        }
    }
    const struct {
        const char *k;
        int64_t *dst;
    } ints[] = {
        {"size_bytes", &rep.row.size_bytes},
        {"last_generation", &rep.row.last_generation},
        {"first_seen_scan_id", &rep.row.first_seen_scan_id},
        {"last_seen_scan_id", &rep.row.last_seen_scan_id},
        {"change_count", &rep.change_count},
        {"last_commit_time", &rep.last_commit_time},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        if (atlas_ipc_result_int(r, ints[i].k, &t)) {
            *ints[i].dst = t;
        }
    }
    if (cb != NULL) {
        st = cb(&rep, ud, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_history_remote(const char *name, const char *path, int64_t limit,
                                          atlas_history_cb cb, void *ud, int64_t *count_out,
                                          atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = repo_path_params(name, path, limit, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("repo.history", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "changes", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_history_row row;
        memset(&row, 0, sizeof row);
        const char *v = NULL;
        int64_t t = 0;
        bool b = false;
        const struct {
            const char *k;
            const char **dst;
        } strs[] = {
            {"commit", &row.commit_oid},   {"author", &row.author_name},
            {"author_email", &row.author_email}, {"subject", &row.subject},
            {"change_type", &row.change_type},   {"path", &row.path_text},
            {"old_path", &row.old_path_text},
        };
        for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "changes", i, strs[k].k, &v)) {
                *strs[k].dst = v;
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "changes", i, "author_time", &t)) {
            row.author_time = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "changes", i, "commit_time", &t)) {
            row.commit_time = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "changes", i, "score", &t)) {
            row.score = (int)t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "changes", i, "score_known", &b)) {
            row.score_known = b;
        }
        st = cb(&row, ud, err);
        if (st == ATLAS_OK && count_out != NULL) {
            (*count_out)++;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* `diff` reads no index at all: it resolves the repository and then observes
 * git. So the remote form is the repository row over the socket plus the very
 * same observation, run here — the split `atlas_service_status_observe_live`
 * already makes, and the reason no `repo.diff` method exists. */
atlas_status atlas_service_diff_remote(const char *name, const atlas_diff_opts *opts,
                                       atlas_diff_entry_cb cb, void *ud, atlas_diff_report *rep,
                                       atlas_err *err) {
    atlas_repo_state_report state;
    atlas_repo_state_report_init(&state);
    atlas_status st = atlas_service_repo_state_remote(name, &state, err);
    if (st == ATLAS_OK) {
        st = atlas_service_diff_repo(&state.repo, opts, cb, ud, rep, err);
    }
    atlas_repo_state_report_free(&state);
    return st;
}

/* --- the structural group --------------------------------------------------- */

atlas_status atlas_service_code_file_remote(const char *name, const char *path,
                                            atlas_code_file_report *out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = repo_path_params(name, path, 0, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.file", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_bool(r, "indexed", &b)) {
        out->indexed = b;
    }
    if (atlas_ipc_result_bool(r, "truncated", &b)) {
        out->truncated = b;
    }
    if (atlas_ipc_result_bool(r, "include_guard", &b)) {
        out->include_guard = b;
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "path", &v)) {
        st = atlas_buf_set_str(&out->path_text, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "parse_detail", &v)) {
        st = atlas_buf_set_str(&out->parse_detail, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "truncated_reason", &v)) {
        st = atlas_buf_set_str(&out->truncated_reason, v, err);
    }
    copy_str(out->language, sizeof out->language,
             atlas_ipc_result_str(r, "language", &v) ? v : NULL);
    copy_str(out->content_hash, sizeof out->content_hash,
             atlas_ipc_result_str(r, "content_hash", &v) ? v : NULL);
    copy_str(out->parse_status, sizeof out->parse_status,
             atlas_ipc_result_str(r, "parse_status", &v) ? v : NULL);
    const struct {
        const char *k;
        int64_t *dst;
    } ints[] = {
        {"symbols", &out->symbol_count},    {"includes", &out->include_count},
        {"call_candidates", &out->occurrence_count}, {"bytes", &out->bytes},
        {"lines", &out->lines},             {"ambiguous", &out->ambiguous},
        {"unresolved", &out->unresolved},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        if (atlas_ipc_result_int(r, ints[i].k, &t)) {
            *ints[i].dst = t;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* Symbol rows, from whichever array of whichever method holds them. One reader
 * because the daemon emits one shape for a symbol wherever it appears. */
static atlas_status read_symbols(const atlas_ipc_response *r, const char *arr,
                                 atlas_code_symbol_cb cb, void *ud, int64_t *count_out,
                                 atlas_err *err) {
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, arr, &n);
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_code_symbol_row row;
        memset(&row, 0, sizeof row);
        const char *v = NULL;
        int64_t t = 0;
        bool b = false;
        const struct {
            const char *k;
            const char **dst;
        } strs[] = {
            {"name", &row.name_text}, {"kind", &row.kind},
            {"linkage", &row.linkage}, {"resolution", &row.resolution},
            {"path", &row.path_text},
        };
        for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, arr, i, strs[k].k, &v)) {
                *strs[k].dst = v;
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, arr, i, "id", &t)) {
            row.id = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, arr, i, "line", &t)) {
            row.line = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, arr, i, "col", &t)) {
            row.col = t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, arr, i, "definition", &b)) {
            row.is_definition = b;
        }
        if (atlas_ipc_result_arr_obj_bool(r, arr, i, "declaration", &b)) {
            row.is_declaration = b;
        }
        st = cb(&row, ud, err);
        if (st == ATLAS_OK && count_out != NULL) {
            (*count_out)++;
        }
    }
    return st;
}

static atlas_status read_edges(const atlas_ipc_response *r, const char *arr,
                               atlas_code_edge_cb cb, void *ud, int64_t *count_out,
                               atlas_err *err) {
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, arr, &n);
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_code_edge_row row;
        memset(&row, 0, sizeof row);
        const char *v = NULL;
        int64_t t = 0;
        const struct {
            const char *k;
            const char **dst;
        } strs[] = {
            {"kind", &row.kind},          {"from_kind", &row.src_kind},
            {"from_path", &row.src_path_text}, {"to_kind", &row.dst_kind},
            {"to_path", &row.dst_path_text},   {"spelling", &row.dst_name_text},
            {"resolution", &row.resolution},   {"provenance", &row.provenance},
            {"reason", &row.detail},
        };
        for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, arr, i, strs[k].k, &v)) {
                *strs[k].dst = v;
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, arr, i, "candidates", &t)) {
            row.candidate_count = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, arr, i, "line", &t)) {
            row.line = t;
        }
        st = cb(&row, ud, err);
        if (st == ATLAS_OK && count_out != NULL) {
            (*count_out)++;
        }
    }
    return st;
}

atlas_status atlas_service_code_file_symbols_remote(const char *name, const char *path,
                                                    int64_t limit, atlas_code_symbol_cb cb,
                                                    void *ud, int64_t *count_out, bool *more_out,
                                                    atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = repo_path_params(name, path, limit, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.file", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st == ATLAS_OK) {
        st = read_symbols(r, "symbols_defined", cb, ud, count_out, err);
        bool b = false;
        if (more_out != NULL && atlas_ipc_result_bool(r, "symbols_more", &b)) {
            *more_out = b;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_code_file_edges_remote(const char *name, const char *path,
                                                  const char *kind, bool inbound, int64_t limit,
                                                  atlas_code_edge_cb cb, void *ud,
                                                  int64_t *count_out, bool *more_out,
                                                  atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = repo_path_params(name, path, limit, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.file", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st == ATLAS_OK) {
        /* The daemon groups a file's edges by what they are, so the array is
         * chosen by the same (kind, direction) pair the local read selects an
         * index with. */
        const char *arr = "depends_on";
        if (kind != NULL && strcmp(kind, "file_includes_file") == 0) {
            arr = "includes";
        } else if (inbound) {
            arr = "depended_on_by";
        }
        st = read_edges(r, arr, cb, ud, count_out, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_code_symbol_search_remote(const char *name, const char *query,
                                                     const char *kind, int64_t limit,
                                                     atlas_code_symbol_cb cb, void *ud,
                                                     int64_t *count_out, bool *more_out,
                                                     atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "query", query, err);
        }
        if (st == ATLAS_OK && kind != NULL && kind[0] != '\0') {
            st = atlas_json_key_str(j, "kind", kind, err);
        }
        if (st == ATLAS_OK && limit > 0) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.symbol.search", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st == ATLAS_OK) {
        st = read_symbols(r, "symbols", cb, ud, count_out, err);
        bool b = false;
        if (more_out != NULL && atlas_ipc_result_bool(r, "more", &b)) {
            *more_out = b;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

static atlas_status symbol_params(const char *name, const char *symbol, int64_t limit,
                                  atlas_buf *out, atlas_err *err) {
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key_str(j, "repo", name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "symbol", symbol, err);
    }
    if (st == ATLAS_OK && limit > 0) {
        st = atlas_json_key_int(j, "limit", limit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, out, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    return st;
}

atlas_status atlas_service_code_symbol_sites_remote(const char *name, const char *symbol,
                                                    int64_t limit, atlas_code_symbol_cb cb,
                                                    void *ud, int64_t *count_out, bool *more_out,
                                                    atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = symbol_params(name, symbol, limit, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.symbol", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st == ATLAS_OK) {
        st = read_symbols(r, "sites", cb, ud, count_out, err);
        bool b = false;
        if (more_out != NULL && atlas_ipc_result_bool(r, "sites_more", &b)) {
            *more_out = b;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_code_symbol_edges_remote(const char *name, const char *symbol,
                                                    bool inbound, int64_t limit,
                                                    atlas_code_edge_cb cb, void *ud,
                                                    int64_t *count_out, bool *more_out,
                                                    atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = symbol_params(name, symbol, limit, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.symbol", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st == ATLAS_OK) {
        st = read_edges(r, inbound ? "callers" : "calls", cb, ud, count_out, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_code_walk_remote(const char *name, const char *path, const char *symbol,
                                            bool inbound, int64_t depth, int64_t limit,
                                            atlas_code_walk_cb cb, void *ud,
                                            atlas_code_walk_summary *sum, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK && path != NULL && path[0] != '\0') {
            st = atlas_json_key_str(j, "path", path, err);
        }
        if (st == ATLAS_OK && symbol != NULL && symbol[0] != '\0') {
            st = atlas_json_key_str(j, "symbol", symbol, err);
        }
        if (st == ATLAS_OK && depth > 0) {
            st = atlas_json_key_int(j, "depth", depth, err);
        }
        if (st == ATLAS_OK && limit > 0) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call(inbound ? "code.impact" : "code.deps", atlas_buf_cstr(&params), &raw,
                               &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "candidates", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_code_walk_row row;
        memset(&row, 0, sizeof row);
        const char *v = NULL;
        int64_t t = 0;
        const struct {
            const char *k;
            const char **dst;
        } strs[] = {
            {"node_kind", &row.node_kind}, {"node", &row.label},
            {"edge", &row.edge_kind},      {"via", &row.via_label},
            {"resolution", &row.resolution}, {"reason", &row.detail},
        };
        for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "candidates", i, strs[k].k, &v)) {
                *strs[k].dst = v;
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "candidates", i, "depth", &t)) {
            row.depth = t;
        }
        st = cb(&row, ud, err);
    }
    if (sum != NULL) {
        int64_t t = 0;
        bool b = false;
        const struct {
            const char *k;
            int64_t *dst;
        } cs[] = {
            {"count", &sum->emitted},         {"exact", &sum->exact},
            {"unique_lexical", &sum->unique_lexical}, {"ambiguous", &sum->ambiguous},
            {"unresolved", &sum->unresolved},
        };
        for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            if (atlas_ipc_result_int(r, cs[i].k, &t)) {
                *cs[i].dst = t;
            }
        }
        if (atlas_ipc_result_bool(r, "truncated", &b)) {
            sum->truncated = b;
        }
        /* `truncated_reason` is a `const char *` the renderer prints after this
         * call returns, and the response it arrived in is freed below. Copied
         * into storage that outlives both rather than aliased — and into a
         * file-static rather than an allocation, because the summary struct has
         * no owner for one and the CLI consumes it immediately. */
        static char trunc[128];
        const char *tv = NULL;
        if (atlas_ipc_result_str(r, "truncated_reason", &tv) && tv != NULL) {
            (void)snprintf(trunc, sizeof trunc, "%s", tv);
            sum->truncated_reason = trunc;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- decisions -------------------------------------------------------------- */

atlas_status atlas_service_decision_show_remote(const char *repo, const char *uid,
                                                int64_t revision_no, atlas_decision_document *out,
                                                atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(repo, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", repo, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "decision", uid, err);
        }
        if (st == ATLAS_OK && revision_no > 0) {
            st = atlas_json_key_int(j, "revision", revision_no, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("decision.get", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    /* `decision.get` answers with a nested shape: `result.document` carries the
     * document's identity and lifecycle, `result.revision` the revision's
     * content and links, and only `repo` sits at the top level. This parser
     * used to read every one of those names flat, match nothing and return a
     * document whose every field was empty — with `ok`, so `atlas decision
     * show` printed a blank record for a decision that was there and `atlas
     * decision export` wrote one. The names below are the daemon's; the nesting
     * is the daemon's; nothing here guesses. */
    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        st = atlas_buf_set_str(&out->repo, v, err);
    }
    const struct {
        const char *obj;
        const char *k;
        atlas_buf *dst;
    } strs[] = {
        {"document", "decision", &out->summary.uid},
        {"document", "status", &out->summary.status},
        {"document", "revision_state", &out->summary.revision_state},
        {"document", "created_at", &out->summary.created_at},
        {"revision", "title", &out->summary.title},
        {"revision", "content_hash", &out->summary.content_hash},
        {"revision", "proposed_by", &out->summary.proposed_by},
        {"revision", "context", &out->context_text},
        {"revision", "decision_body", &out->decision_text},
        {"revision", "rationale", &out->rationale_text},
        {"revision", "consequences", &out->consequences_text},
        {"revision", "scope", &out->scope},
        {"revision", "basis_head", &out->basis_head},
        {"revision", "basis_repo_identity", &out->basis_repo_identity},
        {"revision", "unbound_reason", &out->unbound_reason},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        if (atlas_ipc_result_obj_str(r, strs[i].obj, strs[i].k, &v)) {
            st = atlas_buf_set_str(strs[i].dst, v, err);
        }
    }
    /* The prose lives under `decision` inside the revision, where the same name
     * at document level is the uid. Read after the table so the uid, taken from
     * the document, is never overwritten by it. */
    if (st == ATLAS_OK && out->decision_text.len == 0 &&
        atlas_ipc_result_obj_str(r, "revision", "decision", &v) &&
        strncmp(v, ATLAS_DECISION_UID_PREFIX, strlen(ATLAS_DECISION_UID_PREFIX)) != 0) {
        st = atlas_buf_set_str(&out->decision_text, v, err);
    }
    if (atlas_ipc_result_obj_int(r, "revision", "number", &t)) {
        out->summary.revision_no = t;
    }
    if (atlas_ipc_result_obj_int(r, "document", "latest_revision", &t)) {
        out->summary.latest_revision_no = t;
    }
    if (atlas_ipc_result_obj_bool(r, "revision", "session_unbound", &b)) {
        out->session_unbound = b;
    }
    size_t n = 0;
    (void)atlas_ipc_result_obj_arr_len(r, "revision", "alternatives", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n && i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        const char *a = NULL;
        if (atlas_ipc_result_obj_arr_str(r, "revision", "alternatives", i, &a)) {
            st = atlas_buf_set_str(&out->alternatives[out->alternative_count], a, err);
            if (st == ATLAS_OK) {
                out->alternative_count++;
            }
        }
    }
    (void)atlas_ipc_result_obj_arr_len(r, "revision", "links", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n && i < ATLAS_DECISION_MAX_LINKS; i++) {
        atlas_decision_link_view *lv = &out->links[out->link_count];
        atlas_decision_link_view_init(lv);
        const char *kind = NULL;
        if (!atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i, "kind", &kind)) {
            continue;
        }
        st = atlas_buf_set_str(&lv->kind, kind, err);
        /* The value key depends on the kind, which is how the daemon writes it:
         * a link carries exactly one of these and the renderer wants the one it
         * has, under a single name. */
        static const char *const VALUE_KEYS[] = {"path", "commit", "symbol", "target"};
        for (size_t k = 0; st == ATLAS_OK && k < sizeof VALUE_KEYS / sizeof VALUE_KEYS[0]; k++) {
            if (atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i, VALUE_KEYS[k], &v)) {
                st = atlas_buf_set_str(&lv->value, v, err);
                break;
            }
        }
        if (st == ATLAS_OK &&
            atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i, "symbol_kind", &v)) {
            st = atlas_buf_set_str(&lv->detail, v, err);
        }
        if (st == ATLAS_OK &&
            atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i, "currency", &v)) {
            st = atlas_buf_set_str(&lv->currency, v, err);
        }
        if (st == ATLAS_OK &&
            atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i, "analyzer", &v)) {
            st = atlas_buf_set_str(&lv->analyzer, v, err);
        }
        if (atlas_ipc_result_obj_arr_obj_int(r, "revision", "links", i, "analyzer_version", &t)) {
            lv->analyzer_version = t;
        }
        if (atlas_ipc_result_obj_arr_obj_int(r, "revision", "links", i, "matches", &t)) {
            lv->matches = t;
        }
        /* Migration 10. Already safe-encoded by the daemon, so it is taken as
         * it arrives — encoding it again here is the A8.2 defect. */
        if (st == ATLAS_OK &&
            atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i, "rationale", &v)) {
            st = atlas_buf_set_str(&lv->rationale, v, err);
        }
        if (st == ATLAS_OK && atlas_ipc_result_obj_arr_obj_str(r, "revision", "links", i,
                                                               "rationale_provenance", &v)) {
            st = atlas_buf_set_str(&lv->rationale_provenance, v, err);
        }
        if (strcmp(atlas_buf_cstr(&lv->currency), "CURRENT") != 0 &&
            atlas_buf_cstr(&lv->currency)[0] != '\0' &&
            strcmp(atlas_buf_cstr(&lv->currency), "UNKNOWN") != 0) {
            out->links_needing_review++;
        }
        out->link_count++;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_decision_history_remote(const char *repo, const char *uid,
                                                   atlas_decision_summary_cb rev_cb,
                                                   atlas_decision_timeline_cb event_cb, void *ud,
                                                   bool *ledger_agrees_out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(repo, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", repo, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "decision", uid, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("decision.history", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "revisions", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_decision_summary sum;
        atlas_decision_summary_init(&sum);
        const char *v = NULL;
        int64_t t = 0;
        const struct {
            const char *k;
            atlas_buf *dst;
        } strs[] = {
            {"state", &sum.revision_state}, {"content_hash", &sum.content_hash},
            {"proposed_by", &sum.proposed_by}, {"created_at", &sum.created_at},
            {"title", &sum.title},
        };
        for (size_t k = 0; st == ATLAS_OK && k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "revisions", i, strs[k].k, &v)) {
                st = atlas_buf_set_str(strs[k].dst, v, err);
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "revisions", i, "revision", &t)) {
            sum.revision_no = t;
        }
        if (st == ATLAS_OK && rev_cb != NULL) {
            st = rev_cb(&sum, ud, err);
        }
        atlas_decision_summary_free(&sum);
    }
    (void)atlas_ipc_result_arr_len(r, "timeline", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_decision_timeline_entry e;
        memset(&e, 0, sizeof e);
        const char *v = NULL;
        int64_t t = 0;
        bool b = false;
        const struct {
            const char *k;
            const char **dst;
        } strs[] = {
            {"event", &e.event},           {"actor", &e.actor},
            {"content_hash", &e.content_hash}, {"superseded_by", &e.superseded_by},
            {"detail", &e.detail},         {"at", &e.at},
        };
        for (size_t k = 0; k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "timeline", i, strs[k].k, &v)) {
                *strs[k].dst = v;
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "timeline", i, "revision", &t)) {
            e.revision_no = t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "timeline", i, "operator_channel", &b)) {
            e.operator_channel = b;
        }
        if (event_cb != NULL) {
            st = event_cb(&e, ud, err);
        }
    }
    bool agrees = true;
    if (ledger_agrees_out != NULL && atlas_ipc_result_bool(r, "ledger_agrees", &agrees)) {
        *ledger_agrees_out = agrees;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_decision_orphans_remote(int64_t limit, atlas_decision_summary_cb cb,
                                                   void *ud, int64_t *count_out, bool *more_out,
                                                   atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (limit > 0) {
        st = atlas_buf_appendf(&params, err, "{\"limit\":%lld}", (long long)limit);
    } else {
        st = atlas_buf_set_str(&params, "{}", err);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("decision.orphaned", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "decisions", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_decision_summary sum;
        atlas_decision_summary_init(&sum);
        const char *v = NULL;
        int64_t t = 0;
        const struct {
            const char *k;
            atlas_buf *dst;
        } strs[] = {
            {"decision", &sum.uid},          {"status", &sum.status},
            {"revision_state", &sum.revision_state}, {"title", &sum.title},
            {"content_hash", &sum.content_hash},  {"proposed_by", &sum.proposed_by},
            {"superseded_by", &sum.superseded_by}, {"created_at", &sum.created_at},
            {"updated_at", &sum.updated_at},
        };
        for (size_t k = 0; st == ATLAS_OK && k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "decisions", i, strs[k].k, &v)) {
                st = atlas_buf_set_str(strs[k].dst, v, err);
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "revision", &t)) {
            sum.revision_no = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "latest_revision", &t)) {
            sum.latest_revision_no = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "decisions", i, "links", &t)) {
            sum.link_count = t;
        }
        if (st == ATLAS_OK) {
            st = cb(&sum, ud, err);
        }
        atlas_decision_summary_free(&sum);
        if (st == ATLAS_OK && count_out != NULL) {
            (*count_out)++;
        }
    }
    bool more = false;
    if (more_out != NULL && atlas_ipc_result_bool(r, "more", &more)) {
        *more_out = more;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_decision_legacy_remote(const char *repo, int64_t limit,
                                                  atlas_decision_legacy_view_cb cb, void *ud,
                                                  int64_t *count_out, bool *more_out,
                                                  atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = repo_path_params(repo, NULL, limit, &params, err);
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("decision.legacy", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "legacy", &n);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_decision_legacy_view view;
        atlas_decision_legacy_view_init(&view);
        const char *v = NULL;
        int64_t t = 0;
        bool b = false;
        const struct {
            const char *k;
            atlas_buf *dst;
        } strs[] = {
            {"title", &view.title},           {"statement", &view.statement},
            {"provenance", &view.provenance}, {"created_at", &view.created_at},
            {"imported_uid", &view.imported_uid},
        };
        for (size_t k = 0; st == ATLAS_OK && k < sizeof strs / sizeof strs[0]; k++) {
            if (atlas_ipc_result_arr_obj_str(r, "legacy", i, strs[k].k, &v)) {
                st = atlas_buf_set_str(strs[k].dst, v, err);
            }
        }
        if (atlas_ipc_result_arr_obj_int(r, "legacy", i, "id", &t)) {
            view.id = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "legacy", i, "paths", &t)) {
            view.path_count = t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "legacy", i, "imported", &b)) {
            view.imported = b;
        }
        if (st == ATLAS_OK) {
            st = cb(&view, ud, err);
        }
        atlas_decision_legacy_view_free(&view);
        if (st == ATLAS_OK && count_out != NULL) {
            (*count_out)++;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* `gate show` is `gate.check` with a single-decision filter — the query already
 * carries `only_uid` and the daemon now reads it, so there is one assessment
 * path and one parser rather than a second method that would have to agree. */
atlas_status atlas_service_gate_show_remote(const char *repo, const char *uid,
                                            atlas_gate_report *out, atlas_err *err) {
    atlas_gate_query q;
    memset(&q, 0, sizeof q);
    q.repo_name = repo;
    q.only_uid = uid;
    atlas_status st = atlas_service_gate_check_remote(&q, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_gate_narrow_to_one(out, uid, err);
}

/* --- backup ---------------------------------------------------------------- */

/* Fills the verification report from a response object's members.
 *
 * Shared by both twins because `backup.create` embeds the same verification it
 * ran daemon-side, and a create whose embedded report was parsed differently
 * from a standalone verify would be two answers to one question. */
static void take_verify(const atlas_ipc_response *r, const char *prefix,
                        atlas_backup_verify_report *out) {
    const char *v = NULL;
    int64_t n = 0;
    bool b = false;
    char key[64];
#define K(field) (prefix[0] == '\0' ? (field) : (snprintf(key, sizeof key, "%s%s", prefix, field), key))
    if (atlas_ipc_result_str(r, K("verdict"), &v) && v != NULL) {
        /* Parsed against the closed vocabulary. An unknown verdict is a version
         * mismatch, and defaulting it to OK would report a backup this binary
         * cannot judge as one it judged good. */
        if (!atlas_backup_verdict_parse(v, &out->verdict)) {
            out->verdict = ATLAS_BACKUP_UNREADABLE;
            out->ok = false;
        }
    }
    if (atlas_ipc_result_bool(r, K("ok"), &b)) {
        out->ok = b;
    }
    if (atlas_ipc_result_int(r, K("size_bytes"), &n)) {
        out->size_bytes = n;
    }
    if (atlas_ipc_result_str(r, K("sha256"), &v) && v != NULL) {
        copy_str(out->sha256, sizeof out->sha256, v);
    }
    if (atlas_ipc_result_int(r, K("schema_version"), &n)) {
        out->schema_version = (int)n;
    }
    if (atlas_ipc_result_int(r, K("expected_schema_version"), &n)) {
        out->expected_schema_version = (int)n;
    }
    if (atlas_ipc_result_int(r, K("revisions_checked"), &n)) {
        out->revisions_checked = n;
    }
    if (atlas_ipc_result_int(r, K("revisions_rehashed"), &n)) {
        out->revisions_rehashed = n;
    }
    if (atlas_ipc_result_int(r, K("tables_required"), &n)) {
        out->tables_required = n;
    }
    if (atlas_ipc_result_int(r, K("tables_present"), &n)) {
        out->tables_present = n;
    }
    if (atlas_ipc_result_int(r, K("revisions_corrupt"), &n)) {
        out->revisions_corrupt = n;
    }
    if (atlas_ipc_result_int(r, K("ledger_mismatched"), &n)) {
        out->ledger_mismatched = n;
    }
    if (atlas_ipc_result_int(r, K("repo_count"), &n)) {
        out->repo_count = n;
    }
    atlas_err e;
    atlas_err_init(&e);
    if (atlas_ipc_result_str(r, K("integrity"), &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->integrity, v, &e);
    }
    if (atlas_ipc_result_str(r, K("foreign_key_check"), &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->foreign_key_check, v, &e);
    }
    if (atlas_ipc_result_str(r, K("missing_tables"), &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->missing_tables, v, &e);
    }
    if (atlas_ipc_result_str(r, K("problems"), &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->problems, v, &e);
    }
#undef K
}

/* Tells the user the operation exists, before waiting on it.
 *
 * On stderr, so stdout stays exactly one document — and *before* the wait, so a
 * user who interrupts has already seen the id. The operator guide says to ask
 * again with the id the daemon reported; without this line the daemon never
 * reported one, and the instruction was impossible to follow at the moment it
 * was needed. Interrupting is the case that matters, because interrupting is
 * the one thing the split makes safe. */
static void announce_operation(int64_t op_id, const char *what) {
    (void)fprintf(stderr,
                  "atlas: %s accepted as operation %lld; waiting. Interrupting does not cancel "
                  "it — ask again with `atlas operation status %lld`.\n",
                  what, (long long)op_id, (long long)op_id);
    (void)fflush(stderr);
}

/* Polls one accepted operation until it reaches a terminal state.
 *
 * The wait is a ceiling on this client's patience, never on the operation. If
 * it is reached, the operation is still running on the daemon and the message
 * says so and says how to ask again — because the whole defect being fixed here
 * was a client deciding that a server-side success was a failure. Reporting
 * "still running, ask again" is true; reporting a timeout as a failure is not. */
static atlas_status wait_for_operation(int64_t op_id, atlas_buf *message_out,
                                       atlas_buf *detail_out, atlas_backup_report *backup_out,
                                       int64_t *duration_out, atlas_err *err) {
    int waited_ms = 0;
    for (;;) {
        atlas_buf params = ATLAS_BUF_INIT;
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        atlas_status st = atlas_ipc_params_begin(&p, &j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(j, "operation_id", op_id, err);
            if (st == ATLAS_OK) {
                st = atlas_ipc_params_finish(p, &params, err);
            } else {
                atlas_ipc_params_abort(p);
            }
        }
        atlas_buf raw = ATLAS_BUF_INIT;
        atlas_ipc_response *r = NULL;
        if (st == ATLAS_OK) {
            st = atlas_remote_call("operation.get", atlas_buf_cstr(&params), &raw, &r, err);
        }
        atlas_buf_free(&params);
        if (st != ATLAS_OK) {
            /* A poll that failed says nothing about the operation.
             *
             * This is the same mistake the whole split exists to undo, one
             * layer down: a semantic index makes the daemon write hard for a
             * minute, ordinary reads slow under that load, and the poll's own
             * frame read hit ATLAS_IPC_READ_TIMEOUT_MS — so the client reported
             * failure while the index went on to publish successfully. The
             * operation is running on the daemon and this client's inability to
             * ask about it right now is not evidence about it.
             *
             * So a failed poll is retried until the overall ceiling, and only
             * the ceiling ends the wait — with a message that says the
             * operation is still running rather than that it failed. */
            atlas_buf_free(&raw);
            atlas_ipc_response_free(r);
            atlas_err_init(err);
            if (waited_ms >= ATLAS_OPS_CLIENT_WAIT_MS) {
                return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                     "operation %lld could not be polled and this client stopped "
                                     "waiting after %d seconds. It has not failed and it has not "
                                     "been cancelled; ask again with `atlas operation status %lld`",
                                     (long long)op_id, waited_ms / 1000, (long long)op_id);
            }
            struct timespec rts;
            rts.tv_sec = ATLAS_OPS_POLL_INTERVAL_MS / 1000;
            rts.tv_nsec = (long)(ATLAS_OPS_POLL_INTERVAL_MS % 1000) * 1000000L;
            (void)nanosleep(&rts, NULL);
            waited_ms += ATLAS_OPS_POLL_INTERVAL_MS;
            continue;
        }
        bool done = false;
        bool ok = false;
        const char *v = NULL;
        (void)atlas_ipc_result_bool(r, "done", &done);
        (void)atlas_ipc_result_bool(r, "succeeded", &ok);
        if (done) {
            int64_t dur = 0;
            if (duration_out != NULL && atlas_ipc_result_int(r, "duration_ms", &dur)) {
                *duration_out = dur;
            }
            if (atlas_ipc_result_str(r, "message", &v) && v != NULL && message_out != NULL) {
                (void)atlas_buf_set_str(message_out, v, err);
            }
            if (atlas_ipc_result_str(r, "detail", &v) && v != NULL && detail_out != NULL) {
                (void)atlas_buf_set_str(detail_out, v, err);
            }
            if (backup_out != NULL && ok) {
                int64_t n = 0;
                if (atlas_ipc_result_int(r, "size_bytes", &n)) {
                    backup_out->size_bytes = n;
                }
                if (atlas_ipc_result_int(r, "page_size", &n)) {
                    backup_out->page_size = n;
                }
                if (atlas_ipc_result_int(r, "page_count", &n)) {
                    backup_out->page_count = n;
                }
                if (atlas_ipc_result_int(r, "schema_version", &n)) {
                    backup_out->schema_version = (int)n;
                }
                (void)atlas_ipc_result_bool(r, "source_online", &backup_out->source_online);
                if (atlas_ipc_result_str(r, "sha256", &v) && v != NULL) {
                    copy_str(backup_out->sha256, sizeof backup_out->sha256, v);
                }
                if (atlas_ipc_result_str(r, "atlas_version", &v) && v != NULL) {
                    copy_str(backup_out->atlas_version, sizeof backup_out->atlas_version, v);
                }
            }
            atlas_status result = ATLAS_OK;
            if (!ok) {
                result = atlas_err_set(err, ATLAS_ERR_INTEGRITY, "%s",
                                       message_out != NULL && message_out->len > 0
                                           ? atlas_buf_cstr(message_out)
                                           : "the operation failed");
            }
            atlas_ipc_response_free(r);
            atlas_buf_free(&raw);
            return result;
        }
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);

        if (waited_ms >= ATLAS_OPS_CLIENT_WAIT_MS) {
            return atlas_err_set(
                err, ATLAS_ERR_CONFIG,
                "operation %lld is still running after %d seconds. It has not failed and it has "
                "not been cancelled; this client simply stopped waiting. Ask again with "
                "`atlas operation status %lld`",
                (long long)op_id, waited_ms / 1000, (long long)op_id);
        }
        struct timespec ts;
        ts.tv_sec = ATLAS_OPS_POLL_INTERVAL_MS / 1000;
        ts.tv_nsec = (long)(ATLAS_OPS_POLL_INTERVAL_MS % 1000) * 1000000L;
        (void)nanosleep(&ts, NULL);
        waited_ms += ATLAS_OPS_POLL_INTERVAL_MS;
    }
}

/* Submits a semantic index to the daemon, then polls until it finishes.
 *
 * Before the closeout there was no remote form: under A7.1 the index is 0700
 * `atlasd`, so an operator's only route was to stop the service and run the
 * command as the service account. That is a documented workaround standing in
 * for a missing feature, and the closeout forbids exactly that. The service
 * stays running now; the work is queued to the daemon's writer thread and the
 * caller polls. */
atlas_status atlas_service_sem_index_remote(const char *name, const char *const *compdbs,
                                            size_t compdb_count, bool rebuild,
                                            atlas_sem_index_summary *out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "compdbs", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        for (size_t i = 0; i < compdb_count && st == ATLAS_OK; i++) {
            st = atlas_json_str(j, compdbs[i], err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    if (st == ATLAS_OK && rebuild) {
        st = atlas_json_key_bool(j, "rebuild", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("code.index", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t op_id = 0;
    (void)atlas_ipc_result_int(r, "operation_id", &op_id);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    if (op_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the daemon accepted the index but named no operation to poll");
    }

    atlas_buf message = ATLAS_BUF_INIT;
    atlas_buf detail = ATLAS_BUF_INIT;
    announce_operation(op_id, "semantic index");
    int64_t op_duration_ms = 0;
    st = wait_for_operation(op_id, &message, &detail, NULL, &op_duration_ms, err);
    if (st == ATLAS_OK) {
        /* The summary is read back from the published generation rather than
         * carried through the operation record.
         *
         * The generation is the record; the operation's message is a log line
         * about producing it. Reading it back also means the local and remote
         * forms print numbers that came from the same place, so they cannot
         * describe the same index differently. */
        atlas_sem_status_report srep;
        atlas_sem_status_report_init(&srep);
        st = atlas_service_sem_status_remote(name, &srep, err);
        if (st == ATLAS_OK && srep.have_generation) {
            out->generation_id = srep.generation.id;
            out->units_total = srep.generation.tu_total;
            out->units_complete = srep.generation.tu_complete;
            out->units_partial = srep.generation.tu_partial;
            out->units_failed = srep.generation.tu_failed;
            out->units_unsupported = srep.generation.tu_unsupported;
            out->symbols = srep.generation.symbol_count;
            out->edges = srep.generation.edge_count;
            out->includes = srep.generation.include_count;
            /* This invocation's own duration, not the generation's.
             *
             * The generation records how long it took to build, which for a
             * no-change run is a number from some earlier pass — printing it
             * would tell an operator their half-second run took two minutes.
             * The operation measured what actually just happened. */
            out->duration_ms = op_duration_ms;
            out->published = true;
        }
        atlas_sem_status_report_free(&srep);
    }
    atlas_buf_free(&message);
    atlas_buf_free(&detail);
    return st;
}

/* Maintenance over the socket.
 *
 * A5 gave maintenance no RPC surface, on the reasoning that whoever owns the
 * data directory can prune it anyway — and A7.1 broke that premise exactly as
 * it did for backup: under a system deployment the index is 0700 `atlasd`, so
 * the operator account could neither plan nor prune without becoming the
 * service account. The guarantee A5 wanted is that nothing a *model* can reach
 * may prune the index, and that is untouched: these two methods live in the
 * operator-uid group, and every other peer is told they do not exist. */
atlas_status atlas_service_maintenance_remote(const atlas_maintenance_opts *opts,
                                              atlas_maintenance_report *out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK && opts->older_than_days > 0) {
        st = atlas_json_key_int(j, "older_than_days", opts->older_than_days, err);
    }
    if (st == ATLAS_OK && opts->retain_per_repo > 0) {
        st = atlas_json_key_int(j, "retain_per_repo", opts->retain_per_repo, err);
    }
    if (st == ATLAS_OK && opts->apply) {
        st = atlas_json_key_bool(j, "apply", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, err);
    } else {
        atlas_ipc_params_abort(p);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call(opts->apply ? "maintenance.prune" : "maintenance.plan",
                               atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t n = 0;
    const char *v = NULL;
    (void)atlas_ipc_result_bool(r, "applied", &out->applied);
    if (atlas_ipc_result_int(r, "older_than_days", &n)) {
        out->older_than_days = n;
    }
    if (atlas_ipc_result_int(r, "retain_per_repo", &n)) {
        out->retain_per_repo = n;
    }
    if (atlas_ipc_result_str(r, "cutoff", &v) && v != NULL) {
        copy_str(out->cutoff, sizeof out->cutoff, v);
    }
    if (atlas_ipc_result_int(r, "total_rows", &n)) {
        out->total_rows = n;
    }
    if (atlas_ipc_result_int(r, "total_eligible", &n)) {
        out->total_eligible = n;
    }
    if (atlas_ipc_result_int(r, "total_removed", &n)) {
        out->total_removed = n;
    }
    if (atlas_ipc_result_int(r, "prunable_tables", &n)) {
        out->prunable_tables = (size_t)n;
    }
    if (atlas_ipc_result_int(r, "protected_tables", &n)) {
        out->protected_tables = (size_t)n;
    }
    /* The per-table rows. `table` and `reason` are Atlas-owned constants, but
     * they are copied rather than aliased into the response, which is freed
     * below — the row-callback rule, applied to a response document. */
    size_t rows = 0;
    (void)atlas_ipc_result_arr_len(r, "tables", &rows);
    if (rows > 0) {
        out->tables = calloc(rows, sizeof(*out->tables));
        if (out->tables == NULL) {
            atlas_ipc_response_free(r);
            atlas_buf_free(&raw);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading a maintenance "
                                                          "report");
        }
        out->table_count = rows;
        for (size_t i = 0; i < rows; i++) {
            atlas_maintenance_row *row = &out->tables[i];
            /* The classification, the prunable flag and the written reason come
             * from this binary's own policy, looked up by table name. They are
             * Atlas-owned constants and a report is not the place to start
             * trusting a peer for them; only the counts cross the socket. */
            if (atlas_ipc_result_arr_obj_str(r, "tables", i, "table", &v) && v != NULL) {
                if (!atlas_maintenance_policy_lookup(v, &row->table, &row->cls, &row->prunable,
                                                     &row->reason)) {
                    row->table = "(unknown to this Atlas)";
                    row->reason = "this build's retention policy has no entry for that table";
                }
            }
            (void)atlas_ipc_result_arr_obj_bool(r, "tables", i, "counted", &row->counted);
            if (atlas_ipc_result_arr_obj_int(r, "tables", i, "rows_before", &n)) {
                row->rows_before = n;
            }
            if (atlas_ipc_result_arr_obj_int(r, "tables", i, "rows_eligible", &n)) {
                row->rows_eligible = n;
            }
            if (atlas_ipc_result_arr_obj_int(r, "tables", i, "rows_removed", &n)) {
                row->rows_removed = n;
            }
            if (atlas_ipc_result_arr_obj_int(r, "tables", i, "rows_after", &n)) {
                row->rows_after = n;
            }
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_operation_status_remote(int64_t op_id, atlas_operation_report *out,
                                                   atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "operation_id", op_id, err);
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("operation.get", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *v = NULL;
    int64_t n = 0;
    out->id = op_id;
    if (atlas_ipc_result_str(r, "kind", &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->kind, v, err);
    }
    if (atlas_ipc_result_str(r, "state", &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->state, v, err);
    }
    (void)atlas_ipc_result_bool(r, "done", &out->done);
    (void)atlas_ipc_result_bool(r, "succeeded", &out->succeeded);
    if (atlas_ipc_result_int(r, "duration_ms", &n)) {
        out->duration_ms = n;
    }
    if (atlas_ipc_result_str(r, "message", &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->message, v, err);
    }
    if (atlas_ipc_result_str(r, "detail", &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->detail, v, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* Submits the backup, then polls until it finishes.
 *
 * The two halves used to be one blocking call, and a 437 MiB index took longer
 * than ATLAS_IPC_READ_TIMEOUT_MS — so the client reported "timed out while
 * reading a frame header" and exit 1 while the daemon went on to write and
 * verify a perfectly good backup. Worse, the work ran inside the non-blocking
 * serve loop, so it stalled every other client for the duration. Splitting it
 * fixes both: the request returns as soon as the work is accepted, and the work
 * itself runs on a thread that is not the serve loop. */
atlas_status atlas_service_backup_create_remote(const char *name, atlas_backup_report *out,
                                                atlas_backup_verify_report *verified,
                                                atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK) {
        if (name != NULL && name[0] != '\0') {
            st = atlas_json_key_str(j, "name", name, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("backup.create", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t op_id = 0;
    const char *v = NULL;
    (void)atlas_ipc_result_int(r, "operation_id", &op_id);
    if (atlas_ipc_result_str(r, "backup", &v) && v != NULL) {
        st = atlas_buf_set_str(&out->path, v, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) {
        return st;
    }
    if (op_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the daemon accepted the backup but named no operation to poll");
    }

    announce_operation(op_id, "backup");
    atlas_buf message = ATLAS_BUF_INIT;
    atlas_buf detail = ATLAS_BUF_INIT;
    st = wait_for_operation(op_id, &message, &detail, out, NULL, err);
    if (st == ATLAS_OK && verified != NULL) {
        /* The daemon already verified it before letting the operation succeed.
         * This second read fills in the verification detail the renderers
         * print; it is a read of a finished file, not a re-decision about
         * whether the backup is good. */
        st = atlas_service_backup_verify_remote(name, verified, err);
    }
    atlas_buf_free(&message);
    atlas_buf_free(&detail);
    return st;
}

atlas_status atlas_service_backup_verify_remote(const char *name, atlas_backup_verify_report *out,
                                                atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_ipc_params_begin(&p, &j, err);
    if (st == ATLAS_OK) {
        if (name != NULL && name[0] != '\0') {
            st = atlas_json_key_str(j, "name", name, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("backup.verify", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t op_id = 0;
    const char *v = NULL;
    (void)atlas_ipc_result_int(r, "operation_id", &op_id);
    if (atlas_ipc_result_str(r, "backup", &v) && v != NULL) {
        (void)atlas_buf_set_str(&out->path, v, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    if (op_id <= 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "the daemon accepted the verification but named no operation");
    }

    /* Wait, then read the finished record: it carries the whole report, so the
     * answer arrives from the same place that says the work is done. */
    announce_operation(op_id, "verification");
    atlas_buf message = ATLAS_BUF_INIT;
    atlas_buf detail = ATLAS_BUF_INIT;
    st = wait_for_operation(op_id, &message, &detail, NULL, NULL, err);
    atlas_buf_free(&message);
    atlas_buf_free(&detail);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_buf p2 = ATLAS_BUF_INIT;
    atlas_ipc_params *pp = NULL;
    atlas_json *jj = NULL;
    st = atlas_ipc_params_begin(&pp, &jj, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(jj, "operation_id", op_id, err);
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(pp, &p2, err);
        } else {
            atlas_ipc_params_abort(pp);
        }
    }
    atlas_buf raw2 = ATLAS_BUF_INIT;
    atlas_ipc_response *r2 = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("operation.get", atlas_buf_cstr(&p2), &raw2, &r2, err);
    }
    atlas_buf_free(&p2);
    if (st == ATLAS_OK) {
        take_verify(r2, "", out);
        atlas_ipc_response_free(r2);
    }
    atlas_buf_free(&raw2);
    return st;
}

/* --- decision link add ----------------------------------------------------- */

/* Relate one decision to another, performed by the daemon.
 *
 * The local twin reads the document, re-sends every field and adds one relation.
 * That cannot be done over the socket: the prose comes back safe-encoded and
 * re-sending it encodes it again, so the body drifted every time a relationship
 * was recorded. Here the client sends two ids and the daemon never lets the
 * content out of the database. */
static atlas_status link_op_remote_ev(const char *method, const char *repo, const char *uid,
                                      const char *target_uid, const char *note,
                                      const char *provenance, const char *event,
                                      atlas_decision_outcome *out, bool *removed_out,
                                      atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(repo, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", repo, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "decision_uid", uid, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "decision", uid, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "target", target_uid, err);
        }
        /* Under both names, for the reason `decision_uid` and `decision` are
         * both sent: `decision.link_add` and `decision.link_remove` name the far
         * end `target`, while a routed op serialises every field under its own
         * name and so calls it `edge_target`. Sending one and not the other made
         * `link note` work from a client that owns a context and fail from one
         * that does not — which is the deployment, and the one arrangement a
         * fixture cannot reproduce. */
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "edge_target", target_uid, err);
        }
        /* The reason, under its own key. `target` is a document id and
         * `edge_note` is prose: one key carrying both is the A8.2 defect, and
         * separating them is the fix applied to every new field since. */
        if (st == ATLAS_OK && note != NULL && *note != '\0') {
            st = atlas_json_key_str(j, "edge_note", note, err);
            if (st == ATLAS_OK && provenance != NULL && *provenance != '\0') {
                st = atlas_json_key_str(j, "edge_provenance", provenance, err);
            }
            if (st == ATLAS_OK && event != NULL && *event != '\0') {
                st = atlas_json_key_str(j, "edge_event", event, err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call(method, atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    const char *v = NULL;
    int64_t n = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v) && v != NULL) {
        st = atlas_buf_set_str(&out->repo, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "decision", &v) && v != NULL) {
        st = atlas_buf_set_str(&out->uid, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "state", &v) && v != NULL) {
        /* Parsed against the closed vocabulary before it is stored as text, so
         * a state this binary does not know is a version mismatch rather than
         * something that renders as an unknown word. */
        atlas_decision_state parsed;
        if (!atlas_decision_state_parse(v, &parsed)) {
            st = atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                               "the Atlas daemon reported a decision state this binary does not "
                               "recognise; the two are different versions");
        } else {
            st = atlas_buf_set_str(&out->state, v, err);
        }
    }
    if (atlas_ipc_result_str(r, "content_hash", &v) && v != NULL) {
        (void)snprintf(out->content_hash, sizeof out->content_hash, "%s", v);
    }
    if (atlas_ipc_result_int(r, "revision", &n)) {
        out->revision_no = n;
    }
    if (atlas_ipc_result_bool(r, "created", &b)) {
        out->created = b;
    }
    if (atlas_ipc_result_bool(r, "duplicate", &b)) {
        out->duplicate = b;
    }
    if (atlas_ipc_result_bool(r, "removed", &b) && removed_out != NULL) {
        *removed_out = b;
    }
    out->via_daemon = true;
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

atlas_status atlas_service_decision_link_add_remote(const char *repo, const char *uid,
                                                    const char *target_uid, const char *note,
                                                    const char *provenance,
                                                    atlas_decision_outcome *out, atlas_err *err) {
    return link_op_remote_ev("decision.link_add", repo, uid, target_uid, note, provenance, NULL,
                             out, NULL, err);
}

atlas_status atlas_service_decision_link_note_remote(const char *repo, const char *uid,
                                                     const char *target_uid, const char *note,
                                                     const char *provenance, const char *event,
                                                     atlas_decision_outcome *out, atlas_err *err) {
    return link_op_remote_ev("decision.edge.note", repo, uid, target_uid, note, provenance, event,
                             out, NULL, err);
}

atlas_status atlas_service_decision_link_remove_remote(const char *repo, const char *uid,
                                                       const char *target_uid, const char *note,
                                                       atlas_decision_outcome *out,
                                                       bool *removed_out, atlas_err *err) {
    return link_op_remote_ev("decision.link_remove", repo, uid, target_uid, note, "OPERATOR", NULL,
                             out, removed_out, err);
}

/* --- decision links (migration 10) ----------------------------------------- */

/* The account of one document's relations, read from the daemon. Every value
 * arrives already safe-encoded, so nothing here encodes again — that was the
 * A8.2 defect and it is not repeated. */
atlas_status atlas_service_decision_links_remote(const char *repo, const char *uid,
                                                 atlas_decision_edge_cb cb, void *ud,
                                                 int64_t *count_out, bool *more_out,
                                                 atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    atlas_status st = atlas_db_check_repo_name(repo, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_begin(&p, &j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "repo", repo, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "decision_uid", uid, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "decision", uid, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("decision.links", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        return st;
    }
    int64_t n = 0;
    bool more = false;
    (void)atlas_ipc_result_int(r, "count", &n);
    (void)atlas_ipc_result_bool(r, "more", &more);
    for (int64_t i = 0; st == ATLAS_OK && i < n; i++) {
        atlas_decision_edge_entry e;
        memset(&e, 0, sizeof(e));
        const char *v = NULL;
        int64_t t = 0;
        bool b = false;
        if (atlas_ipc_result_arr_obj_int(r, "edges", (size_t)i, "id", &t)) {
            e.id = t;
        }
        if (atlas_ipc_result_arr_obj_str(r, "edges", (size_t)i, "target", &v)) {
            e.target = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "edges", (size_t)i, "kind", &v)) {
            e.kind = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "edges", (size_t)i, "event", &v)) {
            e.event = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "edges", (size_t)i, "note", &v)) {
            e.note = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "edges", (size_t)i, "provenance", &v)) {
            e.provenance = v;
        }
        if (atlas_ipc_result_arr_obj_str(r, "edges", (size_t)i, "created_at", &v)) {
            e.created_at = v;
        }
        if (atlas_ipc_result_arr_obj_int(r, "edges", (size_t)i, "revision_id", &t)) {
            e.revision_id = t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "edges", (size_t)i, "active", &b)) {
            e.active = b;
        }
        st = cb(&e, ud, err);
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    if (more_out != NULL) {
        *more_out = more;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* --- A8-CI: the compiler-derived semantic reads ----------------------------
 *
 * The remote half of `atlas_service_sem_*`. Same service functions, same
 * report structs, same renderers — only the transport differs, which is what
 * keeps the local and daemon-served answers from drifting apart.
 *
 * Under A7.1 these are the *only* form that works on a deployed machine: the
 * index is 0700 `atlasd`, so an operator's account has no local handle at all
 * and every semantic read is a socket call. */

static void take_freshness(const atlas_ipc_response *r, atlas_sem_generation *gen,
                           atlas_sem_freshness *fresh, const char **reason) {
    const char *v = NULL;
    int64_t t = 0;
    *fresh = ATLAS_SEM_FRESH_ABSENT;
    *reason = NULL;
    if (atlas_ipc_result_str(r, "freshness", &v)) {
        /* Parsed against Atlas' own closed set. A value from anywhere else
         * leaves the safe default rather than being believed. */
        for (int f = 0; f <= (int)ATLAS_SEM_FRESH_REBUILDING; f++) {
            if (strcmp(v, atlas_sem_freshness_name((atlas_sem_freshness)f)) == 0) {
                *fresh = (atlas_sem_freshness)f;
                break;
            }
        }
    }
    if (atlas_ipc_result_str(r, "stale_reason", &v) && atlas_sem_stale_reason_is_known(v)) {
        *reason = atlas_sem_stale_reason_intern(v);
    }
    if (atlas_ipc_result_int(r, "generation_id", &t)) {
        gen->id = t;
    }
    if (atlas_ipc_result_str(r, "indexed_commit", &v)) {
        copy_str(gen->commit_id, sizeof gen->commit_id, v);
    }
}

atlas_status atlas_service_sem_status_remote(const char *name, atlas_sem_status_report *out,
                                             atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\"}", name);
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("sem.status", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        copy_str(out->repo.name, sizeof out->repo.name, v);
    }
    if (atlas_ipc_result_bool(r, "libclang_available", &b)) {
        out->libclang_available = b;
    }
    if (atlas_ipc_result_str(r, "compiler_version", &v)) {
        copy_str(out->compiler_version, sizeof out->compiler_version, v);
    }
    if (atlas_ipc_result_bool(r, "have_generation", &b)) {
        out->have_generation = b;
    }
    take_freshness(r, &out->generation, &out->freshness, &out->stale_reason);

    if (atlas_ipc_result_str(r, "compdb_digest", &v)) {
        copy_str(out->generation.compdb_digest, sizeof out->generation.compdb_digest, v);
    }
    if (atlas_ipc_result_int(r, "compdb_count", &t)) {
        out->generation.compdb_count = t;
    }
    if (atlas_ipc_result_str(r, "completed_at", &v)) {
        copy_str(out->generation.completed_at, sizeof out->generation.completed_at, v);
    }
    if (atlas_ipc_result_str(r, "analyzer_id", &v)) {
        copy_str(out->generation.analyzer_id, sizeof out->generation.analyzer_id, v);
    }
    if (atlas_ipc_result_int(r, "analyzer_version", &t)) {
        out->generation.analyzer_version = t;
    }
    if (atlas_ipc_result_int(r, "tu_total", &t)) {
        out->generation.tu_total = t;
    }
    if (atlas_ipc_result_int(r, "tu_complete", &t)) {
        out->generation.tu_complete = t;
    }
    if (atlas_ipc_result_int(r, "tu_partial", &t)) {
        out->generation.tu_partial = t;
    }
    if (atlas_ipc_result_int(r, "tu_failed", &t)) {
        out->generation.tu_failed = t;
    }
    if (atlas_ipc_result_int(r, "tu_unsupported", &t)) {
        out->generation.tu_unsupported = t;
    }
    if (atlas_ipc_result_int(r, "symbols", &t)) {
        out->generation.symbol_count = t;
    }
    if (atlas_ipc_result_int(r, "edges", &t)) {
        out->generation.edge_count = t;
    }
    if (atlas_ipc_result_int(r, "includes", &t)) {
        out->generation.include_count = t;
    }
    if (atlas_ipc_result_int(r, "duration_ms", &t)) {
        out->generation.duration_ms = t;
    }
    if (atlas_ipc_result_int(r, "units_not_complete", &t)) {
        out->failed_total = t;
    }
    /* The generation is COMPLETE by construction: the daemon only publishes a
     * pointer at one that is, and only a published generation is served. */
    if (out->have_generation) {
        out->generation.status = ATLAS_SEM_GEN_COMPLETE;
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "units", &n);
    for (size_t i = 0; i < n && out->failed_count < ATLAS_SEM_STATUS_MAX_UNITS; i++) {
        atlas_sem_failed_unit *u = &out->failed[out->failed_count];
        if (atlas_ipc_result_arr_obj_str(r, "units", i, "source", &v)) {
            copy_str(u->source, sizeof u->source, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "units", i, "status", &v)) {
            copy_str(u->status, sizeof u->status, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "units", i, "why", &v)) {
            copy_str(u->why, sizeof u->why, v);
        }
        if (atlas_ipc_result_arr_obj_int(r, "units", i, "diagnostics_errors", &t)) {
            u->diagnostics_errors = t;
        }
        out->failed_count++;
    }
    out->failed_truncated = out->failed_total > (int64_t)out->failed_count;

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return ATLAS_OK;
}

atlas_status atlas_service_sem_symbol_remote(const char *name, const char *symbol,
                                             const char *kind, int64_t limit,
                                             atlas_sem_symbols_report *out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        /* Built with the typed writer, never by formatting bytes into JSON —
         * there is still no "write these bytes as JSON" primitive in Atlas. */
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st != ATLAS_OK) {
            atlas_buf_free(&params);
            return st;
        }
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "symbol", symbol, err);
        }
        if (st == ATLAS_OK && kind != NULL && kind[0] != '\0') {
            st = atlas_json_key_str(j, "kind", kind, err);
        }
        if (st == ATLAS_OK && limit > 0) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("sem.symbol", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        copy_str(out->repo.name, sizeof out->repo.name, v);
    }
    take_freshness(r, &out->generation, &out->freshness, &out->stale_reason);
    copy_str(out->query, sizeof out->query, symbol);
    if (atlas_ipc_result_bool(r, "truncated", &b)) {
        out->truncated = b;
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "symbols", &n);
    for (size_t i = 0; i < n; i++) {
        if (out->count >= out->cap) {
            size_t ncap = out->cap == 0 ? 32 : out->cap * 2;
            atlas_sem_symbol_item *ni = realloc(out->items, ncap * sizeof(*ni));
            if (ni == NULL) {
                break;
            }
            out->items = ni;
            out->cap = ncap;
        }
        atlas_sem_symbol_item *it = &out->items[out->count];
        memset(it, 0, sizeof(*it));
        int64_t t = 0;
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "usr", &v)) {
            copy_str(it->usr, sizeof it->usr, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "name", &v)) {
            copy_str(it->name, sizeof it->name, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "kind", &v)) {
            copy_str(it->kind, sizeof it->kind, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "linkage", &v)) {
            copy_str(it->linkage, sizeof it->linkage, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "type", &v)) {
            copy_str(it->type_text, sizeof it->type_text, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "file", &v)) {
            copy_str(it->file_text, sizeof it->file_text, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "symbols", i, "evidence", &v)) {
            copy_str(it->evidence, sizeof it->evidence, v);
        }
        if (atlas_ipc_result_arr_obj_int(r, "symbols", i, "line", &t)) {
            it->line = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "symbols", i, "col", &t)) {
            it->col = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "symbols", i, "end_line", &t)) {
            it->end_line = t;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "symbols", i, "is_definition", &b)) {
            it->is_definition = b;
        }
        if (atlas_ipc_result_arr_obj_bool(r, "symbols", i, "external", &b)) {
            it->external = b;
        }
        out->count++;
    }

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return ATLAS_OK;
}

/* Shared by `sem.graph` and `sem.trace`: both return the same node array and
 * the same summary, so both are read the same way. */
static void take_graph(const atlas_ipc_response *r, atlas_sem_graph_report *out) {
    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        copy_str(out->repo.name, sizeof out->repo.name, v);
    }
    take_freshness(r, &out->generation, &out->freshness, &out->stale_reason);
    if (atlas_ipc_result_str(r, "direction", &v)) {
        out->inbound = strcmp(v, "callers") == 0;
    }

    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "nodes", &n);
    for (size_t i = 0; i < n; i++) {
        if (out->count >= out->cap) {
            size_t ncap = out->cap == 0 ? 32 : out->cap * 2;
            atlas_sem_graph_item *ni = realloc(out->items, ncap * sizeof(*ni));
            if (ni == NULL) {
                break;
            }
            out->items = ni;
            out->cap = ncap;
        }
        atlas_sem_graph_item *it = &out->items[out->count];
        memset(it, 0, sizeof(*it));
        if (atlas_ipc_result_arr_obj_int(r, "nodes", i, "depth", &t)) {
            it->depth = t;
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "usr", &v)) {
            copy_str(it->usr, sizeof it->usr, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "name", &v)) {
            copy_str(it->name, sizeof it->name, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "file", &v)) {
            copy_str(it->file_text, sizeof it->file_text, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "edge_kind", &v)) {
            copy_str(it->edge_kind, sizeof it->edge_kind, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "via", &v)) {
            copy_str(it->via_name, sizeof it->via_name, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "evidence", &v)) {
            copy_str(it->evidence, sizeof it->evidence, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "nodes", i, "site_file", &v)) {
            copy_str(it->site_file, sizeof it->site_file, v);
        }
        if (atlas_ipc_result_arr_obj_int(r, "nodes", i, "line", &t)) {
            it->line = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "nodes", i, "site_line", &t)) {
            it->site_line = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "nodes", i, "candidate_total", &t)) {
            it->candidate_total = t;
        }
        out->count++;
    }

    atlas_sem_walk_summary *s = &out->summary;
    if (atlas_ipc_result_obj_int(r, "summary", "visited", &t)) {
        s->visited = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "emitted", &t)) {
        s->emitted = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "max_depth_reached", &t)) {
        s->max_depth_reached = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "proven", &t)) {
        s->proven = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "candidate", &t)) {
        s->candidate = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "lexical", &t)) {
        s->lexical = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "unknown", &t)) {
        s->unknown = t;
    }
    if (atlas_ipc_result_obj_int(r, "summary", "unresolved_indirect", &t)) {
        s->unresolved_indirect = t;
    }
    if (atlas_ipc_result_obj_bool(r, "summary", "truncated", &b)) {
        s->truncated = b;
    }
    if (atlas_ipc_result_obj_str(r, "summary", "truncated_reason", &v) &&
        atlas_sem_trunc_reason_is_known(v)) {
        s->truncated_reason = atlas_sem_trunc_reason_intern(v);
    }
}

atlas_status atlas_service_sem_graph_remote(const char *name, const char *symbol, bool inbound,
                                            int64_t depth, int64_t limit, bool proven_only,
                                            atlas_sem_graph_report *out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "symbol", symbol, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "inbound", inbound, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(j, "proven_only", proven_only, err);
        }
        if (st == ATLAS_OK && depth > 0) {
            st = atlas_json_key_int(j, "depth", depth, err);
        }
        if (st == ATLAS_OK && limit > 0) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("sem.graph", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    copy_str(out->query, sizeof out->query, symbol);
    take_graph(r, out);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return ATLAS_OK;
}

atlas_status atlas_service_sem_trace_remote(const char *name, const char *from, const char *to,
                                            int64_t depth, atlas_sem_graph_report *out,
                                            atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "from", from, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "to", to, err);
        }
        if (st == ATLAS_OK && depth > 0) {
            st = atlas_json_key_int(j, "depth", depth, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("sem.trace", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }
    (void)snprintf(out->query, sizeof out->query, "%s -> %s", from, to);
    take_graph(r, out);
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return ATLAS_OK;
}

/* --- impact and the context package, over the socket -------------------------
 *
 * Both parse the same item array, so both use one reader. Under A7.1 these are
 * the only forms that work from an operator's account: the index is 0700
 * `atlasd`, and without them `code sem-impact` and `context build` are usable
 * only by the service account — which is exactly the gap this closes. */
static void take_items(const atlas_ipc_response *r, atlas_sem_item **items, size_t *count,
                       size_t *cap) {
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "items", &n);
    for (size_t i = 0; i < n; i++) {
        if (*count >= *cap) {
            size_t ncap = *cap == 0 ? 64 : *cap * 2;
            atlas_sem_item *ni = realloc(*items, ncap * sizeof(*ni));
            if (ni == NULL) {
                return;
            }
            *items = ni;
            *cap = ncap;
        }
        atlas_sem_item *it = &(*items)[*count];
        memset(it, 0, sizeof(*it));
        const char *v = NULL;
        int64_t t = 0;
        if (atlas_ipc_result_arr_obj_str(r, "items", i, "kind", &v)) {
            copy_str(it->kind, sizeof it->kind, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "items", i, "name", &v)) {
            copy_str(it->name, sizeof it->name, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "items", i, "file", &v)) {
            copy_str(it->file_text, sizeof it->file_text, v);
        }
        if (atlas_ipc_result_arr_obj_str(r, "items", i, "evidence", &v)) {
            copy_str(it->evidence, sizeof it->evidence, v);
        }
        /* Re-interned against Atlas' own closed set: a reason that arrived over
         * a socket is a matching string, not Atlas' string. */
        if (atlas_ipc_result_arr_obj_str(r, "items", i, "why", &v)) {
            it->why = atlas_sem_selection_reason_intern(v);
        }
        if (atlas_ipc_result_arr_obj_int(r, "items", i, "line", &t)) {
            it->line = t;
        }
        if (atlas_ipc_result_arr_obj_int(r, "items", i, "depth", &t)) {
            it->depth = t;
        }
        (*count)++;
    }
}

atlas_status atlas_service_sem_impact_remote(const char *name, const char *subject, int64_t depth,
                                             int64_t limit, atlas_sem_impact_report *out,
                                             atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = atlas_json_key_str(j, "repo", name, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "subject", subject, err);
        }
        if (st == ATLAS_OK && depth > 0) {
            st = atlas_json_key_int(j, "depth", depth, err);
        }
        if (st == ATLAS_OK && limit > 0) {
            st = atlas_json_key_int(j, "limit", limit, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("sem.impact", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        copy_str(out->repo.name, sizeof out->repo.name, v);
    }
    take_freshness(r, &out->generation, &out->freshness, &out->stale_reason);
    copy_str(out->query, sizeof out->query, subject);
    if (atlas_ipc_result_str(r, "subject_kind", &v)) {
        out->subject_is_path = strcmp(v, "file") == 0;
    }
    if (atlas_ipc_result_bool(r, "subject_found", &b)) {
        out->subject_found = b;
    }
    take_items(r, &out->items, &out->count, &out->cap);
    if (atlas_ipc_result_int(r, "proven", &t)) {
        out->proven = t;
    }
    if (atlas_ipc_result_int(r, "candidate", &t)) {
        out->candidate = t;
    }
    if (atlas_ipc_result_int(r, "lexical", &t)) {
        out->lexical = t;
    }
    if (atlas_ipc_result_int(r, "unresolved_indirect", &t)) {
        out->unresolved_indirect = t;
    }
    if (atlas_ipc_result_bool(r, "truncated", &b)) {
        out->truncated = b;
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return ATLAS_OK;
}

atlas_status atlas_service_sem_context_remote(const atlas_sem_context_req *req,
                                              atlas_sem_context_report *out, atlas_err *err) {
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_db_check_repo_name(req->repo, err);
    if (st == ATLAS_OK) {
        atlas_ipc_params *p = NULL;
        atlas_json *j = NULL;
        st = atlas_ipc_params_begin(&p, &j, err);
        if (st != ATLAS_OK) {
            return st;
        }
        st = atlas_json_key_str(j, "repo", req->repo, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(j, "task", req->task, err);
        }
        if (st == ATLAS_OK && req->depth > 0) {
            st = atlas_json_key_int(j, "depth", req->depth, err);
        }
        if (st == ATLAS_OK && req->max_tokens > 0) {
            st = atlas_json_key_int(j, "max_tokens", req->max_tokens, err);
        }
        if (st == ATLAS_OK && req->max_items > 0) {
            st = atlas_json_key_int(j, "max_items", req->max_items, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_ipc_params_finish(p, &params, err);
        } else {
            atlas_ipc_params_abort(p);
        }
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    if (st == ATLAS_OK) {
        st = atlas_remote_call("sem.context", atlas_buf_cstr(&params), &raw, &r, err);
    }
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t t = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        copy_str(out->repo.name, sizeof out->repo.name, v);
    }
    if (atlas_ipc_result_str(r, "commit", &v)) {
        copy_str(out->repo.scanned_head, sizeof out->repo.scanned_head, v);
    }
    take_freshness(r, &out->generation, &out->freshness, &out->stale_reason);
    copy_str(out->task, sizeof out->task, req->task);
    if (atlas_ipc_result_int(r, "budget_bytes", &t)) {
        out->budget_bytes = t;
    }
    if (atlas_ipc_result_int(r, "used_bytes", &t)) {
        out->used_bytes = t;
    }
    if (atlas_ipc_result_bool(r, "budget_reached", &b)) {
        out->budget_reached = b;
    }
    take_items(r, &out->items, &out->count, &out->cap);
    /* The package's own gaps, re-interned so what reaches an operator is Atlas'
     * literal rather than a string that merely matched. */
    size_t n = 0;
    (void)atlas_ipc_result_arr_len(r, "not_included", &n);
    static const char *const MISSING[] = {
        ATLAS_SEM_MISSING_INDEX, ATLAS_SEM_MISSING_STALE, ATLAS_SEM_MISSING_SEEDS,
        ATLAS_SEM_MISSING_BUDGET, ATLAS_SEM_MISSING_DECISIONS,
    };
    for (size_t i = 0; i < n && out->missing_count < 8u; i++) {
        if (!atlas_ipc_result_arr_str(r, "not_included", i, &v)) {
            continue;
        }
        for (size_t k = 0; k < sizeof(MISSING) / sizeof(MISSING[0]); k++) {
            if (strcmp(v, MISSING[k]) == 0) {
                out->missing[out->missing_count++] = MISSING[k];
                break;
            }
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return ATLAS_OK;
}

void atlas_operation_report_init(atlas_operation_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->kind);
    atlas_buf_init(&r->state);
    atlas_buf_init(&r->message);
    atlas_buf_init(&r->detail);
}

void atlas_operation_report_free(atlas_operation_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->kind);
    atlas_buf_free(&r->state);
    atlas_buf_free(&r->message);
    atlas_buf_free(&r->detail);
}

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
#include "atlas/code.h"
#include "atlas/gate.h"
#include "atlas/ipc.h"
#include "atlas/pathrep.h"
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

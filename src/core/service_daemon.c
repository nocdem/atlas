/* Atlas - service-layer operations for the daemon, sync and events commands.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * As in A0, the renderers consume these results and nothing else: there is no
 * database access or git invocation above this layer, and the human and JSON
 * output of `atlas daemon status` come from one struct.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/lock.h"
#include "atlas/service.h"
#include "core/service_internal.h"

/* --- daemon status ------------------------------------------------------- */

void atlas_daemon_status_report_init(atlas_daemon_status_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->socket_path);
    atlas_buf_init(&r->lock_holder);
    atlas_buf_init(&r->atlas_version);
    atlas_daemon_record_init(&r->record);
}

void atlas_daemon_status_report_free(atlas_daemon_status_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->socket_path);
    atlas_buf_free(&r->lock_holder);
    atlas_buf_free(&r->atlas_version);
    atlas_daemon_record_free(&r->record);
}

typedef struct state_tally {
    atlas_db *db;
    atlas_daemon_status_report *rep;
    atlas_status st;
} state_tally;

static atlas_status tally_repo(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    state_tally *t = (state_tally *)ud;
    t->rep->repo_count++;

    atlas_index_state s;
    atlas_index_state_init(&s);
    atlas_status st = atlas_db_index_state_get(t->db, ri->id, &s, err);
    if (st == ATLAS_OK && s.present) {
        switch (s.watch_state) {
        case ATLAS_WATCH_WATCHING: t->rep->watched_repos++; break;
        case ATLAS_WATCH_DEGRADED:
        case ATLAS_WATCH_INCOMPLETE:
        case ATLAS_WATCH_ERROR: t->rep->degraded_repos++; break;
        /* P0. Priming counts as neither, and that is the point of having it.
         *
         * It is not `watching`, because the watch set is not fully installed and
         * a repository counted there would let `watching == repositories` be
         * true while part of a tree was unobserved. It is not degraded either,
         * because nothing has gone wrong — so counting it there would make an
         * ordinary startup look like a fault. It is reported on its own. */
        case ATLAS_WATCH_PRIMING: t->rep->priming_repos++; break;
        case ATLAS_WATCH_UNWATCHED:
        default: break;
        }
        if (s.event_gap) {
            t->rep->repos_with_gap++;
        }
    }
    atlas_index_state_free(&s);
    return st;
}

atlas_status atlas_service_daemon_status(atlas_ctx *ctx, atlas_daemon_status_report *out,
                                         atlas_err *err) {
    out->protocol_version = (int)ATLAS_IPC_PROTOCOL_VERSION;
    atlas_status st = atlas_buf_set_str(&out->atlas_version, ATLAS_VERSION_STRING, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Liveness has two independent answers, and reporting both is the point.
     *
     * The lock says whether *something* owns the writer. The socket says whether
     * that something is answering. A daemon wedged in a way that stops it
     * serving would show running-but-not-reachable, which is exactly the state a
     * user needs to see and which a single boolean would hide. */
    bool held = false;
    atlas_err probe_err;
    atlas_err_init(&probe_err);
    if (atlas_lock_probe(atlas_ctx_data_dir(ctx), &held, &out->lock_holder, &probe_err) ==
        ATLAS_OK) {
        out->running = held;
    }
    /* This process may itself be the holder. flock is per open file description,
     * so the probe's separate descriptor conflicts with our own exclusive lock
     * and reports "held" — about us. Reporting that as a running daemon would be
     * exactly backwards, so both the flag and the holder text are cleared. */
    if (atlas_ctx_is_writer(ctx)) {
        out->running = false;
        atlas_buf_reset(&out->lock_holder);
    }

    atlas_err sock_err;
    atlas_err_init(&sock_err);
    if (atlas_ipc_socket_path(&out->socket_path, &sock_err) != ATLAS_OK) {
        /* No XDG_RUNTIME_DIR is a legitimate configuration to report on, not a
         * reason to fail: the index is still describable. */
        (void)atlas_buf_set_str(&out->socket_path, "", err);
    }
    out->reachable = atlas_ipc_daemon_reachable();

    st = atlas_db_daemon_get(atlas_ctx_db(ctx), &out->record, err);
    if (st != ATLAS_OK) {
        return st;
    }

    state_tally t = {atlas_ctx_db(ctx), out, ATLAS_OK};
    return atlas_db_repo_list(atlas_ctx_db(ctx), tally_repo, &t, err);
}

/* --- per-repository state ------------------------------------------------ */

void atlas_repo_state_report_init(atlas_repo_state_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_index_state_init(&r->state);
}

void atlas_repo_state_report_free(atlas_repo_state_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    atlas_index_state_free(&r->state);
}

/* "Current" means a completed generation exists AND nothing is known to have
 * been missed. A repository with an outstanding event gap is never current,
 * however recently it was reconciled — Atlas does not get to describe an index
 * as up to date while it knows there is a hole in what it observed.
 *
 * One authority for the strings, because both the local read and the
 * daemon-served one produce them and a second copy would drift. The daemon
 * sends its own `index_current`/`not_current_reason` too; the remote path
 * re-derives from the flags rather than trusting them, so the two answers
 * cannot disagree about a state they both hold. */
static void derive_index_current(atlas_repo_state_report *out) {
    out->index_current = false;
    if (!out->state.present || out->state.last_complete_generation == 0) {
        out->not_current_reason = "the repository has never been reconciled";
    } else if (out->state.event_gap) {
        out->not_current_reason =
            "filesystem events were missed; a full reconciliation is outstanding";
    } else if (out->state.pending_full_reconcile) {
        out->not_current_reason = "a full reconciliation is outstanding";
    } else if (out->state.watch_state == ATLAS_WATCH_ERROR) {
        out->not_current_reason = "the watcher failed and is not observing this repository";
    } else if (out->state.watch_state == ATLAS_WATCH_DEGRADED) {
        out->not_current_reason = "the watcher is degraded and may not observe every change";
    } else if (out->state.watch_state == ATLAS_WATCH_PRIMING) {
        /* P0. Watches are still being installed, or a directory is waiting for
         * git to say whether it is ignored. Either way there is a part of this
         * tree producing events nobody is receiving, which is the same claim an
         * event gap makes and gets the same answer. */
        out->not_current_reason = "the watcher has not finished installing this repository's "
                                  "watches";
    } else {
        out->index_current = true;
    }
}

atlas_status atlas_service_repo_state(atlas_ctx *ctx, const char *name,
                                      atlas_repo_state_report *out, atlas_err *err) {
    atlas_status st = atlas_service_require_repo(ctx, name, &out->repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_index_state_get(atlas_ctx_db(ctx), out->repo.id, &out->state, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_db_events_head(atlas_ctx_db(ctx), out->repo.id, &out->event_cursor, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The one claim that matters, and the reason it is computed here rather than
     * left to each renderer: "current" means a completed generation exists AND
     * nothing is known to have been missed. A repository with an outstanding
     * event gap is never current, however recently it was reconciled — Atlas
     * does not get to describe an index as up to date while it knows there is a
     * hole in what it observed. */
    derive_index_current(out);
    return ATLAS_OK;
}

atlas_status atlas_service_events(atlas_ctx *ctx, const char *name, int64_t since, int64_t limit,
                                  atlas_event_cb cb, void *ud, int64_t *count_out,
                                  int64_t *next_cursor_out, bool *more_out, atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_db_events_since(atlas_ctx_db(ctx), info.id, since, limit, cb, ud, count_out,
                                   next_cursor_out, more_out, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- sync ---------------------------------------------------------------- */

void atlas_sync_report_init(atlas_sync_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_reconcile_summary_init(&r->summary);
}

void atlas_sync_report_free(atlas_sync_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_reconcile_summary_free(&r->summary);
}

/* Extracts an integer member from a daemon response.
 *
 * The response is a document Atlas itself produced through the streaming writer,
 * so this is not a JSON parser and must not become one: it is a targeted lookup
 * over a known shape. Anything more general belongs behind yyjson. */
static bool response_int(const char *json, const char *key, int64_t *out) {
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return false;
    }
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p += n;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

static bool response_is_ok(const char *json) {
    return strstr(json, "\"ok\":true") != NULL;
}

/* Pulls the daemon's error message out of a failure response so the user sees
 * what the daemon said rather than "the request failed". */
static void response_error_message(const char *json, atlas_buf *out) {
    const char *p = strstr(json, "\"message\":\"");
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (p == NULL) {
        (void)atlas_buf_set_str(out, "the Atlas daemon reported a failure", &ignore);
        return;
    }
    p += 11;
    atlas_buf_reset(out);
    /* The value was written by the streaming JSON writer, so the only escapes it
     * can contain are the ones that writer emits. Unescaping just enough to read
     * it back is safe; the text was already safe-encoded before being written. */
    for (; *p != '\0' && *p != '"'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            char c = *p;
            if (c == 'n') {
                c = ' ';
            } else if (c == 't') {
                c = ' ';
            } else if (c == 'u') {
                continue; /* skip the escape marker; the digits follow harmlessly */
            }
            (void)atlas_buf_append_ch(out, c, &ignore);
            continue;
        }
        (void)atlas_buf_append_ch(out, *p, &ignore);
    }
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Builds a `{"repo":"NAME", ...}` params object.
 *
 * `name` has already been validated as [A-Za-z0-9._-] by
 * atlas_db_check_repo_name, so it cannot carry a quote or a control byte into
 * the document. The validation is repeated here rather than assumed, because
 * this function is one edit away from being called with something else. */
static atlas_status build_repo_params(const char *name, const char *extra, atlas_buf *out,
                                      atlas_err *err) {
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_buf_reset(out);
    st = atlas_buf_appendf(out, err, "{\"repo\":\"%s\"", name);
    if (st == ATLAS_OK && extra != NULL) {
        st = atlas_buf_append_str(out, extra, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '}', err);
    }
    return st;
}

/* Runs the pass in this process. Only reachable when no daemon holds the writer
 * lock, which atlas_ctx_open already established. */
static atlas_status sync_offline(atlas_ctx *ctx, const char *name, bool full,
                                 atlas_sync_report *out, atlas_err *err) {
    if (!atlas_ctx_is_writer(ctx)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "another Atlas writer owns the index and no daemon is answering on "
                             "the IPC socket. Start the daemon (systemctl --user start atlas) or "
                             "wait for the other command to finish.");
    }
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_service_require_repo(ctx, name, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    atlas_git *g = NULL;
    st = atlas_service_open_repo_git(&info, atlas_ctx_data_dir(ctx), &g, err);
    if (st == ATLAS_OK) {
        atlas_reconcile_opts opts;
        atlas_reconcile_opts_init(&opts);
        opts.full = full;
        opts.workers = NULL; /* a one-shot invocation hashes serially */
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

atlas_status atlas_service_sync(atlas_ctx *ctx, const char *name, bool full, bool wait,
                                int timeout_ms, atlas_sync_report *out, atlas_err *err) {
    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_err sock_err;
    atlas_err_init(&sock_err);
    bool have_socket = (atlas_ipc_socket_path(&sock, &sock_err) == ATLAS_OK);

    /* Reachability is not the question: a daemon that answers may own a
     * different index, and routing to it would apply this sync there while
     * `--data-dir` said otherwise. It has to own *this* context's directory. */
    if (!have_socket || !atlas_ipc_daemon_owns(atlas_ctx_data_dir(ctx))) {
        atlas_buf_free(&sock);
        return sync_offline(ctx, name, full, out, err);
    }

    atlas_buf params = ATLAS_BUF_INIT;
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_status st = build_repo_params(name, full ? ",\"full\":true" : NULL, &params, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_call(atlas_buf_cstr(&sock), "repo.sync", atlas_buf_cstr(&params), &resp,
                            err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&sock);
        atlas_buf_free(&params);
        atlas_buf_free(&resp);
        return st;
    }
    if (!response_is_ok(atlas_buf_cstr(&resp))) {
        atlas_buf msg = ATLAS_BUF_INIT;
        response_error_message(atlas_buf_cstr(&resp), &msg);
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s", atlas_buf_cstr(&msg));
        atlas_buf_free(&msg);
        atlas_buf_free(&sock);
        atlas_buf_free(&params);
        atlas_buf_free(&resp);
        return st;
    }

    out->via_daemon = true;
    (void)response_int(atlas_buf_cstr(&resp), "sync_seq", &out->sync_seq);

    if (!wait) {
        atlas_buf_free(&sock);
        atlas_buf_free(&params);
        atlas_buf_free(&resp);
        return ATLAS_OK;
    }

    /* Waiting is done by polling the published state rather than by holding the
     * connection open. A daemon that blocks a connection until a pass finishes
     * is a daemon whose serve loop stalls behind one client's long request; the
     * sequence number exists so the client can wait instead. */
    out->waited = true;
    int64_t deadline = monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 120000);
    st = build_repo_params(name, NULL, &params, err);
    while (st == ATLAS_OK && monotonic_ms() < deadline) {
        struct timespec nap = {0, 100L * 1000000L};
        (void)nanosleep(&nap, NULL);
        st = atlas_ipc_call(atlas_buf_cstr(&sock), "repo.state", atlas_buf_cstr(&params), &resp,
                            err);
        if (st != ATLAS_OK) {
            break;
        }
        if (!response_is_ok(atlas_buf_cstr(&resp))) {
            continue;
        }
        int64_t seq = 0;
        int64_t gen = 0;
        if (response_int(atlas_buf_cstr(&resp), "last_sync_seq", &seq) && seq >= out->sync_seq) {
            (void)response_int(atlas_buf_cstr(&resp), "last_complete_generation", &gen);
            out->generation = gen;
            out->completed = true;
            break;
        }
    }
    if (st == ATLAS_OK && !out->completed) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                           "the reconciliation of \"%s\" had not completed after %d ms; it is "
                           "still queued in the daemon",
                           name, timeout_ms > 0 ? timeout_ms : 120000);
    }
    atlas_buf_free(&sock);
    atlas_buf_free(&params);
    atlas_buf_free(&resp);
    return st;
}

/* --- A7.1: reads served by the daemon -------------------------------------
 *
 * Under a system deployment the index is owned by `atlasd` and is 0700, so a
 * client uid cannot open it — not because Atlas refuses, but because the kernel
 * does, and it has to stay that way while `atlas-worker` is a member of the
 * client group. These two functions are how the CLI answers `repo list` and
 * `status <repo>` in that deployment: over the socket, with no context, no data
 * directory and no database handle.
 *
 * There is deliberately **no fallback to a local read**. A7.1's rule is that a
 * client which cannot reach the daemon must fail rather than quietly read the
 * pre-cutover per-user database, and a fallback here would be exactly that. */

static atlas_status daemon_read(const char *method, const char *params, atlas_buf *raw,
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

/* Copies one listing item into the struct both renderers already consume, so
 * the daemon-served listing and the local one render identically. Absent keys
 * leave their fields empty rather than failing: a daemon older than this binary
 * answers a shorter object, and a blank column is a better outcome than a
 * refusal to list anything. */
static void repo_item_from_response(const atlas_ipc_response *r, size_t i, atlas_repo_info *ri,
                                    atlas_err *err) {
    const char *v = NULL;
    int64_t n = 0;
    bool b = false;
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "repo", &v)) {
        (void)snprintf(ri->name, sizeof(ri->name), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "root", &v)) {
        (void)atlas_buf_set_str(&ri->root_path_text, v, err);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "last_scan_at", &v)) {
        (void)snprintf(ri->last_scan_at, sizeof(ri->last_scan_at), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "scanned_head", &v)) {
        (void)snprintf(ri->scanned_head, sizeof(ri->scanned_head), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "branch", &v)) {
        (void)snprintf(ri->current_branch, sizeof(ri->current_branch), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "head_state", &v)) {
        (void)snprintf(ri->head_state, sizeof(ri->head_state), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "object_format", &v)) {
        (void)snprintf(ri->object_format, sizeof(ri->object_format), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_str(r, "repositories", i, "registered_at", &v)) {
        (void)snprintf(ri->registered_at, sizeof(ri->registered_at), "%s", v);
    }
    if (atlas_ipc_result_arr_obj_int(r, "repositories", i, "id", &n)) {
        ri->id = n;
    }
    if (atlas_ipc_result_arr_obj_bool(r, "repositories", i, "dirty", &b)) {
        ri->dirty = b;
    }
    if (atlas_ipc_result_arr_obj_bool(r, "repositories", i, "linked_worktree", &b)) {
        ri->is_linked_worktree = b;
    }
}

atlas_status atlas_service_repo_list_remote(atlas_repo_cb cb, void *ud, int64_t *count_out,
                                            atlas_err *err) {
    *count_out = 0;
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    atlas_status st = daemon_read("repo.list", "{}", &raw, &r, err);
    for (size_t i = 0; st == ATLAS_OK; i++) {
        const char *probe = NULL;
        if (!atlas_ipc_result_arr_obj_str(r, "repositories", i, "repo", &probe)) {
            break; /* past the end of the array */
        }
        atlas_repo_info ri;
        atlas_repo_info_init(&ri);
        repo_item_from_response(r, i, &ri, err);
        st = cb(&ri, ud, err);
        atlas_repo_info_free(&ri);
        if (st == ATLAS_OK) {
            (*count_out)++;
        }
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

/* `atlas status NAME` with the index behind the daemon.
 *
 * The index facts come over the socket; the live git observation is taken here,
 * fresh, exactly as the local path takes it. That split is not a convenience:
 * head drift is the whole point of the report, and a "live" state observed by
 * the daemon at some other moment would be the stale half of the comparison
 * pretending to be the fresh one. */
atlas_status atlas_service_status_remote(const char *name, atlas_status_report *out,
                                         atlas_err *err) {
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    st = build_repo_params(name, NULL, &params, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&params);
        return st;
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    st = daemon_read("repo.state", atlas_buf_cstr(&params), &raw, &r, err);
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t n = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        (void)snprintf(out->repo.name, sizeof(out->repo.name), "%s", v);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "root", &v)) {
        st = atlas_buf_set_str(&out->repo.root_path_text, v, err);
        if (st == ATLAS_OK) {
            /* The raw bytes are what git is addressed with, and `root` is the
             * reversible `%XX` display form — decoded rather than used as-is,
             * because a path is bytes and may not be UTF-8. */
            atlas_buf_reset(&out->repo.root_path);
            st = atlas_path_text_decode(v, strlen(v), &out->repo.root_path, err);
        }
    }
    if (atlas_ipc_result_str(r, "last_scan_at", &v)) {
        (void)snprintf(out->repo.last_scan_at, sizeof(out->repo.last_scan_at), "%s", v);
    }
    if (atlas_ipc_result_str(r, "scanned_head", &v)) {
        (void)snprintf(out->repo.scanned_head, sizeof(out->repo.scanned_head), "%s", v);
    }
    if (atlas_ipc_result_str(r, "branch", &v)) {
        (void)snprintf(out->repo.current_branch, sizeof(out->repo.current_branch), "%s", v);
    }
    if (atlas_ipc_result_str(r, "head_state", &v)) {
        (void)snprintf(out->repo.head_state, sizeof(out->repo.head_state), "%s", v);
    }
    if (atlas_ipc_result_str(r, "object_format", &v)) {
        (void)snprintf(out->repo.object_format, sizeof(out->repo.object_format), "%s", v);
    }
    if (atlas_ipc_result_str(r, "registered_at", &v)) {
        (void)snprintf(out->repo.registered_at, sizeof(out->repo.registered_at), "%s", v);
    }
    if (atlas_ipc_result_int(r, "id", &n)) {
        out->repo.id = n;
    }
    if (atlas_ipc_result_bool(r, "dirty", &b)) {
        out->repo.dirty = b;
    }
    if (atlas_ipc_result_bool(r, "linked_worktree", &b)) {
        out->repo.is_linked_worktree = b;
    }
    if (atlas_ipc_result_int(r, "files_live", &n)) {
        out->counts.files_live = n;
    }
    if (atlas_ipc_result_int(r, "files_deleted", &n)) {
        out->counts.files_deleted = n;
    }
    if (atlas_ipc_result_int(r, "commits", &n)) {
        out->counts.commits = n;
    }
    if (atlas_ipc_result_int(r, "changes", &n)) {
        out->counts.changes = n;
    }
    if (atlas_ipc_result_int(r, "scans", &n)) {
        out->counts.scans = n;
    }
    if (atlas_ipc_result_int(r, "evidence", &n)) {
        out->counts.evidence = n;
    }
    if (atlas_ipc_result_int(r, "compile_databases", &n)) {
        out->counts.compile_databases = n;
    }
    if (atlas_ipc_result_int(r, "sibling_worktrees", &n)) {
        out->sibling_worktrees = n;
    }
    if (atlas_ipc_result_int(r, "last_scan_id", &n)) {
        out->repo.last_scan_id = n;
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "git_common_dir", &v)) {
        atlas_buf_reset(&out->repo.git_common_dir);
        st = atlas_path_text_decode(v, strlen(v), &out->repo.git_common_dir, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "git_dir", &v)) {
        atlas_buf_reset(&out->repo.git_dir);
        st = atlas_path_text_decode(v, strlen(v), &out->repo.git_dir, err);
    }
    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) {
        return st;
    }

    out->never_scanned = (out->repo.last_scan_id == 0);
    out->scanned = !out->never_scanned;
    /* A13: the remote path. The index facts arrived over the socket; the live
     * HEAD is observed here, in the client, which runs as the operator and can
     * read the tree. NULL is the correct answer rather than a limitation. */
    return atlas_service_status_observe_live(out, NULL, err);
}

atlas_status atlas_service_repo_state_remote(const char *name, atlas_repo_state_report *out,
                                             atlas_err *err) {
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    st = build_repo_params(name, NULL, &params, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&params);
        return st;
    }
    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = NULL;
    st = daemon_read("repo.state", atlas_buf_cstr(&params), &raw, &r, err);
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        atlas_ipc_response_free(r);
        atlas_buf_free(&raw);
        return st;
    }

    const char *v = NULL;
    int64_t n = 0;
    bool b = false;
    if (atlas_ipc_result_str(r, "repo", &v)) {
        (void)snprintf(out->repo.name, sizeof(out->repo.name), "%s", v);
    }
    if (atlas_ipc_result_str(r, "root", &v)) {
        st = atlas_buf_set_str(&out->repo.root_path_text, v, err);
        if (st == ATLAS_OK) {
            /* The raw bytes too. `atlas diff` reads no index — it resolves the
             * repository and then observes git — so this row is what git is
             * addressed with, and a path is bytes rather than text. */
            atlas_buf_reset(&out->repo.root_path);
            st = atlas_path_text_decode(v, strlen(v), &out->repo.root_path, err);
        }
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "git_common_dir", &v)) {
        atlas_buf_reset(&out->repo.git_common_dir);
        st = atlas_path_text_decode(v, strlen(v), &out->repo.git_common_dir, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "git_dir", &v)) {
        atlas_buf_reset(&out->repo.git_dir);
        st = atlas_path_text_decode(v, strlen(v), &out->repo.git_dir, err);
    }
    if (atlas_ipc_result_int(r, "id", &n)) {
        out->repo.id = n;
    }
    if (atlas_ipc_result_int(r, "generation", &n)) {
        out->state.generation = n;
    }
    if (atlas_ipc_result_int(r, "last_complete_generation", &n)) {
        out->state.last_complete_generation = n;
    }
    if (atlas_ipc_result_int(r, "last_sync_seq", &n)) {
        out->state.last_sync_seq = n;
    }
    if (atlas_ipc_result_int(r, "watched_directories", &n)) {
        out->state.watched_dirs = n;
    }
    /* P0. Absent keys keep the initialised zero, so a new CLI against an older
     * daemon reads "not reported" rather than a confident zero, and
     * `watch_reason` stays UNKNOWN rather than being rendered as the nearest
     * match. */
    if (atlas_ipc_result_int(r, "watched_source", &n)) {
        out->state.watched_source = n;
    }
    if (atlas_ipc_result_int(r, "watched_meta", &n)) {
        out->state.watched_meta = n;
    }
    if (atlas_ipc_result_int(r, "watched_shared", &n)) {
        out->state.watched_shared = n;
    }
    if (atlas_ipc_result_str(r, "watch_reason", &v)) {
        out->state.watch_reason = atlas_watch_reason_parse(v);
    }
    if (atlas_ipc_result_int(r, "event_cursor", &n)) {
        out->event_cursor = n;
    }
    if (atlas_ipc_result_bool(r, "event_gap", &b)) {
        out->state.event_gap = b;
    }
    if (atlas_ipc_result_bool(r, "pending_full_reconcile", &b)) {
        out->state.pending_full_reconcile = b;
    }
    if (atlas_ipc_result_str(r, "last_reconcile_at", &v)) {
        (void)snprintf(out->state.last_reconcile_at, sizeof(out->state.last_reconcile_at), "%s", v);
    }
    if (atlas_ipc_result_str(r, "last_complete_at", &v)) {
        (void)snprintf(out->state.last_complete_at, sizeof(out->state.last_complete_at), "%s", v);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "watch_detail", &v)) {
        st = atlas_buf_set_str(&out->state.watch_detail, v, err);
    }
    if (st == ATLAS_OK && atlas_ipc_result_str(r, "last_error", &v)) {
        st = atlas_buf_set_str(&out->state.last_error, v, err);
    }
    if (atlas_ipc_result_str(r, "watch_state", &v)) {
        out->state.watch_state = atlas_watch_state_parse(v);
    }
    /* `present` is not on the wire: the daemon answered about a repository it
     * holds, and a row it could describe is a row that exists. */
    out->state.present = true;
    derive_index_current(out);

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    return st;
}

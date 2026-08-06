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
    } else {
        out->index_current = true;
    }
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
    st = atlas_service_open_repo_git(&info, &g, err);
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

    if (!have_socket || !atlas_ipc_daemon_reachable()) {
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

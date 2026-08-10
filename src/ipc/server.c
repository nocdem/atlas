/* Atlas - the IPC serve loop and method dispatch.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One request at a time, on the main thread.
 *
 * That is a deliberate choice rather than a shortcut. A thread per connection
 * would put an unbounded number of SQLite handles and an unbounded amount of
 * concurrency behind a socket whose whole purpose is to answer cheap questions
 * about an index. Every read here is a bounded query over a read-only handle,
 * and every mutation is handed to the writer with a deadline, so the longest a
 * single request can occupy the loop is bounded by that deadline.
 *
 * Responses are built with the first-party streaming JSON writer, not with
 * yyjson: the escaping contract A0 established is the contract the daemon
 * speaks, and there is exactly one implementation of it.
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/json.h"
#include "atlas/safetext.h"
#include "atlas/service.h"
#include "daemon/daemon_internal.h"
#include "ipc/server_internal.h"

/* --- response construction ----------------------------------------------
 *
 * Built into memory rather than streamed to the socket, because the frame
 * header carries the payload length and the length is not known until the
 * document is finished. The buffer is bounded by ATLAS_IPC_MAX_RESPONSE_BYTES,
 * checked before the frame is sent. */

typedef struct response_builder {
    FILE *stream;
    char *buffer;
    size_t size;
    atlas_json *j;
} response_builder;

static atlas_status rb_open(response_builder *rb, atlas_err *err) {
    memset(rb, 0, sizeof(*rb));
    rb->stream = open_memstream(&rb->buffer, &rb->size);
    if (rb->stream == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot open a response buffer");
    }
    rb->j = atlas_json_new(rb->stream, err);
    if (rb->j == NULL) {
        (void)fclose(rb->stream);
        free(rb->buffer);
        memset(rb, 0, sizeof(*rb));
        return err->status;
    }
    return ATLAS_OK;
}

static atlas_status rb_finish(response_builder *rb, atlas_buf *out, atlas_err *err) {
    atlas_status st = atlas_json_finish(rb->j, err);
    rb->j = NULL;
    if (fclose(rb->stream) != 0 && st == ATLAS_OK) {
        st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot finish a response buffer");
    }
    rb->stream = NULL;
    if (st == ATLAS_OK) {
        st = atlas_buf_set(out, rb->buffer, rb->size, err);
    }
    free(rb->buffer);
    rb->buffer = NULL;
    return st;
}

static void rb_abort(response_builder *rb) {
    if (rb->j != NULL) {
        atlas_json_free(rb->j);
        rb->j = NULL;
    }
    if (rb->stream != NULL) {
        (void)fclose(rb->stream);
        rb->stream = NULL;
    }
    free(rb->buffer);
    rb->buffer = NULL;
}

/* A complete error document.
 *
 * Written from scratch rather than appended to a half-built success document:
 * a response that is partly one answer and partly another is worse than a plain
 * failure. `status` is the same code the CLI exits with, so a caller has one
 * vocabulary rather than two. */
static atlas_status build_error(const char *id, const atlas_err *err, atlas_buf *out,
                                atlas_err *werr) {
    response_builder rb;
    atlas_status st = rb_open(&rb, werr);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);
    st = atlas_json_obj_begin(rb.j, werr);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(rb.j, "id", id != NULL ? id : "0", werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(rb.j, "ok", false, werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(rb.j, "error", werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(rb.j, werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(rb.j, "status", (int64_t)err->status, werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(rb.j, "code", atlas_status_name(err->status), werr);
    }
    if (st == ATLAS_OK) {
        /* The message can quote a repository name or a git error, both of which
         * are untrusted, so it is safe-encoded on the way out. */
        st = atlas_json_key_str(rb.j, "message", atlas_safe(&safe, atlas_err_msg(err)), werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(rb.j, werr);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(rb.j, werr);
    }
    atlas_safe_pool_free(&safe);
    if (st != ATLAS_OK) {
        rb_abort(&rb);
        return st;
    }
    return rb_finish(&rb, out, werr);
}

/* --- method implementations ---------------------------------------------- */

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static atlas_status method_ping(dispatch_state *ds, const atlas_ipc_request *req, atlas_err *err) {
    (void)req;
    atlas_status st = atlas_json_key_bool(ds->j, "pong", true, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "atlas", ATLAS_VERSION_STRING, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "protocol", (int64_t)ATLAS_IPC_PROTOCOL_VERSION, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "pid", (int64_t)getpid(), err);
    }
    /* Which index this daemon owns.
     *
     * A CLI mutation is handed to a reachable daemon rather than performed
     * locally, and "reachable" used to be the whole test. It is not enough:
     * there is one socket per user runtime directory, but a data directory is
     * chosen per invocation by `--data-dir` or `ATLAS_DATA_DIR`, so a caller
     * naming one directory could have its write applied to whichever one the
     * daemon happened to be started with — silently, and with `--data-dir`
     * reported back in neither place.
     *
     * So the daemon says which directory it owns and the caller checks. It is a
     * path, which is bytes, so it is safe-encoded like every other path that
     * reaches a document. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "data_dir",
                                atlas_safe(&ds->safe, ds->ctx->data_dir), err);
    }
    return st;
}

typedef struct status_tally {
    atlas_db *db;
    int64_t repos;
    int64_t watching;
    int64_t degraded;
    int64_t gaps;
} status_tally;

static atlas_status tally(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    status_tally *t = (status_tally *)ud;
    t->repos++;
    atlas_index_state s;
    atlas_index_state_init(&s);
    atlas_status st = atlas_db_index_state_get(t->db, ri->id, &s, err);
    if (st == ATLAS_OK && s.present) {
        if (s.watch_state == ATLAS_WATCH_WATCHING) {
            t->watching++;
        } else if (s.watch_state != ATLAS_WATCH_UNWATCHED) {
            t->degraded++;
        }
        if (s.event_gap) {
            t->gaps++;
        }
    }
    atlas_index_state_free(&s);
    return st;
}

static atlas_status method_status(dispatch_state *ds, const atlas_ipc_request *req,
                                  atlas_err *err) {
    (void)req;
    status_tally t = {ds->db, 0, 0, 0, 0};
    atlas_status st = atlas_db_repo_list(ds->db, tally, &t, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key_str(ds->j, "atlas", ATLAS_VERSION_STRING, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "protocol", (int64_t)ATLAS_IPC_PROTOCOL_VERSION, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "pid", (int64_t)getpid(), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "uptime_ms", monotonic_ms() - ds->ctx->started_at_ms, err);
    }
    if (st == ATLAS_OK) {
        /* The socket path is Atlas' own, derived from XDG_RUNTIME_DIR, but the
         * environment supplied part of it, so it is encoded like any other
         * value that came from outside. */
        st = atlas_json_key_str(ds->j, "socket", atlas_safe(&ds->safe, ds->ctx->socket_path), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "repositories", t.repos, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "watching", t.watching, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "degraded", t.degraded, err);
    }
    if (st == ATLAS_OK) {
        /* The number that decides whether Atlas may call itself current. */
        st = atlas_json_key_int(ds->j, "repositories_with_event_gap", t.gaps, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "watches", atlas_watcher_watch_count(ds->ctx->watcher), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "write_queue_depth",
                                atlas_writer_queue_depth(ds->ctx->writer), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "write_queue_limit", (int64_t)ATLAS_WRITER_QUEUE_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "reconciliations", atlas_writer_passes(ds->ctx->writer),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "workers", (int64_t)atlas_workers_count(ds->ctx->workers),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "primed", atlas_watcher_primed(ds->ctx->watcher), err);
    }
    return st;
}

bool atlas_server_index_current(const atlas_index_state *s, const char **reason_out) {
    /* The one claim a caller actually acts on, computed once here rather than
     * reconstructed by every consumer from the flags. The reason strings are a
     * fixed Atlas vocabulary: they reach a model's context, so they must not be
     * assembled from anything a repository can influence. */
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

atlas_status atlas_server_write_repo_state(dispatch_state *ds, const atlas_repo_info *ri,
                                           atlas_err *err) {
    atlas_index_state s;
    atlas_index_state_init(&s);
    atlas_status st = atlas_db_index_state_get(ds->db, ri->id, &s, err);
    int64_t cursor = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_events_head(ds->db, ri->id, &cursor, err);
    }
    if (st != ATLAS_OK) {
        atlas_index_state_free(&s);
        return st;
    }

    /* `name` is validated on registration and `root_path_text` is already in the
     * safe encoding, so neither is re-encoded — double-encoding would make a
     * path stop round-tripping. `watch_detail` and `last_error` come from git
     * and from the kernel, so they are encoded here. */
    st = atlas_json_key_str(ds->j, "repo", ri->name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "id", ri->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "root", atlas_buf_cstr(&ri->root_path_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation", s.generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "last_complete_generation", s.last_complete_generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "last_sync_seq", s.last_sync_seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "watch_state", atlas_watch_state_name(s.watch_state), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "watched_directories", s.watched_dirs, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "event_gap", s.event_gap, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "pending_full_reconcile", s.pending_full_reconcile, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "last_reconcile_at", s.last_reconcile_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "last_complete_at", s.last_complete_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "event_cursor", cursor, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "watch_detail",
                                atlas_safe(&ds->safe, atlas_buf_cstr(&s.watch_detail)), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "last_error",
                                atlas_safe(&ds->safe, atlas_buf_cstr(&s.last_error)), err);
    }
    if (st == ATLAS_OK) {
        /* The one claim a caller actually acts on, computed in one place rather
         * than reconstructed by every consumer from the flags above. */
        const char *reason = NULL;
        bool current = atlas_server_index_current(&s, &reason);
        st = atlas_json_key_bool(ds->j, "index_current", current, err);
        if (st == ATLAS_OK) {
            /* A fixed Atlas string, never assembled from anything a repository
             * can influence: it reaches a model's context through ai.context. */
            st = atlas_json_key_str_opt(ds->j, "not_current_reason", reason, err);
        }
    }
    atlas_index_state_free(&s);
    return st;
}

static atlas_status list_repo_item(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_server_write_repo_state(ds, ri, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_repo_list(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    (void)req;
    atlas_status st = atlas_json_key(ds->j, "repositories", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_repo_list(ds->db, list_repo_item, ds, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    return st;
}

/* Resolves the `repo` parameter, with the same error text the CLI uses. */
atlas_status atlas_server_require_repo(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_repo_info *out, atlas_err *err) {
    const char *name = NULL;
    if (!atlas_ipc_param_str(req, "repo", &name)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this method needs a \"repo\" string parameter");
    }
    atlas_status st = atlas_db_check_repo_name(name, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool found = false;
    st = atlas_db_repo_get(ds->db, name, out, &found, err);
    if (st == ATLAS_OK && !found) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "no repository named \"%s\" is registered (try: atlas repo list)",
                             atlas_safe(&ds->safe, name));
    }
    return st;
}

static atlas_status method_repo_state(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st == ATLAS_OK) {
        st = atlas_server_write_repo_state(ds, &info, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status method_repo_sync(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    bool full = false;
    (void)atlas_ipc_param_bool(req, "full", &full);

    /* Queued, not performed. A pass can take minutes; performing it here would
     * stall the serve loop for every other client. The sequence number is what
     * the caller waits on instead. */
    int64_t seq = 0;
    /* No named paths: an explicit sync request is about the repository, not
     * about a path somebody observed. An incremental sync therefore relies on
     * the identity check, and `full` is how a caller asks for content
     * verification. */
    st = atlas_writer_submit_reconcile(ds->ctx->writer, info.id, full, false, NULL, 0u, &seq, err);
    atlas_repo_info_free(&info);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_json_key_bool(ds->j, "queued", true, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "sync_seq", seq, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "full", full, err);
    }
    return st;
}

static atlas_status emit_event(const atlas_event_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "cursor", row->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation", row->generation, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", row->kind, err);
    }
    if (st == ATLAS_OK) {
        /* Stored in the safe text encoding already; encoding it again would make
         * it stop decoding back to the original bytes. */
        st = atlas_json_key_str_opt(ds->j, "path", row->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "detail",
                                    row->detail != NULL ? atlas_safe(&ds->safe, row->detail) : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "at", row->created_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_events_since(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }
    int64_t since = 0;
    (void)atlas_ipc_param_int(req, "since", &since);
    if (since < 0) {
        since = 0;
    }
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);

    st = atlas_json_key_str(ds->j, "repo", info.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "events", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    int64_t next = since;
    bool more = false;
    if (st == ATLAS_OK) {
        st = atlas_db_events_since(ds->db, info.id, since, limit, emit_event, ds, &count, &next,
                                   &more, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "cursor", next, err);
    }
    if (st == ATLAS_OK) {
        /* Pagination is explicit: a caller is told there is more rather than
         * being handed a page that silently ends. */
        st = atlas_json_key_bool(ds->j, "more", more, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- dispatch ------------------------------------------------------------ */

static const atlas_method_entry METHODS[] = {
    {"daemon.ping", method_ping},
    {"daemon.status", method_status},
    {"repo.list", method_repo_list},
    {"repo.state", method_repo_state},
    {"repo.sync", method_repo_sync},
    {"events.since", method_events_since},
    /* There is deliberately no daemon.shutdown. Anything local that can open the
     * socket could then disable indexing; systemd owns the lifecycle and SIGTERM
     * already stops the daemon.
     *
     * **A7: and deliberately no `repo.add` or `repo.remove`.** Registering a
     * repository is what makes Atlas treat a directory as one it will read,
     * index and answer questions about; removing one discards that. Both were
     * ordinary RPC methods, which meant anything that could open the socket
     * could decide what Atlas trusts. Registration is now a local CLI operation
     * under the write lock, like backup and restore, so the daemon has to be
     * stopped for the registry to change at all.
     *
     * `repo.resolve` remains, and reports. A caller may still ask whether a
     * path is registered; it may no longer make it so. */
};

atlas_status atlas_server_dispatch(atlas_server_ctx *ctx, const void *payload, size_t len,
                                   atlas_buf *response, atlas_err *err) {
    atlas_ipc_request *req = NULL;
    atlas_err perr;
    atlas_err_init(&perr);
    if (atlas_ipc_request_parse(payload, len, &req, &perr) != ATLAS_OK) {
        /* Malformed JSON is answered, not fatal. The daemon serves other
         * clients and keeps running. */
        return build_error("0", &perr, response, err);
    }

    /* One dispatch, two groups. The A2 methods live in their own translation
     * unit so the serve loop is not buried under them, but they are looked up
     * here rather than by a second dispatcher: two dispatchers is how a method
     * ends up behaving differently depending on which one found it. */
    atlas_method_fn fn = NULL;
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (strcmp(atlas_ipc_request_method(req), METHODS[i].name) == 0) {
            fn = METHODS[i].fn;
            break;
        }
    }
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *ai = atlas_server_ai_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), ai[i].name) == 0) {
                fn = ai[i].fn;
                break;
            }
        }
    }
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *code = atlas_server_code_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), code[i].name) == 0) {
                fn = code[i].fn;
                break;
            }
        }
    }
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *dec = atlas_server_decision_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), dec[i].name) == 0) {
                fn = dec[i].fn;
                break;
            }
        }
    }
    if (fn == NULL) {
        atlas_err merr;
        atlas_err_init(&merr);
        /* The method name was safe-encoded during parsing, so echoing it back
         * cannot carry a control sequence into a terminal. */
        (void)atlas_err_set(&merr, ATLAS_ERR_USAGE, "unknown method \"%s\"",
                            atlas_ipc_request_method(req));
        atlas_status st = build_error(atlas_ipc_request_id(req), &merr, response, err);
        atlas_ipc_request_free(req);
        return st;
    }

    /* A read-only handle per request. Opening it here rather than holding one
     * for the daemon's lifetime means a request always observes a committed
     * snapshot taken at its own start, and a reader can never accumulate an
     * open transaction that pins the WAL. */
    dispatch_state ds;
    memset(&ds, 0, sizeof(ds));
    ds.ctx = ctx;
    atlas_safe_pool_init(&ds.safe);
    atlas_err derr;
    atlas_err_init(&derr);
    atlas_status st = atlas_db_open_readonly(ctx->db_path, &ds.db, &derr);
    if (st != ATLAS_OK) {
        atlas_safe_pool_free(&ds.safe);
        atlas_ipc_request_free(req);
        return build_error(atlas_ipc_request_id(req), &derr, response, err);
    }

    response_builder rb;
    st = rb_open(&rb, err);
    if (st != ATLAS_OK) {
        atlas_db_close(ds.db);
        atlas_safe_pool_free(&ds.safe);
        atlas_ipc_request_free(req);
        return st;
    }
    ds.j = rb.j;

    atlas_err merr;
    atlas_err_init(&merr);
    atlas_status mst = atlas_json_obj_begin(rb.j, &merr);
    if (mst == ATLAS_OK) {
        mst = atlas_json_key_str(rb.j, "id", atlas_ipc_request_id(req), &merr);
    }
    if (mst == ATLAS_OK) {
        mst = atlas_json_key_bool(rb.j, "ok", true, &merr);
    }
    if (mst == ATLAS_OK) {
        mst = atlas_json_key(rb.j, "result", &merr);
    }
    if (mst == ATLAS_OK) {
        mst = atlas_json_obj_begin(rb.j, &merr);
    }
    if (mst == ATLAS_OK) {
        mst = fn(&ds, req, &merr);
    }
    if (mst == ATLAS_OK) {
        mst = atlas_json_obj_end(rb.j, &merr);
    }
    if (mst == ATLAS_OK) {
        mst = atlas_json_obj_end(rb.j, &merr);
    }

    atlas_db_close(ds.db);
    atlas_safe_pool_free(&ds.safe);

    if (mst != ATLAS_OK) {
        /* The partial document is discarded rather than patched: half a success
         * document followed by an error object would be neither. */
        rb_abort(&rb);
        atlas_status st2 = build_error(atlas_ipc_request_id(req), &merr, response, err);
        atlas_ipc_request_free(req);
        return st2;
    }
    st = rb_finish(&rb, response, err);
    atlas_ipc_request_free(req);
    return st;
}

/* --- the serve loop ------------------------------------------------------
 *
 * Non-blocking, with per-connection state.
 *
 * The obvious implementation — poll, then read a whole frame from whichever
 * connection is readable — is wrong in a way that only shows up under a hostile
 * or merely broken peer: a client that sends three bytes of a header and then
 * stops holds the entire loop for the read timeout, and every other client waits
 * behind it. That is a denial of service that any local process can trigger by
 * accident.
 *
 * So each connection carries its own partially-read frame and its own
 * partially-written response, the loop never blocks on any single one of them,
 * and each has a deadline it is dropped at. */

typedef struct client {
    int fd;
    /* Read side: a frame arrives in two stages, header then payload, and either
     * can arrive one byte at a time. */
    unsigned char head[ATLAS_IPC_HEADER_BYTES];
    size_t head_got;
    atlas_buf payload;
    size_t payload_need;
    size_t payload_got;
    bool have_header;
    /* When the frame in flight must be complete by. Set when its first byte
     * arrives, not refreshed per read, so dribbling cannot extend it forever. */
    int64_t read_deadline_ms;
    bool reading;

    /* Write side: a response the peer has not finished reading yet. */
    atlas_buf out;
    size_t out_sent;
    int64_t write_deadline_ms;
} client;

static void client_init(client *c, int fd) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    atlas_buf_init(&c->payload);
    atlas_buf_init(&c->out);
}

static void client_close(client *c) {
    if (c->fd >= 0) {
        (void)close(c->fd);
        c->fd = -1;
    }
    atlas_buf_free(&c->payload);
    atlas_buf_free(&c->out);
}

/* Resets the read state for the next frame on the same connection. */
static void client_frame_done(client *c) {
    c->head_got = 0;
    c->have_header = false;
    c->payload_need = 0;
    c->payload_got = 0;
    c->reading = false;
    atlas_buf_reset(&c->payload);
}

/* Queues a response. Refuses rather than truncates when it would not fit. */
static atlas_status client_queue(client *c, const atlas_buf *response, int64_t now,
                                 atlas_err *err) {
    if (response->len + ATLAS_IPC_HEADER_BYTES > ATLAS_IPC_MAX_CLIENT_BACKLOG_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the response exceeds the client backlog");
    }
    unsigned char head[ATLAS_IPC_HEADER_BYTES];
    atlas_ipc_header_encode(head, (uint32_t)response->len);
    atlas_buf_reset(&c->out);
    c->out_sent = 0;
    atlas_status st = atlas_buf_append(&c->out, head, sizeof(head), err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&c->out, response->data, response->len, err);
    }
    c->write_deadline_ms = now + ATLAS_IPC_WRITE_TIMEOUT_MS;
    return st;
}

/* Sends what the socket will take. Returns false when the connection is dead. */
static bool client_flush(client *c) {
    while (c->out_sent < c->out.len) {
        ssize_t w = send(c->fd, c->out.data + c->out_sent, c->out.len - c->out_sent, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true; /* the peer is slow; come back when it is writable */
            }
            return false;
        }
        c->out_sent += (size_t)w;
    }
    atlas_buf_reset(&c->out);
    c->out_sent = 0;
    return true;
}

/* Reads whatever is available for one client and, when a frame completes,
 * answers it. Returns false when the connection must be dropped. */
static bool client_step_read(atlas_server_ctx *ctx, client *c, int64_t now, atlas_safe_pool *safe) {
    for (;;) {
        if (c->out.len > 0) {
            /* A response is still going out. Not reading until it has drained is
             * the backpressure: it stops a client from pipelining requests
             * faster than it reads answers and growing our memory. */
            return true;
        }

        if (!c->have_header) {
            ssize_t r = recv(c->fd, c->head + c->head_got, sizeof(c->head) - c->head_got, 0);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return (errno == EAGAIN || errno == EWOULDBLOCK);
            }
            if (r == 0) {
                /* A clean close between frames is how a client finishes. A close
                 * part way through one is a truncated frame, and dropping is the
                 * only safe response either way. */
                return false;
            }
            if (!c->reading) {
                c->reading = true;
                c->read_deadline_ms = now + ATLAS_IPC_READ_TIMEOUT_MS;
            }
            c->head_got += (size_t)r;
            if (c->head_got < sizeof(c->head)) {
                return true; /* still arriving */
            }

            atlas_ipc_header h;
            atlas_err herr;
            atlas_err_init(&herr);
            if (atlas_ipc_header_decode(c->head, ATLAS_IPC_MAX_REQUEST_BYTES, &h, &herr) !=
                ATLAS_OK) {
                /* Answered, then dropped: the stream position is no longer
                 * trustworthy, so where the next frame starts is unknown. */
                atlas_buf resp = ATLAS_BUF_INIT;
                atlas_err werr;
                atlas_err_init(&werr);
                if (build_error("0", &herr, &resp, &werr) == ATLAS_OK) {
                    (void)client_queue(c, &resp, now, &werr);
                    (void)client_flush(c);
                }
                atlas_buf_free(&resp);
                return false;
            }
            c->have_header = true;
            c->payload_need = h.length;
            c->payload_got = 0;
            atlas_err perr;
            atlas_err_init(&perr);
            if (atlas_buf_reserve(&c->payload, c->payload_need + 1u, &perr) != ATLAS_OK) {
                return false;
            }
            if (c->payload_need == 0) {
                atlas_err eerr;
                atlas_err_init(&eerr);
                (void)atlas_err_set(&eerr, ATLAS_ERR_USAGE, "empty request payload");
                atlas_buf resp = ATLAS_BUF_INIT;
                atlas_err werr;
                atlas_err_init(&werr);
                if (build_error("0", &eerr, &resp, &werr) == ATLAS_OK) {
                    (void)client_queue(c, &resp, now, &werr);
                    (void)client_flush(c);
                }
                atlas_buf_free(&resp);
                return false;
            }
            continue;
        }

        ssize_t r = recv(c->fd, c->payload.data + c->payload_got, c->payload_need - c->payload_got,
                         0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return (errno == EAGAIN || errno == EWOULDBLOCK);
        }
        if (r == 0) {
            return false; /* truncated frame */
        }
        c->payload_got += (size_t)r;
        if (c->payload_got < c->payload_need) {
            return true;
        }
        c->payload.len = c->payload_need;
        c->payload.data[c->payload.len] = '\0';

        atlas_buf response = ATLAS_BUF_INIT;
        atlas_err derr;
        atlas_err_init(&derr);
        bool ok = (atlas_server_dispatch(ctx, c->payload.data, c->payload.len, &response, &derr) ==
                   ATLAS_OK);
        if (ok && response.len > ATLAS_IPC_MAX_RESPONSE_BYTES) {
            /* Never truncated. A response that does not fit is a structured
             * error about not fitting. */
            atlas_err oerr;
            atlas_err_init(&oerr);
            (void)atlas_err_set(&oerr, ATLAS_ERR_INTERNAL,
                                "the response exceeds the %u byte limit; narrow the request or "
                                "paginate it",
                                (unsigned)ATLAS_IPC_MAX_RESPONSE_BYTES);
            atlas_buf_reset(&response);
            atlas_err werr;
            atlas_err_init(&werr);
            ok = (build_error("0", &oerr, &response, &werr) == ATLAS_OK);
        }
        if (!ok) {
            atlas_daemon_log(ctx->log, "warn", "dropping a client: %s",
                             atlas_safe(safe, atlas_err_msg(&derr)));
            atlas_buf_free(&response);
            return false;
        }
        atlas_err qerr;
        atlas_err_init(&qerr);
        bool queued = (client_queue(c, &response, now, &qerr) == ATLAS_OK);
        atlas_buf_free(&response);
        if (!queued || !client_flush(c)) {
            return false;
        }
        client_frame_done(c);
        /* Loop again: the client may already have pipelined another request. */
    }
}

atlas_status atlas_server_serve(atlas_server_ctx *ctx, int listen_fd, int signal_fd,
                                atomic_bool *stop, atlas_err *err) {
    client clients[ATLAS_IPC_MAX_CLIENTS];
    size_t nclients = 0;
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);

    while (!atomic_load(stop)) {
        struct pollfd pfd[2 + ATLAS_IPC_MAX_CLIENTS];
        nfds_t n = 0;
        pfd[n].fd = listen_fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
        pfd[n].fd = signal_fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        n++;
        for (size_t i = 0; i < nclients; i++) {
            pfd[n].fd = clients[i].fd;
            /* Waiting for writability only while output is pending keeps an idle
             * client from spinning the loop. */
            pfd[n].events = (clients[i].out.len > 0) ? POLLOUT : POLLIN;
            pfd[n].revents = 0;
            n++;
        }

        int rc = poll(pfd, n, 500);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            for (size_t i = 0; i < nclients; i++) {
                client_close(&clients[i]);
            }
            atlas_safe_pool_free(&safe);
            return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno,
                                       "the serve loop's poll failed");
        }
        int64_t now = monotonic_ms();

        if ((pfd[1].revents & POLLIN) != 0) {
            struct signalfd_siginfo si;
            if (read(signal_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                atlas_daemon_log(ctx->log, "info", "received signal %u; shutting down",
                                 (unsigned)si.ssi_signo);
                atomic_store(stop, true);
                break;
            }
        }

        if ((pfd[0].revents & POLLIN) != 0) {
            for (;;) {
                int cfd = -1;
                int64_t peer_pid = 0;
                atlas_err aerr;
                atlas_err_init(&aerr);
                atlas_status ast = atlas_ipc_accept(listen_fd, &cfd, &peer_pid, &aerr);
                if (ast != ATLAS_OK) {
                    /* A refused peer is logged and the loop continues: one
                     * rejected connection must not stop the daemon serving. */
                    atlas_daemon_log(ctx->log, "warn", "%s",
                                     atlas_safe(&safe, atlas_err_msg(&aerr)));
                    continue;
                }
                if (cfd < 0) {
                    break; /* nothing more pending */
                }
                if (nclients >= ATLAS_IPC_MAX_CLIENTS) {
                    /* At the ceiling. Closing immediately is the bounded answer;
                     * queueing would make the limit meaningless. */
                    atlas_daemon_log(ctx->log, "warn",
                                     "refused a client: %u connections are already open",
                                     (unsigned)ATLAS_IPC_MAX_CLIENTS);
                    (void)close(cfd);
                    continue;
                }
                client_init(&clients[nclients], cfd);
                nclients++;
            }
        }

        for (size_t i = 0; i < nclients;) {
            bool alive = true;
            struct pollfd *cp = (i + 2u < n) ? &pfd[2 + i] : NULL;

            if (clients[i].out.len > 0) {
                if (cp != NULL && (cp->revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
                    alive = client_flush(&clients[i]);
                }
                if (alive && clients[i].out.len > 0 && now > clients[i].write_deadline_ms) {
                    /* A client that will not read its answer is dropped rather
                     * than allowed to hold a slot and a buffer indefinitely. */
                    atlas_daemon_log(ctx->log, "warn",
                                     "dropping a client that did not read its response");
                    alive = false;
                }
            } else if (cp != NULL && (cp->revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                alive = client_step_read(ctx, &clients[i], now, &safe);
            }

            /* The slow-client rule: an incomplete frame has a deadline from the
             * moment its first byte arrived, so dribbling bytes cannot extend it
             * and cannot occupy the loop. */
            if (alive && clients[i].reading && now > clients[i].read_deadline_ms) {
                atlas_daemon_log(ctx->log, "warn",
                                 "dropping a client that did not finish sending a frame");
                alive = false;
            }

            if (!alive) {
                client_close(&clients[i]);
                clients[i] = clients[nclients - 1u];
                nclients--;
                continue;
            }
            i++;
        }
    }

    for (size_t i = 0; i < nclients; i++) {
        client_close(&clients[i]);
    }
    atlas_safe_pool_free(&safe);
    return ATLAS_OK;
}

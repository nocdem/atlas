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
 * vocabulary rather than two.
 *
 * A12.0 adds an optional `detail` object, and only one method fills it. See
 * `dispatch_state.has_detail`: a refused planner document is answered with a
 * sentence *and* the line it happened on, and the two travel apart so the plan
 * driver never has to read Atlas' prose to recover a number. `refusal` is NULL
 * for every other refusal in Atlas, which is every refusal whose sentence is the
 * whole answer, and then the document is byte-for-byte what it was before. */
static atlas_status build_error_with_detail(const char *id, const atlas_err *err,
                                            const char *refusal, int line, atlas_buf *out,
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
    if (st == ATLAS_OK && refusal != NULL) {
        st = atlas_json_key(rb.j, "detail", werr);
        if (st == ATLAS_OK) {
            st = atlas_json_obj_begin(rb.j, werr);
        }
        if (st == ATLAS_OK) {
            /* Atlas' own sentence about a planner's bytes. It quotes none of
             * them and is safe-encoded anyway: one discipline for everything
             * that leaves this function. */
            st = atlas_json_key_str(rb.j, "refusal", atlas_safe(&safe, refusal), werr);
        }
        if (st == ATLAS_OK) {
            /* The 1-based line the refusal is about, or 0 for a refusal about the
             * document as a whole. Emitted always, including zero, because an
             * absent key and a zero would otherwise be the same document. */
            st = atlas_json_key_int(rb.j, "line", (int64_t)line, werr);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(rb.j, werr);
        }
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

static atlas_status build_error(const char *id, const atlas_err *err, atlas_buf *out,
                                atlas_err *werr) {
    return build_error_with_detail(id, err, NULL, 0, out, werr);
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
    /* P0. The same live view `atlas_server_write_repo_state` overlays, so the
     * summary and the per-repository documents cannot disagree. Without it a
     * blocked writer produced exactly that disagreement: `watching: 1,
     * priming: 0` beside a repository document reading `priming` and not
     * current, in the same family of commands and about the same instant. */
    atlas_watcher *watcher;
    int64_t repos;
    int64_t watching;
    int64_t degraded;
    int64_t priming;
    int64_t gaps;
} status_tally;

static atlas_status tally(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    status_tally *t = (status_tally *)ud;
    t->repos++;
    atlas_index_state s;
    atlas_index_state_init(&s);
    atlas_status st = atlas_db_index_state_get(t->db, ri->id, &s, err);
    if (st == ATLAS_OK && s.present) {
        atlas_watch_live live;
        atlas_watcher_repo_live(t->watcher, ri->id, &live);
        atlas_server_overlay_live(&s, &live);
        if (s.watch_state == ATLAS_WATCH_WATCHING) {
            t->watching++;
        } else if (s.watch_state == ATLAS_WATCH_PRIMING) {
            /* P0. Counted on its own. Adding it to `degraded` would make an
             * ordinary startup look like a fault, and adding it to `watching`
             * would let `watching == repositories` be true while part of a tree
             * was still unobserved. */
            t->priming++;
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
    status_tally t = {ds->db, ds->ctx != NULL ? ds->ctx->watcher : NULL, 0, 0, 0, 0, 0};
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
        /* Repositories whose *stored row* records an event gap. It is not on
         * its own the number that decides whether Atlas may call itself
         * current: `priming` and `degraded` beside it, and
         * `watch_owed_gaps` below, each also make a repository not current, and
         * the per-repository document's `index_current` is the claim a caller
         * acts on. */
        st = atlas_json_key_int(ds->j, "repositories_with_event_gap", t.gaps, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "priming", t.priming, err);
    }
    if (st == ATLAS_OK) {
        /* P0. `watches` keeps its meaning exactly: physical inotify descriptors,
         * which is what the kernel holds and what it counts against
         * `fs.inotify.max_user_watches`.
         *
         * `watch_subscriptions` is the new number, and it is deliberately not
         * the same one. Two registered worktrees of a repository subscribe to
         * the same descriptor on their shared git directory, so the
         * per-repository figures sum to the subscription count and **exceed**
         * the descriptor count. Any surface that treated them as one number
         * would be wrong for exactly the case worktrees create. */
        atlas_watch_stats ws;
        atlas_watcher_stats(ds->ctx->watcher, &ws);
        st = atlas_json_key_int(ds->j, "watches", ws.watches, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "watch_subscriptions", ws.subscriptions, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "watch_budget_total", ws.budget_total, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "watch_budget_repo", ws.budget_repo, err);
        }
        if (st == ATLAS_OK) {
            /* The kernel's own number, reported beside Atlas' budget so an
             * operator can see which of the two is binding rather than guess. */
            st = atlas_json_key_int(ds->j, "kernel_max_user_watches", ws.kernel_max, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "watch_budget_source",
                                    ws.budget_from_policy ? "policy" : "kernel", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "priming_complete", ws.priming_complete, err);
        }
        if (st == ATLAS_OK) {
            /* P0. Obligations the watcher holds and has not yet seen recorded.
             *
             * `repositories_with_event_gap` counts stored rows, so a repository
             * whose publication is still on the writer's queue contributes to
             * neither it nor `degraded` — and while the writer is held by an
             * unbounded job that is every obligation the watcher has. This is
             * the summary counterpart of the per-repository overlay: the number
             * is the watcher's own, costs one mutex, and does not wait for the
             * writer. */
            st = atlas_json_key_int(ds->j, "watch_owed_gaps", ws.owed_gaps, err);
        }
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
    /* A9.2.2 moved the rule itself to `atlas_index_state_is_current` in
     * `src/db/db_state.c`, because A9.2.2's coverage model has to ask the same
     * question from `src/verify` and a second copy of a currency rule is how two
     * surfaces start disagreeing about whether the index is current. This stays
     * as the name the A2 serve loop has always called. */
    return atlas_index_state_is_current(s, reason_out);
}

/* P0. Overlay the watcher's live view onto the row it has not managed to write
 * yet.
 *
 * Publishing a watch state is asynchronous: `SET_WATCH` goes on the single
 * writer's queue, and A9.2.6 documents that an unbounded semantic pass can own
 * that thread for minutes. Enqueueing is not persistence. Without this, a
 * repository that started priming — or that has decided it owes a full
 * content-verifying pass — is reported from the previous row, which says
 * `watching` and `index_current: true`, for as long as the writer is busy.
 * Atlas would be claiming it observed every change during exactly the window in
 * which it knows it did not.
 *
 * The overlay is applied to the *state struct*, before the currency rule is
 * asked, rather than to the boolean afterwards. That is deliberate: A9.2.2 made
 * `atlas_index_state_is_current` the one implementation of "is the index
 * current?", and a second condition applied to its output would be a second copy
 * of the rule. Adjusting its input keeps one rule, and keeps the reported
 * `watch_state` and `pending_full_reconcile` agreeing with the `index_current`
 * derived from them instead of contradicting it.
 *
 * It can only ever make Atlas *less* confident. Nothing here clears a gap,
 * clears an owed pass, or promotes a state — a stored ERROR or INCOMPLETE stays
 * what it is whatever the watcher currently believes — so the worst a stale live
 * view can do is force an unnecessary full pass, which is the direction every
 * other watcher rule already fails in.
 *
 * Non-blocking by construction: the live view costs one short mutex in the
 * watcher, no writer involvement and no I/O, so a status read stays answerable
 * while the writer thread is held. */
void atlas_server_overlay_live(atlas_index_state *s, const atlas_watch_live *live) {
    if (!live->known) {
        return;
    }
    if (live->owes_gap) {
        s->pending_full_reconcile = true;
    }
    if (s->watch_state == ATLAS_WATCH_WATCHING || s->watch_state == ATLAS_WATCH_UNWATCHED) {
        if (live->degraded) {
            s->watch_state = ATLAS_WATCH_DEGRADED;
        } else if (live->priming) {
            s->watch_state = ATLAS_WATCH_PRIMING;
        }
    }
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

    /* P0. Downgrade the stored row to what the watcher believes right now. */
    if (ds->ctx != NULL) {
        atlas_watch_live live;
        atlas_watcher_repo_live(ds->ctx->watcher, ri->id, &live);
        atlas_server_overlay_live(&s, &live);
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
        /* P0. Still this repository's own count, and still the same field — but
         * it is now the number of *subscriptions* it holds, counted once per
         * (repository, descriptor) pair and written on every path including the
         * degraded one. Before P0 it double-counted a descriptor shared with
         * another worktree and was not written at all when a repository
         * degraded, so it reported whatever had been true the last time things
         * went well. The split below says where the number comes from. */
        st = atlas_json_key_int(ds->j, "watched_directories", s.watched_dirs, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "watched_source", s.watched_source, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "watched_meta", s.watched_meta, err);
    }
    if (st == ATLAS_OK) {
        /* How many of the above are on a descriptor another repository also
         * subscribes to. Non-zero is why the per-repository counts can sum to
         * more than the daemon's physical `watches`. */
        st = atlas_json_key_int(ds->j, "watched_shared", s.watched_shared, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "watch_reason", atlas_watch_reason_name(s.watch_reason),
                                err);
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

/* The registration facts a listing shows, beside the index state.
 *
 * Added so that `atlas repo list` can be answered over the socket by a client
 * that cannot open the index — which under A7.1 is every client, because the
 * data directory is 0700 `atlasd` and must stay so. Purely additive: no key
 * changes meaning and none is removed, so an older reader is unaffected.
 *
 * `name` is validated at registration and `root_path_text` is already in the
 * safe encoding, so neither is re-encoded. The branch comes from git and is
 * encoded here. */
static atlas_status write_repo_registration(dispatch_state *ds, const atlas_repo_info *ri,
                                            atlas_err *err) {
    atlas_status st = atlas_json_key_str(ds->j, "last_scan_at", ri->last_scan_at, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "scanned_head", ri->scanned_head, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "branch", atlas_safe(&ds->safe, ri->current_branch), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "head_state", ri->head_state, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "object_format", ri->object_format, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "registered_at", ri->registered_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "dirty", ri->dirty, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "linked_worktree", ri->is_linked_worktree, err);
    }
    /* Both git directories are raw bytes from the database, so they go on the
     * wire in the reversible `%XX` form the transport can carry and the client
     * decodes them back to bytes. A path is bytes and may not be UTF-8. */
    atlas_buf enc = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_path_text_encode(ri->git_common_dir.data, ri->git_common_dir.len, &enc, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "git_common_dir", atlas_buf_cstr(&enc), err);
    }
    if (st == ATLAS_OK) {
        /* Reset first: the encoder appends, and this buffer just carried the
         * common directory. Without it `git_dir` goes on the wire as the two
         * paths concatenated, which is a value that decodes cleanly, looks like
         * a path, and matches nothing. */
        atlas_buf_reset(&enc);
        st = atlas_path_text_encode(ri->git_dir.data, ri->git_dir.len, &enc, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "git_dir", atlas_buf_cstr(&enc), err);
    }
    atlas_buf_free(&enc);
    return st;
}

/* The index-side facts `atlas status NAME` reports, so that command can be
 * answered over the socket by a client that cannot open the index. Additive.
 *
 * The live git observation is deliberately *not* here: it is taken fresh at
 * query time, by the client, against a repository the client can read. A
 * daemon-supplied "live" state would be live as of whenever the daemon looked,
 * and head drift is the one thing that report exists to show. */
static atlas_status write_repo_status_facts(dispatch_state *ds, const atlas_repo_info *ri,
                                            atlas_err *err) {
    atlas_repo_counts c;
    memset(&c, 0, sizeof c);
    atlas_status st = atlas_db_repo_counts(ds->db, ri->id, &c, err);
    int64_t siblings = 0;
    if (st == ATLAS_OK) {
        st = atlas_db_repo_siblings(ds->db, ri->id, ri->git_common_dir.data,
                                    ri->git_common_dir.len, NULL, NULL, &siblings, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    const struct {
        const char *k;
        int64_t v;
    } ints[] = {
        {"files_live", c.files_live},
        {"files_deleted", c.files_deleted},
        {"commits", c.commits},
        {"changes", c.changes},
        {"scans", c.scans},
        {"evidence", c.evidence},
        {"compile_databases", c.compile_databases},
        {"sibling_worktrees", siblings},
        {"last_scan_id", ri->last_scan_id},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof ints / sizeof ints[0]; i++) {
        st = atlas_json_key_int(ds->j, ints[i].k, ints[i].v, err);
    }
    return st;
}

static atlas_status list_repo_item(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_server_write_repo_state(ds, ri, err);
    }
    if (st == ATLAS_OK) {
        st = write_repo_registration(ds, ri, err);
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
                             "NOT_REGISTERED: no repository named \"%s\" is registered. Repositories are onboarded only by an operator; Atlas does not discover them (try: atlas repo list)",
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
    if (st == ATLAS_OK) {
        st = write_repo_registration(ds, &info, err);
    }
    if (st == ATLAS_OK) {
        st = write_repo_status_facts(ds, &info, err);
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

/* --- repo.file / repo.history ------------------------------------------------
 *
 * Both call the one implementation in `src/core/service.c` rather than
 * re-deriving anything, so the answer this daemon gives and the answer a local
 * CLI gives come from the same code. `path` arrives in the reversible `%XX`
 * form every Atlas path input accepts and is passed through as text, exactly as
 * the CLI passes it: the service layer is what tries the raw and decoded
 * spellings, and doing it here as well would be a second place for that rule to
 * live. */
static atlas_status emit_file_report(const atlas_file_report *rep, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    const atlas_file_row *row = &rep->row;
    /* `path_text` is stored in the safe encoding already; encoding it again
     * would make it stop decoding back to the original bytes. A subject comes
     * raw from git and is encoded here. */
    atlas_status st = atlas_json_key_str(ds->j, "path", row->path_text, err);
    const struct {
        const char *k;
        const char *v;
    } strs[] = {
        {"file_type", row->file_type},
        {"language", row->language},
        {"git_mode", row->git_mode},
        {"git_index_oid", row->git_index_oid},
        {"content_hash", row->content_hash},
        {"content_hash_algo", row->content_hash_algo},
        {"read_error", row->read_error},
        {"truncated_reason", row->truncated_reason},
        {"reason", rep->reason},
        {"reason_evidence", rep->reason_evidence},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof strs / sizeof strs[0]; i++) {
        st = atlas_json_key_str_opt(ds->j, strs[i].k, strs[i].v, err);
    }
    const struct {
        const char *k;
        bool v;
    } bools[] = {
        {"path_is_utf8", row->path_is_utf8},   {"size_known", row->size_known},
        {"is_executable", row->is_executable}, {"is_symlink", row->is_symlink},
        {"unsafe_path", row->unsafe_path},     {"deleted", row->deleted},
        {"tracked", row->tracked},             {"ignored", row->ignored},
        {"truncated", row->truncated},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof bools / sizeof bools[0]; i++) {
        st = atlas_json_key_bool(ds->j, bools[i].k, bools[i].v, err);
    }
    const struct {
        const char *k;
        int64_t v;
    } ints[] = {
        {"size_bytes", row->size_bytes},
        {"last_generation", row->last_generation},
        {"first_seen_scan_id", row->first_seen_scan_id},
        {"last_seen_scan_id", row->last_seen_scan_id},
        {"change_count", rep->change_count},
        {"last_commit_time", rep->last_commit_time},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof ints / sizeof ints[0]; i++) {
        st = atlas_json_key_int(ds->j, ints[i].k, ints[i].v, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "last_commit", rep->last_commit_oid, err);
    }
    if (st == ATLAS_OK && rep->last_commit_subject != NULL) {
        st = atlas_json_key_str(ds->j, "last_commit_subject",
                                atlas_safe(&ds->safe, rep->last_commit_subject), err);
    }
    return st;
}

static atlas_status method_repo_file(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    const char *path = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "path", &path)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "repo.file needs a \"path\" parameter");
    }
    if (st == ATLAS_OK && info.last_scan_id == 0) {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "repository \"%s\" has not been scanned yet (run: atlas scan %s)",
                           info.name, info.name);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_service_file_db(ds->db, info.id, info.name, path, emit_file_report, ds, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status emit_history_row(const atlas_history_row *row, void *ud, atlas_err *err) {
    dispatch_state *ds = (dispatch_state *)ud;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "commit", row->commit_oid, err);
    }
    /* Author identity and subject come raw from git and are encoded here; both
     * path forms are stored encoded and are emitted as they are. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "author", row->author_name != NULL ? atlas_safe(&ds->safe, row->author_name) : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "author_email",
            row->author_email != NULL ? atlas_safe(&ds->safe, row->author_email) : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "subject", row->subject != NULL ? atlas_safe(&ds->safe, row->subject) : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "change_type", row->change_type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "path", row->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "old_path", row->old_path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "author_time", row->author_time, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "commit_time", row->commit_time, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "score_known", row->score_known, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "score", row->score, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "untrusted_data", true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_repo_history(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    const char *path = NULL;
    if (st == ATLAS_OK && !atlas_ipc_param_str(req, "path", &path)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "repo.history needs a \"path\" parameter");
    }
    if (st == ATLAS_OK && info.last_scan_id == 0) {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "repository \"%s\" has not been scanned yet (run: atlas scan %s)",
                           info.name, info.name);
    }
    int64_t limit = 0;
    (void)atlas_ipc_param_int(req, "limit", &limit);
    if (limit <= 0 || limit > ATLAS_IPC_MAX_ROWS) {
        limit = ATLAS_IPC_MAX_ROWS;
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "changes", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t count = 0;
    if (st == ATLAS_OK) {
        st = atlas_service_history_db(ds->db, info.id, path, limit, emit_history_row, ds, &count,
                                      err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "count", count, err);
    }
    atlas_repo_info_free(&info);
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
    {"repo.file", method_repo_file},
    {"repo.history", method_repo_history},
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
                                   int64_t peer_uid, int64_t peer_pid, atlas_buf *response,
                                   atlas_err *err) {
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
        /* A8-CI. Four reads, in the ordinary group: a semantic query needs no
         * more authority than a structural one, and index construction is not
         * here at all. */
        size_t n = 0;
        const atlas_method_entry *sem = atlas_server_sem_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), sem[i].name) == 0) {
                fn = sem[i].fn;
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
        /* A9.2.1. Intake, evaluation and three reads, in the ordinary group.
         *
         * Creating a claim, referencing evidence and attesting are proposals,
         * not authority — the shape `decision.propose` has. `verify.evaluate`
         * is here too and that is the deliberate part: §17 says a model may
         * *request* an evaluation and that Atlas, not the caller, performs any
         * transition the root-owned policy authorises. What keeps that safe is
         * not this table — it is that a deterministic verdict requires an
         * Atlas-attested verifier a model cannot forge, that the empirical path
         * is shadow-only without calibration nothing here can supply, and that
         * every gate is set by a root-owned file this peer cannot read.
         *
         * No method in this group approves, rejects, supersedes, resolves or
         * revalidates, and none mints or spends a warrant. Those are absent
         * rather than refused. */
        size_t n = 0;
        const atlas_method_entry *v = atlas_server_verify_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), v[i].name) == 0) {
                fn = v[i].fn;
                break;
            }
        }
    }
    /* A8: two groups, routed by the peer's uid from SO_PEERCRED.
     *
     * A peer is offered a group only if the policy places it in one, and the
     * names never overlap — `job.` versus `dispatch.` — so a uid that is *both*
     * a submitter and a dispatcher gets both sets rather than losing one. A8.1
     * made that case real: the operator's account submits jobs *and* runs the
     * model dispatcher, and an either/or lookup silently took `job.submit`
     * away from it.
     *
     * Routing is not authorisation. Each method still calls `require_submitter`
     * or `require_dispatcher` for itself, so reaching a name is never the same
     * as being allowed to use it.
     *
     * The two groups are hidden differently, on purpose. The **dispatcher**
     * group is offered only to the uid a root-owned policy names: a name it
     * does not hold answers `unknown method`, the same as a name that does not
     * exist, because a refusal distinguishing "you may not" from "there is no
     * such thing" would tell a caller what to try next. The **client** group is
     * always dispatchable by name and refuses with "orchestration is not
     * enabled" — a submitter learning that this daemon runs no jobs learns
     * nothing it could not learn by reading the root-owned policy path, and the
     * honest answer is what lets an operator tell a disabled policy apart from
     * a binary too old to have the method. */
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *g = atlas_server_orch_client_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), g[i].name) == 0) {
                fn = g[i].fn;
                break;
            }
        }
    }
    /* The operator group, offered only to the peer the root-owned authority
     * policy names. Consulted additively like the two orchestration groups, and
     * hidden the same way: a peer the policy does not name gets `unknown
     * method` for these five names, which is what a name that does not exist
     * gets. The identity is `SO_PEERCRED`; nothing in the request body reaches
     * this decision. */
    if (fn == NULL && atlas_server_peer_is_operator((long long)peer_uid)) {
        size_t n = 0;
        const atlas_method_entry *g = atlas_server_operator_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), g[i].name) == 0) {
                fn = g[i].fn;
                break;
            }
        }
        /* Backup create and verify, behind the same test and consulted
         * additively for the same reason the orchestration groups are: two
         * tables, one gate, so neither can be reached without the other's
         * check having run. */
        if (fn == NULL) {
            const atlas_method_entry *b = atlas_server_backup_methods(&n);
            for (size_t i = 0; i < n; i++) {
                if (strcmp(atlas_ipc_request_method(req), b[i].name) == 0) {
                    fn = b[i].fn;
                    break;
                }
            }
        }

    }
    /* A9's credential methods. Dispatchable by name; each one asks
     * `atlas_server_peer_may_administer_credentials` for itself and answers
     * `unknown method` when the answer is no. Consulted here rather than inside
     * the operator block because the gate is not the same one: see
     * src/ipc/server_apikey.c. */
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *k = atlas_server_apikey_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), k[i].name) == 0) {
                fn = k[i].fn;
                break;
            }
        }
    }
    /* A9. The gateway group, offered only to the uid a root-owned gateway
     * policy names, and consulted additively like the orchestration and
     * operator groups. A daemon with no gateway policy has `gateway_uid` at
     * zero, and zero matches no peer, so this group is offered to nobody. */
    if (fn == NULL && atlas_server_peer_is_gateway(ctx, (long long)peer_uid)) {
        size_t n = 0;
        const atlas_method_entry *g = atlas_server_gateway_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), g[i].name) == 0) {
                fn = g[i].fn;
                break;
            }
        }
    }
    if (fn == NULL &&
        atlas_orchpolicy_is_any_dispatcher(&ctx->orchpolicy, (long long)peer_uid)) {
        size_t n = 0;
        const atlas_method_entry *g = atlas_server_orch_dispatch_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), g[i].name) == 0) {
                fn = g[i].fn;
                break;
            }
        }
    }
    /* A13. The scanner group, consulted with no uid predicate.
     *
     * Every group above is offered only to a peer some root-owned policy names,
     * and each of those policies is in memory here. A scanner's identity is
     * not: it is `repositories.scanner_uid`, and the database is opened below,
     * after this lookup. So the name is dispatchable and `require_scanner`
     * makes the real check where the database exists — the orchestration client
     * group's arrangement, and the same reasoning as the comment above it. */
    if (fn == NULL) {
        size_t n = 0;
        const atlas_method_entry *g = atlas_server_scanner_methods(&n);
        for (size_t i = 0; i < n; i++) {
            if (strcmp(atlas_ipc_request_method(req), g[i].name) == 0) {
                fn = g[i].fn;
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
    ds.peer_uid = peer_uid;
    ds.peer_pid = peer_pid;
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
         * document followed by an error object would be neither.
         *
         * A12.0. Whatever typed detail the method left behind travels with it.
         * `has_detail` is false for every method that did not set one, and then
         * this is the call it always was. */
        rb_abort(&rb);
        atlas_status st2 = build_error_with_detail(atlas_ipc_request_id(req), &merr,
                                                   ds.has_detail ? ds.detail_refusal : NULL,
                                                   ds.detail_line, response, err);
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
    /* The kernel's answer about this peer, recorded once at accept time and
     * never re-derived. A8 selects a method group on it — the dispatcher's
     * orchestration methods are reachable only from the uid a root-owned policy
     * names — so it must come from SO_PEERCRED and from nowhere else. Re-reading
     * it from /proc, or believing a uid in a request body, would put the choice
     * of method group in the caller's hands. */
    int64_t peer_uid;
    int64_t peer_pid;
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

static void client_init(client *c, int fd, int64_t peer_uid, int64_t peer_pid) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->peer_uid = peer_uid;
    c->peer_pid = peer_pid;
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
        bool ok = (atlas_server_dispatch(ctx, c->payload.data, c->payload.len, c->peer_uid,
                                         c->peer_pid, &response, &derr) ==
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
                int64_t peer_uid = 0;
                atlas_err aerr;
                atlas_err_init(&aerr);
                atlas_status ast = atlas_ipc_accept(listen_fd, &ctx->syspolicy, &cfd, &peer_pid,
                                                    &peer_uid, &aerr);
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
                client_init(&clients[nclients], cfd, peer_uid, peer_pid);
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

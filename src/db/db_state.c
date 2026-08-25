/* Atlas - A1 continuous-index state: generations, events, tips, daemon record.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These are the tables that let Atlas answer "is this current?" honestly. The
 * rule the whole file exists to serve: a reader is only ever shown
 * `last_complete_generation`, and while `event_gap` is set nothing may describe
 * the index as current.
 */
#include "db/db_internal.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

/* --- watch state --------------------------------------------------------- */

const char *atlas_watch_state_name(atlas_watch_state s) {
    switch (s) {
    case ATLAS_WATCH_UNWATCHED: return "unwatched";
    case ATLAS_WATCH_WATCHING: return "watching";
    case ATLAS_WATCH_DEGRADED: return "degraded";
    case ATLAS_WATCH_INCOMPLETE: return "incomplete";
    case ATLAS_WATCH_ERROR: return "error";
    case ATLAS_WATCH_PRIMING: return "priming";
    }
    return "unwatched";
}

atlas_watch_state atlas_watch_state_parse(const char *name) {
    if (name == NULL) {
        return ATLAS_WATCH_UNWATCHED;
    }
    if (strcmp(name, "watching") == 0) {
        return ATLAS_WATCH_WATCHING;
    }
    if (strcmp(name, "degraded") == 0) {
        return ATLAS_WATCH_DEGRADED;
    }
    if (strcmp(name, "incomplete") == 0) {
        return ATLAS_WATCH_INCOMPLETE;
    }
    if (strcmp(name, "error") == 0) {
        return ATLAS_WATCH_ERROR;
    }
    if (strcmp(name, "priming") == 0) {
        return ATLAS_WATCH_PRIMING;
    }
    return ATLAS_WATCH_UNWATCHED;
}

/* P0. The watch reason vocabulary.
 *
 * One table, three functions over it, and the names are the stored spelling —
 * they are what migration 26's CHECK lists, so a member added here and not
 * there is refused by the database rather than written and forgotten. The
 * switches have no `default:`, so adding a member is a compile error at every
 * one of them; that is the half the compiler can find, and `docs/extending.md`
 * carries the half it cannot. */
const char *atlas_watch_reason_name(atlas_watch_reason r) {
    switch (r) {
    case ATLAS_WATCH_REASON_UNKNOWN: return "unknown";
    case ATLAS_WATCH_REASON_NONE: return "none";
    case ATLAS_WATCH_REASON_KERNEL_LIMIT: return "kernel_limit";
    case ATLAS_WATCH_REASON_REPO_BUDGET: return "repo_budget";
    case ATLAS_WATCH_REASON_TOTAL_BUDGET: return "total_budget";
    case ATLAS_WATCH_REASON_META_BUDGET: return "meta_budget";
    case ATLAS_WATCH_REASON_DISCOVERY_BOUND: return "discovery_bound";
    case ATLAS_WATCH_REASON_IGNORE_OVERFLOW: return "ignore_overflow";
    case ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW: return "pending_ignore_overflow";
    case ATLAS_WATCH_REASON_FRONTIER_OVERFLOW: return "frontier_overflow";
    case ATLAS_WATCH_REASON_REPO_LIMIT: return "repo_limit";
    case ATLAS_WATCH_REASON_ERROR: return "error";
    }
    return "unknown";
}

atlas_watch_reason atlas_watch_reason_parse(const char *name) {
    if (name == NULL) {
        return ATLAS_WATCH_REASON_UNKNOWN;
    }
    static const struct {
        const char *name;
        atlas_watch_reason reason;
    } TABLE[] = {
        {"none", ATLAS_WATCH_REASON_NONE},
        {"kernel_limit", ATLAS_WATCH_REASON_KERNEL_LIMIT},
        {"repo_budget", ATLAS_WATCH_REASON_REPO_BUDGET},
        {"total_budget", ATLAS_WATCH_REASON_TOTAL_BUDGET},
        {"meta_budget", ATLAS_WATCH_REASON_META_BUDGET},
        {"discovery_bound", ATLAS_WATCH_REASON_DISCOVERY_BOUND},
        {"ignore_overflow", ATLAS_WATCH_REASON_IGNORE_OVERFLOW},
        {"pending_ignore_overflow", ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW},
        {"frontier_overflow", ATLAS_WATCH_REASON_FRONTIER_OVERFLOW},
        {"repo_limit", ATLAS_WATCH_REASON_REPO_LIMIT},
        {"error", ATLAS_WATCH_REASON_ERROR},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            return TABLE[i].reason;
        }
    }
    /* An unrecognised spelling reads as UNKNOWN, never as the nearest match. A
     * newer daemon's reason arriving at an older client must not be silently
     * rendered as a different one — A9.2.5's rule for the remote parser, and the
     * conservative value is the one that claims least. */
    return ATLAS_WATCH_REASON_UNKNOWN;
}

const char *atlas_watch_reason_explain(atlas_watch_reason r) {
    switch (r) {
    case ATLAS_WATCH_REASON_UNKNOWN:
        return "no reason was recorded for this repository's watch state";
    case ATLAS_WATCH_REASON_NONE: return "the watch set is complete";
    case ATLAS_WATCH_REASON_KERNEL_LIMIT:
        /* The one reason whose remedy is not an Atlas setting, so it is the one
         * message that must not send an operator to an Atlas setting. */
        return "the kernel refused another inotify watch for this user; raise "
               "fs.inotify.max_user_watches, or expect the unwatched parts to be covered only by "
               "periodic reconciliation";
    case ATLAS_WATCH_REASON_REPO_BUDGET:
        return "this repository reached its share of the daemon's watch budget for this round";
    case ATLAS_WATCH_REASON_TOTAL_BUDGET:
        return "the daemon's total watch budget is spent across every repository it observes";
    case ATLAS_WATCH_REASON_META_BUDGET:
        return "this repository needs more git metadata watches than Atlas will install";
    case ATLAS_WATCH_REASON_DISCOVERY_BOUND:
        return "the watch installer reached its bound on directories visited in one pass";
    case ATLAS_WATCH_REASON_IGNORE_OVERFLOW:
        return "this repository reports more ignored directories than Atlas will hold, so the "
               "surplus cannot be distinguished from directories that should be watched";
    case ATLAS_WATCH_REASON_PENDING_IGNORE_OVERFLOW:
        return "more directories appeared at once than Atlas will hold while it decides whether "
               "git ignores them";
    case ATLAS_WATCH_REASON_FRONTIER_OVERFLOW:
        return "a directory in this repository has more entries than the watch installer will "
               "hold pending at once";
    case ATLAS_WATCH_REASON_REPO_LIMIT:
        return "more repositories are registered than this watcher will observe";
    case ATLAS_WATCH_REASON_ERROR:
        return "the watch installer failed for this repository";
    }
    return "no reason was recorded for this repository's watch state";
}

void atlas_watch_outcome_init(atlas_watch_outcome *o) {
    memset(o, 0, sizeof(*o));
    /* Both zeros are the conservative value: unwatched, and no reason stated. */
    o->state = ATLAS_WATCH_UNWATCHED;
    o->reason = ATLAS_WATCH_REASON_UNKNOWN;
}

void atlas_index_state_init(atlas_index_state *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->watch_detail);
    atlas_buf_init(&s->last_error);
}

void atlas_index_state_free(atlas_index_state *s) {
    if (s == NULL) {
        return;
    }
    atlas_buf_free(&s->watch_detail);
    atlas_buf_free(&s->last_error);
}

atlas_status atlas_db_index_state_ensure(atlas_db *db, int64_t repo_id, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "INSERT INTO repo_index_state(repo_id) VALUES(?1) ON CONFLICT(repo_id) DO NOTHING;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    return atlas_db_step_done(db, stmt, err);
}

bool atlas_index_state_is_current(const atlas_index_state *s, const char **reason_out) {
    /* The one claim a caller actually acts on, computed once here rather than
     * reconstructed by every consumer from the flags. The reason strings are a
     * fixed Atlas vocabulary: they reach a model's context, so they must not be
     * assembled from anything a repository can influence.
     *
     * A9.2.2 moved this out of `src/ipc/server.c`, where it lived as
     * `atlas_server_index_current`, because the coverage model has to ask the
     * same question from `src/verify` when it decides
     * `ATLAS_COVDIM_REPOSITORY_SNAPSHOT`. Two copies of a currency rule is how
     * the serve loop and the verifier start disagreeing about whether Atlas is
     * looking at the working tree — and a verifier that believed a stale
     * snapshot was current would report "the bytes differ" for bytes it never
     * read. */
    const char *reason = NULL;
    bool current = false;
    if (!s->present || s->last_complete_generation <= 0) {
        reason = "no reconciliation pass has completed for this repository yet";
    } else if (s->event_gap) {
        reason = "an unresolved event gap means Atlas cannot prove it observed every change";
    } else if (s->pending_full_reconcile) {
        reason = "a full content verification is owed and has not completed";
    } else if (s->watch_state == ATLAS_WATCH_ERROR) {
        reason = "the filesystem watcher failed and is not observing this repository";
    } else if (s->watch_state == ATLAS_WATCH_DEGRADED) {
        reason = "the filesystem watcher is running with a known blind spot";
    } else if (s->watch_state == ATLAS_WATCH_PRIMING) {
        /* P0. A watch set that is still being installed is a tree Atlas is not
         * yet observing all of, and the parts it has not reached are producing
         * events nobody is receiving. That is the same claim an event gap makes,
         * so it gets the same answer.
         *
         * It is listed after the gap and the owed-pass checks deliberately: a
         * repository that is priming *and* owes a full pass should be told about
         * the pass, which is the condition an operator can act on and the one
         * that has to clear before anything is current again. */
        reason = "the filesystem watcher has not finished installing this repository's watches";
    } else {
        current = true;
    }
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    return current;
}

atlas_status atlas_db_index_state_get(atlas_db *db, int64_t repo_id, atlas_index_state *out,
                                      atlas_err *err) {
    out->repo_id = repo_id;
    out->present = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT generation, last_complete_generation,"
                                       " last_reconcile_at, last_complete_at, watch_state,"
                                       " watch_detail, watched_dirs, event_gap,"
                                       " pending_full_reconcile, last_error, last_sync_seq,"
                                       " watched_source, watched_meta, watched_shared,"
                                       " watch_reason"
                                       " FROM repo_index_state WHERE repo_id=?1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->present = true;
        out->generation = sqlite3_column_int64(stmt, 0);
        out->last_complete_generation = sqlite3_column_int64(stmt, 1);
        st = atlas_db_col_copy(stmt, 2, out->last_reconcile_at, sizeof(out->last_reconcile_at),
                               "last_reconcile_at", err);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(stmt, 3, out->last_complete_at, sizeof(out->last_complete_at),
                                   "last_complete_at", err);
        }
        if (st == ATLAS_OK) {
            out->watch_state = atlas_watch_state_parse(atlas_db_col_text(stmt, 4));
            st = atlas_buf_set_str(&out->watch_detail, atlas_db_col_text(stmt, 5), err);
        }
        if (st == ATLAS_OK) {
            out->watched_dirs = sqlite3_column_int64(stmt, 6);
            out->event_gap = sqlite3_column_int(stmt, 7) != 0;
            out->pending_full_reconcile = sqlite3_column_int(stmt, 8) != 0;
            st = atlas_buf_set_str(&out->last_error, atlas_db_col_text(stmt, 9), err);
        }
        if (st == ATLAS_OK) {
            out->last_sync_seq = sqlite3_column_int64(stmt, 10);
            out->watched_source = sqlite3_column_int64(stmt, 11);
            out->watched_meta = sqlite3_column_int64(stmt, 12);
            out->watched_shared = sqlite3_column_int64(stmt, 13);
            out->watch_reason = atlas_watch_reason_parse(atlas_db_col_text(stmt, 14));
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read index state");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_generation_begin(atlas_db *db, int64_t repo_id, int64_t *generation_out,
                                       atlas_err *err) {
    *generation_out = 0;
    atlas_status st = atlas_db_index_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));

    sqlite3_stmt *stmt = NULL;
    /* The claimed generation is always strictly greater than both the in-flight
     * and the last complete one, so a generation number is never reused even if
     * a previous pass was abandoned. */
    st = atlas_db_prepare(db,
                          "UPDATE repo_index_state SET"
                          " generation = max(generation, last_complete_generation) + 1,"
                          " last_reconcile_at = ?2 WHERE repo_id = ?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }

    sqlite3_stmt *sel = NULL;
    st = atlas_db_prepare(db, "SELECT generation FROM repo_index_state WHERE repo_id=?1;", &sel,
                          err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(sel, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, sel);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW) {
        *generation_out = sqlite3_column_int64(sel, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read claimed generation");
    }
    atlas_db_finish(db, sel);
    if (st == ATLAS_OK && *generation_out <= 0) {
        return atlas_err_set(err, ATLAS_ERR_DB, "index generation was not claimed");
    }
    return st;
}

atlas_status atlas_db_generation_complete(atlas_db *db, int64_t repo_id, int64_t generation,
                                          bool clear_gap, int64_t sync_seq, atlas_err *err) {
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    sqlite3_stmt *stmt = NULL;
    /* max() rather than assignment: a slow pass that finishes after a newer one
     * must never move the published generation backwards.
     *
     * The gap is only cleared when the caller states it ran a full pass. An
     * incremental pass cannot prove it saw what the gap hid, so it leaves both
     * event_gap and pending_full_reconcile alone. */
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE repo_index_state SET"
                                       " last_complete_generation = max(last_complete_generation, ?2),"
                                       " last_complete_at = ?3,"
                                       " last_sync_seq = max(last_sync_seq, ?4),"
                                       " event_gap = CASE WHEN ?5 THEN 0 ELSE event_gap END,"
                                       " pending_full_reconcile ="
                                       "   CASE WHEN ?5 THEN 0 ELSE pending_full_reconcile END,"
                                       " last_error = CASE WHEN ?5 THEN NULL ELSE last_error END"
                                       " WHERE repo_id = ?1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, generation) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, sync_seq) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 5, clear_gap ? 1 : 0) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind generation");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, now, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* P0. The one writer of everything a watch build establishes.
 *
 * Before P0 the healthy path wrote state, detail and a count here while the
 * degraded path wrote state and detail through `mark_gap` and left the count
 * alone — so a repository that had been watching 7440 directories and then
 * degraded went on reporting 7440 indefinitely, and the number a reader saw was
 * from the last time things had gone well. Every caller now goes through this
 * function with a complete outcome, so there is one place to add a field to and
 * one place that can forget it.
 *
 * `watched_dirs` is written as source + meta rather than taken as its own
 * argument: two numbers and their sum are three chances to disagree, and the
 * sum is the one that can be derived. */
atlas_status atlas_db_index_state_set_watch(atlas_db *db, int64_t repo_id,
                                            const atlas_watch_outcome *outcome, atlas_err *err) {
    atlas_status st = atlas_db_index_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE repo_index_state SET watch_state=?2, watch_detail=?3,"
                          " watched_dirs=?4, watched_source=?5, watched_meta=?6,"
                          " watched_shared=?7, watch_reason=?8 WHERE repo_id=?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, outcome->source_dirs + outcome->meta_dirs) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, outcome->source_dirs) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, outcome->meta_dirs) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 7, outcome->shared_dirs) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind watch fields");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, atlas_watch_state_name(outcome->state), err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, outcome->detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, atlas_watch_reason_name(outcome->reason), err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_index_state_mark_gap(atlas_db *db, int64_t repo_id, const char *detail,
                                           atlas_err *err) {
    atlas_status st = atlas_db_index_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    /* Both flags are persisted, so the obligation to reconcile survives a
     * restart. A daemon that crashes while a gap is open comes back knowing it
     * still owes a full pass. */
    st = atlas_db_prepare(db,
                          "UPDATE repo_index_state SET event_gap=1, pending_full_reconcile=1,"
                          " watch_state='incomplete', watch_detail=?2 WHERE repo_id=?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, detail, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_index_state_set_error(atlas_db *db, int64_t repo_id, const char *detail,
                                            atlas_err *err) {
    atlas_status st = atlas_db_index_state_ensure(db, repo_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    sqlite3_stmt *stmt = NULL;
    st = atlas_db_prepare(db,
                          "UPDATE repo_index_state SET last_error=?2, event_gap=1,"
                          " pending_full_reconcile=1 WHERE repo_id=?1;",
                          &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, detail, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- event journal ------------------------------------------------------- */

atlas_status atlas_db_event_append(atlas_db *db, int64_t repo_id, const atlas_event_record *rec,
                                   bool *inserted_out, atlas_err *err) {
    if (inserted_out != NULL) {
        *inserted_out = false;
    }
    sqlite3_stmt *stmt = NULL;
    /* The dedup key is enforced by a partial unique index, so replaying the same
     * observation after a restart collides here rather than growing the journal.
     * Events with no key (a reconciliation summary, say) are always appended. */
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO repo_events(repo_id, generation, kind, path_raw, path_text,"
                         " detail, created_at, dedup_key) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"
                         " ON CONFLICT DO NOTHING;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, rec->generation) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind event ids");
    }
    st = atlas_db_bind_text_opt(db, stmt, 3, rec->kind, err);
    if (st == ATLAS_OK) {
        st = (rec->path_raw != NULL)
                 ? atlas_db_bind_blob(db, stmt, 4, rec->path_raw, rec->path_raw_len, err)
                 : atlas_db_bind_text_opt(db, stmt, 4, NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, rec->path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 6, rec->detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 7, now, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 8, rec->dedup_key, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && inserted_out != NULL) {
        *inserted_out = sqlite3_changes(db->h) > 0;
    }
    return st;
}

atlas_status atlas_db_events_since(atlas_db *db, int64_t repo_id, int64_t since, int64_t limit,
                                   atlas_event_cb cb, void *ud, int64_t *count_out,
                                   int64_t *next_cursor_out, bool *more_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (next_cursor_out != NULL) {
        *next_cursor_out = since;
    }
    if (more_out != NULL) {
        *more_out = false;
    }
    if (limit <= 0 || limit > ATLAS_EVENTS_PAGE_MAX) {
        limit = ATLAS_EVENTS_PAGE_MAX;
    }

    sqlite3_stmt *stmt = NULL;
    /* One extra row is requested so "there is more" is a fact rather than a
     * guess, and the extra row is never delivered. */
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT id, generation, kind, path_raw, path_text, detail,"
                                       " created_at FROM repo_events"
                                       " WHERE repo_id=?1 AND id>?2 ORDER BY id ASC LIMIT ?3;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, since) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, limit + 1) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind event query");
    }

    int64_t n = 0;
    int64_t cursor = since;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read event row");
            break;
        }
        if (n >= limit) {
            if (more_out != NULL) {
                *more_out = true;
            }
            break;
        }
        atlas_event_row row;
        memset(&row, 0, sizeof(row));
        row.id = sqlite3_column_int64(stmt, 0);
        row.repo_id = repo_id;
        row.generation = sqlite3_column_int64(stmt, 1);
        row.kind = atlas_db_col_text(stmt, 2);
        row.path_raw = sqlite3_column_blob(stmt, 3);
        int blen = sqlite3_column_bytes(stmt, 3);
        row.path_raw_len = blen > 0 ? (size_t)blen : 0u;
        row.path_text = atlas_db_col_text_opt(stmt, 4);
        row.detail = atlas_db_col_text_opt(stmt, 5);
        row.created_at = atlas_db_col_text(stmt, 6);
        cursor = row.id;
        n++;
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
    }
    atlas_db_finish(db, stmt);
    if (st != ATLAS_OK) {
        return st;
    }
    if (count_out != NULL) {
        *count_out = n;
    }
    if (next_cursor_out != NULL) {
        *next_cursor_out = cursor;
    }
    return ATLAS_OK;
}

atlas_status atlas_db_events_head(atlas_db *db, int64_t repo_id, int64_t *out, atlas_err *err) {
    *out = 0;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT coalesce(max(id),0) FROM repo_events WHERE repo_id=?1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read event head");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_events_prune(atlas_db *db, int64_t repo_id, int64_t retain,
                                   int64_t *removed_out, atlas_err *err) {
    if (removed_out != NULL) {
        *removed_out = 0;
    }
    if (retain <= 0) {
        retain = ATLAS_EVENTS_RETAIN_PER_REPO;
    }
    sqlite3_stmt *stmt = NULL;
    /* Raw events have bounded retention. Evidence does not: this statement
     * touches repo_events only, and nothing in it can reach the evidence table. */
    atlas_status st = atlas_db_prepare(db,
                                       "DELETE FROM repo_events WHERE repo_id=?1 AND id <="
                                       " (SELECT id FROM repo_events WHERE repo_id=?1"
                                       "  ORDER BY id DESC LIMIT 1 OFFSET ?2);",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, retain) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind prune bounds");
    }
    st = atlas_db_step_done(db, stmt, err);
    if (st == ATLAS_OK && removed_out != NULL) {
        *removed_out = sqlite3_changes(db->h);
    }
    return st;
}

/* --- commit tips --------------------------------------------------------- */

atlas_status atlas_db_commit_tip_get(atlas_db *db, int64_t repo_id, const char *ref_name,
                                     char *oid_out, size_t oid_out_size, bool *found,
                                     atlas_err *err) {
    *found = false;
    if (oid_out_size > 0) {
        oid_out[0] = '\0';
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "SELECT tip_oid FROM repo_commit_tips WHERE repo_id=?1 AND ref_name=?2;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, ref_name, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        st = atlas_db_col_copy(stmt, 0, oid_out, oid_out_size, "commit tip", err);
        if (st == ATLAS_OK) {
            *found = true;
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read commit tip");
    }
    atlas_db_finish(db, stmt);
    return st;
}

atlas_status atlas_db_commit_tip_set(atlas_db *db, int64_t repo_id, const char *ref_name,
                                     const char *oid, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db,
                         "INSERT INTO repo_commit_tips(repo_id, ref_name, tip_oid, ingested_at)"
                         " VALUES(?1,?2,?3,?4) ON CONFLICT(repo_id, ref_name) DO UPDATE SET"
                         " tip_oid=excluded.tip_oid, ingested_at=excluded.ingested_at;",
                         &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, ref_name, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 3, oid, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, now, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

/* --- daemon record ------------------------------------------------------- */

void atlas_daemon_record_init(atlas_daemon_record *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->socket_path);
}

void atlas_daemon_record_free(atlas_daemon_record *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->socket_path);
}

atlas_status atlas_db_daemon_started(atlas_db *db, int64_t pid, const char *socket_path,
                                     atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db,
        "INSERT INTO daemon_state(id, pid, started_at, last_heartbeat_at, stopped_at,"
        " protocol_version, atlas_version, socket_path) VALUES(1,?1,?2,?2,NULL,?3,?4,?5)"
        " ON CONFLICT(id) DO UPDATE SET pid=excluded.pid, started_at=excluded.started_at,"
        " last_heartbeat_at=excluded.last_heartbeat_at, stopped_at=NULL,"
        " protocol_version=excluded.protocol_version, atlas_version=excluded.atlas_version,"
        " socket_path=excluded.socket_path;",
        &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    if (sqlite3_bind_int64(stmt, 1, pid) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 3, (int)ATLAS_IPC_PROTOCOL_VERSION) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind daemon identity");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 4, ATLAS_VERSION_STRING, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_bind_text_opt(db, stmt, 5, socket_path, err);
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_daemon_heartbeat(atlas_db *db, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st =
        atlas_db_prepare(db, "UPDATE daemon_state SET last_heartbeat_at=?1 WHERE id=1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, now, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_daemon_stopped(atlas_db *db, atlas_err *err) {
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(
        db, "UPDATE daemon_state SET stopped_at=?1, pid=NULL WHERE id=1;", &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    st = atlas_db_bind_text_opt(db, stmt, 1, now, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_daemon_get(atlas_db *db, atlas_daemon_record *out, atlas_err *err) {
    out->present = false;
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT pid, started_at, last_heartbeat_at, stopped_at,"
                                       " protocol_version, atlas_version, socket_path"
                                       " FROM daemon_state WHERE id=1;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->present = true;
        out->pid = sqlite3_column_int64(stmt, 0);
        st = atlas_db_col_copy(stmt, 1, out->started_at, sizeof(out->started_at), "started_at", err);
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(stmt, 2, out->last_heartbeat_at, sizeof(out->last_heartbeat_at),
                                   "last_heartbeat_at", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_col_copy(stmt, 3, out->stopped_at, sizeof(out->stopped_at), "stopped_at",
                                   err);
        }
        if (st == ATLAS_OK) {
            out->protocol_version = sqlite3_column_int(stmt, 4);
            st = atlas_db_col_copy(stmt, 5, out->atlas_version, sizeof(out->atlas_version),
                                   "atlas_version", err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_set_str(&out->socket_path, atlas_db_col_text(stmt, 6), err);
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read daemon state");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* --- filesystem identity ------------------------------------------------- */

bool atlas_fs_identity_same(const atlas_fs_identity *a, const atlas_fs_identity *b) {
    /* An unknown identity on either side is never "the same". A file whose
     * previous observation Atlas does not have is hashed once, and from then on
     * it is cheap. Guessing the other way would silently keep a stale hash. */
    if (!a->known || !b->known) {
        return false;
    }
    /* Every field, including ctime. Dropping ctime here would reintroduce the
     * exact bypass the column exists to close: a same-length in-place edit with
     * the mtime restored by utimensat matches on all the other fields. */
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec &&
           a->ctime_sec == b->ctime_sec && a->ctime_nsec == b->ctime_nsec && a->mode == b->mode;
}

atlas_status atlas_db_file_touch(atlas_db *db, int64_t file_id, int64_t scan_id,
                                 int64_t generation, const atlas_fs_identity *fs, atlas_err *err) {
    char now[ATLAS_TS_MAX];
    atlas_now_iso8601(now, sizeof(now));
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "UPDATE files SET last_seen_scan_id=?1, last_seen_at=?2,"
                                       " last_generation=?3, fs_dev=?4, fs_ino=?5, fs_size=?6,"
                                       " fs_mtime_sec=?7, fs_mtime_nsec=?8, fs_ctime_sec=?9,"
                                       " fs_ctime_nsec=?10, fs_mode=?11"
                                       " WHERE id=?12;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, scan_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, generation) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 12, file_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind file liveness fields");
    }
    st = atlas_db_bind_text_opt(db, stmt, 2, now, err);
    if (st == ATLAS_OK) {
        const int64_t v[ATLAS_FS_IDENTITY_COLUMNS] = {fs->dev,        fs->ino,
                                                      fs->size,       fs->mtime_sec,
                                                      fs->mtime_nsec, fs->ctime_sec,
                                                      fs->ctime_nsec, fs->mode};
        for (int i = 0; i < ATLAS_FS_IDENTITY_COLUMNS && st == ATLAS_OK; i++) {
            int rc = fs->known ? sqlite3_bind_int64(stmt, 4 + i, v[i])
                               : sqlite3_bind_null(stmt, 4 + i);
            if (rc != SQLITE_OK) {
                st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind filesystem identity");
            }
        }
    }
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    return atlas_db_step_done(db, stmt, err);
}

atlas_status atlas_db_file_identity(atlas_db *db, int64_t repo_id, const void *path_raw,
                                    size_t path_len, atlas_fs_identity *out, int64_t *file_id_out,
                                    bool *found, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    *found = false;
    if (file_id_out != NULL) {
        *file_id_out = 0;
    }
    sqlite3_stmt *stmt = NULL;
    atlas_status st = atlas_db_prepare(db,
                                       "SELECT id, fs_dev, fs_ino, fs_size, fs_mtime_sec,"
                                       " fs_mtime_nsec, fs_ctime_sec, fs_ctime_nsec, fs_mode"
                                       " FROM files"
                                       " WHERE repo_id=?1 AND path_raw=?2 AND deleted=0;",
                                       &stmt, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sqlite3_bind_int64(stmt, 1, repo_id) != SQLITE_OK) {
        atlas_db_finish(db, stmt);
        return atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot bind repository id");
    }
    st = atlas_db_bind_blob(db, stmt, 2, path_raw, path_len, err);
    if (st != ATLAS_OK) {
        atlas_db_finish(db, stmt);
        return st;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *found = true;
        if (file_id_out != NULL) {
            *file_id_out = sqlite3_column_int64(stmt, 0);
        }
        /* Any NULL among the identity columns means the row predates A1, or was
         * written without an lstat, or was deliberately stored as unknown
         * because the observation was racy. Treat the whole identity as unknown
         * rather than comparing a partial one: a partial comparison would report
         * "unchanged" on the strength of the fields that happen to be present. */
        bool complete = true;
        for (int c = 1; c <= ATLAS_FS_IDENTITY_COLUMNS; c++) {
            if (sqlite3_column_type(stmt, c) == SQLITE_NULL) {
                complete = false;
            }
        }
        if (complete) {
            out->known = true;
            out->dev = sqlite3_column_int64(stmt, 1);
            out->ino = sqlite3_column_int64(stmt, 2);
            out->size = sqlite3_column_int64(stmt, 3);
            out->mtime_sec = sqlite3_column_int64(stmt, 4);
            out->mtime_nsec = sqlite3_column_int64(stmt, 5);
            out->ctime_sec = sqlite3_column_int64(stmt, 6);
            out->ctime_nsec = sqlite3_column_int64(stmt, 7);
            out->mode = sqlite3_column_int64(stmt, 8);
        }
    } else if (rc != SQLITE_DONE) {
        st = atlas_db_fail(db, err, ATLAS_ERR_DB, "cannot read file identity");
    }
    atlas_db_finish(db, stmt);
    return st;
}

/* Atlas - the provider-neutral AI session service.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is where an adapter's request becomes a durable record. It runs on the
 * daemon's writer thread and nowhere else: `db` is the one writable handle, the
 * writer created it, and every rule in the A1 concurrency model applies here
 * unchanged.
 *
 * Nothing in this file names Claude. It knows about a provider, a client and a
 * session key, which is the whole of what an adapter has to supply.
 *
 * The two rules the file exists to enforce:
 *
 *   - a model's claim is stored as a proposal, never as an approved decision,
 *     and `atlas_provenance_writable_in_a2` refuses the attempt rather than
 *     downgrading it silently;
 *   - a changed path with no reason is left *visibly* unresolved. Nothing here
 *     invents a reason, and recording UNKNOWN is a first-class write with its
 *     own row rather than the absence of one.
 */
#include "atlas/ai.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "atlas/safetext.h"

/* --- vocabulary ---------------------------------------------------------- */

const char *atlas_provenance_name(atlas_provenance p) {
    switch (p) {
    case ATLAS_PROV_ATLAS_OWNED: return "ATLAS_OWNED";
    case ATLAS_PROV_USER_APPROVED_DECISION: return "USER_APPROVED_DECISION";
    case ATLAS_PROV_GIT: return "GIT";
    case ATLAS_PROV_SOURCE: return "SOURCE";
    case ATLAS_PROV_MODEL_PROPOSAL: return "MODEL_PROPOSAL";
    case ATLAS_PROV_MODEL_INFERENCE: return "MODEL_INFERENCE";
    case ATLAS_PROV_UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool atlas_provenance_parse(const char *name, atlas_provenance *out) {
    static const struct {
        const char *name;
        atlas_provenance value;
    } TABLE[] = {
        {"ATLAS_OWNED", ATLAS_PROV_ATLAS_OWNED},
        {"USER_APPROVED_DECISION", ATLAS_PROV_USER_APPROVED_DECISION},
        {"GIT", ATLAS_PROV_GIT},
        {"SOURCE", ATLAS_PROV_SOURCE},
        {"MODEL_PROPOSAL", ATLAS_PROV_MODEL_PROPOSAL},
        {"MODEL_INFERENCE", ATLAS_PROV_MODEL_INFERENCE},
        {"UNKNOWN", ATLAS_PROV_UNKNOWN},
    };
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].value;
            return true;
        }
    }
    /* No default. Defaulting an unrecognised provenance to a known one is
     * exactly how a model proposal would quietly become a recorded fact. */
    return false;
}

bool atlas_provenance_is_untrusted(atlas_provenance p) {
    return p == ATLAS_PROV_GIT || p == ATLAS_PROV_SOURCE || p == ATLAS_PROV_MODEL_PROPOSAL ||
           p == ATLAS_PROV_MODEL_INFERENCE;
}

bool atlas_provenance_writable_in_a2(atlas_provenance p) {
    /* USER_APPROVED_DECISION is refused because A2 cannot prove it: an argument
     * asserting that the user approved something is a string a model produced,
     * and treating it as proof would make the strongest provenance class the
     * easiest one to obtain. ATLAS_OWNED, GIT and SOURCE are refused because
     * they describe what Atlas read, and an adapter does not get to assert
     * those either. */
    return p == ATLAS_PROV_MODEL_PROPOSAL || p == ATLAS_PROV_MODEL_INFERENCE ||
           p == ATLAS_PROV_UNKNOWN;
}

const char *atlas_ai_confidence_name(atlas_ai_confidence c) {
    switch (c) {
    case ATLAS_AI_CONF_NONE: return "none";
    case ATLAS_AI_CONF_LOW: return "low";
    case ATLAS_AI_CONF_MEDIUM: return "medium";
    case ATLAS_AI_CONF_HIGH: return "high";
    }
    return "none";
}

bool atlas_ai_confidence_parse(const char *name, atlas_ai_confidence *out) {
    if (name == NULL) {
        return false;
    }
    if (strcmp(name, "none") == 0) {
        *out = ATLAS_AI_CONF_NONE;
        return true;
    }
    if (strcmp(name, "low") == 0) {
        *out = ATLAS_AI_CONF_LOW;
        return true;
    }
    if (strcmp(name, "medium") == 0) {
        *out = ATLAS_AI_CONF_MEDIUM;
        return true;
    }
    if (strcmp(name, "high") == 0) {
        *out = ATLAS_AI_CONF_HIGH;
        return true;
    }
    return false;
}

const char *atlas_ai_attribution_name(atlas_ai_attribution a) {
    switch (a) {
    case ATLAS_AI_ATTR_DIRECT_EDIT: return "direct_edit";
    case ATLAS_AI_ATTR_OBSERVED: return "observed";
    case ATLAS_AI_ATTR_AMBIGUOUS: return "ambiguous";
    }
    return "observed";
}

bool atlas_ai_op_needs_open_session(atlas_ai_op_kind kind) {
    /* Enumerated rather than defaulted. A new op kind has to be classified here
     * before it compiles cleanly, which is the point: the answer for a record
     * type and the answer for a bookkeeping event are different, and a default
     * would quietly pick one of them. */
    switch (kind) {
    case ATLAS_AI_OP_REASON:
    case ATLAS_AI_OP_DECISION: return true;
    case ATLAS_AI_OP_SESSION_OPEN:
    case ATLAS_AI_OP_SESSION_TOUCH:
    case ATLAS_AI_OP_SESSION_CLOSE:
    case ATLAS_AI_OP_SESSION_CHECKPOINT:
    case ATLAS_AI_OP_ATTACH_ROOT:
    case ATLAS_AI_OP_TOOL_RECORD:
    case ATLAS_AI_OP_CORRELATE:
    case ATLAS_AI_OP_TURN_CLOSE: return false;
    }
    return false;
}

/* --- op lifecycle -------------------------------------------------------- */

void atlas_ai_op_init(atlas_ai_op *op, atlas_ai_op_kind kind) {
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    op->provenance = ATLAS_PROV_MODEL_PROPOSAL;
    op->confidence = ATLAS_AI_CONF_NONE;
    atlas_buf_init(&op->provider);
    atlas_buf_init(&op->client);
    atlas_buf_init(&op->client_version);
    atlas_buf_init(&op->session_key);
    atlas_buf_init(&op->parent_session_key);
    atlas_buf_init(&op->agent_id);
    atlas_buf_init(&op->agent_type);
    atlas_buf_init(&op->root);
    atlas_buf_init(&op->repo_name);
    atlas_buf_init(&op->source);
    atlas_buf_init(&op->tool_name);
    atlas_buf_init(&op->tool_use_id);
    atlas_buf_init(&op->paths);
    atlas_buf_init(&op->summary);
    atlas_buf_init(&op->detail);
    atlas_buf_init(&op->title);
    atlas_buf_init(&op->statement);
    atlas_buf_init(&op->rationale);
    atlas_buf_init(&op->unknown_reason);
    atlas_buf_init(&op->dedup_key);
}

void atlas_ai_op_free(atlas_ai_op *op) {
    if (op == NULL) {
        return;
    }
    atlas_buf_free(&op->provider);
    atlas_buf_free(&op->client);
    atlas_buf_free(&op->client_version);
    atlas_buf_free(&op->session_key);
    atlas_buf_free(&op->parent_session_key);
    atlas_buf_free(&op->agent_id);
    atlas_buf_free(&op->agent_type);
    atlas_buf_free(&op->root);
    atlas_buf_free(&op->repo_name);
    atlas_buf_free(&op->source);
    atlas_buf_free(&op->tool_name);
    atlas_buf_free(&op->tool_use_id);
    atlas_buf_free(&op->paths);
    atlas_buf_free(&op->summary);
    atlas_buf_free(&op->detail);
    atlas_buf_free(&op->title);
    atlas_buf_free(&op->statement);
    atlas_buf_free(&op->rationale);
    atlas_buf_free(&op->unknown_reason);
    atlas_buf_free(&op->dedup_key);
}

void atlas_ai_result_init(atlas_ai_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->repo_name);
    atlas_buf_init(&r->root_text);
    atlas_buf_init(&r->degraded_reason);
}

void atlas_ai_result_free(atlas_ai_result *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->repo_name);
    atlas_buf_free(&r->root_text);
    atlas_buf_free(&r->degraded_reason);
}

/* --- helpers ------------------------------------------------------------- */

static const char *buf_or_null(const atlas_buf *b) {
    return b->len > 0 ? atlas_buf_cstr(b) : NULL;
}

/* Resolves the operation's repository.
 *
 * A path wins over a name: an adapter observing a working directory knows where
 * it is, and a name is only a convenience. The lookup walks up from the given
 * path rather than requiring the exact root, because a hook's cwd is normally a
 * subdirectory of the repository, and a tool's path always is.
 *
 * `*found_out` is false rather than an error when nothing matches. An
 * unregistered directory is a completely normal state and the caller decides
 * what to do about it. */
static atlas_status resolve_repo(atlas_db *db, const atlas_ai_op *op, atlas_repo_info *out,
                                 bool *found_out, atlas_err *err) {
    *found_out = false;
    if (op->repo_name.len > 0) {
        return atlas_db_repo_get(db, atlas_buf_cstr(&op->repo_name), out, found_out, err);
    }
    if (op->root.len == 0) {
        return ATLAS_OK;
    }
    return atlas_db_repo_get_containing(db, op->root.data, op->root.len, out, found_out, err);
}

/* Iterates the NUL-separated raw path list an op carries. */
typedef struct path_iter {
    const char *p;
    const char *end;
} path_iter;

static void path_iter_init(path_iter *it, const atlas_buf *paths) {
    it->p = paths->len > 0 ? paths->data : NULL;
    it->end = it->p != NULL ? paths->data + paths->len : NULL;
}

static bool path_iter_next(path_iter *it, const char **out, size_t *len_out) {
    if (it->p == NULL || it->p >= it->end) {
        return false;
    }
    const char *start = it->p;
    while (it->p < it->end && *it->p != '\0') {
        it->p++;
    }
    *out = start;
    *len_out = (size_t)(it->p - start);
    if (it->p < it->end) {
        it->p++; /* step over the separator */
    }
    return *len_out > 0;
}

/* Ensures the session row and its client, and attaches the repository when one
 * was resolved. Shared by every op kind, because every op needs a session and
 * none of them should each grow their own version of finding one. */
typedef struct session_ctx {
    int64_t client_id;
    int64_t session_id;
    int64_t repo_id;
    int64_t change_set_id;
    bool session_created;
    bool repo_found;
    atlas_repo_info repo;
} session_ctx;

static void session_ctx_init(session_ctx *sc) {
    memset(sc, 0, sizeof(*sc));
    atlas_repo_info_init(&sc->repo);
}

static void session_ctx_free(session_ctx *sc) {
    atlas_repo_info_free(&sc->repo);
}

static atlas_status ensure_client(atlas_db *db, const atlas_ai_op *op, int64_t *client_id,
                                  atlas_err *err) {
    const char *provider = op->provider.len > 0 ? atlas_buf_cstr(&op->provider) : "unknown";
    const char *client = op->client.len > 0 ? atlas_buf_cstr(&op->client) : "unknown";
    return atlas_db_ai_client_upsert(db, provider, client, client_id, err);
}

/* Finds an existing session without creating one.
 *
 * Exact match on `(client, session_key)` or nothing. The repository is not
 * consulted, deliberately: see the attribution section in atlas/ai.h. Every
 * path that does not produce a session records why, so a caller can tell "this
 * is unattributed and here is the reason" from "this is attributed".
 *
 * `sc->repo_found` is unused here and that is the point — a session and a
 * repository are independent facts, and the moment one is allowed to imply the
 * other, two concurrent sessions on one worktree become indistinguishable. */
static atlas_status find_session(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                 atlas_ai_result *out, atlas_err *err) {
    atlas_status st = ensure_client(db, op, &sc->client_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (op->session_key.len == 0) {
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_NO_SESSION_ID;
        return ATLAS_OK;
    }

    bool session_open = false;
    st = atlas_db_ai_session_find_state(db, sc->client_id, atlas_buf_cstr(&op->session_key),
                                        &sc->session_id, &session_open, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (sc->session_id == 0) {
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_UNKNOWN_SESSION;
        return ATLAS_OK;
    }
    if (!session_open && atlas_ai_op_needs_open_session(op->kind)) {
        /* The id is right and the conversation it named is over. Dropping back
         * to sessionless loses the link; keeping it would assert that a turn
         * happening now belongs to a session that ended. The first is a gap the
         * caller can see, the second is a false record. */
        sc->session_id = 0;
        out->session_unbound = true;
        out->unbound_reason = ATLAS_AI_UNBOUND_SESSION_CLOSED;
    }
    return ATLAS_OK;
}

/* --- operations ---------------------------------------------------------- */

static atlas_status op_session_open(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                    atlas_ai_result *out, atlas_err *err) {
    atlas_status st = ensure_client(db, op, &sc->client_id, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A parent, when the client named one, is looked up but never created: a
     * fork whose parent Atlas never saw is a session with no lineage, which is
     * accurate, rather than a session with an invented parent. */
    int64_t parent_id = 0;
    if (op->parent_session_key.len > 0) {
        st = atlas_db_ai_session_find(db, sc->client_id, atlas_buf_cstr(&op->parent_session_key),
                                      &parent_id, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    st = atlas_db_ai_session_open(db, sc->client_id, atlas_buf_cstr(&op->session_key), parent_id,
                                  buf_or_null(&op->agent_id), buf_or_null(&op->agent_type),
                                  buf_or_null(&op->client_version), &sc->session_id,
                                  &sc->session_created, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->session_created = sc->session_created;

    /* Idle sessions are expired opportunistically here rather than on a timer:
     * a session that stopped answering must not hold a change set open forever,
     * and session open is the moment a client is demonstrably present, so it is
     * the cheapest place to do the sweep. */
    char cutoff[ATLAS_TS_MAX];
    atlas_iso8601_before_now(cutoff, sizeof(cutoff), ATLAS_AI_SESSION_IDLE_EXPIRY_MS);
    int64_t expired = 0;
    (void)atlas_db_ai_sessions_expire(db, cutoff, &expired, err);
    atlas_err_init(err);

    return atlas_db_ai_event_append(
        db, sc->session_id, sc->repo_id,
        sc->session_created ? "session_open" : "session_resume", NULL, NULL, NULL, 0u, NULL,
        buf_or_null(&op->dedup_key), NULL, err);
}

/* Attaches a repository to a session and opens its change set. */
static atlas_status attach_repo(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                atlas_ai_result *out, atlas_err *err) {
    if (sc->session_id == 0 || !sc->repo_found) {
        return ATLAS_OK;
    }
    atlas_index_state state;
    atlas_index_state_init(&state);
    atlas_status st = atlas_db_index_state_get(db, sc->repo.id, &state, err);
    int64_t generation = (st == ATLAS_OK && state.present) ? state.last_complete_generation : 0;
    atlas_index_state_free(&state);
    if (st != ATLAS_OK) {
        return st;
    }

    const char *source = op->source.len > 0 ? atlas_buf_cstr(&op->source) : "session_start";
    st = atlas_db_ai_session_attach_repo(db, sc->session_id, sc->repo.id, source,
                                         sc->repo.scanned_head, err);
    if (st == ATLAS_OK) {
        st = atlas_db_ai_change_set_ensure(db, sc->session_id, sc->repo.id, sc->repo.scanned_head,
                                           generation, &sc->change_set_id, err);
    }
    if (st == ATLAS_OK) {
        out->change_set_id = sc->change_set_id;
        out->repo_id = sc->repo.id;
        st = atlas_buf_set_str(&out->repo_name, sc->repo.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->root_text, sc->repo.root_path_text.data,
                           sc->repo.root_path_text.len, err);
    }
    return st;
}

static atlas_status op_tool_record(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                   atlas_ai_result *out, atlas_err *err) {
    (void)out;
    if (sc->session_id == 0) {
        /* A tool observation for a session Atlas never opened is dropped rather
         * than creating one: a session that never had a SessionStart has no
         * base head and no change set, so an invented row would describe a
         * window Atlas cannot bound. */
        return ATLAS_OK;
    }
    const char *kind = "tool_intent";
    switch (op->tool_phase) {
    case ATLAS_AI_TOOL_INTENT: kind = "tool_intent"; break;
    case ATLAS_AI_TOOL_OK: kind = "tool_ok"; break;
    case ATLAS_AI_TOOL_FAILED: kind = "tool_failed"; break;
    }

    /* At most one path, and only the normalized repository-relative one. The
     * tool's input, its output and its error text are all deliberately absent:
     * see docs/ai-trust-boundary.md for what is not stored and why. */
    path_iter it;
    path_iter_init(&it, &op->paths);
    const char *path = NULL;
    size_t path_len = 0;
    bool have_path = path_iter_next(&it, &path, &path_len);

    atlas_buf path_text = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (have_path) {
        st = atlas_path_text_encode(path, path_len, &path_text, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_ai_event_append(db, sc->session_id, sc->repo_id, kind,
                                      buf_or_null(&op->tool_name), buf_or_null(&op->tool_use_id),
                                      have_path ? path : NULL, path_len,
                                      have_path ? atlas_buf_cstr(&path_text) : NULL,
                                      buf_or_null(&op->dedup_key), NULL, err);
    }
    atlas_buf_free(&path_text);
    if (st == ATLAS_OK) {
        st = atlas_db_ai_session_touch(db, sc->session_id, "tool_calls", err);
    }
    if (st == ATLAS_OK) {
        int64_t removed = 0;
        st = atlas_db_ai_events_prune(db, sc->session_id, ATLAS_AI_EVENTS_RETAIN_PER_SESSION,
                                      &removed, err);
    }
    return st;
}

/* Correlates the index's observation of what changed with the session.
 *
 * The order matters. The pass runs first (so the index has seen whatever the
 * tools did), then the worktree snapshot is read, then each changed path is
 * attributed. Attribution is decided by two independent facts: whether this
 * session recorded an edit intent naming the path, and whether any other
 * session had this repository open. The second overrides the first, because two
 * sessions editing one repository makes neither claim supportable. */
typedef struct correlate_ctx {
    atlas_db *db;
    session_ctx *sc;
    int64_t concurrent;
    int64_t recorded;
    bool truncated;
} correlate_ctx;

static atlas_status correlate_one(const atlas_worktree_change_row *row, void *ud, atlas_err *err) {
    correlate_ctx *cc = (correlate_ctx *)ud;
    if (cc->recorded >= ATLAS_AI_MAX_CHANGED_PATHS) {
        cc->truncated = true;
        return ATLAS_OK;
    }

    atlas_buf tool = ATLAS_BUF_INIT;
    bool direct = false;
    atlas_status st = atlas_db_ai_event_intent_for_path(cc->db, cc->sc->session_id, row->path_raw,
                                                        row->path_raw_len, &tool, &direct, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&tool);
        return st;
    }

    /* Concurrency wins. A direct edit recorded by this session does not make the
     * change unambiguous when another session was also in a position to make
     * it: both could have written the same path, and Atlas cannot tell. */
    const char *attribution = "observed";
    if (cc->concurrent > 0) {
        attribution = "ambiguous";
    } else if (direct) {
        attribution = "direct_edit";
    }

    st = atlas_db_ai_changed_path_record(cc->db, cc->sc->change_set_id, row->path_raw,
                                         row->path_raw_len, row->path_text, attribution,
                                         direct ? atlas_buf_cstr(&tool) : NULL, cc->concurrent,
                                         err);
    atlas_buf_free(&tool);
    if (st == ATLAS_OK) {
        cc->recorded++;
    }
    return st;
}

/* Walks the whole working-tree snapshot and attributes every changed path.
 *
 * Run from two places, and it has to be. A batch queues a reconciliation and
 * cannot wait for it — the pass is a job behind this one on the same writer
 * thread, so waiting would be the writer waiting on itself — which means the
 * batch correlates against the *previous* pass's snapshot. By the time the turn
 * closes, the watcher-triggered pass has normally published, so the sweep runs
 * again there. Running it twice is cheap and idempotent; running it once at
 * batch time would report zero changed paths for the common single-batch turn,
 * and Stop would then find nothing to mark UNKNOWN.
 *
 * The cursor is looped rather than taking one page. A repository with more dirty
 * paths than one page is exactly the case where the attribution matters, and a
 * silent cap at the page size would be the "looks complete, gave up" failure the
 * rest of Atlas is arranged to avoid. */
static atlas_status sweep_changed_paths(atlas_db *db, session_ctx *sc, int64_t *concurrent_out,
                                        atlas_err *err) {
    atlas_status st =
        atlas_db_ai_concurrent_sessions(db, sc->repo_id, sc->session_id, concurrent_out, err);
    if (st != ATLAS_OK) {
        return st;
    }

    correlate_ctx cc;
    memset(&cc, 0, sizeof(cc));
    cc.db = db;
    cc.sc = sc;
    cc.concurrent = *concurrent_out;

    int64_t cursor = 0;
    bool more = true;
    while (st == ATLAS_OK && more && cc.recorded < ATLAS_AI_MAX_CHANGED_PATHS) {
        int64_t count = 0;
        /* Every scope: a staged change and an untracked new file are both things
         * this session may have caused. */
        st = atlas_db_worktree_changes_list(db, sc->repo_id, NULL, cursor, ATLAS_MCP_MAX_ROWS,
                                            correlate_one, &cc, &count, &cursor, &more, err);
        if (count == 0) {
            break; /* nothing advanced the cursor; stop rather than spin */
        }
    }
    return st;
}

static atlas_status op_correlate(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                 atlas_ai_sync_fn sync, void *sync_ud, atlas_ai_result *out,
                                 atlas_err *err) {
    if (sc->session_id == 0 || sc->change_set_id == 0) {
        return ATLAS_OK;
    }

    /* The paths the adapter says this session's edit tools named are recorded as
     * intents first, so the correlation below can see them even when the
     * PreToolUse hook was never delivered — a batch is allowed to be the only
     * hook that arrives. */
    path_iter it;
    path_iter_init(&it, &op->paths);
    const char *path = NULL;
    size_t path_len = 0;
    int64_t named = 0;
    atlas_status st = ATLAS_OK;
    while (st == ATLAS_OK && named < ATLAS_AI_MAX_BATCH_PATHS &&
           path_iter_next(&it, &path, &path_len)) {
        atlas_buf path_text = ATLAS_BUF_INIT;
        st = atlas_path_text_encode(path, path_len, &path_text, err);
        if (st == ATLAS_OK) {
            /* A per-path dedup key, so a redelivered batch does not record the
             * same edit twice.
             *
             * Built from the caller's own batch key — for Claude, the first tool
             * use id in the batch — because that is the value that is stable
             * across a redelivery and different between two batches. Deriving it
             * from a counter the caller does not send would make every batch
             * after the first collide with the first, and a path edited in two
             * different turns would record only the earlier intent. */
            atlas_buf key = ATLAS_BUF_INIT;
            st = atlas_buf_appendf(&key, err, "batch:%s:%s",
                                   op->dedup_key.len > 0 ? atlas_buf_cstr(&op->dedup_key) : "0",
                                   atlas_buf_cstr(&path_text));
            if (st == ATLAS_OK) {
                st = atlas_db_ai_event_append(db, sc->session_id, sc->repo_id, "tool_intent",
                                              buf_or_null(&op->tool_name), NULL, path, path_len,
                                              atlas_buf_cstr(&path_text), atlas_buf_cstr(&key),
                                              NULL, err);
            }
            atlas_buf_free(&key);
        }
        atlas_buf_free(&path_text);
        named++;
    }
    if (st != ATLAS_OK) {
        return st;
    }
    out->direct_paths = named;

    /* Ask for an incremental pass. The callback belongs to the daemon's writer,
     * which coalesces it with anything already pending; this file has no
     * knowledge of the job queue. */
    if (op->request_sync && sync != NULL) {
        int64_t seq = 0;
        atlas_err sync_err;
        atlas_err_init(&sync_err);
        if (sync(sync_ud, sc->repo_id, op->paths.len > 0 ? op->paths.data : NULL, op->paths.len,
                 &seq, &sync_err) == ATLAS_OK) {
            out->sync_seq = seq;
        } else {
            /* Backpressure is reported, not hidden. The correlation below still
             * runs against whatever the index already holds. */
            out->degraded = true;
            (void)atlas_buf_set_str(&out->degraded_reason, atlas_err_msg(&sync_err), err);
        }
    }

    st = sweep_changed_paths(db, sc, &out->concurrent_sessions, err);
    if (st != ATLAS_OK) {
        return st;
    }

    st = atlas_db_ai_changed_counts(db, sc->change_set_id, &out->changed_paths, &out->direct_paths,
                                    &out->ambiguous_paths, &out->unresolved_paths, err);
    if (st == ATLAS_OK) {
        st = atlas_db_ai_event_append(db, sc->session_id, sc->repo_id, "batch", NULL, NULL, NULL,
                                      0u, NULL, buf_or_null(&op->dedup_key), NULL, err);
    }
    return st;
}

/* Records a change-reason proposal, or an explicit absence of one. */
static atlas_status op_reason(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                              atlas_ai_result *out, atlas_err *err) {
    if (!sc->repo_found) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "no registered repository matches this request, so there is nothing "
                             "to attach a reason to");
    }
    if (!atlas_provenance_writable_in_a2(op->provenance)) {
        /* Refused rather than downgraded. A caller that asked to record an
         * approved decision must be told Atlas will not, because silently
         * storing it as a proposal would leave the caller believing something
         * stronger was recorded than actually was. */
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "provenance %s cannot be written by an A2 adapter: Atlas has no way "
                             "to prove a human approved anything, so a model record is stored as "
                             "MODEL_PROPOSAL, MODEL_INFERENCE or UNKNOWN and never as an approval",
                             atlas_provenance_name(op->provenance));
    }

    int64_t records = 0;
    atlas_status st = ATLAS_OK;
    if (sc->session_id > 0) {
        atlas_ai_session_report rep;
        atlas_ai_session_report_init(&rep);
        st = atlas_db_ai_session_get(db, sc->client_id, atlas_buf_cstr(&op->session_key), &rep,
                                     err);
        records = rep.records;
        atlas_ai_session_report_free(&rep);
        if (st != ATLAS_OK) {
            return st;
        }
        if (records >= ATLAS_AI_MAX_RECORDS_PER_SESSION) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "this session has already recorded %d reasons and decisions, "
                                 "which is the per-session ceiling; the request was refused rather "
                                 "than dropped",
                                 ATLAS_AI_MAX_RECORDS_PER_SESSION);
        }
    }

    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.session_id = sc->session_id;
    in.repo_id = sc->repo.id;
    in.change_set_id = sc->change_set_id;
    in.provenance = atlas_provenance_name(op->unknown ? ATLAS_PROV_UNKNOWN : op->provenance);
    in.state = op->unknown ? "unknown" : "proposed";
    in.confidence = atlas_ai_confidence_name(op->unknown ? ATLAS_AI_CONF_NONE : op->confidence);
    in.summary = op->unknown ? NULL : buf_or_null(&op->summary);
    in.detail = op->unknown ? NULL : buf_or_null(&op->detail);
    in.unknown_reason = op->unknown ? buf_or_null(&op->unknown_reason) : NULL;
    in.dedup_key = buf_or_null(&op->dedup_key);

    bool duplicate = false;
    st = atlas_db_ai_reason_insert(db, &in, &out->record_id, &duplicate, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->duplicate = duplicate;
    if (duplicate || out->record_id == 0) {
        return ATLAS_OK;
    }

    path_iter it;
    path_iter_init(&it, &op->paths);
    const char *path = NULL;
    size_t path_len = 0;
    int64_t n = 0;
    while (st == ATLAS_OK && n < ATLAS_AI_MAX_PATHS_PER_RECORD &&
           path_iter_next(&it, &path, &path_len)) {
        atlas_buf path_text = ATLAS_BUF_INIT;
        st = atlas_path_text_encode(path, path_len, &path_text, err);
        if (st == ATLAS_OK) {
            st = atlas_db_ai_reason_path_add(db, out->record_id, path, path_len,
                                             atlas_buf_cstr(&path_text), err);
        }
        if (st == ATLAS_OK) {
            /* Link to the SOURCE or GIT evidence Atlas already holds for the
             * path. The model record and the facts stay connected without
             * either becoming the other. */
            st = atlas_db_ai_evidence_link(db, "reason", out->record_id, sc->repo.id, path,
                                           path_len, err);
        }
        atlas_buf_free(&path_text);
        n++;
    }
    if (st == ATLAS_OK && sc->session_id > 0) {
        st = atlas_db_ai_session_touch(db, sc->session_id, "records", err);
    }
    return st;
}

static atlas_status op_decision(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                atlas_ai_result *out, atlas_err *err) {
    if (!sc->repo_found) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "no registered repository matches this request, so there is nothing "
                             "to attach a decision to");
    }
    if (op->provenance != ATLAS_PROV_MODEL_PROPOSAL &&
        op->provenance != ATLAS_PROV_MODEL_INFERENCE) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "a decision recorded by an A2 adapter is a proposal: provenance %s "
                             "is refused because Atlas cannot prove a human approved it",
                             atlas_provenance_name(op->provenance));
    }
    if (op->title.len == 0 || op->statement.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision needs both a title and a statement");
    }

    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.session_id = sc->session_id;
    in.repo_id = sc->repo.id;
    in.provenance = atlas_provenance_name(op->provenance);
    in.state = "proposed";
    in.title = atlas_buf_cstr(&op->title);
    in.statement = atlas_buf_cstr(&op->statement);
    in.rationale = buf_or_null(&op->rationale);
    in.dedup_key = buf_or_null(&op->dedup_key);

    bool duplicate = false;
    atlas_status st = atlas_db_ai_decision_insert(db, &in, &out->record_id, &duplicate, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->duplicate = duplicate;
    if (duplicate || out->record_id == 0) {
        return ATLAS_OK;
    }

    path_iter it;
    path_iter_init(&it, &op->paths);
    const char *path = NULL;
    size_t path_len = 0;
    int64_t n = 0;
    while (st == ATLAS_OK && n < ATLAS_AI_MAX_PATHS_PER_RECORD &&
           path_iter_next(&it, &path, &path_len)) {
        atlas_buf path_text = ATLAS_BUF_INIT;
        st = atlas_path_text_encode(path, path_len, &path_text, err);
        if (st == ATLAS_OK) {
            st = atlas_db_ai_decision_path_add(db, out->record_id, path, path_len,
                                               atlas_buf_cstr(&path_text), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_db_ai_evidence_link(db, "decision", out->record_id, sc->repo.id, path,
                                           path_len, err);
        }
        atlas_buf_free(&path_text);
        n++;
    }
    if (st == ATLAS_OK && sc->session_id > 0) {
        st = atlas_db_ai_session_touch(db, sc->session_id, "records", err);
    }
    return st;
}

static atlas_status op_checkpoint(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                  atlas_ai_result *out, atlas_err *err) {
    if (sc->session_id == 0) {
        return ATLAS_OK;
    }
    int64_t total = 0;
    int64_t direct = 0;
    int64_t ambiguous = 0;
    int64_t unresolved = 0;
    atlas_status st = ATLAS_OK;
    if (sc->change_set_id > 0) {
        st = atlas_db_ai_changed_counts(db, sc->change_set_id, &total, &direct, &ambiguous,
                                        &unresolved, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    int64_t proposed = 0;
    int64_t approved = 0;
    int64_t reasons = 0;
    if (sc->repo_found) {
        st = atlas_db_ai_repo_record_counts(db, sc->repo.id, &proposed, &approved, &reasons, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    atlas_ai_session_report rep;
    atlas_ai_session_report_init(&rep);
    st = atlas_db_ai_session_get(db, sc->client_id, atlas_buf_cstr(&op->session_key), &rep, err);
    int64_t repos = rep.repos;
    atlas_ai_session_report_free(&rep);
    if (st != ATLAS_OK) {
        return st;
    }

    bool inserted = false;
    st = atlas_db_ai_checkpoint_insert(
        db, sc->session_id, op->phase == ATLAS_AI_PHASE_PRE_COMPACT ? "pre_compact" : "post_compact",
        repos, total, unresolved, reasons, proposed, buf_or_null(&op->dedup_key), &inserted, err);
    if (st == ATLAS_OK && inserted && op->phase == ATLAS_AI_PHASE_PRE_COMPACT) {
        st = atlas_db_ai_session_touch(db, sc->session_id, "compactions", err);
    }
    out->changed_paths = total;
    out->unresolved_paths = unresolved;
    out->duplicate = !inserted;
    return st;
}

/* Names the paths a turn-closing UNKNOWN record covers.
 *
 * Only the ones with no reason: a path somebody did explain is not part of what
 * was left unexplained, and folding the two together would make the record
 * claim more ignorance than there is. */
typedef struct unresolved_ctx {
    atlas_db *db;
    int64_t reason_id;
    int64_t repo_id;
    int64_t attached;
} unresolved_ctx;

static atlas_status attach_unresolved(const atlas_ai_changed_row *row, void *ud, atlas_err *err) {
    unresolved_ctx *uc = (unresolved_ctx *)ud;
    if (row->has_reason || uc->attached >= ATLAS_AI_MAX_PATHS_PER_RECORD) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_ai_reason_path_add(uc->db, uc->reason_id, row->path_raw,
                                                  row->path_raw_len, row->path_text, err);
    if (st == ATLAS_OK) {
        st = atlas_db_ai_evidence_link(uc->db, "reason", uc->reason_id, uc->repo_id, row->path_raw,
                                       row->path_raw_len, err);
    }
    if (st == ATLAS_OK) {
        uc->attached++;
    }
    return st;
}

/* Closes out a turn: every changed path with no reason of any kind gets an
 * explicit UNKNOWN row.
 *
 * This is the Stop hook's whole job, and it is the reason Stop never blocks.
 * Atlas does not ask the model to explain itself and does not invent an
 * explanation; it records that at the end of this turn, nobody said why these
 * paths changed. That is a queryable fact rather than a silence. */
static atlas_status op_turn_close(atlas_db *db, const atlas_ai_op *op, session_ctx *sc,
                                  atlas_ai_result *out, atlas_err *err) {
    if (sc->session_id == 0 || sc->change_set_id == 0 || !sc->repo_found) {
        return ATLAS_OK;
    }

    /* Re-sweep before counting.
     *
     * The batch that preceded this turn queued a reconciliation it could not
     * wait for, so it attributed against the snapshot as it was *before* the
     * edits. By now the pass has normally published, and running the sweep again
     * is what makes the count below describe this turn rather than the last one.
     * Without it, a single-batch turn reports zero unresolved paths and nothing
     * is ever marked UNKNOWN — which would quietly defeat the one thing this
     * operation exists to do. */
    int64_t concurrent = 0;
    atlas_status st = sweep_changed_paths(db, sc, &concurrent, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->concurrent_sessions = concurrent;

    int64_t total = 0;
    int64_t direct = 0;
    int64_t ambiguous = 0;
    int64_t unresolved = 0;
    st = atlas_db_ai_changed_counts(db, sc->change_set_id, &total, &direct, &ambiguous, &unresolved,
                                    err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->changed_paths = total;
    out->unresolved_paths = unresolved;
    out->direct_paths = direct;
    out->ambiguous_paths = ambiguous;
    if (unresolved == 0) {
        return ATLAS_OK;
    }

    /* One UNKNOWN row per turn, naming the unresolved paths, deduplicated on the
     * turn so a redelivered Stop does not accumulate rows.
     *
     * The turn identifier comes from the caller's own dedup key — for Claude,
     * `turn_close:<prompt_id>` — because that is the only value that is stable
     * across a redelivery and different between two turns. Deriving it from a
     * counter the caller does not send would make every turn after the first
     * collide with the first and record nothing. */
    atlas_buf key = ATLAS_BUF_INIT;
    st = atlas_buf_appendf(&key, err, "turn-unknown:%lld:%s", (long long)sc->change_set_id,
                           op->dedup_key.len > 0 ? atlas_buf_cstr(&op->dedup_key)
                                                 : "0");
    if (st != ATLAS_OK) {
        atlas_buf_free(&key);
        return st;
    }

    atlas_ai_record_input in;
    memset(&in, 0, sizeof(in));
    in.session_id = sc->session_id;
    in.repo_id = sc->repo.id;
    in.change_set_id = sc->change_set_id;
    in.provenance = atlas_provenance_name(ATLAS_PROV_UNKNOWN);
    in.state = "unknown";
    in.confidence = "none";
    in.unknown_reason =
        "the turn ended with no reason recorded for these paths; Atlas does not infer one";
    in.dedup_key = atlas_buf_cstr(&key);

    bool duplicate = false;
    st = atlas_db_ai_reason_insert(db, &in, &out->record_id, &duplicate, err);
    atlas_buf_free(&key);
    if (st != ATLAS_OK || duplicate || out->record_id == 0) {
        out->duplicate = duplicate;
        return st;
    }

    /* Attach the unresolved paths themselves, so the record names what it
     * covers rather than being a bare counter a reader has to trust. */
    unresolved_ctx uc;
    memset(&uc, 0, sizeof(uc));
    uc.db = db;
    uc.reason_id = out->record_id;
    uc.repo_id = sc->repo.id;

    int64_t listed = 0;
    bool more = false;
    st = atlas_db_ai_changed_list(db, sc->change_set_id, ATLAS_AI_MAX_PATHS_PER_RECORD,
                                  attach_unresolved, &uc, &listed, &more, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_db_ai_event_append(db, sc->session_id, sc->repo_id, "turn_close", NULL, NULL, NULL,
                                    0u, NULL, buf_or_null(&op->dedup_key), NULL, err);
}

/* --- entry point --------------------------------------------------------- */

atlas_status atlas_ai_apply(atlas_db *db, const atlas_ai_op *op, atlas_ai_sync_fn sync,
                            void *sync_ud, atlas_ai_result *out, atlas_err *err) {
    session_ctx sc;
    session_ctx_init(&sc);

    atlas_status st = resolve_repo(db, op, &sc.repo, &sc.repo_found, err);
    if (st != ATLAS_OK) {
        session_ctx_free(&sc);
        return st;
    }
    if (sc.repo_found) {
        sc.repo_id = sc.repo.id;
        out->repo_id = sc.repo.id;
        st = atlas_buf_set_str(&out->repo_name, sc.repo.name, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_set(&out->root_text, sc.repo.root_path_text.data,
                               sc.repo.root_path_text.len, err);
        }
        if (st != ATLAS_OK) {
            session_ctx_free(&sc);
            return st;
        }
    }

    /* Session open creates; every other op finds. An operation for a session
     * Atlas never opened is not an error and does not create one — it simply
     * has nothing to attach to, which the caller can see from the result. */
    if (op->kind == ATLAS_AI_OP_SESSION_OPEN) {
        st = op_session_open(db, op, &sc, out, err);
    } else {
        st = find_session(db, op, &sc, out, err);
    }
    if (st != ATLAS_OK) {
        session_ctx_free(&sc);
        return st;
    }
    out->session_id = sc.session_id;

    if (sc.session_id > 0 && sc.repo_found) {
        if (op->kind == ATLAS_AI_OP_SESSION_OPEN || op->kind == ATLAS_AI_OP_ATTACH_ROOT) {
            st = attach_repo(db, op, &sc, out, err);
        } else {
            st = atlas_db_ai_change_set_find(db, sc.session_id, sc.repo.id, &sc.change_set_id, err);
            out->change_set_id = sc.change_set_id;
        }
    }
    if (st != ATLAS_OK) {
        session_ctx_free(&sc);
        return st;
    }

    switch (op->kind) {
    case ATLAS_AI_OP_SESSION_OPEN:
    case ATLAS_AI_OP_ATTACH_ROOT:
        if (sc.session_id > 0) {
            st = atlas_db_ai_event_append(db, sc.session_id, sc.repo_id, "root_attached", NULL,
                                          NULL, NULL, 0u, NULL, NULL, NULL, err);
        }
        break;
    case ATLAS_AI_OP_SESSION_TOUCH:
        if (sc.session_id > 0) {
            st = atlas_db_ai_session_touch(db, sc.session_id, "turns", err);
            if (st == ATLAS_OK) {
                /* The prompt itself is never stored. What is recorded is that a
                 * turn happened, which is metadata about the session rather
                 * than about what the user said. */
                st = atlas_db_ai_event_append(db, sc.session_id, sc.repo_id, "turn", NULL, NULL,
                                              NULL, 0u, NULL, buf_or_null(&op->dedup_key), NULL,
                                              err);
            }
        }
        break;
    case ATLAS_AI_OP_SESSION_CLOSE:
        if (sc.session_id > 0) {
            st = atlas_db_ai_session_close(db, sc.session_id,
                                           op->source.len > 0 ? atlas_buf_cstr(&op->source) : NULL,
                                           err);
        }
        break;
    case ATLAS_AI_OP_SESSION_CHECKPOINT: st = op_checkpoint(db, op, &sc, out, err); break;
    case ATLAS_AI_OP_TOOL_RECORD: st = op_tool_record(db, op, &sc, out, err); break;
    case ATLAS_AI_OP_CORRELATE: st = op_correlate(db, op, &sc, sync, sync_ud, out, err); break;
    case ATLAS_AI_OP_REASON: st = op_reason(db, op, &sc, out, err); break;
    case ATLAS_AI_OP_DECISION: st = op_decision(db, op, &sc, out, err); break;
    case ATLAS_AI_OP_TURN_CLOSE: st = op_turn_close(db, op, &sc, out, err); break;
    }

    session_ctx_free(&sc);
    return st;
}

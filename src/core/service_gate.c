/* Atlas - the service layer for impact gates.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This is where the snapshot is established, and establishing it correctly is
 * most of what this file does.
 *
 * The order is deliberate and is the whole consistency argument:
 *
 *   1. Open a read transaction and read the repository row. SQLite's deferred
 *      transaction takes its snapshot at the first read, so from here on every
 *      query — decisions, links, the commit graph, the structural relations —
 *      sees one database, whatever the daemon commits meanwhile.
 *   2. Ask Git for the live HEAD, *after* the snapshot rather than before.
 *      A commit that lands between the two makes the two disagree, and a
 *      disagreement is BLOCKED. That is the fail-closed direction: the race
 *      costs a refusal, never a pass on a state Atlas has not seen.
 *   3. Assess every decision against that fixed pair.
 *
 * Read the ordering the other way round and the failure is silent rather than
 * loud: Git first, then a snapshot taken after the daemon indexed the commit
 * Git had just reported, and the two agree about a state neither of them
 * measured together.
 *
 * Nothing here takes the writer lock. `atlas gate check` is a read command and
 * a repository being indexed while it runs is a repository that keeps being
 * indexed — the gate has nothing with which to block a pass, which is what
 * makes "normal read-only indexing is never blocked by the gate" a property of
 * the code rather than a promise.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/gate.h"
#include "atlas/git.h"
#include "atlas/pathrep.h"
#include "core/service_internal.h"
#include "gate/gate_internal.h"

/* Mirrors the file index's own currency check. Same shape and same strings as
 * `atlas_server_index_current`, for the reason A3's copy gives: two answers to
 * one question is one answer too many, and this one decides a gate. */
static bool file_index_current(const atlas_index_state *s) {
    return s->present && s->last_complete_generation > 0 && !s->event_gap &&
           !s->pending_full_reconcile && s->watch_state != ATLAS_WATCH_ERROR &&
           s->watch_state != ATLAS_WATCH_DEGRADED;
}

/* --- collecting the decisions --------------------------------------------- */

typedef struct collector {
    atlas_gate_report *rep;
    atlas_db *db;
    const atlas_gate_env *env;
    const atlas_gate_query *q;
    int64_t depth;
    atlas_status st;
    atlas_err *err;
    /* Scope prefixes, decoded to raw bytes once. */
    atlas_buf prefixes[ATLAS_GATE_MAX_SCOPE_PATHS];
    size_t prefix_count;
} collector;

/* Whether a revision's anchors fall inside the caller's target scope.
 *
 * A repository-scoped decision is always in scope: it claims the whole tree, so
 * narrowing the question does not narrow what it covers. Everything else is in
 * scope when any path anchor is at or below one of the prefixes, matched on a
 * path-component boundary so `src` does not select `srcfoo`. */
static atlas_status in_scope(collector *c, int64_t revision_id, bool *out, atlas_err *err) {
    *out = true;
    if (c->prefix_count == 0) {
        return ATLAS_OK;
    }
    atlas_decision_revision rev;
    atlas_decision_revision_init(&rev);
    bool found = false;
    atlas_status st = atlas_db_decision_revision_load(c->db, revision_id, &rev, &found, err);
    if (st != ATLAS_OK || !found) {
        atlas_decision_revision_free(&rev);
        /* A revision that will not load is not filtered out. It is assessed,
         * and the assessment will have something to say about it. */
        return st;
    }
    if (rev.scope == ATLAS_DECISION_SCOPE_REPOSITORY) {
        atlas_decision_revision_free(&rev);
        *out = true;
        return ATLAS_OK;
    }
    *out = false;
    for (size_t i = 0; i < rev.link_count && !*out; i++) {
        const atlas_decision_link *l = &rev.links[i];
        if (l->path_raw.len == 0) {
            continue;
        }
        for (size_t p = 0; p < c->prefix_count; p++) {
            const atlas_buf *pre = &c->prefixes[p];
            if (pre->len == 0) {
                *out = true;
                break;
            }
            if (l->path_raw.len < pre->len ||
                memcmp(l->path_raw.data, pre->data, pre->len) != 0) {
                continue;
            }
            if (l->path_raw.len == pre->len ||
                ((const char *)l->path_raw.data)[pre->len] == '/') {
                *out = true;
                break;
            }
        }
    }
    atlas_decision_revision_free(&rev);
    return ATLAS_OK;
}

static atlas_status on_document(const atlas_decision_doc_row *row, void *ud, atlas_err *err) {
    collector *c = ud;
    if (c->st != ATLAS_OK) {
        return ATLAS_OK;
    }
    if (c->rep->item_count >= ATLAS_GATE_MAX_DECISIONS) {
        /* Never a pass over the ones that fitted. */
        c->rep->limit_reached = true;
        c->rep->limit_detail = "decisions assessed";
        return ATLAS_OK;
    }

    /* Cheapest possible filter first: a uid comparison, before the revision is
     * loaded and long before anything is walked. */
    if (c->q->only_uid != NULL && c->q->only_uid[0] != '\0' &&
        (row->uid == NULL || strcmp(row->uid, c->q->only_uid) != 0)) {
        return ATLAS_OK;
    }

    int64_t revision_id =
        row->current_revision_id != 0 ? row->current_revision_id : row->head_revision_id;
    bool wanted = true;
    c->st = in_scope(c, revision_id, &wanted, err);
    if (c->st != ATLAS_OK) {
        return ATLAS_OK;
    }
    if (!wanted) {
        c->rep->out_of_scope++;
        return ATLAS_OK;
    }

    atlas_gate_assessment *grown =
        realloc(c->rep->items, (c->rep->item_count + 1u) * sizeof(atlas_gate_assessment));
    if (grown == NULL) {
        c->st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory");
        return ATLAS_OK;
    }
    c->rep->items = grown;
    atlas_gate_assessment *a = &c->rep->items[c->rep->item_count];
    atlas_gate_assessment_init(a);
    c->rep->item_count++;

    c->st = atlas_gate_assess(c->db, c->env, row->id, row, c->depth, a, err);
    return ATLAS_OK;
}

/* --- the query -------------------------------------------------------------- */

static atlas_status build_env(atlas_db *db, const atlas_repo_info *info,
                              const atlas_gate_query *q, atlas_gate_env *env, atlas_err *err) {
    env->repo_id = info->id;
    atlas_status st = atlas_buf_set_str(&env->repo_name, info->name, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&env->root_text, info->root_path_text.data, info->root_path_text.len,
                           err);
    }
    if (st == ATLAS_OK) {
        st = atlas_db_repo_identity_hash(db, info->id, &env->repo_identity_hash, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_index_state file_state;
    atlas_index_state_init(&file_state);
    atlas_code_index_state code_state;
    atlas_code_index_state_init(&code_state);
    st = atlas_db_index_state_get(db, info->id, &file_state, err);
    if (st == ATLAS_OK) {
        st = atlas_db_code_state_get(db, info->id, &code_state, err);
    }
    if (st != ATLAS_OK) {
        atlas_index_state_free(&file_state);
        atlas_code_index_state_free(&code_state);
        return st;
    }

    env->file_index_known = info->scanned_head[0] != '\0' || file_state.present;
    bool file_current = file_index_current(&file_state);
    const char *why = NULL;
    bool code_current =
        atlas_code_index_current(&file_state, &code_state, file_current, &why);
    env->code_index_known = code_state.generation > 0;

    (void)snprintf(env->indexed_commit, sizeof env->indexed_commit, "%s", info->scanned_head);

    /* The requested state. Naming one Atlas has not indexed is INDEX_LAG rather
     * than an extrapolation: Atlas can describe the snapshot it holds and
     * nothing else, and answering about a state it has never seen would be
     * inventing one. */
    if (q->at_commit != NULL && q->at_commit[0] != '\0') {
        (void)snprintf(env->requested_commit, sizeof env->requested_commit, "%s", q->at_commit);
        if (strcmp(env->requested_commit, env->indexed_commit) != 0) {
            atlas_gate_env_note(env, ATLAS_GATE_REASON_INDEX_LAG);
        }
    } else {
        (void)snprintf(env->requested_commit, sizeof env->requested_commit, "%s",
                       env->indexed_commit);
    }

    if (!file_current || env->indexed_commit[0] == '\0') {
        atlas_gate_env_note(env, ATLAS_GATE_REASON_INDEX_LAG);
    }
    if (!code_current) {
        atlas_gate_env_note(env, ATLAS_GATE_REASON_STRUCTURAL_INDEX_STALE);
    }

    atlas_index_state_free(&file_state);
    atlas_code_index_state_free(&code_state);

    /* --- and only now, the live repository ---------------------------------
     *
     * After the snapshot. A HEAD that moved between the two is reported as lag
     * and blocks; a HEAD that moved before it was already indexed or was
     * already lag. Either way Atlas never answers about a state it has not
     * measured. Failing to read Git at all is also lag: an unreadable
     * repository is not a current one. */
    atlas_git *g = NULL;
    atlas_err probe;
    atlas_err_init(&probe);
    if (atlas_service_open_repo_git(info, &g, &probe) != ATLAS_OK) {
        atlas_gate_env_note(env, ATLAS_GATE_REASON_INDEX_LAG);
        return ATLAS_OK;
    }
    atlas_git_head head;
    memset(&head, 0, sizeof head);
    if (atlas_git_read_head(g, &head, &probe) != ATLAS_OK) {
        atlas_gate_env_note(env, ATLAS_GATE_REASON_INDEX_LAG);
    } else if (strcmp(head.oid, env->indexed_commit) != 0) {
        atlas_gate_env_note(env, ATLAS_GATE_REASON_INDEX_LAG);
    }
    atlas_git_close(g);
    return ATLAS_OK;
}

/* Ordering: by decision uid. A uid is Atlas-minted lowercase hex with a fixed
 * prefix, so a byte comparison is a total order over it and two runs against
 * one database emit the same document. */
static int by_uid(const void *a, const void *b) {
    const atlas_gate_assessment *x = a;
    const atlas_gate_assessment *y = b;
    return strcmp(atlas_buf_cstr(&x->uid), atlas_buf_cstr(&y->uid));
}

atlas_status atlas_gate_run(atlas_db *db, const atlas_gate_query *q, atlas_gate_report *out,
                            atlas_err *err) {
    if (q->depth < 0 || q->depth > ATLAS_GATE_MAX_IMPACT_DEPTH) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--depth must be between 1 and %d; a depth Atlas silently reduced "
                             "would be a silently smaller answer",
                             ATLAS_GATE_MAX_IMPACT_DEPTH);
    }
    if (q->path_count > ATLAS_GATE_MAX_SCOPE_PATHS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "at most %u --path values are accepted",
                             (unsigned)ATLAS_GATE_MAX_SCOPE_PATHS);
    }
    int64_t depth = q->depth > 0 ? q->depth : ATLAS_GATE_DEFAULT_IMPACT_DEPTH;
    out->depth = depth;

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    atlas_status st = atlas_db_repo_get(db, q->repo_name, &info, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_REPO,
                           "NOT_REGISTERED: no repository named \"%s\" is registered. Repositories are onboarded only by an operator; Atlas does not discover them (try: atlas repo list)",
                           q->repo_name == NULL ? "" : q->repo_name);
    }
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_gate_env env;
    atlas_gate_env_init(&env);
    collector c;
    memset(&c, 0, sizeof c);

    /* One read transaction for everything below. */
    st = atlas_db_begin(db, err);
    if (st != ATLAS_OK) {
        atlas_gate_env_free(&env);
        atlas_repo_info_free(&info);
        return st;
    }

    st = build_env(db, &info, q, &env, err);
    if (st == ATLAS_OK) {
        out->repo_id = env.repo_id;
        st = atlas_buf_set(&out->repo_name, env.repo_name.data, env.repo_name.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->root_text, env.root_text.data, env.root_text.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->repo_identity_hash, env.repo_identity_hash.data,
                           env.repo_identity_hash.len, err);
    }
    if (st == ATLAS_OK) {
        (void)snprintf(out->indexed_commit, sizeof out->indexed_commit, "%s", env.indexed_commit);
        (void)snprintf(out->requested_commit, sizeof out->requested_commit, "%s",
                       env.requested_commit);

        for (size_t i = 0; i < q->path_count && st == ATLAS_OK; i++) {
            atlas_buf_init(&c.prefixes[c.prefix_count]);
            st = atlas_path_text_decode(q->paths[i], strlen(q->paths[i]),
                                   &c.prefixes[c.prefix_count], err);
            c.prefix_count++;
        }
    }

    if (st == ATLAS_OK) {
        c.rep = out;
        c.db = db;
        c.env = &env;
        c.q = q;
        c.depth = depth;
        c.st = ATLAS_OK;
        c.err = err;
        int64_t count = 0;
        bool more = false;
        /* Approved decisions only. A proposal has never been policy, so there
         * is nothing about it that could have gone stale, and blocking on one
         * would let anybody stop a pipeline by proposing something.
         *
         * **A9.1 filters by status and by nothing else, deliberately.** Every
         * knowledge kind is gated the same way: an approved INVARIANT and an
         * approved OPERATIONAL_FACT both have anchors that can move, and Atlas
         * has no basis for deciding that drift in one matters less than drift in
         * the other. A gate that quietly skipped a kind would report a clean
         * assessment of a repository it had only partly assessed. The kind is
         * reported on each item so a reader can weigh it; the engine does not
         * weigh it for them.
         *
         * A RESOLVED record drops out here for free, and correctly: it is no
         * longer effective, so there is nothing left to have gone stale. */
        st = atlas_db_decision_documents_list(db, info.id, "APPROVED", NULL,
                                              ATLAS_GATE_MAX_DECISIONS + 1, on_document, &c,
                                              &count, &more, err);
        /* A single-decision query is not truncated by a ceiling on how many
         * were *listed*: it assessed exactly the one it was asked about. */
        if (q->only_uid != NULL && q->only_uid[0] != '\0') {
            more = false;
        }
        if (st == ATLAS_OK) {
            st = c.st;
        }
        if (st == ATLAS_OK && more) {
            out->limit_reached = true;
            out->limit_detail = "decisions assessed";
        }
    }

    /* The transaction ends here, and every fact in `out` came from inside it. */
    atlas_db_rollback(db);

    for (size_t i = 0; i < c.prefix_count; i++) {
        atlas_buf_free(&c.prefixes[i]);
    }
    atlas_gate_env_free(&env);
    atlas_repo_info_free(&info);
    if (st != ATLAS_OK) {
        return st;
    }

    /* Guarded: a repository with no approved decisions leaves `items` NULL, and
     * qsort's first argument is declared never-null even for a count of zero. */
    if (out->item_count > 1u) {
        qsort(out->items, out->item_count, sizeof out->items[0], by_uid);
    }

    /* PASS is asserted here rather than defaulted, because BLOCKED absorbs in
     * `atlas_gate_fold` and a report that started at its safe default could
     * never be lifted out of it. This is the one line that says "a real report
     * was produced", and it is immediately followed by everything that can take
     * it away again. */
    out->result = ATLAS_GATE_PASS;
    for (size_t i = 0; i < out->item_count; i++) {
        switch (out->items[i].freshness) {
            case ATLAS_GATE_FRESH: out->fresh++; break;
            case ATLAS_GATE_STALE: out->stale++; break;
            case ATLAS_GATE_IMPACTED: out->impacted++; break;
            case ATLAS_GATE_UNKNOWN:
            default: out->unknown++; break;
        }
        out->result = atlas_gate_fold(out->result, out->items[i].freshness);
    }
    /* A limit reached at the report level is the same kind of fact as one
     * reached inside an assessment: the answer is a subset of an answer. */
    if (out->limit_reached) {
        out->result = ATLAS_GATE_BLOCKED;
    }
    return ATLAS_OK;
}

atlas_status atlas_service_gate_check(atlas_ctx *ctx, const atlas_gate_query *q,
                                      atlas_gate_report *out, atlas_err *err) {
    return atlas_gate_run(atlas_ctx_db(ctx), q, out, err);
}

atlas_status atlas_gate_run_one(atlas_db *db, const char *repo, const char *uid,
                                const char *at_commit, atlas_gate_report *out, atlas_err *err) {
    /* One decision, through the same engine and the same snapshot discipline.
     * A separate implementation would be a second answer to the question the
     * gate already answers, and the two would disagree the first time one of
     * them was fixed. */
    atlas_gate_query q;
    atlas_gate_query_init(&q);
    q.repo_name = repo;
    q.at_commit = at_commit;
    q.only_uid = uid;

    atlas_status st = atlas_gate_run(db, &q, out, err);
    if (st != ATLAS_OK) {
        return st;
    }
    return atlas_gate_narrow_to_one(out, uid, err);
}

/* Turns a whole-repository assessment filtered to one decision into the report
 * `gate show` promises: exactly one item or a refusal, with the counts and the
 * verdict derived from that item alone.
 *
 * Shared with the daemon-served form, which asks `gate.check` with the same
 * single-decision filter — so "there is no such approved decision here" is one
 * answer produced in one place, rather than a local error and a remote empty
 * PASS that both look deliberate. */
atlas_status atlas_gate_narrow_to_one(atlas_gate_report *out, const char *uid, atlas_err *err) {
    if (out->item_count != 1u) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "no approved decision \"%s\" is attached to this repository", uid);
    }
    out->fresh = out->stale = out->impacted = out->unknown = 0;
    switch (out->items[0].freshness) {
        case ATLAS_GATE_FRESH: out->fresh = 1; break;
        case ATLAS_GATE_STALE: out->stale = 1; break;
        case ATLAS_GATE_IMPACTED: out->impacted = 1; break;
        case ATLAS_GATE_UNKNOWN:
        default: out->unknown = 1; break;
    }
    out->result = atlas_gate_fold(ATLAS_GATE_PASS, out->items[0].freshness);
    if (out->limit_reached) {
        out->result = ATLAS_GATE_BLOCKED;
    }
    return ATLAS_OK;
}

atlas_status atlas_service_gate_show(atlas_ctx *ctx, const char *repo, const char *uid,
                                     const char *at_commit, atlas_gate_report *out,
                                     atlas_err *err) {
    return atlas_gate_run_one(atlas_ctx_db(ctx), repo, uid, at_commit, out, err);
}

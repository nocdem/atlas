/* Atlas - bounded traversal of the compiler-derived call graph.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One breadth-first walk, in one of two directions, behind every question this
 * layer answers. Callers and callees are depth 1. Transitive reach is the same
 * walk with a larger depth. A trace is the same walk with a target and a
 * recorded predecessor chain.
 *
 * Two properties are load-bearing and neither is optional:
 *
 *   - **A path is as strong as its weakest edge.** `atlas_sem_evidence_weaker`
 *     folds the class along the way, so a chain that crosses one indirect call
 *     is a candidate chain however many proven edges surround it. There is no
 *     other place evidence is assigned to a reached node, which is what stops a
 *     mostly-proven path from being reported as proven.
 *   - **A bound that is reached is reported.** A truncated walk cannot say it
 *     found nothing. Every ceiling sets `truncated` and a fixed reason, and a
 *     caller that ignores them is reading an answer Atlas did not give — A6's
 *     rule about TRAVERSAL_LIMIT, in the shape a call graph wants.
 *
 * The walk takes no lock, writes no row and creates no process. That is what
 * makes "a semantic query never blocks indexing" a property of the code rather
 * than a promise.
 */
#include "atlas/sem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/sem_ops.h"

void atlas_sem_walk_opts_init(atlas_sem_walk_opts *o) {
    memset(o, 0, sizeof(*o));
    o->depth = ATLAS_SEM_DEFAULT_DEPTH;
    o->max_nodes = ATLAS_SEM_MAX_NODES;
    o->max_rows = ATLAS_SEM_MAX_ROWS;
}

const char *atlas_sem_trunc_reason_intern(const char *reason) {
    static const char *const R[] = {
        ATLAS_SEM_TRUNC_DEPTH,
        ATLAS_SEM_TRUNC_NODES,
        ATLAS_SEM_TRUNC_ROWS,
        ATLAS_SEM_TRUNC_TIME,
    };
    if (reason == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        if (strcmp(reason, R[i]) == 0) {
            return R[i];
        }
    }
    return NULL;
}

bool atlas_sem_trunc_reason_is_known(const char *reason) {
    return atlas_sem_trunc_reason_intern(reason) != NULL;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- the frontier ------------------------------------------------------------
 *
 * A flat array of nodes with an index-based predecessor link, rather than a
 * queue of pointers. It is what lets a trace reconstruct a path by walking
 * `parent` backwards without a second structure, and it keeps the whole walk's
 * memory a property of `max_nodes` rather than of the graph. */

typedef struct node {
    char usr[ATLAS_SEM_MAX_USR_BYTES];
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    char kind[32];
    char file[512];
    int64_t line;
    /* The edge that reached this node. */
    char edge_kind[32];
    char site_file[512];
    int64_t site_line;
    int64_t candidate_total;
    /* The weakest evidence on the whole path to here. */
    atlas_sem_evidence evidence;
    int64_t depth;
    int32_t parent; /* index into nodes[]; -1 for the start */
} node;

typedef struct walk_state {
    node *nodes;
    size_t count;
    size_t cap;
    /* Visited set: an open-addressed table of indices into `nodes`. */
    int32_t *slots;
    size_t slot_cap;
} walk_state;

static void walk_free(walk_state *w) {
    free(w->nodes);
    free(w->slots);
    memset(w, 0, sizeof(*w));
}

static uint64_t hash_usr(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

static int32_t find(const walk_state *w, const char *usr) {
    if (w->slot_cap == 0) {
        return -1;
    }
    size_t j = (size_t)(hash_usr(usr) & (w->slot_cap - 1));
    while (w->slots[j] >= 0) {
        if (strcmp(w->nodes[w->slots[j]].usr, usr) == 0) {
            return w->slots[j];
        }
        j = (j + 1) & (w->slot_cap - 1);
    }
    return -1;
}

static bool grow(walk_state *w, size_t want) {
    if (want <= w->cap) {
        return true;
    }
    size_t ncap = w->cap == 0 ? 256 : w->cap * 2;
    while (ncap < want) {
        ncap *= 2;
    }
    node *nn = realloc(w->nodes, ncap * sizeof(*nn));
    if (nn == NULL) {
        return false;
    }
    w->nodes = nn;
    w->cap = ncap;

    size_t scap = ncap * 2;
    int32_t *ns = malloc(scap * sizeof(*ns));
    if (ns == NULL) {
        return false;
    }
    for (size_t i = 0; i < scap; i++) {
        ns[i] = -1;
    }
    free(w->slots);
    w->slots = ns;
    w->slot_cap = scap;
    for (size_t i = 0; i < w->count; i++) {
        size_t j = (size_t)(hash_usr(w->nodes[i].usr) & (scap - 1));
        while (ns[j] >= 0) {
            j = (j + 1) & (scap - 1);
        }
        ns[j] = (int32_t)i;
    }
    return true;
}

/* Adds a node if its USR has not been seen. Returns its index, or -1 when it was
 * already present (cycle detection) or could not be added. */
static int32_t push(walk_state *w, const char *usr) {
    if (find(w, usr) >= 0) {
        return -1;
    }
    if (!grow(w, w->count + 1)) {
        return -1;
    }
    int32_t idx = (int32_t)w->count++;
    node *n = &w->nodes[idx];
    memset(n, 0, sizeof(*n));
    (void)snprintf(n->usr, sizeof n->usr, "%s", usr);
    n->parent = -1;
    n->evidence = ATLAS_SEM_EV_PROVEN;
    size_t j = (size_t)(hash_usr(usr) & (w->slot_cap - 1));
    while (w->slots[j] >= 0) {
        j = (j + 1) & (w->slot_cap - 1);
    }
    w->slots[j] = idx;
    return idx;
}

/* Fills in the start node's own name and location.
 *
 * Every other node is described by the edge that reached it, which carries the
 * peer's name — but nothing reaches the start, so without this its name is
 * empty and every depth-1 row reports being reached "from" nothing. That reads
 * as a missing fact rather than as the start of the walk, which is why it is
 * looked up rather than left blank. */
typedef struct start_sink {
    node *n;
} start_sink;

static atlas_status take_start(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    (void)err;
    start_sink *s = (start_sink *)ud;
    /* The first row is the best one: the symbol query orders definitions before
     * declarations and repository symbols before external ones. */
    if (s->n->name[0] != '\0') {
        return ATLAS_OK;
    }
    (void)snprintf(s->n->name, sizeof s->n->name, "%s", row->name);
    (void)snprintf(s->n->kind, sizeof s->n->kind, "%s", row->kind);
    (void)snprintf(s->n->file, sizeof s->n->file, "%s", row->file_text);
    s->n->line = row->line;
    return ATLAS_OK;
}

static void describe_start(atlas_db *db, int64_t generation_id, walk_state *w) {
    if (w->count == 0) {
        return;
    }
    atlas_err ignored;
    atlas_err_init(&ignored);
    start_sink s = {&w->nodes[0]};
    int64_t total = 0;
    bool trunc = false;
    /* Best effort: a walk whose start cannot be described is still a valid
     * walk, and failing it over a display detail would be the wrong trade. */
    (void)atlas_db_sem_symbols_by_name(db, generation_id, NULL, w->nodes[0].usr, NULL, 4,
                                       take_start, &s, &total, &trunc, &ignored);
}

/* --- expanding one node ------------------------------------------------------ */

typedef struct expand_ctx {
    walk_state *w;
    int32_t from;
    int64_t max_nodes;
    bool proven_only;
    bool node_limit_hit;
    int64_t unresolved_indirect;
} expand_ctx;

static atlas_status on_edge(const atlas_sem_edge_row *row, void *ud, atlas_err *err) {
    (void)err;
    expand_ctx *x = (expand_ctx *)ud;
    walk_state *w = x->w;

    atlas_sem_evidence ev = ATLAS_SEM_EV_UNKNOWN;
    (void)atlas_sem_evidence_parse(row->evidence, &ev);

    /* An indirect call site with no destination is a *hole in the graph*, not
     * an edge to follow. It is counted rather than dropped, because "the walk
     * passed a call whose target Atlas cannot name" is the single most
     * important thing a bounded call graph reports about its own completeness.
     */
    const char *peer = x->w->nodes[x->from].usr;
    const char *target = row->dst_usr;
    /* Which end is the peer depends on the direction, and the caller set that
     * up: `atlas_db_sem_edges_of` returns edges whose matched end is the node
     * being expanded, so the *other* end is what to follow. */
    if (strcmp(row->dst_usr, peer) == 0) {
        target = row->src_usr;
    }
    if (target == NULL || target[0] == '\0') {
        x->unresolved_indirect++;
        return ATLAS_OK;
    }
    if (x->proven_only && ev != ATLAS_SEM_EV_PROVEN) {
        return ATLAS_OK;
    }
    if ((int64_t)w->count >= x->max_nodes) {
        x->node_limit_hit = true;
        return ATLAS_OK;
    }

    int32_t idx = push(w, target);
    if (idx < 0) {
        return ATLAS_OK; /* already visited, or out of memory: either way, stop */
    }
    node *n = &w->nodes[idx];
    n->parent = x->from;
    n->depth = w->nodes[x->from].depth + 1;
    /* The fold. A path is as strong as its weakest edge, and this is the only
     * place a reached node's evidence is decided. */
    n->evidence = atlas_sem_evidence_weaker(w->nodes[x->from].evidence, ev);
    (void)snprintf(n->edge_kind, sizeof n->edge_kind, "%s", row->kind);
    (void)snprintf(n->site_file, sizeof n->site_file, "%s", row->file_text);
    n->site_line = row->line;
    n->candidate_total = row->candidate_total;
    (void)snprintf(n->name, sizeof n->name, "%s", row->peer_name);
    (void)snprintf(n->file, sizeof n->file, "%s", row->peer_file);
    n->line = row->peer_line;
    return ATLAS_OK;
}

/* --- the walk ----------------------------------------------------------------- */

static void tally(atlas_sem_walk_summary *sum, atlas_sem_evidence ev) {
    switch (ev) {
        case ATLAS_SEM_EV_PROVEN:
            sum->proven++;
            break;
        case ATLAS_SEM_EV_CANDIDATE:
            sum->candidate++;
            break;
        case ATLAS_SEM_EV_LEXICAL:
            sum->lexical++;
            break;
        case ATLAS_SEM_EV_UNKNOWN:
        default:
            sum->unknown++;
            break;
    }
}

static void fill_row(const node *n, atlas_sem_walk_row *out, const walk_state *w) {
    memset(out, 0, sizeof(*out));
    out->depth = n->depth;
    out->usr = n->usr;
    out->name = n->name;
    out->kind = n->kind;
    out->file_text = n->file;
    out->line = n->line;
    out->edge_kind = n->edge_kind;
    out->via_usr = n->parent >= 0 ? w->nodes[n->parent].usr : "";
    out->via_name = n->parent >= 0 ? w->nodes[n->parent].name : "";
    out->evidence = atlas_sem_evidence_name(n->evidence);
    out->site_file = n->site_file;
    out->site_line = n->site_line;
    out->candidate_total = n->candidate_total;
}

atlas_status atlas_sem_walk(atlas_db *db, int64_t generation_id, const atlas_sem_walk_opts *opts,
                            atlas_sem_walk_cb cb, void *ud, atlas_sem_walk_summary *sum,
                            atlas_err *err) {
    if (db == NULL || opts == NULL || opts->usr == NULL || sum == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic walk: bad request");
    }
    memset(sum, 0, sizeof(*sum));

    int64_t depth = opts->depth > 0 ? opts->depth : ATLAS_SEM_DEFAULT_DEPTH;
    if (depth > ATLAS_SEM_MAX_DEPTH) {
        depth = ATLAS_SEM_MAX_DEPTH;
    }
    int64_t max_nodes = opts->max_nodes > 0 ? opts->max_nodes : ATLAS_SEM_MAX_NODES;
    if (max_nodes > ATLAS_SEM_MAX_NODES) {
        max_nodes = ATLAS_SEM_MAX_NODES;
    }
    int64_t max_rows = opts->max_rows > 0 ? opts->max_rows : ATLAS_SEM_MAX_ROWS;
    if (max_rows > ATLAS_SEM_MAX_ROWS) {
        max_rows = ATLAS_SEM_MAX_ROWS;
    }

    walk_state w;
    memset(&w, 0, sizeof(w));
    if (push(&w, opts->usr) < 0) {
        walk_free(&w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic walk: out of memory");
    }
    describe_start(db, generation_id, &w);

    int64_t deadline = now_ms() + ATLAS_SEM_QUERY_TIMEOUT_MS;
    atlas_status st = ATLAS_OK;
    size_t cursor = 0;
    bool depth_hit = false;
    bool node_hit = false;
    bool time_hit = false;

    while (cursor < w.count && st == ATLAS_OK) {
        node *cur = &w.nodes[cursor];
        int64_t cur_depth = cur->depth;
        cursor++;

        if (cur_depth >= depth) {
            /* Not an error and not silence: the frontier stopped here and the
             * summary says so, so a caller knows the answer is a horizon rather
             * than a boundary of the graph. */
            depth_hit = true;
            continue;
        }
        if (now_ms() > deadline) {
            time_hit = true;
            break;
        }

        expand_ctx x;
        memset(&x, 0, sizeof(x));
        x.w = &w;
        x.from = (int32_t)(cursor - 1);
        x.max_nodes = max_nodes;
        x.proven_only = opts->proven_only;

        int64_t total = 0;
        bool trunc = false;
        st = atlas_db_sem_edges_of(db, generation_id, cur->usr, opts->inbound, NULL, true,
                                   ATLAS_SEM_MAX_ROWS, on_edge, &x, &total, &trunc, err);
        sum->unresolved_indirect += x.unresolved_indirect;
        if (x.node_limit_hit || trunc) {
            node_hit = true;
        }
        sum->visited++;
    }

    /* Emit everything but the start node, in breadth-first order — which is
     * deterministic because the edge query is ordered and the frontier is a
     * flat array appended in that order. */
    for (size_t i = 1; i < w.count && st == ATLAS_OK; i++) {
        if (sum->emitted >= max_rows) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_ROWS;
            break;
        }
        atlas_sem_walk_row row;
        fill_row(&w.nodes[i], &row, &w);
        if (cb != NULL) {
            st = cb(&row, ud, err);
            if (st != ATLAS_OK) {
                break;
            }
        }
        sum->emitted++;
        tally(sum, w.nodes[i].evidence);
        if (w.nodes[i].depth > sum->max_depth_reached) {
            sum->max_depth_reached = w.nodes[i].depth;
        }
    }

    /* Report the strongest reason first: running out of time or nodes says less
     * about the graph than reaching the requested depth does. */
    if (!sum->truncated) {
        if (time_hit) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_TIME;
        } else if (node_hit) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_NODES;
        } else if (depth_hit) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_DEPTH;
        }
    }

    walk_free(&w);
    return st;
}

/* --- tracing a path ------------------------------------------------------------ */

atlas_status atlas_sem_trace(atlas_db *db, int64_t generation_id, const char *from_usr,
                             const char *to_usr, int64_t depth, int64_t max_paths,
                             atlas_sem_walk_cb cb, void *ud, atlas_sem_walk_summary *sum,
                             atlas_err *err) {
    if (db == NULL || from_usr == NULL || to_usr == NULL || sum == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic trace: bad request");
    }
    memset(sum, 0, sizeof(*sum));
    if (max_paths <= 0 || max_paths > ATLAS_SEM_MAX_PATHS) {
        max_paths = ATLAS_SEM_MAX_PATHS;
    }
    if (depth <= 0) {
        depth = ATLAS_SEM_DEFAULT_DEPTH;
    }
    if (depth > ATLAS_SEM_MAX_DEPTH) {
        depth = ATLAS_SEM_MAX_DEPTH;
    }

    /* Breadth-first from the source, stopping at the target.
     *
     * Breadth-first means the first path found is a shortest one, which is the
     * answer somebody asking "how does A reach B" almost always wants. Because
     * the visited set admits each USR once, this returns one path per target
     * rather than an enumeration — and it says so rather than implying the path
     * it found is the only one. */
    walk_state w;
    memset(&w, 0, sizeof(w));
    if (push(&w, from_usr) < 0) {
        walk_free(&w);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic trace: out of memory");
    }
    describe_start(db, generation_id, &w);

    int64_t deadline = now_ms() + ATLAS_SEM_QUERY_TIMEOUT_MS;
    atlas_status st = ATLAS_OK;
    size_t cursor = 0;
    int32_t hit = -1;

    while (cursor < w.count && st == ATLAS_OK && hit < 0) {
        node *cur = &w.nodes[cursor];
        int64_t cur_depth = cur->depth;
        cursor++;
        if (cur_depth >= depth) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_DEPTH;
            continue;
        }
        if (now_ms() > deadline) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_TIME;
            break;
        }

        expand_ctx x;
        memset(&x, 0, sizeof(x));
        x.w = &w;
        x.from = (int32_t)(cursor - 1);
        x.max_nodes = ATLAS_SEM_MAX_NODES;

        int64_t total = 0;
        bool trunc = false;
        st = atlas_db_sem_edges_of(db, generation_id, cur->usr, false, NULL, true,
                                   ATLAS_SEM_MAX_ROWS, on_edge, &x, &total, &trunc, err);
        sum->unresolved_indirect += x.unresolved_indirect;
        sum->visited++;
        if (x.node_limit_hit || trunc) {
            sum->truncated = true;
            sum->truncated_reason = ATLAS_SEM_TRUNC_NODES;
        }
        hit = find(&w, to_usr);
    }

    if (hit > 0 && st == ATLAS_OK) {
        /* Walk the predecessor chain back to the source, then emit forwards so
         * the caller reads the path in the direction the calls go. */
        int32_t chain[ATLAS_SEM_MAX_DEPTH + 2];
        size_t n = 0;
        for (int32_t i = hit; i >= 0 && n < sizeof(chain) / sizeof(chain[0]);
             i = w.nodes[i].parent) {
            chain[n++] = i;
        }
        for (size_t i = n; i > 0 && st == ATLAS_OK; i--) {
            int32_t idx = chain[i - 1];
            if (idx == 0) {
                continue; /* the source itself is where the caller started */
            }
            atlas_sem_walk_row row;
            fill_row(&w.nodes[idx], &row, &w);
            if (cb != NULL) {
                st = cb(&row, ud, err);
            }
            if (st == ATLAS_OK) {
                sum->emitted++;
                tally(sum, w.nodes[idx].evidence);
            }
        }
        sum->max_depth_reached = w.nodes[hit].depth;
    }

    walk_free(&w);
    return st;
}

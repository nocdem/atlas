/* Atlas - bounded, deterministic traversal of the structural graph.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * "What does this depend on" and "what may be affected if I change this" are the
 * same breadth-first walk over the same edges in opposite directions, so there
 * is one implementation. Two would answer differently the first time somebody
 * fixed a bug in only one of them.
 *
 * Four properties, all of them load-bearing:
 *
 *   - **Bounded.** Depth is clamped to ATLAS_CODE_MAX_TRAVERSAL_DEPTH and the
 *     node count to ATLAS_CODE_MAX_TRAVERSAL_NODES. Reaching either sets
 *     `truncated` with a reason; nothing is ever silently cut short.
 *   - **Cycle-safe.** A visited set keyed on (node kind, node id). Include
 *     cycles are ordinary in C and a walk that did not expect one would not
 *     terminate.
 *   - **Deterministic.** The frontier is expanded in the order the edge queries
 *     return, which is ordered by resolution and then by path bytes. The same
 *     graph gives the same answer whatever order it was built in — which is what
 *     makes the result reproducible across worker scheduling.
 *   - **Explained.** Every reached node carries the edge kind that reached it,
 *     the node it was reached from, and the **weakest** resolution on the whole
 *     path. A node reached through one ambiguous edge is an ambiguous candidate
 *     however exact the rest of the chain was, because a chain is only as strong
 *     as its weakest link and reporting otherwise would overstate it.
 */
#include "atlas/code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"

/* Bytes of a node's label kept in the walk. A path or an identifier longer than
 * this is reported truncated in the label only; the node id is exact, so a
 * caller that needs the full name asks for the node. */
#define WALK_LABEL_MAX 320u

typedef struct walk_node {
    int32_t kind; /* atlas_code_node_kind */
    int64_t id;
    int64_t depth;
    int32_t resolution; /* the weakest on the path to here */
    char label[WALK_LABEL_MAX];
    char edge_kind[40];
    char detail[64];
    int32_t via; /* index into nodes[]; -1 for the start */
} walk_node;

typedef struct walk_state {
    atlas_db *db;
    int64_t repo_id;
    walk_node *nodes;
    size_t count;
    size_t cap;
    size_t max_nodes;
    int64_t max_depth;
    bool inbound;
    bool truncated;
    /* The node currently being expanded, so the edge callback knows what it is
     * descending from without threading it through the query layer. */
    size_t current;
} walk_state;

void atlas_code_walk_opts_init(atlas_code_walk_opts *o) {
    memset(o, 0, sizeof(*o));
    o->start_kind = ATLAS_CODE_NODE_FILE;
    o->depth = ATLAS_CODE_DEFAULT_TRAVERSAL_DEPTH;
    o->max_nodes = ATLAS_CODE_MAX_TRAVERSAL_NODES;
    o->follow_files = true;
    o->follow_symbols = true;
}

/* The weaker of two resolution classes, ordered by how much they claim.
 *
 * Not the enum order: that is a vocabulary, not a ranking. This is the ranking,
 * written once, so "the weakest link on the path" is a single comparison rather
 * than a rule each call site remembers differently. */
static int strength(atlas_code_resolution r) {
    switch (r) {
    case ATLAS_CODE_RES_SOURCE_EXACT: return 5;
    case ATLAS_CODE_RES_BUILD_METADATA: return 4;
    case ATLAS_CODE_RES_UNIQUE_LEXICAL: return 3;
    case ATLAS_CODE_RES_CONDITIONAL: return 2;
    case ATLAS_CODE_RES_AMBIGUOUS: return 1;
    case ATLAS_CODE_RES_UNRESOLVED:
    case ATLAS_CODE_RES_MODEL_PROPOSAL:
    case ATLAS_CODE_RES_UNKNOWN:
    default: return 0;
    }
}

static atlas_code_resolution weaker(atlas_code_resolution a, atlas_code_resolution b) {
    return strength(a) <= strength(b) ? a : b;
}

static bool visited(const walk_state *w, atlas_code_node_kind kind, int64_t id) {
    for (size_t i = 0; i < w->count; i++) {
        if (w->nodes[i].kind == (int32_t)kind && w->nodes[i].id == id) {
            return true;
        }
    }
    return false;
}

static atlas_status push_node(walk_state *w, atlas_code_node_kind kind, int64_t id, int64_t depth,
                              atlas_code_resolution res, const char *label, const char *edge_kind,
                              const char *detail, int32_t via, atlas_err *err) {
    if (w->count >= w->max_nodes) {
        w->truncated = true;
        return ATLAS_OK;
    }
    if (w->count == w->cap) {
        size_t next = w->cap == 0 ? 128u : w->cap * 2u;
        if (next > w->max_nodes) {
            next = w->max_nodes;
        }
        walk_node *grown = realloc(w->nodes, next * sizeof(*grown));
        if (grown == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory traversing the graph");
        }
        w->nodes = grown;
        w->cap = next;
    }
    walk_node *n = &w->nodes[w->count];
    memset(n, 0, sizeof(*n));
    n->kind = (int32_t)kind;
    n->id = id;
    n->depth = depth;
    n->resolution = (int32_t)res;
    n->via = via;
    (void)snprintf(n->label, sizeof(n->label), "%s", label != NULL ? label : "");
    (void)snprintf(n->edge_kind, sizeof(n->edge_kind), "%s", edge_kind != NULL ? edge_kind : "");
    (void)snprintf(n->detail, sizeof(n->detail), "%s", detail != NULL ? detail : "");
    w->count++;
    return ATLAS_OK;
}

/* One edge out of, or into, the node being expanded. */
static atlas_status on_edge(const atlas_code_edge_row *row, void *ud, atlas_err *err) {
    walk_state *w = (walk_state *)ud;
    const walk_node *from = &w->nodes[w->current];

    /* Which end of the edge is the neighbour depends on the direction, and
     * nothing else does. That is the whole reason inbound and outbound share one
     * implementation. */
    const char *kind_name = w->inbound ? row->src_kind : row->dst_kind;
    int64_t id = w->inbound ? row->src_id : row->dst_id;
    const char *label = w->inbound ? row->src_path_text : row->dst_path_text;
    if (label == NULL) {
        label = row->dst_name_text;
    }
    if (id <= 0 || kind_name == NULL) {
        return ATLAS_OK; /* an unresolved endpoint is not a node to walk to */
    }
    atlas_code_node_kind kind = ATLAS_CODE_NODE_FILE;
    if (strcmp(kind_name, "symbol") == 0) {
        kind = ATLAS_CODE_NODE_SYMBOL;
    } else if (strcmp(kind_name, "unit") == 0) {
        kind = ATLAS_CODE_NODE_UNIT;
    } else if (strcmp(kind_name, "file") != 0) {
        return ATLAS_OK;
    }
    if (visited(w, kind, id)) {
        /* Cycle detection, and the reason a repository with mutually including
         * headers terminates rather than spinning. */
        return ATLAS_OK;
    }
    atlas_code_resolution edge_res = ATLAS_CODE_RES_UNKNOWN;
    if (!atlas_code_resolution_parse(row->resolution, &edge_res)) {
        edge_res = ATLAS_CODE_RES_UNKNOWN;
    }
    atlas_code_resolution path_res =
        weaker((atlas_code_resolution)from->resolution, edge_res);
    return push_node(w, kind, id, from->depth + 1, path_res, label, row->kind, row->detail,
                     (int32_t)w->current, err);
}

atlas_status atlas_code_walk(atlas_db *db, int64_t repo_id, const atlas_code_walk_opts *opts,
                             atlas_code_walk_cb cb, void *ud, atlas_code_walk_summary *sum,
                             atlas_err *err) {
    atlas_code_walk_opts defaults;
    atlas_code_walk_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }
    memset(sum, 0, sizeof(*sum));

    walk_state w;
    memset(&w, 0, sizeof(w));
    w.db = db;
    w.repo_id = repo_id;
    w.inbound = opts->inbound;
    w.max_depth = opts->depth <= 0 ? ATLAS_CODE_DEFAULT_TRAVERSAL_DEPTH : opts->depth;
    if (w.max_depth > ATLAS_CODE_MAX_TRAVERSAL_DEPTH) {
        w.max_depth = ATLAS_CODE_MAX_TRAVERSAL_DEPTH;
    }
    w.max_nodes = opts->max_nodes <= 0 ? (size_t)ATLAS_CODE_MAX_TRAVERSAL_NODES
                                       : (size_t)opts->max_nodes;
    if (w.max_nodes > (size_t)ATLAS_CODE_MAX_TRAVERSAL_NODES) {
        w.max_nodes = (size_t)ATLAS_CODE_MAX_TRAVERSAL_NODES;
    }

    /* The start node is at depth zero with nothing weakening its path yet. */
    atlas_status st = push_node(&w, opts->start_kind, opts->start_id, 0,
                                ATLAS_CODE_RES_SOURCE_EXACT, "", "", NULL, -1, err);

    static const char *const FILE_KINDS[] = {"file_depends_on_file", NULL};
    static const char *const SYMBOL_KINDS[] = {"symbol_calls_symbol", NULL};

    for (size_t i = 0; st == ATLAS_OK && i < w.count; i++) {
        if (w.nodes[i].depth >= w.max_depth) {
            continue;
        }
        w.current = i;
        atlas_code_node_kind kind = (atlas_code_node_kind)w.nodes[i].kind;
        const char *const *kinds = NULL;
        if (kind == ATLAS_CODE_NODE_FILE && opts->follow_files) {
            kinds = FILE_KINDS;
        } else if (kind == ATLAS_CODE_NODE_SYMBOL && opts->follow_symbols) {
            kinds = SYMBOL_KINDS;
        }
        if (kinds == NULL) {
            continue;
        }
        for (size_t k = 0; st == ATLAS_OK && kinds[k] != NULL; k++) {
            int64_t n = 0;
            bool more = false;
            if (w.inbound) {
                st = atlas_db_code_edges_to(db, repo_id, atlas_code_node_kind_name(kind),
                                            w.nodes[i].id, kinds[k], ATLAS_CODE_MAX_ROWS, on_edge,
                                            &w, &n, &more, err);
            } else {
                st = atlas_db_code_edges_from(db, repo_id, atlas_code_node_kind_name(kind),
                                              w.nodes[i].id, kinds[k], ATLAS_CODE_MAX_ROWS,
                                              on_edge, &w, &n, &more, err);
            }
            if (more) {
                w.truncated = true;
            }
        }
    }

    sum->visited = (int64_t)w.count;
    sum->truncated = w.truncated;
    if (w.truncated) {
        sum->truncated_reason = "the traversal reached its node or page ceiling; narrow the depth "
                                "or paginate";
    }

    /* Emitted in the order the frontier was built, which is breadth-first and,
     * within a level, ordered by the edge query's own stable ordering. The start
     * node is skipped: a caller asked what this reaches, not whether it reaches
     * itself. */
    for (size_t i = 1; st == ATLAS_OK && i < w.count; i++) {
        const walk_node *n = &w.nodes[i];
        switch ((atlas_code_resolution)n->resolution) {
        case ATLAS_CODE_RES_SOURCE_EXACT:
        case ATLAS_CODE_RES_BUILD_METADATA: sum->exact++; break;
        case ATLAS_CODE_RES_UNIQUE_LEXICAL: sum->unique_lexical++; break;
        case ATLAS_CODE_RES_AMBIGUOUS: sum->ambiguous++; break;
        /* CONDITIONAL sits with the unresolved rather than with the exact: a
         * path through a branch Atlas did not evaluate is a path it cannot
         * claim. */
        case ATLAS_CODE_RES_CONDITIONAL:
        case ATLAS_CODE_RES_UNRESOLVED:
        case ATLAS_CODE_RES_MODEL_PROPOSAL:
        case ATLAS_CODE_RES_UNKNOWN:
        default: sum->unresolved++; break;
        }
        if (cb == NULL) {
            sum->emitted++;
            continue;
        }
        atlas_code_walk_row row;
        memset(&row, 0, sizeof(row));
        row.depth = n->depth;
        row.node_kind = atlas_code_node_kind_name((atlas_code_node_kind)n->kind);
        row.node_id = n->id;
        row.label = n->label;
        row.edge_kind = n->edge_kind;
        row.via_label = (n->via >= 0) ? w.nodes[n->via].label : "";
        row.resolution = atlas_code_resolution_name((atlas_code_resolution)n->resolution);
        row.detail = n->detail[0] != '\0' ? n->detail : NULL;
        st = cb(&row, ud, err);
        if (st == ATLAS_OK) {
            sum->emitted++;
        }
    }

    free(w.nodes);
    return st;
}

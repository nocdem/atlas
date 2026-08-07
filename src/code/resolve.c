/* Atlas - deterministic resolution of structural relations.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The extractor says what the bytes contain. This file says what those things
 * refer to — and, much more often, that it cannot say.
 *
 * Three properties are the whole design:
 *
 *   - **Nothing is chosen arbitrarily.** Two files called `config.h`, or two
 *     external definitions of `init`, produce AMBIGUOUS with the candidate set
 *     recorded. Picking one would be indistinguishable from being right.
 *   - **Every outcome carries how it was reached.** BUILD_METADATA when a
 *     validated compile-database include directory resolved it, UNIQUE_LEXICAL
 *     when exactly one name matched, UNRESOLVED with a typed reason when none
 *     did. The reasons come from a fixed vocabulary, because they reach a
 *     model's context.
 *   - **The order is stable.** Every candidate query is ordered by raw path
 *     bytes and then by id, so a repository resolves identically however the
 *     worker threads happened to interleave. There is a test that asserts it.
 *
 * C linkage is respected rather than approximated: an internal-linkage
 * definition is a candidate only inside its own file, which is what keeps two
 * files' `static void helper(void)` distinct. That rule lives in the SQL
 * (`atlas_db_code_symbols_named`) so no caller can forget it.
 */
#include "atlas/code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/pathrep.h"

/* --- the candidate set -------------------------------------------------------
 *
 * Bounded on the way in. `total` counts every match the query returned, so
 * `candidate_count` reports the true ambiguity even when more candidates existed
 * than are kept — a ceiling must never make an ambiguity look smaller than it
 * is. */

typedef struct candidate {
    int64_t id;
    int64_t code_file_id;
    bool is_definition;
    char kind[24];
    char linkage[16];
} candidate;

typedef struct candidate_set {
    candidate items[ATLAS_CODE_MAX_CANDIDATES];
    size_t count;
    int64_t total;
    /* Set when at least one match was a definition, so "declared but never
     * defined" is distinguishable from "never mentioned". */
    bool any_definition;
    bool any_macro;
    bool any_function;
} candidate_set;

static atlas_status collect(const atlas_code_match_row *row, void *ud, atlas_err *err) {
    candidate_set *cs = (candidate_set *)ud;
    (void)err;
    cs->total++;
    if (row->is_definition) {
        cs->any_definition = true;
    }
    if (row->kind != NULL) {
        if (strcmp(row->kind, "macro") == 0 || strcmp(row->kind, "macro_function") == 0) {
            cs->any_macro = true;
        } else if (strcmp(row->kind, "function") == 0) {
            cs->any_function = true;
        }
    }
    if (cs->count >= ATLAS_CODE_MAX_CANDIDATES) {
        return ATLAS_OK;
    }
    candidate *c = &cs->items[cs->count];
    memset(c, 0, sizeof(*c));
    c->id = row->id;
    c->code_file_id = row->code_file_id;
    c->is_definition = row->is_definition;
    (void)snprintf(c->kind, sizeof(c->kind), "%s", row->kind != NULL ? row->kind : "");
    (void)snprintf(c->linkage, sizeof(c->linkage), "%s", row->linkage != NULL ? row->linkage : "");
    cs->count++;
    return ATLAS_OK;
}

/* Writes one resolution outcome, replacing any candidate set the edge had. */
static atlas_status settle(atlas_db *db, int64_t relation_id, int64_t prior_candidates,
                           atlas_code_node_kind dst_kind, int64_t dst_id,
                           atlas_code_resolution res, atlas_code_provenance prov,
                           const candidate_set *cs, const char *why, int64_t generation,
                           atlas_code_pass_summary *sum, atlas_err *err) {
    /* The one place the A3 restriction is checked on the write path. A class the
     * indexer may not produce cannot get here through a caller's mistake. */
    if (!atlas_code_resolution_writable_in_a3(res)) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the structural indexer may not record the resolution class %s",
                             atlas_code_resolution_name(res));
    }
    /* A reason that is not one of Atlas' own becomes nothing rather than being
     * reproduced: this value reaches a model. */
    const char *detail = (why != NULL && atlas_code_why_is_known(why)) ? why : NULL;

    /* Clearing a candidate set that cannot exist is a delete per edge, and on a
     * first pass every edge in the repository is in that state. `candidate_count`
     * is written by this function beside the rows themselves and zeroed only by
     * the targeted invalidation, which deletes the rows in the same statement —
     * so zero here provably means there is nothing to delete. */
    atlas_status st = ATLAS_OK;
    if (prior_candidates > 0) {
        st = atlas_db_code_candidates_clear(db, relation_id, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    int64_t total = cs != NULL ? cs->total : 0;
    st = atlas_db_code_relation_resolve(db, relation_id, atlas_code_node_kind_name(dst_kind),
                                        dst_id, atlas_code_resolution_name(res),
                                        atlas_code_provenance_name(prov), total, detail, generation,
                                        err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (res == ATLAS_CODE_RES_AMBIGUOUS && cs != NULL) {
        for (size_t i = 0; st == ATLAS_OK && i < cs->count; i++) {
            st = atlas_db_code_candidate_add(db, relation_id,
                                             dst_kind == ATLAS_CODE_NODE_FILE ? "file" : "symbol",
                                             cs->items[i].id, (int64_t)i, cs->items[i].kind, err);
        }
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (sum != NULL) {
        if (atlas_code_resolution_is_resolved(res)) {
            sum->relations_resolved++;
        } else if (res == ATLAS_CODE_RES_AMBIGUOUS) {
            sum->relations_ambiguous++;
        } else if (res == ATLAS_CODE_RES_UNRESOLVED) {
            sum->relations_unresolved++;
        }
    }
    return ATLAS_OK;
}

/* --- include resolution ------------------------------------------------------- */

typedef struct resolve_ctx {
    atlas_db *db;
    int64_t repo_id;
    int64_t generation;
    atlas_code_pass_summary *sum;
    /* Reused across edges so one allocation serves the whole sweep. */
    atlas_buf scratch;
    atlas_buf scratch2;
    /* The repository-wide set of build include directories, computed once.
     *
     * A header is compiled by no translation unit, so its includes fall back to
     * the whole repository's directories — and that set is identical for every
     * header in a pass. Recomputing it per edge means a DISTINCT-and-sort over
     * every unit's directories thousands of times, which grows with the square
     * of the repository. Once per pass is the same answer. */
    atlas_buf repo_dirs;
    bool repo_dirs_ready;
    /* The directories of the last translation unit looked at, memoised.
     *
     * Edges are swept in id order, which is insertion order, which groups a
     * file's includes together — so this hits for every include of a file after
     * its first. One remembered entry is enough because the access pattern is
     * sequential, and a larger cache would only add bookkeeping. */
    int64_t unit_dirs_file_id;
    atlas_buf unit_dirs;
} resolve_ctx;

/* The directory part of a repository-relative path, or an empty range for a
 * file at the root. */
static void dirname_of(const void *path, size_t len, size_t *out_len) {
    const char *p = (const char *)path;
    size_t cut = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '/') {
            cut = i;
        }
    }
    *out_len = cut;
}

/* Tries one exact repository path. */
static atlas_status try_exact(resolve_ctx *rc, const void *path, size_t len, candidate_set *cs,
                              atlas_err *err) {
    memset(cs, 0, sizeof(*cs));
    if (len == 0) {
        return ATLAS_OK;
    }
    /* Refused rather than tried: a spelling with a `..` in it, or an absolute
     * one, must never become a lookup key. Nothing here opens a file, but a
     * path that is not repository-relative is not a repository path and
     * matching one would be a category error. */
    atlas_err ignore;
    atlas_err_init(&ignore);
    if (atlas_path_check_relative(path, len, &ignore) != ATLAS_OK) {
        return ATLAS_OK;
    }
    int64_t n = 0;
    return atlas_db_code_files_matching(rc->db, rc->repo_id, path, len, true, 2, collect, cs, &n,
                                        err);
}

/* The include directories a build was configured with, as raw bytes.
 *
 * Collected into one NUL-separated arena rather than as ids, because unlike
 * every other candidate query this one answers with paths. External directories
 * never reach here: the query excludes them, because Atlas does not read outside
 * the repository and offering one as a resolution would be offering something
 * that must never happen. */
typedef struct dir_set {
    atlas_buf arena;
    size_t count;
} dir_set;

static atlas_status collect_dir(const atlas_code_match_row *row, void *ud, atlas_err *err) {
    dir_set *ds = (dir_set *)ud;
    if (ds->count >= (size_t)ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_buf_append(&ds->arena, row->path_raw, row->path_raw_len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&ds->arena, '\0', err);
    }
    if (st == ATLAS_OK) {
        ds->count++;
    }
    return st;
}

/* Accumulates distinct file matches across several search directories. */
typedef struct file_hits {
    int64_t ids[ATLAS_CODE_MAX_CANDIDATES];
    size_t count;
    int64_t total;
} file_hits;

static void hits_add(file_hits *h, int64_t id) {
    for (size_t i = 0; i < h->count; i++) {
        if (h->ids[i] == id) {
            return;
        }
    }
    h->total++;
    if (h->count < ATLAS_CODE_MAX_CANDIDATES) {
        h->ids[h->count++] = id;
    }
}

/* Turns accumulated hits into the candidate shape `settle` records. */
static void hits_to_set(const file_hits *h, candidate_set *cs) {
    memset(cs, 0, sizeof(*cs));
    cs->total = h->total;
    for (size_t i = 0; i < h->count; i++) {
        cs->items[i].id = h->ids[i];
        cs->count++;
    }
}

static atlas_status resolve_include(const atlas_code_pending_row *row, void *ud, atlas_err *err) {
    resolve_ctx *rc = (resolve_ctx *)ud;
    if (row->dst_name == NULL || row->dst_name_len == 0) {
        return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_UNRESOLVED,
                      ATLAS_CODE_PROV_SOURCE, NULL, ATLAS_CODE_WHY_NOT_IN_REPO, rc->generation,
                      rc->sum, err);
    }
    const char *spell = (const char *)row->dst_name;
    size_t spell_len = row->dst_name_len;
    bool quoted = row->spelling_form == NULL || strcmp(row->spelling_form, "quote") == 0;

    candidate_set cs;
    atlas_status st = ATLAS_OK;

    /* 1. Relative to the including file's own directory — for the quoted form
     *    only.
     *
     *    This is the first thing every C compiler does for `"x.h"` and the only
     *    thing it does *not* do for `<x.h>`, which is why the spelling form is
     *    carried on the edge. It is also the only step that consults nothing but
     *    the repository's own layout, so it is the only one that can honestly be
     *    SOURCE_EXACT. */
    if (quoted) {
        size_t dir_len = 0;
        dirname_of(row->owner_path_raw, row->owner_path_len, &dir_len);
        atlas_buf_reset(&rc->scratch);
        if (dir_len > 0) {
            st = atlas_buf_append(&rc->scratch, row->owner_path_raw, dir_len, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_append_ch(&rc->scratch, '/', err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append(&rc->scratch, spell, spell_len, err);
        }
        if (st == ATLAS_OK) {
            st = try_exact(rc, rc->scratch.data, rc->scratch.len, &cs, err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        if (cs.total == 1) {
            return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_FILE, cs.items[0].id,
                          ATLAS_CODE_RES_SOURCE_EXACT, ATLAS_CODE_PROV_SOURCE, &cs, NULL,
                          rc->generation, rc->sum, err);
        }
    }

    /* 2. Through the include directories the build was configured with.
     *
     *    A match here came from a validated compile-database record, so it is
     *    BUILD_METADATA: a stronger claim than a bare name match, and a weaker
     *    one than reading the bytes.
     *
     *    Several directories yielding different files is genuinely ambiguous
     *    from where Atlas stands. A compiler resolves it by search order; the
     *    stored directories are deduplicated across every unit, so that order is
     *    not recoverable and pretending otherwise would be inventing one. */
    {
        dir_set ds;
        memset(&ds, 0, sizeof(ds));
        atlas_buf_init(&ds.arena);
        /* A translation unit's own directories when the includer is one; for a
         * header, which no unit compiles, the repository-wide set — computed
         * once per pass rather than once per edge. */
        atlas_code_language lang =
            atlas_code_language_of(row->owner_path_raw, row->owner_path_len);
        bool own_unit = (lang == ATLAS_CODE_LANG_C);
        const atlas_buf *dirs = &ds.arena;
        if (own_unit) {
            if (rc->unit_dirs_file_id != row->owner_file_id) {
                st = atlas_db_code_unit_dirs_for_file(rc->db, rc->repo_id, row->owner_path_raw,
                                                      row->owner_path_len,
                                                      ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT,
                                                      collect_dir, &ds, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_set(&rc->unit_dirs, ds.arena.data, ds.arena.len, err);
                }
                if (st == ATLAS_OK) {
                    rc->unit_dirs_file_id = row->owner_file_id;
                }
            }
            dirs = &rc->unit_dirs;
        } else {
            if (!rc->repo_dirs_ready) {
                dir_set all;
                memset(&all, 0, sizeof(all));
                atlas_buf_init(&all.arena);
                st = atlas_db_code_unit_dirs_for_file(rc->db, rc->repo_id, NULL, 0,
                                                      ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT,
                                                      collect_dir, &all, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_set(&rc->repo_dirs, all.arena.data, all.arena.len, err);
                }
                atlas_buf_free(&all.arena);
                rc->repo_dirs_ready = (st == ATLAS_OK);
            }
            dirs = &rc->repo_dirs;
        }
        file_hits hits;
        memset(&hits, 0, sizeof(hits));
        size_t off = 0;
        while (st == ATLAS_OK && off < dirs->len) {
            size_t dlen = strlen(dirs->data + off);
            atlas_buf_reset(&rc->scratch);
            if (dlen > 0) {
                st = atlas_buf_append(&rc->scratch, dirs->data + off, dlen, err);
                if (st == ATLAS_OK) {
                    st = atlas_buf_append_ch(&rc->scratch, '/', err);
                }
            }
            if (st == ATLAS_OK) {
                st = atlas_buf_append(&rc->scratch, spell, spell_len, err);
            }
            if (st == ATLAS_OK) {
                st = try_exact(rc, rc->scratch.data, rc->scratch.len, &cs, err);
            }
            for (size_t i = 0; st == ATLAS_OK && i < cs.count; i++) {
                hits_add(&hits, cs.items[i].id);
            }
            off += dlen + 1u;
        }
        atlas_buf_free(&ds.arena);
        if (st != ATLAS_OK) {
            return st;
        }
        if (hits.total == 1) {
            candidate_set out;
            hits_to_set(&hits, &out);
            return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_FILE, hits.ids[0],
                          ATLAS_CODE_RES_BUILD_METADATA, ATLAS_CODE_PROV_BUILD_METADATA, &out, NULL,
                          rc->generation, rc->sum, err);
        }
        if (hits.total > 1) {
            candidate_set out;
            hits_to_set(&hits, &out);
            return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_AMBIGUOUS,
                          ATLAS_CODE_PROV_BUILD_METADATA, &out, ATLAS_CODE_WHY_MANY_FILES,
                          rc->generation, rc->sum, err);
        }
    }

    /* 3. A repository-wide match on the trailing components. `#include
     *    "atlas/buf.h"` finding `include/atlas/buf.h` is the common case in
     *    every project with an include directory, and it is a name match — so
     *    UNIQUE_LEXICAL when exactly one file matches, AMBIGUOUS when several
     *    do, and never anything stronger. */
    memset(&cs, 0, sizeof(cs));
    int64_t n = 0;
    st = atlas_db_code_files_matching(rc->db, rc->repo_id, spell, spell_len, false,
                                      ATLAS_CODE_MAX_CANDIDATES + 1, collect, &cs, &n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (cs.total == 1) {
        return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_FILE, cs.items[0].id,
                      ATLAS_CODE_RES_UNIQUE_LEXICAL, ATLAS_CODE_PROV_SOURCE, &cs, NULL,
                      rc->generation, rc->sum, err);
    }
    if (cs.total > 1) {
        return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_AMBIGUOUS,
                      ATLAS_CODE_PROV_SOURCE, &cs, ATLAS_CODE_WHY_MANY_FILES, rc->generation,
                      rc->sum, err);
    }

    /* Nothing in the repository. For an angle include that is the ordinary case
     * — it is a system header — and Atlas says so rather than treating a libc
     * header as a missing file. */
    return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_UNRESOLVED,
                  ATLAS_CODE_PROV_SOURCE, NULL,
                  quoted ? ATLAS_CODE_WHY_NOT_IN_REPO : ATLAS_CODE_WHY_SYSTEM_HEADER,
                  rc->generation, rc->sum, err);
}

/* --- symbol resolution --------------------------------------------------------- */

/* Resolves one call candidate or declaration link.
 *
 * The candidate query already applies C linkage: an internal-linkage symbol is
 * returned only for its own file, and a same-file match sorts first. What is
 * left here is the counting, and the counting is the honest part. */
static atlas_status resolve_symbol_edge(resolve_ctx *rc, const atlas_code_pending_row *row,
                                        bool definitions_only, const char *empty_why,
                                        atlas_err *err) {
    if (row->dst_name == NULL || row->dst_name_len == 0) {
        return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_UNRESOLVED,
                      ATLAS_CODE_PROV_SOURCE, NULL, ATLAS_CODE_WHY_INDIRECT, rc->generation,
                      rc->sum, err);
    }
    candidate_set cs;
    memset(&cs, 0, sizeof(cs));
    int64_t n = 0;
    /* Unordered first. Ordering candidates costs a temporary B-tree per lookup
     * and there is nothing to order until there are two of them — which, on a
     * repository where almost every call resolves uniquely or not at all, is
     * almost never. */
    atlas_status st = atlas_db_code_symbols_named(rc->db, rc->repo_id, row->dst_name,
                                                  row->dst_name_len, definitions_only,
                                                  row->owner_file_id,
                                                  ATLAS_CODE_MAX_CANDIDATES + 1, false, collect,
                                                  &cs, &n, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (cs.total > 1) {
        /* Ambiguous, so the order is reported and has to be the documented one.
         * Ask again, ordered — the same query this always ran, now only on the
         * path where its answer is observable. */
        memset(&cs, 0, sizeof(cs));
        n = 0;
        st = atlas_db_code_symbols_named(rc->db, rc->repo_id, row->dst_name, row->dst_name_len,
                                         definitions_only, row->owner_file_id,
                                         ATLAS_CODE_MAX_CANDIDATES + 1, true, collect, &cs, &n,
                                         err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (cs.total == 0) {
        /* Nothing named this. For a call that usually means a library function,
         * and Atlas says "no definition found" rather than inventing one. */
        return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_UNRESOLVED,
                      ATLAS_CODE_PROV_SOURCE, NULL, empty_why, rc->generation, rc->sum, err);
    }
    if (cs.total == 1) {
        return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_SYMBOL, cs.items[0].id,
                      ATLAS_CODE_RES_UNIQUE_LEXICAL, ATLAS_CODE_PROV_SOURCE, &cs, NULL,
                      rc->generation, rc->sum, err);
    }
    /* Several. A macro and a function sharing a name is a specific, common and
     * genuinely different kind of ambiguity, so it gets its own reason. */
    const char *why = (cs.any_macro && cs.any_function) ? ATLAS_CODE_WHY_MACRO_AND_FUNCTION
                                                        : ATLAS_CODE_WHY_MANY_DEFINITIONS;
    return settle(rc->db, row->id, row->candidate_count, ATLAS_CODE_NODE_UNRESOLVED, 0, ATLAS_CODE_RES_AMBIGUOUS,
                  ATLAS_CODE_PROV_SOURCE, &cs, why, rc->generation, rc->sum, err);
}

static atlas_status resolve_call(const atlas_code_pending_row *row, void *ud, atlas_err *err) {
    resolve_ctx *rc = (resolve_ctx *)ud;
    return resolve_symbol_edge(rc, row, true, ATLAS_CODE_WHY_NO_DEFINITION, err);
}

static atlas_status resolve_defined_by(const atlas_code_pending_row *row, void *ud,
                                       atlas_err *err) {
    resolve_ctx *rc = (resolve_ctx *)ud;
    return resolve_symbol_edge(rc, row, true, ATLAS_CODE_WHY_DECL_ONLY, err);
}

/* --- derived dependency edges --------------------------------------------------
 *
 * `file_depends_on_file` is materialised rather than computed at query time.
 *
 * Traversal is the reason. Impact and reverse-dependency queries walk this edge
 * breadth-first with a node cap and a depth cap, and doing that over a union of
 * two differently-shaped queries per step would be both slower and harder to
 * bound. One materialised edge kind makes inbound and outbound the same indexed
 * lookup — which is what keeps the p95 target reachable at two hundred thousand
 * relations.
 *
 * It is provenance INFERENCE and says which edge kind produced it, so it never
 * reads as something Atlas observed directly. */

typedef struct depends_ctx {
    atlas_db *db;
    int64_t repo_id;
    int64_t generation;
    int64_t owner_file_id;
    atlas_code_pass_summary *sum;
    /* Destinations already recorded for this owner, so the same dependency
     * arising from twelve includes produces one edge. Bounded; past the ceiling
     * the pass reports truncation rather than growing without limit. */
    int64_t seen[512];
    size_t seen_count;
    bool truncated;
} depends_ctx;

static bool depends_seen(depends_ctx *dc, int64_t id) {
    for (size_t i = 0; i < dc->seen_count; i++) {
        if (dc->seen[i] == id) {
            return true;
        }
    }
    if (dc->seen_count < sizeof(dc->seen) / sizeof(dc->seen[0])) {
        dc->seen[dc->seen_count++] = id;
        return false;
    }
    dc->truncated = true;
    return true;
}

static atlas_status add_depends(const atlas_code_edge_row *row, void *ud, atlas_err *err) {
    depends_ctx *dc = (depends_ctx *)ud;
    if (row->dst_id <= 0 || strcmp(row->dst_kind, "file") != 0) {
        return ATLAS_OK;
    }
    if (row->dst_id == dc->owner_file_id) {
        return ATLAS_OK; /* a file does not depend on itself */
    }
    if (depends_seen(dc, row->dst_id)) {
        return ATLAS_OK;
    }
    atlas_code_relation_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.owner_file_id = dc->owner_file_id;
    rec.kind = atlas_code_rel_kind_name(ATLAS_CODE_REL_FILE_DEPENDS_ON_FILE);
    rec.src_kind = "file";
    rec.src_id = dc->owner_file_id;
    rec.dst_kind = "file";
    rec.dst_id = row->dst_id;
    /* The weakest contributing resolution, never a stronger one: a dependency
     * inferred from a UNIQUE_LEXICAL include is itself only lexical. */
    rec.resolution = row->resolution;
    rec.provenance = atlas_code_provenance_name(ATLAS_CODE_PROV_INFERENCE);
    rec.detail = ATLAS_CODE_WHY_DERIVED_INCLUDE;
    rec.generation = dc->generation;
    int64_t id = 0;
    atlas_status st = atlas_db_code_relation_insert(dc->db, dc->repo_id, &rec, &id, err);
    if (st == ATLAS_OK && dc->sum != NULL) {
        dc->sum->relations_written++;
    }
    return st;
}

/* Rebuilds one file's derived dependency edges from its resolved includes. */
static atlas_status rebuild_depends(atlas_db *db, int64_t repo_id, int64_t code_file_id,
                                    int64_t generation, atlas_code_pass_summary *sum,
                                    atlas_err *err) {
    atlas_status st = atlas_db_code_depends_clear(db, code_file_id, err);
    if (st != ATLAS_OK) {
        return st;
    }
    depends_ctx dc;
    memset(&dc, 0, sizeof(dc));
    dc.db = db;
    dc.repo_id = repo_id;
    dc.generation = generation;
    dc.owner_file_id = code_file_id;
    dc.sum = sum;
    int64_t count = 0;
    bool more = false;
    st = atlas_db_code_edges_from(db, repo_id, "file", code_file_id, "file_includes_file",
                                  ATLAS_CODE_MAX_ROWS, add_depends, &dc, &count, &more, err);
    if (st == ATLAS_OK && (dc.truncated || more) && sum != NULL) {
        sum->truncated = true;
        sum->truncated_reason = "a file has more resolved includes than one dependency rebuild "
                                "records";
    }
    return st;
}

/* --- invalidation --------------------------------------------------------------
 *
 * After a file is reparsed or removed, every edge that pointed at one of its
 * rows now points at nothing. Finding those by a left join is exact and cannot
 * miss one; guessing which edges *might* have pointed there could. */

typedef struct dangling_ctx {
    atlas_db *db;
    int64_t generation;
    atlas_code_pass_summary *sum;
    int64_t count;
} dangling_ctx;

static atlas_status unsettle(const atlas_code_pending_row *row, void *ud, atlas_err *err) {
    dangling_ctx *dc = (dangling_ctx *)ud;
    dc->count++;
    /* Back to unresolved, then re-resolved by the sweep below. Two steps rather
     * than one because the sweep is the only place that knows the rules, and
     * two implementations of "what does this name mean" would eventually
     * disagree. */
    /* The candidate rows go with the resolution they belonged to. `settle` reads
     * `candidate_count` to decide whether an edge has any, and this sets it to
     * zero — so leaving them would strand rows nothing would look at again. */
    if (row->candidate_count > 0) {
        atlas_status st = atlas_db_code_candidates_clear(dc->db, row->id, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return atlas_db_code_relation_resolve(dc->db, row->id, "unresolved", 0,
                                          atlas_code_resolution_name(ATLAS_CODE_RES_UNRESOLVED),
                                          atlas_code_provenance_name(ATLAS_CODE_PROV_SOURCE), 0,
                                          NULL, dc->generation, err);
}

/* --- the sweep ------------------------------------------------------------------ */

typedef struct sweep_target {
    const char *kind;
    atlas_code_pending_cb fn;
} sweep_target;

/* --- bounded transactions ------------------------------------------------------
 *
 * Resolution rewrites hundreds of thousands of rows on a first pass, and Atlas'
 * rule is that a transaction is never held across unbounded work: a long-held
 * write lock makes every reader see stale data, and the memory the uncommitted
 * pages occupy grows with the repository rather than with a constant.
 *
 * So each sweep is paged. A chunk of edges is fetched, resolved and committed,
 * and the next chunk starts after the last id. The cursor is on `r.id`, which is
 * stable under the updates the sweep performs, so a resumed sweep never revisits
 * a row and never skips one.
 *
 * A statement may not be left open across a commit, which is why this is a page
 * loop rather than a streaming callback with a commit inside it. */

typedef struct txn_batch {
    atlas_db *db;
    bool open;
} txn_batch;

static atlas_status txn_begin(txn_batch *b, atlas_err *err) {
    if (b->open) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_db_begin(b->db, err);
    b->open = (st == ATLAS_OK);
    return st;
}

static atlas_status txn_commit(txn_batch *b, atlas_err *err) {
    if (!b->open) {
        return ATLAS_OK;
    }
    b->open = false;
    return atlas_db_commit(b->db, err);
}

static void txn_abort(txn_batch *b) {
    if (b->open) {
        atlas_db_rollback(b->db);
        b->open = false;
    }
}

/* Runs one sweep to exhaustion, a bounded transaction per page. */
static atlas_status sweep_paged(txn_batch *b, int64_t repo_id, const char *kind, int64_t owner,
                                atlas_code_sweep mode, const void *name, size_t name_len,
                                atlas_code_pending_cb fn, void *ud, atlas_err *err) {
    int64_t cursor = 0;
    for (;;) {
        atlas_status st = txn_begin(b, err);
        if (st != ATLAS_OK) {
            return st;
        }
        int64_t n = 0;
        int64_t next = cursor;
        st = atlas_db_code_relations_pending(b->db, repo_id, kind, owner, mode, name, name_len,
                                             cursor, ATLAS_DB_BATCH_MAX, fn, ud, &n, &next, err);
        if (st != ATLAS_OK) {
            txn_abort(b);
            return st;
        }
        st = txn_commit(b, err);
        if (st != ATLAS_OK) {
            return st;
        }
        if (n == 0 || next <= cursor) {
            return ATLAS_OK;
        }
        cursor = next;
    }
}

/* Files whose dependency edges have to be rebuilt, collected during the sweep so
 * the rebuild does not happen inside the iteration over the same table. */
typedef struct touched_files {
    int64_t *ids;
    size_t count;
    size_t cap;
    bool truncated;
} touched_files;

static bool touched_add(touched_files *tf, int64_t id) {
    for (size_t i = 0; i < tf->count; i++) {
        if (tf->ids[i] == id) {
            return true;
        }
    }
    if (tf->count == tf->cap) {
        size_t next = tf->cap == 0 ? 64u : tf->cap * 2u;
        if (next > (size_t)ATLAS_CODE_MAX_PARSE_FILES_PER_PASS) {
            next = (size_t)ATLAS_CODE_MAX_PARSE_FILES_PER_PASS;
        }
        if (next <= tf->count) {
            tf->truncated = true;
            return false;
        }
        int64_t *grown = realloc(tf->ids, next * sizeof(*grown));
        if (grown == NULL) {
            tf->truncated = true;
            return false;
        }
        tf->ids = grown;
        tf->cap = next;
    }
    tf->ids[tf->count++] = id;
    return true;
}

typedef struct include_sweep_ctx {
    resolve_ctx *rc;
    touched_files *touched;
} include_sweep_ctx;

static atlas_status resolve_include_and_note(const atlas_code_pending_row *row, void *ud,
                                             atlas_err *err) {
    include_sweep_ctx *isc = (include_sweep_ctx *)ud;
    atlas_status st = resolve_include(row, isc->rc, err);
    if (st == ATLAS_OK) {
        (void)touched_add(isc->touched, row->owner_file_id);
    }
    return st;
}

atlas_status atlas_code_resolve(atlas_db *db, int64_t repo_id, int64_t generation,
                                const atlas_code_resolve_scope *scope,
                                atlas_code_pass_summary *sum, atlas_err *err) {
    atlas_code_resolve_scope empty;
    memset(&empty, 0, sizeof(empty));
    if (scope == NULL) {
        scope = &empty;
    }
    resolve_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.db = db;
    rc.repo_id = repo_id;
    rc.generation = generation;
    rc.sum = sum;
    atlas_buf_init(&rc.scratch);
    atlas_buf_init(&rc.scratch2);
    atlas_buf_init(&rc.repo_dirs);
    atlas_buf_init(&rc.unit_dirs);

    txn_batch batch;
    memset(&batch, 0, sizeof(batch));
    batch.db = db;

    touched_files touched;
    memset(&touched, 0, sizeof(touched));

    /* A name set past the ceiling stops being a restriction and becomes a slow
     * way of saying "everything", so past it the pass says so and sweeps the
     * repository once instead of once per name. */
    int64_t names = 0;
    for (size_t i = 0; i < scope->names_len; i++) {
        if (scope->names[i] == '\0') {
            names++;
        }
    }
    bool full = scope->full || names > ATLAS_CODE_MAX_RESOLVE_NAMES;
    if (full && !scope->full && sum != NULL) {
        sum->resolve_fallback = true;
    }

    /* 1. Invalidate — the repository-wide sweep, and only on the full path.
     *
     *    An edge's destination disappears when the file that owned it was
     *    reparsed or removed, and the writer already unsettled those edges
     *    exactly, by seeking from the ids it was about to delete
     *    (`atlas_db_code_relations_unsettle_for_file`). The incremental path
     *    therefore has nothing left to find here, and this query is a left join
     *    over every relation in the repository — a scan that costs the same
     *    whether it finds one row or none.
     *
     *    It stays for the full path, where a scan is proportionate and where the
     *    scope is by definition not trusted to be exact. Belt and braces, at the
     *    one moment braces are affordable. */
    dangling_ctx dc;
    memset(&dc, 0, sizeof(dc));
    dc.db = db;
    dc.generation = generation;
    dc.sum = sum;
    int64_t n = 0;
    atlas_status st = ATLAS_OK;
    if (full) {
        st = txn_begin(&batch, err);
        if (st == ATLAS_OK) {
            st = atlas_db_code_relations_dangling(db, repo_id, unsettle, &dc, &n, err);
        }
        if (st == ATLAS_OK) {
            st = txn_commit(&batch, err);
        }
    }

    /* 2. Includes.
     *
     *    An include resolves against three things: the including file's own
     *    directory, the build's include directories, and the set of paths in the
     *    repository. None of them changes when an existing file's *contents*
     *    change. So a previously unresolvable include can only become resolvable
     *    when a path appeared or left — and on a pass where none did, the sweep
     *    is restricted to the files this pass parsed, whose include edges were
     *    just rewritten unresolved and have to be settled regardless.
     *
     *    Without that restriction every pass re-attempted every `<stdio.h>` in
     *    the repository: tens of thousands of edges that are unresolvable for a
     *    reason that has not changed and will not. */
    include_sweep_ctx isc = {&rc, &touched};
    if (st == ATLAS_OK) {
        if (full || scope->file_set_changed) {
            st = sweep_paged(&batch, repo_id, "file_includes_file", 0,
                             full ? ATLAS_CODE_SWEEP_ALL : ATLAS_CODE_SWEEP_UNSETTLED, NULL, 0,
                             resolve_include_and_note, &isc, err);
        } else {
            for (size_t i = 0; st == ATLAS_OK && i < scope->file_count; i++) {
                st = sweep_paged(&batch, repo_id, "file_includes_file", scope->files[i],
                                 ATLAS_CODE_SWEEP_UNSETTLED, NULL, 0, resolve_include_and_note,
                                 &isc, err);
            }
        }
    }

    /* 3. Calls and declaration links.
     *
     *    A call resolves by the name it mentions and by nothing else, so the two
     *    sets that can have a different answer than last time are exactly:
     *    edges naming a definition that appeared or vanished, found by an
     *    indexed seek on `dst_name`; and edges owned by a file this pass parsed,
     *    which were rewritten unresolved. Both are bounded by the change rather
     *    than by the repository, which is what makes a one-file edit cost one
     *    file's worth of resolution.
     *
     *    The by-name sweep looks at settled edges too, deliberately: adding a
     *    second definition of a name must take the certainty away from every
     *    UNIQUE_LEXICAL edge that pointed at the first one. It can afford to,
     *    because the name is an index seek — each edge is visited by the sweep
     *    for its own name and by no other. */
    static const sweep_target SYMBOL_SWEEPS[] = {
        {"symbol_calls_symbol", resolve_call},
        {"symbol_defined_by", resolve_defined_by},
    };

    for (size_t k = 0; st == ATLAS_OK && k < sizeof(SYMBOL_SWEEPS) / sizeof(SYMBOL_SWEEPS[0]);
         k++) {
        if (full) {
            st = sweep_paged(&batch, repo_id, SYMBOL_SWEEPS[k].kind, 0, ATLAS_CODE_SWEEP_ALL, NULL,
                             0, SYMBOL_SWEEPS[k].fn, &rc, err);
            continue;
        }
        size_t start = 0;
        for (size_t i = 0; st == ATLAS_OK && i <= scope->names_len; i++) {
            if (i == scope->names_len || scope->names[i] == '\0') {
                size_t nlen = i - start;
                if (nlen > 0) {
                    st = sweep_paged(&batch, repo_id, SYMBOL_SWEEPS[k].kind, 0,
                                     ATLAS_CODE_SWEEP_ALL, scope->names + start, nlen,
                                     SYMBOL_SWEEPS[k].fn, &rc, err);
                }
                start = i + 1u;
            }
        }
        for (size_t i = 0; st == ATLAS_OK && i < scope->file_count; i++) {
            st = sweep_paged(&batch, repo_id, SYMBOL_SWEEPS[k].kind, scope->files[i],
                             ATLAS_CODE_SWEEP_UNSETTLED, NULL, 0, SYMBOL_SWEEPS[k].fn, &rc, err);
        }
    }

    /* 4. Derived dependency edges, for every file whose includes were touched.
     *    Rebuilt after the include sweep rather than during it, so the rebuild
     *    reads resolutions that are already final. */
    for (size_t i = 0; st == ATLAS_OK && i < touched.count; i++) {
        st = txn_begin(&batch, err);
        if (st == ATLAS_OK) {
            st = rebuild_depends(db, repo_id, touched.ids[i], generation, sum, err);
        }
        /* Committed in bounded groups for the same reason the sweeps are: the
         * loop is as long as the repository, and a transaction is never held
         * across unbounded work. */
        if (st == ATLAS_OK && ((i + 1u) % (size_t)ATLAS_DB_BATCH_MAX) == 0) {
            st = txn_commit(&batch, err);
        }
    }
    if (st == ATLAS_OK) {
        st = txn_commit(&batch, err);
    } else {
        txn_abort(&batch);
    }
    if (touched.truncated && sum != NULL) {
        sum->truncated = true;
        sum->truncated_reason = "more files needed dependency rebuilding than one pass records";
    }

    free(touched.ids);
    atlas_buf_free(&rc.scratch);
    atlas_buf_free(&rc.scratch2);
    atlas_buf_free(&rc.repo_dirs);
    atlas_buf_free(&rc.unit_dirs);
    return st;
}

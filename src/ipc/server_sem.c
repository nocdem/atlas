/* Atlas - the A8-CI method group: compiler-derived semantic reads.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * **Every method in this file is a read.** There is no method here that builds
 * an index, invalidates one, clears a cache or changes a lifecycle state, and
 * that is the guarantee rather than an omission: a model reaching every name
 * Atlas exposes over the socket still cannot cause a compiler to run.
 *
 * Index construction is deliberately *not* here. It creates processes, takes
 * minutes and must run on the writer thread, so it belongs to the operator-uid
 * group beside the decision-lifecycle methods — selected by `SO_PEERCRED` and
 * by nothing else, exactly as A8 selects its two orchestration groups. A peer
 * that is not the uid the root-owned policy names gets `unknown method`, the
 * same answer as for a name that does not exist: a refusal distinguishing "you
 * may not" from "there is no such thing" tells a caller what to try next.
 *
 * The reads run on a per-request read-only connection, which is what makes
 * "a semantic query never blocks indexing" true of the code rather than
 * promised: this file has nothing with which to block it.
 */
#include <string.h>

#include "atlas/sem_discover.h"
#include "atlas/sem.h"
#include "atlas/sem_ops.h"
#include "atlas/service.h"
#include "ipc/server_internal.h"


/* A9.2.3. Defined near the bottom, beside the full writer it shares its fields
 * with. Declared here because `sem.status` needs it before that point. */
static atlas_status write_sem_plan_fields(dispatch_state *ds, const atlas_sem_status_report *rep,
                                          atlas_err *err);
/* --- resolving what to answer from ------------------------------------------- */

/* The published generation and its freshness, written into the response before
 * any result is.
 *
 * Freshness first, always. A caller that read the rows without it could not
 * tell an answer about the current code from one about code that has since
 * changed, and those must never be indistinguishable. */
static atlas_status open_generation(dispatch_state *ds, atlas_repo_info *info,
                                    atlas_sem_generation *gen, bool *found, atlas_err *err) {
    atlas_sem_generation_init(gen);
    atlas_status st = atlas_db_sem_current(ds->db, info->id, gen, found, err);
    if (st != ATLAS_OK) {
        return st;
    }

    atlas_sem_generation latest;
    bool have_latest = false;
    st = atlas_db_sem_latest(ds->db, info->id, &latest, &have_latest, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool running = have_latest && latest.status == ATLAS_SEM_GEN_RUNNING;

    /* A9.2.3. One implementation of the freshness question, shared with the
     * local CLI path: the daemon schedules a rebuild by noticing that a
     * generation is stale, so a check that never fires is a repository that
     * never rebuilds — and a check that fires on one surface and not the other
     * is two answers to one question. */
    const char *reason = NULL;
    atlas_sem_freshness f = atlas_sem_freshness_now(ds->db, info, gen, *found, running, &reason);

    st = atlas_json_key_str(ds->j, "repo", info->name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "freshness", atlas_sem_freshness_name(f), err);
    }
    if (st == ATLAS_OK) {
        /* Checked against Atlas' own closed set before it crosses the socket,
         * so a value from anywhere else becomes absent rather than reproduced. */
        st = atlas_json_key_str_opt(ds->j, "stale_reason",
                                    atlas_sem_stale_reason_is_known(reason) ? reason : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "have_generation", *found, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation_id", gen->id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "indexed_commit", gen->commit_id, err);
    }
    return st;
}

/* Shared preamble for the query methods: resolve the repository from the
 * registry, then open the generation. An absent index is a typed refusal, not
 * an empty result — "nobody has indexed this" and "this has no symbols" are
 * different answers. */
static atlas_status begin(dispatch_state *ds, const atlas_ipc_request *req, atlas_repo_info *info,
                          atlas_sem_generation *gen, atlas_err *err) {
    atlas_repo_info_init(info);
    atlas_status st = atlas_server_require_repo(ds, req, info, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool found = false;
    st = open_generation(ds, info, gen, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no semantic index exists for this repository");
    }
    return ATLAS_OK;
}

/* --- sem.status --------------------------------------------------------------- */

typedef struct unit_sink {
    dispatch_state *ds;
    atlas_status st;
} unit_sink;

static atlas_status write_unit(const atlas_sem_unit_report *row, void *ud, atlas_err *err) {
    unit_sink *s = (unit_sink *)ud;
    atlas_status st = atlas_json_obj_begin(s->ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(s->ds->j, "source", atlas_safe(&s->ds->safe, row->source_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(s->ds->j, "status", row->status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(s->ds->j, "why",
                                    atlas_sem_why_is_known(row->why) ? row->why : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(s->ds->j, "diagnostics_errors", row->diagnostics_errors, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(s->ds->j, err);
    }
    return st;
}

static atlas_status method_sem_status(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    /* Whether this Atlas can build an index at all. Reported rather than
     * discovered by a caller when indexing silently produces nothing. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "libclang_available", atlas_sem_available(), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "compiler_version", atlas_sem_compiler_version(), err);
    }
    /* The live compiler's id, beside its version. Sent because it was not: the
     * local path filled it from `atlas_sem_compiler_id()` and the socket path
     * left it empty, so on a system deployment `code sem-status` printed a
     * version with no compiler. The same parity defect as `started_at`, found
     * the same way — by reading the installed system's own output. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "compiler_id", atlas_sem_compiler_id(), err);
    }

    /* A9.2.3: one computation for the whole response.
     *
     * `atlas_sem_status_on` resolves the repository, computes the plan and reads
     * the published generation, and the plan already carries the freshness — so
     * calling `open_generation` here as well would hash every declared
     * compilation database a second time within one response. Measured against a
     * repository with two databases totalling 485 KiB, the redundant work was
     * most of the command's cost; and two computations within one document could
     * disagree if the tree moved between them. */
    atlas_sem_status_report rep;
    atlas_sem_status_report_init(&rep);
    if (st == ATLAS_OK) {
        st = atlas_sem_status_on(ds->db, info.name, &rep, err);
    }
    const atlas_sem_generation gen = rep.generation;
    const bool found = rep.have_generation;

    /* Freshness first, always. A caller that read the rows without it could not
     * tell an answer about the current code from one about code that has since
     * changed, and those must never be indistinguishable. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", info.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "freshness", atlas_sem_freshness_name(rep.freshness), err);
    }
    if (st == ATLAS_OK) {
        /* Checked against Atlas' own closed set before it crosses the socket,
         * so a value from anywhere else becomes absent rather than reproduced. */
        st = atlas_json_key_str_opt(
            ds->j, "stale_reason",
            atlas_sem_stale_reason_is_known(rep.stale_reason) ? rep.stale_reason : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "have_generation", found, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation_id", gen.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "indexed_commit", gen.commit_id, err);
    }

    /* The derived state and build description, **before** the early return
     * for a repository with no generation.
     *
     * That return used to end the response, so a repository Atlas had never
     * indexed reported no state at all over the socket — no activity, no build
     * description, nothing. Which is exactly the case where an operator most
     * needs it: they have just configured the repository and want to know that
     * Atlas took it and that a build is due. `atlas code sem-config` against a
     * fresh repository printed `state UNKNOWN` and `not configured` moments
     * after a write the daemon had accepted, because the write's own read-back
     * went through this method. */
    if (st == ATLAS_OK) {
        st = write_sem_plan_fields(ds, &rep, err);
    }

    if (st != ATLAS_OK || !found) {
        atlas_sem_status_report_free(&rep);
        atlas_repo_info_free(&info);
        return st;
    }

    /* The counts, kept apart. Complete, partial, failed and unsupported are
     * never summed: an index describing nine tenths of a repository must not be
     * reported the way one describing all of it is. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "compdb_digest", gen.compdb_digest, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "compdb_count", gen.compdb_count, err);
    }
    /* A9.2.3. Both of these existed on the generation, were stored, and were
     * never sent — so on a system deployment, where the socket is the only path
     * to the index, `atlas code sem-status` reported an empty compiler id and no
     * start time for a generation that recorded both. That is the A9.2.1 closure
     * defect one layer over: a field added to the struct and to the renderer but
     * not to the wire makes the two surfaces disagree, and only the local one is
     * ever tested by hand. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "generation_compiler_id", gen.compiler_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "generation_compiler_version", gen.compiler_version,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "started_at", gen.started_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "completed_at", gen.completed_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "analyzer_id", gen.analyzer_id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "analyzer_version", gen.analyzer_version, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_total", gen.tu_total, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_complete", gen.tu_complete, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_partial", gen.tu_partial, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_failed", gen.tu_failed, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_unsupported", gen.tu_unsupported, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "symbols", gen.symbol_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "edges", gen.edge_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "includes", gen.include_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "duration_ms", gen.duration_ms, err);
    }
    /* The true number of units that are not COMPLETE, from the generation's own
     * tallies rather than from how many rows the list below returns. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "units_not_complete",
                                gen.tu_partial + gen.tu_failed + gen.tu_unsupported, err);
    }

    /* A9.2.3's coverage manifest. Sent beside the unit counts and never summed
     * with them: `tu_complete/tu_total` says the compilation database's units
     * were parsed, and `scope_covered/scope_candidates` says how much of the
     * repository that was. A surface holding only the first cannot tell the
     * difference between an index of a whole tree and an index of part of one. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "scope_discovery",
                                atlas_sem_scope_discovery_name(gen.scope_discovery), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "scope_candidates", gen.scope_candidates, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "scope_covered", gen.scope_covered, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "scope_uncovered", gen.scope_uncovered, err);
    }
    /* A9.2.4. The generation's own record of the input universe it was sealed
     * under. Reported on the generation rather than only on the plan, because
     * they answer different questions: the plan says what Atlas can account for
     * *now*, and this says what the index being served was built under. They
     * differ exactly when a rebuild is due, which is the moment somebody wants
     * to see both. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "discovery", atlas_sem_discovery_name(gen.discovery), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "input_count", gen.input_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "scope_excluded", gen.scope_excluded, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "test_scope_known", gen.test_scope_known, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_test", gen.tu_test, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "tu_production", gen.tu_production, err);
    }

    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "units", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        unit_sink sink = {ds, ATLAS_OK};
        int64_t listed = 0;
        bool trunc = false;
        st = atlas_db_sem_failed_units(ds->db, gen.id, ATLAS_SEM_STATUS_MAX_UNITS, write_unit,
                                       &sink, &listed, &trunc, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    atlas_sem_status_report_free(&rep);
    atlas_repo_info_free(&info);
    return st;
}

/* --- sem.symbol --------------------------------------------------------------- */

typedef struct sym_sink {
    dispatch_state *ds;
} sym_sink;

static atlas_status write_symbol(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    sym_sink *s = (sym_sink *)ud;
    dispatch_state *ds = s->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "usr", atlas_safe(&ds->safe, row->usr), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "name", atlas_safe(&ds->safe, row->name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "kind", row->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "linkage", row->linkage, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "type", atlas_safe(&ds->safe, row->type_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "is_definition", row->is_definition, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "external", row->external, err);
    }
    /* An entity outside the repository has no location Atlas indexed, so none
     * is claimed: an empty path would read as a file at the repository root. */
    if (st == ATLAS_OK && !row->external) {
        st = atlas_json_key_str_opt(ds->j, "file", atlas_safe(&ds->safe, row->file_text), err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "line", row->line, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "col", row->col, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "end_line", row->end_line, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "evidence", row->evidence, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status method_sem_symbol(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    atlas_repo_info info;
    atlas_sem_generation gen;
    atlas_status st = begin(ds, req, &info, &gen, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    const char *name = NULL;
    if (!atlas_ipc_param_str(req, "symbol", &name) || name[0] == '\0') {
        atlas_repo_info_free(&info);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "sem.symbol needs a \"symbol\" parameter");
    }
    const char *kind = NULL;
    (void)atlas_ipc_param_str(req, "kind", &kind);
    int64_t limit = 0;
    if (!atlas_ipc_param_int(req, "limit", &limit) || limit <= 0) {
        limit = ATLAS_SEM_MAX_ROWS;
    }

    st = atlas_json_key_str_opt(ds->j, "query", atlas_safe(&ds->safe, name), err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "symbols", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    int64_t total = 0;
    bool trunc = false;
    if (st == ATLAS_OK) {
        sym_sink sink = {ds};
        st = atlas_db_sem_symbols_by_name(ds->db, gen.id, name, NULL, kind, limit, write_symbol,
                                          &sink, &total, &trunc, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "truncated", trunc, err);
    }
    atlas_repo_info_free(&info);
    return st;
}

/* --- sem.graph and sem.trace ---------------------------------------------------
 *
 * Callers, callees and a path are one walk, so they are one method plus a
 * direction — the same reason A3 has one traversal. */

typedef struct walk_sink {
    dispatch_state *ds;
} walk_sink;

static atlas_status write_walk(const atlas_sem_walk_row *row, void *ud, atlas_err *err) {
    walk_sink *s = (walk_sink *)ud;
    dispatch_state *ds = s->ds;
    atlas_status st = atlas_json_obj_begin(ds->j, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "depth", row->depth, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "usr", atlas_safe(&ds->safe, row->usr), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "name", atlas_safe(&ds->safe, row->name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "file", atlas_safe(&ds->safe, row->file_text), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "line", row->line, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "edge_kind", row->edge_kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "via", atlas_safe(&ds->safe, row->via_name), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "evidence", row->evidence, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "site_file", atlas_safe(&ds->safe, row->site_file), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "site_line", row->site_line, err);
    }
    /* The true number of candidate targets, which may exceed how many were
     * recorded — a bound that made an ambiguity look smaller than it is would
     * be a bound that lies. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "candidate_total", row->candidate_total, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

static atlas_status write_walk_summary(dispatch_state *ds, const atlas_sem_walk_summary *s,
                                       atlas_err *err) {
    atlas_status st = atlas_json_key(ds->j, "summary", err);
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "visited", s->visited, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "emitted", s->emitted, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "max_depth_reached", s->max_depth_reached, err);
    }
    /* Split by the weakest evidence on the path. Merging them would be exactly
     * the conflation this layer exists to prevent. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "proven", s->proven, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "candidate", s->candidate, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "lexical", s->lexical, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unknown", s->unknown, err);
    }
    /* The walk crossed calls whose targets Atlas cannot name. The answer is
     * incomplete in a way no count of results would reveal. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved_indirect", s->unresolved_indirect, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "truncated", s->truncated, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "truncated_reason",
            atlas_sem_trunc_reason_is_known(s->truncated_reason) ? s->truncated_reason : NULL,
            err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(ds->j, err);
    }
    return st;
}

/* Resolves a symbol name to one USR, refusing when it names several.
 *
 * A caller asking about `parse` when three functions share the name must not be
 * given one of them silently. The refusal says how many there are so the caller
 * can disambiguate with `sem.symbol`. */
static atlas_status one_usr(dispatch_state *ds, int64_t gen_id, const char *name,
                            atlas_buf *usr_out, atlas_err *err);

typedef struct pick_sink {
    atlas_buf *usr;
    size_t distinct;
    atlas_buf seen; /* NUL-separated USRs already counted */
    atlas_status st;
} pick_sink;

static atlas_status pick_usr(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    pick_sink *p = (pick_sink *)ud;
    const char *s = (const char *)p->seen.data;
    const char *end = s + p->seen.len;
    while (s < end) {
        if (strcmp(s, row->usr) == 0) {
            return ATLAS_OK; /* a declaration and its definition are one entity */
        }
        s += strlen(s) + 1;
    }
    p->distinct++;
    if (p->distinct == 1) {
        atlas_status st = atlas_buf_set_str(p->usr, row->usr, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return atlas_buf_append(&p->seen, row->usr, strlen(row->usr) + 1, err);
}

static atlas_status one_usr(dispatch_state *ds, int64_t gen_id, const char *name,
                            atlas_buf *usr_out, atlas_err *err) {
    pick_sink p;
    memset(&p, 0, sizeof(p));
    p.usr = usr_out;
    atlas_buf_init(&p.seen);
    int64_t total = 0;
    bool trunc = false;
    atlas_status st = atlas_db_sem_symbols_by_name(ds->db, gen_id, name, NULL, NULL,
                                                   ATLAS_SEM_MAX_ROWS, pick_usr, &p, &total,
                                                   &trunc, err);
    size_t distinct = p.distinct;
    atlas_buf_free(&p.seen);
    if (st != ATLAS_OK) {
        return st;
    }
    if (distinct == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "no symbol by that name is in the semantic index");
    }
    if (distinct > 1) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "that name resolves to %zu distinct symbols; ask sem.symbol for the "
                             "candidates and pass an exact \"usr\"",
                             distinct);
    }
    return ATLAS_OK;
}

/* A caller may pass an exact `usr` instead of a `symbol`, which is how it acts
 * on the candidates `sem.symbol` returned. A USR is an opaque identifier Atlas
 * produced, not a path and not a name, so accepting one opens nothing. */
static atlas_status target_usr(dispatch_state *ds, const atlas_ipc_request *req, int64_t gen_id,
                               const char *symbol_key, const char *usr_key, atlas_buf *out,
                               atlas_err *err) {
    const char *usr = NULL;
    if (atlas_ipc_param_str(req, usr_key, &usr) && usr[0] != '\0') {
        return atlas_buf_set_str(out, usr, err);
    }
    const char *name = NULL;
    if (!atlas_ipc_param_str(req, symbol_key, &name) || name[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "this method needs \"%s\" or \"%s\"",
                             symbol_key, usr_key);
    }
    return one_usr(ds, gen_id, name, out, err);
}

static atlas_status method_sem_graph(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_repo_info info;
    atlas_sem_generation gen;
    atlas_status st = begin(ds, req, &info, &gen, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_buf usr = ATLAS_BUF_INIT;
    st = target_usr(ds, req, gen.id, "symbol", "usr", &usr, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&usr);
        atlas_repo_info_free(&info);
        return st;
    }

    bool inbound = false;
    (void)atlas_ipc_param_bool(req, "inbound", &inbound);
    bool proven_only = false;
    (void)atlas_ipc_param_bool(req, "proven_only", &proven_only);
    int64_t depth = 0;
    if (!atlas_ipc_param_int(req, "depth", &depth) || depth <= 0) {
        depth = ATLAS_SEM_DEFAULT_DEPTH;
    }

    atlas_sem_walk_opts o;
    atlas_sem_walk_opts_init(&o);
    o.usr = atlas_buf_cstr(&usr);
    o.inbound = inbound;
    o.depth = depth;
    o.max_rows = 0;
    if (!atlas_ipc_param_int(req, "limit", &o.max_rows) || o.max_rows <= 0) {
        o.max_rows = ATLAS_SEM_MAX_ROWS;
    }
    o.proven_only = proven_only;

    st = atlas_json_key_str(ds->j, "direction", inbound ? "callers" : "callees", err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "nodes", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    atlas_sem_walk_summary sum;
    memset(&sum, 0, sizeof(sum));
    if (st == ATLAS_OK) {
        walk_sink sink = {ds};
        st = atlas_sem_walk(ds->db, gen.id, &o, write_walk, &sink, &sum, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = write_walk_summary(ds, &sum, err);
    }
    atlas_buf_free(&usr);
    atlas_repo_info_free(&info);
    return st;
}

static atlas_status method_sem_trace(dispatch_state *ds, const atlas_ipc_request *req,
                                     atlas_err *err) {
    atlas_repo_info info;
    atlas_sem_generation gen;
    atlas_status st = begin(ds, req, &info, &gen, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_buf from = ATLAS_BUF_INIT;
    atlas_buf to = ATLAS_BUF_INIT;
    st = target_usr(ds, req, gen.id, "from", "from_usr", &from, err);
    if (st == ATLAS_OK) {
        st = target_usr(ds, req, gen.id, "to", "to_usr", &to, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&from);
        atlas_buf_free(&to);
        atlas_repo_info_free(&info);
        return st;
    }

    int64_t depth = 0;
    if (!atlas_ipc_param_int(req, "depth", &depth) || depth <= 0) {
        depth = ATLAS_SEM_DEFAULT_DEPTH;
    }

    st = atlas_json_key(ds->j, "nodes", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    atlas_sem_walk_summary sum;
    memset(&sum, 0, sizeof(sum));
    if (st == ATLAS_OK) {
        walk_sink sink = {ds};
        st = atlas_sem_trace(ds->db, gen.id, atlas_buf_cstr(&from), atlas_buf_cstr(&to), depth, 1,
                             write_walk, &sink, &sum, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    if (st == ATLAS_OK) {
        st = write_walk_summary(ds, &sum, err);
    }
    atlas_buf_free(&from);
    atlas_buf_free(&to);
    atlas_repo_info_free(&info);
    return st;
}

/* --- the group ------------------------------------------------------------------
 *
 * Four reads. Adding a fifth means deciding, explicitly, whether it is a read;
 * anything that is plausibly a mutation belongs in the operator-uid group and
 * its name belongs in the negative enumeration in `tests/test_orch_rpc.c`, so
 * the list of names that must answer `unknown method` keeps pace. */

/* --- sem.impact and sem.context --------------------------------------------
 *
 * Both are reads, and both go through exactly the service functions the CLI
 * calls. That is the parity guarantee: there is one implementation of impact
 * and one of the context builder, so a socket answer and a local answer cannot
 * differ in what they contain — only in how they were transported. */

static atlas_status write_items(dispatch_state *ds, const atlas_sem_item *items, size_t count,
                                atlas_err *err) {
    atlas_status st = atlas_json_key(ds->j, "items", err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; i < count && st == ATLAS_OK; i++) {
        const atlas_sem_item *it = &items[i];
        st = atlas_json_obj_begin(ds->j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "kind", it->kind, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str_opt(ds->j, "name", atlas_safe(&ds->safe, it->name), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str_opt(ds->j, "file", atlas_safe(&ds->safe, it->file_text), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "line", it->line, err);
        }
        /* A9.1, and only on a decision item: what sort of knowledge it is and
         * where the approval workflow left it. Absent rather than empty on a
         * symbol or a file, so a consumer does not have to distinguish "not a
         * knowledge record" from "a record with no kind". Both are closed Atlas
         * vocabularies and need no encoding. */
        if (st == ATLAS_OK && it->knowledge_kind[0] != '\0') {
            st = atlas_json_key_str(ds->j, "knowledge_kind", it->knowledge_kind, err);
        }
        if (st == ATLAS_OK && it->knowledge_status[0] != '\0') {
            st = atlas_json_key_str(ds->j, "knowledge_status", it->knowledge_status, err);
        }
        /* A9.2.2, §24. Sent in the shape the daemon produces, so MCP — which
         * relays this object — and the local renderer carry the same three
         * axes. Explicit `UNKNOWN` rather than an absent key: a consumer must
         * not have to decide what a missing truth means. */
        if (st == ATLAS_OK && it->knowledge_kind[0] != '\0') {
            st = atlas_json_key_str(ds->j, "knowledge_truth",
                                    it->knowledge_truth[0] != '\0' ? it->knowledge_truth
                                                                   : "UNKNOWN",
                                    err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "evidence", it->evidence, err);
        }
        if (st == ATLAS_OK) {
            /* The selection reason is checked against Atlas' own closed set
             * before it crosses the socket. */
            st = atlas_json_key_str_opt(
                ds->j, "why", atlas_sem_selection_reason_is_known(it->why) ? it->why : NULL, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "depth", it->depth, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    return st;
}

static atlas_status method_sem_impact(dispatch_state *ds, const atlas_ipc_request *req,
                                      atlas_err *err) {
    const char *repo = NULL;
    if (!atlas_ipc_param_str(req, "repo", &repo)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "sem.impact needs a \"repo\" parameter");
    }
    const char *subject = NULL;
    if (!atlas_ipc_param_str(req, "subject", &subject) || subject[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "sem.impact needs a \"subject\" parameter");
    }
    int64_t depth = 0;
    if (!atlas_ipc_param_int(req, "depth", &depth) || depth <= 0) {
        depth = ATLAS_SEM_DEFAULT_DEPTH;
    }
    int64_t limit = 0;
    if (!atlas_ipc_param_int(req, "limit", &limit) || limit <= 0) {
        limit = ATLAS_SEM_MAX_ROWS;
    }

    /* Resolved here from the registry, then handed to the shared core — the
     * daemon has a read-only handle rather than an `atlas_ctx`, and the core is
     * what both surfaces run. */
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);

    atlas_sem_impact_report rep;
    atlas_sem_impact_report_init(&rep);
    if (st == ATLAS_OK) {
        st = atlas_sem_impact_on(ds->db, &info, subject, depth, limit, &rep, err);
    }
    atlas_repo_info_free(&info);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", rep.repo.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "freshness", atlas_sem_freshness_name(rep.freshness), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation_id", rep.generation.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "subject", atlas_safe(&ds->safe, rep.query), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "subject_kind", rep.subject_is_path ? "file" : "symbol",
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "subject_found", rep.subject_found, err);
    }
    if (st == ATLAS_OK) {
        st = write_items(ds, rep.items, rep.count, err);
    }
    /* Split, never summed. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "proven", rep.proven, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "candidate", rep.candidate, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "lexical", rep.lexical, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "unresolved_indirect", rep.unresolved_indirect, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "truncated", rep.truncated, err);
    }
    atlas_sem_impact_report_free(&rep);
    return st;
}


static atlas_status method_sem_context(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_err *err) {
    const char *task = NULL;
    if (!atlas_ipc_param_str(req, "task", &task) || task[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "sem.context needs a \"task\" parameter");
    }
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    atlas_status st = atlas_server_require_repo(ds, req, &info, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&info);
        return st;
    }

    atlas_sem_context_req creq;
    atlas_sem_context_req_init(&creq);
    creq.repo = info.name;
    /* The task ranks evidence. It authorises nothing and selects no repository:
     * the repository came from the registry above, and an imperative in this
     * text reaches no mutation because this method calls only reads. */
    creq.task = task;
    (void)atlas_ipc_param_int(req, "depth", &creq.depth);
    (void)atlas_ipc_param_int(req, "max_tokens", &creq.max_tokens);
    (void)atlas_ipc_param_int(req, "max_items", &creq.max_items);
    (void)atlas_ipc_param_bool(req, "include_history", &creq.include_history);

    atlas_sem_context_report rep;
    atlas_sem_context_report_init(&rep);
    st = atlas_sem_context_on(ds->db, &info, &creq, &rep, err);
    atlas_repo_info_free(&info);

    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "repo", rep.repo.name, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "commit", rep.repo.scanned_head, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "freshness", atlas_sem_freshness_name(rep.freshness), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "generation_id", rep.generation.id, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "task", atlas_safe(&ds->safe, rep.task), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "budget_bytes", rep.budget_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "used_bytes", rep.used_bytes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "budget_reached", rep.budget_reached, err);
    }
    if (st == ATLAS_OK) {
        st = write_items(ds, rep.items, rep.count, err);
    }
    /* Always emitted, so an empty array is a positive statement that nothing
     * was missing rather than a key somebody forgot. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "not_included", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; i < rep.missing_count && st == ATLAS_OK; i++) {
        st = atlas_json_str(ds->j, rep.missing[i], err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }
    atlas_sem_context_report_free(&rep);
    return st;
}
static const atlas_method_entry SEM_METHODS[] = {
    {"sem.status", method_sem_status},
    {"sem.symbol", method_sem_symbol},
    {"sem.graph", method_sem_graph},
    {"sem.trace", method_sem_trace},
    {"sem.impact", method_sem_impact},
    {"sem.context", method_sem_context},
};

const atlas_method_entry *atlas_server_sem_methods(size_t *count_out) {
    if (count_out != NULL) {
        *count_out = sizeof(SEM_METHODS) / sizeof(SEM_METHODS[0]);
    }
    return SEM_METHODS;
}

/* --- A9.2.3: the derived state, the manifest and the build description -------
 *
 * One writer, for the reason `atlas_service_verify_write_detail` is one: two
 * places emitting the same block is how a read and a write start describing the
 * same repository differently, and here one of them is the command an operator
 * runs to check the other.
 *
 * Every value is Atlas' own: a fixed vocabulary name, an integer Atlas counted,
 * a boolean, or a path an *operator* wrote down. The paths are the only thing
 * here a person chose, and they are written through the ordinary string writer,
 * which escapes them.
 *
 * The four axes stay four fields. `activity` is the fold an operator acts on;
 * `freshness` and `coverage_complete` are the two axes it folds, and both are
 * sent beside it. A surface holding only the fold could not distinguish
 * "current and complete" from "current and describing half the tree", which is
 * the whole state this season adds. */
/* The fields `open_generation` has not already written.
 *
 * Split from the full writer because `sem.status` emits `repo`, `freshness` and
 * `stale_reason` before it has a plan — freshness comes first on every semantic
 * read, which is A8-CI's rule — and a response must not carry a key twice. */
static atlas_status write_sem_plan_fields(dispatch_state *ds, const atlas_sem_status_report *rep,
                                          atlas_err *err) {
    const atlas_sem_plan *p = &rep->plan;
    atlas_status st = atlas_json_key_str(ds->j, "activity", atlas_sem_activity_name(p->activity),
                                         err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(
            ds->j, "hold_reason",
            atlas_sem_hold_reason_is_known(p->hold_reason) ? p->hold_reason : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "coverage_complete", p->coverage_complete, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "configured", p->configured, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "auto_rebuild", p->auto_rebuild, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "rebuild_due", p->should_build, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "fail_count", p->fail_count, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "fail_reason",
                                    atlas_sem_why_is_known(p->fail_reason) ? p->fail_reason : NULL,
                                    err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "fail_at", p->fail_at, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "source_identity", p->source_identity, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "generation_identity", p->generation_identity, err);
    }

    /* --- A9.2.4 ---
     *
     * The activation intent and its provenance travel beside the effective
     * boolean rather than instead of it, because `auto_rebuild = false` is one
     * value and three different situations: an operator's refusal, a
     * machine-wide default, and a default nobody has ever revisited. A client
     * that could only see the boolean would tell somebody their repository is
     * off without telling them whose decision that was.
     *
     * Discovery travels as three separate values for the same reason A9.2.3
     * sends freshness and coverage separately: what Atlas can currently account
     * for, what the served generation was built under, and how many candidates
     * were accepted or refused are four questions, and a fold of them would hide
     * exactly the state this season exists to expose. */
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "auto_intent", atlas_sem_auto_intent_name(p->auto_intent),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "auto_intent_by",
                                atlas_sem_intent_source_name(p->auto_intent_by), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_bool(ds->j, "policy_default", p->policy_default, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "discovery", atlas_sem_discovery_name(p->discovery), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "discovery_mode",
                                atlas_sem_discovery_mode_name(p->discovery_mode), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "generation_discovery",
                                atlas_sem_discovery_name(p->generation_discovery), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "inputs_accepted", p->inputs_accepted, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(ds->j, "inputs_rejected", p->inputs_rejected, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str_opt(ds->j, "discovered_at", p->discovered_at, err);
    }
    if (st == ATLAS_OK) {
        /* Encoded like every other value that could in principle have come from
         * outside — it cannot here, being one of a fixed set of Atlas strings,
         * and encoding it costs nothing and removes the question. */
        st = atlas_json_key_str_opt(ds->j, "discovery_limit",
                                    p->discovery_limit[0] != '\0'
                                        ? atlas_safe(&ds->safe, p->discovery_limit)
                                        : NULL,
                                    err);
    }

    /* Every candidate, accepted and rejected, with the reason for each.
     *
     * Rejected candidates are sent because a candidate nobody is shown is
     * indistinguishable from one that does not exist — which is precisely what
     * kept a third compilation database invisible for a season. The reason is
     * checked against Atlas' own closed set before it crosses the socket, so a
     * value from anywhere else becomes absent rather than reproduced. */
    if (st == ATLAS_OK) {
        st = atlas_json_key(ds->j, "build_inputs", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(ds->j, err);
    }
    for (size_t i = 0; i < rep->input_count && st == ATLAS_OK; i++) {
        const struct atlas_sem_input *in = &rep->inputs[i];
        st = atlas_json_obj_begin(ds->j, err);
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "path", atlas_safe(&ds->safe, in->path), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str(ds->j, "origin", atlas_sem_input_origin_name(in->origin), err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_bool(ds->j, "accepted", in->accepted, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str_opt(
                ds->j, "reject_reason",
                atlas_sem_reject_reason_is_known(in->reject_reason) ? in->reject_reason : NULL,
                err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_str_opt(ds->j, "digest", in->digest, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_key_int(ds->j, "units", in->unit_count, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_obj_end(ds->j, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(ds->j, err);
    }

    /* The declared lists, each element its own string. Sent as arrays rather
     * than as the newline-joined storage form: a client must not have to parse
     * a separator to find out what an operator configured. */
    static const char *const KEYS[4] = {"compdbs", "test_roots", "excludes", "vendor_roots"};
    const atlas_buf *const BUFS[4] = {&rep->compdbs, &rep->test_roots, &rep->excludes,
                                      &rep->vendor_roots};
    for (size_t k = 0; k < 4 && st == ATLAS_OK; k++) {
        st = atlas_json_key(ds->j, KEYS[k], err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(ds->j, err);
        }
        if (st != ATLAS_OK) {
            break;
        }
        const char *data = BUFS[k]->len > 0 ? (const char *)BUFS[k]->data : "";
        size_t len = BUFS[k]->len;
        size_t start = 0;
        for (size_t i = 0; i <= len && st == ATLAS_OK; i++) {
            if (i != len && data[i] != '\n') {
                continue;
            }
            size_t n = i - start;
            /* Bounded and *skipped* rather than truncated when it does not fit.
             * A truncated path names a different file, and a client reading it
             * back would configure the repository to read something else. The
             * bound cannot be reached through this path — `atlas_sem_config_pack`
             * refuses a description longer than `ATLAS_SEM_CONFIG_MAX_BYTES` in
             * total — so this is the belt to that braces. */
            char one[ATLAS_SEM_CONFIG_MAX_BYTES];
            if (n > 0 && n < sizeof one) {
                memcpy(one, data + start, n);
                one[n] = '\0';
                st = atlas_json_str(ds->j, atlas_safe(&ds->safe, one), err);
            }
            start = i + 1u;
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(ds->j, err);
        }
    }
    return st;
}

atlas_status atlas_server_write_sem_config(dispatch_state *ds, const atlas_sem_status_report *rep,
                                           atlas_err *err) {
    const atlas_sem_plan *p = &rep->plan;
    atlas_status st = atlas_json_key_str(ds->j, "repo", rep->repo.name, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(ds->j, "freshness", atlas_sem_freshness_name(p->freshness), err);
    }
    if (st == ATLAS_OK) {
        /* Checked against Atlas' own closed set before it crosses the socket,
         * so a value from anywhere else becomes absent rather than reproduced. */
        st = atlas_json_key_str_opt(
            ds->j, "stale_reason",
            atlas_sem_stale_reason_is_known(p->stale_reason) ? p->stale_reason : NULL, err);
    }
    if (st == ATLAS_OK) {
        st = write_sem_plan_fields(ds, rep, err);
    }
    return st;
}

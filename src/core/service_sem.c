/* Atlas - the `code`/`context` semantic command behaviour.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The service layer for the compiler-derived index. Renderers never query and
 * this file never formats — A0's separation, unchanged.
 *
 * Two things here are worth stating because they are decisions rather than
 * plumbing:
 *
 *   1. **Every entry point resolves the repository through the shared
 *      resolver**, `atlas_service_require_repo`, which reads the persistent
 *      registry and nothing else. That is what makes CLI, RPC and MCP agree
 *      about which repository a name means: there is one resolver, not three
 *      copies of a rule. A name that is not registered is NOT_REGISTERED, and
 *      Atlas does not look at the filesystem to decide that.
 *   2. **Every read reports freshness before it reports results.** A caller
 *      that asks for callers of a symbol gets the answer *and* whether the
 *      index it came from still describes the code. Returning the rows alone
 *      would let a stale answer read exactly like a current one, which is the
 *      conflation the whole evidence model exists to prevent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/sem.h"
#include "atlas/sem_discover.h"
#include "atlas/sem_ops.h"
#include "atlas/syspolicy.h"
#include "atlas/service.h"
#include "atlas/unit.h"
#include "service_internal.h"

/* --- resolving the generation to answer from -------------------------------- */

/* Loads the published generation and computes its freshness against live facts.
 *
 * `found` false means ABSENT — nobody has indexed this repository — which is a
 * different answer from STALE and must stay one. Both are ordinary outcomes and
 * neither is an error: a caller is told what Atlas holds, and decides. */
static atlas_status load_generation(atlas_db *db, atlas_repo_info *repo,
                                    atlas_sem_generation *gen, bool *found,
                                    atlas_sem_freshness *fresh, const char **reason,
                                    atlas_sem_trust *trust, atlas_err *err) {
    atlas_sem_generation_init(gen);
    *fresh = ATLAS_SEM_FRESH_ABSENT;
    *reason = NULL;

    atlas_status st = atlas_db_sem_current(db, repo->id, gen, found, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A generation still being built is reported beside the one being served,
     * so "rebuilding" and "stale" are distinguishable. */
    atlas_sem_generation latest;
    bool have_latest = false;
    st = atlas_db_sem_latest(db, repo->id, &latest, &have_latest, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool running = have_latest && latest.status == ATLAS_SEM_GEN_RUNNING;

    /* Every live fact — the file index's currency, the compilation-database
     * digest and the working-tree identity — is gathered by one function, so
     * this surface and the daemon's cannot disagree about the same generation.
     * They did before A9.2.3: each assembled its own arguments and each passed
     * NULL for the digest, which made the compilation-database check
     * unreachable from either. */
    /* A9.2.5. One gatherer, replacing `atlas_sem_freshness_now` rather than
     * joining it: both compute freshness from the same single pass over the
     * build description, and calling both would hash every source twice per
     * response. The verdict is left unsettled — only the caller knows what it
     * emitted. */
    atlas_sem_trust_now(db, repo, gen, *found, running, trust);
    *fresh = trust->freshness;
    *reason = trust->stale_reason;
    return ATLAS_OK;
}

/* Shared preamble: resolve the repository, load the generation, and refuse
 * clearly when there is nothing to answer from.
 *
 * The refusals are typed and distinct because an operator does different things
 * about them: build an index, rebuild a stale one, or install libclang. */
static atlas_status begin_read(atlas_ctx *ctx, const char *name, atlas_repo_info *repo,
                               atlas_sem_generation *gen, atlas_sem_freshness *fresh,
                               const char **reason, atlas_sem_trust *trust, atlas_err *err) {
    atlas_status st = atlas_service_require_repo(ctx, name, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool found = false;
    st = load_generation(atlas_ctx_db(ctx), repo, gen, &found, fresh, reason, trust, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "no semantic index exists for this repository; an operator builds "
                             "one with `atlas code index`");
    }
    return ATLAS_OK;
}

/* --- status ------------------------------------------------------------------ */

void atlas_sem_status_report_init(atlas_sem_status_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_sem_generation_init(&r->generation);
    atlas_sem_plan_init(&r->plan);
    atlas_buf_init(&r->compdbs);
    atlas_buf_init(&r->test_roots);
    atlas_buf_init(&r->excludes);
    atlas_buf_init(&r->vendor_roots);
}

void atlas_sem_status_report_free(atlas_sem_status_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    atlas_buf_free(&r->compdbs);
    atlas_buf_free(&r->test_roots);
    atlas_buf_free(&r->excludes);
    atlas_buf_free(&r->vendor_roots);
    free(r->inputs);
    r->inputs = NULL;
    r->input_count = 0;
    free(r->obstacles);
    r->obstacles = NULL;
    r->obstacle_count = 0;
}

typedef struct unit_sink {
    atlas_sem_status_report *rep;
} unit_sink;

static atlas_status take_unit(const atlas_sem_unit_report *row, void *ud, atlas_err *err) {
    (void)err;
    unit_sink *s = (unit_sink *)ud;
    if (s->rep->failed_count >= ATLAS_SEM_STATUS_MAX_UNITS) {
        /* Reported as a count that exceeds what is listed, never as a shorter
         * list that reads as the whole story. */
        return ATLAS_OK;
    }
    atlas_sem_failed_unit *u = &s->rep->failed[s->rep->failed_count++];
    (void)snprintf(u->source, sizeof(u->source), "%s", row->source_text);
    (void)snprintf(u->status, sizeof(u->status), "%s", row->status);
    (void)snprintf(u->why, sizeof(u->why), "%s", row->why);
    u->diagnostics_errors = row->diagnostics_errors;
    return ATLAS_OK;
}

/* Resolving a repository from a raw handle, with the same refusal
 * `atlas_service_require_repo` gives.
 *
 * The `_on` forms below exist because the daemon's writer thread has a handle
 * and no `atlas_ctx` — the reason `atlas_sem_index_on` and `atlas_sem_impact_on`
 * exist. Duplicating the *refusal* rather than the resolution keeps
 * NOT_REGISTERED identical on both surfaces, which is what a caller sees. */
static atlas_status require_repo_on(atlas_db *db, const char *name, atlas_repo_info *out,
                                    atlas_err *err) {
    bool found = false;
    atlas_status st = atlas_db_repo_get(db, name, out, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (!found) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "NOT_REGISTERED: no repository named \"%s\" is registered. "
                             "Repositories are onboarded only by an operator; Atlas does not "
                             "discover them (try: atlas repo list)",
                             name);
    }
    return ATLAS_OK;
}

atlas_status atlas_sem_status_on(atlas_db *db, const char *name, atlas_sem_status_report *out,
                                 atlas_err *err) {
    atlas_status st = require_repo_on(db, name, &out->repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->libclang_available = atlas_sem_available();
    (void)snprintf(out->compiler_id, sizeof(out->compiler_id), "%s", atlas_sem_compiler_id());
    (void)snprintf(out->compiler_version, sizeof(out->compiler_version), "%s",
                   atlas_sem_compiler_version());

    /* A9.2.3. The derived state, the freshness and the generation, from **one**
     * computation.
     *
     * The plan is a read — no transaction, no lock, no process — which is what
     * lets the scheduler and this status page be the same function rather than
     * two that agree by inspection. It also already contains the freshness, and
     * that matters for more than tidiness: computing freshness twice within one
     * response means hashing every declared compilation database twice, and it
     * means the two halves of one document could disagree if the tree moved
     * between them. Measured on a repository with two databases totalling 485
     * KiB, the redundant work was most of the command's cost. */
    /* A9.2.5. The generation and the trust block first, then the plan **from
     * them**. `atlas_sem_plan_for` would otherwise reach `live_facts` a second
     * time — see `atlas_sem_plan_for_with_trust`. */
    bool found = false;
    st = atlas_db_sem_current(db, out->repo.id, &out->generation, &found, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_sem_generation latest;
    bool have_latest = false;
    st = atlas_db_sem_latest(db, out->repo.id, &latest, &have_latest, err);
    if (st != ATLAS_OK) {
        return st;
    }
    const bool running = have_latest && latest.status == ATLAS_SEM_GEN_RUNNING;

    atlas_syspolicy pol;
    atlas_syspolicy_load(&pol);
    const bool policy_default = atlas_syspolicy_semantic_auto_default(&pol);
    atlas_sem_trust_now_with_default(db, &out->repo, &out->generation, found, running,
                                     policy_default, &out->trust);
    atlas_sem_trust_settle(&out->trust, 0, false);

    st = atlas_sem_plan_for_with_trust(db, &out->repo, false, policy_default, &out->trust,
                                       &out->plan, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->freshness = out->trust.freshness;
    out->stale_reason = out->trust.stale_reason;
    out->have_generation = found;

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    st = atlas_db_sem_config_get(db, out->repo.id, &cfg, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->compdbs, cfg.compdbs.data, cfg.compdbs.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->test_roots, cfg.test_roots.data, cfg.test_roots.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->excludes, cfg.excludes.data, cfg.excludes.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&out->vendor_roots, cfg.vendor_roots.data, cfg.vendor_roots.len, err);
    }
    atlas_sem_config_free(&cfg);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A9.2.4. Every candidate, accepted and rejected, with its reason.
     *
     * Rejected ones are carried because a candidate nobody is shown is
     * indistinguishable from one that does not exist — which is precisely the
     * confusion that made a third compilation database invisible for a season. */
    out->inputs = calloc(ATLAS_SEM_DISCOVERY_MAX_CANDIDATES, sizeof(*out->inputs));
    if (out->inputs == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading the build inputs");
    }
    st = atlas_db_sem_inputs_get(db, out->repo.id, out->inputs,
                                 ATLAS_SEM_DISCOVERY_MAX_CANDIDATES, &out->input_count, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A9.2.5. Every place the walk could not account for, with its exact path.
     *
     * The same argument as the rejected candidates one line above, applied to
     * directories rather than files: an obstacle nobody is shown is
     * indistinguishable from a subtree that holds nothing, and A9.2.4 kept only
     * the first reason and no path at all. */
    out->obstacles = calloc(ATLAS_SEM_DISCOVERY_MAX_OBSTACLES, sizeof(*out->obstacles));
    if (out->obstacles == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "out of memory reading the discovery obstacles");
    }
    st = atlas_db_sem_obstacles_get(db, out->repo.id, out->obstacles,
                                    ATLAS_SEM_DISCOVERY_MAX_OBSTACLES, &out->obstacle_count,
                                    &out->obstacles_truncated, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The most recent attempt, whatever became of it. A failed index is an
     * operational fact and a status that only ever reported successes could not
     * state it. */
    st = atlas_db_sem_latest(db, out->repo.id, &out->latest, &out->have_latest, err);
    if (st != ATLAS_OK || !found) {
        return st;
    }

    unit_sink sink = {out};
    int64_t listed = 0;
    atlas_status ust = atlas_db_sem_failed_units(db, out->generation.id,
                                                 ATLAS_SEM_STATUS_MAX_UNITS, take_unit, &sink,
                                                 &listed, &out->failed_truncated, err);
    /* The true number, taken from the generation's own tallies rather than from
     * how many rows this page returned. A count that equalled the page size
     * would make a truncated list read as the whole story, which is the one
     * thing a bounded report must never do. */
    out->failed_total = out->generation.tu_partial + out->generation.tu_failed +
                        out->generation.tu_unsupported;
    return ust;
}

atlas_status atlas_service_sem_status(atlas_ctx *ctx, const char *name,
                                      atlas_sem_status_report *out, atlas_err *err) {
    return atlas_sem_status_on(atlas_ctx_db(ctx), name, out, err);
}

/* --- A9.2.3: the durable build description ------------------------------------
 *
 * Writing this row is what authorises the daemon to run a compiler over a
 * repository when that repository changes, which is why it is an operator
 * action and has no model-facing surface at all. Reading it back into the
 * status report afterwards means one command shows the operator exactly what
 * their change did, including the state it moved the repository into. */
atlas_status atlas_sem_config_on(atlas_db *db, const atlas_sem_config_job *job,
                                 atlas_sem_status_report *out, atlas_err *err) {
    if (job == NULL || job->repo_name == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic configuration: bad request");
    }
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    atlas_status st = require_repo_on(db, job->repo_name, &repo, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&repo);
        return st;
    }

    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    st = atlas_db_sem_config_get(db, repo.id, &cfg, err);
    cfg.repo_id = repo.id;

    /* The durable identity, so the row can still say which repository lineage
     * it described after the rowid is gone — the reason every other durable
     * Atlas record that references a repository carries one. */
    if (st == ATLAS_OK) {
        atlas_buf identity = ATLAS_BUF_INIT;
        atlas_err ignored;
        atlas_err_init(&ignored);
        if (atlas_db_repo_identity_hash(db, repo.id, &identity, &ignored) == ATLAS_OK) {
            (void)snprintf(cfg.repo_identity_hash, sizeof cfg.repo_identity_hash, "%s",
                           atlas_buf_cstr(&identity));
        }
        atlas_buf_free(&identity);
    }

    /* NULL leaves a list alone; a non-NULL pointer with a zero length clears
     * one. An operator adjusting the test roots must not silently drop the
     * compilation databases, and vice versa. */
    if (st == ATLAS_OK && job->compdbs != NULL) {
        st = atlas_buf_set(&cfg.compdbs, job->compdbs, job->compdbs_len, err);
    }
    if (st == ATLAS_OK && job->test_roots != NULL) {
        st = atlas_buf_set(&cfg.test_roots, job->test_roots, job->test_roots_len, err);
    }
    if (st == ATLAS_OK && job->excludes != NULL) {
        st = atlas_buf_set(&cfg.excludes, job->excludes, job->excludes_len, err);
    }
    if (st == ATLAS_OK && job->vendor_roots != NULL) {
        st = atlas_buf_set(&cfg.vendor_roots, job->vendor_roots, job->vendor_roots_len, err);
    }
    if (job->auto_rebuild >= 0) {
        /* A9.2.4. The intent *and* its provenance, written together and never
         * apart. A stored value with no provenance is exactly the ambiguity this
         * season exists to end: it could not tell a deliberate refusal from a
         * default nobody had ever revisited. Anything that reaches this line is
         * an operator, because there is no MCP tool, no gateway route and no
         * ordinary RPC method that reaches it. */
        cfg.auto_intent =
            job->auto_rebuild > 0 ? ATLAS_SEM_INTENT_ENABLED : ATLAS_SEM_INTENT_DISABLED;
        cfg.auto_intent_by = ATLAS_SEM_INTENT_BY_OPERATOR;
    }
    if (job->discovery_mode >= 0) {
        cfg.discovery_mode =
            job->discovery_mode > 0 ? ATLAS_SEM_DISCMODE_MANUAL : ATLAS_SEM_DISCMODE_AUTOMATIC;
    }
    /* The legacy boolean is kept in step with the intent so nothing that still
     * reads it can disagree with what the intent says. It is a cache of
     * `atlas_sem_auto_effective` under the *compiled-in* default, never the
     * authority — the plan recomputes it against the live root-owned policy on
     * every read, which is A6's rule about never storing a derived answer. */
    cfg.auto_rebuild = atlas_sem_auto_effective(cfg.auto_intent, ATLAS_SEM_AUTO_DEFAULT);
    if (st == ATLAS_OK) {
        st = atlas_db_sem_config_set(db, &cfg, err);
    }
    atlas_sem_config_free(&cfg);
    if (st == ATLAS_OK) {
        /* A9.2.4. Re-walk immediately, because the write just changed what the
         * walk would do: the mode, the exclusions and the pinned list are all
         * inputs to it. Waiting for the periodic pass would mean an operator who
         * has just excluded a build tree still sees it accepted, which reads as
         * the command not having worked.
         *
         * Best effort and deliberately not allowed to fail the write. An
         * operator's statement about a repository is recorded whether or not
         * Atlas can currently look at the tree — a description can legitimately
         * be written before the tree it describes has been built, which is the
         * same reason paths are not validated here. */
        atlas_sem_discovery_result res;
        atlas_sem_discovery_result_init(&res);
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_sem_discovery_run(db, &repo, &res, &ignored);
    }
    atlas_repo_info_free(&repo);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Read the whole state back, so the operator sees what their change did
     * rather than being told it was accepted. */
    return atlas_sem_status_on(db, job->repo_name, out, err);
}

/* Packs the caller's argument arrays into the storage form and hands the whole
 * request to the core. One place converts, so the CLI's `--compdb` repetition
 * and the daemon's JSON array reach the same bytes. */
static atlas_status pack_optional(const char *const *items, size_t count, atlas_buf *out,
                                  bool *given, atlas_err *err) {
    *given = items != NULL;
    if (items == NULL) {
        return ATLAS_OK;
    }
    return atlas_sem_config_pack(items, count, out, err);
}

/* A9.2.4. Builds the storage-form job from the caller's four optional arrays.
 *
 * One place converts, so `--compdb` repeated on a command line, a JSON array
 * over the socket and a test's literal array all reach the same bytes — the
 * defect A9.2.3 shipped was two packers disagreeing about the separator, and one
 * function is what stops that recurring. */
atlas_status atlas_service_sem_config_set(atlas_ctx *ctx, const atlas_sem_config_request *req,
                                          atlas_sem_status_report *out, atlas_err *err) {
    if (req == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic configuration: bad request");
    }
    atlas_buf packed[4];
    bool given[4] = {false, false, false, false};
    for (size_t i = 0; i < 4; i++) {
        atlas_buf_init(&packed[i]);
    }
    const char *const *lists[4] = {req->compdbs, req->test_roots, req->excludes,
                                   req->vendor_roots};
    const size_t counts[4] = {req->compdb_count, req->test_root_count, req->exclude_count,
                              req->vendor_root_count};

    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < 4; i++) {
        st = pack_optional(lists[i], counts[i], &packed[i], &given[i], err);
    }
    if (st == ATLAS_OK) {
        atlas_sem_config_job job;
        memset(&job, 0, sizeof job);
        job.repo_name = req->name;
        const char *bytes[4];
        for (size_t i = 0; i < 4; i++) {
            bytes[i] = given[i] ? (const char *)(packed[i].data != NULL ? packed[i].data : "")
                                : NULL;
        }
        job.compdbs = bytes[0];
        job.compdbs_len = given[0] ? packed[0].len : 0;
        job.test_roots = bytes[1];
        job.test_roots_len = given[1] ? packed[1].len : 0;
        job.excludes = bytes[2];
        job.excludes_len = given[2] ? packed[2].len : 0;
        job.vendor_roots = bytes[3];
        job.vendor_roots_len = given[3] ? packed[3].len : 0;
        job.auto_rebuild = req->auto_rebuild;
        job.discovery_mode = req->discovery_mode;
        st = atlas_sem_config_on(atlas_ctx_db(ctx), &job, out, err);
    }
    for (size_t i = 0; i < 4; i++) {
        atlas_buf_free(&packed[i]);
    }
    return st;
}

/* --- symbols ------------------------------------------------------------------ */

void atlas_sem_symbols_report_init(atlas_sem_symbols_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_sem_generation_init(&r->generation);
}

void atlas_sem_symbols_report_free(atlas_sem_symbols_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    free(r->items);
    r->items = NULL;
}

typedef struct sym_sink {
    atlas_sem_symbols_report *rep;
    atlas_err *err;
    atlas_status st;
} sym_sink;

static atlas_status take_symbol(const atlas_sem_symbol_row *row, void *ud, atlas_err *err) {
    sym_sink *s = (sym_sink *)ud;
    if (s->rep->count >= s->rep->cap) {
        size_t ncap = s->rep->cap == 0 ? 32 : s->rep->cap * 2;
        atlas_sem_symbol_item *ni = realloc(s->rep->items, ncap * sizeof(*ni));
        if (ni == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory collecting symbols");
        }
        s->rep->items = ni;
        s->rep->cap = ncap;
    }
    /* Copied, not aliased: a row callback's pointers point into a live
     * statement and are valid for the call only. */
    atlas_sem_symbol_item *it = &s->rep->items[s->rep->count++];
    memset(it, 0, sizeof(*it));
    (void)snprintf(it->usr, sizeof(it->usr), "%s", row->usr);
    (void)snprintf(it->name, sizeof(it->name), "%s", row->name);
    (void)snprintf(it->kind, sizeof(it->kind), "%s", row->kind);
    (void)snprintf(it->linkage, sizeof(it->linkage), "%s", row->linkage);
    (void)snprintf(it->type_text, sizeof(it->type_text), "%s", row->type_text);
    (void)snprintf(it->file_text, sizeof(it->file_text), "%s", row->file_text);
    (void)snprintf(it->evidence, sizeof(it->evidence), "%s", row->evidence);
    it->line = row->line;
    it->col = row->col;
    it->end_line = row->end_line;
    it->is_definition = row->is_definition;
    it->external = row->external;
    return ATLAS_OK;
}

atlas_status atlas_service_sem_symbol(atlas_ctx *ctx, const char *name, const char *symbol,
                                      const char *kind, int64_t limit,
                                      atlas_sem_symbols_report *out, atlas_err *err) {
    atlas_status st =
        begin_read(ctx, name, &out->repo, &out->generation, &out->freshness, &out->stale_reason,
                   &out->trust,
                   err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->query, sizeof(out->query), "%s", symbol != NULL ? symbol : "");

    sym_sink sink = {out, err, ATLAS_OK};
    /* A name that resolves to several symbols returns all of them. Choosing one
     * would be inventing — A3's rule about ambiguity, and the reason `code
     * symbol` is how a caller disambiguates before asking for callers. */
    st = atlas_db_sem_symbols_by_name(atlas_ctx_db(ctx), out->generation.id, symbol, NULL, kind,
                                      limit, take_symbol, &sink, &out->total, &out->truncated,
                                      err);
    if (st == ATLAS_OK) {
        atlas_sem_trust_settle(&out->trust, (int64_t)out->count, out->truncated);
    }
    return st;
}

/* --- the call graph ------------------------------------------------------------ */

void atlas_sem_graph_report_init(atlas_sem_graph_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_repo_info_init(&r->repo);
    atlas_sem_generation_init(&r->generation);
}

void atlas_sem_graph_report_free(atlas_sem_graph_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
    free(r->items);
    r->items = NULL;
}

typedef struct walk_sink {
    atlas_sem_graph_report *rep;
} walk_sink;

static atlas_status take_walk(const atlas_sem_walk_row *row, void *ud, atlas_err *err) {
    walk_sink *s = (walk_sink *)ud;
    if (s->rep->count >= s->rep->cap) {
        size_t ncap = s->rep->cap == 0 ? 32 : s->rep->cap * 2;
        atlas_sem_graph_item *ni = realloc(s->rep->items, ncap * sizeof(*ni));
        if (ni == NULL) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory collecting graph rows");
        }
        s->rep->items = ni;
        s->rep->cap = ncap;
    }
    atlas_sem_graph_item *it = &s->rep->items[s->rep->count++];
    memset(it, 0, sizeof(*it));
    it->depth = row->depth;
    (void)snprintf(it->usr, sizeof(it->usr), "%s", row->usr);
    (void)snprintf(it->name, sizeof(it->name), "%s", row->name);
    (void)snprintf(it->file_text, sizeof(it->file_text), "%s", row->file_text);
    (void)snprintf(it->edge_kind, sizeof(it->edge_kind), "%s", row->edge_kind);
    (void)snprintf(it->via_name, sizeof(it->via_name), "%s", row->via_name);
    (void)snprintf(it->evidence, sizeof(it->evidence), "%s", row->evidence);
    (void)snprintf(it->site_file, sizeof(it->site_file), "%s", row->site_file);
    it->line = row->line;
    it->site_line = row->site_line;
    it->candidate_total = row->candidate_total;
    return ATLAS_OK;
}

/* Resolves a symbol name to exactly one USR, or refuses with the alternatives.
 *
 * A caller asking "who calls parse()" when there are three `parse`s must not be
 * given one of them silently. The refusal carries the candidates so the next
 * command can name the right one, and **ambiguity stays a usage error**: it is a
 * question Atlas cannot answer as asked, which is what exit 2 means.
 *
 * A9.2.5 split the *other* refusal out of that class. "No symbol of that name is
 * in the index" was also ATLAS_ERR_USAGE, and it is not an operator's typo — it
 * is a statement about what the index holds, and over an incomplete generation
 * it cannot even be that: the symbol may sit in a file the compilation database
 * never named. It is reported through `found` and settles as an ordinary
 * verdict, so a caller is told UNKNOWN rather than being blamed. */
static atlas_status resolve_one(atlas_ctx *ctx, int64_t gen, const char *symbol, atlas_buf *usr_out,
                                bool *found, atlas_err *err) {
    atlas_sem_symbols_report tmp;
    atlas_sem_symbols_report_init(&tmp);
    sym_sink sink = {&tmp, err, ATLAS_OK};
    int64_t total = 0;
    bool trunc = false;
    atlas_status st = atlas_db_sem_symbols_by_name(atlas_ctx_db(ctx), gen, symbol, NULL, NULL,
                                                   ATLAS_SEM_MAX_ROWS, take_symbol, &sink, &total,
                                                   &trunc, err);
    if (st != ATLAS_OK) {
        atlas_sem_symbols_report_free(&tmp);
        return st;
    }
    if (tmp.count == 0) {
        atlas_sem_symbols_report_free(&tmp);
        if (found != NULL) {
            *found = false;
        }
        return ATLAS_OK;
    }

    /* Distinct entities, not distinct rows: a declaration and its definition
     * share a USR and are one answer. */
    size_t distinct = 0;
    for (size_t i = 0; i < tmp.count; i++) {
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(tmp.items[i].usr, tmp.items[j].usr) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            distinct++;
        }
    }
    if (distinct > 1) {
        atlas_buf list = ATLAS_BUF_INIT;
        for (size_t i = 0; i < tmp.count && i < 8; i++) {
            (void)atlas_buf_appendf(&list, err, "%s%s at %s:%lld", i == 0 ? "" : ", ",
                                    tmp.items[i].kind, tmp.items[i].file_text,
                                    (long long)tmp.items[i].line);
        }
        atlas_status amb = atlas_err_set(
            err, ATLAS_ERR_USAGE,
            "\"%s\" names %zu distinct symbols in this index (%s); ask for one by its file with "
            "`atlas code symbol`",
            symbol, distinct, atlas_buf_cstr(&list));
        atlas_buf_free(&list);
        atlas_sem_symbols_report_free(&tmp);
        return amb;
    }

    st = atlas_buf_set_str(usr_out, tmp.items[0].usr, err);
    if (st == ATLAS_OK && found != NULL) {
        *found = true;
    }
    atlas_sem_symbols_report_free(&tmp);
    return st;
}

atlas_status atlas_service_sem_graph(atlas_ctx *ctx, const char *name, const char *symbol,
                                     bool inbound, int64_t depth, int64_t limit, bool proven_only,
                                     atlas_sem_graph_report *out, atlas_err *err) {
    atlas_status st =
        begin_read(ctx, name, &out->repo, &out->generation, &out->freshness, &out->stale_reason,
                   &out->trust,
                   err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->query, sizeof(out->query), "%s", symbol != NULL ? symbol : "");
    out->inbound = inbound;

    atlas_buf usr = ATLAS_BUF_INIT;
    bool found = false;
    st = resolve_one(ctx, out->generation.id, symbol, &usr, &found, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&usr);
        return st;
    }
    if (!found) {
        /* The subject is not in this generation. That is an empty result set,
         * not a refusal, and it settles exactly like any other: ABSENT over a
         * generation that read the whole tree, UNKNOWN over one that did not. */
        atlas_buf_free(&usr);
        atlas_sem_trust_settle(&out->trust, 0, false);
        return ATLAS_OK;
    }

    atlas_sem_walk_opts o;
    atlas_sem_walk_opts_init(&o);
    o.usr = atlas_buf_cstr(&usr);
    o.inbound = inbound;
    o.depth = depth;
    o.max_rows = limit;
    o.proven_only = proven_only;

    walk_sink sink = {out};
    st = atlas_sem_walk(atlas_ctx_db(ctx), out->generation.id, &o, take_walk, &sink, &out->summary,
                        err);
    atlas_buf_free(&usr);
    if (st == ATLAS_OK) {
        atlas_sem_trust_settle(&out->trust, (int64_t)out->count, out->summary.truncated);
    }
    return st;
}

atlas_status atlas_service_sem_trace(atlas_ctx *ctx, const char *name, const char *from,
                                     const char *to, int64_t depth, atlas_sem_graph_report *out,
                                     atlas_err *err) {
    atlas_status st =
        begin_read(ctx, name, &out->repo, &out->generation, &out->freshness, &out->stale_reason,
                   &out->trust,
                   err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->query, sizeof(out->query), "%s -> %s", from != NULL ? from : "",
                   to != NULL ? to : "");

    atlas_buf from_usr = ATLAS_BUF_INIT;
    atlas_buf to_usr = ATLAS_BUF_INIT;
    bool from_found = false;
    bool to_found = false;
    st = resolve_one(ctx, out->generation.id, from, &from_usr, &from_found, err);
    if (st == ATLAS_OK) {
        st = resolve_one(ctx, out->generation.id, to, &to_usr, &to_found, err);
    }
    /* Either end missing is an empty path set. A trace between two symbols one
     * of which this generation never read is precisely the case that must not
     * read as "there is no path". */
    if (st == ATLAS_OK && from_found && to_found) {
        walk_sink sink = {out};
        st = atlas_sem_trace(atlas_ctx_db(ctx), out->generation.id, atlas_buf_cstr(&from_usr),
                             atlas_buf_cstr(&to_usr), depth, 1, take_walk, &sink, &out->summary,
                             err);
    }
    atlas_buf_free(&from_usr);
    atlas_buf_free(&to_usr);
    if (st == ATLAS_OK) {
        atlas_sem_trust_settle(&out->trust, (int64_t)out->count, out->summary.truncated);
    }
    return st;
}

/* --- indexing ------------------------------------------------------------------
 *
 * The one mutating operation, and the only one in this file that needs a
 * writable handle. Under A7.1 the index is 0700 `atlasd`, so an operator's CLI
 * cannot perform this locally at all — it routes over the socket, and the
 * daemon offers the method only to the peer the root-owned policy names. This
 * function is what both paths call, so the local and remote forms cannot drift.
 */
atlas_status atlas_service_sem_index(atlas_ctx *ctx, const char *name, const char *const *compdbs,
                                     size_t compdb_count, bool rebuild,
                                     atlas_sem_index_summary *out, atlas_err *err) {
    if (!atlas_sem_available()) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "this Atlas was built without libclang, so it cannot build a "
                             "compiler-derived semantic index");
    }
    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    atlas_status st = atlas_service_require_repo(ctx, name, &repo, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&repo);
        return st;
    }

    /* A9.2.4. Naming no compilation database is no longer a usage error.
     *
     * A9.2.3 refused here because Atlas did not search a repository for a file
     * that tells it how to compile things. It does now, inside a bounded search
     * universe, so an operator asking to index a repository whose databases were
     * *discovered* rather than typed is asking a perfectly well-formed question —
     * and refusing it would have made `code index` the one command that could not
     * use the season's own mechanism.
     *
     * A named list is still honoured exactly as given: naming databases is a
     * deliberate act about a particular build, and discovery is not entitled to
     * overrule it. */
    atlas_buf accepted = ATLAS_BUF_INIT;
    const char *slots[ATLAS_SEM_MAX_COMPDBS];
    if (compdbs == NULL || compdb_count == 0) {
        atlas_sem_discovery_result res;
        atlas_sem_discovery_result_init(&res);
        atlas_err ignored;
        atlas_err_init(&ignored);
        /* Walk first, so an operator who has just generated a build directory
         * gets an index of it rather than of what Atlas last happened to see. */
        (void)atlas_sem_discovery_run(atlas_ctx_db(ctx), &repo, &res, &ignored);
        st = atlas_sem_accepted_inputs(atlas_ctx_db(ctx), repo.id, &accepted, NULL, err);
        if (st == ATLAS_OK) {
            const char *p = (const char *)accepted.data;
            const char *end = p != NULL ? p + accepted.len : NULL;
            while (p != NULL && p < end && *p != '\0' && compdb_count < ATLAS_SEM_MAX_COMPDBS) {
                slots[compdb_count++] = p;
                p += strlen(p) + 1u;
            }
            compdbs = slots;
        }
        if (st == ATLAS_OK && compdb_count == 0) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "build-input discovery found no compilation database in this "
                               "repository; generate one, or name it with --compdb");
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_sem_index_on(atlas_ctx_db(ctx), &repo, compdbs, compdb_count, rebuild, out,
                                err);
    }
    atlas_buf_free(&accepted);
    atlas_repo_info_free(&repo);
    return st;
}

/* The indexing core, over a raw handle and an already-resolved repository.
 *
 * The CLI reaches it through `atlas_service_sem_index` above, which resolves
 * the repository from a context it owns. The daemon reaches it from the writer
 * thread, which already holds the only writable handle and has resolved the
 * repository from the registry itself. One implementation, for the reason
 * `atlas_sem_impact_on` and `atlas_sem_context_on` are one each: parity between
 * the surfaces is structural rather than two functions somebody keeps in step.
 *
 * It creates git and parser processes and must therefore never be called with a
 * write transaction open — A1's rule, which `atlas_sem_index_run` observes by
 * chunking its own work. */
atlas_status atlas_sem_index_on(atlas_db *db, const atlas_repo_info *repo_in,
                                const char *const *compdbs, size_t compdb_count, bool rebuild,
                                atlas_sem_index_summary *out, atlas_err *err) {
    const atlas_repo_info repo = *repo_in;
    atlas_status st = ATLAS_OK;

    /* A NUL-separated list, which is how every bounded path list in Atlas is
     * carried: paths are bytes and must never be split on whitespace. */
    atlas_buf list = ATLAS_BUF_INIT;
    for (size_t i = 0; i < compdb_count && st == ATLAS_OK; i++) {
        st = atlas_buf_append(&list, compdbs[i], strlen(compdbs[i]) + 1, err);
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&list);
        return st;
    }

    /* The repository is addressed through the git adapter, which is what
     * verifies the registration still describes this worktree and hands back a
     * root descriptor every later open is relative to. Atlas never chdirs and
     * never re-resolves a path from a string — A8's workspace rule, applied to
     * reading a repository. */
    atlas_git *g = NULL;
    st = atlas_git_open(atlas_buf_cstr(&repo.root_path), &g, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&list);
        return st;
    }

    /* The child parser is this same binary, resolved from `/proc/self/exe` so a
     * PATH lookup can never select a different one. */
    atlas_buf exe = ATLAS_BUF_INIT;
    st = atlas_unit_self_path(&exe, err);

    /* A9.2.3. The durable identity, so a generation can still say which
     * repository lineage it described after the rowid is gone — every other
     * durable Atlas record that references a repository carries it, and this one
     * had a column for it that nothing ever filled. Best effort: a repository
     * whose identity cannot be computed still gets an index, with the field
     * empty and honestly so, rather than no index at all. */
    atlas_buf identity = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        atlas_err ignored;
        atlas_err_init(&ignored);
        if (atlas_db_repo_identity_hash(db, repo.id, &identity, &ignored) != ATLAS_OK) {
            atlas_buf_reset(&identity);
        }
    }

    /* A9.2.3. The operator's declared test roots, used only to classify this
     * generation's units at publication. It changes nothing about what is
     * parsed: excluding tests from the *index* would make a caller in a test
     * invisible rather than merely labelled, which is the opposite of what the
     * test-scope question needs. */
    atlas_sem_config cfg;
    atlas_sem_config_init(&cfg);
    if (st == ATLAS_OK) {
        atlas_err ignored;
        atlas_err_init(&ignored);
        (void)atlas_db_sem_config_get(db, repo.id, &cfg, &ignored);
    }

    if (st == ATLAS_OK) {
        atlas_sem_index_opts o;
        atlas_sem_index_opts_init(&o);
        o.compdbs = (const char *)list.data;
        o.compdbs_len = list.len;
        o.rebuild = rebuild;
        o.atlas_exe = atlas_buf_cstr(&exe);
        o.root = atlas_git_root(g);
        o.root_fd = atlas_git_root_fd(g);
        o.commit_id = repo.scanned_head;
        o.repo_identity_hash = atlas_buf_cstr(&identity);
        o.test_roots = atlas_buf_cstr(&cfg.test_roots);
        st = atlas_sem_index_run(db, repo.id, &o, out, err);
    }

    atlas_sem_config_free(&cfg);
    atlas_buf_free(&identity);
    atlas_buf_free(&exe);
    atlas_git_close(g);
    atlas_buf_free(&list);
    return st;
}

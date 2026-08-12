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
#include "atlas/sem_ops.h"
#include "atlas/service.h"
#include "atlas/unit.h"
#include "service_internal.h"

/* --- resolving the generation to answer from -------------------------------- */

/* Loads the published generation and computes its freshness against live facts.
 *
 * `found` false means ABSENT — nobody has indexed this repository — which is a
 * different answer from STALE and must stay one. Both are ordinary outcomes and
 * neither is an error: a caller is told what Atlas holds, and decides. */
static atlas_status load_generation(atlas_ctx *ctx, const atlas_repo_info *repo,
                                    atlas_sem_generation *gen, bool *found,
                                    atlas_sem_freshness *fresh, const char **reason,
                                    atlas_err *err) {
    atlas_sem_generation_init(gen);
    *fresh = ATLAS_SEM_FRESH_ABSENT;
    *reason = NULL;

    atlas_status st = atlas_db_sem_current(atlas_ctx_db(ctx), repo->id, gen, found, err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* A generation still being built is reported beside the one being served,
     * so "rebuilding" and "stale" are distinguishable. */
    atlas_sem_generation latest;
    bool have_latest = false;
    st = atlas_db_sem_latest(atlas_ctx_db(ctx), repo->id, &latest, &have_latest, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool running = have_latest && latest.status == ATLAS_SEM_GEN_RUNNING;

    /* The file index has to be current too: a semantic graph built on a file
     * index nobody can vouch for is not one to act on. */
    atlas_index_state fs;
    atlas_index_state_init(&fs);
    st = atlas_db_index_state_get(atlas_ctx_db(ctx), repo->id, &fs, err);
    /* The same three conditions `code status` uses: a pass has completed, no
     * unresolved event gap, and no owed full verification. Asked here rather
     * than restated, so the semantic layer and the structural one cannot
     * disagree about what "current" means. */
    bool file_current = st == ATLAS_OK && fs.present && fs.last_complete_generation > 0 &&
                        !fs.event_gap && !fs.pending_full_reconcile;
    atlas_index_state_free(&fs);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The live commit is the repository's own scanned head. The compilation
     * database digest is deliberately *not* recomputed here: doing so would
     * mean opening and hashing every compile database on every read, and a
     * changed one is caught by the next index rather than by every query. The
     * generation records the digest it used, and `code status` reports it, so
     * an operator can see which description the index was built from. */
    *fresh = atlas_sem_freshness_of(gen, *found, running, repo->scanned_head, NULL, file_current,
                                    reason);
    return ATLAS_OK;
}

/* Shared preamble: resolve the repository, load the generation, and refuse
 * clearly when there is nothing to answer from.
 *
 * The refusals are typed and distinct because an operator does different things
 * about them: build an index, rebuild a stale one, or install libclang. */
static atlas_status begin_read(atlas_ctx *ctx, const char *name, atlas_repo_info *repo,
                               atlas_sem_generation *gen, atlas_sem_freshness *fresh,
                               const char **reason, atlas_err *err) {
    atlas_status st = atlas_service_require_repo(ctx, name, repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    bool found = false;
    st = load_generation(ctx, repo, gen, &found, fresh, reason, err);
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
}

void atlas_sem_status_report_free(atlas_sem_status_report *r) {
    if (r == NULL) {
        return;
    }
    atlas_repo_info_free(&r->repo);
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

atlas_status atlas_service_sem_status(atlas_ctx *ctx, const char *name,
                                      atlas_sem_status_report *out, atlas_err *err) {
    atlas_status st = atlas_service_require_repo(ctx, name, &out->repo, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->libclang_available = atlas_sem_available();
    (void)snprintf(out->compiler_id, sizeof(out->compiler_id), "%s", atlas_sem_compiler_id());
    (void)snprintf(out->compiler_version, sizeof(out->compiler_version), "%s",
                   atlas_sem_compiler_version());

    bool found = false;
    st = load_generation(ctx, &out->repo, &out->generation, &found, &out->freshness,
                         &out->stale_reason, err);
    if (st != ATLAS_OK) {
        return st;
    }
    out->have_generation = found;

    /* The most recent attempt, whatever became of it. A failed index is an
     * operational fact and a status that only ever reported successes could not
     * state it. */
    st = atlas_db_sem_latest(atlas_ctx_db(ctx), out->repo.id, &out->latest, &out->have_latest, err);
    if (st != ATLAS_OK || !found) {
        return st;
    }

    unit_sink sink = {out};
    int64_t listed = 0;
    atlas_status ust = atlas_db_sem_failed_units(atlas_ctx_db(ctx), out->generation.id,
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
                   err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->query, sizeof(out->query), "%s", symbol != NULL ? symbol : "");

    sym_sink sink = {out, err, ATLAS_OK};
    /* A name that resolves to several symbols returns all of them. Choosing one
     * would be inventing — A3's rule about ambiguity, and the reason `code
     * symbol` is how a caller disambiguates before asking for callers. */
    return atlas_db_sem_symbols_by_name(atlas_ctx_db(ctx), out->generation.id, symbol, NULL, kind,
                                        limit, take_symbol, &sink, &out->total, &out->truncated,
                                        err);
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
 * command can name the right one. */
static atlas_status resolve_one(atlas_ctx *ctx, int64_t gen, const char *symbol, atlas_buf *usr_out,
                                atlas_err *err) {
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
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "no symbol named \"%s\" is in the semantic index", symbol);
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
    atlas_sem_symbols_report_free(&tmp);
    return st;
}

atlas_status atlas_service_sem_graph(atlas_ctx *ctx, const char *name, const char *symbol,
                                     bool inbound, int64_t depth, int64_t limit, bool proven_only,
                                     atlas_sem_graph_report *out, atlas_err *err) {
    atlas_status st =
        begin_read(ctx, name, &out->repo, &out->generation, &out->freshness, &out->stale_reason,
                   err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->query, sizeof(out->query), "%s", symbol != NULL ? symbol : "");
    out->inbound = inbound;

    atlas_buf usr = ATLAS_BUF_INIT;
    st = resolve_one(ctx, out->generation.id, symbol, &usr, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&usr);
        return st;
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
    return st;
}

atlas_status atlas_service_sem_trace(atlas_ctx *ctx, const char *name, const char *from,
                                     const char *to, int64_t depth, atlas_sem_graph_report *out,
                                     atlas_err *err) {
    atlas_status st =
        begin_read(ctx, name, &out->repo, &out->generation, &out->freshness, &out->stale_reason,
                   err);
    if (st != ATLAS_OK) {
        return st;
    }
    (void)snprintf(out->query, sizeof(out->query), "%s -> %s", from != NULL ? from : "",
                   to != NULL ? to : "");

    atlas_buf from_usr = ATLAS_BUF_INIT;
    atlas_buf to_usr = ATLAS_BUF_INIT;
    st = resolve_one(ctx, out->generation.id, from, &from_usr, err);
    if (st == ATLAS_OK) {
        st = resolve_one(ctx, out->generation.id, to, &to_usr, err);
    }
    if (st == ATLAS_OK) {
        walk_sink sink = {out};
        st = atlas_sem_trace(atlas_ctx_db(ctx), out->generation.id, atlas_buf_cstr(&from_usr),
                             atlas_buf_cstr(&to_usr), depth, 1, take_walk, &sink, &out->summary,
                             err);
    }
    atlas_buf_free(&from_usr);
    atlas_buf_free(&to_usr);
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
    if (compdbs == NULL || compdb_count == 0) {
        /* No search, ever. Atlas does not go looking through a repository for a
         * file that tells it how to compile things. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "name at least one compilation database with --compdb; Atlas does "
                             "not search a repository for one");
    }

    atlas_repo_info repo;
    atlas_repo_info_init(&repo);
    atlas_status st = atlas_service_require_repo(ctx, name, &repo, err);
    if (st != ATLAS_OK) {
        atlas_repo_info_free(&repo);
        return st;
    }
    st = atlas_sem_index_on(atlas_ctx_db(ctx), &repo, compdbs, compdb_count, rebuild, out, err);
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
        o.repo_identity_hash = "";
        st = atlas_sem_index_run(db, repo.id, &o, out, err);
    }

    atlas_buf_free(&exe);
    atlas_git_close(g);
    atlas_buf_free(&list);
    return st;
}

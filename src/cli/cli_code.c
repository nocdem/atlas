/* Atlas - command line front end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The CLI parses arguments, calls the service layer, and hands results to a
 * renderer. It contains no SQL, no git invocation and no output formatting.
 */
#include "atlas/cli.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/backup.h"
#include "atlas/gateway.h"
#include "atlas/daemon.h"
#include "atlas/hook.h"
#include "atlas/integrate.h"
#include "atlas/ipc.h"
#include "atlas/maintenance.h"
#include "atlas/mcp.h"
#include "atlas/sem.h"
#include "atlas/unit.h"
#include "cli/render.h"


#include "cli/cli_internal.h"

/* --- A3: the `code` command group ----------------------------------------
 *
 * A diagnostic surface. Normal AI use is automatic — the daemon indexes and the
 * MCP tools answer — and these exist so a person can see the same facts without
 * a model in the loop, and so a failure can be inspected rather than inferred.
 *
 * Every subcommand goes through the service layer and hands rows to the same
 * renderer interface both output modes implement, so the human and JSON forms
 * cannot describe a repository differently. */

static atlas_status code_symbol_sink(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->code_symbol_item(ls->r, row, err);
}

static atlas_status code_edge_sink(const atlas_code_edge_row *row, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->code_edge_item(ls->r, row, err);
}

static atlas_status code_walk_sink(const atlas_code_walk_row *row, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->code_walk_item(ls->r, row, err);
}


static const char SEM_LOCAL_ONLY[] =
    "NOT_AUTHORIZED: building a semantic index writes to the index, and this process cannot open "
    "it for writing; on a system deployment the index is owned by the `atlasd` account. Every "
    "semantic *read* is served over the socket.";

/* `atlas context build --repo R --task "..."`.
 *
 * The task description ranks evidence Atlas already holds and does nothing
 * else: it selects no repository, authorises nothing, and no imperative in it
 * can cause a write, because this path reaches only read functions. */
atlas_status atlas_cli_run_context(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    if (st->operand_count == 0 || strcmp(st->operands[0], "build") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas context build --repo NAME --task TEXT");
    }
    if (st->opts.repo == NULL || st->opts.task == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas context build needs --repo NAME and --task TEXT");
    }
    atlas_sem_context_req req;
    atlas_sem_context_req_init(&req);
    req.repo = st->opts.repo;
    req.task = st->opts.task;
    req.depth = st->opts.depth;
    req.max_tokens = st->opts.max_tokens;
    req.max_items = st->opts.limit;
    req.include_history = st->opts.history;

    /* A9.1: the seeds the request struct has always had a field for, and which no
     * command line could reach.
     *
     * `atlas_sem_context_req.paths` and `.symbols` are documented as "optional
     * starting points" and were filled by nothing, so `context build` could only
     * rank the whole symbol table by the task's words — and on a repository with
     * no semantic index that is nothing at all. The repeatable `--path` and
     * `--symbol-link` options were already parsed for `decision propose`, so this
     * wires them through rather than inventing a second spelling. NUL-separated,
     * which is the form the field documents. */
    atlas_buf seed_paths = ATLAS_BUF_INIT;
    atlas_buf seed_symbols = ATLAS_BUF_INIT;
    atlas_status seed_st = ATLAS_OK;
    for (size_t i = 0; seed_st == ATLAS_OK && i < st->opts.decision.path_count; i++) {
        seed_st = atlas_buf_append(&seed_paths, st->opts.decision.paths[i],
                                   strlen(st->opts.decision.paths[i]) + 1u, err);
    }
    for (size_t i = 0; seed_st == ATLAS_OK && i < st->opts.decision.symbol_count; i++) {
        seed_st = atlas_buf_append(&seed_symbols, st->opts.decision.symbols[i],
                                   strlen(st->opts.decision.symbols[i]) + 1u, err);
    }
    if (seed_st != ATLAS_OK) {
        atlas_buf_free(&seed_paths);
        atlas_buf_free(&seed_symbols);
        return seed_st;
    }
    req.paths = seed_paths.len > 0 ? seed_paths.data : NULL;
    req.paths_len = seed_paths.len;
    req.symbols = seed_symbols.len > 0 ? seed_symbols.data : NULL;
    req.symbols_len = seed_symbols.len;

    atlas_sem_context_report rep;
    atlas_sem_context_report_init(&rep);
    atlas_status result = ctx != NULL ? atlas_service_sem_context(ctx, &req, &rep, err)
                                      : atlas_service_sem_context_remote(&req, &rep, err);
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "context build", err);
        if (result == ATLAS_OK) {
            result = r->v->sem_context(r, &rep, err);
        }
        result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
    }
    atlas_sem_context_report_free(&rep);
    atlas_buf_free(&seed_paths);
    atlas_buf_free(&seed_symbols);
    return result;
}

atlas_status atlas_cli_run_code(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
                             atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas code status|sync|file|symbol|search|deps|impact"
                             "|index|semantic|callers|callees|trace ...");
    }
    const char *sub = st->operands[0];
    atlas_status result;

    /* --- A8-CI: the compiler-derived index ------------------------------
     *
     * Deliberately separate subcommands from the A3 ones below rather than a
     * flag on them. `code symbol` answers from the lexical graph and `code
     * semantic` from the compiler-derived one, and they are different questions
     * with different evidence — a `--semantic` switch would invite a reader to
     * treat one answer as an improved version of the other. */
    if (strcmp(sub, "index") == 0) {
        /* usage: atlas code index NAME [--compdb PATH]... [--rebuild]
         *
         * A9.2.4: `--compdb` is optional. With none, the databases are whatever
         * build-input discovery accepted — which is the ordinary case now, and
         * refusing it would have made `code index` the one command that could
         * not use the season's own mechanism. With some, those exactly: naming a
         * database is a deliberate act about a particular build and discovery is
         * not entitled to overrule it. */
        if (st->operand_count < 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas code index NAME [--compdb PATH]... [--rebuild]");
        }
        const char *const *compdbs = st->opts.compdbs;
        size_t ncompdb = st->opts.compdb_count;
        bool rebuild = st->opts.rebuild;
        atlas_sem_index_summary sum;
        atlas_sem_index_summary_init(&sum);
        /* Two routes, one behaviour.
         *
         * With a context this process holds the writer lock and indexes here.
         * Without one — which under A7.1 is every operator invocation, because
         * the index is 0700 `atlasd` — it goes over the socket, where the
         * daemon queues it on its writer thread and this client polls. Both
         * end in `atlas_sem_index_on`.
         *
         * The service does not have to be stopped and nobody has to become the
         * service account. That was the state before the closeout, and a
         * documented workaround standing in for a missing feature is a defect
         * rather than a procedure.
         *
         * A context that exists but cannot write is still refused, and refused
         * before any work starts: that is a real condition (another process
         * holds the lock on this data directory) and NOT_AUTHORIZED names it. */
        if (ctx != NULL && !atlas_ctx_is_writer(ctx)) {
            return atlas_err_set(err, ATLAS_ERR_CONFIG, "%s", SEM_LOCAL_ONLY);
        }
        result = ctx != NULL ? atlas_service_sem_index(ctx, st->operands[1], compdbs, ncompdb,
                                                       rebuild, &sum, err)
                             : atlas_service_sem_index_remote(st->operands[1], compdbs, ncompdb,
                                                              rebuild, &sum, err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code index", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_indexed(r, &sum, err);
            }
            result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
        }
        return result;
    }

    if (strcmp(sub, "semantic") == 0 || strcmp(sub, "callers") == 0 ||
        strcmp(sub, "callees") == 0 || strcmp(sub, "trace") == 0) {
        const char *repo_name = st->operand_count > 1u ? st->operands[1] : NULL;
        if (repo_name == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code %s NAME ...", sub);
        }

        if (strcmp(sub, "semantic") == 0) {
            if (st->operand_count < 3u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "usage: atlas code semantic NAME SYMBOL [KIND]");
            }
            atlas_sem_symbols_report rep;
            atlas_sem_symbols_report_init(&rep);
            const char *kind = st->operand_count > 3u ? st->operands[3] : NULL;
            result = ctx != NULL ? atlas_service_sem_symbol(ctx, repo_name, st->operands[2],
                                                            kind, limit, &rep, err)
                                 : atlas_service_sem_symbol_remote(repo_name, st->operands[2],
                                                                   kind, limit, &rep, err);
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code semantic", err);
                if (result == ATLAS_OK) {
                    result = r->v->sem_symbols(r, &rep, err);
                }
                result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
            }
            atlas_sem_symbols_report_free(&rep);
            return result;
        }

        atlas_sem_graph_report rep;
        atlas_sem_graph_report_init(&rep);
        int64_t depth = st->opts.depth > 0 ? st->opts.depth : ATLAS_SEM_DEFAULT_DEPTH;

        if (strcmp(sub, "trace") == 0) {
            if (st->operand_count < 4u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "usage: atlas code trace NAME FROM TO");
            }
            result = ctx != NULL
                         ? atlas_service_sem_trace(ctx, repo_name, st->operands[2],
                                                   st->operands[3], depth, &rep, err)
                         : atlas_service_sem_trace_remote(repo_name, st->operands[2],
                                                          st->operands[3], depth, &rep, err);
        } else {
            if (st->operand_count < 3u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code %s NAME SYMBOL",
                                     sub);
            }
            bool inbound = strcmp(sub, "callers") == 0;
            /* Depth 1 is the direct answer; a caller asking for more gets the
             * bounded transitive one and is told when a bound was reached. */
            result = ctx != NULL
                         ? atlas_service_sem_graph(ctx, repo_name, st->operands[2], inbound, depth,
                                                   limit, st->opts.proven_only, &rep, err)
                         : atlas_service_sem_graph_remote(repo_name, st->operands[2], inbound,
                                                          depth, limit, st->opts.proven_only,
                                                          &rep, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code graph", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_graph(r, &rep, err);
            }
            result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
        }
        atlas_sem_graph_report_free(&rep);
        return result;
    }

    /* `impact`, `tests` and `explain` are one report seen three ways: what a
     * change to the subject reaches. `tests` is that report filtered to test
     * files and `explain` is it with the subject's own definition first, so
     * three commands share one service call rather than three that could
     * disagree. */
    if (strcmp(sub, "sem-impact") == 0 || strcmp(sub, "tests") == 0 ||
        strcmp(sub, "explain") == 0) {
        if (st->operand_count < 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code %s NAME SYMBOL-OR-PATH",
                                 sub);
        }
        atlas_sem_impact_report rep;
        atlas_sem_impact_report_init(&rep);
        int64_t depth = st->opts.depth > 0 ? st->opts.depth : ATLAS_SEM_DEFAULT_DEPTH;
        result = ctx != NULL ? atlas_service_sem_impact(ctx, st->operands[1], st->operands[2],
                                                        depth, limit, &rep, err)
                             : atlas_service_sem_impact_remote(st->operands[1], st->operands[2],
                                                               depth, limit, &rep, err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code impact", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_impact(r, &rep, err);
            }
            result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
        }
        atlas_sem_impact_report_free(&rep);
        return result;
    }

    if (strcmp(sub, "sem-status") == 0) {
        if (st->operand_count != 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code sem-status NAME");
        }
        atlas_sem_status_report rep;
        atlas_sem_status_report_init(&rep);
        result = ctx != NULL ? atlas_service_sem_status(ctx, st->operands[1], &rep, err)
                             : atlas_service_sem_status_remote(st->operands[1], &rep, err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code sem-status", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_status(r, &rep, err);
            }
            result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
        }
        atlas_sem_status_report_free(&rep);
        return result;
    }

    /* usage: atlas code sem-config NAME [--compdb P]... [--test-root P]...
     *                               [--exclude P]... [--vendor-root P]...
     *                               [--discover|--no-discover] [--auto|--no-auto]
     *
     * With no flags it reads. An operator can therefore see the description
     * before changing it, which matters because every flag here is a
     * *replacement* of a list rather than an addition to one — repeating
     * `--compdb` builds the whole list, and a command that named one database
     * would otherwise silently drop the second.
     *
     * A9.2.4: `--compdb` no longer means "these are the compilation databases".
     * It means "these as well as whatever discovery finds", unless
     * `--no-discover` is also given — which makes the pinned list the whole of
     * it and, honestly, leaves discovery UNKNOWN. `--auto`/`--no-auto` now
     * record an *operator intent* that no machine-wide default can overrule in
     * either direction. */
    if (strcmp(sub, "sem-config") == 0) {
        if (st->operand_count != 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas code sem-config NAME [--compdb PATH]... "
                                 "[--test-root PATH]... [--no-test-roots] "
                                 "[--exclude PATH]... [--no-excludes] "
                                 "[--vendor-root PATH]... [--no-vendor-roots] "
                                 "[--discover|--no-discover] [--auto|--no-auto]");
        }
        const bool writing = st->opts.compdb_count > 0 || st->opts.test_roots_given ||
                             st->opts.excludes_given || st->opts.vendor_roots_given ||
                             st->opts.discovery_mode >= 0 || st->opts.auto_rebuild >= 0;
        atlas_sem_status_report rep;
        atlas_sem_status_report_init(&rep);
        if (!writing) {
            result = ctx != NULL ? atlas_service_sem_status(ctx, st->operands[1], &rep, err)
                                 : atlas_service_sem_status_remote(st->operands[1], &rep, err);
        } else {
            atlas_sem_config_request req;
            memset(&req, 0, sizeof req);
            req.name = st->operands[1];
            req.compdbs = st->opts.compdb_count > 0 ? st->opts.compdbs : NULL;
            req.compdb_count = st->opts.compdb_count;
            req.test_roots = st->opts.test_roots_given ? st->opts.test_roots : NULL;
            req.test_root_count = st->opts.test_root_count;
            req.excludes = st->opts.excludes_given ? st->opts.excludes : NULL;
            req.exclude_count = st->opts.exclude_count;
            req.vendor_roots = st->opts.vendor_roots_given ? st->opts.vendor_roots : NULL;
            req.vendor_root_count = st->opts.vendor_root_count;
            req.auto_rebuild = st->opts.auto_rebuild;
            req.discovery_mode = st->opts.discovery_mode;
            /* Routed on `atlas_ctx_is_writer`, never on `ctx != NULL`: with a
             * daemon running, a context in AUTO mode still opens read-only, and
             * the weaker test fails with "attempt to write a readonly
             * database". That is the A9.2.1 defect and it is not repeated. */
            result = (ctx != NULL && atlas_ctx_is_writer(ctx))
                         ? atlas_service_sem_config_set(ctx, &req, &rep, err)
                         : atlas_service_sem_config_set_remote(&req, &rep, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code sem-config", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_config(r, &rep, err);
            }
            result = result == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), result);
        }
        atlas_sem_status_report_free(&rep);
        return result;
    }

    if (strcmp(sub, "status") == 0) {
        if (st->operand_count != 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code status NAME");
        }
        atlas_code_status_report rep;
        atlas_code_status_report_init(&rep);
        result = ctx != NULL ? atlas_service_code_status(ctx, st->operands[1], &rep, err)
                             : atlas_service_code_status_remote(st->operands[1], &rep, err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code status", err);
            if (result == ATLAS_OK) {
                result = r->v->code_status(r, &rep, err);
            }
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_close(r, err);
            } else {
                atlas_cli_renderer_abort(r);
            }
        }
        atlas_code_status_report_free(&rep);
        return result;
    }

    if (strcmp(sub, "sync") == 0) {
        if (st->operand_count != 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas code sync NAME [--rebuild] [--wait]");
        }
        atlas_sync_report rep;
        atlas_sync_report_init(&rep);
        result = atlas_service_code_sync(ctx, st->operands[1], st->opts.rebuild, st->opts.wait,
                                         st->opts.timeout_ms, &rep, err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code sync", err);
            if (result == ATLAS_OK) {
                result = r->v->note_repo(r, st->operands[1], err);
            }
            if (result == ATLAS_OK) {
                result = r->v->sync(r, st->operands[1], &rep, err);
            }
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_close(r, err);
            } else {
                atlas_cli_renderer_abort(r);
            }
        }
        atlas_sync_report_free(&rep);
        return result;
    }

    if (strcmp(sub, "file") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code file NAME PATH");
        }
        atlas_code_file_report rep;
        atlas_code_file_report_init(&rep);
        result = ctx != NULL
                     ? atlas_service_code_file(ctx, st->operands[1], st->operands[2], &rep, err)
                     : atlas_service_code_file_remote(st->operands[1], st->operands[2], &rep, err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code file", err);
        }
        if (result == ATLAS_OK) {
            result = r->v->note_repo(r, st->operands[1], err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_file(r, &rep, err);
        }
        /* The lists follow the header, each with its own count, so a truncated
         * one is visible rather than looking like an empty one. */
        if (result == ATLAS_OK && rep.indexed) {
            list_sink ls = {r};
            int64_t count = 0;
            bool more = false;
            result = r->v->code_list_begin(r, "symbols", err);
            if (result == ATLAS_OK) {
                result = ctx != NULL ? atlas_service_code_file_symbols(
                                           ctx, st->operands[1], st->operands[2], limit,
                                           code_symbol_sink, &ls, &count, &more, err)
                                     : atlas_service_code_file_symbols_remote(
                                           st->operands[1], st->operands[2], limit,
                                           code_symbol_sink, &ls, &count, &more, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "symbols", "symbol", "symbols", count, more, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_begin(r, "includes", err);
            }
            if (result == ATLAS_OK) {
                result = ctx != NULL ? atlas_service_code_file_edges(
                                           ctx, st->operands[1], st->operands[2],
                                           "file_includes_file", false, limit, code_edge_sink, &ls,
                                           &count, &more, err)
                                     : atlas_service_code_file_edges_remote(
                                           st->operands[1], st->operands[2], "file_includes_file",
                                           false, limit, code_edge_sink, &ls, &count, &more, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "includes", "include", "includes", count, more,
                                             err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_begin(r, "dependents", err);
            }
            if (result == ATLAS_OK) {
                result = ctx != NULL ? atlas_service_code_file_edges(
                                           ctx, st->operands[1], st->operands[2],
                                           "file_depends_on_file", true, limit, code_edge_sink,
                                           &ls, &count, &more, err)
                                     : atlas_service_code_file_edges_remote(
                                           st->operands[1], st->operands[2],
                                           "file_depends_on_file", true, limit, code_edge_sink,
                                           &ls, &count, &more, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "dependents", "dependent", "dependents", count,
                                             more, err);
            }
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        atlas_code_file_report_free(&rep);
        return result;
    }

    if (strcmp(sub, "symbol") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code symbol NAME SYMBOL");
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code symbol", err);
        if (result == ATLAS_OK) {
            result = r->v->note_repo(r, st->operands[1], err);
        }
        list_sink ls = {r};
        int64_t count = 0;
        bool more = false;
        /* Every recorded site, not one. Two files' identically named statics are
         * two symbols, and answering with one would be choosing between things
         * Atlas has deliberately kept distinct. */
        if (result == ATLAS_OK) {
            result = r->v->code_list_begin(r, "sites", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_code_symbol_sites(
                                       ctx, st->operands[1], st->operands[2], limit,
                                       code_symbol_sink, &ls, &count, &more, err)
                                 : atlas_service_code_symbol_sites_remote(
                                       st->operands[1], st->operands[2], limit, code_symbol_sink,
                                       &ls, &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_end(r, "sites", "site", "sites", count, more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_begin(r, "callers", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_code_symbol_edges(
                                       ctx, st->operands[1], st->operands[2], true, limit,
                                       code_edge_sink, &ls, &count, &more, err)
                                 : atlas_service_code_symbol_edges_remote(
                                       st->operands[1], st->operands[2], true, limit,
                                       code_edge_sink, &ls, &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_end(r, "callers", "caller", "callers", count, more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_begin(r, "calls", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_code_symbol_edges(
                                       ctx, st->operands[1], st->operands[2], false, limit,
                                       code_edge_sink, &ls, &count, &more, err)
                                 : atlas_service_code_symbol_edges_remote(
                                       st->operands[1], st->operands[2], false, limit,
                                       code_edge_sink, &ls, &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_end(r, "calls", "call", "calls", count, more, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "search") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code search NAME QUERY");
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "code search", err);
        if (result == ATLAS_OK) {
            result = r->v->note_repo(r, st->operands[1], err);
        }
        if (result == ATLAS_OK) {
            list_sink ls = {r};
            int64_t count = 0;
            bool more = false;
            result = r->v->list_begin(r, "symbols", err);
            if (result == ATLAS_OK) {
                result = ctx != NULL ? atlas_service_code_symbol_search(
                                           ctx, st->operands[1], st->operands[2], NULL, limit,
                                           code_symbol_sink, &ls, &count, &more, err)
                                     : atlas_service_code_symbol_search_remote(
                                           st->operands[1], st->operands[2], NULL, limit,
                                           code_symbol_sink, &ls, &count, &more, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->list_end(r, "symbol", "symbols", count, err);
            }
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "deps") == 0 || strcmp(sub, "impact") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas code %s NAME PATH [--depth N] [--symbol]", sub);
        }
        /* Impact is inbound by definition; `deps` is outbound unless asked to
         * reverse. One traversal, two names for the two directions people
         * actually ask in. */
        bool inbound = (strcmp(sub, "impact") == 0) || st->opts.reverse;
        const char *path = st->opts.symbol ? NULL : st->operands[2];
        const char *symbol = st->opts.symbol ? st->operands[2] : NULL;

        result = atlas_cli_renderer_open(r, st->opts.json, st->out,
                               strcmp(sub, "impact") == 0 ? "code impact" : "code deps", err);
        if (result == ATLAS_OK) {
            result = r->v->note_repo(r, st->operands[1], err);
        }
        atlas_code_walk_summary sum;
        memset(&sum, 0, sizeof(sum));
        if (result == ATLAS_OK) {
            list_sink ls = {r};
            result = r->v->list_begin(r, "candidates", err);
            if (result == ATLAS_OK) {
                result = ctx != NULL ? atlas_service_code_walk(
                                           ctx, st->operands[1], path, symbol, inbound,
                                           st->opts.depth, limit, code_walk_sink, &ls, &sum, err)
                                     : atlas_service_code_walk_remote(
                                           st->operands[1], path, symbol, inbound, st->opts.depth,
                                           limit, code_walk_sink, &ls, &sum, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->list_end(r, "candidate", "candidates", sum.emitted, err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_walk_end(r, &sum, err);
            }
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown code subcommand \"%s\"", sub);
}

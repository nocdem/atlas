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

/* --- A12.1 T16: the `memory` command family ---------------------------------
 *
 * `job_render_ctx`/`job_open_late`/`emit_job`'s own shape, generalised across
 * all seven forms rather than copied: the renderer document is opened at the
 * first row a service call actually produces, never before, so a refusal that
 * happens before any row exists never leaves a half-written document behind
 * it (`job list`'s own comment explains why in full). `plural` is non-NULL
 * only for the two forms that stream more than one row (`scan`, `diff`); every
 * other form calls the sink exactly once. */
typedef struct memory_render_ctx {
    atlas_renderer *r;
    cli_state *st;
    const char *command;
    const char *plural;
    bool opened;
    int64_t count;
} memory_render_ctx;

static atlas_status memory_open_late(memory_render_ctx *mc, atlas_err *err) {
    if (mc->opened) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_cli_renderer_open(mc->r, mc->st->opts.json, mc->st->out, mc->command, err);
    if (st == ATLAS_OK && mc->plural != NULL) {
        st = mc->r->v->list_begin(mc->r, mc->plural, err);
    }
    if (st == ATLAS_OK) {
        mc->opened = true;
    }
    return st;
}

static atlas_status emit_memory(const atlas_memory_render *mr, void *ud, atlas_err *err) {
    memory_render_ctx *mc = (memory_render_ctx *)ud;
    atlas_status st = memory_open_late(mc, err);
    if (st == ATLAS_OK) {
        mc->count++;
        st = mc->r->v->memory_item(mc->r, mr, err);
    }
    return st;
}

/* Closes what `memory_open_late` opened, or reports a refusal that happened
 * before any row did -- `run_job`'s own two-branch ending, restated here
 * because it belongs to this ctx type rather than `job_render_ctx`'s. */
static atlas_status memory_finish(memory_render_ctx *mc, atlas_status result, atlas_err *err) {
    if (!mc->opened) {
        return result;
    }
    if (result == ATLAS_OK && mc->plural != NULL) {
        result = mc->r->v->list_end(mc->r, "result", mc->plural, mc->count, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(mc->r, err);
    } else {
        atlas_cli_renderer_abort(mc->r);
    }
    return result;
}

/* `ctx` may be NULL: `scan` and `reconcile` never dereference it (both always
 * submit through the daemon's own operator methods, T11's own design, so
 * there is nothing local to fall back to), and `status` falls back to the
 * `memory.status` RPC when it is NULL. `pack`, `diff`, `patch` and `trailer`
 * require it and refuse with a stated reason otherwise -- see
 * `src/core/service_memory.c`'s header for why they have no remote form in
 * this build. */
atlas_status atlas_cli_run_memory(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas memory <status|scan|reconcile|pack|diff|patch|trailer> "
                             "...");
    }
    const char *sub = st->operands[0];
    memory_render_ctx mc = {r, st, NULL, NULL, false, 0};
    atlas_status result;

    if (strcmp(sub, "status") == 0) {
        mc.command = "memory status";
        result = ctx != NULL ? atlas_service_memory_status(ctx, st->opts.repo, emit_memory, &mc, err)
                             : atlas_service_memory_status_remote(st->opts.repo, emit_memory, &mc,
                                                                  err);
    } else if (strcmp(sub, "scan") == 0) {
        mc.command = "memory scan";
        mc.plural = "results";
        int64_t count = 0;
        result = atlas_service_memory_scan(st->opts.repo, emit_memory, &mc, &count, err);
    } else if (strcmp(sub, "reconcile") == 0) {
        mc.command = "memory reconcile";
        result = atlas_service_memory_reconcile(st->opts.repo, emit_memory, &mc, err);
    } else if (strcmp(sub, "pack") == 0) {
        mc.command = "memory pack";
        result = atlas_service_memory_pack(ctx, st->opts.repo, st->opts.task, st->opts.memory.run,
                                           emit_memory, &mc, err);
    } else if (strcmp(sub, "diff") == 0) {
        mc.command = "memory diff";
        mc.plural = "results";
        int64_t count = 0;
        result = atlas_service_memory_diff(ctx, st->opts.repo, st->opts.memory.generation,
                                           emit_memory, &mc, &count, err);
    } else if (strcmp(sub, "patch") == 0) {
        mc.command = "memory patch";
        result = atlas_service_memory_patch(ctx, st->opts.repo, st->opts.memory.source, emit_memory,
                                            &mc, err);
    } else if (strcmp(sub, "trailer") == 0) {
        mc.command = "memory trailer";
        result = atlas_service_memory_trailer(ctx, st->opts.memory.run, st->opts.memory.reason,
                                              st->opts.memory.commit, st->opts.repo, emit_memory,
                                              &mc, err);
    } else {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown memory subcommand \"%s\"", sub);
    }
    return memory_finish(&mc, result, err);
}

/* The semantic commands need a writable-or-readable handle on the index
 * itself. Under A7.1 the index is 0700 `atlasd`, so an operator running this
 * from their own account has no context and is told exactly that rather than
 * being given an empty answer. Serving these over the socket is the daemon
 * side of the same work and is where this refusal goes away. */

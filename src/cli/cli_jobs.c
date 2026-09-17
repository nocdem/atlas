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

/* --- A8: `atlas job` and `atlas dispatcher` -------------------------------- */

/* The document is opened at the first row rather than before the call.
 *
 * **`renderer_open` is a claim, and it was being made before anything was
 * known.** It writes the header — including `"ok":true` — and a list arm then
 * opens its array, all before the daemon has been reached. The writer emits
 * straight to the `FILE*` and buffers nothing, so when the call then failed
 * those bytes could not be taken back: `main` wrote its error document into the
 * open array and nothing closed the outer one. Measured 2026-08-28 with the
 * suite's own checker: `job list`, `job get`, `job cancel`, `plan list` and
 * `plan status` all emitted **invalid JSON** against an absent daemon —
 * `atlas-jsoncheck` rejects each at the byte where the second document starts.
 * `docs/backlog.md` recorded this as "two documents on stdout", which understated
 * it; two documents can at least be parsed one after the other.
 *
 * Opening late costs nothing here because none of these calls streams: the
 * service layer completes the whole IPC round trip and only then forwards rows
 * out of the response it already holds. So a connection failure is known before
 * any row exists, and at that point the correct output is exactly what `main`
 * already produces on its own — one error document.
 *
 * The rows themselves cannot be buffered instead: `atlas_job_render` is all
 * borrowed pointers into the live response, valid only for the call. */
typedef struct job_render_ctx {
    atlas_renderer *r;
    cli_state *st;
    /* The document's command name, and — for a list — the array to open inside
     * it. `plural` is NULL for the single-item arms. */
    const char *command;
    const char *plural;
    bool opened;
} job_render_ctx;

static atlas_status job_open_late(job_render_ctx *jc, atlas_err *err) {
    if (jc->opened) {
        return ATLAS_OK;
    }
    atlas_status st =
        atlas_cli_renderer_open(jc->r, jc->st->opts.json, jc->st->out, jc->command, err);
    if (st == ATLAS_OK && jc->plural != NULL) {
        st = jc->r->v->list_begin(jc->r, jc->plural, err);
    }
    if (st == ATLAS_OK) {
        jc->opened = true;
    }
    return st;
}

static atlas_status emit_job(const atlas_job_render *jr, void *ud, atlas_err *err) {
    job_render_ctx *jc = (job_render_ctx *)ud;
    atlas_status st = job_open_late(jc, err);
    return st == ATLAS_OK ? jc->r->v->job_item(jc->r, jr, err) : st;
}

/* No `atlas_ctx`, deliberately.
 *
 * A job command speaks only to the daemon: orchestration state lives in the
 * index, `atlasd` is the only writer of it, and on a separated deployment no
 * other account can even open the file. Opening a context here would try to
 * prepare a data directory this uid does not own — which is exactly what it did
 * during the A8 cutover, and the reason these commands are dispatched before any
 * context is opened, alongside `dispatcher`, `backup` and `restore`. */
atlas_status atlas_cli_run_job(cli_state *st, atlas_renderer *r, int64_t limit, atlas_err *err) {
    const char *sub = st->operand_count > 0 ? st->operands[0] : NULL;
    if (sub == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas job <submit|run|run-status|get|list|cancel> "
                             "(try: atlas help)");
    }
    job_render_ctx jc = {r, st, NULL, NULL, false};
    atlas_status result;

    if (strcmp(sub, "submit") == 0) {
        atlas_job_submit_opts o;
        memset(&o, 0, sizeof(o));
        o.repo = st->opts.job.repo;
        o.task = st->opts.job.task;
        o.mode = st->opts.job.mode;
        o.driver = st->opts.job.driver;
        o.idempotency_key = st->opts.job.key;
        o.wall_timeout_ms = st->opts.job.wall_ms;
        o.idle_timeout_ms = st->opts.job.idle_ms;
        o.max_attempts = st->opts.job.attempts;
        /* A11.5a recorded that this arm filled eight fields and dropped
         * `gates`, so an operator watching their own `--gate` flags was told
         * the submission declared no verification command. It is fixed here
         * rather than left recorded because A10.1 needs it: freezing a memory
         * package before either arm of a comparison runs means creating both
         * runs first and driving them afterwards, and a repository-tree task
         * with no gate cannot be created at all. */
        for (size_t g = 0; g < st->opts.job.gate_count; g++) {
            o.gates[o.gate_count++] = st->opts.job.gates[g];
        }
        o.memory = st->opts.job.memory;
        o.parent = st->opts.job.parent;
        o.max_parallel = st->opts.job.parallel;
        jc.command = "job submit";
        result = atlas_service_job_submit(NULL, &o, emit_job, &jc, err);
    } else if (strcmp(sub, "get") == 0) {
        const char *job = st->operand_count > 1 ? st->operands[1] : NULL;
        jc.command = "job get";
        result = atlas_service_job_get(NULL, job, emit_job, &jc, err);
    } else if (strcmp(sub, "cancel") == 0) {
        const char *job = st->operand_count > 1 ? st->operands[1] : NULL;
        jc.command = "job cancel";
        result = atlas_service_job_cancel(NULL, job, emit_job, &jc, err);
    } else if (strcmp(sub, "run") == 0) {
        /* A11.1. The one surface that starts a worker, and it is in the
         * foreground because an operator asked for it in this terminal. It
         * schedules nothing and leaves nothing running behind it. */
        atlas_job_run_opts o;
        memset(&o, 0, sizeof(o));
        o.repo = st->opts.job.repo;
        o.task = st->opts.job.task;
        o.resume = st->opts.job.resume;
        o.mode = st->opts.job.mode;
        o.driver = st->opts.job.driver;
        o.idempotency_key = st->opts.job.key;
        o.wall_timeout_ms = st->opts.job.wall_ms;
        o.idle_timeout_ms = st->opts.job.idle_ms;
        for (size_t g = 0; g < st->opts.job.gate_count; g++) {
            o.gates[o.gate_count++] = st->opts.job.gates[g];
        }
        o.memory = st->opts.job.memory;
        o.max_parallel = st->opts.job.parallel;
        /* The narration goes to stderr, so `--json` still puts exactly one
         * document on stdout. */
        o.log = st->opts.json ? stderr : st->out;
        jc.command = "job run";
        result = atlas_service_job_run(NULL, &o, emit_job, &jc, err);
    } else if (strcmp(sub, "run-status") == 0) {
        const char *run = st->operand_count > 1 ? st->operands[1] : NULL;
        jc.command = "job run-status";
        result = atlas_service_job_run_status(NULL, run, emit_job, &jc, err);
    } else if (strcmp(sub, "list") == 0) {
        jc.command = "job list";
        jc.plural = "jobs";
        int64_t count = 0;
        bool more = false;
        result = atlas_service_job_list(NULL, 0, limit, st->opts.remote, emit_job, &jc,
                                            &count, &more, err);
        /* An empty list is still an answer, so the document is opened here when
         * no row opened it. */
        if (result == ATLAS_OK) {
            result = job_open_late(&jc, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "job", "jobs", count, err);
        }
    } else {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown job subcommand \"%s\"", sub);
    }

    /* Nothing to close or abort when the call failed before a row: no document
     * was opened, no byte was written, and `main` emits the one error document
     * on its own. */
    if (!jc.opened) {
        return result;
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(r, err);
    } else {
        atlas_cli_renderer_abort(r);
    }
    return result;
}

/* --- A12.0: `atlas plan` --------------------------------------------------- */

/* Opened at the first row, for `job_render_ctx`'s reason and by the same
 * mechanism. The `plan show` arm below already refused *its own* argument errors
 * before opening a renderer, with a comment saying why; this extends the same
 * treatment to the refusals that come back from the daemon, which is where the
 * invalid documents were actually being produced. */
typedef struct plan_render_ctx {
    atlas_renderer *r;
    cli_state *st;
    const char *command;
    const char *plural;
    bool opened;
} plan_render_ctx;

static atlas_status plan_open_late(plan_render_ctx *pc, atlas_err *err) {
    if (pc->opened) {
        return ATLAS_OK;
    }
    atlas_status st =
        atlas_cli_renderer_open(pc->r, pc->st->opts.json, pc->st->out, pc->command, err);
    if (st == ATLAS_OK && pc->plural != NULL) {
        st = pc->r->v->list_begin(pc->r, pc->plural, err);
    }
    if (st == ATLAS_OK) {
        pc->opened = true;
    }
    return st;
}

static atlas_status emit_plan(const atlas_plan_render *pr, void *ud, atlas_err *err) {
    plan_render_ctx *pc = (plan_render_ctx *)ud;
    atlas_status st = plan_open_late(pc, err);
    return st == ATLAS_OK ? pc->r->v->plan_item(pc->r, pr, err) : st;
}

/* No `atlas_ctx`, deliberately, and for `run_job`'s reason exactly: a plan
 * command speaks only to the daemon. Plan state lives in the index, `atlasd` is
 * the only writer of it, and on a separated deployment no other account can open
 * the file at all. These are dispatched before any context is opened, alongside
 * `job` and `dispatcher`. */
atlas_status atlas_cli_run_plan(cli_state *st, atlas_renderer *r, int64_t limit, atlas_err *err) {
    const char *sub = st->operand_count > 0 ? st->operands[0] : NULL;
    if (sub == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas plan <run|status|show|list> (try: atlas help)");
    }
    plan_render_ctx pc = {r, st, NULL, NULL, false};
    atlas_status result;

    if (strcmp(sub, "run") == 0) {
        /* A12.0. The one surface that starts a planner, and it is in the
         * foreground because an operator asked for it in this terminal. It
         * schedules nothing and leaves nothing running behind it. */
        atlas_plan_run_opts o;
        memset(&o, 0, sizeof(o));
        o.repo = st->opts.job.repo;
        o.goal = st->opts.plan.goal;
        o.resume = st->opts.job.resume;
        for (size_t g = 0; g < st->opts.job.gate_count; g++) {
            o.gates[o.gate_count++] = st->opts.job.gates[g];
        }
        o.max_parallel = st->opts.job.parallel;
        /* The narration goes to stderr, so `--json` still puts exactly one
         * document on stdout. */
        o.log = st->opts.json ? stderr : st->out;
        pc.command = "plan run";
        result = atlas_service_plan_run(NULL, &o, emit_plan, &pc, err);
    } else if (strcmp(sub, "status") == 0) {
        const char *plan = st->operand_count > 1 ? st->operands[1] : NULL;
        if (plan == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas plan status PLAN");
        }
        pc.command = "plan status";
        result = atlas_service_plan_status(NULL, plan, emit_plan, &pc, err);
    } else if (strcmp(sub, "show") == 0) {
        const char *plan = st->operand_count > 1 ? st->operands[1] : NULL;
        if (plan == NULL || st->opts.plan.rev <= 0) {
            /* Refused here, before a renderer is opened, so `--json` puts one
             * document on stdout rather than a partial one and an error. The
             * service layer refuses the same thing for any other caller — it is
             * the contract — and this is the same arrangement `job submit` has
             * with the daemon's own refusals. */
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas plan show PLAN --rev N");
        }
        pc.command = "plan show";
        result =
            atlas_service_plan_show(NULL, plan, (int)st->opts.plan.rev, emit_plan, &pc, err);
    } else if (strcmp(sub, "list") == 0) {
        pc.command = "plan list";
        pc.plural = "plans";
        int64_t count = 0;
        bool more = false;
        result = atlas_service_plan_list(NULL, 0, limit, emit_plan, &pc, &count, &more, err);
        /* An empty list is still an answer. */
        if (result == ATLAS_OK) {
            result = plan_open_late(&pc, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "plan", "plans", count, err);
        }
    } else {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown plan subcommand \"%s\"", sub);
    }

    /* Nothing was opened when the call failed before a row; see `run_job`. */
    if (!pc.opened) {
        return result;
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(r, err);
    } else {
        atlas_cli_renderer_abort(r);
    }
    return result;
}

/* Whether the index this invocation names belongs to another account.
 *
 * Keyed on the data directory's *source* plus its ownership rather than on the
 * path, so an explicit `--data-dir` or `ATLAS_DATA_DIR` still means exactly
 * what it says and fixtures, tests and per-user daemons behave as they always
 * did on a machine that carries a system policy.
 *
 * Extracted because two places need the same answer and they are not adjacent:
 * `backup` is dispatched before any context is opened, and the general remote
 * decision happens later. Two copies of this test would eventually disagree,
 * and the one that disagreed would decide whether a write went to the daemon or
 * to a database this process cannot open. */

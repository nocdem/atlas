/* Atlas - CLI feature module contracts. Private to these front-end modules.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#ifndef ATLAS_CLI_INTERNAL_H
#define ATLAS_CLI_INTERNAL_H

#include "atlas/cli.h"
#include "cli/render.h"

#define ATLAS_DEFAULT_LIMIT 50

typedef struct cli_state {
    atlas_cli_opts opts;
    const char *command;
    const char *operands[8];
    size_t operand_count;
    FILE *out;
    FILE *errout;
    /* Set once a renderer has written a complete document. A command that has
     * already produced valid output but wants a non-zero exit code — `daemon
     * ping` against a daemon that is not running — must not also emit an error
     * document, because in --json mode that would put two documents on stdout. */
    bool rendered;
    /* A6. A gate outcome is not an error, so it cannot travel back as an
     * `atlas_status`: BLOCKED is a complete, correct, successfully produced
     * answer that must not exit zero. This carries the process exit code
     * separately, and `atlas_cli_main` prefers it over the status only when the
     * command itself succeeded. */
    int gate_exit;
} cli_state;

typedef struct list_sink {
    atlas_renderer *r;
} list_sink;

typedef struct file_sink {
    atlas_renderer *r;
    cli_state *st;
    const char *repo;
    bool opened;
} file_sink;


/* cli_args.c */
atlas_status atlas_cli_parse_args(cli_state *st, int argc, char **argv, bool *want_help,
                               bool *want_version, atlas_err *err);

/* cli_common.c */
atlas_status atlas_cli_take_name(cli_state *st, const char **name_out, atlas_err *err);
atlas_status atlas_cli_take_scanner_uid(cli_state *st, bool *given_out, int64_t *uid_out,
                                     atlas_err *err);
atlas_status atlas_cli_renderer_open(atlas_renderer *r, bool json, FILE *out, const char *command,
                                  atlas_err *err);
atlas_status atlas_cli_renderer_close(atlas_renderer *r, atlas_err *err);
void atlas_cli_renderer_abort(atlas_renderer *r);
atlas_status atlas_cli_repo_item_sink(const atlas_repo_info *ri, void *ud, atlas_err *err);
atlas_status atlas_cli_search_item_sink(const atlas_search_hit *h, void *ud, atlas_err *err);
atlas_status atlas_cli_history_item_sink(const atlas_history_row *h, void *ud, atlas_err *err);
atlas_status atlas_cli_diff_item_sink(const atlas_diff_entry *e, void *ud, atlas_err *err);
atlas_status atlas_cli_file_report_sink(const atlas_file_report *rep, void *ud, atlas_err *err);
atlas_status atlas_cli_need_operands(const cli_state *st, size_t want, const char *usage,
                                  atlas_err *err);
void atlas_cli_scan_opts_from_cli(const cli_state *st, atlas_scan_opts *so);
atlas_status atlas_cli_event_item_sink(const atlas_event_row *row, void *ud, atlas_err *err);

/* cli_daemon.c */
atlas_status atlas_cli_run_daemon(cli_state *st, atlas_err *err);
atlas_status atlas_cli_run_service(cli_state *st, atlas_err *err);
atlas_status atlas_cli_run_integrate(cli_state *st, atlas_err *err);
atlas_status atlas_cli_run_daemon_ping(cli_state *st, atlas_err *err);
atlas_ctx_mode atlas_cli_mode_for(const cli_state *st);
bool atlas_cli_route_to_daemon(const cli_state *st);
atlas_status atlas_cli_call_daemon_mutation(cli_state *st, const char *repo, const char *method,
                                         const char *params,
                                         const char *command, atlas_err *err);

/* cli_decision.c */
atlas_status atlas_cli_run_verify(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err);
atlas_status atlas_cli_run_gate(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err);
atlas_status atlas_cli_run_review(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err);
atlas_status atlas_cli_run_decision(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
                                 atlas_err *err);

/* cli_remote.c */
bool atlas_cli_remote_serves(const cli_state *st);
bool atlas_cli_is_a_command(const char *cmd);
atlas_status atlas_cli_remote_refuse(const cli_state *st, atlas_err *err);

/* cli_memory.c */
atlas_status atlas_cli_run_memory(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err);

/* cli_code.c */
atlas_status atlas_cli_run_context(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err);
atlas_status atlas_cli_run_code(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
                             atlas_err *err);

/* cli_jobs.c */
atlas_status atlas_cli_run_job(cli_state *st, atlas_renderer *r, int64_t limit, atlas_err *err);
atlas_status atlas_cli_run_plan(cli_state *st, atlas_renderer *r, int64_t limit, atlas_err *err);

/* cli_dispatch.c */
atlas_status atlas_cli_run_command(cli_state *st, atlas_err *err);

#endif /* ATLAS_CLI_INTERNAL_H */

/* Atlas - command line front end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Grammar (documented in README.md):
 *   atlas [GLOBAL]... COMMAND [GLOBAL]... [ARGS]...
 * Global options are accepted before or after the subcommand; "--" ends option
 * parsing so that operands beginning with '-' can still be passed.
 */
#ifndef ATLAS_CLI_H
#define ATLAS_CLI_H

#include <stdbool.h>
#include <stdio.h>

#include "atlas/error.h"

typedef struct atlas_cli_opts {
    bool json;
    bool quiet;
    bool yes;
    bool no_history;
    bool no_untracked;
    /* A1 */
    bool wait;
    bool full;
    bool user;   /* `service install --user`: required, and the only mode */
    bool force;
    bool run_once; /* `daemon run --once`: test hook, undocumented in help */
    /* A3. `rebuild` discards the structural index rather than reindexing what
     * changed — a separate request from `full`, which re-reads file content and
     * still parses nothing when the hashes match. */
    bool rebuild;
    bool reverse; /* `code deps`: report what depends on this instead */
    bool symbol;  /* `code deps`/`code impact`: the operand is a symbol name */
    long depth;
    long since;
    const char *data_dir;
    long limit;
    long max_commits;
    int timeout_ms;
} atlas_cli_opts;

/* Runs one command line. `argv[0]` is the program name. Returns the process
 * exit code. Diagnostics go to `errout`; results to `out`. */
int atlas_cli_main(int argc, char **argv, FILE *out, FILE *errout);

void atlas_cli_print_help(FILE *out);
void atlas_cli_print_version(FILE *out, bool json);

#endif /* ATLAS_CLI_H */

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
#include <stddef.h>
#include <stdio.h>

#include "atlas/error.h"
#include "atlas/limits.h"

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
    /* A4. The decision-document fields, grouped rather than spread through the
     * flat set above: there are a dozen of them, they are used by two
     * subcommands, and mixing them in with `--full` and `--depth` would make
     * the option list unreadable for every other command. */
    struct {
        const char *title;
        const char *context_text;
        const char *decision_text;
        const char *rationale;
        const char *consequences;
        const char *scope;
        const char *status;   /* `decision list --status` */
        const char *by;       /* `decision supersede --by` */
        const char *format;   /* `decision export --format` */
        const char *dedup_key;
        /* A6. `gate check --at OID`: the exact repository state the caller is
         * asking about. Naming one Atlas has not indexed is INDEX_LAG and so
         * BLOCKED, never an extrapolation to a state Atlas has never seen. */
        const char *at_commit;
        long revision;        /* 0 means the effective revision */
        /* Repeatable options. Bounded by the same ceilings the storage layer
         * enforces, and refused past them rather than truncated. */
        const char *alternatives[ATLAS_DECISION_MAX_ALTERNATIVES];
        size_t alternative_count;
        const char *paths[ATLAS_DECISION_MAX_LINKS];
        size_t path_count;
        const char *commits[ATLAS_DECISION_MAX_LINKS];
        size_t commit_count;
        const char *symbols[ATLAS_DECISION_MAX_LINKS];
        size_t symbol_count;
        /* Repeatable `--decision-link`: the uids this decision relates to. A
         * general reference, not a lifecycle one — see
         * `ATLAS_DECISION_LINK_RELATES_TO`. */
        const char *decision_links[ATLAS_DECISION_MAX_LINKS];
        size_t decision_link_count;
    } decision;
    /* A5. `apply` is separate from `yes` on purpose: `--yes` confirms an
     * operation the user already named, while `--apply` is what turns
     * `maintenance` from a report into a deletion. A single flag would make
     * "confirm this restore" and "actually delete rows" the same word. */
    bool apply;
    long older_than_days;
    long retain;
    /* A8. `atlas job submit` and friends. Every one of these is a *request*;
     * the daemon resolves the repository, pins the commit and applies the
     * policy's ceilings, so nothing here is trusted as given. */
    struct {
        const char *repo;
        const char *task;
        const char *mode;
        const char *driver;
        const char *key; /* idempotency key */
        long wall_ms;
        long idle_ms;
        long attempts;
        /* `atlas dispatcher run --once`: take at most one job and stop. How the
         * live smoke drives exactly one attempt without a service. */
        bool once;
    } job;
} atlas_cli_opts;

/* Runs one command line. `argv[0]` is the program name. Returns the process
 * exit code. Diagnostics go to `errout`; results to `out`. */
int atlas_cli_main(int argc, char **argv, FILE *out, FILE *errout);

void atlas_cli_print_help(FILE *out);
void atlas_cli_print_version(FILE *out, bool json);

#endif /* ATLAS_CLI_H */

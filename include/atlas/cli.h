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
    /* A8-CI: follow only compiler-proven edges. A caller that wants certainty
     * asks for it explicitly rather than being handed it silently. */
    bool proven_only;
    /* A8-CI `context build`. `--task` is free text used only for ranking. */
    const char *repo;
    const char *task;
    long max_tokens;
    bool history;
    /* A8-CI: `code index --compdb PATH`, repeatable. Repository-relative, and
     * never discovered: Atlas does not search a repository for a file that
     * tells it how to compile things. */
    const char *compdbs[32];
    size_t compdb_count;
    /* A9: `api-key create --label L --scope S [--scope S...]`.
     *
     * `--scope` is repeatable and every value must be in the closed vocabulary;
     * an unrecognised one is a refusal rather than something dropped, because a
     * credential granted fewer scopes than were written down is a credential
     * whose behaviour nobody can predict from the command that made it. */
    const char *label;
    const char *scopes[16];
    size_t scope_count;
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
        /* A9.1. `--kind`: which sort of knowledge record. On `propose` it says
         * what to create; on `list`, `search` and `for-file` it filters; on
         * `revise` it is an assertion Atlas checks against the document and
         * refuses if it differs. One flag because it is one question, and the
         * two dimensions stay separate — `--kind` never affects `--status` and
         * neither implies the other. */
        const char *kind;
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
        /* Migration 10: `--why`, the durable reason a relation exists or was
         * withdrawn. Prose, and deliberately not the same option as
         * `--rationale`, which is the decision's own argument for itself. */
        const char *why;
        /* Migration 10: where a recorded reason came from — OPERATOR (the
         * default), or the manifest or repair pass an imported one came from.
         * Checked against the closed vocabulary at the write point. */
        const char *provenance;
        /* `decision link note`: which kind of event is being recorded about an
         * edge. Only a `note` may name one; add and remove name their own. */
        const char *edge_event;
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
    /* A9.2.1. The verification-intake fields, grouped for the reason the A4
     * ones are: there are a dozen and a half of them, six subcommands use
     * them, and mixing them into the flat set would make every other command's
     * option list unreadable.
     *
     * There is deliberately **no field here for an actor class, an actor
     * identity, a channel or a verifier's verdict.** The first three are
     * derived from the transport and the fourth from having run the verifier,
     * so an option that set any of them would be the forgery this season
     * exists to prevent, offered as a flag. */
    struct {
        const char *claim;     /* the claim uid an operation is about */
        const char *text;      /* the proposition */
        const char *domain;
        const char *scope;
        const char *semantics; /* DESCRIPTIVE | NORMATIVE */
        const char *decision;  /* the knowledge record it bears on */
        const char *verifier;
        const char *verifier_input;
        const char *commit;
        const char *environment;
        const char *cls; /* evidence class */
        const char *path;
        const char *symbol;
        const char *target;
        const char *probe;
        const char *observed;
        const char *observed_at;
        const char *verdict;
        const char *method;
        const char *evidence;
        const char *supersedes;
        const char *derives_from;
        /* §11's asserted metadata. Stored as asserted on every channel — the
         * operator channel authenticates a *uid*, never a name somebody typed. */
        const char *actor;
        const char *provider;
        const char *role;
        long line_start;
        long line_end;
        /* The actor's own number, 0..100, or -1 for "not given". Never Atlas'
         * confidence, and named so that it cannot be mistaken for it. */
        long self_confidence;
    } verify;
} atlas_cli_opts;

/* Runs one command line. `argv[0]` is the program name. Returns the process
 * exit code. Diagnostics go to `errout`; results to `out`. */
int atlas_cli_main(int argc, char **argv, FILE *out, FILE *errout);

void atlas_cli_print_help(FILE *out);
void atlas_cli_print_version(FILE *out, bool json);

#endif /* ATLAS_CLI_H */

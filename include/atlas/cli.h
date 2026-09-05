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
    /* A15 T5. `atlas review apply FILE --check`: a dry run that mints and
     * spends nothing. `--json` is refused without it -- see run_review. */
    bool check;
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
    /* A14. `job list --remote`: list only jobs submitted through the gateway. */
    bool remote;
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
    /* A8-CI: `code index --compdb PATH`, repeatable. Repository-relative.
     *
     * A9.2.4: optional, and no longer the whole story. With none given, the
     * databases are whatever build-input discovery accepted; with some, exactly
     * those, because naming one is a deliberate act about a particular build and
     * discovery is not entitled to overrule it. */
    const char *compdbs[32];
    size_t compdb_count;
    /* A9.2.3: `code sem-config NAME --test-root PATH`, repeatable. Declared
     * prefixes, never guessed: a directory called `tests` is a directory
     * somebody named, and Atlas classifying it on that basis would be inventing
     * the scope information a production-only absence rests on. */
    const char *test_roots[32];
    size_t test_root_count;
    /* Whether any `--test-root` was given at all, which is a different fact
     * from how many: it separates "leave the stored roots alone" from "clear
     * them", and `--no-test-roots` is how the second is spelled. */
    bool test_roots_given;
    /* A9.2.3: `--auto` / `--no-auto`. Negative means neither was given, so the
     * stored value is left alone — an operator adjusting a path list must not
     * turn automatic rebuilding on or off as a side effect. */
    int auto_rebuild;
    /* A9.2.4: `code sem-config NAME --exclude PATH`, repeatable. Prefixes the
     * build-input walk does not enter. Declared rather than guessed, for the
     * reason test roots are, and *shown* on every status surface: an exclusion
     * nobody can see is a hole in the search universe nobody can see. */
    const char *excludes[32];
    size_t exclude_count;
    bool excludes_given;
    /* A9.2.4: `--vendor-root PATH`, repeatable. Prefixes an operator declares to
     * be somebody else's code; candidates under one are reported as excluded
     * rather than as uncovered. */
    const char *vendor_roots[32];
    size_t vendor_root_count;
    bool vendor_roots_given;
    /* A9.2.4: `--discover` / `--no-discover`. Negative means neither was given.
     * Zero is AUTOMATIC (walk the repository), positive is MANUAL (use only the
     * pinned list, and report discovery as UNKNOWN — because a pinned list is a
     * list somebody wrote, and this season exists because one was incomplete). */
    int discovery_mode;
    /* A9: `api-key create --label L --scope S [--scope S...]`.
     *
     * `--scope` is repeatable and every value must be in the closed vocabulary;
     * an unrecognised one is a refusal rather than something dropped, because a
     * credential granted fewer scopes than were written down is a credential
     * whose behaviour nobody can predict from the command that made it. */
    const char *label;
    const char *scopes[16];
    size_t scope_count;
    /* A16: `api-key create --label L --no-scopes` and the same on `rotate`.
     * The deliberate form for a credential the operator wants to authorise
     * nothing on its own — a remote-disposal credential, Decision 2 — never a
     * silent relaxation of the "at least one --scope" rule below. Refused
     * together with `--scope` in the same invocation. */
    bool no_scopes;
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
        /* A11.1. `atlas job run`. The gates a run is fixed with at its root
         * task, and the run to resume instead of starting a new one. A gate is
         * split on spaces here and never by a shell; `argv[0]` is checked
         * against the binary's own allowlist by the layer that runs it. */
        const char *gates[8];
        size_t gate_count;
        const char *resume;
        /* A10.1. `--memory off|bounded`. Absent means off; the value is checked
         * by the daemon, which refuses a spelling it does not know rather than
         * skipping it. */
        const char *memory;
        /* A11.6. `--parent JOB` on `job submit`: the task this one follows, and
         * therefore the run it joins. `--parallel N` on `job submit` and
         * `job run`: how many tasks the run being created may hold active at
         * once. Zero is "not stated" and means one; the daemon refuses anything
         * outside its bound rather than reducing it, and refuses the two
         * together — a run's bound is fixed at its root. */
        const char *parent;
        long parallel;
    } job;
    /* A12.0. `atlas plan run|status|show|list`. Only the two fields the plan
     * commands do not already share with `job`: everything else a plan is given
     * — `--repo`, `--gate`, `--parallel`, `--resume` — is the same flag with the
     * same meaning, and giving it a second field would be a second place for
     * the two to drift.
     *
     * There is deliberately no field here for a driver, a model, a stage or a
     * task: what runs a plan's jobs is the root-owned policy's business and the
     * plan driver's defaults, and a flag that chose one would be a way to ask
     * for a worker the policy did not authorise. */
    struct {
        /* The operator's own words, ≤ ATLAS_PLAN_GOAL_MAX bytes, refused rather
         * than truncated: a goal Atlas shortened is a goal nobody wrote. */
        const char *goal;
        /* `plan show P --rev N`: which revision's document to print. */
        long rev;
    } plan;
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
    /* A12.1 T16. `atlas memory pack|diff|patch|trailer`. `--repo` and `--task`
     * are the flat top-level fields above (`context build`'s own precedent —
     * one flag fills every field that needs it, `--repo`'s own parse branch
     * already does this for `job.repo`). These five are new: `--run` names a
     * frozen pack or a trailer's source run; `--generation` selects one of
     * `memory diff`'s generations; `--source` names a registered source for
     * `memory patch`; `--commit` is filled by the *same* branch that appends
     * to `decision.commits[]` for `decision link add`, one flag serving two
     * commands exactly as `--repo` already does; `--reason` names the change
     * reason a composed trailer records. */
    struct {
        const char *run;
        long generation;
        const char *source;
        const char *commit;
        const char *reason;
    } memory;
} atlas_cli_opts;

/* Runs one command line. `argv[0]` is the program name. Returns the process
 * exit code. Diagnostics go to `errout`; results to `out`. */
int atlas_cli_main(int argc, char **argv, FILE *out, FILE *errout);

void atlas_cli_print_help(FILE *out);
void atlas_cli_print_version(FILE *out, bool json);

#endif /* ATLAS_CLI_H */

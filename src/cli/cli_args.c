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

void atlas_cli_print_help(FILE *out) {
    (void)fprintf(
        out,
        "atlas %s - engineering memory and repository intelligence (phase %s)\n"
        "\n"
        "usage: atlas [OPTIONS] COMMAND [OPTIONS] [ARGS]\n"
        "\n"
        "commands:\n"
        "  job submit --repo NAME --task TEXT [--driver D] [--mode M]\n"
        "                            [--gate CMD]... [--memory off|bounded]\n"
        "                            [--idempotency-key K] [--attempts N]\n"
        "                            [--parent JOB] join that task's run\n"
        "                            [--parallel N] tasks the new run may hold\n"
        "                                           active at once (default 1)\n"
        "  job run --repo NAME --task TEXT --gate CMD [--gate CMD]...\n"
        "                            [--memory off|bounded] [--parallel N]\n"
        "                            start one run and drive it in the foreground\n"
        "  job run --resume RUN      continue a run that already exists\n"
        "  job run-status RUN        what a run is waiting on, and how it ended\n"
        "  job get|cancel JOB        read or cancel one job\n"
        "  job list [--remote]       jobs this principal submitted; --remote lists\n"
        "                            only jobs submitted through the gateway\n"
        "  scanner run --once         ask the daemon which repositories this uid may scan\n"
        "  dispatcher run [--once]   run the job dispatcher (as atlas-worker)\n"
        ,
        ATLAS_VERSION_STRING, ATLAS_PHASE);
    /* A12.0. A fifth split, for the reason the four below exist and by the same
     * mechanism: the seven `plan` lines pushed the first literal past the length
     * ISO C99 guarantees. Everything here prints exactly where it always did —
     * this is where the first literal ends, not a reordering. */
    (void)fprintf(
        out,
        "  plan run --repo NAME --goal TEXT --gate CMD [--gate CMD]...\n"
        "                            [--parallel N] plan the goal, then drive every\n"
        "                            stage as an ordinary run, in the foreground\n"
        "  plan run --resume PLAN    carry on a plan that already exists\n"
        "  plan status PLAN          what a plan is doing, derived on this read\n"
        "  plan show PLAN --rev N    one revision's plan document (untrusted)\n"
        "  plan list                 plans this principal created\n"
        "  doctor                     check the environment, database and search backend\n"
        "  repo add PATH [--name N] [--scanner-uid UID]\n"
        "                             register a git repository (read-only)\n"
        "  repo list                  list registered repositories\n"
        "  repo scanner NAME [--scanner-uid UID]\n"
        "                             which uid's scanner may read this repository\n"
        "  repo remove NAME --yes     forget a repository; never touches the repository\n"
        "  scan NAME                  index tracked files and git history\n"
        "  status NAME                show indexed state next to live git state\n"
        "  search NAME QUERY          search indexed paths and commit messages\n"
        "  file NAME PATH             show what Atlas knows about one path\n"
        "  history NAME PATH          show recorded changes to one path\n"
        "  diff NAME                  show staged, unstaged and untracked changes\n"
        "  daemon run                 run the indexing daemon in the foreground\n"
        "  daemon status              report the daemon and every repository's index state\n"
        "  daemon ping                check whether the daemon is answering\n"
        "  sync NAME                  reconcile a repository now\n"
        "  events NAME                read the durable event journal\n"
        "  code status NAME           report the structural index and how current it is\n"
        "  code sync NAME [--rebuild] reindex structure now; --rebuild discards and redoes it\n"
        "  code file NAME PATH        structural facts about one file\n"
        "  code symbol NAME SYMBOL    every recorded site of a symbol, with callers and calls\n"
        "  code search NAME QUERY     search indexed symbol names\n"
        "  code deps NAME PATH        what a file depends on\n"
        "  code impact NAME PATH      what may be affected if it changes (candidates, not proof)\n"
        "  code sem-status NAME       the semantic index: freshness, coverage, build-input\n"
        "                             discovery and what is due\n"
        "  code sem-config NAME       read or write the semantic build description\n"
        "                             [--compdb P]... [--test-root P]... [--vendor-root P]...\n"
        "                             [--exclude P]... [--discover|--no-discover]\n"
        "                             [--auto|--no-auto]\n"
        "  decision list NAME         recorded knowledge, its kind and its lifecycle status\n"
        "  decision show NAME ID      one decision in full, with its links' currency\n"
        "  decision search NAME QUERY search recorded decisions\n"
        "  decision history NAME ID   every revision and every lifecycle event\n"
        "  decision for-file NAME PATH  decisions concerning one file\n"
        "  decision propose NAME      record a decision as a proposal\n"
        "  decision revise NAME ID    propose a new revision; never edits an approved one\n"
        "  decision approve NAME ID   accept a revision. Needs an interactive terminal\n"
        "  decision reject NAME ID    refuse a revision. Needs an interactive terminal\n"
        "  decision supersede NAME ID --by ID2   replace one decision with another\n"
        "  decision export NAME ID    write the decision to stdout as Markdown or JSON\n"
        "  decision link add REPO SOURCE TARGET   relate one decision to another\n"
        "  decision link remove REPO SOURCE TARGET  withdraw a relation (--why required)\n"
        "  decision link note REPO SOURCE TARGET    record why, without changing links\n"
        "  decision links REPO ID     one decision's relations, with why each exists\n");
    /* A11.6. A fourth split, for the reason the three below exist and by the
     * same mechanism: `job submit --parent/--parallel` pushed the first literal
     * past the length ISO C99 guarantees. The three lines moved here print in
     * the same place they always did — this is where the first block ends, not a
     * reordering. */
    (void)fprintf(
        out,
        "  decision orphaned          decisions attached to no registered repository\n"
        "  decision legacy NAME       A2 decision proposals, and which were promoted\n"
        "  decision promote NAME ID   make an A4 document from an A2 proposal\n");
    /* A9.2.5. A third split, for the reason the second exists: the eight
     * semantic commands below pushed the first literal past the length ISO C99
     * guarantees.
     *
     * All eight existed, were dispatched, were served over the socket, and
     * appeared in no help text — including `code index`, which the "no semantic
     * index exists" error tells an operator to run. CLAUDE.md warns that the
     * COMMANDS[] table is the wiring place that gets forgotten; this is a sixth,
     * and its failure mode is quieter still: the command works, every test
     * passes, and nobody can find it. */
    (void)fprintf(
        out,
        "  code index NAME            build the semantic index (operator; runs a compiler)\n"
        "                             [--compdb PATH]... [--rebuild]\n"
        "  code semantic NAME SYMBOL  compiler-derived facts about a symbol\n"
        "  code callers NAME SYMBOL   who calls it, with the evidence for each edge\n"
        "  code callees NAME SYMBOL   what it calls\n"
        "  code trace NAME FROM TO    a bounded call path between two symbols\n"
        "  code sem-impact NAME SUBJ  compiler-derived change impact for a symbol or file\n"
        "  code tests NAME SUBJ       the same report, filtered to test files\n"
        "  code explain NAME SUBJ     the same report, subject definition first\n"
        "  context build --repo NAME --task TEXT   a deterministic task context package\n");

    /* A second split, for the reason the others exist: the backup note pushed
     * the first literal past the length ISO C99 guarantees. */
    (void)fprintf(
        out,
        "  operation status ID        state of a long operation (backup, code index)\n"
        "  backup create OUTPUT       online snapshot of the index; refuses to overwrite\n"
        "                             system deployment: OUTPUT is a NAME in the daemon's\n"
        "                             backup directory, not a path\n"
        "  backup verify BACKUP       check one; creates nothing and repairs nothing\n"
        "  backup restore BACKUP --yes  replace the index; keeps what it displaced\n"
        "  maintenance plan           what a prune would remove, and why each table is kept\n"
        "  maintenance prune --apply  remove only the rows the plan called eligible\n"
        "  api-key create --label L --scope S   mint a remote credential; prints the secret once\n"
        "  api-key create --label L --no-scopes   mint a credential that authorises nothing\n"
        "                             on its own; only a remote_dispose_key policy line can\n"
        "  api-key list               credential metadata; never a secret\n"
        "  api-key revoke KEY-ID      stops working immediately; the record stays\n"
        "  api-key rotate KEY-ID --label L --scope S   mint a replacement, revoke the old\n"
        "  api-key rotate KEY-ID --label L --no-scopes   same, for a scopeless credential\n"
        "  gateway status             what the root-owned gateway policy says; binds nothing\n"
        "  gateway run                serve remote MCP; Atlas terminates no TLS\n"
        "  service print              print the systemd user unit; changes nothing\n"
        "  service install --user     write the unit; never enables or starts it\n"
        "  service uninstall --user   remove the unit Atlas wrote\n"
        "  mcp                        serve the Model Context Protocol on stdio\n"
        "  hook EVENT                 handle one Claude Code hook event on stdin\n"
        "  integrate claude print     print the one-time setup commands; runs none of them\n"
        "  integrate claude doctor    check the AI integration end to end\n"
        "  integrate claude install --user    record where this Atlas is, for the plugin\n"
        "  integrate claude uninstall --user  remove that record; never the index\n");
    /* A third fprintf for the same reason as the second: A6 pushed the command
     * list past the guaranteed literal length again. */
    (void)fprintf(
        out,
        "  decision revalidate NAME ID  record that an approved decision was checked\n"
        "                             against the current indexed state; needs a terminal\n"
        "  decision resolve NAME ID   record that the demand an approved OBLIGATION or\n"
        "                             ACCEPTED_RISK made has been met. Needs a terminal\n"
        "  gate check NAME            assess every approved decision against the indexed\n"
        "                             state; exits 8 on review required, 9 on blocked\n"
        "  gate show NAME ID          the same assessment, for one decision\n"
        "  review apply FILE [--check]  walk a review sheet through the operator channel\n"
        "                             one entry at a time; --check is a dry run that\n"
        "                             mints and spends nothing\n"
        "  verify claim --repo R --text \"...\"   state a checkable proposition\n"
        "  verify evidence --claim UID --class C  reference what you looked at\n"
        "  verify produce --claim UID            have Atlas run a bounded verifier itself\n"
        "  verify attest --claim UID --verdict V  record a conclusion and what it rests on\n"
        "  verify depend --evidence UID --derives-from UID   declare a shared source, so\n"
        "                             one document read by several agents counts once\n"
        "  verify evaluate --claim UID  weigh everything recorded and store the result\n"
        "  verify show CLAIM-ID       what evidence bears on one claim, and what a policy\n"
        "                             would do about it; writes nothing\n"
        "  verify run NAME CLAIM-ID   the same, recording the result and performing the\n"
        "                             transition when a root-owned policy allows it\n"
        "  verify policy              the root-owned verification policy; opens no index\n"
        "  version                    print the version\n"
        "  help                       print this help\n");
    /* A12.1 T16. A sixth split, for the reason the fifth exists: the seven
     * `memory` lines pushed the string past the guaranteed literal length
     * again. */
    (void)fprintf(
        out,
        "  memory status --repo NAME  registered sources, versions, generation, and\n"
        "                             whether a pass is owed; states the root-owned\n"
        "                             memory-source policy even when none is installed\n"
        "  memory scan --repo NAME    read every registered EXTERNAL_* source as this\n"
        "                             account and submit it (memory.put)\n"
        "  memory reconcile --repo NAME  ask the daemon to reconcile now; accepted, not\n"
        "                             completed -- poll `memory status` afterward\n"
        "  memory pack --repo NAME --task TEXT   preview a Canonical Context Pack\n"
        "  memory pack --repo NAME --run RUN     show a run's frozen pack\n"
        "  memory diff --repo NAME --generation N   one reconciliation's per-claim diff\n"
        "  memory patch --repo NAME --source UID    a proposed deletion-only patch\n"
        "  memory trailer --run RUN --reason UID    compose a commit trailer block\n"
        "  memory trailer --commit OID --repo NAME  show a commit's recorded binding\n");
    /* Split because ISO C only guarantees 4095-byte string literals, and
     * A3's commands pushed the single literal past it. Same reason a migration
     * is a list of statement groups rather than one string. */
    (void)fprintf(
        out,
        "\n"
        "options (accepted before or after the command; '--' ends option parsing):\n"
        "  --json                     emit stable JSON on stdout\n"
        "  --check                    review apply: a dry run; mints and spends nothing.\n"
        "                             --json needs this -- review apply is otherwise an\n"
        "                             interactive command\n"
        "  --wait                     sync: wait for the reconciliation to complete\n"
        "  --full                     sync: re-read every file rather than only changes\n"
        "  --since CURSOR             events: start after this cursor\n"
        "  --rebuild                  code sync: discard the structural index and rebuild it\n"
        "  --depth N                  code deps/impact: traversal depth (max %d)\n"
        "  --reverse                  code deps: report what depends on this instead\n"
        "  --symbol                   code deps/impact: treat the operand as a symbol name\n"
        "  --title T --decision D     decision propose/revise: the required content\n"
        "  --context C --rationale R --consequences Q --scope S\n"
        "                             decision propose/revise: the rest of the document\n"
        "  --alternative A            decision propose/revise: repeatable, up to %d\n"
        "  --why TEXT                 decision link add/remove/note: why the relation\n"
        "  --provenance P --event E   decision link: where a reason came from, and\n"
        "                             which event it records\n"
        "  --path P --commit OID --symbol-link S --decision-link UID\n"
        "                             decision propose/revise: repeatable links\n"
        "  --status S                 decision list: PROPOSED|APPROVED|REJECTED|SUPERSEDED|\n"
        "                             RESOLVED. What stage of the approval workflow a record\n"
        "                             reached; a separate dimension from --kind\n"
        "  --kind K                   which sort of knowledge record: DECISION (the default)|\n"
        "                             POLICY|INVARIANT|OPERATIONAL_FACT|ACCEPTED_RISK|\n"
        "                             OBLIGATION|PARKED|REJECTED_ALTERNATIVE. On propose it\n"
        "                             says what to create, on list/search/for-file it filters,\n"
        "                             and a revision can never change it\n"
        "  --revision N               decision show/approve: a specific revision\n"
        "  --run RUN                  memory pack/trailer: a run uid\n"
        "  --generation N             memory diff: which reconciliation to show\n"
        "  --source UID               memory patch: a registered source uid\n"
        "  --reason UID               memory trailer: a change-reason uid\n"
        "  --at OID                   gate check/show: the exact state to assess\n"
        "  --by ID                    decision supersede: the replacement decision\n"
        "  --format markdown|json     decision export: the output form\n"
        "  --user                     service: operate on the systemd *user* unit\n"
        "  --force                    service: replace or remove a unit Atlas did not write\n"
        "                             backup create: replace an existing destination\n"
        "  --apply                    maintenance: actually delete; without it nothing is written\n"
        "  --older-than DAYS          maintenance: only rows created before this (1-36500)\n"
        "  --retain N                 maintenance: newest events per repository always kept\n"
        "  --data-dir DIR             use DIR instead of the resolved data directory\n"
        "  --limit N                  cap results per kind (default %d)\n"
        "  --max-commits N            stop ingesting history after N commits\n"
        "  --no-history               scan tracked files only\n"
        "  --no-untracked             omit untracked paths from diff\n"
        "  --timeout-ms N             per-git-invocation timeout\n"
        "  --yes                      confirm a destructive metadata operation\n"
        "  -q, --quiet                suppress non-essential output\n"
        "  -h, --help                 print this help\n"
        "  -V, --version              print the version\n"
        "\n"
        "data directory resolution: --data-dir, then ATLAS_DATA_DIR, then\n"
        "XDG_DATA_HOME/atlas, then $HOME/.local/share/atlas\n"
        "\n"
        "exit codes: 0 ok, 1 internal, 2 usage, 3 config, 4 repository, 5 database,\n"
        "6 git, 7 integrity\n"
        "\n"
        "Atlas records facts only. It never infers why something changed: when a reason\n"
        "is requested it answers UNKNOWN.\n",
        ATLAS_CODE_MAX_TRAVERSAL_DEPTH, ATLAS_DECISION_MAX_ALTERNATIVES, ATLAS_DEFAULT_LIMIT);
}

void atlas_cli_print_version(FILE *out, bool json) {
    if (!json) {
        (void)fprintf(out, "atlas %s (phase %s)\n", ATLAS_VERSION_STRING, ATLAS_PHASE);
        return;
    }
    (void)fprintf(out,
                  "{\"atlas\":\"%s\",\"phase\":\"%s\",\"command\":\"version\",\"ok\":true,"
                  "\"version\":\"%s\",\"version_major\":%d,\"version_minor\":%d,"
                  "\"version_patch\":%d,\"schema_version\":%d}\n",
                  ATLAS_VERSION_STRING, ATLAS_PHASE, ATLAS_VERSION_STRING, ATLAS_VERSION_MAJOR,
                  ATLAS_VERSION_MINOR, ATLAS_VERSION_PATCH, ATLAS_SCHEMA_VERSION);
}

/* --- argument parsing ---------------------------------------------------- */

static atlas_status parse_long(const char *text, const char *what, long *out, atlas_err *err) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (end == text || (end != NULL && *end != '\0')) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "%s expects a number, got \"%s\"", what, text);
    }
    if (v < 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "%s must not be negative", what);
    }
    *out = v;
    return ATLAS_OK;
}

/* Global options are accepted anywhere; the first non-option word is the
 * command and the rest are its operands. */
atlas_status atlas_cli_parse_args(cli_state *st, int argc, char **argv, bool *want_help,
                               bool *want_version, atlas_err *err) {
    /* A9.2.3. The one option in Atlas whose "not given" is not the zero value:
     * zero means `--no-auto` and would otherwise disable automatic rebuild on
     * every command that never mentioned it. Set before the loop rather than in
     * the caller's `memset`, so the distinction lives beside the flags it is
     * about. */
    st->opts.auto_rebuild = -1;
    /* A9.2.4. Negative means "neither flag was given", so the stored value is
     * left alone. Zero would mean AUTOMATIC, which is a *statement*, and an
     * operator adjusting a path list must not make one as a side effect. */
    st->opts.discovery_mode = -1;
    bool no_more_options = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!no_more_options && strcmp(a, "--") == 0) {
            no_more_options = true;
            continue;
        }
        if (!no_more_options && a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "--json") == 0) {
                st->opts.json = true;
            } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
                st->opts.quiet = true;
            } else if (strcmp(a, "--yes") == 0) {
                st->opts.yes = true;
            } else if (strcmp(a, "--check") == 0) {
                /* A15 T5. `review apply --check`: a dry run. */
                st->opts.check = true;
            } else if (strcmp(a, "--no-history") == 0) {
                st->opts.no_history = true;
            } else if (strcmp(a, "--no-untracked") == 0) {
                st->opts.no_untracked = true;
            } else if (strcmp(a, "--wait") == 0) {
                st->opts.wait = true;
            } else if (strcmp(a, "--full") == 0) {
                st->opts.full = true;
            } else if (strcmp(a, "--rebuild") == 0) {
                st->opts.rebuild = true;
            } else if (strcmp(a, "--remote") == 0) {
                st->opts.remote = true;
            } else if (strcmp(a, "--reverse") == 0) {
                st->opts.reverse = true;
            } else if (strcmp(a, "--symbol") == 0) {
                st->opts.symbol = true;
            } else if (strcmp(a, "--proven") == 0) {
                st->opts.proven_only = true;
            } else if (strcmp(a, "--history") == 0) {
                st->opts.history = true;
            } else if (strcmp(a, "--repo") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--repo needs a name");
                }
                st->opts.repo = argv[++i];
                /* A8's `job submit` reads its own field. One flag fills both,
                 * because this branch is reached first and the grouped A8 branch
                 * below never sees `--repo` at all. */
                st->opts.job.repo = st->opts.repo;
            } else if (strcmp(a, "--task") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--task needs a description");
                }
                st->opts.task = argv[++i];
                st->opts.job.task = st->opts.task;
            } else if (strcmp(a, "--run") == 0) {
                /* A12.1 T16. `memory pack --run UID` / `memory trailer --run
                 * UID`. New: no earlier command took a bare "--run". */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--run needs a run uid");
                }
                st->opts.memory.run = argv[++i];
            } else if (strcmp(a, "--generation") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--generation needs a number");
                }
                atlas_status ts =
                    parse_long(argv[++i], "--generation", &st->opts.memory.generation, err);
                if (ts != ATLAS_OK) {
                    return ts;
                }
            } else if (strcmp(a, "--source") == 0) {
                /* A12.1 T16. `memory patch --source UID`. New. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--source needs a source uid");
                }
                st->opts.memory.source = argv[++i];
            } else if (strcmp(a, "--reason") == 0) {
                /* A12.1 T16. `memory trailer --run UID --reason UID`. New. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--reason needs a change reason uid");
                }
                st->opts.memory.reason = argv[++i];
            } else if (strcmp(a, "--max-tokens") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--max-tokens needs a number");
                }
                atlas_status ts = parse_long(argv[++i], "--max-tokens", &st->opts.max_tokens, err);
                if (ts != ATLAS_OK) {
                    return ts;
                }
            } else if (strcmp(a, "--rebuild") == 0) {
                st->opts.rebuild = true;
            } else if (strcmp(a, "--compdb") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--compdb needs a path");
                }
                if (st->opts.compdb_count >= sizeof(st->opts.compdbs) / sizeof(st->opts.compdbs[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "at most %zu compilation databases may be named",
                                         sizeof(st->opts.compdbs) / sizeof(st->opts.compdbs[0]));
                }
                st->opts.compdbs[st->opts.compdb_count++] = argv[++i];
            } else if (strcmp(a, "--test-root") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--test-root needs a path");
                }
                if (st->opts.test_root_count >=
                    sizeof(st->opts.test_roots) / sizeof(st->opts.test_roots[0])) {
                    /* Refused, never clamped — A5's rule. A silently dropped
                     * root would classify a test source as production, which is
                     * wrong in the direction that lets a production-scope
                     * absence be answered when it should not be. */
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "at most %zu test roots may be named",
                                         sizeof(st->opts.test_roots) /
                                             sizeof(st->opts.test_roots[0]));
                }
                st->opts.test_roots_given = true;
                st->opts.test_roots[st->opts.test_root_count++] = argv[++i];
            } else if (strcmp(a, "--no-test-roots") == 0) {
                /* Clearing is spelled explicitly, because an empty repetition
                 * of `--test-root` is indistinguishable from not passing it. */
                st->opts.test_roots_given = true;
                st->opts.test_root_count = 0;
            } else if (strcmp(a, "--exclude") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--exclude needs a path");
                }
                if (st->opts.exclude_count >=
                    sizeof(st->opts.excludes) / sizeof(st->opts.excludes[0])) {
                    /* Refused, never clamped. A silently dropped exclusion would
                     * make Atlas walk a subtree an operator asked it not to. */
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "at most %zu discovery exclusions may be named",
                                         sizeof(st->opts.excludes) /
                                             sizeof(st->opts.excludes[0]));
                }
                st->opts.excludes_given = true;
                st->opts.excludes[st->opts.exclude_count++] = argv[++i];
            } else if (strcmp(a, "--no-excludes") == 0) {
                st->opts.excludes_given = true;
                st->opts.exclude_count = 0;
            } else if (strcmp(a, "--vendor-root") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--vendor-root needs a path");
                }
                if (st->opts.vendor_root_count >=
                    sizeof(st->opts.vendor_roots) / sizeof(st->opts.vendor_roots[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "at most %zu vendor roots may be named",
                                         sizeof(st->opts.vendor_roots) /
                                             sizeof(st->opts.vendor_roots[0]));
                }
                st->opts.vendor_roots_given = true;
                st->opts.vendor_roots[st->opts.vendor_root_count++] = argv[++i];
            } else if (strcmp(a, "--no-vendor-roots") == 0) {
                st->opts.vendor_roots_given = true;
                st->opts.vendor_root_count = 0;
            } else if (strcmp(a, "--discover") == 0) {
                st->opts.discovery_mode = 0;
            } else if (strcmp(a, "--no-discover") == 0) {
                st->opts.discovery_mode = 1;
            } else if (strcmp(a, "--auto") == 0) {
                st->opts.auto_rebuild = 1;
            } else if (strcmp(a, "--no-auto") == 0) {
                st->opts.auto_rebuild = 0;
            } else if (strcmp(a, "--label") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--label needs a value");
                }
                st->opts.label = argv[++i];
            } else if (strcmp(a, "--scope") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--scope needs a value");
                }
                if (st->opts.scope_count >= sizeof(st->opts.scopes) / sizeof(st->opts.scopes[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "at most %zu scopes may be named",
                                         sizeof(st->opts.scopes) / sizeof(st->opts.scopes[0]));
                }
                st->opts.scopes[st->opts.scope_count++] = argv[++i];
            } else if (strcmp(a, "--no-scopes") == 0) {
                st->opts.no_scopes = true;
            } else if (strcmp(a, "--depth") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--depth needs a value");
                }
                atlas_status ds = parse_long(argv[++i], "--depth", &st->opts.depth, err);
                if (ds != ATLAS_OK) {
                    return ds;
                }
            } else if (strcmp(a, "--user") == 0) {
                st->opts.user = true;
            } else if (strcmp(a, "--force") == 0) {
                st->opts.force = true;
            } else if (strcmp(a, "--apply") == 0) {
                st->opts.apply = true;
            } else if (strcmp(a, "--older-than") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--older-than needs a number of days");
                }
                atlas_status os = parse_long(argv[++i], "--older-than", &st->opts.older_than_days,
                                             err);
                if (os != ATLAS_OK) {
                    return os;
                }
            } else if (strcmp(a, "--retain") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--retain needs a value");
                }
                atlas_status rs = parse_long(argv[++i], "--retain", &st->opts.retain, err);
                if (rs != ATLAS_OK) {
                    return rs;
                }
            } else if (strcmp(a, "--once") == 0) {
                /* A test hook for `daemon run`: reconcile everything once, then
                 * exit. Deliberately absent from the help text; it exists so the
                 * suite can exercise the daemon without a supervisor. */
                st->opts.run_once = true;
            } else if (strcmp(a, "--since") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--since needs a cursor");
                }
                atlas_status s = parse_long(argv[++i], "--since", &st->opts.since, err);
                if (s != ATLAS_OK) {
                    return s;
                }
            } else if (strcmp(a, "--mode") == 0 || strcmp(a, "--driver") == 0 ||
                       strcmp(a, "--idempotency-key") == 0 || strcmp(a, "--wall-timeout-ms") == 0 ||
                       strcmp(a, "--idle-timeout-ms") == 0 || strcmp(a, "--attempts") == 0) {
                /* A8 job options, grouped for the same reason the A4 ones are:
                 * the "a flag at the end of the line has no value" check lives
                 * once rather than six times. `--repo` and `--task` are handled
                 * above, where the general options are: this branch never saw
                 * them, which is why `job submit` could not read either. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s needs a value", a);
                }
                const char *v = argv[++i];
                if (strcmp(a, "--mode") == 0) {
                    st->opts.job.mode = v;
                } else if (strcmp(a, "--driver") == 0) {
                    st->opts.job.driver = v;
                } else if (strcmp(a, "--idempotency-key") == 0) {
                    st->opts.job.key = v;
                } else if (strcmp(a, "--wall-timeout-ms") == 0) {
                    st->opts.job.wall_ms = strtol(v, NULL, 10);
                } else if (strcmp(a, "--idle-timeout-ms") == 0) {
                    st->opts.job.idle_ms = strtol(v, NULL, 10);
                } else {
                    st->opts.job.attempts = strtol(v, NULL, 10);
                }
            } else if (strcmp(a, "--gate") == 0) {
                /* Repeatable, and bounded at the point of parsing rather than
                 * later: a ninth gate is refused with the bound named, never
                 * dropped. A11.1 fixes this list on the root task and every
                 * follow-up inherits it unchanged. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--gate needs a command");
                }
                if (st->opts.job.gate_count >= 8u) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "a run may declare at most 8 gates");
                }
                st->opts.job.gates[st->opts.job.gate_count++] = argv[++i];
            } else if (strcmp(a, "--memory") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "--memory needs off or bounded");
                }
                st->opts.job.memory = argv[++i];
            } else if (strcmp(a, "--parent") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--parent needs a job identifier");
                }
                st->opts.job.parent = argv[++i];
            } else if (strcmp(a, "--parallel") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--parallel needs a number");
                }
                /* Parsed rather than `strtol`ed, unlike the A8 group above:
                 * `--parallel nonsense` becoming zero would resolve to one at
                 * the daemon, which is a clamp wearing a default's clothes and
                 * the one thing this bound must not do. */
                atlas_status ps = parse_long(argv[++i], "--parallel", &st->opts.job.parallel, err);
                if (ps != ATLAS_OK) {
                    return ps;
                }
            } else if (strcmp(a, "--goal") == 0) {
                /* A12.0. `atlas plan run --goal TEXT`. The operator's own words,
                 * sent verbatim; the service layer refuses one past the bound
                 * rather than shortening it. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--goal needs a description");
                }
                st->opts.plan.goal = argv[++i];
            } else if (strcmp(a, "--rev") == 0) {
                /* A12.0. `atlas plan show P --rev N`. Parsed rather than
                 * `strtol`ed, for `--parallel`'s reason: `--rev nonsense`
                 * becoming zero would silently ask for a different thing. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--rev needs a revision number");
                }
                atlas_status rs = parse_long(argv[++i], "--rev", &st->opts.plan.rev, err);
                if (rs != ATLAS_OK) {
                    return rs;
                }
            } else if (strcmp(a, "--resume") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "--resume needs a run or plan identifier");
                }
                st->opts.job.resume = argv[++i];
            } else if (strcmp(a, "--once") == 0) {
                st->opts.job.once = true;
            } else if (strcmp(a, "--title") == 0 || strcmp(a, "--context") == 0 ||
                       strcmp(a, "--decision") == 0 || strcmp(a, "--rationale") == 0 ||
                       strcmp(a, "--consequences") == 0 || strcmp(a, "--scope") == 0 ||
                       strcmp(a, "--status") == 0 || strcmp(a, "--by") == 0 ||
                       strcmp(a, "--format") == 0 || strcmp(a, "--dedup-key") == 0 ||
                       strcmp(a, "--at") == 0 || strcmp(a, "--kind") == 0) {
                /* One arm for every A4 option that takes exactly one value, so
                 * the "a flag at the end of the line has no value" check exists
                 * once rather than ten times. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s needs a value", a);
                }
                const char *v = argv[++i];
                if (strcmp(a, "--title") == 0) {
                    st->opts.decision.title = v;
                } else if (strcmp(a, "--context") == 0) {
                    st->opts.decision.context_text = v;
                } else if (strcmp(a, "--decision") == 0) {
                    st->opts.decision.decision_text = v;
                } else if (strcmp(a, "--rationale") == 0) {
                    st->opts.decision.rationale = v;
                } else if (strcmp(a, "--consequences") == 0) {
                    st->opts.decision.consequences = v;
                } else if (strcmp(a, "--scope") == 0) {
                    st->opts.decision.scope = v;
                } else if (strcmp(a, "--status") == 0) {
                    st->opts.decision.status = v;
                } else if (strcmp(a, "--by") == 0) {
                    st->opts.decision.by = v;
                } else if (strcmp(a, "--format") == 0) {
                    st->opts.decision.format = v;
                } else if (strcmp(a, "--at") == 0) {
                    st->opts.decision.at_commit = v;
                } else if (strcmp(a, "--kind") == 0) {
                    /* A9.1. One flag, two jobs, and they cannot be confused:
                     * on `propose` it says what to create, on `list`, `search`
                     * and `for-file` it filters. Both are "which kind of
                     * knowledge are we talking about", which is why one name is
                     * right — and on `revise` it is an assertion that is
                     * checked, never applied. */
                    st->opts.decision.kind = v;
                } else {
                    st->opts.decision.dedup_key = v;
                }
            } else if (strcmp(a, "--alternative") == 0 || strcmp(a, "--path") == 0 ||
                       strcmp(a, "--commit") == 0 || strcmp(a, "--symbol-link") == 0 ||
                       strcmp(a, "--why") == 0 || strcmp(a, "--provenance") == 0 ||
                       strcmp(a, "--event") == 0 || strcmp(a, "--decision-link") == 0) {
                /* The repeatable ones. Refused past the ceiling rather than
                 * truncated: a decision that silently recorded three of five
                 * alternatives would claim the other two were never considered,
                 * and one that dropped a path would claim it is not about that
                 * file. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s needs a value", a);
                }
                const char *v = argv[++i];
                if (strcmp(a, "--alternative") == 0) {
                    if (st->opts.decision.alternative_count >= ATLAS_DECISION_MAX_ALTERNATIVES) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE,
                                             "at most %d --alternative options",
                                             ATLAS_DECISION_MAX_ALTERNATIVES);
                    }
                    st->opts.decision.alternatives[st->opts.decision.alternative_count++] = v;
                } else if (strcmp(a, "--path") == 0) {
                    if (st->opts.decision.path_count >= ATLAS_DECISION_MAX_LINKS) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE, "at most %d --path options",
                                             ATLAS_DECISION_MAX_LINKS);
                    }
                    st->opts.decision.paths[st->opts.decision.path_count++] = v;
                } else if (strcmp(a, "--commit") == 0) {
                    if (st->opts.decision.commit_count >= ATLAS_DECISION_MAX_LINKS) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE, "at most %d --commit options",
                                             ATLAS_DECISION_MAX_LINKS);
                    }
                    st->opts.decision.commits[st->opts.decision.commit_count++] = v;
                    /* A12.1 T16. `memory trailer --commit OID --repo R` reads
                     * its own field. One flag fills both, `--repo`'s own
                     * precedent above: this branch is reached first and
                     * `memory`'s dispatch never sees `--commit` at all. */
                    st->opts.memory.commit = v;
                } else if (strcmp(a, "--why") == 0) {
                    st->opts.decision.why = v;
                } else if (strcmp(a, "--provenance") == 0) {
                    st->opts.decision.provenance = v;
                } else if (strcmp(a, "--event") == 0) {
                    st->opts.decision.edge_event = v;
                } else if (strcmp(a, "--decision-link") == 0) {
                    if (st->opts.decision.decision_link_count >= ATLAS_DECISION_MAX_LINKS) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE,
                                             "at most %d --decision-link options",
                                             ATLAS_DECISION_MAX_LINKS);
                    }
                    st->opts.decision.decision_links[st->opts.decision.decision_link_count++] = v;
                } else {
                    if (st->opts.decision.symbol_count >= ATLAS_DECISION_MAX_LINKS) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE,
                                             "at most %d --symbol-link options",
                                             ATLAS_DECISION_MAX_LINKS);
                    }
                    st->opts.decision.symbols[st->opts.decision.symbol_count++] = v;
                }
            } else if (strcmp(a, "--claim") == 0 || strcmp(a, "--text") == 0 ||
                       strcmp(a, "--domain") == 0 || strcmp(a, "--semantics") == 0 ||
                       strcmp(a, "--verifier") == 0 || strcmp(a, "--verifier-input") == 0 ||
                       strcmp(a, "--environment") == 0 || strcmp(a, "--class") == 0 ||
                       strcmp(a, "--symbol") == 0 || strcmp(a, "--target") == 0 ||
                       strcmp(a, "--probe") == 0 || strcmp(a, "--observed") == 0 ||
                       strcmp(a, "--observed-at") == 0 || strcmp(a, "--verdict") == 0 ||
                       strcmp(a, "--method") == 0 || strcmp(a, "--evidence") == 0 ||
                       strcmp(a, "--supersedes") == 0 || strcmp(a, "--derives-from") == 0 ||
                       strcmp(a, "--actor") == 0 || strcmp(a, "--provider") == 0 ||
                       strcmp(a, "--record") == 0 || strcmp(a, "--role") == 0) {
                /* A9.2.1. One arm for every verification option taking exactly
                 * one value, so "a flag at the end of the line has no value" is
                 * checked once rather than twenty times — the shape the A4 arm
                 * above uses, and for the same reason. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s needs a value", a);
                }
                const char *v = argv[++i];
                static const struct {
                    const char *flag;
                    size_t off;
                } VFIELDS[] = {
                    {"--claim", offsetof(atlas_cli_opts, verify.claim)},
                    {"--text", offsetof(atlas_cli_opts, verify.text)},
                    {"--domain", offsetof(atlas_cli_opts, verify.domain)},
                    {"--semantics", offsetof(atlas_cli_opts, verify.semantics)},
                    {"--verifier", offsetof(atlas_cli_opts, verify.verifier)},
                    {"--verifier-input", offsetof(atlas_cli_opts, verify.verifier_input)},
                    {"--environment", offsetof(atlas_cli_opts, verify.environment)},
                    {"--class", offsetof(atlas_cli_opts, verify.cls)},
                    {"--symbol", offsetof(atlas_cli_opts, verify.symbol)},
                    {"--target", offsetof(atlas_cli_opts, verify.target)},
                    {"--probe", offsetof(atlas_cli_opts, verify.probe)},
                    {"--observed", offsetof(atlas_cli_opts, verify.observed)},
                    {"--observed-at", offsetof(atlas_cli_opts, verify.observed_at)},
                    {"--verdict", offsetof(atlas_cli_opts, verify.verdict)},
                    {"--method", offsetof(atlas_cli_opts, verify.method)},
                    {"--evidence", offsetof(atlas_cli_opts, verify.evidence)},
                    {"--supersedes", offsetof(atlas_cli_opts, verify.supersedes)},
                    {"--derives-from", offsetof(atlas_cli_opts, verify.derives_from)},
                    {"--actor", offsetof(atlas_cli_opts, verify.actor)},
                    {"--provider", offsetof(atlas_cli_opts, verify.provider)},
                    {"--role", offsetof(atlas_cli_opts, verify.role)},
                    /* `--record`, not `--decision`: `--decision` already means
                     * a decision's *body text* on `decision propose`, and one
                     * spelling meaning two things is the A8.2 defect — prose
                     * and a document id sharing a key — which A8's closure made
                     * structural rather than detected. */
                    {"--record", offsetof(atlas_cli_opts, verify.decision)},
                };
                for (size_t k = 0; k < sizeof VFIELDS / sizeof VFIELDS[0]; k++) {
                    if (strcmp(a, VFIELDS[k].flag) == 0) {
                        *(const char **)((char *)&st->opts + VFIELDS[k].off) = v;
                        break;
                    }
                }
            } else if (strcmp(a, "--line-start") == 0 || strcmp(a, "--line-end") == 0 ||
                       strcmp(a, "--self-confidence") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s needs a number", a);
                }
                char *endp = NULL;
                long v = strtol(argv[++i], &endp, 10);
                if (endp == NULL || *endp != '\0' || v < 0) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s takes a number that is not "
                                                               "negative", a);
                }
                if (strcmp(a, "--line-start") == 0) {
                    st->opts.verify.line_start = v;
                } else if (strcmp(a, "--line-end") == 0) {
                    st->opts.verify.line_end = v;
                } else {
                    /* A5's rule that bounds refuse rather than clamp: a
                     * self-reported 300 is a caller who meant something Atlas
                     * cannot represent, and silently storing 100 would record a
                     * number nobody wrote. */
                    if (v > 100) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE,
                                             "--self-confidence is 0..100; it is the actor's own "
                                             "number and is never used as Atlas' confidence");
                    }
                    st->opts.verify.self_confidence = v;
                }
            } else if (strcmp(a, "--revision") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--revision needs a number");
                }
                char *endp = NULL;
                long v = strtol(argv[++i], &endp, 10);
                if (endp == NULL || *endp != '\0' || v < 0) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "--revision must be a non-negative number");
                }
                st->opts.decision.revision = v;
            } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
                *want_help = true;
            } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
                *want_version = true;
            } else if (strcmp(a, "--data-dir") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--data-dir needs a directory");
                }
                st->opts.data_dir = argv[++i];
            } else if (strncmp(a, "--data-dir=", 11u) == 0) {
                st->opts.data_dir = a + 11;
            } else if (strcmp(a, "--name") == 0) {
                /* --name belongs to `repo add`; it is collected as an operand so
                 * the command can validate its placement. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--name needs a value");
                }
                if (st->operand_count + 2u > sizeof(st->operands) / sizeof(st->operands[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "too many arguments");
                }
                st->operands[st->operand_count++] = a;
                st->operands[st->operand_count++] = argv[++i];
            } else if (strncmp(a, "--name=", 7u) == 0) {
                if (st->operand_count + 2u > sizeof(st->operands) / sizeof(st->operands[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "too many arguments");
                }
                st->operands[st->operand_count++] = "--name";
                st->operands[st->operand_count++] = a + 7;
            } else if (strcmp(a, "--scanner-uid") == 0) {
                /* A13. Belongs to `repo add` and `repo scanner`; collected as an
                 * operand so the command validates its placement, exactly as
                 * `--name` is. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--scanner-uid needs a value");
                }
                if (st->operand_count + 2u > sizeof(st->operands) / sizeof(st->operands[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "too many arguments");
                }
                st->operands[st->operand_count++] = a;
                st->operands[st->operand_count++] = argv[++i];
            } else if (strncmp(a, "--scanner-uid=", 14u) == 0) {
                if (st->operand_count + 2u > sizeof(st->operands) / sizeof(st->operands[0])) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "too many arguments");
                }
                st->operands[st->operand_count++] = "--scanner-uid";
                st->operands[st->operand_count++] = a + 14;
            } else if (strcmp(a, "--limit") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--limit needs a number");
                }
                atlas_status s = parse_long(argv[++i], "--limit", &st->opts.limit, err);
                if (s != ATLAS_OK) {
                    return s;
                }
            } else if (strcmp(a, "--max-commits") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--max-commits needs a number");
                }
                atlas_status s = parse_long(argv[++i], "--max-commits", &st->opts.max_commits, err);
                if (s != ATLAS_OK) {
                    return s;
                }
            } else if (strcmp(a, "--timeout-ms") == 0) {
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "--timeout-ms needs a number");
                }
                long v = 0;
                atlas_status s = parse_long(argv[++i], "--timeout-ms", &v, err);
                if (s != ATLAS_OK) {
                    return s;
                }
                st->opts.timeout_ms = (int)v;
            } else {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown option \"%s\"", a);
            }
            continue;
        }
        if (st->command == NULL) {
            st->command = a;
        } else {
            if (st->operand_count >= sizeof(st->operands) / sizeof(st->operands[0])) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "too many arguments");
            }
            st->operands[st->operand_count++] = a;
        }
    }
    return ATLAS_OK;
}

/* Extracts --name from the operand list, leaving positional operands behind. */

/* Atlas - command line front end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The CLI parses arguments, calls the service layer, and hands results to a
 * renderer. It contains no SQL, no git invocation and no output formatting.
 */
#include "atlas/cli.h"

#include <stdio.h>
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

void atlas_cli_print_help(FILE *out) {
    (void)fprintf(
        out,
        "atlas %s - engineering memory and repository intelligence (phase %s)\n"
        "\n"
        "usage: atlas [OPTIONS] COMMAND [OPTIONS] [ARGS]\n"
        "\n"
        "commands:\n"
        "  job submit --repo NAME --task TEXT [--driver D] [--mode M]\n"
        "                            [--idempotency-key K] [--attempts N]\n"
        "  job get|cancel JOB        read or cancel one job\n"
        "  job list                  jobs this principal submitted\n"
        "  dispatcher run [--once]   run the job dispatcher (as atlas-worker)\n"
        "  doctor                     check the environment, database and search backend\n"
        "  repo add PATH [--name N]   register a git repository (read-only)\n"
        "  repo list                  list registered repositories\n"
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
        "  decision links REPO ID     one decision's relations, with why each exists\n"
        "  decision orphaned          decisions attached to no registered repository\n"
        "  decision legacy NAME       A2 decision proposals, and which were promoted\n"
        "  decision promote NAME ID   make an A4 document from an A2 proposal\n"
        ,
        ATLAS_VERSION_STRING, ATLAS_PHASE);
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
        "  api-key list               credential metadata; never a secret\n"
        "  api-key revoke KEY-ID      stops working immediately; the record stays\n"
        "  api-key rotate KEY-ID --label L --scope S   mint a replacement, revoke the old\n"
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
    /* Split because ISO C only guarantees 4095-byte string literals, and
     * A3's commands pushed the single literal past it. Same reason a migration
     * is a list of statement groups rather than one string. */
    (void)fprintf(
        out,
        "\n"
        "options (accepted before or after the command; '--' ends option parsing):\n"
        "  --json                     emit stable JSON on stdout\n"
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
static atlas_status parse_args(cli_state *st, int argc, char **argv, bool *want_help,
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
static atlas_status take_name(cli_state *st, const char **name_out, atlas_err *err) {
    *name_out = NULL;
    size_t w = 0;
    for (size_t i = 0; i < st->operand_count; i++) {
        if (strcmp(st->operands[i], "--name") == 0) {
            if (i + 1u >= st->operand_count) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--name needs a value");
            }
            *name_out = st->operands[i + 1u];
            i++;
            continue;
        }
        st->operands[w++] = st->operands[i];
    }
    st->operand_count = w;
    return ATLAS_OK;
}

/* --- renderer construction ---------------------------------------------- */

static atlas_status renderer_open(atlas_renderer *r, bool json, FILE *out, const char *command,
                                  atlas_err *err) {
    memset(r, 0, sizeof(*r));
    r->out = out;
    r->json = json;
    atlas_safe_pool_init(&r->safe);
    r->open_scope = -1;
    r->v = json ? &ATLAS_RENDERER_JSON : &ATLAS_RENDERER_HUMAN;
    if (json) {
        r->j = atlas_json_new(out, err);
        if (r->j == NULL) {
            return err->status;
        }
    }
    return r->v->begin(r, command, err);
}

static atlas_status renderer_close(atlas_renderer *r, atlas_err *err) {
    atlas_status st = r->v->end(r, err);
    atlas_safe_pool_free(&r->safe);
    return st;
}

static void renderer_abort(atlas_renderer *r) {
    /* On failure the partial document is discarded: atlas_render_error writes a
     * complete error document instead. */
    if (r->j != NULL) {
        atlas_json_free(r->j);
        r->j = NULL;
    }
    atlas_safe_pool_free(&r->safe);
}

/* --- command implementations -------------------------------------------- */

typedef struct list_sink {
    atlas_renderer *r;
} list_sink;

static atlas_status repo_item_sink(const atlas_repo_info *ri, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->repo_item(ls->r, ri, err);
}

static atlas_status search_item_sink(const atlas_search_hit *h, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->search_item(ls->r, h, err);
}

static atlas_status history_item_sink(const atlas_history_row *h, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->history_item(ls->r, h, err);
}

static atlas_status diff_item_sink(const atlas_diff_entry *e, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->diff_item(ls->r, e, err);
}

/* `file` is the one streaming read whose service call can fail having produced
 * no row at all — asking a repository for a path it does not index is an error
 * rather than an empty result. Every other streaming command answers that with
 * zero rows.
 *
 * Opened eagerly like the others, the document header and the repository line
 * were already on stdout when the call failed, and the error document went out
 * after them: two documents, the first one unterminated, from a `--json`
 * invocation that promises exactly one. A streaming writer cannot recall bytes,
 * so the fix is not to write them until there is something to write.
 *
 * So the sink opens the renderer on its first call and the command closes it
 * only if the sink ever fired. A failure before the first row leaves stdout
 * untouched and the error document is the whole output. */
typedef struct file_sink {
    atlas_renderer *r;
    cli_state *st;
    const char *repo;
    bool opened;
} file_sink;

static atlas_status file_report_sink(const atlas_file_report *rep, void *ud, atlas_err *err) {
    file_sink *fs = (file_sink *)ud;
    if (!fs->opened) {
        atlas_status s = renderer_open(fs->r, fs->st->opts.json, fs->st->out, "file", err);
        if (s != ATLAS_OK) {
            return s;
        }
        fs->opened = true;
        s = fs->r->v->note_repo(fs->r, fs->repo, err);
        if (s != ATLAS_OK) {
            return s;
        }
    }
    return fs->r->v->file(fs->r, rep, err);
}

static atlas_status need_operands(const cli_state *st, size_t want, const char *usage,
                                  atlas_err *err) {
    if (st->operand_count < want) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas %s", usage);
    }
    if (st->operand_count > want) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unexpected extra argument \"%s\" (usage: atlas %s)",
                             st->operands[want], usage);
    }
    return ATLAS_OK;
}

static void scan_opts_from_cli(const cli_state *st, atlas_scan_opts *so) {
    atlas_scan_opts_init(so);
    so->skip_history = st->opts.no_history;
    so->max_commits = st->opts.max_commits;
    so->timeout_ms = st->opts.timeout_ms;
}

static atlas_status event_item_sink(const atlas_event_row *row, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->event_item(ls->r, row, err);
}

/* --- A1: commands that touch no database -------------------------------- */

/* `atlas daemon run`. Foreground, and it owns its own data directory and lock,
 * so it deliberately does not go through atlas_ctx_open: taking the lock twice
 * would deadlock against itself. */
static atlas_status run_daemon(cli_state *st, atlas_err *err) {
    atlas_daemon_opts dopts;
    atlas_daemon_opts_init(&dopts);
    dopts.data_dir_override = st->opts.data_dir;
    dopts.run_once = st->opts.run_once;
    /* Log lines go to stderr so that stdout stays clean for anything a future
     * caller might want to parse, and so journald captures them either way. */
    return atlas_daemon_run(&dopts, st->errout, err);
}

static atlas_status run_service(cli_state *st, atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas service print|install --user|uninstall --user");
    }
    const char *sub = st->operands[0];
    atlas_renderer r;
    atlas_status result;

    atlas_buf exe = ATLAS_BUF_INIT;
    atlas_status st2 = atlas_unit_self_path(&exe, err);
    if (st2 != ATLAS_OK) {
        atlas_buf_free(&exe);
        return st2;
    }

    if (strcmp(sub, "print") == 0) {
        if (st->operand_count != 1u) {
            atlas_buf_free(&exe);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas service print");
        }
        atlas_buf unit = ATLAS_BUF_INIT;
        result = atlas_unit_render(atlas_buf_cstr(&exe), st->opts.data_dir, &unit, err);
        if (result == ATLAS_OK) {
            result = renderer_open(&r, st->opts.json, st->out, "service print", err);
            if (result == ATLAS_OK) {
                result = r.v->unit_text(&r, atlas_buf_cstr(&unit), err);
            }
            if (result == ATLAS_OK) {
                result = renderer_close(&r, err);
            } else {
                renderer_abort(&r);
            }
        }
        atlas_buf_free(&unit);
        atlas_buf_free(&exe);
        return result;
    }

    bool uninstall = (strcmp(sub, "uninstall") == 0);
    if (!uninstall && strcmp(sub, "install") != 0) {
        atlas_buf_free(&exe);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown service subcommand \"%s\"", sub);
    }
    if (st->operand_count != 1u) {
        atlas_buf_free(&exe);
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas service %s --user", sub);
    }
    /* --user is required rather than assumed. Atlas only ever installs a user
     * unit, and making that explicit means a command copied from documentation
     * that expects a system unit fails loudly instead of quietly doing something
     * different. */
    if (!st->opts.user) {
        atlas_buf_free(&exe);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas service %s needs --user. Atlas installs a systemd *user* unit "
                             "and never a system one: the daemon runs as you, reads your index, "
                             "and never needs root.",
                             sub);
    }

    atlas_unit_install_report rep;
    atlas_unit_install_report_init(&rep);
    result = uninstall ? atlas_unit_uninstall(st->opts.force, &rep, err)
                       : atlas_unit_install(atlas_buf_cstr(&exe), st->opts.data_dir,
                                            st->opts.force, &rep, err);
    if (result == ATLAS_OK) {
        result = renderer_open(&r, st->opts.json, st->out,
                               uninstall ? "service uninstall" : "service install", err);
        if (result == ATLAS_OK) {
            result = r.v->unit_install(&r, &rep, uninstall, err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(&r, err);
        } else {
            renderer_abort(&r);
        }
    }
    atlas_unit_install_report_free(&rep);
    atlas_buf_free(&exe);
    return result;
}

/* `atlas integrate claude ...`.
 *
 * Opens no index and contacts no daemon except to ask whether one is answering.
 * `install` writes exactly one file, in the user's own configuration directory;
 * `uninstall` removes that one file and nothing else. Neither ever edits a
 * Claude-owned file or touches a systemd unit. */
static atlas_status run_integrate(cli_state *st, atlas_err *err) {
    if (st->operand_count < 2u || strcmp(st->operands[0], "claude") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas integrate claude print|doctor|install --user|"
                             "uninstall --user");
    }
    const char *sub = st->operands[1];
    if (st->operand_count != 2u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas integrate claude %s", sub);
    }

    bool install = (strcmp(sub, "install") == 0);
    bool uninstall = (strcmp(sub, "uninstall") == 0);
    bool print = (strcmp(sub, "print") == 0);
    bool doctor = (strcmp(sub, "doctor") == 0);
    if (!install && !uninstall && !print && !doctor) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown integrate subcommand \"%s\"", sub);
    }
    /* --user is required rather than assumed, for the same reason `service
     * install` requires it: everything Atlas writes here is per-user, and a
     * command copied from documentation that expected otherwise should fail
     * loudly instead of quietly doing something else. */
    if ((install || uninstall) && !st->opts.user) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas integrate claude %s needs --user. Atlas writes a per-user "
                             "record and never anything system-wide.",
                             sub);
    }

    atlas_integrate_report rep;
    atlas_integrate_report_init(&rep);
    atlas_status result;
    if (install) {
        result = atlas_integrate_claude_install(&rep, err);
    } else if (uninstall) {
        result = atlas_integrate_claude_uninstall(&rep, err);
    } else {
        result = atlas_integrate_claude_doctor(&rep, err);
    }

    atlas_buf commands = ATLAS_BUF_INIT;
    if (result == ATLAS_OK && print) {
        result = atlas_integrate_claude_commands(&rep, &commands, err);
    }

    if (result == ATLAS_OK) {
        atlas_renderer r;
        char command[64];
        (void)snprintf(command, sizeof(command), "integrate claude %s", sub);
        result = renderer_open(&r, st->opts.json, st->out, command, err);
        if (result == ATLAS_OK) {
            result = r.v->integrate(&r, &rep, sub, print ? atlas_buf_cstr(&commands) : NULL, err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(&r, err);
        } else {
            renderer_abort(&r);
        }
    }
    /* `doctor` reports a problem through its exit code so it is usable in a
     * shell conditional, having already written a complete document. */
    if (result == ATLAS_OK && doctor && !rep.ok) {
        st->rendered = true;
        result = ATLAS_ERR_CONFIG;
    }
    atlas_buf_free(&commands);
    atlas_integrate_report_free(&rep);
    return result;
}

/* `atlas daemon ping` never opens the index: it answers one question about the
 * socket, and must work even when the index is unreadable. */
static atlas_status run_daemon_ping(cli_state *st, atlas_err *err) {
    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_err perr;
    atlas_err_init(&perr);
    bool reachable = false;
    const char *detail = NULL;

    atlas_status result = atlas_ipc_socket_path(&sock, &perr);
    if (result != ATLAS_OK) {
        detail = atlas_err_msg(&perr);
    } else if (atlas_ipc_call(atlas_buf_cstr(&sock), "daemon.ping", "{}", &resp, &perr) ==
               ATLAS_OK) {
        reachable = (strstr(atlas_buf_cstr(&resp), "\"pong\":true") != NULL);
        if (!reachable) {
            detail = "the daemon answered but did not acknowledge the ping";
        }
    } else {
        detail = atlas_err_msg(&perr);
    }

    atlas_renderer r;
    result = renderer_open(&r, st->opts.json, st->out, "daemon ping", err);
    if (result == ATLAS_OK) {
        result = r.v->daemon_ping(&r, reachable, atlas_buf_cstr(&sock), detail, err);
    }
    if (result == ATLAS_OK) {
        result = renderer_close(&r, err);
    } else {
        renderer_abort(&r);
    }
    atlas_buf_free(&sock);
    atlas_buf_free(&resp);
    /* Exit 0 only when the daemon actually answered, so the command is usable in
     * a shell conditional. The document above is complete and correct, so no
     * error document is added on top of it. */
    if (result == ATLAS_OK && !reachable) {
        st->rendered = true;
        return ATLAS_ERR_CONFIG;
    }
    return result;
}

/* Which commands need to own the writer.
 *
 * Everything else opens in AUTO mode, which takes the lock when it is free and
 * degrades to a read-only handle when the daemon holds it. That is what lets
 * every read command keep working while the daemon is running. */
static atlas_ctx_mode mode_for(const cli_state *st) {
    const char *cmd = st->command;
    /* `doctor` observes and creates nothing: no data directory, no database, no
     * lock, no migration. A diagnostic that initialises what it is diagnosing
     * can only ever answer "fine", and it cannot be run at all on a machine
     * where Atlas has never been used — which is exactly when somebody wants
     * to run it. */
    if (strcmp(cmd, "doctor") == 0) {
        return ATLAS_CTX_INSPECT;
    }
    if (strcmp(cmd, "scan") == 0) {
        return ATLAS_CTX_WRITE;
    }
    /* A6. The gate reads, and it must never take the writer lock even when it
     * is free.
     *
     * AUTO would take it, and a gate query that held the writer lock is a gate
     * query that stops indexing for as long as it runs — which contradicts the
     * one operational promise the phase makes about itself. READ is not a
     * fallback here; it is the guarantee, and `scripts/perf-a6.sh` runs a scan
     * against a concurrent gate query to check that it holds. */
    if (strcmp(cmd, "gate") == 0) {
        return ATLAS_CTX_READ;
    }
    /* A9.2. `verify show` assesses and writes nothing, so it must not take the
     * writer lock — the reason `gate` is READ. `verify run` records a result and
     * may transition, so it needs AUTO. `verify policy` opens no index at all,
     * and READ is what lets it answer on a machine where Atlas has never run. */
    if (strcmp(cmd, "verify") == 0 && st->operand_count > 0 &&
        (strcmp(st->operands[0], "show") == 0 || strcmp(st->operands[0], "policy") == 0)) {
        return ATLAS_CTX_READ;
    }
    if (strcmp(cmd, "repo") == 0 && st->operand_count > 0 &&
        (strcmp(st->operands[0], "add") == 0 || strcmp(st->operands[0], "remove") == 0)) {
        return ATLAS_CTX_WRITE;
    }
    return ATLAS_CTX_AUTO;
}

/* True when a mutation should be handed to the daemon rather than performed
 * here. Checked before the context is opened, because opening in WRITE mode
 * would fail against a running daemon — which is the correct behaviour for a
 * command that is *not* routed, and the wrong error for one that is. */
static bool route_to_daemon(const cli_state *st) {
    if (mode_for(st) != ATLAS_CTX_WRITE) {
        return false;
    }
    /* Reachability is not the question. There is one socket per user runtime
     * directory, but the data directory is chosen per invocation, so a daemon
     * that answers may well own a different index — and routing to it would
     * apply the write there while `--data-dir` said otherwise and nothing
     * reported the difference. The daemon has to be the one that owns *this*
     * directory.
     *
     * When it is not, the command runs locally and takes that directory's own
     * writer lock, which the daemon does not hold. That is the correct
     * behaviour and not a degradation. */
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf dir = ATLAS_BUF_INIT;
    bool owns = false;
    if (atlas_datadir_resolve(st->opts.data_dir, &dir, NULL, &err) == ATLAS_OK) {
        owns = atlas_ipc_daemon_owns(atlas_buf_cstr(&dir));
    }
    atlas_buf_free(&dir);
    return owns;
}

/* Sends one mutation to the daemon and renders its answer. */
static atlas_status call_daemon_mutation(cli_state *st, const char *repo, const char *method,
                                         const char *params,
                                         const char *command, atlas_err *err) {
    atlas_buf sock = ATLAS_BUF_INIT;
    atlas_buf resp = ATLAS_BUF_INIT;
    atlas_status result = atlas_ipc_socket_path(&sock, err);
    if (result == ATLAS_OK) {
        result = atlas_ipc_call(atlas_buf_cstr(&sock), method, params, &resp, err);
    }
    if (result == ATLAS_OK && strstr(atlas_buf_cstr(&resp), "\"ok\":true") == NULL) {
        /* The daemon's own message is surfaced rather than replaced, so the user
         * sees why it refused. */
        const char *m = strstr(atlas_buf_cstr(&resp), "\"message\":\"");
        atlas_buf msg = ATLAS_BUF_INIT;
        if (m != NULL) {
            m += 11;
            for (; *m != '\0' && *m != '"'; m++) {
                if (*m == '\\' && m[1] != '\0') {
                    m++;
                }
                (void)atlas_buf_append_ch(&msg, *m, err);
            }
        }
        result = atlas_err_set(err, ATLAS_ERR_INTERNAL, "%s",
                               msg.len > 0 ? atlas_buf_cstr(&msg)
                                           : "the Atlas daemon refused the request");
        atlas_buf_free(&msg);
    }
    if (result == ATLAS_OK) {
        atlas_renderer r;
        result = renderer_open(&r, st->opts.json, st->out, command, err);
        if (result == ATLAS_OK) {
            /* The daemon performed it; the renderer reports that plainly rather
             * than pretending this process did the work.
             *
             * The repository is named. It used to be the empty string, which
             * put `"repo":""` in the JSON document of a routed `atlas scan` —
             * a field that looks answered and says nothing, and the one
             * difference between the routed form of the command and the local
             * one. */
            result = r.v->note_repo(&r, repo != NULL ? repo : "", err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(&r, err);
        } else {
            renderer_abort(&r);
        }
    }
    atlas_buf_free(&sock);
    atlas_buf_free(&resp);
    return result;
}

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


/* --- A4: decision documents ------------------------------------------------
 *
 * The CLI's whole job here is argument shape and renderer choice.
 * `service_decision.c` decides everything else, including whether a write goes
 * to the daemon's writer thread or is taken on this one.
 *
 * One rule is enforced here and nowhere else, because here is where the flag
 * exists: **`--yes` cannot approve anything.** It is refused explicitly rather
 * than ignored, because a flag that is silently ignored is a flag somebody will
 * put in a script and believe. */

typedef struct decision_render {
    cli_state *st;
    atlas_renderer *r;
} decision_render;

static atlas_status on_decision_item(const atlas_decision_summary *s, void *ud, atlas_err *err) {
    decision_render *dr = (decision_render *)ud;
    return dr->r->v->decision_item(dr->r, s, err);
}

/* `decision history` emits two differently shaped lists, and the service layer
 * delivers all the revisions before any of the events. The list boundaries are
 * therefore opened lazily on the first item of each: opening them up front
 * would need the counts before they are known. */
/* Migration 10: one edge event on its way to a renderer. */
typedef struct decision_edge_render {
    cli_state *st;
    atlas_renderer *r;
    int64_t count;
} decision_edge_render;

static atlas_status on_decision_edge(const atlas_decision_edge_entry *e, void *ud,
                                     atlas_err *err) {
    decision_edge_render *er = (decision_edge_render *)ud;
    er->count++;
    return er->r->v->decision_edge(er->r, e, err);
}

typedef struct decision_history_render {
    cli_state *st;
    atlas_renderer *r;
    int64_t revisions;
    int64_t events;
    bool in_events;
} decision_history_render;

static atlas_status on_history_revision(const atlas_decision_summary *s, void *ud,
                                        atlas_err *err) {
    decision_history_render *dr = (decision_history_render *)ud;
    if (dr->revisions == 0) {
        atlas_status st = dr->r->v->code_list_begin(dr->r, "revisions", err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    dr->revisions++;
    return dr->r->v->decision_item(dr->r, s, err);
}

static atlas_status on_history_event(const atlas_decision_timeline_entry *e, void *ud,
                                     atlas_err *err) {
    decision_history_render *dr = (decision_history_render *)ud;
    if (!dr->in_events) {
        atlas_status st = ATLAS_OK;
        if (dr->revisions > 0) {
            st = dr->r->v->code_list_end(dr->r, "revisions", "revision", "revisions",
                                         dr->revisions, false, err);
        } else {
            st = dr->r->v->code_list_begin(dr->r, "revisions", err);
            if (st == ATLAS_OK) {
                st = dr->r->v->code_list_end(dr->r, "revisions", "revision", "revisions", 0, false,
                                             err);
            }
        }
        if (st == ATLAS_OK) {
            st = dr->r->v->code_list_begin(dr->r, "timeline", err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        dr->in_events = true;
    }
    dr->events++;
    return dr->r->v->decision_event(dr->r, e, err);
}

/* An A2 proposal, rendered through the decision-summary shape.
 *
 * Reusing that shape rather than adding a fifth renderer method is deliberate:
 * a legacy proposal *is* a decision-shaped thing that has not been promoted,
 * and giving it its own shape would be a second place for the two to describe
 * one record differently. Its `status` says so in words. */
static atlas_status on_legacy_item(const atlas_decision_legacy_view *v, void *ud,
                                   atlas_err *err) {
    decision_render *dr = (decision_render *)ud;
    atlas_decision_summary s;
    atlas_decision_summary_init(&s);
    atlas_status st = atlas_buf_appendf(&s.uid, err, "a2-proposal-%lld", (long long)v->id);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s.status, v->imported ? "PROMOTED" : "A2_PROPOSAL", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&s.title, v->title.data, v->title.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&s.proposed_by, v->provenance.data, v->provenance.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&s.created_at, v->created_at.data, v->created_at.len, err);
    }
    if (st == ATLAS_OK) {
        /* Where it went, when it went anywhere. */
        st = atlas_buf_set(&s.superseded_by, v->imported_uid.data, v->imported_uid.len, err);
    }
    s.link_count = v->path_count;
    if (st == ATLAS_OK) {
        st = dr->r->v->decision_item(dr->r, &s, err);
    }
    atlas_decision_summary_free(&s);
    return st;
}

static void decision_input_from(const cli_state *st, atlas_decision_input *in) {
    memset(in, 0, sizeof(*in));
    in->title = st->opts.decision.title;
    in->context_text = st->opts.decision.context_text;
    in->decision_text = st->opts.decision.decision_text;
    in->rationale_text = st->opts.decision.rationale;
    in->consequences_text = st->opts.decision.consequences;
    in->scope = st->opts.decision.scope;
    in->kind = st->opts.decision.kind;
    in->alternatives = st->opts.decision.alternatives;
    in->alternative_count = st->opts.decision.alternative_count;
    in->paths = st->opts.decision.paths;
    in->path_count = st->opts.decision.path_count;
    in->commits = st->opts.decision.commits;
    in->commit_count = st->opts.decision.commit_count;
    in->symbols = st->opts.decision.symbols;
    in->symbol_count = st->opts.decision.symbol_count;
    in->decision_links = st->opts.decision.decision_links;
    in->decision_link_count = st->opts.decision.decision_link_count;
    in->dedup_key = st->opts.decision.dedup_key;
}

static atlas_status render_outcome(cli_state *st, atlas_renderer *r, const char *command,
                                   const atlas_decision_outcome *o, atlas_err *err) {
    atlas_status result = renderer_open(r, st->opts.json, st->out, command, err);
    if (result == ATLAS_OK) {
        result = r->v->decision_outcome(r, o, err);
    }
    if (result == ATLAS_OK) {
        result = renderer_close(r, err);
    } else {
        renderer_abort(r);
    }
    return result;
}


/* --- A6: the impact gate -----------------------------------------------------
 *
 * A read command with an exit code that means something. `--at` names the exact
 * repository state the caller is asking about; `--path` narrows the question;
 * `--depth` bounds the structural walk and is refused rather than clamped,
 * because a silently reduced depth is a silently smaller answer.
 *
 * The non-zero exits are the whole point of the command existing rather than
 * `gate check | grep`. PASS is 0, REVIEW_REQUIRED is 8, BLOCKED is 9 — and they
 * are distinct because an automation that treats "a human should look at this"
 * and "Atlas could not tell" identically will eventually be handed the second
 * and behave as though it got the first. */
/* --- A9.2: verification ------------------------------------------------------
 *
 * `verify show` and `verify policy` are reads. `verify run` can change a
 * lifecycle state and is the one subcommand that writes.
 *
 * There is no `verify approve`, no `verify override` and no flag that lowers a
 * threshold, and their absence is deliberate rather than unfinished: every
 * bound on what Atlas may automate lives in a root-owned file that this process
 * cannot edit. A command-line switch that relaxed one would hand the constrained
 * principal the power to unconstrain itself, which is the argument A7 makes
 * about `ATLAS_AUTHORITY_POLICY_PATH` and it holds here word for word.
 *
 * `verify policy` opens no index and binds nothing, so it answers on a machine
 * where Atlas has never run — which is exactly when somebody asks it. */
/* The six intake verbs.
 *
 * These are **intake, not lifecycle finalisation**, and that is why none of
 * them opens a terminal, mints a challenge or asks for a confirmation. Stating
 * a claim, citing evidence and attesting change no knowledge record and no
 * lifecycle state; requiring a `/dev/tty` for them would be ceremony borrowed
 * from an operation with entirely different consequences, and would make the
 * verification workflow unusable from the scripts and agents it exists for.
 * `decision approve` still needs everything it always needed.
 *
 * What the operator channel *does* buy here is one thing: the actor recorded
 * carries PEER_AUTHENTICATED identity rather than SELF_DECLARED, which is a
 * claim about a uid and never about a person. A7.1's honesty limits hold word
 * for word.
 *
 * The local/remote choice is the same one `verify show` makes and for the same
 * reason: under A7.1 the index is 0700 `atlasd`, so from the operator's account
 * the socket is the only path that can write. */
static atlas_status run_verify_intake(cli_state *st, atlas_ctx *ctx, atlas_renderer *r,
                                      const char *sub, atlas_err *err) {
    atlas_verify_op op;
    atlas_verify_op_init(&op);
    /* Which channel the local CLI speaks on is decided by the **root-owned
     * authority policy**, not by the fact that this is the CLI.
     *
     * Asserting OPERATOR unconditionally was wrong, and the fixture caught it:
     * on a machine where the profile is LOCKED — no root-owned policy, or a
     * binary the running uid can replace — the authority to speak as the
     * operator does not exist, so claiming it would mint exactly the
     * PEER_AUTHENTICATED row A7 says nothing may mint from an unprivileged
     * shape. It would also have made the two paths disagree: the socket edge
     * already asks this same probe, so a locked profile downgraded a request
     * that arrived over the socket and not one that ran locally.
     *
     * MODEL is the floor rather than a refusal. A locked profile does not stop
     * anybody recording evidence; it stops the record claiming the uid was
     * established when nothing established it. */
    atlas_authority auth;
    atlas_authority_probe(&auth);
    op.channel = auth.state == ATLAS_AUTHORITY_GRANTED ? ATLAS_VERIFY_CHANNEL_OPERATOR
                                                       : ATLAS_VERIFY_CHANNEL_MODEL;
    op.self_confidence = st->opts.verify.self_confidence > 0
                             ? (int)st->opts.verify.self_confidence
                             : -1;
    op.line_start = st->opts.verify.line_start;
    op.line_end = st->opts.verify.line_end;

    atlas_status s = ATLAS_OK;
    const char *sem = st->opts.verify.semantics;
    if (strcmp(sub, "claim") == 0) {
        op.kind = ATLAS_VERIFY_OP_CLAIM_CREATE;
        if (st->opts.verify.text == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify claim --repo R --text \"...\" "
                                 "[--semantics DESCRIPTIVE|NORMATIVE] [--record UID] [--domain D] "
                                 "[--scope S] [--verifier V --verifier-input I] [--commit OID]");
        }
        if (sem != NULL && !atlas_verify_claim_semantics_parse(sem, &op.semantics)) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "--semantics is DESCRIPTIVE or NORMATIVE: it says whether the "
                                 "claim observes what is or declares what ought to be");
        }
        op.semantics_given = sem != NULL;
    } else if (strcmp(sub, "evidence") == 0) {
        op.kind = ATLAS_VERIFY_OP_EVIDENCE_ADD;
        if (st->opts.verify.claim == NULL || st->opts.verify.cls == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify evidence --claim UID --class C [--path P] "
                                 "[--symbol S] [--commit OID] [--observed \"...\"]");
        }
        if (!atlas_verify_evidence_class_parse(st->opts.verify.cls, &op.evidence_class)) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "--class must name an evidence class; there is no unclassified "
                                 "evidence");
        }
    } else if (strcmp(sub, "produce") == 0) {
        op.kind = ATLAS_VERIFY_OP_EVIDENCE_PRODUCE;
        if (st->opts.verify.claim == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify produce --claim UID [--verifier V]");
        }
    } else if (strcmp(sub, "attest") == 0) {
        op.kind = ATLAS_VERIFY_OP_ATTESTATION_ADD;
        if (st->opts.verify.claim == NULL || st->opts.verify.verdict == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify attest --claim UID --verdict "
                                 "SUPPORT|CONTRADICT|INCONCLUSIVE [--evidence \"uid uid\"] "
                                 "[--method M] [--self-confidence N] [--supersedes UID]");
        }
        if (!atlas_verify_verdict_parse(st->opts.verify.verdict, &op.verdict)) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "--verdict is SUPPORT, CONTRADICT or INCONCLUSIVE");
        }
    } else if (strcmp(sub, "depend") == 0) {
        op.kind = ATLAS_VERIFY_OP_DEPENDENCY_ADD;
        if (st->opts.verify.evidence == NULL || st->opts.verify.derives_from == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify depend --evidence UID --derives-from UID");
        }
    } else {
        op.kind = ATLAS_VERIFY_OP_EVALUATE;
        if (st->opts.verify.claim == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify evaluate --claim UID [--repo R]");
        }
    }

    /* `--path`, `--commit` and `--scope` are already spoken for by the A4 arm,
     * so they are read from where that arm puts them rather than given a second
     * spelling. One flag, one meaning. */
    const char *path = st->opts.decision.path_count > 0 ? st->opts.decision.paths[0] : NULL;
    const char *commit = st->opts.decision.commit_count > 0 ? st->opts.decision.commits[0] : NULL;
    static const struct {
        size_t opt_off;
        size_t op_off;
    } COPY[] = {
        {offsetof(atlas_cli_opts, verify.claim), offsetof(atlas_verify_op, claim_uid)},
        {offsetof(atlas_cli_opts, verify.text), offsetof(atlas_verify_op, text)},
        {offsetof(atlas_cli_opts, verify.domain), offsetof(atlas_verify_op, domain)},
        {offsetof(atlas_cli_opts, verify.decision), offsetof(atlas_verify_op, document_uid)},
        {offsetof(atlas_cli_opts, verify.verifier), offsetof(atlas_verify_op, verifier)},
        {offsetof(atlas_cli_opts, verify.verifier_input),
         offsetof(atlas_verify_op, verifier_input)},
        {offsetof(atlas_cli_opts, verify.environment), offsetof(atlas_verify_op, environment)},
        {offsetof(atlas_cli_opts, verify.symbol), offsetof(atlas_verify_op, symbol)},
        {offsetof(atlas_cli_opts, verify.target), offsetof(atlas_verify_op, target)},
        {offsetof(atlas_cli_opts, verify.probe), offsetof(atlas_verify_op, probe)},
        {offsetof(atlas_cli_opts, verify.observed), offsetof(atlas_verify_op, observed)},
        {offsetof(atlas_cli_opts, verify.observed_at), offsetof(atlas_verify_op, observed_at)},
        {offsetof(atlas_cli_opts, verify.method), offsetof(atlas_verify_op, method)},
        {offsetof(atlas_cli_opts, verify.supersedes), offsetof(atlas_verify_op, supersedes_uid)},
        {offsetof(atlas_cli_opts, verify.actor), offsetof(atlas_verify_op, actor_name)},
        {offsetof(atlas_cli_opts, verify.provider), offsetof(atlas_verify_op, actor_provider)},
        {offsetof(atlas_cli_opts, verify.role), offsetof(atlas_verify_op, actor_role)},
    };
    for (size_t i = 0; s == ATLAS_OK && i < sizeof COPY / sizeof COPY[0]; i++) {
        const char *v = *(const char **)((char *)&st->opts + COPY[i].opt_off);
        if (v != NULL) {
            s = atlas_buf_set_str((atlas_buf *)((char *)&op + COPY[i].op_off), v, err);
        }
    }
    /* A dependency names the derived evidence in `--evidence`; every other verb
     * means "the evidence this attestation rests on" by the same flag. */
    if (s == ATLAS_OK && st->opts.verify.evidence != NULL) {
        s = atlas_buf_set_str(op.kind == ATLAS_VERIFY_OP_DEPENDENCY_ADD ? &op.derived_uid
                                                                        : &op.evidence_uids,
                              st->opts.verify.evidence, err);
    }
    if (s == ATLAS_OK && st->opts.verify.derives_from != NULL) {
        s = atlas_buf_set_str(&op.source_uid, st->opts.verify.derives_from, err);
    }
    if (s == ATLAS_OK && st->opts.repo != NULL) {
        s = atlas_buf_set_str(&op.repo_name, st->opts.repo, err);
    }
    if (s == ATLAS_OK && st->opts.decision.scope != NULL) {
        s = atlas_buf_set_str(&op.scope_note, st->opts.decision.scope, err);
    }
    if (s == ATLAS_OK && path != NULL) {
        s = atlas_buf_set_str(&op.path_text, path, err);
    }
    if (s == ATLAS_OK && commit != NULL) {
        s = atlas_buf_set_str(op.kind == ATLAS_VERIFY_OP_EVIDENCE_ADD ? &op.commit_oid
                                                                      : &op.basis_commit,
                              commit, err);
    }

    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    if (s == ATLAS_OK) {
        /* The test is whether this process *holds the writer lock*, not whether
         * it has a context at all. With a daemon running, `atlas_ctx` in AUTO
         * mode still opens — read-only — so `ctx != NULL` is true and the local
         * write then fails with "attempt to write a readonly database". A4's
         * predicate is the right one: apply locally when this process is the
         * writer, and go over the socket when something else is. */
        s = (ctx != NULL && atlas_ctx_is_writer(ctx))
                ? atlas_verify_intake_apply(atlas_ctx_db(ctx), &op, &res, err)
                : atlas_service_verify_intake_remote(&op, &res, err);
    }
    if (s == ATLAS_OK) {
        s = renderer_open(r, st->opts.json, st->out, "verify", err);
    }
    if (s == ATLAS_OK) {
        s = r->v->verify_intake(r, sub, &res, err);
    }
    s = s == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), s);
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
    return s;
}

static atlas_status run_verify(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas verify show CLAIM-ID | atlas verify run NAME CLAIM-ID "
                             "| atlas verify policy");
    }
    const char *sub = st->operands[0];
    atlas_verify_report rep;
    atlas_verify_report_init(&rep);
    atlas_status result;

    if (strcmp(sub, "policy") == 0) {
        if (st->operand_count != 1) {
            atlas_verify_report_free(&rep);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas verify policy");
        }
        result = atlas_service_verify_policy(&rep, err);
    } else if (strcmp(sub, "show") == 0) {
        if (st->operand_count != 2) {
            atlas_verify_report_free(&rep);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas verify show CLAIM");
        }
        /* A claim may be named by its uid — which is what every surface reports
         * — or by the rowid A9.2's CLI took. A uid never parses as a number, so
         * the two spellings cannot collide.
         *
         * The remote form is not a fallback: under A7.1 the index is 0700
         * `atlasd`, so from the operator's account it is the *only* form that
         * can answer. A9.2.1 shipped the method without this branch and the
         * command reported "no index is available to read" about a claim the
         * daemon was holding. */
        char *endp = NULL;
        long long as_id = strtoll(st->operands[1], &endp, 10);
        bool numeric = endp != NULL && *endp == '\0' && endp != st->operands[1];
        result = ctx != NULL
                     ? atlas_service_verify_show_on(atlas_ctx_db(ctx), numeric ? as_id : 0,
                                                    numeric ? NULL : st->operands[1], &rep, err)
                     : atlas_service_verify_show_remote(numeric ? as_id : 0,
                                                        numeric ? NULL : st->operands[1], &rep,
                                                        err);
    } else if (strcmp(sub, "claim") == 0 || strcmp(sub, "evidence") == 0 ||
               strcmp(sub, "produce") == 0 || strcmp(sub, "attest") == 0 ||
               strcmp(sub, "depend") == 0 || strcmp(sub, "evaluate") == 0) {
        result = run_verify_intake(st, ctx, r, sub, err);
        atlas_verify_report_free(&rep);
        return result;
    } else if (strcmp(sub, "run") == 0) {
        if (st->operand_count != 3) {
            atlas_verify_report_free(&rep);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas verify run NAME CLAIM-ID");
        }
        /* The same two spellings `verify show` accepts, and for the same
         * reason: the uid is what every surface reports, the rowid is what
         * A9.2's CLI took, and a uid never parses as a number. */
        char *rend = NULL;
        long long run_id = strtoll(st->operands[2], &rend, 10);
        bool run_numeric = rend != NULL && *rend == '\0' && rend != st->operands[2];
        result = atlas_service_verify_run(ctx, run_numeric ? run_id : 0,
                                          run_numeric ? NULL : st->operands[2], st->operands[1],
                                          &rep, err);
    } else {
        atlas_verify_report_free(&rep);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas verify claim|evidence|produce|attest|depend|evaluate|"
                             "show|run|policy");
    }

    if (result == ATLAS_OK) {
        result = renderer_open(r, st->opts.json, st->out, "verify", err);
    }
    if (result == ATLAS_OK) {
        result = r->v->verify(r, &rep, err);
    }
    if (result == ATLAS_OK) {
        result = renderer_close(r, err);
    } else {
        renderer_abort(r);
    }
    atlas_verify_report_free(&rep);
    return result;
}

static atlas_status run_gate(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas gate check NAME | atlas gate show NAME DECISION-ID");
    }
    const char *sub = st->operands[0];
    bool one = strcmp(sub, "show") == 0;
    if (!one && strcmp(sub, "check") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas gate check|show");
    }
    size_t want = one ? 3u : 2u;
    if (st->operand_count != want) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas gate %s NAME%s", sub,
                             one ? " DECISION-ID" : "");
    }

    atlas_gate_report rep;
    atlas_gate_report_init(&rep);
    atlas_status result;
    if (one) {
        result = ctx != NULL ? atlas_service_gate_show(ctx, st->operands[1], st->operands[2],
                                                      st->opts.decision.at_commit, &rep, err)
                             : atlas_service_gate_show_remote(st->operands[1], st->operands[2],
                                                              &rep, err);
    } else {
        atlas_gate_query q;
        atlas_gate_query_init(&q);
        q.repo_name = st->operands[1];
        q.at_commit = st->opts.decision.at_commit;
        q.depth = st->opts.depth;
        for (size_t i = 0; i < st->opts.decision.path_count; i++) {
            q.paths[q.path_count++] = st->opts.decision.paths[i];
        }
        result = ctx != NULL ? atlas_service_gate_check(ctx, &q, &rep, err)
                             : atlas_service_gate_check_remote(&q, &rep, err);
    }
    if (result == ATLAS_OK) {
        result = renderer_open(r, st->opts.json, st->out, "gate", err);
    }
    if (result == ATLAS_OK) {
        result = r->v->gate(r, &rep, err);
    }
    if (result == ATLAS_OK) {
        result = renderer_close(r, err);
        /* Only once the document is complete. A non-zero exit beside a
         * half-written answer would tell a caller to act on something it cannot
         * read. */
        st->gate_exit = atlas_gate_exit_code(rep.result);
    } else {
        renderer_abort(r);
    }
    atlas_gate_report_free(&rep);
    return result;
}

/* The operator-only verbs — approve, reject, supersede, revalidate and A9.1's
 * resolve. One function: they differ by intent and by whether a replacement is
 * required, and five copies of the `--yes` refusal would be five chances for one
 * of them to be missing. */
static atlas_status run_decision_confirm(cli_state *st, atlas_ctx *ctx, atlas_renderer *r,
                                         atlas_decision_intent intent, atlas_err *err) {
    /* **A7: authority before anything else, including argument shape.**
     *
     * First, so that a locked profile never reaches the terminal, never mints a
     * capability, and never prints a confirmation prompt. A prompt in a locked
     * profile would be a question whose answer any process with this uid can
     * supply, which is the thing A7 exists to stop pretending about. */
    atlas_status auth = atlas_authority_require(ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE, err);
    if (auth != ATLAS_OK) {
        return auth;
    }
    if (st->operand_count != 3u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision %s NAME DECISION-ID%s",
                             atlas_decision_intent_name(intent),
                             intent == ATLAS_DECISION_INTENT_SUPERSEDE ? " --by DECISION-ID" : "");
    }
    if (st->opts.yes) {
        /* Refused, not ignored. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--yes cannot approve, reject, supersede or resolve a decision. This "
                             "command needs an interactive terminal, and Atlas will not accept a "
                             "confirmation from a flag, a pipe, a file or an environment "
                             "variable.");
    }
    if (st->opts.json) {
        /* The prompt goes to the terminal and the result to stdout, so --json
         * would interleave a human prompt with a machine document. Refusing is
         * clearer than producing either one badly. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--json is not available for %s: it is an interactive command",
                             atlas_decision_intent_name(intent));
    }
    const char *replacement = st->opts.decision.by;
    if (intent == ATLAS_DECISION_INTENT_SUPERSEDE && replacement == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas decision supersede needs --by DECISION-ID, the decision that "
                             "replaces this one");
    }
    if (intent != ATLAS_DECISION_INTENT_SUPERSEDE && replacement != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "--by is only meaningful for supersede");
    }
    atlas_decision_outcome out;
    atlas_decision_outcome_init(&out);
    atlas_status result = atlas_service_decision_confirm(
        ctx, st->operands[1], st->operands[2], intent, replacement, st->opts.decision.revision,
        &out, err);
    if (result == ATLAS_OK) {
        result = render_outcome(st, r, "decision", &out, err);
    }
    atlas_decision_outcome_free(&out);
    return result;
}

static atlas_status run_decision(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
                                 atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas decision list|show|search|history|links|for-file|"
                             "propose|revise|approve|reject|supersede|revalidate|resolve|export|"
                             "orphaned|"
                             "legacy|promote ...");
    }
    const char *sub = st->operands[0];
    atlas_status result;

    if (strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0 || strcmp(sub, "for-file") == 0) {
        size_t want = strcmp(sub, "list") == 0 ? 2u : 3u;
        if (st->operand_count != want) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 strcmp(sub, "list") == 0
                                     ? "usage: atlas decision list NAME [--status STATUS] "
                                       "[--kind KIND]"
                                     : (strcmp(sub, "search") == 0
                                            ? "usage: atlas decision search NAME QUERY"
                                            : "usage: atlas decision for-file NAME PATH"));
        }
        atlas_decision_list_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.limit = limit;
        if (strcmp(sub, "search") == 0) {
            opts.mode = ATLAS_DECISION_LIST_SEARCH;
            opts.query = st->operands[2];
        } else if (strcmp(sub, "for-file") == 0) {
            opts.mode = ATLAS_DECISION_LIST_PATH;
            opts.path = st->operands[2];
        } else if (st->opts.decision.status != NULL) {
            atlas_decision_state parsed;
            if (!atlas_decision_state_parse(st->opts.decision.status, &parsed)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "--status is PROPOSED, APPROVED, REJECTED, SUPERSEDED or "
                                     "RESOLVED");
            }
            opts.mode = ATLAS_DECISION_LIST_STATUS;
            opts.status = st->opts.decision.status;
        }
        /* A9.1. The kind filter is orthogonal to the mode, so it is set after the
         * mode is chosen and applies to all three. Validated here as well as at
         * the service layer, because a misspelt kind must be a usage error rather
         * than an empty result that looks like an answer. */
        if (st->opts.decision.kind != NULL) {
            atlas_decision_kind parsed;
            if (!atlas_decision_kind_parse(st->opts.decision.kind, &parsed)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--kind is one of %s",
                                     atlas_decision_kind_list());
            }
            opts.kind = st->opts.decision.kind;
        }
        result = renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        /* Zeroed here rather than trusted to the callee: the remote path fills
         * only the counts the daemon actually reported, so an older daemon that
         * omits one would otherwise have it read from uninitialised stack. */
        atlas_decision_counts counts;
        memset(&counts, 0, sizeof(counts));
        int64_t count = 0;
        bool more = false;
        result = r->v->note_repo(r, st->operands[1], err);
        if (result == ATLAS_OK) {
            result = r->v->list_begin(r, "decisions", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL
                         ? atlas_service_decision_list(ctx, st->operands[1], &opts,
                                                       on_decision_item, &dr, &counts, &count,
                                                       &more, err)
                         : atlas_service_decision_list_remote(st->operands[1], &opts,
                                                              on_decision_item, &dr, &count, &more,
                                                              &counts, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "decision", "decisions", count, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->decision_counts(r, &counts, err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "show") == 0 || strcmp(sub, "export") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision %s NAME DECISION-ID",
                                 sub);
        }
        bool markdown = strcmp(sub, "export") == 0 &&
                        (st->opts.decision.format == NULL ||
                         strcmp(st->opts.decision.format, "markdown") == 0);
        if (strcmp(sub, "export") == 0 && !markdown && st->opts.decision.format != NULL &&
            strcmp(st->opts.decision.format, "json") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "--format is markdown or json");
        }
        atlas_decision_document doc;
        atlas_decision_document_init(&doc);
        result = ctx != NULL
                     ? atlas_service_decision_show(ctx, st->operands[1], st->operands[2],
                                                   st->opts.decision.revision, &doc, err)
                     : atlas_service_decision_show_remote(st->operands[1], st->operands[2],
                                                          st->opts.decision.revision, &doc, err);
        if (result == ATLAS_OK && markdown) {
            /* Markdown goes to stdout as itself rather than through a renderer:
             * it is a document, not a report, and wrapping it would make it
             * unusable for the one thing an export is for. Never written into
             * the target repository — Atlas is read-only there. */
            result = atlas_service_decision_export_markdown(&doc, st->out, err);
            st->rendered = true;
        } else if (result == ATLAS_OK) {
            bool as_json = st->opts.json || strcmp(sub, "export") == 0;
            result = renderer_open(r, as_json, st->out, "decision", err);
            if (result == ATLAS_OK) {
                result = r->v->decision_show(r, &doc, err);
            }
            if (result == ATLAS_OK) {
                result = renderer_close(r, err);
            } else {
                renderer_abort(r);
            }
        }
        atlas_decision_document_free(&doc);
        return result;
    }

    /* `decision links REPO ID` — the account of one document's relations.
     *
     * A read, and a different ledger from `decision history`: that one is about
     * the document's lifecycle, this one about its edges. Keeping them apart is
     * why an edge annotation cannot be mistaken for a lifecycle transition. */
    if (strcmp(sub, "links") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision links NAME "
                                                       "DECISION-ID");
        }
        result = renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        result = r->v->note_repo(r, st->operands[1], err);
        if (result == ATLAS_OK) {
            result = r->v->code_list_begin(r, "edges", err);
        }
        decision_edge_render er = {st, r, 0};
        int64_t n = 0;
        bool more = false;
        if (result == ATLAS_OK) {
            result = atlas_service_decision_links(ctx, st->operands[1], st->operands[2],
                                                  on_decision_edge, &er, &n, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_end(r, "edges", "edge", "edges", n, more, err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "history") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas decision history NAME DECISION-ID");
        }
        result = renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        bool agrees = true;
        result = r->v->note_repo(r, st->operands[1], err);
        /* **Two lists, and they are two lists.**
         *
         * A revision entry and a timeline entry are different shapes, so
         * putting both in one array would produce a document whose meaning
         * depends on which member a parser happens to look at. The named-list
         * pair carries its own count per list, which is exactly what it exists
         * for — see the comment on `code_list_begin`. */
        decision_history_render dr = {st, r, 0, 0, false};
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_decision_history(
                                       ctx, st->operands[1], st->operands[2], on_history_revision,
                                       on_history_event, &dr, &agrees, err)
                                 : atlas_service_decision_history_remote(
                                       st->operands[1], st->operands[2], on_history_revision,
                                       on_history_event, &dr, &agrees, err);
        }
        if (result == ATLAS_OK && dr.in_events) {
            result = r->v->code_list_end(r, "timeline", "event", "events", dr.events, false, err);
        } else if (result == ATLAS_OK) {
            /* A document with revisions but no events cannot exist — a proposal
             * writes both — so this is the "no revisions at all" case, and both
             * empty lists are still emitted so the shape does not change. */
            result = r->v->code_list_begin(r, "revisions", err);
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "revisions", "revision", "revisions", 0, false,
                                             err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_begin(r, "timeline", err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "timeline", "event", "events", 0, false, err);
            }
        }
        if (result == ATLAS_OK) {
            /* The ledger is canonical and the status columns cache it. Whether
             * the two agree is reported on every timeline, because a timeline
             * is exactly where somebody would notice. */
            result = r->v->decision_ledger(r, agrees, err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "propose") == 0 || strcmp(sub, "revise") == 0) {
        bool revise = strcmp(sub, "revise") == 0;
        size_t want = revise ? 3u : 2u;
        if (st->operand_count != want) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 revise ? "usage: atlas decision revise NAME DECISION-ID --title T "
                                          "--decision D [...]"
                                        : "usage: atlas decision propose NAME --title T "
                                          "--decision D [...]");
        }
        if (st->opts.decision.title == NULL || st->opts.decision.decision_text == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a decision needs --title and --decision");
        }
        atlas_decision_input in;
        decision_input_from(st, &in);
        atlas_decision_outcome out;
        atlas_decision_outcome_init(&out);
        result = revise ? atlas_service_decision_revise(ctx, st->operands[1], st->operands[2], &in,
                                                        &out, err)
                        : atlas_service_decision_propose(ctx, st->operands[1], &in, &out, err);
        if (result == ATLAS_OK) {
            result = render_outcome(st, r, "decision", &out, err);
        }
        atlas_decision_outcome_free(&out);
        return result;
    }

    /* `decision link add SOURCE TARGET`. A proposal write, not an operator
     * operation: it goes through the same authority path `propose` and `revise`
     * use, and mints nothing. */
    if (strcmp(sub, "link") == 0) {
        bool adding = st->operand_count == 5u && strcmp(st->operands[1], "add") == 0;
        bool removing = st->operand_count == 5u && strcmp(st->operands[1], "remove") == 0;
        /* `note` records one event about an edge and touches no link. It is how
         * the history of a relation that is already gone gets written down:
         * there is nothing left to add or remove, only something to say. */
        bool noting = st->operand_count == 5u && strcmp(st->operands[1], "note") == 0;
        if (!adding && !removing && !noting) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas decision link add|remove|note REPO SOURCE_ID "
                                 "TARGET_ID [--why TEXT] [--provenance P] [--event E]");
        }
        /* `--why` is required to withdraw a relation and optional to draw one.
         * The asymmetry is deliberate: an addition that arrives without a
         * reason can be explained later by annotating the edge, but a removal
         * is the last thing that happens to it, so if the reason is not
         * recorded now it is not recorded at all. */
        const char *why = st->opts.decision.why;
        const char *prov = st->opts.decision.provenance != NULL ? st->opts.decision.provenance
                                                                : "OPERATOR";
        if (noting && (why == NULL || *why == '\0')) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "recording a note about a relation needs "
                                                       "--why");
        }
        if (removing && (why == NULL || *why == '\0')) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "withdrawing a relation needs --why: the reason is the only "
                                 "thing that will still explain it afterwards");
        }
        atlas_decision_outcome o;
        atlas_decision_outcome_init(&o);
        bool removed = false;
        atlas_status lr;
        if (noting) {
            lr = atlas_service_decision_link_note(ctx, st->operands[2], st->operands[3],
                                                  st->operands[4], why, prov,
                                                  st->opts.decision.edge_event, &o, err);
        } else if (adding) {
            lr = atlas_service_decision_link_add(ctx, st->operands[2], st->operands[3],
                                                 st->operands[4], why, prov, &o, err);
        } else {
            lr = atlas_service_decision_link_remove(ctx, st->operands[2], st->operands[3],
                                                    st->operands[4], why, &o, &removed, err);
        }
        if (lr == ATLAS_OK) {
            o.is_removal = removing;
            o.removed = removed;
            lr = render_outcome(st, r,
                                noting    ? "decision link note"
                                : adding  ? "decision link add"
                                          : "decision link remove",
                                &o, err);
        }
        atlas_decision_outcome_free(&o);
        return lr;
    }

    if (strcmp(sub, "orphaned") == 0) {
        /* Decisions attached to no live repository. Takes no repository name,
         * because a repository is exactly what these do not have. */
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision orphaned");
        }
        result = renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        int64_t count = 0;
        bool more = false;
        result = r->v->list_begin(r, "orphaned", err);
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_decision_orphans(ctx, limit, on_decision_item,
                                                                  &dr, &count, &more, err)
                                 : atlas_service_decision_orphans_remote(limit, on_decision_item,
                                                                         &dr, &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "orphaned decision", "orphaned decisions", count, err);
        }
        if (result == ATLAS_OK && count > 0) {
            /* The remedy, printed with the finding. An orphan is recoverable
             * and a user who does not know that will assume it is not. */
            result = r->v->note_repo(
                r,
                "register-the-original-repository-and-rescan-to-reattach-these", err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "legacy") == 0) {
        /* The A2 proposals this repository still holds, and which of them have
         * been promoted. Read-only over the A2 tables, which A4 never writes. */
        if (st->operand_count != 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision legacy NAME");
        }
        result = renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        int64_t count = 0;
        bool more = false;
        result = r->v->note_repo(r, st->operands[1], err);
        if (result == ATLAS_OK) {
            result = r->v->list_begin(r, "a2_proposals", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_decision_legacy(ctx, st->operands[1], limit,
                                                                 on_legacy_item, &dr, &count,
                                                                 &more, err)
                                 : atlas_service_decision_legacy_remote(st->operands[1], limit,
                                                                        on_legacy_item, &dr,
                                                                        &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "proposal", "proposals", count, err);
        }
        if (result == ATLAS_OK) {
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "promote") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas decision promote NAME LEGACY-ID");
        }
        char *endp = NULL;
        long legacy = strtol(st->operands[2], &endp, 10);
        if (endp == NULL || *endp != '\0' || legacy <= 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "the A2 proposal id is a positive number, as shown by "
                                 "`atlas decision legacy`");
        }
        atlas_decision_outcome out;
        atlas_decision_outcome_init(&out);
        result = atlas_service_decision_promote(ctx, st->operands[1], legacy, &out, err);
        if (result == ATLAS_OK) {
            result = render_outcome(st, r, "decision", &out, err);
        }
        atlas_decision_outcome_free(&out);
        return result;
    }

    if (strcmp(sub, "approve") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_APPROVE, err);
    }
    if (strcmp(sub, "revalidate") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_REVALIDATE, err);
    }
    if (strcmp(sub, "reject") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_REJECT, err);
    }
    if (strcmp(sub, "supersede") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_SUPERSEDE, err);
    }
    /* A9.1. The same interactive channel as approve, reject, supersede and
     * revalidate, through the same function, so there is one place a lifecycle
     * capability is minted and spent and this adds no second path. */
    if (strcmp(sub, "resolve") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_RESOLVE, err);
    }

    return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown decision subcommand \"%s\"", sub);
}

/* Which commands this process can answer about an index it does not own.
 *
 * A7.1 puts the index behind a separate OS principal: `/var/lib/atlas` is 0700
 * `atlasd`, and it has to stay that way because `atlas-worker` is a member of
 * the client group and must not be able to read the index. So a client uid can
 * never open that database, and every read has to be a question put to the
 * daemon. This function decides only *whether* a command has such a path; the
 * command itself then runs its normal code with `ctx == NULL`, calling the
 * remote twin of its service function at the same call site, so both renderers
 * and the JSON contract are shared rather than reproduced.
 *
 * A command with no path here is refused with what is actually true, rather
 * than with SQLite's "unable to open database file" — which describes a
 * permission the caller was never going to have and points at a file they
 * cannot see. There is no fallback to a local read anywhere: A7.1's rule is
 * that a client which cannot reach the daemon fails, rather than quietly
 * answering from the pre-cutover per-user database. */
static bool remote_serves(const cli_state *st) {
    const char *cmd = st->command;
    const char *sub = st->operand_count > 0 ? st->operands[0] : "";

    /* `doctor` needs no path: it opens in INSPECT mode, creates nothing, and
     * reporting an index it cannot read is the correct answer and the reason
     * somebody runs it. */
    if (strcmp(cmd, "doctor") == 0) {
        return true;
    }
    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "search") == 0 ||
        strcmp(cmd, "events") == 0 || strcmp(cmd, "sync") == 0 || strcmp(cmd, "file") == 0 ||
        strcmp(cmd, "history") == 0 || strcmp(cmd, "diff") == 0) {
        return true;
    }
    /* Keyed on the subcommand, not the command: a command whose siblings have
     * no remote form would otherwise reach its handler with a NULL context and
     * dereference it. */
    if (strcmp(cmd, "repo") == 0) {
        /* Only `list`. `add` and `remove` are absent deliberately — nothing over
         * the socket may change the registry — and there is no `repo state`
         * subcommand: the repository-state report belongs to `atlas events`,
         * which is routed in its own block. */
        return strcmp(sub, "list") == 0;
    }
    if (strcmp(cmd, "daemon") == 0) {
        return strcmp(sub, "status") == 0;
    }
    if (strcmp(cmd, "code") == 0) {
        return strcmp(sub, "status") == 0 || strcmp(sub, "file") == 0 ||
               strcmp(sub, "symbol") == 0 || strcmp(sub, "search") == 0 ||
               strcmp(sub, "deps") == 0 || strcmp(sub, "impact") == 0 ||
               /* A8-CI. Four reads, served by the ordinary method group. Under
                * A7.1 these are the only forms that work at all from an
                * operator's account: the index is 0700 `atlasd`. `index` is
                * absent on purpose — it is a write and has no read method. */
               /* A8-CI closeout: indexing is served over the socket now, so an
                * operator never has to stop the service or become the service
                * account. It is offered only to the peer the root-owned policy
                * names, so reaching the name is not the same as being allowed
                * to use it. */
               strcmp(sub, "index") == 0 ||
               /* A9.2.3. Served for the same reason `index` is: under A7.1 the
                * index is 0700 `atlasd`, so an operator's account has no local
                * handle and could not record a build description at all. It is
                * in the operator-uid group, so being served is not being
                * allowed. */
               strcmp(sub, "sem-config") == 0 ||
               strcmp(sub, "sem-status") == 0 || strcmp(sub, "semantic") == 0 ||
               strcmp(sub, "callers") == 0 || strcmp(sub, "callees") == 0 ||
               strcmp(sub, "trace") == 0 || strcmp(sub, "sem-impact") == 0 ||
               strcmp(sub, "tests") == 0 || strcmp(sub, "explain") == 0;
    }
    if (strcmp(cmd, "gate") == 0) {
        return strcmp(sub, "check") == 0 || strcmp(sub, "show") == 0;
    }
    if (strcmp(cmd, "verify") == 0) {
        /* A9.2.1. The six intake verbs are served too, and they have to be:
         * under A7.1 the index is 0700 `atlasd`, so the socket is the only
         * path by which an operator's account can record a claim at all.
         * Being served is not being authorised — `verify.evaluate` may cause
         * Atlas to move a lifecycle state, but the gates are in a root-owned
         * file no peer here can read, let alone edit.
         *
         * **`run` is deliberately absent from this list, because there is no
         * `verify.run` method.** Claiming it was served cost the accuracy of
         * the one message that explains the situation: on a system deployment
         * the command opened a context, failed to get a writable index, and
         * said "no index is available to write" — which reads as a broken
         * install rather than as an operation this account cannot perform
         * here. Saying nothing is served is the truthful answer, and it points
         * at the documented gap instead of at the operator's machine. */
        return strcmp(sub, "show") == 0 ||
               strcmp(sub, "policy") == 0 || strcmp(sub, "claim") == 0 ||
               strcmp(sub, "evidence") == 0 || strcmp(sub, "produce") == 0 ||
               strcmp(sub, "attest") == 0 || strcmp(sub, "depend") == 0 ||
               strcmp(sub, "evaluate") == 0;
    }
    if (strcmp(cmd, "context") == 0) {
        return strcmp(sub, "build") == 0;
    }
    /* Served over the socket and by nothing else: the operations table lives in
     * the daemon's memory, so there is no local form of this question and never
     * will be. */
    if (strcmp(cmd, "operation") == 0) {
        return strcmp(sub, "status") == 0;
    }
    /* Backup create and verify, and deliberately not restore.
     *
     * A5 gave backup no remote form because the uid that owns the index can
     * copy the file anyway. Under A7.1 that stopped being true and nobody
     * noticed: the index is 0700 `atlasd`, so the operator account could not
     * take a backup at all and got "there is no Atlas index to back up" — which
     * is false. These two are served over the socket and refused by the daemon
     * unless the peer is the uid the root-owned policy names. `restore`
     * remains local-only: replacing the record should require stopping the
     * daemon, which is exactly what the local path already enforces through the
     * writer lock. */
    if (strcmp(cmd, "backup") == 0) {
        return strcmp(sub, "create") == 0 || strcmp(sub, "verify") == 0;
    }
    if (strcmp(cmd, "decision") == 0) {
        return strcmp(sub, "link") == 0 || strcmp(sub, "links") == 0 ||
               strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0 ||
               strcmp(sub, "for-file") == 0 || strcmp(sub, "show") == 0 ||
               strcmp(sub, "export") == 0 || strcmp(sub, "history") == 0 ||
               strcmp(sub, "orphaned") == 0 || strcmp(sub, "legacy") == 0 ||
               /* The operator channel. Served over the socket, and refused by
                * the daemon unless this peer is the uid the root-owned policy
                * names — so reaching the name is not the same as being allowed
                * to use it. `propose` and `revise` are already served by the
                * ordinary decision group. */
               strcmp(sub, "propose") == 0 || strcmp(sub, "revise") == 0 ||
               strcmp(sub, "promote") == 0 || strcmp(sub, "approve") == 0 ||
               strcmp(sub, "reject") == 0 || strcmp(sub, "supersede") == 0 ||
               strcmp(sub, "revalidate") == 0 || strcmp(sub, "resolve") == 0;
    }
    return false;
}

/* Whether `cmd` is a command at all.
 *
 * Without this, an unknown command on a foreign index is answered by the
 * refusal below — which tells somebody who mistyped that the system index is
 * daemon-owned, and never that there is no such command. The dispatch chain's
 * own unknown-command error is produced after a context is opened, which is
 * exactly what a foreign index does not allow, so the check has to happen here.
 *
 * A new command must be added to this list. That duplication is real; what
 * catches it is that the command then reports the wrong error for every client
 * on a system deployment, which the CLI smoke matrix runs. */
static bool is_a_command(const char *cmd) {
    static const char *const COMMANDS[] = {
        "doctor",  "repo",    "scan",      "status",  "search",  "file",     "history",
        "diff",    "daemon",  "sync",      "events",  "code",    "decision", "gate",
        "job",     "dispatcher", "backup", "maintenance", "service", "mcp",  "hook",
        "integrate", "version", "help", "context", "operation", "api-key", "gateway",
        "verify",
    };
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strcmp(cmd, COMMANDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether this invocation would write the index.
 *
 * `mode_for` answers it for `scan`, `repo add` and `repo remove`, which it must
 * because it also chooses the context mode. It answers AUTO for `code index`
 * and `code sync`, which is right for a per-user install — AUTO takes the
 * writer lock when it is free — and wrong for the question asked here.
 *
 * The consequence was that on a system deployment both fell through to the
 * generic "not served over the socket" answer, which does not say that the
 * operation exists and that this account may not perform it. The deterministic
 * NOT_AUTHORIZED distinction was implemented and unreachable on the one
 * deployment where it is the whole point.
 *
 * Two names, not an inventory: these are the only commands that write the index
 * and are not already WRITE. A third would be a deliberate edit here, and the
 * failure if it is forgotten is a vaguer message rather than a wrong one. */
static bool would_write_index(const cli_state *st) {
    if (mode_for(st) == ATLAS_CTX_WRITE) {
        return true;
    }
    return strcmp(st->command, "code") == 0 && st->operand_count > 0 &&
           (strcmp(st->operands[0], "index") == 0 || strcmp(st->operands[0], "sync") == 0);
}

static atlas_status remote_refuse(const cli_state *st, atlas_err *err) {
    /* A write against somebody else's index is not a permissions problem to be
     * reported from inside a chmod; it is a thing this account does not do.
     * Registration and scanning under a system deployment are the operator
     * ceremony in docs/security/A7_1_OPERATIONS.md.
     *
     * NOT_AUTHORIZED leads, as a stable token rather than prose, so a caller
     * tells it from NOT_REGISTERED without reading English. The two answer
     * different questions — "Atlas does not hold this" and "Atlas holds it and
     * you may not do this to it" — and a caller that cannot tell them apart
     * will retry the wrong one. */
    if (would_write_index(st)) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "NOT_AUTHORIZED: `atlas %s%s%s` writes the index, and the index is "
                             "owned by the Atlas service account. The operation exists and this "
                             "account may not perform it. Indexing, registration and scanning are "
                             "operator operations performed as the service account; see "
                             "docs/security/A7_1_OPERATIONS.md.",
                             st->command, st->operand_count > 0 ? " " : "",
                             st->operand_count > 0 ? st->operands[0] : "");
    }
    /* Deliberately says "is not served" rather than "has no daemon-served form
     * yet". The second wording asserts that the name exists, and a mistyped
     * subcommand reaches here too — `atlas code sem-symbol`, which has never
     * been a command, was told the system index is daemon-owned and that its
     * command is merely unavailable. Both halves were false, and the reader's
     * next move is to go looking for a feature nobody has removed.
     *
     * The honest answer covers both cases without a second copy of the command
     * inventory. A list here would be a fourth one — after `COMMANDS[]`,
     * `remote_serves` and the dispatch chain — and its drift would turn a
     * working command into "unknown" on every system deployment, which is a
     * worse fault than an imprecise sentence about a typo. `atlas help` is the
     * one authority, so the message points at it. */
    return atlas_err_set(err, ATLAS_ERR_CONFIG,
                         "the system index is owned by the Atlas service account and cannot be "
                         "read directly, and `atlas %s%s%s` is not served over the socket — it is "
                         "either a write operation or not a command. Every read-only command is "
                         "served; `backup restore` and `maintenance` are local operator "
                         "operations with no RPC surface by design. Run `atlas help` for the "
                         "command list.",
                         st->command, st->operand_count > 0 ? " " : "",
                         st->operand_count > 0 ? st->operands[0] : "");
}

/* The semantic commands need a writable-or-readable handle on the index
 * itself. Under A7.1 the index is 0700 `atlasd`, so an operator running this
 * from their own account has no context and is told exactly that rather than
 * being given an empty answer. Serving these over the socket is the daemon
 * side of the same work and is where this refusal goes away. */
static const char SEM_LOCAL_ONLY[] =
    "NOT_AUTHORIZED: building a semantic index writes to the index, and this process cannot open "
    "it for writing; on a system deployment the index is owned by the `atlasd` account. Every "
    "semantic *read* is served over the socket.";

/* `atlas context build --repo R --task "..."`.
 *
 * The task description ranks evidence Atlas already holds and does nothing
 * else: it selects no repository, authorises nothing, and no imperative in it
 * can cause a write, because this path reaches only read functions. */
static atlas_status run_context(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
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
        result = renderer_open(r, st->opts.json, st->out, "context build", err);
        if (result == ATLAS_OK) {
            result = r->v->sem_context(r, &rep, err);
        }
        result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
    }
    atlas_sem_context_report_free(&rep);
    atlas_buf_free(&seed_paths);
    atlas_buf_free(&seed_symbols);
    return result;
}

static atlas_status run_code(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
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
            result = renderer_open(r, st->opts.json, st->out, "code index", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_indexed(r, &sum, err);
            }
            result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
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
                result = renderer_open(r, st->opts.json, st->out, "code semantic", err);
                if (result == ATLAS_OK) {
                    result = r->v->sem_symbols(r, &rep, err);
                }
                result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
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
            result = renderer_open(r, st->opts.json, st->out, "code graph", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_graph(r, &rep, err);
            }
            result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
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
            result = renderer_open(r, st->opts.json, st->out, "code impact", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_impact(r, &rep, err);
            }
            result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
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
            result = renderer_open(r, st->opts.json, st->out, "code sem-status", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_status(r, &rep, err);
            }
            result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
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
            result = renderer_open(r, st->opts.json, st->out, "code sem-config", err);
            if (result == ATLAS_OK) {
                result = r->v->sem_config(r, &rep, err);
            }
            result = result == ATLAS_OK ? renderer_close(r, err) : (renderer_abort(r), result);
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
            result = renderer_open(r, st->opts.json, st->out, "code status", err);
            if (result == ATLAS_OK) {
                result = r->v->code_status(r, &rep, err);
            }
            if (result == ATLAS_OK) {
                result = renderer_close(r, err);
            } else {
                renderer_abort(r);
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
            result = renderer_open(r, st->opts.json, st->out, "code sync", err);
            if (result == ATLAS_OK) {
                result = r->v->note_repo(r, st->operands[1], err);
            }
            if (result == ATLAS_OK) {
                result = r->v->sync(r, st->operands[1], &rep, err);
            }
            if (result == ATLAS_OK) {
                result = renderer_close(r, err);
            } else {
                renderer_abort(r);
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
            result = renderer_open(r, st->opts.json, st->out, "code file", err);
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
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        atlas_code_file_report_free(&rep);
        return result;
    }

    if (strcmp(sub, "symbol") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code symbol NAME SYMBOL");
        }
        result = renderer_open(r, st->opts.json, st->out, "code symbol", err);
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
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "search") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas code search NAME QUERY");
        }
        result = renderer_open(r, st->opts.json, st->out, "code search", err);
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
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
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

        result = renderer_open(r, st->opts.json, st->out,
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
            result = renderer_close(r, err);
        } else {
            renderer_abort(r);
        }
        return result;
    }

    return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown code subcommand \"%s\"", sub);
}


/* --- A8: `atlas job` and `atlas dispatcher` -------------------------------- */

typedef struct job_render_ctx {
    atlas_renderer *r;
} job_render_ctx;

static atlas_status emit_job(const atlas_job_render *jr, void *ud, atlas_err *err) {
    job_render_ctx *jc = (job_render_ctx *)ud;
    return jc->r->v->job_item(jc->r, jr, err);
}

/* No `atlas_ctx`, deliberately.
 *
 * A job command speaks only to the daemon: orchestration state lives in the
 * index, `atlasd` is the only writer of it, and on a separated deployment no
 * other account can even open the file. Opening a context here would try to
 * prepare a data directory this uid does not own — which is exactly what it did
 * during the A8 cutover, and the reason these commands are dispatched before any
 * context is opened, alongside `dispatcher`, `backup` and `restore`. */
static atlas_status run_job(cli_state *st, atlas_renderer *r, int64_t limit, atlas_err *err) {
    const char *sub = st->operand_count > 0 ? st->operands[0] : NULL;
    if (sub == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas job <submit|get|list|cancel> (try: atlas help)");
    }
    job_render_ctx jc = {r};
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
        result = renderer_open(r, st->opts.json, st->out, "job submit", err);
        if (result == ATLAS_OK) {
            result = atlas_service_job_submit(NULL, &o, emit_job, &jc, err);
        }
    } else if (strcmp(sub, "get") == 0) {
        const char *job = st->operand_count > 1 ? st->operands[1] : NULL;
        result = renderer_open(r, st->opts.json, st->out, "job get", err);
        if (result == ATLAS_OK) {
            result = atlas_service_job_get(NULL, job, emit_job, &jc, err);
        }
    } else if (strcmp(sub, "cancel") == 0) {
        const char *job = st->operand_count > 1 ? st->operands[1] : NULL;
        result = renderer_open(r, st->opts.json, st->out, "job cancel", err);
        if (result == ATLAS_OK) {
            result = atlas_service_job_cancel(NULL, job, emit_job, &jc, err);
        }
    } else if (strcmp(sub, "list") == 0) {
        result = renderer_open(r, st->opts.json, st->out, "job list", err);
        int64_t count = 0;
        bool more = false;
        if (result == ATLAS_OK) {
            result = r->v->list_begin(r, "jobs", err);
        }
        if (result == ATLAS_OK) {
            result = atlas_service_job_list(NULL, 0, limit, emit_job, &jc, &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "job", "jobs", count, err);
        }
    } else {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown job subcommand \"%s\"", sub);
    }

    if (result == ATLAS_OK) {
        result = renderer_close(r, err);
    } else {
        renderer_abort(r);
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
static bool index_is_foreign(const cli_state *st) {
    bool foreign = false;
    atlas_buf resolved = ATLAS_BUF_INIT;
    atlas_datadir_source src = ATLAS_DATADIR_OVERRIDE;
    atlas_err rerr;
    atlas_err_init(&rerr);
    if (atlas_datadir_resolve(st->opts.data_dir, &resolved, &src, &rerr) == ATLAS_OK) {
        foreign = atlas_datadir_is_foreign(atlas_buf_cstr(&resolved), src);
    }
    atlas_buf_free(&resolved);
    return foreign;
}

/* Whether a credential operation has to go over the socket.
 *
 * Two independent reasons, and either alone is enough:
 *
 *   - **The index is foreign.** Under A7.1 it is 0700 `atlasd` and this account
 *     cannot open it at all.
 *   - **A daemon owns this directory.** It holds the writer lock, so the local
 *     path cannot take it — and telling an operator to stop the service in
 *     order to revoke a leaked credential is not an answer.
 *
 * The second is why this is not simply `index_is_foreign`. On an ordinary
 * single-user machine with the daemon running, the index is perfectly readable
 * and the lock is still held, which is the common case rather than an exotic
 * one. */
static bool apikey_needs_daemon(const cli_state *st) {
    if (index_is_foreign(st)) {
        return true;
    }
    atlas_buf dir = ATLAS_BUF_INIT;
    atlas_err rerr;
    atlas_err_init(&rerr);
    bool owns = false;
    if (atlas_datadir_resolve(st->opts.data_dir, &dir, NULL, &rerr) == ATLAS_OK) {
        owns = atlas_ipc_daemon_owns(atlas_buf_cstr(&dir));
    }
    atlas_buf_free(&dir);
    return owns;
}

static atlas_status run_command(cli_state *st, atlas_err *err) {
    const char *cmd = st->command;
    atlas_renderer r;
    atlas_status result;

    /* Commands that need no database or repository first. */
    if (strcmp(cmd, "version") == 0) {
        atlas_status s = need_operands(st, 0, "version", err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = renderer_open(&r, st->opts.json, st->out, "version", err);
        if (s == ATLAS_OK) {
            s = r.v->version(&r, err);
        }
        if (s == ATLAS_OK) {
            return renderer_close(&r, err);
        }
        renderer_abort(&r);
        return s;
    }

    const char *repo_arg_name = NULL;
    atlas_status s = take_name(st, &repo_arg_name, err);
    if (s != ATLAS_OK) {
        return s;
    }

    /* Commands that own their own lifecycle, before any context is opened. */
    if (strcmp(cmd, "daemon") == 0 && st->operand_count > 0 &&
        strcmp(st->operands[0], "run") == 0) {
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas daemon run");
        }
        return run_daemon(st, err);
    }
    if (strcmp(cmd, "daemon") == 0 && st->operand_count > 0 &&
        strcmp(st->operands[0], "ping") == 0) {
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas daemon ping");
        }
        return run_daemon_ping(st, err);
    }
    if (strcmp(cmd, "service") == 0) {
        return run_service(st, err);
    }
    /* The two adapters own their own I/O completely: `mcp` must put nothing but
     * protocol messages on stdout, and `hook` must put exactly one JSON object
     * there. Neither goes through a renderer, and neither opens the index. */
    if (strcmp(cmd, "mcp") == 0) {
        if (st->operand_count != 0u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas mcp");
        }
        atlas_mcp_opts mopts;
        atlas_mcp_opts_init(&mopts);
        mopts.timeout_ms = st->opts.timeout_ms;
        return atlas_mcp_run(stdin, st->out, st->errout, &mopts, err);
    }
    if (strcmp(cmd, "hook") == 0) {
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas hook EVENT");
        }
        atlas_hook_opts hopts;
        atlas_hook_opts_init(&hopts);
        hopts.timeout_ms = st->opts.timeout_ms;
        return atlas_hook_run(st->operands[0], stdin, st->out, st->errout, &hopts);
    }
    if (strcmp(cmd, "integrate") == 0) {
        return run_integrate(st, err);
    }

    /* Mutations go through the daemon when one is running, so that the single
     * writer stays single. */
    if (route_to_daemon(st)) {
        if (strcmp(cmd, "scan") == 0) {
            if (st->operand_count != 1u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas scan NAME");
            }
            atlas_buf params = ATLAS_BUF_INIT;
            atlas_status vst = atlas_db_check_repo_name(st->operands[0], err);
            if (vst == ATLAS_OK) {
                vst = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\",\"full\":true}",
                                        st->operands[0]);
            }
            if (vst == ATLAS_OK) {
                vst = call_daemon_mutation(st, st->operands[0], "repo.sync",
                                           atlas_buf_cstr(&params), "scan", err);
            }
            atlas_buf_free(&params);
            return vst;
        }
        /* **A7: the registry is not routed, because it has nowhere to route
         * to.**
         *
         * `repo.add` and `repo.remove` were RPC methods until A7, which meant
         * anything able to open the socket could decide which directories Atlas
         * treats as repositories it will read, index and answer about. That is
         * an authority decision, and the socket carries no authority: every
         * peer on it is the same uid as the daemon.
         *
         * They are now local operations under the data-directory write lock,
         * which the daemon holds while it runs. So this is a refusal, and it
         * says the actionable thing. It is A5's contract for restore and prune,
         * applied to the registry for the same reason: "the daemon must be
         * stopped" is then enforced by the kernel rather than promised in a
         * manual. */
        if (strcmp(st->operands[0], "add") == 0 || strcmp(st->operands[0], "remove") == 0) {
            /* One line, and no embedded newlines.
             *
             * An error message is untrusted-text-encoded on its way to the
             * terminal, which is right — parts of it can quote a path somebody
             * else chose. But that encoding applies to the whole string, so the
             * `\n` in Atlas' own control text came out as a literal `%0A` and
             * the three-line recipe printed as one unreadable run. The fix is
             * not to exempt the message from encoding — that would reopen the
             * hole for the part of it that is a path — but to stop putting
             * newlines in it.
             *
             * It also no longer names `systemctl --user`. That is right for a
             * per-user install and wrong for the system deployment this
             * refusal is most likely to be seen on, where the unit is
             * system-scoped and stopping it needs root. A recipe that is wrong
             * half the time is worse than a description of what has to be true,
             * so it states the condition and points at the one document that
             * knows which deployment this is. */
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "changing the repository registry is a local operation under the "
                                 "data-directory write lock, and the Atlas daemon currently holds "
                                 "it. Stop the daemon, run `atlas repo %s ...`, then start it "
                                 "again; the unit is user-scoped on a per-user install and "
                                 "system-scoped on a system deployment, see "
                                 "docs/security/A7_1_OPERATIONS.md. Atlas exposes no RPC method "
                                 "for this, so that nothing reachable over the socket — including "
                                 "MCP and hooks — can decide what Atlas indexes.",
                                 st->operands[0]);
        }
    }

    /* `operation status ID` — ask about a long operation.
     *
     * A read, and the counterpart to accepting one. Dispatched here with the
     * backup family because it is answered by the same operator-gated method
     * group, and because the operations it reports on are the two that take
     * longer than a client will hold a socket open for. */
    if (strcmp(cmd, "operation") == 0) {
        atlas_status ost = need_operands(st, 2, "operation status ID", err);
        if (ost != ATLAS_OK) {
            return ost;
        }
        if (strcmp(st->operands[0], "status") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas operation status ID");
        }
        char *end = NULL;
        long long id = strtoll(st->operands[1], &end, 10);
        if (end == NULL || *end != '\0' || id <= 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "an operation id is a positive integer");
        }
        atlas_operation_report rep;
        atlas_operation_report_init(&rep);
        ost = atlas_service_operation_status_remote((int64_t)id, &rep, err);
        if (ost == ATLAS_OK) {
            atlas_renderer orend;
            ost = renderer_open(&orend, st->opts.json, st->out, "operation status", err);
            if (ost == ATLAS_OK) {
                ost = orend.v->operation_status(&orend, &rep, err);
            }
            ost = ost == ATLAS_OK ? renderer_close(&orend, err)
                                  : (renderer_abort(&orend), ost);
        }
        atlas_operation_report_free(&rep);
        return ost;
    }

    /* --- A5: backup and maintenance -------------------------------------
     *
     * Handled here, before any context exists, because neither may go through
     * one. A context in AUTO mode takes the writer lock when it is free, and a
     * backup must never take it — the whole point is that a running daemon
     * keeps writing while the snapshot is taken. Restore and prune need the
     * lock *exclusively* and acquire it themselves, which is also what makes
     * "the daemon must be stopped" a fact the kernel enforces rather than an
     * instruction in a manual.
     *
     * Neither is routed to the daemon, because neither has an RPC method to
     * route to. That absence is the point: nothing reachable over the socket —
     * and so nothing reachable from MCP or a hook — can replace or prune the
     * index. */
    /* A8. The dispatcher is dispatched here, before any `atlas_ctx` is opened,
     * for the reason backup and restore are: a context in AUTO mode takes the
     * writer lock when it is free, and the dispatcher must never take it — it
     * runs as `atlas-worker`, which cannot open the index at all, and a code
     * path that tried would fail confusingly instead of never existing.
     *
     * It also needs no data directory: everything it acts on arrives over the
     * socket. */
    if (strcmp(cmd, "job") == 0) {
        atlas_renderer jr;
        memset(&jr, 0, sizeof(jr));
        int64_t limit = st->opts.limit > 0 ? st->opts.limit : ATLAS_DEFAULT_LIMIT;
        return run_job(st, &jr, limit, err);
    }

    if (strcmp(cmd, "dispatcher") == 0) {
        const char *sub = st->operand_count > 0 ? st->operands[0] : NULL;
        if (sub == NULL || strcmp(sub, "run") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas dispatcher run [--once]");
        }
        /* Logs to stderr so a systemd unit captures them in the journal without
         * the service needing a writable log path. */
        return atlas_service_dispatcher_run(st->opts.job.once, st->errout, err);
    }

    /* A9. Credential administration, dispatched here — before any `atlas_ctx`
     * is opened — for A5's reason about backup and prune: a context in AUTO
     * mode takes the writer lock when it is free, and these take it themselves.
     *
     * There is deliberately no MCP tool, no ordinary RPC method and no gateway
     * route that reaches any of this. The gateway runs as its own account,
     * which is neither the operator uid nor a dispatcher uid, so even the
     * operator-gated methods answer `unknown method` to it. Remote credential
     * administration is absent in A9 rather than refused. */
    /* A9. The gateway runs as its own process and its own account. Dispatched
     * here, before any `atlas_ctx` is opened, because it opens no index at all:
     * every answer it gives comes over the daemon socket. */
    if (strcmp(cmd, "gateway") == 0) {
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas gateway run|status");
        }
        if (strcmp(st->operands[0], "run") == 0) {
            return atlas_service_gateway_run(err);
        }
        if (strcmp(st->operands[0], "status") == 0) {
            return atlas_service_gateway_status(st->out, st->opts.json, err);
        }
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas gateway run|status");
    }

    if (strcmp(cmd, "api-key") == 0) {
        if (st->operand_count == 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas api-key create|list|revoke|rotate ...");
        }
        const char *sub = st->operands[0];
        atlas_status ks = ATLAS_OK;

        if (strcmp(sub, "create") == 0 || strcmp(sub, "rotate") == 0) {
            const bool rotating = strcmp(sub, "rotate") == 0;
            /* `rotate` takes the id it replaces; `create` takes nothing. */
            if (st->operand_count != (rotating ? 2u : 1u)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     rotating ? "usage: atlas api-key rotate KEY-ID --label L "
                                                "--scope S [--scope S...]"
                                              : "usage: atlas api-key create --label L "
                                                "--scope S [--scope S...]");
            }
            if (st->opts.label == NULL) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--label is required");
            }
            if (st->opts.scope_count == 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "at least one --scope is required; a credential with no "
                                     "scopes could not read anything");
            }
            /* Each scope is checked against the closed vocabulary here so the
             * refusal names the offending value. The service layer checks again
             * at the write point, which is the guarantee; this one is the
             * message. */
            atlas_scope_mask mask = 0u;
            for (size_t i = 0; i < st->opts.scope_count; i++) {
                atlas_apikey_scope one = atlas_apikey_scope_parse(st->opts.scopes[i]);
                if (one == ATLAS_SCOPE_UNKNOWN) {
                    /* The offending value is deliberately not echoed. It came
                     * from argv and could carry a control sequence into a
                     * terminal, and there is no safe pool open yet — the
                     * renderer has not been started, because nothing has
                     * succeeded. Listing the vocabulary is more actionable
                     * anyway: it says what to type rather than what was typed. */
                    atlas_buf known = ATLAS_BUF_INIT;
                    for (int si = 1; si < (int)ATLAS_SCOPE__COUNT; si++) {
                        atlas_apikey_scope sc = (atlas_apikey_scope)si;
                        if (!atlas_apikey_scope_grantable(sc)) {
                            continue;
                        }
                        if (known.len > 0) {
                            (void)atlas_buf_append_str(&known, ", ", err);
                        }
                        (void)atlas_buf_append_str(&known, atlas_apikey_scope_name(sc), err);
                    }
                    atlas_status us = atlas_err_set(err, ATLAS_ERR_USAGE,
                                                    "unknown scope; the grantable scopes are %s",
                                                    atlas_buf_cstr(&known));
                    atlas_buf_free(&known);
                    return us;
                }
                mask |= ATLAS_SCOPE_BIT(one);
            }
            atlas_apikey_create_opts co;
            memset(&co, 0, sizeof co);
            co.label = st->opts.label;
            co.scopes = mask;
            co.rotate_from = rotating ? st->operands[1] : NULL;

            atlas_apikey_created created;
            memset(&created, 0, sizeof created);
            /* Routed to the daemon when it owns this index: it holds the
             * writer lock, and under A7.1 this account cannot open the index at
             * all. The methods behind it are operator-gated, so this is the
             * operator's own uid asking, never a remote client. */
            ks = apikey_needs_daemon(st)
                     ? atlas_service_apikey_create_remote(&co, &created, err)
                     : atlas_service_apikey_create(st->opts.data_dir, &co, &created, err);
            if (ks == ATLAS_OK) {
                ks = renderer_open(&r, st->opts.json, st->out, "api-key", err);
                if (ks == ATLAS_OK) {
                    ks = r.v->apikey_created(&r, &created, err);
                }
                ks = ks == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), ks);
            }
            /* Wiped on every path, including the failing ones. This is the only
             * copy of the plaintext that will ever exist. */
            atlas_apikey_created_free(&created);
            return ks;
        }
        if (strcmp(sub, "list") == 0) {
            if (st->operand_count != 1u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas api-key list");
            }
            atlas_apikey_listing l;
            atlas_apikey_listing_init(&l);
            ks = apikey_needs_daemon(st) ? atlas_service_apikey_list_remote(&l, err)
                                         : atlas_service_apikey_list(st->opts.data_dir, &l, err);
            if (ks == ATLAS_OK) {
                ks = renderer_open(&r, st->opts.json, st->out, "api-key", err);
                if (ks == ATLAS_OK) {
                    ks = r.v->apikey_listed(&r, &l, err);
                }
                ks = ks == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), ks);
            }
            atlas_apikey_listing_free(&l);
            return ks;
        }
        if (strcmp(sub, "revoke") == 0) {
            if (st->operand_count != 2u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas api-key revoke KEY-ID");
            }
            char id[ATLAS_APIKEY_SELECTOR_HEX + 1];
            if (!atlas_apikey_id_normalise(st->operands[1], id)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "a key id is %u lowercase hex characters, optionally written "
                                     "\"" ATLAS_APIKEY_ID_PREFIX "<id>\"",
                                     (unsigned)ATLAS_APIKEY_SELECTOR_HEX);
            }
            bool changed = false;
            /* Revocation must never require stopping the daemon. */
            ks = apikey_needs_daemon(st)
                     ? atlas_service_apikey_revoke_remote(id, &changed, err)
                     : atlas_service_apikey_revoke(st->opts.data_dir, id, &changed, err);
            if (ks == ATLAS_OK) {
                ks = renderer_open(&r, st->opts.json, st->out, "api-key", err);
                if (ks == ATLAS_OK) {
                    ks = r.v->apikey_revoked(&r, id, changed, err);
                }
                ks = ks == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), ks);
            }
            return ks;
        }
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas api-key create|list|revoke|rotate ...");
    }

    if (strcmp(cmd, "backup") == 0) {
        if (st->operand_count == 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas backup create|verify|restore ...");
        }
        const char *sub = st->operands[0];
        if (strcmp(sub, "create") == 0) {
            /* A7 considered guarding this behind operator authority and did
             * not, for the reason set out in atlas/authority.h: the index is
             * readable by the uid that owns it, so `cp` produces the same file
             * with no Atlas code involved. A refusal here would relocate the
             * verb and protect nothing, while stopping the owner of an
             * ordinary single-user install from taking a backup. Where a real
             * separation exists, the filesystem already refuses. */
            if (st->operand_count != 2u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "usage: atlas backup create OUTPUT|NAME [--force]");
            }
            atlas_backup_create_opts bo;
            memset(&bo, 0, sizeof bo);
            bo.output = st->operands[1];
            bo.force = st->opts.force;
            atlas_backup_report rep;
            atlas_backup_report_init(&rep);
            /* Remote when this process does not own the index. The operand is
             * then a name inside the daemon's backup directory rather than a
             * path this account chooses, because a client that could name a
             * destination could make the daemon write anywhere it can reach. */
            atlas_status bs =
                !index_is_foreign(st)
                    ? atlas_service_backup_create(st->opts.data_dir, &bo, &rep, err)
                    : atlas_service_backup_create_remote(st->operands[1], &rep, NULL, err);
            if (bs == ATLAS_OK) {
                bs = renderer_open(&r, st->opts.json, st->out, "backup create", err);
                if (bs == ATLAS_OK) {
                    bs = r.v->backup_created(&r, &rep, err);
                }
                bs = bs == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), bs);
            }
            atlas_backup_report_free(&rep);
            return bs;
        }
        if (strcmp(sub, "verify") == 0) {
            if (st->operand_count != 2u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas backup verify BACKUP");
            }
            atlas_backup_verify_report rep;
            atlas_backup_verify_report_init(&rep);
            atlas_status bs = !index_is_foreign(st)
                                  ? atlas_service_backup_verify(st->operands[1], &rep, err)
                                  : atlas_service_backup_verify_remote(st->operands[1], &rep, err);
            if (bs == ATLAS_OK) {
                bs = renderer_open(&r, st->opts.json, st->out, "backup verify", err);
                if (bs == ATLAS_OK) {
                    bs = r.v->backup_verified(&r, &rep, "backup", err);
                }
                bs = bs == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), bs);
                /* A complete document, and then a non-zero exit: an unusable
                 * backup is an answer, not a failure to answer, and a script
                 * must be able to test both. `rendered` keeps the error
                 * document off stdout so --json emits exactly one. */
                if (bs == ATLAS_OK && !rep.ok) {
                    st->rendered = true;
                    bs = ATLAS_ERR_INTEGRITY;
                }
            }
            atlas_backup_verify_report_free(&rep);
            return bs;
        }
        if (strcmp(sub, "restore") == 0) {
            /* Unguarded for the same reason as `create`, in the other
             * direction: the index is writable by the uid that owns it, so
             * `mv` replaces it. */
            if (st->operand_count != 2u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "usage: atlas backup restore BACKUP --yes");
            }
            atlas_backup_restore_opts ro;
            memset(&ro, 0, sizeof ro);
            ro.input = st->operands[1];
            ro.confirmed = st->opts.yes;
            atlas_backup_restore_report rep;
            atlas_backup_restore_report_init(&rep);
            atlas_status bs = atlas_service_backup_restore(st->opts.data_dir, &ro, &rep, err);
            if (bs == ATLAS_OK) {
                bs = renderer_open(&r, st->opts.json, st->out, "backup restore", err);
                if (bs == ATLAS_OK) {
                    bs = r.v->backup_restored(&r, &rep, err);
                }
                bs = bs == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), bs);
            }
            atlas_backup_restore_report_free(&rep);
            return bs;
        }
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown backup subcommand \"%s\"", sub);
    }

    if (strcmp(cmd, "maintenance") == 0) {
        if (st->operand_count == 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas maintenance plan|prune ...");
        }
        const char *sub = st->operands[0];
        bool prune = strcmp(sub, "prune") == 0;
        if (!prune && strcmp(sub, "plan") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown maintenance subcommand \"%s\"",
                                 sub);
        }
        /* Maintenance opens the index directly, and is dispatched before any
         * context exists so that it cannot take the writer lock by accident.
         * That also means it never reaches the refusal every other command gets
         * on a system deployment — so from the operator's account it failed
         * with SQLite's own "unable to open database file", which describes a
         * permission the caller was never going to have and points at a file
         * they cannot see.
         *
         * The answer is the honest one, not an RPC method: A5 gives maintenance
         * no socket surface deliberately, because nothing reachable from a
         * model may prune the index, and adding one to improve an error message
         * would delete that guarantee. */
        /* On a system deployment this goes over the socket, where the daemon
         * offers it only to the peer the root-owned policy names.
         *
         * It used to refuse outright, and the refusal told the operator to
         * become the service account — which is precisely the "manual
         * service-account impersonation" a supported operation must not
         * require. A5's rule that maintenance has no RPC surface rested on the
         * premise that whoever owns the data directory can prune it anyway, and
         * A7.1 broke that premise without anyone noticing, exactly as it did
         * for backup. What A5 actually wanted — that nothing a model can reach
         * may prune the index — is untouched. */
        bool maint_remote = index_is_foreign(st);
        /* And the other way maintenance met a database it could not open: there
         * is no index at all. That is a perfectly ordinary state — nobody has
         * run Atlas in this data directory — and reporting it as SQLite's
         * "unable to open database file" tells the reader nothing they can act
         * on. `atlas doctor` already distinguishes "there is no index" from
         * "there is one I may not read"; so does this. */
        {
            atlas_buf dir = ATLAS_BUF_INIT;
            atlas_buf dbp = ATLAS_BUF_INIT;
            atlas_err rerr;
            atlas_err_init(&rerr);
            bool missing = false;
            if (!maint_remote &&
                atlas_datadir_resolve(st->opts.data_dir, &dir, NULL, &rerr) == ATLAS_OK &&
                atlas_buf_appendf(&dbp, &rerr, "%s/atlas.db", atlas_buf_cstr(&dir)) == ATLAS_OK) {
                missing = access(atlas_buf_cstr(&dbp), F_OK) != 0;
            }
            atlas_status mst = ATLAS_OK;
            if (missing) {
                mst = atlas_err_set(err, ATLAS_ERR_CONFIG,
                                    "there is no Atlas index in %s to maintain. Nothing has been "
                                    "indexed here yet; `atlas doctor` reports what Atlas can see",
                                    atlas_safe(&r.safe, atlas_buf_cstr(&dir)));
            }
            atlas_buf_free(&dbp);
            atlas_buf_free(&dir);
            if (mst != ATLAS_OK) {
                return mst;
            }
        }
        /* `prune` is not guarded by operator authority either: the rows it
         * deletes are in a database the calling uid can already open and
         * delete from. See atlas/authority.h for why a check that an adversary
         * walks around is worse than none. */
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas maintenance %s [--older-than "
                                                       "DAYS] [--retain N]%s",
                                 sub, prune ? " --apply" : "");
        }
        if (prune && !st->opts.apply) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "refusing to prune without --apply; `atlas maintenance plan` "
                                 "reports what would be removed and writes nothing");
        }
        if (!prune && st->opts.apply) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "`maintenance plan` never deletes; use `maintenance prune "
                                 "--apply`");
        }
        atlas_maintenance_opts mo;
        memset(&mo, 0, sizeof mo);
        mo.older_than_days = st->opts.older_than_days;
        mo.retain_per_repo = st->opts.retain;
        mo.apply = prune;
        atlas_maintenance_report rep;
        atlas_maintenance_report_init(&rep);
        atlas_status ms = maint_remote ? atlas_service_maintenance_remote(&mo, &rep, err)
                                       : atlas_service_maintenance(st->opts.data_dir, &mo, &rep,
                                                                   err);
        if (ms == ATLAS_OK) {
            ms = renderer_open(&r, st->opts.json, st->out,
                               prune ? "maintenance prune" : "maintenance plan", err);
            if (ms == ATLAS_OK) {
                ms = r.v->maintenance(&r, &rep, err);
            }
            ms = ms == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), ms);
        }
        atlas_maintenance_report_free(&rep);
        return ms;
    }

    /* **A7.1: an index this process does not own is reached over the socket, or
     * not at all.**
     *
     * Decided here, before any context is opened, for the reason the `job` and
     * `maintenance` commands are dispatched here: opening a context would try
     * to prepare a directory this process has no business preparing, and the
     * first thing the user would see is a chmod failure rather than an answer.
     * A served command then runs with `ctx == NULL` and takes the remote branch
     * at its own call site.
     *
     * Deliberately keyed on the data directory's *source* plus its ownership
     * rather than on the path: an explicit `--data-dir` or `ATLAS_DATA_DIR`
     * still means exactly what it says, so fixtures, tests and per-user daemons
     * behave as they always did on a machine that carries a system policy. */
    bool remote = index_is_foreign(st);
    if (remote && !is_a_command(cmd)) {
        /* The same answer a per-user install gives, produced here because the
         * dispatcher's own unknown-command error comes after a context is
         * opened and a foreign index has no context to open. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown command \"%s\" (try: atlas help)",
                             cmd);
    }
    if (remote && !remote_serves(st)) {
        return remote_refuse(st, err);
    }
    /* `doctor` is the one served command that still opens a context: INSPECT
     * creates nothing, takes no lock, and an index it cannot read is a finding
     * rather than a failure. */
    if (remote && strcmp(cmd, "doctor") == 0) {
        remote = false;
    }

    atlas_ctx_opts copts;
    memset(&copts, 0, sizeof(copts));
    copts.data_dir_override = st->opts.data_dir;
    copts.mode = mode_for(st);
    atlas_ctx *ctx = NULL;
    if (!remote) {
        s = atlas_ctx_open(&copts, &ctx, err);
        if (s != ATLAS_OK) {
            return s;
        }
    }

    int64_t limit = st->opts.limit > 0 ? st->opts.limit : ATLAS_DEFAULT_LIMIT;

    if (strcmp(cmd, "doctor") == 0) {
        result = need_operands(st, 0, "doctor", err);
        if (result == ATLAS_OK) {
            atlas_doctor_report rep;
            atlas_doctor_report_init(&rep);
            result = atlas_service_doctor(ctx, &rep, err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "doctor", err);
                if (result == ATLAS_OK) {
                    result = r.v->doctor(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            }
            atlas_doctor_report_free(&rep);
        }
    } else if (strcmp(cmd, "repo") == 0) {
        if (st->operand_count == 0) {
            result = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "usage: atlas repo add|list|remove ...");
        /* A7 removed every model-reachable route into the registry — there is
         * no `repo.add`, `repo.ensure` or `repo.remove` RPC method, no MCP
         * tool, and no hook that registers. What is left is this local command,
         * and it is not additionally guarded by operator authority: the
         * registry is a table in a database the calling uid can already write.
         * The boundary that matters was the socket, and it is closed. */
        } else if (strcmp(st->operands[0], "add") == 0) {
            if (st->operand_count != 2u) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE,
                                       "usage: atlas repo add PATH [--name NAME]");
            } else {
                atlas_repo_info info;
                atlas_repo_info_init(&info);
                result = atlas_service_repo_add(ctx, st->operands[1], repo_arg_name, &info, err);
                if (result == ATLAS_OK) {
                    result = renderer_open(&r, st->opts.json, st->out, "repo add", err);
                    if (result == ATLAS_OK) {
                        result = r.v->repo_added(&r, &info, err);
                    }
                    if (result == ATLAS_OK) {
                        result = renderer_close(&r, err);
                    } else {
                        renderer_abort(&r);
                    }
                }
                atlas_repo_info_free(&info);
            }
        } else if (strcmp(st->operands[0], "list") == 0) {
            if (st->operand_count != 1u) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas repo list");
            } else {
                result = renderer_open(&r, st->opts.json, st->out, "repo list", err);
                if (result == ATLAS_OK) {
                    list_sink ls = {&r};
                    int64_t count = 0;
                    result = r.v->list_begin(&r, "repositories", err);
                    if (result == ATLAS_OK) {
                        result = ctx != NULL
                                     ? atlas_service_repo_list(ctx, repo_item_sink, &ls, &count,
                                                               err)
                                     : atlas_service_repo_list_remote(repo_item_sink, &ls, &count,
                                                                      err);
                    }
                    if (result == ATLAS_OK) {
                        result = r.v->list_end(&r, "repository", "repositories", count, err);
                    }
                    if (result == ATLAS_OK) {
                        result = renderer_close(&r, err);
                    } else {
                        renderer_abort(&r);
                    }
                }
            }
        } else if (strcmp(st->operands[0], "remove") == 0) {
            if (st->operand_count != 2u) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas repo remove NAME --yes");
            } else if (!st->opts.yes) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE,
                                       "refusing to remove \"%s\" without --yes (this deletes "
                                       "Atlas metadata only; the repository is never touched)",
                                       st->operands[1]);
            } else {
                atlas_repo_info info;
                atlas_repo_info_init(&info);
                result = atlas_service_repo_remove(ctx, st->operands[1], &info, err);
                if (result == ATLAS_OK) {
                    result = renderer_open(&r, st->opts.json, st->out, "repo remove", err);
                    if (result == ATLAS_OK) {
                        result = r.v->repo_removed(&r, &info, err);
                    }
                    if (result == ATLAS_OK) {
                        result = renderer_close(&r, err);
                    } else {
                        renderer_abort(&r);
                    }
                }
                atlas_repo_info_free(&info);
            }
        } else {
            result = atlas_err_set(err, ATLAS_ERR_USAGE, "unknown repo subcommand \"%s\"",
                                   st->operands[0]);
        }
    } else if (strcmp(cmd, "scan") == 0) {
        result = need_operands(st, 1, "scan NAME", err);
        if (result == ATLAS_OK) {
            atlas_scan_opts so;
            scan_opts_from_cli(st, &so);
            atlas_scan_summary sum;
            result = atlas_service_scan(ctx, st->operands[0], &so, &sum, err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "scan", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->scan(&r, st->operands[0], &sum, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            }
        }
    } else if (strcmp(cmd, "status") == 0) {
        result = need_operands(st, 1, "status NAME", err);
        if (result == ATLAS_OK) {
            atlas_status_report rep;
            atlas_status_report_init(&rep);
            result = ctx != NULL ? atlas_service_status(ctx, st->operands[0], &rep, err)
                                 : atlas_service_status_remote(st->operands[0], &rep, err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "status", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->status(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            }
            atlas_status_report_free(&rep);
        }
    } else if (strcmp(cmd, "search") == 0) {
        result = need_operands(st, 2, "search NAME QUERY", err);
        if (result == ATLAS_OK) {
            /* The search mode is needed before any result is printed, so it is
             * resolved from the database capabilities first.
             *
             * There is no database here when the index belongs to the daemon,
             * and no way to guess: claiming FTS5 against an index that has none
             * would put a wrong mode in the header of a correct result. The
             * remote path therefore performs the call first and takes the mode
             * from the answer, buffering the hits; the ordering differs, the
             * rendering does not. */
            atlas_search_mode mode = ATLAS_SEARCH_DEGRADED_LIKE;
            if (ctx != NULL) {
                mode = atlas_db_caps_of(atlas_ctx_db(ctx))->fts5 ? ATLAS_SEARCH_FTS5
                                                                 : ATLAS_SEARCH_DEGRADED_LIKE;
            } else {
                result = atlas_service_search_remote(st->operands[0], st->operands[1], limit, &mode,
                                                     NULL, NULL, &(int64_t){0}, err);
            }
            result = renderer_open(&r, st->opts.json, st->out, "search", err);
            if (result == ATLAS_OK) {
                result = r.v->note_repo(&r, st->operands[0], err);
            }
            if (result == ATLAS_OK) {
                result = r.v->note_query(&r, st->operands[1], mode, err);
            }
            if (result == ATLAS_OK) {
                list_sink ls = {&r};
                int64_t count = 0;
                result = r.v->list_begin(&r, "results", err);
                if (result == ATLAS_OK) {
                    result = ctx != NULL
                                 ? atlas_service_search(ctx, st->operands[0], st->operands[1],
                                                        limit, &mode, search_item_sink, &ls,
                                                        &count, err)
                                 : atlas_service_search_remote(st->operands[0], st->operands[1],
                                                               limit, &mode, search_item_sink, &ls,
                                                               &count, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->list_end(&r, "result", "results", count, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            } else {
                renderer_abort(&r);
            }
        }
    } else if (strcmp(cmd, "file") == 0) {
        result = need_operands(st, 2, "file NAME PATH", err);
        if (result == ATLAS_OK) {
            file_sink fs = {&r, st, st->operands[0], false};
            result = ctx != NULL ? atlas_service_file(ctx, st->operands[0], st->operands[1],
                                                      file_report_sink, &fs, err)
                                 : atlas_service_file_remote(st->operands[0], st->operands[1],
                                                             file_report_sink, &fs, err);
            if (result == ATLAS_OK && !fs.opened) {
                /* The path resolved but produced nothing. Still one complete
                 * document, so a script sees an answer rather than silence. */
                result = renderer_open(&r, st->opts.json, st->out, "file", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                fs.opened = result == ATLAS_OK;
            }
            if (fs.opened) {
                result = result == ATLAS_OK ? renderer_close(&r, err) : (renderer_abort(&r), result);
            }
        }
    } else if (strcmp(cmd, "history") == 0) {
        result = need_operands(st, 2, "history NAME PATH", err);
        if (result == ATLAS_OK) {
            result = renderer_open(&r, st->opts.json, st->out, "history", err);
            if (result == ATLAS_OK) {
                result = r.v->note_repo(&r, st->operands[0], err);
            }
            if (result == ATLAS_OK) {
                list_sink ls = {&r};
                int64_t count = 0;
                result = r.v->list_begin(&r, "changes", err);
                if (result == ATLAS_OK) {
                    result = ctx != NULL
                                 ? atlas_service_history(ctx, st->operands[0], st->operands[1],
                                                         limit, history_item_sink, &ls, &count, err)
                                 : atlas_service_history_remote(st->operands[0], st->operands[1],
                                                                limit, history_item_sink, &ls,
                                                                &count, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->list_end(&r, "change", "changes", count, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            } else {
                renderer_abort(&r);
            }
        }
    } else if (strcmp(cmd, "diff") == 0) {
        result = need_operands(st, 1, "diff NAME", err);
        if (result == ATLAS_OK) {
            /* The header must be written before any entry, and the summary after
             * the last one, so the diff is gathered first and then rendered. */
            atlas_diff_opts dopts;
            atlas_diff_opts_init(&dopts);
            if (st->opts.limit > 0) {
                dopts.max_entries = st->opts.limit;
            }
            dopts.skip_untracked = st->opts.no_untracked;

            atlas_diff_report rep;
            atlas_diff_report_init(&rep);
            /* First pass gathers the report header and counts without rendering. */
            result = ctx != NULL
                         ? atlas_service_diff(ctx, st->operands[0], &dopts, NULL, NULL, &rep, err)
                         : atlas_service_diff_remote(st->operands[0], &dopts, NULL, NULL, &rep,
                                                     err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "diff", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->diff_begin(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    list_sink ls = {&r};
                    atlas_diff_report second;
                    atlas_diff_report_init(&second);
                    result = ctx != NULL
                                 ? atlas_service_diff(ctx, st->operands[0], &dopts, diff_item_sink,
                                                      &ls, &second, err)
                                 : atlas_service_diff_remote(st->operands[0], &dopts,
                                                             diff_item_sink, &ls, &second, err);
                    atlas_diff_report_free(&second);
                }
                if (result == ATLAS_OK) {
                    result = r.v->diff_end(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            }
            atlas_diff_report_free(&rep);
        }
    } else if (strcmp(cmd, "daemon") == 0) {
        if (st->operand_count != 1u || strcmp(st->operands[0], "status") != 0) {
            result = atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas daemon run|status|ping");
        } else {
            atlas_daemon_status_report rep;
            atlas_daemon_status_report_init(&rep);
            result = ctx != NULL ? atlas_service_daemon_status(ctx, &rep, err)
                                 : atlas_service_daemon_status_remote(&rep, err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "daemon status", err);
                if (result == ATLAS_OK) {
                    result = r.v->daemon_status(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            }
            atlas_daemon_status_report_free(&rep);
        }
    } else if (strcmp(cmd, "sync") == 0) {
        result = need_operands(st, 1, "sync NAME [--wait] [--full]", err);
        if (result == ATLAS_OK) {
            atlas_sync_report rep;
            atlas_sync_report_init(&rep);
            result = ctx != NULL
                         ? atlas_service_sync(ctx, st->operands[0], st->opts.full, st->opts.wait,
                                              st->opts.timeout_ms, &rep, err)
                         : atlas_service_sync_remote(st->operands[0], st->opts.full, st->opts.wait,
                                                     st->opts.timeout_ms, &rep, err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "sync", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->sync(&r, st->operands[0], &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            }
            atlas_sync_report_free(&rep);
        }
    } else if (strcmp(cmd, "events") == 0) {
        result = need_operands(st, 1, "events NAME [--since CURSOR] [--limit N]", err);
        if (result == ATLAS_OK) {
            /* The per-repository state is printed above the journal, so a caller
             * can see whether the events they are reading describe a current
             * index or one with a known hole in it. */
            atlas_repo_state_report state;
            atlas_repo_state_report_init(&state);
            result = ctx != NULL ? atlas_service_repo_state(ctx, st->operands[0], &state, err)
                                 : atlas_service_repo_state_remote(st->operands[0], &state, err);
            if (result == ATLAS_OK) {
                result = renderer_open(&r, st->opts.json, st->out, "events", err);
            }
            if (result == ATLAS_OK) {
                result = r.v->note_repo(&r, st->operands[0], err);
            }
            if (result == ATLAS_OK) {
                result = r.v->repo_state(&r, &state, err);
            }
            if (result == ATLAS_OK) {
                list_sink ls = {&r};
                int64_t count = 0;
                int64_t next = st->opts.since;
                bool more = false;
                result = r.v->list_begin(&r, "events", err);
                if (result == ATLAS_OK) {
                    result = ctx != NULL
                                 ? atlas_service_events(ctx, st->operands[0], st->opts.since, limit,
                                                        event_item_sink, &ls, &count, &next, &more,
                                                        err)
                                 : atlas_service_events_remote(st->operands[0], st->opts.since,
                                                               limit, event_item_sink, &ls, &count,
                                                               &next, &more, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->list_end(&r, "event", "events", count, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->events_end(&r, next, more, err);
                }
                if (result == ATLAS_OK) {
                    result = renderer_close(&r, err);
                } else {
                    renderer_abort(&r);
                }
            } else {
                renderer_abort(&r);
            }
            atlas_repo_state_report_free(&state);
        }
    } else if (strcmp(cmd, "code") == 0) {
        result = run_code(st, ctx, &r, limit, err);
    } else if (strcmp(cmd, "context") == 0) {
        result = run_context(st, ctx, &r, err);
    } else if (strcmp(cmd, "gate") == 0) {
        result = run_gate(st, ctx, &r, err);
    } else if (strcmp(cmd, "verify") == 0) {
        result = run_verify(st, ctx, &r, err);
    } else if (strcmp(cmd, "decision") == 0) {
        result = run_decision(st, ctx, &r, limit, err);
    } else {
        result = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "unknown command \"%s\" (try: atlas help)", cmd);
    }

    atlas_ctx_close(ctx);
    return result;
}

/* --- A8-CI: the bounded translation-unit parser ----------------------------
 *
 * `atlas sem-parse` is the child half of the semantic indexer, and it is
 * dispatched from raw argv before any option is parsed and before any
 * `atlas_ctx` is opened. Both placements are guarantees rather than
 * conveniences:
 *
 *   - **It never touches the index.** No context, no database handle, no writer
 *     lock. A context in AUTO mode takes the lock when it is free, and a parser
 *     child that took the writer lock would deadlock the pass that spawned it.
 *     The absence of a code path prevents that, not care at the call site —
 *     A5's argument for dispatching backup before any context exists.
 *   - **It is the process that feeds untrusted repository source to a compiler
 *     front end.** Keeping it a separate process with an address-space rlimit
 *     and a wall clock is what bounds it; keeping it out of the daemon is what
 *     stops a crash inside a compiler library from taking down the process that
 *     owns the index.
 *   - **Its arguments are a protocol, not an operator interface.** Everything
 *     after `--` is a compiler argument vector the compile-database reader
 *     already reduced to a positive allowlist, and those genuinely look like
 *     options. Running them through Atlas' own option parser would either
 *     reject them or, far worse, silently read one as an Atlas flag.
 *
 * It is absent from the help text and from the completion list because it is
 * not an operator command. That is not a security measure: running it by hand
 * does nothing an operator could not do with clang directly, and it can write
 * nothing at all. */
static int atlas_cli_sem_parse_child(int argc, char **argv, FILE *out, atlas_err *err) {
    const char *source = NULL;
    const char *root = NULL;
    const char *directory = NULL;
    const char *args[ATLAS_CODE_MAX_COMPILE_ARGS];
    size_t argn = 0;
    bool rest = false;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (rest) {
            if (argn < ATLAS_CODE_MAX_COMPILE_ARGS) {
                args[argn++] = a;
            }
            continue;
        }
        if (strcmp(a, "--") == 0) {
            rest = true;
        } else if (strcmp(a, "--source") == 0 && i + 1 < argc) {
            source = argv[++i];
        } else if (strcmp(a, "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(a, "--directory") == 0 && i + 1 < argc) {
            directory = argv[++i];
        } else {
            return (int)atlas_err_set(err, ATLAS_ERR_USAGE,
                                      "atlas sem-parse: unexpected argument before \"--\"");
        }
    }
    if (source == NULL || root == NULL) {
        return (int)atlas_err_set(err, ATLAS_ERR_USAGE,
                                  "usage: atlas sem-parse --source PATH --root PATH [-- ARGS...]");
    }

    atlas_sem_parse_req req;
    memset(&req, 0, sizeof(req));
    req.source = source;
    req.root = root;
    req.directory = directory;
    req.args = args;
    req.arg_count = argn;

    atlas_buf doc = ATLAS_BUF_INIT;
    atlas_sem_parse_result res;
    atlas_status st = atlas_sem_parse_here(&req, &doc, &res, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&doc);
        return (int)st;
    }
    /* One JSON document on stdout and nothing else. The parent reads it through
     * the one yyjson facade and checks every field against Atlas' own closed
     * vocabularies, so a child that produced nonsense becomes a failed unit
     * rather than trusted input — A8's rule that a zero exit is not a success
     * claim. */
    (void)fwrite(doc.data, 1, doc.len, out);
    atlas_buf_free(&doc);
    return 0;
}

int atlas_cli_main(int argc, char **argv, FILE *out, FILE *errout) {
    cli_state st;
    memset(&st, 0, sizeof(st));
    st.out = out;
    st.errout = errout;

    atlas_err err;
    atlas_err_init(&err);

    /* A8-CI: the child parser, dispatched from raw argv before options are
     * parsed at all.
     *
     * Its arguments are Atlas' protocol with its own child, not an operator
     * interface: everything after `--` is a compiler argument vector that the
     * compile-database reader already reduced to an allowlist, and those
     * genuinely look like options (`-I`, `-D`, `-std=`). Running them through
     * the CLI's own option parser would either reject them or, far worse,
     * silently interpret one as an Atlas flag. Keeping the two argument
     * languages apart is the point.
     *
     * It opens no context, takes no lock and touches no database — see the
     * handler for why that placement is the guarantee rather than a
     * convenience. */
    if (argc > 1 && strcmp(argv[1], "sem-parse") == 0) {
        int rc = atlas_cli_sem_parse_child(argc, argv, out, &err);
        if (rc != 0) {
            atlas_render_error(out, errout, false, "sem-parse", &err);
        }
        return rc;
    }

    bool want_help = false;
    bool want_version = false;
    atlas_status s = parse_args(&st, argc, argv, &want_help, &want_version, &err);
    if (s != ATLAS_OK) {
        atlas_render_error(out, errout, st.opts.json, st.command, &err);
        return (int)s;
    }

    if (want_help || (st.command != NULL && strcmp(st.command, "help") == 0)) {
        atlas_cli_print_help(out);
        return (int)ATLAS_OK;
    }
    if (want_version) {
        atlas_cli_print_version(out, st.opts.json);
        return (int)ATLAS_OK;
    }
    if (st.command == NULL) {
        atlas_cli_print_help(errout);
        return (int)ATLAS_ERR_USAGE;
    }

    /* A7.1. Declare which index this invocation is about, before anything
     * resolves a socket.
     *
     * A socket belongs to an index. With a system policy in force the shared
     * daemon serves `/var/lib/atlas` on its own socket, but a command pointed
     * explicitly somewhere else — an offline lifecycle operation, or the test
     * suite isolating itself — is talking about a different index and must
     * reach that index's own endpoint. Without this, every `--data-dir`
     * invocation on a deployed machine would address the system socket and
     * either be refused or, far worse, answer about the wrong database.
     *
     * `NULL` when no `--data-dir` was given, which is the ordinary case and
     * leaves the policy in charge. */
    atlas_ipc_socket_scope_set(st.opts.data_dir);

    s = run_command(&st, &err);
    if (s != ATLAS_OK && !st.rendered) {
        atlas_render_error(out, errout, st.opts.json, st.command, &err);
        return (int)s;
    }
    /* A gate result that is not PASS exits non-zero *after* a complete,
     * successful document has been written. It is not an error and no error
     * document is emitted; `atlas gate check --json` still puts exactly one
     * document on stdout, which is the same contract `atlas daemon ping`
     * follows for the same reason. */
    if (s == ATLAS_OK && st.gate_exit != 0) {
        return st.gate_exit;
    }
    return (int)s;
}

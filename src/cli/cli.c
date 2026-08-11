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

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/backup.h"
#include "atlas/daemon.h"
#include "atlas/hook.h"
#include "atlas/integrate.h"
#include "atlas/ipc.h"
#include "atlas/maintenance.h"
#include "atlas/mcp.h"
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
        "  decision list NAME         recorded decisions and their lifecycle status\n"
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
        "  decision orphaned          decisions attached to no registered repository\n"
        "  decision legacy NAME       A2 decision proposals, and which were promoted\n"
        "  decision promote NAME ID   make an A4 document from an A2 proposal\n"
        "  backup create OUTPUT       online snapshot of the index; refuses to overwrite\n"
        "  backup verify BACKUP       check one; creates nothing and repairs nothing\n"
        "  backup restore BACKUP --yes  replace the index; keeps what it displaced\n"
        "  maintenance plan           what a prune would remove, and why each table is kept\n"
        "  maintenance prune --apply  remove only the rows the plan called eligible\n"
        "  service print              print the systemd user unit; changes nothing\n"
        "  service install --user     write the unit; never enables or starts it\n"
        "  service uninstall --user   remove the unit Atlas wrote\n"
        "  mcp                        serve the Model Context Protocol on stdio\n"
        "  hook EVENT                 handle one Claude Code hook event on stdin\n"
        "  integrate claude print     print the one-time setup commands; runs none of them\n"
        "  integrate claude doctor    check the AI integration end to end\n"
        "  integrate claude install --user    record where this Atlas is, for the plugin\n"
        "  integrate claude uninstall --user  remove that record; never the index\n"
        ,
        ATLAS_VERSION_STRING, ATLAS_PHASE);
    /* A third fprintf for the same reason as the second: A6 pushed the command
     * list past the guaranteed literal length again. */
    (void)fprintf(
        out,
        "  decision revalidate NAME ID  record that an approved decision was checked\n"
        "                             against the current indexed state; needs a terminal\n"
        "  gate check NAME            assess every approved decision against the indexed\n"
        "                             state; exits 8 on review required, 9 on blocked\n"
        "  gate show NAME ID          the same assessment, for one decision\n"
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
        "  --path P --commit OID --symbol-link S\n"
        "                             decision propose/revise: repeatable links\n"
        "  --status S                 decision list: PROPOSED|APPROVED|REJECTED|SUPERSEDED\n"
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
            } else if (strcmp(a, "--repo") == 0 || strcmp(a, "--task") == 0 ||
                       strcmp(a, "--mode") == 0 || strcmp(a, "--driver") == 0 ||
                       strcmp(a, "--idempotency-key") == 0 || strcmp(a, "--wall-timeout-ms") == 0 ||
                       strcmp(a, "--idle-timeout-ms") == 0 || strcmp(a, "--attempts") == 0) {
                /* A8 job options, grouped for the same reason the A4 ones are:
                 * the "a flag at the end of the line has no value" check lives
                 * once rather than eight times. */
                if (i + 1 >= argc) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE, "%s needs a value", a);
                }
                const char *v = argv[++i];
                if (strcmp(a, "--repo") == 0) {
                    st->opts.job.repo = v;
                } else if (strcmp(a, "--task") == 0) {
                    st->opts.job.task = v;
                } else if (strcmp(a, "--mode") == 0) {
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
                       strcmp(a, "--at") == 0) {
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
                } else {
                    st->opts.decision.dedup_key = v;
                }
            } else if (strcmp(a, "--alternative") == 0 || strcmp(a, "--path") == 0 ||
                       strcmp(a, "--commit") == 0 || strcmp(a, "--symbol-link") == 0) {
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
                } else {
                    if (st->opts.decision.symbol_count >= ATLAS_DECISION_MAX_LINKS) {
                        return atlas_err_set(err, ATLAS_ERR_USAGE,
                                             "at most %d --symbol-link options",
                                             ATLAS_DECISION_MAX_LINKS);
                    }
                    st->opts.decision.symbols[st->opts.decision.symbol_count++] = v;
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

static atlas_status file_report_sink(const atlas_file_report *rep, void *ud, atlas_err *err) {
    list_sink *ls = (list_sink *)ud;
    return ls->r->v->file(ls->r, rep, err);
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
    in->alternatives = st->opts.decision.alternatives;
    in->alternative_count = st->opts.decision.alternative_count;
    in->paths = st->opts.decision.paths;
    in->path_count = st->opts.decision.path_count;
    in->commits = st->opts.decision.commits;
    in->commit_count = st->opts.decision.commit_count;
    in->symbols = st->opts.decision.symbols;
    in->symbol_count = st->opts.decision.symbol_count;
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

/* The three operator-only verbs. One function: they differ by intent and by
 * whether a replacement is required, and three copies of the `--yes` refusal
 * would be three chances for one of them to be missing. */
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
                             "--yes cannot approve, reject or supersede a decision. This command "
                             "needs an interactive terminal, and Atlas will not accept a "
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
                             "usage: atlas decision list|show|search|history|for-file|propose|"
                             "revise|approve|reject|supersede|revalidate|export|orphaned|legacy|"
                             "promote ...");
    }
    const char *sub = st->operands[0];
    atlas_status result;

    if (strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0 || strcmp(sub, "for-file") == 0) {
        size_t want = strcmp(sub, "list") == 0 ? 2u : 3u;
        if (st->operand_count != want) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 strcmp(sub, "list") == 0
                                     ? "usage: atlas decision list NAME [--status STATUS]"
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
                                     "--status is PROPOSED, APPROVED, REJECTED or SUPERSEDED");
            }
            opts.mode = ATLAS_DECISION_LIST_STATUS;
            opts.status = st->opts.decision.status;
        }
        result = renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        atlas_decision_counts counts;
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
        return strcmp(sub, "list") == 0;
    }
    if (strcmp(cmd, "daemon") == 0) {
        return strcmp(sub, "status") == 0;
    }
    if (strcmp(cmd, "code") == 0) {
        return strcmp(sub, "status") == 0 || strcmp(sub, "file") == 0 ||
               strcmp(sub, "symbol") == 0 || strcmp(sub, "search") == 0 ||
               strcmp(sub, "deps") == 0 || strcmp(sub, "impact") == 0;
    }
    if (strcmp(cmd, "gate") == 0) {
        return strcmp(sub, "check") == 0 || strcmp(sub, "show") == 0;
    }
    if (strcmp(cmd, "decision") == 0) {
        return strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0 ||
               strcmp(sub, "for-file") == 0 || strcmp(sub, "show") == 0 ||
               strcmp(sub, "export") == 0 || strcmp(sub, "history") == 0 ||
               strcmp(sub, "orphaned") == 0 || strcmp(sub, "legacy") == 0;
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
        "integrate", "version", "help",
    };
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strcmp(cmd, COMMANDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static atlas_status remote_refuse(const cli_state *st, atlas_err *err) {
    /* A write against somebody else's index is not a permissions problem to be
     * reported from inside a chmod; it is a thing this account does not do.
     * Registration and scanning under a system deployment are the operator
     * ceremony in docs/security/A7_1_OPERATIONS.md. */
    if (mode_for(st) == ATLAS_CTX_WRITE) {
        return atlas_err_set(err, ATLAS_ERR_CONFIG,
                             "the system index is owned by the Atlas service account and this "
                             "command would write to it. Registration and scanning are operator "
                             "operations performed as the service account; see "
                             "docs/security/A7_1_OPERATIONS.md.");
    }
    return atlas_err_set(err, ATLAS_ERR_CONFIG,
                         "the system index is owned by the Atlas service account and cannot be "
                         "read directly, and `atlas %s%s%s` has no daemon-served form yet. "
                         "Every other read-only command does; `backup` and `maintenance` are "
                         "local operator operations with no RPC surface by design.",
                         st->command, st->operand_count > 0 ? " " : "",
                         st->operand_count > 0 ? st->operands[0] : "");
}

static atlas_status run_code(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
                             atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas code status|sync|file|symbol|search|deps|impact ...");
    }
    const char *sub = st->operands[0];
    atlas_status result;

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
            return atlas_err_set(err, ATLAS_ERR_CONFIG,
                                 "changing the repository registry is a local operation and the "
                                 "Atlas daemon currently owns this index. Stop it first:\n"
                                 "    systemctl --user stop atlas\n"
                                 "    atlas repo %s ...\n"
                                 "    systemctl --user start atlas\n"
                                 "Atlas exposes no RPC method for this, so that nothing reachable "
                                 "over the socket — including MCP and hooks — can decide what "
                                 "Atlas indexes.",
                                 st->operands[0]);
        }
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
                                     "usage: atlas backup create OUTPUT [--force]");
            }
            atlas_backup_create_opts bo;
            memset(&bo, 0, sizeof bo);
            bo.output = st->operands[1];
            bo.force = st->opts.force;
            atlas_backup_report rep;
            atlas_backup_report_init(&rep);
            atlas_status bs = atlas_service_backup_create(st->opts.data_dir, &bo, &rep, err);
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
            atlas_status bs = atlas_service_backup_verify(st->operands[1], &rep, err);
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
        atlas_status ms = atlas_service_maintenance(st->opts.data_dir, &mo, &rep, err);
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
    bool remote = false;
    {
        atlas_buf resolved = ATLAS_BUF_INIT;
        atlas_datadir_source src = ATLAS_DATADIR_OVERRIDE;
        atlas_err rerr;
        atlas_err_init(&rerr);
        if (atlas_datadir_resolve(st->opts.data_dir, &resolved, &src, &rerr) == ATLAS_OK) {
            remote = atlas_datadir_is_foreign(atlas_buf_cstr(&resolved), src);
        }
        atlas_buf_free(&resolved);
    }
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
            result = renderer_open(&r, st->opts.json, st->out, "file", err);
            if (result == ATLAS_OK) {
                result = r.v->note_repo(&r, st->operands[0], err);
            }
            if (result == ATLAS_OK) {
                list_sink ls = {&r};
                result = ctx != NULL ? atlas_service_file(ctx, st->operands[0], st->operands[1],
                                                          file_report_sink, &ls, err)
                                     : atlas_service_file_remote(st->operands[0], st->operands[1],
                                                                 file_report_sink, &ls, err);
            }
            if (result == ATLAS_OK) {
                result = renderer_close(&r, err);
            } else {
                renderer_abort(&r);
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
    } else if (strcmp(cmd, "gate") == 0) {
        result = run_gate(st, ctx, &r, err);
    } else if (strcmp(cmd, "decision") == 0) {
        result = run_decision(st, ctx, &r, limit, err);
    } else {
        result = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "unknown command \"%s\" (try: atlas help)", cmd);
    }

    atlas_ctx_close(ctx);
    return result;
}

int atlas_cli_main(int argc, char **argv, FILE *out, FILE *errout) {
    cli_state st;
    memset(&st, 0, sizeof(st));
    st.out = out;
    st.errout = errout;

    atlas_err err;
    atlas_err_init(&err);

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

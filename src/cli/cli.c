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
#include "atlas/daemon.h"
#include "atlas/hook.h"
#include "atlas/integrate.h"
#include "atlas/ipc.h"
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
} cli_state;

void atlas_cli_print_help(FILE *out) {
    (void)fprintf(
        out,
        "atlas %s - engineering memory and repository intelligence (phase %s)\n"
        "\n"
        "usage: atlas [OPTIONS] COMMAND [OPTIONS] [ARGS]\n"
        "\n"
        "commands:\n"
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
        "  service print              print the systemd user unit; changes nothing\n"
        "  service install --user     write the unit; never enables or starts it\n"
        "  service uninstall --user   remove the unit Atlas wrote\n"
        "  mcp                        serve the Model Context Protocol on stdio\n"
        "  hook EVENT                 handle one Claude Code hook event on stdin\n"
        "  integrate claude print     print the one-time setup commands; runs none of them\n"
        "  integrate claude doctor    check the AI integration end to end\n"
        "  integrate claude install --user    record where this Atlas is, for the plugin\n"
        "  integrate claude uninstall --user  remove that record; never the index\n"
        "  version                    print the version\n"
        "  help                       print this help\n"
        "\n"
        "options (accepted before or after the command; '--' ends option parsing):\n"
        "  --json                     emit stable JSON on stdout\n"
        "  --wait                     sync: wait for the reconciliation to complete\n"
        "  --full                     sync: re-read every file rather than only changes\n"
        "  --since CURSOR             events: start after this cursor\n"
        "  --user                     service: operate on the systemd *user* unit\n"
        "  --force                    service: replace or remove a unit Atlas did not write\n"
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
        ATLAS_VERSION_STRING, ATLAS_PHASE, ATLAS_DEFAULT_LIMIT);
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
            } else if (strcmp(a, "--user") == 0) {
                st->opts.user = true;
            } else if (strcmp(a, "--force") == 0) {
                st->opts.force = true;
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
    return atlas_ipc_daemon_reachable();
}

/* Sends one mutation to the daemon and renders its answer. */
static atlas_status call_daemon_mutation(cli_state *st, const char *method, const char *params,
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
             * than pretending this process did the work. */
            result = r.v->note_repo(&r, "", err);
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
                vst = call_daemon_mutation(st, "repo.sync", atlas_buf_cstr(&params), "scan", err);
            }
            atlas_buf_free(&params);
            return vst;
        }
        if (strcmp(st->operands[0], "add") == 0) {
            if (st->operand_count != 2u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "usage: atlas repo add PATH [--name NAME]");
            }
            /* The path is user input and can contain anything, so it is not
             * spliced into a JSON document by hand: it is refused here and the
             * offline path (which takes it as an argv operand, not as JSON) is
             * used instead. */
            for (const unsigned char *p = (const unsigned char *)st->operands[1]; *p != '\0'; p++) {
                if (*p < 0x20u || *p == '"' || *p == '\\' || *p == 0x7fu) {
                    return atlas_err_set(err, ATLAS_ERR_USAGE,
                                         "this path contains a byte Atlas will not send over IPC "
                                         "(0x%02x). Stop the daemon and register it offline.",
                                         (unsigned)*p);
                }
            }
            atlas_buf params = ATLAS_BUF_INIT;
            atlas_status vst = atlas_buf_appendf(&params, err, "{\"path\":\"%s\"",
                                                 st->operands[1]);
            if (vst == ATLAS_OK && repo_arg_name != NULL) {
                vst = atlas_db_check_repo_name(repo_arg_name, err);
                if (vst == ATLAS_OK) {
                    vst = atlas_buf_appendf(&params, err, ",\"name\":\"%s\"", repo_arg_name);
                }
            }
            if (vst == ATLAS_OK) {
                vst = atlas_buf_append_ch(&params, '}', err);
            }
            if (vst == ATLAS_OK) {
                vst = call_daemon_mutation(st, "repo.add", atlas_buf_cstr(&params), "repo add",
                                           err);
            }
            atlas_buf_free(&params);
            return vst;
        }
        if (strcmp(st->operands[0], "remove") == 0) {
            if (st->operand_count != 2u) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas repo remove NAME --yes");
            }
            if (!st->opts.yes) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "refusing to remove \"%s\" without --yes (this deletes "
                                     "Atlas metadata only; the repository is never touched)",
                                     st->operands[1]);
            }
            atlas_buf params = ATLAS_BUF_INIT;
            atlas_status vst = atlas_db_check_repo_name(st->operands[1], err);
            if (vst == ATLAS_OK) {
                vst = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\"}", st->operands[1]);
            }
            if (vst == ATLAS_OK) {
                vst = call_daemon_mutation(st, "repo.remove", atlas_buf_cstr(&params),
                                           "repo remove", err);
            }
            atlas_buf_free(&params);
            return vst;
        }
    }

    atlas_ctx_opts copts;
    memset(&copts, 0, sizeof(copts));
    copts.data_dir_override = st->opts.data_dir;
    copts.mode = mode_for(st);
    atlas_ctx *ctx = NULL;
    s = atlas_ctx_open(&copts, &ctx, err);
    if (s != ATLAS_OK) {
        return s;
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
                        result = atlas_service_repo_list(ctx, repo_item_sink, &ls, &count, err);
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
            result = atlas_service_status(ctx, st->operands[0], &rep, err);
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
             * resolved from the database capabilities first. */
            atlas_search_mode mode = atlas_db_caps_of(atlas_ctx_db(ctx))->fts5
                                         ? ATLAS_SEARCH_FTS5
                                         : ATLAS_SEARCH_DEGRADED_LIKE;
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
                    result = atlas_service_search(ctx, st->operands[0], st->operands[1], limit,
                                                  &mode, search_item_sink, &ls, &count, err);
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
                result = atlas_service_file(ctx, st->operands[0], st->operands[1],
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
                    result = atlas_service_history(ctx, st->operands[0], st->operands[1], limit,
                                                   history_item_sink, &ls, &count, err);
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
            result = atlas_service_diff(ctx, st->operands[0], &dopts, NULL, NULL, &rep, err);
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
                    result = atlas_service_diff(ctx, st->operands[0], &dopts, diff_item_sink, &ls,
                                                &second, err);
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
            result = atlas_service_daemon_status(ctx, &rep, err);
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
            result = atlas_service_sync(ctx, st->operands[0], st->opts.full, st->opts.wait,
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
            result = atlas_service_repo_state(ctx, st->operands[0], &state, err);
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
                    result = atlas_service_events(ctx, st->operands[0], st->opts.since, limit,
                                                  event_item_sink, &ls, &count, &next, &more, err);
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

    s = run_command(&st, &err);
    if (s != ATLAS_OK && !st.rendered) {
        atlas_render_error(out, errout, st.opts.json, st.command, &err);
    }
    return (int)s;
}

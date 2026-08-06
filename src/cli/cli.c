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
#include "cli/render.h"

#define ATLAS_DEFAULT_LIMIT 50

typedef struct cli_state {
    atlas_cli_opts opts;
    const char *command;
    const char *operands[8];
    size_t operand_count;
    FILE *out;
    FILE *errout;
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
        "  version                    print the version\n"
        "  help                       print this help\n"
        "\n"
        "options (accepted before or after the command; '--' ends option parsing):\n"
        "  --json                     emit stable JSON on stdout\n"
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
        "A0 records facts only. It never infers why something changed: when a reason\n"
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

    atlas_ctx_opts copts;
    memset(&copts, 0, sizeof(copts));
    copts.data_dir_override = st->opts.data_dir;
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
    if (s != ATLAS_OK) {
        atlas_render_error(out, errout, st.opts.json, st.command, &err);
    }
    return (int)s;
}

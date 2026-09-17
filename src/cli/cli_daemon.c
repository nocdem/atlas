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

/* --- A1: commands that touch no database -------------------------------- */

/* `atlas daemon run`. Foreground, and it owns its own data directory and lock,
 * so it deliberately does not go through atlas_ctx_open: taking the lock twice
 * would deadlock against itself. */
atlas_status atlas_cli_run_daemon(cli_state *st, atlas_err *err) {
    atlas_daemon_opts dopts;
    atlas_daemon_opts_init(&dopts);
    dopts.data_dir_override = st->opts.data_dir;
    dopts.run_once = st->opts.run_once;
    /* Log lines go to stderr so that stdout stays clean for anything a future
     * caller might want to parse, and so journald captures them either way. */
    return atlas_daemon_run(&dopts, st->errout, err);
}

atlas_status atlas_cli_run_service(cli_state *st, atlas_err *err) {
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
            result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "service print", err);
            if (result == ATLAS_OK) {
                result = r.v->unit_text(&r, atlas_buf_cstr(&unit), err);
            }
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_close(&r, err);
            } else {
                atlas_cli_renderer_abort(&r);
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
        result = atlas_cli_renderer_open(&r, st->opts.json, st->out,
                               uninstall ? "service uninstall" : "service install", err);
        if (result == ATLAS_OK) {
            result = r.v->unit_install(&r, &rep, uninstall, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(&r, err);
        } else {
            atlas_cli_renderer_abort(&r);
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
atlas_status atlas_cli_run_integrate(cli_state *st, atlas_err *err) {
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
        result = atlas_cli_renderer_open(&r, st->opts.json, st->out, command, err);
        if (result == ATLAS_OK) {
            result = r.v->integrate(&r, &rep, sub, print ? atlas_buf_cstr(&commands) : NULL, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(&r, err);
        } else {
            atlas_cli_renderer_abort(&r);
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
atlas_status atlas_cli_run_daemon_ping(cli_state *st, atlas_err *err) {
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
    result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "daemon ping", err);
    if (result == ATLAS_OK) {
        result = r.v->daemon_ping(&r, reachable, atlas_buf_cstr(&sock), detail, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(&r, err);
    } else {
        atlas_cli_renderer_abort(&r);
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
atlas_ctx_mode atlas_cli_mode_for(const cli_state *st) {
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
        (strcmp(st->operands[0], "add") == 0 || strcmp(st->operands[0], "remove") == 0 ||
         strcmp(st->operands[0], "scanner") == 0)) {
        return ATLAS_CTX_WRITE;
    }
    return ATLAS_CTX_AUTO;
}

/* True when a mutation should be handed to the daemon rather than performed
 * here. Checked before the context is opened, because opening in WRITE mode
 * would fail against a running daemon — which is the correct behaviour for a
 * command that is *not* routed, and the wrong error for one that is. */
bool atlas_cli_route_to_daemon(const cli_state *st) {
    if (atlas_cli_mode_for(st) != ATLAS_CTX_WRITE) {
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
atlas_status atlas_cli_call_daemon_mutation(cli_state *st, const char *repo, const char *method,
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
        result = atlas_cli_renderer_open(&r, st->opts.json, st->out, command, err);
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
            result = atlas_cli_renderer_close(&r, err);
        } else {
            atlas_cli_renderer_abort(&r);
        }
    }
    atlas_buf_free(&sock);
    atlas_buf_free(&resp);
    return result;
}

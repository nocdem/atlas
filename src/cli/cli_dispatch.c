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

atlas_status atlas_cli_run_command(cli_state *st, atlas_err *err) {
    const char *cmd = st->command;
    atlas_renderer r;
    atlas_status result;

    /* Commands that need no database or repository first. */
    if (strcmp(cmd, "version") == 0) {
        atlas_status s = atlas_cli_need_operands(st, 0, "version", err);
        if (s != ATLAS_OK) {
            return s;
        }
        s = atlas_cli_renderer_open(&r, st->opts.json, st->out, "version", err);
        if (s == ATLAS_OK) {
            s = r.v->version(&r, err);
        }
        if (s == ATLAS_OK) {
            return atlas_cli_renderer_close(&r, err);
        }
        atlas_cli_renderer_abort(&r);
        return s;
    }

    const char *repo_arg_name = NULL;
    atlas_status s = atlas_cli_take_name(st, &repo_arg_name, err);
    if (s != ATLAS_OK) {
        return s;
    }

    bool repo_scanner_uid_given = false;
    int64_t repo_scanner_uid = 0;
    s = atlas_cli_take_scanner_uid(st, &repo_scanner_uid_given, &repo_scanner_uid, err);
    if (s != ATLAS_OK) {
        return s;
    }

    /* Commands that own their own lifecycle, before any context is opened. */
    if (strcmp(cmd, "daemon") == 0 && st->operand_count > 0 &&
        strcmp(st->operands[0], "run") == 0) {
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas daemon run");
        }
        return atlas_cli_run_daemon(st, err);
    }
    if (strcmp(cmd, "daemon") == 0 && st->operand_count > 0 &&
        strcmp(st->operands[0], "ping") == 0) {
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas daemon ping");
        }
        return atlas_cli_run_daemon_ping(st, err);
    }
    if (strcmp(cmd, "service") == 0) {
        return atlas_cli_run_service(st, err);
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
        return atlas_cli_run_integrate(st, err);
    }

    /* Mutations go through the daemon when one is running, so that the single
     * writer stays single. */
    if (atlas_cli_route_to_daemon(st)) {
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
                vst = atlas_cli_call_daemon_mutation(st, st->operands[0], "repo.sync",
                                           atlas_buf_cstr(&params), "scan", err);
            }
            atlas_buf_free(&params);
            return vst;
        }
        /* A13. `repo scanner` routes, because A7.1 gave it somewhere to route
         * to: `repo.scanner` is in the operator-uid group, selected by
         * `SO_PEERCRED` against the root-owned policy. See the comment on
         * `method_repo_scanner`. */
        if (strcmp(cmd, "repo") == 0 && st->operand_count == 2u &&
            strcmp(st->operands[0], "scanner") == 0) {
            atlas_buf params = ATLAS_BUF_INIT;
            atlas_status vst = atlas_db_check_repo_name(st->operands[1], err);
            if (vst == ATLAS_OK) {
                vst = atlas_buf_appendf(&params, err, "{\"repo\":\"%s\"", st->operands[1]);
            }
            if (vst == ATLAS_OK && repo_scanner_uid_given) {
                /* Sent only when the operator named one. Absent means "derive it
                 * from the root's owner", and a key carrying a default would
                 * make those two indistinguishable at the write point. */
                vst = atlas_buf_appendf(&params, err, ",\"scanner_uid\":\"%lld\"",
                                        (long long)repo_scanner_uid);
            }
            if (vst == ATLAS_OK) {
                vst = atlas_buf_appendf(&params, err, "}");
            }
            if (vst == ATLAS_OK) {
                vst = atlas_cli_call_daemon_mutation(st, st->operands[1], "repo.scanner",
                                           atlas_buf_cstr(&params), "repo", err);
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
        /* A13. `scanner` is no longer among these. `repo.scanner` is an operator-uid
         * RPC method, so a running daemon is the ordinary way to name a scanner
         * rather than the obstacle to it -- see the comment on `method_repo_scanner`
         * for why that reverses A7's refusal without weakening it. `add` and
         * `remove` are unchanged, and deliberately: they decide which directories
         * Atlas will read at all. */
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
        atlas_status ost = atlas_cli_need_operands(st, 2, "operation status ID", err);
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
            ost = atlas_cli_renderer_open(&orend, st->opts.json, st->out, "operation status", err);
            if (ost == ATLAS_OK) {
                ost = orend.v->operation_status(&orend, &rep, err);
            }
            ost = ost == ATLAS_OK ? atlas_cli_renderer_close(&orend, err)
                                  : (atlas_cli_renderer_abort(&orend), ost);
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
        return atlas_cli_run_job(st, &jr, limit, err);
    }

    /* A12.0. Dispatched here, beside `job`, for the same reason: everything a
     * plan command acts on arrives over the socket, and opening a context would
     * try to prepare a data directory this uid may not own. */
    if (strcmp(cmd, "plan") == 0) {
        atlas_renderer pr;
        memset(&pr, 0, sizeof(pr));
        int64_t limit = st->opts.limit > 0 ? st->opts.limit : ATLAS_DEFAULT_LIMIT;
        return atlas_cli_run_plan(st, &pr, limit, err);
    }

    /* A13. The scanner, dispatched here — before any `atlas_ctx` is opened —
     * for the reason `gateway run` is: it opens no index at all, and every
     * answer it gives comes over the daemon socket. A context in AUTO mode
     * would take the writer lock when it is free, which a scanner must never
     * do: the daemon owns the writer and the scanner is one of its clients. */
    if (strcmp(cmd, "scanner") == 0) {
        const char *sub = st->operand_count > 0 ? st->operands[0] : NULL;
        if (sub == NULL || strcmp(sub, "run") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas scanner run --once");
        }
        /* `--once` sets `opts.run_once`: it is matched by the first of two
         * branches for that flag in the parser's else-if chain, and the second
         * (`opts.job.once`) is unreachable. Verified by running the built
         * binary, which is the only way this kind of thing surfaces. */
        return atlas_service_scanner_run(st->opts.run_once, st->errout, err);
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
                                                "--scope S [--scope S...] | --no-scopes"
                                              : "usage: atlas api-key create --label L "
                                                "--scope S [--scope S...] | --no-scopes");
            }
            if (st->opts.label == NULL) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--label is required");
            }
            if (st->opts.scope_count > 0 && st->opts.no_scopes) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "--scope and --no-scopes cannot both be given");
            }
            if (st->opts.scope_count == 0 && !st->opts.no_scopes) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "at least one --scope is required, or --no-scopes for a "
                                     "remote-disposal credential; a credential with no scopes "
                                     "could not read anything");
            }
            /* Each scope is checked against the closed vocabulary here so the
             * refusal names the offending value. The service layer checks again
             * at the write point, which is the guarantee; this one is the
             * message. With `--no-scopes` there is nothing to parse: the loop
             * below runs zero times and `mask` stays zero, which is the
             * deliberate form `atlas_apikey_create_on` requires
             * `opts->no_scopes` to admit. */
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
            co.no_scopes = st->opts.no_scopes;
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
                ks = atlas_cli_renderer_open(&r, st->opts.json, st->out, "api-key", err);
                if (ks == ATLAS_OK) {
                    ks = r.v->apikey_created(&r, &created, err);
                }
                ks = ks == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), ks);
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
                ks = atlas_cli_renderer_open(&r, st->opts.json, st->out, "api-key", err);
                if (ks == ATLAS_OK) {
                    ks = r.v->apikey_listed(&r, &l, err);
                }
                ks = ks == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), ks);
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
                ks = atlas_cli_renderer_open(&r, st->opts.json, st->out, "api-key", err);
                if (ks == ATLAS_OK) {
                    ks = r.v->apikey_revoked(&r, id, changed, err);
                }
                ks = ks == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), ks);
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
                bs = atlas_cli_renderer_open(&r, st->opts.json, st->out, "backup create", err);
                if (bs == ATLAS_OK) {
                    bs = r.v->backup_created(&r, &rep, err);
                }
                bs = bs == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), bs);
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
                bs = atlas_cli_renderer_open(&r, st->opts.json, st->out, "backup verify", err);
                if (bs == ATLAS_OK) {
                    bs = r.v->backup_verified(&r, &rep, "backup", err);
                }
                bs = bs == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), bs);
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
                bs = atlas_cli_renderer_open(&r, st->opts.json, st->out, "backup restore", err);
                if (bs == ATLAS_OK) {
                    bs = r.v->backup_restored(&r, &rep, err);
                }
                bs = bs == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), bs);
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
            ms = atlas_cli_renderer_open(&r, st->opts.json, st->out,
                               prune ? "maintenance prune" : "maintenance plan", err);
            if (ms == ATLAS_OK) {
                ms = r.v->maintenance(&r, &rep, err);
            }
            ms = ms == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), ms);
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
    if (remote && !atlas_cli_is_a_command(cmd)) {
        /* The same answer a per-user install gives, produced here because the
         * dispatcher's own unknown-command error comes after a context is
         * opened and a foreign index has no context to open. */
        return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown command \"%s\" (try: atlas help)",
                             cmd);
    }
    if (remote && !atlas_cli_remote_serves(st)) {
        return atlas_cli_remote_refuse(st, err);
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
    copts.mode = atlas_cli_mode_for(st);
    atlas_ctx *ctx = NULL;
    if (!remote) {
        s = atlas_ctx_open(&copts, &ctx, err);
        if (s != ATLAS_OK) {
            return s;
        }
    }

    int64_t limit = st->opts.limit > 0 ? st->opts.limit : ATLAS_DEFAULT_LIMIT;

    if (strcmp(cmd, "doctor") == 0) {
        result = atlas_cli_need_operands(st, 0, "doctor", err);
        if (result == ATLAS_OK) {
            atlas_doctor_report rep;
            atlas_doctor_report_init(&rep);
            result = atlas_service_doctor(ctx, &rep, err);
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "doctor", err);
                if (result == ATLAS_OK) {
                    result = r.v->doctor(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            }
            atlas_doctor_report_free(&rep);
        }
    } else if (strcmp(cmd, "repo") == 0) {
        if (st->operand_count == 0) {
            result = atlas_err_set(err, ATLAS_ERR_USAGE,
                                   "usage: atlas repo add|list|remove|scanner ...");
        /* A7 removed every model-reachable route into the registry — there is
         * no `repo.add`, `repo.ensure` or `repo.remove` RPC method, no MCP
         * tool, and no hook that registers. What is left is this local command,
         * and it is not additionally guarded by operator authority: the
         * registry is a table in a database the calling uid can already write.
         * The boundary that matters was the socket, and it is closed. */
        } else if (strcmp(st->operands[0], "add") == 0) {
            if (st->operand_count != 2u) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE,
                                       "usage: atlas repo add PATH [--name NAME] [--scanner-uid UID]");
            } else {
                atlas_repo_info info;
                atlas_repo_info_init(&info);
                result = atlas_service_repo_add_as(ctx, st->operands[1], repo_arg_name,
                                                   repo_scanner_uid_given, repo_scanner_uid, &info,
                                                   err);
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "repo add", err);
                    if (result == ATLAS_OK) {
                        result = r.v->repo_added(&r, &info, err);
                    }
                    if (result == ATLAS_OK) {
                        result = atlas_cli_renderer_close(&r, err);
                    } else {
                        atlas_cli_renderer_abort(&r);
                    }
                }
                atlas_repo_info_free(&info);
            }
        } else if (strcmp(st->operands[0], "scanner") == 0) {
            if (st->operand_count != 2u) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE,
                                       "usage: atlas repo scanner NAME [--scanner-uid UID]");
            } else {
                atlas_repo_info info;
                atlas_repo_info_init(&info);
                result = atlas_service_repo_set_scanner(ctx, st->operands[1],
                                                        repo_scanner_uid_given, repo_scanner_uid,
                                                        &info, err);
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "repo scanner", err);
                    if (result == ATLAS_OK) {
                        result = r.v->repo_scanner_set(&r, &info, err);
                    }
                    if (result == ATLAS_OK) {
                        result = atlas_cli_renderer_close(&r, err);
                    } else {
                        atlas_cli_renderer_abort(&r);
                    }
                }
                atlas_repo_info_free(&info);
            }
        } else if (strcmp(st->operands[0], "list") == 0) {
            if (st->operand_count != 1u) {
                result = atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas repo list");
            } else {
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "repo list", err);
                if (result == ATLAS_OK) {
                    list_sink ls = {&r};
                    int64_t count = 0;
                    result = r.v->list_begin(&r, "repositories", err);
                    if (result == ATLAS_OK) {
                        result = ctx != NULL
                                     ? atlas_service_repo_list(ctx, atlas_cli_repo_item_sink, &ls, &count,
                                                               err)
                                     : atlas_service_repo_list_remote(atlas_cli_repo_item_sink, &ls, &count,
                                                                      err);
                    }
                    if (result == ATLAS_OK) {
                        result = r.v->list_end(&r, "repository", "repositories", count, err);
                    }
                    if (result == ATLAS_OK) {
                        result = atlas_cli_renderer_close(&r, err);
                    } else {
                        atlas_cli_renderer_abort(&r);
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
                    result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "repo remove", err);
                    if (result == ATLAS_OK) {
                        result = r.v->repo_removed(&r, &info, err);
                    }
                    if (result == ATLAS_OK) {
                        result = atlas_cli_renderer_close(&r, err);
                    } else {
                        atlas_cli_renderer_abort(&r);
                    }
                }
                atlas_repo_info_free(&info);
            }
        } else {
            result = atlas_err_set(err, ATLAS_ERR_USAGE, "unknown repo subcommand \"%s\"",
                                   st->operands[0]);
        }
    } else if (strcmp(cmd, "scan") == 0) {
        result = atlas_cli_need_operands(st, 1, "scan NAME", err);
        if (result == ATLAS_OK) {
            atlas_scan_opts so;
            atlas_cli_scan_opts_from_cli(st, &so);
            atlas_scan_summary sum;
            result = atlas_service_scan(ctx, st->operands[0], &so, &sum, err);
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "scan", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->scan(&r, st->operands[0], &sum, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            }
        }
    } else if (strcmp(cmd, "status") == 0) {
        result = atlas_cli_need_operands(st, 1, "status NAME", err);
        if (result == ATLAS_OK) {
            atlas_status_report rep;
            atlas_status_report_init(&rep);
            result = ctx != NULL ? atlas_service_status(ctx, st->operands[0], &rep, err)
                                 : atlas_service_status_remote(st->operands[0], &rep, err);
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "status", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->status(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            }
            atlas_status_report_free(&rep);
        }
    } else if (strcmp(cmd, "search") == 0) {
        result = atlas_cli_need_operands(st, 2, "search NAME QUERY", err);
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
            result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "search", err);
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
                                                        limit, &mode, atlas_cli_search_item_sink, &ls,
                                                        &count, err)
                                 : atlas_service_search_remote(st->operands[0], st->operands[1],
                                                               limit, &mode, atlas_cli_search_item_sink, &ls,
                                                               &count, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->list_end(&r, "result", "results", count, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            } else {
                atlas_cli_renderer_abort(&r);
            }
        }
    } else if (strcmp(cmd, "file") == 0) {
        result = atlas_cli_need_operands(st, 2, "file NAME PATH", err);
        if (result == ATLAS_OK) {
            file_sink fs = {&r, st, st->operands[0], false};
            result = ctx != NULL ? atlas_service_file(ctx, st->operands[0], st->operands[1],
                                                      atlas_cli_file_report_sink, &fs, err)
                                 : atlas_service_file_remote(st->operands[0], st->operands[1],
                                                             atlas_cli_file_report_sink, &fs, err);
            if (result == ATLAS_OK && !fs.opened) {
                /* The path resolved but produced nothing. Still one complete
                 * document, so a script sees an answer rather than silence. */
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "file", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                fs.opened = result == ATLAS_OK;
            }
            if (fs.opened) {
                result = result == ATLAS_OK ? atlas_cli_renderer_close(&r, err) : (atlas_cli_renderer_abort(&r), result);
            }
        }
    } else if (strcmp(cmd, "history") == 0) {
        result = atlas_cli_need_operands(st, 2, "history NAME PATH", err);
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "history", err);
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
                                                         limit, atlas_cli_history_item_sink, &ls, &count, err)
                                 : atlas_service_history_remote(st->operands[0], st->operands[1],
                                                                limit, atlas_cli_history_item_sink, &ls,
                                                                &count, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->list_end(&r, "change", "changes", count, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            } else {
                atlas_cli_renderer_abort(&r);
            }
        }
    } else if (strcmp(cmd, "diff") == 0) {
        result = atlas_cli_need_operands(st, 1, "diff NAME", err);
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
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "diff", err);
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
                                 ? atlas_service_diff(ctx, st->operands[0], &dopts, atlas_cli_diff_item_sink,
                                                      &ls, &second, err)
                                 : atlas_service_diff_remote(st->operands[0], &dopts,
                                                             atlas_cli_diff_item_sink, &ls, &second, err);
                    atlas_diff_report_free(&second);
                }
                if (result == ATLAS_OK) {
                    result = r.v->diff_end(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
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
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "daemon status", err);
                if (result == ATLAS_OK) {
                    result = r.v->daemon_status(&r, &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            }
            atlas_daemon_status_report_free(&rep);
        }
    } else if (strcmp(cmd, "sync") == 0) {
        result = atlas_cli_need_operands(st, 1, "sync NAME [--wait] [--full]", err);
        if (result == ATLAS_OK) {
            atlas_sync_report rep;
            atlas_sync_report_init(&rep);
            result = ctx != NULL
                         ? atlas_service_sync(ctx, st->operands[0], st->opts.full, st->opts.wait,
                                              st->opts.timeout_ms, &rep, err)
                         : atlas_service_sync_remote(st->operands[0], st->opts.full, st->opts.wait,
                                                     st->opts.timeout_ms, &rep, err);
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "sync", err);
                if (result == ATLAS_OK) {
                    result = r.v->note_repo(&r, st->operands[0], err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->sync(&r, st->operands[0], &rep, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            }
            atlas_sync_report_free(&rep);
        }
    } else if (strcmp(cmd, "events") == 0) {
        result = atlas_cli_need_operands(st, 1, "events NAME [--since CURSOR] [--limit N]", err);
        if (result == ATLAS_OK) {
            /* The per-repository state is printed above the journal, so a caller
             * can see whether the events they are reading describe a current
             * index or one with a known hole in it. */
            atlas_repo_state_report state;
            atlas_repo_state_report_init(&state);
            result = ctx != NULL ? atlas_service_repo_state(ctx, st->operands[0], &state, err)
                                 : atlas_service_repo_state_remote(st->operands[0], &state, err);
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_open(&r, st->opts.json, st->out, "events", err);
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
                                                        atlas_cli_event_item_sink, &ls, &count, &next, &more,
                                                        err)
                                 : atlas_service_events_remote(st->operands[0], st->opts.since,
                                                               limit, atlas_cli_event_item_sink, &ls, &count,
                                                               &next, &more, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->list_end(&r, "event", "events", count, err);
                }
                if (result == ATLAS_OK) {
                    result = r.v->events_end(&r, next, more, err);
                }
                if (result == ATLAS_OK) {
                    result = atlas_cli_renderer_close(&r, err);
                } else {
                    atlas_cli_renderer_abort(&r);
                }
            } else {
                atlas_cli_renderer_abort(&r);
            }
            atlas_repo_state_report_free(&state);
        }
    } else if (strcmp(cmd, "code") == 0) {
        result = atlas_cli_run_code(st, ctx, &r, limit, err);
    } else if (strcmp(cmd, "context") == 0) {
        result = atlas_cli_run_context(st, ctx, &r, err);
    } else if (strcmp(cmd, "gate") == 0) {
        result = atlas_cli_run_gate(st, ctx, &r, err);
    } else if (strcmp(cmd, "verify") == 0) {
        result = atlas_cli_run_verify(st, ctx, &r, err);
    } else if (strcmp(cmd, "decision") == 0) {
        result = atlas_cli_run_decision(st, ctx, &r, limit, err);
    } else if (strcmp(cmd, "memory") == 0) {
        result = atlas_cli_run_memory(st, ctx, &r, err);
    } else if (strcmp(cmd, "review") == 0) {
        result = atlas_cli_run_review(st, ctx, &r, err);
    } else {
        result = atlas_err_set(err, ATLAS_ERR_USAGE,
                               "unknown command \"%s\" (try: atlas help)", cmd);
    }

    atlas_ctx_close(ctx);
    return result;
}

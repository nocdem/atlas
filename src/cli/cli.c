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
    atlas_status s = atlas_cli_parse_args(&st, argc, argv, &want_help, &want_version, &err);
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

    s = atlas_cli_run_command(&st, &err);
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

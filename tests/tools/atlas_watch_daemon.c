/* Atlas - the P0 watcher acceptance daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Runs the real daemon with the watch budget injected, and exists for one
 * reason: the acceptance script has to reach a budget the machine's own policy
 * would not give it, and **there is deliberately no public way to set one**.
 *
 * A CLI flag or an environment variable for the watch budget would be a second
 * answer to a question `/etc/atlas/system.conf` owns, reachable by anyone who
 * can start a daemon — which is not the same set of people who can edit a
 * root-owned file. So the budget travels on `atlas_daemon_opts`, and the only
 * things that set it are this tool and `tests/test_watch_budget.c`.
 *
 * What it is not: a different daemon. It calls `atlas_daemon_run` with the
 * options struct production fills in, so the writer, the watcher, the serve
 * loop, the comparison, the allocation rounds and every counter are the shipped
 * ones. The single field that differs is the resolved total.
 *
 * The precedent is `tools/atlas_gen_decisions.c`, which links `atlas_core` for
 * the same reason: a fixture built by a second code path measures a shape the
 * real one never produces.
 *
 * Usage: atlas-watch-daemon DATA_DIR WATCH_BUDGET
 */
#include <stdio.h>
#include <stdlib.h>

#include "atlas/atlas.h"
#include "atlas/daemon.h"
#include "atlas/ipc.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        (void)fprintf(stderr, "usage: atlas-watch-daemon DATA_DIR WATCH_BUDGET\n");
        return 2;
    }
    char *end = NULL;
    long long budget = strtoll(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || budget <= 0) {
        (void)fprintf(stderr, "atlas-watch-daemon: WATCH_BUDGET must be a positive integer\n");
        return 2;
    }

    /* What `atlas --data-dir X daemon run` does when the CLI parses the flag.
     *
     * Without it the socket scope is empty, which means "whatever the root-owned
     * policy says" — so on a machine with a live system deployment this daemon
     * would try to open the shared `/run/atlas` socket owned by `atlasd` and be
     * refused, which is exactly what happened the first time this was run. A
     * fixture daemon addresses its own index and therefore its own socket. */
    atlas_ipc_socket_scope_set(argv[1]);

    atlas_daemon_opts opts;
    atlas_daemon_opts_init(&opts);
    opts.data_dir_override = argv[1];
    opts.watch_budget_total = (int64_t)budget;

    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = atlas_daemon_run(&opts, stderr, &err);
    if (st != ATLAS_OK) {
        (void)fprintf(stderr, "atlas-watch-daemon: %s\n", atlas_err_msg(&err));
        return 1;
    }
    return 0;
}

/* Atlas - the foreground daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas daemon run` stays in the foreground. There is no double fork, no
 * setsid, no pid file and no self-backgrounding: systemd already supervises
 * processes, restarts them, collects their output and reports their state, and
 * a service that re-implements a worse version of that is a service whose
 * failures are invisible.
 *
 * Threads, and what each one is allowed to touch:
 *
 *   main      the IPC serve loop. Owns the listening socket, every client
 *             connection, a signalfd and a read-only database handle. Answers
 *             read requests directly; hands mutations to the writer.
 *   writer    the only thread with a writable database handle. Drains a bounded
 *             job queue and runs reconciliation passes. Every SQLite write in
 *             the daemon happens here.
 *   watcher   owns the inotify descriptor. Translates filesystem events into
 *             debounced reconciliation requests. Touches no database handle.
 *   workers   a bounded pool that hashes file content. Touches no database
 *             handle and creates no process.
 *
 * A SQLite connection is never shared between threads. The rule is structural,
 * not a convention: the writable handle is created by the writer thread and
 * never escapes it, and readers get their own SQLITE_OPEN_READONLY handles.
 */
#ifndef ATLAS_DAEMON_H
#define ATLAS_DAEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "atlas/error.h"

typedef struct atlas_daemon_opts {
    const char *data_dir_override; /* --data-dir */
    size_t worker_count;           /* 0 selects the default */
    int reconcile_interval_ms;     /* 0 selects ATLAS_WATCH_RECONCILE_INTERVAL_MS */
    /* P0. Test hook: the daemon-wide inotify watch budget. 0 resolves it from
     * the root-owned policy and the kernel, which is what production does.
     *
     * Never set by the CLI, and deliberately not settable from one: a boundary
     * test needs a small budget, and a public flag for it would be a second way
     * to configure a resource that `/etc/atlas/system.conf` owns — reachable by
     * anyone who can start a daemon, which is not the same set of people. A
     * positive value replaces the resolved total and changes nothing else, so
     * the comparison, allocation and accounting code under test is production's. */
    int64_t watch_budget_total;
    /* Test hook: reconcile every registered repository once, serve until the
     * queue is empty, then exit 0. Never set by the CLI. */
    bool run_once;
} atlas_daemon_opts;

void atlas_daemon_opts_init(atlas_daemon_opts *o);

/* Runs until SIGTERM or SIGINT. Returns ATLAS_OK on a clean shutdown.
 *
 * `log` receives one line per significant event. It is line-buffered and every
 * untrusted value in it is safe-encoded first, because journald renders these
 * lines and a repository must not be able to write control sequences into a log
 * an operator reads. */
atlas_status atlas_daemon_run(const atlas_daemon_opts *opts, FILE *log, atlas_err *err);

#endif /* ATLAS_DAEMON_H */

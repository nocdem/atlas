/* Atlas - the foreground daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Startup order is load-bearing, so it is written out explicitly:
 *
 *   1. resolve and create the data directory
 *   2. take the writer lock — before anything else, so a second daemon or a
 *      running `atlas scan` is refused now rather than after half a startup
 *   3. freeze the git runtime state, before any thread exists
 *   4. resolve and prepare the runtime directory and socket
 *   5. block the signals we want, then open a signalfd for them
 *   6. start the worker pool, then the writer, then the watcher
 *   7. serve
 *
 * Shutdown is the reverse, and every step is joined rather than abandoned: a
 * daemon that exits with its writer thread mid-transaction is a daemon whose
 * index recovery depends on WAL rather than on shutdown being correct.
 */
#define _GNU_SOURCE 1

#include "atlas/daemon.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/datadir.h"
#include "atlas/ipc.h"
#include "atlas/lock.h"
#include "atlas/safetext.h"
#include "daemon/daemon_internal.h"
#include "git/git_harden.h"

void atlas_daemon_opts_init(atlas_daemon_opts *o) {
    memset(o, 0, sizeof(*o));
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Blocks SIGTERM, SIGINT and SIGPIPE process-wide and returns a signalfd for the
 * first two.
 *
 * Blocking before any thread is created is what makes this correct: a thread
 * inherits the signal mask of its creator, so blocking here means no worker can
 * be the one that receives SIGTERM and dies with a transaction open. SIGPIPE is
 * blocked and never read: writes use MSG_NOSIGNAL, and a daemon should not die
 * because one client hung up. */
static atlas_status install_signals(int *fd_out, atlas_err *err) {
    *fd_out = -1;
    sigset_t mask;
    (void)sigemptyset(&mask);
    (void)sigaddset(&mask, SIGTERM);
    (void)sigaddset(&mask, SIGINT);
    (void)sigaddset(&mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot block signals");
    }
    sigset_t wanted;
    (void)sigemptyset(&wanted);
    (void)sigaddset(&wanted, SIGTERM);
    (void)sigaddset(&wanted, SIGINT);
    int fd = signalfd(-1, &wanted, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_INTERNAL, errno, "cannot create a signalfd");
    }
    *fd_out = fd;
    return ATLAS_OK;
}

atlas_status atlas_daemon_run(const atlas_daemon_opts *opts, FILE *log, atlas_err *err) {
    atlas_daemon_opts defaults;
    atlas_daemon_opts_init(&defaults);
    if (opts == NULL) {
        opts = &defaults;
    }

    atlas_buf data_dir = ATLAS_BUF_INIT;
    atlas_buf db_path = ATLAS_BUF_INIT;
    atlas_buf runtime_dir = ATLAS_BUF_INIT;
    atlas_buf socket_path = ATLAS_BUF_INIT;
    atlas_lock *lock = NULL;
    atlas_workers *workers = NULL;
    atlas_writer *writer = NULL;
    atlas_watcher *watcher = NULL;
    int listen_fd = -1;
    int signal_fd = -1;
    atlas_safe_pool safe;
    atlas_safe_pool_init(&safe);
    atomic_bool stop;
    atomic_init(&stop, false);

    atlas_datadir_source src = ATLAS_DATADIR_OVERRIDE;
    atlas_status st = atlas_datadir_resolve(opts->data_dir_override, &data_dir, &src, err);
    if (st == ATLAS_OK) {
        st = atlas_datadir_ensure(atlas_buf_cstr(&data_dir), err);
    }
    if (st == ATLAS_OK) {
        st = atlas_datadir_db_path(atlas_buf_cstr(&data_dir), &db_path, err);
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    /* The lock first. Everything after this assumes sole ownership of the index,
     * and finding out otherwise later would mean unwinding a half-started
     * daemon. */
    st = atlas_lock_acquire(atlas_buf_cstr(&data_dir), ATLAS_LOCK_ROLE_DAEMON, &lock, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    /* Freeze the git runtime state while the process is still single-threaded,
     * so the PATH search happens exactly once and every later reader observes a
     * value that never changes. A missing git is then reported here, at startup,
     * rather than by whichever worker first needed it. */
    st = atlas_git_runtime_init(err);
    if (st != ATLAS_OK) {
        goto done;
    }

    /* A7.1. Loaded once, here, and carried unchanged into the serve loop.
     *
     * Loading it once means the set of uids this daemon will accept is fixed for
     * the life of the process: a policy edit takes effect on restart, which an
     * operator can reason about, rather than mid-connection. Anything other than
     * a complete root-anchored policy leaves it zeroed, which is legacy per-user
     * mode — the daemon then serves its own uid and nobody else, exactly as
     * before A7.1. */
    atlas_syspolicy syspolicy;
    atlas_syspolicy_load(&syspolicy);
    /* System mode applies only when this daemon is actually serving the index
     * the policy describes.
     *
     * A daemon started with `--data-dir` somewhere else — an isolated fixture,
     * or a second index an operator is inspecting — is not the shared daemon,
     * and must not shape its socket for the client group or accept the client
     * allowlist. It is a per-user daemon that happens to be running on a
     * machine where a system deployment also exists, and it gets per-user
     * rules: its own uid, its own runtime directory, mode 0600.
     *
     * Without this the policy's mere presence would make every fixture daemon
     * try to hand its socket to `atlas-clients`, which is both wrong and, in a
     * test tree, impossible. */
    const bool serving_system_index =
        syspolicy.state == ATLAS_SYSPOLICY_SYSTEM &&
        strcmp(atlas_buf_cstr(&data_dir), syspolicy.data_dir) == 0;
    const atlas_syspolicy *policy_arg = serving_system_index ? &syspolicy : NULL;
    if (!serving_system_index) {
        /* Carried into the serve loop as legacy, so `atlas_ipc_accept` permits
         * this uid and nobody else. */
        memset(&syspolicy, 0, sizeof(syspolicy));
    }
    if (log != NULL) {
        (void)fprintf(log, "atlas: deployment mode %s (%s)\n",
                      policy_arg != NULL ? "system" : "per-user",
                      atlas_syspolicy_reason_name(syspolicy.reason));
        (void)fflush(log);
    }

    st = atlas_ipc_runtime_dir(&runtime_dir, err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_ensure_runtime_dir(atlas_buf_cstr(&runtime_dir), policy_arg, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_socket_path(&socket_path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_ipc_listen(atlas_buf_cstr(&socket_path), policy_arg, &listen_fd, err);
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    st = install_signals(&signal_fd, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    atlas_daemon_log(log, "info", "atlas %s starting: data %s, socket %s", ATLAS_VERSION_STRING,
                     atlas_safe(&safe, atlas_buf_cstr(&data_dir)),
                     atlas_safe(&safe, atlas_buf_cstr(&socket_path)));

    st = atlas_workers_start(opts->worker_count, &workers, err);
    if (st == ATLAS_OK) {
        /* The writer records the daemon's liveness row itself, because it owns
         * the only writable handle. That row is diagnostic only: the lock, not
         * the row, is what proves a daemon is running, since a killed daemon
         * leaves the row behind and the released lock is what disproves it. */
        st = atlas_writer_start(atlas_buf_cstr(&db_path), atlas_buf_cstr(&data_dir),
                                atlas_buf_cstr(&socket_path), workers, log, &writer, err);
    }
    if (st != ATLAS_OK) {
        goto done;
    }

    /* Loaded before the watcher starts, because the watcher's timer is what
     * drives A8's recovery sweep and it has to be told whether to sweep. */
    atlas_orchpolicy orchpolicy;
    if (serving_system_index) {
        atlas_orchpolicy_load(&orchpolicy);
    } else {
        memset(&orchpolicy, 0, sizeof(orchpolicy));
    }

    /* A9. Loaded here for the reason the other two are, and gated the same way:
     * a per-user daemon serving somebody's own index is not a system deployment
     * and must not consult a machine-wide policy to decide who may reach a
     * privileged method group. A zeroed policy leaves `gateway_uid` at zero,
     * and zero matches no peer, so the `gateway.` group is offered to nobody. */
    atlas_gwpolicy gwpolicy;
    if (opts->gwpolicy_text != NULL) {
        /* A16. Test hook, `atlas_daemon_opts.gwpolicy_text`'s own precedent:
         * the only production route to an injected policy, and it is set by
         * no CLI flag. `tests/tools/atlas_gw_daemon.c` reads a fixture-written,
         * user-owned policy file itself -- deliberately not through
         * `atlas_gwpolicy_load_at`'s root-ownership walk, which only a real
         * root-owned file can ever pass -- and hands the bytes here. Checked
         * ahead of `serving_system_index`: a fixture daemon is never "the"
         * system deployment, so the ordinary branch below would otherwise
         * leave this daemon with no gateway policy at all and nothing to
         * inject it with. */
        atlas_gwpolicy_parse_buffer(opts->gwpolicy_text, strlen(opts->gwpolicy_text), &gwpolicy);
    } else if (serving_system_index) {
        atlas_gwpolicy_load(&gwpolicy);
    } else {
        memset(&gwpolicy, 0, sizeof(gwpolicy));
    }

    atlas_watcher_opts wopts;
    atlas_watcher_opts_init(&wopts);
    wopts.orch_enabled = orchpolicy.state == ATLAS_ORCHPOLICY_ENABLED;
    wopts.system_deployment = serving_system_index;
    wopts.reconcile_interval_ms = opts->reconcile_interval_ms;
    /* The only production route to an injected bound, and it is set by no CLI
     * flag: `atlas_daemon_opts.watch_budget_total` exists for the acceptance
     * harness in `tests/tools/atlas_watch_daemon.c`. */
    wopts.inject_budget_total = opts->watch_budget_total;
    st = atlas_watcher_start(atlas_buf_cstr(&db_path), atlas_buf_cstr(&data_dir), writer, log,
                             &wopts, &watcher, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    /* Started after the writer, because a queued semantic index reports back
     * through this table and the writer must never find it missing; stopped
     * before the writer for the same reason, so nothing is still trying to
     * record a result into a table that has gone. */
    atlas_ops *ops = NULL;
    st = atlas_ops_start(atlas_buf_cstr(&data_dir), log, &ops, err);
    if (st != ATLAS_OK) {
        goto done;
    }

    atlas_writer_set_ops(writer, ops);

    atlas_server_ctx sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.db_path = atlas_buf_cstr(&db_path);
    sctx.data_dir = atlas_buf_cstr(&data_dir);
    sctx.socket_path = atlas_buf_cstr(&socket_path);
    sctx.writer = writer;
    sctx.watcher = watcher;
    /* A13. Scanner liveness. NULL is survivable and reads as "never heard from",
     * which only ever subtracts -- so a failed allocation costs conservatism
     * rather than the daemon. */
    sctx.scanner_seen = atlas_scanner_seen_new();
    sctx.workers = workers;
    sctx.ops = ops;
    sctx.log = log;
    sctx.syspolicy = syspolicy;
    sctx.gwpolicy = gwpolicy;
    /* A8. Loaded once, here, for the reason the system policy is loaded once:
     * the set of principals, repositories, modes and drivers orchestration runs
     * under must not change under a running serve loop. A machine with no
     * policy file leaves this zeroed, which is orchestration disabled — every
     * `job.` and `dispatch.` method then refuses, and the daemon serves
     * everything else exactly as before. */
    /* **Only when this daemon is serving the system index.**
     *
     * The orchestration policy is read from a compiled-in path, so its mere
     * presence would otherwise arm orchestration in *every* daemon on the
     * machine — including a fixture daemon in a test tree and any ad-hoc daemon
     * an unprivileged user starts on their own database. That daemon could not
     * touch the live index, but it would be running the operator's orchestration
     * configuration in a context the operator never intended, and a uid the
     * policy lists as a submitter could hand it work.
     *
     * This is the same guard `serving_system_index` applies to the system
     * policy, for the same reason, and a clean-extraction run caught its absence
     * by finding a fixture daemon that had quietly inherited the live policy. */
    sctx.orchpolicy = orchpolicy;
    sctx.started_at_ms = monotonic_ms();

    atlas_daemon_log(log, "info", "serving on %s with %zu workers",
                     atlas_safe(&safe, atlas_buf_cstr(&socket_path)),
                     atlas_workers_count(workers));

    if (opts->run_once) {
        /* Test mode: wait until the watcher has submitted its initial passes and
         * the writer has drained them, then stop. Bounded, so a hung pass fails
         * the test rather than hanging it. */
        int64_t deadline = monotonic_ms() + 120000;
        while (monotonic_ms() < deadline) {
            struct timespec nap = {0, 50L * 1000000L};
            (void)nanosleep(&nap, NULL);
            if (atlas_watcher_primed(watcher) && atlas_writer_queue_depth(writer) == 0) {
                break;
            }
        }
        atomic_store(&stop, true);
    }

    st = atlas_server_serve(&sctx, listen_fd, signal_fd, &stop, err);
    atlas_daemon_log(log, "info", "shutting down");

done:
    /* Reverse order, and every thread joined. The watcher stops first so no new
     * work is queued; the writer then drains what is already queued and closes
     * its handle; the workers are last because the writer may still be using
     * them as it drains. */
    atlas_watcher_stop(watcher);
    atlas_scanner_seen_free(sctx.scanner_seen);
    /* Before the writer: an operation in flight is a backup being verified or a
     * semantic generation being written, and both must reach a decision point
     * rather than stop half-way. Waiting here is what makes "a failed or
     * interrupted index never replaces the last valid generation" true of a
     * clean shutdown as well as of a crash. */
    atlas_ops_stop(ops);
    atlas_writer_stop(writer);
    atlas_workers_stop(workers);
    if (signal_fd >= 0) {
        (void)close(signal_fd);
    }
    if (listen_fd >= 0) {
        (void)close(listen_fd);
        /* Only ours to remove, and only because we are the process that bound
         * it and still hold the writer lock. */
        (void)unlink(atlas_buf_cstr(&socket_path));
    }
    atlas_lock_release(lock);
    atlas_safe_pool_free(&safe);
    atlas_buf_free(&data_dir);
    atlas_buf_free(&db_path);
    atlas_buf_free(&runtime_dir);
    atlas_buf_free(&socket_path);
    return st;
}

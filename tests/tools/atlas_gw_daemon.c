/* Atlas - the A16 T5 / A14 remote-disposal and remote-submit acceptance daemon.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Runs the real daemon with a gateway policy injected from a plain file, and
 * exists for one reason: `tests/test_gw_dispose.c` and
 * `tests/test_orch_remote_rpc.c` have to reach a daemon that offers the
 * remote-disposal and remote-submit groups under a chosen policy pair -- and
 * `atlas_gwpolicy_load`'s root-ownership walk can only ever succeed for a
 * genuinely root-owned `/etc/atlas/gateway.conf`. A fixture cannot produce
 * one and must not try to: this reads the files themselves, by an ordinary
 * `open`/`read`, deliberately bypassing that walk, and hands the raw bytes to
 * `atlas_daemon_run` through `atlas_daemon_opts.gwpolicy_text` and
 * `atlas_daemon_opts.orchpolicy_text`, which are the only production routes to
 * injected policies and are set by no CLI flag.
 *
 * What it is not: a different daemon. It calls `atlas_daemon_run` with the
 * options struct production fills in, so the writer, the serve loop, the
 * dispatcher and the peer test under it are all the shipped ones. The single
 * thing that differs is where the policies' bytes came from.
 *
 * It links `atlas_core`, on `tools/atlas_gen_decisions.c`'s and
 * `tools/atlas_watch_daemon.c`'s precedent: a fixture driven by a second code
 * path measures a shape the real one never produces.
 *
 * A preflight check, before the daemon is ever started: the same
 * `atlas_gwpolicy_parse_buffer` the daemon will call is called here first, and
 * a gateway policy that does not come back ENABLED is reported and refused --
 * `atlas-gw-daemon` never binds a socket for a policy this loader rejected,
 * so a test asserting "the loader refuses this policy" observes the refusal
 * directly rather than inferring it from what a live daemon later declined to
 * offer.  The orchestration policy has no preflight here: a fixture proving the
 * daemon ignores a malformed orch policy can start the daemon and check its
 * behaviour directly.
 *
 * Usage: atlas-gw-daemon DATA_DIR POLICY_FILE [ORCH_POLICY_FILE]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/daemon.h"
#include "atlas/gwpolicy.h"
#include "atlas/ipc.h"

/* A policy is at most a few kilobytes (`atlas_gwpolicy_load_at`'s own bound is
 * 8192). A fixture-written file well past that is not a policy anybody could
 * have meant, so it is refused rather than read partially. */
#define GW_DAEMON_POLICY_MAX_BYTES 65536u

/* Second buffer for the optional orchestration policy text. */
static char orch_policy_text[GW_DAEMON_POLICY_MAX_BYTES + 1u];

static int read_policy_file(const char *path, char *buf, size_t buf_size, size_t *len_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        (void)fprintf(stderr, "atlas-gw-daemon: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t total = 0;
    for (;;) {
        if (total >= buf_size) {
            (void)fprintf(stderr, "atlas-gw-daemon: %s is larger than %zu bytes\n", path,
                          buf_size);
            (void)close(fd);
            return -1;
        }
        ssize_t got = read(fd, buf + total, buf_size - total);
        if (got < 0) {
            (void)fprintf(stderr, "atlas-gw-daemon: cannot read %s: %s\n", path, strerror(errno));
            (void)close(fd);
            return -1;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    (void)close(fd);
    *len_out = total;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        (void)fprintf(stderr,
                      "usage: atlas-gw-daemon DATA_DIR POLICY_FILE [ORCH_POLICY_FILE]\n");
        return 2;
    }
    const char *data_dir = argv[1];
    const char *policy_file = argv[2];
    const char *orch_policy_file = argc >= 4 ? argv[3] : NULL;

    static char policy_text[GW_DAEMON_POLICY_MAX_BYTES + 1u];
    size_t policy_len = 0;
    if (read_policy_file(policy_file, policy_text, GW_DAEMON_POLICY_MAX_BYTES, &policy_len) != 0) {
        return 2;
    }
    policy_text[policy_len] = '\0';

    /* Refused by the loader, before anything is bound. A test proving a
     * malformed policy never offers the disposal group is proving it against
     * this exit code and this message, not against a daemon it never had to
     * start. */
    atlas_gwpolicy preflight;
    atlas_gwpolicy_parse_buffer(policy_text, policy_len, &preflight);
    if (preflight.state != ATLAS_GWPOLICY_ENABLED) {
        (void)fprintf(stderr, "atlas-gw-daemon: policy refused: %s (%s)\n",
                      atlas_gwpolicy_reason_name(preflight.reason), preflight.detail);
        return 1;
    }

    /* A14. Load the orchestration policy file when provided. */
    if (orch_policy_file != NULL) {
        size_t orch_len = 0;
        if (read_policy_file(orch_policy_file, orch_policy_text, GW_DAEMON_POLICY_MAX_BYTES,
                             &orch_len) != 0) {
            return 2;
        }
        orch_policy_text[orch_len] = '\0';
    }

    /* What `atlas --data-dir X daemon run` does when the CLI parses the flag.
     * Without it this daemon would try the shared `/run/atlas` socket a real
     * system deployment on this machine owns. */
    atlas_ipc_socket_scope_set(data_dir);

    atlas_daemon_opts opts;
    atlas_daemon_opts_init(&opts);
    opts.data_dir_override = data_dir;
    opts.gwpolicy_text = policy_text;
    if (orch_policy_file != NULL) {
        opts.orchpolicy_text = orch_policy_text;
    }

    atlas_err err;
    atlas_err_init(&err);
    atlas_status st = atlas_daemon_run(&opts, stderr, &err);
    if (st != ATLAS_OK) {
        (void)fprintf(stderr, "atlas-gw-daemon: %s\n", atlas_err_msg(&err));
        return 1;
    }
    return 0;
}

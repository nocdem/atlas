/* Atlas - A13: the scanner process.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A scanner runs as a repository's owner and reads a tree the daemon cannot.
 * This is the process side of that: it opens no index, takes no lock and holds
 * no database handle. Every answer it gets comes over the daemon socket, which
 * is why the CLI dispatches it before any `atlas_ctx` is opened — the same
 * arrangement `atlas gateway run` and `atlas dispatcher run` use, and for the
 * same reason.
 *
 * This plan ships the channel only. There is no loop yet, and none is
 * simulated: `--once` asks once and returns, and without it the command refuses
 * rather than idling. A process that idles silently looks healthy in
 * `systemctl status` while doing nothing, which is the failure the dispatcher
 * refuses to start for.
 */
#include "atlas/service.h"

#include <stdio.h>

#include "atlas/ipc.h"

atlas_status atlas_service_scanner_run(bool once, FILE *log, atlas_err *err) {
    if (!once) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas scanner run needs --once: the polling loop is not implemented "
                             "yet, and a process that idled instead of saying so would look "
                             "healthy while doing nothing");
    }

    atlas_buf socket_path = ATLAS_BUF_INIT;
    atlas_status st = atlas_ipc_socket_path(&socket_path, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&socket_path);
        return st;
    }

    atlas_buf raw = ATLAS_BUF_INIT;
    st = atlas_ipc_call(atlas_buf_cstr(&socket_path), "scanner.poll", "{}", &raw, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&raw);
        atlas_buf_free(&socket_path);
        return st;
    }

    atlas_ipc_response *resp = NULL;
    st = atlas_ipc_response_parse(raw.data, raw.len, &resp, err);
    atlas_buf_free(&raw);
    if (st != ATLAS_OK) {
        atlas_buf_free(&socket_path);
        return st;
    }
    if (!atlas_ipc_response_ok(resp)) {
        /* The daemon's message is already safe-encoded, and its status is the
         * CLI's exit-code vocabulary, so both travel out unchanged. */
        atlas_status refused = atlas_err_set(err, atlas_ipc_response_status(resp), "%s",
                                             atlas_ipc_response_message(resp));
        atlas_ipc_response_free(resp);
        atlas_buf_free(&socket_path);
        return refused;
    }

    size_t n = 0;
    if (!atlas_ipc_result_arr_len(resp, "repositories", &n)) {
        n = 0;
    }
    if (log != NULL) {
        (void)fprintf(log, "scanner: %zu repository/repositories for this uid\n", n);
        for (size_t i = 0; i < n; i++) {
            const char *name = NULL;
            const char *root = NULL;
            if (!atlas_ipc_result_arr_obj_str(resp, "repositories", i, "name", &name)) {
                name = "";
            }
            if (!atlas_ipc_result_arr_obj_str(resp, "repositories", i, "root", &root)) {
                root = "";
            }
            /* Both were encoded by the daemon on the way out; printing them
             * again through the encoder would double-encode. */
            (void)fprintf(log, "  %s  %s\n", name, root);
        }
    }

    atlas_ipc_response_free(resp);
    atlas_buf_free(&socket_path);
    return ATLAS_OK;
}

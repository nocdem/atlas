/* Atlas - the IPC client used by CLI commands.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Deliberately minimal: connect, one request, one response, close. No session,
 * no reconnection, no retry loop. A command that cannot reach the daemon decides
 * for itself whether to take the offline path, and that decision is visible in
 * the calling code rather than buried in a transport.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/ipc.h"

/* Builds the request document.
 *
 * `method` is a compile-time constant from Atlas' own dispatch table and
 * `params_json` is produced by Atlas, never by a user: both are trusted here, so
 * no escaping is applied. If either ever becomes caller-supplied, it has to be
 * built through the streaming writer instead. */
static atlas_status build_request(const char *method, const char *params_json, atlas_buf *out,
                                  atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append_str(out, "{\"id\":\"cli\",\"method\":\"", err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, method, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, "\",\"params\":", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_str(out, params_json != NULL ? params_json : "{}", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(out, '}', err);
    }
    return st;
}

atlas_status atlas_ipc_call(const char *socket_path, const char *method, const char *params_json,
                            atlas_buf *response_out, atlas_err *err) {
    return atlas_ipc_call_timeout(socket_path, method, params_json, ATLAS_IPC_READ_TIMEOUT_MS,
                                  response_out, err);
}

atlas_status atlas_ipc_call_timeout(const char *socket_path, const char *method,
                                    const char *params_json, int timeout_ms,
                                    atlas_buf *response_out, atlas_err *err) {
    if (timeout_ms <= 0) {
        timeout_ms = ATLAS_IPC_READ_TIMEOUT_MS;
    }
    atlas_buf req = ATLAS_BUF_INIT;
    atlas_status st = build_request(method, params_json, &req, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&req);
        return st;
    }
    if (req.len > ATLAS_IPC_MAX_REQUEST_BYTES) {
        atlas_buf_free(&req);
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "refusing to send a %zu byte request, above the limit", req.len);
    }

    int fd = -1;
    st = atlas_ipc_connect(socket_path, timeout_ms, &fd, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&req);
        return st;
    }

    st = atlas_ipc_write_frame(fd, req.data, req.len, timeout_ms, err);
    atlas_buf_free(&req);
    if (st == ATLAS_OK) {
        bool eof = false;
        st = atlas_ipc_read_frame(fd, ATLAS_IPC_MAX_RESPONSE_BYTES, timeout_ms, response_out, &eof,
                                  err);
        if (st == ATLAS_OK && eof) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                               "the Atlas daemon closed the connection without answering");
        }
    }
    (void)close(fd);
    return st;
}

bool atlas_ipc_daemon_reachable(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf path = ATLAS_BUF_INIT;
    bool ok = false;
    /* Every failure here — no XDG_RUNTIME_DIR, no socket, nothing listening —
     * means the same thing to every caller: take the offline path. Reporting
     * them separately would make each call site re-derive that. */
    if (atlas_ipc_socket_path(&path, &err) == ATLAS_OK) {
        int fd = -1;
        if (atlas_ipc_connect(atlas_buf_cstr(&path), 1000, &fd, &err) == ATLAS_OK) {
            (void)close(fd);
            ok = true;
        }
    }
    atlas_buf_free(&path);
    return ok;
}

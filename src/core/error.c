/* Atlas - error and status handling.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include "atlas/error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

const char *atlas_status_name(atlas_status st) {
    switch (st) {
    case ATLAS_OK: return "ok";
    case ATLAS_ERR_INTERNAL: return "internal";
    case ATLAS_ERR_USAGE: return "usage";
    case ATLAS_ERR_CONFIG: return "config";
    case ATLAS_ERR_REPO: return "repo";
    case ATLAS_ERR_DB: return "db";
    case ATLAS_ERR_GIT: return "git";
    case ATLAS_ERR_INTEGRITY: return "integrity";
    }
    return "unknown";
}

void atlas_err_init(atlas_err *err) {
    if (err == NULL) {
        return;
    }
    err->status = ATLAS_OK;
    err->msg[0] = '\0';
    err->sys_errno = 0;
    err->exit_code = -1;
}

atlas_status atlas_err_setv(atlas_err *err, atlas_status st, const char *fmt, va_list ap) {
    if (err == NULL) {
        return st;
    }
    err->status = st;
    if (fmt == NULL) {
        err->msg[0] = '\0';
    } else {
        int n = vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
        if (n < 0) {
            err->msg[0] = '\0';
        }
    }
    return st;
}

atlas_status atlas_err_set(atlas_err *err, atlas_status st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)atlas_err_setv(err, st, fmt, ap);
    va_end(ap);
    return st;
}

atlas_status atlas_err_set_errno(atlas_err *err, atlas_status st, int errnum, const char *fmt,
                                 ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)atlas_err_setv(err, st, fmt, ap);
    va_end(ap);
    if (err != NULL) {
        err->sys_errno = errnum;
        size_t len = strlen(err->msg);
        if (len + 2u < sizeof(err->msg)) {
            (void)snprintf(err->msg + len, sizeof(err->msg) - len, ": %s", strerror(errnum));
        }
    }
    return st;
}

const char *atlas_err_msg(const atlas_err *err) {
    if (err == NULL) {
        return "";
    }
    return err->msg;
}

/* Atlas - error and status handling.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#ifndef ATLAS_ERROR_H
#define ATLAS_ERROR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Status codes. The numeric values are the process exit codes and are part of
 * the stable CLI contract; see docs/architecture.md. */
typedef enum atlas_status {
    ATLAS_OK = 0,             /* success */
    ATLAS_ERR_INTERNAL = 1,   /* unexpected internal failure (incl. allocation) */
    ATLAS_ERR_USAGE = 2,      /* bad command line */
    ATLAS_ERR_CONFIG = 3,     /* environment/data-directory configuration problem */
    ATLAS_ERR_REPO = 4,       /* repository unknown, invalid, or path not indexed */
    ATLAS_ERR_DB = 5,         /* database/migration failure */
    ATLAS_ERR_GIT = 6,        /* git execution, timeout, or output-parse failure */
    ATLAS_ERR_INTEGRITY = 7   /* safety or integrity invariant violated */
} atlas_status;

/* Human-readable name of a status code (never NULL). */
const char *atlas_status_name(atlas_status st);

/* Error detail carrier. Callers own the storage; it is always by-value so no
 * allocation can fail while reporting an error. */
#define ATLAS_ERR_MSG_MAX 1024u

typedef struct atlas_err {
    atlas_status status;
    char msg[ATLAS_ERR_MSG_MAX];
    /* Optional machine-readable context filled in by some subsystems. */
    int sys_errno;   /* errno value, or 0 */
    int exit_code;   /* child process exit code, or -1 */
    /* A12.0. Whether this failure was the *carriage* of a request rather than an
     * answer to it: the connect, the send, the read, or a reply that was never
     * a reply. See atlas_err_mark_transport. */
    bool transport;
} atlas_err;

/* Reset to a clean, successful state. Safe on any non-NULL pointer. */
void atlas_err_init(atlas_err *err);

/* Set status and message. Truncates safely. Always returns `st` so callers can
 * write `return atlas_err_set(err, ATLAS_ERR_DB, "...");`. `err` may be NULL. */
atlas_status atlas_err_set(atlas_err *err, atlas_status st, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

atlas_status atlas_err_setv(atlas_err *err, atlas_status st, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

/* Set status and message, appending ": <strerror(errnum)>". */
atlas_status atlas_err_set_errno(atlas_err *err, atlas_status st, int errnum,
                                 const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Message text, or "" when unset. Never NULL. */
const char *atlas_err_msg(const atlas_err *err);

/* --- A12.0: a failure of the carriage is not a refusal ----------------------
 *
 * A client that sent a request and did not get an answer, and a client that was
 * answered "no", hold opposite facts. The refusal is final: asking again gets
 * the same answer. The lost answer says nothing at all about whether the
 * request was processed, so it is worth asking again — and, for a request that
 * cannot be applied twice, worth finding out about before it is mourned.
 *
 * Both arrive as an `atlas_status` and a sentence, and no status separates them:
 * a daemon refusal carries whatever code the daemon chose, and a read timeout
 * carries `ATLAS_ERR_INTERNAL`, which a refusal may carry too. So the layer that
 * *experienced* the failure marks it, and a caller asks the mark rather than the
 * prose.
 *
 * **The mark cannot travel a socket.** Only a status and a message cross it, so
 * nothing a daemon says — or quotes back from a repository, a task or a model —
 * can produce one; it is stamped locally by the code that owned the file
 * descriptor. That is the whole difference between this and matching text.
 *
 * Every `atlas_err_set*` clears it, so a fresh error is an answer until the
 * layer that carried it says otherwise, and the mark is applied after the
 * message rather than before it. */
void atlas_err_mark_transport(atlas_err *err);
bool atlas_err_is_transport(const atlas_err *err);

#endif /* ATLAS_ERROR_H */

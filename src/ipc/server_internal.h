/* Atlas - shared state between the IPC serve loop and its method groups.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header.
 *
 * A1 had eight methods and one file. A2 adds a group of them — sessions, change
 * sets, reasons, decisions and the context envelope — and putting those in the
 * same file as the serve loop would bury the loop under them. The dispatch
 * table is still one table, assembled in server.c from the groups, so there is
 * no second dispatcher to drift from the first.
 */
#ifndef ATLAS_IPC_SERVER_INTERNAL_H
#define ATLAS_IPC_SERVER_INTERNAL_H

#include "atlas/db.h"
#include "atlas/error.h"
#include "atlas/ipc.h"
#include "atlas/json.h"
#include "atlas/safetext.h"
#include "daemon/daemon_internal.h"

/* What one method invocation has to work with.
 *
 * `db` is a read-only handle opened for this request and closed after it, so
 * every request observes a committed snapshot taken at its own start and no
 * reader accumulates an open transaction that pins the WAL. A method that needs
 * to write hands the work to the writer thread; it never gets a writable
 * handle, because there is exactly one of those and the writer owns it. */
typedef struct dispatch_state {
    atlas_server_ctx *ctx;
    atlas_db *db; /* read-only */
    atlas_json *j;
    atlas_safe_pool safe;
} dispatch_state;

typedef atlas_status (*atlas_method_fn)(dispatch_state *ds, const atlas_ipc_request *req,
                                        atlas_err *err);

typedef struct atlas_method_entry {
    const char *name;
    atlas_method_fn fn;
} atlas_method_entry;

/* The A2 method group: repository resolution and everything under `ai.`. */
const atlas_method_entry *atlas_server_ai_methods(size_t *count_out);
/* The A3 method group: everything under `code.`. Looked up through the same
 * dispatch as the others, in server.c. */
const atlas_method_entry *atlas_server_code_methods(size_t *count_out);
/* The A4 method group: everything under `decision.`. Looked up through the same
 * dispatch as the others, in server.c. */
const atlas_method_entry *atlas_server_decision_methods(size_t *count_out);

/* Resolves the `repo` parameter with the CLI's error text. Shared because two
 * groups need it and two copies would answer differently. */
atlas_status atlas_server_require_repo(dispatch_state *ds, const atlas_ipc_request *req,
                                       atlas_repo_info *out, atlas_err *err);

/* Writes one repository's index state as a set of object members. Shared by
 * repo.list, repo.state and the A2 context builder so the three cannot describe
 * the same repository differently. */
atlas_status atlas_server_write_repo_state(dispatch_state *ds, const atlas_repo_info *ri,
                                           atlas_err *err);

/* True when this repository's index may be described as current, with the fixed
 * Atlas string explaining why not when it may not. `*reason_out` is NULL when it
 * is current. Computed in one place so no caller reconstructs it from flags. */
bool atlas_server_index_current(const atlas_index_state *s, const char **reason_out);

#endif /* ATLAS_IPC_SERVER_INTERNAL_H */

/* Atlas - internals of src/core/service_decision.c exposed for testing.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header, and not `src/db/db_internal.h`'s precedent -- that
 * header is intra-module sharing between the translation units in `src/db`, a
 * different problem. This is a test-only exposure of one internal function,
 * and the repository's actual precedent for that is P0's `atlas_writer_test_stall`
 * et al. in `src/daemon/daemon_internal.h`: "declared here and nowhere else:
 * no CLI flag, no environment variable, no RPC method, no MCP tool, no policy
 * key, and no caller outside `tests/`." `atlas_service_decision_op_to_params`
 * satisfies the identical rule -- it has external linkage only so
 * `tests/test_decision_remote.c` can drive it directly and prove that a
 * REMOTE op is refused before anything is serialised for the socket, and no
 * production caller outside `src/core/service_decision.c` includes this
 * header.
 */
#ifndef ATLAS_SERVICE_DECISION_INTERNAL_H
#define ATLAS_SERVICE_DECISION_INTERNAL_H

#include "atlas/buf.h"
#include "atlas/decision_ops.h"
#include "atlas/error.h"

/* Serialises `op` into IPC request parameters. Refuses a REMOTE op outright:
 * that channel is handled entirely inside the daemon
 * (`src/ipc/server_remote.c`) and never travels over this socket. */
atlas_status atlas_service_decision_op_to_params(const atlas_decision_op *op, atlas_buf *out,
                                                 atlas_err *err);

#endif /* ATLAS_SERVICE_DECISION_INTERNAL_H */

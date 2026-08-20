/* Atlas - shared helpers inside the service layer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header. Split out so that one command per translation unit can
 * still share repository lookup without duplicating its error messages.
 */
#ifndef ATLAS_SERVICE_INTERNAL_H
#define ATLAS_SERVICE_INTERNAL_H

#include "atlas/ipc.h"
#include "atlas/service.h"

/* Loads a registered repository by name, or fails with a message naming it. */
atlas_status atlas_service_require_repo(atlas_ctx *ctx, const char *name, atlas_repo_info *out,
                                        atlas_err *err);

/* Opens the git adapter for a registered repository and verifies that its
 * canonical root has not moved since registration. */
atlas_status atlas_service_open_repo_git(const atlas_repo_info *info, atlas_git **out,
                                         atlas_err *err);

/* The fresh git observation behind `atlas status NAME`, shared by the local
 * read and the daemon-served one. Fills `live_head`, `live_state`, `git_ok`,
 * `git_error` and `head_drift` from `out->repo.root_path` and `out->scanned`. */
atlas_status atlas_service_status_observe_live(atlas_status_report *out, atlas_err *err);

/* --- talking to the daemon --------------------------------------------------
 *
 * One request out, one parsed response back, for the two domains that have no
 * offline path: orchestration and, since A12.0, plans. Both live in the index,
 * `atlasd` is the only writer of either, and a CLI that fell back to opening the
 * database itself would be a second writer.
 *
 * Shared rather than copied. A12.0 added a second translation unit that speaks
 * this protocol, and a second copy of "build the params, call, parse, turn a
 * refusal into an `atlas_err`" would be a second place for the refusal handling
 * to drift — which is the only interesting part of it.
 *
 * `build` writes the request's parameter members and may be NULL for a method
 * that takes none. On success `*out` is a parsed response the caller frees with
 * `atlas_ipc_response_free`, and `raw` holds the bytes it borrows from and must
 * outlive it. A refusal returns the daemon's own status and message and leaves
 * `*out` non-NULL when the response parsed, so a caller can still read whatever
 * typed detail the refusal carried. */
typedef atlas_status (*atlas_service_build_fn)(atlas_json *j, void *ud, atlas_err *err);

atlas_status atlas_service_orch_call(atlas_ctx *ctx, const char *method,
                                     atlas_service_build_fn build, void *ud,
                                     atlas_ipc_response **out, atlas_buf *raw, atlas_err *err);

#endif /* ATLAS_SERVICE_INTERNAL_H */

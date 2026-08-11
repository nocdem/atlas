/* Atlas - shared helpers inside the service layer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Not a public header. Split out so that one command per translation unit can
 * still share repository lookup without duplicating its error messages.
 */
#ifndef ATLAS_SERVICE_INTERNAL_H
#define ATLAS_SERVICE_INTERNAL_H

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

#endif /* ATLAS_SERVICE_INTERNAL_H */

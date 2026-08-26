/* Atlas - A13: which uid's scanner may report facts about a repository.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The daemon runs as its own principal and cannot read a tree it does not own.
 * A13's answer is a scanner running as the tree's owner, and these two
 * functions are how that owner is identified and how the principals that must
 * never be one are refused.
 *
 * The uid is the owner of the repository root, not whoever ran `atlas repo
 * add`. There is no peer to ask: registration is a local write under the
 * data-directory lock with the daemon stopped, so no socket carries a
 * `SO_PEERCRED`, and which uid performs it depends on how Atlas is deployed —
 * the operator's own in a per-user install, the daemon's in a system one. An
 * answer that changes with the deployment is not an identity; the root's owner
 * is the same answer either way.
 */
#ifndef ATLAS_SCANNER_UID_H
#define ATLAS_SCANNER_UID_H

#include "atlas/error.h"

#include <stdint.h>

/* The uid that owns `root`.
 *
 * `lstat`, never following a link on the final component: a root replaced by a
 * symlink since registration must not redirect the question to whatever it
 * points at. A root that cannot be read, or that is not a directory, is an
 * error carrying a message — never a silent 0, which is how the column records
 * "no scanner assigned" and must stay distinguishable from a failed read. */
atlas_status atlas_scanner_uid_of_root(const char *root, int64_t *out, atlas_err *err);

/* Why `uid` may never be a scanner uid, or NULL when it may. The returned
 * string is a static literal.
 *
 * Root is refused everywhere. The orchestration worker, the model dispatcher
 * and the gateway are refused in a *system* deployment only: there they own
 * none of the trees they would report on, and this season is safe precisely
 * because the reporting principal owns the files it reports — whatever it could
 * misreport, it could equally write. In a per-user install the daemon's uid is
 * the operator's uid and does own the tree, so there is nothing to refuse. */
const char *atlas_scanner_uid_refusal(int64_t uid);

#endif /* ATLAS_SCANNER_UID_H */

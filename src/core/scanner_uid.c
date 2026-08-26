/* Atlas - A13: which uid's scanner may report facts about a repository.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0. */
#include "atlas/scanner_uid.h"

#include "atlas/gwpolicy.h"
#include "atlas/orchpolicy.h"
#include "atlas/syspolicy.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

atlas_status atlas_scanner_uid_of_root(const char *root, int64_t *out, atlas_err *err) {
    if (root == NULL || root[0] == '\0') {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a repository root is required");
    }
    struct stat sb;
    memset(&sb, 0, sizeof(sb));
    /* lstat, not stat: a root replaced by a symlink since registration must not
     * redirect the question to whatever it now points at. */
    if (lstat(root, &sb) != 0) {
        return atlas_err_set_errno(err, ATLAS_ERR_REPO, errno,
                                   "cannot read the repository root to learn its owner");
    }
    if (!S_ISDIR(sb.st_mode)) {
        return atlas_err_set(err, ATLAS_ERR_REPO,
                             "the repository root is not a directory, so it has no owner to scan "
                             "as");
    }
    if (out != NULL) {
        *out = (int64_t)sb.st_uid;
    }
    return ATLAS_OK;
}

const char *atlas_scanner_uid_refusal(int64_t uid) {
    /* Refused in every deployment. 0 is how `repositories.scanner_uid` records
     * "no scanner assigned", so accepting uid 0 would make an assignment
     * indistinguishable from its absence — and a column that cannot tell those
     * apart cannot answer the only question it exists for. */
    if (uid == 0) {
        return "uid 0 is how Atlas records \"no scanner assigned\", so it cannot also name one";
    }

    /* The remaining three are about principals that do not own the trees they
     * would report on.
     *
     * This season is safe because the reporting principal owns the files it
     * reports: whatever it could misreport, it could equally write, so no new
     * authority is created. A principal that owns none of the tree breaks that
     * equivalence — its report would be worth more than anything it already
     * has. A7.1's declared adversary is `atlas-worker`, and it is one of them.
     *
     * In a per-user install the daemon's uid *is* the operator's uid and does
     * own the tree, so there is nothing to refuse and this returns NULL rather
     * than forbidding the whole deployment mode. */
    atlas_syspolicy sp;
    atlas_syspolicy_load(&sp);
    if (sp.state != ATLAS_SYSPOLICY_SYSTEM) {
        return NULL;
    }

    atlas_orchpolicy op;
    atlas_orchpolicy_load(&op);
    if (op.dispatcher_uid > 0 && uid == op.dispatcher_uid) {
        return "the orchestration worker owns none of the trees it would report on";
    }

    /* `model_dispatcher_uid` is deliberately **not** refused.
     *
     * A8.1 names it as the one exception to "every persistent or autonomous
     * model process runs as `atlas-worker`", and it is routinely the operator's
     * own uid — it is 1000 on the machine that produced this season, the same
     * uid that owns both registered repositories. Refusing it would refuse
     * exactly the principal A13 exists to let scan. The reason for the refusals
     * above is that those accounts own none of the trees they would report on;
     * that reason does not reach this one. */

    atlas_gwpolicy gp;
    atlas_gwpolicy_load(&gp);
    if (gp.gateway_uid > 0 && uid == gp.gateway_uid) {
        return "the gateway owns none of the trees it would report on";
    }
    return NULL;
}

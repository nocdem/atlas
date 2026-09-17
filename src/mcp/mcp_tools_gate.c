/* Atlas - gate MCP tools.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Shared transport and trust contracts: mcp_tools.c and mcp_tools_internal.h.
 */
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>

#include "atlas/ai.h"
#include "atlas/atlas.h"
#include "atlas/pathrep.h"
#include "mcp/mcp_internal.h"


#include "mcp/mcp_tools_internal.h"

/* --- A6: reading a gate result ----------------------------------------------
 *
 * The whole of A6's model-facing surface, and it is a read.
 *
 * A model may see that a decision has gone stale and why. It may not clear the
 * result, revalidate the decision, override the gate or cache the answer,
 * because none of those operations exists to be exposed: there is no RPC method
 * for any of them, and the one operation that establishes a new validation
 * point needs a capability that only the interactive terminal channel can
 * obtain. A model with shell access can of course run `atlas decision
 * revalidate` — A4 says so plainly and A6 does not weaken it — but it cannot do
 * so *through Atlas' model-facing surface*, which is the property this tool
 * inventory is evidence of.
 *
 * Marked untrusted because the result carries each decision's title, which is
 * project prose. Everything else in it is a closed Atlas vocabulary, an object
 * id or a count. */
atlas_status atlas_mcp_tools_schema_gate(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "decision", "One decision id, to assess only that decision.",
                      (int64_t)ATLAS_DECISION_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "at",
                      "The exact commit to assess against. A state Atlas has not indexed is "
                      "reported as BLOCKED rather than extrapolated.",
                      (int64_t)ATLAS_OID_HEX_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

typedef struct gate_args {
    const char *repo;
    const char *decision;
    const char *at;
} gate_args;

static atlas_status put_gate_args(atlas_json *j, void *ud, atlas_err *err) {
    const gate_args *a = ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->decision != NULL && a->decision[0] != '\0') {
        st = atlas_json_key_str(j, "decision", a->decision, err);
    }
    if (st == ATLAS_OK && a->at != NULL && a->at[0] != '\0') {
        st = atlas_json_key_str(j, "at", a->at, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_gate(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                             bool *degraded, atlas_err *err) {
    gate_args a;
    memset(&a, 0, sizeof a);
    const char *requested = NULL;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    atlas_buf repo = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_resolve_repo(s, requested, &repo, err);
    }
    if (st == ATLAS_OK) {
        a.repo = atlas_buf_cstr(&repo);
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_UID_MAX, &a.decision, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "at", ATLAS_OID_HEX_MAX, &a.at, err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_gate_args, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "gate.check", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_SOURCE), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* Atlas - memory MCP tools.
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

/* --- write tools ----------------------------------------------------------- */

typedef struct record_args {
    const atlas_mcp_server *server;
    const char *repo;
    const char *summary;
    const char *detail;
    const char *unknown_reason;
    const char *title;
    const char *statement;
    const char *rationale;
    const char *confidence;
    const atlas_jsonv *paths;
    bool unknown;
} record_args;

/* A content-derived idempotency key for a recorded decision.
 *
 * `atlas_record_decision` never sent one, so a redelivered tool call produced a
 * second identical record — and, once the A4 bridge existed, a second document
 * with it. A retry is the same client sending the same content again, so the
 * key is derived from exactly that.
 *
 * **The domain is (client identity, session scope, repository, payload).** Every
 * one of those four is load-bearing:
 *
 *   - *client identity* (provider and client name) so a second adapter cannot
 *     collide with Claude Code's records;
 *   - *session scope* — the exact session key, or the typed sessionless marker
 *     — because a repository-wide key would make two sessions that recorded the
 *     same decision collide, and the second would silently become a duplicate
 *     of the first and lose its own attribution;
 *   - *repository*, because **one session is routinely attached to several
 *     repositories**, and the same decision text is a different decision in
 *     each. Session scope alone would let a proposal about R2 be swallowed by
 *     an identical one already recorded against R1. The stored partial index is
 *     `(repo_id, dedup_key)`, so the repository is enforced twice — but the key
 *     carries it too, so a future change to that index cannot silently merge
 *     them;
 *   - *the complete payload*, every field and the paths in order, so changing
 *     any of them is a different proposal.
 *
 * Length-prefixed, not delimiter-joined, for the reason the revision content
 * hash is: a delimiter is a byte the content can contain. With `|` joining, a
 * session key of `a|b` in repository `c` hashes identically to session `a` in
 * repository `b|c`, and a path containing `|` slides across the list boundary.
 * A dedup collision *loses a record*, so this is not a theoretical concern.
 *
 * What it cannot do, and does not claim to: distinguish a transport retry from
 * a deliberate second identical proposal. Two byte-identical decisions from one
 * session about one repository are indistinguishable by content, and Atlas
 * treats them as one. */
static void dedup_field(atlas_sha256 *ctx, const char *data, size_t len) {
    unsigned char lenbuf[8];
    for (size_t i = 0; i < 8u; i++) {
        lenbuf[i] = (unsigned char)((((uint64_t)len) >> (8u * (7u - i))) & 0xFFu);
    }
    atlas_sha256_update(ctx, lenbuf, sizeof(lenbuf));
    atlas_sha256_update(ctx, data != NULL ? data : "", data != NULL ? len : 0u);
}

static void dedup_str(atlas_sha256 *ctx, const char *s) {
    dedup_field(ctx, s, s != NULL ? strlen(s) : 0u);
}

static atlas_status put_decision_dedup(atlas_json *j, const record_args *a, atlas_err *err) {
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    static const char domain[] = "atlas.a2.decision.dedup.v2";
    atlas_sha256_update(&ctx, domain, sizeof(domain));

    /* Client identity. Constants today, named explicitly so a second adapter
     * has to change them deliberately. */
    dedup_str(&ctx, "anthropic");
    dedup_str(&ctx, "claude-code");

    /* Session scope, with the sessionless case typed rather than empty. A
     * generic MCP client supplies no session id at all; that is a different
     * scope from "a session whose key happens to be empty", which cannot occur
     * because `atlas_mcp_session_id_valid` refuses it. */
    if (a->server->session_key.len > 0) {
        dedup_str(&ctx, "session");
        dedup_field(&ctx, a->server->session_key.data, a->server->session_key.len);
    } else {
        dedup_str(&ctx, "sessionless");
        dedup_field(&ctx, "", 0u);
    }

    /* Repository. A single client session is routinely attached to several
     * repositories, so without this an identical proposal about a second one
     * would be absorbed as a retry of the first. The store enforces the same
     * scope independently — `idx_ai_decisions_dedup` is UNIQUE over
     * `(repo_id, dedup_key)` — and either alone is sufficient; both are here
     * so the property does not depend on which layer someone changes next.
     * `tests/test_decision_bridge.c` removes both and shows the record vanish. */
    dedup_str(&ctx, a->repo);

    /* The payload, every field. */
    dedup_str(&ctx, a->title);
    dedup_str(&ctx, a->statement);
    dedup_str(&ctx, a->rationale);

    /* The paths, in order, with the count first so that no list can encode as a
     * different one. */
    size_t n = 0;
    if (a->paths != NULL) {
        n = atlas_jsonv_arr_len(a->paths);
        if (n > (size_t)ATLAS_AI_MAX_PATHS_PER_RECORD) {
            n = (size_t)ATLAS_AI_MAX_PATHS_PER_RECORD;
        }
    }
    char count[32];
    int cn = snprintf(count, sizeof(count), "%zu", n);
    dedup_field(&ctx, count, cn > 0 ? (size_t)cn : 0u);
    for (size_t i = 0; i < n; i++) {
        const char *p = NULL;
        size_t len = 0;
        if (atlas_jsonv_str(atlas_jsonv_at(a->paths, i), &p, &len)) {
            dedup_field(&ctx, p, len);
        } else {
            dedup_field(&ctx, "", 0u);
        }
    }

    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_hex_encode(digest, sizeof(digest), hex);
    return atlas_json_key_str(j, "dedup_key", hex, err);
}

static atlas_status put_record(atlas_json *j, void *ud, atlas_err *err) {
    record_args *a = (record_args *)ud;
    atlas_status st = atlas_mcp_tools_put_identity(j, a->server, a->repo, err);
    if (st == ATLAS_OK) {
        /* Always a proposal. There is no argument that makes it anything else:
         * the field is written here from a constant, not forwarded from the
         * caller, so a tool call claiming an approval cannot produce one. */
        st = atlas_json_key_str(j, "provenance",
                                atlas_provenance_name(a->unknown ? ATLAS_PROV_UNKNOWN
                                                                 : ATLAS_PROV_MODEL_PROPOSAL),
                                err);
    }
    if (st == ATLAS_OK && a->unknown) {
        st = atlas_json_key_bool(j, "unknown", true, err);
    }
    if (st == ATLAS_OK && a->summary != NULL) {
        st = atlas_json_key_str(j, "summary", a->summary, err);
    }
    if (st == ATLAS_OK && a->detail != NULL) {
        st = atlas_json_key_str(j, "detail", a->detail, err);
    }
    if (st == ATLAS_OK && a->unknown_reason != NULL) {
        st = atlas_json_key_str(j, "unknown_reason", a->unknown_reason, err);
    }
    if (st == ATLAS_OK && a->title != NULL) {
        st = atlas_json_key_str(j, "title", a->title, err);
    }
    if (st == ATLAS_OK && a->statement != NULL) {
        st = atlas_json_key_str(j, "statement", a->statement, err);
    }
    if (st == ATLAS_OK && a->rationale != NULL) {
        st = atlas_json_key_str(j, "rationale", a->rationale, err);
    }
    /* Decisions only. A reason and an unknown-reason are cheap, frequent and
     * legitimately repeatable — the same path can be changed twice in a session
     * for two different reasons — and collapsing those would lose real records.
     * A decision is deliberate and rare, so the same content twice in one
     * session is a redelivery. */
    if (st == ATLAS_OK && a->title != NULL && a->statement != NULL) {
        st = put_decision_dedup(j, a, err);
    }
    if (st == ATLAS_OK && a->confidence != NULL) {
        st = atlas_json_key_str(j, "confidence", a->confidence, err);
    }
    if (st == ATLAS_OK && a->paths != NULL) {
        st = atlas_json_key(j, "paths", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        size_t n = atlas_jsonv_arr_len(a->paths);
        for (size_t i = 0; st == ATLAS_OK && i < n && i < (size_t)ATLAS_AI_MAX_PATHS_PER_RECORD;
             i++) {
            const char *p = NULL;
            if (atlas_jsonv_str(atlas_jsonv_at(a->paths, i), &p, NULL) && p[0] != '/') {
                st = atlas_json_str(j, p, err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

/* Validates the `paths` argument before anything is sent. */
static atlas_status check_paths(const atlas_jsonv *paths, bool required, atlas_err *err) {
    size_t n = atlas_jsonv_arr_len(paths);
    if (required && n == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "\"paths\" must name at least one repository-relative path");
    }
    if (n > (size_t)ATLAS_AI_MAX_PATHS_PER_RECORD) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "\"paths\" holds more than %d entries",
                             ATLAS_AI_MAX_PATHS_PER_RECORD);
    }
    for (size_t i = 0; i < n; i++) {
        const char *p = NULL;
        if (!atlas_jsonv_str(atlas_jsonv_at(paths, i), &p, NULL)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "\"paths\" entry %zu is not a string", i);
        }
        if (p[0] == '/') {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "\"paths\" entry %zu is absolute; Atlas records "
                                 "repository-relative paths only",
                                 i);
        }
        atlas_buf decoded = ATLAS_BUF_INIT;
        atlas_status st = atlas_path_text_decode(p, strlen(p), &decoded, err);
        if (st == ATLAS_OK) {
            st = atlas_path_check_relative(decoded.data, decoded.len, err);
        }
        atlas_buf_free(&decoded);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

atlas_status atlas_mcp_tools_schema_reason(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"paths", "summary", NULL};
    static const char *const CONF[] = {"low", "medium", "high", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_paths(j, "the repository-relative paths this reason explains", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "summary",
                      "one sentence saying why these paths were changed. Record what you "
                      "actually did and why; if you do not know, use "
                      "atlas_record_unknown_reason instead of guessing.",
                      ATLAS_AI_SUMMARY_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "detail", "optional additional explanation", ATLAS_AI_DETAIL_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "confidence", "how sure you are. Reported, never acted on.", CONF, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_reason(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                               bool *degraded, atlas_err *err) {
    record_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    const char *requested = NULL;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_resolve_repo(s, requested, &repo, err);
    }
    if (st == ATLAS_OK) {
        a.repo = atlas_buf_cstr(&repo);
        a.paths = atlas_jsonv_get(args, "paths");
        st = check_paths(a.paths, true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "summary", ATLAS_AI_SUMMARY_MAX, &a.summary, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "detail", ATLAS_AI_DETAIL_MAX, &a.detail, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "confidence", 16u, &a.confidence, err);
    }
    if (st == ATLAS_OK && a.summary == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "\"summary\" is required. If there is no truthful reason to give, call "
                           "atlas_record_unknown_reason instead of inventing one.");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_record, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "ai.reason.record", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_unknown(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"paths", "why", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_paths(j, "the repository-relative paths with no known reason", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "why",
                      "why the reason is unknown, for example 'changed by a build step' or "
                      "'pre-existing edit not made in this session'. This is not a guess at the "
                      "reason; it is a statement that there is not one.",
                      ATLAS_AI_SUMMARY_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_unknown(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                bool *degraded, atlas_err *err) {
    record_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    a.unknown = true;
    const char *requested = NULL;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_resolve_repo(s, requested, &repo, err);
    }
    if (st == ATLAS_OK) {
        a.repo = atlas_buf_cstr(&repo);
        a.paths = atlas_jsonv_get(args, "paths");
        st = check_paths(a.paths, true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "why", ATLAS_AI_SUMMARY_MAX, &a.unknown_reason, err);
    }
    if (st == ATLAS_OK && a.unknown_reason == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"why\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_record, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "ai.reason.record", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_UNKNOWN), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_schema_decision(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"title", "statement", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "title", "a short name for the decision", ATLAS_AI_SUMMARY_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "statement", "what was decided, in one or two sentences",
                      ATLAS_AI_DETAIL_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "rationale", "why this was chosen over the alternatives",
                      ATLAS_AI_DETAIL_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_paths(j, "the repository-relative paths this decision concerns", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_decision(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                 bool *degraded, atlas_err *err) {
    record_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    const char *requested = NULL;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_resolve_repo(s, requested, &repo, err);
    }
    if (st == ATLAS_OK) {
        a.repo = atlas_buf_cstr(&repo);
        a.paths = atlas_jsonv_get(args, "paths");
        st = check_paths(a.paths, false, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "title", ATLAS_AI_SUMMARY_MAX, &a.title, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "statement", ATLAS_AI_DETAIL_MAX, &a.statement, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "rationale", ATLAS_AI_DETAIL_MAX, &a.rationale, err);
    }
    if (st == ATLAS_OK && (a.title == NULL || a.statement == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"title\" and \"statement\" are both required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_record, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "ai.decision.record", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}


/* --- A4: decision documents, with progressive disclosure --------------------
 *
 * Four tools, in the order a caller uses them:
 *
 *   1. `atlas_decisions` — a compact search or listing: ids, status,
 *      provenance and bounded metadata. Titles only, never bodies.
 *   2. `atlas_decision_history` — the timeline of one decision.
 *   3. `atlas_decision` — the full text of one revision, on request.
 *   4. `atlas_propose_decision` — record a proposal.
 *
 * The split is not politeness about response size. A decision body is untrusted
 * prose, and pulling every body in a repository into a model's context because
 * it asked "are there any decisions about auth?" would put a large amount of
 * text somebody else wrote in front of the model for no reason. A caller reads
 * a body when it has decided it needs that one.
 *
 * **There is no approval tool, no rejection tool and no supersession tool, and
 * no tool here accepts a `token` or a `confirmation` argument.** A lifecycle
 * transition needs a capability that only the interactive CLI can obtain, and
 * the absence is structural rather than guarded: the schemas below set
 * `additionalProperties: false` and declare every argument, so there is no
 * member a caller could add. `tests/test_decision_mcp.c` asserts the whole tool
 * inventory and that no schema mentions either word. */

static atlas_status prop_decision_id(atlas_json *j, atlas_err *err) {
    return atlas_mcp_tools_prop_str(j, "decision",
                    "the decision id, as returned by atlas_decisions "
                    "(atlas-dec- followed by 16 hex characters)",
                    ATLAS_DECISION_UID_MAX, err);
}

/* A9.1. The knowledge-kind vocabulary, as a schema enum.
 *
 * Spelled out rather than generated, because `prop_enum` takes a
 * NULL-terminated array of literals and a schema is a contract a client caches.
 * `tests/test_decision_kind.c` checks this list against
 * `atlas_decision_kind_name` member by member, so it cannot drift. */
static const char *const KIND_ENUM[] = {
    "DECISION", "POLICY", "INVARIANT", "OPERATIONAL_FACT", "ACCEPTED_RISK",
    "OBLIGATION", "PARKED", "REJECTED_ALTERNATIVE", NULL,
};

/* The one description of the dimension a model sees, in one place, so the list
 * tool, the propose tool and the revise tool cannot describe it differently. */
static const char KIND_HELP[] =
    "what sort of durable knowledge this is, independent of its lifecycle status. DECISION "
    "(the default) is a choice that sets direction; POLICY is a rule about process; INVARIANT is "
    "a property implementations must preserve; OPERATIONAL_FACT describes what is currently "
    "deployed and carries no permanence; ACCEPTED_RISK is a risk somebody proposes accepting, and "
    "proposing one accepts nothing; OBLIGATION is required future work; PARKED is deliberately "
    "deferred, which is not rejected; REJECTED_ALTERNATIVE is an approach tried and rejected, "
    "recorded with why. A record's kind can never be changed by a later revision.";

atlas_status atlas_mcp_tools_schema_decisions(atlas_json *j, atlas_err *err) {
    static const char *const STATUSES[] = {"PROPOSED",   "APPROVED", "REJECTED",
                                           "SUPERSEDED", "RESOLVED", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "query", "words to look for in the decision text",
                      ATLAS_DECISION_QUERY_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "path", "a repository-relative path; lists decisions concerning it",
                      4096u, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "status", "only decisions in this lifecycle state", STATUSES, err);
    }
    /* Two filters, not one. `status` says how far through the approval workflow a
     * record got and `kind` says what sort of record it is; a model asking for
     * approved invariants needs both and neither implies the other. */
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "kind", KIND_HELP, KIND_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "how many to return", 1, ATLAS_MCP_MAX_ROWS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, NULL, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_decision_one(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"decision", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = prop_decision_id(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "revision",
                      "which revision to read; omit for the one that is currently effective", 1,
                      ATLAS_DECISION_MAX_REVISIONS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_decision_history(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"decision", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = prop_decision_id(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_propose_decision(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"title", "decision", NULL};
    static const char *const SCOPES[] = {"REPOSITORY", "SUBSYSTEM", "PATHS", "UNKNOWN", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "kind", KIND_HELP, KIND_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "title", "a short name for the decision", ATLAS_DECISION_TITLE_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "decision", "what was decided", ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "context", "the problem or situation that prompted it",
                      ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "rationale",
                      "why this was chosen. Record UNKNOWN rather than inventing one",
                      ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "consequences", "what follows from it", ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "scope", "how broad the decision is", SCOPES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str_array(j, "alternatives", "the alternatives that were considered",
                            ATLAS_DECISION_MAX_ALTERNATIVES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_paths(j, "the repository-relative paths this decision concerns", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str_array(j, "symbols", "the symbol names this decision concerns",
                            ATLAS_DECISION_MAX_LINKS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

typedef struct decision_args {
    atlas_mcp_server *server;
    const char *repo;
    const char *decision;
    const char *query;
    const char *path;
    const char *status;
    const char *title;
    const char *body;
    const char *context;
    const char *rationale;
    const char *consequences;
    const char *scope;
    /* A9.1. Forwarded verbatim after the daemon validates it against the
     * vocabulary — the MCP layer opens no database handle and is not the
     * authority on what a kind is. */
    const char *kind;
    const atlas_jsonv *paths;
    const atlas_jsonv *symbols;
    const atlas_jsonv *alternatives;
    int64_t limit;
    int64_t revision;
} decision_args;

static atlas_status put_str_array(atlas_json *j, const char *key, const atlas_jsonv *arr,
                                  atlas_err *err) {
    if (arr == NULL || !atlas_jsonv_is_arr(arr)) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_json_key(j, key, err);
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    size_t n = atlas_jsonv_arr_len(arr);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        const char *s = NULL;
        size_t len = 0;
        if (atlas_jsonv_str(atlas_jsonv_at(arr, i), &s, &len)) {
            st = atlas_json_str(j, s, err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    return st;
}

static atlas_status put_decision_query(atlas_json *j, void *ud, atlas_err *err) {
    decision_args *a = (decision_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->query != NULL) {
        st = atlas_json_key_str(j, "query", a->query, err);
    }
    if (st == ATLAS_OK && a->path != NULL) {
        st = atlas_json_key_str(j, "path", a->path, err);
    }
    if (st == ATLAS_OK && a->status != NULL) {
        st = atlas_json_key_str(j, "status", a->status, err);
    }
    if (st == ATLAS_OK && a->kind != NULL) {
        st = atlas_json_key_str(j, "kind", a->kind, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    return st;
}

static atlas_status put_decision_one(atlas_json *j, void *ud, atlas_err *err) {
    decision_args *a = (decision_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "decision", a->decision, err);
    }
    if (st == ATLAS_OK && a->revision > 0) {
        st = atlas_json_key_int(j, "revision", a->revision, err);
    }
    return st;
}

static atlas_status put_decision_propose(atlas_json *j, void *ud, atlas_err *err) {
    decision_args *a = (decision_args *)ud;
    atlas_status st = atlas_mcp_tools_put_identity(j, a->server, a->repo, err);
    if (st == ATLAS_OK) {
        /* Written here from a constant, never forwarded from the caller. A tool
         * call claiming to be an operator cannot produce one, for the same
         * reason `put_record` pins the A2 provenance. */
        st = atlas_json_key_str(j, "actor",
                                atlas_decision_actor_name(ATLAS_DECISION_ACTOR_MODEL_PROPOSAL),
                                err);
    }
    struct {
        const char *key;
        const char *value;
    } fields[] = {
        {"title", a->title},         {"decision", a->body},
        {"context", a->context},     {"rationale", a->rationale},
        {"consequences", a->consequences}, {"scope", a->scope},
        {"kind", a->kind},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i].value != NULL) {
            st = atlas_json_key_str(j, fields[i].key, fields[i].value, err);
        }
    }
    if (st == ATLAS_OK) {
        st = put_str_array(j, "paths", a->paths, err);
    }
    if (st == ATLAS_OK) {
        st = put_str_array(j, "symbols", a->symbols, err);
    }
    if (st == ATLAS_OK) {
        st = put_str_array(j, "alternatives", a->alternatives, err);
    }
    return st;
}

/* The three read tools and the write tool share their argument plumbing: pick
 * the arguments, resolve the repository against the client's granted roots, and
 * forward. */
static atlas_status decision_common(atlas_mcp_server *s, const atlas_jsonv *args,
                                    decision_args *a, atlas_buf *repo, atlas_err *err) {
    memset(a, 0, sizeof(*a));
    a->server = s;
    a->limit = ATLAS_DECISION_DEFAULT_ROWS;
    const char *requested = NULL;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st == ATLAS_OK) {
        /* A whitelist, not a path comparison: the repository must be one that a
         * granted root resolved to. */
        st = atlas_mcp_resolve_repo(s, requested, repo, err);
    }
    if (st == ATLAS_OK) {
        a->repo = atlas_buf_cstr(repo);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_decisions(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                  bool *degraded, atlas_err *err) {
    decision_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = decision_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "query", ATLAS_DECISION_QUERY_MAX, &a.query, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_rel_path(args, "path", &a.path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "status", 32u, &a.status, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "kind", 32u, &a.kind, err);
    }
    if (st == ATLAS_OK) {
        a.limit = atlas_mcp_tools_arg_int(args, "limit", ATLAS_DECISION_DEFAULT_ROWS);
        if (a.limit <= 0 || a.limit > ATLAS_MCP_MAX_ROWS) {
            a.limit = ATLAS_MCP_MAX_ROWS;
        }
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_decision_query, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        /* Untrusted: the titles in the result are prose somebody else wrote,
         * and an APPROVED status does not change that. */
        st = atlas_mcp_tools_forward(s, "decision.list", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_decision_get(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                     bool *degraded, atlas_err *err) {
    decision_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = decision_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_UID_MAX, &a.decision, err);
    }
    if (st == ATLAS_OK && a.decision == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"decision\" is required");
    }
    if (st == ATLAS_OK) {
        a.revision = atlas_mcp_tools_arg_int(args, "revision", 0);
        if (a.revision < 0 || a.revision > ATLAS_DECISION_MAX_REVISIONS) {
            st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"revision\" is out of range");
        }
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_decision_one, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "decision.get", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_decision_history(atlas_mcp_server *s, const atlas_jsonv *args,
                                         atlas_buf *body, bool *degraded, atlas_err *err) {
    decision_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = decision_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_UID_MAX, &a.decision, err);
    }
    if (st == ATLAS_OK && a.decision == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"decision\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_decision_one, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "decision.history", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

atlas_status atlas_mcp_tools_run_propose_decision(atlas_mcp_server *s, const atlas_jsonv *args,
                                         atlas_buf *body, bool *degraded, atlas_err *err) {
    decision_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = decision_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "title", ATLAS_DECISION_TITLE_MAX, &a.title, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_TEXT_MAX, &a.body, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "context", ATLAS_DECISION_TEXT_MAX, &a.context, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "rationale", ATLAS_DECISION_TEXT_MAX, &a.rationale, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "consequences", ATLAS_DECISION_TEXT_MAX, &a.consequences, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "scope", 32u, &a.scope, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "kind", 32u, &a.kind, err);
    }
    if (st == ATLAS_OK && (a.title == NULL || a.body == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"title\" and \"decision\" are both required");
    }
    if (st == ATLAS_OK) {
        a.paths = atlas_jsonv_get(args, "paths");
        st = check_paths(a.paths, false, err);
    }
    if (st == ATLAS_OK) {
        a.symbols = atlas_jsonv_get(args, "symbols");
        a.alternatives = atlas_jsonv_get(args, "alternatives");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_decision_propose, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "decision.propose", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* --- A9.1: revising an existing record ------------------------------------
 *
 * **This closes a surface gap rather than granting authority, and the difference
 * is the whole justification.**
 *
 * The gap: `decision.revise` has existed since A4 and writes a new PROPOSED
 * revision by a MODEL_PROPOSAL actor — exactly what `atlas_propose_decision`
 * writes, differing only in whether a document already exists. MCP could express
 * the second and not the first, so a model that noticed an approved record was
 * now wrong could only write a *new* record beside it, leaving two documents
 * about one subject and no relation between them. That is a worse record than
 * the one the model was trying to improve, and nothing about it was a security
 * boundary: proposing a revision changes no lifecycle state, and the operator
 * still has to approve it before it means anything.
 *
 * What is deliberately still absent: approve, reject, supersede, revalidate and
 * resolve. Those are in the operator-uid RPC group, need a capability only the
 * terminal channel can obtain, and have no tool here — so a model can now say
 * "this should change" and still cannot make it change. `tests/test_decision_mcp.c`
 * asserts the whole tool inventory and rejects any tool name containing an
 * approval verb.
 *
 * The kind is *not* an argument. A revision cannot reclassify a document, so
 * offering the field would offer a request that is always refused. */
atlas_status atlas_mcp_tools_schema_revise_decision(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"decision", "title", "decision_text", NULL};
    static const char *const SCOPES[] = {"REPOSITORY", "SUBSYSTEM", "PATHS", "UNKNOWN", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = prop_decision_id(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "title", "a short name for the revised record", ATLAS_DECISION_TITLE_MAX,
                      err);
    }
    /* `decision` is the document id here, so the prose needs its own name. That
     * is the A8.2 rule: one key, one meaning. */
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "decision_text", "what the record now says", ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "context", "the problem or situation, as it now stands",
                      ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "rationale",
                      "why the record now says this. Record UNKNOWN rather than inventing one",
                      ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "consequences", "what follows from it", ATLAS_DECISION_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "scope", "how broad the record is", SCOPES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str_array(j, "alternatives", "the alternatives that were considered",
                            ATLAS_DECISION_MAX_ALTERNATIVES, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_paths(j, "the repository-relative paths this record concerns", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str_array(j, "symbols", "the symbol names this record concerns",
                            ATLAS_DECISION_MAX_LINKS, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

static atlas_status put_decision_revise(atlas_json *j, void *ud, atlas_err *err) {
    decision_args *a = (decision_args *)ud;
    atlas_status st = atlas_mcp_tools_put_identity(j, a->server, a->repo, err);
    if (st == ATLAS_OK) {
        /* From a constant, exactly as the propose path does it: a tool call
         * cannot claim to be an operator. */
        st = atlas_json_key_str(j, "actor",
                                atlas_decision_actor_name(ATLAS_DECISION_ACTOR_MODEL_PROPOSAL),
                                err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "decision_uid", a->decision, err);
    }
    struct {
        const char *key;
        const char *value;
    } fields[] = {
        {"title", a->title},           {"decision_body", a->body},
        {"context", a->context},       {"rationale", a->rationale},
        {"consequences", a->consequences}, {"scope", a->scope},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i].value != NULL) {
            st = atlas_json_key_str(j, fields[i].key, fields[i].value, err);
        }
    }
    if (st == ATLAS_OK) {
        st = put_str_array(j, "paths", a->paths, err);
    }
    if (st == ATLAS_OK) {
        st = put_str_array(j, "symbols", a->symbols, err);
    }
    if (st == ATLAS_OK) {
        st = put_str_array(j, "alternatives", a->alternatives, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_revise_decision(atlas_mcp_server *s, const atlas_jsonv *args,
                                        atlas_buf *body, bool *degraded, atlas_err *err) {
    decision_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = decision_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_UID_MAX, &a.decision, err);
    }
    if (st == ATLAS_OK && a.decision == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"decision\" is required");
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "title", ATLAS_DECISION_TITLE_MAX, &a.title, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision_text", ATLAS_DECISION_TEXT_MAX, &a.body, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "context", ATLAS_DECISION_TEXT_MAX, &a.context, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "rationale", ATLAS_DECISION_TEXT_MAX, &a.rationale, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "consequences", ATLAS_DECISION_TEXT_MAX, &a.consequences, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "scope", 32u, &a.scope, err);
    }
    if (st == ATLAS_OK && (a.title == NULL || a.body == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "\"title\" and \"decision_text\" are both required");
    }
    if (st == ATLAS_OK) {
        a.paths = atlas_jsonv_get(args, "paths");
        st = check_paths(a.paths, false, err);
    }
    if (st == ATLAS_OK) {
        a.symbols = atlas_jsonv_get(args, "symbols");
        a.alternatives = atlas_jsonv_get(args, "alternatives");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_decision_revise, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "decision.revise", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

/* --- the tool table -------------------------------------------------------- */

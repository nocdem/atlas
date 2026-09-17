/* Atlas - verify MCP tools.
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

/* --- A9.2.1: the verification workflow ------------------------------------
 *
 * A9.2 shipped an engine that weighs evidence and a root-owned policy that may
 * act on the result, and shipped no way for a model to put evidence in. These
 * eight tools are that way in. What they add up to is: a model may *say what it
 * looked at and what it concluded*, and may ask Atlas to look for itself.
 *
 * Three separations are load-bearing here, and every one of them is enforced at
 * the daemon rather than by this file being careful:
 *
 *   1. **A model may reference evidence; it may not have produced it.**
 *      `atlas_verify_evidence_add` records what the caller read. The four
 *      classes whose entire weight comes from Atlas having *performed* the
 *      act — COMPILER, TEST, RUNTIME, DEPLOYED_CONFIG — are refused on this
 *      path, not discounted, because a discounted forgery still reads as tool
 *      output to somebody skimming a UI. `atlas_verify_evidence_produce` is the
 *      honest route: Atlas runs a named allowlisted verifier and records
 *      whatever it concluded. There is no parameter for the verdict and there
 *      must never be one.
 *
 *   2. **The actor's class is not something the caller says.** Every field this
 *      file sends about the speaker is ASSERTED metadata and is stored as
 *      asserted; the row's `identity` column says so. The one field that
 *      decides how much an attestation is *worth* — the channel — is sent as
 *      `speaking_for: MODEL`, which the daemon honours only because it asserts
 *      *less* than the peer uid would. §10.
 *
 *   3. **Intake is not authority.** Nothing here approves, rejects,
 *      supersedes, resolves or revalidates; nothing mints or spends a warrant;
 *      nothing edits the root-owned policy or lowers a threshold. Those are
 *      absent rather than refused, which is A7's pattern and is what a later
 *      edit cannot weaken.
 *
 * `atlas_verify_evaluate` may cause Atlas to transition a lifecycle state on
 * its own authority. That is not the caller acquiring one: the gates are in a
 * root-owned file this peer cannot read, a deterministic verdict needs an
 * Atlas-attested verifier a model cannot forge, and the empirical path stays in
 * shadow without calibration nothing here can supply. */

/* The evidence classes a model may name. The four that Atlas must have
 * performed are deliberately absent — see `schema_verify_evidence_add`. Their
 * omission is a convenience for whoever reads the schema; the refusal that
 * matters is `atlas_verify_evidence_class_requires_atlas_production` at the
 * write point, which answers the same whether the name was offered here or
 * guessed. */
static const char *const EVIDENCE_ENUM[] = {
    "SOURCE_CODE", "GIT_HISTORY",     "SPECIFICATION",  "DOCUMENT",
    "AI_ANALYSIS", "ATLAS_KNOWLEDGE", "HUMAN_STATEMENT", NULL,
};
static const char *const VERDICT_ENUM[] = {"SUPPORT", "CONTRADICT", "INCONCLUSIVE", NULL};
static const char *const SEMANTICS_ENUM[] = {"DESCRIPTIVE", "NORMATIVE", NULL};
static const char *const VERIFIER_ENUM[] = {
    "atlas.content_hash", "atlas.symbol_present", "atlas.symbol_absent", "atlas.proven_edge", NULL,
};

typedef struct verify_args {
    atlas_mcp_server *server;
    const char *repo;
    const char *claim;
    const char *decision;
    const char *domain;
    const char *text;
    const char *scope;
    const char *semantics;
    const char *verifier;
    const char *verifier_input;
    const char *commit;
    const char *environment;
    const char *evidence_class;
    const char *path;
    const char *symbol;
    const char *target;
    const char *probe;
    const char *observed;
    const char *observed_at;
    const char *verdict;
    const char *method;
    const char *evidence;
    const char *supersedes;
    const char *derives_from;
    /* §11. Everything here is the speaker describing itself, which is exactly
     * as trustworthy as that sounds. Forwarded so "which actor was this?" is
     * answerable; never an input to a verdict. */
    const char *actor;
    const char *provider;
    const char *family;
    const char *model_version;
    const char *role;
    const char *run;
    const char *orchestrator;
    int64_t claim_id;
    int64_t limit;
    int64_t line_start;
    int64_t line_end;
    int64_t self_confidence;
    bool has_self_confidence;
} verify_args;

/* The description every schema shares for the self-description block, in one
 * place so three tools cannot describe the same fields differently. */
static const char ACTOR_HELP[] =
    "who you are, as you describe yourself. Atlas stores this as ASSERTED, never authenticated: "
    "the transport carries no cryptographic statement about which model is speaking, and Atlas "
    "will not pretend otherwise. It identifies the actor for later reading; it changes no weight.";

static atlas_status prop_actor(atlas_json *j, atlas_err *err) {
    atlas_status st = atlas_mcp_tools_prop_str(j, "actor", ACTOR_HELP, ATLAS_VERIFY_NAME_MAX, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "provider", "who runs the model, as you describe it (ASSERTED)",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "family", "the model family, as you describe it (ASSERTED)",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "model_version", "the model version, as you describe it (ASSERTED)",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "role", "what you were doing when you formed this view (ASSERTED)",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "run", "an id for this run, if you have one (ASSERTED)",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "orchestrator",
                      "the actor that dispatched you, if any. Say so when you are one of several "
                      "agents a coordinator spawned: subagents of one orchestrator reading one "
                      "document are not independent corroboration, and this is how Atlas can "
                      "know that (ASSERTED)",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    return st;
}

static atlas_status take_verify_actor(const atlas_jsonv *args, verify_args *a, atlas_err *err) {
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"actor", offsetof(verify_args, actor)},
        {"provider", offsetof(verify_args, provider)},
        {"family", offsetof(verify_args, family)},
        {"model_version", offsetof(verify_args, model_version)},
        {"role", offsetof(verify_args, role)},
        {"run", offsetof(verify_args, run)},
        {"orchestrator", offsetof(verify_args, orchestrator)},
    };
    for (size_t i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        atlas_status st = atlas_mcp_tools_arg_str(args, FIELDS[i].key, ATLAS_VERIFY_NAME_MAX,
                                  (const char **)((char *)a + FIELDS[i].off), err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* §10. `speaking_for` is the only field in any of these requests that changes
 * how much an attestation counts, and it is sent as the *weakest* value there
 * is. The daemon honours it only downwards — a request cannot raise its own
 * channel, and cannot name ATLAS at all — so this line can only ever make Atlas
 * trust the caller less than the kernel would have. That is correct: whatever
 * uid carried it, an MCP tool call is a model speaking. */
static atlas_status put_verify_actor(atlas_json *j, verify_args *a, atlas_err *err) {
    atlas_status st = atlas_json_key_str(j, "speaking_for", "MODEL", err);
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"actor", offsetof(verify_args, actor)},
        {"provider", offsetof(verify_args, provider)},
        {"family", offsetof(verify_args, family)},
        {"model_version", offsetof(verify_args, model_version)},
        {"role", offsetof(verify_args, role)},
        {"run", offsetof(verify_args, run)},
        {"orchestrator", offsetof(verify_args, orchestrator)},
    };
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        const char *v = *(const char **)((char *)a + FIELDS[i].off);
        if (v != NULL) {
            st = atlas_json_key_str(j, FIELDS[i].key, v, err);
        }
    }
    /* The session comes from the server process, not from the model: it is the
     * one identifying field here the caller did not choose. */
    if (st == ATLAS_OK && a->server->session_key.len > 0) {
        st = atlas_json_key_str(j, "session", atlas_buf_cstr(&a->server->session_key), err);
    }
    return st;
}

static atlas_status verify_common(atlas_mcp_server *s, const atlas_jsonv *args, verify_args *a,
                                  atlas_buf *repo, atlas_err *err) {
    memset(a, 0, sizeof(*a));
    a->server = s;
    const char *requested = NULL;
    atlas_status st = atlas_mcp_tools_arg_str(args, "repo", ATLAS_NAME_MAX, &requested, err);
    if (st == ATLAS_OK) {
        /* A whitelist, not a path comparison — A2's rule, unchanged. */
        st = atlas_mcp_resolve_repo(s, requested, repo, err);
    }
    if (st == ATLAS_OK) {
        a->repo = atlas_buf_cstr(repo);
        st = take_verify_actor(args, a, err);
    }
    return st;
}

static atlas_status put_verify_claim_create(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"decision", offsetof(verify_args, decision)},
        {"domain", offsetof(verify_args, domain)},
        {"text", offsetof(verify_args, text)},
        {"scope", offsetof(verify_args, scope)},
        {"semantics", offsetof(verify_args, semantics)},
        {"verifier", offsetof(verify_args, verifier)},
        {"verifier_input", offsetof(verify_args, verifier_input)},
        {"commit", offsetof(verify_args, commit)},
        {"environment", offsetof(verify_args, environment)},
    };
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        const char *v = *(const char **)((char *)a + FIELDS[i].off);
        if (v != NULL) {
            st = atlas_json_key_str(j, FIELDS[i].key, v, err);
        }
    }
    if (st == ATLAS_OK) {
        st = put_verify_actor(j, a, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_claim_create(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"text", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "text",
                      "the proposition, stated so that it could be shown false. \"the parser "
                      "rejects a negative length\" is a claim; \"the parser is robust\" is not",
                      ATLAS_VERIFY_CLAIM_TEXT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "semantics",
                       "DESCRIPTIVE observes what is; NORMATIVE declares what ought to be. "
                       "Defaults to DESCRIPTIVE. A deterministic verifier may never establish a "
                       "NORMATIVE claim: no amount of evidence about the code decides what the "
                       "project should choose",
                       SEMANTICS_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "decision", "the knowledge record this claim bears on, if any",
                      ATLAS_DECISION_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "domain", "a short subject label", ATLAS_VERIFY_DOMAIN_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "scope",
                      "the conditions under which the claim is asserted. Two statements that "
                      "disagree only because they describe different scopes are not a "
                      "contradiction, and this is what lets Atlas tell",
                      ATLAS_VERIFY_SCOPE_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "verifier",
                       "the allowlisted verifier that mechanically decides this claim, if one "
                       "does. Naming it does not run it; call atlas_verify_evidence_produce",
                       VERIFIER_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "verifier_input", "that verifier's bounded argument",
                      ATLAS_VERIFY_VERIFIER_INPUT_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "commit",
                      "the commit the claim is about. Atlas records this and compares it with "
                      "the commit an evaluation actually examined, so a result can never quietly "
                      "become a statement about a tree the repository has left",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "environment", "the deployment a runtime claim is about",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = prop_actor(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_claim_create(atlas_mcp_server *s, const atlas_jsonv *args,
                                            atlas_buf *body, bool *degraded, atlas_err *err) {
    verify_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = verify_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "text", ATLAS_VERIFY_CLAIM_TEXT_MAX, &a.text, err);
    }
    if (st == ATLAS_OK && a.text == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"text\" is required: a claim is a proposition");
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_UID_MAX, &a.decision, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "domain", ATLAS_VERIFY_DOMAIN_MAX, &a.domain, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "scope", ATLAS_VERIFY_SCOPE_MAX, &a.scope, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "semantics", 32u, &a.semantics, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "verifier", ATLAS_VERIFY_NAME_MAX, &a.verifier, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "verifier_input", ATLAS_VERIFY_VERIFIER_INPUT_MAX, &a.verifier_input,
                     err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "commit", ATLAS_VERIFY_NAME_MAX, &a.commit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "environment", ATLAS_VERIFY_NAME_MAX, &a.environment, err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_claim_create, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.claim_create", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

static atlas_status put_verify_evidence_add(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"claim", offsetof(verify_args, claim)},
        {"class", offsetof(verify_args, evidence_class)},
        {"commit", offsetof(verify_args, commit)},
        {"path", offsetof(verify_args, path)},
        {"symbol", offsetof(verify_args, symbol)},
        {"target", offsetof(verify_args, target)},
        {"probe", offsetof(verify_args, probe)},
        {"observed", offsetof(verify_args, observed)},
        {"observed_at", offsetof(verify_args, observed_at)},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        const char *v = *(const char **)((char *)a + FIELDS[i].off);
        if (v != NULL) {
            st = atlas_json_key_str(j, FIELDS[i].key, v, err);
        }
    }
    if (st == ATLAS_OK && a->line_start > 0) {
        st = atlas_json_key_int(j, "line_start", a->line_start, err);
    }
    if (st == ATLAS_OK && a->line_end > 0) {
        st = atlas_json_key_int(j, "line_end", a->line_end, err);
    }
    if (st == ATLAS_OK) {
        st = put_verify_actor(j, a, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_evidence_add(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"claim", "class", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "claim", "the claim this evidence bears on", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "class",
                       "what sort of thing this is. There is no unclassified evidence. COMPILER, "
                       "TEST, RUNTIME and DEPLOYED_CONFIG are absent on purpose and are refused "
                       "if you name them: their whole weight comes from Atlas having performed "
                       "the act, and you saying a test passed is not a test having passed. Use "
                       "atlas_verify_evidence_produce to have Atlas establish one, or record what "
                       "you actually did — AI_ANALYSIS, declaring what you read",
                       EVIDENCE_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "commit", "the commit these bytes are at", ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "path", "the repository-relative path", 4096u, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "symbol", "the symbol name", ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "line_start", "the first line", 0, INT32_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "line_end", "the last line", 0, INT32_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "target", "what the evidence is about, when a path does not say it",
                      ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "probe", "how it was looked at", ATLAS_VERIFY_METHOD_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "observed", "what was found", ATLAS_VERIFY_METHOD_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "observed_at", "when, if that matters", ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = prop_actor(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_evidence_add(atlas_mcp_server *s, const atlas_jsonv *args,
                                            atlas_buf *body, bool *degraded, atlas_err *err) {
    verify_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = verify_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "claim", ATLAS_VERIFY_UID_MAX, &a.claim, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "class", 32u, &a.evidence_class, err);
    }
    if (st == ATLAS_OK && (a.claim == NULL || a.evidence_class == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"claim\" and \"class\" are both required");
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "commit", ATLAS_VERIFY_NAME_MAX, &a.commit, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_rel_path(args, "path", &a.path, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "symbol", ATLAS_VERIFY_NAME_MAX, &a.symbol, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "target", ATLAS_VERIFY_NAME_MAX, &a.target, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "probe", ATLAS_VERIFY_METHOD_MAX, &a.probe, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "observed", ATLAS_VERIFY_METHOD_MAX, &a.observed, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "observed_at", ATLAS_VERIFY_NAME_MAX, &a.observed_at, err);
    }
    if (st == ATLAS_OK) {
        a.line_start = atlas_mcp_tools_arg_int(args, "line_start", 0);
        a.line_end = atlas_mcp_tools_arg_int(args, "line_end", 0);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_evidence_add, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.evidence_add", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

static atlas_status put_verify_produce(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    atlas_status st = atlas_json_key_str(j, "claim", a->claim, err);
    if (st == ATLAS_OK && a->verifier != NULL) {
        st = atlas_json_key_str(j, "verifier", a->verifier, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_evidence_produce(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"claim", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "claim", "the claim to establish mechanically", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "verifier",
                       "which allowlisted verifier to run. Defaults to the one the claim named. "
                       "You choose the verifier; you do not choose what it concludes",
                       VERIFIER_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_evidence_produce(atlas_mcp_server *s, const atlas_jsonv *args,
                                                atlas_buf *body, bool *degraded, atlas_err *err) {
    verify_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    atlas_status st = atlas_mcp_tools_arg_str(args, "claim", ATLAS_VERIFY_UID_MAX, &a.claim, err);
    if (st == ATLAS_OK && a.claim == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"claim\" is required");
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "verifier", ATLAS_VERIFY_NAME_MAX, &a.verifier, err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_produce, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.evidence_produce", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

static atlas_status put_verify_attestation(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    static const struct {
        const char *key;
        size_t off;
    } FIELDS[] = {
        {"claim", offsetof(verify_args, claim)},
        {"verdict", offsetof(verify_args, verdict)},
        {"method", offsetof(verify_args, method)},
        {"scope", offsetof(verify_args, scope)},
        {"evidence", offsetof(verify_args, evidence)},
        {"supersedes", offsetof(verify_args, supersedes)},
        {"commit", offsetof(verify_args, commit)},
    };
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; st == ATLAS_OK && i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        const char *v = *(const char **)((char *)a + FIELDS[i].off);
        if (v != NULL) {
            st = atlas_json_key_str(j, FIELDS[i].key, v, err);
        }
    }
    if (st == ATLAS_OK && a->has_self_confidence) {
        st = atlas_json_key_int(j, "self_confidence", a->self_confidence, err);
    }
    if (st == ATLAS_OK) {
        st = put_verify_actor(j, a, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_attestation_add(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"claim", "verdict", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "claim", "the claim you are attesting to", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_enum(j, "verdict", "what you concluded", VERDICT_ENUM, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "evidence",
                      "the evidence uids you relied on, space-separated. Say what you actually "
                      "read: if you and another agent both read one document, Atlas counts that "
                      "document once however many of you attest to it",
                      ATLAS_VERIFY_METHOD_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "method", "how you reached this", ATLAS_VERIFY_METHOD_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "scope", "the conditions you are asserting it under",
                      ATLAS_VERIFY_SCOPE_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "supersedes",
                      "an earlier attestation of yours this replaces, when you have changed your "
                      "mind. Reversing is not spam and is recorded as a reversal",
                      ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "commit", "the commit you examined", ATLAS_VERIFY_NAME_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "self_confidence",
                      "how sure you are, 0..100. This is data about you, not Atlas' confidence, "
                      "and repeating an attestation never raises a score", 0, 100, err);
    }
    if (st == ATLAS_OK) {
        st = prop_actor(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_attestation_add(atlas_mcp_server *s, const atlas_jsonv *args,
                                               atlas_buf *body, bool *degraded, atlas_err *err) {
    verify_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    atlas_status st = take_verify_actor(args, &a, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "claim", ATLAS_VERIFY_UID_MAX, &a.claim, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "verdict", 32u, &a.verdict, err);
    }
    if (st == ATLAS_OK && (a.claim == NULL || a.verdict == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"claim\" and \"verdict\" are both required");
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "method", ATLAS_VERIFY_METHOD_MAX, &a.method, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "scope", ATLAS_VERIFY_SCOPE_MAX, &a.scope, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "evidence", ATLAS_VERIFY_METHOD_MAX, &a.evidence, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "supersedes", ATLAS_VERIFY_UID_MAX, &a.supersedes, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "commit", ATLAS_VERIFY_NAME_MAX, &a.commit, err);
    }
    if (st == ATLAS_OK && atlas_jsonv_get(args, "self_confidence") != NULL) {
        a.self_confidence = atlas_mcp_tools_arg_int(args, "self_confidence", -1);
        a.has_self_confidence = a.self_confidence >= 0;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_attestation, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.attestation_add", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

static atlas_status put_verify_dependency(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    atlas_status st = atlas_json_key_str(j, "evidence", a->evidence, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "derives_from", a->derives_from, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_dependency_add(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"evidence", "derives_from", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "evidence", "the evidence that derives", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "derives_from", "the evidence it derives from", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_dependency_add(atlas_mcp_server *s, const atlas_jsonv *args,
                                              atlas_buf *body, bool *degraded, atlas_err *err) {
    verify_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    atlas_status st = atlas_mcp_tools_arg_str(args, "evidence", ATLAS_VERIFY_UID_MAX, &a.evidence, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "derives_from", ATLAS_VERIFY_UID_MAX, &a.derives_from, err);
    }
    if (st == ATLAS_OK && (a.evidence == NULL || a.derives_from == NULL)) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "a derivation names both the evidence that derives and the evidence "
                           "it derives from");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_dependency, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.dependency_add", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_MODEL_PROPOSAL), false, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

static atlas_status put_verify_evaluate(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    atlas_status st = atlas_json_key_str(j, "claim", a->claim, err);
    if (st == ATLAS_OK && a->repo != NULL) {
        st = atlas_json_key_str(j, "repo", a->repo, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_evaluate(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {"claim", NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "claim", "the claim to evaluate", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_evaluate(atlas_mcp_server *s, const atlas_jsonv *args,
                                        atlas_buf *body, bool *degraded, atlas_err *err) {
    verify_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = verify_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "claim", ATLAS_VERIFY_UID_MAX, &a.claim, err);
    }
    if (st == ATLAS_OK && a.claim == NULL) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "\"claim\" is required");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_evaluate, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.evaluate", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

static atlas_status put_verify_show(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    atlas_status st = ATLAS_OK;
    if (a->claim != NULL) {
        st = atlas_json_key_str(j, "claim", a->claim, err);
    }
    if (st == ATLAS_OK && a->claim_id > 0) {
        st = atlas_json_key_int(j, "claim_id", a->claim_id, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_show(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "claim", "the claim uid", ATLAS_VERIFY_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "claim_id", "the claim's numeric id, if you have that instead", 0,
                      INT32_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_show(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                    bool *degraded, atlas_err *err) {
    verify_args a;
    memset(&a, 0, sizeof(a));
    a.server = s;
    atlas_status st = atlas_mcp_tools_arg_str(args, "claim", ATLAS_VERIFY_UID_MAX, &a.claim, err);
    if (st == ATLAS_OK) {
        a.claim_id = atlas_mcp_tools_arg_int(args, "claim_id", 0);
    }
    if (st == ATLAS_OK && a.claim == NULL && a.claim_id <= 0) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "name the claim by \"claim\" or \"claim_id\"");
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_show, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.show", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    return st;
}

static atlas_status put_verify_claims(atlas_json *j, void *ud, atlas_err *err) {
    verify_args *a = (verify_args *)ud;
    atlas_status st = atlas_json_key_str(j, "repo", a->repo, err);
    if (st == ATLAS_OK && a->decision != NULL) {
        st = atlas_json_key_str(j, "decision", a->decision, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_int(j, "limit", a->limit, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_schema_verify_claims(atlas_json *j, atlas_err *err) {
    static const char *const REQUIRED[] = {NULL};
    atlas_status st = atlas_mcp_tools_schema_begin(j, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_repo(j, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_str(j, "decision", "only claims bearing on this knowledge record",
                      ATLAS_DECISION_UID_MAX, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_prop_int(j, "limit", "how many to return", 0, 500, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_schema_end(j, REQUIRED, err);
    }
    return st;
}

atlas_status atlas_mcp_tools_run_verify_claims(atlas_mcp_server *s, const atlas_jsonv *args, atlas_buf *body,
                                      bool *degraded, atlas_err *err) {
    verify_args a;
    atlas_buf repo = ATLAS_BUF_INIT;
    atlas_status st = verify_common(s, args, &a, &repo, err);
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_arg_str(args, "decision", ATLAS_DECISION_UID_MAX, &a.decision, err);
    }
    if (st == ATLAS_OK) {
        a.limit = atlas_mcp_tools_arg_int(args, "limit", 0);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_make_params(put_verify_claims, &a, &params, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_mcp_tools_forward(s, "verify.claims", atlas_buf_cstr(&params),
                     atlas_provenance_name(ATLAS_PROV_ATLAS_OWNED), true, body, degraded, err);
    }
    atlas_buf_free(&params);
    atlas_buf_free(&repo);
    return st;
}

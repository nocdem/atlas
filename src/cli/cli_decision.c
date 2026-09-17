/* Atlas - command line front end.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The CLI parses arguments, calls the service layer, and hands results to a
 * renderer. It contains no SQL, no git invocation and no output formatting.
 */
#include "atlas/cli.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/backup.h"
#include "atlas/gateway.h"
#include "atlas/daemon.h"
#include "atlas/hook.h"
#include "atlas/integrate.h"
#include "atlas/ipc.h"
#include "atlas/maintenance.h"
#include "atlas/mcp.h"
#include "atlas/sem.h"
#include "atlas/unit.h"
#include "cli/render.h"


#include "cli/cli_internal.h"

/* --- A4: decision documents ------------------------------------------------
 *
 * The CLI's whole job here is argument shape and renderer choice.
 * `service_decision.c` decides everything else, including whether a write goes
 * to the daemon's writer thread or is taken on this one.
 *
 * One rule is enforced here and nowhere else, because here is where the flag
 * exists: **`--yes` cannot approve anything.** It is refused explicitly rather
 * than ignored, because a flag that is silently ignored is a flag somebody will
 * put in a script and believe. */

typedef struct decision_render {
    cli_state *st;
    atlas_renderer *r;
} decision_render;

static atlas_status on_decision_item(const atlas_decision_summary *s, void *ud, atlas_err *err) {
    decision_render *dr = (decision_render *)ud;
    return dr->r->v->decision_item(dr->r, s, err);
}

/* `decision history` emits two differently shaped lists, and the service layer
 * delivers all the revisions before any of the events. The list boundaries are
 * therefore opened lazily on the first item of each: opening them up front
 * would need the counts before they are known. */
/* Migration 10: one edge event on its way to a renderer. */
typedef struct decision_edge_render {
    cli_state *st;
    atlas_renderer *r;
    int64_t count;
} decision_edge_render;

static atlas_status on_decision_edge(const atlas_decision_edge_entry *e, void *ud,
                                     atlas_err *err) {
    decision_edge_render *er = (decision_edge_render *)ud;
    er->count++;
    return er->r->v->decision_edge(er->r, e, err);
}

typedef struct decision_history_render {
    cli_state *st;
    atlas_renderer *r;
    int64_t revisions;
    int64_t events;
    bool in_events;
} decision_history_render;

static atlas_status on_history_revision(const atlas_decision_summary *s, void *ud,
                                        atlas_err *err) {
    decision_history_render *dr = (decision_history_render *)ud;
    if (dr->revisions == 0) {
        atlas_status st = dr->r->v->code_list_begin(dr->r, "revisions", err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    dr->revisions++;
    return dr->r->v->decision_item(dr->r, s, err);
}

static atlas_status on_history_event(const atlas_decision_timeline_entry *e, void *ud,
                                     atlas_err *err) {
    decision_history_render *dr = (decision_history_render *)ud;
    if (!dr->in_events) {
        atlas_status st = ATLAS_OK;
        if (dr->revisions > 0) {
            st = dr->r->v->code_list_end(dr->r, "revisions", "revision", "revisions",
                                         dr->revisions, false, err);
        } else {
            st = dr->r->v->code_list_begin(dr->r, "revisions", err);
            if (st == ATLAS_OK) {
                st = dr->r->v->code_list_end(dr->r, "revisions", "revision", "revisions", 0, false,
                                             err);
            }
        }
        if (st == ATLAS_OK) {
            st = dr->r->v->code_list_begin(dr->r, "timeline", err);
        }
        if (st != ATLAS_OK) {
            return st;
        }
        dr->in_events = true;
    }
    dr->events++;
    return dr->r->v->decision_event(dr->r, e, err);
}

/* An A2 proposal, rendered through the decision-summary shape.
 *
 * Reusing that shape rather than adding a fifth renderer method is deliberate:
 * a legacy proposal *is* a decision-shaped thing that has not been promoted,
 * and giving it its own shape would be a second place for the two to describe
 * one record differently. Its `status` says so in words. */
static atlas_status on_legacy_item(const atlas_decision_legacy_view *v, void *ud,
                                   atlas_err *err) {
    decision_render *dr = (decision_render *)ud;
    atlas_decision_summary s;
    atlas_decision_summary_init(&s);
    atlas_status st = atlas_buf_appendf(&s.uid, err, "a2-proposal-%lld", (long long)v->id);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s.status, v->imported ? "PROMOTED" : "A2_PROPOSAL", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&s.title, v->title.data, v->title.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&s.proposed_by, v->provenance.data, v->provenance.len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&s.created_at, v->created_at.data, v->created_at.len, err);
    }
    if (st == ATLAS_OK) {
        /* Where it went, when it went anywhere. */
        st = atlas_buf_set(&s.superseded_by, v->imported_uid.data, v->imported_uid.len, err);
    }
    s.link_count = v->path_count;
    if (st == ATLAS_OK) {
        st = dr->r->v->decision_item(dr->r, &s, err);
    }
    atlas_decision_summary_free(&s);
    return st;
}

static void decision_input_from(const cli_state *st, atlas_decision_input *in) {
    memset(in, 0, sizeof(*in));
    in->title = st->opts.decision.title;
    in->context_text = st->opts.decision.context_text;
    in->decision_text = st->opts.decision.decision_text;
    in->rationale_text = st->opts.decision.rationale;
    in->consequences_text = st->opts.decision.consequences;
    in->scope = st->opts.decision.scope;
    in->kind = st->opts.decision.kind;
    in->alternatives = st->opts.decision.alternatives;
    in->alternative_count = st->opts.decision.alternative_count;
    in->paths = st->opts.decision.paths;
    in->path_count = st->opts.decision.path_count;
    in->commits = st->opts.decision.commits;
    in->commit_count = st->opts.decision.commit_count;
    in->symbols = st->opts.decision.symbols;
    in->symbol_count = st->opts.decision.symbol_count;
    in->decision_links = st->opts.decision.decision_links;
    in->decision_link_count = st->opts.decision.decision_link_count;
    in->dedup_key = st->opts.decision.dedup_key;
}

static atlas_status render_outcome(cli_state *st, atlas_renderer *r, const char *command,
                                   const atlas_decision_outcome *o, atlas_err *err) {
    atlas_status result = atlas_cli_renderer_open(r, st->opts.json, st->out, command, err);
    if (result == ATLAS_OK) {
        result = r->v->decision_outcome(r, o, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(r, err);
    } else {
        atlas_cli_renderer_abort(r);
    }
    return result;
}


/* --- A6: the impact gate -----------------------------------------------------
 *
 * A read command with an exit code that means something. `--at` names the exact
 * repository state the caller is asking about; `--path` narrows the question;
 * `--depth` bounds the structural walk and is refused rather than clamped,
 * because a silently reduced depth is a silently smaller answer.
 *
 * The non-zero exits are the whole point of the command existing rather than
 * `gate check | grep`. PASS is 0, REVIEW_REQUIRED is 8, BLOCKED is 9 — and they
 * are distinct because an automation that treats "a human should look at this"
 * and "Atlas could not tell" identically will eventually be handed the second
 * and behave as though it got the first. */
/* --- A9.2: verification ------------------------------------------------------
 *
 * `verify show` and `verify policy` are reads. `verify run` can change a
 * lifecycle state and is the one subcommand that writes.
 *
 * There is no `verify approve`, no `verify override` and no flag that lowers a
 * threshold, and their absence is deliberate rather than unfinished: every
 * bound on what Atlas may automate lives in a root-owned file that this process
 * cannot edit. A command-line switch that relaxed one would hand the constrained
 * principal the power to unconstrain itself, which is the argument A7 makes
 * about `ATLAS_AUTHORITY_POLICY_PATH` and it holds here word for word.
 *
 * `verify policy` opens no index and binds nothing, so it answers on a machine
 * where Atlas has never run — which is exactly when somebody asks it. */
/* The six intake verbs.
 *
 * These are **intake, not lifecycle finalisation**, and that is why none of
 * them opens a terminal, mints a challenge or asks for a confirmation. Stating
 * a claim, citing evidence and attesting change no knowledge record and no
 * lifecycle state; requiring a `/dev/tty` for them would be ceremony borrowed
 * from an operation with entirely different consequences, and would make the
 * verification workflow unusable from the scripts and agents it exists for.
 * `decision approve` still needs everything it always needed.
 *
 * What the operator channel *does* buy here is one thing: the actor recorded
 * carries PEER_AUTHENTICATED identity rather than SELF_DECLARED, which is a
 * claim about a uid and never about a person. A7.1's honesty limits hold word
 * for word.
 *
 * The local/remote choice is the same one `verify show` makes and for the same
 * reason: under A7.1 the index is 0700 `atlasd`, so from the operator's account
 * the socket is the only path that can write. */
static atlas_status run_verify_intake(cli_state *st, atlas_ctx *ctx, atlas_renderer *r,
                                      const char *sub, atlas_err *err) {
    atlas_verify_op op;
    atlas_verify_op_init(&op);
    /* Which channel the local CLI speaks on is decided by the **root-owned
     * authority policy**, not by the fact that this is the CLI.
     *
     * Asserting OPERATOR unconditionally was wrong, and the fixture caught it:
     * on a machine where the profile is LOCKED — no root-owned policy, or a
     * binary the running uid can replace — the authority to speak as the
     * operator does not exist, so claiming it would mint exactly the
     * PEER_AUTHENTICATED row A7 says nothing may mint from an unprivileged
     * shape. It would also have made the two paths disagree: the socket edge
     * already asks this same probe, so a locked profile downgraded a request
     * that arrived over the socket and not one that ran locally.
     *
     * MODEL is the floor rather than a refusal. A locked profile does not stop
     * anybody recording evidence; it stops the record claiming the uid was
     * established when nothing established it. */
    atlas_authority auth;
    atlas_authority_probe(&auth);
    op.channel = auth.state == ATLAS_AUTHORITY_GRANTED ? ATLAS_VERIFY_CHANNEL_OPERATOR
                                                       : ATLAS_VERIFY_CHANNEL_MODEL;
    op.self_confidence = st->opts.verify.self_confidence > 0
                             ? (int)st->opts.verify.self_confidence
                             : -1;
    op.line_start = st->opts.verify.line_start;
    op.line_end = st->opts.verify.line_end;

    atlas_status s = ATLAS_OK;
    const char *sem = st->opts.verify.semantics;
    if (strcmp(sub, "claim") == 0) {
        op.kind = ATLAS_VERIFY_OP_CLAIM_CREATE;
        if (st->opts.verify.text == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify claim --repo R --text \"...\" "
                                 "[--semantics DESCRIPTIVE|NORMATIVE] [--record UID] [--domain D] "
                                 "[--scope S] [--verifier V --verifier-input I] [--commit OID]");
        }
        if (sem != NULL && !atlas_verify_claim_semantics_parse(sem, &op.semantics)) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "--semantics is DESCRIPTIVE or NORMATIVE: it says whether the "
                                 "claim observes what is or declares what ought to be");
        }
        op.semantics_given = sem != NULL;
    } else if (strcmp(sub, "evidence") == 0) {
        op.kind = ATLAS_VERIFY_OP_EVIDENCE_ADD;
        if (st->opts.verify.claim == NULL || st->opts.verify.cls == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify evidence --claim UID --class C [--path P] "
                                 "[--symbol S] [--commit OID] [--observed \"...\"]");
        }
        if (!atlas_verify_evidence_class_parse(st->opts.verify.cls, &op.evidence_class)) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "--class must name an evidence class; there is no unclassified "
                                 "evidence");
        }
    } else if (strcmp(sub, "produce") == 0) {
        op.kind = ATLAS_VERIFY_OP_EVIDENCE_PRODUCE;
        if (st->opts.verify.claim == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify produce --claim UID [--verifier V]");
        }
    } else if (strcmp(sub, "attest") == 0) {
        op.kind = ATLAS_VERIFY_OP_ATTESTATION_ADD;
        if (st->opts.verify.claim == NULL || st->opts.verify.verdict == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify attest --claim UID --verdict "
                                 "SUPPORT|CONTRADICT|INCONCLUSIVE [--evidence \"uid uid\"] "
                                 "[--method M] [--self-confidence N] [--supersedes UID]");
        }
        if (!atlas_verify_verdict_parse(st->opts.verify.verdict, &op.verdict)) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "--verdict is SUPPORT, CONTRADICT or INCONCLUSIVE");
        }
    } else if (strcmp(sub, "depend") == 0) {
        op.kind = ATLAS_VERIFY_OP_DEPENDENCY_ADD;
        if (st->opts.verify.evidence == NULL || st->opts.verify.derives_from == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify depend --evidence UID --derives-from UID");
        }
    } else {
        op.kind = ATLAS_VERIFY_OP_EVALUATE;
        if (st->opts.verify.claim == NULL) {
            atlas_verify_op_free(&op);
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas verify evaluate --claim UID [--repo R]");
        }
    }

    /* `--path`, `--commit` and `--scope` are already spoken for by the A4 arm,
     * so they are read from where that arm puts them rather than given a second
     * spelling. One flag, one meaning. */
    const char *path = st->opts.decision.path_count > 0 ? st->opts.decision.paths[0] : NULL;
    const char *commit = st->opts.decision.commit_count > 0 ? st->opts.decision.commits[0] : NULL;
    static const struct {
        size_t opt_off;
        size_t op_off;
    } COPY[] = {
        {offsetof(atlas_cli_opts, verify.claim), offsetof(atlas_verify_op, claim_uid)},
        {offsetof(atlas_cli_opts, verify.text), offsetof(atlas_verify_op, text)},
        {offsetof(atlas_cli_opts, verify.domain), offsetof(atlas_verify_op, domain)},
        {offsetof(atlas_cli_opts, verify.decision), offsetof(atlas_verify_op, document_uid)},
        {offsetof(atlas_cli_opts, verify.verifier), offsetof(atlas_verify_op, verifier)},
        {offsetof(atlas_cli_opts, verify.verifier_input),
         offsetof(atlas_verify_op, verifier_input)},
        {offsetof(atlas_cli_opts, verify.environment), offsetof(atlas_verify_op, environment)},
        {offsetof(atlas_cli_opts, verify.symbol), offsetof(atlas_verify_op, symbol)},
        {offsetof(atlas_cli_opts, verify.target), offsetof(atlas_verify_op, target)},
        {offsetof(atlas_cli_opts, verify.probe), offsetof(atlas_verify_op, probe)},
        {offsetof(atlas_cli_opts, verify.observed), offsetof(atlas_verify_op, observed)},
        {offsetof(atlas_cli_opts, verify.observed_at), offsetof(atlas_verify_op, observed_at)},
        {offsetof(atlas_cli_opts, verify.method), offsetof(atlas_verify_op, method)},
        {offsetof(atlas_cli_opts, verify.supersedes), offsetof(atlas_verify_op, supersedes_uid)},
        {offsetof(atlas_cli_opts, verify.actor), offsetof(atlas_verify_op, actor_name)},
        {offsetof(atlas_cli_opts, verify.provider), offsetof(atlas_verify_op, actor_provider)},
        {offsetof(atlas_cli_opts, verify.role), offsetof(atlas_verify_op, actor_role)},
    };
    for (size_t i = 0; s == ATLAS_OK && i < sizeof COPY / sizeof COPY[0]; i++) {
        const char *v = *(const char **)((char *)&st->opts + COPY[i].opt_off);
        if (v != NULL) {
            s = atlas_buf_set_str((atlas_buf *)((char *)&op + COPY[i].op_off), v, err);
        }
    }
    /* A dependency names the derived evidence in `--evidence`; every other verb
     * means "the evidence this attestation rests on" by the same flag. */
    if (s == ATLAS_OK && st->opts.verify.evidence != NULL) {
        s = atlas_buf_set_str(op.kind == ATLAS_VERIFY_OP_DEPENDENCY_ADD ? &op.derived_uid
                                                                        : &op.evidence_uids,
                              st->opts.verify.evidence, err);
    }
    if (s == ATLAS_OK && st->opts.verify.derives_from != NULL) {
        s = atlas_buf_set_str(&op.source_uid, st->opts.verify.derives_from, err);
    }
    if (s == ATLAS_OK && st->opts.repo != NULL) {
        s = atlas_buf_set_str(&op.repo_name, st->opts.repo, err);
    }
    if (s == ATLAS_OK && st->opts.decision.scope != NULL) {
        s = atlas_buf_set_str(&op.scope_note, st->opts.decision.scope, err);
    }
    if (s == ATLAS_OK && path != NULL) {
        s = atlas_buf_set_str(&op.path_text, path, err);
    }
    if (s == ATLAS_OK && commit != NULL) {
        s = atlas_buf_set_str(op.kind == ATLAS_VERIFY_OP_EVIDENCE_ADD ? &op.commit_oid
                                                                      : &op.basis_commit,
                              commit, err);
    }

    atlas_verify_intake_result res;
    atlas_verify_intake_result_init(&res);
    if (s == ATLAS_OK) {
        /* The test is whether this process *holds the writer lock*, not whether
         * it has a context at all. With a daemon running, `atlas_ctx` in AUTO
         * mode still opens — read-only — so `ctx != NULL` is true and the local
         * write then fails with "attempt to write a readonly database". A4's
         * predicate is the right one: apply locally when this process is the
         * writer, and go over the socket when something else is. */
        s = (ctx != NULL && atlas_ctx_is_writer(ctx))
                ? atlas_verify_intake_apply(atlas_ctx_db(ctx), &op, &res, err)
                : atlas_service_verify_intake_remote(&op, &res, err);
    }
    if (s == ATLAS_OK) {
        s = atlas_cli_renderer_open(r, st->opts.json, st->out, "verify", err);
    }
    if (s == ATLAS_OK) {
        s = r->v->verify_intake(r, sub, &res, err);
    }
    s = s == ATLAS_OK ? atlas_cli_renderer_close(r, err) : (atlas_cli_renderer_abort(r), s);
    atlas_verify_intake_result_free(&res);
    atlas_verify_op_free(&op);
    return s;
}

atlas_status atlas_cli_run_verify(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas verify show CLAIM-ID | atlas verify run NAME CLAIM-ID "
                             "| atlas verify policy");
    }
    const char *sub = st->operands[0];
    atlas_verify_report rep;
    atlas_verify_report_init(&rep);
    atlas_status result;

    if (strcmp(sub, "policy") == 0) {
        if (st->operand_count != 1) {
            atlas_verify_report_free(&rep);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas verify policy");
        }
        result = atlas_service_verify_policy(&rep, err);
    } else if (strcmp(sub, "show") == 0) {
        if (st->operand_count != 2) {
            atlas_verify_report_free(&rep);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas verify show CLAIM");
        }
        /* A claim may be named by its uid — which is what every surface reports
         * — or by the rowid A9.2's CLI took. A uid never parses as a number, so
         * the two spellings cannot collide.
         *
         * The remote form is not a fallback: under A7.1 the index is 0700
         * `atlasd`, so from the operator's account it is the *only* form that
         * can answer. A9.2.1 shipped the method without this branch and the
         * command reported "no index is available to read" about a claim the
         * daemon was holding. */
        char *endp = NULL;
        long long as_id = strtoll(st->operands[1], &endp, 10);
        bool numeric = endp != NULL && *endp == '\0' && endp != st->operands[1];
        result = ctx != NULL
                     ? atlas_service_verify_show_on(atlas_ctx_db(ctx), numeric ? as_id : 0,
                                                    numeric ? NULL : st->operands[1], &rep, err)
                     : atlas_service_verify_show_remote(numeric ? as_id : 0,
                                                        numeric ? NULL : st->operands[1], &rep,
                                                        err);
    } else if (strcmp(sub, "claim") == 0 || strcmp(sub, "evidence") == 0 ||
               strcmp(sub, "produce") == 0 || strcmp(sub, "attest") == 0 ||
               strcmp(sub, "depend") == 0 || strcmp(sub, "evaluate") == 0) {
        result = run_verify_intake(st, ctx, r, sub, err);
        atlas_verify_report_free(&rep);
        return result;
    } else if (strcmp(sub, "run") == 0) {
        if (st->operand_count != 3) {
            atlas_verify_report_free(&rep);
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas verify run NAME CLAIM-ID");
        }
        /* The same two spellings `verify show` accepts, and for the same
         * reason: the uid is what every surface reports, the rowid is what
         * A9.2's CLI took, and a uid never parses as a number. */
        char *rend = NULL;
        long long run_id = strtoll(st->operands[2], &rend, 10);
        bool run_numeric = rend != NULL && *rend == '\0' && rend != st->operands[2];
        result = atlas_service_verify_run(ctx, run_numeric ? run_id : 0,
                                          run_numeric ? NULL : st->operands[2], st->operands[1],
                                          &rep, err);
    } else {
        atlas_verify_report_free(&rep);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas verify claim|evidence|produce|attest|depend|evaluate|"
                             "show|run|policy");
    }

    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "verify", err);
    }
    if (result == ATLAS_OK) {
        result = r->v->verify(r, &rep, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(r, err);
    } else {
        atlas_cli_renderer_abort(r);
    }
    atlas_verify_report_free(&rep);
    return result;
}

atlas_status atlas_cli_run_gate(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas gate check NAME | atlas gate show NAME DECISION-ID");
    }
    const char *sub = st->operands[0];
    bool one = strcmp(sub, "show") == 0;
    if (!one && strcmp(sub, "check") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas gate check|show");
    }
    size_t want = one ? 3u : 2u;
    if (st->operand_count != want) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas gate %s NAME%s", sub,
                             one ? " DECISION-ID" : "");
    }

    atlas_gate_report rep;
    atlas_gate_report_init(&rep);
    atlas_status result;
    if (one) {
        result = ctx != NULL ? atlas_service_gate_show(ctx, st->operands[1], st->operands[2],
                                                      st->opts.decision.at_commit, &rep, err)
                             : atlas_service_gate_show_remote(st->operands[1], st->operands[2],
                                                              &rep, err);
    } else {
        atlas_gate_query q;
        atlas_gate_query_init(&q);
        q.repo_name = st->operands[1];
        q.at_commit = st->opts.decision.at_commit;
        q.depth = st->opts.depth;
        for (size_t i = 0; i < st->opts.decision.path_count; i++) {
            q.paths[q.path_count++] = st->opts.decision.paths[i];
        }
        result = ctx != NULL ? atlas_service_gate_check(ctx, &q, &rep, err)
                             : atlas_service_gate_check_remote(&q, &rep, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "gate", err);
    }
    if (result == ATLAS_OK) {
        result = r->v->gate(r, &rep, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(r, err);
        /* Only once the document is complete. A non-zero exit beside a
         * half-written answer would tell a caller to act on something it cannot
         * read. */
        st->gate_exit = atlas_gate_exit_code(rep.result);
    } else {
        atlas_cli_renderer_abort(r);
    }
    atlas_gate_report_free(&rep);
    return result;
}

/* The operator-only verbs — approve, reject, supersede, revalidate and A9.1's
 * resolve. One function: they differ by intent and by whether a replacement is
 * required, and five copies of the `--yes` refusal would be five chances for one
 * of them to be missing. */
static atlas_status run_decision_confirm(cli_state *st, atlas_ctx *ctx, atlas_renderer *r,
                                         atlas_decision_intent intent, atlas_err *err) {
    /* **A7: authority before anything else, including argument shape.**
     *
     * First, so that a locked profile never reaches the terminal, never mints a
     * capability, and never prints a confirmation prompt. A prompt in a locked
     * profile would be a question whose answer any process with this uid can
     * supply, which is the thing A7 exists to stop pretending about. */
    atlas_status auth = atlas_authority_require(ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE, err);
    if (auth != ATLAS_OK) {
        return auth;
    }
    if (st->operand_count != 3u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision %s NAME DECISION-ID%s",
                             atlas_decision_intent_name(intent),
                             intent == ATLAS_DECISION_INTENT_SUPERSEDE ? " --by DECISION-ID" : "");
    }
    if (st->opts.yes) {
        /* Refused, not ignored. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--yes cannot approve, reject, supersede or resolve a decision. This "
                             "command needs an interactive terminal, and Atlas will not accept a "
                             "confirmation from a flag, a pipe, a file or an environment "
                             "variable.");
    }
    if (st->opts.json) {
        /* The prompt goes to the terminal and the result to stdout, so --json
         * would interleave a human prompt with a machine document. Refusing is
         * clearer than producing either one badly. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--json is not available for %s: it is an interactive command",
                             atlas_decision_intent_name(intent));
    }
    const char *replacement = st->opts.decision.by;
    if (intent == ATLAS_DECISION_INTENT_SUPERSEDE && replacement == NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "atlas decision supersede needs --by DECISION-ID, the decision that "
                             "replaces this one");
    }
    if (intent != ATLAS_DECISION_INTENT_SUPERSEDE && replacement != NULL) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "--by is only meaningful for supersede");
    }
    atlas_decision_outcome out;
    atlas_decision_outcome_init(&out);
    atlas_status result = atlas_service_decision_confirm(
        ctx, st->operands[1], st->operands[2], intent, replacement, st->opts.decision.revision,
        &out, err);
    if (result == ATLAS_OK) {
        result = render_outcome(st, r, "decision", &out, err);
    }
    atlas_decision_outcome_free(&out);
    return result;
}

/* A15 T5. `atlas review apply FILE [--check] [--json]`: walks a review sheet
 * through the operator channel, one entry at a time, by looping the one
 * function that mints and spends a lifecycle capability
 * (`atlas_service_review_apply` -> `atlas_service_decision_confirm`). This
 * file mints and spends nothing itself.
 *
 * The renderer is opened late, at the first entry actually produced --
 * `memory_open_late`/`memory_finish`'s own shape, adopted rather than
 * copied for the same reason those exist: a refusal that happens before any
 * entry (a malformed sheet, a missing file, the zero-entry refusal
 * `atlas_service_review_apply` itself always raises before its loop) would
 * otherwise land *inside* a JSON document `review_begin` already opened --
 * `"entries":[` with no closing bracket and no comma before the error
 * object -- the same defect CLAUDE.md names for `job list`/`plan list` and
 * `docs/backlog.md` carries as a family-wide fix nobody has made. Opening
 * late does not cover a refusal *after* the first entry has already
 * streamed to stdout in `--json` mode -- nothing can un-write bytes already
 * flushed -- which is the same residual `memory_open_late` itself leaves. */
typedef struct review_render_ctx {
    atlas_renderer *r;
    cli_state *st;
    const char *sheet_path;
    bool check_only;
    bool opened;
} review_render_ctx;

static atlas_status review_open_late(review_render_ctx *rc, atlas_err *err) {
    if (rc->opened) {
        return ATLAS_OK;
    }
    atlas_status st = atlas_cli_renderer_open(rc->r, rc->st->opts.json, rc->st->out, "review apply", err);
    if (st == ATLAS_OK) {
        st = rc->r->v->review_begin(rc->r, rc->sheet_path, rc->check_only, err);
    }
    if (st == ATLAS_OK) {
        rc->opened = true;
    }
    return st;
}

static atlas_status review_entry_sink(const atlas_review_outcome *o, void *ud, atlas_err *err) {
    review_render_ctx *rc = (review_render_ctx *)ud;
    atlas_status st = review_open_late(rc, err);
    if (st == ATLAS_OK) {
        st = rc->r->v->review_entry(rc->r, o, err);
    }
    return st;
}

/* Closes what `review_open_late` opened, or reports a refusal that happened
 * before any entry did -- `memory_finish`'s own two-branch ending. */
static atlas_status review_finish(review_render_ctx *rc, atlas_status result,
                                  const atlas_review_totals *t, bool check_only, atlas_err *err) {
    if (!rc->opened) {
        return result;
    }
    if (result == ATLAS_OK) {
        result = rc->r->v->review_totals(rc->r, check_only, t, err);
    }
    if (result == ATLAS_OK) {
        result = atlas_cli_renderer_close(rc->r, err);
    } else {
        atlas_cli_renderer_abort(rc->r);
    }
    return result;
}

/* Reuses the mechanism `atlas gate` already carries in `st->gate_exit`: a
 * command-specific exit code above the seven-value contract, returned only
 * once a complete, successful document has been written, following gate's
 * own 8/9 precedent (cli.c's help text at "gate check"). 0 when every entry
 * ended APPLIED (or READY under --check); 8 otherwise.
 *
 * `bad` sums all five non-good buckets unconditionally, in both modes,
 * rather than switching on `check_only` to sum only the subset the current
 * walker can produce there. `walk_entry` (src/core/service_review.c) can
 * today only reach READY, MOVED, DISPOSED or MISSING under `check_only` --
 * `abandoned` and `refused` need a spent or refused confirm, which
 * `check_only` never reaches -- so the two forms are equivalent for now and
 * a mode-split would not be wrong today. It is not written that way because
 * it would rot silently: a later change to the pre-check path that lets
 * `check_only` produce REFUSED (or `ready` show up outside it) would exit
 * `0` with a bad row visibly on screen, in a bucket the split branch never
 * summed. Unconditional summation is exclusive by construction and cannot
 * drift out of step with what the walker actually produces. */
static int review_exit_code(const atlas_review_totals *t) {
    int64_t bad = t->abandoned + t->moved + t->disposed + t->missing + t->refused;
    return bad == 0 ? 0 : 8;
}

atlas_status atlas_cli_run_review(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, atlas_err *err) {
    /* Which subcommand this even is, decided before authority is asked about
     * anything -- `run_decision`'s own precedent: a completely bare command
     * or an unrecognised verb answers with a usage line and touches no
     * authority check at all, because no verb has been chosen yet for
     * authority to be a precondition of. `review` has exactly one verb
     * today, so the "no such verb" and "no verb at all" cases share this one
     * frozen usage sentence rather than a second, unpinned message. */
    if (st->operand_count == 0 || strcmp(st->operands[0], "apply") != 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas review apply FILE [--check]");
    }

    /* **A7: authority before anything else the apply verb does, including
     * its own argument shape and --check.** `atlas_service_review_apply`
     * itself skips this check under `check_only` so its own tests can run
     * against a locally-built, non-root-owned binary (see the T4 report's
     * chain); this call is what keeps "a locked profile refuses before the
     * sheet file is opened" true for `--check` as well, at the layer
     * `run_decision_confirm` above already puts it -- authority first, then
     * this verb's own operand count, exactly as `run_decision_confirm`
     * checks authority before its own `operand_count != 3u`. */
    atlas_status auth = atlas_authority_require(ATLAS_AUTHORITY_OP_DECISION_LIFECYCLE, err);
    if (auth != ATLAS_OK) {
        return auth;
    }
    if (st->operand_count != 2u) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas review apply FILE [--check]");
    }
    if (st->opts.yes) {
        /* Refused, not ignored -- the same shape as `decision approve`'s. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--yes cannot apply a review sheet. Each entry needs its "
                             "confirmation typed on an interactive terminal, and Atlas will "
                             "not accept one from a flag, a pipe, the sheet itself or an "
                             "environment variable.");
    }
    if (st->opts.json && !st->opts.check) {
        /* Not symmetric with `decision approve`'s blanket --json refusal on
         * purpose: a real apply prompts on /dev/tty and is not machine
         * readable, but --check mints nothing and reveals nothing this uid
         * could not already read with `decision show`. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "--json is not available for review apply: it is an interactive "
                             "command. Use --check for a machine-readable dry run.");
    }

    const char *sheet_path = st->operands[1];
    bool check_only = st->opts.check;

    review_render_ctx rc = {r, st, sheet_path, check_only, false};
    atlas_review_totals totals;
    memset(&totals, 0, sizeof(totals));
    atlas_status result = atlas_service_review_apply(ctx, sheet_path, check_only, review_entry_sink,
                                                      &rc, &totals, err);
    result = review_finish(&rc, result, &totals, check_only, err);
    if (result == ATLAS_OK) {
        /* Only once the document is complete, for the reason `gate` sets
         * `gate_exit` only there: a non-zero exit beside a half-written
         * answer would tell a caller to act on something it cannot read. */
        st->gate_exit = review_exit_code(&totals);
    }
    return result;
}

atlas_status atlas_cli_run_decision(cli_state *st, atlas_ctx *ctx, atlas_renderer *r, int64_t limit,
                                 atlas_err *err) {
    if (st->operand_count == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "usage: atlas decision list|show|search|history|links|for-file|"
                             "propose|revise|approve|reject|supersede|revalidate|resolve|export|"
                             "orphaned|"
                             "legacy|promote ...");
    }
    const char *sub = st->operands[0];
    atlas_status result;

    if (strcmp(sub, "list") == 0 || strcmp(sub, "search") == 0 || strcmp(sub, "for-file") == 0) {
        size_t want = strcmp(sub, "list") == 0 ? 2u : 3u;
        if (st->operand_count != want) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 strcmp(sub, "list") == 0
                                     ? "usage: atlas decision list NAME [--status STATUS] "
                                       "[--kind KIND]"
                                     : (strcmp(sub, "search") == 0
                                            ? "usage: atlas decision search NAME QUERY"
                                            : "usage: atlas decision for-file NAME PATH"));
        }
        atlas_decision_list_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.limit = limit;
        if (strcmp(sub, "search") == 0) {
            opts.mode = ATLAS_DECISION_LIST_SEARCH;
            opts.query = st->operands[2];
        } else if (strcmp(sub, "for-file") == 0) {
            opts.mode = ATLAS_DECISION_LIST_PATH;
            opts.path = st->operands[2];
        } else if (st->opts.decision.status != NULL) {
            atlas_decision_state parsed;
            if (!atlas_decision_state_parse(st->opts.decision.status, &parsed)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "--status is PROPOSED, APPROVED, REJECTED, SUPERSEDED or "
                                     "RESOLVED");
            }
            opts.mode = ATLAS_DECISION_LIST_STATUS;
            opts.status = st->opts.decision.status;
        }
        /* A9.1. The kind filter is orthogonal to the mode, so it is set after the
         * mode is chosen and applies to all three. Validated here as well as at
         * the service layer, because a misspelt kind must be a usage error rather
         * than an empty result that looks like an answer. */
        if (st->opts.decision.kind != NULL) {
            atlas_decision_kind parsed;
            if (!atlas_decision_kind_parse(st->opts.decision.kind, &parsed)) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "--kind is one of %s",
                                     atlas_decision_kind_list());
            }
            opts.kind = st->opts.decision.kind;
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        /* Zeroed here rather than trusted to the callee: the remote path fills
         * only the counts the daemon actually reported, so an older daemon that
         * omits one would otherwise have it read from uninitialised stack. */
        atlas_decision_counts counts;
        memset(&counts, 0, sizeof(counts));
        int64_t count = 0;
        bool more = false;
        result = r->v->note_repo(r, st->operands[1], err);
        if (result == ATLAS_OK) {
            result = r->v->list_begin(r, "decisions", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL
                         ? atlas_service_decision_list(ctx, st->operands[1], &opts,
                                                       on_decision_item, &dr, &counts, &count,
                                                       &more, err)
                         : atlas_service_decision_list_remote(st->operands[1], &opts,
                                                              on_decision_item, &dr, &count, &more,
                                                              &counts, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "decision", "decisions", count, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->decision_counts(r, &counts, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "show") == 0 || strcmp(sub, "export") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision %s NAME DECISION-ID",
                                 sub);
        }
        bool markdown = strcmp(sub, "export") == 0 &&
                        (st->opts.decision.format == NULL ||
                         strcmp(st->opts.decision.format, "markdown") == 0);
        if (strcmp(sub, "export") == 0 && !markdown && st->opts.decision.format != NULL &&
            strcmp(st->opts.decision.format, "json") != 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "--format is markdown or json");
        }
        atlas_decision_document doc;
        atlas_decision_document_init(&doc);
        result = ctx != NULL
                     ? atlas_service_decision_show(ctx, st->operands[1], st->operands[2],
                                                   st->opts.decision.revision, &doc, err)
                     : atlas_service_decision_show_remote(st->operands[1], st->operands[2],
                                                          st->opts.decision.revision, &doc, err);
        if (result == ATLAS_OK && markdown) {
            /* Markdown goes to stdout as itself rather than through a renderer:
             * it is a document, not a report, and wrapping it would make it
             * unusable for the one thing an export is for. Never written into
             * the target repository — Atlas is read-only there. */
            result = atlas_service_decision_export_markdown(&doc, st->out, err);
            st->rendered = true;
        } else if (result == ATLAS_OK) {
            bool as_json = st->opts.json || strcmp(sub, "export") == 0;
            result = atlas_cli_renderer_open(r, as_json, st->out, "decision", err);
            if (result == ATLAS_OK) {
                result = r->v->decision_show(r, &doc, err);
            }
            if (result == ATLAS_OK) {
                result = atlas_cli_renderer_close(r, err);
            } else {
                atlas_cli_renderer_abort(r);
            }
        }
        atlas_decision_document_free(&doc);
        return result;
    }

    /* `decision links REPO ID` — the account of one document's relations.
     *
     * A read, and a different ledger from `decision history`: that one is about
     * the document's lifecycle, this one about its edges. Keeping them apart is
     * why an edge annotation cannot be mistaken for a lifecycle transition. */
    if (strcmp(sub, "links") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision links NAME "
                                                       "DECISION-ID");
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        result = r->v->note_repo(r, st->operands[1], err);
        if (result == ATLAS_OK) {
            result = r->v->code_list_begin(r, "edges", err);
        }
        decision_edge_render er = {st, r, 0};
        int64_t n = 0;
        bool more = false;
        if (result == ATLAS_OK) {
            result = atlas_service_decision_links(ctx, st->operands[1], st->operands[2],
                                                  on_decision_edge, &er, &n, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->code_list_end(r, "edges", "edge", "edges", n, more, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "history") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas decision history NAME DECISION-ID");
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        bool agrees = true;
        result = r->v->note_repo(r, st->operands[1], err);
        /* **Two lists, and they are two lists.**
         *
         * A revision entry and a timeline entry are different shapes, so
         * putting both in one array would produce a document whose meaning
         * depends on which member a parser happens to look at. The named-list
         * pair carries its own count per list, which is exactly what it exists
         * for — see the comment on `code_list_begin`. */
        decision_history_render dr = {st, r, 0, 0, false};
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_decision_history(
                                       ctx, st->operands[1], st->operands[2], on_history_revision,
                                       on_history_event, &dr, &agrees, err)
                                 : atlas_service_decision_history_remote(
                                       st->operands[1], st->operands[2], on_history_revision,
                                       on_history_event, &dr, &agrees, err);
        }
        if (result == ATLAS_OK && dr.in_events) {
            result = r->v->code_list_end(r, "timeline", "event", "events", dr.events, false, err);
        } else if (result == ATLAS_OK) {
            /* A document with revisions but no events cannot exist — a proposal
             * writes both — so this is the "no revisions at all" case, and both
             * empty lists are still emitted so the shape does not change. */
            result = r->v->code_list_begin(r, "revisions", err);
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "revisions", "revision", "revisions", 0, false,
                                             err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_begin(r, "timeline", err);
            }
            if (result == ATLAS_OK) {
                result = r->v->code_list_end(r, "timeline", "event", "events", 0, false, err);
            }
        }
        if (result == ATLAS_OK) {
            /* The ledger is canonical and the status columns cache it. Whether
             * the two agree is reported on every timeline, because a timeline
             * is exactly where somebody would notice. */
            result = r->v->decision_ledger(r, agrees, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "propose") == 0 || strcmp(sub, "revise") == 0) {
        bool revise = strcmp(sub, "revise") == 0;
        size_t want = revise ? 3u : 2u;
        if (st->operand_count != want) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 revise ? "usage: atlas decision revise NAME DECISION-ID --title T "
                                          "--decision D [...]"
                                        : "usage: atlas decision propose NAME --title T "
                                          "--decision D [...]");
        }
        if (st->opts.decision.title == NULL || st->opts.decision.decision_text == NULL) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "a decision needs --title and --decision");
        }
        atlas_decision_input in;
        decision_input_from(st, &in);
        atlas_decision_outcome out;
        atlas_decision_outcome_init(&out);
        result = revise ? atlas_service_decision_revise(ctx, st->operands[1], st->operands[2], &in,
                                                        &out, err)
                        : atlas_service_decision_propose(ctx, st->operands[1], &in, &out, err);
        if (result == ATLAS_OK) {
            result = render_outcome(st, r, "decision", &out, err);
        }
        atlas_decision_outcome_free(&out);
        return result;
    }

    /* `decision link add SOURCE TARGET`. A proposal write, not an operator
     * operation: it goes through the same authority path `propose` and `revise`
     * use, and mints nothing. */
    if (strcmp(sub, "link") == 0) {
        bool adding = st->operand_count == 5u && strcmp(st->operands[1], "add") == 0;
        bool removing = st->operand_count == 5u && strcmp(st->operands[1], "remove") == 0;
        /* `note` records one event about an edge and touches no link. It is how
         * the history of a relation that is already gone gets written down:
         * there is nothing left to add or remove, only something to say. */
        bool noting = st->operand_count == 5u && strcmp(st->operands[1], "note") == 0;
        if (!adding && !removing && !noting) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas decision link add|remove|note REPO SOURCE_ID "
                                 "TARGET_ID [--why TEXT] [--provenance P] [--event E]");
        }
        /* `--why` is required to withdraw a relation and optional to draw one.
         * The asymmetry is deliberate: an addition that arrives without a
         * reason can be explained later by annotating the edge, but a removal
         * is the last thing that happens to it, so if the reason is not
         * recorded now it is not recorded at all. */
        const char *why = st->opts.decision.why;
        const char *prov = st->opts.decision.provenance != NULL ? st->opts.decision.provenance
                                                                : "OPERATOR";
        if (noting && (why == NULL || *why == '\0')) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "recording a note about a relation needs "
                                                       "--why");
        }
        if (removing && (why == NULL || *why == '\0')) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "withdrawing a relation needs --why: the reason is the only "
                                 "thing that will still explain it afterwards");
        }
        atlas_decision_outcome o;
        atlas_decision_outcome_init(&o);
        bool removed = false;
        atlas_status lr;
        if (noting) {
            lr = atlas_service_decision_link_note(ctx, st->operands[2], st->operands[3],
                                                  st->operands[4], why, prov,
                                                  st->opts.decision.edge_event, &o, err);
        } else if (adding) {
            lr = atlas_service_decision_link_add(ctx, st->operands[2], st->operands[3],
                                                 st->operands[4], why, prov, &o, err);
        } else {
            lr = atlas_service_decision_link_remove(ctx, st->operands[2], st->operands[3],
                                                    st->operands[4], why, &o, &removed, err);
        }
        if (lr == ATLAS_OK) {
            o.is_removal = removing;
            o.removed = removed;
            lr = render_outcome(st, r,
                                noting    ? "decision link note"
                                : adding  ? "decision link add"
                                          : "decision link remove",
                                &o, err);
        }
        atlas_decision_outcome_free(&o);
        return lr;
    }

    if (strcmp(sub, "orphaned") == 0) {
        /* Decisions attached to no live repository. Takes no repository name,
         * because a repository is exactly what these do not have. */
        if (st->operand_count != 1u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision orphaned");
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        int64_t count = 0;
        bool more = false;
        result = r->v->list_begin(r, "orphaned", err);
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_decision_orphans(ctx, limit, on_decision_item,
                                                                  &dr, &count, &more, err)
                                 : atlas_service_decision_orphans_remote(limit, on_decision_item,
                                                                         &dr, &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "orphaned decision", "orphaned decisions", count, err);
        }
        if (result == ATLAS_OK && count > 0) {
            /* The remedy, printed with the finding. An orphan is recoverable
             * and a user who does not know that will assume it is not. */
            result = r->v->note_repo(
                r,
                "register-the-original-repository-and-rescan-to-reattach-these", err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "legacy") == 0) {
        /* The A2 proposals this repository still holds, and which of them have
         * been promoted. Read-only over the A2 tables, which A4 never writes. */
        if (st->operand_count != 2u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "usage: atlas decision legacy NAME");
        }
        result = atlas_cli_renderer_open(r, st->opts.json, st->out, "decision", err);
        if (result != ATLAS_OK) {
            return result;
        }
        decision_render dr = {st, r};
        int64_t count = 0;
        bool more = false;
        result = r->v->note_repo(r, st->operands[1], err);
        if (result == ATLAS_OK) {
            result = r->v->list_begin(r, "a2_proposals", err);
        }
        if (result == ATLAS_OK) {
            result = ctx != NULL ? atlas_service_decision_legacy(ctx, st->operands[1], limit,
                                                                 on_legacy_item, &dr, &count,
                                                                 &more, err)
                                 : atlas_service_decision_legacy_remote(st->operands[1], limit,
                                                                        on_legacy_item, &dr,
                                                                        &count, &more, err);
        }
        if (result == ATLAS_OK) {
            result = r->v->list_end(r, "proposal", "proposals", count, err);
        }
        if (result == ATLAS_OK) {
            result = atlas_cli_renderer_close(r, err);
        } else {
            atlas_cli_renderer_abort(r);
        }
        return result;
    }

    if (strcmp(sub, "promote") == 0) {
        if (st->operand_count != 3u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "usage: atlas decision promote NAME LEGACY-ID");
        }
        char *endp = NULL;
        long legacy = strtol(st->operands[2], &endp, 10);
        if (endp == NULL || *endp != '\0' || legacy <= 0) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "the A2 proposal id is a positive number, as shown by "
                                 "`atlas decision legacy`");
        }
        atlas_decision_outcome out;
        atlas_decision_outcome_init(&out);
        result = atlas_service_decision_promote(ctx, st->operands[1], legacy, &out, err);
        if (result == ATLAS_OK) {
            result = render_outcome(st, r, "decision", &out, err);
        }
        atlas_decision_outcome_free(&out);
        return result;
    }

    if (strcmp(sub, "approve") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_APPROVE, err);
    }
    if (strcmp(sub, "revalidate") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_REVALIDATE, err);
    }
    if (strcmp(sub, "reject") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_REJECT, err);
    }
    if (strcmp(sub, "supersede") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_SUPERSEDE, err);
    }
    /* A9.1. The same interactive channel as approve, reject, supersede and
     * revalidate, through the same function, so there is one place a lifecycle
     * capability is minted and spent and this adds no second path. */
    if (strcmp(sub, "resolve") == 0) {
        return run_decision_confirm(st, ctx, r, ATLAS_DECISION_INTENT_RESOLVE, err);
    }

    return atlas_err_set(err, ATLAS_ERR_USAGE, "unknown decision subcommand \"%s\"", sub);
}

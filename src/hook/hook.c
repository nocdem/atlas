/* Atlas - the Claude Code hook adapter.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * One process per event. It reads a bounded payload, extracts the few fields
 * Atlas is allowed to keep, makes at most one daemon call with a deadline, and
 * writes one JSON object.
 *
 * What is deliberately absent from every code path in this file is the point of
 * it. There is no read of `transcript_path`. There is no branch that copies
 * `user_message`, `last_assistant_message`, `tool_result`, `error`, or the
 * `command` field of a Bash tool input. `tool_input` is reached into for exactly
 * one member — a file path — and nothing else in it is looked at. A reviewer
 * should be able to establish that by reading the file rather than by trusting
 * this comment, which is why the extraction is a single small function.
 */
#define _GNU_SOURCE 1

#include "atlas/hook.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/ipc.h"
#include "atlas/jsonread.h"
#include "atlas/safetext.h"

/* Atlas' own client identity. Provider-neutral storage, named here and only
 * here: a second adapter supplies a different pair and gets its own rows. */
#define HOOK_PROVIDER "anthropic"
#define HOOK_CLIENT "claude-code"

void atlas_hook_opts_init(atlas_hook_opts *o) {
    memset(o, 0, sizeof(*o));
}

/* The events Atlas configures, in the order the plugin lists them.
 *
 * WorktreeCreate is deliberately not here. Configuring it would replace Claude's
 * own worktree creation with whatever Atlas printed, and Atlas has no business
 * deciding where a worktree lives. Worktree changes are observed through
 * CwdChanged and DirectoryAdded, which report the same fact without taking over
 * the mechanism. */
static const char *const HOOK_EVENTS[] = {
    "SessionStart",   "UserPromptSubmit", "PreToolUse",   "PostToolUse", "PostToolUseFailure",
    "PostToolBatch",  "Stop",             "PreCompact",   "PostCompact", "SessionEnd",
    "CwdChanged",     "DirectoryAdded",   "SubagentStart", "SubagentStop", "WorktreeRemove",
    NULL,
};

const char *const *atlas_hook_events(void) {
    return HOOK_EVENTS;
}

bool atlas_hook_event_known(const char *event) {
    if (event == NULL) {
        return false;
    }
    for (size_t i = 0; HOOK_EVENTS[i] != NULL; i++) {
        if (strcmp(HOOK_EVENTS[i], event) == 0) {
            return true;
        }
    }
    return false;
}

/* --- payload input -------------------------------------------------------- */

/* Reads stdin to a hard ceiling.
 *
 * The ceiling is checked as the buffer grows rather than from a claimed length,
 * because stdin has no length to claim. A payload that exceeds it is refused
 * whole: a truncated JSON document would either fail to parse or, worse, parse
 * into something that is not what was sent. */
static atlas_status read_all(FILE *in, atlas_buf *out, atlas_err *err) {
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1u, sizeof(chunk), in)) > 0) {
        if (out->len + n > ATLAS_HOOK_MAX_INPUT_BYTES) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "the hook payload exceeds %u bytes and was refused whole",
                                 (unsigned)ATLAS_HOOK_MAX_INPUT_BYTES);
        }
        atlas_status st = atlas_buf_append(out, chunk, n, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (ferror(in)) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot read the hook payload");
    }
    return ATLAS_OK;
}

/* --- the one place a tool input is inspected -----------------------------
 *
 * Exactly one member is read, and only for the tools whose whole purpose is to
 * write a named file. A Bash tool's `command`, an Edit's `old_string` and
 * `new_string`, a Write's `content` and a Task's `prompt` are never reached.
 *
 * Returning the path is what makes attribution possible: "this session ran Edit
 * on this path" is a stronger claim than "this path changed while this session
 * was open", and Atlas records which of the two it has. */
static const char *edit_path_of(const char *tool_name, const atlas_jsonv *tool_input) {
    if (tool_name == NULL || tool_input == NULL) {
        return NULL;
    }
    if (strcmp(tool_name, "NotebookEdit") == 0) {
        return atlas_jsonv_str_member(tool_input, "notebook_path");
    }
    if (strcmp(tool_name, "Edit") == 0 || strcmp(tool_name, "Write") == 0 ||
        strcmp(tool_name, "MultiEdit") == 0 || strcmp(tool_name, "NotebookWrite") == 0) {
        return atlas_jsonv_str_member(tool_input, "file_path");
    }
    /* Every other tool contributes its name and its outcome and nothing else.
     * That includes Bash, whose input is a command line and is never stored. */
    return NULL;
}

/* --- output --------------------------------------------------------------- */

/* Writes the minimal valid document. Used by every path that has nothing to add,
 * which is most of them: a hook that returns `{}` has told Claude everything it
 * needs to know, and anything more is context somebody pays for. */
static void emit_empty(FILE *out) {
    (void)fputs("{}\n", out);
    (void)fflush(out);
}

/* Writes a document carrying Atlas-owned context.
 *
 * The text has already been through `atlas_ai_context_render`, which enforces
 * the size ceiling and the character allowlist. It is emitted through the
 * streaming JSON writer anyway, because "it was already checked" is not a
 * reason to hand-quote a string. */
static void emit_context(FILE *out, const char *context) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_json *j = atlas_json_new(out, &err);
    if (j == NULL) {
        emit_empty(out);
        return;
    }
    atlas_status st = atlas_json_obj_begin(j, &err);
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "hookSpecificOutput", &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_begin(j, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "hookEventName", "SessionStart", &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "additionalContext", context, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_obj_end(j, &err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_finish(j, &err);
    } else {
        atlas_json_free(j);
    }
    if (st != ATLAS_OK) {
        emit_empty(out);
        return;
    }
    (void)fflush(out);
}

/* --- daemon calls --------------------------------------------------------- */

typedef struct hook_ctx {
    atlas_buf socket;
    int timeout_ms;
    FILE *errout;
    atlas_safe_pool safe;
    bool reachable;

    /* Fields lifted from the payload. Every one of them is metadata. */
    const char *session_id;
    /* The parent's key, captured before `session_id` is rewritten for a
     * subagent. Reading it off `session_id` afterwards would make a subagent its
     * own parent. */
    const char *parent_key;
    const char *prompt_id;
    const char *cwd;
    const char *agent_id;
    const char *agent_type;
    const char *source;   /* SessionStart source / SessionEnd end_reason */
    const char *tool_name;
    const char *tool_use_id;
} hook_ctx;

/* Reports why nothing was recorded, on stderr, without the payload.
 *
 * A message here names the method and the reason; it never quotes the hook
 * input, because stderr is captured by Claude in debug mode and a diagnostic
 * that leaks a prompt is worse than no diagnostic. */
static void note(hook_ctx *hc, const char *what, const char *detail) {
    if (hc->errout == NULL) {
        return;
    }
    (void)fprintf(hc->errout, "atlas hook: %s: %s\n", what,
                  atlas_safe(&hc->safe, detail != NULL ? detail : "no detail"));
}

/* Adds the identity every AI method takes. */
static atlas_status put_identity(atlas_json *j, const hook_ctx *hc, atlas_err *err) {
    atlas_status st = atlas_json_key_str(j, "provider", HOOK_PROVIDER, err);
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "client", HOOK_CLIENT, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key_str(j, "session_key", hc->session_id != NULL ? hc->session_id : "", err);
    }
    if (st == ATLAS_OK && hc->cwd != NULL) {
        st = atlas_json_key_str(j, "root", hc->cwd, err);
    }
    if (st == ATLAS_OK && hc->agent_id != NULL) {
        st = atlas_json_key_str(j, "agent_id", hc->agent_id, err);
    }
    if (st == ATLAS_OK && hc->agent_type != NULL) {
        st = atlas_json_key_str(j, "agent_type", hc->agent_type, err);
    }
    return st;
}

/* One daemon round trip.
 *
 * `build` fills the params; the identity is added first so no caller forgets
 * it. The response is parsed but its failure is not propagated: a hook has
 * nothing useful to do with a daemon error except say so and carry on. */
typedef atlas_status (*params_fn)(atlas_json *j, const hook_ctx *hc, void *ud, atlas_err *err);

static atlas_ipc_response *call(hook_ctx *hc, const char *method, params_fn build, void *ud) {
    if (!hc->reachable) {
        return NULL;
    }
    atlas_err err;
    atlas_err_init(&err);

    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    if (atlas_ipc_params_begin(&p, &j, &err) != ATLAS_OK) {
        note(hc, method, atlas_err_msg(&err));
        return NULL;
    }
    atlas_status st = put_identity(j, hc, &err);
    if (st == ATLAS_OK && build != NULL) {
        st = build(j, hc, ud, &err);
    }
    atlas_buf params = ATLAS_BUF_INIT;
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, &err);
    } else {
        atlas_ipc_params_abort(p);
    }
    if (st != ATLAS_OK) {
        note(hc, method, atlas_err_msg(&err));
        atlas_buf_free(&params);
        return NULL;
    }

    atlas_buf resp = ATLAS_BUF_INIT;
    st = atlas_ipc_call_timeout(atlas_buf_cstr(&hc->socket), method, atlas_buf_cstr(&params),
                                hc->timeout_ms, &resp, &err);
    atlas_buf_free(&params);
    if (st != ATLAS_OK) {
        note(hc, method, atlas_err_msg(&err));
        atlas_buf_free(&resp);
        return NULL;
    }

    atlas_ipc_response *r = NULL;
    st = atlas_ipc_response_parse(resp.data, resp.len, &r, &err);
    atlas_buf_free(&resp);
    if (st != ATLAS_OK) {
        note(hc, method, atlas_err_msg(&err));
        return NULL;
    }
    if (!atlas_ipc_response_ok(r)) {
        note(hc, method, atlas_ipc_response_message(r));
    }
    return r;
}

/* --- per-event parameter builders ---------------------------------------- */

typedef struct tool_params {
    const char *phase; /* intent | ok | failed */
    const char *path;  /* absolute; the daemon relativises it */
    const char *dedup_key;
} tool_params;

static atlas_status put_tool(atlas_json *j, const hook_ctx *hc, void *ud, atlas_err *err) {
    tool_params *tp = (tool_params *)ud;
    atlas_status st = atlas_json_key_str(j, "phase", tp->phase, err);
    if (st == ATLAS_OK && hc->tool_name != NULL) {
        st = atlas_json_key_str(j, "tool", hc->tool_name, err);
    }
    if (st == ATLAS_OK && hc->tool_use_id != NULL) {
        st = atlas_json_key_str(j, "tool_use_id", hc->tool_use_id, err);
    }
    if (st == ATLAS_OK && tp->dedup_key != NULL) {
        st = atlas_json_key_str(j, "dedup_key", tp->dedup_key, err);
    }
    if (st == ATLAS_OK && tp->path != NULL) {
        st = atlas_json_key(j, "paths", err);
        if (st == ATLAS_OK) {
            st = atlas_json_arr_begin(j, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_str(j, tp->path, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_json_arr_end(j, err);
        }
    }
    return st;
}

typedef struct batch_params {
    const atlas_jsonv *tool_calls;
    const char *dedup_key;
} batch_params;

/* Collects the paths this batch's *successful* edit tools named.
 *
 * A tool that reported an error contributes nothing: a failed Write is not
 * evidence that the file changed. A tool that reported success contributes its
 * path as an *intent*, not as a change — whether the file actually changed is
 * decided by the index, not by what a tool said about itself. */
static atlas_status put_batch(atlas_json *j, const hook_ctx *hc, void *ud, atlas_err *err) {
    batch_params *bp = (batch_params *)ud;
    (void)hc;
    atlas_status st = atlas_json_key_bool(j, "sync", true, err);
    if (st == ATLAS_OK && bp->dedup_key != NULL) {
        st = atlas_json_key_str(j, "dedup_key", bp->dedup_key, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_key(j, "paths", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_begin(j, err);
    }
    size_t n = atlas_jsonv_arr_len(bp->tool_calls);
    size_t emitted = 0;
    for (size_t i = 0; st == ATLAS_OK && i < n && emitted < (size_t)ATLAS_AI_MAX_BATCH_PATHS; i++) {
        const atlas_jsonv *tc = atlas_jsonv_at(bp->tool_calls, i);
        const atlas_jsonv *error = atlas_jsonv_get(tc, "error");
        if (error != NULL && !atlas_jsonv_is_null(error)) {
            continue;
        }
        const char *name = atlas_jsonv_str_member(tc, "tool_name");
        const char *path = edit_path_of(name, atlas_jsonv_get(tc, "tool_input"));
        if (path == NULL || path[0] != '/') {
            continue;
        }
        st = atlas_json_str(j, path, err);
        emitted++;
    }
    if (st == ATLAS_OK) {
        st = atlas_json_arr_end(j, err);
    }
    return st;
}

typedef struct simple_params {
    const char *source;
    const char *phase;
    const char *dedup_key;
    const char *parent_session_key;
} simple_params;

static atlas_status put_simple(atlas_json *j, const hook_ctx *hc, void *ud, atlas_err *err) {
    simple_params *sp = (simple_params *)ud;
    (void)hc;
    atlas_status st = ATLAS_OK;
    if (sp->source != NULL) {
        st = atlas_json_key_str(j, "source", sp->source, err);
    }
    if (st == ATLAS_OK && sp->phase != NULL) {
        st = atlas_json_key_str(j, "phase", sp->phase, err);
    }
    if (st == ATLAS_OK && sp->dedup_key != NULL) {
        st = atlas_json_key_str(j, "dedup_key", sp->dedup_key, err);
    }
    if (st == ATLAS_OK && sp->parent_session_key != NULL) {
        st = atlas_json_key_str(j, "parent_session_key", sp->parent_session_key, err);
    }
    return st;
}

/* --- the context envelope ------------------------------------------------- */

/* Asks the daemon for the envelope and emits it, or emits `{}` when there is
 * nothing to say.
 *
 * SessionStart is the only event that injects context, and that is deliberate
 * rather than incidental: it is the one lifecycle event whose output contract
 * carries `additionalContext` *and* which Claude re-delivers after a compaction
 * or a resume, with `source` saying which. So the restore path after compaction
 * is this same function, reached the same way.
 *
 * The envelope is built server-side. That is where the guarantee lives: this
 * process never assembles context from repository data, it forwards a document
 * the daemon already bounded and checked. */
static void emit_envelope(hook_ctx *hc, FILE *out) {
    atlas_ipc_response *r = call(hc, "ai.context", NULL, NULL);
    const char *context = NULL;
    if (r != NULL && atlas_ipc_response_ok(r) && atlas_ipc_result_str(r, "context", &context) &&
        context != NULL) {
        /* A second, independent bound. The daemon checked this; so does the
         * adapter, because a ceiling enforced in one process only is a ceiling
         * that stops existing the moment the two versions differ. */
        if (strlen(context) <= ATLAS_AI_MAX_CONTEXT_BYTES) {
            emit_context(out, context);
            atlas_ipc_response_free(r);
            return;
        }
        note(hc, "ai.context", "the envelope exceeded the Atlas context ceiling and was dropped");
    }
    atlas_ipc_response_free(r);
    emit_empty(out);
}

/* --- event handling ------------------------------------------------------- */

/* Builds a dedup key from a stable pair. Bounded; a key that would not fit is
 * simply omitted, which costs idempotency for one event rather than failing it. */
static void make_key(char *dst, size_t dst_size, const char *prefix, const char *a) {
    (void)snprintf(dst, dst_size, "%s:%s", prefix, a != NULL ? a : "0");
}

/* Asks whether the current working directory is a repository Atlas already
 * knows, and does nothing whatever about it if it is not.
 *
 * **A7 changed this from a registration to a question.** It used to call
 * `repo.ensure`, which registered whatever absolute path it was handed. A
 * session start is a model-triggered event — the model chooses the directory
 * Claude is launched in as surely as a person does — so that made "a model
 * opened a session here" sufficient to create a trusted Atlas registration, and
 * with it an indexing pass over a tree nobody had vouched for. There is no
 * longer any RPC method that could do it; see `src/ipc/server.c`.
 *
 * The call is kept because attaching an already-registered repository is the
 * useful half and carries no authority: it reports, and the envelope the hook
 * emits then describes a repository an operator registered, or describes none.
 *
 * A directory Atlas does not know is a *candidate*, and the only thing Atlas
 * does with a candidate is let `atlas repo list --candidates` show the operator
 * that a session ran somewhere unregistered. Registration is `atlas repo add`,
 * on the local path, under the write lock. */
static void resolve_repository(hook_ctx *hc) {
    if (hc->cwd == NULL || !hc->reachable) {
        return;
    }
    atlas_err err;
    atlas_err_init(&err);
    atlas_ipc_params *p = NULL;
    atlas_json *j = NULL;
    if (atlas_ipc_params_begin(&p, &j, &err) != ATLAS_OK) {
        note(hc, "repo.resolve", atlas_err_msg(&err));
        return;
    }
    atlas_buf params = ATLAS_BUF_INIT;
    atlas_status st = atlas_json_key_str(j, "path", hc->cwd, &err);
    if (st == ATLAS_OK) {
        st = atlas_ipc_params_finish(p, &params, &err);
    } else {
        atlas_ipc_params_abort(p);
    }
    if (st == ATLAS_OK) {
        atlas_buf resp = ATLAS_BUF_INIT;
        /* A read over an index, so the ordinary deadline is enough — the long
         * one existed because registration ran git. Still failing open. */
        if (atlas_ipc_call_timeout(atlas_buf_cstr(&hc->socket), "repo.resolve",
                                   atlas_buf_cstr(&params), hc->timeout_ms, &resp,
                                   &err) != ATLAS_OK) {
            note(hc, "repo.resolve", atlas_err_msg(&err));
        }
        atlas_buf_free(&resp);
    } else {
        note(hc, "repo.resolve", atlas_err_msg(&err));
    }
    atlas_buf_free(&params);
}

static void handle(hook_ctx *hc, const char *event, const atlas_jsonv *root, FILE *out) {
    char key[192];

    if (strcmp(event, "SessionStart") == 0) {
        /* Attach to the repository if an operator registered it, then open the
         * session, then answer with the envelope. An unregistered directory
         * stays unregistered. */
        resolve_repository(hc);
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        sp.source = hc->source;
        make_key(key, sizeof(key), "open", hc->source);
        sp.dedup_key = key;
        atlas_ipc_response_free(call(hc, "ai.session.open", put_simple, &sp));
        emit_envelope(hc, out);
        return;
    }

    if (strcmp(event, "UserPromptSubmit") == 0) {
        /* Metadata only. The prompt is not read, not sent and not stored: what
         * is recorded is that a turn began. */
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        make_key(key, sizeof(key), "turn", hc->prompt_id);
        sp.dedup_key = key;
        atlas_ipc_response_free(call(hc, "ai.session.touch", put_simple, &sp));
        emit_empty(out);
        return;
    }

    if (strcmp(event, "PreToolUse") == 0 || strcmp(event, "PostToolUse") == 0 ||
        strcmp(event, "PostToolUseFailure") == 0) {
        tool_params tp;
        memset(&tp, 0, sizeof(tp));
        if (strcmp(event, "PreToolUse") == 0) {
            tp.phase = "intent";
            make_key(key, sizeof(key), "intent", hc->tool_use_id);
        } else if (strcmp(event, "PostToolUse") == 0) {
            tp.phase = "ok";
            make_key(key, sizeof(key), "ok", hc->tool_use_id);
        } else {
            tp.phase = "failed";
            make_key(key, sizeof(key), "fail", hc->tool_use_id);
        }
        tp.dedup_key = key;
        /* The single member of tool_input Atlas ever reads. */
        tp.path = edit_path_of(hc->tool_name, atlas_jsonv_get(root, "tool_input"));
        if (tp.path != NULL && tp.path[0] != '/') {
            /* A relative path would be resolved against a working directory
             * Atlas cannot see. Dropping it loses attribution for one tool call,
             * which is better than attributing a change to the wrong file. */
            tp.path = NULL;
        }
        atlas_ipc_response_free(call(hc, "ai.tool.record", put_tool, &tp));
        emit_empty(out);
        return;
    }

    if (strcmp(event, "PostToolBatch") == 0) {
        batch_params bp;
        memset(&bp, 0, sizeof(bp));
        bp.tool_calls = atlas_jsonv_get(root, "tool_calls");
        /* Keyed on the first tool use id in the batch, which is stable across a
         * redelivery of the same batch and differs between two batches. */
        const char *first = NULL;
        if (bp.tool_calls != NULL) {
            first = atlas_jsonv_str_member(atlas_jsonv_at(bp.tool_calls, 0), "tool_use_id");
        }
        make_key(key, sizeof(key), "batch", first);
        bp.dedup_key = key;
        atlas_ipc_response_free(call(hc, "ai.batch.correlate", put_batch, &bp));
        emit_empty(out);
        return;
    }

    if (strcmp(event, "Stop") == 0) {
        /* Non-blocking, always.
         *
         * Atlas never returns `decision: block` here and never exits 2, so there
         * is no state in which it can produce a stop loop. That is a structural
         * property rather than a guarded one: there is no code path that emits a
         * blocking document, so `stop_hook_active` is honoured as a courtesy
         * rather than relied on.
         *
         * What this does is close the turn: every changed path with no recorded
         * reason gets an explicit UNKNOWN record. Atlas does not ask the model to
         * explain itself and does not invent an explanation — it records that
         * nobody said why, which is a queryable fact rather than a silence. */
        bool active = false;
        (void)atlas_jsonv_bool(atlas_jsonv_get(root, "stop_hook_active"), &active);
        if (active) {
            emit_empty(out);
            return;
        }
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        make_key(key, sizeof(key), "turn_close", hc->prompt_id);
        sp.dedup_key = key;
        atlas_ipc_response_free(call(hc, "ai.turn.close", put_simple, &sp));
        emit_empty(out);
        return;
    }

    if (strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0) {
        bool pre = (strcmp(event, "PreCompact") == 0);
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        sp.phase = pre ? "pre_compact" : "post_compact";
        make_key(key, sizeof(key), pre ? "ckpt-pre" : "ckpt-post", hc->prompt_id);
        sp.dedup_key = key;
        atlas_ipc_response_free(call(hc, "ai.session.checkpoint", put_simple, &sp));
        /* Both halves emit `{}`.
         *
         * PostCompact has no `additionalContext` in its output contract, so
         * returning one would be ignored and the restore would silently not
         * happen. The channel that does work is SessionStart with
         * `source: "compact"`, which Claude delivers after a compaction and
         * which the SessionStart handler above already answers with the full
         * envelope. Restoring in the place that has a restore mechanism beats
         * restoring in the place it reads more naturally.
         *
         * `compact_summary` is never read in either half. It is a model-written
         * description of a conversation, and ingesting it would turn a summary
         * into evidence. */
        emit_empty(out);
        return;
    }

    if (strcmp(event, "SessionEnd") == 0) {
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        sp.source = hc->source;
        atlas_ipc_response_free(call(hc, "ai.session.close", put_simple, &sp));
        emit_empty(out);
        return;
    }

    if (strcmp(event, "CwdChanged") == 0 || strcmp(event, "DirectoryAdded") == 0) {
        /* A session that changes directory or gains a working directory gains a
         * repository rather than replacing one: work it did before the change
         * still belongs to it, and the change set for the old repository stays
         * open.
         *
         * A7: the newly granted directory is resolved, not ensured. Attaching
         * to a directory Atlas has never seen does nothing, and that is now the
         * intended outcome rather than a gap: adding a directory to a session is
         * a model-visible action, and it must not be the thing that decides
         * what Atlas indexes. An operator registers with `atlas repo add`. */
        resolve_repository(hc);
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        sp.source = (strcmp(event, "CwdChanged") == 0) ? "cwd_changed" : "directory_added";
        atlas_ipc_response_free(call(hc, "ai.session.attach", put_simple, &sp));
        emit_empty(out);
        return;
    }

    if (strcmp(event, "SubagentStart") == 0 || strcmp(event, "SubagentStop") == 0) {
        /* A subagent is modelled as its own session with a parent, so its change
         * set and its records are separable from the parent's without a second
         * set of tables. */
        simple_params sp;
        memset(&sp, 0, sizeof(sp));
        sp.parent_session_key = hc->parent_key;
        sp.source = "subagent";
        if (strcmp(event, "SubagentStart") == 0) {
            make_key(key, sizeof(key), "open", "subagent");
            sp.dedup_key = key;
            atlas_ipc_response_free(call(hc, "ai.session.open", put_simple, &sp));
        } else {
            atlas_ipc_response_free(call(hc, "ai.session.close", put_simple, &sp));
        }
        emit_empty(out);
        return;
    }

    if (strcmp(event, "WorktreeRemove") == 0) {
        /* Observed only. Atlas does not deregister the repository: a worktree
         * that came and went is history the index should keep, and removing a
         * registration is a decision a person makes with `atlas repo remove`. */
        emit_empty(out);
        return;
    }

    /* An event Atlas does not handle still gets a valid document. A future
     * Claude version adding one must not be able to break a session because
     * Atlas answered with nothing. */
    emit_empty(out);
}

atlas_status atlas_hook_run(const char *event, FILE *in, FILE *out, FILE *errout,
                            const atlas_hook_opts *opts) {
    hook_ctx hc;
    memset(&hc, 0, sizeof(hc));
    atlas_buf_init(&hc.socket);
    atlas_safe_pool_init(&hc.safe);
    hc.errout = errout;

    /* An off switch that does not need Claude reconfigured. A user debugging
     * something else should be able to take Atlas out of the loop in one
     * variable rather than by editing a plugin. */
    if (getenv("ATLAS_CLAUDE_DISABLE") != NULL) {
        emit_empty(out);
        atlas_safe_pool_free(&hc.safe);
        atlas_buf_free(&hc.socket);
        return ATLAS_OK;
    }

    hc.timeout_ms = (opts != NULL && opts->timeout_ms > 0) ? opts->timeout_ms
                                                           : ATLAS_HOOK_IPC_TIMEOUT_MS;
    if (event != NULL && strcmp(event, "SessionEnd") == 0 &&
        (opts == NULL || opts->timeout_ms <= 0)) {
        /* Every SessionEnd hook shares a 1.5 second budget across the whole
         * installation, so Atlas takes a fraction of it and fails open past
         * that. Being slow here delays the user's exit. */
        hc.timeout_ms = ATLAS_HOOK_TEARDOWN_TIMEOUT_MS;
    }

    atlas_err err;
    atlas_err_init(&err);
    if (opts != NULL && opts->socket_path != NULL) {
        (void)atlas_buf_set_str(&hc.socket, opts->socket_path, &err);
    } else if (atlas_ipc_socket_path(&hc.socket, &err) != ATLAS_OK) {
        note(&hc, "socket", atlas_err_msg(&err));
    }
    hc.reachable = hc.socket.len > 0;

    atlas_buf payload = ATLAS_BUF_INIT;
    atlas_status st = read_all(in, &payload, &err);
    if (st != ATLAS_OK) {
        note(&hc, "payload", atlas_err_msg(&err));
        emit_empty(out);
        atlas_buf_free(&payload);
        atlas_safe_pool_free(&hc.safe);
        atlas_buf_free(&hc.socket);
        return ATLAS_OK;
    }

    atlas_jsondoc *doc = NULL;
    if (payload.len > 0) {
        st = atlas_jsondoc_parse(payload.data, payload.len, ATLAS_HOOK_MAX_INPUT_BYTES,
                                 ATLAS_IPC_MAX_JSON_DEPTH, &doc, &err);
        if (st != ATLAS_OK) {
            note(&hc, "payload", atlas_err_msg(&err));
        }
    }
    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    if (!atlas_jsonv_is_obj(root)) {
        /* A payload Atlas cannot read is a payload it records nothing from. It
         * still answers, because refusing to answer would break the session for
         * a reason that has nothing to do with the user. */
        emit_empty(out);
        atlas_jsondoc_free(doc);
        atlas_buf_free(&payload);
        atlas_safe_pool_free(&hc.safe);
        atlas_buf_free(&hc.socket);
        return ATLAS_OK;
    }

    /* Every field this adapter reads, in one place.
     *
     * Nothing outside this list is looked at, and the list contains no content:
     * `user_message`, `last_assistant_message`, `tool_result`, `error`,
     * `compact_summary`, `content` and `transcript_path` are all absent by
     * construction, not by a filter that could be relaxed. */
    hc.session_id = atlas_jsonv_str_member(root, "session_id");
    hc.prompt_id = atlas_jsonv_str_member(root, "prompt_id");
    hc.cwd = atlas_jsonv_str_member(root, "cwd");
    hc.agent_id = atlas_jsonv_str_member(root, "agent_id");
    hc.agent_type = atlas_jsonv_str_member(root, "agent_type");
    hc.tool_name = atlas_jsonv_str_member(root, "tool_name");
    hc.tool_use_id = atlas_jsonv_str_member(root, "tool_use_id");
    hc.source = atlas_jsonv_str_member(root, "source");
    if (hc.source == NULL) {
        hc.source = atlas_jsonv_str_member(root, "end_reason");
    }
    if (hc.source == NULL) {
        hc.source = atlas_jsonv_str_member(root, "compaction_trigger");
    }

    /* `directory_path` is the working directory a DirectoryAdded event is
     * about, and it is the one place a path other than cwd names a root. */
    const char *added = atlas_jsonv_str_member(root, "directory_path");
    if (added == NULL) {
        added = atlas_jsonv_str_member(root, "new_cwd");
    }
    if (added != NULL && added[0] == '/') {
        hc.cwd = added;
    }
    if (hc.cwd == NULL) {
        /* The documented fallback, and only that: a bounded, Claude-supplied
         * project root rather than this process's own working directory, which
         * is wherever Claude happened to be. */
        hc.cwd = getenv("CLAUDE_PROJECT_DIR");
    }
    if (hc.cwd != NULL && hc.cwd[0] != '/') {
        hc.cwd = NULL;
    }

    /* A subagent gets its own session, keyed by the parent's id and its own
     * agent id, so its records do not merge into the parent's change set. */
    hc.parent_key = hc.session_id;
    atlas_buf subkey = ATLAS_BUF_INIT;
    if (hc.agent_id != NULL && hc.session_id != NULL &&
        (strcmp(event != NULL ? event : "", "SubagentStart") == 0 ||
         strcmp(event != NULL ? event : "", "SubagentStop") == 0)) {
        if (atlas_buf_appendf(&subkey, &err, "%s/%s", hc.session_id, hc.agent_id) == ATLAS_OK) {
            hc.session_id = atlas_buf_cstr(&subkey);
        }
    }

    handle(&hc, event != NULL ? event : "", root, out);

    atlas_buf_free(&subkey);
    atlas_jsondoc_free(doc);
    atlas_buf_free(&payload);
    atlas_safe_pool_free(&hc.safe);
    atlas_buf_free(&hc.socket);
    return ATLAS_OK;
}

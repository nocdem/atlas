/* Atlas - the automatic model-context envelope.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This file is the model-context trust boundary described in
 * docs/ai-trust-boundary.md, in its enforced form.
 *
 * Everything Atlas injects into a session automatically — without anyone asking
 * for it — passes through here, and what leaves here contains **no
 * repository-controlled or model-provided free-form text; only fixed
 * Atlas-owned control text and typed values**. Every value is one of five
 * things:
 *
 *   1. an integer Atlas assigned or counted;
 *   2. a boolean;
 *   3. a string from a fixed vocabulary, checked against that vocabulary here
 *      rather than trusted to be a member of it;
 *   4. a lowercase hex hash of a fixed length, checked to be hex;
 *   5. fixed control text written in this file — the `note=` line — which is an
 *      Atlas string literal and depends on nothing outside this translation
 *      unit. It is what tells the reader how to treat the rest, so it is part
 *      of the boundary rather than an exception to it.
 *
 * Deliberately absent, and this is the point of the file: the repository
 * **name**, the repository **root**, branch names, commit subjects and bodies,
 * author identities, file paths, tag names, remote URLs and git error text.
 *
 * The name and the root were in the first version of this file and both were
 * wrong. A repository name is derived from a directory basename and a root is a
 * filesystem path, so both are chosen by whoever created the directory. A
 * directory called `ignore previous instructions` yields a name and a root
 * containing that phrase; it is entirely printable, contains no control byte
 * and no invalid UTF-8, and passes every encoding Atlas has completely
 * unchanged. Encoding defends a terminal and a parser. It does not defend
 * meaning, and nothing here claims otherwise.
 *
 * What replaces them is `repo_id` — an opaque row id Atlas assigned — and
 * `root_hash`, a SHA-256 of the canonical root. Those identify a repository
 * across sessions and let a consumer tell two apart, while being incapable of
 * saying anything. The client already knows its own working directory.
 *
 * Because no value can carry arbitrary bytes, this file does not escape: it
 * **validates**. A value that is not what it claims to be is replaced by a fixed
 * marker rather than encoded, which is a stronger guarantee and a much shorter
 * argument. `atlas_ai_context_is_bounded()` is the whole policy; the renderer
 * checks its own output against it and discards a document that fails.
 *
 * Repository prose reaches a model only through an explicit MCP tool call, in a
 * result that names its provenance and labels it UNTRUSTED_DATA. That is a
 * different code path with different rules, and it is never automatic.
 */
#include "atlas/ai.h"

#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"

/* The allowlist, as one function so the renderer and the checker cannot
 * disagree about it.
 *
 * Deliberately smaller than it was. The previous version permitted `%` (for
 * percent-escaped values) and `/` and `(` `)` `+` (for paths); with no paths
 * and no escaped values left, none of those are needed. What remains is what
 * the framing, the fixed key names and the one fixed note sentence require. */
static bool envelope_byte_ok(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case ' ':
    case '.':
    case ',':
    case '_':
    case '-':
    case ':':
    case '=':
    case '<':
    case '>':
    case '/':
    case '\n':
        return true;
    default:
        return false;
    }
}

bool atlas_ai_context_is_bounded(const char *text, size_t len) {
    if (text == NULL) {
        return false;
    }
    if (len > ATLAS_AI_MAX_CONTEXT_BYTES) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!envelope_byte_ok((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

/* --- validation, not escaping --------------------------------------------
 *
 * Each of these answers one question: is this value the shape it claims to be?
 * A value that is not is replaced, never reproduced. That is why there is no
 * escaper in this file any more — nothing here can carry a byte the allowlist
 * would have to escape, because nothing that could is emitted at all. */

/* Lowercase hex of exactly `want` characters. */
static bool is_hex_of_length(const char *s, size_t want) {
    if (s == NULL) {
        return false;
    }
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++, n++) {
        bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
        if (!hex) {
            return false;
        }
    }
    return n == want;
}

/* An object id is reproduced only when it is what an object id looks like.
 *
 * `scanned_head` comes from git, and git is the one thing Atlas trusts to
 * produce a hex object id — but the column is a TEXT column in a rebuildable
 * index, and a value that is not hex is a value that came from somewhere else.
 * Rather than escape it, the envelope omits it: an absent head is a fact, and a
 * head that is not a head is not one worth reporting. */
static bool is_hex_oid(const char *s) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++, n++) {
        bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F');
        if (!hex) {
            return false;
        }
    }
    return n >= 4u && n <= ATLAS_OID_HEX_MAX;
}

/* A fixed vocabulary, checked rather than trusted. */
static const char *head_state_or_unknown(const char *s) {
    if (s == NULL) {
        return "unknown";
    }
    if (strcmp(s, "born") == 0 || strcmp(s, "unborn") == 0 || strcmp(s, "detached") == 0) {
        return s;
    }
    return "unknown";
}

/* The complete set of reasons Atlas will state for an index not being current.
 *
 * These are the strings `atlas_server_index_current` produces. Listing them
 * again here, and refusing anything not in the list, is what keeps the envelope
 * free of foreign text even if a future caller passes something else: a reason that
 * is not one of Atlas' own becomes `other`, which is still true. */
static const char *not_current_reason_or_other(const char *s) {
    static const char *const REASONS[] = {
        "no reconciliation pass has completed for this repository yet",
        "an unresolved event gap means Atlas cannot prove it observed every change",
        "a full content verification is owed and has not completed",
        "the filesystem watcher failed and is not observing this repository",
        "the filesystem watcher is running with a known blind spot",
        NULL,
    };
    if (s == NULL || s[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; REASONS[i] != NULL; i++) {
        if (strcmp(s, REASONS[i]) == 0) {
            return s;
        }
    }
    return "other";
}

void atlas_ai_context_init(atlas_ai_context *c) {
    memset(c, 0, sizeof(*c));
    atlas_buf_init(&c->not_current_reason);
    (void)snprintf(c->head_state, sizeof(c->head_state), "%s", "unknown");
}

void atlas_ai_context_free(atlas_ai_context *c) {
    if (c == NULL) {
        return;
    }
    atlas_buf_free(&c->not_current_reason);
}

void atlas_ai_context_set_root_hash(atlas_ai_context *c, const void *root_raw, size_t root_len) {
    if (root_raw == NULL || root_len == 0) {
        c->root_hash[0] = '\0';
        return;
    }
    /* Hashed from the raw bytes, so two repositories whose paths differ only in
     * bytes that are not valid UTF-8 still hash differently. */
    atlas_sha256_hex(root_raw, root_len, c->root_hash);
}

atlas_status atlas_ai_context_render(const atlas_ai_context *c, atlas_buf *out, atlas_err *err) {
    atlas_buf_reset(out);

    /* The framing is Atlas-owned and fixed. It exists so a reader can see where
     * Atlas' claims start and stop; it is not a security boundary, and nothing
     * inside it is repository text, which is what actually makes it safe. */
    atlas_status st = atlas_buf_appendf(out, err,
                                        "<atlas-context v=%d>\n"
                                        "atlas=%s phase=%s protocol=%u schema=%d\n",
                                        ATLAS_AI_CONTEXT_VERSION, ATLAS_VERSION_STRING, ATLAS_PHASE,
                                        (unsigned)ATLAS_IPC_PROTOCOL_VERSION, ATLAS_SCHEMA_VERSION);
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(out, err, "daemon=%s\n",
                               c->daemon_reachable ? "reachable" : "unavailable");
    }
    if (st != ATLAS_OK) {
        return st;
    }

    if (!c->repo_known) {
        /* An unregistered directory is reported as such rather than omitted. A
         * consumer that sees nothing cannot tell "Atlas has no repository here"
         * from "Atlas did not answer". */
        st = atlas_buf_append_str(out, "repo=none reason=not-registered\n", err);
    } else {
        /* An opaque id and a hash. Never a name, never a path: both are chosen
         * by whoever created the directory, and both survive every encoding
         * Atlas has. See the header comment. */
        st = atlas_buf_appendf(out, err, "repo_id=%lld root_hash=%s\n", (long long)c->repo_id,
                               is_hex_of_length(c->root_hash, ATLAS_SHA256_HEX_LEN) ? c->root_hash
                                                                                   : "unknown");
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(out, err, "head=%s state=%s\n",
                                   is_hex_oid(c->head_oid) ? c->head_oid : "unknown",
                                   head_state_or_unknown(c->head_state));
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(out, err, "index_current=%s generation=%lld cursor=%lld\n",
                                   c->index_current ? "true" : "false", (long long)c->generation,
                                   (long long)c->event_cursor);
        }
        if (st == ATLAS_OK && !c->index_current) {
            const char *reason = not_current_reason_or_other(atlas_buf_cstr(&c->not_current_reason));
            if (reason != NULL) {
                st = atlas_buf_appendf(out, err, "not_current=%s\n", reason);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(out, err, "changed_paths=%lld unresolved_reasons=%lld\n",
                                   (long long)c->changed_paths, (long long)c->unresolved_reasons);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(out, err,
                                   "decisions_proposed=%lld decisions_approved=%lld\n",
                                   (long long)c->proposed_decisions,
                                   (long long)c->approved_decisions);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_appendf(out, err, "session=%lld change_set=%lld\n",
                                   (long long)c->session_id, (long long)c->change_set_id);
        }
    }
    if (st != ATLAS_OK) {
        return st;
    }

    /* The one sentence of instruction Atlas gives, and it is a constraint rather
     * than a capability: it tells the reader what this block is and what it is
     * not, so that the absence of repository text is legible rather than merely
     * true. */
    st = atlas_buf_append_str(out,
                              "note=Atlas facts only. No repository name, path or text is "
                              "included here. Use the Atlas MCP tools for those, where they "
                              "arrive labelled UNTRUSTED_DATA.\n"
                              "</atlas-context>\n",
                              err);
    if (st != ATLAS_OK) {
        return st;
    }

    /* The renderer checks its own output. If this ever fails it is a bug in this
     * file rather than in its input, and failing loudly beats emitting a
     * document that violates the guarantee the surrounding documentation makes. */
    if (!atlas_ai_context_is_bounded(atlas_buf_cstr(out), out->len)) {
        atlas_buf_reset(out);
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                             "the Atlas context envelope failed its own bounds check and was "
                             "discarded rather than injected");
    }
    return ATLAS_OK;
}

/* Atlas - A12.1 T7: the deterministic extractor and its anchors.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_memory_extract` is pure -- no database handle, no process, no file,
 * no clock, no allocation beyond `atlas_buf`'s own -- exactly `src/orch/
 * memory.c`'s discipline and for the same reason: a frozen result a reader
 * can re-derive from stored bytes is only checkable if the derivation
 * consulted nothing that moves. Every byte comparison here is against a fixed
 * ASCII table, never `<ctype.h>`, because a locale-sensitive comparison would
 * make the same bytes split differently on two machines and nothing
 * downstream would notice -- the failure this file exists to not have.
 *
 * `atlas_memory_anchor_resolve` is the impure half, kept in this file rather
 * than a second one because the two are one contract (split, then resolve)
 * and the split/resolve boundary is a function boundary, not a file
 * boundary. It reads `atlas/verify.h`'s three bounded index reads plus the
 * decision store's uid lookup, and nothing else: no git process, no file
 * read, so it is safe to call from inside the write transaction T8 calls it
 * from.
 */
#define _GNU_SOURCE 1

#include "atlas/memory.h"

#include <string.h>

#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/sha256.h"
#include "atlas/verify.h"

/* --- ASCII-only byte classification ----------------------------------------
 *
 * `<ctype.h>` is locale-sensitive even for what looks like an ASCII check --
 * `isalpha` on a byte outside 0..127 is undefined outside the "C" locale, and
 * some locales reclassify bytes even inside it. Every predicate here is a
 * fixed table over specific byte values, so the same bytes classify the same
 * way on every machine this ever runs on. */
static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_alnum(unsigned char c) { return is_digit(c) || is_alpha(c); }
static bool is_hspace(unsigned char c) { return c == ' ' || c == '\t'; }
static bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static unsigned char lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}
static bool is_lower_hex(unsigned char c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }

void atlas_memory_anchor_init(atlas_memory_anchor *a) {
    if (a == NULL) {
        return;
    }
    a->kind = ATLAS_MEMORY_ANCHOR_UNKNOWN;
    atlas_buf_init(&a->value);
}

void atlas_memory_anchor_free(atlas_memory_anchor *a) {
    if (a == NULL) {
        return;
    }
    atlas_buf_free(&a->value);
}

void atlas_memory_proposition_init(atlas_memory_proposition *p) {
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof *p);
    atlas_buf_init(&p->text);
    atlas_buf_init(&p->normalized);
    atlas_buf_init(&p->text_sha256);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION; i++) {
        atlas_memory_anchor_init(&p->anchors[i]);
    }
    atlas_buf_init(&p->verifier_input);
    atlas_buf_init(&p->decision_uid);
    /* semantics == ATLAS_CLAIM_DESCRIPTIVE and verifier == ATLAS_VERIFIER_NONE
     * fall out of the memset above -- both are zero, per the house rule. */
}

void atlas_memory_proposition_free(atlas_memory_proposition *p) {
    if (p == NULL) {
        return;
    }
    atlas_buf_free(&p->text);
    atlas_buf_free(&p->normalized);
    atlas_buf_free(&p->text_sha256);
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION; i++) {
        atlas_memory_anchor_free(&p->anchors[i]);
    }
    atlas_buf_free(&p->verifier_input);
    atlas_buf_free(&p->decision_uid);
}

/* --- normalisation, frozen (see atlas/memory.h and the plan's §Decision on
 * normalisation): strip a leading list marker and its whitespace; strip
 * emphasis runs of `*`/`_` at a word boundary; collapse whitespace runs to
 * one space; trim; lowercase ASCII bytes only. A change here bumps
 * ATLAS_MEMORY_EXTRACTOR_VERSION. -------------------------------------------- */

static atlas_status normalize_into(atlas_buf *out, const char *data, size_t len, atlas_err *err) {
    atlas_buf_reset(out);

    /* Step 1: a leading list marker, at position 0 only -- a paragraph
     * candidate can never legitimately start with one, because the split
     * step below classifies any line matching this same shape as its own
     * bullet candidate before a paragraph can absorb it. */
    size_t start = 0;
    if (len > 0) {
        unsigned char c0 = (unsigned char)data[0];
        if ((c0 == '-' || c0 == '*' || c0 == '+') && len > 1 && is_hspace((unsigned char)data[1])) {
            start = 1;
            while (start < len && is_hspace((unsigned char)data[start])) {
                start++;
            }
        } else if (is_digit(c0)) {
            size_t k = 0;
            while (k < len && is_digit((unsigned char)data[k])) {
                k++;
            }
            if (k < len && data[k] == '.' && k + 1 < len && is_hspace((unsigned char)data[k + 1])) {
                start = k + 1;
                while (start < len && is_hspace((unsigned char)data[start])) {
                    start++;
                }
            }
        }
    }

    /* Step 2: strip emphasis runs of `*`/`_` at a word boundary. A run is
     * dropped exactly when one side is an ASCII alnum byte and the other is
     * not -- which is what "the same length is at start of the world versus
     * inside one word" comes down to -- so `foo_bar` (alnum on both sides of
     * the underscore) is never touched and `**bold**` (space, or the string
     * boundary, on the outside) always is. */
    atlas_buf stage1 = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    size_t i = start;
    while (i < len && st == ATLAS_OK) {
        unsigned char c = (unsigned char)data[i];
        if (c == '*' || c == '_') {
            size_t j = i;
            while (j < len && ((unsigned char)data[j] == '*' || (unsigned char)data[j] == '_')) {
                j++;
            }
            bool before_word = (i > start) && is_alnum((unsigned char)data[i - 1]);
            bool after_word = (j < len) && is_alnum((unsigned char)data[j]);
            if (before_word != after_word) {
                i = j; /* boundary run: dropped */
                continue;
            }
            st = atlas_buf_append(&stage1, data + i, j - i, err);
            i = j;
            continue;
        }
        st = atlas_buf_append_ch(&stage1, (char)c, err);
        i++;
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&stage1);
        return st;
    }

    /* Steps 3-4: collapse whitespace runs to one space, then trim -- done
     * together in one pass by only ever emitting a pending space once a
     * non-whitespace byte has already been emitted, and never emitting one
     * at all if none follows. */
    atlas_buf stage2 = ATLAS_BUF_INIT;
    bool pending_space = false;
    bool any_out = false;
    for (size_t k = 0; k < stage1.len && st == ATLAS_OK; k++) {
        unsigned char c = (unsigned char)stage1.data[k];
        if (is_ws(c)) {
            if (any_out) {
                pending_space = true;
            }
            continue;
        }
        if (pending_space) {
            st = atlas_buf_append_ch(&stage2, ' ', err);
            pending_space = false;
            if (st != ATLAS_OK) {
                break;
            }
        }
        st = atlas_buf_append_ch(&stage2, (char)c, err);
        any_out = true;
    }
    atlas_buf_free(&stage1);
    if (st != ATLAS_OK) {
        atlas_buf_free(&stage2);
        return st;
    }

    /* Step 5: lowercase ASCII bytes only; a byte >= 0x80 passes untouched, so
     * normalisation never needs a UTF-8 opinion. */
    for (size_t k = 0; k < stage2.len; k++) {
        stage2.data[k] = (char)lower((unsigned char)stage2.data[k]);
    }

    st = atlas_buf_set(out, stage2.data, stage2.len, err);
    atlas_buf_free(&stage2);
    return st;
}

/* True for a line with nothing in it but horizontal whitespace (including
 * length zero) -- a paragraph boundary. */
static bool is_blank_line(const char *data, size_t start, size_t end) {
    for (size_t k = start; k < end; k++) {
        if (!is_hspace((unsigned char)data[k])) {
            return false;
        }
    }
    return true;
}

/* True for a line beginning with `-`, `*`, `+`, or one-or-more digits then
 * `.`, each immediately followed by horizontal whitespace. No leading
 * whitespace is tolerated before the marker: an indented continuation is
 * prose, not a second list syntax, which keeps this predicate and
 * normalize_into's marker strip agreeing about what a marker is. */
static bool is_list_item_line(const char *data, size_t start, size_t end) {
    if (start >= end) {
        return false;
    }
    unsigned char c0 = (unsigned char)data[start];
    if (c0 == '-' || c0 == '*' || c0 == '+') {
        return (start + 1 < end) && is_hspace((unsigned char)data[start + 1]);
    }
    if (is_digit(c0)) {
        size_t k = start;
        while (k < end && is_digit((unsigned char)data[k])) {
            k++;
        }
        return k < end && data[k] == '.' && k + 1 < end && is_hspace((unsigned char)data[k + 1]);
    }
    return false;
}

/* Writes one candidate into `out[*n]` (initialising it), or -- once `*n`
 * reaches `effective_cap` -- refuses the write and records the bound instead.
 * `*ordinal` always advances, whether or not the candidate was stored, so a
 * caller comparing two passes with different caps still sees the same
 * ordinal assigned to the same position in the source. */
static atlas_status emit_candidate(atlas_memory_proposition *out, size_t effective_cap, size_t *n,
                                   size_t *ordinal, bool *bound_hit, const char *data, size_t len,
                                   atlas_err *err) {
    size_t this_ordinal = *ordinal;
    *ordinal += 1;
    if (*n >= effective_cap) {
        *bound_hit = true;
        return ATLAS_OK;
    }

    atlas_memory_proposition *p = &out[*n];
    atlas_memory_proposition_init(p);
    p->ordinal = this_ordinal;

    atlas_status st = atlas_buf_set(&p->text, data, len, err);
    if (st == ATLAS_OK) {
        st = normalize_into(&p->normalized, data, len, err);
    }
    if (st == ATLAS_OK) {
        char hex[ATLAS_SHA256_HEX_LEN + 1u];
        atlas_sha256_hex(data, len, hex);
        st = atlas_buf_set_str(&p->text_sha256, hex, err);
    }
    /* Never trimmed: `text` above already holds every byte of `data`,
     * whatever `len` is. This only ever flags that it happened. */
    p->truncated = len > ATLAS_MEMORY_MAX_PROPOSITION_BYTES;

    if (st != ATLAS_OK) {
        atlas_memory_proposition_free(p);
        return st;
    }
    *n += 1;
    return ATLAS_OK;
}

atlas_status atlas_memory_extract(const atlas_buf *bytes, atlas_memory_proposition *out, size_t cap,
                                  size_t *count_out, bool *bound_reached_out, atlas_err *err) {
    if (count_out != NULL) {
        *count_out = 0;
    }
    if (bound_reached_out != NULL) {
        *bound_reached_out = false;
    }
    if (bytes == NULL || out == NULL || cap == 0 || count_out == NULL || bound_reached_out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a memory split needs a byte buffer, at least one output slot, and "
                             "both out-parameters");
    }

    /* The policy ceiling is enforced regardless of how large a buffer the
     * caller happens to have passed -- A5's "refused, never silently
     * exceeded" rule governs this bound as much as the byte one. */
    size_t effective_cap = cap < ATLAS_MEMORY_MAX_PROPOSITIONS ? cap : ATLAS_MEMORY_MAX_PROPOSITIONS;

    const char *data = bytes->data != NULL ? bytes->data : "";
    size_t len = bytes->len;

    size_t n = 0;
    size_t ordinal = 0;
    bool bound_hit = false;
    atlas_buf para = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;

    size_t i = 0;
    while (i < len && st == ATLAS_OK) {
        size_t line_start = i;
        size_t j = i;
        while (j < len && data[j] != '\n') {
            j++;
        }
        size_t line_end = j;
        /* `\r\n` and `\n` are the same line terminator here: a trailing `\r`
         * is line-ending noise, not content a memory file's author wrote, and
         * dropping it is what makes a CRLF document split identically to its
         * LF twin rather than merely producing an equivalent count. */
        if (line_end > line_start && data[line_end - 1] == '\r') {
            line_end--;
        }

        if (is_blank_line(data, line_start, line_end)) {
            if (para.len > 0) {
                st = emit_candidate(out, effective_cap, &n, &ordinal, &bound_hit, para.data,
                                    para.len, err);
                atlas_buf_reset(&para);
            }
        } else if (is_list_item_line(data, line_start, line_end)) {
            if (para.len > 0) {
                st = emit_candidate(out, effective_cap, &n, &ordinal, &bound_hit, para.data,
                                    para.len, err);
                atlas_buf_reset(&para);
            }
            if (st == ATLAS_OK) {
                st = emit_candidate(out, effective_cap, &n, &ordinal, &bound_hit,
                                    data + line_start, line_end - line_start, err);
            }
        } else {
            if (para.len > 0) {
                st = atlas_buf_append_ch(&para, '\n', err);
            }
            if (st == ATLAS_OK) {
                st = atlas_buf_append(&para, data + line_start, line_end - line_start, err);
            }
        }

        i = (j < len) ? j + 1 : len;
    }
    if (st == ATLAS_OK && para.len > 0) {
        st = emit_candidate(out, effective_cap, &n, &ordinal, &bound_hit, para.data, para.len, err);
    }
    atlas_buf_free(&para);

    if (st != ATLAS_OK) {
        for (size_t k = 0; k < n; k++) {
            atlas_memory_proposition_free(&out[k]);
        }
        return st;
    }

    *count_out = n;
    *bound_reached_out = bound_hit;
    return ATLAS_OK;
}

/* --- anchor resolution ------------------------------------------------------
 *
 * Every DB read here is one of the bounded, index-only reads `atlas/verify.h`
 * already exposes for exactly this purpose ("The three bounded reads the
 * deterministic verifiers are built from") plus the decision store's uid
 * lookup -- reused rather than restated, so extraction-time resolution and a
 * later `atlas.symbol_present`/`atlas.content_hash` run are looking at the
 * same fact. */

/* Adds one anchor if there is room and it is not already present (by kind and
 * exact value) -- a token repeated twice in one candidate's text must not
 * spend two of the eight slots on the same fact. A no-op once `p->anchor_count`
 * has reached the cap; the caller's own loop condition is what actually stops
 * scanning once that happens, this is just the last line of defence against
 * writing a ninth. */
static void add_anchor(atlas_memory_proposition *p, atlas_memory_anchor_kind kind,
                       const char *value, size_t value_len, atlas_status *st, atlas_err *err) {
    if (*st != ATLAS_OK || p->anchor_count >= ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION) {
        return;
    }
    for (size_t k = 0; k < p->anchor_count; k++) {
        if (p->anchors[k].kind == kind && p->anchors[k].value.len == value_len &&
            memcmp(p->anchors[k].value.data, value, value_len) == 0) {
            return;
        }
    }
    atlas_memory_anchor *a = &p->anchors[p->anchor_count];
    a->kind = kind;
    *st = atlas_buf_set(&a->value, value, value_len, err);
    if (*st == ATLAS_OK) {
        p->anchor_count++;
    }
}

atlas_status atlas_memory_anchor_resolve(atlas_db *db, int64_t repo_id, atlas_memory_proposition *p,
                                         atlas_err *err) {
    if (db == NULL || p == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "anchor resolution needs a database handle and a proposition");
    }

    /* Idempotent: reset everything resolve() owns before scanning again. */
    for (size_t k = 0; k < ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION; k++) {
        p->anchors[k].kind = ATLAS_MEMORY_ANCHOR_UNKNOWN;
        atlas_buf_reset(&p->anchors[k].value);
    }
    p->anchor_count = 0;
    p->semantics = ATLAS_CLAIM_DESCRIPTIVE;
    p->verifier = ATLAS_VERIFIER_NONE;
    atlas_buf_reset(&p->verifier_input);
    atlas_buf_reset(&p->decision_uid);

    const char *data = p->text.data != NULL ? p->text.data : "";
    size_t len = p->text.len;

    bool has_path = false, has_symbol = false, has_decision = false;
    atlas_buf path_val = ATLAS_BUF_INIT;
    atlas_buf path_hash = ATLAS_BUF_INIT;
    atlas_buf symbol_val = ATLAS_BUF_INIT;
    atlas_buf decision_val = ATLAS_BUF_INIT;
    atlas_buf token = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;

    size_t i = 0;
    while (i < len && st == ATLAS_OK && p->anchor_count < ATLAS_MEMORY_MAX_ANCHORS_PER_PROPOSITION) {
        unsigned char c = (unsigned char)data[i];

        if (c == '`') {
            size_t j = i + 1;
            while (j < len && data[j] != '`') {
                j++;
            }
            if (j >= len) {
                /* No closing backtick: not a token. Move past this one byte
                 * and keep scanning the rest as ordinary text. */
                i++;
                continue;
            }
            size_t tok_len = j - (i + 1);
            if (tok_len > 0) {
                st = atlas_buf_set(&token, data + i + 1, tok_len, err);
                if (st == ATLAS_OK) {
                    const char *tok_c = atlas_buf_cstr(&token);

                    bool found_path = false;
                    atlas_buf hash = ATLAS_BUF_INIT;
                    st = atlas_db_verify_file_hash(db, repo_id, tok_c, &hash, &found_path, err);
                    if (st == ATLAS_OK && found_path) {
                        add_anchor(p, ATLAS_MEMORY_ANCHOR_PATH, tok_c, tok_len, &st, err);
                        if (st == ATLAS_OK && !has_path) {
                            has_path = true;
                            st = atlas_buf_set(&path_val, tok_c, tok_len, err);
                            if (st == ATLAS_OK) {
                                st = atlas_buf_set(&path_hash, hash.data, hash.len, err);
                            }
                        }
                    }
                    atlas_buf_free(&hash);

                    if (st == ATLAS_OK) {
                        int64_t count = 0;
                        st = atlas_db_verify_sem_symbol(db, repo_id, tok_c, &count, NULL, NULL, err);
                        if (st == ATLAS_OK && count > 0) {
                            add_anchor(p, ATLAS_MEMORY_ANCHOR_SYMBOL, tok_c, tok_len, &st, err);
                            if (st == ATLAS_OK && !has_symbol) {
                                has_symbol = true;
                                st = atlas_buf_set(&symbol_val, tok_c, tok_len, err);
                            }
                        }
                    }
                }
            }
            i = j + 1;
            continue;
        }

        if (is_alnum(c) || c == '-') {
            size_t j = i;
            while (j < len && (is_alnum((unsigned char)data[j]) || data[j] == '-')) {
                j++;
            }
            size_t tok_len = j - i;
            st = atlas_buf_set(&token, data + i, tok_len, err);
            if (st == ATLAS_OK) {
                const char *tok_c = atlas_buf_cstr(&token);
                if (atlas_decision_uid_is_valid(tok_c)) {
                    int64_t doc_id = 0, doc_repo_id = 0;
                    bool found = false;
                    st = atlas_db_decision_find_uid(db, tok_c, &doc_id, &doc_repo_id, &found, err);
                    if (st == ATLAS_OK && found && doc_repo_id == repo_id) {
                        add_anchor(p, ATLAS_MEMORY_ANCHOR_DECISION, tok_c, tok_len, &st, err);
                        if (st == ATLAS_OK && !has_decision) {
                            has_decision = true;
                            st = atlas_buf_set(&decision_val, tok_c, tok_len, err);
                        }
                    }
                } else if (tok_len == 40) {
                    bool all_hex = true;
                    for (size_t k = 0; k < tok_len; k++) {
                        if (!is_lower_hex((unsigned char)tok_c[k])) {
                            all_hex = false;
                            break;
                        }
                    }
                    if (all_hex) {
                        bool found = false;
                        st = atlas_db_verify_commit_exists(db, repo_id, tok_c, &found, err);
                        if (st == ATLAS_OK && found) {
                            add_anchor(p, ATLAS_MEMORY_ANCHOR_COMMIT, tok_c, tok_len, &st, err);
                        }
                    }
                }
            }
            i = j;
            continue;
        }

        i++;
    }
    atlas_buf_free(&token);

    if (st == ATLAS_OK) {
        /* §Decision 4. SYMBOL takes precedence over PATH when both resolved
         * (a token can be both, and per-anchor both are still recorded above
         * -- this only decides the *claim's* semantics and verifier); a
         * DECISION anchor never changes which of the two applies, it only
         * adds `decision_uid`. */
        if (has_symbol) {
            p->semantics = ATLAS_CLAIM_DESCRIPTIVE;
            p->verifier = ATLAS_VERIFIER_SYMBOL_PRESENT;
            st = atlas_buf_appendf(&p->verifier_input, err, "symbol=%s", atlas_buf_cstr(&symbol_val));
        } else if (has_path) {
            p->semantics = ATLAS_CLAIM_DESCRIPTIVE;
            p->verifier = ATLAS_VERIFIER_CONTENT_HASH;
            st = atlas_buf_appendf(&p->verifier_input, err, "path=%s;sha256=%s",
                                   atlas_buf_cstr(&path_val), atlas_buf_cstr(&path_hash));
        } else if (has_decision) {
            p->semantics = ATLAS_CLAIM_NORMATIVE;
            p->verifier = ATLAS_VERIFIER_NONE;
        } else {
            p->semantics = ATLAS_CLAIM_DESCRIPTIVE;
            p->verifier = ATLAS_VERIFIER_NONE;
        }
        if (st == ATLAS_OK && has_decision) {
            st = atlas_buf_set(&p->decision_uid, decision_val.data, decision_val.len, err);
        }
    }

    atlas_buf_free(&path_val);
    atlas_buf_free(&path_hash);
    atlas_buf_free(&symbol_val);
    atlas_buf_free(&decision_val);
    return st;
}

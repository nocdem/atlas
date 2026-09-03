/* Atlas - A12.1 T15: the proposed patch.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * `atlas_memory_patch_build` is the whole of this file: one function, plus
 * the line-location and unified-diff-rendering machinery it needs. See the
 * doc comment on the declaration in `include/atlas/memory.h` for what it
 * proposes and why; this header covers only what is specific to *this*
 * implementation.
 *
 * **Locating a proposition in its own source, without re-deriving the
 * split.** `atlas_memory_extract` (`src/memory/extract.c`) hands back each
 * candidate's own verbatim `text` -- for a list item, exactly one physical
 * line's bytes; for a paragraph, several consecutive physical lines joined by
 * a single `\n`, each already stripped of its own trailing `\r` -- but no
 * line number. Re-deriving *which* lines are blank, a list item or a
 * paragraph continuation here would be a second copy of the extractor's own
 * frozen split (`ATLAS_MEMORY_EXTRACTOR_VERSION` governs that one, in
 * extract.c, and nowhere else). This file does not do that. It only re-splits
 * the source into physical lines -- a `\n`-delimited walk with a trailing
 * `\r` dropped per line, byte-for-byte the same transform `atlas_memory_
 * extract`'s own line walk already applies -- and then, for each candidate in
 * document order, finds the shortest run of consecutive physical lines whose
 * `\n`-joined content matches the candidate's `text` exactly, advancing a
 * monotonic cursor. Because a candidate's `text` is by construction an exact
 * contiguous run of physical lines (extract.c never fabricates or reorders
 * bytes), this always succeeds; a failure to locate one is an internal
 * inconsistency between the two walks, not a malformed source, and is refused
 * rather than guessed past.
 *
 * **Correlating a fresh candidate to a stored claim, read-only.** The same
 * shape as `src/memory/reconcile.c`'s `classify_candidate`/`find_prior_cb`
 * (T9's cross-generation diff), reused here as a read: `memory_claim_anchors`
 * is asked which claim uids were ever anchored to this candidate's first
 * anchor tuple (`atlas_db_memory_anchor_claim_uids`), and the one whose
 * stored `text` matches this candidate's byte for byte is "the" claim this
 * line currently states -- oldest-first traversal, last match wins, exactly
 * as that function's own contract says. No claim is created, no anchor row is
 * written, and no candidate is submitted anywhere: this file never opens a
 * write transaction and never calls `atlas_verify_intake_apply_in_tx`.
 */
#include "atlas/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/pathrep.h"
#include "atlas/safetext.h"
#include "atlas/verify.h"

/* --- physical lines ---------------------------------------------------------
 *
 * One line's byte range within its source's own bytes, trailing `\r`
 * dropped -- `atlas_memory_extract`'s own per-line transform, reproduced
 * here (not reused: it is a four-line loop body, not a shared entry point,
 * and duplicating it is disclosed in the T15 report rather than exposing a
 * new symbol from extract.c for one caller). */
typedef struct patch_line {
    size_t start;
    size_t len;
} patch_line;

#define PATCH_CTX_LINES 3u
#define PATCH_UID_MAX 96

static atlas_status split_physical_lines(const atlas_buf *bytes, patch_line **lines_out,
                                         size_t *nlines_out, atlas_err *err) {
    *lines_out = NULL;
    *nlines_out = 0;
    const char *data = bytes->data != NULL ? bytes->data : "";
    size_t len = bytes->len;
    /* At most one line per byte (a source of nothing but blank lines);
     * transient and freed before this function returns to its own caller. */
    patch_line *lines = calloc(len + 1u, sizeof *lines);
    if (lines == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory splitting a source into lines");
    }
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        size_t j = i;
        while (j < len && data[j] != '\n') {
            j++;
        }
        size_t line_end = j;
        if (line_end > line_start && data[line_end - 1] == '\r') {
            line_end--;
        }
        lines[n].start = line_start;
        lines[n].len = line_end - line_start;
        n++;
        i = (j < len) ? j + 1 : len;
    }
    *lines_out = lines;
    *nlines_out = n;
    return ATLAS_OK;
}

/* True when physical lines `[start, start+span)`, `\n`-joined, equal `text`
 * exactly. */
static bool lines_join_matches(const char *data, const patch_line *lines, size_t start, size_t span,
                               const atlas_buf *text) {
    size_t need = 0;
    for (size_t k = 0; k < span; k++) {
        need += lines[start + k].len;
        if (k + 1u < span) {
            need += 1u;
        }
    }
    if (need != text->len) {
        return false;
    }
    const char *t = text->data != NULL ? text->data : "";
    size_t pos = 0;
    for (size_t k = 0; k < span; k++) {
        const patch_line *ln = &lines[start + k];
        if (ln->len > 0 && memcmp(data + ln->start, t + pos, ln->len) != 0) {
            return false;
        }
        pos += ln->len;
        if (k + 1u < span) {
            if (t[pos] != '\n') {
                return false;
            }
            pos++;
        }
    }
    return true;
}

/* --- correlating a fresh candidate to the claim it currently states -------- */

typedef struct claim_match_ctx {
    atlas_db *db;
    const char *match_text;
    size_t match_text_len;
    bool found;
    char uid[PATCH_UID_MAX];
} claim_match_ctx;

static atlas_status claim_match_cb(const char *claim_uid, void *ud, atlas_err *err) {
    claim_match_ctx *mc = ud;
    atlas_verify_claim cand;
    atlas_verify_claim_init(&cand);
    bool cfound = false;
    atlas_status st = atlas_db_verify_claim_find(mc->db, claim_uid, &cand, &cfound, err);
    if (st != ATLAS_OK) {
        atlas_verify_claim_free(&cand);
        return st;
    }
    bool matches = cfound && cand.text.len == mc->match_text_len &&
                  memcmp(cand.text.data != NULL ? cand.text.data : "", mc->match_text,
                        mc->match_text_len) == 0;
    atlas_verify_claim_free(&cand);
    if (matches) {
        (void)snprintf(mc->uid, sizeof mc->uid, "%s", claim_uid);
        mc->found = true;
    }
    return ATLAS_OK;
}

/* --- the three absolutes every deletion arm must hold ----------------------
 *
 * `include/atlas/memory.h`'s own doc comment on `atlas_memory_patch_build`
 * states, as an absolute and not a per-arm choice: NORMATIVE semantics, an
 * IMPLEMENTATION conflict, and a stale verdict never reach a hunk, on ANY arm
 * that proposes one. Fix round (I1) restored the first two guards to the
 * SUPERSEDED arm below after the shipped predicate carried neither; its own
 * re-review found the identical shape once more, one guard narrower --
 * `det_contradicted` carried all three absolutes and `superseded` carried
 * only two, so the `||` between them let a *stale* verdict reach a hunk
 * through the SUPERSEDED arm alone, the exact failure I1 exists to prevent.
 *
 * Filed as two separate findings against two separate arms (I1's NORMATIVE
 * and IMPLEMENTATION guards, this round's `stale` guard), the shape recurs
 * every time a third arm is added and somebody restates the guards by hand
 * instead of by construction: two guards get copied, the third is wherever
 * the author's attention was that day. Fix round (R1, this round): the fix
 * is to stop each arm from stating its own copy of the three absolutes at
 * all, AND to make the disjunction itself pass through this function rather
 * than sit beside it. `patch_may_delete`'s caller (below) has exactly ONE
 * call site: every arm's kind-specific test is ORed *inside* `kind_ok`, never
 * computed as its own named `bool` and ORed into an `if` from outside this
 * function. Two separate calls -- one per arm, each ANDing the three absolutes
 * in on its own -- would still let a fourth arm add a *third* call, or a bare
 * `bool fourth = ...;` ORed straight into the `if`, bypassing this function
 * entirely while reading exactly like a correct edit; that shape was this
 * function's own first draft, and it is what "inherits by construction" has
 * to survive to be true. With one call, a fourth arm's natural edit is `||
 * fourth_kind` inside the parenthesised argument at that call site, which
 * inherits the three absolutes by construction; reaching a deletion any other
 * way needs a visibly separate `if` outside this function's one call, which
 * is what a reviewer is already looking for. */
static bool patch_may_delete(bool kind_ok, atlas_verify_claim_semantics semantics,
                             atlas_verify_conflict conflict, bool stale) {
    return kind_ok && semantics == ATLAS_CLAIM_DESCRIPTIVE &&
          conflict != ATLAS_CONFLICT_IMPLEMENTATION && !stale;
}

/* --- rendering a read obstacle's outcome, as a fixed vocabulary label ------
 *
 * Exhaustive (`-Wswitch-enum`, no `default:`), `reconcile.c`'s own
 * `read_outcome_label` shape -- a second small copy rather than a shared
 * symbol, this codebase's existing practice for a closed, file-local lookup
 * (pack.c's netstring writer and tokenizer disclose the same choice). Every
 * label is a fixed C string literal, never repository content, so nothing
 * here needs `atlas_safe()`. */
static const char *outcome_label(atlas_memory_read_outcome o) {
    switch (o) {
    case ATLAS_MEMORY_READ_UNKNOWN:
        return "UNKNOWN";
    case ATLAS_MEMORY_READ_OK:
        return "OK";
    case ATLAS_MEMORY_READ_ABSENT:
        return "ABSENT";
    case ATLAS_MEMORY_READ_TOO_LARGE:
        return "TOO_LARGE";
    case ATLAS_MEMORY_READ_NOT_OURS:
        return "NOT_OURS";
    case ATLAS_MEMORY_READ_NO_MIRROR:
        return "NO_MIRROR";
    case ATLAS_MEMORY_READ_SYMLINK:
        return "SYMLINK";
    case ATLAS_MEMORY_READ_NOT_MIRRORED:
        return "NOT_MIRRORED";
    }
    return "UNKNOWN";
}

static atlas_status emit_finding(atlas_buf *findings_out, const char *reason, const char *path_text,
                                 const char *claim_uid, size_t ordinal, atlas_err *err) {
    atlas_status st = atlas_buf_appendf(findings_out, err, "%s path=%s", reason, path_text);
    if (st == ATLAS_OK && claim_uid != NULL) {
        st = atlas_buf_appendf(findings_out, err, " claim=%s", claim_uid);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_appendf(findings_out, err, " ordinal=%zu\n", ordinal);
    }
    return st;
}

/* --- rendering the unified diff --------------------------------------------
 *
 * Standard `diff -u` shape for a deletion-only change: context radius
 * `PATCH_CTX_LINES` on each side of a contiguous deleted run, adjacent or
 * overlapping expanded windows merged into one hunk. `prefix[i]` is the
 * count of kept (non-deleted) physical lines strictly before index `i`,
 * which is what lets a hunk's own "+" side line number be read off without
 * re-scanning the whole file per hunk. */
static atlas_status emit_one_hunk(atlas_buf *diff_out, const char *data, const patch_line *lines,
                                  const size_t *prefix, const bool *del, size_t hs, size_t he,
                                  atlas_err *err) {
    size_t old_count = he - hs;
    size_t new_count = 0;
    for (size_t k = hs; k < he; k++) {
        if (!del[k]) {
            new_count++;
        }
    }
    size_t new_start = prefix[hs] + (new_count > 0 ? 1u : 0u);
    atlas_status st = atlas_buf_appendf(diff_out, err, "@@ -%zu,%zu +%zu,%zu @@\n", hs + 1u, old_count,
                                        new_start, new_count);
    for (size_t k = hs; k < he && st == ATLAS_OK; k++) {
        st = atlas_buf_append_ch(diff_out, del[k] ? '-' : ' ', err);
        /* The source's own bytes, safe-encoded at the point of output --
         * repository content is untrusted input (CLAUDE.md), applied to a
         * diff line exactly as it is to a rendered claim in pack.c. Nothing
         * Atlas authored is on this line: the '-'/' ' prefix and the '\n'
         * terminator are the diff's own structural formatting, not prose. */
        if (st == ATLAS_OK) {
            st = atlas_text_encode_safe(data + lines[k].start, lines[k].len, diff_out, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(diff_out, '\n', err);
        }
    }
    return st;
}

static atlas_status render_hunks(const char *path_text, const char *data, const patch_line *lines,
                                 size_t nlines, const bool *del, atlas_buf *diff_out,
                                 bool *any_out, atlas_err *err) {
    if (any_out != NULL) {
        *any_out = false;
    }
    bool any_delete = false;
    for (size_t i = 0; i < nlines; i++) {
        if (del[i]) {
            any_delete = true;
            break;
        }
    }
    if (!any_delete) {
        return ATLAS_OK;
    }

    size_t *prefix = calloc(nlines + 1u, sizeof *prefix);
    if (prefix == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory rendering a patch");
    }
    for (size_t i = 0; i < nlines; i++) {
        prefix[i + 1u] = prefix[i] + (del[i] ? 0u : 1u);
    }

    atlas_status st = atlas_buf_appendf(diff_out, err, "--- a/%s\n+++ b/%s\n", path_text, path_text);

    bool have_pending = false;
    size_t phs = 0, phe = 0;
    size_t i = 0;
    while (st == ATLAS_OK && i < nlines) {
        if (!del[i]) {
            i++;
            continue;
        }
        size_t run_start = i;
        while (i < nlines && del[i]) {
            i++;
        }
        size_t run_end = i; /* exclusive */
        size_t hs = run_start > PATCH_CTX_LINES ? run_start - PATCH_CTX_LINES : 0u;
        size_t he = (run_end + PATCH_CTX_LINES < nlines) ? run_end + PATCH_CTX_LINES : nlines;
        if (have_pending && hs <= phe) {
            if (he > phe) {
                phe = he;
            }
        } else {
            if (have_pending) {
                st = emit_one_hunk(diff_out, data, lines, prefix, del, phs, phe, err);
            }
            phs = hs;
            phe = he;
            have_pending = true;
        }
    }
    if (st == ATLAS_OK && have_pending) {
        st = emit_one_hunk(diff_out, data, lines, prefix, del, phs, phe, err);
    }

    free(prefix);
    if (st == ATLAS_OK && any_out != NULL) {
        *any_out = true;
    }
    return st;
}

/* --- one item (one file's worth of bytes) ----------------------------------
 *
 * `item_path_text` is already `%XX`-encoded (this file's own convention,
 * `path_text` throughout the codebase); `bytes` are the item's own raw
 * content, exactly as `atlas_memory_read_source`/`atlas_db_memory_version_
 * latest` returned it -- HEAD's blob for a tracked REPO_FILE, the plain
 * filesystem bytes otherwise. */
static atlas_status process_item(atlas_db *db, int64_t repo_id, const char *item_path_text,
                                 const atlas_buf *bytes, atlas_memory_read_outcome outcome,
                                 atlas_buf *diff_out, atlas_buf *findings_out, atlas_err *err) {
    if (outcome != ATLAS_MEMORY_READ_OK) {
        /* A read obstacle is a fact about this item, reported rather than
         * silently skipped (A9.2.5's rule, one layer over: an obstacle is
         * recorded with its exact cause, not the first reason and no path) --
         * but there are no bytes here to propose anything about. */
        return atlas_buf_appendf(findings_out, err, "UNREADABLE path=%s outcome=%s\n", item_path_text,
                                 outcome_label(outcome));
    }

    atlas_memory_proposition *cands = calloc(ATLAS_MEMORY_MAX_PROPOSITIONS, sizeof *cands);
    if (cands == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory splitting a source's content");
    }
    size_t got = 0;
    bool bound_reached = false;
    atlas_status st =
        atlas_memory_extract(bytes, cands, ATLAS_MEMORY_MAX_PROPOSITIONS, &got, &bound_reached, err);
    if (st == ATLAS_OK && bound_reached) {
        for (size_t k = 0; k < got; k++) {
            atlas_memory_proposition_free(&cands[k]);
        }
        free(cands);
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "%s holds more propositions than a patch build may examine; refused "
                             "rather than silently incomplete",
                             item_path_text);
    }
    if (st != ATLAS_OK) {
        free(cands);
        return st;
    }

    patch_line *lines = NULL;
    size_t nlines = 0;
    st = split_physical_lines(bytes, &lines, &nlines, err);
    bool *del = NULL;
    if (st == ATLAS_OK) {
        del = calloc(nlines > 0 ? nlines : 1u, sizeof *del);
        if (del == NULL) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory rendering a patch");
        }
    }

    const char *data = bytes->data != NULL ? bytes->data : "";
    size_t cursor = 0;
    for (size_t k = 0; k < got && st == ATLAS_OK; k++) {
        atlas_memory_proposition *p = &cands[k];
        size_t span = 1u;
        const char *ptext = p->text.data != NULL ? p->text.data : "";
        for (size_t z = 0; z < p->text.len; z++) {
            if (ptext[z] == '\n') {
                span++;
            }
        }
        bool located = false;
        size_t line_start = 0;
        while (cursor + span <= nlines) {
            if (lines_join_matches(data, lines, cursor, span, &p->text)) {
                line_start = cursor;
                cursor += span;
                located = true;
                break;
            }
            cursor++;
        }
        if (!located) {
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                               "a split proposition could not be located in its own source's "
                               "physical lines");
            break;
        }
        size_t line_end = line_start + span - 1u;

        st = atlas_memory_anchor_resolve(db, repo_id, p, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (p->anchor_count == 0) {
            continue; /* unanchored: neither a hunk nor a finding */
        }

        claim_match_ctx mc;
        memset(&mc, 0, sizeof mc);
        mc.db = db;
        mc.match_text = ptext;
        mc.match_text_len = p->text.len;
        st = atlas_db_memory_anchor_claim_uids(db, repo_id, p->anchors[0].kind,
                                               atlas_buf_cstr(&p->anchors[0].value), claim_match_cb,
                                               &mc, err);
        if (st != ATLAS_OK) {
            break;
        }
        if (!mc.found) {
            /* Resolved an anchor, but no live claim carries this exact text
             * -- nothing this pass can correlate an assessment to. Treated
             * the same as unanchored, for the same A9.2.2 reason: absence of
             * a correlated claim is not evidence about the line either
             * way. */
            continue;
        }

        atlas_verify_claim claim;
        atlas_verify_claim_init(&claim);
        bool cfound = false;
        st = atlas_db_verify_claim_find(db, mc.uid, &claim, &cfound, err);
        if (st != ATLAS_OK) {
            atlas_verify_claim_free(&claim);
            break;
        }
        if (!cfound) {
            /* claim_match_cb just confirmed this uid's text live under the
             * single-writer rule (A1); gone by the time it is re-fetched
             * here cannot happen in production. Fail closed rather than
             * silently drop the correlation, reconcile.c's own precedent for
             * exactly this shape. */
            atlas_verify_claim_free(&claim);
            st = atlas_err_set(err, ATLAS_ERR_INTERNAL,
                               "a claim confirmed live by its own text vanished before it could "
                               "be re-read");
            break;
        }

        atlas_verify_state state = ATLAS_VERIFY_UNVERIFIED;
        atlas_verify_conflict conflict = ATLAS_CONFLICT_NONE;
        atlas_verify_basis basis = ATLAS_VERIFY_BASIS_UNKNOWN;
        bool stale = false;
        bool result_found = false;
        st = atlas_db_verify_result_latest(db, claim.id, &state, &conflict, &stale, &basis, &result_found,
                                           err);
        if (st != ATLAS_OK) {
            atlas_verify_claim_free(&claim);
            break;
        }

        atlas_memory_diff_kind last_kind = ATLAS_MEMORY_DIFF_UNKNOWN;
        bool diff_found = false;
        st = atlas_db_memory_claim_diff_last_kind(db, repo_id, mc.uid, &last_kind, &diff_found, err);
        if (st != ATLAS_OK) {
            atlas_verify_claim_free(&claim);
            break;
        }

        /* Exactly one call to `patch_may_delete` decides `del[]` for this
         * candidate, and each arm's own kind-specific test is only ever ORed
         * *inside* that one call's first argument -- never computed as its
         * own `bool` and ORed into the `if` from outside it. That is what
         * makes the three absolutes structural rather than a convention every
         * arm's author has to remember: with two separate calls (this
         * function's first draft), a fourth arm could write its own
         * `bool fourth = ...;` and `if (superseded || det_contradicted ||
         * fourth)`, and that edit is indistinguishable at the `if` from a
         * correct one -- it compiles, it reads naturally, and it reaches
         * `del[z] = true` having asked `patch_may_delete` nothing. With one
         * call, a fourth arm's natural edit is `|| fourth_kind` *inside* the
         * parenthesised first argument below, which inherits the guard by
         * construction; reaching `del[z] = true` any other way needs a
         * visibly separate `if`, which is what a reviewer is looking for
         * regardless.
         *
         * `superseded_kind` is just "this claim's most recently recorded diff
         * kind is SUPERSEDED". `basis` is deliberately not part of it, and
         * that is a conclusion, not an omission: "deterministically
         * CONTRADICTED" is a `verify_results` row's own pair (`state`,
         * `basis`), and SUPERSEDED is not a `verify_results` verdict under
         * either of the two ways this project's own documents describe it --
         * a text fact ("the source's new version no longer contains the
         * proposition") or a claim-lineage fact (`verify_claims.
         * superseded_by_claim_id`) -- neither of which any verifier
         * establishes.
         *
         * The whole-branch review's C1 fix gave the claim-lineage half a
         * writer (`ATLAS_VERIFY_OP_CLAIM_SUPERSEDE`, `src/verify/intake.c`,
         * called from `classify_candidate`), and this comment is updated
         * rather than left to read as though it had not: `superseded_kind`
         * here reads `memory_claim_diffs.kind`, a *different* table that
         * fix's writer never touches, so this diff kind still has no producer
         * anywhere in `src/` (`docs/backlog.md` records it as its own,
         * deliberately unfixed, finding). `verify_claims.
         * superseded_by_claim_id` retiring a predecessor row does not, by
         * itself, make `patch_may_delete`'s SUPERSEDED arm reachable from a
         * real reconciliation pass -- see `include/atlas/memory.h`'s own
         * comment on `atlas_db_memory_anchor_prune_one` for how the two
         * writers stay apart.
         *
         * `det_contradicted_kind` is "deterministically CONTRADICTED" --
         * `state == CONTRADICTED && basis == DETERMINISTIC`, T15's context,
         * verbatim, and nothing else. `basis` was not available from
         * `atlas_db_verify_result_latest` before this task widened it;
         * approximating it from `algorithm`, the conflict kind or the
         * confidence score is exactly what A9.2's "a model cannot become a
         * tool" rule forbids.
         *
         * Neither kind-specific test states `conflict` or `stale`: those are
         * read off the claim's latest `verify_results` row *whether or not
         * that row has anything to do with either kind's own condition* --
         * they are two of the three absolutes that apply to every claim's
         * every verdict, not a description of either kind itself, which is
         * why `patch_may_delete` is what supplies them rather than a term
         * written into either test. */
        bool superseded_kind = diff_found && last_kind == ATLAS_MEMORY_DIFF_SUPERSEDED;
        bool det_contradicted_kind = result_found && state == ATLAS_VERIFY_CONTRADICTED &&
                                     basis == ATLAS_VERIFY_BASIS_DETERMINISTIC;

        if (patch_may_delete(superseded_kind || det_contradicted_kind, claim.semantics, conflict,
                             stale)) {
            for (size_t z = line_start; z <= line_end; z++) {
                del[z] = true;
            }
        } else {
            /* IMPLEMENTATION is excluded on purpose (T15's context, §2): the
             * code diverged from what was approved, and the approved thing
             * is not the thing that is wrong. A finding, never a hunk.
             *
             * Fix round (M5): the label no longer requires
             * `basis == DETERMINISTIC`. This is display only -- IMPLEMENTATION
             * already keeps the line out of a hunk on any basis, both above
             * (`det_contradicted` never fires when `conflict ==
             * ATLAS_CONFLICT_IMPLEMENTATION`, unconditionally) and via the
             * SUPERSEDED arm's own IMPLEMENTATION guard, so no deletion path
             * moves. Requiring DETERMINISTIC here only decided which *label*
             * an already-excluded line got: an EMPIRICAL- or JUDGMENT-basis
             * IMPLEMENTATION conflict fell through to the generic "RETAINED"
             * instead of naming the drift, losing the one signal the finding
             * vocabulary exists to carry -- IMPLEMENTATION_DRIFT reports what
             * the stored `conflict` column already says, not a new
             * determination this function makes, so it is accurate on any
             * basis.
             *
             * A stale, deterministically CONTRADICTED line (I2, above) falls
             * through to plain "RETAINED" rather than a dedicated label, by
             * contrast: I2's ruling excludes it from deletion, and the brief
             * asked only for that exclusion, not a new vocabulary member.
             * Adding one was not requested and is left for whoever next
             * extends the finding vocabulary deliberately, the same
             * discipline `docs/extending.md` asks of every other one. */
            const char *reason = "RETAINED";
            if (result_found && state == ATLAS_VERIFY_CONTRADICTED &&
               conflict == ATLAS_CONFLICT_IMPLEMENTATION) {
                reason = "IMPLEMENTATION_DRIFT";
            } else if (claim.semantics != ATLAS_CLAIM_DESCRIPTIVE) {
                reason = "NORMATIVE";
            }
            st = emit_finding(findings_out, reason, item_path_text, mc.uid, p->ordinal, err);
        }
        atlas_verify_claim_free(&claim);
    }

    if (st == ATLAS_OK) {
        st = render_hunks(item_path_text, data, lines, nlines, del, diff_out, NULL, err);
    }

    free(del);
    free(lines);
    for (size_t k = 0; k < got; k++) {
        atlas_memory_proposition_free(&cands[k]);
    }
    free(cands);
    return st;
}

atlas_status atlas_memory_patch_build(atlas_db *db, const atlas_repo_info *repo,
                                      const char *data_dir, const char *source_uid,
                                      atlas_buf *diff_out, atlas_buf *findings_out, atlas_err *err) {
    if (diff_out != NULL) {
        atlas_buf_reset(diff_out);
    }
    if (findings_out != NULL) {
        atlas_buf_reset(findings_out);
    }
    if (db == NULL || repo == NULL || data_dir == NULL || source_uid == NULL || source_uid[0] == '\0' ||
       diff_out == NULL || findings_out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "a patch build needs a repository, a data directory, a source uid and "
                             "both output buffers");
    }

    int64_t source_id = 0, source_repo_id = 0;
    atlas_memory_source_class cls = ATLAS_MEMORY_SOURCE_UNKNOWN;
    atlas_buf path_raw = ATLAS_BUF_INIT;
    atlas_buf path_text = ATLAS_BUF_INIT;
    bool found = false;
    atlas_status st = atlas_db_memory_source_by_uid(db, source_uid, &source_id, &source_repo_id, &cls,
                                                    &path_raw, &path_text, &found, err);
    if (st == ATLAS_OK && !found) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE, "no such registered memory source");
    }
    if (st == ATLAS_OK && source_repo_id != repo->id) {
        st = atlas_err_set(err, ATLAS_ERR_USAGE,
                           "the named source belongs to a different repository than the one given");
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&path_raw);
        atlas_buf_free(&path_text);
        return st;
    }

    bool is_repo_cls = atlas_memory_source_class_is_repo(cls);
    atlas_memory_read_item items[ATLAS_MEMORY_MAX_DIR_ENTRIES];
    size_t item_count = 0;
    for (size_t i = 0; i < ATLAS_MEMORY_MAX_DIR_ENTRIES; i++) {
        atlas_memory_read_item_init(&items[i]);
    }
    atlas_memory_version_row ext_latest;
    atlas_memory_version_row_init(&ext_latest);
    bool ext_found = false;

    if (is_repo_cls) {
        bool from_mirror = false;
        st = atlas_memory_read_source(repo, data_dir, cls, path_raw.data, path_raw.len, items,
                                      ATLAS_MEMORY_MAX_DIR_ENTRIES, &item_count, &from_mirror, err);
    } else {
        /* EXTERNAL_*: this file never reads one itself, exactly as
         * `atlas_memory_observe`'s own EXTERNAL_* shape -- a different
         * principal reads it (T11's `memory.put`), and this reads back what
         * that principal already stored. */
        st = atlas_db_memory_version_latest(db, source_id, &ext_latest, &ext_found, err);
    }

    for (size_t i = 0; st == ATLAS_OK && i < item_count; i++) {
        atlas_buf item_path = ATLAS_BUF_INIT;
        if (items[i].rel_path.len > 0) {
            /* `rel_path` is raw filesystem bytes (`src/memory/read.c`'s
             * `d_name`), never `%XX`-encoded on its own -- encoded here,
             * exactly the way `atlas_memory_observe` encodes a source's own
             * `path_raw` into `path_text`, before it is joined with the
             * source's already-encoded path for display. */
            atlas_buf enc = ATLAS_BUF_INIT;
            st = atlas_path_text_encode(items[i].rel_path.data, items[i].rel_path.len, &enc, err);
            if (st == ATLAS_OK) {
                st = atlas_buf_appendf(&item_path, err, "%s/%s", atlas_buf_cstr(&path_text),
                                       atlas_buf_cstr(&enc));
            }
            atlas_buf_free(&enc);
        } else {
            st = atlas_buf_set(&item_path, path_text.data, path_text.len, err);
        }
        if (st == ATLAS_OK) {
            st = process_item(db, repo->id, atlas_buf_cstr(&item_path), &items[i].bytes,
                              items[i].outcome, diff_out, findings_out, err);
        }
        atlas_buf_free(&item_path);
    }

    if (st == ATLAS_OK && !is_repo_cls && ext_found) {
        st = process_item(db, repo->id, atlas_buf_cstr(&path_text), &ext_latest.content,
                          ATLAS_MEMORY_READ_OK, diff_out, findings_out, err);
    }

    if (st == ATLAS_OK && diff_out->len == 0) {
        /* Nothing was proposed anywhere in this source -- said explicitly
         * rather than left for a reader to infer from silence (T15's
         * context, §5): an empty diff on its own cannot distinguish "this
         * pass looked and found every line clean" from "this pass never
         * looked at all". */
        st = atlas_buf_appendf(findings_out, err,
                               "NONE_PROPOSED path=%s source=%s: no deletions proposed for this "
                               "source\n",
                               atlas_buf_cstr(&path_text), source_uid);
    }

    for (size_t i = 0; i < ATLAS_MEMORY_MAX_DIR_ENTRIES; i++) {
        atlas_memory_read_item_free(&items[i]);
    }
    atlas_memory_version_row_free(&ext_latest);
    atlas_buf_free(&path_raw);
    atlas_buf_free(&path_text);
    return st;
}

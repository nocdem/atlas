/* Atlas - A15 T3: the review sheet grammar, refused rather than repaired.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * See `include/atlas/review.h` for what a review sheet is and for the two
 * house rules this file exists to satisfy: no field the walker reads in
 * place of the operator typing the confirmation on `/dev/tty`, and a defect
 * anywhere refuses the whole sheet before any entry is trusted.
 *
 * This file is pure, `src/memory/source.c`'s discipline for the same reason:
 * no database handle, no process, no file, no clock and no repository read.
 * The only two calls out of this translation unit are `atlas_db_check_repo_
 * name` (a pure grammar check with no SQL of its own -- `src/core/service.c`
 * and half a dozen other files under src/core already call it directly, so
 * this is not a new layering path) and `atlas_decision_intent_parse` /
 * `atlas_decision_uid_is_valid` (also pure grammar checks). The parser calls
 * these rather than restating their grammars, so a name the registry would
 * refuse is a name the sheet refuses, and a decision id shape check never
 * drifts from `decision.c`'s own.
 *
 * The "Frozen formats" section of `docs/plans/2026-09-03-review-surface.md`
 * gives ten refusal sentences verbatim. This parser produces all ten, plus
 * two more that section's own bullet list requires ("Refused above" for a
 * bound that section names but the ten-sentence block does not word) but
 * does not spell out:
 *
 *   - a line over `ATLAS_REVIEW_SHEET_MAX_LINE` bytes: the block gives no
 *     wording for this bound at all, so this file extends the pattern of the
 *     two bound refusals it does give ("review sheet: more than %u entries",
 *     "review sheet: larger than %u bytes") with a line number, since unlike
 *     those two this bound is a property of one line rather than of the
 *     whole sheet: `"review sheet line %zu: longer than %u bytes"`.
 *   - a repository field `atlas_db_check_repo_name` refuses: the block has
 *     no sentence for this either, and unlike every other field this one is
 *     validated by calling out to another module's grammar rather than by a
 *     rule stated here, so this file carries that function's own message
 *     forward behind a line number instead of inventing a second, competing
 *     description of the same rule: `"review sheet line %zu: %s"`. Every
 *     message `atlas_db_check_repo_name` produces is Atlas' own fixed text
 *     plus a checked integer, a byte offset, or `name[0]` -- which by that
 *     function's own guard is only ever `'-'` or `'.'` -- so no byte of an
 *     attacker-chosen name reaches this message.
 *
 * Both are disclosed here and in the T3 report rather than merged silently
 * into one of the ten, because a sentence this file invents and a sentence
 * the plan froze are not the same kind of claim.
 */
#include "atlas/review.h"

#include <string.h>

#include "atlas/buf.h"
#include "atlas/db.h"

/* --- the verdict vocabulary -------------------------------------------------
 *
 * Not used by the parser below -- these verdicts belong to the walker a later
 * task writes -- but produced here, alongside `atlas_review_intent_allowed`,
 * because both are pure functions of a closed vocabulary and this is the one
 * file that vocabulary's own JavaScript mirror is checked against.
 *
 * Every switch below has no `default:`. Adding a member to either enum must
 * fail this file's build, and every other place that has to decide about it,
 * rather than silently falling into a branch that happens to compile. */

const char *atlas_review_verdict_name(atlas_review_verdict v) {
    switch (v) {
    case ATLAS_REVIEW_UNKNOWN: return "UNKNOWN";
    case ATLAS_REVIEW_READY: return "READY";
    case ATLAS_REVIEW_APPLIED: return "APPLIED";
    case ATLAS_REVIEW_ABANDONED: return "ABANDONED";
    case ATLAS_REVIEW_MOVED: return "MOVED";
    case ATLAS_REVIEW_DISPOSED: return "DISPOSED";
    case ATLAS_REVIEW_MISSING: return "MISSING";
    case ATLAS_REVIEW_REFUSED: return "REFUSED";
    }
    return "UNKNOWN";
}

bool atlas_review_verdict_parse(const char *name, atlas_review_verdict *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    /* UNKNOWN deliberately does not parse: zero means "nobody produced a
     * verdict yet", and a parser that accepted the word would let a caller
     * store the absence of a verdict as though it were one. */
    if (strcmp(name, "READY") == 0) {
        *out = ATLAS_REVIEW_READY;
        return true;
    }
    if (strcmp(name, "APPLIED") == 0) {
        *out = ATLAS_REVIEW_APPLIED;
        return true;
    }
    if (strcmp(name, "ABANDONED") == 0) {
        *out = ATLAS_REVIEW_ABANDONED;
        return true;
    }
    if (strcmp(name, "MOVED") == 0) {
        *out = ATLAS_REVIEW_MOVED;
        return true;
    }
    if (strcmp(name, "DISPOSED") == 0) {
        *out = ATLAS_REVIEW_DISPOSED;
        return true;
    }
    if (strcmp(name, "MISSING") == 0) {
        *out = ATLAS_REVIEW_MISSING;
        return true;
    }
    if (strcmp(name, "REFUSED") == 0) {
        *out = ATLAS_REVIEW_REFUSED;
        return true;
    }
    return false;
}

bool atlas_review_intent_allowed(atlas_decision_intent i) {
    switch (i) {
    case ATLAS_DECISION_INTENT_APPROVE: return true;
    case ATLAS_DECISION_INTENT_REJECT: return true;
    case ATLAS_DECISION_INTENT_RESOLVE: return true;
    case ATLAS_DECISION_INTENT_SUPERSEDE: return false;
    case ATLAS_DECISION_INTENT_REVALIDATE: return false;
    }
    return false;
}

/* --- bounded parsing --------------------------------------------------------
 *
 * A sheet is untrusted input transcribed by hand from a browser tab
 * (architecture invariant 6/7 -- untrusted input, bounded parsers). Every
 * function below examines at most `ATLAS_REVIEW_SHEET_MAX_BYTES` bytes in
 * total and at most `ATLAS_REVIEW_SHEET_MAX_LINE` bytes of any one line
 * before refusing, matching `src/memory/trailer.c`'s own two-dimension
 * discipline for the same reason: a count and a raw size are different
 * questions and neither stands in for the other. */

#define REVIEW_ENTRY_FIELDS 5u

/* Splits `content[0..len)` on runs of one or more ASCII space/tab bytes,
 * returning the total field count found (which may exceed
 * REVIEW_ENTRY_FIELDS) and filling `tok`/`tok_len` with the first
 * REVIEW_ENTRY_FIELDS of them. A caller that finds the count is not exactly
 * REVIEW_ENTRY_FIELDS refuses before reading `tok` at all, so fields beyond
 * the fifth are never inspected -- only counted, which is what lets a line
 * carrying a sixth field (a would-be confirmation) be refused by its count
 * rather than by whatever the sixth field happens to contain. */
static size_t find_fields(const char *content, size_t len, const char *tok[REVIEW_ENTRY_FIELDS],
                          size_t tok_len[REVIEW_ENTRY_FIELDS]) {
    size_t count = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (content[i] == ' ' || content[i] == '\t')) {
            i++;
        }
        if (i >= len) {
            break;
        }
        size_t start = i;
        while (i < len && content[i] != ' ' && content[i] != '\t') {
            i++;
        }
        if (count < REVIEW_ENTRY_FIELDS) {
            tok[count] = content + start;
            tok_len[count] = i - start;
        }
        count++;
    }
    return count;
}

/* `r` followed by a decimal in 1..2147483647, no leading zero. Rejects "r0"
 * (the one digit is itself the leading zero), "r01" and a bare "1" all by
 * the same two checks, which is why the grammar names one sentence for all
 * three rather than three. */
static bool parse_revision(const char *tok, size_t tok_len, int64_t *out) {
    if (tok_len == 0 || tok[0] != 'r') {
        return false;
    }
    size_t n = tok_len - 1u;
    const char *d = tok + 1;
    if (n == 0 || n > 10u || d[0] == '0') {
        return false;
    }
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (int64_t)(c - '0');
    }
    if (v > 2147483647) {
        return false;
    }
    *out = v;
    return true;
}

/* Exactly ATLAS_DECISION_CONFIRM_HEX lowercase hex characters -- rejects a
 * 7-hex and a 9-hex prefix by length and an uppercase one by the character
 * class, both through this one check. */
static bool parse_prefix(const char *tok, size_t tok_len) {
    if (tok_len != ATLAS_DECISION_CONFIRM_HEX) {
        return false;
    }
    for (size_t i = 0; i < tok_len; i++) {
        unsigned char c = (unsigned char)tok[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* True when `(repo, decision)` already names an earlier entry in `built`,
 * with `*earlier_line` set to that entry's line. A sheet disposes of a
 * record once; a second occurrence is refused rather than merged or
 * overwritten, because two lines about one record could disagree about the
 * intent, the revision or the prefix and this file has no rule for which one
 * wins. */
static bool already_named(const atlas_review_sheet *built, const char *repo, const char *decision,
                          size_t *earlier_line) {
    for (size_t i = 0; i < built->count; i++) {
        if (strcmp(built->entries[i].repo, repo) == 0 &&
            strcmp(built->entries[i].decision, decision) == 0) {
            *earlier_line = built->entries[i].line;
            return true;
        }
    }
    return false;
}

/* Refuses an intent field that did not parse, or parsed to SUPERSEDE or
 * REVALIDATE. The offending field is truncated to ATLAS_REVIEW_INTENT_MSG_MAX
 * bytes and carried into the message *raw*, the way every other
 * `atlas_err_set` call in this tree carries a repository-adjacent value: an
 * error message holds raw text and is safe-encoded exactly once, at render,
 * by `atlas_render_error` (`src/cli/render_json.c`). This file's own byte
 * scan, in `atlas_review_sheet_parse` before any field is split out, has
 * already refused every byte in the line outside printable ASCII and tab, so
 * the only byte an encoder here could still touch is a literal '%' --
 * `atlas_safe()` escapes exactly that one byte to keep its own `%XX` scheme
 * reversible. Encoding it a second time here produced a doubled `%2525` for
 * that one byte once `atlas_render_error` encoded the already-encoded
 * message again: a caller-visible defect, not a defence, since nothing
 * between this point and the terminal or a JSON document skips
 * `atlas_render_error`'s single pass. Truncating to a fixed *byte* count
 * remains necessary on its own: an unbounded field would let a very long
 * hostile token make the message itself large. */
static atlas_status refuse_bad_intent(size_t line_no, const char *raw, size_t raw_len,
                                      atlas_err *err) {
    atlas_buf copy;
    atlas_buf_init(&copy);
    size_t take = raw_len < ATLAS_REVIEW_INTENT_MSG_MAX ? raw_len : ATLAS_REVIEW_INTENT_MSG_MAX;
    atlas_status st = atlas_buf_set(&copy, raw, take, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&copy);
        return st;
    }
    atlas_status rc = atlas_err_set(
        err, ATLAS_ERR_USAGE,
        "review sheet line %zu: \"%s\" is not an intent a sheet may carry (approve, reject or resolve)",
        line_no, atlas_buf_cstr(&copy));
    atlas_buf_free(&copy);
    return rc;
}

atlas_status atlas_review_sheet_parse(const char *bytes, size_t len, atlas_review_sheet *out,
                                      atlas_err *err) {
    if (out == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "atlas_review_sheet_parse: out is NULL");
    }
    /* Zeroed here and written again only on success below, so every refusal
     * path leaves `out` exactly like this -- an empty sheet, never a partial
     * one -- without needing to repeat the reset at each return statement. */
    memset(out, 0, sizeof *out);

    if (len > ATLAS_REVIEW_SHEET_MAX_BYTES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "review sheet: larger than %u bytes",
                             (unsigned)ATLAS_REVIEW_SHEET_MAX_BYTES);
    }
    if (bytes == NULL) {
        /* Enforced, not merely assumed of the caller: whatever `len` the
         * caller passed alongside a NULL `bytes`, a NULL pointer names no
         * bytes, so the loop below must never read past this one-byte
         * literal. */
        len = 0;
        bytes = "";
    }

    atlas_review_sheet built;
    memset(&built, 0, sizeof built);
    bool header_seen = false;
    size_t pos = 0;
    size_t line_no = 0;

    while (pos < len) {
        line_no++;

        const char *nl = (const char *)memchr(bytes + pos, '\n', len - pos);
        size_t raw_len = (nl != NULL) ? (size_t)(nl - (bytes + pos)) : (len - pos);
        size_t next_pos = pos + raw_len + ((nl != NULL) ? 1u : 0u);

        /* A trailing '\r' is legal only immediately before this line's own
         * '\n' -- i.e. only when this line actually has one -- and is
         * stripped from the content a caller sees. Anywhere else a '\r' is
         * just another disallowed byte, caught by the scan below. */
        size_t content_len = raw_len;
        bool trimmed_cr = false;
        if (content_len > 0 && nl != NULL && bytes[pos + content_len - 1u] == '\r') {
            content_len--;
            trimmed_cr = true;
        }

        if (content_len > ATLAS_REVIEW_SHEET_MAX_LINE) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "review sheet line %zu: longer than %u bytes",
                                 line_no, (unsigned)ATLAS_REVIEW_SHEET_MAX_LINE);
        }

        for (size_t i = 0; i < raw_len; i++) {
            if (trimmed_cr && i + 1u == raw_len) {
                continue; /* the '\r' this line's own '\n' immediately follows */
            }
            unsigned char c = (unsigned char)bytes[pos + i];
            bool ok = (c >= 0x20 && c <= 0x7E) || c == '\t';
            if (!ok) {
                return atlas_err_set(
                    err, ATLAS_ERR_USAGE,
                    "review sheet line %zu: a byte outside printable ASCII; a sheet carries "
                    "identifiers, never prose",
                    line_no);
            }
        }

        const char *content = bytes + pos;

        bool blank = true;
        for (size_t i = 0; i < content_len; i++) {
            if (content[i] != ' ' && content[i] != '\t') {
                blank = false;
                break;
            }
        }
        if (blank) {
            pos = next_pos;
            continue;
        }
        if (content[0] == '#') {
            pos = next_pos;
            continue;
        }

        if (!header_seen) {
            size_t hlen = sizeof(ATLAS_REVIEW_SHEET_HEADER) - 1u;
            if (content_len != hlen || memcmp(content, ATLAS_REVIEW_SHEET_HEADER, hlen) != 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "this is not an Atlas review sheet: the first line must be %s",
                                     ATLAS_REVIEW_SHEET_HEADER);
            }
            header_seen = true;
            pos = next_pos;
            continue;
        }

        /* An entry line. */
        const char *tok[REVIEW_ENTRY_FIELDS];
        size_t tok_len[REVIEW_ENTRY_FIELDS];
        size_t nfields = find_fields(content, content_len, tok, tok_len);
        if (nfields != REVIEW_ENTRY_FIELDS) {
            return atlas_err_set(
                err, ATLAS_ERR_USAGE,
                "review sheet line %zu: expected 5 fields (intent repository decision rN prefix), "
                "found %zu",
                line_no, nfields);
        }

        /* Reused, one field at a time, purely to NUL-terminate a token before
         * handing it to a C-string API; sized for the longest possible field
         * (the whole line), so no field ever overflows it. */
        char scratch[ATLAS_REVIEW_SHEET_MAX_LINE + 1u];

        memcpy(scratch, tok[0], tok_len[0]);
        scratch[tok_len[0]] = '\0';
        atlas_decision_intent intent = ATLAS_DECISION_INTENT_APPROVE;
        bool intent_ok = atlas_decision_intent_parse(scratch, &intent) &&
                         atlas_review_intent_allowed(intent);
        if (!intent_ok) {
            return refuse_bad_intent(line_no, tok[0], tok_len[0], err);
        }

        memcpy(scratch, tok[1], tok_len[1]);
        scratch[tok_len[1]] = '\0';
        atlas_err repo_err;
        atlas_err_init(&repo_err);
        if (atlas_db_check_repo_name(scratch, &repo_err) != ATLAS_OK) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "review sheet line %zu: %s", line_no,
                                 atlas_err_msg(&repo_err));
        }
        /* atlas_db_check_repo_name validates strlen(scratch), not tok_len[1].
         * The two agree only because the byte-legality scan above refuses
         * every byte below 0x20 -- 0x00 included -- so `scratch` can hold no
         * embedded NUL for strlen to stop short at; nothing here should
         * assume that agreement rather than use the length the check itself
         * relied on. Copying `repo_len + 1` (not tok_len[1] + 1, and not
         * `sizeof repo_buf` at the point this is stored into the entry below)
         * is what keeps a future relaxation of that scan (to admit UTF-8,
         * say) from turning this into a copy past the validated prefix. */
        size_t repo_len = strlen(scratch);
        char repo_buf[ATLAS_NAME_MAX + 1u];
        memcpy(repo_buf, scratch, repo_len + 1u);

        memcpy(scratch, tok[2], tok_len[2]);
        scratch[tok_len[2]] = '\0';
        if (!atlas_decision_uid_is_valid(scratch)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "review sheet line %zu: the decision id is malformed",
                                 line_no);
        }
        /* Same reasoning as repo_len above: atlas_decision_uid_is_valid also
         * validates strlen(scratch), not tok_len[2]. Here the check happens
         * to additionally force strlen(scratch) == ATLAS_DECISION_UID_MAX - 1
         * exactly (it walks hex digits until the NUL and demands precisely
         * ATLAS_DECISION_UID_HEX of them), so decision_buf can never actually
         * end up with an unfilled tail -- but this is written the same way as
         * repo's copy for the one reason that matters for both: the checked
         * length is used because it is what was checked, not because it is
         * assumed to equal tok_len[2]. */
        size_t decision_len = strlen(scratch);
        char decision_buf[ATLAS_DECISION_UID_MAX];
        memcpy(decision_buf, scratch, decision_len + 1u);

        int64_t revision_no = 0;
        if (!parse_revision(tok[3], tok_len[3], &revision_no)) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "review sheet line %zu: the revision must be r followed by a "
                                 "positive number",
                                 line_no);
        }

        if (!parse_prefix(tok[4], tok_len[4])) {
            return atlas_err_set(
                err, ATLAS_ERR_USAGE,
                "review sheet line %zu: the confirmation prefix must be exactly %u lowercase hex "
                "characters",
                line_no, (unsigned)ATLAS_DECISION_CONFIRM_HEX);
        }

        if (built.count >= ATLAS_REVIEW_SHEET_MAX_ENTRIES) {
            return atlas_err_set(err, ATLAS_ERR_USAGE, "review sheet: more than %u entries; split it",
                                 (unsigned)ATLAS_REVIEW_SHEET_MAX_ENTRIES);
        }

        size_t earlier_line = 0;
        if (already_named(&built, repo_buf, decision_buf, &earlier_line)) {
            return atlas_err_set(
                err, ATLAS_ERR_USAGE,
                "review sheet line %zu: %s was already named on line %zu; a sheet disposes of a "
                "record once",
                line_no, decision_buf, earlier_line);
        }

        atlas_review_entry *e = &built.entries[built.count];
        e->line = line_no;
        e->intent = intent;
        /* `repo_len + 1` / `decision_len + 1`, not `sizeof repo_buf` /
         * `sizeof decision_buf`: `built` (and so `*e`) was zeroed wholesale at
         * this function's start, and a full-buffer copy here would overwrite
         * that zeroed tail with repo_buf/decision_buf's own uninitialised
         * stack bytes past their NUL -- undoing the zeroing for exactly the
         * bytes a caller has no reason to read but every reason to expect are
         * zero, not indeterminate. Copying only the checked length leaves the
         * tail exactly as the initial memset left it. */
        memcpy(e->repo, repo_buf, repo_len + 1u);
        memcpy(e->decision, decision_buf, decision_len + 1u);
        e->revision_no = revision_no;
        memcpy(e->prefix, tok[4], tok_len[4]);
        e->prefix[tok_len[4]] = '\0';
        built.count++;

        pos = next_pos;
    }

    if (!header_seen) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "this is not an Atlas review sheet: the first line must be %s",
                             ATLAS_REVIEW_SHEET_HEADER);
    }

    *out = built;
    return ATLAS_OK;
}

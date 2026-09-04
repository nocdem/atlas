/* Atlas - A15 T3: the review sheet grammar.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A review sheet is a plain-ASCII list an operator copies out of a Mission
 * Control browser session and hands to a local command
 * (`atlas review apply`, a later task). It stores no authority: its fifth
 * field is the public prefix the operator will type, and it has no field
 * the walker ever reads in place of typing that prefix on `/dev/tty`, per
 * entry, exactly as `atlas decision approve` already requires. That is why
 * a line carrying a sixth field is refused rather than read as an early
 * confirmation -- the sheet's own mirror of the rule that no MCP tool
 * schema in `src/mcp/mcp_tools.c` declares a `"confirmation":` property
 * (`tests/test_decision_mcp.c`).
 *
 * This header and `src/core/review.c` are the grammar and the vocabulary
 * only: bytes in, a parsed sheet or a refusal out. No I/O, no database, no
 * git and no process -- see the "Frozen formats" section of
 * `docs/plans/2026-09-03-review-surface.md` for the grammar itself, the
 * example sheet and the refusal sentences the parser produces, most of them
 * verbatim (`src/core/review.c`'s own header names the two it does not).
 */
#ifndef ATLAS_REVIEW_H
#define ATLAS_REVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atlas/decision.h"
#include "atlas/error.h"
#include "atlas/limits.h"

/* The sheet's own header line. A sheet is refused, not silently upgraded,
 * when the first non-blank, non-comment line does not match this exactly:
 * there is one grammar version and this file carries no fallback for
 * another one. */
#define ATLAS_REVIEW_SHEET_HEADER "atlas-review-sheet/1"

/* One entry's disposition, once `atlas review apply` (a later task) has
 * walked it against the live record. UNKNOWN is never emitted and never
 * parses -- `src/memory/source.c`'s vocabulary discipline, applied here so a
 * later task's mirror of this enum in JavaScript has one place in C to stay
 * honest against. */
typedef enum atlas_review_verdict {
    ATLAS_REVIEW_UNKNOWN = 0,
    ATLAS_REVIEW_READY,     /* --check only: the pre-check passed, nothing was minted */
    ATLAS_REVIEW_APPLIED,   /* the capability was spent; the ledger has the event */
    ATLAS_REVIEW_ABANDONED, /* the operator typed something other than the prefix */
    ATLAS_REVIEW_MOVED,     /* newest revision or hash prefix differs from the sheet */
    ATLAS_REVIEW_DISPOSED,  /* the record's status is no longer the one the intent needs */
    ATLAS_REVIEW_MISSING,   /* no such repository, or no such decision in it */
    ATLAS_REVIEW_REFUSED    /* any other refusal from the confirm; its message is carried */
} atlas_review_verdict;

const char *atlas_review_verdict_name(atlas_review_verdict v);
/* Refuses "UNKNOWN": see the enum's own comment. */
bool atlas_review_verdict_parse(const char *name, atlas_review_verdict *out);

/* True for APPROVE, REJECT and RESOLVE; false for SUPERSEDE and REVALIDATE.
 * The one predicate: the parser below asks it of every entry's intent field,
 * and a later task's page mirrors it in JavaScript rather than restating the
 * list a second time. SUPERSEDE needs a second document named alongside it
 * and REVALIDATE needs one exact repository state named alongside it --
 * both need more than a five-field line can carry honestly, so they are not
 * sheet intents, not merely intents this sheet happens not to use yet. */
bool atlas_review_intent_allowed(atlas_decision_intent i);

/* One parsed entry line. `line` is this entry's 1-based physical line number
 * in the sheet -- counting every line, including a blank one, a comment and
 * the header -- carried so every later verdict can still say which line of
 * the operator's own file it disposes of. */
typedef struct atlas_review_entry {
    size_t line;
    atlas_decision_intent intent;
    char repo[ATLAS_NAME_MAX + 1u];
    char decision[ATLAS_DECISION_UID_MAX];
    int64_t revision_no;
    char prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
} atlas_review_entry;

typedef struct atlas_review_sheet {
    atlas_review_entry entries[ATLAS_REVIEW_SHEET_MAX_ENTRIES];
    size_t count;
} atlas_review_sheet;

/* Parses `len` bytes as a review sheet. Refuses the whole sheet on the first
 * defect it finds, with one of the grammar's refusal sentences; never
 * repairs, never truncates, never skips a line. `out` is zeroed up front and
 * is written again, wholesale, only on success -- so a caller that checked
 * the returned status incorrectly is holding an empty sheet on every refusal
 * path, never a partial one. */
atlas_status atlas_review_sheet_parse(const char *bytes, size_t len, atlas_review_sheet *out,
                                      atlas_err *err);

#endif /* ATLAS_REVIEW_H */

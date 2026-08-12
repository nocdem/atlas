/* Atlas - the decision vocabularies, the canonical content hash and validation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The parts of A4 that are about *meaning* rather than about storage or
 * transport: what the four lifecycle states are, which transitions between them
 * exist, who is allowed to have caused one, and what exactly gets hashed when
 * an approval binds to content.
 *
 * Every vocabulary here is closed and parsed by exact match with no default.
 * That is the same rule `atlas_provenance_parse` and
 * `atlas_code_resolution_parse` follow, and for the same reason: defaulting an
 * unrecognised state to a known one is how a garbled value becomes an approval.
 */
#include "atlas/decision.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"

/* --- lifecycle ------------------------------------------------------------ */

const char *atlas_decision_state_name(atlas_decision_state s) {
    switch (s) {
    case ATLAS_DECISION_PROPOSED: return "PROPOSED";
    case ATLAS_DECISION_APPROVED: return "APPROVED";
    case ATLAS_DECISION_REJECTED: return "REJECTED";
    case ATLAS_DECISION_SUPERSEDED: return "SUPERSEDED";
    case ATLAS_DECISION_RESOLVED: return "RESOLVED";
    }
    return "PROPOSED";
}

bool atlas_decision_state_parse(const char *name, atlas_decision_state *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        atlas_decision_state value;
    } TABLE[] = {
        {"PROPOSED", ATLAS_DECISION_PROPOSED},
        {"APPROVED", ATLAS_DECISION_APPROVED},
        {"REJECTED", ATLAS_DECISION_REJECTED},
        {"SUPERSEDED", ATLAS_DECISION_SUPERSEDED},
        {"RESOLVED", ATLAS_DECISION_RESOLVED},
    };
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].value;
            return true;
        }
    }
    return false;
}

/* --- knowledge kinds (A9.1) ------------------------------------------------
 *
 * One row per kind, carrying the name, one fixed sentence of meaning, and the
 * single lifecycle consequence a kind has. Everything else about a kind is
 * reporting, which is the point: the classification tells a reader how to treat
 * a record and tells Atlas almost nothing.
 *
 * `resolvable` is the whole of the kind's authority over the lifecycle. It is
 * true exactly where an approved record makes a demand that can be discharged,
 * and false everywhere else because "this invariant has been resolved" is not a
 * sentence with a meaning. */
static const struct kind_row {
    atlas_decision_kind value;
    const char *name;
    bool resolvable;
    const char *description;
} KINDS[] = {
    {ATLAS_DECISION_KIND_DECISION, "DECISION", false,
     "a choice between alternatives that establishes project direction or architecture"},
    {ATLAS_DECISION_KIND_POLICY, "POLICY", false,
     "a rule governing development, release, operation or process"},
    {ATLAS_DECISION_KIND_INVARIANT, "INVARIANT", false,
     "a technical property implementations must preserve"},
    {ATLAS_DECISION_KIND_OPERATIONAL_FACT, "OPERATIONAL_FACT", false,
     "a mutable, environment-specific fact about what is currently deployed or relevant; it "
     "carries no architectural permanence and is replaced by superseding it"},
    {ATLAS_DECISION_KIND_ACCEPTED_RISK, "ACCEPTED_RISK", true,
     "a risk that has been explicitly accepted; a proposed one is a risk somebody recorded and "
     "nobody accepted, and acceptance is the ordinary approval"},
    {ATLAS_DECISION_KIND_OBLIGATION, "OBLIGATION", true,
     "required future work: a remediation, a blocker or a release gate"},
    {ATLAS_DECISION_KIND_PARKED, "PARKED", false,
     "work or architecture intentionally deferred and not currently active; parked is not "
     "rejected"},
    {ATLAS_DECISION_KIND_REJECTED_ALTERNATIVE, "REJECTED_ALTERNATIVE", false,
     "an approach considered or built and deliberately rejected, recorded with why so it is not "
     "retried without new evidence"},
};

/* The array bound callers use and the table are the same length, checked here
 * rather than trusted: a kind added to the enum and to KINDS[] but not to the
 * bound would silently write past every `by_kind[]` array in Atlas. */
_Static_assert(sizeof(KINDS) / sizeof(KINDS[0]) == ATLAS_DECISION_KIND_MAX,
               "ATLAS_DECISION_KIND_MAX must equal the number of rows in KINDS[]");

static const struct kind_row *kind_row_of(atlas_decision_kind k) {
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (KINDS[i].value == k) {
            return &KINDS[i];
        }
    }
    return NULL;
}

const char *atlas_decision_kind_name(atlas_decision_kind k) {
    const struct kind_row *row = kind_row_of(k);
    /* A member with no row falls back to the default rather than to a
     * placeholder, because DECISION is what an unset kind means everywhere else
     * in Atlas. `tests/test_decision_kind.c` asserts every member has a row, so
     * this path is unreachable rather than merely unlikely. */
    return row != NULL ? row->name : "DECISION";
}

bool atlas_decision_kind_parse(const char *name, atlas_decision_kind *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (strcmp(name, KINDS[i].name) == 0) {
            *out = KINDS[i].value;
            return true;
        }
    }
    return false;
}

const char *atlas_decision_kind_description(atlas_decision_kind k) {
    const struct kind_row *row = kind_row_of(k);
    return row != NULL ? row->description : KINDS[0].description;
}

size_t atlas_decision_kind_count(void) {
    return sizeof(KINDS) / sizeof(KINDS[0]);
}

atlas_decision_kind atlas_decision_kind_at(size_t index) {
    if (index >= sizeof(KINDS) / sizeof(KINDS[0])) {
        return ATLAS_DECISION_KIND_DECISION;
    }
    return KINDS[index].value;
}

const char *atlas_decision_kind_list(void) {
    return "DECISION, POLICY, INVARIANT, OPERATIONAL_FACT, ACCEPTED_RISK, OBLIGATION, PARKED or "
           "REJECTED_ALTERNATIVE";
}

bool atlas_decision_kind_resolvable(atlas_decision_kind k) {
    const struct kind_row *row = kind_row_of(k);
    return row != NULL && row->resolvable;
}

/* The transition table. This function is the only authority on it.
 *
 * `lifecycle.c` asks it before every write and the tests assert against it, so
 * a test cannot pass by agreeing with a second copy of the rules — which is
 * what would happen if the table were an `if` chain in the writer and a list in
 * a test. Enumerating the refusals rather than the permissions would be shorter
 * and is not done on purpose: the safe default for an unlisted pair is "no".
 *
 * A9.1 gave it the document's kind. The kind widens the table in exactly one
 * place and narrows it nowhere: every kind is proposable, approvable,
 * rejectable and supersedable, and only a kind whose approved form makes a
 * demand may also be resolved. Uniformity in the rest is deliberate — a reader
 * should not have to consult a matrix to find out whether a record can be
 * refused. */
bool atlas_decision_transition_allowed(atlas_decision_kind kind, atlas_decision_state from,
                                       atlas_decision_state to) {
    switch (from) {
    case ATLAS_DECISION_PROPOSED:
        /* A proposal may be accepted or refused. It may not jump straight to
         * SUPERSEDED: superseding something that was never effective would
         * record that policy changed when it never existed. Nor to RESOLVED:
         * discharging an obligation nobody accepted would make recording a
         * demand and satisfying it one step, and the acceptance is the part an
         * operator has to have seen. */
        return to == ATLAS_DECISION_APPROVED || to == ATLAS_DECISION_REJECTED;
    case ATLAS_DECISION_APPROVED:
        /* The ways out of effective. Replacement by a later approval, always.
         * There is deliberately no APPROVED -> REJECTED: retracting a decision
         * is proposing and approving its replacement, which leaves a record of
         * what replaced it instead of a hole where policy used to be.
         *
         * And, for the kinds that make a demand, closure: the demand was met,
         * nothing replaced the record, and it stops being effective. */
        if (to == ATLAS_DECISION_SUPERSEDED) {
            return true;
        }
        return to == ATLAS_DECISION_RESOLVED && atlas_decision_kind_resolvable(kind);
    case ATLAS_DECISION_REJECTED:
    case ATLAS_DECISION_SUPERSEDED:
    case ATLAS_DECISION_RESOLVED:
        /* Terminal. In particular REJECTED -> APPROVED is refused: "we said no
         * and then it quietly became policy" is the failure the ledger exists
         * to make impossible, and revisiting a rejected idea means proposing a
         * new revision of it. RESOLVED is terminal for the same reason and with
         * the same remedy: reopening a discharged obligation is a new revision,
         * approved through the channel, not a state that quietly comes back. */
        return false;
    }
    return false;
}

/* --- actors --------------------------------------------------------------- */

const char *atlas_decision_actor_name(atlas_decision_actor a) {
    switch (a) {
    case ATLAS_DECISION_ACTOR_MODEL_PROPOSAL: return "MODEL_PROPOSAL";
    case ATLAS_DECISION_ACTOR_MODEL_INFERENCE: return "MODEL_INFERENCE";
    case ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED: return "LOCAL_OPERATOR_CONFIRMED";
    case ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC: return "ATLAS_AUTOMATIC";
    }
    return "MODEL_PROPOSAL";
}

bool atlas_decision_actor_parse(const char *name, atlas_decision_actor *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        atlas_decision_actor value;
    } TABLE[] = {
        {"MODEL_PROPOSAL", ATLAS_DECISION_ACTOR_MODEL_PROPOSAL},
        {"MODEL_INFERENCE", ATLAS_DECISION_ACTOR_MODEL_INFERENCE},
        {"LOCAL_OPERATOR_CONFIRMED", ATLAS_DECISION_ACTOR_LOCAL_OPERATOR_CONFIRMED},
        {"ATLAS_AUTOMATIC", ATLAS_DECISION_ACTOR_ATLAS_AUTOMATIC},
    };
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].value;
            return true;
        }
    }
    return false;
}

/* The restriction, as a function, mirroring `atlas_provenance_writable_in_a2`
 * and `atlas_code_resolution_writable_in_a3`.
 *
 * An adapter is anything that reaches Atlas over the socket without going
 * through the operator channel: the MCP server, a hook, a generic MCP client, a
 * `decision propose` that did not consume a challenge. None of them may claim
 * an operator confirmed anything, and none of them may claim Atlas did
 * something automatically either — ATLAS_AUTOMATIC exists for the supersession
 * that an approval implies, and letting a caller assert it would let a caller
 * fabricate that implication. */
bool atlas_decision_actor_writable_by_adapter(atlas_decision_actor a) {
    return a == ATLAS_DECISION_ACTOR_MODEL_PROPOSAL || a == ATLAS_DECISION_ACTOR_MODEL_INFERENCE;
}

/* --- scope ---------------------------------------------------------------- */

const char *atlas_decision_scope_name(atlas_decision_scope s) {
    switch (s) {
    case ATLAS_DECISION_SCOPE_UNKNOWN: return "UNKNOWN";
    case ATLAS_DECISION_SCOPE_REPOSITORY: return "REPOSITORY";
    case ATLAS_DECISION_SCOPE_SUBSYSTEM: return "SUBSYSTEM";
    case ATLAS_DECISION_SCOPE_PATHS: return "PATHS";
    }
    return "UNKNOWN";
}

bool atlas_decision_scope_parse(const char *name, atlas_decision_scope *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        atlas_decision_scope value;
    } TABLE[] = {
        {"UNKNOWN", ATLAS_DECISION_SCOPE_UNKNOWN},
        {"REPOSITORY", ATLAS_DECISION_SCOPE_REPOSITORY},
        {"SUBSYSTEM", ATLAS_DECISION_SCOPE_SUBSYSTEM},
        {"PATHS", ATLAS_DECISION_SCOPE_PATHS},
    };
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].value;
            return true;
        }
    }
    return false;
}

/* --- link kinds ----------------------------------------------------------- */

const char *atlas_decision_link_kind_name(atlas_decision_link_kind k) {
    switch (k) {
    case ATLAS_DECISION_LINK_PATH: return "path";
    case ATLAS_DECISION_LINK_COMMIT: return "commit";
    case ATLAS_DECISION_LINK_CHANGE_SET: return "change_set";
    case ATLAS_DECISION_LINK_SYMBOL: return "symbol";
    case ATLAS_DECISION_LINK_SUPERSEDES: return "supersedes";
    case ATLAS_DECISION_LINK_REPLACED_BY: return "replaced_by";
    case ATLAS_DECISION_LINK_RELATES_TO: return "relates_to";
    }
    return "path";
}

bool atlas_decision_link_kind_parse(const char *name, atlas_decision_link_kind *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        atlas_decision_link_kind value;
    } TABLE[] = {
        {"path", ATLAS_DECISION_LINK_PATH},
        {"commit", ATLAS_DECISION_LINK_COMMIT},
        {"change_set", ATLAS_DECISION_LINK_CHANGE_SET},
        {"symbol", ATLAS_DECISION_LINK_SYMBOL},
        {"supersedes", ATLAS_DECISION_LINK_SUPERSEDES},
        {"replaced_by", ATLAS_DECISION_LINK_REPLACED_BY},
        {"relates_to", ATLAS_DECISION_LINK_RELATES_TO},
    };
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(name, TABLE[i].name) == 0) {
            *out = TABLE[i].value;
            return true;
        }
    }
    return false;
}

const char *atlas_decision_link_currency_name(atlas_decision_link_currency c) {
    switch (c) {
    case ATLAS_DECISION_LINK_UNKNOWN: return "UNKNOWN";
    case ATLAS_DECISION_LINK_CURRENT: return "CURRENT";
    case ATLAS_DECISION_LINK_CHANGED: return "CHANGED";
    case ATLAS_DECISION_LINK_MISSING: return "MISSING";
    case ATLAS_DECISION_LINK_AMBIGUOUS: return "AMBIGUOUS";
    }
    return "UNKNOWN";
}

const char *atlas_decision_intent_name(atlas_decision_intent i) {
    switch (i) {
    case ATLAS_DECISION_INTENT_APPROVE: return "approve";
    case ATLAS_DECISION_INTENT_REJECT: return "reject";
    case ATLAS_DECISION_INTENT_SUPERSEDE: return "supersede";
    case ATLAS_DECISION_INTENT_REVALIDATE: return "revalidate";
    case ATLAS_DECISION_INTENT_RESOLVE: return "resolve";
    }
    return "approve";
}

bool atlas_decision_intent_parse(const char *name, atlas_decision_intent *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "approve") == 0) {
        *out = ATLAS_DECISION_INTENT_APPROVE;
        return true;
    }
    if (strcmp(name, "reject") == 0) {
        *out = ATLAS_DECISION_INTENT_REJECT;
        return true;
    }
    if (strcmp(name, "supersede") == 0) {
        *out = ATLAS_DECISION_INTENT_SUPERSEDE;
        return true;
    }
    if (strcmp(name, "revalidate") == 0) {
        *out = ATLAS_DECISION_INTENT_REVALIDATE;
        return true;
    }
    if (strcmp(name, "resolve") == 0) {
        *out = ATLAS_DECISION_INTENT_RESOLVE;
        return true;
    }
    return false;
}

/* --- structures ----------------------------------------------------------- */

void atlas_decision_link_init(atlas_decision_link *l, atlas_decision_link_kind kind) {
    memset(l, 0, sizeof(*l));
    l->kind = kind;
    atlas_buf_init(&l->path_raw);
    atlas_buf_init(&l->path_text);
    atlas_buf_init(&l->commit_oid);
    atlas_buf_init(&l->target_uid);
    atlas_buf_init(&l->symbol_name);
    atlas_buf_init(&l->symbol_name_text);
    atlas_buf_init(&l->symbol_kind);
    atlas_buf_init(&l->basis_commit);
    atlas_buf_init(&l->file_content_hash);
    atlas_buf_init(&l->analyzer_name);
    l->currency = ATLAS_DECISION_LINK_UNKNOWN;
}

void atlas_decision_link_free(atlas_decision_link *l) {
    if (l == NULL) {
        return;
    }
    atlas_buf_free(&l->path_raw);
    atlas_buf_free(&l->path_text);
    atlas_buf_free(&l->commit_oid);
    atlas_buf_free(&l->target_uid);
    atlas_buf_free(&l->symbol_name);
    atlas_buf_free(&l->symbol_name_text);
    atlas_buf_free(&l->symbol_kind);
    atlas_buf_free(&l->basis_commit);
    atlas_buf_free(&l->file_content_hash);
    atlas_buf_free(&l->analyzer_name);
}

void atlas_decision_revision_init(atlas_decision_revision *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->title);
    atlas_buf_init(&r->context_text);
    atlas_buf_init(&r->decision_text);
    atlas_buf_init(&r->rationale_text);
    atlas_buf_init(&r->consequences_text);
    for (size_t i = 0; i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        atlas_buf_init(&r->alternatives[i]);
    }
    atlas_buf_init(&r->unbound_reason);
    atlas_buf_init(&r->basis_head);
    atlas_buf_init(&r->basis_repo_identity);
    r->scope = ATLAS_DECISION_SCOPE_UNKNOWN;
    r->proposed_by = ATLAS_DECISION_ACTOR_MODEL_PROPOSAL;
    r->state = ATLAS_DECISION_PROPOSED;
}

void atlas_decision_revision_free(atlas_decision_revision *r) {
    if (r == NULL) {
        return;
    }
    atlas_buf_free(&r->title);
    atlas_buf_free(&r->context_text);
    atlas_buf_free(&r->decision_text);
    atlas_buf_free(&r->rationale_text);
    atlas_buf_free(&r->consequences_text);
    for (size_t i = 0; i < ATLAS_DECISION_MAX_ALTERNATIVES; i++) {
        atlas_buf_free(&r->alternatives[i]);
    }
    for (size_t i = 0; i < r->link_count && i < ATLAS_DECISION_MAX_LINKS; i++) {
        atlas_decision_link_free(&r->links[i]);
    }
    r->link_count = 0;
    atlas_buf_free(&r->unbound_reason);
    atlas_buf_free(&r->basis_head);
    atlas_buf_free(&r->basis_repo_identity);
}

atlas_status atlas_decision_revision_add_alternative(atlas_decision_revision *r, const char *text,
                                                     size_t len, atlas_err *err) {
    if (r->alternative_count >= ATLAS_DECISION_MAX_ALTERNATIVES) {
        /* Refused, not dropped. A decision that silently records three of five
         * alternatives claims the other two were never considered. */
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision revision may list at most %d alternatives",
                             ATLAS_DECISION_MAX_ALTERNATIVES);
    }
    atlas_status st = atlas_decision_check_text("alternative", text, len,
                                                ATLAS_DECISION_ALTERNATIVE_MAX, true, err);
    if (st != ATLAS_OK) {
        return st;
    }
    st = atlas_buf_set(&r->alternatives[r->alternative_count], text, len, err);
    if (st == ATLAS_OK) {
        r->alternative_count++;
    }
    return st;
}

atlas_status atlas_decision_revision_add_link(atlas_decision_revision *r, atlas_decision_link *link,
                                              atlas_err *err) {
    if (r->link_count >= ATLAS_DECISION_MAX_LINKS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision revision may carry at most %d links",
                             ATLAS_DECISION_MAX_LINKS);
    }
    /* Move, not copy: the buffers change owner and the caller's struct is left
     * inert so that its `_free` is still correct and frees nothing twice. This
     * is the `atlas_buf_detach` rule applied to a whole struct. */
    r->links[r->link_count] = *link;
    memset(link, 0, sizeof(*link));
    atlas_decision_link_init(link, r->links[r->link_count].kind);
    r->link_count++;
    return ATLAS_OK;
}

/* The inverse of `add_link`, and the whole of what "removing a link" means
 * inside Atlas: a revision is immutable, so nothing is ever deleted — a caller
 * loads the current revision, drops one link from the working copy, and writes
 * the result as a *new* revision. The revision that carried the link keeps it
 * verbatim, for ever, which is why removal costs no history.
 *
 * Reports whether it found anything, so a caller can tell "withdrawn" from
 * "there was nothing to withdraw" and answer the two differently. */
bool atlas_decision_revision_remove_link(atlas_decision_revision *r, atlas_decision_link_kind kind,
                                         const char *target_uid) {
    if (r == NULL || target_uid == NULL) {
        return false;
    }
    for (size_t i = 0; i < r->link_count; i++) {
        atlas_decision_link *l = &r->links[i];
        if (l->kind != kind || strcmp(atlas_buf_cstr(&l->target_uid), target_uid) != 0) {
            continue;
        }
        atlas_decision_link_free(l);
        /* Shift the tail down. The order of a revision's links is part of what
         * its content hash covers for the kinds that keep an order, so closing
         * the gap rather than leaving a hole is not tidiness. */
        for (size_t k = i + 1; k < r->link_count; k++) {
            r->links[k - 1] = r->links[k];
        }
        r->link_count--;
        memset(&r->links[r->link_count], 0, sizeof(r->links[r->link_count]));
        atlas_decision_link_init(&r->links[r->link_count], ATLAS_DECISION_LINK_PATH);
        return true;
    }
    return false;
}

/* --- validation ----------------------------------------------------------- */

/* Strict UTF-8, decoded rather than sniffed.
 *
 * Overlong forms, surrogates and out-of-range code points are all rejected,
 * because each of them is a second spelling of a byte sequence and a document
 * whose bytes have two spellings has two content hashes. A canonical digest
 * over non-canonical input is not canonical. */
static bool utf8_is_strict(const unsigned char *s, size_t len, size_t *bad_at) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        size_t need;
        uint32_t cp;
        if (c < 0x80u) {
            i++;
            continue;
        }
        if ((c & 0xE0u) == 0xC0u) {
            need = 1;
            cp = (uint32_t)(c & 0x1Fu);
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 2;
            cp = (uint32_t)(c & 0x0Fu);
        } else if ((c & 0xF8u) == 0xF0u) {
            need = 3;
            cp = (uint32_t)(c & 0x07u);
        } else {
            *bad_at = i;
            return false;
        }
        if (i + need >= len) {
            /* The sequence announces more continuation bytes than remain. */
            *bad_at = i;
            return false;
        }
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = s[i + k];
            if ((cc & 0xC0u) != 0x80u) {
                *bad_at = i + k;
                return false;
            }
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        /* Overlong, surrogate and out-of-range. */
        if ((need == 1 && cp < 0x80u) || (need == 2 && cp < 0x800u) ||
            (need == 3 && cp < 0x10000u) || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
            *bad_at = i;
            return false;
        }
        i += need + 1u;
    }
    return true;
}

atlas_status atlas_decision_check_text(const char *field, const char *text, size_t len,
                                       size_t max_len, bool allow_newlines, atlas_err *err) {
    if (text == NULL) {
        return ATLAS_OK; /* absent is not invalid; required-ness is checked separately */
    }
    if (len > max_len) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "decision %s is %zu bytes, which is over the %zu byte limit", field,
                             len, max_len);
    }
    const unsigned char *u = (const unsigned char *)text;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = u[i];
        if (c == 0u) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "decision %s contains a NUL byte at offset %zu", field, i);
        }
        if (c == '\n' || c == '\t') {
            if (!allow_newlines) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "decision %s must be a single line, and contains a control "
                                     "character at offset %zu",
                                     field, i);
            }
            continue;
        }
        if (c < 0x20u || c == 0x7Fu) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "decision %s contains a control character at offset %zu", field,
                                 i);
        }
    }
    size_t bad = 0;
    if (!utf8_is_strict(u, len, &bad)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "decision %s is not valid UTF-8 at offset %zu", field, bad);
    }
    /* The multi-byte characters that are dangerous rather than merely unusual.
     *
     * These all survive the byte loop above — they are well-formed UTF-8 — and
     * every one of them changes what a reader sees without changing what is
     * stored. They are the same set `atlas_safetext` escapes, and they are
     * *refused* here rather than escaped for the reason the trust boundary
     * gives: a decision document is durable, canonical and read by a person
     * approving it, and validating beats escaping wherever a value can be
     * required to have a shape.
     *
     *   - C1 controls (U+0080..U+009F), as dangerous to a terminal as C0;
     *   - LINE SEPARATOR and PARAGRAPH SEPARATOR, which some renderers treat as
     *     newlines and others do not;
     *   - the bidi *overrides* and *isolates* (U+202A..U+202E, U+2066..U+2069),
     *     which are the Trojan Source set: they reorder displayed text without
     *     changing its bytes, so an approval prompt could show one decision
     *     while the record holds another. The ordinary directionality marks
     *     used in real right-to-left prose are not in this set and are
     *     accepted. */
    for (size_t i = 0; i < len;) {
        unsigned char c = u[i];
        if (c < 0x80u) {
            i++;
            continue;
        }
        size_t seq = (c & 0xE0u) == 0xC0u ? 2u : ((c & 0xF0u) == 0xE0u ? 3u : 4u);
        uint32_t cp = 0;
        if (seq == 2u) {
            cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(u[i + 1u] & 0x3Fu);
        } else if (seq == 3u) {
            cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(u[i + 1u] & 0x3Fu) << 6) |
                 (uint32_t)(u[i + 2u] & 0x3Fu);
        }
        bool dangerous = (cp >= 0x80u && cp <= 0x9Fu) || cp == 0x2028u || cp == 0x2029u ||
                   (cp >= 0x202Au && cp <= 0x202Eu) || (cp >= 0x2066u && cp <= 0x2069u);
        if (dangerous) {
            return atlas_err_set(err, ATLAS_ERR_USAGE,
                                 "decision %s contains U+%04X at offset %zu, which changes what a "
                                 "reader sees without changing what is stored",
                                 field, (unsigned)cp, i);
        }
        i += seq;
    }
    return ATLAS_OK;
}

atlas_status atlas_decision_revision_validate(const atlas_decision_revision *r, atlas_err *err) {
    if (r->title.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "a decision revision needs a title");
    }
    if (r->decision_text.len == 0) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "a decision revision needs a decision: what was decided");
    }
    /* A title is one line. It is shown in lists and in the approval prompt, and
     * a multi-line title in a confirmation display is the beginning of a forged
     * prompt rather than a formatting preference. */
    atlas_status st = atlas_decision_check_text("title", r->title.data, r->title.len,
                                                ATLAS_DECISION_TITLE_MAX, false, err);
    if (st == ATLAS_OK) {
        st = atlas_decision_check_text("context", r->context_text.data, r->context_text.len,
                                       ATLAS_DECISION_TEXT_MAX, true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_decision_check_text("decision", r->decision_text.data, r->decision_text.len,
                                       ATLAS_DECISION_TEXT_MAX, true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_decision_check_text("rationale", r->rationale_text.data, r->rationale_text.len,
                                       ATLAS_DECISION_TEXT_MAX, true, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_decision_check_text("consequences", r->consequences_text.data,
                                       r->consequences_text.len, ATLAS_DECISION_TEXT_MAX, true,
                                       err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < r->alternative_count; i++) {
        st = atlas_decision_check_text("alternative", r->alternatives[i].data,
                                       r->alternatives[i].len, ATLAS_DECISION_ALTERNATIVE_MAX, true,
                                       err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    if (r->alternative_count > ATLAS_DECISION_MAX_ALTERNATIVES) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "too many alternatives");
    }
    if (r->link_count > ATLAS_DECISION_MAX_LINKS) {
        return atlas_err_set(err, ATLAS_ERR_USAGE, "too many links");
    }
    for (size_t i = 0; i < r->link_count; i++) {
        const atlas_decision_link *l = &r->links[i];
        switch (l->kind) {
        case ATLAS_DECISION_LINK_PATH:
            if (l->path_raw.len == 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "a path link needs a path");
            }
            break;
        case ATLAS_DECISION_LINK_COMMIT:
            if (l->commit_oid.len == 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "a commit link needs an object id");
            }
            break;
        case ATLAS_DECISION_LINK_SYMBOL:
            if (l->symbol_name.len == 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "a symbol link needs a symbol name");
            }
            break;
        case ATLAS_DECISION_LINK_SUPERSEDES:
        case ATLAS_DECISION_LINK_REPLACED_BY:
        case ATLAS_DECISION_LINK_RELATES_TO:
            if (!atlas_decision_uid_is_valid(atlas_buf_cstr(&l->target_uid))) {
                return atlas_err_set(err, ATLAS_ERR_USAGE,
                                     "a document link needs a valid decision id");
            }
            break;
        case ATLAS_DECISION_LINK_CHANGE_SET:
            if (l->change_set_id <= 0) {
                return atlas_err_set(err, ATLAS_ERR_USAGE, "a change-set link needs an id");
            }
            break;
        }
    }
    return ATLAS_OK;
}

/* --- public identifiers ---------------------------------------------------- */

bool atlas_decision_uid_is_valid(const char *uid) {
    if (uid == NULL) {
        return false;
    }
    static const char prefix[] = ATLAS_DECISION_UID_PREFIX;
    size_t plen = sizeof(prefix) - 1u;
    if (strncmp(uid, prefix, plen) != 0) {
        return false;
    }
    const char *hex = uid + plen;
    size_t n = 0;
    for (const char *p = hex; *p != '\0'; p++, n++) {
        bool ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
        if (!ok) {
            return false;
        }
    }
    return n == ATLAS_DECISION_UID_HEX;
}

/* Reads `n` bytes from the kernel CSPRNG, or fails. Shared by the challenge
 * token and the document uid: both need unpredictability, and a second
 * hand-rolled reader would be a second place to get it subtly wrong. */
static atlas_status read_urandom(void *out, size_t n, atlas_err *err) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "cannot open /dev/urandom; Atlas will not substitute a predictable "
                             "value");
    }
    size_t got = 0;
    unsigned char *p = (unsigned char *)out;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            (void)close(fd);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "cannot read randomness");
        }
        got += (size_t)r;
    }
    (void)close(fd);
    return ATLAS_OK;
}

atlas_status atlas_decision_uid_derive(const char *identity_hash, int64_t document_id,
                                       const char *created_at, unsigned attempt, char *out,
                                       size_t out_size, atlas_err *err) {
    if (out_size < ATLAS_DECISION_UID_MAX) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "decision uid buffer is too small");
    }
    out[0] = '\0';
    unsigned char entropy[ATLAS_DECISION_UID_ENTROPY_BYTES];
    atlas_status st = read_urandom(entropy, sizeof(entropy), err);
    if (st != ATLAS_OK) {
        return st;
    }
    /* Domain-separated like the content hash, and for the same reason: this
     * digest must not be confusable with any other SHA-256 Atlas computes.
     *
     * Every input is Atlas-chosen — an identity hash Atlas computed, a row id, a
     * timestamp Atlas took, a retry counter, and kernel entropy — so the result
     * carries no byte anybody else selected. That is the property that lets it
     * appear in automatic model context. */
    atlas_sha256 ctx;
    atlas_sha256_init(&ctx);
    static const char domain[] = "atlas.decision.uid.v2";
    atlas_sha256_update(&ctx, domain, sizeof(domain)); /* including the NUL, as a separator */
    atlas_sha256_update(&ctx, identity_hash != NULL ? identity_hash : "",
                        strlen(identity_hash != NULL ? identity_hash : ""));
    char idbuf[64];
    int n = snprintf(idbuf, sizeof(idbuf), "|%lld|%u|", (long long)document_id, attempt);
    atlas_sha256_update(&ctx, idbuf, n > 0 ? (size_t)n : 0u);
    atlas_sha256_update(&ctx, created_at != NULL ? created_at : "",
                        strlen(created_at != NULL ? created_at : ""));
    atlas_sha256_update(&ctx, entropy, sizeof(entropy));
    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&ctx, digest);
    char hex[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_hex_encode(digest, sizeof(digest), hex);
    hex[ATLAS_DECISION_UID_HEX] = '\0';
    (void)snprintf(out, out_size, "%s%s", ATLAS_DECISION_UID_PREFIX, hex);
    return ATLAS_OK;
}

/* --- the canonical content hash -------------------------------------------
 *
 * The encoding is: a domain tag, then one record per field. A record is the
 * field's fixed Atlas-owned name, a NUL, the value's length as eight big-endian
 * bytes, and the value.
 *
 * Length-prefixed rather than delimited, because a delimiter is a byte the
 * content can contain. With a delimiter, a title of "a|b" and a decision of "c"
 * encode to the same bytes as a title of "a" and a decision of "b|c" — so two
 * different documents share a digest, and a digest two documents share is not
 * an identity. Every length is fixed-width for the same reason: a decimal
 * length is itself delimited.
 *
 * The field names are in the hash rather than only in the order, so that
 * inserting a new field later cannot make an old document's bytes reinterpret
 * as a new document's. Adding a field is still a hash change for documents that
 * use it, which is why the domain tag carries a version.
 */

static atlas_status put_u64be(atlas_buf *out, uint64_t v, atlas_err *err) {
    unsigned char b[8];
    for (size_t i = 0; i < 8u; i++) {
        b[i] = (unsigned char)((v >> (8u * (7u - i))) & 0xFFu);
    }
    return atlas_buf_append(out, b, sizeof(b), err);
}

static atlas_status put_field(atlas_buf *out, const char *name, const void *data, size_t len,
                              atlas_err *err) {
    atlas_status st = atlas_buf_append(out, name, strlen(name) + 1u, err); /* name and its NUL */
    if (st == ATLAS_OK) {
        st = put_u64be(out, (uint64_t)len, err);
    }
    if (st == ATLAS_OK && len > 0) {
        st = atlas_buf_append(out, data, len, err);
    }
    return st;
}

static atlas_status put_buf(atlas_buf *out, const char *name, const atlas_buf *b, atlas_err *err) {
    return put_field(out, name, b->data, b->len, err);
}

static atlas_status put_i64(atlas_buf *out, const char *name, int64_t v, atlas_err *err) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    return put_field(out, name, tmp, n > 0 ? (size_t)n : 0u, err);
}

/* Links participate in a canonical order Atlas imposes.
 *
 * A revision's links are a *set*: naming three paths in a different order is
 * the same decision about the same three paths. If the hash depended on the
 * order, a retry that shuffled its arguments would produce a new revision of an
 * unchanged document, and rule 11 — retries are idempotent — would be false in
 * exactly the case it is most needed.
 *
 * Alternatives are *not* sorted. A list of alternatives is ordered by the
 * proposer's judgement, and reordering it says something different. */
typedef struct link_order {
    size_t index;
    const atlas_decision_link *link;
} link_order;

static int link_cmp_bytes(const atlas_buf *a, const atlas_buf *b) {
    size_t n = a->len < b->len ? a->len : b->len;
    if (n > 0) {
        int c = memcmp(a->data, b->data, n);
        if (c != 0) {
            return c;
        }
    }
    if (a->len == b->len) {
        return 0;
    }
    return a->len < b->len ? -1 : 1;
}

static int link_cmp(const void *pa, const void *pb) {
    const link_order *a = (const link_order *)pa;
    const link_order *b = (const link_order *)pb;
    if (a->link->kind != b->link->kind) {
        return a->link->kind < b->link->kind ? -1 : 1;
    }
    int c = link_cmp_bytes(&a->link->path_raw, &b->link->path_raw);
    if (c != 0) {
        return c;
    }
    c = link_cmp_bytes(&a->link->commit_oid, &b->link->commit_oid);
    if (c != 0) {
        return c;
    }
    c = link_cmp_bytes(&a->link->symbol_name, &b->link->symbol_name);
    if (c != 0) {
        return c;
    }
    c = link_cmp_bytes(&a->link->target_uid, &b->link->target_uid);
    if (c != 0) {
        return c;
    }
    if (a->link->change_set_id != b->link->change_set_id) {
        return a->link->change_set_id < b->link->change_set_id ? -1 : 1;
    }
    /* Fully equal on every hashed field: keep the supplied order so the sort is
     * stable and the encoding is deterministic even for duplicate links. */
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

atlas_status atlas_decision_canonical_bytes(const atlas_decision_revision *r, atlas_buf *out,
                                            atlas_err *err) {
    atlas_buf_reset(out);
    atlas_status st = atlas_buf_append(out, ATLAS_DECISION_HASH_DOMAIN,
                                       sizeof(ATLAS_DECISION_HASH_DOMAIN), err); /* with NUL */
    if (st == ATLAS_OK) {
        st = put_buf(out, "title", &r->title, err);
    }
    if (st == ATLAS_OK) {
        st = put_buf(out, "context", &r->context_text, err);
    }
    if (st == ATLAS_OK) {
        st = put_buf(out, "decision", &r->decision_text, err);
    }
    if (st == ATLAS_OK) {
        st = put_buf(out, "rationale", &r->rationale_text, err);
    }
    if (st == ATLAS_OK) {
        st = put_buf(out, "consequences", &r->consequences_text, err);
    }
    if (st == ATLAS_OK) {
        st = put_field(out, "scope", atlas_decision_scope_name(r->scope),
                       strlen(atlas_decision_scope_name(r->scope)), err);
    }
    /* The basis: what this decision was taken against.
     *
     * A decision made against commit X is not the same decision made against
     * commit Y, and an approval that did not cover the basis would let the
     * basis be rewritten under an approved record. */
    if (st == ATLAS_OK) {
        st = put_buf(out, "basis_head", &r->basis_head, err);
    }
    /* Which repository. The durable identity hash rather than the row id: a row
     * id is reused and is not comparable across databases, and "we decided this
     * about that repository" is part of what was approved. */
    if (st == ATLAS_OK) {
        st = put_buf(out, "repo_identity", &r->basis_repo_identity, err);
    }
    /* Who proposed it. Mutating MODEL_INFERENCE to MODEL_PROPOSAL would upgrade
     * the apparent standing of an approved record without changing a word of
     * it, so the actor is part of the approved meaning. */
    if (st == ATLAS_OK) {
        const char *actor = atlas_decision_actor_name(r->proposed_by);
        st = put_field(out, "proposed_by", actor, strlen(actor), err);
    }
    /* The count is hashed before the elements so that a list of two empty
     * alternatives cannot encode identically to a list of none. */
    if (st == ATLAS_OK) {
        st = put_i64(out, "alternatives", (int64_t)r->alternative_count, err);
    }
    for (size_t i = 0; st == ATLAS_OK && i < r->alternative_count; i++) {
        st = put_buf(out, "alternative", &r->alternatives[i], err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    link_order order[ATLAS_DECISION_MAX_LINKS];
    size_t n = r->link_count <= ATLAS_DECISION_MAX_LINKS ? r->link_count : ATLAS_DECISION_MAX_LINKS;
    for (size_t i = 0; i < n; i++) {
        order[i].index = i;
        order[i].link = &r->links[i];
    }
    if (n > 1u) {
        qsort(order, n, sizeof(order[0]), link_cmp);
    }
    st = put_i64(out, "links", (int64_t)n, err);
    for (size_t i = 0; st == ATLAS_OK && i < n; i++) {
        const atlas_decision_link *l = order[i].link;
        const char *kind = atlas_decision_link_kind_name(l->kind);
        st = put_field(out, "link.kind", kind, strlen(kind), err);
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.path", &l->path_raw, err);
        }
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.commit", &l->commit_oid, err);
        }
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.symbol", &l->symbol_name, err);
        }
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.symbol_kind", &l->symbol_kind, err);
        }
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.target", &l->target_uid, err);
        }
        if (st == ATLAS_OK) {
            st = put_i64(out, "link.change_set", l->change_set_id, err);
        }
        if (st == ATLAS_OK) {
            st = put_i64(out, "link.symbol_line", l->symbol_line, err);
        }
        /* **The snapshot's provenance is hashed.**
         *
         * The basis commit, the file content hash captured when the link was
         * taken, and the analyzer name and version that produced the structural
         * facts are all immutable and all change what was approved: they are
         * the record of *what the decision was about at the time*. An approval
         * that did not cover them would let the captured hash be rewritten
         * under it, turning a CHANGED link into a CURRENT one without anybody
         * approving anything.
         *
         * What is not hashed is the *live* currency — CURRENT, CHANGED,
         * MISSING, AMBIGUOUS, UNKNOWN — and the match count. Those are
         * observations made when the link is read, and hashing them would make
         * an approved revision's identity change every time the code changed,
         * which is exactly backwards. Neither is stored, so neither can be. */
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.basis_commit", &l->basis_commit, err);
        }
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.file_content_hash", &l->file_content_hash, err);
        }
        if (st == ATLAS_OK) {
            st = put_buf(out, "link.analyzer", &l->analyzer_name, err);
        }
        if (st == ATLAS_OK) {
            st = put_i64(out, "link.analyzer_version", l->analyzer_version, err);
        }
    }
    return st;
}

atlas_status atlas_decision_content_hash(const atlas_decision_revision *r, char *hex_out,
                                         atlas_err *err) {
    atlas_buf canonical = ATLAS_BUF_INIT;
    atlas_status st = atlas_decision_canonical_bytes(r, &canonical, err);
    if (st == ATLAS_OK) {
        atlas_sha256_hex(canonical.data, canonical.len, hex_out);
    }
    atlas_buf_free(&canonical);
    return st;
}

/* --- the operator channel -------------------------------------------------- */

void atlas_decision_challenge_init(atlas_decision_challenge *c) {
    memset(c, 0, sizeof(*c));
}

atlas_status atlas_decision_challenge_token(char *out, size_t out_size, atlas_err *err) {
    if (out_size < ATLAS_DECISION_CHALLENGE_HEX + 1u) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "challenge token buffer is too small");
    }
    unsigned char raw[ATLAS_DECISION_CHALLENGE_BYTES];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* No fallback, deliberately.
         *
         * A capability whose token can be guessed is not a capability, and the
         * usual fallbacks — the clock, the pid, a hash of both — are guessable
         * by anything that can read `/proc`. Refusing to issue a challenge is a
         * visible failure that somebody fixes; issuing a weak one is an
         * invisible one that nobody does. */
        return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                             "cannot open /dev/urandom, so no approval challenge can be issued; "
                             "Atlas will not substitute a predictable token");
    }
    size_t got = 0;
    while (got < sizeof(raw)) {
        ssize_t n = read(fd, raw + got, sizeof(raw) - got);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            (void)close(fd);
            return atlas_err_set(err, ATLAS_ERR_INTERNAL,
                                 "cannot read randomness for an approval challenge");
        }
        got += (size_t)n;
    }
    (void)close(fd);
    atlas_hex_encode(raw, sizeof(raw), out);
    return ATLAS_OK;
}

void atlas_decision_confirm_phrase(const char *content_hash, char *out, size_t out_size) {
    size_t want = ATLAS_DECISION_CONFIRM_HEX;
    if (out_size == 0) {
        return;
    }
    if (want > out_size - 1u) {
        want = out_size - 1u;
    }
    size_t have = content_hash != NULL ? strlen(content_hash) : 0u;
    size_t n = have < want ? have : want;
    if (n > 0) {
        memcpy(out, content_hash, n);
    }
    out[n] = '\0';
}

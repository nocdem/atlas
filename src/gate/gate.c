/* Atlas - the A6 vocabularies, the fold, and the assessment container.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Nothing here reads a database or a repository. This file is the meaning of
 * the phase — what the words are, and what they imply about each other — and it
 * is separate from `assess.c` for the same reason `decision.c` is separate from
 * `lifecycle.c`: a table of rules that a test can ask directly is a table that
 * cannot silently disagree with the code that applies it.
 */
#include "gate/gate_internal.h"

#include <stdlib.h>
#include <string.h>

/* --- freshness ------------------------------------------------------------ */

static const char *const FRESHNESS_NAMES[] = {
    [ATLAS_GATE_UNKNOWN] = "UNKNOWN",
    [ATLAS_GATE_FRESH] = "FRESH",
    [ATLAS_GATE_STALE] = "STALE",
    [ATLAS_GATE_IMPACTED] = "IMPACTED",
};

const char *atlas_gate_freshness_name(atlas_gate_freshness f) {
    if ((size_t)f >= sizeof FRESHNESS_NAMES / sizeof FRESHNESS_NAMES[0]) {
        return "UNKNOWN";
    }
    return FRESHNESS_NAMES[f];
}

bool atlas_gate_freshness_parse(const char *name, atlas_gate_freshness *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof FRESHNESS_NAMES / sizeof FRESHNESS_NAMES[0]; i++) {
        if (strcmp(name, FRESHNESS_NAMES[i]) == 0) {
            *out = (atlas_gate_freshness)i;
            return true;
        }
    }
    return false;
}

/* --- the gate ------------------------------------------------------------- */

const char *atlas_gate_result_name(atlas_gate_result r) {
    switch (r) {
        case ATLAS_GATE_PASS: return "PASS";
        case ATLAS_GATE_REVIEW_REQUIRED: return "REVIEW_REQUIRED";
        case ATLAS_GATE_BLOCKED: break;
    }
    return "BLOCKED";
}

atlas_gate_result atlas_gate_fold(atlas_gate_result running, atlas_gate_freshness f) {
    /* BLOCKED absorbs. A query that has already failed to prove one thing does
     * not become safe by proving the next; the unproven one is still unproven,
     * and it is the one the caller would have acted on. */
    if (running == ATLAS_GATE_BLOCKED) {
        return ATLAS_GATE_BLOCKED;
    }
    switch (f) {
        case ATLAS_GATE_UNKNOWN: return ATLAS_GATE_BLOCKED;
        case ATLAS_GATE_STALE:
        case ATLAS_GATE_IMPACTED: return ATLAS_GATE_REVIEW_REQUIRED;
        case ATLAS_GATE_FRESH: break;
    }
    return running;
}

int atlas_gate_exit_code(atlas_gate_result r) {
    switch (r) {
        case ATLAS_GATE_PASS: return 0;
        case ATLAS_GATE_REVIEW_REQUIRED: return ATLAS_EXIT_GATE_REVIEW_REQUIRED;
        case ATLAS_GATE_BLOCKED: break;
    }
    return ATLAS_EXIT_GATE_BLOCKED;
}

/* --- reason codes ---------------------------------------------------------
 *
 * One table, carrying the name and the freshness together, so that adding a
 * reason without deciding what it implies is a compile-time hole rather than a
 * runtime surprise. */
static const struct {
    const char *name;
    atlas_gate_freshness freshness;
} REASONS[ATLAS_GATE_REASON__COUNT] = {
    [ATLAS_GATE_REASON_NO_RELEVANT_CHANGE] = {"NO_RELEVANT_CHANGE", ATLAS_GATE_FRESH},

    [ATLAS_GATE_REASON_DIRECT_EVIDENCE_CHANGED] = {"DIRECT_EVIDENCE_CHANGED", ATLAS_GATE_STALE},
    [ATLAS_GATE_REASON_LINKED_PATH_MISSING] = {"LINKED_PATH_MISSING", ATLAS_GATE_STALE},
    [ATLAS_GATE_REASON_LINKED_SYMBOL_MISSING] = {"LINKED_SYMBOL_MISSING", ATLAS_GATE_STALE},
    [ATLAS_GATE_REASON_LINKED_SYMBOL_AMBIGUOUS] = {"LINKED_SYMBOL_AMBIGUOUS", ATLAS_GATE_STALE},
    [ATLAS_GATE_REASON_LINKED_COMMIT_MISSING] = {"LINKED_COMMIT_MISSING", ATLAS_GATE_STALE},

    [ATLAS_GATE_REASON_DEPENDENCY_CHANGED] = {"DEPENDENCY_CHANGED", ATLAS_GATE_IMPACTED},

    [ATLAS_GATE_REASON_INDEX_LAG] = {"INDEX_LAG", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_STRUCTURAL_INDEX_STALE] = {"STRUCTURAL_INDEX_STALE", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_UNREACHABLE_BASE] = {"UNREACHABLE_BASE", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_HISTORY_REWRITTEN] = {"HISTORY_REWRITTEN", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_TRAVERSAL_LIMIT] = {"TRAVERSAL_LIMIT", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_EVIDENCE_UNRESOLVED] = {"EVIDENCE_UNRESOLVED", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_MISSING_VALIDATION_POINT] = {"MISSING_VALIDATION_POINT", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_REPOSITORY_AMBIGUOUS] = {"REPOSITORY_AMBIGUOUS", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_CONTENT_HASH_MISMATCH] = {"CONTENT_HASH_MISMATCH", ATLAS_GATE_UNKNOWN},
    [ATLAS_GATE_REASON_SCOPE_NOT_ASSESSABLE] = {"SCOPE_NOT_ASSESSABLE", ATLAS_GATE_UNKNOWN},
};

const char *atlas_gate_reason_name(atlas_gate_reason r) {
    if ((size_t)r >= ATLAS_GATE_REASON__COUNT || REASONS[r].name == NULL) {
        return "UNKNOWN_REASON";
    }
    return REASONS[r].name;
}

bool atlas_gate_reason_parse(const char *name, atlas_gate_reason *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < ATLAS_GATE_REASON__COUNT; i++) {
        if (REASONS[i].name != NULL && strcmp(name, REASONS[i].name) == 0) {
            *out = (atlas_gate_reason)i;
            return true;
        }
    }
    return false;
}

atlas_gate_freshness atlas_gate_reason_freshness(atlas_gate_reason r) {
    if ((size_t)r >= ATLAS_GATE_REASON__COUNT || REASONS[r].name == NULL) {
        return ATLAS_GATE_UNKNOWN;
    }
    return REASONS[r].freshness;
}

/* --- an assessment -------------------------------------------------------- */

void atlas_gate_assessment_init(atlas_gate_assessment *a) {
    memset(a, 0, sizeof(*a));
    atlas_buf_init(&a->repo_name);
    atlas_buf_init(&a->root_text);
    atlas_buf_init(&a->repo_identity_hash);
    atlas_buf_init(&a->uid);
    atlas_buf_init(&a->title);
    /* Zero is UNKNOWN for both, which is the correct state for an assessment
     * that has not been made yet. */
}

void atlas_gate_assessment_free(atlas_gate_assessment *a) {
    atlas_buf_free(&a->repo_name);
    atlas_buf_free(&a->root_text);
    atlas_buf_free(&a->repo_identity_hash);
    atlas_buf_free(&a->uid);
    atlas_buf_free(&a->title);
}

/* Freshness ordered from weakest to strongest, so "the verdict is the weakest
 * of its reasons" is one comparison rather than a nest of cases. */
static int strength(atlas_gate_freshness f) {
    switch (f) {
        case ATLAS_GATE_UNKNOWN: return 0;
        case ATLAS_GATE_STALE: return 1;
        case ATLAS_GATE_IMPACTED: return 2;
        case ATLAS_GATE_FRESH: break;
    }
    return 3;
}

void atlas_gate_assessment_note(atlas_gate_assessment *a, atlas_gate_reason r) {
    if ((size_t)r >= ATLAS_GATE_REASON__COUNT) {
        r = ATLAS_GATE_REASON_CONTENT_HASH_MISMATCH; /* unreachable; still UNKNOWN */
    }
    atlas_gate_freshness f = atlas_gate_reason_freshness(r);

    /* The verdict is folded first and unconditionally. A reason that does not
     * fit in the list must still be able to weaken the answer, or a decision
     * with thirteen problems would report a better verdict than one with
     * twelve. */
    if (a->reason_count == 0) {
        a->freshness = f;
    } else if (strength(f) < strength(a->freshness)) {
        a->freshness = f;
    }

    for (size_t i = 0; i < a->reason_count; i++) {
        if (a->reasons[i] == r) {
            return;
        }
    }
    if (a->reason_count < ATLAS_GATE_MAX_REASONS) {
        a->reasons[a->reason_count++] = r;
    }
}

/* --- the packed reason list ------------------------------------------------ */

atlas_status atlas_gate_reasons_pack(const atlas_gate_assessment *a, atlas_buf *out,
                                     atlas_err *err) {
    /* Ascending enum order rather than the order they were noted: the stored
     * form is a set, and two assessments that found the same problems in a
     * different order are the same assessment. */
    atlas_status st = ATLAS_OK;
    bool first = true;
    for (size_t r = 0; r < ATLAS_GATE_REASON__COUNT && st == ATLAS_OK; r++) {
        bool present = false;
        for (size_t i = 0; i < a->reason_count; i++) {
            if ((size_t)a->reasons[i] == r) {
                present = true;
                break;
            }
        }
        if (!present) {
            continue;
        }
        const char *name = atlas_gate_reason_name((atlas_gate_reason)r);
        if (out->len + strlen(name) + 1u > ATLAS_GATE_MAX_REASON_TEXT) {
            return atlas_err_set(err, ATLAS_ERR_INTERNAL, "the reason list is too long to store");
        }
        st = atlas_buf_appendf(out, err, "%s%s", first ? "" : " ", name);
        first = false;
    }
    return st;
}

atlas_status atlas_gate_reasons_unpack(const char *packed, atlas_gate_reason *out, size_t max,
                                       size_t *count_out, atlas_err *err) {
    *count_out = 0;
    if (packed == NULL || packed[0] == '\0') {
        return ATLAS_OK;
    }
    if (strlen(packed) >= ATLAS_GATE_MAX_REASON_TEXT) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "the stored reason list is too long");
    }
    const char *p = packed;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        size_t n = (size_t)(p - start);
        /* A token is compared against the vocabulary and never reproduced. The
         * stored bytes cannot become output by failing to parse. */
        char token[64];
        if (n >= sizeof token) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the stored reason list holds an unknown code");
        }
        memcpy(token, start, n);
        token[n] = '\0';
        atlas_gate_reason r;
        if (!atlas_gate_reason_parse(token, &r)) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the stored reason list holds an unknown code");
        }
        if (*count_out >= max) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "the stored reason list holds too many codes");
        }
        out[(*count_out)++] = r;
    }
    return ATLAS_OK;
}

/* --- a query and its report ------------------------------------------------ */

void atlas_gate_query_init(atlas_gate_query *q) {
    memset(q, 0, sizeof(*q));
}

void atlas_gate_report_init(atlas_gate_report *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->repo_name);
    atlas_buf_init(&r->root_text);
    atlas_buf_init(&r->repo_identity_hash);
    /* BLOCKED until an assessment says otherwise. A report nobody filled in
     * must not read as a pass — and because BLOCKED absorbs in
     * `atlas_gate_fold`, the engine must set this to PASS deliberately at the
     * point it commits to producing a real report, rather than folding into the
     * safe default and discovering that nothing can ever lift it. */
    r->result = ATLAS_GATE_BLOCKED;
}

/* --- the shared repository environment ------------------------------------- */

void atlas_gate_env_init(atlas_gate_env *e) {
    memset(e, 0, sizeof(*e));
    atlas_buf_init(&e->repo_name);
    atlas_buf_init(&e->root_text);
    atlas_buf_init(&e->repo_identity_hash);
}

void atlas_gate_env_free(atlas_gate_env *e) {
    atlas_buf_free(&e->repo_name);
    atlas_buf_free(&e->root_text);
    atlas_buf_free(&e->repo_identity_hash);
}

void atlas_gate_env_note(atlas_gate_env *e, atlas_gate_reason r) {
    for (size_t i = 0; i < e->reason_count; i++) {
        if (e->reasons[i] == r) {
            return;
        }
    }
    if (e->reason_count < ATLAS_GATE_MAX_REASONS) {
        e->reasons[e->reason_count++] = r;
    }
}

void atlas_gate_report_free(atlas_gate_report *r) {
    for (size_t i = 0; i < r->item_count; i++) {
        atlas_gate_assessment_free(&r->items[i]);
    }
    free(r->items);
    r->items = NULL;
    r->item_count = 0;
    atlas_buf_free(&r->repo_name);
    atlas_buf_free(&r->root_text);
    atlas_buf_free(&r->repo_identity_hash);
}

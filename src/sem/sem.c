/* Atlas - the semantic vocabulary and the configuration digest.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The names here are a contract: they are stored in the database, they appear
 * in JSON, and they reach a model's context through explicit MCP results. Every
 * parse function refuses anything not in its set rather than defaulting to a
 * member of it — A3's rule, and the reason is the same. An unrecognised
 * evidence class that quietly became PROVEN would be a guess wearing the word
 * this whole layer exists to protect.
 */
#include "atlas/sem.h"

#include <string.h>

#include "atlas/sha256.h"

/* --- evidence -------------------------------------------------------------- */

static const char *const EVIDENCE_NAMES[] = {
    "UNKNOWN",
    "PROVEN",
    "CANDIDATE",
    "LEXICAL",
};

const char *atlas_sem_evidence_name(atlas_sem_evidence e) {
    size_t i = (size_t)e;
    if (i >= sizeof(EVIDENCE_NAMES) / sizeof(EVIDENCE_NAMES[0])) {
        return "UNKNOWN";
    }
    return EVIDENCE_NAMES[i];
}

bool atlas_sem_evidence_parse(const char *name, atlas_sem_evidence *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(EVIDENCE_NAMES) / sizeof(EVIDENCE_NAMES[0]); i++) {
        if (strcmp(name, EVIDENCE_NAMES[i]) == 0) {
            *out = (atlas_sem_evidence)i;
            return true;
        }
    }
    return false;
}

/* Strength order, asked rather than restated.
 *
 * The enum's numeric order is UNKNOWN, PROVEN, CANDIDATE, LEXICAL — chosen so
 * UNKNOWN is zero, which means the numeric order is *not* the strength order.
 * That is deliberate: a comparison written as `a < b` would silently be wrong,
 * so there is no comparison to get wrong. Everything asks this function. */
static int strength(atlas_sem_evidence e) {
    switch (e) {
        case ATLAS_SEM_EV_PROVEN:
            return 3;
        case ATLAS_SEM_EV_CANDIDATE:
            return 2;
        case ATLAS_SEM_EV_LEXICAL:
            return 1;
        case ATLAS_SEM_EV_UNKNOWN:
        default:
            return 0;
    }
}

atlas_sem_evidence atlas_sem_evidence_weaker(atlas_sem_evidence a, atlas_sem_evidence b) {
    return strength(a) <= strength(b) ? a : b;
}

bool atlas_sem_evidence_writable_by_indexer(atlas_sem_evidence e) {
    return e == ATLAS_SEM_EV_PROVEN || e == ATLAS_SEM_EV_CANDIDATE ||
           e == ATLAS_SEM_EV_LEXICAL || e == ATLAS_SEM_EV_UNKNOWN;
}

/* --- symbol kinds ---------------------------------------------------------- */

static const char *const SYMBOL_KIND_NAMES[] = {
    "UNKNOWN", "FUNCTION", "STRUCT",   "UNION",     "ENUM",     "ENUM_CONSTANT",
    "TYPEDEF", "FIELD",    "VARIABLE", "PARAMETER", "MACRO",
};

const char *atlas_sem_symbol_kind_name(atlas_sem_symbol_kind k) {
    size_t i = (size_t)k;
    if (i >= sizeof(SYMBOL_KIND_NAMES) / sizeof(SYMBOL_KIND_NAMES[0])) {
        return "UNKNOWN";
    }
    return SYMBOL_KIND_NAMES[i];
}

bool atlas_sem_symbol_kind_parse(const char *name, atlas_sem_symbol_kind *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(SYMBOL_KIND_NAMES) / sizeof(SYMBOL_KIND_NAMES[0]); i++) {
        if (strcmp(name, SYMBOL_KIND_NAMES[i]) == 0) {
            *out = (atlas_sem_symbol_kind)i;
            return true;
        }
    }
    return false;
}

/* --- linkage --------------------------------------------------------------- */

static const char *const LINKAGE_NAMES[] = {
    "UNKNOWN",
    "EXTERNAL",
    "INTERNAL",
    "NONE",
};

const char *atlas_sem_linkage_name(atlas_sem_linkage l) {
    size_t i = (size_t)l;
    if (i >= sizeof(LINKAGE_NAMES) / sizeof(LINKAGE_NAMES[0])) {
        return "UNKNOWN";
    }
    return LINKAGE_NAMES[i];
}

bool atlas_sem_linkage_parse(const char *name, atlas_sem_linkage *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(LINKAGE_NAMES) / sizeof(LINKAGE_NAMES[0]); i++) {
        if (strcmp(name, LINKAGE_NAMES[i]) == 0) {
            *out = (atlas_sem_linkage)i;
            return true;
        }
    }
    return false;
}

/* --- edge kinds ------------------------------------------------------------ */

static const char *const EDGE_KIND_NAMES[] = {
    "UNKNOWN",       "CALLS",     "MAY_CALL",  "ADDRESS_TAKEN", "REFERENCES",
    "DECLARATION_OF", "HAS_FIELD", "HAS_TYPE", "PARAM_TYPE",    "RETURN_TYPE",
    "EXPANDED_FROM",
};

const char *atlas_sem_edge_kind_name(atlas_sem_edge_kind k) {
    size_t i = (size_t)k;
    if (i >= sizeof(EDGE_KIND_NAMES) / sizeof(EDGE_KIND_NAMES[0])) {
        return "UNKNOWN";
    }
    return EDGE_KIND_NAMES[i];
}

bool atlas_sem_edge_kind_parse(const char *name, atlas_sem_edge_kind *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(EDGE_KIND_NAMES) / sizeof(EDGE_KIND_NAMES[0]); i++) {
        if (strcmp(name, EDGE_KIND_NAMES[i]) == 0) {
            *out = (atlas_sem_edge_kind)i;
            return true;
        }
    }
    return false;
}

/* The ceiling each edge kind may carry.
 *
 * This is the single authority, asked at the write point, so a bug in the
 * extractor cannot mint a proven indirect call. MAY_CALL caps at CANDIDATE
 * because that is what an indirect call *is*: Clang gave a prototype, Atlas
 * found functions whose address was taken with that prototype, and a bounded
 * possible set is not a resolution. Claiming otherwise is the one overclaim
 * this season forbids by name. */
atlas_sem_evidence atlas_sem_edge_kind_max_evidence(atlas_sem_edge_kind k) {
    switch (k) {
        case ATLAS_SEM_EDGE_MAY_CALL:
            return ATLAS_SEM_EV_CANDIDATE;
        case ATLAS_SEM_EDGE_CALLS:
        case ATLAS_SEM_EDGE_ADDRESS_TAKEN:
        case ATLAS_SEM_EDGE_REFERENCES:
        case ATLAS_SEM_EDGE_DECLARATION_OF:
        case ATLAS_SEM_EDGE_HAS_FIELD:
        case ATLAS_SEM_EDGE_HAS_TYPE:
        case ATLAS_SEM_EDGE_PARAM_TYPE:
        case ATLAS_SEM_EDGE_RETURN_TYPE:
        case ATLAS_SEM_EDGE_EXPANDED_FROM:
            return ATLAS_SEM_EV_PROVEN;
        case ATLAS_SEM_EDGE_UNKNOWN:
        default:
            return ATLAS_SEM_EV_UNKNOWN;
    }
}

/* --- generation and unit status -------------------------------------------- */

static const char *const GEN_STATUS_NAMES[] = {
    "UNKNOWN",
    "RUNNING",
    "COMPLETE",
    "FAILED",
};

const char *atlas_sem_gen_status_name(atlas_sem_gen_status s) {
    size_t i = (size_t)s;
    if (i >= sizeof(GEN_STATUS_NAMES) / sizeof(GEN_STATUS_NAMES[0])) {
        return "UNKNOWN";
    }
    return GEN_STATUS_NAMES[i];
}

bool atlas_sem_gen_status_parse(const char *name, atlas_sem_gen_status *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(GEN_STATUS_NAMES) / sizeof(GEN_STATUS_NAMES[0]); i++) {
        if (strcmp(name, GEN_STATUS_NAMES[i]) == 0) {
            *out = (atlas_sem_gen_status)i;
            return true;
        }
    }
    return false;
}

static const char *const TU_STATUS_NAMES[] = {
    "UNKNOWN", "COMPLETE", "PARTIAL", "FAILED", "UNSUPPORTED",
};

const char *atlas_sem_tu_status_name(atlas_sem_tu_status s) {
    size_t i = (size_t)s;
    if (i >= sizeof(TU_STATUS_NAMES) / sizeof(TU_STATUS_NAMES[0])) {
        return "UNKNOWN";
    }
    return TU_STATUS_NAMES[i];
}

bool atlas_sem_tu_status_parse(const char *name, atlas_sem_tu_status *out) {
    if (name == NULL || out == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(TU_STATUS_NAMES) / sizeof(TU_STATUS_NAMES[0]); i++) {
        if (strcmp(name, TU_STATUS_NAMES[i]) == 0) {
            *out = (atlas_sem_tu_status)i;
            return true;
        }
    }
    return false;
}

static const char *const WHY[] = {
    ATLAS_SEM_WHY_PARSE_ERROR,  ATLAS_SEM_WHY_NO_TU,       ATLAS_SEM_WHY_OUTSIDE_REPO,
    ATLAS_SEM_WHY_ARG_REFUSED,  ATLAS_SEM_WHY_TIMEOUT,     ATLAS_SEM_WHY_TOO_LARGE,
    ATLAS_SEM_WHY_CHILD_FAILED, ATLAS_SEM_WHY_MISSING_FILE,
};

const char *atlas_sem_why_intern(const char *why) {
    if (why == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(WHY) / sizeof(WHY[0]); i++) {
        if (strcmp(why, WHY[i]) == 0) {
            return WHY[i];
        }
    }
    return NULL;
}

bool atlas_sem_why_is_known(const char *why) { return atlas_sem_why_intern(why) != NULL; }

/* --- freshness ------------------------------------------------------------- */

static const char *const FRESHNESS_NAMES[] = {
    "ABSENT",
    "CURRENT",
    "STALE",
    "REBUILDING",
};

const char *atlas_sem_freshness_name(atlas_sem_freshness f) {
    size_t i = (size_t)f;
    if (i >= sizeof(FRESHNESS_NAMES) / sizeof(FRESHNESS_NAMES[0])) {
        return "ABSENT";
    }
    return FRESHNESS_NAMES[i];
}

const char *atlas_sem_stale_reason_intern(const char *reason) {
    static const char *const REASONS[] = {
        ATLAS_SEM_STALE_COMMIT,     ATLAS_SEM_STALE_COMPDB,     ATLAS_SEM_STALE_COMPILER,
        ATLAS_SEM_STALE_ANALYZER,   ATLAS_SEM_STALE_FILE_INDEX, ATLAS_SEM_STALE_INCOMPLETE,
    };
    if (reason == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(REASONS) / sizeof(REASONS[0]); i++) {
        if (strcmp(reason, REASONS[i]) == 0) {
            return REASONS[i];
        }
    }
    return NULL;
}

bool atlas_sem_stale_reason_is_known(const char *reason) {
    return atlas_sem_stale_reason_intern(reason) != NULL;
}

void atlas_sem_generation_init(atlas_sem_generation *g) {
    if (g == NULL) {
        return;
    }
    memset(g, 0, sizeof(*g));
    /* Everything a memset leaves is the safe reading: UNKNOWN status, no
     * counts, not current. */
}

/* --- the configuration digest ----------------------------------------------
 *
 * Domain-separated and length-prefixed, never delimited. With any single-byte
 * separator a define of `A=B` followed by `C` would encode identically to `A`
 * followed by `B=C`, and two genuinely different compilation configurations
 * would share an identity — which is exactly the confusion this digest exists
 * to prevent. A4's canonical content hash makes the same argument; this is the
 * same construction. */
static void feed(atlas_sha256 *h, const char *s) {
    uint64_t n = s == NULL ? 0 : (uint64_t)strlen(s);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (unsigned char)((n >> (8 * (7 - i))) & 0xffu);
    }
    atlas_sha256_update(h, len, sizeof(len));
    if (n > 0) {
        atlas_sha256_update(h, s, (size_t)n);
    }
}

atlas_status atlas_sem_config_digest(const atlas_code_compdb *db, size_t unit_index, char out[65],
                                     atlas_err *err) {
    if (db == NULL || out == NULL || unit_index >= db->unit_count) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic configuration digest: bad request");
    }
    const atlas_code_cu *cu = &db->units[unit_index];

    atlas_sha256 h;
    atlas_sha256_init(&h);
    feed(&h, ATLAS_SEM_CONFIG_DOMAIN);
    feed(&h, atlas_code_compdb_str(db, cu->std_off));
    feed(&h, atlas_code_compdb_str(db, cu->lang_off));

    /* Include directories in the order the entry gave them: search order is
     * semantics, not presentation, so this is a list rather than a set. */
    for (size_t i = 0; i < cu->incdir_count; i++) {
        const atlas_code_cu_incdir *d = &db->incdirs[cu->incdir_first + i];
        feed(&h, atlas_code_incdir_kind_name((atlas_code_incdir_kind)d->kind));
        feed(&h, atlas_code_compdb_str(db, d->path_off));
        feed(&h, d->external ? "external" : "internal");
    }
    /* Defines likewise: a later -D overrides an earlier one. */
    for (size_t i = 0; i < cu->define_count; i++) {
        const atlas_code_cu_define *d = &db->defines[cu->define_first + i];
        feed(&h, d->undef ? "U" : "D");
        feed(&h, atlas_code_compdb_str(db, d->name_off));
        feed(&h, d->value_len > 0 ? atlas_code_compdb_str(db, d->value_off) : "");
    }

    unsigned char digest[ATLAS_SHA256_DIGEST_LEN];
    atlas_sha256_final(&h, digest);
    atlas_hex_encode(digest, sizeof(digest), out);
    out[ATLAS_SHA256_HEX_LEN] = '\0';
    return ATLAS_OK;
}

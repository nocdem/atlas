/* Atlas - the structural vocabulary, language classification and file roles.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The names in this file are a contract: they are stored in the database, they
 * appear in JSON, and they reach a model's context through explicit MCP
 * results. Every parse function refuses anything not in its set rather than
 * defaulting to a member of it — defaulting an unknown resolution class to a
 * known one is how a guess becomes a recorded fact.
 */
#include "atlas/code.h"

#include <string.h>

/* --- resolution ----------------------------------------------------------- */

static const char *const RESOLUTION_NAMES[] = {
    "SOURCE_EXACT", "BUILD_METADATA", "UNIQUE_LEXICAL", "AMBIGUOUS",
    "UNRESOLVED",   "CONDITIONAL",    "MODEL_PROPOSAL", "UNKNOWN",
};

const char *atlas_code_resolution_name(atlas_code_resolution r) {
    size_t i = (size_t)r;
    if (i >= sizeof(RESOLUTION_NAMES) / sizeof(RESOLUTION_NAMES[0])) {
        return "UNKNOWN";
    }
    return RESOLUTION_NAMES[i];
}

bool atlas_code_resolution_parse(const char *name, atlas_code_resolution *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(RESOLUTION_NAMES) / sizeof(RESOLUTION_NAMES[0]); i++) {
        if (strcmp(name, RESOLUTION_NAMES[i]) == 0) {
            *out = (atlas_code_resolution)i;
            return true;
        }
    }
    return false;
}

bool atlas_code_resolution_writable_in_a3(atlas_code_resolution r) {
    /* MODEL_PROPOSAL exists in the vocabulary and in the schema so a later phase
     * can start producing it deliberately. A3's indexer reads bytes; it has no
     * model, and a class it cannot justify is one it must not be able to write
     * by accident. Same shape as atlas_provenance_writable_in_a2. */
    return r != ATLAS_CODE_RES_MODEL_PROPOSAL;
}

bool atlas_code_resolution_is_resolved(atlas_code_resolution r) {
    return r == ATLAS_CODE_RES_SOURCE_EXACT || r == ATLAS_CODE_RES_BUILD_METADATA ||
           r == ATLAS_CODE_RES_UNIQUE_LEXICAL;
}

static const char *const PROVENANCE_NAMES[] = {
    "SOURCE",
    "BUILD_METADATA",
    "INFERENCE",
    "UNKNOWN",
};

const char *atlas_code_provenance_name(atlas_code_provenance p) {
    size_t i = (size_t)p;
    if (i >= sizeof(PROVENANCE_NAMES) / sizeof(PROVENANCE_NAMES[0])) {
        return "UNKNOWN";
    }
    return PROVENANCE_NAMES[i];
}

/* The complete set of reasons Atlas will state for an unresolved or ambiguous
 * relation.
 *
 * Listed again here, and checked before a reason is emitted, for the same
 * reason the context envelope re-lists its own `not_current` strings: this value
 * reaches a model, so a value that is not one of Atlas' own must become "other"
 * rather than be reproduced. */
bool atlas_code_why_is_known(const char *why) {
    static const char *const WHY[] = {
        ATLAS_CODE_WHY_SYSTEM_HEADER,     ATLAS_CODE_WHY_NOT_IN_REPO,
        ATLAS_CODE_WHY_MANY_FILES,        ATLAS_CODE_WHY_NO_DEFINITION,
        ATLAS_CODE_WHY_DECL_ONLY,         ATLAS_CODE_WHY_MANY_DEFINITIONS,
        ATLAS_CODE_WHY_INDIRECT,          ATLAS_CODE_WHY_MACRO_AND_FUNCTION,
        ATLAS_CODE_WHY_CONDITIONAL,       ATLAS_CODE_WHY_DERIVED_INCLUDE,
        ATLAS_CODE_WHY_DERIVED_CALL,      ATLAS_CODE_WHY_TRUNCATED,
        NULL,
    };
    if (why == NULL || why[0] == '\0') {
        return false;
    }
    for (size_t i = 0; WHY[i] != NULL; i++) {
        if (strcmp(why, WHY[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* --- currency ---------------------------------------------------------------
 *
 * The complete set of reasons Atlas will state for a structural index not being
 * current. Listed once, checked before one is reported, for the same reason
 * `atlas_ai_context_is_bounded` re-lists its own: these strings reach a model,
 * and a value that is not one of Atlas' own must become "other" rather than be
 * reproduced. */

#define WHY_NO_PASS "no structural pass has completed for this repository yet"
#define WHY_OLDER "the structural index describes an older generation than the file index"
#define WHY_DEGRADED "the structural index is degraded: a parse failed or a ceiling was reached"
#define WHY_FILE_STALE "the file index is not current, so the structural index cannot be either"
#define WHY_ANALYZER "the structural index was produced by a different analyzer version"

/* Resolves a reason back to the Atlas literal it must be, for a reader that
 * received one over the socket: `not_current_reason` is a `const char *` into
 * static storage everywhere else, so aliasing a response buffer about to be
 * freed would leave a dangling pointer in a report. An unrecognised value
 * becomes NULL rather than being reproduced. */
const char *atlas_code_not_current_reason_intern(const char *reason) {
    static const char *const REASONS[] = {
        WHY_NO_PASS, WHY_OLDER, WHY_DEGRADED, WHY_FILE_STALE, WHY_ANALYZER, NULL,
    };
    if (reason == NULL || reason[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; REASONS[i] != NULL; i++) {
        if (strcmp(reason, REASONS[i]) == 0) {
            return REASONS[i];
        }
    }
    return NULL;
}

bool atlas_code_not_current_reason_is_known(const char *reason) {
    static const char *const REASONS[] = {
        WHY_NO_PASS, WHY_OLDER, WHY_DEGRADED, WHY_FILE_STALE, WHY_ANALYZER, NULL,
    };
    if (reason == NULL || reason[0] == '\0') {
        return false;
    }
    for (size_t i = 0; REASONS[i] != NULL; i++) {
        if (strcmp(reason, REASONS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool atlas_code_index_current(const atlas_index_state *file_state,
                              const atlas_code_index_state *code_state, bool file_current,
                              const char **reason_out) {
    if (code_state == NULL || !code_state->present || code_state->last_complete_generation <= 0) {
        *reason_out = WHY_NO_PASS;
        return false;
    }
    if (!file_current) {
        /* A graph built from a file index nobody can vouch for is not a graph
         * anybody should act on. The structural index inherits the file index's
         * honesty rather than being allowed to look better than its input. */
        *reason_out = WHY_FILE_STALE;
        return false;
    }
    if (file_state != NULL &&
        code_state->last_complete_generation < file_state->last_complete_generation) {
        *reason_out = WHY_OLDER;
        return false;
    }
    if (code_state->degraded) {
        *reason_out = WHY_DEGRADED;
        return false;
    }
    if (!atlas_code_analyzer_matches(code_state)) {
        /* Every generation can line up, every byte of the repository can be
         * unchanged, and the graph can still be wrong — because the algorithm
         * that produced it has been corrected since. Nothing else in this
         * function can see that, which is precisely why this check exists
         * rather than being implied by the others. */
        *reason_out = WHY_ANALYZER;
        return false;
    }
    *reason_out = NULL;
    return true;
}

bool atlas_code_analyzer_matches(const atlas_code_index_state *code_state) {
    if (code_state == NULL || !code_state->present) {
        return false;
    }
    if (code_state->analyzer_version != (int64_t)ATLAS_CODE_ANALYZER_VERSION) {
        return false;
    }
    const char *name = atlas_buf_cstr(&code_state->analyzer_name);
    return name != NULL && strcmp(name, ATLAS_CODE_ANALYZER_ID) == 0;
}

/* --- language -------------------------------------------------------------- */

const char *atlas_code_language_name(atlas_code_language l) {
    switch (l) {
    case ATLAS_CODE_LANG_C: return "c";
    case ATLAS_CODE_LANG_C_HEADER: return "c-header";
    case ATLAS_CODE_LANG_C_FRAGMENT: return "c-fragment";
    case ATLAS_CODE_LANG_NONE:
    default: return "";
    }
}

/* True when the path's last component ends with `ext`.
 *
 * Deliberately case-sensitive. `.C` is C++ by long convention and `.H` appears
 * in projects that mean C++ headers; matching them case-insensitively would have
 * Atlas extract C semantics from C++, which is exactly the guess A3 refuses to
 * make. A case-insensitive filesystem does not change what the bytes say. */
static bool ends_with(const void *path, size_t len, const char *ext) {
    size_t n = strlen(ext);
    if (len < n) {
        return false;
    }
    return memcmp((const char *)path + (len - n), ext, n) == 0;
}

atlas_code_language atlas_code_language_of(const void *path_raw, size_t path_len) {
    if (path_raw == NULL || path_len == 0) {
        return ATLAS_CODE_LANG_NONE;
    }
    if (ends_with(path_raw, path_len, ".c")) {
        return ATLAS_CODE_LANG_C;
    }
    if (ends_with(path_raw, path_len, ".h")) {
        return ATLAS_CODE_LANG_C_HEADER;
    }
    if (ends_with(path_raw, path_len, ".inc") || ends_with(path_raw, path_len, ".def")) {
        return ATLAS_CODE_LANG_C_FRAGMENT;
    }
    return ATLAS_CODE_LANG_NONE;
}

/* --- symbol vocabulary ------------------------------------------------------ */

static const char *const SYMBOL_KIND_NAMES[] = {
    "function", "macro",         "macro_function", "typedef", "struct",
    "union",    "enum",          "enum_constant",  "variable", "unknown",
};

const char *atlas_code_symbol_kind_name(atlas_code_symbol_kind k) {
    size_t i = (size_t)k;
    if (i >= sizeof(SYMBOL_KIND_NAMES) / sizeof(SYMBOL_KIND_NAMES[0])) {
        return "unknown";
    }
    return SYMBOL_KIND_NAMES[i];
}

bool atlas_code_symbol_kind_parse(const char *name, atlas_code_symbol_kind *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(SYMBOL_KIND_NAMES) / sizeof(SYMBOL_KIND_NAMES[0]); i++) {
        if (strcmp(name, SYMBOL_KIND_NAMES[i]) == 0) {
            *out = (atlas_code_symbol_kind)i;
            return true;
        }
    }
    return false;
}

const char *atlas_code_linkage_name(atlas_code_linkage l) {
    switch (l) {
    case ATLAS_CODE_LINK_EXTERNAL: return "external";
    case ATLAS_CODE_LINK_INTERNAL: return "internal";
    case ATLAS_CODE_LINK_NONE: return "none";
    case ATLAS_CODE_LINK_UNKNOWN:
    default: return "unknown";
    }
}

static const char *const ROLE_NAMES[] = {
    "implementation", "public_header", "private_header", "test",   "build_metadata",
    "documentation",  "vendored",      "generated",      "unknown",
};

const char *atlas_code_role_name(atlas_code_role r) {
    size_t i = (size_t)r;
    if (i >= sizeof(ROLE_NAMES) / sizeof(ROLE_NAMES[0])) {
        return "unknown";
    }
    return ROLE_NAMES[i];
}

static const char *const BASIS_NAMES[] = {
    "extension", "path_naming", "content_marker", "build_metadata", "include_graph", "none",
};

const char *atlas_code_role_basis_name(atlas_code_role_basis b) {
    size_t i = (size_t)b;
    if (i >= sizeof(BASIS_NAMES) / sizeof(BASIS_NAMES[0])) {
        return "none";
    }
    return BASIS_NAMES[i];
}

static const char *const REL_KIND_NAMES[] = {
    "file_includes_file",         "file_defines_symbol", "file_declares_symbol",
    "unit_compiles_file",         "unit_uses_header",    "symbol_contains_occurrence",
    "symbol_calls_symbol",        "symbol_declared_by",  "symbol_defined_by",
    "file_depends_on_file",
};

const char *atlas_code_rel_kind_name(atlas_code_rel_kind k) {
    size_t i = (size_t)k;
    if (i >= sizeof(REL_KIND_NAMES) / sizeof(REL_KIND_NAMES[0])) {
        return "file_depends_on_file";
    }
    return REL_KIND_NAMES[i];
}

bool atlas_code_rel_kind_parse(const char *name, atlas_code_rel_kind *out) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(REL_KIND_NAMES) / sizeof(REL_KIND_NAMES[0]); i++) {
        if (strcmp(name, REL_KIND_NAMES[i]) == 0) {
            *out = (atlas_code_rel_kind)i;
            return true;
        }
    }
    return false;
}

const char *atlas_code_node_kind_name(atlas_code_node_kind k) {
    switch (k) {
    case ATLAS_CODE_NODE_FILE: return "file";
    case ATLAS_CODE_NODE_SYMBOL: return "symbol";
    case ATLAS_CODE_NODE_UNIT: return "unit";
    case ATLAS_CODE_NODE_OCCURRENCE: return "occurrence";
    case ATLAS_CODE_NODE_UNRESOLVED:
    default: return "unresolved";
    }
}

const char *atlas_code_parse_status_name(atlas_code_parse_status s) {
    switch (s) {
    case ATLAS_CODE_PARSE_OK: return "ok";
    case ATLAS_CODE_PARSE_PARTIAL: return "partial";
    case ATLAS_CODE_PARSE_FAILED: return "failed";
    case ATLAS_CODE_PARSE_SKIPPED:
    default: return "skipped";
    }
}

const char *atlas_code_incdir_kind_name(atlas_code_incdir_kind k) {
    switch (k) {
    case ATLAS_CODE_INCDIR_SEARCH: return "search";
    case ATLAS_CODE_INCDIR_QUOTE: return "quote";
    case ATLAS_CODE_INCDIR_SYSTEM: return "system";
    case ATLAS_CODE_INCDIR_AFTER:
    default: return "after";
    }
}

/* --- file roles -------------------------------------------------------------
 *
 * Classification from path bytes and a bounded content prefix, with the basis
 * recorded alongside every role.
 *
 * The basis is the honest part. A file under `tests/` is *named* like a test;
 * that is a fact about the path and not proof about the file, and a consumer
 * shown `role=test basis=path_naming` knows exactly how much Atlas knows. A role
 * with no basis is not recorded at all.
 *
 * Roles are additive: a `.c` file under `tests/` is both an implementation and a
 * test, and a vendored header is both vendored and a header. Collapsing those to
 * one "primary" role would throw away the fact somebody actually wants. */

/* True when `needle` is a whole path component of `path`. Component matching
 * rather than substring, so a directory called `thirdpartytools` is not
 * vendored and a file called `contributing.md` is not either. */
static bool has_component(const void *path, size_t len, const char *needle) {
    const char *p = (const char *)path;
    size_t n = strlen(needle);
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || p[i] == '/') {
            if (i - start == n && memcmp(p + start, needle, n) == 0) {
                return true;
            }
            start = i + 1u;
        }
    }
    return false;
}

/* The last path component. */
static const char *basename_of(const void *path, size_t len, size_t *out_len) {
    const char *p = (const char *)path;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '/') {
            start = i + 1u;
        }
    }
    *out_len = len - start;
    return p + start;
}

static bool starts_with(const void *s, size_t len, const char *prefix) {
    size_t n = strlen(prefix);
    return len >= n && memcmp(s, prefix, n) == 0;
}

static bool name_is(const void *s, size_t len, const char *want) {
    size_t n = strlen(want);
    return len == n && memcmp(s, want, n) == 0;
}

static void add_role(atlas_code_roles *out, atlas_code_role role, atlas_code_role_basis basis,
                     atlas_code_resolution res) {
    if (out->count >= ATLAS_CODE_MAX_ROLES_PER_FILE) {
        return;
    }
    for (size_t i = 0; i < out->count; i++) {
        if (out->items[i].role == (int32_t)role) {
            return; /* already claimed, on whatever basis came first */
        }
    }
    out->items[out->count].role = (int32_t)role;
    out->items[out->count].basis = (int32_t)basis;
    out->items[out->count].resolution = (int32_t)res;
    out->count++;
}

/* Case-insensitive search for a generated-file marker in a bounded prefix.
 *
 * Bounded to what the caller supplied, which is the first few kilobytes: a
 * marker that far into a file is a coincidence rather than a header, and
 * scanning a whole generated file to find one would cost more than the answer. */
static bool prefix_contains_ci(const void *prefix, size_t len, const char *needle) {
    if (prefix == NULL || len == 0) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)prefix;
    size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return false;
    }
    for (size_t i = 0; i + n <= len; i++) {
        size_t k = 0;
        while (k < n) {
            unsigned char a = p[i + k];
            unsigned char b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z') {
                a = (unsigned char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (unsigned char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            k++;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

void atlas_code_classify_roles(const void *path_raw, size_t path_len, const void *prefix,
                               size_t prefix_len, atlas_code_roles *out) {
    memset(out, 0, sizeof(*out));
    if (path_raw == NULL || path_len == 0) {
        add_role(out, ATLAS_CODE_ROLE_UNKNOWN, ATLAS_CODE_BASIS_NONE, ATLAS_CODE_RES_UNKNOWN);
        return;
    }
    size_t base_len = 0;
    const char *base = basename_of(path_raw, path_len, &base_len);

    /* Vendored first: a vendored test or header is still vendored, and knowing
     * that changes what a reader does with everything else about it. */
    if (has_component(path_raw, path_len, "third_party") ||
        has_component(path_raw, path_len, "thirdparty") ||
        has_component(path_raw, path_len, "vendor") ||
        has_component(path_raw, path_len, "vendored") ||
        has_component(path_raw, path_len, "external") ||
        has_component(path_raw, path_len, "node_modules")) {
        add_role(out, ATLAS_CODE_ROLE_VENDORED, ATLAS_CODE_BASIS_PATH_NAMING,
                 ATLAS_CODE_RES_SOURCE_EXACT);
    }

    /* A test, by where it lives or what it is called. Both are path naming and
     * both say so. */
    if (has_component(path_raw, path_len, "test") || has_component(path_raw, path_len, "tests") ||
        starts_with(base, base_len, "test_") || starts_with(base, base_len, "Test")) {
        add_role(out, ATLAS_CODE_ROLE_TEST, ATLAS_CODE_BASIS_PATH_NAMING,
                 ATLAS_CODE_RES_SOURCE_EXACT);
    }

    atlas_code_language lang = atlas_code_language_of(path_raw, path_len);
    if (lang == ATLAS_CODE_LANG_C || lang == ATLAS_CODE_LANG_C_FRAGMENT) {
        add_role(out, ATLAS_CODE_ROLE_IMPLEMENTATION, ATLAS_CODE_BASIS_EXTENSION,
                 ATLAS_CODE_RES_SOURCE_EXACT);
    } else if (lang == ATLAS_CODE_LANG_C_HEADER) {
        /* Public versus private is a claim about intent, and the only evidence a
         * path carries for it is whether the header lives somewhere a consumer
         * would be pointed at. That is path naming and it is reported as such. */
        if (has_component(path_raw, path_len, "include") ||
            has_component(path_raw, path_len, "public") || has_component(path_raw, path_len, "api")) {
            add_role(out, ATLAS_CODE_ROLE_PUBLIC_HEADER, ATLAS_CODE_BASIS_PATH_NAMING,
                     ATLAS_CODE_RES_SOURCE_EXACT);
        } else {
            add_role(out, ATLAS_CODE_ROLE_PRIVATE_HEADER, ATLAS_CODE_BASIS_PATH_NAMING,
                     ATLAS_CODE_RES_SOURCE_EXACT);
        }
    }

    if (name_is(base, base_len, "CMakeLists.txt") || name_is(base, base_len, "Makefile") ||
        name_is(base, base_len, "GNUmakefile") || name_is(base, base_len, "meson.build") ||
        name_is(base, base_len, "build.ninja") || name_is(base, base_len, "Makefile.am") ||
        name_is(base, base_len, "configure.ac") ||
        name_is(base, base_len, "compile_commands.json") ||
        ends_with(base, base_len, ".cmake") || ends_with(base, base_len, ".mk")) {
        add_role(out, ATLAS_CODE_ROLE_BUILD_METADATA, ATLAS_CODE_BASIS_PATH_NAMING,
                 ATLAS_CODE_RES_SOURCE_EXACT);
    }

    if (ends_with(base, base_len, ".md") || ends_with(base, base_len, ".rst") ||
        ends_with(base, base_len, ".adoc") || ends_with(base, base_len, ".txt") ||
        starts_with(base, base_len, "README") || starts_with(base, base_len, "LICENSE") ||
        starts_with(base, base_len, "COPYING") || starts_with(base, base_len, "NOTICE") ||
        starts_with(base, base_len, "CHANGELOG")) {
        add_role(out, ATLAS_CODE_ROLE_DOCUMENTATION, ATLAS_CODE_BASIS_PATH_NAMING,
                 ATLAS_CODE_RES_SOURCE_EXACT);
    }

    /* Generated is the one role with content evidence, and it is the one worth
     * having: a path says nothing about whether a file was written by a person,
     * and a generated file is exactly the thing nobody should be editing. */
    if (prefix != NULL && prefix_len > 0 &&
        (prefix_contains_ci(prefix, prefix_len, "@generated") ||
         prefix_contains_ci(prefix, prefix_len, "do not edit") ||
         prefix_contains_ci(prefix, prefix_len, "automatically generated") ||
         prefix_contains_ci(prefix, prefix_len, "autogenerated") ||
         prefix_contains_ci(prefix, prefix_len, "auto-generated") ||
         prefix_contains_ci(prefix, prefix_len, "generated by"))) {
        add_role(out, ATLAS_CODE_ROLE_GENERATED, ATLAS_CODE_BASIS_CONTENT_MARKER,
                 ATLAS_CODE_RES_SOURCE_EXACT);
    }

    if (out->count == 0) {
        /* Not a silence. "Atlas looked and nothing matched" and "Atlas did not
         * look" are different facts, and only one of them is worth acting on. */
        add_role(out, ATLAS_CODE_ROLE_UNKNOWN, ATLAS_CODE_BASIS_NONE, ATLAS_CODE_RES_UNKNOWN);
    }
}

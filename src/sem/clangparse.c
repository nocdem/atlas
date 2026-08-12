/* Atlas - the libclang translation-unit reader (the child half).
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This file is the only place in Atlas that links against a compiler library,
 * and everything about how it is called is decided elsewhere. It receives a
 * source path and a vector of counted arguments that
 * `atlas_code_compdb_parse` already reduced to a positive allowlist — include
 * directories, defines and undefines, the standard, the explicit language — and
 * it passes exactly those to `clang_parseTranslationUnit2`.
 *
 * What that means, stated so it cannot quietly stop being true:
 *
 *   - **The `command` string from `compile_commands.json` never reaches here.**
 *     It is SHA-256'd and discarded by the compile-database reader. Nothing in
 *     this file executes anything: libclang is a library, there is no fork, no
 *     exec, no shell, and no compiler driver is invoked.
 *   - **A repository cannot choose code to run.** It can only contribute
 *     preprocessor state and search paths, from the allowlist. `-fplugin=`,
 *     `@response-files`, `-B`, `--config` and every other argument that could
 *     name something to load were dropped before this function was called, and
 *     `refuse_arg` below rejects them a second time — at the point of use,
 *     because the allowlist produces the better message and the check here is
 *     the guarantee. That is the same two-place shape A4 uses for the actor
 *     restriction.
 *   - **Only facts located inside the repository are described.** A symbol
 *     declared in a system header is recorded as an *external endpoint* — its
 *     USR and name, no location, no contents — because an edge needs a
 *     destination and a system header is not something Atlas indexes. This is
 *     also what bounds the output: without it, one translation unit emits every
 *     macro in libc.
 *
 * The identity is Clang's USR. Atlas does not invent a mangling: a USR already
 * distinguishes two files' `static void helper(void)`, two scopes' `i`, and a
 * struct from a typedef of the same name. A declaration and its definition
 * share a USR, which is correct — they are the same entity — so decl-versus-def
 * is a property of the row (`is_definition`) rather than of the identity, and
 * the definition/declaration relationship is answered by the rows that share a
 * USR. `ATLAS_SEM_EDGE_DECLARATION_OF` stays in the vocabulary and stays
 * unwritten, the way A3 keeps `symbol_contains_occurrence`.
 */
#include "atlas/sem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atlas/json.h"

#ifdef ATLAS_HAVE_LIBCLANG

#include <clang-c/Index.h>

/* --- a bounded string set, for within-unit deduplication --------------------
 *
 * One translation unit sees the same header many times over, and a header's
 * declarations would otherwise be emitted once per inclusion path. Open
 * addressing over offsets into one arena: no per-key allocation, and the whole
 * set is freed by freeing the arena. */

typedef struct strset {
    uint32_t *slots; /* arena offset + 1; 0 means empty */
    size_t cap;
    size_t count;
    atlas_buf arena;
    bool full; /* the ceiling was reached; further keys are treated as unseen */
} strset;

static void strset_init(strset *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->arena);
}

static void strset_free(strset *s) {
    free(s->slots);
    s->slots = NULL;
    atlas_buf_free(&s->arena);
}

static uint64_t str_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

static bool strset_grow(strset *s) {
    size_t ncap = s->cap == 0 ? 1024 : s->cap * 2;
    if (ncap > (1u << 22)) {
        s->full = true;
        return false;
    }
    uint32_t *ns = calloc(ncap, sizeof(*ns));
    if (ns == NULL) {
        s->full = true;
        return false;
    }
    for (size_t i = 0; i < s->cap; i++) {
        if (s->slots[i] == 0) {
            continue;
        }
        const char *key = (const char *)s->arena.data + (s->slots[i] - 1);
        size_t j = (size_t)(str_hash(key) & (ncap - 1));
        while (ns[j] != 0) {
            j = (j + 1) & (ncap - 1);
        }
        ns[j] = s->slots[i];
    }
    free(s->slots);
    s->slots = ns;
    s->cap = ncap;
    return true;
}

/* True when `key` was not present and has now been added. False when it was
 * already there — or when the set is full, in which case nothing is deduplicated
 * and a duplicate fact is emitted. Emitting a duplicate is the safe failure: the
 * database applies facts idempotently, so the cost is bytes rather than a
 * silently missing symbol. */
static bool strset_add(strset *s, const char *key) {
    if (s->full) {
        return true;
    }
    if (s->count * 2 >= s->cap && !strset_grow(s)) {
        return true;
    }
    size_t j = (size_t)(str_hash(key) & (s->cap - 1));
    while (s->slots[j] != 0) {
        const char *have = (const char *)s->arena.data + (s->slots[j] - 1);
        if (strcmp(have, key) == 0) {
            return false;
        }
        j = (j + 1) & (s->cap - 1);
    }
    size_t off = s->arena.len;
    atlas_err ignored;
    atlas_err_init(&ignored);
    if (atlas_buf_append(&s->arena, key, strlen(key) + 1, &ignored) != ATLAS_OK) {
        s->full = true;
        return true;
    }
    s->slots[j] = (uint32_t)(off + 1);
    s->count++;
    return true;
}

/* --- parse state ----------------------------------------------------------- */

typedef struct pstate {
    atlas_json *j;
    const char *root;
    size_t root_len;
    strset sym_seen;
    strset edge_seen;
    strset inc_seen;
    strset ext_seen;
    int64_t symbols;
    int64_t edges;
    int64_t includes;
    int64_t max_facts;
    bool truncated;
    atlas_status st;
    atlas_err *err;
    atlas_buf key; /* reused scratch for dedup keys */
} pstate;

static bool budget_left(pstate *ps) {
    if (ps->st != ATLAS_OK) {
        return false;
    }
    if (ps->symbols + ps->edges + ps->includes >= ps->max_facts) {
        ps->truncated = true;
        return false;
    }
    return true;
}

/* Every emission goes through here, so a write failure stops the walk exactly
 * once rather than being checked at forty call sites. */
static void emit_begin(pstate *ps, const char *record) {
    if (ps->st != ATLAS_OK) {
        return;
    }
    ps->st = atlas_json_obj_begin(ps->j, ps->err);
    if (ps->st == ATLAS_OK) {
        ps->st = atlas_json_key_str(ps->j, "r", record, ps->err);
    }
}

static void emit_end(pstate *ps) {
    if (ps->st != ATLAS_OK) {
        return;
    }
    ps->st = atlas_json_obj_end(ps->j, ps->err);
}

static void emit_str(pstate *ps, const char *k, const char *v) {
    if (ps->st != ATLAS_OK || v == NULL || v[0] == '\0') {
        return;
    }
    ps->st = atlas_json_key_str(ps->j, k, v, ps->err);
}

static void emit_int(pstate *ps, const char *k, int64_t v) {
    if (ps->st != ATLAS_OK || v == 0) {
        return;
    }
    ps->st = atlas_json_key_int(ps->j, k, v, ps->err);
}

static void emit_bool(pstate *ps, const char *k, bool v) {
    if (ps->st != ATLAS_OK || !v) {
        return;
    }
    ps->st = atlas_json_key_bool(ps->j, k, v, ps->err);
}

/* --- Clang helpers ---------------------------------------------------------- */

/* A CXString copied into a caller buffer and disposed. Bounded on purpose: a
 * USR from a pathological nesting is truncated to the recorded ceiling and the
 * truncation is visible, rather than being allowed to grow without limit. */
static void cxstr(CXString s, char *out, size_t cap) {
    const char *c = clang_getCString(s);
    if (c == NULL) {
        c = "";
    }
    size_t n = strlen(c);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, c, n);
    out[n] = '\0';
    clang_disposeString(s);
}

/* Where a cursor is, and whether Atlas describes that place.
 *
 * Returns true when the location is inside the repository root, in which case
 * `rel` receives the repository-relative path. A cursor with no file (the
 * predefines buffer, a builtin) and a cursor in a system or vendored header
 * outside the root both return false — the caller then records the entity as an
 * external endpoint rather than as a located symbol. */
static bool loc_in_root(pstate *ps, CXSourceLocation loc, char *rel, size_t relcap, int64_t *line,
                        int64_t *col) {
    CXFile file = NULL;
    unsigned l = 0, c = 0, off = 0;
    clang_getFileLocation(loc, &file, &l, &c, &off);
    if (file == NULL) {
        return false;
    }
    char path[4096];
    cxstr(clang_getFileName(file), path, sizeof(path));
    if (path[0] == '\0') {
        return false;
    }
    size_t n = strlen(path);
    if (n <= ps->root_len || strncmp(path, ps->root, ps->root_len) != 0 ||
        path[ps->root_len] != '/') {
        return false;
    }
    const char *r = path + ps->root_len + 1;
    size_t rn = strlen(r);
    if (rn >= relcap) {
        return false;
    }
    memcpy(rel, r, rn + 1);
    if (line != NULL) {
        *line = (int64_t)l;
    }
    if (col != NULL) {
        *col = (int64_t)c;
    }
    return true;
}

/* An if-chain rather than a switch, deliberately.
 *
 * Atlas builds with `-Werror=switch-enum`, which requires every member of an
 * enumeration to be named. That rule is valuable for Atlas' own closed
 * vocabularies — it is what makes adding a state a compile error at every place
 * that must consider it — and unworkable against `CXCursorKind`, which has
 * several hundred members Atlas has no opinion about and which grows with every
 * Clang release. Suppressing the warning for this file would weaken it for the
 * Atlas enums in the same file, so the mapping is written as comparisons and
 * the closed-vocabulary rule keeps its full strength everywhere it belongs.
 *
 * The C++ and Objective-C members are the ones deliberately absent: Atlas
 * indexes C. Anything not named here is UNKNOWN, which is the honest answer and
 * not a silent omission — an UNKNOWN symbol kind is simply not recorded. */
static atlas_sem_symbol_kind kind_of(enum CXCursorKind k, CXCursor c) {
    if (k == CXCursor_FunctionDecl) {
        return ATLAS_SEM_SYM_FUNCTION;
    }
    if (k == CXCursor_StructDecl) {
        return ATLAS_SEM_SYM_STRUCT;
    }
    if (k == CXCursor_UnionDecl) {
        return ATLAS_SEM_SYM_UNION;
    }
    if (k == CXCursor_EnumDecl) {
        return ATLAS_SEM_SYM_ENUM;
    }
    if (k == CXCursor_EnumConstantDecl) {
        return ATLAS_SEM_SYM_ENUM_CONSTANT;
    }
    if (k == CXCursor_TypedefDecl) {
        return ATLAS_SEM_SYM_TYPEDEF;
    }
    if (k == CXCursor_FieldDecl) {
        return ATLAS_SEM_SYM_FIELD;
    }
    if (k == CXCursor_MacroDefinition) {
        return ATLAS_SEM_SYM_MACRO;
    }
    if (k == CXCursor_ParmDecl) {
        return ATLAS_SEM_SYM_PARAMETER;
    }
    if (k == CXCursor_VarDecl) {
        /* Only file-scope variables are symbols. A local is a fact about one
         * function body and indexing every one of them would multiply the graph
         * without answering a question anybody asks of it. */
        CXCursor sp = clang_getCursorSemanticParent(c);
        return clang_getCursorKind(sp) == CXCursor_TranslationUnit ? ATLAS_SEM_SYM_VARIABLE
                                                                  : ATLAS_SEM_SYM_UNKNOWN;
    }
    return ATLAS_SEM_SYM_UNKNOWN;
}

static atlas_sem_linkage linkage_of(CXCursor c) {
    enum CXLinkageKind l = clang_getCursorLinkage(c);
    if (l == CXLinkage_External || l == CXLinkage_UniqueExternal) {
        return ATLAS_SEM_LINK_EXTERNAL;
    }
    if (l == CXLinkage_Internal) {
        return ATLAS_SEM_LINK_INTERNAL;
    }
    if (l == CXLinkage_NoLinkage) {
        return ATLAS_SEM_LINK_NONE;
    }
    return ATLAS_SEM_LINK_UNKNOWN;
}

/* Records an entity Atlas does not describe — a libc function, a type from a
 * system header — so an edge into it has a destination. Name and USR only: no
 * location is claimed, because Atlas did not index the file it lives in. */
static void emit_external(pstate *ps, const char *usr, const char *name,
                          atlas_sem_symbol_kind kind) {
    if (usr[0] == '\0' || !budget_left(ps)) {
        return;
    }
    if (!strset_add(&ps->ext_seen, usr)) {
        return;
    }
    emit_begin(ps, "s");
    emit_str(ps, "usr", usr);
    emit_str(ps, "name", name);
    emit_str(ps, "kind", atlas_sem_symbol_kind_name(kind));
    emit_bool(ps, "ext", true);
    emit_end(ps);
    ps->symbols++;
}

/* The USR of a cursor, plus whether Atlas has a located row for it. When the
 * cursor lives outside the repository an external endpoint is emitted as a side
 * effect, which is why this is one function rather than two. */
static void endpoint(pstate *ps, CXCursor ref, char *usr, size_t usrcap) {
    usr[0] = '\0';
    if (clang_Cursor_isNull(ref)) {
        return;
    }
    cxstr(clang_getCursorUSR(ref), usr, usrcap);
    if (usr[0] == '\0') {
        return;
    }
    char rel[4096];
    if (!loc_in_root(ps, clang_getCursorLocation(ref), rel, sizeof(rel), NULL, NULL)) {
        char name[ATLAS_SEM_MAX_NAME_BYTES];
        cxstr(clang_getCursorSpelling(ref), name, sizeof(name));
        emit_external(ps, usr, name, kind_of(clang_getCursorKind(ref), ref));
    }
}

static void emit_edge(pstate *ps, atlas_sem_edge_kind kind, const char *src_usr,
                      const char *dst_usr, atlas_sem_evidence ev, const char *file, int64_t line,
                      int64_t col, const char *detail) {
    if (!budget_left(ps)) {
        return;
    }
    /* The single authority on what an edge kind may claim. A MAY_CALL that
     * arrived here asking for PROVEN is capped, not trusted — the extractor is
     * not the place that decides what the word means. */
    atlas_sem_evidence cap = atlas_sem_edge_kind_max_evidence(kind);
    ev = atlas_sem_evidence_weaker(ev, cap);

    /* One edge per (kind, src, dst, site). The same call inside a macro used
     * twice on one line is one fact, not two. */
    atlas_err ignored;
    atlas_err_init(&ignored);
    ps->key.len = 0;
    char head[64];
    (void)snprintf(head, sizeof(head), "%d|%lld|%lld|", (int)kind, (long long)line, (long long)col);
    if (atlas_buf_append(&ps->key, head, strlen(head), &ignored) != ATLAS_OK ||
        atlas_buf_append(&ps->key, src_usr, strlen(src_usr), &ignored) != ATLAS_OK ||
        atlas_buf_append(&ps->key, "|", 1, &ignored) != ATLAS_OK ||
        atlas_buf_append(&ps->key, dst_usr, strlen(dst_usr) + 1, &ignored) != ATLAS_OK) {
        return;
    }
    if (!strset_add(&ps->edge_seen, (const char *)ps->key.data)) {
        return;
    }

    emit_begin(ps, "e");
    emit_str(ps, "kind", atlas_sem_edge_kind_name(kind));
    emit_str(ps, "src", src_usr);
    emit_str(ps, "dst", dst_usr);
    emit_str(ps, "ev", atlas_sem_evidence_name(ev));
    emit_str(ps, "file", file);
    emit_int(ps, "line", line);
    emit_int(ps, "col", col);
    emit_str(ps, "detail", detail);
    emit_end(ps);
    ps->edges++;
}

/* The canonical prototype of what an indirect call goes through.
 *
 * This is the whole basis for a candidate set: two functions are possible
 * targets of one call site when the site's pointee prototype and the function's
 * own prototype are the same canonical type. Recorded as text and matched later,
 * across translation units, by the indexer — the parser sees one unit and
 * cannot know what else in the repository has a matching prototype. */
static void proto_of(CXType t, char *out, size_t cap) {
    out[0] = '\0';
    CXType cur = clang_getCanonicalType(t);
    if (cur.kind == CXType_Pointer) {
        cur = clang_getCanonicalType(clang_getPointeeType(cur));
    }
    if (cur.kind == CXType_FunctionProto || cur.kind == CXType_FunctionNoProto) {
        cxstr(clang_getTypeSpelling(cur), out, cap);
    }
}

typedef struct visit_ctx {
    pstate *ps;
    const char *enclosing; /* USR of the function whose body we are inside, or "" */
    /* Armed while descending a call expression whose callee Clang named. The
     * first reference to that callee is the callee itself and is not an address
     * being taken; anything after it is. Without this, every ordinary call would
     * also record its target as address-taken, and every function in the
     * repository would become a candidate for every indirect call — a candidate
     * set that large is indistinguishable from no information at all. */
    const char *skip_callee;
    bool *skip_used;
} visit_ctx;

static enum CXChildVisitResult on_child(CXCursor c, CXCursor parent, CXClientData data);

static void descend(CXCursor c, pstate *ps, const char *enclosing, const char *skip_callee,
                    bool *skip_used) {
    visit_ctx ctx;
    ctx.ps = ps;
    ctx.enclosing = enclosing;
    ctx.skip_callee = skip_callee;
    ctx.skip_used = skip_used;
    clang_visitChildren(c, on_child, &ctx);
}

/* Emits the located symbol row for a declaration cursor, and the type edges
 * that belong to it. */
static void handle_decl(pstate *ps, CXCursor c, atlas_sem_symbol_kind kind, const char *usr,
                        const char *rel, int64_t line, int64_t col) {
    if (!budget_left(ps)) {
        return;
    }
    char name[ATLAS_SEM_MAX_NAME_BYTES];
    cxstr(clang_getCursorSpelling(c), name, sizeof(name));

    bool is_def = clang_isCursorDefinition(c) != 0;

    /* One row per (usr, file, line, definition-ness). A declaration repeated by
     * every inclusion of its header is one row; a declaration and a definition
     * in different places are two, which is exactly the pair a caller asks for
     * when it wants "where is this declared and where is it defined". */
    atlas_err ignored;
    atlas_err_init(&ignored);
    ps->key.len = 0;
    char head[64];
    (void)snprintf(head, sizeof(head), "%lld|%d|", (long long)line, is_def ? 1 : 0);
    if (atlas_buf_append(&ps->key, head, strlen(head), &ignored) != ATLAS_OK ||
        atlas_buf_append(&ps->key, rel, strlen(rel), &ignored) != ATLAS_OK ||
        atlas_buf_append(&ps->key, "|", 1, &ignored) != ATLAS_OK ||
        atlas_buf_append(&ps->key, usr, strlen(usr) + 1, &ignored) != ATLAS_OK) {
        return;
    }
    if (!strset_add(&ps->sym_seen, (const char *)ps->key.data)) {
        return;
    }

    char type_text[ATLAS_SEM_MAX_TYPE_BYTES];
    cxstr(clang_getTypeSpelling(clang_getCursorType(c)), type_text, sizeof(type_text));

    int64_t end_line = 0;
    {
        CXSourceRange range = clang_getCursorExtent(c);
        CXFile f = NULL;
        unsigned l = 0, cc = 0, off = 0;
        clang_getFileLocation(clang_getRangeEnd(range), &f, &l, &cc, &off);
        end_line = (int64_t)l;
    }

    emit_begin(ps, "s");
    emit_str(ps, "usr", usr);
    emit_str(ps, "name", name);
    emit_str(ps, "kind", atlas_sem_symbol_kind_name(kind));
    emit_str(ps, "link", atlas_sem_linkage_name(linkage_of(c)));
    emit_str(ps, "type", type_text);
    emit_str(ps, "file", rel);
    emit_int(ps, "line", line);
    emit_int(ps, "col", col);
    emit_int(ps, "end", end_line);
    emit_bool(ps, "def", is_def);
    emit_end(ps);
    ps->symbols++;

    /* Type structure. Every one of these is PROVEN: the compiler resolved the
     * type, and Atlas is recording what it resolved to. */
    if (kind == ATLAS_SEM_SYM_FUNCTION) {
        char dst[ATLAS_SEM_MAX_USR_BYTES];
        endpoint(ps, clang_getTypeDeclaration(clang_getCanonicalType(
                         clang_getResultType(clang_getCursorType(c)))),
                 dst, sizeof(dst));
        if (dst[0] != '\0') {
            emit_edge(ps, ATLAS_SEM_EDGE_RETURN_TYPE, usr, dst, ATLAS_SEM_EV_PROVEN, rel, line, col,
                      NULL);
        }
        int n = clang_Cursor_getNumArguments(c);
        for (int i = 0; i < n && i < 64; i++) {
            CXCursor arg = clang_Cursor_getArgument(c, (unsigned)i);
            endpoint(ps, clang_getTypeDeclaration(clang_getCanonicalType(clang_getCursorType(arg))),
                     dst, sizeof(dst));
            if (dst[0] != '\0') {
                emit_edge(ps, ATLAS_SEM_EDGE_PARAM_TYPE, usr, dst, ATLAS_SEM_EV_PROVEN, rel, line,
                          col, NULL);
            }
        }
    } else if (kind == ATLAS_SEM_SYM_FIELD || kind == ATLAS_SEM_SYM_TYPEDEF ||
               kind == ATLAS_SEM_SYM_VARIABLE) {
        char dst[ATLAS_SEM_MAX_USR_BYTES];
        CXType t = kind == ATLAS_SEM_SYM_TYPEDEF ? clang_getTypedefDeclUnderlyingType(c)
                                                 : clang_getCursorType(c);
        endpoint(ps, clang_getTypeDeclaration(clang_getCanonicalType(t)), dst, sizeof(dst));
        if (dst[0] != '\0') {
            emit_edge(ps, ATLAS_SEM_EDGE_HAS_TYPE, usr, dst, ATLAS_SEM_EV_PROVEN, rel, line, col,
                      NULL);
        }
        if (kind == ATLAS_SEM_SYM_FIELD) {
            char owner[ATLAS_SEM_MAX_USR_BYTES];
            endpoint(ps, clang_getCursorSemanticParent(c), owner, sizeof(owner));
            if (owner[0] != '\0') {
                emit_edge(ps, ATLAS_SEM_EDGE_HAS_FIELD, owner, usr, ATLAS_SEM_EV_PROVEN, rel, line,
                          col, NULL);
            }
        }
    }
}

static enum CXChildVisitResult on_child(CXCursor c, CXCursor parent, CXClientData data) {
    visit_ctx *ctx = (visit_ctx *)data;
    pstate *ps = ctx->ps;
    (void)parent;

    if (ps->st != ATLAS_OK) {
        return CXChildVisit_Break;
    }
    if (ps->symbols + ps->edges + ps->includes >= ps->max_facts) {
        ps->truncated = true;
        return CXChildVisit_Break;
    }

    enum CXCursorKind ck = clang_getCursorKind(c);
    char rel[4096];
    int64_t line = 0, col = 0;
    bool located = loc_in_root(ps, clang_getCursorLocation(c), rel, sizeof(rel), &line, &col);

    /* An inclusion directive is a fact about the including file, which is the
     * one Atlas indexes. Where it leads may well be outside the repository, and
     * that is recorded as a spelling with no target rather than dropped. */
    if (ck == CXCursor_InclusionDirective) {
        if (located && budget_left(ps)) {
            char spelling[1024];
            cxstr(clang_getCursorSpelling(c), spelling, sizeof(spelling));
            CXFile inc = clang_getIncludedFile(c);
            char to[4096];
            bool to_in_root = false;
            if (inc != NULL) {
                char abs[4096];
                cxstr(clang_getFileName(inc), abs, sizeof(abs));
                size_t n = strlen(abs);
                if (n > ps->root_len && strncmp(abs, ps->root, ps->root_len) == 0 &&
                    abs[ps->root_len] == '/') {
                    (void)snprintf(to, sizeof(to), "%s", abs + ps->root_len + 1);
                    to_in_root = true;
                }
            }
            atlas_err ignored;
            atlas_err_init(&ignored);
            ps->key.len = 0;
            if (atlas_buf_append(&ps->key, rel, strlen(rel), &ignored) == ATLAS_OK &&
                atlas_buf_append(&ps->key, "|", 1, &ignored) == ATLAS_OK &&
                atlas_buf_append(&ps->key, to_in_root ? to : spelling,
                                 strlen(to_in_root ? to : spelling) + 1, &ignored) == ATLAS_OK &&
                strset_add(&ps->inc_seen, (const char *)ps->key.data)) {
                emit_begin(ps, "i");
                emit_str(ps, "from", rel);
                emit_str(ps, "to", to_in_root ? to : "");
                emit_str(ps, "spell", spelling);
                emit_int(ps, "line", line);
                /* PROVEN: the preprocessor opened this file. An include Atlas
                 * placed outside the repository keeps its spelling and gets no
                 * target, which is UNKNOWN about the destination and PROVEN
                 * about the directive. */
                emit_str(ps, "ev",
                         atlas_sem_evidence_name(to_in_root ? ATLAS_SEM_EV_PROVEN
                                                            : ATLAS_SEM_EV_UNKNOWN));
                emit_end(ps);
                ps->includes++;
            }
        }
        return CXChildVisit_Continue;
    }

    char usr[ATLAS_SEM_MAX_USR_BYTES];
    usr[0] = '\0';

    atlas_sem_symbol_kind kind = kind_of(ck, c);
    if (kind != ATLAS_SEM_SYM_UNKNOWN && located) {
        cxstr(clang_getCursorUSR(c), usr, sizeof(usr));
        if (usr[0] != '\0' && kind != ATLAS_SEM_SYM_PARAMETER) {
            handle_decl(ps, c, kind, usr, rel, line, col);
        }
    }

    /* Inside a function body, edges are attributed to that function. */
    const char *enclosing = ctx->enclosing;
    if (ck == CXCursor_FunctionDecl && clang_isCursorDefinition(c) && usr[0] != '\0') {
        enclosing = usr;
        /* A fresh skip state per function body. */
        bool used = true;
        descend(c, ps, enclosing, "", &used);
        return CXChildVisit_Continue;
    }

    if (ck == CXCursor_CallExpr) {
        CXCursor ref = clang_getCursorReferenced(c);
        char dst[ATLAS_SEM_MAX_USR_BYTES];
        endpoint(ps, ref, dst, sizeof(dst));
        bool direct = !clang_Cursor_isNull(ref) &&
                      clang_getCursorKind(ref) == CXCursor_FunctionDecl && dst[0] != '\0';
        char site[4096];
        int64_t sline = line, scol = col;
        if (!located) {
            site[0] = '\0';
            sline = 0;
            scol = 0;
        } else {
            (void)snprintf(site, sizeof(site), "%s", rel);
        }

        if (direct) {
            emit_edge(ps, ATLAS_SEM_EDGE_CALLS, enclosing, dst, ATLAS_SEM_EV_PROVEN, site, sline,
                      scol, NULL);
            bool used = false;
            descend(c, ps, enclosing, dst, &used);
        } else {
            /* An indirect call. The edge is recorded with no destination and
             * UNKNOWN evidence; the indexer may later attach candidate targets
             * by matching this prototype against the functions whose address
             * was taken. It never becomes PROVEN — `emit_edge` caps it. */
            char proto[ATLAS_SEM_MAX_TYPE_BYTES];
            proto_of(clang_getCursorType(c), proto, sizeof(proto));
            if (proto[0] == '\0') {
                /* Fall back to the callee expression's own type. */
                CXCursor callee = clang_getCursorReferenced(c);
                proto_of(clang_getCursorType(callee), proto, sizeof(proto));
            }
            emit_edge(ps, ATLAS_SEM_EDGE_MAY_CALL, enclosing, "", ATLAS_SEM_EV_UNKNOWN, site, sline,
                      scol, proto);
            bool used = true;
            descend(c, ps, enclosing, "", &used);
        }
        return CXChildVisit_Continue;
    }

    if (ck == CXCursor_DeclRefExpr || ck == CXCursor_MemberRefExpr || ck == CXCursor_TypeRef) {
        CXCursor ref = clang_getCursorReferenced(c);
        if (!clang_Cursor_isNull(ref)) {
            char dst[ATLAS_SEM_MAX_USR_BYTES];
            endpoint(ps, ref, dst, sizeof(dst));
            if (dst[0] != '\0') {
                bool is_func = clang_getCursorKind(ref) == CXCursor_FunctionDecl;
                bool is_callee = is_func && ctx->skip_callee != NULL &&
                                 ctx->skip_used != NULL && !*ctx->skip_used &&
                                 strcmp(ctx->skip_callee, dst) == 0;
                if (is_callee) {
                    *ctx->skip_used = true;
                } else if (is_func) {
                    char proto[ATLAS_SEM_MAX_TYPE_BYTES];
                    proto_of(clang_getCursorType(ref), proto, sizeof(proto));
                    emit_edge(ps, ATLAS_SEM_EDGE_ADDRESS_TAKEN, enclosing, dst,
                              ATLAS_SEM_EV_PROVEN, located ? rel : "", located ? line : 0,
                              located ? col : 0, proto);
                } else {
                    emit_edge(ps, ATLAS_SEM_EDGE_REFERENCES, enclosing, dst, ATLAS_SEM_EV_PROVEN,
                              located ? rel : "", located ? line : 0, located ? col : 0, NULL);
                }
            }
        }
    }

    descend(c, ps, enclosing, ctx->skip_callee, ctx->skip_used);
    return CXChildVisit_Continue;
}

/* --- refused arguments ------------------------------------------------------
 *
 * The compile-database reader already dropped everything not on its positive
 * allowlist, so nothing on this list should ever arrive. It is checked anyway,
 * at the point of use, because that is where the guarantee has to hold: A4
 * checks the actor restriction at the IPC edge *and* at the write point for
 * exactly this reason, and a second reader of this file should be able to see
 * that a plugin cannot be loaded without having to go and read another one. */
static bool refuse_arg(const char *a) {
    static const char *const PREFIXES[] = {
        "-fplugin", "-load", "-Xclang", "-B", "--config", "-fmodules", "-fprebuilt-module-path",
        "-gen-reproducer", "-fprofile", "-specs", "--sysroot=/", "-idirafter/",
    };
    if (a == NULL || a[0] == '\0') {
        return true;
    }
    /* A response file names something to read and expand, and Atlas expands
     * nothing. */
    if (a[0] == '@') {
        return true;
    }
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        if (strncmp(a, PREFIXES[i], strlen(PREFIXES[i])) == 0) {
            return true;
        }
    }
    /* An embedded NUL cannot occur in a C string, but an argument longer than
     * the recorded ceiling is a bound being reached and is refused rather than
     * truncated: a truncated `-I` names a different directory. */
    if (strlen(a) > ATLAS_CODE_MAX_ARG_BYTES) {
        return true;
    }
    return false;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

bool atlas_sem_available(void) { return true; }

const char *atlas_sem_compiler_id(void) { return "clang"; }

const char *atlas_sem_compiler_version(void) {
    static char buf[96];
    if (buf[0] == '\0') {
        cxstr(clang_getClangVersion(), buf, sizeof(buf));
        if (buf[0] == '\0') {
            (void)snprintf(buf, sizeof(buf), "unknown");
        }
    }
    return buf;
}

atlas_status atlas_sem_parse_here(const atlas_sem_parse_req *req, atlas_buf *out,
                                  atlas_sem_parse_result *res, atlas_err *err) {
    if (req == NULL || out == NULL || res == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic parse: bad request");
    }
    memset(res, 0, sizeof(*res));
    res->status = ATLAS_SEM_TU_UNKNOWN;

    for (size_t i = 0; i < req->arg_count; i++) {
        if (refuse_arg(req->args[i])) {
            res->status = ATLAS_SEM_TU_UNSUPPORTED;
            res->why = ATLAS_SEM_WHY_ARG_REFUSED;
            return ATLAS_OK;
        }
    }

    double t0 = now_ms();

    /* The document is built with Atlas' own streaming writer, never with
     * yyjson — A2's rule, unchanged. `open_memstream` gives the writer a FILE*
     * backed by memory so the child can be exercised directly by a test without
     * a process being created. */
    char *mem = NULL;
    size_t memlen = 0;
    FILE *fp = open_memstream(&mem, &memlen);
    if (fp == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic parse: cannot open output stream");
    }

    atlas_json *j = atlas_json_new(fp, err);
    if (j == NULL) {
        (void)fclose(fp);
        free(mem);
        return err != NULL && err->status != ATLAS_OK ? err->status : ATLAS_ERR_INTERNAL;
    }

    pstate ps;
    memset(&ps, 0, sizeof(ps));
    ps.j = j;
    ps.root = req->root;
    ps.root_len = req->root == NULL ? 0 : strlen(req->root);
    ps.max_facts = req->max_facts > 0 ? req->max_facts : ATLAS_SEM_MAX_FACTS_PER_UNIT;
    ps.st = ATLAS_OK;
    ps.err = err;
    strset_init(&ps.sym_seen);
    strset_init(&ps.edge_seen);
    strset_init(&ps.inc_seen);
    strset_init(&ps.ext_seen);
    atlas_buf_init(&ps.key);

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = NULL;
    enum CXErrorCode ec = CXError_Failure;
    if (index != NULL) {
        ec = clang_parseTranslationUnit2(index, req->source, req->args, (int)req->arg_count, NULL,
                                         0, CXTranslationUnit_DetailedPreprocessingRecord, &tu);
    }

    ps.st = atlas_json_obj_begin(j, err);
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key(j, "facts", err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_arr_begin(j, err);
    }

    int64_t errors = 0;
    if (ec != CXError_Success || tu == NULL) {
        res->status = ATLAS_SEM_TU_FAILED;
        res->why = ATLAS_SEM_WHY_NO_TU;
    } else {
        unsigned nd = clang_getNumDiagnostics(tu);
        for (unsigned i = 0; i < nd; i++) {
            CXDiagnostic dg = clang_getDiagnostic(tu, i);
            /* Diagnostics are **counted, never reproduced**. A diagnostic quotes
             * the source, and repository source is untrusted text that must not
             * reach a model or a terminal through a channel that looks like
             * Atlas' own reporting. The count is the fact; the text is not
             * Atlas' to relay. */
            if (clang_getDiagnosticSeverity(dg) >= CXDiagnostic_Error) {
                errors++;
            }
            clang_disposeDiagnostic(dg);
        }
        if (ps.st == ATLAS_OK) {
            descend(clang_getTranslationUnitCursor(tu), &ps, "", "", NULL);
        }
        res->status = errors > 0 ? ATLAS_SEM_TU_PARTIAL : ATLAS_SEM_TU_COMPLETE;
        res->why = errors > 0 ? ATLAS_SEM_WHY_PARSE_ERROR : NULL;
    }

    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_arr_end(j, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key_int(j, "symbols", ps.symbols, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key_int(j, "edges", ps.edges, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key_int(j, "includes", ps.includes, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key_int(j, "errors", errors, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key_bool(j, "truncated", ps.truncated, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_key_str(j, "status", atlas_sem_tu_status_name(res->status), err);
    }
    if (ps.st == ATLAS_OK && res->why != NULL) {
        ps.st = atlas_json_key_str(j, "why", res->why, err);
    }
    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_obj_end(j, err);
    }

    if (ps.st == ATLAS_OK) {
        ps.st = atlas_json_finish(j, err);
    } else {
        atlas_json_free(j);
    }

    if (tu != NULL) {
        clang_disposeTranslationUnit(tu);
    }
    if (index != NULL) {
        clang_disposeIndex(index);
    }

    (void)fclose(fp);

    atlas_status st = ps.st;
    if (st == ATLAS_OK && mem != NULL) {
        st = atlas_buf_append(out, mem, memlen, err);
    }
    free(mem);

    strset_free(&ps.sym_seen);
    strset_free(&ps.edge_seen);
    strset_free(&ps.inc_seen);
    strset_free(&ps.ext_seen);
    atlas_buf_free(&ps.key);

    res->symbols = ps.symbols;
    res->edges = ps.edges;
    res->includes = ps.includes;
    res->diagnostics_errors = errors;
    res->truncated = ps.truncated;
    res->duration_ms = (int64_t)(now_ms() - t0);
    if (ps.truncated) {
        res->why = ATLAS_SEM_WHY_TOO_LARGE;
    }
    return st;
}

#else /* !ATLAS_HAVE_LIBCLANG */

/* Built without libclang.
 *
 * Atlas reports the absence rather than pretending an empty index is an answer:
 * every semantic entry point asks `atlas_sem_available()` first and returns a
 * typed refusal, and `code status` says so. A silent empty graph would be
 * indistinguishable from a repository with no code in it, which is exactly the
 * conflation this layer exists to prevent. */

bool atlas_sem_available(void) { return false; }
const char *atlas_sem_compiler_id(void) { return "none"; }
const char *atlas_sem_compiler_version(void) { return ""; }

atlas_status atlas_sem_parse_here(const atlas_sem_parse_req *req, atlas_buf *out,
                                  atlas_sem_parse_result *res, atlas_err *err) {
    (void)req;
    (void)out;
    if (res != NULL) {
        memset(res, 0, sizeof(*res));
        res->status = ATLAS_SEM_TU_UNSUPPORTED;
        res->why = ATLAS_SEM_WHY_ARG_REFUSED;
    }
    return atlas_err_set(err, ATLAS_ERR_CONFIG,
                         "this Atlas was built without libclang; semantic indexing is unavailable");
}

#endif /* ATLAS_HAVE_LIBCLANG */

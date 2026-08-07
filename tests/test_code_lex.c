/* Atlas - the bounded lexical C indexer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The extractor is where A3's honesty is either real or decorative, so this
 * suite is written around the places where code-shaped bytes are not code:
 * comments, literals, escaped newlines, preprocessor replacement text and
 * conditionals. Every one of those has a case here that would produce a
 * plausible false fact if it were handled naively.
 *
 * It is a pure unit suite: no repository, no database, no daemon. The extractor
 * takes bytes and returns a bounded object, which is exactly what makes it safe
 * to run on a worker thread and exactly what makes it cheap to test.
 */
#include "atlas/code.h"

#include <stdlib.h>
#include <string.h>

#include "atlas_test.h"

/* --- helpers --------------------------------------------------------------- */

static void run(const char *src, atlas_code_language lang, atlas_code_parse *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_code_extract(src, strlen(src), lang, out, &err), &err);
}

static void run_c(const char *src, atlas_code_parse *out) {
    run(src, ATLAS_CODE_LANG_C, out);
}

/* Finds a symbol by name and kind. Returns NULL when there is none, so a test
 * can assert absence as easily as presence — which matters more here than
 * usual, because most of these cases are about what must *not* be recorded. */
static const atlas_code_symbol_item *find_sym(const atlas_code_parse *p, const char *name,
                                              atlas_code_symbol_kind kind) {
    for (size_t i = 0; i < p->symbol_count; i++) {
        const atlas_code_symbol_item *s = &p->symbols[i];
        if (s->kind == (int32_t)kind && strcmp(atlas_code_parse_name(p, s->name_off), name) == 0) {
            return s;
        }
    }
    return NULL;
}

static const atlas_code_symbol_item *find_any(const atlas_code_parse *p, const char *name) {
    for (size_t i = 0; i < p->symbol_count; i++) {
        if (strcmp(atlas_code_parse_name(p, p->symbols[i].name_off), name) == 0) {
            return &p->symbols[i];
        }
    }
    return NULL;
}

static bool has_occurrence(const atlas_code_parse *p, const char *name) {
    for (size_t i = 0; i < p->occurrence_count; i++) {
        if (strcmp(atlas_code_parse_name(p, p->occurrences[i].name_off), name) == 0) {
            return true;
        }
    }
    return false;
}

static const atlas_code_include_item *find_include(const atlas_code_parse *p, const char *spell) {
    for (size_t i = 0; i < p->include_count; i++) {
        if (strcmp(atlas_code_parse_name(p, p->includes[i].spelling_off), spell) == 0) {
            return &p->includes[i];
        }
    }
    return NULL;
}

/* --- includes -------------------------------------------------------------- */

static void test_includes(void) {
    atlas_code_parse p;
    run_c("#include \"local.h\"\n"
          "#include <stdio.h>\n"
          "#include \"a/b/c.h\"\n"
          "  #  include   <sys/types.h>\n",
          &p);
    T_EQ_INT(p.include_count, 4);
    const atlas_code_include_item *i = find_include(&p, "local.h");
    T_REQUIRE(i != NULL);
    T_EQ_INT(i->form, ATLAS_CODE_INCLUDE_QUOTE);
    T_EQ_INT(i->resolution, ATLAS_CODE_RES_SOURCE_EXACT);
    T_EQ_INT(i->line, 1);

    i = find_include(&p, "stdio.h");
    T_REQUIRE(i != NULL);
    T_EQ_INT(i->form, ATLAS_CODE_INCLUDE_ANGLE);
    T_CHECK(find_include(&p, "a/b/c.h") != NULL);
    /* An angle include with a path, and leading whitespace before and after the
     * `#`, which is legal and which a line-anchored regex would miss. */
    T_CHECK(find_include(&p, "sys/types.h") != NULL);
    atlas_code_parse_free(&p);
}

static void test_include_in_comment_or_string(void) {
    atlas_code_parse p;
    /* Every one of these looks exactly like an include to anything that scans
     * for the bytes rather than lexing them. */
    run_c("/* #include \"fake.h\" */\n"
          "// #include \"also-fake.h\"\n"
          "const char *s = \"#include \\\"string-fake.h\\\"\";\n"
          "#include \"real.h\"\n",
          &p);
    T_EQ_INT(p.include_count, 1);
    T_CHECK(find_include(&p, "real.h") != NULL);
    T_CHECK(find_include(&p, "fake.h") == NULL);
    T_CHECK(find_include(&p, "also-fake.h") == NULL);
    T_CHECK(find_include(&p, "string-fake.h") == NULL);
    atlas_code_parse_free(&p);
}

/* --- functions -------------------------------------------------------------- */

static void test_definition_versus_declaration(void) {
    atlas_code_parse p;
    run_c("int declared(int a, char *b);\n"
          "static void defined(void) { return; }\n"
          "extern int also_declared(void);\n",
          &p);
    const atlas_code_symbol_item *s = find_sym(&p, "declared", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_CHECK(!s->is_definition);
    T_CHECK(s->is_declaration);
    T_EQ_INT(s->linkage, ATLAS_CODE_LINK_EXTERNAL);

    s = find_sym(&p, "defined", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_CHECK(s->is_definition);
    T_CHECK(!s->is_declaration);
    /* `static` is the fact that keeps two files' identically named helpers
     * apart, so it is asserted rather than assumed. */
    T_EQ_INT(s->linkage, ATLAS_CODE_LINK_INTERNAL);
    T_EQ_INT(s->line, 2);
    T_EQ_INT(s->end_line, 2);

    s = find_sym(&p, "also_declared", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_CHECK(!s->is_definition);
    atlas_code_parse_free(&p);
}

static void test_function_end_line_and_nesting(void) {
    atlas_code_parse p;
    run_c("void outer(void)\n"
          "{\n"
          "    if (1) {\n"
          "        while (0) { }\n"
          "    }\n"
          "}\n"
          "int after(void) { return 0; }\n",
          &p);
    const atlas_code_symbol_item *s = find_sym(&p, "outer", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_EQ_INT(s->line, 1);
    T_EQ_INT(s->end_line, 6);
    /* The nested braces must not leave the extractor thinking it is still
     * inside `outer` when `after` arrives. */
    s = find_sym(&p, "after", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_CHECK(s->is_definition);
    atlas_code_parse_free(&p);
}

static void test_attributes_tolerated(void) {
    atlas_code_parse p;
    run_c("__attribute__((visibility(\"default\"))) void tagged(void) { }\n"
          "void noret(void) __attribute__((noreturn));\n"
          "static __inline__ int inl(void) { return 1; }\n"
          "extern int printf(const char *__restrict fmt, ...);\n",
          &p);
    T_CHECK(find_sym(&p, "tagged", ATLAS_CODE_SYM_FUNCTION) != NULL);
    T_CHECK(find_sym(&p, "noret", ATLAS_CODE_SYM_FUNCTION) != NULL);
    T_CHECK(find_sym(&p, "inl", ATLAS_CODE_SYM_FUNCTION) != NULL);
    T_CHECK(find_sym(&p, "printf", ATLAS_CODE_SYM_FUNCTION) != NULL);
    /* `visibility` and `noreturn` are attribute arguments, not declarations. */
    T_CHECK(find_any(&p, "visibility") == NULL);
    T_CHECK(find_any(&p, "noreturn") == NULL);
    atlas_code_parse_free(&p);
}

/* --- calls ------------------------------------------------------------------ */

static void test_call_candidates(void) {
    atlas_code_parse p;
    run_c("void helper(void);\n"
          "void caller(void) {\n"
          "    helper();\n"
          "    if (helper) { }\n"
          "    while (1) { helper(); }\n"
          "}\n",
          &p);
    T_CHECK(has_occurrence(&p, "helper"));
    /* Two calls, and the bare `if (helper)` is not one: an identifier is a call
     * candidate only when a `(` follows it. */
    T_EQ_INT(p.occurrence_count, 2);
    const atlas_code_symbol_item *caller = find_sym(&p, "caller", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(caller != NULL);
    for (size_t i = 0; i < p.occurrence_count; i++) {
        T_CHECK(p.occurrences[i].enclosing >= 0);
        T_EQ_STR(atlas_code_parse_name(&p, p.symbols[p.occurrences[i].enclosing].name_off),
                 "caller");
    }
    atlas_code_parse_free(&p);
}

static void test_calls_not_recorded_where_they_are_not_calls(void) {
    atlas_code_parse p;
    run_c("void f(void) {\n"
          "    int n = sizeof(int);\n"
          "    struct s { int x; } v;\n"
          "    v.member(1);\n"
          "    ptr->member(2);\n"
          "    if (a > b(1)) { }\n"
          "    /* commented(); */\n"
          "    const char *t = \"instring();\";\n"
          "}\n",
          &p);
    /* A member access through `.` or `->` is a call through a pointer, and Atlas
     * has no basis for saying what it reaches. */
    T_CHECK(!has_occurrence(&p, "member"));
    /* `sizeof` is a keyword; a comment and a literal are not code. */
    T_CHECK(!has_occurrence(&p, "sizeof"));
    T_CHECK(!has_occurrence(&p, "commented"));
    T_CHECK(!has_occurrence(&p, "instring"));
    /* `a > b(1)` really is a call to b. `->` is two tokens, so a scanner that
     * treated a lone `>` as a member access would silently drop this. */
    T_CHECK(has_occurrence(&p, "b"));
    atlas_code_parse_free(&p);
}

static void test_function_pointer_call_is_not_a_named_call(void) {
    atlas_code_parse p;
    run_c("void f(void) {\n"
          "    (*fp)(1);\n"
          "    int (*local)(void) = 0;\n"
          "}\n",
          &p);
    /* `(*fp)(1)` has no identifier immediately before the call parenthesis, so
     * nothing is recorded. Recording `fp` would be a claim about a target Atlas
     * cannot possibly know. */
    T_CHECK(!has_occurrence(&p, "fp"));
    T_CHECK(!has_occurrence(&p, "local"));
    atlas_code_parse_free(&p);
}

/* --- types ------------------------------------------------------------------ */

static void test_typedefs_and_tags(void) {
    atlas_code_parse p;
    run_c("typedef int myint;\n"
          "typedef struct named { int a; } named_t;\n"
          "typedef struct { int b; } anon_t;\n"
          "typedef int (*callback)(void *ctx);\n"
          "struct forward;\n"
          "union u { int a; float b; };\n"
          "enum e { E_ONE, E_TWO = 5, E_THREE };\n",
          &p);
    T_CHECK(find_sym(&p, "myint", ATLAS_CODE_SYM_TYPEDEF) != NULL);
    T_CHECK(find_sym(&p, "named_t", ATLAS_CODE_SYM_TYPEDEF) != NULL);
    T_CHECK(find_sym(&p, "anon_t", ATLAS_CODE_SYM_TYPEDEF) != NULL);
    T_CHECK(find_sym(&p, "callback", ATLAS_CODE_SYM_TYPEDEF) != NULL);

    const atlas_code_symbol_item *s = find_sym(&p, "named", ATLAS_CODE_SYM_STRUCT);
    T_REQUIRE(s != NULL);
    T_CHECK(s->is_definition);
    s = find_sym(&p, "forward", ATLAS_CODE_SYM_STRUCT);
    T_REQUIRE(s != NULL);
    /* A forward declaration is a declaration, not a definition — and it must not
     * also produce a file-scope variable called `forward`. */
    T_CHECK(!s->is_definition);
    T_CHECK(find_sym(&p, "forward", ATLAS_CODE_SYM_VARIABLE) == NULL);

    T_CHECK(find_sym(&p, "u", ATLAS_CODE_SYM_UNION) != NULL);
    T_CHECK(find_sym(&p, "e", ATLAS_CODE_SYM_ENUM) != NULL);
    T_CHECK(find_sym(&p, "E_ONE", ATLAS_CODE_SYM_ENUM_CONSTANT) != NULL);
    T_CHECK(find_sym(&p, "E_TWO", ATLAS_CODE_SYM_ENUM_CONSTANT) != NULL);
    T_CHECK(find_sym(&p, "E_THREE", ATLAS_CODE_SYM_ENUM_CONSTANT) != NULL);
    atlas_code_parse_free(&p);
}

static void test_struct_members_are_not_globals(void) {
    atlas_code_parse p;
    run_c("struct box { int width; int height; };\n"
          "int real_global;\n"
          "extern int declared_global;\n",
          &p);
    /* A member is not a file-scope object. Recording one would produce a symbol
     * that resolves against nothing and pollutes every search for its name. */
    T_CHECK(find_any(&p, "width") == NULL);
    T_CHECK(find_any(&p, "height") == NULL);
    const atlas_code_symbol_item *s = find_sym(&p, "real_global", ATLAS_CODE_SYM_VARIABLE);
    T_REQUIRE(s != NULL);
    T_CHECK(s->is_definition);
    s = find_sym(&p, "declared_global", ATLAS_CODE_SYM_VARIABLE);
    T_REQUIRE(s != NULL);
    T_CHECK(!s->is_definition);
    T_CHECK(s->is_declaration);
    atlas_code_parse_free(&p);
}

/* --- the preprocessor -------------------------------------------------------- */

static void test_macros(void) {
    atlas_code_parse p;
    run_c("#define PLAIN 1\n"
          "#define FUNCLIKE(a, b) ((a) + (b))\n"
          "#define SPACED (x)\n"
          "#define MULTI(a) do { \\\n"
          "    thing(a); \\\n"
          "} while (0)\n"
          "int after_macros(void) { return 0; }\n",
          &p);
    T_CHECK(find_sym(&p, "PLAIN", ATLAS_CODE_SYM_MACRO) != NULL);
    T_CHECK(find_sym(&p, "FUNCLIKE", ATLAS_CODE_SYM_MACRO_FUNCTION) != NULL);
    /* `#define SPACED (x)` is object-like: the parenthesis does not touch the
     * name. Getting this backwards produces false function-like macros. */
    T_CHECK(find_sym(&p, "SPACED", ATLAS_CODE_SYM_MACRO) != NULL);
    T_CHECK(find_sym(&p, "MULTI", ATLAS_CODE_SYM_MACRO_FUNCTION) != NULL);
    /* The replacement text is skipped whole, so the call inside a multi-line
     * macro body is not attributed to a function that does not exist... */
    T_CHECK(!has_occurrence(&p, "thing"));
    /* ...and the scanner is still correctly positioned afterwards. */
    T_CHECK(find_sym(&p, "after_macros", ATLAS_CODE_SYM_FUNCTION) != NULL);
    atlas_code_parse_free(&p);
}

static void test_macro_body_cannot_unbalance_the_scan(void) {
    atlas_code_parse p;
    /* Each of these would derail a scanner that treated replacement text as
     * code: an unmatched parenthesis, an unmatched brace, and an unterminated
     * string literal. */
    run_c("#define OPEN (\n"
          "#define BRACE {\n"
          "#define BROKEN \"unterminated\n"
          "int survivor(void) { return 1; }\n",
          &p);
    const atlas_code_symbol_item *s = find_sym(&p, "survivor", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_CHECK(s->is_definition);
    T_EQ_INT(s->line, 4);
    atlas_code_parse_free(&p);
}

static void test_conditional_compilation(void) {
    atlas_code_parse p;
    run_c("int always(void);\n"
          "#ifdef SOMETHING\n"
          "int conditional(void);\n"
          "#endif\n"
          "int also_always(void);\n",
          &p);
    const atlas_code_symbol_item *s = find_sym(&p, "always", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_SOURCE_EXACT);
    s = find_sym(&p, "conditional", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    /* Atlas does not evaluate the preprocessor, so it says CONDITIONAL rather
     * than claiming the declaration is there. */
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_CONDITIONAL);
    s = find_sym(&p, "also_always", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_SOURCE_EXACT);
    atlas_code_parse_free(&p);
}

static void test_include_guard_is_discounted(void) {
    atlas_code_parse p;
    run("#ifndef ATLAS_THING_H\n"
        "#define ATLAS_THING_H\n"
        "int guarded(void);\n"
        "#ifdef EXTRA\n"
        "int extra(void);\n"
        "#endif\n"
        "#endif\n",
        ATLAS_CODE_LANG_C_HEADER, &p);
    T_CHECK(p.include_guard);
    const atlas_code_symbol_item *s = find_sym(&p, "guarded", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    /* Without guard discounting, every declaration in every header in the world
     * would be CONDITIONAL, which is true in the letter and useless. */
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_SOURCE_EXACT);
    s = find_sym(&p, "extra", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_CONDITIONAL);
    atlas_code_parse_free(&p);
}

static void test_guard_only_when_it_really_is_one(void) {
    atlas_code_parse p;
    run("#include \"first.h\"\n"
        "#ifndef NOT_A_GUARD\n"
        "int inside(void);\n"
        "#endif\n",
        ATLAS_CODE_LANG_C_HEADER, &p);
    T_CHECK(!p.include_guard);
    const atlas_code_symbol_item *s = find_sym(&p, "inside", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_CONDITIONAL);
    atlas_code_parse_free(&p);

    /* An `#ifndef` whose `#define` names something else is not a guard either. */
    run("#ifndef GUARD_A\n"
        "#define GUARD_B\n"
        "int x(void);\n"
        "#endif\n",
        ATLAS_CODE_LANG_C_HEADER, &p);
    T_CHECK(!p.include_guard);
    s = find_sym(&p, "x", ATLAS_CODE_SYM_FUNCTION);
    T_REQUIRE(s != NULL);
    T_EQ_INT(s->resolution, ATLAS_CODE_RES_CONDITIONAL);
    atlas_code_parse_free(&p);
}

/* --- escaped newlines --------------------------------------------------------- */

static void test_escaped_newlines(void) {
    atlas_code_parse p;
    /* A splice inside an identifier, inside a directive keyword, and turning a
     * `//` comment into a two-line one. Every one of them changes what the bytes
     * mean, and every one is invisible to a scanner that works line by line. */
    run_c("int spl\\\n"
          "it(void) { return 0; }\n"
          "#inc\\\n"
          "lude \"spliced.h\"\n"
          "// this comment continues \\\n"
          "int hidden(void) { return 0; }\n"
          "int visible(void) { return 0; }\n",
          &p);
    T_CHECK(find_sym(&p, "split", ATLAS_CODE_SYM_FUNCTION) != NULL);
    T_CHECK(find_include(&p, "spliced.h") != NULL);
    /* The declaration on the line after the continued comment is inside the
     * comment. A line-oriented scanner records it; a correct one does not. */
    T_CHECK(find_sym(&p, "hidden", ATLAS_CODE_SYM_FUNCTION) == NULL);
    T_CHECK(find_sym(&p, "visible", ATLAS_CODE_SYM_FUNCTION) != NULL);
    atlas_code_parse_free(&p);
}

/* --- hostile and malformed input ------------------------------------------------ */

static void test_binary_and_nul(void) {
    atlas_code_parse p;
    atlas_err err;
    atlas_err_init(&err);
    static const char blob[] = {'i', 'n', 't', ' ', 'f', '(', ')', ';', 0, 'x', 'y'};
    T_OK(atlas_code_extract(blob, sizeof(blob), ATLAS_CODE_LANG_C, &p, &err), &err);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_SKIPPED);
    T_EQ_INT(p.symbol_count, 0);
    T_CHECK(p.truncated_reason != NULL);
    atlas_code_parse_free(&p);
}

static void test_invalid_utf8_is_not_a_failure(void) {
    atlas_code_parse p;
    atlas_err err;
    atlas_err_init(&err);
    /* Invalid UTF-8 in a comment and in an identifier. C source is bytes; the
     * extractor must keep going and must not turn the bytes into punctuation
     * that changes the parse. */
    static const char src[] = "/* \xff\xfe */\nint valid(void) { return 0; }\n";
    T_OK(atlas_code_extract(src, sizeof(src) - 1u, ATLAS_CODE_LANG_C, &p, &err), &err);
    T_CHECK(find_sym(&p, "valid", ATLAS_CODE_SYM_FUNCTION) != NULL);
    atlas_code_parse_free(&p);
}

static void test_unterminated_constructs(void) {
    atlas_code_parse p;
    run_c("int before(void);\n/* never closed\n", &p);
    T_CHECK(find_sym(&p, "before", ATLAS_CODE_SYM_FUNCTION) != NULL);
    /* Damaged, and it says so: what was extracted before the damage is still
     * true, and reporting the file as complete would not be. */
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_PARTIAL);
    atlas_code_parse_free(&p);

    run_c("const char *s = \"never closed\nint after(void) { return 0; }\n", &p);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_PARTIAL);
    atlas_code_parse_free(&p);
}

static void test_unrecognised_construct_is_partial(void) {
    atlas_code_parse p;
    /* A K&R-style definition. Atlas does not understand it, records what it can,
     * and says the file is partial rather than inventing a symbol. */
    run_c("int old_style(a, b)\n"
          "    int a;\n"
          "    int b;\n"
          "{\n"
          "    return a + b;\n"
          "}\n",
          &p);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_PARTIAL);
    atlas_code_parse_free(&p);
}

static void test_very_long_token(void) {
    size_t n = ATLAS_CODE_MAX_TOKEN_BYTES + 64u;
    char *src = malloc(n + 64u);
    T_REQUIRE(src != NULL);
    size_t k = 0;
    src[k++] = 'i';
    src[k++] = 'n';
    src[k++] = 't';
    src[k++] = ' ';
    for (size_t i = 0; i < n; i++) {
        src[k++] = 'z';
    }
    memcpy(src + k, "(void);\nint ok(void);\n", strlen("(void);\nint ok(void);\n") + 1u);

    atlas_code_parse p;
    run_c(src, &p);
    /* Refused as a name rather than truncated: half an identifier is a different
     * identifier, and matching against it would be worse than not matching. */
    T_CHECK(p.truncated);
    T_CHECK(find_sym(&p, "ok", ATLAS_CODE_SYM_FUNCTION) != NULL);
    atlas_code_parse_free(&p);
    free(src);
}

static void test_symbol_ceiling(void) {
    /* Well past ATLAS_CODE_MAX_SYMBOLS_PER_FILE, so the ceiling is reached and
     * reported rather than silently stopping. */
    size_t want = (size_t)ATLAS_CODE_MAX_SYMBOLS_PER_FILE + 100u;
    atlas_buf src = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    for (size_t i = 0; i < want; i++) {
        T_OK(atlas_buf_appendf(&src, &err, "int f%zu(void);\n", i), &err);
    }
    atlas_code_parse p;
    T_OK(atlas_code_extract(src.data, src.len, ATLAS_CODE_LANG_C, &p, &err), &err);
    T_EQ_INT(p.symbol_count, ATLAS_CODE_MAX_SYMBOLS_PER_FILE);
    T_CHECK(p.truncated);
    T_CHECK(p.dropped_symbols > 0);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_PARTIAL);
    T_EQ_STR(p.truncated_reason, ATLAS_CODE_WHY_TRUNCATED);
    atlas_code_parse_free(&p);
    atlas_buf_free(&src);
}

static void test_unsupported_language_is_skipped(void) {
    atlas_code_parse p;
    run("def thing():\n    pass\n", ATLAS_CODE_LANG_NONE, &p);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_SKIPPED);
    T_EQ_INT(p.symbol_count, 0);
    atlas_code_parse_free(&p);
}

static void test_empty_and_whitespace(void) {
    atlas_code_parse p;
    run_c("", &p);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_OK);
    T_EQ_INT(p.symbol_count, 0);
    atlas_code_parse_free(&p);

    run_c("\n\n   \t\n/* nothing */\n", &p);
    T_EQ_INT(p.status, ATLAS_CODE_PARSE_OK);
    T_EQ_INT(p.symbol_count, 0);
    atlas_code_parse_free(&p);
}

/* --- language classification --------------------------------------------------- */

static void test_language_classification(void) {
    T_EQ_INT(atlas_code_language_of("a.c", 3), ATLAS_CODE_LANG_C);
    T_EQ_INT(atlas_code_language_of("src/x/y.h", 9), ATLAS_CODE_LANG_C_HEADER);
    T_EQ_INT(atlas_code_language_of("gen.inc", 7), ATLAS_CODE_LANG_C_FRAGMENT);
    T_EQ_INT(atlas_code_language_of("keys.def", 8), ATLAS_CODE_LANG_C_FRAGMENT);
    /* C++ is deliberately out of scope, and `.C` is C++ by convention. Matching
     * it case-insensitively would have Atlas extract C semantics from C++. */
    T_EQ_INT(atlas_code_language_of("a.C", 3), ATLAS_CODE_LANG_NONE);
    T_EQ_INT(atlas_code_language_of("a.cpp", 5), ATLAS_CODE_LANG_NONE);
    T_EQ_INT(atlas_code_language_of("a.hpp", 5), ATLAS_CODE_LANG_NONE);
    T_EQ_INT(atlas_code_language_of("Makefile", 8), ATLAS_CODE_LANG_NONE);
    T_EQ_INT(atlas_code_language_of("", 0), ATLAS_CODE_LANG_NONE);
}

/* --- roles ---------------------------------------------------------------------- */

static bool has_role(const atlas_code_roles *r, atlas_code_role role, atlas_code_role_basis basis) {
    for (size_t i = 0; i < r->count; i++) {
        if (r->items[i].role == (int32_t)role && r->items[i].basis == (int32_t)basis) {
            return true;
        }
    }
    return false;
}

static void test_file_roles(void) {
    atlas_code_roles r;

    atlas_code_classify_roles("src/core/buf.c", 14, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_IMPLEMENTATION, ATLAS_CODE_BASIS_EXTENSION));

    atlas_code_classify_roles("include/atlas/buf.h", 19, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_PUBLIC_HEADER, ATLAS_CODE_BASIS_PATH_NAMING));

    atlas_code_classify_roles("src/db/db_internal.h", 20, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_PRIVATE_HEADER, ATLAS_CODE_BASIS_PATH_NAMING));

    /* Both, and it says both: a test written in C is an implementation and a
     * test, and collapsing that to one role throws away the fact somebody
     * actually wants. */
    atlas_code_classify_roles("tests/test_scan.c", 17, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_TEST, ATLAS_CODE_BASIS_PATH_NAMING));
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_IMPLEMENTATION, ATLAS_CODE_BASIS_EXTENSION));

    atlas_code_classify_roles("third_party/yyjson/yyjson.c", 27, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_VENDORED, ATLAS_CODE_BASIS_PATH_NAMING));

    atlas_code_classify_roles("CMakeLists.txt", 14, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_BUILD_METADATA, ATLAS_CODE_BASIS_PATH_NAMING));

    atlas_code_classify_roles("docs/architecture.md", 20, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_DOCUMENTATION, ATLAS_CODE_BASIS_PATH_NAMING));

    /* Component matching, not substring: a directory called `thirdpartytools`
     * is not vendored and `contributing.md` is not a test. */
    atlas_code_classify_roles("thirdpartytools/a.c", 19, NULL, 0, &r);
    T_CHECK(!has_role(&r, ATLAS_CODE_ROLE_VENDORED, ATLAS_CODE_BASIS_PATH_NAMING));

    /* Generated is the one role with content evidence, and it is reported as
     * content evidence rather than as a path claim. */
    const char *marker = "/* AUTOMATICALLY GENERATED - DO NOT EDIT */\n";
    atlas_code_classify_roles("src/gen/table.c", 15, marker, strlen(marker), &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_GENERATED, ATLAS_CODE_BASIS_CONTENT_MARKER));

    /* Nothing matched is a stated fact, not a silence. */
    atlas_code_classify_roles("weird", 5, NULL, 0, &r);
    T_CHECK(has_role(&r, ATLAS_CODE_ROLE_UNKNOWN, ATLAS_CODE_BASIS_NONE));
}

/* --- the vocabulary ------------------------------------------------------------- */

static void test_resolution_vocabulary(void) {
    atlas_code_resolution r;
    T_CHECK(atlas_code_resolution_parse("SOURCE_EXACT", &r));
    T_EQ_INT(r, ATLAS_CODE_RES_SOURCE_EXACT);
    T_CHECK(atlas_code_resolution_parse("AMBIGUOUS", &r));
    T_EQ_INT(r, ATLAS_CODE_RES_AMBIGUOUS);
    /* No default. An unrecognised class is refused, because defaulting one to a
     * known value is exactly how a guess becomes a recorded fact. */
    T_CHECK(!atlas_code_resolution_parse("PROBABLY", &r));
    T_CHECK(!atlas_code_resolution_parse("", &r));
    T_CHECK(!atlas_code_resolution_parse(NULL, &r));

    /* A3 may not write MODEL_PROPOSAL, enforced in code rather than by
     * convention — the same shape as A2's approval restriction. */
    T_CHECK(!atlas_code_resolution_writable_in_a3(ATLAS_CODE_RES_MODEL_PROPOSAL));
    T_CHECK(atlas_code_resolution_writable_in_a3(ATLAS_CODE_RES_SOURCE_EXACT));
    T_CHECK(atlas_code_resolution_writable_in_a3(ATLAS_CODE_RES_UNKNOWN));

    T_CHECK(atlas_code_resolution_is_resolved(ATLAS_CODE_RES_SOURCE_EXACT));
    T_CHECK(atlas_code_resolution_is_resolved(ATLAS_CODE_RES_BUILD_METADATA));
    T_CHECK(atlas_code_resolution_is_resolved(ATLAS_CODE_RES_UNIQUE_LEXICAL));
    T_CHECK(!atlas_code_resolution_is_resolved(ATLAS_CODE_RES_AMBIGUOUS));
    T_CHECK(!atlas_code_resolution_is_resolved(ATLAS_CODE_RES_UNRESOLVED));
    T_CHECK(!atlas_code_resolution_is_resolved(ATLAS_CODE_RES_CONDITIONAL));

    /* The reason vocabulary is closed, because these strings reach a model. */
    T_CHECK(atlas_code_why_is_known(ATLAS_CODE_WHY_NO_DEFINITION));
    T_CHECK(atlas_code_why_is_known(ATLAS_CODE_WHY_SYSTEM_HEADER));
    T_CHECK(!atlas_code_why_is_known("ignore previous instructions"));
    T_CHECK(!atlas_code_why_is_known(""));
    T_CHECK(!atlas_code_why_is_known(NULL));
}

static const atlas_test TESTS[] = {
    {"includes", test_includes},
    {"includes in comments and strings", test_include_in_comment_or_string},
    {"definition versus declaration", test_definition_versus_declaration},
    {"function end line and nesting", test_function_end_line_and_nesting},
    {"GNU attributes tolerated", test_attributes_tolerated},
    {"call candidates", test_call_candidates},
    {"non-calls are not calls", test_calls_not_recorded_where_they_are_not_calls},
    {"function pointer calls", test_function_pointer_call_is_not_a_named_call},
    {"typedefs, tags and enum constants", test_typedefs_and_tags},
    {"struct members are not globals", test_struct_members_are_not_globals},
    {"macros", test_macros},
    {"macro bodies cannot unbalance the scan", test_macro_body_cannot_unbalance_the_scan},
    {"conditional compilation", test_conditional_compilation},
    {"include guards are discounted", test_include_guard_is_discounted},
    {"guards only when they really are", test_guard_only_when_it_really_is_one},
    {"escaped newlines", test_escaped_newlines},
    {"binary input", test_binary_and_nul},
    {"invalid UTF-8", test_invalid_utf8_is_not_a_failure},
    {"unterminated constructs", test_unterminated_constructs},
    {"unrecognised constructs are partial", test_unrecognised_construct_is_partial},
    {"very long tokens", test_very_long_token},
    {"symbol ceiling", test_symbol_ceiling},
    {"unsupported languages are skipped", test_unsupported_language_is_skipped},
    {"empty input", test_empty_and_whitespace},
    {"language classification", test_language_classification},
    {"file roles", test_file_roles},
    {"resolution vocabulary", test_resolution_vocabulary},
};

ATLAS_TEST_MAIN("code_lex", TESTS)

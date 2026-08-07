/* Atlas - the bounded lexical C indexer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Bytes in, a bounded result object out. This file opens no file, touches no
 * database handle and creates no process, which is what lets it run on the
 * daemon's worker threads beside the hash jobs.
 *
 * It is a lexer and a small state machine over its tokens, and it is
 * deliberately not more than that. What it produces is described exactly by
 * docs/code-intelligence.md; the two properties that matter here:
 *
 *   - **What it recognises, it recognises from the bytes.** A `#include` really
 *     is at that line. A `{` really does follow that declarator. Those are
 *     SOURCE_EXACT, and the resolution of what they *refer to* is somebody
 *     else's problem — src/code/resolve.c — with its own, weaker classes.
 *
 *   - **What it does not understand, it says it does not understand.** An
 *     unrecognised construct sets `partial` rather than producing a guess, and
 *     every ceiling sets `truncated` with a fixed reason. A file that reads as
 *     fully described when it is not is worse than one that reports a gap.
 *
 * Three things are handled by construction rather than by filtering, because
 * every one of them is a place where code-shaped bytes are not code:
 * comments, string and character literals, and line splices. A backslash before
 * a newline is removed by the reader itself, so it cannot hide inside an
 * identifier, a directive, a comment or a literal and change what follows.
 * Preprocessor replacement text is skipped whole, so `#define OPEN (` does not
 * open a parenthesis and `#define S "unterminated` does not swallow the file.
 */
#include "atlas/code.h"

#include <stdlib.h>
#include <string.h>

/* --- the reader -----------------------------------------------------------
 *
 * Every read goes through here, and every read removes line splices first. That
 * is the only place continuation handling exists, which is what makes "an
 * escaped newline cannot change what a token is" a property of the reader
 * rather than a rule each caller has to remember. */

typedef struct lexer {
    const unsigned char *p;
    size_t len;
    size_t pos;
    int64_t line;
    size_t line_start;
    /* True while nothing but whitespace and comments has been seen on this
     * logical line. A `#` is a directive only then. */
    bool at_line_start;
    /* Something was not understood: an unterminated comment or literal. */
    bool damaged;
} lexer;

static void lx_skip_splices(lexer *lx) {
    while (lx->pos + 1u < lx->len && lx->p[lx->pos] == '\\') {
        size_t k = lx->pos + 1u;
        if (lx->p[k] == '\r' && k + 1u < lx->len && lx->p[k + 1u] == '\n') {
            k += 2u;
        } else if (lx->p[k] == '\n') {
            k += 1u;
        } else {
            break;
        }
        lx->pos = k;
        lx->line++;
        lx->line_start = lx->pos;
    }
}

static int lx_peek(lexer *lx) {
    lx_skip_splices(lx);
    return lx->pos < lx->len ? (int)lx->p[lx->pos] : -1;
}

static int lx_get(lexer *lx) {
    lx_skip_splices(lx);
    if (lx->pos >= lx->len) {
        return -1;
    }
    int c = (int)lx->p[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->line_start = lx->pos;
        lx->at_line_start = true;
    }
    return c;
}

static int64_t lx_col(const lexer *lx, size_t off) {
    if (off < lx->line_start) {
        return 1;
    }
    return (int64_t)(off - lx->line_start) + 1;
}

/* --- tokens ---------------------------------------------------------------- */

typedef enum tok_kind {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_CHAR,
    TOK_PUNCT
} tok_kind;

typedef struct token {
    tok_kind kind;
    size_t off; /* first byte */
    size_t len;
    int64_t line;
    int64_t col;
    char ch;         /* punctuation character */
    bool line_start; /* first token on its logical line */
    bool oversize;   /* longer than ATLAS_CODE_MAX_TOKEN_BYTES; unusable as a name */
} token;

static bool ident_start(int c) {
    /* Bytes at or above 0x80 are treated as identifier characters. C11 permits
     * universal character names in identifiers, and more usefully it keeps a
     * file that is not valid UTF-8 from derailing the scan: the byte becomes
     * part of a name Atlas will not match anything against, rather than a
     * punctuation character that changes the parse. */
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

static bool ident_cont(int c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

/* Skips a block comment. Returns false when it was never terminated, which is a
 * damaged file rather than a reason to keep scanning its remainder as code. */
static bool skip_block_comment(lexer *lx) {
    int prev = 0;
    for (;;) {
        int c = lx_get(lx);
        if (c < 0) {
            lx->damaged = true;
            return false;
        }
        if (prev == '*' && c == '/') {
            return true;
        }
        prev = c;
    }
}

static void skip_line_comment(lexer *lx) {
    /* Splices are already removed by the reader, so a `//` comment continued by
     * a backslash correctly swallows the next line too — which is exactly what
     * a compiler does and exactly the case a naive scanner gets wrong. */
    int c;
    while ((c = lx_peek(lx)) >= 0 && c != '\n') {
        (void)lx_get(lx);
    }
}

static void next_token(lexer *lx, token *t) {
    memset(t, 0, sizeof(*t));
    for (;;) {
        int c = lx_peek(lx);
        if (c < 0) {
            t->kind = TOK_EOF;
            t->line = lx->line;
            t->off = lx->pos;
            return;
        }
        if (c == '\n') {
            (void)lx_get(lx);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            (void)lx_get(lx);
            continue;
        }
        if (c == '/') {
            size_t save = lx->pos;
            int64_t sline = lx->line;
            size_t sls = lx->line_start;
            (void)lx_get(lx);
            int d = lx_peek(lx);
            if (d == '/') {
                skip_line_comment(lx);
                continue;
            }
            if (d == '*') {
                (void)lx_get(lx);
                if (!skip_block_comment(lx)) {
                    t->kind = TOK_EOF;
                    t->line = lx->line;
                    return;
                }
                continue;
            }
            /* Not a comment. Rewind so it is emitted as ordinary punctuation. */
            lx->pos = save;
            lx->line = sline;
            lx->line_start = sls;
        }
        break;
    }

    lx_skip_splices(lx);
    t->line_start = lx->at_line_start;
    lx->at_line_start = false;
    t->off = lx->pos;
    t->line = lx->line;
    t->col = lx_col(lx, lx->pos);

    int c = lx_peek(lx);
    if (ident_start(c)) {
        t->kind = TOK_IDENT;
        while (ident_cont(lx_peek(lx))) {
            (void)lx_get(lx);
        }
    } else if (c >= '0' && c <= '9') {
        t->kind = TOK_NUMBER;
        /* A pp-number: digits, letters, dots, and a sign after an exponent
         * character. Consumed as one token so `1e+5` does not look like an
         * addition and `0x1p-3` does not look like a subtraction. */
        int prev = 0;
        for (;;) {
            int d = lx_peek(lx);
            if (d < 0) {
                break;
            }
            bool sign_ok = (d == '+' || d == '-') &&
                           (prev == 'e' || prev == 'E' || prev == 'p' || prev == 'P');
            if (!(ident_cont(d) || d == '.' || sign_ok)) {
                break;
            }
            prev = lx_get(lx);
        }
    } else if (c == '"' || c == '\'') {
        /* A literal is consumed whole so that code-shaped bytes inside it are
         * never scanned. An unterminated one stops at the newline and damages
         * the file rather than consuming the rest of it. */
        int quote = lx_get(lx);
        t->kind = (quote == '"') ? TOK_STRING : TOK_CHAR;
        for (;;) {
            int d = lx_peek(lx);
            if (d < 0 || d == '\n') {
                lx->damaged = true;
                break;
            }
            (void)lx_get(lx);
            if (d == '\\') {
                int e = lx_peek(lx);
                if (e >= 0 && e != '\n') {
                    (void)lx_get(lx);
                }
                continue;
            }
            if (d == quote) {
                break;
            }
        }
    } else {
        t->kind = TOK_PUNCT;
        t->ch = (char)lx_get(lx);
    }
    t->len = lx->pos - t->off;
    if (t->len > ATLAS_CODE_MAX_TOKEN_BYTES) {
        /* Refused as a name rather than truncated: half an identifier is a
         * different identifier, and matching against one would be worse than
         * matching against nothing. */
        t->oversize = true;
    }
}

/* --- token text -------------------------------------------------------------
 *
 * The reader steps over line splices, but the token's *byte range* still spans
 * them: `spl\<newline>it` is one identifier occupying seven bytes of source and
 * spelling five. Every comparison and every stored name therefore goes through
 * here.
 *
 * This is not a nicety. Without it `#inc\<newline>lude` is not recognised as a
 * directive and the symbol `spl\<newline>it` is stored under a name nothing will
 * ever match — both of which look like the extractor simply missing something,
 * rather than like the encoding bug they are. */

static bool has_splice(const unsigned char *s, size_t n) {
    for (size_t i = 0; i + 1u < n; i++) {
        if (s[i] == '\\' && (s[i + 1u] == '\n' || s[i + 1u] == '\r')) {
            return true;
        }
    }
    return false;
}

static const unsigned char *desplice(const unsigned char *s, size_t n, unsigned char *buf,
                                     size_t cap, size_t *out_len) {
    if (!has_splice(s, n) || n >= cap) {
        *out_len = n;
        return s;
    }
    size_t k = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\\' && i + 1u < n) {
            if (s[i + 1u] == '\n') {
                i += 2u;
                continue;
            }
            if (s[i + 1u] == '\r') {
                i += (i + 2u < n && s[i + 2u] == '\n') ? 3u : 2u;
                continue;
            }
        }
        buf[k++] = s[i++];
    }
    *out_len = k;
    return buf;
}

static const unsigned char *tok_text(const lexer *lx, const token *t, unsigned char *buf,
                                     size_t cap, size_t *out_len) {
    return desplice(lx->p + t->off, t->len, buf, cap, out_len);
}

/* Enough for any token Atlas will treat as a name; an oversize token is refused
 * as a name anyway, so it never needs to be copied. */
#define TOK_BUF_BYTES (ATLAS_CODE_MAX_TOKEN_BYTES + 1u)

/* --- keywords ---------------------------------------------------------------
 *
 * Two lists with different jobs. `is_keyword` decides whether an identifier can
 * be a declarator name; `is_not_a_call` additionally covers the GNU
 * pseudo-functions that are spelled exactly like a call and are not one. */

static bool str_eq(const unsigned char *p, size_t n, const char *s) {
    size_t m = strlen(s);
    return n == m && memcmp(p, s, m) == 0;
}

static bool is_keyword(const unsigned char *p, size_t n) {
    static const char *const KW[] = {
        "auto",       "break",     "case",       "char",      "const",     "continue",
        "default",    "do",        "double",     "else",      "enum",      "extern",
        "float",      "for",       "goto",       "if",        "inline",    "int",
        "long",       "register",  "restrict",   "return",    "short",     "signed",
        "sizeof",     "static",    "struct",     "switch",    "typedef",   "union",
        "unsigned",   "void",      "volatile",   "while",     "_Alignas",  "_Alignof",
        "_Atomic",    "_Bool",     "_Complex",   "_Generic",  "_Imaginary", "_Noreturn",
        "_Static_assert", "_Thread_local",
        /* GNU and MSVC spellings Atlas tolerates rather than understands. */
        "__inline",   "__inline__", "__restrict", "__restrict__", "__const",   "__volatile",
        "__volatile__", "__signed", "__signed__", "__extension__", "__typeof", "__typeof__",
        "typeof",     "__attribute__", "__attribute", "__asm", "__asm__", "asm",
        "__declspec", "__builtin_va_list", "__thread", "__complex__",
        NULL,
    };
    for (size_t i = 0; KW[i] != NULL; i++) {
        if (str_eq(p, n, KW[i])) {
            return true;
        }
    }
    return false;
}

/* Identifiers that are followed by `(` and are not calls.
 *
 * `sizeof(x)` and `__attribute__((...))` are already keywords. What is left is
 * the handful of things that look exactly like a call, are not one, and would
 * otherwise pollute every call graph in a codebase that uses them. */
static bool is_not_a_call(const unsigned char *p, size_t n) {
    static const char *const NOT_CALLS[] = {
        "defined", "static_assert", "alignof", "_Alignof", "_Generic", "sizeof", NULL,
    };
    for (size_t i = 0; NOT_CALLS[i] != NULL; i++) {
        if (str_eq(p, n, NOT_CALLS[i])) {
            return true;
        }
    }
    return false;
}

/* --- the result object ------------------------------------------------------ */

void atlas_code_parse_init(atlas_code_parse *p) {
    memset(p, 0, sizeof(*p));
    atlas_buf_init(&p->arena);
    p->status = ATLAS_CODE_PARSE_OK;
}

void atlas_code_parse_free(atlas_code_parse *p) {
    if (p == NULL) {
        return;
    }
    atlas_buf_free(&p->arena);
    free(p->symbols);
    free(p->includes);
    free(p->occurrences);
    p->symbols = NULL;
    p->includes = NULL;
    p->occurrences = NULL;
    p->symbol_count = 0;
    p->include_count = 0;
    p->occurrence_count = 0;
}

const char *atlas_code_parse_name(const atlas_code_parse *p, uint32_t off) {
    if (p->arena.data == NULL || off >= p->arena.len) {
        return "";
    }
    return p->arena.data + off;
}

/* Interns a name. Offset 0 is reserved for the empty string, which is pushed
 * first, so 0 is a usable "absent" sentinel throughout. */
static atlas_status intern(atlas_code_parse *p, const void *s, size_t n, uint32_t *off_out,
                           uint32_t *len_out, atlas_err *err) {
    if (p->arena.len == 0) {
        atlas_status st = atlas_buf_append_ch(&p->arena, '\0', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (n == 0 || n > ATLAS_CODE_MAX_NAME_BYTES) {
        *off_out = 0;
        *len_out = 0;
        return ATLAS_OK;
    }
    *off_out = (uint32_t)p->arena.len;
    *len_out = (uint32_t)n;
    atlas_status st = atlas_buf_append(&p->arena, s, n, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&p->arena, '\0', err);
    }
    return st;
}

/* Grows one of the item arrays. Bounded: past the ceiling the caller records a
 * drop and sets `truncated`, so a file with a hundred thousand symbols reports
 * a partial answer rather than an unbounded allocation. */
static atlas_status grow(void **items, size_t *cap, size_t count, size_t elem, size_t max,
                         atlas_err *err) {
    if (count < *cap) {
        return ATLAS_OK;
    }
    size_t next = (*cap == 0) ? 64u : *cap * 2u;
    if (next > max) {
        next = max;
    }
    if (next <= count) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "structural item array ceiling reached");
    }
    void *grown = realloc(*items, next * elem);
    if (grown == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory extracting code structure");
    }
    *items = grown;
    *cap = next;
    return ATLAS_OK;
}

static void note_truncated(atlas_code_parse *p, const char *why) {
    if (!p->truncated) {
        p->truncated = true;
        p->truncated_reason = why;
    }
    if (p->status == ATLAS_CODE_PARSE_OK) {
        p->status = ATLAS_CODE_PARSE_PARTIAL;
    }
}

static atlas_status add_symbol(atlas_code_parse *p, const void *name, size_t name_len,
                               atlas_code_symbol_kind kind, atlas_code_linkage linkage,
                               atlas_code_resolution res, bool is_def, bool is_decl, int64_t line,
                               int64_t col, int64_t off, int32_t enclosing, int32_t *index_out,
                               atlas_err *err) {
    if (index_out != NULL) {
        *index_out = -1;
    }
    if (name == NULL || name_len == 0 || name_len > ATLAS_CODE_MAX_NAME_BYTES) {
        return ATLAS_OK; /* nameless or oversize: nothing worth recording */
    }
    if (p->symbol_count >= (size_t)ATLAS_CODE_MAX_SYMBOLS_PER_FILE) {
        p->dropped_symbols++;
        note_truncated(p, ATLAS_CODE_WHY_TRUNCATED);
        return ATLAS_OK;
    }
    atlas_status st = grow((void **)&p->symbols, &p->symbol_cap, p->symbol_count,
                           sizeof(*p->symbols), (size_t)ATLAS_CODE_MAX_SYMBOLS_PER_FILE, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_code_symbol_item *it = &p->symbols[p->symbol_count];
    memset(it, 0, sizeof(*it));
    st = intern(p, name, name_len, &it->name_off, &it->name_len, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (it->name_len == 0) {
        return ATLAS_OK;
    }
    it->kind = (int32_t)kind;
    it->linkage = (int32_t)linkage;
    it->resolution = (int32_t)res;
    it->is_definition = is_def;
    it->is_declaration = is_decl;
    it->line = line;
    it->col = col;
    it->byte_offset = off;
    it->end_line = line;
    it->enclosing = enclosing;
    if (index_out != NULL) {
        *index_out = (int32_t)p->symbol_count;
    }
    p->symbol_count++;
    return ATLAS_OK;
}

static atlas_status add_include(atlas_code_parse *p, const void *spelling, size_t n,
                                atlas_code_include_form form, atlas_code_resolution res,
                                int64_t line, int64_t col, atlas_err *err) {
    if (spelling == NULL || n == 0 || n > ATLAS_CODE_MAX_NAME_BYTES) {
        return ATLAS_OK;
    }
    if (p->include_count >= (size_t)ATLAS_CODE_MAX_INCLUDES_PER_FILE) {
        p->dropped_includes++;
        note_truncated(p, ATLAS_CODE_WHY_TRUNCATED);
        return ATLAS_OK;
    }
    atlas_status st = grow((void **)&p->includes, &p->include_cap, p->include_count,
                           sizeof(*p->includes), (size_t)ATLAS_CODE_MAX_INCLUDES_PER_FILE, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_code_include_item *it = &p->includes[p->include_count];
    memset(it, 0, sizeof(*it));
    st = intern(p, spelling, n, &it->spelling_off, &it->spelling_len, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (it->spelling_len == 0) {
        return ATLAS_OK;
    }
    it->form = (int32_t)form;
    it->resolution = (int32_t)res;
    it->line = line;
    it->col = col;
    p->include_count++;
    return ATLAS_OK;
}

static atlas_status add_occurrence(atlas_code_parse *p, const void *name, size_t n,
                                   int32_t enclosing, atlas_code_resolution res, int64_t line,
                                   int64_t col, int64_t off, atlas_err *err) {
    if (name == NULL || n == 0 || n > ATLAS_CODE_MAX_NAME_BYTES) {
        return ATLAS_OK;
    }
    if (p->occurrence_count >= (size_t)ATLAS_CODE_MAX_OCCURRENCES_PER_FILE) {
        p->dropped_occurrences++;
        note_truncated(p, ATLAS_CODE_WHY_TRUNCATED);
        return ATLAS_OK;
    }
    atlas_status st =
        grow((void **)&p->occurrences, &p->occurrence_cap, p->occurrence_count,
             sizeof(*p->occurrences), (size_t)ATLAS_CODE_MAX_OCCURRENCES_PER_FILE, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_code_occurrence_item *it = &p->occurrences[p->occurrence_count];
    memset(it, 0, sizeof(*it));
    st = intern(p, name, n, &it->name_off, &it->name_len, err);
    if (st != ATLAS_OK) {
        return st;
    }
    if (it->name_len == 0) {
        return ATLAS_OK;
    }
    it->enclosing = enclosing;
    it->resolution = (int32_t)res;
    it->line = line;
    it->col = col;
    it->byte_offset = off;
    p->occurrence_count++;
    return ATLAS_OK;
}

/* --- the declaration run ----------------------------------------------------
 *
 * A "run" is the token sequence from the start of a file-scope declaration to
 * the `;` or `{` that ends it. Everything the extractor claims about a
 * declarator is decided from one run, which is what keeps the state small
 * enough to reason about. */

/* One remembered identifier: its text with splices removed, and where it was.
 *
 * The text is stored rather than a byte range into the source, because a range
 * would have to be de-spliced again at every use and one forgotten call site is
 * a symbol recorded under a name nothing matches. */
typedef struct name_buf {
    char text[ATLAS_CODE_MAX_NAME_BYTES + 1u];
    size_t len;
    bool have;
    int64_t line;
    int64_t col;
    int64_t off;
} name_buf;

static void name_set(name_buf *nb, const unsigned char *s, size_t n, const token *t) {
    if (n == 0 || n > ATLAS_CODE_MAX_NAME_BYTES) {
        nb->have = false;
        return;
    }
    memcpy(nb->text, s, n);
    nb->text[n] = '\0';
    nb->len = n;
    nb->have = true;
    nb->line = t->line;
    nb->col = t->col;
    nb->off = (int64_t)t->off;
}

typedef struct run_state {
    bool has_typedef;
    bool is_static;
    bool is_extern;
    bool saw_equals;
    /* The run opened a brace block: `struct X { ... };` defines the tag, while
     * `struct X;` only declares it, and the two are different facts. Remembered
     * on the run rather than read from the aggregate state, which the closing
     * brace has already cleared by the time the `;` arrives. */
    bool saw_body;
    int paren_depth;
    /* True while inside a parenthesised *declarator* group — `(*name)` — rather
     * than a parameter list.
     *
     * The two look identical to a depth counter and mean opposite things. In
     * `typedef int (*callback)(void *ctx)` the name is `callback`, inside the
     * first group; `ctx` is a parameter and naming the typedef after it would be
     * wrong every time. The distinguishing evidence is that a declarator group
     * opens with `*`. */
    bool in_declarator_paren;
    /* The first identifier immediately before a `(` at parenthesis depth zero:
     * the declarator name of a function. */
    name_buf name;
    /* The last non-keyword identifier eligible to name the declaration, which is
     * what a typedef or an object declaration is named by. */
    name_buf last;
    /* A struct/union/enum introducer and its tag, when it had one. */
    atlas_code_symbol_kind tag_kind;
    bool have_tag_kind;
    name_buf tag;
} run_state;

static void run_reset(run_state *r) {
    memset(r, 0, sizeof(*r));
}

/* --- directives -------------------------------------------------------------- */

/* Consumes the remainder of a preprocessor directive.
 *
 * Block comments are skipped properly rather than scanned past, because a
 * comment that spans lines inside a directive would otherwise leave the scanner
 * reading comment text as code — which is precisely the class of mistake this
 * file exists to avoid. */
static void skip_directive(lexer *lx) {
    for (;;) {
        int c = lx_peek(lx);
        if (c < 0) {
            return;
        }
        if (c == '\n') {
            (void)lx_get(lx);
            return;
        }
        if (c == '/') {
            size_t save = lx->pos;
            int64_t sline = lx->line;
            size_t sls = lx->line_start;
            (void)lx_get(lx);
            int d = lx_peek(lx);
            if (d == '*') {
                (void)lx_get(lx);
                if (!skip_block_comment(lx)) {
                    return;
                }
                continue;
            }
            if (d == '/') {
                skip_line_comment(lx);
                continue;
            }
            lx->pos = save;
            lx->line = sline;
            lx->line_start = sls;
        }
        (void)lx_get(lx);
    }
}

/* Reads an angle-bracketed include spelling from the raw bytes.
 *
 * `<stdio.h>` does not tokenize as one thing — it is `<`, `stdio`, `.`, `h`,
 * `>` — so the spelling is taken from the bytes directly, which is also what the
 * standard says a header-name is. */
static bool read_angle_spelling(lexer *lx, const unsigned char **out, size_t *out_len) {
    lx_skip_splices(lx);
    if (lx_peek(lx) != '<') {
        return false;
    }
    (void)lx_get(lx);
    size_t start = lx->pos;
    for (;;) {
        int c = lx_peek(lx);
        if (c < 0 || c == '\n') {
            return false;
        }
        if (c == '>') {
            *out = lx->p + start;
            *out_len = lx->pos - start;
            (void)lx_get(lx);
            return true;
        }
        (void)lx_get(lx);
    }
}

/* --- the extractor ----------------------------------------------------------- */

typedef struct extract_state {
    lexer lx;
    atlas_code_parse *p;
    run_state run;

    int brace_depth;
    int cond_depth;
    /* A leading `#ifndef X` / `#define X` pair whose `#endif` closes the file is
     * an include guard, and counting it as a conditional would mark every fact
     * in every header CONDITIONAL — which is true in the letter and useless in
     * practice. It is discounted, and `include_guard` reports that it was, so
     * the choice is visible rather than silent. */
    int guard_stage; /* 0 looking, 1 saw #ifndef first, 2 confirmed, 3 not a guard */
    char guard_name[ATLAS_CODE_MAX_NAME_BYTES + 1u];
    size_t guard_name_len;

    /* Function body state. */
    bool in_function;
    int32_t function_index;
    int function_brace_depth;

    /* Enum body state. */
    bool in_enum;
    int enum_brace_depth;
    bool enum_elem_start;

    /* Aggregate body state, so a `typedef struct { ... } NAME;` keeps its run
     * open across the braces. */
    bool in_aggregate;
    int aggregate_brace_depth;

    /* The two previous tokens, which is exactly what distinguishing `a->f(` from
     * `a > f(` needs: `->` is lexed as two punctuation tokens, and treating a
     * lone `>` as a member access would silently drop every call written after a
     * comparison. */
    token prev;
    token prev2;
    bool have_prev;
    bool have_prev2;
} extract_state;

static void push_prev(extract_state *es, const token *t) {
    es->prev2 = es->prev;
    es->have_prev2 = es->have_prev;
    es->prev = *t;
    es->have_prev = true;
}

static bool prev_is_member_access(const extract_state *es) {
    if (!es->have_prev || es->prev.kind != TOK_PUNCT) {
        return false;
    }
    if (es->prev.ch == '.') {
        return true;
    }
    return es->prev.ch == '>' && es->have_prev2 && es->prev2.kind == TOK_PUNCT &&
           es->prev2.ch == '-';
}

static int effective_cond_depth(const extract_state *es) {
    if (es->guard_stage == 2 && es->cond_depth > 0) {
        return es->cond_depth - 1;
    }
    return es->cond_depth;
}

static atlas_code_resolution here(const extract_state *es) {
    /* Everything below an unevaluated conditional is CONDITIONAL and nothing
     * else. Atlas does not run the preprocessor, so it cannot say whether the
     * branch is taken, and saying SOURCE_EXACT would be claiming it does. */
    return effective_cond_depth(es) > 0 ? ATLAS_CODE_RES_CONDITIONAL
                                        : ATLAS_CODE_RES_SOURCE_EXACT;
}

static atlas_code_linkage run_linkage(const run_state *r) {
    if (r->is_static) {
        return ATLAS_CODE_LINK_INTERNAL;
    }
    return ATLAS_CODE_LINK_EXTERNAL;
}

/* One preprocessor directive. */
static atlas_status do_directive(extract_state *es, atlas_err *err) {
    lexer *lx = &es->lx;
    atlas_status st = ATLAS_OK;
    token t;
    next_token(lx, &t);
    if (t.kind != TOK_IDENT) {
        skip_directive(lx);
        return ATLAS_OK;
    }
    /* De-spliced before it is compared, so `#inc\<newline>lude` is an include. */
    unsigned char dbuf[TOK_BUF_BYTES];
    size_t dn = 0;
    const unsigned char *d = tok_text(lx, &t, dbuf, sizeof(dbuf), &dn);

    if (str_eq(d, dn, "include") || str_eq(d, dn, "include_next") || str_eq(d, dn, "import")) {
        int64_t line = t.line;
        int64_t col = t.col;
        const unsigned char *sp = NULL;
        size_t spn = 0;
        lx_skip_splices(lx);
        int c = lx_peek(lx);
        while (c == ' ' || c == '\t') {
            (void)lx_get(lx);
            c = lx_peek(lx);
        }
        unsigned char sbuf[TOK_BUF_BYTES];
        if (c == '"') {
            token s;
            next_token(lx, &s);
            if (s.kind == TOK_STRING && s.len >= 2u) {
                size_t tn = 0;
                const unsigned char *txt = tok_text(lx, &s, sbuf, sizeof(sbuf), &tn);
                if (tn >= 2u) {
                    sp = txt + 1u;
                    spn = tn - 2u;
                }
            }
            st = add_include(es->p, sp, spn, ATLAS_CODE_INCLUDE_QUOTE, here(es), line, col, err);
        } else if (c == '<') {
            const unsigned char *raw = NULL;
            size_t rawn = 0;
            if (read_angle_spelling(lx, &raw, &rawn)) {
                sp = desplice(raw, rawn, sbuf, sizeof(sbuf), &spn);
                st = add_include(es->p, sp, spn, ATLAS_CODE_INCLUDE_ANGLE, here(es), line, col,
                                 err);
            }
        }
        /* A computed include — `#include MACRO` — resolves to nothing a lexical
         * reader can know, so nothing is recorded and the file is partial. */
        else if (c >= 0 && c != '\n') {
            note_truncated(es->p, ATLAS_CODE_WHY_INDIRECT);
        }
        skip_directive(lx);
        if (es->guard_stage == 0) {
            es->guard_stage = 3;
        }
        return st;
    }

    if (str_eq(d, dn, "define")) {
        token n;
        next_token(lx, &n);
        if (n.kind == TOK_IDENT && !n.oversize) {
            unsigned char nbuf[TOK_BUF_BYTES];
            size_t nn = 0;
            const unsigned char *name = tok_text(lx, &n, nbuf, sizeof(nbuf), &nn);
            /* Function-like exactly when the `(` touches the name. `#define A (x)`
             * is an object-like macro whose replacement happens to start with a
             * parenthesis, and treating it as function-like would be wrong in
             * the one direction that produces false call edges. */
            bool fnlike = (lx->pos < lx->len && lx->p[lx->pos] == '(');
            if (es->guard_stage == 1 && es->guard_name_len == nn &&
                memcmp(name, es->guard_name, nn) == 0) {
                es->guard_stage = 2;
                es->p->include_guard = true;
            } else if (es->guard_stage == 0) {
                es->guard_stage = 3;
            }
            st = add_symbol(es->p, name, nn,
                            fnlike ? ATLAS_CODE_SYM_MACRO_FUNCTION : ATLAS_CODE_SYM_MACRO,
                            ATLAS_CODE_LINK_NONE, here(es), true, true, n.line, n.col,
                            (int64_t)n.off, -1, NULL, err);
        }
        /* The replacement text is skipped whole. That is the difference between
         * a scanner that works and one that opens a parenthesis on
         * `#define OPEN (` and never closes it. */
        skip_directive(lx);
        return st;
    }

    if (str_eq(d, dn, "ifndef") && es->guard_stage == 0 && es->p->symbol_count == 0 &&
        es->p->include_count == 0) {
        token n;
        next_token(lx, &n);
        unsigned char nbuf[TOK_BUF_BYTES];
        size_t nn = 0;
        const unsigned char *name =
            (n.kind == TOK_IDENT) ? tok_text(lx, &n, nbuf, sizeof(nbuf), &nn) : NULL;
        if (name != NULL && nn > 0 && nn <= ATLAS_CODE_MAX_NAME_BYTES) {
            es->guard_stage = 1;
            memcpy(es->guard_name, name, nn);
            es->guard_name[nn] = '\0';
            es->guard_name_len = nn;
        } else {
            es->guard_stage = 3;
        }
        es->cond_depth++;
        skip_directive(lx);
        return ATLAS_OK;
    }

    if (str_eq(d, dn, "if") || str_eq(d, dn, "ifdef") || str_eq(d, dn, "ifndef")) {
        if (es->guard_stage == 0) {
            es->guard_stage = 3;
        }
        es->cond_depth++;
        if (es->cond_depth > (int)ATLAS_CODE_MAX_NESTING_DEPTH) {
            note_truncated(es->p, ATLAS_CODE_WHY_TRUNCATED);
        }
        skip_directive(lx);
        return ATLAS_OK;
    }
    if (str_eq(d, dn, "endif")) {
        if (es->cond_depth > 0) {
            es->cond_depth--;
        }
        skip_directive(lx);
        return ATLAS_OK;
    }
    if (str_eq(d, dn, "pragma")) {
        skip_directive(lx);
        if (es->guard_stage == 0) {
            /* `#pragma once` is a guard by another name, and it costs no
             * conditional level, so nothing has to be discounted. */
            es->guard_stage = 3;
        }
        return ATLAS_OK;
    }
    /* else, elif, undef, line, error, warning and anything else: no structure to
     * take from them, and no depth change except the ones handled above. */
    if (es->guard_stage == 0) {
        es->guard_stage = 3;
    }
    skip_directive(lx);
    return ATLAS_OK;
}

/* Records whatever a run turned out to be, at its terminating `;`. */
static atlas_status finish_run_semicolon(extract_state *es, atlas_err *err) {
    run_state *r = &es->run;
    atlas_code_parse *p = es->p;
    atlas_status st = ATLAS_OK;

    if (r->tag.have && r->have_tag_kind) {
        /* `struct X { ... };` records the tag; `struct X;` records it as a
         * declaration. Both are recorded because a forward declaration is a real
         * fact about a file. */
        st = add_symbol(p, r->tag.text, r->tag.len, r->tag_kind, ATLAS_CODE_LINK_EXTERNAL,
                        here(es), r->saw_body, true, r->tag.line, r->tag.col, r->tag.off, -1, NULL,
                        err);
        if (st != ATLAS_OK) {
            return st;
        }
    }

    if (r->has_typedef) {
        if (r->last.have) {
            st = add_symbol(p, r->last.text, r->last.len, ATLAS_CODE_SYM_TYPEDEF,
                            ATLAS_CODE_LINK_NONE, here(es), true, true, r->last.line, r->last.col,
                            r->last.off, -1, NULL, err);
        } else {
            /* A typedef whose name could not be identified is exactly the case
             * where guessing would be worse than admitting it. */
            note_truncated(p, ATLAS_CODE_WHY_TRUNCATED);
            if (p->status == ATLAS_CODE_PARSE_OK) {
                p->status = ATLAS_CODE_PARSE_PARTIAL;
            }
        }
        return st;
    }

    if (r->name.have) {
        /* A declarator followed by `;` is a prototype: a declaration, never a
         * definition. That distinction is the whole reason declarations and
         * definitions are separate columns. */
        return add_symbol(p, r->name.text, r->name.len, ATLAS_CODE_SYM_FUNCTION, run_linkage(r),
                          here(es), false, true, r->name.line, r->name.col, r->name.off, -1, NULL,
                          err);
    }

    if (r->last.have) {
        /* A file-scope object. `extern` makes it a declaration only; anything
         * else is a tentative definition, which is as much as C itself says.
         *
         * A tag introducer does not suppress this: `enum Color { RED } c;`
         * declares both the enum and the object `c`, and the tag identifier was
         * consumed when it was recognised, so it cannot arrive here pretending
         * to be one. */
        return add_symbol(p, r->last.text, r->last.len, ATLAS_CODE_SYM_VARIABLE, run_linkage(r),
                          here(es), !r->is_extern, true, r->last.line, r->last.col, r->last.off, -1,
                          NULL, err);
    }
    return ATLAS_OK;
}

atlas_status atlas_code_extract(const void *data, size_t len, atlas_code_language lang,
                                atlas_code_parse *out, atlas_err *err) {
    atlas_code_parse_init(out);
    if (lang == ATLAS_CODE_LANG_NONE) {
        out->status = ATLAS_CODE_PARSE_SKIPPED;
        out->truncated_reason = "not a language Atlas extracts structure from";
        return ATLAS_OK;
    }
    if (data == NULL || len == 0) {
        out->status = ATLAS_CODE_PARSE_OK;
        return ATLAS_OK;
    }

    /* A NUL in the first sniff window means this is not source, whatever its
     * extension says. Parsing it would produce garbage symbols that look exactly
     * like real ones. */
    const unsigned char *bytes = (const unsigned char *)data;
    size_t sniff = len < 8000u ? len : 8000u;
    for (size_t i = 0; i < sniff; i++) {
        if (bytes[i] == 0) {
            out->status = ATLAS_CODE_PARSE_SKIPPED;
            out->truncated_reason = "the file contains a NUL byte and was treated as binary";
            out->bytes = (int64_t)len;
            return ATLAS_OK;
        }
    }

    size_t use = len;
    if (use > ATLAS_CODE_MAX_FILE_BYTES) {
        use = ATLAS_CODE_MAX_FILE_BYTES;
        note_truncated(out, ATLAS_CODE_WHY_TRUNCATED);
        out->truncated_reason = "the file exceeds the structural parse ceiling";
    }

    extract_state es;
    memset(&es, 0, sizeof(es));
    es.p = out;
    es.lx.p = bytes;
    es.lx.len = use;
    es.lx.line = 1;
    es.lx.at_line_start = true;
    es.function_index = -1;
    run_reset(&es.run);

    atlas_status st = ATLAS_OK;
    for (;;) {
        token t;
        next_token(&es.lx, &t);
        if (t.kind == TOK_EOF) {
            break;
        }
        if (out->status == ATLAS_CODE_PARSE_FAILED) {
            break;
        }

        if (t.kind == TOK_PUNCT && t.ch == '#' && t.line_start) {
            st = do_directive(&es, err);
            if (st != ATLAS_OK) {
                break;
            }
            /* A directive between declarations does not end a run — an `#ifdef`
             * inside a struct body is ordinary — so the run state is left
             * exactly as it was. */
            continue;
        }

        run_state *r = &es.run;

        if (t.kind == TOK_PUNCT) {
            switch (t.ch) {
            case '(':
                r->paren_depth++;
                if (r->paren_depth == 1 && es.brace_depth == 0) {
                    /* A parenthesised declarator — `(*name)` — opens with `*`;
                     * a parameter list does not. One token of lookahead is all
                     * that separates `typedef int (*cb)(void *ctx)` naming `cb`
                     * from it naming `ctx`. */
                    size_t save = es.lx.pos;
                    int64_t sline = es.lx.line;
                    size_t sls = es.lx.line_start;
                    bool sstart = es.lx.at_line_start;
                    token nx;
                    next_token(&es.lx, &nx);
                    r->in_declarator_paren = (nx.kind == TOK_PUNCT && nx.ch == '*');
                    es.lx.pos = save;
                    es.lx.line = sline;
                    es.lx.line_start = sls;
                    es.lx.at_line_start = sstart;
                }
                break;
            case ')':
                if (r->paren_depth > 0) {
                    r->paren_depth--;
                }
                if (r->paren_depth == 0) {
                    r->in_declarator_paren = false;
                }
                break;
            case '=':
                if (r->paren_depth == 0) {
                    r->saw_equals = true;
                }
                break;
            case '{':
                es.brace_depth++;
                if (es.brace_depth > (int)ATLAS_CODE_MAX_NESTING_DEPTH) {
                    note_truncated(out, ATLAS_CODE_WHY_TRUNCATED);
                    out->status = ATLAS_CODE_PARSE_PARTIAL;
                }
                if (es.brace_depth == 1) {
                    r->saw_body = true;
                    if (r->name.have && !r->has_typedef) {
                        /* A declarator followed by `{` is a definition. */
                        int32_t idx = -1;
                        st = add_symbol(out, r->name.text, r->name.len, ATLAS_CODE_SYM_FUNCTION,
                                        run_linkage(r), here(&es), true, false, r->name.line,
                                        r->name.col, r->name.off, -1, &idx, err);
                        if (st != ATLAS_OK) {
                            break;
                        }
                        es.in_function = true;
                        es.function_index = idx;
                        es.function_brace_depth = 1;
                    } else if (r->have_tag_kind) {
                        es.in_aggregate = true;
                        es.aggregate_brace_depth = 1;
                        if (r->tag_kind == ATLAS_CODE_SYM_ENUM) {
                            es.in_enum = true;
                            es.enum_brace_depth = 1;
                            es.enum_elem_start = true;
                        }
                    } else if (!r->saw_equals) {
                        /* Something at file scope opened a block and Atlas could
                         * not say what. Nothing else in C does: a function has a
                         * declarator, an aggregate has an introducer, and an
                         * initialiser has an `=`. Reported rather than guessed
                         * at, because a symbol invented here would be
                         * indistinguishable from one that was really there. */
                        note_truncated(out, "a file-scope construct was not recognised");
                        if (out->status == ATLAS_CODE_PARSE_OK) {
                            out->status = ATLAS_CODE_PARSE_PARTIAL;
                        }
                    }
                }
                break;
            case '}':
                if (es.brace_depth > 0) {
                    es.brace_depth--;
                }
                if (es.in_function && es.brace_depth < es.function_brace_depth) {
                    if (es.function_index >= 0 &&
                        (size_t)es.function_index < out->symbol_count) {
                        out->symbols[es.function_index].end_line = t.line;
                    }
                    es.in_function = false;
                    es.function_index = -1;
                    run_reset(r);
                    continue;
                }
                if (es.in_enum && es.brace_depth < es.enum_brace_depth) {
                    es.in_enum = false;
                }
                if (es.in_aggregate && es.brace_depth < es.aggregate_brace_depth) {
                    es.in_aggregate = false;
                    /* The run stays open: `typedef struct { ... } NAME;` has its
                     * name after the closing brace, and closing the run here
                     * would lose every anonymous-struct typedef in a codebase. */
                }
                break;
            case ';':
                if (es.brace_depth == 0) {
                    st = finish_run_semicolon(&es, err);
                    run_reset(r);
                    if (st != ATLAS_OK) {
                        break;
                    }
                    continue;
                }
                break;
            case ',':
                if (es.in_enum && es.brace_depth == es.enum_brace_depth) {
                    es.enum_elem_start = true;
                }
                break;
            default: break;
            }
            if (st != ATLAS_OK) {
                break;
            }
            push_prev(&es, &t);
            continue;
        }

        if (t.kind != TOK_IDENT) {
            push_prev(&es, &t);
            continue;
        }

        /* De-spliced once, here, so every comparison and every stored name below
         * sees the identifier the compiler would see. */
        unsigned char idbuf[TOK_BUF_BYTES];
        size_t idn = 0;
        const unsigned char *id = tok_text(&es.lx, &t, idbuf, sizeof(idbuf), &idn);
        bool kw = is_keyword(id, idn);

        /* Enum constants: the identifier at the head of each element. */
        if (es.in_enum && es.brace_depth == es.enum_brace_depth && es.enum_elem_start && !kw &&
            !t.oversize) {
            st = add_symbol(out, id, idn, ATLAS_CODE_SYM_ENUM_CONSTANT, ATLAS_CODE_LINK_EXTERNAL,
                            here(&es), true, true, t.line, t.col, (int64_t)t.off, -1, NULL, err);
            es.enum_elem_start = false;
            if (st != ATLAS_OK) {
                break;
            }
            push_prev(&es, &t);
            continue;
        }

        if (es.in_function) {
            /* A call candidate: an identifier that is not a keyword, is followed
             * by `(`, and is not reached through a member access.
             *
             * The occurrence's existence is exact. What it refers to is not, and
             * this file deliberately says nothing about that — the resolver does,
             * with its own weaker classes. */
            bool member = prev_is_member_access(&es);
            if (!kw && !t.oversize && !member && !is_not_a_call(id, idn)) {
                size_t save = es.lx.pos;
                int64_t sline = es.lx.line;
                size_t sls = es.lx.line_start;
                bool sstart = es.lx.at_line_start;
                token nx;
                next_token(&es.lx, &nx);
                bool is_call = (nx.kind == TOK_PUNCT && nx.ch == '(');
                es.lx.pos = save;
                es.lx.line = sline;
                es.lx.line_start = sls;
                es.lx.at_line_start = sstart;
                if (is_call) {
                    st = add_occurrence(out, id, idn, es.function_index, here(&es), t.line, t.col,
                                        (int64_t)t.off, err);
                    if (st != ATLAS_OK) {
                        break;
                    }
                }
            }
            push_prev(&es, &t);
            continue;
        }

        /* Below file scope — inside a struct, union or enum body, or inside an
         * initialiser — nothing contributes to the run. A member called `a` is
         * not a file-scope object, and letting it become the run's "last
         * identifier" is how `struct X { int a; };` comes to record a variable
         * named `a` that does not exist. */
        if (es.brace_depth > 0) {
            push_prev(&es, &t);
            continue;
        }

        /* File scope: build the run. */
        if (kw) {
            if (str_eq(id, idn, "typedef") && r->paren_depth == 0) {
                r->has_typedef = true;
            } else if (str_eq(id, idn, "static") && r->paren_depth == 0) {
                r->is_static = true;
            } else if (str_eq(id, idn, "extern") && r->paren_depth == 0) {
                r->is_extern = true;
            } else if (r->paren_depth == 0 &&
                       (str_eq(id, idn, "struct") || str_eq(id, idn, "union") ||
                        str_eq(id, idn, "enum"))) {
                r->have_tag_kind = true;
                r->tag_kind = str_eq(id, idn, "struct")
                                  ? ATLAS_CODE_SYM_STRUCT
                                  : (str_eq(id, idn, "union") ? ATLAS_CODE_SYM_UNION
                                                              : ATLAS_CODE_SYM_ENUM);
                /* The tag, when there is one, is the identifier that follows —
                 * and it is *consumed* rather than peeked at.
                 *
                 * Peeking and restoring would leave the tag to be read again as
                 * an ordinary identifier, which would make it the run's "last
                 * identifier" as well. `struct X;` would then record both a
                 * struct declaration and a file-scope variable, both called X,
                 * and only one of them would exist. */
                size_t save = es.lx.pos;
                int64_t sline = es.lx.line;
                size_t sls = es.lx.line_start;
                bool sstart = es.lx.at_line_start;
                token nx;
                next_token(&es.lx, &nx);
                unsigned char tbuf[TOK_BUF_BYTES];
                size_t tn = 0;
                const unsigned char *tag =
                    (nx.kind == TOK_IDENT) ? tok_text(&es.lx, &nx, tbuf, sizeof(tbuf), &tn) : NULL;
                if (tag != NULL && !is_keyword(tag, tn) && !nx.oversize) {
                    name_set(&r->tag, tag, tn, &nx);
                } else {
                    es.lx.pos = save;
                    es.lx.line = sline;
                    es.lx.line_start = sls;
                    es.lx.at_line_start = sstart;
                }
            }
            push_prev(&es, &t);
            continue;
        }

        if (!t.oversize) {
            /* Eligible to name the declaration only at parenthesis depth zero,
             * or inside a parenthesised declarator group. An identifier in a
             * parameter list names a parameter, and `typedef int (*cb)(void
             * *ctx);` would otherwise define a type called `ctx`. */
            if (r->paren_depth == 0 || r->in_declarator_paren) {
                name_set(&r->last, id, idn, &t);
            }

            if (!r->name.have && r->paren_depth == 0) {
                /* The first non-keyword identifier immediately before a `(` at
                 * parenthesis depth zero is the declarator name. Looking ahead
                 * one token is the whole rule; anything more elaborate starts
                 * being a parser, and a parser that is nearly right is worse
                 * than a rule that knows when it does not apply. */
                size_t save = es.lx.pos;
                int64_t sline = es.lx.line;
                size_t sls = es.lx.line_start;
                bool sstart = es.lx.at_line_start;
                token nx;
                next_token(&es.lx, &nx);
                if (nx.kind == TOK_PUNCT && nx.ch == '(') {
                    name_set(&r->name, id, idn, &t);
                }
                es.lx.pos = save;
                es.lx.line = sline;
                es.lx.line_start = sls;
                es.lx.at_line_start = sstart;
            }
        } else {
            note_truncated(out, "an identifier exceeded the structural token ceiling");
        }
        push_prev(&es, &t);
    }

    out->bytes = (int64_t)len;
    out->lines = es.lx.line;
    if (st != ATLAS_OK) {
        /* Running out of memory is the only thing that can fail in here, and it
         * is not a property of the input, so it is reported as a status rather
         * than folded into `partial`. Whatever was extracted before it is left
         * in place: the caller frees the result either way, and a partially
         * filled result is easier to reason about than a half-freed one. */
        out->status = ATLAS_CODE_PARSE_FAILED;
        out->truncated = true;
        if (out->truncated_reason == NULL) {
            out->truncated_reason = "the structural parse could not complete";
        }
        return st;
    }
    if (es.lx.damaged && out->status == ATLAS_CODE_PARSE_OK) {
        out->status = ATLAS_CODE_PARSE_PARTIAL;
        if (out->truncated_reason == NULL) {
            out->truncated_reason = "an unterminated comment or literal was found";
        }
    }
    return ATLAS_OK;
}

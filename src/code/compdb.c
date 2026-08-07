/* Atlas - compile_commands.json, read as data.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A compile database is a build system's description of how it invokes a
 * compiler. It is written by a tool, it lives in a repository, and it is
 * therefore untrusted input in exactly the way a commit message is — with one
 * extra property that makes it worse: every entry contains something that looks
 * like a command line.
 *
 * So the rules here are absolute rather than careful:
 *
 *   - **Nothing is executed.** There is no `atlas_proc_run` in this file, no
 *     `exec`, no argv construction. A reviewer can establish that by reading it.
 *   - **The `command` string is not even stored.** Its presence and a SHA-256 of
 *     it are recorded, which is enough to notice that a build line changed and
 *     incapable of being run. A value nothing holds is a value nothing can
 *     accidentally pass somewhere.
 *   - **`arguments` is walked with a positive allowlist.** Anything not on it —
 *     `-include`, `-fplugin=`, `@response-file`, the compiler's own name — is
 *     counted in `dropped_args` and otherwise ignored. Response files are never
 *     opened.
 *   - **Paths are normalised lexically and checked against the repository.** No
 *     symlink is followed and nothing is stat'ed, because resolution must not
 *     depend on what the filesystem looks like at parse time. A `file` outside
 *     the repository is dropped; an include directory outside it is kept with
 *     `external` set and is **never opened** by anything.
 *
 * The document is parsed through `atlas/jsonread.h`, the one facade over the
 * vendored parser, so "where does Atlas parse untrusted JSON?" stays a directory
 * rather than a grep.
 */
#include "atlas/code.h"

#include <stdlib.h>
#include <string.h>

#include "atlas/jsonread.h"
#include "atlas/limits.h"
#include "atlas/sha256.h"

/* --- the result object ------------------------------------------------------ */

void atlas_code_compdb_init(atlas_code_compdb *c) {
    memset(c, 0, sizeof(*c));
    atlas_buf_init(&c->arena);
}

void atlas_code_compdb_free(atlas_code_compdb *c) {
    if (c == NULL) {
        return;
    }
    atlas_buf_free(&c->arena);
    free(c->units);
    free(c->incdirs);
    free(c->defines);
    c->units = NULL;
    c->incdirs = NULL;
    c->defines = NULL;
    c->unit_count = 0;
    c->incdir_count = 0;
    c->define_count = 0;
}

const char *atlas_code_compdb_str(const atlas_code_compdb *c, uint32_t off) {
    if (c->arena.data == NULL || off >= c->arena.len) {
        return "";
    }
    return c->arena.data + off;
}

static atlas_status cd_intern(atlas_code_compdb *c, const void *s, size_t n, uint32_t *off_out,
                              uint32_t *len_out, atlas_err *err) {
    if (c->arena.len == 0) {
        atlas_status st = atlas_buf_append_ch(&c->arena, '\0', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    if (s == NULL || n == 0) {
        *off_out = 0;
        *len_out = 0;
        return ATLAS_OK;
    }
    *off_out = (uint32_t)c->arena.len;
    *len_out = (uint32_t)n;
    atlas_status st = atlas_buf_append(&c->arena, s, n, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&c->arena, '\0', err);
    }
    return st;
}

static atlas_status cd_grow(void **items, size_t *cap, size_t count, size_t elem, size_t max,
                            atlas_err *err) {
    if (count < *cap) {
        return ATLAS_OK;
    }
    size_t next = (*cap == 0) ? 64u : *cap * 2u;
    if (next > max) {
        next = max;
    }
    if (next <= count) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "compile-database array ceiling reached");
    }
    void *grown = realloc(*items, next * elem);
    if (grown == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "out of memory reading a compile database");
    }
    *items = grown;
    *cap = next;
    return ATLAS_OK;
}

/* --- lexical path normalisation ---------------------------------------------
 *
 * Deliberately lexical: no symlink is followed, nothing is stat'ed, no `realpath`
 * is called. Resolution must not depend on what the filesystem looks like at
 * parse time, and a compile database that names a path through a symlink must
 * not be able to steer a later read anywhere — which it cannot, because nothing
 * in Atlas opens a path that came from here. */

static atlas_status normalise_into(const char *base, size_t base_len, const char *path,
                                   size_t path_len, atlas_buf *out, atlas_err *err) {
    atlas_buf joined = ATLAS_BUF_INIT;
    atlas_status st = ATLAS_OK;
    if (path_len > 0 && path[0] == '/') {
        st = atlas_buf_set(&joined, path, path_len, err);
    } else {
        if (base_len > 0) {
            st = atlas_buf_set(&joined, base, base_len, err);
            if (st == ATLAS_OK && (base_len == 0 || base[base_len - 1u] != '/')) {
                st = atlas_buf_append_ch(&joined, '/', err);
            }
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append(&joined, path, path_len, err);
        }
    }
    if (st != ATLAS_OK) {
        atlas_buf_free(&joined);
        return st;
    }

    /* Fold the components. `..` pops; `.` and empty components vanish. A `..`
     * with nothing to pop is dropped rather than escaping upward, which keeps a
     * hostile `../../../etc` inside whatever it started under. */
    atlas_buf_reset(out);
    bool absolute = joined.len > 0 && joined.data[0] == '/';
    size_t stack[256];
    size_t lens[256];
    size_t depth = 0;
    size_t i = 0;
    while (i < joined.len) {
        while (i < joined.len && joined.data[i] == '/') {
            i++;
        }
        size_t start = i;
        while (i < joined.len && joined.data[i] != '/') {
            i++;
        }
        size_t n = i - start;
        if (n == 0) {
            continue;
        }
        if (n == 1 && joined.data[start] == '.') {
            continue;
        }
        if (n == 2 && joined.data[start] == '.' && joined.data[start + 1u] == '.') {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if (depth >= sizeof(stack) / sizeof(stack[0])) {
            atlas_buf_free(&joined);
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY,
                                 "a compile-database path has more components than Atlas will "
                                 "normalise");
        }
        stack[depth] = start;
        lens[depth] = n;
        depth++;
    }
    if (absolute) {
        st = atlas_buf_append_ch(out, '/', err);
    }
    for (size_t k = 0; st == ATLAS_OK && k < depth; k++) {
        if (k > 0) {
            st = atlas_buf_append_ch(out, '/', err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append(out, joined.data + stack[k], lens[k], err);
        }
    }
    atlas_buf_free(&joined);
    return st;
}

/* True when `abs` is the repository root or lies inside it. On success
 * `*rel_out`/`*rel_len_out` point into `abs` at the repository-relative part. */
static bool inside_root(const atlas_buf *abs, const void *root, size_t root_len,
                        const char **rel_out, size_t *rel_len_out) {
    if (abs->len < root_len || memcmp(abs->data, root, root_len) != 0) {
        return false;
    }
    if (abs->len == root_len) {
        *rel_out = "";
        *rel_len_out = 0;
        return true;
    }
    if (abs->data[root_len] != '/') {
        return false; /* a sibling directory whose name starts the same way */
    }
    *rel_out = abs->data + root_len + 1u;
    *rel_len_out = abs->len - root_len - 1u;
    return true;
}

/* --- the argument allowlist -------------------------------------------------- */

static bool arg_is(const char *a, const char *want) {
    return strcmp(a, want) == 0;
}

static bool arg_starts(const char *a, const char *prefix, const char **rest) {
    size_t n = strlen(prefix);
    if (strncmp(a, prefix, n) != 0) {
        return false;
    }
    *rest = a + n;
    return true;
}

typedef struct unit_build {
    atlas_code_compdb *out;
    const void *root;
    size_t root_len;
    const char *directory;
    size_t directory_len;
    atlas_code_cu *cu;
} unit_build;

static atlas_status add_incdir(unit_build *ub, const char *dir, atlas_code_incdir_kind kind,
                               atlas_err *err) {
    atlas_code_compdb *c = ub->out;
    if (dir == NULL || dir[0] == '\0') {
        return ATLAS_OK;
    }
    size_t n = strlen(dir);
    if (n > ATLAS_CODE_MAX_ARG_BYTES) {
        ub->cu->dropped_args++;
        return ATLAS_OK;
    }
    if (ub->cu->incdir_count >= (size_t)ATLAS_CODE_MAX_INCLUDE_DIRS_PER_UNIT) {
        ub->cu->dropped_args++;
        return ATLAS_OK;
    }
    atlas_buf abs = ATLAS_BUF_INIT;
    atlas_status st = normalise_into(ub->directory, ub->directory_len, dir, n, &abs, err);
    if (st != ATLAS_OK) {
        /* A path Atlas will not normalise is dropped, not guessed at. */
        atlas_buf_free(&abs);
        atlas_err_init(err);
        ub->cu->dropped_args++;
        return ATLAS_OK;
    }
    const char *rel = NULL;
    size_t rel_len = 0;
    bool internal = inside_root(&abs, ub->root, ub->root_len, &rel, &rel_len);

    st = cd_grow((void **)&c->incdirs, &c->incdir_cap, c->incdir_count, sizeof(*c->incdirs),
                 (size_t)ATLAS_CODE_MAX_COMPILE_UNITS * 4u, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&abs);
        return st;
    }
    atlas_code_cu_incdir *d = &c->incdirs[c->incdir_count];
    memset(d, 0, sizeof(*d));
    /* An internal directory is stored repository-relative so it can be matched
     * against indexed paths. An external one keeps its absolute form, because
     * it is a statement about where a build looks and nothing more. */
    st = cd_intern(c, internal ? rel : abs.data, internal ? rel_len : abs.len, &d->path_off,
                   &d->path_len, err);
    atlas_buf_free(&abs);
    if (st != ATLAS_OK) {
        return st;
    }
    d->kind = (int32_t)kind;
    d->external = !internal;
    if (ub->cu->incdir_count == 0) {
        ub->cu->incdir_first = c->incdir_count;
    }
    ub->cu->incdir_count++;
    c->incdir_count++;
    return ATLAS_OK;
}

static atlas_status add_define(unit_build *ub, const char *text, bool undef, atlas_err *err) {
    atlas_code_compdb *c = ub->out;
    if (text == NULL || text[0] == '\0') {
        return ATLAS_OK;
    }
    size_t n = strlen(text);
    if (n > ATLAS_CODE_MAX_ARG_BYTES ||
        ub->cu->define_count >= (size_t)ATLAS_CODE_MAX_DEFINES_PER_UNIT) {
        ub->cu->dropped_args++;
        return ATLAS_OK;
    }
    const char *eq = memchr(text, '=', n);
    size_t name_len = eq != NULL ? (size_t)(eq - text) : n;
    const char *value = eq != NULL ? eq + 1 : NULL;
    size_t value_len = eq != NULL ? n - name_len - 1u : 0;
    if (name_len == 0) {
        ub->cu->dropped_args++;
        return ATLAS_OK;
    }

    atlas_status st = cd_grow((void **)&c->defines, &c->define_cap, c->define_count,
                              sizeof(*c->defines),
                              (size_t)ATLAS_CODE_MAX_COMPILE_UNITS * 8u, err);
    if (st != ATLAS_OK) {
        return st;
    }
    atlas_code_cu_define *d = &c->defines[c->define_count];
    memset(d, 0, sizeof(*d));
    st = cd_intern(c, text, name_len, &d->name_off, &d->name_len, err);
    if (st == ATLAS_OK && value != NULL) {
        st = cd_intern(c, value, value_len, &d->value_off, &d->value_len, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    d->undef = undef;
    if (ub->cu->define_count == 0) {
        ub->cu->define_first = c->define_count;
    }
    ub->cu->define_count++;
    c->define_count++;
    return ATLAS_OK;
}

/* Walks `arguments` with the allowlist.
 *
 * Everything the allowlist does not name is counted and dropped. That includes
 * the compiler's own path, the source file, `-include`, `-fplugin=` and any
 * `@response-file` — none of which Atlas acts on, and the last of which it
 * explicitly does not open. */
static atlas_status walk_arguments(unit_build *ub, const atlas_jsonv *args, atlas_buf *output_out,
                                   atlas_buf *std_out, atlas_buf *lang_out, atlas_err *err) {
    size_t n = atlas_jsonv_arr_len(args);
    if (n > (size_t)ATLAS_CODE_MAX_COMPILE_ARGS) {
        n = (size_t)ATLAS_CODE_MAX_COMPILE_ARGS;
        ub->out->truncated = true;
        ub->out->truncated_reason = "a compile-database entry has more arguments than Atlas walks";
    }
    ub->cu->arg_count = (int64_t)atlas_jsonv_arr_len(args);

    atlas_status st = ATLAS_OK;
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        const char *a = NULL;
        if (!atlas_jsonv_str(atlas_jsonv_at(args, i), &a, NULL)) {
            ub->cu->dropped_args++;
            continue;
        }
        const char *rest = NULL;

        /* Separated forms first: `-I dir` before `-Idir`, so the prefix match
         * below cannot swallow the exact match. */
        if (arg_is(a, "-I") || arg_is(a, "-iquote") || arg_is(a, "-isystem") ||
            arg_is(a, "-idirafter")) {
            atlas_code_incdir_kind k = arg_is(a, "-I")          ? ATLAS_CODE_INCDIR_SEARCH
                                       : arg_is(a, "-iquote")   ? ATLAS_CODE_INCDIR_QUOTE
                                       : arg_is(a, "-isystem")  ? ATLAS_CODE_INCDIR_SYSTEM
                                                                : ATLAS_CODE_INCDIR_AFTER;
            const char *v = NULL;
            if (i + 1u < n && atlas_jsonv_str(atlas_jsonv_at(args, i + 1u), &v, NULL)) {
                st = add_incdir(ub, v, k, err);
                i++;
            } else {
                ub->cu->dropped_args++;
            }
            continue;
        }
        if (arg_is(a, "-D") || arg_is(a, "-U")) {
            const char *v = NULL;
            if (i + 1u < n && atlas_jsonv_str(atlas_jsonv_at(args, i + 1u), &v, NULL)) {
                st = add_define(ub, v, arg_is(a, "-U"), err);
                i++;
            } else {
                ub->cu->dropped_args++;
            }
            continue;
        }
        if (arg_is(a, "-o")) {
            const char *v = NULL;
            if (i + 1u < n && atlas_jsonv_str(atlas_jsonv_at(args, i + 1u), &v, NULL) &&
                strlen(v) <= ATLAS_CODE_MAX_ARG_BYTES) {
                st = atlas_buf_set_str(output_out, v, err);
                i++;
            } else {
                ub->cu->dropped_args++;
            }
            continue;
        }
        if (arg_is(a, "-x")) {
            const char *v = NULL;
            if (i + 1u < n && atlas_jsonv_str(atlas_jsonv_at(args, i + 1u), &v, NULL) &&
                strlen(v) <= 32u) {
                st = atlas_buf_set_str(lang_out, v, err);
                i++;
            } else {
                ub->cu->dropped_args++;
            }
            continue;
        }
        if (arg_starts(a, "-I", &rest) && rest[0] != '\0') {
            st = add_incdir(ub, rest, ATLAS_CODE_INCDIR_SEARCH, err);
            continue;
        }
        if (arg_starts(a, "-iquote", &rest) && rest[0] != '\0') {
            st = add_incdir(ub, rest, ATLAS_CODE_INCDIR_QUOTE, err);
            continue;
        }
        if (arg_starts(a, "-isystem", &rest) && rest[0] != '\0') {
            st = add_incdir(ub, rest, ATLAS_CODE_INCDIR_SYSTEM, err);
            continue;
        }
        if (arg_starts(a, "-idirafter", &rest) && rest[0] != '\0') {
            st = add_incdir(ub, rest, ATLAS_CODE_INCDIR_AFTER, err);
            continue;
        }
        if (arg_starts(a, "-D", &rest) && rest[0] != '\0') {
            st = add_define(ub, rest, false, err);
            continue;
        }
        if (arg_starts(a, "-U", &rest) && rest[0] != '\0') {
            st = add_define(ub, rest, true, err);
            continue;
        }
        if (arg_starts(a, "-std=", &rest) && rest[0] != '\0' && strlen(rest) <= 32u) {
            st = atlas_buf_set_str(std_out, rest, err);
            continue;
        }
        if (arg_starts(a, "-o", &rest) && rest[0] != '\0' &&
            strlen(rest) <= ATLAS_CODE_MAX_ARG_BYTES) {
            st = atlas_buf_set_str(output_out, rest, err);
            continue;
        }
        /* Not on the allowlist. Counted so "the build had flags Atlas ignored"
         * is a number rather than a silence, and otherwise untouched. A
         * `@response-file` lands here and is never opened. */
        ub->cu->dropped_args++;
    }
    return st;
}

/* --- one entry ---------------------------------------------------------------- */

static atlas_status read_entry(atlas_code_compdb *c, const atlas_jsonv *entry, const void *root,
                               size_t root_len, size_t index, atlas_err *err) {
    c->entries_seen++;
    if (!atlas_jsonv_is_obj(entry)) {
        c->entries_dropped++;
        return ATLAS_OK;
    }
    const char *file = atlas_jsonv_str_member(entry, "file");
    if (file == NULL || file[0] == '\0') {
        c->entries_dropped++;
        return ATLAS_OK;
    }
    const char *directory = atlas_jsonv_str_member(entry, "directory");
    size_t dir_len = directory != NULL ? strlen(directory) : 0;
    if (dir_len > ATLAS_CODE_MAX_ARG_BYTES) {
        c->entries_dropped++;
        return ATLAS_OK;
    }

    atlas_buf abs = ATLAS_BUF_INIT;
    atlas_status st = normalise_into(directory, dir_len, file, strlen(file), &abs, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&abs);
        atlas_err_init(err);
        c->entries_dropped++;
        return ATLAS_OK;
    }
    const char *rel = NULL;
    size_t rel_len = 0;
    if (!inside_root(&abs, root, root_len, &rel, &rel_len) || rel_len == 0) {
        /* A translation unit outside the registered repository. Dropped with a
         * count rather than recorded: Atlas has nothing to say about a file it
         * does not index, and a unit pointing outside the tree is exactly the
         * shape a hostile compile database would take. */
        atlas_buf_free(&abs);
        c->entries_dropped++;
        return ATLAS_OK;
    }

    st = cd_grow((void **)&c->units, &c->unit_cap, c->unit_count, sizeof(*c->units),
                 (size_t)ATLAS_CODE_MAX_COMPILE_UNITS, err);
    if (st != ATLAS_OK) {
        atlas_buf_free(&abs);
        return st;
    }
    atlas_code_cu *cu = &c->units[c->unit_count];
    memset(cu, 0, sizeof(*cu));
    cu->entry_index = 0;
    st = cd_intern(c, rel, rel_len, &cu->source_off, &cu->source_len, err);
    atlas_buf_free(&abs);
    if (st != ATLAS_OK) {
        return st;
    }
    if (st == ATLAS_OK && directory != NULL) {
        st = cd_intern(c, directory, dir_len, &cu->dir_off, &cu->dir_len, err);
    }
    if (st != ATLAS_OK) {
        return st;
    }

    /* The command string, hashed and discarded.
     *
     * This is the only thing Atlas does with it. Storing the text would put a
     * shell command line in the index for no purpose Atlas has, and the hash
     * answers the one question that matters — did the build line change? */
    const char *command = atlas_jsonv_str_member(entry, "command");
    if (command != NULL) {
        cu->command_present = true;
        atlas_sha256_hex(command, strlen(command), cu->command_hash);
    }

    atlas_buf output = ATLAS_BUF_INIT;
    atlas_buf std = ATLAS_BUF_INIT;
    atlas_buf lang = ATLAS_BUF_INIT;
    const char *explicit_output = atlas_jsonv_str_member(entry, "output");
    if (explicit_output != NULL && strlen(explicit_output) <= ATLAS_CODE_MAX_ARG_BYTES) {
        st = atlas_buf_set_str(&output, explicit_output, err);
    }

    unit_build ub;
    memset(&ub, 0, sizeof(ub));
    ub.out = c;
    ub.root = root;
    ub.root_len = root_len;
    ub.directory = directory;
    ub.directory_len = dir_len;
    ub.cu = cu;

    const atlas_jsonv *args = atlas_jsonv_get(entry, "arguments");
    if (st == ATLAS_OK && args != NULL && atlas_jsonv_is_arr(args)) {
        st = walk_arguments(&ub, args, &output, &std, &lang, err);
    }
    if (st == ATLAS_OK) {
        st = cd_intern(c, output.data, output.len, &cu->output_off, &cu->output_len, err);
    }
    if (st == ATLAS_OK) {
        st = cd_intern(c, std.data, std.len, &cu->std_off, &cu->std_len, err);
    }
    if (st == ATLAS_OK) {
        st = cd_intern(c, lang.data, lang.len, &cu->lang_off, &cu->lang_len, err);
    }
    atlas_buf_free(&output);
    atlas_buf_free(&std);
    atlas_buf_free(&lang);
    if (st != ATLAS_OK) {
        return st;
    }
    cu->entry_index = (int64_t)index;
    c->unit_count++;
    return ATLAS_OK;
}

atlas_status atlas_code_compdb_parse(const void *data, size_t len, const void *root_raw,
                                     size_t root_len, atlas_code_compdb *out, atlas_err *err) {
    atlas_code_compdb_init(out);
    if (data == NULL || len == 0) {
        out->truncated_reason = "the compile database is empty";
        return ATLAS_OK;
    }
    if (len > ATLAS_CODE_MAX_COMPILE_DB_BYTES) {
        /* Checked before the parser is entered, so a claimed size can never
         * become an allocation. An oversized database is an ordinary outcome
         * with a reason, not a failure that takes a pass down. */
        out->truncated = true;
        out->truncated_reason = "the compile database exceeds the size Atlas will read";
        return ATLAS_OK;
    }

    atlas_jsondoc *doc = NULL;
    atlas_err perr;
    atlas_err_init(&perr);
    if (atlas_jsondoc_parse(data, len, ATLAS_CODE_MAX_COMPILE_DB_BYTES, ATLAS_IPC_MAX_JSON_DEPTH,
                            &doc, &perr) != ATLAS_OK) {
        /* Malformed is a fact about the repository, not an Atlas failure. Zero
         * units and a reason; the structural index carries on without build
         * metadata and says its resolution is lexical. */
        out->truncated = true;
        out->truncated_reason = "the compile database is not valid JSON";
        atlas_jsondoc_free(doc);
        return ATLAS_OK;
    }
    const atlas_jsonv *rootv = atlas_jsondoc_root(doc);
    if (!atlas_jsonv_is_arr(rootv)) {
        out->truncated = true;
        out->truncated_reason = "the compile database is not a JSON array";
        atlas_jsondoc_free(doc);
        return ATLAS_OK;
    }

    size_t n = atlas_jsonv_arr_len(rootv);
    atlas_status st = ATLAS_OK;
    for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
        if (out->unit_count >= (size_t)ATLAS_CODE_MAX_COMPILE_UNITS) {
            out->truncated = true;
            out->truncated_reason = "the compile database has more entries than Atlas records";
            break;
        }
        st = read_entry(out, atlas_jsonv_at(rootv, i), root_raw, root_len, i, err);
    }
    atlas_jsondoc_free(doc);
    return st;
}

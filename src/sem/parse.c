/* Atlas - spawning and reading the bounded translation-unit parser.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The parent half. `atlas_sem_parse_unit` builds an explicit argument vector,
 * runs it through `atlas_proc_run` — still the one process-creation path in
 * Atlas — and reads the child's single JSON document through the one yyjson
 * facade.
 *
 * What the argument vector is made of is the security property:
 *
 *   argv[0]  Atlas' own resolved absolute executable path.
 *   argv[1]  the literal "sem-parse", compiled in.
 *   the rest  option flags that are compiled-in literals, and *values* that are
 *            a source path already checked to be inside the repository root,
 *            the root itself, the compilation entry's directory, and the
 *            allowlisted compiler arguments.
 *
 * No element of it is chosen by the repository. A compile database contributes
 * values after `--`, never the program, never a flag Atlas did not write, and
 * never a string that is interpreted as more than one argument — this is A8's
 * rule that a validation command is a vector of counted arguments and never a
 * string, applied to compilation. There is no shell anywhere in this path and no
 * field that could hold a fragment of one.
 *
 * A unit that fails is an ordinary outcome. The only statuses this returns are
 * an allocation failure and an inability to create a process at all; everything
 * a hostile or broken translation unit can do arrives through `res`.
 */
#include "atlas/sem.h"

#include <stdlib.h>
#include <string.h>

#include "atlas/jsonread.h"
#include "atlas/limits.h"
#include "atlas/proc.h"

/* --- reading the child's document ------------------------------------------ */

static const char *str_or(const atlas_jsonv *obj, const char *key, const char *fallback) {
    const char *v = atlas_jsonv_str_member(obj, key);
    return v == NULL ? fallback : v;
}

static int64_t int_or(const atlas_jsonv *obj, const char *key, int64_t fallback) {
    const atlas_jsonv *v = atlas_jsonv_get(obj, key);
    int64_t out = 0;
    if (v != NULL && atlas_jsonv_int(v, &out)) {
        return out;
    }
    return fallback;
}

static bool bool_or(const atlas_jsonv *obj, const char *key, bool fallback) {
    const atlas_jsonv *v = atlas_jsonv_get(obj, key);
    bool out = false;
    if (v != NULL && atlas_jsonv_bool(v, &out)) {
        return out;
    }
    return fallback;
}

/* Turns the child's document into calls on `cb`.
 *
 * Every string handed to the callback is borrowed from the parsed document and
 * valid for the call only, which is the rule every row callback in Atlas
 * follows. Nothing here trusts the child to have produced a well-formed record:
 * a fact missing its identity is skipped, not guessed at. */
static atlas_status read_document(const char *data, size_t len, atlas_sem_fact_cb cb, void *ud,
                                  atlas_sem_parse_result *res, atlas_err *err) {
    atlas_jsondoc *doc = NULL;
    /* The document is two levels deep — an object holding one array of flat
     * objects — so the depth bound is small and generous rather than a guess. */
    atlas_status st = atlas_jsondoc_parse(data, len, ATLAS_SEM_PARSE_MAX_STDOUT, 8, &doc, err);
    if (st != ATLAS_OK) {
        res->status = ATLAS_SEM_TU_FAILED;
        res->why = ATLAS_SEM_WHY_CHILD_FAILED;
        /* A child that produced something which is not a result document is a
         * failed unit, not a failed pass — A8's rule that a zero exit is not a
         * success claim, and that the structural check is deliberately shallow
         * because a parser's output is never parsed as authority. */
        atlas_err_init(err);
        return ATLAS_OK;
    }

    const atlas_jsonv *root = atlas_jsondoc_root(doc);
    if (root == NULL || !atlas_jsonv_is_obj(root)) {
        atlas_jsondoc_free(doc);
        res->status = ATLAS_SEM_TU_FAILED;
        res->why = ATLAS_SEM_WHY_CHILD_FAILED;
        return ATLAS_OK;
    }

    atlas_sem_tu_status status = ATLAS_SEM_TU_UNKNOWN;
    if (!atlas_sem_tu_status_parse(str_or(root, "status", ""), &status)) {
        status = ATLAS_SEM_TU_UNKNOWN;
    }
    res->status = status;
    const char *why = atlas_jsonv_str_member(root, "why");
    /* A reason from the child is checked against Atlas' own closed vocabulary
     * before it is kept. The child is Atlas, but this value reaches an operator
     * and a model, and "it came from a process we started" is not the same
     * property as "it is one of the strings we defined". */
    res->why = atlas_sem_why_is_known(why) ? atlas_sem_why_intern(why) : NULL;
    res->diagnostics_errors = int_or(root, "errors", 0);
    res->truncated = bool_or(root, "truncated", false);

    const atlas_jsonv *facts = atlas_jsonv_get(root, "facts");
    if (facts != NULL && atlas_jsonv_is_arr(facts)) {
        size_t n = atlas_jsonv_arr_len(facts);
        for (size_t i = 0; i < n && st == ATLAS_OK; i++) {
            const atlas_jsonv *f = atlas_jsonv_at(facts, i);
            if (f == NULL || !atlas_jsonv_is_obj(f)) {
                continue;
            }
            const char *record = atlas_jsonv_str_member(f, "r");
            if (record == NULL) {
                continue;
            }

            atlas_sem_fact fact;
            memset(&fact, 0, sizeof(fact));

            if (strcmp(record, "s") == 0) {
                fact.record = "symbol";
                fact.usr = str_or(f, "usr", "");
                if (fact.usr[0] == '\0') {
                    continue; /* a symbol with no identity is not a symbol */
                }
                fact.name = str_or(f, "name", "");
                fact.kind = str_or(f, "kind", "UNKNOWN");
                fact.linkage = str_or(f, "link", "UNKNOWN");
                fact.type_text = str_or(f, "type", "");
                fact.file = str_or(f, "file", "");
                fact.line = int_or(f, "line", 0);
                fact.col = int_or(f, "col", 0);
                fact.end_line = int_or(f, "end", 0);
                fact.is_definition = bool_or(f, "def", false);
                fact.external = bool_or(f, "ext", false);
                res->symbols++;
            } else if (strcmp(record, "e") == 0) {
                fact.record = "edge";
                fact.kind = str_or(f, "kind", "UNKNOWN");
                fact.src_usr = str_or(f, "src", "");
                fact.dst_usr = str_or(f, "dst", "");
                fact.evidence = str_or(f, "ev", "UNKNOWN");
                fact.file = str_or(f, "file", "");
                fact.line = int_or(f, "line", 0);
                fact.col = int_or(f, "col", 0);
                fact.detail = str_or(f, "detail", "");
                res->edges++;
            } else if (strcmp(record, "i") == 0) {
                fact.record = "include";
                fact.include_from = str_or(f, "from", "");
                fact.include_to = str_or(f, "to", "");
                fact.dst_name = str_or(f, "spell", "");
                fact.evidence = str_or(f, "ev", "UNKNOWN");
                fact.line = int_or(f, "line", 0);
                if (fact.include_from[0] == '\0') {
                    continue;
                }
                res->includes++;
            } else {
                continue;
            }

            if (cb != NULL) {
                st = cb(&fact, ud, err);
            }
        }
    }

    atlas_jsondoc_free(doc);
    return st;
}

/* --- the parent ------------------------------------------------------------ */

atlas_status atlas_sem_parse_unit(const char *atlas_exe, const atlas_sem_parse_req *req,
                                  atlas_sem_fact_cb cb, void *ud, atlas_sem_parse_result *res,
                                  atlas_err *err) {
    if (atlas_exe == NULL || req == NULL || req->source == NULL || req->root == NULL ||
        res == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic parse: bad request");
    }
    memset(res, 0, sizeof(*res));
    res->status = ATLAS_SEM_TU_UNKNOWN;

    if (!atlas_sem_available()) {
        res->status = ATLAS_SEM_TU_UNSUPPORTED;
        res->why = ATLAS_SEM_WHY_ARG_REFUSED;
        return ATLAS_OK;
    }
    if (req->arg_count > ATLAS_CODE_MAX_COMPILE_ARGS) {
        res->status = ATLAS_SEM_TU_UNSUPPORTED;
        res->why = ATLAS_SEM_WHY_TOO_LARGE;
        return ATLAS_OK;
    }

    /* argv: exe, "sem-parse", --source S, --root R, [--directory D], "--", args...
     *
     * Built as an array of pointers. Every flag is a literal in this file; every
     * value is something Atlas validated. */
    size_t fixed = 2 /* exe, subcommand */ + 2 /* --source S */ + 2 /* --root R */ +
                   2 /* --directory D */ + 1 /* -- */;
    size_t total = fixed + req->arg_count + 1 /* NULL */;
    const char **argv = calloc(total, sizeof(*argv));
    if (argv == NULL) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "semantic parse: out of memory");
    }

    size_t n = 0;
    argv[n++] = atlas_exe;
    argv[n++] = "sem-parse";
    argv[n++] = "--source";
    argv[n++] = req->source;
    argv[n++] = "--root";
    argv[n++] = req->root;
    if (req->directory != NULL && req->directory[0] != '\0') {
        argv[n++] = "--directory";
        argv[n++] = req->directory;
    }
    argv[n++] = "--";
    for (size_t i = 0; i < req->arg_count; i++) {
        argv[n++] = req->args[i];
    }
    argv[n] = NULL;

    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    /* An empty environment. Nothing an operator or a repository set can reach
     * the compiler front end: no CPATH, no C_INCLUDE_PATH, no CCACHE_*, no
     * LD_PRELOAD. This is the same construction `ATLAS_GIT_ENV` makes for git,
     * and for the same reason — the search paths must be the ones the recorded
     * compilation configuration named, and nothing else. */
    opts.env = NULL;
    opts.timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : ATLAS_SEM_PARSE_TIMEOUT_MS;
    opts.idle_timeout_ms = ATLAS_SEM_PARSE_IDLE_MS;
    opts.max_stdout = ATLAS_SEM_PARSE_MAX_STDOUT;
    opts.max_address_space = ATLAS_SEM_PARSE_MAX_ADDRESS_SPACE;
    /* The child does not need a working directory: it resolves nothing
     * relatively, and every path it is given is absolute. Leaving this NULL
     * keeps Atlas' own working directory, which is what every other caller
     * does. */
    opts.cwd = NULL;

    atlas_buf out;
    atlas_buf_init(&out);
    atlas_buf stderr_buf;
    atlas_buf_init(&stderr_buf);
    atlas_proc_result pr;
    memset(&pr, 0, sizeof(pr));

    atlas_status st = atlas_proc_run(&opts, atlas_proc_sink_buf, &out, &stderr_buf, &pr, err);
    free(argv);

    if (st != ATLAS_OK) {
        atlas_buf_free(&out);
        atlas_buf_free(&stderr_buf);
        return st;
    }

    /* The child's stderr is captured and **discarded**, deliberately.
     *
     * It can only hold compiler noise, and compiler noise quotes untrusted
     * repository source. Atlas counts diagnostics and reports the count; it does
     * not relay their text, because a message that reaches an operator's
     * terminal or a model's context through Atlas' own reporting channel is a
     * message Atlas is vouching for. */
    atlas_buf_free(&stderr_buf);

    if (pr.timed_out || pr.idle_timed_out) {
        res->status = ATLAS_SEM_TU_FAILED;
        res->why = ATLAS_SEM_WHY_TIMEOUT;
        atlas_buf_free(&out);
        return ATLAS_OK;
    }
    if (pr.stdout_truncated) {
        res->status = ATLAS_SEM_TU_FAILED;
        res->why = ATLAS_SEM_WHY_TOO_LARGE;
        atlas_buf_free(&out);
        return ATLAS_OK;
    }
    if (pr.exit_code != 0 || pr.term_signal != 0 || out.len == 0) {
        /* Includes the case where the address-space ceiling killed the child.
         * One translation unit is lost and says so; the pass continues. */
        res->status = ATLAS_SEM_TU_FAILED;
        res->why = ATLAS_SEM_WHY_CHILD_FAILED;
        atlas_buf_free(&out);
        return ATLAS_OK;
    }

    st = read_document((const char *)out.data, out.len, cb, ud, res, err);
    atlas_buf_free(&out);
    return st;
}

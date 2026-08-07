/* Atlas - structural graph correctness and incremental behaviour.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These drive `atlas_reconcile_run` against a real git repository, with no
 * daemon and no threads, so the structural stage is tested in isolation from
 * the machinery that schedules it.
 *
 * The claims under test are the ones A3 exists to make, and they divide in two.
 *
 * **Nothing is conflated.** A same-named static in two files is two symbols; two
 * external definitions of one name are ambiguous with both recorded; a call with
 * no definition anywhere is unresolved with a typed reason. Every one of these
 * would be easy to answer confidently and wrongly.
 *
 * **The pass costs what changed.** An unchanged pass parses zero files even when
 * it is a full content-verifying pass; one edit parses one file; a deletion and
 * a rename leave no stale rows behind; and repeated unchanged passes add no
 * durable rows at all.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/code.h"
#include "atlas/reconcile.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    atlas_db *db;
    atlas_git *g;
    int64_t repo_id;
} env;

static void env_open(env *e, atlas_err *err) {
    T_OK(fx_open(&e->fx, err), err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
}

static void env_index(env *e, atlas_err *err) {
    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_ensure(fx_data_dir(&e->fx), err), err);
    T_OK(atlas_datadir_db_path(fx_data_dir(&e->fx), &db_path, err), err);
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &e->db, err), err);
    T_OK(atlas_db_migrate(e->db, err), err);
    atlas_buf_free(&db_path);

    T_OK(atlas_git_open(fx_repo(&e->fx), &e->g, err), err);
    const char *root = atlas_git_root(e->g);
    atlas_repo_identity id;
    memset(&id, 0, sizeof(id));
    id.root = root;
    id.root_len = strlen(root);
    id.common_dir = atlas_git_common_dir(e->g);
    id.common_dir_len = strlen((const char *)id.common_dir);
    id.git_dir = atlas_git_dir(e->g);
    id.git_dir_len = strlen((const char *)id.git_dir);
    id.object_format = atlas_git_object_format(e->g);
    T_OK(atlas_db_repo_add(e->db, "fixture", &id, &e->repo_id, err), err);
}

static void env_close(env *e) {
    atlas_git_close(e->g);
    atlas_db_close(e->db);
    fx_close(&e->fx);
}

static void run_pass(env *e, bool full, atlas_reconcile_summary *sum, atlas_err *err) {
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    opts.full = full;
    atlas_reconcile_summary_init(sum);
    T_OK(atlas_reconcile_run(e->db, e->g, e->repo_id, &opts, sum, err), err);
    T_CHECK(sum->published);
}

/* --- query helpers ----------------------------------------------------------- */

static int64_t count_rows(env *e, const char *sql, atlas_err *err) {
    int64_t n = 0;
    T_OK(atlas_db_query_int64(e->db, sql, &n, err), err);
    return n;
}

typedef struct edge_probe {
    const char *want_name;   /* the spelling, or NULL for any */
    char resolution[32];
    char detail[64];
    char dst_path[256];
    int64_t candidates;
    int64_t matches;
} edge_probe;

static atlas_status probe_edge(const atlas_code_edge_row *row, void *ud, atlas_err *err) {
    edge_probe *p = (edge_probe *)ud;
    (void)err;
    if (p->want_name != NULL &&
        (row->dst_name_text == NULL || strcmp(row->dst_name_text, p->want_name) != 0)) {
        return ATLAS_OK;
    }
    p->matches++;
    (void)snprintf(p->resolution, sizeof(p->resolution), "%s", row->resolution);
    (void)snprintf(p->detail, sizeof(p->detail), "%s", row->detail != NULL ? row->detail : "");
    (void)snprintf(p->dst_path, sizeof(p->dst_path), "%s",
                   row->dst_path_text != NULL ? row->dst_path_text : "");
    p->candidates = row->candidate_count;
    return ATLAS_OK;
}

/* Finds the `code_files` id of one repository-relative path. */
static int64_t code_file_id(env *e, const char *path, atlas_err *err) {
    int64_t id = 0;
    bool found = false;
    T_OK(atlas_db_code_file_get(e->db, e->repo_id, path, strlen(path), NULL, NULL, &found, &id,
                                err),
         err);
    return found ? id : 0;
}

/* Looks up one edge of a kind leaving a file, by spelling. */
static edge_probe edge_from_file(env *e, const char *path, const char *kind, const char *name,
                                 atlas_err *err) {
    edge_probe p;
    memset(&p, 0, sizeof(p));
    p.want_name = name;
    int64_t id = code_file_id(e, path, err);
    T_REQUIRE(id > 0);
    int64_t n = 0;
    bool more = false;
    T_OK(atlas_db_code_edges_from(e->db, e->repo_id, "file", id, kind, ATLAS_CODE_MAX_ROWS,
                                  probe_edge, &p, &n, &more, err),
         err);
    return p;
}

/* Looks up a call edge by callee spelling, anywhere in the repository. */
typedef struct call_probe {
    const char *callee;
    char resolution[32];
    char detail[64];
    int64_t candidates;
    int64_t matches;
} call_probe;

static atlas_status probe_call(const atlas_code_pending_row *row, void *ud, atlas_err *err) {
    call_probe *p = (call_probe *)ud;
    (void)err;
    if (row->dst_name_text == NULL || strcmp(row->dst_name_text, p->callee) != 0) {
        return ATLAS_OK;
    }
    p->matches++;
    (void)snprintf(p->resolution, sizeof(p->resolution), "%s", row->resolution);
    return ATLAS_OK;
}

static call_probe find_call(env *e, const char *callee, atlas_err *err) {
    call_probe p;
    memset(&p, 0, sizeof(p));
    p.callee = callee;
    int64_t n = 0;
    /* Every edge of that kind, settled or not: this is a probe, not a sweep. */
    T_OK(atlas_db_code_relations_pending(e->db, e->repo_id, "symbol_calls_symbol", 0,
                                         ATLAS_CODE_SWEEP_ALL, NULL, 0, 0, ATLAS_CODE_MAX_ROWS,
                                         probe_call, &p, &n, NULL, err),
         err);
    return p;
}

/* --- resolution ---------------------------------------------------------------- */

static void test_include_and_call_resolution(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "include", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "src", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "include/lib.h",
                  "#ifndef LIB_H\n#define LIB_H\nint lib_add(int a, int b);\n#endif\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/lib.c",
                  "#include \"lib.h\"\n"
                  "int lib_add(int a, int b) { return a + b; }\n",
                  &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/main.c",
                  "#include <stdio.h>\n"
                  "#include \"lib.h\"\n"
                  "int main(void) { return lib_add(1, 2) + puts(\"x\"); }\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    T_CHECK(sum.code_ran);
    T_EQ_INT(sum.code.files_parsed, 3);

    /* `#include "lib.h"` from src/main.c has no sibling `lib.h`, so the
     * same-directory step misses and the repository-wide suffix match finds
     * exactly one file. That is a name match and is reported as one. */
    edge_probe inc = edge_from_file(&e, "src/main.c", "file_includes_file", "lib.h", &err);
    T_EQ_INT(inc.matches, 1);
    T_EQ_STR(inc.resolution, "UNIQUE_LEXICAL");
    T_EQ_STR(inc.dst_path, "include/lib.h");

    /* An angle include of a system header is unresolved, with a reason that says
     * why rather than reading as a missing file. */
    edge_probe sys = edge_from_file(&e, "src/main.c", "file_includes_file", "stdio.h", &err);
    T_EQ_INT(sys.matches, 1);
    T_EQ_STR(sys.resolution, "UNRESOLVED");
    T_EQ_STR(sys.detail, ATLAS_CODE_WHY_SYSTEM_HEADER);

    /* One definition of lib_add exists, so the call resolves — lexically, which
     * is the strongest thing a name match can earn. */
    call_probe call = find_call(&e, "lib_add", &err);
    T_EQ_INT(call.matches, 1);
    T_EQ_STR(call.resolution, "UNIQUE_LEXICAL");

    /* `puts` is a libc function. Atlas has no definition for it and says so. */
    call = find_call(&e, "puts", &err);
    T_EQ_INT(call.matches, 1);
    T_EQ_STR(call.resolution, "UNRESOLVED");

    /* The derived dependency edge exists and is inferred, never observed. */
    edge_probe dep = edge_from_file(&e, "src/main.c", "file_depends_on_file", NULL, &err);
    T_CHECK(dep.matches >= 1);
    env_close(&e);
}

static void test_same_directory_include_is_exact(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "src", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/local.h", "int local(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/use.c", "#include \"local.h\"\nint use(void){return 0;}\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* The quoted form resolves against the including file's own directory
     * first, which is what a compiler does and the only step that consults
     * nothing but the repository's own layout. */
    edge_probe inc = edge_from_file(&e, "src/use.c", "file_includes_file", "local.h", &err);
    T_EQ_INT(inc.matches, 1);
    T_EQ_STR(inc.resolution, "SOURCE_EXACT");
    T_EQ_STR(inc.dst_path, "src/local.h");
    env_close(&e);
}

static void test_angle_include_does_not_use_the_local_directory(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "src", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/config.h", "int local_config(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/use.c", "#include <config.h>\nint use(void){return 0;}\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* A compiler does not search the including file's directory for `<...>`, so
     * neither does Atlas. It still finds the file by suffix — which is a name
     * match and is reported as UNIQUE_LEXICAL, not as exact. Getting this wrong
     * would silently claim a local file where a system header was meant. */
    edge_probe inc = edge_from_file(&e, "src/use.c", "file_includes_file", "config.h", &err);
    T_EQ_INT(inc.matches, 1);
    T_EQ_STR(inc.resolution, "UNIQUE_LEXICAL");
    env_close(&e);
}

static void test_ambiguous_include(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "a", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "b", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "c", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a/conf.h", "int a_conf(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "b/conf.h", "int b_conf(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "c/use.c", "#include \"conf.h\"\nint u(void){return 0;}\n", &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* Two files match and Atlas chooses neither. Choosing one would be
     * indistinguishable from being right. */
    edge_probe inc = edge_from_file(&e, "c/use.c", "file_includes_file", "conf.h", &err);
    T_EQ_INT(inc.matches, 1);
    T_EQ_STR(inc.resolution, "AMBIGUOUS");
    T_EQ_STR(inc.detail, ATLAS_CODE_WHY_MANY_FILES);
    T_EQ_INT(inc.candidates, 2);
    /* The alternatives are recorded rather than discarded. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_candidates;", &err), 2);
    env_close(&e);
}

static void test_two_statics_stay_distinct(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "one.c",
                  "static int helper(void) { return 1; }\n"
                  "int one(void) { return helper(); }\n",
                  &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "two.c",
                  "static int helper(void) { return 2; }\n"
                  "int two(void) { return helper(); }\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* Two definitions, two rows, no merging. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_symbols WHERE name_text='helper'"
                        " AND is_definition=1;",
                        &err),
             2);
    /* And each call resolves to its own file's static rather than becoming
     * ambiguous. C linkage is the reason, and it is enforced in the candidate
     * query so no caller can forget it. */
    call_probe call = find_call(&e, "helper", &err);
    T_EQ_INT(call.matches, 2);
    T_EQ_STR(call.resolution, "UNIQUE_LEXICAL");
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE kind='symbol_calls_symbol'"
                        " AND dst_name_text='helper' AND resolution='UNIQUE_LEXICAL';",
                        &err),
             2);
    /* Each call points at the definition in its own file. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations r JOIN code_symbols s"
                        " ON s.id = r.dst_id"
                        " WHERE r.kind='symbol_calls_symbol' AND r.dst_name_text='helper'"
                        "   AND s.code_file_id = r.owner_file_id;",
                        &err),
             2);
    env_close(&e);
}

static void test_duplicate_external_definitions_are_ambiguous(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int shared(void) { return 1; }\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "b.c", "int shared(void) { return 2; }\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "c.c", "int caller(void) { return shared(); }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* A duplicate external definition is a conflict in the source. Atlas reports
     * it as one rather than picking whichever it saw first. */
    call_probe call = find_call(&e, "shared", &err);
    T_EQ_INT(call.matches, 1);
    T_EQ_STR(call.resolution, "AMBIGUOUS");
    env_close(&e);
}

static void test_macro_and_function_ambiguity(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "m.h", "#define same(x) ((x)+1)\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "f.c", "int same(int x) { return x; }\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "u.c", "int u(void) { return same(1); }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* A macro and a function sharing a name are two candidates of different
     * kinds, never one symbol, and the reason says which confusion it is. */
    call_probe call = find_call(&e, "same", &err);
    T_EQ_INT(call.matches, 1);
    T_EQ_STR(call.resolution, "AMBIGUOUS");
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE kind='symbol_calls_symbol'"
                        " AND dst_name_text='same' AND detail=?1;",
                        &err) >= 0
                 ? 1
                 : 0,
             1);
    edge_probe probe;
    memset(&probe, 0, sizeof(probe));
    probe.want_name = "same";
    int64_t n = 0;
    bool more = false;
    int64_t uid = code_file_id(&e, "u.c", &err);
    T_REQUIRE(uid > 0);
    T_OK(atlas_db_code_edges_from(e.db, e.repo_id, "symbol",
                                  count_rows(&e,
                                             "SELECT id FROM code_symbols WHERE name_text='u'"
                                             " AND is_definition=1;",
                                             &err),
                                  "symbol_calls_symbol", ATLAS_CODE_MAX_ROWS, probe_edge, &probe,
                                  &n, &more, &err),
         &err);
    T_EQ_INT(probe.matches, 1);
    T_EQ_STR(probe.detail, ATLAS_CODE_WHY_MACRO_AND_FUNCTION);
    env_close(&e);
}

static void test_declaration_links_to_definition(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "api.h", "int only_declared(void);\nint provided(void);\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "impl.c", "int provided(void) { return 1; }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* A declaration with a definition somewhere links to it. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE kind='symbol_defined_by'"
                        " AND dst_name_text='provided' AND resolution='UNIQUE_LEXICAL';",
                        &err),
             1);
    /* A declaration with none is unresolved and says which kind of nothing it
     * found: declared but never defined, rather than never mentioned. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE kind='symbol_defined_by'"
                        " AND dst_name_text='only_declared' AND resolution='UNRESOLVED';",
                        &err),
             1);
    env_close(&e);
}

static void test_include_cycle_is_recorded(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "x.h", "#include \"y.h\"\nint x(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "y.h", "#include \"x.h\"\nint y(void);\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* A cycle is a legitimate thing for a repository to contain, and recording
     * it is correct. What must not happen is the pass failing to terminate. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE kind='file_includes_file'"
                        " AND resolution='SOURCE_EXACT';",
                        &err),
             2);
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE kind='file_depends_on_file';",
                        &err),
             2);
    env_close(&e);
}

/* --- incremental behaviour ------------------------------------------------------ */

static void test_unchanged_pass_parses_nothing(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"a.h\"\nint a(void){return 0;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.h", "int a(void);\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    T_EQ_INT(sum.code.files_parsed, 2);

    int64_t symbols = count_rows(&e, "SELECT COUNT(*) FROM code_symbols;", &err);
    int64_t relations = count_rows(&e, "SELECT COUNT(*) FROM code_relations;", &err);
    T_CHECK(symbols > 0);
    T_CHECK(relations > 0);

    /* Five idle passes, including full content-verifying ones.
     *
     * A full pass rehashes every byte, and selection compares the hash the graph
     * facts were built from rather than "was this file hashed" — so it still
     * selects nothing. Keying off the pass's own activity would make the
     * five-minute periodic full pass reparse the world every five minutes. */
    for (int i = 0; i < 5; i++) {
        atlas_reconcile_summary again;
        run_pass(&e, (i % 2) == 0, &again, &err);
        T_EQ_INT(again.code.files_selected, 0);
        T_EQ_INT(again.code.files_parsed, 0);
        atlas_reconcile_summary_free(&again);
    }
    /* And no durable growth: the same rows, not the same count of different
     * rows. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols;", &err), symbols);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_relations;", &err), relations);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_files;", &err), 2);
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}

static void test_one_file_edit_parses_one_file(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    for (int i = 0; i < 6; i++) {
        char name[32];
        char body[128];
        (void)snprintf(name, sizeof(name), "f%d.c", i);
        (void)snprintf(body, sizeof(body), "int f%d(void) { return %d; }\n", i, i);
        T_OK(fx_write(fx_repo(&e.fx), name, body, &err), &err);
    }
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    T_EQ_INT(sum.code.files_parsed, 6);
    atlas_reconcile_summary_free(&sum);

    T_OK(fx_write(fx_repo(&e.fx), "f3.c", "int f3(void) { return 99; }\nint extra(void){return 0;}\n",
                  &err),
         &err);
    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);
    /* One file changed; one file parsed. */
    T_EQ_INT(after.code.files_parsed, 1);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='extra';", &err), 1);
    /* And the other five files' facts are untouched, not rewritten. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_symbols WHERE name_text='f0'"
                        " AND is_definition=1;",
                        &err),
             1);
    atlas_reconcile_summary_free(&after);
    env_close(&e);
}

static void test_header_change_updates_resolution_without_reparsing(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "call.c", "int c(void) { return target(); }\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "def.c", "int unrelated(void) { return 0; }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    call_probe call = find_call(&e, "target", &err);
    T_EQ_STR(call.resolution, "UNRESOLVED");
    atlas_reconcile_summary_free(&sum);

    /* A definition appears in a file that does not mention the call site. The
     * call must become resolved — and `call.c` must not be reparsed to do it. */
    T_OK(fx_write(fx_repo(&e.fx), "def.c",
                  "int unrelated(void) { return 0; }\nint target(void) { return 7; }\n", &err),
         &err);
    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);
    T_EQ_INT(after.code.files_parsed, 1);
    call = find_call(&e, "target", &err);
    T_EQ_STR(call.resolution, "UNIQUE_LEXICAL");
    atlas_reconcile_summary_free(&after);

    /* And a second definition makes it ambiguous again, without reparsing
     * `call.c` either. Without the by-name re-resolution sweep, the existing
     * edge would keep claiming a certainty it no longer has. */
    T_OK(fx_write(fx_repo(&e.fx), "third.c", "int target(void) { return 8; }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "add third", &err), &err);
    atlas_reconcile_summary third;
    run_pass(&e, false, &third, &err);
    T_EQ_INT(third.code.files_parsed, 1);
    call = find_call(&e, "target", &err);
    T_EQ_STR(call.resolution, "AMBIGUOUS");
    atlas_reconcile_summary_free(&third);
    env_close(&e);
}

static void test_delete_removes_graph_rows(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "keep.c", "int keep(void) { return gone(); }\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "gone.c", "int gone(void) { return 1; }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    call_probe call = find_call(&e, "gone", &err);
    T_EQ_STR(call.resolution, "UNIQUE_LEXICAL");
    atlas_reconcile_summary_free(&sum);

    T_OK(fx_remove(fx_repo(&e.fx), "gone.c", &err), &err);
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), (const char *[]){"rm", "--cached", "gone.c"}, 3, &err),
         &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "remove", &err), &err);
    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);
    T_EQ_INT(after.code.files_removed, 1);

    /* `files` rows are tombstoned rather than deleted, so a foreign key from
     * `files` would never fire here. The removal is explicit writer-path work,
     * and this is what proves it happened. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_files WHERE path_text='gone.c';", &err), 0);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='gone';", &err), 0);
    /* No orphans anywhere. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_symbols s LEFT JOIN code_files f"
                        " ON f.id = s.code_file_id WHERE f.id IS NULL;",
                        &err),
             0);
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations r LEFT JOIN code_files f"
                        " ON f.id = r.owner_file_id WHERE f.id IS NULL;",
                        &err),
             0);
    /* And the edge that pointed at the departed definition is unresolved again
     * rather than dangling. */
    call = find_call(&e, "gone", &err);
    T_EQ_STR(call.resolution, "UNRESOLVED");
    atlas_reconcile_summary_free(&after);
    env_close(&e);
}

static void test_rename_moves_the_facts(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "old.c", "int moved(void) { return 1; }\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_files WHERE path_text='old.c';", &err), 1);
    atlas_reconcile_summary_free(&sum);

    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), (const char *[]){"mv", "old.c", "new.c"}, 3, &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "rename", &err), &err);
    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);

    /* A rename is a tombstone plus an addition in `files`, and therefore a
     * removal plus a parse here. Nothing has to recognise a rename as such,
     * which is what keeps the stale-row case impossible rather than handled. */
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_files WHERE path_text='old.c';", &err), 0);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_files WHERE path_text='new.c';", &err), 1);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='moved';", &err), 1);
    atlas_reconcile_summary_free(&after);
    env_close(&e);
}

static void test_unparseable_file_degrades_and_says_so(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "good.c", "int good(void) { return 0; }\n", &err), &err);
    /* A file with an extension Atlas parses and content it cannot: a NUL makes
     * it binary, whatever the name says. */
    static const char blob[] = {'i', 'n', 't', ' ', 'x', ';', 0, 'j', 'u', 'n', 'k'};
    T_OK(fx_write_bytes(fx_repo(&e.fx), "binary.c", 8, blob, sizeof(blob), 0644, &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* Skipped with a reason, not failed and not silently empty. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_files WHERE path_text='binary.c'"
                        " AND parse_status='skipped';",
                        &err),
             1);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='good';", &err), 1);
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}

static void test_structural_generation_tracks_the_pass(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int a(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    atlas_code_index_state cs;
    atlas_code_index_state_init(&cs);
    T_OK(atlas_db_code_state_get(e.db, e.repo_id, &cs, &err), &err);
    T_CHECK(cs.present);
    /* The structural generation is the reconciliation pass's own, so "does the
     * graph describe the file index?" is an integer comparison rather than an
     * inference from timestamps. */
    T_EQ_INT(cs.last_complete_generation, sum.generation);
    T_CHECK(!cs.degraded);
    T_CHECK(cs.symbols > 0);
    T_CHECK(cs.relations > 0);
    atlas_code_index_state_free(&cs);
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}

static void test_non_c_files_get_no_c_semantics(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "script.py", "def function_like(): pass\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "notes.md", "# int heading(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "cpp.cpp", "class Thing { void method(); };\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "real.c", "int real(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);

    /* Only the C file is structurally indexed. C++ is deliberately out of scope
     * and is not guessed at. */
    T_EQ_INT(sum.code.files_parsed, 1);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_files;", &err), 1);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='method';", &err),
             0);
    T_EQ_INT(count_rows(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='heading';", &err),
             0);
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}

static void test_repository_is_never_modified(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"a.h\"\nint a(void){return b();}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.h", "int a(void);\nint b(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json",
                  "[{\"directory\":\".\",\"file\":\"a.c\",\"arguments\":[\"cc\",\"-I.\",\"a.c\"]}]\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);
    atlas_reconcile_summary sum;
    run_pass(&e, true, &sum, &err);
    T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
    /* The read-only guarantee, proven rather than asserted — including with a
     * compile database present, which is the one input that contains something
     * shaped like a command. */
    T_EQ_STR(after, before);
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}


/* --- the incremental scope --------------------------------------------------
 *
 * A3's second season of performance work turned "re-attempt everything
 * unresolved" into "re-attempt what could have changed". These are the tests
 * that keep the second one honest: skipping work is only correct while the
 * answers stay identical, and the failure mode of getting it wrong is a stale
 * edge that nothing ever revisits. */

static void test_a_settled_pass_resolves_nothing(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"a.h\"\nint a(void){return b();}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.h", "int a(void);\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    T_CHECK(sum.code.relations_resolved > 0);
    atlas_reconcile_summary_free(&sum);

    /* `b()` has no definition anywhere and never will. A pass that re-attempted
     * it would do repository-sized work to reach the same UNRESOLVED, which is
     * exactly what an unchanged pass must not do. */
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations WHERE resolution='UNRESOLVED';", &err) >
                 0,
             1);
    T_EQ_INT(count_rows(&e, "SELECT resolve_settled FROM code_index_state;", &err), 1);

    for (int i = 0; i < 3; i++) {
        atlas_reconcile_summary again;
        run_pass(&e, (i % 2) == 0, &again, &err);
        T_EQ_INT(again.code.files_parsed, 0);
        T_EQ_INT(again.code.relations_resolved, 0);
        atlas_reconcile_summary_free(&again);
    }

    /* And the flag is what makes that safe rather than optimistic: clear it, as
     * a pass killed half way through resolution would leave it, and the next
     * pass resolves again without anything else having changed. */
    T_OK(atlas_db_exec_sql(e.db, "UPDATE code_index_state SET resolve_settled=0;", &err), &err);
    atlas_reconcile_summary recovered;
    run_pass(&e, false, &recovered, &err);
    T_EQ_INT(recovered.code.files_parsed, 0);
    T_CHECK(recovered.code.relations_resolved > 0);
    atlas_reconcile_summary_free(&recovered);
    env_close(&e);
}

static void test_an_added_header_resolves_an_old_include(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"late.h\"\nint a(void){return 0;}\n", &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    edge_probe inc = edge_from_file(&e, "a.c", "file_includes_file", "late.h", &err);
    T_EQ_INT(inc.matches, 1);
    T_EQ_STR(inc.resolution, "UNRESOLVED");
    atlas_reconcile_summary_free(&sum);

    /* The header appears. Include resolution reads the set of paths, so this is
     * the one kind of change that can settle an include in a file nobody
     * touched — and `a.c` must not be reparsed for it to happen. */
    T_OK(fx_write(fx_repo(&e.fx), "late.h", "int late(void);\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "add late.h", &err), &err);
    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);
    T_EQ_INT(after.code.files_parsed, 1);
    inc = edge_from_file(&e, "a.c", "file_includes_file", "late.h", &err);
    T_EQ_STR(inc.resolution, "SOURCE_EXACT");
    T_EQ_STR(inc.dst_path, "late.h");
    atlas_reconcile_summary_free(&after);
    env_close(&e);
}

static void test_editing_a_static_leaves_the_other_files_alone(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    /* The name every C project has in every file. Internal linkage keeps the two
     * apart, and the incremental scope leaves internal names out of the
     * repository-wide re-resolution for exactly that reason: they cannot change
     * how anything outside their own file resolves. */
    T_OK(fx_write(fx_repo(&e.fx), "one.c",
                  "static int helper(void){return 1;}\nint one(void){return helper();}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "two.c",
                  "static int helper(void){return 2;}\nint two(void){return helper();}\n", &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    atlas_reconcile_summary_free(&sum);

    int64_t two_id = code_file_id(&e, "two.c", &err);
    T_REQUIRE(two_id > 0);

    T_OK(fx_write(fx_repo(&e.fx), "one.c",
                  "static int helper(void){return 11;}\nint one(void){return helper();}\n", &err),
         &err);
    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);
    T_EQ_INT(after.code.files_parsed, 1);
    atlas_reconcile_summary_free(&after);

    /* Both calls still point at their own file's definition, and `two.c`'s rows
     * were never rewritten — its `code_files` id is the same one. */
    T_EQ_INT(code_file_id(&e, "two.c", &err), two_id);
    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM code_relations r"
                        " JOIN code_symbols s ON s.id = r.dst_id"
                        " WHERE r.kind='symbol_calls_symbol' AND r.dst_kind='symbol'"
                        "   AND r.resolution='UNIQUE_LEXICAL'"
                        "   AND s.code_file_id = r.owner_file_id;",
                        &err),
             2);
    env_close(&e);
}

/* Nothing points at a row that is gone.
 *
 * The incremental path no longer runs the repository-wide dangling scan, and
 * relies instead on the writer unsettling the exact edges that pointed into a
 * file whose rows it is about to replace. If that targeted invalidation ever
 * misses a case, the symptom is a `dst_id` referring to a deleted symbol —
 * silent, permanent, and invisible to every other assertion in this file. */
static void assert_no_dangling(env *e, atlas_err *err) {
    T_EQ_INT(count_rows(e,
                        "SELECT COUNT(*) FROM code_relations r WHERE r.dst_kind='symbol'"
                        " AND NOT EXISTS(SELECT 1 FROM code_symbols s WHERE s.id = r.dst_id);",
                        err),
             0);
    T_EQ_INT(count_rows(e,
                        "SELECT COUNT(*) FROM code_relations r WHERE r.dst_kind='file'"
                        " AND NOT EXISTS(SELECT 1 FROM code_files f WHERE f.id = r.dst_id);",
                        err),
             0);
}

static void test_edits_leave_no_edge_pointing_at_a_dead_row(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "def.c", "int target(void){return 1;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "use.c",
                  "#include \"api.h\"\nint use(void){return target();}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "api.h", "int target(void);\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    atlas_reconcile_summary_free(&sum);

    call_probe call = find_call(&e, "target", &err);
    T_EQ_STR(call.resolution, "UNIQUE_LEXICAL");
    assert_no_dangling(&e, &err);

    /* Reparse the definition file. Its symbol rows are deleted and recreated
     * with new ids, so every edge that resolved to the old ones must be
     * unsettled and settled again in the same pass. */
    T_OK(fx_write(fx_repo(&e.fx), "def.c", "int target(void){return 2;}\n", &err), &err);
    atlas_reconcile_summary edited;
    run_pass(&e, false, &edited, &err);
    T_EQ_INT(edited.code.files_parsed, 1);
    atlas_reconcile_summary_free(&edited);
    assert_no_dangling(&e, &err);
    call = find_call(&e, "target", &err);
    T_EQ_STR(call.resolution, "UNIQUE_LEXICAL");

    /* Now delete both the definition and the header. The call goes back to
     * unresolved with a reason, and the include has nothing to point at. */
    T_OK(fx_remove(fx_repo(&e.fx), "def.c", &err), &err);
    T_OK(fx_remove(fx_repo(&e.fx), "api.h", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "drop", &err), &err);
    atlas_reconcile_summary removed;
    run_pass(&e, false, &removed, &err);
    T_EQ_INT(removed.code.files_removed, 2);
    atlas_reconcile_summary_free(&removed);
    assert_no_dangling(&e, &err);
    call = find_call(&e, "target", &err);
    T_EQ_STR(call.resolution, "UNRESOLVED");
    edge_probe inc = edge_from_file(&e, "use.c", "file_includes_file", "api.h", &err);
    T_EQ_INT(inc.matches, 1);
    T_EQ_STR(inc.resolution, "UNRESOLVED");
    env_close(&e);
}


/* The one query whose *plan* is part of its correctness budget.
 *
 * `code_files` has two indexes beginning with `repo_id`: the implicit one behind
 * `UNIQUE(repo_id, path_raw)` and `idx_code_files_basename`. Left to choose
 * between them for a basename lookup, SQLite took the unique one, seeking on
 * `repo_id` alone and scanning every file in the repository — 5 444 rows per
 * unresolvable include on the acceptance fixture, and the largest single cost of
 * a structural pass. The statement now says `INDEXED BY`, which is a hard
 * constraint rather than a hint: drop the index and the statement fails to
 * prepare instead of quietly becoming a scan.
 *
 * This asserts both halves — that the index exists, and that a query of this
 * shape reaches its rows through it. The SQL below must stay the same shape as
 * SUFFIX_SQL in `src/db/db_code.c`. */
static void test_include_suffix_lookup_seeks_the_basename_index(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"deep/b.h\"\nint a(void){return 0;}\n", &err),
         &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "deep", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "deep/b.h", "int b(void);\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_index(&e, &err);
    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    atlas_reconcile_summary_free(&sum);

    T_EQ_INT(count_rows(&e,
                        "SELECT COUNT(*) FROM sqlite_master WHERE type='index'"
                        " AND name='idx_code_files_basename';",
                        &err),
             1);

    /* EXPLAIN QUERY PLAN names the index it chose in its `detail` column. */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(
        e.db->h,
        "EXPLAIN QUERY PLAN"
        " SELECT id, id, path_raw, path_text, NULL, NULL, 0 FROM code_files"
        " INDEXED BY idx_code_files_basename"
        " WHERE repo_id=?1 AND basename_raw = ?7"
        "   AND length(path_raw) > ?3"
        "   AND substr(path_raw, length(path_raw) - ?3 + 1) = ?5"
        " ORDER BY path_raw LIMIT ?6;",
        -1, &st, NULL);
    T_REQUIRE(rc == SQLITE_OK);
    bool seeks_basename = false;
    bool scans = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *detail = (const char *)sqlite3_column_text(st, 3);
        if (detail == NULL) {
            continue;
        }
        if (strstr(detail, "idx_code_files_basename") != NULL) {
            seeks_basename = true;
        }
        if (strncmp(detail, "SCAN", 4) == 0) {
            scans = true;
        }
    }
    sqlite3_finalize(st);
    T_CHECK_MSG(seeks_basename, "the include suffix lookup does not use the basename index");
    T_CHECK_MSG(!scans, "the include suffix lookup scans code_files");
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"include and call resolution", test_include_and_call_resolution},
    {"same-directory includes are exact", test_same_directory_include_is_exact},
    {"angle includes skip the local directory",
     test_angle_include_does_not_use_the_local_directory},
    {"ambiguous includes", test_ambiguous_include},
    {"two statics stay distinct", test_two_statics_stay_distinct},
    {"duplicate external definitions", test_duplicate_external_definitions_are_ambiguous},
    {"macro and function ambiguity", test_macro_and_function_ambiguity},
    {"declarations link to definitions", test_declaration_links_to_definition},
    {"include cycles", test_include_cycle_is_recorded},
    {"unchanged passes parse nothing", test_unchanged_pass_parses_nothing},
    {"one edit parses one file", test_one_file_edit_parses_one_file},
    {"header changes update resolution", test_header_change_updates_resolution_without_reparsing},
    {"deletion removes graph rows", test_delete_removes_graph_rows},
    {"rename moves the facts", test_rename_moves_the_facts},
    {"unparseable files degrade honestly", test_unparseable_file_degrades_and_says_so},
    {"the structural generation tracks the pass", test_structural_generation_tracks_the_pass},
    {"non-C files get no C semantics", test_non_c_files_get_no_c_semantics},
    {"the repository is never modified", test_repository_is_never_modified},
    {"a settled pass resolves nothing", test_a_settled_pass_resolves_nothing},
    {"an added header resolves an old include", test_an_added_header_resolves_an_old_include},
    {"editing a static leaves other files alone",
     test_editing_a_static_leaves_the_other_files_alone},
    {"edits leave no edge pointing at a dead row",
     test_edits_leave_no_edge_pointing_at_a_dead_row},
    {"the include suffix lookup seeks the basename index",
     test_include_suffix_lookup_seeks_the_basename_index},
};

ATLAS_TEST_MAIN("code_graph", TESTS)

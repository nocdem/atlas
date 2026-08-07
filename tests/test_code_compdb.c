/* Atlas - compile database ingestion, and the proof that nothing runs.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A compile database is the one repository file that contains something shaped
 * like a command line. Everything here is about that.
 *
 * The parser half is driven directly with bytes — malformed documents, hostile
 * paths, response files, duplicate entries — because those cases are cheap to
 * state and expensive to get wrong. The execution half is proven the way
 * `tests/test_git_hardening.c` proves it for git: a real helper is planted, a
 * real compile database points at it, a real pass runs, and the marker the
 * helper would create is asserted never to exist.
 *
 * A comment saying "this file never executes anything" is worth nothing. A test
 * that would fail if it did is worth something.
 */
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/code.h"
#include "atlas/reconcile.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#define ROOT "/repo"

/* A raw byte search, so "this string is nowhere in the result" is asserted over
 * bytes rather than over the fields somebody thought to check. */
static bool bytes_contain(const void *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (hay == NULL || n == 0 || hay_len < n) {
        return false;
    }
    const char *p = (const char *)hay;
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(p + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* --- the parser ---------------------------------------------------------------- */

static void parse(const char *json, atlas_code_compdb *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_code_compdb_parse(json, strlen(json), ROOT, strlen(ROOT), out, &err), &err);
}

static const atlas_code_cu *unit_for(const atlas_code_compdb *c, const char *rel) {
    for (size_t i = 0; i < c->unit_count; i++) {
        if (strcmp(atlas_code_compdb_str(c, c->units[i].source_off), rel) == 0) {
            return &c->units[i];
        }
    }
    return NULL;
}

static bool has_incdir(const atlas_code_compdb *c, const atlas_code_cu *cu, const char *dir,
                       atlas_code_incdir_kind kind, bool external) {
    for (size_t k = 0; k < cu->incdir_count; k++) {
        const atlas_code_cu_incdir *d = &c->incdirs[cu->incdir_first + k];
        if (d->kind == (int32_t)kind && d->external == external &&
            strcmp(atlas_code_compdb_str(c, d->path_off), dir) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_define(const atlas_code_compdb *c, const atlas_code_cu *cu, const char *name,
                       const char *value, bool undef) {
    for (size_t k = 0; k < cu->define_count; k++) {
        const atlas_code_cu_define *d = &c->defines[cu->define_first + k];
        if (d->undef != undef || strcmp(atlas_code_compdb_str(c, d->name_off), name) != 0) {
            continue;
        }
        if (value == NULL) {
            return d->value_len == 0;
        }
        return strcmp(atlas_code_compdb_str(c, d->value_off), value) == 0;
    }
    return false;
}

static void test_arguments_allowlist(void) {
    atlas_code_compdb c;
    parse("[{\"directory\":\"/repo\",\"file\":\"src/a.c\",\"output\":\"a.o\","
          "\"arguments\":[\"/usr/bin/cc\",\"-I\",\"include\",\"-Isrc\",\"-iquote\",\"src/priv\","
          "\"-isystem\",\"/usr/include/foo\",\"-idirafter\",\"late\","
          "\"-DDEBUG\",\"-DLEVEL=3\",\"-U\",\"NDEBUG\",\"-std=c17\",\"-x\",\"c\","
          "\"-o\",\"build/a.o\",\"-c\",\"src/a.c\"]}]",
          &c);
    T_EQ_INT(c.unit_count, 1);
    const atlas_code_cu *cu = unit_for(&c, "src/a.c");
    T_REQUIRE(cu != NULL);

    /* Both spellings of every directory flag, separated and joined. */
    T_CHECK(has_incdir(&c, cu, "include", ATLAS_CODE_INCDIR_SEARCH, false));
    T_CHECK(has_incdir(&c, cu, "src", ATLAS_CODE_INCDIR_SEARCH, false));
    T_CHECK(has_incdir(&c, cu, "src/priv", ATLAS_CODE_INCDIR_QUOTE, false));
    T_CHECK(has_incdir(&c, cu, "late", ATLAS_CODE_INCDIR_AFTER, false));
    /* A directory outside the repository is kept as metadata, marked external,
     * and stored absolute. It is never opened by anything. */
    T_CHECK(has_incdir(&c, cu, "/usr/include/foo", ATLAS_CODE_INCDIR_SYSTEM, true));

    T_CHECK(has_define(&c, cu, "DEBUG", NULL, false));
    T_CHECK(has_define(&c, cu, "LEVEL", "3", false));
    T_CHECK(has_define(&c, cu, "NDEBUG", NULL, true));

    T_EQ_STR(atlas_code_compdb_str(&c, cu->std_off), "c17");
    T_EQ_STR(atlas_code_compdb_str(&c, cu->lang_off), "c");
    /* `-o` wins over the `output` member when both are present; either way the
     * identity is recorded so two configurations of one file stay distinct. */
    T_EQ_STR(atlas_code_compdb_str(&c, cu->output_off), "build/a.o");
    /* The compiler path and `-c src/a.c` are not on the allowlist. They are
     * counted so "the build had flags Atlas ignored" is a number. */
    T_CHECK(cu->dropped_args >= 3);
    atlas_code_compdb_free(&c);
}

static void test_command_is_hashed_not_stored(void) {
    atlas_code_compdb c;
    const char *cmd = "/usr/bin/cc -DSECRET=hunter2 -c src/a.c -o a.o";
    char json[512];
    (void)snprintf(json, sizeof(json),
                   "[{\"directory\":\"/repo\",\"file\":\"src/a.c\",\"command\":\"%s\"}]", cmd);
    parse(json, &c);
    const atlas_code_cu *cu = unit_for(&c, "src/a.c");
    T_REQUIRE(cu != NULL);
    T_CHECK(cu->command_present);
    T_EQ_INT(strlen(cu->command_hash), ATLAS_SHA256_HEX_LEN);

    /* The string itself is nowhere in the result. Searched as raw bytes across
     * the whole arena, so a value stored in a field nobody thought to check is
     * still caught. A value nothing holds is a value nothing can run. */
    T_CHECK(!bytes_contain(c.arena.data, c.arena.len, "hunter2"));
    T_CHECK(!bytes_contain(c.arena.data, c.arena.len, "/usr/bin/cc"));

    /* `command` is not parsed either: it carries `-DSECRET` and no define is
     * recorded from it. Shell-splitting it would be the beginning of
     * interpreting a command line, which Atlas does not do. */
    T_EQ_INT(cu->define_count, 0);
    atlas_code_compdb_free(&c);
}

static void test_hostile_command_strings(void) {
    atlas_code_compdb c;
    /* Every one of these is a command-injection attempt against something that
     * splits and executes. Atlas neither splits nor executes, so all it has to
     * do is store a hash and carry on — which is what makes the whole class a
     * non-event rather than a filter to maintain. */
    parse("[{\"directory\":\"/repo\",\"file\":\"a.c\","
          "\"command\":\"cc; rm -rf / #\"},"
          "{\"directory\":\"/repo\",\"file\":\"b.c\","
          "\"command\":\"cc $(touch /tmp/pwned) -c b.c\"},"
          "{\"directory\":\"/repo\",\"file\":\"c.c\","
          "\"command\":\"cc `id` -c c.c\"},"
          "{\"directory\":\"/repo\",\"file\":\"d.c\","
          "\"command\":\"cc\\nrm -rf /\\n-c d.c\"}]",
          &c);
    T_EQ_INT(c.unit_count, 4);
    for (size_t i = 0; i < c.unit_count; i++) {
        T_CHECK(c.units[i].command_present);
        T_EQ_INT(strlen(c.units[i].command_hash), ATLAS_SHA256_HEX_LEN);
    }
    atlas_code_compdb_free(&c);
}

static void test_response_files_and_plugins_are_dropped(void) {
    atlas_code_compdb c;
    parse("[{\"directory\":\"/repo\",\"file\":\"a.c\",\"arguments\":["
          "\"cc\",\"@args.rsp\",\"@/etc/shadow\",\"-fplugin=/tmp/evil.so\","
          "\"-include\",\"/etc/passwd\",\"-Iinclude\",\"a.c\"]}]",
          &c);
    const atlas_code_cu *cu = unit_for(&c, "a.c");
    T_REQUIRE(cu != NULL);
    /* Only the include directory survives; everything else is counted and
     * ignored. A response file is never opened, and `-include` never becomes a
     * file Atlas reads. */
    T_EQ_INT(cu->incdir_count, 1);
    T_CHECK(has_incdir(&c, cu, "include", ATLAS_CODE_INCDIR_SEARCH, false));
    T_CHECK(cu->dropped_args >= 5);
    atlas_code_compdb_free(&c);
}

static void test_paths_outside_the_repository(void) {
    atlas_code_compdb c;
    parse("[{\"directory\":\"/repo\",\"file\":\"/etc/passwd\"},"
          "{\"directory\":\"/repo\",\"file\":\"../../etc/shadow\"},"
          "{\"directory\":\"/elsewhere\",\"file\":\"x.c\"},"
          "{\"directory\":\"/repository-lookalike\",\"file\":\"y.c\"},"
          "{\"directory\":\"/repo\",\"file\":\"good.c\"}]",
          &c);
    /* Only the entry inside the repository survives. `/repository-lookalike` is
     * the case a plain prefix comparison gets wrong: it starts with `/repo` and
     * is a different directory. */
    T_EQ_INT(c.unit_count, 1);
    T_CHECK(unit_for(&c, "good.c") != NULL);
    T_EQ_INT(c.entries_dropped, 4);
    T_EQ_INT(c.entries_seen, 5);
    atlas_code_compdb_free(&c);
}

static void test_traversal_is_folded_not_followed(void) {
    atlas_code_compdb c;
    /* Normalisation is lexical: no symlink is followed and nothing is stat'ed,
     * so resolution cannot depend on what the filesystem looks like. */
    parse("[{\"directory\":\"/repo/src/../src\",\"file\":\"./nested/../a.c\"}]", &c);
    T_EQ_INT(c.unit_count, 1);
    T_EQ_STR(atlas_code_compdb_str(&c, c.units[0].source_off), "src/a.c");
    atlas_code_compdb_free(&c);
}

static void test_duplicate_and_multi_config_entries(void) {
    atlas_code_compdb c;
    parse("[{\"directory\":\"/repo\",\"file\":\"a.c\",\"output\":\"a.o\"},"
          "{\"directory\":\"/repo\",\"file\":\"a.c\",\"output\":\"a.o\"},"
          "{\"directory\":\"/repo\",\"file\":\"a.c\",\"output\":\"a-debug.o\"}]",
          &c);
    /* All three are parsed; the storage layer collapses the duplicate pair on
     * its uniqueness key and keeps the second configuration, because one file
     * compiled twice with different flags is two configurations and collapsing
     * them would lose what a compile database exists to record. */
    T_EQ_INT(c.unit_count, 3);
    atlas_code_compdb_free(&c);
}

static void test_malformed_documents(void) {
    static const char *const BAD[] = {
        "",
        "not json at all",
        "{}",
        "[",
        "[{\"file\":}]",
        "{\"file\":\"a.c\"}",
        "[1, 2, 3]",
        "[null]",
        "[{\"file\":123}]",
        "[{\"directory\":\"/repo\"}]",
        NULL,
    };
    for (size_t i = 0; BAD[i] != NULL; i++) {
        atlas_code_compdb c;
        atlas_err err;
        atlas_err_init(&err);
        /* Malformed is a fact about the repository, not an Atlas failure: zero
         * units and a reason, never a status that takes a pass down. */
        T_OK(atlas_code_compdb_parse(BAD[i], strlen(BAD[i]), ROOT, strlen(ROOT), &c, &err), &err);
        T_EQ_INT(c.unit_count, 0);
        atlas_code_compdb_free(&c);
    }
}

static void test_oversize_document_is_refused_before_parsing(void) {
    atlas_code_compdb c;
    atlas_err err;
    atlas_err_init(&err);
    /* The length is checked before the parser is entered, so a claimed size can
     * never become an allocation. The buffer is not actually that large — the
     * point is that nothing reads it. */
    static const char small[] = "[]";
    T_OK(atlas_code_compdb_parse(small, (size_t)ATLAS_CODE_MAX_COMPILE_DB_BYTES + 1u, ROOT,
                                 strlen(ROOT), &c, &err),
         &err);
    T_EQ_INT(c.unit_count, 0);
    T_CHECK(c.truncated);
    T_CHECK(c.truncated_reason != NULL);
    atlas_code_compdb_free(&c);
}

static void test_entry_ceiling(void) {
    atlas_buf json = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_append_ch(&json, '[', &err), &err);
    for (int i = 0; i < ATLAS_CODE_MAX_COMPILE_UNITS + 10; i++) {
        T_OK(atlas_buf_appendf(&json, &err, "%s{\"directory\":\"/repo\",\"file\":\"f%d.c\"}",
                               i > 0 ? "," : "", i),
             &err);
    }
    T_OK(atlas_buf_append_ch(&json, ']', &err), &err);
    atlas_code_compdb c;
    T_OK(atlas_code_compdb_parse(json.data, json.len, ROOT, strlen(ROOT), &c, &err), &err);
    T_EQ_INT(c.unit_count, ATLAS_CODE_MAX_COMPILE_UNITS);
    T_CHECK(c.truncated);
    atlas_code_compdb_free(&c);
    atlas_buf_free(&json);
}

static void test_no_compile_database_still_works(void) {
    /* Stated as a property of the parser: an absent database is zero units and
     * no error. The pipeline half is covered by every other suite here, none of
     * which writes a compile_commands.json. */
    atlas_code_compdb c;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_code_compdb_parse(NULL, 0, ROOT, strlen(ROOT), &c, &err), &err);
    T_EQ_INT(c.unit_count, 0);
    atlas_code_compdb_free(&c);
}

/* --- the pipeline, against a real repository -------------------------------------- */

typedef struct env {
    fixture fx;
    atlas_db *db;
    atlas_git *g;
    int64_t repo_id;
} env;

static void env_start(env *e, atlas_err *err) {
    T_OK(fx_open(&e->fx, err), err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, err), err);
}

static void env_register(env *e, atlas_err *err) {
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

static void env_stop(env *e) {
    atlas_git_close(e->g);
    atlas_db_close(e->db);
    fx_close(&e->fx);
}

static void pass(env *e, atlas_reconcile_summary *sum, atlas_err *err) {
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    atlas_reconcile_summary_init(sum);
    T_OK(atlas_reconcile_run(e->db, e->g, e->repo_id, &opts, sum, err), err);
}

static int64_t count(env *e, const char *sql, atlas_err *err) {
    int64_t n = 0;
    T_OK(atlas_db_query_int64(e->db, sql, &n, err), err);
    return n;
}

/* The one that matters: a compile database naming a real, runnable helper, a
 * real pass over it, and the assertion that the helper never ran.
 *
 * The helper is the same one `tests/test_git_hardening.c` uses to prove that a
 * hostile git config cannot make Atlas execute anything. If any code path ever
 * shell-splits or execs a compile-database command, this fails. */
static void test_nothing_from_a_compile_database_is_executed(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_start(&e, &err);

    atlas_buf helper = ATLAS_BUF_INIT;
    atlas_buf marker = ATLAS_BUF_INIT;
    T_OK(fx_install_marker(fx_repo(&e.fx), "fake-cc", &helper, &marker, &err), &err);
    fx_marker_clear(atlas_buf_cstr(&marker));

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "#include \"a.h\"\nint a(void){return 0;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.h", "int a(void);\n", &err), &err);

    atlas_buf json = ATLAS_BUF_INIT;
    /* Every field that could plausibly be executed points at the helper: the
     * command string, argv[0], a plugin argument, and a response file. */
    T_OK(atlas_buf_appendf(&json, &err,
                           "[{\"directory\":\"%s\",\"file\":\"a.c\","
                           "\"command\":\"%s --run\","
                           "\"arguments\":[\"%s\",\"-fplugin=%s\",\"@%s\",\"-I.\",\"a.c\"]}]\n",
                           fx_repo(&e.fx), atlas_buf_cstr(&helper), atlas_buf_cstr(&helper),
                           atlas_buf_cstr(&helper), atlas_buf_cstr(&helper)),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", atlas_buf_cstr(&json), &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_register(&e, &err);

    atlas_reconcile_summary sum;
    pass(&e, &sum, &err);
    T_CHECK(sum.code.compile_db_present);
    T_EQ_INT(sum.code.compile_units, 1);

    /* The proof. */
    T_CHECK_MSG(!fx_marker_fired(atlas_buf_cstr(&marker)),
                "a compile-database command was executed");

    /* And the helper's path is nowhere in the index, because the command string
     * is hashed rather than stored. Searched as raw bytes in the one column
     * that could hold it. */
    T_EQ_INT(count(&e,
                   "SELECT COUNT(*) FROM code_units WHERE command_hash LIKE '%fake-cc%'"
                   " OR directory_text LIKE '%fake-cc%' OR output_text LIKE '%fake-cc%';",
                   &err),
             0);
    T_CHECK(count(&e, "SELECT COUNT(*) FROM code_units WHERE command_present=1;", &err) == 1);

    atlas_reconcile_summary_free(&sum);
    atlas_buf_free(&json);
    atlas_buf_free(&helper);
    atlas_buf_free(&marker);
    env_stop(&e);
}

static void test_build_metadata_resolves_an_include(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_start(&e, &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "vendor_include", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "src", &err), &err);
    /* The header is not beside the source and its spelling does not match the
     * tail of its path, so only a build-metadata include directory can place
     * it. */
    T_OK(fx_write(fx_repo(&e.fx), "vendor_include/only.h", "int only(void);\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src/u.c", "#include <only.h>\nint u(void){return 0;}\n", &err),
         &err);
    atlas_buf json = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&json, &err,
                           "[{\"directory\":\"%s\",\"file\":\"src/u.c\","
                           "\"arguments\":[\"cc\",\"-Ivendor_include\",\"-c\",\"src/u.c\"]}]\n",
                           fx_repo(&e.fx)),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", atlas_buf_cstr(&json), &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_register(&e, &err);

    atlas_reconcile_summary sum;
    pass(&e, &sum, &err);
    /* Resolved through the build, and labelled as coming from the build: a
     * stronger claim than a name match, a weaker one than reading the bytes. */
    T_EQ_INT(count(&e,
                   "SELECT COUNT(*) FROM code_relations WHERE kind='file_includes_file'"
                   " AND dst_name_text='only.h' AND resolution='BUILD_METADATA'"
                   " AND provenance='BUILD_METADATA';",
                   &err),
             1);
    /* The translation unit is recorded and linked to its source. */
    T_EQ_INT(count(&e,
                   "SELECT COUNT(*) FROM code_relations WHERE kind='unit_compiles_file';", &err),
             1);
    atlas_reconcile_summary_free(&sum);
    atlas_buf_free(&json);
    env_stop(&e);
}

static void test_external_include_dirs_are_metadata_only(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_start(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "u.c", "#include <stdio.h>\nint u(void){return 0;}\n", &err),
         &err);
    atlas_buf json = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&json, &err,
                           "[{\"directory\":\"%s\",\"file\":\"u.c\","
                           "\"arguments\":[\"cc\",\"-isystem\",\"/usr/include\",\"-c\",\"u.c\"]}]\n",
                           fx_repo(&e.fx)),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", atlas_buf_cstr(&json), &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_register(&e, &err);
    atlas_reconcile_summary sum;
    pass(&e, &sum, &err);

    /* Recorded, and marked external. */
    T_EQ_INT(count(&e, "SELECT COUNT(*) FROM code_unit_includes WHERE external=1;", &err), 1);
    /* And it resolves nothing: recording where a build looks is not the same as
     * being allowed to look there, and `<stdio.h>` stays unresolved. */
    T_EQ_INT(count(&e,
                   "SELECT COUNT(*) FROM code_relations WHERE dst_name_text='stdio.h'"
                   " AND resolution='UNRESOLVED';",
                   &err),
             1);
    atlas_reconcile_summary_free(&sum);
    atlas_buf_free(&json);
    env_stop(&e);
}

static void test_compile_database_change_is_noticed(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_start(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "u.c", "int u(void){return 0;}\n", &err), &err);
    atlas_buf json = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&json, &err,
                           "[{\"directory\":\"%s\",\"file\":\"u.c\","
                           "\"arguments\":[\"cc\",\"-DA\",\"-c\",\"u.c\"]}]\n",
                           fx_repo(&e.fx)),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", atlas_buf_cstr(&json), &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_register(&e, &err);

    atlas_reconcile_summary first;
    pass(&e, &first, &err);
    T_CHECK(first.code.compile_db_changed);
    T_EQ_INT(count(&e, "SELECT COUNT(*) FROM code_unit_defines WHERE name='A';", &err), 1);
    atlas_reconcile_summary_free(&first);

    /* An unchanged database is not re-ingested: the recorded hash is what the
     * pass compares, so a repeated pass does no work and writes no rows. */
    atlas_reconcile_summary second;
    pass(&e, &second, &err);
    T_CHECK(!second.code.compile_db_changed);
    atlas_reconcile_summary_free(&second);

    atlas_buf_reset(&json);
    T_OK(atlas_buf_appendf(&json, &err,
                           "[{\"directory\":\"%s\",\"file\":\"u.c\","
                           "\"arguments\":[\"cc\",\"-DB\",\"-c\",\"u.c\"]}]\n",
                           fx_repo(&e.fx)),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", atlas_buf_cstr(&json), &err), &err);
    atlas_reconcile_summary third;
    pass(&e, &third, &err);
    T_CHECK(third.code.compile_db_changed);
    /* Replaced wholesale, not merged: a define that left the build must leave
     * the index. */
    T_EQ_INT(count(&e, "SELECT COUNT(*) FROM code_unit_defines WHERE name='A';", &err), 0);
    T_EQ_INT(count(&e, "SELECT COUNT(*) FROM code_unit_defines WHERE name='B';", &err), 1);
    atlas_reconcile_summary_free(&third);
    atlas_buf_free(&json);
    env_stop(&e);
}

static void test_malformed_database_does_not_stop_indexing(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_start(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "u.c", "int u(void){return 0;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", "{ this is not a compile database\n",
                  &err),
         &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_register(&e, &err);
    atlas_reconcile_summary sum;
    pass(&e, &sum, &err);

    /* The structural index is still built; only the build metadata is missing,
     * and the reason is recorded rather than the whole pass failing. */
    T_EQ_INT(count(&e, "SELECT COUNT(*) FROM code_symbols WHERE name_text='u';", &err), 1);
    T_EQ_INT(count(&e, "SELECT COUNT(*) FROM code_units;", &err), 0);
    T_CHECK(count(&e, "SELECT COUNT(*) FROM code_index_errors WHERE kind='compile_db_error';",
                  &err) >= 1);
    atlas_reconcile_summary_free(&sum);
    env_stop(&e);
}


/* The crash window between applying a file and finishing the pass.
 *
 * A unit's edges are *owned by* the unit's source file, so reparsing that file
 * deletes them along with everything else the file owns, and the same pass puts
 * them back after resolution. A pass that dies in between leaves them missing —
 * and the recovery pass parses nothing, so it has no file to relink by id. If
 * recovery only re-resolved, the unit edges would stay durably absent with
 * nothing ever noticing: the compile database is unchanged, so it is not
 * re-ingested, and no later edit to any other file would restore them.
 *
 * `resolve_settled` is the flag that says a pass died mid-flight, and it has to
 * drive the relink as well as the resolution, because the same dead pass
 * damaged both. */
static void test_a_crashed_pass_relinks_the_unit_edges(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_start(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "u.c", "#include \"u.h\"\nint u(void){return 0;}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "u.h", "int u(void);\n", &err), &err);
    atlas_buf json = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&json, &err,
                           "[{\"directory\":\"%s\",\"file\":\"u.c\","
                           "\"arguments\":[\"cc\",\"-I.\",\"-c\",\"u.c\"]}]\n",
                           fx_repo(&e.fx)),
         &err);
    T_OK(fx_write(fx_repo(&e.fx), "compile_commands.json", atlas_buf_cstr(&json), &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "seed", &err), &err);
    env_register(&e, &err);

    atlas_reconcile_summary first;
    pass(&e, &first, &err);
    int64_t linked = count(&e,
                           "SELECT COUNT(*) FROM code_relations"
                           " WHERE kind IN ('unit_compiles_file','unit_uses_header');",
                           &err);
    T_CHECK(linked > 0);
    atlas_reconcile_summary_free(&first);

    /* The state a killed pass leaves: the file's rows were replaced, taking the
     * unit edges with them, and `resolve_settled` was never set again. Nothing
     * else changes — the compile database is byte-identical and no file is
     * touched, so an ordinary pass would find nothing to do. */
    T_OK(atlas_db_exec_sql(e.db,
                           "DELETE FROM code_relations"
                           " WHERE kind IN ('unit_compiles_file','unit_uses_header');"
                           "UPDATE code_index_state SET resolve_settled=0;",
                           &err),
         &err);
    T_EQ_INT(count(&e,
                   "SELECT COUNT(*) FROM code_relations"
                   " WHERE kind IN ('unit_compiles_file','unit_uses_header');",
                   &err),
             0);

    atlas_reconcile_summary recovered;
    pass(&e, &recovered, &err);
    T_EQ_INT(recovered.code.files_parsed, 0);
    T_CHECK(!recovered.code.compile_db_changed);
    T_EQ_INT(count(&e,
                   "SELECT COUNT(*) FROM code_relations"
                   " WHERE kind IN ('unit_compiles_file','unit_uses_header');",
                   &err),
             linked);
    atlas_reconcile_summary_free(&recovered);
    atlas_buf_free(&json);
    env_stop(&e);
}

static const atlas_test TESTS[] = {
    {"the argument allowlist", test_arguments_allowlist},
    {"the command is hashed, not stored", test_command_is_hashed_not_stored},
    {"hostile command strings", test_hostile_command_strings},
    {"response files and plugins are dropped", test_response_files_and_plugins_are_dropped},
    {"paths outside the repository", test_paths_outside_the_repository},
    {"traversal is folded, not followed", test_traversal_is_folded_not_followed},
    {"duplicate and multi-configuration entries", test_duplicate_and_multi_config_entries},
    {"malformed documents", test_malformed_documents},
    {"oversize documents", test_oversize_document_is_refused_before_parsing},
    {"the entry ceiling", test_entry_ceiling},
    {"no compile database", test_no_compile_database_still_works},
    {"nothing is executed", test_nothing_from_a_compile_database_is_executed},
    {"build metadata resolves an include", test_build_metadata_resolves_an_include},
    {"external include directories are metadata", test_external_include_dirs_are_metadata_only},
    {"a changed database is noticed", test_compile_database_change_is_noticed},
    {"a malformed database does not stop indexing", test_malformed_database_does_not_stop_indexing},
    {"a crashed pass relinks the unit edges", test_a_crashed_pass_relinks_the_unit_edges},
};

ATLAS_TEST_MAIN("code_compdb", TESTS)

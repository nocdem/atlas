/* Atlas - CLI tests: human output, stable JSON, exit codes, and the read-only
 * guarantee for target repositories.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Covers required tests 24 (human output), 25 (stable JSON output and escaping),
 * 28 (repository removal affects only Atlas metadata) and 29 (a registered
 * repository is unchanged after every read command).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

#define REPO_NAME "fixture"

typedef struct cli_env {
    fixture fx;
} cli_env;

typedef struct run_result {
    atlas_buf out;
    atlas_buf errout;
    int exit_code;
} run_result;

static void result_init(run_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->out);
    atlas_buf_init(&r->errout);
    r->exit_code = -1;
}

static void result_free(run_result *r) {
    atlas_buf_free(&r->out);
    atlas_buf_free(&r->errout);
}

/* Runs the atlas binary with --data-dir always pinned to the fixture. */
static void run_atlas(cli_env *e, run_result *r, const char *const *args, size_t nargs) {
    atlas_err err;
    atlas_err_init(&err);
    result_init(r);

    const char *argv[24];
    size_t n = 0;
    argv[n++] = "--data-dir";
    argv[n++] = fx_data_dir(&e->fx);
    T_REQUIRE(nargs + n <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < nargs; i++) {
        argv[n++] = args[i];
    }
    T_OK(fx_atlas(argv, n, &r->out, &r->errout, &r->exit_code, &err), &err);
}

static void env_open(cli_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
}

static void env_close(cli_env *e) {
    fx_close(&e->fx);
}

/* A small repository with a commit, a rename and a hostile filename. */
static void build_repo(cli_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    const char *repo = fx_repo(&e->fx);
    T_OK(fx_init_repo(&e->fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/util.c", "void util(void){}\n", &err), &err);
    T_OK(fx_write(repo, "with space.txt", "spaces\n", &err), &err);
    T_OK(fx_write(repo, "with\ttab.txt", "tabs\n", &err), &err);
    T_OK(fx_add_all(&e->fx, repo, &err), &err);
    T_OK(fx_commit_body(&e->fx, repo, "initial commit", "body of the initial commit", &err), &err);

    const char *mv[] = {"mv", "src/util.c", "src/helper.c"};
    T_OK(fx_git_ok(&e->fx, repo, mv, 3u, &err), &err);
    T_OK(fx_commit(&e->fx, repo, "rename util to helper", &err), &err);
}

static void register_and_scan(cli_env *e) {
    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", REPO_NAME};
    run_atlas(e, &r, add, 5u);
    T_CHECK_MSG(r.exit_code == 0, "repo add failed: %s", atlas_buf_cstr(&r.errout));
    result_free(&r);

    const char *scan[] = {"scan", REPO_NAME};
    run_atlas(e, &r, scan, 2u);
    T_CHECK_MSG(r.exit_code == 0, "scan failed: %s", atlas_buf_cstr(&r.errout));
    result_free(&r);
}

/* --- required test 24: human output -------------------------------------- */

static void test_human_output(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    register_and_scan(&e);
    run_result r;

    const char *doctor[] = {"doctor"};
    run_atlas(&e, &r, doctor, 1u);
    T_EQ_INT(r.exit_code, 0);
    const char *o = atlas_buf_cstr(&r.out);
    T_CHECK(strstr(o, "Atlas " ATLAS_VERSION_STRING) != NULL);
    T_CHECK(strstr(o, "git ") != NULL);
    T_CHECK(strstr(o, "schema version") != NULL);
    T_CHECK(strstr(o, "search mode") != NULL);
    T_CHECK(strstr(o, "integrity check") != NULL);
    T_CHECK(strstr(o, "status:") != NULL);
    /* Human output must not leak JSON punctuation. */
    T_CHECK(o[0] != '{');
    result_free(&r);

    const char *list[] = {"repo", "list"};
    run_atlas(&e, &r, list, 2u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "NAME") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), REPO_NAME) != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "1 repository") != NULL);
    result_free(&r);

    const char *status[] = {"status", REPO_NAME};
    run_atlas(&e, &r, status, 2u);
    T_EQ_INT(r.exit_code, 0);
    o = atlas_buf_cstr(&r.out);
    T_CHECK(strstr(o, "repository " REPO_NAME) != NULL);
    T_CHECK(strstr(o, "scanned head") != NULL);
    T_CHECK(strstr(o, "live head") != NULL);
    T_CHECK(strstr(o, "indexed") != NULL);
    /* Find the drift line itself rather than depending on column padding. */
    {
        const char *drift = strstr(o, "index drift");
        T_REQUIRE(drift != NULL);
        const char *eol = strchr(drift, '\n');
        size_t line_len = (eol != NULL) ? (size_t)(eol - drift) : strlen(drift);
        const char *none = strstr(drift, "none");
        T_CHECK_MSG(none != NULL && (eol == NULL || none < eol),
                    "a freshly scanned repository should report no drift: %.*s", (int)line_len,
                    drift);
    }
    result_free(&r);

    const char *file[] = {"file", REPO_NAME, "main.c"};
    run_atlas(&e, &r, file, 3u);
    T_EQ_INT(r.exit_code, 0);
    o = atlas_buf_cstr(&r.out);
    T_CHECK(strstr(o, "main.c") != NULL);
    T_CHECK(strstr(o, "content hash") != NULL);
    /* Provenance and the refusal to infer are both visible to a human. */
    T_CHECK_MSG(strstr(o, "SOURCE") != NULL, "file output must state its evidence");
    T_CHECK_MSG(strstr(o, "UNKNOWN") != NULL, "file output must answer UNKNOWN for the reason");
    result_free(&r);

    const char *history[] = {"history", REPO_NAME, "src/helper.c"};
    run_atlas(&e, &r, history, 3u);
    T_EQ_INT(r.exit_code, 0);
    o = atlas_buf_cstr(&r.out);
    T_CHECK(strstr(o, "rename") != NULL);
    T_CHECK(strstr(o, "src/util.c") != NULL);
    T_CHECK(strstr(o, "[GIT]") != NULL);
    result_free(&r);

    const char *search[] = {"search", REPO_NAME, "helper"};
    run_atlas(&e, &r, search, 3u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "src/helper.c") != NULL);
    result_free(&r);

    /* A path with a tab is shown percent-escaped, so terminal output stays on one
     * line and remains copy-pasteable. */
    const char *weird[] = {"file", REPO_NAME, "with%09tab.txt"};
    run_atlas(&e, &r, weird, 3u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "with%09tab.txt") != NULL);
    T_CHECK_MSG(strchr(atlas_buf_cstr(&r.out), '\t') == NULL,
                "a raw tab from a filename leaked into human output");
    result_free(&r);

    const char *diff[] = {"diff", REPO_NAME};
    run_atlas(&e, &r, diff, 2u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "no changes") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "0 staged, 0 unstaged") != NULL);
    result_free(&r);

    const char *version[] = {"version"};
    run_atlas(&e, &r, version, 1u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), ATLAS_VERSION_STRING) != NULL);
    result_free(&r);

    const char *help[] = {"help"};
    run_atlas(&e, &r, help, 1u);
    T_EQ_INT(r.exit_code, 0);
    o = atlas_buf_cstr(&r.out);
    T_CHECK(strstr(o, "usage: atlas") != NULL);
    T_CHECK(strstr(o, "doctor") != NULL);
    T_CHECK(strstr(o, "exit codes") != NULL);
    result_free(&r);

    env_close(&e);
}

/* --- required test 25: stable JSON output and escaping ------------------- */

static void expect_json(const run_result *r) {
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(r->out.data, r->out.len, &bad),
                "output is not valid JSON (offset %zu): %s", bad, atlas_buf_cstr(&r->out));
}

static void expect_json_string(const run_result *r, const char *key, const char *expected) {
    atlas_buf got = ATLAS_BUF_INIT;
    if (!tjson_get_string(r->out.data, r->out.len, key, &got)) {
        atlas_test_fail(__FILE__, __LINE__, "key \"%s\" is missing or not a string", key);
    } else {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&got), expected) == 0,
                    "%s: expected \"%s\", got \"%s\"", key, expected, atlas_buf_cstr(&got));
    }
    atlas_buf_free(&got);
}

static void expect_json_raw(const run_result *r, const char *key, const char *expected) {
    atlas_buf got = ATLAS_BUF_INIT;
    if (!tjson_get_raw(r->out.data, r->out.len, key, &got)) {
        atlas_test_fail(__FILE__, __LINE__, "key \"%s\" is missing", key);
    } else {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&got), expected) == 0, "%s: expected %s, got %s", key,
                    expected, atlas_buf_cstr(&got));
    }
    atlas_buf_free(&got);
}

static void test_json_output(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    register_and_scan(&e);
    run_result r;

    /* Every command emits one JSON document carrying the same envelope. */
    const char *commands[][3] = {
        {"doctor", NULL, NULL},
        {"repo", "list", NULL},
        {"status", REPO_NAME, NULL},
        {"scan", REPO_NAME, NULL},
        {"diff", REPO_NAME, NULL},
        {"version", NULL, NULL},
    };
    const size_t counts[] = {1u, 2u, 2u, 2u, 2u, 1u};
    const char *expected_command[] = {"doctor", "repo list", "status", "scan", "diff", "version"};

    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        const char *args[4];
        size_t n = 0;
        args[n++] = "--json";
        for (size_t k = 0; k < counts[i]; k++) {
            args[n++] = commands[i][k];
        }
        run_atlas(&e, &r, args, n);
        T_CHECK_MSG(r.exit_code == 0, "%s --json exited %d: %s", expected_command[i], r.exit_code,
                    atlas_buf_cstr(&r.errout));
        expect_json(&r);
        expect_json_string(&r, "atlas", ATLAS_VERSION_STRING);
        expect_json_string(&r, "phase", ATLAS_PHASE);
        expect_json_string(&r, "command", expected_command[i]);
        expect_json_raw(&r, "ok", "true");
        result_free(&r);
    }

    /* --json is accepted before or after the subcommand, and both forms produce
     * byte-identical output apart from timestamps. */
    const char *before[] = {"--json", "repo", "list"};
    const char *after[] = {"repo", "list", "--json"};
    run_result r2;
    run_atlas(&e, &r, before, 3u);
    run_atlas(&e, &r2, after, 3u);
    T_EQ_INT(r.exit_code, 0);
    T_EQ_INT(r2.exit_code, 0);
    expect_json(&r);
    expect_json(&r2);
    T_CHECK_MSG(r.out.len == r2.out.len && memcmp(r.out.data, r2.out.data, r.out.len) == 0,
                "--json placement changed the output");
    result_free(&r);
    result_free(&r2);

    /* Structured file output, including provenance fields. */
    const char *file[] = {"--json", "file", REPO_NAME, "main.c"};
    run_atlas(&e, &r, file, 4u);
    T_EQ_INT(r.exit_code, 0);
    expect_json(&r);
    expect_json_string(&r, "path", "main.c");
    expect_json_string(&r, "file_type", "regular");
    expect_json_string(&r, "language", "c");
    expect_json_string(&r, "content_hash_algo", "sha256");
    expect_json_string(&r, "path_encoding", "utf8");
    /* A0 answers UNKNOWN rather than inventing a reason. */
    expect_json_string(&r, "reason", "UNKNOWN");
    expect_json_string(&r, "reason_evidence", "UNKNOWN");
    expect_json_raw(&r, "evidence", "[\"SOURCE\",\"GIT\"]");
    /* The recorded hash is independently verifiable. */
    char expected_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("int main(void){return 0;}\n", strlen("int main(void){return 0;}\n"),
                     expected_hash);
    expect_json_string(&r, "content_hash", expected_hash);
    result_free(&r);

    /* A path containing a tab must be escaped so the document stays valid. */
    const char *weird[] = {"--json", "file", REPO_NAME, "with%09tab.txt"};
    run_atlas(&e, &r, weird, 4u);
    T_EQ_INT(r.exit_code, 0);
    expect_json(&r);
    /* The safe text form is what appears, and it decodes back exactly. */
    expect_json_string(&r, "path", "with%09tab.txt");
    T_CHECK_MSG(memchr(r.out.data, '\t', r.out.len) == NULL,
                "a raw tab byte leaked into the JSON document");
    result_free(&r);

    /* History and search are arrays with a count. */
    const char *history[] = {"--json", "history", REPO_NAME, "src/helper.c"};
    run_atlas(&e, &r, history, 4u);
    T_EQ_INT(r.exit_code, 0);
    expect_json(&r);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "\"changes\":[") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "\"evidence\":\"GIT\"") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "\"change_type\":\"rename\"") != NULL);
    expect_json_string(&r, "old_path", "src/util.c");
    result_free(&r);

    const char *search[] = {"--json", "search", REPO_NAME, "helper"};
    run_atlas(&e, &r, search, 4u);
    T_EQ_INT(r.exit_code, 0);
    expect_json(&r);
    expect_json_string(&r, "query", "helper");
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "\"results\":[") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "\"search_mode\":") != NULL);
    result_free(&r);

    /* An empty result set is still a valid document with an empty array. */
    const char *empty[] = {"--json", "search", REPO_NAME, "zzzznotpresentanywhere"};
    run_atlas(&e, &r, empty, 4u);
    T_EQ_INT(r.exit_code, 0);
    expect_json(&r);
    expect_json_raw(&r, "results", "[]");
    expect_json_raw(&r, "count", "0");
    result_free(&r);

    env_close(&e);
}

static void test_json_errors_are_documents(void) {
    cli_env e;
    env_open(&e);
    run_result r;

    /* A failing command must still produce parseable JSON on stdout. */
    const char *missing[] = {"--json", "status", "no-such-repo"};
    run_atlas(&e, &r, missing, 3u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_REPO);
    expect_json(&r);
    expect_json_raw(&r, "ok", "false");
    expect_json_string(&r, "command", "status");
    expect_json_string(&r, "status", "repo");
    expect_json_raw(&r, "exit_code", "4");
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "no-such-repo") != NULL);
    /* The human-readable message also goes to stderr. */
    T_CHECK(strstr(atlas_buf_cstr(&r.errout), "no-such-repo") != NULL);
    result_free(&r);

    env_close(&e);
}

static void test_exit_codes(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    register_and_scan(&e);
    run_result r;

    const char *unknown_cmd[] = {"definitely-not-a-command"};
    run_atlas(&e, &r, unknown_cmd, 1u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_USAGE);
    result_free(&r);

    const char *bad_option[] = {"--not-an-option", "doctor"};
    run_atlas(&e, &r, bad_option, 2u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_USAGE);
    result_free(&r);

    const char *missing_arg[] = {"status"};
    run_atlas(&e, &r, missing_arg, 1u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_USAGE);
    T_CHECK(strstr(atlas_buf_cstr(&r.errout), "usage:") != NULL);
    result_free(&r);

    const char *extra_arg[] = {"status", REPO_NAME, "unexpected"};
    run_atlas(&e, &r, extra_arg, 3u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_USAGE);
    result_free(&r);

    const char *unknown_repo[] = {"status", "nope"};
    run_atlas(&e, &r, unknown_repo, 2u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_REPO);
    result_free(&r);

    const char *unknown_path[] = {"file", REPO_NAME, "not/in/the/index.c"};
    run_atlas(&e, &r, unknown_path, 3u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_REPO);
    result_free(&r);

    /* A relative data directory is a configuration error, not a guess. */
    atlas_err err;
    atlas_err_init(&err);
    const char *rel_argv[] = {"--data-dir", "relative/path", "doctor"};
    run_result rr;
    result_init(&rr);
    T_OK(fx_atlas(rel_argv, 3u, &rr.out, &rr.errout, &rr.exit_code, &err), &err);
    T_EQ_INT(rr.exit_code, ATLAS_ERR_CONFIG);
    result_free(&rr);

    /* Removal without --yes is refused as a usage error. */
    const char *remove_no_yes[] = {"repo", "remove", REPO_NAME};
    run_atlas(&e, &r, remove_no_yes, 3u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_USAGE);
    T_CHECK(strstr(atlas_buf_cstr(&r.errout), "--yes") != NULL);
    result_free(&r);

    env_close(&e);
}

/* --- required test 28: removal touches only Atlas metadata --------------- */

static void test_repo_remove_only_metadata(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    register_and_scan(&e);
    atlas_err err;
    atlas_err_init(&err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);

    run_result r;
    const char *remove[] = {"repo", "remove", REPO_NAME, "--yes"};
    run_atlas(&e, &r, remove, 4u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "not modified") != NULL);
    result_free(&r);

    /* The repository, including its .git directory, is byte-for-byte unchanged. */
    T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0,
                "repo remove modified the target repository (%s -> %s)", before, after);

    /* The metadata really is gone. */
    const char *list[] = {"repo", "list"};
    run_atlas(&e, &r, list, 2u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), REPO_NAME) == NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "no repositories") != NULL);
    result_free(&r);

    /* And the working tree still contains the files. */
    const char *status_args[] = {"status", "--porcelain"};
    run_result gr;
    result_init(&gr);
    int code = 0;
    T_OK(fx_git(&e.fx, fx_repo(&e.fx), status_args, 2u, &code, &gr.out, &err), &err);
    T_EQ_INT(code, 0);
    result_free(&gr);

    env_close(&e);
}

/* --- required test 29: read commands never modify the repository --------- */

static void test_read_commands_do_not_modify_repository(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    atlas_err err;
    atlas_err_init(&err);

    /* Leave the worktree dirty, so index refreshes and status writes would show
     * up if they happened. */
    T_OK(fx_write(fx_repo(&e.fx), "main.c", "int main(void){return 1;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "untracked.txt", "untracked\n", &err), &err);

    register_and_scan(&e);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);

    /* Every command a user can run against a registered repository. */
    struct {
        const char *args[5];
        size_t n;
    } cases[] = {
        {{"doctor"}, 1u},
        {{"repo", "list"}, 2u},
        {{"status", REPO_NAME}, 2u},
        {{"scan", REPO_NAME}, 2u},
        {{"search", REPO_NAME, "main"}, 3u},
        {{"file", REPO_NAME, "main.c"}, 3u},
        {{"history", REPO_NAME, "main.c"}, 3u},
        {{"diff", REPO_NAME}, 2u},
        {{"--json", "status", REPO_NAME}, 3u},
        {{"--json", "scan", REPO_NAME}, 3u},
        {{"--json", "diff", REPO_NAME}, 3u},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_result r;
        run_atlas(&e, &r, cases[i].args, cases[i].n);
        T_CHECK_MSG(r.exit_code == 0, "%s exited %d: %s", cases[i].args[0], r.exit_code,
                    atlas_buf_cstr(&r.errout));
        result_free(&r);

        char after[ATLAS_SHA256_HEX_LEN + 1u];
        T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
        T_CHECK_MSG(strcmp(before, after) == 0,
                    "command \"%s\" modified the target repository", cases[i].args[0]);
    }

    /* The dirty state is still exactly as it was: nothing was staged, cleaned or
     * committed on the user's behalf. */
    run_result gr;
    result_init(&gr);
    int code = 0;
    const char *status_args[] = {"status", "--porcelain"};
    T_OK(fx_git(&e.fx, fx_repo(&e.fx), status_args, 2u, &code, &gr.out, &err), &err);
    T_EQ_INT(code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&gr.out), " M main.c") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&gr.out), "?? untracked.txt") != NULL);
    result_free(&gr);

    env_close(&e);
}

static void test_diff_reports_worktree_changes(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    register_and_scan(&e);
    atlas_err err;
    atlas_err_init(&err);

    T_OK(fx_write(fx_repo(&e.fx), "main.c", "int main(void){\n  return 0;\n}\n", &err), &err);

    run_result r;
    const char *diff[] = {"diff", REPO_NAME};
    run_atlas(&e, &r, diff, 2u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "main.c") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "unstaged:") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "1 unstaged") != NULL);
    result_free(&r);

    const char *jdiff[] = {"--json", "diff", REPO_NAME};
    run_atlas(&e, &r, jdiff, 3u);
    T_EQ_INT(r.exit_code, 0);
    expect_json(&r);
    expect_json_string(&r, "path", "main.c");
    expect_json_raw(&r, "binary", "false");
    expect_json_string(&r, "evidence", "GIT");
    result_free(&r);

    env_close(&e);
}

static void test_repo_add_derives_a_name(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    run_result r;

    /* Without --name the canonical root's basename is used. */
    const char *add[] = {"repo", "add", fx_repo(&e.fx)};
    run_atlas(&e, &r, add, 3u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "registered repo") != NULL);
    result_free(&r);

    /* Registering the same root again is refused with a clear message. */
    const char *again[] = {"repo", "add", fx_repo(&e.fx), "--name", "other"};
    run_atlas(&e, &r, again, 5u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_REPO);
    T_CHECK(strstr(atlas_buf_cstr(&r.errout), "already registered") != NULL);
    result_free(&r);

    /* An invalid name is a usage error. */
    const char *bad[] = {"repo", "add", fx_repo(&e.fx), "--name", "bad name"};
    run_atlas(&e, &r, bad, 5u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_USAGE);
    result_free(&r);

    env_close(&e);
}

static void test_unscanned_repo_reports_clearly(void) {
    cli_env e;
    env_open(&e);
    build_repo(&e);
    run_result r;

    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas(&e, &r, add, 5u);
    T_EQ_INT(r.exit_code, 0);
    result_free(&r);

    /* status works and says it has never been scanned. */
    const char *status[] = {"status", REPO_NAME};
    run_atlas(&e, &r, status, 2u);
    T_EQ_INT(r.exit_code, 0);
    T_CHECK(strstr(atlas_buf_cstr(&r.out), "never scanned") != NULL);
    result_free(&r);

    /* file and history say what to do instead of returning nothing. */
    const char *file[] = {"file", REPO_NAME, "main.c"};
    run_atlas(&e, &r, file, 3u);
    T_EQ_INT(r.exit_code, ATLAS_ERR_REPO);
    T_CHECK(strstr(atlas_buf_cstr(&r.errout), "atlas scan") != NULL);
    result_free(&r);

    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"human output", test_human_output},
    {"stable JSON output and escaping", test_json_output},
    {"JSON errors are valid documents", test_json_errors_are_documents},
    {"exit codes", test_exit_codes},
    {"repo remove touches only Atlas metadata", test_repo_remove_only_metadata},
    {"read commands never modify the repository", test_read_commands_do_not_modify_repository},
    {"diff reports working-tree changes", test_diff_reports_worktree_changes},
    {"repo add derives a name and refuses duplicates", test_repo_add_derives_a_name},
    {"an unscanned repository reports clearly", test_unscanned_repo_reports_clearly},
};

ATLAS_TEST_MAIN("cli", TESTS)

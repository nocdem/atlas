/* Atlas - adversarial tests for git subprocess hardening.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The argv allowlist stops Atlas from asking git to write. It does nothing about
 * git being *configured* to execute a helper: an fsmonitor hook, an external diff
 * driver, a textconv filter, a pager, an askpass program. Those are closed by the
 * constructed environment and the `-c` prefix in src/git/git_harden.c, and this is
 * where that is proven.
 *
 * Each vector test has two halves, and both matter:
 *
 *   CONTROL  plain git, driven by the fixture, must actually run the helper. A
 *            vector that cannot fire would make the assertion below vacuous.
 *   ATLAS    every Atlas command against the same repository must not run it.
 *
 * The helper is tests/tools/atlas_marker.c, copied into the fixture so the config
 * value is a bare absolute path: git execs it directly, with no shell involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "git/git_harden.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

#define REPO_NAME "hostile"

typedef struct harden_env {
    fixture fx;
    atlas_buf helper;  /* absolute path to the marker helper */
    atlas_buf marker;  /* the file it creates if it ever runs */
} harden_env;

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

static void env_open(harden_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->helper);
    atlas_buf_init(&e->marker);
    /* Installed in the fixture root, not the repository, so it is not itself a
     * tracked or untracked file that Atlas would report. */
    T_OK(fx_install_marker(atlas_buf_cstr(&e->fx.root), "hostile-helper", &e->helper, &e->marker,
                           &err),
         &err);
}

static void env_close(harden_env *e) {
    atlas_buf_free(&e->helper);
    atlas_buf_free(&e->marker);
    fx_close(&e->fx);
}

/* A repository with something to diff, something staged and something untracked,
 * so every Atlas command has real work to do. */
static void build_repo(harden_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    const char *repo = fx_repo(&e->fx);
    T_OK(fx_init_repo(&e->fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "one\ntwo\n", &err), &err);
    /* Byte-exact: fx_write would stop at the leading NUL and create an empty file,
     * leaving nothing for a textconv filter to render. */
    static const char bin1[] = {0x00, 0x01, 'b', 'i', 'n', 0x00, 0x02};
    T_OK(fx_write_bytes(repo, "b.bin", 5u, bin1, sizeof(bin1), 0644, &err), &err);
    T_OK(fx_add_all(&e->fx, repo, &err), &err);
    T_OK(fx_commit(&e->fx, repo, "initial commit", &err), &err);
    T_OK(fx_write(repo, "a.txt", "one\nthree\n", &err), &err);
    T_OK(fx_write(repo, "staged.txt", "s\n", &err), &err);
    const char *add[] = {"add", "--", "staged.txt"};
    T_OK(fx_git_ok(&e->fx, repo, add, 3u, &err), &err);
    T_OK(fx_write(repo, "untracked.txt", "u\n", &err), &err);
}

static void set_repo_config(harden_env *e, const char *key, const char *value) {
    atlas_err err;
    atlas_err_init(&err);
    const char *args[] = {"config", key, value};
    T_OK(fx_git_ok(&e->fx, fx_repo(&e->fx), args, 3u, &err), &err);
}

static void run_atlas_in(harden_env *e, run_result *r, const char *const *args, size_t nargs,
                         const char *const *extra_env) {
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
    T_OK(fx_atlas_env(argv, n, extra_env, &r->out, &r->errout, &r->exit_code, &err), &err);
}

/* Runs every Atlas command that touches the repository and asserts the helper
 * never ran and the repository never changed. */
static void expect_atlas_never_runs_helper(harden_env *e, const char *const *extra_env,
                                           const char *vector) {
    atlas_err err;
    atlas_err_init(&err);
    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e->fx), before, &err), &err);
    fx_marker_clear(atlas_buf_cstr(&e->marker));

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", REPO_NAME};
    run_atlas_in(e, &r, add, 5u, extra_env);
    T_CHECK_MSG(r.exit_code == 0, "%s: repo add exited %d: %s", vector, r.exit_code,
                atlas_buf_cstr(&r.errout));
    result_free(&r);

    struct {
        const char *args[4];
        size_t n;
    } cases[] = {
        {{"scan", REPO_NAME}, 2u},
        {{"status", REPO_NAME}, 2u},
        {{"diff", REPO_NAME}, 2u},
        {{"search", REPO_NAME, "one"}, 3u},
        {{"file", REPO_NAME, "a.txt"}, 3u},
        {{"history", REPO_NAME, "a.txt"}, 3u},
        {{"--json", "diff", REPO_NAME}, 3u},
        {{"--json", "status", REPO_NAME}, 3u},
        {{"scan", REPO_NAME}, 2u}, /* twice, so a cached-index path is exercised */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_atlas_in(e, &r, cases[i].args, cases[i].n, extra_env);
        T_CHECK_MSG(r.exit_code == 0, "%s: %s exited %d: %s", vector, cases[i].args[0],
                    r.exit_code, atlas_buf_cstr(&r.errout));
        result_free(&r);
        T_CHECK_MSG(!fx_marker_fired(atlas_buf_cstr(&e->marker)),
                    "%s: the hostile helper RAN during atlas %s", vector, cases[i].args[0]);
    }

    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e->fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "%s: the repository was modified", vector);
}

/* --- vector 1: repo-local core.fsmonitor -------------------------------- */

static void test_fsmonitor(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);
    set_repo_config(&e, "core.fsmonitor", atlas_buf_cstr(&e.helper));

    /* CONTROL: plain git really does execute the fsmonitor helper, on both of the
     * commands Atlas relies on. Without the -c override this is a live
     * code-execution path into every scan. */
    fx_marker_clear(atlas_buf_cstr(&e.marker));
    const char *status_args[] = {"status", "--porcelain=v2", "-z"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), status_args, 3u, &err), &err);
    T_CHECK_MSG(fx_marker_fired(atlas_buf_cstr(&e.marker)),
                "control failed: plain git status did not run the fsmonitor helper, so this "
                "test would prove nothing");

    fx_marker_clear(atlas_buf_cstr(&e.marker));
    const char *ls_args[] = {"ls-files", "-z", "--stage", "--cached"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), ls_args, 4u, &err), &err);
    T_CHECK_MSG(fx_marker_fired(atlas_buf_cstr(&e.marker)),
                "control failed: plain git ls-files did not run the fsmonitor helper");

    /* ATLAS: never. */
    expect_atlas_never_runs_helper(&e, NULL, "core.fsmonitor");
    env_close(&e);
}

/* --- vector 2: repo-local diff.external --------------------------------- */

static void test_diff_external(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);
    set_repo_config(&e, "diff.external", atlas_buf_cstr(&e.helper));

    /* CONTROL: a patch-producing diff runs the external driver. */
    fx_marker_clear(atlas_buf_cstr(&e.marker));
    const char *diff_args[] = {"diff"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), diff_args, 1u, &err), &err);
    T_CHECK_MSG(fx_marker_fired(atlas_buf_cstr(&e.marker)),
                "control failed: plain git diff did not run the external diff driver");

    expect_atlas_never_runs_helper(&e, NULL, "diff.external");
    env_close(&e);
}

/* --- vector 3: .gitattributes plus a textconv filter -------------------- */

static void test_textconv(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    /* Attach a diff driver to the binary file and give that driver a textconv
     * filter, which git runs to render the file as text. */
    T_OK(fx_write(fx_repo(&e.fx), ".gitattributes", "*.bin diff=hostile\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "add attributes", &err), &err);
    set_repo_config(&e, "diff.hostile.textconv", atlas_buf_cstr(&e.helper));
    /* Change the binary file so a diff has something to render. */
    static const char bin2[] = {0x00, 0x09, 'c', 'h', 'g', 0x00, 0x03, 0x04};
    T_OK(fx_write_bytes(fx_repo(&e.fx), "b.bin", 5u, bin2, sizeof(bin2), 0644, &err), &err);

    /* CONTROL: a patch-producing diff runs the textconv filter. */
    fx_marker_clear(atlas_buf_cstr(&e.marker));
    const char *diff_args[] = {"diff"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), diff_args, 1u, &err), &err);
    T_CHECK_MSG(fx_marker_fired(atlas_buf_cstr(&e.marker)),
                "control failed: plain git diff did not run the textconv filter");

    expect_atlas_never_runs_helper(&e, NULL, "diff.<driver>.textconv");
    env_close(&e);
}

/* --- vector 4: repository hooks ----------------------------------------- */

static void test_hooks_path(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    /* A hooks directory of the repository's choosing, containing every hook name a
     * read-only command might plausibly trigger. */
    T_OK(fx_mkdir(atlas_buf_cstr(&e.fx.root), "hooks", &err), &err);
    atlas_buf hookdir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&hookdir, &err, "%s/hooks", atlas_buf_cstr(&e.fx.root)), &err);
    const char *hook_names[] = {"post-index-change", "pre-auto-gc", "reference-transaction",
                                "fsmonitor-watchman"};
    for (size_t i = 0; i < sizeof(hook_names) / sizeof(hook_names[0]); i++) {
        atlas_buf ignore_helper = ATLAS_BUF_INIT;
        atlas_buf ignore_marker = ATLAS_BUF_INIT;
        /* Each hook is a copy of the marker, so its own name is the marker path. */
        T_OK(fx_install_marker(atlas_buf_cstr(&hookdir), hook_names[i], &ignore_helper,
                               &ignore_marker, &err),
             &err);
        atlas_buf_free(&ignore_helper);
        atlas_buf_free(&ignore_marker);
    }
    set_repo_config(&e, "core.hooksPath", atlas_buf_cstr(&hookdir));

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(atlas_buf_cstr(&hookdir), before, &err), &err);

    expect_atlas_never_runs_helper(&e, NULL, "core.hooksPath");

    /* No hook left a marker beside itself either. */
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(atlas_buf_cstr(&hookdir), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "a hook ran and left a marker in the hooks directory");
    atlas_buf_free(&hookdir);
    env_close(&e);
}

/* --- vector 5: hostile inherited environment ---------------------------- */

static void test_hostile_inherited_env(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    /* Everything a caller could plant in Atlas' own environment to make git
     * execute something or read from somewhere else. Atlas builds the child
     * environment from scratch, so none of this may reach git. */
    atlas_buf ext_diff = ATLAS_BUF_INIT;
    atlas_buf pager = ATLAS_BUF_INIT;
    atlas_buf gpager = ATLAS_BUF_INIT;
    atlas_buf askpass = ATLAS_BUF_INIT;
    atlas_buf ssh_askpass = ATLAS_BUF_INIT;
    atlas_buf ssh_cmd = ATLAS_BUF_INIT;
    atlas_buf cfg_value = ATLAS_BUF_INIT;
    const char *helper = atlas_buf_cstr(&e.helper);
    T_OK(atlas_buf_appendf(&ext_diff, &err, "GIT_EXTERNAL_DIFF=%s", helper), &err);
    T_OK(atlas_buf_appendf(&pager, &err, "PAGER=%s", helper), &err);
    T_OK(atlas_buf_appendf(&gpager, &err, "GIT_PAGER=%s", helper), &err);
    T_OK(atlas_buf_appendf(&askpass, &err, "GIT_ASKPASS=%s", helper), &err);
    T_OK(atlas_buf_appendf(&ssh_askpass, &err, "SSH_ASKPASS=%s", helper), &err);
    T_OK(atlas_buf_appendf(&ssh_cmd, &err, "GIT_SSH_COMMAND=%s", helper), &err);
    /* Config injection through the environment: one key/value pair setting
     * core.fsmonitor to the helper. */
    T_OK(atlas_buf_appendf(&cfg_value, &err, "GIT_CONFIG_VALUE_0=%s", helper), &err);

    const char *extra_env[] = {
        atlas_buf_cstr(&ext_diff),
        atlas_buf_cstr(&pager),
        atlas_buf_cstr(&gpager),
        atlas_buf_cstr(&askpass),
        atlas_buf_cstr(&ssh_askpass),
        atlas_buf_cstr(&ssh_cmd),
        "GIT_CONFIG_COUNT=1",
        "GIT_CONFIG_KEY_0=core.fsmonitor",
        atlas_buf_cstr(&cfg_value),
        "GIT_DIFF_OPTS=--hostile",
        "GIT_TRACE=1",
        "GIT_TRACE2=1",
        "GIT_NO_REPLACE_OBJECTS=",
        "GIT_OPTIONAL_LOCKS=1",
        "GIT_TERMINAL_PROMPT=1",
        NULL,
    };

    expect_atlas_never_runs_helper(&e, extra_env, "hostile inherited environment");

    /* And Atlas' own environment was not mutated on the way: the child got a
     * constructed environment instead. Checked by asking Atlas for the same
     * repository twice with and without the hostile variables and requiring
     * identical results. */
    run_result with_hostile;
    run_result without;
    const char *args[] = {"--json", "status", REPO_NAME};
    run_atlas_in(&e, &with_hostile, args, 3u, extra_env);
    run_atlas_in(&e, &without, args, 3u, NULL);
    T_EQ_INT(with_hostile.exit_code, 0);
    T_EQ_INT(without.exit_code, 0);
    atlas_buf a = ATLAS_BUF_INIT;
    atlas_buf b = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(with_hostile.out.data, with_hostile.out.len, "scanned_head", &a));
    T_CHECK(tjson_get_string(without.out.data, without.out.len, "scanned_head", &b));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&a), atlas_buf_cstr(&b)) == 0,
                "a hostile environment changed what Atlas reported (%s vs %s)",
                atlas_buf_cstr(&a), atlas_buf_cstr(&b));
    atlas_buf_free(&a);
    atlas_buf_free(&b);
    result_free(&with_hostile);
    result_free(&without);

    atlas_buf_free(&ext_diff);
    atlas_buf_free(&pager);
    atlas_buf_free(&gpager);
    atlas_buf_free(&askpass);
    atlas_buf_free(&ssh_askpass);
    atlas_buf_free(&ssh_cmd);
    atlas_buf_free(&cfg_value);
    env_close(&e);
}

/* --- vector 6: hostile inherited GIT_DIR / GIT_WORK_TREE ---------------- */

static void test_hostile_repository_selectors(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    /* A decoy repository with different content. If Atlas forwarded GIT_DIR or
     * GIT_WORK_TREE, git would read the decoy while Atlas reported the real
     * repository's name, which would be a silent, total misattribution. */
    atlas_buf decoy = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&decoy, &err, "%s/decoy", atlas_buf_cstr(&e.fx.root)), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&e.fx.root), "decoy", &err), &err);
    T_OK(fx_init_repo(&e.fx, atlas_buf_cstr(&decoy), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&decoy), "DECOY-ONLY.txt", "decoy\n", &err), &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&decoy), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&decoy), "decoy commit", &err), &err);

    atlas_buf gitdir = ATLAS_BUF_INIT;
    atlas_buf worktree = ATLAS_BUF_INIT;
    atlas_buf index_file = ATLAS_BUF_INIT;
    atlas_buf objdir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&gitdir, &err, "GIT_DIR=%s/.git", atlas_buf_cstr(&decoy)), &err);
    T_OK(atlas_buf_appendf(&worktree, &err, "GIT_WORK_TREE=%s", atlas_buf_cstr(&decoy)), &err);
    T_OK(atlas_buf_appendf(&index_file, &err, "GIT_INDEX_FILE=%s/.git/index",
                           atlas_buf_cstr(&decoy)),
         &err);
    T_OK(atlas_buf_appendf(&objdir, &err, "GIT_OBJECT_DIRECTORY=%s/.git/objects",
                           atlas_buf_cstr(&decoy)),
         &err);
    const char *extra_env[] = {
        atlas_buf_cstr(&gitdir),      atlas_buf_cstr(&worktree),
        atlas_buf_cstr(&index_file),  atlas_buf_cstr(&objdir),
        "GIT_COMMON_DIR=/nonexistent", "GIT_ALTERNATE_OBJECT_DIRECTORIES=/nonexistent",
        "GIT_NAMESPACE=hostile",       NULL,
    };

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas_in(&e, &r, add, 5u, extra_env);
    T_CHECK_MSG(r.exit_code == 0, "repo add exited %d: %s", r.exit_code,
                atlas_buf_cstr(&r.errout));
    result_free(&r);

    const char *scan[] = {"scan", REPO_NAME};
    run_atlas_in(&e, &r, scan, 2u, extra_env);
    T_CHECK_MSG(r.exit_code == 0, "scan exited %d: %s", r.exit_code, atlas_buf_cstr(&r.errout));
    result_free(&r);

    /* The real repository's files are indexed and the decoy's are not. */
    const char *file_real[] = {"file", REPO_NAME, "a.txt"};
    run_atlas_in(&e, &r, file_real, 3u, extra_env);
    T_CHECK_MSG(r.exit_code == 0, "the real repository's file should be indexed: %s",
                atlas_buf_cstr(&r.errout));
    result_free(&r);

    const char *file_decoy[] = {"file", REPO_NAME, "DECOY-ONLY.txt"};
    run_atlas_in(&e, &r, file_decoy, 3u, extra_env);
    T_CHECK_MSG(r.exit_code == ATLAS_ERR_REPO,
                "a file from the decoy repository was indexed: GIT_DIR was honoured");
    result_free(&r);

    /* The registered root is the real one, not the decoy. */
    const char *status[] = {"--json", "status", REPO_NAME};
    run_atlas_in(&e, &r, status, 3u, extra_env);
    T_EQ_INT(r.exit_code, 0);
    atlas_buf root = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(r.out.data, r.out.len, "root", &root));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&root), "/decoy") == NULL,
                "the decoy repository became the registered root: %s", atlas_buf_cstr(&root));
    atlas_buf_free(&root);
    result_free(&r);

    atlas_buf_free(&decoy);
    atlas_buf_free(&gitdir);
    atlas_buf_free(&worktree);
    atlas_buf_free(&index_file);
    atlas_buf_free(&objdir);
    env_close(&e);
}

/* --- vector 7: partial (promisor) repositories -------------------------- */

/* Writes the configuration a partial clone carries. Detection is what is under
 * test, so the markers are planted directly rather than depending on a clone
 * against a filtering server. */
static void make_promisor_config(harden_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    set_repo_config(e, "remote.origin.url", "https://example.invalid/repo.git");
    set_repo_config(e, "remote.origin.promisor", "true");
    set_repo_config(e, "remote.origin.partialclonefilter", "blob:none");
}

static void test_partial_clone_config_is_refused(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);
    make_promisor_config(&e);

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas_in(&e, &r, add, 5u, NULL);
    /* Fails closed: a repository whose objects may not all be local cannot be
     * read without risking a network fetch. */
    T_CHECK_MSG(r.exit_code == ATLAS_ERR_INTEGRITY,
                "expected exit %d for a promisor repository, got %d: %s", ATLAS_ERR_INTEGRITY,
                r.exit_code, atlas_buf_cstr(&r.errout));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&r.errout), "partial") != NULL,
                "the error should name the problem: %s", atlas_buf_cstr(&r.errout));
    result_free(&r);

    /* And the JSON form is a valid document a caller can act on. */
    const char *jadd[] = {"--json", "repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas_in(&e, &r, jadd, 6u, NULL);
    T_EQ_INT(r.exit_code, ATLAS_ERR_INTEGRITY);
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(r.out.data, r.out.len, &bad),
                "the failure must still be valid JSON (offset %zu): %s", bad,
                atlas_buf_cstr(&r.out));
    atlas_buf status_field = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_string(r.out.data, r.out.len, "status", &status_field));
    T_EQ_STR(atlas_buf_cstr(&status_field), "integrity");
    atlas_buf_free(&status_field);
    atlas_buf raw = ATLAS_BUF_INIT;
    T_CHECK(tjson_get_raw(r.out.data, r.out.len, "ok", &raw));
    T_EQ_STR(atlas_buf_cstr(&raw), "false");
    atlas_buf_free(&raw);
    result_free(&r);

    env_close(&e);
}

static void test_promisor_pack_is_refused(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    /* The other marker of a partial clone: a promisor pack. */
    T_OK(fx_mkdir(fx_repo(&e.fx), ".git/objects/pack", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), ".git/objects/pack/pack-atlas-test.promisor", "", &err), &err);

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas_in(&e, &r, add, 5u, NULL);
    T_CHECK_MSG(r.exit_code == ATLAS_ERR_INTEGRITY,
                "a promisor pack should fail closed, got exit %d: %s", r.exit_code,
                atlas_buf_cstr(&r.errout));
    T_CHECK(strstr(atlas_buf_cstr(&r.errout), "promisor") != NULL);
    result_free(&r);
    env_close(&e);
}

/* A registered repository that later becomes partial must fail on the next scan
 * rather than reading it anyway. */
static void test_repository_becoming_partial_fails_closed(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas_in(&e, &r, add, 5u, NULL);
    T_EQ_INT(r.exit_code, 0);
    result_free(&r);
    const char *scan[] = {"scan", REPO_NAME};
    run_atlas_in(&e, &r, scan, 2u, NULL);
    T_EQ_INT(r.exit_code, 0);
    result_free(&r);

    make_promisor_config(&e);

    run_atlas_in(&e, &r, scan, 2u, NULL);
    T_CHECK_MSG(r.exit_code == ATLAS_ERR_INTEGRITY,
                "a repository that became partial must fail closed on rescan, got %d",
                r.exit_code);
    result_free(&r);
    env_close(&e);
}

/* --- environment policy, asserted directly ------------------------------ */

static void test_env_policy(void) {
    atlas_err err;
    atlas_err_init(&err);

    atlas_buf slots[ATLAS_GIT_ENV_MAX];
    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_init(&slots[i]);
    }
    const char *env[ATLAS_GIT_ENV_MAX + 1u];
    size_t count = 0;
    T_OK(atlas_git_build_env(slots, ATLAS_GIT_ENV_MAX, env, ATLAS_GIT_ENV_MAX + 1u, &count, &err),
         &err);
    T_CHECK(count > 0);

    /* Every setting the policy promises is present. */
    static const char *const required[] = {
        "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_SYSTEM=/dev/null", "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_COUNT=0",          "GIT_OPTIONAL_LOCKS=0",        "GIT_TERMINAL_PROMPT=0",
        "GIT_PAGER=cat",               "PAGER=cat",                   "GIT_ATTR_NOSYSTEM=1",
        "LC_ALL=C",                    "GIT_ALLOW_PROTOCOL=none",     "GIT_NO_REPLACE_OBJECTS=1",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        bool found = false;
        for (size_t k = 0; k < count; k++) {
            if (strcmp(env[k], required[i]) == 0) {
                found = true;
                break;
            }
        }
        T_CHECK_MSG(found, "the git environment is missing %s", required[i]);
    }

    /* And nothing forbidden is present. */
    const char *offender = NULL;
    T_CHECK_MSG(atlas_git_env_is_sanitized(env, &offender),
                "the constructed environment carries %s", offender != NULL ? offender : "?");

    /* The sanitizer actually rejects each forbidden variable, including the
     * prefix-matched families. */
    static const char *const hostile[] = {
        "GIT_DIR=/x",
        "GIT_WORK_TREE=/x",
        "GIT_COMMON_DIR=/x",
        "GIT_INDEX_FILE=/x",
        "GIT_OBJECT_DIRECTORY=/x",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES=/x",
        "GIT_EXTERNAL_DIFF=/x",
        "GIT_DIFF_OPTS=-x",
        "GIT_ASKPASS=/x",
        "SSH_ASKPASS=/x",
        "GIT_SSH=/x",
        "GIT_SSH_COMMAND=/x",
        "GIT_EXEC_PATH=/x",
        "GIT_CONFIG=/x",
        "GIT_CONFIG_KEY_0=core.fsmonitor",
        "GIT_CONFIG_VALUE_0=/x",
        "GIT_TRACE=1",
        "GIT_TRACE2_EVENT=/x",
        "HOME=/root",
        "XDG_CONFIG_HOME=/x",
    };
    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        const char *probe_env[3];
        probe_env[0] = "LC_ALL=C";
        probe_env[1] = hostile[i];
        probe_env[2] = NULL;
        const char *who = NULL;
        T_CHECK_MSG(!atlas_git_env_is_sanitized(probe_env, &who),
                    "%s should be rejected by the environment policy", hostile[i]);
    }

    for (size_t i = 0; i < ATLAS_GIT_ENV_MAX; i++) {
        atlas_buf_free(&slots[i]);
    }
}

static void test_argv_policy(void) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[ATLAS_GIT_ARGV_MAX];
    size_t n = 0;
    T_OK(atlas_git_build_argv(argv, ATLAS_GIT_ARGV_MAX, &n, "/usr/bin/git", "/repo", &err), &err);
    argv[n] = NULL;

    /* The global prefix. */
    static const char *const required[] = {
        "--no-pager", "--no-optional-locks", "--no-replace-objects",
        "core.fsmonitor=false", "core.hooksPath=/dev/null", "color.ui=false",
        "diff.external=", "core.askPass=", "protocol.allow=never",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        bool found = false;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(argv[k], required[i]) == 0) {
                found = true;
                break;
            }
        }
        T_CHECK_MSG(found, "the git prefix is missing %s", required[i]);
    }
    /* The executable comes first and the repository is addressed with -C, never by
     * changing directory. */
    T_EQ_STR(argv[0], "/usr/bin/git");
    T_EQ_STR(argv[1], "-C");
    T_EQ_STR(argv[2], "/repo");

    /* Per-kind flags are subcommand options and are reported separately, so they
     * can be placed after the subcommand rather than before it. */
    const char *const *diff_flags = atlas_git_cmd_flags(ATLAS_GIT_CMD_DIFF);
    bool ext = false;
    bool textconv = false;
    bool submodules = false;
    for (size_t i = 0; diff_flags[i] != NULL; i++) {
        if (strcmp(diff_flags[i], "--no-ext-diff") == 0) {
            ext = true;
        }
        if (strcmp(diff_flags[i], "--no-textconv") == 0) {
            textconv = true;
        }
        if (strcmp(diff_flags[i], "--ignore-submodules=all") == 0) {
            submodules = true;
        }
    }
    T_CHECK_MSG(ext, "diff commands must pass --no-ext-diff");
    T_CHECK_MSG(textconv, "diff commands must pass --no-textconv");
    T_CHECK_MSG(submodules, "diff commands must pass --ignore-submodules=all");

    const char *const *status_flags = atlas_git_cmd_flags(ATLAS_GIT_CMD_STATUS);
    T_REQUIRE(status_flags[0] != NULL);
    T_EQ_STR(status_flags[0], "--ignore-submodules=all");
    /* A plain command needs none of them. */
    T_CHECK(atlas_git_cmd_flags(ATLAS_GIT_CMD_PLAIN)[0] == NULL);
}

static void test_executable_is_resolved_once(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf a = ATLAS_BUF_INIT;
    atlas_buf b = ATLAS_BUF_INIT;
    T_OK(atlas_git_executable(&a, &err), &err);
    T_OK(atlas_git_executable(&b, &err), &err);
    T_CHECK(a.len > 0);
    T_CHECK(atlas_buf_cstr(&a)[0] == '/');
    T_EQ_STR(atlas_buf_cstr(&b), atlas_buf_cstr(&a));
    atlas_buf_free(&a);
    atlas_buf_free(&b);
}

/* --- no prompting, no network ------------------------------------------- */

static void test_no_prompt_and_no_network(void) {
    harden_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    build_repo(&e);

    /* A remote that would fail loudly if anything tried to reach it, plus an
     * askpass helper that would fire if git ever asked for credentials. */
    set_repo_config(&e, "remote.origin.url", "https://127.0.0.1:1/nonexistent.git");
    set_repo_config(&e, "core.askPass", atlas_buf_cstr(&e.helper));
    set_repo_config(&e, "credential.helper", atlas_buf_cstr(&e.helper));

    expect_atlas_never_runs_helper(&e, NULL, "askpass and credential helper");

    /* stdin is /dev/null for every child, so nothing can block on a prompt: the
     * commands above all completed, which is the observable proof. */
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"environment policy", test_env_policy},
    {"argv policy", test_argv_policy},
    {"git executable is resolved once", test_executable_is_resolved_once},
    {"repo-local core.fsmonitor never runs", test_fsmonitor},
    {"repo-local diff.external never runs", test_diff_external},
    {"gitattributes textconv filter never runs", test_textconv},
    {"repository hooks never run", test_hooks_path},
    {"hostile inherited environment is not forwarded", test_hostile_inherited_env},
    {"hostile GIT_DIR and GIT_WORK_TREE are ignored", test_hostile_repository_selectors},
    {"promisor config fails closed", test_partial_clone_config_is_refused},
    {"promisor pack fails closed", test_promisor_pack_is_refused},
    {"a repository that becomes partial fails closed", test_repository_becoming_partial_fails_closed},
    {"no prompt and no network helper runs", test_no_prompt_and_no_network},
};

ATLAS_TEST_MAIN("git-hardening", TESTS)

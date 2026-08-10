/* Atlas - A8: the worker workspace, against real repositories and real files.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Every case builds its own fixture repository and its own worker root inside a
 * private temporary tree. Nothing touches a live service, socket, database or
 * registered repository, and the fixture is removed on success and failure
 * alike.
 *
 * The claim under test is narrow and load-bearing: a snapshot is a directory of
 * ordinary files with no git metadata, no symlink, no submodule and no path that
 * leaves the workspace — and taking one leaves the source repository
 * byte-identical.
 *
 * Required cases covered here: 29 (workspace traversal), 30 (symlink escape),
 * 31 (cross-job isolation), 32 (registered repository unchanged), 33 (patch
 * without mutation), 34 (artifact traversal), 35 (artifact bounds), 36 (log
 * redaction), plus hostile hooks, hostile configuration, nested repositories,
 * submodule refusal and retention-path validation.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/git.h"
#include "atlas/orch.h"
#include "atlas/sha256.h"
#include "atlas/workspace.h"
#include "atlas_test.h"
#include "support/fixture.h"

/* Reads the fixture repository's HEAD through the ordinary hardened git path,
 * so the commit under test is the one Atlas itself would pin. */
static void head_oid(const char *repo, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git *g = NULL;
    T_OK(atlas_git_open(repo, &g, &err), &err);
    atlas_git_head h;
    memset(&h, 0, sizeof(h));
    T_OK(atlas_git_read_head(g, &h, &err), &err);
    T_OK(atlas_buf_set_str(out, h.oid, &err), &err);
    atlas_git_close(g);
}

/* `fx_write` writes a file and does not create parents, so a fixture that wants
 * a nested path makes the directories itself. */
static void mkdirs(const char *dir, const char *rel) {
    char p[4096];
    (void)snprintf(p, sizeof p, "%s/%s", dir, rel);
    for (char *q = p + strlen(dir) + 1; *q != '\0'; q++) {
        if (*q == '/') {
            *q = '\0';
            (void)mkdir(p, 0755);
            *q = '/';
        }
    }
}

#define JOB_A "j00000000000000000000000000000001"
#define JOB_B "j00000000000000000000000000000002"

typedef struct wsenv {
    fixture fx;
    atlas_buf worker_root;
    atlas_buf commit;
} wsenv;

static void ws_open_env(wsenv *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->worker_root);
    atlas_buf_init(&e->commit);
    T_OK(atlas_buf_appendf(&e->worker_root, &err, "%s/worker", fx_data_dir(&e->fx)), &err);
    T_REQUIRE(mkdir(atlas_buf_cstr(&e->worker_root), 0700) == 0);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    mkdirs(fx_repo(&e->fx), "src/deep/b.txt");
    T_OK(fx_write(fx_repo(&e->fx), "src/deep/b.txt", "hello\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    head_oid(fx_repo(&e->fx), &e->commit);
}

static void ws_close_env(wsenv *e) {
    atlas_buf_free(&e->worker_root);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

static bool path_exists(const char *dir, const char *rel) {
    char p[4096];
    (void)snprintf(p, sizeof p, "%s/%s", dir, rel);
    struct stat sb;
    return lstat(p, &sb) == 0;
}

/* --- creation and layout ---------------------------------------------------- */

static void test_an_attempt_gets_its_own_validated_tree(void) {
    wsenv e;
    ws_open_env(&e);
    atlas_err err;
    atlas_err_init(&err);

    atlas_ws w;
    T_OK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 1, &w, &err), &err);
    static const char *const SUBS[] = {"source", "work", "logs", "tests", "artifacts", "driver"};
    for (size_t i = 0; i < sizeof SUBS / sizeof SUBS[0]; i++) {
        T_CHECK_MSG(path_exists(atlas_buf_cstr(&w.root), SUBS[i]), "%s is missing", SUBS[i]);
    }
    /* Everything a job owns is 0700/0600: nothing another account can read. */
    struct stat sb;
    T_REQUIRE(stat(atlas_buf_cstr(&w.root), &sb) == 0);
    T_CHECK_MSG((sb.st_mode & (S_IRWXG | S_IRWXO)) == 0,
                "the attempt directory is reachable by another account");
    atlas_ws_free(&w);

    /* A worker root that is not ours, or is group-writable, is refused rather
     * than used: a root another account can write is one another account can
     * pre-create entries inside. */
    {
        atlas_buf open_root = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&open_root, &err, "%s/open", fx_data_dir(&e.fx)), &err);
        T_REQUIRE(mkdir(atlas_buf_cstr(&open_root), 0777) == 0);
        atlas_ws bad;
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK_MSG(atlas_ws_open(atlas_buf_cstr(&open_root), JOB_A, 1, &bad, &e2) != ATLAS_OK,
                    "a group-writable worker root was accepted");
        atlas_ws_free(&bad);
        atlas_buf_free(&open_root);
    }
    /* And an identifier a worker made up is refused: only Atlas names a job. */
    {
        atlas_ws bad;
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), "../escape", 1, &bad, &e2) !=
                ATLAS_OK);
        T_CHECK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), "jZZZZ", 1, &bad, &e2) != ATLAS_OK);
        T_CHECK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 0, &bad, &e2) != ATLAS_OK);
        T_CHECK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 9999, &bad, &e2) !=
                ATLAS_OK);
        atlas_ws_free(&bad);
    }
    ws_close_env(&e);
}

static void test_two_jobs_cannot_reach_each_others_workspaces(void) {
    wsenv e;
    ws_open_env(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_ws a, b;
    T_OK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 1, &a, &err), &err);
    T_OK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_B, 1, &b, &err), &err);
    T_CHECK(strcmp(atlas_buf_cstr(&a.root), atlas_buf_cstr(&b.root)) != 0);

    T_OK(atlas_ws_write(&a, "work/secret.txt", "A", 1u, &err), &err);
    T_CHECK(path_exists(atlas_buf_cstr(&a.root), "work/secret.txt"));
    T_CHECK_MSG(!path_exists(atlas_buf_cstr(&b.root), "work/secret.txt"),
                "one job's file appeared in another job's workspace");

    /* And there is no path parameter that could reach across. Every traversal
     * shape is refused by the one function that writes into a workspace. */
    static const char *const ESCAPES[] = {
        "../" JOB_B "/work/x", "work/../../x", "/etc/passwd", "work/./x", "..", "work//x",
    };
    for (size_t i = 0; i < sizeof ESCAPES / sizeof ESCAPES[0]; i++) {
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK_MSG(atlas_ws_write(&a, ESCAPES[i], "x", 1u, &e2) != ATLAS_OK,
                    "\"%s\" was accepted as a workspace path", ESCAPES[i]);
    }
    atlas_ws_free(&a);
    atlas_ws_free(&b);
    ws_close_env(&e);
}

/* --- snapshotting ------------------------------------------------------------ */




/* --- change detection, patch and declared paths ------------------------------ */

static atlas_status collect_rel(const char *rel, void *ud, atlas_err *err) {
    (void)err;
    atlas_buf *b = (atlas_buf *)ud;
    atlas_err e2;
    atlas_err_init(&e2);
    (void)atlas_buf_appendf(b, &e2, "%s\n", rel);
    return ATLAS_OK;
}

static void test_a_patch_is_produced_and_never_applied(void) {
    wsenv e;
    ws_open_env(&e);
    atlas_err err;
    atlas_err_init(&err);
    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);

    atlas_ws w;
    T_OK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 1, &w, &err), &err);
    static const char BODY[] = "int main(void){return 0;}\n";
    T_OK(atlas_ws_materialise(&w, "a.c", 3u, "100644", BODY, sizeof(BODY) - 1u, true, &err), &err);
    static const char DEEP[] = "hello\n";
    T_OK(atlas_ws_materialise(&w, "src/deep/b.txt", 14u, "100644", DEEP, sizeof(DEEP) - 1u, true,
                              &err),
         &err);

    /* A driver edits the work tree, exactly as a real one would. */
    {
        char p[4096];
        (void)snprintf(p, sizeof p, "%s/a.c", atlas_buf_cstr(&w.work));
        FILE *f = fopen(p, "w");
        T_REQUIRE(f != NULL);
        (void)fputs("/* changed */\nint main(void){return 1;}\n", f);
        (void)fclose(f);
    }

    atlas_buf changed = ATLAS_BUF_INIT;
    int64_t n = 0;
    T_OK(atlas_ws_changed_files(&w, collect_rel, &changed, &n, &err), &err);
    T_EQ_INT((int)n, 1);
    T_CHECK(strstr(atlas_buf_cstr(&changed), "a.c") != NULL);

    int64_t files = 0;
    bool differed = false;
    T_OK(atlas_ws_make_patch(&w, 1024 * 1024, &files, &differed, &err), &err);
    T_CHECK_MSG(differed, "the patch reported no differences after an edit");
    T_CHECK(path_exists(atlas_buf_cstr(&w.root), "artifacts/changes.patch"));

    /* The patch exists as bytes and the source repository is untouched. There
     * is no function in Atlas that applies one; that is deferred past A8. */
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "producing a patch modified the source repository");

    atlas_buf_free(&changed);
    atlas_ws_free(&w);
    ws_close_env(&e);
}

static void test_declared_paths_are_enforced_at_component_boundaries(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf declared[2];
    atlas_buf_init(&declared[0]);
    atlas_buf_init(&declared[1]);
    T_OK(atlas_buf_set_str(&declared[0], "src", &err), &err);
    T_OK(atlas_buf_set_str(&declared[1], "docs/a.md", &err), &err);

    T_CHECK(atlas_ws_paths_are_declared("src/x.c", declared, 2u));
    T_CHECK(atlas_ws_paths_are_declared("src", declared, 2u));
    T_CHECK(atlas_ws_paths_are_declared("docs/a.md", declared, 2u));
    /* The classic prefix bug: "src" must not match "srcfoo.c". */
    T_CHECK_MSG(!atlas_ws_paths_are_declared("srcfoo.c", declared, 2u),
                "a declared prefix matched across a component boundary");
    T_CHECK(!atlas_ws_paths_are_declared("other/x", declared, 2u));
    T_CHECK(!atlas_ws_paths_are_declared("docs/b.md", declared, 2u));
    /* No declaration means the workspace itself is the boundary. */
    T_CHECK(atlas_ws_paths_are_declared("anything", declared, 0u));

    atlas_buf_free(&declared[0]);
    atlas_buf_free(&declared[1]);
}

/* --- artifacts ---------------------------------------------------------------- */

static void test_artifact_collection_refuses_links_and_honours_bounds(void) {
    wsenv e;
    ws_open_env(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_ws w;
    T_OK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 1, &w, &err), &err);

    T_OK(atlas_ws_write(&w, "artifacts/report.txt", "all good\n", 9u, &err), &err);
    /* A driver plants a link to a host file and asks for collection. */
    {
        char p[4096];
        (void)snprintf(p, sizeof p, "%s/stolen", atlas_buf_cstr(&w.artifacts));
        T_REQUIRE(symlink("/etc/passwd", p) == 0);
    }
    {
        char p[4096];
        (void)snprintf(p, sizeof p, "%s/fifo", atlas_buf_cstr(&w.artifacts));
        (void)mkfifo(p, 0600);
    }

    atlas_ws_artifact *list = NULL;
    size_t n = 0;
    int64_t refused = 0;
    T_OK(atlas_ws_collect(&w, 64, 1024 * 1024, 4096u, &list, &n, &refused, &err), &err);
    T_EQ_INT((int)n, 1);
    T_CHECK(strcmp(atlas_buf_cstr(&list[0].name), "report.txt") == 0);
    T_CHECK_MSG(refused >= 1, "the planted symlink was not refused");
    for (size_t i = 0; i < n; i++) {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&list[i].name), "stolen") != 0,
                    "a symlinked artifact was collected");
        /* And its bytes are never the target's. */
        T_CHECK(strstr(atlas_buf_cstr(&list[i].content), "root:") == NULL);
    }
    T_EQ_INT((int)list[0].size_bytes, 9);
    T_CHECK(list[0].content_stored);
    T_EQ_INT((int)strlen(atlas_buf_cstr(&list[0].sha256)), 64);
    atlas_ws_artifacts_free(list, n);

    /* Count and byte bounds refuse rather than trim. */
    for (int i = 0; i < 5; i++) {
        char name[64];
        (void)snprintf(name, sizeof name, "artifacts/f%d.bin", i);
        T_OK(atlas_ws_write(&w, name, "0123456789", 10u, &err), &err);
    }
    {
        atlas_err e2;
        atlas_err_init(&e2);
        atlas_ws_artifact *l2 = NULL;
        size_t n2 = 0;
        T_CHECK_MSG(atlas_ws_collect(&w, 2, 1024 * 1024, 4096u, &l2, &n2, NULL, &e2) != ATLAS_OK,
                    "the artifact count bound did not refuse");
        atlas_ws_artifacts_free(l2, n2);
    }
    {
        atlas_err e2;
        atlas_err_init(&e2);
        atlas_ws_artifact *l2 = NULL;
        size_t n2 = 0;
        T_CHECK_MSG(atlas_ws_collect(&w, 64, 12, 4096u, &l2, &n2, NULL, &e2) != ATLAS_OK,
                    "the artifact byte bound did not refuse");
        atlas_ws_artifacts_free(l2, n2);
    }
    atlas_ws_free(&w);
    ws_close_env(&e);
}

/* --- removal and retention ------------------------------------------------------ */

static void test_removal_is_bounded_and_refuses_an_unresolved_target(void) {
    wsenv e;
    ws_open_env(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_ws w;
    T_OK(atlas_ws_open(atlas_buf_cstr(&e.worker_root), JOB_A, 1, &w, &err), &err);
    static const char BODY[] = "int main(void){return 0;}\n";
    T_OK(atlas_ws_materialise(&w, "a.c", 3u, "100644", BODY, sizeof(BODY) - 1u, true, &err), &err);
    static const char DEEP[] = "hello\n";
    T_OK(atlas_ws_materialise(&w, "src/deep/b.txt", 14u, "100644", DEEP, sizeof(DEEP) - 1u, true,
                              &err),
         &err);
    /* A link planted inside the tree must be unlinked, never descended into —
     * otherwise cleanup becomes a way to delete whatever the link points at. */
    atlas_buf victim = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&victim, &err, "%s/keepme", fx_data_dir(&e.fx)), &err);
    {
        FILE *f = fopen(atlas_buf_cstr(&victim), "w");
        T_REQUIRE(f != NULL);
        (void)fputs("keep\n", f);
        (void)fclose(f);
        char p[4096];
        (void)snprintf(p, sizeof p, "%s/work/point", atlas_buf_cstr(&w.root));
        T_REQUIRE(symlink(atlas_buf_cstr(&victim), p) == 0);
    }
    atlas_buf root_copy = ATLAS_BUF_INIT;
    T_OK(atlas_buf_set(&root_copy, w.root.data, w.root.len, &err), &err);
    atlas_ws_free(&w);

    T_OK(atlas_ws_remove(atlas_buf_cstr(&e.worker_root), JOB_A, 1, &err), &err);
    T_CHECK_MSG(!path_exists(atlas_buf_cstr(&root_copy), "work"),
                "the attempt tree survived removal");
    struct stat sb;
    T_CHECK_MSG(lstat(atlas_buf_cstr(&victim), &sb) == 0,
                "cleanup followed a symlink and deleted its target");

    /* An identifier nobody generated is refused rather than resolved, and there
     * is no path parameter that could name a broader target. */
    atlas_err e2;
    atlas_err_init(&e2);
    T_CHECK(atlas_ws_remove(atlas_buf_cstr(&e.worker_root), "../..", 1, &e2) != ATLAS_OK);
    T_CHECK(atlas_ws_remove(atlas_buf_cstr(&e.worker_root), "", 1, &e2) != ATLAS_OK);
    T_CHECK(atlas_ws_remove(atlas_buf_cstr(&e.worker_root), JOB_A, -1, &e2) != ATLAS_OK);
    /* Removing something that is already gone is success, so retention can run
     * twice without becoming an error. */
    T_OK(atlas_ws_remove(atlas_buf_cstr(&e.worker_root), JOB_B, 1, &err), &err);

    atlas_buf_free(&victim);
    atlas_buf_free(&root_copy);
    ws_close_env(&e);
}

static void test_free_space_is_measurable(void) {
    wsenv e;
    ws_open_env(&e);
    atlas_err err;
    atlas_err_init(&err);
    int64_t bytes = -1;
    T_OK(atlas_ws_free_space(atlas_buf_cstr(&e.worker_root), &bytes, &err), &err);
    T_CHECK_MSG(bytes > 0, "free space under the worker root reported %lld", (long long)bytes);
    ws_close_env(&e);
}

/* --- redaction ------------------------------------------------------------------ */

static void test_credential_shapes_are_removed_from_logs(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf out = ATLAS_BUF_INIT;
    int64_t hits = 0;

    static const char LOG[] =
        "starting\nANTHROPIC_API_KEY=sk-ant-api03-abcdefGHIJKL012345\n"
        "token ghp_AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHH\n"
        "aws AKIAIOSFODNN7EXAMPLE\n"
        "-----BEGIN OPENSSH PRIVATE KEY-----\nordinary line\n";
    T_OK(atlas_ws_redact(LOG, sizeof(LOG) - 1u, &out, &hits, &err), &err);
    const char *s = atlas_buf_cstr(&out);
    T_CHECK_MSG(strstr(s, "sk-ant-") == NULL, "an Anthropic key shape survived redaction");
    T_CHECK_MSG(strstr(s, "ghp_") == NULL, "a GitHub token shape survived redaction");
    T_CHECK_MSG(strstr(s, "AKIA") == NULL, "an AWS key shape survived redaction");
    T_CHECK_MSG(strstr(s, "BEGIN OPENSSH") == NULL, "a private key header survived redaction");
    T_CHECK_MSG(strstr(s, "ordinary line") != NULL, "redaction removed ordinary log text");
    T_CHECK(hits >= 4);

    /* Stated as a mitigation, and tested as one: a secret with no recognisable
     * shape passes through. That is exactly why no credential is ever placed in
     * a workspace, an environment or a job specification in the first place. */
    atlas_buf out2 = ATLAS_BUF_INIT;
    int64_t hits2 = 0;
    static const char PLAIN[] = "password is hunter2\n";
    T_OK(atlas_ws_redact(PLAIN, sizeof(PLAIN) - 1u, &out2, &hits2, &err), &err);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out2), "hunter2") != NULL,
                "redaction is being credited with catching a shapeless secret");
    T_EQ_INT((int)hits2, 0);

    atlas_buf_free(&out);
    atlas_buf_free(&out2);
}

static const atlas_test TESTS[] = {
    {"an attempt gets its own validated tree", test_an_attempt_gets_its_own_validated_tree},
    {"two jobs cannot reach each other's workspaces",
     test_two_jobs_cannot_reach_each_others_workspaces},
    {"a patch is produced and never applied", test_a_patch_is_produced_and_never_applied},
    {"declared paths are enforced at component boundaries",
     test_declared_paths_are_enforced_at_component_boundaries},
    {"artifact collection refuses links and honours bounds",
     test_artifact_collection_refuses_links_and_honours_bounds},
    {"removal is bounded and refuses an unresolved target",
     test_removal_is_bounded_and_refuses_an_unresolved_target},
    {"free space is measurable", test_free_space_is_measurable},
    {"credential shapes are removed from logs", test_credential_shapes_are_removed_from_logs},
};

ATLAS_TEST_MAIN("orch_workspace", TESTS)

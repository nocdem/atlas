/* Atlas - search tests covering the FTS5 and degraded paths (required test 23).
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#define REPO_NAME "fixture"

typedef struct search_env {
    fixture fx;
    atlas_ctx *ctx;
} search_env;

typedef struct hits {
    atlas_buf files;
    atlas_buf commits;
    int file_count;
    int commit_count;
} hits;

static void hits_init(hits *h) {
    memset(h, 0, sizeof(*h));
    atlas_buf_init(&h->files);
    atlas_buf_init(&h->commits);
}

static void hits_free(hits *h) {
    atlas_buf_free(&h->files);
    atlas_buf_free(&h->commits);
}

static atlas_status collect_hit(const atlas_search_hit *hit, void *ud, atlas_err *err) {
    hits *h = (hits *)ud;
    if (strcmp(hit->kind, "file") == 0) {
        h->file_count++;
        /* Provenance must accompany every result. */
        if (strcmp(hit->evidence, "SOURCE") != 0) {
            return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "file hit had evidence \"%s\"",
                                 hit->evidence);
        }
        atlas_status st = atlas_buf_append_ch(&h->files, '[', err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&h->files, hit->path_text, err);
        }
        if (st == ATLAS_OK) {
            st = atlas_buf_append_ch(&h->files, ']', err);
        }
        return st;
    }
    h->commit_count++;
    if (strcmp(hit->evidence, "GIT") != 0) {
        return atlas_err_set(err, ATLAS_ERR_INTEGRITY, "commit hit had evidence \"%s\"",
                             hit->evidence);
    }
    return atlas_buf_appendf(&h->commits, err, "[%s]", hit->subject);
}

static void env_open_with_content(search_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);

    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = fx_data_dir(&e->fx);
    T_OK(atlas_ctx_open(&opts, &e->ctx, &err), &err);

    const char *repo = fx_repo(&e->fx);
    T_OK(fx_init_repo(&e->fx, repo, NULL, &err), &err);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/buffer.c", "buffer\n", &err), &err);
    T_OK(fx_write(repo, "src/scanner.c", "scanner\n", &err), &err);
    T_OK(fx_write(repo, "README.md", "readme\n", &err), &err);
    T_OK(fx_add_all(&e->fx, repo, &err), &err);
    T_OK(fx_commit_body(&e->fx, repo, "add the buffer implementation",
                        "This body mentions provenance explicitly.", &err),
         &err);
    T_OK(fx_write(repo, "src/scanner.c", "scanner v2\n", &err), &err);
    T_OK(fx_add_all(&e->fx, repo, &err), &err);
    T_OK(fx_commit(&e->fx, repo, "tune the scanner", &err), &err);

    T_OK(atlas_service_repo_add(e->ctx, repo, REPO_NAME, NULL, &err), &err);
    atlas_scan_opts sopts;
    atlas_scan_opts_init(&sopts);
    atlas_scan_summary sum;
    T_OK(atlas_service_scan(e->ctx, REPO_NAME, &sopts, &sum, &err), &err);
}

static void env_close(search_env *e) {
    atlas_ctx_close(e->ctx);
    e->ctx = NULL;
    fx_close(&e->fx);
}

static void run_search(search_env *e, const char *query, hits *h, atlas_search_mode *mode,
                       int64_t *count) {
    atlas_err err;
    atlas_err_init(&err);
    hits_init(h);
    T_OK(atlas_service_search(e->ctx, REPO_NAME, query, 50, mode, collect_hit, h, count, &err),
         &err);
}

/* --- FTS5 path ----------------------------------------------------------- */

static void test_search_fts5(void) {
    search_env e;
    env_open_with_content(&e);
    const atlas_db_caps *caps = atlas_db_caps_of(atlas_ctx_db(e.ctx));
    if (!caps->fts5) {
        atlas_test_note("this SQLite has no FTS5; the ranked path cannot be tested here");
        env_close(&e);
        return;
    }

    hits h;
    atlas_search_mode mode = ATLAS_SEARCH_DEGRADED_LIKE;
    int64_t count = 0;

    /* A path component matches a file. */
    run_search(&e, "buffer", &h, &mode, &count);
    T_EQ_INT(mode, ATLAS_SEARCH_FTS5);
    T_CHECK_MSG(h.file_count >= 1, "expected a file hit for \"buffer\"");
    T_CHECK(strstr(atlas_buf_cstr(&h.files), "src/buffer.c") != NULL);
    /* And the commit that mentions it. */
    T_CHECK_MSG(h.commit_count >= 1, "expected a commit hit for \"buffer\"");
    T_CHECK(strstr(atlas_buf_cstr(&h.commits), "add the buffer implementation") != NULL);
    T_EQ_INT(count, h.file_count + h.commit_count);
    hits_free(&h);

    /* A word only in a commit body still matches. */
    run_search(&e, "provenance", &h, &mode, &count);
    T_CHECK_MSG(h.commit_count >= 1, "a word in the commit body should match");
    hits_free(&h);

    /* Prefix matching, which is what makes path search usable. */
    run_search(&e, "scann", &h, &mode, &count);
    T_CHECK_MSG(count >= 1, "prefix search should match \"scanner\"");
    hits_free(&h);

    /* A dotted path tokenises sensibly. */
    run_search(&e, "buffer.c", &h, &mode, &count);
    T_CHECK(h.file_count >= 1);
    hits_free(&h);

    /* Nothing matches: zero results, not an error. */
    run_search(&e, "zzzznotpresent", &h, &mode, &count);
    T_EQ_INT(count, 0);
    T_EQ_INT(h.file_count, 0);
    T_EQ_INT(h.commit_count, 0);
    hits_free(&h);

    env_close(&e);
}

/* An arbitrary query string must never be interpreted as FTS5 query syntax. */
static void test_search_query_is_never_syntax(void) {
    search_env e;
    env_open_with_content(&e);
    atlas_err err;
    atlas_err_init(&err);

    static const char *const hostile[] = {
        "\"unbalanced",       "buffer OR",      "NEAR(",        "*",
        "^",                  "a AND (b",       "\"quoted\"",   "col:value",
        "buffer NOT scanner", "-minus",         "{brace}",      "back\\slash",
    };
    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        hits h;
        atlas_search_mode mode = ATLAS_SEARCH_FTS5;
        int64_t count = 0;
        hits_init(&h);
        /* The requirement is that it does not error, not that it matches. */
        atlas_status st = atlas_service_search(e.ctx, REPO_NAME, hostile[i], 50, &mode, collect_hit,
                                               &h, &count, &err);
        T_CHECK_MSG(st == ATLAS_OK, "query \"%s\" failed: %s", hostile[i], atlas_err_msg(&err));
        hits_free(&h);
        atlas_err_init(&err);
    }

    /* An empty query is a usage error, reported clearly. */
    hits h;
    hits_init(&h);
    int64_t count = 0;
    T_FAILS_WITH(atlas_service_search(e.ctx, REPO_NAME, "", 50, NULL, collect_hit, &h, &count, &err),
                 ATLAS_ERR_USAGE, &err);
    T_FAILS_WITH(
        atlas_service_search(e.ctx, REPO_NAME, "   ", 50, NULL, collect_hit, &h, &count, &err),
        ATLAS_ERR_USAGE, &err);
    hits_free(&h);

    env_close(&e);
}

/* --- degraded path ------------------------------------------------------- */

static void test_search_degraded_fallback(void) {
    search_env e;
    env_open_with_content(&e);

    /* Simulate a SQLite build without FTS5 so the fallback is exercised even on a
     * build that has it. */
    atlas_db_disable_fts_for_tests(atlas_ctx_db(e.ctx));

    hits h;
    atlas_search_mode mode = ATLAS_SEARCH_FTS5;
    int64_t count = 0;
    run_search(&e, "buffer", &h, &mode, &count);

    /* The caller is told the results are degraded, never silently given fewer. */
    T_EQ_INT(mode, ATLAS_SEARCH_DEGRADED_LIKE);
    T_EQ_STR(atlas_search_mode_name(mode), "degraded-like");
    T_CHECK_MSG(h.file_count >= 1, "degraded search should still find src/buffer.c");
    T_CHECK(strstr(atlas_buf_cstr(&h.files), "src/buffer.c") != NULL);
    T_CHECK_MSG(h.commit_count >= 1, "degraded search should still find the commit");
    hits_free(&h);

    /* Substring matching, which is what LIKE gives. */
    run_search(&e, "uffer", &h, &mode, &count);
    T_CHECK_MSG(h.file_count >= 1, "degraded search is substring-based");
    hits_free(&h);

    /* LIKE metacharacters are escaped, so they match literally and find nothing
     * rather than matching everything. */
    run_search(&e, "%", &h, &mode, &count);
    T_CHECK_MSG(count == 0, "an unescaped LIKE wildcard matched %lld rows", (long long)count);
    hits_free(&h);

    run_search(&e, "_", &h, &mode, &count);
    T_CHECK_MSG(count == 0, "an unescaped LIKE single-char wildcard matched %lld rows",
                (long long)count);
    hits_free(&h);

    env_close(&e);
}

static void test_search_limit_is_respected(void) {
    search_env e;
    atlas_err err;
    atlas_err_init(&err);
    memset(&e, 0, sizeof(e));
    T_OK(fx_open(&e.fx, &err), &err);
    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = fx_data_dir(&e.fx);
    T_OK(atlas_ctx_open(&opts, &e.ctx, &err), &err);

    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    for (int i = 0; i < 12; i++) {
        char name[64];
        (void)snprintf(name, sizeof(name), "widget%02d.txt", i);
        T_OK(fx_write(repo, name, "x\n", &err), &err);
    }
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "many widgets", &err), &err);
    T_OK(atlas_service_repo_add(e.ctx, repo, REPO_NAME, NULL, &err), &err);
    atlas_scan_opts sopts;
    atlas_scan_opts_init(&sopts);
    atlas_scan_summary sum;
    T_OK(atlas_service_scan(e.ctx, REPO_NAME, &sopts, &sum, &err), &err);

    hits h;
    hits_init(&h);
    int64_t count = 0;
    T_OK(atlas_service_search(e.ctx, REPO_NAME, "widget", 5, NULL, collect_hit, &h, &count, &err),
         &err);
    T_CHECK_MSG(h.file_count <= 5, "the limit was exceeded: %d file hits", h.file_count);
    hits_free(&h);

    env_close(&e);
}

static void test_search_requires_known_repo(void) {
    search_env e;
    env_open_with_content(&e);
    atlas_err err;
    atlas_err_init(&err);
    hits h;
    hits_init(&h);
    int64_t count = 0;
    T_FAILS_WITH(atlas_service_search(e.ctx, "no-such-repo", "buffer", 50, NULL, collect_hit, &h,
                                      &count, &err),
                 ATLAS_ERR_REPO, &err);
    hits_free(&h);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"ranked search with FTS5", test_search_fts5},
    {"a query is never treated as FTS5 syntax", test_search_query_is_never_syntax},
    {"degraded fallback is reported, not silent", test_search_degraded_fallback},
    {"the result limit is respected", test_search_limit_is_respected},
    {"search requires a registered repository", test_search_requires_known_repo},
};

ATLAS_TEST_MAIN("search", TESTS)

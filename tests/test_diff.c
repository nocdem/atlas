/* Atlas - working-tree diff semantics.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Covers staged-only, unstaged-only, staged-plus-further-unstaged, untracked,
 * staged rename, binary content, unborn HEAD, the truncation ceiling, and the
 * requirement that the target repository is byte-identical afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "support/fixture.h"

#define REPO_NAME "fixture"

typedef struct diff_env {
    fixture fx;
    atlas_ctx *ctx;
} diff_env;

/* One collected entry, copied because the callback receives borrowed pointers. */
typedef struct seen_entry {
    atlas_change_scope scope;
    char status;
    char change_type[24];
    char path[512];
    char old_path[512];
    bool has_old_path;
    bool binary;
    bool counts_known;
    int64_t added;
    int64_t deleted;
    bool size_known;
    int64_t size_bytes;
    char content_hash[ATLAS_SHA256_HEX_LEN + 1u];
    bool has_hash;
    bool is_directory;
    int score;
    bool score_known;
} seen_entry;

typedef struct seen {
    seen_entry items[64];
    size_t count;
} seen;

static atlas_status collect(const atlas_diff_entry *e, void *ud, atlas_err *err) {
    seen *s = (seen *)ud;
    if (s->count >= sizeof(s->items) / sizeof(s->items[0])) {
        return atlas_err_set(err, ATLAS_ERR_INTERNAL, "test collector is full");
    }
    seen_entry *o = &s->items[s->count++];
    memset(o, 0, sizeof(*o));
    o->scope = e->scope;
    o->status = e->status;
    (void)snprintf(o->change_type, sizeof(o->change_type), "%s", e->change_type);
    (void)snprintf(o->path, sizeof(o->path), "%s", e->path_text);
    if (e->old_path_text != NULL) {
        (void)snprintf(o->old_path, sizeof(o->old_path), "%s", e->old_path_text);
        o->has_old_path = true;
    }
    o->binary = e->binary;
    o->counts_known = e->counts_known;
    o->added = e->added;
    o->deleted = e->deleted;
    o->size_known = e->size_known;
    o->size_bytes = e->size_bytes;
    if (e->content_hash != NULL) {
        (void)snprintf(o->content_hash, sizeof(o->content_hash), "%s", e->content_hash);
        o->has_hash = true;
    }
    o->is_directory = e->is_directory;
    o->score = e->score;
    o->score_known = e->score_known;
    return ATLAS_OK;
}

static const seen_entry *find_entry(const seen *s, atlas_change_scope scope, const char *path) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i].scope == scope && strcmp(s->items[i].path, path) == 0) {
            return &s->items[i];
        }
    }
    return NULL;
}

static void env_open(diff_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = fx_data_dir(&e->fx);
    T_OK(atlas_ctx_open(&opts, &e->ctx, &err), &err);
}

static void env_close(diff_env *e) {
    atlas_ctx_close(e->ctx);
    e->ctx = NULL;
    fx_close(&e->fx);
}

static void env_register(diff_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_service_repo_add(e->ctx, fx_repo(&e->fx), REPO_NAME, NULL, &err), &err);
}

static void run_diff(diff_env *e, const atlas_diff_opts *opts, seen *s, atlas_diff_report *rep) {
    atlas_err err;
    atlas_err_init(&err);
    memset(s, 0, sizeof(*s));
    atlas_diff_report_init(rep);
    T_OK(atlas_service_diff(e->ctx, REPO_NAME, opts, collect, s, rep, &err), &err);
}

/* Every diff must leave the repository untouched; asserted in each test. */
static void expect_unchanged(diff_env *e, const char *before) {
    atlas_err err;
    atlas_err_init(&err);
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e->fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "atlas diff modified the target repository");
}

static void digest_now(diff_env *e, char *out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_tree_digest(fx_repo(&e->fx), out, &err), &err);
}

/* --- staged only --------------------------------------------------------- */

static void test_staged_only(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "base.txt", "base\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    /* Staged and nothing else: the file on disk matches the index. */
    T_OK(fx_write(repo, "added.txt", "one\ntwo\n", &err), &err);
    const char *add[] = {"add", "--", "added.txt"};
    T_OK(fx_git_ok(&e.fx, repo, add, 3u, &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    T_EQ_INT(rep.staged_count, 1);
    T_EQ_INT(rep.unstaged_count, 0);
    T_EQ_INT(rep.untracked_count, 0);
    T_CHECK(rep.dirty);
    T_EQ_STR(rep.head_state, "born");
    T_CHECK_MSG(rep.base_head[0] != '\0', "a born HEAD must report a base");

    const seen_entry *st = find_entry(&s, ATLAS_SCOPE_STAGED, "added.txt");
    T_REQUIRE_MSG(st != NULL, "the staged addition was not reported");
    T_EQ_STR(st->change_type, "add");
    T_EQ_INT(st->status, 'A');
    /* Line counts come from the staged comparison against HEAD. */
    T_CHECK_MSG(st->counts_known, "staged line counts should be known");
    T_EQ_INT(st->added, 2);
    T_EQ_INT(st->deleted, 0);
    T_CHECK(!st->binary);
    T_CHECK(find_entry(&s, ATLAS_SCOPE_UNSTAGED, "added.txt") == NULL);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

/* --- unstaged only ------------------------------------------------------- */

static void test_unstaged_only(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "tracked.txt", "one\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    T_OK(fx_write(repo, "tracked.txt", "one\ntwo\nthree\n", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    T_EQ_INT(rep.staged_count, 0);
    T_EQ_INT(rep.unstaged_count, 1);
    const seen_entry *un = find_entry(&s, ATLAS_SCOPE_UNSTAGED, "tracked.txt");
    T_REQUIRE(un != NULL);
    T_EQ_STR(un->change_type, "modify");
    T_EQ_INT(un->status, 'M');
    T_CHECK(un->counts_known);
    T_EQ_INT(un->added, 2);
    T_EQ_INT(un->deleted, 0);
    T_CHECK(find_entry(&s, ATLAS_SCOPE_STAGED, "tracked.txt") == NULL);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

/* --- staged, then modified again ----------------------------------------- */

static void test_staged_then_modified_again(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "both.txt", "v1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    /* Stage one change, then change the file again on disk. This is the case a
     * single-scope diff cannot express: both facts are true at once. */
    T_OK(fx_write(repo, "both.txt", "v2\n", &err), &err);
    const char *add[] = {"add", "--", "both.txt"};
    T_OK(fx_git_ok(&e.fx, repo, add, 3u, &err), &err);
    T_OK(fx_write(repo, "both.txt", "v3\n", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    T_EQ_INT(rep.staged_count, 1);
    T_EQ_INT(rep.unstaged_count, 1);
    const seen_entry *staged = find_entry(&s, ATLAS_SCOPE_STAGED, "both.txt");
    const seen_entry *unstaged = find_entry(&s, ATLAS_SCOPE_UNSTAGED, "both.txt");
    T_REQUIRE_MSG(staged != NULL, "the staged modification was not reported");
    T_REQUIRE_MSG(unstaged != NULL, "the further unstaged modification was not reported");
    T_EQ_STR(staged->change_type, "modify");
    T_EQ_STR(unstaged->change_type, "modify");
    /* Both comparisons carry their own counts. */
    T_CHECK(staged->counts_known);
    T_CHECK(unstaged->counts_known);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

/* --- untracked ----------------------------------------------------------- */

static void test_untracked(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "tracked.txt", "t\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    const char *contents = "untracked content\n";
    T_OK(fx_write(repo, "new.txt", contents, &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    T_EQ_INT(rep.untracked_count, 1);
    const seen_entry *un = find_entry(&s, ATLAS_SCOPE_UNTRACKED, "new.txt");
    T_REQUIRE(un != NULL);
    T_EQ_STR(un->change_type, "untracked");
    /* Identity is recorded: size and content hash, never the contents. */
    T_CHECK(un->size_known);
    T_EQ_INT(un->size_bytes, (int64_t)strlen(contents));
    T_REQUIRE_MSG(un->has_hash, "an untracked file should be hashed");
    char expected[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(contents, strlen(contents), expected);
    T_EQ_STR(un->content_hash, expected);
    T_CHECK(!un->is_directory);
    /* Line counts do not apply to a file git has never seen. */
    T_CHECK(!un->counts_known);

    /* Skipping untracked paths is honoured. */
    atlas_diff_opts opts;
    atlas_diff_opts_init(&opts);
    opts.skip_untracked = true;
    seen s2;
    atlas_diff_report rep2;
    run_diff(&e, &opts, &s2, &rep2);
    T_EQ_INT(rep2.untracked_count, 0);
    T_CHECK(find_entry(&s2, ATLAS_SCOPE_UNTRACKED, "new.txt") == NULL);
    atlas_diff_report_free(&rep2);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

static void test_untracked_directory_is_collapsed(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "tracked.txt", "t\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    /* A wholly untracked directory: git collapses it, which keeps the report
     * bounded when someone drops a huge build tree in. */
    T_OK(fx_mkdir(repo, "buildout", &err), &err);
    for (int i = 0; i < 5; i++) {
        char name[64];
        (void)snprintf(name, sizeof(name), "buildout/obj%d.o", i);
        T_OK(fx_write(repo, name, "x\n", &err), &err);
    }

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    const seen_entry *dir = find_entry(&s, ATLAS_SCOPE_UNTRACKED, "buildout/");
    T_REQUIRE_MSG(dir != NULL, "the untracked directory was not reported as one entry");
    T_CHECK_MSG(dir->is_directory, "the entry should be marked as a directory");
    T_CHECK_MSG(!dir->has_hash, "a directory must not be hashed");
    T_EQ_INT(rep.untracked_count, 1);

    atlas_diff_report_free(&rep);
    env_close(&e);
}

/* --- staged rename ------------------------------------------------------- */

static void test_staged_rename(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "before.txt", "a reasonably long body so rename detection fires\n", &err),
         &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    const char *mv[] = {"mv", "before.txt", "after.txt"};
    T_OK(fx_git_ok(&e.fx, repo, mv, 3u, &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    const seen_entry *ren = find_entry(&s, ATLAS_SCOPE_STAGED, "after.txt");
    T_REQUIRE_MSG(ren != NULL, "the staged rename was not reported");
    T_EQ_STR(ren->change_type, "rename");
    T_EQ_INT(ren->status, 'R');
    T_CHECK_MSG(ren->has_old_path, "a rename must report where it came from");
    T_EQ_STR(ren->old_path, "before.txt");
    T_CHECK_MSG(ren->score_known, "a rename should carry a similarity score");
    T_EQ_INT(ren->score, 100);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

static void test_staged_rename_then_modified(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "before.txt", "a reasonably long body so rename detection fires\n", &err),
         &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    const char *mv[] = {"mv", "before.txt", "after.txt"};
    T_OK(fx_git_ok(&e.fx, repo, mv, 3u, &err), &err);
    T_OK(fx_write(repo, "after.txt", "a reasonably long body so rename detection fires\nplus\n",
                  &err),
         &err);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    /* The rename is staged and the extra edit is unstaged: two entries for one
     * path, each describing a different comparison. */
    const seen_entry *staged = find_entry(&s, ATLAS_SCOPE_STAGED, "after.txt");
    const seen_entry *unstaged = find_entry(&s, ATLAS_SCOPE_UNSTAGED, "after.txt");
    T_REQUIRE(staged != NULL);
    T_REQUIRE(unstaged != NULL);
    T_EQ_STR(staged->change_type, "rename");
    T_EQ_STR(staged->old_path, "before.txt");
    T_EQ_STR(unstaged->change_type, "modify");
    /* The unstaged side is not a rename, so it carries no origin path. */
    T_CHECK(!unstaged->has_old_path);

    atlas_diff_report_free(&rep);
    env_close(&e);
}

/* --- binary ------------------------------------------------------------- */

static void test_binary_changes(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    static const char blob1[] = {'\x00', '\x01', '\x02', 'B', 'I', 'N', '\x00'};
    T_OK(fx_write_bytes(repo, "image.bin", 9u, blob1, sizeof(blob1), 0644, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    static const char blob2[] = {'\x00', '\x09', '\x08', 'B', 'I', 'N', '\x00', '\x01'};
    T_OK(fx_write_bytes(repo, "image.bin", 9u, blob2, sizeof(blob2), 0644, &err), &err);
    /* An untracked binary file too, so the binary heuristic is exercised on the
     * path where git tells us nothing. */
    T_OK(fx_write_bytes(repo, "extra.bin", 9u, blob1, sizeof(blob1), 0644, &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    const seen_entry *mod = find_entry(&s, ATLAS_SCOPE_UNSTAGED, "image.bin");
    T_REQUIRE(mod != NULL);
    T_CHECK_MSG(mod->binary, "a binary change must be marked binary");
    /* Line counts are meaningless for binary content and are reported unknown
     * rather than as zero. */
    T_CHECK_MSG(!mod->counts_known, "binary entries must not claim line counts");

    const seen_entry *untracked = find_entry(&s, ATLAS_SCOPE_UNTRACKED, "extra.bin");
    T_REQUIRE(untracked != NULL);
    T_CHECK_MSG(untracked->binary, "an untracked binary file should be detected as binary");
    T_CHECK(untracked->has_hash);

    T_CHECK_MSG(rep.binary_changes >= 2, "expected at least 2 binary changes, got %lld",
                (long long)rep.binary_changes);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

/* --- HEAD states --------------------------------------------------------- */

static void test_unborn_head(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    env_register(&e);

    /* Nothing is committed yet: one staged addition and one untracked file. */
    T_OK(fx_write(repo, "staged.txt", "s\n", &err), &err);
    const char *add[] = {"add", "--", "staged.txt"};
    T_OK(fx_git_ok(&e.fx, repo, add, 3u, &err), &err);
    T_OK(fx_write(repo, "loose.txt", "l\n", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    /* An unborn HEAD is a normal state, reported as such rather than failing. */
    T_EQ_STR(rep.head_state, "unborn");
    T_CHECK_MSG(rep.base_head[0] == '\0', "an unborn HEAD has no base commit");
    T_EQ_INT(rep.staged_count, 1);
    T_EQ_INT(rep.untracked_count, 1);

    const seen_entry *staged = find_entry(&s, ATLAS_SCOPE_STAGED, "staged.txt");
    T_REQUIRE_MSG(staged != NULL, "a staged path must be reported even with no HEAD");
    T_EQ_STR(staged->change_type, "add");
    /* There is no base to diff against, so counts are honestly unknown. */
    T_CHECK_MSG(!staged->counts_known,
                "with no HEAD there is no base, so line counts must be unknown");

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

static void test_detached_head(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    const char *detach[] = {"checkout", "-q", "--detach", "HEAD"};
    T_OK(fx_git_ok(&e.fx, repo, detach, 4u, &err), &err);
    env_register(&e);

    T_OK(fx_write(repo, "a.txt", "a\nb\n", &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    T_EQ_STR(rep.head_state, "detached");
    T_CHECK_MSG(rep.base_head[0] != '\0', "a detached HEAD still has a base commit");
    T_CHECK_MSG(rep.branch[0] == '\0', "a detached HEAD has no branch");
    T_EQ_INT(rep.unstaged_count, 1);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

static void test_clean_worktree(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    T_CHECK(!rep.dirty);
    T_EQ_INT(rep.total_entries, 0);
    T_EQ_INT(rep.staged_count, 0);
    T_EQ_INT(rep.unstaged_count, 0);
    T_EQ_INT(rep.untracked_count, 0);
    T_CHECK(!rep.truncated);
    T_EQ_INT(s.count, 0);

    atlas_diff_report_free(&rep);
    env_close(&e);
}

/* --- the truncation ceiling ---------------------------------------------- */

static void test_output_ceiling(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "base.txt", "b\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    /* More changed paths than the ceiling allows. */
    for (int i = 0; i < 20; i++) {
        char name[64];
        (void)snprintf(name, sizeof(name), "file%02d.txt", i);
        T_OK(fx_write(repo, name, "x\n", &err), &err);
    }
    T_OK(fx_add_all(&e.fx, repo, &err), &err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    digest_now(&e, before);

    atlas_diff_opts opts;
    atlas_diff_opts_init(&opts);
    opts.max_entries = 5;

    seen s;
    atlas_diff_report rep;
    run_diff(&e, &opts, &s, &rep);

    /* Bounded, and honest about being bounded. */
    T_CHECK_MSG(rep.truncated, "exceeding the ceiling must set truncated");
    T_EQ_INT(rep.total_entries, 5);
    T_EQ_INT(s.count, 5);
    T_CHECK_MSG(rep.truncated_reason.len > 0, "truncation must come with a reason");
    /* The counts still reflect reality, so the summary is not silently wrong. */
    T_CHECK_MSG(rep.staged_count == 20, "expected 20 staged paths counted, got %lld",
                (long long)rep.staged_count);

    atlas_diff_report_free(&rep);
    expect_unchanged(&e, before);
    env_close(&e);
}

/* --- hostile paths ------------------------------------------------------- */

static void test_hostile_paths_are_encoded(void) {
    diff_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "base.txt", "b\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    /* An untracked file whose name carries a terminal escape. */
    static const char nasty[] = {'\x1b', '[', '3', '1', 'm', 'r', 'e', 'd', '.', 't', 'x', 't'};
    if (!fx_can_create_name(repo, nasty, sizeof(nasty))) {
        atlas_test_note("this filesystem refuses escape bytes in filenames; skipping");
        env_close(&e);
        return;
    }
    T_OK(fx_write_bytes(repo, nasty, sizeof(nasty), "x\n", 2u, 0644, &err), &err);
    T_OK(fx_write(repo, "with\ttab.txt", "t\n", &err), &err);

    seen s;
    atlas_diff_report rep;
    run_diff(&e, NULL, &s, &rep);

    /* Paths reach the caller already encoded, so no consumer can be surprised. */
    T_CHECK_MSG(find_entry(&s, ATLAS_SCOPE_UNTRACKED, "%1B[31mred.txt") != NULL,
                "the escape byte in the filename was not encoded");
    T_CHECK_MSG(find_entry(&s, ATLAS_SCOPE_UNTRACKED, "with%09tab.txt") != NULL,
                "the tab in the filename was not encoded");
    for (size_t i = 0; i < s.count; i++) {
        for (const char *p = s.items[i].path; *p != '\0'; p++) {
            unsigned char c = (unsigned char)*p;
            T_CHECK_MSG(c >= 0x20u && c != 0x7fu, "a control byte reached the caller: 0x%02x", c);
        }
    }

    atlas_diff_report_free(&rep);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"staged only", test_staged_only},
    {"unstaged only", test_unstaged_only},
    {"staged plus a further unstaged modification", test_staged_then_modified_again},
    {"untracked file identity", test_untracked},
    {"untracked directory is collapsed", test_untracked_directory_is_collapsed},
    {"staged rename", test_staged_rename},
    {"staged rename then modified", test_staged_rename_then_modified},
    {"binary changes", test_binary_changes},
    {"unborn HEAD", test_unborn_head},
    {"detached HEAD", test_detached_head},
    {"clean worktree", test_clean_worktree},
    {"output ceiling and truncation", test_output_ceiling},
    {"hostile paths are encoded", test_hostile_paths_are_encoded},
};

ATLAS_TEST_MAIN("diff", TESTS)

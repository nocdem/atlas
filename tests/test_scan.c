/* Atlas - scanner integration tests against real git repositories.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Covers required tests 3 through 22: clean and dirty scans, repeated scans,
 * added/modified/deleted/renamed files, executable-bit changes, tracked symlinks,
 * symlink escape attempts, hostile filenames, detached HEAD, empty repositories,
 * both object formats, and compile_commands.json detection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#define REPO_NAME "fixture"

typedef struct scan_env {
    fixture fx;
    atlas_ctx *ctx;
} scan_env;

static void env_open(scan_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);

    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    /* Always an explicit temporary directory: the suite must never open the real
     * user database. */
    opts.data_dir_override = fx_data_dir(&e->fx);
    T_OK(atlas_ctx_open(&opts, &e->ctx, &err), &err);
}

static void env_close(scan_env *e) {
    atlas_ctx_close(e->ctx);
    e->ctx = NULL;
    fx_close(&e->fx);
}

static void env_register(scan_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_service_repo_add(e->ctx, fx_repo(&e->fx), REPO_NAME, NULL, &err), &err);
}

static void env_scan(scan_env *e, atlas_scan_summary *sum) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_scan_opts opts;
    atlas_scan_opts_init(&opts);
    T_OK(atlas_service_scan(e->ctx, REPO_NAME, &opts, sum, &err), &err);
}

/* --- reading back a file row -------------------------------------------- */

typedef struct file_snapshot {
    bool found;
    atlas_buf path_text;
    atlas_buf file_type;
    atlas_buf content_hash;
    atlas_buf git_mode;
    atlas_buf git_index_oid;
    atlas_buf read_error;
    atlas_buf language;
    bool path_is_utf8;
    bool is_executable;
    bool is_symlink;
    bool unsafe_path;
    bool deleted;
    bool size_known;
    int64_t size_bytes;
} file_snapshot;

static void snapshot_init(file_snapshot *s) {
    memset(s, 0, sizeof(*s));
    atlas_buf_init(&s->path_text);
    atlas_buf_init(&s->file_type);
    atlas_buf_init(&s->content_hash);
    atlas_buf_init(&s->git_mode);
    atlas_buf_init(&s->git_index_oid);
    atlas_buf_init(&s->read_error);
    atlas_buf_init(&s->language);
}

static void snapshot_free(file_snapshot *s) {
    atlas_buf_free(&s->path_text);
    atlas_buf_free(&s->file_type);
    atlas_buf_free(&s->content_hash);
    atlas_buf_free(&s->git_mode);
    atlas_buf_free(&s->git_index_oid);
    atlas_buf_free(&s->read_error);
    atlas_buf_free(&s->language);
}

static atlas_status copy_row(const atlas_file_row *row, void *ud, atlas_err *err) {
    file_snapshot *s = (file_snapshot *)ud;
    s->found = true;
    atlas_status st = atlas_buf_set_str(&s->path_text, row->path_text, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s->file_type, row->file_type, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s->content_hash,
                               row->content_hash != NULL ? row->content_hash : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s->git_mode, row->git_mode != NULL ? row->git_mode : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s->git_index_oid,
                               row->git_index_oid != NULL ? row->git_index_oid : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s->read_error, row->read_error != NULL ? row->read_error : "", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_set_str(&s->language, row->language != NULL ? row->language : "", err);
    }
    if (st != ATLAS_OK) {
        return st;
    }
    s->path_is_utf8 = row->path_is_utf8;
    s->is_executable = row->is_executable;
    s->is_symlink = row->is_symlink;
    s->unsafe_path = row->unsafe_path;
    s->deleted = row->deleted;
    s->size_known = row->size_known;
    s->size_bytes = row->size_bytes;
    return ATLAS_OK;
}

static void snap_bytes(scan_env *e, const void *path, size_t len, file_snapshot *s) {
    atlas_err err;
    atlas_err_init(&err);
    snapshot_init(s);
    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(atlas_ctx_db(e->ctx), REPO_NAME, &info, &found, &err), &err);
    T_REQUIRE(found);
    bool row_found = false;
    T_OK(atlas_db_file_get(atlas_ctx_db(e->ctx), info.id, path, len, copy_row, s, &row_found, &err),
         &err);
    s->found = row_found;
    atlas_repo_info_free(&info);
}

static void snap(scan_env *e, const char *path, file_snapshot *s) {
    snap_bytes(e, path, strlen(path), s);
}

static void expect_hash_of(const file_snapshot *s, const char *content, const char *what) {
    char expected[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex(content, strlen(content), expected);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&s->content_hash), expected) == 0,
                "%s: expected sha256 %s, got %s", what, expected,
                atlas_buf_cstr(&s->content_hash));
}

/* Reaches into the db layer only to assert row counts in tests. */
static int64_t count_sql(scan_env *e, const char *sql) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t v = -1;
    T_OK(atlas_db_query_int64(atlas_ctx_db(e->ctx), sql, &v, &err), &err);
    return v;
}

/* --- required test 3: clean repository scan ------------------------------ */

static void test_clean_scan(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_write(repo, "README.md", "# Readme\n", &err), &err);
    T_OK(fx_mkdir(repo, "src", &err), &err);
    T_OK(fx_write(repo, "src/util.c", "void util(void){}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "initial commit", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);

    T_EQ_INT(sum.files_total, 3);
    T_EQ_INT(sum.files_added, 3);
    T_EQ_INT(sum.files_modified, 0);
    T_EQ_INT(sum.files_deleted, 0);
    T_EQ_INT(sum.files_unchanged, 0);
    T_EQ_INT(sum.files_unreadable, 0);
    T_EQ_INT(sum.files_unsafe, 0);
    T_CHECK_MSG(!sum.dirty, "a freshly committed tree must be clean");
    T_EQ_STR(sum.head_state, "born");
    T_EQ_STR(sum.branch, "main");
    T_CHECK(sum.head_oid[0] != '\0');
    T_EQ_INT(sum.commits_ingested, 1);
    T_EQ_INT(sum.changes_ingested, 3);
    T_CHECK(sum.evidence_created > 0);

    file_snapshot s;
    snap(&e, "main.c", &s);
    T_CHECK(s.found);
    T_EQ_STR(atlas_buf_cstr(&s.file_type), "regular");
    T_EQ_STR(atlas_buf_cstr(&s.git_mode), "100644");
    T_EQ_STR(atlas_buf_cstr(&s.language), "c");
    T_CHECK(s.path_is_utf8);
    T_CHECK(!s.is_executable);
    T_CHECK(!s.is_symlink);
    T_CHECK(!s.deleted);
    T_CHECK(s.size_known);
    T_EQ_INT(s.size_bytes, 26);
    /* The recorded hash is the working-tree content, verifiable independently. */
    expect_hash_of(&s, "int main(void){return 0;}\n", "main.c");
    snapshot_free(&s);

    snap(&e, "src/util.c", &s);
    T_CHECK(s.found);
    T_EQ_STR(atlas_buf_cstr(&s.path_text), "src/util.c");
    snapshot_free(&s);

    env_close(&e);
}

/* --- required test 4: dirty repository scan ------------------------------ */

static void test_dirty_scan(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "tracked.txt", "one\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "initial", &err), &err);

    /* One unstaged modification, one staged addition, one untracked file. */
    T_OK(fx_write(repo, "tracked.txt", "one\ntwo\n", &err), &err);
    T_OK(fx_write(repo, "staged.txt", "staged\n", &err), &err);
    const char *add_args[] = {"add", "--", "staged.txt"};
    T_OK(fx_git_ok(&e.fx, repo, add_args, 3u, &err), &err);
    T_OK(fx_write(repo, "untracked.txt", "untracked\n", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);

    T_CHECK_MSG(sum.dirty, "the worktree has uncommitted changes and must be reported dirty");
    /* The staged addition is part of the index, so it is scanned. */
    T_EQ_INT(sum.files_total, 2);

    atlas_status_report rep;
    atlas_status_report_init(&rep);
    T_OK(atlas_service_status(e.ctx, REPO_NAME, &rep, &err), &err);
    T_CHECK(rep.git_ok);
    T_CHECK(rep.live_state.dirty);
    T_EQ_INT(rep.live_state.untracked, 1);
    T_EQ_INT(rep.live_state.unstaged, 1);
    T_EQ_INT(rep.live_state.staged, 1);
    T_CHECK(rep.repo.dirty);
    atlas_status_report_free(&rep);

    /* The scanned content is what is on disk, not what was committed. */
    file_snapshot s;
    snap(&e, "tracked.txt", &s);
    expect_hash_of(&s, "one\ntwo\n", "modified working-tree file");
    snapshot_free(&s);

    env_close(&e);
}

/* --- required test 5: repeated unchanged scan is idempotent -------------- */

static void test_repeated_scan_is_idempotent(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_write(repo, "b.txt", "b\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "initial", &err), &err);

    env_register(&e);
    atlas_scan_summary first;
    env_scan(&e, &first);
    T_EQ_INT(first.files_added, 2);

    int64_t files = count_sql(&e, "SELECT count(*) FROM files;");
    int64_t commits = count_sql(&e, "SELECT count(*) FROM commits;");
    int64_t changes = count_sql(&e, "SELECT count(*) FROM file_changes;");
    int64_t evidence = count_sql(&e, "SELECT count(*) FROM evidence;");

    for (int round = 0; round < 3; round++) {
        atlas_scan_summary again;
        env_scan(&e, &again);
        T_CHECK_MSG(again.files_added == 0, "round %d reported %lld additions", round,
                    (long long)again.files_added);
        T_EQ_INT(again.files_modified, 0);
        T_EQ_INT(again.files_deleted, 0);
        T_EQ_INT(again.files_unchanged, 2);
        T_EQ_INT(again.commits_ingested, 0);
        T_EQ_INT(again.changes_ingested, 0);
        /* No new evidence: nothing new was learned. */
        T_EQ_INT(again.evidence_created, 0);

        T_EQ_INT(count_sql(&e, "SELECT count(*) FROM files;"), files);
        T_EQ_INT(count_sql(&e, "SELECT count(*) FROM commits;"), commits);
        T_EQ_INT(count_sql(&e, "SELECT count(*) FROM file_changes;"), changes);
        T_EQ_INT(count_sql(&e, "SELECT count(*) FROM evidence;"), evidence);
    }
    env_close(&e);
}

/* --- required tests 6 to 10: change detection ---------------------------- */

static void test_added_modified_deleted_renamed_exec(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "keep.txt", "keep\n", &err), &err);
    T_OK(fx_write(repo, "change.txt", "before\n", &err), &err);
    T_OK(fx_write(repo, "remove.txt", "remove\n", &err), &err);
    T_OK(fx_write(repo, "move.txt", "move me somewhere else entirely\n", &err), &err);
    T_OK(fx_write(repo, "flip.sh", "#!/bin/sh\nexit 0\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "initial", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_EQ_INT(sum.files_added, 5);

    /* Required test 6: an added file. */
    T_OK(fx_write(repo, "added.txt", "new\n", &err), &err);
    /* Required test 7: a modified file. */
    T_OK(fx_write(repo, "change.txt", "after\n", &err), &err);
    /* Required test 8: a deleted file. */
    const char *rm_args[] = {"rm", "-q", "--", "remove.txt"};
    T_OK(fx_git_ok(&e.fx, repo, rm_args, 4u, &err), &err);
    /* Required test 9: a renamed file. */
    const char *mv_args[] = {"mv", "move.txt", "moved.txt"};
    T_OK(fx_git_ok(&e.fx, repo, mv_args, 3u, &err), &err);
    /* Required test 10: an executable-bit change with identical content. */
    T_OK(fx_chmod(repo, "flip.sh", 0755, &err), &err);

    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "add, modify, delete, rename, chmod", &err), &err);

    env_scan(&e, &sum);
    T_EQ_INT(sum.files_total, 5);
    /* added.txt and moved.txt are new paths. */
    T_EQ_INT(sum.files_added, 2);
    /* change.txt content and flip.sh mode both changed. */
    T_EQ_INT(sum.files_modified, 2);
    /* remove.txt and move.txt are gone from the index. */
    T_EQ_INT(sum.files_deleted, 2);
    T_EQ_INT(sum.files_unchanged, 1);

    file_snapshot s;
    snap(&e, "added.txt", &s);
    T_CHECK(s.found);
    T_CHECK(!s.deleted);
    expect_hash_of(&s, "new\n", "added.txt");
    snapshot_free(&s);

    snap(&e, "change.txt", &s);
    T_CHECK(s.found);
    expect_hash_of(&s, "after\n", "change.txt");
    snapshot_free(&s);

    /* A deleted path is retained and marked, not dropped: its history matters. */
    snap(&e, "remove.txt", &s);
    T_CHECK_MSG(s.found, "a deleted path must remain in the index");
    T_CHECK(s.deleted);
    snapshot_free(&s);

    snap(&e, "move.txt", &s);
    T_CHECK(s.found);
    T_CHECK(s.deleted);
    snapshot_free(&s);

    snap(&e, "moved.txt", &s);
    T_CHECK(s.found);
    T_CHECK(!s.deleted);
    snapshot_free(&s);

    /* The executable bit is recorded from the git mode. */
    snap(&e, "flip.sh", &s);
    T_CHECK(s.found);
    T_CHECK_MSG(s.is_executable, "the executable bit change was not recorded");
    T_EQ_STR(atlas_buf_cstr(&s.git_mode), "100755");
    /* Content is unchanged, so the content hash must be unchanged too. */
    expect_hash_of(&s, "#!/bin/sh\nexit 0\n", "flip.sh");
    snapshot_free(&s);

    /* The rename is recorded as a rename, with both sides of the pair. */
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM file_changes WHERE change_type='rename';"), 1);
    T_EQ_INT(count_sql(&e,
                       "SELECT count(*) FROM file_changes WHERE change_type='rename'"
                       " AND old_path_text='move.txt' AND path_text='moved.txt';"),
             1);
    T_CHECK(count_sql(&e, "SELECT count(*) FROM file_changes WHERE change_type='delete';") >= 1);
    T_CHECK(count_sql(&e, "SELECT count(*) FROM file_changes WHERE change_type='add';") >= 1);
    T_CHECK(count_sql(&e, "SELECT count(*) FROM file_changes WHERE change_type='modify';") >= 1);

    /* History for the new name reaches back through the rename. */
    int64_t hits = 0;
    T_OK(atlas_service_history(e.ctx, REPO_NAME, "moved.txt", 50, NULL, NULL, &hits, &err), &err);
    T_CHECK_MSG(hits >= 1, "history for a renamed path should find the rename");

    env_close(&e);
}

/* --- required test 11: tracked symlink ----------------------------------- */

static void test_tracked_symlink(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "target.txt", "the real contents of the target file\n", &err), &err);
    T_OK(fx_symlink(repo, "target.txt", "link.txt", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "add a symlink", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_EQ_INT(sum.files_total, 2);

    file_snapshot s;
    snap(&e, "link.txt", &s);
    T_REQUIRE(s.found);
    T_EQ_STR(atlas_buf_cstr(&s.file_type), "symlink");
    T_CHECK(s.is_symlink);
    T_EQ_STR(atlas_buf_cstr(&s.git_mode), "120000");
    /* The hash is of the link text, exactly as git stores a symlink blob. */
    expect_hash_of(&s, "target.txt", "symlink content is the link text");
    T_EQ_INT(s.size_bytes, (int64_t)strlen("target.txt"));

    /* And it is definitely not the hash of what the link points at. */
    char target_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("the real contents of the target file\n",
                     strlen("the real contents of the target file\n"), target_hash);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&s.content_hash), target_hash) != 0,
                "the symlink was followed: its hash matches the target's contents");
    snapshot_free(&s);

    env_close(&e);
}

/* --- required test 12: symlink escape attempts --------------------------- */

static void test_symlink_escape_attempt(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    /* A tracked symlink aimed at a sensitive file outside the repository. */
    T_OK(fx_symlink(repo, "/etc/passwd", "escape.txt", &err), &err);
    /* A tracked file inside a directory that will be replaced by a symlink
     * pointing outside the repository. */
    T_OK(fx_mkdir(repo, "sub", &err), &err);
    T_OK(fx_write(repo, "sub/inside.txt", "inside\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "escape attempts", &err), &err);

    /* Build the escape: sub/ becomes a symlink to a directory outside the repo
     * that also contains inside.txt with different contents. */
    atlas_buf outside = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&outside, &err, "%s/outside", atlas_buf_cstr(&e.fx.root)), &err);
    T_OK(fx_mkdir(atlas_buf_cstr(&e.fx.root), "outside", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&outside), "inside.txt", "SECRET OUTSIDE CONTENT\n", &err), &err);
    T_OK(fx_remove(repo, "sub/inside.txt", &err), &err);
    {
        atlas_buf subdir = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&subdir, &err, "%s/sub", repo), &err);
        T_REQUIRE(rmdir(atlas_buf_cstr(&subdir)) == 0);
        atlas_buf_free(&subdir);
    }
    T_OK(fx_symlink(repo, "../outside", "sub", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);

    /* The scan completes: a hostile layout is data, not a crash. */
    T_EQ_INT(sum.files_total, 2);
    T_CHECK_MSG(sum.files_unsafe == 1, "expected exactly one refused path, got %lld",
                (long long)sum.files_unsafe);

    file_snapshot s;
    snap(&e, "escape.txt", &s);
    T_REQUIRE(s.found);
    T_EQ_STR(atlas_buf_cstr(&s.file_type), "symlink");
    /* Only the link text was read; /etc/passwd was never opened. */
    expect_hash_of(&s, "/etc/passwd", "a symlink aimed outside the repository");
    T_EQ_INT(s.size_bytes, (int64_t)strlen("/etc/passwd"));
    snapshot_free(&s);

    snap(&e, "sub/inside.txt", &s);
    T_REQUIRE(s.found);
    T_CHECK_MSG(s.unsafe_path, "a path traversing a symlinked directory must be refused");
    T_EQ_STR(atlas_buf_cstr(&s.content_hash), "");
    T_CHECK(strstr(atlas_buf_cstr(&s.read_error), "symlink") != NULL);
    /* The content outside the repository was never read. */
    char secret[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("SECRET OUTSIDE CONTENT\n", strlen("SECRET OUTSIDE CONTENT\n"), secret);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&s.content_hash), secret) != 0,
                "content from outside the repository was read through a symlinked directory");
    snapshot_free(&s);

    atlas_buf_free(&outside);
    env_close(&e);
}

/* --- required tests 13 to 16: hostile filenames -------------------------- */

static void check_weird_name(const void *name, size_t name_len, const char *expected_text,
                             const char *contents, const char *what) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);

    if (!fx_can_create_name(repo, name, name_len)) {
        atlas_test_note("this filesystem refuses %s; skipping", what);
        env_close(&e);
        return;
    }
    T_OK(fx_write_bytes(repo, name, name_len, contents, strlen(contents), 0644, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "add a hostile filename", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_CHECK_MSG(sum.files_total == 1, "%s: expected 1 file, got %lld", what,
                (long long)sum.files_total);

    /* Looked up by its exact bytes, never by a re-parsed string. */
    file_snapshot s;
    snap_bytes(&e, name, name_len, &s);
    T_CHECK_MSG(s.found, "%s: the path could not be found by its exact bytes", what);
    if (s.found) {
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&s.path_text), expected_text) == 0,
                    "%s: expected text form \"%s\", got \"%s\"", what, expected_text,
                    atlas_buf_cstr(&s.path_text));
        expect_hash_of(&s, contents, what);
        /* The text form is always valid UTF-8 and therefore safe to print. */
        T_CHECK(atlas_utf8_valid(s.path_text.data, s.path_text.len));
    }
    snapshot_free(&s);
    env_close(&e);
}

static void test_filename_with_space(void) {
    check_weird_name("with space.txt", 14u, "with space.txt", "spaces\n", "a name with a space");
}

static void test_filename_with_tab(void) {
    check_weird_name("with\ttab.txt", 12u, "with%09tab.txt", "tabs\n", "a name with a tab");
}

static void test_filename_with_newline(void) {
    check_weird_name("with\nnewline.txt", 16u, "with%0Anewline.txt", "newlines\n",
                     "a name with a newline");
}

static void test_filename_non_utf8(void) {
    static const char name[] = {'b', 'a', 'd', (char)0xff, (char)0xfe, '.', 't', 'x', 't'};
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);

    if (!fx_can_create_name(repo, name, sizeof(name))) {
        atlas_test_note("this filesystem refuses non-UTF-8 filenames; skipping");
        env_close(&e);
        return;
    }
    T_OK(fx_write_bytes(repo, name, sizeof(name), "bytes\n", 6u, 0644, &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "add a non-UTF-8 filename", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_EQ_INT(sum.files_total, 1);

    file_snapshot s;
    snap_bytes(&e, name, sizeof(name), &s);
    T_REQUIRE(s.found);
    T_CHECK_MSG(!s.path_is_utf8, "the path should be recorded as not valid UTF-8");
    T_EQ_STR(atlas_buf_cstr(&s.path_text), "bad%FF%FE.txt");
    /* The text form is printable and safe; the raw bytes remain the key. */
    T_CHECK(atlas_utf8_valid(s.path_text.data, s.path_text.len));
    expect_hash_of(&s, "bytes\n", "non-UTF-8 filename");
    snapshot_free(&s);

    /* The escaped text form is accepted as input too, so output can be pasted
     * back in. */
    T_OK(atlas_service_file(e.ctx, REPO_NAME, "bad%FF%FE.txt", NULL, NULL, &err), &err);

    env_close(&e);
}

/* --- required test 17: detached HEAD ------------------------------------- */

static void test_detached_head(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    T_OK(fx_write(repo, "b.txt", "b\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "second", &err), &err);

    const char *detach[] = {"checkout", "-q", "--detach", "HEAD"};
    T_OK(fx_git_ok(&e.fx, repo, detach, 4u, &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);

    T_EQ_STR(sum.head_state, "detached");
    T_CHECK_MSG(sum.branch[0] == '\0', "a detached HEAD has no branch, got \"%s\"", sum.branch);
    T_CHECK(sum.head_oid[0] != '\0');
    T_EQ_INT(sum.files_total, 2);
    T_EQ_INT(sum.commits_ingested, 2);

    atlas_status_report rep;
    atlas_status_report_init(&rep);
    T_OK(atlas_service_status(e.ctx, REPO_NAME, &rep, &err), &err);
    T_EQ_STR(rep.repo.head_state, "detached");
    T_EQ_STR(rep.live_head.state, "detached");
    T_CHECK(!rep.head_drift);
    atlas_status_report_free(&rep);

    env_close(&e);
}

/* --- required test 18: empty repository ---------------------------------- */

static void test_empty_repository(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    env_register(&e);

    atlas_scan_summary sum;
    env_scan(&e, &sum);

    /* An unborn HEAD is a normal state, not an error. */
    T_EQ_STR(sum.head_state, "unborn");
    T_EQ_STR(sum.head_oid, "");
    T_EQ_INT(sum.files_total, 0);
    T_EQ_INT(sum.commits_seen, 0);
    T_EQ_INT(sum.commits_ingested, 0);
    T_CHECK(!sum.dirty);

    atlas_status_report rep;
    atlas_status_report_init(&rep);
    T_OK(atlas_service_status(e.ctx, REPO_NAME, &rep, &err), &err);
    T_CHECK(rep.git_ok);
    T_EQ_STR(rep.live_head.state, "unborn");
    T_EQ_INT(rep.counts.files_live, 0);
    atlas_status_report_free(&rep);

    /* Scanning again is still a no-op. */
    env_scan(&e, &sum);
    T_EQ_INT(sum.files_total, 0);

    env_close(&e);
}

/* --- required tests 19 and 20: object formats ---------------------------- */

static void check_object_format(const char *format, size_t expected_oid_len) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    atlas_status st = fx_init_repo(&e.fx, repo, format, &err);
    if (st != ATLAS_OK) {
        atlas_test_note("this git cannot create %s repositories; skipping", format);
        env_close(&e);
        return;
    }
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);

    T_CHECK_MSG(strlen(sum.head_oid) == expected_oid_len, "%s: head oid is %zu chars, expected %zu",
                format, strlen(sum.head_oid), expected_oid_len);

    atlas_repo_info info;
    atlas_repo_info_init(&info);
    bool found = false;
    T_OK(atlas_db_repo_get(atlas_ctx_db(e.ctx), REPO_NAME, &info, &found, &err), &err);
    T_REQUIRE(found);
    T_EQ_STR(info.object_format, format);
    T_CHECK(strlen(info.scanned_head) == expected_oid_len);
    atlas_repo_info_free(&info);

    file_snapshot s;
    snap(&e, "a.txt", &s);
    T_REQUIRE(s.found);
    T_CHECK_MSG(strlen(atlas_buf_cstr(&s.git_index_oid)) == expected_oid_len,
                "%s: index oid is %zu chars", format, strlen(atlas_buf_cstr(&s.git_index_oid)));
    /* The content hash is Atlas' own SHA-256 and does not depend on the git
     * object format. */
    expect_hash_of(&s, "a\n", "content hash under a different object format");
    snapshot_free(&s);

    env_close(&e);
}

static void test_sha1_repository(void) {
    check_object_format("sha1", 40u);
}

static void test_sha256_repository(void) {
    check_object_format("sha256", 64u);
}

/* --- required tests 21 and 22: compile_commands.json --------------------- */

static void test_no_compile_database(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "no compile db here", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_CHECK(!sum.compile_db_found);
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM compile_databases;"), 0);
    env_close(&e);
}

static void test_compile_database_regular_and_symlink(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "main.c", "int main(void){return 0;}\n", &err), &err);
    /* Untracked and generated, which is the usual case. */
    T_OK(fx_write(repo, "compile_commands.json", "[]\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "with a compile database", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_CHECK_MSG(sum.compile_db_found, "compile_commands.json was not detected");
    T_CHECK(!sum.compile_db_is_symlink);
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM compile_databases WHERE is_regular_file=1;"), 1);
    /* A0 records it and stops there. */
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM compile_databases WHERE parsed=1;"), 0);
    T_EQ_INT(count_sql(&e,
                       "SELECT count(*) FROM compile_databases WHERE content_hash IS NOT NULL;"),
             1);

    env_close(&e);

    /* Required test 22: the same name as a symlink must be recorded as a symlink
     * and must not be followed. */
    env_open(&e);
    repo = fx_repo(&e.fx);
    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "real_cc.json", "[{\"file\":\"main.c\"}]\n", &err), &err);
    T_OK(fx_symlink(repo, "real_cc.json", "compile_commands.json", &err), &err);
    T_OK(fx_write(repo, "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "compile database via symlink", &err), &err);

    env_register(&e);
    env_scan(&e, &sum);
    T_CHECK(sum.compile_db_found);
    T_CHECK_MSG(sum.compile_db_is_symlink, "the compile database symlink was not recognised");
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM compile_databases WHERE is_symlink=1;"), 1);
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM compile_databases WHERE is_regular_file=1;"), 0);

    /* The recorded hash is of the link text, not the file it points at. */
    char link_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("real_cc.json", strlen("real_cc.json"), link_hash);
    atlas_buf sql = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sql, &err,
                           "SELECT count(*) FROM compile_databases WHERE content_hash='%s';",
                           link_hash),
         &err);
    T_EQ_INT(count_sql(&e, atlas_buf_cstr(&sql)), 1);
    atlas_buf_free(&sql);

    env_close(&e);
}

/* --- scan option coverage ------------------------------------------------ */

static void test_scan_without_history(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);

    env_register(&e);
    atlas_scan_opts opts;
    atlas_scan_opts_init(&opts);
    opts.skip_history = true;
    atlas_scan_summary sum;
    T_OK(atlas_service_scan(e.ctx, REPO_NAME, &opts, &sum, &err), &err);

    T_CHECK(sum.history_skipped);
    T_EQ_INT(sum.commits_seen, 0);
    T_EQ_INT(sum.files_total, 1);
    T_EQ_INT(count_sql(&e, "SELECT count(*) FROM commits;"), 0);
    env_close(&e);
}

static void test_scan_max_commits(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    for (int i = 0; i < 5; i++) {
        char name[32];
        (void)snprintf(name, sizeof(name), "f%d.txt", i);
        T_OK(fx_write(repo, name, "x\n", &err), &err);
        T_OK(fx_add_all(&e.fx, repo, &err), &err);
        T_OK(fx_commit(&e.fx, repo, name, &err), &err);
    }

    env_register(&e);
    atlas_scan_opts opts;
    atlas_scan_opts_init(&opts);
    opts.max_commits = 2;
    atlas_scan_summary sum;
    T_OK(atlas_service_scan(e.ctx, REPO_NAME, &opts, &sum, &err), &err);

    /* A bounded walk is reported honestly rather than presented as complete. */
    T_EQ_INT(sum.commits_seen, 2);
    T_EQ_INT(sum.commits_ingested, 2);
    T_EQ_INT(sum.files_total, 5);
    env_close(&e);
}

static void test_stale_registration_is_refused(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    env_register(&e);

    atlas_scan_summary sum;
    env_scan(&e, &sum);

    /* Turn the registered root into a subdirectory of a different repository, so
     * its canonical root changes. Scanning must refuse rather than silently index
     * something else. */
    atlas_buf gitdir = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&gitdir, &err, "%s/.git", repo), &err);
    {
        atlas_buf moved = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&moved, &err, "%s/.git-disabled", repo), &err);
        T_REQUIRE(rename(atlas_buf_cstr(&gitdir), atlas_buf_cstr(&moved)) == 0);
        atlas_buf_free(&moved);
    }
    /* The parent directory becomes a repository, so `repo` is now inside it. */
    T_OK(fx_init_repo(&e.fx, atlas_buf_cstr(&e.fx.root), NULL, &err), &err);

    atlas_scan_opts opts;
    atlas_scan_opts_init(&opts);
    atlas_scan_summary sum2;
    T_FAILS_WITH(atlas_service_scan(e.ctx, REPO_NAME, &opts, &sum2, &err), ATLAS_ERR_INTEGRITY,
                 &err);
    T_CHECK(strstr(atlas_err_msg(&err), "re-register") != NULL);

    atlas_buf_free(&gitdir);
    env_close(&e);
}

static void test_non_repository_is_rejected(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);

    /* A plain directory is not a repository. */
    T_FAILS_WITH(atlas_service_repo_add(e.ctx, fx_repo(&e.fx), REPO_NAME, NULL, &err),
                 ATLAS_ERR_REPO, &err);
    /* Neither is a path that does not exist. */
    T_FAILS_WITH(atlas_service_repo_add(e.ctx, "/nonexistent/atlas/path", "x", NULL, &err),
                 ATLAS_ERR_REPO, &err);
    /* Nor a file. */
    T_OK(fx_write(fx_repo(&e.fx), "afile", "x\n", &err), &err);
    atlas_buf filepath = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&filepath, &err, "%s/afile", fx_repo(&e.fx)), &err);
    T_FAILS_WITH(atlas_service_repo_add(e.ctx, atlas_buf_cstr(&filepath), "y", NULL, &err),
                 ATLAS_ERR_REPO, &err);
    atlas_buf_free(&filepath);
    env_close(&e);
}

static void test_submodule_is_recorded_not_followed(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "a.txt", "a\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);

    /* Build a gitlink entry directly, which is what a submodule looks like in the
     * index, without needing network access. */
    atlas_buf inner = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&inner, &err, "%s/inner", repo), &err);
    T_OK(fx_mkdir(repo, "inner", &err), &err);
    T_OK(fx_init_repo(&e.fx, atlas_buf_cstr(&inner), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&inner), "secret.txt", "inner content\n", &err), &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&inner), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&inner), "inner commit", &err), &err);

    const char *add_inner[] = {"add", "--", "inner"};
    int code = 0;
    T_OK(fx_git(&e.fx, repo, add_inner, 3u, &code, NULL, &err), &err);
    if (code != 0) {
        atlas_test_note("this git refused to add a nested repository; skipping");
        atlas_buf_free(&inner);
        env_close(&e);
        return;
    }

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);

    file_snapshot s;
    snap(&e, "inner", &s);
    if (s.found) {
        T_EQ_STR(atlas_buf_cstr(&s.git_mode), "160000");
        T_EQ_STR(atlas_buf_cstr(&s.file_type), "other");
        /* The submodule's contents belong to another repository and are not read. */
        T_EQ_STR(atlas_buf_cstr(&s.content_hash), "");
        T_CHECK(strstr(atlas_buf_cstr(&s.read_error), "submodule") != NULL);
    }
    snapshot_free(&s);
    atlas_buf_free(&inner);
    env_close(&e);
}

static void test_tracked_file_missing_from_worktree(void) {
    scan_env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e);
    const char *repo = fx_repo(&e.fx);

    T_OK(fx_init_repo(&e.fx, repo, NULL, &err), &err);
    T_OK(fx_write(repo, "gone.txt", "here for now\n", &err), &err);
    T_OK(fx_add_all(&e.fx, repo, &err), &err);
    T_OK(fx_commit(&e.fx, repo, "first", &err), &err);
    /* Deleted from disk but still in the index. */
    T_OK(fx_remove(repo, "gone.txt", &err), &err);

    env_register(&e);
    atlas_scan_summary sum;
    env_scan(&e, &sum);
    T_EQ_INT(sum.files_total, 1);

    file_snapshot s;
    snap(&e, "gone.txt", &s);
    T_REQUIRE(s.found);
    /* Still tracked, so not "deleted" in Atlas terms, but clearly not readable. */
    T_EQ_STR(atlas_buf_cstr(&s.file_type), "missing");
    T_CHECK(!s.deleted);
    T_EQ_STR(atlas_buf_cstr(&s.content_hash), "");
    T_CHECK(strstr(atlas_buf_cstr(&s.read_error), "not present") != NULL);
    snapshot_free(&s);
    env_close(&e);
}

static void test_language_detection(void) {
    T_EQ_STR(atlas_detect_language("src/core/buf.c", 14u), "c");
    T_EQ_STR(atlas_detect_language("include/atlas/buf.h", 19u), "c-header");
    T_EQ_STR(atlas_detect_language("CMakeLists.txt", 14u), "cmake");
    T_EQ_STR(atlas_detect_language("Makefile", 8u), "make");
    T_EQ_STR(atlas_detect_language("docs/readme.md", 14u), "markdown");
    T_EQ_STR(atlas_detect_language("a/b/script.sh", 13u), "shell");
    T_CHECK(atlas_detect_language("noextension", 11u) == NULL);
    T_CHECK(atlas_detect_language("", 0) == NULL);
    /* A directory whose name looks like an extension must not fool detection. */
    T_CHECK(atlas_detect_language("weird.c/plain", 13u) == NULL);
}

static const atlas_test TESTS[] = {
    {"clean repository scan", test_clean_scan},
    {"dirty repository scan", test_dirty_scan},
    {"repeated unchanged scan is idempotent", test_repeated_scan_is_idempotent},
    {"added, modified, deleted, renamed and chmod", test_added_modified_deleted_renamed_exec},
    {"tracked symlink hashes the link text", test_tracked_symlink},
    {"symlink escape attempts are refused", test_symlink_escape_attempt},
    {"filename containing a space", test_filename_with_space},
    {"filename containing a tab", test_filename_with_tab},
    {"filename containing a newline", test_filename_with_newline},
    {"filename that is not valid UTF-8", test_filename_non_utf8},
    {"detached HEAD", test_detached_head},
    {"empty repository", test_empty_repository},
    {"sha1 object format", test_sha1_repository},
    {"sha256 object format", test_sha256_repository},
    {"missing compile_commands.json", test_no_compile_database},
    {"compile_commands.json as a file and as a symlink",
     test_compile_database_regular_and_symlink},
    {"scan without history", test_scan_without_history},
    {"scan with a commit ceiling", test_scan_max_commits},
    {"a stale registration is refused", test_stale_registration_is_refused},
    {"non-repositories are rejected", test_non_repository_is_rejected},
    {"submodules are recorded, not followed", test_submodule_is_recorded_not_followed},
    {"tracked file missing from the working tree", test_tracked_file_missing_from_worktree},
    {"language detection", test_language_detection},
};

ATLAS_TEST_MAIN("scan", TESTS)

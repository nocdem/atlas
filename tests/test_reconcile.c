/* Atlas - incremental reconciliation.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These drive atlas_reconcile_run directly, with no daemon and no threads, so
 * that the indexing behaviour is tested in isolation from the machinery that
 * schedules it. The central claim under test is the one A1 exists to make: a
 * pass over an unchanged repository reads no file content at all.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/reconcile.h"
#include "atlas_test.h"
/* For the one case that has to fabricate a row shaped like an A0-era one. No
 * production code path can produce a partial identity, so the test writes it
 * directly rather than pretending some code path might. */
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
}

/* --- the incremental claim ----------------------------------------------- */

static void test_second_pass_reads_nothing(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "int a;\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "b.c", "int b;\n", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "sub", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "sub/c.c", "int c;\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    T_CHECK(first.published);
    T_EQ_INT(first.files_examined, 3);
    T_EQ_INT(first.files_hashed, 3);
    T_EQ_INT(first.files_added, 3);
    atlas_reconcile_summary_free(&first);

    /* The property A1 is for: nothing changed, so nothing is read. Not "less
     * work" — no content read at all. */
    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    T_CHECK(second.published);
    T_EQ_INT(second.files_examined, 3);
    T_EQ_INT(second.files_hashed, 0);
    T_EQ_INT(second.files_identity_hit, 3);
    T_EQ_INT(second.files_added, 0);
    T_EQ_INT(second.files_modified, 0);
    /* And the generation advanced, so a reader can tell the passes apart. */
    T_CHECK(second.generation > first.generation);
    atlas_reconcile_summary_free(&second);

    env_close(&e);
}

static void test_one_change_reads_one_file(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    for (int i = 0; i < 12; i++) {
        char name[32];
        (void)snprintf(name, sizeof(name), "f%02d.c", i);
        T_OK(fx_write(fx_repo(&e.fx), name, "x\n", &err), &err);
    }
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    T_EQ_INT(first.files_hashed, 12);
    atlas_reconcile_summary_free(&first);

    /* A file's mtime has one-second granularity on some filesystems; the fixture
     * writes different content and a different size, so the identity differs
     * regardless. */
    T_OK(fx_write(fx_repo(&e.fx), "f05.c", "changed content\n", &err), &err);

    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    T_EQ_INT(second.files_examined, 12);
    T_EQ_INT(second.files_hashed, 1);
    T_EQ_INT(second.files_identity_hit, 11);
    T_EQ_INT(second.files_modified, 1);
    /* One changed file produces one event, not twelve. */
    T_CHECK_MSG(second.events_appended <= 2,
                "one changed file should append about one event, got %lld",
                (long long)second.events_appended);
    atlas_reconcile_summary_free(&second);
    env_close(&e);
}

static void test_delete_is_recorded(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "keep.c", "1\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "gone.c", "2\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    atlas_reconcile_summary_free(&first);

    const char *rm[] = {"rm", "gone.c"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), rm, 2u, &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "remove", &err), &err);

    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    T_EQ_INT(second.files_deleted, 1);
    atlas_reconcile_summary_free(&second);
    env_close(&e);
}

/* --- untracked discovery (the A1 acceptance criteria) -------------------- */

static void test_untracked_directory_is_discovered_per_file(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "tracked.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    /* A brand new directory holding several files, and a nested one inside it.
     * This is the A0 limitation the roadmap made an A1 acceptance criterion:
     * `git status` collapses this to one entry, and Atlas must not. */
    T_OK(fx_mkdir(fx_repo(&e.fx), "newwork", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "newwork/one.c", "1\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "newwork/two.c", "2\n", &err), &err);
    T_OK(fx_mkdir(fx_repo(&e.fx), "newwork/deeper", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "newwork/deeper/three.c", "3\n", &err), &err);

    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    T_EQ_INT(sum.untracked_discovered, 3);
    T_EQ_INT(sum.files_examined, 4); /* one tracked plus three discovered */
    atlas_reconcile_summary_free(&sum);

    /* Each discovered file is recorded individually, with its own hash. */
    static const char *const PATHS[] = {"newwork/one.c", "newwork/two.c",
                                        "newwork/deeper/three.c"};
    for (size_t i = 0; i < 3u; i++) {
        atlas_fs_identity id;
        int64_t file_id = 0;
        bool found = false;
        T_OK(atlas_db_file_identity(e.db, e.repo_id, PATHS[i], strlen(PATHS[i]), &id, &file_id,
                                    &found, &err),
             &err);
        T_CHECK_MSG(found, "%s should have been indexed individually", PATHS[i]);
        T_CHECK_MSG(id.known, "%s should have a recorded filesystem identity", PATHS[i]);
    }
    env_close(&e);
}

static void test_ignored_files_are_reported_separately(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), ".gitignore", "build/\n*.o\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    T_OK(fx_mkdir(fx_repo(&e.fx), "build", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "build/out.bin", "junk\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "src.o", "junk\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "visible.c", "2\n", &err), &err);

    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    /* The ignored files are not indexed... */
    T_EQ_INT(sum.untracked_discovered, 1);
    /* ...and "skipped because ignored" is reported separately from "skipped
     * because a ceiling was reached", which is the distinction that lets a user
     * tell a working ignore rule from a truncated scan. */
    T_CHECK_MSG(sum.ignored_paths >= 2, "expected at least two ignored roots, got %lld",
                (long long)sum.ignored_paths);
    T_CHECK(!sum.truncated);

    bool found = false;
    atlas_fs_identity id;
    int64_t fid = 0;
    T_OK(atlas_db_file_identity(e.db, e.repo_id, "build/out.bin", 13u, &id, &fid, &found, &err),
         &err);
    T_CHECK_MSG(!found, "an ignored file must not be indexed");
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}

typedef struct link_row {
    atlas_buf hash;
    bool is_symlink;
} link_row;

/* Row callbacks receive borrowed pointers valid only for the call, so anything
 * that has to outlive it is copied here. */
static atlas_status capture_link_row(const atlas_file_row *row, void *ud, atlas_err *err) {
    link_row *out = (link_row *)ud;
    out->is_symlink = row->is_symlink;
    return atlas_buf_set_str(&out->hash, row->content_hash != NULL ? row->content_hash : "", err);
}

static void test_symlink_out_of_tree_is_not_followed(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "real.c", "1\n", &err), &err);
    /* A link pointing at a file outside the repository. Atlas records the link
     * text and must never read through it. */
    T_OK(fx_symlink(fx_repo(&e.fx), "/etc/passwd", "escape", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary sum;
    run_pass(&e, false, &sum, &err);
    atlas_reconcile_summary_free(&sum);

    /* The recorded hash must be of the link *text*. Hashing the target's content
     * would mean a repository could make Atlas read any file the user can. */
    char expected[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("/etc/passwd", strlen("/etc/passwd"), expected);

    link_row got;
    memset(&got, 0, sizeof(got));
    atlas_buf_init(&got.hash);
    bool found = false;
    T_OK(atlas_db_file_get(e.db, e.repo_id, "escape", 6u, capture_link_row, &got, &found, &err),
         &err);
    T_CHECK(found);
    T_CHECK_MSG(got.is_symlink, "the entry should be recorded as a symlink");
    T_EQ_STR(atlas_buf_cstr(&got.hash), expected);
    atlas_buf_free(&got.hash);
    env_close(&e);
}

/* --- history ------------------------------------------------------------- */

static void test_history_is_incremental(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "one", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "2\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "two", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    T_EQ_INT(first.commits_ingested, 2);
    T_CHECK(first.history_full_replay);
    atlas_reconcile_summary_free(&first);

    /* Nothing new: the walk must find no commits at all, not re-walk two. */
    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    T_EQ_INT(second.commits_ingested, 0);
    T_EQ_INT(second.commits_seen, 0);
    T_CHECK(!second.history_full_replay);
    atlas_reconcile_summary_free(&second);

    /* One new commit: exactly one is walked. */
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "3\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "three", &err), &err);
    atlas_reconcile_summary third;
    run_pass(&e, false, &third, &err);
    T_EQ_INT(third.commits_ingested, 1);
    T_EQ_INT(third.commits_seen, 1);
    atlas_reconcile_summary_free(&third);
    env_close(&e);
}

static void test_branch_rewrite_is_detected(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "one", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "2\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "two", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    atlas_reconcile_summary_free(&first);

    /* Reset back one commit and commit something else: the stored tip is now
     * unreachable from HEAD, which is what a force-push looks like from the
     * index's point of view. */
    const char *reset[] = {"reset", "--hard", "HEAD~1"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), reset, 3u, &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "different\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "rewritten", &err), &err);

    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    T_CHECK_MSG(second.branch_rewrite, "a rewritten history must be detected, not walked past");
    T_CHECK(second.history_full_replay);
    atlas_reconcile_summary_free(&second);
    env_close(&e);
}

static void test_unborn_and_detached_head(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    env_index(&e, &err);

    /* An unborn HEAD: no commits at all. A pass must succeed and say so rather
     * than fail trying to walk nothing. */
    atlas_reconcile_summary unborn;
    run_pass(&e, false, &unborn, &err);
    T_CHECK(unborn.published);
    T_EQ_STR(unborn.head_state, "unborn");
    T_EQ_INT(unborn.commits_ingested, 0);
    atlas_reconcile_summary_free(&unborn);

    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "one", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "2\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "two", &err), &err);

    atlas_reconcile_summary born;
    run_pass(&e, false, &born, &err);
    T_EQ_STR(born.head_state, "born");
    atlas_reconcile_summary_free(&born);

    /* Detached HEAD gets its own tip key, so checking out a commit does not
     * corrupt the branch's recorded position. */
    const char *co[] = {"checkout", "--detach", "HEAD~1"};
    T_OK(fx_git_ok(&e.fx, fx_repo(&e.fx), co, 3u, &err), &err);
    atlas_reconcile_summary det;
    run_pass(&e, false, &det, &err);
    T_CHECK(det.published);
    T_EQ_STR(det.head_state, "detached");
    atlas_reconcile_summary_free(&det);
    env_close(&e);
}

/* --- generations and events ---------------------------------------------- */

static void test_generation_and_event_cursor(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "one", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, true, &first, &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_OK(atlas_db_index_state_get(e.db, e.repo_id, &s, &err), &err);
    T_CHECK(s.present);
    /* Only a completed pass is published, and it is the only generation a reader
     * is ever shown. */
    T_EQ_INT(s.last_complete_generation, first.generation);
    T_CHECK_MSG(!s.event_gap, "a completed full pass must leave no event gap");

    int64_t head = 0;
    T_OK(atlas_db_events_head(e.db, e.repo_id, &head, &err), &err);
    T_CHECK_MSG(head > 0, "a pass that added a file must advance the event cursor");

    /* The cursor is monotonic: a later pass never renumbers earlier events. */
    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    int64_t head2 = 0;
    T_OK(atlas_db_events_head(e.db, e.repo_id, &head2, &err), &err);
    T_CHECK_MSG(head2 >= head, "the event cursor must never move backwards");

    atlas_index_state_free(&s);
    atlas_reconcile_summary_free(&first);
    atlas_reconcile_summary_free(&second);
    env_close(&e);
}

static void test_repeated_passes_do_not_duplicate_evidence(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "one", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    atlas_repo_counts after_first;
    T_OK(atlas_db_repo_counts(e.db, e.repo_id, &after_first, &err), &err);
    atlas_reconcile_summary_free(&first);

    for (int i = 0; i < 4; i++) {
        atlas_reconcile_summary s;
        run_pass(&e, false, &s, &err);
        atlas_reconcile_summary_free(&s);
    }
    atlas_repo_counts after_many;
    T_OK(atlas_db_repo_counts(e.db, e.repo_id, &after_many, &err), &err);

    /* Durable evidence must not grow when nothing changed. An indexer whose
     * database grows on every idle pass is one nobody can leave running. */
    T_EQ_INT(after_many.evidence, after_first.evidence);
    T_EQ_INT(after_many.commits, after_first.commits);
    T_EQ_INT(after_many.changes, after_first.changes);
    T_EQ_INT(after_many.files_live, after_first.files_live);
    env_close(&e);
}

static void test_gap_only_cleared_by_a_full_pass(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "one", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary warm;
    run_pass(&e, true, &warm, &err);
    atlas_reconcile_summary_free(&warm);

    /* Something was missed. */
    T_OK(atlas_db_index_state_mark_gap(e.db, e.repo_id, "simulated queue overflow", &err), &err);

    atlas_index_state s;
    atlas_index_state_init(&s);
    T_OK(atlas_db_index_state_get(e.db, e.repo_id, &s, &err), &err);
    T_CHECK(s.event_gap);
    T_EQ_INT(s.watch_state, ATLAS_WATCH_INCOMPLETE);

    /* An incremental pass cannot prove it saw what the gap hid, so it must not
     * clear it — this is the difference between being current and merely having
     * run recently. */
    atlas_reconcile_summary incr;
    run_pass(&e, false, &incr, &err);
    atlas_index_state_free(&s);
    atlas_index_state_init(&s);
    T_OK(atlas_db_index_state_get(e.db, e.repo_id, &s, &err), &err);
    T_CHECK_MSG(s.event_gap, "an incremental pass must not clear an event gap");
    atlas_reconcile_summary_free(&incr);

    /* A full pass did look at everything, so it may. */
    atlas_reconcile_summary full;
    run_pass(&e, true, &full, &err);
    atlas_index_state_free(&s);
    atlas_index_state_init(&s);
    T_OK(atlas_db_index_state_get(e.db, e.repo_id, &s, &err), &err);
    T_CHECK_MSG(!s.event_gap, "a full pass must clear the gap it resolved");
    T_CHECK(!s.pending_full_reconcile);
    atlas_reconcile_summary_free(&full);
    atlas_index_state_free(&s);
    env_close(&e);
}

/* --- the ctime defect ----------------------------------------------------
 *
 * The identity originally recorded device, inode, size, mtime and mode. Every
 * one of those survives a same-length in-place edit whose mtime is put back with
 * utimensat, so the file compared as unchanged, was never read, and kept its old
 * content hash indefinitely.
 *
 * ctime is what closes it: no syscall sets it, and the utimensat that restores
 * mtime bumps it like any other inode write. */

/* Overwrites `rel` with `bytes` (which must be the same length as the current
 * content) and restores the original mtime to the nanosecond. */
static void rewrite_same_length_restoring_mtime(const char *dir, const char *rel,
                                                const char *bytes, atlas_err *err) {
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, err, "%s/%s", dir, rel), err);

    struct stat before;
    T_REQUIRE(lstat(atlas_buf_cstr(&path), &before) == 0);
    T_REQUIRE((size_t)before.st_size == strlen(bytes));

    int fd = open(atlas_buf_cstr(&path), O_WRONLY);
    T_REQUIRE(fd >= 0);
    T_REQUIRE(write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
    T_REQUIRE(close(fd) == 0);

    /* Put mtime back exactly, nanoseconds included. atime is set to its old
     * value too so the only thing that differs afterwards is ctime. */
    struct timespec times[2];
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    T_REQUIRE(utimensat(AT_FDCWD, atlas_buf_cstr(&path), times, AT_SYMLINK_NOFOLLOW) == 0);

    struct stat after;
    T_REQUIRE(lstat(atlas_buf_cstr(&path), &after) == 0);
    /* The premise of the test: everything the old identity looked at is
     * identical, and only ctime moved. If a platform ever breaks this, the test
     * says so rather than passing vacuously. */
    T_EQ_INT(after.st_size, before.st_size);
    T_EQ_INT(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
    T_EQ_INT(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
    T_EQ_INT(after.st_ino, before.st_ino);
    T_EQ_INT(after.st_mode, before.st_mode);
    T_CHECK_MSG(after.st_ctim.tv_sec != before.st_ctim.tv_sec ||
                    after.st_ctim.tv_nsec != before.st_ctim.tv_nsec,
                "ctime must move when the file is written; this platform cannot support the "
                "identity check");
    atlas_buf_free(&path);
}

static void read_hash(env *e, const char *rel, atlas_buf *out, atlas_err *err) {
    link_row row;
    memset(&row, 0, sizeof(row));
    atlas_buf_init(&row.hash);
    bool found = false;
    T_OK(atlas_db_file_get(e->db, e->repo_id, rel, strlen(rel), capture_link_row, &row, &found,
                           err),
         err);
    T_REQUIRE(found);
    T_OK(atlas_buf_set(out, row.hash.data, row.hash.len, err), err);
    atlas_buf_free(&row.hash);
}

static void test_same_length_edit_with_restored_mtime_is_caught(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "target.c", "AAAAAAAAAAAAAAAA\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "other.c", "unrelated\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    atlas_reconcile_summary_free(&first);

    atlas_buf before_hash = ATLAS_BUF_INIT;
    read_hash(&e, "target.c", &before_hash, &err);
    T_CHECK(before_hash.len > 0);

    /* The attack, and the exact scenario in the finding: same length, different
     * bytes, original mtime restored to the nanosecond. */
    rewrite_same_length_restoring_mtime(fx_repo(&e.fx), "target.c", "BBBBBBBBBBBBBBBB\n", &err);

    atlas_reconcile_summary second;
    run_pass(&e, false, &second, &err);
    T_CHECK_MSG(second.files_hashed >= 1,
                "the modified file must be read; it was not (hashed %lld)",
                (long long)second.files_hashed);
    T_EQ_INT(second.files_modified, 1);
    /* The untouched file is still a cache hit, so the fix did not simply disable
     * incremental behaviour. */
    T_EQ_INT(second.files_identity_hit, 1);

    atlas_buf after_hash = ATLAS_BUF_INIT;
    read_hash(&e, "target.c", &after_hash, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&before_hash), atlas_buf_cstr(&after_hash)) != 0,
                "the stored content hash must change: before %s, after %s",
                atlas_buf_cstr(&before_hash), atlas_buf_cstr(&after_hash));
    T_CHECK_MSG(second.generation > first.generation, "the generation must advance");

    atlas_buf_free(&before_hash);
    atlas_buf_free(&after_hash);
    env_close(&e);
}

/* An explicitly named path is read even when its metadata tuple is untouched.
 * This is the override that does not depend on ctime at all: it is what makes an
 * observed event outrank any metadata argument. */
static void test_named_dirty_path_overrides_metadata(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_write(fx_repo(&e.fx), "b.c", "2\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    atlas_reconcile_summary_free(&first);

    /* Nothing at all has changed on disk. Naming a.c must still read it. */
    atlas_reconcile_opts opts;
    atlas_reconcile_opts_init(&opts);
    opts.dirty_paths = "a.c\0";
    opts.dirty_paths_len = 4u;

    atlas_reconcile_summary sum;
    atlas_reconcile_summary_init(&sum);
    T_OK(atlas_reconcile_run(e.db, e.g, e.repo_id, &opts, &sum, &err), &err);
    T_EQ_INT(sum.files_dirty_forced, 1);
    T_EQ_INT(sum.files_hashed, 1);
    /* The path that was not named is still a cache hit. */
    T_EQ_INT(sum.files_identity_hit, 1);
    /* Reading it changed nothing, because nothing had changed — the override
     * costs a read, never a false report. */
    T_EQ_INT(sum.files_modified, 0);
    /* And a forced read is not content verification. */
    T_CHECK_MSG(!sum.content_verified,
                "naming one path must not let a pass claim it verified everything");
    atlas_reconcile_summary_free(&sum);
    env_close(&e);
}

/* Only a completed content-verifying pass may clear an event gap — and it must
 * find content that differs from what a lying metadata tuple implied. */
static void test_gap_recovery_reads_content(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "target.c", "AAAAAAAAAAAAAAAA\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary warm;
    run_pass(&e, true, &warm, &err);
    T_CHECK(warm.content_verified);
    atlas_reconcile_summary_free(&warm);

    atlas_buf before_hash = ATLAS_BUF_INIT;
    read_hash(&e, "target.c", &before_hash, &err);

    /* Something was missed, and while nothing was watching the content changed
     * behind a metadata tuple that still looks identical. */
    T_OK(atlas_db_index_state_mark_gap(e.db, e.repo_id, "simulated queue overflow", &err), &err);
    rewrite_same_length_restoring_mtime(fx_repo(&e.fx), "target.c", "CCCCCCCCCCCCCCCC\n", &err);

    /* An incremental pass may not clear the gap, whatever it finds. */
    atlas_reconcile_summary incr;
    run_pass(&e, false, &incr, &err);
    T_CHECK_MSG(!incr.content_verified, "an incremental pass never verifies content");
    atlas_index_state s;
    atlas_index_state_init(&s);
    T_OK(atlas_db_index_state_get(e.db, e.repo_id, &s, &err), &err);
    T_CHECK_MSG(s.event_gap, "an incremental pass must not clear an event gap");
    atlas_reconcile_summary_free(&incr);

    /* The recovery pass reads the file and finds the difference. */
    atlas_reconcile_summary full;
    run_pass(&e, true, &full, &err);
    T_CHECK(full.content_verified);
    T_EQ_INT(full.files_hashed, 1);

    atlas_buf after_hash = ATLAS_BUF_INIT;
    read_hash(&e, "target.c", &after_hash, &err);
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&before_hash), atlas_buf_cstr(&after_hash)) != 0,
                "full recovery must read the bytes and record the new hash");

    atlas_index_state_free(&s);
    atlas_index_state_init(&s);
    T_OK(atlas_db_index_state_get(e.db, e.repo_id, &s, &err), &err);
    T_CHECK_MSG(!s.event_gap, "a completed content-verifying pass clears the gap");
    T_CHECK(!s.pending_full_reconcile);

    atlas_index_state_free(&s);
    atlas_reconcile_summary_free(&full);
    atlas_buf_free(&before_hash);
    atlas_buf_free(&after_hash);
    env_close(&e);
}

/* `--full` means content verification: every eligible regular file is read, and
 * no identity is consulted. */
static void test_full_pass_reads_every_eligible_file(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    const int N = 20;
    for (int i = 0; i < N; i++) {
        char name[32];
        (void)snprintf(name, sizeof(name), "f%02d.c", i);
        T_OK(fx_write(fx_repo(&e.fx), name, "x\n", &err), &err);
    }
    /* A symlink and a submodule-shaped entry are not "eligible regular files";
     * the symlink is still read as link text, so it counts as content read. */
    T_OK(fx_symlink(fx_repo(&e.fx), "f00.c", "alias", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary warm;
    run_pass(&e, true, &warm, &err);
    atlas_reconcile_summary_free(&warm);

    /* Warm cache, nothing changed: a full pass must still read everything. */
    atlas_reconcile_summary full;
    run_pass(&e, true, &full, &err);
    T_EQ_INT(full.files_identity_hit, 0);
    T_EQ_INT(full.files_hashed, full.files_examined);
    T_EQ_INT(full.files_hashed, N + 1);
    T_CHECK(full.content_verified);
    atlas_reconcile_summary_free(&full);

    /* And the incremental pass right after it still reads nothing, so "full
     * reads everything" has not been achieved by breaking the cache. */
    atlas_reconcile_summary incr;
    run_pass(&e, false, &incr, &err);
    T_EQ_INT(incr.files_hashed, 0);
    T_EQ_INT(incr.files_identity_hit, N + 1);
    atlas_reconcile_summary_free(&incr);
    env_close(&e);
}

/* A partial identity is unknown, not unchanged. A row whose ctime columns are
 * NULL — which is what every A0 row and every racy observation looks like — must
 * be read, not trusted on the strength of the fields that are present. */
static void test_partial_identity_is_not_a_hit(void) {
    env e;
    atlas_err err;
    atlas_err_init(&err);
    env_open(&e, &err);
    T_OK(fx_write(fx_repo(&e.fx), "a.c", "1\n", &err), &err);
    T_OK(fx_add_all(&e.fx, fx_repo(&e.fx), &err), &err);
    T_OK(fx_commit(&e.fx, fx_repo(&e.fx), "first", &err), &err);
    env_index(&e, &err);

    atlas_reconcile_summary first;
    run_pass(&e, false, &first, &err);
    atlas_reconcile_summary_free(&first);

    atlas_reconcile_summary hit;
    run_pass(&e, false, &hit, &err);
    T_EQ_INT(hit.files_identity_hit, 1);
    atlas_reconcile_summary_free(&hit);

    /* Blank the ctime columns, exactly as an A0-era row would have them. */
    T_OK(atlas_db_exec_sql(e.db, "UPDATE files SET fs_ctime_sec=NULL, fs_ctime_nsec=NULL;", &err),
         &err);

    atlas_reconcile_summary after;
    run_pass(&e, false, &after, &err);
    T_EQ_INT(after.files_identity_hit, 0);
    T_EQ_INT(after.files_hashed, 1);
    atlas_reconcile_summary_free(&after);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a second pass reads no file content", test_second_pass_reads_nothing},
    {"one changed file is the only file read", test_one_change_reads_one_file},
    {"a deleted file is recorded", test_delete_is_recorded},
    {"a new untracked directory is indexed per file",
     test_untracked_directory_is_discovered_per_file},
    {"ignored paths are skipped and counted separately",
     test_ignored_files_are_reported_separately},
    {"a symlink out of the tree is hashed as link text", test_symlink_out_of_tree_is_not_followed},
    {"history is walked incrementally", test_history_is_incremental},
    {"a rewritten branch is detected", test_branch_rewrite_is_detected},
    {"unborn and detached HEAD", test_unborn_and_detached_head},
    {"generations advance and the event cursor is monotonic",
     test_generation_and_event_cursor},
    {"repeated passes do not duplicate durable evidence",
     test_repeated_passes_do_not_duplicate_evidence},
    {"an event gap is cleared only by a full pass", test_gap_only_cleared_by_a_full_pass},
    /* The ctime defect and the rules that close it. */
    {"a same-length edit with the mtime restored is caught",
     test_same_length_edit_with_restored_mtime_is_caught},
    {"a named dirty path is read whatever its metadata says",
     test_named_dirty_path_overrides_metadata},
    {"gap recovery reads content and only then clears the gap", test_gap_recovery_reads_content},
    {"a full pass reads every eligible file", test_full_pass_reads_every_eligible_file},
    {"a partial identity is unknown, not unchanged", test_partial_identity_is_not_a_hit},
};

ATLAS_TEST_MAIN("reconcile", TESTS)

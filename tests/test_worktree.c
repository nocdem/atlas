/* Atlas - multiple Git worktrees sharing one object store.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Two worktrees of one repository have distinct canonical roots, HEADs, branches
 * and dirty states, but share a common Git directory. Atlas must keep them apart
 * as index entries while recording that they are related. See the identity model
 * in docs/data-model.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

#define MAIN_NAME "wt-main"
#define LINKED_NAME "wt-linked"

typedef struct wt_env {
    fixture fx;
    atlas_ctx *ctx;
    atlas_buf main_root;   /* <root>/repo, created by the fixture */
    atlas_buf linked_root; /* <root>/linked, added as a worktree */
} wt_env;

static void wt_open(wt_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->main_root);
    atlas_buf_init(&e->linked_root);

    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = fx_data_dir(&e->fx);
    T_OK(atlas_ctx_open(&opts, &e->ctx, &err), &err);

    T_OK(atlas_buf_append_str(&e->main_root, fx_repo(&e->fx), &err), &err);
    T_OK(atlas_buf_appendf(&e->linked_root, &err, "%s/linked", atlas_buf_cstr(&e->fx.root)), &err);
}

static void wt_close(wt_env *e) {
    atlas_ctx_close(e->ctx);
    e->ctx = NULL;
    atlas_buf_free(&e->main_root);
    atlas_buf_free(&e->linked_root);
    fx_close(&e->fx);
}

/* Builds a repository with one commit and adds a second worktree on a new
 * branch. Returns false when this git cannot create worktrees. */
static bool wt_build(wt_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    const char *main_root = atlas_buf_cstr(&e->main_root);

    T_OK(fx_init_repo(&e->fx, main_root, NULL, &err), &err);
    T_OK(fx_write(main_root, "shared.txt", "shared content\n", &err), &err);
    T_OK(fx_write(main_root, "main-only.txt", "main\n", &err), &err);
    T_OK(fx_add_all(&e->fx, main_root, &err), &err);
    T_OK(fx_commit(&e->fx, main_root, "initial commit", &err), &err);

    const char *args[] = {"worktree", "add", "-q", atlas_buf_cstr(&e->linked_root), "-b", "feature"};
    int code = 0;
    T_OK(fx_git(&e->fx, main_root, args, 6u, &code, NULL, &err), &err);
    if (code != 0) {
        atlas_test_note("this git cannot create worktrees (exit %d); skipping", code);
        return false;
    }
    return true;
}

static void wt_register_both(wt_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_service_repo_add(e->ctx, atlas_buf_cstr(&e->main_root), MAIN_NAME, NULL, &err), &err);
    T_OK(atlas_service_repo_add(e->ctx, atlas_buf_cstr(&e->linked_root), LINKED_NAME, NULL, &err),
         &err);
}

static void load(wt_env *e, const char *name, atlas_repo_info *out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_repo_info_init(out);
    bool found = false;
    T_OK(atlas_db_repo_get(atlas_ctx_db(e->ctx), name, out, &found, &err), &err);
    T_REQUIRE_MSG(found, "repository %s should be registered", name);
}

static void scan(wt_env *e, const char *name, atlas_scan_summary *sum) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_scan_opts opts;
    atlas_scan_opts_init(&opts);
    T_OK(atlas_service_scan(e->ctx, name, &opts, sum, &err), &err);
}

/* --- identity ------------------------------------------------------------ */

static void test_distinct_identity(void) {
    wt_env e;
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    atlas_repo_info m;
    atlas_repo_info l;
    load(&e, MAIN_NAME, &m);
    load(&e, LINKED_NAME, &l);

    /* Distinct canonical roots. */
    T_EQ_STR(atlas_buf_cstr(&m.root_path), atlas_buf_cstr(&e.main_root));
    T_EQ_STR(atlas_buf_cstr(&l.root_path), atlas_buf_cstr(&e.linked_root));
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&m.root_path), atlas_buf_cstr(&l.root_path)) != 0,
                "the two worktrees must have different canonical roots");

    /* One shared object store. */
    T_CHECK_MSG(m.git_common_dir.len > 0 && l.git_common_dir.len > 0,
                "both worktrees must record a common git dir");
    T_CHECK_MSG(m.git_common_dir.len == l.git_common_dir.len &&
                    memcmp(m.git_common_dir.data, l.git_common_dir.data, m.git_common_dir.len) == 0,
                "worktrees of one repository must share the common git dir (%s vs %s)",
                atlas_buf_cstr(&m.git_common_dir), atlas_buf_cstr(&l.git_common_dir));

    /* Distinct per-worktree git dirs: this is what tells them apart. */
    T_CHECK_MSG(m.git_dir.len > 0 && l.git_dir.len > 0, "both must record their own git dir");
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&m.git_dir), atlas_buf_cstr(&l.git_dir)) != 0,
                "the two worktrees must have different git dirs");
    T_CHECK_MSG(!m.is_linked_worktree, "the main worktree must not be marked linked");
    T_CHECK_MSG(l.is_linked_worktree, "the added worktree must be marked linked");
    /* The main worktree's git dir *is* the common dir. */
    T_CHECK(m.git_dir.len == m.git_common_dir.len &&
            memcmp(m.git_dir.data, m.git_common_dir.data, m.git_dir.len) == 0);

    atlas_repo_info_free(&m);
    atlas_repo_info_free(&l);
    wt_close(&e);
}

static void test_siblings_are_discoverable(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    atlas_status_report rep;
    atlas_status_report_init(&rep);
    T_OK(atlas_service_status(e.ctx, MAIN_NAME, &rep, &err), &err);
    T_CHECK_MSG(rep.sibling_worktrees == 1, "expected 1 sibling worktree, got %lld",
                (long long)rep.sibling_worktrees);
    atlas_status_report_free(&rep);

    atlas_status_report rep2;
    atlas_status_report_init(&rep2);
    T_OK(atlas_service_status(e.ctx, LINKED_NAME, &rep2, &err), &err);
    T_EQ_INT(rep2.sibling_worktrees, 1);
    atlas_status_report_free(&rep2);

    wt_close(&e);
}

/* --- independent state --------------------------------------------------- */

static void test_heads_and_branches_are_distinct(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    /* Advance only the linked worktree, so the two HEADs diverge. */
    T_OK(fx_write(atlas_buf_cstr(&e.linked_root), "feature.txt", "feature work\n", &err), &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&e.linked_root), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&e.linked_root), "feature commit", &err), &err);

    atlas_scan_summary ms;
    atlas_scan_summary ls;
    scan(&e, MAIN_NAME, &ms);
    scan(&e, LINKED_NAME, &ls);

    T_EQ_STR(ms.branch, "main");
    T_EQ_STR(ls.branch, "feature");
    T_CHECK_MSG(strcmp(ms.head_oid, ls.head_oid) != 0,
                "the two worktrees should be at different commits");
    /* The linked worktree has the extra file; the main one does not. */
    T_EQ_INT(ms.files_total, 2);
    T_EQ_INT(ls.files_total, 3);

    atlas_repo_info m;
    atlas_repo_info l;
    load(&e, MAIN_NAME, &m);
    load(&e, LINKED_NAME, &l);
    T_EQ_STR(m.current_branch, "main");
    T_EQ_STR(l.current_branch, "feature");
    T_CHECK(strcmp(m.scanned_head, l.scanned_head) != 0);
    atlas_repo_info_free(&m);
    atlas_repo_info_free(&l);

    wt_close(&e);
}

static void test_dirty_states_are_distinct(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    /* Dirty the linked worktree only. */
    T_OK(fx_write(atlas_buf_cstr(&e.linked_root), "shared.txt", "modified in the worktree\n", &err),
         &err);
    T_OK(fx_write(atlas_buf_cstr(&e.linked_root), "untracked-here.txt", "u\n", &err), &err);

    atlas_scan_summary ms;
    atlas_scan_summary ls;
    scan(&e, MAIN_NAME, &ms);
    scan(&e, LINKED_NAME, &ls);

    T_CHECK_MSG(!ms.dirty, "the main worktree should still be clean");
    T_CHECK_MSG(ls.dirty, "the linked worktree should be dirty");

    atlas_status_report mrep;
    atlas_status_report_init(&mrep);
    T_OK(atlas_service_status(e.ctx, MAIN_NAME, &mrep, &err), &err);
    T_CHECK(!mrep.live_state.dirty);
    T_EQ_INT(mrep.live_state.untracked, 0);
    atlas_status_report_free(&mrep);

    atlas_status_report lrep;
    atlas_status_report_init(&lrep);
    T_OK(atlas_service_status(e.ctx, LINKED_NAME, &lrep, &err), &err);
    T_CHECK(lrep.live_state.dirty);
    T_EQ_INT(lrep.live_state.untracked, 1);
    T_EQ_INT(lrep.live_state.unstaged, 1);
    atlas_status_report_free(&lrep);

    /* And each diff sees only its own worktree's changes. */
    atlas_diff_report mdiff;
    atlas_diff_report_init(&mdiff);
    T_OK(atlas_service_diff(e.ctx, MAIN_NAME, NULL, NULL, NULL, &mdiff, &err), &err);
    T_EQ_INT(mdiff.unstaged_count, 0);
    T_EQ_INT(mdiff.untracked_count, 0);
    atlas_diff_report_free(&mdiff);

    atlas_diff_report ldiff;
    atlas_diff_report_init(&ldiff);
    T_OK(atlas_service_diff(e.ctx, LINKED_NAME, NULL, NULL, NULL, &ldiff, &err), &err);
    T_EQ_INT(ldiff.unstaged_count, 1);
    T_EQ_INT(ldiff.untracked_count, 1);
    atlas_diff_report_free(&ldiff);

    wt_close(&e);
}

/* --- scanning one does not disturb the other ----------------------------- */

static void test_scan_isolation(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    T_OK(fx_write(atlas_buf_cstr(&e.linked_root), "feature.txt", "feature work\n", &err), &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&e.linked_root), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&e.linked_root), "feature commit", &err), &err);

    /* Scan both, then rescan one repeatedly. The other's rows must not move. */
    atlas_scan_summary ms;
    atlas_scan_summary ls;
    scan(&e, MAIN_NAME, &ms);
    scan(&e, LINKED_NAME, &ls);

    atlas_repo_info before;
    load(&e, MAIN_NAME, &before);
    int64_t before_scan_id = before.last_scan_id;
    char before_head[ATLAS_OID_HEX_MAX_INCL];
    (void)snprintf(before_head, sizeof(before_head), "%s", before.scanned_head);
    atlas_repo_counts before_counts;
    T_OK(atlas_db_repo_counts(atlas_ctx_db(e.ctx), before.id, &before_counts, &err), &err);
    atlas_repo_info_free(&before);

    for (int i = 0; i < 3; i++) {
        atlas_scan_summary again;
        scan(&e, LINKED_NAME, &again);
        /* Rescanning the linked worktree finds nothing new. */
        T_EQ_INT(again.files_added, 0);
        T_EQ_INT(again.files_modified, 0);
        T_EQ_INT(again.commits_ingested, 0);
    }

    atlas_repo_info after;
    load(&e, MAIN_NAME, &after);
    T_CHECK_MSG(after.last_scan_id == before_scan_id,
                "scanning the linked worktree changed the main worktree's scan id");
    T_EQ_STR(after.scanned_head, before_head);
    T_EQ_STR(after.current_branch, "main");
    atlas_repo_counts after_counts;
    T_OK(atlas_db_repo_counts(atlas_ctx_db(e.ctx), after.id, &after_counts, &err), &err);
    T_CHECK_MSG(after_counts.files_live == before_counts.files_live,
                "the main worktree's file count changed: %lld -> %lld",
                (long long)before_counts.files_live, (long long)after_counts.files_live);
    T_EQ_INT(after_counts.commits, before_counts.commits);
    atlas_repo_info_free(&after);

    /* The file that exists only in the linked worktree must not appear under the
     * main worktree's registration. */
    atlas_repo_info m;
    atlas_repo_info l;
    load(&e, MAIN_NAME, &m);
    load(&e, LINKED_NAME, &l);
    bool found = false;
    T_OK(atlas_db_file_get(atlas_ctx_db(e.ctx), m.id, "feature.txt", 11u, NULL, NULL, &found, &err),
         &err);
    T_CHECK_MSG(!found, "feature.txt belongs to the linked worktree only");
    found = false;
    T_OK(atlas_db_file_get(atlas_ctx_db(e.ctx), l.id, "feature.txt", 11u, NULL, NULL, &found, &err),
         &err);
    T_CHECK_MSG(found, "feature.txt should be indexed under the linked worktree");
    /* main-only.txt exists in both worktrees because it is committed on main and
     * the branch was created from it, so it is indexed under both. */
    atlas_repo_info_free(&m);
    atlas_repo_info_free(&l);

    wt_close(&e);
}

/* --- queries return the right worktree ----------------------------------- */

static void test_queries_are_per_worktree(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    /* Give the same path different content in each worktree. */
    T_OK(fx_write(atlas_buf_cstr(&e.linked_root), "shared.txt", "linked version\n", &err), &err);
    T_OK(fx_add_all(&e.fx, atlas_buf_cstr(&e.linked_root), &err), &err);
    T_OK(fx_commit(&e.fx, atlas_buf_cstr(&e.linked_root), "change shared in the worktree", &err),
         &err);

    atlas_scan_summary sum;
    scan(&e, MAIN_NAME, &sum);
    scan(&e, LINKED_NAME, &sum);

    /* Each registration reports its own working-tree content hash. */
    char main_hash[ATLAS_SHA256_HEX_LEN + 1u];
    char linked_hash[ATLAS_SHA256_HEX_LEN + 1u];
    atlas_sha256_hex("shared content\n", strlen("shared content\n"), main_hash);
    atlas_sha256_hex("linked version\n", strlen("linked version\n"), linked_hash);
    T_CHECK(strcmp(main_hash, linked_hash) != 0);

    atlas_repo_info m;
    atlas_repo_info l;
    load(&e, MAIN_NAME, &m);
    load(&e, LINKED_NAME, &l);

    atlas_buf got_main = ATLAS_BUF_INIT;
    atlas_buf got_linked = ATLAS_BUF_INIT;
    atlas_buf sql = ATLAS_BUF_INIT;
    for (int which = 0; which < 2; which++) {
        int64_t id = (which == 0) ? m.id : l.id;
        atlas_buf_reset(&sql);
        T_OK(atlas_buf_appendf(&sql, &err,
                               "SELECT count(*) FROM files WHERE repo_id=%lld"
                               " AND path_text='shared.txt' AND content_hash='%s';",
                               (long long)id, (which == 0) ? main_hash : linked_hash),
             &err);
        int64_t n = 0;
        T_OK(atlas_db_query_int64(atlas_ctx_db(e.ctx), atlas_buf_cstr(&sql), &n, &err), &err);
        T_CHECK_MSG(n == 1, "%s should hold its own version of shared.txt",
                    (which == 0) ? MAIN_NAME : LINKED_NAME);
    }
    atlas_buf_free(&got_main);
    atlas_buf_free(&got_linked);
    atlas_buf_free(&sql);

    /* History for the shared path differs: the linked worktree has one more
     * commit touching it. */
    int64_t main_hits = 0;
    int64_t linked_hits = 0;
    T_OK(atlas_service_history(e.ctx, MAIN_NAME, "shared.txt", 100, NULL, NULL, &main_hits, &err),
         &err);
    T_OK(atlas_service_history(e.ctx, LINKED_NAME, "shared.txt", 100, NULL, NULL, &linked_hits,
                               &err),
         &err);
    T_CHECK_MSG(linked_hits > main_hits,
                "the linked worktree should record more changes to shared.txt (%lld vs %lld)",
                (long long)linked_hits, (long long)main_hits);

    atlas_repo_info_free(&m);
    atlas_repo_info_free(&l);
    wt_close(&e);
}

/* --- removal is independent ---------------------------------------------- */

static void test_removal_is_independent(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    atlas_scan_summary sum;
    scan(&e, MAIN_NAME, &sum);
    scan(&e, LINKED_NAME, &sum);

    atlas_repo_info l;
    load(&e, LINKED_NAME, &l);
    atlas_repo_counts before;
    T_OK(atlas_db_repo_counts(atlas_ctx_db(e.ctx), l.id, &before, &err), &err);
    T_CHECK(before.files_live > 0);
    int64_t linked_id = l.id;
    atlas_repo_info_free(&l);

    /* Digest both worktrees so removal can be proven not to touch either. */
    char main_before[ATLAS_SHA256_HEX_LEN + 1u];
    char linked_before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(atlas_buf_cstr(&e.main_root), main_before, &err), &err);
    T_OK(fx_tree_digest(atlas_buf_cstr(&e.linked_root), linked_before, &err), &err);

    atlas_repo_info removed;
    atlas_repo_info_init(&removed);
    T_OK(atlas_service_repo_remove(e.ctx, MAIN_NAME, &removed, &err), &err);
    atlas_repo_info_free(&removed);

    /* The linked registration and all of its rows survive untouched. */
    atlas_repo_info still;
    load(&e, LINKED_NAME, &still);
    T_EQ_INT(still.id, linked_id);
    atlas_repo_counts after;
    T_OK(atlas_db_repo_counts(atlas_ctx_db(e.ctx), still.id, &after, &err), &err);
    T_CHECK_MSG(after.files_live == before.files_live,
                "removing one worktree changed the other's file rows: %lld -> %lld",
                (long long)before.files_live, (long long)after.files_live);
    T_EQ_INT(after.commits, before.commits);
    T_EQ_INT(after.evidence, before.evidence);
    /* With its sibling gone, the survivor reports no siblings. */
    atlas_status_report rep;
    atlas_status_report_init(&rep);
    T_OK(atlas_service_status(e.ctx, LINKED_NAME, &rep, &err), &err);
    T_EQ_INT(rep.sibling_worktrees, 0);
    atlas_status_report_free(&rep);
    atlas_repo_info_free(&still);

    /* And the removed registration really is gone. */
    atlas_repo_info gone;
    atlas_repo_info_init(&gone);
    bool found = true;
    T_OK(atlas_db_repo_get(atlas_ctx_db(e.ctx), MAIN_NAME, &gone, &found, &err), &err);
    T_CHECK(!found);
    atlas_repo_info_free(&gone);

    /* Neither target worktree was modified. */
    char main_after[ATLAS_SHA256_HEX_LEN + 1u];
    char linked_after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(atlas_buf_cstr(&e.main_root), main_after, &err), &err);
    T_OK(fx_tree_digest(atlas_buf_cstr(&e.linked_root), linked_after, &err), &err);
    T_CHECK_MSG(strcmp(main_before, main_after) == 0, "the main worktree was modified");
    T_CHECK_MSG(strcmp(linked_before, linked_after) == 0, "the linked worktree was modified");

    wt_close(&e);
}

/* --- both worktrees survive every read command --------------------------- */

static void test_both_worktrees_unchanged_by_reads(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    /* Leave both dirty, so an index refresh would be visible. */
    T_OK(fx_write(atlas_buf_cstr(&e.main_root), "shared.txt", "main edit\n", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&e.linked_root), "shared.txt", "linked edit\n", &err), &err);
    T_OK(fx_write(atlas_buf_cstr(&e.main_root), "untracked-main.txt", "u\n", &err), &err);

    char main_before[ATLAS_SHA256_HEX_LEN + 1u];
    char linked_before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(atlas_buf_cstr(&e.main_root), main_before, &err), &err);
    T_OK(fx_tree_digest(atlas_buf_cstr(&e.linked_root), linked_before, &err), &err);

    /* Every command, against both registrations. */
    const char *names[] = {MAIN_NAME, LINKED_NAME};
    for (size_t i = 0; i < 2u; i++) {
        atlas_scan_summary sum;
        scan(&e, names[i], &sum);

        atlas_status_report rep;
        atlas_status_report_init(&rep);
        T_OK(atlas_service_status(e.ctx, names[i], &rep, &err), &err);
        atlas_status_report_free(&rep);

        atlas_diff_report drep;
        atlas_diff_report_init(&drep);
        T_OK(atlas_service_diff(e.ctx, names[i], NULL, NULL, NULL, &drep, &err), &err);
        atlas_diff_report_free(&drep);

        int64_t count = 0;
        T_OK(atlas_service_search(e.ctx, names[i], "shared", 50, NULL, NULL, NULL, &count, &err),
             &err);
        T_OK(atlas_service_history(e.ctx, names[i], "shared.txt", 50, NULL, NULL, &count, &err),
             &err);
        T_OK(atlas_service_file(e.ctx, names[i], "shared.txt", NULL, NULL, &err), &err);

        char main_after[ATLAS_SHA256_HEX_LEN + 1u];
        char linked_after[ATLAS_SHA256_HEX_LEN + 1u];
        T_OK(fx_tree_digest(atlas_buf_cstr(&e.main_root), main_after, &err), &err);
        T_OK(fx_tree_digest(atlas_buf_cstr(&e.linked_root), linked_after, &err), &err);
        T_CHECK_MSG(strcmp(main_before, main_after) == 0,
                    "commands against %s modified the main worktree", names[i]);
        T_CHECK_MSG(strcmp(linked_before, linked_after) == 0,
                    "commands against %s modified the linked worktree", names[i]);
    }

    wt_close(&e);
}

/* --- schema representation ---------------------------------------------- */

static void test_schema_records_worktree_identity(void) {
    wt_env e;
    atlas_err err;
    atlas_err_init(&err);
    wt_open(&e);
    if (!wt_build(&e)) {
        wt_close(&e);
        return;
    }
    wt_register_both(&e);

    /* The migration that introduced worktree identity is applied. */
    T_EQ_INT(atlas_db_schema_version(atlas_ctx_db(e.ctx), &err), ATLAS_SCHEMA_VERSION);
    T_CHECK_MSG(ATLAS_SCHEMA_VERSION >= 2, "worktree identity arrived in schema version 2");

    /* Two rows, one shared common dir, two distinct git dirs, exactly one linked. */
    int64_t v = 0;
    T_OK(atlas_db_query_int64(atlas_ctx_db(e.ctx),
                              "SELECT count(DISTINCT git_common_dir) FROM repositories;", &v, &err),
         &err);
    T_EQ_INT(v, 1);
    T_OK(atlas_db_query_int64(atlas_ctx_db(e.ctx),
                              "SELECT count(DISTINCT git_dir) FROM repositories;", &v, &err),
         &err);
    T_EQ_INT(v, 2);
    T_OK(atlas_db_query_int64(atlas_ctx_db(e.ctx),
                              "SELECT count(*) FROM repositories WHERE is_linked_worktree=1;", &v,
                              &err),
         &err);
    T_EQ_INT(v, 1);
    T_OK(atlas_db_query_int64(atlas_ctx_db(e.ctx),
                              "SELECT count(*) FROM repositories WHERE git_dir IS NULL;", &v, &err),
         &err);
    T_EQ_INT(v, 0);

    wt_close(&e);
}

static const atlas_test TESTS[] = {
    {"worktrees have distinct identity and a shared object store", test_distinct_identity},
    {"sibling worktrees are discoverable", test_siblings_are_discoverable},
    {"heads and branches stay distinct", test_heads_and_branches_are_distinct},
    {"dirty states stay distinct", test_dirty_states_are_distinct},
    {"scanning one worktree does not disturb the other", test_scan_isolation},
    {"file and history queries are per worktree", test_queries_are_per_worktree},
    {"removing one registration leaves the other intact", test_removal_is_independent},
    {"both worktrees are unchanged by every read", test_both_worktrees_unchanged_by_reads},
    {"the schema records worktree identity", test_schema_records_worktree_identity},
};

ATLAS_TEST_MAIN("worktree", TESTS)

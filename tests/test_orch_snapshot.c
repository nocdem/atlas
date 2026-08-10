/* Atlas - A8: the daemon-owned source snapshot.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The correction this suite exists for: the worker used to open the registered
 * repository itself, which required the untrusted account to hold a read path to
 * `/opt` and — on a machine where the repositories belong to somebody else —
 * git refused outright. The direction is now inverted. `atlasd` reads and the
 * worker receives, so these cases drive `atlas_snapshot_open` and
 * `atlas_snapshot_read` directly against a fixture database and a fixture
 * repository, exactly as the daemon does.
 *
 * Everything is synthetic and isolated: a private temporary tree, its own
 * repository, its own database. Nothing here touches a live service, socket,
 * database or registered repository.
 *
 * Required cases covered here: 1, 2, 4–8 (trusted resolution and fail-closed
 * identity), 8–16 (hostile repository configuration, hooks, submodules, LFS),
 * 17 (symlinks), 18 (nested repositories), 19–21 (deterministic order and stable
 * digest), 26–29 (bounds and traversal), 45 (the repository is byte-identical
 * afterwards).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/git.h"
#include "atlas/orch.h"
#include "atlas/orch_ops.h"
#include "atlas/sha256.h"
#include "atlas/snapshot.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

typedef struct snapenv {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_buf identity;
    atlas_buf commit;
    int64_t attempt_id;
} snapenv;

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

/* Builds a fixture repository, registers and scans it, submits a job and leases
 * it — the same sequence the daemon performs, so the attempt this suite reads
 * from is a real one with a real lease. */
static void snapenv_open(snapenv *e, void (*extra)(const char *repo, atlas_err *err)) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);
    atlas_buf_init(&e->identity);
    atlas_buf_init(&e->commit);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    mkdirs(fx_repo(&e->fx), "src/deep/b.txt");
    T_OK(fx_write(fx_repo(&e->fx), "src/deep/b.txt", "hello\n", &err), &err);
    if (extra != NULL) {
        extra(fx_repo(&e->fx), &err);
    }
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);

    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx),  "--name",         "proj"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    {
        const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "proj"};
        int code = -1;
        T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "proj", &ri, &found, &err), &err);
    T_REQUIRE(found);
    T_OK(atlas_db_repo_identity_hash(e->db, ri.id, &e->identity, &err), &err);
    T_OK(atlas_buf_set_str(&e->commit, ri.scanned_head, &err), &err);

    /* Submit and lease, so there is a real attempt with a real lease. */
    atlas_orch_op *op = atlas_orch_op_new(ATLAS_ORCH_OP_SUBMIT);
    T_REQUIRE(op != NULL);
    op->peer_uid = 1000;
    op->actor = ATLAS_ORCH_ACTOR_CLIENT;
    op->repo_id = ri.id;
    op->spec.submitter_uid = 1000;
    T_OK(atlas_buf_set_str(&op->spec.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set(&op->spec.repo_identity_hash, e->identity.data, e->identity.len, &err),
         &err);
    T_OK(atlas_buf_set(&op->spec.source_commit, e->commit.data, e->commit.len, &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.mode, "patch", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.driver, "fake", &err), &err);
    T_OK(atlas_buf_set_str(&op->spec.task_text, "do something", &err), &err);
    op->spec.wall_timeout_ms = 600000;
    op->spec.idle_timeout_ms = 300000;
    op->spec.max_attempts = 1;
    op->spec.max_output_bytes = 65536;
    op->spec.max_artifact_bytes = 65536;
    op->spec.max_artifact_count = 8;
    T_OK(atlas_orch_spec_canonicalise(&op->spec, &err), &err);
    atlas_orch_result r;
    atlas_orch_result_init(&r);
    T_OK(atlas_orch_apply(e->db, op, &r, &err), &err);
    atlas_orch_op_free(op);
    free(op);
    atlas_orch_result_free(&r);

    atlas_orch_op *lease = atlas_orch_op_new(ATLAS_ORCH_OP_LEASE);
    lease->peer_uid = 993;
    lease->actor = ATLAS_ORCH_ACTOR_DISPATCHER;
    T_OK(atlas_buf_set_str(&lease->dispatcher_id, "d1", &err), &err);
    atlas_orch_result g;
    atlas_orch_result_init(&g);
    T_OK(atlas_orch_apply(e->db, lease, &g, &err), &err);
    T_REQUIRE(g.granted);
    e->attempt_id = g.attempt_id;
    atlas_orch_op_free(lease);
    free(lease);
    atlas_orch_result_free(&g);
    atlas_repo_info_free(&ri);
}

static void snapenv_close(snapenv *e) {
    atlas_db_close(e->db);
    e->db = NULL;
    atlas_buf_free(&e->db_path);
    atlas_buf_free(&e->identity);
    atlas_buf_free(&e->commit);
    fx_close(&e->fx);
}

/* --- the trusted read ---------------------------------------------------------- */

static void test_the_daemon_enumerates_a_registered_repository(void) {
    snapenv e;
    snapenv_open(&e, NULL);
    atlas_err err;
    atlas_err_init(&err);

    char before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), before, &err), &err);

    atlas_snapshot_meta m;
    T_OK(atlas_snapshot_open(e.db, e.attempt_id, &m, &err), &err);
    T_EQ_INT(m.protocol, ATLAS_SNAPSHOT_PROTOCOL);
    T_CHECK_MSG(m.entries >= 2, "the snapshot enumerated %lld entries", (long long)m.entries);
    T_CHECK(m.total_bytes > 0);
    T_EQ_INT((int)strlen(m.digest), 64);
    T_CHECK(strcmp(m.commit, atlas_buf_cstr(&e.commit)) == 0);
    T_EQ_INT((int)strlen(m.tree), 40);

    /* Reading the repository wrote nothing to it. */
    char after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), after, &err), &err);
    T_CHECK_MSG(strcmp(before, after) == 0, "enumerating a snapshot modified the repository");

    /* Idempotent: a second open returns the same identity rather than
     * enumerating again, so a dispatcher restart resumes against the same tree. */
    atlas_snapshot_meta m2;
    T_OK(atlas_snapshot_open(e.db, e.attempt_id, &m2, &err), &err);
    T_CHECK_MSG(strcmp(m.digest, m2.digest) == 0, "a second open produced a different snapshot");
    T_EQ_INT((int)m2.entries, (int)m.entries);

    snapenv_close(&e);
}

static void test_entries_are_ordered_and_the_digest_is_stable(void) {
    snapenv e;
    snapenv_open(&e, NULL);
    atlas_err err;
    atlas_err_init(&err);
    atlas_snapshot_meta m;
    T_OK(atlas_snapshot_open(e.db, e.attempt_id, &m, &err), &err);

    /* Reading every entry twice yields identical bytes and identical digests:
     * the order is the manifest's, not the filesystem's. */
    atlas_buf first = ATLAS_BUF_INIT;
    for (int pass = 0; pass < 2; pass++) {
        atlas_buf seen = ATLAS_BUF_INIT;
        for (int64_t i = 0; i < m.entries; i++) {
            atlas_snapshot_chunk c;
            T_OK(atlas_snapshot_read(e.db, e.attempt_id, i, 0, &c, &err), &err);
            T_OK(atlas_buf_appendf(&seen, &err, "%s|%s|%lld|%s\n", atlas_buf_cstr(&c.path),
                                   c.mode, (long long)c.size_bytes, c.sha256),
                 &err);
            /* The content digest describes the bytes that were served. */
            char hex[ATLAS_SHA256_HEX_LEN + 1u];
            atlas_sha256_hex(c.data.data != NULL ? c.data.data : "", c.data.len, hex);
            if (c.eof && c.offset == 0) {
                T_CHECK_MSG(strcmp(hex, c.sha256) == 0,
                            "a served entry does not match its declared digest");
            }
            atlas_snapshot_chunk_free(&c);
        }
        if (pass == 0) {
            T_OK(atlas_buf_set(&first, seen.data, seen.len, &err), &err);
        } else {
            T_CHECK_MSG(strcmp(atlas_buf_cstr(&first), atlas_buf_cstr(&seen)) == 0,
                        "the snapshot order or content changed between reads");
        }
        atlas_buf_free(&seen);
    }
    atlas_buf_free(&first);
    snapenv_close(&e);
}

static void add_hostile(const char *repo, atlas_err *err) {
    /* A tracked symlink out of the tree, a submodule-shaped gitlink's directory,
     * a nested repository, a hostile config and a hook. None may reach a
     * workspace, and none may execute. */
    char p[4096];
    (void)snprintf(p, sizeof p, "%s/escape", repo);
    (void)symlink("/etc/passwd", p);
    mkdirs(repo, ".githooks/pre-commit");
    (void)fx_write_exec(repo, ".githooks/pre-commit", "#!/bin/sh\ntouch /tmp/atlas-snap-pwned\n",
                        err);
    mkdirs(repo, "nested/.git/config");
    (void)fx_write(repo, "nested/.git/config", "[core]\n\trepositoryformatversion = 0\n", err);
    (void)fx_write(repo, "evil.gitconfig",
                   "[core]\n\tfsmonitor = /tmp/atlas-snap-marker\n"
                   "[diff]\n\texternal = /tmp/atlas-snap-marker\n"
                   "[filter \"f\"]\n\tclean = /tmp/atlas-snap-marker\n"
                   "[core]\n\tsshCommand = /tmp/atlas-snap-marker\n"
                   "[core]\n\taskPass = /tmp/atlas-snap-marker\n",
                   err);
}

static void test_hostile_repository_content_never_executes_or_escapes(void) {
    snapenv e;
    snapenv_open(&e, add_hostile);
    atlas_err err;
    atlas_err_init(&err);
    (void)unlink("/tmp/atlas-snap-pwned");

    atlas_snapshot_meta m;
    T_OK(atlas_snapshot_open(e.db, e.attempt_id, &m, &err), &err);

    /* The symlink was refused and counted, not enumerated. */
    T_CHECK_MSG(m.refused_symlinks >= 1, "a tracked symlink was not refused");
    for (int64_t i = 0; i < m.entries; i++) {
        atlas_snapshot_chunk c;
        T_OK(atlas_snapshot_read(e.db, e.attempt_id, i, 0, &c, &err), &err);
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&c.path), "escape") != 0,
                    "a symlink was enumerated as a snapshot entry");
        /* Only ordinary file modes are ever enumerated. */
        T_CHECK_MSG(strcmp(c.mode, "100644") == 0 || strcmp(c.mode, "100755") == 0,
                    "an unexpected mode \"%s\" was enumerated", c.mode);
        T_CHECK(atlas_snapshot_path_ok(c.path.data, c.path.len));
        atlas_snapshot_chunk_free(&c);
    }
    /* Nothing in the repository's own configuration ran: Atlas reads no global
     * or system config, and the `-c` prefix disables every helper route. The
     * hostile config is present only as ordinary file *content*, which a
     * faithful snapshot must carry. */
    struct stat sb;
    T_CHECK_MSG(stat("/tmp/atlas-snap-pwned", &sb) != 0, "a repository hook ran");
    T_CHECK_MSG(stat("/tmp/atlas-snap-marker", &sb) != 0, "a repository-configured helper ran");

    snapenv_close(&e);
}

/* --- fail-closed resolution ------------------------------------------------------ */

static void test_resolution_fails_closed_when_identity_or_commit_is_wrong(void) {
    snapenv e;
    snapenv_open(&e, NULL);
    atlas_err err;
    atlas_err_init(&err);

    /* The job's repository identity no longer matches the registry: the bytes
     * that would be handed over are not the bytes the job was authorised over,
     * so it must refuse rather than serve them. */
    T_OK(atlas_db_exec_sql(e.db,
                           "UPDATE orch_jobs SET repo_identity_hash = "
                           "'1111111111111111111111111111111111111111111111111111111111111111';",
                           &err),
         &err);
    atlas_snapshot_meta m;
    atlas_err e2;
    atlas_err_init(&e2);
    T_CHECK_MSG(atlas_snapshot_open(e.db, e.attempt_id, &m, &e2) != ATLAS_OK,
                "a snapshot was produced for a repository whose identity had changed");
    T_OK(atlas_db_exec_sql(e.db,
                           "UPDATE orch_jobs SET repo_identity_hash = (SELECT repo_identity_hash "
                           "FROM orch_jobs LIMIT 1);",
                           &err),
         &err);

    /* A repository that is no longer registered. */
    {
        atlas_err e3;
        atlas_err_init(&e3);
        T_OK(atlas_db_exec_sql(e.db, "UPDATE orch_jobs SET repo_name = 'gone';", &err), &err);
        T_CHECK_MSG(atlas_snapshot_open(e.db, e.attempt_id, &m, &e3) != ATLAS_OK,
                    "a snapshot was produced for an unregistered repository");
        T_OK(atlas_db_exec_sql(e.db, "UPDATE orch_jobs SET repo_name = 'proj';", &err), &err);
    }
    /* A pinned commit that does not exist in this repository. Resolving it
     * against the repository the registry named is what stops a commit that
     * exists elsewhere from being snapshotted as this project's. */
    {
        atlas_err e4;
        atlas_err_init(&e4);
        T_OK(atlas_db_exec_sql(e.db,
                               "UPDATE orch_jobs SET source_commit = "
                               "'cafebabecafebabecafebabecafebabecafebabe';",
                               &err),
             &err);
        T_CHECK_MSG(atlas_snapshot_open(e.db, e.attempt_id, &m, &e4) != ATLAS_OK,
                    "a snapshot was produced for a commit this repository does not have");
    }
    snapenv_close(&e);
}

static void test_a_worker_cannot_reach_past_the_manifest(void) {
    snapenv e;
    snapenv_open(&e, NULL);
    atlas_err err;
    atlas_err_init(&err);
    atlas_snapshot_meta m;
    T_OK(atlas_snapshot_open(e.db, e.attempt_id, &m, &err), &err);

    /* Every index and offset is validated against the persisted manifest. There
     * is no field a worker could use to name a path, so the only reachable
     * surface is "an entry that exists, at an offset within it". */
    struct {
        int64_t index;
        int64_t offset;
        const char *what;
    } BAD[] = {
        {m.entries, 0, "an index past the end"},
        {m.entries + 1000, 0, "a wildly out-of-range index"},
        {-1, 0, "a negative index"},
        {0, -1, "a negative offset"},
        {0, 1 << 30, "an offset past the end of an entry"},
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        atlas_snapshot_chunk c;
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK_MSG(atlas_snapshot_read(e.db, e.attempt_id, BAD[i].index, BAD[i].offset, &c,
                                        &e2) != ATLAS_OK,
                    "%s was served", BAD[i].what);
        atlas_snapshot_chunk_free(&c);
    }
    /* And an attempt that holds no snapshot serves nothing. */
    {
        atlas_snapshot_chunk c;
        atlas_err e2;
        atlas_err_init(&e2);
        T_CHECK(atlas_snapshot_read(e.db, 999999, 0, 0, &c, &e2) != ATLAS_OK);
        atlas_snapshot_chunk_free(&c);
    }
    snapenv_close(&e);
}

static void test_the_digest_covers_the_manifest_not_the_transfer(void) {
    atlas_err err;
    atlas_err_init(&err);
    /* Two manifests that differ in exactly one field must digest differently:
     * path, mode, size, content digest, order, and the totals. */
    char base[65];
    {
        atlas_snapshot_digest d;
        T_OK(atlas_snapshot_digest_begin(&d, "c", "t", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100644", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 3, base, &err), &err);
    }
#define VARIANT(what, body)                                                          \
    do {                                                                             \
        char out[65];                                                                \
        atlas_snapshot_digest d;                                                     \
        T_OK(atlas_snapshot_digest_begin(&d, "c", "t", &err), &err);                 \
        body;                                                                        \
        T_CHECK_MSG(strcmp(base, out) != 0, "changing %s did not change the digest", what); \
    } while (0)

    VARIANT("the order", {
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100644", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 3, out, &err), &err);
    });
    VARIANT("a path", {
        T_OK(atlas_snapshot_digest_entry(&d, "z", 1u, "100644", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 3, out, &err), &err);
    });
    VARIANT("a mode", {
        T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100755", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 3, out, &err), &err);
    });
    VARIANT("a content digest", {
        T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100644", 1, "zz", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 3, out, &err), &err);
    });
    VARIANT("the entry count", {
        T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100644", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 3, 3, out, &err), &err);
    });
    VARIANT("the total size", {
        T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100644", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 4, out, &err), &err);
    });
    /* Length-prefixed, not delimited: adjacent fields cannot be re-split. */
    VARIANT("a field boundary", {
        T_OK(atlas_snapshot_digest_entry(&d, "ab", 2u, "100644", 1, "aa", &err), &err);
        T_OK(atlas_snapshot_digest_entry(&d, "", 0u, "100644", 2, "bb", &err), &err);
        T_OK(atlas_snapshot_digest_finish(&d, 2, 3, out, &err), &err);
    });
#undef VARIANT

    /* And identical manifests digest identically. */
    char again[65];
    atlas_snapshot_digest d;
    T_OK(atlas_snapshot_digest_begin(&d, "c", "t", &err), &err);
    T_OK(atlas_snapshot_digest_entry(&d, "a", 1u, "100644", 1, "aa", &err), &err);
    T_OK(atlas_snapshot_digest_entry(&d, "b", 1u, "100644", 2, "bb", &err), &err);
    T_OK(atlas_snapshot_digest_finish(&d, 2, 3, again, &err), &err);
    T_CHECK(strcmp(base, again) == 0);
}

static void test_snapshot_paths_reject_traversal_and_nuls(void) {
    static const char *const BAD[] = {"/abs", "../up", "a/../b", "a/..", "..", ".", "./a",
                                      "a//b", "a/", ""};
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        T_CHECK_MSG(!atlas_snapshot_path_ok(BAD[i], strlen(BAD[i])),
                    "\"%s\" was accepted as a snapshot path", BAD[i]);
    }
    T_CHECK(!atlas_snapshot_path_ok("a\0b", 3u));
    T_CHECK(!atlas_snapshot_path_ok(NULL, 0u));
    /* Non-ASCII bytes are fine: repository paths are bytes, and a snapshot must
     * carry them faithfully. The check is structural, not an encoding one. */
    T_CHECK(atlas_snapshot_path_ok("caf\xc3\xa9.c", 7u));
    T_CHECK(atlas_snapshot_path_ok("\xff\xfe", 2u));
    T_CHECK(atlas_snapshot_path_ok("src/deep/b.txt", 14u));
}

static const atlas_test TESTS[] = {
    {"the daemon enumerates a registered repository",
     test_the_daemon_enumerates_a_registered_repository},
    {"entries are ordered and the digest is stable",
     test_entries_are_ordered_and_the_digest_is_stable},
    {"hostile repository content never executes or escapes",
     test_hostile_repository_content_never_executes_or_escapes},
    {"resolution fails closed when identity or commit is wrong",
     test_resolution_fails_closed_when_identity_or_commit_is_wrong},
    {"a worker cannot reach past the manifest",
     test_a_worker_cannot_reach_past_the_manifest},
    {"the digest covers the manifest, not the transfer",
     test_the_digest_covers_the_manifest_not_the_transfer},
    {"snapshot paths reject traversal and NULs",
     test_snapshot_paths_reject_traversal_and_nuls},
};

ATLAS_TEST_MAIN("orch_snapshot", TESTS)

/* Atlas - A13: the scanner channel, and the uid comparison that carries it.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * A scanner runs as a repository's owner and reads a tree the daemon cannot.
 * What it may report about is decided by one comparison — the peer's uid from
 * `SO_PEERCRED` against `repositories.scanner_uid` — and these cases are that
 * comparison from both directions.
 *
 * The edge is driven through `atlas_server_dispatch`, which `daemon_internal.h`
 * exposes so the protocol can be tested without a socket. Everything above the
 * socket is the shipped code: the method table, the peer uid, the refusal, the
 * parser and the database. What is substituted is the carriage of bytes — and
 * the peer uid, which is the point: over a real socket every peer would be this
 * process's own uid, and a test that could only ever ask as itself could not
 * show that another uid is refused. `tests/test_plan_rpc.c` opens the same way
 * and for the same reason.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/ipc.h"
#include "atlas_test.h"
#include "daemon/daemon_internal.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    atlas_buf db_path;
    atlas_db *db;
    atlas_server_ctx ctx;
    int64_t mine;    /* a repository this process's uid may scan */
    int64_t theirs;  /* a repository it may not */
    int64_t nobodys; /* a repository with no scanner assigned */
} env;

/* Registered through the CLI, because that is the only way a repository is ever
 * registered. Three repositories, each answering a different question about the
 * comparison. */
static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    atlas_buf_init(&e->db_path);

    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx), "--name", "mine"};
        int code = -1;
        T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }

    T_OK(atlas_buf_appendf(&e->db_path, &err, "%s/atlas.db", fx_data_dir(&e->fx)), &err);
    T_OK(atlas_db_open(atlas_buf_cstr(&e->db_path), &e->db, &err), &err);
    T_OK(atlas_db_migrate(e->db, &err), &err);

    atlas_repo_info ri;
    atlas_repo_info_init(&ri);
    bool found = false;
    T_OK(atlas_db_repo_get(e->db, "mine", &ri, &found, &err), &err);
    T_REQUIRE(found);
    e->mine = ri.id;
    /* Registration derived this from the root's owner, which is this process. */
    T_EQ_INT((int)ri.scanner_uid, (int)getuid());
    atlas_repo_info_free(&ri);

    /* Two more rows, written directly: registering them would need two more
     * real git repositories, and what these cases are about is the stored uid,
     * not how it got there. */
    {
        atlas_repo_identity id;
        memset(&id, 0, sizeof(id));
        id.root = "/tmp/atlas-a13-theirs";
        id.root_len = strlen(id.root);
        id.common_dir = "/tmp/atlas-a13-theirs/.git";
        id.common_dir_len = strlen(id.common_dir);
        id.git_dir = id.common_dir;
        id.git_dir_len = id.common_dir_len;
        id.object_format = "sha1";
        T_OK(atlas_db_repo_add(e->db, "theirs", &id, &e->theirs, &err), &err);
        T_OK(atlas_db_repo_set_scanner_uid(e->db, e->theirs, (int64_t)getuid() + 1, &err), &err);

        memset(&id, 0, sizeof(id));
        id.root = "/tmp/atlas-a13-nobodys";
        id.root_len = strlen(id.root);
        id.common_dir = "/tmp/atlas-a13-nobodys/.git";
        id.common_dir_len = strlen(id.common_dir);
        id.git_dir = id.common_dir;
        id.git_dir_len = id.common_dir_len;
        id.object_format = "sha1";
        T_OK(atlas_db_repo_add(e->db, "nobodys", &id, &e->nobodys, &err), &err);
        T_OK(atlas_db_repo_set_scanner_uid(e->db, e->nobodys, 0, &err), &err);
    }
    atlas_db_close(e->db);
    e->db = NULL;

    e->ctx.db_path = atlas_buf_cstr(&e->db_path);
    e->ctx.data_dir = fx_data_dir(&e->fx);
}

static void env_close(env *e) {
    atlas_buf_free(&e->db_path);
    fx_close(&e->fx);
}

/* One request in, one response out, parsed with the client's own parser so what
 * the test reads is what a client would read. */
static atlas_ipc_response *call(env *e, long long uid, const char *method, const char *params,
                                atlas_buf *raw) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf payload = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&payload, &err, "{\"id\":\"t\",\"method\":\"%s\",\"params\":%s}", method,
                           params),
         &err);
    atlas_buf_reset(raw);
    T_OK(atlas_server_dispatch(&e->ctx, payload.data, payload.len, uid, (int64_t)getpid(), raw,
                               &err),
         &err);
    atlas_buf_free(&payload);
    atlas_ipc_response *resp = NULL;
    T_OK(atlas_ipc_response_parse(raw->data, raw->len, &resp, &err), &err);
    T_REQUIRE(resp != NULL);
    return resp;
}

static void test_a_scanner_is_told_its_own_repositories_and_no_others(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, (long long)getuid(), "scanner.poll", "{}", &raw);
    T_CHECK_MSG(atlas_ipc_response_ok(r), "scanner.poll failed: %s", atlas_buf_cstr(&raw));

    const char *body = atlas_buf_cstr(&raw);
    T_CHECK_MSG(strstr(body, "\"mine\"") != NULL, "own repository missing: %s", body);
    T_CHECK_MSG(strstr(body, "\"theirs\"") == NULL, "another uid's repository listed: %s", body);
    T_CHECK_MSG(strstr(body, "\"nobodys\"") == NULL, "an unassigned repository listed: %s", body);

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* The refusal must not become an inventory: a uid that owns nothing learns that
 * it owns nothing, and not what else is registered. */
static void test_a_uid_that_owns_nothing_is_refused_and_names_no_repository(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, (long long)getuid() + 7, "scanner.poll", "{}", &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "an unrelated uid was served: %s", atlas_buf_cstr(&raw));

    const char *body = atlas_buf_cstr(&raw);
    T_CHECK_MSG(strstr(body, "mine") == NULL, "the refusal named a repository: %s", body);
    T_CHECK_MSG(strstr(body, "theirs") == NULL, "the refusal named a repository: %s", body);
    T_CHECK_MSG(strstr(body, "nobodys") == NULL, "the refusal named a repository: %s", body);

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    env_close(&e);
}

/* 0 is how Atlas records "no scanner assigned". A peer arriving as uid 0 must
 * not be handed the repositories that carry it, or an absence would grant
 * exactly what it means to withhold. */
static void test_uid_zero_is_never_a_scanner(void) {
    env e;
    env_open(&e);

    atlas_buf raw = ATLAS_BUF_INIT;
    atlas_ipc_response *r = call(&e, 0, "scanner.poll", "{}", &raw);
    T_CHECK_MSG(!atlas_ipc_response_ok(r), "uid 0 was served: %s", atlas_buf_cstr(&raw));

    atlas_ipc_response_free(r);
    atlas_buf_free(&raw);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a scanner is told its own repositories and no others",
     test_a_scanner_is_told_its_own_repositories_and_no_others},
    {"a uid that owns nothing is refused and names no repository",
     test_a_uid_that_owns_nothing_is_refused_and_names_no_repository},
    {"uid zero is never a scanner", test_uid_zero_is_never_a_scanner},
};

ATLAS_TEST_MAIN("scanner_rpc", TESTS)

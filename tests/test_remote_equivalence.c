/* Atlas - the daemon-served reads answer exactly what the local reads answer.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Under A7.1 a client uid cannot open the index, so every read-only command is
 * answered over the socket. That is only safe if the two paths agree — and the
 * places they would most easily disagree are the ones nobody looks at: an empty
 * result, a path that is not indexed, a symbol that does not exist, a decision
 * uid that was never issued, a file with no history, a deleted file.
 *
 * So this suite runs both against **one fixture database**: the local service
 * function over an `atlas_ctx`, and its `_remote` twin over the socket of a
 * daemon serving that same database. Anything the parsers drop, mistype or
 * silently default shows up here as a disagreement rather than as a wrong
 * answer in production.
 *
 * The `_remote` functions do not test for foreignness — that decision belongs
 * to the CLI — so pointing them at a fixture daemon is exactly what they do in
 * a system deployment, with a different socket. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atlas/service.h"
#include "atlas_test.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    fx_daemon d;
    atlas_ctx *ctx;
} env;

static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "a.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "gone.c", "int gone(void){return 1;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    /* A deleted file: indexed, then removed and committed, so the row survives
     * with `deleted` set. Both paths must say the same thing about it. */
    T_OK(fx_remove(fx_repo(&e->fx), "gone.c", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "remove gone.c", &err), &err);
    {
        const char *rescan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "proj"};
        (void)rescan;
    }

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

    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &err), &err);
    /* The remote functions resolve the socket from the environment, exactly as
     * a client does; pointing this process at the fixture's runtime directory
     * is what makes them talk to the fixture daemon rather than to a real one. */
    T_REQUIRE(setenv("XDG_RUNTIME_DIR", atlas_buf_cstr(&e->d.runtime_dir), 1) == 0);
    /* And an explicit data directory, which outranks a root-owned system
     * policy. Without it this process would resolve the *deployed* index, decide
     * it is in system scope, and put its questions to the live daemon — which is
     * the door `tests/support/fixture.c` describes and closes for children, and
     * which this suite has to close for itself because it is a client too. */
    T_REQUIRE(setenv("ATLAS_DATA_DIR", fx_data_dir(&e->fx), 1) == 0);

    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.data_dir_override = fx_data_dir(&e->fx);
    opts.mode = ATLAS_CTX_READ; /* the daemon owns the writer */
    T_OK(atlas_ctx_open(&opts, &e->ctx, &err), &err);
}

static void env_close(env *e) {
    atlas_ctx_close(e->ctx);
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    fx_close(&e->fx);
}

/* --- collectors ------------------------------------------------------------- */

typedef struct sink {
    atlas_buf text;
    int64_t rows;
} sink;

static void sink_init(sink *s) {
    atlas_buf_init(&s->text);
    s->rows = 0;
}
static void sink_free(sink *s) { atlas_buf_free(&s->text); }

static atlas_status on_file(const atlas_file_report *rep, void *ud, atlas_err *err) {
    sink *s = (sink *)ud;
    s->rows++;
    return atlas_buf_appendf(&s->text, err, "path=%s type=%s deleted=%d tracked=%d hash=%s "
                                            "changes=%lld last=%s size=%lld\n",
                             rep->row.path_text != NULL ? rep->row.path_text : "",
                             rep->row.file_type != NULL ? rep->row.file_type : "",
                             rep->row.deleted ? 1 : 0, rep->row.tracked ? 1 : 0,
                             rep->row.content_hash != NULL ? rep->row.content_hash : "",
                             (long long)rep->change_count,
                             rep->last_commit_oid != NULL ? rep->last_commit_oid : "",
                             (long long)rep->row.size_bytes);
}

static atlas_status on_history(const atlas_history_row *row, void *ud, atlas_err *err) {
    sink *s = (sink *)ud;
    s->rows++;
    return atlas_buf_appendf(&s->text, err, "%s %s %s %lld\n", row->commit_oid,
                             row->change_type != NULL ? row->change_type : "",
                             row->path_text != NULL ? row->path_text : "",
                             (long long)row->author_time);
}

static atlas_status on_hit(const atlas_search_hit *h, void *ud, atlas_err *err) {
    sink *s = (sink *)ud;
    s->rows++;
    return atlas_buf_appendf(&s->text, err, "%s %s %s %d\n", h->kind,
                             h->path_text != NULL ? h->path_text : "",
                             h->commit_oid != NULL ? h->commit_oid : "", h->deleted ? 1 : 0);
}

static atlas_status on_symbol(const atlas_code_symbol_row *row, void *ud, atlas_err *err) {
    sink *s = (sink *)ud;
    s->rows++;
    return atlas_buf_appendf(&s->text, err, "%s %s %s %lld\n",
                             row->name_text != NULL ? row->name_text : "",
                             row->kind != NULL ? row->kind : "",
                             row->path_text != NULL ? row->path_text : "", (long long)row->line);
}

/* --- the comparisons -------------------------------------------------------- */

#define SAME(what, a, b)                                                                        \
    T_CHECK_MSG(strcmp(atlas_buf_cstr(&(a).text), atlas_buf_cstr(&(b).text)) == 0,               \
                "%s: local and remote disagree\n  local : %s\n  remote: %s", what,              \
                atlas_buf_cstr(&(a).text), atlas_buf_cstr(&(b).text));                          \
    T_CHECK_MSG((a).rows == (b).rows, "%s: local %lld rows, remote %lld", what,                 \
                (long long)(a).rows, (long long)(b).rows)

static void pair_file(env *e, const char *path, const char *what) {
    atlas_err le, re;
    atlas_err_init(&le);
    atlas_err_init(&re);
    sink l, r;
    sink_init(&l);
    sink_init(&r);
    atlas_status ls = atlas_service_file(e->ctx, "proj", path, on_file, &l, &le);
    atlas_status rs = atlas_service_file_remote("proj", path, on_file, &r, &re);
    T_CHECK_MSG(ls == rs, "%s: local status %d, remote %d (%s / %s)", what, (int)ls, (int)rs,
                atlas_err_msg(&le), atlas_err_msg(&re));
    SAME(what, l, r);
    sink_free(&l);
    sink_free(&r);
}

static void test_file_present_deleted_and_missing(void) {
    env e;
    env_open(&e);
    pair_file(&e, "a.c", "file: an indexed path");
    pair_file(&e, "gone.c", "file: a deleted path");
    pair_file(&e, "no/such/file.c", "file: a path that is not indexed");
    env_close(&e);
}

static void test_history_including_a_path_with_none(void) {
    env e;
    env_open(&e);
    const char *paths[] = {"a.c", "gone.c", "never-existed.c"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        atlas_err le, re;
        atlas_err_init(&le);
        atlas_err_init(&re);
        sink l, r;
        sink_init(&l);
        sink_init(&r);
        int64_t lc = 0, rc = 0;
        atlas_status ls = atlas_service_history(e.ctx, "proj", paths[i], 50, on_history, &l, &lc,
                                                &le);
        atlas_status rs = atlas_service_history_remote("proj", paths[i], 50, on_history, &r, &rc,
                                                       &re);
        T_CHECK_MSG(ls == rs, "history %s: local %d, remote %d", paths[i], (int)ls, (int)rs);
        T_CHECK_MSG(lc == rc, "history %s: local %lld rows, remote %lld", paths[i], (long long)lc,
                    (long long)rc);
        SAME(paths[i], l, r);
        sink_free(&l);
        sink_free(&r);
    }
    env_close(&e);
}

static void test_search_including_a_query_with_no_hits(void) {
    env e;
    env_open(&e);
    const char *queries[] = {"a", "zzzz-nothing-matches-this"};
    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; i++) {
        atlas_err le, re;
        atlas_err_init(&le);
        atlas_err_init(&re);
        sink l, r;
        sink_init(&l);
        sink_init(&r);
        int64_t lc = 0, rc = 0;
        atlas_search_mode lm = ATLAS_SEARCH_FTS5, rm = ATLAS_SEARCH_FTS5;
        atlas_status ls =
            atlas_service_search(e.ctx, "proj", queries[i], 50, &lm, on_hit, &l, &lc, &le);
        atlas_status rs =
            atlas_service_search_remote("proj", queries[i], 50, &rm, on_hit, &r, &rc, &re);
        T_CHECK_MSG(ls == rs, "search %s: local %d, remote %d", queries[i], (int)ls, (int)rs);
        T_CHECK_MSG(lm == rm, "search %s: local mode %d, remote %d", queries[i], (int)lm, (int)rm);
        SAME(queries[i], l, r);
        sink_free(&l);
        sink_free(&r);
    }
    env_close(&e);
}

static void test_a_symbol_that_does_not_exist(void) {
    env e;
    env_open(&e);
    atlas_err le, re;
    atlas_err_init(&le);
    atlas_err_init(&re);
    sink l, r;
    sink_init(&l);
    sink_init(&r);
    int64_t lc = 0, rc = 0;
    bool lm = false, rm = false;
    atlas_status ls = atlas_service_code_symbol_sites(e.ctx, "proj", "no_such_symbol", 50,
                                                      on_symbol, &l, &lc, &lm, &le);
    atlas_status rs = atlas_service_code_symbol_sites_remote("proj", "no_such_symbol", 50,
                                                             on_symbol, &r, &rc, &rm, &re);
    T_CHECK_MSG(ls == rs, "symbol: local %d, remote %d (%s / %s)", (int)ls, (int)rs,
                atlas_err_msg(&le), atlas_err_msg(&re));
    SAME("an absent symbol", l, r);
    sink_free(&l);
    sink_free(&r);
    env_close(&e);
}

static void test_a_decision_uid_that_was_never_issued(void) {
    env e;
    env_open(&e);
    atlas_err le, re;
    atlas_err_init(&le);
    atlas_err_init(&re);
    atlas_decision_document ldoc, rdoc;
    atlas_decision_document_init(&ldoc);
    atlas_decision_document_init(&rdoc);
    atlas_status ls = atlas_service_decision_show(e.ctx, "proj", "atlas-dec-nope", 0, &ldoc, &le);
    atlas_status rs = atlas_service_decision_show_remote("proj", "atlas-dec-nope", 0, &rdoc, &re);
    /* Both must refuse, and with the same status: "no such decision" is an
     * answer, and a remote path that turned it into an empty document would be
     * reporting a decision that does not exist. */
    T_CHECK_MSG(ls != ATLAS_OK, "local accepted an unissued decision uid");
    T_CHECK_MSG(ls == rs, "decision show: local status %d, remote %d (%s / %s)", (int)ls, (int)rs,
                atlas_err_msg(&le), atlas_err_msg(&re));
    atlas_decision_document_free(&ldoc);
    atlas_decision_document_free(&rdoc);
    env_close(&e);
}

static void test_an_empty_decision_listing_and_gate(void) {
    env e;
    env_open(&e);
    atlas_err le, re;
    atlas_err_init(&le);
    atlas_err_init(&re);

    atlas_gate_report lrep, rrep;
    atlas_gate_report_init(&lrep);
    atlas_gate_report_init(&rrep);
    atlas_gate_query q;
    memset(&q, 0, sizeof q);
    q.repo_name = "proj";
    atlas_status ls = atlas_service_gate_check(e.ctx, &q, &lrep, &le);
    atlas_status rs = atlas_service_gate_check_remote(&q, &rrep, &re);
    T_CHECK_MSG(ls == rs, "gate: local %d, remote %d (%s / %s)", (int)ls, (int)rs,
                atlas_err_msg(&le), atlas_err_msg(&re));
    if (ls == ATLAS_OK && rs == ATLAS_OK) {
        /* A repository with no approved decisions passes, and both must agree
         * that it did — including on the counts, which are what a caller acts
         * on and which a parser that dropped a field would quietly zero. */
        T_CHECK_MSG(lrep.result == rrep.result, "gate result: local %d, remote %d",
                    (int)lrep.result, (int)rrep.result);
        T_CHECK_MSG(lrep.item_count == rrep.item_count, "gate items: local %zu, remote %zu",
                    lrep.item_count, rrep.item_count);
        T_CHECK(lrep.fresh == rrep.fresh && lrep.stale == rrep.stale &&
                lrep.impacted == rrep.impacted && lrep.unknown == rrep.unknown);
        T_CHECK(lrep.limit_reached == rrep.limit_reached);
    }
    atlas_gate_report_free(&lrep);
    atlas_gate_report_free(&rrep);
    env_close(&e);
}

/* A well-formed uid that was never issued. The local path refuses it by name;
 * a remote path that filtered an assessment and found nothing would answer PASS
 * about a decision that does not exist, which is the most confident wrong
 * answer in the surface. */
static void test_gate_show_for_a_decision_that_does_not_exist(void) {
    env e;
    env_open(&e);
    atlas_err le, re;
    atlas_err_init(&le);
    atlas_err_init(&re);
    atlas_gate_report lrep, rrep;
    atlas_gate_report_init(&lrep);
    atlas_gate_report_init(&rrep);
    static const char UID[] = "atlas-dec-00000000000000000000000000000000";
    atlas_status ls = atlas_service_gate_show(e.ctx, "proj", UID, NULL, &lrep, &le);
    atlas_status rs = atlas_service_gate_show_remote("proj", UID, &rrep, &re);
    T_CHECK_MSG(ls != ATLAS_OK, "local accepted a decision that does not exist");
    T_CHECK_MSG(ls == rs, "gate show: local status %d, remote %d", (int)ls, (int)rs);
    T_CHECK_MSG(strcmp(atlas_err_msg(&le), atlas_err_msg(&re)) == 0,
                "gate show messages differ\n  local : %s\n  remote: %s", atlas_err_msg(&le),
                atlas_err_msg(&re));
    atlas_gate_report_free(&lrep);
    atlas_gate_report_free(&rrep);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"gate show for a decision that does not exist",
     test_gate_show_for_a_decision_that_does_not_exist},
    {"file: present, deleted and not indexed", test_file_present_deleted_and_missing},
    {"history, including a path that has none", test_history_including_a_path_with_none},
    {"search, including a query with no hits", test_search_including_a_query_with_no_hits},
    {"a symbol that does not exist", test_a_symbol_that_does_not_exist},
    {"a decision uid that was never issued", test_a_decision_uid_that_was_never_issued},
    {"an empty gate assessment", test_an_empty_decision_listing_and_gate},
};

ATLAS_TEST_MAIN("remote_equivalence", TESTS)

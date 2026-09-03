/* Atlas - A15 T4: the review walker.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * This suite is the `--check` half only: everything `atlas_service_review_apply`
 * does before it would mint a capability, exercised with `check_only = true` so
 * nothing here needs a terminal or a pseudo-terminal at all. T6 owns the
 * interactive half, which needs the real operator channel and therefore a real
 * pty.
 *
 * Setup goes through the built binary (repo add, scan, propose, revise) the way
 * `tests/test_decision_operator.c` does, because those are ordinary mutating CLI
 * commands with no reason to bypass them. The walk itself is called directly, in
 * process, against a fresh `atlas_ctx` opened read-only on the same data
 * directory -- which is also what proves `check_only` needs neither authority
 * nor a terminal: this test's own binary is not root-owned (no test binary on
 * this machine ever is, see `tests/test_a7_authority.c`), so an unconditional
 * authority check would refuse before the sheet was ever read and every
 * assertion below would be unreachable.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/limits.h"
#include "atlas/review.h"
#include "atlas/service.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"

/* --- the fixture ------------------------------------------------------------ */

typedef struct env {
    fixture fx;
} env;

static void run_atlas(env *e, const char *const *extra, size_t n, atlas_buf *out, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    const char *argv[24];
    size_t k = 0;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    T_REQUIRE(n + k <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    atlas_buf errout = ATLAS_BUF_INIT;
    T_OK(fx_atlas(argv, k, out, &errout, code, &err), &err);
    atlas_buf_free(&errout);
}

static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "main.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "init", &err), &err);

    int code = 0;
    atlas_buf out = ATLAS_BUF_INIT;
    const char *add[] = {"repo", "add", fx_repo(&e->fx), "--name", "proj"};
    run_atlas(e, add, 5u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_reset(&out);

    const char *scan[] = {"scan", "proj"};
    run_atlas(e, scan, 2u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);
}

static void env_close(env *e) {
    fx_close(&e->fx);
}

/* Proposes a decision, returning its uid (a fresh atlas_buf the caller frees). */
static void propose(env *e, atlas_buf *uid_out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_init(uid_out);
    int code = 0;
    atlas_buf out = ATLAS_BUF_INIT;
    const char *args[] = {
        "decision",  "propose", "proj",  "--title", "Use WAL journalling",
        "--decision", "Enable WAL on the index database.",
    };
    run_atlas(e, args, 7u, &out, &code);
    T_EQ_INT(code, 0);
    const char *p = strstr(atlas_buf_cstr(&out), ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE_MSG(p != NULL, "propose did not print a decision id: %s", atlas_buf_cstr(&out));
    size_t len = strlen(ATLAS_DECISION_UID_PREFIX) + ATLAS_DECISION_UID_HEX;
    T_OK(atlas_buf_set(uid_out, p, len, &err), &err);
    T_REQUIRE(atlas_decision_uid_is_valid(atlas_buf_cstr(uid_out)));
    atlas_buf_free(&out);
}

/* Revises an existing decision, producing its revision 2. */
static void revise(env *e, const char *uid) {
    int code = 0;
    atlas_buf out = ATLAS_BUF_INIT;
    const char *args[] = {
        "decision", "revise", "proj", uid, "--title", "Use WAL journalling, revisited",
        "--decision", "Enable WAL on the index database and set a busy timeout.",
    };
    run_atlas(e, args, 8u, &out, &code);
    T_EQ_INT(code, 0);
    atlas_buf_free(&out);
}

/* A fresh, read-only context on the fixture's own data directory -- the same
 * one `atlas_service_review_apply` will be handed directly, in process. */
static void open_ctx(env *e, atlas_ctx **out) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_ctx_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir_override = fx_data_dir(&e->fx);
    opts.mode = ATLAS_CTX_READ;
    T_OK(atlas_ctx_open(&opts, out, &err), &err);
}

/* The first ATLAS_DECISION_CONFIRM_HEX hex characters of one revision's own
 * content hash, read back through the exact function the walker itself calls
 * -- so the expected value in an assertion below is never a hash this test
 * guessed or hard-coded, only one it read the same way the code under test
 * does. */
static void hash_prefix_at(atlas_ctx *ctx, const char *uid, int64_t revision_no,
                           char out8[ATLAS_DECISION_CONFIRM_HEX + 1u]) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_decision_document doc;
    atlas_decision_document_init(&doc);
    T_OK(atlas_service_decision_show(ctx, "proj", uid, revision_no, &doc, &err), &err);
    (void)snprintf(out8, ATLAS_DECISION_CONFIRM_HEX + 1u, "%s",
                   atlas_buf_cstr(&doc.summary.content_hash));
    atlas_decision_document_free(&doc);
}

/* --- capturing the walk's callback ------------------------------------------- */

#define CAP_MAX 8u

typedef struct cap_row {
    atlas_review_verdict verdict;
    atlas_buf detail;
    atlas_buf status;
    int64_t current_revision_no;
    char current_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
} cap_row;

typedef struct capture {
    cap_row rows[CAP_MAX];
    size_t count;
} capture;

static atlas_status on_outcome(const atlas_review_outcome *o, void *ud, atlas_err *err) {
    capture *c = (capture *)ud;
    T_REQUIRE(c->count < CAP_MAX);
    cap_row *row = &c->rows[c->count++];
    row->verdict = o->verdict;
    row->current_revision_no = o->current_revision_no;
    memcpy(row->current_prefix, o->current_prefix, sizeof(row->current_prefix));
    atlas_status st = atlas_buf_set(&row->detail, o->detail.data, o->detail.len, err);
    if (st == ATLAS_OK) {
        st = atlas_buf_set(&row->status, o->status.data, o->status.len, err);
    }
    return st;
}

static void capture_free(capture *c) {
    for (size_t i = 0; i < c->count; i++) {
        atlas_buf_free(&c->rows[i].detail);
        atlas_buf_free(&c->rows[i].status);
    }
}

/* Writes a review sheet to `path`, header first, one line per varargs entry
 * (already-formatted "intent repo decision rN prefix" strings, NULL-terminated
 * list). */
static void write_sheet(env *e, const char *rel, ...) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&body, "atlas-review-sheet/1\n", &err), &err);
    va_list ap;
    va_start(ap, rel);
    for (;;) {
        const char *line = va_arg(ap, const char *);
        if (line == NULL) {
            break;
        }
        T_OK(atlas_buf_append_str(&body, line, &err), &err);
        T_OK(atlas_buf_append_ch(&body, '\n', &err), &err);
    }
    va_end(ap);
    T_OK(fx_write(fx_data_dir(&e->fx), rel, atlas_buf_cstr(&body), &err), &err);
    atlas_buf_free(&body);
}

static int64_t challenge_count(atlas_ctx *ctx) {
    atlas_err err;
    atlas_err_init(&err);
    int64_t n = -1;
    T_OK(atlas_db_query_int64(atlas_ctx_db(ctx), "SELECT COUNT(*) FROM decision_challenges;", &n,
                              &err),
         &err);
    return n;
}

/* --- the test ----------------------------------------------------------------
 *
 * Five entries, in sheet order, covering every pre-check verdict that does not
 * require a terminal:
 *   0. a record still at r1, named at r1 with its real prefix -- READY.
 *   1. a record revised to r2, named at r1 with r1's own (still correct) prefix
 *      -- MOVED by the *latest-revision-number* trigger.
 *   2. a record still at r1, named at r1 with a wrong prefix -- MOVED by the
 *      *content-hash* trigger, the second and independent one the contract
 *      names.
 *   3. a decision id that was never proposed -- MISSING.
 *   4. a record still PROPOSED, named with intent resolve, which needs
 *      APPROVED -- DISPOSED.
 */
static void test_check_only_precheck_verdicts(void) {
    env e;
    env_open(&e);

    atlas_buf ready_uid, moved_latest_uid, moved_prefix_uid, disposed_uid;
    propose(&e, &ready_uid);
    propose(&e, &moved_latest_uid);
    revise(&e, atlas_buf_cstr(&moved_latest_uid));
    propose(&e, &moved_prefix_uid);
    propose(&e, &disposed_uid);

    char missing_uid[ATLAS_DECISION_UID_MAX];
    (void)snprintf(missing_uid, sizeof(missing_uid), "%sffffffffffffffffffffffffffffffff",
                   ATLAS_DECISION_UID_PREFIX);
    T_REQUIRE(atlas_decision_uid_is_valid(missing_uid));

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);

    char ready_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&ready_uid), 1, ready_prefix);
    char moved_latest_r1_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&moved_latest_uid), 1, moved_latest_r1_prefix);
    char moved_latest_r2_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&moved_latest_uid), 2, moved_latest_r2_prefix);
    char moved_prefix_real_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&moved_prefix_uid), 1, moved_prefix_real_prefix);

    /* l4's prefix must be the record's own r1 prefix, or the pre-check would
     * report MOVED (wrong hash) before it ever gets to compare the status --
     * this test wants to isolate the DISPOSED path. */
    char disposed_prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&disposed_uid), 1, disposed_prefix);

    char l0[256], l1[256], l2[256], l3[256], l4[256];
    (void)snprintf(l0, sizeof(l0), "approve proj %s r1 %s", atlas_buf_cstr(&ready_uid),
                   ready_prefix);
    (void)snprintf(l1, sizeof(l1), "approve proj %s r1 %s", atlas_buf_cstr(&moved_latest_uid),
                   moved_latest_r1_prefix);
    (void)snprintf(l2, sizeof(l2), "approve proj %s r1 00000000",
                   atlas_buf_cstr(&moved_prefix_uid));
    (void)snprintf(l3, sizeof(l3), "approve proj %s r1 00000000", missing_uid);
    (void)snprintf(l4, sizeof(l4), "resolve proj %s r1 %s", atlas_buf_cstr(&disposed_uid),
                   disposed_prefix);

    atlas_buf sheet_path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/review.txt", fx_data_dir(&e.fx)), &err);
    write_sheet(&e, "review.txt", l0, l1, l2, l3, l4, NULL);

    int64_t before = challenge_count(ctx);

    capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_review_totals totals;
    atlas_status st = atlas_service_review_apply(ctx, atlas_buf_cstr(&sheet_path),
                                                 /* check_only */ true, on_outcome, &cap, &totals,
                                                 &err);
    T_OK(st, &err);

    int64_t after = challenge_count(ctx);
    T_CHECK_MSG(after == before, "check_only minted %lld capabilities",
                (long long)(after - before));

    T_REQUIRE(cap.count == 5u);

    /* 0: READY */
    T_EQ_INT(cap.rows[0].verdict, ATLAS_REVIEW_READY);
    T_EQ_STR(atlas_buf_cstr(&cap.rows[0].status), "PROPOSED");
    T_EQ_INT(cap.rows[0].detail.len, 0);

    /* 1: MOVED, the latest-revision-number trigger */
    T_EQ_INT(cap.rows[1].verdict, ATLAS_REVIEW_MOVED);
    T_EQ_INT(cap.rows[1].current_revision_no, 2);
    T_EQ_STR(cap.rows[1].current_prefix, moved_latest_r2_prefix);
    {
        char want[256];
        (void)snprintf(want, sizeof(want), "reviewed r1 (%s), now r2 (%s)",
                       moved_latest_r1_prefix, moved_latest_r2_prefix);
        T_EQ_STR(atlas_buf_cstr(&cap.rows[1].detail), want);
    }

    /* 2: MOVED, the content-hash trigger (same revision number both sides) */
    T_EQ_INT(cap.rows[2].verdict, ATLAS_REVIEW_MOVED);
    T_EQ_INT(cap.rows[2].current_revision_no, 1);
    T_EQ_STR(cap.rows[2].current_prefix, moved_prefix_real_prefix);
    {
        char want[256];
        (void)snprintf(want, sizeof(want), "reviewed r1 (00000000), now r1 (%s)",
                       moved_prefix_real_prefix);
        T_EQ_STR(atlas_buf_cstr(&cap.rows[2].detail), want);
    }

    /* 3: MISSING */
    T_EQ_INT(cap.rows[3].verdict, ATLAS_REVIEW_MISSING);
    T_EQ_STR(atlas_buf_cstr(&cap.rows[3].detail), "no such decision in proj");
    T_EQ_INT(cap.rows[3].status.len, 0);

    /* 4: DISPOSED */
    T_EQ_INT(cap.rows[4].verdict, ATLAS_REVIEW_DISPOSED);
    T_EQ_STR(atlas_buf_cstr(&cap.rows[4].detail), "the record is PROPOSED; resolve needs APPROVED");
    T_EQ_STR(atlas_buf_cstr(&cap.rows[4].status), "PROPOSED");

    T_EQ_INT(totals.ready, 1);
    T_EQ_INT(totals.moved, 2);
    T_EQ_INT(totals.missing, 1);
    T_EQ_INT(totals.disposed, 1);
    T_EQ_INT(totals.applied, 0);
    T_EQ_INT(totals.abandoned, 0);
    T_EQ_INT(totals.refused, 0);

    capture_free(&cap);
    atlas_buf_free(&sheet_path);
    atlas_ctx_close(ctx);
    atlas_buf_free(&ready_uid);
    atlas_buf_free(&moved_latest_uid);
    atlas_buf_free(&moved_prefix_uid);
    atlas_buf_free(&disposed_uid);
    env_close(&e);
}

/* A header-only sheet is grammatically valid (T3's parser accepts it on
 * purpose) but is this walker's own refusal: zero entries would otherwise make
 * "every entry ended APPLIED" vacuously true and exit 0 having disposed of
 * nothing. */
static void test_zero_entry_sheet_refused(void) {
    env e;
    env_open(&e);

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);

    atlas_buf sheet_path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/empty.txt", fx_data_dir(&e.fx)), &err);
    write_sheet(&e, "empty.txt", NULL);

    capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_review_totals totals;
    atlas_status st = atlas_service_review_apply(ctx, atlas_buf_cstr(&sheet_path),
                                                 /* check_only */ true, on_outcome, &cap, &totals,
                                                 &err);
    T_CHECK_MSG(st != ATLAS_OK, "a header-only sheet was accepted");
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "no entries") != NULL,
                "wrong refusal for an empty sheet: %s", atlas_err_msg(&err));
    T_EQ_INT(cap.count, 0);

    capture_free(&cap);
    atlas_buf_free(&sheet_path);
    atlas_ctx_close(ctx);
    env_close(&e);
}

/* A sheet whose file is larger than ATLAS_REVIEW_SHEET_MAX_BYTES must be
 * refused by the parser's own frozen sentence -- never silently truncated to
 * the byte ceiling and parsed as though it were a smaller, valid sheet. This
 * is the case ruling #2 in the task exists to guard: reading at most
 * ATLAS_REVIEW_SHEET_MAX_BYTES bytes (rather than ATLAS_REVIEW_SHEET_MAX_BYTES
 * + 1) would make a 70 KB file whose 65536th byte happens to end a comment
 * line parse as a perfectly valid, truncated sheet -- silently dropping
 * everything past that offset and never reaching this refusal at all. One
 * valid entry line precedes the padding so a truncation bug would surface as
 * a *wrongly accepted* sheet with a nonzero entry count, not merely as a
 * zero-entry refusal that could be mistaken for this one. */
static void test_sheet_larger_than_max_bytes_refused(void) {
    env e;
    env_open(&e);

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);

    atlas_err err;
    atlas_err_init(&err);
    atlas_buf body = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&body, "atlas-review-sheet/1\n", &err), &err);
    T_OK(atlas_buf_append_str(
             &body,
             "approve proj atlas-dec-ffffffffffffffffffffffffffffffff r1 00000000\n", &err),
         &err);
    static const char PAD[] =
        "# padding to push this sheet past the byte ceiling, one line at a time\n";
    while (body.len <= (size_t)ATLAS_REVIEW_SHEET_MAX_BYTES) {
        T_OK(atlas_buf_append_str(&body, PAD, &err), &err);
    }
    T_REQUIRE(body.len > (size_t)ATLAS_REVIEW_SHEET_MAX_BYTES);
    T_OK(fx_write(fx_data_dir(&e.fx), "huge.txt", atlas_buf_cstr(&body), &err), &err);
    atlas_buf_free(&body);

    atlas_buf sheet_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/huge.txt", fx_data_dir(&e.fx)), &err);

    capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_review_totals totals;
    atlas_status st = atlas_service_review_apply(ctx, atlas_buf_cstr(&sheet_path),
                                                 /* check_only */ true, on_outcome, &cap, &totals,
                                                 &err);
    T_CHECK_MSG(st != ATLAS_OK, "a sheet larger than the byte ceiling was accepted");
    T_CHECK_MSG(strstr(atlas_err_msg(&err), "larger than") != NULL,
                "wrong refusal for an oversized sheet: %s", atlas_err_msg(&err));
    T_EQ_INT(cap.count, 0);

    capture_free(&cap);
    atlas_buf_free(&sheet_path);
    atlas_ctx_close(ctx);
    env_close(&e);
}

/* `sheet_path` is opened with O_NOFOLLOW: a symlink at that path must be
 * refused, never traversed. */
static void test_sheet_path_symlink_refused(void) {
    env e;
    env_open(&e);

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);

    atlas_err err;
    atlas_err_init(&err);
    write_sheet(&e, "real.txt",
               "approve proj atlas-dec-ffffffffffffffffffffffffffffffff r1 00000000", NULL);
    atlas_buf real_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&real_path, &err, "%s/real.txt", fx_data_dir(&e.fx)), &err);
    T_OK(fx_symlink(fx_data_dir(&e.fx), atlas_buf_cstr(&real_path), "link.txt", &err), &err);

    atlas_buf link_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&link_path, &err, "%s/link.txt", fx_data_dir(&e.fx)), &err);

    capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_review_totals totals;
    atlas_status st = atlas_service_review_apply(ctx, atlas_buf_cstr(&link_path),
                                                 /* check_only */ true, on_outcome, &cap, &totals,
                                                 &err);
    T_CHECK_MSG(st != ATLAS_OK, "a symlinked sheet path was accepted");
    T_EQ_INT(cap.count, 0);

    capture_free(&cap);
    atlas_buf_free(&link_path);
    atlas_buf_free(&real_path);
    atlas_ctx_close(ctx);
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"check_only walks MISSING, MOVED (both triggers), DISPOSED and READY "
     "without minting a capability",
     test_check_only_precheck_verdicts},
    {"a header-only sheet (zero entries) is refused rather than vacuously "
     "succeeding",
     test_zero_entry_sheet_refused},
    {"a sheet larger than the byte ceiling is refused, never silently "
     "truncated and parsed",
     test_sheet_larger_than_max_bytes_refused},
    {"a symlinked sheet path is refused, never traversed",
     test_sheet_path_symlink_refused},
};

ATLAS_TEST_MAIN("review_apply", TESTS)

/* Atlas - A15 T4/T6: the review walker, checked and then run for real.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * T4's half (unchanged below) is `--check` only: everything
 * `atlas_service_review_apply` does before it would mint a capability,
 * exercised with `check_only = true` so nothing there needs a terminal or a
 * pseudo-terminal at all. Setup goes through the built binary (repo add,
 * scan, propose, revise) the way `tests/test_decision_operator.c` does,
 * because those are ordinary mutating CLI commands with no reason to bypass
 * them. The walk itself is called directly, in process, against a fresh
 * `atlas_ctx` opened read-only on the same data directory -- which is also
 * what proves `check_only` needs neither authority nor a terminal: this
 * test's own binary is not root-owned (no test binary on this machine ever
 * is, see `tests/test_a7_authority.c`), so an unconditional authority check
 * would refuse before the sheet was ever read and every assertion there
 * would be unreachable.
 *
 * T6's half, below the `--- A15 T6` marker, is the first time this walker
 * runs against a real terminal, actually mints a capability, and actually
 * spends one -- through the CLI's `run_review`, which checks
 * `atlas_authority_require` unconditionally, before the "apply" verb's own
 * operand count, before `--yes`, before `--json` and before the walker
 * itself. That check can only ever answer GRANTED for a process whose own
 * executable (`/proc/self/exe`) is root-owned and unwritable by any other
 * uid -- which no test binary on this machine is. So, exactly like
 * `tests/test_operator_peer.c`, this suite asks the real, already-deployed
 * policy and binary rather than trying to manufacture either, and skips
 * where the machine has none or where the installed binary predates
 * `review apply` itself. See `unlocked_suite_is_live` below for the exact
 * two-part check and what each half establishes. */
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "atlas/atlas.h"
#include "atlas/authority.h"
#include "atlas/db.h"
#include "atlas/decision.h"
#include "atlas/decision_ops.h"
#include "atlas/limits.h"
#include "atlas/proc.h"
#include "atlas/review.h"
#include "atlas/service.h"
#include "atlas_test.h"
#include "db/db_internal.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"
#include "support/pty.h"

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

    char l0[256], l1[256], l2[256], l3[256], l4[256], l5[256];
    (void)snprintf(l0, sizeof(l0), "approve proj %s r1 %s", atlas_buf_cstr(&ready_uid),
                   ready_prefix);
    (void)snprintf(l1, sizeof(l1), "approve proj %s r1 %s", atlas_buf_cstr(&moved_latest_uid),
                   moved_latest_r1_prefix);
    (void)snprintf(l2, sizeof(l2), "approve proj %s r1 00000000",
                   atlas_buf_cstr(&moved_prefix_uid));
    (void)snprintf(l3, sizeof(l3), "approve proj %s r1 00000000", missing_uid);
    (void)snprintf(l4, sizeof(l4), "resolve proj %s r1 %s", atlas_buf_cstr(&disposed_uid),
                   disposed_prefix);
    /* A repository that is grammatically valid but was never `repo add`-ed --
     * the other MISSING case, mapped from atlas_service_require_repo's
     * NOT_REGISTERED rather than from resolve_uid's refusals above. Nothing
     * in this test names "ghost" as a repository, which is the point. */
    (void)snprintf(l5, sizeof(l5), "approve ghost %s r1 00000000", missing_uid);

    atlas_buf sheet_path = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/review.txt", fx_data_dir(&e.fx)), &err);
    write_sheet(&e, "review.txt", l0, l1, l2, l3, l4, l5, NULL);

    int64_t before = challenge_count(ctx);
    /* Pinned to a value the test asserts, not merely compared to itself:
     * nothing in this fixture ever creates a challenge row, so a
     * challenge_count() that silently answered 0 against the wrong database
     * or a table that did not exist would make the before/after comparison
     * below pass for the wrong reason (0 == 0 regardless of what the walk
     * did). */
    T_EQ_INT(before, 0);

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

    T_REQUIRE(cap.count == 6u);

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

    /* 5: MISSING, via the unregistered-repository branch specifically --
     * ATLAS_ERR_REPO / NOT_REGISTERED, distinct from the ATLAS_ERR_USAGE path
     * row 3 already exercised. */
    T_EQ_INT(cap.rows[5].verdict, ATLAS_REVIEW_MISSING);
    T_EQ_STR(atlas_buf_cstr(&cap.rows[5].detail), "no such repository");
    T_EQ_INT(cap.rows[5].status.len, 0);

    T_EQ_INT(totals.ready, 1);
    T_EQ_INT(totals.moved, 2);
    T_EQ_INT(totals.missing, 2);
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

/* --- A15 T6: the operator channel under a real terminal --------------------- */

/* The same deployed path `tests/test_operator_peer.c` already probes -- not a
 * private copy this suite installs itself. Installing a root-owned binary
 * would be exactly the "manufacture a root-owned artifact for a test" move
 * every other authority suite on this tree refuses to make (see that file's
 * own header comment), and it is not this suite's place to be the exception. */
#define INSTALLED_ATLAS "/usr/local/bin/atlas"

/* GRANTED is necessary but not sufficient: the installed binary can predate
 * the code this suite is testing. Probed with a bare `review`, a local usage
 * refusal `run_review` (src/cli/cli.c) reaches before it checks anything else
 * -- no authority needed to ask it.
 *
 * A `--data-dir` override IS needed, though, and its absence was this fix
 * round's own Critical 1: with no override, a system deployment resolves the
 * *system* data directory (`atlas_datadir_resolve`), `atlas_datadir_is_foreign`
 * (src/core/datadir.c:191) is true because that directory's owner is
 * `atlasd` and not this uid, `dispatch` (cli.c:4651) takes the foreign
 * branch, and `remote_serves` (cli.c:2832) answers `remote_refuse` for a
 * bare `review` before `run_review`'s own usage line at cli.c:2178 is ever
 * reached -- so the probe measured the foreign-index refusal, never the
 * usage line, and always answered false on exactly the deployment shape
 * where the authority half can be true. A throwaway fixture is the same
 * device every other test in this file already uses to keep every command
 * local; here it only ever needs to exist, never to hold a repository. */
static bool installed_atlas_knows_review(void) {
    atlas_err err;
    atlas_err_init(&err);
    fixture probe_fx;
    if (fx_open(&probe_fx, &err) != ATLAS_OK) {
        return false;
    }
    const char *argv[] = {INSTALLED_ATLAS, "--data-dir", fx_data_dir(&probe_fx), "review", NULL};
    const char *envp[] = {"PATH=/usr/bin:/bin", NULL};
    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.env = envp;
    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
    atlas_status st = atlas_proc_run(&opts, atlas_proc_sink_buf, &out, &errout, &res, &err);
    bool ok = st == ATLAS_OK && strstr(atlas_buf_cstr(&errout), "usage: atlas review apply") != NULL;
    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    fx_close(&probe_fx);
    return ok;
}

/* The two ways this machine's readiness can resolve, plus the state this
 * suite is actually live in. `UNLOCK_NOT_GRANTED` is an environment limit --
 * no test can manufacture a root-owned policy, so it is worth nothing more
 * than a note. `UNLOCK_STALE_BINARY` is not: authority answered GRANTED, so
 * a root-owned, current-uid-matching deployment exists, and the *installed
 * binary itself* not knowing `review` is either a stale deploy or a broken
 * probe -- a fact about this machine's own consistency, not a limitation of
 * what an unprivileged test can prove. Conflating the two is exactly how a
 * whole fix round was reported "live" when nothing had run. */
typedef enum unlock_state {
    UNLOCK_LIVE,
    UNLOCK_NOT_GRANTED,
    UNLOCK_STALE_BINARY,
} unlock_state;

/* `why` receives a human-readable reason, named so a skip or failure message
 * says what would change it. `atlas_authority_probe_at`, never `_require` --
 * a probe answers without needing this test's own process to be the one
 * asking through the real check. */
static unlock_state unlocked_suite_state(atlas_buf *why) {
    atlas_authority a;
    atlas_authority_probe_at(ATLAS_AUTHORITY_POLICY_PATH, INSTALLED_ATLAS, &a);
    atlas_err err;
    atlas_err_init(&err);
    if (a.state != ATLAS_AUTHORITY_GRANTED) {
        (void)atlas_buf_appendf(why, &err,
                                "no root-owned authority policy on this machine grants uid %lld "
                                "against %s (%s)",
                                (long long)getuid(), INSTALLED_ATLAS,
                                atlas_authority_reason_name(a.reason));
        return UNLOCK_NOT_GRANTED;
    }
    if (!installed_atlas_knows_review()) {
        (void)atlas_buf_appendf(why, &err,
                                "%s is granted for uid %lld but does not recognise `review` -- a "
                                "stale deploy or a broken probe, not an environment limit",
                                INSTALLED_ATLAS, (long long)getuid());
        return UNLOCK_STALE_BINARY;
    }
    return UNLOCK_LIVE;
}

static bool unlocked_suite_is_live(atlas_buf *why) {
    return unlocked_suite_state(why) == UNLOCK_LIVE;
}

/* `atlas_test_note`, never `T_CHECK_MSG(true, ...)`: the latter's condition
 * never fails, so `atlas_test_fail` -- the only thing that ever prints a
 * `T_CHECK_MSG` string -- never runs, and the message is computed and
 * silently thrown away. Every one of these tests printed a bare "ok" whether
 * it had measured anything or not, which is what let a whole fix round be
 * reported "live" from nothing but that green output. `atlas_test_note` is
 * this tree's own idiom for exactly this (about twenty suites use it,
 * e.g. tests/test_branch_switch.c, tests/test_proc.c): it always prints. */
#define SKIP_UNLESS_UNLOCKED()                                                                  \
    do {                                                                                        \
        atlas_buf why_ = ATLAS_BUF_INIT;                                                        \
        if (!unlocked_suite_is_live(&why_)) {                                                   \
            atlas_test_note("skipped: %s", atlas_buf_cstr(&why_));                              \
            atlas_buf_free(&why_);                                                              \
            return;                                                                             \
        }                                                                                       \
        atlas_buf_free(&why_);                                                                  \
    } while (0)

/* A test in its own right, not only a guard: it names the exact condition of
 * this machine so a report can point at one line that flips from "skipped"
 * to "ok" the moment somebody deploys current code, without needing to
 * re-derive the reason. Note-and-pass only for `UNLOCK_NOT_GRANTED`, which no
 * test can do anything about; `UNLOCK_STALE_BINARY` is a genuine failure --
 * see `unlocked_suite_state`'s own comment for why the two are not the same
 * kind of "not live". */
static void test_the_machine_state_this_suite_depends_on(void) {
    atlas_buf why = ATLAS_BUF_INIT;
    unlock_state st = unlocked_suite_state(&why);
    switch (st) {
    case UNLOCK_LIVE: break;
    case UNLOCK_NOT_GRANTED: atlas_test_note("skipped: %s", atlas_buf_cstr(&why)); break;
    case UNLOCK_STALE_BINARY: T_CHECK_MSG(false, "%s", atlas_buf_cstr(&why)); break;
    }
    atlas_buf_free(&why);
}

/* Runs `INSTALLED_ATLAS` non-interactively (stdin, stdout and stderr are all
 * pipes -- `atlas_proc_run` always gives a child `/dev/null` on stdin, which
 * is not a terminal either), isolated from any real daemon on this machine by
 * a private, empty `XDG_RUNTIME_DIR` the fixture owns -- so a `review apply`
 * invocation against the currently-deployed binary can never reach the
 * socket the real `atlasd` happens to be serving. */
static void run_installed(env *e, const char *const *extra, size_t n, atlas_buf *out,
                          atlas_buf *errout, int *code) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_mkdir(atlas_buf_cstr(&e->fx.root), "instroot", &err), &err);
    atlas_buf rt = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&rt, &err, "%s/instroot", atlas_buf_cstr(&e->fx.root)), &err);

    const char *argv[24];
    size_t k = 0;
    argv[k++] = INSTALLED_ATLAS;
    argv[k++] = "--data-dir";
    argv[k++] = fx_data_dir(&e->fx);
    T_REQUIRE(n + k <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < n; i++) {
        argv[k++] = extra[i];
    }
    argv[k] = NULL;

    char home[1024], xdgrt[1200];
    (void)snprintf(home, sizeof(home), "HOME=%s", fx_data_dir(&e->fx));
    (void)snprintf(xdgrt, sizeof(xdgrt), "XDG_RUNTIME_DIR=%s", atlas_buf_cstr(&rt));
    const char *envp[] = {"PATH=/usr/bin:/bin", home, xdgrt, "LC_ALL=C", "TZ=UTC", NULL};

    atlas_proc_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.env = envp;
    opts.timeout_ms = 20000;
    opts.max_stdout = 4u * 1024u * 1024u;

    atlas_proc_result res;
    memset(&res, 0, sizeof(res));
    T_OK(atlas_proc_run(&opts, atlas_proc_sink_buf, out, errout, &res, &err), &err);
    *code = res.exit_code;
    atlas_buf_free(&rt);
}

/* Reads `decision_challenges.consumed` for the single challenge minted
 * against `uid`'s document, or -1 if none exists. `uid` is always one of this
 * file's own checked-shape decision ids, never repository-controlled text, so
 * building the query with `snprintf` rather than a bound parameter carries no
 * injection risk here. */
static int64_t challenge_consumed_for(atlas_ctx *ctx, const char *uid) {
    char sql[512];
    (void)snprintf(sql, sizeof(sql),
                   "SELECT consumed FROM decision_challenges WHERE document_id = "
                   "(SELECT id FROM decision_documents WHERE uid = '%s') ORDER BY id DESC LIMIT 1;",
                   uid);
    atlas_err err;
    atlas_err_init(&err);
    int64_t v = -1;
    T_OK(atlas_db_query_int64(atlas_ctx_db(ctx), sql, &v, &err), &err);
    return v;
}

static size_t count_occurrences(const char *hay, const char *needle) {
    size_t n = 0;
    size_t nlen = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

/* Like `pty_expect`, but waits for the `want`-th occurrence of `needle`
 * rather than the first -- `pty_expect` alone cannot tell a suite's second
 * prompt from its first, because a needle already in the transcript makes it
 * return immediately. Kept local to this file rather than folded into
 * `tests/support/pty.c`, which Step 1 moved unchanged in behaviour. */
static bool pty_expect_nth(pty *p, const char *needle, size_t want, atlas_buf *transcript) {
    atlas_err err;
    atlas_err_init(&err);
    for (int waited = 0; waited < 200; waited++) {
        if (count_occurrences(atlas_buf_cstr(transcript), needle) >= want) {
            return true;
        }
        struct pollfd pfd = {p->master, POLLIN, 0};
        int rc = poll(&pfd, 1u, 50);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            continue;
        }
        char buf[1024];
        ssize_t got = read(p->master, buf, sizeof(buf));
        if (got <= 0) {
            break;
        }
        if (atlas_buf_append(transcript, buf, (size_t)got, &err) != ATLAS_OK) {
            return false;
        }
    }
    return count_occurrences(atlas_buf_cstr(transcript), needle) >= want;
}

/* (a) a sheet on stdin is refused before anything is minted: `run_installed`
 * gives the child a non-terminal stdin and stdout (a pipe, never a pty), so
 * `atlas_terminal_open` refuses before the sheet -- named here as
 * `/dev/stdin` -- is ever opened. */
static void test_a_sheet_on_stdin_is_refused_before_anything_is_minted(void) {
    SKIP_UNLESS_UNLOCKED();
    env e;
    env_open(&e);
    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);
    int64_t before = challenge_count(ctx);

    atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
    int code = 0;
    const char *args[] = {"review", "apply", "/dev/stdin"};
    run_installed(&e, args, 3u, &out, &errout, &code);
    /* The frozen contract: `atlas_terminal_open` returns `ATLAS_ERR_USAGE`
     * (src/core/terminal.c:34-36), which is exit 2 -- not merely nonzero,
     * which a wrong refusal (3, "config"; 7, "integrity") would also satisfy. */
    T_EQ_INT(code, 2);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "interactive terminal") != NULL,
                "wrong refusal for a non-terminal stdin: %s", atlas_buf_cstr(&errout));

    int64_t after = challenge_count(ctx);
    T_CHECK_MSG(after == before, "a refused sheet minted %lld capabilities",
                (long long)(after - before));

    atlas_buf_free(&out);
    atlas_buf_free(&errout);
    atlas_ctx_close(ctx);
    env_close(&e);
}

/* (b) `--yes` and `--json` without `--check` are refused with the frozen
 * sentences, exit 2, and mint nothing. */
static void test_yes_and_json_without_check_are_refused(void) {
    SKIP_UNLESS_UNLOCKED();
    env e;
    env_open(&e);
    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);
    int64_t before = challenge_count(ctx);

    static const char YES_SENTENCE[] =
        "--yes cannot apply a review sheet. Each entry needs its confirmation typed on an "
        "interactive terminal, and Atlas will not accept one from a flag, a pipe, the sheet "
        "itself or an environment variable.";
    static const char JSON_SENTENCE[] =
        "--json is not available for review apply: it is an interactive command. Use --check "
        "for a machine-readable dry run.";

    {
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", "/nonexistent-sheet", "--yes"};
        run_installed(&e, args, 4u, &out, &errout, &code);
        T_EQ_INT(code, 2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), YES_SENTENCE) != NULL,
                    "--yes did not print the frozen sentence: %s", atlas_buf_cstr(&errout));
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }
    {
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", "/nonexistent-sheet", "--json"};
        run_installed(&e, args, 4u, &out, &errout, &code);
        T_EQ_INT(code, 2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), JSON_SENTENCE) != NULL,
                    "--json without --check did not print the frozen sentence: %s",
                    atlas_buf_cstr(&errout));
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }

    int64_t after = challenge_count(ctx);
    T_CHECK_MSG(after == before, "a refused invocation minted %lld capabilities",
                (long long)(after - before));

    atlas_ctx_close(ctx);
    env_close(&e);
}

/* (c) and (g): a two-entry sheet under a real pty -- entry 1 current and
 * applied for real, entry 2 revised after the sheet was written and so MOVED
 * -- plus the adversarial obligation that the registered repository is never
 * modified by any of it. */
static void test_two_entry_sheet_under_a_pty_applies_and_moves(void) {
    SKIP_UNLESS_UNLOCKED();
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    atlas_buf uid_a, uid_b;
    propose(&e, &uid_a);
    propose(&e, &uid_b);

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);
    char prefix_a[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid_a), 1, prefix_a);
    char prefix_b_r1[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid_b), 1, prefix_b_r1);

    char l0[256], l1[256];
    (void)snprintf(l0, sizeof(l0), "approve proj %s r1 %s", atlas_buf_cstr(&uid_a), prefix_a);
    (void)snprintf(l1, sizeof(l1), "approve proj %s r1 %s", atlas_buf_cstr(&uid_b), prefix_b_r1);
    write_sheet(&e, "review.txt", l0, l1, NULL);
    atlas_buf sheet_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/review.txt", fx_data_dir(&e.fx)), &err);

    /* Entry 2's decision is revised *after* the sheet was written: the sheet
     * still names r1's own prefix, which the pre-check now finds stale. */
    revise(&e, atlas_buf_cstr(&uid_b));
    char prefix_b_r2[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid_b), 2, prefix_b_r2);

    int64_t before = challenge_count(ctx);
    char digest_before[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), digest_before, &err), &err);

    const char *args[] = {"review", "apply", atlas_buf_cstr(&sheet_path)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(fx_data_dir(&e.fx), INSTALLED_ATLAS, args, 3u, &p, &err) == ATLAS_OK);

    atlas_buf transcript = ATLAS_BUF_INIT;
    T_CHECK_MSG(pty_expect(&p, "Type ", &transcript), "no prompt appeared for entry 1:\n%s",
                atlas_buf_cstr(&transcript));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&transcript), "revision   : 1") != NULL,
                "the prompt did not show revision 1:\n%s", atlas_buf_cstr(&transcript));
    char digest_needle[80];
    (void)snprintf(digest_needle, sizeof(digest_needle), "digest     : %s", prefix_a);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&transcript), digest_needle) != NULL,
                "the prompt did not show the expected digest:\n%s", atlas_buf_cstr(&transcript));

    pty_type(&p, prefix_a);
    T_CHECK_MSG(pty_expect(&p, "APPLIED", &transcript), "entry 1 was not APPLIED:\n%s",
                atlas_buf_cstr(&transcript));
    T_CHECK_MSG(pty_expect(&p, "MOVED", &transcript), "entry 2 was not MOVED:\n%s",
                atlas_buf_cstr(&transcript));

    int code = pty_wait(&p, &transcript);
    T_REQUIRE_MSG(code != PTY_WAIT_TIMED_OUT,
                  "the walker never exited (it may be stuck prompting):\n%s",
                  atlas_buf_cstr(&transcript));
    const char *text = atlas_buf_cstr(&transcript);

    /* No prompt for entry 2: "Type " appears exactly once in the whole
     * transcript, because entry 2's pre-check finds it MOVED before a
     * capability is ever minted for it. */
    T_CHECK_MSG(count_occurrences(text, "Type ") == 1u,
                "expected exactly one prompt (\"Type \"), found %zu:\n%s",
                count_occurrences(text, "Type "), text);

    char moved_detail[160];
    (void)snprintf(moved_detail, sizeof(moved_detail), "reviewed r1 (%s), now r2 (%s)",
                   prefix_b_r1, prefix_b_r2);
    T_CHECK_MSG(strstr(text, moved_detail) != NULL, "wrong MOVED detail:\n%s", text);
    T_EQ_INT(code, 8); /* one entry ended other than APPLIED */

    int64_t after = challenge_count(ctx);
    T_CHECK_MSG(after == before + 1, "expected exactly one capability minted, changed by %lld",
                (long long)(after - before));

    atlas_buf hist = ATLAS_BUF_INIT;
    int hcode = 0;
    const char *histargs[] = {"--json", "decision", "history", "proj", atlas_buf_cstr(&uid_a)};
    run_atlas(&e, histargs, 5u, &hist, &hcode);
    T_EQ_INT(hcode, 0);
    /* Exactly one APPROVED event, not merely one somewhere in the document's
     * history (which also carries the PROPOSE event) -- `"event":"..."` is
     * `j_decision_event`'s first key, from the closed state-name vocabulary,
     * so the literal substring is an unambiguous count. */
    T_CHECK_MSG(count_occurrences(atlas_buf_cstr(&hist), "\"event\":\"APPROVED\"") == 1u,
                "expected exactly one APPROVED event: %s", atlas_buf_cstr(&hist));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&hist), "\"actor\":\"LOCAL_OPERATOR_CONFIRMED\"") != NULL,
                "history does not show the operator-channel actor: %s", atlas_buf_cstr(&hist));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&hist), "\"operator_channel\":true") != NULL,
                "history does not mark operator_channel true: %s", atlas_buf_cstr(&hist));
    atlas_buf_free(&hist);

    /* (g): the registered repository is untouched by a real apply. */
    char digest_after[ATLAS_SHA256_HEX_LEN + 1u];
    T_OK(fx_tree_digest(fx_repo(&e.fx), digest_after, &err), &err);
    T_CHECK_MSG(strcmp(digest_before, digest_after) == 0,
                "the registered repository was modified by review apply");

    atlas_buf_free(&transcript);
    atlas_buf_free(&sheet_path);
    atlas_ctx_close(ctx);
    atlas_buf_free(&uid_a);
    atlas_buf_free(&uid_b);
    env_close(&e);
}

/* (d) a mistyped confirmation on entry 1 of a two-entry sheet: ABANDONED, the
 * challenge left unconsumed, entry 2 still prompted and applied, exit 8, and
 * the real (non-`--check`) totals line. */
static void test_a_mistyped_confirmation_abandons_and_leaves_the_challenge_unconsumed(void) {
    SKIP_UNLESS_UNLOCKED();
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);

    atlas_buf uid_a, uid_b;
    propose(&e, &uid_a);
    propose(&e, &uid_b);

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);
    char prefix_a[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid_a), 1, prefix_a);
    char prefix_b[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid_b), 1, prefix_b);

    char l0[256], l1[256];
    (void)snprintf(l0, sizeof(l0), "approve proj %s r1 %s", atlas_buf_cstr(&uid_a), prefix_a);
    (void)snprintf(l1, sizeof(l1), "approve proj %s r1 %s", atlas_buf_cstr(&uid_b), prefix_b);
    write_sheet(&e, "review.txt", l0, l1, NULL);
    atlas_buf sheet_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/review.txt", fx_data_dir(&e.fx)), &err);

    int64_t before = challenge_count(ctx);

    const char *args[] = {"review", "apply", atlas_buf_cstr(&sheet_path)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(fx_data_dir(&e.fx), INSTALLED_ATLAS, args, 3u, &p, &err) == ATLAS_OK);

    atlas_buf transcript = ATLAS_BUF_INIT;
    T_CHECK_MSG(pty_expect_nth(&p, "Type ", 1u, &transcript), "no prompt for entry 1:\n%s",
                atlas_buf_cstr(&transcript));
    pty_type(&p, "0000000000000000"); /* deliberately not the confirmation */

    T_CHECK_MSG(pty_expect_nth(&p, "Type ", 2u, &transcript), "no prompt for entry 2:\n%s",
                atlas_buf_cstr(&transcript));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&transcript), "ABANDONED") != NULL,
                "entry 1 must already read ABANDONED before entry 2 prompts:\n%s",
                atlas_buf_cstr(&transcript));

    pty_type(&p, prefix_b);
    T_CHECK_MSG(pty_expect(&p, "APPLIED", &transcript), "entry 2 was not APPLIED:\n%s",
                atlas_buf_cstr(&transcript));

    int code = pty_wait(&p, &transcript);
    T_REQUIRE_MSG(code != PTY_WAIT_TIMED_OUT,
                  "the walker never exited (it may be stuck prompting):\n%s",
                  atlas_buf_cstr(&transcript));
    const char *text = atlas_buf_cstr(&transcript);
    T_CHECK_MSG(strstr(text, "nothing was changed") != NULL,
                "wrong ABANDONED detail:\n%s", text);
    /* `\r\n`, not `\n`: this transcript came off a pty *master*, and bytes a
     * pty slave writes pass through the line discipline's default `ONLCR`,
     * which turns every `\n` the CLI wrote into `\r\n` on the wire. Proven
     * with a standalone probe (a pty child writing "a b\n" reads back
     * `61 20 62 0d 0a` on the master). `run_installed`'s callers read a pipe,
     * never a pty, so their needles keep a plain `\n` -- see
     * `test_human_check_totals_line_and_missing_status_null`, which checks
     * the identical totals shape unmodified. */
    T_CHECK_MSG(strstr(text, "applied 1, abandoned 1, moved 0, disposed 0, missing 0, "
                             "refused 0\r\n") != NULL,
                "wrong real (non-check) totals line:\n%s", text);
    T_EQ_INT(code, 8);

    /* Total, not only the unconsumed row: `ORDER BY id DESC LIMIT 1` alone
     * would still pass a walker that re-minted a second challenge for entry
     * 1 after the mistype and then abandoned that one too, leaving only the
     * *newest* row unconsumed while an equally-wrong earlier one from the
     * same entry sat underneath it, uncounted. Exactly one challenge per
     * entry -- entry 1's abandoned, entry 2's spent -- is the actual claim;
     * the unconsumed check below answers a narrower question and stays
     * alongside this one rather than instead of it. */
    int64_t after = challenge_count(ctx);
    T_CHECK_MSG(after == before + 2,
                "expected exactly two capabilities minted (one abandoned, one spent), changed "
                "by %lld",
                (long long)(after - before));

    int64_t consumed = challenge_consumed_for(ctx, atlas_buf_cstr(&uid_a));
    T_EQ_INT(consumed, 0);

    atlas_buf_free(&transcript);
    atlas_buf_free(&sheet_path);
    atlas_ctx_close(ctx);
    atlas_buf_free(&uid_a);
    atlas_buf_free(&uid_b);
    env_close(&e);
}

/* (e) an already-approved record on an `approve` line: DISPOSED, no prompt,
 * nothing minted. Approving happens through the write point directly
 * (`tests/test_decision_operator.c`'s own `approve_through_the_write_point`
 * pattern), since this test's *setup* needs an APPROVED record and the
 * interactive CLI path is exactly what is being reserved to check the
 * DISPOSED behaviour itself. */
static void test_an_already_approved_record_on_an_approve_line_is_disposed(void) {
    SKIP_UNLESS_UNLOCKED();
    atlas_err err;
    atlas_err_init(&err);
    env e;
    env_open(&e);
    atlas_buf uid;
    propose(&e, &uid);

    atlas_buf db_path = ATLAS_BUF_INIT;
    T_OK(atlas_datadir_db_path(fx_data_dir(&e.fx), &db_path, &err), &err);
    atlas_db *db = NULL;
    T_OK(atlas_db_open(atlas_buf_cstr(&db_path), &db, &err), &err);
    atlas_decision_op ch;
    atlas_decision_op_init(&ch, ATLAS_DECISION_OP_CHALLENGE);
    T_OK(atlas_buf_set_str(&ch.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ch.uid, atlas_buf_cstr(&uid), &err), &err);
    ch.intent = ATLAS_DECISION_INTENT_APPROVE;
    atlas_decision_result cr;
    atlas_decision_result_init(&cr);
    T_OK(atlas_decision_apply(db, &ch, &cr, &err), &err);
    atlas_decision_op ap;
    atlas_decision_op_init(&ap, ATLAS_DECISION_OP_APPROVE);
    T_OK(atlas_buf_set_str(&ap.repo_name, "proj", &err), &err);
    T_OK(atlas_buf_set_str(&ap.uid, atlas_buf_cstr(&uid), &err), &err);
    T_OK(atlas_buf_set(&ap.token, cr.token.data, cr.token.len, &err), &err);
    T_OK(atlas_buf_set_str(&ap.confirmation, cr.confirm, &err), &err);
    atlas_decision_result ar;
    atlas_decision_result_init(&ar);
    T_OK(atlas_decision_apply(db, &ap, &ar, &err), &err);
    T_REQUIRE(ar.state == ATLAS_DECISION_APPROVED);
    atlas_decision_result_free(&ar);
    atlas_decision_op_free(&ap);
    atlas_decision_result_free(&cr);
    atlas_decision_op_free(&ch);
    atlas_db_close(db);
    atlas_buf_free(&db_path);

    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);
    int64_t before = challenge_count(ctx);

    char prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid), 1, prefix);
    char l0[256];
    (void)snprintf(l0, sizeof(l0), "approve proj %s r1 %s", atlas_buf_cstr(&uid), prefix);
    write_sheet(&e, "review.txt", l0, NULL);
    atlas_buf sheet_path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&sheet_path, &err, "%s/review.txt", fx_data_dir(&e.fx)), &err);

    const char *args[] = {"review", "apply", atlas_buf_cstr(&sheet_path)};
    pty p = {-1, -1};
    T_REQUIRE(pty_spawn(fx_data_dir(&e.fx), INSTALLED_ATLAS, args, 3u, &p, &err) == ATLAS_OK);
    atlas_buf transcript = ATLAS_BUF_INIT;
    int code = pty_wait(&p, &transcript);
    T_REQUIRE_MSG(code != PTY_WAIT_TIMED_OUT,
                  "the walker never exited (it may be stuck prompting):\n%s",
                  atlas_buf_cstr(&transcript));
    const char *text = atlas_buf_cstr(&transcript);

    T_CHECK_MSG(strstr(text, "Type ") == NULL, "a DISPOSED entry must never prompt:\n%s", text);
    T_CHECK_MSG(strstr(text, "DISPOSED") != NULL, "expected DISPOSED:\n%s", text);
    T_CHECK_MSG(strstr(text, "the record is APPROVED; approve needs PROPOSED") != NULL,
                "wrong DISPOSED detail:\n%s", text);
    T_EQ_INT(code, 8);

    int64_t after = challenge_count(ctx);
    T_CHECK_MSG(after == before, "a DISPOSED entry must mint nothing, changed by %lld",
                (long long)(after - before));

    atlas_buf_free(&transcript);
    atlas_buf_free(&sheet_path);
    atlas_ctx_close(ctx);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* `run_atlas` (T4, above) discards stderr, and a human-mode error goes to
 * stderr (`atlas_render_error`) -- `run_atlas` returns only `out`, stdout,
 * which a refused command never writes to at all. This local variant keeps
 * stderr instead, for the one test below that needs to read a refusal's
 * text rather than only its exit code. */
static void run_atlas_keep_stderr(env *e, const char *const *extra, size_t n, atlas_buf *errout,
                                  int *code) {
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
    atlas_buf out = ATLAS_BUF_INIT;
    T_OK(fx_atlas(argv, k, &out, errout, code, &err), &err);
    atlas_buf_free(&out);
}

/* (f) a locked profile: run against `ATLAS_BIN` (this build's own binary,
 * never root-owned), naming a sheet path that does not exist -- the refusal
 * must be the authority sentence, not a file-open error, proving the walk
 * never reaches the sheet. Needs no unlocked machine at all. */
static void test_a_locked_profile_refuses_before_the_sheet_is_opened(void) {
    env e;
    env_open(&e);
    atlas_buf errout = ATLAS_BUF_INIT;
    int code = 0;
    const char *args[] = {"review", "apply", "/definitely/does/not/exist-a15t6", "--check"};
    run_atlas_keep_stderr(&e, args, 4u, &errout, &code);
    /* The frozen contract: `atlas_authority_require`'s refusal is
     * `ATLAS_ERR_CONFIG` (src/core/authority.c:345-346), which is exit 3 --
     * not merely nonzero, which a wrong refusal (2, a usage error; 7, an
     * integrity refusal) would also satisfy while meaning something entirely
     * different about where the command stopped. Same finding as obligation
     * (a)'s Minor, one test over. */
    T_EQ_INT(code, 3);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "locked in this Atlas profile") != NULL,
                "expected the authority refusal, not a file error: %s", atlas_buf_cstr(&errout));
    atlas_buf_free(&errout);
    env_close(&e);
}

/* item 2: under `--json --check`, a missing file, a malformed sheet and a
 * header-only sheet must each produce stdout that parses as *exactly one*
 * JSON value. The renderer is opened late precisely so a pre-entry refusal
 * cannot land inside a half-opened document; nothing has ever exercised
 * that, because reading the code is not evidence here. */
static void test_json_check_produces_exactly_one_document_on_every_pre_entry_refusal(void) {
    SKIP_UNLESS_UNLOCKED();
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    struct {
        const char *what;
        const char *rel; /* NULL means "the path does not exist at all" */
        const char *body;
    } cases[] = {
        {"a missing file", NULL, NULL},
        {"a malformed sheet", "malformed.txt", "not-the-right-header\n"},
        {"a header-only sheet", "empty.txt", "atlas-review-sheet/1\n"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        atlas_buf path = ATLAS_BUF_INIT;
        if (cases[i].rel != NULL) {
            T_OK(fx_write(fx_data_dir(&e.fx), cases[i].rel, cases[i].body, &err), &err);
            T_OK(atlas_buf_appendf(&path, &err, "%s/%s", fx_data_dir(&e.fx), cases[i].rel), &err);
        } else {
            T_OK(atlas_buf_appendf(&path, &err, "%s/does-not-exist-at-all", fx_data_dir(&e.fx)),
                 &err);
        }

        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", atlas_buf_cstr(&path), "--check", "--json"};
        run_installed(&e, args, 5u, &out, &errout, &code);

        size_t bad = 0;
        T_CHECK_MSG(tjson_valid(out.data, out.len, &bad),
                    "%s: stdout is not exactly one JSON value (offset %zu): %s", cases[i].what,
                    bad, atlas_buf_cstr(&out));
        atlas_buf ok_val = ATLAS_BUF_INIT;
        T_CHECK_MSG(tjson_get_raw(out.data, out.len, "ok", &ok_val) &&
                        strcmp(atlas_buf_cstr(&ok_val), "false") == 0,
                    "%s: expected \"ok\":false, got: %s", cases[i].what, atlas_buf_cstr(&out));
        atlas_buf_free(&ok_val);

        atlas_buf_free(&out);
        atlas_buf_free(&errout);
        atlas_buf_free(&path);
    }
    env_close(&e);
}

/* items 3, 6 and 7: the `--check` exit code reflects every entry's verdict
 * (all-READY exits 0, one MISSING exits 8), a one-operand `review apply`
 * reaches the usage line (only testable unlocked, since authority is checked
 * first), and the JSON envelope carries `atlas`, `phase`, `ok` and
 * `text_encoding` around this command's own `check` -- checked as presence,
 * never as "ok" immediately followed by "check": the plan's frozen JSON
 * excerpt elides the first three, which the real envelope does not omit. */
static void test_check_exit_code_usage_line_and_envelope(void) {
    SKIP_UNLESS_UNLOCKED();
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf uid;
    propose(&e, &uid);
    atlas_ctx *ctx = NULL;
    open_ctx(&e, &ctx);
    char prefix[ATLAS_DECISION_CONFIRM_HEX + 1u];
    hash_prefix_at(ctx, atlas_buf_cstr(&uid), 1, prefix);

    /* all-READY: exit 0, plus the envelope check. */
    {
        char l0[256];
        (void)snprintf(l0, sizeof(l0), "approve proj %s r1 %s", atlas_buf_cstr(&uid), prefix);
        write_sheet(&e, "ready.txt", l0, NULL);
        atlas_buf path = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&path, &err, "%s/ready.txt", fx_data_dir(&e.fx)), &err);
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", atlas_buf_cstr(&path), "--check", "--json"};
        run_installed(&e, args, 5u, &out, &errout, &code);
        T_EQ_INT(code, 0);
        size_t bad = 0;
        T_CHECK_MSG(tjson_valid(out.data, out.len, &bad),
                    "not exactly one JSON value (offset %zu): %s", bad, atlas_buf_cstr(&out));
        atlas_buf v = ATLAS_BUF_INIT;
        T_CHECK_MSG(tjson_get_string(out.data, out.len, "atlas", &v), "missing \"atlas\": %s",
                    atlas_buf_cstr(&out));
        atlas_buf_reset(&v);
        T_CHECK_MSG(tjson_get_string(out.data, out.len, "phase", &v), "missing \"phase\": %s",
                    atlas_buf_cstr(&out));
        atlas_buf_reset(&v);
        T_CHECK_MSG(tjson_get_raw(out.data, out.len, "ok", &v) &&
                        strcmp(atlas_buf_cstr(&v), "true") == 0,
                    "missing or wrong \"ok\": %s", atlas_buf_cstr(&out));
        atlas_buf_reset(&v);
        T_CHECK_MSG(tjson_get_string(out.data, out.len, "text_encoding", &v),
                    "missing \"text_encoding\": %s", atlas_buf_cstr(&out));
        atlas_buf_reset(&v);
        T_CHECK_MSG(tjson_get_raw(out.data, out.len, "check", &v) &&
                        strcmp(atlas_buf_cstr(&v), "true") == 0,
                    "missing or wrong \"check\": %s", atlas_buf_cstr(&out));
        atlas_buf_free(&v);
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
        atlas_buf_free(&path);
    }

    /* one MISSING: exit 8. */
    {
        char missing_uid[ATLAS_DECISION_UID_MAX];
        (void)snprintf(missing_uid, sizeof(missing_uid), "%sffffffffffffffffffffffffffffffff",
                       ATLAS_DECISION_UID_PREFIX);
        char l0[256];
        (void)snprintf(l0, sizeof(l0), "approve proj %s r1 00000000", missing_uid);
        write_sheet(&e, "missing.txt", l0, NULL);
        atlas_buf path = ATLAS_BUF_INIT;
        T_OK(atlas_buf_appendf(&path, &err, "%s/missing.txt", fx_data_dir(&e.fx)), &err);
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", atlas_buf_cstr(&path), "--check"};
        run_installed(&e, args, 4u, &out, &errout, &code);
        T_EQ_INT(code, 8);
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
        atlas_buf_free(&path);
    }

    /* item 6: one operand reaches the usage line. */
    {
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply"};
        run_installed(&e, args, 2u, &out, &errout, &code);
        T_EQ_INT(code, 2);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&errout), "usage: atlas review apply FILE [--check]") !=
                        NULL,
                    "expected the usage line, got: %s", atlas_buf_cstr(&errout));
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }

    atlas_ctx_close(ctx);
    atlas_buf_free(&uid);
    env_close(&e);
}

/* item 5: the human `--check` totals line, and `"status":null` for a MISSING
 * entry -- neither is frozen in the plan, so this test is what fixes them. */
static void test_human_check_totals_line_and_missing_status_null(void) {
    SKIP_UNLESS_UNLOCKED();
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    char missing_uid[ATLAS_DECISION_UID_MAX];
    (void)snprintf(missing_uid, sizeof(missing_uid), "%sffffffffffffffffffffffffffffffff",
                   ATLAS_DECISION_UID_PREFIX);
    char l0[256];
    (void)snprintf(l0, sizeof(l0), "approve proj %s r1 00000000", missing_uid);
    write_sheet(&e, "missing.txt", l0, NULL);
    atlas_buf path = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&path, &err, "%s/missing.txt", fx_data_dir(&e.fx)), &err);

    {
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", atlas_buf_cstr(&path), "--check"};
        run_installed(&e, args, 4u, &out, &errout, &code);
        T_EQ_INT(code, 8);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "ready 0, moved 0, disposed 0, missing 1\n") !=
                        NULL,
                    "unexpected human --check totals line: %s", atlas_buf_cstr(&out));
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }
    {
        atlas_buf out = ATLAS_BUF_INIT, errout = ATLAS_BUF_INIT;
        int code = 0;
        const char *args[] = {"review", "apply", atlas_buf_cstr(&path), "--check", "--json"};
        run_installed(&e, args, 5u, &out, &errout, &code);
        T_EQ_INT(code, 8);
        size_t bad = 0;
        T_CHECK_MSG(tjson_valid(out.data, out.len, &bad),
                    "not exactly one JSON value (offset %zu): %s", bad, atlas_buf_cstr(&out));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"status\":null") != NULL,
                    "a MISSING entry must show \"status\":null: %s", atlas_buf_cstr(&out));
        atlas_buf_free(&out);
        atlas_buf_free(&errout);
    }

    atlas_buf_free(&path);
    env_close(&e);
}

/* (h): the operator channel's two write points keep exactly the callers this
 * season documents. Mirrors `tests/test_decision_mcp.c`'s own source-walk
 * shape (`test_the_single_write_point_has_exactly_three_callers`), reused
 * here rather than shared, because a claim about a call graph decays the
 * moment somebody adds a caller and each suite needs its own tripwire. */
typedef struct callsite_scan {
    const char *needle;
    size_t files_with_calls;
    atlas_buf names;
} callsite_scan;

static void scan_for_needle(const char *path, void *ud) {
    callsite_scan *sc = (callsite_scan *)ud;
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return;
    }
    atlas_buf text = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1u, sizeof(chunk), fp)) > 0) {
        T_OK(atlas_buf_append(&text, chunk, n, &err), &err);
    }
    (void)fclose(fp);
    if (strstr(atlas_buf_cstr(&text), sc->needle) != NULL) {
        sc->files_with_calls++;
        T_OK(atlas_buf_appendf(&sc->names, &err, "%s ", path), &err);
    }
    atlas_buf_free(&text);
}

static void walk_sources(const char *dir, void (*visit)(const char *path, void *ud), void *ud) {
    DIR *d = opendir(dir);
    T_REQUIRE_MSG(d != NULL, "cannot open %s", dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk_sources(path, visit, ud);
            continue;
        }
        size_t len = strlen(path);
        if (len > 2u && path[len - 2u] == '.' && (path[len - 1u] == 'c' || path[len - 1u] == 'h')) {
            visit(path, ud);
        }
    }
    (void)closedir(d);
}

static void test_the_operator_channel_write_points_have_the_documented_callers(void) {
    callsite_scan sc1 = {"atlas_decision_apply_in_tx(", 0u, ATLAS_BUF_INIT};
    walk_sources(ATLAS_SRC_DIR "/src", scan_for_needle, &sc1);
    T_CHECK_MSG(sc1.files_with_calls == 3u,
                "expected exactly 3 files naming the single write point, found %zu: %s",
                sc1.files_with_calls, atlas_buf_cstr(&sc1.names));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&sc1.names), "/src/core/service_review.c") == NULL,
                "the review walker must call no write point directly: %s",
                atlas_buf_cstr(&sc1.names));
    atlas_buf_free(&sc1.names);

    /* Three files, not two: the substring names the definition as well as
     * every call site, and `src/core/service_decision.c` is where
     * `atlas_service_decision_confirm` is defined -- `test_decision_mcp.c`'s
     * own tripwire for `atlas_decision_apply_in_tx` carries the identical
     * shape ("one of which is lifecycle.c, which also defines it"). The
     * *caller* count constraints.md and this season's rule describe is two:
     * `src/cli/cli.c` (one caller before A15) and `src/core/service_review.c`
     * (the second, added by this season) -- checked explicitly below by
     * naming both and excluding every other caller. */
    callsite_scan sc2 = {"atlas_service_decision_confirm(", 0u, ATLAS_BUF_INIT};
    walk_sources(ATLAS_SRC_DIR "/src", scan_for_needle, &sc2);
    T_CHECK_MSG(sc2.files_with_calls == 3u,
                "expected exactly 3 files naming the operator channel (1 definition + 2 "
                "callers), found %zu: %s",
                sc2.files_with_calls, atlas_buf_cstr(&sc2.names));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&sc2.names), "/src/core/service_decision.c") != NULL,
                "service_decision.c must still define it: %s", atlas_buf_cstr(&sc2.names));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&sc2.names), "/src/cli/cli.c") != NULL,
                "cli.c must remain a caller: %s", atlas_buf_cstr(&sc2.names));
    T_CHECK_MSG(strstr(atlas_buf_cstr(&sc2.names), "/src/core/service_review.c") != NULL,
                "service_review.c must be the second caller: %s", atlas_buf_cstr(&sc2.names));
    atlas_buf_free(&sc2.names);
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

    /* --- A15 T6 --- */
    {"the machine state this suite depends on",
     test_the_machine_state_this_suite_depends_on},
    {"a sheet on stdin is refused before anything is minted",
     test_a_sheet_on_stdin_is_refused_before_anything_is_minted},
    {"--yes and --json without --check are refused",
     test_yes_and_json_without_check_are_refused},
    {"a two-entry sheet under a pty applies and moves, and the repository is "
     "never modified",
     test_two_entry_sheet_under_a_pty_applies_and_moves},
    {"a mistyped confirmation abandons and leaves the challenge unconsumed",
     test_a_mistyped_confirmation_abandons_and_leaves_the_challenge_unconsumed},
    {"an already-approved record on an approve line is disposed",
     test_an_already_approved_record_on_an_approve_line_is_disposed},
    {"a locked profile refuses before the sheet is opened",
     test_a_locked_profile_refuses_before_the_sheet_is_opened},
    {"--json --check produces exactly one document on every pre-entry refusal",
     test_json_check_produces_exactly_one_document_on_every_pre_entry_refusal},
    {"the --check exit code, the one-operand usage line, and the JSON envelope",
     test_check_exit_code_usage_line_and_envelope},
    {"the human --check totals line, and status:null for a MISSING entry",
     test_human_check_totals_line_and_missing_status_null},
    {"the operator channel's write points have the documented callers",
     test_the_operator_channel_write_points_have_the_documented_callers},
};

ATLAS_TEST_MAIN("review_apply", TESTS)

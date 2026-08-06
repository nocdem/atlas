/* Atlas - terminal safety of real CLI output.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * test_safetext.c proves the encoder is correct. This proves the CLI actually
 * applies it: a repository is built whose filenames, branch name, author identity
 * and commit subject all carry terminal control payloads, every command is run,
 * and no byte that a terminal would interpret is allowed to reach stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas/safetext.h"
#include "atlas_test.h"
#include "support/fixture.h"
#include "support/jsoncheck.h"

#define REPO_NAME "hostile"

/* Payloads a terminal would act on if they were printed raw. */
#define ANSI_COLOUR "\x1b[31m"
#define OSC_TITLE "\x1b]0;pwned\x07"
#define OSC_LINK "\x1b]8;;http://evil.example\x1b\\"
#define CLEAR_SCREEN "\x1b[2J\x1b[H"
#define BIDI_OVERRIDE "\xe2\x80\xae"

typedef struct term_env {
    fixture fx;
} term_env;

typedef struct run_result {
    atlas_buf out;
    atlas_buf errout;
    int exit_code;
} run_result;

static void result_init(run_result *r) {
    memset(r, 0, sizeof(*r));
    atlas_buf_init(&r->out);
    atlas_buf_init(&r->errout);
    r->exit_code = -1;
}

static void result_free(run_result *r) {
    atlas_buf_free(&r->out);
    atlas_buf_free(&r->errout);
}

static void run_atlas(term_env *e, run_result *r, const char *const *args, size_t nargs) {
    atlas_err err;
    atlas_err_init(&err);
    result_init(r);
    const char *argv[24];
    size_t n = 0;
    argv[n++] = "--data-dir";
    argv[n++] = fx_data_dir(&e->fx);
    T_REQUIRE(nargs + n <= sizeof(argv) / sizeof(argv[0]));
    for (size_t i = 0; i < nargs; i++) {
        argv[n++] = args[i];
    }
    T_OK(fx_atlas(argv, n, &r->out, &r->errout, &r->exit_code, &err), &err);
}

/* Searches for the UTF-8 encoding of U+202E without needing a GNU extension. */
static bool contains_bidi_override(const atlas_buf *b) {
    static const unsigned char rlo[] = {0xe2u, 0x80u, 0xaeu};
    if (b->len < sizeof(rlo)) {
        return false;
    }
    for (size_t i = 0; i + sizeof(rlo) <= b->len; i++) {
        if (memcmp(b->data + i, rlo, sizeof(rlo)) == 0) {
            return true;
        }
    }
    return false;
}

/* The core assertion.
 *
 * LF (0x0A) is a C0 control character, and legitimate output contains it: it is
 * how Atlas separates its own lines. The precise claim is therefore not "no C0
 * bytes" but:
 *
 *   ALLOWED   exactly one control byte, LF (0x0A), and only as a line separator
 *             emitted by Atlas itself. Nothing else: not TAB, not CR, not ESC.
 *   FORBIDDEN every other C0 byte (0x00-0x09, 0x0B-0x1F), DEL (0x7F), every C1
 *             control (U+0080-U+009F), and every bidirectional override.
 *
 * LF is not a loophole for untrusted data, because the encoder escapes an LF
 * originating in repository content as "%0A". So every LF in the output is
 * structural by construction, and expect_absent_raw() below closes the loop by
 * requiring the untrusted payload bytes to be absent from the output entirely. */
static void expect_terminal_safe(const atlas_buf *b, const char *what) {
    size_t newlines = 0;
    for (size_t i = 0; i < b->len; i++) {
        unsigned char c = (unsigned char)b->data[i];
        if (c == '\n') {
            newlines++;
            continue; /* the one allowed structural control byte */
        }
        T_CHECK_MSG(c != 0x1bu, "%s: an ESC byte reached the output at offset %zu", what, i);
        T_CHECK_MSG(c != 0x07u, "%s: a BEL byte reached the output at offset %zu", what, i);
        T_CHECK_MSG(c != 0x0du, "%s: a CR byte reached the output at offset %zu", what, i);
        T_CHECK_MSG(c != 0x09u, "%s: a TAB byte reached the output at offset %zu", what, i);
        T_CHECK_MSG(c != 0x7fu, "%s: a DEL byte reached the output at offset %zu", what, i);
        T_CHECK_MSG(c >= 0x20u, "%s: control byte 0x%02x reached the output at offset %zu", what, c,
                    i);
    }
    /* Output is line-structured, so a non-empty result ends with the separator. */
    if (b->len > 0) {
        T_CHECK_MSG(b->data[b->len - 1u] == '\n', "%s: output does not end with a newline", what);
        T_CHECK_MSG(newlines > 0, "%s: no line separator at all", what);
    }
    /* C1 controls are valid UTF-8 but still terminal controls. */
    for (size_t i = 0; i + 1u < b->len; i++) {
        if ((unsigned char)b->data[i] == 0xc2u) {
            unsigned char next = (unsigned char)b->data[i + 1u];
            T_CHECK_MSG(next < 0x80u || next > 0x9fu,
                        "%s: a C1 control (U+00%02X) reached the output", what, next);
        }
    }
    /* Bidirectional overrides would make the output read differently from the
     * bytes it describes. */
    T_CHECK_MSG(!contains_bidi_override(b), "%s: a bidi override reached the output", what);
}

/* Requires that a specific untrusted byte sequence does not appear in the output
 * at all. This is the assertion that makes the LF allowance airtight: it does not
 * matter which control bytes are permitted structurally if the payload itself
 * cannot be found anywhere in the result. */
static void expect_absent_raw(const atlas_buf *b, const void *payload, size_t n,
                              const char *what) {
    if (n == 0 || b->len < n) {
        return;
    }
    for (size_t i = 0; i + n <= b->len; i++) {
        if (memcmp(b->data + i, payload, n) == 0) {
            atlas_test_fail(__FILE__, __LINE__,
                            "%s: the raw untrusted payload appears verbatim at offset %zu", what,
                            i);
            return;
        }
    }
}

/* Builds a repository in which every piece of metadata is hostile. */
static bool build_hostile(term_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    const char *repo = fx_repo(&e->fx);
    T_OK(fx_init_repo(&e->fx, repo, NULL, &err), &err);

    /* A filename carrying an ANSI colour sequence, and one carrying an OSC
     * title-setting payload. */
    static const char colour_name[] = ANSI_COLOUR "red.c";
    static const char osc_name[] = OSC_TITLE "title.c";
    bool have_hostile_names = fx_can_create_name(repo, colour_name, sizeof(colour_name) - 1u) &&
                              fx_can_create_name(repo, osc_name, sizeof(osc_name) - 1u);
    if (have_hostile_names) {
        T_OK(fx_write_bytes(repo, colour_name, sizeof(colour_name) - 1u, "red\n", 4u, 0644, &err),
             &err);
        T_OK(fx_write_bytes(repo, osc_name, sizeof(osc_name) - 1u, "osc\n", 4u, 0644, &err), &err);
    } else {
        atlas_test_note("this filesystem refuses escape bytes in filenames; names skipped");
    }
    /* A carriage return would let a filename overwrite an already-printed line. */
    static const char cr_name[] = "safe.txt\rFAKE OK";
    if (fx_can_create_name(repo, cr_name, sizeof(cr_name) - 1u)) {
        T_OK(fx_write_bytes(repo, cr_name, sizeof(cr_name) - 1u, "cr\n", 3u, 0644, &err), &err);
    }
    /* A bidi override in a filename. */
    static const char bidi_name[] = BIDI_OVERRIDE "gnp.txt";
    if (fx_can_create_name(repo, bidi_name, sizeof(bidi_name) - 1u)) {
        T_OK(fx_write_bytes(repo, bidi_name, sizeof(bidi_name) - 1u, "bidi\n", 5u, 0644, &err),
             &err);
    }
    T_OK(fx_write(repo, "plain.c", "int main(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, repo, &err), &err);

    /* A commit subject and body carrying colour, a screen clear and a hyperlink. */
    T_OK(fx_commit_body(&e->fx, repo, ANSI_COLOUR "subject in red" CLEAR_SCREEN,
                        OSC_LINK "body with a hyperlink" OSC_TITLE, &err),
         &err);
    return have_hostile_names;
}

/* git refuses some author names, so this is attempted and reported, not required. */
static bool add_hostile_author_commit(term_env *e) {
    atlas_err err;
    atlas_err_init(&err);
    const char *repo = fx_repo(&e->fx);
    T_OK(fx_write(repo, "second.c", "second\n", &err), &err);
    T_OK(fx_add_all(&e->fx, repo, &err), &err);

    const char *args[] = {"commit",  "-q", "--allow-empty", "-m",
                          ANSI_COLOUR "second subject",
                          "--author", ANSI_COLOUR "Red Author <red@example.org>"};
    int code = 0;
    T_OK(fx_git(&e->fx, repo, args, 7u, &code, NULL, &err), &err);
    if (code != 0) {
        atlas_test_note("this git refused an author name containing ESC; author payload skipped");
        /* Commit anyway so the tree is consistent. */
        T_OK(fx_commit(&e->fx, repo, "second subject", &err), &err);
        return false;
    }
    return true;
}

static void test_human_output_is_terminal_safe(void) {
    term_env e;
    atlas_err err;
    atlas_err_init(&err);
    memset(&e, 0, sizeof(e));
    T_OK(fx_open(&e.fx, &err), &err);
    (void)build_hostile(&e);
    (void)add_hostile_author_commit(&e);

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas(&e, &r, add, 5u);
    T_CHECK_MSG(r.exit_code == 0, "repo add failed: %s", atlas_buf_cstr(&r.errout));
    expect_terminal_safe(&r.out, "repo add");
    result_free(&r);

    /* A branch name carrying an escape sequence, if this git accepts one. */
    static const char branch[] = ANSI_COLOUR "hostile-branch";
    const char *co[] = {"checkout", "-q", "-b", branch};
    int code = 0;
    T_OK(fx_git(&e.fx, fx_repo(&e.fx), co, 4u, &code, NULL, &err), &err);
    if (code != 0) {
        atlas_test_note("this git refused a branch name containing ESC; branch payload skipped");
    }

    /* An untracked file whose name is hostile, so diff must encode it too. */
    static const char untracked[] = OSC_TITLE "untracked.txt";
    if (fx_can_create_name(fx_repo(&e.fx), untracked, sizeof(untracked) - 1u)) {
        T_OK(fx_write_bytes(fx_repo(&e.fx), untracked, sizeof(untracked) - 1u, "u\n", 2u, 0644,
                            &err),
             &err);
    }

    struct {
        const char *args[4];
        size_t n;
        const char *what;
    } cases[] = {
        {{"scan", REPO_NAME}, 2u, "scan"},
        {{"repo", "list"}, 2u, "repo list"},
        {{"status", REPO_NAME}, 2u, "status"},
        {{"search", REPO_NAME, "subject"}, 3u, "search"},
        {{"search", REPO_NAME, "red"}, 3u, "search red"},
        {{"history", REPO_NAME, "plain.c"}, 3u, "history"},
        {{"file", REPO_NAME, "plain.c"}, 3u, "file"},
        {{"diff", REPO_NAME}, 2u, "diff"},
        {{"doctor"}, 1u, "doctor"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_atlas(&e, &r, cases[i].args, cases[i].n);
        T_CHECK_MSG(r.exit_code == 0, "%s exited %d: %s", cases[i].what, r.exit_code,
                    atlas_buf_cstr(&r.errout));
        expect_terminal_safe(&r.out, cases[i].what);
        expect_terminal_safe(&r.errout, cases[i].what);
        /* And the payload bytes themselves appear nowhere, so the LF allowance
         * above cannot be a loophole. */
        expect_absent_raw(&r.out, ANSI_COLOUR, sizeof(ANSI_COLOUR) - 1u, cases[i].what);
        expect_absent_raw(&r.out, OSC_TITLE, sizeof(OSC_TITLE) - 1u, cases[i].what);
        expect_absent_raw(&r.out, CLEAR_SCREEN, sizeof(CLEAR_SCREEN) - 1u, cases[i].what);
        expect_absent_raw(&r.out, OSC_LINK, sizeof(OSC_LINK) - 1u, cases[i].what);
        expect_absent_raw(&r.out, BIDI_OVERRIDE, sizeof(BIDI_OVERRIDE) - 1u, cases[i].what);
        expect_absent_raw(&r.out, "\rFAKE OK", 8u, cases[i].what);
        result_free(&r);
    }

    /* The encoded forms are what appear instead. */
    const char *search[] = {"search", REPO_NAME, "subject"};
    run_atlas(&e, &r, search, 3u);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&r.out), "%1B") != NULL,
                "the escape byte should appear percent-escaped: %s", atlas_buf_cstr(&r.out));
    result_free(&r);

    fx_close(&e.fx);
}

static void test_json_output_is_safe_and_reversible(void) {
    term_env e;
    atlas_err err;
    atlas_err_init(&err);
    memset(&e, 0, sizeof(e));
    T_OK(fx_open(&e.fx, &err), &err);
    (void)build_hostile(&e);

    run_result r;
    const char *add[] = {"repo", "add", fx_repo(&e.fx), "--name", REPO_NAME};
    run_atlas(&e, &r, add, 5u);
    T_CHECK_MSG(r.exit_code == 0, "repo add failed: %s", atlas_buf_cstr(&r.errout));
    result_free(&r);
    const char *scan[] = {"scan", REPO_NAME};
    run_atlas(&e, &r, scan, 2u);
    T_EQ_INT(r.exit_code, 0);
    result_free(&r);

    struct {
        const char *args[5];
        size_t n;
        const char *what;
    } cases[] = {
        {{"--json", "doctor"}, 2u, "doctor"},
        {{"--json", "repo", "list"}, 3u, "repo list"},
        {{"--json", "status", REPO_NAME}, 3u, "status"},
        {{"--json", "search", REPO_NAME, "subject"}, 4u, "search"},
        {{"--json", "history", REPO_NAME, "plain.c"}, 4u, "history"},
        {{"--json", "file", REPO_NAME, "plain.c"}, 4u, "file"},
        {{"--json", "diff", REPO_NAME}, 3u, "diff"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_atlas(&e, &r, cases[i].args, cases[i].n);
        T_CHECK_MSG(r.exit_code == 0, "%s exited %d: %s", cases[i].what, r.exit_code,
                    atlas_buf_cstr(&r.errout));
        /* Still a valid document, and still safe to print. */
        size_t bad = 0;
        T_CHECK_MSG(tjson_valid(r.out.data, r.out.len, &bad),
                    "%s: invalid JSON at offset %zu", cases[i].what, bad);
        expect_terminal_safe(&r.out, cases[i].what);
        /* The contract marker names the encoding a consumer must reverse. */
        atlas_buf enc = ATLAS_BUF_INIT;
        T_CHECK(tjson_get_string(r.out.data, r.out.len, "text_encoding", &enc));
        T_EQ_STR(atlas_buf_cstr(&enc), ATLAS_TEXT_ENCODING_NAME);
        atlas_buf_free(&enc);
        result_free(&r);
    }

    /* The commit subject survives as a reversible encoding of the exact bytes.
     * "red" is searched rather than "subject" because the escape byte is a token
     * separator, so "subject" is part of the token "31msubject". */
    const char *search[] = {"--json", "search", REPO_NAME, "red"};
    run_atlas(&e, &r, search, 4u);
    T_EQ_INT(r.exit_code, 0);
    atlas_buf subject = ATLAS_BUF_INIT;
    if (tjson_get_string(r.out.data, r.out.len, "subject", &subject)) {
        atlas_buf decoded = ATLAS_BUF_INIT;
        T_OK(atlas_text_decode_safe(atlas_buf_cstr(&subject), subject.len, &decoded, &err), &err);
        /* Decoding recovers the original escape bytes exactly. */
        T_CHECK_MSG(memcmp(decoded.data, ANSI_COLOUR "subject in red",
                           strlen(ANSI_COLOUR "subject in red")) == 0,
                    "the subject did not round-trip: %s", atlas_buf_cstr(&subject));
        atlas_buf_free(&decoded);
    } else {
        atlas_test_fail(__FILE__, __LINE__, "no subject field in search output");
    }
    atlas_buf_free(&subject);
    result_free(&r);

    fx_close(&e.fx);
}

static void test_error_messages_are_safe(void) {
    term_env e;
    atlas_err err;
    atlas_err_init(&err);
    memset(&e, 0, sizeof(e));
    T_OK(fx_open(&e.fx, &err), &err);

    /* An error message quoting a hostile repository name must be encoded too. */
    run_result r;
    static const char nasty_name[] = ANSI_COLOUR "nope" OSC_TITLE;
    const char *args[] = {"status", nasty_name};
    run_atlas(&e, &r, args, 2u);
    T_CHECK(r.exit_code != 0);
    expect_terminal_safe(&r.out, "error stdout");
    expect_terminal_safe(&r.errout, "error stderr");
    result_free(&r);

    /* And in JSON mode the error document stays valid and safe. */
    const char *jargs[] = {"--json", "status", nasty_name};
    run_atlas(&e, &r, jargs, 3u);
    T_CHECK(r.exit_code != 0);
    size_t bad = 0;
    T_CHECK_MSG(tjson_valid(r.out.data, r.out.len, &bad), "invalid JSON error document at %zu", bad);
    expect_terminal_safe(&r.out, "json error");
    result_free(&r);

    fx_close(&e.fx);
}

static const atlas_test TESTS[] = {
    {"human output is terminal safe", test_human_output_is_terminal_safe},
    {"json output is safe and reversible", test_json_output_is_safe_and_reversible},
    {"error messages are terminal safe", test_error_messages_are_safe},
};

ATLAS_TEST_MAIN("terminal", TESTS)

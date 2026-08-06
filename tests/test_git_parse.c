/* Atlas - git output parser tests, including adversarial input (required test 32).
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * These tests drive the parsers directly with byte streams, so hostile output can
 * be exercised without needing a repository that produces it.
 */
#include <stdlib.h>
#include <string.h>

#include "atlas/atlas.h"
#include "atlas_test.h"
#include "git/git_parse.h"

/* --- helpers ------------------------------------------------------------- */

typedef struct captured_tokens {
    atlas_buf joined; /* tokens separated by '|' for easy assertions */
    int count;
} captured_tokens;

static atlas_status collect_token(const char *tok, size_t len, void *ud, atlas_err *err) {
    captured_tokens *c = (captured_tokens *)ud;
    if (c->count > 0) {
        atlas_status st = atlas_buf_append_ch(&c->joined, '|', err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    c->count++;
    return atlas_buf_append(&c->joined, tok, len, err);
}

/* Feeds `data` through the splitter in fixed-size chunks so boundary handling is
 * exercised, then returns the result of finish(). */
static atlas_status feed_chunked(atlas_nulsplit *s, const char *data, size_t n, size_t chunk,
                                 atlas_err *err) {
    for (size_t off = 0; off < n; off += chunk) {
        size_t take = (n - off) < chunk ? (n - off) : chunk;
        atlas_status st = atlas_nulsplit_feed(s, data + off, take, err);
        if (st != ATLAS_OK) {
            return st;
        }
    }
    return ATLAS_OK;
}

/* --- NUL splitter -------------------------------------------------------- */

static void test_nulsplit_chunking(void) {
    static const char data[] = "alpha\0beta\0\0gamma\0";
    const size_t n = sizeof(data) - 1u;

    for (size_t chunk = 1; chunk <= n + 1u; chunk++) {
        atlas_err err;
        atlas_err_init(&err);
        captured_tokens cap;
        memset(&cap, 0, sizeof(cap));
        atlas_buf_init(&cap.joined);
        atlas_nulsplit s;
        atlas_nulsplit_init(&s, 0, collect_token, &cap);

        T_OK(feed_chunked(&s, data, n, chunk, &err), &err);
        T_OK(atlas_nulsplit_finish(&s, &err), &err);
        /* An empty record between two NULs is a real, preserved record. */
        T_CHECK_MSG(strcmp(atlas_buf_cstr(&cap.joined), "alpha|beta||gamma") == 0,
                    "chunk %zu produced \"%s\"", chunk, atlas_buf_cstr(&cap.joined));
        T_EQ_INT(cap.count, 4);
        atlas_buf_free(&cap.joined);
        atlas_nulsplit_free(&s);
    }
}

static void test_nulsplit_unterminated_tail(void) {
    atlas_err err;
    atlas_err_init(&err);
    captured_tokens cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.joined);
    atlas_nulsplit s;
    atlas_nulsplit_init(&s, 0, collect_token, &cap);

    T_OK(atlas_nulsplit_feed(&s, "done\0partial", 12u, &err), &err);
    /* Truncated output must be an error: git always terminates its records. */
    T_FAILS_WITH(atlas_nulsplit_finish(&s, &err), ATLAS_ERR_GIT, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "mid-record") != NULL);
    atlas_buf_free(&cap.joined);
    atlas_nulsplit_free(&s);
}

static void test_nulsplit_token_ceiling(void) {
    atlas_err err;
    atlas_err_init(&err);
    captured_tokens cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.joined);
    atlas_nulsplit s;
    atlas_nulsplit_init(&s, 16u, collect_token, &cap);

    /* A record larger than the ceiling fails instead of growing memory. */
    char big[64];
    memset(big, 'x', sizeof(big));
    T_FAILS_WITH(atlas_nulsplit_feed(&s, big, sizeof(big), &err), ATLAS_ERR_GIT, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "limit") != NULL);
    atlas_buf_free(&cap.joined);
    atlas_nulsplit_free(&s);
}

/* --- ls-files ------------------------------------------------------------ */

typedef struct index_capture {
    atlas_buf log;
    int count;
} index_capture;

static atlas_status on_index(const atlas_git_index_entry *e, void *ud, atlas_err *err) {
    index_capture *c = (index_capture *)ud;
    c->count++;
    atlas_status st = atlas_buf_appendf(&c->log, err, "[%s %s %d ", e->mode, e->oid, e->stage);
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&c->log, e->path, e->path_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&c->log, ']', err);
    }
    return st;
}

static atlas_status run_lsfiles(const char *data, size_t n, index_capture *cap, atlas_err *err) {
    atlas_lsfiles_parser lp;
    memset(&lp, 0, sizeof(lp));
    lp.cb = on_index;
    lp.ud = cap;
    atlas_nulsplit s;
    atlas_nulsplit_init(&s, 0, atlas_lsfiles_token, &lp);
    atlas_status st = atlas_nulsplit_feed(&s, data, n, err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&s, err);
    }
    atlas_nulsplit_free(&s);
    return st;
}

#define OID40 "5626abf0f72e58d7a153368ba57db4c673c0e171"
#define OID64 "5626abf0f72e58d7a153368ba57db4c673c0e1715626abf0f72e58d7a153368b"

static void test_lsfiles_valid(void) {
    atlas_err err;
    atlas_err_init(&err);
    index_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.log);

    /* Paths containing spaces, tabs and newlines must survive intact: the record
     * is split on the first tab only, never on whitespace in the path. */
    static const char data[] = "100644 " OID40 " 0\twith space.txt\0"
                               "100755 " OID40 " 0\tscript.sh\0"
                               "120000 " OID40 " 0\tlink\0"
                               "100644 " OID40 " 0\twith\ttab.txt\0"
                               "100644 " OID40 " 0\twith\nnewline.txt\0"
                               "100644 " OID64 " 0\tsha256-repo.txt\0";
    T_OK(run_lsfiles(data, sizeof(data) - 1u, &cap, &err), &err);
    T_EQ_INT(cap.count, 6);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[100644 " OID40 " 0 with space.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[100644 " OID40 " 0 with\ttab.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[100644 " OID40 " 0 with\nnewline.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[120000 " OID40 " 0 link]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[100644 " OID64 " 0 sha256-repo.txt]") != NULL);
    atlas_buf_free(&cap.log);
}

static void expect_lsfiles_rejects(const char *record, const char *why) {
    atlas_err err;
    atlas_err_init(&err);
    index_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.log);
    /* One record plus its terminator. */
    size_t n = strlen(record) + 1u;
    atlas_status st = run_lsfiles(record, n, &cap, &err);
    T_CHECK_MSG(st == ATLAS_ERR_GIT, "%s: expected a git parse error, got %s (%s)", why,
                atlas_status_name(st), atlas_err_msg(&err));
    atlas_buf_free(&cap.log);
}

static void test_lsfiles_malformed(void) {
    expect_lsfiles_rejects("", "empty record");
    expect_lsfiles_rejects("100644 " OID40 " 0 no-tab.txt", "no tab separator");
    expect_lsfiles_rejects("100644\t" OID40, "no mode separator");
    expect_lsfiles_rejects("100644 " OID40 "\tmissing-stage.txt", "no stage separator");
    expect_lsfiles_rejects("10064 " OID40 " 0\tshort-mode.txt", "mode too short");
    expect_lsfiles_rejects("1006x4 " OID40 " 0\tnon-octal-mode.txt", "non-octal mode");
    expect_lsfiles_rejects("100644 nothex 0\tbad-oid.txt", "oid not hex");
    expect_lsfiles_rejects("100644 " OID40 " 9\tbad-stage.txt", "stage out of range");
    expect_lsfiles_rejects("100644 " OID40 " x\tnon-numeric-stage.txt", "non-numeric stage");
    expect_lsfiles_rejects("100644 " OID40 " 0\t", "empty path");
}

/* --- log ---------------------------------------------------------------- */

typedef struct log_capture {
    atlas_buf commits;
    atlas_buf changes;
    int commit_count;
    int change_count;
} log_capture;

static atlas_status on_log_commit(const atlas_git_commit *c, void *ud, atlas_err *err) {
    log_capture *lc = (log_capture *)ud;
    lc->commit_count++;
    return atlas_buf_appendf(&lc->commits, err, "[%s p=%d parents=%s an=%s ae=%s at=%lld ct=%lld s=%s]",
                             c->oid, c->parent_count, c->parents, c->author_name, c->author_email,
                             (long long)c->author_time, (long long)c->commit_time, c->subject);
}

static atlas_status on_log_change(const atlas_git_commit *c, const atlas_git_change *ch, void *ud,
                                  atlas_err *err) {
    log_capture *lc = (log_capture *)ud;
    lc->change_count++;
    atlas_status st = atlas_buf_appendf(&lc->changes, err, "[%s %s %s ", c->oid, ch->raw_status,
                                        atlas_git_change_type_name(ch->kind));
    if (st == ATLAS_OK && ch->old_path != NULL) {
        st = atlas_buf_append(&lc->changes, ch->old_path, ch->old_path_len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&lc->changes, "->", err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&lc->changes, ch->path, ch->path_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&lc->changes, ']', err);
    }
    return st;
}

static atlas_status run_log(const char *data, size_t n, log_capture *cap, atlas_err *err) {
    atlas_log_parser lp;
    atlas_log_parser_init(&lp, on_log_commit, on_log_change, cap);
    atlas_nulsplit s;
    atlas_nulsplit_init(&s, 0, atlas_log_token, &lp);
    atlas_status st = atlas_nulsplit_feed(&s, data, n, err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&s, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_log_parser_finish(&lp, err);
    }
    atlas_nulsplit_free(&s);
    atlas_log_parser_free(&lp);
    return st;
}

#define OID_A "418b681e1241df1a0be87601908c95029ab0444c"
#define OID_C "1863bdf335bda57ada197281c266167a92466d84"

/* The exact framing git produces: a sentinel-prefixed header record, then
 * name-status records where the first carries a leading newline. */
static void test_log_full_sequence(void) {
    atlas_err err;
    atlas_err_init(&err);
    log_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.commits);
    atlas_buf_init(&cap.changes);

    static const char data[] =
        "\001" OID_A "\037" OID_C "\037Ada L\037ada@example.org\0371786014580\0371786014581"
        "\037second: rename and more\n\0"
        "\nM\0b c.txt\0"
        "A\0new.txt\0"
        "R100\0a.txt\0renamed.txt\0"
        "D\0sub/d.txt\0"
        "\001" OID_C "\037\037Ada L\037ada@example.org\0371786014520\0371786014520"
        "\037first commit\n\nbody line1\nbody line2\n\0"
        "\nA\0a.txt\0"
        "A\0b c.txt\0";

    T_OK(run_log(data, sizeof(data) - 1u, &cap, &err), &err);
    T_EQ_INT(cap.commit_count, 2);
    T_EQ_INT(cap.change_count, 6);

    /* Header fields are parsed, including a root commit with no parents. */
    T_CHECK(strstr(atlas_buf_cstr(&cap.commits),
                   "[" OID_A " p=1 parents=" OID_C " an=Ada L ae=ada@example.org "
                   "at=1786014580 ct=1786014581 s=second: rename and more]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.commits), "[" OID_C " p=0 parents= ") != NULL);
    /* Subject is the first line only. */
    T_CHECK(strstr(atlas_buf_cstr(&cap.commits), "s=first commit]") != NULL);

    /* Change types, rename pairing and a path containing a space. */
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " M modify b c.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " A add new.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes),
                   "[" OID_A " R100 rename a.txt->renamed.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " D delete sub/d.txt]") != NULL);

    atlas_buf_free(&cap.commits);
    atlas_buf_free(&cap.changes);
}

static void test_log_hostile_paths(void) {
    atlas_err err;
    atlas_err_init(&err);
    log_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.commits);
    atlas_buf_init(&cap.changes);

    /* A filename may begin with a newline or look exactly like a status code.
     * Path operands are positional, so neither can be misread as a record type. */
    static const char data[] =
        "\001" OID_A "\037\037A\037a@e\0371\0372\037subject\n\0"
        "\nA\0\nleading-newline.txt\0"
        "A\0M\0"
        "A\0R100\0"
        "C75\0src/orig.c\0src/copy.c\0";

    T_OK(run_log(data, sizeof(data) - 1u, &cap, &err), &err);
    T_EQ_INT(cap.change_count, 4);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " A add \nleading-newline.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " A add M]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " A add R100]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.changes), "[" OID_A " C75 copy src/orig.c->src/copy.c]") !=
            NULL);
    atlas_buf_free(&cap.commits);
    atlas_buf_free(&cap.changes);
}

static void expect_log_rejects(const char *data, size_t n, const char *why) {
    atlas_err err;
    atlas_err_init(&err);
    log_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.commits);
    atlas_buf_init(&cap.changes);
    atlas_status st = run_log(data, n, &cap, &err);
    T_CHECK_MSG(st == ATLAS_ERR_GIT, "%s: expected a git parse error, got %s (%s)", why,
                atlas_status_name(st), atlas_err_msg(&err));
    atlas_buf_free(&cap.commits);
    atlas_buf_free(&cap.changes);
}

static void test_log_malformed(void) {
    /* Truncated header: only three of the six separators present. */
    static const char short_header[] = "\001" OID_A "\037\037A\037a@e\0";
    expect_log_rejects(short_header, sizeof(short_header) - 1u, "missing header fields");

    /* Object id that is not hex of a supported length. */
    static const char bad_oid[] = "\001nothex\037\037A\037a@e\0371\0372\037s\n\0";
    expect_log_rejects(bad_oid, sizeof(bad_oid) - 1u, "malformed object id");

    /* A field separator inside the author name shifts the timestamp fields,
     * which validation catches instead of silently mis-parsing. */
    static const char shifted[] = "\001" OID_A "\037\037A\037da\037a@e\0371\0372\037s\n\0";
    expect_log_rejects(shifted, sizeof(shifted) - 1u, "separator inside a field");

    /* Non-numeric timestamp. */
    static const char bad_time[] = "\001" OID_A "\037\037A\037a@e\037notanumber\0372\037s\n\0";
    expect_log_rejects(bad_time, sizeof(bad_time) - 1u, "malformed timestamp");

    /* A name-status entry before any commit header. */
    static const char orphan[] = "\nM\0some/path.txt\0";
    expect_log_rejects(orphan, sizeof(orphan) - 1u, "change before any commit");

    /* An unrecognised record type. */
    static const char junk[] = "\001" OID_A "\037\037A\037a@e\0371\0372\037s\n\0"
                               "\nZZZZZZ\0path\0";
    expect_log_rejects(junk, sizeof(junk) - 1u, "unknown record");

    /* Output that ends while a rename is still awaiting its second path. */
    static const char truncated_rename[] =
        "\001" OID_A "\037\037A\037a@e\0371\0372\037s\n\0"
        "\nR100\0only-one-path.txt\0";
    expect_log_rejects(truncated_rename, sizeof(truncated_rename) - 1u, "incomplete rename pair");

    /* An empty path operand. */
    static const char empty_path[] = "\001" OID_A "\037\037A\037a@e\0371\0372\037s\n\0"
                                     "\nM\0\0";
    expect_log_rejects(empty_path, sizeof(empty_path) - 1u, "empty path operand");
}

/* --- status --porcelain=v2 ---------------------------------------------- */

/* Collects the entries the status parser emits, so both the summary counts and the
 * individual facts can be asserted. */
typedef struct status_capture {
    atlas_buf log;
    int count;
} status_capture;

static atlas_status on_status_entry(const atlas_git_status_entry *e, void *ud, atlas_err *err) {
    status_capture *c = (status_capture *)ud;
    c->count++;
    atlas_status st = atlas_buf_appendf(&c->log, err, "[%s %c ", atlas_change_scope_name(e->scope),
                                        e->status);
    if (st == ATLAS_OK && e->old_path != NULL) {
        st = atlas_buf_append(&c->log, e->old_path, e->old_path_len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&c->log, "->", err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&c->log, e->path, e->path_len, err);
    }
    if (st == ATLAS_OK && e->score_known) {
        st = atlas_buf_appendf(&c->log, err, " %d%%", e->score);
    }
    if (st == ATLAS_OK && e->is_directory) {
        st = atlas_buf_append_str(&c->log, " dir", err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&c->log, ']', err);
    }
    return st;
}

static atlas_status run_status_cap(const char *data, size_t n, atlas_git_worktree_state *out,
                                   status_capture *cap, atlas_err *err) {
    memset(out, 0, sizeof(*out));
    atlas_status_parser sp;
    atlas_status_parser_init(&sp, out, cap != NULL ? on_status_entry : NULL, cap);
    atlas_nulsplit s;
    atlas_nulsplit_init(&s, 0, atlas_status_token, &sp);
    atlas_status st = atlas_nulsplit_feed(&s, data, n, err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&s, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_status_parser_finish(&sp, err);
    }
    atlas_nulsplit_free(&s);
    atlas_status_parser_free(&sp);
    return st;
}

static atlas_status run_status(const char *data, size_t n, atlas_git_worktree_state *out,
                               atlas_err *err) {
    return run_status_cap(data, n, out, NULL, err);
}

static void test_status_clean(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git_worktree_state wt;
    static const char data[] = "# branch.oid " OID_A "\0# branch.head main\0";
    T_OK(run_status(data, sizeof(data) - 1u, &wt, &err), &err);
    T_CHECK(!wt.dirty);
    T_EQ_STR(wt.oid, OID_A);
    T_EQ_STR(wt.branch, "main");
    T_CHECK(!wt.unborn);
}

static void test_status_dirty_and_rename(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git_worktree_state wt;
    /* One unstaged change, one staged change, a rename (whose original path
     * arrives as a separate record), an unmerged entry and an untracked file. */
    static const char data[] = "# branch.oid " OID_A "\0# branch.head main\0"
                               "1 .M N... 100644 100644 100644 " OID40 " " OID40 " a.txt\0"
                               "1 M. N... 100644 100644 100644 " OID40 " " OID40 " staged.txt\0"
                               /* RM: renamed in the index and then modified again on
                                  disk, so both sides are true at once. */
                               "2 RM N... 100644 100644 100644 " OID40 " " OID40 " R100 new.txt\0"
                               "old.txt\0"
                               "u UU N... 100644 100644 100644 100644 " OID40 " " OID40 " " OID40
                               " conflict.txt\0"
                               "? untracked.txt\0"
                               "! ignored.txt\0";
    status_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.log);
    T_OK(run_status_cap(data, sizeof(data) - 1u, &wt, &cap, &err), &err);
    T_CHECK(wt.dirty);
    /* a.txt was modified on disk, and so was the renamed path. */
    T_EQ_INT(wt.unstaged, 2);
    /* The staged column counts the staged change and the rename. */
    T_EQ_INT(wt.staged, 2);
    T_EQ_INT(wt.unmerged, 1);
    T_EQ_INT(wt.untracked, 1);
    T_EQ_STR(wt.branch, "main");

    /* Each side of a record is a separate fact. The rename is staged and carries
     * its origin path; the extra unstaged edit to the same path does not. */
    const char *log = atlas_buf_cstr(&cap.log);
    T_CHECK_MSG(strstr(log, "[unstaged M a.txt]") != NULL, "log was: %s", log);
    T_CHECK_MSG(strstr(log, "[staged M staged.txt]") != NULL, "log was: %s", log);
    T_CHECK_MSG(strstr(log, "[staged R old.txt->new.txt 100%]") != NULL, "log was: %s", log);
    T_CHECK_MSG(strstr(log, "[unstaged M new.txt]") != NULL, "log was: %s", log);
    T_CHECK_MSG(strstr(log, "[unmerged U conflict.txt]") != NULL, "log was: %s", log);
    T_CHECK_MSG(strstr(log, "[untracked ? untracked.txt]") != NULL, "log was: %s", log);
    /* Ignored files are not reported at all. */
    T_CHECK(strstr(log, "ignored.txt") == NULL);
    T_EQ_INT(cap.count, 6);
    atlas_buf_free(&cap.log);
}

/* An untracked directory and hostile untracked paths. */
static void test_status_untracked_shapes(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git_worktree_state wt;
    status_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.log);

    static const char data[] = "# branch.oid " OID_A "\0# branch.head main\0"
                               "? buildout/\0"
                               "? with space.txt\0"
                               "? with\ttab.txt\0"
                               "? \nleading-newline.txt\0";
    T_OK(run_status_cap(data, sizeof(data) - 1u, &wt, &cap, &err), &err);
    T_EQ_INT(wt.untracked, 4);
    const char *log = atlas_buf_cstr(&cap.log);
    /* A collapsed directory is flagged rather than descended into. */
    T_CHECK_MSG(strstr(log, "[untracked ? buildout/ dir]") != NULL, "log was: %s", log);
    /* Paths are taken verbatim, including whitespace and newlines. */
    T_CHECK(strstr(log, "[untracked ? with space.txt]") != NULL);
    T_CHECK(strstr(log, "[untracked ? with\ttab.txt]") != NULL);
    T_CHECK(strstr(log, "[untracked ? \nleading-newline.txt]") != NULL);
    atlas_buf_free(&cap.log);
}

/* A rename whose origin-path record never arrives must be an error, not a
 * silently dropped entry. */
static void test_status_truncated_rename(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git_worktree_state wt;
    static const char data[] = "# branch.head main\0"
                               "2 R. N... 100644 100644 100644 " OID40 " " OID40 " R100 new.txt\0";
    T_FAILS_WITH(run_status(data, sizeof(data) - 1u, &wt, &err), ATLAS_ERR_GIT, &err);
    T_CHECK(strstr(atlas_err_msg(&err), "origin path") != NULL);
}

static void test_status_unborn_and_detached(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git_worktree_state wt;
    static const char unborn[] = "# branch.oid (initial)\0# branch.head main\0";
    T_OK(run_status(unborn, sizeof(unborn) - 1u, &wt, &err), &err);
    T_CHECK(wt.unborn);
    T_EQ_STR(wt.oid, "");

    static const char detached[] = "# branch.oid " OID_A "\0# branch.head (detached)\0";
    T_OK(run_status(detached, sizeof(detached) - 1u, &wt, &err), &err);
    T_EQ_STR(wt.branch, "");
    T_CHECK(!wt.unborn);
}

static void test_status_malformed(void) {
    atlas_err err;
    atlas_err_init(&err);
    atlas_git_worktree_state wt;

    static const char bad_oid[] = "# branch.oid zzzz\0";
    T_FAILS_WITH(run_status(bad_oid, sizeof(bad_oid) - 1u, &wt, &err), ATLAS_ERR_GIT, &err);

    static const char truncated_entry[] = "1 .\0";
    T_FAILS_WITH(run_status(truncated_entry, sizeof(truncated_entry) - 1u, &wt, &err),
                 ATLAS_ERR_GIT, &err);

    static const char unknown[] = "Q something\0";
    T_FAILS_WITH(run_status(unknown, sizeof(unknown) - 1u, &wt, &err), ATLAS_ERR_GIT, &err);

    /* A branch name longer than the field must be rejected, not truncated. */
    atlas_buf longbranch = ATLAS_BUF_INIT;
    T_OK(atlas_buf_append_str(&longbranch, "# branch.head ", &err), &err);
    for (int i = 0; i < 400; i++) {
        T_OK(atlas_buf_append_ch(&longbranch, 'b', &err), &err);
    }
    T_OK(atlas_buf_append_ch(&longbranch, '\0', &err), &err);
    T_FAILS_WITH(run_status(longbranch.data, longbranch.len, &wt, &err), ATLAS_ERR_GIT, &err);
    atlas_buf_free(&longbranch);
}

/* --- diff --numstat ----------------------------------------------------- */

typedef struct numstat_capture {
    atlas_buf log;
    int count;
} numstat_capture;

static atlas_status on_numstat(const atlas_git_diff_entry *e, void *ud, atlas_err *err) {
    numstat_capture *c = (numstat_capture *)ud;
    c->count++;
    atlas_status st;
    if (e->binary) {
        st = atlas_buf_append_str(&c->log, "[bin ", err);
    } else {
        st = atlas_buf_appendf(&c->log, err, "[%lld/%lld ", (long long)e->added,
                               (long long)e->deleted);
    }
    if (st == ATLAS_OK && e->old_path != NULL) {
        st = atlas_buf_append(&c->log, e->old_path, e->old_path_len, err);
        if (st == ATLAS_OK) {
            st = atlas_buf_append_str(&c->log, "->", err);
        }
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append(&c->log, e->path, e->path_len, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_buf_append_ch(&c->log, ']', err);
    }
    return st;
}

static atlas_status run_numstat(const char *data, size_t n, numstat_capture *cap, atlas_err *err) {
    atlas_numstat_parser np;
    atlas_numstat_parser_init(&np, on_numstat, cap);
    atlas_nulsplit s;
    atlas_nulsplit_init(&s, 0, atlas_numstat_token, &np);
    atlas_status st = atlas_nulsplit_feed(&s, data, n, err);
    if (st == ATLAS_OK) {
        st = atlas_nulsplit_finish(&s, err);
    }
    if (st == ATLAS_OK) {
        st = atlas_numstat_parser_finish(&np, err);
    }
    atlas_nulsplit_free(&s);
    atlas_numstat_parser_free(&np);
    return st;
}

static void test_numstat_parsing(void) {
    atlas_err err;
    atlas_err_init(&err);
    numstat_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.log);

    static const char data[] = "12\t3\tsrc/a.c\0"
                               "0\t0\twith space.txt\0"
                               "-\t-\timage.png\0"
                               "5\t5\t\0old/name.c\0new/name.c\0";
    T_OK(run_numstat(data, sizeof(data) - 1u, &cap, &err), &err);
    T_EQ_INT(cap.count, 4);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[12/3 src/a.c]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[0/0 with space.txt]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[bin image.png]") != NULL);
    T_CHECK(strstr(atlas_buf_cstr(&cap.log), "[5/5 old/name.c->new/name.c]") != NULL);
    atlas_buf_free(&cap.log);
}

static void test_numstat_malformed(void) {
    atlas_err err;
    atlas_err_init(&err);
    numstat_capture cap;
    memset(&cap, 0, sizeof(cap));
    atlas_buf_init(&cap.log);

    static const char no_tab[] = "12 3 src/a.c\0";
    T_FAILS_WITH(run_numstat(no_tab, sizeof(no_tab) - 1u, &cap, &err), ATLAS_ERR_GIT, &err);

    static const char one_tab[] = "12\tsrc/a.c\0";
    T_FAILS_WITH(run_numstat(one_tab, sizeof(one_tab) - 1u, &cap, &err), ATLAS_ERR_GIT, &err);

    static const char bad_counts[] = "x\ty\tsrc/a.c\0";
    T_FAILS_WITH(run_numstat(bad_counts, sizeof(bad_counts) - 1u, &cap, &err), ATLAS_ERR_GIT, &err);

    /* A rename form whose path operands never arrive. */
    static const char dangling[] = "5\t5\t\0only-old.c\0";
    T_FAILS_WITH(run_numstat(dangling, sizeof(dangling) - 1u, &cap, &err), ATLAS_ERR_GIT, &err);
    atlas_buf_free(&cap.log);
}

/* --- validators and the read-only allowlist ------------------------------ */

static void test_oid_and_int_validators(void) {
    T_CHECK(atlas_git_is_hex_oid(OID40, 40u));
    T_CHECK(atlas_git_is_hex_oid(OID64, 64u));
    T_CHECK(!atlas_git_is_hex_oid("abc", 3u));
    T_CHECK(!atlas_git_is_hex_oid("", 0));
    T_CHECK(!atlas_git_is_hex_oid("z626abf0f72e58d7a153368ba57db4c673c0e171", 40u));

    int64_t v = 0;
    T_CHECK(atlas_parse_i64("0", 1u, &v) && v == 0);
    T_CHECK(atlas_parse_i64("1786014580", 10u, &v) && v == 1786014580);
    T_CHECK(atlas_parse_i64("-42", 3u, &v) && v == -42);
    T_CHECK(!atlas_parse_i64("", 0, &v));
    T_CHECK(!atlas_parse_i64("12x", 3u, &v));
    T_CHECK(!atlas_parse_i64("-", 1u, &v));
    T_CHECK(!atlas_parse_i64("99999999999999999999", 20u, &v));
}

static void test_readonly_allowlist(void) {
    const char *reason = NULL;

    const char *ok_log[] = {"/usr/bin/git", "-C", "/repo", "--no-pager", "log", "-z", NULL};
    T_CHECK(atlas_git_argv_is_readonly(ok_log, &reason));

    const char *ok_version[] = {"/usr/bin/git", "--version", NULL};
    T_CHECK(atlas_git_argv_is_readonly(ok_version, &reason));

    /* A -c value that happens to name a subcommand must not be mistaken for one. */
    const char *ok_cfg[] = {"/usr/bin/git", "-c", "commit.gpgsign=false", "status", NULL};
    T_CHECK(atlas_git_argv_is_readonly(ok_cfg, &reason));

    const char *deny_commit[] = {"/usr/bin/git", "-C", "/repo", "commit", "-m", "x", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_commit, &reason));

    const char *deny_push[] = {"/usr/bin/git", "push", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_push, &reason));

    const char *deny_checkout[] = {"/usr/bin/git", "-C", "/repo", "checkout", "main", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_checkout, &reason));

    const char *deny_clean[] = {"/usr/bin/git", "clean", "-fdx", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_clean, &reason));

    const char *deny_reset[] = {"/usr/bin/git", "reset", "--hard", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_reset, &reason));

    const char *deny_gc[] = {"/usr/bin/git", "gc", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_gc, &reason));

    const char *deny_config[] = {"/usr/bin/git", "config", "user.name", "x", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(deny_config, &reason));

    const char *dangling_c[] = {"/usr/bin/git", "-c", NULL};
    T_CHECK(!atlas_git_argv_is_readonly(dangling_c, &reason));
}

static void test_change_type_mapping(void) {
    T_EQ_STR(atlas_git_change_type_name('A'), "add");
    T_EQ_STR(atlas_git_change_type_name('M'), "modify");
    T_EQ_STR(atlas_git_change_type_name('D'), "delete");
    T_EQ_STR(atlas_git_change_type_name('R'), "rename");
    T_EQ_STR(atlas_git_change_type_name('C'), "copy");
    T_EQ_STR(atlas_git_change_type_name('T'), "typechange");
    T_EQ_STR(atlas_git_change_type_name('U'), "unmerged");
    T_EQ_STR(atlas_git_change_type_name('X'), "unknown");
    T_EQ_STR(atlas_git_change_type_name('?'), "unknown");
}

static const atlas_test TESTS[] = {
    {"nul splitter is chunk-independent", test_nulsplit_chunking},
    {"nul splitter rejects a truncated tail", test_nulsplit_unterminated_tail},
    {"nul splitter enforces a record ceiling", test_nulsplit_token_ceiling},
    {"ls-files parses hostile paths", test_lsfiles_valid},
    {"ls-files rejects malformed records", test_lsfiles_malformed},
    {"log parses the real framing", test_log_full_sequence},
    {"log keeps hostile path operands intact", test_log_hostile_paths},
    {"log rejects malformed output", test_log_malformed},
    {"status parses a clean tree", test_status_clean},
    {"status counts dirt and renames", test_status_dirty_and_rename},
    {"status reports untracked shapes", test_status_untracked_shapes},
    {"status rejects a truncated rename", test_status_truncated_rename},
    {"status reports untracked shapes", test_status_untracked_shapes},
    {"status rejects a truncated rename", test_status_truncated_rename},
    {"status handles unborn and detached HEAD", test_status_unborn_and_detached},
    {"status rejects malformed output", test_status_malformed},
    {"numstat parsing", test_numstat_parsing},
    {"numstat rejects malformed output", test_numstat_malformed},
    {"oid and integer validators", test_oid_and_int_validators},
    {"read-only git allowlist", test_readonly_allowlist},
    {"change type mapping", test_change_type_mapping},
};

ATLAS_TEST_MAIN("git-parse", TESTS)

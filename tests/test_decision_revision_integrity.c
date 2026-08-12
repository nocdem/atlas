/* Atlas - a revision stores what it was given, and a link change changes only links.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * The defect this suite exists for
 * --------------------------------
 * `decision` carried two different things depending on which operation was in
 * flight: a propose put its **prose** there, a revise put the **document id**
 * there and its prose in `decision_body`. The server read `decision` in both
 * cases and never read `decision_body`, so every revise stored the document's
 * own id as its decision text — and returned success. `decision link add` is a
 * revise with the same links re-sent, so it corrupted the prose too, which
 * meant *recording a relationship silently destroyed the document's content*.
 *
 * Nothing caught it because nothing compared what went in with what came back.
 * A revise that returns `ok` and a new revision number looks like a revise that
 * worked; the corruption is only visible if you read the stored text. So every
 * case below reads it back and compares bytes.
 *
 * Two keys now carry the two meanings — `decision_uid` and `decision_body` —
 * and the server additionally refuses prose that equals the document's own id,
 * whatever key it arrived under. That guard is the one that would have caught
 * the original defect, so it is tested directly rather than assumed.
 *
 * Cross-uid is not testable here: a test binary may not create a second OS
 * principal, and the system socket's path comes from a root-owned policy this
 * suite must not write. What is testable is the *mode* — a client with no
 * context talking to a daemon that owns the index — and that is what the socket
 * cases below run. */
#include <stdlib.h>
#include <string.h>

#include "atlas/service.h"
#include "atlas_test.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    fx_daemon d;
} env;

/* Multi-line, non-ASCII prose. A defect that truncates, re-encodes or
 * substitutes the body is far more visible against text with structure than
 * against one short line. */
#define BODY_ONE                                                                                   \
    "First line of the decision.\n"                                                                \
    "Second line with a tab\there and a quote \" here.\n"                                          \
    "Third line: naive resume, Turkce karakterler cgiosu, and an em dash - like this."

#define BODY_TWO                                                                                   \
    "Replaced body, still multi-line.\n"                                                           \
    "It must arrive byte-for-byte, including this trailing detail."

static void env_open(env *e) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "one.c",
                  "int only_here(void){return 0;}\nint twice(void){return 1;}\n", &err),
         &err);
    T_OK(fx_write(fx_repo(&e->fx), "two.c", "int twice(void){return 2;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, fx_repo(&e->fx), &err), &err);
    T_OK(fx_commit(&e->fx, fx_repo(&e->fx), "first", &err), &err);
    {
        const char *add[] = {"--data-dir", fx_data_dir(&e->fx), "repo", "add",
                             fx_repo(&e->fx), "--name", "proj"};
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
    {
        const char *cs[] = {"--data-dir", fx_data_dir(&e->fx), "code", "sync", "proj"};
        int code = -1;
        T_OK(fx_atlas(cs, 5u, NULL, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    fx_daemon_init(&e->d);
    T_OK(fx_daemon_start(&e->fx, &e->d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e->d, 15000, &err), &err);
}

static void env_close(env *e) {
    fx_daemon_stop(&e->d, false);
    fx_daemon_free(&e->d);
    fx_close(&e->fx);
}

static int run_remote(env *e, const char *const *args, size_t n, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    int code = -1;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, n, out, NULL, &code, &err), &err);
    T_REQUIRE(code != 139); /* a crash is never an answer */
    return code;
}

static void json_str(const atlas_buf *json, const char *key, char *out, size_t n) {
    char pat[64];
    (void)snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *k = strstr(atlas_buf_cstr(json), pat);
    T_REQUIRE(k != NULL);
    k += strlen(pat);
    const char *end = strchr(k, '"');
    T_REQUIRE(end != NULL);
    size_t len = (size_t)(end - k);
    T_REQUIRE(len + 1u < n);
    memcpy(out, k, len);
    out[len] = '\0';
}

/* The stored body, decoded out of the export.
 *
 * Values leave Atlas in its safe-text encoding — `%0A` for a newline, `%09` for
 * a tab, `%25` for a literal percent — so the comparison has to decode before
 * it compares. That encoding is also exactly where the second defect lived:
 * `link add` re-sent an already-encoded body and it was encoded again, so
 * `%0A` became `%250A`. Decoding once and comparing against the original text
 * catches both a lost body and a doubly-encoded one. */
static void stored_body(env *e, const char *uid, atlas_buf *out) {
    atlas_buf j = ATLAS_BUF_INIT;
    const char *args[] = {"--json", "decision", "export", "proj", uid, "--format", "json"};
    T_REQUIRE(run_remote(e, args, 7u, &j) == 0);
    const char *k = strstr(atlas_buf_cstr(&j), "\"decision_text\":\"");
    T_REQUIRE(k != NULL);
    k += strlen("\"decision_text\":\"");
    atlas_err err;
    atlas_err_init(&err);
    atlas_buf_reset(out);
    for (const char *p = k; *p != '\0' && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1] != '\0') {
            /* JSON escaping happens outside the safe-text encoding, so it is
             * undone first: the body deliberately contains a quote. */
            p++;
            c = (*p == 'n') ? '\n' : (*p == 't') ? '\t' : *p;
            T_OK(atlas_buf_append(out, &c, 1u, &err), &err);
            continue;
        }
        if (c == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = {p[1], p[2], '\0'};
            char *endp = NULL;
            long v = strtol(hex, &endp, 16);
            if (endp != NULL && *endp == '\0') {
                c = (char)v;
                p += 2;
            }
        }
        T_OK(atlas_buf_append(out, &c, 1u, &err), &err);
    }
    atlas_buf_free(&j);
}

static void propose(env *e, const char *title, const char *body, char *uid_out, size_t n,
                    const char *const *extra, size_t nextra) {
    atlas_buf out = ATLAS_BUF_INIT;
    const char *base[] = {"--json", "decision", "propose", "proj", "--title", title,
                          "--decision", body};
    const char *args[24];
    size_t k = 0;
    for (; k < 8u; k++) {
        args[k] = base[k];
    }
    for (size_t i = 0; i < nextra; i++) {
        args[k++] = extra[i];
    }
    T_REQUIRE(run_remote(e, args, k, &out) == 0);
    json_str(&out, "decision", uid_out, n);
    atlas_buf_free(&out);
}

/* A propose stores its prose, and the prose is not the id.
 *
 * The first half of the contract: `decision_body` carries the text on every
 * operation now, including the one that used to send it under `decision`. */
static void test_propose_body_is_not_the_uid(void) {
    env e;
    env_open(&e);
    char uid[64];
    propose(&e, "proposed", BODY_ONE, uid, sizeof uid, NULL, 0);
    atlas_buf got = ATLAS_BUF_INIT;
    stored_body(&e, uid, &got);
    T_EQ_STR(atlas_buf_cstr(&got), BODY_ONE);
    T_REQUIRE(strcmp(atlas_buf_cstr(&got), uid) != 0);
    atlas_buf_free(&got);
    env_close(&e);
}

/* A revise stores the body it was given, and leaves the old revision alone.
 *
 * This is the case that was silently wrong: it returned ok, incremented the
 * revision, and stored the uid. */
static void test_revise_stores_the_body(void) {
    env e;
    env_open(&e);
    char uid[64];
    propose(&e, "revised", BODY_ONE, uid, sizeof uid, NULL, 0);

    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json",     "decision", "revise", "proj", uid,
                              "--title",    "revised",  "--decision", BODY_TWO};
        T_REQUIRE(run_remote(&e, args, 9u, &out) == 0);
        T_REQUIRE(strstr(atlas_buf_cstr(&out), "\"revision\":2") != NULL);
    }
    atlas_buf_free(&out);

    atlas_buf got = ATLAS_BUF_INIT;
    stored_body(&e, uid, &got);
    T_EQ_STR(atlas_buf_cstr(&got), BODY_TWO);
    T_REQUIRE(strcmp(atlas_buf_cstr(&got), uid) != 0);
    atlas_buf_free(&got);

    /* The superseded revision is immutable and still holds the original. */
    atlas_buf j = ATLAS_BUF_INIT;
    const char *show[] = {"--json", "decision", "show", "proj", uid, "--revision", "1"};
    T_REQUIRE(run_remote(&e, show, 7u, &j) == 0);
    T_REQUIRE(strstr(atlas_buf_cstr(&j), "First line of the decision.") != NULL);
    T_REQUIRE(strstr(atlas_buf_cstr(&j), "Replaced body") == NULL);
    atlas_buf_free(&j);
    env_close(&e);
}

/* `link add` changes links and nothing else.
 *
 * It goes through revise with the current content re-sent, so before the fix it
 * destroyed the prose it was re-sending. Title, rationale and consequences are
 * compared as well as the body: a fix that preserved only the body would leave
 * the same class of defect one field over. */
static void test_link_add_preserves_content(void) {
    env e;
    env_open(&e);
    char base[64], target[64];
    propose(&e, "the target", "a target decision", target, sizeof target, NULL, 0);
    {
        const char *extra[] = {"--rationale", "the reason it was chosen",
                               "--consequences", "what follows from it",
                               "--path", "one.c", "--symbol-link", "only_here"};
        propose(&e, "the source", BODY_ONE, base, sizeof base, extra, 8u);
    }
    atlas_buf before = ATLAS_BUF_INIT;
    stored_body(&e, base, &before);

    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json", "decision", "link", "add", "proj", base, target};
        T_REQUIRE(run_remote(&e, args, 7u, &out) == 0);
    }
    atlas_buf_reset(&out);

    atlas_buf after = ATLAS_BUF_INIT;
    stored_body(&e, base, &after);
    T_EQ_STR(atlas_buf_cstr(&after), atlas_buf_cstr(&before));
    T_REQUIRE(strcmp(atlas_buf_cstr(&after), base) != 0);

    /* Everything else survived, and the relation actually landed. */
    const char *show[] = {"--json", "decision", "show", "proj", base};
    T_REQUIRE(run_remote(&e, show, 5u, &out) == 0);
    const char *s = atlas_buf_cstr(&out);
    T_REQUIRE(strstr(s, "the source") != NULL);
    T_REQUIRE(strstr(s, "the reason it was chosen") != NULL);
    T_REQUIRE(strstr(s, "what follows from it") != NULL);
    T_REQUIRE(strstr(s, "one.c") != NULL);
    T_REQUIRE(strstr(s, "only_here") != NULL);
    T_REQUIRE(strstr(s, target) != NULL);
    atlas_buf_free(&out);
    atlas_buf_free(&before);
    atlas_buf_free(&after);
    env_close(&e);
}

/* A revise carrying an explicit complete link set replaces the links exactly.
 *
 * There is no `link remove`, so replacing a link set — dropping an unsupported
 * relation, swapping a commit for a better one — is a revise that re-sends the
 * set it wants. That has to be one atomic revision, not one per change. */
static void test_exact_link_set_replacement(void) {
    env e;
    env_open(&e);
    atlas_buf head = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json", "status", "proj"};
        T_REQUIRE(run_remote(&e, args, 3u, &head) == 0);
    }
    char oid[64];
    json_str(&head, "head", oid, sizeof oid);
    atlas_buf_free(&head);

    char keep[64], drop[64], subject[64];
    propose(&e, "kept relation", "a relation that stays", keep, sizeof keep, NULL, 0);
    propose(&e, "dropped relation", "a relation that goes", drop, sizeof drop, NULL, 0);
    {
        const char *extra[] = {"--path", "one.c", "--path", "two.c",
                               "--decision-link", keep, "--decision-link", drop};
        propose(&e, "subject", BODY_ONE, subject, sizeof subject, extra, 8u);
    }
    /* One revise: drop a relation, drop a path, add a commit. */
    atlas_buf out = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json",   "decision", "revise", "proj",   subject,
                              "--title",  "subject",  "--decision", BODY_ONE,
                              "--path",   "one.c",    "--commit",   oid,
                              "--decision-link", keep};
        T_REQUIRE(run_remote(&e, args, 15u, &out) == 0);
        T_REQUIRE(strstr(atlas_buf_cstr(&out), "\"revision\":2") != NULL);
    }
    atlas_buf_reset(&out);
    const char *show[] = {"--json", "decision", "show", "proj", subject};
    T_REQUIRE(run_remote(&e, show, 5u, &out) == 0);
    const char *s = atlas_buf_cstr(&out);
    T_REQUIRE(strstr(s, "one.c") != NULL);
    T_REQUIRE(strstr(s, "two.c") == NULL);  /* dropped */
    T_REQUIRE(strstr(s, keep) != NULL);
    T_REQUIRE(strstr(s, drop) == NULL);     /* dropped */
    T_REQUIRE(strstr(s, oid) != NULL);      /* added */
    atlas_buf_free(&out);

    atlas_buf got = ATLAS_BUF_INIT;
    stored_body(&e, subject, &got);
    T_EQ_STR(atlas_buf_cstr(&got), BODY_ONE);
    atlas_buf_free(&got);
    env_close(&e);
}

/* Every refusal, and what they must share: a controlled exit and no mutation. */
static void test_refusals_write_nothing(void) {
    env e;
    env_open(&e);
    char uid[64];
    propose(&e, "untouched", BODY_ONE, uid, sizeof uid, NULL, 0);

    atlas_buf out = ATLAS_BUF_INIT;
    /* A revise with no body at all. */
    {
        const char *args[] = {"decision", "revise", "proj", uid, "--title", "untouched"};
        T_REQUIRE(run_remote(&e, args, 6u, &out) != 0);
    }
    atlas_buf_reset(&out);
    /* The prose deliberately set to the document's own id: the guard that would
     * have caught the original defect. */
    {
        const char *args[] = {"decision", "revise", "proj", uid, "--title", "untouched",
                              "--decision", uid};
        T_REQUIRE(run_remote(&e, args, 8u, &out) != 0);
    }
    atlas_buf_reset(&out);
    /* No document named at all. */
    {
        const char *args[] = {"decision", "revise", "proj", "--title", "x", "--decision", "y"};
        T_REQUIRE(run_remote(&e, args, 7u, &out) != 0);
    }
    atlas_buf_reset(&out);
    /* A malformed document id. */
    {
        const char *args[] = {"decision", "revise", "proj", "not-a-decision-id",
                              "--title", "x", "--decision", "y"};
        T_REQUIRE(run_remote(&e, args, 8u, &out) != 0);
    }
    atlas_buf_reset(&out);
    /* A relation to a document that does not exist, and to itself. */
    {
        const char *args[] = {"decision", "revise", "proj", uid, "--title", "untouched",
                              "--decision", BODY_ONE, "--decision-link",
                              "atlas-dec-00000000000000000000000000000000"};
        T_REQUIRE(run_remote(&e, args, 10u, &out) != 0);
    }
    atlas_buf_reset(&out);
    {
        const char *args[] = {"decision", "revise", "proj", uid, "--title", "untouched",
                              "--decision", BODY_ONE, "--decision-link", uid};
        T_REQUIRE(run_remote(&e, args, 10u, &out) != 0);
    }
    atlas_buf_reset(&out);

    /* Nothing above may have written a revision, and the body is untouched. */
    const char *show[] = {"--json", "decision", "show", "proj", uid};
    T_REQUIRE(run_remote(&e, show, 5u, &out) == 0);
    T_REQUIRE(strstr(atlas_buf_cstr(&out), "\"revision\":1") != NULL);
    atlas_buf_free(&out);

    atlas_buf got = ATLAS_BUF_INIT;
    stored_body(&e, uid, &got);
    T_EQ_STR(atlas_buf_cstr(&got), BODY_ONE);
    atlas_buf_free(&got);
    env_close(&e);
}

/* The socket and the local path must store identical bytes.
 *
 * Compared by content hash, which is computed from the revision's content: two
 * documents with the same hash are the same document however they were written.
 * A body that survived one route and not the other shows up here. */
static void test_local_socket_revise_equivalence(void) {
    env e;
    env_open(&e);
    atlas_err err;
    atlas_err_init(&err);

    char uid[64];
    propose(&e, "equivalence", BODY_ONE, uid, sizeof uid, NULL, 0);
    atlas_buf remote_out = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json",  "decision", "revise", "proj", uid,
                              "--title", "equivalence", "--decision", BODY_TWO};
        T_REQUIRE(run_remote(&e, args, 9u, &remote_out) == 0);
    }
    char rhash[80];
    json_str(&remote_out, "content_hash", rhash, sizeof rhash);
    atlas_buf_free(&remote_out);

    /* Same content, written locally onto a second document. The daemon must be
     * stopped first: it holds the writer lock for its whole life. */
    fx_daemon_stop(&e.d, false);
    atlas_buf local_out = ATLAS_BUF_INIT;
    char luid[64];
    {
        const char *args[] = {"--json", "--data-dir", fx_data_dir(&e.fx), "decision", "propose",
                              "proj",   "--title",    "equivalence",      "--decision", BODY_ONE};
        int code = -1;
        T_OK(fx_atlas(args, 10u, &local_out, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
        json_str(&local_out, "decision", luid, sizeof luid);
    }
    atlas_buf_reset(&local_out);
    {
        const char *args[] = {"--json",      "--data-dir",  fx_data_dir(&e.fx), "decision",
                              "revise",      "proj",        luid,               "--title",
                              "equivalence", "--decision",  BODY_TWO};
        int code = -1;
        T_OK(fx_atlas(args, 11u, &local_out, NULL, &code, &err), &err);
        T_REQUIRE(code == 0);
    }
    char lhash[80];
    json_str(&local_out, "content_hash", lhash, sizeof lhash);
    atlas_buf_free(&local_out);

    T_EQ_STR(lhash, rhash);
    fx_daemon_free(&e.d);
    fx_close(&e.fx);
}

/* A9.1. Every read surface reports the kind the record was created with — through
 * the binary, over the socket, because that is the layer the defect was in.
 *
 * The defect: `decision list` reported the kind correctly and `decision show`
 * reported nothing, because the show path copies the summary field by field into
 * a second struct and the new field had not been added to the copy. Both paths
 * read the same row and the same projection, so a database-level test could not
 * see it — and `decision export`, which reads the show path, was printing
 * `DECISION` for an ACCEPTED_RISK. A confident wrong classification is worse than
 * no classification, which is the whole reason this suite compares what came back
 * with what went in rather than checking that a command exited zero. */
static void test_every_read_surface_reports_the_kind(void) {
    env e;
    env_open(&e);
    char uid[64];
    {
        /* A path link, so the `for-file` case below has something to find. */
        const char *extra[] = {"--kind", "ACCEPTED_RISK", "--path", "one.c"};
        propose(&e, "an accepted risk", BODY_ONE, uid, sizeof uid, extra, 4u);
    }

    /* propose echoes it. */
    atlas_buf j = ATLAS_BUF_INIT;
    {
        const char *args[] = {"--json",         "decision", "propose",   "proj",
                              "--title",        "a second risk",
                              "--decision",     "another body",
                              "--kind",         "ACCEPTED_RISK"};
        T_REQUIRE(run_remote(&e, args, 10u, &j) == 0);
        T_REQUIRE_MSG(strstr(atlas_buf_cstr(&j), "\"kind\":\"ACCEPTED_RISK\"") != NULL,
                      "propose did not echo the kind: %s", atlas_buf_cstr(&j));
    }
    atlas_buf_free(&j);

    /* list, show and for-file all report it, and none of them says DECISION. */
    static const char *const CASES[][6] = {
        {"--json", "decision", "list", "proj", NULL, NULL},
        {"--json", "decision", "show", "proj", NULL /* uid */, NULL},
        {"--json", "decision", "for-file", "proj", "one.c", NULL},
    };
    static const size_t NARGS[] = {4u, 5u, 5u};
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const char *args[6];
        for (size_t k = 0; k < NARGS[i]; k++) {
            args[k] = CASES[i][k] != NULL ? CASES[i][k] : uid;
        }
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, NARGS[i], &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"ACCEPTED_RISK\"") != NULL,
                    "%s did not report the kind: %s", CASES[i][2], atlas_buf_cstr(&out));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"DECISION\"") == NULL,
                    "%s reported the wrong kind: %s", CASES[i][2], atlas_buf_cstr(&out));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"\"") == NULL,
                    "%s reported an empty kind: %s", CASES[i][2], atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* And the Markdown export, which reads the show path. */
    {
        atlas_buf md = ATLAS_BUF_INIT;
        const char *args[] = {"decision", "export", "proj", uid, "--format", "markdown"};
        T_REQUIRE(run_remote(&e, args, 6u, &md) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&md), "- kind: **ACCEPTED_RISK**") != NULL,
                    "the export did not name the kind: %s", atlas_buf_cstr(&md));
        T_CHECK_MSG(strstr(atlas_buf_cstr(&md), "**DECISION**") == NULL,
                    "the export claimed the wrong kind: %s", atlas_buf_cstr(&md));
        atlas_buf_free(&md);
    }

    /* A record proposed without a kind reads as DECISION, everywhere. */
    char plain[64];
    propose(&e, "no kind was named", "an ordinary decision", plain, sizeof plain, NULL, 0);
    {
        atlas_buf out = ATLAS_BUF_INIT;
        const char *args[] = {"--json", "decision", "show", "proj", plain};
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"DECISION\"") != NULL,
                    "an unclassified record did not read as DECISION: %s", atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a proposed body is stored, and is not the uid", test_propose_body_is_not_the_uid},
    {"every read surface reports the kind", test_every_read_surface_reports_the_kind},
    {"a revise stores its body and leaves revision 1 alone", test_revise_stores_the_body},
    {"link add changes links and preserves all content", test_link_add_preserves_content},
    {"an exact link set replaces links in one revision", test_exact_link_set_replacement},
    {"every refusal writes nothing", test_refusals_write_nothing},
    {"local and socket revise store identical bytes", test_local_socket_revise_equivalence},
};

ATLAS_TEST_MAIN("decision_revision_integrity", TESTS)

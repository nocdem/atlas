/* Atlas - a relation between two decisions is explicable, withdrawable, and
 * cannot be confused with the decisions it relates.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 *
 * Three defects motivated this suite, and the coverage that existed when they
 * shipped could not have found any of them, because every case used **one**
 * document: a link whose source and target are the same id proves nothing about
 * a field that holds a source or a target.
 *
 *  1. **A relation had nowhere to record why it existed.** `relates_to` became
 *     expressible in migration 9 and stayed unexplained: the manifest that drew
 *     a hundred edges validated a rationale for each one and then discarded it,
 *     because Atlas had no field to put it in. The only copy lived in a shell
 *     script in a temporary directory.
 *
 *  2. **A relation could be drawn and never withdrawn.** There was no removal
 *     on any surface, so a wrong edge was permanent.
 *
 *  3. **Prose and a document id shared a key** — the A8.2 defect. A revise
 *     stored the document's own id where its decision text belonged, so
 *     recording a relationship destroyed the document's content. Every case
 *     below that supplies a uid where prose is expected, or prose where a uid
 *     is expected, exists because that actually happened.
 *
 * So every case uses **two distinct documents** and checks the direction of
 * every edge, and the cases that matter most are the ones that pass the wrong
 * id in the right place. */
#include <stdlib.h>
#include <string.h>

#include "atlas/limits.h"
#include "atlas/service.h"
#include "atlas_test.h"
#include "support/fixture.h"

typedef struct env {
    fixture fx;
    fx_daemon d;
    bool daemon_running;
} env;

static void env_open(env *e, bool with_daemon) {
    atlas_err err;
    atlas_err_init(&err);
    memset(e, 0, sizeof(*e));
    T_OK(fx_open(&e->fx, &err), &err);
    T_OK(fx_init_repo(&e->fx, fx_repo(&e->fx), NULL, &err), &err);
    T_OK(fx_write(fx_repo(&e->fx), "one.c", "int only_here(void){return 0;}\n", &err), &err);
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
    if (with_daemon) {
        fx_daemon_init(&e->d);
        T_OK(fx_daemon_start(&e->fx, &e->d, &err), &err);
        T_OK(fx_daemon_wait_ready(&e->d, 15000, &err), &err);
        e->daemon_running = true;
    }
}

/* A second repository, for the isolation cases. */
static void env_add_second_repo(env *e, atlas_buf *root_out) {
    atlas_err err;
    atlas_err_init(&err);
    T_OK(fx_mkdir(e->fx.root.data, "other", &err), &err);
    T_OK(atlas_buf_appendf(root_out, &err, "%s/other", e->fx.root.data), &err);
    T_OK(fx_init_repo(&e->fx, atlas_buf_cstr(root_out), NULL, &err), &err);
    T_OK(fx_write(atlas_buf_cstr(root_out), "x.c", "int x(void){return 0;}\n", &err), &err);
    T_OK(fx_add_all(&e->fx, atlas_buf_cstr(root_out), &err), &err);
    T_OK(fx_commit(&e->fx, atlas_buf_cstr(root_out), "first", &err), &err);
    const char *add[] = {"--data-dir",           fx_data_dir(&e->fx), "repo", "add",
                         atlas_buf_cstr(root_out), "--name",          "other"};
    int code = -1;
    T_OK(fx_atlas(add, 7u, NULL, NULL, &code, &err), &err);
    T_REQUIRE(code == 0);
    const char *scan[] = {"--data-dir", fx_data_dir(&e->fx), "scan", "other"};
    T_OK(fx_atlas(scan, 4u, NULL, NULL, &code, &err), &err);
    T_REQUIRE(code == 0);
}

static void env_close(env *e) {
    if (e->daemon_running) {
        fx_daemon_stop(&e->d, false);
        fx_daemon_free(&e->d);
    }
    fx_close(&e->fx);
}

/* Local: the CLI owns the index. */
static int run_local(env *e, const char *const *args, size_t n, atlas_buf *out) {
    (void)e; /* local runs carry --data-dir explicitly, like every other suite */
    atlas_err err;
    atlas_err_init(&err);
    int code = -1;
    T_OK(fx_atlas(args, n, out, NULL, &code, &err), &err);
    T_REQUIRE(code != 139);
    return code;
}

/* Over the socket: the daemon owns the index. */
static int run_remote(env *e, const char *const *args, size_t n, atlas_buf *out) {
    atlas_err err;
    atlas_err_init(&err);
    int code = -1;
    T_OK(fx_atlas_with_runtime(&e->fx, &e->d, args, n, out, NULL, &code, &err), &err);
    T_REQUIRE(code != 139);
    return code;
}

static void extract_str(const atlas_buf *json, const char *key, char *out, size_t n) {
    atlas_buf pat = ATLAS_BUF_INIT;
    atlas_err err;
    atlas_err_init(&err);
    T_OK(atlas_buf_appendf(&pat, &err, "\"%s\":\"", key), &err);
    const char *k = strstr(atlas_buf_cstr(json), atlas_buf_cstr(&pat));
    T_REQUIRE_MSG(k != NULL, "key %s not present", key);
    k += pat.len;
    const char *end = strchr(k, '"');
    T_REQUIRE(end != NULL);
    size_t len = (size_t)(end - k);
    T_REQUIRE(len + 1u < n);
    memcpy(out, k, len);
    out[len] = '\0';
    atlas_buf_free(&pat);
}

/* Proposes one decision and returns its uid. Every case needs at least two, and
 * the titles differ so a mix-up is visible rather than silently consistent. */
static void propose(env *e, bool remote, const char *repo, const char *title, char *uid_out,
                    size_t n) {
    atlas_buf out = ATLAS_BUF_INIT;
    const char *args_local[] = {"--data-dir", fx_data_dir(&e->fx), "--json", "decision",
                                "propose",    repo,                "--title", title,
                                "--decision", title,               "--scope", "SUBSYSTEM"};
    const char *args_remote[] = {"--json",  "decision", "propose", repo,       "--title", title,
                                 "--decision", title,   "--scope", "SUBSYSTEM"};
    int code = remote ? run_remote(e, args_remote, 10u, &out)
                      : run_local(e, args_local, 12u, &out);
    T_REQUIRE_MSG(code == 0, "propose failed: %s", atlas_buf_cstr(&out));
    extract_str(&out, "decision", uid_out, n);
    atlas_buf_free(&out);
}

/* --- 1. the round trip, with genuinely different source and target ---------
 *
 * The case the old coverage could not express. A links to B, and the assertions
 * are about *direction*: B's document must not report the edge, and the note
 * must come back attached to the edge rather than to either document. */
static void test_cross_uid_rationale_round_trip(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);
    T_REQUIRE_MSG(strcmp(a, b) != 0, "the two documents must have different ids");

    static const char WHY[] = "A depends on B: the first thesis assumes the second holds.";
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a,
                              b,            "--why",            WHY};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_local(&e, args, 11u, &out) == 0, "link add failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* The source document reports the edge, pointing at B, carrying the note. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "show",             "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(s, "\"kind\":\"relates_to\"") != NULL, "no relates_to link on A");
        T_CHECK_MSG(strstr(s, b) != NULL, "A's relation does not name B");
        T_CHECK_MSG(strstr(s, WHY) != NULL, "the rationale did not survive: %s", s);
        T_CHECK_MSG(strstr(s, "\"rationale_provenance\":\"OPERATOR\"") != NULL,
                    "provenance not recorded");
        atlas_buf_free(&out);
    }
    /* The target does not. An edge is directed, and a reader must not find it
     * from the far end as though it were. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "show",             "proj", b};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"relates_to\"") == NULL,
                    "B reports a relation it is only the target of");
        atlas_buf_free(&out);
    }
    env_close(&e);
}

/* --- 2. the A8.2 confusion, at link granularity --------------------------- */

static void test_uid_and_prose_cannot_be_confused(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);

    /* A rationale that is a document id. This is the shape that destroyed a
     * document's prose in A8.2, and it is refused structurally rather than
     * detected afterwards. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a,
                              b,            "--why",            b};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK_MSG(run_local(&e, args, 11u, &out) != 0,
                    "a decision id was accepted as a rationale");
        atlas_buf_free(&out);
    }
    /* Prose where a target id belongs. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",  "decision",
                              "link",       "add",              "proj",    a,
                              "because it depends on it",       "--why",   "x"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK(run_local(&e, args, 11u, &out) != 0);
        atlas_buf_free(&out);
    }
    /* A source id that is not a document. */
    {
        const char *args[] = {"--data-dir",
                              fx_data_dir(&e.fx),
                              "--json",
                              "decision",
                              "link",
                              "add",
                              "proj",
                              "atlas-dec-00000000000000000000000000000000",
                              b,
                              "--why",
                              "x"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK(run_local(&e, args, 11u, &out) != 0);
        atlas_buf_free(&out);
    }
    /* A target id that is not a document. */
    {
        const char *args[] = {"--data-dir",
                              fx_data_dir(&e.fx),
                              "--json",
                              "decision",
                              "link",
                              "add",
                              "proj",
                              a,
                              "atlas-dec-00000000000000000000000000000000",
                              "--why",
                              "x"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK(run_local(&e, args, 11u, &out) != 0);
        atlas_buf_free(&out);
    }
    /* A decision may not relate to itself, whichever way it is asked. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a,
                              a,            "--why",            "x"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK(run_local(&e, args, 11u, &out) != 0);
        atlas_buf_free(&out);
    }

    /* And the whole point: after all of those refusals, A's prose is still what
     * it was. A8.2's defect was silent, so the assertion has to be about the
     * document rather than about the exit code. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "show",             "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "the first thesis") != NULL,
                    "the document's prose did not survive the refused links");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), a) != NULL, "the document lost its own id");
        atlas_buf_free(&out);
    }
    env_close(&e);
}

/* --- 3. withdrawal, and the history that outlives it ---------------------- */

static void test_link_remove_keeps_its_history(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);

    static const char WHY_ADD[] = "A depends on B for its ordering guarantee.";
    static const char WHY_GONE[] = "B was restated and no longer carries that guarantee.";
    const char *add[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                         "link",       "add",              "proj",   a,
                         b,            "--why",            WHY_ADD};
    const char *rm[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                        "link",       "remove",           "proj",   a,
                        b,            "--why",            WHY_GONE};
    {
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, add, 11u, &out) == 0);
        atlas_buf_free(&out);
    }
    {
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_local(&e, rm, 11u, &out) == 0, "link remove failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }

    /* The current revision no longer asserts it. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "show",             "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"relates_to\"") == NULL,
                    "a withdrawn relation is still asserted");
        atlas_buf_free(&out);
    }
    /* But the account of it survives whole: both events, both reasons, in the
     * order they happened. This is the requirement removal must not break — a
     * withdrawn edge has to stay explicable. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "links",            "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        const char *added = strstr(s, "\"event\":\"ADDED\"");
        const char *removed = strstr(s, "\"event\":\"REMOVED\"");
        T_CHECK_MSG(added != NULL, "the creation event is gone: %s", s);
        T_CHECK_MSG(removed != NULL, "the removal event is gone: %s", s);
        T_CHECK_MSG(added != NULL && removed != NULL && added < removed,
                    "the events are out of order");
        T_CHECK_MSG(strstr(s, WHY_ADD) != NULL, "why it was drawn is gone");
        T_CHECK_MSG(strstr(s, WHY_GONE) != NULL, "why it was withdrawn is gone");
        T_CHECK_MSG(strstr(s, b) != NULL, "the history does not name the target");
        T_CHECK_MSG(strstr(s, "\"active\":false") != NULL,
                    "a withdrawn edge is not reported as withdrawn");
        atlas_buf_free(&out);
    }

    /* Withdrawing it again is a no-op rather than an error or a second empty
     * revision: a caller retrying asked for a state that already holds. */
    {
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK_MSG(run_local(&e, rm, 11u, &out) == 0, "a repeated removal failed");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"removed\":false") != NULL ||
                        strstr(atlas_buf_cstr(&out), "\"duplicate\":false") != NULL,
                    "a repeated removal did not report that nothing changed: %s",
                    atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    /* And it can be drawn again. Re-adding is a new edge event on the same
     * semantic edge, so the history now holds three. */
    {
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, add, 11u, &out) == 0);
        atlas_buf_free(&out);
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "links",            "proj", a};
        atlas_buf log = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &log) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&log), "\"active\":true") != NULL,
                    "the re-added edge is not active");
        atlas_buf_free(&log);
    }
    env_close(&e);
}

/* Withdrawing a relation that was never drawn. */
static void test_removing_an_unknown_link(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);
    const char *rm[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                        "link",       "remove",           "proj",   a,
                        b,            "--why",            "it was never right"};
    atlas_buf out = ATLAS_BUF_INIT;
    T_CHECK_MSG(run_local(&e, rm, 11u, &out) == 0, "removing an absent relation errored");
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"removed\":false") != NULL,
                "removing an absent relation claimed to remove one: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    /* Nothing was written, so there is no history to find. A no-op that left an
     * event behind would make the ledger claim something happened. */
    const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                          "decision",   "links",            "proj", a};
    atlas_buf log = ATLAS_BUF_INIT;
    T_REQUIRE(run_local(&e, args, 7u, &log) == 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&log), "\"event\":") == NULL,
                "a no-op removal wrote an event: %s", atlas_buf_cstr(&log));
    atlas_buf_free(&log);
    env_close(&e);
}

/* --- 4. explaining an edge must not disturb an approval ------------------- */

/* `link add --why` on a relation that is already there records the reason and
 * writes **no revision**. That is the property that makes it possible to
 * explain the relations of an approved decision at all: a new revision would
 * move a content hash to cover something the approval never saw. */
static void test_annotating_an_existing_edge_writes_no_revision(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);

    const char *add_bare[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a, b};
    atlas_buf first = ATLAS_BUF_INIT;
    T_REQUIRE(run_local(&e, add_bare, 9u, &first) == 0);
    char hash_before[80];
    extract_str(&first, "content_hash", hash_before, sizeof hash_before);
    atlas_buf_free(&first);

    static const char WHY[] = "recorded after the fact, without touching the revision";
    const char *annotate[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a,
                              b,            "--why",            WHY};
    atlas_buf second = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(run_local(&e, annotate, 11u, &second) == 0, "annotate failed: %s",
                  atlas_buf_cstr(&second));
    char hash_after[80];
    extract_str(&second, "content_hash", hash_after, sizeof hash_after);
    atlas_buf_free(&second);

    T_CHECK_MSG(strcmp(hash_before, hash_after) == 0,
                "explaining an existing relation changed the content hash (%s -> %s)", hash_before,
                hash_after);

    /* And the reason is retrievable all the same. */
    const char *show[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                          "decision",   "show",             "proj", a};
    atlas_buf out = ATLAS_BUF_INIT;
    T_REQUIRE(run_local(&e, show, 7u, &out) == 0);
    T_CHECK_MSG(strstr(atlas_buf_cstr(&out), WHY) != NULL,
                "the annotation is not readable: %s", atlas_buf_cstr(&out));
    atlas_buf_free(&out);
    env_close(&e);
}

/* --- 5. bounds and encoding ----------------------------------------------- */

static void test_bounds_and_hostile_text(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);

    /* Over the bound: refused, never truncated. A silently shortened reason
     * reads as complete and is not. */
    {
        size_t n = ATLAS_DECISION_EDGE_NOTE_MAX + 1u;
        char *big = malloc(n + 1u);
        T_REQUIRE(big != NULL);
        memset(big, 'x', n);
        big[n] = '\0';
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a,
                              b,            "--why",            big};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK_MSG(run_local(&e, args, 11u, &out) != 0, "an oversized rationale was accepted");
        atlas_buf_free(&out);
        free(big);
    }
    /* Withdrawal without a reason is refused: it is the last thing that happens
     * to an edge, so an unrecorded reason is an unrecorded reason for ever. */
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "remove",           "proj",   a, b};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK_MSG(run_local(&e, args, 9u, &out) != 0, "a reasonless withdrawal was accepted");
        atlas_buf_free(&out);
    }
    /* Control characters and a bidi override survive as safe text rather than
     * reaching a terminal: the rationale is untrusted prose like every other
     * decision text, and it is encoded on the way out, once. */
    {
        static const char NASTY[] = "line\x1b[31m break\xe2\x80\xae and back";
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                              "link",       "add",              "proj",   a,
                              b,            "--why",            NASTY};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_local(&e, args, 11u, &out) == 0, "hostile text was refused outright");
        atlas_buf_free(&out);

        const char *show[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "show",             "proj", a};
        atlas_buf doc = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, show, 7u, &doc) == 0);
        const char *s = atlas_buf_cstr(&doc);
        T_CHECK_MSG(strstr(s, "\x1b") == NULL, "an escape byte reached the output");
        T_CHECK_MSG(strstr(s, "\xe2\x80\xae") == NULL, "a bidi override reached the output");
        T_CHECK_MSG(strstr(s, "%1B") != NULL, "the escape was not safe-encoded: %s", s);
        /* Encoded exactly once. A `%251B` here is the double-encoding defect. */
        T_CHECK_MSG(strstr(s, "%251B") == NULL, "the rationale was encoded twice");
        atlas_buf_free(&doc);
    }
    env_close(&e);
}

/* --- 6. isolation --------------------------------------------------------- */

static void test_a_relation_may_not_cross_repositories(void) {
    env e;
    env_open(&e, false);
    atlas_buf other = ATLAS_BUF_INIT;
    env_add_second_repo(&e, &other);
    char a[64], x[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "other", "a thesis elsewhere", x, sizeof x);

    const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                          "link",       "add",              "proj",   a,
                          x,            "--why",            "they look related"};
    atlas_buf out = ATLAS_BUF_INIT;
    T_CHECK_MSG(run_local(&e, args, 11u, &out) != 0, "a cross-repository relation was accepted");
    atlas_buf_free(&out);

    /* And naming the wrong repository for a document it does not hold. */
    const char *wrong[] = {"--data-dir", fx_data_dir(&e.fx), "--json", "decision",
                           "links",      "other",            a};
    atlas_buf out2 = ATLAS_BUF_INIT;
    T_CHECK_MSG(run_local(&e, wrong, 7u, &out2) != 0,
                "a document was read through a repository that does not hold it");
    atlas_buf_free(&out2);
    atlas_buf_free(&other);
    env_close(&e);
}

/* --- 7. the socket, and parity with the local path ------------------------ */

/* The same edge recorded and read over the socket. The local path was never the
 * one that broke in A8.2 — the daemon was — so this is the case that matters,
 * and the last assertion is that the two agree. */
static void test_socket_records_and_reads_the_same_edge(void) {
    env e;
    env_open(&e, true);
    char a[64], b[64];
    propose(&e, true, "proj", "the first thesis", a, sizeof a);
    propose(&e, true, "proj", "the second thesis", b, sizeof b);

    static const char WHY[] = "A depends on B, recorded over the socket.";
    {
        const char *args[] = {"--json", "decision", "link", "add", "proj", a, b, "--why", WHY};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_remote(&e, args, 9u, &out) == 0, "socket link add failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--json", "decision", "show", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(s, b) != NULL, "the socket lost the target");
        T_CHECK_MSG(strstr(s, WHY) != NULL, "the socket lost the rationale: %s", s);
        /* The document's own prose is intact. This is the A8.2 regression: a
         * link write must not touch the body. */
        T_CHECK_MSG(strstr(s, "the first thesis") != NULL,
                    "recording a relation damaged the document over the socket");
        atlas_buf_free(&out);
    }
    /* The audit read works over the socket too. */
    {
        const char *args[] = {"--json", "decision", "links", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), WHY) != NULL,
                    "the socket audit read lost the rationale");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"active\":true") != NULL,
                    "the socket audit read does not report the edge as live");
        atlas_buf_free(&out);
    }
    /* Withdrawal over the socket. */
    {
        const char *args[] = {"--json",  "decision", "link", "remove", "proj",
                              a,         b,          "--why", "restated"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_remote(&e, args, 9u, &out) == 0, "socket link remove failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--json", "decision", "links", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(s, "\"event\":\"REMOVED\"") != NULL, "no removal event over the socket");
        T_CHECK_MSG(strstr(s, "\"active\":false") != NULL, "the edge still reads as live");
        T_CHECK_MSG(strstr(s, WHY) != NULL,
                    "the original rationale was lost when the edge was withdrawn");
        atlas_buf_free(&out);
    }

    /* Parity: with the daemon stopped, the local path must render the same
     * account of the same document. */
    fx_daemon_stop(&e.d, false);
    e.daemon_running = false;
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "links",            "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(s, "\"event\":\"ADDED\"") != NULL, "local path lost the creation event");
        T_CHECK_MSG(strstr(s, "\"event\":\"REMOVED\"") != NULL, "local path lost the removal event");
        T_CHECK_MSG(strstr(s, WHY) != NULL, "local path lost the rationale");
        atlas_buf_free(&out);
    }
    fx_daemon_free(&e.d);
    fx_close(&e.fx);
}

/* --- 8. durability -------------------------------------------------------- */

/* A rationale that lives only in a process is not durable. This restarts the
 * daemon and then round-trips the record through backup and restore, because
 * "survives a restart" and "survives being copied and put back" are different
 * claims and the second is the one an operator relies on. */
static void test_rationale_survives_restart_and_backup_restore(void) {
    env e;
    env_open(&e, true);
    atlas_err err;
    atlas_err_init(&err);
    char a[64], b[64];
    propose(&e, true, "proj", "the first thesis", a, sizeof a);
    propose(&e, true, "proj", "the second thesis", b, sizeof b);
    static const char WHY[] = "durable across a restart and a restore";
    {
        const char *args[] = {"--json", "decision", "link", "add", "proj", a, b, "--why", WHY};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 9u, &out) == 0);
        atlas_buf_free(&out);
    }

    /* Restart. */
    fx_daemon_stop(&e.d, false);
    fx_daemon_free(&e.d);
    fx_daemon_init(&e.d);
    T_OK(fx_daemon_start(&e.fx, &e.d, &err), &err);
    T_OK(fx_daemon_wait_ready(&e.d, 15000, &err), &err);
    {
        const char *args[] = {"--json", "decision", "links", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), WHY) != NULL,
                    "the rationale did not survive a daemon restart");
        atlas_buf_free(&out);
    }
    fx_daemon_stop(&e.d, false);
    e.daemon_running = false;
    fx_daemon_free(&e.d);

    /* Backup, then restore over the live index, then read it back. */
    atlas_buf backup = ATLAS_BUF_INIT;
    T_OK(atlas_buf_appendf(&backup, &err, "%s/edge.db", fx_data_dir(&e.fx)), &err);
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "backup", "create",
                              atlas_buf_cstr(&backup)};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_local(&e, args, 5u, &out) == 0, "backup create failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "backup", "verify",
                              atlas_buf_cstr(&backup)};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_local(&e, args, 5u, &out) == 0, "backup verify failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "backup", "restore",
                              atlas_buf_cstr(&backup), "--yes"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_local(&e, args, 6u, &out) == 0, "backup restore failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "links",            "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, args, 7u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(s, WHY) != NULL, "the rationale did not survive backup and restore");
        T_CHECK_MSG(strstr(s, b) != NULL, "the edge lost its target through a restore");
        T_CHECK_MSG(strstr(s, "\"active\":true") != NULL,
                    "the restored edge is not reported as live");
        atlas_buf_free(&out);
    }
    atlas_buf_free(&backup);
    fx_close(&e.fx);
}

/* --- 10. a withdrawal without a reason is refused on both write paths ------
 *
 * The CLI's check is a better message; these are the guarantee. A socket caller
 * that skipped the CLI would otherwise withdraw a relation and write no removal
 * event, leaving the edge gone from the current revision with nothing saying
 * why — which is the one outcome removal must never produce. */
static void test_a_reasonless_withdrawal_is_refused_over_the_socket(void) {
    env e;
    env_open(&e, true);
    char a[64], b[64];
    propose(&e, true, "proj", "the first thesis", a, sizeof a);
    propose(&e, true, "proj", "the second thesis", b, sizeof b);
    {
        const char *args[] = {"--json",  "decision", "link", "add", "proj",
                              a,         b,          "--why", "A depends on B"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 9u, &out) == 0);
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--json", "decision", "link", "remove", "proj", a, b};
        atlas_buf out = ATLAS_BUF_INIT;
        T_CHECK_MSG(run_remote(&e, args, 7u, &out) != 0,
                    "a reasonless withdrawal was accepted over the socket");
        atlas_buf_free(&out);
    }
    /* And the relation is still there: a refused withdrawal withdraws nothing. */
    {
        const char *args[] = {"--json", "decision", "show", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"relates_to\"") != NULL,
                    "a refused withdrawal removed the relation anyway");
        atlas_buf_free(&out);
    }
    env_close(&e);
}

/* --- 11. recording what happened to a relation that is already gone -------
 *
 * `link note` writes one event about an edge and touches no link. It is the
 * only honest way to record the history of a relation that has already been
 * withdrawn — there is nothing left to add or remove — and it must not bring
 * the relation back. */
static void test_a_note_records_history_without_changing_links(void) {
    env e;
    env_open(&e, false);
    char a[64], b[64];
    propose(&e, false, "proj", "the first thesis", a, sizeof a);
    propose(&e, false, "proj", "the second thesis", b, sizeof b);

    static const char NOTE[] = "withdrawn during the repair pass: the relation was unsupported";
    const char *args[] = {"--data-dir", fx_data_dir(&e.fx), "--json",   "decision",
                          "link",       "note",             "proj",     a,
                          b,            "--why",            NOTE,       "--provenance",
                          "D3_REPAIR",  "--event",          "REMOVED"};
    atlas_buf out = ATLAS_BUF_INIT;
    T_REQUIRE_MSG(run_local(&e, args, 15u, &out) == 0, "link note failed: %s",
                  atlas_buf_cstr(&out));
    atlas_buf_free(&out);

    /* No relation was created. */
    {
        const char *show[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                              "decision",   "show",             "proj", a};
        atlas_buf doc = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, show, 7u, &doc) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&doc), "\"kind\":\"relates_to\"") == NULL,
                    "recording a note created the relation it was describing");
        T_CHECK_MSG(strstr(atlas_buf_cstr(&doc), "\"revision\":1") != NULL,
                    "recording a note wrote a revision");
        atlas_buf_free(&doc);
    }
    /* But the account is there, with the provenance it was given. */
    {
        const char *log[] = {"--data-dir", fx_data_dir(&e.fx), "--json",
                             "decision",   "links",            "proj", a};
        atlas_buf out2 = ATLAS_BUF_INIT;
        T_REQUIRE(run_local(&e, log, 7u, &out2) == 0);
        const char *s2 = atlas_buf_cstr(&out2);
        T_CHECK_MSG(strstr(s2, NOTE) != NULL, "the note is not readable: %s", s2);
        T_CHECK_MSG(strstr(s2, "\"provenance\":\"D3_REPAIR\"") != NULL,
                    "the provenance was not recorded: %s", s2);
        T_CHECK_MSG(strstr(s2, "\"event\":\"REMOVED\"") != NULL, "the event was not recorded");
        T_CHECK_MSG(strstr(s2, "\"active\":false") != NULL,
                    "a relation that does not exist reads as live");
        atlas_buf_free(&out2);
    }
    /* An invented provenance is refused against the closed vocabulary. */
    {
        const char *bad[] = {"--data-dir", fx_data_dir(&e.fx), "--json",   "decision",
                             "link",       "note",             "proj",     a,
                             b,            "--why",            "x",        "--provenance",
                             "INVENTED"};
        atlas_buf out3 = ATLAS_BUF_INIT;
        T_CHECK_MSG(run_local(&e, bad, 13u, &out3) != 0, "an invented provenance was accepted");
        atlas_buf_free(&out3);
    }
    env_close(&e);
}

/* --- 12. `link note` over the socket --------------------------------------
 *
 * The local path names the edge by argument; the socket names it in the
 * request, and the two used different keys — the daemon looked for
 * `edge_target` while the client sent `target`, which every other link method
 * uses. So `link note` worked locally and failed over the socket with a message
 * about a field the caller had supplied. Case 11 could not see it: with a
 * context present it never crosses the socket at all.
 *
 * **And this case does not reach it either, which is worth stating rather than
 * implying.** A fixture client always resolves a context, so its "remote" calls
 * are a *routed op* — the whole typed operation serialised under its own field
 * names — not the context-less client path the system deployment uses, which
 * builds a request by hand. `tests/test_decision_link_rpc.c` says the same
 * thing about the uid split for the same reason: a test binary cannot create a
 * second OS principal or write the root-owned policy that names the system
 * socket. What this case covers is that the note survives the socket at all.
 * The key mismatch itself was found against the real deployment, and the fix is
 * that the client now sends the far end under both names and the daemon accepts
 * either — so neither path can be the one that is right. */
static void test_a_note_crosses_the_socket(void) {
    env e;
    env_open(&e, true);
    char a[64], b[64];
    propose(&e, true, "proj", "the first thesis", a, sizeof a);
    propose(&e, true, "proj", "the second thesis", b, sizeof b);

    static const char NOTE[] = "recorded over the socket about a relation that is not live";
    {
        const char *args[] = {"--json",     "decision",  "link",  "note",   "proj",
                              a,            b,           "--why", NOTE,     "--provenance",
                              "D3_REPAIR",  "--event",   "REMOVED"};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE_MSG(run_remote(&e, args, 13u, &out) == 0, "link note over the socket failed: %s",
                      atlas_buf_cstr(&out));
        atlas_buf_free(&out);
    }
    {
        const char *args[] = {"--json", "decision", "links", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        const char *s = atlas_buf_cstr(&out);
        T_CHECK_MSG(strstr(s, NOTE) != NULL, "the note did not cross the socket: %s", s);
        T_CHECK_MSG(strstr(s, "\"provenance\":\"D3_REPAIR\"") != NULL,
                    "the provenance did not cross the socket");
        T_CHECK_MSG(strstr(s, "\"event\":\"REMOVED\"") != NULL,
                    "the event did not cross the socket");
        atlas_buf_free(&out);
    }
    /* And no relation was created by describing one. */
    {
        const char *args[] = {"--json", "decision", "show", "proj", a};
        atlas_buf out = ATLAS_BUF_INIT;
        T_REQUIRE(run_remote(&e, args, 5u, &out) == 0);
        T_CHECK_MSG(strstr(atlas_buf_cstr(&out), "\"kind\":\"relates_to\"") == NULL,
                    "a note created the relation it was describing");
        atlas_buf_free(&out);
    }
    env_close(&e);
}

static const atlas_test TESTS[] = {
    {"a relation between two different documents keeps its reason",
     test_cross_uid_rationale_round_trip},
    {"a document id is never prose and prose is never a document id",
     test_uid_and_prose_cannot_be_confused},
    {"withdrawing a relation keeps the account of it", test_link_remove_keeps_its_history},
    {"withdrawing a relation that was never drawn", test_removing_an_unknown_link},
    {"explaining an existing relation writes no revision",
     test_annotating_an_existing_edge_writes_no_revision},
    {"a reason is bounded, required to withdraw, and never reaches a terminal raw",
     test_bounds_and_hostile_text},
    {"a relation may not cross repositories", test_a_relation_may_not_cross_repositories},
    {"the socket records and reads the same edge as the local path",
     test_socket_records_and_reads_the_same_edge},
    {"a reason survives a restart and a restore", test_rationale_survives_restart_and_backup_restore},
    {"a withdrawal without a reason is refused over the socket",
     test_a_reasonless_withdrawal_is_refused_over_the_socket},
    {"a note records history without changing any link",
     test_a_note_records_history_without_changing_links},
    {"a note crosses the socket naming the edge the same way every link does",
     test_a_note_crosses_the_socket},
};

ATLAS_TEST_MAIN("decision_edge", TESTS)
